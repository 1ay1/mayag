#pragma once
// mayag::ScrollState — scrolling as plain state
//
// A scroll position is APPLICATION STATE, not a hidden property of a widget.
// It lives in your Model, it is a value you can save, restore, animate, and
// assert on in a test, and `update()` changes it like anything else. That is
// the whole design: mayag never owns a scroll offset behind your back.
//
//     struct Model { ScrollState list; };
//
//     // view
//     scroll(m.list, v(rows...)) | size(300, 400) | id<"list">
//
//     // subscribe
//     Sub<Msg>::on_scroll<"list">([](Vec2 d) { return Scrolled{d}; })
//
//     // update
//     m.list.scroll_by(e.delta);
//
// The runtime clamps against content bounds after layout, so you can never
// scroll into emptiness, and momentum/overscroll are opt-in rather than
// surprises.

#include "../core/geometry.hpp"

#include <cstdint>

namespace mayag {

/// Which axes a region may scroll along.
enum class ScrollAxis : std::uint8_t { vertical, horizontal, both };

struct ScrollState {
    /// Current offset, in content pixels. Positive means the content has
    /// moved UP/LEFT — i.e. you are looking further down/right.
    Vec2 offset{};

    // ── measurements, written by layout ─────────────────────────────────
    //
    // `mutable` because `view()` is a PURE function of the model and takes it
    // by const reference — but layout genuinely learns these during the pass
    // and the state is the only sensible place to record them.
    //
    // This is safe precisely because they are DERIVED, not authoritative: they
    // are recomputed from scratch every layout, so nothing observable depends
    // on their previous value. The offset above, which IS authoritative, stays
    // non-mutable and can only be changed by update().

    /// How far the content extends past the viewport. Zero when everything
    /// fits, which is also how a view decides whether to draw a bar at all.
    mutable Vec2 max_offset{};

    /// Viewport and content extents, for scrollbar geometry and page
    /// arithmetic.
    mutable Vec2 viewport{};
    mutable Vec2 content{};

    mutable ScrollAxis axis = ScrollAxis::vertical;

    /// Pixels per wheel/trackpad unit. Trackpads already report pixels, so
    /// this is 1 by default; a line-based wheel is normalised in the platform
    /// layer rather than here.
    float speed = 1.0f;

    friend constexpr bool operator==(const ScrollState&, const ScrollState&) = default;

    // ── queries ─────────────────────────────────────────────────────────

    [[nodiscard]] constexpr bool scrollable_x() const noexcept {
        return axis != ScrollAxis::vertical && max_offset.x > 0.5f;
    }
    [[nodiscard]] constexpr bool scrollable_y() const noexcept {
        return axis != ScrollAxis::horizontal && max_offset.y > 0.5f;
    }
    [[nodiscard]] constexpr bool at_top() const noexcept { return offset.y <= 0.5f; }
    [[nodiscard]] constexpr bool at_bottom() const noexcept {
        return offset.y >= max_offset.y - 0.5f;
    }

    /// Fraction scrolled, 0..1. What a scrollbar thumb's position is.
    [[nodiscard]] constexpr Vec2 progress() const noexcept {
        return {max_offset.x > 0.0f ? num::saturate(offset.x / max_offset.x) : 0.0f,
                max_offset.y > 0.0f ? num::saturate(offset.y / max_offset.y) : 0.0f};
    }

    /// Visible fraction of the content, 0..1. What a scrollbar thumb's SIZE
    /// is — a long document gets a short thumb, which is the only honest
    /// signal of how much is off-screen.
    [[nodiscard]] constexpr Vec2 visible_fraction() const noexcept {
        return {content.x > 0.0f ? num::saturate(viewport.x / content.x) : 1.0f,
                content.y > 0.0f ? num::saturate(viewport.y / content.y) : 1.0f};
    }

    // ── mutation ────────────────────────────────────────────────────────

    /// Scroll by a delta, clamped. This is what a wheel event calls.
    ///
    /// The sign convention matches every platform: a downward wheel gesture
    /// reveals content BELOW, so it increases `offset.y`.
    constexpr void scroll_by(Vec2 delta) noexcept {
        offset.x = num::clamp(offset.x - delta.x * speed, 0.0f, max_offset.x);
        offset.y = num::clamp(offset.y - delta.y * speed, 0.0f, max_offset.y);
    }

    constexpr void scroll_to(Vec2 position) noexcept {
        offset.x = num::clamp(position.x, 0.0f, max_offset.x);
        offset.y = num::clamp(position.y, 0.0f, max_offset.y);
    }

    constexpr void scroll_to_top() noexcept { offset = Vec2{}; }
    constexpr void scroll_to_bottom() noexcept { offset = max_offset; }

    /// Page up/down, keeping a line of overlap so the reader has context.
    /// Jumping a clean viewport height loses the line you were reading.
    constexpr void page(float direction, float overlap = 24.0f) noexcept {
        const float step = num::max(viewport.y - overlap, viewport.y * 0.5f);
        scroll_to({offset.x, offset.y + step * direction});
    }

    /// Bring a rect (in CONTENT coordinates) into view with minimal movement.
    ///
    /// Minimal is the important part: scrolling something already visible to
    /// the centre is disorienting, which is why `scrollIntoView` feels wrong
    /// on the web when misused.
    constexpr void reveal(const Rect& r, float margin = 8.0f) noexcept {
        if (r.top() - margin < offset.y) {
            offset.y = num::max(r.top() - margin, 0.0f);
        } else if (r.bottom() + margin > offset.y + viewport.y) {
            offset.y = num::min(r.bottom() + margin - viewport.y, max_offset.y);
        }

        if (r.left() - margin < offset.x) {
            offset.x = num::max(r.left() - margin, 0.0f);
        } else if (r.right() + margin > offset.x + viewport.x) {
            offset.x = num::min(r.right() + margin - viewport.x, max_offset.x);
        }
    }

    /// Recompute the limits after layout has measured the content.
    ///
    /// Called by the layout pass; clamps the offset too, so shrinking the
    /// content (deleting rows, collapsing a section) can never leave the view
    /// scrolled past the end showing blank space.
    constexpr void measured(Vec2 viewport_size, Vec2 content_size) const noexcept {
        viewport = viewport_size;
        content  = content_size;
        max_offset = {num::max(content_size.x - viewport_size.x, 0.0f),
                      num::max(content_size.y - viewport_size.y, 0.0f)};
        // Clamping the offset is the one write to authoritative state, and it
        // is idempotent: it can only pull an out-of-range offset back into
        // range, which shrinking content must do or the view shows blank space.
        auto& mutable_offset = const_cast<Vec2&>(offset);
        mutable_offset.x = num::clamp(offset.x, 0.0f, max_offset.x);
        mutable_offset.y = num::clamp(offset.y, 0.0f, max_offset.y);
    }

    // ── scrollbar geometry ──────────────────────────────────────────────

    /// Thumb rect for a vertical scrollbar occupying `track`.
    ///
    /// Returns an empty rect when nothing is scrollable, so a view can say
    /// `when(state.scrollable_y(), scrollbar(...))` and get the right answer
    /// without duplicating the test.
    [[nodiscard]] constexpr Rect thumb_v(const Rect& track, float min_length = 24.0f) const noexcept {
        if (!scrollable_y()) return {};
        const float frac = visible_fraction().y;
        const float len  = num::max(track.height() * frac, min_length);
        const float span = track.height() - len;
        return Rect{track.left(), track.top() + span * progress().y, track.width(), len};
    }

    [[nodiscard]] constexpr Rect thumb_h(const Rect& track, float min_length = 24.0f) const noexcept {
        if (!scrollable_x()) return {};
        const float frac = visible_fraction().x;
        const float len  = num::max(track.width() * frac, min_length);
        const float span = track.width() - len;
        return Rect{track.left() + span * progress().x, track.top(), len, track.height()};
    }

    /// Convert a drag on the scrollbar track into an offset.
    constexpr void drag_thumb_v(float track_top, float track_height, float pointer_y) noexcept {
        const float len  = num::max(track_height * visible_fraction().y, 24.0f);
        const float span = track_height - len;
        if (span <= 0.0f) return;
        const float t = num::saturate((pointer_y - track_top - len * 0.5f) / span);
        offset.y = t * max_offset.y;
    }
};

}  // namespace mayag
