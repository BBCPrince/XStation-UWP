// XStation USB file access (2026-11-09). SPDX-License-Identifier: GPL-2.0-or-later
//
// Measured on the Xbox dev kit: on a USB drive, CreateFile2 / FindFirstFileExW / GetFileAttributesW fail with
// ERROR_ACCESS_DENIED, while CreateFileFromAppW / FindFirstFileExFromAppW / GetFileAttributesExFromAppW open every
// file. The game engines (and the C runtime, libc++ and FFmpeg they use) only make the plain calls, so their import
// slots for those calls are pointed at the wrappers below: plain call first, FromApp version only when it is denied.
#include <windows.h>
#include <fileapifromapp.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>
#include "usb_files.h"

namespace {

std::atomic<uint64_t> fallbacks{0};
std::wstring report;

bool Denied() { return GetLastError() == ERROR_ACCESS_DENIED; }

std::wstring Wide(LPCSTR text) {
    if (!text) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), n);
    out.resize(size_t(n) - 1);
    return out;
}

void Narrow(const WIN32_FIND_DATAW& w, WIN32_FIND_DATAA& a) {
    a.dwFileAttributes = w.dwFileAttributes;
    a.ftCreationTime = w.ftCreationTime;
    a.ftLastAccessTime = w.ftLastAccessTime;
    a.ftLastWriteTime = w.ftLastWriteTime;
    a.nFileSizeHigh = w.nFileSizeHigh;
    a.nFileSizeLow = w.nFileSizeLow;
    a.dwReserved0 = w.dwReserved0;
    a.dwReserved1 = w.dwReserved1;
    WideCharToMultiByte(CP_ACP, 0, w.cFileName, -1, a.cFileName, MAX_PATH, nullptr, nullptr);
    WideCharToMultiByte(CP_ACP, 0, w.cAlternateFileName, -1, a.cAlternateFileName, 14, nullptr, nullptr);
}

// The original functions, from kernelbase (where kernel32 and the api-ms-win-core sets lead).
template <class F> F Original(const char* name) {
    HMODULE base = GetModuleHandleW(L"kernelbase.dll");
    FARPROC p = base ? GetProcAddress(base, name) : nullptr;
    if (!p) p = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    return reinterpret_cast<F>(p);
}

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFile2_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using FindFirstFileExW_t = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FindFirstFileExA_t = HANDLE(WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FindFirstFileW_t = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindFirstFileA_t = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using GetFileAttributesW_t = DWORD(WINAPI*)(LPCWSTR);
using GetFileAttributesA_t = DWORD(WINAPI*)(LPCSTR);
using GetFileAttributesExW_t = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using GetFileAttributesExA_t = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

CreateFileW_t realCreateFileW;
CreateFileA_t realCreateFileA;
CreateFile2_t realCreateFile2;
FindFirstFileExW_t realFindFirstFileExW;
FindFirstFileExA_t realFindFirstFileExA;
FindFirstFileW_t realFindFirstFileW;
FindFirstFileA_t realFindFirstFileA;
GetFileAttributesW_t realGetFileAttributesW;
GetFileAttributesA_t realGetFileAttributesA;
GetFileAttributesExW_t realGetFileAttributesExW;
GetFileAttributesExA_t realGetFileAttributesExA;

HANDLE Counted(HANDLE h) {
    if (h != INVALID_HANDLE_VALUE) ++fallbacks;
    return h;
}

HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ) {
    HANDLE h = realCreateFileW(name, access, share, sa, creation, flags, templ);
    if (h != INVALID_HANDLE_VALUE || !name || !Denied()) return h;
    return Counted(CreateFileFromAppW(name, access, share, nullptr, creation, flags, templ)); // the broker gives no inheritable handles: the CRT asks for one (fopen) and would get error 87
}
HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ) {
    HANDLE h = realCreateFileA(name, access, share, sa, creation, flags, templ);
    if (h != INVALID_HANDLE_VALUE || !name || !Denied()) return h;
    return Counted(CreateFileFromAppW(Wide(name).c_str(), access, share, nullptr, creation, flags, templ));
}
HANDLE WINAPI HookCreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD creation, LPCREATEFILE2_EXTENDED_PARAMETERS params) {
    HANDLE h = realCreateFile2(name, access, share, creation, params);
    if (h != INVALID_HANDLE_VALUE || !name || !Denied()) return h;
    CREATEFILE2_EXTENDED_PARAMETERS copy{};
    if (params) { copy = *params; copy.lpSecurityAttributes = nullptr; copy.hTemplateFile = nullptr; }
    return Counted(CreateFile2FromAppW(name, access, share, creation, params ? &copy : nullptr));
}
HANDLE WINAPI HookFindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    HANDLE h = realFindFirstFileExW(name, level, data, op, filter, flags);
    if (h != INVALID_HANDLE_VALUE || !name || !Denied()) return h;
    return Counted(FindFirstFileExFromAppW(name, level, data, op, filter, flags));
}
HANDLE WINAPI HookFindFirstFileExA(LPCSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    HANDLE h = realFindFirstFileExA(name, level, data, op, filter, flags);
    if (h != INVALID_HANDLE_VALUE || !name || !data || !Denied()) return h;
    WIN32_FIND_DATAW w{};
    h = FindFirstFileExFromAppW(Wide(name).c_str(), level, &w, op, filter, flags);
    if (h != INVALID_HANDLE_VALUE) Narrow(w, *static_cast<WIN32_FIND_DATAA*>(data));
    return Counted(h);
}
HANDLE WINAPI HookFindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data) {
    HANDLE h = realFindFirstFileW(name, data);
    if (h != INVALID_HANDLE_VALUE || !name || !Denied()) return h;
    return Counted(FindFirstFileExFromAppW(name, FindExInfoStandard, data, FindExSearchNameMatch, nullptr, 0));
}
HANDLE WINAPI HookFindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data) {
    HANDLE h = realFindFirstFileA(name, data);
    if (h != INVALID_HANDLE_VALUE || !name || !data || !Denied()) return h;
    WIN32_FIND_DATAW w{};
    h = FindFirstFileExFromAppW(Wide(name).c_str(), FindExInfoStandard, &w, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) Narrow(w, *data);
    return Counted(h);
}
DWORD AttributesFromApp(const std::wstring& name) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExFromAppW(name.c_str(), GetFileExInfoStandard, &d)) return INVALID_FILE_ATTRIBUTES;
    ++fallbacks;
    return d.dwFileAttributes;
}
DWORD WINAPI HookGetFileAttributesW(LPCWSTR name) {
    const DWORD a = realGetFileAttributesW(name);
    if (a != INVALID_FILE_ATTRIBUTES || !name || !Denied()) return a;
    return AttributesFromApp(name);
}
DWORD WINAPI HookGetFileAttributesA(LPCSTR name) {
    const DWORD a = realGetFileAttributesA(name);
    if (a != INVALID_FILE_ATTRIBUTES || !name || !Denied()) return a;
    return AttributesFromApp(Wide(name));
}
BOOL WINAPI HookGetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID data) {
    if (realGetFileAttributesExW(name, level, data)) return TRUE;
    if (!name || level != GetFileExInfoStandard || !Denied()) return FALSE;
    if (!GetFileAttributesExFromAppW(name, level, data)) return FALSE;
    ++fallbacks;
    return TRUE;
}
BOOL WINAPI HookGetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID data) {
    if (realGetFileAttributesExA(name, level, data)) return TRUE;
    if (!name || level != GetFileExInfoStandard || !Denied()) return FALSE;
    if (!GetFileAttributesExFromAppW(Wide(name).c_str(), level, data)) return FALSE;
    ++fallbacks;
    return TRUE;
}

struct Hook {
    const char* name;
    void* replacement;
    void** original;
};

Hook hooks[] = {
    {"CreateFileW", reinterpret_cast<void*>(&HookCreateFileW), reinterpret_cast<void**>(&realCreateFileW)},
    {"CreateFileA", reinterpret_cast<void*>(&HookCreateFileA), reinterpret_cast<void**>(&realCreateFileA)},
    {"CreateFile2", reinterpret_cast<void*>(&HookCreateFile2), reinterpret_cast<void**>(&realCreateFile2)},
    {"FindFirstFileExW", reinterpret_cast<void*>(&HookFindFirstFileExW), reinterpret_cast<void**>(&realFindFirstFileExW)},
    {"FindFirstFileExA", reinterpret_cast<void*>(&HookFindFirstFileExA), reinterpret_cast<void**>(&realFindFirstFileExA)},
    {"FindFirstFileW", reinterpret_cast<void*>(&HookFindFirstFileW), reinterpret_cast<void**>(&realFindFirstFileW)},
    {"FindFirstFileA", reinterpret_cast<void*>(&HookFindFirstFileA), reinterpret_cast<void**>(&realFindFirstFileA)},
    {"GetFileAttributesW", reinterpret_cast<void*>(&HookGetFileAttributesW), reinterpret_cast<void**>(&realGetFileAttributesW)},
    {"GetFileAttributesA", reinterpret_cast<void*>(&HookGetFileAttributesA), reinterpret_cast<void**>(&realGetFileAttributesA)},
    {"GetFileAttributesExW", reinterpret_cast<void*>(&HookGetFileAttributesExW), reinterpret_cast<void**>(&realGetFileAttributesExW)},
    {"GetFileAttributesExA", reinterpret_cast<void*>(&HookGetFileAttributesExA), reinterpret_cast<void**>(&realGetFileAttributesExA)},
};

bool LoadOriginals() {
    static bool loaded = false;
    if (loaded) return true;
    realCreateFileW = Original<CreateFileW_t>("CreateFileW");
    realCreateFileA = Original<CreateFileA_t>("CreateFileA");
    realCreateFile2 = Original<CreateFile2_t>("CreateFile2");
    realFindFirstFileExW = Original<FindFirstFileExW_t>("FindFirstFileExW");
    realFindFirstFileExA = Original<FindFirstFileExA_t>("FindFirstFileExA");
    realFindFirstFileW = Original<FindFirstFileW_t>("FindFirstFileW");
    realFindFirstFileA = Original<FindFirstFileA_t>("FindFirstFileA");
    realGetFileAttributesW = Original<GetFileAttributesW_t>("GetFileAttributesW");
    realGetFileAttributesA = Original<GetFileAttributesA_t>("GetFileAttributesA");
    realGetFileAttributesExW = Original<GetFileAttributesExW_t>("GetFileAttributesExW");
    realGetFileAttributesExA = Original<GetFileAttributesExA_t>("GetFileAttributesExA");
    for (const Hook& h : hooks) if (!*h.original) return false;
    loaded = true;
    return true;
}

// Modules whose imports are patched: everything in the package (exe, engines, their DLLs) and the C/C++ runtimes.
bool Wanted(const std::wstring& path, const std::wstring& packageDir) {
    if (!packageDir.empty() && path.size() > packageDir.size() && !_wcsnicmp(path.c_str(), packageDir.c_str(), packageDir.size())) return true;
    const size_t slash = path.find_last_of(L"\\/");
    const wchar_t* file = path.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
    for (const wchar_t* runtime : {L"ucrtbase.dll", L"msvcp140_app.dll", L"msvcp140.dll", L"vcruntime140_app.dll", L"vcruntime140.dll",
                                   L"vcruntime140_1_app.dll", L"msvcp140_1_app.dll", L"msvcp140_2_app.dll", L"concrt140_app.dll"}) {
        if (!_wcsicmp(file, runtime)) return true;
    }
    return false;
}

unsigned PatchModule(uint8_t* base) {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;
    unsigned patched = 0;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->FirstThunk) continue;
        auto slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        auto names = imp->OriginalFirstThunk ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk) : nullptr;
        for (size_t i = 0; slot[i].u1.Function; ++i) {
            void* current = reinterpret_cast<void*>(slot[i].u1.Function);
            for (const Hook& h : hooks) {
                bool match = current == *h.original;
                if (!match && names && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
                    auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                    match = !strcmp(reinterpret_cast<const char*>(byName->Name), h.name);
                }
                if (!match || current == h.replacement) continue;
                DWORD old = 0;
                if (!VirtualProtectFromApp(&slot[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old)) break;
                slot[i].u1.Function = reinterpret_cast<ULONG_PTR>(h.replacement);
                VirtualProtectFromApp(&slot[i].u1.Function, sizeof(void*), old, &old);
                ++patched;
                break;
            }
        }
    }
    return patched;
}

} // namespace

unsigned XStationUsbFilesInstall() {
    if (!LoadOriginals()) return 0;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring packageDir = exe;
    const size_t slash = packageDir.find_last_of(L'\\');
    packageDir = slash == std::wstring::npos ? std::wstring() : packageDir.substr(0, slash + 1);
    unsigned patched = 0;
    report = L"USB file fallback, modules:\n";
    // The loaded modules, from the process's loader list (x64 layouts; winternl.h is not available to Store apps).
    struct Name { USHORT Length, MaximumLength; PWSTR Buffer; };
    struct Entry { LIST_ENTRY InLoadOrder, InMemoryOrder, InInitOrder; PVOID DllBase, EntryPoint; ULONG SizeOfImage; Name FullDllName; };
    const auto peb = reinterpret_cast<const uint8_t*>(__readgsqword(0x60));
    const auto ldr = *reinterpret_cast<const uint8_t* const*>(peb + 0x18);
    const LIST_ENTRY* head = reinterpret_cast<const LIST_ENTRY*>(ldr + 0x10); // InLoadOrderModuleList
    for (const LIST_ENTRY* e = head->Flink; e != head; e = e->Flink) {
        auto entry = reinterpret_cast<const Entry*>(e);
        if (!entry->DllBase || !entry->FullDllName.Buffer) continue;
        const std::wstring path(entry->FullDllName.Buffer, entry->FullDllName.Length / sizeof(wchar_t));
        const bool wanted = Wanted(path, packageDir);
        const unsigned n = wanted ? PatchModule(static_cast<uint8_t*>(entry->DllBase)) : 0;
        patched += n;
        report += (wanted ? L"  patched " + std::to_wstring(n) + L"  " : L"  skipped    ") + path + L"\n";
    }
    return patched;
}

uint64_t XStationUsbFilesFallbacks() { return fallbacks.load(); }
std::wstring XStationUsbFilesReport() { return report; }
