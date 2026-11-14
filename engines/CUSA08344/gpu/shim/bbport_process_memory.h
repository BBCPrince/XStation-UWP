// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace BbProcessMemory {

struct Usage {
    std::uint64_t private_bytes; ///< committed memory of the process (Linux: its virtual size)
    std::uint64_t working_set;   ///< in RAM now
};

Usage Get();

/// bbport: how close the process is to a hard commit limit (UWP on Xbox: 5 GiB for everything,
/// guest memory, GPU memory and the host; at the limit an allocation fails and the game dies).
/// Level 0: no limit known, or more than 1 GiB left; 1: under 1 GiB left; 2: under 600 MiB;
/// 3: under 300 MiB. The caches evict what is idle sooner the higher the level.
struct AppPressure {
    std::uint64_t usage; ///< private + shared commit
    std::uint64_t limit; ///< 0: none known
    std::uint64_t trigger, pressure, critical; ///< usage at levels 1, 2, 3
    int level;
};

/// Measured at most once a second (cheap to call per submission).
AppPressure GetAppPressure();

/// TLOU debug: whether the page holding `address` is in the working set (QueryWorkingSetEx).
bool IsResident(const void* address);

/// TLOU debug: the calling thread's CPU time so far (100 ns units) and cycles.
struct ThreadTimes {
    std::uint64_t user, kernel, cycles;
};
ThreadTimes CurrentThreadTimes();

} // namespace BbProcessMemory
