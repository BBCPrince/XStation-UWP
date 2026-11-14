/* SPDX-License-Identifier: GPL-2.0-or-later */
extern const unsigned char yield570_begin[],yield570_end[],yield570_modeptr[],yield570_callbackptr[],yield570_normalptr[],yield570_sleepptr[];
extern volatile LONG yield570_mode;
extern void yield570_callback(void);
extern void yield570_start(void);
static int yield570_supported(void) {
    unsigned a,b,c,d;uint32_t lo,hi;
    if(!__get_cpuid(1,&a,&b,&c,&d) || (c&((1u<<26)|(1u<<27)|(1u<<28)))!=((1u<<26)|(1u<<27)|(1u<<28)))return 0;
    __asm__ volatile("xgetbv":"=a"(lo),"=d"(hi):"c"(0));(void)hi;
    if((lo&7)!=7)return 0;
    __cpuid_count(0xd,2,a,b,c,d);
    return a && b>=576 && b+a<=832; /* standard XSAVE AVX state fits reserved area */
}
static int yield570_install(unsigned char* base,uint64_t size,const Segment* segments,uint64_t count) {
    static const unsigned char expected[]={0x80,0x7f,0x59,0,0x75,4,0xf3,0x90,0xeb,0x59,0x49,0x81,0xf8,0xe8,3,0,0};
    if(size<0xaaca36+sizeof(expected) || !sleep551_rx(segments,count,0xaaca36,sizeof(expected)) || memcmp(base+0xaaca36,expected,sizeof(expected)) || !yield570_supported())return 0;
    const size_t bytes=(uintptr_t)yield570_end-(uintptr_t)yield570_begin;
    if(bytes>page_size || bytes<32)return 0;
    unsigned char* thunk=allocate(page_size);
    const intptr_t relative=(intptr_t)thunk-(intptr_t)(base+0xaaca3b);
    if(relative<INT32_MIN || relative>INT32_MAX)return 0;
    memcpy(thunk,yield570_begin,bytes);
    const struct {const unsigned char* slot;uintptr_t value;} slots[]={
        {yield570_modeptr,(uintptr_t)&yield570_mode},{yield570_callbackptr,(uintptr_t)&yield570_callback},
        {yield570_normalptr,(uintptr_t)(base+0xaaca99)},{yield570_sleepptr,(uintptr_t)(base+0xaaca40)}};
    for(unsigned i=0;i<4;++i){size_t at=(uintptr_t)slots[i].slot-(uintptr_t)yield570_begin;if(at>bytes-8)return 0;memcpy(thunk+at,&slots[i].value,8);}
    protect(thunk,page_size,5);
    const int32_t rel=(int32_t)relative;
    base[0xaaca36]=0xe9;memcpy(base+0xaaca37,&rel,4);memset(base+0xaaca3b,0x90,5);
    printf("YIELD570 patch: validated=1 offset=0xaaca36 thunk=%p bytes=%zu interval=1024 global_sleep_unchanged=1 before_guest_entry=1\n",(void*)thunk,bytes);
    return 1;
}
