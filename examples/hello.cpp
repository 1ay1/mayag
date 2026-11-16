// examples/hello.cpp — the smallest complete mayag app.
//
// A live, animated counter in one screen:
//
//   Model      plain struct — all the state, nothing hidden
//   Msg        a variant of named messages
//   update     one overload per message, changes the model in place
//   view       pure Model -> Node; each control says what it does, inline
//   subscribe  the frame clock, only while the spring is moving
//
// Build:  cmake --build build && ./build/examples/mayag_hello
// (--headless self-checks without a window; --png/--gpu write a screenshot)

#include <mayag/mayag.hpp>

#include "harness.hpp"

#include <optional>
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
    static Cmd update(Model& m, Tick t) { m.bump.step(t.dt); return {}; }
    static Cmd update(Model&,   Quit)   { return Cmd::quit(); }

    // ── view: a pure function of the model ──────────────────────────────
    // Colours and sizes are theme roles, and each button says what it sends.
    static Node view(const Model& m) {
        const float pop = 1.0f + static_cast<float>(m.bump) * 0.25f;   // 1.0 → 1.25

        constexpr auto key = styles(size(64, 64), center, radius(var::radius_lg), font(28), bold,
                                    fg(var::on_accent), hovered(scale(1.06f)), pressed(scale(0.94f)),
                                    transition(var::motion_emphasis));

        return v(text<"HELLO"> | font(12) | semibold | tracking(3.0f) | fg(var::text_secondary),
                 textf("{}", m.count) | font(96.0f * pop) | bold,
                 h(h(text<"-">) | key | bg(var::danger) | on_click(Dec{}),
                   h(text<"+">) | key | bg(var::success) | on_click(Inc{})) | gap(16))
             | gap(20) | center | fill_both() | bg(var::background) | fg(var::text_primary);
    }

    // ── keys and the window ──────────────────────────────────────────────
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
        rt.click_text("+"); rt.click_text("+"); rt.click_text("-");
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
        return fails;
    });
}
