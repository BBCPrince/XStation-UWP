/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,

 * protection changes and queries. One sparse memfd backs the whole direct

 * pool, so any mapping of any physical range aliases the same storage.

 * Windows: a pagefile-backed section plays the memfd's part, and the PS4 user range is

 * reserved as one placeholder that views of the section replace (VirtualAlloc2/MapViewOfFile3,

 * as shadPS4 does); see the "host mappings" section below.

 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors

 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */

#define _GNU_SOURCE

#include "runtime.h"

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#include <inttypes.h>

#include <limits.h>

#include <pthread.h>

#include <time.h>

#include <errno.h>

#ifdef _WIN32

#include <windows.h>

#include <winioctl.h>

#include <psapi.h>

/* TLOU debug: every VirtualProtect in this file, counted by source line with its bytes and time.

 * After the move into Boston the GPU thread spent ~35% of its time in the kernel (fresh load

 * ~10%, 2026-11-01); protection changes interrupt every core. BB_MEM_TRACE prints the rates. */

#define PROTECT_LINES 4096

static uint64_t protect_line_calls[PROTECT_LINES], protect_line_bytes[PROTECT_LINES],

    protect_line_ticks[PROTECT_LINES];

static BOOL counted_virtual_protect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old,

                                    unsigned line) {

    static int on = -1;

    if (on < 0) {

        const char *e = getenv("BB_MEM_TRACE");

        on = e && e[0] == '1';

    }

    if (!on) return VirtualProtect(address, size, protect, old);

    LARGE_INTEGER a, b;

    QueryPerformanceCounter(&a);

    const BOOL ok = VirtualProtect(address, size, protect, old);

    QueryPerformanceCounter(&b);

    line &= PROTECT_LINES - 1;

    __atomic_add_fetch(&protect_line_calls[line], 1, __ATOMIC_RELAXED);

    __atomic_add_fetch(&protect_line_bytes[line], size, __ATOMIC_RELAXED);

    __atomic_add_fetch(&protect_line_ticks[line], (uint64_t)(b.QuadPart - a.QuadPart),

                       __ATOMIC_RELAXED);

    return ok;

}

#define VirtualProtect(a, s, p, o) counted_virtual_protect((a), (s), (p), (o), __LINE__)

/* Since the last call: total calls, bytes and microseconds; up to 4 busiest lines as text. */

void runtime_memory_protect_stats(uint64_t *calls, uint64_t *bytes, uint64_t *us, char *top,

                                  size_t capacity) {

    static uint64_t last_calls[PROTECT_LINES], last_bytes[PROTECT_LINES], last_ticks[PROTECT_LINES];

    uint64_t delta[PROTECT_LINES];

    uint64_t c = 0, by = 0, t = 0;

    for (unsigned i = 0; i < PROTECT_LINES; ++i) {

        const uint64_t nc = __atomic_load_n(&protect_line_calls[i], __ATOMIC_RELAXED);

        const uint64_t nb = __atomic_load_n(&protect_line_bytes[i], __ATOMIC_RELAXED);

        const uint64_t nt = __atomic_load_n(&protect_line_ticks[i], __ATOMIC_RELAXED);

        delta[i] = nc - last_calls[i];

        c += delta[i];

        by += nb - last_bytes[i];

        t += nt - last_ticks[i];

        last_calls[i] = nc;

        last_bytes[i] = nb;

        last_ticks[i] = nt;

    }

    LARGE_INTEGER f;

    QueryPerformanceFrequency(&f);

    *calls = c;

    *bytes = by;

    *us = f.QuadPart ? t * 1000000 / (uint64_t)f.QuadPart : 0;

    size_t used = 0;

    top[0] = 0;

    for (int k = 0; k < 4; ++k) {

        unsigned best = 0;

        for (unsigned i = 1; i < PROTECT_LINES; ++i)

            if (delta[i] > delta[best]) best = i;

        if (!delta[best] || used + 24 >= capacity) break;

        used += (size_t)snprintf(top + used, capacity - used, "%sL%u:%llu", k ? " " : "", best,

                                 (unsigned long long)delta[best]);

        delta[best] = 0;

    }

}

#else

#include <fcntl.h>

#include <sys/mman.h>

#include <unistd.h>

#endif

/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. BB_DMEM_MB raises it (the resolution

 * patches above 1080p need about 4 GiB more; run.sh sets it). */

static uint64_t pool_size_bytes(void) {

    static uint64_t size;

    if (!size) {

        const char *env = getenv("BB_DMEM_MB");

        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;

        if (mb < 5056 || mb > 16384) mb = 5056;

        size = mb * 1024 * 1024;

    }

    return size;

}

#define POOL_SIZE pool_size_bytes()

#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)

#define PAGE UINT64_C(16384)

/* Spider-Man (2026-11-04): the game maps direct memory FIXED at 0x7f0000000 (PS4's system-managed
 * range, below 0x1000000000), so the guest range managed here starts at 0x700000000. Allocations
 * without a usable hint still start at ALLOC_MIN, the PS4 user range, as before. */
#define USER_MIN UINT64_C(0x0700000000)
#define ALLOC_MIN UINT64_C(0x1000000000)

#define USER_MAX UINT64_C(0xfc00000000)

#define LIMIT 4096

#define INVALID ((int32_t)UINT32_C(0x80020016))

#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))

#define ACCESS ((int32_t)UINT32_C(0x8002000d))

#define NO_SPACE ((int32_t)UINT32_C(0x80020023))

#define MAP_FIXED_FLAG 0x10

#define MAP_NO_OVERWRITE 0x80

enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };

typedef struct { uint64_t start, size; int type, used; } Block;

typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; char name[32]; } Vma;

static ABI int32_t set_range_name(const void *address,uint64_t size,const char *name);

typedef struct {

    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;

    uint32_t flags; char name[32];

} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */

typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;

_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");

_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");



/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page

 * protection) share it; map/unmap/protect take it exclusively. Both nest: code

 * under the lock may write guest memory, and a GPU-tracked page then faults into

 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's

 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */

static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;

static __thread unsigned exclusive_depth,shared_depth;

#ifdef _WIN32

static INIT_ONCE table_lock_once=INIT_ONCE_STATIC_INIT;

static CRITICAL_SECTION private_table_lock;

static int private_table_mode;

static BOOL CALLBACK initialize_table_lock(PINIT_ONCE once,PVOID parameter,PVOID *context) {

    (void)once;(void)parameter;(void)context;

    InitializeCriticalSection(&private_table_lock);

    const char *mode=getenv("BB_UWP_PRIVATE_CPU");private_table_mode=mode && mode[0]=='1';

    return TRUE;

}

static int recursive_table_mode(void) {

    InitOnceExecuteOnce(&table_lock_once,initialize_table_lock,NULL,NULL);return private_table_mode;

}

#endif

/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */

static uint64_t table_generation;

#ifdef _WIN32

/* Diagnostic timing only. Per-thread counters avoid a new global hot lock.

 * General table locks are sampled 1/64; upload timings are measured per call. */

typedef struct {

    int initialized,enabled;

    uint64_t frequency,last,lock_checks,lock_samples,lock_wait;

    uint64_t copies,bytes,zero_bytes,copy_ticks,copy_lock_ticks,queries,query_ticks,cache_hits,cache_misses;

    uint64_t cache_cold948,cache_generation948,cache_collision948,cache_odd948,query_commit948,query_reserve948,query_other948;

} UploadTiming;

static __thread UploadTiming upload_timing;

static uint64_t upload_clock(void) { LARGE_INTEGER t;QueryPerformanceCounter(&t);return (uint64_t)t.QuadPart; }

static int upload_timing_enabled(void) {

    if (!upload_timing.initialized) {

        upload_timing.initialized=1;

        const char *v=getenv("BB_FRAME_STATS");upload_timing.enabled=v && v[0]=='1';

        if (upload_timing.enabled) { LARGE_INTEGER f;QueryPerformanceFrequency(&f);upload_timing.frequency=(uint64_t)f.QuadPart;upload_timing.last=upload_clock(); }

    }

    return upload_timing.enabled;

}

#include "copy_trace948.h"

static void upload_timing_report(uint64_t now) {

    UploadTiming *p=&upload_timing;

    if(now-p->last < p->frequency*5)return;

    const double ms=1000.0/(double)p->frequency;

    printf("Xbox memory copies: thread=%lu window_ms=%.1f calls=%" PRIu64 " MiB=%.3f zero_MiB=%.3f total_ms=%.3f copy_lock_ms=%.3f queries=%" PRIu64 " query_ms=%.3f table_lock_samples=%" PRIu64 " table_lock_sample_ms=%.3f table_lock_checks=%" PRIu64 " cache_hits=%" PRIu64 " cache_misses=%" PRIu64 "\n",

           (unsigned long)GetCurrentThreadId(),(double)(now-p->last)*ms,p->copies,(double)p->bytes/1048576.0,(double)p->zero_bytes/1048576.0,(double)p->copy_ticks*ms,(double)p->copy_lock_ticks*ms,p->queries,(double)p->query_ticks*ms,p->lock_samples,(double)p->lock_wait*ms,p->lock_checks,p->cache_hits,p->cache_misses);

    printf("CACHE948: tick=%llu tid=%lu calls=%llu misses=%llu cold=%llu generation=%llu collision=%llu odd=%llu commit=%llu reserve=%llu other=%llu\n",

        (unsigned long long)GetTickCount64(),(unsigned long)GetCurrentThreadId(),

        (unsigned long long)p->copies,(unsigned long long)p->cache_misses,

        (unsigned long long)p->cache_cold948,(unsigned long long)p->cache_generation948,

        (unsigned long long)p->cache_collision948,(unsigned long long)p->cache_odd948,

        (unsigned long long)p->query_commit948,(unsigned long long)p->query_reserve948,

        (unsigned long long)p->query_other948);

    cpu_copy_report948(GetTickCount64(),p->frequency);

    const uint64_t frequency=p->frequency;

    *p=(UploadTiming){.initialized=1,.enabled=1,.frequency=frequency,.last=now};

}

#endif



/* TLOU debug (BB_MEM_TRACE line): table write locks by source line (each one invalidates every

 * thread's region cache), read locks taken and those that had to wait, and the wait. */

#define LOCK_LINES 4096

static uint64_t write_lock_lines[LOCK_LINES], read_locks, read_lock_waits, read_lock_wait_ticks;

/* The read-lock counters only with BB_MEM_TRACE=1 (a shared counter on ~400k locks/s). */

static int lock_trace_on(void) {

    static int on=-1;

    if (on<0) { const char *e=getenv("BB_MEM_TRACE"); on=e && e[0]=='1'; }

    return on;

}

static void write_lock_at(unsigned line) {

    if (!exclusive_depth) __atomic_add_fetch(&write_lock_lines[line & (LOCK_LINES-1)],1,__ATOMIC_RELAXED);

    if (!exclusive_depth++) {

#ifdef _WIN32

        if (recursive_table_mode()) EnterCriticalSection(&private_table_lock);else

#endif

        pthread_rwlock_wrlock(&lock);

        __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL);

    }

}

static void write_unlock(void) {

    if (!--exclusive_depth) {

        __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE);

#ifdef _WIN32

        if (recursive_table_mode()) LeaveCriticalSection(&private_table_lock);else

#endif

        pthread_rwlock_unlock(&lock);

    }

}

#define write_lock() write_lock_at(__LINE__)

static void read_lock(void) { if (!exclusive_depth && !shared_depth++) {

#ifdef _WIN32

    uint64_t began=0;

    if(upload_timing_enabled() && (++upload_timing.lock_checks%64)==0)began=upload_clock();

    if (recursive_table_mode()) {

        if (!lock_trace_on()) EnterCriticalSection(&private_table_lock);

        else if (__atomic_add_fetch(&read_locks,1,__ATOMIC_RELAXED),!TryEnterCriticalSection(&private_table_lock)) {

            const uint64_t wait_began=upload_clock();

            EnterCriticalSection(&private_table_lock);

            __atomic_add_fetch(&read_lock_waits,1,__ATOMIC_RELAXED);

            __atomic_add_fetch(&read_lock_wait_ticks,upload_clock()-wait_began,__ATOMIC_RELAXED);

        }

    } else

#endif

    pthread_rwlock_rdlock(&lock);

#ifdef _WIN32

    if(began) {upload_timing.lock_samples++;upload_timing.lock_wait+=upload_clock()-began;}

#endif

} }

static void read_unlock(void) { if (!exclusive_depth && !--shared_depth) {

#ifdef _WIN32

    if (recursive_table_mode()) LeaveCriticalSection(&private_table_lock);else

#endif

    pthread_rwlock_unlock(&lock);

} }

/* read_lock without waiting: 0 when another thread holds the table (the pager's page-in helper:

 * the thread that asked it may hold the table and wait for it, 2026-11-02). */

static int try_read_lock(void) {

    if (exclusive_depth || shared_depth) { if (!exclusive_depth) ++shared_depth; return 1; }

#ifdef _WIN32

    if (recursive_table_mode()) { if (!TryEnterCriticalSection(&private_table_lock)) return 0; }

    else

#endif

    if (pthread_rwlock_tryrdlock(&lock)) return 0;

    shared_depth=1;

    return 1;

}

#ifdef _WIN32

/* host_commit_fault: faults it saw since the last call (stale and let through, first touch,

 * passed on to the GPU/guest handlers) and microseconds in its first VirtualQuery. */

static uint64_t host_fault_query_ticks;

extern uint64_t runtime_host_fault_counts[3];

void runtime_memory_fault_stats(uint64_t counts[3], uint64_t *query_us) {

    static uint64_t last[3], last_ticks;

    for (int i=0;i<3;++i) {

        const uint64_t v=__atomic_load_n(&runtime_host_fault_counts[i],__ATOMIC_RELAXED);

        counts[i]=v-last[i]; last[i]=v;

    }

    const uint64_t t=__atomic_load_n(&host_fault_query_ticks,__ATOMIC_RELAXED);

    LARGE_INTEGER f; QueryPerformanceFrequency(&f);

    *query_us=f.QuadPart ? (t-last_ticks)*1000000/(uint64_t)f.QuadPart : 0;

    last_ticks=t;

}

/* Since the last call: write locks, read locks, waits and wait microseconds; busiest writers. */

void runtime_memory_lock_stats(uint64_t *writes, uint64_t *reads, uint64_t *waits, uint64_t *wait_us,

                               char *top, size_t capacity) {

    static uint64_t last_lines[LOCK_LINES], last_reads, last_waits, last_ticks;

    uint64_t delta[LOCK_LINES], w=0;

    for (unsigned i=0;i<LOCK_LINES;++i) {

        const uint64_t v=__atomic_load_n(&write_lock_lines[i],__ATOMIC_RELAXED);

        delta[i]=v-last_lines[i]; last_lines[i]=v; w+=delta[i];

    }

    const uint64_t r=__atomic_load_n(&read_locks,__ATOMIC_RELAXED), n=__atomic_load_n(&read_lock_waits,__ATOMIC_RELAXED),

        t=__atomic_load_n(&read_lock_wait_ticks,__ATOMIC_RELAXED);

    LARGE_INTEGER f; QueryPerformanceFrequency(&f);

    *writes=w; *reads=r-last_reads; *waits=n-last_waits;

    *wait_us=f.QuadPart ? (t-last_ticks)*1000000/(uint64_t)f.QuadPart : 0;

    last_reads=r; last_waits=n; last_ticks=t;

    size_t used=0; top[0]=0;

    for (int k=0;k<4;++k) {

        unsigned best=0;

        for (unsigned i=1;i<LOCK_LINES;++i) if (delta[i]>delta[best]) best=i;

        if (!delta[best] || used+24>=capacity) break;

        used+=(size_t)snprintf(top+used,capacity-used,"%sL%u:%llu",k?" ":"",best,(unsigned long long)delta[best]);

        delta[best]=0;

    }

}

#endif

static Block blocks[LIMIT];

static Vma *vmas; static size_t vma_count, vma_capacity;

#ifndef _WIN32

static int pool_fd=-1;

#endif

static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */

#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)

static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];

/* GPU hooks (bbgpu): notified outside the lock, in order, after each operation. */

typedef void (*GpuRange)(uintptr_t address, uint64_t size);

static GpuRange hook_map, hook_unmap, hook_invalidate;

enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };

typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;

/* Small operations need no allocation. A fragmented mapping or a batch can

 * legitimately produce more than 64 notifications before the table lock can

 * be dropped. Grow storage without dropping, merging, or executing callbacks

 * under that lock. pending_count rollback in direct_unmap remains valid. */

static PendingHook pending_inline[64];

static PendingHook *pending=pending_inline;

static size_t pending_count, pending_capacity=64;

static size_t allocations, maps, flexible_maps, protects, queries;

static uint64_t live_bytes, flexible_bytes;



static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }

static int block_type(uint64_t phys);

static size_t vma_index(uintptr_t a);

static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }

static int host_prot(int prot) {

    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */

    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);

}

#ifdef _WIN32

#define TOUCH_CHUNK_SHIFT 26

#define SHARED_CHUNK (UINT64_C(1)<<TOUCH_CHUNK_SHIFT)

static int gpu_shared_enabled(void);

static int gpu_shared_host_map(uintptr_t address,uint64_t size,int prot,uint64_t phys,int cpu_only);

/* Spider-Man port (2026-11-05): BB_GPU_SHARED_RANGES, guest address ranges to share (see gpu_shared_host_map). */
static uintptr_t gpu_shared_ranges[16][2];
static int gpu_shared_range_count=-1;
static int gpu_shared_in_ranges(uintptr_t a, uint64_t size) {
    if (gpu_shared_range_count<0) {
        gpu_shared_range_count=0;
        const char *at=getenv("BB_GPU_SHARED_RANGES");
        while (at && *at && gpu_shared_range_count<16) {
            char *end;
            const unsigned long long lo=strtoull(at,&end,16);
            if (*end!='-') break;
            const unsigned long long hi=strtoull(end+1,&end,16);
            gpu_shared_ranges[gpu_shared_range_count][0]=(uintptr_t)lo;
            gpu_shared_ranges[gpu_shared_range_count][1]=(uintptr_t)hi;
            ++gpu_shared_range_count;
            if (*end!=',') break;
            at=end+1;
        }
    }
    for (int i=0;i<gpu_shared_range_count;++i)
        if (a<gpu_shared_ranges[i][1] && gpu_shared_ranges[i][0]<a+size) return 1;
    return 0;
}

/* ---- host mappings (Windows) ----

 * [USER_MIN,USER_MAX) is covered by regions, each one placeholder or one view of the

 * section. Mapping a range carves it out as a single placeholder first: views overlapping

 * it are unmapped, placeholders coalesced and split, and the parts of views outside the

 * range mapped again with the page protections they had (GPU page tracking included). */

typedef struct { uintptr_t start, end; uint64_t phys; int mapped; DWORD initial_protect; } HostRegion;

typedef struct { uintptr_t start, end; DWORD protect; } ProtRun;

static HostRegion *regions; static size_t region_count, region_capacity;

static HANDLE section;

static HANDLE backing_files[64],file_sections[64];

static int file_backed;

/* Sectioned pool (2026-11-02): the Xbox caps shared (section) commit at ~950 MiB per process

 * (shared-cap-probe-20261102: 952 MiB alone, 954 MiB beside 3 GiB private, 948 MiB over two

 * sections), and committed pages of a pagefile section are never decommitted, only zeroed

 * (host_discard). One pool section only grew to that cap over a session (Xbox stops 4 and 8 at

 * shared 859/875 MiB). With the pool in sections of pool_section_bytes (BB_POOL_SECTION_MB,

 * default 32 with private CPU chunks; 0: one section), a section no view references any more can

 * be closed and made again empty, which gives its commit back (shared_reclaim). */

static HANDLE *pool_sections;

static uint64_t pool_section_bytes;

static size_t pool_section_count;

static int private_cpu;

static int private_gpu;

static uintptr_t zero_trim_guard,zero_trim_cursor;

#define PRIVATE_CHUNK UINT64_C(65536)

/* The pager (below, runtime_memory_pager_*): private chunks the guest leaves alone go to a file

 * near the app's memory limit. Code that changes or reads a private chunk's host mapping brings

 * the chunks back first; a chunk that came back is a view of a section of its own. */



/* ---- Pager and renderer events (2026-11-02) ----

 * The Xbox GPU hung after checkpoints only with the pager on (3 sessions; none in 19 min with

 * BB_PAGER=0), yet the pager's own work under checkpoint-like I/O and GPU load did not hang it

 * (pager-probe-20261102: 3.8 GB out and back in 15 min). So it is the pager together with

 * something the renderer does. This ring keeps the last TRACE_EVENTS pager actions, faults and

 * bring-backs on paged chunks (with the code that touched them), GPU submits and guest thread

 * starts; runtime_trace_dump() writes them out when the GPU is lost or stalls

 * (<user dir>\pager-gpu-events-N.txt). BB_PAGER_EVENTS=0 turns it off. */

enum { TR_SWEEP=1, TR_EVICT, TR_EVICT_ABORT, TR_PAGE_IN, TR_FAULT, TR_GUARD_WAIT, TR_BRING_BACK, TR_PROMOTE,

       TR_MAKE_ROOM, TR_SUBMIT, TR_THREAD, TR_HOST_PIN, TR_PAGE_IN_DIFF, TR_KINDS };

static const char *const trace_names[TR_KINDS]={"?","sweep","evict","evict-abort","page-in","fault","guard-wait",

    "bring-back","promote","make-room","gpu-submit","guest-thread","host-pin","page-in-diff"};

typedef struct { int64_t qpc; uint64_t a, b; uintptr_t code; uint32_t tid; volatile uint16_t kind; uint16_t pad; } TraceEvent;

#define TRACE_EVENTS 65536

static TraceEvent *volatile trace_ring;

static volatile LONG64 trace_next;

/* Set up by pager_start: events come from the fault handler, often on a guest fiber's small stack,

 * where getenv or VirtualAlloc overflowed it (PC 2026-11-02: 0xc00000fd in ucrtbase). */

static void trace_start(void) {

    const char *e=getenv("BB_PAGER_EVENTS");

    if (e && e[0]=='0') return;

    trace_ring=VirtualAlloc(NULL,TRACE_EVENTS*sizeof(TraceEvent),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);

}

void runtime_trace_event(int kind, uint64_t a, uint64_t b, const void *code) {

    if (!trace_ring) return;

    TraceEvent *e=&trace_ring[(InterlockedIncrement64(&trace_next)-1)&(TRACE_EVENTS-1)];

    LARGE_INTEGER now; QueryPerformanceCounter(&now);

    e->kind=0;

    e->qpc=now.QuadPart; e->a=a; e->b=b; e->code=(uintptr_t)code; e->tid=GetCurrentThreadId();

    __atomic_store_n(&e->kind,(uint16_t)kind,__ATOMIC_RELEASE);

}

/* Code addresses inside this DLL print as rt+offset (llvm-symbolizer on the unstripped build). */

static void trace_code(char *out, size_t size, uintptr_t code, uintptr_t base, uintptr_t image) {

    if (!code) snprintf(out,size,"-");

    else if (code>=base && code<base+image) snprintf(out,size,"rt+0x%llx",(unsigned long long)(code-base));

    else snprintf(out,size,"%p",(void*)code);

}

void runtime_trace_dump(const char *why) {

    static volatile LONG dumps;

    if (!trace_ring) { printf("Pager events: none recorded (%s)\n",why); return; }

    const LONG number=InterlockedIncrement(&dumps);

    if (number>4) return;

    LARGE_INTEGER frequency, now; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&now);

    HMODULE self=NULL;

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)runtime_trace_dump,&self);

    const uintptr_t base=(uintptr_t)self;

    const uintptr_t image=self ? ((const IMAGE_NT_HEADERS*)(base+((const IMAGE_DOS_HEADER*)self)->e_lfanew))->OptionalHeader.SizeOfImage : 0;

    extern const char *runtime_file_user_dir(void);

    char path[MAX_PATH*2];

    snprintf(path,sizeof(path),"%s\\pager-gpu-events-%ld.txt",runtime_file_user_dir(),(long)number);

    FILE *out=fopen(path,"w");

    const LONG64 end=trace_next, begin=end>TRACE_EVENTS ? end-TRACE_EVENTS : 0;

    unsigned recent5[TR_KINDS]={0}, recent30[TR_KINDS]={0}, host5=0, host30=0;

    if (out) fprintf(out,"why=%s events=%lld (kept %lld) rt=%p; ms before the dump, thread, kind, a, b, code\n",why,(long long)end,(long long)(end-begin),(void*)base);

    for (LONG64 n=begin;n<end;++n) {

        const TraceEvent *e=&trace_ring[n&(TRACE_EVENTS-1)];

        const unsigned kind=__atomic_load_n(&e->kind,__ATOMIC_ACQUIRE);

        if (!kind || kind>=TR_KINDS) continue;

        const double ms=(double)(now.QuadPart-e->qpc)*1000.0/(double)frequency.QuadPart;

        const int in_host=e->code>=base && e->code<base+image;

        if (ms<=5000) { ++recent5[kind]; host5+=kind==TR_FAULT && in_host; }

        if (ms<=30000) { ++recent30[kind]; host30+=kind==TR_FAULT && in_host; }

        if (!out) continue;

        char code[48]; trace_code(code,sizeof(code),e->code,base,image);

        if (kind==TR_THREAD) { char name[17]={0}; memcpy(name,&e->a,8); memcpy(name+8,&e->b,8);

            fprintf(out,"%10.3f %6lu %-12s %s\n",-ms,(unsigned long)e->tid,trace_names[kind],name); }

        else fprintf(out,"%10.3f %6lu %-12s %llx %llx %s\n",-ms,(unsigned long)e->tid,trace_names[kind],

                     (unsigned long long)e->a,(unsigned long long)e->b,code);

    }

    if (out) fclose(out);

    for (int w=0;w<2;++w) {

        const unsigned *c=w ? recent30 : recent5;

        printf("Pager events (%s), last %s s: sweep %u, evict %u (aborted %u), page-in %u, fault %u (%u by host code), guard-wait %u, bring-back %u, promote %u, make-room %u, gpu-submit %u, guest threads %u\n",

               why,w ? "30" : "5",c[TR_SWEEP],c[TR_EVICT],c[TR_EVICT_ABORT],c[TR_PAGE_IN],c[TR_FAULT],w ? host30 : host5,

               c[TR_GUARD_WAIT],c[TR_BRING_BACK],c[TR_PROMOTE],c[TR_MAKE_ROOM],c[TR_SUBMIT],c[TR_THREAD]);

    }

    printf("Pager events: written to %s%s\n",path,out ? "" : " (NOT: open failed)");

    fflush(stdout);

}

static int pager_active;

static void pager_bring_back(uintptr_t a, uintptr_t b);

static int pager_unmap_view(uintptr_t chunk);

static int pager_is_view(uintptr_t chunk);

static void pager_start(void);

static uint64_t pager_make_room(uint64_t want);

static BOOL commit_with_room(void *address, SIZE_T size, DWORD protect, const char *what);

static unsigned char **private_store;
/* Spider-Man port (2026-11-05): 16 KiB private pieces (regions of type 4) and their off-address store. */
#define SMALL_CHUNK UINT64_C(16384)
static unsigned char **small_store;
static uint64_t small_store_count;
static int small_private=-1;
static uint64_t small_mapped_bytes, small_said;

static int discarding_private;

static uint64_t discard_begin,discard_end;

#define FILE_VIEW_BYTES (UINT64_C(128)*1024*1024)

#define FILE_STORE_BYTES (UINT64_C(512)*1024*1024)

static int memory_reserved, lazy_guest_memory, demand_commit, touch_commit;

static uint64_t touch_cpu_min_bytes;

static uint64_t touch_cpu_page=PAGE;

static int touch_flexible,touch_all_host_pages;

static uint64_t touch_faults;

static uint64_t private_touch_page=PAGE;

typedef struct { uintptr_t rip, address, rsp, r11, r9; uint64_t stack[16]; } TouchFault;

static __thread TouchFault touch_history[8];

static __thread unsigned touch_history_index;

void runtime_memory_dump_touch_history(void) {

    if (!touch_commit) return;

    for (unsigned n=0;n<8 && n<touch_history_index;++n) {

        TouchFault *f=&touch_history[(touch_history_index-1-n)%8];

        fprintf(stderr,"Touch history: rip=%p address=%p rsp=%p r11=%p r9=%p redzone[-128]=%llx redzone[-48]=%llx redzone[-8]=%llx\n",

                (void*)f->rip,(void*)f->address,(void*)f->rsp,(void*)f->r11,(void*)f->r9,

                (unsigned long long)f->stack[0],(unsigned long long)f->stack[10],(unsigned long long)f->stack[15]);

    }

}

static DWORD win_prot(int prot) {

    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;

    if (prot & PROT_WRITE) return PAGE_READWRITE;

    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;

}

static void win_fail(const char *what, uintptr_t address, uint64_t size) {

    fprintf(stderr,"STOP: %s failed at %p (+0x%" PRIx64 "), error %lu\n",what,(void *)address,size,GetLastError());

    exit(21);

}

static size_t region_index(uintptr_t a) { /* first region with end > a */

    size_t lo=0, hi=region_count;

    while (lo<hi) { size_t mid=(lo+hi)/2; if (regions[mid].end<=a) lo=mid+1; else hi=mid; }

    return lo;

}

static void region_insert(size_t at, HostRegion r) {

    if (region_count==region_capacity) {

        size_t capacity=region_capacity ? region_capacity*2 : 256;

        HostRegion *next=realloc(regions,capacity*sizeof(*regions));

        if (!next) { fputs("Out of memory (host regions)\n",stderr); exit(1); }

        regions=next; region_capacity=capacity;

    }

    memmove(regions+at+1,regions+at,(region_count-at)*sizeof(*regions));

    regions[at]=r; ++region_count;

}

static void region_erase(size_t at, size_t n) {

    memmove(regions+at,regions+at+n,(region_count-at-n)*sizeof(*regions)); region_count-=n;

}

static void claim_early_spans(void);

/* The guest range is reserved before anything else can be placed there (runtime_start). */

void runtime_memory_reserve(void) {

    if (memory_reserved) return;

    const char *lazy = getenv("BB_UWP_LAZY_RESERVATION");

    if (lazy && lazy[0]=='1') {

        /* UWP host stacks/heaps can already occupy part of this range. Keep

         * them intact and claim only the free spans needed by guest mappings. */

        lazy_guest_memory=1;

        memory_reserved=1;

        puts("Runtime: reserving guest virtual spans on demand; existing host mappings are excluded");

        claim_early_spans();

        return;

    }

    if (!VirtualAlloc2(NULL,(void *)USER_MIN,USER_MAX-USER_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))

        win_fail("reserving the PS4 address range (link with --disable-high-entropy-va)",USER_MIN,USER_MAX-USER_MIN);

    region_insert(0,(HostRegion){USER_MIN,USER_MAX,0,0,0});

    memory_reserved=1;

}



/* True only for free address space or placeholders/views owned by this runtime.

 * Round outward to allocation granularity before checking, so claiming a small

 * PS4 page cannot include an adjacent host allocation. */

static int host_can_own(uintptr_t address, uint64_t size, uintptr_t *after_conflict) {

    const uint64_t granularity=UINT64_C(65536);

    if (address<USER_MIN || address>=USER_MAX || size>USER_MAX-address) return 0;

    uintptr_t at=address&~(uintptr_t)(granularity-1), end=align_up(address+size,granularity);

    while (at<end) {

        size_t i=region_index(at);

        if (i<region_count && regions[i].start<=at) {

            at=regions[i].end;

            continue;

        }

        MEMORY_BASIC_INFORMATION info;

        if (!VirtualQuery((void *)at,&info,sizeof(info))) return 0;

        uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;

        if (next<=at) return 0;

        if (info.State!=MEM_FREE) {

            if (after_conflict) *after_conflict=align_up(next,granularity);

            return 0;

        }

        if (i<region_count && next>regions[i].start) next=regions[i].start;

        at=next;

    }

    return 1;

}



static int host_ensure_owned(uintptr_t address, uint64_t size) {

    if (!host_can_own(address,size,NULL)) return 0;

    const uint64_t granularity=UINT64_C(65536);

    uintptr_t at=address&~(uintptr_t)(granularity-1), end=align_up(address+size,granularity);

    while (at<end) {

        size_t i=region_index(at);

        if (i<region_count && regions[i].start<=at) {

            at=regions[i].end;

            continue;

        }

        uintptr_t next=i<region_count && regions[i].start<end ? regions[i].start : end;

        /* Another host thread may allocate after the query. Reservation fails

         * safely in that case; never replace or release an unowned mapping. */

        if (!VirtualAlloc2(NULL,(void *)at,next-at,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)) return 0;

        region_insert(i,(HostRegion){at,next,0,0,0});

        at=next;

    }

    return 1;

}

/* 2026-10-31: TLOU maps its direct memory first-fit from USER_MIN (heaps at 0x1000000000,

 * 0x1100000000 and 0x1200000000) and its code assumes those bases. Claimed on demand, a guest

 * or host thread stack that Windows placed at 0x106a9f... before the first mapping pushed the

 * 2588 MiB heap to 0x107fa00000 and the game read 0xffffffffffffffff at startup. The free spans

 * of the first BB_UWP_EARLY_RESERVE_MB (default 9216; 0 off) are claimed before any guest

 * thread exists; host mappings already there are kept (and logged). */

static void claim_early_spans(void) {

    const char *env=getenv("BB_UWP_EARLY_RESERVE_MB");

    const uint64_t mb=env ? strtoull(env,NULL,10) : 9216;

    if (!mb) return;

    uintptr_t end=ALLOC_MIN+(uintptr_t)(mb<<20);

    if (end>USER_MAX || end<ALLOC_MIN) end=USER_MAX;

    uint64_t claimed=0, kept=0;

    unsigned conflicts=0;

    for (uintptr_t at=ALLOC_MIN; at<end;) {

        MEMORY_BASIC_INFORMATION m;

        if (!VirtualQuery((void *)at,&m,sizeof(m))) break;

        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;

        if (next>end) next=end;

        if (next<=at) break;

        if (m.State==MEM_FREE) {

            const uintptr_t s=align_up(at,UINT64_C(65536)), e=next&~(uintptr_t)0xffff;

            if (e>s && host_ensure_owned(s,e-s)) claimed+=e-s;

        } else {

            kept+=next-at;

            if (++conflicts<=4)

                printf("Runtime: host mapping %p..%p in the game's heap range kept (state 0x%lx, type 0x%lx)\n",

                       (void *)at,(void *)next,(unsigned long)m.State,(unsigned long)m.Type);

        }

        at=next;

    }

    printf("Runtime: claimed %llu MiB of the game's heap range at startup; %u host mappings there kept (%llu MiB)\n",

           (unsigned long long)(claimed>>20),conflicts,(unsigned long long)(kept>>20));

}

static uint64_t view_part_size(uint64_t phys,uint64_t remaining) {

    uint64_t n=file_backed ? FILE_VIEW_BYTES-(phys%FILE_VIEW_BYTES) :

               pool_section_bytes ? pool_section_bytes-(phys%pool_section_bytes) : remaining;

    return n<remaining ? n : remaining;

}

/* Commits [phys,+size) of the backing view: one call per section (a commit cannot span views). */

static BOOL backing_commit(uint64_t phys,uint64_t size) {

    for (uint64_t done=0;done<size;) {

        const uint64_t n=view_part_size(phys+done,size-done);

        if (!VirtualAlloc(backing_base+phys+done,n,MEM_COMMIT,PAGE_READWRITE)) return FALSE;

        done+=n;

    }

    return TRUE;

}

static void map_section_range(uintptr_t address,uint64_t phys,uint64_t size,DWORD protect) {

    for (uint64_t done=0;done<size;) {

        const uint64_t n=view_part_size(phys+done,size-done);

        if (n<size-done && !VirtualFree((void*)(address+done),n,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))

            win_fail("splitting file view placeholder",address+done,n);

        const uint64_t physical=phys+done;

        HANDLE source=file_backed ? file_sections[physical/FILE_STORE_BYTES] :

                      pool_section_bytes ? pool_sections[physical/pool_section_bytes] : section;

        const uint64_t offset=file_backed ? physical%FILE_STORE_BYTES : pool_section_bytes ? physical%pool_section_bytes : physical;

        if (!MapViewOfFile3(source,GetCurrentProcess(),(void*)(address+done),offset,n,MEM_REPLACE_PLACEHOLDER,protect,NULL,0)) {

            const DWORD error=GetLastError();

            PROCESS_MEMORY_COUNTERS_EX memory={.cb=sizeof(memory)};

            K32GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&memory,sizeof(memory));

            fprintf(stderr,"Section view failed: file=%d physical=0x%llx range_done=%llu bytes=%llu working_set=%llu private=%llu error=%lu\n",

                    file_backed,(unsigned long long)(phys+done),(unsigned long long)done,(unsigned long long)n,

                    (unsigned long long)memory.WorkingSetSize,(unsigned long long)memory.PrivateUsage,error);

            SetLastError(error);

            win_fail("MapViewOfFile3",address+done,n);

        }

        done+=n;

    }

}

static int protect_section_range(uintptr_t address,uint64_t phys,uint64_t size,DWORD protect) {

    DWORD old;

    for (uint64_t done=0;done<size;) {

        const uint64_t n=view_part_size(phys+done,size-done);

        if (!VirtualProtect((void*)(address+done),n,protect,&old)) return 0;

        done+=n;

    }

    return 1;

}

static void view_unmap(const HostRegion *r) {

    uint64_t done=0,parts=0,size=r->end-r->start;

    while (done<size) {

        const uint64_t n=view_part_size(r->phys+done,size-done);

        if (!UnmapViewOfFile2(GetCurrentProcess(),(void*)(r->start+done),MEM_PRESERVE_PLACEHOLDER))

            win_fail("UnmapViewOfFile2",r->start+done,n);

        done+=n; ++parts;

    }

    if (parts>1 && !VirtualFree((void*)r->start,size,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))

        win_fail("coalescing file view placeholders",r->start,size);

}

static void view_map(const HostRegion *r, DWORD protect) {

    DWORD initial=protect==PAGE_NOACCESS ? PAGE_READWRITE : protect;

    map_section_range(r->start,r->phys,r->end-r->start,initial);

    if (initial!=protect) {

        // PAGE_NOACCESS cannot be the initial section-view protection. A

        // protection change needs committed pages, unlike a normal lazy map.

        if (touch_commit && !backing_commit(r->phys,r->end-r->start))

            win_fail("committing no-access view",r->start,r->end-r->start);

        if (!protect_section_range(r->start,r->phys,r->end-r->start,protect))

            win_fail("VirtualProtect",r->start,r->end-r->start);

    }

}

static size_t save_protections(uintptr_t start, uintptr_t end, ProtRun **runs, size_t count, size_t *capacity) {

    for (uintptr_t at=start; at<end;) {

        MEMORY_BASIC_INFORMATION m;

        if (!VirtualQuery((void *)at,&m,sizeof(m))) win_fail("VirtualQuery",at,0);

        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;

        if (next>end) next=end;

        if (m.State==MEM_COMMIT) {

            if (count==*capacity) { *capacity=*capacity ? *capacity*2 : 16; *runs=realloc(*runs,*capacity*sizeof(**runs)); }

            (*runs)[count++]=(ProtRun){at,next,m.Protect};

        }

        at=next;

    }

    return count;

}

/* CPU-only chunks have one private owner. A second alias is promoted to the

 * section, while a full unmap can keep its contents privately off-address. */

static void private_copy_pages(unsigned char *source,unsigned char *dest,uint64_t size) {

    for (uint64_t at=0;at<size;) {

        MEMORY_BASIC_INFORMATION info;

        if (!VirtualQuery(source+at,&info,sizeof(info))) win_fail("querying private CPU storage",(uintptr_t)(source+at),size-at);

        uint64_t n=(uintptr_t)info.BaseAddress+info.RegionSize-(uintptr_t)(source+at);

        if(n>size-at)n=size-at;

        if(!n)win_fail("invalid private CPU extent",(uintptr_t)(source+at),size-at);

        if(info.State==MEM_COMMIT) {

            if(info.Protect==PAGE_NOACCESS)win_fail("private CPU page requires promotion before no-access",(uintptr_t)(source+at),n);

            if(!commit_with_room(dest+at,n,PAGE_READWRITE,"committing CPU storage destination"))win_fail("committing CPU storage destination",(uintptr_t)(dest+at),n);

            memcpy(dest+at,source+at,n);

        }

        at+=n;

    }

}

static void private_placeholder(HostRegion *r) {

    /* A chunk that came back from the pager's file is a view of a section, not an allocation. */

    if(pager_active && pager_unmap_view(r->start)) { r->mapped=0; return; }

    if(!VirtualFree((void*)r->start,r->end-r->start,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) {

        const DWORD error=GetLastError();

        if(error!=ERROR_INVALID_PARAMETER && error!=ERROR_NOT_SUPPORTED)

            win_fail("releasing private CPU chunk to placeholder",r->start,r->end-r->start);

        /* Some Xbox builds reject preserving a replaced private allocation.

         * Release only this owned chunk and claim the exact free range again.

         * A competing host allocation makes reservation fail safely. */

        if(!VirtualFree((void*)r->start,0,MEM_RELEASE))win_fail("releasing owned private CPU chunk",r->start,r->end-r->start);

        if(!VirtualAlloc2(NULL,(void*)r->start,r->end-r->start,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))

            win_fail("reclaiming private CPU placeholder",r->start,r->end-r->start);

    }

    r->mapped=0;

}

static void private_promote(size_t index) {

    HostRegion *r=&regions[index];

    if(r->mapped!=2 && r->mapped!=4)return;

    runtime_trace_event(TR_PROMOTE,r->start,0,__builtin_return_address(0));

    pager_bring_back(r->start,r->end);

    ProtRun protections[16];size_t count=0;

    for(uintptr_t at=r->start;at<r->end;) {

        MEMORY_BASIC_INFORMATION info;

        if(!VirtualQuery((void*)at,&info,sizeof(info)))win_fail("querying CPU promotion",at,r->end-at);

        uintptr_t end=(uintptr_t)info.BaseAddress+info.RegionSize;if(end>r->end)end=r->end;

        if(info.State==MEM_COMMIT)protections[count++]=(ProtRun){at,end,info.Protect};

        at=end;

    }

    private_copy_pages((unsigned char*)r->start,backing_base+r->phys,r->end-r->start);

    private_placeholder(r);r->mapped=1;

    view_map(r,r->initial_protect);

    for(size_t n=0;n<count;++n) {

        DWORD old;

        if(!VirtualProtect((void*)protections[n].start,protections[n].end-protections[n].start,protections[n].protect,&old))

            win_fail("restoring promoted CPU protection",protections[n].start,protections[n].end-protections[n].start);

    }

}

static void private_save_unmapped(size_t index,int discard) {

    HostRegion *r=&regions[index];

    if(!discard) {

        unsigned char **slot=&private_store[r->phys/PRIVATE_CHUNK];

        if(*slot)win_fail("duplicate private CPU owner",r->start,PRIVATE_CHUNK);

        *slot=VirtualAlloc(NULL,PRIVATE_CHUNK,MEM_RESERVE,PAGE_READWRITE);

        if(!*slot)win_fail("reserving unmapped CPU storage",r->start,PRIVATE_CHUNK);

        private_copy_pages((unsigned char*)r->start,*slot,PRIVATE_CHUNK);

    }

    private_placeholder(r);r->phys=0;r->initial_protect=0;

}

static void private_restore_shadow(uint64_t phys,unsigned char *dest,DWORD protect) {

    unsigned char **slot=&private_store[phys/PRIVATE_CHUNK];

    if(!*slot)return;

    private_copy_pages(*slot,dest,PRIVATE_CHUNK);

    if(protect!=PAGE_READWRITE) for(uint64_t at=0;at<PRIVATE_CHUNK;) {

        MEMORY_BASIC_INFORMATION info;DWORD old;

        if(!VirtualQuery(dest+at,&info,sizeof(info)))win_fail("querying restored CPU storage",(uintptr_t)(dest+at),PRIVATE_CHUNK-at);

        uint64_t n=(uintptr_t)info.BaseAddress+info.RegionSize-(uintptr_t)(dest+at);

        if(n>PRIVATE_CHUNK-at)n=PRIVATE_CHUNK-at;

        if(info.State==MEM_COMMIT && !VirtualProtect(dest+at,n,protect,&old))win_fail("protecting restored CPU storage",(uintptr_t)(dest+at),n);

        at+=n;

    }

    if(!VirtualFree(*slot,0,MEM_RELEASE))win_fail("freeing unmapped CPU storage",(uintptr_t)*slot,PRIVATE_CHUNK);

    *slot=NULL;

}

/* Spider-Man port (2026-11-05): a 16 KiB private piece unmapped whole keeps its contents off-address,
   keyed by its physical page, like private_save_unmapped / private_restore_shadow for 64 KiB chunks. */
static void small_save_unmapped(size_t index,int discard) {

    HostRegion *r=&regions[index];

    if(!discard && r->phys/SMALL_CHUNK<small_store_count) {

        unsigned char **slot=&small_store[r->phys/SMALL_CHUNK];

        if(*slot) { VirtualFree(*slot,0,MEM_RELEASE); *slot=NULL; }

        *slot=VirtualAlloc(NULL,SMALL_CHUNK,MEM_RESERVE,PAGE_READWRITE);

        if(!*slot)win_fail("reserving unmapped CPU storage (16 KiB)",r->start,SMALL_CHUNK);

        private_copy_pages((unsigned char*)r->start,*slot,SMALL_CHUNK);

    }

    private_placeholder(r);r->phys=0;r->initial_protect=0;

}

static void small_restore_shadow(uint64_t phys,unsigned char *dest,DWORD protect) {

    if(phys/SMALL_CHUNK>=small_store_count)return;

    unsigned char **slot=&small_store[phys/SMALL_CHUNK];

    if(!*slot)return;

    private_copy_pages(*slot,dest,SMALL_CHUNK);

    if(protect!=PAGE_READWRITE) for(uint64_t at=0;at<SMALL_CHUNK;) {

        MEMORY_BASIC_INFORMATION info;DWORD old;

        if(!VirtualQuery(dest+at,&info,sizeof(info)))win_fail("querying restored CPU storage",(uintptr_t)(dest+at),SMALL_CHUNK-at);

        uint64_t n=(uintptr_t)info.BaseAddress+info.RegionSize-(uintptr_t)(dest+at);

        if(n>SMALL_CHUNK-at)n=SMALL_CHUNK-at;

        if(info.State==MEM_COMMIT && !VirtualProtect(dest+at,n,protect,&old))win_fail("protecting restored CPU storage",(uintptr_t)(dest+at),n);

        at+=n;

    }

    VirtualFree(*slot,0,MEM_RELEASE);

    *slot=NULL;

}

/* Leaves [a,b) as exactly one unmapped placeholder; returns its region index. */

static size_t carve_host(uintptr_t a, uintptr_t b) {

    for(size_t i=region_index(a);i<region_count && regions[i].start<b;++i)

        if(regions[i].mapped==3)win_fail("remapping GPU-shared guest memory (unsupported)",a,b-a);

    if(private_cpu) {

        for(size_t i=region_index(a);i<region_count && regions[i].start<b;++i) {

            HostRegion *r=&regions[i];if(r->mapped!=2 && r->mapped!=4)continue;

            pager_bring_back(r->start,r->end); /* its contents are read below */

            if(a<=r->start && r->end<=b) {

                const uint64_t span=r->end-r->start;

                const int discard=r->phys>=POOL_SIZE || (discarding_private && discard_begin<=r->phys && r->phys+span<=discard_end);

                if(r->mapped==4) small_save_unmapped(i,discard); else private_save_unmapped(i,discard);

            } else private_promote(i);

        }

    }

    size_t first=region_index(a), last=first;

    if (first==region_count || regions[first].start>a) win_fail("guest range outside the reservation",a,b-a);

    while (last<region_count && regions[last].start<b) ++last;

    if (regions[last-1].end<b) win_fail("guest range outside the reservation",a,b-a);

    uintptr_t lo=regions[first].start, hi=regions[last-1].end;

    HostRegion left=regions[first], right=regions[last-1];

    int keep_left=left.mapped && lo<a, keep_right=right.mapped && b<hi;

    if (first==last-1 && !left.mapped && lo==a && hi==b) return first;

    ProtRun *runs=NULL; size_t nruns=0, capacity=0, left_runs=0;

    if (keep_left) left_runs=nruns=save_protections(lo,a,&runs,nruns,&capacity);

    if (keep_right) nruns=save_protections(b,hi,&runs,nruns,&capacity);

    for (size_t i=first;i<last;++i) if (regions[i].mapped) {

        view_unmap(&regions[i]);

    }

    if (last-first>1 && !VirtualFree((void *)lo,hi-lo,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))

        win_fail("coalescing placeholders",lo,hi-lo);

    region_erase(first,last-first);

    region_insert(first,(HostRegion){lo,hi,0,0,0});

    size_t at=first;

    if (lo<a) {

        if (!VirtualFree((void *)lo,a-lo,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",lo,a-lo);

        regions[at].end=a;

        region_insert(at+1,(HostRegion){a,hi,0,0,0});

        if (keep_left) {

            regions[at].phys=left.phys; regions[at].mapped=1; regions[at].initial_protect=left.initial_protect;

            view_map(&regions[at],left.initial_protect);

        }

        ++at;

    }

    if (b<hi) {

        if (!VirtualFree((void *)a,b-a,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",a,b-a);

        regions[at].end=b;

        region_insert(at+1,(HostRegion){b,hi,0,0,0});

        if (keep_right) {

            regions[at+1].phys=right.phys+(b-right.start); regions[at+1].mapped=1; regions[at+1].initial_protect=right.initial_protect;

            view_map(&regions[at+1],right.initial_protect);

        }

    }

    for (size_t i=0;i<nruns;++i) {

        DWORD old;

        if (!VirtualProtect((void *)runs[i].start,runs[i].end-runs[i].start,runs[i].protect,&old))

            win_fail("restoring page protection",runs[i].start,runs[i].end-runs[i].start);

    }

    (void)left_runs;

    free(runs);

    return at;

}

static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys,int cpu_only) {

    if (lazy_guest_memory && !host_ensure_owned(address,size)) return -1;

    if (!reserved && gpu_shared_enabled() && phys+size<=POOL_SIZE &&
        (gpu_shared_in_ranges(address,size) || (gpu_shared_range_count==0 && block_type(phys)==3)) &&

        !(address%PRIVATE_CHUNK) && !(size%PRIVATE_CHUNK) && (prot&PROT_READ) &&

        address>=USER_MIN && size<=USER_MAX-address) {

        const int split=gpu_shared_host_map(address,size,prot,phys,cpu_only);

        if (split) return split<0 ? -1 : 0;

    }

    int use_private=private_cpu && (cpu_only || private_gpu) && !reserved && !(address%PRIVATE_CHUNK) && !(phys%PRIVATE_CHUNK) && !(size%PRIVATE_CHUNK) && (prot&PROT_READ);

    int use_small=small_private>0 && !use_private && private_cpu && (cpu_only || private_gpu) && !reserved &&
                  !((address|phys|size)%SMALL_CHUNK) && (prot&PROT_READ) && phys+size<=POOL_SIZE;

    if(private_cpu && !reserved) {

        for(size_t j=0;j<region_count;++j) {

            HostRegion *r=&regions[j];

            if(!r->mapped || r->phys>=phys+size || phys>=r->phys+(r->end-r->start))continue;

            if(r->mapped==3)win_fail("aliasing GPU-shared guest memory (unsupported)",address,size);

            use_private=0; use_small=0;

            if(r->mapped==2 || r->mapped==4)private_promote(j);

        }

        if(use_private) for(uint64_t at=0;at<size;) {

            MEMORY_BASIC_INFORMATION info;

            if(!VirtualQuery(backing_base+phys+at,&info,sizeof(info)))return -1;

            if(info.State==MEM_COMMIT) { use_private=0;break; }

            uint64_t n=(uintptr_t)info.BaseAddress+info.RegionSize-(uintptr_t)(backing_base+phys+at);

            if(!n)return -1;at+=n;

        }

    }

    if(use_small) for(uint64_t at=0;at<size;) {

        /* the backing already holds data for it (an earlier shared mapping): stays shared */

        MEMORY_BASIC_INFORMATION info;

        if(!VirtualQuery(backing_base+phys+at,&info,sizeof(info)))return -1;

        if(info.State==MEM_COMMIT) { use_small=0;break; }

        uint64_t n=(uintptr_t)info.BaseAddress+info.RegionSize-(uintptr_t)(backing_base+phys+at);

        if(!n)return -1;at+=n;

    }

    if(use_small) {

        size_t i=carve_host(address,address+size);

        const DWORD protect=win_prot(prot);

        for(uint64_t done=0;done<size;done+=SMALL_CHUNK,++i) {

            if(done+SMALL_CHUNK<size) {

                if(!VirtualFree((void*)(address+done),SMALL_CHUNK,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))win_fail("splitting private 16 KiB placeholder",address+done,SMALL_CHUNK);

                regions[i].end=address+done+SMALL_CHUNK;

                region_insert(i+1,(HostRegion){address+done+SMALL_CHUNK,address+size,0,0,0});

            }

            if(!VirtualAlloc2(NULL,(void*)(address+done),SMALL_CHUNK,MEM_RESERVE|MEM_REPLACE_PLACEHOLDER,protect,NULL,0))

                win_fail("reserving private 16 KiB piece",address+done,SMALL_CHUNK);

            regions[i]=(HostRegion){address+done,address+done+SMALL_CHUNK,phys+done,4,protect};

            small_restore_shadow(phys+done,(unsigned char*)(address+done),protect);

            if(!touch_commit && !commit_with_room((void*)(address+done),SMALL_CHUNK,protect,"committing private 16 KiB piece"))

                win_fail("committing private 16 KiB piece",address+done,SMALL_CHUNK);

        }

        small_mapped_bytes+=size;

        if(small_mapped_bytes-small_said>=((uint64_t)256<<20)) {

            small_said=small_mapped_bytes;

            printf("Small private: %llu MiB of 16 KiB-aligned mappings made private so far\n",(unsigned long long)(small_mapped_bytes>>20));

            fflush(stdout);

        }

        return 0;

    }

    if(use_private) {

        size_t i=carve_host(address,address+size);

        for(uint64_t done=0;done<size;done+=PRIVATE_CHUNK,++i) {

            if(done+PRIVATE_CHUNK<size) {

                if(!VirtualFree((void*)(address+done),PRIVATE_CHUNK,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))win_fail("splitting private CPU placeholder",address+done,PRIVATE_CHUNK);

                regions[i].end=address+done+PRIVATE_CHUNK;

                region_insert(i+1,(HostRegion){address+done+PRIVATE_CHUNK,address+size,0,0,0});

            }

            const DWORD protect=win_prot(prot);

            if(!VirtualAlloc2(NULL,(void*)(address+done),PRIVATE_CHUNK,MEM_RESERVE|MEM_REPLACE_PLACEHOLDER,protect,NULL,0))

                win_fail("reserving private CPU chunk",address+done,PRIVATE_CHUNK);

            regions[i]=(HostRegion){address+done,address+done+PRIVATE_CHUNK,phys+done,2,protect};

            private_restore_shadow(phys+done,(unsigned char*)(address+done),protect);

            if(!touch_commit && !commit_with_room((void*)(address+done),PRIVATE_CHUNK,protect,"committing private CPU chunk"))

                win_fail("committing private CPU chunk",address+done,PRIVATE_CHUNK);

        }

        return 0;

    }

    if(private_cpu && !reserved) {

        /* Spider-Man port (2026-11-05): why mappings go to shared backing (the Xbox ran out of commit
           at ~930 MB shared while private memory still had room). Printed every 128 MiB. */
        {
            static uint64_t by[6], total, said;
            int why=!(prot&PROT_READ) ? 0 : ((address|phys|size)%PRIVATE_CHUNK) ? 1 : (!cpu_only && !private_gpu) ? 2 : 3;
            if (why==3) for(size_t j=0;j<region_count;++j) {
                const HostRegion *r=&regions[j];
                if(r->mapped && r->phys<phys+size && phys<r->phys+(r->end-r->start)) { why=4; break; }
            }
            by[why]+=size; total+=size;
            if (total-said>=((uint64_t)128<<20)) {
                said=total;
                printf("Shared map: %llu MiB mapped shared (no read %llu, unaligned %llu, GPU not private %llu, backing committed %llu, aliased %llu MiB); last %#llx size %#llx phys %#llx prot %d cpu_only %d\n",
                       (unsigned long long)(total>>20),(unsigned long long)(by[0]>>20),(unsigned long long)(by[1]>>20),
                       (unsigned long long)(by[2]>>20),(unsigned long long)(by[3]>>20),(unsigned long long)(by[4]>>20),
                       (unsigned long long)address,(unsigned long long)size,(unsigned long long)phys,prot,cpu_only);
                fflush(stdout);
            }
        }

        const uint64_t begin=phys&~(PRIVATE_CHUNK-1),end=align_up(phys+size,PRIVATE_CHUNK);

        for(uint64_t at=begin;at<end;at+=PRIVATE_CHUNK)

            private_restore_shadow(at,backing_base+at,PAGE_READWRITE);

    }

    // Small CPU control/scratch mappings retain the original eager commitment.

    // Large CPU heaps can be tested separately from graphics and flexible memory.

    const int lazy_cpu=(touch_cpu_min_bytes && size>=touch_cpu_min_bytes &&

                       phys<POOL_SIZE && block_type(phys)==0) ||

                       (touch_flexible && phys>=POOL_SIZE);

    if (!reserved && demand_commit && (!touch_commit || (!lazy_cpu && (phys>=POOL_SIZE || block_type(phys)!=3))) &&

        !backing_commit(phys,size)) return -1;

    size_t i=carve_host(address,address+size);

    if (reserved) return 0;

    regions[i].phys=phys; regions[i].mapped=1; regions[i].initial_protect=win_prot(prot);

    view_map(&regions[i],win_prot(prot));

    return 0;

}

static int host_unmap(uintptr_t address, uint64_t size) { carve_host(address,address+size); return 0; }

static uint64_t upload_committed_span382(uintptr_t source);

/* bbport (unless BB_PROTECT_COMMIT_CACHE=0): GPU watch changes committed their range first on

 * every call (one VirtualAlloc system call each, ~4% of the Xbox GPU command thread), though it is

 * nearly always committed already: the upload commit cache (positive answers only, same table

 * generation) says so for ranges this thread copied from. A miss commits as before. */

static int protect_range_committed(uintptr_t source, uint64_t size) {

    static int enabled=-1;

    if(enabled<0){const char*e=getenv("BB_PROTECT_COMMIT_CACHE");enabled=!(e&&e[0]=='0');}

    return enabled && upload_committed_span382(source)>=size;

}

static int host_protect(uintptr_t address, uint64_t size, int prot) {

    DWORD protect=win_prot(prot), old;

    uintptr_t end=address+size;

    for (size_t i=region_index(address); i<region_count && regions[i].start<end; ++i) {

        if (!regions[i].mapped) continue;

        uintptr_t a=regions[i].start>address ? regions[i].start : address, b=regions[i].end<end ? regions[i].end : end;

        if(regions[i].mapped==3) {

            if(!VirtualProtect((void*)a,b-a,protect,&old))return -1;

            continue;

        }

        if(regions[i].mapped==2 || regions[i].mapped==4) {

            pager_bring_back(regions[i].start,regions[i].end);

            if(!(prot&PROT_READ)) private_promote(i);

            else if(pager_active && pager_is_view(regions[i].start)) {

                /* a chunk back from the pager's file: a view, committed whole */

                if(!VirtualProtect((void*)a,b-a,protect,&old))return -1;

                continue;

            } else {

                // Removing a GPU watch must not physically allocate untouched

                // neighbours in the write-fault window. Their first access

                // already restores the initial read/write protection, and the

                // GPU tracker marks this entire unprotected range CPU-dirty.

                if(protect==PAGE_READWRITE && regions[i].initial_protect==PAGE_READWRITE) {

                    // bbport: all of it committed (upload commit cache): one call, no walk.

                    if(protect_range_committed(a,b-a)) {

                        if(!VirtualProtect((void*)a,b-a,protect,&old))return -1;

                        continue;

                    }

                    for(uintptr_t at=a;at<b;) {

                        MEMORY_BASIC_INFORMATION info;

                        if(!VirtualQuery((void*)at,&info,sizeof(info)))return -1;

                        uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;

                        if(next>b)next=b;

                        if(next<=at)return -1;

                        if(info.State==MEM_COMMIT && !VirtualProtect((void*)at,next-at,protect,&old))return -1;

                        at=next;

                    }

                    continue;

                }

                /* Detroit (2026-11-11): the commit cache only knows whole 64 KiB chunks, so this
                 * committed the range again before nearly every protection change (~1 ms per
                 * heavy frame of ZwAllocateVirtualMemory, dt-class1). VirtualProtect fails
                 * without changing anything when a page is not committed: try it first.
                 * BB_PROTECT_TRY_FIRST=0: the old order. */
                static int try_first=-1;
                if(try_first<0){const char*e=getenv("BB_PROTECT_TRY_FIRST");try_first=!(e&&e[0]=='0');}
                if(try_first && VirtualProtect((void*)a,b-a,protect,&old))continue;
                if(!protect_range_committed(a,b-a) && !VirtualAlloc((void*)a,b-a,MEM_COMMIT,PAGE_READWRITE))return -1;
                if(!VirtualProtect((void*)a,b-a,protect,&old))return -1;

                continue;

            }

        }

        // GPU watches only protect ranges they actually use. Commit those

        // bytes before VirtualProtect; untouched portions retain view defaults.

        if (touch_commit && !protect_range_committed((uintptr_t)(backing_base+regions[i].phys+(a-regions[i].start)),b-a) &&

            !backing_commit(regions[i].phys+(a-regions[i].start),b-a)) return -1;

        if (!protect_section_range(a,regions[i].phys+(a-regions[i].start),b-a,protect)) return -1;

    }

    return 0;

}

/* Commit through the physical backing view, preserving every alias's own

 * protection. Only uncommitted pages belonging to this runtime are handled;

 * genuine protection faults still reach the existing GPU/guest handlers. */

static int win_access_permitted(DWORD protect, ULONG_PTR access) {

    if (protect & (PAGE_GUARD|PAGE_NOACCESS)) return 0;

    protect &= 0xff;

    if (access==1) return protect==PAGE_READWRITE || protect==PAGE_WRITECOPY ||

                         protect==PAGE_EXECUTE_READWRITE || protect==PAGE_EXECUTE_WRITECOPY;

    if (access==8) return protect==PAGE_EXECUTE || protect==PAGE_EXECUTE_READ ||

                         protect==PAGE_EXECUTE_READWRITE || protect==PAGE_EXECUTE_WRITECOPY;

    if (access!=0) return 0;

    return protect==PAGE_READONLY || protect==PAGE_READWRITE || protect==PAGE_WRITECOPY ||

           protect==PAGE_EXECUTE_READ || protect==PAGE_EXECUTE_READWRITE || protect==PAGE_EXECUTE_WRITECOPY;

}



/* bbport (BB_FRAME_STATS): host_commit_fault outcomes: stale (committed page whose current

 * protection already allows the access: retried), first-touch commits, passed on. */

uint64_t runtime_host_fault_counts[3];

/* GPU-shared backing sizing: 64 MiB chunks of the guest range whose pages were ever first-touched

 * (the renderer prints the count with its memory line). */

static uint8_t touched_chunks[(USER_MAX-USER_MIN)>>TOUCH_CHUNK_SHIFT];

uint64_t runtime_touched_chunks;

static void note_touch(uintptr_t address) {

    if (address<USER_MIN || address>=USER_MAX) return;

    uint8_t *slot=&touched_chunks[(address-USER_MIN)>>TOUCH_CHUNK_SHIFT];

    if (!__atomic_load_n(slot,__ATOMIC_RELAXED) && !__atomic_exchange_n(slot,1,__ATOMIC_RELAXED))

        __atomic_add_fetch(&runtime_touched_chunks,1,__ATOMIC_RELAXED);

}

/* bbport (BB_GPU_SHARED=1): GPU-shared guest memory. The guest's GPU (WC_GARLIC, type 3) direct

 * mapping becomes, when mapped, one host-cached Vulkan allocation mapped at the guest address

 * itself (HostRegion.mapped 3): the buffer cache binds it instead of copying it into arenas. One

 * allocation, as TLOU binds ranges of up to 512 MiB anywhere in it.

 * Xbox D3D12 refuses to import existing memory, so the renderer allocates and the mapping is

 * steered into the guest address: every other free range of 64 MiB or more is reserved for the

 * duration of the allocation (the first 16 MiB of each stay free for small allocations). A miss

 * leaves the mapping private. Not supported there: unmapping, aliases. */

typedef int (*GpuSharedAlloc)(uintptr_t base, uint64_t size);

static GpuSharedAlloc gpu_shared_alloc;

static unsigned gpu_shared_misses;

uint64_t runtime_gpu_shared_bytes;

/* The renderer registers before the guest pool exists. */

void runtime_memory_set_gpu_shared(GpuSharedAlloc alloc) {

    const char *env=getenv("BB_GPU_SHARED");

    if(env && env[0]=='1') gpu_shared_alloc=alloc;

}

static int gpu_shared_enabled(void) { return gpu_shared_alloc && private_cpu && !gpu_shared_misses; }

/* The touched 64 MiB chunks as hex indices from USER_MIN, for the memory log. */

void runtime_memory_touched_list(char *out, size_t capacity) {

    size_t used=0;out[0]=0;

    for(size_t i=0;i<sizeof(touched_chunks) && used+8<capacity;++i)

        if(__atomic_load_n(&touched_chunks[i],__ATOMIC_RELAXED))

            used+=(size_t)snprintf(out+used,capacity-used,"%zx ",i);

}

/* Copies to or from GPU-shared memory past its page protections (the GPU tracker's), as the

 * section's backing view does for other memory. Caller holds the table lock. */

static void shared_copy(unsigned char *guest, unsigned char *other, uint64_t size, int to_guest) {

    for(uint64_t done=0;done<size;) {

        MEMORY_BASIC_INFORMATION info;

        if(!VirtualQuery(guest+done,&info,sizeof(info)))win_fail("querying GPU-shared memory",(uintptr_t)(guest+done),size-done);

        uint64_t n=(uintptr_t)info.BaseAddress+info.RegionSize-(uintptr_t)(guest+done);

        if(n>size-done)n=size-done;

        const DWORD need=to_guest ? PAGE_READWRITE : PAGE_READONLY;

        DWORD old=info.Protect;

        const int open=to_guest ? win_access_permitted(old,1) : win_access_permitted(old,0);

        if(!open && !VirtualProtect(guest+done,n,need,&old))win_fail("opening GPU-shared memory",(uintptr_t)(guest+done),n);

        if(to_guest)memcpy(guest+done,other+done,n);else memcpy(other+done,guest+done,n);

        if(!open && !VirtualProtect(guest+done,n,old,&old))win_fail("restoring GPU-shared protection",(uintptr_t)(guest+done),n);

        done+=n;

    }

}

/* Reserves every free range of SHARED_CHUNK or more except [hole,+size); the free range holding

 * the hole (the guest pool's first mapping borders free space) is fenced around it. Returns the

 * count. */

static size_t fence_free_ranges(uintptr_t hole,uint64_t size,void **fences,size_t capacity) {

    const uint64_t keep=UINT64_C(16)<<20;

    size_t count=0;

    for(uintptr_t at=0x10000;at<UINT64_C(0x7ffffffe0000) && count+1<capacity;) {

        MEMORY_BASIC_INFORMATION info;

        if(!VirtualQuery((void*)at,&info,sizeof(info)))break;

        const uintptr_t base=(uintptr_t)info.BaseAddress,next=base+info.RegionSize;

        if(next<=at)break;

        if(info.State==MEM_FREE && base<=hole && hole+size<=next) {

            if(base<hole && (fences[count]=VirtualAlloc((void*)base,hole-base,MEM_RESERVE,PAGE_NOACCESS)))++count;

            if(hole+size<next && (fences[count]=VirtualAlloc((void*)(hole+size),next-hole-size,MEM_RESERVE,PAGE_NOACCESS)))++count;

        } else if(info.State==MEM_FREE && info.RegionSize>=SHARED_CHUNK) {

            const uintptr_t start=align_up(base+keep,0x10000);

            if(start<next && (fences[count]=VirtualAlloc((void*)start,next-start,MEM_RESERVE,PAGE_NOACCESS)))++count;

        }

        at=next;

    }

    return count;

}

/* For the renderer's allocation self-test (BB_GPU_SHARED_TEST=1): fences around a free hole of

 * `size` it picks (*hole_out). */

size_t runtime_memory_fence(uint64_t size,uintptr_t *hole_out,void **fences,size_t capacity) {

    void *pick=VirtualAlloc(NULL,size,MEM_RESERVE,PAGE_NOACCESS);

    if(!pick)return *hole_out=0;

    VirtualFree(pick,0,MEM_RELEASE);

    *hole_out=(uintptr_t)pick;

    return fence_free_ranges((uintptr_t)pick,size,fences,capacity);

}

void runtime_memory_unfence(void **fences,size_t count) {

    for(size_t n=0;n<count;++n)VirtualFree(fences[n],0,MEM_RELEASE);

}

/* host_map of a GPU direct range, write lock held: 1 when it is GPU-shared now. */

static int gpu_shared_map(uintptr_t address,uint64_t size,uint64_t phys,DWORD protect) {

    const size_t i=carve_host(address,address+size);

    if(regions[i].start!=address || regions[i].end!=address+size || regions[i].mapped) {

        printf("Runtime: GPU-shared memory %p (+%llu MiB): carved region %p-%p mapped=%d\n",(void*)address,

               (unsigned long long)(size>>20),(void*)regions[i].start,(void*)regions[i].end,regions[i].mapped);

        return 0;

    }

    if(!VirtualFree((void*)address,0,MEM_RELEASE))win_fail("releasing the placeholder for GPU-shared memory",address,size);

    static void *fences[1024];

    const size_t fenced=fence_free_ranges(address,size,fences,1024);

    const int placed=gpu_shared_alloc(address,size);

    runtime_memory_unfence(fences,fenced);

    if(!placed) {

        if(!VirtualAlloc2(NULL,(void*)address,size,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))

            win_fail("restoring the placeholder after GPU-shared memory",address,size);

        printf("Runtime: GPU-shared memory %p (+%llu MiB) not placed (%zu ranges fenced); it stays private\n",

               (void*)address,(unsigned long long)(size>>20),fenced);

        ++gpu_shared_misses;

        return 0;

    }

    DWORD old;

    if(protect!=PAGE_READWRITE && !VirtualProtect((void*)address,size,protect,&old))

        win_fail("protecting GPU-shared memory",address,size);

    regions[i]=(HostRegion){address,address+size,phys,3,protect};

    __atomic_add_fetch(&runtime_gpu_shared_bytes,size,__ATOMIC_RELAXED);

    printf("Runtime: GPU-shared memory %p (+%llu MiB), phys 0x%llx\n",(void*)address,

           (unsigned long long)(size>>20),(unsigned long long)phys);

    return 1;

}

/* BB_GPU_SHARED_SPANS: the 64 MiB chunks of the GPU mapping (hex, counted from its start; "a-b"

 * ranges, comma separated) shared when it is mapped. Sharing commits a span whole, so the default

 * holds what TLOU touches of its GPU memory on the courtyard save plus the range of its 512 MiB

 * binding (chunks 9-11): ~1.2 GB for ~0.8 GB touched. */

static uint8_t gpu_shared_span[256];

static void gpu_shared_parse_spans(void) {

    const char *spans=getenv("BB_GPU_SHARED_SPANS");

    if(!spans || !spans[0])spans="0,9-11,14-19,24-25,28";

    for(const char *at=spans;*at;) {

        char *end;

        const unsigned long first=strtoul(at,&end,16);

        unsigned long last=first;

        if(end==at)break;

        if(*end=='-')last=strtoul(end+1,&end,16);

        for(unsigned long n=first;n<=last && n<sizeof(gpu_shared_span);++n)gpu_shared_span[n]=1;

        if(*end!=',')break;

        at=end+1;

    }

    printf("Runtime: GPU-shared guest memory on, chunks %s of the GPU mapping\n",spans);

}

static int gpu_shared_span_at(uint64_t offset) {

    const uint64_t n=offset>>TOUCH_CHUNK_SHIFT;

    return n<sizeof(gpu_shared_span) && gpu_shared_span[n];

}

/* host_map of the guest's GPU direct mapping: its span chunks GPU-shared, the rest as usual. */

static int gpu_shared_host_map(uintptr_t address,uint64_t size,int prot,uint64_t phys,int cpu_only) {

    static int parsed,splitting;

    if(splitting)return 0;

    if(!parsed){parsed=1;gpu_shared_parse_spans();}

    printf("Runtime: GPU mapping %p (+%llu MiB) split for sharing\n",(void*)address,(unsigned long long)(size>>20));

    splitting=1;

    for(uint64_t offset=0;offset<size;) {

        const int by_range=gpu_shared_range_count>0;

        const int span=by_range ? gpu_shared_in_ranges(address+offset,SHARED_CHUNK) : gpu_shared_span_at(offset);

        uint64_t next=offset+SHARED_CHUNK;

        while(next<size && (by_range ? gpu_shared_in_ranges(address+next,SHARED_CHUNK) : gpu_shared_span_at(next))==span)next+=SHARED_CHUNK;

        if(next>size)next=size;

        const uintptr_t at=address+offset;

        if(!(span && !gpu_shared_misses && gpu_shared_map(at,next-offset,phys+offset,win_prot(prot))) &&

           host_map(at,next-offset,prot,0,phys+offset,cpu_only)) {

            printf("Runtime: GPU mapping part %p (+%llu MiB) failed\n",(void*)at,(unsigned long long)((next-offset)>>20));

            splitting=0;return -1;

        }

        offset=next;

    }

    splitting=0;

    return 1;

}

static LONG CALLBACK host_commit_fault(EXCEPTION_POINTERS *exception) {

    const EXCEPTION_RECORD *record=exception->ExceptionRecord;

    if (!touch_commit || !backing_base || record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || record->NumberParameters<2)

        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t address=(uintptr_t)record->ExceptionInformation[1];

    const uintptr_t trimming=__atomic_load_n(&zero_trim_guard,__ATOMIC_ACQUIRE);

    if(record->ExceptionInformation[0]==1 && trimming && address>=trimming && address-trimming<PAGE) {

        // A zero-page scan temporarily revoked writes. Wait for its decision,

        // then retry the original write (or its ordinary first-touch fault).

        read_lock();read_unlock();return EXCEPTION_CONTINUE_EXECUTION;

    }

    MEMORY_BASIC_INFORMATION info;

    /* TLOU debug (BB_MEM_TRACE): the time its first VirtualQuery takes, per fault. */

    const uint64_t query_began=lock_trace_on() ? upload_clock() : 0;

    if (!VirtualQuery((void *)address,&info,sizeof(info)))

        return EXCEPTION_CONTINUE_SEARCH;

    if(query_began)__atomic_add_fetch(&host_fault_query_ticks,upload_clock()-query_began,__ATOMIC_RELAXED);

    if(info.State!=MEM_RESERVE) {

        /* Preserve TLOU's stale in-flight fault handling, checking ownership

         * and current protection under the metadata lock. Real protection

         * faults must still reach the existing GPU/guest handlers. */

        read_lock();

        const size_t owned_index=region_index(address);

        const int owned=(address>=(uintptr_t)backing_base &&

                         address-(uintptr_t)backing_base<POOL_SIZE+FLEX_SPAN) ||

                        (owned_index<region_count && regions[owned_index].start<=address && regions[owned_index].mapped);

        MEMORY_BASIC_INFORMATION current;

        const int handled=owned && VirtualQuery((void*)address,&current,sizeof(current)) &&

                          current.State==MEM_COMMIT && win_access_permitted(current.Protect,record->ExceptionInformation[0]);

        read_unlock();

        __atomic_add_fetch(&runtime_host_fault_counts[handled?0:2],1,__ATOMIC_RELAXED);

        return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;

    }

    __atomic_add_fetch(&runtime_host_fault_counts[1],1,__ATOMIC_RELAXED);

    uint64_t physical=UINT64_MAX;

    read_lock();

    if (address>=(uintptr_t)backing_base && address-(uintptr_t)backing_base<POOL_SIZE+FLEX_SPAN)

        physical=address-(uintptr_t)backing_base;

    else {

        size_t i=region_index(address);

        if (i<region_count && regions[i].start<=address && regions[i].mapped)

            physical=regions[i].phys+(address-regions[i].start);

    }

    int handled=0;

    size_t private_index=region_index(address);

    if(private_cpu && private_index<region_count && regions[private_index].start<=address && (regions[private_index].mapped==2 || regions[private_index].mapped==4)) {

        const uintptr_t page=address&~(uintptr_t)(private_touch_page-1);

        /* Committed meanwhile (a shared view turned private while this thread waited for the table,

         * 2026-11-02): retried with the protections it got, not committed again with the initial

         * ones (that would drop a GPU write watch). */

        MEMORY_BASIC_INFORMATION now;

        if (VirtualQuery((void*)address,&now,sizeof(now)) && now.State==MEM_COMMIT) { read_unlock(); return EXCEPTION_CONTINUE_EXECUTION; }

        handled=commit_with_room((void*)page,private_touch_page,regions[private_index].initial_protect,"private CPU first-touch commit");

        if(!handled)win_fail("private CPU first-touch commit",page,private_touch_page);

        note_touch(address);

        read_unlock();return EXCEPTION_CONTINUE_EXECUTION;

    }

    if (physical!=UINT64_MAX) {

        TouchFault *f=&touch_history[touch_history_index++%8];

        CONTEXT *context=exception->ContextRecord;

        *f=(TouchFault){.rip=context->Rip,.address=address,.rsp=context->Rsp,.r11=context->R11,.r9=context->R9};

        SIZE_T got=0;

        ReadProcessMemory(GetCurrentProcess(),(void*)(context->Rsp-128),f->stack,sizeof(f->stack),&got);

        const uint64_t commit_size=touch_cpu_page<PAGE &&

            (touch_all_host_pages || (physical<POOL_SIZE && block_type(physical)==0)) ? touch_cpu_page : PAGE;

        const uint64_t page=physical&~(commit_size-1);

        handled=commit_with_room(backing_base+page,commit_size,PAGE_READWRITE,"first-touch commit");

        if (!handled) {

            const DWORD error=GetLastError();

            APP_MEMORY_INFORMATION app={0};

            GetProcessInformation(GetCurrentProcess(),ProcessAppMemoryInfo,&app,sizeof(app));

            MEMORYSTATUSEX system={.dwLength=sizeof(system)};

            GlobalMemoryStatusEx(&system);

            fprintf(stderr,"STOP: first-touch commit failed: address=%p physical=0x%llx page=0x%llx error=%lu app_total=%llu app_available=%llu global_available=%llu global_commit_available=%llu\n",

                    (void*)address,(unsigned long long)physical,(unsigned long long)page,error,

                    (unsigned long long)app.TotalCommitUsage,(unsigned long long)app.AvailableCommit,

                    (unsigned long long)system.ullAvailPhys,(unsigned long long)system.ullAvailPageFile);

            /* A genuine exhausted commit cannot be resolved by the GPU fault

             * handler. Stop once rather than repeatedly queueing readbacks. */

            TerminateProcess(GetCurrentProcess(),21);

        }

        if (handled) { __atomic_add_fetch(&touch_faults,1,__ATOMIC_RELAXED); note_touch(address); }

    }

    read_unlock();

    return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;

}

/* Released pages read as zero when reused, like the memfd's punched holes. */

static void host_discard(uint64_t phys, uint64_t size) {

    if(small_store) for(uint64_t at=phys&~(SMALL_CHUNK-1);at<phys+size && at/SMALL_CHUNK<small_store_count;at+=SMALL_CHUNK) {

        unsigned char **slot=&small_store[at/SMALL_CHUNK];

        if(*slot && at>=phys && at+SMALL_CHUNK<=phys+size) { VirtualFree(*slot,0,MEM_RELEASE);*slot=NULL; }

    }

    if(private_cpu) for(uint64_t at=phys;at<phys+size;) {

        const uint64_t base=at&~(PRIVATE_CHUNK-1),offset=at-base;

        uint64_t n=PRIVATE_CHUNK-offset;if(n>phys+size-at)n=phys+size-at;

        unsigned char **slot=&private_store[base/PRIVATE_CHUNK];

        if(*slot) {

            if(!offset && n==PRIVATE_CHUNK) { VirtualFree(*slot,0,MEM_RELEASE);*slot=NULL; }

            else if(!VirtualFree(*slot+offset,n,MEM_DECOMMIT))win_fail("discarding unmapped CPU pages",(uintptr_t)(*slot+offset),n);

        }

        at+=n;

    }

    if (file_backed) {

        for (uint64_t done=0;done<size;) {

            const uint64_t at=phys+done,offset=at%FILE_STORE_BYTES;

            uint64_t n=FILE_STORE_BYTES-offset;if(n>size-done)n=size-done;

            FILE_ZERO_DATA_INFORMATION zero={.FileOffset.QuadPart=offset,.BeyondFinalZero.QuadPart=offset+n};

            DWORD returned=0;

            if (!DeviceIoControl(backing_files[at/FILE_STORE_BYTES],FSCTL_SET_ZERO_DATA,&zero,sizeof(zero),NULL,0,&returned,NULL))

                win_fail("zeroing released file backing",(uintptr_t)(backing_base+at),n);

            done+=n;

        }

        return;

    }

    if (!demand_commit) { memset(backing_base+phys,0,size); return; }

    /* SEC_RESERVE pages which were never mapped are already zero and must not

     * be touched. Committed shared pages cannot be decommitted with VirtualFree;

     * clear only those pages, preserving the existing zero-on-reuse contract. */

    uintptr_t at=(uintptr_t)(backing_base+phys), end=at+size;

    while (at<end) {

        MEMORY_BASIC_INFORMATION info;

        if (!VirtualQuery((void *)at,&info,sizeof(info))) win_fail("querying guest backing",at,end-at);

        uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;

        if (next>end) next=end;

        if (next<=at) win_fail("invalid guest backing extent",at,end-at);

        if (info.State==MEM_COMMIT) memset((void *)at,0,next-at);

        at=next;

    }

}



/* ---- Giving shared commit back (sectioned pool, 2026-11-02) ----

 * A section can be made again empty once no guest view uses it and what it holds that must stay

 * (memory nobody maps now) is kept off-address in private_store, as for an unmapped private chunk.

 * A shared view whose physical range nobody else maps becomes private chunks first (the alias that

 * made it shared is gone): same address, same page protections, contents copied from the backing

 * view. Views still aliased, not chunk-aligned, or with no-access pages (GPU read watches; private

 * chunks cannot hold them) keep their section. Under the table's write lock. */

typedef struct { uint64_t phys, end; uintptr_t start; int mapped; } PhysUse;

static int phys_use_order(const void *a, const void *b) {

    const PhysUse *x=a, *y=b;

    return x->phys<y->phys ? -1 : x->phys>y->phys;

}

static uint64_t backing_committed(uint64_t phys, uint64_t size) {

    uint64_t sum=0;

    uintptr_t at=(uintptr_t)backing_base+phys, end=at+size;

    while (at<end) {

        MEMORY_BASIC_INFORMATION m;

        if (!VirtualQuery((void*)at,&m,sizeof(m))) break;

        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;

        if (next>end) next=end;

        if (next<=at) break;

        if (m.State==MEM_COMMIT) sum+=next-at;

        at=next;

    }

    return sum;

}

/* Whether the shared view at regions[i] can become private chunks: 1 with its per-4 KiB-page

 * protections in *out (0: not committed), -1 not chunk-aligned / no-access from the start, -2 a

 * no-access or guard page (a GPU read watch: GPU data is written in through the backing view). */

static int shared_scan(size_t i, DWORD **out) {

    const HostRegion r=regions[i];

    *out=NULL;

    if (r.mapped!=1 || (r.start|r.end|r.phys)%PRIVATE_CHUNK || r.initial_protect==PAGE_NOACCESS) return -1;

    DWORD *prot=calloc((size_t)((r.end-r.start)/4096),sizeof(DWORD));

    if (!prot) return -1;

    for (uintptr_t at=r.start; at<r.end;) {

        MEMORY_BASIC_INFORMATION m;

        if (!VirtualQuery((void*)at,&m,sizeof(m))) { free(prot); return -1; }

        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;

        if (next>r.end) next=r.end;

        if (m.State==MEM_COMMIT) {

            if (m.Protect==PAGE_NOACCESS || (m.Protect&PAGE_GUARD)) { free(prot); return -2; }

            for (uintptr_t p=at; p<next; p+=4096) prot[(p-r.start)/4096]=m.Protect;

        }

        at=next;

    }

    *out=prot;

    return 1;

}

/* The shared view at regions[i] as private chunks: same address, the protections shared_scan

 * found (prot, freed here), contents copied from the backing view. */

static void shared_demote(size_t i, DWORD *prot) {

    const HostRegion r=regions[i];

    const uint64_t size=r.end-r.start;

    size_t j=carve_host(r.start,r.end);

    for (uint64_t done=0; done<size; done+=PRIVATE_CHUNK, ++j) {

        if (done+PRIVATE_CHUNK<size) {

            if (!VirtualFree((void*)(r.start+done),PRIVATE_CHUNK,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))

                win_fail("splitting a shared view turned private",r.start+done,PRIVATE_CHUNK);

            regions[j].end=r.start+done+PRIVATE_CHUNK;

            region_insert(j+1,(HostRegion){r.start+done+PRIVATE_CHUNK,r.end,0,0,0});

        }

        if (!VirtualAlloc2(NULL,(void*)(r.start+done),PRIVATE_CHUNK,MEM_RESERVE|MEM_REPLACE_PLACEHOLDER,r.initial_protect,NULL,0))

            win_fail("reserving a shared view turned private",r.start+done,PRIVATE_CHUNK);

        regions[j]=(HostRegion){r.start+done,r.start+done+PRIVATE_CHUNK,r.phys+done,2,r.initial_protect};

        for (uint64_t p=0; p<PRIVATE_CHUNK;) {

            const DWORD want=prot[(done+p)/4096];

            if (!want) { p+=4096; continue; }

            uint64_t q=p+4096;

            while (q<PRIVATE_CHUNK && prot[(done+q)/4096]==want) q+=4096;

            unsigned char *dest=(unsigned char*)(r.start+done+p);

            if (!commit_with_room(dest,(SIZE_T)(q-p),PAGE_READWRITE,"committing a shared view turned private"))

                win_fail("committing a shared view turned private",(uintptr_t)dest,q-p);

            memcpy(dest,backing_base+r.phys+done+p,q-p);

            DWORD old;

            if (want!=PAGE_READWRITE && !VirtualProtect(dest,(SIZE_T)(q-p),want,&old))

                win_fail("protecting a shared view turned private",(uintptr_t)dest,q-p);

            p=q;

        }

    }

    free(prot);

}

/* What a chunk of physical memory nobody maps holds, off-address (private_store), unless kept

 * there already (the newer copy) or all zero (released memory is zeroed: host_discard). */

static void shared_keep_unmapped(uint64_t phys) {

    unsigned char **slot=&private_store[phys/PRIVATE_CHUNK];

    if (*slot) return;

    unsigned char *source=backing_base+phys;

    for (uint64_t at=0; at<PRIVATE_CHUNK;) { /* one query per run (a VirtualQuery cost up to ~88 us on the Xbox) */

        MEMORY_BASIC_INFORMATION m;

        if (!VirtualQuery(source+at,&m,sizeof(m))) break;

        uint64_t next=(uint64_t)((unsigned char*)m.BaseAddress+m.RegionSize-source);

        if (next>PRIVATE_CHUNK) next=PRIVATE_CHUNK;

        if (next<=at) break;

        if (m.State==MEM_COMMIT) for (uint64_t p=at; p<next; p+=4096) {

            const uint64_t *w=(const uint64_t*)(source+p);

            int zero=1;

            for (size_t n=0; n<4096/8 && zero; ++n) zero=!w[n];

            if (zero) continue;

            if (!*slot && !(*slot=VirtualAlloc(NULL,PRIVATE_CHUNK,MEM_RESERVE,PAGE_READWRITE)))

                win_fail("reserving unmapped CPU storage",phys,PRIVATE_CHUNK);

            if (!commit_with_room(*slot+p,4096,PAGE_READWRITE,"keeping unmapped shared memory"))

                win_fail("keeping unmapped shared memory",phys+p,4096);

            memcpy(*slot+p,source+p,4096);

        }

        at=next;

    }

}

static void shared_section_renew(size_t k) {

    void *at=backing_base+k*pool_section_bytes;

    if (!UnmapViewOfFile2(GetCurrentProcess(),at,MEM_PRESERVE_PLACEHOLDER)) win_fail("unmapping a pool section",k*pool_section_bytes,pool_section_bytes);

    CloseHandle(pool_sections[k]);

    pool_sections[k]=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_RESERVE,pool_section_bytes,NULL,NULL,0);

    if (!pool_sections[k]) win_fail("making a pool section again",k*pool_section_bytes,pool_section_bytes);

    if (!MapViewOfFile3(pool_sections[k],GetCurrentProcess(),at,0,pool_section_bytes,MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0))

        win_fail("mapping a pool section again",k*pool_section_bytes,pool_section_bytes);

}

static LONG64 shared_renewed, shared_views_private, shared_kept_back;

static uint64_t shared_reclaim_min=UINT64_C(4)<<20; /* a section is worth emptying from (BB_SHARED_RECLAIM_MIN_KB) */

/* Up to `want` bytes of shared commit back. The table is held meanwhile, and the game waits for it:

 * one section at least, more only within budget_ms (2026-11-03 Xbox: 5-6 sections a pass held it

 * 234-578 ms, visible hitches; one section costs ~50 ms). */

static int shared_reclaim(uint64_t want, uint64_t shared_now, ULONGLONG budget_ms) {

    const ULONGLONG began=GetTickCount64();

    typedef struct { uint64_t committed; size_t k; } Candidate;

    Candidate *candidates=malloc(pool_section_count*sizeof(*candidates));

    if (!candidates) return 0;

    /* Sections by committed bytes, measured before the table is taken (VirtualQuery walks: slow

     * on the Xbox; an estimate is enough), most committed first. */

    for (size_t k=0; k<pool_section_count; ++k) candidates[k]=(Candidate){backing_committed(k*pool_section_bytes,pool_section_bytes),k};

    for (size_t a=0; a<32 && a<pool_section_count; ++a)

        for (size_t b=a+1; b<pool_section_count; ++b)

            if (candidates[b].committed>candidates[a].committed) { Candidate t=candidates[a]; candidates[a]=candidates[b]; candidates[b]=t; }

    const ULONGLONG locked=GetTickCount64();

    write_lock();

    PhysUse *uses=malloc(region_count*sizeof(*uses));

    if (!uses) { free(candidates); write_unlock(); return 0; }

    size_t n=0;

    for (size_t i=0; i<region_count; ++i)

        if (regions[i].mapped) uses[n++]=(PhysUse){regions[i].phys,regions[i].phys+(regions[i].end-regions[i].start),regions[i].start,regions[i].mapped};

    qsort(uses,n,sizeof(*uses),phys_use_order);

    uint64_t freed=0; unsigned renewed=0, kept=0, views=0, why[5]={0}; /* kept: GPU-shared, aliased, unaligned, watched, gone */

    for (size_t c=0; c<pool_section_count && c<32 && freed<want && (!renewed || GetTickCount64()-locked<budget_ms); ++c) {

        if (candidates[c].committed<shared_reclaim_min) break;

        const size_t k=candidates[c].k;

        const uint64_t lo=k*pool_section_bytes, hi=lo+pool_section_bytes;

        /* the shared views on this section, and the span of physical memory they cover */

        uint64_t span_lo=lo, span_hi=hi; int ok=1;

        for (size_t u=0; u<n && uses[u].phys<hi; ++u) {

            if (uses[u].end<=lo || uses[u].mapped!=1) continue;

            if (uses[u].phys<span_lo) span_lo=uses[u].phys;

            if (uses[u].end>span_hi) span_hi=uses[u].end;

        }

        span_lo&=~(PRIVATE_CHUNK-1);

        /* how many mappings cover each chunk of that span: a view on a chunk mapped twice stays */

        const size_t chunks=(size_t)((span_hi-span_lo+PRIVATE_CHUNK-1)/PRIVATE_CHUNK);

        uint8_t *cover=calloc(chunks,1);

        if (!cover) break;

        for (size_t u=0; u<n && uses[u].phys<span_hi; ++u) {

            if (uses[u].end<=span_lo) continue;

            const uint64_t a=uses[u].phys>span_lo ? uses[u].phys : span_lo, b=uses[u].end<span_hi ? uses[u].end : span_hi;

            for (uint64_t x=(a-span_lo)/PRIVATE_CHUNK; x<(b-span_lo+PRIVATE_CHUNK-1)/PRIVATE_CHUNK; ++x) if (cover[x]<255) ++cover[x];

        }

        for (size_t u=0; u<n && uses[u].phys<hi && ok; ++u) {

            if (uses[u].end<=lo) continue;

            if (uses[u].mapped==1 && ((uses[u].phys|uses[u].start|uses[u].end)%PRIVATE_CHUNK)) { ok=0; ++why[2]; break; }

            if (uses[u].mapped==3) { ok=0; ++why[0]; break; }

            if (uses[u].mapped!=1) continue;

            for (uint64_t x=(uses[u].phys-span_lo)/PRIVATE_CHUNK; x<(uses[u].end-span_lo+PRIVATE_CHUNK-1)/PRIVATE_CHUNK; ++x)

                if (cover[x]>1) { ok=0; ++why[1]; break; }

        }

        /* every view on it scanned first: a section that has to stay is left as it was */

        size_t nviews=0;

        for (size_t u=0; u<n && uses[u].phys<hi; ++u) if (uses[u].end>lo && uses[u].mapped==1) ++nviews;

        DWORD **prots=ok && nviews ? calloc(nviews,sizeof(*prots)) : NULL;

        if (nviews && !prots) ok=0;

        size_t v=0;

        if (ok) for (size_t u=0; u<n && uses[u].phys<hi && ok; ++u) {

            if (uses[u].end<=lo || uses[u].mapped!=1) continue;

            const size_t i=region_index(uses[u].start);

            if (i>=region_count || regions[i].start!=uses[u].start || regions[i].mapped!=1) { ok=0; ++why[4]; break; }

            const int scanned=shared_scan(i,&prots[v]);

            if (scanned<0) { ok=0; ++why[scanned==-2 ? 3 : 2]; break; }

            ++v;

        }

        if (ok) {

            v=0;

            for (size_t u=0; u<n && uses[u].phys<hi; ++u) {

                if (uses[u].end<=lo || uses[u].mapped!=1) continue;

                shared_demote(region_index(uses[u].start),prots[v]); prots[v++]=NULL;

                uses[u].mapped=2; ++views;

            }

        }

        if (prots) { for (size_t x=0; x<nviews; ++x) free(prots[x]); free(prots); }

        if (ok) {

            for (uint64_t at=lo; at<hi; at+=PRIVATE_CHUNK)

                if (!cover[(at-span_lo)/PRIVATE_CHUNK]) shared_keep_unmapped(at);

            shared_section_renew(k);

            freed+=candidates[c].committed; ++renewed;

        } else ++kept;

        free(cover);

    }

    write_unlock();

    const ULONGLONG unlocked=GetTickCount64();

    free(uses); free(candidates);

    shared_renewed+=renewed; shared_views_private+=views; shared_kept_back+=kept;

    printf("Shared reclaim: shared %llu MiB: %u sections emptied (%llu MiB back), %u views turned private, %u sections kept (GPU-shared %u, aliased %u, unaligned %u, watched %u, changed %u); %llu ms measuring, %llu ms with the table held (in all %lld emptied, %lld views, %lld kept)\n",

           (unsigned long long)(shared_now>>20),renewed,(unsigned long long)(freed>>20),views,kept,why[0],why[1],why[2],why[3],why[4],(unsigned long long)(locked-began),

           (unsigned long long)(unlocked-locked),(long long)shared_renewed,(long long)shared_views_private,(long long)shared_kept_back);

    fflush(stdout);

    return (int)renewed;

}

/* Above BB_SHARED_RECLAIM_MB of shared commit (default 640; the cap is ~950), back to 128 below. */

static DWORD WINAPI shared_reclaim_thread(void *unused) {

    (void)unused;

    const char *e=getenv("BB_SHARED_RECLAIM_MB");

    const char *min=getenv("BB_SHARED_RECLAIM_MIN_KB");

    if (min) shared_reclaim_min=strtoull(min,NULL,10)<<10;

    const uint64_t high=(e ? strtoull(e,NULL,10) : 640)<<20, low=high>(UINT64_C(256)<<20) ? high-(UINT64_C(128)<<20) : high/2;

    printf("Shared reclaim: on above %llu MiB of shared commit (Xbox cap ~950 MiB)\n",(unsigned long long)(high>>20));

    fflush(stdout);

    for (;;) {

        Sleep(1000);

        APP_MEMORY_INFORMATION app={0};

        if (!GetProcessInformation(GetCurrentProcess(),ProcessAppMemoryInfo,&app,sizeof(app))) continue;

        const uint64_t shared=app.TotalCommitUsage>app.PrivateCommitUsage ? app.TotalCommitUsage-app.PrivateCommitUsage : 0;

        /* near the cap a pass may take longer, and passes follow each other sooner */

        const int urgent=shared>(UINT64_C(850)<<20);

        if (shared>high && shared_reclaim(shared-low,shared,urgent ? 200 : 30)) Sleep(urgent ? 50 : 250);

    }

}

/* Direct memory occupies [0,POOL_SIZE) of the section, flexible memory [POOL_SIZE,+FLEX_SPAN). */

static int pool(void) {

    if (backing_base) return 0;

    runtime_memory_reserve();

    uint64_t bytes=POOL_SIZE+FLEX_SPAN;

    const char *private_mode=getenv("BB_UWP_PRIVATE_CPU");

    private_cpu=private_mode && private_mode[0]=='1';

    const char *private_gpu_mode=getenv("BB_UWP_PRIVATE_GPU");

    private_gpu=private_cpu && private_gpu_mode && private_gpu_mode[0]=='1';

    const char *small_touch=getenv("BB_UWP_HOST_PAGE_TOUCH");

    if(private_cpu && small_touch && small_touch[0]=='1') {

        SYSTEM_INFO info;GetNativeSystemInfo(&info);

        if(info.dwPageSize && info.dwPageSize<=PAGE && !(info.dwPageSize&(info.dwPageSize-1)))

            private_touch_page=info.dwPageSize;

    }

    printf("Runtime: private first-touch granularity=%llu bytes; guest mappings remain 16384 bytes\n",(unsigned long long)private_touch_page);

    if(private_cpu) {

        private_store=calloc((bytes+PRIVATE_CHUNK-1)/PRIVATE_CHUNK,sizeof(*private_store));

        if(!private_store)return -1;

        {
            const char *e=getenv("BB_SMALL_PRIVATE");
            small_private=!(e && e[0]=='0');
            small_store_count=(bytes+SMALL_CHUNK-1)/SMALL_CHUNK;
            small_store=small_private ? calloc(small_store_count,sizeof(*small_store)) : NULL;
            if(small_private && !small_store) return -1;
            printf("Runtime: 16 KiB-aligned guest mappings %s (BB_SMALL_PRIVATE)\n",small_private ? "use private 16 KiB pieces" : "use shared backing");
        }

        printf("Runtime: %s mappings use private 64 KiB chunks; physical aliases and GPU writeback promote to shared backing\n",private_gpu ? "single-mapped CPU/GPU" : "CPU-only");

    }

    const char *lazy=getenv("BB_UWP_DEMAND_COMMIT");

    demand_commit=lazy && lazy[0]=='1';

    const char *touch=getenv("BB_UWP_TOUCH_COMMIT");

    touch_commit=demand_commit && touch && touch[0]=='1';

    const char *cpu_min=getenv("BB_UWP_TOUCH_CPU_MIN_MB");

    touch_cpu_min_bytes=touch_commit && cpu_min ? strtoull(cpu_min,NULL,10)*1024*1024 : 0;

    const char *host_commit=getenv("BB_UWP_HOST_COMMIT_TRIAL");

    const char *cpu_host_pages=getenv("BB_UWP_CPU_HOST_PAGE_TOUCH");

    touch_flexible=touch_commit && host_commit && host_commit[0]=='1' && !host_commit[1];

    touch_all_host_pages=touch_flexible;

    if(touch_commit && ((cpu_host_pages && cpu_host_pages[0]=='1') || touch_all_host_pages)) {

        SYSTEM_INFO info;GetNativeSystemInfo(&info);

        if(info.dwPageSize && info.dwPageSize<=PAGE && !(info.dwPageSize&(info.dwPageSize-1))) {

            touch_cpu_page=info.dwPageSize;

            if(private_cpu)private_touch_page=info.dwPageSize;

        }

    }

    {   /* Spider-Man port (2026-11-05): BB_COMMIT_PROBE=1 -- how far section (shared) and private commit go
           here (the Xbox refused section commits at ~926 MB shared with 1.2 GB "available"). */
        const char *probe=getenv("BB_COMMIT_PROBE");
        if (probe && probe[0]=='1') {
            const uint64_t step=(uint64_t)64<<20, max=(uint64_t)4<<30;
            HANDLE section=CreateFileMappingFromApp(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE|SEC_RESERVE,max,NULL);
            uint64_t shared_ok=0, private_ok=0; DWORD shared_err=0, private_err=0;
            unsigned char *view=section ? MapViewOfFileFromApp(section,FILE_MAP_WRITE,0,max) : NULL;
            if (view) {
                for (; shared_ok<max; shared_ok+=step) {
                    if (!VirtualAlloc(view+shared_ok,step,MEM_COMMIT,PAGE_READWRITE)) { shared_err=GetLastError(); break; }
                }
            } else shared_err=GetLastError();
            APP_MEMORY_INFORMATION app={0};
            GetProcessInformation(GetCurrentProcess(),ProcessAppMemoryInfo,&app,sizeof(app));
            printf("Commit probe: section commit stopped at %llu MiB (error %lu); app total %llu private %llu available %llu MiB\n",
                   (unsigned long long)(shared_ok>>20),shared_err,(unsigned long long)(app.TotalCommitUsage>>20),
                   (unsigned long long)(app.PrivateCommitUsage>>20),(unsigned long long)(app.AvailableCommit>>20));
            if (view) UnmapViewOfFile(view);
            if (section) CloseHandle(section);
            unsigned char *priv=VirtualAlloc(NULL,max,MEM_RESERVE,PAGE_READWRITE);
            if (priv) {
                for (; private_ok<max; private_ok+=step) {
                    if (!VirtualAlloc(priv+private_ok,step,MEM_COMMIT,PAGE_READWRITE)) { private_err=GetLastError(); break; }
                }
                GetProcessInformation(GetCurrentProcess(),ProcessAppMemoryInfo,&app,sizeof(app));
                VirtualFree(priv,0,MEM_RELEASE);
            } else private_err=GetLastError();
            printf("Commit probe: private commit stopped at %llu MiB (error %lu); app total then %llu MiB\n",
                   (unsigned long long)(private_ok>>20),private_err,(unsigned long long)(app.TotalCommitUsage>>20));
            fflush(stdout);
        }
    }
    printf("TLOU memory backend trial: private_cpu=%d private_gpu=%d shared_touch=%llu private_touch=%llu all_types=%d flexible=%d\n",

        private_cpu,private_gpu,(unsigned long long)touch_cpu_page,(unsigned long long)private_touch_page,

        touch_all_host_pages,touch_flexible);



    const char *file_path=getenv("BB_UWP_FILE_BACKING");

    if (file_path && file_path[0]) {

        CREATEFILE2_EXTENDED_PARAMETERS options={.dwSize=sizeof(options),.dwFileAttributes=FILE_ATTRIBUTE_TEMPORARY,

                                                  .dwFileFlags=FILE_FLAG_DELETE_ON_CLOSE};

        if ((bytes+FILE_STORE_BYTES-1)/FILE_STORE_BYTES>64) return -1;

        for (uint64_t at=0;at<bytes;at+=FILE_STORE_BYTES) {

            const unsigned index=(unsigned)(at/FILE_STORE_BYTES);

            char path[32768];wchar_t filename[32768];

            snprintf(path,sizeof(path),"%s.%02u",file_path,index);

            if (!MultiByteToWideChar(CP_UTF8,0,path,-1,filename,32768)) return -1;

            backing_files[index]=CreateFile2(filename,GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,0,CREATE_ALWAYS,&options);

            if (backing_files[index]==INVALID_HANDLE_VALUE) win_fail("creating private file backing",at,bytes-at);

            DWORD returned=0;

            if (!DeviceIoControl(backing_files[index],FSCTL_SET_SPARSE,NULL,0,NULL,0,&returned,NULL))

                win_fail("marking private backing sparse",at,bytes-at);

            LARGE_INTEGER length={.QuadPart=bytes-at<FILE_STORE_BYTES ? bytes-at : FILE_STORE_BYTES};

            if (!SetFilePointerEx(backing_files[index],length,NULL,FILE_BEGIN) || !SetEndOfFile(backing_files[index]))

                win_fail("sizing private file backing",at,length.QuadPart);

            file_sections[index]=CreateFileMappingFromApp(backing_files[index],NULL,PAGE_EXECUTE_READWRITE,length.QuadPart,NULL);

            if (!file_sections[index]) win_fail("creating private file section",at,length.QuadPart);

        }

        backing_base=VirtualAlloc2(NULL,NULL,bytes,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0);

        if (!backing_base) win_fail("reserving file backing views",0,bytes);

        file_backed=1; demand_commit=touch_commit=0;

        map_section_range((uintptr_t)backing_base,0,bytes,PAGE_READWRITE);

        printf("Runtime: file-backed guest pool (%llu MiB), 512 MiB files and 128 MiB views preserve physical aliases; mapped-file residency must be measured separately\n",

               (unsigned long long)(bytes>>20));

        return 0;

    }

    const char *section_mb=getenv("BB_POOL_SECTION_MB");

    const uint64_t sectioned=section_mb ? strtoull(section_mb,NULL,10) : (private_cpu && demand_commit ? 32 : 0);

    if (sectioned && demand_commit && !(sectioned&(sectioned-1))) {

        pool_section_bytes=sectioned<<20;

        pool_section_count=(size_t)((bytes+pool_section_bytes-1)/pool_section_bytes);

        bytes=pool_section_count*pool_section_bytes;

        pool_sections=calloc(pool_section_count,sizeof(*pool_sections));

        for (size_t k=0;pool_sections && k<pool_section_count;++k) {

            pool_sections[k]=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_RESERVE,pool_section_bytes,NULL,NULL,0);

            if (!pool_sections[k]) win_fail("creating a guest pool section",k*pool_section_bytes,pool_section_bytes);

        }

        backing_base=VirtualAlloc2(NULL,NULL,bytes,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0);

        if (!backing_base) win_fail("reserving the guest pool views",0,bytes);

        map_section_range((uintptr_t)backing_base,0,bytes,PAGE_READWRITE);

        printf("Runtime: guest pool in %zu sections of %llu MiB (a section no view uses gives its shared commit back)\n",

               pool_section_count,(unsigned long long)sectioned);

        const char *reclaim=getenv("BB_SHARED_RECLAIM_MB");

        if (!(reclaim && reclaim[0]=='0' && !reclaim[1])) {

            HANDLE thread=CreateThread(NULL,0,shared_reclaim_thread,NULL,0,NULL);

            if (thread) CloseHandle(thread);

        }

    } else {

        section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,demand_commit ? SEC_RESERVE : SEC_COMMIT,bytes,NULL,NULL,0);

        if (!section) { fprintf(stderr,"Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",bytes>>20,GetLastError()); return -1; }

        backing_base=MapViewOfFile3(section,GetCurrentProcess(),NULL,0,bytes,0,PAGE_READWRITE,NULL,0);

        if (!backing_base) { CloseHandle(section); section=NULL; }

    }

    if (backing_base && touch_commit) {

        if (!AddVectoredExceptionHandler(1,host_commit_fault)) win_fail("installing first-touch commit handler",0,0);

        puts("Runtime: physical guest backing commits on first access (16 KiB pages), not whole mappings");

        printf("Runtime: first-touch CPU mapping threshold=%llu MiB (0 disables); flexible memory remains eager\n",

               (unsigned long long)(touch_cpu_min_bytes>>20));

    }

    if (backing_base && demand_commit) printf("Runtime: guest backing reserved (%" PRIu64 " MiB), committing only mapped physical ranges\n",bytes>>20);

    if (backing_base) pager_start(); /* its fault handler goes first, before the first-touch one */

    return backing_base ? 0 : -1;

}

#else

/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */

static int pool(void) {

    if (pool_fd>=0) return 0;

    pool_fd=memfd_create("bb-guest-memory", MFD_CLOEXEC);

    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;

    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);

    if (view==MAP_FAILED) return -1;

    backing_base=view;

    return 0;

}

void runtime_memory_reserve(void) {}

static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys,int cpu_only) {

    (void)cpu_only;

    void *mapped = !reserved

        ? mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys)

        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);

    return mapped==MAP_FAILED ? -1 : 0;

}

static int host_unmap(uintptr_t address, uint64_t size) { return munmap((void *)address,size); }

static int host_protect(uintptr_t address, uint64_t size, int prot) { return mprotect((void *)address,size,prot); }

static void host_discard(uint64_t phys, uint64_t size) {

    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);

}

#endif

static void queue_hook(int kind, uintptr_t address, uint64_t size) {

    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;

    if (!hook) return;

    if (pending_count==pending_capacity) {

        if (pending_capacity>SIZE_MAX/2/sizeof(*pending)) {

            fputs("STOP: GPU hook queue size overflow\n",stderr); exit(21);

        }

        size_t capacity=pending_capacity*2;

        PendingHook *next=malloc(capacity*sizeof(*next));

        if (!next) {

            fprintf(stderr,"STOP: GPU hook queue allocation failed: count=%zu bytes=%zu\n",

                    pending_count,capacity*sizeof(*next)); exit(21);

        }

        memcpy(next,pending,pending_count*sizeof(*next));

        if (pending!=pending_inline) free(pending);

        pending=next;pending_capacity=capacity;

        fprintf(stderr,"Runtime: GPU hook queue grew to %zu entries (%zu bytes)\n",

                capacity,capacity*sizeof(*pending));

    }

    pending[pending_count++]=(PendingHook){kind,address,size};

}

/* Runs queued GPU notifications; callers invoke it after dropping the lock. */

static void flush_hooks(void) {

    for (;;) {

        write_lock();

        if (!pending_count) { write_unlock(); return; }

        PendingHook h=pending[0];

        memmove(pending,pending+1,--pending_count*sizeof(*pending));

        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;

        write_unlock();

        if (hook) hook(h.address,h.size);

    }

}

static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }

static void flex_set(uint64_t first, uint64_t count, int used) {

    for (uint64_t p=first;p<first+count;++p) {

        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));

    }

}

static uint64_t flex_alloc(uint64_t size) {

    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;

    for (uint64_t p=0;p<total;++p) {

        run = flex_test(p) ? 0 : run+1;

        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }

    }

    return UINT64_MAX;

}

static void flex_free(uint64_t phys, uint64_t size) {

    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);

    host_discard(phys,size);

}

static size_t vma_index(uintptr_t a) { /* first VMA with end > a */

    size_t lo=0, hi=vma_count;

    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }

    return lo;

}

static int vma_insert(size_t at, Vma v) {

    if (vma_count==vma_capacity) {

        size_t capacity=vma_capacity ? vma_capacity*2 : 256;

        /* God of War (2026-11-09), Detroit (2026-11-11): the old array is never freed, as a
         * lock-free reader (table_view) may still be reading it; the doublings together stay
         * below the final size. Pointer first, then capacity: a reader that sees the new capacity
         * sees the new array. */
        Vma *next=malloc(capacity*sizeof(*vmas));
        if (!next) return -1;
        if (vma_count) memcpy(next,vmas,vma_count*sizeof(*vmas));
        __atomic_store_n(&vmas,next,__ATOMIC_RELEASE);
        __atomic_store_n(&vma_capacity,capacity,__ATOMIC_RELEASE);

    }

    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));

    vmas[at]=v; ++vma_count; return 0;

}

static void vma_erase(size_t at, size_t n) {

    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;

}

static int split_at(uintptr_t a) {

    size_t i=vma_index(a);

    if (i==vma_count || vmas[i].start>=a) return 0;

    Vma right=vmas[i];

    right.start=a;

    if (right.kind!=KIND_RESERVED) right.phys+=a-vmas[i].start;

    vmas[i].end=a;

    return vma_insert(i+1,right);

}

/* Range [start,end) split on its edges; returns the first index inside it. */

static size_t carve(uintptr_t start, uintptr_t end, int *error) {

    *error = split_at(start) || split_at(end);

    return vma_index(start);

}

static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {

    size_t i=vma_index(start); uintptr_t at=start;

    for (; at<end; ++i) {

        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;

        at=vmas[i].end;

    }

    return 1;

}

static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {

    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)

        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;

    return 0;

}

/* First-fit search in the PS4 user range, starting from the hint. */

static uintptr_t find_free_from(uintptr_t hint, uint64_t size, uint64_t alignment) {

    uintptr_t at=align_up(hint<ALLOC_MIN ? ALLOC_MIN : hint, alignment);

    while (at<=USER_MAX && size<=USER_MAX-at) {

        size_t i=vma_index(at);

        if (i<vma_count && vmas[i].start<at+size) {

            at=align_up(vmas[i].end,alignment);

            continue;

        }

#ifdef _WIN32

        if (lazy_guest_memory) {

            uintptr_t after=0;

            if (!host_can_own(at,size,&after)) {

                if (!after || after<=at) return 0;

                at=align_up(after,alignment);

                continue;

            }

        }

#endif

        return at;

    }

    return 0;

}

/* Spider-Man port (2026-11-04): mappings asked for without an address start near the top of the user
 * range (BB_HINTLESS_BASE, hex, default 0xf000000000; 0 = from the bottom as before). Spider-Man hands
 * out its own 2 GiB slots from 0x1000000000 up and maps them with the slot as a hint; libc's heap
 * (no address) had taken 0x1000000000, so slot 0 moved 16 MiB up while the game kept using the slot
 * address (sm-pc3: write fault at 0x1000f90000). On the PS4 that address is free. */
static uintptr_t hintless_base(void) {
    static uintptr_t base=1;
    if (base==1) {
        const char *e=getenv("BB_HINTLESS_BASE");
        base=e && *e ? (uintptr_t)strtoull(e,NULL,16) : UINT64_C(0xf000000000);
    }
    return base;
}
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    if (!hint && hintless_base()) {
        uintptr_t high=find_free_from(hintless_base(),size,alignment);
        if (high) return high;
    }
    return find_free_from(hint,size,alignment);
}
static void drop_range(uintptr_t start, uintptr_t end) {

    int error; size_t i=carve(start,end,&error), n=0;

    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {

        if (vmas[i+n].kind==KIND_FLEXIBLE) {

            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;

            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);

        }

        ++n;

    }

    vma_erase(i,n);

}

/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */

static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,

                     int kind, int type, uint64_t phys) {

    uintptr_t address=(uintptr_t)*inout;

    if (flags & MAP_FIXED_FLAG) {

        if (!address || address%PAGE) return INVALID;

        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;

    } else {

        address=find_free(address,size,alignment);

        if (!address) return NO_MEMORY;

    }

    if (host_map(address,size,host_prot(prot),kind==KIND_RESERVED,phys,type==0 && !(prot&0x30))) return NO_MEMORY;

    void *mapped=(void *)address;

    drop_range(address,address+size);

    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys,{0}})) return NO_MEMORY;

    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;

    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);

    *inout=mapped;

    return 0;

}

static int block_type(uint64_t phys) {

    for (int i=0;i<LIMIT;++i)

        if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) return blocks[i].type;

    return -1;

}

static int direct_range_allocated(uint64_t phys, uint64_t size) {

    for (uint64_t at=phys; at<phys+size;) {

        int found=0;

        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {

            at=blocks[i].start+blocks[i].size; found=1; break;

        }

        if (!found) return 0;

    }

    return 1;

}



static ABI uint64_t direct_size(void) { return POOL_SIZE; }

static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,

                                  uint64_t alignment, int type, int64_t *out) {

    if (!alignment) alignment = PAGE;

    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)

        return INVALID;

    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;

    if ((uint64_t)low >= end || size > end - (uint64_t)low) return NO_SPACE;

    write_lock();

    if (pool()) { write_unlock(); return NO_MEMORY; }

    uint64_t start = align_up((uint64_t)low, alignment);

    int slot = -1;

    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }

    for (;;) {

        if (slot<0 || start > end || size > end-start) { write_unlock(); return NO_SPACE; }

        int overlap = 0;

        for (int i=0; i<LIMIT; ++i) {

            Block b = blocks[i];

            if (b.used && start < b.start+b.size && b.start < start+size) {

                start = align_up(b.start+b.size, alignment); overlap = 1; break;

            }

        }

        if (!overlap) break;

    }

    blocks[slot] = (Block){start, size, type, 1}; *out = (int64_t)start;

    ++allocations; live_bytes += size;

    write_unlock();

    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);

    return 0;

}

static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {

    if (!alignment) alignment=PAGE;

    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||

        (prot & ~0x37)) return INVALID;

    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;

    /* GPU-shared backing needs one guest address per physical page: report any second mapping. */

    for (size_t i=0;i<vma_count;++i)

        if (vmas[i].kind==KIND_DIRECT && vmas[i].phys<(uint64_t)physical+size &&

            (uint64_t)physical<vmas[i].phys+(vmas[i].end-vmas[i].start))

            printf("Runtime: direct alias phys=0x%" PRIx64 " size=%" PRIu64 " already mapped at %p (phys 0x%" PRIx64 ")\n",

                   (uint64_t)physical,size,(void*)vmas[i].start,vmas[i].phys);

    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);

    if (!result) ++maps;

    return result;

}

static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {

    write_lock();

    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);

    write_unlock();

    flush_hooks();

    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);

    return result;

}

static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,

                                    uint64_t alignment, const char *name) {

    int32_t result=direct_map(out,size,prot,flags,physical,alignment);

    return !result && name ? set_range_name(*out,size,name) : result;

}

static int32_t unmap_locked(uintptr_t start, uint64_t size) {

    if (start%PAGE || !size) return INVALID;

    uint64_t end=start+align_up(size,PAGE);

    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)

        if (vmas[i].kind!=KIND_RESERVED) {

            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;

            queue_hook(HOOK_UNMAP,a,b-a);

        }

    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {

        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;

        if (host_unmap(a,b-a)) return INVALID;

    }

    drop_range(start,end);

    return 0;

}

static ABI int32_t direct_unmap(void *address, uint64_t size) {

    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */

    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));

    write_lock();

    size_t before=pending_count;

    int32_t result=unmap_locked((uintptr_t)address,size);

    pending_count=before; /* already notified above */

    write_unlock();

    return result;

}

static ABI int32_t direct_release(uint64_t start, uint64_t size) {

    if (start % PAGE || size % PAGE) return INVALID;

    if (!size) return 0;

    write_lock();

    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }

#ifdef _WIN32

    discarding_private=1;discard_begin=start;discard_end=start+size;

#endif

    /* Mappings of released physical pages disappear with them. */

    for (size_t i=0; i<vma_count;) {

        Vma v=vmas[i];

        uint64_t length=v.end-v.start;

        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {

            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;

            unmap_locked(v.start+(a-v.phys),b-a);

            i=vma_index(v.start);

            continue;

        }

        ++i;

    }

    for (int i=0; i<LIMIT; ++i) {

        Block *b=&blocks[i];

        if (!b->used || b->start>=start+size || start>=b->start+b->size) continue;

        uint64_t a=b->start>start ? b->start : start, e=b->start+b->size<start+size ? b->start+b->size : start+size;

        Block left=*b, right=*b;

        left.size=a-b->start; right.start=e; right.size=b->start+b->size-e;

        b->used=0;

        if (left.size) *b=left;

        if (right.size) for (int j=0;j<LIMIT;++j) if (!blocks[j].used) { blocks[j]=right; break; }

        live_bytes-=e-a;

    }

    host_discard(start,size); /* zero on reuse */

#ifdef _WIN32

    discarding_private=0;

#endif

    write_unlock();

    flush_hooks();

    return 0;

}

static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {

    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;

    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;

    uint64_t phys=flex_alloc(size);

    if (phys==UINT64_MAX) return NO_MEMORY;

    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);

    if (result) flex_free(phys,size); else ++flexible_maps;

    return result;

}

static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {

    write_lock();

    int32_t result=map_flexible_locked(inout,size,prot,flags);

    write_unlock();

    flush_hooks();

    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);

    else fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",

                 size, (unsigned)result, flexible_bytes);

    return result;

}

static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {

    int32_t result=map_flexible(inout,size,prot,flags);

    return !result && name ? set_range_name(*inout,size,name) : result;

}

static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }

static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {

    if (!alignment) alignment=PAGE;

    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;

    write_lock();

    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout,(uintptr_t)*inout+size,0)) {

        write_unlock(); return NO_MEMORY;

    }

    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);

    write_unlock();

    if (!result) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *inout, size);

    return result;

}

static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {

    uintptr_t end=start+align_up(size,PAGE);

    start&=~(uintptr_t)(PAGE-1);

    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;

    if (host_protect(start,end-start,host_prot(prot))) return INVALID;

    int error; size_t i=carve(start,end,&error);

    if (error) return NO_MEMORY;

    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }

    queue_hook(HOOK_INVALIDATE,start,end-start);

    ++protects;

    return 0;

}

static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {

    write_lock();

    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);

    write_unlock();

    flush_hooks();

    return result;

}

/* God of War (2026-11-03): protection with a memory type (the type is recorded, as protect_locked

 * does for direct memory), and POSIX mmap as libc uses it (after GoW3's port): anonymous memory

 * from the flexible pool; file mappings fail with ENODEV. */

static ABI int32_t mtype_protect(const void *address, uint64_t size, int type, int prot) {

    write_lock();

    int32_t result=protect_locked((uintptr_t)address,size,prot,type);

    write_unlock();

    flush_hooks();

    return result;

}

static ABI void *posix_mmap(void *address, uint64_t size, int prot, int flags, int fd, int64_t offset) {

    if (!(flags & 0x1000) || fd!=-1 || offset) { *runtime_errno()=19; return (void *)-1; }

    void *out=address;

    if (map_flexible(&out,(size+PAGE-1)&~(uint64_t)(PAGE-1),prot,flags & 0x10)) { *runtime_errno()=12; return (void *)-1; }

    return out;

}

static ABI int32_t posix_msync(void *address, uint64_t size, int flags) { (void)address; (void)size; (void)flags; return 0; }

static ABI int32_t set_range_name(const void *address,uint64_t size,const char *name) {

    if (!name) return (int32_t)UINT32_C(0x8002000e);

    size_t length=strnlen(name,32);

    if (length==32) return (int32_t)UINT32_C(0x8002003f);

    uintptr_t start=(uintptr_t)address;

    if (!size || start<USER_MIN || start>=USER_MAX || size>USER_MAX-start || start%PAGE || size%PAGE) return INVALID;

    write_lock();

    if (!covered(start,start+size,1)) { write_unlock(); return ACCESS; }

    int error=0;

    size_t first=carve(start,start+size,&error);

    if (error) { write_unlock(); return NO_MEMORY; }

    for (size_t i=first;i<vma_count && vmas[i].start<start+size;++i) {

        memset(vmas[i].name,0,sizeof(vmas[i].name));

        memcpy(vmas[i].name,name,length);

    }

    write_unlock();

    return 0;

}

static ABI int32_t clear_range_name(const void *address,uint64_t size) { return set_range_name(address,size,""); }

static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {

    read_lock();

    size_t i=vma_index((uintptr_t)address);

    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {

        read_unlock(); return ACCESS;

    }

    /* Report the whole contiguous run with identical protection. */

    size_t first=i, last=i;

    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;

    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;

    if (start) *start=(void *)vmas[first].start;

    if (end) *end=(void *)vmas[last].end;

    if (prot) *prot=(uint32_t)vmas[i].prot;

    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);

    read_unlock();

    return 0;

}

static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {

    if (!info || info_size!=sizeof(*info)) return INVALID;

    read_lock();

    size_t i=vma_index((uintptr_t)address);

    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }

    Vma v=vmas[i];

    memset(info,0,sizeof(*info));

    info->start=v.start; info->end=v.end;

    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;

    info->protection=v.prot; info->memory_type=v.type;

    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);

    memcpy(info->name,v.name,sizeof(info->name));

    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);

    read_unlock();

    return 0;

}

static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {

    read_lock();

    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {

        if (type) *type=blocks[i].type;

        if (start) *start=(void *)(uintptr_t)blocks[i].start;

        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);

        read_unlock(); return 0;

    }

    read_unlock();

    return ACCESS;

}

static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {

    if (!entries || count<0) return INVALID;

    int32_t result=0; int done=0;

    /* Detroit (2026-11-10): every mapped entry printed a line here, inside the write lock that every
     * other thread's fault and mapping waits for (22100 lines in a 13 min walk, 2-5 s stalls while the
     * game streams). Now counted, and a batch that held the lock 50 ms or more says so after it. */
    static volatile long long batch_mapped_entries;
    const ULONGLONG batch_begin=GetTickCount64();
    uint64_t batch_bytes=0;

    write_lock();

    for (; done<count; ++done) {

        BatchEntry *e=&entries[done];

        switch (e->operation) {

        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0);

            if (!result) { batch_bytes+=e->length; __atomic_add_fetch(&batch_mapped_entries,1,__ATOMIC_RELAXED); }

            break;

        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;

        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;

        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;

        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;

        default: result=INVALID;

        }

        if (result) {

            /* God of War port (2026-11-03): a failed entry ends the batch; said, since a game that

             * goes on writes to the pages it thought mapped (TexStream into the PRT aperture). */

            static volatile long said;

            if (__atomic_add_fetch(&said,1,__ATOMIC_RELAXED)<=32)

                fprintf(stderr,"Runtime: batch map entry %d of %d failed: operation %d at %p, phys=0x%" PRIx64 ", size=%" PRIu64

                        ", prot=0x%x, type=%d, flags=0x%x -> 0x%08x\n",done,count,e->operation,e->start,(uint64_t)e->offset,

                        (uint64_t)e->length,(unsigned)(uint8_t)e->prot,e->type,flags,(unsigned)result);

            break;

        }

    }

    write_unlock();

    {
        const ULONGLONG held=GetTickCount64()-batch_begin;
        if (held>=50) {
            printf("Runtime: batch map of %d entries (%llu KiB mapped) took %llu ms with the memory lock held; %lld entries mapped so far\n",
                   count,(unsigned long long)(batch_bytes>>10),(unsigned long long)held,(long long)batch_mapped_entries);
            fflush(stdout);
        }
    }

    flush_hooks();

    if (processed) *processed=done;

    return result;

}

static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {

    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);

}

static const RuntimeExport exports[]={

    {"sceKernelSetVirtualRangeName",set_range_name}, {"sceKernelClearVirtualRangeName",clear_range_name},

    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},

    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},

    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},

    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},

    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},

    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},

    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},

    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},

    /* God of War (2026-11-03) */

    {"sceKernelMtypeprotect",mtype_protect}, {"sceKernelCheckedReleaseDirectMemory",direct_release},

    {"mmap",posix_mmap}, {"msync",posix_msync},

};

uintptr_t runtime_memory_resolve(const char *name) {

    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */

    if (!strcmp(name, "pO96TwzOm5E#p#J")) return (uintptr_t)direct_size;

    if (!strcmp(name, "rTXw65xmLIA#p#J")) return (uintptr_t)direct_allocate;

    if (!strcmp(name, "L-Q3LEjIbgA#p#J")) return (uintptr_t)direct_map;

    if (!strcmp(name, "MBuItvba6z8#p#J")) return (uintptr_t)direct_release;

    if (!strcmp(name, "cQke9UuBQOk#p#J")) return (uintptr_t)direct_unmap;

    return RUNTIME_LOOKUP(exports,name);

}

/* Host-owned memory the guest can see (image, stacks, trampolines) lives in

 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */

#define LOW_MIN UINT64_C(0x0400000000) /* up to USER_MIN (0x700000000): 12 GiB */

static uintptr_t low_next=LOW_MIN;

void *runtime_low_map(size_t size, int prot) {

    size=align_up(size,PAGE);

    write_lock();

#ifdef _WIN32

    void *p=NULL;

    while (!p && low_next+size<=USER_MIN) {

        p=VirtualAlloc((void *)low_next,size,MEM_RESERVE|MEM_COMMIT,win_prot(prot));

        low_next+=size+UINT64_C(65536); /* unmapped gap catches overruns; allocation granularity */

    }

    write_unlock();

    return p;

#else

    void *p=MAP_FAILED;

    while (low_next+size<=USER_MIN) {

        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);

        low_next+=size+PAGE; /* unmapped gap catches overruns */

        if (p!=MAP_FAILED) break;

    }

    write_unlock();

    return p==MAP_FAILED ? NULL : p;

#endif

}

/* ---- GPU library interface (gpu/shim/bbgpu.cpp) ---- */

/* Optimizations switched off at run time (diagnostics): the number in the file named by

 * BB_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,

 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,

 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,

 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,

 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,

 * 4096 resource sharps read by the draw-preparation workers,

 * 8192 prepared draws from the draw-preparation workers,

 * 16384 small read-only buffer copies on the recording thread,

 * 32768 hot pages (opt-in with BB_HOT_PAGES=1),

 * 65536 unprotect the 256 KiB window around a guest write fault (BB_FAULT_WINDOW KiB),

 * 131072 large guest memory copies split across copy threads (BB_COPY_THREADS),

 * 262144 with BB_ASYNC_FENCES=1: wait for guest copies at fences again,

 * 524288 small guest copies batched for the copy threads instead of the recording thread,

 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */

/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/bbport_toggles.h). */

uint64_t runtime_disabled_optimizations;

/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery

 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */

__thread sigjmp_buf *runtime_fault_recover;

static void *toggle_watcher(void *path) {

    for (unsigned long long last=ULLONG_MAX;;) {

        FILE *f=fopen(path,"r");

        unsigned long long value=0;

        if (f) { if (fscanf(f,"%llu",&value)!=1) value=0; fclose(f); }

        if (value!=last) {

            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);

            printf("Runtime: disabled optimizations mask=%llu\n",value);

            last=value;

        }

#ifdef _WIN32

        compat_sleep_ns(250000000);

#else

        struct timespec t={0,250000000}; nanosleep(&t,NULL);

#endif

    }

    return NULL;

}

void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {

    const char *toggles=getenv("BB_TOGGLE_FILE");

    static pthread_t watcher;

    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);

    write_lock();

    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;

    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);

    write_unlock();

    flush_hooks();

}

/* Writes through the backing view; 0 when part of the range has no backing. */

uint64_t runtime_memory_trim_zero_private(uint64_t scan_bytes) {

#ifdef _WIN32

    if(!private_cpu || !touch_commit)return 0;

    static const unsigned char zeros[PAGE];

    uint64_t freed=0,scanned=0;

    write_lock();

    size_t i=region_index(zero_trim_cursor),visited=0;

    if(i>=region_count)i=0;

    while(visited++<region_count && visited<=4096 && scanned<scan_bytes) {

        HostRegion *r=&regions[i];

        if(r->mapped==2 && r->initial_protect==PAGE_READWRITE) {

            for(uintptr_t at=r->start;at<r->end && scanned<scan_bytes;at+=PAGE) {

                MEMORY_BASIC_INFORMATION info;

                if(!VirtualQuery((void*)at,&info,sizeof(info)) || info.State!=MEM_COMMIT || info.Protect!=PAGE_READWRITE || (uintptr_t)info.BaseAddress+info.RegionSize<at+PAGE)continue;

                scanned+=PAGE;DWORD old;

                __atomic_store_n(&zero_trim_guard,at,__ATOMIC_RELEASE);

                if(VirtualProtect((void*)at,PAGE,PAGE_READONLY,&old)) {

                    if(!memcmp((void*)at,zeros,PAGE)) {

                        if(!VirtualFree((void*)at,PAGE,MEM_DECOMMIT))win_fail("decommitting zero private page",at,PAGE);

                        freed+=PAGE;

                    } else if(!VirtualProtect((void*)at,PAGE,old,&old))win_fail("restoring zero scan protection",at,PAGE);

                }

                __atomic_store_n(&zero_trim_guard,0,__ATOMIC_RELEASE);

            }

        }

        zero_trim_cursor=r->end;

        if(++i>=region_count) { i=0;zero_trim_cursor=regions[0].start; }

    }

    write_unlock();return freed;

#else

    (void)scan_bytes;return 0;

#endif

}

/* bbport: as runtime_memory_write_backing, but declines (returns 0, nothing written) memory in

 * private CPU regions instead of promoting them: the GPU command thread's compute fence labels.

 * A plain store to the guest's view of a shared region costs ~2.3 us on the Xbox (a first-write

 * trap every fence, ~43k/s); the same physical page through the backing view does not. */

int runtime_memory_write_backing_shared(uintptr_t address, const void *data, uint64_t size) {

    read_lock();

    int ok=1;

    for (uintptr_t at=address, end=address+size; at<end && ok;) {

        size_t i=vma_index(at);

        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }

        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;

#ifdef _WIN32

        if(private_cpu) {

            size_t r=region_index(at);

            if(r<region_count && regions[r].start<=at) {

                if(regions[r].mapped>=2) { ok=0; break; }

                if(n>regions[r].end-at)n=regions[r].end-at;

            }

        }

#endif

        at+=n;

    }

    if(ok) {

        for (uintptr_t at=address, end=address+size; at<end;) {

            size_t i=vma_index(at);

            uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;

            memcpy(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);

            at+=n;

        }

    }

    read_unlock();

    return ok;

}

int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {

    read_lock();

    int ok=1;

    for (uintptr_t at=address, end=address+size; at<end && ok;) {

        size_t i=vma_index(at);

        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }

        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;

#ifdef _WIN32

        if(private_cpu) {

            size_t r=region_index(at);

            if(r<region_count && regions[r].start<=at) {

                if(n>regions[r].end-at)n=regions[r].end-at;

                if(regions[r].mapped==3) {

                    shared_copy((unsigned char*)at,(unsigned char*)data+(at-address),n,1);

                    at+=n;continue;

                }

                if(regions[r].mapped==2 || regions[r].mapped==4)private_promote(r);

            }

        }

#endif

        memcpy(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);

        at+=n;

    }

    read_unlock();

    return ok;

}

/* Reads through the backing view: the host's copies of guest memory (uploads), which the GPU's

 * read protection (BB_READBACKS=2: GPU-written pages) must not stop. A fault there would ask the

 * GPU thread for a readback while it waits for these very copies (a hang at start). Gaps read

 * as zeros. */



#ifdef _WIN32

/* Only positive MEM_COMMIT answers are retained, under the existing region

 * lock. Mapping/release/zero-trim writers change table_generation. New commits

 * cannot make a positive answer stale; reserved pages are NEVER cached.

 * GPU protection changes do not change commitment; memcpy still enforces the

 * current access protection and existing fault/coherency handlers.

 * A private-to-shared promotion copies every committed page before replacing

 * its private view; subsequent upload selection uses the physical backing.

 * During a nested writer (odd generation), fall back to VirtualQuery. */

typedef struct {uintptr_t start,end;uint64_t generation;} UploadCommittedRange382;

static __thread UploadCommittedRange382 upload_committed382[1024];

/* Mode3 capacity trial. Allocate only on an uploading thread which selects it;

 * pthread TLS destruction frees it on thread exit. No per-call key lookup. */

static __thread UploadCommittedRange382 *upload_large958;

static pthread_key_t upload_key958;

static pthread_once_t upload_key_once958=PTHREAD_ONCE_INIT;

static int upload_key_error958;

static void upload_free958(void *p){if(upload_large958==p)upload_large958=NULL;free(p);}

static void upload_key_init958(void){upload_key_error958=pthread_key_create(&upload_key958,upload_free958);}

static int upload_large_ready958(void){

    if(upload_large958)return 1;

    if(pthread_once(&upload_key_once958,upload_key_init958)||upload_key_error958)return 0;

    UploadCommittedRange382 *p=calloc(16384,sizeof(*p));if(!p)return 0;

    if(pthread_setspecific(upload_key958,p)){free(p);return 0;}

    upload_large958=p;return 1;

}



/* Positive last-span lookaside: same generation and commit-only contract. */

static __thread UploadCommittedRange382 upload_last697;

static __thread unsigned upload_mode_calls382,upload_mode382;

static __thread ULONGLONG upload_mode_next382;

static __attribute__((noinline)) unsigned upload_read_mode382(void) {

    char path[32768];DWORD n=GetEnvironmentVariableA("BB_GPU_USER_DIR",path,sizeof(path)-80);

    if(!n || n>=sizeof(path)-80)return 0;

    snprintf(path+n,sizeof(path)-n,"\\upload-query-cache-mode.txt");

    /* No file: mode 3 (2026-10-29, Xbox courtyard 13.9 -> 16.7 FPS with copy-map-mode). */

    FILE *f=fopen(path,"rb");if(!f)return 3;

    int c;do{c=fgetc(f);}while(c!=EOF && (c==' '||c=='\r'||c=='\n'||c=='\t'));

    unsigned mode=(c>='0'&&c<='3')?(unsigned)(c-'0'):0;int valid=c>='0'&&c<='3';

    while((c=fgetc(f))!=EOF)if(c!=' '&&c!='\r'&&c!='\n'&&c!='\t')valid=0;

    if(ferror(f))valid=0;fclose(f);return valid?mode:0;

}

static unsigned upload_query_mode382(void) {

    /* Poll outside the table lock, at most once/s/thread. */

    if(!(upload_mode_calls382++ & 4095)) {

        ULONGLONG now=GetTickCount64();

        if(now>=upload_mode_next382) {

            upload_mode_next382=now+1000;unsigned value=upload_read_mode382();

            if(value==3 && !upload_large_ready958()){

                printf("CAP958 allocation_failed: tid=%lu fallback_mode=1\n",(unsigned long)GetCurrentThreadId());

                value=1;

            }

            if(value!=upload_mode382){

                memset(upload_committed382,0,sizeof(upload_committed382));memset(&upload_last697,0,sizeof(upload_last697));

                if(upload_large958)memset(upload_large958,0,16384*sizeof(*upload_large958));

                printf("CAP958 mode: tid=%lu old=%u mode=%u entries=%u\n",(unsigned long)GetCurrentThreadId(),upload_mode382,value,value==3?16384:1024);

            }

            upload_mode382=value;

        }

    }

    return upload_mode382;

}

static UploadCommittedRange382 *upload_slot382(uintptr_t source) {

    uint64_t page=source>>12;page^=page>>11;page^=page>>23;

    if(upload_mode382==3 && upload_large958)return &upload_large958[page & 16383];

    return &upload_committed382[page & 1023];

}

/* Committed 64 KiB chunks, shared by all threads (2026-11-02). After the move into Boston a

 * VirtualQuery here cost ~88 us (fresh load ~5 us: it walks the page tables of the whole run) and

 * the per-thread page-hashed cache above still missed ~2k times/s on the GPU thread: ~20% of that

 * thread in the kernel, 26 instead of 30 FPS. One query now marks every whole chunk of its run.

 * Only with the recursive table lock (BB_UWP_PRIVATE_CPU): readers and writers are exclusive, so

 * plain accesses are safe. Cleared when the table generation changes. Pager evictions do not

 * change it: every reader brings a chunk back first, and a page-in commits the pages it had

 * (a marked chunk had all 16). BB_COMMIT_BITMAP=0 turns it off. */

#define COMMIT_SHIFT 16

static uint64_t *commit_guest_bits, *commit_backing_bits, commit_bits_generation=1;

static size_t commit_guest_lo=SIZE_MAX, commit_guest_hi, commit_backing_lo=SIZE_MAX, commit_backing_hi;

static int commit_bitmap_on(void) {

    static int enabled=-1;

    if(enabled<0) {

        const char *e=getenv("BB_COMMIT_BITMAP");

        enabled=!(e && e[0]=='0') && recursive_table_mode();

        if(enabled) {

            commit_guest_bits=calloc((size_t)((USER_MAX-LOW_MIN)>>COMMIT_SHIFT)/64,sizeof(uint64_t));

            commit_backing_bits=calloc((size_t)(POOL_SIZE>>COMMIT_SHIFT)/64+1,sizeof(uint64_t));

            enabled=commit_guest_bits && commit_backing_bits;

        }

        printf("Commit bitmap: %s\n",enabled ? "on" : "off");

    }

    return enabled;

}

/* The bitmap and chunk index of `address`, or NULL; the dirty word range to clear later. */

static uint64_t *commit_bitmap_of(uintptr_t address, size_t *chunk, size_t **lo, size_t **hi) {

    if(address>=LOW_MIN && address<USER_MAX) {

        *chunk=(size_t)((address-LOW_MIN)>>COMMIT_SHIFT);*lo=&commit_guest_lo;*hi=&commit_guest_hi;

        return commit_guest_bits;

    }

    const uintptr_t base=(uintptr_t)backing_base;

    if(base && address>=base && address-base<POOL_SIZE) {

        *chunk=(size_t)((address-base)>>COMMIT_SHIFT);*lo=&commit_backing_lo;*hi=&commit_backing_hi;

        return commit_backing_bits;

    }

    return NULL;

}

static void commit_bitmap_sync(uint64_t generation) {

    if(generation==commit_bits_generation)return;

    if(commit_guest_lo<=commit_guest_hi)

        memset(commit_guest_bits+commit_guest_lo,0,(commit_guest_hi-commit_guest_lo+1)*sizeof(uint64_t));

    if(commit_backing_lo<=commit_backing_hi)

        memset(commit_backing_bits+commit_backing_lo,0,(commit_backing_hi-commit_backing_lo+1)*sizeof(uint64_t));

    commit_guest_lo=commit_backing_lo=SIZE_MAX;commit_guest_hi=commit_backing_hi=0;

    commit_bits_generation=generation;

}

/* Bytes from `source` to the end of its run of marked chunks (at most 64 MiB), or 0. */

static uint64_t commit_bitmap_span(uintptr_t source) {

    size_t chunk,*lo,*hi;

    const uint64_t *bits=commit_bitmap_of(source,&chunk,&lo,&hi);

    if(!bits || !(bits[chunk/64]>>(chunk%64)&1))return 0;

    const uintptr_t first=source&~(uintptr_t)((1u<<COMMIT_SHIFT)-1);

    size_t n=1;

    while(n<1024 && commit_bitmap_of(first+((uintptr_t)n<<COMMIT_SHIFT),&chunk,&lo,&hi)==bits &&

          (bits[chunk/64]>>(chunk%64)&1)) ++n;

    return first+((uintptr_t)n<<COMMIT_SHIFT)-source;

}

static void commit_bitmap_mark(uintptr_t begin, uintptr_t end) {

    const uintptr_t mask=(uintptr_t)((1u<<COMMIT_SHIFT)-1);

    for(uintptr_t at=(begin+mask)&~mask;at+mask<end && at+mask>at;at+=mask+1) {

        size_t chunk,*lo,*hi;

        uint64_t *bits=commit_bitmap_of(at,&chunk,&lo,&hi);

        if(!bits)continue;

        bits[chunk/64]|=UINT64_C(1)<<(chunk%64);

        if(chunk/64<*lo)*lo=chunk/64;

        if(chunk/64>*hi)*hi=chunk/64;

    }

}

static uint64_t upload_committed_span382(uintptr_t source) {

    const uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);

    if(generation&1){if(upload_timing.enabled)++upload_timing.cache_odd948;return 0;}

    if(commit_bitmap_on()) {

        commit_bitmap_sync(generation);

        const uint64_t span=commit_bitmap_span(source);

        if(span)return span;

    }

    if(upload_mode382==2 && upload_last697.generation==generation &&

       upload_last697.start<=source && source<upload_last697.end)

        return upload_last697.end-source;

    const UploadCommittedRange382 *entry=upload_slot382(source);

    if(entry->generation==generation && entry->start<=source && source<entry->end) {

        if(upload_mode382==2)upload_last697=*entry;

        return entry->end-source;

    }

    if(upload_timing.enabled){

        if(!entry->end)++upload_timing.cache_cold948;

        else if(entry->generation!=generation)++upload_timing.cache_generation948;

        else ++upload_timing.cache_collision948;

    }

    return 0;

}

static void upload_remember_commit382(const MEMORY_BASIC_INFORMATION *info,uintptr_t source) {

    const uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);

    if((generation&1) || info->State!=MEM_COMMIT)return;

    uintptr_t begin=(uintptr_t)info->BaseAddress,end=begin+info->RegionSize;

    if(end<=begin || source<begin || source>=end)return;

    if(commit_bitmap_on()) {

        commit_bitmap_sync(generation);

        commit_bitmap_mark(begin,end);

    }

    *upload_slot382(source)=(UploadCommittedRange382){begin,end,generation};

    if(upload_mode382==2)upload_last697=(UploadCommittedRange382){begin,end,generation};

}

#endif





#ifdef _WIN32

/* Cache only table lookup indices while the existing read lock is held.

 * The global generation invalidates structural changes; source selection

 * still re-reads live Vma.phys and HostRegion.mapped on every upload.

 * No cached host pointers, cached gaps, protection bypass or cached zeros. */

typedef struct {uint64_t generation;uintptr_t start,end;size_t vma,region;} UploadMap498;

static __thread UploadMap498 upload_maps498[4];

static __thread unsigned upload_map_next498,upload_map_calls498,upload_map_mode498;

static __thread uint64_t upload_map_hits498,upload_map_misses498;

static __thread ULONGLONG upload_map_poll498;

static unsigned upload_map_control498(void) {

    if(!(upload_map_calls498++ & 4095)) {

        ULONGLONG now=GetTickCount64();if(now>=upload_map_poll498) {

            upload_map_poll498=now+1000;char path[32768];unsigned mode=1; /* on without a file */

            DWORD n=GetEnvironmentVariableA("BB_GPU_USER_DIR",path,sizeof(path)-64);

            if(n && n<sizeof(path)-64) {

                snprintf(path+n,sizeof(path)-n,"\\copy-map-mode.txt");

                FILE *f=fopen(path,"rb");if(f){mode=fgetc(f)=='1';fclose(f);}

            }

            if(mode!=upload_map_mode498) {

                printf("MAP498 mode: tick=%llu tid=%lu old=%u mode=%u hits=%llu misses=%llu\n",

                    (unsigned long long)now,(unsigned long)GetCurrentThreadId(),upload_map_mode498,mode,

                    (unsigned long long)upload_map_hits498,(unsigned long long)upload_map_misses498);

                memset(upload_maps498,0,sizeof(upload_maps498));upload_map_hits498=upload_map_misses498=0;upload_map_mode498=mode;

            }

        }

    }

    return upload_map_mode498;

}

static size_t upload_map_lookup498(uintptr_t at,unsigned enabled,size_t *region) {

    *region=SIZE_MAX;if(!enabled)return vma_index(at);

    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);

    if(!(generation&1))for(unsigned j=0;j<4;++j) {

        const UploadMap498 *c=&upload_maps498[j];

        if(c->generation==generation && c->end && c->start<=at && at<c->end && c->vma<vma_count) {

            ++upload_map_hits498;*region=c->region;return c->vma;

        }

    }

    ++upload_map_misses498;size_t i=vma_index(at);

    if((generation&1)||i==vma_count||vmas[i].start>at||vmas[i].kind==KIND_RESERVED)return i;

    uintptr_t begin=vmas[i].start,end=vmas[i].end;

    size_t r=SIZE_MAX;

    if(private_cpu) {

        size_t found=region_index(at);

        if(found<region_count && regions[found].start<=at) {

            r=found;if(begin<regions[r].start)begin=regions[r].start;

            if(end>regions[r].end)end=regions[r].end;

        }

    }

    upload_maps498[upload_map_next498++%4]=(UploadMap498){generation,begin,end,i,r};

    *region=r;return i;

}

#endif



void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {

    unsigned char *out=data;

#ifdef _WIN32

    const unsigned query_cache=upload_query_mode382();

    const unsigned map_cache498=upload_map_control498();

    const int profile=upload_timing_enabled();

    CpuSample948 cpu_sample948=cpu_copy_begin948(profile);

    const uint64_t began=profile ? upload_clock() : 0;

#endif

    read_lock();

#ifdef _WIN32

    if(profile) {upload_timing.copies++;upload_timing.bytes+=size;upload_timing.copy_lock_ticks+=upload_clock()-began;}

#endif

    for (uintptr_t at=address, end=address+size; at<end;) {

#ifdef _WIN32

        size_t upload_region498=SIZE_MAX;

        const size_t i=upload_map_lookup498(at,map_cache498,&upload_region498);

#else

        const size_t i=vma_index(at);

#endif

        uint64_t n;

        if (i<vma_count && vmas[i].start<=at && vmas[i].kind!=KIND_RESERVED) {

            n=(vmas[i].end<end ? vmas[i].end : end)-at;

            unsigned char *source=backing_base+vmas[i].phys+(at-vmas[i].start);

#ifdef _WIN32

            if(private_cpu) {

                size_t r=upload_region498!=SIZE_MAX ? upload_region498 : region_index(at);

                if(r<region_count && regions[r].start<=at) {

                    if(n>regions[r].end-at)n=regions[r].end-at;

                    /* a chunk in the pager's file would read as reserved, zeros: back first */

                    if(regions[r].mapped==2)pager_bring_back(at,at+n);

                    if(regions[r].mapped==2 || regions[r].mapped==4)source=(unsigned char*)at;

                    if(regions[r].mapped==3) {

                        shared_copy((unsigned char*)at,out+(at-address),n,0);

                        at+=n;continue;

                    }

                }

            }

            /* Untouched SEC_RESERVE pages are logically zero. Uploads must not

             * commit real shared backing simply to read those zeros. Apart

             * from wasting RAM that would fault back into the region lock. */

            for (uint64_t done=0;done<n;) {

                const uint64_t cached=query_cache ? upload_committed_span382((uintptr_t)(source+done)) : 0;

                if(cached) {

                    uint64_t run=cached;if(run>n-done)run=n-done;

                    memcpy(out+(at-address)+done,source+done,run);done+=run;

                    if(profile)upload_timing.cache_hits++;

                    continue;

                }

                if(profile && query_cache)upload_timing.cache_misses++;

                MEMORY_BASIC_INFORMATION info;

                const uint64_t query_began=profile ? upload_clock() : 0;

                if (!VirtualQuery(source+done,&info,sizeof(info)))

                    win_fail("querying upload backing",(uintptr_t)(source+done),n-done);

                if(profile) {upload_timing.queries++;upload_timing.query_ticks+=upload_clock()-query_began;}

                if(profile){

                    if(info.State==MEM_COMMIT)++upload_timing.query_commit948;

                    else if(info.State==MEM_RESERVE)++upload_timing.query_reserve948;

                    else ++upload_timing.query_other948;

                }

                if(query_cache)upload_remember_commit382(&info,(uintptr_t)(source+done));

                uint64_t run=(uintptr_t)info.BaseAddress+info.RegionSize-(uintptr_t)(source+done);

                if (run>n-done) run=n-done;

                if (!run) win_fail("invalid upload backing extent",(uintptr_t)(source+done),n-done);

                if (info.State==MEM_RESERVE) {memset(out+(at-address)+done,0,run);if(profile)upload_timing.zero_bytes+=run;}

                else memcpy(out+(at-address)+done,source+done,run);

                done+=run;

            }

#else

            memcpy(out+(at-address),source,n);

#endif

        } else {

            const uintptr_t next=i==vma_count ? end : vmas[i].start>at ? vmas[i].start : vmas[i].end;

            n=(next<end ? next : end)-at;

            memset(out+(at-address),0,n);

        }

        at+=n;

    }

#ifdef _WIN32

    const size_t profile_vmas=vma_count,profile_regions=region_count;

#endif

    read_unlock();

#ifdef _WIN32

    cpu_copy_end948(cpu_sample948,size);

    if(profile) {

        const uint64_t now=upload_clock(),elapsed=now-began;

        upload_timing.copy_ticks+=elapsed;

        static volatile LONG slow_reports;

        if(elapsed>=upload_timing.frequency/50 && InterlockedIncrement(&slow_reports)<=64)

            printf("Xbox slow backing copy376: thread=%lu address=0x%llx bytes=%llu elapsed_ms=%.3f vmas=%zu regions=%zu\n",

                (unsigned long)GetCurrentThreadId(),(unsigned long long)address,(unsigned long long)size,

                (double)elapsed*1000.0/upload_timing.frequency,profile_vmas,profile_regions);

        upload_timing_report(now);

    }

#endif

}

/* Per-thread cache of recently found regions for the GPU's per-draw queries:

 * entries hold for as long as the table generation they were read at. */

typedef struct { uint64_t generation; uintptr_t start, end; int kind; } CachedRegion;

static __thread CachedRegion region_cache[8];

static __thread unsigned region_cache_next;

static const CachedRegion *cached_region(uintptr_t address) {

    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);

    if (generation & 1) return NULL;

    for (unsigned i=0;i<8;++i) {

        const CachedRegion *c=&region_cache[i];

        if (c->generation==generation && c->end && c->start<=address && address<c->end) return c;

    }

    return NULL;

}

/* Called under the read lock after a lookup of address. */

static void cache_region(size_t i) {

    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);

    if (i==vma_count || (generation & 1)) return;

    region_cache[region_cache_next++ % 8]=(CachedRegion){generation,vmas[i].start,vmas[i].end,vmas[i].kind};

}

/* Lock-free table lookups (God of War, 2026-11-09; Detroit 2026-11-11). With BB_UWP_PRIVATE_CPU the
 * table lock is one critical section, so the GPU threads' lookups excluded each other and waited
 * behind runtime_memory_gpu_protect, which holds it across VirtualProtect (Detroit heavy frames:
 * ~1-2 ms per frame blocked in read_lock, plus ClampRangeSize ~1.3 ms, dt-class1/2).
 * A lookup reads the table without the lock and keeps the answer only if table_generation is even
 * and unchanged afterwards: every table change happens under write_lock, which makes it odd
 * before and even again after. Arrays are never freed (vma_insert) and the count is kept within
 * the array's capacity, so a lookup racing a writer reads stale values, never freed memory; it
 * then retries, and falls back to the lock. A thread that holds the table itself takes the old
 * path. BB_TABLE_LOCKFREE=0: off. */
static uint64_t lockfree_reads, lockfree_retries;
uint64_t runtime_lockfree_counts(uint64_t *retries) {
    *retries=__atomic_load_n(&lockfree_retries,__ATOMIC_RELAXED);
    return __atomic_load_n(&lockfree_reads,__ATOMIC_RELAXED);
}
static int table_lockfree_on(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_TABLE_LOCKFREE"); on=!(e && e[0]=='0'); }
    return on;
}
typedef struct { const Vma *p; size_t n; uint64_t generation; } TableView;
static int table_view(TableView *v) {
    if (exclusive_depth || shared_depth || !table_lockfree_on()) return 0;
    v->generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (v->generation & 1) return 0;
    const size_t capacity=__atomic_load_n(&vma_capacity,__ATOMIC_ACQUIRE);
    v->p=__atomic_load_n(&vmas,__ATOMIC_ACQUIRE);
    const size_t n=__atomic_load_n(&vma_count,__ATOMIC_RELAXED);
    v->n=n<capacity ? n : capacity;
    if (!v->p) v->n=0;
    return 1;
}
/* After the lookup: 1 if no writer touched the table while it ran. */
static int table_view_valid(const TableView *v) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&table_generation,__ATOMIC_RELAXED)==v->generation) {
        if (lock_trace_on()) __atomic_add_fetch(&lockfree_reads,1,__ATOMIC_RELAXED);
        return 1;
    }
    if (lock_trace_on()) __atomic_add_fetch(&lockfree_retries,1,__ATOMIC_RELAXED);
    return 0;
}
static uintptr_t view_start(const TableView *v, size_t i) { return __atomic_load_n(&v->p[i].start,__ATOMIC_RELAXED); }
static uintptr_t view_end(const TableView *v, size_t i) { return __atomic_load_n(&v->p[i].end,__ATOMIC_RELAXED); }
static int view_kind(const TableView *v, size_t i) { return __atomic_load_n(&v->p[i].kind,__ATOMIC_RELAXED); }
static size_t view_index(const TableView *v, uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=v->n;
    while (lo<hi) { size_t mid=lo+(hi-lo)/2; if (view_end(v,mid)<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static void view_cache(const TableView *v, uintptr_t start, uintptr_t end, int kind) {
    region_cache[region_cache_next++ % 8]=(CachedRegion){v->generation,start,end,kind};
}
/* Mapped length from address, up to size, across adjacent mappings. */

uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return size;
    TableView v;
    for (int attempt=0; attempt<4 && table_view(&v); ++attempt) {
        const size_t first=view_index(&v,address);
        uintptr_t cs=0, ce=0; int ck=0, cache=0;
        if (first<v.n && view_start(&v,first)<=address) { cs=view_start(&v,first); ce=view_end(&v,first); ck=view_kind(&v,first); cache=1; }
        uint64_t length=0;
        for (size_t i=first; i<v.n && length<size; ++i) {
            const uintptr_t s=view_start(&v,i), e=view_end(&v,i);
            if (s>address+length || view_kind(&v,i)==KIND_RESERVED || e<=address) break;
            length=e-address;
        }
        if (!table_view_valid(&v)) continue;
        if (cache && cs<ce) view_cache(&v,cs,ce,ck);
        return length<size ? length : size;
    }
    read_lock();

    { size_t i=vma_index(address); if (i<vma_count && vmas[i].start<=address) cache_region(i); }

    uint64_t length=0;

    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {

        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;

        length=vmas[i].end-address;

    }

    read_unlock();

    return length<size ? length : size;

}

/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */

int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    TableView v;
    for (int attempt=0; attempt<4 && table_view(&v); ++attempt) {
        const size_t i=view_index(&v,address);
        const int found=i<v.n;
        uintptr_t s=0, e=0; int m=0, here=0, k=0;
        if (found) {
            const uintptr_t is=view_start(&v,i);
            if (is<=address) { s=is; e=view_end(&v,i); k=view_kind(&v,i); m=k!=KIND_RESERVED; here=1; }
            else { s=i ? view_end(&v,i-1) : 0; e=is; m=0; }
        }
        if (!table_view_valid(&v)) continue;
        if (found) { *start=s; *end=e; *mapped=m; if (here && s<e) view_cache(&v,s,e,k); }
        return found;
    }
    read_lock();

    size_t i=vma_index(address);

    int found=i<vma_count;

    if (found && vmas[i].start<=address) {

        cache_region(i);

        *start=vmas[i].start; *end=vmas[i].end; *mapped=vmas[i].kind!=KIND_RESERVED;

    } else if (found) {

        *start=i ? vmas[i-1].end : 0; *end=vmas[i].start; *mapped=0;

    }

    read_unlock();

    return found;

}

/* GPU page tracking: restrict host access, never beyond the guest's own protection. */

void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {

    read_lock();

    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {

        if (vmas[i].kind==KIND_RESERVED) continue;

        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;

        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;

        int guest=host_prot(vmas[i].prot);

        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);

        host_protect(a,b-a,guest & want);

    }

    read_unlock();

}

/* Host view of a guest range, for the GPU backend: the range must be mapped. */

int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return 1;
    TableView v;
    for (int attempt=0; attempt<4 && table_view(&v); ++attempt) {
        /* covered(address,address+size,0) on the view */
        size_t i=view_index(&v,address); uintptr_t at=address; int result=1;
        while (at<address+size) {
            if (i>=v.n || view_start(&v,i)>at || view_kind(&v,i)==KIND_RESERVED) { result=0; break; }
            const uintptr_t e=view_end(&v,i);
            if (e<=at) { result=0; break; } /* torn read: retried below */
            at=e; ++i;
        }
        if (table_view_valid(&v)) return result;
    }
    read_lock();

    int result=covered(address,address+size,0);

    read_unlock();

    return result;

}

/* GPU cache reclamation: true if any live mapping remains, excluding a range

 * whose early-unmap notification is being handled before host unmapping. */

int runtime_memory_has_mapping_except(uintptr_t address, uint64_t size,

                                     uintptr_t excluded, uint64_t excluded_size) {

    if (address < USER_MIN) return 1; /* host-owned image/stack allocations */

    read_lock();

    int result=0;

    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {

        if (vmas[i].kind==KIND_RESERVED) continue;

        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;

        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;

        if (!excluded_size || a<excluded || b>excluded+excluded_size) { result=1; break; }

    }

    read_unlock();

    return result;

}

void runtime_memory_report(void) {

    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",

           allocations, maps, live_bytes, POOL_SIZE);

    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",

           flexible_maps, flexible_bytes, protects, queries, vma_count);

}



/* Diagnostic only: committed bytes, not the much larger virtual reservations.

 * Count shared physical storage through its canonical view once, and private

 * owners/shadows once each; guest aliases are intentionally not summed. */

void runtime_memory_report_committed(void) {

#ifdef _WIN32

    const ULONGLONG started=GetTickCount64();

    uint64_t shared=0,private_cpu_bytes=0,private_gpu_bytes=0,shadow=0;

    uint64_t private_chunks=0,shadow_chunks=0,query_count=0;

    read_lock();

    if(backing_base) {

        const uintptr_t end=(uintptr_t)backing_base+POOL_SIZE+FLEX_SPAN;

        for(uintptr_t at=(uintptr_t)backing_base;at<end;) {

            MEMORY_BASIC_INFORMATION info;

            if(!VirtualQuery((void*)at,&info,sizeof(info)))break;

            ++query_count;

            uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;if(next>end)next=end;

            if(next<=at)break;

            if(info.State==MEM_COMMIT)shared+=next-at;

            at=next;

        }

    }

    for(size_t n=0;n<region_count;++n) {

        const HostRegion *r=&regions[n];

        if(r->mapped!=2)continue;

        ++private_chunks;

        uint64_t committed=0;

        for(uintptr_t at=r->start;at<r->end;) {

            MEMORY_BASIC_INFORMATION info;

            if(!VirtualQuery((void*)at,&info,sizeof(info)))break;

            ++query_count;

            uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;if(next>r->end)next=r->end;

            if(next<=at)break;

            if(info.State==MEM_COMMIT)committed+=next-at;

            at=next;

        }

        const size_t v=vma_index(r->start);

        if(v<vma_count && vmas[v].start<=r->start && vmas[v].end>r->start && (vmas[v].prot&0x30))

            private_gpu_bytes+=committed;

        else private_cpu_bytes+=committed;

    }

    if(private_store)for(uint64_t n=0;n<(POOL_SIZE+FLEX_SPAN)/PRIVATE_CHUNK;++n) {

        if(!private_store[n])continue;

        ++shadow_chunks;

        const uintptr_t end=(uintptr_t)private_store[n]+PRIVATE_CHUNK;

        for(uintptr_t at=(uintptr_t)private_store[n];at<end;) {

            MEMORY_BASIC_INFORMATION info;

            if(!VirtualQuery((void*)at,&info,sizeof(info)))break;

            ++query_count;

            uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;if(next>end)next=end;

            if(next<=at)break;

            if(info.State==MEM_COMMIT)shadow+=next-at;

            at=next;

        }

    }

    printf("Guest committed: total=%llu KiB, private CPU=%llu KiB, private GPU-addressable=%llu KiB, shared physical=%llu KiB, unmapped preserved=%llu KiB; private chunks=%llu shadows=%llu; query count=%llu elapsed=%llu ms; tick_ms=%llu\n",

        (unsigned long long)((private_cpu_bytes+private_gpu_bytes+shared+shadow)>>10),

        (unsigned long long)(private_cpu_bytes>>10),(unsigned long long)(private_gpu_bytes>>10),

        (unsigned long long)(shared>>10),(unsigned long long)(shadow>>10),

        (unsigned long long)private_chunks,(unsigned long long)shadow_chunks,

        (unsigned long long)query_count,(unsigned long long)(GetTickCount64()-started),

        (unsigned long long)GetTickCount64());

    // Large guest mappings identify which engine heaps account for that total.

    for(size_t i=0;i<vma_count;++i)if(vmas[i].kind!=KIND_RESERVED && vmas[i].end-vmas[i].start>=128ull*1024*1024)

        printf("Guest heap: address=0x%llx size=%llu MiB physical=0x%llx kind=%d prot=0x%x type=%d\n",

            (unsigned long long)vmas[i].start,(unsigned long long)((vmas[i].end-vmas[i].start)>>20),

            (unsigned long long)vmas[i].phys,vmas[i].kind,vmas[i].prot,vmas[i].type);

    read_unlock();

#endif

}



/* Diagnostic only (BB_HOST_REPORT=1): where the process's committed memory is that the guest

 * report does not cover (on Xbox ~1.2 GB beside guest and GPU memory, growing ~20 MiB/min in

 * play). One walk of the whole address space: committed bytes by kind, the largest private

 * allocations and modules, the heaps, and the private commit no region shows (GPU memory). */

typedef struct { uintptr_t base; uint64_t committed, reserved; DWORD protect; } HostAlloc;

static void host_alloc_top(HostAlloc *top, size_t n, HostAlloc a) {

    if (!a.committed) return;

    for (size_t i=0;i<n;++i) if (a.committed>top[i].committed) {

        memmove(top+i+1,top+i,(n-i-1)*sizeof(*top));

        top[i]=a;

        return;

    }

}

void runtime_memory_report_host(void) {

#ifdef _WIN32

    enum { TOP=12 };

    const ULONGLONG started=GetTickCount64();

    const uintptr_t backing_lo=(uintptr_t)backing_base, backing_hi=backing_lo+POOL_SIZE+FLEX_SPAN;

    uint64_t guest=0, guest_views=0, backing=0, priv=0, mapped=0, image=0, image_writable=0, queries=0;

    uint64_t size_class[4]={0}, size_count[4]={0}; /* private allocations: <1, <16, <64, >=64 MiB */

    HostAlloc top_private[TOP]={0}, top_mapped[6]={0}, top_image[8]={0};

    HostAlloc cur={0}; DWORD cur_type=0;

    for (uintptr_t at=0x10000;;) {

        MEMORY_BASIC_INFORMATION m;

        const int more=at<UINT64_C(0x7ffffffe0000) && VirtualQuery((void*)at,&m,sizeof(m));

        if (!more) m.AllocationBase=NULL, m.Type=0;

        ++queries;

        const uintptr_t next=more ? (uintptr_t)m.BaseAddress+m.RegionSize : at;

        /* An allocation ends where the next region belongs to another (or the walk ends). */

        if (!more || (uintptr_t)m.AllocationBase!=cur.base || m.Type!=cur_type) {

            if (cur_type==MEM_PRIVATE && cur.base) {

                host_alloc_top(top_private,TOP,cur);

                const uint64_t mib=cur.committed>>20;

                const int c=mib<1 ? 0 : mib<16 ? 1 : mib<64 ? 2 : 3;

                if (cur.committed) { size_class[c]+=cur.committed; ++size_count[c]; }

            } else if (cur_type==MEM_MAPPED) host_alloc_top(top_mapped,6,cur);

            else if (cur_type==MEM_IMAGE) host_alloc_top(top_image,8,cur);

            if (!more || next<=at) break;

            cur=(HostAlloc){(uintptr_t)m.AllocationBase,0,0,m.AllocationProtect}; cur_type=m.Type;

        }

        if (next<=at) break;

        if (m.State!=MEM_FREE) cur.reserved+=next-at;

        if (m.State==MEM_COMMIT) {

            const uint64_t n=next-at;

            if (at>=USER_MIN && at<USER_MAX) { if (m.Type==MEM_PRIVATE) guest+=n; else guest_views+=n; }

            else if (backing_lo && at>=backing_lo && at<backing_hi) backing+=n;

            else {

                cur.committed+=n;

                if (m.Type==MEM_PRIVATE) priv+=n;

                else if (m.Type==MEM_MAPPED) mapped+=n;

                else {

                    image+=n;

                    if (m.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))

                        image_writable+=n;

                }

            }

        }

        at=next;

    }

    PROCESS_MEMORY_COUNTERS_EX pmc={.cb=sizeof(pmc)};

    K32GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&pmc,sizeof(pmc));

    /* Private commit no committed private region accounts for (D3D12 memory not mapped here). */

    const uint64_t visible=priv+image_writable+guest;

    printf("Host memory: process private %llu MiB = guest private %llu + host private %llu + image writable %llu"

           " + not in any region %llu MiB; mapped views %llu MiB, images %llu MiB, guest views %llu MiB,"

           " guest backing view %llu MiB;"

           " %llu queries, %llu ms\n",

           (unsigned long long)(pmc.PrivateUsage>>20),(unsigned long long)(guest>>20),(unsigned long long)(priv>>20),

           (unsigned long long)(image_writable>>20),

           (unsigned long long)(pmc.PrivateUsage>visible ? (pmc.PrivateUsage-visible)>>20 : 0),

           (unsigned long long)(mapped>>20),(unsigned long long)(image>>20),(unsigned long long)(guest_views>>20),

           (unsigned long long)(backing>>20),

           (unsigned long long)queries,(unsigned long long)(GetTickCount64()-started));

    printf("Host memory: private allocations <1 MiB %llu (%llu MiB), 1-16 MiB %llu (%llu MiB), 16-64 MiB %llu (%llu MiB), >=64 MiB %llu (%llu MiB)\n",

           (unsigned long long)size_count[0],(unsigned long long)(size_class[0]>>20),

           (unsigned long long)size_count[1],(unsigned long long)(size_class[1]>>20),

           (unsigned long long)size_count[2],(unsigned long long)(size_class[2]>>20),

           (unsigned long long)size_count[3],(unsigned long long)(size_class[3]>>20));

    for (size_t i=0;i<TOP && top_private[i].committed;++i)

        printf("Host memory: private %p committed %llu MiB of %llu MiB, protect 0x%lx\n",(void*)top_private[i].base,

               (unsigned long long)(top_private[i].committed>>20),(unsigned long long)(top_private[i].reserved>>20),

               top_private[i].protect);

    for (size_t i=0;i<6 && top_mapped[i].committed;++i)

        printf("Host memory: mapped view %p committed %llu MiB of %llu MiB\n",(void*)top_mapped[i].base,

               (unsigned long long)(top_mapped[i].committed>>20),(unsigned long long)(top_mapped[i].reserved>>20));

    for (size_t i=0;i<8 && top_image[i].committed;++i) {

        char name[MAX_PATH]="?";

        GetModuleFileNameA((HMODULE)top_image[i].base,name,sizeof(name));

        const char *slash=strrchr(name,'\\');

        printf("Host memory: image %s committed %llu MiB\n",slash ? slash+1 : name,

               (unsigned long long)(top_image[i].committed>>20));

    }

    /* Not exported on Xbox (the DLL failed to load importing them): looked up, else skipped. */

    typedef DWORD (WINAPI *GetHeapsFn)(DWORD,PHANDLE);

    typedef BOOL (WINAPI *HeapSummaryFn)(HANDLE,DWORD,LPHEAP_SUMMARY);

    HMODULE kernel=GetModuleHandleW(L"kernelbase.dll");

    if (!kernel) kernel=GetModuleHandleW(L"kernel32.dll");

    GetHeapsFn get_heaps=kernel ? (GetHeapsFn)(void*)GetProcAddress(kernel,"GetProcessHeaps") : NULL;

    HeapSummaryFn heap_summary=kernel ? (HeapSummaryFn)(void*)GetProcAddress(kernel,"HeapSummary") : NULL;

    HANDLE heaps[64];

    const DWORD count=get_heaps && heap_summary ? get_heaps(64,heaps) : 0;

    if (!count) printf("Host memory: heap summaries unavailable (GetProcessHeaps %p, HeapSummary %p)\n",

                       (void*)get_heaps,(void*)heap_summary);

    uint64_t heap_committed=0, heap_allocated=0;

    for (DWORD i=0;i<count && i<64;++i) {

        HEAP_SUMMARY s={.cb=sizeof(s)};

        if (!heap_summary(heaps[i],0,&s)) continue;

        heap_committed+=s.cbCommitted; heap_allocated+=s.cbAllocated;

        if (s.cbCommitted>=(16u<<20))

            printf("Host memory: heap %p%s allocated %llu MiB, committed %llu MiB, reserved %llu MiB\n",

                   heaps[i],heaps[i]==GetProcessHeap() ? " (process heap)" : "",

                   (unsigned long long)(s.cbAllocated>>20),(unsigned long long)(s.cbCommitted>>20),

                   (unsigned long long)(s.cbReserved>>20));

    }

    printf("Host memory: %lu heaps, allocated %llu MiB, committed %llu MiB\n",count,

           (unsigned long long)(heap_allocated>>20),(unsigned long long)(heap_committed>>20));

    fflush(stdout);

#endif

}





/* Diagnostic only (BB_COLD_REPORT=1): how much of the guest's private memory nobody touches, the

 * memory a pager could keep in a file (Kyty's Xbox pager does: 64 KiB windows untouched for a

 * second go to a file). Each second a thread drops the next 16 MiB of private chunks from the

 * working set (VirtualUnlock: contents and protections stay; a touch is a soft fault that brings

 * the page back), and QueryWorkingSetEx tells 10 s and 60 s later which pages are still out:

 * untouched since. Read-only for the guest: nothing is decommitted or protected. */

typedef BOOL (WINAPI *VirtualUnlockFn)(LPVOID,SIZE_T);

typedef BOOL (WINAPI *QueryWorkingSetExFn)(HANDLE,PVOID,DWORD);

typedef struct { uintptr_t start; ULONGLONG trimmed_ms; uint16_t committed; uint8_t checked; } ColdSample;

enum { COLD_SAMPLES=65536, COLD_BATCH=256 };

static ColdSample cold_samples[COLD_SAMPLES];

static size_t cold_head, cold_tail; /* samples [tail, head) are waiting for their checks */

static uint64_t cold_sampled[2], cold_untouched[2]; /* bytes, at 10 s and at 60 s */

static uint16_t cold_pages_out(QueryWorkingSetExFn query, uintptr_t start, uint16_t committed) {

    PSAPI_WORKING_SET_EX_INFORMATION info[16];

    for (int p=0;p<16;++p) { info[p].VirtualAddress=(void*)(start+(uintptr_t)p*4096); info[p].VirtualAttributes.Flags=0; }

    if (!query(GetCurrentProcess(),info,sizeof(info))) return 0;

    uint16_t out=0;

    for (int p=0;p<16;++p) if ((committed>>p)&1 && !(info[p].VirtualAttributes.Flags&1)) out|=(uint16_t)(1u<<p);

    return out;

}

static DWORD WINAPI cold_sampler(void *unused) {

    (void)unused;

    HMODULE kernel=GetModuleHandleW(L"kernelbase.dll");

    if (!kernel) kernel=GetModuleHandleW(L"kernel32.dll");

    VirtualUnlockFn unlock=kernel ? (VirtualUnlockFn)(void*)GetProcAddress(kernel,"VirtualUnlock") : NULL;

    QueryWorkingSetExFn query=kernel ? (QueryWorkingSetExFn)(void*)GetProcAddress(kernel,"QueryWorkingSetEx") : NULL;

    if (!query && kernel) query=(QueryWorkingSetExFn)(void*)GetProcAddress(kernel,"K32QueryWorkingSetEx");

    if (!unlock || !query) {

        printf("Guest cold memory: unavailable (VirtualUnlock %p, QueryWorkingSetEx %p)\n",(void*)unlock,(void*)query);

        fflush(stdout);

        return 0;

    }

    uintptr_t hand=0;

    for (;;) {

        Sleep(1000);

        const ULONGLONG now=GetTickCount64();

        /* the next private chunks, from where the last batch ended */

        uintptr_t batch[COLD_BATCH]; size_t picked=0;

        read_lock();

        for (size_t i=region_index(hand); i<region_count && picked<COLD_BATCH; ++i)

            if (regions[i].mapped==2) batch[picked++]=regions[i].start;

        read_unlock();

        hand=picked==COLD_BATCH ? batch[picked-1]+PRIVATE_CHUNK : 0;

        for (size_t n=0; n<picked && (cold_head+1)%COLD_SAMPLES!=cold_tail; ++n) {

            uint16_t committed=0;

            for (uintptr_t at=batch[n]; at<batch[n]+PRIVATE_CHUNK;) {

                MEMORY_BASIC_INFORMATION m;

                if (!VirtualQuery((void*)at,&m,sizeof(m))) break;

                uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;

                if (next>batch[n]+PRIVATE_CHUNK) next=batch[n]+PRIVATE_CHUNK;

                if (next<=at) break;

                if (m.State==MEM_COMMIT) {

                    for (uintptr_t p=at; p<next; p+=4096) committed|=(uint16_t)(1u<<((p-batch[n])>>12));

                    unlock((void*)at,next-at); /* fails with ERROR_NOT_LOCKED, and trims */

                }

                at=next;

            }

            if (committed) cold_samples[cold_head++%COLD_SAMPLES]=(ColdSample){batch[n],now,committed,0};

            cold_head%=COLD_SAMPLES;

        }

        /* checks: at 10 s (all samples) and at 60 s (the ones untouched at 10 s) */

        for (size_t at=cold_tail; at!=cold_head; at=(at+1)%COLD_SAMPLES) {

            ColdSample *s=&cold_samples[at];

            const ULONGLONG age=now-s->trimmed_ms;

            if (s->checked==0 && age>=10000) {

                const uint16_t out=cold_pages_out(query,s->start,s->committed);

                cold_sampled[0]+=(uint64_t)__builtin_popcount(s->committed)*4096;

                cold_untouched[0]+=(uint64_t)__builtin_popcount(out)*4096;

                s->committed=out; s->checked=1; /* only what stayed out goes on to 60 s */

            } else if (s->checked==1 && age>=60000) {

                const uint16_t out=cold_pages_out(query,s->start,s->committed);

                cold_sampled[1]+=(uint64_t)__builtin_popcount(s->committed)*4096;

                cold_untouched[1]+=(uint64_t)__builtin_popcount(out)*4096;

                s->checked=2;

            }

        }

        while (cold_tail!=cold_head && (cold_samples[cold_tail].checked==2 ||

               (cold_samples[cold_tail].checked==1 && !cold_samples[cold_tail].committed)))

            cold_tail=(cold_tail+1)%COLD_SAMPLES;

    }

}

void runtime_memory_report_cold(void) {

    static int started;

    if (!started) {

        started=1;

        HANDLE thread=CreateThread(NULL,0,cold_sampler,NULL,0,NULL);

        if (thread) CloseHandle(thread);

        printf("Guest cold memory: sampling 16 MiB of private guest memory a second\n");

        return;

    }

    /* at 60 s: the share still out of what was out at 10 s (those checked at 60 s so far) */

    const double at10=cold_sampled[0] ? 100.0*(double)cold_untouched[0]/(double)cold_sampled[0] : 0;

    const double at60=cold_sampled[1] ? at10*(double)cold_untouched[1]/(double)cold_sampled[1] : 0;

    printf("Guest cold memory: of %llu MiB sampled, untouched after 10 s %.1f%%, after 60 s %.1f%% (%llu MiB checked at 60 s)\n",

           (unsigned long long)(cold_sampled[0]>>20),at10,at60,(unsigned long long)(cold_sampled[1]>>20));

}



/* ---- The pager (2026-10-31; after Kyty's Xbox pager, ps5-shellui-uwp b7ff56f3) ----

 * The app's memory is capped (Xbox UWP: 5 GiB) and the guest fills its 4.9 GB pool as it plays,

 * though most of what it holds sits untouched: of the private chunks, ~93% untouched for 10 s and

 * ~84% for 60 s on a walk (BB_COLD_REPORT). Above BB_PAGER_HIGH_MB of app memory the pager sweeps

 * private chunks (64 KiB, mapped==2): their committed pages become no-access; a chunk untouched

 * for PAGER_DWELL_MS is made read-only, copied to a file (LocalState user dir, unbuffered), and

 * released to a placeholder. A touch is a fault: a swept chunk gets its protections back; a chunk

 * in the file comes back, on a helper thread (the faulting thread's stack is the guest's), as a

 * view of a section of its own, filled before it is mapped (a chunk committed and then filled

 * reads as zeros for a thread that does not fault: Kyty's runs ended within minutes that way).

 * Not paged: chunks with no-access or other protections (GPU read watches), fiber stacks

 * (pinned for good: a fault there could not be dispatched), buffers of system I/O while it runs.

 * Locks: the region lock, then pager_lock. The fault handler takes pager_lock only; nothing

 * under pager_lock touches guest memory that could fault. */

#define PG_SWEPT 1

#define PG_EVICTING 2

#define PG_EVICTED 3

#define PG_LEAF_BITS 12

#define PG_PIN_FOREVER 255

#define PAGER_DWELL_MS 5000

/* Pace (2026-10-31): 4096 chunks swept and paged out in one go during an area load froze the game

 * ~15 s (every protection change interrupts all cores running the game, and the writes compete

 * with the load). Per 200 ms loop: 24 chunks out (1.5 MiB, ~7.5 MiB/s) and up to 384 swept; near

 * the limit (PAGER_RUSH_MB below it) as fast as before.

 * Even (2026-10-31): sweeps of 384 still came in bursts (only when the queue was empty) and began at

 * 4020 MiB, which play reaches during area loads. Now a 100 ms loop sweeps and pages out at most 12

 * chunks each (same ~7.5 MiB/s, small steps), sweeps while fewer are queued than needed, and starts

 * at limit - 1400 MiB (3720 of 5120) so the work happens in calm play before a load. */

#define PAGER_LOOP_MS 100

#define PAGER_OUT_PER_LOOP 12

#define PAGER_SWEEP_PER_LOOP 12

#define PAGER_RUSH_MB 500

#define PAGER_URGENT_MB 700

#define PAGER_FAST_MB 1000

#define PAGER_CLIMB_MB 200

#define PAGER_SCAN_PER_LOOP 2048

#define PAGER_SCAN_BACKOFF_MS 5000

static volatile int pager_urgent; /* pager_thread: near the limit, the cooldown does not hold */

/* Thrash (2026-10-31): at 3720 MiB with a 5 s dwell, a large area (working set above 3720) paged

 * 2 GB out and 900 MiB back in one session, 500-1000 page-ins per 2 s in the slowest stretches

 * (5 s windows at 7-15 FPS). Back to limit - 1100 (4020 of 5120); a chunk must stay untouched for

 * PAGER_CALM_DWELL_MS (30 s; ~84% of private memory is untouched for 60 s) unless the app is

 * within PAGER_RUSH_MB of the limit (then PAGER_DWELL_MS); and a chunk that came back or was

 * touched while swept is left alone for PAGER_COOLDOWN_S. BB_PAGER_DWELL_S overrides the 30 s. */

#define PAGER_HIGH_BELOW_LIMIT_MB 1100

#define PAGER_CALM_DWELL_MS 30000

#define PAGER_COOLDOWN_S 120

#define PAGER_URGENT_COOLDOWN_S 30

typedef struct { uint32_t slot; uint8_t state, view, pinned, init, group4; uint8_t prot[16]; uint32_t used_at; } PagerEntry; /* prot: 0 none, 1 RO, 2 RW; used_at: s, last time it came back or was touched swept */

static uint32_t pager_now_s(void) { return (uint32_t)(GetTickCount64()/1000); }

static PagerEntry *pager_leaves[(((USER_MAX-USER_MIN)>>16)>>PG_LEAF_BITS)+1];

static SRWLOCK pager_lock=SRWLOCK_INIT;

static __thread int pager_holding;

static HANDLE pager_file=INVALID_HANDLE_VALUE;

static uint32_t *pager_free; static size_t pager_free_count, pager_free_capacity; static uint32_t pager_next_slot;

static uint64_t pager_limit, pager_high, pager_low;

static volatile LONG64 pager_views, pager_in_file, pager_evicted, pager_page_ins, pager_page_in_us, pager_aborted, pager_swept,

                       pager_touched, pager_released, pager_failed;

static volatile LONG64 pager_stacks, pager_stack_bytes; /* fiber stacks pinned */

static void pager_take(void) { AcquireSRWLockExclusive(&pager_lock); pager_holding=1; }

static void pager_give(void) { pager_holding=0; ReleaseSRWLockExclusive(&pager_lock); }

static PagerEntry *pager_entry(uintptr_t chunk, int create) {

    if (chunk<USER_MIN || chunk>=USER_MAX) return NULL;

    const uint64_t n=(chunk-USER_MIN)>>16;

    PagerEntry **slot=&pager_leaves[n>>PG_LEAF_BITS];

    PagerEntry *leaf=__atomic_load_n(slot,__ATOMIC_ACQUIRE);

    if (!leaf) {

        if (!create) return NULL;

        leaf=VirtualAlloc(NULL,sizeof(PagerEntry)<<PG_LEAF_BITS,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);

        if (!leaf) return NULL;

        PagerEntry *expected=NULL;

        if (!__atomic_compare_exchange_n(slot,&expected,leaf,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {

            VirtualFree(leaf,0,MEM_RELEASE);

            leaf=expected;

        }

    }

    return &leaf[n&((1u<<PG_LEAF_BITS)-1)];

}

static DWORD pager_protect_of(uint8_t code) { return code==1 ? PAGE_READONLY : PAGE_READWRITE; }

/* The pages a sweep made no-access (or an eviction read-only) get their protections back. */

/* Spider-Man port (2026-11-05): a 64 KiB chunk of four 16 KiB private pieces (type 4, one allocation each,
   same initial protection) is paged as one chunk. VirtualProtect cannot span allocations: runs of pages
   stop at piece boundaries (pager_run_end). */
static int pager_group4(size_t i) {

    if (i+3>=region_count) return 0;

    const uintptr_t chunk=regions[i].start;

    if (chunk%PRIVATE_CHUNK || regions[i].mapped!=4) return 0;

    for (int k=0;k<4;++k) {

        const HostRegion *r=&regions[i+k];

        if (r->mapped!=4 || r->start!=chunk+(uintptr_t)k*SMALL_CHUNK || r->end!=r->start+SMALL_CHUNK ||
            r->initial_protect!=regions[i].initial_protect) return 0;

    }

    return 1;

}

static int pager_candidate(size_t i) { return regions[i].mapped==2 || pager_group4(i); }

static int pager_run_end(const PagerEntry *e, int p, int q) {

    if (e->group4) { const int limit=(p/4+1)*4; if (q>limit) q=limit; }

    return q;

}

static void pager_restore(uintptr_t chunk, PagerEntry *e) {

    e->used_at=pager_now_s(); /* in use (or let go): not swept again for PAGER_COOLDOWN_S */

    for (int p=0;p<16;) {

        const uint8_t code=e->prot[p];

        int q=p+1;

        while (q<16 && e->prot[q]==code) ++q;

        q=pager_run_end(e,p,q);

        DWORD old;

        if (code && !VirtualProtect((void*)(chunk+(uintptr_t)p*4096),(SIZE_T)(q-p)*4096,pager_protect_of(code),&old))

            win_fail("pager: restoring a swept chunk",chunk+(uintptr_t)p*4096,(uint64_t)(q-p)*4096);

        p=q;

    }

    e->state=0;

}

/* Buffered: an unbuffered read locks its destination for the transfer, and near the app's limit

 * that failed (ERROR_WORKING_SET_QUOTA, 1453: the game stopped, 2026-10-31). A read that fails

 * is tried again (a chunk that cannot come back ends the game). */

static volatile LONG64 pager_io_retries;

static int pager_slot_io(uint32_t slot, void *buffer, int write) {

    for (int attempt=0;;++attempt) {

        OVERLAPPED at={0};

        const uint64_t offset=(uint64_t)slot*PRIVATE_CHUNK;

        at.Offset=(DWORD)offset; at.OffsetHigh=(DWORD)(offset>>32);

        DWORD n=0;

        const BOOL ok=write ? WriteFile(pager_file,buffer,(DWORD)PRIVATE_CHUNK,&n,&at) : ReadFile(pager_file,buffer,(DWORD)PRIVATE_CHUNK,&n,&at);

        if (ok && n==PRIVATE_CHUNK) return 1;

        const DWORD error=ok ? ERROR_HANDLE_EOF : GetLastError();

        if (write || attempt>=200) {

            fprintf(stderr,"Pager: %s of slot %u failed, error %lu (attempt %d)\n",write ? "write" : "read",slot,error,attempt+1);

            fflush(stderr);

            return 0;

        }

        if (_InterlockedExchangeAdd64(&pager_io_retries,1)<16) {

            fprintf(stderr,"Pager: read of slot %u failed, error %lu: again\n",slot,error);

            fflush(stderr);

        }

        Sleep(attempt<20 ? 1 : 10);

    }

}

/* pager_lock held. */

static void pager_free_slot(uint32_t slot) {

    if (pager_free_count==pager_free_capacity) {

        pager_free_capacity=pager_free_capacity ? pager_free_capacity*2 : 4096;

        pager_free=realloc(pager_free,pager_free_capacity*sizeof(*pager_free));

    }

    pager_free[pager_free_count++]=slot;

    _InterlockedExchangeAdd64(&pager_in_file,-1);

}

static uint32_t pager_alloc_slot(void) {

    _InterlockedExchangeAdd64(&pager_in_file,1);

    return pager_free_count ? pager_free[--pager_free_count] : pager_next_slot++;

}

/* God of War port (2026-11-03): an all-zero chunk leaves without a file slot (nothing written) and

 * comes back as zeros. BB_PAGER_ZERO=0 writes it out like any other. */

#define PAGER_ZERO_SLOT 0xffffffffu

static volatile LONG64 pager_zero_out;

static int pager_zero_chunks(void) {

    static int on=-1;

    if (on<0) {

        const char *z=getenv("BB_PAGER_ZERO"), *v=getenv("BB_PAGER_VIEWS");

        on=!(z && z[0]=='0') && !(v && v[0]=='1'); /* the section-view page-in reads its slot */

    }

    return on;

}

static int pager_is_zero(const unsigned char *data) {

    const uint64_t *w=(const uint64_t *)data;

    for (size_t i=0;i<PRIVATE_CHUNK/8;++i) if (w[i]) return 0;

    return 1;

}

/* Checksums of what each slot holds: a chunk that comes back different is said (and counted). */

static uint64_t *pager_sums; static size_t pager_sums_capacity;

static volatile LONG64 pager_corrupt, pager_fill_diffs;

static uint64_t pager_checksum(const void *data) {

    const uint64_t *w=data; uint64_t sum=0x9E3779B97F4A7C15ull;

    for (size_t i=0;i<PRIVATE_CHUNK/8;++i) sum=(sum^w[i])*0x100000001B3ull;

    return sum;

}

/* pager_lock held. */

static void pager_set_sum(uint32_t slot, uint64_t sum) {

    if (slot>=pager_sums_capacity) {

        size_t capacity=pager_sums_capacity ? pager_sums_capacity : 65536;

        while (capacity<=slot) capacity*=2;

        pager_sums=realloc(pager_sums,capacity*sizeof(*pager_sums));

        memset(pager_sums+pager_sums_capacity,0,(capacity-pager_sums_capacity)*sizeof(*pager_sums));

        pager_sums_capacity=capacity;

    }

    pager_sums[slot]=sum;

}

/* A chunk in the file comes back as a view of a section of its own, filled first. pager_lock held. */

/* Private page-in (2026-10-31 crash): a chunk used to come back as a view of a 64 KiB section of

 * its own. Section views count as shared commit, which the Xbox caps near 0.9 GiB (probe: views

 * failed with error 8 at 896 MiB); the game's own shared memory in a large area plus ~3600 such

 * views reached it ("view to fill a chunk coming back failed ... error 8"). Now the chunk comes

 * back as private memory (like before it went out): read into a private buffer under pager_lock,

 * committed in place read-write with guard pages (another thread's touch stops at a guard and

 * waits for pager_lock, as before), copied run by run, and each run set to its protection right

 * after its copy. The copy's own guard faults are passed (pager_filling). */

/* God of War port (2026-11-04): volatile, with signal fences around the copy: a plain thread-local

 * whose address is never taken may be stored after the memcpy it brackets; the copy's own guard

 * fault then found it 0 and was fatal (Xbox boot31: exception 0x80000001 in ucrtbase memcpy during

 * the first world load's paging). */

static __thread volatile int pager_filling;

static unsigned char *pager_fill_buffer;

/* 1 when the chunk is back (here or by another page-in), 0 when the commit failed at the memory

 * limit and room could not be made from this thread (the table is held elsewhere: see

 * try_read_lock); the caller makes room itself and tries again. */

/* The allocation a chunk comes back into: one 64 KiB private allocation, or four 16 KiB pieces (group4). */
static void *pager_alloc_back(uintptr_t chunk, const PagerEntry *e, DWORD protect) {

    if (!e->group4) return VirtualAlloc2(NULL,(void*)chunk,PRIVATE_CHUNK,MEM_RESERVE|MEM_COMMIT|MEM_REPLACE_PLACEHOLDER,protect,NULL,0);

    for (int k=0;k<4;++k) {

        if (!VirtualAlloc2(NULL,(void*)(chunk+(uintptr_t)k*SMALL_CHUNK),SMALL_CHUNK,MEM_RESERVE|MEM_COMMIT|MEM_REPLACE_PLACEHOLDER,protect,NULL,0)) {

            const DWORD error=GetLastError();

            for (int j=0;j<k;++j) VirtualFree((void*)(chunk+(uintptr_t)j*SMALL_CHUNK),SMALL_CHUNK,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER);

            SetLastError(error);

            return NULL;

        }

    }

    return (void*)chunk;

}

static int pager_page_in_private(uintptr_t chunk, PagerEntry *e, ULONGLONG began) {

    if (!pager_fill_buffer) {

        pager_fill_buffer=VirtualAlloc(NULL,PRIVATE_CHUNK,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);

        if (!pager_fill_buffer) win_fail("pager: buffer to fill a chunk coming back",chunk,PRIVATE_CHUNK);

    }

    /* At the memory limit this commit fails like any other (PC 2026-11-02, a 1 GB test balloon:

     * "committing a chunk coming back failed ... error 1455" right after a make-room): room is made

     * with pager_lock let go, and the commit tried again. The chunk may have come back meanwhile

     * (another page-in), and the fill buffer is shared: it is read again after such a wait.

     * Room is made only if the table can be had without waiting: on the page-in helper, the thread

     * that asked for this page-in may hold it while it waits.

     * The file is read BEFORE the commit (2026-11-02 Xbox crashes 6 and 7, guest null and -1 reads):

     * a guard page stops one touch only. A second thread that touched the chunk during the fill took

     * that page's guard and waited, and a third then read the still-empty page without a fault. With

     * the 15 ms read between commit and copy (roomfix order) that window was there every page-in. */

    unsigned char *fill=pager_fill_buffer;

    int filled=0;

    static int no_guard;

    void *at=NULL;

    for (int attempt=0;;++attempt) {

        if (!filled && e->slot==PAGER_ZERO_SLOT) {

            memset(fill,0,PRIVATE_CHUNK);

            filled=1;

        } else if (!filled) {

            if (!pager_slot_io(e->slot,fill,0)) win_fail("pager: reading a chunk back from the file",chunk,PRIVATE_CHUNK);

            if (e->slot<pager_sums_capacity && pager_sums[e->slot] && pager_checksum(fill)!=pager_sums[e->slot]) {

                if (_InterlockedExchangeAdd64(&pager_corrupt,1)<16) {

                    fprintf(stderr,"Pager: the chunk at %p came back different from what went out (slot %u)\n",(void*)chunk,e->slot);

                    fflush(stderr);

                }

            }

            filled=1;

        }

        at=no_guard ? NULL : pager_alloc_back(chunk,e,PAGE_READWRITE|PAGE_GUARD);

        DWORD error=at ? 0 : GetLastError();

        const int at_limit=error==ERROR_COMMITMENT_LIMIT || error==ERROR_NOT_ENOUGH_MEMORY || error==ERROR_WORKING_SET_QUOTA;

        if (!at && !no_guard && !at_limit) {

            no_guard=1;

            fprintf(stderr,"Pager: guarded private memory was refused (error %lu): chunks come back read-write, then protected\n",error);

            fflush(stderr);

        }

        if (!at && no_guard) {

            at=pager_alloc_back(chunk,e,PAGE_READWRITE);

            error=at ? 0 : GetLastError();

        }

        if (at==(void*)chunk) break;

        if (!(error==ERROR_COMMITMENT_LIMIT || error==ERROR_NOT_ENOUGH_MEMORY || error==ERROR_WORKING_SET_QUOTA) || attempt>=8)

            win_fail("pager: committing a chunk coming back",chunk,PRIVATE_CHUNK);

        pager_give();

        uint64_t made=0;

        const int table=try_read_lock();

        if (table) { made=pager_make_room(64); read_unlock(); }

        pager_take();

        if (e->state!=PG_EVICTED) return 1; /* brought back while the lock was let go */

        if (!table) return 0;

        if (!made && attempt>=2) win_fail("pager: committing a chunk coming back (no room)",chunk,PRIVATE_CHUNK);

        filled=0; /* another page-in may have used the fill buffer meanwhile */

    }

    pager_filling=1;

    __atomic_signal_fence(__ATOMIC_SEQ_CST);

    for (int p=0;p<16;) {

        const uint8_t code=e->prot[p] ? e->prot[p] : e->init;

        int q=p+1;

        while (q<16 && (e->prot[q] ? e->prot[q] : e->init)==code) ++q;

        q=pager_run_end(e,p,q);

        memcpy((void*)(chunk+(uintptr_t)p*4096),fill+(size_t)p*4096,(size_t)(q-p)*4096);

        DWORD old;

        if ((code==1 || !no_guard) &&

            !VirtualProtect((void*)(chunk+(uintptr_t)p*4096),(SIZE_T)(q-p)*4096,code==1 ? PAGE_READONLY : PAGE_READWRITE,&old))

            win_fail("pager: protecting a chunk that came back",chunk,PRIVATE_CHUNK);

        p=q;

    }

    __atomic_signal_fence(__ATOMIC_SEQ_CST);

    pager_filling=0;

    /* Checked (2026-11-03, Xbox crash: a guest read a stale pointer out of a chunk paged in 6 ms

     * before): the chunk now holds what the file gave back. A page another thread wrote after its

     * fill would also differ, so the line says which page and the event ring keeps the time. */

    if (memcmp((void*)chunk,fill,PRIVATE_CHUNK)) {

        int page=0;

        while (page<16 && !memcmp((void*)(chunk+(uintptr_t)page*4096),fill+(size_t)page*4096,4096)) ++page;

        runtime_trace_event(TR_PAGE_IN_DIFF,chunk,(uint64_t)page,NULL);

        if (_InterlockedExchangeAdd64(&pager_fill_diffs,1)<16) {

            fprintf(stderr,"Pager: the chunk at %p differs from its file copy after the page-in, from page %d\n",(void*)chunk,page);

            fflush(stderr);

        }

    }

    if (e->slot!=PAGER_ZERO_SLOT) pager_free_slot(e->slot);

    e->state=0; e->view=0;

    e->used_at=pager_now_s();

    runtime_trace_event(TR_PAGE_IN,chunk,GetTickCount64()-began,NULL);

    _InterlockedExchangeAdd64(&pager_page_ins,1);

    const ULONGLONG took=GetTickCount64()-began;

    _InterlockedExchangeAdd64(&pager_page_in_us,(LONG64)(took*1000));

    if (took>=50) { fprintf(stderr,"Pager: a chunk at %p took %llu ms to come back\n",(void*)chunk,(unsigned long long)took); fflush(stderr); }

    return 1;

}

/* 1 when the chunk is back; 0: see pager_page_in_private. pager_lock held. */

static int pager_page_in(uintptr_t chunk, PagerEntry *e) {

    const ULONGLONG began=GetTickCount64();

    /* BB_PAGER_VIEWS=1: the section-view page-in below (the old way) */

    static int views=-1;

    if (views<0) { const char *v=getenv("BB_PAGER_VIEWS"); views=v && v[0]=='1'; }

    if (!views) return pager_page_in_private(chunk,e,began);

    HANDLE own=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_READWRITE,SEC_COMMIT,PRIVATE_CHUNK,NULL,NULL,0);

    if (!own) win_fail("pager: section for a chunk coming back",chunk,PRIVATE_CHUNK);

    void *fill=MapViewOfFile3(own,GetCurrentProcess(),NULL,0,PRIVATE_CHUNK,0,PAGE_READWRITE,NULL,0);

    if (!fill) win_fail("pager: view to fill a chunk coming back",chunk,PRIVATE_CHUNK);

    if (!pager_slot_io(e->slot,fill,0)) win_fail("pager: reading a chunk back from the file",chunk,PRIVATE_CHUNK);

    if (e->slot<pager_sums_capacity && pager_sums[e->slot] && pager_checksum(fill)!=pager_sums[e->slot]) {

        if (_InterlockedExchangeAdd64(&pager_corrupt,1)<16) {

            fprintf(stderr,"Pager: the chunk at %p came back different from what went out (slot %u)\n",(void*)chunk,e->slot);

            fflush(stderr);

        }

    }

    UnmapViewOfFile2(GetCurrentProcess(),fill,0);

    /* Read-write (a view mapped read-only cannot be made writable: error 87 on the Xbox), as guard

     * pages: a thread that touches the chunk before its protections hold (read-only pages are GPU

     * write watches: a write there must fault) stops at the guard and waits for pager_lock. Setting

     * the protections below clears the guards. */

    static int no_guard;

    void *view=no_guard ? NULL : MapViewOfFile3(own,GetCurrentProcess(),(void*)chunk,0,PRIVATE_CHUNK,MEM_REPLACE_PLACEHOLDER,

                                                PAGE_READWRITE|PAGE_GUARD,NULL,0);

    if (!view && !no_guard) {

        no_guard=1;

        fprintf(stderr,"Pager: a guarded view was refused (error %lu): chunks come back read-write, then protected\n",GetLastError());

        fflush(stderr);

    }

    if (!view) view=MapViewOfFile3(own,GetCurrentProcess(),(void*)chunk,0,PRIVATE_CHUNK,MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0);

    CloseHandle(own); /* the view keeps it */

    if (view!=(void*)chunk) win_fail("pager: mapping a chunk back",chunk,PRIVATE_CHUNK);

    for (int p=0;p<16;) {

        const uint8_t code=e->prot[p] ? e->prot[p] : e->init;

        int q=p+1;

        while (q<16 && (e->prot[q] ? e->prot[q] : e->init)==code) ++q;

        DWORD old;

        if ((code==1 || !no_guard) &&

            !VirtualProtect((void*)(chunk+(uintptr_t)p*4096),(SIZE_T)(q-p)*4096,code==1 ? PAGE_READONLY : PAGE_READWRITE,&old))

            win_fail("pager: protecting a chunk that came back",chunk,PRIVATE_CHUNK);

        p=q;

    }

    pager_free_slot(e->slot);

    e->state=0; e->view=1;

    e->used_at=pager_now_s();

    _InterlockedExchangeAdd64(&pager_views,1);

    _InterlockedExchangeAdd64(&pager_page_ins,1);

    const ULONGLONG took=GetTickCount64()-began;

    _InterlockedExchangeAdd64(&pager_page_in_us,(LONG64)(took*1000));

    if (took>=50) { fprintf(stderr,"Pager: a chunk at %p took %llu ms to come back\n",(void*)chunk,(unsigned long long)took); fflush(stderr); }

    return 1;

}



/* Page-ins run here for faults and for bring-backs on small stacks: the asking thread only waits. */

typedef struct PageInRequest { uintptr_t chunk; int done, result; struct PageInRequest *next; } PageInRequest;

static pthread_mutex_t pagein_mutex=PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t pagein_wake=PTHREAD_COND_INITIALIZER, pagein_done=PTHREAD_COND_INITIALIZER;

static PageInRequest *pagein_queue;

static DWORD WINAPI pagein_thread(void *unused) {

    (void)unused;

    for (;;) {

        pthread_mutex_lock(&pagein_mutex);

        while (!pagein_queue) pthread_cond_wait(&pagein_wake,&pagein_mutex);

        PageInRequest *request=pagein_queue;

        pagein_queue=request->next;

        pthread_mutex_unlock(&pagein_mutex);

        pager_take();

        PagerEntry *e=pager_entry(request->chunk,0);

        int result=1;

        if (e && e->state==PG_EVICTED) result=pager_page_in(request->chunk,e);

        else if (e && e->state) { pager_restore(request->chunk,e); _InterlockedExchangeAdd64(&pager_touched,1); }

        pager_give();

        pthread_mutex_lock(&pagein_mutex);

        request->result=result;

        request->done=1;

        pthread_cond_broadcast(&pagein_done);

        pthread_mutex_unlock(&pagein_mutex);

    }

}

/* A chunk back by the helper (pager_lock not held). When its commit hit the memory limit and the

 * helper could not make room (the table was held, maybe by this thread), room is made here. */

static void pager_page_in_elsewhere(uintptr_t chunk) {

    for (int attempt=0;;++attempt) {

        PageInRequest request={chunk,0,0,NULL};

        pthread_mutex_lock(&pagein_mutex);

        request.next=pagein_queue; pagein_queue=&request;

        pthread_cond_signal(&pagein_wake);

        while (!request.done) pthread_cond_wait(&pagein_done,&pagein_mutex);

        pthread_mutex_unlock(&pagein_mutex);

        if (request.result) return;

        if (!pager_make_room(64) && attempt>=2) win_fail("pager: committing a chunk coming back (no room)",chunk,PRIVATE_CHUNK);

        if (attempt>=8) win_fail("pager: committing a chunk coming back",chunk,PRIVATE_CHUNK);

    }

}

/* Stack left below the caller, or 0 when it runs on a stack that is not its thread's own. Guest

 * threads have 64 KiB stacks (often less is left: a guest write fault, the renderer's write

 * tracker, then a bring-back), and a page-in takes 16 guard-page exceptions while it fills the

 * chunk: PC 2026-11-02, 0xc00000fd in memcpy under host_protect -> pager_bring_back. */

#define PAGER_INLINE_STACK (256u*1024u)

static size_t pager_stack_left(void) {

    const uintptr_t here=(uintptr_t)__builtin_frame_address(0);

    /* a guest fiber's stack (guest memory; the thread's limits do not describe it): the second

     * PC overflow had rsp 0x12011ea178 and the thread's limits said there was room */

    if (here>=USER_MIN && here<USER_MAX) return 0;

    ULONG_PTR low=0, high=0;

    GetCurrentThreadStackLimits(&low,&high);

    return here>low && here<high ? here-low : 0;

}

/* Chunks of [a,b) as the guest left them, resident (a region change or a host read follows). */

static void pager_bring_back(uintptr_t a, uintptr_t b) {

    if (!pager_active) return;

    int any=0;

    for (uintptr_t chunk=a&~(uintptr_t)(PRIVATE_CHUNK-1); chunk<b && !any; chunk+=PRIVATE_CHUNK) {

        const PagerEntry *e=pager_entry(chunk,0);

        any=e && __atomic_load_n(&e->state,__ATOMIC_ACQUIRE);

    }

    if (!any) return;

    runtime_trace_event(TR_BRING_BACK,a,b-a,__builtin_return_address(0));

    if (pager_holding) win_fail("pager: bring-back under its own lock",a,b-a);

    const int small_stack=pager_stack_left()<PAGER_INLINE_STACK;

    pager_take();

    for (uintptr_t chunk=a&~(uintptr_t)(PRIVATE_CHUNK-1); chunk<b; chunk+=PRIVATE_CHUNK) {

        PagerEntry *e=pager_entry(chunk,0);

        if (!e || !e->state) continue;

        if (e->state!=PG_EVICTED) { pager_restore(chunk,e); _InterlockedExchangeAdd64(&pager_touched,1); continue; }

        if (!small_stack && pager_page_in(chunk,e)) continue;

        pager_give();

        pager_page_in_elsewhere(chunk);

        pager_take();

    }

    pager_give();

}

static int pager_is_view(uintptr_t chunk) {

    const PagerEntry *e=pager_entry(chunk,0);

    return e && e->view;

}

/* private_placeholder: a chunk that is a view goes back to a placeholder by an unmap. */

static int pager_unmap_view(uintptr_t chunk) {

    PagerEntry *e=pager_entry(chunk,0);

    if (!e || !e->view) return 0;

    pager_take();

    if (!UnmapViewOfFile2(GetCurrentProcess(),(void*)chunk,MEM_PRESERVE_PLACEHOLDER)) win_fail("pager: unmapping a chunk view",chunk,PRIVATE_CHUNK);

    e->view=0;

    _InterlockedExchangeAdd64(&pager_views,-1);

    pager_give();

    return 1;

}



static volatile LONG64 pager_guard_waits;

/* This DLL's code (set in pager_start) and the chunks its own faults pinned (under pager_lock) */

#define PAGER_HOST_PIN_MAX 2048

static uintptr_t pager_host_lo, pager_host_hi;

static LONG64 pager_host_pins;

static LONG CALLBACK pager_fault(EXCEPTION_POINTERS *exception) {

    const EXCEPTION_RECORD *record=exception->ExceptionRecord;

    /* A chunk coming back is guarded until its protections hold (pager_page_in): the touch waits

     * for the page-in under pager_lock, then retries with them. */

    if (record->ExceptionCode==EXCEPTION_GUARD_PAGE && record->NumberParameters>=2) {

        const uintptr_t at=(uintptr_t)record->ExceptionInformation[1]&~(uintptr_t)(PRIVATE_CHUNK-1);

        if (pager_filling && pager_entry(at,0)) return EXCEPTION_CONTINUE_EXECUTION; /* the copy's own touch: guard gone */

        if (!pager_entry(at,0) || pager_holding) return EXCEPTION_CONTINUE_SEARCH;

        runtime_trace_event(TR_GUARD_WAIT,record->ExceptionInformation[1],0,(const void*)(uintptr_t)exception->ContextRecord->Rip);

        pager_take(); pager_give();

        _InterlockedExchangeAdd64(&pager_guard_waits,1);

        return EXCEPTION_CONTINUE_EXECUTION;

    }

    if (record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || record->NumberParameters<2) return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t chunk=(uintptr_t)record->ExceptionInformation[1]&~(uintptr_t)(PRIVATE_CHUNK-1);

    PagerEntry *e=pager_entry(chunk,0);

    if (!e || !__atomic_load_n(&e->state,__ATOMIC_ACQUIRE)) return EXCEPTION_CONTINUE_SEARCH;

    if (pager_holding) return EXCEPTION_CONTINUE_SEARCH; /* the pager's own access faulted: a report, not a hang */

    /* b: the chunk's state, and 1<<8 for a write; code: the instruction that touched it */

    runtime_trace_event(TR_FAULT,record->ExceptionInformation[1],(uint64_t)e->state|((uint64_t)record->ExceptionInformation[0]<<8),

                        (const void*)(uintptr_t)exception->ContextRecord->Rip);

    /* A stack the pager took (one it does not know: not a fiber's): pinned for good from now on,

     * and said once (a fault deeper in such a stack could not be dispatched at all). */

    const uintptr_t rsp=(uintptr_t)exception->ContextRecord->Rsp;

    if (chunk+PRIVATE_CHUNK+4096>=rsp-256 && chunk<=rsp+(UINT64_C(1)<<20)) {

        static volatile LONG said;

        if (InterlockedIncrement(&said)<=16) {

            fprintf(stderr,"Pager: fault at %p near the stack (rsp %p, thread %lu): pinned\n",

                    (void*)record->ExceptionInformation[1],(void*)rsp,GetCurrentThreadId());

            fflush(stderr);

        }

        e->pinned=PG_PIN_FOREVER;

    }

    pager_take();

    /* Host code read or wrote a paged chunk directly, not through a bring-back (2026-11-02 Xbox

     * crash 5: the GPU hung right after the command processor, Liverpool::ProcessCompute reading

     * PM4 headers, faulted on command-buffer chunks swept 380 ms before; its next submit was the

     * last). Such a chunk stays in memory for good: command buffers, shader binaries. Not chunks

     * pinned for system I/O at the time (file reads touch their buffers first), and at most

     * PAGER_HOST_PIN_MAX of them. */

    const uintptr_t rip=(uintptr_t)exception->ContextRecord->Rip;

    if (rip>=pager_host_lo && rip<pager_host_hi && !e->pinned && pager_host_pins<PAGER_HOST_PIN_MAX) {

        e->pinned=PG_PIN_FOREVER;

        ++pager_host_pins;

        runtime_trace_event(TR_HOST_PIN,chunk,0,(const void*)rip);

    }

    const uint8_t state=e->state;

    if (state==PG_SWEPT || state==PG_EVICTING) { pager_restore(chunk,e); _InterlockedExchangeAdd64(&pager_touched,1); }

    pager_give();

    if (state==PG_EVICTED) pager_page_in_elsewhere(chunk);

    return EXCEPTION_CONTINUE_EXECUTION; /* retried: a protection fault of its own comes back with state 0 */

}



/* I/O pins: a buffer the system reads or writes stays out of the pager's sweeps meanwhile. */

static void pager_pin(uintptr_t a, uint64_t size, int delta) {

    if (!size) return;

    pager_take();

    for (uintptr_t chunk=a&~(uintptr_t)(PRIVATE_CHUNK-1); chunk<a+size; chunk+=PRIVATE_CHUNK) {

        PagerEntry *e=pager_entry(chunk,delta>=0); /* a pin needs an entry (a fiber stack nobody swept yet has none) */

        if (!e || e->pinned==PG_PIN_FOREVER) continue;

        if (delta==0) { e->pinned=PG_PIN_FOREVER; if (e->state) { if (e->state==PG_EVICTED) { if (!pager_page_in(chunk,e)) { pager_give(); pager_page_in_elsewhere(chunk); pager_take(); } } else pager_restore(chunk,e); } }

        else if (delta>0 && e->pinned<PG_PIN_FOREVER-1) ++e->pinned;

        else if (delta<0 && e->pinned) --e->pinned;

    }

    pager_give();

}

void runtime_memory_pager_pin(const void *buffer, uint64_t size, int delta) {

    if (!pager_active || !delta) return;

    pager_pin((uintptr_t)buffer,size,delta>0 ? 1 : -1);

}

/* Fiber stacks: a fault there would have no stack to be dispatched on. Recorded whether or not

 * the pager runs (it may start later). */

void runtime_memory_pager_pin_forever(const void *buffer, uint64_t size) {

    if ((uintptr_t)buffer<USER_MIN || (uintptr_t)buffer>=USER_MAX) return;

    pager_pin((uintptr_t)buffer,size,0);

    /* And committed, every page now (2026-11-02): guest memory commits on first touch, but the

     * frame of an exception taken on a fiber goes below its rsp, on a page the fiber may never

     * have reached. Not there, Windows cannot dispatch the exception and stops the game with

     * 0xc00000fd (in Boston: memcpy touched a paged-out chunk with rsp 0xb8 into its page). */

    for (uintptr_t at=(uintptr_t)buffer&~(uintptr_t)4095;at<(uintptr_t)buffer+size;at+=4096)

        (void)*(volatile const unsigned char*)at;

    _InterlockedExchangeAdd64(&pager_stacks,1);

    _InterlockedExchangeAdd64(&pager_stack_bytes,(LONG64)size);

}



/* make-room's last pass (pager_lock held): the cooldown no longer holds a chunk back */

static int pager_sweep_ignores_cooldown;

/* Sweep: region lock (shared) and pager_lock held. 1 when the chunk is swept now. */

static int pager_sweep(size_t index) {

    const uintptr_t chunk=regions[index].start;

    PagerEntry *e=pager_entry(chunk,1);

    if (!e || e->state || e->pinned) return 0;

    /* just came back / in use: left alone (urgent: for a shorter time, never 0: ignoring it thrashed,

     * 2026-10-31 crash 3: 11 GB out and 10 GB back in one session) */

    if (e->used_at && !pager_sweep_ignores_cooldown &&

        pager_now_s()-e->used_at<(pager_urgent ? PAGER_URGENT_COOLDOWN_S : PAGER_COOLDOWN_S)) return 0;

    uint8_t prot[16]={0};

    int committed=0;

    for (uintptr_t at=chunk; at<chunk+PRIVATE_CHUNK;) {

        MEMORY_BASIC_INFORMATION m;

        if (!VirtualQuery((void*)at,&m,sizeof(m))) return 0;

        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;

        if (next>chunk+PRIVATE_CHUNK) next=chunk+PRIVATE_CHUNK;

        if (next<=at) return 0;

        if (m.State==MEM_COMMIT) {

            const DWORD p=m.Protect;

            const uint8_t code=p==PAGE_READONLY ? 1 : p==PAGE_READWRITE ? 2 : 0;

            if (!code) return 0; /* no-access (a GPU read watch), guard, executable: left alone */

            for (uintptr_t q=at;q<next;q+=4096) prot[(q-chunk)>>12]=code;

            ++committed;

        } else if (m.State!=MEM_RESERVE || e->view) return 0;

        at=next;

    }

    if (!committed) return 0;

    memcpy(e->prot,prot,16);

    e->init=regions[index].initial_protect==PAGE_READONLY ? 1 : 2;

    e->group4=regions[index].mapped==4;

    for (int p=0;p<16;) {

        int q=p+1;

        while (q<16 && (e->prot[q]!=0)==(e->prot[p]!=0)) ++q;

        q=pager_run_end(e,p,q);

        DWORD old;

        if (e->prot[p] && !VirtualProtect((void*)(chunk+(uintptr_t)p*4096),(SIZE_T)(q-p)*4096,PAGE_NOACCESS,&old)) {

            pager_restore(chunk,e);

            return 0;

        }

        p=q;

    }

    __atomic_store_n(&e->state,PG_SWEPT,__ATOMIC_RELEASE);

    _InterlockedExchangeAdd64(&pager_swept,1);

    runtime_trace_event(TR_SWEEP,chunk,0,NULL);

    return 1;

}

/* The store file, opened on first use (pager thread or a make-room). */

static int pager_open_store(void) {

    static SRWLOCK open_lock=SRWLOCK_INIT;

    if (pager_file!=INVALID_HANDLE_VALUE) return 1;

    AcquireSRWLockExclusive(&open_lock);

    if (pager_file==INVALID_HANDLE_VALUE) {

        char path[MAX_PATH*2]; wchar_t wide[MAX_PATH*2];

        extern const char *runtime_file_user_dir(void);

        snprintf(path,sizeof(path),"%s\\pager-store.tmp",runtime_file_user_dir());

        /* buffered (see pager_slot_io), written through: the cache keeps clean pages only */

        CREATEFILE2_EXTENDED_PARAMETERS options={.dwSize=sizeof(options),.dwFileAttributes=FILE_ATTRIBUTE_NORMAL,

            .dwFileFlags=FILE_FLAG_DELETE_ON_CLOSE|FILE_FLAG_WRITE_THROUGH};

        if (MultiByteToWideChar(CP_UTF8,0,path,-1,wide,MAX_PATH*2))

            pager_file=CreateFile2(wide,GENERIC_READ|GENERIC_WRITE,0,CREATE_ALWAYS,&options);

        printf("Pager: store %s %s (error %lu)\n",path,pager_file!=INVALID_HANDLE_VALUE ? "opened" : "NOT opened",

               pager_file!=INVALID_HANDLE_VALUE ? 0ul : GetLastError());

        fflush(stdout);

    }

    ReleaseSRWLockExclusive(&open_lock);

    return pager_file!=INVALID_HANDLE_VALUE;

}

/* Out to the file: swept and untouched since. Region lock and pager_lock taken here. */

static void pager_evict(uintptr_t chunk, unsigned char *copy) {

    read_lock();

    pager_take();

    size_t i=region_index(chunk);

    PagerEntry *e=pager_entry(chunk,0);

    if (!e || e->state!=PG_SWEPT || i>=region_count || regions[i].start!=chunk ||
        !(regions[i].mapped==2 || (e->group4 && pager_group4(i)))) { pager_give(); read_unlock(); return; }

    for (int p=0;p<16;) { /* one call per run of committed pages (it was one per page) */

        int q=p+1;

        while (q<16 && (e->prot[q]!=0)==(e->prot[p]!=0)) ++q;

        q=pager_run_end(e,p,q);

        DWORD old;

        if (e->prot[p] && !VirtualProtect((void*)(chunk+(uintptr_t)p*4096),(SIZE_T)(q-p)*4096,PAGE_READONLY,&old)) {

            pager_restore(chunk,e); pager_give(); read_unlock(); return;

        }

        p=q;

    }

    __atomic_store_n(&e->state,PG_EVICTING,__ATOMIC_RELEASE);

    for (int p=0;p<16;++p) { /* committed pages only: a reserved page would fault under the lock */

        if (e->prot[p]) memcpy(copy+p*4096,(void*)(chunk+(uintptr_t)p*4096),4096);

        else memset(copy+p*4096,0,4096);

    }

    const int zero=pager_zero_chunks() && pager_is_zero(copy);

    const uint32_t slot=zero ? PAGER_ZERO_SLOT : pager_alloc_slot();

    pager_give();

    read_unlock();

    const uint64_t sum=zero ? 0 : pager_checksum(copy);

    /* a writer meanwhile faults and stops the eviction */

    const int written=zero ? 1 : pager_slot_io(slot,copy,1);

    read_lock();

    pager_take();

    i=region_index(chunk);

    int left=0;

    if (written && e->state==PG_EVICTING && i<region_count && regions[i].start==chunk &&
        (regions[i].mapped==2 || (e->group4 && pager_group4(i)))) {

        int same=1;

        for (int p=0;p<16 && same;++p) if (e->prot[p]) same=!memcmp(copy+p*4096,(void*)(chunk+(uintptr_t)p*4096),4096);

        if (same) {

            BOOL released;

            if (e->group4) {

                released=TRUE;

                for (int k=0;k<4;++k)

                    if (!VirtualFree((void*)(chunk+(uintptr_t)k*SMALL_CHUNK),SMALL_CHUNK,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))

                        win_fail("pager: releasing a 16 KiB piece",chunk+(uintptr_t)k*SMALL_CHUNK,SMALL_CHUNK);

            } else released=e->view ? UnmapViewOfFile2(GetCurrentProcess(),(void*)chunk,MEM_PRESERVE_PLACEHOLDER)

                                    : VirtualFree((void*)chunk,PRIVATE_CHUNK,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER);

            if (released) {

                if (e->view) { e->view=0; _InterlockedExchangeAdd64(&pager_views,-1); }

                if (zero) _InterlockedExchangeAdd64(&pager_zero_out,1);

                else pager_set_sum(slot,sum);

                e->slot=slot;

                __atomic_store_n(&e->state,PG_EVICTED,__ATOMIC_RELEASE);

                left=1;

                _InterlockedExchangeAdd64(&pager_evicted,1);

                runtime_trace_event(TR_EVICT,chunk,slot,NULL);

            } else _InterlockedExchangeAdd64(&pager_failed,1);

        }

        if (!left) pager_restore(chunk,e);

    }

    if (!left) { if (!zero) pager_free_slot(slot); _InterlockedExchangeAdd64(&pager_aborted,1); runtime_trace_event(TR_EVICT_ABORT,chunk,written,NULL); }

    pager_give();

    read_unlock();

}

/* Make room (2026-10-31, after KytyPS5's Regions::MakeRoom): a commit failed at the process's

 * memory limit (crashes 1 and 3: "committing CPU storage destination failed ... error 1455").

 * Up to `want` chunks leave memory on the spot so the commit can be tried again: chunks already

 * swept (cold, waiting for the pager thread) first, then cold ones swept and written out right

 * away (cooldown respected, so what the game just used stays). The caller may hold the region

 * lock (it nests); not pager_lock (then nothing is done). One make-room at a time.

 * Last pass (2026-11-02, Xbox crash after 53 min: an area load during a save, 8 make-rooms, the

 * last found no cold chunk and the game stopped): with nothing cold left, chunks in their cooldown

 * go too, all but those touched in the last PAGER_ROOM_KEEP_S seconds. A chunk that comes straight

 * back costs a page-in; the commit failing costs the game. */

#define PAGER_ROOM_KEEP_S 2

static unsigned char *room_copy;

static volatile LONG64 pager_room_made, pager_room_calls, pager_room_warm;

static uint64_t pager_make_room(uint64_t want) {

    if (!pager_active || pager_holding || !room_copy || !pager_open_store()) return 0;

    static SRWLOCK room_lock=SRWLOCK_INIT;

    AcquireSRWLockExclusive(&room_lock);

    static uintptr_t room_hand;

    uint64_t made=0, warm=0;

    for (int pass=0; pass<3 && made<want; ++pass) {

        size_t examined=0;

        while (made<want && examined<region_count) {

            uintptr_t picked[32]; unsigned n=0;

            read_lock();

            pager_take();

            size_t i=region_index(room_hand), seen=0;

            for (; i<region_count && seen<512 && n<32 && made+n<want; ++i, ++seen) {

                if (!pager_candidate(i)) continue;

                const uintptr_t chunk=regions[i].start;

                PagerEntry *e=pager_entry(chunk,0);

                if (pass==0) { if (e && e->state==PG_SWEPT && !e->pinned) picked[n++]=chunk; }

                else if (pass==1) { if (pager_sweep(i)) picked[n++]=chunk; }

                else if (!e || !e->used_at || pager_now_s()-e->used_at>=PAGER_ROOM_KEEP_S) {

                    pager_sweep_ignores_cooldown=1;

                    if (pager_sweep(i)) picked[n++]=chunk;

                    pager_sweep_ignores_cooldown=0;

                }

            }

            room_hand=i<region_count ? regions[i].start : 0;

            pager_give();

            read_unlock();

            examined+=seen ? seen : 1;

            for (unsigned k=0; k<n; ++k) {

                const LONG64 before=pager_evicted;

                pager_evict(picked[k],room_copy);

                if (pager_evicted!=before) { ++made; warm+=pass==2; }

            }

            if (i>=region_count) room_hand=0;

        }

    }

    ReleaseSRWLockExclusive(&room_lock);

    _InterlockedExchangeAdd64(&pager_room_made,(LONG64)made);

    const LONG64 warm_all=_InterlockedExchangeAdd64(&pager_room_warm,(LONG64)warm)+(LONG64)warm;

    runtime_trace_event(TR_MAKE_ROOM,want,made|warm<<32,__builtin_return_address(0));

    const LONG64 calls=_InterlockedIncrement64(&pager_room_calls);

    if (calls<=8 || calls%64==0 || (warm && warm_all==(LONG64)warm)) {

        fprintf(stderr,"Pager: a commit failed at the memory limit: made room for %llu chunks on the spot, %llu of them used in their cooldown (%lld times, %lld chunks in all, %lld in cooldown)\n",

                (unsigned long long)made,(unsigned long long)warm,(long long)calls,(long long)pager_room_made,(long long)warm_all);

        fflush(stderr);

    }

    return made;

}

/* For the renderer (2026-10-31 wall test: "Failed allocating image with error

 * ErrorOutOfDeviceMemory" after freeing 0 MiB of unused textures): on the Xbox GPU memory comes out

 * of the same commit as guest memory, so cold guest chunks leaving makes room for an image or an

 * arena too. Returns the bytes that left. */

uint64_t runtime_memory_make_room(uint64_t bytes) {

    return pager_make_room((bytes>>16)+16)<<16;

}

/* VirtualAlloc(MEM_COMMIT) that makes room and tries again when the process is at its limit. */

static BOOL commit_with_room(void *address, SIZE_T size, DWORD protect, const char *what) {

    for (int attempt=0;;++attempt) {

        if (VirtualAlloc(address,size,MEM_COMMIT,protect)) return TRUE;

        const DWORD error=GetLastError();

        {   /* Spider-Man port (2026-11-05): which commit failed, with the app's private / shared split. */
            static volatile LONG said;
            if (_InterlockedIncrement(&said)<=40) {
                APP_MEMORY_INFORMATION app={0};
                GetProcessInformation(GetCurrentProcess(),ProcessAppMemoryInfo,&app,sizeof(app));
                fprintf(stderr,"Commit failed: %s %p size %#llx error %lu attempt %d; app total %llu MiB private %llu MiB shared %llu MiB available %llu MiB\n",
                        what,address,(unsigned long long)size,error,attempt,
                        (unsigned long long)(app.TotalCommitUsage>>20),(unsigned long long)(app.PrivateCommitUsage>>20),
                        (unsigned long long)((app.TotalCommitUsage-app.PrivateCommitUsage)>>20),
                        (unsigned long long)(app.AvailableCommit>>20));
                fflush(stderr);
            }
        }

        if ((error!=ERROR_COMMITMENT_LIMIT && error!=ERROR_NOT_ENOUGH_MEMORY && error!=ERROR_WORKING_SET_QUOTA) || attempt>=4) {

            SetLastError(error);

            return FALSE;

        }

        const uint64_t want=(attempt ? 256 : 64)+(size>>16);

        if (!pager_make_room(want)) {

            fprintf(stderr,"Pager: %s failed at the memory limit (error %lu) and no room could be made\n",what,error);

            fflush(stderr);

            SetLastError(error);

            return FALSE;

        }

    }

}

static uint64_t pager_app_usage(void) {

    APP_MEMORY_INFORMATION app={0};

    return GetProcessInformation(GetCurrentProcess(),ProcessAppMemoryInfo,&app,sizeof(app)) ? app.TotalCommitUsage : 0;

}

/* God of War port (2026-11-04): back-pressure on the game's file reads. God of War's first world load

 * adds ~3 GiB in ~10 s (Xbox boot19-22: +3.0..3.9 GiB in 10 s; the pager writes ~80 MiB/s and only

 * pages chunks untouched for a few seconds), so the app reached its 5 GiB limit mid-load and the next

 * allocation that could not make room ended the game. A read of 64 KiB or more while app memory is

 * within BB_IO_BACKPRESSURE_MB (default 600) of the limit first makes room for itself and then waits,

 * up to 2 s in 10 ms steps, for app memory to drop: the load runs at the pager's pace instead of over

 * the limit. BB_IO_BACKPRESSURE=0: off. */

static volatile LONG64 backpressure_waits, backpressure_ms;

void runtime_memory_io_backpressure(uint64_t size) {

    static int margin_mb=-1;

    if (margin_mb<0) {

        const char *off=getenv("BB_IO_BACKPRESSURE"), *mb=getenv("BB_IO_BACKPRESSURE_MB");

        margin_mb=(off && off[0]=='0') ? 0 : (mb && atoi(mb)>0 ? atoi(mb) : 600);

    }

    if (!margin_mb || !pager_active || !pager_limit || size<((uint64_t)64<<10)) return;

    const uint64_t threshold=pager_limit-((uint64_t)margin_mb<<20);

    if (pager_app_usage()+size<=threshold) return;

    runtime_memory_make_room(size);

    const ULONGLONG began=GetTickCount64();

    while (pager_app_usage()+size>threshold && GetTickCount64()-began<2000) Sleep(10);

    const LONG64 waits=_InterlockedIncrement64(&backpressure_waits);

    const LONG64 total=_InterlockedExchangeAdd64(&backpressure_ms,(LONG64)(GetTickCount64()-began))+(LONG64)(GetTickCount64()-began);

    if (waits<=4 || waits%64==0) {

        printf("Pager: back-pressure on a %llu KiB read: app %llu MiB (threshold %llu MiB); %lld waits, %lld ms in all\n",

               (unsigned long long)(size>>10),(unsigned long long)(pager_app_usage()>>20),(unsigned long long)(threshold>>20),

               (long long)waits,(long long)total);

        fflush(stdout);

    }

}

static DWORD WINAPI pager_thread(void *unused) {

    (void)unused;

    typedef struct { uintptr_t chunk; ULONGLONG at; } Swept;

    enum { QUEUE=1<<16 };

    static Swept queue[QUEUE];

    size_t head=0, tail=0;

    unsigned char *copy=VirtualAlloc(NULL,PRIVATE_CHUNK,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);

    uintptr_t hand=0;

    ULONGLONG last_line=0;

    for (;;) {

        Sleep(PAGER_LOOP_MS);

        const ULONGLONG now=GetTickCount64();

        uint64_t usage=pager_app_usage();

        /* BB_PAGER_TRACE=1: a line every 2 s, flushed (the last state before a hang or a kill) */

        static int trace=-1;

        if (trace<0) { const char *t=getenv("BB_PAGER_TRACE"); trace=t && t[0]=='1'; }

        if (trace && now-last_line>=2000) {

            last_line=now;

            /* Crash 3 failed commit at app ~4500 MiB (crash 1: 4808; private balloon: 4888): the

             * system's own count of commit left, to see what else uses the budget. */

            typedef BOOL (WINAPI *MemoryStatusFn)(LPMEMORYSTATUSEX);

            static MemoryStatusFn memory_status; static int resolved;

            if (!resolved) { resolved=1; HMODULE k=GetModuleHandleW(L"kernel32.dll"); if (k) memory_status=(MemoryStatusFn)GetProcAddress(k,"GlobalMemoryStatusEx"); }

            MEMORYSTATUSEX ms={.dwLength=sizeof(ms)};

            if (memory_status && memory_status(&ms))

                fprintf(stderr,"Pager memory t=%llu: commit left %llu of %llu MiB, physical free %llu of %llu MiB, app %llu MiB\n",

                        (unsigned long long)(now/1000),(unsigned long long)(ms.ullAvailPageFile>>20),(unsigned long long)(ms.ullTotalPageFile>>20),

                        (unsigned long long)(ms.ullAvailPhys>>20),(unsigned long long)(ms.ullTotalPhys>>20),(unsigned long long)(usage>>20));

            fprintf(stderr,"Pager trace t=%llu: app %llu MiB, file %lld, out %lld, back %lld, aborted %lld, swept %lld, touched %lld, let go %lld, views %lld, failed %lld, queue %zu, fiber stacks pinned %lld (%lld KiB), came back different %lld, guard waits %lld, read retries %lld, host pins %lld, fill diffs %lld\n",

                    (unsigned long long)(now/1000),(unsigned long long)(usage>>20),(long long)pager_in_file,(long long)pager_evicted,

                    (long long)pager_page_ins,(long long)pager_aborted,(long long)pager_swept,(long long)pager_touched,

                    (long long)pager_released,(long long)pager_views,(long long)pager_failed,(head+QUEUE-tail)%QUEUE,

                    (long long)pager_stacks,(long long)(pager_stack_bytes>>10),(long long)pager_corrupt,(long long)pager_guard_waits,

                    (long long)pager_io_retries,(long long)pager_host_pins,(long long)pager_fill_diffs);

            fflush(stderr);

        }

        /* TLOU test switch (2026-11-01): while <user dir>\pager-pause.txt exists the pager neither

         * sweeps nor pages out (chunks still come back when touched). After the move into Boston,

         * guest copies of resident memory ran ~10x slower than after a fresh load; the suspect is

         * the protection changes of sweeps and page-ins (each one interrupts every core). */

        {

            extern const char *runtime_file_user_dir(void);

            static int paused; static ULONGLONG checked;

            if (now-checked>=2000) {

                checked=now;

                char path[1024];

                snprintf(path,sizeof(path),"%s\\pager-pause.txt",runtime_file_user_dir());

                FILE *f=fopen(path,"rb");

                const int pause_now=f!=NULL;

                if (f) fclose(f);

                if (pause_now!=paused) {

                    paused=pause_now;

                    printf("Pager: %s (pager-pause.txt)\n",paused ? "paused" : "resumed");

                    fflush(stdout);

                }

            }

            if (paused) continue;

        }

        /* Pressure (2026-10-31 crash: a new-area load took the app 4084 -> 4808 MiB in ~60 s and

         * commit failed, error 1455, while 11008 swept chunks still waited out their dwell):

         *   urgent: within PAGER_URGENT_MB of the limit: 1 s dwell, 512 out / loop, cooldown ignored;

         *   fast:   within PAGER_FAST_MB, or the app grew PAGER_CLIMB_MB in 10 s above the low mark:

         *           5 s dwell, 256 out / loop;

         *   calm:   30 s dwell (BB_PAGER_DWELL_S), PAGER_OUT_PER_LOOP. */

        static uint64_t history[100]; /* usage per loop, 10 s */

        static unsigned history_at, history_n;

        history[history_at]=usage; history_at=(history_at+1)%100; if (history_n<100) ++history_n;

        uint64_t lowest=usage;

        for (unsigned h=0; h<history_n; ++h) if (history[h]<lowest) lowest=history[h];

        /* Thrash guard (crash 3): pages coming back at half or more the rate they go out over the

         * last 10 s means the pager is evicting the working set: urgent then acts like fast. */

        static LONG64 outs[100], ins[100];

        outs[history_at==0 ? 99 : history_at-1]=pager_evicted; ins[history_at==0 ? 99 : history_at-1]=pager_page_ins;

        const unsigned oldest=history_n<100 ? 0 : history_at;

        const LONG64 out10=pager_evicted-outs[oldest], in10=pager_page_ins-ins[oldest];

        const int thrashing=in10>=200 && in10*2>=out10;

        const int urgent=usage+((uint64_t)PAGER_URGENT_MB<<20)>pager_limit && !thrashing;

        const int climbing=usage>pager_low && usage-lowest>=((uint64_t)PAGER_CLIMB_MB<<20);

        const int rush=urgent || thrashing || climbing || usage+((uint64_t)PAGER_FAST_MB<<20)>pager_limit;

        pager_urgent=urgent;

        static int last_level=0;

        const int level=urgent ? 2 : thrashing ? 3 : rush ? 1 : 0;

        if (level!=last_level) {

            printf("Pager: pressure %s (app %llu MiB, +%llu MiB in 10 s; 10 s: %lld out, %lld back)\n",

                   level==2 ? "urgent" : level==3 ? "fast (thrashing)" : level ? "fast" : "calm",

                   (unsigned long long)(usage>>20),(unsigned long long)((usage-lowest)>>20),(long long)out10,(long long)in10);

            fflush(stdout);

            last_level=level;

        }

        /* 1. swept chunks a dwell ago: out while above the low mark, else let go */

        const unsigned out_cap=urgent ? 512 : rush ? 256 : PAGER_OUT_PER_LOOP;

        static ULONGLONG calm_dwell=0;

        if (!calm_dwell) { const char *s=getenv("BB_PAGER_DWELL_S"); calm_dwell=s && atoi(s)>0 ? (ULONGLONG)atoi(s)*1000 : PAGER_CALM_DWELL_MS; }

        const ULONGLONG dwell=urgent ? 3000 : rush ? PAGER_DWELL_MS : calm_dwell;

        for (unsigned n=0; tail!=head && now-queue[tail].at>=dwell && n<out_cap; ++n) {

            const uintptr_t chunk=queue[tail].chunk;

            tail=(tail+1)%QUEUE;

            if (usage>pager_low) {

                if (!pager_open_store()) pager_active=0;

                if (pager_file!=INVALID_HANDLE_VALUE) pager_evict(chunk,copy);

            } else {

                pager_take();

                PagerEntry *e=pager_entry(chunk,0);

                if (e && e->state==PG_SWEPT) { pager_restore(chunk,e); _InterlockedExchangeAdd64(&pager_released,1); }

                pager_give();

            }

            if (!pager_active) break;

        }

        if (!pager_active) {

            /* the store failed: everything swept goes back, and the pager stops */

            for (; tail!=head; tail=(tail+1)%QUEUE) pager_bring_back(queue[tail].chunk,queue[tail].chunk+PRIVATE_CHUNK);

            return 0;

        }

        /* 2. above the high mark: sweep a few more chunks while fewer are queued than needed */

        usage=pager_app_usage();

        const uint64_t queued=(head+QUEUE-tail)%QUEUE;

        const uint64_t need=usage>pager_low ? (usage-pager_low)/PRIVATE_CHUNK : 0;

        const uint64_t want=need+need/2+16;

        /* Scan budget (2026-10-31): above the high mark with almost every chunk in use or cooling

         * down, each 100 ms loop walked the whole region table (~76k, up to two wraps) taking

         * read_lock + pager_lock every 128 regions: copy threads stalled behind those locks and the

         * game ran at 3-7 FPS for minutes (copy-thread wait 392 of 434 ms per frame). Now at most

         * PAGER_SCAN_PER_LOOP regions are examined per loop (urgent: 8x), and a full pass that

         * found less than wanted pauses sweeping for PAGER_SCAN_BACKOFF_MS (urgent: no pause). */

        static ULONGLONG scan_pause_until=0;

        static uint64_t pass_found=0, pass_seen=0;

        if (usage>pager_high && queued<want && queued<QUEUE/2 && (urgent || now>=scan_pause_until)) {

            uint64_t target=want-queued;

            const uint64_t sweep_cap=urgent ? 4096 : rush ? 256 : PAGER_SWEEP_PER_LOOP;

            if (target>sweep_cap) target=sweep_cap;

            const uint64_t budget=urgent ? 8*PAGER_SCAN_PER_LOOP : PAGER_SCAN_PER_LOOP;

            uint64_t count=0, examined=0;

            while (count<target && examined<budget) {

                read_lock();

                pager_take();

                size_t i=region_index(hand), seen=0;

                for (; i<region_count && seen<128 && count<target; ++i, ++seen) { /* locks dropped every 128 */

                    if (!pager_candidate(i)) continue;

                    if (pager_sweep(i)) { queue[head]=(Swept){regions[i].start,now}; head=(head+1)%QUEUE; ++count; }

                }

                hand=i<region_count ? regions[i].start : 0;

                const int end=i>=region_count;

                pager_give();

                read_unlock();

                examined+=seen;

                pass_seen+=seen;

                if (end || pass_seen>=region_count) {

                    /* a full pass: scarce when it found less than this loop wanted */

                    if (pass_found+count<target && !urgent) scan_pause_until=now+PAGER_SCAN_BACKOFF_MS;

                    pass_found=0; pass_seen=0;

                    break;

                }

            }

            pass_found+=count;

        }

    }

}

static void pager_start(void) {

    trace_start();

    HMODULE self=NULL;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)pager_start,&self)) {

        pager_host_lo=(uintptr_t)self;

        pager_host_hi=pager_host_lo+((const IMAGE_NT_HEADERS*)(pager_host_lo+((const IMAGE_DOS_HEADER*)self)->e_lfanew))->OptionalHeader.SizeOfImage;

    }

    const char *env=getenv("BB_PAGER"), *limit=getenv("BB_APP_LIMIT_MB"), *high=getenv("BB_PAGER_HIGH_MB");

    pager_limit=limit ? strtoull(limit,NULL,10)<<20 : 0;

    if ((env && env[0]=='0') || !pager_limit || !private_cpu) {

        printf("Pager: off (%s)\n",env && env[0]=='0' ? "BB_PAGER=0" : !pager_limit ? "no app memory limit" : "no private chunks");

        return;

    }

    pager_high=high ? strtoull(high,NULL,10)<<20 : pager_limit-((uint64_t)PAGER_HIGH_BELOW_LIMIT_MB<<20);

    pager_low=pager_high-(UINT64_C(256)<<20);

    if (!AddVectoredExceptionHandler(1,pager_fault)) { printf("Pager: off (no fault handler)\n"); return; }

    /* make-room's copy buffer exists before memory is tight (allocating it then could fail) */

    room_copy=VirtualAlloc(NULL,PRIVATE_CHUNK,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);

    /* BB_TEST_BALLOON_MB=N (test only): N MiB committed and touched at start, so the game meets

     * the real commit limit early and make-room is exercised. */

    const char *balloon=getenv("BB_TEST_BALLOON_MB");

    if (balloon && atoi(balloon)>0) {

        const SIZE_T bytes=(SIZE_T)atoi(balloon)<<20;

        unsigned char *b=VirtualAlloc(NULL,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);

        if (b) for (SIZE_T at=0;at<bytes;at+=4096) b[at]=1;

        printf("Pager: test balloon of %d MiB %s\n",atoi(balloon),b ? "committed" : "refused");

    }

    pager_active=1;

    HANDLE t1=CreateThread(NULL,0,pager_thread,NULL,0,NULL), t2=CreateThread(NULL,0,pagein_thread,NULL,0,NULL);

    if (t1) CloseHandle(t1);

    if (t2) CloseHandle(t2);

    printf("Pager: on above %llu MiB of app memory (down to %llu MiB), chunks untouched for %d s go to a file; app memory now %llu MiB\n",

           (unsigned long long)(pager_high>>20),(unsigned long long)(pager_low>>20),(getenv("BB_PAGER_DWELL_S") && atoi(getenv("BB_PAGER_DWELL_S"))>0 ? atoi(getenv("BB_PAGER_DWELL_S")) : PAGER_CALM_DWELL_MS/1000),

           (unsigned long long)(pager_app_usage()>>20));

}

void runtime_memory_report_pager(void) {

    if (!pager_active) return;

    const LONG64 ins=pager_page_ins;

    printf("Pager: %lld MiB in the file; out %lld MiB (all-zero, not written %lld MiB), back %lld MiB (%lld us each), stopped by a touch %lld, swept %lld, "

           "touched while swept %lld, let go %lld, views %lld, failed %lld; app memory %llu MiB (above %llu MiB it pages)\n",

           (long long)(pager_in_file*64/1024),(long long)(pager_evicted*64/1024),(long long)(pager_zero_out*64/1024),(long long)(ins*64/1024),

           (long long)(ins ? pager_page_in_us/ins : 0),(long long)pager_aborted,(long long)pager_swept,(long long)pager_touched,

           (long long)pager_released,(long long)pager_views,(long long)pager_failed,

           (unsigned long long)(pager_app_usage()>>20),(unsigned long long)(pager_high>>20));

}



int runtime_memory_dependency_key(uintptr_t address,uint64_t size,

                                  uint64_t *physical,uint64_t *generation) {

    typedef struct { uintptr_t start,end; uint64_t phys,generation; int valid; } KeyCache;

    static __thread KeyCache cache;

    if (!size || address<USER_MIN || size>UINTPTR_MAX-address || address+size>USER_MAX)

        return 0;

    read_lock();

    const uint64_t current=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);

    if (current&1) { read_unlock(); return 0; }

    if (cache.valid && cache.generation==current && address>=cache.start &&

        address+size<=cache.end) {

        *physical=cache.phys+(address-cache.start); *generation=current;

        read_unlock(); return 1;

    }

    cache.valid=0;

    const size_t at=vma_index(address);

    int valid=at<vma_count && vmas[at].start<=address && vmas[at].end>=address+size &&

              vmas[at].kind!=KIND_RESERVED;

    if (valid) {

        const Vma source=vmas[at];

        const uint64_t end=source.phys+(source.end-source.start);

        for (size_t i=0;i<vma_count;++i) {

            if (i==at || vmas[i].kind==KIND_RESERVED) continue;

            const uint64_t other_end=vmas[i].phys+(vmas[i].end-vmas[i].start);

            if (source.phys<other_end && vmas[i].phys<end) { valid=0; break; }

        }

        if (valid) {

            cache=(KeyCache){source.start,source.end,source.phys,current,1};

            *physical=source.phys+(address-source.start); *generation=current;

        }

    }

    read_unlock(); return valid;

}





void runtime_memory_report_uploads(void) {

    static int reported;

    if(!reported) {

        reported=1;

        puts("Upload accounting: private-memory trial uses sparse/private reader; runtime18 upload counters are unavailable in this comparison.");

    }

}


/* God of War port (2026-11-04), BB_HEAP_CHECKPOINTS=1 (diagnostic): HeapValidate of the process heap at
 * fixed points of the startup path (renderer init, pipeline warm-up, guest entry), the first bad one
 * said once. About a third of the Xbox boots died of heap corruption found by the guest's first
 * allocations; checkpoints on the startup thread bracket the code that did it, whatever the timing. */
void runtime_heap_checkpoint(const char *where) {
#ifdef _WIN32
    static int on=-1;
    static volatile LONG said_bad;
    if (on<0) { const char *e=getenv("BB_HEAP_CHECKPOINTS"); on=e && e[0]=='1'; }
    if (!on) return;
    const int ok=HeapValidate(GetProcessHeap(),0,NULL)!=0;
    if (ok) { printf("Heap checkpoint ok: %s\n",where); fflush(stdout); return; }
    if (InterlockedIncrement(&said_bad)==1) {
        fprintf(stderr,"Heap checkpoint BAD (first): %s\n",where); fflush(stderr);
        printf("Heap checkpoint BAD (first): %s\n",where); fflush(stdout);
    }
#else
    (void)where;
#endif
}
