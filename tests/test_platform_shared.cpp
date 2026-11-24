// tests/test_platform_shared.cpp — what every window backend shares.
//
// platform/keys.hpp (a character key is what the layout types) and
// platform/photon.hpp (input to photon) are used by the Wayland AND the
// Cocoa window, so they are tested here, headless, on every platform --
// not only on the machine that has the backend.

#include <mayag/platform/keys.hpp>
#include <mayag/platform/photon.hpp>

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
    std::printf("platform shared: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
