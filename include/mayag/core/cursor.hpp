#pragma once
// mayag/core/cursor.hpp — the pointer shapes a node can ask for.
//
// Core vocabulary: Style carries one (`| cursor(...)`) and every platform
// maps it to its own cursor. Kept here, below both, so neither has to
// include the other.

namespace mayag {

/// Standard cursor shapes.
enum class CursorShape : int {
    arrow = 0, text = 1, pointer = 2, crosshair = 3,
    resize_h = 4, resize_v = 5, resize_nwse = 6, resize_nesw = 7,
    grab = 8, grabbing = 9, not_allowed = 10, wait = 11,
};

}  // namespace mayag
