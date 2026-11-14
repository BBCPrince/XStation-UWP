// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

Id SubgroupScope(EmitContext& ctx) {
    return ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup));
}

Id EmitWarpId(EmitContext& ctx) {
    UNREACHABLE();
}

Id EmitLaneId(EmitContext& ctx) {
    return ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
}

Id EmitQuadBroadcast(EmitContext& ctx, Id value, Id index) {
    return ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], SubgroupScope(ctx), value, index);
}

Id EmitReadFirstLane(EmitContext& ctx, Id value) {
    const Id first = ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], SubgroupScope(ctx), value);
    // bbport (Uncharted Xbox port 2026-11-04, ported to Spider-Man 2026-11-05): GCN "waterfall" loops
    // (v_readfirstlane, matching lanes work and leave, repeat). D3D12 (Dozen) leaves helper lanes out
    // of wave ops, so a helper lane with another index never matches and loops forever (GPU hang /
    // seconds per frame). In pixel shaders a helper lane reads its own value; real lanes unchanged.
    // BB_READFIRSTLANE_HELPERS=0: old behaviour.
    if (ctx.sw_stage == SwStage::Fragment && Sirit::ValidId(ctx.helper_invocation)) {
        static const bool keep = [] {
            const char* env = std::getenv("BB_READFIRSTLANE_HELPERS");
            return env && env[0] == '0';
        }();
        if (!keep) {
            const Id is_helper = ctx.OpLoad(ctx.U1[1], ctx.helper_invocation);
            return ctx.OpSelect(ctx.U32[1], is_helper, value, first);
        }
    }
    return first;
}

// Detroit (2026-11-08): GCN's "wave maximum" (WaveMaxPatternPass). Only lanes in the loop count. As in
// EmitReadFirstLane, a helper lane reads its own value: D3D12 (Dozen) leaves helper lanes out of wave
// ops, and such a lane would never match the maximum and spin to the loop guard.
Id EmitWaveMaxU32(EmitContext& ctx, Id value) {
    const Id max = ctx.OpGroupNonUniformUMax(ctx.U32[1], SubgroupScope(ctx),
                                             spv::GroupOperation::Reduce, value);
    if (ctx.sw_stage == SwStage::Fragment && Sirit::ValidId(ctx.helper_invocation)) {
        const Id is_helper = ctx.OpLoad(ctx.U1[1], ctx.helper_invocation);
        return ctx.OpSelect(ctx.U32[1], is_helper, value, max);
    }
    return max;
}

Id EmitShuffle(EmitContext& ctx, Id value, Id index) {
    return ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, index);
}

Id EmitShuffleXor(EmitContext& ctx, Id value, Id mask) {
    return ctx.OpGroupNonUniformShuffleXor(ctx.U32[1], SubgroupScope(ctx), value, mask);
}

Id EmitReadLane(EmitContext& ctx, Id value, Id lane) {
    return ctx.OpGroupNonUniformBroadcast(ctx.U32[1], SubgroupScope(ctx), value, lane);
}

Id EmitWriteLane(EmitContext& ctx, Id value, Id write_value, u32 lane) {
    return ctx.u32_zero_value;
}

Id EmitBallot(EmitContext& ctx, Id bit) {
    const Id ballot{ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), bit)};
    return ctx.OpBitcast(ctx.U64, ctx.OpVectorShuffle(ctx.U32[2], ballot, ballot, 0, 1));
}

Id EmitBallotFindLsb(EmitContext& ctx, Id mask) {
    const Id value{ctx.OpCompositeConstruct(ctx.U32[4], ctx.OpBitcast(ctx.U32[2], mask),
                                            ctx.u32_zero_value, ctx.u32_zero_value)};
    return ctx.OpGroupNonUniformBallotFindLSB(ctx.U32[1], SubgroupScope(ctx), value);
}

Id EmitInverseBallot(EmitContext& ctx, Id mask) {
    const Id value{ctx.OpCompositeConstruct(ctx.U32[4], ctx.OpBitcast(ctx.U32[2], mask),
                                            ctx.u32_zero_value, ctx.u32_zero_value)};
    return ctx.OpGroupNonUniformInverseBallot(ctx.U1[1], SubgroupScope(ctx), value);
}

Id EmitGroupAny(EmitContext& ctx, Id bit) {
    return ctx.OpGroupNonUniformAny(ctx.U1[1], SubgroupScope(ctx), bit);
}

} // namespace Shader::Backend::SPIRV
