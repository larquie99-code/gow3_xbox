// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
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
    CreateDescriptorPool();
}

DescriptorHeap::~DescriptorHeap() {
    const char* stats = std::getenv("SPARTAN_GPU_MEMORY_STATS");
    if (stats && stats[0] == '1') {
        std::printf("GPU descriptor cache: %llu hits, %llu writes\n",
            static_cast<unsigned long long>(cache_hits.load()),
            static_cast<unsigned long long>(cache_misses.load()));
    }
    device.destroyDescriptorPool(curr_pool);
    for (const auto [pool, tick] : pending_pools) {
        work_semaphore->Wait(tick);
        device.destroyDescriptorPool(pool);
    }
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout set_layout) {
    std::scoped_lock lock{mutex};
    return CommitLocked(set_layout);
}

vk::DescriptorSet DescriptorHeap::CommitCached(vk::DescriptorSetLayout layout,
                                              std::span<const vk::WriteDescriptorSet> writes, u64 recording_tick) {
    std::scoped_lock lock{mutex};
    if (!recording_tick) recording_tick = work_semaphore->CurrentTick();
    if (cache_tick != recording_tick) {
        cached_sets.clear(); // Avoid stale handles across resource lifetimes/submissions.
        cache_tick = recording_tick;
    }
    key_scratch.clear();
    key_scratch.push_back(std::bit_cast<u64>(layout));
    bool cacheable = true;
    for (const auto& write : writes) {
        key_scratch.insert(key_scratch.end(), {write.dstBinding, write.dstArrayElement,
            u64(write.descriptorType), write.descriptorCount});
        if (write.pNext) { cacheable = false; break; }
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            switch (write.descriptorType) {
            case vk::DescriptorType::eStorageBuffer:
            case vk::DescriptorType::eStorageBufferDynamic:
            case vk::DescriptorType::eUniformBuffer:
            case vk::DescriptorType::eUniformBufferDynamic: {
                const auto& info = write.pBufferInfo[i];
                key_scratch.insert(key_scratch.end(), {std::bit_cast<u64>(info.buffer), info.offset, info.range});
                break;
            }
            case vk::DescriptorType::eSampler:
            case vk::DescriptorType::eCombinedImageSampler:
            case vk::DescriptorType::eSampledImage:
            case vk::DescriptorType::eStorageImage: {
                const auto& info = write.pImageInfo[i];
                // Ignore fields not used by this descriptor type (they may be uninitialized).
                key_scratch.push_back(write.descriptorType == vk::DescriptorType::eSampledImage ||
                    write.descriptorType == vk::DescriptorType::eStorageImage ? 0 : std::bit_cast<u64>(info.sampler));
                key_scratch.push_back(write.descriptorType == vk::DescriptorType::eSampler ? 0 : std::bit_cast<u64>(info.imageView));
                key_scratch.push_back(write.descriptorType == vk::DescriptorType::eSampler ? 0 : u64(info.imageLayout));
                break;
            }
            default: cacheable = false; break;
            }
        }
    }
    if (cacheable) {
        if (const auto it = cached_sets.find(key_scratch); it != cached_sets.end()) {
            ++cache_hits;
            return it->second;
        }
    }
    ++cache_misses;
    const auto set = CommitLocked(layout);
    std::vector<vk::WriteDescriptorSet> patched(writes.begin(), writes.end());
    for (auto& write : patched) write.dstSet = set;
    device.updateDescriptorSets(patched, {});
    if (cacheable) {
        if (cached_sets.size() >= 256) cached_sets.clear(); // Bound retained CPU memory.
        cached_sets.emplace(key_scratch, set);
    }
    return set;
}

vk::DescriptorSet DescriptorHeap::CommitLocked(vk::DescriptorSetLayout set_layout) {
    const u64 set_key = std::bit_cast<u64>(set_layout);
    const auto [it, _] = descriptor_sets.try_emplace(set_key);

    // Check if allocated sets exist and pick one.
    if (!it->second.empty()) {
        const auto desc_set = it->second.back();
        it.value().pop_back();
        return desc_set;
    }

    DescSetBatch desc_sets(DescriptorSetBatch);
    std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
    layouts.fill(set_layout);

    vk::DescriptorSetAllocateInfo alloc_info = {
        .descriptorPool = curr_pool,
        .descriptorSetCount = DescriptorSetBatch,
        .pSetLayouts = layouts.data(),
    };

    // Attempt to allocate the descriptor set batch.
    auto result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
    if (result == vk::Result::eSuccess) {
        const auto desc_set = desc_sets.back();
        desc_sets.pop_back();
        it.value() = std::move(desc_sets);
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
    result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
    ASSERT_MSG(result == vk::Result::eSuccess,
               "Unexpected error during descriptor set allocation {}", vk::to_string(result));

    // We've changed pool so also reset descriptor batch cache.
    descriptor_sets.clear();
    cached_sets.clear();
    const auto desc_set = desc_sets.back();
    desc_sets.pop_back();
    descriptor_sets[set_key] = std::move(desc_sets);
    return desc_set;
}

void DescriptorHeap::CreateDescriptorPool() {
    const vk::DescriptorPoolCreateInfo pool_info = {
        .flags =
#ifdef GOW3_UWP
            vk::DescriptorPoolCreateFlagBits{},
#else
            vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
#endif
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
