// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_timeline.h"
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#ifndef _WIN32
#include <dlfcn.h>
#else
#include <windows.h>
#endif
#include <functional>

#include "bbport_copy.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_threads.h"
#include "bbport_sampler.h"

#ifdef _WIN32
extern "C" void register_gpu_thread194(const char* name); // tlou_gpu_thread_trace194.cpp
// tlou194_uwp_memory.c: the pager/renderer event ring (kind 10: a GPU submit, the timeline it signals and its tick)
extern "C" void runtime_trace_event(int kind, uint64_t a, uint64_t b, const void* code);
namespace {
void TraceSubmit(const Vulkan::SubmitInfo& info) {
    runtime_trace_event(10, info.num_signal_semas ? reinterpret_cast<uint64_t>(static_cast<VkSemaphore>(info.signal_semas[0])) : 0,
                        info.num_signal_semas ? info.signal_ticks[0] : 0, nullptr);
}
} // namespace
#endif

namespace Vulkan {
void BbSubmitRingDump(u64 gpu_tick, u64 waited_tick); // vk_rasterizer.cpp
}
namespace Vulkan {

std::mutex Scheduler::submit_mutex;

void Scheduler::SetExecutionQueue(vk::Queue queue) {
    if (execution_queue == queue) {
        return;
    }
    // The timeline and command-pool retirement order span this scheduler's submissions.
    // Drain the old queue before a provider switch can signal a later value on another queue.
    Finish();
    execution_queue = queue;
}

static constexpr std::array HostDescriptorPoolSizes{
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 4096},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 4096},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 4096},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 4096},
    vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 4096},
};

Scheduler::Scheduler(const Instance& instance, bool threaded_recording, vk::Queue queue)
    : instance{instance}, execution_queue{queue ? queue : instance.GetGraphicsQueue()},
      work_semaphore{instance}, command_pool{instance, &work_semaphore},
      host_descriptor_heap{instance,&work_semaphore,HostDescriptorPoolSizes} {
    // bbport: BB_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("BB_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        recording_threaded = true;
        const char* async_env = std::getenv("BB_ASYNC_SUBMIT");
        async_submit = !(async_env && async_env[0] == '0');
        const char* copies_env = std::getenv("BB_ORDERED_COPIES");
        if (!(copies_env && copies_env[0] == '0')) {
            ordered_copies = true;
            copy_batch.reserve(64);
            copy_thread = std::jthread(std::bind_front(&Scheduler::OrderedCopyThread, this));
            std::printf("Ordered copies: on\n");
        }
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
    if (copy_thread.joinable()) {
        WaitHostCopies();
        copy_thread.request_stop();
        copy_cv.notify_all();
        copy_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BindHostDescriptors(vk::CommandBuffer cmdbuf, vk::PipelineBindPoint bind_point,
                                    vk::PipelineLayout layout, vk::DescriptorSetLayout set_layout,
                                    vk::ArrayProxy<const vk::WriteDescriptorSet> writes) {
    std::scoped_lock lock{host_descriptor_mutex};
    // Host passes can destroy and recreate layouts while this heap survives.
    // Drivers may recycle a raw layout handle for an incompatible layout; a
    // cached batch keyed by that handle would then return sets of the old type.
    const auto set = host_descriptor_heap.Commit(set_layout, false);
    std::vector<vk::WriteDescriptorSet> updates(writes.begin(), writes.end());
    for (auto& write : updates) {
        write.dstSet = set;
    }
    instance.GetDevice().updateDescriptorSets(updates, {});
    cmdbuf.bindDescriptorSets(bind_point, layout, 0, set, {});
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            render_state.area_width
                ? vk::Rect2D{.offset = {render_state.area_x, render_state.area_y},
                             .extent = {render_state.area_width, render_state.area_height}}
                : vk::Rect2D{.offset = {0, 0},
                             .extent = {render_state.width, render_state.height}},
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (!recording_threaded) {
        current_cmdbuf.beginRendering(rendering_info);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    Record([info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        cmdbuf.beginRendering(info);
    });
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
#ifdef _WIN32
        // The containing module's base, for an RVA to symbolize offline.
        struct { const char* dli_fname; void* dli_fbase; } info{"module", nullptr};
        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(top[i].second), &module)) {
            info.dli_fbase = module;
        }
#else
        Dl_info info{};
        dladdr(top[i].second, &info);
#endif
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        signal();
        return;
    }
    BbCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    HandOffCopies();
    Record([this, signal = std::move(signal), done = deferred_signals_done,
            copies = ordered_copies ? host_copies_issued : 0](vk::CommandBuffer) mutable {
        WaitCopiesUpTo(copies);
        BbCopy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
            signal();
            done->fetch_add(1, std::memory_order_release);
        });
    });
    KickRecording(true);
}

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        BbToggle::Disabled(BbToggle::OrderedGuestWrites)) {
        return;
    }
    BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        // Helps the copy threads the signals wait for.
        BbCopy::WaitAsync();
        std::this_thread::yield();
    }
}

void Scheduler::QueueHostCopy(const BbCopy::Item& item) {
    if (!ordered_copies) {
        RecordHostCopy([item] { item.run(item); });
        return;
    }
    ++host_copies_issued;
    if (!copy_batch.empty() && BbCopy::TryMerge(copy_batch.back(), item)) {
        return;
    }
    copy_batch.push_back(item);
    if (copy_batch.size() >= 64) {
        HandOffCopies();
    }
}

void Scheduler::HandOffCopies() {
    if (copy_batch.empty()) {
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{copy_mutex};
        std::vector<BbCopy::Item> next;
        if (!free_copy_batches.empty()) {
            next = std::move(free_copy_batches.back());
            free_copy_batches.pop_back();
        }
        copy_queue.emplace_back(std::exchange(copy_batch, std::move(next)), host_copies_issued);
        queued_copy_batches.store(copy_queue.size(), std::memory_order_release);
        wake = copy_thread_sleeping;
    }
    if (wake) {
        copy_cv.notify_one();
    }
}

void Scheduler::WaitCopiesUpTo(u64 seq) const {
    while (host_copies_done.load(std::memory_order_acquire) < seq) {
        std::this_thread::yield();
    }
}

void Scheduler::OrderedCopyThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:OrderedCopies");
    BbSampler::Register("copies"); // BB_IP_SAMPLE_SPIKE_MS (Detroit 5 s hitches, 2026-11-10)
    while (true) {
        // As the recording thread: spin briefly, the next batch usually follows soon.
        static const auto spin_time = std::chrono::microseconds(
            BbThreads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_copy_batches.load(std::memory_order_acquire) == 0; ++spins) {
            __builtin_ia32_pause();
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::vector<BbCopy::Item> batch;
        u64 last = 0;
        {
            std::unique_lock lk{copy_mutex};
            copy_thread_sleeping = true;
            copy_cv.wait(lk, stoken, [this] { return !copy_queue.empty(); });
            copy_thread_sleeping = false;
            if (copy_queue.empty()) {
                return; // stop requested
            }
            batch = std::move(copy_queue.front().first);
            last = copy_queue.front().second;
            copy_queue.pop_front();
            queued_copy_batches.store(copy_queue.size(), std::memory_order_release);
        }
        if (BbStats::recorder_stats) {
            const auto start = std::chrono::steady_clock::now();
            for (const auto& item : batch) {
                item.run(item);
            }
            BbStats::recorder_host_copies.fetch_add(batch.size(), std::memory_order_relaxed);
            BbStats::recorder_host_copy_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count(),
                std::memory_order_relaxed);
        } else {
            for (const auto& item : batch) {
                item.run(item);
            }
        }
        host_copies_done.store(last, std::memory_order_release);
        batch.clear();
        std::scoped_lock lk{copy_mutex};
        free_copy_batches.push_back(std::move(batch));
    }
}

void Scheduler::WaitHostCopies(bool required_only) {
    HandOffCopies();
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
        BbStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        const u64 tl_begin = BbTimeline::NowNs();
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
        if (const u64 us = (BbTimeline::NowNs() - tl_begin) / 1000; us >= 100) {
            BbTimeline::Note('i', us, 0);
        }
    }
    BbStats::WaitTimer timer{BbStats::copy_threads_wait_ns};
    if (required_only) {
        BbCopy::WaitRequired();
    } else {
        BbCopy::WaitAsync();
    }
}

void Scheduler::KickRecording(bool force) {
    if (!recording_threaded) {
        return;
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    // The copies issued with these commands start too (BB_ORDERED_COPIES).
    HandOffCopies();
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    // bbport: the replacement is taken before the current chunk is handed over, so record_chunk
    // is never null (see RetireChunk). AcquireChunk() takes recorder_mutex itself.
    std::unique_ptr<RecordChunk> next = record_chunk->Empty() ? nullptr : AcquireChunk();
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back(std::move(chunk));
        }
        if (next) {
            recorder_queue.push_back(std::exchange(record_chunk, std::move(next)));
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
}

void Scheduler::SyncRecording() {
    if (!recording_threaded) {
        return;
    }
    KickRecording(true);
    BbStats::WaitTimer timer{BbStats::sync_recording_ns};
    const u64 tl_begin = BbTimeline::NowNs();
    struct TlSync {
        u64 begin;
        ~TlSync() {
            const u64 us = (BbTimeline::NowNs() - begin) / 1000;
            if (us >= 100) {
                BbTimeline::Note('d', us, 0);
            }
        }
    } tl_sync{tl_begin};
    std::unique_lock lk{recorder_mutex};
    if (BbStats::recorder_stats) {
        BbStats::recorder_syncs.fetch_add(1, std::memory_order_relaxed);
        BbStats::recorder_sync_backlog.fetch_add(recorder_queue.size() + (recorder_busy ? 1 : 0),
                                                 std::memory_order_relaxed);
    }
    recorder_idle_cv.wait(lk, [this] { return recorder_queue.empty() && !recorder_busy; });
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:VkRecorder");
    BbThreads::RaiseCriticalPriority();
    BbSampler::Register("recorder"); // BB_IP_SAMPLE_THREAD=recorder
#ifdef _WIN32
    // Detroit (2026-11-11), BB_DT_AFFINITY=1: this thread alone on CPU 2 (runtime_thread.c).
    if (const char* aff = std::getenv("BB_DT_AFFINITY"); aff && aff[0] == '1') {
        const DWORD_PTR was = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << 2);
        std::printf("Detroit: Vulkan recording thread on CPU 2 (was %#llx)%c", (unsigned long long)was, 10);
    }
#endif
#ifdef _WIN32
    register_gpu_thread194("Vulkan recording thread"); // thread CPU stats (BB_FRAME_STATS)
#endif
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        static const auto spin_time = std::chrono::microseconds(
            BbThreads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            __builtin_ia32_pause();
            // The clock is read every 256 pauses, not per iteration.
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            const u64 tl_begin = BbTimeline::NowNs();
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            // Timeline: the recorder was idle (a = us) before this chunk.
            if (const u64 us = (BbTimeline::NowNs() - tl_begin) / 1000; us >= 200) {
                BbTimeline::Note('b', us, 0);
            }
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
        // current_cmdbuf only changes after SyncRecording(), which waits for this thread, or
        // here in a FlushDeferred() submission, the last command of its chunk.
        if (BbStats::recorder_stats) {
            const auto start = std::chrono::steady_clock::now();
            chunk->Execute(current_cmdbuf);
            BbStats::recorder_exec_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count(),
                std::memory_order_relaxed);
        } else {
            chunk->Execute(current_cmdbuf);
        }
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::FlushDeferred() {
#if TRACY_GPU_ENABLED
    Flush();
    return;
#endif
    if (!async_submit || !IsRecordingDeferred()) {
        Flush();
        return;
    }
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        Flush();
        return;
    }
    static const bool announced = [] {
        std::printf("Async submit: on\n");
        return true;
    }();
    (void)announced;
    SubmitInfo info{};
    vk::Queue queue;
    {
        // As SubmitExecution(): on_submit may bind sparse memory on the graphics queue.
        std::scoped_lock lk{submit_mutex};
        const u64 signal_value = work_semaphore.NextTick();
        if (on_submit) {
            on_submit(info);
        }
        info.AddSignal(work_semaphore.Handle(), signal_value);
        queue = execution_queue;
    }
    EndRendering();
    std::array<vk::PipelineStageFlags, 4> wait_stage_masks;
    wait_stage_masks.fill(vk::PipelineStageFlagBits::eAllCommands);
    wait_stage_masks[1] = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    if (pending_external_wait) {
        ASSERT(info.num_wait_semas < info.wait_semas.size());
        wait_stage_masks[info.num_wait_semas] = vk::PipelineStageFlagBits::eAllCommands;
        info.AddWait(pending_external_wait, pending_external_value);
        pending_external_wait = nullptr;
    }
    // Guest memory copies this thread batched for the submission start now; the recording
    // thread waits for them (and every earlier one) before submitting.
    BbCopy::FlushBatch();
    HandOffCopies();
    deferred_submits_issued.fetch_add(1, std::memory_order_relaxed);
    Record([this, info, queue, wait_stage_masks,
            copies = ordered_copies ? host_copies_issued : 0](vk::CommandBuffer cmdbuf) mutable {
        WaitCopiesUpTo(copies);
        SubmitOnRecorder(cmdbuf, info, queue, wait_stage_masks);
    });
    // The submission ends its chunk (the next chunk records into the new command buffer), and
    // waits for it must not depend on a later kick.
    KickRecording(true);
    // Later commands start from no dynamic state, as in the new command buffer.
    dynamic_state.Invalidate();
    PopPendingOperations();
}

void Scheduler::SubmitOnRecorder(vk::CommandBuffer cmdbuf, SubmitInfo& info, vk::Queue queue,
                                 const std::array<vk::PipelineStageFlags, 4>& wait_stage_masks) {
    // The host copies recorded before this point ran on this thread already; the copy
    // threads' copies issued so far must be done before the GPU reads their staging memory.
    using Clock = std::chrono::steady_clock;
    const auto ns_since = [](Clock::time_point t) {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
    };
    auto stage_start = Clock::now();
    std::atomic<bool> copies_done{false};
    BbCopy::AfterCopies([&copies_done] { copies_done.store(true, std::memory_order_release); });
    while (!copies_done.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    if (BbStats::recorder_stats) {
        BbStats::recorder_submits.fetch_add(1, std::memory_order_relaxed);
        BbStats::recorder_copy_wait_ns.fetch_add(ns_since(stage_start),
                                                 std::memory_order_relaxed);
        stage_start = Clock::now();
    }
    // Spider-Man port (2026-11-05): Dozen fails the command buffer when a draw-time pipeline
    // variant cannot be created (E_OUTOFMEMORY on the PC); with BB_SUBMIT_RING=1 the recorded
    // draws are printed first (the failing buffer's are those with the highest tick).
    if (const vk::Result end_result = cmdbuf.end(); end_result != vk::Result::eSuccess) {
        BbSubmitRingDump(0, 0);
        Check(end_result);
    }

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };
    vk::Result submit_result;
    {
        std::scoped_lock lk{submit_mutex};
        TraceSubmit(info);
        submit_result = queue.submit(submit_info, info.fence);
    }
    if (submit_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceLost("submit");
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    work_semaphore.Refresh();
    if (BbStats::recorder_stats) {
        BbStats::recorder_submit_ns.fetch_add(ns_since(stage_start), std::memory_order_relaxed);
        stage_start = Clock::now();
    }

    // Only this thread uses the command pool until the next SyncRecording().
    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    if (BbStats::recorder_stats) {
        BbStats::recorder_begin_ns.fetch_add(ns_since(stage_start), std::memory_order_relaxed);
    }
    deferred_submits_done.fetch_add(1, std::memory_order_release);
}

namespace {
thread_local bool in_finish = false; ///< Wait() inside Finish(): counted for Finish's caller

/// bbport (BB_FRAME_STATS): wall time of Scheduler::Wait/Finish by caller (module RVA), the
/// top ones printed every 2048 waits.
void NoteWait(void* caller, u64 ns) {
    static std::mutex mutex;
    static std::unordered_map<void*, std::pair<u64, u64>> callers;
    static u64 calls = 0;
    std::scoped_lock lk{mutex};
    auto& [count, total] = callers[caller];
    ++count;
    total += ns;
    if (++calls % 2048) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, entry] : callers) {
        top.emplace_back(entry.second, address);
    }
    std::ranges::sort(top, std::greater{});
    std::printf("Scheduler waits (last 2048):");
    for (size_t i = 0; i < std::min<size_t>(top.size(), 6); ++i) {
        uintptr_t base = 0;
#ifdef _WIN32
        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(top[i].second), &module)) {
            base = reinterpret_cast<uintptr_t>(module);
        }
#endif
        std::printf(" +0x%lx %llux %.2f ms;",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) - base),
                    static_cast<unsigned long long>(callers[top[i].second].first),
                    top[i].first / 1e6);
    }
    std::printf("\n");
    callers.clear();
}
} // Anonymous namespace

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const auto start = BbStats::enabled ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    in_finish = true;
    Wait(presubmit_tick);
    in_finish = false;
    if (BbStats::enabled) {
        NoteWait(__builtin_return_address(0),
                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - start).count());
    }
}

void Scheduler::WaitSubmitted() {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1);
    }
}

void Scheduler::Wait(u64 tick) {
    const auto start = BbStats::enabled && !in_finish ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    {
        BbStats::WaitTimer timer{BbStats::tick_wait_ns};
        const u64 tl_begin = BbTimeline::NowNs();
        work_semaphore.Wait(tick);
        if (const u64 us = (BbTimeline::NowNs() - tl_begin) / 1000; us >= 100) {
            BbTimeline::Note('u', us, tick);
        }
    }
    if (BbStats::enabled && !in_finish) {
        NoteWait(__builtin_return_address(0),
                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - start).count());
    }
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // bbport: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !BbToggle::Disabled(BbToggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    // bbport: FlushDeferred() submissions still queued take submit_mutex on the recording
    // thread; they go out first (lower timeline values), outside the lock.
    if (deferred_submits_done.load(std::memory_order_acquire) !=
        deferred_submits_issued.load(std::memory_order_relaxed)) {
        SyncRecording();
    }
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies();
    if (const vk::Result end_result = current_cmdbuf.end(); end_result != vk::Result::eSuccess) {
        BbSubmitRingDump(0, 0); // Spider-Man port: see SubmitOnRecorder
        Check(end_result);
    }

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    // 1.8.2's masks (the second wait is the presenter's image), one for every wait the
    // submission can carry, and the external provider's wait after them.
    std::array<vk::PipelineStageFlags, std::tuple_size_v<decltype(info.wait_semas)>>
        wait_stage_masks;
    wait_stage_masks.fill(vk::PipelineStageFlagBits::eAllCommands);
    wait_stage_masks[1] = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    if (pending_external_wait) {
        ASSERT(info.num_wait_semas < info.wait_semas.size());
        wait_stage_masks[info.num_wait_semas] = vk::PipelineStageFlagBits::eAllCommands;
        info.AddWait(pending_external_wait, pending_external_value);
        pending_external_wait = nullptr;
    }

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    TraceSubmit(info);
    auto submit_result = execution_queue.submit(submit_info, info.fence);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceLost("submit");
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    work_semaphore.Refresh();
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsExtendedDynamicStateSupported(), instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
