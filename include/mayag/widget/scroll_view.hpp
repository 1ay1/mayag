#pragma once
// mayag::widget scroll_view — a scrolling region with a real scrollbar
//
//   scroll_view(c, "content", m.scroll, content)
//
// The bar is a flow column beside the viewport, so it never depends on the
// content's padding or the caller's coordinates: the viewport and the track
// are the same height by construction. Put padding on the CONTENT, not on
// the scroll_view, and the numbers line up.
//
// It shows whenever there is something to scroll (a bar that only appears on
// hover is a bar new users never find), widens under the pointer, and can be
// dragged or clicked.
//
// Interaction (all on part_id(id, part_bar)):
//   Pressed  -> scrollbar_press(m.scroll, p.local.y)   click above/below the
//               thumb jumps there; pressing ON the thumb grabs it in place
//   Dragged  -> scrollbar_drag(m.scroll, d.delta.y)    the thumb follows the
//               pointer 1:1
// Wheel and trackpad scrolling arrive as Scrolled on node_id(id), as for any
// `| scroll(state)` node.

#include "control.hpp"
#include "../core/scroll_state.hpp"

#include <string_view>

namespace mayag::dsl {

inline constexpr std::size_t part_bar = part_dec + 3;

namespace detail {
inline constexpr float kBarWidth = 12.0f;   // hit target; the thumb is drawn thinner
inline constexpr float kBarInset = 3.0f;    // gap above/below the thumb's travel

[[nodiscard]] constexpr Rect bar_track(const ScrollState& s) noexcept {
    return Rect{0.0f, kBarInset, kBarWidth, num::max(s.viewport.y - 2.0f * kBarInset, 0.0f)};
}
}  // namespace detail

/// Pressed on the bar at `local_y` (px from the top of the bar).
inline void scrollbar_press(ScrollState& s, float local_y) noexcept {
    const Rect track = detail::bar_track(s);
    const Rect thumb = s.thumb_v(track);
    if (local_y >= thumb.top() && local_y <= thumb.bottom()) return;   // grabbed: drag moves it
    s.drag_thumb_v(track.top(), track.height(), local_y);              // jump, centred on the click
}

/// The pointer moved `dy` px while holding the bar.
inline void scrollbar_drag(ScrollState& s, float dy) noexcept {
    const Rect track = detail::bar_track(s);
    const float span = track.height() - s.thumb_v(track).height();
    if (span <= 0.0f || s.max_offset.y <= 0.0f) return;
    s.scroll_to(Vec2{s.offset.x, s.offset.y + dy * (s.max_offset.y / span)});
}

template <ControlContext C, Element Content>
[[nodiscard]] inline auto scroll_view(const C& c, std::string_view id, const ScrollState& state,
                                      Content content) {
    const Theme& t = c.theme;
    const auto bar_id = part_id(id, part_bar);
    const bool active = c.hovered(bar_id) || c.pressed(bar_id);
    const Rect thumb = state.thumb_v(detail::bar_track(state));
    const bool show = state.scrollable_y() && thumb.height() > 0.0f;
    const float w = active ? 8.0f : 6.0f;

    auto bar = z(box() | size(w, num::max(thumb.height(), 0.0f))
                       | bg(show ? (c.pressed(bar_id) ? t.text_secondary
                                    : active ? t.text_secondary.fade(0.85f)
                                             : t.text_disabled.fade(0.75f))
                                 : t.text_disabled.fade(0.0f))
                       | radius(w * 0.5f)
                       | absolute((detail::kBarWidth - w) * 0.5f, thumb.top()))
             | width(detail::kBarWidth) | height(pct(100))
             | bg(show && active ? t.text_disabled.fade(0.12f) : t.background.fade(0.0f))
             | radius(detail::kBarWidth * 0.5f)
             | role(a11y::Role::slider) | label("scroll")
             | a11y_value(state.max_offset.y > 0.0f ? state.offset.y / state.max_offset.y : 0.0f)
             | id_of(id, part_bar);

    return h(v(content) | scroll(state) | grow() | height(pct(100)) | min_size(0, 0) | id_of(id),
             std::move(bar))
         | gap(2) | width(pct(100)) | grow() | min_size(0, 0);
}

}  // namespace mayag::dsl
