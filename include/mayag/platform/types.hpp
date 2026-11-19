#pragma once
// mayag::platform types shared by every backend and by the runtime.
// Split out so a backend header can include these without pulling in the
// backend-selection logic that in turn includes the backend.

#include "../core/color.hpp"
#include "../core/cursor.hpp"   // CursorShape: core vocabulary, re-exported here
#include "../core/geometry.hpp"

#include <chrono>
#include <cstdint>
#include <string>

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
    /// A secondary window of an app that already has one: no app-level setup
    /// (activation, launch handshake), placed near the main window.
    bool        child       = false;
    /// macOS: ⌥ is Alt/Meta for the app (⌥b = Alt+b) instead of typing
    /// characters (⌥e ´ then e = é, ⌥2 = ™). Off is the Mac convention;
    /// terminals offer it for readline/emacs. ⌥ with keys that type nothing
    /// (arrows, Delete) is Alt either way.
    bool        option_as_alt = false;
};

}  // namespace mayag::platform
