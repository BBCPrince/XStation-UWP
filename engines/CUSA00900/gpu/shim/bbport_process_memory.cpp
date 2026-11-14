// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the process's own memory, for the once-a-minute memory line in the log.
#include "bbport_process_memory.h"
#include <cstdio>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace BbProcessMemory {

std::uint64_t TickMilliseconds() {
#ifdef _WIN32
    return GetTickCount64();
#else
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

Usage Get() {
    Usage usage{};
#ifdef _WIN32
    APP_MEMORY_INFORMATION app{};
    if (GetProcessInformation(GetCurrentProcess(), ProcessAppMemoryInfo, &app, sizeof(app))) {
        usage.app_bytes = app.TotalCommitUsage;
        usage.available_commit = app.AvailableCommit;
        usage.app_info_valid = true;
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters))) {
        usage.private_bytes = counters.PrivateUsage;
        usage.working_set = counters.WorkingSetSize;
    }
#else
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long long size = 0, resident = 0;
        if (std::fscanf(f, "%llu %llu", &size, &resident) == 2) {
            const unsigned long long page = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
            usage.private_bytes = size * page;
            usage.working_set = resident * page;
        }
        std::fclose(f);
    }
#endif
    return usage;
}

void TrimUnusedHeapCaches() {
#ifdef _WIN32
    static ULONGLONG last=0;
    static bool unavailable=false;
    const auto now=GetTickCount64();
    if(unavailable || now-last<2000)return;
    last=now;
    const auto before=Get();
    const HEAP_OPTIMIZE_RESOURCES_INFORMATION options={HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION,0};
    const BOOL ok=HeapSetInformation(NULL,HeapOptimizeResources,
        const_cast<HEAP_OPTIMIZE_RESOURCES_INFORMATION*>(&options),sizeof(options));
    const DWORD error=ok ? 0 : GetLastError();
    const auto after=Get();
    unavailable=!ok;
    std::printf("Host heap cleanup: supported=%u error=%lu private before=%llu KiB after=%llu KiB elapsed=%llu ms\n",
        unsigned(ok!=0),(unsigned long)error,(unsigned long long)(before.private_bytes>>10),
        (unsigned long long)(after.private_bytes>>10),(unsigned long long)(GetTickCount64()-now));
#endif
}

} // namespace BbProcessMemory
