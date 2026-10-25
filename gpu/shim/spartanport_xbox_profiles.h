// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "spartanport_settings.h"
#include "spartanport_graphics.h"
#include <array>

namespace SpartanXbox {
struct Profile { const char *name, *resolution; int fps, queued; bool vsync; bool tune_streaming = false; };
inline constexpr std::array Profiles{
    Profile{"Balanced", "900p", 0, 2, true},
    Profile{"Lower latency", "720p", 0, 1, false},
    Profile{"Image quality", "native", 0, 2, true},
    Profile{"30 FPS limit", "720p", 30, 2, true},
    Profile{"FPS and stability", "720p", 0, 2, false, true},
};
// Existing profiles retain their five-control contract. The opt-in FPS/stability
// profile also tunes image cost and streaming. Never change readback correctness policies.
inline bool ApplyProfile(SpartanSettings::Values& s, int profile) {
    if (profile < 0 || profile >= int(Profiles.size())) return false;
    const auto& p = Profiles[profile];
    s.render_resolution = SpartanGraphics::ParseResolution(p.resolution);
    s.fps_limit = p.fps;
    s.frames_queued = p.queued;
    s.vsync = p.vsync;
    s.async_shaders = true;
    if (p.tune_streaming) {
        s.rcas = false;
        s.anisotropy = 0; // Preserve game filtering rather than force expensive overrides.
        s.texture_lod_bias = 1; // Sampling only; does not reduce resident allocation sizes.
        s.staging_profile = 0; // Compact: 16 MiB preallocation, more application headroom.
        s.texture_preallocate = true;
        s.texture_warmup = true;
        s.background_warmup = false; // Prepare known pipelines before gameplay.
        s.pipeline_cache = true;
        s.threaded_recording = true;
        s.batch_image_barriers = true;
        s.stream_memory = 0; // Capability-aware automatic memory selection.
    }
    return true;
}
inline int MatchProfile(const SpartanSettings::Values& s) {
    for (int i = 0; i < int(Profiles.size()); ++i) {
        const auto& p = Profiles[i];
        if (s.render_resolution == SpartanGraphics::ParseResolution(p.resolution) &&
            s.fps_limit == p.fps && s.frames_queued == p.queued && s.vsync == p.vsync && s.async_shaders &&
            (!p.tune_streaming || (!s.rcas && s.anisotropy == 0 && s.texture_lod_bias == 1 &&
                s.staging_profile == 0 && s.texture_preallocate && s.texture_warmup &&
                !s.background_warmup && s.pipeline_cache && s.threaded_recording &&
                s.batch_image_barriers && s.stream_memory == 0)))
            return i;
    }
    return -1;
}
} // namespace SpartanXbox
