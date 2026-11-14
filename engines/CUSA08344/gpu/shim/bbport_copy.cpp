// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_copy.h"
#include "bbport_threads.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bbport_toggles.h"
#include "common/thread.h"

namespace BbCopy {
namespace {

thread_local bool in_copy_thread = false;

class Pool {
public:
    Pool() {
        // Copies are on the critical path (fences wait for them): normal priority, a quarter
        // of the hardware threads available to the process (Steam Deck 2).
        // bbport: half of the hardware threads, up to 4 (a quarter left the Xbox with 1-2, and its
        // GPU command thread waited ~25% for them; 4 on PCs either way).
        unsigned count = std::clamp(BbThreads::Available() / 2, 1u, 4u);
        if (const char* env = std::getenv("BB_COPY_THREADS")) {
            count = static_cast<unsigned>(std::clamp(std::atoi(env), 0, 16));
        }
        std::printf("Copy threads: %u (%u hardware threads available)\n", count,
                    BbThreads::Available());
        epochs.emplace_back();
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this, i] { Loop(i); });
        }
    }
    ~Pool() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    bool Enabled() const {
        return !threads.empty();
    }

    void Async(std::function<void()> task, bool deferrable = false) {
        {
            std::scoped_lock lk{mutex};
            const u64 epoch = epoch_base + epochs.size() - 1;
            ++epochs.back().pending;
            ++pending;
            pending_required += deferrable ? 0 : 1;
            async_tasks.push_back({std::move(task), epoch, deferrable});
        }
        cv.notify_one();
    }

    /// Waits for the tasks that are not deferrable, running them here when not started yet.
    void WaitRequired() {
        while (true) {
            Task task;
            {
                std::scoped_lock lk{mutex};
                if (pending_required == 0) {
                    return;
                }
                const auto it = std::ranges::find(async_tasks, false, &Task::deferrable);
                if (it != async_tasks.end()) {
                    task = std::move(*it);
                    async_tasks.erase(it);
                }
            }
            const auto start = std::chrono::steady_clock::now();
            if (task.run) {
                RunAsync(task);
            } else {
                std::this_thread::yield();
            }
            (task.run ? BbStats::copy_help_ns : BbStats::copy_idle_ns)
                .fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start).count(),
                           std::memory_order_relaxed);
        }
    }

    void AfterCopies(std::function<void()> callback) {
        {
            std::unique_lock lk{mutex};
            if (epochs.size() > 1 || epochs.back().pending != 0 || draining) {
                // Runs once every copy issued so far is done, in order with earlier ones.
                epochs.back().callbacks.push_back(std::move(callback));
                epochs.emplace_back();
                return;
            }
        }
        callback();
    }

    void WaitAll() {
        // The waiting thread helps instead of only spinning.
        while (true) {
            Task task;
            {
                std::scoped_lock lk{mutex};
                if (pending == 0 && epochs.size() == 1 && !draining) {
                    return;
                }
                if (!async_tasks.empty()) {
                    task = std::move(async_tasks.front());
                    async_tasks.pop_front();
                }
            }
            const auto start = std::chrono::steady_clock::now();
            if (task.run) {
                RunAsync(task);
            } else {
                std::this_thread::yield();
            }
            (task.run ? BbStats::copy_help_ns : BbStats::copy_idle_ns)
                .fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start).count(),
                           std::memory_order_relaxed);
        }
    }

    void Run(std::size_t count, const std::function<void(std::size_t)>& task) {
        auto job = std::make_shared<Job>();
        job->task = &task;
        job->count = count;
        {
            std::scoped_lock lk{mutex};
            current = job;
            ++generation;
        }
        cv.notify_all();
        Work(*job);
        // Workers only call the task for indices below count, all done before this returns.
        while (job->done.load(std::memory_order_acquire) < count) {
            std::this_thread::yield();
        }
        std::scoped_lock lk{mutex};
        if (current == job) {
            current.reset();
        }
    }

private:
    using u64 = unsigned long long;
    struct Task {
        std::function<void()> run;
        u64 epoch{};
        bool deferrable = false;
    };
    /// Copies issued between two AfterCopies() calls, and the callbacks waiting for them.
    struct Epoch {
        std::size_t pending = 0;
        std::vector<std::function<void()>> callbacks;
    };
    struct Job {
        const std::function<void(std::size_t)>* task{};
        std::size_t count{};
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
    };

    void RunAsync(Task& task) {
        const bool was = in_copy_thread;
        in_copy_thread = true;
        task.run();
        in_copy_thread = was;
        Complete(task.epoch, task.deferrable);
    }

    /// Retires a task and runs the callbacks of epochs with nothing left, oldest first. One
    /// thread drains at a time so the callbacks keep their order.
    void Complete(u64 epoch, bool deferrable) {
        std::unique_lock lk{mutex};
        --epochs[epoch - epoch_base].pending;
        --pending;
        pending_required -= deferrable ? 0 : 1;
        if (draining) {
            return;
        }
        draining = true;
        while (true) {
            std::vector<std::function<void()>> ready;
            while (epochs.size() > 1 && epochs.front().pending == 0) {
                for (auto& callback : epochs.front().callbacks) {
                    ready.push_back(std::move(callback));
                }
                epochs.pop_front();
                ++epoch_base;
            }
            if (ready.empty()) {
                break;
            }
            lk.unlock();
            for (auto& callback : ready) {
                callback();
            }
            lk.lock();
        }
        draining = false;
    }

    static void Work(Job& job) {
        const bool was = in_copy_thread;
        in_copy_thread = true;
        for (std::size_t i; (i = job.next.fetch_add(1, std::memory_order_relaxed)) < job.count;) {
            (*job.task)(i);
            job.done.fetch_add(1, std::memory_order_release);
        }
        in_copy_thread = was;
    }

    void Loop(unsigned index) {
        Common::SetCurrentThreadName(("bb:Copy" + std::to_string(index)).c_str());
        u64 seen = 0;
        std::unique_lock lk{mutex};
        while (true) {
            cv.wait(lk, [&] {
                return stop || !async_tasks.empty() || (generation != seen && current);
            });
            if (stop) {
                return;
            }
            if (!async_tasks.empty()) {
                Task task = std::move(async_tasks.front());
                async_tasks.pop_front();
                lk.unlock();
                const auto start = std::chrono::steady_clock::now();
                RunAsync(task);
                BbStats::copy_pool_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::chrono::steady_clock::now() - start).count(),
                                                std::memory_order_relaxed);
                lk.lock();
                continue;
            }
            seen = generation;
            const auto job = current;
            lk.unlock();
            Work(*job);
            lk.lock();
        }
    }

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<Job> current;
    u64 generation = 0;
    bool stop = false;
    std::deque<Task> async_tasks;
    std::deque<Epoch> epochs; ///< back() is open for new copies
    u64 epoch_base = 0;       ///< epoch number of epochs.front()
    std::size_t pending = 0;
    std::size_t pending_required = 0; ///< pending tasks that are not deferrable
    bool draining = false;
};

Pool& GetPool() {
    static Pool pool;
    return pool;
}

struct Batch {
    std::vector<Item> items;
    unsigned long long bytes = 0;
};
thread_local Batch batch;
thread_local Batch deferred_batch; ///< deferrable copies (QueueCopy)

void Flush(Batch& b, bool deferrable) {
    if (b.items.empty()) {
        return;
    }
    auto items = std::make_shared<std::vector<Item>>(std::move(b.items));
    b.items = {};
    b.items.reserve(256);
    b.bytes = 0;
    GetPool().Async(
        [items] {
            for (const auto& item : *items) {
                item.run(item);
            }
        },
        deferrable);
}

/// Runs the caller's batch here: handing it over only to wait for it costs a wakeup.
void RunInline(Batch& b) {
    if (b.items.empty()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    BbStats::copy_inline_items.fetch_add(b.items.size(), std::memory_order_relaxed);
    for (const auto& item : b.items) {
        item.run(item);
    }
    b.items.clear();
    b.bytes = 0;
    BbStats::copy_inline_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - start).count(),
                                      std::memory_order_relaxed);
}

} // namespace

bool Enabled() {
    return GetPool().Enabled() && !BbToggle::Disabled(BbToggle::ParallelCopies);
}

void Async(std::function<void()> task) {
    if (!Enabled()) {
        task();
        return;
    }
    GetPool().Async(std::move(task));
}

void FlushBatch() {
    if (!Enabled()) {
        return;
    }
    Flush(batch, false);
    Flush(deferred_batch, true);
}

bool DeferGraphicsCopies() {
    static const bool defer = [] {
        const char* env = std::getenv("BB_DEFER_GFX_COPIES");
        return !(env && env[0] == '0');
    }();
    return defer;
}

void QueueCopy(const Item& item, bool deferrable) {
    if (!Enabled()) {
        item.run(item);
        return;
    }
    deferrable = deferrable && !in_compute_queue && DeferGraphicsCopies();
    auto& batch = deferrable ? deferred_batch : BbCopy::batch;
    // Smaller batches (32 items / 64 KiB, done before the next fence) measured slower.
    if (!batch.items.empty() && TryMerge(batch.items.back(), item)) {
        batch.bytes += item.size;
        return;
    }
    batch.items.push_back(item);
    batch.bytes += item.size;
    // bbport: BB_COPY_BATCH=<items> hands smaller batches to the copy threads. On the Xbox the
    // GPU command thread ran its whole batch itself at ~23k copy waits/s (25% of its time).
    static const size_t batch_items = [] {
        const char* env = std::getenv("BB_COPY_BATCH");
        const int value = env ? std::atoi(env) : 0;
        return value > 0 ? static_cast<size_t>(std::min(value, 256)) : size_t{256};
    }();
    if (batch.bytes >= 512 * 1024 || batch.items.size() >= batch_items) {
        Flush(batch, deferrable);
    }
}

bool TryMerge(Item& last, const Item& item) {
    static const bool merge = [] {
        const char* env = std::getenv("BB_COPY_MERGE");
        return env && env[0] == '1';
    }();
    if (!merge || last.run != item.run || last.context != item.context ||
        last.extra != item.extra || last.source + last.size != item.source ||
        last.destination + last.size != item.destination) {
        return false;
    }
    last.size += item.size;
    BbStats::copy_merged.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void AfterCopies(std::function<void()> callback) {
    FlushBatch();
    GetPool().AfterCopies(std::move(callback));
}

void WaitAsync() {
    BbStats::copy_waits.fetch_add(1, std::memory_order_relaxed);
    RunInline(batch);
    RunInline(deferred_batch);
    GetPool().WaitAll();
}

void WaitRequired() {
    BbStats::copy_waits.fetch_add(1, std::memory_order_relaxed);
    RunInline(batch);
    if (Enabled()) {
        GetPool().WaitRequired();
    }
}

void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task) {
    if (count <= 1 || in_copy_thread || !Enabled()) {
        for (std::size_t i = 0; i < count; ++i) {
            task(i);
        }
        return;
    }
    GetPool().Run(count, task);
}

} // namespace BbCopy
