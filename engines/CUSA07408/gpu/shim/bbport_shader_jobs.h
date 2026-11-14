// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: shader translations in flight (2026-11-03). A graphics stage that needs a program or
// permutation not translated yet is translated on worker threads while its draws are skipped
// (vk_pipeline_cache.cpp, BB_ASYNC_SHADERS). A translation reads guest memory: the code, the
// draw's user-data tables and descriptors. That memory stays as the draw left it only until the
// guest learns the GPU went past it, so every guest-visible signal (fence writes, label writes,
// DMA to guest memory, interrupts) waits here for the translations started before it. The waiting
// thread runs queued translations itself meanwhile.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace BbShaderJobs {

inline std::atomic<unsigned> in_flight{0};
inline std::mutex done_mutex;
inline std::condition_variable done_cv;
/// Runs one queued translation on the calling thread; false when none is queued.
inline std::atomic<bool (*)()> help{nullptr};

inline void Started() {
    in_flight.fetch_add(1, std::memory_order_acq_rel);
}

inline void Finished() {
    if (in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard lock{done_mutex};
        done_cv.notify_all();
    }
}

/// Before a signal the guest can see: no translation started earlier is still reading.
inline void BeforeGuestSignal() {
    if (in_flight.load(std::memory_order_acquire) == 0) [[likely]] {
        return;
    }
    while (in_flight.load(std::memory_order_acquire) != 0) {
        if (const auto run = help.load(std::memory_order_acquire); run && run()) {
            continue;
        }
        std::unique_lock lock{done_mutex};
        done_cv.wait_for(lock, std::chrono::milliseconds(1),
                         [] { return in_flight.load(std::memory_order_acquire) == 0; });
    }
}

} // namespace BbShaderJobs
