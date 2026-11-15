#pragma once
// mayag::widget pane — terminal splits
//
//   pane_split(c, "split-1", Axis::horizontal, m.ratio, left_pane, right_pane)
//   pane_frame(c, "pane-2", {.title = "cargo watch", .focused = false}, content)
//
// The split TREE is the app's data (a vector of nodes, or a recursive
// variant); these two widgets draw one level of it. pane_split lays two
// children out at `ratio` with a draggable divider; pane_frame gives a pane
// its optional title strip, focus ring, zoom badge and a "process exited"
// footer that offers to restart.
//
// Interaction:
//   Dragged on part_id(id, part_bar)       -> ratio += delta / extent
//   Clicked on part_id(id, part_bar) x2    -> ratio = 0.5
//   pane_frame: Pressed on node_id(id)      -> focus this pane
//               Clicked on part_id(id, part_inc) -> restart (when exited)
//               Clicked on part_id(id, pane_part_close) -> close pane

#include "control.hpp"
#include "dot.hpp"
#include "scroll_view.hpp"   // part_bar

#include <string>

namespace mayag::dsl {

inline constexpr std::size_t pane_part_close = part_dec + 5;

struct PaneInfo {
    std::string title;
    bool        focused = false;
    bool        zoomed = false;
    bool        show_title = true;      ///< title strip (off when the tab has one pane)
    bool        exited = false;
    int         exit_code = 0;
    bool        broadcast = false;      ///< input is being sent to every pane
};

template <ControlContext C>
[[nodiscard]] inline Node pane_frame(const C& c, std::string_view id, const PaneInfo& p, Node content) {
    const Theme& t = c.theme;
    Node strip = p.show_title
        ? (h(node(dot(p.focused ? t.accent : t.text_disabled, 6).build()),
             text_owned(p.title) | font(t.font_size * 0.88f) | fg(p.focused ? t.text_primary : t.text_secondary)
                 | ellipsis | grow() | min_size(0, 0),
             text_owned(p.broadcast ? "BROADCAST" : "") | font(t.font_size * 0.72f) | bold | tracking(0.6f) | fg(t.danger),
             text_owned(p.zoomed ? "ZOOMED" : "") | font(t.font_size * 0.72f) | bold | tracking(0.6f) | fg(t.warning),
             h(text<"\u00D7"> | font(t.font_size) | fg(c.hovered(part_id(id, pane_part_close)) ? t.text_primary : t.text_disabled))
                 | size(18, 18) | center | pointer_cursor | id_of(id, pane_part_close))
           | gap(7) | align(Align::center) | pad(0, 8) | height(24) | width(pct(100))
           | bg(p.focused ? t.surface_raised : t.surface)).build()
        : Node{};
    Node footer = p.exited
        ? (h(text_owned(p.exit_code == 0 ? "[process completed]" : "[process exited with code " + std::to_string(p.exit_code) + "]")
                 | font(t.font_size * 0.88f) | fg(p.exit_code == 0 ? t.text_secondary : t.danger),
             spacer(),
             h(text<"Restart  \u23CE"> | font(t.font_size * 0.85f) | fg(t.on_accent)) | pad(3, 10) | radius(4)
                 | bg(c.hovered(part_id(id, part_inc)) ? t.accent_hover : t.accent) | pointer_cursor | id_of(id, part_inc))
           | align(Align::center) | pad(6, 12) | width(pct(100)) | bg(t.surface)).build()
        : Node{};
    return (v(node(std::move(strip)), v(node(std::move(content))) | grow() | width(pct(100)) | min_size(0, 0), node(std::move(footer)))
            | grow() | width(pct(100)) | height(pct(100)) | min_size(0, 0)
            | border(p.focused && p.show_title ? 1.0f : 0.0f, t.accent.fade(0.6f)) | clip
            | id_of(id)).build();
}

template <ControlContext C>
[[nodiscard]] inline Node pane_split(const C& c, std::string_view id, Axis axis, float ratio, Node first, Node second) {
    const Theme& t = c.theme;
    const auto did = part_id(id, part_bar);
    const bool hot = c.hovered(did) || c.pressed(did);
    const float r = std::clamp(ratio, 0.05f, 0.95f);
    const bool horiz = axis == Axis::horizontal;
    Node divider = horiz
        ? (h(node((box() | width(hot ? 2.0f : 1.0f) | height(pct(100)) | bg(hot ? t.accent : t.border)).build()))
           | width(5) | height(pct(100)) | center | cursor(CursorShape::resize_h) | role(a11y::Role::separator) | id_of(id, part_bar)).build()
        : (v(node((box() | height(hot ? 2.0f : 1.0f) | width(pct(100)) | bg(hot ? t.accent : t.border)).build()))
           | height(5) | width(pct(100)) | center | cursor(CursorShape::resize_v) | role(a11y::Role::separator) | id_of(id, part_bar)).build();
    // Grow factors carry the ratio, so the split scales with the window.
    // Wrapped, not piped: piping onto node(x) would replace x's own style.
    auto a = v(node(std::move(first))) | grow(r * 100.0f) | min_size(0, 0);
    auto b = v(node(std::move(second))) | grow((1.0f - r) * 100.0f) | min_size(0, 0);
    if (horiz) return (h(a | width(0) | height(pct(100)), node(std::move(divider)), b | width(0) | height(pct(100)))
                       | grow() | width(pct(100)) | height(pct(100)) | min_size(0, 0) | id_of(id)).build();
    return (v(a | height(0) | width(pct(100)), node(std::move(divider)), b | height(0) | width(pct(100)))
            | grow() | width(pct(100)) | height(pct(100)) | min_size(0, 0) | id_of(id)).build();
}

}  // namespace mayag::dsl
