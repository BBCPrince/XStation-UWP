// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (on unless BB_ASYNC_IRQ=0): TLOU signals ~36k compute queue fences per second, each with an
// interrupt whose handlers trigger the guest's event queue and wake a guest thread: ~6 us of the
// GPU command thread per fence on the Xbox (21% of it). The fence value is still written on the
// command thread; the interrupt follows from a helper thread, in order. The helper spins for
// BB_ASYNC_IRQ_SPIN_US (default 0) after its last interrupt before sleeping (Xbox courtyard,
// frames measured without screenshots: off 28.95, spin 0 29.59, spin 200 29.25 FPS: spinning
// takes a core from the game).
#include "bbport_irq.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "common/thread.h"
#include "core/platform.h"

namespace BbIrq {
namespace {

class Queue {
public:
    Queue() {
        const char* env = std::getenv("BB_ASYNC_IRQ_SPIN_US");
        spin = std::chrono::microseconds(env ? std::atoi(env) : 0);
        std::printf("Async compute interrupts: on (spin %lld us)\n",
                    static_cast<long long>(spin.count()));
        thread = std::thread([this] { Loop(); });
        thread.detach();
    }

    void Post(std::uint32_t irq) {
        const std::uint64_t head = tail_reserved;
        // Full ring: deliver in order by waiting for room (does not happen at 1024 entries).
        while (head - consumed.load(std::memory_order_acquire) >= ring.size()) {
            std::this_thread::yield();
        }
        ring[head % ring.size()] = irq;
        tail_reserved = head + 1;
        produced.store(head + 1, std::memory_order_release);
        if (sleeping.load(std::memory_order_seq_cst)) {
            produced.notify_one();
        }
    }

private:
    void Loop() {
        Common::SetCurrentThreadName("bb:AsyncIrq");
        std::uint64_t next = 0;
        while (true) {
            const auto spin_until = std::chrono::steady_clock::now() + spin;
            std::uint64_t available;
            for (std::uint32_t spins = 0;
                 (available = produced.load(std::memory_order_acquire)) == next; ++spins) {
                if ((spins & 255) == 255 && std::chrono::steady_clock::now() >= spin_until) {
                    sleeping.store(true, std::memory_order_seq_cst);
                    if (produced.load(std::memory_order_seq_cst) == next) {
                        produced.wait(next, std::memory_order_acquire);
                    }
                    sleeping.store(false, std::memory_order_relaxed);
                }
                _mm_pause();
            }
            for (; next < available; ++next) {
                const auto irq = static_cast<Platform::InterruptId>(ring[next % ring.size()]);
                consumed.store(next + 1, std::memory_order_release);
                Platform::IrqC::Instance()->Signal(irq);
            }
        }
    }

    std::array<std::uint32_t, 1024> ring{};
    std::uint64_t tail_reserved = 0;           ///< producer only
    std::atomic<std::uint64_t> produced{0};    ///< entries published
    std::atomic<std::uint64_t> consumed{0};    ///< entries taken by the helper
    std::atomic<bool> sleeping{false};
    std::chrono::microseconds spin{0};
    std::thread thread;
};

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_ASYNC_IRQ");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

void Post(std::uint32_t irq) {
    static Queue queue;
    queue.Post(irq);
}

} // namespace BbIrq
