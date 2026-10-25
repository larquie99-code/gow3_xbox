// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/texture_cache/image_barrier_batch.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
// Keep Vulkan-Hpp dispatcher ABI identical to the renderer, then enable test assertions.
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <vector>

extern "C" void runtime_report() { std::abort(); }
using Barriers = VideoCore::Image::Barriers;

static vk::ImageMemoryBarrier2 Barrier(uint32_t mip, uint32_t layer, unsigned state = 0) {
    return {.srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = state ? vk::ImageLayout::eGeneral : vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = mip, .levelCount = 1, .baseArrayLayer = layer, .layerCount = 1}};
}

static void CheckCoverage(const Barriers& before, const Barriers& after) {
    // Independent expansion checks exact slice coverage and every dependency field.
    using Slice = std::pair<uint32_t, uint32_t>;
    std::map<Slice, vk::ImageMemoryBarrier2> expected, actual;
    auto expand = [](auto& map, const Barriers& barriers) {
        for (auto barrier : barriers) {
            const auto r = barrier.subresourceRange;
            barrier.subresourceRange = vk::ImageSubresourceRange{};
            for (uint32_t mip = r.baseMipLevel; mip < r.baseMipLevel + r.levelCount; ++mip)
                for (uint32_t layer = r.baseArrayLayer; layer < r.baseArrayLayer + r.layerCount; ++layer)
                    assert(map.emplace(Slice{mip, layer}, barrier).second);
        }
    };
    expand(expected, before); expand(actual, after);
    assert(expected == actual);
}

static void CpuTests() {
    Barriers barriers;
    for (uint32_t mip = 0; mip < 8; ++mip)
        for (uint32_t layer = 0; layer < 16; ++layer) barriers.push_back(Barrier(mip, layer));
    auto original = barriers;
    VideoCore::CompactImageBarriers(barriers, 0);
    assert(barriers.size() == 1 && barriers[0].subresourceRange.levelCount == 8 &&
           barriers[0].subresourceRange.layerCount == 16);
    CheckCoverage(original, barriers);

    std::mt19937 random{913};
    for (unsigned trial = 0; trial < 500; ++trial) {
        barriers.clear();
        for (uint32_t mip = 0; mip < 8; ++mip)
            for (uint32_t layer = 0; layer < 16; ++layer)
                if (random() % 4) barriers.push_back(Barrier(mip, layer, random() % 2));
        original = barriers;
        VideoCore::CompactImageBarriers(barriers, 0);
        CheckCoverage(original, barriers); // Gaps/mixed states must never acquire extra transitions.
    }
    // Different masks, layouts, queues, aspects, image or pNext must stay separate.
    for (int field = 0; field < 11; ++field) {
        barriers = {Barrier(0, 0), Barrier(0, 1)};
        auto& b = barriers.back();
        switch (field) {
        case 0: b.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands; break;
        case 1: b.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands; break;
        case 2: b.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite; break;
        case 3: b.dstAccessMask = vk::AccessFlagBits2::eMemoryRead; break;
        case 4: b.oldLayout = vk::ImageLayout::eGeneral; break;
        case 5: b.newLayout = vk::ImageLayout::eGeneral; break;
        case 6: b.srcQueueFamilyIndex = 0; break;
        case 7: b.dstQueueFamilyIndex = 0; break;
        case 8: b.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth; break;
        case 9: b.image = vk::Image{reinterpret_cast<VkImage>(uintptr_t{1})}; break;
        case 10: b.pNext = &field; break;
        }
        VideoCore::CompactImageBarriers(barriers, 0);
        assert(barriers.size() == 2);
    }
    barriers = {Barrier(0, 0), Barrier(0, 1), Barrier(0, 2)};
    VideoCore::CompactImageBarriers(barriers, 1);
    assert(barriers.size() == 2 && barriers[0].subresourceRange.layerCount == 1 &&
           barriers[1].subresourceRange.layerCount == 2); // Previous Transit boundary preserved.
    assert(!VideoCore::AdjacentImageRange(0, VK_REMAINING_ARRAY_LAYERS, 1, 1));
    assert(!VideoCore::AdjacentImageRange(UINT32_MAX - 2, 1, UINT32_MAX - 1, 2));
    std::puts("CPU image barrier coverage: 128 -> 1, 500 mixed-state/gap cases passed");
}

static void GpuTest() {
    Vulkan::Instance instance{0, true};
    Vulkan::Scheduler scheduler{instance, std::getenv("SPARTAN_TEST_THREADED_RECORDING") != nullptr};
    Vulkan::Runtime runtime{instance, scheduler};
    Common::SlotVector<VideoCore::ImageView> views;
    // Start with a format-less guest placeholder, then attach a real test backing.
    VideoCore::Image image{instance, runtime, views, VideoCore::ImageInfo{}};
    image.info.resources = {.levels = 4, .layers = 8};
    auto& backing = image.backing_images.emplace_back();
    image.backing = &backing;
    backing.image = VideoCore::UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    backing.image.Create({.imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm, .extent = {8,8,1}, .mipLevels = 4, .arrayLayers = 8,
        .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc});
    VideoCore::Buffer readback{instance, 0, 64_KB, VideoCore::MemoryType::HostCached};
    Barriers barriers;
    const VideoCore::SubresourceRange part{.base = {.level = 1, .layer = 2},
                                           .extent = {.levels = 2, .layers = 4}};
    image.GetBarriers(barriers, vk::ImageLayout::eTransferDstOptimal,
                     vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eClear, part);
    assert(barriers.size() == 1 && barriers[0].subresourceRange.levelCount == 2 &&
           barriers[0].subresourceRange.layerCount == 4);
    // Capture the actual production barriers by value, including deferred recording mode.
    scheduler.Record([barriers, handle = image.GetImage()](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = uint32_t(barriers.size()),
                              .pImageMemoryBarriers = barriers.data()});
        vk::ClearColorValue color{};
        color.float32[0] = color.float32[3] = 1.0f;
        const auto range = barriers.front().subresourceRange;
        cmd.clearColorImage(handle, vk::ImageLayout::eTransferDstOptimal, color, range);
    });
    barriers.clear();
    image.GetBarriers(barriers, vk::ImageLayout::eTransferSrcOptimal,
                     vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eCopy, part);
    assert(barriers.size() == 1);
    scheduler.Record([barriers, handle = image.GetImage(), buffer = readback.Handle()](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = uint32_t(barriers.size()),
                              .pImageMemoryBarriers = barriers.data()});
        const vk::BufferImageCopy copy{.imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
            .mipLevel = 2, .baseArrayLayer = 2, .layerCount = 4}, .imageExtent = {2,2,1}};
        cmd.copyImageToBuffer(handle, vk::ImageLayout::eTransferSrcOptimal, buffer, copy);
        const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost, .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
    });
    scheduler.Finish();
    readback.Invalidate(0, 64);
    for (int pixel = 0; pixel < 16; ++pixel) {
        const auto* p = readback.mapped_data.data() + pixel * 4;
        assert(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255);
    }
    // Read-after-read needs no barrier; same-layout write-after-write still does.
    barriers.clear();
    image.GetBarriers(barriers, vk::ImageLayout::eTransferSrcOptimal,
                     vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eCopy, part);
    assert(barriers.empty());
    image.GetBarriers(barriers, vk::ImageLayout::eTransferDstOptimal,
                     vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eClear, part);
    assert(barriers.size() == 1);
    barriers.clear();
    image.GetBarriers(barriers, vk::ImageLayout::eTransferDstOptimal,
                     vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eClear, part);
    assert(barriers.size() == 1); // Required memory dependency survives compaction.
    std::puts("GPU production GetBarriers partial mip/layer clear and readback passed");
}
int main(int argc, char**) {
    CpuTests();
    if (argc == 1) GpuTest(); // Any argument selects CPU-only coverage.
}
