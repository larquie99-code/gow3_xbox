// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <initializer_list>
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
// Coalesce only subresources of ONE transition. Never merge across Transit calls:
// an intervening access or a different layout may require the original ordering.
inline bool SameImageDependency(const vk::ImageMemoryBarrier2& a,
                                const vk::ImageMemoryBarrier2& b) {
    return !a.pNext && !b.pNext && a.image == b.image &&
           a.srcStageMask == b.srcStageMask && a.srcAccessMask == b.srcAccessMask &&
           a.dstStageMask == b.dstStageMask && a.dstAccessMask == b.dstAccessMask &&
           a.oldLayout == b.oldLayout && a.newLayout == b.newLayout &&
           a.srcQueueFamilyIndex == b.srcQueueFamilyIndex &&
           a.dstQueueFamilyIndex == b.dstQueueFamilyIndex &&
           a.subresourceRange.aspectMask == b.subresourceRange.aspectMask;
}

inline bool AdjacentImageRange(uint32_t base, uint32_t count, uint32_t next,
                               uint32_t next_count) {
    // VK_REMAINING_* is UINT32_MAX: do not interpret it as a finite range.
    constexpr auto remaining = std::numeric_limits<uint32_t>::max();
    return count && next_count && count != remaining && next_count != remaining &&
           uint64_t(base) + count == next &&
           uint64_t(next) + next_count < remaining;
}

template <class Barriers>
void CompactImageBarriers(Barriers& barriers, size_t first) {
    if (first >= barriers.size() || barriers.size() - first < 2) return;
    // Image::GetBarriers emits mip-major, layer-minor order. First combine layer
    // runs within a mip, then mip runs with exactly the same layer coverage.
    // Keeping rectangular coverage avoids adding transitions to untouched slices.
    for (const bool layers : {true, false}) {
        size_t out = first;
        const size_t end = barriers.size();
        for (size_t in = first; in < end; ++in) {
            if (out > first && SameImageDependency(barriers[out - 1], barriers[in])) {
                auto& a = barriers[out - 1].subresourceRange;
                const auto& b = barriers[in].subresourceRange;
                const bool join = layers
                    ? a.baseMipLevel == b.baseMipLevel && a.levelCount == b.levelCount &&
                      AdjacentImageRange(a.baseArrayLayer, a.layerCount, b.baseArrayLayer, b.layerCount)
                    : a.baseArrayLayer == b.baseArrayLayer && a.layerCount == b.layerCount &&
                      AdjacentImageRange(a.baseMipLevel, a.levelCount, b.baseMipLevel, b.levelCount);
                if (join) {
                    if (layers) a.layerCount += b.layerCount;
                    else a.levelCount += b.levelCount;
                    continue;
                }
            }
            if (out != in) barriers[out] = barriers[in];
            ++out;
        }
        barriers.resize(out);
    }
}
} // namespace VideoCore
