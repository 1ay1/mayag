// tests/test_platform_shared.cpp — what every window backend shares.
//
// platform/keys.hpp (a character key is what the layout types) and
// platform/photon.hpp (input to photon) are used by the Wayland AND the
// Cocoa window, so they are tested here, headless, on every platform --
// not only on the machine that has the backend.

#include <mayag/platform/keys.hpp>
#include <mayag/platform/photon.hpp>
#include <mayag/platform/compositor_queue.hpp>

#include <cmath>
#include <cstdio>

using namespace mayag;
using namespace mayag::platform;

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("  FAIL  %s\n", what);
    }
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

void test_keys() {
    check(char_key_of('a') == Key::a && char_key_of('Z') == Key::z, "letters either case");
    check(char_key_of('7') == Key::n7 && char_key_of('/') == Key::slash, "digits and punctuation");
    check(char_key_of(0x0436) == Key::unknown, "a Cyrillic letter is no character Key");
    check(layout_key(Key::q, 'a') == Key::a, "AZERTY: the key at Q's place types a, so it is A");
    check(layout_key(Key::z, 'w') == Key::w, "AZERTY: the key at Z's place is W");
    check(layout_key(Key::c, 0x0441) == Key::c, "Russian: c's key types \u0441, keeps its position (ctrl+c copies)");
    check(layout_key(Key::a, 0) == Key::a, "nothing typed (dead key, no layout): by position");
    check(layout_key(Key::escape, 'a') == Key::a, "a virtual keyboard's `a` on any key is A");
    check(layout_key(Key::enter, 0) == Key::enter, "named keys keep their position");
    check(is_char_key(Key::space) && is_char_key(Key::grave) && !is_char_key(Key::enter), "is_char_key");
}

void test_compositor_queue() {
    constexpr double P = 0.010;   // 100 Hz, vblanks at whole 10 ms
    {   // fullscreen (depth 1): a commit is taken for the next vblank -- a second one would wait behind it
        CompositorQueue q;
        q.presented(q.committed(0.995), 1.000, P);   // the grid: a vblank at 1.000
        for (int i = 1; i <= 8; ++i) {               // frames committed mid-refresh, shown at the next vblank
            const double v = 1.000 + i * P;
            q.presented(q.committed(v - 0.006), v, P);
        }
        check(q.depth() == 1, "shown at the first vblank after the commit: depth 1");
        const double v = 1.090;
        (void)q.committed(v + 0.002);   // an input's frame, 2 ms into the refresh
        check(q.full(v + 0.004), "the compositor holds it for the vblank: a commit now waits a refresh");
        q.presented(q.latch_misses().first + 10, v + P, P);   // (another ticket: nothing)
        check(!q.full(v + P + 0.001), "next refresh: the held frame has flipped, nothing waits");
    }
    {   // windowed (depth 2): the compositor latches the newest commit at the vblank -- no hold
        CompositorQueue q;
        q.presented(q.committed(0.995), 1.000, P);
        for (int i = 1; i <= 8; ++i) {
            const double v = 1.000 + i * P;
            q.presented(q.committed(v - P - 0.004), v, P);   // committed a refresh and a bit before
        }
        check(q.depth() == 2, "shown two vblanks on: depth 2");
        (void)q.committed(1.092);
        check(!q.full(1.095), "a deeper compositor replaces: another commit this refresh is fine");
        check(q.full(1.110 + 0.003), "...but a frame past the vblank it was due at is overdue: hold");
    }
    {   // the margin learns the deadline from the miss RATE of frames aimed at it, and only from those
        CompositorQueue q;
        q.presented(q.committed(0.995), 1.000, P);
        for (int i = 1; i <= 8; ++i) q.presented(q.committed(1.000 + i * P - 0.006), 1.000 + i * P, P);
        const double m0 = q.margin();
        double v = 1.090;
        // one aimed frame late: a hiccup (Hyprland: 10-20% at every phase), not a deadline
        q.presented(q.committed(v + P - m0), v + 2 * P, P);
        check(q.margin() == m0, "one late aimed frame does not move the margin");
        // a quarter of them late: the deadline is earlier than the margin
        for (int i = 0; i < 3; ++i) { v += 2 * P; q.presented(q.committed(v + P - m0), v + 2 * P, P); }
        check(q.margin() > m0, "a quarter of aimed frames late widens the margin");
        const double m1 = q.margin();
        // drawn mid-refresh for an input and late anyway: teaches nothing
        for (int i = 0; i < 8; ++i) { v += 2 * P; q.presented(q.committed(v + 0.002), v + 2 * P, P); }
        check(q.margin() == m1, "mid-refresh misses teach nothing");
        // half of them late, forever: the margin stays bounded (it once ran away to 11 ms). (Always
        // late by exactly one refresh would be a deeper compositor, relearned as depth 2.)
        for (int i = 0; i < 400; ++i) {
            v += 2 * P;
            q.presented(q.committed(v + P - q.margin()), v + (i % 2 ? 2 : 1) * P, P);
        }
        check(q.depth() == 1 && q.margin() > m1, "a high miss rate widens the margin");
        check(q.margin() <= CompositorQueue::kMaxMargin + 1e-9, "the margin is bounded");
        // a clean stretch brings it back down
        const double m2 = q.margin();
        for (int i = 0; i < 40; ++i) { v += P; q.presented(q.committed(v + P - q.margin()), v + P, P); }
        check(q.margin() < m2, "aimed frames all on time narrow it again");
    }
    {   // a frame queued behind one the compositor already took teaches nothing
        CompositorQueue q;
        q.presented(q.committed(0.995), 1.000, P);
        for (int i = 1; i <= 8; ++i) q.presented(q.committed(1.000 + i * P - 0.006), 1.000 + i * P, P);
        const double m0 = q.margin();
        const auto a = q.committed(1.082);   // taken for 1.090
        const auto b = q.committed(1.090 - m0);   // at the latch point, but behind a
        q.presented(a, 1.090, P);
        q.presented(b, 1.100, P);
        check(q.margin() == m0, "lateness behind our own frame is the queue's, not the deadline's");
    }
    {   // no feedback yet (no grid): never claims a full queue
        CompositorQueue q;
        (void)q.committed(1.0);
        check(!q.full(1.001), "before any presentation nothing is known: no hold");
    }
}

void test_photon() {
    {   // the oldest unshown input is measured, through to its frame
        PhotonMeter m;
        m.presented(PhotonMeter::Frame{}, 0.5);   // a window already on screen
        m.note(1.000, 1.001);
        m.note(1.005, 1.006);   // a later input does not overwrite it
        const auto f = m.take(1.002);
        m.presented(f, 1.010);
        const auto s = m.stats();
        check(s.samples == 1 && near(s.last_ms, 10.0), "oldest unshown input to its frame: 10 ms");
        check(near(s.stage_ms[0], 1.0) && near(s.stage_ms[1], 1.0) && near(s.stage_ms[2], 8.0),
              "stages: to us 1, in us 1, compositor 8");
        check(!m.has_unshown(), "shown: nothing waits");
    }
    {   // an input from before the window's first frame waited for STARTUP: not a sample
        PhotonMeter m;
        m.note(0.900, 0.950);                  // pressed while the app was still opening
        m.presented(m.take(1.100), 1.390);     // the first frame: carries it
        check(m.stats().samples == 0, "a key pressed during startup is not an input-to-photon sample");
        m.note(1.500, 1.501);
        m.presented(m.take(1.502), 1.510);
        check(m.stats().samples == 1 && near(m.stats().last_ms, 10.0), "...and the next key is measured as usual");
    }
    {   // a replaced frame does not take the sample with it
        PhotonMeter m;
        m.presented(PhotonMeter::Frame{}, 0.5);   // a window already on screen
        m.note(2.000, 2.000);
        const auto dropped = m.take(2.001);
        (void)dropped;   // replaced before scanout: never presented
        const auto shown = m.take(2.004);
        m.presented(shown, 2.016);
        check(m.stats().samples == 1 && near(m.stats().last_ms, 16.0), "the next frame carries a replaced frame's input");
    }
    {   // an input shown once is not measured again
        PhotonMeter m;
        m.presented(PhotonMeter::Frame{}, 0.5);   // a window already on screen
        m.note(3.0, 3.0);
        const auto a = m.take(3.001), b = m.take(3.002);
        m.presented(a, 3.010);
        m.presented(b, 3.020);
        check(m.stats().samples == 1, "the first frame to show an input is its sample");
    }
    {   // pointer motion nothing answered does not time a LATER frame (the 260 ms "stalls")
        PhotonMeter m;
        m.presented(PhotonMeter::Frame{}, 0.5);   // a window already on screen
        m.note(4.000, 4.000, true);   // the pointer moved over text: no frame owed
        m.settle();                   // the host drew nothing for it
        check(!m.has_unshown(), "unanswered motion is forgotten");
        m.note(4.250, 4.250);         // a key, 250 ms later
        m.presented(m.take(4.253), 4.260);
        check(m.stats().samples == 1 && near(m.stats().last_ms, 10.0), "the key is timed from the key, not the motion");
    }
    {   // ...but a key whose answer comes later (a pty's echo) is still waiting
        PhotonMeter m;
        m.presented(PhotonMeter::Frame{}, 0.5);
        m.note(5.000, 5.000);
        m.settle();   // nothing drawn yet: the shell hasn't echoed
        check(m.has_unshown(), "a key survives settle: the echo is its answer");
        m.presented(m.take(5.004), 5.012);
        check(near(m.stats().last_ms, 12.0), "timed from the key to the echo's frame");
    }
    {   // a key after motion that has not been settled yet is the input measured
        PhotonMeter m;
        m.presented(PhotonMeter::Frame{}, 0.5);
        m.note(6.000, 6.000, true);
        m.note(6.100, 6.100);   // no frame carried the motion: the key is what is answered
        m.presented(m.take(6.102), 6.110);
        check(near(m.stats().last_ms, 10.0), "uncarried motion yields to the key");
    }
    {   // motion a committed frame carries is kept: it is that frame's input
        PhotonMeter m;
        m.presented(PhotonMeter::Frame{}, 0.5);
        m.note(7.000, 7.000, true);
        const auto f = m.take(7.002);   // a hover redraw, committed
        m.settle();                     // nothing more is owed
        m.note(7.003, 7.003);           // a key before the hover frame shows
        m.presented(f, 7.012);
        check(m.stats().samples == 1 && near(m.stats().last_ms, 12.0), "a drawn hover is measured from its motion");
    }
    {   // percentiles
        PhotonMeter m;
        m.presented(PhotonMeter::Frame{}, 0.5);   // a window already on screen
        for (int i = 1; i <= 100; ++i) {
            const double t = i * 1.0;
            m.note(t, t);
            m.presented(m.take(t), t + i * 1e-3);
        }
        const auto s = m.stats();
        check(near(s.best_ms, 1.0) && near(s.worst_ms, 100.0), "best and worst");
        check(std::fabs(s.p50_ms - 51.0) < 0.01 && std::fabs(s.p95_ms - 95.0) < 0.01, "p50 and p95 (nearest rank)");
    }
    {   // what starts the clock
        check(!starts_photon_clock(Event{KeyEvent{Key::ctrl, {}, false}}), "a bare ctrl starts nothing");
        check(!starts_photon_clock(Event{KeyEvent{Key::shift, {}, false}}), "a bare shift starts nothing");
        check(starts_photon_clock(Event{KeyEvent{Key::a, {}, false}}), "a key does");
        check(!starts_photon_clock(Event{ResizeEvent{}}), "a resize does not");
    }
}

}  // namespace

int main() {
    test_keys();
    test_photon();
    test_compositor_queue();
    std::printf("platform shared: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
