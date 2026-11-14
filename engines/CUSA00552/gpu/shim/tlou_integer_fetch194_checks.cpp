#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include "shader_recompiler/info.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
void RunIntegerFetchRegression194(const Vulkan::Instance& instance,Vulkan::Scheduler& scheduler) {
    namespace IR=Shader::IR;
    using namespace Vulkan;
    const auto device=instance.GetDevice();
    constexpr u32 w=3,h=5,channels=4;
    const std::array<std::array<float,2>,12> points{{
        {-.3f,-.7f},{0.f,0.f},{1.f,1.f},{1.3f,1.7f},
        {.5f,.5f},{.34f,.41f},{.99f,.01f},{.01f,.99f},
        {1.f/3.f,.2f},{.6666f,.7999f},{2.f/3.f,.8f},{.25f,.75f}}};
    std::array<u32,w*h*channels> values;
    for(u32 i=0;i<values.size();++i)values[i]=0x80000000u+0x13579u*i;
    values[0]=0xffffffffu;values[1]=0x7fc00001u;values[2]=0;
    auto image=Check(device.createImageUnique(vk::ImageCreateInfo{
        .imageType=vk::ImageType::e2D,.format=vk::Format::eR32G32B32A32Uint,.extent={w,h,1},
        .mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eSampled}));
    const auto req=device.getImageMemoryRequirements(*image);u32 memory_type=0;
    while(!(req.memoryTypeBits&(1u<<memory_type)))++memory_type;
    auto memory=Check(device.allocateMemoryUnique(vk::MemoryAllocateInfo{.allocationSize=req.size,.memoryTypeIndex=memory_type}));
    ASSERT(device.bindImageMemory(*image,*memory,0)==vk::Result::eSuccess);
    auto view=Check(device.createImageViewUnique(vk::ImageViewCreateInfo{
        .image=*image,.viewType=vk::ImageViewType::e2D,.format=vk::Format::eR32G32B32A32Uint,
        .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}));
    auto sampler=Check(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter=vk::Filter::eNearest,.minFilter=vk::Filter::eNearest,.mipmapMode=vk::SamplerMipmapMode::eNearest,
        .addressModeU=vk::SamplerAddressMode::eClampToEdge,.addressModeV=vk::SamplerAddressMode::eClampToEdge,
        .addressModeW=vk::SamplerAddressMode::eClampToEdge,.maxLod=16.f}));
    VideoCore::Buffer upload{instance,0,4096,VideoCore::MemoryType::HostCached};
    VideoCore::Buffer output{instance,0,4096,VideoCore::MemoryType::HostCached};
    std::memcpy(upload.mapped_data.data(),values.data(),sizeof(values));upload.Flush(0,sizeof(values));
    std::memset(output.mapped_data.data(),0xcd,4096);output.Flush(0,4096);
    Shader::Info info{};info.hw_stage=Shader::HwStage::Compute;info.sw_stage=Shader::SwStage::Compute;
    info.pgm_hash=0xff56b297u;
    info.buffers.push_back(Shader::BufferResource{.used_types=IR::Type::F32,.buffer_type=Shader::BufferType::SharedMemory,.is_written=true});
    info.images.push_back(Shader::ImageResource{});
    auto sharp=AmdGpu::Image::Null(false);sharp.base_address=0x100;
    sharp.type=u64(AmdGpu::ImageType::Color2D);sharp.data_format=u64(AmdGpu::DataFormat::Format32_32_32_32);
    sharp.num_format=u64(AmdGpu::NumberFormat::Uint);sharp.width=w-1;sharp.height=h-1;sharp.base_level=sharp.last_level=0;
    std::memcpy(info.images[0].sharp_fetch.immediates.data(),&sharp,sizeof(sharp));
    info.samplers.push_back(Shader::SamplerResource{});
    AmdGpu::Sampler ssharp{};ssharp.raw0=0x00fff00000000092ULL;ssharp.raw1=0x4000000004000000ULL;
    std::memcpy(info.samplers[0].sharp_fetch.immediates.data(),&ssharp,sizeof(ssharp));
    Shader::RuntimeInfo runtime_info{};runtime_info.Initialize(Shader::HwStage::Compute,Shader::SwStage::Compute);
    runtime_info.hw.cs.workgroup_size={1,1,1};
    Common::ObjectPool<IR::Inst> pool;IR::Block block{pool};IR::Program program{info};
    program.blocks.push_back(&block);program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
    IR::IREmitter ir{block};
    for(u32 i=0;i<points.size();++i) {
        const auto uv=ir.CompositeConstruct(ir.Imm32(points[i][0]),ir.Imm32(points[i][1]));
        ir.StoreBufferF32(4,ir.Imm32(0u),ir.Imm32(i*4),ir.ImageSampleImplicitLod(ir.Imm32(0u),uv,{}, {},{}),{});
    }
    const Shader::Profile profile{.supported_spirv=instance.SupportedSpirvVersion(),.subgroup_size=instance.SubgroupSize()};
    Shader::Backend::Bindings bindings;
    const auto spirv=Shader::Backend::SPIRV::EmitSPIRV(profile,runtime_info,program,bindings);
    if(const char* file=std::getenv("TLOU_TEST_SPV")) {
        FILE* f=std::fopen(file,"wb");ASSERT(f);std::fwrite(spirv.data(),sizeof(u32),spirv.size(),f);std::fclose(f);
    }
    const auto module=CompileSPV(spirv,device);
    const std::array sizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer,8},
        vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage,8},vk::DescriptorPoolSize{vk::DescriptorType::eSampler,8}};
    DescriptorHeap heap{instance,scheduler.GetWorkSemaphore(),sizes};
    ComputePipeline::SerializationSupport sdata;
    ComputePipeline pipeline{instance,scheduler,heap,profile,{}, {0},info,module,sdata,false};ASSERT(pipeline.IsValid());
    const auto cmd=scheduler.CommandBuffer();
    vk::ImageMemoryBarrier2 barrier{.dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite,
        .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eTransferDstOptimal,
        .image=*image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
    cmd.copyBufferToImage(upload.Handle(),*image,vk::ImageLayout::eTransferDstOptimal,vk::BufferImageCopy{
        .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={w,h,1}});
    barrier.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;barrier.srcAccessMask=vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader;barrier.dstAccessMask=vk::AccessFlagBits2::eShaderRead;
    barrier.oldLayout=vk::ImageLayout::eTransferDstOptimal;barrier.newLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
    const vk::DescriptorBufferInfo bi{output.Handle(),0,4096};
    const vk::DescriptorImageInfo ii{VK_NULL_HANDLE,*view,vk::ImageLayout::eShaderReadOnlyOptimal},si{*sampler};
    Pipeline::DescriptorWrites writes;
    writes.push_back(vk::WriteDescriptorSet{.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&bi});
    writes.push_back(vk::WriteDescriptorSet{.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&ii});
    writes.push_back(vk::WriteDescriptorSet{.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampler,.pImageInfo=&si});
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline.Handle());pipeline.BindResources(writes,Shader::PushData{});
    cmd.dispatch(1,1,1);scheduler.Finish();output.Invalidate(0,4096);
    for(u32 i=0;i<points.size();++i)for(u32 c=0;c<4;++c) {
        const int x=std::clamp(int(std::floor(points[i][0]*w)),0,int(w)-1);
        const int y=std::clamp(int(std::floor(points[i][1]*h)),0,int(h)-1);
        u32 actual;std::memcpy(&actual,output.mapped_data.data()+i*16+c*4,4);
        ASSERT_MSG(actual==values[(y*w+x)*4+c],"Integer point-clamp sample {} channel {}: {} != {}",i,c,actual,values[(y*w+x)*4+c]);
    }
    for(u32 i=points.size()*16;i<4096;++i)ASSERT(output.mapped_data[i]==0xcd);
    scheduler.Finish();device.destroyShaderModule(module);
    std::puts("PASS: integer point-clamp fetch matches12 edge/interior/outside coordinates, preserves all192 output bytes including high-bit/NaN patterns, and leaves output guards intact");
}
