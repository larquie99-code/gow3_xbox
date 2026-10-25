// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <list>
#include <memory>
#include "video_core/buffer_cache/buffer.h"

namespace VideoCore {
// Stable CPU addresses through arena replacement, command preparation and GPU completion.
class DenseArenaStore {
public:
    Buffer* Create(const Vulkan::Instance& instance, VAddr address, u64 size);
    void Retire(const Buffer* buffer);
    // Only at a guest submission boundary, after draining CPU draw preparation.
    // Individual scheduler submits may happen while a draw still holds Buffer pointers.
    u64 QueueRetired(Vulkan::Scheduler& scheduler);
    u64 ActiveBytes() const;
    u64 PendingBytes() const { return pending_bytes->load(std::memory_order_relaxed); }
private:
    std::list<Buffer> active, retired;
    std::shared_ptr<std::atomic<u64>> pending_bytes = std::make_shared<std::atomic<u64>>(0);
};
}
