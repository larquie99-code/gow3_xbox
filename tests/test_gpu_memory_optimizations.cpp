// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <vector>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_memory_policy.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline_library.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/blit_helper.h"
#include "shader_recompiler/info.h"
#include <vk_mem_alloc.h>
#undef NDEBUG
#include <cassert>
extern "C" void runtime_report() { std::abort(); }

class ProbePipeline : public Vulkan::Pipeline {
public:
    ProbePipeline(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                  Vulkan::DescriptorHeap& heap, const Shader::Profile& profile)
        : Pipeline(instance, scheduler, heap, profile, VK_NULL_HANDLE) {
        std::array bindings{vk::DescriptorSetLayoutBinding{.binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment}};
        ConfigureDescriptorBindings(bindings, instance.MaxPushDescriptors()); // Exercise set fallback.
        assert(!uses_push_descriptors && dynamic_buffer_bindings.size() == 1);
        auto layout = instance.GetDevice().createDescriptorSetLayoutUnique({
            .bindingCount = 1, .pBindings = bindings.data()});
        assert(layout.result == vk::Result::eSuccess);
        desc_layout = std::move(layout.value);
        const vk::DescriptorSetLayout set = *desc_layout;
        const vk::PushConstantRange push{.stageFlags = Vulkan::AllGraphicsStageBits,
            .size = sizeof(Shader::PushData)};
        auto result = instance.GetDevice().createPipelineLayoutUnique({.setLayoutCount = 1,
            .pSetLayouts = &set, .pushConstantRangeCount = 1, .pPushConstantRanges = &push});
        assert(result.result == vk::Result::eSuccess);
        pipeline_layout = std::move(result.value);
    }
    vk::DescriptorSetLayout SetLayout() const { return *desc_layout; }
};

vk::UniqueShaderModule LoadShader(vk::Device device, const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    assert(file && file.tellg() > 0);
    std::vector<uint32_t> words(size_t(file.tellg()) / 4);
    file.seekg(0);
    assert(file.read(reinterpret_cast<char*>(words.data()), words.size() * 4));
    auto module = device.createShaderModuleUnique({.codeSize = words.size() * 4, .pCode = words.data()});
    assert(module.result == vk::Result::eSuccess);
    return std::move(module.value);
}

int main(int argc, char** argv) {
    assert(argc == 3);
    VkPhysicalDeviceProperties properties{.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU};
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryTypeCount = 1;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    assert(Vulkan::HasUnifiedUploadMemory(properties, memory));
    properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    assert(!Vulkan::HasUnifiedUploadMemory(properties, memory)); // BAR is not automatic UMA.
    assert(Vulkan::CanUploadToMappedBuffer(true, 3, 3, 4));
    assert(!Vulkan::CanUploadToMappedBuffer(true, 4, 3, 4)); // Still recording.
    assert(!Vulkan::CanUploadToMappedBuffer(true, 3, 2, 4)); // Still executing.
    assert(!Vulkan::CanUploadToMappedBuffer(false, 0, 0, 1));

    Vulkan::Instance instance{0, true};
    Vulkan::Scheduler scheduler{instance, std::getenv("SPARTAN_TEST_THREADED_RECORDING") != nullptr};
    Vulkan::Runtime runtime{instance, scheduler};
    const auto device = instance.GetDevice();
    VideoCore::Buffer input{instance, 0, 64_KB, VideoCore::MemoryType::DeviceLocal};
    VideoCore::Buffer readback{instance, 0, 64_KB, VideoCore::MemoryType::HostCached};
    assert(!input.mapped_data.empty()); // This regression runs on lavapipe's unified heap.
    assert(runtime.CanUploadDirect(&input));
    std::memset(input.mapped_data.data(), 0x37, input.SizeBytes());
    input.Flush(0, input.SizeBytes());
    runtime.AccessBuffer(&input, 0, input.SizeBytes(), vk::PipelineStageFlagBits2::eHost,
                         vk::AccessFlagBits2::eHostWrite);
    assert(!runtime.CanUploadDirect(&input));
    const vk::BufferCopy copy{.size = input.SizeBytes()};
    runtime.CopyBuffer(&input, &readback, std::span{&copy, 1});
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
    });
    scheduler.Finish();
    assert(runtime.CanUploadDirect(&input));
    readback.Invalidate(0, readback.SizeBytes());
    assert(readback.mapped_data.front() == 0x37 && readback.mapped_data.back() == 0x37);
    const u64 offset = std::max<u64>(256, instance.StorageMinAlignment());
    const float green[4] = {0, 1, 0, 1}, red[4] = {1, 0, 0, 1};
    std::memcpy(input.mapped_data.data(), green, sizeof(green));
    std::memcpy(input.mapped_data.data() + offset, red, sizeof(red));
    input.Flush(0, offset + sizeof(red));

    const std::array sizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageBufferDynamic, 64}};
    Vulkan::DescriptorHeap heap{instance, scheduler.GetWorkSemaphore(), sizes, 64};
    Shader::Profile profile{};
    ProbePipeline probe{instance, scheduler, heap, profile};
    auto vertex = LoadShader(device, argv[1]), fragment = LoadShader(device, argv[2]);
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex, .module = *vertex, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment, .module = *fragment, .pName = "main"}};
    const vk::PipelineVertexInputStateCreateInfo vi{};
    const vk::PipelineInputAssemblyStateCreateInfo ia{.topology = vk::PrimitiveTopology::eTriangleList};
    const vk::Viewport viewport{0, 0, 1, 1, 0, 1};
    const vk::Rect2D scissor{{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo vp{.viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor};
    const vk::PipelineRasterizationStateCreateInfo rs{.polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone, .lineWidth = 1};
    const vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::PipelineColorBlendAttachmentState attachment{.colorWriteMask = vk::ColorComponentFlags{0xF}};
    const vk::PipelineColorBlendStateCreateInfo cb{.attachmentCount = 1, .pAttachments = &attachment};
    const vk::Format format = vk::Format::eR8G8B8A8Unorm;
    const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    const vk::GraphicsPipelineCreateInfo ci{.pNext = &rendering, .stageCount = 2, .pStages = stages.data(),
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = probe.GetLayout()};
    auto cache = device.createPipelineCacheUnique(vk::PipelineCacheCreateInfo{});
    assert(cache.result == vk::Result::eSuccess);
    std::vector<vk::UniquePipeline> libraries; // Outlives the linked pipeline.
    auto pipeline = instance.IsGraphicsPipelineLibrarySupported()
        ? Vulkan::CreateGraphicsPipelineWithLibraries(device, *cache.value, ci, libraries)
        : device.createGraphicsPipelineUnique(*cache.value, ci);
    assert(pipeline.result == vk::Result::eSuccess);
    // Verify that this driver's cache can be restored, including library compilation data.
    auto cache_data = device.getPipelineCacheData(*cache.value);
    assert(cache_data.result == vk::Result::eSuccess);
    // Exercise an actual disk roundtrip of the driver's opaque cache bytes.
    char temp_path[] = "/tmp/gow3-pipeline-cache-XXXXXX";
    assert(mkdtemp(temp_path));
    const auto cache_file = std::filesystem::path{temp_path} / "driver.vkcache";
    {
        std::ofstream file(cache_file, std::ios::binary);
        assert(file.write(reinterpret_cast<const char*>(cache_data.value.data()), cache_data.value.size()));
    }
    std::vector<u8> loaded(cache_data.value.size());
    {
        std::ifstream file(cache_file, std::ios::binary);
        assert(file.read(reinterpret_cast<char*>(loaded.data()), loaded.size()));
    }
    assert(loaded == cache_data.value);
    std::filesystem::remove_all(temp_path);
    auto restored = device.createPipelineCacheUnique({.initialDataSize = loaded.size(),
        .pInitialData = loaded.data()});
    assert(restored.result == vk::Result::eSuccess);

    VideoCore::UniqueImage image{device, instance.GetAllocator()};
    image.Create({.imageType = vk::ImageType::e2D, .format = format, .extent = {1,1,1},
        .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc});
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    auto view = device.createImageViewUnique({.image = image.image, .viewType = vk::ImageViewType::e2D,
        .format = format, .subresourceRange = range});
    assert(view.result == vk::Result::eSuccess);
    scheduler.Record([&](vk::CommandBuffer cmd) {
        const vk::ImageMemoryBarrier2 barrier{.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.image, .subresourceRange = range};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        const vk::RenderingAttachmentInfo color{.imageView = *view.value,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare, .storeOp = vk::AttachmentStoreOp::eStore};
        cmd.beginRendering({.renderArea = scissor, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &color});
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline.value);
    });
    vk::DescriptorBufferInfo info{input.Handle(), 0, 16};
    Vulkan::Pipeline::DescriptorWrites writes{{.dstBinding = 0, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &info}};
    const auto hits = Vulkan::DescriptorHeap::cache_hits.load();
    probe.BindResources(writes, Shader::PushData{});
    scheduler.Record([](vk::CommandBuffer cmd) { cmd.draw(3,1,0,0); });
    info.offset = offset;
    probe.BindResources(writes, Shader::PushData{});
    assert(writes.front().pBufferInfo == &info); // Binding must not leave dangling pointers.
    scheduler.Record([&](vk::CommandBuffer cmd) {
        cmd.draw(3,1,0,0);
        cmd.endRendering();
        const vk::ImageMemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy, .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal, .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.image, .subresourceRange = range};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        const vk::BufferImageCopy region{.imageSubresource = {vk::ImageAspectFlagBits::eColor,0,0,1}, .imageExtent = {1,1,1}};
        cmd.copyImageToBuffer(image.image, vk::ImageLayout::eTransferSrcOptimal, readback.Handle(), region);
        const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
    });
    scheduler.Finish();
    assert(Vulkan::DescriptorHeap::cache_hits == hits + 1);
    readback.Invalidate(0, 4);
    assert(readback.mapped_data[0] == 255 && readback.mapped_data[1] == 0 && readback.mapped_data[3] == 255);
    // Whole-size descriptors must bind with a zero dynamic offset, even with a base offset.
    info.offset = offset;
    info.range = VK_WHOLE_SIZE;
    probe.BindResources(writes, Shader::PushData{});
    info = vk::DescriptorBufferInfo{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
    probe.BindResources(writes, Shader::PushData{});
    scheduler.Finish();
    auto cached_write = writes.front();
    cached_write.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
    const auto misses = Vulkan::DescriptorHeap::cache_misses.load();
    // Force pool rotation with distinct ranges; no stale cached sets may survive reset.
    for (unsigned i = 0; i < 96; ++i) {
        info = vk::DescriptorBufferInfo{input.Handle(), 0, 16 + 16 * i};
        heap.CommitCached(probe.SetLayout(), std::span{&cached_write, 1});
    }
    assert(Vulkan::DescriptorHeap::cache_misses == misses + 96);
    const auto cached_hits = Vulkan::DescriptorHeap::cache_hits.load();
    heap.CommitCached(probe.SetLayout(), std::span{&cached_write, 1});
    assert(Vulkan::DescriptorHeap::cache_hits == cached_hits + 1);
    heap.CommitCached(probe.SetLayout(), std::span{&cached_write, 1}, scheduler.CurrentTick() + 1);
    assert(Vulkan::DescriptorHeap::cache_misses == misses + 97);
    std::printf("GPU memory optimizations: mapped upload, retirement, dynamic descriptors and %s pipeline passed\n",
        instance.IsGraphicsPipelineLibrarySupported() ? "library" : "monolithic");
}
