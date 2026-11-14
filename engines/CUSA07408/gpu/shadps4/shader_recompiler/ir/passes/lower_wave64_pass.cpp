// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <magic_enum/magic_enum.hpp>
#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsDivergentCondition(const IR::U1& condition) {
    if (condition.IsImmediate()) {
        return false;
    }
    const IR::Inst* const condition_inst = condition.Inst();
    return IR::BreadthFirstSearch(condition_inst,
                                  [](const IR::Inst* inst) -> std::optional<bool> {
                                      switch (inst->GetOpcode()) {
                                      case IR::Opcode::LaneId:
                                          return true;
                                      case IR::Opcode::GetAttributeU32:
                                          if (inst->Arg(0).Attribute() ==
                                              IR::Attribute::LocalInvocationId) {
                                              return true;
                                          }
                                          break;
                                      default:
                                          break;
                                      }
                                      return std::nullopt;
                                  })
        .value_or(false);
}

static std::vector<IR::Block*> FindUniformBlocks(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;

    struct ConditionalScope {
        const IR::Block* merge;
        bool divergent;
    };

    std::vector<IR::Block*> blocks;
    std::vector<ConditionalScope> conditionals;
    std::vector<const IR::Block*> loops;
    u32 divergence_depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsDivergentCondition(node.data.if_node.cond);
            conditionals.push_back({node.data.if_node.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::EndIf:
            ASSERT(!conditionals.empty() && conditionals.back().merge == node.data.end_if.merge);
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Loop:
            loops.push_back(node.data.loop.merge);
            break;
        case Type::Repeat:
            if (loops.empty() || loops.back() != node.data.repeat.merge) {
                return {};
            }
            loops.pop_back();
            break;
        case Type::Block:
            if (divergence_depth == 0 && loops.empty()) {
                blocks.push_back(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty() || !loops.empty()) {
        return {};
    }
    return blocks;
}

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || profile.subgroup_size == 64) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    if (num_threads <= 32) {
        return;
    }

    std::vector<IR::Inst*> worklist;
    // BB_WAVE64_EXTRA=1: the InverseBallot / ReadFirstLane lowerings below (experiment).
    static const bool wave64_extra = [] {
        const char* env = std::getenv("BB_WAVE64_EXTRA");
        return env && env[0] == '1';
    }();
    const auto uniform_blocks = FindUniformBlocks(program);
    for (IR::Block* block : program.blocks) {
        const bool is_uniform = std::ranges::contains(uniform_blocks, block);
        const auto push_worklist = [&](IR::Inst& inst) {
            if (is_uniform) {
                worklist.push_back(&inst);
            } else {
                LOG_WARNING(Render_Recompiler, "{} instruction in non uniform control flow",
                            inst.GetOpcode());
                // God of War port (2026-11-03): printed once per shader (the log drops it): such
                // an instruction sees 32 lanes of the 64.
                static std::mutex said_lock;
                static std::set<std::pair<u64, IR::Opcode>> said;
                std::scoped_lock lock{said_lock};
                if (said.emplace(program.info.pgm_hash, inst.GetOpcode()).second) {
                    std::printf("Wave64: shader %#llx: %s in non-uniform control flow is not "
                                "lowered\n",
                                static_cast<unsigned long long>(program.info.pgm_hash),
                                std::string(magic_enum::enum_name(inst.GetOpcode())).c_str());
                }
            }
        };
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate()) {
                push_worklist(inst);
            } else if (inst.GetOpcode() == IR::Opcode::Ballot) {
                const auto is_unpack = [](const IR::Use& use) {
                    return use.user->GetOpcode() == IR::Opcode::UnpackUint2x32;
                };
                if (std::ranges::any_of(inst.Uses(), is_unpack)) {
                    push_worklist(inst);
                }
            } else if (inst.GetOpcode() == IR::Opcode::GetAttributeU32 &&
                       inst.Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) {
                worklist.push_back(&inst);
            } else if (wave64_extra && inst.GetOpcode() == IR::Opcode::InverseBallot) {
                // God of War port (2026-11-03), experiment, off: InverseBallot testing bit
                // (index & 63). Wrong as a rule: exec masks from Ballots left native hold each
                // subgroup's own 32 lanes in the low word, which the native InverseBallot
                // matches; lowering it broke the frame (posterized colours, boot59w64).
                worklist.push_back(&inst);
            } else if (wave64_extra && inst.GetOpcode() == IR::Opcode::ReadFirstLane) {
                // Experiment, off: ReadFirstLane in uniform blocks as lane 0 of the 64.
                push_worklist(inst);
            }
        }
    }
    // God of War port (2026-11-03): an InverseBallot over a mask built from lowered Ballots (64-lane
    // masks) tests the bit of the invocation's lane in the 64-lane wave. Left native it tested bits
    // 0..31 in both halves: in the tile-list builder (cs 0x417fd8da) lanes 32-40 took the bits of
    // lanes 0-8 ("id < 9 && count > 0"), added garbage counts with the global atomic and broke the
    // tile lists of the tiled lighting (the checkerboard). Masks from Ballots left native (32-lane,
    // per subgroup) keep the native InverseBallot, which matches them. BB_WAVE64_INVBALLOT=0: off.
    static const bool invballot_on = [] {
        const char* env = std::getenv("BB_WAVE64_INVBALLOT");
        return !(env && env[0] == '0');
    }();
    if (invballot_on && !wave64_extra) {
        std::set<const IR::Inst*> lowered_ballots;
        for (const IR::Inst* inst : worklist) {
            if (inst->GetOpcode() == IR::Opcode::Ballot ||
                inst->GetOpcode() == IR::Opcode::GetAttributeU32) {
                lowered_ballots.insert(inst);
            }
        }
        // Leaves of the mask: lowered (64-lane) or native (32-lane) Ballots; other values are
        // neutral (immediates, saved masks). Through 64-bit bitwise ops and selects only.
        const auto classify = [&](const IR::Value& root, bool& lowered, bool& native) {
            std::vector<IR::Value> stack{root};
            std::set<const IR::Inst*> seen;
            while (!stack.empty()) {
                const IR::Value v = stack.back();
                stack.pop_back();
                const IR::Inst* inst = v.TryInst();
                if (!inst || !seen.insert(inst).second || seen.size() > 64) {
                    continue;
                }
                switch (inst->GetOpcode()) {
                case IR::Opcode::Ballot:
                    (lowered_ballots.contains(inst) ? lowered : native) = true;
                    break;
                case IR::Opcode::GetAttributeU32:
                    lowered |= lowered_ballots.contains(inst);
                    break;
                case IR::Opcode::BitwiseAnd64:
                case IR::Opcode::BitwiseOr64:
                case IR::Opcode::BitwiseXor64:
                case IR::Opcode::BitwiseNot64:
                case IR::Opcode::SelectU64:
                case IR::Opcode::PackUint2x32:
                case IR::Opcode::UnpackUint2x32:
                case IR::Opcode::CompositeConstructU32x2:
                case IR::Opcode::CompositeExtractU32x2:
                case IR::Opcode::Phi:
                    for (size_t i = 0; i < inst->NumArgs(); ++i) {
                        stack.push_back(inst->Arg(i));
                    }
                    break;
                default:
                    break;
                }
            }
        };
        u32 lowered_count = 0, total = 0, mixed = 0;
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::InverseBallot) {
                    continue;
                }
                bool lowered = false, native = false;
                classify(inst.Arg(0), lowered, native);
                ++total;
                mixed += lowered && native;
                if (lowered && !native) {
                    worklist.push_back(&inst);
                    ++lowered_count;
                }
            }
        }
        if (total) {
            static std::mutex said_lock;
            static std::set<u64> said;
            std::scoped_lock lock{said_lock};
            if (said.insert(program.info.pgm_hash).second) {
                std::printf("Wave64: shader %#llx: %u of %u InverseBallot of 64-lane masks "
                            "lowered (%u mixed)\n",
                            static_cast<unsigned long long>(program.info.pgm_hash), lowered_count,
                            total, mixed);
            }
        }
    }
    if (worklist.empty()) {
        return;
    }

    const u32 scratch_base = Common::AlignUp(runtime_info.hw.cs.shared_memory_size, sizeof(u64));
    const u32 scratch_size =
        (Common::AlignUp(num_threads, 64) / profile.subgroup_size) * sizeof(u32);
    program.info.shared_memory_scratch_size =
        scratch_base + scratch_size - runtime_info.hw.cs.shared_memory_size;

    for (IR::Inst* inst : worklist) {
        LOG_INFO(Render_Recompiler, "Lowering {} instruction for wave64", inst->GetOpcode());
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 subgroup_id = ir.ShiftRightLogical(invocation_index, ir.Imm32(5));
        if (inst->GetOpcode() == IR::Opcode::Ballot) {
            const IR::U32 mask_low =
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, mask_low, offset);
            ir.Barrier();
            // God of War port (2026-11-03): the two 32-lane halves read as two 32-bit values, not
            // one 64-bit value over the same bytes: that aliasing needs
            // VK_KHR_workgroup_memory_explicit_layout, which Dozen lacks (the shader was invalid
            // SPIR-V once the game's own shared memory was 32-bit too).
            const IR::U32 pair = ir.BitwiseAnd(offset, ir.Imm32(~7u));
            const IR::U32 low = IR::U32{ir.LoadShared(32, false, pair)};
            const IR::U32 high = IR::U32{ir.LoadShared(32, false, ir.IAdd(pair, ir.Imm32(4u)))};
            const IR::U64 mask = ir.PackUint2x32(ir.CompositeConstruct(low, high));
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(mask);
        } else if (inst->GetOpcode() == IR::Opcode::ReadLane) {
            const IR::U32 lane32 = ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(31));
            const IR::U32 half = ir.ShiftRightLogical(IR::U32{inst->Arg(1)}, ir.Imm32(5u));
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadLane(IR::U32{inst->Arg(0)}, lane32), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false,
                                      ir.IAdd(ir.BitwiseAnd(offset, ir.Imm32(~7u)),
                                              ir.ShiftLeftLogical(half, ir.Imm32(2u))))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::InverseBallot) {
            const IR::U32 lane = ir.BitwiseAnd(invocation_index, ir.Imm32(63));
            const IR::Value words = ir.UnpackUint2x32(IR::U64{inst->Arg(0)});
            const IR::U32 low{ir.CompositeExtract(words, 0)}, high{ir.CompositeExtract(words, 1)};
            const IR::U32 word{
                ir.Select(ir.IGreaterThanEqual(lane, ir.Imm32(32u), false), high, low)};
            const IR::U32 bit{ir.BitwiseAnd(
                ir.ShiftRightLogical(word, ir.BitwiseAnd(lane, ir.Imm32(31u))), ir.Imm32(1u))};
            inst->ReplaceUsesWithAndRemove(ir.INotEqual(bit, ir.Imm32(0u)));
        } else if (inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadLane(IR::U32{inst->Arg(0)}, ir.Imm32(0u)), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false, ir.BitwiseAnd(offset, ir.Imm32(~7u)))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) {
            const IR::U32 subgroup_invocation_id = ir.BitwiseAnd(invocation_index, ir.Imm32(63));
            const IR::U64 mask = ir.ISub(
                ir.ShiftLeftLogical(ir.Imm64(u64{1}), subgroup_invocation_id), ir.Imm64(u64{1}));
            inst->ReplaceUsesWithAndRemove(
                ir.CompositeExtract(ir.UnpackUint2x32(mask), inst->Arg(1).U32()));
        }
    }
}

// God of War port (2026-11-03): v_writelane_b32 writes one lane of a VGPR and keeps the others.
// The SPIR-V backend emitted 0 for every lane (EmitWriteLane was a stub), wiping the register:
// God of War's tiled lighting (cs 0xf028e7c5 and its seven variants, 8 writelanes each) builds
// per-lane data with it, and its tiles came out dark (the checkerboard). The lane is the 64-lane
// wave's where compute waves are emulated on 32-lane subgroups, else the subgroup's.
void LowerWriteLanePass(IR::Program& program, const RuntimeInfo& runtime_info,
                        const Profile& profile) {
    // BB_WRITELANE=1 (off by default until tested on a scene that reads the lanes back: in the
    // tile lighting the written register turned out unused).
    static const bool enabled = [] {
        const char* env = std::getenv("BB_WRITELANE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    bool wave64_index = false;
    if (program.info.hw_stage == HwStage::Compute && profile.subgroup_size != 64) {
        const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
        wave64_index = size_x * size_y * size_z > 32;
    }
    std::vector<IR::Inst*> worklist;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::WriteLane) {
                worklist.push_back(&inst);
            }
        }
    }
    for (IR::Inst* inst : worklist) {
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 lane_id =
            wave64_index ? IR::U32{ir.BitwiseAnd(
                               ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex), ir.Imm32(63u))}
                         : ir.LaneId();
        const IR::U32 old_value{inst->Arg(0)}, write_value{inst->Arg(1)}, lane{inst->Arg(2)};
        const IR::U32 result{ir.Select(ir.IEqual(lane_id, lane), write_value, old_value)};
        inst->ReplaceUsesWithAndRemove(result);
    }
}

} // namespace Shader::Optimization
