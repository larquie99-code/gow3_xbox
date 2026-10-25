// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string>
#include <string_view>

namespace SpartanXbox {
inline constexpr std::array MemoryKeys{"auto", "cached", "host", "device"};
inline constexpr std::array MemoryNames{"Automatic (unified when available)", "CPU cached",
                                       "CPU sequential writes", "GPU-visible upload (BAR/UMA)"};
inline int ParseMemory(std::string_view value) {
    for (int i = 0; i < int(MemoryKeys.size()); ++i)
        if (value == MemoryKeys[i]) return i;
    return 0;
}
inline int NormalizeMemory(int value) {
    return value >= 0 && value < int(MemoryKeys.size()) ? value : 0;
}
inline constexpr std::array StagingKeys{"compact", "balanced", "streaming"};
inline constexpr std::array StagingNames{"Compact (16 MiB blocks)", "Balanced (16 MiB blocks)",
                                        "Streaming (32 MiB blocks)"};
inline int ParseStaging(std::string_view value) {
    for (int i = 0; i < int(StagingKeys.size()); ++i) if (value == StagingKeys[i]) return i;
    return 1;
}
inline int NormalizeStaging(int value) { return value >= 0 && value < 3 ? value : 1; }
struct StagingPolicy {
    unsigned block_mb, keep_mb, host_blocks, device_blocks, idle_frames;
};
inline constexpr std::array StagingPolicies{
    StagingPolicy{16, 32, 1, 0, 120}, StagingPolicy{16, 64, 2, 1, 600},
    StagingPolicy{32, 128, 4, 2, 1800},
};
struct StartupOption { const char *key, *variable, *fallback; };
inline constexpr std::array StartupOptions{
    StartupOption{"pipeline_cache", "SPARTAN_PIPELINE_CACHE", "1"},
    StartupOption{"threaded_recording", "SPARTAN_VK_RECORD_THREAD", "1"},
    StartupOption{"stream_memory", "SPARTAN_STREAM_MEMORY", "auto"},
    StartupOption{"staging_profile", "SPARTAN_STAGING_PROFILE", "balanced"},
    StartupOption{"texture_preallocate", "SPARTAN_TEXTURE_PREALLOCATE", "1"},
    StartupOption{"texture_warmup", "SPARTAN_TEXTURE_WARMUP", "1"},
    StartupOption{"background_warmup", "SPARTAN_BACKGROUND_WARMUP", "0"},
    StartupOption{"batch_image_barriers", "SPARTAN_BATCH_IMAGE_BARRIERS", "1"},
};
inline std::string Canonical(const StartupOption& option, std::string_view value) {
    if (std::string_view(option.key) == "stream_memory") return MemoryKeys[ParseMemory(value)];
    if (std::string_view(option.key) == "staging_profile") return StagingKeys[ParseStaging(value)];
    return value == "0" || value == "1" ? std::string(value) : option.fallback;
}
// Called before renderer initialization. Explicit environment/debug overrides win.
// Callbacks keep the Windows host and unit tests on exactly the same bridge logic.
template <class ReadSetting, class ReadEnvironment, class WriteEnvironment>
void ApplyStartupOptions(ReadSetting read, ReadEnvironment environment, WriteEnvironment write) {
    for (const auto& option : StartupOptions)
        if (!environment(option.variable))
            write(option.variable, Canonical(option, read(option.key, option.fallback)).c_str());
}
} // namespace SpartanXbox
