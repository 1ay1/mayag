// tests/test_concurrency.cpp — background work under real contention, on jaal
//
// jaal owns the threads: tasks, keyed tasks, streams, timers, shutdown. Its
// own suite tests that machinery. This file drives it through mayag's
// headless host with many producers, high rates and repeated start/stop, and
// checks the properties a mayag app relies on:
//
//   * no message is lost, duplicated or reordered per producer;
//   * a stream whose condition goes false is stopped, and its in-flight
//     messages never reach update();
//   * a keyed task: only the newest request's result lands;
//   * replacing a slow stream doesn't stall the frame;
//   * shutdown while producers are mid-send doesn't crash or hang;
//   * update() only ever runs on the loop thread, so a Model needs no locks.
//
// Built a second time under -fsanitize=thread (mayag_concurrency_tsan).

#include <mayag/mayag.hpp>
#include <mayag/widget/find_bar.hpp>
#include <mayag/widget/scroll_view.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;
using namespace std::chrono_literals;

namespace {

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

std::thread::id  g_loop_thread{};
std::atomic<int> g_update_off_loop{0};
void on_loop() { if (std::this_thread::get_id() != g_loop_thread) ++g_update_off_loop; }

Node tiny() { return (box() | width(px(10)) | height(px(10)) | bg(ctx().theme.surface)).build(); }

AppConfig small() {
    AppConfig cfg;
    cfg.size = {80, 40};
    cfg.log_renderer = false;
    return cfg;
}

// ── many producers into one mailbox ─────────────────────────────────────
struct Fan {
    static constexpr int producers = 4;
    static constexpr int per_producer = 250;

    struct Model {
        std::vector<std::vector<int>> got = std::vector<std::vector<int>>(producers);
        int total = 0;
    };
    struct Value { int producer; int n; };
    struct Start {};
    using Msg = std::variant<Value, Start>;
    using Cmd = mayag::Cmd<Msg>;

    static Cmd update(Model& m, Value v) {
        on_loop();
        m.got[static_cast<std::size_t>(v.producer)].push_back(v.n);
        ++m.total;
        return {};
    }
    static Cmd update(Model&, Start) {
        on_loop();
        std::vector<Cmd> cmds;
        for (int p = 0; p < producers; ++p) {
            cmds.push_back(Cmd::task([](mayag::Sink<Msg> out, std::stop_token st, int who) {
                for (int i = 0; i < per_producer && !st.stop_requested(); ++i)
                    (void)out.send(Value{who, i});
            }, p));
        }
        return Cmd::batch(std::move(cmds));
    }
    static Node view(const Model&) { return tiny(); }
};

// ── a stream that starts and stops with the model ───────────────────────
std::atomic<int> g_live_producers{0};

struct Cycle {
    struct Model { bool on = false; int ticks = 0; };
    struct Toggle {}; struct Tick {};
    using Msg = std::variant<Toggle, Tick>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    static Cmd update(Model& m, Toggle) { on_loop(); m.on = !m.on; return {}; }
    static Cmd update(Model& m, Tick)   { on_loop(); ++m.ticks; return {}; }
    static Node view(const Model&) { return tiny(); }

    static Sub subscribe(const Model& m) {
        return when(m.on, Sub::stream("feed", [](mayag::Sink<Msg> out, std::stop_token st) {
            ++g_live_producers;
            while (!st.stop_requested()) {
                (void)out.send(Tick{});
                std::this_thread::sleep_for(200us);
            }
            --g_live_producers;
        }));
    }
};

// ── a stopped stream's in-flight messages ───────────────────────────────
// A producer that checks its stop token and then sends has done two things,
// and the model can change in between. The gate has to be at fold time.
std::atomic<int> g_from_dead_stream{0};

struct Hammer {
    struct Model { int epoch = 0; };
    struct Bump {}; struct Value { int epoch; };
    using Msg = std::variant<Bump, Value>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    static Cmd update(Model& m, Bump) { on_loop(); ++m.epoch; return {}; }
    static Cmd update(Model& m, Value v) {
        on_loop();
        if (v.epoch != m.epoch) ++g_from_dead_stream;
        return {};
    }
    static Node view(const Model&) { return tiny(); }

    static Sub subscribe(const Model& m) {
        // Fast, to keep the window between "stop checked" and "sent" wide, but
        // with a yield every 64 sends so a slow (TSAN) loop isn't buried.
        return Sub::stream("hammer:" + std::to_string(m.epoch),
                           [](mayag::Sink<Msg> out, std::stop_token st, int e) {
            for (int i = 0; !st.stop_requested(); ++i) {
                (void)out.send(Value{e});
                if ((i & 63) == 63) std::this_thread::yield();
            }
        }, m.epoch);
    }
};

// ── a key built from model state restarts the producer ──────────────────
std::vector<int> g_epoch_starts;
std::mutex       g_epoch_mutex;

struct EpochSrc {
    struct Model { int epoch = 0; };
    struct Bump {}; struct Ping { int epoch; };
    using Msg = std::variant<Bump, Ping>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    static Cmd update(Model& m, Bump) { on_loop(); ++m.epoch; return {}; }
    static Cmd update(Model&, Ping)   { on_loop(); return {}; }
    static Node view(const Model&) { return tiny(); }

    static Sub subscribe(const Model& m) {
        return Sub::stream("gen:" + std::to_string(m.epoch),
                           [](mayag::Sink<Msg> out, std::stop_token st, int e) {
            { std::lock_guard lk{g_epoch_mutex}; g_epoch_starts.push_back(e); }
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(5ms);
                (void)out.send(Ping{e});
            }
        }, m.epoch);
    }
};

// ── replacing a slow stream must not block the loop ─────────────────────
std::atomic<int> g_scrub_live{0};

struct Scrub {
    struct Model { int epoch = 0; };
    struct Seek {}; struct Frame {};
    using Msg = std::variant<Seek, Frame>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    static Cmd update(Model& m, Seek)  { on_loop(); ++m.epoch; return {}; }
    static Cmd update(Model&,   Frame) { on_loop(); return {}; }
    static Node view(const Model&) { return tiny(); }

    static Sub subscribe(const Model& m) {
        // Slow to notice it was stopped, like a decoder that checks between frames.
        return Sub::stream("scrub:" + std::to_string(m.epoch),
                           [](mayag::Sink<Msg> out, std::stop_token st) {
            ++g_scrub_live;
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(300ms);
                if (!st.stop_requested()) (void)out.send(Frame{});
            }
            --g_scrub_live;
        });
    }
};

// ── keyed task: the newest request wins ─────────────────────────────────
std::atomic<int> g_finished{0};

struct Drag {
    struct Model { int value = 0; std::vector<int> seen; };
    struct Request { int v; int delay_ms; };
    struct Done { int v; };
    using Msg = std::variant<Request, Done>;
    using Cmd = mayag::Cmd<Msg>;

    static Cmd update(Model&, Request r) {
        on_loop();
        // Inverted delays: the first request is slowest, so a naive runtime
        // delivers it last and it wins.
        return Cmd::perform<Done>(mayag::TaskKey{"scrub"}, [](int v, int delay_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            ++g_finished;
            return v;
        }, r.v, r.delay_ms);
    }
    static Cmd update(Model& m, Done d) { on_loop(); m.value = d.v; m.seen.push_back(d.v); return {}; }
    static Node view(const Model&) { return tiny(); }
};

// ── keyed Cmd::after: a debounce ─────────────────────────────────────────
struct Search {
    struct Model { std::string query; std::vector<std::string> fired; };
    struct Typed { char c; }; struct Fire { std::string q; };
    using Msg = std::variant<Typed, Fire>;
    using Cmd = mayag::Cmd<Msg>;

    static Cmd update(Model& m, Typed t) {
        on_loop();
        m.query += t.c;
        return Cmd::after("search", 80ms, Fire{m.query});   // restarts on every key
    }
    static Cmd update(Model& m, const Fire& f) { on_loop(); m.fired.push_back(f.q); return {}; }
    static Node view(const Model&) { return tiny(); }
};

// ── two streams with the same key ───────────────────────────────────────
std::atomic<int> g_a{0}, g_b{0};

struct Twin {
    struct Model { int a = 0, b = 0; };
    struct FromA {}; struct FromB {};
    using Msg = std::variant<FromA, FromB>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    static Cmd update(Model& m, FromA) { on_loop(); ++m.a; return {}; }
    static Cmd update(Model& m, FromB) { on_loop(); ++m.b; return {}; }
    static Node view(const Model&) { return tiny(); }

    static Sub subscribe(const Model&) {
        // Two different producers that happen to share a key: two instances
        // of a component, or two modules that both picked "feed".
        return Sub::batch(
            Sub::stream("feed", [](mayag::Sink<Msg> out, std::stop_token st) {
                ++g_a;
                while (!st.stop_requested()) { (void)out.send(FromA{}); std::this_thread::sleep_for(2ms); }
            }),
            Sub::stream("feed", [](mayag::Sink<Msg> out, std::stop_token st) {
                ++g_b;
                while (!st.stop_requested()) { (void)out.send(FromB{}); std::this_thread::sleep_for(2ms); }
            }));
    }
};

// ── at most one running, newest pending: coalescing in the model ───────
// jaal has no coalescing policy. A continuous gesture (a scrub drag) needs
// it: a keyed task supersedes the RUNNING work, so with requests faster than
// the work nothing ever finishes. The model keeps it instead: one in flight,
// one pending slot the newest request overwrites.
std::atomic<int> g_coalesce_ran{0};

struct Coalesce {
    struct Model { bool busy = false; std::optional<int> pending; int done = 0; int last = -1; };
    struct Ask { int v; }; struct Done { int v; };
    using Msg = std::variant<Ask, Done>;
    using Cmd = mayag::Cmd<Msg>;

    static Cmd start(Model& m, int v) {
        m.busy = true;
        return Cmd::perform<Done>([](int x) {
            ++g_coalesce_ran;
            std::this_thread::sleep_for(40ms);
            return x;
        }, v);
    }
    static Cmd update(Model& m, Ask a) {
        on_loop();
        if (m.busy) { m.pending = a.v; return {}; }
        return start(m, a.v);
    }
    static Cmd update(Model& m, Done d) {
        on_loop();
        ++m.done;
        m.last = d.v;
        m.busy = false;
        if (m.pending) { const int next = *m.pending; m.pending.reset(); return start(m, next); }
        return {};
    }
    static Node view(const Model&) { return tiny(); }
};

}  // namespace

// ── two runtimes, two threads ───────────────────────────────────────────────────────────────
// mayag's ambient per-frame state (the frame's Ctx, the theme memo, the
// animation request, the scroll-geometry flag, the style free list) is per
// thread so that two runtimes never see each other's. This proves it: the
// same program, driven the same way, gives the same pixels alone and while
// another runtime renders on another thread.
struct Doppel {
    struct Model { int n = 0; mayag::ScrollState scroll; };
    struct Bump {};
    using Msg = std::variant<Bump>;
    using Cmd = mayag::Cmd<Msg>;
    static Cmd update(Model& m, Bump) { ++m.n; return {}; }
    static Node view(const Model& m) {
        if (m.n < 3) request_animation_frame();   // the per-thread animation flag
        std::vector<Node> rows;
        for (int i = 0; i < 12; ++i)
            rows.push_back((text_owned("row " + std::to_string(i + m.n)) | font(13)).build());
        return (v(text_owned("count " + std::to_string(m.n)) | font(16) | bold,
                  themed(make_theme(Color<Srgb>{0.9f, 0.3f, 0.5f, 1.0f}, Mode::dark),   // the theme memo
                         [&] { return (box() | width(px(60)) | height(px(14)) | bg(ctx().theme.accent)).build(); }),
                  scroll_view(ctx(), "list", m.scroll, node(v(std::move(rows))) | height(px(120))))   // the scroll-geometry flag
                | pad(6) | bg(ctx().theme.surface)).build();
    }
};

/// Drive a Doppel through `steps` bumps and frames; its final pixels.
std::vector<std::uint8_t> twin_run(int steps) {
    std::vector<std::uint8_t> px;
    AppConfig cfg = small();
    cfg.size = {220, 200};
    run_headless<Doppel>(cfg, [&](auto& rt) {
        for (int i = 0; i < steps; ++i) {
            rt.send(Doppel::Bump{});
            rt.tick();
        }
        rt.tick();
        px = rt.window().read_pixels();
    });
    return px;
}

int main() {
    g_loop_thread = std::this_thread::get_id();
    std::printf("mayag concurrency\n=================\n");

    section("two runtimes on two threads");
    {
        const auto alone = twin_run(40);
        int differ_a = 0, differ_b = 0;   // each written by one thread
        {
            std::jthread a([&] { for (int r = 0; r < 6; ++r) differ_a += twin_run(40) != alone ? 1 : 0; });
            std::jthread b([&] { for (int r = 0; r < 6; ++r) differ_b += twin_run(40) != alone ? 1 : 0; });
        }
        check(!alone.empty(), "a runtime renders");
        check(differ_a + differ_b == 0, "two runtimes rendering at once each get exactly their own pixels (" +
                                            std::to_string(differ_a + differ_b) + "/12 differed)");
    }

    section("fan-in: 4 tasks x 250 messages");
    {
        int total = 0;
        bool complete = true, ordered = true;
        run_headless<Fan>(small(), [&](auto& rt) {
            rt.send(Fan::Start{});
            (void)rt.settle([](const auto& m) { return m.total >= Fan::producers * Fan::per_producer; },
                            10s);
            total = rt.model().total;
            for (const auto& seq : rt.model().got) {
                if (static_cast<int>(seq.size()) != Fan::per_producer) complete = false;
                for (std::size_t i = 0; i < seq.size(); ++i)
                    if (seq[i] != static_cast<int>(i)) ordered = false;
            }
        });
        check(total == Fan::producers * Fan::per_producer,
              "every message arrived exactly once (" + std::to_string(total) + "/" +
                  std::to_string(Fan::producers * Fan::per_producer) + ")");
        check(complete, "each producer delivered its full sequence");
        check(ordered, "per-producer order is preserved");
    }

    section("lifecycle: a stream starts and stops with the model");
    {
        bool ran = false, stopped = false;
        run_headless<Cycle>(small(), [&](auto& rt) {
            for (int cycle = 0; cycle < 10; ++cycle) {
                rt.send(Cycle::Toggle{});                       // on
                const int before = rt.model().ticks;
                if (rt.settle([&](const auto& m) { return m.ticks > before; }, 2s)) ran = true;
                rt.send(Cycle::Toggle{});                       // off
            }
            const auto end = std::chrono::steady_clock::now() + 2s;
            while (g_live_producers.load() != 0 && std::chrono::steady_clock::now() < end)
                std::this_thread::yield();
            stopped = g_live_producers.load() == 0;
        });
        check(ran, "an active stream delivers");
        check(stopped, "10 on/off cycles leave no producer running");
    }

    section("shutdown: tearing down under load");
    {
        for (int rep = 0; rep < 20; ++rep) {
            run_headless<Fan>(small(), [&](auto& rt) {
                rt.send(Fan::Start{});
                for (int i = 0; i < 3; ++i) rt.tick();   // leave them running
            });
        }
        check(true, "20 teardowns mid-send completed without crash or hang");
    }

    section("keyed task: only the newest request can reach update()");
    {
        std::size_t delivered = 0;
        int final_value = 0;
        run_headless<Drag>(small(), [&](auto& rt) {
            rt.send(Drag::Request{1, 220});
            rt.send(Drag::Request{2, 170});
            rt.send(Drag::Request{3, 120});
            rt.send(Drag::Request{4,  70});
            rt.send(Drag::Request{5,  20});
            const auto end = std::chrono::steady_clock::now() + 5s;
            while (g_finished.load() < 5 && std::chrono::steady_clock::now() < end)
                (void)rt.settle([](const auto&) { return false; }, 10ms);
            (void)rt.settle([](const auto&) { return false; }, 50ms);   // anything late lands now
            delivered = rt.model().seen.size();
            final_value = rt.model().value;
        });
        check(g_finished.load() == 5, "every worker ran (nothing is killed)");
        check(delivered == 1, "but only ONE result reached update() (" + std::to_string(delivered) + ")");
        check(final_value == 5, "and it is the newest request, not the last to finish");
    }

    section("keyed Cmd::after: a debounce that actually debounces");
    {
        std::vector<std::string> fired;
        run_headless<Search>(small(), [&](auto& rt) {
            for (char c : {'h', 'e', 'l', 'l', 'o'}) rt.send(Search::Typed{c});
            (void)rt.settle([](const auto& m) { return !m.fired.empty(); }, 2s);
            (void)rt.settle([](const auto&) { return false; }, 150ms);   // a second fire would land here
            fired = rt.model().fired;
        });
        check(fired.size() == 1,
              "five fast keystrokes produce ONE delayed message (" + std::to_string(fired.size()) + ")");
        check(!fired.empty() && fired.front() == "hello", "and it carries the newest state");
    }

    section("duplicate stream keys: the first runs, the second doesn't");
    {
        int a_msgs = 0, b_msgs = 0;
        run_headless<Twin>(small(), [&](auto& rt) {
            (void)rt.settle([](const auto& m) { return m.a >= 10; }, 2s);
            a_msgs = rt.model().a;
            b_msgs = rt.model().b;
        });
        check(g_a.load() == 1 && a_msgs > 0, "the first stream with a given key runs");
        check(g_b.load() == 0 && b_msgs == 0, "the second is dropped, not run twice under one key");
    }

    section("a stream key built from model state restarts the producer");
    {
        run_headless<EpochSrc>(small(), [&](auto& rt) {
            for (int i = 0; i < 3; ++i) {
                const std::size_t want = static_cast<std::size_t>(i) + 1;
                (void)rt.settle([&](const auto&) {
                    std::lock_guard lk{g_epoch_mutex};
                    return g_epoch_starts.size() >= want;
                }, 2s);
                rt.send(EpochSrc::Bump{});
            }
            (void)rt.settle([&](const auto&) {
                std::lock_guard lk{g_epoch_mutex};
                return g_epoch_starts.size() >= 4;
            }, 2s);
        });
        std::lock_guard lk{g_epoch_mutex};
        check(g_epoch_starts.size() >= 4,
              "each computed key started a distinct producer (" +
                  std::to_string(g_epoch_starts.size()) + " started)");
        bool ordered = true;
        for (std::size_t i = 1; i < g_epoch_starts.size(); ++i)
            if (g_epoch_starts[i] <= g_epoch_starts[i - 1]) ordered = false;
        check(ordered, "and each ran with its own arguments, in order");
    }

    section("a stopped stream's in-flight messages are never applied");
    {
#if defined(__SANITIZE_THREAD__)
        constexpr int bumps = 30;     // TSAN is ~50x slower per send
#else
        constexpr int bumps = 300;
#endif
        run_headless<Hammer>(small(), [&](auto& rt) {
            for (int i = 0; i < bumps; ++i) rt.send(Hammer::Bump{});
        });
        check(g_from_dead_stream.load() == 0,
              "no message from an already-stopped stream reaches update() (" +
                  std::to_string(g_from_dead_stream.load()) + ")");
    }

    section("coalescing in the model: a fast gesture still gets results");
    {
        int done = 0, last = -1, requests = 0;
        bool idle = false;
        run_headless<Coalesce>(small(), [&](auto& rt) {
            // A request every 5 ms against 40 ms of work, for 400 ms.
            const auto until = std::chrono::steady_clock::now() + 400ms;
            while (std::chrono::steady_clock::now() < until) {
                rt.send(Coalesce::Ask{requests++});
                (void)rt.settle([](const auto&) { return false; }, 5ms);
            }
            (void)rt.settle([](const auto& m) { return !m.busy && !m.pending; }, 5s);
            done = rt.model().done;
            last = rt.model().last;
            idle = !rt.model().busy && !rt.model().pending;
        });
        check(done > 1, "results keep arriving DURING the gesture (" + std::to_string(done) + ")");
        check(g_coalesce_ran.load() < 40,
              "far fewer tasks ran than were requested (" + std::to_string(g_coalesce_ran.load()) + ")");
        check(last == requests - 1,
              "and the FINAL request is the one that lands (" + std::to_string(last) + " vs " +
                  std::to_string(requests - 1) + ")");
        check(idle, "nothing is left running or pending");
    }

    section("replacing a slow stream doesn't stall the loop");
    {
        double worst_ms = 0;
        run_headless<Scrub>(small(), [&](auto& rt) {
            (void)rt.settle([](const auto&) { return g_scrub_live.load() > 0; }, 1s);
            for (int i = 0; i < 6; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                rt.send(Scrub::Seek{});          // folds, reconciles: stops the old stream
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                worst_ms = ms > worst_ms ? ms : worst_ms;
                (void)rt.settle([](const auto&) { return false; }, 15ms);
            }
        });
        // The producer sleeps 300 ms between checks. A step that waited for it
        // lands near that; one that just signals it is well under a millisecond.
        check(worst_ms < 100.0,
              "stopping a slow stream does not block the loop (worst " + std::to_string(worst_ms) + " ms)");
        const auto end = std::chrono::steady_clock::now() + 2s;
        while (g_scrub_live.load() != 0 && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        check(g_scrub_live.load() == 0,
              "and every replaced producer did stop (" + std::to_string(g_scrub_live.load()) + " left)");
    }

    section("contract: update() is single-threaded");
    check(g_update_off_loop.load() == 0,
          "update() only ever ran on the loop thread (" +
              std::to_string(g_update_off_loop.load()) + " violations)");

    std::printf("\n%s  %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
