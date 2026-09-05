// examples/hello.cpp — the smallest complete mayag app.
//
// A live, animated counter in one screen. It shows every part of the
// architecture exactly once, using the ergonomic API:
//
//   Model     plain struct — all the state, nothing hidden
//   Msg       a variant of named messages
//   init      the starting Model (+ optional startup Cmd)
//   update    pure (Model, Msg) -> (Model, Cmd) via match()
//   view      pure (Model, Ctx) -> Node, click targets self-wired with action()
//   subscribe what to listen for, as a function of state; on_frame() animates
//
// Build:  cmake --build build && ./build/examples/mayag_hello
// (--headless self-checks without a window; --png/--gpu write a screenshot)

#include <mayag/mayag.hpp>

#include "harness.hpp"

#include <string>
#include <utility>

using namespace mayag;
using namespace mayag::dsl;

struct Hello {
    // ── state ───────────────────────────────────────────────────────────
    struct Model {
        int             count = 0;
        Animated<float> bump{0.0f};   // a spring that pulses on each change
    };

    // ── messages ────────────────────────────────────────────────────────
    struct Inc {}; struct Dec {}; struct Tick { float dt; }; struct Quit {};
    using Msg = std::variant<Inc, Dec, Tick, Quit>;

    // Each clickable is declared ONCE: the id and the message it sends live
    // together, so the view tag (.on) and the subscription (.click()) can
    // never drift out of sync.
    static inline const auto inc = action(node_id("inc"), Msg{Inc{}});
    static inline const auto dec = action(node_id("dec"), Msg{Dec{}});

    // ── init ────────────────────────────────────────────────────────────
    static std::pair<Model, Cmd<Msg>> init() {
        return {Model{}, Cmd<Msg>::set_title("mayag — hello")};
    }

    // ── update: pure, returns the next Model + any effect ───────────────
    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Inc) -> R  { m.count++; m.bump.snap(1.0f); m.bump.to(0.0f); return {m, Cmd<Msg>::none()}; },
            [&](Dec) -> R  { m.count--; m.bump.snap(1.0f); m.bump.to(0.0f); return {m, Cmd<Msg>::none()}; },
            [&](Tick t) -> R { m.bump.step(t.dt); return {m, Cmd<Msg>::none()}; },
            [&](Quit) -> R { return {m, Cmd<Msg>::quit()}; });
    }

    // ── view: a pure function of the model ──────────────────────────────
    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = c.theme;
        const float pop = 1.0f + static_cast<float>(m.bump) * 0.25f;   // 1.0 → 1.25

        const auto btn = [&](std::string label, Color<Srgb> tone, Action<Msg> act) {
            return h(text_owned(std::move(label)) | font(28) | bold | fg(t.on_accent)) | center
                 | size(64, 64) | bg(tone) | radius(t.radius_large)
                 | act.on;   // <- makes this box `act`'s click target
        };

        return v(text<"HELLO"> | font(12) | semibold | tracking(3.0f) | fg(t.text_secondary),
                 text_owned(std::to_string(m.count))
                     | font(96.0f * pop) | bold | fg(t.text_primary),
                 h(btn("-", t.danger, dec),
                   btn("+", t.success, inc)) | gap(16))
             | gap(20) | center
             | width(pct(100)) | height(pct(100)) | bg(t.background);
    }

    // ── subscribe: clicks + animation, as data ──────────────────────────
    static Sub<Msg> subscribe(const Model& m) {
        return Sub<Msg>::batch(
            inc.click(),
            dec.click(),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_close(Quit{}),
            // Frames flow only while the spring is moving; a settled app is 0% CPU.
            when(m.bump.animating(),
                 Sub<Msg>::on_frame([](float dt) { return Tick{dt}; })));
    }
};

int main(int argc, char** argv) {
    // Same harness as every other example, so `--png`, `--gpu`, `--headless`,
    // `--size`, `--dpi` and the shared font configuration all apply here too.
    // hello used to hand-roll its own main and understood only `--headless`,
    // which meant fixes to the harness (its font stack, its backend selection)
    // silently did not reach the one example newcomers are told to copy.
    const auto opts = demo::parse_args(argc, argv, {360, 320});

    AppConfig cfg;
    cfg.title = "mayag — hello";
    cfg.size  = opts.size;

    // The whole app is testable without a window: drive it, assert on state.
    return demo::run<Hello>(opts, std::move(cfg), [](auto& rt) {
        int fails = 0;
        const auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            if (!c) ++fails;
        };
        rt.click("inc"); rt.click("inc"); rt.click("dec");
        ok(rt.model().count == 1, "clicks route to the right messages");
        ok(rt.model().bump.animating(), "a change starts the bump spring");
        // drive_frames makes headless ticks carry real time, so the spring
        // actually advances — then it settles and stops asking for frames.
        rt.window().drive_frames(true);
        for (int i = 0; i < 600 && rt.model().bump.animating(); ++i) rt.tick();
        rt.window().drive_frames(false);
        ok(!rt.model().bump.animating(), "the spring settles on its own (0% CPU)");
        ok(rt.window().frames_presented() >= 1, "the app rendered");
        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    });
}
