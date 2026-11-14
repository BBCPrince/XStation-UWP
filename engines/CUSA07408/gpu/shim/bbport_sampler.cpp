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

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace BbSampler {
namespace {

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
void Callers(DWORD64 rsp, DWORD64* out) {
    const DWORD64 limit = std::min<DWORD64>(stack_base, rsp + 64 * 1024);
    std::size_t found = 0;
    for (DWORD64 at = rsp; at + 8 <= limit && found < Depth - 1; at += 8) {
        const DWORD64 value = *reinterpret_cast<const DWORD64*>(at);
        const Module* module = FindModule(value);
        if (!module || value < module->base + 4096 + 6) {
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

} // namespace

void Register(const char* who) {
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

#else

namespace BbSampler {
void Register(const char*) {}
} // namespace BbSampler

#endif
