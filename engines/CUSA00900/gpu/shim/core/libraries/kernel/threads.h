// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"
#include "core/libraries/kernel/threads/pthread.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        Stop();
        std::lock_guard lock(lifecycle);
        stop_source = std::stop_source{};
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([this, func = std::move(func), done = finished,
                               stop = stop_source.get_token()] {
            current_thread = this;
            runtime_thread_attach_host("bb:hle");
            func(stop);
            done->store(true);
            current_thread = nullptr;
        });
    }
    // AvPlayer workers call Join on themselves as they finish, while the demuxer
    // or game thread may also join them. Self-join must leave ownership intact:
    // detaching here races the owner's join and can release buffers too early.
    void Join() {
        if (current_thread == this) return;
        std::lock_guard lock(lifecycle);
        JoinLocked();
    }
    bool Joinable() const {
        std::lock_guard lock(lifecycle);
        return thread.joinable();
    }
    void Stop() {
        // The demuxer may already be joining a decoder blocked on an empty
        // queue. Request cancellation before waiting for that join's mutex.
        // Run/restart is owner-serialized; stop_source stays stable while live.
        stop_source.request_stop();
        if (current_thread == this) return;
        std::lock_guard lock(lifecycle);
        if (thread.joinable()) {
            JoinLocked();
        }
    }

private:
    void JoinLocked() {
        if (!thread.joinable()) return;
        try {
            thread.join();
        } catch (const std::system_error& error) {
            // Windows (libc++ on Win32 threads): the join can fail although the thread is fine.
            // Wait for its function to return, so nothing it uses is freed under it, then let go.
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            while (finished && !finished->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (thread.joinable()) thread.detach();
        }
    }
    inline static thread_local Thread* current_thread = nullptr;
    mutable std::mutex lifecycle;
    std::jthread thread;
    std::stop_source stop_source;
    std::shared_ptr<std::atomic<bool>> finished;
};
} // namespace Libraries::Kernel
