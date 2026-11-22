#pragma once
// mayag::widget stepper — a number changed one unit at a time
//
// Interaction: Clicked on part_id(id, part_dec) / part_id(id, part_inc).
// `at_min` / `at_max` dim the button that would do nothing.

#include "control.hpp"

#include <string>
#include <string_view>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline auto stepper(const C& c, std::string_view id, std::string value,
                                  bool at_min = false, bool at_max = false) {
    const Theme& t = c.theme;
    const auto button_part = [&](std::size_t part, const char* glyph, bool off) {
        const auto pid = part_id(id, part);
        const bool hov = !off && c.hovered(pid);
        return (h(text_owned(glyph) | font(t.type(Tier::large))
                    | fg(off ? t.text_disabled : t.text_primary))
                | size(28, 28) | center
                | bg(c.pressed(pid) && !off ? t.surface_raised
                                            : (hov ? t.surface_raised.fade(0.6f) : t.surface.fade(0.0f)))
                | border(t.stroke(1), c.focused(pid) ? t.focus_ring : t.border.fade(0.0f))
                | radius(t.radius_small - 2.0f)
                | cursor(off ? CursorShape::arrow : CursorShape::pointer)
                | role(a11y::Role::button) | disabled(off)
                | label(part == part_dec ? "decrease" : "increase")
                | id_of(id, part)).build();
    };
    return h(node(button_part(part_dec, "\u2212", at_min)),
             text_owned(std::move(value)) | font(t.type(Tier::body)) | semibold | fg(var::text_primary)
                 | text_align(TextAlign::center) | width(46),
             node(button_part(part_inc, "+", at_max)))
         | align(Align::center) | pad(t.spaced(2))
         | bg(var::surface) | border(t.stroke(1), t.border) | radius(t.radius_small);
}

}  // namespace mayag::dsl
