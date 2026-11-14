#pragma once
// mayag::widget color_swatch — a pickable colour chip
//
// Interaction: Clicked / Activated on part_id(base, index) -> select it.
// The selected ring is drawn OUTSIDE the swatch so the colour itself is
// never covered by the indicator.

#include "control.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline auto swatch(const C& c, std::string_view base, std::size_t index,
                                 Color<Srgb> colour, bool selected_now, float d = 28.0f) {
    const Theme& t = c.theme;
    const auto pid = part_id(base, index);
    const bool hov = c.hovered(pid);
    return box() | size(d, d)
                 | bg(colour)
                 | border(selected_now ? 2.5f : (hov ? 2.0f : 1.0f),
                          selected_now ? t.text_primary
                                       : (c.focused(pid) ? t.focus_ring
                                                         : (hov ? t.border_strong : t.border)),
                          selected_now ? StrokeAlign::outside : StrokeAlign::inside)
                 | radius(t.radius_small)
                 | pointer_cursor
                 | role(a11y::Role::radio) | checked(selected_now)
                 | id_of(base, index);
}

/// `#RRGGBB` for a colour, the spelling every config file uses.
[[nodiscard]] inline std::string hex(Color<Srgb> c) {
    const auto b = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", b(c.c0), b(c.c1), b(c.c2));
    return buf;
}

}  // namespace mayag::dsl
