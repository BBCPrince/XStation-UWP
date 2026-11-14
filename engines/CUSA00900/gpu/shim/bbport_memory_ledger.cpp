// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_memory_ledger.h"
#include "bbport_process_memory.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace BbMemoryLedger {
namespace {
thread_local char owner[192] = "unlabelled";
std::mutex output_mutex;
std::uint64_t sequence{}, written{};
constexpr std::uint64_t limit = 64ull * 1024 * 1024;
bool capped{};
FILE* Output() {
    static FILE* output=[] {
        const char* path=std::getenv("BB_MEMORY_LEDGER");
        FILE* file=path && *path ? std::fopen(path,"wb") : nullptr;
        if(file) {
            std::setvbuf(file,nullptr,_IONBF,0);
            std::fputs("sequence,tick_ms,action,domain,id,parent,requested_bytes,backing_bytes,offset,memory_type,detail,owner\n",file);
            std::puts("Memory ledger: enabled;64MiB cap; resource bytes are contained in VMA blocks, do not add both; app-memory.csv remains total app accounting");
        }
        return file;
    }();
    return output;
}
void Write(const char* action,const char* domain,std::uint64_t id,std::uint64_t parent,
           std::uint64_t requested,std::uint64_t size,std::uint64_t offset,
           unsigned type,std::uint64_t detail,const char* label) {
    if(!Enabled())return;
    char clean[192];std::size_t n=0;
    for(;label && label[n] && n<sizeof(clean)-1;++n) {
        const unsigned char c=label[n];
        clean[n]=(c<32 || c==',' || c=='"') ? ' ' : char(c);
    }
    clean[n]=0;
    std::scoped_lock lock{output_mutex};
    if(capped)return;
    if(written>=limit) {
        std::fprintf(Output(),"%llu,%llu,truncated,coverage,0,0,0,0,0,0,0,64MiB event cap reached\n",
            (unsigned long long)++sequence,(unsigned long long)BbProcessMemory::TickMilliseconds());
        capped=true;std::fputs("Memory ledger: event cap reached; later lifetime totals are incomplete\n",stderr);return;
    }
    const int bytes=std::fprintf(Output(),"%llu,%llu,%s,%s,0x%llx,0x%llx,%llu,%llu,%llu,%u,%llu,%s\n",
        (unsigned long long)++sequence,(unsigned long long)BbProcessMemory::TickMilliseconds(),
        action,domain,(unsigned long long)id,(unsigned long long)parent,
        (unsigned long long)requested,(unsigned long long)size,(unsigned long long)offset,type,
        (unsigned long long)detail,clean);
    if(bytes>0)written+=bytes;
    else {capped=true;std::fputs("Memory ledger: output write failed; history incomplete\n",stderr);}
}
void VKAPI_PTR Allocate(VmaAllocator,unsigned type,VkDeviceMemory memory,VkDeviceSize size,void*) {
    Write("create","vma_block",std::bit_cast<std::uint64_t>(memory),0,size,size,0,type,0,owner);
}
void VKAPI_PTR Free(VmaAllocator,unsigned type,VkDeviceMemory memory,VkDeviceSize size,void*) {
    Write("destroy","vma_block",std::bit_cast<std::uint64_t>(memory),0,0,size,0,type,0,owner);
}
}
bool Enabled() {return Output()!=nullptr;}
Scope::Scope(std::string_view name) : enabled{Enabled()} {
    if(!enabled)return;
    std::memcpy(previous,owner,sizeof(owner));
    const auto n=std::min(name.size(),sizeof(owner)-1);
    if(n)std::memcpy(owner,name.data(),n);owner[n]=0;
}
Scope::~Scope() {if(enabled)std::memcpy(owner,previous,sizeof(owner));}
const VmaDeviceMemoryCallbacks* Callbacks() {
    static const VmaDeviceMemoryCallbacks callbacks{Allocate,Free,nullptr};
    return Enabled() ? &callbacks : nullptr;
}
void Resource(const char* action,const char* kind,VmaAllocator allocator,VmaAllocation allocation,
              std::uint64_t requested,std::uint64_t usage) {
    if(!allocation || !Enabled())return;
    VmaAllocationInfo info{};vmaGetAllocationInfo(allocator,allocation,&info);
    Write(action,kind,reinterpret_cast<std::uintptr_t>(allocation),
        std::bit_cast<std::uint64_t>(info.deviceMemory),requested,info.size,info.offset,
        info.memoryType,usage,info.pName ? info.pName : owner);
}
void Defer(VmaAllocator allocator,VmaAllocation allocation,std::uint64_t tick) {
    Resource("defer","buffer",allocator,allocation,0,tick);
}
void Summary(VmaAllocator allocator,std::uint64_t staging) {
    if(!Enabled())return;
    VmaTotalStatistics stats{};vmaCalculateStatistics(allocator,&stats);
    Write("sample","vma_totals",0,0,stats.total.statistics.allocationBytes,
        stats.total.statistics.blockBytes,staging,stats.total.statistics.allocationCount,
        stats.total.statistics.blockCount,"requested=VMAallocationBytes backing=VMAblockBytes offset=stagingBytes nonadditive");
}
}
