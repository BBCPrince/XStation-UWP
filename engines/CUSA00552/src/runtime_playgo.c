#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PG_ERROR(n) ((int32_t)(UINT32_C(0x80b20000)+(n)))
#define PG_POINTER PG_ERROR(10)
#define PG_SIZE PG_ERROR(11)
#define PG_CHUNK PG_ERROR(12)
typedef struct { const void *buffer; uint32_t size,reserved; } InitParams;
typedef struct { uint64_t progress,total; } Progress;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    int initialized,opened,speed;
    uint32_t chunks;
    uint64_t sizes[1000],language,speed_tick;
} state;

static uint16_t u16(const unsigned char *p) { uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t u32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t u64(const unsigned char *p) { uint64_t v; memcpy(&v,p,8); return v; }
static int span(uint32_t offset,uint32_t size,uint32_t total) { return offset<=total && size<=total-offset; }
static uint64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC,&t) ? 0 : (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static int32_t handle_valid(uint32_t handle) {
    if (!state.initialized) return PG_ERROR(5);
    return handle==1 && state.opened ? 0 : PG_ERROR(9);
}
static int32_t ids_valid(uint32_t handle,const uint16_t *ids,uint32_t count) {
    int32_t error=handle_valid(handle);
    if (error) return error;
    if (!ids) return PG_POINTER;
    if (!count) return PG_SIZE;
    for (uint32_t i=0;i<count;++i) if (ids[i]>=state.chunks) return PG_CHUNK;
    return 0;
}
static int load_metadata(void) {
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    if (fd<0) return 0;
    unsigned char header[256];
    if (runtime_file_read(fd,header,sizeof(header))!=sizeof(header) || u32(header)!=UINT32_C(0x6f676c70)) {
        runtime_file_close(fd); return 0;
    }
    uint32_t size=u32(header+16),chunks=u16(header+10),microchunks=u16(header+12);
    if (size<256 || size>1024*1024 || !chunks || chunks>1000 || microchunks>8000) {
        runtime_file_close(fd); return 0;
    }
    unsigned char *bytes=malloc(size);
    if (!bytes) { runtime_file_close(fd); return 0; }
    memcpy(bytes,header,sizeof(header));
    int valid=runtime_file_read(fd,bytes+256,size-256)==size-256;
    runtime_file_close(fd);
    uint32_t attrs=u32(header+192),attrs_size=u32(header+196);
    uint32_t lists=u32(header+200),lists_size=u32(header+204);
    uint32_t micros=u32(header+216),micros_size=u32(header+220);
    valid=valid && span(attrs,attrs_size,size) && attrs_size>=chunks*32 &&
          span(lists,lists_size,size) && span(micros,micros_size,size) && micros_size>=microchunks*16;
    for (uint32_t i=0;valid && i<chunks;++i) {
        const unsigned char *attr=bytes+attrs+i*32;
        uint32_t count=u16(attr+14),offset=u32(attr+24);
        if (!span(offset,count*2,lists_size)) { valid=0; break; }
        uint64_t total=0;
        for (uint32_t j=0;j<count;++j) {
            uint32_t id=u16(bytes+lists+offset+j*2);
            if (id>=microchunks) { valid=0; break; }
            uint64_t part=u64(bytes+micros+id*16+8)&UINT64_C(0x0000ffffffffffff);
            if (part>UINT64_MAX-total) { valid=0; break; }
            total+=part;
        }
        state.sizes[i]=total;
    }
    if (valid) state.chunks=chunks;
    free(bytes);
    return valid;
}
static ABI int32_t initialize(const InitParams *params) {
    if (!params || !params->buffer) return PG_POINTER;
    if (params->size<0x200000) return PG_SIZE;
    pthread_mutex_lock(&lock);
    int32_t error=state.initialized ? PG_ERROR(6) : 0;
    if (!error && !load_metadata()) error=PG_ERROR(14);
    if (!error) {
        state.initialized=1;
        state.speed=1;
        state.language=UINT64_C(1)<<62;
        printf("PlayGo: %u chunks from package metadata, verified local content profile\n",state.chunks);
    }
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t open_handle(uint32_t *handle,const void *params) {
    (void)params;
    if (!handle) return PG_POINTER;
    pthread_mutex_lock(&lock);
    int32_t error=state.initialized ? 0 : PG_ERROR(5);
    if (!error) { state.opened=1; *handle=1; }
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t get_speed(uint32_t handle,int32_t *speed) {
    if (!speed) return PG_POINTER;
    pthread_mutex_lock(&lock);
    int32_t error=handle_valid(handle);
    if (!error) {
        uint64_t tick=now_ms();
        if (!state.speed && tick>=state.speed_tick && tick-state.speed_tick>30000) state.speed=1;
        *speed=state.speed;
    }
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t set_speed(uint32_t handle,int32_t speed) {
    if (speed<0 || speed>2) return PG_ERROR(4);
    pthread_mutex_lock(&lock);
    int32_t error=handle_valid(handle);
    if (!error) { state.speed=speed; state.speed_tick=now_ms(); }
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t get_language(uint32_t handle,uint64_t *mask) {
    if (!mask) return PG_POINTER;
    pthread_mutex_lock(&lock);
    int32_t error=handle_valid(handle);
    if (!error) *mask=state.language;
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t set_language(uint32_t handle,uint64_t mask) {
    pthread_mutex_lock(&lock);
    int32_t error=handle_valid(handle);
    if (!error) state.language=mask;
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t get_chunks(uint32_t handle,uint16_t *ids,uint32_t capacity,uint32_t *count) {
    if (!count) return PG_POINTER;
    if (ids && !capacity) return PG_SIZE;
    pthread_mutex_lock(&lock);
    int32_t error=handle_valid(handle);
    if (!error) {
        *count=!ids || capacity>state.chunks ? state.chunks : capacity;
        if (ids) for (uint32_t i=0;i<*count;++i) ids[i]=(uint16_t)i;
    }
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t get_locus(uint32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (!loci) return PG_POINTER;
    pthread_mutex_lock(&lock);
    int32_t error=ids_valid(handle,ids,count);
    if (!error) memset(loci,3,count);
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t get_progress(uint32_t handle,const uint16_t *ids,uint32_t count,Progress *out) {
    if (!out) return PG_POINTER;
    pthread_mutex_lock(&lock);
    int32_t error=ids_valid(handle,ids,count);
    if (!error) {
        uint64_t total=0;
        for (uint32_t i=0;i<count;++i) {
            if (state.sizes[ids[i]]>UINT64_MAX-total) { error=PG_ERROR(4); break; }
            total+=state.sizes[ids[i]];
        }
        if (!error) out->progress=out->total=total;
    }
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t get_eta(uint32_t handle,const uint16_t *ids,uint32_t count,int64_t *eta) {
    if (!eta) return PG_POINTER;
    pthread_mutex_lock(&lock);
    int32_t error=ids_valid(handle,ids,count);
    if (!error) *eta=0;
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t get_todo(uint32_t handle,void *todo,uint32_t capacity,uint32_t *count) {
    if (!todo || !count) return PG_POINTER;
    if (!capacity) return PG_SIZE;
    pthread_mutex_lock(&lock);
    int32_t error=handle_valid(handle);
    if (!error) *count=0;
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t prefetch(uint32_t handle,const uint16_t *ids,uint32_t count,int8_t locus) {
    if (locus!=2 && locus!=3) return PG_ERROR(16);
    pthread_mutex_lock(&lock);
    int32_t error=ids_valid(handle,ids,count);
    pthread_mutex_unlock(&lock);
    return error;
}
uintptr_t runtime_playgo_resolve(const char *name) {
    static const RuntimeExport exports[]={
        {"scePlayGoInitialize",initialize}, {"scePlayGoOpen",open_handle},
        {"scePlayGoGetChunkId",get_chunks}, {"scePlayGoGetInstallSpeed",get_speed},
        {"scePlayGoSetInstallSpeed",set_speed}, {"scePlayGoGetLanguageMask",get_language},
        {"scePlayGoSetLanguageMask",set_language}, {"scePlayGoGetProgress",get_progress},
        {"scePlayGoGetLocus",get_locus}, {"scePlayGoGetEta",get_eta},
        {"scePlayGoGetToDoList",get_todo}, {"scePlayGoPrefetch",prefetch},
    };
    return RUNTIME_LOOKUP(exports,name);
}
