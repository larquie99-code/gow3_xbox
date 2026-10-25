// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "spartanport_toggles.h"
#include "spartanport_xbox_renderer_options.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// spartanport: staging memory is populated (allocated and cleared) by the kernel on first CPU access,
// ~2 ms per 16 MiB block. Streaming bursts a few seconds apart re-created the blocks trimmed
// in between and copies into them ran at 0.3 GB/s (stutter). Blocks now live 30 s idle, the
// upload ring keeps SPARTAN_STAGING_KEEP_MB (512) and is populated at startup.
#ifdef GOW3_UWP
constexpr u64 RING_IDLE_FRAMES = 120;
constexpr u64 LARGE_IDLE_FRAMES = 120;
constexpr u64 PREWARM_BLOCKS = 2;
constexpr u64 DEFAULT_KEEP_MB = 64;
#else
constexpr u64 RING_IDLE_FRAMES = 3000;
constexpr u64 LARGE_IDLE_FRAMES = 3000;
constexpr u64 PREWARM_BLOCKS = 8;
constexpr u64 DEFAULT_KEEP_MB = 512;
#endif
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    ring_idle_frames = RING_IDLE_FRAMES;
    large_idle_frames = LARGE_IDLE_FRAMES;
    u64 host_blocks = PREWARM_BLOCKS, device_blocks = 0, keep_mb = DEFAULT_KEEP_MB;
    const char* profile = std::getenv("SPARTAN_STAGING_PROFILE");
#ifdef GOW3_UWP
    if (!profile) profile = "balanced";
#endif
    if (profile) {
        const auto policy = SpartanXbox::StagingPolicies[SpartanXbox::ParseStaging(profile)];
        block_size = u64(policy.block_mb) * 1_MB;
        keep_mb = policy.keep_mb;
        host_blocks = policy.host_blocks;
        device_blocks = policy.device_blocks;
        ring_idle_frames = large_idle_frames = policy.idle_frames;
    }
    // Preserve the debug override, but bound malformed/oversized values before multiplication.
    if (const char* env = std::getenv("SPARTAN_STAGING_KEEP_MB")) {
        char* end{};
        const auto parsed = std::strtol(env, &end, 10);
        if (end != env && *end == 0) keep_mb = u64(std::clamp(parsed, 0L, 512L));
    }
    keep_blocks = keep_mb * 1_MB / block_size;
    const char* preallocate = std::getenv("SPARTAN_TEXTURE_PREALLOCATE");
    if (preallocate && *preallocate == '0') host_blocks = device_blocks = 0;
    for (const auto type : {MemoryType::HostUncached, MemoryType::DeviceLocal}) {
        Ring& ring = rings[u32(type)];
        const u64 count = std::min(keep_blocks, type == MemoryType::HostUncached ? host_blocks : device_blocks);
        for (u64 i = 0; i < count; ++i) {
            auto& block = ring.blocks.emplace_back(Block{
                .buffer = std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, block_size),
                .last_used_frame = frame,
            });
            if (!block.buffer->mapped_data.empty()) {
                std::memset(block.buffer->mapped_data.data(), 0, block.buffer->mapped_data.size());
                block.buffer->Flush(0, block.buffer->SizeBytes());
            }
        }
    }
    std::printf("Texture staging: %llu MiB blocks, retain up to %llu MiB per ring, idle %llu frames\n",
                (unsigned long long)(block_size / 1_MB), (unsigned long long)keep_mb,
                (unsigned long long)ring_idle_frames);

}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment,
                                            bool deferred) {
    Ring& ring = rings[u32(type)];
    if (deferred || size > block_size) {
        return RequestLarge(size, type, deferred);
    }
    return RequestFromRing(ring, size, alignment, type);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type) {
    Block* best{};
    u64 best_offset{};
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (ring.current + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        best = &ring.blocks.emplace_back(Block{
            .buffer =
                std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, block_size),
            .last_used_frame = frame,
        });
        ring.current = ring.blocks.size() - 1;
        best_offset = *best->buffer->Reserve(size, alignment, false);
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            !scheduler.IsFree(std::max(entry.tick, entry.buffer->last_gpu_tick))) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    for (Ring& ring : rings) {
        TrimRing(ring);
    }
    for (auto& cache : large_caches) {
        TrimLarge(cache);
    }
    PublishStats();
}

u64 StagingBufferPool::ReclaimUnused(u64 target_bytes) {
    if (!target_bytes) return 0;
    // One completion snapshot for the entire pass, not one driver query per busy block.
    auto* completed = scheduler.GetWorkSemaphore();
    completed->Refresh();
    u64 freed = 0;
    // Dedicated allocations include large transient uploads; held CPU readbacks stay owned.
    for (auto& cache : large_caches) {
        std::erase_if(cache, [&](const LargeBuffer& entry) {
            if (freed >= target_bytes || entry.held || entry.last_used_frame == frame ||
                !completed->IsFree(std::max(entry.tick, entry.buffer->last_gpu_tick))) return false;
            freed += entry.buffer->SizeBytes();
            return true;
        });
    }
    for (auto& ring : rings) {
        for (size_t i = ring.blocks.size(); i-- > 0 && freed < target_bytes;) {
            const auto& block = ring.blocks[i];
            if (block.last_used_frame == frame || !completed->IsFree(std::max(block.buffer->LastTick(), block.buffer->last_gpu_tick))) continue;
            freed += block.buffer->SizeBytes();
            ring.blocks.erase(ring.blocks.begin() + i);
        }
        // Erasing any block can shift the ring's hint. Buffer objects remain at stable addresses.
        ring.current = 0;
    }
    pressure_reclaimed_bytes.fetch_add(freed, std::memory_order_relaxed);
    return freed;
}

u64 StagingBufferPool::AllocatedBytes() const {
    u64 bytes = 0;
    for (const auto& ring : rings)
        for (const auto& block : ring.blocks) bytes += block.buffer->SizeBytes();
    for (const auto& cache : large_caches)
        for (const auto& entry : cache) bytes += entry.buffer->SizeBytes();
    return bytes;
}

void StagingBufferPool::TrimRing(Ring& ring) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        if (blocks.size() <= keep_blocks || frame - block.last_used_frame < ring_idle_frames ||
            !scheduler.IsFree(std::max(block.buffer->LastTick(), block.buffer->last_gpu_tick))) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache) {
    std::erase_if(cache, [this](const LargeBuffer& entry) {
        return !entry.held && frame - entry.last_used_frame >= large_idle_frames &&
               scheduler.IsFree(std::max(entry.tick, entry.buffer->last_gpu_tick));
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
