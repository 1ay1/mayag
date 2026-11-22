#pragma once
// mayag::platform::PhotonMeter — input to photon, kept the same way on every
// window backend.
//
// A backend knows two platform things: when an input happened (an event's
// own timestamp, on the clock its compositor reports presentation on) and
// when a frame reached the glass. Everything between -- which input a frame
// is measuring, what happens to a replaced frame, the samples, their
// percentiles, the per-stage means -- is policy, and lives here once:
//
//   note(t, read_at)   an input at `t`, read by us at `read_at`. The OLDEST
//                      unshown input is the one measured (a frame shows the
//                      newest input it was drawn with, but the user waited
//                      since the first one), so a later input never
//                      overwrites an earlier unshown one.
//   take()             what the frame being committed now carries (the
//                      unshown input stays unshown: a frame replaced before
//                      scanout must not take the sample with it -- the next
//                      frame carries the same input).
//   presented(f, t)    frame `f` hit the display at `t`: one sample, if it
//                      showed an input no earlier frame showed.
//   stats()            best / p50 / mean / p95 / worst over the last 120,
//                      and the mean of each stage (event -> us, us -> commit,
//                      commit -> scanout).
//
// The backend's clock is opaque here: seconds, any epoch, as long as input
// times, commit times and presentation times share it.

#include "types.hpp"
#include "../core/event.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>
#include <variant>

namespace mayag::platform {

class PhotonMeter {
  public:
    /// What a committed frame carries, echoed back when it is presented.
    struct Frame {
        double input_at  = -1.0;   ///< the input it answers; < 0 = none
        double read_at   = -1.0;   ///< when we read that input
        double commit_at = 0.0;    ///< when the frame was handed over
    };

    /// MAYAG_PHOTON=2: a line per measured frame; =3: and every commit and
    /// presentation (to see frames that answer no input).
    [[nodiscard]] static int trace_level() noexcept {
        static const int lvl = [] {
            const char* e = std::getenv("MAYAG_PHOTON");
            return (e != nullptr && *e >= '2' && *e <= '9') ? *e - '0' : 0;
        }();
        return lvl;
    }

    /// An input event at `at`, read at `read_at` (both backend seconds).
    void note(double at, double read_at) noexcept {
        if (unshown_ >= 0.0) return;   // the oldest unshown input is the one to measure
        unshown_ = at;
        unshown_read_ = read_at;
    }
    /// The program says the next frame answers input from `at` (manual mode:
    /// a forwarding app whose window's own events are not the input).
    void mark(double at, double now) noexcept {
        if (unshown_ < 0.0 || at < unshown_) { unshown_ = at; unshown_read_ = now; }
    }
    [[nodiscard]] bool has_unshown() const noexcept { return unshown_ >= 0.0; }
    [[nodiscard]] double unshown() const noexcept { return unshown_; }

    /// The frame committed at `now` carries the oldest unshown input.
    [[nodiscard]] Frame take(double now) const noexcept {
        if (trace_level() >= 3) {
            if (unshown_ >= 0.0) std::fprintf(stderr, "commit %.3f  +%.3f ms after input\n", now, (now - unshown_) * 1e3);
            else std::fprintf(stderr, "commit %.3f\n", now);
        }
        return Frame{unshown_, unshown_read_, now};
    }

    /// Frame `f` was presented at `at`. `flags` is for the trace only.
    void presented(const Frame& f, double at, double period_ms = 0.0, const char* flags = "") {
        if (trace_level() >= 3)
            std::fprintf(stderr, "present %.3f  commit %.3f  +%.2f ms\n", at, f.commit_at, (at - f.commit_at) * 1e3);
        if (f.input_at < 0.0 || f.input_at <= shown_) return;   // nothing new on screen
        shown_ = f.input_at;
        if (unshown_ == f.input_at) unshown_ = -1.0;   // an older mark made since stays unshown
        const double ms = (at - f.input_at) * 1e3;
        if (ms < 0.0 || ms > 5000.0) return;   // a clock glitch, not a frame
        record(ms);
        const double read = f.read_at >= 0.0 ? f.read_at : f.input_at;
        const std::array<double, 3> st{(read - f.input_at) * 1e3, (f.commit_at - read) * 1e3,
                                       (at - f.commit_at) * 1e3};
        if (trace_level() >= 2)
            std::fprintf(stderr,
                         "photon %.3f  to-us %6.2f  in-us %6.2f  compositor %6.2f  = %6.2f ms  (period %.2f%s%s)\n",
                         f.input_at, st[0], st[1], st[2], ms, period_ms, *flags ? ", flags " : "", flags);
        for (std::size_t i = 0; i < 3; ++i) stage_sum_[i] += st[i];
        ++stage_n_;
    }

    /// Every measured sample, as it is measured (ms).
    void set_sink(std::function<void(double)> f) { sink_ = std::move(f); }

    [[nodiscard]] PhotonStats stats() const noexcept {
        PhotonStats s;
        s.samples = static_cast<int>(count_);
        s.last_ms = last_;
        if (count_ > 0) {
            double sum = 0.0;
            for (std::size_t i = 0; i < count_; ++i) {
                sum += samples_[i];
                s.worst_ms = std::max(s.worst_ms, samples_[i]);
            }
            s.mean_ms = sum / static_cast<double>(count_);
            std::array<double, kWindow> v{};
            std::copy_n(samples_.begin(), count_, v.begin());
            std::sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(count_));
            const auto at = [&](double q) { return v[static_cast<std::size_t>(q * static_cast<double>(count_ - 1) + 0.5)]; };
            s.best_ms = v[0];
            s.p50_ms = at(0.50);
            s.p95_ms = at(0.95);
        }
        if (stage_n_ > 0)
            for (std::size_t i = 0; i < 3; ++i) s.stage_ms[i] = stage_sum_[i] / static_cast<double>(stage_n_);
        return s;
    }

  private:
    static constexpr std::size_t kWindow = 120;

    void record(double ms) {
        samples_[next_++ % kWindow] = ms;
        count_ = std::min(count_ + 1, kWindow);
        last_ = ms;
        if (sink_) sink_(ms);
    }

    std::array<double, kWindow> samples_{};
    std::size_t next_ = 0, count_ = 0;
    double last_ = 0.0;
    double unshown_ = -1.0, unshown_read_ = -1.0;
    double shown_ = -1.0;   // newest input a presented frame has shown
    std::array<double, 3> stage_sum_{};
    std::uint64_t stage_n_ = 0;
    std::function<void(double)> sink_;
};

/// Does this input start the photon clock? Not a window event (resize,
/// focus, close: no answer is drawn for them), and not a bare modifier press:
/// ctrl or shift alone answers with nothing on screen, so timing it measured
/// the wait for some LATER frame (a 500 ms blink) -- the 200 ms "zoom"
/// spikes measured on Wayland.
template <class E>
[[nodiscard]] bool starts_photon_clock(const E& e) noexcept {
    if (const auto* k = std::get_if<KeyEvent>(&e))
        return !(k->key == Key::ctrl || k->key == Key::shift || k->key == Key::alt || k->key == Key::super ||
                 k->key == Key::caps_lock);
    return !std::holds_alternative<ResizeEvent>(e) && !std::holds_alternative<FocusEvent>(e) &&
           !std::holds_alternative<CloseRequest>(e);
}

}  // namespace mayag::platform
