// tests/test_live.cpp — streams and the live window, on jaal
//
// A live app gets data from somewhere other than the window: a socket, a
// worker thread, a poller. In mayag that is a jaal Sub::stream. jaal's own
// suite covers the stream machinery; these tests check the mayag side:
//
//   * stream messages reach update() through the headless host, in order
//   * a stream runs exactly while subscribe() returns it
//   * Sub::every starts and stops with the model
//   * a REAL window, blocked with no window activity, is woken by a stream
//     thread and folds its message promptly. This is the one that matters:
//     the old runtime had its own waker for it; now jaal's reactor does it.
//     Needs a Wayland compositor and skips cleanly without one.

#include <mayag/mayag.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stop_token>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <variant>

using namespace mayag;
using namespace std::chrono_literals;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}
void section(const char* name) { std::printf("\n%s\n", name); }

AppConfig quiet() {
    AppConfig cfg;
    cfg.log_renderer = false;
    cfg.size = {200, 120};
    return cfg;
}

// ── a finite stream ─────────────────────────────────────────────────────

struct StreamApp {
    struct Model { int received = 0; int last = 0; bool started = false; };
    struct Start {};
    struct Value { int n; };
    using Msg = std::variant<Start, Value>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    static Cmd update(Model& m, Start) { m.started = true; return {}; }
    static Cmd update(Model& m, Value v) { ++m.received; m.last = v.n; return {}; }
    static Node view(const Model&) { return dsl::box().build(); }

    static Sub subscribe(const Model& m) {
        return when(m.started, Sub::stream("count", [](jaal::Sink<Msg> out, std::stop_token st) {
            for (int i = 1; i <= 5 && !st.stop_requested(); ++i) {
                std::this_thread::sleep_for(2ms);
                (void)out.send(Value{i});
            }
        }));
    }
};

void test_stream_delivers() {
    section("Sub::stream delivers");

    run_headless<StreamApp>(quiet(), [](auto& rt) {
        rt.send(StreamApp::Start{});
        { const bool settled_ = rt.settle([](const auto& m) { return m.received == 5; });
          check(settled_, "all five streamed messages arrived (" + std::to_string(rt.model().received) + ")"); }
        check(rt.model().last == 5, "in order (last == 5)");
    });
    check(true, "the program shut the stream down without hanging");
}

// ── a stream tied to the model ──────────────────────────────────────────

std::atomic<int> g_live_producers{0};

struct SourceApp {
    struct Model { bool connected = false; int received = 0; };
    struct Connect {}; struct Disconnect {}; struct Value { int n; };
    using Msg = std::variant<Connect, Disconnect, Value>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    static Cmd update(Model& m, Connect)    { m.connected = true;  return {}; }
    static Cmd update(Model& m, Disconnect) { m.connected = false; return {}; }
    static Cmd update(Model& m, Value)      { ++m.received; return {}; }
    static Node view(const Model&) { return dsl::box().build(); }

    static Sub subscribe(const Model& m) {
        // The producer exists ONLY while connected.
        return when(m.connected, Sub::stream("feed", [](jaal::Sink<Msg> out, std::stop_token st) {
            g_live_producers.fetch_add(1);
            for (int n = 1; !st.stop_requested(); ++n) {
                std::this_thread::sleep_for(2ms);
                (void)out.send(Value{n});
            }
            g_live_producers.fetch_sub(1);
        }));
    }
};

void test_stream_lifecycle() {
    section("Sub::stream lifecycle");

    run_headless<SourceApp>(quiet(), [](auto& rt) {
        rt.tick();
        check(g_live_producers.load() == 0, "no producer runs before the model asks for it");

        rt.send(SourceApp::Connect{});
        { const bool settled_ = rt.settle([](const auto& m) { return m.received >= 3; });
          check(settled_, "a connected stream delivers (" + std::to_string(rt.model().received) + ")"); }
        check(g_live_producers.load() == 1, "exactly one producer runs while connected");

        rt.send(SourceApp::Disconnect{});
        const auto end = std::chrono::steady_clock::now() + 1s;
        while (g_live_producers.load() != 0 && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        check(g_live_producers.load() == 0, "the producer stops when the model disconnects");

        const int frozen = rt.model().received;
        (void)rt.settle([](const auto&) { return false; }, 30ms);
        check(rt.model().received == frozen, "and nothing it sent afterwards reaches update()");
    });
}

// ── Sub::every ──────────────────────────────────────────────────────────

struct EveryApp {
    struct Model { int ticks = 0; bool running = true; };
    struct Tick {}; struct Stop {};
    using Msg = std::variant<Tick, Stop>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;
    static Cmd update(Model& m, Tick) { ++m.ticks; return {}; }
    static Cmd update(Model& m, Stop) { m.running = false; return {}; }
    static Node view(const Model&) { return dsl::box().build(); }
    static Sub subscribe(const Model& m) { return when(m.running, Sub::every(10ms, Tick{})); }
};

void test_every_reconciliation() {
    section("Sub::every reconciliation");

    run_headless<EveryApp>(quiet(), [](auto& rt) {
        { const bool settled_ = rt.settle([](const auto& m) { return m.ticks >= 3; }, 1s);
          check(settled_, "an active Sub::every fires (" + std::to_string(rt.model().ticks) + " ticks)"); }
        rt.send(EveryApp::Stop{});
        const int base = rt.model().ticks;
        (void)rt.settle([](const auto&) { return false; }, 60ms);
        check(rt.model().ticks == base, "it stops when its condition goes false");
    });
}

// ── a window with no fd (Cocoa) is polled, but idle still draws nothing ─

struct IdleApp {
    struct Model {};
    struct Done {};
    using Msg = std::variant<Done>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd init(Model&) { return Cmd::after(300ms, Done{}); }
    static Cmd update(Model&, Done) { return Cmd::quit(); }
    static Node view(const Model&) { return dsl::box().build(); }
};

void test_fdless_window_idles() {
    section("fd-less window");

    // The headless window has no fd(), like Cocoa, so the real jaal loop
    // polls it every few ms. Those polls must not turn into frames.
    AppConfig cfg = quiet();
    auto win = platform::Headless::open(detail::window_config(cfg));
    window_host<IdleApp, platform::Headless> host{*win, cfg};
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = jaal::run<IdleApp>(host);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    check(rc == 0, "ran for " + std::to_string(ms) + " ms and quit");
    check(win->frames_presented() <= 3,
          "polling an fd-less window doesn't draw (" +
              std::to_string(win->frames_presented()) + " frames in 300 ms)");
}

// ── end to end: a real, idle window is woken by a stream ────────────────

using clk = std::chrono::steady_clock;
std::atomic<long long> g_wake_ms{-1};
std::atomic<long long> g_loop_cpu_us{-1};
std::atomic<bool>      g_timed_out{false};
clk::time_point        g_sent_at{};
long long              g_cpu_at_init = 0;

// CPU time of the calling thread. update() and init() run on the loop
// thread, so the difference is what the loop burned while it waited.
long long thread_cpu_us() {
    rusage u{};
    ::getrusage(RUSAGE_THREAD, &u);
    return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1'000'000LL +
           u.ru_utime.tv_usec + u.ru_stime.tv_usec;
}

struct WakeApp {
    struct Model {};
    struct Ping {}; struct Timeout {};
    using Msg = std::variant<Ping, Timeout>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    // A backstop so a broken wakeup fails instead of hanging the suite. It
    // is far later than the ping, so it can't be what wakes the loop.
    static Cmd init(Model&) { g_cpu_at_init = thread_cpu_us(); return Cmd::after(3000ms, Timeout{}); }

    static Cmd update(Model&, Ping) {
        g_wake_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - g_sent_at).count();
        g_loop_cpu_us = thread_cpu_us() - g_cpu_at_init;
        return Cmd::quit();
    }
    static Cmd update(Model&, Timeout) { g_timed_out = true; return Cmd::quit(); }
    static Node view(const Model&) { return dsl::box().build(); }

    static Sub subscribe(const Model&) {
        return Sub::stream("ping", [](jaal::Sink<Msg> out, std::stop_token st) {
            // Long enough for the window to map and the loop to go to sleep.
            for (int i = 0; i < 40 && !st.stop_requested(); ++i) std::this_thread::sleep_for(5ms);
            g_sent_at = clk::now();
            (void)out.send(Ping{});
        });
    }
};

void test_idle_window_wakes() {
    section("live window wakeup");

    const char* disp = std::getenv("WAYLAND_DISPLAY");
    if (disp == nullptr || *disp == '\0') {
        std::printf("  (no compositor — skipped)\n");
        return;
    }

    AppConfig cfg = quiet();
    cfg.title = "mayag live test";
    const int rc = mayag::run<WakeApp>(cfg);
    check(rc == 0, "the program ran and quit cleanly (exit " + std::to_string(rc) + ")");
    check(!g_timed_out.load(), "the stream's message arrived before the backstop");
    const long long ms = g_wake_ms.load();
    std::printf("  (folded %lld ms after the send)\n", ms);
    check(ms >= 0 && ms < 100,
          "an idle window is woken by a background thread, not left asleep");
    // ~200 ms passed between init and the ping. A loop that spun instead of
    // sleeping would have burned most of it.
    const long long cpu = g_loop_cpu_us.load();
    std::printf("  (loop thread used %lld us of CPU over ~200 ms)\n", cpu);
    check(cpu >= 0 && cpu < 60'000, "and the loop was asleep while it waited");
}

}  // namespace

int main() {
    std::printf("mayag live / streaming\n======================\n");
    test_stream_delivers();
    test_stream_lifecycle();
    test_every_reconciliation();
    test_fdless_window_idles();
    test_idle_window_wakes();
    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
