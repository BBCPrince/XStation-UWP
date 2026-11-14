/* SPDX-License-Identifier: GPL-2.0-or-later
 * v1.9.4 renderer in the existing TLOU UWP host ABI. No old renderer objects. */
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <errno.h>
#include "uwp_host.h"
#include "runtime_guarded_image.h"
int bb_uwp_runtime_main(int,char**);
void schedule_gpu_trace194(const char*);
void register_gpu_thread194(const char*);
uint32_t runtime_thread_tcb_offset(void);
static volatile LONG started;
__declspec(dllexport) uint32_t __cdecl tlou_uwp_initialize(void){return runtime_thread_tcb_offset();}
__declspec(dllexport) uint32_t __cdecl tlou_uwp_abi_version(void){return 1;}
__declspec(dllexport) const char* __cdecl tlou_uwp_identity(void){return "TLOU on Bloodborne windows-v1.9.4 / 3fabfa7a; UWP platform bridge; GPL-2.0-or-later";}
static int set(const char*n,const char*v){return !_putenv_s(n,v)&&SetEnvironmentVariableA(n,v);}
/* GPU hang reports (2026-11-02): the swapchain handed to the host leads to Dozen's own D3D12 device. */
void bbport_d3d12_hold_swapchain(void*);
static int32_t(*host_attach194)(void*,void*);
static int32_t attach194(void*context,void*swapchain){bbport_d3d12_hold_swapchain(swapchain);return host_attach194(context,swapchain);}
static int path(char*out,size_t n,const char*a,const char*b){int k=snprintf(out,n,"%s\\%s",a,b);return k>=0&&(size_t)k<n;}
extern uint64_t runtime_disabled_optimizations;
static int switch194(const char* local,const char* name){
 char file[32768];if(!path(file,sizeof(file),local,name))return -1;
 FILE* f=fopen(file,"r");if(!f)return errno==ENOENT?0:-1;
 int value=-1;char tail;int n=fscanf(f,"%d %c",&value,&tail);fclose(f);
 return n==1&&(value==0||value==1)?value:-1;
}
static HANDLE fatal194=INVALID_HANDLE_VALUE;
static LONG CALLBACK fatal_trace194(EXCEPTION_POINTERS* e){
 if(fatal194!=INVALID_HANDLE_VALUE){
  char line[1024],module[512]={0};HMODULE m=0;DWORD wrote;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)e->ExceptionRecord->ExceptionAddress,&m);
  if(m)GetModuleFileNameA(m,module,sizeof(module));
  uintptr_t* st=(uintptr_t*)e->ContextRecord->Rsp;
  int n=snprintf(line,sizeof(line),"code=%08lx;ip=%p;module=%s;rva=%llx;info=%llx,%llx;stack=%llx,%llx,%llx,%llx\n",e->ExceptionRecord->ExceptionCode,e->ExceptionRecord->ExceptionAddress,module,(unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress-(uintptr_t)m),(unsigned long long)e->ExceptionRecord->ExceptionInformation[0],(unsigned long long)e->ExceptionRecord->ExceptionInformation[1],(unsigned long long)st[0],(unsigned long long)st[1],(unsigned long long)st[2],(unsigned long long)st[3]);
  if(n>0&&n<sizeof(line)){WriteFile(fatal194,line,n,&wrote,0);FlushFileBuffers(fatal194);}
 }
 return EXCEPTION_CONTINUE_SEARCH;
}
typedef struct{int argc;char**argv;int result;} Start;
static DWORD WINAPI run(void*p){register_gpu_thread194("guest-main");Start*s=p;HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);if(FAILED(hr)){s->result=(int)hr;return (DWORD)hr;}s->result=bb_uwp_runtime_main(s->argc,s->argv);CoUninitialize();return (DWORD)s->result;}
__declspec(dllexport) int __cdecl tlou_uwp_run_game(const char*local,const char*packaged,const BbUwpHost*host,int cpu_only){
 if(!local||!packaged||!*local||!*packaged||strlen(local)>30000||strlen(packaged)>30000||(cpu_only!=0&&cpu_only!=1))return 87;
 if(!cpu_only&&(!host||host->size!=sizeof(*host)||host->version!=1||!host->context||!host->extent||!host->attach_swapchain||!host->read_input||!host->closed))return 87;
 if(InterlockedCompareExchange(&started,1,0))return 170;
 char output[32768],error[32768],icd[32768],boot[32768],content[32768],game[32768],user[32768],config[32768];
 if(!path(output,sizeof(output),local,"tlou-stdout.log")||!path(error,sizeof(error),local,"tlou-stderr.log")||!path(icd,sizeof(icd),packaged,"dzn_icd.x86_64.json")||!path(boot,sizeof(boot),packaged,"GameBoot\\boot-linked.bin")||!path(content,sizeof(content),packaged,"GameBoot\\content.bin")||!path(game,sizeof(game),packaged,"GameData")||!path(user,sizeof(user),local,"tlou-bb194-user")||!path(config,sizeof(config),local,"bbport.ini"))return 87;
 if(!freopen(output,"w",stdout)||!freopen(error,"w",stderr))return 5;
 setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
 char fatalpath[32768];path(fatalpath,sizeof(fatalpath),local,"native-runtime-crash194.log");
 fatal194=CreateFileA(fatalpath,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
 void* fatalcookie=AddVectoredExceptionHandler(0,fatal_trace194);
 printf("TLOU194: native crash trace installed=%d; handle=%p\n",fatalcookie!=NULL,fatal194);

 if(_mkdir(user)&&errno!=EEXIST)return 5;
 printf("TLOU194 UWP: upstream=3fabfa7a7af67a34a6dfc630b6ec2539b68fad4d; private_profile=%s; hostABI=1; new_gpu_objects=1\n",user);
 /* BB_FPS_LIMIT=30: a flip is presented when it arrives, a 30 FPS period after the previous one,
    instead of on the next tick of the emulated 60 Hz vblank. On those ticks a frame done just
    after one waited 16.7 ms more (50 ms, then 16 ms): 5-10% of courtyard frames; with the limit
    they stay at 33-35 ms (probe fl30, 2026-10-29). */
 if(!set("BB_GPU_USER_DIR",user)||!set("BB_USER_DIR",user)||!set("BB_CONFIG",config)||!set("VK_DRIVER_FILES",icd)||!set("VK_LOADER_LAYERS_DISABLE","~implicit~")||!set("BB_UPSCALER","off")||!set("BB_OBJECT_MOTION","0")||!set("BB_ONLINE","0")||!set("BB_FPS_LIMIT","30")||!set("BB_READBACKS","2")||!set("BB_GUEST_COMMIT_ON_MAP","1")||!set("BB_TLOU_WORKER_FIXES","1"))return 5;
 if(!set("BB_UWP_LAZY_RESERVATION","1")||!set("BB_UWP_DEMAND_COMMIT","1")||!set("BB_UWP_TOUCH_COMMIT","1")||!set("BB_UWP_PRIVATE_CPU","1")||!set("BB_UWP_PRIVATE_GPU","1")||!set("BB_UWP_HOST_COMMIT_TRIAL","1")||!set("BB_UWP_HOST_PAGE_TOUCH","1")||!set("BB_UWP_TOUCH_CPU_MIN_MB","64")||!set("BB_FRAME_STATS","0"))return 5;
 if(!set("BB_BUFFER_ARENAS","dense")||!set("BB_STAGING_KEEP_MB","32")||!set("BB_STAGING_KEEP_VRAM_MB","16"))return 5;
 char checkpath194[32768];path(checkpath194,sizeof(checkpath194),local,"allocator-check194.txt");
 FILE* check194=fopen(checkpath194,"r");int checkmode194=0;if(check194){fscanf(check194,"%d",&checkmode194);fclose(check194);}
 if(!set("BB_ALLOCATOR_CHECK194",checkmode194==1?"1":"0"))return 5;
 int ring194=switch194(local,"constant-ring-off.txt"),draw194=switch194(local,"draw-pipeline-off.txt"),copy194=switch194(local,"copy-worker-mode.txt"),cmd194=switch194(local,"command-copy-mode.txt");
 if(ring194<0||draw194<0||copy194<0||cmd194<0)return 87;
 uint64_t mask194=(ring194?(UINT64_C(1)<<36):0)|(draw194?(UINT64_C(1)<<37):0)|(copy194?UINT64_C(524288):0);
 __atomic_store_n(&runtime_disabled_optimizations,mask194,__ATOMIC_RELEASE);
 if(!set("BB_DRAW_PIPE",draw194?"0":"")||!set("BB_COPY_GPU_BUFFERS",cmd194?"1":"0"))return 5;
 printf("TLOU194 Xbox controls: draw_pipeline_off=%d; constant_ring_off=%d; copy_workers=%d; retain_command_bytes=%d; mask=%llu; fps_limit=30\n",draw194,ring194,copy194,cmd194,(unsigned long long)mask194);
 if(!runtime_guarded_image_verify(boot)){fputs("TLOU194: first-touch requires matching validated guard image\n",stderr);return 87;}
 if(!cpu_only){
  int e=bbgpu_uwp_set_host(host);if(e)return e;
  /* BB_DRED=1: Dozen's DRED on its own device factory, logging only after a fault
   * (DZN_PRIVATE_DRED would also log every command and descriptor). Off by default: the Xbox
   * answers DXGI_ERROR_UNSUPPORTED for DRED's data even when it is enabled (2026-11-02). */
  const char*dred=getenv("BB_DRED");if(dred&&dred[0]=='1'&&!set("DZN_PRIVATE_DRED_CAPTURE","1"))return 5;
  HMODULE driver=LoadPackagedLibrary(L"vulkan_dzn.dll",0);if(!driver)return (int)GetLastError();
  typedef HRESULT(__cdecl*Presenter)(uint32_t,void*,void(*)(void*,uint32_t*,uint32_t*),int32_t(*)(void*,void*));
  Presenter f=(Presenter)GetProcAddress(driver,"dzn_uwp_set_presenter");if(!f)return 127;
  host_attach194=host->attach_swapchain;
  HRESULT hr=f(1,host->context,host->extent,attach194);if(FAILED(hr))return (int)hr;
 }
 if(switch194(local,"thread-trace194.txt")==1){char trace194[32768];path(trace194,sizeof(trace194),local,"gpu-thread-trace194.log");schedule_gpu_trace194(trace194);}
 char name[]="Tlou194Uwp",profile[]="--content-profile",app[]="--app0",usr[]="--user",timeout[]="--timeout",zero[]="0",cpu[]="--cpu-only";
 char*args[]={name,boot,profile,content,app,game,usr,user,timeout,zero,cpu,NULL};Start st={cpu_only?11:10,args,-1};
 HANDLE t=CreateThread(NULL,16*1024*1024,run,&st,0,NULL);if(!t)return (int)GetLastError();DWORD w=WaitForSingleObject(t,INFINITE);DWORD e=w==WAIT_OBJECT_0?0:GetLastError();CloseHandle(t);return e?(int)e:st.result;
}
