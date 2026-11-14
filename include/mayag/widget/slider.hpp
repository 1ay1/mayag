#pragma once
// mayag::widget slider — a continuous value on a track
//
// `fraction` is 0..1; the program owns the real range and units.
// Interaction: Pressed / Dragged on node_id(id) carry `local.x`; the value
// is `clamp(local.x / track_w, 0, 1)` (pointer capture keeps a drag
// reporting after it leaves the track). Focused + Left/Right should nudge it.
// The hit target is the full 24 px row, not the 4 px track: a slider you
// have to aim at is a slider nobody can use.

#include "control.hpp"

#include <algorithm>
#include <string_view>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline auto slider(const C& c, std::string_view id, float fraction, float track_w) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    const bool hov = c.hovered(nid), held = c.pressed(nid), foc = c.focused(nid);
    const float knob = held ? 18.0f : 16.0f;
    const float x = f * track_w;
    return z(box() | size(track_w, 4) | bg(t.surface_raised) | radius(2) | absolute(0, 10),
             box() | size(std::max(x, 0.0f), 4) | bg(t.accent) | radius(2) | absolute(0, 10),
             box() | size(knob, knob)
                   | bg(t.text_primary)
                   | border(foc ? 3.0f : (hov || held ? 2.0f : 0.0f), foc ? t.focus_ring : t.accent)
                   | radius(knob * 0.5f)
                   | elevation(held ? 3.0f : 1.5f)
                   | absolute(x - knob * 0.5f, 12.0f - knob * 0.5f))
         | size(track_w, 24)
         | pointer_cursor
         | role(a11y::Role::slider) | a11y_value(f) | label(id)
         | id_of(id);
}

}  // namespace mayag::dsl
