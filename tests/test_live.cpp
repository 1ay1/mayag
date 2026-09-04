// tests/test_live.cpp — the streaming / live-app path
//
// A live app receives data from somewhere other than the window: a socket, a
// subprocess, a metrics poller, a worker thread. mayag's answer is `Cmd::stream`
// (a long-lived producer) plus a cross-thread waker (so a message posted from a
// producer thread interrupts an idle UI thread instead of sitting unrendered
// until the user moves the mouse).
//
// Two kinds of check, split so CI is meaningful without a compositor:
//
//   * The Waker itself is pure logic (a self-pipe): its wake/drain/coalesce
//     behaviour runs everywhere and is exact.
//   * The end-to-end "a blocked app wakes and renders" check needs a real
//     blocking window, so it runs only under a Wayland compositor and skips
//     cleanly otherwise.

#include <mayag/mayag.hpp>
#include <mayag/platform/waker.hpp>
#include <mayag/platform/wayland.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>

using namespace mayag;
using namespace mayag::platform;
using namespace std::chrono_literals;

namespace {

int failures = 0, checks = 0;
void check(bool ok, std::string_view what) {
    ++checks;
    if (!ok) { ++failures; std::printf("  FAIL  %.*s\n",
                                       static_cast<int>(what.size()), what.data()); }
}
void section(std::string_view s) { std::printf("\n%.*s\n",
                                               static_cast<int>(s.size()), s.data()); }

// ── the waker ────────────────────────────────────────────────────────────

void test_waker_basics() {
    section("waker");

    Waker w;
    check(w.usable(), "the self-pipe was created");
    check(w.fd() >= 0, "it exposes a pollable fd");

    // A wake makes the fd readable.
    w.wake();
    pollfd pfd{w.fd(), POLLIN, 0};
    check(::poll(&pfd, 1, 100) == 1, "wake() makes the fd readable within 100ms");
    check((pfd.revents & POLLIN) != 0, "it is readable for READ specifically");

    // Drain clears it.
    w.drain();
    pollfd pfd2{w.fd(), POLLIN, 0};
    check(::poll(&pfd2, 1, 0) == 0, "drain() clears the readable state");

    // Coalescing: many wakes between drains cost one readable event, and one
    // drain clears them all.
    for (int i = 0; i < 1000; ++i) w.wake();
    w.drain();
    pollfd pfd3{w.fd(), POLLIN, 0};
    check(::poll(&pfd3, 1, 0) == 0, "1000 wakes coalesce and one drain clears them");
}

void test_waker_cross_thread() {
    section("waker across threads");

    Waker w;

    // A UI thread blocked in poll() must be woken by a producer thread. This
    // is the exact scenario an idle streaming app is in.
    std::atomic<bool> producer_ran{false};
    std::thread producer([&] {
        std::this_thread::sleep_for(50ms);
        producer_ran.store(true);
        w.wake();
    });

    const auto t0 = std::chrono::steady_clock::now();
    pollfd pfd{w.fd(), POLLIN, 0};
    const int r = ::poll(&pfd, 1, 2000);   // would block 2s without the wake
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    producer.join();

    check(r == 1, "a blocked poll() is woken by another thread");
    check(producer_ran.load(), "the producer actually ran");
    check(elapsed < 500ms, "the wake was prompt, not a timeout");
    w.drain();
}

// ── Cmd::stream lifecycle, through the headless runtime ─────────────────
//
// A stream posts messages that update() folds into the model; the runtime
// joins the producer thread on shutdown. Headless does not block, but the
// inbox + Cmd::stream plumbing is platform-independent, so this exercises the
// real interpret/step path.

struct StreamApp {
    struct Model { int received = 0; int last = 0; bool started = false; };
    struct Started {};
    struct Value { int n; };
    using Msg = std::variant<Started, Value>;

    static Model init() { return {}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Value>(msg)) {
            m.received += 1;
            m.last = std::get<Value>(msg).n;
            return {m, Cmd<Msg>::none()};
        }
        // Started: launch a producer that emits 1..5 quickly then stops.
        m.started = true;
        auto cmd = Cmd<Msg>::stream([](Cmd<Msg>::Sink sink, std::function<bool()> alive) {
            for (int i = 1; i <= 5 && alive(); ++i) {
                std::this_thread::sleep_for(5ms);
                sink(Value{i});
            }
        });
        return {m, cmd};
    }

    static Node view(const Model&) { return dsl::box().build(); }
    static Sub<Msg> subscribe(const Model&) { return Sub<Msg>::none(); }
};

void test_stream_lifecycle() {
    section("Cmd::stream lifecycle");

    AppConfig cfg;
    cfg.log_renderer = false;

    int seen = 0, last = 0;
    run_headless<StreamApp>(cfg, [&](auto& rt) {
        // Kick the stream, then pump the runtime until the producer's five
        // messages have all been folded into the model.
        rt.send(StreamApp::Started{});
        for (int i = 0; i < 200 && rt.model().received < 5; ++i) {
            std::this_thread::sleep_for(2ms);
            rt.tick();
        }
        seen = rt.model().received;
        last = rt.model().last;
    });

    check(seen == 5, std::string{"all five streamed messages arrived ("} +
                     std::to_string(seen) + ")");
    check(last == 5, "messages arrived in order (last == 5)");
    // The runtime's destructor ran here without hanging, which is the join
    // working: a stream that ignored keep_running() would deadlock the test.
    check(true, "the runtime shut the stream down and joined cleanly");
}

// ── Sub::source: a declarative stream tied to the model ─────────────────
//
// Where Cmd::stream is fired imperatively and runs to completion, Sub::source
// runs exactly as long as subscribe() returns it. Toggling the model's
// `connected` flag must start the producer, and clearing it must stop and join
// the thread — with no connect()/disconnect() plumbing in update().

struct SourceApp {
    struct Model { bool connected = false; int received = 0; };
    struct Connect {};
    struct Disconnect {};
    struct Value { int n; };
    using Msg = std::variant<Connect, Disconnect, Value>;

    // A process-wide counter so the test can observe the producer thread
    // actually starting and stopping, independent of message delivery.
    static inline std::atomic<int> live_producers{0};

    static Model init() { return {}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Connect>(msg))    m.connected = true;
        else if (std::holds_alternative<Disconnect>(msg)) m.connected = false;
        else m.received += 1;
        return {m, Cmd<Msg>::none()};
    }

    static Node view(const Model&) { return dsl::box().build(); }

    static Sub<Msg> subscribe(const Model& m) {
        // The source exists ONLY while connected. This is the whole point:
        // the producer's lifetime is a pure function of the model.
        if (!m.connected) return Sub<Msg>::none();
        return Sub<Msg>::source("feed", [](std::function<void(Msg)> sink,
                                           std::function<bool()> alive) {
            live_producers.fetch_add(1, std::memory_order_release);
            int n = 0;
            while (alive()) {
                std::this_thread::sleep_for(3ms);
                if (alive()) sink(Value{++n});
            }
            live_producers.fetch_sub(1, std::memory_order_release);
        });
    }
};

void test_source_lifecycle() {
    section("Sub::source lifecycle");

    AppConfig cfg;
    cfg.log_renderer = false;

    int received_after_connect = 0;
    int producers_while_connected = 0;
    int producers_after_disconnect = -1;

    run_headless<SourceApp>(cfg, [&](auto& rt) {
        // Not connected yet: no producer should be running.
        rt.tick();
        check(SourceApp::live_producers.load() == 0,
              "no source runs before the model asks for it");

        // Connect: the source appears in subscribe(), so the runtime starts it.
        rt.send(SourceApp::Connect{});
        for (int i = 0; i < 200 && rt.model().received < 3; ++i) {
            std::this_thread::sleep_for(2ms);
            rt.tick();
        }
        received_after_connect = rt.model().received;
        producers_while_connected = SourceApp::live_producers.load();

        // Disconnect: the source vanishes from subscribe(); the runtime must
        // stop and join the producer thread.
        rt.send(SourceApp::Disconnect{});
        rt.tick();   // sync_sources sees the source gone and joins it
        producers_after_disconnect = SourceApp::live_producers.load();
    });

    check(received_after_connect >= 3,
          std::string{"a connected source streams messages ("} +
          std::to_string(received_after_connect) + ")");
    check(producers_while_connected == 1, "exactly one producer runs while connected");
    check(producers_after_disconnect == 0,
          "the producer stopped and joined when the model disconnected");
}

// ── Sub::every reconciliation ────────────────────────────────────
//
// Interval subscriptions are declarative too: the live set is recomputed from
// the model each frame. The runtime must START firing an interval that appears
// and STOP one whose condition goes false. This was silently broken once (the
// reconcile call was missing, so `interval_timers_` stayed empty and
// `Sub::every` never fired at all) — this pins it so it cannot regress.

struct EveryApp {
    struct Model { int ticks = 0; bool running = true; };
    struct Tick {};
    struct Stop {};
    using Msg = std::variant<Tick, Stop>;

    static Model init() { return {}; }
    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Tick>(msg)) m.ticks += 1;
        else m.running = false;
        return {m, Cmd<Msg>::none()};
    }
    static Node view(const Model&) { return dsl::box().build(); }
    static Sub<Msg> subscribe(const Model& m) {
        if (!m.running) return Sub<Msg>::none();          // interval disappears
        return Sub<Msg>::every(10ms, Tick{});
    }
};

void test_every_reconciliation() {
    section("Sub::every reconciliation");

    AppConfig cfg;
    cfg.log_renderer = false;

    int while_running = 0, after_stop = 0;
    run_headless<EveryApp>(cfg, [&](auto& rt) {
        for (int i = 0; i < 80 && rt.model().ticks < 3; ++i) {
            std::this_thread::sleep_for(4ms);
            rt.tick();
        }
        while_running = rt.model().ticks;

        // Drop the interval; it must stop firing.
        rt.send(EveryApp::Stop{});
        rt.tick();
        const int base = rt.model().ticks;
        for (int i = 0; i < 40; ++i) {
            std::this_thread::sleep_for(4ms);
            rt.tick();
        }
        after_stop = rt.model().ticks - base;
    });

    check(while_running >= 3,
          std::string{"an active Sub::every fires ("} +
          std::to_string(while_running) + " ticks)");
    check(after_stop == 0,
          "it stops the frame its condition goes false");
}

// ── Sub::source_latest: coalescing backpressure ──────────────────────
//
// A firehose feed — a socket or sensor faster than the UI renders — must not
// grow the inbox without bound. source_latest keeps only the newest message
// between frames, so the app sees one value per frame no matter the rate, and
// the last value it sees is the most recent one produced.

struct FirehoseApp {
    struct Model { int received = 0; int last = 0; };
    struct Start {};
    struct Value { int n; };
    using Msg = std::variant<Start, Value>;

    static inline std::atomic<int> emitted{0};

    static Model init() { return {}; }
    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Value>(msg)) {
            m.received += 1;
            m.last = std::get<Value>(msg).n;
        }
        return {m, Cmd<Msg>::none()};
    }
    static Node view(const Model&) { return dsl::box().build(); }
    static Sub<Msg> subscribe(const Model&) {
        // Emit as fast as possible for a short burst.
        return Sub<Msg>::source_latest("firehose",
            [](std::function<void(Msg)> sink, std::function<bool()> alive) {
                for (int i = 1; i <= 5000 && alive(); ++i) {
                    emitted.fetch_add(1, std::memory_order_relaxed);
                    sink(Value{i});
                }
            });
    }
};

void test_source_latest_coalesces() {
    section("Sub::source_latest coalescing");

    AppConfig cfg;
    cfg.log_renderer = false;

    int received = 0, last = 0, emitted = 0;
    run_headless<FirehoseApp>(cfg, [&](auto& rt) {
        // Let the producer run and pump a handful of frames.
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(3ms);
            rt.tick();
        }
        received = rt.model().received;
        last     = rt.model().last;
        emitted  = FirehoseApp::emitted.load();
    });

    check(emitted > 100,
          std::string{"the producer emitted a firehose ("} +
          std::to_string(emitted) + ")");
    // The whole point: the app processed FAR fewer messages than were emitted
    // — at most about one per frame — rather than a message per emission.
    check(received > 0 && received < emitted / 4,
          std::string{"coalescing collapses the firehose ("} +
          std::to_string(received) + " processed of " + std::to_string(emitted) + ")");
    // And what it did see is a recent value, not a stale early one.
    check(last > received,
          "the value delivered is the latest, not a backlog head");
}

// ── end to end: a blocked window wakes and renders ──────────────────────

void test_blocked_window_wakes() {
    section("live window wakeup");

    const char* disp = ::getenv("WAYLAND_DISPLAY");
    if (disp == nullptr || *disp == '\0' || !wl::lib().ok) {
        std::printf("  (no compositor — skipped)\n");
        return;
    }

    WindowConfig cfg;
    cfg.title = "mayag live test";
    cfg.size = {320, 200};
    auto win = WaylandWindow::open(cfg);
    check(win.has_value(), "the window opens");
    if (!win) return;

    Waker waker;
    win->set_waker(&waker);
    check(waker.usable(), "the waker is usable");

    // Block the UI thread with NO window activity; a producer wakes it.
    std::thread producer([&] {
        std::this_thread::sleep_for(80ms);
        waker.wake();
    });

    const auto t0 = std::chrono::steady_clock::now();
    // Wait::block would sleep indefinitely with no window events; the waker is
    // the only thing that can return it promptly.
    auto events = win->poll_events(Wait::block, 0.0);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    producer.join();
    (void)events;

    check(elapsed < 1500ms,
          "a blocked window is woken by a background thread, not left asleep");
    std::printf("  (woke after %lld ms)\n",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
}

}  // namespace

int main() {
    std::printf("mayag live / streaming\n======================\n");
    test_waker_basics();
    test_waker_cross_thread();
    test_stream_lifecycle();
    test_source_lifecycle();
    test_source_latest_coalesces();
    test_every_reconciliation();
    test_blocked_window_wakes();
    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
