/* SPDX-License-Identifier: GPL-2.0-or-later
 * v1.9.4 renderer in the existing TLOU UWP host ABI. No old renderer objects. */
#include <windows.h>
#include <psapi.h>
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
void schedule_gpu_trace194(const char*) __attribute__((weak));
void register_gpu_thread194(const char*) __attribute__((weak)); /* TLOU trace; absent in Uncharted */
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
  /* Uncharted Xbox port (2026-11-04): breakpoints (the Xbox D3D12 driver's checks) with the call chain
     unwound from the exception context. */
  if(e->ExceptionRecord->ExceptionCode==0x80000003){
   CONTEXT c=*e->ContextRecord;
   for(int i=0;i<40&&c.Rip;++i){
    HMODULE fm=0;char fname[512]="?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)(uintptr_t)c.Rip,&fm);
    if(fm)GetModuleFileNameA(fm,fname,sizeof(fname));const char*base=strrchr(fname,'\\');
    int k=snprintf(line,sizeof(line),"  unwind %s+%llx\n",base?base+1:fname,(unsigned long long)(c.Rip-(uintptr_t)fm));
    if(k>0&&k<(int)sizeof(line))WriteFile(fatal194,line,k,&wrote,0);
    DWORD64 image=0;PRUNTIME_FUNCTION f=RtlLookupFunctionEntry(c.Rip,&image,NULL);
    if(!f){c.Rip=*(DWORD64*)(uintptr_t)c.Rsp;c.Rsp+=8;continue;}
    void*hd=NULL;DWORD64 fr=0;RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,c.Rip,f,&c,&hd,&fr,NULL);
   }
   FlushFileBuffers(fatal194);
  }
  /* Uncharted Xbox port (2026-11-04): debug output (OutputDebugString: D3D12/Dozen messages) written out. */
  if(e->ExceptionRecord->ExceptionCode==0x40010006&&e->ExceptionRecord->NumberParameters>=2){
   const char*text=(const char*)e->ExceptionRecord->ExceptionInformation[1];
   size_t len=(size_t)e->ExceptionRecord->ExceptionInformation[0];if(len>65536)len=65536;
   if(text&&len){WriteFile(fatal194,"  debug: ",9,&wrote,0);WriteFile(fatal194,text,(DWORD)len,&wrote,0);WriteFile(fatal194,"\n",1,&wrote,0);FlushFileBuffers(fatal194);}}
  /* God of War port (2026-11-04): heap corruption (c0000374) and access violations: the call stack as
     module+rva, to name the code that freed or allocated when the heap check failed. */
  if(e->ExceptionRecord->ExceptionCode==0xc0000374||e->ExceptionRecord->ExceptionCode==0xc0000005){
   void*frames[40];USHORT count=CaptureStackBackTrace(0,40,frames,NULL);
   for(USHORT i=0;i<count;++i){HMODULE fm=0;char fname[512]="?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)frames[i],&fm);
    if(fm)GetModuleFileNameA(fm,fname,sizeof(fname));const char*base=strrchr(fname,'\\');
    int k=snprintf(line,sizeof(line),"  frame %u %s+%llx\n",(unsigned)i,base?base+1:fname,(unsigned long long)((uintptr_t)frames[i]-(uintptr_t)fm));
    if(k>0&&k<(int)sizeof(line))WriteFile(fatal194,line,k,&wrote,0);}
   FlushFileBuffers(fatal194);}
 }
 return EXCEPTION_CONTINUE_SEARCH;
}
/* Uncharted Xbox port (2026-11-04): the game process ended a second after the swapchain attached with
   no exception at all (native-runtime-crash194.log empty). abort() and process exit (exit/ExitProcess
   on any thread runs the DLL's destructors on that thread) are written there with the call stack. */
#include <signal.h>
static void exit_stack194(const char* why){
 if(fatal194==INVALID_HANDLE_VALUE)return;
 char line[1024];DWORD wrote;
 int n=snprintf(line,sizeof(line),"%s; thread %lu\n",why,GetCurrentThreadId());
 if(n>0&&n<(int)sizeof(line))WriteFile(fatal194,line,n,&wrote,0);
 void*frames[48];USHORT count=CaptureStackBackTrace(0,48,frames,NULL);
 for(USHORT i=0;i<count;++i){HMODULE fm=0;char fname[512]="?";
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)frames[i],&fm);
  if(fm)GetModuleFileNameA(fm,fname,sizeof(fname));const char*base=strrchr(fname,'\\');
  int k=snprintf(line,sizeof(line),"  frame %u %s+%llx\n",(unsigned)i,base?base+1:fname,(unsigned long long)((uintptr_t)frames[i]-(uintptr_t)fm));
  if(k>0&&k<(int)sizeof(line))WriteFile(fatal194,line,k,&wrote,0);}
 FlushFileBuffers(fatal194);
}
/* Uncharted Xbox port (2026-11-04): the boot stops after the Vulkan device without a crash. 25 s and
   60 s after the game entry every thread's native stack (RtlVirtualUnwind) goes to
   native-runtime-crash194.log. BB_STACK_DUMP=0: off. */
#include <tlhelp32.h>
static volatile int dump_stop_at_guest194;
static void dump_thread194(HANDLE h, DWORD tid){
 char line[1024];DWORD wrote;
 CONTEXT c;memset(&c,0,sizeof(c));c.ContextFlags=CONTEXT_FULL;
 if(SuspendThread(h)==(DWORD)-1)return;
 if(!GetThreadContext(h,&c)){ResumeThread(h);return;}
 /* The return addresses are collected while it is suspended; the module names (loader locks,
    which a suspended thread may hold: st1's whole-process stop) after it is resumed. */
 DWORD64 rips[40];int count=0;
 for(;count<40&&c.Rip;++count){
  rips[count]=c.Rip;
  DWORD64 image=0;PRUNTIME_FUNCTION f=RtlLookupFunctionEntry(c.Rip,&image,NULL);
  if(!f){ if(dump_stop_at_guest194||IsBadReadPtr((void*)(uintptr_t)c.Rsp,8)){++count;break;} c.Rip=*(DWORD64*)(uintptr_t)c.Rsp;c.Rsp+=8;continue;}
  void*handler_data=NULL;DWORD64 frame=0;
  RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,c.Rip,f,&c,&handler_data,&frame,NULL);
 }
 ResumeThread(h);
 int n=snprintf(line,sizeof(line),"thread %lu\n",tid);WriteFile(fatal194,line,n,&wrote,0);
 for(int i=0;i<count;++i){
  HMODULE fm=0;char fname[512]="?";
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)(uintptr_t)rips[i],&fm);
  if(fm)GetModuleFileNameA(fm,fname,sizeof(fname));const char*base=strrchr(fname,'\\');
  int k=snprintf(line,sizeof(line),"  %s+%llx\n",base?base+1:fname,(unsigned long long)(rips[i]-(uintptr_t)fm));
  if(k>0&&k<(int)sizeof(line))WriteFile(fatal194,line,k,&wrote,0);
 }
}
static volatile LONG report194_seq;
extern void runtime_memory_lock_owner(unsigned long *tid, unsigned long *line);
static void dump_all194(const char *title){
 char line[192];DWORD wrote;
 unsigned long owner=0,owner_line=0;runtime_memory_lock_owner(&owner,&owner_line);
 int n=snprintf(line,sizeof(line),"== %s; memory table write lock: thread %lu (line %lu)\n",title,owner,owner_line);
 WriteFile(fatal194,line,n,&wrote,0);
 const DWORD self=GetCurrentThreadId(),pid=GetCurrentProcessId();
 HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
 if(snap==INVALID_HANDLE_VALUE)return;
 THREADENTRY32 te;te.dwSize=sizeof(te);
 for(BOOL ok=Thread32First(snap,&te);ok;ok=Thread32Next(snap,&te)){
  if(te.th32OwnerProcessID!=pid||te.th32ThreadID==self)continue;
  HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,te.th32ThreadID);
  if(h){dump_thread194(h,te.th32ThreadID);CloseHandle(h);}
 }
 CloseHandle(snap);FlushFileBuffers(fatal194);
}
/* Uncharted (2026-11-04): a report that stops for 20 s (it waits on the memory table) is a stall:
   every thread's stack, twice 10 s apart. */
/* And a game that presents no frame for 15 s once it has presented some (a freeze in a load). */
extern uint64_t tlou194_unique_game_frames(void);
/* Also every second: our fault handlers back in front of the D3D12 driver's (probe.c), which it
   may install again at any time and which breaks into the debugger on the pager's faults. */
extern void bb_fault_handlers_to_front(void);
/* No C runtime in the loop (st1: the whole process stopped and this thread never wrote): the
   environment is read once, the heartbeat is formatted by hand and written with WriteFile. */
static int fmt_u64_194(char*out,uint64_t v){char t[24];int n=0;do{t[n++]=(char)('0'+v%10);v/=10;}while(v);for(int i=0;i<n;++i)out[i]=t[n-1-i];return n;}
static DWORD WINAPI stall_watch194(void*arg){
 (void)arg;
 LONG last=-1;DWORD same=0;int dumps=0;
 uint64_t frames_last=0;DWORD frames_same=0;int frame_dumps=0;
 int report_on=-1;unsigned beat=0;
 for(;;){
  Sleep(1000);
  if(fatal194==INVALID_HANDLE_VALUE)return 0;
  const uint64_t frames=tlou194_unique_game_frames();
  if(++beat%5==0){
   char hb[96];int n=0;const char*p="watch ";while(*p)hb[n++]=*p++;
   n+=fmt_u64_194(hb+n,GetTickCount64());hb[n++]=' ';n+=fmt_u64_194(hb+n,frames);hb[n++]=' ';
   n+=fmt_u64_194(hb+n,(uint64_t)report194_seq);hb[n++]='\n';
   DWORD wrote;WriteFile(fatal194,hb,n,&wrote,0);
  }
  if(frames!=frames_last||frames<100){frames_last=frames;frames_same=0;}
  else{
   frames_same+=1000;
   if(frame_dumps<2&&frames_same>=15000u*(frame_dumps+1)){
    char title[96];snprintf(title,sizeof(title),"stall dump (no game frame for %lu s, frame %llu)",(unsigned long)(frames_same/1000),(unsigned long long)frames);
    dump_all194(title);++frame_dumps;
   }
  }
  if(report_on<0&&beat>=60){const char*hr=getenv("BB_HOST_REPORT");report_on=hr&&hr[0]=='1';}
  if(report_on!=1){same=0;continue;}
  const LONG now=report194_seq;
  if(now!=last){last=now;same=0;continue;}
  same+=1000;
  if(dumps<2&&same>=10000+5000u*dumps){dump_all194(dumps?"stall dump 2 (report stopped)":"stall dump 1 (report stopped 10 s)");++dumps;}
 }
 return 0;
}
/* Uncharted (2026-11-04): BB_ZERO_TRIM_MB=N scans N MiB of private guest memory a second and
   decommits the all-zero pages (they read as zero again on the next touch; nothing goes to disk). */
extern uint64_t runtime_memory_trim_zero_private(uint64_t scan_bytes);
static volatile LONG64 zero_trimmed194;
static DWORD WINAPI zero_trim194(void*arg){
 (void)arg;
 Sleep(30000); /* bb-env is applied by now */
 const char*mb=getenv("BB_ZERO_TRIM_MB");
 const uint64_t per=mb?strtoull(mb,NULL,10)<<20:0;
 if(!per)return 0;
 for(;;){Sleep(1000);InterlockedExchangeAdd64(&zero_trimmed194,(LONG64)runtime_memory_trim_zero_private(per));}
}
/* FPS work (2026-11-07), BB_SPIKE_DUMP_MS=N: where every thread is when no game frame came for N ms
   (and again 100 ms later if there is still none). At most one spike a second, 80 dumps. */
static DWORD WINAPI spike_watch194(void*arg){
 (void)arg;
 Sleep(30000); /* bb-env is applied by now */
 const char*v=getenv("BB_SPIKE_DUMP_MS");
 const DWORD limit=v?strtoul(v,NULL,10):0;
 if(!limit)return 0;
 dump_stop_at_guest194=1;
 {const char*a=getenv("BB_SPIKE_AFTER_S");const DWORD s=a?strtoul(a,NULL,10):0;if(s>30)Sleep((s-30)*1000);}
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
 uint64_t last=0;ULONGLONG at=GetTickCount64(),last_spike=0;int fired=0,dumps=0;
 for(;dumps<80;){
  Sleep(2);
  const uint64_t frames=tlou194_unique_game_frames();
  const ULONGLONG now=GetTickCount64();
  if(frames!=last){last=frames;at=now;fired=0;continue;}
  if(last<100)continue;
  const ULONGLONG waited=now-at;
  if((fired==0&&waited>=limit&&now-last_spike>=1000)||(fired==1&&waited>=limit+100)){
   char title[96];snprintf(title,sizeof(title),"spike: no frame for %llu ms, frame %llu, tick %llu",(unsigned long long)waited,(unsigned long long)last,(unsigned long long)now);
   dump_all194(title);++fired;++dumps;last_spike=now;
  }
 }
 return 0;
}
/* FPS work (2026-11-07): a sampling profile of every thread. Record: tid, cpu-state word, depth, then return addresses.
   Unwinding stops at the first address with no unwind data (guest code). Modules and thread names are written at the end. */
static char profile_path194[600];
typedef HRESULT (WINAPI *GetThreadDescription194)(HANDLE,PWSTR*);
static DWORD WINAPI profile194(void*arg){
 (void)arg;
 Sleep(30000);
 const char*v=getenv("BB_PROFILE_MS");
 const DWORD period=v?strtoul(v,NULL,10):0;
 if(!period||!profile_path194[0])return 0;
 const char*a=getenv("BB_PROFILE_AFTER_S");const DWORD after=a?strtoul(a,NULL,10):130;
 const char*f=getenv("BB_PROFILE_FOR_S");const DWORD span=f?strtoul(f,NULL,10):120;
 if(after>30)Sleep((after-30)*1000);
 enum{DEPTH=24,CAP=64<<20};
 uint64_t*buf=(uint64_t*)VirtualAlloc(NULL,CAP,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 if(!buf)return 0;
 size_t used=0;const size_t cap=CAP/8;
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
 const DWORD self=GetCurrentThreadId(),pid=GetCurrentProcessId();
 enum{MAXT=128};HANDLE handles[MAXT];DWORD tids[MAXT];int thread_count=0;
 const ULONGLONG began=GetTickCount64();ULONGLONG refreshed=0;uint64_t rounds=0;
 while(GetTickCount64()-began<span*1000ull&&used+MAXT*(DEPTH+3)<cap){
  const ULONGLONG now=GetTickCount64();
  if(now-refreshed>=2000){
   refreshed=now;
   HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
   if(snap!=INVALID_HANDLE_VALUE){
    THREADENTRY32 te;te.dwSize=sizeof(te);
    for(BOOL ok=Thread32First(snap,&te);ok;ok=Thread32Next(snap,&te)){
     if(te.th32OwnerProcessID!=pid||te.th32ThreadID==self||thread_count>=MAXT)continue;
     int known=0;for(int i=0;i<thread_count;++i)if(tids[i]==te.th32ThreadID){known=1;break;}
     if(known)continue;
     HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,te.th32ThreadID);
     if(h){handles[thread_count]=h;tids[thread_count]=te.th32ThreadID;++thread_count;}
    }
    CloseHandle(snap);
   }
  }
  for(int t=0;t<thread_count;++t){
   CONTEXT c;memset(&c,0,sizeof(c));c.ContextFlags=CONTEXT_FULL;
   if(SuspendThread(handles[t])==(DWORD)-1)continue;
   if(!GetThreadContext(handles[t],&c)){ResumeThread(handles[t]);continue;}
   uint64_t*head=buf+used;size_t depth=0;
   for(;depth<DEPTH&&c.Rip;++depth){
    head[3+depth]=c.Rip;
    DWORD64 image=0;PRUNTIME_FUNCTION fn=RtlLookupFunctionEntry(c.Rip,&image,NULL);
    if(!fn){++depth;break;}
    void*data=NULL;DWORD64 frame=0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,c.Rip,fn,&c,&data,&frame,NULL);
   }
   ResumeThread(handles[t]);
   head[0]=tids[t];head[1]=rounds;head[2]=depth;used+=3+depth;
  }
  ++rounds;
  Sleep(period);
 }
 HANDLE out=CreateFileA(profile_path194,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
 if(out==INVALID_HANDLE_VALUE)return 0;
 DWORD wrote;char line[700];int n;
 HMODULE mods[256];DWORD need=0;
 n=snprintf(line,sizeof(line),"profile rounds %llu ms %llu samples_u64 %llu\n",(unsigned long long)rounds,(unsigned long long)(GetTickCount64()-began),(unsigned long long)used);WriteFile(out,line,n,&wrote,0);
 if(EnumProcessModules(GetCurrentProcess(),mods,sizeof(mods),&need)){
  for(DWORD i=0;i<need/sizeof(HMODULE)&&i<256;++i){
   MODULEINFO mi;char name[400]="?";
   if(!GetModuleInformation(GetCurrentProcess(),mods[i],&mi,sizeof(mi)))continue;
   GetModuleFileNameA(mods[i],name,sizeof(name));
   const char*base=strrchr(name,'\\');
   n=snprintf(line,sizeof(line),"module %llx %lx %s\n",(unsigned long long)(uintptr_t)mi.lpBaseOfDll,(unsigned long)mi.SizeOfImage,base?base+1:name);
   WriteFile(out,line,n,&wrote,0);
  }
 }
 GetThreadDescription194 describe=(GetThreadDescription194)(void*)GetProcAddress(GetModuleHandleA("kernelbase.dll"),"GetThreadDescription");
 for(int t=0;t<thread_count;++t){
  char name[200]="";
  if(describe){PWSTR w=NULL;if(SUCCEEDED(describe(handles[t],&w))&&w){WideCharToMultiByte(CP_UTF8,0,w,-1,name,sizeof(name),NULL,NULL);LocalFree(w);}}
  FILETIME c0,e0,k0,u0;unsigned long long ku=0,uu=0;
  if(GetThreadTimes(handles[t],&c0,&e0,&k0,&u0)){ku=(((unsigned long long)k0.dwHighDateTime<<32)|k0.dwLowDateTime)/10000;uu=(((unsigned long long)u0.dwHighDateTime<<32)|u0.dwLowDateTime)/10000;}
  n=snprintf(line,sizeof(line),"thread %lu kernel_ms %llu user_ms %llu %s\n",tids[t],ku,uu,name);WriteFile(out,line,n,&wrote,0);
 }
 n=snprintf(line,sizeof(line),"binary\n");WriteFile(out,line,n,&wrote,0);
 size_t at=0;while(at<used){const size_t chunk=used-at>(1u<<24)?(1u<<24):used-at;if(!WriteFile(out,buf+at,(DWORD)(chunk*8),&wrote,0))break;at+=chunk;}
 CloseHandle(out);VirtualFree(buf,0,MEM_RELEASE);
 return 0;
}
/* The calling thread's CPU time, for the GPU thread's statistics. */
void bb_thread_cpu_us(unsigned long long*user,unsigned long long*kernel){
 FILETIME c,e,k,u;
 if(GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u)){
  *user=(((unsigned long long)u.dwHighDateTime<<32)|u.dwLowDateTime)/10;
  *kernel=(((unsigned long long)k.dwHighDateTime<<32)|k.dwLowDateTime)/10;
 }
}
static DWORD WINAPI stack_dump194(void*arg){
 (void)arg;
 {HANDLE w=CreateThread(NULL,0,stall_watch194,NULL,0,NULL);if(w)CloseHandle(w);}
 {HANDLE w=CreateThread(NULL,0,spike_watch194,NULL,0,NULL);if(w)CloseHandle(w);}
 {HANDLE w=CreateThread(NULL,0,profile194,NULL,0,NULL);if(w)CloseHandle(w);}
 {HANDLE z=CreateThread(NULL,0,zero_trim194,NULL,0,NULL);if(z)CloseHandle(z);}
 const DWORD waits[2]={25000,35000};
 for(int round=0;round<2;++round){
  Sleep(waits[round]);
  if(fatal194==INVALID_HANDLE_VALUE)return 0;
  char line[128];DWORD wrote;int n=snprintf(line,sizeof(line),"== stack dump %d (%lu ms after entry)\n",round+1,round?60000UL:25000UL);
  WriteFile(fatal194,line,n,&wrote,0);
  const DWORD self=GetCurrentThreadId(),pid=GetCurrentProcessId();
  HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
  if(snap==INVALID_HANDLE_VALUE){n=snprintf(line,sizeof(line),"thread snapshot failed (%lu)\n",GetLastError());WriteFile(fatal194,line,n,&wrote,0);continue;}
  THREADENTRY32 te;te.dwSize=sizeof(te);
  for(BOOL ok=Thread32First(snap,&te);ok;ok=Thread32Next(snap,&te)){
   if(te.th32OwnerProcessID!=pid||te.th32ThreadID==self)continue;
   HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,te.th32ThreadID);
   if(h){dump_thread194(h,te.th32ThreadID);CloseHandle(h);}
  }
  CloseHandle(snap);FlushFileBuffers(fatal194);
 }
 return 0;
}
static void abort_trace194(int sig){(void)sig;exit_stack194("abort (SIGABRT)");fflush(stdout);signal(SIGABRT,SIG_DFL);abort();}
/* Uncharted Xbox port (2026-11-04): BB_HOST_REPORT=1 writes the memory layer's guest commit by kind and
   the host commit (heaps, modules, GPU memory) to stdout every 15 s (God of War hooks these into its
   texture cache's memory line, which this renderer does not have). */
extern void runtime_memory_report_committed(void);
extern void runtime_memory_report_host(void);
extern void bb_dense_arena_stats(unsigned long long* bytes, unsigned long long* count);
extern void bb_zero_copy_stats(unsigned long long* bytes, unsigned long long* count, unsigned long long* misses, unsigned long long* waits);
extern void runtime_memory_gpu_import_stats(uint64_t *bytes, uint64_t *refused, uint64_t *broken);
extern void runtime_memory_gpu_share_stats(uint64_t *bytes, uint64_t *count, uint64_t *refused, uint64_t *missed);
extern void bb_staging_stats(unsigned long long* ring, unsigned long long* large);
extern void bb_vram_stats(unsigned long long* images, unsigned long long* image_count, unsigned long long* buffers, unsigned long long* device);
static DWORD WINAPI memory_report194(void*arg){
 (void)arg;
 for(;;){
  const char*ms=getenv("BB_HOST_REPORT_MS"); /* the interval (default 15 s) */
  const unsigned long wait=ms?strtoul(ms,NULL,10):0;Sleep(wait>=500?wait:15000);
  const char*hr=getenv("BB_HOST_REPORT"); /* read here: bb-env.txt is applied after this thread starts */
  if(!hr||hr[0]!='1')continue;
  /* the lock-free GPU counters first: the host walk below can wait on the memory layer's lock */
  {unsigned long long ab=0,ac=0;bb_dense_arena_stats(&ab,&ac);printf("Buffer cache: %llu dense arenas, %llu MiB\n",ac,ab>>20);}
  {unsigned long long zb=0,zc=0,zm=0,zw=0;bb_zero_copy_stats(&zb,&zc,&zm,&zw);
   uint64_t gb=0,gr=0,gx=0;runtime_memory_gpu_import_stats(&gb,&gr,&gx);
   uint64_t sb=0,sc=0,sr=0,sm=0;runtime_memory_gpu_share_stats(&sb,&sc,&sr,&sm);
   if(sc||sr||sm)printf("Runtime: GPU-shared guest memory %llu MiB in %llu ranges; %llu refused, %llu not placed\n",
                        (unsigned long long)(sb>>20),(unsigned long long)sc,(unsigned long long)sr,(unsigned long long)sm);
   if(zc||zm||gr)printf("Buffer cache: %llu imported arenas, %llu MiB (guest pinned %llu MiB); %llu not imported, %llu refused, %llu broken; %llu read-back waits\n",
                        zc,zb>>20,(unsigned long long)(gb>>20),zm,(unsigned long long)gr,(unsigned long long)gx,zw);}
  {unsigned long long im=0,ic=0,bu=0,dv=0;bb_vram_stats(&im,&ic,&bu,&dv);printf("GPU memory: images %llu MiB (%llu), device-local buffers %llu MiB, device memory %llu MiB%c",im>>20,ic,bu>>20,dv>>20,10);}
  {unsigned long long sr=0,sl=0;bb_staging_stats(&sr,&sl);printf("Staging (device-local): rings %llu MiB, large %llu MiB%c",sr>>20,sl>>20,10);}
  {PROCESS_MEMORY_COUNTERS_EX pm;memset(&pm,0,sizeof(pm));pm.cb=sizeof(pm);
   if(GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&pm,sizeof(pm)))
    printf("Process memory: private %llu MiB, working set %llu MiB; zero pages trimmed %llu MiB%c",(unsigned long long)(pm.PrivateUsage>>20),(unsigned long long)(pm.WorkingSetSize>>20),(unsigned long long)(zero_trimmed194>>20),10);}
  fflush(stdout);
  runtime_memory_report_committed();runtime_memory_report_host();
  {extern void runtime_memory_report_pager(void);runtime_memory_report_pager();}
  fflush(stdout);InterlockedIncrement(&report194_seq);}
 return 0;
}
__attribute__((destructor)) static void detach_trace194(void){exit_stack194("process exit (runtime DLL destructors)");}

/* God of War port (2026-11-04), BB_HEAP_WATCH=1 (diagnostic, Xbox): about a third of the boots died
   of heap corruption a few seconds after the pipeline warm-up (c0000374, found by an unrelated malloc:
   boot43 in mutex_init). Every 200 ms for the first 3 minutes the process heap is validated; at the
   first failure the heap is walked and the last busy blocks before the walk broke are written to
   native-runtime-crash194.log (address, size, first 64 bytes, first qword as module+rva). */
static void heap_hex194(char*out,size_t n,const unsigned char*p,size_t bytes){
 size_t at=0;for(size_t i=0;i<bytes&&at+3<n;++i)at+=(size_t)snprintf(out+at,n-at,"%02x",p[i]);
}
static DWORD WINAPI heap_watch194(void*period_ms){
 const DWORD period=(DWORD)(uintptr_t)period_ms;HANDLE heap=GetProcessHeap();const ULONGLONG began=GetTickCount64();ULONGLONG last_ok=began;
 while(GetTickCount64()-began<180000){
  Sleep(period);
  if(HeapValidate(heap,0,NULL)){last_ok=GetTickCount64();continue;}
  char line[1024];DWORD wrote;
  int k=snprintf(line,sizeof(line),"heap watch: invalid at %llu ms (valid at %llu ms)\n",
                 (unsigned long long)(GetTickCount64()-began),(unsigned long long)(last_ok-began));
  if(fatal194!=INVALID_HANDLE_VALUE&&k>0)WriteFile(fatal194,line,k,&wrote,0);
  struct{void*at;size_t size;unsigned char head[64];}ring[4];int filled=0,pos=0;
  PROCESS_HEAP_ENTRY entry;memset(&entry,0,sizeof(entry));
  if(HeapLock(heap)){
   while(HeapWalk(heap,&entry)){
    if(!(entry.wFlags&PROCESS_HEAP_ENTRY_BUSY))continue;
    ring[pos].at=entry.lpData;ring[pos].size=entry.cbData;
    size_t take=entry.cbData<64?entry.cbData:64;memset(ring[pos].head,0,64);memcpy(ring[pos].head,entry.lpData,take);
    pos=(pos+1)%4;if(filled<4)++filled;
   }
   const DWORD walk_error=GetLastError();HeapUnlock(heap);
   k=snprintf(line,sizeof(line),"heap watch: walk stopped (error %lu) after these busy blocks:\n",walk_error);
   if(fatal194!=INVALID_HANDLE_VALUE&&k>0)WriteFile(fatal194,line,k,&wrote,0);
  }
  for(int i=0;i<filled;++i){
   const int r=(pos-filled+i+4)%4;char hex[160];heap_hex194(hex,sizeof(hex),ring[r].head,64);
   uintptr_t q=0;memcpy(&q,ring[r].head,8);HMODULE m=0;char name[512]="-";
   if(q&&GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)q,&m)&&m){
    GetModuleFileNameA(m,name,sizeof(name));}
   const char*base=strrchr(name,'\\');
   k=snprintf(line,sizeof(line),"  block %p size %llu head %s first %s+%llx\n",ring[r].at,(unsigned long long)ring[r].size,hex,
              base?base+1:name,(unsigned long long)(m?q-(uintptr_t)m:0));
   if(fatal194!=INVALID_HANDLE_VALUE&&k>0)WriteFile(fatal194,line,k,&wrote,0);
  }
  if(fatal194!=INVALID_HANDLE_VALUE)FlushFileBuffers(fatal194);
  return 1;
 }
 return 0;
}
static DWORD WINAPI stdout_flush194(void*unused){(void)unused;for(;;){Sleep(500);fflush(stdout);}return 0;}
typedef struct{int argc;char**argv;int result;} Start;
static DWORD WINAPI run(void*p){if(register_gpu_thread194)register_gpu_thread194("guest-main");Start*s=p;HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);if(FAILED(hr)){s->result=(int)hr;return (DWORD)hr;}s->result=bb_uwp_runtime_main(s->argc,s->argv);CoUninitialize();return (DWORD)s->result;}
__declspec(dllexport) int __cdecl tlou_uwp_run_game(const char*local,const char*packaged,const BbUwpHost*host,int cpu_only){
 if(!local||!packaged||!*local||!*packaged||strlen(local)>30000||strlen(packaged)>30000||(cpu_only!=0&&cpu_only!=1))return 87;
 if(!cpu_only&&(!host||host->size!=sizeof(*host)||host->version!=1||!host->context||!host->extent||!host->attach_swapchain||!host->read_input||!host->closed))return 87;
 if(InterlockedCompareExchange(&started,1,0))return 170;
 char output[32768],error[32768],icd[32768],boot[32768],content[32768],game[32768],user[32768],config[32768];
 if(!path(output,sizeof(output),local,"tlou-stdout.log")||!path(error,sizeof(error),local,"tlou-stderr.log")||!path(icd,sizeof(icd),packaged,"dzn_icd.x86_64.json")||!path(boot,sizeof(boot),packaged,"GameBoot\\boot-linked.bin")||!path(content,sizeof(content),packaged,"GameBoot\\content.bin")||!path(game,sizeof(game),packaged,"GameData")||!path(user,sizeof(user),local,"tlou-bb194-user")||!path(config,sizeof(config),local,"bbport.ini"))return 87;
 if(!freopen(output,"w",stdout)||!freopen(error,"w",stderr))return 5;
 /* God of War port (2026-11-04): stdout buffered (64 KiB), flushed every 500 ms by stdout_flush194;
    unbuffered, every printf was a WriteFile on the calling thread: ~4% of the Xbox GPU command
    thread (boot87). stderr (fault reports) stays unbuffered. BB_STDOUT_UNBUFFERED=1: as before. */
 {const char*unbuffered=getenv("BB_STDOUT_UNBUFFERED");
  /* Uncharted (2026-11-04): unbuffered by default while bringing it up (BB_STDOUT_UNBUFFERED=0: buffered). */
  if(!unbuffered||unbuffered[0]!='0')setvbuf(stdout,NULL,_IONBF,0);
  else{setvbuf(stdout,NULL,_IOFBF,1<<16);HANDLE f=CreateThread(NULL,0,stdout_flush194,NULL,0,NULL);if(f)CloseHandle(f);}}
 setvbuf(stderr,NULL,_IONBF,0);
 char fatalpath[32768];path(fatalpath,sizeof(fatalpath),local,"native-runtime-crash194.log");
 path(profile_path194,sizeof(profile_path194),local,"profile194.bin");
 fatal194=CreateFileA(fatalpath,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
 void* fatalcookie=AddVectoredExceptionHandler(0,fatal_trace194);
 signal(SIGABRT,abort_trace194);
 {HANDLE t=CreateThread(NULL,0,memory_report194,NULL,0,NULL);if(t)CloseHandle(t);}
 {const char*dump=getenv("BB_STACK_DUMP");if(!dump||dump[0]!='0'){HANDLE t=CreateThread(NULL,0,stack_dump194,NULL,0,NULL);if(t)CloseHandle(t);}}
 printf("TLOU194: native crash trace installed=%d; handle=%p\n",fatalcookie!=NULL,fatal194);

 if(_mkdir(user)&&errno!=EEXIST)return 5;
 /* God of War port (2026-11-04): <user>\bb-env.txt (KEY=VALUE lines) applied here, before any
    game thread runs. Applied by the renderer (ApplyBbEnvFile, _putenv_s while guest threads read
    the environment) it corrupted the heap at boot on the Xbox (c0000374, xbox-boot3/4skipsnap);
    BB_ENV_EARLY tells it the file is done. */
 {char envpath[32768];path(envpath,sizeof(envpath),user,"bb-env.txt");FILE*envfile=fopen(envpath,"r");
  printf("bb-env (early): %s %s\n",envpath,envfile?"found":"absent");
  if(envfile){char line[1024];
   while(fgets(line,sizeof(line),envfile)){size_t n=strlen(line);while(n&&(line[n-1]=='\n'||line[n-1]=='\r'||line[n-1]==' '))line[--n]=0;
    char*eq=strchr(line,'=');if(!n||line[0]=='#'||!eq)continue;*eq=0;
    if(!set(line,eq+1))return 5;printf("bb-env (early): %s=%s\n",line,eq+1);}
   fclose(envfile);}
  if(!set("BB_ENV_EARLY","1"))return 5;}
 {const char*watch=getenv("BB_HEAP_WATCH");
  /* 1: every 200 ms; a larger number: every that many milliseconds */
  const unsigned long period=watch?strtoul(watch,NULL,10):0;
  if(period){const unsigned long ms=period==1?200:period;
   HANDLE w=CreateThread(NULL,0,heap_watch194,(void*)(uintptr_t)ms,0,NULL);
   printf("heap watch: %s, every %lu ms for 3 min\n",w?"on":"failed to start",ms);if(w)CloseHandle(w);}}
 printf("TLOU194 UWP: upstream=3fabfa7a7af67a34a6dfc630b6ec2539b68fad4d; private_profile=%s; hostABI=1; new_gpu_objects=1\n",user);
 /* BB_FPS_LIMIT=30: a flip is presented when it arrives, a 30 FPS period after the previous one,
    instead of on the next tick of the emulated 60 Hz vblank. On those ticks a frame done just
    after one waited 16.7 ms more (50 ms, then 16 ms): 5-10% of courtyard frames; with the limit
    they stay at 33-35 ms (probe fl30, 2026-10-29). */
 if(!set("BB_GPU_USER_DIR",user)||!set("BB_USER_DIR",user)||!set("BB_CONFIG",config)||!set("VK_DRIVER_FILES",icd)||!set("VK_LOADER_LAYERS_DISABLE","~implicit~")||!set("BB_UPSCALER","off")||!set("BB_OBJECT_MOTION","0")||!set("BB_ONLINE","0")||!set("BB_GUEST_COMMIT_ON_MAP","1"))return 5;
 /* Uncharted (2026-11-04): 30 FPS unless bb-env.txt says otherwise (the PS4 ran these at 60); the
    settings run.py gives Uncharted 1 (eboot.bin): mesh raycaster copies landing in guest memory
    (U1 crashed in the Customs House and cutscenes without), tiny/small readbacks off, 60 Hz vblank. */
 if(!getenv("BB_FPS_LIMIT")&&!set("BB_FPS_LIMIT","30"))return 5;
 if(!getenv("BB_VBLANK_HZ")&&!set("BB_VBLANK_HZ","60"))return 5;
 if(!getenv("BB_SCALAR_COPY64_SYNC")&&!set("BB_SCALAR_COPY64_SYNC","2"))return 5;
 if(!getenv("BB_SCALAR_COPY64_DEFER")&&!set("BB_SCALAR_COPY64_DEFER","1"))return 5;
 if(!getenv("BB_TINY_READBACK")&&!set("BB_TINY_READBACK","0"))return 5;
 if(!getenv("BB_READBACK_SMALL_BUFFERS")&&!set("BB_READBACK_SMALL_BUFFERS","0"))return 5;
 {char loadexec[32768];path(loadexec,sizeof(loadexec),user,"loadexec.txt");if(!set("BB_LOADEXEC_FILE",loadexec))return 5;}
 if(!set("BB_UWP_LAZY_RESERVATION","1")||!set("BB_UWP_DEMAND_COMMIT","1")||!set("BB_UWP_TOUCH_COMMIT","1")||!set("BB_UWP_PRIVATE_CPU","1")||!set("BB_UWP_PRIVATE_GPU","1")||!set("BB_UWP_HOST_COMMIT_TRIAL","1")||!set("BB_UWP_HOST_PAGE_TOUCH","1")||!set("BB_UWP_TOUCH_CPU_MIN_MB","64"))return 5;
 if(!getenv("BB_FRAME_STATS")&&!set("BB_FRAME_STATS","0"))return 5; /* bb-env.txt may turn it on (2026-11-04) */
 /* Precise readbacks (2) stay the Xbox default; bb-env.txt may choose another mode (God of War port,
    2026-11-04: with them the PC re-uploaded GPU-written indirect arguments from guest memory). */
 /* Uncharted: the port's own readback default (run.py sets none); bb-env.txt may set BB_READBACKS. */
 if(!set("BB_BUFFER_ARENAS","dense")||!set("BB_STAGING_KEEP_MB","32")||!set("BB_STAGING_KEEP_VRAM_MB","16"))return 5;
 /* God of War port (2026-11-04): cs 0x3eac35c9 fails to build on the Xbox at every boot and about half
    of the boots then died of heap corruption after the warm-up (xbox-boot24/25/26/30): never built. */
 /* (God of War only: BB_NO_CREATE_CS=3eac35c9; not for Uncharted) */
 /* God of War port (2026-11-04), workaround: about a third of the Xbox boots died of heap corruption a
    few seconds after the pipeline warm-up (boot24-26, 30, 37, 40, 43, 45, 46, 48, 49, 53, 54, 79), never
    with the startup heap checkpoints on (boot57-62, 69-72: ten in a row). The checks (HeapValidate at
    bbgpu_init, warm-up, guest entry and the first mutex inits) hold the startup thread back at those
    points: a race between it and another startup thread (window events, VideoOut's flip thread), not
    found yet. On by default here; BB_HEAP_CHECKPOINTS=0 in bb-env.txt turns them off. Note: bb-env is
    applied before this line, so its value wins. */
 /* (Reverted the same day: boot80 and boot81 died with every checkpoint passing. Off unless bb-env.) */
 char checkpath194[32768];path(checkpath194,sizeof(checkpath194),local,"allocator-check194.txt");
 FILE* check194=fopen(checkpath194,"r");int checkmode194=0;if(check194){fscanf(check194,"%d",&checkmode194);fclose(check194);}
 if(!set("BB_ALLOCATOR_CHECK194",checkmode194==1?"1":"0"))return 5;
 int ring194=switch194(local,"constant-ring-off.txt"),draw194=switch194(local,"draw-pipeline-off.txt"),copy194=switch194(local,"copy-worker-mode.txt"),cmd194=switch194(local,"command-copy-mode.txt");
 if(ring194<0||draw194<0||copy194<0||cmd194<0)return 87;
 uint64_t mask194=(ring194?(UINT64_C(1)<<36):0)|(draw194?(UINT64_C(1)<<37):0)|(copy194?UINT64_C(524288):0);
 /* God of War port (2026-11-04): BB_DISABLE_MASK=<hex> (bb-env.txt) adds BbToggle bits, e.g.
    0x400000000000 (bit 46) = PipelinedIndirectDraws off: indirect draws drain the draw pipeline. */
 {const char*extra=getenv("BB_DISABLE_MASK");if(extra&&*extra)mask194|=(uint64_t)strtoull(extra,NULL,16);}
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
 if(switch194(local,"thread-trace194.txt")==1){char trace194[32768];path(trace194,sizeof(trace194),local,"gpu-thread-trace194.log");if(schedule_gpu_trace194)schedule_gpu_trace194(trace194);}
 char name[]="Tlou194Uwp",profile[]="--content-profile",app[]="--app0",usr[]="--user",timeout[]="--timeout",zero[]="0",cpu[]="--cpu-only";
 char*args[]={name,boot,profile,content,app,game,usr,user,timeout,zero,cpu,NULL};Start st={cpu_only?11:10,args,-1};
 HANDLE t=CreateThread(NULL,16*1024*1024,run,&st,0,NULL);if(!t)return (int)GetLastError();DWORD w=WaitForSingleObject(t,INFINITE);DWORD e=w==WAIT_OBJECT_0?0:GetLastError();CloseHandle(t);return e?(int)e:st.result;
}
