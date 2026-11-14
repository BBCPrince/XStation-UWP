// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstring>
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
void RunIntegerFetchRegression194(const Vulkan::Instance&,Vulkan::Scheduler&);
namespace VideoCore {
bool BufferCache::RunDenseRegression194() {
    ASSERT_MSG(!sparse_buffers,"Allocator regression requires actual conventional buffers");
    const u64 page=u64{1}<<arena_page_bits;
    const VAddr base=0x9000000000ULL;
    const std::array<VAddr,3> address{base+page-256,base+page+256,base+2*page+512};
    const std::array<u32,3> pattern{0x12345678,0x87654321,0xcafed00d};
    const Buffer* merged=nullptr;
    for(u32 i=0;i<3;++i) {
        auto* separate=GetDenseArena(address[i]>>block_shift,(address[i]+255)>>block_shift);
        EnsureResident(separate,address[i]>>block_shift,(address[i]+255)>>block_shift);
        runtime.FillBuffer(separate,separate->Offset(address[i]),256,pattern[i]);
        merged=GetDenseArena(address[0]>>block_shift,(address[i]+255)>>block_shift);
        ASSERT(merged->IsInBounds(address[0],address[i]-address[0]+256));
    }
    Buffer readback{instance,0,4096,MemoryType::HostCached,"Dense194 verification"};
    std::array<vk::BufferCopy,3> copies;
    for(u32 i=0;i<3;++i)copies[i]={merged->Offset(address[i]),i*256,256};
    runtime.CopyBuffer(merged,&readback,copies);
    auto* table=GetBdaPageTableBuffer();
    ASSERT(table==GetBdaPageTableBuffer());
    for(u32 i=0;i<3;++i)copies[i]={(address[i]>>block_shift)*8,1024+i*8,8};
    runtime.CopyBuffer(table,&readback,copies);
    scheduler.Finish();
    readback.Invalidate(0,4096);
    for(u32 i=0;i<3;++i) {
        for(u32 word=0;word<64;++word) {
            u32 actual;std::memcpy(&actual,readback.mapped_data.data()+i*256+word*4,4);
            ASSERT_MSG(actual==pattern[i],"Dense migration corrupted GPU-written range {}, word {}",i,word);
        }
        vk::DeviceAddress actual;std::memcpy(&actual,readback.mapped_data.data()+1024+i*8,8);
        const auto expected=merged->BufferDeviceAddress()+((address[i]>>block_shift)<<block_shift)-merged->cpu_addr;
        ASSERT_MSG(actual==expected,"Dense migration left stale BDA entry {}",i);
    }
    std::puts("PASS194: real GPU-written bytes survive two-page and three-page arena merges; lazy BDA entries point to the replacement buffer");
    const u64 before=address_space.AllocatedBytes();
    ASSERT(before<4_MB);
    ASSERT(address_space.Get(0xf000000000ULL>>arena_page_bits)==nullptr);
    ASSERT(before==address_space.AllocatedBytes());
    std::puts("PASS194: distant 40-bit addresses use sparse CPU lookup leaves; empty queries allocate nothing");
    runtime.FillBuffer(GetFaultBuffer(),0,GetFaultBuffer()->SizeBytes(),0);
    for(unsigned i=0;i<40;++i)ProcessFaultBuffer();
    scheduler.Finish();scheduler.PopPendingOperations();
    ASSERT(merged->Handle());
    std::puts("PASS194: 40 production fault-parser compute dispatches complete with ordinary descriptor sets and GPU-fenced retirement");
    RunIntegerFetchRegression194(instance,scheduler);
    std::puts("ALLOCATOR194 CHECKS PASSED");std::fflush(stdout);
    return true;
}
}
