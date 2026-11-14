// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <unordered_map>
#include <miniz.h>
#include <ranges>
#include <string>
#include <unordered_set>

#include "common/hash.h"
#include "common/io_file.h"
#include "common/serdes.h"
#include "common/path_util.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "bbport_overlay.h"
#include "bbport_shader_jobs.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_baked_state.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_areas.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

extern thread_local bool g_shader_prebuild_test;
extern thread_local const std::vector<unsigned>* g_shader_fake_flat;
extern thread_local const void* g_shader_fake_fetch;
extern thread_local void (*g_shader_fake_sharps)(Shader::Info&);

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        info.hw.cs.batched = batched_compute;
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

struct PipelineCache::BuildWorkers {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<GraphicsPipeline*> queue;
    std::deque<GraphicsPipeline*> low; ///< BB_PIPELINE_AREAS: an area's saved ones, after the rest
    bool low_busy = false;             ///< a worker builds one of `low` (one at a time)
    u32 low_rest_ms = 4;               ///< BB_AREA_BUILD_REST
    std::vector<std::jthread> threads;

    explicit BuildWorkers(std::atomic<u32>& done) {
        if (const char* env = std::getenv("BB_AREA_BUILD_REST"); env && *env) {
            low_rest_ms = u32(std::strtoul(env, nullptr, 10));
        }
        // A few cores: the game's own threads and the GPU thread keep the rest.
        const u32 count = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
        for (u32 i = 0; i < count; ++i) {
            threads.emplace_back([this, &done](std::stop_token token) {
                Common::SetCurrentThreadName("bbport:PipelineBuild");
                while (true) {
                    GraphicsPipeline* pipeline;
                    bool is_low = false;
                    {
                        std::unique_lock lock{mutex};
                        Common::CondvarWait(cv, lock, token,
                                            [this] { return !queue.empty() || !low.empty(); });
                        if (token.stop_requested()) {
                            return;
                        }
                        // An area's saved pipelines: one thread at a time, with a rest after
                        // each (all at once, 616 of them stuttered the game for 3 s on the Xbox).
                        if (queue.empty() && low_busy) {
                            lock.unlock();
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            continue;
                        }
                        is_low = queue.empty();
                        auto& from = is_low ? low : queue;
                        pipeline = from.front();
                        from.pop_front();
                        if (is_low) {
                            low_busy = true;
                        }
                    }
                    pipeline->BuildDeferred();
                    done.fetch_add(1, std::memory_order_release);
                    if (is_low) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(low_rest_ms));
                        std::scoped_lock lock{mutex};
                        low_busy = false;
                    }
                }
            });
        }
        std::printf("Pipelines: built in the background on %u threads\n", count);
    }

    void Submit(GraphicsPipeline* pipeline, bool low_priority = false) {
        {
            std::scoped_lock lock{mutex};
            (low_priority ? low : queue).push_back(pipeline);
        }
        cv.notify_one();
    }
};

/// TLOU BB_PIPELINE_AREAS, GPU thread: when the game reads another area, its saved pipelines are
/// set up here as the preload did (preloading: the saved vertex input, no descriptors read) and
/// built by the workers after any pipeline a draw waits for. One the driver cannot build is
/// dropped, to be made again at a draw like any new pipeline.
void PipelineCache::BuildAreaPipelines() {
    auto& s = *area_state;
    if (!s.building.empty() && async_done.load(std::memory_order_acquire) != s.seen_builds) {
        s.seen_builds = async_done.load(std::memory_order_acquire);
        for (size_t i = 0; i < s.building.size();) {
            if (!s.building[i]->IsBuilt()) {
                ++i;
                continue;
            }
            if (s.building[i]->IsValid()) {
                ++s.built;
            } else {
                ++s.failed;
                graphics_pipelines.erase(s.building_keys[i]);
            }
            s.building[i] = s.building.back();
            s.building.pop_back();
            s.building_keys[i] = s.building_keys.back();
            s.building_keys.pop_back();
            if (s.building.empty()) {
                std::printf("Pipeline areas: %llu built, %llu failed so far\n",
                            static_cast<unsigned long long>(s.built),
                            static_cast<unsigned long long>(s.failed));
                std::fflush(stdout);
            }
        }
    }
    const u32 epoch = PipelineAreas::Get().epoch.load(std::memory_order_acquire);
    if (epoch == s.seen_epoch) {
        return;
    }
    s.seen_epoch = epoch;
    const auto name = PipelineAreas::Current();
    const int id = s.AreaId(name);
    if (id < 0) {
        std::printf("Pipeline areas: the game reads %s (not in the index)\n", name.c_str());
        std::fflush(stdout);
        return;
    }
    if (!build_workers) {
        build_workers = std::make_unique<BuildWorkers>(async_done);
    }
    u32 count = 0;
    for (const u32 index : s.by_area[id]) {
        auto& d = s.deferred[index];
        if (d.queued) {
            continue;
        }
        d.queued = true;
        const auto [it, is_new] = graphics_pipelines.try_emplace(d.key);
        if (!is_new) {
            continue; // a draw made it already
        }
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, d.key, *pipeline_cache, d.infos,
            d.runtime_infos, d.fetch_shader, d.modules, d.sdata, true, true);
        s.building.push_back(it.value().get());
        s.building_keys.push_back(d.key);
        build_workers->Submit(it.value().get(), true);
        ++count;
    }
    s.queued += count;
    std::printf("Pipeline areas: the game reads %s: %u saved pipelines queued\n", name.c_str(), count);
    std::fflush(stdout);
}

// bbport: background shader translation (BB_ASYNC_SHADERS, see vk_pipeline_cache.h). A job holds
// what the translation needs from the draw: the user-data registers (copied: the GPU thread
// rewrites them every draw) and the code (in guest memory; its address also gives the program
// base for inline constants), and reads the rest through them while the guest keeps that memory
// as the draw left it, until the next guest-visible signal (bbport_shader_jobs.h).
struct PipelineCache::TranslateJob {
    Shader::HwStage hw_stage;
    Shader::SwStage sw_stage;
    u64 hash;
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data;
    std::span<const u32> code;
    Shader::RuntimeInfo runtime_info{}; // aggregate: its default constructor is deleted
    Shader::Backend::Bindings start{};
    bool new_program;
    size_t perm_idx;
    std::optional<Shader::StageSpecialization> spec; ///< a permutation's: made at the draw
    // results
    std::unique_ptr<Program> program; ///< a new program, translated
    vk::ShaderModule module{};
    std::vector<u32> spv;
    bool failed = false;
    std::atomic<bool> done{false};
    // BB_ASYNC_VERTEX: Guest reads tables through the user data (BB_ASYNC_SHADERS); Snapshot
    // translates from the copies below; Walker only learns a new program's SRT walker.
    enum class Kind : u8 { Guest, Snapshot, Walker } kind = Kind::Guest;
    std::vector<u32> flat;                                ///< the draw's flat user data
    std::optional<Shader::Gcn::FetchShaderData> fetch;    ///< the draw's fetch shader
    std::array<std::vector<u32>, Shader::ShaderParams::NumShaderUserData> tables; ///< vertex V# tables
    std::vector<u8> walker; ///< result: the SRT walker this translation generated
    std::vector<u32> check_spv;  ///< BB_ASYNC_VERTEX_CHECK: the same translated at the draw
    std::vector<u8> check_walker;
    u32 flat_dw = 0;
    bool has_fetch = false;
    u32 fetch_sgpr = 0;

    Shader::ShaderParams Params() const {
        return {std::span<const u32, Shader::ShaderParams::NumShaderUserData>(user_data), code,
                hash};
    }
};

struct PipelineCache::TranslateWorkers {
    PipelineCache& cache;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<TranslateJob*> queue;
    std::vector<std::jthread> threads;
    static inline std::atomic<TranslateWorkers*> current{nullptr};

    explicit TranslateWorkers(PipelineCache& cache_) : cache{cache_} {
        // Two on the Xbox (8 hardware threads): the game's threads and the GPU thread keep the
        // rest; a thread that waits for them (a guest signal) translates too.
        u32 count = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u);
        if (const char* env = std::getenv("BB_TRANSLATE_THREADS"); env && *env) {
            count = std::clamp(u32(std::strtoul(env, nullptr, 10)), 1u, 4u);
        }
        current.store(this, std::memory_order_release);
        BbShaderJobs::help.store(&HelpOne, std::memory_order_release);
        for (u32 i = 0; i < count; ++i) {
            threads.emplace_back([this](std::stop_token token) {
                Common::SetCurrentThreadName("bbport:ShaderTranslate");
                while (true) {
                    TranslateJob* job;
                    {
                        std::unique_lock lock{mutex};
                        Common::CondvarWait(cv, lock, token, [this] { return !queue.empty(); });
                        if (token.stop_requested()) {
                            return;
                        }
                        job = queue.front();
                        queue.pop_front();
                    }
                    Run(*job);
                }
            });
        }
        std::printf("Shaders: translated in the background on %u threads\n", count);
        std::fflush(stdout);
    }
    ~TranslateWorkers() {
        BbShaderJobs::help.store(nullptr, std::memory_order_release);
        current.store(nullptr, std::memory_order_release);
    }

    void Run(TranslateJob& job) {
        const bool guest = job.kind == TranslateJob::Kind::Guest; // read before done: then it may go
        cache.RunTranslation(job);
        job.done.store(true, std::memory_order_release);
        cache.translate_done.fetch_add(1, std::memory_order_release);
        if (guest) {
            BbShaderJobs::Finished(); // only those count for guest signals
        }
    }
    bool RunOne() {
        TranslateJob* job = nullptr;
        {
            std::scoped_lock lock{mutex};
            if (queue.empty()) {
                return false;
            }
            job = queue.front();
            queue.pop_front();
        }
        Run(*job);
        return true;
    }
    static bool HelpOne() {
        auto* workers = current.load(std::memory_order_acquire);
        return workers && workers->RunOne();
    }
    void Submit(TranslateJob* job) {
        {
            std::scoped_lock lock{mutex};
            queue.push_back(job);
        }
        cv.notify_one();
    }
};

namespace {
bool AsyncShaders();
}

/// The translation the GPU thread would have done at the draw, on this thread. A fault (a table
/// pointer into memory the GPU wrote and the CPU may not read now) fails the job: that program
/// is then translated at its next draw on the GPU thread, as before.
void PipelineCache::RunTranslation(TranslateJob& job) {
    thread_local Shader::Pools job_pools;
    using Kind = TranslateJob::Kind;
    // BB_ASYNC_VERTEX: the translation's reads of guest tables go to the job's copies instead
    // (the hooks of the TLOU shader replay test, which proved the SPIR-V byte for byte the same).
    const bool snapshot = job.kind != Kind::Guest;
    if (snapshot) {
        g_shader_prebuild_test = true;
        g_shader_fake_flat = job.kind == Kind::Snapshot ? &job.flat : nullptr;
        g_shader_fake_fetch = job.fetch ? &*job.fetch : nullptr;
    }
    const auto keep_walker = [&job](const Shader::Info& info) {
        const auto* code = reinterpret_cast<const u8*>(info.srt_info.walker_func);
        if (code) {
            job.walker.assign(code, code + info.srt_info.walker_func_size);
        }
        job.flat_dw = info.srt_info.flattened_bufsize_dw;
        job.has_fetch = info.has_fetch_shader;
        job.fetch_sgpr = info.fetch_shader_sgpr_base;
    };
    // Kept apart from the sigsetjmp below: it must not cross objects with destructors.
    const auto body = [this, &job, &keep_walker] {
        try {
            const auto params = job.Params();
            auto binding = job.start;
            if (job.kind == Kind::Walker) {
                Shader::Info info(job.hw_stage, job.sw_stage, params);
                Shader::RuntimeInfo runtime = job.runtime_info;
                Shader::TranslateProgram(params.code, job_pools, info, runtime, profile);
                if (info.translation_failed) {
                    job.failed = true;
                    return;
                }
                keep_walker(info);
            } else if (job.new_program) {
                job.program = std::make_unique<Program>(job.hw_stage, job.sw_stage, params);
                job.module = TranslateModule(job.program->info, job.runtime_info, params.code,
                                             job.perm_idx, binding, job_pools, job.spv);
                job.spec.emplace(job.program->info, job.runtime_info, profile, job.start);
                if (job.kind == Kind::Snapshot) {
                    keep_walker(job.program->info);
                }
            } else {
                auto info = Shader::Info(job.hw_stage, job.sw_stage, params);
                job.module = TranslateModule(info, job.runtime_info, params.code, job.perm_idx,
                                             binding, job_pools, job.spv);
            }
        } catch (const std::exception&) {
            job.failed = true;
        }
    };
    sigjmp_buf* const outer = runtime_fault_recover;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        job.failed = true;
    } else {
        runtime_fault_recover = &recover;
        body();
    }
    runtime_fault_recover = outer;
    if (snapshot) {
        g_shader_fake_flat = nullptr;
        g_shader_fake_fetch = nullptr;
        g_shader_prebuild_test = false;
    }
}

namespace {
/// BB_ASYNC_VERTEX=1: vertex shaders translated in the background from copies; 2: pixel too.
int AsyncVertex() {
    static const int value = [] {
        const char* env = std::getenv("BB_ASYNC_VERTEX");
        return env && env[0] >= '1' && env[0] <= '2' && !EmulatorSettings.IsShaderCollect()
                   ? env[0] - '0'
                   : 0;
    }();
    return value;
}
bool SnapshotStage(HwStage hw_stage, SwStage sw_stage) {
    const int mode = AsyncVertex();
    return mode && ((hw_stage == HwStage::Vertex && sw_stage == SwStage::Vertex) ||
                    (mode >= 2 && hw_stage == HwStage::Fragment));
}
} // namespace

struct PipelineCache::SnapshotWalker {
    Shader::PFN_SrtWalker func{};
    std::vector<u8> code;
    u32 flat_dw{};
    bool has_fetch{};
    u32 fetch_sgpr{};
};

/// BB_ASYNC_VERTEX: the draw's tables copied now, on the GPU thread, as a translation here would
/// read them; the translation runs in the background from the copies and the draw is skipped.
/// `info` is the program's (flat buffer refreshed for this draw) for a new permutation, with the
/// draw's `spec`; null for a new program, whose walker is learned by a first job.
bool PipelineCache::QueueSnapshot(PipelineSelection& sel, Shader::HwStage hw_stage,
                                  Shader::SwStage sw_stage, const Shader::ShaderParams& params,
                                  const Shader::RuntimeInfo& runtime_info,
                                  const Shader::Backend::Bindings& start, const Shader::Info* info,
                                  std::optional<Shader::StageSpecialization>& spec,
                                  size_t perm_idx) {
    if (&sel != &this->sel || !SnapshotStage(hw_stage, sw_stage) ||
        translate_sync.contains(params.hash)) {
        return false;
    }
    sel.translation_pending = true;
    if (translating.contains(params.hash)) {
        return true; // one job a program at a time: this draw waits for it
    }
    if (!translate_workers) {
        translate_workers = std::make_unique<TranslateWorkers>(*this);
    }
    auto job = std::make_unique<TranslateJob>();
    job->hw_stage = hw_stage;
    job->sw_stage = sw_stage;
    job->hash = params.hash;
    std::copy(params.user_data.begin(), params.user_data.end(), job->user_data.begin());
    job->code = params.code; // the program's code: it stays while the program is in use
    job->runtime_info = runtime_info;
    job->start = start;
    job->perm_idx = perm_idx;
    job->new_program = !info;
    std::optional<Shader::Info> fresh;
    if (!info) {
        const auto it = snapshot_walkers.find(params.hash);
        if (it == snapshot_walkers.end()) {
            job->kind = TranslateJob::Kind::Walker;
        } else {
            const auto& walker = *it->second;
            fresh.emplace(hw_stage, sw_stage, params);
            fresh->srt_info.walker_func = walker.func;
            fresh->srt_info.flattened_bufsize_dw = walker.flat_dw;
            fresh->has_fetch_shader = walker.has_fetch;
            fresh->fetch_shader_sgpr_base = walker.fetch_sgpr;
            fresh->RefreshFlatBuf();
            job->fetch = Shader::Gcn::ParseFetchShader(*fresh);
            info = &*fresh;
        }
    } else {
        job->fetch = spec ? spec->fetch_shader_data : std::nullopt;
        job->spec = std::move(spec);
    }
    if (job->kind != TranslateJob::Kind::Walker) {
        job->kind = TranslateJob::Kind::Snapshot;
        const auto flat = info->FlatUserData();
        job->flat.assign(flat.begin(), flat.end());
        if (job->fetch) {
            // The vertex buffer descriptors the fetch shader names, read through table pointers in
            // the user data: copied into tables of the job's, and its user data points there.
            for (const auto& attrib : job->fetch->attributes) {
                if (attrib.sgpr_base + 1 >= Shader::ShaderParams::NumShaderUserData) {
                    continue; // in the user data itself
                }
                auto& table = job->tables[attrib.sgpr_base];
                const size_t end = attrib.dword_offset + sizeof(AmdGpu::Buffer) / sizeof(u32);
                if (table.size() < end) {
                    table.resize(end);
                }
                const auto sharp = info->ReadUdReg<AmdGpu::Buffer>(attrib.sgpr_base, attrib.dword_offset);
                std::memcpy(&table[attrib.dword_offset], &sharp, sizeof(sharp));
            }
            for (u32 reg = 0; reg < job->tables.size(); ++reg) {
                if (!job->tables[reg].empty()) {
                    const u64 pointer = reinterpret_cast<u64>(job->tables[reg].data());
                    std::memcpy(&job->user_data[reg], &pointer, sizeof(pointer));
                }
            }
        }
    }
    // BB_ASYNC_VERTEX_CHECK=1 (PC test): also translate here, as the GPU thread did before, from
    // the same guest memory; FinishTranslations compares.
    static const bool check = [] {
        const char* env = std::getenv("BB_ASYNC_VERTEX_CHECK");
        return env && env[0] == '1';
    }();
    if (check) {
        auto check_params = params;
        Shader::Info check_info(hw_stage, sw_stage, check_params);
        auto check_runtime = runtime_info;
        auto check_binding = start;
        const auto module = TranslateModule(check_info, check_runtime, params.code, perm_idx,
                                            check_binding, pools, job->check_spv);
        instance.GetDevice().destroyShaderModule(module);
        if (const auto* code = reinterpret_cast<const u8*>(check_info.srt_info.walker_func)) {
            job->check_walker.assign(code, code + check_info.srt_info.walker_func_size);
        }
    }
    translating.emplace(params.hash, 1);
    BbOverlay::ShaderStarted(job->kind != TranslateJob::Kind::Walker);
    translate_workers->Submit(job.get());
    translate_jobs.push_back(std::move(job));
    return true;
}

bool PipelineCache::QueueTranslation(PipelineSelection& sel, Shader::HwStage hw_stage,
                                     Shader::SwStage sw_stage, const Shader::ShaderParams& params,
                                     const Shader::RuntimeInfo& runtime_info,
                                     const Shader::Backend::Bindings& start, bool new_program,
                                     size_t perm_idx,
                                     std::optional<Shader::StageSpecialization> spec) {
    if (&sel != &this->sel || hw_stage == Shader::HwStage::Compute || !AsyncShaders() ||
        translate_sync.contains(params.hash)) {
        return false;
    }
    sel.translation_pending = true;
    if (translating.contains(params.hash)) {
        return true; // one job a program at a time: this draw waits for it
    }
    if (!translate_workers) {
        translate_workers = std::make_unique<TranslateWorkers>(*this);
    }
    auto job = std::make_unique<TranslateJob>();
    job->hw_stage = hw_stage;
    job->sw_stage = sw_stage;
    job->hash = params.hash;
    std::copy(params.user_data.begin(), params.user_data.end(), job->user_data.begin());
    job->code = params.code;
    job->runtime_info = runtime_info;
    job->start = start;
    job->new_program = new_program;
    job->perm_idx = perm_idx;
    job->spec = std::move(spec);
    translating.emplace(params.hash, 1);
    BbShaderJobs::Started();
    BbOverlay::ShaderStarted(true);
    translate_workers->Submit(job.get());
    translate_jobs.push_back(std::move(job));
    return true;
}

/// GPU thread: translated programs and permutations into the cache, as the draw would have.
void PipelineCache::FinishTranslations() {
    translate_seen = translate_done.load(std::memory_order_acquire);
    static const std::array<u32, Shader::ShaderParams::NumShaderUserData> no_user_data{};
    std::erase_if(translate_jobs, [this](std::unique_ptr<TranslateJob>& job) {
        if (!job->done.load(std::memory_order_acquire)) {
            return false;
        }
        translating.erase(job->hash);
        BbOverlay::ShaderFinished();
        using Kind = TranslateJob::Kind;
        const auto drop = [this, &job](u32 counter) {
            ++snapshot_counts[counter];
            translate_sync.emplace(job->hash, 1); // translated on the GPU thread from now on
            if (job->module) {
                instance.GetDevice().destroyShaderModule(job->module);
            }
            return true;
        };
        if (job->failed) {
            ++translate_failed;
            if (job->kind != Kind::Guest) {
                return drop(2);
            }
            translate_sync.emplace(job->hash, 1);
            return true;
        }
        if (job->kind != Kind::Guest && (!job->check_spv.empty() || !job->check_walker.empty())) {
            const bool walker_same = (job->kind == Kind::Snapshot && !job->new_program) ||
                                     job->check_walker == job->walker;
            const bool spv_same = job->kind == Kind::Walker || job->check_spv == job->spv;
            static u64 same = 0, differ = 0;
            ++(walker_same && spv_same ? same : differ);
            if (!(walker_same && spv_same) || ((same + differ) & 31) == 0) {
                std::printf("Shaders check: %s %016llx %s: walker %s, SPIR-V %s (%zu/%zu words); "
                            "%llu same, %llu differ\n",
                            job->hw_stage == HwStage::Fragment ? "pixel" : "vertex",
                            static_cast<unsigned long long>(job->hash),
                            job->kind == Kind::Walker ? "walker job" : job->new_program ? "new program" : "permutation",
                            walker_same ? "same" : "DIFFERS", spv_same ? "same" : "DIFFERS",
                            job->spv.size(), job->check_spv.size(), static_cast<unsigned long long>(same),
                            static_cast<unsigned long long>(differ));
                std::fflush(stdout);
            }
        }
        if (job->kind == Kind::Walker) {
            auto walker = std::make_unique<SnapshotWalker>();
            walker->code = std::move(job->walker);
            walker->func = walker->code.empty()
                               ? nullptr
                               : Shader::RegisterWalkerCode(walker->code.data(), walker->code.size());
            walker->flat_dw = job->flat_dw;
            walker->has_fetch = job->has_fetch;
            walker->fetch_sgpr = job->fetch_sgpr;
            snapshot_walkers[job->hash] = std::move(walker);
            ++snapshot_counts[0];
            return true;
        }
        if (job->kind == Kind::Snapshot && job->new_program) {
            // The walker that made the draw's copy must be the one this translation made.
            const auto it = snapshot_walkers.find(job->hash);
            auto& srt = job->program->info.srt_info;
            if (it == snapshot_walkers.end() || it->second->code != job->walker ||
                it->second->flat_dw != srt.flattened_bufsize_dw || program_cache.contains(job->hash)) {
                return drop(3);
            }
            srt.walker_func = it->second->func; // was this job's thread-local copy
            snapshot_walkers.erase(it);
        }
        if (job->kind == Kind::Snapshot && (++snapshot_counts[1] & 31) == 0) {
            std::printf("Shaders: %llu translated in the background from copies (%llu walkers learned, "
                        "%llu failed, %llu walker mismatch)\n",
                        static_cast<unsigned long long>(snapshot_counts[1]),
                        static_cast<unsigned long long>(snapshot_counts[0]),
                        static_cast<unsigned long long>(snapshot_counts[2]),
                        static_cast<unsigned long long>(snapshot_counts[3]));
            std::fflush(stdout);
        }
        const auto perm_hash = HashCombine(job->hash, job->perm_idx);
        if (job->new_program) {
            auto& program = job->program;
            RegisterShaderBinary(std::move(job->spv), job->hash, job->perm_idx);
            RegisterShaderMeta(program->info, job->spec->fetch_shader_data, *job->spec, perm_hash,
                               job->perm_idx);
            program->InsertPermut(job->module, std::move(*job->spec), job->perm_idx);
            program->last_used = job->perm_idx;
            // its user data was the job's copy; each lookup sets the draw's
            program->info.user_data = no_user_data;
            program->info_template = std::make_unique<Shader::Info>(program->info);
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(job->hash, std::move(program));
        } else {
            const auto it_pgm = program_cache.find(job->hash);
            if (it_pgm == program_cache.end()) {
                return true;
            }
            auto& program = it_pgm.value();
            RegisterShaderBinary(std::move(job->spv), job->hash, job->perm_idx);
            RegisterShaderMeta(program->info, job->spec->fetch_shader_data, *job->spec, perm_hash,
                               job->perm_idx);
            std::unique_lock lk{programs_mutex};
            program->InsertPermut(job->module, std::move(*job->spec), job->perm_idx);
            program->last_used = job->perm_idx;
        }
        if ((++translated_async & 127) == 0) {
            std::printf("Shaders: %llu translated in the background, %llu left to the GPU thread (a table it could not read)\n",
                        static_cast<unsigned long long>(translated_async),
                        static_cast<unsigned long long>(translate_failed));
            std::fflush(stdout);
        }
        return true;
    });
}

// TLOU test (BB_SHADER_PACK=<file from tools/psarc_extract_shaders.py>, 2026-11-09): how long does it
// take to translate every shader in the game files at startup? Each goes through the same
// GCN -> IR -> SPIR-V path as a draw, on all CPU threads, with no game state behind it: descriptors
// read as null and there is no fetch shader. So this is a timing, not a usable cache: what comes out
// is thrown away, nothing is written to the shader cache, and the program exits at the end.
static void ShaderPrebuildTest(const char* path, const Shader::Profile& profile) {
    using Clock = std::chrono::steady_clock;
    const auto Seconds = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    };
    const auto start_read = Clock::now();
    std::vector<u8> pack;
    {
        std::ifstream file(std::filesystem::path{path}, std::ios::binary | std::ios::ate);
        if (!file) {
            std::printf("Shader prebuild test: cannot open %s\n", path);
            std::fflush(stdout);
            return;
        }
        pack.resize(size_t(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(pack.data()), std::streamsize(pack.size()));
    }
    struct Entry {
        u64 hash;
        u32 type;
        std::span<const u32> code;
    };
    std::vector<Entry> entries;
    if (pack.size() < 12 || std::memcmp(pack.data(), "GCNPACK1", 8) != 0) {
        std::printf("Shader prebuild test: %s is not a shader pack\n", path);
        std::fflush(stdout);
        return;
    }
    u32 count;
    std::memcpy(&count, pack.data() + 8, 4);
    entries.reserve(count);
    size_t at = 12;
    for (u32 i = 0; i < count && at + 16 <= pack.size(); ++i) {
        u64 hash;
        u32 type, size;
        std::memcpy(&hash, pack.data() + at, 8);
        std::memcpy(&type, pack.data() + at + 8, 4);
        std::memcpy(&size, pack.data() + at + 12, 4);
        at += 16;
        // The last 32 bytes are the footer; the code length is in it, as for a draw.
        u32 bits;
        std::memcpy(&bits, pack.data() + at + size - 32 + 8, 4);
        entries.push_back({hash, type,
                           std::span{reinterpret_cast<const u32*>(pack.data() + at),
                                     (bits >> 8) / sizeof(u32)}});
        at += size;
    }
    const double read_s = Seconds(start_read, Clock::now());

    u32 threads = std::thread::hardware_concurrency();
    if (const char* env = std::getenv("BB_SHADER_PACK_THREADS"); env && *env) {
        threads = u32(std::strtoul(env, nullptr, 10));
    }
    threads = std::max(threads, 1u);
    std::printf("Shader prebuild test: %zu shaders, %.1f MiB read in %.2f s, %u threads\n",
                entries.size(), double(pack.size()) / 1048576.0, read_s, threads);
    std::fflush(stdout);

    struct StageStats {
        std::atomic<u64> ok{0}, failed{0}, asserted{0}, faulted{0}, spv_bytes{0}, micros{0}, max_micros{0};
    };
    std::array<StageStats, 3> stats; // pixel, vertex, compute
    std::atomic<u64> skipped{0};
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    const auto start = Clock::now();

    // Kept apart from the loop: sigsetjmp must not cross objects with destructors.
    const auto translate = [&profile](const Entry& entry, Shader::Pools& pools, StageStats& stage,
                                      Shader::HwStage hw, Shader::SwStage sw) {
        static const std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
        try {
            const Shader::ShaderParams params{
                std::span<const u32, Shader::ShaderParams::NumShaderUserData>(user_data), entry.code,
                entry.hash};
            Shader::Info info(hw, sw, params);
            Shader::RuntimeInfo runtime{};
            runtime.Initialize(hw, sw);
            runtime.props.num_user_data = 16;
            runtime.props.num_input_vgprs = 3;
            runtime.props.num_allocated_vgprs = 128;
            if (hw == Shader::HwStage::Fragment) {
                runtime.hw.fs.en_flags.persp_center_ena = 1;
                runtime.hw.fs.addr_flags.persp_center_ena = 1;
                runtime.hw.fs.num_inputs = 32;
                for (u32 i = 0; i < 32; ++i) {
                    runtime.hw.fs.inputs[i].param_index = u8(i);
                }
            } else if (hw == Shader::HwStage::Compute) {
                runtime.hw.cs.workgroup_size = {64, 1, 1};
                runtime.hw.cs.tgid_enable = {true, true, true};
                runtime.hw.cs.shared_memory_size = 65536;
            }
            Shader::Backend::Bindings binding{};
            const auto program = Shader::TranslateProgram(entry.code, pools, info, runtime, profile);
            const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
            stage.spv_bytes.fetch_add(spv.size() * sizeof(u32), std::memory_order_relaxed);
            (info.translation_failed ? stage.failed : stage.ok).fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception&) {
            stage.asserted.fetch_add(1, std::memory_order_relaxed);
        }
    };

    const auto worker = [&] {
        Common::SetCurrentThreadName("bbport:ShaderPrebuild");
        g_shader_prebuild_test = true;
        static thread_local Shader::Pools pools;
        while (true) {
            const size_t index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= entries.size()) {
                break;
            }
            const Entry& entry = entries[index];
            const int slot = entry.type == 0 ? 0 : entry.type == 1 ? 1 : entry.type == 4 ? 2 : -1;
            if (slot < 0) {
                skipped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            auto& stage = stats[slot];
            const auto begin = Clock::now();
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                stage.faulted.fetch_add(1, std::memory_order_relaxed);
            } else {
                runtime_fault_recover = &recover;
                translate(entry, pools, stage,
                          slot == 0   ? Shader::HwStage::Fragment
                          : slot == 1 ? Shader::HwStage::Vertex
                                      : Shader::HwStage::Compute,
                          slot == 0   ? Shader::SwStage::Fragment
                          : slot == 1 ? Shader::SwStage::Vertex
                                      : Shader::SwStage::Compute);
                runtime_fault_recover = nullptr;
            }
            const u64 micros = u64(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count());
            stage.micros.fetch_add(micros, std::memory_order_relaxed);
            u64 longest = stage.max_micros.load(std::memory_order_relaxed);
            while (longest < micros && !stage.max_micros.compare_exchange_weak(longest, micros)) {
            }
            const size_t finished = done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (finished % 2000 == 0) {
                std::printf("Shader prebuild test: %zu of %zu after %.1f s\n", finished, entries.size(),
                            Seconds(start, Clock::now()));
                std::fflush(stdout);
            }
        }
        g_shader_prebuild_test = false;
    };
    {
        std::vector<std::jthread> pool;
        for (u32 i = 0; i < threads; ++i) {
            pool.emplace_back(worker);
        }
    }
    const double wall = Seconds(start, Clock::now());
    static constexpr std::array<const char*, 3> names{"pixel", "vertex", "compute"};
    std::printf("Shader prebuild test: DONE all %zu in %.1f s on %u threads (%llu not tried)\n",
                entries.size(), wall, threads, static_cast<unsigned long long>(skipped.load()));
    for (size_t i = 0; i < stats.size(); ++i) {
        const auto& s = stats[i];
        const u64 n = s.ok + s.failed + s.asserted + s.faulted;
        std::printf("Shader prebuild test: %s: %llu ok, %llu unknown opcode, %llu assert, %llu fault; "
                    "avg %.2f ms, longest %.1f ms, thread time %.1f s, SPIR-V %.1f MiB\n",
                    names[i], static_cast<unsigned long long>(s.ok.load()),
                    static_cast<unsigned long long>(s.failed.load()),
                    static_cast<unsigned long long>(s.asserted.load()),
                    static_cast<unsigned long long>(s.faulted.load()),
                    n ? double(s.micros) / 1000.0 / double(n) : 0.0, double(s.max_micros) / 1000.0,
                    double(s.micros) / 1e6, double(s.spv_bytes) / 1048576.0);
    }
    std::fflush(stdout);
    if (const char* env = std::getenv("BB_SHADER_PACK_EXIT"); !(env && env[0] == '0')) {
        std::_Exit(0);
    }
}

#include "vk_shader_replay.inc"
#include "vk_shader_prebuild.inc"

/// The runtime's file layer: bytes read from a level archive.
extern "C" void bbgpu_shader_area_read(const char* archive, unsigned long long bytes) {
    PipelineAreas::Read(archive, bytes);
    PrebuiltStore::Get().AreaRead(archive, bytes);
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = instance.SupportedSpirvVersion(),
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // bbport: older NVIDIA (Pascal) has no barycentrics; BB_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("BB_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        // Dozen may execute a PS4 64-lane workgroup as multiple hardware waves.
        // Preserve LDS write/read ordering across those waves, as on native NVIDIA.
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp ||
                              instance.GetDriverID() == vk::DriverId::eMesaDozen,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
        .needs_typed_storage_images = instance.NeedsTypedStorageImages(),
        .supports_demote_to_helper = instance.SupportsDemoteToHelper(),
    };
    // bbport: the driver cache before WarmUp: the preload builds its pipelines through it. It
    // used to be made after the preload, which so built every cached pipeline without one.
    if (EmulatorSettings.IsPipelineCacheEnabled()) {
        Storage::DataBase::Instance().Open();
    }
    CreateDriverCache();
    WarmUp();
    if (const char* replay = std::getenv("BB_SHADER_REPLAY"); replay && *replay) {
        if (const char* packs = std::getenv("BB_SHADER_REPLAY_GUESS"); packs && *packs) {
            ShaderGuessTest(replay, packs, std::getenv("BB_SHADER_REPLAY_OUT"));
        }
        ShaderReplayTest(replay, std::getenv("BB_SHADER_PACK"), std::getenv("BB_SHADER_REPLAY_OUT"));
    } else if (const char* pack = std::getenv("BB_SHADER_PACK"); pack && *pack) {
        ShaderPrebuildTest(pack, profile);
    } else if (const char* packs = std::getenv("BB_SHADER_PREBUILD");
               packs && *packs && !EmulatorSettings.IsShaderCollect()) {
        PrebuiltStore::Get().Start(packs, profile);
    }
}

namespace {
bool AsyncPipelines() {
    static const bool value = [] {
        // TLOU: on unless BB_ASYNC_PIPELINES=0 (Dozen builds a pipeline in tens of ms).
        const char* env = std::getenv("BB_ASYNC_PIPELINES");
        return !(env && env[0] == '0') && !EmulatorSettings.IsShaderCollect();
    }();
    return value;
}

// TLOU: OFF unless BB_ASYNC_SHADERS=1 (2026-11-03 Xbox: the aim-down-sights depth of field drew
// striped garbage). Translations read the draw's descriptor tables, which the constant engine
// keeps rewriting in its ring (DumpConstRam: memory writes without a guest-visible signal), so a
// translation that runs a few ms after its draw can read the next draws' descriptors and build
// the shader for another image type. Waiting at every DumpConstRam would make it synchronous.
bool AsyncShaders() {
    static const bool value = [] {
        const char* env = std::getenv("BB_ASYNC_SHADERS");
        return env && env[0] == '1' && AsyncPipelines();
    }();
    return value;
}

// A pipeline the draw can use now: built (not still in the background) and valid.
const GraphicsPipeline* Usable(const GraphicsPipeline* pipeline) {
    return pipeline && pipeline->IsBuilt() && pipeline->IsValid() ? pipeline : nullptr;
}
} // namespace

void PipelineCache::FinishAsyncBuilds() {
    async_seen = async_done.load(std::memory_order_acquire);
    std::erase_if(async_builds, [this](AsyncBuild& build) {
        if (!build.pipeline->IsBuilt()) {
            return false;
        }
        if (build.pipeline->IsValid()) {
            RegisterPipelineData(build.key, build.hash, build.sdata);
            ++num_new_pipelines;
            ++driver_cache_changes;
        } else {
            // As for one built at once: kept empty, skipped, not compiled again.
            graphics_pipelines[build.key].reset();
        }
        return true;
    });
}

PipelineCache::~PipelineCache() {
    build_workers.reset();       // waits for a build in progress
    driver_cache_saver = {}; // stops and joins it
    if (!driver_cache_file.empty() && driver_cache_changes.load() != 0) {
        SaveDriverCache();
    }
}

void PipelineCache::CreateDriverCache() {
    std::vector<u8> data;
    auto& storage = Storage::DataBase::Instance();
    // TLOU: kept on disk only with BB_DRIVER_CACHE=1 (Dozen's cache copy is large; the host's
    // compiled-shader cache already keeps the D3D12 pipelines).
    const char* keep = std::getenv("BB_DRIVER_CACHE");
    if (storage.IsOpened() && keep && keep[0] == '1') {
        driver_cache_file = storage.DriverCachePath();
        std::ifstream file{driver_cache_file, std::ios::binary | std::ios::ate};
        if (file) {
            data.resize(static_cast<size_t>(file.tellg()));
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()))) {
                data.clear();
            }
        }
    }

    // The driver checks the header as well; one from another GPU or driver is not offered.
    struct Header {
        u32 size;
        u32 version;
        u32 vendor_id;
        u32 device_id;
        std::array<u8, VK_UUID_SIZE> uuid;
    } header{};
    const auto props = instance.GetPhysicalDevice().getProperties();
    if (data.size() > sizeof(header)) {
        std::memcpy(&header, data.data(), sizeof(header));
    }
    const bool usable =
        data.size() > sizeof(header) && header.version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
        header.vendor_id == props.vendorID && header.device_id == props.deviceID &&
        std::memcmp(header.uuid.data(), props.pipelineCacheUUID.data(), VK_UUID_SIZE) == 0;
    if (!usable && !data.empty()) {
        std::printf("Driver pipeline cache: made by another GPU or driver, starting a new one\n");
        data.clear();
    }

    const auto device = instance.GetDevice();
    auto [cache_result, cache] = device.createPipelineCacheUnique({
        .initialDataSize = data.size(),
        .pInitialData = data.data(),
    });
    if (cache_result != vk::Result::eSuccess && !data.empty()) {
        LOG_WARNING(Render_Vulkan, "The driver refused the saved pipeline cache ({}): new one",
                    vk::to_string(cache_result));
        data.clear();
        auto retry = device.createPipelineCacheUnique({});
        cache_result = retry.result;
        cache = std::move(retry.value);
    }
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    if (driver_cache_file.empty()) {
        return;
    }
    std::printf("Driver pipeline cache: %s (%zu KiB)\n", data.empty() ? "new" : "loaded",
                data.size() >> 10);

    // Saved while the game runs (it is usually ended, not shut down): every 20 seconds when
    // pipelines were made since.
    driver_cache_saver = std::jthread{[this](std::stop_token token) {
        Common::SetCurrentThreadName("bbport:DriverPipelineCache");
        u32 seconds = 0;
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (++seconds >= 20 && driver_cache_changes.load(std::memory_order_relaxed) != 0) {
                seconds = 0;
                SaveDriverCache();
            }
        }
    }};
}

void PipelineCache::SaveDriverCache() {
    std::scoped_lock lock{driver_cache_mutex};
    const u32 changes = driver_cache_changes.exchange(0);
    auto [result, data] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
    if (result != vk::Result::eSuccess || data.empty()) {
        LOG_WARNING(Render_Vulkan, "Could not read the driver pipeline cache: {}",
                    vk::to_string(result));
        driver_cache_changes += changes;
        return;
    }
    // Written beside and then renamed: a game ended while writing keeps the previous file.
    auto temp = driver_cache_file;
    temp += ".new";
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!file) {
            LOG_WARNING(Render_Vulkan, "Could not write {}", temp.string());
            return;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, driver_cache_file, error);
    if (error) {
        LOG_WARNING(Render_Vulkan, "Could not save the driver pipeline cache: {}", error.message());
        std::filesystem::remove(temp, error);
        return;
    }
    LOG_INFO(Render_Vulkan, "Driver pipeline cache saved ({} KiB)", data.size() >> 10);
}

// bbport: shader/pipeline compile time on the GPU thread, reported by BB_FRAME_STATS.
std::atomic<u64> g_bb_compile_ns;
std::atomic<u32> g_bb_compiles;
// bbport: the same by kind (CompileKind), never reset: the Hitch line takes the difference.
std::atomic<u64> g_bb_compile_kind_ns[3];
std::atomic<u32> g_bb_compile_kind_count[3];
namespace {
enum CompileKind : u32 {
    ShaderTranslation = 0, ///< GCN -> SPIR-V and the shader module (CompileModule)
    GraphicsSetup = 1,     ///< a new graphics pipeline on the GPU thread (all of it when not async)
    ComputeBuild = 2,      ///< a new compute pipeline, built by the driver on the GPU thread
};
struct CompileTimer {
    CompileKind kind;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit CompileTimer(CompileKind kind_) : kind{kind_} {}
    ~CompileTimer() {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
        g_bb_compile_ns += ns;
        ++g_bb_compiles;
        g_bb_compile_kind_ns[kind].fetch_add(ns, std::memory_order_relaxed);
        g_bb_compile_kind_count[kind].fetch_add(1, std::memory_order_relaxed);
    }
};
} // namespace

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() ? Usable(it->second.get()) : nullptr;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    used_prepared = nullptr;
    if (!async_builds.empty() && async_done.load(std::memory_order_acquire) != async_seen) {
        FinishAsyncBuilds();
    }
    if (!translate_jobs.empty() && translate_done.load(std::memory_order_acquire) != translate_seen) {
        FinishTranslations();
    }
    if (area_state) {
        BuildAreaPipelines();
    }
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    if (!RefreshGraphicsKey(sel)) {
        return nullptr;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        CompileTimer timer{GraphicsSetup};

        GraphicsPipeline::SerializationSupport sdata{};
        const bool async = AsyncPipelines();
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false, async);
        if (async) {
            if (!build_workers) {
                build_workers = std::make_unique<BuildWorkers>(async_done);
            }
            async_builds.push_back({sel.graphics_key, pipeline_hash, sdata, it.value().get()});
            build_workers->Submit(it.value().get());
            sel.fetch_shader.reset();
            return nullptr;
        }
        // bbport: the driver could not create it. An empty entry keeps the key from being
        // compiled again on every draw; it is not saved, so the next start does not load it.
        if (!it.value()->IsValid()) {
            it.value().reset();
            sel.fetch_shader.reset();
            return nullptr;
        }

        RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;
        ++driver_cache_changes;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    return Usable(it->second.get());
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        CompileTimer timer{ComputeBuild};

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *sel.infos[0],
                                                       sel.modules[0], sdata, false);
        // bbport: as for graphics pipelines, kept empty and not saved.
        if (!it.value()->IsValid()) {
            it.value().reset();
            return nullptr;
        }
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;
        ++driver_cache_changes;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    if (!instance.IsExtendedDynamicStateSupported()) {
        // Keep the pipeline key in sync with Rasterizer's dynamic-state calculations.
        key.viewport_count = std::max<u32>(1, std::ranges::count_if(
            regs.viewports, [](const auto& vp) { return vp.xscale != 0; }));
        key.depth_test_enable = regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
        key.depth_write_enable = key.depth_test_enable && regs.depth_control.depth_write_enable &&
                                 !regs.depth_render_control.depth_clear_enable;
        key.depth_compare_op = key.depth_test_enable
                                   ? LiverpoolToVK::CompareOp(regs.depth_control.depth_func)
                                   : vk::CompareOp::eAlways;
        key.depth_bounds_enable = instance.IsDepthBoundsSupported() &&
                                  regs.depth_control.depth_bounds_enable;
        key.depth_bias_enable = regs.polygon_control.NeedsBias();
        if (key.depth_bias_enable && BakedDynamicState()) {
            // As Rasterizer::UpdateDepthStencilState sets them.
            const bool front = regs.polygon_control.enable_polygon_offset_front;
            key.depth_bias_constant =
                front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset;
            key.depth_bias_clamp = regs.poly_offset.depth_bias;
            key.depth_bias_slope =
                (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f;
        }
        key.stencil_test_enable = regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
        if (key.stencil_test_enable) {
            key.stencil_front = {
                .failOp = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
                .passOp = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
                .depthFailOp = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
                .compareOp = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
            };
            key.stencil_back = regs.depth_control.backface_enable ? vk::StencilOpState{
                .failOp = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
                .passOp = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
                .depthFailOp = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
                .compareOp = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
            } : key.stencil_front;
            if (BakedDynamicState()) {
                const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
                const auto& front = regs.stencil_ref_front;
                const auto& back = regs.depth_control.backface_enable ? regs.stencil_ref_back
                                                                      : regs.stencil_ref_front;
                key.stencil_front.compareMask = front.stencil_mask;
                key.stencil_front.writeMask = stencil_clear ? 0U : u32(front.stencil_write_mask);
                key.stencil_back.compareMask = back.stencil_mask;
                key.stencil_back.writeMask = stencil_clear ? 0U : u32(back.stencil_write_mask);
            }
        }
        const auto topology = LiverpoolToVK::PrimitiveType(regs.primitive_type);
        const bool list = topology == vk::PrimitiveTopology::ePointList ||
                          topology == vk::PrimitiveTopology::eLineList ||
                          topology == vk::PrimitiveTopology::eTriangleList ||
                          topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
                          topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
        const bool patch = regs.primitive_type == AmdGpu::PrimitiveType::PatchPrimitive ||
                           regs.primitive_type == AmdGpu::PrimitiveType::QuadList ||
                           regs.primitive_type == AmdGpu::PrimitiveType::RectList;
        key.primitive_restart_enable = (regs.enable_primitive_restart & 1) &&
            (instance.IsListRestartSupported() || !list) &&
            (instance.IsPatchListRestartSupported() || !patch);
        key.rasterizer_discard_enable = regs.clipper_control.dx_rasterization_kill;
        key.cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                            ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                            : vk::CullModeFlagBits::eNone;
        key.front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // BB_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("BB_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("BB_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (sel.translation_pending) {
            return false; // translated in the background: this draw is skipped
        }
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.translation_pending = false;

    bind_stage(HwStage::Fragment, SwStage::Fragment);
    if (sel.translation_pending) {
        return false; // the vertex stage's runtime info needs the fragment program's
    }

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        if (sel.translation_pending) {
            return false;
        }
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    // bbport: a draw whose vertex buffer table pointer is null (reported before a crash at the
    // cutscene after Rom and in normal play) has no vertex input to build: skipped.
    if (vs_info && sel.fetch_shader) {
        for (const auto& attrib : sel.fetch_shader->attributes) {
            if (vs_info->UdPointerNull(attrib.sgpr_base)) {
                static std::atomic<u32> reported{0};
                if (reported.fetch_add(1, std::memory_order_relaxed) < 5) {
                    std::printf("GPU: draw skipped: its vertex buffer table pointer is null\n");
                }
                return false;
            }
        }
    }
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            if (!instance.IsExtendedDynamicStateSupported()) {
                key.vertex_buffer_strides[vertex_binding] = buffer.GetStride();
            }
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::TranslateModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                                const std::span<const u32>& code, size_t perm_idx,
                                                Shader::Backend::Bindings& binding,
                                                Shader::Pools& job_pools, std::vector<u32>& spv) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    const auto ir_program = Shader::TranslateProgram(code, job_pools, info, runtime_info, profile);
    spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;
    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    if (patch && EmulatorSettings.IsPatchShaders()) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }
    Vulkan::SetObjectName(instance.GetDevice(), module, GetShaderName(info.hw_stage, info.pgm_hash, perm_idx));
    return module;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    CompileTimer timer{ShaderTranslation};
    BbOverlay::ShaderBuilt();
    std::vector<u32> spv;
    const auto module = TranslateModule(info, runtime_info, code, perm_idx, binding, pools, spv);
    if (EmulatorSettings.IsShaderCollect()) {
        auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
        const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
        DebugState.CollectShader(GetShaderName(info.hw_stage, info.pgm_hash, perm_idx), info.sw_stage,
                                 module, spv, code, patch ? *patch : std::span<const u32>{}, is_patched);
    }
    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // bbport: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found_program = program_cache.find(params.hash);
        if (found_program == program_cache.end() || !found_program->second->info_template) {
            worker.failed = true;
            return {};
        }
        const Program* program = found_program->second.get();
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            worker.failed = true;
            return {};
        }
        runtime_fault_recover = &recover;
        info.RefreshFlatBuf();
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        runtime_fault_recover = nullptr;
        const auto it = program->FindPermut(spec);
        if (it == program->modules.end()) {
            worker.failed = true;
            return {};
        }
        info.AddBindings(binding);
        const size_t perm_idx = std::distance(program->modules.begin(), it);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
        return std::make_tuple(&info, it->module, it->spec.fetch_shader_data,
                               HashCombine(params.hash, perm_idx));
    }

    // bbport: a permutation's index names its cached binary and metadata (<pgm>_<index>) and
    // the pipelines that use it. A session that did not load every permutation of a program
    // (motion pipelines are never preloaded, nor are failed ones) handed out an index the cache
    // already held for another permutation and wrote over it; the pipelines saved with the old
    // one then loaded the wrong shader at the next start (orange screen, black ground, 2026-10-31).
    // A new permutation takes the first index the cache does not hold yet.
    const auto free_index = [&params](size_t index) {
        auto& storage = Storage::DataBase::Instance();
        if (!storage.IsOpened()) {
            return index;
        }
        for (size_t tries = 0; tries < 64; ++tries, ++index) {
            std::vector<u8> meta;
            storage.Load(Storage::BlobType::ShaderMeta,
                         fmt::format("{:#018x}", HashCombine(params.hash, index)), meta);
            if (meta.empty()) {
                break;
            }
        }
        return index;
    };

    auto it_pgm = program_cache.find(params.hash); // this thread is the only writer
    // TLOU pre-build: the entry's Info is what a translation would make, whatever the descriptors.
    PrebuiltPtr prebuilt;
    auto& prebuilt_store = PrebuiltStore::Get();
    if (it_pgm == program_cache.end() && prebuilt_store.Enabled() && !AsyncShaders() &&
        !translating.contains(params.hash)) {
        prebuilt = prebuilt_store.Find(params.hash);
        if (prebuilt) {
            auto loaded = std::make_unique<Program>();
            Shader::StageSpecialization ignored{};
            if (PrebuiltGuess(*prebuilt, loaded->info, ignored, true) && loaded->info.hw_stage == hw_stage &&
                loaded->info.sw_stage == sw_stage) {
                std::unique_lock lk{programs_mutex};
                it_pgm = program_cache.emplace(params.hash, std::move(loaded)).first;
            } else {
                prebuilt = nullptr;
            }
        }
    }
    if (it_pgm == program_cache.end()) {
        std::optional<Shader::StageSpecialization> no_spec;
        if (SnapshotStage(hw_stage, sw_stage) &&
            QueueSnapshot(sel, hw_stage, sw_stage, params, runtime_info, binding, nullptr, no_spec,
                          translating.contains(params.hash) ? 0 : free_index(0))) {
            return {};
        }
        if (QueueTranslation(sel, hw_stage, sw_stage, params, runtime_info, binding, true,
                             translating.contains(params.hash) ? 0 : free_index(0), std::nullopt)) {
            return {};
        }
        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto start = binding;
        const size_t first_idx = free_index(0);
        const auto module =
            CompileModule(new_program->info, runtime_info, params.code, first_idx, binding);
        auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, first_idx);

        RegisterShaderMeta(new_program->info, spec.fetch_shader_data, spec, perm_hash, first_idx);
        new_program->InsertPermut(module, std::move(spec), first_idx);
        new_program->last_used = first_idx;
        new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
        Program* program = new_program.get();
        {
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
        }
        return std::make_tuple(&program->info, module,
                               program->modules[first_idx].spec.fetch_shader_data, perm_hash);
    }

    auto& program = it_pgm.value();
    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        program->info_template = std::make_unique<Shader::Info>(program->info);
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    // bbport: consecutive draws of a program almost always use the same permutation.
    auto it = program->last_used < program->modules.size() &&
                      program->modules[program->last_used].module &&
                      program->modules[program->last_used].spec == spec
                  ? program->modules.begin() + program->last_used
                  : program->FindPermut(spec);
    if (it != program->modules.end()) {
        program->last_used = std::distance(program->modules.begin(), it);
    }
    if (it == program->modules.end()) {
        std::optional<Shader::StageSpecialization> snapshot_spec;
        const bool snapshot_stage = SnapshotStage(hw_stage, sw_stage);
        if (snapshot_stage && translating.contains(params.hash)) {
            // a background job is adding a permutation to this program: wait for it
            if (QueueSnapshot(sel, hw_stage, sw_stage, params, runtime_info, binding, &info,
                              snapshot_spec, 0)) {
                return {};
            }
        }
        perm_idx = free_index(perm_idx);
        if (prebuilt_store.Enabled() && !AsyncShaders()) {
            const u32 slot = hw_stage == HwStage::Fragment ? 0 : hw_stage == HwStage::Compute ? 3 : 6;
            if (!prebuilt && slot != 6) {
                prebuilt = prebuilt_store.Find(params.hash);
            }
            if (slot == 6) {
                ++prebuilt_store.counts[6];
            } else if (!prebuilt) {
                ++prebuilt_store.counts[slot + 2];
            } else {
                Shader::Info unused_info{};
                Shader::StageSpecialization guess{};
                const char* why = PrebuiltGuess(*prebuilt, unused_info, guess, false)
                                      ? PrebuiltMismatch(spec, guess, params.hash)
                                      : "unreadable";
                if (why) {
                    ++prebuilt_store.counts[slot + 1];
                    ++prebuilt_store.wrong[why];
                } else {
                    // Byte for byte what a translation with these settings would make.
                    std::vector<u32> spv(prebuilt->spv_words);
                    if (Unsqueeze(prebuilt->spv, spv.data(), spv.size() * 4)) {
                        ++prebuilt_store.counts[slot];
                        perm_hash = HashCombine(params.hash, perm_idx);
                        module = CompileSPV(spv, instance.GetDevice());
                        RegisterShaderBinary(std::move(spv), params.hash, perm_idx);
                        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
                        info.AddBindings(binding);
                        std::unique_lock lk{programs_mutex};
                        program->InsertPermut(module, std::move(spec), perm_idx);
                        program->last_used = perm_idx;
                        lk.unlock();
                        prebuilt_store.Report();
                        return std::make_tuple(&program->info, module,
                                               program->modules[perm_idx].spec.fetch_shader_data, perm_hash);
                    }
                }
            }
            prebuilt_store.Report();
        }
        if (snapshot_stage) {
            snapshot_spec.emplace(spec);
            if (QueueSnapshot(sel, hw_stage, sw_stage, params, runtime_info, binding, &info,
                              snapshot_spec, perm_idx)) {
                return {};
            }
        }
        if (QueueTranslation(sel, hw_stage, sw_stage, params, runtime_info, binding, false, perm_idx,
                             translating.contains(params.hash) ? std::nullopt
                                                               : std::optional{spec})) {
            return {};
        }
        perm_hash = HashCombine(params.hash, perm_idx);
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        std::unique_lock lk{programs_mutex};
        program->InsertPermut(module, std::move(spec), perm_idx);
        program->last_used = perm_idx;
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    return std::make_tuple(&program->info, module,
                           program->modules[perm_idx].spec.fetch_shader_data, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
