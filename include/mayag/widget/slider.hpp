#pragma once
// mayag::widget slider — a continuous value on a track
//
// `fraction` is 0..1; the program owns the real range and units.
// Interaction: Pressed / Dragged on node_id(id) carry `local.x`; the value
// is `clamp(local.x / track_w, 0, 1)` (pointer capture keeps a drag
// reporting after it leaves the track). Focused + Left/Right should nudge it.
// The hit target is the full 24 px row, not the 4 px track: a slider you
// have to aim at is a slider nobody can use.
//
// Look: Sheet::slider_track / slider_fill / slider_knob. The knob responds
// to the ROW's hover, press and keyboard focus through the group, so the
// whole look -- including how it reacts -- belongs to the theme.

#include "control.hpp"
#include "sheet.hpp"

#include <algorithm>
#include <string_view>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline auto slider(const C& c, std::string_view id, float fraction, float track_w) {
    const Sheet& s = sheet_of(c.theme);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    constexpr float knob = 16.0f, row = 24.0f, bar = 4.0f;
    const float x = f * track_w;
    return z(box() | size(track_w, bar) | absolute(0, (row - bar) * 0.5f) | with_look(s.slider_track),
             box() | size(std::max(x, 0.0f), bar) | absolute(0, (row - bar) * 0.5f) | with_look(s.slider_fill),
             box() | size(knob, knob) | absolute(x - knob * 0.5f, (row - knob) * 0.5f)
                   | with_look(s.slider_knob))
         | size(track_w, row) | group() | with_look(no_look(), c.theme)
         | pointer_cursor
         | role(a11y::Role::slider) | a11y_value(f) | label(id)
         | id_of(id);
}

}  // namespace mayag::dsl
