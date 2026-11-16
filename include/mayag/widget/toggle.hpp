#pragma once
// mayag::widget toggle — on/off switches
//
//   toggle(t, on)                 a drawn switch, no interaction of its own
//   switch_control(c, "id", on)   the interactive one: hover, press, focus,
//                                 a11y checkbox role, named node
//
// switch_control interaction: Clicked / Activated on node_id(id) -> flip.

#include "control.hpp"
#include "sheet.hpp"

#include <string_view>

namespace mayag::dsl {

/// A toggle switch, drawn declaratively: the knob is an absolutely positioned
/// child whose x offset is the animation parameter.
[[nodiscard]] constexpr auto toggle(const Theme& t, bool on, float w = 40.0f, float h = 22.0f) {
    const float knob = h - 6.0f;
    const float x    = on ? (w - knob - 3.0f) : 3.0f;
    return z(box() | size(knob, knob)
                   | bg(on ? t.on_accent : t.text_secondary)
                   | radius(t.capsule(knob))
                   | absolute(x, 3.0f)
                   | elevation(t.lift(2.0f)))
         | size(w, h)
         | bg(on ? t.accent : t.surface_raised)
         | border(t.stroke(1), on ? t.accent : t.border_strong)
         | radius(t.capsule(h));
}

/// An interactive on/off switch. Its look -- including hover, press,
/// focus and how the knob moves -- is the theme's (Sheet::track / knob);
/// the knob reads `checked` through the group and slides on a transform, so
/// it springs with the theme's motion instead of jumping.
template <ControlContext C>
[[nodiscard]] inline auto switch_control(const C& c, std::string_view id, bool on) {
    const Sheet& s = sheet_of(c.theme);
    constexpr float w = 38.0f, hgt = 22.0f, knob = 16.0f, inset = 3.0f;
    return z(box() | size(knob, knob) | absolute(inset, inset)
                   | with_look(s.knob)
                   | when(state::group(state::checked), translate(w - knob - 2.0f * inset, 0.0f)))
         | size(w, hgt) | group()
         | with_look(s.track, c.theme)
         | pointer_cursor
         | role(a11y::Role::checkbox) | checked(on) | label(id)
         | id_of(id);
}

}  // namespace mayag::dsl
