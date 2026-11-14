/* God of War (2018): sce::Job::JobManager (libSceJobManager), stage 1 (2026-11-03): a probe.
 * No public description of the library exists, so the members the game calls are first recorded:
 * construction, sizing and initialization succeed (memory is the caller's), and the interface
 * getSequenceFactoryInterface returns is an object whose virtual functions (48 slots) each stop the
 * game with the slot number, the arguments and the caller's image offset. Those reports decide
 * what stage 2 implements. Member functions: this in rdi (System V). */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uintptr_t runtime_guest_image_base(void); /* probe.c */
#define SLOTS 48

static void say(const char *what, uintptr_t self, uintptr_t a, uintptr_t b, uintptr_t caller) {
    printf("JobManager: %s(this=%p, 0x%llx, 0x%llx) from image+0x%llx\n",what,(void *)self,
           (unsigned long long)a,(unsigned long long)b,(unsigned long long)(caller-runtime_guest_image_base()));
    fflush(stdout);
}
static __attribute__((noreturn)) void slot_called(int slot, uintptr_t self, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                                                   uintptr_t a4, uintptr_t caller) {
    printf("STOP: JobManager sequence factory virtual slot %d (vtable+0x%x) called: this=%p args 0x%llx 0x%llx 0x%llx 0x%llx; "
           "caller image+0x%llx\n",slot,slot*8,(void *)self,(unsigned long long)a1,(unsigned long long)a2,(unsigned long long)a3,
           (unsigned long long)a4,(unsigned long long)(caller-runtime_guest_image_base()));
    runtime_report();
    fflush(NULL);
    exit(20);
}
#define SLOT(n) static ABI uintptr_t slot_##n(uintptr_t self, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) { \
    slot_called(n,self,a1,a2,a3,a4,(uintptr_t)__builtin_return_address(0)); }
SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11)
SLOT(12) SLOT(13) SLOT(14) SLOT(15) SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23)
SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31) SLOT(32) SLOT(33) SLOT(34) SLOT(35)
SLOT(36) SLOT(37) SLOT(38) SLOT(39) SLOT(40) SLOT(41) SLOT(42) SLOT(43) SLOT(44) SLOT(45) SLOT(46) SLOT(47)
static const void *const factory_vtable[SLOTS]={
    slot_0,slot_1,slot_2,slot_3,slot_4,slot_5,slot_6,slot_7,slot_8,slot_9,slot_10,slot_11,
    slot_12,slot_13,slot_14,slot_15,slot_16,slot_17,slot_18,slot_19,slot_20,slot_21,slot_22,slot_23,
    slot_24,slot_25,slot_26,slot_27,slot_28,slot_29,slot_30,slot_31,slot_32,slot_33,slot_34,slot_35,
    slot_36,slot_37,slot_38,slot_39,slot_40,slot_41,slot_42,slot_43,slot_44,slot_45,slot_46,slot_47};
static struct { const void *const *vtable; uint64_t priority; } factories[8];

static ABI void construct(uintptr_t self) { say("JobManager()",self,0,0,(uintptr_t)__builtin_return_address(0)); }
static ABI void destruct(uintptr_t self) { say("~JobManager()",self,0,0,(uintptr_t)__builtin_return_address(0)); }
static ABI uint64_t required_memory(const uint64_t *params) {
    say("calculateRequiredMemorySize",0,params ? params[0] : 0,params ? params[1] : 0,(uintptr_t)__builtin_return_address(0));
    return 1u<<20;
}
static ABI int32_t initialize(uintptr_t self, void *memory, uint64_t size, uint32_t a, uint32_t b, uint32_t c, uint64_t d, const void *config) {
    say("initialize",self,(uintptr_t)memory,size,(uintptr_t)__builtin_return_address(0));
    printf("JobManager: initialize args %u %u %u 0x%llx config=%p\n",a,b,c,(unsigned long long)d,config);
    return 0;
}
static ABI int32_t set_persistent_threads(uintptr_t self, uint32_t count) {
    say("setPersistentThreadCount",self,count,0,(uintptr_t)__builtin_return_address(0));
    return 0;
}
static ABI void *sequence_factory(uintptr_t self, uint32_t priority) {
    say("getSequenceFactoryInterface",self,priority,0,(uintptr_t)__builtin_return_address(0));
    const uint32_t i=priority<8 ? priority : 7;
    factories[i].vtable=factory_vtable; factories[i].priority=priority;
    return &factories[i];
}
static ABI int32_t shutdown_manager(uintptr_t self) { say("shutdown",self,0,0,(uintptr_t)__builtin_return_address(0)); return 0; }

static const RuntimeExport exports[]={
    {"_ZN3sce3Job10JobManagerC1Ev",construct}, {"_ZN3sce3Job10JobManagerD1Ev",destruct},
    {"_ZN3sce3Job10JobManager27calculateRequiredMemorySizeEPKNS1_21MemorySizeQueryParamsE",required_memory},
    {"_ZN3sce3Job10JobManager10initializeEPvmjjjmPKNS1_12ConfigParamsE",initialize},
    {"_ZN3sce3Job10JobManager24setPersistentThreadCountEj",set_persistent_threads},
    {"_ZN3sce3Job10JobManager27getSequenceFactoryInterfaceENS1_8PriorityE",sequence_factory},
    {"_ZN3sce3Job10JobManager8shutdownEv",shutdown_manager},
};
uintptr_t runtime_jobmanager_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
