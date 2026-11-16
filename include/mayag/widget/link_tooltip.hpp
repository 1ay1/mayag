#pragma once
// mayag::widget link_tooltip — where a hovered hyperlink goes
//
//   link_tooltip(c, "link", mouse_pos, "https://example.com/docs", "\u2318-click to open")
//
// OSC 8 links hide their target behind text; the tooltip shows it before the
// user opens it (the same reason browsers show a status-bar URL).

#include "control.hpp"
#include "../scene/overlay.hpp"

#include <string>

namespace mayag::dsl {

template <ControlContext C>
inline void link_tooltip(const C& c, std::string_view id, Vec2 at, std::string_view url, std::string_view hint = {}) {
    const Theme& t = c.theme;
    c.overlay(Overlay{
        .content = (h(text_owned(std::string{url}) | font(t.font_size * 0.88f) | fg(var::info) | ellipsis | max_size(520, 1e4f),
                      text_owned(std::string{hint}) | font(t.font_size * 0.8f) | fg(var::text_disabled))
                    | gap(t.spaced(10)) | align(Align::center) | pad(t.spaced(5), t.spaced(9))
                    | bg(var::overlay) | border(t.stroke(1), t.border_strong) | radius(t.radius_small) | elevation(t.lift(10))).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::cursor,
        .dismiss = Dismiss::never,
        .layer = 12,
        .at = Vec2{at.x + 12.0f, at.y + 18.0f},
    });
}

}  // namespace mayag::dsl
