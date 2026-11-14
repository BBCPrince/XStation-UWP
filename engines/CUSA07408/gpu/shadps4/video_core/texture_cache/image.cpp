// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include <atomic>
#include <ranges>
#include "common/assert.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>
#include <cstdio>
#include <cstdlib>

extern "C" uint64_t runtime_memory_make_room(uint64_t bytes); // tlou194_uwp_memory.c

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance& instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used.
            usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Destroy() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

// bbport: integrated GPUs whose only device-local heap is small. AMD APUs on Windows report the
// BIOS's frame buffer (UMA, 256-512 MiB) as that heap and give the GPU the rest from shared
// system memory, the same RAM. Every image preferred the small heap, textures filled it, and a
// render target that needed it failed later in play (ErrorOutOfDeviceMemory after some minutes,
// 256 MiB, Ryzen 5600H). There, textures (images that are never rendered to) prefer system
// memory and keep the small heap for render targets. BB_IMAGES_PREFER_HOST=1 or 0 forces it.
static bool TexturesPreferHost(VmaAllocator allocator) {
    static const bool prefer = [allocator] {
        if (const char* env = std::getenv("BB_IMAGES_PREFER_HOST"); env && env[0]) {
            return env[0] == '1';
        }
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties(allocator, &props);
        u64 device_local = 0;
        bool shared_heap = false;
        for (u32 i = 0; i < props->memoryHeapCount; ++i) {
            if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                device_local += props->memoryHeaps[i].size;
            } else {
                shared_heap = true;
            }
        }
        const bool small = shared_heap && device_local < (1ull << 30);
        if (small) {
            std::printf("GPU: device-local memory %llu MiB: textures in shared system memory, the "
                        "device-local heap for render targets\n",
                        (unsigned long long)(device_local >> 20));
            std::fflush(stdout);
        }
        return small;
    }();
    return prefer;
}

// bbport: what each memory heap holds and allows, for the log when an image cannot be allocated.
static void LogHeaps(VmaAllocator allocator, vk::Device device, const vk::ImageCreateInfo& image_ci) {
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(allocator, &props);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, budgets);
    u32 type_bits = 0;
    vk::DeviceSize needed = 0, alignment = 0;
    if (auto [result, probe] = device.createImage(image_ci); result == vk::Result::eSuccess) {
        const auto req = device.getImageMemoryRequirements(probe);
        type_bits = req.memoryTypeBits;
        needed = req.size;
        alignment = req.alignment;
        device.destroyImage(probe);
    }
    std::printf("GPU: image %ux%u format %s usage %s could not be allocated (memory types %#x)\n",
                image_ci.extent.width, image_ci.extent.height, vk::to_string(image_ci.format).c_str(),
                vk::to_string(image_ci.usage).c_str(), type_bits);
    // God of War port (2026-11-03): the rest of the image, to tell an odd image from a full heap.
    std::printf("GPU:   depth %u, mips %u, layers %u, samples %s, type %s, tiling %s, flags %s; needs %llu bytes "
                "(alignment %llu)\n",
                image_ci.extent.depth, image_ci.mipLevels, image_ci.arrayLayers,
                vk::to_string(image_ci.samples).c_str(), vk::to_string(image_ci.imageType).c_str(),
                vk::to_string(image_ci.tiling).c_str(), vk::to_string(image_ci.flags).c_str(),
                (unsigned long long)needed, (unsigned long long)alignment);
    for (u32 i = 0; i < props->memoryHeapCount; ++i) {
        std::printf("GPU: heap %u%s: %llu MiB, used %llu of budget %llu MiB\n", i,
                    props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (device-local)" : "",
                    (unsigned long long)(props->memoryHeaps[i].size >> 20),
                    (unsigned long long)(budgets[i].usage >> 20),
                    (unsigned long long)(budgets[i].budget >> 20));
    }
    std::fflush(stdout);
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    bool created = TryCreate(image_ci);
    // 2026-10-31: at the Xbox's memory limit, cold guest memory goes to the pager's file (the
    // same commit) and the image is tried again.
    for (int attempt = 0; attempt < 3 && !created; ++attempt) {
        if (!runtime_memory_make_room(u64(128) << 20 << attempt)) {
            break;
        }
        created = TryCreate(image_ci);
    }
    ASSERT_MSG(created, "Failed allocating image with error {}",
               vk::to_string(vk::Result::eErrorOutOfDeviceMemory));
}

bool UniqueImage::TryCreate(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    const bool texture = !(image_ci.usage & (vk::ImageUsageFlagBits::eColorAttachment |
                                             vk::ImageUsageFlagBits::eDepthStencilAttachment));
    VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = texture && TexturesPreferHost(allocator) ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                                                          : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image,
                                     &allocation, &alloc_info);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // bbport: VRAM full (6 GB cards, high resolutions). Past the budget the driver keeps the
        // image in system memory, which is slower but no crash.
        Vulkan::NoteOverBudget("image");
        VmaAllocationCreateInfo over_budget = alloc_ci;
        over_budget.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(allocator, &image_ci_unsafe, &over_budget, &unsafe_image,
                                &allocation, &alloc_info);
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            // bbport: the preferred heap is full: wherever the image may go instead.
            over_budget.usage = alloc_ci.usage == VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                                    ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
                                    : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            result = vmaCreateImage(allocator, &image_ci_unsafe, &over_budget, &unsafe_image,
                                    &allocation, &alloc_info);
        }
        if (result != VK_SUCCESS) {
            LogHeaps(allocator, device, image_ci);
        }
    }
    // bbport: out of memory past the budget too (AMD keeps render targets in VRAM only, so a
    // 4 GB card has no system memory to fall back on): the caller frees memory and tries again.
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        allocation = {};
        return false;
    }
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
    return true;
}

Image::Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : Image(instance, runtime_, slot_image_views_, info_, 0) {}
Image::Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_,
             u32 host_skip)
    : guest_begin{info_.guest_address}, guest_end{info_.guest_address + info_.guest_size},
      info{info_}, runtime{&runtime_}, slot_image_views{&slot_image_views_} {
    BbStats::Timer timer{BbStats::t_image_create};
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }
    usage.host_skip = host_skip;

    image_uid = global_image_uid.Next();
    // God of War port (2026-11-03): texture arrays beyond the device's layer limit (D3D12: 2048;
    // God of War makes a 256x1 array of 4096) fail to allocate and the device is lost. They are
    // made with as many layers as the device allows; uploads and views follow the smaller count.
    {
        static const u32 max_layers =
            instance.GetPhysicalDevice().getProperties().limits.maxImageArrayLayers;
        if (info.resources.layers > max_layers && info.type != AmdGpu::ImageType::Color3D) {
            static std::atomic<u32> said{0};
            if (said.fetch_add(1) < 8) {
                std::printf("GPU: %ux%u texture array of %u layers capped at the device's %u\n",
                            info.size.width, info.size.height, info.resources.layers, max_layers);
                std::fflush(stdout);
            }
            info.resources.layers = max_layers;
        }
    }
    mip_hashes.resize(info.resources.levels);
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance.GetSupportedFormat(info.pixel_format, format_features);
    vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = ConvertImageType(info.type),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    auto image_format_properties =
        instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    // bbport: storage usage is speculative (see ImageUsageFlags). Drivers that refuse it for a
    // format (AMD's Windows driver for BC6H) get the image without it: creating the unsupported
    // combination anyway loses the device a few frames later.
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported &&
        (usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        format_info.usage = usage_flags & ~vk::ImageUsageFlagBits::eStorage;
        image_format_properties = instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
        if (image_format_properties.result == vk::Result::eSuccess) {
            usage_flags = format_info.usage;
            format_features = FormatFeatureFlags(usage_flags);
        } else {
            format_info.usage = usage_flags;
        }
    }
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(supported_format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;

    const vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = std::max(info.size.width >> host_skip, 1u),
            .height = std::max(info.size.height >> host_skip, 1u),
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels) - host_skip,
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = LiverpoolToVK::NumSamples(info.num_samples, supported_samples),
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    // bbport: BB_IMAGE_FAIL_EVERY=N (tests): every Nth image finds no memory once, as on a full
    // 4 GB card, so TextureCache::InsertImage's freeing runs often on any GPU.
    static const u32 fail_every = [] {
        const char* env = std::getenv("BB_IMAGE_FAIL_EVERY");
        return env ? u32(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    static std::atomic<u32> num_created{0};
    if (fail_every && (num_created.fetch_add(1, std::memory_order_relaxed) + 1) % fail_every == 0) {
        backing->image.image_ci = image_ci;
        allocation_failed = true;
        return;
    }
    if (!backing->image.TryCreate(image_ci)) {
        allocation_failed = true; // TextureCache::InsertImage frees memory and retries
        return;
    }

    Vulkan::SetObjectName(instance.GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

bool Image::RetryAllocation() {
    if (!backing->image.TryCreate(backing->image.image_ci)) {
        return false;
    }
    allocation_failed = false;
    return true;
}

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    if (backing->last_view < view_infos.size() && view_infos[backing->last_view] == view_info) {
        return (*slot_image_views)[backing->image_view_ids[backing->last_view]];
    }
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        backing->last_view = static_cast<u32>(std::distance(view_infos.begin(), it));
        const auto view_id = backing->image_view_ids[backing->last_view];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(runtime->GetInstance(), view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

void Image::GetBarriers(Barriers& barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                        vk::PipelineStageFlags2 dst_stage,
                        std::optional<SubresourceRange> subres_range) {
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;
    // bbport (2026-10-28): reduced texture detail. Ranges come in guest levels; the Vulkan image
    // has host_skip fewer (a guest level it leaves out is its finest level).
    const SubresourceExtent resources{info.resources.levels - usage.host_skip,
                                      info.resources.layers};
    if (usage.host_skip && subres_range) {
        const u32 skip = usage.host_skip;
        const u32 end = subres_range->base.level + subres_range->extent.levels;
        const u32 base = subres_range->base.level > skip ? subres_range->base.level - skip : 0;
        subres_range->base.level = base;
        subres_range->extent.levels = end > skip + base ? end - skip - base : 1;
    }
    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != resources);
    const bool partially_transited = !subresource_states.empty();

    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(resources.levels * resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.level,
                                           subres_range->base.level + subres_range->extent.levels)
                : std::views::iota(0u, resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.layer,
                                           subres_range->base.layer + subres_range->extent.layers)
                : std::views::iota(0u, info.resources.layers);

        for (u32 mip : mips) {
            // God of War port (2026-11-03): adjacent layers in the same old state share one
            // barrier (one per layer overflowed the 128 KiB recording chunk for a 2048-layer array).
            State run_state{};
            u32 run_start = 0, run_count = 0;
            const auto flush = [&] {
                if (!run_count) {
                    return;
                }
                barriers.emplace_back(vk::ImageMemoryBarrier2{
                    .srcStageMask = run_state.pl_stage,
                    .srcAccessMask = run_state.access_mask,
                    .dstStageMask = dst_stage,
                    .dstAccessMask = dst_mask,
                    .oldLayout = run_state.layout,
                    .newLayout = dst_layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = GetImage(),
                    .subresourceRange{
                        .aspectMask = aspect_mask,
                        .baseMipLevel = mip,
                        .levelCount = 1,
                        .baseArrayLayer = run_start,
                        .layerCount = run_count,
                    },
                });
                run_count = 0;
            };
            for (u32 layer : layers) {
                // God of War port: a range past an array capped at the device's layer limit
                // (see the constructor) has no subresources here.
                if (mip >= info.resources.levels || layer >= info.resources.layers) {
                    continue;
                }
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    const bool same = run_count && run_start + run_count == layer &&
                                      run_state.layout == state.layout &&
                                      run_state.access_mask == state.access_mask &&
                                      run_state.pl_stage == state.pl_stage;
                    if (same) {
                        ++run_count;
                    } else {
                        flush();
                        run_state = state;
                        run_start = layer;
                        run_count = 1;
                    }
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                } else {
                    flush();
                }
            }
            flush();
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return;
        }

        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;
}

} // namespace VideoCore
