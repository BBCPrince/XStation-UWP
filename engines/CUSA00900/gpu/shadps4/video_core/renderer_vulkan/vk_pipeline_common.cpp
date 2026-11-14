// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>
#include <cstdlib>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;


void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    // Null images are specialized to constants/no-ops by the recompiler. Leave
    // their statically unused bindings unwritten instead of submitting illegal
    // VK_NULL_HANDLE views on devices without robustness2.nullDescriptor.
    boost::container::small_vector<u32, 16> null_images;
    u32 binding = 0;
    for (const auto* stage : GetStages()) {
        if (!stage) continue;
        binding += stage->buffers.size();
        for (const auto& image : stage->images) {
            if (!image.GetSharp(*stage)) null_images.push_back(binding);
            binding += image.NumBindings(*stage);
        }
        binding += stage->samplers.size();
    }
    set_writes.erase(std::remove_if(set_writes.begin(), set_writes.end(), [&](const auto& write) {
        return (write.descriptorType == vk::DescriptorType::eSampledImage ||
                write.descriptorType == vk::DescriptorType::eStorageImage) &&
               std::find(null_images.begin(), null_images.end(), write.dstBinding) != null_images.end();
    }), set_writes.end());
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    static const bool defer_updates = [] {
        const char* flag = std::getenv("BB_DEFER_DESCRIPTOR_UPDATES");
        return flag && flag[0] == '1';
    }();
    if (uses_push_descriptors || (defer_updates && scheduler.IsRecordingDeferred())) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        if (uses_push_descriptors) {
            scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
            });
        } else {
            // Allocation stays on the decoder, the heap's sole owner. Copy the
            // descriptor payload into this chunk before returning: the next draw
            // overwrites the caller's arrays. Update precedes bind on the recorder;
            // SyncRecording still completes both before submission/pool retirement.
            const auto desc_set = desc_heap.Commit(*desc_layout);
            for (size_t i=0; i<writes.size(); ++i) patched[i].dstSet = desc_set;
            const auto device = instance.GetDevice();
            scheduler.Record([device, bind_point, layout, desc_set, writes](vk::CommandBuffer cmdbuf) {
                device.updateDescriptorSets(writes, {});
                cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
            });
        }
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
