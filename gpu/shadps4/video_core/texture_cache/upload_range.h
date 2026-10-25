// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <span>
#include "video_core/texture_cache/image_info.h"

namespace VideoCore {
struct UploadRange { u64 offset, size; };
// Tiled translation needs the complete original layout. Linear images can stage only
// the bounding range of the mips actually refreshed, including all layers and padding.
inline UploadRange GetUploadRange(const ImageInfo& info, std::span<const vk::BufferImageCopy> copies) {
    const UploadRange full{0, info.guest_size};
    if (info.props.is_tiled) return full;
    if (copies.empty()) return {0, 0};
    u64 begin = info.guest_size, end = 0;
    for (const auto& copy : copies) {
        const auto level = copy.imageSubresource.mipLevel;
        if (level >= info.resources.levels || level >= info.mips_layout.size()) return full;
        const auto& mip = info.mips_layout[level];
        if (!mip.size || mip.offset > info.guest_size || mip.size > info.guest_size - mip.offset ||
            copy.bufferOffset != mip.offset) return full;
        begin = std::min<u64>(begin, mip.offset);
        end = std::max<u64>(end, u64(mip.offset) + mip.size);
    }
    // Preserve Vulkan's 4-byte / compressed-block alignment of every rebased copy.
    begin &= ~u64{15};
    return {begin, end - begin};
}
inline void RebaseUploadCopies(std::span<vk::BufferImageCopy> copies, UploadRange range, u64 offset) {
    for (auto& copy : copies) copy.bufferOffset = copy.bufferOffset - range.offset + offset;
}
}
