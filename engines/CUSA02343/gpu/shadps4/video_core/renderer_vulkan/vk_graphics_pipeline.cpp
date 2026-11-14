// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::Backend::SPIRV::AuxShaderType;

struct GraphicsPipeline::Variants {
    std::mutex lock;
    vk::Device device;
    vk::PipelineCache cache;
    vk::PipelineLayout layout;
    std::string debug;
    std::vector<vk::PipelineShaderStageCreateInfo> stages;
    vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{};
    bool use_subgroup{};
    bool has_vertex_input{};
    std::vector<vk::VertexInputAttributeDescription> attributes;
    std::vector<vk::VertexInputBindingDescription> bindings;
    std::vector<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    vk::PrimitiveTopology topology{};
    vk::PipelineTessellationStateCreateInfo tessellation{};
    vk::PipelineRasterizationStateCreateInfo raster{};
    bool provoking{};
    vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_info{};
    bool depth_clip{};
    vk::PipelineRasterizationDepthClipStateCreateInfoEXT depth_clip_info{};
    vk::PipelineMultisampleStateCreateInfo multisample{};
    bool clip_control{};
    vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control_info{};
    std::vector<vk::DynamicState> dynamic_states;
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats{};
    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
    bool use_mixed{};
    vk::AttachmentSampleCountInfoAMD mixed{};
    vk::PipelineRenderingCreateInfo rendering{};
    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments{};
    vk::PipelineColorBlendStateCreateInfo blend{};
    bool depth_bounds{};
    std::unordered_map<u64, vk::UniquePipeline> built;
    u64 last_key = ~0ULL;
    vk::Pipeline last{};
};

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const vk::ShaderModule> modules, SerializationSupport& sdata, bool preloading)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_},
      fetch_shader{std::move(fetch_shader_)} {
    const vk::Device device = instance.GetDevice();
    std::ranges::copy(infos, stages.begin());
    BuildDescSetLayout(preloading);
    const auto debug_str = GetDebugString();

    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);

    if (!preloading) {
        VertexInputs<AmdGpu::Buffer> guest_buffers;
        if (!instance.IsVertexInputDynamicState()) {
            const auto& vs_info = runtime_infos[u32(Shader::SwStage::Vertex)].sw.vs;
            GetVertexInputs(sdata.vertex_attributes, sdata.vertex_bindings, sdata.divisors,
                            guest_buffers, vs_info.step_rate_0, vs_info.step_rate_1);
        }
    }

    const vk::PipelineVertexInputDivisorStateCreateInfo divisor_state = {
        .vertexBindingDivisorCount = static_cast<u32>(sdata.divisors.size()),
        .pVertexBindingDivisors = sdata.divisors.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .pNext = sdata.divisors.empty() ? nullptr : &divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(sdata.vertex_bindings.size()),
        .pVertexBindingDescriptions = sdata.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(sdata.vertex_attributes.size()),
        .pVertexAttributeDescriptions = sdata.vertex_attributes.data(),
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    // Uncharted Xbox port (2026-11-05): rect lists drawn with a geometry shader (3 vertices in, a
    // 4-vertex strip out) instead of the tessellation emulation, which rasterises nothing on the
    // Xbox (Dozen on the console's D3D12/AMD): every full-screen RectList pass (exposure, depth
    // of field, half-res depth, the scene composite) wrote nothing and the picture was black.
    // The GS code is kept in sdata.tcs (no serialization change; PipelineKeyVersion bumped).
    // BB_RECT_LIST_GS=1 only (2026-11-05): the geometry shaders kill the Xbox build at the first
    // shader compile (xbox-qgs1/qgs2). Quad lists are triangle lists instead (QuadListAsTriangles).
    static const bool rect_gs_wanted = [] {
        const char* env = std::getenv("BB_RECT_LIST_GS");
        return env && env[0] == '1';
    }();
    const bool quad_tri = is_quad_list && QuadListAsTriangles() &&
                          !infos[u32(Shader::SwStage::Geometry)] &&
                          !infos[u32(Shader::SwStage::TessellationControl)] &&
                          !infos[u32(Shader::SwStage::TessellationEval)];
    // QuadList too (2026-11-05): Uncharted's full-screen passes are QuadLists (prim 19); a quad's
    // 4 indices form one lines-with-adjacency primitive for the geometry shader.
    const bool rect_gs = (is_rect_list || (is_quad_list && !quad_tri)) && rect_gs_wanted &&
                         instance.IsGeometryStageSupported() &&
                         !infos[u32(Shader::SwStage::Geometry)] &&
                         !infos[u32(Shader::SwStage::TessellationControl)] &&
                         !infos[u32(Shader::SwStage::TessellationEval)];
    if ((is_rect_list || is_quad_list) && !rect_gs && !quad_tri) {
        static std::atomic<u32> rect_tess_reported{0};
        if (rect_tess_reported.fetch_add(1, std::memory_order_relaxed) < 3) {
            std::printf("Rect/QuadList: tessellation path (quad %d, wanted %d, geometry shaders %d)\n",
                        int(is_quad_list), int(rect_gs_wanted),
                        int(instance.IsGeometryStageSupported()));
        }
    }
    const auto topology =
        quad_tri  ? vk::PrimitiveTopology::eTriangleList
        : rect_gs ? (is_quad_list ? vk::PrimitiveTopology::eLineListWithAdjacency
                                  : vk::PrimitiveTopology::eTriangleList)
                  : LiverpoolToVK::PrimitiveType(key.prim_type);
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = topology,
    };
    const vk::PipelineTessellationStateCreateInfo tessellation_state = {
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    vk::StructureChain raster_chain = {
        vk::PipelineRasterizationStateCreateInfo{
            .depthClampEnable = key.depth_clamp_enable &&
                                (!key.depth_clip_enable || instance.IsDepthClipEnableSupported()),
            .rasterizerDiscardEnable = false,
            .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
            .lineWidth = 1.0f,
        },
        vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT{
            .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                       ? vk::ProvokingVertexModeEXT::eFirstVertex
                                       : vk::ProvokingVertexModeEXT::eLastVertex,
        },
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT{
            .depthClipEnable = key.depth_clip_enable,
        },
    };

    if (!instance.IsProvokingVertexSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!instance.IsDepthClipEnableSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    if (!preloading) {
        const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
        sdata.multisampling = {
            .rasterizationSamples = LiverpoolToVK::NumSamples(
                key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
            .sampleShadingEnable =
                fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
        };
    }

    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control = {
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };

    const bool eds = instance.IsExtendedDynamicStateSupported();
    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .pNext = instance.IsDepthClipControlSupported() ? &clip_control : nullptr,
        .viewportCount = eds ? 0U : 1U,
        .scissorCount = eds ? 0U : 1U,
    };

    boost::container::static_vector<vk::DynamicState, 32> dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };

    if (!eds) {
        // Xbox port (2026-11-04): only Vulkan 1.0 dynamic state; the rest is baked (VariantHandle).
        dynamic_states = {
            vk::DynamicState::eViewport,           vk::DynamicState::eScissor,
            vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthBias,
            vk::DynamicState::eStencilReference,   vk::DynamicState::eStencilCompareMask,
            vk::DynamicState::eStencilWriteMask,   vk::DynamicState::eLineWidth,
        };
    }
    if (instance.IsDepthBoundsSupported()) {
        if (eds) {
            dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        }
        dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!sdata.vertex_bindings.empty() && eds) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
        shader_stages;
    auto stage = u32(Shader::SwStage::Vertex);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Geometry);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (rect_gs) {
        static std::atomic<u32> rect_gs_reported{0};
        if (rect_gs_reported.fetch_add(1, std::memory_order_relaxed) < 3) {
            std::printf("Rect/QuadList: geometry shader path (quad %d, preloading %d)\n",
                        int(is_quad_list), int(preloading));
        }
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
                is_quad_list ? AuxShaderType::QuadListGS : AuxShaderType::RectListGS, fs_info);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = CompileSPV(sdata.tcs, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationControl);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if ((is_rect_list || is_quad_list) && !rect_gs && !quad_tri) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs_info);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = CompileSPV(sdata.tcs, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationEval);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if ((is_rect_list || is_quad_list) && !rect_gs && !quad_tri) {
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
                AuxShaderType::PassthroughTES, fs_info);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = CompileSPV(sdata.tes, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Fragment);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.clip_distance_emulation) {
        if (!preloading) {
            const auto& vs = runtime_infos[static_cast<u32>(Shader::SwStage::Vertex)].hw.vs;

            sdata.fragment = Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs.outputs);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = CompileSPV(sdata.fragment, instance.GetDevice()),
            .pName = "main",
        });
    }

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    for (auto& stage : shader_stages) {
        stage.pNext = instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr;
    }

    const auto depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats;
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!instance.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        color_formats[i] = color_format;
    }

    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    std::ranges::transform(key.color_samples, color_samples.begin(), [&instance](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    const vk::AttachmentSampleCountInfoAMD mixed_samples = {
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .pNext = instance.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? depth_format
                                       : vk::Format::eUndefined,
    };

    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments;
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        // Vulkan ignores blend factors for min/max, but a factor that zeroes one operand
        // makes the operation collapse to a plain selection: min(s, 0) is 0 and max(s, 0)
        // is s for normalized alpha. Rewrite those to the equivalent add so the result is
        // exact instead of leaving the other operand to survive.
        auto eff_src_alpha = src_alpha;
        auto eff_dst_alpha = dst_alpha;
        auto eff_alpha_blend = alpha_blend;
        if (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) {
            const bool takes_max = alpha_blend == vk::BlendOp::eMax;
            if (src_alpha == vk::BlendFactor::eOne && dst_alpha == vk::BlendFactor::eZero) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
                eff_dst_alpha = vk::BlendFactor::eZero;
            } else if (src_alpha == vk::BlendFactor::eZero && dst_alpha == vk::BlendFactor::eOne) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = vk::BlendFactor::eZero;
                eff_dst_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
            }
        }

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne) &&
            !key.color_buffers[i].blend_self_scale;
        const auto alpha_scaled_min_max =
            (eff_alpha_blend == vk::BlendOp::eMin || eff_alpha_blend == vk::BlendOp::eMax) &&
            (eff_src_alpha != vk::BlendFactor::eOne || eff_dst_alpha != vk::BlendFactor::eOne);
        if (color_scaled_min_max || alpha_scaled_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = eff_src_alpha,
            .dstAlphaBlendFactor = eff_dst_alpha,
            .alphaBlendOp = eff_alpha_blend,
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // The shader squares its color output for this attachment (see PsColorBuffer), so the
        // factors must not scale the operands again.
        if (key.color_buffers[i].blend_self_scale) {
            LOG_WARNING(
                Render_Vulkan,
                "Emulating scaled min/max blend with squared shader output on attachment {}", i);
            attachments[i].srcColorBlendFactor = vk::BlendFactor::eOne;
            attachments[i].dstColorBlendFactor = vk::BlendFactor::eOne;
        }

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it.
    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil_info = {};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &vertex_input_info : nullptr,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = &tessellation_state,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_chain.get(),
        .pMultisampleState = &sdata.multisampling,
        .pDepthStencilState =
            !instance.IsExtendedDynamicState3Supported() ? &depth_stencil_info : nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

    if (!eds) {
        auto v = std::make_unique<Variants>();
        v->device = device;
        v->cache = pipeline_cache;
        v->layout = *pipeline_layout;
        v->debug = debug_str;
        v->stages.assign(shader_stages.begin(), shader_stages.end());
        v->subgroup = subgroup_size_ci;
        v->use_subgroup = instance.IsSubgroupSize64Supported();
        v->has_vertex_input = !instance.IsVertexInputDynamicState();
        v->attributes.assign(sdata.vertex_attributes.begin(), sdata.vertex_attributes.end());
        v->bindings.assign(sdata.vertex_bindings.begin(), sdata.vertex_bindings.end());
        v->divisors.assign(sdata.divisors.begin(), sdata.divisors.end());
        v->topology = topology;
        v->tessellation = tessellation_state;
        v->raster = raster_chain.get<vk::PipelineRasterizationStateCreateInfo>();
        v->provoking = instance.IsProvokingVertexSupported();
        v->provoking_info = raster_chain.get<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
        v->depth_clip = instance.IsDepthClipEnableSupported();
        v->depth_clip_info = raster_chain.get<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
        v->multisample = sdata.multisampling;
        v->clip_control = instance.IsDepthClipControlSupported();
        v->clip_control_info = clip_control;
        v->dynamic_states.assign(dynamic_states.begin(), dynamic_states.end());
        v->color_formats = color_formats;
        v->color_samples = color_samples;
        v->use_mixed = instance.IsMixedDepthSamplesSupported();
        v->mixed = mixed_samples;
        v->rendering = pipeline_rendering_ci;
        v->attachments = attachments;
        v->blend = color_blending;
        v->depth_bounds = instance.IsDepthBoundsSupported();
        variants = std::move(v);
    }

    auto [pipeline_result, pipe] =
        device.createGraphicsPipelineUnique(pipeline_cache, pipeline_info);
    if (pipeline_result != vk::Result::eSuccess) {
        // bbport (Mevlock, drakolordx7): some drivers refuse a pipeline only because of the
        // pipeline cache (ErrorInitializationFailed on a Radeon RX 9070 XT at start): try once
        // more without it.
        auto [retry_result, retry_pipe] = device.createGraphicsPipelineUnique(nullptr, pipeline_info);
        if (retry_result == vk::Result::eSuccess) {
            LOG_WARNING(Render_Vulkan,
                        "Graphics pipeline {} was refused with the pipeline cache ({}); created without it",
                        debug_str, vk::to_string(pipeline_result));
            pipeline_result = retry_result;
            pipe = std::move(retry_pipe);
        }
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
}

GraphicsPipeline::~GraphicsPipeline() = default;


vk::Pipeline GraphicsPipeline::VariantHandle(const DynamicState& s,
                                             std::span<const vk::DeviceSize> strides) const {
    if (!variants) {
        return Handle();
    }
    auto& v = *variants;
    struct Packed {
        u8 viewports, scissors, cull, front_face;
        u8 depth_test, depth_write, depth_bounds_test, depth_bias;
        u8 stencil_test, restart, discard, pad;
        u32 depth_compare;
        StencilOps front, back;
    } p{};
    p.viewports = static_cast<u8>(std::max<size_t>(s.viewports.size(), 1));
    p.scissors = static_cast<u8>(std::max<size_t>(s.scissors.size(), 1));
    p.cull = static_cast<u8>(static_cast<u32>(s.cull_mode));
    p.front_face = static_cast<u8>(s.front_face);
    p.depth_test = s.depth_test_enabled;
    p.depth_write = s.depth_test_enabled && s.depth_write_enabled;
    p.depth_compare = s.depth_test_enabled ? static_cast<u32>(s.depth_compare_op)
                                           : static_cast<u32>(vk::CompareOp::eAlways);
    p.depth_bounds_test = v.depth_bounds && s.depth_bounds_test_enabled;
    p.depth_bias = s.depth_bias_enabled;
    p.stencil_test = s.stencil_test_enabled;
    if (s.stencil_test_enabled) {
        p.front = s.stencil_front_ops;
        p.back = s.stencil_back_ops;
    }
    p.restart = s.primitive_restart_enable;
    p.discard = s.rasterizer_discard_enable;
    std::array<u32, 32> used_strides{};
    for (size_t i = 0; i < v.bindings.size() && i < used_strides.size(); ++i) {
        const u32 slot = v.bindings[i].binding;
        used_strides[i] = slot < strides.size() ? static_cast<u32>(strides[slot]) : v.bindings[i].stride;
    }
    const u64 hash = XXH3_64bits_withSeed(used_strides.data(), v.bindings.size() * sizeof(u32),
                                          XXH3_64bits(&p, sizeof(p)));

    std::scoped_lock guard{v.lock};
    if (hash == v.last_key) {
        return v.last;
    }
    if (const auto it = v.built.find(hash); it != v.built.end()) {
        v.last_key = hash;
        v.last = *it->second;
        return v.last;
    }

    auto bindings = v.bindings;
    for (size_t i = 0; i < bindings.size() && i < used_strides.size(); ++i) {
        bindings[i].stride = used_strides[i];
    }
    const vk::PipelineVertexInputDivisorStateCreateInfo divisor_state = {
        .vertexBindingDivisorCount = static_cast<u32>(v.divisors.size()),
        .pVertexBindingDivisors = v.divisors.data(),
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input = {
        .pNext = v.divisors.empty() ? nullptr : &divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(bindings.size()),
        .pVertexBindingDescriptions = bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(v.attributes.size()),
        .pVertexAttributeDescriptions = v.attributes.data(),
    };
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = v.topology,
        .primitiveRestartEnable = p.restart,
    };
    auto provoking = v.provoking_info;
    auto depth_clip = v.depth_clip_info;
    const void* chain = nullptr;
    if (v.depth_clip) {
        depth_clip.pNext = nullptr;
        chain = &depth_clip;
    }
    if (v.provoking) {
        provoking.pNext = chain;
        chain = &provoking;
    }
    auto raster = v.raster;
    raster.pNext = chain;
    raster.cullMode = s.cull_mode;
    raster.frontFace = s.front_face;
    raster.rasterizerDiscardEnable = p.discard;
    raster.depthBiasEnable = p.depth_bias;
    auto clip_control = v.clip_control_info;
    const vk::PipelineViewportStateCreateInfo viewport = {
        .pNext = v.clip_control ? &clip_control : nullptr,
        .viewportCount = p.viewports,
        .scissorCount = p.scissors,
    };
    const auto op_state = [](const StencilOps& o) {
        return vk::StencilOpState{.failOp = o.fail_op, .passOp = o.pass_op,
                                  .depthFailOp = o.depth_fail_op, .compareOp = o.compare_op};
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_stencil = {
        .depthTestEnable = p.depth_test,
        .depthWriteEnable = p.depth_write,
        .depthCompareOp = static_cast<vk::CompareOp>(p.depth_compare),
        .depthBoundsTestEnable = p.depth_bounds_test,
        .stencilTestEnable = p.stencil_test,
        .front = op_state(p.front),
        .back = op_state(p.back),
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 1.0f,
    };
    auto stages = v.stages;
    for (auto& stage : stages) {
        stage.pNext = v.use_subgroup ? &v.subgroup : nullptr;
    }
    auto mixed = v.mixed;
    mixed.pColorAttachmentSamples = v.color_samples.data();
    auto rendering = v.rendering;
    rendering.pNext = v.use_mixed ? &mixed : nullptr;
    rendering.pColorAttachmentFormats = v.color_formats.data();
    auto blend = v.blend;
    blend.pAttachments = v.attachments.data();
    const auto multisample = v.multisample;
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(v.dynamic_states.size()),
        .pDynamicStates = v.dynamic_states.data(),
    };
    const vk::GraphicsPipelineCreateInfo info = {
        .pNext = &rendering,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = v.has_vertex_input ? &vertex_input : nullptr,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = &v.tessellation,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic_info,
        .layout = v.layout,
    };
    auto [result, pipe] = v.device.createGraphicsPipelineUnique(v.cache, info);
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Pipeline variant of {} failed: {}", v.debug, vk::to_string(result));
        return Handle();
    }
    const vk::Pipeline handle = *pipe;
    v.built.emplace(hash, std::move(pipe));
    v.last_key = hash;
    v.last = handle;
    static std::atomic<u64> total{0};
    const u64 n = ++total;
    if ((n & (n - 1)) == 0) {
        LOG_INFO(Render_Vulkan, "Pipeline variants without extended dynamic state: {}", n);
    }
    return handle;
}


template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::SwStage::Vertex);
    for (const auto& attrib : fetch_shader->attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        attributes.push_back(Attribute{
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = attrib.semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;

void GraphicsPipeline::BuildDescSetLayout(bool preloading) {
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto stage_bit = LogicalStageToStageBit[u32(stage->sw_stage)];
        for (const auto& buffer : stage->buffers) {
            (void)buffer; // the V# is not needed for the layout (and reading it may touch memory)
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [layout_result, layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics descriptor set layout: {}", vk::to_string(layout_result));
    desc_layout = std::move(layout);
}

} // namespace Vulkan
