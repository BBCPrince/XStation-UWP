// SPDX-License-Identifier: GPL-2.0-or-later
// TLOU (2026-11-09), BB_FG_SCAN=1: finds what frame generation needs in a game without the camera
// hookup (CameraMotion knows Bloodborne's constants only). Logs (stdout, "FG scan:"):
//  - the render-target setups of draws with a depth target: color target count, depth size and
//    draws per 5 s, to find the scene (G-buffer) pass;
//  - perspective projection matrices in the constant buffers of those draws: shader, buffer size,
//    float offset and layout (row or column vectors), first sight of each, plus the 16 floats
//    before and after, to find the view matrix next to it.
#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

#include "common/types.h"

namespace FgScan {

inline bool On() {
    static const bool on = [] {
        const char* env = std::getenv("BB_FG_SCAN");
        return env && env[0] == '1';
    }();
    return on;
}

struct State {
    std::mutex mutex;
    std::map<std::tuple<u32, u32, u32>, u64> targets; // (colors, depth w, depth h) -> draws
    std::map<std::tuple<u64, u64, u32, int>, u64> found; // (shader, size, offset, layout) -> hits
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    u32 prints = 0;
};

inline thread_local u32 depth_w = 0, depth_h = 0;

inline State& Get() {
    static State state;
    return state;
}

inline void PrintLocked(State& s) {
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last < std::chrono::seconds(5) || s.prints >= 40) {
        return;
    }
    s.last = now;
    ++s.prints;
    std::printf("FG scan: targets (colors, depth WxH: draws/5 s):");
    for (const auto& [key, n] : s.targets) {
        std::printf(" [%u, %ux%u: %llu]", std::get<0>(key), std::get<1>(key), std::get<2>(key),
                    static_cast<unsigned long long>(n));
    }
    std::printf("\n");
    for (const auto& [key, n] : s.found) {
        std::printf("FG scan: matrix kind %d in shader %016llx buffer %llu B at float %u: %llu "
                    "draws/5 s\n",
                    std::get<3>(key), static_cast<unsigned long long>(std::get<0>(key)),
                    static_cast<unsigned long long>(std::get<1>(key)), std::get<2>(key),
                    static_cast<unsigned long long>(n));
    }
    s.targets.clear();
    s.found.clear();
    std::fflush(stdout);
}

/// A draw with a depth target: `colors` color targets, depth `w` x `h`.
inline void NoteTargets(u32 colors, u32 w, u32 h) {
    depth_w = w;
    depth_h = h;
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    ++s.targets[{colors, w, h}];
    PrintLocked(s);
}

/// 1 if m (16 floats) looks like a perspective projection with row vectors (D3D: 1 in [2][3]),
/// 2 with column vectors (1 in [3][2]); jitter may sit in the third row / column.
inline int ProjectionLayout(const float* m) {
    const auto zero = [](float v) { return std::fabs(v) < 1e-6f; };
    const auto scale = [](float v) { return std::fabs(v) > 0.2f && std::fabs(v) < 8.0f; };
    const auto unit = [](float v) { return std::fabs(std::fabs(v) - 1.0f) < 1e-4f; };
    if (!scale(m[0]) || !scale(m[5]) || !zero(m[1]) || !zero(m[3]) || !zero(m[4]) || !zero(m[7]) ||
        !zero(m[15]) || std::fabs(std::fabs(m[0] / m[5]) - 0.5625f) > 0.01f) {
        return 0;
    }
    // Row vectors: [xs 0 0 0][0 ys 0 0][jx jy zs ±1][0 0 zo 0]
    if (zero(m[2]) && zero(m[6]) && unit(m[11]) && zero(m[12]) && zero(m[13]) && !zero(m[14])) {
        return 1;
    }
    // Column vectors: [xs 0 jx 0][0 ys jy 0][0 0 zs zo][0 0 ±1 0]
    if (zero(m[8]) && zero(m[9]) && unit(m[14]) && zero(m[12]) && zero(m[13]) && !zero(m[11])) {
        return 2;
    }
    return 0;
}

/// 3 if m looks like a combined view-projection with row vectors (clip = v * m), 4 with column
/// vectors (clip = m * v): the w axis is the view's unit z axis, the z axis is parallel to it
/// (depth), and x and y (minus any jitter along w) are perpendicular, scaled by the projection.
inline int ViewProjectionLayout(const float* m) {
    for (int layout = 3; layout <= 4; ++layout) {
        // Axis k (0..3) as a 3-vector: column k for row vectors, row k for column vectors.
        const auto axis = [&](int k) {
            return layout == 3 ? std::array<float, 3>{m[k], m[4 + k], m[8 + k]}
                               : std::array<float, 3>{m[4 * k], m[4 * k + 1], m[4 * k + 2]};
        };
        const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        };
        const auto w = axis(3);
        if (std::fabs(dot(w, w) - 1.0f) > 1e-3f) {
            continue;
        }
        auto x = axis(0), y = axis(1), z = axis(2);
        const float xw = dot(x, w), yw = dot(y, w), zw = dot(z, w);
        for (int i = 0; i < 3; ++i) {
            x[i] -= xw * w[i];
            y[i] -= yw * w[i];
            z[i] -= zw * w[i];
        }
        const float lx = std::sqrt(dot(x, x)), ly = std::sqrt(dot(y, y));
        if (lx < 0.2f || lx > 8.0f || ly < 0.2f || ly > 8.0f || std::fabs(zw) < 1e-4f ||
            std::sqrt(dot(z, z)) > 1e-3f * std::fabs(zw) || std::fabs(dot(x, y)) > 1e-3f * lx * ly ||
            std::fabs(lx / ly - 0.5625f) > 0.01f) {
            continue;
        }
        return layout;
    }
    return 0;
}

/// Per frame: the 64-byte per-draw transforms (TLOU vertex shaders: world-view-projection,
/// column vectors) of draws with the 1920x1080 scene depth, counted by value. Static world
/// geometry has no world transform, so the most common value should be the camera's.
struct Modes {
    std::map<std::array<float, 16>, u32> counts;
    u32 frames = 0;
};

inline Modes& GetModes() {
    static Modes modes;
    return modes;
}

inline void NoteTransform(const float* data) {
    if (depth_w != 1920 || depth_h != 1080 || ViewProjectionLayout(data) != 4) {
        return;
    }
    std::array<float, 16> m;
    std::copy(data, data + 16, m.begin());
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    ++GetModes().counts[m];
}

/// Once per presented frame.
inline void EndFrame() {
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    auto& modes = GetModes();
    ++modes.frames;
    if (modes.counts.empty()) {
        return;
    }
    u32 total = 0, best = 0, second = 0;
    const std::array<float, 16>* top = nullptr;
    for (const auto& [m, n] : modes.counts) {
        total += n;
        if (n > best) {
            second = best;
            best = n;
            top = &m;
        } else if (n > second) {
            second = n;
        }
    }
    const auto& m = *top;
    // Column vectors: row 3 is the view's z axis (w), row 2 = zs * row 3 + zo, rows 0 and 1 are
    // the scaled x and y axes plus jitter along w.
    const auto dot3 = [&](int a, int b) {
        return m[4 * a] * m[4 * b] + m[4 * a + 1] * m[4 * b + 1] + m[4 * a + 2] * m[4 * b + 2];
    };
    const float zs = dot3(2, 3), zo = m[11] - zs * m[15];
    const float jx = dot3(0, 3), jy = dot3(1, 3);
    const float xs = std::sqrt(std::max(0.0f, dot3(0, 0) - jx * jx));
    const float ys = std::sqrt(std::max(0.0f, dot3(1, 1) - jy * jy));
    std::printf("FG scan: frame %u: %u transforms, %zu values, top %u, second %u; xs %.4f ys %.4f "
                "zs %.6f zo %.5f jitter %.5f %.5f; w row %.4f %.4f %.4f %.3f\n",
                modes.frames, total, modes.counts.size(), best, second, xs, ys, zs, zo, jx, jy,
                m[12], m[13], m[14], m[15]);
    modes.counts.clear();
}

/// The constants (`size` bytes) of shader `shader` in a draw with a depth target.
inline void ScanConstants(const float* data, u64 size, u64 shader) {
    if (!data || size < 64 || size > 64 * 1024) {
        return;
    }
    if (size == 64) {
        NoteTransform(data);
    }
    const u32 floats = u32(size / 4);
    for (u32 i = 0; i + 16 <= floats; i += 4) {
        int layout = ProjectionLayout(data + i);
        if (!layout) {
            layout = ViewProjectionLayout(data + i);
        }
        if (!layout) {
            continue;
        }
        auto& s = Get();
        std::scoped_lock lk{s.mutex};
        static std::set<std::tuple<u64, u64, u32, int>> seen;
        const auto key = std::make_tuple(shader, size, i, layout + 10 * int(depth_w));
        if (seen.insert(key).second && seen.size() <= 200) {
            static constexpr const char* kinds[] = {"", "projection, row vectors",
                "projection, column vectors", "view-projection, row vectors",
                "view-projection, column vectors"};
            std::printf("FG scan: NEW %s, depth %ux%u, shader %016llx buffer %llu B float %u:", kinds[layout], depth_w, depth_h,
                        static_cast<unsigned long long>(shader),
                        static_cast<unsigned long long>(size), i);
            const u32 from = i;
            const u32 to = std::min(floats, i + 32);
            for (u32 k = from; k < to; ++k) {
                std::printf("%s%g", k == i ? " | " : " ", data[k]);
            }
            std::printf("\n");
        }
        ++s.found[key];
        PrintLocked(s);
    }
}

} // namespace FgScan
