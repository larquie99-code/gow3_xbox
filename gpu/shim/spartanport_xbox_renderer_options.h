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
struct StartupOption { const char *key, *variable, *fallback; };
inline constexpr std::array StartupOptions{
    StartupOption{"pipeline_cache", "SPARTAN_PIPELINE_CACHE", "1"},
    StartupOption{"threaded_recording", "SPARTAN_VK_RECORD_THREAD", "1"},
    StartupOption{"stream_memory", "SPARTAN_STREAM_MEMORY", "auto"},
};
inline std::string Canonical(const StartupOption& option, std::string_view value) {
    if (std::string_view(option.key) == "stream_memory") return MemoryKeys[ParseMemory(value)];
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
