// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/interval_set.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {

// Interval endpoints count sparse blocks; memory offsets are always in bytes.
struct SparseBacking : public Interval {
    vk::DeviceMemory memory;
    u64 offset;
    u64 block_bytes;
    constexpr bool CanMergeWith(const SparseBacking& other) const noexcept {
        return memory == other.memory && block_bytes == other.block_bytes &&
               offset + ((end - start) * block_bytes) == other.offset;
    }
    constexpr SparseBacking SubRange(u64 a, u64 b) const noexcept {
        return {{a, b}, memory, offset + ((a - start) * block_bytes), block_bytes};
    }
};

} // namespace VideoCore
