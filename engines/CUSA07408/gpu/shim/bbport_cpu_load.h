// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: how busy each CPU core is, for the FPS counter.
#pragma once

#include <vector>

namespace BbCpuLoad {
struct Loads {
    std::vector<float> cores; ///< 0-100 per logical core, empty when the system does not say
    float app = 0.0f;         ///< the app's own CPU time, in cores (1.0 = one core fully)
};
/// The load over the last second; samples at most once a second (cheap to call every frame).
const Loads& Get();
} // namespace BbCpuLoad
