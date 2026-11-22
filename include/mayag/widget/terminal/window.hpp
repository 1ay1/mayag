#pragma once
// mayag::widget terminal/window — a terminal's window: the tab strip and the
// terminal, and how they move
//
//   tabbed_window(c, props, strip, body)
//
// The strip (any node: tab_strip / tab_line) eases in from its edge as the
// second tab opens and the grid gives up exactly the rows it takes; on a tab
// switch the body slides in from the side the new tab came from.

#include "../control.hpp"
#include "../tab_strip.hpp"   // window_controls

#include <algorithm>
#include <string>
#include <vector>

namespace mayag::dsl {

/// A window with no title bar and no tab strip still needs its buttons:
/// three small cells floating in the top-right corner over the content --
/// quiet until the pointer is on them, then full strength (close in danger).
/// Interaction: Clicked on part_id(id, tab_part_window + 0/1/2).
struct CornerControlsProps {
    float width = 0.0f;         ///< the window's width (they sit flush in its corner)
    float right_inset = 0.0f;   ///< what owns the right edge (a scrollbar / minimap): they sit left of it
    float cell = 18.0f;         ///< each button's height; width is 1.4x
};

/// The three buttons' box, for hit-testing by the app.
[[nodiscard]] inline Vec2 corner_controls_size(const CornerControlsProps& p) {
    return {3.0f * std::round(p.cell * 1.4f), p.cell};
}

template <ControlContext C>
inline void corner_controls(const C& c, std::string_view id, const CornerControlsProps& p) {
    const Theme& t = c.theme;
    bool any = false;
    for (std::size_t k = 0; k < 3; ++k) any = any || c.hovered(part_id(id, tab_part_window + k));
    const float w = std::round(p.cell * 1.4f), fs = std::round(p.cell * 0.62f);
    std::vector<Node> cells;
    static constexpr const char* glyphs[] = {"\u2212", "\u25A1", "\u00D7"};
    static constexpr const char* names[] = {"minimise", "maximise", "close window"};
    for (std::size_t k = 0; k < 3; ++k) {
        const auto pid = part_id(id, tab_part_window + k);
        const bool hov = c.hovered(pid), close = k == 2;
        cells.push_back((h(text_owned(glyphs[k]) | glyph_size(close ? fs * 1.15f : fs) | monospace
                           | fg(hov ? (close ? colors::white : t.text_primary) : t.text_secondary))
                         | size(w, p.cell) | center
                         | bg(hov ? (close ? t.danger : mix(t.background, t.text_primary, 0.14f)) : t.background.fade(0.0f))
                         | pointer_cursor | label(names[k]) | id_of(id, tab_part_window + k)).build());
    }
    const float total = 3.0f * w;
    c.overlay(Overlay{
        .content = (h_list(std::move(cells)) | size(total, p.cell)
                    | bg(any ? mix(t.background, t.text_primary, 0.05f) : t.background.fade(0.0f))
                    | opacity(any ? 1.0f : 0.45f)
                    | role(a11y::Role::group) | label("window") | id_of(id)).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::cursor,
        .dismiss = Dismiss::never,
        .offset = 0.0f,
        .layer = 30,
        .at = Vec2{std::round(p.width - p.right_inset - total), 0.0f},
    });
}

struct TabbedWindowProps {
    Vec2  size{};               ///< the whole window
    float strip_full = 26.0f;   ///< the strip's height when shown
    float strip_t = 1.0f;       ///< 0..1: how far it has eased in (0 = hidden: no strip at all)
    bool  bottom = false;       ///< the strip under the terminal
    float switch_t = 1.0f;      ///< 0..1: a tab switch's slide
    int   switch_dir = 0;       ///< -1 from the right, +1 from the left
};

template <ControlContext C>
[[nodiscard]] inline Node tabbed_window(const C&, const TabbedWindowProps& p, Node strip, Node body) {
    if (p.switch_t < 1.0f) {
        const float e = 1.0f - (1.0f - p.switch_t) * (1.0f - p.switch_t) * (1.0f - p.switch_t);   // ease-out cubic
        body = (node(std::move(body)) | translate(static_cast<float>(p.switch_dir) * (1.0f - e) * 42.0f, 0.0f)
                | opacity(0.35f + 0.65f * e)).build();
    }
    const float sh = std::round(p.strip_full * std::clamp(p.strip_t, 0.0f, 1.0f));
    if (sh < 0.5f) return body;
    strip = (node(std::move(strip)) | translate(0.0f, p.bottom ? 0.0f : sh - p.strip_full)
             | opacity(std::clamp(p.strip_t * 1.4f, 0.0f, 1.0f))).build();
    Node bar = (v(std::move(strip)) | height(sh) | width(pct(100)) | clip).build();
    if (p.bottom) return (v(std::move(body), std::move(bar)) | size(p.size.x, p.size.y)).build();
    return (v(std::move(bar), std::move(body)) | size(p.size.x, p.size.y)).build();
}

}  // namespace mayag::dsl
