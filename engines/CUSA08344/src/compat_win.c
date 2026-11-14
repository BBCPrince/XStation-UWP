/* Windows implementations of compat_win.h. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include "compat_win.h"

/* bb_jmp_buf: rbx rbp rdi rsi r12-r15 (0-56), rsp after return (64), return address (72),
 * xmm6-15 (80-239), mxcsr (240), x87 control word (244). */
__asm__(".text\n"
        ".globl bb_setjmp\n"
        ".def bb_setjmp; .scl 2; .type 32; .endef\n"
        "bb_setjmp:\n"
        "    mov %rbx,0(%rcx)\n    mov %rbp,8(%rcx)\n    mov %rdi,16(%rcx)\n    mov %rsi,24(%rcx)\n"
        "    mov %r12,32(%rcx)\n    mov %r13,40(%rcx)\n    mov %r14,48(%rcx)\n    mov %r15,56(%rcx)\n"
        "    lea 8(%rsp),%rdx\n    mov %rdx,64(%rcx)\n    mov (%rsp),%rdx\n    mov %rdx,72(%rcx)\n"
        "    movups %xmm6,80(%rcx)\n    movups %xmm7,96(%rcx)\n    movups %xmm8,112(%rcx)\n"
        "    movups %xmm9,128(%rcx)\n    movups %xmm10,144(%rcx)\n    movups %xmm11,160(%rcx)\n"
        "    movups %xmm12,176(%rcx)\n    movups %xmm13,192(%rcx)\n    movups %xmm14,208(%rcx)\n"
        "    movups %xmm15,224(%rcx)\n    stmxcsr 240(%rcx)\n    fnstcw 244(%rcx)\n"
        "    xor %eax,%eax\n    ret\n"
        ".globl bb_longjmp\n"
        ".def bb_longjmp; .scl 2; .type 32; .endef\n"
        "bb_longjmp:\n"
        "    mov %edx,%eax\n    test %eax,%eax\n    jnz 1f\n    mov $1,%eax\n"
        "1:  mov 0(%rcx),%rbx\n    mov 8(%rcx),%rbp\n    mov 16(%rcx),%rdi\n    mov 24(%rcx),%rsi\n"
        "    mov 32(%rcx),%r12\n    mov 40(%rcx),%r13\n    mov 48(%rcx),%r14\n    mov 56(%rcx),%r15\n"
        "    movups 80(%rcx),%xmm6\n    movups 96(%rcx),%xmm7\n    movups 112(%rcx),%xmm8\n"
        "    movups 128(%rcx),%xmm9\n    movups 144(%rcx),%xmm10\n    movups 160(%rcx),%xmm11\n"
        "    movups 176(%rcx),%xmm12\n    movups 192(%rcx),%xmm13\n    movups 208(%rcx),%xmm14\n"
        "    movups 224(%rcx),%xmm15\n    ldmxcsr 240(%rcx)\n    fldcw 244(%rcx)\n"
        "    mov 64(%rcx),%rsp\n    jmp *72(%rcx)\n");

int compat_errno_from_win32(unsigned long error) {
    switch (error) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME: case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT: return EACCES;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT: return ENOMEM;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return ENAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_NOACCESS: return EFAULT;
    case ERROR_HANDLE_EOF: return 0;
    case ERROR_NEGATIVE_SEEK: case ERROR_INVALID_PARAMETER: return EINVAL;
    default: return EIO;
    }
}

/* Positioned I/O on a synchronous handle moves the file pointer: it is restored afterwards.
 * A concurrent read() on the same descriptor would race, as with a shared offset anyway. */
static ssize_t positioned(int fd, void *buffer, size_t size, int64_t offset, int write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    LARGE_INTEGER zero = {0}, saved;
    if (!SetFilePointerEx(h, zero, &saved, FILE_CURRENT)) { errno = compat_errno_from_win32(GetLastError()); return -1; }
    size_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > 0x40000000u ? 0x40000000u : (DWORD)(size - done), n = 0;
        OVERLAPPED at = {0};
        uint64_t position = (uint64_t)offset + done;
        at.Offset = (DWORD)position; at.OffsetHigh = (DWORD)(position >> 32);
        BOOL ok = write ? WriteFile(h, (const char *)buffer + done, chunk, &n, &at)
                        : ReadFile(h, (char *)buffer + done, chunk, &n, &at);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
            if (done) return (ssize_t)done;
            errno = compat_errno_from_win32(e);
            return -1;
        }
        done += n;
        if (n < chunk) break;
    }
    SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
    return (ssize_t)done;
}
ssize_t compat_pread(int fd, void *buffer, size_t size, int64_t offset) { return positioned(fd, buffer, size, offset, 0); }
ssize_t compat_pwrite(int fd, const void *buffer, size_t size, int64_t offset) { return positioned(fd, (void *)buffer, size, offset, 1); }

long compat_utc_offset(time_t t) {
    struct tm local, utc;
    if (localtime_s(&local, &t) || gmtime_s(&utc, &t)) return 0;
    /* mktime interprets utc as local time: the difference is the zone offset (with DST). */
    utc.tm_isdst = local.tm_isdst;
    time_t as_local = mktime(&utc);
    return as_local == (time_t)-1 ? 0 : (long)(t - as_local);
}

int compat_random(void *buffer, size_t size) {
    while (size) {
        ULONG chunk = size > 0x10000000u ? 0x10000000u : (ULONG)size;
        if (BCryptGenRandom(NULL, buffer, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return -1;
        buffer = (char *)buffer + chunk; size -= chunk;
    }
    return 0;
}

/* One high-resolution timer per thread (Windows 10 1803+); a plain waitable timer before. */
static __thread HANDLE sleep_timer;
/* Detroit (2026-11-11), BB_WAIT_STATS=1: how late timed waits wake (sleeps, condition timeouts,
 * event-queue timeouts, timer events), printed every 5 s. The game's 30 FPS pacing label came
 * 0.5-3 ms past 33.3 ms in most frames. Kinds: 0 sleep, 1 cond (relative, by QPC), 2 equeue, 3 timer event, 4 cond (absolute). */
static int wait_stats_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("BB_WAIT_STATS"); on = e && e[0] == '1'; }
    return on;
}
static volatile LONG64 ws_n[5], ws_sum_us[5], ws_req_us[5], ws_b[5][8], ws_last;
static int64_t ws_now_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / f.QuadPart * 1000000 + c.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
}
void compat_wait_stat(int kind, int64_t requested_us, int64_t late_us) {
    if (!wait_stats_on() || kind < 0 || kind > 4) return;
    static const int64_t edges[7] = {100, 500, 1000, 2000, 4000, 8000, 16000};
    int b = 0;
    while (b < 7 && late_us >= edges[b]) ++b;
    InterlockedIncrement64(&ws_n[kind]);
    InterlockedAdd64(&ws_sum_us[kind], late_us);
    InterlockedAdd64(&ws_req_us[kind], requested_us);
    InterlockedIncrement64(&ws_b[kind][b]);
    const int64_t now = ws_now_us(), last = ws_last;
    if (!last) { InterlockedCompareExchange64(&ws_last, now, 0); return; }
    if (now - last < 5000000 || InterlockedCompareExchange64(&ws_last, now, last) != last) return;
    static const char *names[5] = {"sleep", "cond", "equeue", "timer", "cond-abs"};
    for (int k = 0; k < 5; ++k) {
        const LONG64 n = InterlockedExchange64(&ws_n[k], 0);
        const LONG64 sum = InterlockedExchange64(&ws_sum_us[k], 0), req = InterlockedExchange64(&ws_req_us[k], 0);
        LONG64 h[8];
        for (int i = 0; i < 8; ++i) h[i] = InterlockedExchange64(&ws_b[k][i], 0);
        if (n) printf("Wait lateness (5 s) %s: %lld waits, asked avg %lld us, late avg %lld us; late <0.1 %lld, <0.5 %lld, "
                      "<1 %lld, <2 %lld, <4 %lld, <8 %lld, <16 %lld, more %lld (ms)\n", names[k], (long long)n,
                      (long long)(req / n), (long long)(sum / n), (long long)h[0], (long long)h[1], (long long)h[2],
                      (long long)h[3], (long long)h[4], (long long)h[5], (long long)h[6], (long long)h[7]);
    }
    fflush(stdout);
}
int64_t compat_now_us(void) { return ws_now_us(); }

/* Detroit (2026-11-11), BB_TIMER_RES_US=N: asks Windows for an N-microsecond timer resolution
 * for this process (NtSetTimerResolution). Timed condition waits (winpthreads, millisecond
 * WaitForSingleObject timeouts) woke 8-16 ms late on the Xbox: the 15.6 ms default tick. */
void compat_timer_res_once(void) {
    static volatile LONG done;
    if (done || InterlockedExchange(&done, 1)) return;
    const char *e = getenv("BB_TIMER_RES_US");
    const unsigned long us = e ? strtoul(e, NULL, 10) : 0;
    if (!us) return;
    typedef LONG (WINAPI *SetRes)(ULONG, BOOLEAN, PULONG);
    typedef LONG (WINAPI *QueryRes)(PULONG, PULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    SetRes set = ntdll ? (SetRes)(void *)GetProcAddress(ntdll, "NtSetTimerResolution") : NULL;
    QueryRes query = ntdll ? (QueryRes)(void *)GetProcAddress(ntdll, "NtQueryTimerResolution") : NULL;
    ULONG lo = 0, hi = 0, cur = 0, now = 0;
    if (query) query(&lo, &hi, &cur);
    const LONG st = set ? set((ULONG)(us * 10), TRUE, &now) : -1;
    printf("Timer resolution: asked %lu us, status %#lx, now %lu.%lu ms (was %lu.%lu, range %lu.%lu-%lu.%lu ms)\n",
           us, (unsigned long)st, now / 10000, now / 100 % 100, cur / 10000, cur / 100 % 100,
           hi / 10000, hi / 100 % 100, lo / 10000, lo / 100 % 100);
    fflush(stdout);
}

void compat_sleep_ns(uint64_t ns) {
    compat_timer_res_once();
    if (!ns) { SwitchToThread(); return; }
    if (!sleep_timer) {
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (wait_stats_on()) printf("Sleep timer: high resolution %s (thread %lu)\n", sleep_timer ? "yes" : "no", GetCurrentThreadId());
        if (!sleep_timer) sleep_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    /* Detroit (2026-11-11), BB_PRECISE_SLEEP_US=M: a sleep of at least 2*M us waits on the timer
     * until M us before its end, then yields until the end. Sleeps woke 0.5-1 ms late on the
     * Xbox, and the game's 30 FPS pacing released frames at ~34 ms instead of 33.3. */
    static int64_t precise_margin_us = -1;
    if (precise_margin_us < 0) {
        const char *e = getenv("BB_PRECISE_SLEEP_US");
        precise_margin_us = e ? (int64_t)strtoul(e, NULL, 10) : 0;
    }
    int64_t precise_end_us = 0;
    if (precise_margin_us > 0 && (int64_t)(ns / 1000) >= 2 * precise_margin_us) {
        precise_end_us = ws_now_us() + (int64_t)(ns / 1000);
        ns -= (uint64_t)precise_margin_us * 1000;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100); /* relative, 100 ns units */
    const int64_t ws_begin = ns >= 200000 && wait_stats_on() ? ws_now_us() : 0;
    if (sleep_timer && SetWaitableTimer(sleep_timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(sleep_timer, INFINITE);
    else
        Sleep((DWORD)((ns + 999999) / 1000000));
    if (precise_end_us) {
        while (ws_now_us() < precise_end_us) SwitchToThread();
        ns += (uint64_t)precise_margin_us * 1000;
    }
    if (ws_begin) compat_wait_stat(0, (int64_t)(ns / 1000), ws_now_us() - ws_begin - (int64_t)(ns / 1000));
}

int compat_remove_tree(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY);
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryA(path) ? 0 : -1;
        return DeleteFileA(path) ? 0 : -1;
    }
    char pattern[MAX_PATH * 4];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    int result = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
            char child[MAX_PATH * 4];
            snprintf(child, sizeof(child), "%s\\%s", path, entry.cFileName);
            if (compat_remove_tree(child)) result = -1;
        } while (FindNextFileA(find, &entry));
        FindClose(find);
    }
    if (!RemoveDirectoryA(path)) result = -1;
    return result;
}

int compat_cpu_times(int who, int64_t *user_us, int64_t *kernel_us) {
    FILETIME created, exited, kernel, user;
    BOOL ok = who ? GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)
                  : GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    if (!ok) return -1;
    *user_us = (int64_t)(((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime) / 10);
    *kernel_us = (int64_t)(((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) / 10);
    return 0;
}
#endif
