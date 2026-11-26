// tests/test_pacing.cpp — frame pacing can't regress
//
// Drives mayag::pacing (the rules window_host schedules every frame with)
// through simulated displays -- 60 / 120 / 144 Hz, phases, margins -- with
// adversarial wake-ups: timers that fire up to 3 ms late (Windows' default
// timer slop is 1-15 ms; 3 ms is a loaded machine at 1 ms resolution),
// deschedulings, input at mouse rate. Each case is a past root cause of
// "the motion jerks"; each must hold for EVERY seed:
//
//   held      a drag (input every 1-8 ms, draws held for the latch point,
//             the timer late) puts a frame on every refresh       (rule 1, 4)
//   ticks     an animation ticking on booked latch points, timers late,
//             skips no refresh and steps exactly one refresh each (rule 2, 6)
//   resume    motion resuming after an idle steps one refresh     (rule 3)
//   monotone  display time never goes backwards                   (rule 2)
//   fullq     a full queue holds to a latch point, never 'now'    (rule 5)
//
// Deterministic: a fixed-seed LCG, no clock, no threads, no window.

#include <mayag/app/pacing.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace pacing = mayag::pacing;

namespace {

using clk = std::chrono::steady_clock;
using tp  = clk::time_point;
using ns  = std::chrono::nanoseconds;
using namespace std::chrono_literals;

int fails = 0;
void ok(bool c, const std::string& what) {
    std::printf("  %s %s\n", c ? "ok  " : "FAIL", what.c_str());
    if (!c) ++fails;
}

struct Rng {
    std::uint64_t s;
    std::uint64_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return s >> 33; }
    ns upto(ns max) { return ns{static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(max.count() + 1))}; }
};

struct Display { ns period; ns phase; ns margin; const char* name; };
const Display displays[] = {
    {ns{16'666'667}, 0ns, ns{8'333'333}, "60 Hz, half-refresh margin (Win32/DWM)"},
    {ns{16'666'667}, 3'100'000ns, 2'000'000ns, "60 Hz, 2 ms margin, odd phase"},
    {ns{8'333'333}, 1'000'000ns, ns{4'166'666}, "120 Hz"},
    {ns{6'944'444}, 500'000ns, 1'700'000ns, "144 Hz, tight margin"},
};

pacing::Grid<tp> grid_of(const Display& d) { return {tp{} + d.phase, d.period, d.margin}; }

// ── rule 1 + 4: every input is on screen at the earliest refresh it can make ──
// The host's input path: a change arrives; if the slot already has its frame
// or the latch point is ahead, the draw is held (Hold::decide) and a timer is
// set for the deadline; the timer fires LATE by up to `slop`; at the wake the
// host asks again and draws if now >= deadline. An input arriving before slot
// s's latch point must be shown at s's vblank (s+1 past it). Count the late ones.
int late_inputs(const Display& d, std::uint64_t seed, ns slop, bool use_hold) {
    const auto g = grid_of(d);
    Rng r{seed};
    pacing::Hold<tp> hold;
    std::int64_t drawn = -1;
    std::optional<tp> timer;
    tp now = g.slot_start(10);
    const tp end = g.slot_start(10 + 600);
    std::optional<std::int64_t> owed;   // the earliest refresh a pending input must make
    int late = 0;
    tp next_input = now;
    while (now < end) {
        const tp fire = timer ? *timer : tp::max();   // the wake instant, lateness rolled when armed
        now = std::min(next_input, fire);
        const bool input = now == next_input;
        if (input) {
            next_input = now + 1ms + r.upto(7ms);
            const auto s = g.slot_of(now);
            const auto need = now <= g.latch_point(s) ? s : s + 1;
            if (!owed) owed = need;
        } else {
            timer.reset();
        }
        if (!owed) continue;
        // a refresh went by with the input still not drawn: one late refresh each
        while (now > g.slot_start(*owed + 1)) { ++late; ++*owed; }
        const auto deadline = [&] { return g.one_deep_deadline(now, drawn); };
        const tp at = use_hold ? hold.decide(deadline) : deadline();
        if (now >= at) {
            // The margin is the compositor's deadline PLUS the wake-up slack
            // budget, so a draw up to `slop` past the latch point still makes
            // the vblank ending its slot; later than that it shows a refresh on.
            const auto s = g.slot_of(now);
            const auto shown = now <= g.slot_start(s + 1) - (d.margin - slop) ? s : s + 1;
            if (shown > *owed) ++late;
            owed.reset();
            drawn = s;   // as the host: the slot it was drawn in
            hold.drawn();
            timer.reset();
        } else {
            timer = at + r.upto(slop);   // the timer fires late by up to `slop`
        }
    }
    return late;
}

// ── rule 2 + 6: animation ticks ────────────────────────────────────────────
// The host re-anchors the grid on every compositor tick: the anchor jumps to
// the latest vblank (renumbering every slot) and the measured period wobbles
// by a few microseconds. Bookings must survive that -- a slot index did not
// (it pointed hundreds of refreshes ahead after the first re-anchor and the
// animation stalled; the test did not move its grid and missed it).
struct TickResult { int skipped = 0; int uneven = 0; int ticks = 0; int stalled = 0; int late = 0; };
TickResult animate(const Display& d, std::uint64_t seed, ns slop, bool booked) {
    auto g = grid_of(d);
    Rng r{seed};
    std::optional<tp> last;
    std::optional<tp> book;
    tp due = g.latch_point(20);
    TickResult out;
    for (int i = 0; i < 1000; ++i) {
        const tp now = due + r.upto(slop);   // the timer fires late
        // re-anchor: the anchor moves to the latest vblank, the period wobbles
        const ns wobble = ns{static_cast<std::int64_t>(r.next() % 9) - 4} * 1000 / 1000;
        g.vblank = g.slot_start(g.slot_of(now));
        g.period = d.period + wobble;
        const auto target = booked ? g.tick_target(now, book) : g.display_time(now);   // unbooked = the old rule
        if (target - now > d.period) ++out.late;   // aimed a whole refresh past the vblank it could make
        const auto st = pacing::tick_step(last, target, d.period);
        if (st.counted) {
            ++out.ticks;
            if (st.skipped) ++out.skipped;
            const double p = std::chrono::duration<double>(d.period).count();
            if (st.dt < 0.99 * p || st.dt > 1.01 * p) ++out.uneven;
        }
        last = target;
        book = booked ? std::optional<tp>{g.next_tick_target(now, target)} : std::nullopt;
        // the old code: re-decide the refresh from `now`, wake at the next latch point after it
        due = booked ? g.wake_for(*book) : g.full_queue_deadline(now);
        if (due - now > 2 * d.period) ++out.stalled;   // the next tick booked far away: the animation stops
    }
    return out;
}

}  // namespace

int main() {
    std::printf("pacing: every refresh gets its frame, every step is one refresh\n");
    constexpr int kSeeds = 200;
    for (const auto& d : displays) {
        std::printf(" %s\n", d.name);
        const ns slop = std::min<ns>(3ms, d.margin / 2);   // the slack budget inside the margin

        int late = 0;
        for (int s = 1; s <= kSeeds; ++s) late += late_inputs(d, static_cast<std::uint64_t>(s), slop, true);
        ok(late == 0, "drag input (every 1-8 ms, timers up to " + std::to_string(slop.count() / 1000) +
                          " us late) is on screen at the earliest refresh it can make (" + std::to_string(late) + " late, " +
                          std::to_string(kSeeds) + " seeds x 600 refreshes)");
        // the rule must be load-bearing: without it, the same drag shows input a refresh late
        int late_nohold = 0;
        for (int s = 1; s <= kSeeds; ++s) late_nohold += late_inputs(d, static_cast<std::uint64_t>(s), slop, false);
        ok(late_nohold > 0, "...and re-deciding the deadline at each wake would make " + std::to_string(late_nohold) + " late: the test bites");

        TickResult t{};
        for (int s = 1; s <= kSeeds; ++s) { const auto x = animate(d, static_cast<std::uint64_t>(s), slop, true); t.skipped += x.skipped; t.uneven += x.uneven; t.ticks += x.ticks; t.stalled += x.stalled; t.late += x.late; }
        ok(t.late == 0, "each animation frame is aimed at the nearest vblank it can still make (" + std::to_string(t.late) + " a refresh late)");
        ok(t.skipped == 0, "animation ticks skip no refresh, grid re-anchored every frame (" + std::to_string(t.skipped) + " of " + std::to_string(t.ticks) + ")");
        ok(t.stalled == 0, "...and the next tick is never booked away from the next refresh (" + std::to_string(t.stalled) + " stalls)");
        ok(t.uneven == 0, "every animation step is exactly one refresh of motion (" + std::to_string(t.uneven) + " uneven)");
        TickResult u{};
        for (int s = 1; s <= kSeeds; ++s) { const auto x = animate(d, static_cast<std::uint64_t>(s), slop, false); u.late += x.late; }
        ok(u.late > 0, "...and re-deciding from the late wake-up would aim " + std::to_string(u.late) + " a refresh late: the test bites");

        const auto g = grid_of(d);
        tp prev = g.display_time(g.slot_start(5));
        bool mono = true;
        Rng r{7};
        for (tp now = g.slot_start(5); now < g.slot_start(400); now += 100us + r.upto(900us)) {
            const tp dtime = g.display_time(now);
            if (dtime < prev || dtime <= now) mono = false;
            prev = dtime;
        }
        ok(mono, "display time is monotonic and always ahead of now");

        bool fq = true;
        for (tp now = g.slot_start(5); now < g.slot_start(60); now += 250us) {
            const tp h = g.full_queue_deadline(now);
            const auto on_grid = ((h + d.margin) - g.vblank).count() % d.period.count() == 0;
            if (h <= now || !on_grid || h - now > d.period) fq = false;
        }
        ok(fq, "a full queue holds to a latch point ahead, never presents now");

        const auto resume = pacing::tick_step<tp>(g.slot_start(10), g.slot_start(500), d.period);
        ok(resume.dt == std::chrono::duration<double>(d.period).count() && !resume.counted,
           "motion resuming after an idle steps one refresh, never a leap");
    }
    std::printf(fails ? "FAIL\n" : "PASS\n");
    return fails ? 1 : 0;
}
