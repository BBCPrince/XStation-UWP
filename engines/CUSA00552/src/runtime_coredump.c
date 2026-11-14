#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define COREDUMP_PARAM ((int32_t)UINT32_C(0x81180000))
#define COREDUMP_NOT_REGISTERED ((int32_t)UINT32_C(0x81180001))
#define COREDUMP_ALREADY_REGISTERED ((int32_t)UINT32_C(0x81180002))
#define COREDUMP_THREAD_CREATE ((int32_t)UINT32_C(0x81180004))

typedef int32_t (ABI *CoredumpHandler)(void *common);
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static CoredumpHandler callback;
static void *callback_common,*worker;
static int stopping,requested,completed;
static _Thread_local int in_callback;

static ABI void *coredump_worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (!stopping && !requested) pthread_cond_wait(&wake,&lock);
        if (stopping) break;
        CoredumpHandler handler=callback;
        void *common=callback_common;
        requested=0;
        pthread_mutex_unlock(&lock);
        in_callback=1;
        int32_t result=handler(common);
        in_callback=0;
        fprintf(stderr,"Coredump: guest callback returned 0x%08x\n",(unsigned)result);
        pthread_mutex_lock(&lock);
        completed=1;
        pthread_cond_broadcast(&wake);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

static ABI int32_t register_handler(CoredumpHandler handler,uint64_t stack_size,void *common) {
    if (!handler || stack_size>=UINT64_C(0x20000000)) return COREDUMP_PARAM;
    pthread_mutex_lock(&lock);
    if (callback) { pthread_mutex_unlock(&lock); return COREDUMP_ALREADY_REGISTERED; }
    callback=handler;
    callback_common=common;
    stopping=requested=completed=0;
    int32_t error=runtime_thread_spawn(&worker,coredump_worker,NULL,stack_size,"coredump");
    if (error) {
        callback=NULL;
        callback_common=worker=NULL;
        pthread_mutex_unlock(&lock);
        return COREDUMP_THREAD_CREATE;
    }
    pthread_mutex_unlock(&lock);
    puts("Runtime: coredump handler registered on a dedicated guest thread");
    return 0;
}

static ABI int32_t unregister_handler(void) {
    pthread_mutex_lock(&lock);
    if (!callback) { pthread_mutex_unlock(&lock); return COREDUMP_NOT_REGISTERED; }
    if (in_callback) { pthread_mutex_unlock(&lock); return COREDUMP_PARAM; }
    stopping=1;
    pthread_cond_broadcast(&wake);
    void *join=worker;
    pthread_mutex_unlock(&lock);
    int32_t error=runtime_thread_join_spawned(join,NULL);
    pthread_mutex_lock(&lock);
    if (!error) { callback=NULL; callback_common=worker=NULL; }
    pthread_mutex_unlock(&lock);
    return error ? COREDUMP_THREAD_CREATE : 0;
}

void runtime_coredump_notify(void) {
    if (in_callback || pthread_mutex_trylock(&lock)) return;
    if (!callback || stopping || requested || completed) { pthread_mutex_unlock(&lock); return; }
    requested=1;
    pthread_cond_broadcast(&wake);
    struct timespec deadline;
    if (!clock_gettime(CLOCK_REALTIME,&deadline)) {
        deadline.tv_sec+=5;
        while (!completed && !stopping) {
            int error=pthread_cond_timedwait(&wake,&lock,&deadline);
            if (error) {
                fprintf(stderr,"Coredump: callback wait stopped (%d)\n",error);
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);
}

uintptr_t runtime_coredump_resolve(const char *name) {
    static const RuntimeExport exports[]={
        {"sceCoredumpRegisterCoredumpHandler",register_handler},
        {"sceCoredumpUnregisterCoredumpHandler",unregister_handler},
    };
    return RUNTIME_LOOKUP(exports,name);
}
