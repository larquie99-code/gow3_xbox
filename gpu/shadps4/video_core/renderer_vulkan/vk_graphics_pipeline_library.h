// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"
namespace Vulkan {
vk::ResultValue<vk::UniquePipeline> CreateGraphicsPipelineWithLibraries(
    vk::Device device, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info,
    std::vector<vk::UniquePipeline>& retained_libraries);
} // namespace Vulkan
