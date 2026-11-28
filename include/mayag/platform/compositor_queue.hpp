#pragma once
// mayag::platform::CompositorQueue -- what a compositor that takes a frame
// the moment it is committed (Wayland: Hyprland, sway, any wlroots-style
// scheduler) is doing with ours, read back from presentation feedback.
//
// Such a compositor renders a commit at once when it is idle and flips it at
// the next vblank. It never REPLACES a frame it has taken: a second commit in
// the same refresh waits behind the pending flip and shows a refresh late.
// Worse, it stays that way -- every later commit lands while the previous
// flip is still pending, so every frame is a refresh behind (measured on
// Hyprland: 1.8 ms commit->glass in one run of frames, then one late frame,
// then 11.5 ms for every frame after it, for as long as frames kept coming).
// Only a refresh with no commit gets the compositor idle again.
//
// So the window host treats it as a one-deep queue (pacing rules 4 and 5):
// one present per refresh at its late-latch point, and never a present while
// a frame of ours is queued behind a vblank -- this class answers when that
// is (full), and how early before a vblank a commit must be to make it,
// learned per launch (margin), the way D3D11 learns DWM's deadline.
//
//   committed(t)         a frame handed over at t (seconds, the presentation
//                        clock); returns its ticket
//   presented(k, at, p)  ticket k reached the glass at `at`, refresh p
//   discarded(k)         replaced before scanout
//   full(now)            a frame that should have reached the glass by now is
//                        still waiting: a commit now queues behind it
//   margin()             how long before a vblank a commit still makes it
//
// Pure: no clock, no protocol. tests/test_platform_shared.cpp drives it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace mayag::platform {

class CompositorQueue {
  public:
    /// The learned margin's bounds and steps (seconds). It moves on the miss
    /// RATE of frames aimed at the latch point, never on one frame: measured
    /// on Hyprland, 10-20% of frames are shown a refresh late at EVERY commit
    /// phase (VRR, the compositor's own hiccups), and a margin that grew on
    /// each of them ran away to 11 ms -- every frame drawn most of a refresh
    /// early, its input that much staler. Widen when a quarter of the last
    /// kWindow aimed frames missed; narrow after a clean window.
    static constexpr double kStartMargin = 0.003;   // fullscreen Hyprland's deadline, measured: ~3 ms
    static constexpr double kMinMargin   = 0.001;
    static constexpr double kMaxMargin   = 0.006;
    static constexpr double kWiden       = 0.0005;
    static constexpr double kNarrow      = 0.00025;
    static constexpr int    kWindow      = 16;

    /// A frame was committed at `t`. One committed while another of ours is
    /// waiting in the SAME refresh, or overdue, queues behind it: its lateness
    /// is the queue's, not the margin's, so it teaches nothing. (A frame from
    /// an earlier refresh still in flight is the compositor's normal pipeline
    /// when it is deeper than one: windowed, every commit has one ahead.)
    std::uint64_t committed(double t) {
        const bool behind = std::any_of(pending_.begin(), pending_.end(), [&](const Entry& e) {
            if (stale(e, t)) return false;
            if (period_ <= 0.0 || vblank_ < 0.0) return true;
            return taken(e, t) || t > due(e.at);
        });
        pending_.push_back(Entry{++next_, t, !behind});
        if (pending_.size() > 64) pending_.erase(pending_.begin());   // a compositor that never answers
        return next_;
    }

    /// Ticket `k` reached the glass at `at`; `period` is the refresh (s).
    void presented(std::uint64_t k, double at, double period) {
        if (period > 0.0) period_ = period;
        const double last_vblank = vblank_;
        vblank_ = at;
        const auto it = find(k);
        if (it == pending_.end()) return;
        const Entry e = *it;
        pending_.erase(it);
        if (!e.counts || period_ <= 0.0 || last_vblank < 0.0) return;
        // How many vblanks after the one before its commit it was shown. On
        // time that is the compositor's pipeline depth: 1 for a window it
        // takes straight to the next flip (fullscreen), 2 where it composes
        // on the vblank for the one after (windowed Hyprland). One more is a
        // miss: the commit came after the compositor's deadline.
        const double waited = at - e.at;
        if (waited < 0.0 || waited > 4.0 * period_) return;   // a clock glitch, an occluded stretch
        const int k_shown = vblanks_after(e.at, at);
        if (k_shown < 1) return;
        learn_depth(k_shown);
        // How early before the next vblank it was committed: the host latches
        // at (next vblank - margin) whatever the depth -- a deeper compositor's
        // deadline for showing a frame `depth` vblanks on is still just before
        // the NEXT one, where it composes.
        const double lead = prev_vblank(e.at) + period_ - e.at;
        const bool missed = k_shown > depth_;
        missed ? ++misses_ : ++hits_;
        // Only a frame aimed AT the latch point says where the deadline is.
        // One drawn mid-refresh for an input (lead far above the margin)
        // that was late anyway hit a hiccup; one drawn past the latch point
        // (lead below it) was late by choice.
        if (std::fabs(lead - margin_) > 0.001) return;
        ++aimed_;
        if (missed) ++aimed_misses_;
        if (aimed_misses_ * 4 >= kWindow) {
            margin_ = std::min(margin_ + kWiden, kMaxMargin);
            aimed_ = aimed_misses_ = 0;
        } else if (aimed_ >= kWindow) {
            if (aimed_misses_ == 0) margin_ = std::max(margin_ - kNarrow, kMinMargin);
            aimed_ = aimed_misses_ = 0;
        }
    }

    void discarded(std::uint64_t k) {
        if (const auto it = find(k); it != pending_.end()) pending_.erase(it);
    }

    /// A commit now would wait behind a frame of ours instead of making the
    /// next vblank: one the compositor has already TAKEN for it (depth 1: it
    /// takes a commit the moment it arrives and holds it until that vblank),
    /// or one overdue -- past the vblank it was due at by a quarter refresh,
    /// so that vblank's feedback has had time to arrive, and not shown.
    /// (Deeper compositors latch the newest commit at the vblank: a frame of
    /// ours waiting for that is simply replaced, and holds nothing up.)
    [[nodiscard]] bool full(double now) const noexcept {
        if (period_ <= 0.0 || vblank_ < 0.0) return false;
        return std::any_of(pending_.begin(), pending_.end(), [&](const Entry& e) {
            return !stale(e, now) && (taken(e, now) || now > due(e.at) + period_ / 4.0);
        });
    }

    /// When a frame committed at `t` reaches the glass, if it makes the
    /// compositor's deadline (seconds; t + a refresh before any feedback).
    [[nodiscard]] double due(double t) const noexcept {
        if (vblank_ < 0.0 || period_ <= 0.0) return t + period_;
        return prev_vblank(t) + depth_ * period_;
    }

    [[nodiscard]] double margin() const noexcept { return margin_; }
    [[nodiscard]] int depth() const noexcept { return depth_; }
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> latch_misses() const noexcept { return {hits_, misses_}; }
    [[nodiscard]] std::size_t waiting() const noexcept { return pending_.size(); }

  private:
    struct Entry {
        std::uint64_t ticket;
        double at;      // committed
        bool counts;    // nothing was queued ahead of it: it teaches the margin
    };
    static constexpr double kTolerance = 0.0003;   // clock noise between commit and vblank stamps

    // A frame no answer will come for (an occluded window's, a compositor
    // that dropped it): after a few refreshes it no longer holds anything up.
    [[nodiscard]] bool stale(const Entry& e, double now) const noexcept {
        return period_ > 0.0 ? now - e.at > 4.0 * period_ : now - e.at > 0.1;
    }
    /// The vblank at or before `t` on the grid anchored at the newest presentation.
    [[nodiscard]] double prev_vblank(double t) const noexcept {
        return vblank_ + std::floor((t - vblank_) / period_ + kTolerance / period_) * period_;
    }
    /// Depth 1: frame `e`, committed in the refresh `now` is in, is being
    /// flipped at its end -- the compositor has it, and the next vblank too.
    [[nodiscard]] bool taken(const Entry& e, double now) const noexcept {
        return depth_ == 1 && std::fabs(prev_vblank(e.at) - prev_vblank(now)) < period_ / 2.0;
    }
    [[nodiscard]] int vblanks_after(double commit, double shown) const noexcept {
        return static_cast<int>(std::lround((shown - prev_vblank(commit)) / period_));
    }
    // The depth is the SMALLEST lag of recent frames: a deadline miss only
    // ever adds to it. A window (re)entering fullscreen changes it, so it is
    // the minimum over a window of frames, not over all time.
    void learn_depth(int k) noexcept {
        recent_[recent_n_++ % recent_.size()] = static_cast<std::int8_t>(std::min(k, 8));
        int d = 8;
        const std::size_t n = std::min<std::size_t>(recent_n_, recent_.size());
        for (std::size_t i = 0; i < n; ++i) d = std::min<int>(d, recent_[i]);
        depth_ = d;
    }
    [[nodiscard]] std::vector<Entry>::iterator find(std::uint64_t k) {
        return std::find_if(pending_.begin(), pending_.end(), [k](const Entry& e) { return e.ticket == k; });
    }

    std::vector<Entry> pending_;
    std::array<std::int8_t, 32> recent_{};
    std::size_t recent_n_ = 0;
    int depth_ = 1;
    int aimed_ = 0, aimed_misses_ = 0;   // frames aimed at the latch point in this window, and their misses
    std::uint64_t next_ = 0, hits_ = 0, misses_ = 0;
    double period_ = 0.0;   // seconds; 0 until the first presentation
    double vblank_ = -1.0;  // the newest presentation time: the grid's anchor
    double margin_ = kStartMargin;
};

}  // namespace mayag::platform
