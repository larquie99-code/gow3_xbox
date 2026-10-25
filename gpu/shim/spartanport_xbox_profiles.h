// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "spartanport_settings.h"
#include "spartanport_graphics.h"
#include <array>

namespace SpartanXbox {
struct Profile { const char *name, *resolution; int fps, queued; bool vsync; };
inline constexpr std::array Profiles{
    Profile{"Balanced", "900p", 0, 2, true},
    Profile{"Lower latency", "720p", 0, 1, false},
    Profile{"Image quality", "native", 0, 2, true},
    Profile{"30 FPS limit", "720p", 30, 2, true},
};
// Only these five presentation/quality controls change. Readback, memory,
// cache and recording choices remain independently configurable.
inline bool ApplyProfile(SpartanSettings::Values& s, int profile) {
    if (profile < 0 || profile >= int(Profiles.size())) return false;
    const auto& p = Profiles[profile];
    s.render_resolution = SpartanGraphics::ParseResolution(p.resolution);
    s.fps_limit = p.fps;
    s.frames_queued = p.queued;
    s.vsync = p.vsync;
    s.async_shaders = true;
    return true;
}
inline int MatchProfile(const SpartanSettings::Values& s) {
    for (int i = 0; i < int(Profiles.size()); ++i) {
        const auto& p = Profiles[i];
        if (s.render_resolution == SpartanGraphics::ParseResolution(p.resolution) &&
            s.fps_limit == p.fps && s.frames_queued == p.queued && s.vsync == p.vsync && s.async_shaders)
            return i;
    }
    return -1;
}
} // namespace SpartanXbox
