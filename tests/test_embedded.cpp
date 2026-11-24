// tests/test_embedded.cpp — a program driven by a loop mayag doesn't own
//
// On macOS the Cocoa run loop owns the main thread and calls into mayag
// (embedded_app). This drives the same code from a minimal fake run loop on
// Linux, with the same rules the Cocoa glue follows:
//
//   wake()      any thread: signal the loop (CFRunLoopSourceSignal)
//   arm(d)      a one-shot timer (CFRunLoopTimer)
//   frames(on)  start/stop a display link (CADisplayLink), 60 Hz here
//
// and checks: idle means the loop sleeps (no turns, no draws); a background
// thread's message is folded at once; timers fire; animation runs on the
// display link and stops it when it settles; quit leaves the loop.

#include <mayag/mayag.hpp>
#include <mayag/widget/agent_state.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>

using namespace mayag;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

namespace {

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}
void section(const char* s) { std::printf("\n%s\n", s); }

/// The fake run loop: one thread, sleeps until woken, a timer, or a vsync.
struct FakeLoop {
    std::mutex              m;
    std::condition_variable cv;
    bool                    signalled = false;   // the wake source
    std::optional<clk::time_point> timer;
    bool                    link_on = false;
    clk::time_point         next_vsync{};
    bool                    quit = false;
    int                     link_starts = 0;
    int                     wakes_from_other_threads = 0;

    loop_hooks hooks() {
        loop_hooks h;
        h.wake = [this] {
            std::lock_guard lk{m};
            if (std::this_thread::get_id() != loop_thread) ++wakes_from_other_threads;
            signalled = true;
            cv.notify_one();
        };
        h.arm = [this](std::optional<std::chrono::nanoseconds> d) {
            timer = d ? std::optional(clk::now() + *d) : std::nullopt;
        };
        h.frames = [this](bool on) {
            if (on && !link_on) { ++link_starts; next_vsync = clk::now() + 16ms; }
            link_on = on;
        };
        h.stopped = [this] { quit = true; };
        return h;
    }

    /// Run until quit or `budget` passes. Each wakeup is one callback.
    template <class App>
    void run(App& app, std::chrono::milliseconds budget) {
        const auto end = clk::now() + budget;
        auto last_vsync = clk::now();
        while (!quit && clk::now() < end) {
            enum { wake, tick, vsync, none } what = none;
            {
                std::unique_lock lk{m};
                auto until = end;
                if (timer && *timer < until) until = *timer;
                if (link_on && next_vsync < until) until = next_vsync;
                cv.wait_until(lk, until, [&] { return signalled; });
                const auto now = clk::now();
                if (signalled) { signalled = false; what = wake; }
                else if (link_on && now >= next_vsync) what = vsync;
                else if (timer && now >= *timer) {
                    lateness_ms.push_back(std::chrono::duration<double, std::milli>(now - *timer).count());
                    timer.reset();
                    what = tick;
                }
            }
            if (what == wake || what == tick) app.turn();
            if (what == vsync) {
                const auto now = clk::now();
                next_vsync += 16ms;
                app.vsync(std::chrono::duration<double>(now - last_vsync).count());
                last_vsync = now;
            }
        }
    }

    std::thread::id loop_thread = std::this_thread::get_id();
    std::vector<double> lateness_ms;   ///< how late each armed timer actually fired (the OS's share)
};

// ── a program with every kind of wake source ────────────────────────────

std::atomic<long long> g_fold_ms{-1};
clk::time_point        g_sent{};

struct App {
    struct Model {
        int  pings = 0;
        int  ticks = 0;
        bool timer_armed = false;
        bool timer_fired = false;
        bool animating = false;
        double t = 0.0;
        bool streaming = false;
    };
    struct Ping {}; struct Timer {}; struct Animate {}; struct Frame { double dt; };
    struct Stream {}; struct Quit {};
    using Msg = std::variant<Ping, Timer, Animate, Frame, Stream, Quit>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    static Cmd update(Model& m, Ping) {
        ++m.pings;
        g_fold_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - g_sent).count();
        m.streaming = false;
        return {};
    }
    static Cmd update(Model& m, Timer)   {
        if (!m.timer_armed) { m.timer_armed = true; return Cmd::after(40ms, Timer{}); }
        m.timer_fired = true;           // the delayed one
        return {};
    }
    static Cmd update(Model& m, Animate) { m.animating = true; m.t = 0; return {}; }
    static Cmd update(Model& m, Frame f) {
        ++m.ticks;
        m.t += f.dt;
        if (m.t >= 0.25) m.animating = false;
        return {};
    }
    static Cmd update(Model& m, Stream)  { m.streaming = true; return {}; }
    static Cmd update(Model&, Quit)      { return Cmd::quit(3); }
    static Node view(const Model&) { return dsl::box().build(); }

    // Input, as the Cocoa glue delivers it: an event, then turn().
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{{Key::s, Stream{}}, {Key::t, Timer{}},
                                      {Key::a, Animate{}}, {Key::q, Quit{}}};
        return keys(k);
    }

    static Sub subscribe(const Model& m) {
        return Sub::batch(
            when(m.animating, Sub::on_frame([](double dt) { return Frame{dt}; })),
            when(m.streaming, Sub::stream("ping", [](mayag::Sink<Msg> out, std::stop_token st) {
                for (int i = 0; i < 10 && !st.stop_requested(); ++i) std::this_thread::sleep_for(5ms);
                g_sent = clk::now();
                (void)out.send(Ping{});
            })));
    }
};

AppConfig quiet() {
    AppConfig cfg;
    cfg.log_renderer = false;
    cfg.size = {120, 80};
    return cfg;
}

/// A program showing ONLY a spinner: no subscription, no message. Its view
/// books its own next frame (stepper -> request_frame_at). The view records
/// each glyph it draws and when, so the test sees the cadence the user sees.
struct Glyph { double at; std::string g; };
std::vector<Glyph> g_drawn;
struct Ticker {
    struct Model {};
    struct Nothing {};
    using Msg = std::variant<Nothing>;
    using Cmd = mayag::Cmd<Msg>;
    static Cmd update(Model&, Nothing) { return {}; }
    static Node view(const Model&) {
        const std::string g = dsl::spinner_frame(ctx().time);
        const double now = std::chrono::duration<double>(clk::now().time_since_epoch()).count();
        if (g_drawn.empty() || g_drawn.back().g != g) g_drawn.push_back({now, g});
        return (dsl::text_owned(g) | dsl::font(14)).build();
    }
};

}  // namespace

int main() {
    std::printf("mayag embedded driver\n=====================\n");

    AppConfig cfg = quiet();
    auto win = platform::Headless::open(detail::window_config(cfg));
    FakeLoop loop;
    embedded_app<App, platform::Headless> app{*win, cfg, loop.hooks()};

    section("idle");
    app.turn();   // the loop's first call
    const auto t0 = app.turns(), d0 = app.draws();
    loop.run(app, 150ms);
    check(app.draws() == d0 && d0 >= 1, "an idle program draws its first frame and then nothing");
    check(app.turns() == t0, "and the loop is never woken while idle (" +
                                 std::to_string(app.turns() - t0) + " turns)");
    check(!loop.link_on && loop.link_starts == 0, "the display link stays off");

    const auto press = [&](Key k) { win->push(KeyEvent{k, {}, false}); app.turn(); };

    section("a background thread wakes the loop");
    press(Key::s);                           // starts a stream that sends in ~50 ms
    const int wakes_before = loop.wakes_from_other_threads;
    loop.run(app, 1000ms);                   // returns early? no: runs the budget
    check(app.model().pings == 1, "the stream's message was folded");
    check(loop.wakes_from_other_threads > wakes_before, "through wake() from the worker thread");
    check(g_fold_ms.load() >= 0 && g_fold_ms.load() < 20,
          "and promptly (" + std::to_string(g_fold_ms.load()) + " ms after the send)");

    section("timers fire through arm()");
    press(Key::t);
    check(app.model().timer_armed && !app.model().timer_fired, "the key armed a 40 ms timer");
    const auto td = app.draws();
    loop.run(app, 150ms);
    check(app.model().timer_fired, "the timer fired through arm()");
    check(app.draws() > td, "and its message was folded and drawn");

    section("animation runs on the display link");
    press(Key::a);
    check(loop.link_on, "on_frame starts the display link");
    const auto ad = app.draws();
    loop.run(app, 600ms);
    const int ticks = app.model().ticks;
    check(ticks >= 12 && ticks <= 20,
          "one tick per refresh for 0.25 s (" + std::to_string(ticks) + " ticks)");
    check(static_cast<int>(app.draws() - ad) <= ticks + 2,
          "one draw per tick (" + std::to_string(app.draws() - ad) + " draws)");
    check(!app.model().animating && !loop.link_on,
          "the animation ends and stops the display link");
    const auto idle_turns = app.turns();
    loop.run(app, 100ms);
    check(app.turns() == idle_turns, "then the loop sleeps again");

    section("quit");
    press(Key::q);
    check(app.finished() && loop.quit, "Cmd::quit tells the loop to stop");
    check(app.exit_code() == 3, "with the program's exit code");

    section("a spinner alone keeps its own time (the Cocoa path)");
    {   // On macOS the run loop is external: a view's booked frame must arm
        // the loop's timer itself. It did not -- wait_hint returned early for
        // an external loop -- so a spinner with nothing else animating
        // advanced only when something unrelated woke the loop (the blink,
        // the 1 Hz clock, output): the tab spinner "lags".
        auto tw = platform::Headless::open(detail::window_config(cfg));
        FakeLoop tl;
        embedded_app<Ticker, platform::Headless> ticker{*tw, cfg, tl.hooks()};
        ticker.turn();
        // the deterministic part, what was broken: after a frame, the loop's
        // timer is armed for the spinner's next step (~83 ms ahead), not left
        // unset ("sleep until something else happens")
        const auto armed_in = tl.timer ? std::chrono::duration<double, std::milli>(*tl.timer - clk::now()).count() : -1.0;
        check(armed_in > 0.0 && armed_in < 90.0, "a drawn spinner arms the loop's timer for its next step (in " +
                                                   std::to_string(static_cast<int>(armed_in)) + " ms)");
        g_drawn.clear();
        tl.run(ticker, 1000ms);
        double worst = 0;
        for (std::size_t i = 1; i < g_drawn.size(); ++i) worst = std::max(worst, g_drawn[i].at - g_drawn[i - 1].at);
        // the timing part: loose, since a parallel ctest descheduling this
        // thread is not the code (the bug drew NO glyph in the whole second)
        check(g_drawn.size() >= 6, "it turns at its own rate with nothing else running (" +
                                       std::to_string(g_drawn.size()) + " glyphs in 1 s, ~12 expected)");
        check(worst < 0.4, "and never stalls: the longest gap between glyphs is " + std::to_string(static_cast<int>(worst * 1000)) +
                               " ms (a step is 83 ms)");
        check(tl.link_starts == 0, "on its own timer, without running the display link");
        if (!tl.lateness_ms.empty()) {
            auto l = tl.lateness_ms;
            std::sort(l.begin(), l.end());
            std::printf("        timer lateness: p50 %.2f ms  max %.2f ms over %zu wakes\n", l[l.size() / 2], l.back(), l.size());
        }
    }

    std::printf("\n%s  %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
