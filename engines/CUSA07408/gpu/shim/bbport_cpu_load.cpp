// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: per-core CPU load (NtQuerySystemInformation, processor performance times) and the app's
// own CPU time, sampled once a second for the FPS counter; also logged every 10 s.
#include "bbport_cpu_load.h"

#include <chrono>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#endif

namespace BbCpuLoad {
namespace {

#ifdef _WIN32
struct ProcessorTimes { // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION
    LARGE_INTEGER idle, kernel, user, dpc, interrupt;
    ULONG interrupt_count;
};
constexpr int SystemProcessorPerformanceInformationClass = 8;

std::vector<ProcessorTimes> ReadCores() {
    std::vector<ProcessorTimes> times(64);
    ULONG length = 0;
    const NTSTATUS status = NtQuerySystemInformation(
        static_cast<SYSTEM_INFORMATION_CLASS>(SystemProcessorPerformanceInformationClass),
        times.data(), ULONG(times.size() * sizeof(ProcessorTimes)), &length);
    times.resize(status >= 0 ? length / sizeof(ProcessorTimes) : 0);
    return times;
}

unsigned long long AppTime() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0;
    }
    auto value = [](const FILETIME& t) {
        return (unsigned long long)t.dwHighDateTime << 32 | t.dwLowDateTime;
    };
    return value(kernel) + value(user);
}
#endif

Loads loads;

} // namespace

const Loads& Get() {
#ifdef _WIN32
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last_at{};
    static std::vector<ProcessorTimes> last_cores;
    static unsigned long long last_app = 0;
    static int samples = 0;
    const auto now = Clock::now();
    if (last_at != Clock::time_point{} && now - last_at < std::chrono::seconds(1)) {
        return loads;
    }
    const auto cores = ReadCores();
    const unsigned long long app = AppTime();
    if (last_at != Clock::time_point{}) {
        if (cores.size() == last_cores.size()) {
            loads.cores.resize(cores.size());
            for (size_t i = 0; i < cores.size(); ++i) {
                // Kernel time includes idle time.
                const long long total = (cores[i].kernel.QuadPart - last_cores[i].kernel.QuadPart) +
                                        (cores[i].user.QuadPart - last_cores[i].user.QuadPart);
                const long long idle = cores[i].idle.QuadPart - last_cores[i].idle.QuadPart;
                loads.cores[i] = total > 0 ? 100.0f * float(total - idle) / float(total) : 0.0f;
            }
        }
        const double seconds = std::chrono::duration<double>(now - last_at).count();
        loads.app = seconds > 0 ? float((app - last_app) / 1e7 / seconds) : 0.0f;
        if (++samples % 10 == 0) {
            std::string line;
            for (const float core : loads.cores) {
                line += ' ' + std::to_string(int(core + 0.5f));
            }
            std::printf("CPU cores (%%):%s; app %.2f cores\n", line.c_str(), loads.app);
            std::fflush(stdout);
        }
    }
    last_at = now;
    last_cores = cores;
    last_app = app;
#endif
    return loads;
}

} // namespace BbCpuLoad
