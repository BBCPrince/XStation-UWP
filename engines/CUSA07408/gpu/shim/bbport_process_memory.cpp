// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the process's own memory, for the once-a-minute memory line in the log.
#include "bbport_process_memory.h"
#include "bbport_toggles.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

extern "C" void runtime_memory_protect_stats(std::uint64_t* calls, std::uint64_t* bytes,
                                             std::uint64_t* us, char* top, std::size_t capacity);
extern "C" void runtime_memory_lock_stats(std::uint64_t* writes, std::uint64_t* reads,
                                          std::uint64_t* waits, std::uint64_t* wait_us, char* top,
                                          std::size_t capacity);
extern "C" void runtime_memory_fault_stats(std::uint64_t counts[3], std::uint64_t* query_us);

namespace BbProcessMemory {

Usage Get() {
    Usage usage{};
#ifdef _WIN32
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

namespace {

constexpr std::uint64_t MiB = 1ull << 20;

#ifdef _WIN32
// PROCESS_MEMORY_COUNTERS_EX2 (Windows 11 SDK; not in every MinGW psapi.h): the shared commit
// (the guest's section-backed memory) counts against the app's limit too.
struct CountersEx2 {
    PROCESS_MEMORY_COUNTERS_EX ex;
    SIZE_T PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};
#endif

std::uint64_t CommitNow() {
#ifdef _WIN32
    CountersEx2 counters{};
    counters.ex.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters))) {
        return counters.ex.PrivateUsage + counters.SharedCommitUsage;
    }
#endif
    return Get().private_bytes;
}

// BB_APP_LIMIT_MB=N (0: none), else the job object's memory limit (UWP apps run in one).
std::uint64_t FindLimit(const char** source) {
    if (const char* env = std::getenv("BB_APP_LIMIT_MB")) {
        *source = "BB_APP_LIMIT_MB";
        return std::strtoull(env, nullptr, 10) * MiB;
    }
#ifdef _WIN32
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &info, sizeof(info),
                                  nullptr)) {
        const DWORD flags = info.BasicLimitInformation.LimitFlags;
        std::uint64_t limit = 0;
        if (flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) {
            limit = info.ProcessMemoryLimit;
        }
        if ((flags & JOB_OBJECT_LIMIT_JOB_MEMORY) && (!limit || info.JobMemoryLimit < limit)) {
            limit = info.JobMemoryLimit;
        }
        *source = limit ? "job object" : "job object without a memory limit";
        return limit;
    }
    *source = "no job object";
#else
    *source = "none";
#endif
    return 0;
}

} // namespace

AppPressure GetAppPressure() {
    static const AppPressure fixed = [] {
        AppPressure p{};
        const char* source = "";
        p.limit = FindLimit(&source);
        if (p.limit) {
            // 2026-10-31: was 1024 / 600 / 300 MiB below the limit (4096 / 4520 / 4820 of 5120).
            // With the pager freeing idle guest memory from 4020 MiB (urgent from 4420), level 1 at
            // 4096 only churned: after playing into a large area (app 4200-4400 MiB) buffer arenas
            // were dropped after 20 s and rebuilt 10-50 per 2 s (~180 MiB/s of copies, 9-16 FPS),
            // while a fresh load in the same spot (under 4096) ran normally. Now 700 / 450 / 300:
            // level 1 at the pager's urgent line (4420), so near the limit the caches shed (1.1 GB of
            // arenas stayed at level 0 while the pager thrashed guest memory, crash 3).
            p.trigger = p.limit - std::min<std::uint64_t>(p.limit / 2, 700 * MiB);
            p.pressure = p.limit - std::min<std::uint64_t>(p.limit / 4, 450 * MiB);
            p.critical = p.limit - std::min<std::uint64_t>(p.limit / 8, 300 * MiB);
        }
        std::printf("App memory: limit %llu MiB (%s); caches evict sooner from %llu / %llu / %llu "
                    "MiB; usage now %llu MiB\n",
                    (unsigned long long)(p.limit / MiB), source,
                    (unsigned long long)(p.trigger / MiB), (unsigned long long)(p.pressure / MiB),
                    (unsigned long long)(p.critical / MiB), (unsigned long long)(CommitNow() / MiB));
        std::fflush(stdout);
        return p;
    }();
    if (!fixed.limit) {
        return fixed;
    }
    static std::atomic<std::int64_t> measured_at{0};
    static std::atomic<std::uint64_t> usage{0};
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    std::int64_t last = measured_at.load(std::memory_order_relaxed);
    if (now - last >= 1000 &&
        measured_at.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        usage.store(CommitNow(), std::memory_order_relaxed);
        // TLOU debug (BB_MEM_TRACE=1): the app's commit every second, flushed at once: the
        // game ended without any log after the move into Boston (2026-11-01); was it the limit?
        static const bool trace = [] {
            const char* env = std::getenv("BB_MEM_TRACE");
            return env && env[0] == '1';
        }();
        if (trace) {
            const Usage u = Get();
            // Page faults (soft + hard) since the last line: guest-memory reads ran ~3x slower
            // after the move into Boston (2026-11-01); trimmed pages coming back would show here.
            static std::uint64_t last_faults = 0;
            std::uint64_t faults = 0;
#ifdef _WIN32
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof(pmc);
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                faults = pmc.PageFaultCount;
            }
#endif
            // Protection changes and guest faults per second: the GPU thread spent ~35% of its
            // time in the kernel after the move into Boston (fresh load ~10%).
            std::uint64_t vp_calls = 0, vp_bytes = 0, vp_us = 0;
            char vp_top[128] = "";
            runtime_memory_protect_stats(&vp_calls, &vp_bytes, &vp_us, vp_top, sizeof(vp_top));
            static std::uint64_t last_pm_calls, last_rf, last_wf;
            const std::uint64_t pm_calls = BbStats::protect_calls.load(std::memory_order_relaxed);
            const std::uint64_t rf = BbStats::read_faults.load(std::memory_order_relaxed);
            const std::uint64_t wf = BbStats::write_fault_count.load(std::memory_order_relaxed);
            std::printf("Mem trace: app %llu MiB, private %llu MiB, working set %llu MiB, "
                        "page faults %llu/s; VirtualProtect %llu/s, %llu MiB/s, %llu us/s (%s), "
                        "tracker %llu/s; guest faults read %llu/s, write %llu/s\n",
                        (unsigned long long)(usage.load(std::memory_order_relaxed) / MiB),
                        (unsigned long long)(u.private_bytes / MiB),
                        (unsigned long long)(u.working_set / MiB),
                        (unsigned long long)(last_faults ? faults - last_faults : 0),
                        (unsigned long long)vp_calls, (unsigned long long)(vp_bytes / MiB),
                        (unsigned long long)vp_us, vp_top,
                        (unsigned long long)(pm_calls - last_pm_calls),
                        (unsigned long long)(rf - last_rf), (unsigned long long)(wf - last_wf));
            std::uint64_t writes = 0, reads = 0, waits = 0, wait_us = 0;
            char lock_top[128] = "";
            runtime_memory_lock_stats(&writes, &reads, &waits, &wait_us, lock_top, sizeof(lock_top));
            std::printf("Table locks: %llu writes/s (%s), %llu reads/s, %llu waited, %llu us/s waiting\n",
                        (unsigned long long)writes, lock_top, (unsigned long long)reads,
                        (unsigned long long)waits, (unsigned long long)wait_us);
            std::uint64_t host_faults[3] = {}, host_query_us = 0;
            runtime_memory_fault_stats(host_faults, &host_query_us);
            std::printf("Host faults: %llu stale, %llu first touch, %llu passed on per s; "
                        "VirtualQuery %llu us/s\n",
                        (unsigned long long)host_faults[0], (unsigned long long)host_faults[1],
                        (unsigned long long)host_faults[2], (unsigned long long)host_query_us);
            last_faults = faults;
            last_pm_calls = pm_calls;
            last_rf = rf;
            last_wf = wf;
            std::fflush(stdout);
        }
    }
    AppPressure p = fixed;
    p.usage = usage.load(std::memory_order_relaxed);
    p.level = p.usage >= p.critical ? 3 : p.usage >= p.pressure ? 2 : p.usage >= p.trigger ? 1 : 0;
    return p;
}

ThreadTimes CurrentThreadTimes() {
    ThreadTimes t{};
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        t.kernel = (std::uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        t.user = (std::uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    }
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        t.cycles = cycles;
    }
#endif
    return t;
}

bool IsResident(const void* address) {
#ifdef _WIN32
    PSAPI_WORKING_SET_EX_INFORMATION info{};
    info.VirtualAddress = const_cast<void*>(address);
    if (K32QueryWorkingSetEx(GetCurrentProcess(), &info, sizeof(info))) {
        return info.VirtualAttributes.Valid;
    }
#endif
    (void)address;
    return true;
}

} // namespace BbProcessMemory
