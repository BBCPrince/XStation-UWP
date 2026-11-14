// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from the user's Spider-Man native compiler cache. Keep all source,
// driver, firmware, serializer and machine-code guards in the copied header.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <winrt/base.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <fstream>
#include <mutex>
#include <string>
#include "NativeCompilerDiskCache.h"

std::wstring XStationTitleFolder(); // game_app.cpp

namespace {
namespace N=SpiderNativeCompilerCache;
std::once_flag configured;
std::wstring root;
bool enabled=false;
uint64_t driverIdentity=0,lastPoll=0;
bool driverRejected=false;
N::Attribution totals;
void Log(const std::string& line) noexcept {
    try {std::ofstream f(root+L"\\native-compiler-cache.log",std::ios::app);f<<GetTickCount64()<<' '<<line<<'\n';}catch(...){}
}
void Configure(){
    std::call_once(configured,[]{
        // XStation: the game's own folder (Titles\<id>), so the switch and the cache are per game.
        root=XStationTitleFolder();
        if(root.empty()) root=winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str();
        std::ifstream f(root+L"\\native-compiled-cache-mode.txt");
        unsigned value=0;
        if(f>>value){f>>std::ws;enabled=value==1&&f.eof();}
        Log(std::string("native_cache_configured enabled=")+(enabled?"1":"0")+" disk_limit_MiB=256 evict=lru free_disk_floor_MiB=64 memory_reserve_MiB=64");
    });
}
bool Room(uint64_t bytes) noexcept {
    const DWORD previous=GetLastError();bool ok=false;
    try {
        const auto usage=winrt::Windows::System::MemoryManager::AppMemoryUsage();
        const auto limit=winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
        ok=limit>usage && bytes<=limit-usage && limit-usage-bytes>=64ull*1024*1024;
    }catch(...){}
    SetLastError(previous);return ok;
}
bool Install(ID3D12Device* device){
    if(N::installed)return true;
    if(N::initialized||driverRejected||!GetModuleHandleW(L"xbsc_xs.dll"))return false;
    if(!driverIdentity){
        winrt::com_ptr<IDXGIFactory4> factory;winrt::com_ptr<IDXGIAdapter1> adapter;
        LARGE_INTEGER identity{};
        if(!device || FAILED(CreateDXGIFactory2(0,__uuidof(IDXGIFactory4),factory.put_void())) ||
           FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),__uuidof(IDXGIAdapter1),adapter.put_void())) ||
           FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&identity)) || !identity.QuadPart){
            driverRejected=true;Log("native_cache_install=0 reason=driver_identity_unavailable");return false;
        }
        driverIdentity=static_cast<uint64_t>(identity.QuadPart);
    }
    const bool ok=N::Install(std::filesystem::path(root)/L"tlou-native-compiled-cache",driverIdentity,&Room);
    Log(std::string("native_cache_install=")+(ok?"1":"0")+" compiler_abi_guard=1 firmware_guard=1 driver="+std::to_string(driverIdentity));
    return ok;
}
}

// A token holds the existing cache's serialization gate across ONE native PSO
// creation. Plain C ABI across host/Mesa CRTs; no C++ objects cross that boundary.
extern "C" __declspec(dllexport) uint32_t __cdecl tlou_native_cache_enter(void* raw) noexcept {
    bool locked=false;
    try {
        Configure();if(!enabled)return 0;
        N::callMutex.lock();locked=true;
        if(!Install(static_cast<ID3D12Device*>(raw))){N::callMutex.unlock();return 0;}
        const uint32_t token=N::active?3:1;N::active=true;N::attribution=&totals;
        return token;
    }catch(...){if(locked)N::callMutex.unlock();return 0;}
}
extern "C" __declspec(dllexport) void __cdecl tlou_native_cache_leave(uint32_t token,void* raw) noexcept {
    if(token&1){
        N::active=(token&2)!=0;
        if(!N::active)N::attribution=nullptr;
        N::callMutex.unlock();return;
    }
    // The first device call can load the compiler. Install after that call,
    // before the next startup PSO; no game threads have started at this point.
    try {Configure();if(!enabled)return;std::lock_guard<std::recursive_mutex> lock(N::callMutex);Install(static_cast<ID3D12Device*>(raw));}catch(...){}
}
extern "C" void tlou_native_cache_poll() noexcept {
    try {
        Configure();if(!enabled)return;
        std::unique_lock<std::recursive_mutex> lock(N::callMutex,std::try_to_lock);
        if(!lock.owns_lock())return;
        const auto now=GetTickCount64();if(now-lastPoll<5000)return;lastPoll=now;
        Log("native_cache_stats installed="+std::to_string(N::installed)+
            " requests="+std::to_string(N::requests.load())+" hits="+std::to_string(N::hits.load())+
            " misses="+std::to_string(N::misses.load())+" saved="+std::to_string(N::saved.load())+" evicted="+std::to_string(N::evicted.load())+
            " rejected="+std::to_string(N::rejected.load())+" errors="+std::to_string(N::errors.load())+
            " native_calls="+std::to_string(N::nativeCalls.load())+" held_bytes="+std::to_string(N::heldBytes.load())+
            " peak_held_bytes="+std::to_string(N::peakHeldBytes.load())+" disk_bytes="+std::to_string(N::diskBytes)+
            " compile_ms="+std::to_string(totals.compileMs)+" source_ms="+std::to_string(totals.sourceMs)+
            " load_ms="+std::to_string(totals.loadMs)+" restore_ms="+std::to_string(totals.restoreMs)+
            " store_ms="+std::to_string(totals.storeMs));
    }catch(...){}
}
