// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/buffer_cache/sparse_backing.h"

#undef NDEBUG
#include <cassert>

int main() {
    constexpr u64 block = 64_KB;
    const vk::DeviceMemory memory{reinterpret_cast<VkDeviceMemory>(uintptr_t{1})};
    // Distinct allocations may share memory. Only contiguous byte ranges can merge.
    const VideoCore::SparseBacking a{{10, 12}, memory, 4 * block, block};
    const VideoCore::SparseBacking b{{12, 14}, memory, 6 * block, block};
    const VideoCore::SparseBacking gap{{14, 16}, memory, 9 * block, block};
    assert(a.CanMergeWith(b));
    assert(!b.CanMergeWith(gap));
    const auto tail = a.SubRange(11, 12);
    assert(tail.start == 11 && tail.end == 12 && tail.offset == 5 * block);
    IntervalList<VideoCore::SparseBacking> ranges;
    ranges.Add(a);
    ranges.Add(b);
    ranges.Add(gap);
    assert(ranges.Size() == 2);
    assert(ranges.begin()->start == 10 && ranges.begin()->end == 14);
    // Overwriting a middle interval must preserve the byte offsets on either side.
    ranges.Add({{11, 13}, memory, 20 * block, block});
    assert(ranges.Size() == 4);
    const auto right = ranges.Find(13);
    assert(right != ranges.end() && right->offset == 7 * block);
    assert(ranges.Find(10)->offset == 4 * block);
    assert(ranges.Find(11)->offset == 20 * block);
}
