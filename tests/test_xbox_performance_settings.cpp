// SPDX-License-Identifier: GPL-2.0-or-later
#include "spartanport_xbox_renderer_options.h"
#include "spartanport_xbox_profiles.h"
#include "test_environment.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

namespace X = SpartanXbox;
namespace S = SpartanSettings;
static const char* path = "xbox-performance-settings-test.ini";
static void ClearOverrides() {
    for (const auto& option : X::StartupOptions) SetTestEnvironment(option.variable, "");
    SetTestEnvironment("SPARTAN_ASYNC_SHADERS", "");
}
static void BootBridge() {
    std::map<std::string, std::string> values;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        auto eq = line.find('=');
        if (eq != std::string::npos) values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    X::ApplyStartupOptions([&](const char* key, const char* fallback) {
        auto it = values.find(key);
        return it == values.end() ? std::string(fallback) : it->second;
    }, [](const char* key) { return std::getenv(key); }, SetTestEnvironment);
}
int main() {
    SetTestEnvironment("SPARTAN_CONFIG", path);
    ClearOverrides();
    std::ofstream(path) << "# existing installation\n";
    BootBridge(); S::Load();
    auto& s = S::Get();
    assert(s.pipeline_cache && s.threaded_recording && s.stream_memory == 0);
    assert(S::RendererRestartChanges() == 0 && !S::GraphicsNeedRestart());
    s.pipeline_cache = false; s.threaded_recording = false; s.stream_memory = 3;
    assert(S::RendererRestartChanges() == 3 && S::GraphicsNeedRestart());
    assert(s.startup_pipeline_cache && s.startup_threaded_recording && s.startup_stream_memory == 0);
    S::Save(); ClearOverrides(); BootBridge(); S::Load();
    assert(!s.pipeline_cache && !s.threaded_recording && s.stream_memory == 3);
    assert(!s.startup_pipeline_cache && !s.startup_threaded_recording && s.startup_stream_memory == 3);
    assert(S::RendererRestartChanges() == 0 && !S::GraphicsNeedRestart());
    // All memory modes survive disk -> Windows host bridge -> effective renderer preference.
    for (int memory = 0; memory < 4; ++memory) {
        s.stream_memory = memory;
        S::Save(); ClearOverrides(); BootBridge(); S::Load();
        assert(s.stream_memory == memory && s.startup_stream_memory == memory);
    }
    // Explicit private debug overrides beat saved preferences and are displayed as active.
    s.pipeline_cache = true; s.threaded_recording = true; s.stream_memory = 0; S::Save();
    SetTestEnvironment("SPARTAN_PIPELINE_CACHE", "0");
    SetTestEnvironment("SPARTAN_VK_RECORD_THREAD", "0");
    SetTestEnvironment("SPARTAN_STREAM_MEMORY", "cached");
    BootBridge(); S::Load();
    assert(!s.pipeline_cache && !s.threaded_recording && s.stream_memory == 1);
    assert(S::RendererRestartChanges() == 0);
    // Match the backend's flag syntax when mirroring existing debug overrides.
    SetTestEnvironment("SPARTAN_PIPELINE_CACHE", "yes");
    SetTestEnvironment("SPARTAN_VK_RECORD_THREAD", "yes");
    S::Load(); assert(s.pipeline_cache && s.threaded_recording);
    ClearOverrides();
    std::ofstream(path) << "pipeline_cache=broken\nthreaded_recording=-8\nstream_memory=invalid\n";
    BootBridge(); S::Load();
    assert(s.pipeline_cache && s.threaded_recording && s.stream_memory == 0);
    assert(S::RendererRestartChanges() == 0);
    // Presets only touch the advertised controls; no readback or startup-renderer surprises.
    s.stale_readback = false; s.deferred_readback = false;
    s.pipeline_cache = false; s.threaded_recording = false; s.stream_memory = 1;
    const int expected_res[] = {3, 2, 0, 2}; // 900p, 720p, native, 720p.
    const int expected_fps[] = {0, 0, 0, 30}, expected_queue[] = {2, 1, 2, 2};
    for (int profile = 0; profile < 4; ++profile) {
        assert(X::ApplyProfile(s, profile));
        assert(s.render_resolution == expected_res[profile]);
        assert(s.fps_limit == expected_fps[profile] && s.frames_queued == expected_queue[profile]);
        assert(s.vsync == (profile != 1) && s.async_shaders);
        assert(!s.stale_readback && !s.deferred_readback && !s.pipeline_cache &&
               !s.threaded_recording && s.stream_memory == 1);
        assert(X::MatchProfile(s) == profile);
        s.vsync = !s.vsync; assert(X::MatchProfile(s) == -1);
    }
    const bool previous = s.vsync;
    assert(!X::ApplyProfile(s, -1) && !X::ApplyProfile(s, 99) && s.vsync == previous);
    // New renderer fields participate in pending changes; live VSync does not.
    ClearOverrides(); S::Save(); BootBridge(); S::Load();
    s.vsync = !s.vsync;
    assert(!S::GraphicsNeedRestart());
    s.threaded_recording = !s.threaded_recording;
    assert(S::RendererRestartChanges() == 1 && S::GraphicsNeedRestart());
    s.threaded_recording = s.startup_threaded_recording;
    assert(S::RendererRestartChanges() == 0 && !S::GraphicsNeedRestart());
    ClearOverrides(); SetTestEnvironment("SPARTAN_CONFIG", "");
    std::remove(path);
    std::puts("Xbox settings: profiles, custom, persistence, host bridge, overrides, live VSync and restart detection passed");
}
