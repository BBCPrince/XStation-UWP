/* God of War (CUSA07408, 2018) imports the Bloodborne/TLOU runtime did not serve (2026-11-03):
 * libkernel bookkeeping (thread destructors and atexit hooks of the SDK 5 libc, sanitizer and
 * hardware queries, time conversion), event flags, POSIX semaphores, offline sockets, and the
 * services the game asks once at start (share menu, coredump, platform privacy). Calls that only
 * report something are said once in the log, so a later failure can be traced to them. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ONCE(...) do { static int said_; if (!__atomic_exchange_n(&said_,1,__ATOMIC_RELAXED)) { \
    printf("GoW runtime: " __VA_ARGS__); putchar('\n'); fflush(stdout); } } while (0)

/* ---- libc's thread exit hooks (SDK 5): recorded; guest threads here never run them ---- */
static uintptr_t thread_dtors, atexit_count, atexit_report;
static ABI void set_thread_dtors(uintptr_t f) { thread_dtors=f; ONCE("libc thread destructors at %p",(void *)f); }
static ABI void set_atexit_count(uintptr_t f) { atexit_count=f; }
static ABI void set_atexit_report(uintptr_t f) { atexit_report=f; }
static ABI void rtld_atexit_increment(uint64_t *count) { if (count) __atomic_add_fetch(count,1,__ATOMIC_RELAXED); }
static ABI void rtld_atexit_decrement(uint64_t *count) { if (count) __atomic_sub_fetch(count,1,__ATOMIC_RELAXED); }

/* ---- hardware and process queries: a base PS4, no sanitizer ---- */
static ABI int32_t zero(void) { return 0; }
static ABI int32_t is_neo_mode(void) { ONCE("sceKernelIsNeoMode: base PS4 (0)"); return 0; }
static ABI uint64_t get_gpi(void) { return 0; }
static ABI int32_t get_page_table_stats(int32_t *cpu_total, int32_t *cpu_available, int32_t *gpu_total, int32_t *gpu_available) {
    if (cpu_total) *cpu_total=0x8000; if (cpu_available) *cpu_available=0x4000;
    if (gpu_total) *gpu_total=0x8000; if (gpu_available) *gpu_available=0x4000;
    return 0;
}
static ABI int32_t is_stack(void *address, void **start, void **end) {
    (void)address; if (start) *start=NULL; if (end) *end=NULL;
    return ERR(3);
}
static ABI int32_t set_prt_aperture(int32_t id, uint64_t address, uint64_t size) {
    printf("GoW runtime: sceKernelSetPrtAperture(%d, 0x%llx, 0x%llx) recorded (partially resident textures unused)\n",
           id,(unsigned long long)address,(unsigned long long)size);
    return id>=0 && id<3 ? 0 : ERR(22);
}
static ABI int32_t uuid_create(uint8_t *uuid) {
    if (!uuid) return ERR(22);
    static uint64_t state;
    if (!state) state=(uint64_t)time(NULL)*0x9E3779B97F4A7C15ull^(uint64_t)(uintptr_t)uuid;
    for (int i=0;i<16;++i) { state^=state<<13; state^=state>>7; state^=state<<17; uuid[i]=(uint8_t)state; }
    uuid[6]=(uint8_t)((uuid[6]&0x0f)|0x40); uuid[8]=(uint8_t)((uuid[8]&0x3f)|0x80);
    return 0;
}
/* Time zone: UTC (no offsets), as the console's clock is read here. */
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } Timesec;
static ABI int32_t utc_to_local(int64_t utc, int64_t *local, Timesec *st, uint64_t *dst) {
    if (!local) return ERR(22);
    *local=utc;
    if (st) { st->t=utc; st->west_sec=0; st->dst_sec=0; }
    if (dst) *dst=0;
    return 0;
}
typedef struct { int32_t minuteswest, dsttime; } Timezone;
static ABI int32_t local_to_utc(int64_t local, int64_t unused, int64_t *utc, Timezone *tz, int32_t *dst) {
    (void)unused;
    if (!utc) return ERR(22);
    *utc=local;
    if (tz) { tz->minuteswest=0; tz->dsttime=0; }
    if (dst) *dst=0;
    return 0;
}
/* Modules: libc.prx and libSceFios2.prx are linked into the image already; others are absent. */
static ABI int32_t load_start_module(const char *path, uint64_t argc, const void *argv, uint32_t flags, const void *option, int32_t *result) {
    (void)argc; (void)argv; (void)flags; (void)option;
    const char *name=path ? strrchr(path,'/') : NULL;
    name=name ? name+1 : path;
    printf("GoW runtime: sceKernelLoadStartModule(%s)\n",path ? path : "(null)");
    if (result) *result=0;
    if (name && !strcmp(name,"libc.prx")) return 0x10;
    if (name && !strcmp(name,"libSceFios2.prx")) return 0x11;
    return ERR(2); /* ENOENT */
}
static ABI int32_t kernel_dlsym(int32_t handle, const char *symbol, void **address) {
    printf("GoW runtime: sceKernelDlsym(%d, %s): not found\n",handle,symbol ? symbol : "(null)");
    if (address) *address=NULL;
    return ERR(2);
}
static ABI int32_t no_module_info(uint64_t address, int32_t flags, void *info) {
    (void)address; (void)flags; (void)info;
    return ERR(3); /* ESRCH: the runtime keeps no module records */
}
static ABI int32_t segment_info(void *info) { (void)info; return ERR(3); }
/* libc's unwinder (after GoW3's port): no frame data; resuming a context is a stop. */
static ABI int32_t elf_phdr_match_addr(void *info, uint64_t address) { (void)info; (void)address; return 0; }
static ABI int32_t is_signal_return(uint64_t pc) { (void)pc; return 0; }
static ABI void pthread_cxa_finalize(void *dso) { (void)dso; }
static ABI __attribute__((noreturn)) void sigreturn_stop(void) {
    fputs("STOP: guest C++ exception unwinding (sigreturn) is not supported\n",stderr);
    runtime_report();
    exit(23);
}
static ABI int32_t libc_heap_trace_info(uint64_t *info) {
    /* {size, flags, mspace atomic id mask*, mstate table*}: libc's own heap needs no tracing. */
    static uint64_t mask, table[64];
    if (!info) return ERR(22);
    info[2]=(uint64_t)(uintptr_t)&mask; info[3]=(uint64_t)(uintptr_t)table;
    return 0;
}

/* ---- event flags (libkernel): 64 bits, AND/OR waits, clear on wake ---- */
#define EF_WAITMODE_AND 1
#define EF_WAITMODE_OR 2
#define EF_CLEAR_ALL 0x10
#define EF_CLEAR_PAT 0x20
typedef struct EventFlag {
    pthread_mutex_t lock; pthread_cond_t changed;
    uint64_t bits; uint32_t attr, waiters, generation; int cancelled;
    char name[32];
} EventFlag;
static ABI int32_t ef_create(EventFlag **out, const char *name, uint32_t attr, uint64_t init, const void *param) {
    (void)param;
    if (!out) return ERR(22);
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    pthread_mutex_init(&f->lock,NULL); pthread_cond_init(&f->changed,NULL);
    f->bits=init; f->attr=attr;
    snprintf(f->name,sizeof(f->name),"%s",name ? name : "");
    *out=f;
    return 0;
}
static ABI int32_t ef_delete(EventFlag *f) {
    if (!f) return ERR(3);
    pthread_mutex_lock(&f->lock);
    f->cancelled=1; ++f->generation;
    pthread_cond_broadcast(&f->changed);
    const int idle=!f->waiters;
    pthread_mutex_unlock(&f->lock);
    if (idle) { pthread_cond_destroy(&f->changed); pthread_mutex_destroy(&f->lock); free(f); }
    return 0; /* a flag deleted under waiters is left allocated (they wake with ECANCELED) */
}
static ABI int32_t ef_set(EventFlag *f, uint64_t bits) {
    if (!f) return ERR(3);
    pthread_mutex_lock(&f->lock);
    f->bits|=bits; ++f->generation;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static ABI int32_t ef_clear(EventFlag *f, uint64_t bits) {
    if (!f) return ERR(3);
    pthread_mutex_lock(&f->lock); f->bits&=bits; pthread_mutex_unlock(&f->lock);
    return 0;
}
static int ef_met(const EventFlag *f, uint64_t pattern, uint32_t mode) {
    return (mode & EF_WAITMODE_AND) ? (f->bits & pattern)==pattern : (f->bits & pattern)!=0;
}
static void ef_take(EventFlag *f, uint64_t pattern, uint32_t mode, uint64_t *result) {
    if (result) *result=f->bits;
    if (mode & EF_CLEAR_ALL) f->bits=0;
    else if (mode & EF_CLEAR_PAT) f->bits&=~pattern;
}
static ABI int32_t ef_wait(EventFlag *f, uint64_t pattern, uint32_t mode, uint64_t *result, uint32_t *timeout_us) {
    if (!f) return ERR(3);
    if (!pattern || !(mode & (EF_WAITMODE_AND|EF_WAITMODE_OR))) return ERR(22);
    struct timespec deadline={0,0};
    if (timeout_us) {
        clock_gettime(CLOCK_REALTIME,&deadline);
        const uint64_t ns=(uint64_t)deadline.tv_nsec+(uint64_t)*timeout_us*1000;
        deadline.tv_sec+=(time_t)(ns/1000000000); deadline.tv_nsec=(long)(ns%1000000000);
    }
    pthread_mutex_lock(&f->lock);
    ++f->waiters;
    int32_t r=0;
    while (!ef_met(f,pattern,mode) && !f->cancelled) {
        if (timeout_us) {
            if (pthread_cond_timedwait(&f->changed,&f->lock,&deadline)==ETIMEDOUT && !ef_met(f,pattern,mode)) { r=ERR(60); break; }
        } else pthread_cond_wait(&f->changed,&f->lock);
    }
    if (!r && f->cancelled) r=ERR(85); /* ECANCELED */
    if (!r) ef_take(f,pattern,mode,result);
    else if (result) *result=f->bits;
    if (timeout_us && r==ERR(60)) *timeout_us=0;
    --f->waiters;
    pthread_mutex_unlock(&f->lock);
    return r;
}
static ABI int32_t ef_poll(EventFlag *f, uint64_t pattern, uint32_t mode, uint64_t *result) {
    if (!f) return ERR(3);
    if (!pattern || !(mode & (EF_WAITMODE_AND|EF_WAITMODE_OR))) return ERR(22);
    pthread_mutex_lock(&f->lock);
    const int met=ef_met(f,pattern,mode);
    if (met) ef_take(f,pattern,mode,result); else if (result) *result=f->bits;
    pthread_mutex_unlock(&f->lock);
    return met ? 0 : ERR(16); /* EBUSY */
}
static ABI int32_t ef_cancel(EventFlag *f, uint64_t bits, int32_t *waiters) {
    if (!f) return ERR(3);
    pthread_mutex_lock(&f->lock);
    if (waiters) *waiters=(int32_t)f->waiters;
    f->bits=bits; ++f->generation;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return 0;
}

/* ---- POSIX semaphores (libScePosix): the guest sem_t holds a pointer ---- */
typedef struct { pthread_mutex_t lock; pthread_cond_t posted; uint32_t value; } Semaphore;
static int sem_fail(int e) { *runtime_errno()=e; return -1; }
static ABI int32_t sem_init_guest(Semaphore **s, int32_t shared, uint32_t value) {
    (void)shared;
    if (!s) return sem_fail(22);
    Semaphore *m=calloc(1,sizeof(*m));
    if (!m) return sem_fail(12);
    pthread_mutex_init(&m->lock,NULL); pthread_cond_init(&m->posted,NULL); m->value=value;
    *s=m;
    return 0;
}
static ABI int32_t sem_destroy_guest(Semaphore **s) {
    if (!s || !*s) return sem_fail(22);
    pthread_cond_destroy(&(*s)->posted); pthread_mutex_destroy(&(*s)->lock); free(*s); *s=NULL;
    return 0;
}
static ABI int32_t sem_post_guest(Semaphore **s) {
    if (!s || !*s) return sem_fail(22);
    pthread_mutex_lock(&(*s)->lock); ++(*s)->value; pthread_cond_signal(&(*s)->posted); pthread_mutex_unlock(&(*s)->lock);
    return 0;
}
typedef struct { int64_t sec, nsec; } GuestTimespec;
static int32_t sem_take(Semaphore **s, const GuestTimespec *until) {
    if (!s || !*s) return sem_fail(22);
    Semaphore *m=*s;
    struct timespec deadline={0,0};
    if (until) { deadline.tv_sec=(time_t)until->sec; deadline.tv_nsec=(long)until->nsec; }
    pthread_mutex_lock(&m->lock);
    while (!m->value) {
        if (!until) pthread_cond_wait(&m->posted,&m->lock);
        else if (pthread_cond_timedwait(&m->posted,&m->lock,&deadline)==ETIMEDOUT && !m->value) {
            pthread_mutex_unlock(&m->lock);
            return sem_fail(60); /* ETIMEDOUT */
        }
    }
    --m->value;
    pthread_mutex_unlock(&m->lock);
    return 0;
}
static ABI int32_t sem_wait_guest(Semaphore **s) { return sem_take(s,NULL); }
static ABI int32_t sem_timedwait_guest(Semaphore **s, const GuestTimespec *until) {
    if (!until) return sem_fail(22);
    return sem_take(s,until);
}

/* ---- sockets: the game is offline here; every connection fails as a down network does ---- */
static ABI int32_t net_down(void) { ONCE("POSIX socket call: network is down (offline)"); return sem_fail(50); /* ENETDOWN */ }

/* ---- services asked once at start ---- */
static ABI int32_t service_ok(void) { return 0; }
static ABI int32_t platform_privacy(int32_t type, int32_t *value) { (void)type; if (value) *value=0; return 0; }
static ABI int32_t share_open_menu(void) { ONCE("share menu: not available"); return 0; }
static ABI int32_t sysmodule_load_internal_arg(uint32_t id, uint64_t argc, const void *argv, uint32_t flags, int32_t *result) {
    (void)argc; (void)argv; (void)flags;
    printf("GoW runtime: sceSysmoduleLoadModuleInternalWithArg(0x%x)\n",id);
    if (result) *result=0;
    return 0;
}
static ABI int32_t discmap_no_bitmap(void) { return (int32_t)0x81100004; } /* NO_BITMAP_INFO: all on the HDD */
static ABI int32_t safe_area(float *info) { if (!info) return (int32_t)0x80A10003; *info=1.0f; return 0; } /* full screen (GoW3 port) */
static ABI int32_t playgo_dialog_open(const void *param) { (void)param; ONCE("PlayGo dialog: everything is installed; not shown"); return 0; }

static const RuntimeExport exports[]={
    {"_sceKernelSetThreadDtors",set_thread_dtors}, {"_sceKernelSetThreadAtexitCount",set_atexit_count},
    {"_sceKernelSetThreadAtexitReport",set_atexit_report},
    {"_sceKernelRtldThreadAtexitIncrement",rtld_atexit_increment}, {"_sceKernelRtldThreadAtexitDecrement",rtld_atexit_decrement},
    {"sceKernelIsAddressSanitizerEnabled",zero}, {"sceKernelGetSanitizerMallocReplaceExternal",zero},
    {"sceKernelGetSanitizerNewReplaceExternal",zero}, {"sceKernelIsNeoMode",is_neo_mode},
    {"sceKernelIsProspero",zero}, {"sceKernelGetGPI",get_gpi}, {"sceKernelGetPageTableStats",get_page_table_stats},
    {"sceKernelIsStack",is_stack}, {"sceKernelSetPrtAperture",set_prt_aperture}, {"sceKernelUuidCreate",uuid_create},
    {"sceKernelConvertUtcToLocaltime",utc_to_local}, {"sceKernelConvertLocaltimeToUtc",local_to_utc},
    {"sceKernelLoadStartModule",load_start_module}, {"sceKernelDlsym",kernel_dlsym},
    {"sceKernelGetModuleInfoFromAddr",no_module_info}, {"sceKernelGetModuleInfoForUnwind",no_module_info},
    {"sceKernelInternalMemoryGetModuleSegmentInfo",segment_info}, {"sceKernelMlock",zero},
    {"__elf_phdr_match_addr",elf_phdr_match_addr}, {"_is_signal_return",is_signal_return},
    {"__pthread_cxa_finalize",pthread_cxa_finalize}, {"sigreturn",sigreturn_stop},
    {"sceLibcHeapGetTraceInfo",libc_heap_trace_info},
    {"sceKernelCreateEventFlag",ef_create}, {"sceKernelDeleteEventFlag",ef_delete},
    {"sceKernelSetEventFlag",ef_set}, {"sceKernelClearEventFlag",ef_clear}, {"sceKernelWaitEventFlag",ef_wait},
    {"sceKernelPollEventFlag",ef_poll}, {"sceKernelCancelEventFlag",ef_cancel},
    {"sem_init",sem_init_guest}, {"sem_destroy",sem_destroy_guest}, {"sem_post",sem_post_guest},
    {"sem_wait",sem_wait_guest}, {"sem_timedwait",sem_timedwait_guest},
    {"connect",net_down}, {"send",net_down}, {"recv",net_down}, {"select",net_down}, {"shutdown",net_down},
    {"sceSystemServiceGetPlatformPrivacySetting",platform_privacy},
    {"sceShareUtilityInitializeEx2",service_ok}, {"sceShareUtilityAdvanceInitialize",service_ok},
    {"sceShareUtilityAdvanceSetUploadContentData",service_ok}, {"sceShareUtilityOpenShareMenuDefault",share_open_menu},
    {"sceCoredumpAttachMemoryRegion",service_ok}, {"sceCoredumpSetUserDataType",service_ok},
    {"sceCoredumpWriteUserData",service_ok}, {"sceSysmoduleLoadModuleInternalWithArg",sysmodule_load_internal_arg},
    {"sceSystemServiceGetDisplaySafeAreaInfo",safe_area}, {"scePlayGoClose",service_ok}, {"scePlayGoTerminate",service_ok},
    {"scePlayGoDialogOpen",playgo_dialog_open}, {"sceNpTrophyDestroyHandle",service_ok}, {"sceNpTrophyAbortHandle",service_ok},
};
uintptr_t runtime_gow_resolve(const char *name) {
    /* libSceDiscMap's unnamed export (fJgP+wqifno), imported by Fios2 */
    if (!strncmp(name,"fJgP+wqifno#",12)) return (uintptr_t)discmap_no_bitmap;
    return RUNTIME_LOOKUP(exports,name);
}
