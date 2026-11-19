#pragma once
// mayag::widget value_slider — a settings knob
//
//   value_slider(c, "font.size", f, {.width = 200, .origin = 0, .reset = def},
//                [](float f) { return SetSize{f}; })
//
// What a settings panel wants from a slider and a bare `slider` lacks:
//
//   * the fill starts at the value's ORIGIN: 0 for a size, the centre for a
//     signed knob (-0.12 .. +0.12), so "how far from neutral" is what you see
//   * a tick at the DEFAULT, so you can always find your way back -- and a
//     double click puts the knob there
//   * step ticks for small integer ranges (an ANSI slot, a tab count)
//   * the whole 24 px row is the hit target, the knob grows under the
//     pointer and while held (Sheet::slider_knob's group states)
//
// Look: Sheet::slider_track / slider_fill / slider_knob, like `slider`.
// Interaction is attached: `set(fraction)` is the message for a new value
// (press, drag, and double click with the default), so the app needs no
// on(Pressed) / on(Dragged) branch.

#include "control.hpp"
#include "sheet.hpp"
#include "../app/handlers.hpp"

#include <algorithm>
#include <concepts>
#include <string_view>
#include <vector>

namespace mayag::dsl {

struct ValueSliderProps {
    float width  = 200.0f;
    float origin = 0.0f;    ///< where the fill starts, 0..1 (0.5 for a signed range)
    float reset  = -1.0f;   ///< the default, 0..1; <0: none (no tick, no double-click reset)
    int   steps  = 0;       ///< >1: draw that many step ticks (small integer ranges)
};

template <ControlContext C, class Set>
    requires std::invocable<const Set&, float>
[[nodiscard]] inline Node value_slider(const C& c, std::string_view id, float fraction, const ValueSliderProps& p,
                                       Set set) {
    const Theme& t = c.theme;
    const Sheet& s = sheet_of(t);
    constexpr float row = 24.0f, bar = 4.0f, knob = 14.0f;
    const float w = p.width;
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    const float o = std::clamp(p.origin, 0.0f, 1.0f);
    const float x = f * w, ox = o * w;
    const float y = (row - bar) * 0.5f;

    std::vector<Node> layers;
    layers.push_back((box() | size(w, bar) | absolute(0, y) | with_look(s.slider_track)).build());
    for (int i = 0; p.steps > 1 && i < p.steps; ++i) {
        const float tx = w * static_cast<float>(i) / static_cast<float>(p.steps - 1);
        layers.push_back((box() | size(1.0f, 6.0f) | absolute(std::clamp(tx - 0.5f, 0.0f, w - 1.0f), y + bar + 2.0f)
                          | bg(var::border_strong)).build());
    }
    layers.push_back((box() | size(std::max(std::abs(x - ox), 0.0f), bar) | absolute(std::min(x, ox), y)
                      | with_look(s.slider_fill)).build());
    if (p.reset >= 0.0f) {
        const float dx = std::clamp(p.reset, 0.0f, 1.0f) * w;
        layers.push_back((box() | size(2.0f, 10.0f) | radius(1.0f) | absolute(std::clamp(dx - 1.0f, 0.0f, w - 2.0f), y - 3.0f)
                          | bg(var::text_disabled)).build());
    }
    layers.push_back((box() | size(knob, knob) | absolute(x - knob * 0.5f, (row - knob) * 0.5f)
                      | with_look(s.slider_knob)).build());

    using M = std::invoke_result_t<const Set&, float>;
    const float reset = p.reset;
    return (z(std::move(layers)) | size(w, row) | group() | with_look(no_look(), t)
            | pointer_cursor | role(a11y::Role::slider) | a11y_value(f) | label(id) | id_of(id)
            | on_slide([set, w](Vec2 local) { return set(std::clamp(local.x / w, 0.0f, 1.0f)); })
            | on_click([set, reset](const Clicked& k) -> std::optional<M> {
                  if (k.count != 2 || reset < 0.0f) return std::nullopt;
                  return set(reset);
              })).build();
}

}  // namespace mayag::dsl
