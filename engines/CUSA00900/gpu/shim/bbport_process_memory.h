// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace BbProcessMemory {

struct Usage {
    std::uint64_t private_bytes; ///< committed memory of the process (Linux: its virtual size)
    std::uint64_t working_set;   ///< in RAM now
    std::uint64_t app_bytes;    ///< combined app charge, including shared GPU memory
    std::uint64_t available_commit;
    bool app_info_valid;
};

Usage Get();
// Same clock as the UWP app-memory CSV on Windows.
std::uint64_t TickMilliseconds();
// Return already-freed Windows heap cache pages to the OS under pressure.
void TrimUnusedHeapCaches();

} // namespace BbProcessMemory
