// tests/test_simulate.cpp — deterministic simulation of a real GUI program
//
// A search box whose results come from a background task, driven through
// the real host (typing, inline handlers, layout, drawing) under seeded
// simulated time. The classic async bug -- a slow early request landing
// after a fast late one and overwriting it (docs/async-hazards.md, hazard 2)
// -- depends entirely on task timing, so on real threads it is a flake.
// Under simulation it is a seed: found by explore(), replayed exactly, and
// shown fixed by a generation stamp across a thousand seeds.

#include <mayag/mayag.hpp>
#include <mayag/widget/agent_state.hpp>
#include <mayag/widget/text_editor.hpp>
#include <mayag/app/simulate.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <variant>

using namespace mayag;
using namespace mayag::dsl;
using namespace std::chrono_literals;

namespace {
int fails = 0;
void ok(bool c, const std::string& what) {
    std::printf("  %s %s\n", c ? "ok  " : "FAIL", what.c_str());
    if (!c) ++fails;
}

/// Results for a query: what a backend would return. Captureless (a task
/// body) and pure.
struct Results { std::string query; int gen = 0; };
Results lookup(std::string q, int gen) { return {std::move(q), gen}; }

template <bool Stamped>
struct Search {
    struct Model {
        std::string query;
        std::string shown_for;   ///< the query the visible results belong to
        int gen = 0;             ///< bumped per request (the fix)
        int in_flight = 0;
    };
    struct Typed { std::string s; };
    struct Clear {};
    using Msg = std::variant<Typed, Clear, Results>;
    using Cmd = mayag::Cmd<Msg>;

    static Cmd init(Model&) { return {}; }
    static Cmd update(Model& m, const Typed& t) {
        m.query += t.s;
        ++m.gen;
        ++m.in_flight;
        return Cmd::template perform<Results>(&lookup, m.query, m.gen);
    }
    static Cmd update(Model& m, Clear) { m.query.clear(); m.shown_for.clear(); ++m.gen; return {}; }
    static Cmd update(Model& m, const Results& r) {
        --m.in_flight;
        if constexpr (Stamped) {
            if (r.gen != m.gen) return {};   // stale: an older request finished late
        }
        m.shown_for = r.query;
        return {};
    }
    static Node view(const Model& m) {
        return v(textf("query: {}", m.query), textf("results for: {}", m.shown_for),
                 h(text<"Clear">) | pad(6) | on_click(Clear{}))
             | gap(8) | pad(8);
    }
    static std::optional<Msg> on(const Model&, const TextEvent& e) { return Typed{e.text}; }
};

/// Type "may" with human gaps, let everything land, check the screen.
template <class App>
int scenario(App& app) {
    app.type("m"); app.wait(3ms);
    app.type("a"); app.wait(2ms);
    app.type("y");
    app.settle();
    const auto& m = app.model();
    return (m.in_flight == 0 && m.shown_for == m.query) ? 0 : 1;
}
/// A program that shows ONLY a spinner: no subscription, no message, no
/// model clock. The spinner schedules its own frames (stepper).
struct Spinner {
    struct Model {};
    struct Nothing {};
    using Msg = std::variant<Nothing>;
    using Cmd = mayag::Cmd<Msg>;
    static Cmd init(Model&) { return {}; }
    static Cmd update(Model&, Nothing) { return {}; }
    static Node view(const Model&) { return text_owned(spinner_frame(ctx().time)) | font(14); }
};
std::string glyph_of(const Node& n) {
    if (n.kind() == NodeKind::text) return std::string{n.text()};
    for (const auto& k : n.children()) if (auto g = glyph_of(k); !g.empty()) return g;
    return {};
}
bool has_text(const Node& n, std::string_view s) {
    if (n.kind() == NodeKind::text && n.text() == s) return true;
    for (const auto& k : n.children()) if (has_text(k, s)) return true;
    return false;
}
/// An editor whose save the test starts and lands by message.
struct Saver {
    struct Model { TextEditorState st = text_editor_ops::open("x\n", "a.txt"); bool saving = false; };
    struct Save {};
    struct Saved {};
    using Msg = std::variant<Save, Saved>;
    using Cmd = mayag::Cmd<Msg>;
    static Cmd init(Model&) { return {}; }
    static Cmd update(Model& m, Save) { m.saving = true; return {}; }
    static Cmd update(Model& m, Saved) { m.saving = false; return {}; }
    static Node view(const Model& m) {
        TextEditorProps p;
        p.path = "a.txt";
        p.saving = m.saving;
        p.minimap = false;
        return text_editor(ctx(), "ed", m.st, p, [](TextEditorOp) -> Msg { return Saved{}; });
    }
};
}  // namespace

int main() {
    AppConfig cfg;
    cfg.size = {320, 200};
    cfg.fonts = typo::system::default_stack();   // shared by every simulated run
    // A backend whose latency varies more than a person's typing gaps (each
    // keystroke here is ~20 ms apart): exactly when replies can cross.
    SimOptions slowish;
    slowish.task_latency_min = 1ms;
    slowish.task_latency_max = 120ms;

    std::printf("-- deterministic simulation\n");

    {   // a spinner on screen turns by itself, at its own rate, forever
        simulated_app<Spinner> app(cfg, 1);
        std::vector<std::string> seen;
        for (int i = 0; i < 120; ++i) {   // two simulated seconds
            app.tick();
            const std::string g = glyph_of(app.tree());
            if (seen.empty() || seen.back() != g) seen.push_back(g);
        }
        ok(seen.size() >= 20, "a spinner with no subscription and no messages keeps turning (" +
                                  std::to_string(seen.size()) + " glyph changes in 2 s, 12 fps expected ~24)");
    }
    {   // "saving…" only for a save that takes long enough to be worth saying
        {
            simulated_app<Saver> fast(cfg, 1);
            fast.send(Saver::Msg{Saver::Save{}});
            const bool flashed = has_text(fast.tree(), "saving\u2026");
            fast.wait(16ms);
            fast.send(Saver::Msg{Saver::Saved{}});
            ok(!flashed && !has_text(fast.tree(), "saving\u2026"), "a save that lands in a frame never flashes \"saving\u2026\"");
        }
        simulated_app<Saver> slow(cfg, 1);
        (void)slow.settle(5s);   // every transition at rest: the only frame left to come is the mark's own
        slow.send(Saver::Msg{Saver::Save{}});
        ok(slow.host().next_change_at() > 0.0 && !has_text(slow.tree(), "saving\u2026"),
           "a save in flight books the frame its mark is due in (and not before)");
        slow.wait(300ms);   // no messages, no subscription: the widget's own booked frame
        ok(has_text(slow.tree(), "saving\u2026"), "a slow one shows it, by itself, a moment in");
    }

    // 1. The unstamped version is wrong for SOME timings: explore finds one.
    const auto bug = explore<Search<false>>(cfg, {.first_seed = 1, .runs = 1000, .sim = slowish},
                                            [](auto& app) { return scenario(app); });
    ok(!bug.ok(), "explore() finds a seed where a late result overwrites a newer one (seed " +
                      std::to_string(bug.seed) + ", after " + std::to_string(bug.runs) + " runs)");

    // 2. That seed is a reproduction: the same failure, every time.
    int replays_failed = 0;
    for (int i = 0; i < 5; ++i)
        replays_failed += run_simulated<Search<false>>(cfg, bug.seed, [](auto& app) { return scenario(app); }, slowish);
    ok(replays_failed == 5, "replaying the seed fails identically every time");

    // 3. ... down to the screen: the stale query is what is drawn.
    run_simulated<Search<false>>(cfg, bug.seed, [&](auto& app) {
        (void)scenario(app);
        bool drew_stale = false;
        const auto walk = [&](const auto& self, const Node& n) -> void {
            if (n.text() == "results for: " + app.model().shown_for && app.model().shown_for != app.model().query)
                drew_stale = true;
            for (const auto& k : n.children()) self(self, k);
        };
        walk(walk, app.tree());
        ok(drew_stale, "and the host drew the stale results (\"" + app.model().shown_for + "\" for \"" +
                           app.model().query + "\")");
        return 0;
    }, slowish);

    // 2b. Bit for bit: two runs of one seed draw the same tree and end at
    //     the same simulated instant.
    {
        // What is on screen: every node's text, frame and fill, in order.
        const auto screen = [](const Node& root) {
            std::string out;
            const auto walk = [&](const auto& self, const Node& n) -> void {
                const Rect f = n.frame();
                out += std::format("{}|{},{},{},{}|{:08x};", n.text(), f.origin.x, f.origin.y, f.size.x,
                                   f.size.y, pack_rgba8(n.style().fill.color));
                for (const auto& k : n.children()) self(self, k);
            };
            walk(walk, root);
            return out;
        };
        std::string first, second;
        std::chrono::nanoseconds t1{}, t2{};
        run_simulated<Search<false>>(cfg, 42, [&](auto& app) { (void)scenario(app); first = screen(app.tree()); t1 = app.elapsed(); return 0; }, slowish);
        run_simulated<Search<false>>(cfg, 42, [&](auto& app) { (void)scenario(app); second = screen(app.tree()); t2 = app.elapsed(); return 0; }, slowish);
        ok(first == second && t1 == t2, "the same seed draws the identical screen at the identical simulated time");
    }

    // 4. The generation stamp fixes it under every explored timing.
    const auto fixed = explore<Search<true>>(cfg, {.first_seed = 1, .runs = 1000, .sim = slowish},
                                             [](auto& app) { return scenario(app); });
    ok(fixed.ok() && fixed.runs == 1000, "the stamped version holds across 1000 seeds");

    // 5. Simulated time: a 30 s settle limit and 20 ms task latencies cost
    //    no wall-clock time, and the host's clock IS the simulated one.
    const auto t0 = std::chrono::steady_clock::now();
    run_simulated<Search<true>>(cfg, 7, [&](auto& app) {
        app.type("x");
        app.wait(30s);
        ok(app.elapsed() >= 30s, "wait(30s) advances simulated time by 30 s");
        ok(app.model().shown_for == "x", "the task landed inside it");
        app.click_text("Clear");
        ok(app.model().query.empty(), "inline handlers work under simulation");
        return 0;
    });
    const double wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ok(wall_ms < 2000.0, "and 30 simulated seconds take " + std::to_string(static_cast<int>(wall_ms)) + " ms of wall time");

    // 6. Fault injection: crashed tasks are reported, not hung on.
    SimOptions crashy = slowish;
    crashy.task_crash = 1.0;
    AppConfig quiet = cfg;
    int faults = 0;
    quiet.on_error = [&](std::string_view, std::string_view) { ++faults; };
    run_simulated<Search<true>>(quiet, 3, [&](auto& app) {
        app.type("q");
        ok(app.settle(), "a run where every task crashes still settles");
        ok(app.model().shown_for.empty(), "and no result was invented");
        return 0;
    }, crashy);
    ok(faults == 1, "the crash reached the app's fault handler once");

    std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
