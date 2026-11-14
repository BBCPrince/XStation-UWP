// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (BB_IP_SAMPLE=<delay s>[,<duration s>]): wall-time instruction pointer samples of the
// thread that calls BbSampler::Register() (the GPU command thread), every ~2 ms. While the
// target is suspended the collector only reads its context into a preallocated array (no
// allocation, lock or stdio: the target may hold the heap lock) and scans its stack for the
// first return addresses into known modules (the bytes before each are a call; stale values
// left on the stack can still pass). Prints "IP chain:" lines (count, then module!RVA or
// module!export+offset for the leaf and up to four callers) once done; RVAs in this module and
// in vulkan_dzn.dll are symbolized offline against their unstripped files.
#include "bbport_sampler.h"
#include "bbport_timeline.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace BbSampler {
namespace {
std::atomic<double> longest_frame_ms{0.0};

using SuspendFn = DWORD(WINAPI*)(HANDLE);
using ContextFn = BOOL(WINAPI*)(HANDLE, LPCONTEXT);

constexpr std::size_t Depth = 24; ///< leaf + 23 callers (God of War port 2026-11-03: 9 did not reach Dozen under D3D12)

struct Module {
    DWORD64 base, end;
    const char* name;
    bool symbols_offline;
};
std::array<Module, 16> modules{};
std::size_t module_count = 0;
DWORD64 stack_base = 0;

const Module* FindModule(DWORD64 address) {
    for (std::size_t i = 0; i < module_count; ++i) {
        if (address >= modules[i].base && address < modules[i].end) {
            return &modules[i];
        }
    }
    return nullptr;
}

void AddModule(HMODULE handle, const char* name, bool symbols_offline) {
    if (!handle || module_count == modules.size()) {
        return;
    }
    const auto base = reinterpret_cast<DWORD64>(handle);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    modules[module_count++] = {base, base + nt->OptionalHeader.SizeOfImage, name, symbols_offline};
}

/// Return addresses on the stack (into known modules; the bytes before each are a call).
void Callers(DWORD64 rsp, DWORD64* out, DWORD64 top = 0) {
    const DWORD64 limit = std::min<DWORD64>(top ? top : stack_base, rsp + 256 * 1024);
    std::size_t found = 0;
    for (DWORD64 at = rsp; at + 8 <= limit && found < Depth - 1; at += 8) {
        const DWORD64 value = *reinterpret_cast<const DWORD64*>(at);
        const Module* module = FindModule(value);
        if (!module || value < module->base + 4096 + 6) {
            continue;
        }
        // Detroit (2026-11-10): deep driver stacks filled every slot with ntdll/umd values before
        // a runtime or Dozen frame; the last 8 slots take only those two.
        if (found >= Depth - 9 && !module->symbols_offline) {
            continue;
        }
        // The game's image commits its pages on first touch: reading its code from here faulted
        // (dt-win2, a fatal read at 0x8ffffc). Its return addresses are taken without the check.
        if (module->base == 0x400000) {
            out[found++] = value;
            continue;
        }
        const auto* code = reinterpret_cast<const unsigned char*>(value);
        if (code[-5] == 0xE8 || (code[-2] == 0xFF && (code[-1] & 0x38) == 0x10) ||
            (code[-3] == 0xFF && (code[-2] & 0x38) == 0x10) ||
            (code[-6] == 0xFF && (code[-5] & 0x38) == 0x10)) {
            out[found++] = value;
        }
    }
}

/// The nearest export at or below `rva` of a loaded module, for modules without symbols here.
const char* NearestExport(DWORD64 base, DWORD64 rva, DWORD64& offset) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) {
        return nullptr;
    }
    const auto* exports =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const char* best = nullptr;
    DWORD best_rva = 0;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const DWORD f = functions[ordinals[i]];
        if (f <= rva && f >= best_rva) {
            best_rva = f;
            best = reinterpret_cast<const char*>(base + names[i]);
        }
    }
    offset = rva - best_rva;
    return best;
}

std::string Describe(DWORD64 address) {
    char text[256];
    const Module* module = FindModule(address);
    if (!module) {
        std::snprintf(text, sizeof(text), "?!0x%llx", static_cast<unsigned long long>(address));
        return text;
    }
    const DWORD64 rva = address - module->base;
    DWORD64 offset = 0;
    const char* exported =
        module->symbols_offline ? nullptr : NearestExport(module->base, rva, offset);
    if (exported) {
        std::snprintf(text, sizeof(text), "%s!%s+0x%llx", module->name, exported,
                      static_cast<unsigned long long>(offset));
    } else {
        std::snprintf(text, sizeof(text), "%s!0x%llx", module->name,
                      static_cast<unsigned long long>(rva));
    }
    return text;
}

void Collect(HANDLE thread, unsigned delay_s, unsigned duration_s) {
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto suspend = reinterpret_cast<SuspendFn>(GetProcAddress(kernel, "SuspendThread"));
    const auto resume = reinterpret_cast<SuspendFn>(GetProcAddress(kernel, "ResumeThread"));
    const auto context = reinterpret_cast<ContextFn>(GetProcAddress(kernel, "GetThreadContext"));
    if (!suspend || !resume || !context) {
        std::printf("IP sample: unavailable\n");
        return;
    }
    Sleep(delay_s * 1000);
    if (const char* stall = std::getenv("BB_IP_SAMPLE_STALL_MS"); stall && std::atoi(stall) > 0) {
        const double wanted = std::atoi(stall);
        longest_frame_ms.store(0.0);
        while (longest_frame_ms.load() < wanted) {
            Sleep(100);
        }
        std::printf("IP sample: a %.0f ms frame seen, sampling now\n", longest_frame_ms.load());
    }
    // Modules loaded by now (the device exists): the driver stack included.
    const struct {
        const wchar_t* file;
        const char* name;
        bool symbols_offline;
    } known[] = {
        {L"ntdll.dll", "ntdll", false},           {L"KERNELBASE.dll", "kernelbase", false},
        {L"kernel32.dll", "kernel32", false},     {L"ucrtbase.dll", "ucrtbase", false},
        {L"msvcrt.dll", "msvcrt", false},         {L"libc++.dll", "libc++", false},
        {L"vulkan_dzn.dll", "dzn", true},         {L"D3D12Core.dll", "d3d12core", false},
        {L"d3d12.dll", "d3d12", false},           {L"umd12ddi_arden.dll", "umd", false},
        {L"win32u.dll", "win32u", false},         {L"libxxhash.dll", "xxhash", false},
        {L"vulkan-1.dll", "vulkan-1", false},
    };
    for (const auto& k : known) {
        AddModule(GetModuleHandleW(k.file), k.name, k.symbols_offline);
    }
    std::vector<std::array<DWORD64, Depth>> chains(duration_s * 1000);
    std::size_t count = 0, failed = 0;
    // CPU time of the thread against wall time: below ~100% (when not blocked) it was preempted.
    const auto cpu_time = [thread] {
        FILETIME a{}, b{}, kernel{}, user{};
        GetThreadTimes(thread, &a, &b, &kernel, &user);
        const auto value = [](const FILETIME& t) {
            return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        return value(kernel) + value(user);
    };
    const unsigned long long cpu_begin = cpu_time();
    const auto wall_begin = GetTickCount64();
    const auto end = GetTickCount64() + duration_s * 1000ull;
    while (GetTickCount64() < end && count < chains.size()) {
        alignas(16) CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        auto& chain = chains[count];
        chain = {};
        const DWORD prior = suspend(thread);
        if (prior == static_cast<DWORD>(-1)) {
            break;
        }
        const BOOL valid = context(thread, &ctx);
        if (valid) {
            Callers(ctx.Rsp, chain.data() + 1);
        }
        resume(thread);
        if (valid) {
            chain[0] = ctx.Rip;
            ++count;
        } else {
            ++failed;
        }
        Sleep(2);
    }
    std::printf("IP sample: thread CPU time %.1f%% of wall time\n",
                (cpu_time() - cpu_begin) / 1e4 / double(GetTickCount64() - wall_begin) * 100.0);
    struct Hash {
        std::size_t operator()(const std::array<DWORD64, Depth>& k) const {
            std::size_t h = 0;
            for (const auto v : k) {
                h = h * 1000003 ^ std::hash<DWORD64>{}(v);
            }
            return h;
        }
    };
    std::unordered_map<std::array<DWORD64, Depth>, unsigned, Hash> counts;
    for (std::size_t i = 0; i < count; ++i) {
        ++counts[chains[i]];
    }
    std::vector<std::pair<std::array<DWORD64, Depth>, unsigned>> sorted(counts.begin(),
                                                                         counts.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::printf("IP sample: %zu samples, %zu failed, %zu chains\n", count, failed, sorted.size());
    for (const auto& [chain, n] : sorted) {
        std::string line = "IP chain: " + std::to_string(n);
        for (const auto address : chain) {
            if (!address) {
                break;
            }
            line += ' ';
            line += Describe(address);
        }
        std::printf("%s\n", line.c_str());
    }
    std::fflush(stdout);
}

// Detroit (2026-11-10), BB_IP_SAMPLE_SPIKE_MS=<ms>: the 5 s hitches (GPU thread waiting for host
// copies) come at random moments, so sampling that starts once one was seen misses it. Every
// registered thread (command, recorder, drawrec, copies, present) is sampled all the time into a
// ring of its last ~9 s; a frame of at least <ms> prints, per thread, the chains of that frame.
namespace Spike {
constexpr std::size_t MaxThreads = 24, Ring = 4096, MaxReports = 8;
struct Entry {
    ULONGLONG tick;
    std::array<DWORD64, Depth> chain;
};
struct Target {
    HANDLE handle;
    const char* name;
    DWORD64 stack_top;
    Entry* ring;
    std::size_t next;
    DWORD id;
};
std::mutex targets_lock;
std::array<Target, MaxThreads> targets{};
std::atomic<std::size_t> target_count{0};
std::atomic<ULONGLONG> request_tick{0};
std::atomic<double> request_ms{0.0};
double threshold_ms = 0.0;

void Report(double ms, ULONGLONG at, unsigned number) {
    // BB_IP_SAMPLE_SPIKE_WINDOW=<ms>: the last <ms> before the frame instead of the frame alone (a
    // profile of a slow stretch: heavy scenes run at 15-20 FPS without single long frames).
    static const ULONGLONG window = [] {
        const char* env = std::getenv("BB_IP_SAMPLE_SPIKE_WINDOW");
        return env ? static_cast<ULONGLONG>(std::atoi(env)) : 0ull;
    }();
    const ULONGLONG span = window ? window : static_cast<ULONGLONG>(ms) + 100;
    const ULONGLONG from = at > span ? at - span : 0;
    std::printf("Spike %u: %.0f ms frame ending at tick %llu\n", number, ms,
                static_cast<unsigned long long>(at));
    struct Hash {
        std::size_t operator()(const std::array<DWORD64, Depth>& k) const {
            std::size_t h = 0;
            for (const auto v : k) {
                h = h * 1000003 ^ std::hash<DWORD64>{}(v);
            }
            return h;
        }
    };
    const std::size_t count = target_count.load(std::memory_order_acquire);
    for (std::size_t t = 0; t < count; ++t) {
        const auto& target = targets[t];
        std::unordered_map<std::array<DWORD64, Depth>, unsigned, Hash> counts;
        unsigned samples = 0;
        for (std::size_t i = 0; i < Ring; ++i) {
            const auto& e = target.ring[i];
            if (e.tick >= from && e.tick <= at && e.chain[0]) {
                ++counts[e.chain];
                ++samples;
            }
        }
        std::vector<std::pair<std::array<DWORD64, Depth>, unsigned>> sorted(counts.begin(),
                                                                             counts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](auto& a, auto& b) { return a.second > b.second; });
        std::printf("Spike %u thread %s: %u samples, %zu chains\n", number, target.name, samples,
                    sorted.size());
        for (std::size_t i = 0; i < sorted.size() && i < (std::getenv("BB_IP_SAMPLE_SPIKE_WINDOW") ? 40u : 6u); ++i) {
            std::string line = "IP chain: " + std::to_string(sorted[i].second);
            for (const auto address : sorted[i].first) {
                if (!address) {
                    break;
                }
                line += ' ';
                line += Describe(address);
            }
            std::printf("Spike %u %s %s\n", number, target.name, line.c_str());
        }
        // Window mode: every sample by its leaf (self time) and by its leaf + first caller, for
        // the runtime's own threads (the top chains above miss the work spread over many chains).
        if (std::getenv("BB_IP_SAMPLE_SPIKE_WINDOW") && target.stack_top != 0) {
            std::unordered_map<DWORD64, unsigned> leaves;
            for (std::size_t i = 0; i < Ring; ++i) {
                const auto& e = target.ring[i];
                if (e.tick >= from && e.tick <= at && e.chain[0]) {
                    ++leaves[e.chain[0]];
                }
            }
            std::vector<std::pair<DWORD64, unsigned>> by_leaf(leaves.begin(), leaves.end());
            std::sort(by_leaf.begin(), by_leaf.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });
            for (std::size_t i = 0; i < by_leaf.size() && i < 400; ++i) {
                std::printf("Spike %u %s leaf: %u %s\n", number, target.name, by_leaf[i].second,
                            Describe(by_leaf[i].first).c_str());
            }
        }
    }
    std::fflush(stdout);
}

std::atomic<ULONGLONG> last_flip_tick{0};
DWORD live_ms = 1200;

// Detroit (2026-11-11), BB_IP_SAMPLE_CLASS=<lo>,<hi> (with BB_IP_SAMPLE_SPIKE_MS set, e.g. 100000):
// a profile of the heavy frames only. Every game frame of lo..hi ms adds its samples (all threads)
// to the "H" class; every 8th frame under 36 ms to the "L" class. The chains (12 deep) are printed
// with their counts once 300, 1000, 2500 and 5000 heavy frames were seen ("CP <dump> <class>
// <thread>|<count>|<chain>"). Heavy scenes run at 40-50 ms a frame without single long frames, and
// the window mode mixes them with the light frames around them.
struct ClassRequest {
    ULONGLONG at;
    float ms;
    int cls;
};
constexpr unsigned ClassQueue = 256;
std::array<ClassRequest, ClassQueue> class_queue{};
std::atomic<unsigned> class_head{0}, class_tail{0};
double class_lo = 0.0, class_hi = 0.0;
constexpr std::size_t ClassDepth = 6;
struct ChainHash {
    std::size_t operator()(const std::array<DWORD64, Depth>& k) const {
        std::size_t h = 0;
        for (const auto v : k) {
            h = h * 1000003 ^ std::hash<DWORD64>{}(v);
        }
        return h;
    }
};
using ChainCounts = std::unordered_map<std::array<DWORD64, Depth>, unsigned, ChainHash>;
ChainCounts* class_counts[2][MaxThreads]{};
unsigned class_frames[2]{};
double class_ms[2]{};
unsigned class_samples[2][MaxThreads]{};

void ClassAccumulate(const ClassRequest& req) {
    const ULONGLONG span = static_cast<ULONGLONG>(req.ms);
    const ULONGLONG from = req.at > span ? req.at - span : 0;
    ++class_frames[req.cls];
    class_ms[req.cls] += req.ms;
    const std::size_t count = target_count.load(std::memory_order_acquire);
    for (std::size_t t = 0; t < count; ++t) {
        const auto& target = targets[t];
        auto*& counts = class_counts[req.cls][t];
        if (!counts) {
            counts = new ChainCounts;
        }
        for (std::size_t k = 1; k <= Ring; ++k) {
            const auto& e = target.ring[(target.next + Ring - k) % Ring];
            if (!e.tick || e.tick < from) {
                break;
            }
            if (e.tick > req.at || !e.chain[0]) {
                continue;
            }
            auto key = e.chain;
            for (std::size_t d = ClassDepth; d < Depth; ++d) {
                key[d] = 0;
            }
            ++(*counts)[key];
            ++class_samples[req.cls][t];
        }
    }
}

void ClassDump(unsigned number) {
    std::printf("Class dump %u: heavy frames %u (%.0f ms in all), light frames sampled %u (%.0f ms)\n",
                number, class_frames[0], class_ms[0], class_frames[1], class_ms[1]);
    const std::size_t count = target_count.load(std::memory_order_acquire);
    for (int cls = 0; cls < 2; ++cls) {
        for (std::size_t t = 0; t < count; ++t) {
            const auto* counts = class_counts[cls][t];
            if (!counts) {
                continue;
            }
            std::vector<std::pair<std::array<DWORD64, Depth>, unsigned>> sorted(counts->begin(),
                                                                                 counts->end());
            std::sort(sorted.begin(), sorted.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });
            std::printf("CPT %u %c %s: %u samples, %zu chains\n", number, cls ? 'L' : 'H',
                        targets[t].name, class_samples[cls][t], sorted.size());
            // Every sample by its leaf (complete: the chain list below is cut).
            std::unordered_map<DWORD64, unsigned> leaves;
            for (const auto& [chain, n] : *counts) {
                leaves[chain[0]] += n;
            }
            std::vector<std::pair<DWORD64, unsigned>> by_leaf(leaves.begin(), leaves.end());
            std::sort(by_leaf.begin(), by_leaf.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });
            for (const auto& [leaf, n] : by_leaf) {
                std::printf("CL %u %c %s|%u|%s\n", number, cls ? 'L' : 'H', targets[t].name, n,
                            Describe(leaf).c_str());
            }
            const std::size_t limit = cls ? 2500 : 6000;
            for (std::size_t i = 0; i < sorted.size() && i < limit; ++i) {
                std::string line;
                for (const auto address : sorted[i].first) {
                    if (!address) {
                        break;
                    }
                    line += ' ';
                    line += Describe(address);
                }
                std::printf("CP %u %c %s|%u|%s\n", number, cls ? 'L' : 'H', targets[t].name,
                            sorted[i].second, line.c_str());
            }
        }
    }
    std::fflush(stdout);
}

/// Every thread of the process while no frame has come for a while (the stall is still on):
/// one thread at a time is suspended, its stack read, and resumed; names and printing after.
void DumpAll(ULONGLONG gap, unsigned number) {
    using DescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto describe = reinterpret_cast<DescFn>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription"));
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto suspend = reinterpret_cast<SuspendFn>(GetProcAddress(kernel, "SuspendThread"));
    const auto resume = reinterpret_cast<SuspendFn>(GetProcAddress(kernel, "ResumeThread"));
    const auto context = reinterpret_cast<ContextFn>(GetProcAddress(kernel, "GetThreadContext"));
    struct Snap {
        DWORD id;
        HANDLE handle;
        std::array<DWORD64, Depth> chain;
    };
    std::vector<Snap> snaps;
    snaps.reserve(512);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (all == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(all, &te); ok && snaps.size() < snaps.capacity();
         ok = Thread32Next(all, &te)) {
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) {
            snaps.push_back({te.th32ThreadID, nullptr, {}});
        }
    }
    CloseHandle(all);
    for (auto& s : snaps) {
        s.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, s.id);
    }
    for (auto& s : snaps) {
        if (!s.handle || suspend(s.handle) == static_cast<DWORD>(-1)) {
            continue;
        }
        alignas(16) CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (context(s.handle, &ctx)) {
            MEMORY_BASIC_INFORMATION mbi{};
            DWORD64 top = 0;
            if (VirtualQuery(reinterpret_cast<void*>(ctx.Rsp), &mbi, sizeof(mbi))) {
                top = reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize;
            }
            s.chain[0] = ctx.Rip;
            if (top > ctx.Rsp) {
                Callers(ctx.Rsp, s.chain.data() + 1, top);
            }
        }
        resume(s.handle);
    }
    std::printf("Spike live %u: no frame for %llu ms, %zu threads\n", number,
                static_cast<unsigned long long>(gap), snaps.size());
    for (auto& s : snaps) {
        char name[64] = "?";
        PWSTR wide = nullptr;
        if (describe && s.handle && SUCCEEDED(describe(s.handle, &wide)) && wide) {
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name), nullptr, nullptr);
            LocalFree(wide);
        }
        if (s.chain[0]) {
            std::string line;
            for (const auto address : s.chain) {
                if (!address) {
                    break;
                }
                line += ' ';
                line += Describe(address);
            }
            std::printf("Spike live %u thread %lu %s:%s\n", number, s.id, name, line.c_str());
        }
        if (s.handle) {
            CloseHandle(s.handle);
        }
    }
    std::fflush(stdout);
}

/// The game's threads that were working through the 5 s stalls (Spike live dumps, dt-spike2).
void AddGuestThreads() {
    using DescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto describe = reinterpret_cast<DescFn>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription"));
    static char names[MaxThreads][40];
    if (!describe) {
        return;
    }
    const wchar_t* wanted_all[] = {L"Loading Thread", L"StreamingManage", L"RenderQueue",
                                   L"Game factory", L"JobWorkerThread", L"UserInterface", L"GameMain"};
    const wchar_t* wanted_class[] = {L"RenderQueue", L"GameMain"};
    const std::span<const wchar_t*> wanted = class_lo > 0.0
                                                 ? std::span<const wchar_t*>{wanted_class}
                                                 : std::span<const wchar_t*>{wanted_all};
    const DWORD pid = GetCurrentProcessId();
    HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (all == INVALID_HANDLE_VALUE) {
        return;
    }
    std::scoped_lock lk{targets_lock};
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(all, &te); ok; ok = Thread32Next(all, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        bool known = false;
        for (std::size_t i = 0; i < target_count.load(); ++i) {
            known |= targets[i].id == te.th32ThreadID;
        }
        if (known) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, te.th32ThreadID);
        if (!h) {
            continue;
        }
        PWSTR wide = nullptr;
        bool keep = false;
        if (SUCCEEDED(describe(h, &wide)) && wide) {
            for (const auto* w : wanted) {
                if (std::wcsncmp(wide, w, std::wcslen(w)) == 0) {
                    keep = true;
                }
            }
            const std::size_t index = target_count.load();
            if (keep && index < MaxThreads) {
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, names[index], sizeof(names[index]),
                                    nullptr, nullptr);
                auto& target = targets[index];
                target.handle = h;
                target.name = names[index];
                target.stack_top = 0;
                target.ring = new Entry[Ring]{};
                target.next = 0;
                target.id = te.th32ThreadID;
                target_count.store(index + 1, std::memory_order_release);
            } else {
                keep = false;
            }
            LocalFree(wide);
        }
        if (!keep) {
            CloseHandle(h);
        }
    }
    CloseHandle(all);
}

void Run() {
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto suspend = reinterpret_cast<SuspendFn>(GetProcAddress(kernel, "SuspendThread"));
    const auto resume = reinterpret_cast<SuspendFn>(GetProcAddress(kernel, "ResumeThread"));
    const auto context = reinterpret_cast<ContextFn>(GetProcAddress(kernel, "GetThreadContext"));
    if (!suspend || !resume || !context) {
        return;
    }
    Sleep(20000); // the device and the driver modules exist by now
    const struct {
        const wchar_t* file;
        const char* name;
        bool symbols_offline;
    } known[] = {
        {L"ntdll.dll", "ntdll", false},           {L"KERNELBASE.dll", "kernelbase", false},
        {L"kernel32.dll", "kernel32", false},     {L"ucrtbase.dll", "ucrtbase", false},
        {L"libc++.dll", "libc++", false},         {L"vulkan_dzn.dll", "dzn", true},
        {L"D3D12Core.dll", "d3d12core", false},   {L"d3d12.dll", "d3d12", false},
        {L"umd12ddi_arden.dll", "umd", false},    {L"win32u.dll", "win32u", false},
        {L"XStation.exe", "xstation", false},     {L"xbsc_xs.dll", "xbsc", false},
    };
    for (const auto& k : known) {
        AddModule(GetModuleHandleW(k.file), k.name, k.symbols_offline);
    }
    // Detroit: the game's own code (image at 0x400000, eboot text 0x1b30064 bytes): "game!<rva>" in
    // the chains is the address in IDA's eboot.elf.
    if (module_count < modules.size()) {
        modules[module_count++] = {0x400000, 0x400000 + 0x1b30064, "game", true};
    }
    AddGuestThreads();
    std::printf("IP sample: spike mode, frames of %.0f ms or more, %zu threads\n", threshold_ms,
                target_count.load());
    std::fflush(stdout);
    unsigned reports = 0;
    ULONGLONG handled = 0;
    while (reports < MaxReports) {
        const std::size_t count = target_count.load(std::memory_order_acquire);
        for (std::size_t t = 0; t < count; ++t) {
            auto& target = targets[t];
            alignas(16) CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            auto& entry = target.ring[target.next];
            if (suspend(target.handle) == static_cast<DWORD>(-1)) {
                continue;
            }
            entry.chain = {};
            const BOOL valid = context(target.handle, &ctx);
            if (valid) {
                DWORD64 top = target.stack_top;
                if (!top) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (VirtualQuery(reinterpret_cast<void*>(ctx.Rsp), &mbi, sizeof(mbi))) {
                        top = reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize;
                    }
                }
                if (top > ctx.Rsp) {
                    Callers(ctx.Rsp, entry.chain.data() + 1, top);
                }
            }
            resume(target.handle);
            entry.chain[0] = valid ? ctx.Rip : 0;
            entry.tick = GetTickCount64();
            target.next = (target.next + 1) % Ring;
        }
        const ULONGLONG at = request_tick.load(std::memory_order_acquire);
        static ULONGLONG reported_at = 0;
        static const ULONGLONG spacing = std::getenv("BB_IP_SAMPLE_SPIKE_WINDOW") ? 30000 : 0;
        if (at && at != handled && (!reported_at || at - reported_at >= spacing)) {
            handled = at;
            reported_at = at;
            Report(request_ms.load(), at, ++reports);
        }
        if (class_lo > 0.0) {
            static unsigned class_dumps = 0;
            unsigned tail = class_tail.load(std::memory_order_relaxed);
            const unsigned head = class_head.load(std::memory_order_acquire);
            for (; tail != head; tail = (tail + 1) % ClassQueue) {
                ClassAccumulate(class_queue[tail]);
            }
            class_tail.store(tail, std::memory_order_release);
            static const unsigned marks[] = {1000, 2500, 5000};
            if (class_dumps < 3 && class_frames[0] >= marks[class_dumps]) {
                ClassDump(++class_dumps);
            }
        }
        static ULONGLONG rescan = GetTickCount64() + 10000;
        if (GetTickCount64() >= rescan) {
            rescan = GetTickCount64() + 10000;
            AddGuestThreads();
        }
        // While the stall is on: every thread, at live_ms and again 1.5 s later.
        static ULONGLONG dumped_for = 0;
        static int dumped = 0;
        static unsigned lives = 0;
        const ULONGLONG flip = last_flip_tick.load(std::memory_order_acquire);
        if (flip && lives < 12) {
            const ULONGLONG gap = GetTickCount64() - flip;
            if (flip != dumped_for) {
                dumped_for = flip;
                dumped = 0;
            }
            if ((dumped == 0 && gap >= live_ms) || (dumped == 1 && gap >= live_ms + 1500)) {
                DumpAll(gap, ++lives);
                ++dumped;
            }
        }
        Sleep(2);
    }
}
} // namespace Spike

} // namespace

void NoteFrame(double ms) {
    if (Spike::class_lo > 0.0 && Spike::target_count.load(std::memory_order_relaxed)) {
        static std::atomic<unsigned> seen{0};
        const unsigned n = seen.fetch_add(1, std::memory_order_relaxed);
        const bool heavy = ms >= Spike::class_lo && ms <= Spike::class_hi;
        const bool light = ms < 36.0 && n % 8 == 0;
        if (n > 3000 && (heavy || light)) {
            const unsigned head = Spike::class_head.load(std::memory_order_relaxed);
            const unsigned next = (head + 1) % Spike::ClassQueue;
            if (next != Spike::class_tail.load(std::memory_order_acquire)) {
                Spike::class_queue[head] = {GetTickCount64(), static_cast<float>(ms), heavy ? 0 : 1};
                Spike::class_head.store(next, std::memory_order_release);
            }
        }
    }
    static std::atomic<unsigned> spike_frames{0};
    if (Spike::threshold_ms > 0.0 && spike_frames.load(std::memory_order_relaxed) > 300) {
        Spike::last_flip_tick.store(GetTickCount64(), std::memory_order_release);
    }
    if (Spike::threshold_ms > 0.0 && spike_frames.fetch_add(1, std::memory_order_relaxed) > 300 &&
        ms >= Spike::threshold_ms && Spike::target_count.load(std::memory_order_relaxed)) {
        Spike::request_ms.store(ms);
        Spike::request_tick.store(GetTickCount64(), std::memory_order_release);
    }
    // BB_IP_SAMPLE_STALL_AFTER=<frames> (default 3000): boot and loading hitches come first.
    static const unsigned after = [] {
        const char* env = std::getenv("BB_IP_SAMPLE_STALL_AFTER");
        return env ? static_cast<unsigned>(std::atoi(env)) : 3000u;
    }();
    static std::atomic<unsigned> frames{0};
    if (frames.fetch_add(1, std::memory_order_relaxed) < after) {
        return;
    }
    double seen = longest_frame_ms.load(std::memory_order_relaxed);
    while (ms > seen && !longest_frame_ms.compare_exchange_weak(seen, ms)) {
    }
}

void Register(const char* who) {
    if (const char* spike = std::getenv("BB_IP_SAMPLE_SPIKE_MS"); spike && std::atoi(spike) > 0) {
        static std::once_flag once;
        std::call_once(once, [spike] {
            Spike::threshold_ms = std::atoi(spike);
            if (const char* cls = std::getenv("BB_IP_SAMPLE_CLASS")) {
                double lo = 0.0, hi = 0.0;
                if (std::sscanf(cls, "%lf,%lf", &lo, &hi) == 2 && lo > 0.0 && hi > lo) {
                    Spike::class_lo = lo;
                    Spike::class_hi = hi;
                }
            }
            HMODULE self{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&Register), &self);
            AddModule(self, "runtime", true);
            std::thread([] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                Spike::Run();
            }).detach();
        });
        std::scoped_lock lk{Spike::targets_lock};
        const std::size_t index = Spike::target_count.load();
        if (index == Spike::MaxThreads) {
            return;
        }
        HANDLE thread{};
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                             FALSE, 0)) {
            return;
        }
        auto& target = Spike::targets[index];
        target.handle = thread;
        target.name = who;
        target.stack_top =
            reinterpret_cast<DWORD64>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
        target.ring = new Spike::Entry[Spike::Ring]{};
        target.next = 0;
        target.id = GetCurrentThreadId();
        Spike::target_count.store(index + 1, std::memory_order_release);
        return;
    }
    const char* env = std::getenv("BB_IP_SAMPLE");
    if (!env || !*env) {
        return;
    }
    const char* wanted = std::getenv("BB_IP_SAMPLE_THREAD");
    if (std::strcmp(wanted && *wanted ? wanted : "command", who) != 0) {
        return;
    }
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        return;
    }
    unsigned delay = 0, duration = 20;
    std::sscanf(env, "%u,%u", &delay, &duration);
    duration = std::clamp(duration, 1u, 60u);
    stack_base = reinterpret_cast<DWORD64>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Register), &self);
    AddModule(self, "runtime", true);
    HANDLE thread{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        std::printf("IP sample: no thread handle (%lu)\n", GetLastError());
        return;
    }
    std::printf("IP sample: thread %lu, %u s after this, for %u s\n", GetCurrentThreadId(), delay,
                duration);
    std::thread([thread, delay, duration] {
        Collect(thread, delay, duration);
        CloseHandle(thread);
    }).detach();
}

} // namespace BbSampler

namespace BbTimeline {
std::uint32_t CurrentTid() { return GetCurrentThreadId(); }
Event ring[Capacity];
std::atomic<std::uint32_t> used{0};
std::atomic<int> state{0};
std::atomic<std::uint64_t> flips{0};
} // namespace BbTimeline

#else

namespace BbSampler {
void Register(const char*) {}
void NoteFrame(double) {}
} // namespace BbSampler
namespace BbTimeline {
std::uint32_t CurrentTid() { return 0; }
Event ring[Capacity];
std::atomic<std::uint32_t> used{0};
std::atomic<int> state{0};
std::atomic<std::uint64_t> flips{0};
} // namespace BbTimeline

#endif
