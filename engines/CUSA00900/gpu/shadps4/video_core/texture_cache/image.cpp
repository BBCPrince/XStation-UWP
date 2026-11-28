// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include "bbport_memory_ledger.h"
#include <ranges>
#include <vector>
#include <utility>
#include <mutex>
#include <memory>
#include <map>
#include <atomic>
#include <cstdlib>
#include <fmt/format.h>
#include "common/assert.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

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
    } else if (instance.IsBlockTexelViewSupported()) {
        // Storage usage on a compressed image is for an uncompressed block
        // view. Extended usage cannot supply a view type the device does not
        // support; uploads and detiling already use transfer buffers otherwise.
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
    Destroy();
}

void UniqueImage::Destroy() {
    if (image) {
        BbMemoryLedger::Resource("destroy","image",allocator,allocation);
        vmaDestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

extern "C" uint64_t runtime_memory_make_room(uint64_t bytes); // runtime_memory.c (pager)

// XStation 2026-11-21: images share VMA's memory blocks (BB_IMAGE_SUBALLOC=0: one heap each, the old way). A spot in a
// shared block keeps an old image's contents where a new heap was zeros, so Image's constructor clears it once.
// XStation 2026-11-21 (eighth version): only block-compressed images share blocks (set by Image's constructor around
// Create). Drawable ones in shared blocks showed wrong glows even zeroed (0.1.4.17): they keep their own heap.
thread_local bool bb_image_shareable = false;
static bool BbImageSuballoc() {
    static const bool on = [] {
        const char* env = std::getenv("BB_IMAGE_SUBALLOC");
        return !env || env[0] != '0';
    }();
    return on;
}

// XStation 2026-11-21: the memory figures when an image's memory can't be had (first 8 times): VMA's memory objects
// (each one D3D12 heap) and per heap VMA's and the driver's usage and budget.
static void BbLogImageAllocation(VmaAllocator allocator, const char* what, VkResult result) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) >= 8) {
        return;
    }
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, budgets);
    VmaAllocatorInfo info{};
    vmaGetAllocatorInfo(allocator, &info);
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT driver{};
    vk::PhysicalDeviceMemoryProperties2 props{};
    props.pNext = &driver;
    vk::PhysicalDevice{info.physicalDevice}.getMemoryProperties2(&props);
    const auto mib = [](u64 v) { return (unsigned long long)(v >> 20); };
    std::printf("GPU image memory: %s (%s); VMA %u memory objects %llu MiB, %u allocations %llu MiB, largest free range %llu KiB\n",
                what, vk::to_string(vk::Result{result}).c_str(), stats.total.statistics.blockCount,
                mib(stats.total.statistics.blockBytes), stats.total.statistics.allocationCount,
                mib(stats.total.statistics.allocationBytes), (unsigned long long)(stats.total.unusedRangeSizeMax >> 10));
    for (u32 h = 0; h < props.memoryProperties.memoryHeapCount; ++h) {
        std::printf("  heap %u: %llu MiB; VMA %u objects %llu MiB, usage %llu of budget %llu MiB; driver usage %llu of budget %llu MiB\n",
                    h, mib(props.memoryProperties.memoryHeaps[h].size), budgets[h].statistics.blockCount,
                    mib(budgets[h].statistics.blockBytes), mib(budgets[h].usage), mib(budgets[h].budget),
                    mib(driver.heapUsage[h]), mib(driver.heapBudget[h]));
    }
    std::fflush(stdout);
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    const auto allocation_name = fmt::format("Image {}x{}x{} {} layers={} mips={}",
        image_ci.extent.width, image_ci.extent.height, image_ci.extent.depth,
        vk::to_string(image_ci.format), image_ci.arrayLayers, image_ci.mipLevels);
    const BbMemoryLedger::Scope ledger_owner{allocation_name};
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    // XStation 2026-11-21: shared blocks, not one memory object (D3D12 heap) per image: Dozen prefers a dedicated
    // allocation for every optimal-tiling image, and the Xbox refused new heaps at about 1,260 of them (two crashes
    // on world loads with app memory to spare). Plain requirements (no dedicated preference): VMA suballocates,
    // and only images over half a block (BB_VMA_BLOCK_MB) get their own. BB_IMAGE_SUBALLOC=1 only (test).
    const bool suballoc = BbImageSuballoc();
    static std::atomic<int> suballoc_noted{0};
    if (suballoc && !suballoc_noted.exchange(1)) {
        std::printf("GPU image memory: images share memory blocks (not one heap each)\n");
    }
    VmaAllocatorInfo vma_info{};
    vmaGetAllocatorInfo(allocator, &vma_info);
    const auto create = [&](const VmaAllocationCreateInfo& ci) -> VkResult {
        if (!suballoc || !bb_image_shareable) {
            return vmaCreateImage(allocator, &image_ci_unsafe, &ci, &unsafe_image, &allocation,
                                  &alloc_info);
        }
        const auto& api = VULKAN_HPP_DEFAULT_DISPATCHER;
        VkImage created{};
        VkResult r = api.vkCreateImage(vma_info.device, &image_ci_unsafe, nullptr, &created);
        if (r != VK_SUCCESS) {
            return r;
        }
        VkMemoryRequirements requirements{};
        api.vkGetImageMemoryRequirements(vma_info.device, created, &requirements);
        VmaAllocationCreateInfo plain = ci;
        plain.usage = VMA_MEMORY_USAGE_UNKNOWN; // the AUTO usages need vmaCreateImage
        plain.preferredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VmaAllocation memory{};
        r = vmaAllocateMemory(allocator, &requirements, &plain, &memory, &alloc_info);
        if (r == VK_SUCCESS) {
            r = vmaBindImageMemory(allocator, memory, created);
        }
        if (r != VK_SUCCESS) {
            if (memory) {
                vmaFreeMemory(allocator, memory);
            }
            api.vkDestroyImage(vma_info.device, created, nullptr);
            return r;
        }
        unsafe_image = created;
        allocation = memory;
        return VK_SUCCESS;
    };
    VkResult result = create(alloc_ci);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // bbport: VRAM full (6 GB cards, high resolutions). Past the budget the driver keeps the
        // image in system memory, which is slower but no crash.
        Vulkan::NoteOverBudget("image");
        VmaAllocationCreateInfo over_budget = alloc_ci;
        over_budget.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = create(over_budget);
        // bbport (pager, 2026-11-03; TLOU 2026-10-31): on the Xbox the image's memory is the same
        // commit as guest memory: cold guest memory goes to the pager's file, then a retry.
        for (int attempt = 0; attempt < 3 && result == VK_ERROR_OUT_OF_DEVICE_MEMORY; ++attempt) {
            if (!runtime_memory_make_room(u64(128) << 20 << attempt)) {
                break;
            }
            result = create(over_budget);
        }
        if (result != VK_SUCCESS) {
            BbLogImageAllocation(allocator, "failed", result);
        }
    }
    ASSERT_MSG(result == VK_SUCCESS,
               "Failed allocating image {} {}x{}x{} layers={} mips={} usage={} flags={}: {}",
               vk::to_string(image_ci.format),image_ci.extent.width,image_ci.extent.height,
               image_ci.extent.depth,image_ci.arrayLayers,image_ci.mipLevels,
               vk::to_string(image_ci.usage),vk::to_string(image_ci.flags),
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
    vmaSetAllocationName(allocator, allocation, allocation_name.c_str());
    BbMemoryLedger::Resource("create","image",allocator,allocation,0,u32(image_ci.usage));
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
    // XStation 2026-11-23 (layered_own_heap.py, build 18): multi-layer compressed images keep their own heap.
    static const bool bb_share_layered = [] {
        const char* env = std::getenv("BB_SHARE_LAYERED");
        return env && env[0] == '1';
    }();
    bb_image_shareable = info.props.is_block && (info.resources.layers <= 1 || bb_share_layered);
    const bool bb_layered_shared = bb_image_shareable && info.resources.layers > 1;
    backing->image.Create(image_ci);
    bb_image_shareable = false;
    if (info.props.is_block && info.resources.layers > 1) {
        static std::atomic<int> placement_lines{0};
        if (placement_lines.fetch_add(1) < 600) {
            VmaAllocationInfo ai{};
            vmaGetAllocationInfo(backing->image.allocator, backing->image.allocation, &ai);
            std::printf("Layered placement: %#llx %s %ux%u, %u levels, %u layers: %s, memory %#llx offset %#llx size "
                        "%#llx\n",
                        (unsigned long long)info.guest_address, vk::to_string(image_ci.format).c_str(),
                        info.size.width, info.size.height, info.resources.levels, info.resources.layers,
                        bb_layered_shared ? "shared" : "own heap",
                        (unsigned long long)reinterpret_cast<uintptr_t>(ai.deviceMemory),
                        (unsigned long long)ai.offset, (unsigned long long)ai.size);
            std::fflush(stdout);
        }
    }
    // XStation 2026-11-21: in a shared block (BbImageSuballoc) the image starts with whatever was there; zeros once,
    // before any use, as a fresh heap had (the first shared-block test, without this, broke guest data in 55 s).
    // Block-compressed images can't be cleared; they are uploaded whole from the guest.
    // 2026-11-21 (seventh version): every image in a shared block starts as zeros, as a fresh heap was. The fifth
    // version cleared only drawable ones and the Hunter's Dream showed rainbow glows (old bytes in light textures).
    // Depth and multisampled images: a clear. All others (compressed ones too): a copy from a zeroed buffer
    // (images over 16 MiB are never in a shared block: VMA gives them their own above half a 16 MiB block).
    constexpr vk::DeviceSize kZeroBytes = 16ull << 20;
    const bool depth = bool(aspect_mask & (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil));
    const bool multisampled = image_ci.samples != vk::SampleCountFlagBits::e1;
    const bool by_clear = depth || multisampled;
    // Eighth version: only block-compressed images share blocks, so only they are zeroed (by the copy).
    if (BbImageSuballoc() && info.props.is_block && !by_clear && (usage_flags & vk::ImageUsageFlagBits::eTransferDst) &&
        backing->image.size_bytes <= kZeroBytes) {
        static std::mutex zero_lock;
        static VkBuffer zero_buffer = VK_NULL_HANDLE;
        static VmaAllocation zero_allocation = VK_NULL_HANDLE;
        static bool zero_filled = false;
        bool fill_now = false;
        if (!by_clear) {
            std::scoped_lock lock{zero_lock};
            if (!zero_buffer) {
                const VkBufferCreateInfo buffer_ci{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .size = kZeroBytes,
                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                };
                const VmaAllocationCreateInfo zero_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
                if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &zero_ci, &zero_buffer,
                                    &zero_allocation, nullptr) != VK_SUCCESS) {
                    zero_buffer = VK_NULL_HANDLE;
                }
            }
            if (zero_buffer && !zero_filled) {
                zero_filled = fill_now = true;
            }
        }
        if (by_clear || zero_buffer) {
            auto& scheduler = runtime->GetScheduler();
            scheduler.EndRendering();
            const vk::ImageSubresourceRange range{aspect_mask, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            std::vector<vk::BufferImageCopy> copies;
            if (!by_clear) {
                const vk::Extent3D extent = image_ci.extent;
                for (u32 level = 0; level < image_ci.mipLevels; ++level) {
                    copies.push_back(vk::BufferImageCopy{
                        .bufferOffset = 0,
                        .bufferRowLength = 0,
                        .bufferImageHeight = 0,
                        .imageSubresource{aspect_mask, level, 0, image_ci.arrayLayers},
                        .imageOffset{0, 0, 0},
                        .imageExtent{std::max(extent.width >> level, 1u), std::max(extent.height >> level, 1u),
                                     std::max(extent.depth >> level, 1u)},
                    });
                }
            }
            scheduler.Record([image = GetImage(), range, depth, by_clear, fill_now, copies = std::move(copies),
                              zero = vk::Buffer{zero_buffer}](vk::CommandBuffer cmdbuf) {
                if (fill_now) {
                    cmdbuf.fillBuffer(zero, 0, VK_WHOLE_SIZE, 0);
                    const vk::MemoryBarrier2 filled{
                        .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                    };
                    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &filled});
                }
                const vk::ImageMemoryBarrier2 barrier{
                    .srcStageMask = vk::PipelineStageFlagBits2::eNone,
                    .srcAccessMask = vk::AccessFlagBits2::eNone,
                    .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .oldLayout = vk::ImageLayout::eUndefined,
                    .newLayout = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image,
                    .subresourceRange = range,
                };
                cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                    .imageMemoryBarrierCount = 1,
                    .pImageMemoryBarriers = &barrier,
                });
                if (!by_clear) {
                    cmdbuf.copyBufferToImage(zero, image, vk::ImageLayout::eTransferDstOptimal, copies);
                } else if (depth) {
                    cmdbuf.clearDepthStencilImage(image, vk::ImageLayout::eTransferDstOptimal,
                                                  vk::ClearDepthStencilValue{0.0f, 0u}, range);
                } else {
                    cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal,
                                           vk::ClearColorValue{}, range);
                }
            });
            backing->state = State{
                .pl_stage = vk::PipelineStageFlagBits2::eAllTransfer,
                .access_mask = vk::AccessFlagBits2::eTransferWrite,
                .layout = vk::ImageLayout::eTransferDstOptimal,
            };
        }
    }

    Vulkan::SetObjectName(instance.GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

// XStation 2026-11-21 (twin.py in Bloodborne-AllocFallback-20261121): see there.
thread_local bool bb_twin_allowed = false;
static int BbTwinFamily(vk::Format f, u32& bytes) {
    using F = vk::Format;
    switch (f) {
    case F::eR32Sfloat: case F::eR32Uint: case F::eR32Sint:
        bytes = 4; return 1;
    case F::eR8G8B8A8Unorm: case F::eR8G8B8A8Snorm: case F::eR8G8B8A8Uint: case F::eR8G8B8A8Sint:
    case F::eR8G8B8A8Srgb: case F::eB8G8R8A8Unorm: case F::eB8G8R8A8Srgb: case F::eA8B8G8R8UnormPack32:
    case F::eA8B8G8R8SrgbPack32:
        bytes = 4; return 2;
    case F::eR16G16Unorm: case F::eR16G16Snorm: case F::eR16G16Uint: case F::eR16G16Sint: case F::eR16G16Sfloat:
        bytes = 4; return 3;
    case F::eA2B10G10R10UnormPack32: case F::eA2B10G10R10UintPack32: case F::eA2R10G10B10UnormPack32:
        bytes = 4; return 4;
    case F::eB10G11R11UfloatPack32:
        bytes = 4; return 5;
    case F::eR32G32Sfloat: case F::eR32G32Uint: case F::eR32G32Sint:
        bytes = 8; return 7;
    case F::eR16G16B16A16Unorm: case F::eR16G16B16A16Snorm: case F::eR16G16B16A16Uint:
    case F::eR16G16B16A16Sint: case F::eR16G16B16A16Sfloat:
        bytes = 8; return 8;
    case F::eR16Unorm: case F::eR16Snorm: case F::eR16Uint: case F::eR16Sint: case F::eR16Sfloat:
        bytes = 2; return 9;
    case F::eR8G8Unorm: case F::eR8G8Snorm: case F::eR8G8Uint: case F::eR8G8Sint:
        bytes = 2; return 10;
    default:
        bytes = 0; return 0;
    }
}

// True when a view of `view_format` on this image needs a twin (another cast family, same bytes per texel).
bool BbNeedsTwin(const Image& image, vk::Format view_format) {
    if (!image.backing || image.info.props.is_depth || image.info.num_samples > 1) {
        return false;
    }
    u32 image_bytes = 0, view_bytes = 0;
    const int image_family = BbTwinFamily(image.backing->image.image_ci.format, image_bytes);
    const int view_family = BbTwinFamily(view_format, view_bytes);
    return image_family && view_family && image_family != view_family && image_bytes == view_bytes;
}

namespace {
struct BbTwin {
    std::unique_ptr<Image> image;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    vk::DeviceSize bytes = 0;
};
std::mutex bb_twin_lock;
std::map<std::pair<u64, int>, BbTwin> bb_twins; // (image uid, view format)
} // namespace

// The twin's view, after recording the copy of this image's bits into it; nullptr: no twin (the caller goes on).
static ImageView* BbTwinView(Image& source, const ImageViewInfo& view_info) {
    std::scoped_lock lock{bb_twin_lock};
    const auto key = std::make_pair(source.image_uid, int(view_info.format));
    auto it = bb_twins.find(key);
    const vk::ImageCreateInfo& ci = source.backing->image.image_ci;
    u32 texel = 0;
    BbTwinFamily(view_info.format, texel);
    if (it == bb_twins.end()) {
        if (bb_twins.size() >= 64) {
            return nullptr;
        }
        vk::DeviceSize bytes = 0;
        for (u32 level = 0; level < ci.mipLevels; ++level) {
            bytes += vk::DeviceSize(std::max(ci.extent.width >> level, 1u)) * std::max(ci.extent.height >> level, 1u) *
                     std::max(ci.extent.depth >> level, 1u) * ci.arrayLayers * texel;
        }
        bytes = (bytes + 0xffff) & ~vk::DeviceSize(0xffff);
        BbTwin twin;
        ImageInfo twin_info = source.info;
        twin_info.pixel_format = view_info.format;
        twin.image = std::make_unique<Image>(source.runtime->GetInstance(), *source.runtime, *source.slot_image_views,
                                             twin_info, u32(source.usage.host_skip));
        const vk::ImageCreateInfo& tci = twin.image->backing->image.image_ci;
        if (tci.extent != ci.extent || tci.mipLevels != ci.mipLevels || tci.arrayLayers != ci.arrayLayers ||
            tci.format != view_info.format) {
            return nullptr; // another shape or a substituted format: no bit copy
        }
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bytes,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(source.runtime->GetInstance().GetAllocator(), &buffer_ci, &alloc_ci, &twin.buffer,
                            &twin.allocation, nullptr) != VK_SUCCESS) {
            return nullptr;
        }
        twin.bytes = bytes;
        std::printf("Image twin: %s reads of a %s image %ux%u go through a copy\n",
                    vk::to_string(view_info.format).c_str(), vk::to_string(ci.format).c_str(), ci.extent.width,
                    ci.extent.height);
        std::fflush(stdout);
        it = bb_twins.emplace(key, std::move(twin)).first;
    }
    BbTwin& twin = it->second;
    Image& target = *twin.image;
    auto& runtime = *source.runtime;
    runtime.GetScheduler().EndRendering();
    bool flush = runtime.Transit(&source, vk::ImageLayout::eTransferSrcOptimal,
                                 vk::PipelineStageFlagBits2::eAllTransfer, vk::AccessFlagBits2::eTransferRead);
    flush |= runtime.Transit(&target, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eAllTransfer,
                             vk::AccessFlagBits2::eTransferWrite);
    if (flush) {
        runtime.FlushBarriers();
    }
    std::vector<vk::BufferImageCopy> copies;
    vk::DeviceSize offset = 0;
    for (u32 level = 0; level < ci.mipLevels; ++level) {
        const vk::Extent3D extent{std::max(ci.extent.width >> level, 1u), std::max(ci.extent.height >> level, 1u),
                                  std::max(ci.extent.depth >> level, 1u)};
        copies.push_back(vk::BufferImageCopy{
            .bufferOffset = offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{vk::ImageAspectFlagBits::eColor, level, 0, ci.arrayLayers},
            .imageOffset{0, 0, 0},
            .imageExtent = extent,
        });
        offset += (vk::DeviceSize(extent.width) * extent.height * extent.depth * ci.arrayLayers * texel + 511) &
                  ~vk::DeviceSize(511);
        if (offset > twin.bytes) {
            copies.pop_back();
            break;
        }
    }
    runtime.GetScheduler().Record([src = source.GetImage(), dst = target.GetImage(), buffer = vk::Buffer{twin.buffer},
                                   copies](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(src, vk::ImageLayout::eTransferSrcOptimal, buffer, copies);
        const vk::BufferMemoryBarrier2 written{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &written});
        cmdbuf.copyBufferToImage(buffer, dst, vk::ImageLayout::eTransferDstOptimal, copies);
    });
    // The rasterizer binds the view in the layout it puts the source in for sampling.
    if (runtime.Transit(&target, vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead)) {
        runtime.FlushBarriers();
    }
    ImageViewInfo twin_view = view_info;
    return &target.FindView(twin_view, false);
}

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (bb_twin_allowed && !view_info.is_storage && BbNeedsTwin(*this, view_info.format)) {
        if (ImageView* twin = BbTwinView(*this, view_info)) {
            return *twin;
        }
    }
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
            for (u32 layer : layers) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
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
