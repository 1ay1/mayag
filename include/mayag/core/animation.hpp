#pragma once
// mayag::Animated<T> — values that move on their own
//
// Apps kept hand-rolling `phase += dt` in `update()` and deriving positions
// with sin/cos. That works, but it means every app reimplements easing,
// nothing is reusable, and the animation is entangled with application logic.
//
// `Animated<T>` is a value that knows where it is going. You set a target;
// it approaches. `update()` advances it by dt; `view()` reads its current
// value like any other field.
//
//     struct Model { Animated<float> panel_x{0.0f}; };
//
//     m.panel_x.to(open ? 0.0f : -300.0f);   // in update()
//     m.panel_x.step(dt);                     // once per frame
//     box() | offset(m.panel_x.value(), 0)    // in view()
//
// Springs, not curves, are the default. A duration-based tween restarts from
// zero when its target changes mid-flight, which is exactly what happens in a
// real UI (a user toggles a panel before it settles) and produces a visible
// stutter. A spring carries its VELOCITY across retargets, so an interrupted
// animation continues smoothly — that is the whole reason iOS and Android
// moved to spring-based systems.

#include "../core/geometry.hpp"
#include "../core/color.hpp"

#include <cstdint>

namespace mayag {

// ── easing ──────────────────────────────────────────────────────────────

namespace ease {

[[nodiscard]] constexpr float linear(float t) noexcept { return t; }
[[nodiscard]] constexpr float in_quad(float t) noexcept { return t * t; }
[[nodiscard]] constexpr float out_quad(float t) noexcept { return t * (2.0f - t); }
[[nodiscard]] constexpr float in_out_quad(float t) noexcept {
    return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
}
[[nodiscard]] constexpr float in_cubic(float t) noexcept { return t * t * t; }
[[nodiscard]] constexpr float out_cubic(float t) noexcept {
    const float u = t - 1.0f;
    return u * u * u + 1.0f;
}
[[nodiscard]] constexpr float in_out_cubic(float t) noexcept {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f + 4.0f * (t - 1.0f) * (t - 1.0f) * (t - 1.0f);
}
/// Overshoots slightly then settles — good for things appearing.
[[nodiscard]] constexpr float out_back(float t) noexcept {
    constexpr float c1 = 1.70158f;
    const float u = t - 1.0f;
    return 1.0f + (c1 + 1.0f) * u * u * u + c1 * u * u;
}

}  // namespace ease

// ── spring parameters ───────────────────────────────────────────────────

/// A critically-damped-ish spring, described the way a designer thinks about
/// it rather than in physical constants.
struct Spring {
    /// How fast it converges. Higher is snappier.
    float stiffness = 170.0f;
    /// How much it resists overshoot. At `2*sqrt(stiffness)` it is critically
    /// damped: fastest approach with no bounce.
    float damping = 26.0f;

    /// Presets covering what UIs actually need.
    [[nodiscard]] static constexpr Spring snappy()  { return {320.0f, 32.0f}; }
    [[nodiscard]] static constexpr Spring smooth()  { return {170.0f, 26.0f}; }
    [[nodiscard]] static constexpr Spring gentle()  { return {120.0f, 22.0f}; }
    [[nodiscard]] static constexpr Spring bouncy()  { return {220.0f, 14.0f}; }
    [[nodiscard]] static constexpr Spring stiff()   { return {500.0f, 42.0f}; }
};

// ── the animated value ──────────────────────────────────────────────────

namespace detail {

/// Component-wise access, so one implementation serves float / Vec2 / Color.
template <typename T> struct Components;

template <> struct Components<float> {
    static constexpr int count = 1;
    static constexpr float get(const float& v, int) noexcept { return v; }
    static constexpr void set(float& v, int, float x) noexcept { v = x; }
};

template <> struct Components<Vec2> {
    static constexpr int count = 2;
    static constexpr float get(const Vec2& v, int i) noexcept { return i == 0 ? v.x : v.y; }
    static constexpr void set(Vec2& v, int i, float x) noexcept { (i == 0 ? v.x : v.y) = x; }
};

template <> struct Components<Vec4> {
    static constexpr int count = 4;
    static constexpr float get(const Vec4& v, int i) noexcept {
        return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w;
    }
    static constexpr void set(Vec4& v, int i, float x) noexcept {
        (i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w) = x;
    }
};

}  // namespace detail

/// A value that approaches a target over time.
///
/// Plain data: copyable, comparable, part of your Model. Nothing here owns a
/// timer or a thread — `step(dt)` is called by your `update()`, so animation
/// is as testable and replayable as everything else.
template <typename T>
class Animated {
  public:
    using Comp = detail::Components<T>;

    Animated() = default;
    explicit Animated(T initial) : current_{initial}, target_{initial} {}

    // ── reading ─────────────────────────────────────────────────────────

    [[nodiscard]] const T& value() const noexcept { return current_; }
    [[nodiscard]] const T& target() const noexcept { return target_; }
    operator const T&() const noexcept { return current_; }

    /// True while still moving. The runtime uses this to decide whether the
    /// app needs frames — an animation that has settled must stop requesting
    /// them, or an idle window burns a core forever.
    [[nodiscard]] bool animating() const noexcept { return animating_; }

    // ── driving ─────────────────────────────────────────────────────────

    /// Set a new target. The value keeps its current VELOCITY, so retargeting
    /// mid-flight continues smoothly instead of restarting.
    void to(T destination) {
        if (destination == target_) return;
        target_ = destination;
        animating_ = true;
    }

    /// Jump immediately, cancelling any motion. For initial state and for
    /// "restore without animating".
    void snap(T v) {
        current_ = target_ = v;
        for (int i = 0; i < Comp::count; ++i) velocity_[i] = 0.0f;
        animating_ = false;
    }

    /// Advance by `dt` seconds. Returns true while still moving.
    ///
    /// The integration is sub-stepped at a fixed 1/240 s. A spring integrated
    /// with a variable timestep is not merely inaccurate, it is UNSTABLE: one
    /// long frame (a GC pause, a window drag) can make a stiff spring explode
    /// into oscillation. Fixed sub-steps make the motion identical whether
    /// the app is running at 30, 60 or 120 Hz.
    bool step(double dt, Spring spring = Spring::smooth()) {
        if (!animating_) return false;

        constexpr float fixed = 1.0f / 240.0f;
        float remaining = num::clamp(static_cast<float>(dt), 0.0f, 0.25f);

        while (remaining > 0.0f) {
            const float h = num::min(remaining, fixed);
            remaining -= h;

            for (int i = 0; i < Comp::count; ++i) {
                const float x = Comp::get(current_, i);
                const float g = Comp::get(target_, i);
                const float a = spring.stiffness * (g - x) - spring.damping * velocity_[i];
                velocity_[i] += a * h;
                Comp::set(current_, i, x + velocity_[i] * h);
            }
        }

        // Settle when both the displacement and the velocity are below what a
        // pixel can show. Testing displacement alone leaves a spring
        // "finished" at the top of an overshoot, still moving.
        bool settled = true;
        for (int i = 0; i < Comp::count; ++i) {
            if (num::abs(Comp::get(target_, i) - Comp::get(current_, i)) > 0.01f ||
                num::abs(velocity_[i]) > 0.01f) {
                settled = false;
                break;
            }
        }
        if (settled) {
            current_ = target_;
            for (int i = 0; i < Comp::count; ++i) velocity_[i] = 0.0f;
            animating_ = false;
        }
        return animating_;
    }

    /// Duration-based alternative, for motion that must take a known time
    /// (a progress bar, a timed reveal).
    bool step_eased(double dt, double duration, float (*curve)(float) = ease::in_out_cubic) {
        if (!animating_) return false;
        elapsed_ += dt;
        const float t = duration > 0.0
            ? num::saturate(static_cast<float>(elapsed_ / duration))
            : 1.0f;
        const float k = curve(t);
        for (int i = 0; i < Comp::count; ++i) {
            Comp::set(current_, i, num::lerp(Comp::get(from_, i), Comp::get(target_, i), k));
        }
        if (t >= 1.0f) { current_ = target_; animating_ = false; }
        return animating_;
    }

    /// Begin a duration-based transition from the current value.
    void tween_to(T destination) {
        from_ = current_;
        target_ = destination;
        elapsed_ = 0.0;
        animating_ = true;
    }

    friend bool operator==(const Animated&, const Animated&) = default;

  private:
    T     current_{};
    T     target_{};
    T     from_{};
    float velocity_[Comp::count]{};
    double elapsed_ = 0.0;
    bool  animating_ = false;
};

/// Convenience aliases for the types UIs animate.
using AnimatedFloat = Animated<float>;
using AnimatedVec2  = Animated<Vec2>;

/// Colour animation goes through Oklch, so a fade between two hues stays
/// vivid instead of passing through grey — the same reasoning as gradients.
class AnimatedColor {
  public:
    AnimatedColor() = default;
    explicit AnimatedColor(Color<Srgb> initial)
        : lch_{Vec4{initial.to<Oklch>().c0, initial.to<Oklch>().c1,
                    initial.to<Oklch>().c2, initial.a}} {}

    [[nodiscard]] Color<Srgb> value() const {
        const Vec4 v = lch_.value();
        return Color<Oklch>{v.x, v.y, v.z, v.w}.to<Srgb>();
    }
    [[nodiscard]] bool animating() const noexcept { return lch_.animating(); }

    void to(Color<Srgb> c) {
        const auto t = c.to<Oklch>();
        Vec4 want{t.c0, t.c1, t.c2, c.a};

        // Take the short way around the hue circle, or a red-to-magenta fade
        // detours through green.
        const Vec4 cur = lch_.value();
        float dh = want.z - cur.z;
        if (dh > 180.0f)  want.z -= 360.0f;
        if (dh < -180.0f) want.z += 360.0f;

        lch_.to(want);
    }
    void snap(Color<Srgb> c) {
        const auto t = c.to<Oklch>();
        lch_.snap(Vec4{t.c0, t.c1, t.c2, c.a});
    }
    bool step(double dt, Spring s = Spring::smooth()) { return lch_.step(dt, s); }

  private:
    Animated<Vec4> lch_;
};

}  // namespace mayag
