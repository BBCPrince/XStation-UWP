// SPDX-License-Identifier: GPL-2.0-or-later

// Adapted from the locally snapshotted Bloodborne UWP host (see provenance.json).

// UWP owns the window/input; the TLOU native runtime and Dozen share this process.

#include <windows.h>

#include <windows.ui.xaml.media.dxinterop.h>

#include <dxgi1_4.h>

#include <winrt/Windows.ApplicationModel.h>

#include <winrt/Windows.ApplicationModel.Activation.h>

#include <winrt/Windows.Foundation.Collections.h>

#include <winrt/Windows.Gaming.Input.h>

#include <winrt/Windows.Storage.h>

#include <winrt/Windows.Storage.Streams.h>

#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include <winrt/Windows.System.h>

#include <winrt/Windows.UI.h>

#include <winrt/Windows.UI.Core.h>

#include <winrt/Windows.UI.ViewManagement.h>

#include <winrt/Windows.UI.Xaml.h>

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>

#include <winrt/Windows.UI.Xaml.Media.h>

#include <algorithm>

#include <atomic>

#include <chrono>

#include <cmath>

#include <cstdlib>

#include <fstream>

#include <mutex>

#include <string>

#include <thread>
#include <cstring>
#include <stdexcept>

#include "xbox_test_input.h"
#include "xbox_profile_snapshot.h"
#include "uwp_host.h"
#include "runtime_text_input.h"
#include "dependency_names.h"
#include "cxx_imports.h"
#include "dashboard.h"
#include "usb_files.h"
#include <map>
#include <ctime>
#include <vector>
#include <tlhelp32.h>
#include <winrt/Windows.ApplicationModel.Core.h>

using namespace winrt;

using namespace Windows::UI::Xaml;

using namespace Windows::UI::Xaml::Controls;

using namespace Windows::Storage;

using namespace Windows::Gaming::Input;

extern "C" void tlou_native_cache_poll() noexcept;

namespace {

HMODULE runtimeModule{};

std::atomic<int> phase{0}, nativeResult{-1};

std::atomic<DWORD> loadError{0};

std::wstring localPath;
// XStation: the game this process runs (empty in the dashboard).
std::wstring titleId, enginePath, gameFolder;
// XStation: Bloodborne's engine has its own entry points and folder names (1.09 layout).
bool bbEngine=false;
const wchar_t* BootFolderName() { return bbEngine ? L"GameBoot109" : L"GameBoot"; }
const wchar_t* DataFolderName() { return bbEngine ? L"GameData109" : L"GameData"; }
uint64_t statsSeconds{};
ULONGLONG statsBegan{};
void WriteStats() {
    std::ofstream f(localPath+L"\\stats.txt",std::ios::trunc);
    f<<statsSeconds+(GetTickCount64()-statsBegan)/1000<<' '<<static_cast<unsigned long long>(_time64(nullptr))<<'\n';
}
void Log(const std::string& value);
void ReturnToDashboard() {
    WriteStats();
    Log("usb_fallback_opens="+std::to_string(XStationUsbFilesFallbacks()));
    auto op=winrt::Windows::ApplicationModel::Core::CoreApplication::RequestRestartAsync(L"");
    op.Completed([](auto const& o,auto){
        if(o.GetResults()!=winrt::Windows::ApplicationModel::Core::AppRestartFailureReason::RestartPending)
            winrt::Windows::UI::Xaml::Window::Current().Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,[]{winrt::Windows::UI::Xaml::Application::Current().Exit();});
    });
}
HMODULE LoadEngine(const wchar_t* name) {
    return LoadPackagedLibrary((enginePath+L"\\"+name).c_str(),0);
}
bool dumpRendererProfile{};
using ShaderWarmupStatus=uint32_t(__cdecl*)(uint64_t*,uint32_t);
ShaderWarmupStatus shaderWarmupStatus{};
uint64_t __cdecl ShaderWarmupMemory() noexcept {
    try { return Windows::System::MemoryManager::AppMemoryUsage(); }
    catch (...) { return UINT64_MAX; } // Do not admit more PSOs if charge is unavailable.
}


void Log(const std::string& value) {

    std::ofstream out(localPath + L"\\uwp-host.log", std::ios::app);

    out << GetTickCount64() << ' ' << value << '\n';

}

void DiagnoseDependencies() {
    std::ofstream out(localPath+L"\\dependency-loads.csv",std::ios::trunc);
    out<<"module,load_error\n";
    unsigned absent=0;
    for (const auto* name:dependencyNames) {
        HMODULE module=LoadPackagedLibrary(name,0);
        const DWORD error=module ? 0 : GetLastError();
        out<<to_string(name)<<','<<error<<'\n';
        if (!module) { ++absent; Log("dependency_load_failed="+to_string(name)+"; error="+std::to_string(error)); }
        /* Retain successful loads for the lifetime of this diagnostic process. */
    }
    Log("dependency_load_failures="+std::to_string(absent));
    out.close();
    std::ofstream check(localPath+L"\\cxx-imports.csv",std::ios::trunc);
    check<<"module,symbol,load_error,found\n";
    std::map<std::wstring,std::pair<HMODULE,DWORD>> modules;
    for (const auto& entry:cxxImports) {
        auto [it,added]=modules.try_emplace(entry.module,std::pair<HMODULE,DWORD>{});
        if (added) { HMODULE h=LoadLibraryExW(entry.module,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32); it->second={h,h ? 0 : GetLastError()}; }
        const auto [h,error]=it->second;
        check<<to_string(entry.module)<<','<<entry.symbol<<','<<error<<','<<(h && GetProcAddress(h,entry.symbol) ? 1 : 0)<<'\n';
    }
    HMODULE cxx=LoadPackagedLibrary(L"libc++.dll",0);
    const DWORD cxxError=cxx ? 0 : GetLastError();
    Log("cxx_retry_error="+std::to_string(cxxError));
    runtimeModule=LoadEngine(L"tlou-uwp-runtime.dll");
    Log("runtime_retry_error="+std::to_string(runtimeModule ? 0 : GetLastError()));
}

static HANDLE loadTrace194=INVALID_HANDLE_VALUE;
static LONG loadTraceCount194=0;
static LONG CALLBACK LoadTrace194(EXCEPTION_POINTERS* ep) {
 if(loadTrace194!=INVALID_HANDLE_VALUE && InterlockedIncrement(&loadTraceCount194)<=32){
  HMODULE module{};char moduleName[512]{};char line[1024]{};DWORD written{};
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)ep->ExceptionRecord->ExceptionAddress,&module);
  if(module)GetModuleFileNameA(module,moduleName,sizeof(moduleName));
  int n=snprintf(line,sizeof(line),"exception=%08lx; address=%p; module=%s; rva=%llx; info=%llx,%llx\n",ep->ExceptionRecord->ExceptionCode,ep->ExceptionRecord->ExceptionAddress,moduleName,(unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress-(uintptr_t)module),(unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],(unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
  if(n>0&&n<sizeof(line)){WriteFile(loadTrace194,line,(DWORD)n,&written,nullptr);FlushFileBuffers(loadTrace194);}
  auto c=ep->ContextRecord;
  n=snprintf(line,sizeof(line),"rsp=%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx\n",c->Rsp,c->Rax,c->Rbx,c->Rcx,c->Rdx,c->R8,c->R9);
  if(n>0&&n<sizeof(line))WriteFile(loadTrace194,line,(DWORD)n,&written,nullptr);
  auto stack=(const uint64_t*)c->Rsp;
  n=snprintf(line,sizeof(line),"stack=%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx\n",stack[0],stack[1],stack[2],stack[3],stack[4],stack[5],stack[6],stack[7]);
  if(n>0&&n<sizeof(line)){WriteFile(loadTrace194,line,(DWORD)n,&written,nullptr);FlushFileBuffers(loadTrace194);}

 }
 return EXCEPTION_CONTINUE_SEARCH;
}
using OwnershipReport1562=void(__cdecl*)();
OwnershipReport1562 ownershipReport1562{};
unsigned ownershipSamples1562{};
using Unique194=uint64_t(__cdecl*)();
Unique194 unique194{},reused194{};
void InitializeRuntime() {
    // Xbox UWP exposes cs_6_4. Do not force SM6.6 descriptor-heap instructions.
    if(_putenv_s("DZN_DEBUG","")!=0){loadError=ERROR_INVALID_DATA;phase=3;return;}
    check_bool(SetEnvironmentVariableW(L"DZN_DEBUG",nullptr));
    Log("ownership_candidate1562=true; forced_bindless=false");

    // Developer profiling is selected before loading the runtime: its optional
    // timers cache BB_FRAME_STATS during DLL static initialization.
    std::ifstream profiling(localPath+L"\\frame-profiling.txt");
    unsigned profileEnabled=0;
    profiling>>profileEnabled;
    const bool profileFrames=profileEnabled==1;
    if (_putenv_s("BB_FRAME_STATS",profileFrames ? "1" : "")!=0) {
        loadError=ERROR_INVALID_DATA; phase=3; return;
    }
    check_bool(SetEnvironmentVariableW(L"BB_FRAME_STATS",profileFrames ? L"1" : nullptr));
    Log(std::string("frame_profiling=")+(profileFrames ? "1" : "0"));
    unsigned recorderEnabled=0;
    std::ifstream recorderControl(localPath+L"\\recorder-profile.txt");
    recorderControl>>recorderEnabled;
    dumpRendererProfile=recorderEnabled==1;
    unsigned signalWaitMode=0;
    std::ifstream signalControl(localPath+L"\\signal-wait-mode.txt");
    if(signalControl.is_open()) {
        if(!(signalControl>>signalWaitMode) || signalWaitMode>1) {loadError=ERROR_INVALID_DATA;phase=3;Log("invalid_signal_wait_mode=true");return;}
        signalControl>>std::ws;
        if(!signalControl.eof()) {loadError=ERROR_INVALID_DATA;phase=3;Log("invalid_signal_wait_mode=true");return;}
    }
    const char* signalWait=signalWaitMode ? "1" : "0";
    if(_putenv_s("BB_XBOX_SIGNAL_WAIT",signalWait)) {loadError=ERROR_INVALID_DATA;phase=3;return;}
    check_bool(SetEnvironmentVariableW(L"BB_XBOX_SIGNAL_WAIT",signalWaitMode ? L"1" : L"0"));
    Log("signal_wait_mode="+std::to_string(signalWaitMode));
    unsigned drawPipeOff=0;
    std::ifstream drawPipeControl(localPath+L"\\draw-pipeline-off.txt");
    if(drawPipeControl.is_open()) {
        if(!(drawPipeControl>>drawPipeOff) || drawPipeOff>1) {loadError=ERROR_INVALID_DATA;phase=3;Log("invalid_draw_pipeline_off=true");return;}
        drawPipeControl>>std::ws;
        if(!drawPipeControl.eof()) {loadError=ERROR_INVALID_DATA;phase=3;Log("invalid_draw_pipeline_off=true");return;}
    }
    if(_putenv_s("BB_DRAW_PIPE",drawPipeOff ? "0" : "")) {loadError=ERROR_INVALID_DATA;phase=3;return;}
    check_bool(SetEnvironmentVariableW(L"BB_DRAW_PIPE",drawPipeOff ? L"0" : nullptr));
    Log("draw_pipeline_off="+std::to_string(drawPipeOff));

    // Existing renderer option: retain top-level submitted command bytes until
    // the decoder coroutine retires. This does not copy game textures/VRAM.
    unsigned commandCopy=0;
    std::ifstream commandControl(localPath+L"\\command-copy-mode.txt");
    if(commandControl.is_open()) {
        if(!(commandControl>>commandCopy) || commandCopy>1) {loadError=ERROR_INVALID_DATA;phase=3;Log("invalid_command_copy_mode=true");return;}
        commandControl>>std::ws;
        if(!commandControl.eof()) {loadError=ERROR_INVALID_DATA;phase=3;Log("invalid_command_copy_mode=true");return;}
    }
    if(_putenv_s("BB_COPY_GPU_BUFFERS",commandCopy ? "1" : "0")) {loadError=ERROR_INVALID_DATA;phase=3;return;}
    check_bool(SetEnvironmentVariableW(L"BB_COPY_GPU_BUFFERS",commandCopy ? L"1" : L"0"));
    Log("command_copy_mode="+std::to_string(commandCopy));


    std::ifstream pacing(localPath+L"\\frame-limit.txt");
    unsigned frameLimit=0;
    pacing>>frameLimit;
    if (frameLimit!=0 && frameLimit!=30 && frameLimit!=60) {
        loadError=ERROR_INVALID_DATA; phase=3; return;
    }
    const auto frameLimitText=std::to_string(frameLimit);
    if (_putenv_s("BB_FPS_LIMIT",frameLimitText.c_str())!=0 ||
        _putenv_s("BB_VBLANK_HZ","60")!=0) {
        loadError=ERROR_INVALID_DATA; phase=3; return;
    }
    check_bool(SetEnvironmentVariableW(L"BB_FPS_LIMIT",to_hstring(frameLimitText).c_str()));
    check_bool(SetEnvironmentVariableW(L"BB_VBLANK_HZ",L"60"));
    Log("frame_limit="+frameLimitText+"; guest_vblank_hz=60");
    // XStation (2026-11-09): "60 FPS (FSR)" on the game page writes frame-gen.txt = 1 (The Last of Us only): FSR 3.1
    // frame generation with the engine's TLOU camera hookup. The engine keeps the game itself at 30.
    {
        std::ifstream frameGenFile(localPath+L"\\frame-gen.txt");
        int frameGen=0;
        frameGenFile>>frameGen;
        if (frameGen==1) {
            if (_putenv_s("BB_FRAME_GEN","fsr3")!=0 || _putenv_s("BB_FG_CAMERA","tlou")!=0) {
                loadError=ERROR_INVALID_DATA; phase=3; return;
            }
            check_bool(SetEnvironmentVariableW(L"BB_FRAME_GEN",L"fsr3"));
            check_bool(SetEnvironmentVariableW(L"BB_FG_CAMERA",L"tlou"));
        }
        Log(std::string("frame_generation=")+(frameGen==1?"fsr3":"off"));
    }
    // XStation (2026-11-09): "FSR On / Off" (Bloodborne) writes fsr-upscale.txt = 1 or 0: FSR 3.1 upscaling. No
    // file: the engine's bbport.ini decides (upscaler=fsr3 in XStation's Bloodborne defaults).
    {
        std::ifstream upscaleFile(localPath+L"\\fsr-upscale.txt");
        int upscale=-1;
        upscaleFile>>upscale;
        if (upscale==0 || upscale==1) {
            const char* value=upscale==1 ? "fsr3" : "off";
            if (_putenv_s("BB_UPSCALER",value)!=0) { loadError=ERROR_INVALID_DATA; phase=3; return; }
            check_bool(SetEnvironmentVariableW(L"BB_UPSCALER",upscale==1 ? L"fsr3" : L"off"));
        }
        // 2026-11-10: Bloodborne's game patch draws 960x540 whatever the scene setting; other settings add a white streak. Keep 540p.
        if (bbEngine && (upscale==0 || upscale==1)) {
            std::ofstream scene(localPath+L"\\scene-resolution.txt",std::ios::trunc);
            scene<<"540p";
        }
        Log(std::string("fsr_upscale=")+(upscale==1?"fsr3":upscale==0?"off":"ini"));
    }
    // XStation (2026-11-13): "CPU On / Off" on the game page writes cpu-counter.txt = 1 or 0: the engine's FPS and CPU
    // box (show_fps in this title's bbport.ini; every engine reads BB_CONFIG). No file: on.
    {
        const std::wstring ini=localPath+L"\\bbport.ini";
        if (_putenv_s("BB_CONFIG",to_string(ini).c_str())!=0) { loadError=ERROR_INVALID_DATA; phase=3; return; }
        check_bool(SetEnvironmentVariableW(L"BB_CONFIG",ini.c_str()));
        std::ifstream choiceFile(localPath+L"\\cpu-counter.txt");
        int choice=1; // 2026-11-14 (user): on for every game unless the game page turned it off
        { int value=1; if (choiceFile>>value) choice=value; }
        if (choice==0 || choice==1) {
            std::string kept;
            {
                std::ifstream in(ini);
                std::string line;
                while (std::getline(in,line)) {
                    if (!line.empty() && line.back()=='\r') line.pop_back();
                    if (line.rfind("show_fps=",0)!=0) kept+=line+"\n";
                }
            }
            std::ofstream out(ini,std::ios::trunc);
            out<<kept<<"show_fps="<<choice<<"\n";
        }
        Log(std::string("cpu_counter=")+(choice==1?"on":choice==0?"off":"ini"));
    }

    // The app's memory limit (5 GiB on Xbox; not in the job object): the renderer's caches evict
    // what is idle sooner as the game nears it (BbProcessMemory::GetAppPressure).
    const auto appLimitText=std::to_string(Windows::System::MemoryManager::AppMemoryUsageLimit()>>20);
    if (_putenv_s("BB_APP_LIMIT_MB",appLimitText.c_str())!=0) {
        loadError=ERROR_INVALID_DATA; phase=3; return;
    }
    check_bool(SetEnvironmentVariableW(L"BB_APP_LIMIT_MB",to_hstring(appLimitText).c_str()));
    Log("app_limit_env_MiB="+appLimitText);

    const auto installed = Windows::ApplicationModel::Package::Current().InstalledLocation().Path();

    const auto user=localPath+L"\\tlou-bb194-user";

    const auto icd=std::wstring(installed.c_str())+L"\\dzn_icd.x86_64.json";

    check_bool(SetEnvironmentVariableW(L"BB_GPU_USER_DIR",user.c_str()));

    check_bool(SetEnvironmentVariableW(L"VK_DRIVER_FILES",icd.c_str()));

    check_bool(SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE",L"~implicit~"));

    // Reuse the working Xbox driver's diagnostics before its first device is
    // created. Missing file means off; 3 enables bounded fault capture, while
    // 1/2 additionally trace commands and 2 requests the native debug layer.
    unsigned driverDiagnostics=0;
    std::ifstream driverControl(localPath+L"\\gpu-diagnostics.txt");
    if (driverControl.is_open()) {
        if (!(driverControl>>driverDiagnostics) || driverDiagnostics>3) {
            loadError=ERROR_INVALID_DATA; phase=3; Log("invalid_gpu_diagnostics=true"); return;
        }
        driverControl>>std::ws;
        if (!driverControl.eof()) {
            loadError=ERROR_INVALID_DATA; phase=3; Log("invalid_gpu_diagnostics=true"); return;
        }
    }
    const char* trace=driverDiagnostics==1 || driverDiagnostics==2 ? "1" : "0";
    const char* capture=driverDiagnostics ? "1" : "0";
    const char* nativeDebug=driverDiagnostics==2 ? "1" : "0";
    if (_putenv_s("DZN_PRIVATE_DRED",trace) ||
        _putenv_s("DZN_PRIVATE_DRED_CAPTURE",capture) ||
        _putenv_s("DZN_PRIVATE_NATIVE_DEBUG",nativeDebug) ||
        _putenv_s("BB_DUMP_SHADERS",capture)) {
        loadError=ERROR_INVALID_DATA; phase=3; return;
    }
    check_bool(SetEnvironmentVariableW(L"DZN_PRIVATE_DRED",to_hstring(trace).c_str()));
    check_bool(SetEnvironmentVariableW(L"DZN_PRIVATE_DRED_CAPTURE",to_hstring(capture).c_str()));
    check_bool(SetEnvironmentVariableW(L"DZN_PRIVATE_NATIVE_DEBUG",to_hstring(nativeDebug).c_str()));
    check_bool(SetEnvironmentVariableW(L"BB_DUMP_SHADERS",to_hstring(capture).c_str()));
    Log("gpu_diagnostics="+std::to_string(driverDiagnostics));

    if (driverDiagnostics) {
        const auto triggerPath=localPath+L"\\frame-capture-trigger.txt";
        const auto captureDir=to_string(localPath);
        const auto captureTrigger=to_string(triggerPath);
        if (_putenv_s("BB_CAPTURE_DIR",captureDir.c_str()) ||
            _putenv_s("BB_DIAGNOSTIC_IMAGE_DIR",captureDir.c_str()) ||
            _putenv_s("BB_CAPTURE_TRIGGER",captureTrigger.c_str())) {
            loadError=ERROR_INVALID_DATA; phase=3; return;
        }
        check_bool(SetEnvironmentVariableW(L"BB_CAPTURE_DIR",localPath.c_str()));
        check_bool(SetEnvironmentVariableW(L"BB_DIAGNOSTIC_IMAGE_DIR",localPath.c_str()));
        check_bool(SetEnvironmentVariableW(L"BB_CAPTURE_TRIGGER",triggerPath.c_str()));
        Log("frame_capture_trigger_ready=true");
    }

    // Developer-only startup selection, before memory metadata locks or the
    // runtime are initialized. Missing control retains the shared backend.
    unsigned memoryBackend=0;
    std::ifstream memoryControl(localPath+L"\\xbox-memory-backend.txt");
    if (memoryControl.is_open()) {
        if (!(memoryControl>>memoryBackend) || memoryBackend>2) {
            loadError=ERROR_INVALID_DATA; phase=3; Log("invalid_memory_backend=true"); return;
        }
        memoryControl>>std::ws;
        if (!memoryControl.eof()) {
            loadError=ERROR_INVALID_DATA; phase=3; Log("invalid_memory_backend=true"); return;
        }
    }
    const char* privateCpu=memoryBackend ? "1" : "0";
    const char* privateGpu=memoryBackend==2 ? "1" : "0";
    if (_putenv_s("BB_UWP_PRIVATE_CPU",privateCpu) ||
        _putenv_s("BB_UWP_PRIVATE_GPU",privateGpu)) {
        loadError=ERROR_INVALID_DATA; phase=3; return;
    }
    check_bool(SetEnvironmentVariableW(L"BB_UWP_PRIVATE_CPU",to_hstring(privateCpu).c_str()));
    check_bool(SetEnvironmentVariableW(L"BB_UWP_PRIVATE_GPU",to_hstring(privateGpu).c_str()));
    Log("memory_backend="+std::to_string(memoryBackend));

    loadTrace194=CreateFile2((localPath+L"\\runtime-load-exceptions194.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,CREATE_ALWAYS,nullptr);
    auto addVeh194=(PVOID(WINAPI*)(ULONG,PVECTORED_EXCEPTION_HANDLER))GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"AddVectoredExceptionHandler");
    auto removeVeh194=(ULONG(WINAPI*)(PVOID))GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"RemoveVectoredExceptionHandler");
    void* loadVeh194=addVeh194?addVeh194(1,LoadTrace194):nullptr;
    if(!LoadEngine(L"vulkan_dzn.dll")) Log("engine_dozen_load_error="+std::to_string(GetLastError()));
    runtimeModule=LoadEngine(bbEngine ? L"bb-uwp-runtime.dll" : L"tlou-uwp-runtime.dll");
    // XStation (2026-11-09): the engine and its DLLs are loaded now; their file calls get the USB fallback too.
    Log("usb_fallback_slots_engine="+std::to_string(XStationUsbFilesInstall()));
    { std::wofstream report(localPath+L"\\usb-files.txt",std::ios::trunc); report<<XStationUsbFilesReport(); }
    if(loadVeh194&&removeVeh194)removeVeh194(loadVeh194);
    if(loadTrace194!=INVALID_HANDLE_VALUE){CloseHandle(loadTrace194);loadTrace194=INVALID_HANDLE_VALUE;}


    if (!runtimeModule) { loadError=GetLastError(); phase=3; Log("runtime_load_error="+std::to_string(loadError.load())); DiagnoseDependencies(); return; }

    if (bbEngine) {
        using InitializeBb=int(__cdecl*)(const char*,const char*);
        const auto initializeBb=reinterpret_cast<InitializeBb>(GetProcAddress(runtimeModule,"bb_uwp_initialize"));
        if (!initializeBb || !GetProcAddress(runtimeModule,"bb_uwp_run_game")) { loadError=ERROR_PROC_NOT_FOUND; phase=3; Log("runtime_ABI_mismatch=true"); return; }
        const auto localText=to_string(localPath),packageText=to_string(gameFolder);
        const int started=initializeBb(localText.c_str(),packageText.c_str());
        Log("bloodborne_engine_initialize="+std::to_string(started));
        if (started) { loadError=started; phase=3; }
        return;
    }

    unsigned ownershipTrace=0;
    std::ifstream ownershipControl(localPath+L"\\ownership-profile.txt");
    if(ownershipControl.is_open()){
        if(!(ownershipControl>>ownershipTrace)||ownershipTrace>1){loadError=ERROR_INVALID_DATA;phase=3;return;}
        ownershipControl>>std::ws;if(!ownershipControl.eof()){loadError=ERROR_INVALID_DATA;phase=3;return;}
    }
    if(ownershipTrace){
        ownershipReport1562=reinterpret_cast<OwnershipReport1562>(GetProcAddress(runtimeModule,"tlou_buffer_ownership_report1550"));
        if(!ownershipReport1562){loadError=ERROR_PROC_NOT_FOUND;phase=3;return;}
    }
    Log("ownership_report_enabled="+std::to_string(ownershipTrace)+"; maximum_samples=180");
    using Version=uint32_t(__cdecl*)();

    const auto version=reinterpret_cast<Version>(GetProcAddress(runtimeModule,"tlou_uwp_abi_version"));

    if (!version || version()!=1 || !GetProcAddress(runtimeModule,"tlou_uwp_run_game")) {

        loadError=ERROR_PROC_NOT_FOUND; phase=3; Log("runtime_ABI_mismatch=true"); return;

    }

    using ConfigureWarmup=void(__cdecl*)(uint32_t,uint64_t,uint64_t,uint64_t(__cdecl*)());
    const auto configureWarmup=reinterpret_cast<ConfigureWarmup>(GetProcAddress(runtimeModule,"tlou_shader_warmup_configure"));
    shaderWarmupStatus=reinterpret_cast<ShaderWarmupStatus>(GetProcAddress(runtimeModule,"tlou_shader_warmup_status"));
    if(configureWarmup && shaderWarmupStatus) {
        configureWarmup(0,60000,0,&ShaderWarmupMemory);
        Log("shader_warmup_mode=0; upstream194_pipeline_cache=true");
    } else Log("shader_warmup_ABI_unavailable=true; upstream194_pipeline_cache=true");
    unique194=reinterpret_cast<Unique194>(GetProcAddress(runtimeModule,"tlou194_unique_game_frames"));
    reused194=reinterpret_cast<Unique194>(GetProcAddress(runtimeModule,"tlou194_reused_presents"));
    using Identity194=const char*(__cdecl*)();
    auto identity194=reinterpret_cast<Identity194>(GetProcAddress(runtimeModule,"tlou_uwp_identity"));
    if(!unique194 || !reused194 || !identity194){loadError=ERROR_PROC_NOT_FOUND;phase=3;return;}
    Log(std::string("runtime_identity194=")+identity194());

    using Initialize=uint32_t(__cdecl*)();
    const auto initialize=reinterpret_cast<Initialize>(GetProcAddress(runtimeModule,"tlou_uwp_initialize"));
    if (!initialize) { loadError=ERROR_PROC_NOT_FOUND; phase=3; return; }
    const uint32_t offset=initialize();
    if (offset<0x1480 || offset>=0x1680 || offset%8) { loadError=ERROR_INVALID_DATA; phase=3; return; }
    Log("runtime_loaded=true; ABI=1; early_tcb_offset="+std::to_string(offset));

}

// XStation: while a game plays, no other process of this app may hold memory: it would come out of
// the game's budget. A heap crash left one holding 386 MiB and the next run ran out about 460 MiB early.
void CloseEarlierProcesses(const std::wstring& root) {
    const auto kernel=GetModuleHandleW(L"kernel32.dll");
    const auto openProcess=reinterpret_cast<HANDLE(WINAPI*)(DWORD,BOOL,DWORD)>(GetProcAddress(kernel,"OpenProcess"));
    const auto imageName=reinterpret_cast<BOOL(WINAPI*)(HANDLE,DWORD,LPWSTR,PDWORD)>(GetProcAddress(kernel,"QueryFullProcessImageNameW"));
    const auto snapshot=reinterpret_cast<HANDLE(WINAPI*)(DWORD,DWORD)>(GetProcAddress(kernel,"CreateToolhelp32Snapshot"));
    // PROCESSENTRY32W's layout: the app target's headers hide it.
    struct ProcessEntry { DWORD dwSize,cntUsage,th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID,cntThreads,th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; wchar_t szExeFile[MAX_PATH]; };
    const auto first=reinterpret_cast<BOOL(WINAPI*)(HANDLE,ProcessEntry*)>(GetProcAddress(kernel,"Process32FirstW"));
    const auto next=reinterpret_cast<BOOL(WINAPI*)(HANDLE,ProcessEntry*)>(GetProcAddress(kernel,"Process32NextW"));
    const DWORD self=GetCurrentProcessId();
    const std::wstring pidFile=root+L"\\active-process.txt";
    std::vector<DWORD> candidates;
    { std::ifstream f(pidFile); DWORD pid=0; if(f>>pid&&pid&&pid!=self) candidates.push_back(pid); }
    size_t listed=0;
    if(snapshot&&first&&next){
        const HANDLE list=snapshot(2 /*TH32CS_SNAPPROCESS*/,0);
        if(list!=INVALID_HANDLE_VALUE){
            ProcessEntry entry{}; entry.dwSize=sizeof(entry);
            for(BOOL more=first(list,&entry);more;more=next(list,&entry)){
                ++listed;
                if(entry.th32ProcessID!=self&&_wcsicmp(entry.szExeFile,L"XStation.exe")==0&&
                   std::find(candidates.begin(),candidates.end(),entry.th32ProcessID)==candidates.end())
                    candidates.push_back(entry.th32ProcessID);
            }
            CloseHandle(list);
        }
    }
    Log("earlier_process_check; self="+std::to_string(self)+"; processes_listed="+std::to_string(listed)+"; candidates="+std::to_string(candidates.size()));
    if(openProcess&&imageName) for(const DWORD pid:candidates){
        const HANDLE process=openProcess(PROCESS_TERMINATE|PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
        if(!process){ Log("earlier_process="+std::to_string(pid)+"; open_error="+std::to_string(GetLastError())); continue; }
        // The saved number may belong to something else by now: only this app's own exe is closed.
        wchar_t name[1024]{}; DWORD size=1024;
        const bool named=imageName(process,0,name,&size)!=FALSE;
        const std::wstring path=named?name:L"";
        const bool ours=path.size()>=13&&_wcsicmp(path.c_str()+path.size()-13,L"\\XStation.exe")==0;
        if(ours){
            const BOOL closed=TerminateProcess(process,1);
            const DWORD gone=WaitForSingleObject(process,5000);
            Log("earlier_process="+std::to_string(pid)+"; terminated="+std::to_string(closed)+"; wait="+std::to_string(gone));
        } else Log("earlier_process="+std::to_string(pid)+"; not_ours");
        CloseHandle(process);
    }
    std::ofstream(pidFile,std::ios::trunc)<<self;
}

std::atomic<bool> keepCrashedProcess{false};
// After the runtime's own trace (later handlers run later): a corrupted heap can't recover, so the
// process ends now rather than waiting on the system's crash handling with its memory still held.
LONG CALLBACK EndOnFatal(PEXCEPTION_POINTERS info) {
    const DWORD code=info->ExceptionRecord->ExceptionCode;
    if((code==0xC0000374||code==0xC0000409||code==0xC0000602)&&!keepCrashedProcess.load())
        TerminateProcess(GetCurrentProcess(),code);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Developer switch, for testing the above; absent on a normal install. A real heap corruption.
void DevHeapCrash() {
    HeapSetInformation(nullptr,HeapEnableTerminationOnCorruption,nullptr,0);
    std::thread([]{
        const HANDLE heap=GetProcessHeap(); // The real crash was in the process heap, not a private one.
        void* block=HeapAlloc(heap,0,4096);
        void* keep=HeapAlloc(heap,0,4096);
        HeapFree(heap,0,block);
        HeapFree(heap,0,block); // Freed twice: the heap's own check reports c0000374.
        Log("dev_heap_crash_no_fault="+std::to_string(reinterpret_cast<uintptr_t>(keep)));
        RaiseException(0xC0000374,EXCEPTION_NONCONTINUABLE,0,nullptr);
    }).detach();
}

struct App : ApplicationT<App> {

    SwapChainPanel gamePanel{nullptr};

    // XStation: the game's own picture, from Play until the game shows frames (then released).
    Grid rootGrid{nullptr};
    Image loadingArt{nullptr};
    ULONGLONG loadingSince=0;

    fire_and_forget LoadArt(std::wstring path) {
        auto keep=get_strong();
        std::ifstream file(path,std::ios::binary);
        if(!file) co_return;
        std::string bytes((std::istreambuf_iterator<char>(file)),{});
        Windows::Storage::Streams::InMemoryRandomAccessStream stream;
        Windows::Storage::Streams::DataWriter writer(stream);
        writer.WriteBytes(array_view<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()),reinterpret_cast<const uint8_t*>(bytes.data())+bytes.size()));
        co_await writer.StoreAsync();
        writer.DetachStream();
        stream.Seek(0);
        Media::Imaging::BitmapImage image;
        co_await image.SetSourceAsync(stream);
        if(loadingArt) loadingArt.Source(image);
    }

    void ShowLoadingArt() {
        std::wstring art;
        for(const wchar_t* name:{L"pic1.png",L"pic0.png",L"icon0.png"}) {
            const auto path=gameFolder+L"\\"+DataFolderName()+L"\\sce_sys\\"+name;
            if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) { art=path; break; }
        }
        if(art.empty()) return;
        loadingArt=Image();
        loadingArt.Stretch(Media::Stretch::UniformToFill);
        loadingArt.HorizontalAlignment(HorizontalAlignment::Center);
        loadingArt.VerticalAlignment(VerticalAlignment::Center);
        rootGrid.Children().Append(loadingArt);
        loadingSince=GetTickCount64();
        LoadArt(art);
    }

    // Ends once the game has shown a few frames. A failed start needs its message seen, and the
    // picture never stays past 3 minutes.
    void UpdateLoadingArt() {
        if(!loadingArt) return;
        UINT presents=0;
        const bool frames=unique194 ? unique194()>=10 : (monitoredSwapchain && SUCCEEDED(monitoredSwapchain->GetLastPresentCount(&presents)) && presents>=10);
        if(!frames && phase.load()!=3 && GetTickCount64()-loadingSince<180000) return;
        uint32_t index=0;
        if(rootGrid.Children().IndexOf(loadingArt,index)) rootGrid.Children().RemoveAt(index);
        loadingArt.Source(nullptr);
        loadingArt=nullptr;
        Log("loading_art_shown_ms="+std::to_string(GetTickCount64()-loadingSince));
    }

    StackPanel info{nullptr};

    TextBlock status{nullptr}, memory{nullptr}, hud{nullptr};

    Button startButton{nullptr};

    DispatcherTimer timer{nullptr};

    std::ofstream memoryLog;

    ULONGLONG lastFlush{}, presentTick{}, rendererProfileTick{};
    com_ptr<IDXGISwapChain> monitoredSwapchain;
    UINT lastPresentCount{};
    double presentFps{};
    bool presentFpsValid{};
    uint64_t uniqueTick194{},uniqueTotal194{},reusedTotal194{};
    double uniqueFps194{};
    bool uniqueValid194{};

    bool started{};
    XboxTestInput testInput{};
    ULONGLONG testInputUntil{}, nextTestInputPoll{};


    std::atomic<bool> closed{false};

    std::atomic<uint32_t> keys{0}, keyboardButtons{0}, keyboardPressed{0};

    std::mutex inputMutex;
    std::mutex textMutex;
    std::atomic<int> textState{0}; // 0 idle, 1 visible/pending, 2 confirmed, 3 cancelled
    std::string textResult;

    void ClearKeyboard() { keys=0; keyboardButtons=0; keyboardPressed=0; }

    fire_and_forget ShowTextDialog(std::string initial, std::string prompt) {
        auto lifetime=get_strong();
        int resultState=3;
        std::string result;
        try {
            ContentDialog dialog;
            dialog.Title(box_value(hstring(prompt.find(" save slot\n")!=std::string::npos ? L"Choose a save slot" : L"Enter text")));
            StackPanel body;
            TextBlock message;
            message.Text(to_hstring(prompt));
            message.TextWrapping(TextWrapping::Wrap);
            ScrollViewer scroll;
            scroll.MaxHeight(320);
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.Content(message);
            body.Children().Append(scroll);
            TextBox input;
            input.Text(to_hstring(initial));
            input.MaxLength(1024);
            input.AcceptsReturn(false);
            input.Margin(Thickness{0,12,0,0});
            body.Children().Append(input);
            dialog.Content(body);
            dialog.PrimaryButtonText(L"Confirm");
            dialog.SecondaryButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary);
            dialog.Opened([input](const auto&,const auto&) {
                input.Focus(FocusState::Programmatic); input.SelectAll();
            });
            if (co_await dialog.ShowAsync()==ContentDialogResult::Primary) {
                result=to_string(input.Text()); resultState=2;
            }
        } catch (const hresult_error& error) {
            Log("text_dialog_error="+std::to_string(error.code().value));
        } catch (...) { Log("text_dialog_error=unexpected"); }
        { std::scoped_lock lock(textMutex); textResult=std::move(result); }
        ClearKeyboard();
        textState=resultState;
    }

    static int TextBegin(void* context,const char* initial,const char* prompt) noexcept {
        auto* self=static_cast<App*>(context);
        int idle=0;
        if (!self->textState.compare_exchange_strong(idle,1)) return 0;
        self->ClearKeyboard();
        try {
            if (!initial || !prompt || strnlen_s(initial,8193)>8192 || strnlen_s(prompt,8193)>8192)
                throw std::runtime_error("Invalid text dialog input");
            const std::string first(initial),label(prompt);
            auto lifetime=self->get_strong();
            self->gamePanel.Dispatcher().RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
                [lifetime,first,label] { lifetime->ShowTextDialog(first,label); });
            return 1;
        } catch (...) { self->textState=0; return 0; }
    }

    static int TextPoll(void* context,char* output,uint64_t capacity) noexcept {
        auto* self=static_cast<App*>(context);
        const int state=self->textState.load();
        if (state<2) return 0;
        int result=2;
        { std::scoped_lock lock(self->textMutex);
            if (output && capacity && state==2 && self->textResult.size()<capacity) {
                memcpy(output,self->textResult.c_str(),self->textResult.size()+1); result=1;
            } else if (output && capacity) output[0]=0;
            self->textResult.clear();
        }
        self->ClearKeyboard(); self->textState=0;
        return result;
    }


    BbUwpPad pad{0,128,128,128,128,0,0,1,0};

    BbUwpHost host{};
    // Bloodborne's engine: version 2 of the host table, with the text dialog in it.
    struct BbUwpHost2 {
        BbUwpHost base;
        int (*text_begin)(void*,const char*,const char*,uint32_t);
        int (*text_poll)(void*,char*,uint64_t);
        void (*text_cancel)(void*);
    } host2{};
    static int TextBegin2(void* context,const char* initial,const char* prompt,uint32_t) noexcept { return TextBegin(context,initial,prompt); }
    static void TextCancel2(void*) noexcept {}

    static void Extent(void*, uint32_t* width, uint32_t* height) { *width = 1920; *height = 1080; }

    static int Closed(void* context) { return static_cast<App*>(context)->closed ? 1 : 0; }

    static int32_t Attach(void* context, void* raw) noexcept {

        auto* self = static_cast<App*>(context);

        try {

            com_ptr<IUnknown> unknown;

            if (raw) unknown.copy_from(static_cast<IUnknown*>(raw));

            com_ptr<IDXGISwapChain> chain = unknown ? unknown.as<IDXGISwapChain>() : nullptr;

            const auto assign = [self, chain] {

                check_hresult(self->gamePanel.as<ISwapChainPanelNative>()->SetSwapChain(chain.get()));
                self->monitoredSwapchain=chain;
                self->presentTick=0; self->presentFpsValid=false;

                Log(chain ? "uwp_swapchain_attached=true" : "uwp_swapchain_detached=true");

            };

            auto dispatcher = self->gamePanel.Dispatcher();

            if (dispatcher.HasThreadAccess()) assign();

            else dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, assign).get();

            return 0;

        } catch (const hresult_error& error) {

            Log("swapchain_attach_error=" + std::to_string(error.code().value) + "; " + to_string(error.message()));

            return error.code().value;

        } catch (...) { return E_FAIL; }

    }

    static void Input(void* context, BbUwpPad* output) {

        auto* self = static_cast<App*>(context);

        if (self->textState.load()!=0) { *output=BbUwpPad{0,128,128,128,128,0,0,1,0}; return; }
        { std::scoped_lock lock(self->inputMutex); *output = self->pad; }

        const uint32_t held = self->keys.load();

        // Preserve a real key-down until at least one guest pad sample sees it.

        // Otherwise a short press can begin and end between 30 Hz game polls.

        output->buttons |= self->keyboardButtons.load() | self->keyboardPressed.exchange(0);

        const auto axis = [held](uint32_t negative, uint32_t positive, uint8_t old) {

            const bool n = (held & negative) != 0, p = (held & positive) != 0;

            return n || p ? uint8_t(n == p ? 128 : n ? 0 : 255) : old;

        };

        output->left_x = axis(4,8,output->left_x); output->left_y = axis(1,2,output->left_y);

        output->right_x = axis(64,128,output->right_x); output->right_y = axis(16,32,output->right_y);

        if (output->buttons & 0x100) output->l2 = 255;

        if (output->buttons & 0x200) output->r2 = 255;

    }

    void Key(Windows::System::VirtualKey key, bool down) {

        if (textState.load()!=0) return;
        using K = Windows::System::VirtualKey;

        uint32_t button = 0, movement = 0;

        switch (key) {

        case K::Space: button=0x4000; break; case K::Shift: case K::LeftShift: button=0x2000; break;

        case K::E: button=0x8000; break; case K::V: button=0x1000; break; case K::Enter: button=8; break;

        case K::Number1: button=0x400; break; case K::Number3: button=0x800; break;

        case K::R: button=0x100; break; case K::F: button=0x200; break;

        case K::Z: button=2; break; case K::Q: case K::C: button=4; break;

        case K::I: button=16; break; case K::K: button=64; break; case K::J: button=128; break; case K::L: button=32; break;

        case K::Tab: button=0x100000; break;

        case K::W: movement=1; break; case K::S: movement=2; break; case K::A: movement=4; break; case K::D: movement=8; break;

        case K::Up: movement=16; break; case K::Down: movement=32; break; case K::Left: movement=64; break; case K::Right: movement=128; break;

        default: break;

        }

        if (down) { keyboardButtons.fetch_or(button); keyboardPressed.fetch_or(button); keys.fetch_or(movement); }

        else { keyboardButtons.fetch_and(~button); keys.fetch_and(~movement); }

    }

    void PollController() {

        BbUwpPad next{0,128,128,128,128,0,0,1,0};

        const auto controllers = Gamepad::Gamepads();

        static uint32_t previousCount=UINT32_MAX;
        const auto controllerCount=controllers.Size();
        if (controllerCount!=previousCount) {
            previousCount=controllerCount;
            Log("gamepad_count="+std::to_string(controllerCount));
        }

        if (controllers.Size()) {

            const auto reading = controllers.GetAt(0).GetCurrentReading();

            // Bounded evidence that physical gamepad buttons reach the host.
            static uint32_t previousButtons=0;
            static unsigned buttonReports=0;
            const auto physicalButtons=static_cast<uint32_t>(reading.Buttons);
            if (physicalButtons!=previousButtons) {
                previousButtons=physicalButtons;
                if (buttonReports<16) {
                    ++buttonReports;
                    Log("gamepad_buttons="+std::to_string(physicalButtons));
                }
            }

            const auto map = [&](GamepadButtons button, uint32_t ps4) { if ((reading.Buttons & button) != GamepadButtons::None) next.buttons |= ps4; };

            map(GamepadButtons::A,0x4000); map(GamepadButtons::B,0x2000); map(GamepadButtons::X,0x8000); map(GamepadButtons::Y,0x1000);

            map(GamepadButtons::Menu,8); map(GamepadButtons::View,0x100000);

            map(GamepadButtons::LeftShoulder,0x400); map(GamepadButtons::RightShoulder,0x800);

            map(GamepadButtons::LeftThumbstick,2); map(GamepadButtons::RightThumbstick,4);

            map(GamepadButtons::DPadUp,16); map(GamepadButtons::DPadRight,32); map(GamepadButtons::DPadDown,64); map(GamepadButtons::DPadLeft,128);

            const auto axis = [](double value) { return static_cast<uint8_t>(std::clamp(std::lround(128 + value * 127),0L,255L)); };

            next.left_x=axis(reading.LeftThumbstickX); next.left_y=axis(-reading.LeftThumbstickY);

            next.right_x=axis(reading.RightThumbstickX); next.right_y=axis(-reading.RightThumbstickY);

            next.l2=static_cast<uint8_t>(reading.LeftTrigger*255); next.r2=static_cast<uint8_t>(reading.RightTrigger*255);

            if (next.l2 > 128) next.buttons|=0x100;

            if (next.r2 > 128) next.buttons|=0x200;

        }

        const auto now=GetTickCount64();
        if (now>=nextTestInputPoll) {
            nextTestInputPoll=now+100;
            std::ifstream developer(localPath+L"\\developer-input.txt");
            char text[256]{};
            if (developer.getline(text,sizeof(text))) {
                XboxTestInput request{};
                if (ParseXboxTestInput(text,testInput.nonce,request)) {
                    testInput=request; testInputUntil=now+request.duration;
                    developer.close(); // Release the read handle before consuming the command.
                    DeleteFileW((localPath+L"\\developer-input.txt").c_str());
                    Log("developer_input_nonce="+std::to_string(request.nonce)+"; duration="+std::to_string(request.duration)+"; buttons="+std::to_string(request.buttons));
                }
            }
        }
        if (now<testInputUntil) {
            next.buttons|=testInput.buttons;
            if(testInput.axes[0]!=128)next.left_x=testInput.axes[0];
            if(testInput.axes[1]!=128)next.left_y=testInput.axes[1];
            if(testInput.axes[2]!=128)next.right_x=testInput.axes[2];
            if(testInput.axes[3]!=128)next.right_y=testInput.axes[3];
            next.l2=std::max(next.l2,testInput.axes[4]);
            next.r2=std::max(next.r2,testInput.axes[5]);
        }
        std::scoped_lock lock(inputMutex); pad=next;

    }

    void Start(bool cpuOnly=false) {

        if (started || phase==3) return;

        started=true; startButton.IsEnabled(false);

        status.Text(L"Starting...");

        info.Visibility(Visibility::Collapsed); hud.Visibility(Visibility::Visible);

        std::thread([this,cpuOnly] {

            try {

                init_apartment(apartment_type::multi_threaded);

                const auto installed=Windows::ApplicationModel::Package::Current().InstalledLocation().Path();

                (void)installed;
                const auto local=to_string(localPath),package=to_string(gameFolder);

                using Play=int(__cdecl*)(const char*,const char*,const BbUwpHost*,int);

                const auto play=reinterpret_cast<Play>(GetProcAddress(runtimeModule,bbEngine ? "bb_uwp_run_game" : "tlou_uwp_run_game"));

                if (!play) { loadError=ERROR_PROC_NOT_FOUND; phase=3; return; }

                phase=4; Log("game_entry_called=true; cpu_only="+std::to_string(cpuOnly));

                loadTrace194=CreateFile2((localPath+L"\\native-crash194.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,CREATE_ALWAYS,nullptr);
                loadTraceCount194=0;
                auto addCrash194=(PVOID(WINAPI*)(ULONG,PVECTORED_EXCEPTION_HANDLER))GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"AddVectoredExceptionHandler");
                if(addCrash194)addCrash194(0,LoadTrace194);
                nativeResult=play(local.c_str(),package.c_str(),bbEngine ? &host2.base : &host,cpuOnly ? 1 : 0);

                Log("game_entry_return="+std::to_string(nativeResult.load()));

                phase=nativeResult==0 ? 2 : 3;

            } catch (const hresult_error& error) { loadError=static_cast<DWORD>(error.code().value); Log(to_string(error.message())); phase=3; }

              catch (...) { Log("worker_exception=unknown"); phase=3; }

        }).detach();

    }

    void OnLaunched(const Windows::ApplicationModel::Activation::LaunchActivatedEventArgs& arguments) {

        // Xbox otherwise defaults to emulating a mouse with the gamepad.
        RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
        Log("gamepad_pointer_mode=WhenRequested");

        const auto version=Windows::ApplicationModel::Package::Current().Id().Version();

        Log("uwp_activation=true; integration_version="+std::to_string(version.Major)+"."+

            std::to_string(version.Minor)+"."+std::to_string(version.Build)+"."+std::to_string(version.Revision));

        try {

            const auto view=Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();

            view.IsScreenCaptureEnabled(true);

            Log("screen_capture_enabled="+std::to_string(view.IsScreenCaptureEnabled()));

        } catch (const hresult_error& error) {

            Log("screen_capture_setting_error="+std::to_string(error.code().value));

        }

        HANDLE token{}; DWORD container{}, returned{};

        if (OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {

            if (GetTokenInformation(token,TokenIsAppContainer,&container,sizeof(container),&returned)) Log("app_container=" + std::to_string(container));

            CloseHandle(token);

        }

        host={sizeof(BbUwpHost),1,this,Extent,Attach,Input,Closed};
        host2.base=host; host2.base.size=sizeof(BbUwpHost2); host2.base.version=2;
        host2.text_begin=TextBegin2; host2.text_poll=TextPoll; host2.text_cancel=TextCancel2;

        Grid root; root.Background(Media::SolidColorBrush(Windows::UI::Colors::Black()));

        gamePanel=SwapChainPanel(); gamePanel.HorizontalAlignment(HorizontalAlignment::Center); gamePanel.VerticalAlignment(VerticalAlignment::Center);

        // The game presents a fixed 1080p buffer. Scale the whole composition

        // visual so smaller windows show the complete image instead of cropping.

        gamePanel.Width(1920); gamePanel.Height(1080);

        Viewbox viewport; viewport.Stretch(Media::Stretch::Uniform);

        viewport.Child(gamePanel); root.Children().Append(viewport);

        info=StackPanel(); info.Padding(Thickness{40}); info.Spacing(16); info.VerticalAlignment(VerticalAlignment::Top);

        info.Background(Media::SolidColorBrush(Windows::UI::Color{255,16,24,39}));

        const auto text=[this](const wchar_t* value,double size) {

            TextBlock block; block.Text(value); block.FontSize(size); block.TextWrapping(TextWrapping::Wrap);

            block.Foreground(Media::SolidColorBrush(Windows::UI::Colors::White())); info.Children().Append(block); return block;

        };

        text(L"XStation",32);

        status=text(L"Ready",22);

        text(L"Development build — uses your locally staged game files and a separate save copy.",18);

        text(L"A / Space confirms · Menu / Enter opens the game menu · Left stick / WASD moves · Right stick / arrows controls the camera",16);

        startButton=Button(); startButton.Foreground(Media::SolidColorBrush(Windows::UI::Colors::White())); startButton.Background(Media::SolidColorBrush(Windows::UI::Color{255,42,80,59})); startButton.Content(box_value(L"Start")); startButton.Click([this](const auto&,const auto&) { Start(); });

        const auto installed=Windows::ApplicationModel::Package::Current().InstalledLocation().Path();

        (void)installed;
        const auto boot=gameFolder+L"\\"+BootFolderName()+L"\\boot-linked.bin";

        const bool available=GetFileAttributesW(boot.c_str())!=INVALID_FILE_ATTRIBUTES;

        startButton.IsEnabled(available && phase!=3);

        if (!available) text(L"This game has no prepared image.",16);

        info.Children().Append(startButton);

        memory=text(L"Reading app memory…",16); root.Children().Append(info);

        hud=TextBlock(); hud.FontSize(16); hud.Margin(Thickness{12}); hud.VerticalAlignment(VerticalAlignment::Top);

        hud.Foreground(Media::SolidColorBrush(Windows::UI::Colors::White())); hud.Visibility(Visibility::Collapsed); root.Children().Append(hud);

        rootGrid=root;
        if (available) ShowLoadingArt();

        using SetTextHost=int(__cdecl*)(const TlouTextInputHost*);
        if (runtimeModule && !bbEngine) {
            const auto setText=reinterpret_cast<SetTextHost>(GetProcAddress(runtimeModule,"tlou_uwp_set_text_input_host"));
            const TlouTextInputHost textHost{sizeof(TlouTextInputHost),1,this,TextBegin,TextPoll};
            if (!setText || setText(&textHost)!=0) throw hresult_error(E_FAIL,L"UWP text dialog registration failed");
        }
        auto window=Window::Current(); window.Content(root);

        window.CoreWindow().KeyDown([this](const auto&,const Windows::UI::Core::KeyEventArgs& e){ Key(e.VirtualKey(),true); });

        window.CoreWindow().KeyUp([this](const auto&,const Windows::UI::Core::KeyEventArgs& e){ Key(e.VirtualKey(),false); });

        window.Dispatcher().AcceleratorKeyActivated([this](const auto&,const Windows::UI::Core::AcceleratorKeyEventArgs& e){

            using Type=Windows::UI::Core::CoreAcceleratorKeyEventType;

            if (e.EventType()==Type::KeyDown || e.EventType()==Type::SystemKeyDown) Key(e.VirtualKey(),true);

            else if (e.EventType()==Type::KeyUp || e.EventType()==Type::SystemKeyUp) Key(e.VirtualKey(),false);

        });

        window.CoreWindow().Closed([this](const auto&,const auto&){ closed=true; });

        window.CoreWindow().Activated([this](const auto&,const Windows::UI::Core::WindowActivatedEventArgs& e){

            if (e.WindowActivationState()==Windows::UI::Core::CoreWindowActivationState::Deactivated) { keys=0; keyboardButtons=0; keyboardPressed=0; }

        });

        // Physical B is polled separately as PS4 Circle. Consume UWP's system
        // Back request so the shell cannot navigate away from this game.
        Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
            [this](const auto&, const Windows::UI::Core::BackRequestedEventArgs& e) {
                e.Handled(true);
                static unsigned logged=0;
                if(logged++<8)Log("system_back_handled=true; guest_circle_mapping_retained=true");
            });
        Log("system_back_handler_installed=true");

        window.Activate();

        memoryLog.open(localPath+L"\\app-memory.csv",std::ios::trunc); memoryLog<<"tick_ms,app_usage_bytes,app_limit_bytes,phase,private_commit_bytes,shared_commit_bytes,peak_private_commit_bytes,presentation_fps,presentation_fps_valid,unique_game_fps,unique_game_fps_valid,unique_frames_total,unique_sample_tick_ms,reused_presents_total\n";

        timer=DispatcherTimer(); timer.Interval(std::chrono::milliseconds(50));

        timer.Tick([this](const auto&,const auto&){ Tick(); }); timer.Start();

        std::ifstream startup(localPath+L"\\startup-mode.txt");

        std::string requestedMode;

        std::getline(startup,requestedMode);

        startup.close();

        if (!requestedMode.empty()) {

            DeleteFileW((localPath+L"\\startup-mode.txt").c_str());

            Log("startup_mode="+requestedMode);

        }

        if (available && requestedMode!="boot-check") Start(); // XStation: Play was pressed in the dashboard

        else if ((arguments.Arguments()==L"--boot-check" || requestedMode=="boot-check") && available) Start(true);

    }

    void DumpRendererProfile() {
        if (!dumpRendererProfile || !runtimeModule) return;
        using Snapshot=uint32_t(__cdecl*)(TlouXboxProfileSnapshot*,uint32_t);
        const auto snapshot=reinterpret_cast<Snapshot>(GetProcAddress(runtimeModule,"tlou_xbox_profile_snapshot"));
        if (!snapshot) {Log("renderer_profile_export_missing=true");dumpRendererProfile=false;return;}
        TlouXboxProfileSnapshot data{};
        const auto result=snapshot(&data,sizeof(data));
        if (result) {Log("renderer_profile_error="+std::to_string(result));return;}
        const char* lanes[]={"DrawRec","VkRecorder","Completion"};
        const char* phases[]={"Other","RenderState","Resources","Buffers","Textures","Samplers","VertexInputs","IndexInputs","BeginRendering","ResetBindings","Descriptors","DescriptorAllocate","DescriptorUpdate","PendingOperations","RecorderKick","RecorderJoin","HostCopies","DeferredSignals","Submit","GpuWait","DriverCommand"};
        std::ofstream out(localPath+L"\\xbox-renderer-profile.csv",std::ios::trunc);
        out<<"kind,lane,phase,segments,wall_ns,cycles,max_ns\n";
        out<<"metadata,tick_ms,"<<data.tick_ms<<",0,0,0,0\n";
        out<<"metadata,cycle_failures,"<<data.cycle_failures<<",0,0,0,0\n";
        out<<"metadata,site_overflow,"<<data.site_overflow<<",0,0,0,0\n";
        for(unsigned l=0;l<3;++l) {
            out<<"root,"<<lanes[l]<<",seen,"<<data.seen[l]<<','<<data.root_ns[l]<<','<<data.selected[l]<<",0\n";
            for(unsigned ph=0;ph<21;++ph) {const auto& c=data.time[l][ph];out<<"phase,"<<lanes[l]<<','<<phases[ph]<<','<<c.segments<<','<<c.wall_ns<<','<<c.cycles<<','<<c.max_ns<<'\n';}
        }
        for(const auto& site:data.sites) {
            if(!site.name[0])continue;
            std::string name(site.name);std::replace(name.begin(),name.end(),',',';');std::replace(name.begin(),name.end(),'\n',' ');std::replace(name.begin(),name.end(),'\r',' ');
            const auto& c=site.counter;out<<"site,VkRecorder,"<<name<<','<<c.segments<<','<<c.wall_ns<<','<<c.cycles<<','<<c.max_ns<<'\n';
        }
    }

    void LogResourceGrant() {
        // Read-only queries: never release or change the app's CPU assignment.
        HMODULE module=LoadLibraryExW(L"api-ms-win-gaming-expandedresources-l1-1-0.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module)module=LoadLibraryExW(L"gamemode.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module) {Log("expanded_resource_query_load_error="+std::to_string(GetLastError()));return;}
        using Has=HRESULT(WINAPI*)(BOOL*);using Count=HRESULT(WINAPI*)(ULONG*);
        const auto has=reinterpret_cast<Has>(GetProcAddress(module,"HasExpandedResources"));
        const auto count=reinterpret_cast<Count>(GetProcAddress(module,"GetExpandedResourceExclusiveCpuCount"));
        BOOL expanded=FALSE;ULONG cpus=0;
        const auto hr=has ? has(&expanded) : E_NOINTERFACE;
        const auto cr=count ? count(&cpus) : E_NOINTERFACE;
        Log("expanded_resources_hr="+std::to_string(hr)+"; granted="+std::to_string(expanded)+"; exclusive_cpu_hr="+std::to_string(cr)+"; exclusive_cpus="+std::to_string(cpus));
        FreeLibrary(module);
    }

    void Tick() {
        UpdateLoadingArt();
        {
            static ULONGLONG heldSince=0,lastStats=0;
            const ULONGLONG tick=GetTickCount64();
            const auto pads=Gamepad::Gamepads();
            const uint32_t both=static_cast<uint32_t>(GamepadButtons::View)|static_cast<uint32_t>(GamepadButtons::Menu);
            const bool held=pads.Size()&&(static_cast<uint32_t>(pads.GetAt(0).GetCurrentReading().Buttons)&both)==both;
            if(!held) heldSince=0;
            else if(!heldSince) heldSince=tick;
            else if(tick-heldSince>=3000){ Log("xstation_return_to_dashboard=true"); heldSince=0; ReturnToDashboard(); return; }
            if(tick-lastStats>=60000){ lastStats=tick; WriteStats(); }
            // Two seconds into the game, so this handler comes after the runtime's crash trace.
            static ULONGLONG gameSince=0,devCrashAt=0;
            static bool endHandlerAdded=false;
            if(!gameSince&&phase.load()==4) gameSince=tick;
            if(gameSince&&!endHandlerAdded&&tick-gameSince>=2000){
                endHandlerAdded=true;
                const auto add=reinterpret_cast<PVOID(WINAPI*)(ULONG,PVECTORED_EXCEPTION_HANDLER)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"AddVectoredExceptionHandler"));
                if(add) add(0,EndOnFatal);
                std::ifstream keep(localPath+L"\\dev-keep-crashed.txt");
                keepCrashedProcess=keep.is_open();
                std::ifstream crash(localPath+L"\\dev-heap-crash.txt");
                unsigned seconds=0;
                if(crash>>seconds){ crash.close(); DeleteFileW((localPath+L"\\dev-heap-crash.txt").c_str()); devCrashAt=tick+seconds*1000ull; Log("dev_heap_crash_in="+std::to_string(seconds)); }
            }
            if(devCrashAt&&tick>=devCrashAt){ devCrashAt=0; DevHeapCrash(); }
        }

        // 40 MiB under the app's current limit (5 GiB for a UWP game; higher when the package is
        // in debug mode, which lifts the limit), rather than a fixed 5080 MiB.
        {
            const auto limit=Windows::System::MemoryManager::AppMemoryUsageLimit();
            static uint64_t loggedLimit=0;
            if(limit!=loggedLimit){ loggedLimit=limit; Log("app_memory_limit_MiB="+std::to_string(limit>>20)); }
            const uint64_t guard=limit>80ull*1024*1024 ? limit-40ull*1024*1024 : limit;
            // LocalState\memory-guard-off.txt = 1: no guard (tests of the limit itself, e.g. the
            // package in debug mode); the system then decides what happens past the limit.
            static const bool guardOff=[]{ std::ifstream f(localPath+L"\\memory-guard-off.txt"); unsigned v=0; return bool(f>>v) && v==1; }();
            static bool loggedOff=false;
            if(guardOff && !loggedOff){ loggedOff=true; Log("memory_guard_off=true"); }
            const uint64_t used=Windows::System::MemoryManager::AppMemoryUsage();
            if(!guardOff && used>=guard) {
                Log("uwp194_memory_guard="+std::to_string(guard>>20)+"MiB=true");memoryLog.flush();
                // XStation (2026-11-13): the dashboard shows this reason at the next launch.
                { std::ofstream why(localPath+L"\\last-exit-reason.txt",std::ios::trunc); why<<"memory_guard "<<(used>>20)<<' '<<(limit>>20)<<'\n'; }
                Application::Current().Exit();return;
            }
        }
        PollController();

        const auto now=GetTickCount64(), usage=Windows::System::MemoryManager::AppMemoryUsage(), limit=Windows::System::MemoryManager::AppMemoryUsageLimit();

        const auto report=Windows::System::MemoryManager::GetAppMemoryReport();

        const auto privateBytes=report.PrivateCommitUsage(), totalBytes=report.TotalCommitUsage();
        if(dumpRendererProfile && now-rendererProfileTick>=5000) {
            rendererProfileTick=now; DumpRendererProfile(); LogResourceGrant();
        }
        if (!presentTick || now-presentTick>=1000) {
            UINT count{};
            if (monitoredSwapchain && SUCCEEDED(monitoredSwapchain->GetLastPresentCount(&count))) {
                presentFpsValid=presentTick!=0;
                if (presentFpsValid) presentFps=double(UINT(count-lastPresentCount))*1000.0/double(now-presentTick);
                lastPresentCount=count; presentTick=now;
            } else { presentTick=0; presentFpsValid=false; }
        }

        if(unique194 && (!uniqueTick194 || now-uniqueTick194>=1000)) {
            const uint64_t total=unique194();
            uniqueValid194=uniqueTick194!=0 && total>=uniqueTotal194;
            if(uniqueValid194) uniqueFps194=double(total-uniqueTotal194)*1000.0/double(now-uniqueTick194);
            uniqueTotal194=total;uniqueTick194=now;reusedTotal194=reused194?reused194():0;
        }
        memoryLog<<now<<','<<usage<<','<<limit<<','<<phase.load()<<','<<privateBytes<<','

                 <<(totalBytes>=privateBytes ? totalBytes-privateBytes : 0)<<','<<report.PeakPrivateCommitUsage()<<','<<presentFps<<','<<int(presentFpsValid)<<','<<uniqueFps194<<','<<int(uniqueValid194)<<','<<uniqueTotal194<<','<<uniqueTick194<<','<<reusedTotal194<<'\n';

        if (now-lastFlush>=1000) {

            memoryLog.flush(); lastFlush=now;
            tlou_native_cache_poll();
            if(ownershipReport1562 && ownershipSamples1562<180){
                Log("ownership_sample_tick="+std::to_string(now)+"; sample="+std::to_string(++ownershipSamples1562));
                ownershipReport1562();
            }

            const auto label=L"Memory: "+to_hstring(usage/1048576)+L" MiB"+
                (presentFpsValid ? L" | FPS: "+to_hstring(static_cast<int32_t>(std::lround(presentFps))) : L"");

            uint64_t warmup[10]{};
            std::wstring shaderLabel;
            if(shaderWarmupStatus && shaderWarmupStatus(warmup,10)==1){
                if(warmup[0]==1)shaderLabel=L" | Preparing shaders: "+std::to_wstring(warmup[2])+L" / "+std::to_wstring(warmup[1]);
                static bool loggedWarmup=false;
                if(warmup[0]==2&&!loggedWarmup){
                    loggedWarmup=true;Log("shader_warmup_complete=true; graphics="+std::to_string(warmup[3])+"; compute="+std::to_string(warmup[4])+"; skipped="+std::to_string(warmup[5])+"; deferred="+std::to_string(warmup[6])+"; elapsed_ms="+std::to_string(warmup[7])+"; reason="+std::to_string(warmup[8]));
                }
            }
            memory.Text(label); hud.Text(label+shaderLabel);

            if (phase==2) status.Text(L"Native runtime returned successfully.");

            if (phase==3) { info.Visibility(Visibility::Visible); status.Text(L"Startup stopped. Native result: "+to_hstring(nativeResult.load())+L"; loader error: "+to_hstring(static_cast<uint32_t>(loadError.load()))+L". See the app-local logs."); }

        }

    }

};

}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        init_apartment(apartment_type::multi_threaded);
        const std::wstring root=ApplicationData::Current().LocalFolder().Path().c_str();
        const std::wstring installed=Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();
        localPath=root;
        // XStation (2026-11-09): files on a USB drive open only through the FromApp calls; every plain file call in
        // this process that is denied is tried again that way (usb_files.cpp).
        const unsigned usbSlots=XStationUsbFilesInstall();
        // XStation: LocalState\play.txt names the game to run; it is removed first, so a crash in
        // the game returns to the dashboard at the next launch.
        std::wifstream request(root+L"\\play.txt");
        std::getline(request,titleId);
        request.close();
        DeleteFileW((root+L"\\play.txt").c_str());
        while(!titleId.empty()&&(titleId.back()==L'\r'||titleId.back()==L' ')) titleId.pop_back();
        for(const wchar_t c:titleId) if(!iswalnum(c)) titleId.clear();
        if(titleId.empty()){
            CreateDirectoryW((root+L"\\Import").c_str(),nullptr);
            CloseEarlierProcesses(root);
            RunXStationDashboard(root,installed);
            return 0;
        }
        CreateDirectoryW((root+L"\\Titles").c_str(),nullptr);
        localPath=root+L"\\Titles\\"+titleId;
        CreateDirectoryW(localPath.c_str(),nullptr);
        // XStation (2026-11-13): the dashboard reads these at the next launch to say why the game closed.
        DeleteFileW((localPath+L"\\last-exit-reason.txt").c_str());
        { std::wofstream last(root+L"\\last-game.txt",std::ios::trunc); last<<titleId; }
        CreateDirectoryW((localPath+L"\\tlou-bb194-user").c_str(),nullptr);
        CloseEarlierProcesses(root);
        enginePath=L"Engines\\"+titleId;
        bbEngine=GetFileAttributesW((installed+L"\\"+enginePath+L"\\bb-uwp-runtime.dll").c_str())!=INVALID_FILE_ATTRIBUTES;
        const std::wstring bootName=BootFolderName();
        gameFolder=installed+L"\\Games\\"+titleId;
        // XStation (2026-11-09): a mounted game (mounts.txt, written by the dashboard) runs from its USB folder.
        bool mountedGame=false;
        {
            std::ifstream mounts(root+L"\\mounts.txt"); // UTF-8 (titles can have symbols)
            std::string text;
            while(std::getline(mounts,text)){
                const std::wstring line=to_hstring(text).c_str();
                const size_t tab=line.find(L'\t');
                if(tab==std::wstring::npos || line.substr(0,tab)!=titleId) continue;
                std::wstring folder=line.substr(tab+1);
                folder=folder.substr(0,folder.find(L'\t')); // then the title, for the dashboard
                while(!folder.empty()&&(folder.back()==L'\r'||folder.back()==L' ')) folder.pop_back();
                if(!folder.empty() && GetFileAttributesW((folder+L"\\"+bootName).c_str())!=INVALID_FILE_ATTRIBUTES){ gameFolder=folder; mountedGame=true; }
                break;
            }
        }
        if(mountedGame || GetFileAttributesW((gameFolder+L"\\"+bootName).c_str())==INVALID_FILE_ATTRIBUTES){
            if(!mountedGame){
                gameFolder=L"D:\\DevelopmentFiles\\XStation\\Games\\"+titleId;
                if(GetFileAttributesW((gameFolder+L"\\"+bootName).c_str())==INVALID_FILE_ATTRIBUTES) gameFolder=root+L"\\Games\\"+titleId;
            }
            // The Vulkan loader reads the driver's location from a file in the game folder (the engine sets
            // VK_DRIVER_FILES to it), on a USB drive too: a 190-byte file in the game's own XStation folder.
            std::string dll=to_string(installed+L"\\"+enginePath+L"\\vulkan_dzn.dll"),json;
            for(const char c:dll){ if(c=='\\') json+="\\\\"; else json+=c; }
            std::ofstream icd(gameFolder+L"\\dzn_icd.x86_64.json",std::ios::trunc);
            icd<<"{\n  \"file_format_version\": \"1.0.0\",\n  \"ICD\": {\n    \"library_path\": \""<<json<<"\",\n    \"api_version\": \"1.1.354\"\n  }\n}\n";
        }
        // First run of this game: the engine's tested settings (never over the user's own).
        const std::wstring defaults=installed+L"\\"+enginePath+L"\\defaults";
        COPYFILE2_EXTENDED_PARAMETERS keepExisting{sizeof(keepExisting)};
        keepExisting.dwCopyFlags=COPY_FILE_FAIL_IF_EXISTS;
        CopyFile2((defaults+L"\\bb-env.txt").c_str(),(localPath+L"\\tlou-bb194-user\\bb-env.txt").c_str(),&keepExisting);
        // XStation (2026-11-08): every other file there too (The Last of Us has about 40 switch files,
        // Bloodborne its layout and ini, God of War draw-pipeline-off.txt).
        {
            WIN32_FIND_DATAW found{};
            const HANDLE find=FindFirstFileExW((defaults+L"\\*").c_str(),FindExInfoBasic,&found,FindExSearchNameMatch,nullptr,0);
            if(find!=INVALID_HANDLE_VALUE){
                do {
                    if((found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || !_wcsicmp(found.cFileName,L"bb-env.txt")) continue;
                    CopyFile2((defaults+L"\\"+found.cFileName).c_str(),(localPath+L"\\"+found.cFileName).c_str(),&keepExisting);
                } while(FindNextFileW(find,&found));
                FindClose(find);
            }
        }
        {
            std::ifstream stats(localPath+L"\\stats.txt");
            stats>>statsSeconds;
        }
        statsBegan=GetTickCount64();
        WriteStats();
        Log("host_entry=true; xstation_title="+to_string(titleId)+"; game_folder="+to_string(gameFolder)+(mountedGame?"; mounted=usb":"")+"; usb_fallback_slots="+std::to_string(usbSlots)); InitializeRuntime();
        Application::Start([](const auto&){ make<App>(); });
        return 0;
    } catch (const hresult_error& error) { Log("startup_error="+std::to_string(error.code().value)+"; "+to_string(error.message())); return error.code().value; }
}

// XStation (2026-11-07): the running game's folder (Titles\<id>), where native_cache_bridge.cpp reads
// native-compiled-cache-mode.txt and keeps its cache, so each game has its own switch and cache.
std::wstring XStationTitleFolder() { return localPath; }
