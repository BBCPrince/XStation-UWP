// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

namespace Vulkan {

/// bbport (Dozen, no extended dynamic state): depth bias and the stencil compare/write masks
/// go into the pipeline key instead of dynamic state. Dozen cannot set them on a D3D12 command
/// list (no dynamic depth bias on the Xbox, masks live in the PSO), so it built a second PSO for
/// every new value in the middle of a draw; now the pipeline is built with them, in the
/// background. Off unless BB_BAKED_STATE=1: pipelines loaded from the cache with it drew wrong
/// (2026-11-01: Joel without his shirt, dark screens) while the same ones built in game did not.
inline bool BakedDynamicState() {
    static const bool value = [] {
        const char* env = std::getenv("BB_BAKED_STATE");
        return env && env[0] == '1';
    }();
    return value;
}

} // namespace Vulkan
