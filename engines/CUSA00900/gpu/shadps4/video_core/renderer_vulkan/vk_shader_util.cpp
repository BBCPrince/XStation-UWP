// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

void DispatchComputeBatches(vk::CommandBuffer cmd, vk::PipelineLayout layout,
                            u32 x, u32 y, u32 z, u32 max_x) {
    ASSERT(max_x != 0);
    u32 base = 0;
    do {
        const u32 count = std::min(x - base, max_x);
        const std::array<u32, 2> grid{base, x};
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute,
                          offsetof(Shader::PushData, motion_param), sizeof(grid), grid.data());
        cmd.dispatch(count, y, z);
        base += count;
        if (base < x) {
            const vk::MemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        }
    } while (base < x);
}

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    return module;
}

} // namespace Vulkan
