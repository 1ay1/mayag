#pragma once
// mayag::platform types shared by every backend and by the runtime.
// Split out so a backend header can include these without pulling in the
// backend-selection logic that in turn includes the backend.

#include "../core/color.hpp"
#include "../core/geometry.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace mayag {

/// Standard cursor shapes.
enum class CursorShape : int {
    arrow = 0, text = 1, pointer = 2, crosshair = 3,
    resize_h = 4, resize_v = 5, resize_nwse = 6, resize_nesw = 7,
    grab = 8, grabbing = 9, not_allowed = 10, wait = 11,
};

}  // namespace mayag

namespace mayag::platform {

/// When the display last refreshed and how often it does, as reported by the
/// compositor (wp_presentation). The host schedules frames against this.
struct DisplayTiming {
    std::chrono::steady_clock::time_point vblank{};   ///< a recent refresh
    std::chrono::nanoseconds              period{};   ///< refresh interval
    std::uint64_t                         seq = 0;    ///< bumps on each new report
};

/// Input to photon, measured: from when mayag received an input event to
/// when the frame showing it was put on the display.
struct PhotonStats {
    double last_ms  = 0.0;
    double mean_ms  = 0.0;
    double worst_ms = 0.0;   ///< over the recent window
    int    samples  = 0;
};

struct WindowConfig {
    std::string title = "mayag";
    Vec2        size{1024, 640};
    bool        resizable   = true;
    bool        decorated   = true;
    bool        transparent = false;
    bool        vsync       = true;
    Color<Srgb> background  = rgb<0x0B0D10>;
};

}  // namespace mayag::platform
