/* 2026-10-31 (BB_PGO=generate only): LLVM's profile runtime (libclang_rt.profile) references
 * Win32 functions the rest of the DLL does not, some of which a UWP app on the Xbox cannot import
 * (UnmapViewOfFile, VirtualUnlock and others: a missing export stops the DLL from loading,
 * error 127). Defining their import pointers here keeps them out of the import table: the file
 * mapping functions go to the *FromApp ones every UWP app has, the rest are looked up at run time
 * and fail softly when absent. The profile is written with fopen/fwrite (no merging, no %h, no
 * continuous mode), so none of these is on the path actually used. */
#include <windows.h>

static FARPROC resolve(const char *name) {
    HMODULE m = GetModuleHandleW(L"kernelbase.dll");
    FARPROC p = m ? GetProcAddress(m, name) : NULL;
    if (!p) {
        m = GetModuleHandleW(L"kernel32.dll");
        p = m ? GetProcAddress(m, name) : NULL;
    }
    return p;
}

static HANDLE WINAPI pgo_CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect,
                                            DWORD high, DWORD low, LPCSTR name) {
    (void)name;
    return CreateFileMappingFromApp(file, attributes, protect, ((ULONG64)high << 32) | low, NULL);
}
static LPVOID WINAPI pgo_MapViewOfFile(HANDLE mapping, DWORD access, DWORD high, DWORD low, SIZE_T size) {
    return MapViewOfFileFromApp(mapping, access, ((ULONG64)high << 32) | low, size);
}
static BOOL WINAPI pgo_UnmapViewOfFile(LPCVOID address) {
    return UnmapViewOfFile2(GetCurrentProcess(), (PVOID)address, 0);
}
static BOOL WINAPI pgo_FlushViewOfFile(LPCVOID address, SIZE_T size) {
    typedef BOOL(WINAPI * Fn)(LPCVOID, SIZE_T);
    Fn f = (Fn)resolve("FlushViewOfFile");
    return f ? f(address, size) : TRUE;
}
static BOOL WINAPI pgo_GetComputerNameExW(COMPUTER_NAME_FORMAT format, LPWSTR buffer, LPDWORD size) {
    typedef BOOL(WINAPI * Fn)(COMPUTER_NAME_FORMAT, LPWSTR, LPDWORD);
    Fn f = (Fn)resolve("GetComputerNameExW");
    if (f) return f(format, buffer, size);
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
static BOOL WINAPI pgo_LockFileEx(HANDLE file, DWORD flags, DWORD reserved, DWORD low, DWORD high,
                                  LPOVERLAPPED overlapped) {
    typedef BOOL(WINAPI * Fn)(HANDLE, DWORD, DWORD, DWORD, DWORD, LPOVERLAPPED);
    Fn f = (Fn)resolve("LockFileEx");
    return f ? f(file, flags, reserved, low, high, overlapped) : TRUE;
}
static BOOL WINAPI pgo_UnlockFile(HANDLE file, DWORD offset_low, DWORD offset_high, DWORD low, DWORD high) {
    typedef BOOL(WINAPI * Fn)(HANDLE, DWORD, DWORD, DWORD, DWORD);
    Fn f = (Fn)resolve("UnlockFile");
    return f ? f(file, offset_low, offset_high, low, high) : TRUE;
}
static BOOL WINAPI pgo_VirtualUnlock(LPVOID address, SIZE_T size) {
    typedef BOOL(WINAPI * Fn)(LPVOID, SIZE_T);
    Fn f = (Fn)resolve("VirtualUnlock");
    return f ? f(address, size) : TRUE;
}
static BOOL WINAPI pgo_GetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped, LPDWORD bytes, BOOL wait) {
    typedef BOOL(WINAPI * Fn)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
    Fn f = (Fn)resolve("GetOverlappedResult");
    if (f) return f(file, overlapped, bytes, wait);
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
static HANDLE WINAPI pgo_CreateEventA(LPSECURITY_ATTRIBUTES attributes, BOOL manual, BOOL initial, LPCSTR name) {
    return CreateEventExA(attributes, name, (manual ? CREATE_EVENT_MANUAL_RESET : 0) | (initial ? CREATE_EVENT_INITIAL_SET : 0),
                          EVENT_ALL_ACCESS);
}
static BOOL WINAPI pgo_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES attributes) {
    typedef BOOL(WINAPI * Fn)(LPCSTR, LPSECURITY_ATTRIBUTES);
    Fn f = (Fn)resolve("CreateDirectoryA");
    if (f) return f(path, attributes);
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

void *__imp_CreateFileMappingA = (void *)pgo_CreateFileMappingA;
void *__imp_MapViewOfFile = (void *)pgo_MapViewOfFile;
void *__imp_UnmapViewOfFile = (void *)pgo_UnmapViewOfFile;
void *__imp_FlushViewOfFile = (void *)pgo_FlushViewOfFile;
void *__imp_GetComputerNameExW = (void *)pgo_GetComputerNameExW;
void *__imp_LockFileEx = (void *)pgo_LockFileEx;
void *__imp_UnlockFile = (void *)pgo_UnlockFile;
void *__imp_VirtualUnlock = (void *)pgo_VirtualUnlock;
void *__imp_GetOverlappedResult = (void *)pgo_GetOverlappedResult;
void *__imp_CreateEventA = (void *)pgo_CreateEventA;
void *__imp_CreateDirectoryA = (void *)pgo_CreateDirectoryA;
