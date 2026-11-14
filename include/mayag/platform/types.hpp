#pragma once
// mayag::platform types shared by every backend and by the runtime.
// Split out so a backend header can include these without pulling in the
// backend-selection logic that in turn includes the backend.

#include "../core/color.hpp"
#include "../core/geometry.hpp"

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
