#pragma once
// bbport (Xbox, 2026-10-27): whole-frame flicker detector (diagnostic, cheap).
// Every presented frame is also blitted into a 64x36 RGBA8 image and copied to a host buffer
// (8-slot ring, read back once the present submission's tick is reached, never waited for).
// A frame whose thumbnail differs from both neighbours while the neighbours match each other
// (A-B-A, or A-B-B-A for two frames) is logged with its context and the thumbnails are
// written to BB_GPU_USER_DIR/flicker-<index>.ppm (first 32 events). Camera motion and cuts
// do not produce that pattern; full-screen flashes, black or stale frames do.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vk_mem_alloc.h>

namespace Vulkan::BbFlicker {
constexpr u32 W = 64, H = 36, Cells = W * H, Slots = 8, HudRows = 8;
struct Meta {
    u64 index = 0, tick = 0, draws = 0, ms = 0, ready_tick = 0, flip = 0;
    u32 frame_id = 0;
    bool reusing = false, game = false;
};
struct Slot {
    VkBuffer buffer{};
    VmaAllocation alloc{};
    void* map{};
    Meta meta{};
    bool pending = false;
};
struct Thumb {
    std::array<u8, Cells * 3> rgb{};
    std::array<u8, Cells> y{};
    Meta meta{};
};
struct State {
    bool init = false, disabled = false;
    VkImage image{};
    VmaAllocation image_alloc{};
    Slot slots[Slots];
    u32 next = 0;
    int recorded = -1;
    u64 index = 0, last_draws = 0, frames = 0, events = 0, dropped = 0, files = 0;
    u64 last_report_ms = 0;
    std::array<Thumb, 4> hist{};
    u32 hist_count = 0;
};
inline State s;

inline u64 NowMs() {
    return u64(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

inline bool Init(const Instance& instance) {
    s.init = true;
    const char* env = std::getenv("BB_FLICKER_DETECT");
    if (env && env[0] == '0') {
        s.disabled = true;
        return false;
    }
    const VkImageCreateInfo image_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {W, H, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo image_alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(instance.GetAllocator(), &image_ci, &image_alloc_ci, &s.image, &s.image_alloc,
                       nullptr) != VK_SUCCESS) {
        std::printf("Flicker detector: image creation failed, disabled\n");
        s.disabled = true;
        return false;
    }
    for (auto& slot : s.slots) {
        const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                           .size = VkDeviceSize(Cells) * 4,
                                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &slot.buffer, &slot.alloc,
                            &info) != VK_SUCCESS ||
            !info.pMappedData) {
            std::printf("Flicker detector: buffer creation failed, disabled\n");
            s.disabled = true;
            return false;
        }
        slot.map = info.pMappedData;
    }
    std::printf("Flicker detector: on (%ux%u thumbnails, %u-slot readback ring)\n", W, H, Slots);
    return true;
}

inline double Diff(const Thumb& a, const Thumb& b, u32 row0, u32 row1) {
    u64 sum = 0;
    for (u32 i = row0 * W; i < row1 * W; ++i) {
        sum += u32(std::abs(int(a.y[i]) - int(b.y[i])));
    }
    return double(sum) / double((row1 - row0) * W);
}
inline double Mean(const Thumb& a) {
    u64 sum = 0;
    for (u8 v : a.y) sum += v;
    return double(sum) / Cells;
}
inline u32 MaxDiff(const Thumb& a, const Thumb& b) {
    u32 m = 0;
    for (u32 i = 0; i < Cells; ++i) m = std::max<u32>(m, u32(std::abs(int(a.y[i]) - int(b.y[i]))));
    return m;
}

inline void WritePpm(const Thumb* const* thumbs, u32 count, u64 index) {
    const char* dir = std::getenv("BB_GPU_USER_DIR");
    if (!dir || !*dir || s.files >= 32) return;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/flicker-%llu.ppm", dir, (unsigned long long)index);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    ++s.files;
    std::fprintf(f, "P6\n%u %u\n255\n", W * count, H);
    for (u32 y = 0; y < H; ++y) {
        for (u32 t = 0; t < count; ++t) {
            std::fwrite(thumbs[t]->rgb.data() + y * W * 3, 1, W * 3, f);
        }
    }
    std::fclose(f);
}

inline void Report(const char* kind, const Thumb* const* t, u32 count, u32 first_odd, u32 odd) {
    const Thumb& a = *t[0];
    const Thumb& b = *t[first_odd];
    const Thumb& z = *t[count - 1];
    ++s.events;
    std::printf("Flicker detector: %s outlier frame #%llu (+%u): mean luma %.1f -> %.1f -> %.1f; "
                "diff in %.1f out %.1f across %.1f (max cell %u); HUD rows %.1f scene rows %.1f; "
                "draws %llu/%llu/%llu; reusing %d game %d frame id %u ready tick %llu present tick %llu; "
                "t=%llu ms\n",
                kind, (unsigned long long)b.meta.index, odd, Mean(a), Mean(b), Mean(z),
                Diff(a, b, 0, H), Diff(*t[first_odd + odd - 1], z, 0, H), Diff(a, z, 0, H),
                MaxDiff(a, b), Diff(a, b, 0, HudRows), Diff(a, b, HudRows, H),
                (unsigned long long)a.meta.draws, (unsigned long long)b.meta.draws,
                (unsigned long long)z.meta.draws, int(b.meta.reusing), int(b.meta.game),
                b.meta.frame_id, (unsigned long long)b.meta.ready_tick,
                (unsigned long long)b.meta.tick, (unsigned long long)b.meta.ms);
    WritePpm(t, count, b.meta.index);
}

inline void Analyze() {
    constexpr double T = 8.0;
    const u32 n = std::min<u32>(s.hist_count, 4);
    const auto at = [&](u32 back) -> const Thumb& { return s.hist[(s.hist_count - 1 - back) % 4]; };
    if (n >= 3) {
        const Thumb* t[3] = {&at(2), &at(1), &at(0)};
        const double ab = Diff(*t[0], *t[1], 0, H), bc = Diff(*t[1], *t[2], 0, H),
                     ac = Diff(*t[0], *t[2], 0, H);
        if (ab >= T && bc >= T && ac * 3 <= std::min(ab, bc)) {
            Report("single", t, 3, 1, 1);
            return;
        }
    }
    if (n >= 4) {
        const Thumb* t[4] = {&at(3), &at(2), &at(1), &at(0)};
        const double ab = Diff(*t[0], *t[1], 0, H), bc = Diff(*t[1], *t[2], 0, H),
                     cd = Diff(*t[2], *t[3], 0, H), ad = Diff(*t[0], *t[3], 0, H);
        if (ab >= T && cd >= T && bc * 3 <= std::min(ab, cd) && ad * 3 <= std::min(ab, cd)) {
            Report("double", t, 4, 1, 2);
        }
    }
}

inline void Process(Slot& slot, const Instance& instance) {
    vmaInvalidateAllocation(instance.GetAllocator(), slot.alloc, 0, VK_WHOLE_SIZE);
    Thumb& t = s.hist[s.hist_count % 4];
    const u8* p = static_cast<const u8*>(slot.map);
    for (u32 i = 0; i < Cells; ++i) {
        const u8 r = p[i * 4], g = p[i * 4 + 1], b = p[i * 4 + 2];
        t.rgb[i * 3] = r;
        t.rgb[i * 3 + 1] = g;
        t.rgb[i * 3 + 2] = b;
        t.y[i] = u8((54u * r + 183u * g + 19u * b) >> 8);
    }
    t.meta = slot.meta;
    ++s.hist_count;
    ++s.frames;
    Analyze();
}

inline void Poll(Scheduler& scheduler, const Instance& instance) {
    scheduler.GetWorkSemaphore()->Refresh();
    while (true) {
        Slot* oldest = nullptr;
        for (auto& slot : s.slots) {
            if (slot.pending && (!oldest || slot.meta.index < oldest->meta.index)) oldest = &slot;
        }
        if (!oldest || !scheduler.GetWorkSemaphore()->IsFree(oldest->meta.tick)) break;
        Process(*oldest, instance);
        oldest->pending = false;
    }
    const u64 now = NowMs();
    if (now - s.last_report_ms >= 10000) {
        s.last_report_ms = now;
        std::printf("Flicker detector: %llu frames analysed, %llu outliers, %llu not sampled\n",
                    (unsigned long long)s.frames, (unsigned long long)s.events,
                    (unsigned long long)s.dropped);
    }
}

/// In the present command buffer, after the frame image (TransferSrcOptimal) was blitted.
inline void Record(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                   vk::Image src, u32 width, u32 height, bool reusing, bool game, u32 frame_id,
                   u64 ready_tick) {
    if (!s.init && !Init(instance)) return;
    if (s.disabled) return;
    Poll(scheduler, instance);
    Slot& slot = s.slots[s.next];
    const u64 draws = BbStats::draws.load(std::memory_order_relaxed);
    if (slot.pending) {
        ++s.dropped;
        s.last_draws = draws;
        return;
    }
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier to_dst{
        .srcAccessMask = vk::AccessFlagBits::eTransferRead,
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = vk::Image{s.image},
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                           {}, {}, {}, to_dst);
    const vk::ImageBlit blit{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(width), s32(height), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(W), s32(H), 1}},
    };
    cmdbuf.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, vk::Image{s.image},
                     vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
    const vk::ImageMemoryBarrier to_src{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = vk::Image{s.image},
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                           {}, {}, {}, to_src);
    cmdbuf.copyImageToBuffer(vk::Image{s.image}, vk::ImageLayout::eTransferSrcOptimal,
                             vk::Buffer{slot.buffer},
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {W, H, 1}});
    const vk::BufferMemoryBarrier to_host{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                          .dstAccessMask = vk::AccessFlagBits::eHostRead,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .buffer = vk::Buffer{slot.buffer},
                                          .offset = 0,
                                          .size = VK_WHOLE_SIZE};
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {},
                           {}, to_host, {});
    slot.meta = {};
    slot.meta.index = ++s.index;
    slot.meta.draws = draws - s.last_draws;
    s.last_draws = draws;
    slot.meta.ms = NowMs();
    slot.meta.ready_tick = ready_tick;
    slot.meta.frame_id = frame_id;
    slot.meta.reusing = reusing;
    slot.meta.game = game;
    s.recorded = int(s.next);
    s.next = (s.next + 1) % Slots;
}

/// Right before the present submission: the tick it signals.
inline void Submitted(u64 tick) {
    if (s.recorded < 0) return;
    s.slots[s.recorded].meta.tick = tick;
    s.slots[s.recorded].pending = true;
    s.recorded = -1;
}
} // namespace Vulkan::BbFlicker
