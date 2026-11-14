// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_copy.h"
#include "bbport_threads.h"

#include <algorithm>
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
        // From God of War: half of the hardware threads, up to 4 (a quarter left the Xbox with
        // 1-2, and its GPU command thread waited ~25% for them; 4 on PCs either way).
        unsigned count = std::clamp(BbThreads::Available() / 2, 1u, 4u);
        if (const char* env = std::getenv("BB_COPY_THREADS")) {
            count = static_cast<unsigned>(std::clamp(std::atoi(env), 0, 16));
        }
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

    void Async(std::function<void()> task) {
        {
            std::scoped_lock lk{mutex};
            const u64 epoch = epoch_base + epochs.size() - 1;
            ++epochs.back().pending;
            ++pending;
            async_tasks.push_back({std::move(task), epoch});
        }
        cv.notify_one();
    }

    /// Registers `callback` to run after every copy issued so far; returns false (callback
    /// untouched) when nothing is pending, so the caller runs it after releasing its own locks.
    bool DeferAfterCopies(std::function<void()>& callback) {
        std::unique_lock lk{mutex};
        if (epochs.size() > 1 || epochs.back().pending != 0 || draining) {
            epochs.back().callbacks.push_back(std::move(callback));
            epochs.emplace_back();
            return true;
        }
        return false;
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
            if (task.run) {
                RunAsync(task);
            } else {
                std::this_thread::yield();
            }
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
        Complete(task.epoch);
    }

    /// Retires a task and runs the callbacks of epochs with nothing left, oldest first. One
    /// thread drains at a time so the callbacks keep their order.
    void Complete(u64 epoch) {
        std::unique_lock lk{mutex};
        --epochs[epoch - epoch_base].pending;
        --pending;
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
                RunAsync(task);
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
// From Bloodborne (Xbox, 2026-10-28): one batch shared by all threads (was thread_local: a flush or
// fence on one thread did not cover copies another thread had queued but not started).
std::mutex batch_mutex;
Batch batch;
/// With batch_mutex held: takes the batch (null when empty).
std::shared_ptr<std::vector<Item>> TakeBatchLocked() {
    if (batch.items.empty()) {
        return nullptr;
    }
    auto items = std::make_shared<std::vector<Item>>(std::move(batch.items));
    batch.items = {};
    batch.items.reserve(256);
    batch.bytes = 0;
    return items;
}
/// With batch_mutex held: hands the batch to the pool (so a later flush/fence orders after it).
void StartBatchLocked() {
    if (auto items = TakeBatchLocked()) {
        GetPool().Async([items] {
            for (const auto& item : *items) {
                item.run(item);
            }
        });
    }
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
    std::scoped_lock lk{batch_mutex};
    StartBatchLocked();
}

void QueueCopy(const Item& item) {
    if (!Enabled()) {
        item.run(item);
        return;
    }
    // Smaller batches (32 items / 64 KiB, done before the next fence) measured slower.
    std::scoped_lock lk{batch_mutex};
    batch.items.push_back(item);
    batch.bytes += item.size;
    if (batch.bytes >= 512 * 1024 || batch.items.size() >= 256) {
        StartBatchLocked();
    }
}

void AfterCopies(std::function<void()> callback) {
    if (!Enabled()) {
        callback();
        return;
    }
    bool deferred;
    {
        std::scoped_lock lk{batch_mutex};
        StartBatchLocked();
        deferred = GetPool().DeferAfterCopies(callback);
    }
    if (!deferred) {
        callback();
    }
}

void WaitAsync() {
    // The pending batch runs here: handing it over only to wait for it costs a wakeup. Taken under
    // the lock, so every batch taken earlier by another thread is already in the pool.
    std::shared_ptr<std::vector<Item>> items;
    {
        std::scoped_lock lk{batch_mutex};
        items = TakeBatchLocked();
    }
    if (items) {
        for (const auto& item : *items) {
            item.run(item);
        }
    }
    GetPool().WaitAll();
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
