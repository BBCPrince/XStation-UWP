// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

// Detroit (2026-11-08): the third argument is the workgroup id (3 dwords) for walkers whose
// tables depend on it (PersistentSrtInfo::workgroup_dependent); never null.
using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/,
                                            const u32* /*workgroup*/);
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs
    u32 workgroup_dependent = 0;   // Detroit: workgroup id components the tables depend on
    // Detroit (2026-11-08), bindless materials. A vertex shader exporting
    // float(user data SGPR k [+ instance]) + 0.5 as attribute N.c: bit 31 | k << 16 | N << 8 | c.
    u32 draw_value_out = 0;
    // A pixel shader whose tables are reached through int(attribute N.c) (the walker reads it
    // from its third argument, word 0): bit 31 | N << 8 | c.
    u32 draw_value_in = 0;

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
