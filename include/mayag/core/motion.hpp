#pragma once
// mayag::motion — animation that follows real geometry
//
// The failure this exists to prevent: an app animating a MAGIC NUMBER that
// approximates a layout it cannot see. mayag's own todo example slid a filter
// underline with `indicator * 52.0f`, guessing the chips were 52px apart and
// 46px wide. They were at x=293/333/385 with widths 34/51/47, so the
// underline drifted further wrong with every tab — visibly, in a screenshot.
//
// The fix is not a better guess. It is to animate the MEASURED RECT of a real
// node:
//
//     // view
//     indicator(m.underline, node_id("filter-active"))   // follows that node
//
// `Tracked` remembers where a named node actually landed last frame, and the
// spring drives toward that. When the layout changes — different font, wider
// label, a resized window — the animation follows, because it never had a
// number of its own.
//
// The second half is `request_animation_frame()`: a widget that is mid-motion
// says so during the view pass, and the runtime folds that into the same
// "when do I next wake" decision it already makes for timers and frame
// subscriptions. There is no second clock, and an app cannot forget to stop
// animating because nothing was ever started.

#include "../core/animation.hpp"
#include "../core/geometry.hpp"
#include "../scene/node.hpp"

#include <cstdint>
#include <unordered_map>

namespace mayag {

// ── frame requests ──────────────────────────────────────────────────────

namespace detail {
/// Set during a view pass by anything that is mid-animation.
///
/// Thread-local because a headless test and a window can render on different
/// threads, and a stray request from one must not keep the other awake.
inline thread_local bool animation_requested = false;
}  // namespace detail

/// "I am animating; please schedule another frame."
///
/// Called from `view()` by widgets that are mid-motion. The runtime collects
/// these alongside timers and frame subscriptions, so animation participates
/// in the SAME wake computation as everything else — no separate clock, no
/// parallel pump, and no way to leave an app spinning after motion ends.
inline void request_animation_frame() noexcept {
    detail::animation_requested = true;
}

[[nodiscard]] inline bool animation_was_requested() noexcept {
    return detail::animation_requested;
}
inline void clear_animation_request() noexcept {
    detail::animation_requested = false;
}

// ── tracked geometry ────────────────────────────────────────────────────

/// A rect that follows a named node's real layout position.
///
/// Holds the animated value AND the last measured target, so it can be driven
/// from `update()` (which has no tree) using geometry recorded during the
/// previous `view()`.
struct Tracked {
    // `mutable` for the same reason ScrollState's measurements are: `view()`
    // is a pure function of the model and takes it by const reference, but
    // the runtime genuinely learns the target rect during layout and must
    // record it somewhere.
    //
    // Safe because the target is DERIVED — recomputed from the tree every
    // frame — and the animation is a pure function of it. Two identical
    // models still produce identical frames.
    mutable Animated<Vec2> position{};
    mutable Animated<Vec2> extent{};

    /// Which node is being followed. Changing this retargets the spring, and
    /// because springs carry velocity, switching tabs mid-slide continues
    /// smoothly rather than restarting.
    mutable std::uint64_t target_id = 0;

    /// True once a real rect has been observed; before that the first
    /// observation SNAPS rather than animating in from the origin.
    mutable bool initialised = false;

    [[nodiscard]] Rect rect() const noexcept {
        return Rect{position.value(), extent.value()};
    }

    /// False until the target has been measured at least once.
    ///
    /// On frame ZERO the layout has not run yet, so there is no rect to
    /// follow. A view must not draw a follower before then — it would be a
    /// zero-size node at the origin, which is both invisible and (correctly)
    /// reported by `layout::audit()` as a collapsed node.
    [[nodiscard]] bool ready() const noexcept { return initialised; }
    [[nodiscard]] bool animating() const noexcept {
        return position.animating() || extent.animating();
    }

    /// Point at a different node.
    void follow(std::uint64_t id) const noexcept { target_id = id; }

    /// Record where the target actually is. Called by the runtime after
    /// layout, so the spring always chases a REAL rect rather than a guess.
    void observe(const Rect& r) const {
        if (!initialised) {
            position.snap(r.origin);
            extent.snap(r.size);
            initialised = true;
            return;
        }
        position.to(r.origin);
        extent.to(r.size);
    }

    /// Advance. Returns true while still moving.
    bool step(double dt, Spring spring = Spring::snappy()) const {
        const bool a = position.step(dt, spring);
        const bool b = extent.step(dt, spring);
        return a || b;
    }
};

/// Every `Tracked` in a Model, resolved after layout.
///
/// The runtime walks this once per frame: for each entry it finds the target
/// node's rect and calls `observe()`. That is the whole mechanism — an app
/// never queries the tree itself, and never has to.
class TrackedSet {
  public:
    void register_tracked(const Tracked* t) {
        if (t != nullptr) items_.push_back(t);
    }
    void clear() noexcept { items_.clear(); }

    /// Point every tracked value at its node's measured rect.
    void observe_all(const Node& root) {
        for (const Tracked* t : items_) {
            if (t->target_id == 0) continue;
            if (const Node* n = root.find(t->target_id)) t->observe(n->frame());
        }
    }

    /// Advance every tracked value; returns true if any is still moving.
    bool step_all(double dt, Spring spring = Spring::snappy()) {
        bool moving = false;
        for (const Tracked* t : items_) moving |= t->step(dt, spring);
        return moving;
    }

    [[nodiscard]] bool animating() const noexcept {
        for (const Tracked* t : items_) if (t->animating()) return true;
        return false;
    }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

  private:
    std::vector<const Tracked*> items_;
};

}  // namespace mayag
