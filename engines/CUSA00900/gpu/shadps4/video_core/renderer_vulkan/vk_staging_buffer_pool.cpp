// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include "bbport_process_memory.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// bbport: staging memory is populated (allocated and cleared) by the kernel on first CPU access,
// ~2 ms per 16 MiB block. Streaming bursts a few seconds apart re-created the blocks trimmed
// in between and copies into them ran at 0.3 GB/s (stutter). Blocks now live 30 s idle, the
// upload ring keeps BB_STAGING_KEEP_MB (512) and is populated at startup.
constexpr u64 RING_IDLE_FRAMES = 3000;
constexpr u64 PREWARM_BLOCKS = 8;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* env = std::getenv("BB_STAGING_KEEP_MB");
    const bool compact = EmulatorSettings.IsCompactMemoryEnabled();
    keep_blocks = (env ? std::strtoull(env, nullptr, 10) : compact ? 32 : 512) * 1_MB / BLOCK_SIZE;
    idle_frames = compact ? 180 : RING_IDLE_FRAMES;
    if (compact && keep_blocks == 1) {
        ring_grace_frames = std::min<u64>(8,
            EmulatorSettingsImpl::Number("BB_STAGING_RING_GRACE_FRAMES", 0));
    }
    allocation_pressure_bytes = compact ? 128_MB : 0;
    if (const char* pressure = std::getenv("BB_STAGING_PRESSURE_MB"))
        allocation_pressure_bytes = std::max<u64>(16, std::strtoull(pressure, nullptr, 10)) * 1_MB;
    Ring& ring = rings[u32(MemoryType::HostUncached)];
    for (u64 i = 0; i < (compact ? 1 : PREWARM_BLOCKS); ++i) {
        auto& block = ring.blocks.emplace_back(Block{
            .buffer = std::make_unique<VideoCore::StreamBuffer>(
                instance, scheduler, MemoryType::HostUncached, BLOCK_SIZE,"StagingBufferPool:Ring"),
            .last_used_frame = frame,
        });
        if (!block.buffer->mapped_data.empty()) {
            std::memset(block.buffer->mapped_data.data(), 0, block.buffer->mapped_data.size());
        }
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment,
                                            bool deferred) {
    BbStats::Timer timer{BbStats::t_staging};
    Ring& ring = rings[u32(type)];
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred);
    }
    return RequestFromRing(ring, size, alignment, type);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type) {
    Block* best{};
    u64 best_offset{};
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (ring.current + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        TrimForAllocationPressure(BLOCK_SIZE);
        best = &ring.blocks.emplace_back(Block{
            .buffer =
                std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, BLOCK_SIZE,"StagingBufferPool:Ring"),
            .last_used_frame = frame,
        });
        ring.current = ring.blocks.size() - 1;
        best_offset = *best->buffer->Reserve(size, alignment, false);
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            !scheduler.IsFree(entry.tick)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        // Prefer an existing compatible allocation above. Only discard other
        // completed cached allocations when a new backing allocation is needed.
        TrimForAllocationPressure(RoundAllocationSize(size));
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    for (Ring& ring : rings) {
        TrimRing(ring);
    }
    for (auto& cache : large_caches) {
        TrimLarge(cache);
    }
    PublishStats();
}

void StagingBufferPool::TrimRing(Ring& ring, bool force) {
    auto& blocks = ring.blocks;
    // Keep at most one additional 16 MiB GPU upload block for a few frames.
    // Immediate deletion made alternating uploads allocate a new D3D12 backing
    // every frame. This grace is opt-in, excludes CPU readbacks, and disappears
    // under app pressure or an explicit allocation-pressure reclaim.
    bool warm_block = !force && ring_grace_frames &&
                      &ring == &rings[u32(MemoryType::DeviceLocal)] &&
                      blocks.size() > keep_blocks;
    if (warm_block) {
        const auto usage = BbProcessMemory::Get();
        warm_block = usage.app_info_valid && usage.available_commit >= 512_MB;
    }
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        // Cycling through a large ring can keep every block "recent" forever.
        // Compact mode sheds completed excess immediately, while in-flight
        // watches and allocations in the current submission remain protected.
        if (blocks.size() <= keep_blocks ||
            (!EmulatorSettings.IsCompactMemoryEnabled() && frame - block.last_used_frame < idle_frames) ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        if (warm_block && blocks.size() == keep_blocks + 1 &&
            frame - block.last_used_frame < ring_grace_frames) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache) {
    std::erase_if(cache, [this](const LargeBuffer& entry) {
        return !entry.held && frame - entry.last_used_frame >= idle_frames &&
               scheduler.IsFree(entry.tick);
    });
}

void StagingBufferPool::TrimForAllocationPressure(u64 incoming_bytes) {
    if (!allocation_pressure_bytes) return;
    const u64 before = AllocatedBytes();
    if (before + incoming_bytes <= allocation_pressure_bytes) return;
    // Refresh completion without submitting or waiting. Pending work and held
    // CPU readbacks remain protected; this is a cache watermark, not a hard cap.
    scheduler.GetWorkSemaphore()->Refresh();
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        // A synchronous CPU readback can still be consumed by its caller after
        // Finish runs deferred allocation callbacks. Preserve the ordinary
        // readback cache lifetime even if it was not explicitly held.
        if (i == u32(MemoryType::HostCached)) continue;
        std::erase_if(large_caches[i], [this](const LargeBuffer& entry) {
            return !entry.held && scheduler.IsFree(entry.tick);
        });
    }
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        if (i != u32(MemoryType::HostCached)) TrimRing(rings[i], true);
    }
    const u64 after = AllocatedBytes();
    if (after < before && pressure_reports++ < 16) {
        std::printf("Staging pressure: reclaimed %llu MiB before allocating %llu MiB; %llu MiB retained\n",
                    (unsigned long long)((before - after) >> 20),
                    (unsigned long long)(incoming_bytes >> 20),
                    (unsigned long long)(after >> 20));
    }
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

u64 StagingBufferPool::AllocatedBytes() const {
    u64 bytes = 0;
    for (const auto& ring : rings)
        for (const auto& block : ring.blocks)
            bytes += block.buffer->SizeBytes();
    for (const auto& cache : large_caches)
        for (const auto& entry : cache)
            bytes += entry.buffer->SizeBytes();
    return bytes;
}

u64 StagingBufferPool::UploadAllocatedBytes() const {
    u64 bytes = 0;
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        if (i == u32(MemoryType::HostCached)) continue;
        for (const auto& block : rings[i].blocks) bytes += block.buffer->SizeBytes();
        for (const auto& entry : large_caches[i]) bytes += entry.buffer->SizeBytes();
    }
    return bytes;
}

u64 StagingBufferPool::UploadPressureBytes() const {
    const u64 allocated = UploadAllocatedBytes();
    const auto& blocks = rings[u32(MemoryType::DeviceLocal)].blocks;
    if (!ring_grace_frames || blocks.size() <= keep_blocks) return allocated;
    const auto usage = BbProcessMemory::Get();
    if (!usage.app_info_valid || usage.available_commit < 512_MB) return allocated;
    for (const auto& block : blocks) {
        if (frame - block.last_used_frame < ring_grace_frames &&
            scheduler.IsFree(block.buffer->LastTick())) {
            // This backing is cached, not pending GPU upload work. Counting it
            // against the live-upload watermark would Finish and delete it at
            // the next image, undoing reuse and adding an unnecessary GPU wait.
            return allocated - BLOCK_SIZE;
        }
    }
    return allocated;
}

void StagingBufferPool::ReclaimCompletedUploads() {
    TrimForAllocationPressure(0);
}

} // namespace Vulkan
