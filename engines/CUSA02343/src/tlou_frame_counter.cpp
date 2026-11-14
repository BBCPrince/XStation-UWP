#include <atomic>
#include <cstdint>
extern "C" {
__declspec(dllexport) std::atomic<uint64_t> g_bbv194_game_presents{},g_bbv194_reused_presents{},g_bbv194_all_presents{};
}
extern "C" void RealBb194Present(void*,void*,bool,bool) __asm__("__real__ZN6Vulkan9Presenter7PresentEPNS_5FrameEbb");
extern "C" void WrapBb194Present(void*,void*,bool,bool) __asm__("__wrap__ZN6Vulkan9Presenter7PresentEPNS_5FrameEbb");
extern "C" void WrapBb194Present(void*p,void*f,bool reused,bool game) {
 RealBb194Present(p,f,reused,game);
 g_bbv194_all_presents.fetch_add(1,std::memory_order_relaxed);
 if(reused) g_bbv194_reused_presents.fetch_add(1,std::memory_order_relaxed);
 if(game&&!reused) g_bbv194_game_presents.fetch_add(1,std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) uint64_t tlou194_unique_game_frames(void){return g_bbv194_game_presents.load(std::memory_order_relaxed);}
extern "C" __declspec(dllexport) uint64_t tlou194_reused_presents(void){return g_bbv194_reused_presents.load(std::memory_order_relaxed);}
