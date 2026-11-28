/* SPDX-License-Identifier: GPL-2.0-or-later
 * C ABI bridge for an in-process UWP host. No game launch is claimed here. */
#include <windows.h>
#include <winioctl.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <direct.h>
#include <objbase.h>
#include "uwp_host.h"
#include "../gpu/bbgpu.h"

int bb_uwp_runtime_main(int argc, char** argv);
void runtime_memory_reserve(void);
uint32_t runtime_thread_tcb_offset(void);
#include "bb_platform_resource_audit.h"
extern uint64_t runtime_disabled_optimizations;
static int runtime_initialized;
static char runtime_packaged_path[32768];

static int setting(const char* name, const char* value) {
    return _putenv_s(name, value) == 0 && SetEnvironmentVariableA(name, value);
}

/* XStation 2026-11-16 (test switch): pipeline-cache-namespace.txt in the title folder names another cache
 * folder (letters, digits, - and _), so a first start can be tested without the player's own cache. */
static int cache_namespace_test(const char* local) {
    char path[32768], name[80] = {0};
    snprintf(path,sizeof(path),"%s\\pipeline-cache-namespace.txt",local);
    FILE *f=fopen(path,"r");
    if(!f) return 1;
    if(!fgets(name,sizeof(name),f)) name[0]=0;
    fclose(f);
    name[strcspn(name,"\r\n ")]=0;
    for(const char *c=name;*c;++c) if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='-'||*c=='_')) return 1;
    if(!name[0]) return 1;
    printf("UWP: pipeline cache folder %s (pipeline-cache-namespace.txt)\n",name);
    return setting("BB_PIPELINE_CACHE_NAMESPACE",name);
}

__declspec(dllexport) const char* __cdecl bb_uwp_runtime_identity(void) {
    return "Bloodborne native runtime; UWP renderer integration 1; GPL-2.0-or-later";
}

/* A bounded diagnostic, separate from game boot. Compare private and shared
 * commitments in this exact process/partition, keeping at most 128 MiB live. */
static void memory_probe_snapshot(FILE *output,const char *stage,DWORD error) {
    APP_MEMORY_INFORMATION app={0};
    const BOOL queried=GetProcessInformation(GetCurrentProcess(),ProcessAppMemoryInfo,&app,sizeof(app));
    MEMORYSTATUSEX system={.dwLength=sizeof(system)};
    const BOOL global=GlobalMemoryStatusEx(&system);
    PROCESS_MEMORY_COUNTERS_EX counters={.cb=sizeof(counters)};
    K32GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&counters,sizeof(counters));
    fprintf(output,"%llu,%s,%lu,%d,%llu,%llu,%llu,%llu,%d,%llu,%llu,%llu,%llu,%llu,%llu\n",
            (unsigned long long)GetTickCount64(),stage,error,queried,
            (unsigned long long)app.AvailableCommit,(unsigned long long)app.PrivateCommitUsage,
            (unsigned long long)app.PeakPrivateCommitUsage,(unsigned long long)app.TotalCommitUsage,global,
            (unsigned long long)system.ullTotalPhys,(unsigned long long)system.ullAvailPhys,
            (unsigned long long)system.ullTotalPageFile,(unsigned long long)system.ullAvailPageFile,
            (unsigned long long)counters.WorkingSetSize,(unsigned long long)counters.PrivateUsage);
    fflush(output);
}

__declspec(dllexport) int __cdecl bb_uwp_run_memory_probe(const char *local) {
    if (!local || strlen(local)>30000) return ERROR_INVALID_PARAMETER;
    char path[32768];
    snprintf(path,sizeof(path),"%s\\memory-probe.csv",local);
    FILE *output=fopen(path,"w");
    if (!output) return ERROR_ACCESS_DENIED;
    fputs("tick_ms,stage,error,app_query,available_commit,private_commit,peak_private_commit,total_commit,global_query,physical_total,physical_available,commit_limit,commit_available,working_set,process_private\n",output);
    memory_probe_snapshot(output,"baseline",0);
    const SIZE_T step=(SIZE_T)32*1024*1024;
    int result=0;
    for (unsigned kind=0;kind<15;++kind) {
        const SIZE_T reserve=(SIZE_T)(kind==4 ? 128 : kind==6 || kind==7 ? 2048 : 6080)*1024*1024;
        const SIZE_T view_bytes=kind>=8 ? (SIZE_T)128*1024*1024 : reserve;
        const SIZE_T view_offset=kind==13 ? (SIZE_T)512*1024*1024 : 0;
        HANDLE section=NULL;
        HANDLE file=INVALID_HANDLE_VALUE;
        unsigned char *view=NULL,*alias=NULL;
        void *placeholder=NULL;
        void *extra_views[64]={0};unsigned extra_count=0;
        const char *name=kind==0 ? "private" : kind==1 ? "section-data" : kind==2 ? "section-executable" : kind==3 ? "section-from-app" : kind==4 ? "section-file-local" : kind==5 ? "section-file-data-6080" : kind==6 ? "section-file-exec-2048" : kind==7 ? "section-file-data-2048" : kind==8 ? "section-file-small-view-6080" : kind==9 ? "section-sparse-file-small-view" : kind==10 ? "section-sparse-placeholder" : kind==11 ? "section-file-placeholder" : kind==12 ? "section-file-many-views" : kind==13 ? "section-file-high-offset" : "section-file-many-aliases";
        char stage[96];
        DWORD error=0;
        if (!kind) view=VirtualAlloc(NULL,reserve,MEM_RESERVE,PAGE_READWRITE);
        else {
            if (kind>=4) {
                wchar_t filename[32768];
                MultiByteToWideChar(CP_UTF8,0,kind==4 ? local : runtime_packaged_path,-1,filename,32700);
                wcscat(filename,L"\\memory-probe-backing.tmp");
                CREATEFILE2_EXTENDED_PARAMETERS params={.dwSize=sizeof(params),
                    .dwFileAttributes=FILE_ATTRIBUTE_TEMPORARY,.dwFileFlags=FILE_FLAG_DELETE_ON_CLOSE};
                file=CreateFile2(filename,GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,0,CREATE_ALWAYS,&params);
                error=file==INVALID_HANDLE_VALUE ? GetLastError() : 0;
                snprintf(stage,sizeof(stage),"%s-open",name);memory_probe_snapshot(output,stage,error);
                if (!error && (kind==9 || kind==10)) {
                    DWORD returned=0;
                    if (!DeviceIoControl(file,FSCTL_SET_SPARSE,NULL,0,NULL,0,&returned,NULL)) error=GetLastError();
                    snprintf(stage,sizeof(stage),"%s-sparse",name);memory_probe_snapshot(output,stage,error);
                }
                LARGE_INTEGER length={.QuadPart=reserve};
                if (!error && (!SetFilePointerEx(file,length,NULL,FILE_BEGIN) || !SetEndOfFile(file))) error=GetLastError();
                snprintf(stage,sizeof(stage),"%s-length",name);memory_probe_snapshot(output,stage,error);
                if (!error) {
                    section=CreateFileMappingFromApp(file,NULL,kind==5 || kind==7 ? PAGE_READWRITE : PAGE_EXECUTE_READWRITE,reserve,NULL);
                    if (!section) error=GetLastError();
                    snprintf(stage,sizeof(stage),"%s-section",name);memory_probe_snapshot(output,stage,error);
                }
            }
            else if (kind==3) section=CreateFileMappingFromApp(INVALID_HANDLE_VALUE,NULL,
                                    PAGE_EXECUTE_READWRITE|SEC_RESERVE,reserve,NULL);
            else section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,
                                       kind==1 ? PAGE_READWRITE : PAGE_EXECUTE_READWRITE,
                                       SEC_RESERVE,reserve,NULL,NULL,0);
            if (!section) { if (!error) error=GetLastError(); }
            else {
                if (kind==10 || kind==11) {
                    placeholder=VirtualAlloc2(NULL,NULL,view_bytes,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0);
                    if (!placeholder) error=GetLastError();
                }
                if (!error) view=MapViewOfFile3(section,GetCurrentProcess(),placeholder,view_offset,view_bytes,placeholder ? MEM_REPLACE_PLACEHOLDER : 0,PAGE_READWRITE,NULL,0);
            }
        }
        if (!view && !error) error=GetLastError();
        snprintf(stage,sizeof(stage),"%s-reserved",name);
        memory_probe_snapshot(output,stage,error);
        if (error || !view) result=1;
        if (kind>=12 && view) {
            for (SIZE_T offset=view_bytes;offset<(kind==14 ? view_bytes*6 : kind==13 ? view_bytes*2 : reserve);offset+=view_bytes) {
                const SIZE_T bytes=reserve-offset<view_bytes ? reserve-offset : view_bytes;
                const SIZE_T physical=kind==14 ? 0 : kind==13 ? reserve-view_bytes : offset;
                void *extra=MapViewOfFile3(section,GetCurrentProcess(),NULL,physical,bytes,0,PAGE_READWRITE,NULL,0);
                error=extra ? 0 : GetLastError();
                snprintf(stage,sizeof(stage),"%s-window-%u-offset-%u-MiB",name,extra_count+1,(unsigned)(physical/(1024*1024)));
                memory_probe_snapshot(output,stage,error);
                if (!extra) { result=1;break; }
                extra_views[extra_count++]=extra;
            }
        }
        SIZE_T committed=0;
        for (unsigned i=0;view && !error && i<4;++i) {
            if (kind<4 && !VirtualAlloc(view+committed,step,MEM_COMMIT,PAGE_READWRITE)) {
                error=GetLastError(); result=1;
            } else {
                /* Fault real pages in; merely reserving address space is not RAM use. */
                volatile unsigned char *touch=view+committed;
                for (SIZE_T page=0;page<step;page+=4096) touch[page]=0x5a;
                committed+=step;
            }
            snprintf(stage,sizeof(stage),"%s-touched-%u-MiB",name,(unsigned)(committed/(1024*1024)));
            memory_probe_snapshot(output,stage,error);
            if (error) break;
        }
        if (section && committed) {
            alias=MapViewOfFile3(section,GetCurrentProcess(),NULL,view_offset,committed,0,PAGE_READWRITE,NULL,0);
            error=alias ? 0 : GetLastError();
            if (alias) {
                alias[0]=0xa7;
                if (view[0]!=0xa7 || alias[committed-4096]!=0x5a) { error=ERROR_INVALID_DATA; result=1; }
            } else result=1;
            snprintf(stage,sizeof(stage),"%s-alias",name);
            memory_probe_snapshot(output,stage,error);
        }
        if (alias && !UnmapViewOfFile(alias)) result=1;
        for (unsigned n=0;n<extra_count;++n) if (!UnmapViewOfFile(extra_views[n])) result=1;
        if (view) {
            if (section) { if (!UnmapViewOfFile(view)) result=1; }
            else if (!VirtualFree(view,0,MEM_RELEASE)) result=1;
        }
        if (section && !CloseHandle(section)) result=1;
        if (placeholder && !view && !VirtualFree(placeholder,0,MEM_RELEASE)) result=1;
        if (file!=INVALID_HANDLE_VALUE && !CloseHandle(file)) result=1;
        snprintf(stage,sizeof(stage),"%s-released",name);
        memory_probe_snapshot(output,stage,0);
    }
    {
        HANDLE files[48],sections[48]={0};void *views[48]={0};
        for (unsigned n=0;n<48;++n) files[n]=INVALID_HANDLE_VALUE;
        for (unsigned n=0;n<48;++n) {
            char filename[32768],stage[96];wchar_t wide[32768];DWORD error=0;
            snprintf(filename,sizeof(filename),"%s\\memory-independent-%u.tmp",runtime_packaged_path,n);
            MultiByteToWideChar(CP_UTF8,0,filename,-1,wide,32768);
            CREATEFILE2_EXTENDED_PARAMETERS options={.dwSize=sizeof(options),.dwFileAttributes=FILE_ATTRIBUTE_TEMPORARY,.dwFileFlags=FILE_FLAG_DELETE_ON_CLOSE};
            files[n]=CreateFile2(wide,GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,0,CREATE_ALWAYS,&options);
            if (files[n]==INVALID_HANDLE_VALUE) error=GetLastError();
            LARGE_INTEGER length={.QuadPart=128*1024*1024};
            if (!error && (!SetFilePointerEx(files[n],length,NULL,FILE_BEGIN) || !SetEndOfFile(files[n]))) error=GetLastError();
            if (!error) { sections[n]=CreateFileMappingFromApp(files[n],NULL,PAGE_EXECUTE_READWRITE,length.QuadPart,NULL);if (!sections[n]) error=GetLastError(); }
            if (!error) { views[n]=MapViewOfFile3(sections[n],GetCurrentProcess(),NULL,0,length.QuadPart,0,PAGE_READWRITE,NULL,0);if (!views[n]) error=GetLastError(); }
            if (!error) *(volatile unsigned char*)views[n]=0x5a;
            snprintf(stage,sizeof(stage),"independent-file-window-%u",n+1);
            memory_probe_snapshot(output,stage,error);
            if (error) { result=1;break; }
        }
        for (unsigned n=0;n<48;++n) {
            if (views[n]) UnmapViewOfFile(views[n]);
            if (sections[n]) CloseHandle(sections[n]);
            if (files[n]!=INVALID_HANDLE_VALUE) CloseHandle(files[n]);
        }
        memory_probe_snapshot(output,"independent-files-released",0);
    }
    memory_probe_snapshot(output,"complete",result);
    fclose(output);
    return result;
}

__declspec(dllexport) int __cdecl bb_uwp_initialize(const char* local, const char* packaged) {
    if (!local || !packaged || strlen(local) > 30000 || strlen(packaged) > 30000) return 87;
    if (runtime_initialized) return 0;
    strcpy(runtime_packaged_path,packaged);
    /* Reversible Xbox A/B: reuse the existing atomic toggle watcher. Only
     * PoolSmallCopies bit524288 is changed by the test control. Default0
     * retains the production recorder route; dependencies remain unchanged. */
    char copy_toggle_path[32768];
    snprintf(copy_toggle_path,sizeof(copy_toggle_path),"%s\\copy-batching-20261026.txt",local);
    if (!setting("BB_TOGGLE_FILE",copy_toggle_path)) return 5;

    char output[32768], error[32768], icd[32768], user[32768];
    if (snprintf(output, sizeof(output), "%s\\runtime-stdout.log", local) < 0 ||
        snprintf(error, sizeof(error), "%s\\runtime-stderr.log", local) < 0 ||
        snprintf(icd, sizeof(icd), "%s\\dzn_icd.x86_64.json", packaged) < 0 ||
        snprintf(user, sizeof(user), "%s\\runtime-user", local) < 0) return 87;
    if (!freopen(output, "w", stdout) || !freopen(error, "w", stderr)) return 5;
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    /* Graphics fixtures need the initial route before the guest GPU hook
     * installs the live watcher. Accept only this single A/B bit. */
    unsigned long long copy_toggle_value=0;
    FILE *copy_toggle_file=fopen(copy_toggle_path,"r");
    if(copy_toggle_file) { fscanf(copy_toggle_file,"%llu",&copy_toggle_value); fclose(copy_toggle_file); }
    if(copy_toggle_value!=0 && copy_toggle_value!=524288) return 87;
    __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)copy_toggle_value,__ATOMIC_RELEASE);
    printf("Runtime: disabled optimizations mask=%llu\n",copy_toggle_value);

    if (!setting("VK_DRIVER_FILES", icd) || !setting("VK_LOADER_LAYERS_DISABLE", "~implicit~") ||
        !setting("BB_BUFFER_ARENAS", "dense") || !setting("BB_COMPACT_MEMORY", "1") ||
        !setting("BB_DIRECT_MEMORY_ACCESS", "0") || !setting("BB_GPU_USER_DIR", user) ||
        !setting("BB_GPU_TEST_NO_VALIDATION", "1") || !setting("VK_LOADER_DEBUG", "all") ||
        !setting("BB_UWP_LAZY_RESERVATION", "1") || !setting("BB_UWP_DEMAND_COMMIT", "1") ||
        !setting("BB_UWP_TOUCH_COMMIT", "1") || !setting("BB_UWP_TOUCH_CPU_MIN_MB", "0") ||
        !setting("BB_UWP_PRIVATE_CPU","0") || !setting("BB_UWP_PRIVATE_GPU","0") ||
        !setting("BB_VMA_BLOCK_MB","16") || !setting("BB_GC_BUDGET_MB","2048") ||
        !setting("BB_GC_IDLE_SECONDS","2") || !setting("BB_GC_DROP_TILED","0") ||
        !setting("BB_CLEAN_GC_THRESHOLD_MB","3840") || !setting("BB_STREAM_HEADROOM_MB","384") ||
        !setting("BB_STAGING_KEEP_MB","16") ||
        !setting("BB_STAGING_RING_GRACE_FRAMES","0") ||
        !setting("BB_STAGING_PRESSURE_MB","32") || !setting("BB_UPLOAD_SUBMIT_MB","32") ||
        !setting("BB_MEMORY_REPORT_SECONDS","5") || !setting("BB_FPS_STATS","1") ||
        !setting("BB_TRIM_ZERO_PRIVATE","0") ||
        !setting("BB_BUFFER_CLEAN_GC_MB","3840") || !setting("BB_BUFFER_DRAIN_MB","0") ||
        !setting("BB_GUEST_MEMORY_REPORT","0") || !setting("BB_HOST_HEAP_TRIM","1") ||
        !setting("BB_UWP_HOST_PAGE_TOUCH","1") || !setting("BB_MEMORY_ALLOCATION_TRACE","0") ||
        !setting("BB_ARENA_RETIRE_MB","32") || !setting("BB_BUFFER_CLEAN_IDLE_MS","2000") ||
        !setting("BB_BUFFER_WRITEBACK_GC","1")) return 5;
    /* Opt-in PC profiling: correlate named VMA allocations with OS tracing.
     * Keep this off in normal gameplay; detailed allocator walks cost time. */
    char profile_path[32768];
    snprintf(profile_path,sizeof(profile_path),"%s\\fault-window.txt",local);
    FILE *fault_window_file=fopen(profile_path,"r");
    unsigned fault_window_kib=256;
    if(fault_window_file) {
        unsigned requested=0;
        if(fscanf(fault_window_file,"%u",&requested)==1 && requested==1024)fault_window_kib=1024;
        fclose(fault_window_file);
    }
    if(!setting("BB_FAULT_WINDOW",fault_window_kib==1024 ? "1024" : "256"))return 5;
    printf("UWP: write-fault coalescing window=%u KiB\n",fault_window_kib);
    snprintf(profile_path,sizeof(profile_path),"%s\\gpu-profile.txt",local);
    FILE *gpu_profile_file=fopen(profile_path,"r");
    int gpu_profile_enabled=0;
    if(gpu_profile_file) {
        gpu_profile_enabled=fgetc(gpu_profile_file)=='1';
        fclose(gpu_profile_file);
    }
    if(!setting("BB_GPU_PROFILE",gpu_profile_enabled ? "1" : "0"))return 5;
    if(gpu_profile_enabled)puts("UWP: GPU timestamp profiling enabled; timing overhead applies");
    snprintf(profile_path,sizeof(profile_path),"%s\\cpu-profile.txt",local);
    FILE *cpu_profile_file=fopen(profile_path,"r");
    int cpu_profile_enabled=0;
    if(cpu_profile_file) {
        cpu_profile_enabled=fgetc(cpu_profile_file)=='1';
        fclose(cpu_profile_file);
    }
    if(!setting("BB_CPU_PROFILE",cpu_profile_enabled ? "1" : "0"))return 5;
    if(cpu_profile_enabled)puts("UWP: aggregate CPU renderer timers enabled; overlapping thread times are not a critical-path breakdown");
    snprintf(profile_path,sizeof(profile_path),"%s\\descriptor-updates.txt",local);
    FILE *descriptor_file=fopen(profile_path,"r");
    int deferred_descriptors=0;
    if(descriptor_file) {
        deferred_descriptors=fgetc(descriptor_file)=='1';
        fclose(descriptor_file);
    }
    if(!setting("BB_DEFER_DESCRIPTOR_UPDATES",deferred_descriptors ? "1" : "0"))return 5;
    printf("UWP: descriptor updates=%s\n",deferred_descriptors ? "recording-thread experiment" : "immediate");
    snprintf(profile_path,sizeof(profile_path),"%s\\recorder-priority.txt",local);
    FILE *priority_file=fopen(profile_path,"r");
    int recorder_priority=0;
    if(priority_file) {
        recorder_priority=fgetc(priority_file)=='1';
        fclose(priority_file);
    }
    if(!setting("BB_VK_RECORD_PRIORITY",recorder_priority ? "1" : "0"))return 5;
    snprintf(profile_path,sizeof(profile_path),"%s\\memory-profile.txt",local);
    FILE *profile_file=fopen(profile_path,"r");
    if(profile_file) {
        const int mode=fgetc(profile_file);fclose(profile_file);
        const int enabled=mode=='1' || mode=='2';
        if(enabled) {
            if(!setting("BB_GUEST_MEMORY_REPORT","1") || !setting("BB_MEMORY_ALLOCATION_TRACE","1"))return 5;
            snprintf(profile_path,sizeof(profile_path),"%s\\memory-snapshots",local);
            if(_mkdir(profile_path) && errno!=EEXIST)return 5;
            if(!setting("BB_MEMORY_SNAPSHOT_DIR",profile_path))return 5;
            puts("UWP: detailed GPU memory snapshots enabled (profiling overhead applies)");
            if(mode=='2') {
                if(!setting("BB_BUFFER_WRITEBACK_GC","0"))return 5;
                puts("UWP: PC profiling baseline excludes cold GPU-buffer writeback after NVIDIA readback regression; both comparison runs use this baseline");
            }
        }
    }
    snprintf(profile_path,sizeof(profile_path),"%s\\memory-ledger.txt",local);
    profile_file=fopen(profile_path,"r");
    if(profile_file) {
        const int enabled=fgetc(profile_file)=='1';fclose(profile_file);
        if(enabled) {
            if(!setting("BB_GUEST_MEMORY_REPORT","1") || !setting("BB_MEMORY_ALLOCATION_TRACE","1"))return 5;
            snprintf(profile_path,sizeof(profile_path),"%s\\memory-ledger.csv",local);
            if(!setting("BB_MEMORY_LEDGER",profile_path) || !setting("BB_MEMORY_REPORT_MS","1000"))return 5;
            puts("UWP: allocation lifetime ledger enabled; same memory and graphics settings, one-second owner totals");
        }
    }
    char backend_path[32768],backend[32]={0};
    snprintf(backend_path,sizeof(backend_path),"%s\\stream-drain-backoff.txt",local);
    FILE *stream_policy_file=fopen(backend_path,"r");
    if(stream_policy_file) {
        const int enabled=fgetc(stream_policy_file)=='1';fclose(stream_policy_file);
        if(!setting("BB_STREAM_DRAIN_BACKOFF",enabled ? "1" : "0"))return 5;
        printf("UWP: streaming drain retry backoff=%d\n",enabled);
    }
    snprintf(backend_path,sizeof(backend_path),"%s\\arena-growth-padding.txt",local);
    FILE *arena_policy_file=fopen(backend_path,"r");
    if(arena_policy_file) {
        const int enabled=fgetc(arena_policy_file)=='1';fclose(arena_policy_file);
        if(!setting("BB_DENSE_GROWTH_PADDING",enabled ? "1" : "0"))return 5;
        printf("UWP: bounded dense arena growth padding=%d (8 MiB active/pending budget)\n",enabled);
    }
    snprintf(backend_path,sizeof(backend_path),"%s\\memory-backend.txt",local);
    FILE *backend_file=fopen(backend_path,"r");
    if (backend_file) { fgets(backend,sizeof(backend),backend_file); fclose(backend_file); }
    if (!strncmp(backend,"file",4)) {
        snprintf(backend_path,sizeof(backend_path),"%s\\guest-memory.tmp",packaged);
        if (!setting("BB_UWP_FILE_BACKING",backend_path) || !setting("BB_UWP_DEMAND_COMMIT","0") ||
            !setting("BB_UWP_TOUCH_COMMIT","0")) return 5;
        printf("UWP: experimental file-backed guest memory selected; app commit alone excludes mapped-file residency\n");
    }
    if (!strncmp(backend,"private",7)) {
        if(!setting("BB_UWP_PRIVATE_CPU","1"))return 5;
        puts("UWP: private CPU heaps selected; real physical aliases retain shared backing");
        if (!strncmp(backend,"private-gpu",11)) {
            if(!setting("BB_UWP_PRIVATE_GPU","1"))return 5;
            puts("UWP: single-mapped GPU heaps also use private backing; aliases and protected readback promote affected chunks");
        }
    }
    snprintf(backend_path,sizeof(backend_path),"%s\\gpu-diagnostics.txt",local);
    backend_file=fopen(backend_path,"r");
    if(backend_file) {
        const int mode=fgetc(backend_file);fclose(backend_file);
        if(!setting("DZN_PRIVATE_DRED",mode=='1' || mode=='2' ? "1" : "0") ||
           !setting("DZN_PRIVATE_DRED_CAPTURE",mode=='3' ? "1" : "0") ||
           !setting("DZN_PRIVATE_NATIVE_DEBUG",mode=='2' ? "1" : "0"))return 5;
    }
    printf("UWP native runtime initialized; renderer tests and game startup are separate entry points.\n");
    printf("Validation layer is not packaged; desktop validation runs are recorded separately.\n");
    /* Guest pointers must fit the PS4's 40-bit range. Reserve virtual address
     * space before the graphics driver places its own mappings there. This
     * does not commit or allocate the entire guest memory pool. */
    unsigned conflicts = 0;
    for (uintptr_t address = UINT64_C(0x1000000000); address < UINT64_C(0xfc00000000);) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void*)address, &info, sizeof(info))) break;
        if (info.State != MEM_FREE && conflicts++ < 8)
            fprintf(stderr, "Guest range occupied before reservation: base=%p, bytes=%llu, state=0x%lx, type=0x%lx\n",
                    info.BaseAddress, (unsigned long long)info.RegionSize, info.State, info.Type);
        const uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next <= address) break;
        address = next;
    }
    runtime_memory_reserve();
    printf("Guest TCB slot reserved at TEB offset 0x%x\n",runtime_thread_tcb_offset());
    runtime_initialized = 1;
    printf("Guest virtual reservation policy initialized before renderer startup.\n");
    return 0;
}

typedef struct { int argc; char **argv; int result; } GameStart;
static DWORD WINAPI game_thread(void *opaque) {
    GameStart *start=opaque;
    const HRESULT initialized=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    ULONG_PTR low=0, high=0;
    GetCurrentThreadStackLimits(&low,&high);
    printf("UWP game thread: stack=%p..%p, bytes=%llu, MTA=0x%lx\n",
           (void*)low,(void*)high,(unsigned long long)(high-low),(unsigned long)initialized);
    start->result=bb_uwp_runtime_main(start->argc,start->argv);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return (DWORD)start->result;
}

__declspec(dllexport) int __cdecl bb_uwp_run_game(const char *local,const char *packaged,
                                                  const BbUwpHost *host,int cpu_only) {
    int result=bb_uwp_initialize(local,packaged);
    if (result) return result;
    char boot[32768],content[32768],patches[32768],game[32768],user[32768],config[32768];
    char layout_path[32768],layout[32]={0};
    snprintf(layout_path,sizeof(layout_path),"%s\\game-layout.txt",local);
    FILE *layout_file=fopen(layout_path,"r");
    if(layout_file) { fgets(layout,sizeof(layout),layout_file);fclose(layout_file); }
    const int update_109=!strncmp(layout,"109",3);
    bb_platform_resource_audit_start(local);
    if (update_109) {
        // See docs/WINDOWS_TROUBLESHOOTING.md: keep submitted commands alive
        // while decoded, and use the serial binding path for the 1.09 test.
        if (!setting("BB_COPY_GPU_BUFFERS","1") || !setting("BB_DRAW_PIPE","1") ||
            !setting("BB_PREP_WORKERS","3")) return 5;
        puts("UWP: 1.09 ordered parallel submissions: command snapshots on, draw pipeline on, 3 preparation workers; retirement publication synchronized");
    }
    const char *boot_folder=update_109 ? "GameBoot109" : "GameBoot";
    char scene_path[32768],scene[32]={0};
    snprintf(scene_path,sizeof(scene_path),"%s\\scene-resolution.txt",local);
    FILE *scene_file=fopen(scene_path,"r");
    if(scene_file) { fgets(scene,sizeof(scene),scene_file);fclose(scene_file); }
    const int scene_540=update_109 && !strncmp(scene,"540p",4);
    const char *render_res=scene_540 ? "960x540" : "1280x720";
    if(!setting("BB_RENDER_RES",update_109 ? render_res : "") ||
       !setting("BB_OUTPUT_RES",update_109 ? "1920x1080" : ""))return 5;
    printf("UWP: selected game layout %s; scene %s\n",update_109 ? "1.09" : "base 1.00",update_109 ? render_res : "1920x1080");
    snprintf(boot,sizeof(boot),"%s\\%s\\boot-linked.bin",packaged,boot_folder);
    char guarded_boot[32768];
    snprintf(guarded_boot,sizeof(guarded_boot),"%s\\%s\\boot-memory.bin",packaged,boot_folder);
    FILE *guarded=fopen(guarded_boot,"rb");
    if (guarded) {
        fclose(guarded);
        strcpy(boot,guarded_boot);
        if (!setting("BB_UWP_TOUCH_CPU_MIN_MB","128")) return 5;
        printf("UWP: using private version-matched red-zone guarded boot image for CPU first-touch testing\n");
    }
    snprintf(content,sizeof(content),"%s\\%s\\content.bin",packaged,boot_folder);
    snprintf(patches,sizeof(patches),"%s\\%s\\%s",packaged,boot_folder,scene_540 ? "patches-540p.bin" : "patches.bin");
    if(scene_540) {
        FILE *scene_patch=fopen(patches,"rb");
        if(!scene_patch) { fputs("UWP: selected 540p scene requires its matching prepared patches\n",stderr);return 2; }
        fclose(scene_patch);
    }
    snprintf(game,sizeof(game),"%s\\%s",packaged,update_109 ? "GameData109" : "GameData");
    if(!setting("BB_APP0_DIR",game))return 5; /* XStation 2026-11-16: the cache recipe reads the game's shaders */
    char save_profile_path[32768],save_profile[32]={0};
    snprintf(save_profile_path,sizeof(save_profile_path),"%s\\save-profile.txt",local);
    FILE *save_profile_file=fopen(save_profile_path,"r");
    if(save_profile_file) { fgets(save_profile,sizeof(save_profile),save_profile_file);fclose(save_profile_file); }
    const int isolated_save=!strncmp(save_profile,"newgame-test",12);
    snprintf(user,sizeof(user),"%s\\%s",local,isolated_save ? "game-user-newgame-test" : "game-user");
    if (_mkdir(user) && errno!=EEXIST) return 5;
    printf("UWP: save profile %s\n",isolated_save ? "isolated new-game diagnostic (main characters preserved)" : "main");
    char driver_log[32768];snprintf(driver_log,sizeof(driver_log),"%s\\driver-diagnostics.log",user);
    FILE *driver_output=fopen(driver_log,"w");
    if(driver_output) { fprintf(driver_output,"New UWP game run at tick %llu\n",(unsigned long long)GetTickCount64());fclose(driver_output); }
    snprintf(config,sizeof(config),"%s\\bbport.ini",local);
    // One-shot default-name preset for an explicitly requested developer retest.
    // Normal launches retain the native Xbox keyboard path.
    char ime_test_path[32768],ime_test[32]={0};
    snprintf(ime_test_path,sizeof(ime_test_path),"%s\\ime-test.txt",local);
    FILE *ime_test_file=fopen(ime_test_path,"r");
    if(ime_test_file) {
        fgets(ime_test,sizeof(ime_test),ime_test_file);fclose(ime_test_file);remove(ime_test_path);
        ime_test[strcspn(ime_test,"\r\n")]=0;
        size_t n=strlen(ime_test);int valid=n>0 && n<=16;
        for(size_t i=0;i<n;++i)if(!((ime_test[i]>='A'&&ime_test[i]<='Z') || (ime_test[i]>='a'&&ime_test[i]<='z') || (ime_test[i]>='0'&&ime_test[i]<='9') || ime_test[i]==' '))valid=0;
        if(valid) { if(!setting("BB_IME_TEXT",ime_test))return 5;puts("UWP: one-shot developer name preset selected for loading reproduction"); }
    }
    char capture_dir[32768],capture_trigger[32768];
    snprintf(capture_dir,sizeof(capture_dir),"%s\\frame-captures",user);
    snprintf(capture_trigger,sizeof(capture_trigger),"%s\\frame-capture.txt",local);
    if (_mkdir(capture_dir) && errno!=EEXIST) return 5;
    if (!setting("BB_FRAME_DUMP_TRIGGER",capture_trigger) || !setting("BB_DUMP_DIR",capture_dir)) return 5;
    /* XStation 2026-11-21 (capture_env.py): the frame analyzer: frame-analyze.txt records the next frame's passes. */
    {
        char analyze_trigger[32768];
        snprintf(analyze_trigger,sizeof(analyze_trigger),"%s\\frame-analyze.txt",local);
        if (!setting("BB_CAPTURE_TRIGGER",analyze_trigger) || !setting("BB_CAPTURE_DIR",capture_dir) ||
            !setting("BB_DIAGNOSTIC_IMAGE_DIR",capture_dir)) return 5;
    }
    FILE *file=fopen(boot,"rb");
    if (!file) { fprintf(stderr,"UWP: prepared game image is not readable: %s (%d)\n",boot,errno); return 2; }
    fclose(file);
    if (_chdir(local) || !setting("BB_GPU_USER_DIR",user) || !setting("BB_USER_DIR",user) ||
        !setting("BB_CONFIG",config) || !setting("BB_FPS_LIMIT","0") || !setting("BB_VBLANK_HZ","0") ||
        !setting("BB_OBJECT_MOTION","0") ||
        !setting("BB_UPLOAD_SUBMIT_MB","32") ||
        !setting("BB_ONLINE","0") || !setting("BB_PIPELINE_CACHE_NAMESPACE","uwp-mesa-startup-v1") || !cache_namespace_test(local) || !setting("BB_VK_VALIDATION","0") ||
        !setting("BB_DUMP_SHADERS","0") || !setting("BB_TRACE_MUTEX","0") || !setting("VK_LOADER_DEBUG","error,warn")) return 5;
    {
        /* bbport (Xbox, 2026-10-27): frame-limit.txt selects a paced 30 FPS mode at launch. */
        char limit_path[32768], mode[16] = {0};
        snprintf(limit_path,sizeof(limit_path),"%s\\frame-limit.txt",local);
        FILE *limit_file=fopen(limit_path,"r");
        if(limit_file) { if(!fgets(mode,sizeof(mode),limit_file)) mode[0]=0; fclose(limit_file); }
        if(!strncmp(mode,"30sw",4)) {
            if(!setting("BB_FPS_LIMIT","30"))return 5;
            puts("UWP: frame limit 30 FPS, software limiter only (comparison)");
        } else if(!strncmp(mode,"30",2)) {
            if(!setting("BB_FPS_LIMIT","30") || !setting("BB_PRESENT_MODE","Fifo") ||
               !setting("BB_DXGI_SYNC_INTERVAL","2"))return 5;
            puts("UWP: frame limit 30 FPS, display-locked (FIFO, DXGI sync interval 2)");
        } else {
            puts("UWP: frame limit off (uncapped)");
        }
    }
    char cache_setting_path[32768];
    snprintf(cache_setting_path,sizeof(cache_setting_path),"%s\\pipeline-cache.txt",local);
    FILE *cache_setting=fopen(cache_setting_path,"r");
    const int cache_enabled=!cache_setting || fgetc(cache_setting)!='0';
    if(cache_setting)fclose(cache_setting);
    if(!setting("BB_PIPELINE_CACHE",cache_enabled ? "1" : "0"))return 5;
    printf("UWP: persistent shader/pipeline cache %s; learned pipelines warm up before game startup\n",
           cache_enabled ? "enabled" : "disabled");
    char mutex_trace_path[32768];
    snprintf(mutex_trace_path,sizeof(mutex_trace_path),"%s\\mutex-trace.txt",local);
    FILE *mutex_trace=fopen(mutex_trace_path,"r");
    if(mutex_trace) {
        const int enabled=fgetc(mutex_trace)=='1';fclose(mutex_trace);
        if(enabled && !setting("BB_TRACE_MUTEX","1"))return 5;
        if(enabled)puts("UWP: mutex lifecycle tracing enabled");
    }
    if (!cpu_only) {
        result=bbgpu_uwp_set_host(host);
        if (result) return result;
        HMODULE driver=LoadPackagedLibrary(L"vulkan_dzn.dll",0);
        if (!driver) return (int)GetLastError();
        typedef HRESULT (__cdecl *SetPresenter)(uint32_t,void*,void(*)(void*,uint32_t*,uint32_t*),int32_t(*)(void*,void*));
        SetPresenter present=(SetPresenter)GetProcAddress(driver,"dzn_uwp_set_presenter");
        if (!present) { fprintf(stderr,"UWP: Mesa driver lacks the required composition presenter ABI\n"); return 127; }
        HRESULT attached=present(1,host->context,host->extent,host->attach_swapchain);
        if (FAILED(attached)) return (int)attached;
        printf("UWP: private Mesa composition presenter ABI 1 configured\n");
    }
    char presentation_path[32768];snprintf(presentation_path,sizeof(presentation_path),"%s\\presentation-test.txt",local);
    FILE *presentation=fopen(presentation_path,"r");
    if(presentation) {
        fclose(presentation);remove(presentation_path);
        extern void bb_uwp_install_unhandled_report(void);
        bb_uwp_install_unhandled_report();
        if(!setting("BB_PRESENTATION_SMOKE","1"))return 5;
        const BbGpuConfig config={.title="Bloodborne UWP presentation diagnostic",.serial="CUSA00900",.user_dir=user,
                                  .sdk_version=0x02000071,.width=1920,.height=1080};
        const int initialized=bbgpu_init(&config);
        return initialized ? initialized : bbgpu_presentation_test();
    }
    printf("UWP: starting owned prepared game image in this AppContainer; cpu_only=%d\n",cpu_only);
    char name[]="BloodborneUwpGame",profile[]="--content-profile",patch[]="--patches",app[]="--app0",usr[]="--user",timeout[]="--timeout",zero[]="0",cpu[]="--cpu-only";
    char *args[]={name,boot,profile,content,patch,patches,app,game,usr,user,timeout,zero,cpu,NULL};
    GameStart start={cpu_only ? 13 : 12,args,-1};
    HANDLE thread=CreateThread(NULL,16*1024*1024,game_thread,&start,0,NULL);
    if (!thread) return (int)GetLastError();
    DWORD waited=WaitForSingleObject(thread,INFINITE);
    CloseHandle(thread);
    return waited==WAIT_OBJECT_0 ? start.result : (int)GetLastError();
}

__declspec(dllexport) int __cdecl bb_uwp_run_renderer_tests(const char* local, const char* packaged) {
    const int initialized = bb_uwp_initialize(local, packaged);
    if (initialized) return initialized;
    char path[32768], mode[64] = {0}, stage[16] = {0};
    snprintf(path,sizeof(path),"%s\\graphics-probe.txt",local);
    FILE *probe=fopen(path,"r");
    if (probe) {
        if (fgets(mode,sizeof(mode),probe)) mode[strcspn(mode,"\r\n")]=0;
        if (fgets(stage,sizeof(stage),probe)) stage[strcspn(stage,"\r\n")]=0;
        fclose(probe);
        remove(path);
        if (!strcmp(mode,"barriers") || !strcmp(mode,"clear") || !strcmp(mode,"draw") ||
            !strcmp(mode,"clear-no-depth") || !strcmp(mode,"draw-no-depth")) {
            if (!setting("BB_GPU_GRAPHICS_PROBE",mode)) return 5;
            const int stage_number=atoi(stage);
            if (stage_number>0 && stage_number<=11) {
                char selected[16];
                snprintf(selected,sizeof(selected),"%d",stage_number);
                if (!setting("DZN_PRIVATE_DRAW_STAGE",selected)) return 5;
                printf("Diagnostic graphics-state prefix %d; actual draw omitted\n",stage_number);
            }
            printf("Diagnostic graphics subset: %s; successful subset returns 77, not full-suite success\n",mode);
        }
    }
    char name[] = "BloodborneUwpRuntime", option[] = "--buffer-arena-test";
    char* args[] = {name, option, NULL};
    return bb_uwp_runtime_main(2, args);
}
