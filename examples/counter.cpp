// examples/counter.cpp — a complete mayag application
//
// This is what a mayag app IS: five pure functions and two types. None of
// them touch the window, the GPU, the clock, or the filesystem.
//
//   Model   your state, a plain value
//   Msg     every event that can happen, as a closed sum
//   init    initial state
//   update  (Model, Msg) -> (Model, Cmd)     — pure
//   view    (Model, Ctx) -> Node             — pure
//   subscribe (Model) -> Sub                 — pure
//
// Run it windowed:   ./mayag_counter
// Run it headless:   ./mayag_counter --headless    (scripts input, asserts, exits)

#include <mayag/mayag.hpp>

#include <cstdio>
#include <cstring>
#include <variant>

using namespace mayag;
using namespace mayag::dsl;

struct Counter {
    // ── state ───────────────────────────────────────────────────────────

    struct Model {
        int  count = 0;
        int  step  = 1;
        bool celebrating = false;
        double pulse = 0.0;
    };

    // ── every possible event, enumerated ────────────────────────────────

    struct Increment {};
    struct Decrement {};
    struct Reset     {};
    struct BumpStep  {};
    struct Tick      { double dt; };
    struct Quit      {};

    using Msg = std::variant<Increment, Decrement, Reset, BumpStep, Tick, Quit>;

    // ── init ────────────────────────────────────────────────────────────

    static std::pair<Model, Cmd<Msg>> init() {
        return {Model{}, Cmd<Msg>::set_title("mayag — counter")};
    }

    // ── update: the only place state changes, and it is PURE ────────────
    //
    // Note there is no `if (window)` here, no rendering, no I/O. Effects are
    // returned as data. That is what makes this testable with `==`.

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        return std::visit([&](auto&& e) -> std::pair<Model, Cmd<Msg>> {
            using T = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<T, Increment>) {
                m.count += m.step;
                // Crossing a multiple of 10 starts a celebration animation.
                // The animation is state, so it is replayable and testable.
                if (m.count % 10 == 0 && m.count != 0) {
                    m.celebrating = true;
                    m.pulse = 0.0;
                }
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Decrement>) {
                m.count -= m.step;
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Reset>) {
                m.count = 0;
                m.celebrating = false;
                return {m, Cmd<Msg>::write_clipboard("0")};
            }
            else if constexpr (std::is_same_v<T, BumpStep>) {
                m.step = (m.step == 1) ? 5 : (m.step == 5 ? 10 : 1);
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Tick>) {
                m.pulse += e.dt;
                if (m.pulse > 0.8) { m.celebrating = false; m.pulse = 0.0; }
                return {m, Cmd<Msg>::none()};
            }
            else {
                return {m, Cmd<Msg>::quit()};
            }
        }, msg);
    }

    // ── view: pure function of (Model, Ctx) -> Node ─────────────────────
    //
    // Hover and press styling read from `ctx`, NOT from the Model. Transient
    // presentation state does not belong in application state — this is the
    // GPU-UI equivalent of CSS `:hover`.

    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = c.theme;

        auto action = [&]<fixed_string Name>(auto label, Color<Srgb> tone) {
            const auto nid = node_id(Name.view());
            const bool hot  = c.hovered(nid);
            const bool down = c.pressed(nid);

            // The whole interaction affordance, expressed declaratively.
            return h(label | font(20) | bold | fg(down ? t.on_accent : tone))
                 | center
                 | size(64, 56)
                 | bg(down ? tone : (hot ? tone.fade(0.22f) : t.surface_raised))
                 | border(1, hot ? tone : t.border)
                 | radius(t.radius_medium)
                 | when(hot && !down, elevation(6.0f))
                 | id_of(Name.view());
        };

        // The celebration ring is a pure function of `pulse` — no animation
        // controller, no tween objects, no callbacks.
        const float glow = m.celebrating
            ? num::sin(static_cast<float>(m.pulse) * num::pi / 0.8f)
            : 0.0f;

        auto readout =
            v(text_of(std::to_string(m.count)) | font(72) | bold
                                               | fg(m.celebrating ? t.success : t.text_primary),
              text_of("step " + std::to_string(m.step)) | font(11) | fg(t.text_secondary)
                                                        | tracking(1.5f))
            | gap(2) | center
            | size(240, 150)
            | bg(t.surface)
            | border(m.celebrating ? 2.0f : 1.0f,
                     m.celebrating ? t.success.fade(0.3f + glow * 0.7f) : t.border)
            | radius(t.radius_large)
            | when(m.celebrating, shadow(28.0f * glow, t.success.fade(0.5f * glow), {0, 0}))
            | elevation(10.0f);

        return v(text<"COUNTER"> | font(11) | semibold | fg(t.text_secondary) | tracking(3.0f),
                 readout,
                 h(action.template operator()<"dec">(text<"-">, t.danger),
                   action.template operator()<"step">(text<"S">, t.warning),
                   action.template operator()<"inc">(text<"+">, t.success)) | gap(10),
                 h(text<"R"> | font(10) | fg(t.text_disabled),
                   text<"reset"> | font(10) | fg(t.text_disabled),
                   spacer(),
                   text<"esc"> | font(10) | fg(t.text_disabled),
                   text<"quit"> | font(10) | fg(t.text_disabled))
                 | gap(6) | width(240))
             | gap(18) | center
             | width(pct(100)) | height(pct(100))
             | bg(t.background);
    }

    // ── subscribe: what to listen for, as a function of state ───────────
    //
    // The frame subscription only EXISTS while celebrating. When the flag
    // clears, the runtime stops the display link and the app goes back to
    // blocking at 0% CPU. There is no `stop_animation()` to forget to call.

    static Sub<Msg> subscribe(const Model& m) {
        return Sub<Msg>::batch(
            Sub<Msg>::on_click<"inc">(Increment{}),
            Sub<Msg>::on_click<"dec">(Decrement{}),
            Sub<Msg>::on_click<"step">(BumpStep{}),

            Sub<Msg>::on_key(Key::up,    Increment{}),
            Sub<Msg>::on_key(Key::down,  Decrement{}),
            Sub<Msg>::on_key(Key::r,     Reset{}),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_close(Quit{}),

            when(m.celebrating,
                 Sub<Msg>::every_frame([](FrameEvent f) { return Tick{f.delta}; })));
    }
};

static_assert(Program<Counter>, "Counter must satisfy the Program concept");

// ────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const bool headless = argc > 1 && std::strcmp(argv[1], "--headless") == 0;

    AppConfig cfg{.title = "mayag — counter", .size = {420, 460}};

    if (!headless) {
        return run<Counter>(cfg);
    }

    // Headless: script the input, assert the behaviour, and prove the app
    // works end to end without a display. This is the SAME runtime the
    // windowed path uses — not a parallel fake.
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    };

    run_headless<Counter>(cfg, [&](auto& rt) {
        std::printf("counter headless\n");

        // Ask the runtime for rects by NAME every time. Never hold a
        // `const Node*` across a tick(): each render rebuilds the tree
        // wholesale (that is what makes view() pure), so the pointer dangles.
        check(rt.node_rect("inc") && rt.node_rect("dec") && rt.node_rect("step"),
              "named buttons exist in the laid-out tree");

        check(rt.model().count == 0, "starts at zero");
        check(rt.window().title() == "mayag — counter", "Cmd::set_title was interpreted");

        // A real click: move, press, release, then one runtime tick.
        rt.click("inc");
        check(rt.model().count == 1, "clicking + increments");

        rt.click("dec");
        check(rt.model().count == 0, "clicking - decrements");

        // Cycle the step size, then verify it applies.
        rt.click("step");
        check(rt.model().step == 5, "clicking S cycles the step to 5");

        rt.click("inc");
        check(rt.model().count == 5, "increment now uses the new step");

        // Press and DRAG OFF before releasing: must not fire.
        const Vec2 c = rt.center_of("inc");
        rt.window().push(MouseMove{c, {}, {}});
        rt.window().push(MouseDown{c, MouseButton::left, {}, 1});
        rt.window().push(MouseMove{c + Vec2{200, 0}, {200, 0}, {}});
        rt.window().push(MouseUp{c + Vec2{200, 0}, MouseButton::left, {}});
        rt.tick();
        check(rt.model().count == 5, "press then drag off does NOT click");

        // Keyboard.
        rt.window().press_key(Key::up);
        rt.tick();
        check(rt.model().count == 10, "arrow key increments");
        check(rt.model().celebrating, "crossing 10 starts the celebration");

        // The animation subscription now exists, so frames drive it.
        rt.window().drive_frames(true);
        for (int i = 0; i < 60 && rt.model().celebrating; ++i) rt.tick();
        check(!rt.model().celebrating, "celebration ends on its own");
        rt.window().drive_frames(false);

        rt.window().press_key(Key::r);
        rt.tick();
        check(rt.model().count == 0, "R resets");
        check(rt.window().get_clipboard() == "0", "Cmd::write_clipboard was interpreted");

        // Hover styling comes from Ctx, not the Model.
        rt.window().move_to(rt.center_of("inc"));
        rt.tick();
        check(rt.input().is_hovered(node_id("inc")), "hover is tracked");
        check(rt.model().count == 0, "hovering does not change application state");

        // Pixels actually changed.
        check(rt.window().frames_presented() > 1, "frames were presented");
        (void)image::write_png("counter_headless.png", rt.window().read_pixels(),
                               static_cast<int>(cfg.size.x), static_cast<int>(cfg.size.y));

        rt.window().press_key(Key::escape);
        rt.tick();
        check(rt.finished(), "escape quits");
    });

    std::printf("%s\n", failures == 0 ? "\nPASS" : "\nFAIL");
    return failures == 0 ? 0 : 1;
}
