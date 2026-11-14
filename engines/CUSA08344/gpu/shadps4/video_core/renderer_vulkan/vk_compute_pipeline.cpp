// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <string>
#include <boost/container/small_vector.hpp>
#include <fmt/format.h>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

void BbSubmitRingDump(u64 gpu_tick, u64 waited_tick); // vk_rasterizer.cpp

namespace {
/// Detroit (2026-11-08): a lost device makes every later creation fail as "out of host memory"
/// in Dozen (the Xbox: root signatures, pipelines). Says whether the device is still there, and
/// prints the recorded draws/dispatches (BB_SUBMIT_RING=1) when it is not.
void ReportCreationFailure(const Instance& instance, const char* what, const std::string& name) {
    const vk::Result idle = instance.GetDevice().waitIdle();
    std::printf("GPU: %s %s failed; device %s (waitIdle %s)\n", what, name.c_str(),
                idle == vk::Result::eErrorDeviceLost ? "LOST" : "present",
                vk::to_string(idle).c_str());
    std::fflush(stdout);
    // Dozen on the Xbox reports a removed device as ErrorOutOfDeviceMemory here (its fence creation
    // fails with DXGI_ERROR_DEVICE_REMOVED, xbox dt-x8): any failure gets the full report.
    if (idle != vk::Result::eSuccess) {
        instance.ReportDeviceLost(what);
        BbSubmitRingDump(0, 0);
    }
}
} // namespace

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata, bool preloading /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();
    // Detroit (2026-11-08), BB_NO_LAYOUT_CS (hex program hashes): not even the layout is made (the
    // Xbox removed the device, "invalid call", creating cs 0x6769a3fd's layout: xbox dt-x9).
    {
        static const std::string no_layout = [] {
            const char* env = std::getenv("BB_NO_LAYOUT_CS");
            return "," + std::string{env ? env : ""} + ",";
        }();
        if (no_layout.size() > 2 &&
            no_layout.find(fmt::format(",{:x},", info_.pgm_hash)) != std::string::npos) {
            LOG_WARNING(Render_Vulkan, "Compute pipeline {} and its layout not made (BB_NO_LAYOUT_CS); "
                        "its dispatches are skipped", debug_str);
            return;
        }
    }

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    if (layout_result != vk::Result::eSuccess) {
        ReportCreationFailure(instance, "compute pipeline layout for", debug_str);
    }
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
    // God of War port (2026-11-04), BB_NO_CREATE_CS (hex program hashes, comma-separated; the
    // Xbox entry sets 3eac35c9): never built, its dispatches skipped as after a failed build. The
    // Xbox's D3D12 fails cs 0x3eac35c9 at every boot (ErrorOutOfHostMemory, a C++ exception in
    // dxcompiler), and about half of the boots then died of heap corruption right after the
    // pipeline warm-up (c0000374; xbox-boot24/25/26/30).
    {
        static const std::string no_create = [] {
            const char* env = std::getenv("BB_NO_CREATE_CS");
            return "," + std::string{env ? env : ""} + ",";
        }();
        const auto key = fmt::format(",{:x},", info_.pgm_hash);
        if (no_create.size() > 2 && no_create.find(key) != std::string::npos) {
            LOG_WARNING(Render_Vulkan, "Compute pipeline {} not built (BB_NO_CREATE_CS); its "
                        "dispatches are skipped", debug_str);
            return;
        }
    }
    auto [pipeline_result, pipe] =
        instance.GetDevice().createComputePipelineUnique(pipeline_cache, compute_pipeline_ci);
    // bbport: as for graphics pipelines, a driver failure skips its dispatches.
    if (pipeline_result != vk::Result::eSuccess) {
        ReportCreationFailure(instance, "compute pipeline", debug_str);
        LOG_ERROR(Render_Vulkan, "Failed to create compute pipeline {}: {}; its dispatches are skipped",
                  debug_str, vk::to_string(pipeline_result));
        return;
    }
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Compute Pipeline {}", debug_str);
}

ComputePipeline::~ComputePipeline() = default;

} // namespace Vulkan
