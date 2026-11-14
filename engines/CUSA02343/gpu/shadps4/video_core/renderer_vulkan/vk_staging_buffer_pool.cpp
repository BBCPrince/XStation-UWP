// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_vram.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// bbport: staging memory is populated (allocated and cleared) by the kernel on first CPU access,
// ~2 ms per 16 MiB block. Streaming bursts a few seconds apart re-created the blocks trimmed
// in between and copies into them ran at 0.3 GB/s (stutter). Blocks now live 30 s idle, the
// upload ring keeps BB_STAGING_KEEP_MB (512) and is populated at startup.
constexpr u64 RING_IDLE_FRAMES = 3000;
constexpr u64 LARGE_IDLE_FRAMES = 3000;
constexpr u64 PREWARM_BLOCKS = 8;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;

// Uncharted Xbox port (2026-11-04): a level load uploads hundreds of textures within a few frames and
// the rings grew by ~1 GB of staging on the Xbox (whose Dozen budget is far above 5 GB, so never
// "tight"). BB_STAGING_CAP_MB=N: a ring holding N MiB reuses its blocks (waiting for the GPU) instead
// of adding one; BB_STAGING_TIGHT=1: the small-card trimming (idle blocks go after ~4 s).
u64 StagingCapBlocks() {
    static const u64 cap = [] {
        const char* env = std::getenv("BB_STAGING_CAP_MB");
        return env ? std::strtoull(env, nullptr, 10) * 1_MB / BLOCK_SIZE : 0;
    }();
    return cap;
}
bool StagingTight() {
    static const bool tight = [] {
        const char* env = std::getenv("BB_STAGING_TIGHT");
        return env && env[0] == '1';
    }();
    return tight;
}

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* env = std::getenv("BB_STAGING_KEEP_MB");
    keep_blocks = (env ? std::strtoull(env, nullptr, 10) : 512) * 1_MB / BLOCK_SIZE;
    Ring& ring = rings[u32(MemoryType::HostUncached)];
    for (u64 i = 0; i < PREWARM_BLOCKS; ++i) {
        auto& block = ring.blocks.emplace_back(Block{
            .buffer = std::make_unique<VideoCore::StreamBuffer>(
                instance, scheduler, MemoryType::HostUncached, BLOCK_SIZE),
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

    if (!best && StagingCapBlocks() && ring.blocks.size() >= StagingCapBlocks()) {
        // At the cap: the next block again, waiting until the GPU is done with its oldest data.
        const size_t index = (ring.current + 1) % ring.blocks.size();
        if (const auto offset = ring.blocks[index].buffer->Reserve(size, alignment, true)) {
            ring.current = index;
            best = &ring.blocks[index];
            best_offset = *offset;
        }
    }
    if (!best) {
        best = &ring.blocks.emplace_back(Block{
            .buffer =
                std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, BLOCK_SIZE),
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

    // Uncharted Xbox port (2026-11-04): under the cap any idle buffer big enough is reused (the
    // smallest), not only one within a quarter of the size: uploads of every size freed and made
    // buffers again each frame (Dozen on the PC: ~30% of the recording thread in GPU waits and
    // allocations, 42 instead of 60 FPS in the jungle).
    const bool capped = StagingCapBlocks() != 0;
    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || (!capped && entry_size > max_size) ||
            !scheduler.IsFree(entry.tick)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best && StagingCapBlocks()) {
        // Uncharted Xbox port (2026-11-04): over the cap, the large buffers the GPU is done with go
        // first (waiting for the GPU once if that is not enough).
        const u64 cap = StagingCapBlocks() * BLOCK_SIZE;
        const auto total = [&] {
            u64 bytes = 0;
            for (const LargeBuffer& entry : cache) {
                bytes += entry.buffer->SizeBytes();
            }
            return bytes;
        };
        // Idle buffers too small for this request go first, the oldest first, only as many as
        // needed (the others stay for the next uploads).
        while (total() + size > cap) {
            LargeBuffer* oldest = nullptr;
            for (LargeBuffer& entry : cache) {
                if (!entry.held && scheduler.IsFree(entry.tick) &&
                    (!oldest || entry.tick < oldest->tick)) {
                    oldest = &entry;
                }
            }
            if (!oldest) {
                break;
            }
            const VideoCore::Buffer* gone = oldest->buffer.get();
            std::erase_if(cache, [gone](const LargeBuffer& entry) { return entry.buffer.get() == gone; });
        }
        if (total() + size > cap) {
            // Still over: wait only for the oldest idle large buffer (not a Finish of all work, which
            // cost the Xbox ~40 FPS) and free it, until under the cap.
            while (total() + size > cap) {
                LargeBuffer* oldest = nullptr;
                for (LargeBuffer& entry : cache) {
                    if (!entry.held && (!oldest || entry.tick < oldest->tick)) {
                        oldest = &entry;
                    }
                }
                if (!oldest) {
                    break;
                }
                scheduler.Wait(oldest->tick);
                const VideoCore::Buffer* gone = oldest->buffer.get();
                std::erase_if(cache, [gone](const LargeBuffer& entry) { return entry.buffer.get() == gone; });
            }
        }
    }
    if (!best) {
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
    // bbport: the device-local rings (detiling scratch) hold video memory: on a small card idle
    // blocks go after ~4 s instead of staying (up to 512 MiB) for 30 s.
    const bool tight = (instance.GetTotalMemoryBudget() < 5_GB || StagingTight()) && !Vram::Legacy();
    for (size_t i = 0; i < rings.size(); ++i) {
        TrimRing(rings[i], tight && i == u32(MemoryType::DeviceLocal));
    }
    for (size_t i = 0; i < large_caches.size(); ++i) {
        TrimLarge(large_caches[i], tight && i == u32(MemoryType::DeviceLocal));
    }
    PublishStats();
}

void StagingBufferPool::TrimRing(Ring& ring, bool eager) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        if (blocks.size() <= (eager ? 1 : keep_blocks) ||
            frame - block.last_used_frame < (eager ? 240 : RING_IDLE_FRAMES) ||
            !scheduler.IsFree(block.buffer->LastTick())) {
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

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache, bool eager) {
    std::erase_if(cache, [this, eager](const LargeBuffer& entry) {
        return !entry.held && frame - entry.last_used_frame >= (eager ? 240 : LARGE_IDLE_FRAMES) &&
               scheduler.IsFree(entry.tick);
    });
}

std::atomic<u64> g_staging_ring_bytes{0}, g_staging_large_bytes{0};
extern "C" void bb_staging_stats(unsigned long long* ring, unsigned long long* large) {
    *ring = g_staging_ring_bytes.load(std::memory_order_relaxed);
    *large = g_staging_large_bytes.load(std::memory_order_relaxed);
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    {
        // Uncharted Xbox port (2026-11-04): device-local staging for the 15 s report.
        u64 ring_bytes = 0, large_bytes = 0;
        for (const Block& block : rings[u32(MemoryType::DeviceLocal)].blocks) {
            ring_bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[u32(MemoryType::DeviceLocal)]) {
            large_bytes += entry.buffer->SizeBytes();
        }
        g_staging_ring_bytes.store(ring_bytes, std::memory_order_relaxed);
        g_staging_large_bytes.store(large_bytes, std::memory_order_relaxed);
    }
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

} // namespace Vulkan
