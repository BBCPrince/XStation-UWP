// SPDX-License-Identifier: GPL-2.0-or-later
// Detroit (2026-11-11), BB_DT_TIMELINE=<flip>: from that guest flip on, the next 240 flips' worth of
// events (guest submissions, the graphics queue's label waits, end-of-pipe writes, flips) with
// their times, printed at once when done. Shows which side of the CPU/GPU chain each frame waits
// on. Lock-free: one atomic index into a fixed ring; events past the ring are dropped.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace BbTimeline {

struct Event {
    std::uint64_t ns;
    char type;
    std::uint32_t tid;
    std::uint64_t a, b;
};

constexpr std::uint32_t Capacity = 65536;
// Defined once in bbport_sampler.cpp (inline variables were not shared between this header's users).
extern Event ring[Capacity];
extern std::atomic<std::uint32_t> used;
extern std::atomic<int> state; // 0 waiting, 1 recording, 2 printed
extern std::atomic<std::uint64_t> flips;

inline std::uint64_t Start() {
    static const std::uint64_t start = [] {
        const char* env = std::getenv("BB_DT_TIMELINE");
        return env ? std::strtoull(env, nullptr, 10) : 0ull;
    }();
    return start;
}

/// BB_DT_TIMELINE_HEAVY=<ms>: with BB_DT_TIMELINE=<flip>, recording starts at the first flip from
/// <flip> on where the last 16 flip intervals averaged <ms> or more (a heavy stretch), for 150 flips.
inline double HeavyMs() {
    static const double ms = [] {
        const char* env = std::getenv("BB_DT_TIMELINE_HEAVY");
        return env ? std::strtod(env, nullptr) : 0.0;
    }();
    return ms;
}

inline std::uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::uint32_t CurrentTid();

inline void Note(char type, std::uint64_t a = 0, std::uint64_t b = 0) {
    if (state.load(std::memory_order_relaxed) != 1) {
        return;
    }
    const std::uint32_t i = used.fetch_add(1, std::memory_order_relaxed);
    if (i >= Capacity) {
        return;
    }
    ring[i] = {NowNs(), type, CurrentTid(), a, b};
}

/// Called for every guest flip request (VideoOut).
inline void OnFlip(std::uint64_t index) {
    if (!Start()) {
        return;
    }
    const std::uint64_t n = flips.fetch_add(1, std::memory_order_relaxed) + 1;
    static std::uint64_t last_ns = 0, began_at = 0;
    static double recent[16] = {};
    const std::uint64_t now_ns = NowNs();
    recent[n % 16] = last_ns ? (now_ns - last_ns) / 1e6 : 0.0;
    last_ns = now_ns;
    if (HeavyMs() > 0.0) {
        if (n >= Start() && state.load(std::memory_order_relaxed) == 0) {
            double sum = 0.0, worst = 0.0;
            for (const double v : recent) {
                sum += v;
                worst = v > worst ? v : worst;
            }
            if (sum / 16 >= HeavyMs() && worst < 100.0) {
                began_at = n;
                state.store(1, std::memory_order_release);
            }
        }
    } else if (n == Start()) {
        began_at = n;
        state.store(1, std::memory_order_release);
    }
    Note('F', index, n);
    const std::uint64_t length = HeavyMs() > 0.0 ? 150 : 240;
    if (began_at && n == began_at + length && state.exchange(2) == 1) {
        const std::uint32_t count = std::min<std::uint32_t>(used.load(), Capacity);
        const std::uint64_t t0 = count ? ring[0].ns : 0;
        std::printf("Timeline: %u events, %llu flips from flip %llu (ms from the first event; tid; "
                    "type a b)\n",
                    count, (unsigned long long)length, (unsigned long long)began_at);
        for (std::uint32_t k = 0; k < count; ++k) {
            const auto& e = ring[k];
            std::printf("TL %.3f %u %c %#llx %#llx\n", (e.ns - t0) / 1e6, e.tid, e.type,
                        (unsigned long long)e.a, (unsigned long long)e.b);
        }
        std::fflush(stdout);
    }
}

} // namespace BbTimeline
