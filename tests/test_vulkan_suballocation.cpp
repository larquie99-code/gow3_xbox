// SPDX-License-Identifier: GPL-2.0-or-later
// Live Vulkan regression: exercise the renderer's BDA buffers past 4096 resources.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "video_core/buffer_cache/buffer.h"
#include <vk_mem_alloc.h>

#undef NDEBUG
#include <cassert>

// This test has no guest process or runtime report to produce.
extern "C" void runtime_report() { std::abort(); }

int main() {
    VULKAN_HPP_DEFAULT_DISPATCHER.init();
    const vk::ApplicationInfo app{.pApplicationName = "suballocation-test",
                                   .apiVersion = VK_API_VERSION_1_2};
    const vk::InstanceCreateInfo ici{.pApplicationInfo = &app};
    const auto instance_result = vk::createInstance(ici);
    assert(instance_result.result == vk::Result::eSuccess);
    const auto instance = instance_result.value;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
    const auto physicals = instance.enumeratePhysicalDevices();
    assert(physicals.result == vk::Result::eSuccess && !physicals.value.empty());
    const auto physical = physicals.value.front();
    const auto features = physical.getFeatures2<vk::PhysicalDeviceFeatures2,
                                                 vk::PhysicalDeviceBufferDeviceAddressFeatures>();
    assert(features.get<vk::PhysicalDeviceBufferDeviceAddressFeatures>().bufferDeviceAddress);
    const auto families = physical.getQueueFamilyProperties();
    uint32_t family = 0;
    while (family < families.size() && !(families[family].queueFlags & vk::QueueFlagBits::eGraphics)) {
        ++family;
    }
    assert(family < families.size());
    const float priority = 1.0f;
    const vk::DeviceQueueCreateInfo qci{.queueFamilyIndex = family, .queueCount = 1,
                                       .pQueuePriorities = &priority};
    const vk::PhysicalDeviceBufferDeviceAddressFeatures bda{.bufferDeviceAddress = true};
    const vk::DeviceCreateInfo dci{.pNext = &bda, .queueCreateInfoCount = 1,
                                   .pQueueCreateInfos = &qci};
    const auto device_result = physical.createDevice(dci);
    assert(device_result.result == vk::Result::eSuccess);
    const auto device = device_result.value;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

    const VmaVulkanFunctions functions{
        .vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
    };
    const VmaAllocatorCreateInfo aci{
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = physical,
        .device = device,
        .preferredLargeHeapBlockSize = 128ull * 1024 * 1024,
        .pVulkanFunctions = &functions,
        .instance = instance,
        .vulkanApiVersion = VK_API_VERSION_1_2,
    };
    VmaAllocator allocator{};
    assert(vmaCreateAllocator(&aci, &allocator) == VK_SUCCESS);
    constexpr uint32_t count = 5000;
    std::vector<VideoCore::UniqueBuffer> buffers;
    std::vector<VmaAllocationInfo> infos(count);
    buffers.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        buffers.emplace_back(device, allocator);
        vk::BufferCreateInfo ci{.size = 4096, .usage = VideoCore::AllFlags};
        buffers.back().Create(ci, VideoCore::MemoryType::HostCached, &infos[i]);
        assert(buffers.back().bda_addr && infos[i].pMappedData);
    }
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    assert(stats.total.statistics.allocationCount == count);
    assert(stats.total.statistics.blockCount < 16);
    assert(infos[0].deviceMemory == infos[1].deviceMemory && infos[0].offset != infos[1].offset);
    std::printf("%u BDA buffers in %u Vulkan memory blocks\n", count,
                stats.total.statistics.blockCount);

    // Fragment the pool and refill holes with smaller allocations.
    for (uint32_t i = 0; i < count; i += 2) {
        buffers[i].Destroy();
        vk::BufferCreateInfo ci{.size = 2048, .usage = VideoCore::AllFlags};
        buffers[i].Create(ci, VideoCore::MemoryType::HostCached, &infos[i]);
    }
    const auto pool_result = device.createCommandPool({.queueFamilyIndex = family});
    assert(pool_result.result == vk::Result::eSuccess);
    const auto commands = device.allocateCommandBuffers({.commandPool = pool_result.value,
        .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1});
    assert(commands.result == vk::Result::eSuccess);
    const auto cmd = commands.value.front();
    assert(cmd.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
    for (uint32_t i = 0; i < count; ++i) {
        cmd.fillBuffer(buffers[i].buffer, 0, VK_WHOLE_SIZE, i + 1);
    }
    const vk::MemoryBarrier barrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                    .dstAccessMask = vk::AccessFlagBits::eHostRead};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                        {}, barrier, {}, {});
    assert(cmd.end() == vk::Result::eSuccess);
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
    assert(device.getQueue(family, 0).submit(submit) == vk::Result::eSuccess);
    assert(device.waitIdle() == vk::Result::eSuccess);
    for (uint32_t i = 0; i < count; ++i) {
        assert(vmaInvalidateAllocation(allocator, buffers[i].allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
        uint32_t first{}, last{};
        const auto* bytes = static_cast<const char*>(infos[i].pMappedData);
        const size_t size = i % 2 ? 4096 : 2048;
        std::memcpy(&first, bytes, sizeof(first));
        std::memcpy(&last, bytes + size - sizeof(last), sizeof(last));
        assert(first == i + 1 && last == i + 1);
    }
    device.destroyCommandPool(pool_result.value);
    buffers.clear();
    vmaCalculateStatistics(allocator, &stats);
    assert(stats.total.statistics.allocationCount == 0);
    vmaDestroyAllocator(allocator);
    device.destroy();
    instance.destroy();
}
