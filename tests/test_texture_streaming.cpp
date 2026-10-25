// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/tile_manager.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/upload_range.h"
#include "video_core/buffer_cache/dense_arena_store.h"
#include "spartanport_memory_pressure.h"
#include <vk_mem_alloc.h>
#include "video_core/texture_cache/image_view.h"
#include "video_core/cache_storage.h"
#include "common/path_util.h"
#include "common/elf_info.h"
#include "../gpu/spartanportgpu.h"
#include <filesystem>
#include <fstream>
#include <thread>
// Keep Vulkan-Hpp's ABI consistent with the release renderer, then enable test assertions.
#undef NDEBUG
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Same inline friend definition as the host; initializes only synthetic test metadata.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const SpartanGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};
}
extern "C" void runtime_report() { std::abort(); }

static void Detile(VideoCore::TileManager& tiles, Vulkan::Instance& instance,
                   Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime, unsigned bits) {
    const unsigned bytes = bits / 8, size = 64 * bytes;
    VideoCore::Buffer input{instance, 0, 64_KB, VideoCore::MemoryType::HostUncached};
    VideoCore::Buffer output{instance, 0, 64_KB, VideoCore::MemoryType::HostCached};
    // Independent CPU Morton oracle for one 8x8 Thin1D microtile. Values vary per byte/block.
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
        unsigned tiled = 0;
        for (unsigned b = 0; b < 3; ++b) tiled |= ((x >> b) & 1) << (2*b) | ((y >> b) & 1) << (2*b+1);
        for (unsigned c = 0; c < bytes; ++c) input.mapped_data[tiled*bytes+c] = u8((y*8+x)*17+c);
    }
    input.Flush(0, size);
    VideoCore::ImageInfo info{};
    info.tile_mode = AmdGpu::TileMode::Thin1DThin;
    info.array_mode = AmdGpu::GetArrayMode(info.tile_mode);
    info.props.is_tiled = 1;
    info.props.is_block = bits >= 64;
    info.num_bits = bits;
    info.num_samples = info.guest_samples = 1;
    info.guest_size = size;
    info.resources = {.levels = 1, .layers = 1};
    const unsigned extent = info.props.is_block ? 32 : 8;
    info.size = {extent, extent, 1};
    info.mips_layout[0] = {size, extent, extent, 0};
    const auto count = tiles.PipelineCount();
    const auto [linear, offset] = tiles.DetileImage(&input, 0, info);
    assert(tiles.PipelineCount() == count); // Common pipeline prewarmed; no first-use compile.
    const vk::BufferCopy copy{.srcOffset = offset, .dstOffset = 0, .size = size};
    runtime.CopyBuffer(linear, &output, {&copy, 1});
    runtime.FlushBarriers();
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host});
    });
    scheduler.Finish();
    output.Invalidate(0, size);
    for (unsigned pixel = 0; pixel < 64; ++pixel) for (unsigned c = 0; c < bytes; ++c)
        assert(output.mapped_data[pixel*bytes+c] == u8(pixel*17+c));
}
static void UploadMips(Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime) {
    Common::SlotVector<VideoCore::ImageView> views;
    VideoCore::Image image{instance,runtime,views,VideoCore::ImageInfo{}};
    image.info.resources = {.levels=2,.layers=1};
    image.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    image.info.num_samples = 1;
    image.info.guest_size = 1024;
    image.info.mips_layout[0] = {64, 4, 4, 0};
    image.info.mips_layout[1] = {16, 2, 2, 512};
    auto& backing = image.backing_images.emplace_back();
    image.backing = &backing;
    backing.image = VideoCore::UniqueImage{instance.GetDevice(),instance.GetAllocator()};
    backing.image.Create({.imageType=vk::ImageType::e2D,.format=image.info.pixel_format,
        .extent={4,4,1},.mipLevels=2,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
        .tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc});
    VideoCore::Buffer input{instance,0,64_KB,VideoCore::MemoryType::HostUncached};
    VideoCore::Buffer output{instance,0,64_KB,VideoCore::MemoryType::HostCached};
    std::memset(input.mapped_data.data(), 0xCD, 1024);
    for(unsigned i=0;i<64;++i) input.mapped_data[i]=u8(i);
    for(unsigned i=0;i<16;++i) input.mapped_data[512+i]=u8(200+i);
    input.Flush(0,1024);
    const std::array<vk::BufferImageCopy,2> copies{{
        {.bufferOffset=0,.imageSubresource={.aspectMask=vk::ImageAspectFlagBits::eColor,.mipLevel=0,.layerCount=1},.imageExtent={4,4,1}},
        {.bufferOffset=512,.imageSubresource={.aspectMask=vk::ImageAspectFlagBits::eColor,.mipLevel=1,.layerCount=1},.imageExtent={2,2,1}}
    }};
    runtime.UploadImage(&image,&input,copies);
    // Only refresh mip1. The original guest range is 1024 bytes; stage 16, preserve mip0.
    std::array<vk::BufferImageCopy, 1> partial{copies[1]};
    const auto range = VideoCore::GetUploadRange(image.info, partial);
    assert(range.offset == 512 && range.size == 16);
    auto tiled = image.info; tiled.props.is_tiled = 1;
    assert(VideoCore::GetUploadRange(tiled, partial).size == 1024);
    auto invalid = image.info; invalid.mips_layout[1].size = 2048;
    assert(VideoCore::GetUploadRange(invalid, partial).size == 1024);
    const auto all = VideoCore::GetUploadRange(image.info, copies);
    assert(all.offset == 0 && all.size == 528);
    assert(VideoCore::GetUploadRange(image.info, {}).size == 0);
    auto staged = runtime.GetStagingPool().Request(range.size, VideoCore::MemoryType::HostUncached);
    for (unsigned i = 0; i < range.size; ++i) staged.mapped[i] = u8(100 + i);
    staged.Flush();
    VideoCore::RebaseUploadCopies(partial, range, staged.offset);
    runtime.UploadImage(&image, staged.buffer, partial);
    runtime.DownloadImage(&image,&output,copies);
    runtime.FlushBarriers();
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host});
    });
    scheduler.Finish(); output.Invalidate(0,1024);
    assert(std::memcmp(input.mapped_data.data(),output.mapped_data.data(),64)==0);
    for (unsigned i = 0; i < 16; ++i) assert(output.mapped_data[512+i] == u8(100+i));
}
static void DenseRetirement(Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime) {
    VideoCore::DenseArenaStore store;
    auto* first = store.Create(instance, 0, 64_KB);
    auto* second = store.Create(instance, 64_KB, 64_KB);
    auto* merged = store.Create(instance, 0, 128_KB);
    runtime.FillBuffer(first, 0, 64_KB, 0x12121212);
    runtime.FillBuffer(second, 0, 64_KB, 0x34343434);
    vk::BufferCopy region{0, 0, 64_KB};
    runtime.CopyBuffer(first, merged, {&region, 1});
    region.dstOffset = 64_KB;
    runtime.CopyBuffer(second, merged, {&region, 1});
    store.Retire(first); store.Retire(second);
    assert(store.ActiveBytes() == 128_KB);
    // An internal GPU submission may complete while CPU code still holds old arena pointers.
    scheduler.Finish();
    VideoCore::Buffer result{instance, 0, 128_KB, VideoCore::MemoryType::HostCached};
    region = vk::BufferCopy{0, 0, 64_KB};
    runtime.CopyBuffer(first, &result, {&region, 1}); // Old object must still be alive.
    VmaTotalStatistics before{}, after{};
    vmaCalculateStatistics(instance.GetAllocator(), &before);
    assert(store.QueueRetired(scheduler) == 128_KB && store.PendingBytes() == 128_KB);
    for (int i = 0; i < 40; ++i) scheduler.PopPendingOperations();
    assert(store.PendingBytes() == 128_KB); // Current tick not submitted yet.
    scheduler.Finish();
    for (int i = 0; i < 40; ++i) scheduler.PopPendingOperations();
    assert(store.PendingBytes() == 0);
    vmaCalculateStatistics(instance.GetAllocator(), &after);
    assert(after.total.statistics.allocationCount + 2 == before.total.statistics.allocationCount);
    // Repeat growth: only the replacement arena stays allocated after GPU completion.
    auto* larger = store.Create(instance, 0, 192_KB);
    region = vk::BufferCopy{0, 0, 128_KB};
    runtime.CopyBuffer(merged, larger, {&region, 1});
    store.Retire(merged);
    runtime.CopyBuffer(larger, &result, {&region, 1});
    store.QueueRetired(scheduler);
    runtime.FlushBarriers();
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host});
    });
    scheduler.Finish();
    for (int i = 0; i < 40; ++i) scheduler.PopPendingOperations();
    result.Invalidate(0, 128_KB);
    for (size_t i = 0; i < 128_KB; ++i) assert(result.mapped_data[i] == (i < 64_KB ? 0x12 : 0x34));
    assert(store.ActiveBytes() == 192_KB && store.PendingBytes() == 0);
}
static void PressureReclaim(Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime) {
    assert(SpartanMemory::StagingReclaimTarget(4352_MB - 1, 5120_MB) == 0);
    assert(SpartanMemory::StagingReclaimTarget(4352_MB, 5120_MB) == 64_MB);
    assert(SpartanMemory::StagingReclaimTarget(6000_MB, 5120_MB) == 64_MB);
    assert(SpartanMemory::StagingReclaimTarget(100_MB, 0) == 0);
    auto& pool = runtime.GetStagingPool();
    const auto busy = pool.Request(20_MB, VideoCore::MemoryType::HostUncached);
    const auto held = pool.Request(64_KB, VideoCore::MemoryType::HostCached, 0, true);
    std::memset(busy.mapped, 0x57, 64_KB); busy.Flush();
    std::memset(held.mapped, 0x68, 64_KB); held.Flush();
    pool.TickFrame();
    const auto tick = scheduler.CurrentTick();
    pool.ReclaimUnused(~u64{0});
    assert(scheduler.CurrentTick() == tick); // Never submits or waits to free busy resources.
    scheduler.Finish(); // Allocation tick completes before a later GPU use of the same buffer.
    VideoCore::Buffer result{instance, 0, 64_KB, VideoCore::MemoryType::HostCached};
    const vk::BufferCopy copy{busy.offset, 0, 64_KB};
    runtime.CopyBuffer(busy.buffer, &result, {&copy, 1});
    pool.ReclaimUnused(~u64{0}); // Must also honor last_gpu_tick, not only reservation tick.
    runtime.FlushBarriers();
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host});
    });
    scheduler.Finish();
    pool.TickFrame();
    const auto before = pool.AllocatedBytes();
    const auto freed = pool.ReclaimUnused(~u64{0});
    assert(freed > 0 && pool.AllocatedBytes() == before - freed);
    assert(pool.AllocatedBytes() == held.buffer->SizeBytes());
    result.Invalidate(0,64_KB);
    for (size_t i = 0; i < 64_KB; ++i) {
        assert(result.mapped_data[i] == 0x57);
        assert(held.mapped[i] == 0x68); // CPU still owns this readback despite completed GPU work.
    }
    pool.FreeDeferred(held);
    pool.TickFrame();
    assert(pool.ReclaimUnused(~u64{0}) == 0); // Release is tagged with the current pending tick.
    scheduler.Finish(); pool.TickFrame();
    assert(pool.ReclaimUnused(~u64{0}) == 64_KB && pool.AllocatedBytes() == 0);
    auto fresh = pool.Request(64_KB, VideoCore::MemoryType::HostUncached);
    assert(fresh.buffer && fresh.mapped); // Empty ring remains reusable.
    scheduler.Finish();
}
int main(int argc, char** argv) {
    assert(argc == 2); // caller provides an empty disposable cache directory
    std::filesystem::create_directories(argv[1]);
    Common::FS::SetUserPath(Common::FS::PathType::CacheDir, argv[1]);
    SpartanGpuConfig config{}; config.serial = "TEXTURE_TEST";
    Core::Emulator::FillElfInfo(config);
    auto& db = Storage::DataBase::Instance();
    db.Open(); db.FinishPreload();
    Vulkan::Instance instance{0, true};
    Vulkan::Scheduler scheduler{instance, std::getenv("SPARTAN_TEST_THREADED_RECORDING") != nullptr};
    Vulkan::Runtime runtime{instance, scheduler};
    VideoCore::StreamBuffer uniforms{instance, scheduler, VideoCore::MemoryType::Stream, 1_MB};
    {
        VideoCore::TileManager tiles{instance, scheduler, runtime, uniforms};
        unsigned calls = 0;
        tiles.WarmUp([&](u32 done,u32 total) { assert(done <= total); ++calls; });
        assert(calls >= 10 && tiles.PipelineCount() == 10);
        for (unsigned bits : {8u,16u,32u,64u,128u}) Detile(tiles,instance,scheduler,runtime,bits);
        tiles.WarmUp([&](u32,u32) { assert(false); }); // Initialization is idempotent.
        tiles.CheckpointCache(true);
    }
    const auto cache = std::filesystem::path(argv[1]) / Common::ElfInfo::Instance().GameSerial();
    // Database writes asynchronously. Wait for both complete atomic files, bounded to five seconds.
    for (int i = 0; i < 100 && (!std::filesystem::exists(cache / "texture-translation-v1.vkcache") ||
         !std::filesystem::exists(cache / "texture-translation-keys-v1.vkcache")); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(std::filesystem::file_size(cache / "texture-translation-v1.vkcache") >= sizeof(VkPipelineCacheHeaderVersionOne));
    assert(std::filesystem::file_size(cache / "texture-translation-keys-v1.vkcache") == 10*4*sizeof(u32));
    {
        VideoCore::TileManager restored{instance,scheduler,runtime,uniforms};
        restored.WarmUp();
        assert(restored.PipelineCount() == 10 && restored.LoadedCacheBytes() >= sizeof(VkPipelineCacheHeaderVersionOne));
        Detile(restored,instance,scheduler,runtime,128);
    }
    UploadMips(instance,scheduler,runtime);
    DenseRetirement(instance,scheduler,runtime);
    auto& pool = runtime.GetStagingPool();
    const auto ref = pool.Request(20_MB, VideoCore::MemoryType::HostUncached);
    // Streaming profile uses a 32 MiB ring, rather than a dedicated 20 MiB resource.
    assert(ref.buffer->SizeBytes() == 32_MB && ref.mapped);
    const auto held = pool.Request(64_KB, VideoCore::MemoryType::HostCached, 0, true);
    const auto other = pool.Request(64_KB, VideoCore::MemoryType::HostCached, 0, true);
    assert(held.buffer != other.buffer);
    pool.FreeDeferred(held); pool.FreeDeferred(other);
    scheduler.Finish();
    PressureReclaim(instance,scheduler,runtime);
    db.Close();
    puts("Texture streaming: 5 bit widths/BC blocks read back exactly, cached/prewarmed pipelines, 32MiB staging and held readbacks passed");
}
