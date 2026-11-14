// bbport: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
// bbport (Windows): no signals. The loader's exception handler resumes the faulting thread
// in bb_longjmp (src/compat_win.c: no unwinding through the faulting frames).
typedef unsigned long long sigjmp_buf[32];
extern "C" __attribute__((returns_twice)) int bb_setjmp(sigjmp_buf buffer);
#define sigsetjmp(buffer, save) bb_setjmp(buffer)
#endif

extern "C" std::uint64_t runtime_disabled_optimizations;
/// Recovery point for speculative guest memory reads on this thread (runtime_memory.c).
extern "C" __thread sigjmp_buf* runtime_fault_recover;

namespace BbToggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    UpdateImageFastPath = 1u << 30,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace BbToggle

namespace BbStats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Stall diagnostics (BB_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0};
/// Diagnostics are collected only with BB_FRAME_STATS=1. Not const (2026-10-31): this runs when
/// the DLL loads, before bb-env.txt is applied on Xbox, so ApplyBbEnvFile (vk_instance.cpp) sets
/// it again; before that fix every Timer measured 0 there.
inline bool enabled = [] {
    const char* env = std::getenv("BB_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Stall diagnostics (2026-10-31, area-change stutters): dense arena creation, the idle-arena
/// eviction pass (with its compare-before-evict check), and GPU-to-guest downloads (ns, count).
inline std::atomic<std::uint64_t> t_arena_create{0}, t_arena_evict{0}, arenas_evicted{0},
    t_readback{0}, readbacks{0};
/// gpu_idle_ns without the resets of the 5 s summaries (for the per-stall line).
inline std::atomic<std::uint64_t> gpu_idle_total_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0};
/// The same four waits without the resets of the 5 s summaries (2026-10-31, per-stall line).
inline std::atomic<std::uint64_t> sync_recording_total_ns{0}, host_copies_total_ns{0},
    tick_wait_total_ns{0}, copy_threads_total_ns{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        const std::uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start).count();
        total.fetch_add(ns, std::memory_order_relaxed);
        std::atomic<std::uint64_t>* const kept =
            &total == &sync_recording_ns     ? &sync_recording_total_ns
            : &total == &host_copies_wait_ns ? &host_copies_total_ns
            : &total == &tick_wait_ns        ? &tick_wait_total_ns
            : &total == &copy_threads_wait_ns ? &copy_threads_total_ns
                                              : nullptr;
        if (kept) {
            kept->fetch_add(ns, std::memory_order_relaxed);
        }
    }
};
/// BB_FAST_READBACK: readbacks copied on the readback queue (and their wall time), and those
/// that took the usual path because a writer was not complete or not known.
inline std::atomic<std::uint64_t> fast_readbacks{0}, fast_readback_ns{0},
    fast_readback_pending{0}, fast_readback_unknown{0};
/// BB_RECORDER_STATS=1 (Scheduler): the recording thread's time executing chunks; of it, its
/// submissions (count, waiting for the copy threads, vkQueueSubmit, the next command buffer);
/// SyncRecording() calls and the chunks queued (or being recorded) when each started.
inline std::atomic<std::uint64_t> recorder_exec_ns{0}, recorder_submits{0},
    recorder_copy_wait_ns{0}, recorder_submit_ns{0}, recorder_begin_ns{0}, recorder_syncs{0},
    recorder_sync_backlog{0}, recorder_host_copy_ns{0}, recorder_host_copies{0};
/// bbport: also with BB_FRAME_STATS=1 (the Xbox's frame-profiling.txt; bb-env.txt is applied
/// after this is initialized).
inline const bool recorder_stats = [] {
    const char* env = std::getenv("BB_RECORDER_STATS");
    return (env && env[0] == '1') || enabled;
}();
/// Copy pool (bbport_copy.cpp): WaitAsync calls, the caller's own batch run inline (items, ns),
/// pool tasks the waiting caller ran (ns), its idle waiting (ns), pool threads' task time (ns).
inline std::atomic<std::uint64_t> copy_waits{0}, copy_inline_items{0}, copy_inline_ns{0},
    copy_help_ns{0}, copy_idle_ns{0}, copy_pool_ns{0}, copy_merged{0};
/// Compute queue ReleaseMem fences, and those with no compute work since the previous one.
inline std::atomic<std::uint64_t> compute_fences{0}, compute_fences_idle{0};
/// GPU-modified memory downloads (BufferCache::ReadMemory) for guest writes and guest reads.
inline std::atomic<std::uint64_t> downloads_for_write{0}, downloads_for_read{0};
/// Readbacks that waited for a writer not submitted yet (still being recorded).
inline std::atomic<std::uint64_t> readback_unsubmitted{0};
/// Compute queue packets by PM4 opcode (BB_FRAME_STATS).
inline std::atomic<std::uint64_t> compute_opcodes[256]{}, compute_opcode_ns[256]{};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// TLOU debug: write protection faults taken by any thread (read faults: read_faults).
inline std::atomic<std::uint64_t> write_fault_count{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
/// BB_HOT_READBACK_FLUSH submissions right after a draw/dispatch wrote a hot readback block.
inline std::atomic<std::uint64_t> hot_readback_flushes{0};
/// Pipeline barriers recorded (Runtime::FlushBarriers) and buffer copy commands (CopyBuffer).
inline std::atomic<std::uint64_t> barrier_flushes{0}, buffer_copy_cmds{0};
/// BB_GFX_YIELD: graphics queue yields to pending compute submissions.
inline std::atomic<std::uint64_t> gfx_yields{0};
/// Compute queue round trip: submissions, our side (submission -> fence, ns, count) and the
/// guest's side (fence -> next submission of that queue, ns, count).
inline std::atomic<std::uint64_t> asc_submits{0}, asc_gpu_side_ns{0}, asc_gpu_sides{0},
    asc_guest_gap_ns{0}, asc_guest_gaps{0};
/// BB_FRAME_STATS: compute dispatch steps (ns): pipeline, resources, barriers, end rendering,
/// descriptors, record, reset, kick; then compute fence steps: copies, deferred signals,
/// image downloads, fence write + interrupt.
inline std::atomic<std::uint64_t> step_ns[13]{};
/// Compute fence stores: count and TSC cycles of the store alone; fences by data select.
inline std::atomic<std::uint64_t> fence_stores{0}, fence_store_cycles{0}, fence_data_sel[8]{},
    fence_store2_cycles{0}, fence_read_cycles{0};
/// Accumulates the time since the previous Mark() (or construction) into step_ns[index].
struct Laps {
    std::chrono::steady_clock::time_point last =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    void Mark(unsigned index) {
        if (!enabled) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        step_ns[index].fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count(),
            std::memory_order_relaxed);
        last = now;
    }
};
/// Compute dispatches run inside batched dispatches, and those batches (Rasterizer::DispatchBatch).
inline std::atomic<std::uint64_t> batched_dispatches{0}, dispatch_batches{0};
/// Dense buffer arenas: created, live bytes; guest memory unmaps (calls, bytes) for the memory report.
inline std::atomic<std::uint64_t> dense_arenas_created{0}, dense_arena_bytes{0}, unmap_calls{0},
    unmap_bytes{0};
/// BB_FRAME_STATS: compute dispatch resource binding (ns, calls): user data, ObtainBuffer,
/// image invalidation, barrier check, other buffer work, textures, DMA sync; written-buffer
/// ObtainBuffer; inside ObtainBuffer (compute queue): SynchronizeMemory, GPU-modified range add.
inline std::atomic<std::uint64_t> bind_ns[10]{}, bind_calls[10]{};
/// Compute dispatch buffers bound like the previous dispatch of the same shader (same slot,
/// address, size), all buffers, and dispatches whose buffers all were.
inline std::atomic<std::uint64_t> same_buffer_binds{0}, compute_buffer_binds{0},
    same_binding_dispatches{0};
/// Like Laps, into bind_ns/bind_calls, and only while `on` (a compute dispatch is bound).
struct BindLaps {
    bool on;
    std::chrono::steady_clock::time_point last =
        on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    void Mark(unsigned index) {
        if (!on) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        bind_ns[index].fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count(),
            std::memory_order_relaxed);
        bind_calls[index].fetch_add(1, std::memory_order_relaxed);
        last = now;
    }
};
} // namespace BbStats
