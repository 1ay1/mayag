#pragma once

/// Window and workspace transitions: what a desktop actually animates.
///
/// A compositor's motion is not decoration. It is how a user keeps track of
/// where things went: a window that vanishes is confusing, and one that
/// shrinks toward the dock it minimised into is obvious. Getting this right
/// is the difference between a desktop that feels alive and one that feels
/// like a slideshow.
///
/// Nothing here is new machinery. mayag already has springs -- with presets
/// tuned for exactly this -- and `Effects`, which the renderer already knows
/// how to draw. A transition is just a spring driving those fields, and
/// keeping it that way is deliberate: an effect that only animations can
/// produce is an effect a compositor cannot compose with anything else.
///
/// SPRINGS, NOT DURATIONS. A duration-based animation has to be restarted
/// when its target changes mid-flight, and restarting loses the velocity --
/// which is why so many desktops stutter when you close a window while it is
/// still opening. A spring carries velocity through a retarget, so
/// interrupting an animation is not a special case that needs handling; it is
/// the normal thing the type already does.

#include "../core/animation.hpp"
#include "output.hpp"

#include <cstdint>

namespace mayag::output {

/// How a window enters and leaves.
///
/// Named rather than parameterised because these are the vocabulary of a
/// desktop, and a compositor choosing between "scale" and "genie" is making a
/// design decision, not tuning a number.
enum class WindowTransition : std::uint8_t {
    /// Fade only. The safe default and the right one for a dialog, where
    /// movement would draw the eye away from what it is asking.
    fade,
    /// Grow from slightly small while fading in. What almost every modern
    /// desktop does, because the scale reads as "this is arriving" without
    /// implying it came from anywhere in particular.
    scale,
    /// Rise from below. Good for sheets and notifications, which conceptually
    /// come from an edge.
    slide_up,
    /// Shrink toward a point -- the dock icon a window minimises into. The
    /// one transition that carries INFORMATION rather than polish: it tells
    /// the user where the window went, which is the whole reason minimising
    /// is not just hiding.
    genie,
};

/// A window animating in, out, or between states.
///
/// Holds springs rather than timers, so a window closed while still opening
/// reverses smoothly from wherever it had reached, with the velocity it had.
///
/// NOT ANIMATED, on purpose: `Effects::backdrop_blur`. Animating a blur means
/// re-blurring every frame of the transition, which is the one effect where
/// "just animate it" costs real power -- a blur is a multi-tap sample per
/// pixel, so a fading one is the most expensive thing on screen for exactly
/// as long as it is least noticeable. A compositor that wants it can drive
/// the field itself; deliberately not made easy.
struct WindowMotion {
    /// 0 while absent, 1 while fully present. Everything else is derived from
    /// this, so a transition cannot get into a state where its opacity and
    /// its scale disagree.
    Animated<float> presence{0.0f};
    /// Where the window is going, animated so a tiled layout change slides
    /// rather than teleports.
    Animated<Vec2>  position{Vec2{}};
    Animated<Vec2>  size{Vec2{}};
    /// Where a genie transition shrinks toward. Ignored otherwise.
    Vec2            anchor{};
    WindowTransition kind = WindowTransition::scale;
    /// How it moves. Snappy by default: a window appearing should feel
    /// immediate, and anything slower reads as the machine thinking.
    Spring          spring = Spring::snappy();

    void step(float dt) {
        presence.step(static_cast<double>(dt), spring);
        position.step(static_cast<double>(dt), spring);
        size.step(static_cast<double>(dt), spring);
    }
    /// Is anything still moving? A compositor asks this to know whether it
    /// may stop requesting frames -- an idle desktop must cost nothing.
    [[nodiscard]] bool animating() const noexcept {
        return presence.animating() || position.animating() || size.animating();
    }

    void open()  { presence.to(1.0f); }
    void close() { presence.to(0.0f); }
    /// True once a closing window has finished and may be dropped.
    [[nodiscard]] bool gone() const noexcept {
        return static_cast<float>(presence) < 0.01f && !presence.animating();
    }
};

/// Apply a window's motion to the layer that draws it.
///
/// Returns the layer by value: a transition DECIDES geometry and effects, it
/// does not own the layer. That keeps a compositor free to run its own layout
/// first and hand the result here, rather than the other way round.
[[nodiscard]] inline Layer animate(Layer l, const WindowMotion& m) {
    const float p = static_cast<float>(m.presence);
    l.effects.opacity *= p;

    switch (m.kind) {
    case WindowTransition::fade:
        break;                       // opacity is the whole effect

    case WindowTransition::scale: {
        // From 92%, not from zero. A window that grows from nothing reads as
        // an explosion; the small remaining scale is what makes it feel like
        // it was always there and is settling in.
        const float s = 0.92f + 0.08f * p;
        const Vec2 centre{l.dst.origin.x + l.dst.size.x * 0.5f,
                          l.dst.origin.y + l.dst.size.y * 0.5f};
        l.dst.size = Vec2{l.dst.size.x * s, l.dst.size.y * s};
        l.dst.origin = Vec2{centre.x - l.dst.size.x * 0.5f,
                            centre.y - l.dst.size.y * 0.5f};
        break;
    }

    case WindowTransition::slide_up: {
        // Travel proportional to the window, not a fixed distance: a fixed
        // offset looks right on a dialog and barely moves a fullscreen window.
        const float travel = l.dst.size.y * 0.08f;
        l.dst.origin.y += travel * (1.0f - p);
        break;
    }

    case WindowTransition::genie: {
        // Shrink toward the anchor -- the dock icon it minimises into. The
        // window keeps its aspect while collapsing, so the eye can follow it
        // all the way down rather than losing it halfway.
        const Vec2 centre{l.dst.origin.x + l.dst.size.x * 0.5f,
                          l.dst.origin.y + l.dst.size.y * 0.5f};
        const Vec2 target = m.anchor;
        const Vec2 now{centre.x + (target.x - centre.x) * (1.0f - p),
                       centre.y + (target.y - centre.y) * (1.0f - p)};
        l.dst.size = Vec2{l.dst.size.x * p, l.dst.size.y * p};
        l.dst.origin = Vec2{now.x - l.dst.size.x * 0.5f,
                            now.y - l.dst.size.y * 0.5f};
        break;
    }
    }
    return l;
}

// ── workspaces ──────────────────────────────────────────────────────────

/// How workspaces move past each other.
enum class WorkspaceTransition : std::uint8_t {
    /// Slide horizontally. The spatial metaphor: workspaces are side by side,
    /// and the animation is what teaches that.
    slide,
    /// Slide, with the outgoing workspace scaling down slightly. Reads as
    /// depth -- the old workspace receding rather than being shoved aside.
    slide_depth,
    /// Cross-fade. No spatial claim, which is right when workspaces are a
    /// list rather than a strip.
    fade,
};

/// A workspace switch in progress.
struct WorkspaceMotion {
    /// Current position in workspace units. 1.5 means halfway between
    /// workspace 1 and 2 -- a continuous value rather than a from/to pair,
    /// because a user can flick past three workspaces before the first
    /// animation finishes and a pair cannot express that.
    Animated<float>     offset{0.0f};
    WorkspaceTransition kind = WorkspaceTransition::slide_depth;
    /// Smooth rather than snappy: a workspace switch moves the whole screen,
    /// and at that scale a fast spring reads as a jolt.
    Spring              spring = Spring::smooth();

    void step(float dt) { offset.step(static_cast<double>(dt), spring); }
    [[nodiscard]] bool animating() const noexcept { return offset.animating(); }
    void go_to(int index) { offset.to(static_cast<float>(index)); }
};

/// Place a layer belonging to `workspace` for the current switch state.
///
/// `viewport` is the output size, so a workspace one to the right sits one
/// screen width away.
[[nodiscard]] inline Layer animate(Layer l, const WorkspaceMotion& m,
                                   int workspace, Vec2 viewport) {
    const float delta = static_cast<float>(workspace) - static_cast<float>(m.offset);

    switch (m.kind) {
    case WorkspaceTransition::fade: {
        // Visible only near its own workspace. abs() rather than a signed
        // fade so both neighbours behave the same.
        const float d = delta < 0.0f ? -delta : delta;
        l.effects.opacity *= num::clamp(1.0f - d, 0.0f, 1.0f);
        break;
    }

    case WorkspaceTransition::slide:
        l.dst.origin.x += delta * viewport.x;
        break;

    case WorkspaceTransition::slide_depth: {
        l.dst.origin.x += delta * viewport.x;
        // Scale toward 88% at one screen away, so the outgoing workspace
        // recedes instead of merely sliding. Clamped because a workspace
        // three away would otherwise invert.
        const float d = delta < 0.0f ? -delta : delta;
        const float s = num::clamp(1.0f - 0.12f * d, 0.5f, 1.0f);
        const Vec2 centre{l.dst.origin.x + l.dst.size.x * 0.5f,
                          l.dst.origin.y + l.dst.size.y * 0.5f};
        l.dst.size = Vec2{l.dst.size.x * s, l.dst.size.y * s};
        l.dst.origin = Vec2{centre.x - l.dst.size.x * 0.5f,
                            centre.y - l.dst.size.y * 0.5f};
        break;
    }
    }
    return l;
}

// ── overview ────────────────────────────────────────────────────────────

/// The "show all windows" state, scaled into a grid.
///
/// Held as one animated value rather than per-window targets because every
/// window moves together: one spring for the whole gesture keeps them in
/// lockstep, where N springs would arrive at slightly different times and
/// read as jitter.
struct OverviewMotion {
    /// 0 is normal, 1 is fully zoomed out.
    Animated<float> t{0.0f};
    Spring          spring = Spring::smooth();

    void step(float dt) { t.step(static_cast<double>(dt), spring); }
    [[nodiscard]] bool animating() const noexcept { return t.animating(); }
    void show() { t.to(1.0f); }
    void hide() { t.to(0.0f); }
    [[nodiscard]] bool active() const noexcept {
        return static_cast<float>(t) > 0.01f;
    }
};

/// Interpolate a layer between its real position and its overview slot.
///
/// The compositor computes the grid -- that is layout, and it depends on how
/// many windows there are and what the user is looking for. This only moves
/// the layer along the way, which is the part with a right answer.
[[nodiscard]] inline Layer animate(Layer l, const OverviewMotion& m,
                                   const Rect& slot) {
    const float t = static_cast<float>(m.t);
    if (t <= 0.0f) return l;

    const auto lerp = [t](float a, float b) { return a + (b - a) * t; };
    const Rect from = l.dst;
    l.dst = Rect{{lerp(from.origin.x, slot.origin.x),
                  lerp(from.origin.y, slot.origin.y)},
                 {lerp(from.size.x, slot.size.x),
                  lerp(from.size.y, slot.size.y)}};

    // Round the corners on the way in. A grid of sharp rectangles reads as a
    // screenshot; rounding says these are objects you can pick up.
    l.effects.corner_radius = lerp(l.effects.corner_radius, 12.0f);
    return l;
}

}  // namespace mayag::output
