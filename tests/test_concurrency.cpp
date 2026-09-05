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

// ── task_coalesce: keep the pipe full under a continuous gesture ───────
//
// task_latest starves here. It supersedes the RUNNING task, so when requests
// arrive faster than the work completes, nothing ever finishes -- the video
// scrub bar showed zero frames across a 2 s drag. task_coalesce lets the
// running task complete and collapses the waiting ones, so results keep
// landing and the last request is never dropped.
std::atomic<int> g_coalesce_ran{0};

struct Coalesce {
    struct Model { int done = 0; int last = -1; bool settled = false; };
    struct Ask { int v; };
    struct Done { int v; };
    struct Settled {};
    using Msg = std::variant<Ask, Done, Settled>;

    static std::pair<Model, Cmd<Msg>> init() { return {Model{}, Cmd<Msg>::none()}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::this_thread::get_id() != g_ui_thread) ++g_update_off_ui;
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Ask a) -> R {
                m.settled = false;
                return {m, Cmd<Msg>::task_coalesce("work",
                    [v = a.v] {
                        ++g_coalesce_ran;
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
                        return Msg{Done{v}};
                    },
                    Msg{Settled{}})};
            },
            [&](Done d) -> R { ++m.done; m.last = d.v; return {m, Cmd<Msg>::none()}; },
            [&](Settled) -> R { m.settled = true; return {m, Cmd<Msg>::none()}; });
    }

    static Node view(const Model&, const Ctx& c) {
        return (box() | width(px(80)) | height(px(40))
                      | bg(c.theme.surface)).build();
    }
};

std::atomic<int> g_finished{0};

// ── a COMPUTED source id must actually distinguish sources ──────────
//
// Folding state into the id is the documented way to say "this is a different
// producer now" -- source("chat:" + room, ...). Every such id is a temporary
// std::string that dies when subscribe() returns, so while Source::id was a
// string_view the runtime diffed FREED memory: two epochs compared equal
// (ASan: stack-use-after-return), the id looked unchanged, and the old
// producer was left running.
//
// The visible symptom was a video that showed the seeked frame and then
// carried on playing from the position before the seek.
std::vector<int> g_epoch_starts;
std::mutex       g_epoch_mutex;

struct EpochSrc {
    struct Model { int epoch = 0; };
    struct Bump {};
    struct Ping { int epoch; };
    using Msg = std::variant<Bump, Ping>;

    static std::pair<Model, Cmd<Msg>> init() { return {Model{}, Cmd<Msg>::none()}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::this_thread::get_id() != g_ui_thread) ++g_update_off_ui;
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Bump) -> R { ++m.epoch; return {m, Cmd<Msg>::none()}; },
            [&](Ping) -> R { return {m, Cmd<Msg>::none()}; });
    }

    static Node view(const Model&, const Ctx& c) {
        return (box() | width(px(80)) | height(px(40))
                      | bg(c.theme.surface)).build();
    }

    static Sub<Msg> subscribe(const Model& m) {
        // The id is a TEMPORARY string, as every real use of this pattern is.
        return Sub<Msg>::source("gen:" + std::to_string(m.epoch),
                                [e = m.epoch](auto sink, auto alive) {
            {
                std::lock_guard lk{g_epoch_mutex};
                g_epoch_starts.push_back(e);
            }
            while (alive()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (alive()) sink(Msg{Ping{e}});
            }
        });
    }
};

// ── replacing a slow source must not block the UI thread ────────────────
//
// The video example folds a seek position into its playback source id, so
// every drag move asks for a DIFFERENT source and the runtime stops the old
// one. If that stop joins inline, the frame waits for a producer that may be
// mid-decode -- measured at 370 ms of frozen UI per drag move before the
// runtime learned to retire sources instead of joining them.
std::atomic<int> g_scrub_live{0};

struct Scrub {
    struct Model { int epoch = 0; };
    struct Seek {};
    struct Frame {};
    using Msg = std::variant<Seek, Frame>;

    static std::pair<Model, Cmd<Msg>> init() { return {Model{}, Cmd<Msg>::none()}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::this_thread::get_id() != g_ui_thread) ++g_update_off_ui;
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Seek)  -> R { ++m.epoch; return {m, Cmd<Msg>::none()}; },
            [&](Frame) -> R { return {m, Cmd<Msg>::none()}; });
    }

    static Node view(const Model&, const Ctx& c) {
        return (box() | width(px(80)) | height(px(40))
                      | bg(c.theme.surface)).build();
    }

    static Sub<Msg> subscribe(const Model& m) {
        // Slow to notice it has been stopped, like a decoder that only polls
        // alive() between frames.
        return Sub<Msg>::source("scrub:" + std::to_string(m.epoch),
                                [](auto sink, auto alive) {
            ++g_scrub_live;
            while (alive()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (alive()) sink(Msg{Frame{}});
            }
            --g_scrub_live;
        });
    }
};

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


struct Search {
    struct Model { std::string query; std::vector<std::string> fired; };
    struct Typed { char c; };
    struct Fire { std::string q; };
    using Msg = std::variant<Typed, Fire>;

    static std::pair<Model, Cmd<Msg>> init() { return {Model{}, Cmd<Msg>::none()}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Typed t) -> R {
                m.query += t.c;
                // The classic debounce: "search 80ms after the user stops".
                return {m, Cmd<Msg>::after_latest("search",
                                           std::chrono::milliseconds(80),
                                           Msg{Fire{m.query}})};
            },
            [&](Fire f) -> R { m.fired.push_back(f.q); return {m, Cmd<Msg>::none()}; });
    }
    static Node view(const Model&, const Ctx& c) {
        return (box() | width(px(10)) | height(px(10)) | bg(c.theme.surface)).build();
    }
    static Sub<Msg> subscribe(const Model&) { return Sub<Msg>::none(); }
};


std::atomic<int> g_a{0}, g_b{0};

struct Twin {
    struct Model { int a = 0, b = 0; };
    struct FromA {}; struct FromB {};
    using Msg = std::variant<FromA, FromB>;

    static std::pair<Model, Cmd<Msg>> init() { return {Model{}, Cmd<Msg>::none()}; }
    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](FromA) -> R { ++m.a; return {m, Cmd<Msg>::none()}; },
            [&](FromB) -> R { ++m.b; return {m, Cmd<Msg>::none()}; });
    }
    static Node view(const Model&, const Ctx& c) {
        return (box() | width(px(10)) | height(px(10)) | bg(c.theme.surface)).build();
    }
    static Sub<Msg> subscribe(const Model&) {
        // Two DIFFERENT producers that happen to share an id. In a real app
        // this is two components both naming their feed "feed", or the same
        // component instantiated twice.
        return Sub<Msg>::batch({
            Sub<Msg>::source("feed", [](auto sink, auto alive) {
                ++g_a;
                while (alive()) {
                    sink(Msg{FromA{}});
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }),
            Sub<Msg>::source("feed", [](auto sink, auto alive) {
                ++g_b;
                while (alive()) {
                    sink(Msg{FromB{}});
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }),
        });
    }
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

    // ── debounce ─────────────────────────────────────────────────────
    section("after_latest: a debounce that actually debounces");
    {
        // Cmd::after schedules a timer with no identity, so the canonical
        // "search 80ms after the user stops typing" fires once PER KEYSTROKE:
        // typing "hello" queued five timers and ran five searches, four for
        // stale queries. In a real app that is five network requests a word.
        //
        // Headless runs a VIRTUAL clock advancing per tick, so the test types
        // at 10ms/char of tick time -- sleeping in wall time would prove
        // nothing.
        AppConfig cfg;
        cfg.size = {10, 10};
        std::vector<std::string> fired;

        run_headless<Search>(cfg, [&](auto& rt) {
            rt.window().set_tick_seconds(0.010);
            for (char c : {'h', 'e', 'l', 'l', 'o'}) {
                rt.send(Search::Msg{Search::Typed{c}});
                rt.tick();
            }
            for (int i = 0; i < 40; ++i) rt.tick();
            fired = rt.model().fired;
        });

        check(fired.size() == 1,
              "five fast keystrokes produce ONE delayed message (" +
                  std::to_string(fired.size()) + ")");
        check(!fired.empty() && fired.front() == "hello",
              "and it carries the newest state, not a stale one");
    }

    // ── id collisions ────────────────────────────────────────────────
    section("duplicate Sub::source ids are detected, not silently dropped");
    {
        // Sources are reconciled BY id, so two subscriptions sharing one means
        // the second never starts: no crash, no warning, just a feed that
        // never produces while the app looks fine. Two instances of a
        // component, or two modules that both picked "feed", is all it takes.
        //
        // The runtime now reports it. This test pins the BEHAVIOUR that makes
        // the report necessary -- first wins, second is dropped -- so if the
        // reconciliation is ever changed to something better (running both
        // under distinct internal keys), this fails and gets revisited rather
        // than the warning quietly becoming a lie.
        AppConfig cfg;
        cfg.size = {10, 10};
        int a_msgs = 0, b_msgs = 0;

        run_headless<Twin>(cfg, [&](auto& rt) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(600);
            while (std::chrono::steady_clock::now() < deadline) {
                rt.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            a_msgs = rt.model().a;
            b_msgs = rt.model().b;
        });

        check(g_a.load() == 1 && a_msgs > 0,
              "the first source with a given id runs");
        check(g_b.load() == 0 && b_msgs == 0,
              "the second is dropped (which is why the runtime warns)");
    }

    // ── a COMPUTED source id must actually distinguish sources ──────
    section("a source id built from model state restarts the producer");
    {
        AppConfig cfg;
        cfg.size = {80, 40};

        run_headless<EpochSrc>(cfg, [&](auto& rt) {
            for (int i = 0; i < 3; ++i) {
                rt.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                rt.send(EpochSrc::Msg{EpochSrc::Bump{}});
            }
            rt.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            rt.tick();
        });

        std::lock_guard lk{g_epoch_mutex};
        // Each bump is a new id, so each must have started its OWN producer.
        check(g_epoch_starts.size() >= 3,
              "each computed id started a distinct producer (" +
                  std::to_string(g_epoch_starts.size()) + " started)");
        bool ordered = true;
        for (std::size_t i = 1; i < g_epoch_starts.size(); ++i)
            if (g_epoch_starts[i] <= g_epoch_starts[i - 1]) ordered = false;
        check(ordered, "and each ran with its own captured state, in order");
    }

    // ── coalescing under a continuous gesture ────────────────────
    section("task_coalesce: a fast gesture still gets results, and the last one lands");
    {
        AppConfig cfg;
        cfg.size = {80, 40};
        int done = 0, last = -1, requests = 0;
        bool settled = false;

        run_headless<Coalesce>(cfg, [&](auto& rt) {
            // A "drag": a request every 5 ms against 40 ms of work, for
            // 400 ms. task_latest would finish none of these.
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(400);
            while (std::chrono::steady_clock::now() < until) {
                rt.send(typename Coalesce::Msg{typename Coalesce::Ask{requests++}});
                rt.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const int final_request = requests - 1;

            // Release: let the queue drain.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(10);
            while (!rt.model().settled &&
                   std::chrono::steady_clock::now() < deadline) {
                rt.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            done = rt.model().done;
            last = rt.model().last;
            settled = rt.model().settled;
            requests = final_request;
        });

        check(done > 1,
              "results keep arriving DURING the gesture (" +
                  std::to_string(done) + " delivered)");
        // The whole point: intermediate requests are dropped, not queued, so
        // the worker never builds a backlog.
        check(g_coalesce_ran.load() < 40,
              "but far fewer tasks ran than were requested (" +
                  std::to_string(g_coalesce_ran.load()) + " ran)");
        check(last == requests,
              "and the FINAL request is the one that lands (" +
                  std::to_string(last) + " vs " + std::to_string(requests) + ")");
        check(settled,
              "`settled` fires once nothing is running or pending");
    }

    // ── replacing a source must not stall the frame ────────────────
    section("a replaced source is retired off the UI thread, not joined on it");
    {
        AppConfig cfg;
        cfg.size = {80, 40};
        double worst_ms = 0;

        run_headless<Scrub>(cfg, [&](auto& rt) {
            rt.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            // Drag: a new source id per move, each replacing a producer that
            // takes up to 300 ms to notice.
            for (int i = 0; i < 6; ++i) {
                rt.send(Scrub::Msg{Scrub::Seek{}});
                const auto t0 = std::chrono::steady_clock::now();
                rt.tick();               // reconciles: stops the old source
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                worst_ms = ms > worst_ms ? ms : worst_ms;
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        });

        // The producer sleeps 300 ms between alive() polls. A tick that waits
        // for it lands near that; a tick that retires it is sub-millisecond.
        // 100 ms is far below the old 370 ms and far above the new ~0.5 ms.
        check(worst_ms < 100.0,
              "stopping a slow source does not block the frame (worst tick " +
                  std::to_string(worst_ms) + " ms)");

        // And nothing leaks: run_headless destroys the runtime, whose
        // teardown joins retirees as well as running sources.
        check(g_scrub_live.load() == 0,
              "every retired producer was still joined by shutdown (" +
                  std::to_string(g_scrub_live.load()) + " left running)");
    }

    // ── the threading contract ──────────────────────────────────
    section("contract: update() is single-threaded");
    check(g_update_off_ui.load() == 0,
          "update() only ever ran on the UI thread (" +
              std::to_string(g_update_off_ui.load()) + " violations)");

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
