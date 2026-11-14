#pragma once
// mayag::widget toggle — on/off switches
//
//   toggle(t, on)                 a drawn switch, no interaction of its own
//   switch_control(c, "id", on)   the interactive one: hover, press, focus,
//                                 a11y checkbox role, named node
//
// switch_control interaction: Clicked / Activated on node_id(id) -> flip.

#include "control.hpp"

#include <string_view>

namespace mayag::dsl {

/// A toggle switch, drawn declaratively: the knob is an absolutely positioned
/// child whose x offset is the animation parameter.
[[nodiscard]] constexpr auto toggle(const Theme& t, bool on, float w = 40.0f, float h = 22.0f) {
    const float knob = h - 6.0f;
    const float x    = on ? (w - knob - 3.0f) : 3.0f;
    return z(box() | size(knob, knob)
                   | bg(on ? t.on_accent : t.text_secondary)
                   | radius(knob * 0.5f)
                   | absolute(x, 3.0f)
                   | elevation(2.0f))
         | size(w, h)
         | bg(on ? t.accent : t.surface_raised)
         | border(1, on ? t.accent : t.border_strong)
         | radius(h * 0.5f);
}

template <ControlContext C>
[[nodiscard]] inline auto switch_control(const C& c, std::string_view id, bool on) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    constexpr float w = 38.0f, hgt = 22.0f, knob = 16.0f;
    const bool hov = c.hovered(nid);
    return z(box() | size(knob, knob)
                   | bg(on ? t.on_accent : (hov ? t.text_primary : t.text_secondary))
                   | radius(knob * 0.5f)
                   | absolute(on ? w - knob - 3.0f : 3.0f, 3.0f)
                   | elevation(1.5f))
         | size(w, hgt)
         | bg(on ? (c.pressed(nid) ? t.accent_active : hov ? t.accent_hover : t.accent)
                 : t.surface_raised)
         | border(1.5f, detail::edge(t, c.focused(nid), hov, on ? t.accent : t.border_strong))
         | radius(hgt * 0.5f)
         | pointer_cursor
         | role(a11y::Role::checkbox) | checked(on) | label(id)
         | id_of(id);
}

}  // namespace mayag::dsl
