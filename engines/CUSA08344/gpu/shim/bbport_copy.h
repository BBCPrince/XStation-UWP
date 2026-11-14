// bbport: parallel copies of guest memory. Streaming a new area uploads 50-400 MB of textures
// and buffers per frame; copied on one thread that is tens of milliseconds (stutter).
#pragma once
#include <cstddef>
#include <functional>

namespace BbCopy {

/// Runs `task(i)` for every i in [0, count) on the copy threads and the caller; returns when
/// all are done. Runs inline when count is 1, the pool is disabled, or when called from a copy
/// thread (no nesting).
void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task);

/// True when ParallelFor would split work (more than one thread available).
bool Enabled();

/// Starts `task` on a copy thread now (inline when the pool is disabled). Tasks run in any
/// order and concurrently.
void Async(std::function<void()> task);

/// A small copy for QueueCopy(): `run(item)` does the work on a copy thread.
struct Item {
    void (*run)(const Item&);
    void* context;
    unsigned long long source, destination, size, extra;
};

/// Queues a small copy in the calling thread's batch; the batch is started with Async() once it
/// holds about 512 KiB or 256 items, or by FlushBatch()/WaitAsync(). One wakeup per batch
/// instead of one per copy (thousands of constant buffer copies per frame).
/// bbport: `deferrable` copies (graphics stream buffer copies: their destination is used by
/// one draw only) go to a second batch that WaitRequired() leaves to the copy threads.
void QueueCopy(const Item& item, bool deferrable = false);

/// bbport: compute queue processing on this thread (copies issued there are never deferrable).
inline thread_local bool in_compute_queue = false;
struct ComputeQueueScope {
    bool was = in_compute_queue;
    ComputeQueueScope() {
        in_compute_queue = true;
    }
    ~ComputeQueueScope() {
        in_compute_queue = was;
    }
};

/// bbport (BB_DEFER_GFX_COPIES, on unless 0): whether graphics stream copies may be deferred.
bool DeferGraphicsCopies();

/// Starts the calling thread's required batch and waits until every task passed to Async()
/// so far that holds required copies has finished; deferrable copies keep going on the copy
/// threads (a compute queue fence orders that queue's own work, not the graphics queue's).
void WaitRequired();

/// bbport (BB_COPY_MERGE=1): extends `last` by `item` when it continues it (same routine,
/// context and buffer; source and destination right after it). Each copy pays a fixed cost
/// (Xbox: lock + region lookup, ~1.6 us).
bool TryMerge(Item& last, const Item& item);

/// Starts the calling thread's batch.
void FlushBatch();

/// Starts the calling thread's batch, then waits until every task passed to Async() so far
/// has finished.
void WaitAsync();

/// Starts the calling thread's batch and runs `callback` once every task passed to Async() so
/// far has finished (at once when none is pending), on whichever thread finishes last.
/// Callbacks run in the order they were registered.
void AfterCopies(std::function<void()> callback);

} // namespace BbCopy
