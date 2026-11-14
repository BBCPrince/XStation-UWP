// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <set>
#include <tuple>
#include <string>
#include <array>
#include <chrono>
#include <pthread.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <time.h>
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include "bbport_threads.h"
#include "bbport_irq.h"
#include <cstdio>
#include <boost/preprocessor/stringize.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "bbport_write_log.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" int runtime_memory_write_backing_shared(uintptr_t address, const void* data,
                                                  uint64_t size);

namespace AmdGpu {

namespace {
/// bbport (BB_FRAME_STATS): per compute queue, when it was last submitted to and when its last
/// fence was written (steady clock ns), for the guest's compute round trip.
std::array<std::atomic<u64>, 64> asc_submit_ns{}, asc_fence_ns{};
u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace


static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
}

void Liverpool::ProcessCommands() {
    if (num_commands == 0) {
        return;
    }
    // bbport: commands touch the caches and the scheduler. No command may run before the drain:
    // one queued by a guest thread after a check that saw none would run beside the draw
    // recording thread (both recording into the scheduler: null record chunk).
    if (rasterizer) {
        rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCommands);
    }
    // Process incoming commands with high priority
    while (num_commands) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        callback();
    }
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
    BbThreads::RaiseCriticalPriority();
#ifndef _WIN32
    if (clockid_t clock; pthread_getcpuclockid(pthread_self(), &clock) == 0) {
        BbStats::gpu_thread_clock.store(static_cast<int>(clock));
    }
#endif
    gpu_id = std::this_thread::get_id();
#if defined(__linux__) || defined(_WIN32)
    gpu_tid = gettid();
#endif

    while (!stoken.stop_requested()) {
        {
            const auto idle_start = std::chrono::steady_clock::now();
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(submit_cv, lk, stoken,
                                [this] { return num_commands || num_submits || submit_done; });
            BbStats::gpu_idle_total_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - idle_start).count(),
                std::memory_order_relaxed);
            BbStats::gpu_idle_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - idle_start).count(),
                std::memory_order_relaxed);
        }
        if (stoken.stop_requested()) {
            break;
        }

        VideoCore::StartCapture();

        curr_qid = -1;

        while (num_submits || num_commands) {
            ProcessCommands();

            curr_qid = (curr_qid + 1) % num_mapped_queues;

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            task.resume();

            if (task.done()) {
                task.destroy();

                std::scoped_lock lock{queue.m_access};
                queue.submits.pop();

                if (curr_qid != GfxQueueId) {
                    asc_pending.fetch_sub(1, std::memory_order_relaxed);
                }
                --num_submits;
                std::scoped_lock lock2{submit_mutex};
                submit_cv.notify_all();
            }
        }

        if (submit_done) {
            VideoCore::EndCapture();
            if (rasterizer) {
                rasterizer->OnSubmit();
                rasterizer->Flush();
            }
            submit_done = false;
        }

        // bbport: the guest takes GPU idle (sceGnmSubmitDone) as its work being done and frees
        // the objects that hold its fence labels. The labels must be written before: the draw
        // recording thread and the fences it deferred to the Vulkan recording thread
        // (RecorderFences). Else a late fence write lands in freed memory (a corrupted guest
        // heap free list after minutes of play).
        // The same holds when sceGnmSubmitDone finds the GPU idle and does not block at all:
        // decoded (num_submits 0) is not done until then (work_retired).
        if (rasterizer) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonSubmissionEnd);
            rasterizer->WaitDeferredSignals();
        }
        {
            std::scoped_lock lk{submit_mutex};
            if (num_submits == 0) {
                work_retired = true;
            }
        }
        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
    }
}

namespace {
/// bbport: packets the GPU command thread handles while the draw recording thread may still
/// work on earlier draws (register state, draws it hands over, nested buffers). Every other
/// packet first waits for that thread to run dry (Rasterizer::DrainDrawPipe).
bool PipelinedOpcode(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::Nop:
    case PM4ItOpcode::ContextControl:
    case PM4ItOpcode::ClearState:
    case PM4ItOpcode::SetConfigReg:
    case PM4ItOpcode::SetContextReg:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetUconfigReg:
    case PM4ItOpcode::IndexType:
    case PM4ItOpcode::NumInstances:
    case PM4ItOpcode::IndexBase:
    case PM4ItOpcode::IndexBufferSize:
    case PM4ItOpcode::SetBase:
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::IndirectBuffer:
    case PM4ItOpcode::AcquireMem:
    case PM4ItOpcode::PfpSyncMe:
    case PM4ItOpcode::IncrementDeCounter:
    case PM4ItOpcode::WaitOnCeCounter:
    case PM4ItOpcode::EventWrite:     // occlusion results only, no rasterizer state
    case PM4ItOpcode::EventWriteEop:  // runs in order on the recording thread (RunInOrder)
    case PM4ItOpcode::EventWriteEos:
    case PM4ItOpcode::WaitRegMem:     // waits for the recording thread only while unmet
    case PM4ItOpcode::DispatchDirect: // handed over like draws (PipelinedDispatch)
    case PM4ItOpcode::DrawIndirect:   // likewise (PipelinedIndirectDraws; else drains itself)
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
    case PM4ItOpcode::DispatchIndirect:
    case PM4ItOpcode::WriteData:     // run in order on the recording thread (RunInOrder)
    case PM4ItOpcode::DmaData:
        return true;
    default:
        return false;
    }
}
} // namespace

namespace {
void SignalEop(const PM4CmdEventWriteEop& eop) {
    eop.SignalFence(
        [](void* address, u64 data, u32 num_bytes) {
            auto* memory = Core::Memory::Instance();
            BbWriteLog::Note(reinterpret_cast<u64>(address), &data, num_bytes, BbWriteLog::Fence);
            if (!memory->TryWriteBacking(address, &data, num_bytes)) {
                memcpy(address, &data, num_bytes);
            }
        },
        [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
}

// bbport: end-of-pipe/-shader events run in order with the draws (Rasterizer::RunInOrder: on
// the draw recording thread when the draw pipeline is in use).
void RunEventWriteEop(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto eop = *reinterpret_cast<const PM4CmdEventWriteEop*>(data);
    rasterizer.ProcessDownloadImages();
    // Guest memory copies on the copy threads precede the fence the guest sees.
    // BB_ASYNC_FENCES=1 writes it once they are done instead of waiting for them; measured
    // slower (79 vs 83 FPS): the guest waits on these fences, and later fences stall it more
    // than the wait costs.
    static const bool async_fences = [] {
        const char* env = std::getenv("BB_ASYNC_FENCES");
        return env && env[0] == '1';
    }();
    if (async_fences && !BbToggle::Disabled(BbToggle::AsyncFences)) {
        BbCopy::AfterCopies([eop] { SignalEop(eop); });
    } else if (Vulkan::DrawPipe::OnStageB() && !BbToggle::Disabled(BbToggle::RecorderFences)) {
        // The draw recording thread does not wait: the Vulkan recording thread signals the
        // fence after the copies queued before it.
        rasterizer.SignalAfterHostCopies([eop] { SignalEop(eop); });
    } else {
        // Labels are written in order: not before the ones deferred to the recording thread.
        rasterizer.WaitHostCopies();
        rasterizer.WaitDeferredSignals();
        SignalEop(eop);
    }
}

// bbport (Rasterizer::PipelinesCompute): a compute queue fence, in order with the dispatches
// handed to the draw recording thread; as ProcessCompute's ReleaseMem.
struct ComputeFenceTask {
    PM4CmdReleaseMem release_mem;
    u32 pipe_id;
    bool idle; ///< no dispatch or memory write on its queue since the previous fence
};

void RunComputeFence(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto& task = *reinterpret_cast<const ComputeFenceTask*>(data);
    if (!task.idle) {
        rasterizer.WaitHostCopies(true);
        rasterizer.WaitDeferredSignals();
        rasterizer.ProcessDownloadImages();
    }
    static const bool backing = [] {
        const char* env = std::getenv("BB_FENCE_BACKING");
        return !(env && env[0] == '0');
    }();
    task.release_mem.SignalFence(
        [](void* address, u64 value, u32 num_bytes) {
            if (!(backing && runtime_memory_write_backing_shared(
                                 reinterpret_cast<uintptr_t>(address), &value, num_bytes))) {
                std::memcpy(address, &value, num_bytes);
            }
            if (BbStats::enabled) {
                BbStats::fence_stores.fetch_add(1, std::memory_order_relaxed);
            }
        },
        [pipe_id = task.pipe_id] {
            if (BbIrq::Enabled()) {
                BbIrq::Post(pipe_id);
                return;
            }
            Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(pipe_id));
        },
        [&rasterizer](VAddr dst, u16 gds_index, u16 num_dwords) {
            rasterizer.CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false, true);
        });
}

/// bbport: a comma-separated list of hex values (shader addresses) from an environment variable.
std::vector<u64> HexList(const char* name) {
    std::vector<u64> list;
    if (const char* env = std::getenv(name)) {
        for (const char* p = env; *p;) {
            char* end = nullptr;
            const u64 value = std::strtoull(p, &end, 16);
            if (end == p) {
                break;
            }
            list.push_back(value);
            p = *end ? end + 1 : end;
        }
    }
    return list;
}

// God of War port (2026-11-03, diagnostics): a DmaData whose destination is the group counts of
// a DispatchDirect packet (an indirect dispatch made by patching the command buffer). Printed once
// per queue, source kind, size and whether the parser applies it (the counts are read when the
// packet is parsed, so a GPU-timeline copy there arrives late).
void NoteDispatchPatch(const PM4DmaData* dma, const char* queue, bool handled) {
    if (dma->dst_sel != DmaDataDst::Memory && dma->dst_sel != DmaDataDst::MemoryUsingL2) {
        return;
    }
    const VAddr dst = dma->DstAddress<VAddr>();
    const u32 bytes = dma->NumBytes();
    if (bytes == 0 || bytes > 12 || (dst & 3) != 0) {
        return;
    }
    for (u32 back = 4; back <= 12; back += 4) {
        if ((dst & 0xfff) < back) {
            break; // the header would be on another page
        }
        const auto* header = reinterpret_cast<const PM4Header*>(dst - back);
        if (header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
            static std::mutex lock;
            static std::set<std::tuple<std::string, u32, u32, u32, bool>> said;
            std::scoped_lock guard{lock};
            if (said.emplace(queue, u32(dma->src_sel), bytes, back, handled).second) {
                std::printf("Dispatch patch: %s queue DmaData src_sel %u, %u bytes at dim %u of a "
                            "DispatchDirect: %s\n",
                            queue, u32(dma->src_sel), bytes, back / 4 - 1,
                            handled ? "applied" : "NOT applied (counts read before the copy)");
                std::fflush(stdout);
            }
            return;
        }
    }
}

void RunDmaData(Vulkan::Rasterizer& rasterizer, const u8* data) {
    BbShaderJobs::BeforeGuestSignal(); // translations started before it are done reading guest memory
    const auto* dma_data = reinterpret_cast<const PM4DmaData*>(data);
    ASSERT(dma_data->command.das == 0);
    if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
        rasterizer.FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                               dma_data->data, true);
    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
               dma_data->dst_sel == DmaDataDst::Gds) {
        rasterizer.CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                               dma_data->NumBytes(), true, false);
    } else if (dma_data->src_sel == DmaDataSrc::Data &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                               dma_data->data, false);
    } else if (dma_data->src_sel == DmaDataSrc::Gds &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                               dma_data->NumBytes(), false, true);
    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.CopyBuffer(dma_data->DstAddress<VAddr>(),
                               dma_data->SrcAddress<VAddr>(), dma_data->NumBytes(),
                               false, false);
    } else {
        UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                        u32(dma_data->dst_sel));
    }
}

void SignalFlip(Vulkan::Rasterizer& rasterizer, const u8*) {
    rasterizer.WaitDeferredSignals();
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
}

void RunWriteData(Vulkan::Rasterizer& rasterizer, const u8* data) {
    BbShaderJobs::BeforeGuestSignal(); // translations started before it are done reading guest memory
    const auto* header = reinterpret_cast<const PM4Header*>(data);
    const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(data);
    // Copies deferred to the recording thread, and fences deferred to it (RecorderFences),
    // precede writes the guest sees.
    rasterizer.WaitHostCopies();
    rasterizer.WaitDeferredSignals();
    BbWriteLog::Note(write_data->Address<u64>(), write_data->data,
                     (header->type3.count.Value() - 2) * sizeof(u32), BbWriteLog::WriteData);
    std::memcpy(write_data->Address<u64*>(), write_data->data,
                (header->type3.count.Value() - 2) * sizeof(u32));
}

void RunEventWriteEos(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto& event_eos = *reinterpret_cast<const PM4CmdEventWriteEos*>(data);
    // Copies deferred to the recording thread precede writes the guest sees.
    rasterizer.WaitHostCopies();
    rasterizer.WaitDeferredSignals();
    rasterizer.ProcessDownloadImages();
    event_eos.SignalFence([](void* address, u64 value, u32 num_bytes) {
        auto* memory = Core::Memory::Instance();
        BbWriteLog::Note(reinterpret_cast<u64>(address), &value, num_bytes, BbWriteLog::Fence);
        if (!memory->TryWriteBacking(address, &value, num_bytes)) {
            memcpy(address, &value, num_bytes);
        }
    });
    if (event_eos.command == PM4CmdEventWriteEos::Command::GdsStore) {
        ASSERT(event_eos.size == 1);
        rasterizer.Finish();
        const u32 value = rasterizer.ReadDataFromGds(event_eos.gds_index);
        *event_eos.Address() = value;
    }
}
} // namespace

// God of War port (2026-11-03, BB_WAIT_TRACE): per waited address, drains after which the
// value was there / was still not.
static std::unordered_map<u64, std::pair<u64, u64>> wait_drain_results;

void Liverpool::NotePendingFences(const auto& event) {
    const u64 position = rasterizer->DrawPipeHead();
    while (!pending_fences.empty() && rasterizer->DrawPipeReached(pending_fences.front().position)) {
        pending_fences.pop_front();
    }
    constexpr bool is_eop = requires { event.SignalFence([](void*, u64, u32) {}, [] {}); };
    const auto note = [&](void* address, u64 data, u32 num_bytes) {
        BbWriteLog::NoteIntent(reinterpret_cast<u64>(address), &data, num_bytes,
                               is_eop ? BbWriteLog::FenceIntent : BbWriteLog::EosIntent);
        pending_fences.push_back({reinterpret_cast<VAddr>(address), data, num_bytes, position});
        rasterizer->NotePendingGpuWrite(reinterpret_cast<VAddr>(address), num_bytes);
    };
    if constexpr (requires { event.SignalFence(note, [] {}); }) {
        event.SignalFence(note, [] {});
    } else {
        event.SignalFence(note);
    }
}

void Liverpool::NotePendingWrite(const PM4CmdWriteData& write_data, u32 num_bytes) {
    if (num_bytes > sizeof(u64)) {
        return; // labels are small; larger writes are not waited on
    }
    u64 data = 0;
    std::memcpy(&data, write_data.data, num_bytes);
    pending_fences.push_back(
        {write_data.Address<VAddr>(), data, num_bytes, rasterizer->DrawPipeHead()});
}

bool Liverpool::PendingFenceValue(VAddr address, u32& value) {
    for (auto it = pending_fences.rbegin(); it != pending_fences.rend(); ++it) {
        if (rasterizer->DrawPipeReached(it->position)) {
            break; // written already: memory holds it (or a later write)
        }
        if (address >= it->address && address + sizeof(u32) <= it->address + it->num_bytes) {
            u64 data = it->data;
            value = static_cast<u32>(data >> ((address - it->address) * 8));
            return true;
        }
    }
    return false;
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            // bbport: earlier draws on the recording thread may still read constants here.
            if (rasterizer) {
                rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonConstRam);
            }
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            task.handle.destroy(); // bbport: the finished frame (final_suspend keeps it)
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

// bbport: graphics-register effects of a type-3 packet. The GPU thread and the draw-preparation
// workers both go through this function, and both fold every register-writing packet into a
// running checksum: equal checksums at a draw mean equal register files.
u64 Liverpool::HashRegisterPacket(u64 checksum, const u32* words, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        checksum = (checksum ^ words[i]) * 0x9E3779B97F4A7C15ull;
        checksum ^= checksum >> 29;
    }
    return checksum;
}

void Liverpool::ApplyGraphicsRegisterPacket(Regs& regs, const PM4Header* header, u64& checksum,
                                             RegDirty* dirty) {
    const u32 count = header->type3.NumWords();
    const auto* payload = reinterpret_cast<const u32*>(header + 2);
    const auto mark = [&](u32 word, u32 words) {
        if (dirty) {
            dirty->Mark(word, words);
        }
    };
    const auto mark_field = [&](const auto& field) {
        if (dirty) {
            dirty->MarkField(regs, field);
        }
    };
    switch (header->type3.opcode) {
    case PM4ItOpcode::ClearState:
        regs.SetDefaults();
        if (dirty) {
            dirty->Clear();
            dirty->reset = true;
        }
        break;
    case PM4ItOpcode::SetConfigReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::ConfigRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::ConfigRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::SetContextReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::ContextRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::ContextRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::SetShReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        // The compute program range goes to the queue's cs_state, not to regs.
        if (!(set_data->reg_offset >= 0x200 &&
              set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4))) {
            std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset], payload,
                        (count - 1) * sizeof(u32));
            mark(Regs::ShRegWordOffset + set_data->reg_offset, count - 1);
        }
        break;
    }
    case PM4ItOpcode::SetUconfigReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::UconfigRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::IndexType:
        regs.index_buffer_type.raw = reinterpret_cast<const PM4CmdDrawIndexType*>(header)->raw;
        mark_field(regs.index_buffer_type);
        break;
    case PM4ItOpcode::DrawIndex2: {
        const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
        regs.max_index_size = draw_index->max_size;
        regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
        regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
        regs.num_indices = draw_index->index_count;
        mark_field(regs.max_index_size);
        mark_field(regs.index_base_address);
        mark_field(regs.num_indices);
        regs.draw_initiator = draw_index->draw_initiator;
        mark_field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexOffset2: {
        const auto* draw_index_off = reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
        regs.max_index_size = draw_index_off->max_size;
        regs.num_indices = draw_index_off->index_count;
        regs.draw_initiator = draw_index_off->draw_initiator;
        mark_field(regs.max_index_size);
        mark_field(regs.num_indices);
        mark_field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexAuto: {
        const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
        mark_field(regs.num_indices);
        mark_field(regs.draw_initiator);
        regs.num_indices = draw_index->index_count;
        regs.draw_initiator = draw_index->draw_initiator;
        break;
    }
    case PM4ItOpcode::NumInstances:
        regs.num_instances.num_instances =
            reinterpret_cast<const PM4CmdDrawNumInstances*>(header)->num_instances;
        mark_field(regs.num_instances);
        break;
    case PM4ItOpcode::IndexBase: {
        const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
        regs.index_base_address.base_addr_lo = index_base->addr_lo;
        regs.index_base_address.base_addr_hi = index_base->addr_hi;
        mark_field(regs.index_base_address);
        break;
    }
    case PM4ItOpcode::IndexBufferSize:
        regs.num_indices = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header)->num_indices;
        mark_field(regs.num_indices);
        break;
    case PM4ItOpcode::EventWrite: {
        const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
        if (event->event_type.Value() != EventType::SoVgtStreamoutFlush) {
            return;
        }
        // TODO: handle proper synchronization, for now signal that update is done immediately
        regs.cp_strmout_cntl.offset_update_done = 1;
        mark_field(regs.cp_strmout_cntl);
        break;
    }
    default:
        return;
    }
    checksum = HashRegisterPacket(checksum, reinterpret_cast<const u32*>(header), count + 1);
}

// bbport: BB_DCB_STATS=1 — structure of graphics command buffers (read-only scan, printed every
// 5 s): how many buffers, draws per buffer, state set before the first draw. Input for
// processing command buffers on several threads.
namespace {
struct DcbStats {
    u64 buffers[2]{}, with_draws[2]{}, draws[2]{}, dispatches[2]{}, max_draws[2]{};
    u64 clear_first[2]{}, ctx_before[2]{}, sh_before[2]{}, ibs[2]{}, dwords[2]{};
    u64 hist[2][6]{}; // draws per buffer: 0, 1-9, 10-99, 100-499, 500-1999, 2000+
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};
DcbStats g_dcb_stats;
int g_dcb_depth;

bool IsDrawOpcode(PM4ItOpcode op) {
    switch (op) {
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::DrawIndirect:
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
        return true;
    default:
        return false;
    }
}

void ScanDcb(std::span<const u32> dcb, int depth) {
    auto& st = g_dcb_stats;
    const int d = depth > 0 ? 1 : 0;
    u64 draws = 0, dispatches = 0, ctx = 0, sh = 0, ibs = 0;
    bool clear_first = false;
    st.dwords[d] += dcb.size();
    for (size_t at = 0; at < dcb.size();) {
        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data() + at);
        if (header->type == 2) {
            ++at;
            continue;
        }
        if (header->type != 3) {
            break;
        }
        const PM4ItOpcode op = header->type3.opcode;
        if (IsDrawOpcode(op)) {
            ++draws;
        } else if (op == PM4ItOpcode::DispatchDirect || op == PM4ItOpcode::DispatchIndirect) {
            ++dispatches;
        } else if (op == PM4ItOpcode::IndirectBuffer) {
            ++ibs;
        } else if (draws == 0) {
            if (op == PM4ItOpcode::ClearState) {
                clear_first = true;
            } else if (op == PM4ItOpcode::SetContextReg) {
                ctx += header->type3.NumWords() - 1;
            } else if (op == PM4ItOpcode::SetShReg) {
                sh += header->type3.NumWords() - 1;
            }
        }
        at += header->type3.NumWords() + 1;
    }
    ++st.buffers[d];
    st.draws[d] += draws;
    st.dispatches[d] += dispatches;
    st.ibs[d] += ibs;
    st.max_draws[d] = std::max(st.max_draws[d], draws);
    if (draws) {
        ++st.with_draws[d];
        st.clear_first[d] += clear_first;
        st.ctx_before[d] += ctx;
        st.sh_before[d] += sh;
    }
    const int bucket = draws == 0 ? 0 : draws < 10 ? 1 : draws < 100 ? 2 : draws < 500 ? 3
                     : draws < 2000 ? 4 : 5;
    ++st.hist[d][bucket];

    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - st.window).count();
    if (depth == 0 && secs >= 5.0) {
        for (int k = 0; k < 2; ++k) {
            const double with = std::max<u64>(st.with_draws[k], 1);
            std::printf("DCB stats %s: %.0f buffers/s (%.0f with draws), %.0f draws/s, %.0f "
                        "dispatches/s, max %llu draws in one buffer, %.0f IBs/s, %.0f kdw/s; "
                        "draw buffers: %.0f%% start with ClearState, %.0f ctx + %.0f sh reg "
                        "writes before first draw; draws/buffer hist 0:%llu 1-9:%llu 10-99:%llu "
                        "100-499:%llu 500-1999:%llu 2000+:%llu\n",
                        k ? "indirect" : "submitted", st.buffers[k] / secs, st.with_draws[k] / secs,
                        st.draws[k] / secs, st.dispatches[k] / secs,
                        static_cast<unsigned long long>(st.max_draws[k]), st.ibs[k] / secs,
                        st.dwords[k] / secs / 1000.0, 100.0 * st.clear_first[k] / with,
                        st.ctx_before[k] / with, st.sh_before[k] / with,
                        static_cast<unsigned long long>(st.hist[k][0]),
                        static_cast<unsigned long long>(st.hist[k][1]),
                        static_cast<unsigned long long>(st.hist[k][2]),
                        static_cast<unsigned long long>(st.hist[k][3]),
                        static_cast<unsigned long long>(st.hist[k][4]),
                        static_cast<unsigned long long>(st.hist[k][5]));
        }
        st = DcbStats{};
    }
}
} // namespace

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                           u64 seq,
                                           std::shared_ptr<const BbCommandBufferCopy> command_copy) {
    // The coroutine parameter keeps the copied spans alive across every yield.
    (void)command_copy;
    FIBER_ENTER(dcb_task_name);
    // Top-level buffers enqueued for the draw preparation workers (nested IBs are not).
    Vulkan::DrawPreparation* draw_prep =
        seq != NoSeq && rasterizer ? &rasterizer->GetDrawPreparation() : nullptr;
    if (draw_prep) {
        draw_prep->BeginSubmission(seq, regs, gfx_reg_checksum);
    }
    static const bool dcb_stats = EmulatorSettingsImpl::Flag("BB_DCB_STATS", false);
    const int dcb_depth = g_dcb_depth;
    if (dcb_stats) {
        ScanDcb(dcb, dcb_depth);
    }

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    // bbport (BB_GFX_YIELD=<packets>, 0 off): the graphics queue only yielded to the compute
    // queues when it had to wait, so compute work the guest submitted (and waits for: TLOU's
    // ~1400 skinning dispatches per frame, each followed by a fence) waited for the rest of the
    // command buffer. With compute submissions pending it yields every so many packets.
    static const u32 yield_packets = [] {
        const char* env = std::getenv("BB_GFX_YIELD");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    u32 packets_since_yield = 0;
    while (!dcb.empty()) {
        ProcessCommands();
        if (yield_packets && ++packets_since_yield >= yield_packets &&
            asc_pending.load(std::memory_order_relaxed) != 0) {
            packets_since_yield = 0;
            BbStats::gfx_yields.fetch_add(1, std::memory_order_relaxed);
            YIELD_GFX();
        }

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;

        switch (type) {
        default:
        case 0:
            // bbport: a garbage dword read as a packet header (a command buffer the game was
            // already reusing: "Unimplemented PM4 type 0 ... size: 15810") stopped the whole
            // game. The rest of this buffer is dropped instead; the loop ends normally, so the
            // submission's cleanup below still runs. Idea from parsifal295 (PR #57).
            if (static int reported = 0; reported < 16) {
                ++reported;
                LOG_WARNING(Render, "PM4 header {:#010x} (type {}) at +{:#x}: rest of the buffer "
                                    "dropped",
                            header->raw, type, reinterpret_cast<uintptr_t>(dcb.data()) - base_addr);
            }
            dcb = {};
            continue;
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            ApplyGraphicsRegisterPacket(regs, header, gfx_reg_checksum, &pipe_dirty);
            // DmaData to 0x3022C does nothing here (skipped below): no need to wait.
            if (rasterizer && !PipelinedOpcode(opcode) &&
                !(opcode == PM4ItOpcode::DmaData &&
                  reinterpret_cast<const PM4DmaData*>(header)->dst_addr_lo == 0x3022C)) {
                rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                // They may write guest memory: not before fences deferred earlier.
                rasterizer->WaitDeferredSignals();
            }
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    // bbport: after the writes before it (the buffer label: WriteData, which
                    // may still be queued on the draw recording thread).
                    if (rasterizer) {
                        rasterizer->RunInOrder(&SignalFlip, header, sizeof(u32),
                                               BbToggle::PipelinedMemoryWrites);
                    } else {
                        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState:
            case PM4ItOpcode::SetConfigReg: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } // other SH registers: ApplyGraphicsRegisterPacket
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetPredication: {
                LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION");
                break;
            }
            case PM4ItOpcode::IndexType: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::DrawIndex2: {
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker("gfx:{}:DrawIndex2", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(true, 0, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexOffset2", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->Draw(true, draw_index_off->index_offset, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(false, 0, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_index_indirect->base_vtx_loc,
                                                 draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count, 0, draw_index_indirect->base_vtx_loc,
                            draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectCountMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count,
                            draw_index_indirect->count_indirect_enable.Value()
                                ? draw_index_indirect->count_addr
                                : 0,
                            draw_index_indirect->base_vtx_loc, draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DispatchDirect", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->DispatchDirect(); });
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->DispatchIndirect(indirect_args_addr, offset, size); });
                break;
            }
            case PM4ItOpcode::NumInstances:
            case PM4ItOpcode::IndexBase:
            case PM4ItOpcode::IndexBufferSize: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // registers: ApplyGraphicsRegisterPacket
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        static constexpr u64 OcclusionCounterStep = 0x2FFFFFFULL;
                        u64* results = event->Address<u64*>();
                        for (s32 i = 0; i < num_counter_pairs; ++i, results += 2) {
                            *results = pixel_counter | OcclusionCounterValidMask;
                        }
                        pixel_counter += OcclusionCounterStep;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                if (rasterizer &&
                    rasterizer->RunInOrder(&RunEventWriteEos, event_eos, sizeof(*event_eos))) {
                    NotePendingFences(*event_eos);
                }
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                if (rasterizer) {
                    if (rasterizer->RunInOrder(&RunEventWriteEop, event_eop, sizeof(*event_eop))) {
                        NotePendingFences(*event_eop);
                    }
                } else {
                    SignalEop(*event_eop);
                }
                break;
            }
            case PM4ItOpcode::DmaData: {
                BbShaderJobs::BeforeGuestSignal(); // translations started before it are done reading guest memory
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                NoteDispatchPatch(dma_data, "graphics", false);
                if (rasterizer->RunInOrder(&RunDmaData, dma_data, sizeof(PM4DmaData),
                                           BbToggle::PipelinedMemoryWrites) &&
                    (dma_data->dst_sel == DmaDataDst::Memory ||
                     dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->NotePendingGpuWrite(dma_data->DstAddress<VAddr>(),
                                                    dma_data->NumBytes());
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                BbShaderJobs::BeforeGuestSignal(); // translations started before it are done reading guest memory
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                ASSERT(!write_data->wr_one_addr.Value());
                if (rasterizer) {
                    // In order with the draws (on the draw recording thread when in use).
                    BbWriteLog::NoteIntent(write_data->Address<u64>(), write_data->data,
                                           (count - 2) * sizeof(u32),
                                           BbWriteLog::WriteDataIntent);
                    if (rasterizer->RunInOrder(&RunWriteData, header, (count + 1) * sizeof(u32),
                                               BbToggle::PipelinedMemoryWrites)) {
                        NotePendingWrite(*write_data, (count - 2) * sizeof(u32));
                        rasterizer->NotePendingGpuWrite(write_data->Address<VAddr>(),
                                                        (count - 2) * sizeof(u32));
                    }
                } else {
                    std::memcpy(write_data->Address<u64*>(), write_data->data, (count - 2) * 4);
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    mem_semaphore->Signal();
                } else {
                    while (!mem_semaphore->Signaled()) {
                        YIELD_GFX();
                    }
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
                while (!rewind->Valid()) {
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    if (rasterizer) {
                        rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                    }
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); });
                    break;
                }
                // bbport: a fence handed to the draw recording thread counts as written: that
                // thread runs everything in stream order.
                if (u32 value; wait_reg_mem->mem_space.Value() ==
                                   PM4CmdWaitRegMem::MemSpace::Memory &&
                               !BbToggle::Disabled(BbToggle::PendingFenceWaits) &&
                               PendingFenceValue(wait_reg_mem->Address<VAddr>(), value) &&
                               wait_reg_mem->TestValue(value)) {
                    break;
                }
                // God of War port (2026-11-03, BB_WAIT_TRACE=1): the waits that drain, by
                // address, every 5 s: function, reference, the value found, whether a pending
                // fence of any value is there.
                static const bool wait_trace = [] {
                    const char* env = std::getenv("BB_WAIT_TRACE");
                    return env && env[0] == '1';
                }();
                if (wait_trace && !wait_reg_mem->Test(regs.reg_array)) {
                    struct WaitSite {
                        u64 count = 0;
                        u32 function = 0, ref = 0, mask = 0, found = 0;
                        bool pending = false, memory = false;
                    };
                    static std::unordered_map<u64, WaitSite> sites;
                    static auto printed = std::chrono::steady_clock::now();
                    const bool memory =
                        wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory;
                    const u64 key = memory ? wait_reg_mem->Address<VAddr>() : wait_reg_mem->Reg();
                    auto& site = sites[key];
                    ++site.count;
                    site.function = static_cast<u32>(wait_reg_mem->function.Value());
                    site.ref = wait_reg_mem->ref;
                    site.mask = wait_reg_mem->mask;
                    site.memory = memory;
                    site.found = memory ? *wait_reg_mem->Address() : regs.reg_array[key];
                    site.pending = false;
                    for (const auto& fence : pending_fences) {
                        site.pending |= memory && key >= fence.address &&
                                        key < fence.address + fence.num_bytes;
                    }
                    if (std::chrono::steady_clock::now() - printed > std::chrono::seconds(5)) {
                        printed = std::chrono::steady_clock::now();
                        std::vector<std::pair<u64, WaitSite>> top(sites.begin(), sites.end());
                        std::ranges::sort(top, [](const auto& a, const auto& b) {
                            return a.second.count > b.second.count;
                        });
                        std::printf("Wait trace (graphics WaitRegMem not met, 5 s):\n");
                        for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
                            const auto& [k, s] = top[i];
                            const auto drained = wait_drain_results[k];
                            std::printf("  %s %#llx: %llu waits, function %u ref %#x mask %#x, "
                                        "found %#x, pending fence there %u; drains then met %llu, "
                                        "not met %llu\n",
                                        s.memory ? "addr" : "reg", (unsigned long long)k,
                                        (unsigned long long)s.count, s.function, s.ref, s.mask,
                                        s.found, u32(s.pending), (unsigned long long)drained.first,
                                        (unsigned long long)drained.second);
                        }
                        std::fflush(stdout);
                        sites.clear();
                        wait_drain_results.clear();
                    }
                }
                // God of War port (2026-11-03): yield first. God of War's graphics queue waits
                // for counters its async compute queues advance (BB_WAIT_TRACE: values 4-13
                // behind, mostly no fence of theirs handed over yet): their packets were not
                // decoded, and draining the draw pipe (~2,500 times a second, 46% of this
                // thread's time) could not help. Other queues run first; the pipe is drained only
                // when a yield brought nothing new (no fence handed over, nothing written).
                // Measured slower (17 vs 20 FPS, boot102yield): opt-in, BB_WAIT_YIELD_FIRST=1.
                static const bool yield_first = [] {
                    const char* env = std::getenv("BB_WAIT_YIELD_FIRST");
                    return env && env[0] == '1';
                }();
                const bool pending_check =
                    wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory &&
                    !BbToggle::Disabled(BbToggle::PendingFenceWaits);
                // God of War Xbox (2026-11-05) experiment, BB_WAIT_SKIP_GPU_WRITTEN=1: a wait on memory
                // the GPU writes (a buffer marked GPU-modified, e.g. a counter an async compute shader
                // advances) is not waited for on the CPU: all queues' work goes through the one draw
                // pipe into one Vulkan queue in stream order, so the producer recorded before this point
                // runs first on the GPU anyway. ~1,800 drains + GPU round trips a second on the Xbox
                // (gow-pipe5: WaitRegMem 29% of stage A). Wrong if the producer is decoded only later.
                static const bool skip_gpu_written = [] {
                    const char* env = std::getenv("BB_WAIT_SKIP_GPU_WRITTEN");
                    return env && env[0] == '1';
                }();
                if (skip_gpu_written && pending_check && rasterizer &&
                    !wait_reg_mem->Test(regs.reg_array) &&
                    rasterizer->GetBufferCache().IsRegionGpuModified(wait_reg_mem->Address<VAddr>(), 4)) {
                    static std::atomic<u64> skipped{0};
                    const u64 n = skipped.fetch_add(1, std::memory_order_relaxed);
                    if ((n & (n + 1)) == 0 && n < 70000) { // 1, 2, 4, ... : a few lines only
                        std::printf("Wait skip: %llu waits on GPU-written memory not held on the CPU\n",
                                    (unsigned long long)(n + 1));
                    }
                    break;
                }
                bool progress = yield_first;
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    if (u32 value; pending_check &&
                                   PendingFenceValue(wait_reg_mem->Address<VAddr>(), value) &&
                                   wait_reg_mem->TestValue(value)) {
                        break;
                    }
                    if (rasterizer && !rasterizer->DrawPipeIdle() && !progress) {
                        rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                        if (wait_trace) {
                            auto& result = wait_drain_results[pending_check
                                                                  ? wait_reg_mem->Address<VAddr>()
                                                                  : wait_reg_mem->Reg()];
                            ++(wait_reg_mem->Test(regs.reg_array) ? result.first : result.second);
                        }
                        progress = yield_first;
                        continue;
                    }
                    const size_t fences_before = pending_fences.size();
                    const u64 head_before = rasterizer ? rasterizer->DrawPipeHead() : 0;
                    const u32 value_before = wait_reg_mem->mem_space.Value() ==
                                                     PM4CmdWaitRegMem::MemSpace::Memory
                                                 ? *wait_reg_mem->Address()
                                                 : 0;
                    YIELD_GFX();
                    progress = yield_first &&
                               (pending_fences.size() != fences_before ||
                                (rasterizer && rasterizer->DrawPipeHead() != head_before) ||
                                (wait_reg_mem->mem_space.Value() ==
                                     PM4CmdWaitRegMem::MemSpace::Memory &&
                                 *wait_reg_mem->Address() != value_before));
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                // Tasks start suspended: the nested body (and its scan) runs on this resume.
                g_dcb_depth = dcb_depth + 1;
                RESUME_GFX(task);
                g_dcb_depth = dcb_depth;

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                // bbport: free the finished nested task (final_suspend keeps its frame): one
                // leaked per indirect buffer, the RAM growth of long sessions.
                task.handle.destroy();
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }

    if (rasterizer && seq != NoSeq) {
        rasterizer->RetireSubmission(); // prepared draws live until the recording thread is past
    }
    if (draw_prep) {
        draw_prep->EndSubmission();
    }
    if (seq != NoSeq && BbStats::enabled) {
        BbStats::submissions.fetch_add(1, std::memory_order_relaxed);
#ifndef _WIN32 // bbport: per-thread rusage statistics are Linux-only
        if (rusage usage{}; getrusage(RUSAGE_THREAD, &usage) == 0) {
            BbStats::gpu_user_us.store(u64(usage.ru_utime.tv_sec) * 1000000 + usage.ru_utime.tv_usec,
                                       std::memory_order_relaxed);
            BbStats::gpu_sys_us.store(u64(usage.ru_stime.tv_sec) * 1000000 + usage.ru_stime.tv_usec,
                                      std::memory_order_relaxed);
            BbStats::gpu_invol_switches.store(usage.ru_nivcsw, std::memory_order_relaxed);
            BbStats::gpu_vol_switches.store(usage.ru_nvcsw, std::memory_order_relaxed);
            BbStats::gpu_minor_faults.store(usage.ru_minflt, std::memory_order_relaxed);
        }
#endif
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();

    // bbport (BB_BATCH_CS=<hex shader addresses>): consecutive direct dispatches of these shaders
    // run as one batched dispatch (Rasterizer::DispatchBatch), the fences between them after it
    // in order. A batch runs before any other packet and at the end of this submission, so
    // nothing the guest can wait for (WaitRegMem, semaphores, yields) is held back.
    static const std::vector<u64> batch_shaders = HexList("BB_BATCH_CS");
    struct PendingFence {
        PM4CmdReleaseMem release_mem;
        bool idle;
    };
    std::vector<ComputeProgram> batch_jobs;
    std::vector<PendingFence> batch_fences;
    const auto flush_batch = [&] {
        if (batch_jobs.empty()) {
            return;
        }
        {
            BbCopy::ComputeQueueScope scope;
            rasterizer->DispatchBatch(batch_jobs);
        }
        for (const auto& fence : batch_fences) {
            ComputeFenceTask task{fence.release_mem, queue.pipe_id, fence.idle};
            RunComputeFence(*rasterizer, reinterpret_cast<const u8*>(&task));
        }
        batch_jobs.clear();
        batch_fences.clear();
    };

    while (!acb.empty()) {
        ProcessCommands();

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        u32 next_dw_off = header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;

        // bbport: compute work uses the caches, so the draw pipeline is drained first, except
        // for packets that touch neither memory nor shared state: NOPs, this queue's program
        // registers, and indirect buffers (their packets drain themselves). Draining before
        // every packet waited ~21k times/s on the Xbox (22% of stage A). BB_COMPUTE_DRAIN_ALL=1:
        // before every packet again.
        // A compute fence orders this queue's own work only (the hardware does not wait for the
        // graphics queue): with no dispatch or memory write on this queue since its previous
        // fence there is nothing to wait for before writing it (TLOU: ~26k such fences/s, each
        // waiting for every graphics copy). BB_COMPUTE_FENCE_SYNC=1: wait at every fence.
        static const bool fence_sync = [] {
            const char* env = std::getenv("BB_COMPUTE_FENCE_SYNC");
            return env && env[0] == '1';
        }();
        bool idle_fence = false;
        // Time per compute packet by opcode, including its drain (BB_FRAME_STATS).
        const auto packet_start = BbStats::enabled ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
        if (BbStats::enabled) {
            BbStats::compute_opcodes[static_cast<u32>(opcode) & 255].fetch_add(
                1, std::memory_order_relaxed);
        }
        if (opcode == PM4ItOpcode::ReleaseMem) {
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            idle_fence = !fence_sync && !queue.work_since_fence &&
                         release_mem->data_sel.Value() != DataSelect::GdsMemStore;
            queue.work_since_fence = false;
            BbStats::compute_fences.fetch_add(1, std::memory_order_relaxed);
            if (idle_fence) {
                BbStats::compute_fences_idle.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (opcode == PM4ItOpcode::DispatchDirect ||
                   opcode == PM4ItOpcode::DispatchIndirect || opcode == PM4ItOpcode::DmaData ||
                   opcode == PM4ItOpcode::WriteData) {
            queue.work_since_fence = true;
        }
        if (!batch_jobs.empty()) {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const bool joins =
                opcode == PM4ItOpcode::Nop || opcode == PM4ItOpcode::ReleaseMem ||
                opcode == PM4ItOpcode::DispatchDirect ||
                (opcode == PM4ItOpcode::SetShReg && set_data->reg_offset >= 0x200 &&
                 set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4));
            if (!joins) {
                flush_batch();
            }
        }

        // bbport (Rasterizer::PipelinesCompute): dispatches and fences of this queue go to the
        // draw recording thread in stream order instead of waiting for it to run dry.
        const bool pipelined_compute = rasterizer && rasterizer->PipelinesCompute();
        if (rasterizer) {
            static const bool drain_all = [] {
                const char* env = std::getenv("BB_COMPUTE_DRAIN_ALL");
                return env && env[0] == '1';
            }();
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const bool program_registers =
                opcode == PM4ItOpcode::SetShReg && set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4);
            // God of War port (2026-11-03): a WaitRegMem drains in its own case, only when its
            // value is not there yet with BB_ASC_WAIT_DRAIN_FIRST=0 (default: before it, as before). God of
            // War's compute queues wait ~2,500 times a second, mostly on values already written
            // or pending: each drained the draw pipe first (46% of this thread's time).
            // Measured (boot106ascw): 2,480 -> 318 such drains/s, the rest still 38%, FPS
            // unchanged (the remaining waits are real graphics -> compute dependencies).
            static const bool wait_drain_first = [] {
                const char* env = std::getenv("BB_ASC_WAIT_DRAIN_FIRST");
                return !(env && env[0] == '0');
            }();
            const bool handed_over =
                pipelined_compute &&
                (opcode == PM4ItOpcode::DispatchDirect || opcode == PM4ItOpcode::DispatchIndirect ||
                 opcode == PM4ItOpcode::ReleaseMem ||
                 (opcode == PM4ItOpcode::WaitRegMem && !wait_drain_first));
            // God of War port (2026-11-05): AcquireMem and SetQueueReg do nothing here; they drained
            // the pipe ~1,250 times a second (Xbox gow-pend1, 10% of stage A). BB_COMPUTE_NOOP_DRAIN=1: old.
            static const bool noop_drain = [] {
                const char* env = std::getenv("BB_COMPUTE_NOOP_DRAIN");
                return env && env[0] == '1';
            }();
            const bool no_op = !noop_drain && (opcode == PM4ItOpcode::AcquireMem ||
                                               opcode == PM4ItOpcode::SetQueueReg);
            if (drain_all || !(opcode == PM4ItOpcode::Nop || program_registers ||
                               opcode == PM4ItOpcode::IndirectBuffer || idle_fence ||
                               handed_over || no_op)) {
                // Counted by opcode in the "drains by reason" stats.
                rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
            }
        }

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            task.handle.destroy(); // bbport: the finished frame (final_suspend keeps it)
            break;
        }
        case PM4ItOpcode::DmaData: {
            BbShaderJobs::BeforeGuestSignal(); // translations started before it are done reading guest memory
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            ASSERT(dma_data->command.das == 0);
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(), dma_data->data,
                                       true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                       dma_data->NumBytes(), true, false);
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                       dma_data->data, false);
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                NoteDispatchPatch(dma_data, "compute", false);
                rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                       dma_data->NumBytes(), false, true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                    NoteDispatchPatch(dma_data, "compute", true);
                } else {
                    NoteDispatchPatch(dma_data, "compute", false);
                    rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                u32(dma_data->dst_sel));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            while (!rewind->Valid()) {
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
                // bbport: interleaves with the graphics stream at an unpredictable point, so
                // draw-preparation workers cannot reproduce it: make their checksums differ.
                gfx_reg_checksum = HashRegisterPacket(gfx_reg_checksum ^ 0xA5C0A5C0A5C0ull,
                                                      reinterpret_cast<const u32*>(header),
                                                      header->type3.NumWords() + 1);
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            if (static bool said = false; !said) {
                said = true;
                LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x} (said once)",
                            set_data->vqid.Value(), set_data->reg_offset.Value());
            }
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                flush_batch();
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                BbCopy::ComputeQueueScope scope;
                rasterizer->DispatchIndirect(it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            // bbport (test only, BB_SKIP_CS=<hex address>,...): these compute queue shaders are
            // not run (wrong output): the frame time without them, as an upper bound.
            static const std::vector<u64> skip_cs = [] {
                std::vector<u64> list;
                if (const char* env = std::getenv("BB_SKIP_CS")) {
                    for (const char* p = env; *p;) {
                        char* end = nullptr;
                        const u64 value = std::strtoull(p, &end, 16);
                        if (end == p) {
                            break;
                        }
                        list.push_back(value);
                        p = *end ? end + 1 : end;
                    }
                }
                return list;
            }();
            if (!skip_cs.empty() &&
                std::ranges::find(skip_cs, cs_program.Address<u64>()) != skip_cs.end()) {
                break;
            }
            if (!batch_shaders.empty() && cs_program.dim_y == 1 && cs_program.dim_z == 1 &&
                !rasterizer->PipelinesCompute() &&
                std::ranges::find(batch_shaders, cs_program.Address<u64>()) !=
                    batch_shaders.end()) {
                if (!batch_jobs.empty() &&
                    (batch_jobs.front().Address<u64>() != cs_program.Address<u64>() ||
                     batch_jobs.size() >= 256)) {
                    flush_batch();
                }
                batch_jobs.push_back(cs_program);
                break;
            }
            flush_batch();
            if (BbStats::enabled) {
                // bbport (BB_FRAME_STATS): compute queue dispatches by shader, every 2^16.
                struct Seen {
                    u64 count, groups, threads;
                };
                static std::unordered_map<u64, Seen> seen;
                static u64 total = 0;
                auto& s = seen[cs_program.Address<u64>()];
                ++s.count;
                s.groups += u64(cs_program.dim_x) * cs_program.dim_y * cs_program.dim_z;
                s.threads = u64(cs_program.num_thread_x.full) * cs_program.num_thread_y.full *
                            cs_program.num_thread_z.full;
                if ((++total & 0xFFFF) == 0) {
                    std::vector<std::pair<u64, Seen>> top(seen.begin(), seen.end());
                    std::ranges::sort(top, [](auto& a, auto& b) { return a.second.count > b.second.count; });
                    std::printf("Compute queue dispatches (last %llu, %zu shaders):",
                                static_cast<unsigned long long>(total), top.size());
                    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
                        // The code up to s_endpgm, once: <user dir>/cs-<address>.bin.
                        static std::unordered_map<u64, bool> dumped;
                        if (const char* dir = std::getenv("BB_GPU_USER_DIR");
                            dir && !dumped[top[i].first]) {
                            dumped[top[i].first] = true;
                            const auto* code = reinterpret_cast<const u32*>(top[i].first);
                            size_t words = 0;
                            while (words < 4096 && code[words++] != 0xBF810000u) {
                            }
                            const auto path = fmt::format("{}/cs-{:x}.bin", dir, top[i].first);
                            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                                std::fwrite(code, 4, words, f);
                                std::fclose(f);
                            }
                        }
                        std::printf(" %llx x%llu groups~%.1f threads/group %llu;",
                                    static_cast<unsigned long long>(top[i].first),
                                    static_cast<unsigned long long>(top[i].second.count),
                                    double(top[i].second.groups) / top[i].second.count,
                                    static_cast<unsigned long long>(top[i].second.threads));
                    }
                    std::printf("\n");
                    seen.clear();
                    total = 0;
                }
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchDirect",
                                    fmt::make_format_args(vqid, cmd_address), [&] {
                                        BbCopy::ComputeQueueScope scope;
                                        rasterizer->DispatchDirect();
                                    });
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchIndirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] {
                                        BbCopy::ComputeQueueScope scope;
                                        rasterizer->DispatchIndirect(ib_address, 0, size);
                                    });
            break;
        }
        case PM4ItOpcode::WriteData: {
            BbShaderJobs::BeforeGuestSignal(); // translations started before it are done reading guest memory
            // bbport: copies deferred to the recording thread precede writes the guest sees, and
            // so do graphics fences deferred to it (the pipe was drained before this packet).
            if (rasterizer) {
                rasterizer->WaitHostCopies();
                rasterizer->WaitDeferredSignals();
            }
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                mem_semaphore->Signal();
            } else {
                while (!mem_semaphore->Signaled()) {
                    YIELD_ASC(vqid);
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            // God of War port (2026-11-03): with pipelined compute, a fence handed to the draw
            // recording thread counts as written here too (what follows is handed over after
            // it), as for the graphics queue: God of War's compute queues wait on graphics
            // labels still in the pipe, and the graphics queue on theirs, until a drain.
            // No FPS change measured (boot103asc): opt-in, BB_ASC_PENDING_WAITS=1.
            static const bool asc_pending = [] {
                const char* env = std::getenv("BB_ASC_PENDING_WAITS");
                return env && env[0] == '1';
            }();
            const bool pending_check =
                asc_pending && pipelined_compute &&
                wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory &&
                !BbToggle::Disabled(BbToggle::PendingFenceWaits);
            while (!wait_reg_mem->Test(regs.reg_array)) {
                if (u32 value; pending_check &&
                               PendingFenceValue(wait_reg_mem->Address<VAddr>(), value) &&
                               wait_reg_mem->TestValue(value)) {
                    break;
                }
                // Not drained before this packet (see handed_over): the value may come from
                // work still in the pipe.
                if (pipelined_compute && !rasterizer->DrawPipeIdle()) {
                    // God of War port (2026-11-03, BB_WAIT_TRACE=1): compute waits that drain,
                    // by address: whether a pending fence / pipe write is there, and whether the
                    // drain brought the value.
                    static const bool asc_trace = [] {
                        const char* env = std::getenv("BB_WAIT_TRACE");
                        return env && env[0] == '1';
                    }();
                    const VAddr wait_addr = wait_reg_mem->Address<VAddr>();
                    const bool fence_there = [&] {
                        for (const auto& fence : pending_fences) {
                            if (wait_addr >= fence.address &&
                                wait_addr < fence.address + fence.num_bytes) {
                                return true;
                            }
                        }
                        return false;
                    }();
                    const bool write_there = rasterizer->PendingGpuWriteAt(wait_addr, 4);
                    const u32 found = *wait_reg_mem->Address();
                    rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                    if (asc_trace) {
                        struct AscSite {
                            u64 drains = 0, met = 0;
                            u32 function = 0, ref = 0, found = 0, queue = 0;
                            bool fence = false, write = false;
                        };
                        static std::unordered_map<u64, AscSite> asc_sites;
                        static auto asc_printed = std::chrono::steady_clock::now();
                        auto& site = asc_sites[wait_addr];
                        ++site.drains;
                        site.met += wait_reg_mem->Test(regs.reg_array);
                        site.function = static_cast<u32>(wait_reg_mem->function.Value());
                        site.ref = wait_reg_mem->ref;
                        site.found = found;
                        site.queue = vqid;
                        site.fence = fence_there;
                        site.write = write_there;
                        if (std::chrono::steady_clock::now() - asc_printed >
                            std::chrono::seconds(5)) {
                            asc_printed = std::chrono::steady_clock::now();
                            std::vector<std::pair<u64, AscSite>> top(asc_sites.begin(),
                                                                     asc_sites.end());
                            std::ranges::sort(top, [](const auto& a, const auto& b) {
                                return a.second.drains > b.second.drains;
                            });
                            std::printf("ASC wait trace (compute WaitRegMem drains, 5 s):\n");
                            for (size_t i = 0; i < std::min<size_t>(top.size(), 10); ++i) {
                                const auto& [k, s] = top[i];
                                std::printf("  queue %u addr %#llx: %llu drains, met after %llu; "
                                            "function %u ref %#x found %#x; pending fence %u, "
                                            "pipe write %u\n",
                                            s.queue, (unsigned long long)k,
                                            (unsigned long long)s.drains,
                                            (unsigned long long)s.met, s.function, s.ref, s.found,
                                            u32(s.fence), u32(s.write));
                            }
                            std::fflush(stdout);
                            asc_sites.clear();
                        }
                    }
                    continue;
                }
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            if (!batch_jobs.empty()) {
                // Written after the batch it follows (flush_batch).
                batch_fences.push_back(
                    {*reinterpret_cast<const PM4CmdReleaseMem*>(header), idle_fence});
                break;
            }
            if (pipelined_compute) {
                // In order with the dispatches handed over before it (idle fences too: a later
                // fence value must not land before an earlier one).
                ComputeFenceTask task{*reinterpret_cast<const PM4CmdReleaseMem*>(header),
                                      queue.pipe_id, idle_fence};
                // God of War port (2026-11-03): a compute fence handed to the draw recording
                // thread counts as written for the graphics queue's WaitRegMem, as graphics
                // EOP/EOS fences do (NotePendingFences). God of War's graphics queue waits on
                // its async compute fences ~2,500 times a second; each found the label not yet
                // written and drained the draw pipe (46% of the command processor's time).
                // No FPS change measured (boot100pcf: those waits came from the compute queues'
                // own drains): opt-in, BB_PENDING_COMPUTE_FENCES=1.
                static const bool note_pending = [] {
                    const char* env = std::getenv("BB_PENDING_COMPUTE_FENCES");
                    return env && env[0] == '1';
                }();
                if (rasterizer->RunInOrder(&RunComputeFence, &task, sizeof(task)) &&
                    note_pending) {
                    const u64 position = rasterizer->DrawPipeHead();
                    while (!pending_fences.empty() &&
                           rasterizer->DrawPipeReached(pending_fences.front().position)) {
                        pending_fences.pop_front();
                    }
                    task.release_mem.SignalFence(
                        [&](void* address, u64 data, u32 num_bytes) {
                            pending_fences.push_back(
                                {reinterpret_cast<VAddr>(address), data, num_bytes, position});
                            rasterizer->NotePendingGpuWrite(reinterpret_cast<VAddr>(address),
                                                            num_bytes);
                        },
                        [] {}, [](VAddr, u16, u16) {});
                }
                break;
            }
            // bbport: copies deferred to the recording thread precede writes the guest sees, and
            // so do graphics fences deferred to it (the pipe was drained before this packet);
            // not for a fence after no work of this queue (idle_fence above). Graphics stream
            // copies are left to the copy threads (BbCopy::WaitRequired).
            if (BbStats::enabled && vqid + 1 < asc_submit_ns.size()) {
                // Our side: from the queue's last submission to this fence.
                const u64 now = NowNs();
                if (const u64 submit = asc_submit_ns[vqid + 1].load(std::memory_order_relaxed);
                    submit && now > submit) {
                    BbStats::asc_gpu_side_ns.fetch_add(now - submit, std::memory_order_relaxed);
                    BbStats::asc_gpu_sides.fetch_add(1, std::memory_order_relaxed);
                }
                asc_fence_ns[vqid + 1].store(now, std::memory_order_relaxed);
            }
            BbStats::Laps laps;
            if (BbStats::enabled) {
                BbStats::fence_data_sel[static_cast<u32>(reinterpret_cast<const PM4CmdReleaseMem*>(
                                            header)->data_sel.Value()) & 7]
                    .fetch_add(1, std::memory_order_relaxed);
            }
            if (rasterizer && !idle_fence) {
                rasterizer->WaitHostCopies(true);
                laps.Mark(8);
                rasterizer->WaitDeferredSignals();
                laps.Mark(9);
            }
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            if (rasterizer && !idle_fence) {
                rasterizer->ProcessDownloadImages();
                laps.Mark(10);
            }
            // A plain store: through the memory backing (as SignalEop) it no longer faults, but
            // then the fault no longer opens the window around it for the guest's own writes:
            // Xbox 27.5 -> 15 FPS, guest copies x3.
            release_mem->SignalFence(
                [&laps](void* address, u64 value, u32 num_bytes) {
                    // bbport: through the memory backing's view of the same page where the
                    // region is shared (not a private CPU copy): a store to the guest's view
                    // took a ~2.3 us first-write trap per fence on the Xbox (~43k/s).
                    // BB_FENCE_BACKING=0: always the plain store.
                    static const bool backing = [] {
                        const char* env = std::getenv("BB_FENCE_BACKING");
                        return !(env && env[0] == '0');
                    }();
                    const u64 t0 = __rdtsc();
                    const bool via_backing =
                        backing && runtime_memory_write_backing_shared(
                                       reinterpret_cast<uintptr_t>(address), &value, num_bytes);
                    if (!via_backing) {
                        std::memcpy(address, &value, num_bytes);
                    }
                    if (BbStats::enabled) {
                        BbStats::fence_store_cycles.fetch_add(__rdtsc() - t0,
                                                              std::memory_order_relaxed);
                        BbStats::fence_stores.fetch_add(1, std::memory_order_relaxed);
                        if (!via_backing) {
                            BbStats::fence_store2_cycles.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    laps.Mark(11);
                },
                [pipe_id = queue.pipe_id] {
                    // bbport (BB_ASYNC_IRQ=1): delivered by a helper thread (bbport_irq.cpp).
                    if (BbIrq::Enabled()) {
                        BbIrq::Post(pipe_id);
                        return;
                    }
                    Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(pipe_id));
                },
                [this](VAddr dst, u16 gds_index, u16 num_dwords) {
                    rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false, true);
                });
            laps.Mark(12);
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }
        if (BbStats::enabled) {
            BbStats::compute_opcode_ns[static_cast<u32>(opcode) & 255].fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - packet_start).count(),
                std::memory_order_relaxed);
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }
    flush_batch();

    FIBER_EXIT;
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];

    std::shared_ptr<const BbCommandBufferCopy> command_copy;
    if (EmulatorSettings.IsCopyGpuBuffers()) {
        command_copy = std::make_shared<BbCommandBufferCopy>(dcb, ccb);
        dcb = command_copy->Draw();
        ccb = command_copy->Constant();
    }

    // The copy for the draw preparation workers is made before taking the queue lock, which
    // the GPU thread needs to pick up work.
    auto* draw_prep = rasterizer ? &rasterizer->GetDrawPreparation() : nullptr;
    auto prep_submission = draw_prep ? draw_prep->Build(dcb) : nullptr;
    {
        // Numbering, enqueueing and queueing under one lock keep the three orders identical.
        std::scoped_lock lock{queue.m_access};
        u64 seq = NoSeq;
        if (draw_prep) {
            seq = gfx_submit_seq++;
            draw_prep->Enqueue(seq, std::move(prep_submission));
        }
        auto task = ProcessGraphics(dcb, ccb, seq, std::move(command_copy));
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    work_retired = false;
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    if (BbStats::enabled && gnm_vqid < asc_submit_ns.size()) {
        const u64 now = NowNs();
        BbStats::asc_submits.fetch_add(1, std::memory_order_relaxed);
        // The guest's side of the round trip: from this queue's last fence to this submission.
        if (const u64 fence = asc_fence_ns[gnm_vqid].exchange(0); fence && now > fence) {
            BbStats::asc_guest_gap_ns.fetch_add(now - fence, std::memory_order_relaxed);
            BbStats::asc_guest_gaps.fetch_add(1, std::memory_order_relaxed);
        }
        asc_submit_ns[gnm_vqid].store(now, std::memory_order_relaxed);
    }
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    asc_pending.fetch_add(1, std::memory_order_relaxed);
    ++num_submits;
    work_retired = false;
    submit_cv.notify_one();
}

} // namespace AmdGpu
