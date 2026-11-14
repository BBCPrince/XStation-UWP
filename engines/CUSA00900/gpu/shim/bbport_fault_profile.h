// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>

namespace BbFaultProfile {
inline std::atomic<std::uint64_t> calls{0}, buffer_ns{0}, texture_ns{0}, window_ns{0};
inline std::atomic<std::uint64_t> tracker_lock_ns{0}, tracker_state_ns{0}, readback_wait_ns{0}, readbacks{0};
inline bool Enabled() {
    static const bool enabled = [] {
        const char* flag = std::getenv("BB_CPU_PROFILE");
        return flag && flag[0] == '1';
    }();
    return enabled;
}
struct Scope {
    std::atomic<std::uint64_t>& total;
    bool enabled = Enabled();
    std::chrono::steady_clock::time_point start = enabled ? std::chrono::steady_clock::now()
                                                         : std::chrono::steady_clock::time_point{};
    ~Scope() {
        if (enabled) total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count(), std::memory_order_relaxed);
    }
};
}

// The same opt-in CPU profile also measures nested renderer work. These are wall
// times across threads, not additive CPU utilization or a critical-path trace.
namespace BbFrameProfile {
using Scope = BbFaultProfile::Scope;
inline std::atomic<std::uint64_t> draw_ns{0}, pipeline_ns{0}, render_state_ns{0},
    resources_ns{0}, descriptors_ns{0}, dynamic_ns{0}, recorder_ns{0},
    deferred_wait_ns{0}, host_wait_ns{0}, submit_ns{0};
inline std::atomic<std::uint64_t> recorder_user_ns{0}, recorder_kernel_ns{0};
// Dense arena growth can migrate a large buffer for a small newly bound tail.
// Keep this separate from upload/fault time to identify repeated replacement
// work during camera movement. Counters are updated only with profiling on.
inline std::atomic<std::uint64_t> arena_growth_ns{0}, arena_drain_ns{0},
    arena_creates{0}, arena_migrations{0}, arena_new_bytes{0}, arena_old_bytes{0},
    arena_copy_bytes{0}, arena_drains{0};
}
