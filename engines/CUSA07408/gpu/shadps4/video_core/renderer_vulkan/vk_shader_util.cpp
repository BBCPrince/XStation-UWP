// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

namespace {
// bbport (Xbox, unless BB_SHADOW_FIX=0): TLOU's cached-shadow compute shader (cs 84879c67)
// appends shadow candidates to a shared-memory record array at the index an atomic returns, then
// reduces 32->16->8->4->2->1 in shared memory without barriers between the stages. On the Xbox
// driver the records come out corrupted: 16x16 dark blocks in the 480x270 shadow mask (the
// "black squares", e.g. Sarah's bedroom after the opening cutscene). The fix validated on the
// Xbox for the earlier runtime (atomic-shadow-fix-248) is applied here by structure, so it does
// not depend on the exact bytes the recompiler produces:
//   - a workgroup barrier at the start of each of the five reduction stages;
//   - the atomic's result passes through a per-lane shared slot (word 2112 + lane; only lanes
//     below 64 append, and the 2176-word array leaves 2112..2175 unused) with a workgroup memory
//     barrier before it addresses a record;
//   - a workgroup barrier at the start of the candidate batch loop's continue block.
// On the Xbox the module is byte-identical to the one fix 248 replaced (d5b41c00...), and the
// result here equals its validated replacement (c9adce42...) instruction for instruction.
// Recognized only with LocalSize 16x16x1, exactly one shared-memory OpAtomicIAdd and the five
// "16/8/4/2/1 > lane" compares at the start of a block; anything else is left unchanged.
bool PatchSharedAtomicShadow(std::span<const u32> code, std::vector<u32>& out) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SHADOW_FIX");
        return !(env && env[0] == '0');
    }();
    constexpr u32 OpExecutionMode = 16, OpConstant = 43, OpFunction = 54, OpVariable = 59,
                  OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpIAdd = 128,
                  OpUGreaterThan = 172, OpControlBarrier = 224, OpMemoryBarrier = 225,
                  OpAtomicIAdd = 234, OpLoopMerge = 246, OpLabel = 248, LocalSize = 17,
                  Workgroup = 4,
                  Volatile = 1;
    if (!enabled || code.size() < 5 || code[0] != 0x07230203u) {
        return false;
    }
    struct Inst {
        size_t at;
        u32 op, words;
    };
    std::vector<Inst> insts;
    for (size_t at = 5; at < code.size();) {
        const u32 words = code[at] >> 16;
        if (words == 0 || at + words > code.size()) {
            return false;
        }
        insts.push_back({at, code[at] & 0xffff, words});
        at += words;
    }
    std::unordered_map<u32, u32> constants;      // id -> 32-bit value
    std::unordered_map<u32, size_t> definitions; // result id -> instruction
    std::unordered_set<u32> workgroup_vars;
    bool local_size = false;
    for (size_t i = 0; i < insts.size(); ++i) {
        const auto& in = insts[i];
        const u32* w = &code[in.at];
        if (in.op == OpExecutionMode && in.words == 6 && w[2] == LocalSize) {
            local_size = w[3] == 16 && w[4] == 16 && w[5] == 1;
        } else if (in.op == OpConstant && in.words == 4) {
            constants[w[2]] = w[3];
        } else if (in.op == OpVariable && in.words >= 4 && w[3] == Workgroup) {
            workgroup_vars.insert(w[2]);
        }
        if (in.op == OpAccessChain || in.op == OpAtomicIAdd || in.op == OpIAdd) {
            definitions[w[2]] = i;
        }
    }
    if (!local_size) {
        return false;
    }
    // One shared-memory atomic append through an access chain of a workgroup variable.
    size_t atomic = SIZE_MAX;
    for (size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].op != OpAtomicIAdd) {
            continue;
        }
        const auto it = definitions.find(code[insts[i].at + 3]);
        if (it == definitions.end() || insts[it->second].op != OpAccessChain ||
            !workgroup_vars.contains(code[insts[it->second].at + 3]) || atomic != SIZE_MAX) {
            return false;
        }
        atomic = i;
    }
    if (atomic == SIZE_MAX) {
        return false;
    }
    const u32* chain = &code[insts[definitions[code[insts[atomic].at + 3]]].at];
    // base + element (Xbox: no explicit-layout block) or base + struct member + element (PC).
    const u32 chain_words = insts[definitions[code[insts[atomic].at + 3]]].words;
    if (chain_words != 5 && chain_words != 6) {
        return false;
    }
    // The reduction stages: "N > lane" (N = 16, 8, 4, 2, 1) as a block's first instruction.
    u32 lane = 0, found = 0;
    std::vector<size_t> stage_labels;
    for (size_t i = 1; i < insts.size(); ++i) {
        if (insts[i].op != OpUGreaterThan || insts[i - 1].op != OpLabel) {
            continue;
        }
        const u32* w = &code[insts[i].at];
        const auto value = constants.find(w[3]);
        if (value == constants.end() || std::popcount(value->second) != 1 || value->second > 16) {
            continue;
        }
        if ((lane && w[4] != lane) || (found & value->second)) {
            return false;
        }
        lane = w[4];
        found |= value->second;
        stage_labels.push_back(i - 1);
    }
    // An existing workgroup barrier gives the scope and semantics ids.
    const u32* barrier = nullptr;
    for (const auto& in : insts) {
        if (in.op == OpControlBarrier && in.words == 4 && constants.count(code[in.at + 1]) &&
            constants[code[in.at + 1]] == 2 && constants.count(code[in.at + 3]) &&
            constants[code[in.at + 3]] == 264) {
            barrier = &code[in.at];
            break;
        }
    }
    if (found != 0x1f || stage_labels.size() != 5 || !barrier) {
        return false;
    }
    // The candidate batch loop (innermost loop around the atomic): a barrier at the start of its
    // continue block, before the next batch reuses the shared records.
    std::unordered_map<u32, size_t> labels;
    for (size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].op == OpLabel) {
            labels[code[insts[i].at + 1]] = i;
        }
    }
    size_t loop_continue = SIZE_MAX;
    for (size_t i = 0; i < atomic; ++i) {
        if (insts[i].op != OpLoopMerge) {
            continue;
        }
        const auto merge = labels.find(code[insts[i].at + 1]);
        const auto cont = labels.find(code[insts[i].at + 2]);
        if (merge != labels.end() && cont != labels.end() && merge->second > atomic &&
            cont->second > atomic) {
            loop_continue = cont->second; // the last one before the atomic is the innermost
        }
    }
    if (loop_continue == SIZE_MAX) {
        return false;
    }
    stage_labels.push_back(loop_continue);

    u32 bound = code[3];
    const u32 c2112 = bound++, appended = bound++, slot_index = bound++, slot = bound++;
    const u32 uint_type = code[insts[atomic].at + 1];
    const u32 result = code[insts[atomic].at + 2];
    out.assign(code.begin(), code.begin() + 5);
    bool constant_done = false;
    for (size_t i = 0; i < insts.size(); ++i) {
        const auto& in = insts[i];
        if (in.op == OpFunction && !constant_done) {
            out.insert(out.end(), {(4u << 16) | OpConstant, uint_type, c2112, 2112u});
            constant_done = true;
        }
        if (i == atomic) {
            out.push_back(code[in.at]);
            out.push_back(uint_type);
            out.push_back(appended);
            out.insert(out.end(), code.begin() + in.at + 3, code.begin() + in.at + in.words);
            out.insert(out.end(), {(5u << 16) | OpIAdd, uint_type, slot_index, c2112, lane});
            // The same access chain with the element index replaced.
            out.insert(out.end(), {(chain_words << 16) | OpAccessChain, chain[1], slot, chain[3]});
            out.insert(out.end(), chain + 4, chain + chain_words - 1);
            out.push_back(slot_index);
            out.insert(out.end(), {(4u << 16) | OpStore, slot, appended, Volatile});
            out.insert(out.end(), {(3u << 16) | OpMemoryBarrier, barrier[2], barrier[3]});
            out.insert(out.end(), {(5u << 16) | OpLoad, uint_type, result, slot, Volatile});
            continue;
        }
        out.insert(out.end(), code.begin() + in.at, code.begin() + in.at + in.words);
        if (std::ranges::find(stage_labels, i) != stage_labels.end()) {
            out.insert(out.end(), {(4u << 16) | OpControlBarrier, barrier[1], barrier[2], barrier[3]});
        }
    }
    out[3] = bound;
    return constant_done;
}
} // namespace

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    thread_local std::vector<u32> patched;
    if (PatchSharedAtomicShadow(code, patched)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::printf("TLOU Xbox shadow fix: shared atomic append + reduction barriers applied "
                        "(cs 84879c67)\n");
            std::fflush(stdout);
        }
        if (const char* dir = std::getenv("BB_SHADOW_FIX_DUMP")) {
            if (FILE* f = std::fopen(dir, "wb")) {
                std::fwrite(patched.data(), 4, patched.size(), f);
                std::fclose(f);
            }
        }
        code = patched;
    } else if (code.size() > 16) {
        // Not recognized: if this is the shadow shader (its name string), keep a copy to see why.
        static const std::array<u32, 3> name = [] {
            std::array<u32, 3> words{};
            std::memcpy(words.data(), "0x84879c67\0\0", 12);
            return words;
        }();
        const auto it = std::search(code.begin(), code.end(), name.begin(), name.end());
        static std::atomic<bool> dumped{false};
        if (it != code.end() && !dumped.exchange(true)) {
            std::printf("TLOU Xbox shadow fix: cs 84879c67 not recognized (%zu words)\n",
                        code.size());
            std::fflush(stdout);
            if (const char* dir = std::getenv("BB_GPU_USER_DIR")) {
                const std::string path = std::string(dir) + "/shadow-84879c67-unpatched.spv";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(code.data(), 4, code.size(), f);
                    std::fclose(f);
                }
            }
        }
    }
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
