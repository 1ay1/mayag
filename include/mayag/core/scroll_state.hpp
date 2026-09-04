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

    /// Current momentum, in content px/second. Nonzero after a fling; decays
    /// under friction in `step()`. This is authoritative state — it lives in
    /// the model, so a scroll in flight is saved, restored, and replayed like
    /// everything else.
    Vec2 velocity{};

    /// Rubber-band overscroll. When on, dragging or flinging past an edge
    /// pulls the content beyond its bounds with diminishing resistance, and it
    /// springs back on release — the iOS/macOS bounce. Off by default because
    /// a desktop list usually wants a hard stop; a touch surface wants this.
    bool overscroll = false;

    /// How far, in px, the content can be pulled past an edge at full pull.
    /// The resistance curve asymptotes here, so it is a soft ceiling, not a
    /// hard one.
    float overscroll_limit = 120.0f;

    friend constexpr bool operator==(const ScrollState&, const ScrollState&) = default;

    /// Is momentum still in flight, OR is the content held past an edge and
    /// owed a spring-back? Either needs frames.
    [[nodiscard]] constexpr bool coasting() const noexcept {
        return velocity.x != 0.0f || velocity.y != 0.0f ||
               overscroll_amount().x != 0.0f || overscroll_amount().y != 0.0f;
    }

    /// How far the offset is currently past an edge (signed: negative past the
    /// top/left, positive past the bottom/right). Zero when in bounds.
    [[nodiscard]] constexpr Vec2 overscroll_amount() const noexcept {
        return {offset.x < 0.0f ? offset.x : (offset.x > max_offset.x ? offset.x - max_offset.x : 0.0f),
                offset.y < 0.0f ? offset.y : (offset.y > max_offset.y ? offset.y - max_offset.y : 0.0f)};
    }

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
    ///
    /// With `overscroll` on, movement PAST an edge is rubber-banded: the
    /// further out you already are, the less each pixel of gesture moves the
    /// content (resistance = 1 - overshoot/limit, the same feel as iOS), so it
    /// asymptotes at `overscroll_limit` instead of stopping dead.
    constexpr void scroll_by(Vec2 delta) noexcept {
        offset.x = apply_scroll(offset.x, -delta.x * speed, max_offset.x);
        offset.y = apply_scroll(offset.y, -delta.y * speed, max_offset.y);
    }

    constexpr void scroll_to(Vec2 position) noexcept {
        offset.x = num::clamp(position.x, 0.0f, max_offset.x);
        offset.y = num::clamp(position.y, 0.0f, max_offset.y);
    }

    // ── kinetic scrolling ────────────────────────────────────────
    //
    // A flick should glide and coast to a stop, not halt the instant the
    // finger lifts. The producer of the gesture calls `fling` with the release
    // velocity (px/s); the runtime then calls `step(dt)` each frame while
    // `coasting()`, and the offset decelerates under exponential friction —
    // the model iOS and every good touch UI use. It is all pure: the momentum
    // lives in the model, so the glide is deterministic and replayable.

    /// Start (or add to) momentum from a release velocity, in content px/s.
    /// The sign matches `scroll_by`: an upward flick (negative delta) reveals
    /// content below, so it drives `offset` up.
    constexpr void fling(Vec2 v) noexcept {
        velocity.x += -v.x * speed;
        velocity.y += -v.y * speed;
        // A tiny flick is a tap; do not coast on noise.
        if (num::abs(velocity.x) < 40.0f) velocity.x = 0.0f;
        if (num::abs(velocity.y) < 40.0f) velocity.y = 0.0f;
    }

    /// Advance momentum by `dt` seconds. Returns true while still coasting, so
    /// the caller keeps requesting frames until it settles. Friction is
    /// exponential (a constant fraction of speed shed per second), which gives
    /// the long, smooth tail-off that feels right; the glide stops when speed
    /// drops below a pixel-per-second threshold or hits an edge.
    constexpr bool step(float dt) noexcept {
        if (!coasting() || dt <= 0.0f) return false;

        // 0.0025 per second retained => ~6 units/s after 1s from 1000; a
        // firm-but-not-endless deceleration. pow via exp/log kept out of the
        // hot path: dt is small, so a first-order-exact form is fine.
        constexpr float friction = 0.0025f;
        const float decay = num::pow(friction, dt);

        offset.x += velocity.x * dt;
        offset.y += velocity.y * dt;
        velocity.x *= decay;
        velocity.y *= decay;

        if (overscroll) {
            // Past an edge: a critically-damped spring pulls back to it, and
            // momentum bleeds off fast so a fling overshoots a little and
            // returns — the bounce. When in bounds this is a no-op.
            spring_axis(offset.x, velocity.x, max_offset.x, dt);
            spring_axis(offset.y, velocity.y, max_offset.y, dt);
        } else {
            // Hard stop: hitting an edge kills momentum on that axis.
            if (offset.x <= 0.0f || offset.x >= max_offset.x) velocity.x = 0.0f;
            if (offset.y <= 0.0f || offset.y >= max_offset.y) velocity.y = 0.0f;
            offset.x = num::clamp(offset.x, 0.0f, max_offset.x);
            offset.y = num::clamp(offset.y, 0.0f, max_offset.y);
        }

        // Settle: below ~8 px/s the motion is imperceptible; snap to rest so
        // the frame subscription can end and the app fall back to 0% CPU. In
        // overscroll mode also require the offset to have returned to bounds.
        if (num::abs(velocity.x) < 8.0f) velocity.x = 0.0f;
        if (num::abs(velocity.y) < 8.0f) velocity.y = 0.0f;
        return coasting();
    }

    /// Stop any glide immediately — a fresh touch on a coasting list grabs it.
    constexpr void halt() noexcept { velocity = Vec2{}; }

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

  private:
    /// Apply a scroll delta on one axis with rubber-band resistance outside
    /// [0, max]. In bounds it is a plain clamp; past an edge each pixel of
    /// gesture moves the content by (1 - overshoot/limit), so it asymptotes at
    /// `overscroll_limit` and never runs away.
    [[nodiscard]] constexpr float apply_scroll(float pos, float delta, float max) const noexcept {
        if (!overscroll) return num::clamp(pos + delta, 0.0f, max);

        float next = pos + delta;
        const float over = next < 0.0f ? -next : (next > max ? next - max : 0.0f);
        if (over <= 0.0f) return num::clamp(next, 0.0f, max);

        // Recompute the moved portion with resistance applied to the part that
        // crossed the edge. Resistance grows with how far out we already are.
        const float edge = next < 0.0f ? 0.0f : max;
        const float prev_over = pos < 0.0f ? -pos : (pos > max ? pos - max : 0.0f);
        const float raw_step = (next - edge) - (pos - edge);   // signed gesture past edge
        const float resist = 1.0f - num::saturate(prev_over / overscroll_limit);
        float result = pos + raw_step * resist * (prev_over > 0.0f ? 1.0f : 0.5f);
        // Clamp the overshoot to the soft ceiling.
        if (result < -overscroll_limit) result = -overscroll_limit;
        if (result > max + overscroll_limit) result = max + overscroll_limit;
        return result;
    }

    /// Critically-damped spring back toward the nearest edge for one axis, if
    /// the offset is past it. Mutates offset and velocity in place.
    static constexpr void spring_axis(float& pos, float& vel, float max, float dt) noexcept {
        const float target = pos < 0.0f ? 0.0f : (pos > max ? max : pos);
        if (target == pos) return;   // in bounds

        // Stiff spring + heavy damping: the content snaps back promptly without
        // oscillating. Tuned so a modest overshoot returns in ~0.3 s.
        constexpr float stiffness = 200.0f;
        constexpr float damping   = 28.0f;
        const float displacement = pos - target;
        const float accel = -stiffness * displacement - damping * vel;
        vel += accel * dt;
        pos += vel * dt;

        // Land exactly on the edge once close, so the spring terminates.
        if (num::abs(pos - target) < 0.5f && num::abs(vel) < 20.0f) {
            pos = target;
            vel = 0.0f;
        }
    }
};

}  // namespace mayag
