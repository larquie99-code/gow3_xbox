// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
// Discrete BAR memory can be host visible too, but CPU writes to it can be slow.
inline bool HasUnifiedUploadMemory(const VkPhysicalDeviceProperties& properties,
                                   const VkPhysicalDeviceMemoryProperties& memory) {
#ifndef GOW3_UWP
    if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
        properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) return false;
#endif
    constexpr auto flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((memory.memoryTypes[i].propertyFlags & flags) == flags) return true;
    }
    return false;
}

inline bool CanUploadToMappedBuffer(bool mapped, uint64_t last_use, uint64_t completed,
                                    uint64_t recording) {
    // A completed GPU submission does not cover commands still being recorded.
    return mapped && last_use < recording && last_use <= completed;
}
} // namespace Vulkan
