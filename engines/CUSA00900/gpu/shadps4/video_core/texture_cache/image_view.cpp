// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

#include <magic_enum/magic_enum.hpp>
#include <cstdio>
#include <mutex>
#include <set>
#include <utility>

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

// XStation 2026-11-21: the D3D12 typeless family a format casts within (0 = not listed: left alone).
static int BbCastFamily(vk::Format f) {
    using F = vk::Format;
    switch (f) {
    case F::eR32Sfloat: case F::eR32Uint: case F::eR32Sint: case F::eD32Sfloat:
        return 1;
    case F::eR8G8B8A8Unorm: case F::eR8G8B8A8Snorm: case F::eR8G8B8A8Uint: case F::eR8G8B8A8Sint:
    case F::eR8G8B8A8Srgb: case F::eB8G8R8A8Unorm: case F::eB8G8R8A8Srgb: case F::eB8G8R8A8Snorm:
    case F::eB8G8R8A8Uint: case F::eB8G8R8A8Sint: case F::eA8B8G8R8UnormPack32: case F::eA8B8G8R8SnormPack32:
    case F::eA8B8G8R8UintPack32: case F::eA8B8G8R8SintPack32: case F::eA8B8G8R8SrgbPack32:
        return 2;
    case F::eR16G16Unorm: case F::eR16G16Snorm: case F::eR16G16Uint: case F::eR16G16Sint: case F::eR16G16Sfloat:
        return 3;
    case F::eA2B10G10R10UnormPack32: case F::eA2B10G10R10UintPack32: case F::eA2R10G10B10UnormPack32:
    case F::eA2R10G10B10UintPack32:
        return 4;
    case F::eB10G11R11UfloatPack32:
        return 5;
    case F::eE5B9G9R9UfloatPack32:
        return 6;
    case F::eR32G32Sfloat: case F::eR32G32Uint: case F::eR32G32Sint:
        return 7;
    case F::eR16G16B16A16Unorm: case F::eR16G16B16A16Snorm: case F::eR16G16B16A16Uint:
    case F::eR16G16B16A16Sint: case F::eR16G16B16A16Sfloat:
        return 8;
    case F::eR16Unorm: case F::eR16Snorm: case F::eR16Uint: case F::eR16Sint: case F::eR16Sfloat: case F::eD16Unorm:
        return 9;
    case F::eR8G8Unorm: case F::eR8G8Snorm: case F::eR8G8Uint: case F::eR8G8Sint: case F::eR8G8Srgb:
        return 10;
    case F::eR8Unorm: case F::eR8Snorm: case F::eR8Uint: case F::eR8Sint: case F::eR8Srgb:
        return 11;
    case F::eR32G32B32A32Sfloat: case F::eR32G32B32A32Uint: case F::eR32G32B32A32Sint:
        return 12;
    case F::eR5G6B5UnormPack16: case F::eB5G6R5UnormPack16:
        return 13;
    case F::eR5G5B5A1UnormPack16: case F::eB5G5R5A1UnormPack16: case F::eA1R5G5B5UnormPack16:
        return 14;
    case F::eR4G4B4A4UnormPack16: case F::eB4G4R4A4UnormPack16:
        return 15;
    default:
        return 0;
    }
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

    // XStation 2026-11-21 (view_cast.py in Bloodborne-AllocFallback-20261121): the Xbox's D3D12 removes the device on
    // a view in another cast family than the image (R32_SFLOAT viewed as R8G8B8A8_UNORM on a fast travel).
    {
        const vk::Format image_format = image.backing->image.image_ci.format;
        const int view_family = BbCastFamily(format), image_family = BbCastFamily(image_format);
        if (view_family && image_family && view_family != image_family) {
            static std::mutex noted_lock;
            static std::set<std::pair<int, int>> noted;
            std::scoped_lock lock{noted_lock};
            if (noted.emplace(int(format), int(image_format)).second) {
                std::printf("Image view: %s view of a %s image made as %s (no cast across D3D12 format families)\n",
                            vk::to_string(format).c_str(), vk::to_string(image_format).c_str(),
                            vk::to_string(image_format).c_str());
                std::fflush(stdout);
            }
            format = image_format;
        }
    }

    const vk::ImageViewCreateInfo image_view_ci = {
        .pNext = &usage_ci,
        .image = image.GetImage(),
        .viewType = ConvertImageViewType(info.type),
        .format = instance.GetSupportedFormat(format, image.format_features),
        .components = info.mapping,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = base_level,
            .levelCount = num_levels,
            .baseArrayLayer = info.range.base.layer,
            .layerCount = info.range.extent.layers,
        },
    };
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
