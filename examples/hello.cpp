// examples/hello.cpp — the smallest complete mayag app.
//
// A live, animated counter in one screen:
//
//   Model      plain struct — all the state, nothing hidden
//   Msg        a variant of named messages
//   update     one overload per message, changes the model in place
//   view       pure (Model, Ctx) -> Node
//   on         window input -> Msg (clicks arrive already resolved to a node)
//   subscribe  the frame clock, only while the spring is moving
//
// Build:  cmake --build build && ./build/examples/mayag_hello
// (--headless self-checks without a window; --png/--gpu write a screenshot)

#include <mayag/mayag.hpp>

#include "harness.hpp"

#include <optional>
#include <string>
#include <utility>

using namespace mayag;
using namespace mayag::dsl;

struct Hello {
    // ── state ───────────────────────────────────────────────────────────────────
    struct Model {
        int             count = 0;
        Animated<float> bump{0.0f};   // a spring that pulses on each change
    };

    // ── messages ────────────────────────────────────────────────────────────────
    struct Inc {}; struct Dec {}; struct Tick { double dt; }; struct Quit {};
    using Msg = std::variant<Inc, Dec, Tick, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model&) { return SetTitle{"mayag — hello"}; }

    // ── update: one overload per message ────────────────────────────────────
    static Cmd update(Model& m, Inc)    { m.count++; m.bump.snap(1.0f); m.bump.to(0.0f); return {}; }
    static Cmd update(Model& m, Dec)    { m.count--; m.bump.snap(1.0f); m.bump.to(0.0f); return {}; }
    static Cmd update(Model& m, Tick t) { m.bump.step(static_cast<float>(t.dt)); return {}; }
    static Cmd update(Model&,   Quit)   { return Cmd::quit(); }

    // ── view: a pure function of the model ──────────────────────────────
    static Node view(const Model& m) {
        const Theme& t = ctx().theme;
        const float pop = 1.0f + static_cast<float>(m.bump) * 0.25f;   // 1.0 → 1.25

        const auto btn = [&](std::string label, Color<Srgb> tone, auto tag) {
            return h(text_owned(std::move(label)) | font(28) | bold | fg(t.on_accent)) | center
                 | size(64, 64) | bg(tone) | radius(t.radius_large) | tag;
        };

        return v(text<"HELLO"> | font(12) | semibold | tracking(3.0f) | fg(t.text_secondary),
                 text_owned(std::to_string(m.count))
                     | font(96.0f * pop) | bold | fg(t.text_primary),
                 h(btn("-", t.danger, dsl::id<"dec">),
                   btn("+", t.success, dsl::id<"inc">)) | gap(16))
             | gap(20) | center
             | width(pct(100)) | height(pct(100)) | bg(t.background);
    }

    // ── input ───────────────────────────────────────────────────────────────────
    static std::optional<Msg> on(const Model&, const Clicked& c) {
        if (is(c, "inc")) return Inc{};
        if (is(c, "dec")) return Dec{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (k.key == Key::escape) return Quit{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    // ── subscribe: frames flow only while the spring is moving ───────────
    static Sub subscribe(const Model& m) {
        return when(m.bump.animating(), Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
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
