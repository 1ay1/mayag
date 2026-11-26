#pragma once
// mayag::pacing — WHEN to draw, as pure arithmetic on the display's grid.
//
// Every frame-timing decision the window host makes lives here, as functions
// of (now, the vblank grid, a little state) with no clock, no OS and no
// window. That is the point: each rule below was once a bug that made motion
// jerk while every frame was "on time", and each is now pinned by
// tests/test_pacing.cpp, which drives these exact functions through
// thousands of refreshes with jittered wake-ups -- timers fire late, the
// thread is descheduled, input arrives at mouse rate -- and fails the build
// if a single refresh goes without its frame.
//
// The rules (each a past root cause):
//   1. A held draw keeps the deadline it was held FOR (Hold). Re-deciding
//      when the timer fires a hair past that point -- "deadline passed, next
//      refresh" -- slipped every input-driven frame a refresh: drags at
//      20-30 fps while animation ran at 60.
//   2. Motion advances by DISPLAY time (display_time / tick_step): whole
//      refreshes, not wake-up deltas. Stepping by wake time moved content
//      15 ms then 18 ms while each frame showed 16.7 -- judder with no late
//      frame anywhere.
//   3. Motion that starts after an idle steps one refresh, never leaps.
//   4. A one-deep queue gets ONE present per refresh, at its late-latch
//      point (one_deep_deadline); a second present queues behind the first
//      and shows a refresh late.
//   5. Never present into a full queue: hold to the next latch point
//      (full_queue_deadline); presenting blocks to the vblank and leaves
//      every following frame a refresh behind.
//   6. A frame tick booked for a vblank serves THAT vblank (tick_target),
//      even when its timer wakes a little late -- booked as a time, so a
//      re-anchored grid cannot renumber it.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace mayag::pacing {

/// The display's refresh grid: slot n is [vblank + n*period, vblank + (n+1)*period).
template <class TimePoint>
struct Grid {
    using time_point = TimePoint;
    using duration   = typename TimePoint::duration;
    time_point vblank{};
    duration   period{};
    duration   margin{};   ///< how long before a vblank a frame must be presented

    [[nodiscard]] std::int64_t slot_of(time_point t) const noexcept {
        const auto d = t - vblank;
        return d >= duration::zero() ? d / period : -((-d) / period) - 1;
    }
    [[nodiscard]] time_point slot_start(std::int64_t n) const noexcept { return vblank + n * period; }
    /// Late enough to carry the newest input, early enough to make the vblank ending slot n.
    [[nodiscard]] time_point latch_point(std::int64_t n) const noexcept { return slot_start(n + 1) - margin; }

    /// Rule 2: when a frame drawn now reaches the glass -- the vblank ending
    /// the refresh whose latch point it still makes. Monotonic in `now`.
    [[nodiscard]] time_point display_time(time_point now) const noexcept {
        auto s = slot_of(now);
        if (now > latch_point(s)) ++s;
        return slot_start(s + 1);
    }

    /// Rule 4: the one present a refresh, at its latch point; past it, the
    /// next refresh's; a refresh that already has its frame is skipped.
    [[nodiscard]] time_point one_deep_deadline(time_point now, std::int64_t drawn_slot) const noexcept {
        auto t = slot_of(now);
        if (now > latch_point(t)) ++t;
        if (t <= drawn_slot) t = drawn_slot + 1;
        return std::max(now, latch_point(t));
    }

    /// Rule 5: a full queue holds the draw to the next latch point.
    [[nodiscard]] time_point full_queue_deadline(time_point now) const noexcept {
        const auto s = slot_of(now);
        return now < latch_point(s) ? latch_point(s) : latch_point(s + 1);
    }

    /// Rule 6: the vblank a frame tick serves. A tick BOOKED for a vblank
    /// serves THAT vblank even when its timer wakes a little after the latch
    /// point -- re-deciding from `now` ("past the latch: next refresh")
    /// skipped a refresh on every late wake-up. Only once the booked vblank
    /// itself has passed is it gone. Booked as a TIME, never a slot index:
    /// the grid is re-anchored every frame (the compositor clock), which
    /// renumbers slots -- a stored index then pointed hundreds of refreshes
    /// ahead and the animation stalled.
    [[nodiscard]] time_point tick_target(time_point now, std::optional<time_point> booked) const noexcept {
        if (booked && now < *booked) return *booked;
        // unbooked (the first tick of a motion): a wake within half the
        // margin past a latch point still makes that vblank -- the margin
        // carries that much slack for exactly this
        const auto s = slot_of(now);
        return now <= latch_point(s) + margin / 2 ? slot_start(s + 1) : slot_start(s + 2);
    }
    /// The vblank the NEXT tick is booked for, after serving `served`, and
    /// the instant to wake for it (that vblank's latch point).
    [[nodiscard]] time_point next_tick_target(time_point now, time_point served) const noexcept {
        const auto next = display_time(std::max(now, served - margin + period / 2));
        return next > served ? next : served + period;
    }
    [[nodiscard]] time_point wake_for(time_point target) const noexcept { return target - margin; }
};

/// Rule 1: a deadline decided once. decide() computes it the first time and
/// returns the SAME instant on every later call until drawn() -- a wake-up
/// a little late must draw now, not re-ask and land in the next refresh.
template <class TimePoint>
class Hold {
  public:
    template <class Compute>
    [[nodiscard]] TimePoint decide(Compute&& compute) {
        if (!at_) at_ = compute();
        return *at_;
    }
    [[nodiscard]] bool held() const noexcept { return at_.has_value(); }
    [[nodiscard]] TimePoint deadline() const noexcept { return at_.value_or(TimePoint{}); }
    void drawn() noexcept { at_.reset(); }
  private:
    std::optional<TimePoint> at_;
};

/// Rules 2-3: the animation step for a tick whose frame reaches the glass at
/// `target`, after a previous tick that targeted `last`.
struct Step {
    double dt = 0.0;       ///< seconds of motion to advance
    bool   skipped = false;   ///< a refresh went by with no new frame mid-motion (visible judder)
    bool   counted = false;   ///< a mid-motion tick (the denominator for `skipped`)
};
template <class TimePoint>
[[nodiscard]] Step tick_step(std::optional<TimePoint> last, TimePoint target, typename TimePoint::duration period) noexcept {
    const double p = std::chrono::duration<double>(period).count();
    if (!last) return {p, false, false};
    const double d = std::chrono::duration<double>(target - *last).count();
    if (d > 4.0 * p) return {p, false, false};   // motion STARTING after an idle: one refresh, never a leap
    return {std::max(d, 0.0), d > 1.5 * p, d > 0.5 * p};   // a few refreshes missed: catch up, keep speed
}

}  // namespace mayag::pacing
