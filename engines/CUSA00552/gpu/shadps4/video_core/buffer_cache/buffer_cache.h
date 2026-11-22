// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "bbport_copy.h"

#include <array>
#include <chrono>
#include <deque>
#include <unordered_set>
#include <mutex>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // bbport: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 16;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer();
    bool RunDenseRegression194();
    const Buffer* GetNullBuffer() const noexcept { return &null_buffer; }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// bbport: memory backing guest memory for the GPU (never freed), the reserve kept for it,
    /// and the part of the reserve used (for the memory report).
    [[nodiscard]] u64 ArenaBackedBytes() const { return arena_backed; }
    [[nodiscard]] u64 ArenaReserveBytes() const { return reserve_size; }
    /// bbport: the last eviction pass over the budget: live and idle arena bytes, idle ones kept
    /// for unread GPU-written data, the largest arena, the arena count.
    struct ArenaReport {
        u64 live{}, idle_gpu{}, largest{}, count{}, idle{};
        u64 differed{}, differed_bytes{}; ///< idle arenas kept: unlike guest memory (all passes)
    };
    [[nodiscard]] const ArenaReport& LastArenaReport() const { return arena_report; }
    [[nodiscard]] u64 ArenaReserveUsedBytes() const { return reserve_used; }
    /// bbport (BB_HOST_REPORT): arenas of 16 MiB or more, with their host mapping, to match them
    /// with the large allocations in the host memory report.
    void ReportLargeArenas() const;
    /// bbport: whether a mapped arena holds what guest memory does, where the CPU has not
    /// written since (else it holds GPU-made data guest memory never got: it must not go).
    bool ArenaMatchesGuest(const Buffer* arena);
    u64 arena_differed{}, arena_differed_bytes{};

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// bbport: drops dense arenas unused for a minute while their total is over budget (GPU
    /// command thread, at submissions; checks every 2 s).
    void EvictIdleArenas();

private:
    // Allocate lookup leaves only for guest regions that actually acquire an
    // arena. A flat 64 KiB-page table for 40-bit addresses would cost 128 MiB.
    class ArenaPageTable {
        static constexpr u64 LeafEntries = 4096;
        using Leaf = std::array<const Buffer*, LeafEntries>;
        std::vector<std::unique_ptr<Leaf>> leaves;
    public:
        void resize(u64 entries) {
            leaves.resize((entries + LeafEntries - 1) / LeafEntries);
        }
        const Buffer* Get(u64 page) const {
            const auto& leaf = leaves[page / LeafEntries];
            return leaf ? (*leaf)[page % LeafEntries] : nullptr;
        }
        void Set(u64 page, const Buffer* buffer) {
            auto& leaf = leaves[page / LeafEntries];
            if (!leaf) {
                if (!buffer) return;
                leaf = std::make_unique<Leaf>();
            }
            (*leaf)[page % LeafEntries] = buffer;
        }
        u64 AllocatedBytes() const {
            u64 bytes = leaves.capacity() * sizeof(std::unique_ptr<Leaf>);
            for (const auto& leaf : leaves) if (leaf) bytes += sizeof(Leaf);
            return bytes;
        }
    };

    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);
    const Buffer* GetDenseArena(u64 first_block, u64 last_block, bool overlay = false);
    const Buffer* SplitDenseArena(u64 first_page, u64 last_page, u64 joined, u64 limit, bool mapped);
    void UpdateDensePageTable(const Buffer* arena, std::span<const Interval> ranges);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

public:
    /// bbport (BB_HOT_READBACK_FLUSH=1): a draw or dispatch wrote memory that readbacks recently
    /// had to wait for before it was even submitted; the caller submits right after it.
    bool TakeHotWriteFlush() {
        ++hot_flush_calls;
        if (!std::exchange(hot_write_flush, false) || hot_flush_calls - hot_flush_last < 32) {
            return false;
        }
        hot_flush_last = hot_flush_calls;
        return true;
    }

private:
    void NoteHotReadback(VAddr address, u64 size);
    bool IsHotReadback(VAddr address, u64 size) const;
    /// 4 KiB blocks (address >> 12), oldest first. TLOU (2026-11-01): 32 were too few, Boston's
    /// GPU results the CPU reads every frame span hundreds (BB_HOT_READBACK_BLOCKS, default 1024).
    std::unordered_set<u64> hot_readback_set;
    std::deque<u64> hot_readback_order;
    u32 hot_readback_next = 0;
    bool hot_write_flush = false;
    u64 hot_flush_calls = 0, hot_flush_last = 0; ///< TakeHotWriteFlush calls (draws/dispatches)

    /// bbport (BB_GPU_SHARED): the GPU-shared guest memory chunk holding the whole range, or
    /// nullptr (none, or the range leaves it).
    const Buffer* SharedArena(VAddr addr, u64 size);
    /// Read-only bindings of guest memory the CPU wrote after a shared binding of it take a
    /// copy: the guest may rewrite it as soon as the copies are done (its fences do not wait
    /// for the GPU), before the GPU reads it.
    bool IsSharedDynamic(VAddr addr, u64 size) const;
    void NoteSharedUse(VAddr addr, u64 size);
    void NoteSharedWrite(VAddr addr, u64 size, bool assume_locks);
    void ForwardToOverlays(std::span<const std::pair<u64, u64>> dirty);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);
    /// bbport (BB_FAST_READBACK): DownloadMemory's copy on the readback queue when every
    /// writer of the copied bytes has completed; false (nothing done) otherwise.
    bool FastReadback(const Buffer* arena, const Buffer* staging,
                      std::span<const vk::BufferCopy> copies);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item, bool deferrable = false);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    Buffer null_buffer;
    RangeSet gpu_modified_ranges;
    vk::Queue readback_queue;
    vk::UniqueCommandPool readback_pool;
    vk::CommandBuffer readback_cmdbuf;
    vk::UniqueFence readback_fence;
    std::mutex readback_mutex;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    ArenaPageTable address_space;
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;
    /// bbport: live dense arenas, the eviction epoch (advanced every 2 s) and its clock.
    std::vector<const Buffer*> dense_live;
    ArenaReport arena_report;
    u64 use_epoch = 1;
    std::chrono::steady_clock::time_point evict_time{};
    void EvictArena(const Buffer* arena);
    /// bbport: block ranges recently found resident (resident_ranges only grows: never stale).
    std::array<std::pair<u64, u64>, 16> resident_hint{};
    u32 resident_hint_next = 0;

    bool sparse_buffers{};
    u32 arena_memory_type_index{};
    u32 arena_memory_type_bits{}; ///< bbport: the types the arena accepts (system memory fallback)
    /// bbport: VRAM kept for the arena when the GPU has no other room (discrete GPUs).
    vk::DeviceMemory reserve_memory{};
    u64 reserve_size{};
    u64 reserve_used{};
    u64 arena_backed{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
