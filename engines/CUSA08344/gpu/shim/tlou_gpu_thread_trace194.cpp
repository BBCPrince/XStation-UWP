#include <windows.h>
#include <array>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
namespace {
struct Thread194{DWORD id;char name[96];HANDLE handle;unsigned long long last_cpu;};
std::mutex mutex194;std::array<Thread194,256> threads194{};unsigned count194{};
struct Sample194{Thread194 thread;CONTEXT ctx;DWORD error;uintptr_t stack[256];SIZE_T bytes;};
DWORD WINAPI capture194(void* arg){
 char path[32768];std::strcpy(path,(const char*)arg);HeapFree(GetProcessHeap(),0,arg);
 Sleep(15000);std::array<Sample194,256> samples{};unsigned count;
 {std::scoped_lock l{mutex194};count=count194;for(unsigned i=0;i<count;++i)samples[i].thread=threads194[i];}
 for(unsigned i=0;i<count;++i){auto& s=samples[i];if(s.thread.id==GetCurrentThreadId())continue;
  HANDLE t=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,s.thread.id);
  if(!t){s.error=GetLastError();continue;}
  if(SuspendThread(t)==DWORD(-1)){s.error=GetLastError();CloseHandle(t);continue;}
  s.ctx.ContextFlags=CONTEXT_FULL;
  if(!GetThreadContext(t,&s.ctx))s.error=GetLastError();
  else ReadProcessMemory(GetCurrentProcess(),(void*)s.ctx.Rsp,s.stack,sizeof(s.stack),&s.bytes);
  ResumeThread(t);CloseHandle(t);
 }
 FILE* f=std::fopen(path,"w");if(!f)return 1;
 std::fprintf(f,"One-shot GPU thread snapshot; tick=%llu; all sampled threads resumed before writing\n",GetTickCount64());
 for(unsigned i=0;i<count;++i){auto& s=samples[i];
  HMODULE m{};char file[1024]{};GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)s.ctx.Rip,&m);if(m)GetModuleFileNameA(m,file,sizeof(file));
  std::fprintf(f,"thread=%lu name=%s error=%lu ip=%llx module=%s rva=%llx rsp=%llx\n",s.thread.id,s.thread.name,s.error,s.ctx.Rip,file,s.ctx.Rip-(uintptr_t)m,s.ctx.Rsp);
  for(unsigned j=0;j<s.bytes/sizeof(uintptr_t);++j){auto address=s.stack[j];HMODULE sm{};char name[1024]{};
   if(address>=0x800000000ULL && address<0x804000000ULL)std::fprintf(f," guest_stack[%u]=%llx offset=%llx\n",j,(unsigned long long)address,(unsigned long long)(address-0x800000000ULL));
   if(address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)address,&sm)){
    GetModuleFileNameA(sm,name,sizeof(name));std::fprintf(f," stack[%u]=%llx %s+%llx\n",j,(unsigned long long)address,name,(unsigned long long)(address-(uintptr_t)sm));
   }
  }
 }
 std::fclose(f);return 0;
}
}
extern "C" void register_gpu_thread194(const char* name){std::scoped_lock l{mutex194};if(count194>=threads194.size())return;auto& t=threads194[count194++];t.id=GetCurrentThreadId();std::snprintf(t.name,sizeof(t.name),"%s",name);
 // bbport: for print_thread_cpu194.
 if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&t.handle,THREAD_QUERY_INFORMATION|THREAD_QUERY_LIMITED_INFORMATION,FALSE,0))t.handle=nullptr;}
// bbport (BB_FRAME_STATS): CPU time of the registered threads (guest threads, GPU command and Vulkan
// recording threads) over the last window, the busiest ones: a thread near 100% limits the frame.
extern "C" void print_thread_cpu194(double window_s){
 struct Row{double pct;const char* name;DWORD id;};std::vector<Row> rows;
 std::scoped_lock l{mutex194};
 for(unsigned i=0;i<count194;++i){auto& t=threads194[i];if(!t.handle)continue;
  FILETIME a{},b{},k{},u{};if(!GetThreadTimes(t.handle,&a,&b,&k,&u))continue;
  DWORD code=0;if(GetExitCodeThread(t.handle,&code)&&code!=STILL_ACTIVE)continue;
  const auto v=[](const FILETIME& f){return (static_cast<unsigned long long>(f.dwHighDateTime)<<32)|f.dwLowDateTime;};
  const unsigned long long cpu=v(k)+v(u);
  if(t.last_cpu)rows.push_back({(cpu-t.last_cpu)/1e5/window_s,t.name,t.id});
  t.last_cpu=cpu;}
 std::sort(rows.begin(),rows.end(),[](const Row& x,const Row& y){return x.pct>y.pct;});
 std::printf("Thread CPU %% (of one core):");
 for(size_t i=0;i<std::min<size_t>(rows.size(),12);++i)std::printf(" %s %.0f;",rows[i].name,rows[i].pct);
 std::printf("\n");std::fflush(stdout);}
extern "C" void schedule_gpu_trace194(const char* path){if(!path||!*path)return;auto p=(char*)HeapAlloc(GetProcessHeap(),0,strlen(path)+1);if(!p)return;strcpy(p,path);HANDLE t=CreateThread(0,0,capture194,p,0,0);if(t)CloseHandle(t);else HeapFree(GetProcessHeap(),0,p);}
