// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
extern "C" {alignas(64) volatile LONG yield570_mode=0;}
namespace Yield570 {
struct alignas(64) Slot {std::atomic<uint64_t> calls{0},success{0};};
std::array<Slot,64> slots;std::atomic<unsigned> count{0};
unsigned Mode(const std::string& p) {FILE* f=std::fopen(p.c_str(),"rb");if(!f)return 0;unsigned mode=0;char extra;const int n=std::fscanf(f,"%u %c",&mode,&extra);std::fclose(f);return n==1 && mode<=2?mode:0;}
}
extern "C" void yield570_callback(void) {
    const LONG mode=InterlockedCompareExchange(&yield570_mode,0,0);
    if(!mode)return;
    thread_local const unsigned index=Yield570::count.fetch_add(1,std::memory_order_relaxed);
    bool success=false;
    if(mode==1)success=SwitchToThread()!=0;
    else if(mode==2){Sleep(1);success=true;}
    if(index<Yield570::slots.size()) {auto& s=Yield570::slots[index];s.calls.fetch_add(1,std::memory_order_relaxed);s.success.fetch_add(unsigned(success),std::memory_order_relaxed);}
}
extern "C" void yield570_start(void) {
    const char* dir=std::getenv("BB_GPU_USER_DIR");if(!dir)return;
    const std::string path=std::string(dir)+"\\worker-yield-mode.txt";
    InterlockedExchange(&yield570_mode,Yield570::Mode(path));
    std::printf("YIELD570 start: mode=%ld priority_fix_unchanged=1 global_sleep_unchanged=1\n",yield570_mode);std::fflush(stdout);
    std::thread([path] {
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
        uint64_t last=GetTickCount64(),previous=0,previous_success=0;
        for(;;) {
            Sleep(1000);const LONG mode=Yield570::Mode(path);const LONG old=InterlockedExchange(&yield570_mode,mode);
            if(mode!=old){std::printf("YIELD570 switch: tick=%llu old=%ld mode=%ld\n",(unsigned long long)GetTickCount64(),old,mode);std::fflush(stdout);}
            const auto now=GetTickCount64();if(now-last<5000)continue;
            uint64_t calls=0,success=0;for(auto& s:Yield570::slots){calls+=s.calls.load(std::memory_order_relaxed);success+=s.success.load(std::memory_order_relaxed);}
            std::printf("YIELD570 totals: tick=%llu mode=%ld wall_ms=%llu calls=%llu success=%llu threads=%u\n",(unsigned long long)now,mode,(unsigned long long)(now-last),(unsigned long long)(calls-previous),(unsigned long long)(success-previous_success),Yield570::count.load(std::memory_order_relaxed));std::fflush(stdout);
            previous=calls;previous_success=success;last=now;
        }
    }).detach();
}
