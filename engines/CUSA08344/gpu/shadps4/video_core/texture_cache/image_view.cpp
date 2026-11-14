// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include <vulkan/vulkan_format_traits.hpp>

#include <magic_enum/magic_enum.hpp>

namespace VideoCore {

vk::ImageViewType ConvertImageViewType(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return vk::ImageViewType::e1D;
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageViewType::e1DArray;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return vk::ImageViewType::e2D;
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageViewType::e2DArray;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageViewType::e3D;
    default:
        UNREACHABLE();
    }
}

bool IsViewTypeCompatible(AmdGpu::ImageType view_type, AmdGpu::ImageType image_type) {
    switch (view_type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return image_type == AmdGpu::ImageType::Color1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DMsaaArray:
        return image_type == AmdGpu::ImageType::Color2D || image_type == AmdGpu::ImageType::Color3D;
    case AmdGpu::ImageType::Color3D:
        return image_type == AmdGpu::ImageType::Color3D;
    default:
        UNREACHABLE();
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept
    : is_storage{desc.is_written} {
    const auto dfmt = image.GetDataFmt();
    auto nfmt = image.GetNumberFmt();
    if (is_storage && nfmt == AmdGpu::NumberFormat::Srgb) {
        nfmt = AmdGpu::NumberFormat::Unorm;
    }
    format = Vulkan::LiverpoolToVK::SurfaceFormat(dfmt, nfmt);
    if (desc.is_depth) {
        format = Vulkan::LiverpoolToVK::PromoteFormatToDepth(format);
    }

    range.base.level = image.base_level;
    range.base.layer = std::min<u32>(image.base_array, image.NumLayers() - 1);
    range.extent.levels = image.NumViewLevels(desc.is_array);
    range.extent.layers = image.NumViewLayers(desc.is_array);
    type = image.GetViewType(desc.is_array);
    min_lod = static_cast<u32>(image.min_lod);

    if (!is_storage) {
        mapping = Vulkan::LiverpoolToVK::ComponentMapping(image.DstSelect());
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept {
    range.base.layer = col_buffer.BaseSlice();
    range.extent.layers = col_buffer.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
    format =
        Vulkan::LiverpoolToVK::SurfaceFormat(col_buffer.GetDataFmt(), col_buffer.GetNumberFmt());
}

ImageViewInfo::ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                             AmdGpu::DepthControl ctl) {
    format = Vulkan::LiverpoolToVK::DepthFormat(depth_buffer.z_info.format,
                                                depth_buffer.stencil_info.format);
    is_storage = ctl.depth_write_enable;
    range.base.layer = view.slice_start;
    range.extent.layers = view.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
}

ImageView::ImageView(const Vulkan::Instance& instance, const ImageViewInfo& info_,
                     const Image& image)
    : info{info_} {
    vk::ImageViewUsageCreateInfo usage_ci{.usage = image.usage_flags};
    if (!info.is_storage) {
        usage_ci.usage &= ~vk::ImageUsageFlagBits::eStorage;
    }
    // bbport (2026-10-28): reduced texture detail. The view stays in guest levels (info); the
    // Vulkan view is made in the image's, which has host_skip fewer finest levels.
    u32 base_level = info.range.base.level;
    u32 num_levels = info.range.extent.levels;
    float min_lod = static_cast<float>(info.min_lod) / 256.f;
    if (const u32 skip = image.usage.host_skip) {
        const u32 end = base_level + num_levels;
        base_level = base_level > skip ? base_level - skip : 0;
        num_levels = end > skip + base_level ? end - skip - base_level : 1;
        min_lod = std::max(min_lod - static_cast<float>(skip), 0.f);
    }
    vk::ImageViewMinLodCreateInfoEXT min_lod_ci{};
    if (min_lod != 0.f && instance.IsImageViewMinLodSupported()) {
        const float last_level = static_cast<float>(base_level + num_levels - 1);
        min_lod_ci.minLod = std::min(min_lod, last_level);
        usage_ci.pNext = &min_lod_ci;
    }
    // When sampling D32/D16 texture from shader, the T# specifies R32/R16 format so adjust it.
    vk::Format format = info.format;
    vk::ImageAspectFlags aspect = image.aspect_mask;
    if (image.aspect_mask & vk::ImageAspectFlagBits::eDepth &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eDepth;
    }
    if (image.aspect_mask & vk::ImageAspectFlagBits::eStencil &&
        Vulkan::LiverpoolToVK::IsFormatStencilCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eStencil;
    }

    // bbport: storage writes never convert to sRGB and sRGB formats cannot be storage images
    // (AMD's driver loses the device on such a view; NVIDIA tolerates it): write through the
    // UNORM format, which stores the same bytes.
    if (info.is_storage) {
        switch (format) {
        case vk::Format::eR8G8B8A8Srgb:
            format = vk::Format::eR8G8B8A8Unorm;
            break;
        case vk::Format::eB8G8R8A8Srgb:
            format = vk::Format::eB8G8R8A8Unorm;
            break;
        case vk::Format::eA8B8G8R8SrgbPack32:
            format = vk::Format::eA8B8G8R8UnormPack32;
            break;
        case vk::Format::eR8Srgb:
            format = vk::Format::eR8Unorm;
            break;
        case vk::Format::eR8G8Srgb:
            format = vk::Format::eR8G8Unorm;
            break;
        default:
            break;
        }
    }

    // God of War port (2026-11-03): D3D12 has no blue-first 10:10:10:2 format, so Dozen cannot
    // view A2R10G10B10 (God of War samples its A2B10G10R10 targets that way; DXGI UNKNOWN removed
    // the device). A view in the red-first format with red and blue swapped reads the same values.
    vk::ComponentMapping components = info.mapping;
    const auto sampleable = [&](vk::Format f) {
        return bool(instance.GetPhysicalDevice().getFormatProperties(f).optimalTilingFeatures &
                    vk::FormatFeatureFlagBits::eSampledImage);
    };
    if ((format == vk::Format::eA2R10G10B10UnormPack32 || format == vk::Format::eA2R10G10B10UintPack32) &&
        !sampleable(format)) {
        format = format == vk::Format::eA2R10G10B10UnormPack32 ? vk::Format::eA2B10G10R10UnormPack32
                                                               : vk::Format::eA2B10G10R10UintPack32;
        const auto swap_rb = [](vk::ComponentSwizzle s, vk::ComponentSwizzle self) {
            if (s == vk::ComponentSwizzle::eIdentity) {
                s = self;
            }
            return s == vk::ComponentSwizzle::eR   ? vk::ComponentSwizzle::eB
                   : s == vk::ComponentSwizzle::eB ? vk::ComponentSwizzle::eR
                                                   : s;
        };
        components = {swap_rb(components.r, vk::ComponentSwizzle::eR), swap_rb(components.g, vk::ComponentSwizzle::eG),
                      swap_rb(components.b, vk::ComponentSwizzle::eB), swap_rb(components.a, vk::ComponentSwizzle::eA)};
    }

    // God of War port (2026-11-03): within the image's layers (an array capped at the device's
    // layer limit has fewer than the guest's view asks for).
    const u32 image_layers = std::max(image.info.resources.layers, 1u);
    const u32 base_layer = std::min(info.range.base.layer, image_layers - 1);
    u32 layer_count = std::min(info.range.extent.layers, image_layers - base_layer);
    // Spider-Man port (2026-11-05): D3D12 casts a block-compressed image to an uncompressed view
    // (or back) only for ONE mip and ONE array slice; Spider-Man's 384-layer, 6-mip BC texture
    // array viewed as uint removed the device entering a new game (CreateShaderResourceView /
    // CreateUnorderedAccessView INVALIDDIMENSIONS, sm-pc81). Such views are clamped to their first
    // level and layer. BB_BC_CAST_CLAMP=0: off.
    {
        static const bool clamp = [] {
            const char* env = std::getenv("BB_BC_CAST_CLAMP");
            return !(env && env[0] == '0');
        }();
        const vk::Format view_format = instance.GetSupportedFormat(format, image.format_features);
        if (clamp && vk::isCompressed(image.info.pixel_format) != vk::isCompressed(view_format) &&
            (num_levels > 1 || layer_count > 1)) {
            static std::atomic<int> said{0};
            if (said.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::printf("GPU: %s view (storage %d) of a %s image %ux%u, %u layers, %u levels: "
                            "clamped to 1 level and 1 layer (D3D12 format casting)\n",
                            vk::to_string(view_format).c_str(), int(info.is_storage),
                            vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                            image.info.size.height, layer_count, num_levels);
                std::fflush(stdout);
            }
            num_levels = 1;
            layer_count = 1;
        }
    }
    const vk::ImageViewCreateInfo image_view_ci = {
        .pNext = &usage_ci,
        .image = image.GetImage(),
        .viewType = ConvertImageViewType(info.type),
        .format = instance.GetSupportedFormat(format, image.format_features),
        .components = components,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = base_level,
            .levelCount = num_levels,
            .baseArrayLayer = base_layer,
            .layerCount = layer_count,
        },
    };
    // God of War port (2026-11-03): a view format the driver cannot sample (Dozen maps it to
    // DXGI UNKNOWN and D3D12 removes the device), said once per pair of view and image format.
    {
        const auto props = instance.GetPhysicalDevice().getFormatProperties(image_view_ci.format);
        if (!(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)) {
            static std::mutex said_lock;
            static std::vector<std::pair<vk::Format, vk::Format>> said;
            std::scoped_lock lock{said_lock};
            const auto pair = std::make_pair(image_view_ci.format, image.info.pixel_format);
            if (std::find(said.begin(), said.end(), pair) == said.end()) {
                said.push_back(pair);
                std::printf("GPU: view format %s (asked %s, storage %d) of a %s image %ux%u is not sampleable here\n",
                            vk::to_string(image_view_ci.format).c_str(), vk::to_string(info.format).c_str(),
                            int(info.is_storage), vk::to_string(image.info.pixel_format).c_str(),
                            image.info.size.width, image.info.size.height);
                std::fflush(stdout);
            }
        }
    }
    if (!IsViewTypeCompatible(info.type, image.info.type)) {
        LOG_ERROR(Render_Vulkan, "image view type {} is incompatible with image type {}",
                  magic_enum::enum_name(info.type), magic_enum::enum_name(image.info.type));
    }

    auto [view_result, view] = instance.GetDevice().createImageViewUnique(image_view_ci);
    ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(view_result));
    image_view = std::move(view);

    const auto view_aspect = aspect & vk::ImageAspectFlagBits::eDepth     ? "Depth"
                             : aspect & vk::ImageAspectFlagBits::eStencil ? "Stencil"
                                                                          : "Color";
    Vulkan::SetObjectName(
        instance.GetDevice(), *image_view, "ImageView {}x{}x{} {:#x}:{:#x} {}:{} {}:{} ({})",
        image.info.size.width, image.info.size.height, image.info.size.depth,
        image.info.guest_address, image.info.guest_size, info.range.base.level,
        info.range.base.level + info.range.extent.levels - 1, info.range.base.layer,
        info.range.base.layer + info.range.extent.layers - 1, view_aspect);
}

ImageView::~ImageView() = default;

} // namespace VideoCore
