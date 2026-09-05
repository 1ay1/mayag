// tests/test_concurrency.cpp — the async surface under real contention.
//
// mayag's concurrency story is "the UI thread owns the model; everything else
// posts messages". That is the right design, and it is also the kind of design
// that looks correct in review while hiding a use-after-free on the shutdown
// path, or dropping messages under contention, or leaking a thread when a
// subscription's condition goes false.
//
// Cmd::task, Cmd::stream and Sub::source had NO test and NO example. This
// exercises them with many producers, high rates, and repeated start/stop
// cycles -- the conditions where those bugs actually appear -- and asserts the
// properties that must hold:
//
//   * no message is lost, duplicated or reordered per producer;
//   * a source whose condition goes false is stopped AND joined, not leaked;
//   * shutdown while producers are mid-send does not crash or hang;
//   * update() only ever runs on the UI thread, so a Model needs no locking.
//
// Run it under -fsanitize=thread to make the last one enforceable rather than
// merely asserted; the CMake target mayag_concurrency_tests_tsan does that
// when sanitizers are available.

#include <mayag/mayag.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

// Which thread ran update()? If this is ever not the UI thread, every app's
// Model is silently racy and the whole "no locking needed" claim is false.
std::thread::id g_ui_thread{};
std::atomic<int> g_update_off_ui{0};

// ── an app driven entirely by background producers ──────────────────────
struct Fan {
    static constexpr int producers = 4;
    static constexpr int per_producer = 250;

    struct Model {
        // Per-producer receipt log, so we can check ordering per stream rather
        // than globally (interleaving between producers is expected).
        std::vector<std::vector<int>> got{producers};
        int  total = 0;
        bool started = false;
    };

    struct Value { int producer; int n; };
    struct Start {};
    struct Quit {};
    using Msg = std::variant<Value, Start, Quit>;

    static std::pair<Model, Cmd<Msg>> init() { return {Model{}, Cmd<Msg>::none()}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::this_thread::get_id() != g_ui_thread) ++g_update_off_ui;
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Value v) -> R {
                m.got[static_cast<std::size_t>(v.producer)].push_back(v.n);
                ++m.total;
                return {m, Cmd<Msg>::none()};
            },
            [&](Start) -> R {
                m.started = true;
                // Fan out: several streams posting concurrently into one inbox.
                std::vector<Cmd<Msg>> cmds;
                for (int p = 0; p < producers; ++p) {
                    cmds.push_back(Cmd<Msg>::stream([p](auto sink, auto alive) {
                        for (int i = 0; i < per_producer && alive(); ++i) {
                            sink(Msg{Value{p, i}});
                        }
                    }));
                }
                return {m, Cmd<Msg>::batch(std::move(cmds))};
            },
            [&](Quit) -> R { return {m, Cmd<Msg>::quit()}; });
    }

    static Node view(const Model&, const Ctx& c) {
        return (box() | width(px(80)) | height(px(40))
                      | bg(c.theme.surface)).build();
    }
    static Sub<Msg> subscribe(const Model&) { return Sub<Msg>::none(); }
};

// ── a source that starts and stops with the model ───────────────────────
std::atomic<int> g_live_producers{0};

struct Cycle {
    struct Model { bool on = false; int ticks = 0; };
    struct Toggle {};
    struct Tick {};
    using Msg = std::variant<Toggle, Tick>;

    static std::pair<Model, Cmd<Msg>> init() { return {Model{}, Cmd<Msg>::none()}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::this_thread::get_id() != g_ui_thread) ++g_update_off_ui;
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Toggle) -> R { m.on = !m.on; return {m, Cmd<Msg>::none()}; },
            [&](Tick)   -> R { ++m.ticks;    return {m, Cmd<Msg>::none()}; });
    }

    static Node view(const Model&, const Ctx& c) {
        return (box() | width(px(80)) | height(px(40))
                      | bg(c.theme.surface)).build();
    }

    static Sub<Msg> subscribe(const Model& m) {
        // The producer exists exactly while m.on is true. Nothing calls stop().
        return when(m.on, Sub<Msg>::source("feed", [](auto sink, auto alive) {
            ++g_live_producers;
            while (alive()) {
                sink(Msg{Tick{}});
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            --g_live_producers;
        }));
    }
};

std::atomic<int> g_finished{0};

struct Drag {
    struct Model { int value = 0; std::vector<int> seen; };
    struct Request { int v; int delay_ms; };
    struct Done { int v; };
    using Msg = std::variant<Request, Done>;

    static std::pair<Model, Cmd<Msg>> init() { return {Model{}, Cmd<Msg>::none()}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Request r) -> R {
                // Deliberately INVERTED delays: the first request is slowest,
                // so a naive implementation delivers it last and it wins.
                return {m, Cmd<Msg>::task_latest("scrub", [r] {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(r.delay_ms));
                    ++g_finished;
                    return Msg{Done{r.v}};
                })};
            },
            [&](Done d) -> R {
                m.value = d.v;
                m.seen.push_back(d.v);
                return {m, Cmd<Msg>::none()};
            });
    }
    static Node view(const Model&, const Ctx& c) {
        return (box() | width(px(10)) | height(px(10)) | bg(c.theme.surface)).build();
    }
    static Sub<Msg> subscribe(const Model&) { return Sub<Msg>::none(); }
};


}  // namespace

int main() {
    g_ui_thread = std::this_thread::get_id();
    std::printf("mayag concurrency\n=================\n");

    // ── many producers into one inbox ────────────────────────────────
    section("fan-in: 4 streams x 250 messages");
    {
        AppConfig cfg;
        cfg.size = {80, 40};
        int total = 0;
        bool ordered = true, complete = true;

        run_headless<Fan>(cfg, [&](auto& rt) {
            rt.send(Fan::Msg{Fan::Start{}});
            // Pump until everything arrives. Bounded by TIME, not iteration
            // count: this test runs alongside 11 others under `ctest -j 12`,
            // and a fixed loop budget that is generous on an idle machine is
            // not generous on a loaded one. A test that flakes under load is
            // worse than no test, because it teaches you to re-run instead of
            // to look.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(20);
            while (rt.model().total < Fan::producers * Fan::per_producer &&
                   std::chrono::steady_clock::now() < deadline) {
                rt.tick();
            }
            const auto& m = rt.model();
            total = m.total;
            for (int p = 0; p < Fan::producers; ++p) {
                const auto& v = m.got[static_cast<std::size_t>(p)];
                if (static_cast<int>(v.size()) != Fan::per_producer) complete = false;
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (v[i] != static_cast<int>(i)) { ordered = false; break; }
                }
            }
        });

        check(total == Fan::producers * Fan::per_producer,
              "every message arrived exactly once (" + std::to_string(total) +
                  "/" + std::to_string(Fan::producers * Fan::per_producer) + ")");
        check(complete, "each producer delivered its full sequence");
        check(ordered, "per-producer order is preserved");
    }

    // ── declarative source lifecycle ─────────────────────────────────
    section("lifecycle: a source starts and stops with the model");
    {
        AppConfig cfg;
        cfg.size = {80, 40};
        bool ran = false, stopped = false, restarted = false;

        run_headless<Cycle>(cfg, [&](auto& rt) {
            const auto pump = [&](auto predicate) {
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(10);
                while (!predicate() &&
                       std::chrono::steady_clock::now() < deadline) {
                    rt.tick();
                    std::this_thread::yield();
                }
            };

            // Off by default: nothing running.
            for (int i = 0; i < 5; ++i) rt.tick();
            const bool idle_at_start = g_live_producers.load() == 0;

            // Turn it on; the producer should appear and deliver.
            rt.send(Cycle::Msg{Cycle::Toggle{}});
            pump([&] { return rt.model().ticks >= 5; });
            ran = rt.model().ticks > 0;

            // Turn it off; the thread must be signalled AND joined, so the
            // count returns to zero rather than leaking.
            rt.send(Cycle::Msg{Cycle::Toggle{}});
            pump([&] { return g_live_producers.load() == 0; });
            stopped = g_live_producers.load() == 0;

            // And it must be startable again -- a one-shot stop is a bug that
            // only shows on the second use.
            const int before = rt.model().ticks;
            rt.send(Cycle::Msg{Cycle::Toggle{}});
            pump([&] { return rt.model().ticks > before + 4; });
            restarted = rt.model().ticks > before;

            check(idle_at_start, "no producer runs while the condition is false");
        });

        check(ran, "the source runs while its condition holds");
        check(stopped, "it is stopped and joined when the condition goes false");
        check(restarted, "and it can be started again");
        check(g_live_producers.load() == 0,
              "no producer outlives the runtime (" +
                  std::to_string(g_live_producers.load()) + " leaked)");
    }

    // ── shutdown while producers are mid-flight ──────────────────────
    section("shutdown: tearing down under load");
    {
        // The dangerous case: destroy the Runtime while threads are actively
        // posting. If the inbox is freed before producers stop, this is a
        // use-after-free -- which is exactly what stop_all_sources + join is
        // for. Repeat it, because a race that fires 1 time in 20 is still a
        // crash in production.
        for (int rep = 0; rep < 20; ++rep) {
            AppConfig cfg;
            cfg.size = {80, 40};
            run_headless<Fan>(cfg, [&](auto& rt) {
                rt.send(Fan::Msg{Fan::Start{}});
                for (int i = 0; i < 3; ++i) rt.tick();   // leave them running
            });
        }
        check(true, "20 teardowns mid-send completed without crash or hang");
    }

    // ── supersession ─────────────────────────────────────────────────
    section("task_latest: only the newest request can reach update()");
    {
        // A scrub drag fires a task per pointer move. They finish OUT OF
        // ORDER -- here deliberately so, slowest first -- and without
        // supersession the last one to finish wins, which is not the one the
        // user asked for last. Every app hits this and re-invents the same
        // generation-counter defence; task_latest makes it the runtime's job.
        AppConfig cfg;
        cfg.size = {10, 10};
        std::size_t delivered = 0;
        int final_value = 0;

        run_headless<Drag>(cfg, [&](auto& rt) {
            rt.send(Drag::Msg{Drag::Request{1, 220}});
            rt.send(Drag::Msg{Drag::Request{2, 170}});
            rt.send(Drag::Msg{Drag::Request{3, 120}});
            rt.send(Drag::Msg{Drag::Request{4,  70}});
            rt.send(Drag::Msg{Drag::Request{5,  20}});

            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(15);
            while (g_finished.load() < 5 &&
                   std::chrono::steady_clock::now() < deadline) {
                rt.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            for (int i = 0; i < 50; ++i) {
                rt.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            delivered = rt.model().seen.size();
            final_value = rt.model().value;
        });

        check(g_finished.load() == 5, "every worker ran (nothing is killed)");
        check(delivered == 1, "but only ONE result reached update()");
        check(final_value == 5,
              "and it is the newest request, not the last to finish");
    }

    // ── the threading contract ───────────────────────────────────────
    section("contract: update() is single-threaded");
    check(g_update_off_ui.load() == 0,
          "update() only ever ran on the UI thread (" +
              std::to_string(g_update_off_ui.load()) + " violations)");

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
