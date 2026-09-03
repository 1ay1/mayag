#pragma once
// mayag::latency — input to photon, measured
//
// "Fast" is not a property you can claim; it is a number you have to show.
// This header exists so an app can SEE its own latency budget and assert on
// it in CI, rather than discovering a regression by feel six months later.
//
// The budget for one frame, measured on an M1:
//
//     input pickup      ~0.01 ms   (drained, not waited for)
//     update              0.01 ms
//     view                0.09 ms
//     layout              0.03 ms
//     paint               0.07 ms
//     raster              0.57 ms
//     encode              0.26 ms
//     ---------------------------
//     CPU total          ~1.0 ms
//     compositor        up to 16.7 ms   <-- the dominant term
//
// The CPU work is 6% of a 60 Hz frame. Everything that actually costs the
// user latency is scheduling: when the loop wakes, how many events it takes
// per frame, and how long the compositor holds the result. So that is what
// this file is about.
//
// Three techniques, in order of how much they win:
//
//   1. COALESCE. Drain every pending event before rendering ONCE. A trackpad
//      delivers ~120 moves/second; rendering each is 120 frames of work for
//      60 frames of visible result, and it makes the app *feel* slower
//      because the frame you see is several events stale.
//   2. LATE LATCH. Render close to the deadline, not immediately after the
//      event. A frame produced 15 ms early shows 15 ms-old input.
//   3. DAMAGE. Skip work whose output cannot have changed.

#include "../core/geometry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace mayag {

/// Where one frame's time went.
///
/// Every field is measured, not estimated. An app can print this, assert on
/// it, or draw it — the point is that latency is observable.
struct FrameTiming {
    double input_ms   = 0.0;   ///< draining and dispatching events
    double update_ms  = 0.0;   ///< P::update over all messages this frame
    double view_ms    = 0.0;   ///< P::view
    double layout_ms  = 0.0;
    double paint_ms   = 0.0;   ///< tree -> draw list
    double render_ms  = 0.0;   ///< rasterise + encode + present

    /// Events coalesced into this frame. Above 1 means the loop correctly
    /// batched a burst instead of rendering each one.
    int    events_coalesced = 0;

    /// Messages `update()` processed this frame.
    int    messages = 0;

    /// Pointer moves dropped as superseded. High during a drag, and that is
    /// the point: only the newest position can affect what is drawn.
    int    coalesced_moves = 0;

    /// Instances submitted, and draw calls after batching.
    std::uint32_t instances = 0;
    std::uint32_t draw_calls = 0;

    [[nodiscard]] double cpu_ms() const noexcept {
        return input_ms + update_ms + view_ms + layout_ms + paint_ms + render_ms;
    }

    /// Headroom against a refresh interval. Negative means the frame was
    /// missed — which is the number that actually matters, because a missed
    /// frame costs a full refresh of latency, not the microseconds it
    /// overran by.
    [[nodiscard]] double headroom_ms(double refresh_hz = 60.0) const noexcept {
        return (1000.0 / refresh_hz) - cpu_ms();
    }
    [[nodiscard]] bool missed_frame(double refresh_hz = 60.0) const noexcept {
        return headroom_ms(refresh_hz) < 0.0;
    }
};

/// A rolling window of frame timings.
///
/// Averages hide the frames that hurt. A UI that renders in 2 ms with an
/// occasional 30 ms hitch feels WORSE than one that steadily takes 8 ms,
/// because the hitch is what the user notices — so this reports the 99th
/// percentile and the worst frame alongside the mean.
class LatencyStats {
  public:
    static constexpr std::size_t window = 240;   ///< ~4 s at 60 Hz

    void record(const FrameTiming& t) {
        samples_[next_ % window] = t;
        ++next_;
        count_ = num::min<std::size_t>(count_ + 1, window);
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] const FrameTiming& last() const noexcept {
        return samples_[(next_ + window - 1) % window];
    }

    [[nodiscard]] double mean_cpu_ms() const noexcept {
        if (count_ == 0) return 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < count_; ++i) sum += samples_[i].cpu_ms();
        return sum / static_cast<double>(count_);
    }

    /// The frame that hurt. Reported because a single 30 ms hitch is more
    /// visible than a permanently mediocre average.
    [[nodiscard]] double worst_cpu_ms() const noexcept {
        double worst = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            worst = std::max(worst, samples_[i].cpu_ms());
        }
        return worst;
    }

    [[nodiscard]] double percentile_cpu_ms(double p) const {
        if (count_ == 0) return 0.0;
        double sorted[window];
        for (std::size_t i = 0; i < count_; ++i) sorted[i] = samples_[i].cpu_ms();
        std::sort(sorted, sorted + count_);
        const auto idx = static_cast<std::size_t>(
            num::clamp(p, 0.0, 1.0) * static_cast<double>(count_ - 1));
        return sorted[idx];
    }

    [[nodiscard]] int missed_frames(double refresh_hz = 60.0) const noexcept {
        int n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (samples_[i].missed_frame(refresh_hz)) ++n;
        }
        return n;
    }

    void clear() noexcept { next_ = 0; count_ = 0; }

  private:
    FrameTiming samples_[window]{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

/// Detects an app that never stops rendering.
///
/// The single most common way a UI framework produces something that "looks
/// hung" is not a deadlock — it is an app that renders continuously and pins
/// a core, so the window is technically alive but the machine is unusable.
/// mayag shipped exactly that: a demo whose animation flag defaulted to true
/// rendered 60 fps from launch, forever.
///
/// A deadlock is easy to notice. A busy loop is not, because everything
/// keeps working — which is why it needs a detector rather than an assertion.
class BusyLoopDetector {
  public:
    /// Call once per presented frame.
    void frame_presented(double now_seconds) {
        if (window_start_ <= 0.0) { window_start_ = now_seconds; frames_ = 0; }
        ++frames_;

        const double elapsed = now_seconds - window_start_;
        if (elapsed >= window_seconds) {
            rate_ = static_cast<double>(frames_) / elapsed;
            sustained_ = (rate_ > threshold_hz);
            window_start_ = now_seconds;
            frames_ = 0;
        }
    }

    /// Call when a frame was SKIPPED because nothing changed. Resets the
    /// window — an app that goes quiet is behaving correctly.
    void idle() noexcept {
        window_start_ = 0.0;
        frames_ = 0;
        sustained_ = false;
    }

    /// True when the app has rendered continuously for several seconds.
    /// Not necessarily a bug — a game or a visualiser genuinely animates —
    /// but always worth surfacing, because the alternative is a user
    /// reporting "the app is hung".
    [[nodiscard]] bool busy() const noexcept { return sustained_; }
    [[nodiscard]] double frame_rate() const noexcept { return rate_; }

  private:
    static constexpr double window_seconds = 3.0;
    static constexpr double threshold_hz   = 30.0;

    double window_start_ = 0.0;
    int    frames_ = 0;
    double rate_ = 0.0;
    bool   sustained_ = false;
};

/// Scoped timer writing into a double, in milliseconds.
class ScopedTimer {
  public:
    explicit ScopedTimer(double& out) noexcept
        : out_{&out}, start_{std::chrono::steady_clock::now()} {}

    ~ScopedTimer() {
        *out_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_).count();
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

  private:
    double*                                            out_;
    std::chrono::steady_clock::time_point              start_;
};

}  // namespace mayag
