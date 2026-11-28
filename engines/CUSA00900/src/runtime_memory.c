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
#define USER_MIN UINT64_C(0x1000000000)
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
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
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
static __thread unsigned exclusive_depth;
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
static void write_lock(void) {
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
static void read_lock(void) { if (!exclusive_depth) {
#ifdef _WIN32
    if (recursive_table_mode()) EnterCriticalSection(&private_table_lock);else
#endif
    pthread_rwlock_rdlock(&lock);
} }
static void read_unlock(void) { if (!exclusive_depth) {
#ifdef _WIN32
    if (recursive_table_mode()) LeaveCriticalSection(&private_table_lock);else
#endif
    pthread_rwlock_unlock(&lock);
} }
/* bbport (pager, 2026-11-03): read_lock without waiting: 0 when another thread holds the table (the
 * pager's page-in helper: the thread that asked it may hold the table and wait for it). */
static int try_read_lock(void) {
    if (exclusive_depth) return 1;
#ifdef _WIN32
    if (recursive_table_mode()) return TryEnterCriticalSection(&private_table_lock) ? 1 : 0;
#endif
    return pthread_rwlock_tryrdlock(&lock)==0;
}
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
static PendingHook pending[64]; static size_t pending_count;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static int block_type(uint64_t phys);
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
#ifdef _WIN32
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
static int private_cpu;
static int private_gpu;
static uintptr_t zero_trim_guard,zero_trim_cursor;
#define PRIVATE_CHUNK UINT64_C(65536)
/* The pager (TLOU's, after Kyty's Xbox pager; see "The pager" below): private chunks the guest
 * leaves alone go to a file near the app's memory limit. Code that changes or reads a private
 * chunk's host mapping brings the chunks back first. */
static int pager_active;
void runtime_memory_report_pager(void);
static void pager_bring_back(uintptr_t a, uintptr_t b);
static void pager_start(void);
static uint64_t pager_make_room(uint64_t want);
static BOOL commit_with_room(void *address, SIZE_T size, DWORD protect, const char *what);
void runtime_trace_event(int kind, uint64_t a, uint64_t b, const void *code);
static int pager_events_on=1;
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
    if ((e && e[0]=='0') || !pager_events_on) return;
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
static unsigned char **private_store;
static int discarding_private;
static uint64_t discard_begin,discard_end;
#define FILE_VIEW_BYTES (UINT64_C(128)*1024*1024)
#define FILE_STORE_BYTES (UINT64_C(512)*1024*1024)
static int memory_reserved, lazy_guest_memory, demand_commit, touch_commit;
static uint64_t touch_cpu_min_bytes;
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
static uint64_t view_part_size(uint64_t phys,uint64_t remaining) {
    uint64_t n=file_backed ? FILE_VIEW_BYTES-(phys%FILE_VIEW_BYTES) : remaining;
    return n<remaining ? n : remaining;
}
static void map_section_range(uintptr_t address,uint64_t phys,uint64_t size,DWORD protect) {
    for (uint64_t done=0;done<size;) {
        const uint64_t n=view_part_size(phys+done,size-done);
        if (n<size-done && !VirtualFree((void*)(address+done),n,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
            win_fail("splitting file view placeholder",address+done,n);
        const uint64_t physical=phys+done;
        HANDLE source=file_backed ? file_sections[physical/FILE_STORE_BYTES] : section;
        const uint64_t offset=file_backed ? physical%FILE_STORE_BYTES : physical;
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
        if (touch_commit && !VirtualAlloc(backing_base+r->phys,r->end-r->start,MEM_COMMIT,PAGE_READWRITE))
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

/* The upload source remains under the runtime table lock until memcpy ends.
 * Cache only committed runs: untouched reservations must be re-queried after
 * first-touch commits (which do not change the mapping generation). Mapping,
 * protection and zero-trim writers invalidate by table_generation. Private
 * release/promotion also invalidates when invoked under a nested read lock. */
static uint64_t bb_upload_release_epoch;
typedef struct { uintptr_t begin,end; } BbUploadExtent;
typedef struct {
    uint64_t generation,epoch;unsigned hand;
    BbUploadExtent entries[64];
} BbUploadCache;
static __thread BbUploadCache bb_upload;
static uint64_t bb_upload_query(unsigned char *source,int *committed) {
    uintptr_t at=(uintptr_t)source;
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    uint64_t epoch=__atomic_load_n(&bb_upload_release_epoch,__ATOMIC_ACQUIRE);
    if(bb_upload.generation!=generation || bb_upload.epoch!=epoch) {
        memset(bb_upload.entries,0,sizeof(bb_upload.entries));
        bb_upload.generation=generation;bb_upload.epoch=epoch;
    }
    if(!(generation&1)) for(unsigned j=0;j<64;++j) {
        BbUploadExtent *e=&bb_upload.entries[(bb_upload.hand-j)&63];
        if(e->begin<=at && at<e->end){*committed=1;return e->end-at;}
    }
    MEMORY_BASIC_INFORMATION info;
    if(!VirtualQuery(source,&info,sizeof(info)))win_fail("querying GPU upload backing",at,0);
    uintptr_t end=(uintptr_t)info.BaseAddress+info.RegionSize;
    if(end<=at)win_fail("invalid GPU upload backing extent",at,0);
    *committed=info.State!=MEM_RESERVE;
    if(!(generation&1) && info.State==MEM_COMMIT && !(info.Protect&(PAGE_NOACCESS|PAGE_GUARD)))
        bb_upload.entries[++bb_upload.hand&63]=(BbUploadExtent){(uintptr_t)info.BaseAddress,end};
    return end-at;
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
    __atomic_add_fetch(&bb_upload_release_epoch,1,__ATOMIC_RELEASE);
    if(!VirtualFree((void*)r->start,r->end-r->start,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) {
        const DWORD error=GetLastError();
        if(error!=ERROR_INVALID_PARAMETER && error!=ERROR_NOT_SUPPORTED)
            win_fail("releasing private CPU chunk to placeholder",r->start,r->end-r->start);
        /* Some Xbox builds reject preserving a replaced private allocation.
         * Release only this owned chunk and claim the exact free range again.
         * A competing host allocation makes reservation fail safely. */
        if(!VirtualFree((void*)r->start,0,MEM_RELEASE))win_fail("releasing owned private CPU chunk",r->start,PRIVATE_CHUNK);
        if(!VirtualAlloc2(NULL,(void*)r->start,PRIVATE_CHUNK,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
            win_fail("reclaiming private CPU placeholder",r->start,PRIVATE_CHUNK);
    }
    r->mapped=0;
}
static void private_promote(size_t index) {
    HostRegion *r=&regions[index];
    if(r->mapped!=2)return;
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
/* Leaves [a,b) as exactly one unmapped placeholder; returns its region index. */
static size_t carve_host(uintptr_t a, uintptr_t b) {
    if(private_cpu) {
        for(size_t i=region_index(a);i<region_count && regions[i].start<b;++i) {
            HostRegion *r=&regions[i];if(r->mapped!=2)continue;
            pager_bring_back(r->start,r->end); /* its contents are read below */
            if(a<=r->start && r->end<=b) {
                const int discard=r->phys>=POOL_SIZE || (discarding_private && discard_begin<=r->phys && r->phys+PRIVATE_CHUNK<=discard_end);
                private_save_unmapped(i,discard);
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
    int use_private=private_cpu && (cpu_only || private_gpu) && !reserved && !(address%PRIVATE_CHUNK) && !(phys%PRIVATE_CHUNK) && !(size%PRIVATE_CHUNK) && (prot&PROT_READ);
    if(private_cpu && !reserved) {
        for(size_t j=0;j<region_count;++j) {
            HostRegion *r=&regions[j];
            if(!r->mapped || r->phys>=phys+size || phys>=r->phys+(r->end-r->start))continue;
            use_private=0;
            if(r->mapped==2)private_promote(j);
        }
        if(use_private) for(uint64_t at=0;at<size;) {
            MEMORY_BASIC_INFORMATION info;
            if(!VirtualQuery(backing_base+phys+at,&info,sizeof(info)))return -1;
            if(info.State==MEM_COMMIT) { use_private=0;break; }
            uint64_t n=(uintptr_t)info.BaseAddress+info.RegionSize-(uintptr_t)(backing_base+phys+at);
            if(!n)return -1;at+=n;
        }
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
        const uint64_t begin=phys&~(PRIVATE_CHUNK-1),end=align_up(phys+size,PRIVATE_CHUNK);
        for(uint64_t at=begin;at<end;at+=PRIVATE_CHUNK)
            private_restore_shadow(at,backing_base+at,PAGE_READWRITE);
    }
    // Small CPU control/scratch mappings retain the original eager commitment.
    // Large CPU heaps can be tested separately from graphics and flexible memory.
    const int lazy_cpu=touch_cpu_min_bytes && size>=touch_cpu_min_bytes &&
                       phys<POOL_SIZE && block_type(phys)==0;
    if (!reserved && demand_commit && (!touch_commit || (!lazy_cpu && (phys>=POOL_SIZE || block_type(phys)!=3))) &&
        !VirtualAlloc(backing_base+phys,size,MEM_COMMIT,PAGE_READWRITE)) return -1;
    size_t i=carve_host(address,address+size);
    if (reserved) return 0;
    regions[i].phys=phys; regions[i].mapped=1; regions[i].initial_protect=win_prot(prot);
    view_map(&regions[i],win_prot(prot));
    return 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { carve_host(address,address+size); return 0; }
static SRWLOCK private_protect_lock; /* private_write.py */
static int host_protect_locked(uintptr_t address, uint64_t size, int prot);
static int host_protect(uintptr_t address, uint64_t size, int prot) {
    AcquireSRWLockExclusive(&private_protect_lock);
    const int result=host_protect_locked(address,size,prot);
    ReleaseSRWLockExclusive(&private_protect_lock);
    return result;
}
static int host_protect_locked(uintptr_t address, uint64_t size, int prot) {
    DWORD protect=win_prot(prot), old;
    uintptr_t end=address+size;
    for (size_t i=region_index(address); i<region_count && regions[i].start<end; ++i) {
        if (!regions[i].mapped) continue;
        uintptr_t a=regions[i].start>address ? regions[i].start : address, b=regions[i].end<end ? regions[i].end : end;
        if(regions[i].mapped==2) {
            pager_bring_back(regions[i].start,regions[i].end);
            if(!(prot&PROT_READ)) private_promote(i);
            else {
                // Removing a GPU watch must not physically allocate untouched
                // neighbours in the write-fault window. Their first access
                // already restores the initial read/write protection, and the
                // GPU tracker marks this entire unprotected range CPU-dirty.
                if(protect==PAGE_READWRITE && regions[i].initial_protect==PAGE_READWRITE) {
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
                if(!VirtualAlloc((void*)a,b-a,MEM_COMMIT,PAGE_READWRITE))return -1;
                if(!VirtualProtect((void*)a,b-a,protect,&old))return -1;
                continue;
            }
        }
        // GPU watches only protect ranges they actually use. Commit those
        // bytes before VirtualProtect; untouched portions retain view defaults.
        if (touch_commit && !VirtualAlloc(backing_base+regions[i].phys+(a-regions[i].start),b-a,MEM_COMMIT,PAGE_READWRITE)) return -1;
        if (!protect_section_range(a,regions[i].phys+(a-regions[i].start),b-a,protect)) return -1;
    }
    return 0;
}
/* Commit through the physical backing view, preserving every alias's own
 * protection. Only uncommitted pages belonging to this runtime are handled;
 * genuine protection faults still reach the existing GPU/guest handlers. */
/* bbport (Xbox, 2026-10-28): a commit that fails because the app is at its memory limit is
 * retried for up to 5 s. Such moments are transient: the GPU thread's superseded buffer arenas,
 * kept while one draw is built, are released at that draw's fence. */
static int bb_commit_retry(void *address, SIZE_T size, DWORD protect) {
    for (unsigned waited=0;;waited+=5) {
        if (VirtualAlloc(address,size,MEM_COMMIT,protect)) {
            if (waited) {
                static volatile long reports;
                if (__atomic_add_fetch(&reports,1,__ATOMIC_RELAXED)<=32)
                    fprintf(stderr,"Memory: commit at the app limit succeeded after %u ms\n",waited);
            }
            return 1;
        }
        const DWORD error=GetLastError();
        if ((error!=ERROR_COMMITMENT_LIMIT && error!=ERROR_NOT_ENOUGH_MEMORY && error!=ERROR_COMMITMENT_MINIMUM &&
             error!=ERROR_WORKING_SET_QUOTA) || waited>=5000) {
            SetLastError(error);
            return 0;
        }
        /* bbport (pager, 2026-11-03): cold guest chunks go to the pager's file first */
        static __thread unsigned room_tries;
        if (waited<20 && room_tries<4) {
            ++room_tries;
            const uint64_t made=pager_make_room((waited ? 256 : 64)+(size>>16));
            --room_tries;
            if (made) continue;
        }
        Sleep(5);
    }
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
    if (!VirtualQuery((void *)address,&info,sizeof(info)))
        return EXCEPTION_CONTINUE_SEARCH;
    if(info.State!=MEM_RESERVE) {
        if(record->ExceptionInformation[0]==1 && info.State==MEM_COMMIT && private_cpu) {
            read_lock();size_t r=region_index(address);
            MEMORY_BASIC_INFORMATION current;
            const int retry=r<region_count && regions[r].start<=address && regions[r].mapped==2 && regions[r].initial_protect==PAGE_READWRITE &&
                VirtualQuery((void*)address,&current,sizeof(current)) && (current.State==MEM_RESERVE || current.Protect==PAGE_READWRITE);
            read_unlock();if(retry)return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
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
    if(private_cpu && private_index<region_count && regions[private_index].start<=address && regions[private_index].mapped==2) {
        const uintptr_t page=address&~(uintptr_t)(private_touch_page-1);
        handled=bb_commit_retry((void*)page,private_touch_page,regions[private_index].initial_protect);
        if(!handled)win_fail("private CPU first-touch commit",page,private_touch_page);
        read_unlock();return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (physical!=UINT64_MAX) {
        TouchFault *f=&touch_history[touch_history_index++%8];
        CONTEXT *context=exception->ContextRecord;
        *f=(TouchFault){.rip=context->Rip,.address=address,.rsp=context->Rsp,.r11=context->R11,.r9=context->R9};
        SIZE_T got=0;
        ReadProcessMemory(GetCurrentProcess(),(void*)(context->Rsp-128),f->stack,sizeof(f->stack),&got);
        const uint64_t page=physical&~(PAGE-1);
        handled=bb_commit_retry(backing_base+page,PAGE,PAGE_READWRITE);
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
        if (handled) __atomic_add_fetch(&touch_faults,1,__ATOMIC_RELAXED);
    }
    read_unlock();
    return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
/* Released pages read as zero when reused, like the memfd's punched holes. */
static void host_discard(uint64_t phys, uint64_t size) {
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
/* Direct memory occupies [0,POOL_SIZE) of the section, flexible memory [POOL_SIZE,+FLEX_SPAN). */
/* bbport (Bloodborne Xbox, 2026-11-03): LocalState\pager.txt, read once when the pool is made (the
 * working directory is LocalState): "0" turns the pager off; "high=MB" (app memory it pages above;
 * default the limit - 1100), "dwell=S" (calm dwell, default 30), "trace=1" (a line every 2 s),
 * "events=0" (no event ring), "balloon=MB" (test: commit that much at start). */
static char pager_settings[512];
static int pager_trace_on;
static ULONGLONG pager_calm_dwell_ms=60000; /* pager_tune.py 2026-11-27: 60 s (old 30 s) */
static int pager_old; /* BB_PAGER_OLD=1 / pager.txt old=1: the pre-2026-11-27 thresholds */
static int pager_ahead=8, pager_cache_mib=32; /* pager_cache_ahead.py 2026-11-27: pager.txt ahead=N cache=MiB */
static long pager_setting(const char *key, long fallback) {
    if (!strcmp(key,"off")) return pager_settings[0]=='0';
    const char *at=strstr(pager_settings,key);
    return at ? strtol(at+strlen(key),NULL,10) : fallback;
}
static void pager_read_settings(void) {
    FILE *f=fopen("pager.txt","rb");
    if (f) { const size_t n=fread(pager_settings,1,sizeof(pager_settings)-1,f); pager_settings[n]=0; fclose(f); }
    const char *trace=getenv("BB_PAGER_TRACE"), *dwell=getenv("BB_PAGER_DWELL_S"), *old=getenv("BB_PAGER_OLD");
    pager_old=pager_setting("old=",old ? strtol(old,NULL,10) : 1)==1; /* pager_public_defaults.py: old by default */
    if (pager_old) pager_calm_dwell_ms=30000;
    pager_trace_on=pager_setting("trace=",trace ? strtol(trace,NULL,10) : 0)==1;
    pager_events_on=pager_setting("events=",1)!=0;
    pager_ahead=(int)pager_setting("ahead=",8); if (pager_ahead<0) pager_ahead=0; if (pager_ahead>32) pager_ahead=32;
    pager_cache_mib=(int)pager_setting("cache=",32); if (pager_cache_mib<1) pager_ahead=0; if (pager_cache_mib>256) pager_cache_mib=256;
    const long s=pager_setting("dwell=",dwell ? strtol(dwell,NULL,10) : 0);
    if (s>0) pager_calm_dwell_ms=(ULONGLONG)s*1000;
    if (f) { for (char *c=pager_settings;*c;++c) if (*c=='\r' || *c=='\n') *c=' '; printf("Pager: pager.txt \"%s\"\n",pager_settings); }
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
#define PAGER_HIGH_BELOW_LIMIT_MB 1100 /* pager_tune.py 2026-11-27: kept (850 tried: more slow seconds) */
#define PAGER_OLD_HIGH_BELOW_LIMIT_MB 1100
#define PAGER_TUNED_FAST_MB 750
#define PAGER_TUNED_FAST_DWELL_MS 10000
#define PAGER_CALM_DWELL_MS 30000
#define PAGER_COOLDOWN_S 120
#define PAGER_URGENT_COOLDOWN_S 30
typedef struct { uint32_t slot; uint8_t state, view, pinned, init; uint8_t prot[16]; uint32_t used_at; } PagerEntry; /* prot: 0 none, 1 RO, 2 RW; used_at: s, last time it came back or was touched swept */
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
static void pager_restore(uintptr_t chunk, PagerEntry *e) {
    e->used_at=pager_now_s(); /* in use (or let go): not swept again for PAGER_COOLDOWN_S */
    for (int p=0;p<16;) {
        const uint8_t code=e->prot[p];
        int q=p+1;
        while (q<16 && e->prot[q]==code) ++q;
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
/* pager_cache_ahead.py 2026-11-27: chunks read ahead into RAM the game never sees. An entry is valid with
 * chunk + slot + checksum; it is made invalid (under pager_lock) before its buffer is written again. */
typedef struct { uintptr_t chunk; uint32_t slot; int valid; } PagerCacheEntry;
static PagerCacheEntry *pager_cache; static unsigned char *pager_cache_data; static unsigned pager_cache_n, pager_cache_next;
static volatile LONG64 pager_cache_hits, pager_cache_filled, pager_cache_bad, pager_read_us, pager_total_us, pager_timed;
static double pager_qpc_us;
static uint64_t pager_qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
static int pager_cache_find(uintptr_t chunk, uint32_t slot) { /* pager_lock held */
    for (unsigned i=0;i<pager_cache_n;++i) if (pager_cache[i].valid && pager_cache[i].chunk==chunk && pager_cache[i].slot==slot) return (int)i;
    return -1;
}
/* pager_lock held: the cached copy of the chunk into buf (entry used up), 1 when it matched its checksum */
static int pager_cache_take(uintptr_t chunk, uint32_t slot, unsigned char *buf) {
    if (!pager_cache_n) return 0;
    const int i=pager_cache_find(chunk,slot);
    if (i<0) return 0;
    pager_cache[i].valid=0;
    memcpy(buf,pager_cache_data+(size_t)i*PRIVATE_CHUNK,PRIVATE_CHUNK);
    if (slot>=pager_sums_capacity || !pager_sums[slot] || pager_checksum(buf)!=pager_sums[slot]) { _InterlockedExchangeAdd64(&pager_cache_bad,1); return 0; }
    _InterlockedExchangeAdd64(&pager_cache_hits,1);
    return 1;
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
static __thread int pager_filling;
static unsigned char *pager_fill_buffer;
/* 1 when the chunk is back (here or by another page-in), 0 when the commit failed at the memory
 * limit and room could not be made from this thread (the table is held elsewhere: see
 * try_read_lock); the caller makes room itself and tries again. */
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
        if (!filled) {
            if (!pager_cache_take(chunk,e->slot,fill)) {
                const uint64_t r0=pager_qpc();
                if (!pager_slot_io(e->slot,fill,0)) win_fail("pager: reading a chunk back from the file",chunk,PRIVATE_CHUNK);
                if (pager_qpc_us>0) _InterlockedExchangeAdd64(&pager_read_us,(LONG64)((pager_qpc()-r0)/pager_qpc_us));
            }
            if (e->slot<pager_sums_capacity && pager_sums[e->slot] && pager_checksum(fill)!=pager_sums[e->slot]) {
                if (_InterlockedExchangeAdd64(&pager_corrupt,1)<16) {
                    fprintf(stderr,"Pager: the chunk at %p came back different from what went out (slot %u)\n",(void*)chunk,e->slot);
                    fflush(stderr);
                }
            }
            filled=1;
        }
        /* No access (2026-11-16, as TLOU 2026-11-15), not guard pages: a guard stops only the first
         * touch of a page. Once one thread had taken a page's guard and waited, others read that page
         * as zeros until the copy reached it ("differs from its file copy after the page-in"). A
         * no-access page faults every touch (pager_fault waits for pager_lock). */
        at=no_guard ? NULL : VirtualAlloc2(NULL,(void*)chunk,PRIVATE_CHUNK,MEM_RESERVE|MEM_COMMIT|MEM_REPLACE_PLACEHOLDER,
                                           PAGE_NOACCESS,NULL,0);
        DWORD error=at ? 0 : GetLastError();
        const int at_limit=error==ERROR_COMMITMENT_LIMIT || error==ERROR_NOT_ENOUGH_MEMORY || error==ERROR_WORKING_SET_QUOTA;
        if (!at && !no_guard && !at_limit) {
            no_guard=1;
            fprintf(stderr,"Pager: no-access private memory was refused (error %lu): chunks come back read-write, then protected\n",error);
            fflush(stderr);
        }
        if (!at && no_guard) {
            at=VirtualAlloc2(NULL,(void*)chunk,PRIVATE_CHUNK,MEM_RESERVE|MEM_COMMIT|MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0);
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
    /* Page by page (2026-11-16): each page is opened right before its 4 KiB copy, so a page shows zeros
     * to another thread for about a microsecond, not until the copy of the whole chunk gets there.
     * Read-only pages (GPU write watches) are opened read-write for their copy, then closed. */
    pager_filling=1;
    for (int p=0;p<16;++p) {
        const uint8_t code=e->prot[p] ? e->prot[p] : e->init;
        unsigned char *page=(unsigned char*)(chunk+(uintptr_t)p*4096);
        DWORD old;
        if (!no_guard && !VirtualProtect(page,4096,PAGE_READWRITE,&old))
            win_fail("pager: opening a page that came back",(uintptr_t)page,4096);
        memcpy(page,fill+(size_t)p*4096,4096);
        if (code==1 && !VirtualProtect(page,4096,PAGE_READONLY,&old))
            win_fail("pager: protecting a chunk that came back",chunk,PRIVATE_CHUNK);
    }
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
    pager_free_slot(e->slot);
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
    const uint64_t t0=pager_qpc();
    const int r=pager_page_in_private(chunk,e,GetTickCount64());
    if (pager_qpc_us>0) { _InterlockedExchangeAdd64(&pager_total_us,(LONG64)((pager_qpc()-t0)/pager_qpc_us)); _InterlockedExchangeAdd64(&pager_timed,1); }
    return r;
}
/* Page-ins run here for faults and for bring-backs on small stacks: the asking thread only waits. */
typedef struct PageInRequest { uintptr_t chunk; int done, result; struct PageInRequest *next; } PageInRequest;
static pthread_mutex_t pagein_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pagein_wake=PTHREAD_COND_INITIALIZER, pagein_done=PTHREAD_COND_INITIALIZER;
static PageInRequest *pagein_queue;
/* pager_cache_ahead.py: the evicted neighbours of a chunk a fault just brought back, read into the cache on the
 * helper after the waiting thread is released (the game's memory is not touched). */
static uint64_t pager_app_usage(void);
static void pager_cache_ahead(uintptr_t asked) {
    if (!pager_cache) {
        const unsigned n=(unsigned)pager_cache_mib*16u;
        pager_cache_data=VirtualAlloc(NULL,(SIZE_T)n*PRIVATE_CHUNK,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        pager_cache=pager_cache_data ? calloc(n,sizeof(*pager_cache)) : NULL;
        if (!pager_cache) { pager_ahead=0; fprintf(stderr,"Pager: no read-ahead cache (%d MiB refused)\n",pager_cache_mib); fflush(stderr); return; }
        pager_take(); pager_cache_n=n; pager_give();
    }
    for (int dir=1; dir>=-1; dir-=2) {
        const int n=dir>0 ? pager_ahead : pager_ahead/4;
        for (int k=1; k<=n; ++k) {
            if (__atomic_load_n(&pagein_queue,__ATOMIC_ACQUIRE)) return; /* a thread waits: it goes first */
            if (pager_app_usage()+((uint64_t)PAGER_URGENT_MB<<20)>pager_limit) return;
            const uintptr_t chunk=dir>0 ? asked+(uintptr_t)k*PRIVATE_CHUNK : asked-(uintptr_t)k*PRIVATE_CHUNK;
            pager_take();
            PagerEntry *e=pager_entry(chunk,0);
            const int evicted=e && e->state==PG_EVICTED && !e->pinned;
            const uint32_t slot=evicted ? e->slot : 0;
            int idx=-1;
            if (evicted && pager_cache_find(chunk,slot)<0 && slot<pager_sums_capacity && pager_sums[slot]) {
                idx=(int)(pager_cache_next++%pager_cache_n);
                pager_cache[idx].valid=0; /* its buffer is written next, outside the lock */
            }
            pager_give();
            if (!evicted) break;
            if (idx<0) continue;
            unsigned char *buf=pager_cache_data+(size_t)idx*PRIVATE_CHUNK;
            if (!pager_slot_io(slot,buf,0)) continue;
            const uint64_t sum=pager_checksum(buf);
            pager_take();
            e=pager_entry(chunk,0);
            if (e && e->state==PG_EVICTED && e->slot==slot && slot<pager_sums_capacity && pager_sums[slot]==sum) {
                pager_cache[idx].chunk=chunk; pager_cache[idx].slot=slot; pager_cache[idx].valid=1;
                _InterlockedExchangeAdd64(&pager_cache_filled,1);
            }
            pager_give();
        }
    }
}
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
        const int asked_evicted=e && e->state==PG_EVICTED; /* pager_cache_ahead.py */
        if (asked_evicted) result=pager_page_in(request->chunk,e);
        else if (e && e->state) { pager_restore(request->chunk,e); _InterlockedExchangeAdd64(&pager_touched,1); }
        pager_give();
        const uintptr_t asked=request->chunk; /* the request lives on the waiter's stack */
        const int was_evicted=result && asked_evicted;
        pthread_mutex_lock(&pagein_mutex);
        request->result=result;
        request->done=1;
        pthread_cond_broadcast(&pagein_done);
        pthread_mutex_unlock(&pagein_mutex);
        if (was_evicted && pager_ahead>0) pager_cache_ahead(asked);
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
        } else if (m.State!=MEM_RESERVE) return 0;
        at=next;
    }
    if (!committed) return 0;
    memcpy(e->prot,prot,16);
    e->init=regions[index].initial_protect==PAGE_READONLY ? 1 : 2;
    for (int p=0;p<16;) {
        int q=p+1;
        while (q<16 && (e->prot[q]!=0)==(e->prot[p]!=0)) ++q;
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
    if (!e || e->state!=PG_SWEPT || i>=region_count || regions[i].start!=chunk || regions[i].mapped!=2) { pager_give(); read_unlock(); return; }
    for (int p=0;p<16;) { /* one call per run of committed pages (it was one per page) */
        int q=p+1;
        while (q<16 && (e->prot[q]!=0)==(e->prot[p]!=0)) ++q;
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
    const uint32_t slot=pager_alloc_slot();
    pager_give();
    read_unlock();
    const uint64_t sum=pager_checksum(copy);
    const int written=pager_slot_io(slot,copy,1); /* a writer meanwhile faults and stops the eviction */
    read_lock();
    pager_take();
    i=region_index(chunk);
    int left=0;
    if (written && e->state==PG_EVICTING && i<region_count && regions[i].start==chunk && regions[i].mapped==2) {
        int same=1;
        for (int p=0;p<16 && same;++p) if (e->prot[p]) same=!memcmp(copy+p*4096,(void*)(chunk+(uintptr_t)p*4096),4096);
        if (same) {
            const BOOL released=VirtualFree((void*)chunk,PRIVATE_CHUNK,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER);
            if (released) {
                /* Bloodborne: the upload commit cache (bb_upload_query) holds no released chunk */
                __atomic_add_fetch(&bb_upload_release_epoch,1,__ATOMIC_RELEASE);
                pager_set_sum(slot,sum);
                e->slot=slot;
                __atomic_store_n(&e->state,PG_EVICTED,__ATOMIC_RELEASE);
                left=1;
                _InterlockedExchangeAdd64(&pager_evicted,1);
                runtime_trace_event(TR_EVICT,chunk,slot,NULL);
            } else _InterlockedExchangeAdd64(&pager_failed,1);
        }
        if (!left) pager_restore(chunk,e);
    }
    if (!left) { pager_free_slot(slot); _InterlockedExchangeAdd64(&pager_aborted,1); runtime_trace_event(TR_EVICT_ABORT,chunk,written,NULL); }
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
/* Bloodborne (2026-11-03): no make-room lock. The region table lock is a recursive critical
 * section here: a caller that holds it (a first-touch fault, a host map) waiting for another
 * make-room, which waits for the table, would never return. Calls run side by side, each with
 * one of ROOM_COPIES copy buffers (none free: nothing is made, the caller waits and retries). */
#define ROOM_COPIES 4
static unsigned char *room_copies[ROOM_COPIES];
static volatile LONG room_copies_busy;
static uintptr_t room_hand;
static volatile LONG64 pager_room_made, pager_room_calls, pager_room_warm;
static uint64_t pager_make_room(uint64_t want) {
    if (!pager_active || pager_holding || !room_copies[0] || !pager_open_store()) return 0;
    int copy_slot=-1;
    for (int k=0;k<ROOM_COPIES && copy_slot<0;++k)
        if (room_copies[k] && !(InterlockedOr(&room_copies_busy,1L<<k)&(1L<<k))) copy_slot=k;
    if (copy_slot<0) return 0;
    unsigned char *room_copy=room_copies[copy_slot];
    uint64_t made=0, warm=0;
    for (int pass=0; pass<3 && made<want; ++pass) {
        size_t examined=0;
        while (made<want && examined<region_count) {
            uintptr_t picked[32]; unsigned n=0;
            read_lock();
            pager_take();
            size_t i=region_index(__atomic_load_n(&room_hand,__ATOMIC_RELAXED)), seen=0;
            for (; i<region_count && seen<512 && n<32 && made+n<want; ++i, ++seen) {
                if (regions[i].mapped!=2) continue;
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
            __atomic_store_n(&room_hand,i<region_count ? regions[i].start : 0,__ATOMIC_RELAXED);
            pager_give();
            read_unlock();
            examined+=seen ? seen : 1;
            for (unsigned k=0; k<n; ++k) {
                const LONG64 before=pager_evicted;
                pager_evict(picked[k],room_copy);
                if (pager_evicted!=before) { ++made; warm+=pass==2; }
            }
            if (i>=region_count) __atomic_store_n(&room_hand,0,__ATOMIC_RELAXED);
        }
    }
    InterlockedAnd(&room_copies_busy,~(1L<<copy_slot));
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
        if (pager_trace_on && now-last_line>=2000) {
            last_line=now;
            /* Crash 3 failed commit at app ~4500 MiB (crash 1: 4808; private balloon: 4888): the
             * system's own count of commit left, to see what else uses the budget. */
            MEMORYSTATUSEX ms={.dwLength=sizeof(ms)};
            if (GlobalMemoryStatusEx(&ms))
                fprintf(stderr,"Pager memory t=%llu: commit left %llu of %llu MiB, physical free %llu of %llu MiB, app %llu MiB\n",
                        (unsigned long long)(now/1000),(unsigned long long)(ms.ullAvailPageFile>>20),(unsigned long long)(ms.ullTotalPageFile>>20),
                        (unsigned long long)(ms.ullAvailPhys>>20),(unsigned long long)(ms.ullTotalPhys>>20),(unsigned long long)(usage>>20));
            fprintf(stderr,"Pager trace t=%llu: app %llu MiB, file %lld, out %lld, back %lld, aborted %lld, swept %lld, touched %lld, let go %lld, views %lld, failed %lld, queue %zu, fiber stacks pinned %lld (%lld KiB), came back different %lld, guard waits %lld, read retries %lld, host pins %lld, fill diffs %lld, cache filled %lld hits %lld bad %lld, page-in %lld us (disk read %lld us) over %lld\n",
                    (unsigned long long)(now/1000),(unsigned long long)(usage>>20),(long long)pager_in_file,(long long)pager_evicted,
                    (long long)pager_page_ins,(long long)pager_aborted,(long long)pager_swept,(long long)pager_touched,
                    (long long)pager_released,(long long)pager_views,(long long)pager_failed,(head+QUEUE-tail)%QUEUE,
                    (long long)pager_stacks,(long long)(pager_stack_bytes>>10),(long long)pager_corrupt,(long long)pager_guard_waits,
                    (long long)pager_io_retries,(long long)pager_host_pins,(long long)pager_fill_diffs,
                    (long long)pager_cache_filled,(long long)pager_cache_hits,(long long)pager_cache_bad,
                    (long long)pager_total_us,(long long)pager_read_us,(long long)pager_timed);
            fflush(stderr);
        }
        {
            static ULONGLONG reported; static LONG64 last_out=-1, last_in=-1;
            if (now-reported>=30000 && (pager_evicted!=last_out || pager_page_ins!=last_in)) {
                reported=now; last_out=pager_evicted; last_in=pager_page_ins;
                runtime_memory_report_pager();
            }
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
        /* pager_tune.py 2026-11-27: thrashing made the pager fast (5 s dwell), evicting the working set
         * harder; now it slows down (dwell doubled, calm pace) unless the app is near the limit */
        const int near_limit=usage+((uint64_t)(pager_old ? PAGER_FAST_MB : PAGER_TUNED_FAST_MB)<<20)>pager_limit;
        const int rush=pager_old ? (urgent || thrashing || climbing || near_limit) : (urgent || near_limit || (climbing && !thrashing));
        const int slow=!pager_old && thrashing && !rush;
        pager_urgent=urgent;
        static int last_level=0;
        const int level=urgent ? 2 : slow ? 4 : thrashing ? 3 : rush ? 1 : 0;
        if (level!=last_level) {
            printf("Pager: pressure %s (app %llu MiB, +%llu MiB in 10 s; 10 s: %lld out, %lld back)\n",
                   level==2 ? "urgent" : level==4 ? "slow (thrashing)" : level==3 ? "fast (thrashing)" : level ? "fast" : "calm",
                   (unsigned long long)(usage>>20),(unsigned long long)((usage-lowest)>>20),(long long)out10,(long long)in10);
            fflush(stdout);
            last_level=level;
        }
        /* 1. swept chunks a dwell ago: out while above the low mark, else let go */
        const unsigned out_cap=urgent ? 512 : rush ? 256 : PAGER_OUT_PER_LOOP;
        const ULONGLONG dwell=urgent ? 3000 : rush ? (pager_old ? PAGER_DWELL_MS : PAGER_TUNED_FAST_DWELL_MS) :
                              slow ? 2*pager_calm_dwell_ms : pager_calm_dwell_ms;
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
                    if (regions[i].mapped!=2) continue;
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
    pager_read_settings();
    trace_start();
    HMODULE self=NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)pager_start,&self)) {
        pager_host_lo=(uintptr_t)self;
        pager_host_hi=pager_host_lo+((const IMAGE_NT_HEADERS*)(pager_host_lo+((const IMAGE_DOS_HEADER*)self)->e_lfanew))->OptionalHeader.SizeOfImage;
    }
    const char *env=getenv("BB_PAGER"), *limit=getenv("BB_APP_LIMIT_MB"), *high=getenv("BB_PAGER_HIGH_MB");
    pager_limit=limit ? strtoull(limit,NULL,10)<<20 : 0;
    if (!pager_limit) {
        /* Bloodborne: the app's own limit (Xbox: 5120 MiB), when it has one */
        APP_MEMORY_INFORMATION app={0};
        if (GetProcessInformation(GetCurrentProcess(),ProcessAppMemoryInfo,&app,sizeof(app))) {
            const uint64_t l=app.TotalCommitUsage+app.AvailableCommit;
            if (l>=(UINT64_C(2)<<30) && l<=(UINT64_C(16)<<30)) pager_limit=l;
        }
    }
    const int off=(env && env[0]=='0') || pager_setting("off",0);
    if (off || !pager_limit || !private_cpu) {
        printf("Pager: off (%s)\n",off ? "BB_PAGER=0 or pager.txt" : !pager_limit ? "no app memory limit" : "no private chunks");
        return;
    }
    const long high_mb=pager_setting("high=",high ? strtol(high,NULL,10) : 0);
    pager_high=high_mb>0 ? (uint64_t)high_mb<<20 :
        pager_limit-((uint64_t)(pager_old ? PAGER_OLD_HIGH_BELOW_LIMIT_MB : PAGER_HIGH_BELOW_LIMIT_MB)<<20);
    printf("Pager: %s thresholds (pager_tune.py 2026-11-27; BB_PAGER_OLD)\n",pager_old ? "old" : "tuned");
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); pager_qpc_us=(double)f.QuadPart/1e6; }
    printf("Pager: cached read-ahead %d chunks forward, %d back, cache %d MiB (pager_cache_ahead.py; pager.txt ahead= cache=)\n",
           pager_ahead,pager_ahead/4,pager_cache_mib);
    pager_low=pager_high-(UINT64_C(256)<<20);
    if (!AddVectoredExceptionHandler(1,pager_fault)) { printf("Pager: off (no fault handler)\n"); return; }
    /* make-room's copy buffer exists before memory is tight (allocating it then could fail) */
    for (int k=0;k<ROOM_COPIES;++k) room_copies[k]=VirtualAlloc(NULL,PRIVATE_CHUNK,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    /* BB_TEST_BALLOON_MB=N (test only): N MiB committed and touched at start, so the game meets
     * the real commit limit early and make-room is exercised. */
    const char *balloon_env=getenv("BB_TEST_BALLOON_MB");
    const long balloon=pager_setting("balloon=",balloon_env ? strtol(balloon_env,NULL,10) : 0);
    if (balloon>0) {
        const SIZE_T bytes=(SIZE_T)balloon<<20;
        unsigned char *b=VirtualAlloc(NULL,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if (b) for (SIZE_T at=0;at<bytes;at+=4096) b[at]=1;
        printf("Pager: test balloon of %ld MiB %s\n",balloon,b ? "committed" : "refused");
    }
    pager_active=1;
    HANDLE t1=CreateThread(NULL,0,pager_thread,NULL,0,NULL), t2=CreateThread(NULL,0,pagein_thread,NULL,0,NULL);
    if (t1) CloseHandle(t1);
    if (t2) CloseHandle(t2);
    printf("Pager: on above %llu MiB of app memory (down to %llu MiB), chunks untouched for %d s go to a file; app memory now %llu MiB\n",
           (unsigned long long)(pager_high>>20),(unsigned long long)(pager_low>>20),(int)(pager_calm_dwell_ms/1000),
           (unsigned long long)(pager_app_usage()>>20));
    printf("Pager: app memory limit %llu MiB (%s)%s\n",(unsigned long long)(pager_limit>>20),limit ? "BB_APP_LIMIT_MB" : "app memory information",
           pager_trace_on ? "; trace lines every 2 s" : "");
}
void runtime_memory_report_pager(void) {
    if (!pager_active) return;
    const LONG64 ins=pager_page_ins;
    printf("Pager: %lld MiB in the file; out %lld MiB, back %lld MiB (%lld us each), stopped by a touch %lld, swept %lld, "
           "touched while swept %lld, let go %lld, views %lld, failed %lld; cache hits %lld of %lld cached, page-in %lld us of which disk read %lld us; app memory %llu MiB (above %llu MiB it pages)\n",
           (long long)(pager_in_file*64/1024),(long long)(pager_evicted*64/1024),(long long)(ins*64/1024),
           (long long)(ins ? pager_page_in_us/ins : 0),(long long)pager_aborted,(long long)pager_swept,(long long)pager_touched,
           (long long)pager_released,(long long)pager_views,(long long)pager_failed,
           (long long)pager_cache_hits,(long long)pager_cache_filled,
           (long long)(pager_timed ? pager_total_us/pager_timed : 0),(long long)(pager_timed ? pager_read_us/pager_timed : 0),
           (unsigned long long)(pager_app_usage()>>20),(unsigned long long)(pager_high>>20));
}

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
        printf("Runtime: %s mappings use private 64 KiB chunks; physical aliases and GPU writeback promote to shared backing\n",private_gpu ? "single-mapped CPU/GPU" : "CPU-only");
    }
    const char *lazy=getenv("BB_UWP_DEMAND_COMMIT");
    demand_commit=lazy && lazy[0]=='1';
    const char *touch=getenv("BB_UWP_TOUCH_COMMIT");
    touch_commit=demand_commit && touch && touch[0]=='1';
    const char *cpu_min=getenv("BB_UWP_TOUCH_CPU_MIN_MB");
    touch_cpu_min_bytes=touch_commit && cpu_min ? strtoull(cpu_min,NULL,10)*1024*1024 : 0;
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
    section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,demand_commit ? SEC_RESERVE : SEC_COMMIT,bytes,NULL,NULL,0);
    if (!section) { fprintf(stderr,"Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",bytes>>20,GetLastError()); return -1; }
    backing_base=MapViewOfFile3(section,GetCurrentProcess(),NULL,0,bytes,0,PAGE_READWRITE,NULL,0);
    if (!backing_base) { CloseHandle(section); section=NULL; }
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
    if (pending_count==sizeof(pending)/sizeof(*pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); exit(21); }
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
        Vma *next=realloc(vmas,capacity*sizeof(*vmas));
        if (!next) return -1;
        vmas=next; vma_capacity=capacity;
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
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
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
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
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
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
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
    (void)name; return map_flexible(inout,size,prot,flags);
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
    strcpy(info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : "flexible");
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
    write_lock();
    for (; done<count; ++done) {
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
static const RuntimeExport exports[]={
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
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
#define LOW_MIN UINT64_C(0x0800000000)
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
/* XStation 2026-11-23 (private_write.py): GPU writes into a private chunk, in place. NtWriteVirtualMemory
 * (not WriteProcessMemory: that one changes the page protection and restores it, racing the GPU tracker's own
 * protection changes; f2-fix hung at boot like the guarded promotion did) fails on a read-only or no-access page
 * instead of faulting or touching protections; such writes promote the chunk as before (counted). */
typedef LONG (NTAPI *BbNtWriteVirtualMemory)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
static BbNtWriteVirtualMemory bb_nt_write;
static volatile LONG64 private_direct_writes, private_direct_fallbacks, private_direct_ro, private_direct_noaccess;
static int private_direct=-1;
static volatile LONG64 private_direct_opened;
/* host_protect (the GPU tracker's protection changes) and the read-only case below take this lock: a page
 * opened for a GPU write is closed again before the tracker can change it, so no stale protection is restored. */
static SRWLOCK private_protect_lock=SRWLOCK_INIT; /* tentatively declared at host_protect */

static volatile LONG64 private_direct_raced;
static SRWLOCK private_race_lock=SRWLOCK_INIT;
static CONDITION_VARIABLE private_race_wake=CONDITION_VARIABLE_INIT;
static uintptr_t private_race_pages[1024]; static unsigned private_race_head, private_race_tail;
static DWORD WINAPI private_race_thread(void *unused) {
    (void)unused;
    for (;;) {
        AcquireSRWLockExclusive(&private_race_lock);
        while (private_race_head==private_race_tail) SleepConditionVariableSRW(&private_race_wake,&private_race_lock,INFINITE,0);
        const uintptr_t page=private_race_pages[private_race_tail++%1024];
        ReleaseSRWLockExclusive(&private_race_lock);
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery((void*)page,&info,sizeof(info)) && info.State==MEM_COMMIT &&
            ((info.Protect&0xff)==PAGE_READONLY || (info.Protect&0xff)==PAGE_READWRITE))
            __atomic_fetch_or((volatile uint32_t*)page,0u,__ATOMIC_SEQ_CST); /* a write: faults to the tracker if watched */
    }
    return 0;
}
static void private_race_queue(uintptr_t page) {
    static volatile LONG started;
    if (!InterlockedExchange(&started,1)) { HANDLE h=CreateThread(NULL,0,private_race_thread,NULL,0,NULL); if (h) CloseHandle(h); }
    AcquireSRWLockExclusive(&private_race_lock);
    if (private_race_head-private_race_tail<1024) private_race_pages[private_race_head++%1024]=page;
    ReleaseSRWLockExclusive(&private_race_lock);
    WakeConditionVariable(&private_race_wake);
}
static int bb_private_write(size_t r, uintptr_t at, const unsigned char *src, uint64_t n) {
    if (private_direct<0) {
        const char *v=getenv("BB_PRIVATE_DIRECT_WRITE");
        HMODULE nt=GetModuleHandleW(L"ntdll.dll");
        bb_nt_write=nt ? (BbNtWriteVirtualMemory)(void*)GetProcAddress(nt,"NtWriteVirtualMemory") : NULL;
        private_direct=!(v && v[0]=='0') && bb_nt_write;
        printf("Runtime: GPU writes into private chunks %s (BB_PRIVATE_DIRECT_WRITE%s)\n",private_direct ? "in place" : "promote the chunk",
               bb_nt_write ? "" : "; NtWriteVirtualMemory missing");
        fflush(stdout);
    }
    if (!private_direct) return 0;
    pager_bring_back(at,at+n);
    if (regions[r].mapped!=2) return 0;
    for (uintptr_t p=at&~(uintptr_t)(private_touch_page-1); p<at+n; p+=private_touch_page) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void*)p,&info,sizeof(info))) return 0;
        if (info.State==MEM_RESERVE && !bb_commit_retry((void*)p,private_touch_page,regions[r].initial_protect)) return 0;
    }
    SIZE_T done=0;
    LONG status=bb_nt_write(GetCurrentProcess(),(void*)at,(void*)src,(SIZE_T)n,&done);
    if (status<0 || done!=n) {
        /* 2026-11-23 (wm1: 845 promotions from read-only fallbacks, one lost 7 bytes of commands = a "splice"):
         * read-only pages (GPU tracker watch) are opened under the protection lock, written, closed. */
        /* retried: a page can change between the query and the write (pager sweep, the write watch) */
        for (int attempt=0; attempt<16 && (status<0 || done!=n); ++attempt) {
        if (attempt) { pager_bring_back(at,at+n); if (regions[r].mapped!=2) return 0; SwitchToThread(); }
        uintptr_t runs[32][2]; unsigned char *snaps[32]; int count=0, ok=1;
        AcquireSRWLockExclusive(&private_protect_lock);
        for (uintptr_t q=at; q<at+n && ok;) {
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery((void*)q,&info,sizeof(info)) || info.State!=MEM_COMMIT) { ok=0; break; }
            uintptr_t e=(uintptr_t)info.BaseAddress+info.RegionSize; if (e>at+n) e=at+n;
            const DWORD base=info.Protect&0xff;
            if (base==PAGE_READONLY) {
                const uintptr_t a=q&~(uintptr_t)4095, b=(e+4095)&~(uintptr_t)4095;
                DWORD old;
                if (count==32) { ok=0; break; }
                snaps[count]=(b-a<=(1u<<20)) ? malloc(b-a) : NULL; /* what the page held before it was opened */
                if (snaps[count]) memcpy(snaps[count],(const void*)a,b-a);
                if (!VirtualProtect((void*)a,b-a,PAGE_READWRITE,&old)) { free(snaps[count]); ok=0; break; }
                runs[count][0]=a; runs[count][1]=b; ++count;
            } else if (base!=PAGE_READWRITE && base!=PAGE_EXECUTE_READWRITE) { ok=0; break; }
            q=e;
        }
        if (ok) { done=0; status=bb_nt_write(GetCurrentProcess(),(void*)at,(void*)src,(SIZE_T)n,&done); }
        for (int k=0;k<count;++k) {
            MEMORY_BASIC_INFORMATION info; DWORD old;
            if (VirtualQuery((void*)runs[k][0],&info,sizeof(info)) && info.Protect==PAGE_READWRITE)
                VirtualProtect((void*)runs[k][0],runs[k][1]-runs[k][0],PAGE_READONLY,&old);
        }
        ReleaseSRWLockExclusive(&private_protect_lock);
        const int wrote=ok && status>=0 && done==n;
        for (int k=0;k<count;++k) { /* other writes that landed while the page was open */
            for (uintptr_t pg=runs[k][0]; pg<runs[k][1]; pg+=4096) {
                int changed=!snaps[k];
                const unsigned char *now=(const unsigned char*)pg, *before=snaps[k] ? snaps[k]+(pg-runs[k][0]) : NULL;
                for (size_t i=0;i<4096 && !changed;++i) {
                    const uintptr_t x=pg+i;
                    const unsigned char want=(wrote && x>=at && x<at+n) ? src[x-at] : before[i];
                    changed=now[i]!=want;
                }
                if (changed) {
                    const LONG64 c=_InterlockedIncrement64(&private_direct_raced);
                    if (!(c&(c-1))) { printf("Private write: %lld pages changed by another writer while opened for a GPU write (last %p); handed to the GPU tracker\n",(long long)c,(void*)pg); fflush(stdout); }
                    private_race_queue(pg);
                }
            }
            free(snaps[k]);
        }
        if (wrote) { _InterlockedIncrement64(&private_direct_opened); goto written; }
        if (!ok) status=-1;
        }
    }
    if (status<0 || done!=n) {
        MEMORY_BASIC_INFORMATION info; DWORD protect=0;
        if (VirtualQuery((void*)(at+done),&info,sizeof(info))) protect=info.Protect;
        if ((protect&0xff)==PAGE_READONLY || (protect&0xff)==PAGE_EXECUTE_READ) _InterlockedIncrement64(&private_direct_ro);
        else if ((protect&0xff)==PAGE_NOACCESS) _InterlockedIncrement64(&private_direct_noaccess);
        const LONG64 f=_InterlockedIncrement64(&private_direct_fallbacks);
        if (!(f&(f-1))) { printf("Private write: %lld direct, %lld fell back to promotion (%lld read-only, %lld no-access; status %#lx, protect %#lx at %p)\n",
                                 (long long)private_direct_writes,(long long)f,(long long)private_direct_ro,(long long)private_direct_noaccess,
                                 (unsigned long)status,(unsigned long)protect,(void*)(at+done)); fflush(stdout); }
        return 0;
    }
written:;
    const LONG64 w=_InterlockedIncrement64(&private_direct_writes);
    if (!(w&(w-1)) && w<=(1<<24)) { printf("Private write: %lld direct (%lld through read-only pages), %lld fell back to promotion (%lld read-only, %lld no-access)\n",(long long)w,
                                           (long long)private_direct_opened,(long long)private_direct_fallbacks,(long long)private_direct_ro,(long long)private_direct_noaccess); fflush(stdout); }
    return 1;
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
                /* XStation 2026-11-23 (private_write.py): in place, no promotion copy that loses concurrent writes */
                if(regions[r].mapped==2 && bb_private_write(r,at,(const unsigned char *)data+(at-address),n)) { at+=n; continue; }
                if(regions[r].mapped==2)private_promote(r);
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
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {
    unsigned char *out=data;
    read_lock();
    for (uintptr_t at=address, end=address+size; at<end;) {
        const size_t i=vma_index(at);
        uint64_t n;
        if (i<vma_count && vmas[i].start<=at && vmas[i].kind!=KIND_RESERVED) {
            n=(vmas[i].end<end ? vmas[i].end : end)-at;
            unsigned char *source=backing_base+vmas[i].phys+(at-vmas[i].start);
#ifdef _WIN32
            if(private_cpu) {
                size_t r=region_index(at);
                if(r<region_count && regions[r].start<=at) {
                    if(n>regions[r].end-at)n=regions[r].end-at;
                    /* a chunk in the pager's file would read as reserved, zeros: back first */
                    if(regions[r].mapped==2)pager_bring_back(at,at+n);
                    if(regions[r].mapped==2)source=(unsigned char*)at;
                }
            }
            /* Untouched SEC_RESERVE pages are logically zero. Uploads must not
             * commit real shared backing simply to read those zeros. Apart
             * from wasting RAM that would fault back into the region lock. */
            for (uint64_t done=0;done<n;) {
                int committed;
                uint64_t run=bb_upload_query(source+done,&committed);
                if (run>n-done) run=n-done;
                if (!run) win_fail("invalid upload backing extent",(uintptr_t)(source+done),n-done);
                if (!committed) memset(out+(at-address)+done,0,run);
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
    read_unlock();
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
/* Mapped length from address, up to size, across adjacent mappings. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return size;
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

