// SPDX-License-Identifier: GPL-2.0-or-later
#include "dense_arena_store.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <algorithm>

namespace VideoCore {
Buffer* DenseArenaStore::Create(const Vulkan::Instance& instance, VAddr address, u64 size) {
    return &active.emplace_back(instance, address, size, MemoryType::DeviceLocal);
}
void DenseArenaStore::Retire(const Buffer* buffer) {
    const auto it = std::find_if(active.begin(), active.end(),
                                 [buffer](const Buffer& entry) { return &entry == buffer; });
    ASSERT_MSG(it != active.end(), "Retiring an arena not owned by the active store");
    retired.splice(retired.end(), active, it); // Never move a Buffer while callers hold its address.
}
u64 DenseArenaStore::QueueRetired(Vulkan::Scheduler& scheduler) {
    if (retired.empty()) return 0;
    u64 bytes = 0;
    for (const auto& buffer : retired) bytes += buffer.SizeBytes();
    pending_bytes->fetch_add(bytes, std::memory_order_relaxed);
    std::list<Buffer> batch;
    batch.splice(batch.end(), retired);
    scheduler.DeferOperation([buffers = std::move(batch), pending = pending_bytes, bytes]() mutable {
        buffers.clear();
        pending->fetch_sub(bytes, std::memory_order_relaxed);
    });
    return bytes;
}
u64 DenseArenaStore::ActiveBytes() const {
    u64 bytes = 0;
    for (const auto& buffer : active) bytes += buffer.SizeBytes();
    return bytes;
}
}
