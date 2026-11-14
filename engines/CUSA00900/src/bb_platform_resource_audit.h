#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>

/* Read-only platform inspection. No QoS, priority, affinity, quota, feature,
 * scheduling or game/render settings are changed. Runs once/second, bounded. */
typedef HRESULT (WINAPI *BbExpandedFn)(BOOL*);
typedef HRESULT (WINAPI *BbExclusiveFn)(ULONG*);
typedef BOOL (WINAPI *BbProcessInfoFn)(HANDLE,int,LPVOID,DWORD);
typedef BOOL (WINAPI *BbThreadInfoFn)(HANDLE,int,LPVOID,DWORD);
typedef BOOL (WINAPI *BbJobInfoFn)(HANDLE,int,LPVOID,DWORD,LPDWORD);
typedef HRESULT (WINAPI *BbDescriptionFn)(HANDLE,PWSTR*);
typedef BOOL (WINAPI *BbCpuSetsFn)(HANDLE,PULONG,ULONG,PULONG);
typedef struct {ULONG Version,ControlMask,StateMask;} BbPowerState;
typedef struct {DWORD ControlFlags;DWORD CpuRate;} BbJobRate;
typedef struct {DWORD tid;uint64_t cpu,cycles;} BbAuditPrevious;
static uint64_t bb_audit_filetime(FILETIME t){return ((uint64_t)t.dwHighDateTime<<32)|t.dwLowDateTime;}
static FARPROC bb_audit_resolve(const char* name){
 const wchar_t* mods[]={L"api-ms-win-gaming-expandedresources-l1-1-0.dll",L"kernelbase.dll",L"kernel32.dll"};
 for(unsigned i=0;i<3;i++){HMODULE m=GetModuleHandleW(mods[i]);if(m){FARPROC p=GetProcAddress(m,name);if(p)return p;}}
 return NULL;
}
static DWORD WINAPI bb_platform_resource_audit_thread(void* opaque){
 char* root=opaque;char path[32768];snprintf(path,sizeof(path),"%s/platform-resource-audit.txt",root);FILE* report=fopen(path,"w");
 snprintf(path,sizeof(path),"%s/platform-resource-samples.csv",root);FILE* samples=fopen(path,"w");
 snprintf(path,sizeof(path),"%s/platform-resource-threads.csv",root);FILE* threads=fopen(path,"w");
 if(!report||!samples||!threads){if(report)fclose(report);if(samples)fclose(samples);if(threads)fclose(threads);free(root);return 1;}
 HMODULE expanded=LoadLibraryExW(L"api-ms-win-gaming-expandedresources-l1-1-0.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);DWORD expanded_error=expanded?0:GetLastError();
 BbExpandedFn has=(BbExpandedFn)bb_audit_resolve("HasExpandedResources");BbExclusiveFn count=(BbExclusiveFn)bb_audit_resolve("GetExpandedResourceExclusiveCpuCount");
 BbProcessInfoFn proc_info=(BbProcessInfoFn)bb_audit_resolve("GetProcessInformation");BbThreadInfoFn thread_info=(BbThreadInfoFn)bb_audit_resolve("GetThreadInformation");BbJobInfoFn job_info=(BbJobInfoFn)bb_audit_resolve("QueryInformationJobObject");BbDescriptionFn description=(BbDescriptionFn)bb_audit_resolve("GetThreadDescription");BbCpuSetsFn cpu_sets=(BbCpuSetsFn)bb_audit_resolve("GetThreadSelectedCpuSets");
 DWORD_PTR process_affinity=0,system_affinity=0;BOOL aff_ok=GetProcessAffinityMask(GetCurrentProcess(),&process_affinity,&system_affinity);DWORD aff_error=aff_ok?0:GetLastError();
 fprintf(report,"scope=read-only bounded90second in-process resource/QoS/job/thread observation; no setters\npid=%lu\naffinity_ok=%u\naffinity_error=%lu\nprocess_affinity=0x%llx\nsystem_affinity=0x%llx\nexpanded_module_error=%lu\nHasExpandedResources_available=%u\nGetExpandedResourceExclusiveCpuCount_available=%u\n",GetCurrentProcessId(),aff_ok,aff_error,(unsigned long long)process_affinity,(unsigned long long)system_affinity,expanded_error,has!=NULL,count!=NULL);
 SYSTEM_INFO info;GetSystemInfo(&info);fprintf(report,"reported_processors=%lu\nGetProcessInformation_available=%u\nGetThreadInformation_available=%u\nQueryInformationJobObject_available=%u\nGetThreadDescription_available=%u\n",info.dwNumberOfProcessors,proc_info!=NULL,thread_info!=NULL,job_info!=NULL,description!=NULL);
 if(job_info){JOBOBJECT_BASIC_LIMIT_INFORMATION limit={0};SetLastError(0);BOOL ok=job_info(NULL,2,&limit,sizeof(limit),NULL);DWORD error=ok?0:GetLastError();fprintf(report,"job_basic_ok=%u\njob_basic_error=%lu\njob_limit_flags=0x%lx\njob_affinity=0x%llx\njob_priority_class=0x%lx\n",ok,error,limit.LimitFlags,(unsigned long long)limit.Affinity,limit.PriorityClass);}
 fprintf(samples,"tick,expanded_hresult,expanded_granted,exclusive_hresult,exclusive_count,process_power_ok,process_power_error,process_power_control,process_power_state,process_priority_class,job_rate_ok,job_rate_error,job_rate_flags,job_rate_value,process_cpu_ms_delta\n");
 fprintf(threads,"tick,tid,name,cpu_ms_delta,cycles_delta,priority,priority_error,power_ok,power_error,power_control,power_state,selected_ok,selected_count,selected_first\n");
 BbAuditPrevious previous[512]={0};unsigned prev_count=0;uint64_t proc_previous=0;
 const uint64_t begin=GetTickCount64();
 for(unsigned sample=0;sample<90;sample++){
  BOOL granted=FALSE;ULONG exclusive=0;HRESULT hr=has?has(&granted):E_NOTIMPL;HRESULT chr=count?count(&exclusive):E_NOTIMPL;BbPowerState power={.Version=1};SetLastError(0);BOOL pok=proc_info&&proc_info(GetCurrentProcess(),4,&power,sizeof(power));DWORD perr=pok?0:GetLastError();BbJobRate rate={0};SetLastError(0);BOOL jok=job_info&&job_info(NULL,15,&rate,sizeof(rate),NULL);DWORD jerr=jok?0:GetLastError();FILETIME created,exited,kernel,user;uint64_t pcpu=0;if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))pcpu=bb_audit_filetime(kernel)+bb_audit_filetime(user);uint64_t now=GetTickCount64();
  fprintf(samples,"%llu,0x%08lx,%u,0x%08lx,%lu,%u,%lu,0x%lx,0x%lx,0x%lx,%u,%lu,0x%lx,%lu,%.6f\n",(unsigned long long)now,(unsigned long)hr,granted,(unsigned long)chr,exclusive,pok,perr,power.ControlMask,power.StateMask,GetPriorityClass(GetCurrentProcess()),jok,jerr,rate.ControlFlags,rate.CpuRate,proc_previous?(double)(pcpu-proc_previous)/10000:0);proc_previous=pcpu;
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);unsigned matched=0,opened=0;
  if(snapshot!=INVALID_HANDLE_VALUE){THREADENTRY32 entry={.dwSize=sizeof(entry)};
   if(Thread32First(snapshot,&entry))do{if(entry.th32OwnerProcessID!=GetCurrentProcessId())continue;matched++;HANDLE t=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);if(!t)continue;opened++;
    uint64_t cpu=0,cycles=0;BOOL valid=GetThreadTimes(t,&created,&exited,&kernel,&user);if(valid)cpu=bb_audit_filetime(kernel)+bb_audit_filetime(user);QueryThreadCycleTime(t,&cycles);unsigned at=0;for(;at<prev_count;at++)if(previous[at].tid==entry.th32ThreadID)break;if(at==prev_count&&prev_count<512)previous[prev_count++].tid=entry.th32ThreadID;
    char name[256]="";PWSTR wide=NULL;if(description&&SUCCEEDED(description(t,&wide))&&wide){WideCharToMultiByte(CP_UTF8,0,wide,-1,name,sizeof(name),NULL,NULL);LocalFree(wide);}for(char* c=name;*c;c++)if(*c==','||*c=='\n'||*c=='\r')*c=' ';
    SetLastError(0);int priority=GetThreadPriority(t);DWORD priority_error=priority==THREAD_PRIORITY_ERROR_RETURN?GetLastError():0;BbPowerState tp={.Version=1};SetLastError(0);BOOL tok=thread_info&&thread_info(t,3,&tp,sizeof(tp));DWORD terr=tok?0:GetLastError();ULONG ids[64]={0},required=0;BOOL sok=cpu_sets&&cpu_sets(t,ids,64,&required);
    double delta=at<512&&previous[at].cpu?(double)(cpu-previous[at].cpu)/10000:0;uint64_t cyc_delta=at<512&&previous[at].cycles?cycles-previous[at].cycles:0;
    fprintf(threads,"%llu,%lu,%s,%.6f,%llu,%d,%lu,%u,%lu,0x%lx,0x%lx,%u,%lu,%lu\n",(unsigned long long)now,entry.th32ThreadID,name,delta,(unsigned long long)cyc_delta,priority,priority_error,tok,terr,tp.ControlMask,tp.StateMask,sok,required,required?ids[0]:0);
    if(at<512){previous[at].cpu=cpu;previous[at].cycles=cycles;}CloseHandle(t);
   }while(Thread32Next(snapshot,&entry));CloseHandle(snapshot);
  }
  fprintf(report,"sample=%u tick=%llu thread_snapshot_ok=%u matched=%u opened=%u snapshot_error=%lu\n",sample,(unsigned long long)now,snapshot!=INVALID_HANDLE_VALUE,matched,opened,snapshot==INVALID_HANDLE_VALUE?GetLastError():0);fflush(report);fflush(samples);fflush(threads);
  uint64_t due=begin+(uint64_t)(sample+1)*1000;now=GetTickCount64();if(due>now)Sleep((DWORD)(due-now));
 }
 fprintf(report,"complete=1\n");fclose(threads);fclose(samples);fclose(report);if(expanded)FreeLibrary(expanded);free(root);return 0;
}
static void bb_platform_resource_audit_start(const char* root){
 char path[32768];snprintf(path,sizeof(path),"%s/platform-resource-audit.request",root);FILE* f=fopen(path,"rb");if(!f)return;fclose(f);
 char* copy=_strdup(root);if(!copy)return;HANDLE t=CreateThread(NULL,64*1024,bb_platform_resource_audit_thread,copy,0,NULL);if(t)CloseHandle(t);else free(copy);
}
