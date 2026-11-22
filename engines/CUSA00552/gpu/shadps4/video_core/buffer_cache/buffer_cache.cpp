// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "bbport_copy.h"
#include "bbport_process_memory.h"
#include "bbport_toggles.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

// bbport (BB_GPU_SHARED=1, integrated GPUs): guest memory the GPU uses itself. The memory backend
// (tlou194_uwp_memory.c) maps the guest's GPU direct memory as host-cached device memory at the
// guest address; bindings inside it use a buffer over it instead of a dense arena copy: no arena
// memory, no uploads, readbacks only wait.
extern "C" void runtime_memory_set_gpu_shared(int (*alloc)(uintptr_t base, uint64_t size));
extern "C" uint64_t runtime_gpu_shared_bytes;
extern "C" uint64_t runtime_memory_make_room(uint64_t bytes); // tlou194_uwp_memory.c
namespace {
constexpr VAddr SharedBase = 0x1000000000ull, SharedEnd = 0xfc00000000ull;
constexpr u64 SharedShift = 26, SharedGranuleShift = 16;
struct SharedChunk {
    std::unique_ptr<Buffer> buffer;
    /// Per 64 KiB: the scheduler tick of the latest binding, and whether the CPU wrote it since.
    std::unique_ptr<std::atomic<u64>[]> last_use;
    std::unique_ptr<std::atomic<bool>[]> dynamic;
};
struct SharedChunks {
    const Vulkan::Instance* instance{};
    u32 memory_type{};
    /// The shared memory holding each 64 MiB of the guest range (one entry per slot it touches).
    std::array<std::atomic<SharedChunk*>, ((SharedEnd - SharedBase) >> SharedShift)> chunks{};
    std::atomic<u64> straddles{}, dynamic_shared{}, dynamic_copies{}, write_waits{};
};
SharedChunks shared_chunks;
SharedChunk* SharedChunkAt(VAddr addr) {
    if (addr < SharedBase || addr >= SharedEnd) {
        return nullptr;
    }
    return shared_chunks.chunks[(addr - SharedBase) >> SharedShift].load(std::memory_order_acquire);
}
/// Calls func(address, size) for the parts of [addr, addr+size) inside GPU-shared memory.
template <typename F>
void ForEachSharedPart(VAddr addr, u64 size, F&& func) {
    for (VAddr at = addr, end = addr + size; at < end;) {
        const VAddr slot_end = std::min<VAddr>(((at >> SharedShift) + 1) << SharedShift, end);
        const auto* chunk = SharedChunkAt(at);
        if (chunk) {
            const VAddr part_end = std::min<VAddr>(slot_end, chunk->buffer->cpu_addr + chunk->buffer->SizeBytes());
            if (part_end > at) {
                func(at, part_end - at);
            }
        }
        at = slot_end;
    }
}
bool IsSharedMemory(VAddr addr, vk::DeviceMemory memory) {
    const auto* chunk = SharedChunkAt(addr);
    return chunk && chunk->buffer->shared_memory == memory;
}
// Called by the memory backend with its table lock held: device memory mapped exactly at `base`
// (1), or nothing (0).
int AllocateSharedChunk(uintptr_t base, uint64_t size) {
    const auto device = shared_chunks.instance->GetDevice();
    const auto [buffer_result, buffer] = device.createBuffer({
        .size = size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (buffer_result != vk::Result::eSuccess) {
        std::printf("GPU: shared memory %#llx+%llu MiB: buffer %s (limit %llu MiB)\n",
                    static_cast<unsigned long long>(base), static_cast<unsigned long long>(size >> 20),
                    vk::to_string(buffer_result).c_str(),
                    static_cast<unsigned long long>(shared_chunks.instance->GetMaxBufferSize() >> 20));
        return 0;
    }
    const auto reqs = device.getBufferMemoryRequirements(buffer);
    const vk::MemoryAllocateFlagsInfo flags = {.flags = vk::MemoryAllocateFlagBits::eDeviceAddress};
    vk::DeviceMemory memory{};
    void* mapped = nullptr;
    vk::Result alloc_result = vk::Result::eErrorFeatureNotPresent, map_result = alloc_result;
    if (((reqs.memoryTypeBits >> shared_chunks.memory_type) & 1) && reqs.size <= size) {
        std::tie(alloc_result, memory) = device.allocateMemory({
            .pNext = &flags,
            .allocationSize = size,
            .memoryTypeIndex = shared_chunks.memory_type,
        });
        if (alloc_result != vk::Result::eSuccess) {
            memory = vk::DeviceMemory{};
        } else {
            std::tie(map_result, mapped) = device.mapMemory(memory, 0, size);
            if (map_result != vk::Result::eSuccess) {
                mapped = nullptr;
            }
        }
    }
    if (mapped != reinterpret_cast<void*>(base) ||
        device.bindBufferMemory(buffer, memory, 0) != vk::Result::eSuccess) {
        if (mapped) {
            device.unmapMemory(memory);
        }
        if (memory) {
            device.freeMemory(memory);
        }
        device.destroyBuffer(buffer);
        std::printf("GPU: shared memory %#llx+%llu MiB landed at %p (type %u, buffer types %#x, "
                    "needs %llu MiB, allocation %s, map %s)\n",
                    static_cast<unsigned long long>(base), static_cast<unsigned long long>(size >> 20),
                    mapped, shared_chunks.memory_type, reqs.memoryTypeBits,
                    static_cast<unsigned long long>(reqs.size >> 20),
                    vk::to_string(alloc_result).c_str(), vk::to_string(map_result).c_str());
        return 0;
    }
    auto* chunk = new SharedChunk;
    const u64 granules = (size + (u64{1} << SharedGranuleShift) - 1) >> SharedGranuleShift;
    chunk->last_use.reset(new std::atomic<u64>[granules]());
    chunk->dynamic.reset(new std::atomic<bool>[granules]());
    chunk->buffer = std::make_unique<Buffer>(*shared_chunks.instance, base, size, buffer, memory);
    for (u64 slot = (base - SharedBase) >> SharedShift; slot <= (base + size - 1 - SharedBase) >> SharedShift;
         ++slot) {
        shared_chunks.chunks[slot].store(chunk, std::memory_order_release);
    }
    return 1;
}
// bbport (BB_GPU_SHARED_TEST=1): at start, the shared allocation by memory type, size, device
// address flag and address fence, each freed again: which of them D3D12 refuses, and where the
// mapping lands (the fence leaves one free hole of the size; "hit": the mapping is there).
extern "C" size_t runtime_memory_fence(uint64_t size, uintptr_t* hole, void** fences, size_t capacity);
extern "C" void runtime_memory_unfence(void** fences, size_t count);
void SharedAllocationSelfTest(const Vulkan::Instance& instance) {
    const auto device = instance.GetDevice();
    const auto& props = instance.GetMemoryProperties();
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        std::printf("GPU shared test: memory type %u flags %#x heap %u\n", i,
                    static_cast<u32>(props.memoryTypes[i].propertyFlags),
                    props.memoryTypes[i].heapIndex);
    }
    for (u32 i = 0; i < props.memoryHeapCount; ++i) {
        std::printf("GPU shared test: heap %u %llu MiB flags %#x\n", i,
                    static_cast<unsigned long long>(props.memoryHeaps[i].size >> 20),
                    static_cast<u32>(props.memoryHeaps[i].flags));
    }
    struct Case {
        u32 type;
        u64 mib;
        bool bda, fence;
    };
    static constexpr Case cases[] = {
        {2, 64, true, false},  {2, 64, false, false}, {2, 64, true, true},   {2, 64, false, true},
        {2, 256, true, false}, {2, 256, true, true},  {2, 512, true, false}, {2, 1024, true, false},
        {2, 2588, true, false}, {0, 64, true, false}, {1, 64, true, false},  {0, 64, true, true},
    };
    for (const auto& c : cases) {
        if (c.type >= props.memoryTypeCount) {
            continue;
        }
        const u64 size = c.mib << 20;
        static void* fences[1024];
        uintptr_t hole = 0;
        const size_t fenced = c.fence ? runtime_memory_fence(size, &hole, fences, 1024) : 0;
        const u64 before = BbProcessMemory::Get().private_bytes;
        const vk::MemoryAllocateFlagsInfo flags = {.flags = vk::MemoryAllocateFlagBits::eDeviceAddress};
        const auto [alloc_result, memory] = device.allocateMemory({
            .pNext = c.bda ? &flags : nullptr,
            .allocationSize = size,
            .memoryTypeIndex = c.type,
        });
        vk::Result map_result = vk::Result::eErrorFeatureNotPresent;
        void* mapped = nullptr;
        if (alloc_result == vk::Result::eSuccess) {
            std::tie(map_result, mapped) = device.mapMemory(memory, 0, size);
        }
        const u64 after = BbProcessMemory::Get().private_bytes;
        runtime_memory_unfence(fences, fenced);
        std::printf("GPU shared test: type %u %llu MiB bda=%d fence=%d (%zu ranges): allocation %s, "
                    "map %s at %p, hole %#llx%s, private +%lld MiB\n",
                    c.type, static_cast<unsigned long long>(c.mib), c.bda, c.fence, fenced,
                    vk::to_string(alloc_result).c_str(), vk::to_string(map_result).c_str(), mapped,
                    static_cast<unsigned long long>(hole),
                    c.fence ? (mapped == reinterpret_cast<void*>(hole) ? " hit" : " miss") : "",
                    static_cast<long long>(after - before) >> 20);
        std::fflush(stdout);
        if (map_result == vk::Result::eSuccess) {
            device.unmapMemory(memory);
        }
        if (alloc_result == vk::Result::eSuccess) {
            device.freeMemory(memory);
        }
    }
}
} // namespace

u64 BufferCacheSharedStats(u64* straddles, u64* dynamic_shared, u64* dynamic_copies,
                           u64* write_waits) {
    *straddles = shared_chunks.straddles.load(std::memory_order_relaxed);
    *dynamic_shared = shared_chunks.dynamic_shared.load(std::memory_order_relaxed);
    *dynamic_copies = shared_chunks.dynamic_copies.load(std::memory_order_relaxed);
    *write_waits = shared_chunks.write_waits.load(std::memory_order_relaxed);
    return __atomic_load_n(&runtime_gpu_shared_bytes, __ATOMIC_RELAXED);
}

const Buffer* BufferCache::SharedArena(VAddr addr, u64 size) {
    auto* chunk = SharedChunkAt(addr);
    if (chunk && chunk->buffer->IsInBounds(addr, size)) {
        return chunk->buffer.get();
    }
    if (chunk || SharedChunkAt(addr + size - 1)) {
        // Partly in a shared chunk: a dense arena, which the chunk's own bindings do not see.
        if (shared_chunks.straddles.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::printf("GPU: binding %#llx+%#llx leaves a shared chunk\n",
                        static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size));
        }
    }
    return nullptr;
}

bool BufferCache::IsSharedDynamic(VAddr addr, u64 size) const {
    auto* chunk = SharedChunkAt(addr);
    if (!chunk || !chunk->buffer->IsInBounds(addr, size)) {
        return false;
    }
    const u64 base = chunk->buffer->cpu_addr;
    for (u64 g = (addr - base) >> SharedGranuleShift; g <= (addr + size - 1 - base) >> SharedGranuleShift; ++g) {
        if (chunk->dynamic[g].load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void BufferCache::NoteSharedUse(VAddr addr, u64 size) {
    auto* chunk = SharedChunkAt(addr);
    const u64 base = chunk->buffer->cpu_addr, tick = scheduler.CurrentTick();
    for (u64 g = (addr - base) >> SharedGranuleShift; g <= (addr + size - 1 - base) >> SharedGranuleShift; ++g) {
        chunk->last_use[g].store(tick, std::memory_order_release);
    }
}

// A CPU write (the tracker's fault, before the store) to memory a shared binding used waits for
// those GPU commands, the first time only; later read-only bindings there take copies.
void BufferCache::NoteSharedWrite(VAddr addr, u64 size, bool assume_locks) {
    auto* chunk = SharedChunkAt(addr);
    if (!chunk || size == 0) {
        return;
    }
    const u64 base = chunk->buffer->cpu_addr;
    const u64 last = std::min<u64>(addr + size - 1 - base, chunk->buffer->SizeBytes() - 1);
    u64 tick = 0;
    for (u64 g = (addr - base) >> SharedGranuleShift; g <= last >> SharedGranuleShift; ++g) {
        const u64 used = chunk->last_use[g].load(std::memory_order_acquire);
        if (used && !chunk->dynamic[g].exchange(true, std::memory_order_acq_rel)) {
            tick = std::max(tick, used);
        }
    }
    if (tick == 0) {
        return;
    }
    shared_chunks.write_waits.fetch_add(1, std::memory_order_relaxed);
    const auto wait = [this, tick] {
        if (!scheduler.IsFree(tick)) {
            scheduler.Wait(tick);
        }
    };
    if (assume_locks) {
        wait();
    } else {
        liverpool->SendCommand<true>(wait);
    }
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>((std::printf("GPU194 init: memory tracker\n"),tracker))},
      stream_buffer{(std::printf("GPU194 init: stream buffer\n"),instance), scheduler, MemoryType::Stream, instance.SupportsSparseBuffers() ? STREAM_BUFFER_SIZE : 32_MB},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      null_buffer{instance, 0, 64_KB, MemoryType::DeviceLocal, "Null Buffer Binding"},
      memory_semaphore{instance} {
    memory_semaphore.SetName("sparse binds");
    if ((readback_queue = instance.GetReadbackQueue())) {
        const auto device = instance.GetDevice();
        readback_pool = Vulkan::Check(device.createCommandPoolUnique({
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
        }));
        readback_cmdbuf = Vulkan::Check(device.allocateCommandBuffers({
            .commandPool = *readback_pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        }))[0];
        readback_fence = Vulkan::Check(device.createFenceUnique({}));
    }
    const char* mode=std::getenv("BB_BUFFER_ARENAS");
    sparse_buffers=instance.SupportsSparseBuffers() && !(mode && std::string_view(mode)=="dense");
    if(!sparse_buffers) arena_page_bits=MIN_ARENA_PAGE_BITS;
    std::printf("GPU194 allocator: %s buffers; conventional path uses legacy memory-requirement queries\n",sparse_buffers?"sparse":"conventional");
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            sparse_buffers ? vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency : vk::BufferCreateFlags{},
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto device=instance.GetDevice();
    auto probe=Vulkan::Check<"create arena alignment probe">(device.createBufferUnique(probe_ci));
    const auto reqs=device.getBufferMemoryRequirements(*probe);
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    arena_page_bits=std::max<u64>(arena_page_bits,block_shift);
    arena_page_size=u64{1}<<arena_page_bits;
    num_arena_pages=u64{1}<<(ADDRESS_SPACE_BITS-arena_page_bits);
    address_space.resize(num_arena_pages);
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    arena_memory_type_bits = reqs.memoryTypeBits;
    if (!sparse_buffers && instance.IsIntegrated()) {
        constexpr auto shared_flags =
            vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCached | vk::MemoryPropertyFlagBits::eHostCoherent;
        if (const auto type = FindMemoryType(instance.GetMemoryProperties(), shared_flags,
                                             reqs.memoryTypeBits)) {
            shared_chunks.instance = &instance;
            shared_chunks.memory_type = *type;
            if (const char* test = std::getenv("BB_GPU_SHARED_TEST"); test && test[0] == '1') {
                SharedAllocationSelfTest(instance);
            }
            runtime_memory_set_gpu_shared(&AllocateSharedChunk);
        }
    }
    // bbport: discrete GPUs keep a reserve of VRAM for the guest memory backing (EnsureResident).
    // It only grows as areas load and, on NVIDIA, cannot be outside VRAM (sparse buffers accept
    // VRAM types only): on 4 GB cards textures filled VRAM and the next area stopped the game
    // with ErrorOutOfDeviceMemory. Textures keep their own placement (a reserve made by moving
    // them to system memory cost FPS after every area load in 1.9).
    // bbport: only where the arena has no memory type outside VRAM. AMD and Intel accept system
    // memory for it (EnsureResident falls back to that first): there the reserve held ~480 MiB
    // of a 4 GB card for good and was never used (RX 6500 XT: "reserve 0 of 478 MiB used").
    const auto& arena_props = instance.GetMemoryProperties();
    const u32 arena_heap = arena_props.memoryTypes[arena_memory_type_index].heapIndex;
    bool system_fallback = false;
    for (u32 i = 0; i < arena_props.memoryTypeCount; ++i) {
        system_fallback |= ((arena_memory_type_bits >> i) & 1) != 0 &&
                           arena_props.memoryTypes[i].heapIndex != arena_heap;
    }
    if (sparse_buffers && !instance.IsIntegrated() && !system_fallback) {
        const auto& props = instance.GetMemoryProperties();
        const u64 heap = props.memoryHeaps[props.memoryTypes[arena_memory_type_index].heapIndex].size;
        const u64 size = Common::AlignDown(std::min<u64>(512_MB, heap / 8), block_size);
        const vk::MemoryAllocateFlagsInfo reserve_flags = {
            .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
        };
        const auto [result, memory] = device.allocateMemory({
            .pNext = &reserve_flags,
            .allocationSize = size,
            .memoryTypeIndex = arena_memory_type_index,
        });
        if (result == vk::Result::eSuccess) {
            reserve_memory = memory;
            reserve_size = size;
        }
    }

    fault_manager=std::make_unique<FaultManager>(instance,scheduler,*this,block_shift,blocks_per_arena_page*num_arena_pages);
    if(sparse_buffers) (void)GetBdaPageTableBuffer();
    runtime.FillBuffer(&null_buffer,0,null_buffer.SizeBytes(),0);
    runtime.FillBuffer(fault_manager->GetFaultBuffer(),0,fault_manager->GetFaultBuffer()->SizeBytes(),0);
}

BufferCache::~BufferCache() = default;

Buffer* BufferCache::GetBdaPageTableBuffer() {
    if (bda_pagetable_buffer) return bda_pagetable_buffer.get();
    const u64 bytes = (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bytes, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0, bytes, 0);
    // Compact dense mode can have populated/migrated/reclaimed arenas before
    // the first shader needs the table. Reconstruct only current cached ranges.
    if (!sparse_buffers) {
        for (const auto& arena : arenas) {
            if (address_space.Get(arena.cpu_addr >> arena_page_bits) != &arena) continue;
            boost::container::small_vector<Interval, 16> ranges;
            resident_ranges.ForEachInRange(arena.cpu_addr >> block_shift,
                (arena.cpu_addr + arena.SizeBytes()) >> block_shift,
                [&](const Backing& range) {
                    ranges.push_back({std::max(range.start, arena.cpu_addr >> block_shift),
                        std::min(range.end, (arena.cpu_addr + arena.SizeBytes()) >> block_shift)});
                });
            UpdateDensePageTable(&arena, ranges);
        }
        const SharedChunk* previous = nullptr;
        for (const auto& slot : shared_chunks.chunks) {
            const auto* chunk = slot.load(std::memory_order_acquire);
            if (!chunk || chunk == previous) continue;
            previous = chunk;
            const Buffer& arena = *chunk->buffer;
            boost::container::small_vector<Interval, 16> ranges;
            resident_ranges.ForEachInRange(arena.cpu_addr >> block_shift,
                (arena.cpu_addr + arena.SizeBytes()) >> block_shift,
                [&](const Backing& range) {
                    if (range.memory != arena.shared_memory) return;
                    ranges.push_back({std::max(range.start, arena.cpu_addr >> block_shift),
                        std::min(range.end, (arena.cpu_addr + arena.SizeBytes()) >> block_shift)});
                });
            UpdateDensePageTable(&arena, ranges);
        }
    }
    std::printf("Memory: activated BDA page table, %llu MiB\n", (unsigned long long)(bytes >> 20));
    return bda_pagetable_buffer.get();
}



void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    NoteSharedWrite(device_addr, size, assume_locks);
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// bbport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in Hunter's Nightmare, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const BbCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    // TLOU debug (BB_COPY_TIMING=1): reading guest memory vs flushing the arena, every 5 s. After
    // the move into Boston each copy cost ~4x what it does after a fresh load (2026-11-01).
    static const bool timing = [] {
        const char* env = std::getenv("BB_COPY_TIMING");
        return env && env[0] == '1';
    }();
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    if (!timing) {
        cache->memory->CopySparseMemory(item.source, dst, item.size);
        buffer->Flush(dst - buffer->mapped_data.data(), item.size);
        return;
    }
    static std::atomic<u64> copies{0}, bytes{0}, copy_ns{0}, flush_ns{0}, noncoherent{0},
        slow{0}, printed_at{0}, sampled{0}, not_resident{0}, sampled_ns{0}, not_resident_ns{0};
    using Clock = std::chrono::steady_clock;
    // One copy in 64: is its first source page in the working set? (then timed apart)
    const bool sample = (copies.load(std::memory_order_relaxed) & 63) == 0;
    const bool resident = !sample || BbProcessMemory::IsResident(reinterpret_cast<const void*>(item.source));
    // And its destination (mapped GPU memory: the video memory manager may have taken it back).
    static std::atomic<u64> dst_not_resident{0}, dst_not_resident_ns{0};
    const bool dst_resident = !sample || BbProcessMemory::IsResident(dst);
    const auto t0 = Clock::now();
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    const auto t1 = Clock::now();
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
    const auto t2 = Clock::now();
    const u64 c = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (sample) {
        sampled.fetch_add(1, std::memory_order_relaxed);
        sampled_ns.fetch_add(c, std::memory_order_relaxed);
        if (!resident) {
            not_resident.fetch_add(1, std::memory_order_relaxed);
            not_resident_ns.fetch_add(c, std::memory_order_relaxed);
        }
        if (!dst_resident) {
            dst_not_resident.fetch_add(1, std::memory_order_relaxed);
            dst_not_resident_ns.fetch_add(c, std::memory_order_relaxed);
        }
    }
    copies.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(item.size, std::memory_order_relaxed);
    copy_ns.fetch_add(c, std::memory_order_relaxed);
    flush_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count(),
                       std::memory_order_relaxed);
    noncoherent.fetch_add(buffer->is_coherent ? 0 : 1, std::memory_order_relaxed);
    slow.fetch_add(c > 50'000 ? 1 : 0, std::memory_order_relaxed);
    const u64 now_s = std::chrono::duration_cast<std::chrono::seconds>(t2.time_since_epoch()).count();
    // Per copying thread, every 5 s: of the wall time, how much it ran in user and kernel mode.
    {
        thread_local u64 thread_printed = 0;
        thread_local Clock::time_point wall_at{};
        thread_local BbProcessMemory::ThreadTimes times_at{};
        if (thread_printed == 0) {
            thread_printed = now_s;
            wall_at = t2;
            times_at = BbProcessMemory::CurrentThreadTimes();
        } else if (now_s >= thread_printed + 5) {
            const auto times = BbProcessMemory::CurrentThreadTimes();
            const double wall_100ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - wall_at).count() / 100.0;
            std::printf("Copy thread %lu: user %.1f%%, kernel %.1f%% of %.1f s; %.0f Mcycles/s\n",
                        static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000),
                        100.0 * (times.user - times_at.user) / wall_100ns,
                        100.0 * (times.kernel - times_at.kernel) / wall_100ns, wall_100ns / 1e7,
                        (times.cycles - times_at.cycles) / (wall_100ns / 1e7) / 1e6);
            thread_printed = now_s;
            wall_at = t2;
            times_at = times;
        }
    }
    u64 last = printed_at.load(std::memory_order_relaxed);
    if (now_s >= last + 5 && printed_at.compare_exchange_strong(last, now_s)) {
        const u64 n = std::max<u64>(copies.exchange(0), 1);
        std::printf("Guest copies in 5 s: %llu, %llu KiB; read guest %.2f us each, flush %.2f us each; "
                    "%llu to non-coherent arenas, %llu over 50 us\n",
                    (unsigned long long)n, (unsigned long long)(bytes.exchange(0) >> 10),
                    copy_ns.exchange(0) / 1e3 / n, flush_ns.exchange(0) / 1e3 / n,
                    (unsigned long long)noncoherent.exchange(0), (unsigned long long)slow.exchange(0));
        const u64 s = std::max<u64>(sampled.exchange(0), 1), nr = not_resident.exchange(0);
        const u64 s_ns = sampled_ns.exchange(0), nr_ns = not_resident_ns.exchange(0);
        std::printf("Guest copy sources: %llu sampled, %llu not in the working set (%.1f%%); "
                    "resident %.2f us each, not resident %.2f us each\n",
                    (unsigned long long)s, (unsigned long long)nr, 100.0 * nr / s,
                    (s_ns - nr_ns) / 1e3 / std::max<u64>(s - nr, 1), nr_ns / 1e3 / std::max<u64>(nr, 1));
        const u64 dnr = dst_not_resident.exchange(0), dnr_ns = dst_not_resident_ns.exchange(0);
        std::printf("Guest copy destinations: %llu of %llu sampled not in the working set (%.1f%%), "
                    "%.2f us each\n",
                    (unsigned long long)dnr, (unsigned long long)s, 100.0 * dnr / s,
                    dnr_ns / 1e3 / std::max<u64>(dnr, 1));
    }
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
// `deferrable`: a graphics stream buffer copy, needed by its draw only (BbCopy::QueueCopy).
void BufferCache::SmallGuestCopy(const BbCopy::Item& item, bool deferrable) {
    if (scheduler.IsRecordingDeferred() && !BbToggle::Disabled(BbToggle::PoolSmallCopies)) {
        scheduler.QueueHostCopy(item);
        return;
    }
    BbCopy::QueueCopy(item, deferrable);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("BB_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || BbToggle::Disabled(BbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

// bbport debug (BB_READBACK_TRACE=1): per 64 KiB block, the readbacks it caused and the GPU
// writes bound over it (size, texel or not), printed every 5 s: which memory the CPU keeps
// reading back after the GPU wrote it.
namespace {
struct ReadbackTraceBlock {
    u64 readbacks = 0, read_faults = 0, writes = 0, last_write_addr = 0;
    u64 copies = 0, copy_bytes = 0, last_read_addr = 0; // downloads that copied data from here
    u32 last_write_size = 0;
    bool texel = false;
};
std::mutex readback_trace_mutex;
std::unordered_map<u64, ReadbackTraceBlock> readback_trace;
bool ReadbackTraceEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_READBACK_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}
void ReadbackTracePrint() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    const auto now = Clock::now();
    if (now - last < std::chrono::seconds(5)) {
        return;
    }
    last = now;
    std::vector<std::pair<u64, ReadbackTraceBlock>> top(readback_trace.begin(), readback_trace.end());
    std::erase_if(top, [](const auto& e) { return e.second.readbacks == 0; });
    std::ranges::sort(top, [](const auto& a, const auto& b) { return a.second.readbacks > b.second.readbacks; });
    u64 total = 0;
    for (const auto& e : top) total += e.second.readbacks;
    std::printf("Readback trace: %llu readbacks in 5 s over %zu blocks\n", (unsigned long long)total,
                top.size());
    for (size_t i = 0; i < std::min<size_t>(top.size(), 10); ++i) {
        const auto& [block, b] = top[i];
        std::printf("  block %#llx: readbacks %llu (read faults %llu), GPU writes %llu, last write %#llx +%u%s\n",
                    (unsigned long long)(block << 16), (unsigned long long)b.readbacks,
                    (unsigned long long)b.read_faults, (unsigned long long)b.writes,
                    (unsigned long long)b.last_write_addr, b.last_write_size, b.texel ? " texel" : "");
    }
    std::vector<std::pair<u64, ReadbackTraceBlock>> copied(readback_trace.begin(), readback_trace.end());
    std::erase_if(copied, [](const auto& e) { return e.second.copies == 0; });
    std::ranges::sort(copied, [](const auto& a, const auto& b) { return a.second.copies > b.second.copies; });
    u64 copies = 0, bytes = 0;
    for (const auto& e : copied) copies += e.second.copies, bytes += e.second.copy_bytes;
    std::printf("Readback copies: %llu in 5 s, %llu KiB, over %zu blocks\n", (unsigned long long)copies,
                (unsigned long long)(bytes >> 10), copied.size());
    for (size_t i = 0; i < std::min<size_t>(copied.size(), 10); ++i) {
        const auto& [block, b] = copied[i];
        std::printf("  copied %#llx: %llu copies %llu B, read at %#llx; GPU writes %llu, last %#llx +%u%s\n",
                    (unsigned long long)(block << 16), (unsigned long long)b.copies,
                    (unsigned long long)b.copy_bytes, (unsigned long long)b.last_read_addr,
                    (unsigned long long)b.writes, (unsigned long long)b.last_write_addr,
                    b.last_write_size, b.texel ? " texel" : "");
    }
    for (auto& [block, b] : readback_trace) {
        b.readbacks = b.read_faults = b.writes = b.copies = b.copy_bytes = 0;
    }
}
thread_local VAddr readback_trace_read_addr = 0;
void ReadbackTraceCopy(VAddr addr, u64 size) {
    std::scoped_lock lk{readback_trace_mutex};
    auto& b = readback_trace[addr >> 16];
    ++b.copies;
    b.copy_bytes += size;
    b.last_read_addr = readback_trace_read_addr;
}
void ReadbackTraceRead(VAddr addr, bool is_write) {
    std::scoped_lock lk{readback_trace_mutex};
    auto& b = readback_trace[addr >> 16];
    ++b.readbacks;
    b.read_faults += !is_write;
    ReadbackTracePrint();
}
void ReadbackTraceWrite(VAddr addr, u32 size, bool texel) {
    std::scoped_lock lk{readback_trace_mutex};
    for (u64 block = addr >> 16; block <= (addr + std::max<u32>(size, 1) - 1) >> 16 && block < (addr >> 16) + 64; ++block) {
        auto& b = readback_trace[block];
        ++b.writes;
        b.last_write_addr = addr;
        b.last_write_size = size;
        b.texel = texel;
    }
}
} // namespace

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        // bbport: BB_READBACK_WINDOW_KB (power of two, default 512): the download waits for the
        // latest writer anywhere in the window (Xbox: ~1 ms per wait for a pending writer).
        static const u64 WindowSize = [] {
            const char* env = std::getenv("BB_READBACK_WINDOW_KB");
            const u64 kib = env ? std::strtoull(env, nullptr, 10) : 512;
            return std::bit_ceil(std::clamp<u64>(kib, 4, 4096)) * 1024;
        }();
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        (is_write ? BbStats::downloads_for_write : BbStats::downloads_for_read)
            .fetch_add(1, std::memory_order_relaxed);
        if (ReadbackTraceEnabled()) {
            ReadbackTraceRead(device_addr, is_write);
            readback_trace_read_addr = device_addr;
        }
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    BbStats::Timer readback_timer{BbStats::t_readback};
    BbStats::readbacks.fetch_add(1, std::memory_order_relaxed);
    if (arena->shared_memory) {
        // The GPU wrote guest memory itself: wait for the writers, nothing to copy.
        bool any = false, known = true;
        u64 writer = 0;
        memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
            gpu_modified_ranges.ForEachInRange(address, size, [&](VAddr start, VAddr end) {
                any = true;
                const u64 tick = runtime.LatestWriter(start, end - start);
                known &= tick != 0;
                writer = std::max(writer, tick);
            });
            gpu_modified_ranges.Subtract(address, size);
        });
        if (!any) {
            return;
        }
        if (!known) {
            scheduler.Finish();
        } else if (!scheduler.IsFree(writer)) {
            scheduler.Wait(writer);
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
        // Overlays there hold the data from before the GPU wrote it: they upload it again.
        for (u64 page = device_addr >> arena_page_bits; page <= (device_addr + size - 1) >> arena_page_bits;
             ++page) {
            if (address_space.Get(page)) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                break;
            }
        }
        return;
    }
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    if (ReadbackTraceEnabled()) {
        for (const auto& copy : copies) {
            ReadbackTraceCopy(arena_base + copy.srcOffset, copy.size);
        }
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    if (!FastReadback(arena, download.buffer, copies)) {
        runtime.CopyBuffer(arena, download.buffer, copies);
        scheduler.Finish();
    }

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

namespace {
// bbport: BB_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    // bbport (on unless BB_COMPUTE_ARENA=0): not for compute queue dispatches (no render pass
    // to break): the stream copies every use, the arena only pages the guest modified (TLOU's
    // skinning dispatches, ~26k/s on the Xbox, read mostly unchanged mesh data; Xbox courtyard
    // 21 -> 25.7 FPS, guest copies run by the GPU command thread 210k -> 31k/s).
    static const bool compute_arena = [] {
        const char* env = std::getenv("BB_COMPUTE_ARENA");
        return !(env && env[0] == '0');
    }();
    // BB_COMPUTE_ARENA_MIN=<bytes> (default 4096): compute queue reads smaller than this still
    // take the stream copy (read fresh at every use). Fixes one-frame skinning glitches (Sarah's
    // hair stretched about once a minute, Xbox 2026-10-30): small per-frame inputs such as bone
    // matrices may be written through a guest address the tracker does not watch, leaving the
    // arena copy stale. 0 = arena for every compute read (old behaviour).
    static const u64 compute_arena_min = [] {
        const char* env = std::getenv("BB_COMPUTE_ARENA_MIN");
        return env ? std::strtoull(env, nullptr, 10) : 4096ull;
    }();
    const bool compute_arena_read =
        compute_arena && BbCopy::in_compute_queue && size >= compute_arena_min;
    if (!is_written && size <= STREAM_THRESHOLD && !compute_arena_read &&
        !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // bbport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !BbToggle::Disabled(BbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                }, true);
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    // bbport (BB_GPU_SHARED): data the guest rewrites is read through an overlay: a dense arena
    // over the shared memory, uploaded where the CPU changed it (copies, as the guest's fences do
    // not wait for the GPU). Not while the GPU's own writes there are pending: those are in the
    // shared memory only.
    if (!is_written && IsSharedDynamic(device_addr, size) && !IsRegionGpuModified(device_addr, size)) {
        shared_chunks.dynamic_copies.fetch_add(1, std::memory_order_relaxed);
        const auto* overlay = GetDenseArena(first_block, last_block, true);
        SynchronizeMemory(overlay, device_addr, size, false, is_texel_buffer);
        return {overlay, overlay->Offset(device_addr)};
    }
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    BbStats::BindLaps laps{BbStats::enabled && BbCopy::in_compute_queue};
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    laps.Mark(8);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        laps.last = std::chrono::steady_clock::now();
        gpu_modified_ranges.Add(device_addr, size);
        laps.Mark(9);
        if (ReadbackTraceEnabled()) {
            ReadbackTraceWrite(device_addr, size, is_texel_buffer);
        }
        hot_write_flush |= IsHotReadback(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        // bbport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const BbCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                BbCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

// bbport (BB_HOT_READBACK_FLUSH=1): ~85% of the readbacks that wait for their writer (~120/s on
// the Xbox, ~1 ms each) find it not even submitted. The 64 KiB blocks they read are
// remembered (4 KiB; 64 KiB matched most draws); a draw or dispatch writing one is submitted
// right after it (TakeHotWriteFlush), at most every 32 draws/dispatches.
void BufferCache::NoteHotReadback(VAddr address, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_HOT_READBACK_FLUSH");
        return env && env[0] == '1';
    }();
    if (!enabled || size == 0) {
        return;
    }
    static const size_t capacity = [] {
        const char* env = std::getenv("BB_HOT_READBACK_BLOCKS");
        return std::clamp<size_t>(env ? std::strtoull(env, nullptr, 10) : 1024, 32, 65536);
    }();
    const u64 first = address >> 12, last = (address + size - 1) >> 12;
    for (u64 block = first; block <= last && block < first + 4; ++block) {
        if (hot_readback_set.insert(block).second) {
            hot_readback_order.push_back(block);
            ++hot_readback_next;
            if (hot_readback_order.size() > capacity) {
                hot_readback_set.erase(hot_readback_order.front());
                hot_readback_order.pop_front();
            }
        }
    }
}

bool BufferCache::IsHotReadback(VAddr address, u64 size) const {
    if (hot_readback_next == 0 || size == 0) {
        return false;
    }
    const u64 first = address >> 12, last = (address + size - 1) >> 12;
    if (last - first < hot_readback_set.size()) {
        for (u64 block = first; block <= last; ++block) {
            if (hot_readback_set.contains(block)) {
                return true;
            }
        }
        return false;
    }
    for (const u64 block : hot_readback_set) {
        if (block >= first && block <= last) {
            return true;
        }
    }
    return false;
}

bool BufferCache::FastReadback(const Buffer* arena, const Buffer* staging,
                               std::span<const vk::BufferCopy> copies) {
    if (!readback_queue) {
        return false;
    }
    // In the courtyard the writer of ~78% of the readbacks had completed: Finish waited ~1 ms
    // for the recorder and for unrelated work queued since. Then only the copy is waited for.
    u64 writer = 0;
    for (const auto& copy : copies) {
        const u64 tick = runtime.LatestWriter(arena->cpu_addr + copy.srcOffset, copy.size);
        if (tick == 0) {
            BbStats::fast_readback_unknown.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        writer = std::max(writer, tick);
    }
    const bool mapped = !arena->mapped_data.empty() && !staging->mapped_data.empty();
    if (writer >= scheduler.CurrentTick() || !scheduler.IsFree(writer)) {
        BbStats::fast_readback_pending.fetch_add(1, std::memory_order_relaxed);
        if (!mapped) {
            return false;
        }
        // bbport: mapped arena: wait for the writer only (submitting it when still recorded),
        // not for everything recorded since plus a GPU copy (Scheduler::Finish).
        if (writer >= scheduler.CurrentTick()) {
            BbStats::readback_unsubmitted.fetch_add(1, std::memory_order_relaxed);
            for (const auto& copy : copies) {
                NoteHotReadback(arena->cpu_addr + copy.srcOffset, copy.size);
            }
        }
        scheduler.Wait(writer);
    }
    const auto start = std::chrono::steady_clock::now();
    // bbport (BB_MAPPED_ARENAS): the writer has completed; read the arena directly.
    if (mapped) {
        for (const auto& copy : copies) {
            const_cast<Buffer*>(arena)->Invalidate(copy.srcOffset, copy.size);
            std::memcpy(staging->mapped_data.data() + copy.dstOffset,
                        arena->mapped_data.data() + copy.srcOffset, copy.size);
        }
        BbStats::fast_readbacks.fetch_add(1, std::memory_order_relaxed);
        BbStats::fast_readback_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count(),
            std::memory_order_relaxed);
        return true;
    }
    std::scoped_lock lk{readback_mutex};
    const auto device = instance.GetDevice();
    Vulkan::Check(readback_cmdbuf.begin({
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    readback_cmdbuf.copyBuffer(arena->Handle(), staging->Handle(),
                               static_cast<u32>(copies.size()), copies.data());
    const vk::MemoryBarrier2 to_host = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    readback_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1u,
        .pMemoryBarriers = &to_host,
    });
    Vulkan::Check(readback_cmdbuf.end());
    // Waiting for the writer's (signalled) timeline value makes its writes visible to the copy.
    const vk::Semaphore timeline = scheduler.GetWorkSemaphore()->Handle();
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &writer,
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &timeline,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &readback_cmdbuf,
    };
    Vulkan::Check(readback_queue.submit(submit_info, *readback_fence));
    Vulkan::Check(device.waitForFences(*readback_fence, true, UINT64_MAX));
    Vulkan::Check(device.resetFences(*readback_fence));
    BbStats::fast_readbacks.fetch_add(1, std::memory_order_relaxed);
    BbStats::fast_readback_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count(),
        std::memory_order_relaxed);
    return true;
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        const Buffer* arena = SharedArena(device_addr, size);
        SynchronizeMemory(arena ? arena : address_space.Get(page), device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    if(!sparse_buffers) return GetDenseArena(first_block,last_block);
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space.Get(first_page);
    const auto* last_arena = address_space.Get(last_page);
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space.Set(first_page, new_arena);
            address_space.Set(last_page, new_arena);
        }
        return address_space.Get(first_page);
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space.Set(base_page + page, new_arena);
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space.Set(base_page + page, new_arena);
    }
    return new_arena;
}

void BufferCache::UpdateDensePageTable(const Buffer* arena, std::span<const Interval> ranges) {
    u64 count = 0;
    for (const auto& range : ranges) {
        count += range.end - range.start;
    }
    if (!count) {
        return;
    }
    VmaAllocationInfo allocation{};
    if (arena->shared_memory) {
        allocation.deviceMemory = arena->shared_memory;
    } else {
        vmaGetAllocationInfo(instance.GetAllocator(), arena->buffer.allocation, &allocation);
    }
    for (const auto& range : ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = allocation.deviceMemory;
        backing.offset = allocation.offset + (range.start << block_shift) - arena->cpu_addr;
        resident_ranges.Add(backing);
    }
    if (!bda_pagetable_buffer) return;
    const auto staging = staging_pool.Request(count * sizeof(vk::DeviceAddress), MemoryType::HostUncached);
    auto* addresses = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    u64 offset = staging.offset;
    for (const auto& range : ranges) {
        for (u64 block = range.start; block < range.end; ++block) {
            *addresses++ = arena->BufferDeviceAddress() + (block << block_shift) - arena->cpu_addr;
        }
        const u64 bytes = (range.end - range.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, range.start * sizeof(vk::DeviceAddress), bytes);
        offset += bytes;
    }
    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}


const Buffer* BufferCache::GetDenseArena(u64 first_block,u64 last_block,bool overlay) {
    u64 first_page=first_block>>blocks_per_arena_page_shift;
    u64 last_page=last_block>>blocks_per_arena_page_shift;
    ASSERT(first_block<=last_block && last_page<num_arena_pages);
    if (!overlay) {
        if (const auto* shared = SharedArena(first_block << block_shift,
                                             (last_block - first_block + 1) << block_shift)) {
            return shared;
        }
    }
    if(const auto* current=address_space.Get(first_page);
       current && current->IsInBounds(first_block<<block_shift,(last_block-first_block+1)<<block_shift)) {
        current->last_use_epoch = use_epoch;
        return current;
    }
    const u64 asked_first = first_page, asked_last = last_page;
    std::vector<const Buffer*> previous;
    bool expanded;
    do {
        expanded=false;
        for(u64 page=first_page;page<=last_page;++page) {
            const auto* old=address_space.Get(page);if(!old)continue;
            if(std::find(previous.begin(),previous.end(),old)==previous.end())previous.push_back(old);
            const u64 begin=old->cpu_addr>>arena_page_bits;
            const u64 end=(old->cpu_addr+old->SizeBytes()-1)>>arena_page_bits;
            if(begin<first_page || end>last_page){first_page=std::min(first_page,begin);last_page=std::max(last_page,end);expanded=true;}
        }
    } while(expanded);
    const VAddr base=first_page<<arena_page_bits;
    const u64 size=(last_page-first_page+1)<<arena_page_bits;
    // bbport (integrated GPUs, unless BB_MAPPED_ARENAS=0): arenas in host-visible device memory
    // (the same RAM there), so FastReadback copies a completed writer's data on the CPU instead
    // of a GPU copy and its fence (~117 us each, ~660/s, 7.7% of the Xbox GPU command thread;
    // Xbox courtyard ~25.8 -> ~27.9 FPS).
    static const bool mapped_arenas = [&] {
        const char* env = std::getenv("BB_MAPPED_ARENAS");
        return !(env && env[0] == '0') &&
               instance.GetPhysicalDevice().getProperties().deviceType ==
                   vk::PhysicalDeviceType::eIntegratedGpu;
    }();
    // XStation (2026-11-19, Series S player logs): Dozen allows buffers of a quarter of the GPU
    // memory, 1024 MiB on a Series X but much less on a Series S (1280 MiB GPU memory), where
    // joining the arenas around a binding went over it and stopped TLOU about 20 s in ("Conventional
    // arena exceeds driver buffer size"). Over the limit, the arenas there are cut instead of joined
    // (SplitDenseArena). BB_BUFFER_LIMIT_MIB sets the limit (to test a Series S's on a Series X);
    // BB_ARENA_SPLIT=0 stops as before.
    static const u64 arena_limit = [&] {
        const char* env = std::getenv("BB_BUFFER_LIMIT_MIB");
        const u64 mib = env ? std::strtoull(env, nullptr, 10) : 0;
        const u64 limit = mib ? mib << 20 : instance.GetMaxBufferSize();
        std::printf("Buffer arenas: limit %llu MiB (driver %llu MiB)\n",
                    static_cast<unsigned long long>(limit >> 20),
                    static_cast<unsigned long long>(instance.GetMaxBufferSize() >> 20));
        return limit;
    }();
    static const bool split_arenas = [] {
        const char* env = std::getenv("BB_ARENA_SPLIT");
        return !(env && env[0] == '0');
    }();
    if (size > arena_limit && split_arenas) {
        return SplitDenseArena(asked_first, asked_last, size, arena_limit, mapped_arenas);
    }
    ASSERT_MSG(size<=arena_limit,"Conventional arena exceeds driver buffer size");
    BbStats::Timer create_timer{BbStats::t_arena_create}; // to the end: creation, copies, tables
    auto* arena=&arenas.emplace_back(instance,base,size,
                                     mapped_arenas ? MemoryType::Stream : MemoryType::DeviceLocal);
    boost::container::small_vector<Interval,16> preserved;
    for(const auto* old:previous) {
        boost::container::small_vector<vk::BufferCopy,8> copies;
        resident_ranges.ForEachInRange(old->cpu_addr>>block_shift,(old->cpu_addr+old->SizeBytes())>>block_shift,[&](const Backing& r){
            if (IsSharedMemory(r.start << block_shift, r.memory)) return; // not this arena's
            const u64 begin=std::max(r.start,old->cpu_addr>>block_shift);
            const u64 end=std::min(r.end,(old->cpu_addr+old->SizeBytes())>>block_shift);
            preserved.push_back({begin,end});
            const u64 address=begin<<block_shift;
            copies.emplace_back(address-old->cpu_addr,address-base,(end-begin)<<block_shift);
        });
        if(!copies.empty())runtime.CopyBuffer(old,arena,copies);
    }
    for(u64 page=first_page;page<=last_page;++page)address_space.Set(page,arena);
    UpdateDensePageTable(arena,preserved);
    // A recorded draw may still reference a superseded handle. Retire only
    // through the scheduler's normal completion fence, never at replacement.
    for(const auto* old:previous)scheduler.DeferOperation([old]{const_cast<Buffer*>(old)->buffer.Destroy();});
    BbStats::dense_arenas_created.fetch_add(1, std::memory_order_relaxed);
    BbStats::dense_arena_bytes.fetch_add(size, std::memory_order_relaxed);
    for (const auto* old : previous) {
        BbStats::dense_arena_bytes.fetch_sub(old->SizeBytes(), std::memory_order_relaxed);
        std::erase(dense_live, old);
    }
    arena->last_use_epoch = use_epoch;
    dense_live.push_back(arena);
    // An overlay holds no data where it covers shared memory: the tracker uploads it there.
    ForEachSharedPart(base, size, [this](VAddr at, u64 bytes) {
        memory_tracker->MarkRegionAsCpuModified(at, bytes);
    });
    return arena;
}

// XStation (2026-11-19): joining would make an arena over the driver's buffer limit (Series S). Instead
// the asked pages get an arena of their own, and each arena reaching into them is cut into the part
// outside them (one each side), so no arena grows. Every live arena owns all pages of its range, so the
// parts cover exactly the old arenas' pages; their GPU data is copied on the GPU like a join's.
const Buffer* BufferCache::SplitDenseArena(u64 first_page, u64 last_page, u64 joined, u64 limit,
                                           bool mapped) {
    BbStats::Timer create_timer{BbStats::t_arena_create};
    std::vector<const Buffer*> previous;
    for (u64 page = first_page; page <= last_page; ++page) {
        const auto* old = address_space.Get(page);
        if (old && std::find(previous.begin(), previous.end(), old) == previous.end()) {
            previous.push_back(old);
        }
    }
    struct Part {
        u64 first, last;
        Buffer* arena;
        boost::container::small_vector<Interval, 16> preserved;
    };
    std::vector<Part> parts;
    parts.push_back({first_page, last_page, nullptr, {}});
    for (const auto* old : previous) {
        const u64 begin = old->cpu_addr >> arena_page_bits;
        const u64 end = (old->cpu_addr + old->SizeBytes() - 1) >> arena_page_bits;
        if (begin < first_page) parts.push_back({begin, first_page - 1, nullptr, {}});
        if (end > last_page) parts.push_back({last_page + 1, end, nullptr, {}});
    }
    const auto type = mapped ? MemoryType::Stream : MemoryType::DeviceLocal;
    // All copies first: the page table's backings change only after every part has read the old ones.
    for (auto& part : parts) {
        const VAddr base = part.first << arena_page_bits;
        const u64 size = (part.last - part.first + 1) << arena_page_bits;
        part.arena = &arenas.emplace_back(instance, base, size, type);
        const u64 part_begin = base >> block_shift, part_end = (base + size) >> block_shift;
        for (const auto* old : previous) {
            const u64 old_begin = old->cpu_addr >> block_shift;
            const u64 old_end = (old->cpu_addr + old->SizeBytes()) >> block_shift;
            const u64 from = std::max(part_begin, old_begin), to = std::min(part_end, old_end);
            if (from >= to) continue;
            boost::container::small_vector<vk::BufferCopy, 8> copies;
            resident_ranges.ForEachInRange(from, to, [&](const Backing& r) {
                if (IsSharedMemory(r.start << block_shift, r.memory)) return; // not this arena's
                const u64 b = std::max(r.start, from), e = std::min(r.end, to);
                if (b >= e) return;
                part.preserved.push_back({b, e});
                const u64 address = b << block_shift;
                copies.emplace_back(address - old->cpu_addr, address - base, (e - b) << block_shift);
            });
            if (!copies.empty()) runtime.CopyBuffer(old, part.arena, copies);
        }
    }
    for (auto& part : parts) {
        for (u64 page = part.first; page <= part.last; ++page) address_space.Set(page, part.arena);
        UpdateDensePageTable(part.arena, part.preserved);
    }
    // A recorded draw may still reference a superseded handle: retired through the scheduler.
    for (const auto* old : previous) {
        scheduler.DeferOperation([old] { const_cast<Buffer*>(old)->buffer.Destroy(); });
        BbStats::dense_arena_bytes.fetch_sub(old->SizeBytes(), std::memory_order_relaxed);
        std::erase(dense_live, old);
    }
    u64 made = 0;
    for (auto& part : parts) {
        const u64 size = part.arena->SizeBytes();
        made += size;
        BbStats::dense_arenas_created.fetch_add(1, std::memory_order_relaxed);
        BbStats::dense_arena_bytes.fetch_add(size, std::memory_order_relaxed);
        part.arena->last_use_epoch = use_epoch;
        dense_live.push_back(part.arena);
        // An overlay holds no data where it covers shared memory: the tracker uploads it there.
        ForEachSharedPart(part.arena->cpu_addr, size, [this](VAddr at, u64 bytes) {
            memory_tracker->MarkRegionAsCpuModified(at, bytes);
        });
    }
    static u64 splits = 0;
    ++splits;
    const u64 asked = (last_page - first_page + 1) << arena_page_bits;
    if (splits <= 20 || splits % 100 == 0 || asked > limit) {
        std::printf("Buffer arenas: split %llu: joined would be %llu MiB (limit %llu MiB); asked %llu MiB, "
                    "%zu old -> %zu arenas, %llu MiB%s\n",
                    static_cast<unsigned long long>(splits), static_cast<unsigned long long>(joined >> 20),
                    static_cast<unsigned long long>(limit >> 20), static_cast<unsigned long long>(asked >> 20),
                    previous.size(), parts.size(), static_cast<unsigned long long>(made >> 20),
                    asked > limit ? " (the asked range alone is over the limit)" : "");
        std::fflush(stdout);
    }
    return parts[0].arena;
}

// bbport: the game never unmaps its memory, and every range the GPU ever used kept a dense arena
// (a GPU-side copy of that guest memory): TLOU's Xbox build grew ~8-12 MiB/min in a stationary
// scene (the end of long sessions at the 5 GiB guard). Arenas unused for BB_ARENA_IDLE_S seconds
// (default 60) are dropped, oldest first, while all of them take more than BB_ARENA_BUDGET_MB
// (default 512; 0: never).
// Not one holding GPU-written data that was not read back. Its pages are marked CPU-modified, so
// the next binding there creates a new arena and uploads the guest data again.
void BufferCache::EvictIdleArenas() {
    if (sparse_buffers) {
        return;
    }
    BbStats::Timer evict_timer{BbStats::t_arena_evict};
    static const u64 budget = [] {
        const char* env = std::getenv("BB_ARENA_BUDGET_MB");
        return (env ? std::strtoull(env, nullptr, 10) : 512ull) << 20;
    }();
    const auto now = std::chrono::steady_clock::now();
    if (budget == 0 || now - evict_time < std::chrono::seconds(2)) {
        return;
    }
    evict_time = now;
    ++use_epoch;
    u64 live = BbStats::dense_arena_bytes.load(std::memory_order_relaxed);
    if (live <= budget) {
        return;
    }
    // BB_ARENA_IDLE_S: seconds unused before an arena may go (default 60; epochs are 2 s).
    static const u64 IdleEpochs = [] {
        const char* env = std::getenv("BB_ARENA_IDLE_S");
        return std::max<u64>(1, (env ? std::strtoull(env, nullptr, 10) : 60ull) / 2);
    }();
    // bbport: near a hard limit on the app's memory (UWP on Xbox), sooner: unused for 20 s
    // (under 1 GiB left), 8 s (under 600 MiB) or 6 s (under 300 MiB); in real play the arenas
    // grew ~30-50 MiB/min with none unused for a minute, up to the limit (BbProcessMemory).
    // Tried at 20 / 6 / 2 s without the check below (2026-10-30): characters' bodies broke and
    // the game crashed on a garbage pointer near the limit. Some arenas hold data the GPU made
    // that guest memory never got and the tracker does not call GPU-modified; a new arena then
    // uploads the stale guest data. So a mapped arena goes only if it still matches guest memory
    // where the CPU has not written since (ArenaMatchesGuest).
    static constexpr u64 PressureIdleEpochs[] = {0, 10, 4, 3};
    const int level = BbProcessMemory::GetAppPressure().level;
    const u64 idle_epochs = level ? std::min(IdleEpochs, PressureIdleEpochs[level]) : IdleEpochs;
    // TLOU (Xbox, 2026-11-01): above BB_ARENA_WRITEBACK_MB of app memory (default 3900; 0 = off)
    // an idle arena the GPU wrote has its data downloaded to guest memory (at most 64 MiB a pass),
    // so a later pass can drop it like any idle arena. After the move into Boston 90-127 MiB of such
    // arenas stayed (25 after a fresh load) and kept the app at the pager's line: the pager then
    // paged out guest memory still in use, guest reads ran 3x slower (~20 FPS instead of 30).
    static const u64 writeback_mark = [] {
        const char* env = std::getenv("BB_ARENA_WRITEBACK_MB");
        return (env ? std::strtoull(env, nullptr, 10) : 3900ull) << 20;
    }();
    const auto app_pressure = BbProcessMemory::GetAppPressure();
    const bool write_back = writeback_mark != 0 && app_pressure.limit && app_pressure.usage >= writeback_mark;
    constexpr u64 WritebackBudget = 64ull << 20;
    u64 written_back = 0;
    std::vector<const Buffer*> idle;
    u64 idle_gpu = 0, largest = 0;
    for (const auto* arena : dense_live) {
        largest = std::max<u64>(largest, arena->SizeBytes());
        if (use_epoch - arena->last_use_epoch >= idle_epochs) {
            if (!IsRegionGpuModified(arena->cpu_addr, arena->SizeBytes())) {
                idle.push_back(arena);
            } else {
                idle_gpu += arena->SizeBytes();
                if (write_back && !arena->shared_memory &&
                    written_back + arena->SizeBytes() <= WritebackBudget) {
                    DownloadMemory(arena, arena->cpu_addr, arena->SizeBytes());
                    written_back += arena->SizeBytes();
                }
            }
        }
    }
    if (written_back) {
        static u64 written_total = 0;
        written_total += written_back;
        std::printf("Buffer arenas: wrote back %llu MiB of idle GPU-written arenas (%llu MiB in all), "
                    "app %llu MiB\n",
                    static_cast<unsigned long long>(written_back >> 20),
                    static_cast<unsigned long long>(written_total >> 20),
                    static_cast<unsigned long long>(app_pressure.usage >> 20));
    }
    // For the memory log: what keeps arenas over the budget.
    arena_report = {live, idle_gpu, largest, dense_live.size()};
    for (const auto* arena : idle) {
        arena_report.idle += arena->SizeBytes();
    }
    std::ranges::sort(idle, {}, &Buffer::last_use_epoch);
    u32 evicted = 0;
    // Comparing reads the arena's (uncached) mapping: at most 8 MiB a pass (~4 MB/s of evictions).
    constexpr u64 CompareBudget = 8ull << 20;
    u64 compared = 0;
    for (const auto* arena : idle) {
        if (live <= budget) {
            break;
        }
        if (!arena->mapped_data.empty()) {
            if (compared != 0 && compared + arena->SizeBytes() > CompareBudget) {
                break;
            }
            compared += arena->SizeBytes();
            if (!ArenaMatchesGuest(arena)) {
                ++arena_differed;
                arena_differed_bytes += arena->SizeBytes();
                arena->last_use_epoch = use_epoch; // kept; looked at again when idle again
                continue;
            }
        }
        live -= arena->SizeBytes();
        EvictArena(arena);
        ++evicted;
    }
    arena_report.differed = arena_differed;
    arena_report.differed_bytes = arena_differed_bytes;
    if (evicted) {
        BbStats::arenas_evicted.fetch_add(evicted, std::memory_order_relaxed);
        static u64 total = 0;
        total += evicted;
        std::printf("Buffer arenas: %u idle arenas dropped (%llu in all), %llu MiB live; unused "
                    "for %llu s, app eviction level %d\n",
                    evicted, static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(BbStats::dense_arena_bytes.load() >> 20),
                    static_cast<unsigned long long>(idle_epochs * 2), level);
        std::fflush(stdout);
    }
}

bool BufferCache::ArenaMatchesGuest(const Buffer* arena) {
    static thread_local std::vector<u8> guest;
    guest.resize(block_size);
    const u8* data = arena->mapped_data.data();
    for (u64 offset = 0; offset < arena->SizeBytes(); offset += block_size) {
        const VAddr address = arena->cpu_addr + offset;
        const u64 size = std::min<u64>(block_size, arena->SizeBytes() - offset);
        if (memory_tracker->IsRegionCpuModified(address, size)) {
            continue; // guest memory is newer there: the arena's copy is stale anyway
        }
        memory->CopySparseMemory(address, guest.data(), size);
        if (std::memcmp(guest.data(), data + offset, size) != 0) {
            return false;
        }
    }
    return true;
}

void BufferCache::ReportLargeArenas() const {
    for (const auto* arena : dense_live) {
        if (arena->SizeBytes() < (16ull << 20)) {
            continue;
        }
        std::printf("Host memory: arena guest 0x%llx, %llu MiB, host %p, unused for %llu s\n",
                    static_cast<unsigned long long>(arena->cpu_addr),
                    static_cast<unsigned long long>(arena->SizeBytes() >> 20),
                    static_cast<const void*>(arena->mapped_data.data()),
                    static_cast<unsigned long long>((use_epoch - arena->last_use_epoch) * 2));
    }
}

void BufferCache::EvictArena(const Buffer* arena) {
    const u64 first_page = arena->cpu_addr >> arena_page_bits;
    const u64 last_page = (arena->cpu_addr + arena->SizeBytes() - 1) >> arena_page_bits;
    for (u64 page = first_page; page <= last_page; ++page) {
        if (address_space.Get(page) == arena) {
            address_space.Set(page, nullptr);
        }
    }
    const u64 first_block = arena->cpu_addr >> block_shift;
    const u64 end_block = (arena->cpu_addr + arena->SizeBytes()) >> block_shift;
    // An overlay's blocks in shared memory keep the shared memory's device addresses.
    boost::container::small_vector<Interval, 4> own{{first_block, end_block}};
    ForEachSharedPart(arena->cpu_addr, arena->SizeBytes(), [&](VAddr at, u64 bytes) {
        const u64 a = at >> block_shift, b = (at + bytes) >> block_shift;
        boost::container::small_vector<Interval, 4> rest;
        for (const auto& r : own) {
            if (r.start < a) rest.push_back({r.start, std::min(r.end, a)});
            if (r.end > b) rest.push_back({std::max(r.start, b), r.end});
        }
        own = rest;
    });
    for (const auto& r : own) {
        resident_ranges.Subtract(r.start, r.end);
        if (bda_pagetable_buffer) {
            // Its blocks no longer have device addresses.
            runtime.FillBuffer(bda_pagetable_buffer.get(), r.start * sizeof(vk::DeviceAddress),
                               (r.end - r.start) * sizeof(vk::DeviceAddress), 0);
        }
    }
    resident_hint.fill({});
    // The next binding uploads the guest data into a new arena.
    memory_tracker->MarkRegionAsCpuModified(arena->cpu_addr, arena->SizeBytes());
    std::erase(dense_live, arena);
    BbStats::dense_arena_bytes.fetch_sub(arena->SizeBytes(), std::memory_order_relaxed);
    scheduler.DeferOperation([arena] { const_cast<Buffer*>(arena)->buffer.Destroy(); });
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    // bbport: blocks stay resident once they are, so a range inside one found resident recently
    // needs no interval search (EnsureResident ran for every binding: ~1.7% of the Xbox GPU
    // command thread).
    for (const auto& [start, end] : resident_hint) {
        if (first_block >= start && last_block < end) {
            return;
        }
    }
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        resident_hint[resident_hint_next++ % resident_hint.size()] = {first_block, last_block + 1};
        return;
    }
    if(!sparse_buffers) {
        boost::container::small_vector<Interval,8> ranges;
        for(const auto& range:bind_ranges) ranges.push_back(range);
        UpdateDensePageTable(arena,ranges);return;
    }
    BbStats::Timer timer{BbStats::t_resident};

    vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    // BB_ARENA_LIMIT_MB=N: VRAM counts as full once N MiB are backed here (tests the reserve).
    static const u64 arena_limit = [] {
        const char* env = std::getenv("BB_ARENA_LIMIT_MB");
        return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
    }();
    static u64 arena_allocated = 0;
    vk::Result alloc_result = vk::Result::eErrorOutOfDeviceMemory;
    vk::DeviceMemory device_memory{};
    if (!arena_limit || arena_allocated + alloc_info.allocationSize <= arena_limit) {
        std::tie(alloc_result, device_memory) = instance.GetDevice().allocateMemory(alloc_info);
        if (alloc_result == vk::Result::eSuccess) {
            arena_allocated += alloc_info.allocationSize;
        }
    }
    u64 memory_base{}; ///< the backing's offset in `device_memory` (non-zero in the reserve)
    // 2026-10-31: at the Xbox's memory limit, cold guest memory goes to the pager's file first
    // (same commit as this arena), then the allocation is tried again.
    for (int attempt = 0; attempt < 2 && alloc_result == vk::Result::eErrorOutOfDeviceMemory &&
                          (!arena_limit || arena_allocated + alloc_info.allocationSize <= arena_limit);
         ++attempt) {
        if (!runtime_memory_make_room(alloc_info.allocationSize + (u64(64) << 20))) {
            break;
        }
        std::tie(alloc_result, device_memory) = instance.GetDevice().allocateMemory(alloc_info);
        if (alloc_result == vk::Result::eSuccess) {
            arena_allocated += alloc_info.allocationSize;
        }
    }
    if (alloc_result == vk::Result::eErrorOutOfDeviceMemory) {
        // bbport: VRAM full. The guest memory the GPU uses is backed here and only grows as areas
        // load; on 4-8 GB cards it ran out ("ErrorOutOfDeviceMemory" stopped the game). Another
        // memory type the arena accepts, outside VRAM (system memory the GPU reaches), is slower
        // but keeps the game going.
        const auto& props = instance.GetMemoryProperties();
        const u32 vram_heap = props.memoryTypes[arena_memory_type_index].heapIndex;
        for (u32 i = 0; i < props.memoryTypeCount && alloc_result != vk::Result::eSuccess; ++i) {
            if (((arena_memory_type_bits >> i) & 1) == 0 || props.memoryTypes[i].heapIndex == vram_heap) {
                continue;
            }
            alloc_info.memoryTypeIndex = i;
            std::tie(alloc_result, device_memory) = instance.GetDevice().allocateMemory(alloc_info);
        }
        if (alloc_result == vk::Result::eSuccess) {
            Vulkan::NoteOverBudget("buffer");
        } else if (reserve_memory && reserve_size - reserve_used >= alloc_info.allocationSize) {
            // The reserve kept at start for this (no other memory type on NVIDIA).
            device_memory = reserve_memory;
            memory_base = reserve_used;
            const u64 quarter_before = reserve_used * 4 / reserve_size;
            reserve_used += alloc_info.allocationSize;
            alloc_result = vk::Result::eSuccess;
            if (memory_base == 0 || reserve_used * 4 / reserve_size != quarter_before) {
                std::printf("GPU: video memory full: game memory from its reserve (%llu of %llu "
                            "MiB used)\n",
                            static_cast<unsigned long long>(reserve_used >> 20),
                            static_cast<unsigned long long>(reserve_size >> 20));
                std::fflush(stdout);
            }
        } else {
            std::printf("GPU: video memory full: no room for %llu MiB of game memory (only VRAM "
                        "can hold it on this GPU). A lower output resolution or upscaler preset "
                        "needs less.\n",
                        static_cast<unsigned long long>(alloc_info.allocationSize >> 20));
            std::fflush(stdout);
        }
    }
    device_memory = Vulkan::Check(vk::ResultValue<vk::DeviceMemory>{alloc_result, device_memory});
    arena_backed += alloc_info.allocationSize;

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{memory_base};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

// The tracker's CPU changes a shared binding consumed, uploaded into the overlays over them.
void BufferCache::ForwardToOverlays(std::span<const std::pair<u64, u64>> dirty) {
    boost::container::small_vector<std::pair<const Buffer*, boost::container::small_vector<vk::BufferCopy, 4>>, 2> uploads;
    for (const auto& [addr, bytes] : dirty) {
        for (VAddr at = addr, end = addr + bytes; at < end;) {
            const VAddr page_end = std::min<VAddr>(((at >> arena_page_bits) + 1) << arena_page_bits, end);
            if (const auto* overlay = address_space.Get(at >> arena_page_bits)) {
                auto it = std::ranges::find(uploads, overlay, &decltype(uploads)::value_type::first);
                if (it == uploads.end()) {
                    uploads.emplace_back(overlay, decltype(uploads)::value_type::second_type{});
                    it = std::prev(uploads.end());
                }
                auto& copies = it->second;
                if (!copies.empty() && copies.back().dstOffset + copies.back().size == at) {
                    copies.back().size += page_end - at;
                } else {
                    copies.push_back({0, at, page_end - at});
                }
            }
            at = page_end;
        }
    }
    for (auto& [overlay, copies] : uploads) {
        u64 total = 0;
        for (auto& copy : copies) {
            copy.srcOffset = total;
            total += copy.size;
        }
        if (const auto* staging = UploadCopies(overlay, copies, total)) {
            runtime.CopyBuffer(staging, overlay, copies);
        }
    }
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    if (arena->shared_memory) {
        // Nothing to copy; the tracker still learns of later CPU writes (NoteSharedWrite). The
        // tick goes in first: a write right after the protection must find it.
        NoteSharedUse(device_addr, size);
        boost::container::small_vector<std::pair<u64, u64>, 4> dirty;
        memory_tracker->ForEachUploadRange(
            device_addr, size, is_written, [&](u64 addr, u64 bytes) { dirty.emplace_back(addr, bytes); },
            [] {});
        ForwardToOverlays(dirty);
        if (is_texel_buffer && !is_written) {
            return SynchronizeMemoryFromImage(arena, device_addr, size);
        }
        return false;
    }
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    BbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // bbport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!BbToggle::Disabled(BbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            BbCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        BbCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // bbport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !BbToggle::Disabled(BbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // bbport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if(!sparse_buffers) { ASSERT(pending_binds.empty());return; }
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceLost("sparse binding");
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
