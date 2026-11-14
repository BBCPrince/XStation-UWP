/* SPDX-License-Identifier: MIT
 * Bounded sampled own-thread call chains. No guest memory or GPU state touched.
 * Capture occurs before the memory-table lock and before timed copy service.
 */
typedef USHORT (WINAPI *CpuCapture948)(ULONG,ULONG,PVOID*,PULONG);
typedef struct {uintptr_t frames[8];unsigned count;uint64_t samples,bytes,ticks;} CpuChain948;
static __thread CpuChain948 cpu_chains948[64];
static __thread uint64_t cpu_rng948=UINT64_C(0x4d595df4d0f33173);
static __thread unsigned cpu_overflow948;
static __thread CpuCapture948 cpu_capture948;
static __thread int cpu_capture_resolved948;
typedef struct {int slot;uint64_t began;} CpuSample948;
static __attribute__((noinline)) CpuSample948 cpu_copy_begin948(int enabled) {
    CpuSample948 sample={-1,0};if(!enabled)return sample;
    uint64_t x=cpu_rng948;x^=x>>12;x^=x<<25;x^=x>>27;cpu_rng948=x;
    if(((x*UINT64_C(2685821657736338717))>>54)!=0)return sample;
    if(!cpu_capture_resolved948){
        cpu_capture_resolved948=1;
        cpu_capture948=(CpuCapture948)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"RtlCaptureStackBackTrace");
        if(!cpu_capture948)cpu_capture948=(CpuCapture948)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlCaptureStackBackTrace");
    }
    if(!cpu_capture948)return sample;
    void *frames[8]={0};unsigned n=cpu_capture948(0,8,frames,NULL);if(!n)return sample;
    for(unsigned i=0;i<64;i++){
        CpuChain948 *c=&cpu_chains948[i];
        if(!c->count){c->count=n;memcpy(c->frames,frames,n*sizeof(void*));}
        if(c->count==n&&!memcmp(c->frames,frames,n*sizeof(void*))) {
            sample.slot=(int)i;sample.began=upload_clock();return sample;
        }
    }
    ++cpu_overflow948;return sample;
}
static void cpu_copy_end948(CpuSample948 sample,uint64_t size) {
    if(sample.slot<0)return;CpuChain948 *c=&cpu_chains948[sample.slot];
    ++c->samples;c->bytes+=size;c->ticks+=upload_clock()-sample.began;
}
static void cpu_copy_report948(uint64_t tick,uint64_t frequency) {
    for(unsigned i=0;i<64;i++){
        CpuChain948 *c=&cpu_chains948[i];if(!c->samples)continue;
        printf("COPY948 chain: tick=%llu tid=%lu samples=%llu bytes=%llu total_ms=%.6f depth=%u overflow=%u",
            (unsigned long long)tick,(unsigned long)GetCurrentThreadId(),(unsigned long long)c->samples,
            (unsigned long long)c->bytes,(double)c->ticks*1000.0/frequency,c->count,cpu_overflow948);
        for(unsigned j=0;j<c->count;j++)printf(" f%u=0x%llx",j,(unsigned long long)c->frames[j]);
        putchar('\n');c->samples=c->bytes=c->ticks=0;
    }
    cpu_overflow948=0;
}
