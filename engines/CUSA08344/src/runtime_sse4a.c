/* SSE4a for CPUs without it (2026-11-03, God of War port). God of War was built for the PS4's AMD
 * Jaguar and uses INSERTQ/EXTRQ; AMD CPUs (the Xbox's Zen 2) run them, Intel ones fault with an
 * illegal instruction. On a CPU without SSE4a (CPUID 0x80000001 ECX bit 6) such a fault is
 * emulated on the faulting thread's XMM registers and execution continues after the instruction.
 * Forms: EXTRQ xmm, imm8, imm8 (66 0F 78 /0); EXTRQ xmm, xmm (66 0F 79);
 *        INSERTQ xmm, xmm, imm8, imm8 (F2 0F 78); INSERTQ xmm, xmm (F2 0F 79); REX allowed.
 * The upper 64 bits of the destination are architecturally undefined: left unchanged. */
#include "runtime.h"
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#include <cpuid.h>

static volatile LONG64 emulated;
static uint64_t field_mask(unsigned length) { return length>=64 ? ~UINT64_C(0) : (UINT64_C(1)<<length)-1; }
static LONG CALLBACK sse4a_fault(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode!=EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c=info->ContextRecord;
    const uint8_t *p=(const uint8_t *)(uintptr_t)c->Rip;
    unsigned i=0;
    const uint8_t prefix=p[i++];
    if (prefix!=0x66 && prefix!=0xf2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t rex=0;
    if ((p[i]&0xf0)==0x40) rex=p[i++];
    if (p[i++]!=0x0f) return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t op=p[i++];
    if (op!=0x78 && op!=0x79) return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t modrm=p[i++];
    if ((modrm>>6)!=3) return EXCEPTION_CONTINUE_SEARCH; /* register operands only */
    const unsigned reg=((modrm>>3)&7)|((rex&4) ? 8 : 0), rm=(modrm&7)|((rex&1) ? 8 : 0);
    M128A *x=&c->Xmm0;
    unsigned length, index, dst;
    if (prefix==0x66) { /* EXTRQ: dst = (dst >> index) & mask(length) */
        if (op==0x78) {
            if ((modrm>>3)&7) return EXCEPTION_CONTINUE_SEARCH;
            dst=rm; length=p[i]&63; index=p[i+1]&63; i+=2;
        } else {
            dst=reg; length=(unsigned)(x[rm].Low&63); index=(unsigned)((x[rm].Low>>8)&63);
        }
        if (!length) length=64;
        if (index+length>64) length=64-index;
        x[dst].Low=(x[dst].Low>>index)&field_mask(length);
    } else { /* INSERTQ: the low `length` bits of src into dst at `index` */
        dst=reg;
        const unsigned src=rm;
        if (op==0x78) { length=p[i]&63; index=p[i+1]&63; i+=2; }
        else { length=(unsigned)(x[src].High&63); index=(unsigned)((x[src].High>>8)&63); }
        if (!length) length=64;
        if (index+length>64) length=64-index;
        const uint64_t mask=field_mask(length);
        x[dst].Low=(x[dst].Low&~(mask<<index))|((x[src].Low&mask)<<index);
    }
    c->Rip+=i;
    if (_InterlockedIncrement64(&emulated)==1) {
        printf("CPU: SSE4a instruction emulated (this CPU has no SSE4a; the Xbox runs it natively), first at %p\n",(void *)p);
        fflush(stdout);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
void runtime_sse4a_install(void) {
    unsigned a,b,cx,d;
    if (__get_cpuid(0x80000001,&a,&b,&cx,&d) && (cx & (1u<<6))) return; /* SSE4a present */
    if (AddVectoredExceptionHandler(1,sse4a_fault)) puts("CPU: no SSE4a; INSERTQ/EXTRQ are emulated (slow, PC tests only)");
}
uint64_t runtime_sse4a_emulated(void) { return (uint64_t)emulated; }
#else
void runtime_sse4a_install(void) {}
uint64_t runtime_sse4a_emulated(void) { return 0; }
#endif
