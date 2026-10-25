// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include "video_core/renderer_vulkan/vk_graphics_pipeline_library.h"
#include "video_core/renderer_vulkan/vk_async_compiler.h"

namespace Vulkan {
vk::ResultValue<vk::UniquePipeline> CreateGraphicsPipelineWithLibraries(
    vk::Device device, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info,
    std::vector<vk::UniquePipeline>& retained_libraries) {
    retained_libraries.reserve(retained_libraries.size() + 4);
    using Part = vk::GraphicsPipelineLibraryFlagBitsEXT;
    std::vector<vk::PipelineShaderStageCreateInfo> pre_stages, fragment_stages;
    for (uint32_t i = 0; i < info.stageCount; ++i) {
        (info.pStages[i].stage == vk::ShaderStageFlagBits::eFragment ? fragment_stages : pre_stages)
            .push_back(info.pStages[i]);
    }
    // Fragment-only libraries require depth/stencil state even when the final
    // pipeline has no depth attachment. Supply the disabled default when omitted.
    const vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    const auto create = [&](Part part) {
        const vk::GraphicsPipelineLibraryCreateInfoEXT library{.pNext = info.pNext, .flags = part};
        auto ci = info;
        ci.pNext = &library;
        ci.flags |= vk::PipelineCreateFlagBits::eLibraryKHR;
        if (part == Part::eFragmentShader && !ci.pDepthStencilState) {
            ci.pDepthStencilState = &depth_stencil;
        }
        const auto& stages = part == Part::eFragmentShader ? fragment_stages : pre_stages;
        const bool shaders = part == Part::ePreRasterizationShaders || part == Part::eFragmentShader;
        ci.stageCount = shaders ? uint32_t(stages.size()) : 0;
        ci.pStages = shaders ? stages.data() : nullptr;
        return device.createGraphicsPipelineUnique(cache, ci);
    };
    // One additional compiler for the process, with a bounded queue. Other parts use the
    // existing pipeline workers. Never enqueue to the pool the calling worker belongs to.
    static AsyncCompiler compiler{1, 4};
    AsyncJob<vk::ResultValue<vk::UniquePipeline>> pre_job;
    const bool parallel = pre_job.Start(compiler, [&] { return create(Part::ePreRasterizationShaders); });
    auto fragment = create(Part::eFragmentShader);
    auto vertex = create(Part::eVertexInputInterface);
    auto output = create(Part::eFragmentOutputInterface);
    auto pre = [&] {
        if (!parallel) return create(Part::ePreRasterizationShaders);
        pre_job.Wait();
        auto result = pre_job.Poll();
        if (result) return std::move(*result);
        return vk::ResultValue<vk::UniquePipeline>{vk::Result::eErrorUnknown, vk::UniquePipeline{}};
    }();
    if (pre.result == vk::Result::eSuccess && fragment.result == vk::Result::eSuccess &&
        vertex.result == vk::Result::eSuccess && output.result == vk::Result::eSuccess) {
        const std::array libraries{*vertex.value, *pre.value, *fragment.value, *output.value};
        const vk::PipelineLibraryCreateInfoKHR link{.pNext = info.pNext,
            .libraryCount = uint32_t(libraries.size()), .pLibraries = libraries.data()};
        const vk::GraphicsPipelineCreateInfo ci{.pNext = &link, .flags = info.flags, .layout = info.layout};
        auto result = device.createGraphicsPipelineUnique(cache, ci);
        if (result.result == vk::Result::eSuccess) {
            // Libraries must outlive every linked executable pipeline.
            retained_libraries.push_back(std::move(vertex.value));
            retained_libraries.push_back(std::move(pre.value));
            retained_libraries.push_back(std::move(fragment.value));
            retained_libraries.push_back(std::move(output.value));
            return result;
        }
    }
    // Optional optimization: failures must not prevent rendering on a particular driver.
    std::puts("GPU: graphics pipeline library compilation failed; using monolithic pipeline");
    return device.createGraphicsPipelineUnique(cache, info);
}
} // namespace Vulkan
