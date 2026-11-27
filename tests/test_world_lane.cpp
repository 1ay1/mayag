// tests/test_world_lane.cpp — the world can't stall the UI
//
// A world effect (mayag::run: write a pty, save a file, open a URL) is I/O,
// and I/O takes as long as the world likes. In a window a person uses, the
// host runs every one on its world lane -- one worker, in order -- never on
// the UI thread. Checked through the REAL loop (mayag::run_on, jaal::run):
//
//   unblocked  a 300 ms world effect runs while the loop keeps ticking
//              (an every-10ms timer keeps folding; inline it would be frozen)
//   off-loop   the effect body runs on another thread than update()
//   ordered    a slow effect then a fast one answer in the order asked
//   drained    `batch(save, quit)`: the save still lands before run_on returns
//   scripted   under the headless (scripted) window the same program runs its
//              effects inline: answered in the step that asked, deterministic

#include <mayag/mayag.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <variant>

using namespace mayag;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) ++failures;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

std::atomic<int>             g_ticks{0};
std::atomic<int>             g_ticks_at_answer{-1};
std::atomic<int>             g_first{0}, g_second{0};
std::atomic<bool>            g_saved{false};
std::atomic<std::thread::id> g_loop_thread{};
std::atomic<std::thread::id> g_world_thread{};

struct LaneApp {
    struct Model { int answers = 0; };
    struct Tick {};
    struct Done { int n; };
    using Msg = std::variant<Tick, Done>;
    using Cmd = mayag::Cmd<Msg, mayag::world>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model&) {
        g_loop_thread = std::this_thread::get_id();
        return Cmd::batch(
            Cmd::run([](int n) -> Msg {   // slow: a pipe whose reader is busy
                g_world_thread = std::this_thread::get_id();
                std::this_thread::sleep_for(300ms);
                return Done{n};
            }, 1),
            Cmd::run([](int n) -> Msg { return Done{n}; }, 2));   // fast, asked second
    }
    static Cmd update(Model&, Tick) { ++g_ticks; return {}; }
    static Cmd update(Model& m, Done d) {
        ++m.answers;
        (m.answers == 1 ? g_first : g_second) = d.n;
        if (d.n == 1) g_ticks_at_answer = g_ticks.load();
        if (m.answers < 2) return {};
        // The save is QUEUED behind a busy effect when quit lands: only the
        // exit drain gets it run (a pool being destroyed drops queued work).
        return Cmd::batch(Cmd::run([] { std::this_thread::sleep_for(100ms); }),
                          Cmd::run([] { g_saved = true; }),
                          Cmd::quit());
    }
    static Sub subscribe(const Model&) { return Sub::every(10ms, Tick{}); }
    static Node view(const Model&) { return dsl::box().build(); }
};

/// A window a person would use: everything Headless does, but not scripted.
struct Desk : platform::Headless {
    static constexpr bool scripted = false;
    explicit Desk(platform::Headless&& h) : platform::Headless(std::move(h)) {}
};
static_assert(!platform::has<Desk, platform::cap::scripted>);
static_assert(platform::has<platform::Headless, platform::cap::scripted>);

AppConfig quiet() {
    AppConfig cfg;
    cfg.log_renderer = false;
    cfg.size = {200, 120};
    return cfg;
}

void test_lane() {
    std::printf("\nworld lane (a window a person uses)\n");
    AppConfig cfg = quiet();
    Desk win{std::move(*platform::Headless::open(detail::window_config(cfg)))};
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = run_on<LaneApp>(win, cfg);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(rc == 0, "ran and quit (" + std::to_string(ms) + " ms)");
    check(g_ticks_at_answer.load() >= 10,
          "the loop kept folding while a 300 ms world effect ran (" + std::to_string(g_ticks_at_answer.load()) +
              " ticks before its answer; inline: 0)");
    check(g_world_thread.load() != std::thread::id{} && g_world_thread.load() != g_loop_thread.load(),
          "the effect body ran off the loop thread");
    check(g_first.load() == 1 && g_second.load() == 2, "a slow effect then a fast one answer in the order asked");
    check(g_saved.load(), "batch(save, quit): the save landed before run_on returned");
}

void test_scripted() {
    std::printf("\nworld effects under the scripted window\n");
    g_ticks = 0; g_first = 0; g_second = 0; g_ticks_at_answer = -1;
    g_world_thread = std::thread::id{};
    run_headless<LaneApp>(quiet(), [](auto& rt) {
        check(rt.model().answers == 2, "both answered before the first frame (inline, in order)");
        check(g_world_thread.load() == g_loop_thread.load(), "the body ran on the driver's thread");
        check(g_first.load() == 1 && g_second.load() == 2, "in the order asked");
    });
}

}  // namespace

int main() {
    test_lane();
    test_scripted();
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
