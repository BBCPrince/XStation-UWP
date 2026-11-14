// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <bit>
#include <cstdlib>
#include <optional>

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace Vulkan {

ResourcePool::ResourcePool(Semaphore* work_semaphore_, std::size_t grow_step_)
    : work_semaphore{work_semaphore_}, grow_step{grow_step_} {}

std::size_t ResourcePool::CommitResource() {
    u64 gpu_tick = work_semaphore->KnownGpuTick();
    const auto search = [this, gpu_tick](std::size_t begin,
                                         std::size_t end) -> std::optional<std::size_t> {
        for (std::size_t iterator = begin; iterator < end; ++iterator) {
            if (gpu_tick >= ticks[iterator]) {
                ticks[iterator] = work_semaphore->CurrentTick();
                return iterator;
            }
        }
        return std::nullopt;
    };

    // Try to find a free resource from the hinted position to the end.
    auto found = search(hint_iterator, ticks.size());
    if (!found) {
        // Refresh semaphore to query updated results
        work_semaphore->Refresh();
        gpu_tick = work_semaphore->KnownGpuTick();
        found = search(hint_iterator, ticks.size());
    }
    if (!found) {
        // Search from beginning to the hinted position.
        found = search(0, hint_iterator);
        if (!found) {
            // Both searches failed, the pool is full; handle it.
            const std::size_t free_resource = ManageOverflow();

            ticks[free_resource] = work_semaphore->CurrentTick();
            found = free_resource;
        }
    }

    // Free iterator is hinted to the resource after the one that's been commited.
    hint_iterator = (*found + 1) % ticks.size();
    return *found;
}

std::size_t ResourcePool::ManageOverflow() {
    const std::size_t old_capacity = ticks.size();
    ticks.resize(old_capacity + grow_step);
    Allocate(old_capacity, old_capacity + grow_step);
    return old_capacity;
}

constexpr std::size_t COMMAND_BUFFER_POOL_SIZE = 4;

CommandPool::CommandPool(const Instance& instance, Semaphore* work_semaphore)
    : ResourcePool{work_semaphore, COMMAND_BUFFER_POOL_SIZE}, instance{instance} {
    const vk::CommandPoolCreateInfo pool_create_info = {
        .flags = vk::CommandPoolCreateFlagBits::eTransient |
                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    const vk::Device device = instance.GetDevice();
    auto [pool_result, pool] = device.createCommandPoolUnique(pool_create_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create command pool: {}",
               vk::to_string(pool_result));
    cmd_pool = std::move(pool);
    SetObjectName(device, *cmd_pool, "CommandPool");
}

CommandPool::~CommandPool() = default;

void CommandPool::Allocate(std::size_t begin, std::size_t end) {
    cmd_buffers.resize(end);

    const vk::CommandBufferAllocateInfo buffer_alloc_info = {
        .commandPool = *cmd_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = COMMAND_BUFFER_POOL_SIZE,
    };

    const vk::Device device = instance.GetDevice();
    const auto result =
        device.allocateCommandBuffers(&buffer_alloc_info, cmd_buffers.data() + begin);
    ASSERT(result == vk::Result::eSuccess);

    for (std::size_t i = begin; i < end; ++i) {
        SetObjectName(device, cmd_buffers[i], "CommandPool: Command Buffer {}", i);
    }
}

vk::CommandBuffer CommandPool::Commit() {
    const std::size_t index = CommitResource();
    return cmd_buffers[index];
}

DescriptorHeap::DescriptorHeap(const Instance& instance, Semaphore* work_semaphore_,
                               std::span<const vk::DescriptorPoolSize> pool_sizes_,
                               u32 descriptor_heap_count_)
    : device{instance.GetDevice()}, work_semaphore{work_semaphore_},
      descriptor_heap_count{descriptor_heap_count_}, pool_sizes{pool_sizes_} {
    const char* ring_env = std::getenv("BB_DESC_RING");
    ring_mode = !(ring_env && ring_env[0] == '0');
    CreateDescriptorPool();
}

DescriptorHeap::~DescriptorHeap() {
    device.destroyDescriptorPool(curr_pool);
    for (const auto pool : ring_pools) {
        device.destroyDescriptorPool(pool);
    }
    for (const auto [pool, tick] : pending_pools) {
        work_semaphore->Wait(tick);
        device.destroyDescriptorPool(pool);
    }
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout set_layout, bool batch_by_layout) {
    std::scoped_lock lock{commit_mutex};
    if (ring_mode) {
        return CommitRing(set_layout);
    }
    const u64 set_key = std::bit_cast<u64>(set_layout);
    auto it = descriptor_sets.end();
    if (batch_by_layout) {
        it = descriptor_sets.try_emplace(set_key).first;
    }

    // Check if allocated sets exist and pick one.
    if (batch_by_layout && !it->second.empty()) {
        const auto desc_set = it->second.back();
        it.value().pop_back();
        return desc_set;
    }

    const u32 batch_size = batch_by_layout ? DescriptorSetBatch : 1;
    DescSetBatch desc_sets(batch_size);
    std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
    layouts.fill(set_layout);

    vk::DescriptorSetAllocateInfo alloc_info = {
        .descriptorPool = curr_pool,
        .descriptorSetCount = batch_size,
        .pSetLayouts = layouts.data(),
    };

    const auto allocate_sets = [&] {
        desc_sets.resize(batch_size);
        alloc_info.descriptorSetCount = batch_size;
        auto result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
        if (batch_size > 1 && (result == vk::Result::eErrorOutOfPoolMemory ||
                              result == vk::Result::eErrorFragmentedPool)) {
            // A pool may fit a valid set but not the whole cache batch. Use
            // its remaining capacity, including pools smaller than a batch.
            desc_sets.resize(1);
            alloc_info.descriptorSetCount = 1;
            result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
        }
        return result;
    };
    auto result = allocate_sets();
    if (result == vk::Result::eSuccess) {
        const auto desc_set = desc_sets.back();
        desc_sets.pop_back();
        if (batch_by_layout) it.value() = std::move(desc_sets);
        return desc_set;
    }

    // The pool has run out. Record current tick and place it in pending list.
    ASSERT_MSG(result == vk::Result::eErrorOutOfPoolMemory ||
                   result == vk::Result::eErrorFragmentedPool,
               "Unexpected error during descriptor set allocation: {}", vk::to_string(result));
    pending_pools.emplace_back(curr_pool, work_semaphore->CurrentTick());
    if (const auto [pool, tick] = pending_pools.front(); work_semaphore->IsFree(tick)) {
        curr_pool = pool;
        pending_pools.pop_front();

        const auto reset_result = device.resetDescriptorPool(curr_pool);
        ASSERT_MSG(reset_result == vk::Result::eSuccess,
                   "Unexpected error resetting descriptor pool: {}", vk::to_string(reset_result));
    } else {
        CreateDescriptorPool();
    }

    // Attempt to allocate again with fresh pool.
    alloc_info.descriptorPool = curr_pool;
    result = allocate_sets();
    ASSERT_MSG(result == vk::Result::eSuccess,
               "Unexpected error during descriptor set allocation {}", vk::to_string(result));

    // We've changed pool so also reset descriptor batch cache.
    descriptor_sets.clear();
    const auto desc_set = desc_sets.back();
    desc_sets.pop_back();
    if (batch_by_layout) descriptor_sets[set_key] = std::move(desc_sets);
    return desc_set;
}

vk::DescriptorSet DescriptorHeap::CommitRing(vk::DescriptorSetLayout set_layout) {
    auto& ring = rings[std::bit_cast<u64>(set_layout)];
    // A set bound since tick `t` is in a command buffer submitted as `t` or later: CurrentTick()
    // is never below the submission that will carry this binding (as for pending_pools).
    const u64 tick = work_semaphore->CurrentTick();
    if (!ring.empty() && work_semaphore->IsFree(ring.front().tick)) {
        RingSet reused = ring.front();
        ring.pop_front();
        reused.tick = tick;
        ring.push_back(reused);
        return reused.set;
    }
    vk::DescriptorSetAllocateInfo alloc_info = {
        .descriptorPool = curr_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    vk::DescriptorSet set{};
    auto result = device.allocateDescriptorSets(&alloc_info, &set);
    if (result == vk::Result::eErrorOutOfPoolMemory ||
        result == vk::Result::eErrorFragmentedPool) {
        // Full pools stay: their sets are reused, never freed or reset.
        ring_pools.push_back(curr_pool);
        CreateDescriptorPool();
        alloc_info.descriptorPool = curr_pool;
        result = device.allocateDescriptorSets(&alloc_info, &set);
    }
    ASSERT_MSG(result == vk::Result::eSuccess, "Descriptor set allocation failed: {}",
               vk::to_string(result));
    ring.push_back({set, tick});
    return set;
}

void DescriptorHeap::CreateDescriptorPool() {
    const vk::DescriptorPoolCreateInfo pool_info = {
        .flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = descriptor_heap_count,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };
    auto [pool_result, pool] = device.createDescriptorPool(pool_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create descriptor pool: {}",
               vk::to_string(pool_result));
    curr_pool = pool;
}

} // namespace Vulkan
