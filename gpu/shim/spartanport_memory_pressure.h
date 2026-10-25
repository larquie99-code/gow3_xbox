// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace SpartanMemory {
// Xbox RAM and GPU allocations share the app budget. Reclaim before image GC starts.
// Bound each pass; these are targets, not promises that VMA releases an OS heap block.
inline uint64_t StagingReclaimTarget(uint64_t used, uint64_t budget) {
    constexpr uint64_t MiB = 1ull << 20;
    if (budget <= 896 * MiB || used < budget - 768 * MiB) return 0;
    return std::min(64 * MiB, used - (budget - 896 * MiB));
}
}
