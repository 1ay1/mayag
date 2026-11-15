#pragma once
// mayag::widget split — resizable panes
//
//   h(left | width(m.sidebar_w), split_handle(c, "split-left", Axis::horizontal), editor)
//
// A thin drag handle between two panes. The width it controls is the app's
// (a float in the model); the handle draws, highlights under the pointer and
// reports drags.
//
// Interaction:
//   Dragged on node_id(id) -> m.sidebar_w = clamp(m.sidebar_w + d.delta.x, lo, hi)
//   Clicked with count 2   -> reset to the default width

#include "control.hpp"

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline Node split_handle(const C& c, std::string_view id, Axis axis = Axis::horizontal) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool hot = c.hovered(nid) || c.pressed(nid);
    const bool horiz = axis == Axis::horizontal;
    auto line = box() | bg(hot ? t.accent : t.border);
    if (horiz)
        return (h(node((line | width(hot ? 2.0f : 1.0f) | height(pct(100))).build())) | width(5) | height(pct(100)) | center
                | cursor(CursorShape::resize_h) | role(a11y::Role::separator) | id_of(id)).build();
    return (v(node((line | height(hot ? 2.0f : 1.0f) | width(pct(100))).build())) | height(5) | width(pct(100)) | center
            | cursor(CursorShape::resize_v) | role(a11y::Role::separator) | id_of(id)).build();
}

}  // namespace mayag::dsl
