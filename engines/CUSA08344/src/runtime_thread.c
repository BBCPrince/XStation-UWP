/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`, so GS base = guest TCB while glibc
 * keeps FS. Priorities/affinity are recorded, not enforced by a PS4 scheduler.
 * Windows: GS is the TEB and cannot be moved. The guest TCB lives in a TLS slot of the TEB
 * instead, and the loader rewrites the load to `mov rax, gs:[0x1480+8*slot]` (TlsSlots),
 * the same 9-byte instruction with another displacement. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <errno.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <sys/mman.h>
#include <asm/prctl.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
/* tlou194_uwp_memory.c (Xbox memory backend only): the pager/renderer event ring */
extern void runtime_trace_event(int kind, uint64_t a, uint64_t b, const void *code) __attribute__((weak));
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    /* Guest-visible TCB is allocated separately; this is the ScePthread handle.
     * Spider-Man (2026-11-04): PS4's struct pthread starts with the thread id, and the engine reads it
     * straight after pthread_create returns (0 = failed). The TCB below is only set once the new thread
     * runs, so the id comes first and is set when the record is made. */
    uint64_t tid;
    uint64_t *tcb;
    unsigned char *tls_block;
    pthread_t host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
#ifdef _WIN32
    bb_jmp_buf exit_jump; /* no SEH unwinding through guest frames */
#else
    jmp_buf exit_jump;
#endif
    struct GuestThread *next;
} GuestThread;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static ThreadAttr *attributes;
static GuestThread *threads;
static _Thread_local GuestThread *current;
/* The calling guest thread's name, or NULL on a host thread (fault reports). */
const char *runtime_thread_name(void) { return current ? current->name : NULL; }
static _Thread_local int32_t guest_errno;
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align=16;
static size_t created, joined_count, exited;

void runtime_set_main_tls(const void *data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    tls_template=data; tls_filesz=filesz; tls_memsz=memsz; tls_align=align ? align : 16;
}
static uint64_t tls_offset(void) { return (tls_memsz+tls_align-1)&~(tls_align-1); }
#ifdef _WIN32
/* TEB TlsSlots[64] start at gs:0x1480; slots 64+ live elsewhere and cannot be used. */
static DWORD tls_slot=TLS_OUT_OF_INDEXES;
uint32_t runtime_thread_tcb_offset(void) {
    if (tls_slot==TLS_OUT_OF_INDEXES) {
        tls_slot=TlsAlloc();
        if (tls_slot>=64) { fprintf(stderr,"STOP: no TEB TLS slot below 64 (got %lu)\n",tls_slot); exit(21); }
    }
    return 0x1480+8*(uint32_t)tls_slot;
}
static void set_gs(void *base) {
    runtime_thread_tcb_offset();
    if (!TlsSetValue(tls_slot,base)) { fprintf(stderr,"STOP: TlsSetValue failed (%lu)\n",GetLastError()); exit(21); }
}
#define tls_block_alloc(size) _aligned_malloc(size,64)
#else
static void set_gs(void *base) {
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); exit(21); }
}
#define tls_block_alloc(size) aligned_alloc(64,size)
#endif
/* Build TCB/static TLS for the calling host thread and point GS at it. */
static void attach(GuestThread *t) {
    uint64_t offset=tls_offset();
    size_t total=offset+256;
    unsigned char *block=tls_block_alloc((total+63)&~(size_t)63);
    if (!block) { fputs("Cannot allocate guest TLS\n",stderr); exit(1); }
    memset(block,0,total);
    if (tls_filesz) memcpy(block,tls_template,tls_filesz);
    uint64_t *tcb=(uint64_t *)(block+offset);
    static uint64_t dtv[3];
    tcb[0]=(uint64_t)(uintptr_t)tcb;         /* tcb_self */
    tcb[1]=(uint64_t)(uintptr_t)dtv;         /* tcb_dtv (static module only) */
    tcb[2]=(uint64_t)(uintptr_t)t;           /* tcb_thread */
    t->tls_block=block; t->tcb=tcb;
    set_gs(tcb);
    current=t;
}
static GuestThread *new_thread(void) {
#ifdef _WIN32
    /* Bloodborne packs the owner returned by scePthreadSelf into 40 bits.
     * A UWP process may place its ordinary heap above that range, even when
     * HIGHENTROPYVA is disabled. Keep guest-visible handles representable;
     * records are retained until process exit, just as before. */
    GuestThread *t=runtime_low_map(sizeof(*t),PROT_READ|PROT_WRITE);
    if (t) memset(t,0,sizeof(*t));
#else
    GuestThread *t=calloc(1,sizeof(*t));
#endif
    if (!t) return NULL;
    static uint64_t next_tid=100000;
    t->tid=__atomic_fetch_add(&next_tid,1,__ATOMIC_RELAXED);
    t->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.stack=DEFAULT_STACK,.affinity=0x7f};
    return t;
}
static void publish(GuestThread *t) {
    pthread_mutex_lock(&lock); t->next=threads; threads=t; pthread_mutex_unlock(&lock);
}
/* The calling thread's guest name, or "host" for threads without a record (file timing). */
const char *runtime_thread_current_name(void) { return current ? current->name : "host"; }
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (current) return current;
    GuestThread *t=new_thread();
    if (!t) { fputs("Cannot allocate guest thread\n",stderr); exit(1); }
    t->host=pthread_self(); t->host_owned=1;
    snprintf(t->name,sizeof(t->name),"host");
    attach(t); publish(t);
    return t;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *name) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "host");
}
#ifdef _WIN32
/* BB_THREAD_SAMPLE=<seconds> (God of War port, 2026-11-03): every so often, each guest thread's
 * RIP and the first guest-code addresses on its stack (mostly return addresses): where a boot
 * that shows nothing waits. Nothing that can lock (printf, malloc, our fault handler) runs while a
 * thread is suspended; its stack is read with ReadProcessMemory. */
uintptr_t runtime_guest_image_base(void);
static void describe(char *out,size_t size,uintptr_t address,uintptr_t image) {
    HMODULE module=NULL; char name[MAX_PATH];
    if (address-image<0x20000000) { snprintf(out,size,"g+0x%llx",(unsigned long long)(address-image)); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module &&
        GetModuleFileNameA(module,name,sizeof(name))) {
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%llx",base ? base+1 : name,(unsigned long long)(address-(uintptr_t)module));
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
static void *thread_sampler(void *arg) {
    const DWORD period=(DWORD)(uintptr_t)arg*1000;
    enum { WORDS=1024, HITS=10 };
    static uint64_t stack[WORDS];
    for (;;) {
        Sleep(period);
        const uintptr_t image=runtime_guest_image_base();
        printf("Thread sample:\n");
        pthread_mutex_lock(&lock);
        for (GuestThread *t=threads;t;t=t->next) {
            if (t->finished || t==current) continue;
            HANDLE h=pthread_gethandle(t->host);
            if (!h || SuspendThread(h)==(DWORD)-1) continue;
            CONTEXT c; memset(&c,0,sizeof(c)); c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
            const BOOL got=GetThreadContext(h,&c);
            SIZE_T read=0;
            if (got) ReadProcessMemory(GetCurrentProcess(),(const void *)c.Rsp,stack,sizeof(stack),&read);
            ResumeThread(h);
            if (!got) continue;
            char line[1024], where[160]; int n;
            describe(where,sizeof(where),c.Rip,image);
            n=snprintf(line,sizeof(line),"  '%s' rip %s rdi %llx |",t->name,where,(unsigned long long)c.Rdi);
            for (size_t i=0,hits=0;i<read/8 && hits<HITS && n<(int)sizeof(line)-32;++i)
                if (stack[i]-image<0x20000000) { n+=snprintf(line+n,sizeof(line)-n," %llx",(unsigned long long)(stack[i]-image)); ++hits; }
            printf("%s\n",line);
        }
        pthread_mutex_unlock(&lock);
        fflush(stdout);
    }
    return NULL;
}
#endif
void runtime_thread_attach_main(void) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"main");
#ifdef _WIN32
    const char *sample=getenv("BB_THREAD_SAMPLE");
    if (sample && atoi(sample)>0) {
        pthread_t sampler;
        if (!pthread_create(&sampler,NULL,thread_sampler,(void *)(uintptr_t)atoi(sample))) pthread_detach(sampler);
    }
#endif
}
static GuestThread *find_thread(void *handle) {
    GuestThread *found=NULL;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) if (t==handle) { found=t; break; }
    pthread_mutex_unlock(&lock);
    return found;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    ThreadAttr *found=NULL;
    pthread_mutex_lock(&lock);
    for (ThreadAttr *a=attributes;a;a=a->next) if (a==*slot && a->magic==ATTR_MAGIC) { found=a; break; }
    pthread_mutex_unlock(&lock);
    return found;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ERR(22);
    ThreadAttr *a=calloc(1,sizeof(*a));
    if (!a) return ERR(12);
    *a=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=4096,.affinity=0x7f};
    pthread_mutex_lock(&lock); a->next=attributes; attributes=a; pthread_mutex_unlock(&lock);
    *out=a; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    pthread_mutex_lock(&lock);
    ThreadAttr **link=&attributes;
    while (*link!=a) link=&(*link)->next;
    *link=a->next; a->magic=0;
    pthread_mutex_unlock(&lock);
    free(a); *slot=NULL; return 0;
}
static ABI int32_t attr_get(void *thread,ThreadAttr **out) {
    ThreadAttr *a=find_attr(out);
    if (!a || !thread) return ERR(22);
    GuestThread *t=find_thread(thread);
    if (!t) return ERR(3);
    ThreadAttr *next=a->next;
    *a=t->attr; a->magic=ATTR_MAGIC; a->next=next;
    a->detached=t->detached;
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || size<16384) return ERR(22);
    a->stack=size; return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **slot,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !size) return ERR(22);
    *size=a->stack; return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **slot,int state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (state!=0 && state!=1)) return ERR(22);
    a->detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **slot,int *state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !state) return ERR(22);
    *state=a->detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **slot,int policy) {
    ThreadAttr *a=find_attr(slot);
    if (!a || policy<1 || policy>3) return ERR(22);
    a->policy=policy; return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **slot,int inherit) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (inherit!=0 && inherit!=4)) return ERR(22);
    a->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **slot,const int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    a->prio=*param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **slot,int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    *param=a->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot,uint64_t mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **slot,uint64_t *mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !mask) return ERR(22);
    *mask=a->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->guard=size; return 0;
}

/* Linux thread names hold 15 characters; longer ones would be rejected. */
static void set_host_name(const char *name) {
    char host[16]={0};
    memcpy(host,name,strnlen(name,sizeof(host)-1));
    pthread_setname_np(pthread_self(),host);
}
#ifdef _WIN32
#define exit_setjmp(t) bb_setjmp((t)->exit_jump)
#define exit_longjmp(t) bb_longjmp((t)->exit_jump,1)
#else
#define exit_setjmp(t) setjmp((t)->exit_jump)
#define exit_longjmp(t) longjmp((t)->exit_jump,1)
#endif
/* Guest priorities run 256 (highest) to 767; the game's are 700 except FMOD's (256-265).
 * Host threads all ran at normal priority, so on 6-core CPUs the draw workers and game
 * threads delayed FMOD's mixer and output threads: underruns heard as crackling.
 * BB_THREAD_PRIORITY=0 keeps every thread at normal priority. */
static void set_host_priority(int prio) {
#ifdef _WIN32
    static int enabled=-1;
    if (enabled<0) { const char *e=getenv("BB_THREAD_PRIORITY"); enabled=!e || e[0]!='0'; }
    if (enabled && prio<400) SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
#else
    (void)prio;
#endif
}

/* TLOU's six named job workers only. Guest priorities remain recorded as
 * before; do not apply the worker policy to the renderer, main or audio. */
static int tlou_worker_fixes;
void runtime_tlou_worker_fixes(int mode) { tlou_worker_fixes=mode; }
static int tlou_job_index(const char *name) {
    static const char prefix[]="NdJobWorkerThread";
    if (strncmp(name,prefix,sizeof(prefix)-1) ||
        (name[17]!=' ' && name[17]!='_') || name[18]<'0' || name[18]>'5' || name[19]) return -1;
    return name[18]-'0';
}
static void tlou_set_worker_priority(GuestThread *t) {
#ifdef _WIN32
    const int index=tlou_job_index(t->name);
    if (index<0) return;
    const int before=GetThreadPriority(GetCurrentThread());
    if (tlou_worker_fixes &&
        (!SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL) ||
         GetThreadPriority(GetCurrentThread())!=THREAD_PRIORITY_BELOW_NORMAL)) {
        fprintf(stderr,"STOP: TLOU worker priority fix failed (error %lu)\n",GetLastError());
        exit(21);
    }
    printf("WORKERFIX194 worker: mode=%d index=%d tid=%lu before=%d os_priority=%d guest_priority=%d\n",
           tlou_worker_fixes,index,(unsigned long)GetCurrentThreadId(),before,
           GetThreadPriority(GetCurrentThread()),t->attr.prio);
#else
    (void)t;
#endif
}

static void *host_start(void *p) {
    GuestThread *t=p;
    attach(t);
    set_host_name(t->name);
    extern void register_gpu_thread194(const char*);
    if(strcmp(t->name,"DCLoader"))register_gpu_thread194(t->name);
    set_host_priority(t->attr.prio);
    tlou_set_worker_priority(t);
#ifdef _WIN32
    /* Detroit (2026-11-11), BB_DT_IDEAL=1: 8 hardware threads (4 cores, SMT); runs where the busy
     * threads shared a core ran ~20% slower throughout. Preferred processors keep them apart:
     * GPU command thread 0, GameMain 2, RenderQueue 4 (a hint, not a pin). */
    {
        const char *ideal=getenv("BB_DT_IDEAL");
        /* BB_DT_AFFINITY=1: hard placement. Heavy threads alone on even CPUs (one per core: GPU
         * command 0, Vulkan recording 2, GameMain 4, RenderQueue 6), all other guest threads on
         * the odd CPUs (the SMT siblings). Slow runs had the recording thread ~30% slower. */
        const char *aff=getenv("BB_DT_AFFINITY");
        if (aff && aff[0]=='1') {
            const DWORD_PTR mask=!strcmp(t->name,"RenderQueue") ? (DWORD_PTR)1<<6 : (DWORD_PTR)0xAA;
            const DWORD_PTR was=SetThreadAffinityMask(GetCurrentThread(),mask);
            static volatile long said;
            if (!was || !strcmp(t->name,"RenderQueue") || __atomic_add_fetch(&said,1,__ATOMIC_RELAXED)<=2)
                printf("Detroit: thread '%s' on CPUs %#llx (was %#llx)\n",t->name,(unsigned long long)mask,(unsigned long long)was);
        }
        if (ideal && ideal[0]=='1' && !strcmp(t->name,"RenderQueue")) {
            const DWORD was=SetThreadIdealProcessor(GetCurrentThread(),4);
            printf("Detroit: RenderQueue prefers processor 4 (was %ld)\n",(long)was);
        }
    }
#endif
    if (!exit_setjmp(t)) t->result=t->entry(t->argument);
    runtime_thread_keys_cleanup();
    pthread_mutex_lock(&lock); t->finished=1; ++exited; pthread_mutex_unlock(&lock);
    return t->result;
}
static int32_t create(GuestThread **out,ThreadAttr **attr_slot,GuestEntry entry,void *argument,const char *name) {
    if (!out || !entry) return ERR(22);
    ThreadAttr *a=NULL;
    if (attr_slot && !(a=find_attr(attr_slot))) return ERR(22);
    GuestThread *t=new_thread();
    if (!t) return ERR(12);
    if (a) { t->attr=*a; t->attr.next=NULL; }
    t->entry=entry; t->argument=argument; t->detached=t->attr.detached;
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "guest");
    pthread_attr_t host;
    pthread_attr_init(&host);
    uint64_t stack=t->attr.stack<MIN_STACK ? MIN_STACK : t->attr.stack;
    /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
    size_t stack_bytes=(size_t)stack+STACK_MARGIN;
#ifdef _WIN32
    /* Windows places the stack itself (low: no high-entropy ASLR) and commits all of it:
     * guest code has no stack probes and would step over the guard page. */
    pthread_attr_setstacksize(&host,stack_bytes);
#else
    void *stack_memory=runtime_low_map(stack_bytes,PROT_READ|PROT_WRITE);
    if (stack_memory) pthread_attr_setstack(&host,stack_memory,stack_bytes);
    else pthread_attr_setstacksize(&host,stack_bytes);
#endif
    publish(t);
    /* Publish the handle before the thread can run and inspect itself. */
    *out=t;
    int e=pthread_create(&t->host,&host,host_start,t);
    pthread_attr_destroy(&host);
    if (e) { fprintf(stderr,"STOP: host pthread_create failed: %d\n",e); exit(21); }
    if (t->detached) pthread_detach(t->host);
    pthread_mutex_lock(&lock); ++created; pthread_mutex_unlock(&lock);
    printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n",t->name,(unsigned long long)stack,t->attr.prio);
    if (runtime_trace_event) { /* the pager/renderer event ring: checkpoints show as SaveWorker */
        uint64_t name[2]={0,0}; strncpy((char*)name,t->name,sizeof(name));
        runtime_trace_event(11,name[0],name[1],NULL);
    }
    return 0;
}
static ABI int32_t thread_create(GuestThread **out,ThreadAttr **attr,GuestEntry entry,void *argument,const char *name) {
    return create(out,attr,entry,argument,name);
}
static ABI int32_t thread_join(GuestThread *t,void **result) {
    if (!find_thread(t) || t->host_owned) return ERR(3);
    if (t==current) return ERR(11);
    if (t->detached || t->joined) return ERR(22);
    t->joined=1;
    void *value=NULL;
    int e=pthread_join(t->host,&value);
    if (e) { fprintf(stderr,"STOP: host pthread_join failed: %d\n",e); exit(21); }
    if (result) *result=t->result;
    pthread_mutex_lock(&lock); ++joined_count; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (t->detached) return ERR(22);
    t->detached=1;
    if (!t->host_owned) pthread_detach(t->host);
    return 0;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t=runtime_thread_current();
    if (t->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); exit(21); }
    t->result=value;
    /* No host unwinder: guest frames have no registered FDEs. */
    exit_longjmp(t);
}
/* pthread_exit for the network libraries' threads (gpu/bbnet/bbnet_glue.cpp). */
__attribute__((noreturn)) void runtime_thread_exit(void *value) { thread_exit(value); }
static ABI int32_t thread_yield(void) { sched_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    if (!find_thread(t)) return ERR(3);
    if (!prio) return ERR(22);
    *prio=t->attr.prio; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    if (!find_thread(t)) return ERR(3);
    t->attr.prio=prio; return 0;
}
static ABI int32_t thread_set_affinity(GuestThread *t,uint64_t mask) {
    if (!find_thread(t)) return ERR(3);
    t->attr.affinity=mask; return 0;
}
static ABI int32_t thread_get_affinity(GuestThread *t,uint64_t *mask) {
    if (!find_thread(t)) return ERR(3);
    if (!mask) return ERR(22);
    *mask=t->attr.affinity; return 0;
}
static ABI int32_t thread_rename(GuestThread *t,const char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(t->name,sizeof(t->name),"%s",name);
    if (t==current) set_host_name(t->name);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }
/* God of War (2026-11-03): a thread's name and scheduling parameters (recorded as prio is), and
 * cancellation, which no guest thread here can honour: said, and refused. */
static ABI int32_t thread_get_name(GuestThread *t,char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(name,32,"%s",t->name);
    return 0;
}
static ABI int32_t thread_get_schedparam(GuestThread *t,int *policy,int *param) {
    if (!find_thread(t)) return ERR(3);
    if (!policy || !param) return ERR(22);
    *policy=t->attr.policy; *param=t->attr.prio;
    return 0;
}
/* Detroit (2026-11-08): POSIX pthread_getschedparam returns the errno value itself */
static ABI int32_t posix_get_schedparam(GuestThread *t,int *policy,int *param) {
    int32_t r=thread_get_schedparam(t,policy,param); return r ? r&0xffff : 0;
}
static ABI int32_t thread_set_schedparam(GuestThread *t,int policy,const int *param) {
    if (!find_thread(t)) return ERR(3);
    if (!param) return ERR(22);
    t->attr.policy=policy;
    return thread_set_prio(t,*param);
}
static ABI int32_t thread_cancel(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    printf("Runtime: scePthreadCancel('%s') not supported; the thread keeps running\n",t->name);
    return ERR(45); /* EOPNOTSUPP */
}

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static ABI int32_t posix_set_schedparam(GuestThread *t,int policy,const int *param) { return posix(thread_set_schedparam(t,policy,param)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **a,int s) { return posix(attr_set_detach(a,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **a,uint64_t s) { return posix(attr_set_stack(a,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **a,const int *p) { return posix(attr_set_param(a,p)); }
static ABI int32_t posix_create(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg) { return posix(create(t,a,e,arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg,const char *name) { return posix(create(t,a,e,arg,name)); }
static ABI int32_t posix_join(GuestThread *t,void **r) { return posix(thread_join(t,r)); }

/* Co-op: guest threads started by host libraries (NP matching/signaling threads call the
 * game's callbacks). Both return 0 or a positive errno value. */
int32_t runtime_thread_spawn(void **out,void *(ABI *entry)(void *),void *argument,uint64_t stack,const char *name) {
    ThreadAttr *attr=NULL;
    int32_t r=attr_init(&attr);
    if (!r && stack) r=attr_set_stack(&attr,stack);
    if (!r) r=create((GuestThread **)out,&attr,entry,argument,name);
    if (attr) attr_destroy(&attr);
    return posix(r);
}
int32_t runtime_thread_join_spawned(void *thread,void **result) {
    return posix(thread_join(thread,result));
}

uintptr_t runtime_thread_resolve(const char *name) {
    static const RuntimeExport exports[]={
        {"scePthreadSelf",thread_self},
        {"pthread_self",thread_self},
        {"__error",guest_error},
        {"scePthreadAttrInit",attr_init},
        {"scePthreadAttrDestroy",attr_destroy},
        {"scePthreadAttrGet",attr_get},
        {"scePthreadAttrGetaffinity",attr_affinity},
        {"scePthreadAttrSetstacksize",attr_set_stack},
        {"scePthreadAttrSetdetachstate",attr_set_detach},
        {"scePthreadAttrSetschedpolicy",attr_set_policy},
        {"scePthreadAttrSetschedparam",attr_set_param},
        {"scePthreadAttrSetaffinity",attr_set_affinity},
        {"scePthreadAttrSetinheritsched",attr_set_inherit},
        {"scePthreadAttrGetdetachstate",attr_get_detach},
        {"scePthreadAttrGetstacksize",attr_get_stack},
        {"scePthreadAttrGetschedparam",attr_get_param},
        {"scePthreadAttrSetguardsize",attr_set_guard},
        {"scePthreadCreate",thread_create},
        {"scePthreadJoin",thread_join},
        {"scePthreadDetach",thread_detach},
        {"scePthreadExit",thread_exit},
        {"scePthreadYield",thread_yield},
        {"scePthreadGetprio",thread_get_prio},
        {"scePthreadSetprio",thread_set_prio},
        {"scePthreadSetaffinity",thread_set_affinity},
        {"scePthreadGetaffinity",thread_get_affinity},
        {"scePthreadRename",thread_rename},
        {"scePthreadEqual",thread_equal},
        {"scePthreadGetname",thread_get_name},
        {"scePthreadGetschedparam",thread_get_schedparam}, {"pthread_getschedparam",posix_get_schedparam},
        {"scePthreadSetschedparam",thread_set_schedparam},
        {"scePthreadCancel",thread_cancel},
        {"pthread_setschedparam",posix_set_schedparam},
        {"pthread_attr_init",posix_attr_init},
        {"pthread_attr_destroy",posix_attr_destroy},
        {"pthread_attr_setdetachstate",posix_attr_set_detach},
        {"pthread_attr_setstacksize",posix_attr_set_stack},
        {"pthread_attr_setschedparam",posix_attr_set_param},
        {"pthread_create",posix_create},
        {"pthread_create_name_np",posix_create_name},
        {"pthread_join",posix_join},
        {"pthread_exit",thread_exit},
    };
    return RUNTIME_LOOKUP(exports,name);
}
void runtime_thread_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu\n",created,exited,joined_count);
    pthread_mutex_unlock(&lock);
}

