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
