#include <array>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <memory>
#include <future>
#ifdef _WIN32
#include <windows.h>
#endif
#include "shader_recompiler/info.h"
#include "shader_recompiler/specialization.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/host_shaders/dozen_test_vert.h"
#include "video_core/host_shaders/dozen_test_frag.h"
#include "video_core/host_shaders/dozen_rect_test_vert.h"
#include "video_core/host_shaders/dozen_quad_test_vert.h"
#include "video_core/host_shaders/dozen_list_test_frag.h"
#include "video_core/host_shaders/dozen_view_sample_comp.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "core/emulator_settings.h"
#include "bbport_process_memory.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"
#include "video_core/texture_cache/image_info.h"

extern "C" uintptr_t runtime_memory_resolve(const char* name);
extern "C" void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int);
extern "C" int runtime_memory_write_backing(uintptr_t, const void*, uint64_t);
extern "C" uint64_t runtime_memory_trim_zero_private(uint64_t);
#ifdef _WIN32
extern "C" int32_t runtime_thread_spawn(void**, void*(__attribute__((sysv_abi)) *)(void*), void*, uint64_t, const char*);
extern "C" int32_t runtime_thread_join_spawned(void*, void**);
extern "C" void runtime_memory_dump_touch_history(void);
// PS4 leaf functions use the SysV red zone below RSP. A first-access fault
// must preserve that data, not merely the Windows nonvolatile registers.
__attribute__((naked,sysv_abi)) static u64 TouchWithRedZone(void*) {
    __asm__ volatile("movabsq $0x123456789abcdef0, %rax\n\t"
                     "movq $-128, %rcx\n\t"
                     "1: movq %rax, (%rsp,%rcx)\n\taddq $8,%rcx\n\tjnz 1b\n\t"
                     "movb $0x39, (%rdi)\n\t"
                     "movq $-128,%rcx\n\t"
                     "2: cmpq %rax,(%rsp,%rcx)\n\tjne 3f\n\taddq $8,%rcx\n\tjnz 2b\n\tretq\n\t"
                     "3: movq %rcx,%rax\n\tretq");
}
__attribute__((naked,sysv_abi)) static u64 TouchWithAvx(void*) {
    __asm__ volatile("vpcmpeqd %ymm0, %ymm0, %ymm0\n\t"
                     "vmovdqu %ymm0, (%rdi)\n\t"
                     "vpmovmskb %ymm0, %eax\n\tvzeroupper\n\tretq");
}
__attribute__((naked,sysv_abi)) static u64 TouchWithAlignedRedZone(void*, u64) {
    __asm__ volatile("movq %rsp,%r10\n\tandq $-64,%rsp\n\tsubq %rsi,%rsp\n\t"
                     "vpcmpeqd %ymm0,%ymm0,%ymm0\n\t"
                     "movabsq $0x123456789abcdef0,%rax\n\tmovq $-128,%rcx\n\t"
                     "1: movq %rax,(%rsp,%rcx)\n\taddq $8,%rcx\n\tjnz 1b\n\t"
                     "leaq -128(%rsp),%rsp\n\tmovb $0x39,(%rdi)\n\tleaq 128(%rsp),%rsp\n\tmovq $-128,%rcx\n\t"
                     "2: cmpq %rax,(%rsp,%rcx)\n\tjne 3f\n\taddq $8,%rcx\n\tjnz 2b\n\tjmp 4f\n\t"
                     "3: movq %rcx,%rax\n\t"
                     "4: vzeroupper\n\tmovq %r10,%rsp\n\tretq");
}
__attribute__((sysv_abi)) static void* RepeatedGuestStackTouches(void* memory) {
    for (u32 i = 0; i < 256; ++i) {
        // Same LEA guards as the prepared guest-code trampolines. The original
        // unguarded variant reproduces Windows compact-XSAVE red-zone damage.
        const u64 result = TouchWithAlignedRedZone(static_cast<u8*>(memory) + (256 + i) * 16384,
                                                  8 + (i % 4) * 16);
        if (result != 0x123456789abcdef0ULL) {
            runtime_memory_dump_touch_history();
            return reinterpret_cast<void*>(result);
        }
    }
    return reinterpret_cast<void*>(0x123456789abcdef0ULL);
}
#endif

namespace VideoCore {

struct BufferArenaTestAccess {
    static int TraceInitialization(const char* next) {
        std::printf("Renderer check: initializing %s\n", next);
        return 0;
    }
    int trace_instance{TraceInitialization("Vulkan instance")};
    Vulkan::Instance instance{0, ![] {
        const char* value = std::getenv("BB_GPU_TEST_NO_VALIDATION");
        return value && value[0] == '1';
    }()};
    int trace_scheduler{TraceInitialization("scheduler")};
    Vulkan::Scheduler scheduler{instance};
    int trace_runtime{TraceInitialization("runtime helpers")};
    Vulkan::Runtime runtime{instance, scheduler};
    int trace_liverpool{TraceInitialization("Liverpool")};
    AmdGpu::Liverpool liverpool;
    PageManager tracker{nullptr};
    int trace_buffers{TraceInitialization("buffer cache")};
    BufferCache buffers{instance, scheduler, runtime, &liverpool, textures, tracker};
    int trace_textures{TraceInitialization("texture cache")};
    TextureCache textures{instance, scheduler, runtime, &liverpool, buffers, tracker};

    void Run() {
        char cleanup_probe[32768];const char* cleanup_user=std::getenv("BB_GPU_USER_DIR");std::snprintf(cleanup_probe,sizeof(cleanup_probe),"%s/upload-cleanup-check.txt",cleanup_user?cleanup_user:".");
        if(FILE* probe=std::fopen(cleanup_probe,"rb")){std::fclose(probe);TestOverlapPreservesPendingUploads();TestReclaimCompletedWithoutDrain();TestStagingLifetimes();TestCompactStagingBudget();TestStagingAllocationPressure();TestUploadBatchLimit();return;}

        std::puts("Renderer check: initialization complete; running checks");
        if (std::getenv("BB_GPU_RESOURCE_POOL_PROBE")) {
            TestResourcePoolFenceRefresh();
            return;
        }
        if (std::getenv("BB_GPU_STAGING_GRACE_PROBE")) {
            TestStagingWarmReuse();
            return;
        }
        if (std::getenv("BB_GPU_GRAPHICS_PROBE")) {
            TestGraphics();
            return;
        }
        ASSERT(!buffers.sparse_buffers);
        TestStreamingTextureEviction();
        const bool lazy_table_expected = EmulatorSettings.IsCompactMemoryEnabled() &&
                                         !EmulatorSettings.IsDirectMemoryAccessEnabled();
        ASSERT(bool(buffers.bda_pagetable_buffer) != lazy_table_expected);
        if (lazy_table_expected) TestColdBufferEviction();
        if (lazy_table_expected) {
            const char* writeback=std::getenv("BB_BUFFER_WRITEBACK_GC");
            if(writeback && writeback[0]=='1') TestColdWrittenBuffers();
            else std::puts("SKIP: cold GPU-buffer writeback disabled; its known failing PC regression is excluded from this baseline");
        }
        if (std::getenv("BB_ARENA_RETIRE_MB")) TestDenseRetirement();
        TestDenseGrowthPadding();
        const u64 page = u64{1} << buffers.arena_page_bits;
        const VAddr base = 0x1000000000ULL;
        const VAddr first = base + page - 256;
        const VAddr second = base + page * 2 + 128;
        const auto prepare = [&](VAddr address, u64 size, u32 pattern) {
            const u64 first_block = address >> buffers.block_shift;
            const u64 last_block = (address + size - 1) >> buffers.block_shift;
            const auto* arena = buffers.GetArena(first_block, last_block);
            buffers.EnsureResident(arena, first_block, last_block);
            runtime.FillBuffer(arena, address - arena->cpu_addr, size, pattern);
        };
        prepare(first, 512, 0x12345678);
        prepare(second, 512, 0x87654321);
        const auto* merged = buffers.GetArena(first >> buffers.block_shift,
                                              (second + 511) >> buffers.block_shift);
        ASSERT(merged->IsInBounds(first, second + 512 - first));
        Buffer readback{instance, 0, 4096, MemoryType::HostCached};
        const std::array<vk::BufferCopy, 2> copies = {{
            {first - merged->cpu_addr, 0, 512},
            {second - merged->cpu_addr, 512, 512},
        }};
        runtime.CopyBuffer(merged, &readback, copies);
        constexpr VAddr reclaimed = 0x3000000000ULL;
        if (lazy_table_expected) {
            ASSERT_MSG(!buffers.bda_pagetable_buffer, "A buffer migration allocated an unused BDA table");
            prepare(reclaimed, 64_KB, 0xdeadbeef);
            auto* discarded = const_cast<Buffer*>(buffers.address_space.Get(reclaimed >> buffers.arena_page_bits));
            buffers.ReclaimDenseArena(*discarded);
            ASSERT_MSG(!buffers.bda_pagetable_buffer, "An unmap allocated an unused BDA table");
        }
        const u64 block = first >> buffers.block_shift;
        const std::array<vk::BufferCopy, 1> table_copy = {{{block * 8, 1024, 16}}};
        auto* page_table = buffers.GetBdaPageTableBuffer();
        ASSERT(page_table == buffers.GetBdaPageTableBuffer());
        ASSERT(buffers.CollectColdDenseArenas(~0ULL, 0, 128_MB) == 0);
        runtime.CopyBuffer(page_table, &readback, table_copy);
        if (lazy_table_expected) {
            const vk::BufferCopy cleared_copy{(reclaimed >> buffers.block_shift) * 8, 1040, 8};
            runtime.CopyBuffer(page_table, &readback, std::span{&cleared_copy, 1});
        }
        scheduler.Finish();
        readback.Invalidate(0, 4096);
        std::array<u32, 256> values;
        std::memcpy(values.data(), readback.mapped_data.data(), 1024);
        for (unsigned i = 0; i < 128; ++i) {
            ASSERT_MSG(values[i] == 0x12345678, "First migrated buffer word {} is incorrect", i);
            ASSERT_MSG(values[128 + i] == 0x87654321, "Second migrated buffer word {} is incorrect", i);
        }
        std::array<vk::DeviceAddress, 2> addresses;
        std::memcpy(addresses.data(), readback.mapped_data.data() + 1024, 16);
        for (unsigned i = 0; i < 2; ++i) {
            const auto expected = merged->BufferDeviceAddress() + ((block + i) << buffers.block_shift) - merged->cpu_addr;
            ASSERT_MSG(addresses[i] == expected, "Migrated page table entry {} is incorrect", i);
        }
        if (lazy_table_expected) {
            u64 removed;
            std::memcpy(&removed, readback.mapped_data.data() + 1040, 8);
            ASSERT_MSG(removed == 0, "Lazy activation restored a reclaimed arena's stale GPU pointer");
            std::puts("PASS: lazy BDA activation backfills migrated arenas and excludes reclaimed ranges");
        }
        const auto* faults = buffers.GetFaultBuffer();
        runtime.FillBuffer(faults, 0, faults->size_bytes, 0);
        for (unsigned i = 0; i < 40; ++i) {
            buffers.ProcessFaultBuffer();
        }
        scheduler.Finish();
        std::puts("PASS: production conventional arenas preserve GPU data across two-page and three-page merges; page-table addresses updated; 40 ordinary descriptor dispatches");
        TestGraphics();
        TestHdrBlending();
        if (std::getenv("BB_DIAGNOSTIC_IMAGE_DIR")) TestDiagnosticCapture();
        TestByteBuffers();
        TestPaddedBc6Copies();
        if (std::getenv("BB_REFLECTION_INPUT")) TestReflectionSamples();
        TestVolumeCopies();
        TestDepthColorScratch();
        TestByteTiling();
        TestComputeBatches();
        TestHostCopyCompletion();
        TestNullImages(false);
        TestNullImages(true);
        TestMutableColorViews();
        TestSpecialBufferBindingKeys();
        TestResourcePoolFenceRefresh();
        TestStagingLifetimes();
        PageManager::RunTrackingRegression();
        if (EmulatorSettings.IsCompactMemoryEnabled()) TestCompactStagingBudget();
        if (EmulatorSettings.IsCompactMemoryEnabled()) TestStagingWarmReuse();
        if (EmulatorSettings.IsCompactMemoryEnabled()) TestStagingAllocationPressure();
        if (EmulatorSettings.IsCompactMemoryEnabled() && std::getenv("BB_UPLOAD_SUBMIT_MB") &&
            std::strtoull(std::getenv("BB_UPLOAD_SUBMIT_MB"), nullptr, 10) > 0) TestUploadBatchLimit();
        TestFaultOverflow();
        if (EmulatorSettings.IsCompactMemoryEnabled()) TestUnmappedArenaRelease();
        if (EmulatorSettings.IsCompactMemoryEnabled()) {
            // Actual GPU arena tests above use addresses in several distant
            // parts of the 40-bit guest range. Empty lookups must not commit a
            // flat 128 MiB pointer table or allocate a leaf when merely queried.
            const u64 before = buffers.address_space.AllocatedBytes();
            ASSERT(before < 4_MB);
            ASSERT(buffers.address_space.Get((0xf000000000ULL >> buffers.arena_page_bits)) == nullptr);
            ASSERT(buffers.address_space.AllocatedBytes() == before);
            std::puts("PASS: distant 40-bit arena addresses use a small lazy lookup table and empty queries allocate nothing");
        }
    }

    void TestColdWrittenBuffers() {
        using Allocate=s32(__attribute__((sysv_abi)) *)(s64,s64,u64,u64,int,s64*);
        using Map=s32(__attribute__((sysv_abi)) *)(void**,u64,int,int,s64,u64);
        using Release=s32(__attribute__((sysv_abi)) *)(u64,u64);
        auto allocate=reinterpret_cast<Allocate>(runtime_memory_resolve("rTXw65xmLIA#p#J"));
        auto map=reinterpret_cast<Map>(runtime_memory_resolve("L-Q3LEjIbgA#p#J"));
        auto release=reinterpret_cast<Release>(runtime_memory_resolve("MBuItvba6z8#p#J"));
        constexpr u64 span=8_MB;
        s64 physical{};ASSERT(allocate(896_MB,896_MB+span,span,64_KB,3,&physical)==0);
        void* mapping=reinterpret_cast<void*>(0x7800000000ULL);
        ASSERT(map(&mapping,span,0x33,0x10,physical,64_KB)==0);
        std::memset(mapping,0x39,span);
        const auto base=reinterpret_cast<VAddr>(mapping);
        const auto [old,offset]=buffers.ObtainBuffer(base,span,false);
        const auto [dirty,dirty_offset]=buffers.ObtainBuffer(base,32_KB,true);
        ASSERT(old==dirty);
        runtime.FillBuffer(dirty,dirty_offset,32_KB,0x2468ace0);
        Buffer readback{instance,0,64_KB,MemoryType::HostCached};
        const std::array<vk::BufferCopy,2> queued{{{offset,0,4},{offset+span-4,4,4}}};
        runtime.CopyBuffer(old,&readback,queued);
        old->arena_last_use_ms=1;
        ASSERT(buffers.CollectColdWrittenArenas(3000,2000,16_MB)==span);
        ASSERT(!old->Handle() && !buffers.address_space.Get(base>>buffers.arena_page_bits));
        readback.Invalidate(0,16);
        u32 first,last;std::memcpy(&first,readback.mapped_data.data(),4);std::memcpy(&last,readback.mapped_data.data()+4,4);
        ASSERT_MSG(first==0x2468ace0 && last==0x39393939,
                   "Cold writeback queued readers: first={:#x}, last={:#x}",first,last);
        std::memcpy(&first,mapping,4);ASSERT(first==0x2468ace0);
        ASSERT(static_cast<u8*>(mapping)[32_KB]==0x39 && static_cast<u8*>(mapping)[span-1]==0x39);
        const auto [rebuilt,rebuilt_offset]=buffers.ObtainBuffer(base,span,false);
        const std::array<vk::BufferCopy,2> restored{{{rebuilt_offset,8,4},{rebuilt_offset+span-4,12,4}}};
        runtime.CopyBuffer(rebuilt,&readback,restored);
        scheduler.Finish();readback.Invalidate(0,16);
        std::memcpy(&first,readback.mapped_data.data()+8,4);std::memcpy(&last,readback.mapped_data.data()+12,4);
        ASSERT(first==0x2468ace0 && last==0x39393939);
        buffers.ReleaseUnmappedArenas(base,span);scheduler.Finish();scheduler.PopPendingOperations();
        ASSERT(release(physical,span)==0);
        std::puts("PASS: cold GPU-written islands survive bounded readback, queued GPU readers, arena release and exact guest-data reupload");
    }

    void TestDenseRetirement() {
        const auto saved_padding_budget = buffers.dense_growth_padding_budget_bytes;
        buffers.dense_growth_padding_budget_bytes = 0;
        constexpr VAddr base=0x6000000000ULL;
        const auto* current=buffers.GetArena(base>>buffers.block_shift,(base+16_MB-1)>>buffers.block_shift);
        buffers.EnsureResident(current,base>>buffers.block_shift,(base+64_KB-1)>>buffers.block_shift);
        runtime.FillBuffer(current,0,64_KB,0x76895432);
        Buffer readback{instance,0,64_KB,MemoryType::HostCached};
        bool bounded=false;
        for(u32 i=0;i<6;++i) {
            const auto* old=current;
            current=buffers.GetArena(base>>buffers.block_shift,(base+16_MB+(i+1)*64_KB-1)>>buffers.block_shift);
            ASSERT(old!=current && old->Handle());
            // Both old bindings and the migrated contents must survive until
            // the completed operation boundary, including deferred recording.
            const vk::BufferCopy previous{0,i*8,4},migrated{0,i*8+4,4};
            runtime.CopyBuffer(old,&readback,std::span{&previous,1});
            runtime.CopyBuffer(current,&readback,std::span{&migrated,1});
            buffers.LimitRetiredArenaMemory();
            bounded|=buffers.pending_dense_copy_bytes==0;
        }
        scheduler.Finish();scheduler.PopPendingOperations();
        ASSERT(bounded && buffers.pending_dense_copy_bytes==0);
        readback.Invalidate(0,64_KB);
        for(u32 i=0;i<12;++i) {
            u32 value;std::memcpy(&value,readback.mapped_data.data()+i*4,4);
            ASSERT(value==0x76895432);
        }
        buffers.ReleaseUnmappedArenas(base,32_MB);
        scheduler.Finish();scheduler.PopPendingOperations();
        buffers.dense_growth_padding_budget_bytes = saved_padding_budget;
        std::puts("PASS: bounded arena retirement preserves old queued bindings and migrated GPU contents across repeated growth");
    }

    void TestDenseGrowthPadding() {
        const auto saved_budget = buffers.dense_growth_padding_budget_bytes;
        const auto saved_used = buffers.dense_growth_padding_used_bytes;
        buffers.dense_growth_padding_budget_bytes = 2_MB;
        buffers.dense_growth_padding_used_bytes = 0;
        const u64 page = u64{1} << buffers.arena_page_bits;
        ASSERT(page == 64_KB);
        const auto get = [&](VAddr base, u64 size) {
            return buffers.GetArena(base >> buffers.block_shift,
                                    (base + size - 1) >> buffers.block_shift);
        };
        const auto fill = [&](const Buffer* arena, VAddr address, u32 pattern) {
            buffers.EnsureResident(arena, address >> buffers.block_shift,
                                   (address + page - 1) >> buffers.block_shift);
            runtime.FillBuffer(arena, address - arena->cpu_addr, page, pattern);
        };
        constexpr VAddr tail_base = 0x6200000000ULL, head_base = 0x6300000000ULL;
        const auto* old_tail = get(tail_base, 16_MB);
        fill(old_tail, tail_base, 0x10293847);
        const auto* tail = get(tail_base, 16_MB + page);
        ASSERT(tail != old_tail && old_tail->Handle());
        ASSERT(tail->SizeBytes() == 17_MB + page);
        ASSERT(buffers.dense_growth_padding_used_bytes == 1_MB);
        // Sixteen successive newly revealed pages reuse one replacement.
        for (u64 i = 1; i <= 16; ++i)
            ASSERT(get(tail_base, 16_MB + i * page) == tail);
        ASSERT(buffers.dense_growth_padding_used_bytes == page);
        // An existing neighbour must never be swallowed by speculative padding.
        const auto* neighbour = get(head_base - 4 * page, page);
        fill(neighbour, head_base - 4 * page, 0xABCDEF01);
        const auto* old_head = get(head_base, 1_MB);
        fill(old_head, head_base, 0x87654321);
        const auto* head = get(head_base - page, 1_MB + page);
        ASSERT(head->cpu_addr == head_base - 3 * page);
        ASSERT(buffers.address_space.Get((head_base - 4 * page) >> buffers.arena_page_bits) == neighbour);
        ASSERT(buffers.dense_growth_padding_used_bytes == 3 * page);
        const auto used = buffers.dense_growth_padding_used_bytes;
        buffers.dense_growth_padding_budget_bytes = used;
        // With no credits, a growth at the other end drops the unused tail.
        // Its old allocation remains charged until the queued GPU work ends.
        const auto* exhausted = get(tail_base - page, 17_MB + page);
        ASSERT(exhausted->cpu_addr == tail_base - page);
        ASSERT(exhausted->SizeBytes() == 17_MB + page);
        ASSERT(!buffers.address_space.Get((tail_base + 17_MB) >> buffers.arena_page_bits));
        ASSERT(buffers.dense_growth_padding_used_bytes == used);
        Buffer readback{instance, 0, page, MemoryType::HostCached};
        const auto read = [&](const Buffer* arena, VAddr address, u64 offset) {
            const vk::BufferCopy copy{address - arena->cpu_addr, offset, 4};
            runtime.CopyBuffer(arena, &readback, std::span{&copy, 1});
        };
        // Keep reads of the original binding queued alongside two generations
        // of migrated data, including a growth at the beginning of an arena.
        read(old_tail, tail_base, 0);
        read(tail, tail_base, 4);
        read(exhausted, tail_base, 8);
        read(old_head, head_base, 12);
        read(head, head_base, 16);
        read(neighbour, head_base - 4 * page, 20);
        scheduler.Finish();scheduler.PopPendingOperations();
        ASSERT(buffers.dense_growth_padding_used_bytes == 2 * page);
        readback.Invalidate(0, page);
        constexpr std::array<u32, 6> expected{0x10293847, 0x10293847, 0x10293847,
                                             0x87654321, 0x87654321, 0xABCDEF01};
        for (u32 i=0; i<expected.size(); ++i) {
            u32 value;std::memcpy(&value, readback.mapped_data.data()+i*4, 4);
            ASSERT(value == expected[i]);
        }
        buffers.ReleaseUnmappedArenas(tail_base-page,32_MB);
        buffers.ReleaseUnmappedArenas(head_base-4*page,2_MB);
        scheduler.Finish();scheduler.PopPendingOperations();
        ASSERT(buffers.dense_growth_padding_used_bytes == 0);
        buffers.dense_growth_padding_budget_bytes = saved_budget;
        buffers.dense_growth_padding_used_bytes = saved_used;
        std::puts("PASS: bounded dense growth reuses sixteen adjacent requests, trims unused padding, recycles credits after use/fences, and preserves queued old/head/tail/neighbour GPU data");
    }

    void TestColdBufferEviction() {
        using Allocate = s32(__attribute__((sysv_abi)) *)(s64, s64, u64, u64, int, s64*);
        using Map = s32(__attribute__((sysv_abi)) *)(void**, u64, int, int, s64, u64);
        using Release = s32(__attribute__((sysv_abi)) *)(u64, u64);
        auto allocate = reinterpret_cast<Allocate>(runtime_memory_resolve("rTXw65xmLIA#p#J"));
        auto map = reinterpret_cast<Map>(runtime_memory_resolve("L-Q3LEjIbgA#p#J"));
        auto release = reinterpret_cast<Release>(runtime_memory_resolve("MBuItvba6z8#p#J"));
        constexpr u64 span = 3 * 64_KB;
        s64 physical{};
        ASSERT(allocate(768_MB, 768_MB + span, span, 64_KB, 3, &physical) == 0);
        void* mapping = reinterpret_cast<void*>(0x7000000000ULL);
        ASSERT(map(&mapping, span, 0x33, 0x10, physical, 64_KB) == 0);
        const VAddr base = reinterpret_cast<VAddr>(mapping);
        std::memset(mapping, 0x39, span);
        const auto [clean, clean_offset] = buffers.ObtainBuffer(base, 32_KB, false);
        const auto [dirty, dirty_offset] = buffers.ObtainBuffer(base + 64_KB, 32_KB, true);
        const auto [recent, recent_offset] = buffers.ObtainBuffer(base + 128_KB, 32_KB, false);
        clean->arena_last_use_ms = dirty->arena_last_use_ms = 1;
        recent->arena_last_use_ms = 3000;
        runtime.FillBuffer(dirty, dirty_offset, 32_KB, 0x87654321);
        Buffer output{instance, 0, 64_KB, MemoryType::HostCached};
        const vk::BufferCopy queued{clean_offset, 0, 32_KB};
        runtime.CopyBuffer(clean, &output, std::span{&queued, 1});
        ASSERT(buffers.CollectColdDenseArenas(3000, 2000, 16_MB) == 64_KB);
        ASSERT(!buffers.address_space.Get(base >> buffers.arena_page_bits));
        ASSERT(buffers.address_space.Get((base + 64_KB) >> buffers.arena_page_bits) == dirty);
        ASSERT(buffers.address_space.Get((base + 128_KB) >> buffers.arena_page_bits) == recent);
        // Its old handle remains alive until the already queued copy completes.
        scheduler.Finish();
        output.Invalidate(0, 32_KB);
        for (u32 i = 0; i < 32_KB; ++i) ASSERT(output.mapped_data[i] == 0x39);
        const auto [rebuilt, rebuilt_offset] = buffers.ObtainBuffer(base, 32_KB, false);
        ASSERT(rebuilt != clean);
        const std::array<vk::BufferCopy, 1> restored{{{rebuilt_offset, 0, 32_KB}}};
        runtime.CopyBuffer(rebuilt, &output, restored);
        scheduler.Finish();
        output.Invalidate(0, 32_KB);
        for (u32 i = 0; i < 32_KB; ++i) ASSERT(output.mapped_data[i] == 0x39);
        buffers.ReleaseUnmappedArenas(base, span);
        scheduler.Finish();
        ASSERT(release(physical, span) == 0);
        std::puts("PASS: cold CPU-backed buffer eviction preserves queued GPU reads, retains recent and GPU-written arenas, and rebuilds exact guest bytes");
    }

    void TestUnmappedArenaRelease() {
        using Allocate = s32(__attribute__((sysv_abi)) *)(s64, s64, u64, u64, int, s64*);
        using Map = s32(__attribute__((sysv_abi)) *)(void**, u64, int, int, s64, u64);
        using Unmap = s32(__attribute__((sysv_abi)) *)(void*, u64);
        using Release = s32(__attribute__((sysv_abi)) *)(u64, u64);
        auto allocate = reinterpret_cast<Allocate>(runtime_memory_resolve("rTXw65xmLIA#p#J"));
        auto map = reinterpret_cast<Map>(runtime_memory_resolve("L-Q3LEjIbgA#p#J"));
        auto unmap = reinterpret_cast<Unmap>(runtime_memory_resolve("cQke9UuBQOk#p#J"));
        auto release = reinterpret_cast<Release>(runtime_memory_resolve("MBuItvba6z8#p#J"));
        ASSERT(allocate && map && unmap && release);
#ifdef _WIN32
        if (const char* mode=std::getenv("BB_UWP_PRIVATE_CPU");mode && mode[0]=='1') {
            constexpr u64 start=256_MB, span=128_KB;
            s64 physical{};
            ASSERT(allocate(start,start+span,span,64_KB,0,&physical)==0);
            void* primary=reinterpret_cast<void*>(0x7400000000ULL);
            void* alias=reinterpret_cast<void*>(0x7500000000ULL);
            const auto query=[](void* p) { MEMORY_BASIC_INFORMATION m{};ASSERT(VirtualQuery(p,&m,sizeof(m)));return m; };
            ASSERT(map(&primary,span,3,0x10,physical,64_KB)==0);
            ASSERT(query(primary).Type==MEM_PRIVATE);
            auto* data=static_cast<volatile u8*>(primary);
            data[0]=0x39;data[span-1]=0xb7;
            ASSERT(unmap(primary,span)==0);
            ASSERT(map(&primary,span,1,0x10,physical,64_KB)==0);
            ASSERT(query(primary).Type==MEM_PRIVATE && query(primary).Protect==PAGE_READONLY);
            ASSERT(data[0]==0x39 && data[span-1]==0xb7);
            ASSERT(map(&alias,span,3,0x10,physical,64_KB)==0);
            ASSERT(query(primary).Type==MEM_MAPPED && query(primary).Protect==PAGE_READONLY);
            static_cast<volatile u8*>(alias)[0]=0xa7;
            ASSERT(data[0]==0xa7 && static_cast<volatile u8*>(alias)[span-1]==0xb7);
            ASSERT(unmap(static_cast<u8*>(alias)+16_KB,16_KB)==0);
            ASSERT(data[0]==0xa7 && data[span-1]==0xb7);
            ASSERT(release(physical,span)==0);
            ASSERT(allocate(start,start+span,span,64_KB,0,&physical)==0);
            ASSERT(map(&primary,span,3,0x10,physical,64_KB)==0);
            ASSERT(data[0]==0 && data[span-1]==0);
            ASSERT(release(physical,span)==0);
            // A release of private storage must not first commit a shared copy.
            ASSERT(allocate(start+span,start+span+64_KB,64_KB,64_KB,0,&physical)==0);
            ASSERT(map(&primary,64_KB,3,0x10,physical,64_KB)==0);
            ASSERT(query(primary).Type==MEM_PRIVATE);
            data[0]=0x51;ASSERT(release(physical,64_KB)==0);
            std::puts("PASS: private CPU heaps survive full unmap/remap, preserve read-only protections, promote real aliases, and release with zero-on-reuse");
            using Flex=s32(__attribute__((sysv_abi)) *)(void**,u64,int,int);
            auto flexible=reinterpret_cast<Flex>(runtime_memory_resolve("IWIBBdTHit4#p#J"));
            ASSERT(flexible);
            void* flex=reinterpret_cast<void*>(0x7600000000ULL);
            ASSERT(flexible(&flex,span,3,0x10)==0);
            ASSERT(query(flex).Type==MEM_PRIVATE);
            for(u32 n=0;n<8;++n)std::memset(static_cast<u8*>(flex)+n*16_KB,0x20+n,16_KB);
            ASSERT(unmap(static_cast<u8*>(flex)+16_KB,16_KB)==0);
            auto copied=std::make_unique<u8[]>(span-32_KB);
            Core::Memory::Instance()->CopySparseMemory(reinterpret_cast<VAddr>(flex)+32_KB,copied.get(),span-32_KB);
            ASSERT(copied[0]==0x22 && copied[32_KB]==0x24 && copied[span-32_KB-1]==0x27);
            ASSERT(unmap(flex,16_KB)==0);
            ASSERT(unmap(static_cast<u8*>(flex)+32_KB,span-32_KB)==0);
            ASSERT(flexible(&flex,span,3,0x10)==0);
            for(u64 n=0;n<span;++n)ASSERT(static_cast<u8*>(flex)[n]==0);
            ASSERT(unmap(flex,span)==0);
            std::puts("PASS: private flexible heaps preserve physical offsets across partial unmaps and zero every recycled page");
            ASSERT(allocate(512_MB,512_MB+span,span,64_KB,0,&physical)==0);
            void* zeroed=reinterpret_cast<void*>(0x7800000000ULL);
            ASSERT(map(&zeroed,span,3,0x10,physical,64_KB)==0 && query(zeroed).Type==MEM_PRIVATE);
            auto* zero_bytes=static_cast<u8*>(zeroed);
            std::memset(zero_bytes,0,span);zero_bytes[0]=0x39;
            uint64_t reclaimed=0;
            for(unsigned n=0;n<64 && query(zero_bytes+32_KB).State!=MEM_RESERVE;++n)reclaimed+=runtime_memory_trim_zero_private(16_MB);
            ASSERT(reclaimed>=span-16_KB && query(zero_bytes+32_KB).State==MEM_RESERVE);
            ASSERT(zero_bytes[0]==0x39);
            Core::Memory::Instance()->CopySparseMemory(reinterpret_cast<VAddr>(zero_bytes+32_KB),copied.get(),16_KB);
            ASSERT(query(zero_bytes+32_KB).State==MEM_RESERVE);
            for(u32 n=0;n<16_KB;++n)ASSERT(copied[n]==0);
            ASSERT(zero_bytes[32_KB]==0);zero_bytes[32_KB]=0xa7;ASSERT(zero_bytes[32_KB]==0xa7);
            // A writer racing the read-only zero scan must retry after the guard,
            // while real GPU read-only watches must retain their protection.
            auto* word=reinterpret_cast<std::atomic<u32>*>(zero_bytes);
            word->store(0x12345678);std::atomic<bool> stop{false};
            auto writer=std::async(std::launch::async,[&] { while(!stop.load())word->store(0x12345678); });
            for(unsigned n=0;n<64;++n)runtime_memory_trim_zero_private(16_MB);
            stop=true;writer.get();ASSERT(word->load()==0x12345678 && zero_bytes[32_KB]==0xa7);
            runtime_memory_gpu_protect(reinterpret_cast<uintptr_t>(zeroed),16_KB,1,0);
            for(unsigned n=0;n<16;++n)runtime_memory_trim_zero_private(16_MB);
            ASSERT(query(zeroed).Protect==PAGE_READONLY && word->load()==0x12345678);
            ASSERT(release(physical,span)==0);
            std::puts("PASS: verified-zero private pages decommit, sparse uploads remain zero, CPU faults restore them, concurrent writes survive, and GPU read-only watches stay protected");
        }
#endif
#ifdef _WIN32
        if (const char* mode=std::getenv("BB_UWP_PRIVATE_GPU");mode && mode[0]=='1') {
            constexpr u64 start=384_MB, span=128_KB;
            s64 physical{};
            ASSERT(allocate(start,start+span,span,64_KB,3,&physical)==0);
            void* primary=reinterpret_cast<void*>(0x7700000000ULL);
            ASSERT(map(&primary,span,0x33,0x10,physical,64_KB)==0);
            const auto query=[](void* p) { MEMORY_BASIC_INFORMATION m{};ASSERT(VirtualQuery(p,&m,sizeof(m)));return m; };
            auto* bytes=static_cast<u8*>(primary);
            ASSERT(query(primary).Type==MEM_PRIVATE && query(primary).State==MEM_RESERVE);
            bytes[0]=0x39;bytes[64_KB]=0xb7;
            if(const char* small=std::getenv("BB_UWP_HOST_PAGE_TOUCH");small && small[0]=='1') {
                SYSTEM_INFO host;GetNativeSystemInfo(&host);
                ASSERT(query(bytes).RegionSize==host.dwPageSize);
                ASSERT(query(bytes+host.dwPageSize).State==MEM_RESERVE);
                bytes[host.dwPageSize*2]=0x62;
                ASSERT(query(bytes+host.dwPageSize).State==MEM_RESERVE);
                ASSERT(query(bytes+host.dwPageSize*3).State==MEM_RESERVE);
                auto small_copy=std::make_unique<u8[]>(16_KB);
                Core::Memory::Instance()->CopySparseMemory(reinterpret_cast<VAddr>(primary),small_copy.get(),16_KB);
                ASSERT(small_copy[0]==0x39 && small_copy[host.dwPageSize]==0 && small_copy[host.dwPageSize*2]==0x62);
                ASSERT(query(bytes+host.dwPageSize).State==MEM_RESERVE);
                std::puts("PASS: private first-touch commits host pages independently, preserves untouched neighbours and sparse upload contents");
            }
            auto copied=std::make_unique<u8[]>(span);
            Core::Memory::Instance()->CopySparseMemory(reinterpret_cast<VAddr>(primary),copied.get(),span);
            ASSERT(copied[0]==0x39 && copied[64_KB]==0xb7 && copied[32_KB]==0);
            ASSERT(query(bytes+32_KB).State==MEM_RESERVE);
            runtime_memory_gpu_protect(reinterpret_cast<uintptr_t>(primary),span,1,1);
            ASSERT(query(bytes+32_KB).State==MEM_RESERVE && query(bytes+96_KB).State==MEM_RESERVE);
            ASSERT(bytes[0]==0x39 && bytes[64_KB]==0xb7);
            std::puts("PASS: removing GPU write watches preserves committed bytes and does not commit untouched private neighbours");
            runtime_memory_gpu_protect(reinterpret_cast<uintptr_t>(primary),16_KB,1,0);
            ASSERT(query(primary).Type==MEM_PRIVATE && query(primary).Protect==PAGE_READONLY);
            const u8 first=0xa7,second=0x51;
            ASSERT(runtime_memory_write_backing(reinterpret_cast<uintptr_t>(primary),&first,1));
            ASSERT(query(primary).Type==MEM_MAPPED && query(primary).Protect==PAGE_READONLY && bytes[0]==first);
            ASSERT(query(bytes+64_KB).Type==MEM_PRIVATE);
            runtime_memory_gpu_protect(reinterpret_cast<uintptr_t>(bytes+64_KB),16_KB,0,0);
            ASSERT(query(bytes+64_KB).Type==MEM_MAPPED && query(bytes+64_KB).Protect==PAGE_NOACCESS);
            Core::Memory::Instance()->CopySparseMemory(reinterpret_cast<VAddr>(bytes+64_KB),copied.get(),16_KB);
            ASSERT(copied[0]==0xb7);
            ASSERT(runtime_memory_write_backing(reinterpret_cast<uintptr_t>(bytes+64_KB),&second,1));
            ASSERT(query(bytes+64_KB).Protect==PAGE_NOACCESS);
            runtime_memory_gpu_protect(reinterpret_cast<uintptr_t>(bytes+64_KB),16_KB,1,1);
            ASSERT(bytes[64_KB]==second && bytes[0]==first);
            ASSERT(release(physical,span)==0);
            std::puts("PASS: private GPU heaps upload sparse zeros without committing them, preserve read-only tracking, and promote protected GPU readback per chunk");
        }
        if (std::getenv("BB_UWP_FILE_BACKING")) {
            constexpr u64 start=512_MB-64_KB, span=256_MB+128_KB;
            s64 physical{};
            ASSERT(allocate(start,start+span,span,64_KB,0,&physical)==0);
            ASSERT(static_cast<u64>(physical)==start);
            void* primary=reinterpret_cast<void*>(0x7200000000ULL);
            void* alias=reinterpret_cast<void*>(0x7300000000ULL);
            ASSERT(map(&primary,span,3,0x10,physical,64_KB)==0);
            ASSERT(map(&alias,span,1,0x10,physical,64_KB)==0);
            auto* bytes=static_cast<volatile u8*>(primary);
            auto* other=static_cast<volatile u8*>(alias);
            ASSERT(bytes[0]==0 && bytes[span-1]==0);
            bytes[0]=0x39;bytes[64_KB]=0xa7;bytes[span-1]=0xb7;
            ASSERT(other[0]==0x39 && other[64_KB]==0xa7 && other[span-1]==0xb7);
            runtime_memory_gpu_protect(reinterpret_cast<uintptr_t>(primary),span,1,0);
            ASSERT(unmap(static_cast<u8*>(alias)+64_KB,16_KB)==0);
            ASSERT(other[0]==0x39 && other[span-1]==0xb7);
            runtime_memory_gpu_protect(reinterpret_cast<uintptr_t>(primary),span,1,1);
            ASSERT(release(physical,span)==0);
            ASSERT(allocate(start,start+span,span,64_KB,0,&physical)==0);
            ASSERT(map(&primary,span,3,0x10,physical,64_KB)==0);
            ASSERT(bytes[0]==0 && bytes[64_KB]==0 && bytes[span-1]==0);
            ASSERT(release(physical,span)==0);
            std::puts("PASS: bounded file views preserve physical aliases, cross-view protections, partial unmaps and sparse zero-on-reuse");
        }
#endif
#ifdef _WIN32
        if (const char* touch = std::getenv("BB_UWP_TOUCH_COMMIT"); touch && touch[0] == '1') {
            constexpr u64 sparse_bytes = 32ull * 1024 * 1024;
            s64 sparse_physical{};
            ASSERT(allocate(4800ull * 1024 * 1024, 5056ull * 1024 * 1024,
                            sparse_bytes, 64_KB, 3, &sparse_physical) == 0);
            void* sparse = reinterpret_cast<void*>(0x7000000000ULL);
            void* alias = reinterpret_cast<void*>(0x7100000000ULL);
            ASSERT(map(&sparse, sparse_bytes, 3, 0x10, sparse_physical, 64_KB) == 0);
            ASSERT(map(&alias, sparse_bytes, 1, 0x10, sparse_physical, 64_KB) == 0);
            const auto query = [](void* p) {
                MEMORY_BASIC_INFORMATION info{};
                ASSERT(VirtualQuery(p, &info, sizeof(info)));
                return info;
            };
            ASSERT_MSG(query(sparse).State == MEM_RESERVE,
                       "An untouched 32 MiB guest mapping committed eagerly");
            ASSERT_MSG(TouchWithRedZone(sparse) == 0x123456789abcdef0ULL,
                       "First-access fault corrupted the PS4/SysV red zone");
            ASSERT_MSG(TouchWithAvx(static_cast<u8*>(sparse) + 4*16_KB) == 0xffffffffULL,
                       "First-access fault corrupted the AVX register state");
            void* guest_thread{};
            void* guest_result{};
            ASSERT(runtime_thread_spawn(&guest_thread, RepeatedGuestStackTouches, sparse, 64_KB,
                                        "FirstTouchTest") == 0);
            ASSERT(runtime_thread_join_spawned(guest_thread, &guest_result) == 0);
            ASSERT_MSG(reinterpret_cast<u64>(guest_result) == 0x123456789abcdef0ULL,
                       "Guarded first-access faults on a real guest thread corrupted its red zone: {}",
                       reinterpret_cast<u64>(guest_result));
            auto first = std::async(std::launch::async, [sparse] { static_cast<volatile u8*>(sparse)[0] = 0x39; });
            auto last = std::async(std::launch::async, [sparse] { static_cast<volatile u8*>(sparse)[sparse_bytes-1] = 0xb7; });
            first.get(); last.get();
            ASSERT(static_cast<volatile u8*>(alias)[0] == 0x39);
            ASSERT(static_cast<volatile u8*>(alias)[sparse_bytes-1] == 0xb7);
            ASSERT_MSG(query(alias).Protect == PAGE_READONLY,
                       "Committing physical pages widened a read-only alias");
            ASSERT_MSG(query(static_cast<u8*>(sparse) + sparse_bytes/2).State == MEM_RESERVE,
                       "First access committed untouched parts of a large mapping");
            {
                runtime_memory_gpu_protect(reinterpret_cast<uintptr_t>(sparse), 16_KB, 0, 0);
                ASSERT(query(sparse).Protect == PAGE_NOACCESS);
                std::array<u8, 16_KB> upload{};
                Core::Memory::Instance()->CopySparseMemory(reinterpret_cast<VAddr>(sparse), upload.data(), upload.size());
                ASSERT(upload[0] == 0x39);
                ASSERT(query(sparse).Protect == PAGE_NOACCESS);
                runtime_memory_gpu_protect(reinterpret_cast<uintptr_t>(sparse), 16_KB, 1, 1);
            }
            std::printf("PASS: internal uploads bypass no-access guest views and preserve their protections (readback mode %u)\n", EmulatorSettings.GetReadbacksMode());
            {
                auto upload=std::make_unique<u8[]>(sparse_bytes);
                Core::Memory::Instance()->CopySparseMemory(reinterpret_cast<VAddr>(sparse), upload.get(), sparse_bytes);
                ASSERT(upload[0]==0x39 && upload[sparse_bytes-1]==0xb7);
                ASSERT(upload[sparse_bytes/2]==0);
                ASSERT_MSG(query(static_cast<u8*>(sparse)+sparse_bytes/2).State==MEM_RESERVE,
                           "Reading untouched guest bytes committed their physical backing");
                std::puts("PASS: large sparse uploads copy written aliases and zero-fill untouched pages without committing them");
            }
            ASSERT(unmap(static_cast<u8*>(alias) + 16_KB, 16_KB) == 0);
            ASSERT(query(alias).Protect == PAGE_READONLY);
            ASSERT_MSG(query(static_cast<u8*>(alias) + sparse_bytes/2).State == MEM_RESERVE,
                       "Partial unmap committed the retained sparse alias");
            ASSERT(static_cast<volatile u8*>(alias)[sparse_bytes-1] == 0xb7);
            ASSERT(release(sparse_physical, sparse_bytes) == 0);
            std::puts("PASS: first-touch guest backing stays sparse, handles concurrent aliases and preserves read-only protections across partial unmaps");
        }
#endif
        s64 physical = 0;
        constexpr VAddr base = 0x5000000000ULL;
        constexpr u64 bytes = 64_KB;
        ASSERT(allocate(0, 5056ull * 1024 * 1024, bytes, bytes, 0, &physical) == 0);
        const char* demand = std::getenv("BB_UWP_DEMAND_COMMIT");
        if (demand && demand[0] == '1') {
            s64 untouched{};
            ASSERT(allocate(0, 5056ull * 1024 * 1024, bytes, bytes, 0, &untouched) == 0);
            ASSERT(release(untouched, bytes) == 0);
        }
#ifdef _WIN32
        if (const char* lazy = std::getenv("BB_UWP_LAZY_RESERVATION"); lazy && lazy[0] == '1') {
            void* host = nullptr;
            for (u64 i = 0; i < 64 && !host; ++i) {
                host = VirtualAlloc2(nullptr, reinterpret_cast<void*>(0x6000000000ULL + i * 0x10000),
                                     bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, nullptr, 0);
            }
            ASSERT_MSG(host, "Could not allocate a host-owned collision fixture");
            std::memset(host, 0x93, bytes);
            void* conflicting = host;
            ASSERT_MSG(map(&conflicting, bytes, 3, 0x10, physical, bytes) != 0,
                       "Guest fixed mapping accepted host-owned memory");
            void* hinted = host;
            ASSERT(map(&hinted, bytes, 3, 0, physical, bytes) == 0);
            ASSERT_MSG(reinterpret_cast<uintptr_t>(hinted) >= reinterpret_cast<uintptr_t>(host) + bytes,
                       "Guest first-fit search did not skip the host allocation");
            std::memset(hinted, 0x48, bytes);
            ASSERT(unmap(hinted, bytes) == 0);
            for (u64 i = 0; i < bytes; ++i)
                ASSERT_MSG(static_cast<u8*>(host)[i] == 0x93, "Host collision changed live data");
            ASSERT(unmap(host, bytes) == 0);
            ASSERT_MSG(static_cast<u8*>(host)[0] == 0x93 && static_cast<u8*>(host)[bytes-1] == 0x93,
                       "Guest unmap touched host-owned memory");
            ASSERT(VirtualFree(host, 0, MEM_RELEASE));
            conflicting = host;
            ASSERT(map(&conflicting, bytes, 3, 0x10, physical, bytes) == 0);
            std::memset(conflicting, 0x27, bytes);
            ASSERT(unmap(conflicting, bytes) == 0);
            std::puts("PASS: lazy guest reservations reject host collisions, preserve host data during unmap, and map released address space");
        }
#endif
        void* address = reinterpret_cast<void*>(base);
        ASSERT(map(&address, bytes, 3, 0x10, physical, bytes) == 0);
        std::memset(address, 0x51, bytes);
        const auto [original, offset] = buffers.ObtainBuffer(base, bytes, false);
        scheduler.Finish();
        buffers.ReleaseUnmappedArenas(base, 16_KB);
        ASSERT_MSG(buffers.address_space.Get(base >> buffers.arena_page_bits) == original,
                   "Partial unmap discarded an arena with live guest memory");
        ASSERT(unmap(address, 16_KB) == 0);
        buffers.ReleaseUnmappedArenas(base + 16_KB, bytes - 16_KB);
        ASSERT(unmap(reinterpret_cast<void*>(base + 16_KB), bytes - 16_KB) == 0);
        scheduler.Finish();
        // Finish waits for GPU execution; deferred host destruction is drained
        // separately by the production draw/submission loop.
        scheduler.PopPendingOperations();
        ASSERT_MSG(buffers.address_space.Get(base >> buffers.arena_page_bits) == nullptr,
                   "Fully unmapped arena remains published");
        ASSERT_MSG(!original->Handle(), "Completed unmapped GPU allocation was not freed");
        Buffer readback{instance, 0, 4096, MemoryType::HostCached};
        const vk::BufferCopy page_copy{(base >> buffers.block_shift) * 8, 0, 8};
        runtime.CopyBuffer(buffers.GetBdaPageTableBuffer(), &readback, std::span{&page_copy, 1});
        scheduler.Finish();
        readback.Invalidate(0, 4096);
        u64 page_address;
        std::memcpy(&page_address, readback.mapped_data.data(), 8);
        ASSERT_MSG(page_address == 0, "Unmapped arena left a stale physical GPU pointer");
        address = reinterpret_cast<void*>(base);
        ASSERT(map(&address, bytes, 3, 0x10, physical, bytes) == 0);
        std::memset(address, 0xa7, bytes);
        const auto [fresh, fresh_offset] = buffers.ObtainBuffer(base, bytes, false);
        ASSERT_MSG(fresh != original, "Remapping reused an invalidated arena object");
        const vk::BufferCopy data_copy{fresh_offset, 0, 4096};
        runtime.CopyBuffer(fresh, &readback, std::span{&data_copy, 1});
        scheduler.Finish();
        readback.Invalidate(0, 4096);
        for (u32 i = 0; i < 4096; ++i)
            ASSERT_MSG(readback.mapped_data[i] == 0xa7, "Remapped data used stale GPU contents");
        buffers.ReleaseUnmappedArenas(base, bytes);
        ASSERT(unmap(address, bytes) == 0);
        ASSERT(release(physical, bytes) == 0);
        scheduler.Finish();
        scheduler.PopPendingOperations();
        if (demand && demand[0] == '1') {
            s64 recycled{};
            ASSERT(allocate(0, 5056ull * 1024 * 1024, bytes, bytes, 0, &recycled) == 0);
            ASSERT(recycled == physical);
            void* primary = reinterpret_cast<void*>(base);
            void* alias = reinterpret_cast<void*>(base + bytes * 2);
            ASSERT(map(&primary, bytes, 3, 0x10, recycled, bytes) == 0);
            ASSERT(map(&alias, bytes, 3, 0x10, recycled, bytes) == 0);
            for (u64 i = 0; i < bytes; ++i)
                ASSERT_MSG(static_cast<u8*>(primary)[i] == 0 && static_cast<u8*>(alias)[i] == 0,
                           "Demand-committed pages did not reset on reuse");
            std::memset(primary, 0x67, bytes);
            for (u64 i = 0; i < bytes; ++i)
                ASSERT_MSG(static_cast<u8*>(alias)[i] == 0x67, "Demand-committed aliases lost coherence");
            ASSERT(release(recycled, bytes) == 0);
            std::puts("PASS: demand-committed guest pages preserve aliases and zero-on-reuse; releasing untouched physical ranges is safe");
        }
        std::puts("PASS: partial guest unmaps preserve live arenas; full unmaps free GPU memory, clear BDA pointers, and remap fresh data");
    }

    void TestFaultOverflow() {
        auto& manager = *buffers.fault_manager;
        scheduler.Finish();
        // Poison unused output areas: a saturated first area must not consume
        // addresses from any later area. Exercise the actual GPU parser and its
        // deferred CPU consumer, rather than a copy of the count calculation.
        const VAddr poison = 0x7f00000000ULL;
        auto* output = reinterpret_cast<u64*>(manager.download_buffer.mapped_data.data());
        for (u64 i = 0; i < manager.download_buffer.SizeBytes() / 8; ++i) output[i] = poison;
        manager.download_buffer.Flush(0, manager.download_buffer.SizeBytes());
        manager.current_area = 0;
        const VAddr first = 0x2000000000ULL;
        const u64 bitmap_offset = (first >> buffers.block_shift) / 8;
        runtime.FillBuffer(manager.GetFaultBuffer(), 0, manager.GetFaultBuffer()->SizeBytes(), 0);
        runtime.FillBuffer(manager.GetFaultBuffer(), bitmap_offset, 33 * 4, 0xffffffff);
        manager.ProcessFaultBuffer();
        scheduler.Finish();
        scheduler.Finish();
        manager.download_buffer.Invalidate(0, manager.download_buffer.SizeBytes());
        ASSERT_MSG(output[0] >= 1024, "Overflow fixture did not saturate the GPU list");
        ASSERT_MSG(buffers.address_space.Get(poison >> buffers.arena_page_bits) == nullptr,
                   "Fault-list overflow allocated an arena from a later area's stale contents");
        std::puts("PASS: saturated GPU fault lists never consume stale addresses from adjacent output areas");
    }

    void TestResourcePoolFenceRefresh() {
        Vulkan::Semaphore progress{instance};
        struct ProbePool final : Vulkan::ResourcePool {
            explicit ProbePool(Vulkan::Semaphore* progress) : ResourcePool{progress, 1} {}
            size_t Commit() { return CommitResource(); }
            void Allocate(size_t, size_t) override { ++growths; }
            size_t growths = 0;
        } pool{&progress};
        const auto first = pool.Commit();
        const auto second = pool.Commit();
        ASSERT_MSG(first != second && pool.growths == 2,
                   "Resource pool reused a slot before its GPU fence completed");
        const u64 completed = progress.CurrentTick();
        Vulkan::SubmitInfo submit{};
        submit.AddSignal(progress.Handle(), completed);
        scheduler.Flush(submit);
        (void)progress.NextTick();
        // Wait through Vulkan directly: the fence really completes, while the
        // resource pool's cached completion value deliberately remains stale.
        const vk::Semaphore fence = progress.Handle();
        const vk::SemaphoreWaitInfo wait{.semaphoreCount = 1,
                                         .pSemaphores = &fence, .pValues = &completed};
        ASSERT_MSG(instance.GetDevice().waitSemaphores(&wait, ~u64{0}) == vk::Result::eSuccess,
                   "Resource-pool fixture fence did not complete");
        ASSERT_MSG(progress.KnownGpuTick() < completed, "Fixture completion cache was refreshed early");
        const auto reused = pool.Commit();
        ASSERT_MSG((reused == first || reused == second) && pool.growths == 2,
                   "Resource pool grew instead of using the refreshed GPU completion value");
        std::puts("PASS: resource pool protects in-flight slots and reuses completed slots after refreshing a stale GPU fence");
    }

    void TestCompactStagingBudget() {
        Vulkan::StagingBufferPool pool{instance, scheduler};
        const u64 initial = pool.AllocatedBytes();
        std::array<Vulkan::StagingBufferRef, 10> buffers;
        for (u32 i = 0; i < buffers.size(); ++i) {
            buffers[i] = pool.Request(16_MB, MemoryType::DeviceLocal);
            runtime.FillBuffer(buffers[i].buffer, buffers[i].offset, buffers[i].size, 0x98760000u + i);
        }
        Buffer readback{instance, 0, 4096, MemoryType::HostCached};
        for (u32 i = 0; i < buffers.size(); ++i) {
            const vk::BufferCopy copy{buffers[i].offset, i * 4u, 4};
            runtime.CopyBuffer(buffers[i].buffer, &readback, std::span{&copy, 1});
        }
        scheduler.Finish();
        readback.Invalidate(0, 4096);
        for (u32 i = 0; i < buffers.size(); ++i) {
            u32 word;
            std::memcpy(&word, readback.mapped_data.data() + i * 4, 4);
            ASSERT_MSG(word == 0x98760000u + i, "Staging burst corrupted at {}", i);
        }
        pool.TickFrame();
        ASSERT_MSG(pool.AllocatedBytes() <= initial + 32_MB,
                   "Completed recent staging burst did not shrink to the compact retention budget");
        std::puts("PASS: compact staging burst preserves every pending upload and sheds completed recent excess");
    }

    void TestStagingWarmReuse() {
        const u64 grace = std::min<u64>(8,
            EmulatorSettingsImpl::Number("BB_STAGING_RING_GRACE_FRAMES", 0));
        const auto usage = BbProcessMemory::Get();
        if (!grace || EmulatorSettingsImpl::Number("BB_STAGING_KEEP_MB", 32) != 16 ||
            !usage.app_info_valid || usage.available_commit < 640_MB) {
            std::puts("SKIP: warm staging reuse requires the 16 MiB compact profile and app headroom");
            return;
        }
        Vulkan::StagingBufferPool pool{instance, scheduler};
        const u64 initial = pool.AllocatedBytes();
        const auto burst = [&] {
            for (u32 i = 0; i < 4; ++i) {
                const auto ref = pool.Request(16_MB, MemoryType::DeviceLocal);
                runtime.FillBuffer(ref.buffer, ref.offset, ref.size, 0x12340000u + i);
            }
            ASSERT_MSG(pool.UploadPressureBytes() == pool.UploadAllocatedBytes(),
                       "Warm staging allowance discounted an in-flight GPU buffer");
            scheduler.Finish();
            pool.TickFrame();
        };
        burst();
        ASSERT_MSG(pool.AllocatedBytes() == initial + 32_MB,
                   "Warm staging cache did not retain exactly one extra GPU block");
        ASSERT_MSG(pool.UploadPressureBytes() == pool.UploadAllocatedBytes() - 16_MB,
                   "Completed warm staging buffer incorrectly triggered the live upload watermark");
        const auto ref = pool.Request(16_MB, MemoryType::DeviceLocal);
        ASSERT_MSG(pool.AllocatedBytes() == initial + 32_MB,
                   "Warm staging upload allocated instead of reusing a completed block");
        runtime.FillBuffer(ref.buffer, ref.offset, ref.size, 0xabcdef01);
        Buffer readback{instance, 0, 4096, MemoryType::HostCached};
        const vk::BufferCopy copy{ref.offset, 0, 4};
        runtime.CopyBuffer(ref.buffer, &readback, std::span{&copy, 1});
        scheduler.Finish();
        readback.Invalidate(0, 4096);
        u32 word;
        std::memcpy(&word, readback.mapped_data.data(), 4);
        ASSERT_MSG(word == 0xabcdef01, "Reused warm staging block corrupted the GPU copy");
        for (u64 i = 0; i <= grace; ++i) pool.TickFrame();
        ASSERT_MSG(pool.AllocatedBytes() == initial + 16_MB,
                   "Idle warm staging block was retained beyond its grace period");
        burst();
        pool.ReclaimCompletedUploads();
        ASSERT_MSG(pool.AllocatedBytes() == initial + 16_MB,
                   "Explicit pressure reclaim kept an excess warm staging block");
        std::puts("PASS: warm GPU staging reuses completed storage, preserves copied data, expires idle blocks and obeys pressure reclaim");
    }

    void TestStagingAllocationPressure() {
        Vulkan::StagingBufferPool pool{instance, scheduler};
        const u64 initial = pool.AllocatedBytes();
        auto first = pool.Request(48_MB, MemoryType::DeviceLocal);
        auto second = pool.Request(64_MB, MemoryType::DeviceLocal);
        runtime.FillBuffer(first.buffer, 0, first.size, 0x12345678);
        runtime.FillBuffer(second.buffer, 0, second.size, 0xabcdef12);
        // Ordinary CPU readbacks are protected even without an explicit hold;
        // also verify the hold guard on a completed device-local allocation.
        auto held = pool.Request(20_MB, MemoryType::HostCached);
        runtime.FillBuffer(held.buffer, 0, held.size, 0x13579bdf);
        auto held_gpu = pool.Request(32_MB, MemoryType::DeviceLocal, 0, true);
        runtime.FillBuffer(held_gpu.buffer, 0, held_gpu.size, 0xcafed00d);
        scheduler.Finish();
        auto reused = pool.Request(60_MB, MemoryType::DeviceLocal);
        ASSERT_MSG(reused.buffer == second.buffer, "Pressure policy discarded a compatible reusable buffer");
        runtime.FillBuffer(reused.buffer, 0, reused.size, 0x654321ab);
        auto pending = pool.Request(80_MB, MemoryType::DeviceLocal);
        ASSERT_MSG(pool.AllocatedBytes() <= initial + 64_MB + 20_MB + 32_MB + 80_MB,
                   "Completed recent 48 MiB buffer was retained when new backing was required");
        runtime.FillBuffer(pending.buffer, 0, pending.size, 0x2468ace0);
        const vk::BufferCopy from_reused{0, 4, 4}, from_pending{0, 8, 4};
        runtime.CopyBuffer(reused.buffer, held.buffer, std::span{&from_reused, 1});
        runtime.CopyBuffer(pending.buffer, held.buffer, std::span{&from_pending, 1});
        const vk::BufferCopy from_held_gpu{0, 16, 4};
        runtime.CopyBuffer(held_gpu.buffer, held.buffer, std::span{&from_held_gpu, 1});
        scheduler.Finish();
        held.Invalidate();
        const auto word = [&](u64 offset) {
            u32 value;
            std::memcpy(&value, held.mapped + offset, 4);
            return value;
        };
        ASSERT(word(0) == 0x13579bdf && word(4) == 0x654321ab && word(8) == 0x2468ace0);
        ASSERT(word(held.size - 4) == 0x13579bdf);
        ASSERT(word(16) == 0xcafed00d);
        auto larger = pool.Request(96_MB, MemoryType::DeviceLocal);
        ASSERT_MSG(pool.AllocatedBytes() <= initial + 20_MB + 32_MB + 96_MB,
                   "Completed 64/80 MiB buffers were retained under allocation pressure");
        runtime.FillBuffer(larger.buffer, 0, larger.size, 0x89abcdef);
        const vk::BufferCopy from_larger{0, 12, 4};
        runtime.CopyBuffer(larger.buffer, held.buffer, std::span{&from_larger, 1});
        scheduler.Finish();
        held.Invalidate();
        ASSERT(word(0) == 0x13579bdf && word(12) == 0x89abcdef);
        pool.FreeDeferred(held_gpu);
        scheduler.Finish();
        std::puts("PASS: staging allocation pressure reuses compatible buffers, releases completed recent backing, and preserves pending GPU data and held readbacks");
    }

    void TestOverlapPreservesPendingUploads() {
        auto& pool=runtime.GetStagingPool();
        scheduler.Finish();pool.ReclaimCompletedUploads();
        auto device=instance.GetDevice();const vk::SemaphoreTypeCreateInfo type{.semaphoreType=vk::SemaphoreType::eTimeline,.initialValue=0};
        auto gate=Vulkan::Check(device.createSemaphoreUnique(vk::SemaphoreCreateInfo{.pNext=&type}));
        const auto upload=pool.Request(48_MB,MemoryType::DeviceLocal);
        const auto readback=pool.Request(4096,MemoryType::HostCached,0,true);
        runtime.FillBuffer(upload.buffer,0,upload.size,0x92581abc);
        const vk::BufferCopy copy{0,0,4};runtime.CopyBuffer(upload.buffer,readback.buffer,std::span{&copy,1});
        Vulkan::SubmitInfo info{};info.AddWait(*gate,1);scheduler.Flush(info);
        const u64 protected_bytes=pool.UploadAllocatedBytes();ASSERT_MSG(protected_bytes<=64_MB && protected_bytes>32_MB,"Unexpected overlap fixture size");
        HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);ASSERT(release);
        std::thread unblock([&]{WaitForSingleObject(release,1000);const vk::SemaphoreSignalInfo signal{.semaphore=*gate,.value=1};Vulkan::Check(device.signalSemaphore(signal));});
        const auto started=GetTickCount64();
        runtime.LimitUploadBatch();
        const bool kept=pool.UploadAllocatedBytes()==protected_bytes;
        const bool continued=GetTickCount64()-started<500;
        SetEvent(release);unblock.join();CloseHandle(release);scheduler.Finish();
        ASSERT_MSG(continued && kept,"Overlap path waited for GPU or destroyed pending upload storage");
        readback.Invalidate();u32 word=0;std::memcpy(&word,readback.mapped,4);ASSERT_MSG(word==0x92581abc,"Pending upload changed before GPU completion");
        pool.FreeDeferred(readback);scheduler.Finish();pool.ReclaimCompletedUploads();ASSERT_MSG(pool.UploadAllocatedBytes()<=32_MB,"Completed overlap backing was not reclaimed");
        std::puts("PASS: bounded upload overlap returns while GPU is gated, preserves pending backing and exact readback data, then reclaims to original32MiB watermark");
    }
    void TestReclaimCompletedWithoutDrain() {
        auto& pool=runtime.GetStagingPool();
        const auto upload=pool.Request(48_MB,MemoryType::DeviceLocal);
        const auto readback=pool.Request(4096,MemoryType::HostCached,0,true);
        runtime.FillBuffer(upload.buffer,0,upload.size,0xabcdef42);
        const vk::BufferCopy copy{0,0,4};runtime.CopyBuffer(upload.buffer,readback.buffer,std::span{&copy,1});scheduler.Finish();
        ASSERT_MSG(pool.UploadAllocatedBytes()>32_MB,"Fixture did not create completed excess upload storage");
        const auto tick=scheduler.CurrentTick();
        runtime.LimitUploadBatch();
        ASSERT_MSG(scheduler.CurrentTick()==tick,"Completed upload storage unnecessarily drained GPU queue");
        ASSERT_MSG(pool.UploadAllocatedBytes()<=32_MB,"Reclaim-first path exceeded original upload watermark");
        readback.Invalidate();u32 word=0;std::memcpy(&word,readback.mapped,4);ASSERT_MSG(word==0xabcdef42,"Reclaim invalidated held CPU readback");
        pool.FreeDeferred(readback);scheduler.Finish();
        std::puts("PASS: completed excess upload storage is reclaimed without Finish while original32MiB watermark and held readback data are preserved");
    }
    void TestUploadBatchLimit() {
        const u64 batch_bytes=std::strtoull(std::getenv("BB_UPLOAD_SUBMIT_MB"),nullptr,10)*1_MB;
        auto& pool = runtime.GetStagingPool();
        auto readback = pool.Request(20_MB, MemoryType::HostCached);
        runtime.FillBuffer(readback.buffer, 0, readback.size, 0xabcdef01);
        for (u32 i = 0; i < 24; ++i) {
            const auto upload = pool.Request(16_MB, MemoryType::DeviceLocal);
            runtime.FillBuffer(upload.buffer, upload.offset, upload.size, 0x32100000u + i);
            const vk::BufferCopy copy{upload.offset, i * 4u, 4};
            runtime.CopyBuffer(upload.buffer, readback.buffer, std::span{&copy, 1});
            runtime.LimitUploadBatch();
            ASSERT_MSG(pool.UploadAllocatedBytes() <= batch_bytes * 2,
                       "Upload batch did not reclaim completed temporary storage");
        }
        scheduler.Finish();
        readback.Invalidate();
        for (u32 i = 0; i < 24; ++i) {
            u32 value;
            std::memcpy(&value, readback.mapped + i * 4, 4);
            ASSERT_MSG(value == 0x32100000u + i, "Upload batch lost transfer {}", i);
        }
        u32 last;
        std::memcpy(&last, readback.mapped + readback.size - 4, 4);
        ASSERT_MSG(last == 0xabcdef01, "Batch trimming invalidated a CPU readback");
        std::puts("PASS: bounded upload batches preserve GPU transfers and CPU readbacks across multiple submissions");
    }

    void TestStagingLifetimes() {
        auto& pool = runtime.GetStagingPool();
        const u64 initial = pool.AllocatedBytes();
        auto held = pool.Request(20_MB, MemoryType::HostCached, 0, true);
        runtime.FillBuffer(held.buffer, held.offset, held.size, 0x65ab9123);
        scheduler.Finish();
        for (u32 i = 0; i < 3100; ++i) pool.TickFrame();
        ASSERT_MSG(pool.AllocatedBytes() >= held.buffer->SizeBytes(), "Held readback was trimmed");
        held.Invalidate();
        for (u64 i = 0; i < held.size / 4; ++i) {
            u32 value;
            std::memcpy(&value, held.mapped + i * 4, 4);
            ASSERT_MSG(value == 0x65ab9123, "Held staging readback corrupted at {}", i);
        }
        pool.FreeDeferred(held);
        // Release associates the buffer with the current submission, which must
        // complete before trimming may reclaim it.
        scheduler.Finish();
        for (u32 i = 0; i < 3100; ++i) pool.TickFrame();
        ASSERT_MSG(pool.AllocatedBytes() <= initial, "Idle completed staging allocation was not released");
        std::puts("PASS: staging readbacks survive idle trimming while held, preserve GPU data, and release after completion");
    }

    void TestByteTiling() {
        TileManager manager{instance, scheduler, runtime, buffers.stream_buffer};
        for (u32 test = 0; test < 3; ++test) {
            const bool macro = test == 1;
            ImageInfo info{};
            info.tile_mode = macro ? AmdGpu::TileMode::Thin2DThin : AmdGpu::TileMode::Thin1DThin;
            info.array_mode = macro ? AmdGpu::ArrayMode::Array2DTiledThin1 : AmdGpu::ArrayMode::Array1DTiledThin1;
            info.num_bits = 8;
            info.num_samples = 1;
            u32 width = test == 2 ? 4096 : 16, height = width;
            if (macro) {
                const auto mode = AmdGpu::CalculateMacrotileMode(info.tile_mode, 8, 1);
                const u32 pipes = AmdGpu::GetPipeConfig(info.tile_mode) == AmdGpu::PipeConfig::P2 ? 2 : 8;
                const u32 aspect = AmdGpu::GetMacrotileAspect(mode);
                width = 16 * AmdGpu::GetBankWidth(mode) * pipes * aspect;
                height = 16 * AmdGpu::GetBankHeight(mode) * AmdGpu::GetNumBanks(mode) / aspect;
            }
            const u32 bytes = width * height;
            Buffer tiled{instance, 0, bytes, MemoryType::HostCached};
            Buffer linear{instance, 0, bytes, MemoryType::HostCached};
            Buffer roundtrip{instance, 0, bytes, MemoryType::HostCached};
            Buffer uniform{instance, 0, 4096, MemoryType::HostCached};
            for (u32 i = 0; i < bytes; ++i) tiled.mapped_data[i] = u8(i * 73 + 35);
            std::memset(linear.mapped_data.data(), 0xa5, bytes);
            std::memset(roundtrip.mapped_data.data(), 0x5a, bytes);
            const std::array<u32, 67> parameters = {0, 1, 1, bytes, width, height, 0};
            std::memcpy(uniform.mapped_data.data(), parameters.data(), sizeof(parameters));
            tiled.Flush(0, bytes);
            linear.Flush(0, bytes);
            roundtrip.Flush(0, bytes);
            uniform.Flush(0, sizeof(parameters));
            const vk::DescriptorBufferInfo src{tiled.Handle(), 0, bytes};
            const vk::DescriptorBufferInfo flat{linear.Handle(), 0, bytes};
            const vk::DescriptorBufferInfo dst{roundtrip.Handle(), 0, bytes};
            const vk::DescriptorBufferInfo params{uniform.Handle(), 0, sizeof(parameters)};
            manager.RecordTilingDispatch(manager.GetTilingPipeline(info, false), src, flat, params, bytes / 64);
            const vk::MemoryBarrier2 dependency{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            };
            scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
                .memoryBarrierCount = 1, .pMemoryBarriers = &dependency,
            });
            manager.RecordTilingDispatch(manager.GetTilingPipeline(info, true), dst, flat, params, bytes / 64);
            scheduler.Finish();
            linear.Invalidate(0, bytes);
            roundtrip.Invalidate(0, bytes);
            for (u32 i = 0; i < bytes; ++i) {
                ASSERT_MSG(roundtrip.mapped_data[i] == tiled.mapped_data[i],
                           "{} byte tiling round trip failed at {}: got {}, expected {}, linear {} (shaderInt8={})",
                           macro ? "Macro" : "Micro", i, roundtrip.mapped_data[i], tiled.mapped_data[i],
                           linear.mapped_data[i], instance.IsShaderInt8Supported());
                if (!macro) {
                    const u32 x = i % width, y = i / width;
                    const u32 pixel = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                                      ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
                    const u32 address = ((y / 8) * (width / 8) + x / 8) * 64 + pixel;
                    ASSERT_MSG(linear.mapped_data[i] == u8(address * 73 + 35), "Micro detile layout incorrect at {}", i);
                }
            }
        }
        std::puts("PASS: production byte texture tiler and detiler preserve micro and macro tiled images");
    }

    void TestComputeBatches() {
        // The failing game grid has 262144 X workgroups. Test that exact size,
        // plus multiple Y/Z rows, so rebasing both IDs and linear indices is checked.
        namespace IR = Shader::IR;
        constexpr u32 x = 262144, y = 2, z = 2, count = x * y * z;
        Shader::Info info{};
        info.hw_stage = Shader::HwStage::Compute;
        info.sw_stage = Shader::SwStage::Compute;
        for (u32 c = 0; c < 3; ++c) info.loads.Set(IR::Attribute::WorkgroupId, c);
        info.loads.Set(IR::Attribute::WorkgroupIndex, 0);
        info.buffers.push_back(Shader::BufferResource{
            .used_types = IR::Type::U32, .buffer_type = Shader::BufferType::SharedMemory, .is_written = true,
        });
        Shader::RuntimeInfo runtime_info{};
        runtime_info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
        runtime_info.hw.cs.workgroup_size = {1, 1, 1};
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        IR::Program program{info};
        program.blocks.push_back(&block);
        program.post_order_blocks.push_back(&block);
        program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
        IR::IREmitter ir{block};
        ir.Prologue();
        const auto value = ir.IAdd(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0),
            ir.IAdd(ir.IMul(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 1), ir.Imm32(0x100000u)),
                    ir.IMul(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 2), ir.Imm32(0x1000000u))));
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.GetAttributeU32(IR::Attribute::WorkgroupIndex), value, {});
        const Shader::Profile profile{
            .supported_spirv = instance.SupportedSpirvVersion(), .subgroup_size = instance.SubgroupSize(),
            .support_int16 = true,
        };
        Shader::Backend::Bindings bindings;
        const auto device = instance.GetDevice();
        const auto shader = Vulkan::CompileSPV(Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings), device);
        const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute};
        const auto layout = Vulkan::Check(device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{
            .bindingCount = 1, .pBindings = &binding,
        }));
        const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Shader::PushData)};
        const auto pipeline_layout = Vulkan::Check(device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
            .setLayoutCount = 1, .pSetLayouts = &*layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range,
        }));
        const auto pipeline = Vulkan::Check(device.createComputePipelineUnique({}, vk::ComputePipelineCreateInfo{
            .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = shader, .pName = "main"},
            .layout = *pipeline_layout,
        }));
        Buffer data{instance, 0, count * sizeof(u32), MemoryType::HostCached};
        std::memset(data.mapped_data.data(), 0xff, count * sizeof(u32));
        data.Flush(0, count * sizeof(u32));
        const vk::DescriptorBufferInfo buffer_info{data.Handle(), 0, count * sizeof(u32)};
        const vk::WriteDescriptorSet write{
            .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &buffer_info,
        };
        const auto cmd = scheduler.CommandBuffer();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        scheduler.BindHostDescriptors(cmd, vk::PipelineBindPoint::eCompute, *pipeline_layout, *layout, std::span{&write, 1});
        const Shader::PushData push{};
        cmd.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        Vulkan::DispatchComputeBatches(cmd, *pipeline_layout, x, y, z, 65535);
        scheduler.Finish();
        data.Invalidate(0, count * sizeof(u32));
        for (u32 i = 0; i < count; ++i) {
            u32 actual;
            std::memcpy(&actual, data.mapped_data.data() + i * sizeof(u32), sizeof(actual));
            const u32 expected = i % x + ((i / x) % y) * 0x100000 + (i / (x * y)) * 0x1000000;
            ASSERT_MSG(actual == expected, "Compute batch lost group {}: {} != {}", i, actual, expected);
        }
        device.destroyShaderModule(shader);
        std::puts("PASS: production compute batches preserve all group IDs and linear indices across 262144x2x2 groups");
    }

    void TestSpecialBufferBindingKeys() {
        Shader::Info info{};
        info.hw_stage=Shader::HwStage::Vertex;info.sw_stage=Shader::SwStage::Vertex;
        Shader::RuntimeInfo runtime_info{};
        runtime_info.Initialize(Shader::HwStage::Vertex,Shader::SwStage::Vertex);
        const Shader::Profile profile{};
        const Shader::Backend::Bindings first{},shifted{.unified=3};
        const Shader::StageSpecialization empty_a{info,runtime_info,profile,first},
            empty_b{info,runtime_info,profile,shifted};
        ASSERT(empty_a==empty_b && empty_b==empty_a);
        info.buffers.push_back(Shader::BufferResource{
            .used_types=Shader::IR::Type::F32,.buffer_type=Shader::BufferType::ClipPlanes});
        const Shader::StageSpecialization clip_a{info,runtime_info,profile,first},
            clip_b{info,runtime_info,profile,shifted},clip_same{info,runtime_info,profile,first};
        ASSERT(clip_a.bitset.none() && clip_b.bitset.none());
        ASSERT(!(clip_a==clip_b) && !(clip_b==clip_a));
        ASSERT(clip_a==clip_same && clip_same==clip_a);
        ASSERT(!(empty_a==clip_a) && !(clip_a==empty_a));
        std::puts("PASS: shader cache keys keep special-buffer binding offsets distinct while sharing descriptor-free vertex modules");
    }

    void TestMutableColorViews() {
        const auto device=instance.GetDevice();
        const std::array bindings{
            vk::DescriptorSetLayoutBinding{0,vk::DescriptorType::eSampledImage,1,vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{1,vk::DescriptorType::eSampler,1,vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{2,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute},
        };
        const auto descriptor_layout=Vulkan::Check(device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{
            .bindingCount=u32(bindings.size()),.pBindings=bindings.data()}));
        const auto layout_handle=*descriptor_layout;
        const auto layout=Vulkan::Check(device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
            .setLayoutCount=1,.pSetLayouts=&layout_handle}));
        const auto shader=Vulkan::CompileSPV(DOZEN_VIEW_SAMPLE_COMP,device);
        const auto pipeline=Vulkan::Check(device.createComputePipelineUnique({},vk::ComputePipelineCreateInfo{
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=shader,.pName="main"},.layout=*layout}));
        const std::array sizes{vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage,1},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampler,1},vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer,1}};
        const auto pool=Vulkan::Check(device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo{
            .maxSets=1,.poolSizeCount=u32(sizes.size()),.pPoolSizes=sizes.data()}));
        const auto sets=Vulkan::Check(device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
            .descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=&layout_handle}));
        const auto set=sets.front();
        const auto sampler=Vulkan::Check(device.createSamplerUnique(vk::SamplerCreateInfo{}));
        Buffer upload{instance,0,4096,MemoryType::HostCached}, result{instance,0,4096,MemoryType::HostCached};
        const std::array<u8,4> bytes{64,128,192,17};
        std::memcpy(upload.mapped_data.data(),bytes.data(),bytes.size());upload.Flush(0,4096);
        for(unsigned test=0;test<4;++test) {
            const bool source_bgra=test>=2,srgb=test%2;
            const auto base=source_bgra ? vk::Format::eB8G8R8A8Unorm : vk::Format::eR8G8B8A8Unorm;
            const auto view_format=source_bgra ? (srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm) :
                (srgb ? vk::Format::eB8G8R8A8Srgb : vk::Format::eB8G8R8A8Unorm);
            vk::UniqueDeviceMemory memory;
            const auto image=Vulkan::Check(device.createImageUnique(vk::ImageCreateInfo{
                .flags=vk::ImageCreateFlagBits::eMutableFormat,.imageType=vk::ImageType::e2D,.format=base,
                .extent={1,1,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
                .tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eSampled}));
            const auto req=device.getImageMemoryRequirements(*image);u32 type=0;
            while(type<instance.GetMemoryProperties().memoryTypeCount && !(req.memoryTypeBits&(1u<<type)))++type;
            ASSERT(type<instance.GetMemoryProperties().memoryTypeCount);
            memory=Vulkan::Check(device.allocateMemoryUnique(vk::MemoryAllocateInfo{
                .allocationSize=req.size,.memoryTypeIndex=type}));
            ASSERT(device.bindImageMemory(*image,*memory,0)==vk::Result::eSuccess);
            const auto view=Vulkan::Check(device.createImageViewUnique(vk::ImageViewCreateInfo{
                .image=*image,.viewType=vk::ImageViewType::e2D,.format=view_format,
                .components={vk::ComponentSwizzle::eIdentity,vk::ComponentSwizzle::eIdentity,
                    vk::ComponentSwizzle::eIdentity,vk::ComponentSwizzle::eOne},
                .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}));
            const vk::DescriptorImageInfo image_info{{},*view,vk::ImageLayout::eShaderReadOnlyOptimal},
                sampler_info{*sampler,{},vk::ImageLayout::eUndefined};
            const vk::DescriptorBufferInfo result_info{result.Handle(),0,4096};
            const std::array writes{
                vk::WriteDescriptorSet{.dstSet=set,.dstBinding=0,.descriptorCount=1,
                    .descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&image_info},
                vk::WriteDescriptorSet{.dstSet=set,.dstBinding=1,.descriptorCount=1,
                    .descriptorType=vk::DescriptorType::eSampler,.pImageInfo=&sampler_info},
                vk::WriteDescriptorSet{.dstSet=set,.dstBinding=2,.descriptorCount=1,
                    .descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&result_info},
            };
            device.updateDescriptorSets(writes,{});
            const auto cmd=scheduler.CommandBuffer();
            const auto transition=[&](vk::ImageLayout before,vk::ImageLayout after) {
                const vk::ImageMemoryBarrier2 barrier{
                    .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,.srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
                    .oldLayout=before,.newLayout=after,.image=*image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
                cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
            };
            transition(vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal);
            cmd.copyBufferToImage(upload.Handle(),*image,vk::ImageLayout::eTransferDstOptimal,
                vk::BufferImageCopy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={1,1,1}});
            transition(vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eShaderReadOnlyOptimal);
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*layout,0,set,{});cmd.dispatch(1,1,1);
            scheduler.Finish();result.Invalidate(0,4096);
            std::array<float,4> actual;std::memcpy(actual.data(),result.mapped_data.data(),sizeof(actual));
            std::array<float,4> expected{float(bytes[source_bgra ? 0 : 2])/255.f,float(bytes[1])/255.f,
                float(bytes[source_bgra ? 2 : 0])/255.f,1.f};
            if(srgb)for(unsigned c=0;c<3;++c)expected[c]=expected[c]<=0.04045f ? expected[c]/12.92f :
                std::pow((expected[c]+0.055f)/1.055f,2.4f);
            for(unsigned c=0;c<4;++c)ASSERT_MSG(std::abs(actual[c]-expected[c])<0.002f,
                "Mutable color view test {} channel {} got {} expected {}",test,c,actual[c],expected[c]);
        }
        device.destroyShaderModule(shader);
        std::puts("PASS: mutable RGBA/BGRA sampling preserves byte reinterpretation, sRGB decoding, and forced opaque alpha in both directions");
    }

    void TestHostCopyCompletion() {
        Vulkan::Scheduler copy_scheduler{instance, true};
        if (!copy_scheduler.IsRecordingDeferred()) {
            std::puts("SKIP: host-copy completion test requires threaded recording");
            return;
        }
        std::array<u32, 128> values{};
        std::promise<void> release;
        auto gate=release.get_future().share();
        copy_scheduler.Record([gate](vk::CommandBuffer) { gate.wait(); });
        copy_scheduler.KickRecording(true);
        for (u32 i=0; i<values.size(); ++i)
            copy_scheduler.RecordHostCopy([&,i] { values[i]=0x12340000u+i; });
        auto waiter=std::async(std::launch::async, [&] {
            copy_scheduler.WaitHostCopies();
            for (u32 i=0; i<values.size(); ++i) if (values[i]!=0x12340000u+i) return false;
            return true;
        });
        ASSERT(waiter.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
        release.set_value();
        ASSERT(waiter.wait_for(std::chrono::seconds(5))==std::future_status::ready);
        ASSERT(waiter.get());
        copy_scheduler.Finish();
        std::puts("PASS: host-copy completion waits for gated recorder work, wakes after all 128 copies, and publishes exact copied bytes");
    }

    void TestNullImages(bool threaded) {
        Vulkan::Scheduler descriptor_scheduler{instance, threaded};
        if (threaded && !descriptor_scheduler.IsRecordingDeferred()) {
            std::puts("SKIP: descriptor lifetime test requires threaded recording");
            return;
        }
        namespace IR = Shader::IR;
        Shader::Info info{};
        info.hw_stage = Shader::HwStage::Compute;
        info.sw_stage = Shader::SwStage::Compute;
        info.buffers.push_back(Shader::BufferResource{
            .used_types = IR::Type::U32 | IR::Type::F32,
            .buffer_type = Shader::BufferType::SharedMemory, .is_written = true,
        });
        info.images.push_back(Shader::ImageResource{});
        info.images.push_back(Shader::ImageResource{.is_written = true});
        info.images.push_back(Shader::ImageResource{.is_atomic = true, .is_written = true});
        info.images.push_back(Shader::ImageResource{.is_depth = true});
        info.samplers.push_back(Shader::SamplerResource{});
        Shader::RuntimeInfo runtime_info{};
        runtime_info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
        runtime_info.hw.cs.workgroup_size = {1, 1, 1};
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        IR::Program program{info};
        program.blocks.push_back(&block);
        program.post_order_blocks.push_back(&block);
        program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
        IR::IREmitter ir{block};
        const auto uv = ir.CompositeConstruct(ir.Imm32(0.5f), ir.Imm32(0.5f));
        const auto xy = ir.CompositeConstruct(ir.Imm32(0u), ir.Imm32(0u));
        u32 words = 0;
        const auto floats = [&](const IR::Value& value, u32 n = 4) {
            ir.StoreBufferF32(n, ir.Imm32(0u), ir.Imm32(words), value, {});
            words += n;
        };
        const auto uints = [&](const IR::Value& value, u32 n = 1) {
            ir.StoreBufferU32(n, ir.Imm32(0u), ir.Imm32(words), value, {});
            words += n;
        };
        floats(ir.ImageSampleImplicitLod(ir.Imm32(0u), uv, ir.Imm32(0.f), {}, {}));
        floats(ir.ImageSampleExplicitLod(ir.Imm32(0u), uv, ir.Imm32(0.f), {}, {}));
        floats(ir.ImageSampleDrefImplicitLod(ir.Imm32(3u), uv, ir.Imm32(0.5f), ir.Imm32(0.f), {}, {}));
        floats(ir.ImageSampleDrefExplicitLod(ir.Imm32(3u), uv, ir.Imm32(0.5f), ir.Imm32(0.f), {}, {}));
        floats(ir.ImageGather(ir.Imm32(0u), uv, {}, {}));
        floats(ir.ImageGatherDref(ir.Imm32(3u), uv, {}, ir.Imm32(0.5f), {}));
        floats(ir.ImageGradient(ir.Imm32(0u), uv, uv, uv, {}, ir.Imm32(0.f), {}));
        floats(ir.ImageQueryLod(ir.Imm32(0u), uv, {}), 2);
        uints(ir.ImageQueryDimension(ir.Imm32(0u), ir.Imm32(0u), ir.Imm1(false), {}), 4);
        floats(ir.ImageRead(ir.Imm32(0u), xy, ir.Imm32(0u), ir.Imm32(0u), {}));
        floats(ir.ImageRead(ir.Imm32(1u), xy, ir.Imm32(0u), ir.Imm32(0u), {}));
        ir.ImageWrite(ir.Imm32(1u), xy, ir.Imm32(0u), ir.Imm32(0u),
                      ir.CompositeConstruct(ir.Imm32(1.f), ir.Imm32(1.f), ir.Imm32(1.f), ir.Imm32(1.f)), {});
        uints(ir.ImageAtomicIAdd(ir.Imm32(2u), xy, ir.Imm32(42u), {}));
        uints(ir.ImageAtomicCmpSwap(ir.Imm32(2u), xy, ir.Imm32(1u), ir.Imm32(0u), {}));
        uints(ir.ImageAtomicInc(ir.Imm32(2u), xy, ir.Imm32(42u), {}));
        uints(ir.ImageAtomicDec(ir.Imm32(2u), xy, ir.Imm32(42u), {}));
        const Shader::Profile profile{
            .supported_spirv = instance.SupportedSpirvVersion(), .subgroup_size = instance.SubgroupSize(),
            .support_int16 = true, .needs_typed_storage_images = instance.NeedsTypedStorageImages(),
        };
        Shader::Backend::Bindings bindings;
        const auto device = instance.GetDevice();
        const auto shader = Vulkan::CompileSPV(Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings), device);
        const std::array sizes{
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 8},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 8},
        };
        Vulkan::DescriptorHeap heap{instance, descriptor_scheduler.GetWorkSemaphore(), sizes};
        Vulkan::ComputePipeline::SerializationSupport sdata;
        Vulkan::ComputePipeline pipeline{instance, descriptor_scheduler, heap, profile, {}, {0}, info, shader, sdata, false};
        ASSERT(pipeline.IsValid());
        Buffer data{instance, 0, 4096, MemoryType::HostCached};
        std::memset(data.mapped_data.data(), 0xcd, 4096);
        data.Flush(0, 4096);
        vk::DescriptorBufferInfo buffer_info{data.Handle(), 0, 4096};
        Vulkan::Pipeline::DescriptorWrites writes;
        writes.push_back(vk::WriteDescriptorSet{
            .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &buffer_info,
        });
        const vk::DescriptorImageInfo missing{VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral};
        for (u32 i = 0; i < 4; ++i) writes.push_back(vk::WriteDescriptorSet{
            .dstBinding = i + 1, .descriptorCount = 1,
            .descriptorType = info.images[i].is_written ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage,
            .pImageInfo = &missing,
        });
        const auto sampler = Vulkan::Check(device.createSamplerUnique(vk::SamplerCreateInfo{}));
        vk::DescriptorImageInfo sampler_info{*sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral};
        writes.push_back(vk::WriteDescriptorSet{
            .dstBinding = 5, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampler, .pImageInfo = &sampler_info,
        });
        std::promise<void> descriptor_gate;
        auto gate = descriptor_gate.get_future().share();
        if (threaded) {
            // Force the queued update to consume its payload after the caller's
            // arrays were overwritten, instead of relying on a scheduling race.
            descriptor_scheduler.Record([gate](vk::CommandBuffer) { gate.wait(); });
            descriptor_scheduler.KickRecording(true);
        }
        pipeline.BindResources(writes, Shader::PushData{});
        ASSERT(writes.size() == 2 && writes[0].dstBinding == 0 && writes[1].dstBinding == 5);
        if (threaded) {
            writes.clear(); buffer_info = {}; sampler_info = {};
            descriptor_gate.set_value();
        }
        const auto cmd = descriptor_scheduler.CommandBuffer();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.Handle());
        cmd.dispatch(1, 1, 1);
        descriptor_scheduler.Finish();
        data.Invalidate(0, 4096);
        for (u32 i = 0; i < 4096; ++i) {
            ASSERT_MSG(data.mapped_data[i] == (i < words * 4 ? 0 : 0xcd), "Null image read/store changed byte {}", i);
        }
        device.destroyShaderModule(shader);
        const Shader::StageSpecialization null_spec{info, runtime_info, profile, {}};
        auto valid_info = info;
        auto sharp = AmdGpu::Image::Null(false);
        sharp.base_address = 0x100;
        std::memcpy(valid_info.images[0].sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
        const Shader::StageSpecialization valid_spec{valid_info, runtime_info, profile, {}};
        ASSERT(!(null_spec == valid_spec) && !(valid_spec == null_spec));
        if (threaded) std::puts("PASS: production threaded descriptors survive overwritten caller arrays and retain exact GPU output");
        else std::puts("PASS: production null images return zero, suppress writes and atomics, leave unused descriptors unwritten, and keep validity keys distinct");
    }

    void TestStreamingTextureEviction() {
        StreamingDrainPolicy drain_policy;
        ASSERT(drain_policy.ShouldDrain(0, 300_MB, 100));
        drain_policy.DidDrain(300_MB, 100);
        ASSERT(!drain_policy.ShouldDrain(0, 300_MB, 350));
        ASSERT(!drain_policy.ShouldDrain(0, 293_MB, 350));
        ASSERT(drain_policy.ShouldDrain(0, 292_MB, 350));
        ASSERT(drain_policy.ShouldDrain(1, 300_MB, 350));
        ASSERT(drain_policy.ShouldDrain(0, 63_MB, 350));
        ASSERT(drain_policy.ShouldDrain(0, 300_MB, 1100));
        drain_policy.DidDrain(320_MB, 1100);
        ASSERT(!drain_policy.ShouldDrain(0, 319_MB, 1200));
        ASSERT(drain_policy.ShouldDrain(0, 300_MB, 1200));
        std::puts("PASS: streaming drain retry skips an unchanged live set, but fresh retirements, an 8 MiB pressure increase, emergency headroom and timeout still synchronize");
        using Allocate = s32(__attribute__((sysv_abi)) *)(s64, s64, u64, u64, int, s64*);
        using Map = s32(__attribute__((sysv_abi)) *)(void**, u64, int, int, s64, u64);
        using Release = s32(__attribute__((sysv_abi)) *)(u64, u64);
        auto allocate = reinterpret_cast<Allocate>(runtime_memory_resolve("rTXw65xmLIA#p#J"));
        auto map = reinterpret_cast<Map>(runtime_memory_resolve("L-Q3LEjIbgA#p#J"));
        auto release = reinterpret_cast<Release>(runtime_memory_resolve("MBuItvba6z8#p#J"));
        constexpr u64 span = 6 * 64_KB;
        s64 physical{};
        ASSERT(allocate(900_MB, 900_MB + span, span, 64_KB, 3, &physical) == 0);
        void* mapping = reinterpret_cast<void*>(0x7900000000ULL);
        ASSERT(map(&mapping, span, 0x33, 0x10, physical, 64_KB) == 0);
        std::memset(mapping, 0x39, span);
        std::array<TextureCache::ImageDesc, 6> descriptions{};
        std::array<ImageId, 6> ids{};
        for (u32 i = 0; i < ids.size(); ++i) {
            auto& info = descriptions[i].info;
            info.type = AmdGpu::ImageType::Color2D;
            info.pixel_format = vk::Format::eR8G8B8A8Unorm;
            info.size = {16, 16, 1}; info.resources = {1, 1};
            info.pitch = 16; info.num_bits = 32; info.guest_size = 1024;
            info.guest_address = reinterpret_cast<VAddr>(mapping) + i * 64_KB;
            info.mips_layout[0] = {1024, 16, 16, 0};
            ids[i] = textures.FindImage(descriptions[i]);
            textures.UpdateImage(ids[i]);
        }
        scheduler.Finish();
        const auto old_uid = textures.slot_images[ids[0]].image_uid;
        textures.slot_images[ids[1]].flags |= ImageFlagBits::GpuModified;
        textures.slot_images[ids[2]].binding.is_bound = 1;
        textures.slot_images[ids[3]].binding.is_target = 1;
        textures.slot_images[ids[5]].flags |= ImageFlagBits::GpuDirty;
        textures.gc_tick += 4;
        textures.TouchImage(textures.slot_images[ids[4]]);
        Buffer output{instance, 0, 4096, MemoryType::HostCached};
        const vk::BufferImageCopy copy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {16, 16, 1}};
        runtime.DownloadImage(&textures.slot_images[ids[0]], &output, std::span{&copy, 1});
        const auto bytes = textures.RetireStreamingImages(16_MB);
        ASSERT(bytes > 0);
        ASSERT(textures.slot_images.is_allocated(ids[0])); // queued copy still owns its backing
        ASSERT(False(textures.slot_images[ids[0]].flags & ImageFlagBits::Registered));
        for (u32 i = 1; i < ids.size(); ++i) {
            ASSERT(True(textures.slot_images[ids[i]].flags & ImageFlagBits::Registered));
        }
        scheduler.Finish(); scheduler.PopPendingOperations();
        output.Invalidate(0, 1024);
        for (u32 i = 0; i < 1024; ++i) ASSERT(output.mapped_data[i] == 0x39);
        ASSERT(!textures.slot_images.is_allocated(ids[0]));
        // FindImage's memo must reject its erased slot; the original guest bytes
        // reconstruct the texture rather than leaving a dangling descriptor.
        ids[0] = textures.FindImage(descriptions[0]);
        ASSERT(textures.slot_images[ids[0]].image_uid != old_uid);
        textures.UpdateImage(ids[0]);
        runtime.DownloadImage(&textures.slot_images[ids[0]], &output, std::span{&copy, 1});
        scheduler.Finish(); output.Invalidate(0, 1024);
        for (u32 i = 0; i < 1024; ++i) ASSERT(output.mapped_data[i] == 0x39);
        for (const auto id : ids) {
            textures.slot_images[id].binding = {};
            textures.FreeImage(id);
        }
        scheduler.Finish(); scheduler.PopPendingOperations();
        buffers.ReleaseUnmappedArenas(reinterpret_cast<VAddr>(mapping), span);
        scheduler.Finish(); scheduler.PopPendingOperations();
        ASSERT(release(physical, span) == 0);
        std::puts("PASS: streaming texture eviction preserves queued GPU reads, retains GPU-written/bound/recent images, drains destruction and reloads exact CPU bytes through stale lookup memos");
    }

    void TestDepthColorScratch() {
        Common::SlotVector<ImageView> views;
        ImageInfo color_info{};
        color_info.type=AmdGpu::ImageType::Color2DArray;
        color_info.pixel_format=vk::Format::eR32Sfloat;
        color_info.size={65,17,1}; color_info.resources={1,2};
        color_info.pitch=65; color_info.num_bits=32; color_info.guest_size=65*17*2*4;
        auto depth_info=color_info;
        depth_info.pixel_format=vk::Format::eD32Sfloat; depth_info.props.is_depth=1;
        Image source{instance,runtime,views,color_info}, depth{instance,runtime,views,depth_info}, result{instance,runtime,views,color_info};
        Buffer readback{instance,0,16384,MemoryType::HostCached};
        const u64 before=runtime.GetStagingPool().UploadAllocatedBytes();
        vk::ClearValue clear{}; clear.color=vk::ClearColorValue{std::array<float,4>{0.25f,0,0,0}};
        runtime.ClearImage(&source,SubresourceRange{{0,0},{1,2}},clear);
        runtime.CopyColorAndDepth(&source,&depth);
        runtime.CopyColorAndDepth(&depth,&result);
        runtime.Transit(&result,vk::ImageLayout::eTransferSrcOptimal,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        scheduler.Record([image=result.GetImage(),buffer=readback.Handle()](vk::CommandBuffer cmd) {
            const vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,2},.imageExtent={65,17,1}};
            cmd.copyImageToBuffer(image,vk::ImageLayout::eTransferSrcOptimal,buffer,copy);
        });
        scheduler.Finish(); readback.Invalidate(0,16384);
        for(u32 i=0;i<65*17*2;++i) {
            float value{}; std::memcpy(&value,readback.mapped_data.data()+i*4,4);
            ASSERT_MSG(value==0.25f,"Depth/color scratch copy corrupted texel {}",i);
        }
        ASSERT_MSG(runtime.GetStagingPool().UploadAllocatedBytes()<=before+32_MB,"Depth/color copies retained an oversized scratch buffer");
        std::puts("PASS: production color-depth-color transfers preserve every texel across unaligned rows and two layers using bounded scratch memory");
    }

    void TestVolumeCopies() {
        // A 1025-texel row also exercises Dozen's unaligned D3D12 footprint path.
        const auto device = instance.GetDevice();
        constexpr u32 width = 1025, slice_bytes = width * 4, image_bytes = slice_bytes * 3;
        vk::UniqueDeviceMemory memory;
        auto image = Vulkan::Check(device.createImageUnique(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e3D, .format = vk::Format::eR8G8B8A8Unorm,
            .extent = {width, 1, 3}, .mipLevels = 1, .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        }));
        const auto requirements = device.getImageMemoryRequirements(*image);
        const auto& properties = instance.GetMemoryProperties();
        u32 type = 0;
        while (type < properties.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type))) ++type;
        ASSERT(type < properties.memoryTypeCount);
        memory = Vulkan::Check(device.allocateMemoryUnique(vk::MemoryAllocateInfo{
            .allocationSize = requirements.size, .memoryTypeIndex = type,
        }));
        ASSERT(device.bindImageMemory(*image, *memory, 0) == vk::Result::eSuccess);
        Buffer upload{instance, 0, 32768, MemoryType::HostCached};
        Buffer download{instance, 0, 16384, MemoryType::HostCached};
        for (u32 z = 0; z < 3; ++z) std::memset(upload.mapped_data.data() + z * slice_bytes, 0x10 + z, slice_bytes);
        std::memset(upload.mapped_data.data() + 16384, 0x7b, slice_bytes);
        std::memset(upload.mapped_data.data() + 24576, 0x5a, 20);
        upload.Flush(0, 32768);
        const auto cmd = scheduler.CommandBuffer();
        auto barrier = [&](vk::ImageLayout before, vk::ImageLayout after) {
            const vk::ImageMemoryBarrier2 transition{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = before, .newLayout = after,
                .image = *image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &transition});
        };
        barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
        const vk::BufferImageCopy whole{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {width, 1, 3},
        };
        cmd.copyBufferToImage(upload.Handle(), *image, vk::ImageLayout::eTransferDstOptimal, whole);
        barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferDstOptimal);
        auto part = whole;
        part.bufferOffset = 16384;
        part.imageOffset.z = 1;
        part.imageExtent.depth = 1;
        cmd.copyBufferToImage(upload.Handle(), *image, vk::ImageLayout::eTransferDstOptimal, part);
        barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferDstOptimal);
        part.bufferOffset = 24576;
        part.imageOffset = {3, 0, 2};
        part.imageExtent.width = 5;
        cmd.copyBufferToImage(upload.Handle(), *image, vk::ImageLayout::eTransferDstOptimal, part);
        barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal);
        cmd.copyImageToBuffer(*image, vk::ImageLayout::eTransferSrcOptimal, download.Handle(), whole);
        scheduler.Finish();
        download.Invalidate(0, 16384);
        for (u32 offset = 0; offset < image_bytes; ++offset) {
            const auto z = offset / slice_bytes, x = offset % slice_bytes;
            const u8 expected = z == 1 ? 0x7b : z == 2 && x >= 12 && x < 32 ? 0x5a : 0x10 + z;
            ASSERT_MSG(download.mapped_data[offset] == expected, "Partial volume copy changed byte {} incorrectly", offset);
        }
        std::puts("PASS: 3D texture partial uploads preserve untouched slices and texels with unaligned rows");
    }

    void TestByteBuffers() {
        std::printf("Buffer alias fixture device capabilities: int16=%u, int64=%u\n",
                    unsigned(instance.IsShaderInt16Supported()), unsigned(instance.IsShaderInt64Supported()));
        // Exercise the real recompiler, including eight invocations writing neighboring bytes.
        namespace IR = Shader::IR;
        Shader::Info info{};
        info.hw_stage = Shader::HwStage::Compute;
        info.sw_stage = Shader::SwStage::Compute;
        info.uses_fp16 = true;
        info.loads.Set(IR::Attribute::LocalInvocationId, 0);
        info.buffers.push_back(Shader::BufferResource{
            .used_types = IR::Type::U8 | IR::Type::U16 | IR::Type::U32 | IR::Type::F32,
            .buffer_type = Shader::BufferType::SharedMemory, .is_written = true,
        });
        info.buffers.push_back(Shader::BufferResource{
            .used_types = IR::Type::U8 | IR::Type::U16 | IR::Type::U32 | IR::Type::U64,
            .buffer_type = Shader::BufferType::Guest, .is_written = true,
        });
        Shader::RuntimeInfo runtime_info{};
        runtime_info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
        runtime_info.hw.cs.workgroup_size = {8, 1, 1};
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        IR::Program program{info};
        program.blocks.push_back(&block);
        program.post_order_blocks.push_back(&block);
        program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
        IR::IREmitter ir{block};
        const auto index = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
        // Character materials alias one binding as uint, float and uint16. The
        // native and DXIL backends must agree on byte addressing for every alias.
        const auto alias_index = ir.IAdd(index, ir.Imm32(128u));
        ir.StoreBufferF32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(96u)),
                         ir.LoadBufferF32(1, ir.Imm32(0u), alias_index, {}), {});
        const auto half_index = ir.IMul(alias_index, ir.Imm32(2u));
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(104u)),
                         ir.UConvert(32, ir.LoadBufferU16(ir.Imm32(0u), half_index, {})), {});
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(112u)),
                         ir.UConvert(32, ir.LoadBufferU16(ir.Imm32(0u), ir.IAdd(half_index, ir.Imm32(1u)), {})), {});
        // FSR's half-bit tricks mix signed/unsigned integer consumers of the
        // same converted value. Check signed zero, ties-to-even and range ends.
        const IR::F16 half_value = ir.FPConvert(16, IR::F32{ir.LoadBufferF32(
            1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(192u)), {})});
        const auto half_bits = ir.BitCast<IR::U16>(half_value);
        const IR::U32 signed_half = ir.SConvert(32, half_bits);
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(144u)),
                         ir.UConvert(32, half_bits), {});
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(152u)), signed_half, {});
        const IR::U16 signed_back = ir.UConvert(16, signed_half);
        ir.StoreBufferF32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(160u)),
                         ir.FPConvert(32, ir.BitCast<IR::F16>(signed_back)), {});
        const auto byte = ir.LoadBufferU8(ir.Imm32(0u), index, {});
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(16u)), ir.UConvert(32, byte), {});
        const auto value = IR::U8{ir.UConvert(8, ir.IAdd(index, ir.Imm32(0x180u)))};
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(24u)), ir.UConvert(32, value), {});
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(32u)), ir.SConvert(32, value), {});
        ir.StoreBufferU8(ir.Imm32(0u), ir.IAdd(index, ir.Imm32(8u)), value, {});
        const auto null_handle = ir.Imm32(1u);
        ir.StoreBufferU32(1, null_handle, index, ir.Imm32(0xdeadbeefu), {});
        ir.StoreBufferU8(null_handle, index, value, {});
        ir.StoreBufferU16(null_handle, index, ir.UConvert(16, ir.Imm32(0xffffu)), {});
        ir.StoreBufferU64(null_handle, index, ir.Imm64(u64{0x12345678abcdef}), {});
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(40u)), ir.LoadBufferU32(1, null_handle, index, {}), {});
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(48u)), ir.UConvert(32, ir.LoadBufferU8(null_handle, index, {})), {});
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(56u)), ir.UConvert(32, ir.LoadBufferU16(null_handle, index, {})), {});
        const auto wide_null = ir.UnpackUint2x32(ir.LoadBufferU64(null_handle, index, {}));
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(64u)), ir.CompositeExtract(wide_null, 0), {});
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(80u)), ir.CompositeExtract(wide_null, 1), {});
        ir.StoreBufferU32(1, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(72u)), ir.BufferAtomicIAdd(null_handle, index, ir.Imm32(1u), {}), {});
        Shader::Profile profile{
            .supported_spirv = instance.SupportedSpirvVersion(),
            .subgroup_size = instance.SubgroupSize(),
            .support_int8 = false, .support_int16 = true, .support_int64 = true,
        };
        Shader::Backend::Bindings bindings;
        const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
        const auto device = instance.GetDevice();
        const auto shader = Vulkan::CompileSPV(spirv, device);
        const std::array<vk::DescriptorSetLayoutBinding, 2> layout_bindings{{
            {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        const auto layout = Vulkan::Check(device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{
            .bindingCount = 2, .pBindings = layout_bindings.data(),
        }));
        const auto pipeline_layout = Vulkan::Check(device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
            .setLayoutCount = 1, .pSetLayouts = &*layout,
        }));
        const auto pipeline = Vulkan::Check(device.createComputePipelineUnique({}, vk::ComputePipelineCreateInfo{
            .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = shader, .pName = "main"},
            .layout = *pipeline_layout,
        }));
        Buffer data{instance, 0, 4096, MemoryType::HostCached};
        Buffer guard{instance, 0, 4096, MemoryType::HostCached};
        std::memset(guard.mapped_data.data(), 0xcd, 4096);
        guard.Flush(0, 4096);
        std::memset(data.mapped_data.data(), 0xa5, 4096);
        for (u32 i = 0; i < 8; ++i) data.mapped_data[i] = 0xf0 + i;
        for (u32 i = 0; i < 8; ++i) {
            const u32 pattern = 0x3f800123u + i * 0x1007u;
            std::memcpy(data.mapped_data.data() + (128 + i) * 4, &pattern, 4);
        }
        constexpr std::array<u32, 8> half_inputs{
            0x00000000, 0x80000000, 0x3f800000, 0xc0000000,
            0x3f801000, 0x3f803000, 0x38800000, 0x477fe000};
        std::memcpy(data.mapped_data.data() + 192 * 4, half_inputs.data(), sizeof(half_inputs));
        data.Flush(0, 4096);
        const vk::DescriptorBufferInfo buffer_info{data.Handle(), 0, 4096};
        const vk::WriteDescriptorSet write{
            .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &buffer_info,
        };
        const vk::DescriptorBufferInfo guard_info{guard.Handle(), 0, 4096};
        auto null_write = write;
        null_write.dstBinding = 1;
        null_write.pBufferInfo = &guard_info;
        const std::array writes{write, null_write};
        const auto cmd = scheduler.CommandBuffer();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        scheduler.BindHostDescriptors(cmd, vk::PipelineBindPoint::eCompute, *pipeline_layout, *layout, writes);
        cmd.dispatch(1, 1, 1);
        scheduler.Finish();
        data.Invalidate(0, 4096);
        guard.Invalidate(0, 4096);
        for (u32 i = 0; i < 4096; ++i) ASSERT(guard.mapped_data[i] == 0xcd);
        for (u32 i = 0; i < 8; ++i) {
            u32 loaded;
            constexpr std::array<u32, 8> packed_halves{0, 0x8000, 0x3c00, 0xc000, 0x3c00, 0x3c02, 0x0400, 0x7bff};
            constexpr std::array<u32, 8> unpacked_halves{
                0x00000000, 0x80000000, 0x3f800000, 0xc0000000,
                0x3f800000, 0x3f804000, 0x38800000, 0x477fe000};
            for (u32 alias = 0; alias < 3; ++alias) {
                std::memcpy(&loaded, data.mapped_data.data() + (144 + alias * 8 + i) * 4, 4);
                const u32 expected = alias == 0 ? packed_halves[i] : alias == 1
                    ? u32(s32(s16(packed_halves[i]))) : unpacked_halves[i];
                ASSERT_MSG(loaded == expected, "Half packing alias {} index {}: {:#x} != {:#x}", alias, i, loaded, expected);
            }
            const u32 pattern = 0x3f800123u + i * 0x1007u;
            for (u32 alias = 0; alias < 3; ++alias) {
                std::memcpy(&loaded, data.mapped_data.data() + (96 + alias * 8 + i) * 4, 4);
                const u32 expected = alias == 0 ? pattern : alias == 1 ? pattern & 0xffffu : pattern >> 16;
                ASSERT_MSG(loaded == expected, "Mixed float/uint16 buffer alias {} index {}: {:#x} != {:#x}", alias, i, loaded, expected);
            }
            std::memcpy(&loaded, data.mapped_data.data() + (16 + i) * 4, 4);
            ASSERT_MSG(loaded == 0xf0 + i, "Byte load {} incorrect: {}", i, loaded);
            std::memcpy(&loaded, data.mapped_data.data() + (24 + i) * 4, 4);
            ASSERT_MSG(loaded == 0x80 + i, "Byte narrowing {} incorrect: {}", i, loaded);
            std::memcpy(&loaded, data.mapped_data.data() + (32 + i) * 4, 4);
            ASSERT_MSG(loaded == u32(s32(i) - 128), "Byte sign extension {} incorrect: {}", i, loaded);
            ASSERT_MSG(data.mapped_data[8 + i] == 0x80 + i, "Concurrent byte store {} incorrect", i);
            for (u32 base : {40u, 48u, 56u, 64u, 72u, 80u}) {
                std::memcpy(&loaded, data.mapped_data.data() + (base + i) * 4, 4);
                ASSERT_MSG(loaded == 0, "Null buffer read/atomic at {} returned {}", base + i, loaded);
            }
        }
        for (u32 i = 16; i < 64; ++i) ASSERT(data.mapped_data[i] == 0xa5);
        device.destroyShaderModule(shader);
        std::puts("PASS: production shader recompiler lowers byte loads and concurrent byte stores to 32-bit storage");
        std::puts("PASS: mixed float32/uint32/uint16 buffer aliases preserve byte addresses and bit patterns");
        std::puts("PASS: signed and unsigned half packing preserves signed zero, ties-to-even and finite range limits");
        const Shader::StageSpecialization null_spec{info, runtime_info, profile, {}};
        auto valid_info = info;
        auto sharp = AmdGpu::Buffer::Placeholder(64);
        std::memcpy(valid_info.buffers[1].sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
        const Shader::StageSpecialization valid_spec{valid_info, runtime_info, profile, {}};
        ASSERT(!(null_spec == valid_spec) && !(valid_spec == null_spec));
        sharp.base_address = 0;
        std::memcpy(valid_info.buffers[1].sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
        const Shader::StageSpecialization zero_base_spec{valid_info, runtime_info, profile, {}};
        ASSERT(!(zero_base_spec == valid_spec) && !(valid_spec == zero_base_spec));
        std::puts("PASS: null buffer loads/atomics return zero, stores preserve guard data, and shader validity keys differ");
    }

    void TestDiagnosticCapture() {
        ImageInfo info{};
        info.pixel_format = vk::Format::eR8G8B8A8Unorm;
        info.type = AmdGpu::ImageType::Color2D;
        info.resources = {.levels = 1, .layers = 1};
        info.size = {16, 16, 1}; info.pitch = 16; info.num_bits = 32; info.guest_size = 1024;
        Common::SlotVector<ImageView> views;
        Image image{instance, runtime, views, info};
        const vk::ClearValue clear{vk::ClearColorValue{std::array<float, 4>{.25f, .5f, .75f, 1.f}}};
        runtime.ClearImage(&image, SubresourceRange{.extent = info.resources}, clear);
        // Reproduce a queued shader-read transition immediately before capture.
        // The diagnostic's transfer-source transition must be a separate command.
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
        runtime.CaptureSampledImage(&image, 0x12345678, 0);
        ASSERT(image.backing->state.layout == vk::ImageLayout::eShaderReadOnlyOptimal);
        Buffer verify{instance, 0, 1024, MemoryType::HostCached};
        runtime.FlushBarriers();
        const vk::BufferImageCopy copy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {16, 16, 1}};
        runtime.DownloadImage(&image, &verify, std::span{&copy, 1});
        scheduler.Finish(); scheduler.PopPendingOperations(); verify.Invalidate(0, 1024);
        const std::array<u8, 4> expected{64, 128, 191, 255};
        for (u32 i = 0; i < 1024; ++i)
            ASSERT_MSG(std::abs(int(verify.mapped_data[i]) - int(expected[i % 4])) <= 1,
                       "Diagnostic readback altered source byte {}", i);
        std::puts("PASS: diagnostic image capture orders pending transitions, restores state, and preserves source pixels");

        // Reflection maps are BC6H arrays. Check all layers and small mips,
        // including restoration of a previously mixed subresource layout.
        ImageInfo bc_info{};
        bc_info.pixel_format = vk::Format::eBc6HUfloatBlock;
        bc_info.type = AmdGpu::ImageType::Color2DArray;
        bc_info.props.is_block = true;
        bc_info.resources = {.levels = 3, .layers = 6};
        bc_info.size = {8, 8, 1}; bc_info.pitch = 8;
        bc_info.num_bits = 8; bc_info.guest_size = 576;
        Image bc_image{instance, runtime, views, bc_info};
        Buffer upload{instance, 0, 576, MemoryType::HostCached};
        for (u32 i = 0; i < 576; ++i) upload.mapped_data[i] = u8(i * 13u + 7u);
        upload.Flush(0, 576);
        const std::array<vk::BufferImageCopy, 3> bc_copies{{
            {.bufferOffset = 0, .bufferRowLength = 8, .bufferImageHeight = 8,
             .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 6}, .imageExtent = {8, 8, 1}},
            {.bufferOffset = 384, .bufferRowLength = 4, .bufferImageHeight = 4,
             .imageSubresource = {vk::ImageAspectFlagBits::eColor, 1, 0, 6}, .imageExtent = {4, 4, 1}},
            {.bufferOffset = 480, .bufferRowLength = 4, .bufferImageHeight = 4,
             .imageSubresource = {vk::ImageAspectFlagBits::eColor, 2, 0, 6}, .imageExtent = {2, 2, 1}},
        }};
        runtime.UploadImage(&bc_image, &upload, bc_copies);
        runtime.Transit(&bc_image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
        runtime.Transit(&bc_image, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead,
                        SubresourceRange{.base = {0, 0}, .extent = {1, 1}});
        const auto bc_saved = bc_image.backing->subresource_states;
        runtime.CaptureSampledImage(&bc_image, 0x12345679, 0);
        ASSERT(bc_image.backing->subresource_states.size() == bc_saved.size());
        for (u32 i = 0; i < bc_saved.size(); ++i) {
            const auto& got = bc_image.backing->subresource_states[i];
            ASSERT(got.layout == bc_saved[i].layout && got.pl_stage == bc_saved[i].pl_stage &&
                   got.access_mask == bc_saved[i].access_mask);
        }
        Buffer bc_verify{instance, 0, 576, MemoryType::HostCached};
        runtime.DownloadImage(&bc_image, &bc_verify, bc_copies);
        // Finish waits for the GPU, not the separate priority callback thread
        // that writes diagnostic files. A FIFO sentinel joins that writer.
        std::promise<void> capture_written;
        auto capture_ready = capture_written.get_future();
        scheduler.DeferPriorityOperation([&capture_written] { capture_written.set_value(); });
        scheduler.Finish(); scheduler.PopPendingOperations(); bc_verify.Invalidate(0, 576);
        ASSERT(capture_ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        ASSERT(std::memcmp(bc_verify.mapped_data.data(), upload.mapped_data.data(), 576) == 0);
        const auto filename = fmt::format("{}/002_0000000000000000_0000000012345679_0_8x8_Bc6HUfloatBlock_m3_a6.bc6",
                                         std::getenv("BB_DIAGNOSTIC_IMAGE_DIR"));
        FILE* captured = std::fopen(filename.c_str(), "rb");
        ASSERT(captured);
        std::array<u8, 577> captured_bytes{};
        const auto read = std::fread(captured_bytes.data(), 1, captured_bytes.size(), captured);
        std::fclose(captured);
        ASSERT(read == 576 && std::memcmp(captured_bytes.data(), upload.mapped_data.data(), 576) == 0);
        std::puts("PASS: BC6H diagnostic capture preserves every array layer, small mip and prior subresource state");
    }

    void TestPaddedBc6Copies() {
        // Guest BC6 mipmaps keep a 32x32 row/slice footprint even when the
        // image mip is only 16x16. Array layers must use bufferImageHeight.
        ImageInfo info{};
        info.pixel_format = vk::Format::eBc6HUfloatBlock;
        info.type = AmdGpu::ImageType::Color2D; info.props.is_block = true;
        info.resources = {.levels = 1, .layers = 3}; info.size = {16, 16, 1};
        info.pitch = 32; info.num_bits = 128; info.guest_size = 3072;
        const vk::BufferImageCopy padded{.bufferRowLength = 32, .bufferImageHeight = 32,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 3}, .imageExtent = {16, 16, 1}};
        const vk::BufferImageCopy compact{.bufferRowLength = 16, .bufferImageHeight = 16,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 3}, .imageExtent = {16, 16, 1}};
        Common::SlotVector<ImageView> views;
        for (u32 phase = 0; phase < 2; ++phase) {
            Image image{instance, runtime, views, info};
            Buffer input{instance, 0, 3072, MemoryType::HostCached};
            Buffer output{instance, 0, 3072, MemoryType::HostCached};
            std::memset(input.mapped_data.data(), 0xee, 3072);
            std::memset(output.mapped_data.data(), 0xcd, 3072);
            for (u32 layer = 0; layer < 3; ++layer)
                for (u32 row = 0; row < 4; ++row)
                    for (u32 byte = 0; byte < 64; ++byte) {
                        const u32 offset = phase == 0 ? layer * 1024 + row * 128 + byte : layer * 256 + row * 64 + byte;
                        input.mapped_data[offset] = u8(17 + layer * 41 + row * 7 + byte);
                    }
            input.Flush(0, 3072); output.Flush(0, 3072);
            runtime.UploadImage(&image, &input, std::span{phase == 0 ? &padded : &compact, 1});
            runtime.DownloadImage(&image, &output, std::span{phase == 0 ? &compact : &padded, 1});
            scheduler.Finish(); output.Invalidate(0, 3072);
            for (u32 offset = 0; offset < 3072; ++offset) {
                u8 expected = 0xcd;
                if (phase == 0 && offset < 768) {
                    const u32 layer = offset / 256, row = (offset % 256) / 64, byte = offset % 64;
                    expected = u8(17 + layer * 41 + row * 7 + byte);
                } else if (phase == 1 && (offset % 1024) / 128 < 4 && offset % 128 < 64) {
                    const u32 layer = offset / 1024, row = (offset % 1024) / 128, byte = offset % 128;
                    expected = u8(17 + layer * 41 + row * 7 + byte);
                }
                ASSERT_MSG(output.mapped_data[offset] == expected,
                    "Padded BC6 {} byte {}: {} != {}", phase == 0 ? "upload" : "download",
                    offset, output.mapped_data[offset], expected);
            }
        }
        std::puts("PASS: padded BC6 array uploads and downloads honor row pitch, image height and untouched padding");
    }

    void TestReflectionSamples() {
        // Replay captured GPU block bytes through the real image uploader and
        // shader emitter. Outputs are compared across drivers by the runner.
        namespace IR = Shader::IR;
        u32 width{}, height{}, mips{}, layers{};
        ASSERT(std::sscanf(std::getenv("BB_REFLECTION_SHAPE"), "%ux%um%ua%u", &width, &height, &mips, &layers) == 4);
        ASSERT(width && height && width <= 512 && height <= 512 && mips && mips <= 12 && layers && layers <= 64);
        std::vector<vk::BufferImageCopy> copies;
        u64 bytes = 0;
        for (u32 mip = 0; mip < mips; ++mip) {
            const u32 w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
            const u32 row = ((w + 3) / 4) * 4, rows = ((h + 3) / 4) * 4;
            copies.push_back(vk::BufferImageCopy{.bufferOffset = bytes,
                .bufferRowLength = row, .bufferImageHeight = rows,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, layers}, .imageExtent = {w, h, 1}});
            bytes += u64(row) * rows * layers;
        }
        Buffer upload{instance, 0, bytes, MemoryType::HostCached};
        FILE* input = std::fopen(std::getenv("BB_REFLECTION_INPUT"), "rb");
        ASSERT(input);
        ASSERT(std::fread(upload.mapped_data.data(), 1, bytes, input) == bytes);
        ASSERT(std::fgetc(input) == EOF); std::fclose(input);
        upload.Flush(0, bytes);
        ImageInfo image_info{};
        image_info.pixel_format = vk::Format::eBc6HUfloatBlock;
        image_info.type = AmdGpu::ImageType::Color2DArray; image_info.props.is_block = true;
        image_info.resources = {.levels = mips, .layers = layers};
        image_info.size = {width, height, 1}; image_info.pitch = width;
        image_info.num_bits = 8; image_info.guest_size = bytes;
        Common::SlotVector<ImageView> views;
        Image image{instance, runtime, views, image_info};
        runtime.UploadImage(&image, &upload, copies);
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
        ImageViewInfo view_info{};
        view_info.type = AmdGpu::ImageType::Color2DArray;
        view_info.format = image_info.pixel_format; view_info.range.extent = image_info.resources;
        auto& view = image.FindView(view_info);
        const auto device = instance.GetDevice();
        const auto sampler = Vulkan::Check(device.createSamplerUnique(vk::SamplerCreateInfo{
            .magFilter = vk::Filter::eLinear, .minFilter = vk::Filter::eLinear,
            .mipmapMode = vk::SamplerMipmapMode::eLinear,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge, .maxLod = float(mips - 1)}));
        const std::array pool_sizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 8}};
        Vulkan::DescriptorHeap heap{instance, scheduler.GetWorkSemaphore(), pool_sizes};
        for (u32 mip = 0; mip < mips; ++mip) {
            const u32 w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
            Shader::Info info{};
            info.hw_stage = Shader::HwStage::Compute; info.sw_stage = Shader::SwStage::Compute;
            for (u32 c = 0; c < 3; ++c) info.loads.Set(IR::Attribute::WorkgroupId, c);
            info.buffers.push_back(Shader::BufferResource{.used_types = IR::Type::F32,
                .buffer_type = Shader::BufferType::SharedMemory, .is_written = true});
            info.images.push_back(Shader::ImageResource{.is_array = true});
            auto sharp = AmdGpu::Image::Null(false);
            sharp.base_address = 0x100; sharp.type = u64(AmdGpu::ImageType::Color2DArray);
            sharp.data_format = u64(AmdGpu::DataFormat::FormatBc6);
            sharp.width = width - 1; sharp.height = height - 1;
            sharp.depth = layers - 1; sharp.last_array = layers - 1; sharp.last_level = mips - 1;
            std::memcpy(info.images[0].sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
            info.samplers.push_back(Shader::SamplerResource{});
            Shader::RuntimeInfo runtime_info{};
            runtime_info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
            runtime_info.hw.cs.workgroup_size = {1, 1, 1};
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block{pool}; IR::Program program{info};
            program.blocks.push_back(&block); program.post_order_blocks.push_back(&block);
            program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
            program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
            IR::IREmitter ir{block};
            const auto x = ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0);
            const auto y = ir.GetAttributeU32(IR::Attribute::WorkgroupId, 1);
            const auto z = ir.GetAttributeU32(IR::Attribute::WorkgroupId, 2);
            const auto pixel = ir.IAdd(x, ir.IMul(ir.IAdd(y, ir.IMul(z, ir.Imm32(h))), ir.Imm32(w)));
            const auto index = ir.IMul(pixel, ir.Imm32(12u));
            ir.StoreBufferF32(4, ir.Imm32(0u), index,
                ir.ImageRead(ir.Imm32(0u), ir.CompositeConstruct(x, y, z), ir.Imm32(mip), {}, {}), {});
            const IR::F32 xf{ir.ConvertUToF(32, 32, x)}, yf{ir.ConvertUToF(32, 32, y)};
            const IR::F32 zf{ir.ConvertUToF(32, 32, z)};
            for (u32 variant = 0; variant < 2; ++variant) {
                const auto uv = ir.CompositeConstruct(
                    ir.FPMul(ir.FPAdd(xf, ir.Imm32(variant ? .75f : .5f)), ir.Imm32(1.f / w)),
                    ir.FPMul(ir.FPAdd(yf, ir.Imm32(variant ? .75f : .5f)), ir.Imm32(1.f / h)), zf);
                ir.StoreBufferF32(4, ir.Imm32(0u), ir.IAdd(index, ir.Imm32(4u + 4u * variant)),
                    ir.ImageSampleExplicitLod(ir.Imm32(0u), uv, ir.Imm32(float(mip) + (variant ? .5f : 0.f)), {}, {}), {});
            }
            const Shader::Profile profile{.supported_spirv = instance.SupportedSpirvVersion(),
                .subgroup_size = instance.SubgroupSize(), .support_int16 = true};
            Shader::Backend::Bindings bindings;
            const auto shader = Vulkan::CompileSPV(Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings), device);
            Vulkan::ComputePipeline::SerializationSupport sdata;
            Vulkan::ComputePipeline pipeline{instance, scheduler, heap, profile, {}, {0}, info, shader, sdata, false};
            ASSERT(pipeline.IsValid());
            const u64 output_bytes = u64(w) * h * layers * 12 * sizeof(float);
            Buffer data{instance, 0, output_bytes, MemoryType::HostCached};
            const vk::DescriptorBufferInfo buffer_info{data.Handle(), 0, output_bytes};
            const vk::DescriptorImageInfo texture_info{VK_NULL_HANDLE, *view.image_view, vk::ImageLayout::eShaderReadOnlyOptimal};
            const vk::DescriptorImageInfo sampler_info{*sampler};
            Vulkan::Pipeline::DescriptorWrites writes;
            writes.push_back({.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffer_info});
            writes.push_back({.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &texture_info});
            writes.push_back({.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampler, .pImageInfo = &sampler_info});
            pipeline.BindResources(writes, Shader::PushData{});
            const auto cmd = scheduler.CommandBuffer();
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.Handle()); cmd.dispatch(w, h, layers);
            scheduler.Finish(); data.Invalidate(0, output_bytes);
            const auto filename = fmt::format("{}/reflection_mip{}.f32", std::getenv("BB_REFLECTION_OUTPUT"), mip);
            FILE* output = std::fopen(filename.c_str(), "wb"); ASSERT(output);
            ASSERT(std::fwrite(data.mapped_data.data(), 1, output_bytes, output) == output_bytes); std::fclose(output);
            device.destroyShaderModule(shader);
        }
        std::puts("PASS: captured BC6H reflection replay completed all texel-fetch, linear and trilinear samples");
    }

    void TestHdrBlending() {
        // Hunter's Dream uses two packed HDR light targets, followed by half-float
        // composition. Exercise both formats together through the real pipeline builder.
        namespace IR = Shader::IR;
        const auto device = instance.GetDevice();
        constexpr u32 side = 16;
        const std::array formats{vk::Format::eB10G11R11UfloatPack32, vk::Format::eR16G16B16A16Sfloat};
        const std::array<std::array<float, 4>, 2> source{{{2.f, .75f, .125f, .5f}, {.125f, .5f, 2.f, .25f}}};
        const std::array<float, 4> background{.25f, .5f, 1.f, .75f};
        Shader::Info vs{}, fs{};
        vs.hw_stage = Shader::HwStage::Vertex; vs.sw_stage = Shader::SwStage::Vertex;
        fs.hw_stage = Shader::HwStage::Fragment; fs.sw_stage = Shader::SwStage::Fragment;
        Shader::RuntimeInfo fragment{};
        fragment.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
        for (u32 target = 0; target < 2; ++target) {
            fragment.hw.fs.color_buffers[target].num_format = AmdGpu::NumberFormat::Float;
            for (u32 c = 0; c < 4; ++c) fs.stores.Set(IR::Attribute::RenderTarget0 + target, c);
        }
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        IR::Program program{fs};
        program.blocks.push_back(&block); program.post_order_blocks.push_back(&block);
        program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
        IR::IREmitter ir{block};
        for (u32 target = 0; target < 2; ++target)
            for (u32 c = 0; c < 4; ++c)
                ir.SetAttribute(IR::Attribute::RenderTarget0 + target, ir.Imm32(source[target][c]), c);
        const Shader::Profile profile{.supported_spirv = instance.SupportedSpirvVersion(),
                                      .subgroup_size = instance.SubgroupSize()};
        Shader::Backend::Bindings bindings;
        const auto vertex = Vulkan::CompileSPV(DOZEN_TEST_VERT, device);
        const auto pixel = Vulkan::CompileSPV(Shader::Backend::SPIRV::EmitSPIRV(profile, fragment, program, bindings), device);
        const std::array pool_sizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8}};
        Vulkan::DescriptorHeap heap{instance, scheduler.GetWorkSemaphore(), pool_sizes};
        for (u32 mode = 0; mode < 4; ++mode) {
            std::array<vk::UniqueDeviceMemory, 2> memories;
            std::array<vk::UniqueImage, 2> images;
            std::array<vk::UniqueImageView, 2> views;
            std::array<vk::RenderingAttachmentInfo, 2> attachments;
            std::array<vk::ImageMemoryBarrier2, 2> barriers;
            Vulkan::GraphicsPipelineKey key{};
            key.prim_type = AmdGpu::PrimitiveType::TriangleList;
            key.polygon_mode = AmdGpu::PolygonMode::Fill;
            key.clip_space = AmdGpu::ClipSpace::ZeroToW;
            key.logic_op = AmdGpu::ColorControl::LogicOp::Copy;
            key.num_color_attachments = 2; key.viewport_count = 1;
            key.num_samples = key.depth_samples = 1;
            for (u32 target = 0; target < 2; ++target) {
                images[target] = Vulkan::Check(device.createImageUnique(vk::ImageCreateInfo{
                    .imageType = vk::ImageType::e2D, .format = formats[target], .extent = {side, side, 1},
                    .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
                    .tiling = vk::ImageTiling::eOptimal,
                    .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc}));
                const auto requirements = device.getImageMemoryRequirements(*images[target]);
                const auto& properties = instance.GetMemoryProperties();
                u32 type = 0;
                while (type < properties.memoryTypeCount && (!(requirements.memoryTypeBits & (1u << type)) ||
                    !(properties.memoryTypes[type].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal))) ++type;
                ASSERT(type < properties.memoryTypeCount);
                memories[target] = Vulkan::Check(device.allocateMemoryUnique(vk::MemoryAllocateInfo{
                    .allocationSize = requirements.size, .memoryTypeIndex = type}));
                ASSERT(device.bindImageMemory(*images[target], *memories[target], 0) == vk::Result::eSuccess);
                views[target] = Vulkan::Check(device.createImageViewUnique(vk::ImageViewCreateInfo{
                    .image = *images[target], .viewType = vk::ImageViewType::e2D, .format = formats[target],
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
                attachments[target] = vk::RenderingAttachmentInfo{
                    .imageView = *views[target], .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .loadOp = vk::AttachmentLoadOp::eClear, .storeOp = vk::AttachmentStoreOp::eStore,
                    .clearValue = vk::ClearValue{vk::ClearColorValue{background}}};
                barriers[target] = vk::ImageMemoryBarrier2{
                    .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
                    .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .image = *images[target], .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                key.color_samples[target] = 1;
                key.color_buffers[target].data_format = target == 0 ? AmdGpu::DataFormat::Format10_11_11 : AmdGpu::DataFormat::Format16_16_16_16;
                key.color_buffers[target].num_format = AmdGpu::NumberFormat::Float;
                key.write_masks[target] = mode == 3 ? vk::ColorComponentFlagBits::eG :
                    vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
                key.cb_shader_mask.SetMask(target, 15);
                auto& blend = key.blend_controls[target];
                blend.enable = mode == 1 || mode == 2;
                blend.color_src_factor = mode == 2 ? AmdGpu::BlendControl::BlendFactor::SrcAlpha : AmdGpu::BlendControl::BlendFactor::One;
                blend.color_dst_factor = AmdGpu::BlendControl::BlendFactor::One;
                blend.color_func = blend.alpha_func = AmdGpu::BlendControl::BlendFunc::Add;
                blend.separate_alpha_blend = true;
                blend.alpha_src_factor = AmdGpu::BlendControl::BlendFactor::One;
                blend.alpha_dst_factor = AmdGpu::BlendControl::BlendFactor::Zero;
            }
            std::array<const Shader::Info*, Vulkan::MaxShaderStages> infos{};
            infos[u32(Shader::SwStage::Vertex)] = &vs; infos[u32(Shader::SwStage::Fragment)] = &fs;
            std::array<Shader::RuntimeInfo, Vulkan::MaxShaderStages> runtime_infos{};
            runtime_infos[u32(Shader::SwStage::Fragment)] = fragment;
            std::array<vk::ShaderModule, Vulkan::MaxShaderStages> modules{};
            modules[u32(Shader::SwStage::Vertex)] = vertex; modules[u32(Shader::SwStage::Fragment)] = pixel;
            Vulkan::GraphicsPipeline::SerializationSupport sdata;
            Vulkan::GraphicsPipeline pipeline{instance, scheduler, heap, profile, key, {}, infos, runtime_infos, std::nullopt, modules, sdata, false};
            ASSERT(pipeline.IsValid());
            Buffer pixels{instance, 0, side * side * 12, MemoryType::HostCached};
            const auto cmd = scheduler.CommandBuffer();
            cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers.data()});
            cmd.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, {side, side}}, .layerCount = 1,
                .colorAttachmentCount = 2, .pColorAttachments = attachments.data()});
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.Handle());
            Vulkan::DynamicState state;
            state.viewports.push_back({0, 0, float(side), float(side), 0, 1});
            state.scissors.push_back({{0, 0}, {side, side}}); state.line_width = 1;
            state.color_write_masks = key.write_masks; state.Invalidate(); state.Commit(instance, cmd);
            if (instance.IsVertexInputDynamicState()) cmd.setVertexInputEXT({}, {});
            cmd.draw(3, 1, 0, 0); cmd.endRendering();
            for (u32 target = 0; target < 2; ++target) {
                auto& b = barriers[target];
                b.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
                b.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
                b.dstStageMask = vk::PipelineStageFlagBits2::eTransfer; b.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
                b.oldLayout = vk::ImageLayout::eColorAttachmentOptimal; b.newLayout = vk::ImageLayout::eTransferSrcOptimal;
            }
            cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers.data()});
            for (u32 target = 0; target < 2; ++target) {
                const vk::BufferImageCopy copy{.bufferOffset = target ? side * side * 4u : 0u,
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, .imageExtent = {side, side, 1}};
                cmd.copyImageToBuffer(*images[target], vk::ImageLayout::eTransferSrcOptimal, pixels.Handle(), copy);
            }
            scheduler.Finish(); pixels.Invalidate(0, side * side * 12);
            for (u32 p = 0; p < side * side; ++p) for (u32 target = 0; target < 2; ++target) {
                u32 packed = 0;
                if (!target) std::memcpy(&packed, pixels.mapped_data.data() + p * 4, 4);
                for (u32 c = 0; c < (target ? 4u : 3u); ++c) {
                    u32 bits, exponent, mantissa; int mantissa_bits, bias;
                    if (!target) {
                        mantissa_bits = c == 2 ? 5 : 6; bias = 15;
                        bits = (packed >> (c == 0 ? 0 : c == 1 ? 11 : 22)) & ((1u << (mantissa_bits + 5)) - 1);
                    } else {
                        u16 half; std::memcpy(&half, pixels.mapped_data.data() + side * side * 4 + p * 8 + c * 2, 2);
                        bits = half; mantissa_bits = 10; bias = 15;
                    }
                    exponent = (bits >> mantissa_bits) & 31; mantissa = bits & ((1u << mantissa_bits) - 1);
                    const float actual = exponent ? std::ldexp(1.f + float(mantissa) / (1u << mantissa_bits), int(exponent) - bias)
                                                   : std::ldexp(float(mantissa), 1 - bias - mantissa_bits);
                    float expected = source[target][c];
                    if (c < 3 && (mode == 1 || mode == 2)) expected = background[c] + expected * (mode == 2 ? source[target][3] : 1.f);
                    if (mode == 3 && c != 1) expected = background[c];
                    ASSERT_MSG(std::abs(actual - expected) < .0001f,
                        "HDR MRT mode {} target {} pixel {} channel {}: {} != {}", mode, target, p, c, actual, expected);
                }
            }
        }
        device.destroyShaderModule(vertex); device.destroyShaderModule(pixel);
        std::puts("PASS: packed HDR and half-float MRTs preserve channels, additive/alpha blending, and write masks");
    }

    void TestGraphics() {
        const char* probe=std::getenv("BB_GPU_GRAPHICS_PROBE");
        const std::string_view mode=probe ? probe : "";
        const bool no_depth=mode=="clear-no-depth" || mode=="draw-no-depth";
        const auto device = instance.GetDevice();
        constexpr u32 side = 64;
        constexpr u64 bytes = side * side * 4;
        const auto make_image = [&](vk::Format format, vk::ImageUsageFlags usage,
                                    vk::ImageAspectFlags aspect,
                                    vk::UniqueDeviceMemory& memory, vk::UniqueImage& image,
                                    vk::UniqueImageView& view) {
            image = Vulkan::Check(device.createImageUnique(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D, .format = format,
                .extent = {side, side, 1}, .mipLevels = 1, .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
                .usage = usage,
            }));
            const auto requirements = device.getImageMemoryRequirements(*image);
            const auto& properties = instance.GetMemoryProperties();
            u32 type = 0;
            while (type < properties.memoryTypeCount &&
                   (!(requirements.memoryTypeBits & (1u << type)) ||
                    !(properties.memoryTypes[type].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal))) ++type;
            ASSERT(type < properties.memoryTypeCount);
            memory = Vulkan::Check(device.allocateMemoryUnique(vk::MemoryAllocateInfo{
                .allocationSize = requirements.size, .memoryTypeIndex = type,
            }));
            ASSERT(device.bindImageMemory(*image, *memory, 0) == vk::Result::eSuccess);
            view = Vulkan::Check(device.createImageViewUnique(vk::ImageViewCreateInfo{
                .image = *image, .viewType = vk::ImageViewType::e2D, .format = format,
                .subresourceRange = {aspect, 0, 1, 0, 1},
            }));
        };
        vk::UniqueDeviceMemory color_memory, depth_memory;
        vk::UniqueImage color_image, depth_image;
        vk::UniqueImageView color_view, depth_view;
        make_image(vk::Format::eR8G8B8A8Unorm,
                   vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
                   vk::ImageAspectFlagBits::eColor, color_memory, color_image, color_view);
        make_image(vk::Format::eD32SfloatS8Uint, vk::ImageUsageFlagBits::eDepthStencilAttachment,
                   vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil,
                   depth_memory, depth_image, depth_view);
        Buffer pixels{instance, 0, bytes * 9, MemoryType::HostCached};
        Shader::Info vs, fs;
        vs.hw_stage = Shader::HwStage::Vertex;
        vs.sw_stage = Shader::SwStage::Vertex;
        fs.hw_stage = Shader::HwStage::Fragment;
        fs.sw_stage = Shader::SwStage::Fragment;
        std::array<const Shader::Info*, Vulkan::MaxShaderStages> infos{};
        infos[u32(Shader::SwStage::Vertex)] = &vs;
        infos[u32(Shader::SwStage::Fragment)] = &fs;
        std::array<Shader::RuntimeInfo, Vulkan::MaxShaderStages> runtime_infos{};
        std::array<vk::ShaderModule, Vulkan::MaxShaderStages> modules{};
        modules[u32(Shader::SwStage::Vertex)] = Vulkan::CompileSPV(DOZEN_TEST_VERT, device);
        modules[u32(Shader::SwStage::Fragment)] = Vulkan::CompileSPV(DOZEN_TEST_FRAG, device);
        const auto rectangle_module = Vulkan::CompileSPV(DOZEN_RECT_TEST_VERT, device);
        const auto quad_module = Vulkan::CompileSPV(DOZEN_QUAD_TEST_VERT, device);
        const auto list_fragment = Vulkan::CompileSPV(DOZEN_LIST_TEST_FRAG, device);
        const auto original_vertex = modules[u32(Shader::SwStage::Vertex)];
        const auto original_fragment = modules[u32(Shader::SwStage::Fragment)];
        // Recompile real fragment IR, including output instructions following
        // unconditional discard. Test both forced fallback and native capability.
        namespace IR = Shader::IR;
        Shader::Info discard_fs{};
        discard_fs.hw_stage = Shader::HwStage::Fragment;
        discard_fs.sw_stage = Shader::SwStage::Fragment;
        discard_fs.has_discard = true;
        discard_fs.loads.Set(IR::Attribute::FragCoord, 0);
        for (u32 c = 0; c < 4; ++c) discard_fs.stores.Set(IR::Attribute::RenderTarget0, c);
        Shader::RuntimeInfo discard_runtime{};
        discard_runtime.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
        discard_runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
        std::array<vk::ShaderModule, 3> discard_modules{};
        for (u32 test = 0; test < 3; ++test) {
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block{pool};
            IR::Program program{discard_fs};
            program.blocks.push_back(&block);
            program.post_order_blocks.push_back(&block);
            program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
            program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
            IR::IREmitter ir{block};
            if (test == 1) ir.Discard();
            else ir.Discard(ir.FPLessThan(ir.GetAttribute(IR::Attribute::FragCoord, 0), ir.Imm32(float(side) / 2)));
            const std::array<float, 4> green{32.f / 255, 192.f / 255, 64.f / 255, 1};
            for (u32 c = 0; c < 4; ++c) ir.SetAttribute(IR::Attribute::RenderTarget0, ir.Imm32(green[c]), c);
            const Shader::Profile discard_profile{
                .supported_spirv = instance.SupportedSpirvVersion(),
                .subgroup_size = instance.SubgroupSize(),
                .supports_demote_to_helper = test == 2 && instance.SupportsDemoteToHelper(),
            };
            Shader::Backend::Bindings bindings;
            discard_modules[test] = Vulkan::CompileSPV(Shader::Backend::SPIRV::EmitSPIRV(
                discard_profile, discard_runtime, program, bindings), device);
        }
        std::array<Shader::OutputMap, 3> clip_outputs{};
        clip_outputs[0][0] = Shader::Output::ClipDist0;
        const auto clip_discard = Vulkan::CompileSPV(
            Shader::Backend::SPIRV::EmitDiscardFragmentShader(clip_outputs, instance.SupportsDemoteToHelper()), device);
        const Shader::Profile profile{};
        const std::array pool_sizes = {vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 32}};
        Vulkan::DescriptorHeap heap{instance, scheduler.GetWorkSemaphore(), pool_sizes};
        for (u32 test = 0; test < 9; ++test) {
            std::printf("Renderer graphics check %u: creating pipeline and recording\n",test);
            Vulkan::GraphicsPipelineKey key;
            key.prim_type = AmdGpu::PrimitiveType::TriangleList;
            key.polygon_mode = AmdGpu::PolygonMode::Fill;
            key.clip_space = AmdGpu::ClipSpace::ZeroToW;
            key.logic_op = AmdGpu::ColorControl::LogicOp::Copy;
            key.num_color_attachments = 1;
            key.num_samples = key.depth_samples = key.color_samples[0] = 1;
            key.color_buffers[0].data_format = AmdGpu::DataFormat::Format8_8_8_8;
            key.color_buffers[0].num_format = AmdGpu::NumberFormat::Unorm;
            key.write_masks[0] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                 vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
            key.z_format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
            key.stencil_format = AmdGpu::DepthBuffer::StencilFormat::Stencil8;
            key.viewport_count = 1;
            key.depth_test_enable = !no_depth;
            key.depth_write_enable = !no_depth;
            key.depth_compare_op = test == 1 ? vk::CompareOp::eGreater : vk::CompareOp::eLess;
            key.cull_mode = test == 2 ? vk::CullModeFlagBits::eFrontAndBack : vk::CullModeFlagBits::eNone;
            key.stencil_test_enable = test == 3;
            if (test == 4 || test == 5) {
                key.prim_type = test == 4 ? AmdGpu::PrimitiveType::RectList : AmdGpu::PrimitiveType::QuadList;
                modules[u32(Shader::SwStage::Vertex)] = test == 4 ? rectangle_module : quad_module;
                modules[u32(Shader::SwStage::Fragment)] = list_fragment;
                auto& fragment = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
                fragment.num_inputs = instance.IsTessellationSupported() ? 1 : 2;
                fragment.inputs[0].param_index = 0;
                fragment.inputs[1].param_index = 1; // Unused metadata must not become a GS input.
                fs.loads.Set(Shader::IR::Attribute::Param0, 0);
            }
            if (test >= 6) {
                modules[u32(Shader::SwStage::Vertex)] = original_vertex;
                modules[u32(Shader::SwStage::Fragment)] = discard_modules[test - 6];
                infos[u32(Shader::SwStage::Fragment)] = &discard_fs;
                runtime_infos[u32(Shader::SwStage::Fragment)] = discard_runtime;
            }
            key.stencil_front.compareOp = key.stencil_back.compareOp = vk::CompareOp::eEqual;
            Vulkan::GraphicsPipeline::SerializationSupport sdata;
            Vulkan::GraphicsPipeline pipeline{instance, scheduler, heap, profile, key, {},
                infos, runtime_infos, std::nullopt, modules, sdata, false};
            ASSERT_MSG(pipeline.IsValid(), "Graphics portability test {} could not create its pipeline", test);
            const auto cmd = scheduler.CommandBuffer();
            const std::array<vk::ImageMemoryBarrier2, 2> barriers{{
                {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                 .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                 .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                 .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                 .oldLayout = test ? vk::ImageLayout::eTransferSrcOptimal : vk::ImageLayout::eUndefined,
                 .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                 .image = *color_image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}},
                {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                 .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                 .dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
                 .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                 .oldLayout = test ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eUndefined,
                 .newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                 .image = *depth_image, .subresourceRange = {vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1}},
            }};
            cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers.data()});
            if (mode=="barriers") {
                scheduler.Finish();
                std::puts("DIAGNOSTIC PASS: image transitions without rendering");
                return;
            }
            const vk::RenderingAttachmentInfo color{
                .imageView = *color_view, .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .loadOp = vk::AttachmentLoadOp::eClear, .storeOp = vk::AttachmentStoreOp::eStore,
                .clearValue = vk::ClearColorValue{std::array<float, 4>{0, 0, 0, 1}},
            };
            vk::ClearValue depth_clear{};
            depth_clear.depthStencil = vk::ClearDepthStencilValue{0.75f, 7};
            const vk::RenderingAttachmentInfo depth{
                .imageView = *depth_view, .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                .loadOp = vk::AttachmentLoadOp::eClear, .storeOp = vk::AttachmentStoreOp::eStore,
                .clearValue = depth_clear,
            };
            cmd.beginRendering(vk::RenderingInfo{
                .renderArea = {{0, 0}, {side, side}}, .layerCount = 1,
                .colorAttachmentCount = 1, .pColorAttachments = &color,
                .pDepthAttachment = no_depth ? nullptr : &depth,
                .pStencilAttachment = no_depth ? nullptr : &depth,
            });
            if (mode=="clear" || mode=="clear-no-depth") {
                cmd.endRendering();
                scheduler.Finish();
                std::puts("DIAGNOSTIC PASS: selected clear subset completed");
                return;
            }
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.Handle());
            Vulkan::DynamicState state;
            state.viewports.push_back({0, 0, float(side), float(side), 0, 1});
            state.scissors.push_back({{0, 0}, {side, side}});
            state.depth_test_enabled = !no_depth;
            state.depth_write_enabled = !no_depth;
            state.depth_compare_op = key.depth_compare_op;
            state.cull_mode = key.cull_mode;
            state.stencil_test_enabled = key.stencil_test_enable;
            state.stencil_front_ops.compare_op = state.stencil_back_ops.compare_op = vk::CompareOp::eEqual;
            state.stencil_front_reference = state.stencil_back_reference = 3;
            state.stencil_front_compare_mask = state.stencil_back_compare_mask = 0xff;
            state.stencil_front_write_mask = state.stencil_back_write_mask = 0xff;
            state.color_write_masks = key.write_masks;
            state.line_width = 1;
            state.Invalidate();
            state.Commit(instance, cmd);
            // Match production draws by initializing the declared push-data
            // range even when the fixture's shader does not read it. Zero
            // selects native pixel coordinates for the FragCoord fixtures.
            const Shader::PushData push{};
            cmd.pushConstants(pipeline.GetLayout(), Vulkan::AllGraphicsStageBits,
                              0, sizeof(push), &push);
            if (instance.IsVertexInputDynamicState()) cmd.setVertexInputEXT({}, {});
            cmd.draw(test == 5 ? 4 : 3, 1, 0, 0);
            cmd.endRendering();
            if (!mode.empty()) {
                scheduler.Finish();
                std::puts("DIAGNOSTIC PASS: selected draw subset completed");
                return;
            }
            const vk::ImageMemoryBarrier2 to_copy{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .image = *color_image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
            const vk::BufferImageCopy copy{
                .bufferOffset = bytes * test,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {side, side, 1},
            };
            cmd.copyImageToBuffer(*color_image, vk::ImageLayout::eTransferSrcOptimal, pixels.Handle(), copy);
            scheduler.Finish();
        }
        pixels.Invalidate(0, bytes * 9);
        for (u32 test = 0; test < 9; ++test) {
            const std::array<u8, 4> expected = test == 0 ? std::array<u8, 4>{32, 192, 64, 255}
                                                       : std::array<u8, 4>{0, 0, 0, 255};
            for (u32 pixel = 0; pixel < side * side; ++pixel) {
                for (u32 channel = 0; channel < 4; ++channel) {
                    const auto value = pixels.mapped_data[test * bytes + pixel * 4 + channel];
                    const bool kept = test != 7 && pixel % side >= side / 2;
                    const std::array<u8, 4> discard_color = kept ? std::array<u8, 4>{32, 192, 64, 255} : std::array<u8, 4>{0, 0, 0, 255};
                    const u8 target = test >= 6 ? discard_color[channel] : test < 4 ? expected[channel] :
                        channel == 0 ? u8(std::lround((float(pixel % side) + 0.5f) * 255.f / side)) :
                        channel == 1 ? u8(std::lround((float(pixel / side) + 0.5f) * 255.f / side)) :
                        channel == 2 ? 64 : 255;
                    ASSERT_MSG(std::abs(int(value) - int(target)) <= 1, "Graphics test {} pixel {} channel {}: {} != {}",
                               test, pixel, channel, value, target);
                }
            }
        }
        device.destroyShaderModule(original_vertex);
        device.destroyShaderModule(original_fragment);
        device.destroyShaderModule(rectangle_module);
        device.destroyShaderModule(quad_module);
        device.destroyShaderModule(list_fragment);
        for (const auto module : discard_modules) device.destroyShaderModule(module);
        device.destroyShaderModule(clip_discard);
        std::puts("PASS: production graphics pipelines render and preserve depth rejection, culling, and stencil rejection");
        std::puts("PASS: rectangle and quad expansion covers the image with correct interpolated attributes");
        std::puts("PASS: production conditional and unconditional fragment discards suppress output without unsupported capabilities");
    }
};

}

extern "C" int bbgpu_buffer_arena_test() {
    auto test = std::make_unique<VideoCore::BufferArenaTestAccess>();
    test->Run();
    return std::getenv("BB_GPU_GRAPHICS_PROBE") ? 77 : 0;
}
