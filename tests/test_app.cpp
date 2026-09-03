// tests/test_app.cpp — the application runtime
//
// The layer that makes mayag a GUI framework rather than an image generator.
// These tests drive the REAL runtime (`Runtime<P, Headless>`) — the same code
// path `run<P>()` uses on a real window — so a pass here is evidence about
// shipped behaviour, not about a parallel test harness.
//
// What is asserted, in order of how badly it breaks an app if wrong:
//   1. update() is pure          — same inputs, same outputs, no I/O
//   2. clicks hit the right node — including the cases everyone gets wrong
//   3. effects are interpreted   — Cmd actually reaches the platform
//   4. subscriptions are dynamic — timers/animations start and STOP by state
//   5. the whole thing is deterministic and replayable

#include <mayag/mayag.hpp>

#include <cstdio>
#include <string>
#include <variant>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int failures = 0;
int checks   = 0;

void check(bool cond, const std::string& what) {
    ++checks;
    if (!cond) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* n) { std::printf("\n%s\n", n); }

// ════════════════════════════════════════════════════════════════════════
// A representative app: buttons, keyboard, a text field, an animation,
// async work, and a drag. Enough surface to exercise every runtime path.
// ════════════════════════════════════════════════════════════════════════

struct TestApp {
    struct Model {
        int         count    = 0;
        std::string text;
        bool        animating = false;
        double      phase    = 0.0;
        float       slider   = 0.5f;
        bool        loaded   = false;
        int         hovers   = 0;
    };

    struct Inc {}; struct Dec {}; struct Animate {}; struct Tick { double dt; };
    struct Typed { std::string s; }; struct Slide { float v; };
    struct Load {}; struct Loaded { int value; };
    struct Entered {}; struct Quit {};

    using Msg = std::variant<Inc, Dec, Animate, Tick, Typed, Slide,
                             Load, Loaded, Entered, Quit>;

    static std::pair<Model, Cmd<Msg>> init() {
        return {Model{}, Cmd<Msg>::set_title("test-app")};
    }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        return std::visit([&](auto&& e) -> std::pair<Model, Cmd<Msg>> {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, Inc>) {
                m.count++;
                return {m, Cmd<Msg>::write_clipboard(std::to_string(m.count))};
            }
            else if constexpr (std::is_same_v<T, Dec>) { m.count--; return {m, Cmd<Msg>::none()}; }
            else if constexpr (std::is_same_v<T, Animate>) {
                m.animating = true; m.phase = 0.0;
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Tick>) {
                m.phase += e.dt;
                if (m.phase >= 0.25) m.animating = false;
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Typed>) { m.text += e.s; return {m, Cmd<Msg>::none()}; }
            else if constexpr (std::is_same_v<T, Slide>) { m.slider = e.v; return {m, Cmd<Msg>::none()}; }
            else if constexpr (std::is_same_v<T, Load>) {
                return {m, Cmd<Msg>::task([] { return Msg{Loaded{42}}; })};
            }
            else if constexpr (std::is_same_v<T, Loaded>) {
                m.loaded = true; m.count = e.value;
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Entered>) { m.hovers++; return {m, Cmd<Msg>::none()}; }
            else { return {m, Cmd<Msg>::quit()}; }
        }, msg);
    }

    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = c.theme;
        return v(h(box() | size(80, 40) | bg(t.accent)   | id<"inc">,
                   box() | size(80, 40) | bg(t.danger)   | id<"dec">,
                   box() | size(80, 40) | bg(t.warning)  | id<"hoverme">) | gap(10),
                 box() | size(200, 24) | bg(t.surface) | id<"slider">,
                 text_of(m.text) | font(14) | fg(t.text_primary) | id<"label">,
                 text_owned(std::to_string(m.count)) | font(20) | fg(t.text_primary))
             | gap(12) | pad(20)
             | width(pct(100)) | height(pct(100))
             | bg(m.animating ? t.success : t.background);
    }

    static Sub<Msg> subscribe(const Model& m) {
        return Sub<Msg>::batch(
            Sub<Msg>::on_click<"inc">(Inc{}),
            Sub<Msg>::on_click<"dec">(Dec{}),
            Sub<Msg>::on_enter<"hoverme">(Entered{}),
            Sub<Msg>::on_drag<"slider">([](Vec2 local) {
                return Msg{Slide{num::saturate(local.x / 200.0f)}};
            }),
            Sub<Msg>::on_key(Key::space, Animate{}),
            Sub<Msg>::on_key(Key::l, Load{}),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_text([](std::string_view s) { return Msg{Typed{std::string{s}}}; }),
            Sub<Msg>::on_close(Quit{}),
            when(m.animating,
                 Sub<Msg>::every_frame([](FrameEvent f) { return Msg{Tick{f.delta}}; })));
    }
};

static_assert(Program<TestApp>);

const AppConfig kCfg{.title = "test-app", .size = {480, 320}};

// ════════════════════════════════════════════════════════════════════════

/// update() is a PURE FUNCTION. This is the property everything else rests
/// on: no window, no GPU, no clock, no mocks — just call it.
void test_update_purity() {
    section("update() purity");

    using M = TestApp::Model;
    using Msg = TestApp::Msg;

    const M start{.count = 5};

    auto [a1, c1] = TestApp::update(start, Msg{TestApp::Inc{}});
    auto [a2, c2] = TestApp::update(start, Msg{TestApp::Inc{}});
    check(a1.count == a2.count && a1.count == 6, "same input yields same output");

    // The input model is untouched — update takes by value and returns anew.
    check(start.count == 5, "update does not mutate its argument");

    // Effects are DATA, inspectable without performing them.
    check(!c1.is_none(), "Inc returns a clipboard effect as data");
    check(TestApp::update(start, Msg{TestApp::Quit{}}).second.is_quit(),
          "Quit returns a quit effect as data");

    // A whole interaction sequence is testable with no runtime at all.
    M m{};
    for (int i = 0; i < 10; ++i) m = TestApp::update(m, Msg{TestApp::Inc{}}).first;
    for (int i = 0; i < 3;  ++i) m = TestApp::update(m, Msg{TestApp::Dec{}}).first;
    check(m.count == 7, "a sequence of updates composes without a window");
}

/// Cmd and Sub are functors — `map` is what lets components nest.
void test_functor_laws() {
    section("Cmd/Sub functor");

    using ChildMsg = int;
    struct Wrapped { int v; bool operator==(const Wrapped&) const = default; };

    auto child = Cmd<ChildMsg>::batch(Cmd<ChildMsg>::quit(),
                                      Cmd<ChildMsg>::set_title("child"));
    auto lifted = child.map([](ChildMsg v) { return Wrapped{v}; });
    check(!lifted.is_none(), "a child Cmd lifts into the parent's Msg type");

    // Identity: mapping with identity changes nothing observable.
    auto id_mapped = Cmd<ChildMsg>::quit().map([](ChildMsg v) { return v; });
    check(id_mapped.is_quit(), "map(id) preserves the effect");

    // Batch flattening keeps the tree from growing without bound across
    // frames — a real leak in naive implementations.
    auto nested = Cmd<ChildMsg>::batch(
        Cmd<ChildMsg>::batch(Cmd<ChildMsg>::none(), Cmd<ChildMsg>::none()),
        Cmd<ChildMsg>::none());
    check(nested.is_none(), "batching only `none` collapses to `none`");
}

/// The interaction state machine: the cases every GUI must get right.
void test_interaction() {
    section("interaction");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        check(rt.node_rect("inc").has_value(), "named nodes are addressable after layout");

        rt.click("inc");
        check(rt.model().count == 1, "click fires on the node under the cursor");

        rt.click("dec");
        check(rt.model().count == 0, "click targets the correct sibling");

        // Clicking empty space must not fire anything.
        rt.window().click({470, 310});
        rt.tick();
        check(rt.model().count == 0, "clicking outside every widget fires nothing");

        // THE cancel affordance: press, drag away, release. Must not click.
        const Vec2 c = rt.center_of("inc");
        rt.window().push(MouseMove{c, {}, {}});
        rt.window().push(MouseDown{c, MouseButton::left, {}, 1});
        rt.window().push(MouseMove{{460, 300}, {}, {}});
        rt.window().push(MouseUp{{460, 300}, MouseButton::left, {}});
        rt.tick();
        check(rt.model().count == 0, "press then release elsewhere does NOT click");

        // Press, drag away, drag BACK, release: this IS a click.
        rt.window().push(MouseMove{c, {}, {}});
        rt.window().push(MouseDown{c, MouseButton::left, {}, 1});
        rt.window().push(MouseMove{{460, 300}, {}, {}});
        rt.window().push(MouseMove{c, {}, {}});
        rt.window().push(MouseUp{c, MouseButton::left, {}});
        rt.tick();
        check(rt.model().count == 1, "dragging back onto the node still clicks");

        // enter/leave fire exactly once per boundary crossing, not per motion.
        const int before = rt.model().hovers;
        const Vec2 h = rt.center_of("hoverme");
        rt.window().move_to(h);
        rt.tick();
        check(rt.model().hovers == before + 1, "entering a node fires `enter` once");

        rt.window().move_to(h + Vec2{2, 2});
        rt.tick();
        check(rt.model().hovers == before + 1, "moving WITHIN a node does not re-fire");

        rt.window().move_to({470, 310});
        rt.tick();
        rt.window().move_to(h);
        rt.tick();
        check(rt.model().hovers == before + 2, "leaving and re-entering fires again");

        // REGRESSION: consecutive clicks must be SEPARATE clicks.
        //
        // The runtime's clock previously only advanced on FrameEvent, so a
        // non-animating app saw t=0 forever; every click landed inside the
        // double-click window of the one before it, and every second click
        // was silently swallowed as the tail of a double. Three clicks in a
        // row must increment three times.
        const int base = rt.model().count;
        rt.click("inc");
        rt.click("inc");
        rt.click("inc");
        check(rt.model().count == base + 3,
              "three deliberate clicks fire three times (got " +
              std::to_string(rt.model().count - base) + ")");
    });
}

/// The click model: ONE gesture per click, carrying its count.
///
/// The earlier design emitted `click` and then `double_click` as separate
/// gestures, which meant an app handling both fired its single-click action
/// on every double. It also had no notion of a triple click, and it
/// recomputed the count from timestamps while THROWING AWAY the authoritative
/// count the platform had already given it.
void test_click_counts() {
    section("click counts");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const int base = rt.model().count;

        // A double click must fire `on_click` EXACTLY ONCE (for the first
        // click of the pair), not twice.
        rt.window().double_click(rt.center_of("inc"));
        rt.tick();
        check(rt.model().count == base + 1,
              "a double click fires a single-click handler once, not twice");

        // Separated clicks are independent singles.
        const int b2 = rt.model().count;
        rt.click("inc");
        rt.click("inc");
        check(rt.model().count == b2 + 2, "deliberate clicks are separate");
    });

    // Counts are produced at the interaction layer, so test them directly:
    // the runtime path above cannot show a triple.
    {
        Node root = (v(box() | size(100, 50) | bg(colors::red) | dsl::id<"b">)).build();
        layout::layout_tree(root, {200, 100}, layout::default_measurer());
        const Vec2 c = root.children()[0].frame().center();

        // ---- the platform reports the count: use it verbatim ----
        {
            Interaction in;
            std::vector<int> counts;
            for (int n = 1; n <= 4; ++n) {
                (void)in.handle(MouseDown{c, MouseButton::left, {}, n}, root, n * 0.1);
                for (const auto& g : in.handle(MouseUp{c, MouseButton::left, {}}, root, n * 0.1)) {
                    if (g.kind == Gesture::Kind::click) counts.push_back(g.click_count);
                }
            }
            check(counts == std::vector<int>({1, 2, 3, 4}),
                  "platform-reported click counts pass through unchanged");
        }

        // ---- no platform count: synthesise, to any depth ----
        {
            Interaction in;
            std::vector<int> counts;
            for (int n = 0; n < 4; ++n) {
                (void)in.handle(MouseDown{c, MouseButton::left, {}, 0}, root, n * 0.05);
                for (const auto& g : in.handle(MouseUp{c, MouseButton::left, {}}, root, n * 0.05)) {
                    if (g.kind == Gesture::Kind::click) counts.push_back(g.click_count);
                }
            }
            check(counts == std::vector<int>({1, 2, 3, 4}),
                  "synthesised counts reach triple and beyond, not just double");
        }

        // ---- too slow: the sequence restarts ----
        {
            Interaction in;
            std::vector<int> counts;
            for (int n = 0; n < 3; ++n) {
                (void)in.handle(MouseDown{c, MouseButton::left, {}, 0}, root, n * 2.0);
                for (const auto& g : in.handle(MouseUp{c, MouseButton::left, {}}, root, n * 2.0)) {
                    if (g.kind == Gesture::Kind::click) counts.push_back(g.click_count);
                }
            }
            check(counts == std::vector<int>({1, 1, 1}), "slow clicks each count as 1");
        }

        // ---- fast but moving: also restarts ----
        //
        // Distance matters as much as time. Two rapid clicks in different
        // places are two clicks, not a double — otherwise a fast user
        // clicking down a list triggers double-click actions.
        {
            Interaction in;
            std::vector<int> counts;
            for (int n = 0; n < 3; ++n) {
                const Vec2 p = c + Vec2{static_cast<float>(n) * 40.0f, 0.0f};
                (void)in.handle(MouseDown{p, MouseButton::left, {}, 0}, root, n * 0.05);
                for (const auto& g : in.handle(MouseUp{p, MouseButton::left, {}}, root, n * 0.05)) {
                    if (g.kind == Gesture::Kind::click) counts.push_back(g.click_count);
                }
            }
            check(counts.size() >= 1 && counts[0] == 1,
                  "rapid clicks at different positions do not chain");
            for (int n : counts) {
                check(n == 1, "each moved click counts as 1");
            }
        }

        // ---- exactly one gesture per click ----
        {
            Interaction in;
            int clicks = 0;
            (void)in.handle(MouseDown{c, MouseButton::left, {}, 2}, root, 0.1);
            for (const auto& g : in.handle(MouseUp{c, MouseButton::left, {}}, root, 0.1)) {
                if (g.kind == Gesture::Kind::click) ++clicks;
            }
            check(clicks == 1, "a double click emits ONE click gesture, not two");
        }
    }
}

/// Pointer capture: a drag keeps targeting the node it started on.
void test_drag_capture() {
    section("drag capture");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const auto slider = rt.node_rect("slider");
        check(slider.has_value(), "slider exists");
        if (!slider) return;

        const Vec2 start{slider->left() + 20.0f, slider->center().y};
        rt.window().push(MouseMove{start, {}, {}});
        rt.window().push(MouseDown{start, MouseButton::left, {}, 1});
        rt.tick();

        // Drag far past the slider's right edge. A naive implementation
        // re-hit-tests every move and loses the drag the instant the cursor
        // exits the widget — which is why real sliders feel broken.
        const Vec2 far{slider->right() + 300.0f, slider->center().y + 200.0f};
        rt.window().push(MouseMove{far, far - start, {}});
        rt.tick();
        check(rt.model().slider > 0.9f,
              "drag keeps targeting the pressed node after leaving its bounds");

        rt.window().push(MouseUp{far, MouseButton::left, {}});
        rt.tick();

        // And after release, motion no longer drives it.
        const float held = rt.model().slider;
        rt.window().move_to({slider->left() + 5.0f, slider->center().y});
        rt.tick();
        check(rt.model().slider == held, "release ends the capture");
    });
}

/// Cmd values must actually reach the platform.
void test_effects() {
    section("effect interpretation");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        check(rt.window().title() == "test-app", "Cmd::set_title reached the window");

        rt.click("inc");
        check(rt.window().get_clipboard() == "1", "Cmd::write_clipboard reached the window");

        rt.click("inc");
        check(rt.window().get_clipboard() == "2", "and again with the new value");

        // Cmd::task runs off-thread and delivers back to update().
        rt.window().press_key(Key::l);
        rt.tick();
        for (int i = 0; i < 200 && !rt.model().loaded; ++i) {
            rt.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(rt.model().loaded, "Cmd::task delivered its result back to update()");
        check(rt.model().count == 42, "the async value reached the model");

        rt.window().press_key(Key::escape);
        rt.tick();
        check(rt.finished(), "Cmd::quit stops the runtime");
    });
}

/// Subscriptions are a function of state, so they start and STOP by themselves.
void test_subscriptions() {
    section("subscriptions");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        // Not animating: no frame subscription, so the runtime should be
        // willing to block. This is the property that makes an idle mayag app
        // cost 0% CPU.
        check(!TestApp::subscribe(rt.model()).wants_frames(),
              "an idle model requests no frames");

        rt.window().press_key(Key::space);
        rt.tick();
        check(rt.model().animating, "space starts the animation");
        check(TestApp::subscribe(rt.model()).wants_frames(),
              "an animating model requests frames");

        // Drive it to completion.
        rt.window().drive_frames(true);
        for (int i = 0; i < 120 && rt.model().animating; ++i) rt.tick();
        rt.window().drive_frames(false);

        check(!rt.model().animating, "the animation ends from its own state");
        check(!TestApp::subscribe(rt.model()).wants_frames(),
              "and the frame subscription disappears with it");

        // Text input is separate from key events, so a shortcut and the
        // character it would type never both fire.
        rt.window().type("hi");
        rt.tick();
        check(rt.model().text == "hi", "text events append");
    });
}

/// Same events in, same pixels out.
void test_determinism() {
    section("determinism");

    auto run_script = [] {
        std::vector<std::uint8_t> pixels;
        int final_count = 0;
        run_headless<TestApp>(kCfg, [&](auto& rt) {
            rt.click("inc");
            rt.click("inc");
            rt.click("dec");
            rt.window().type("abc");
            rt.tick();
            rt.window().press_key(Key::space);
            rt.tick();
            rt.window().drive_frames(true);
            for (int i = 0; i < 30; ++i) rt.tick();
            pixels = rt.window().read_pixels();
            final_count = rt.model().count;
        });
        return std::pair{pixels, final_count};
    };

    const auto [px1, n1] = run_script();
    const auto [px2, n2] = run_script();

    check(n1 == n2 && n1 == 1, "the same script yields the same model");
    check(px1 == px2, "the same script yields byte-identical pixels");
    check(!px1.empty(), "and those pixels exist");
}

/// The view is a pure function of (Model, Ctx) — including hover state, which
/// lives in Ctx rather than the Model.
void test_view_purity() {
    section("view purity");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const auto before = rt.model();

        // Hovering changes what is DRAWN without touching application state
        // (beyond the counter this app deliberately keeps, to prove the
        // distinction is a choice rather than an accident).
        rt.window().move_to(rt.center_of("inc"));
        rt.tick();
        check(rt.input().is_hovered(node_id("inc")), "runtime tracks hover");
        check(rt.model().count == before.count, "hovering did not change the count");

        // Two identical renders of the same model produce identical trees.
        Ctx c1{{480, 320}, 1.0f, 0.0, themes::midnight, nullptr};
        const Node a = TestApp::view(before, c1);
        const Node b = TestApp::view(before, c1);
        check(a.count() == b.count(), "view is deterministic");
    });
}

/// Keyboard navigation comes from the runtime, not from each app.
void test_focus() {
    section("focus");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        rt.window().press_key(Key::tab);
        rt.tick();
        const auto first = rt.input().focused();
        check(first != 0, "tab moves focus to a named node");

        rt.window().press_key(Key::tab);
        rt.tick();
        check(rt.input().focused() != first, "tab advances");

        rt.window().press_key(Key::tab, Mods{.shift = true});
        rt.tick();
        check(rt.input().focused() == first, "shift-tab goes back");

        // Clicking moves focus too — including onto nothing, which is how a
        // text field is dismissed.
        rt.click("inc");
        check(rt.input().focused() == node_id("inc"), "clicking focuses the target");

        rt.window().click({470, 310});
        rt.tick();
        check(rt.input().focused() == 0, "clicking empty space clears focus");
    });
}

/// The runtime only repaints when something changed.
/// The runtime must never go to sleep owing the screen a frame.
///
/// This is the "pressed a theme chip and it hung" bug. The loop is
/// wait -> events -> update -> render, so a message from ANYWHERE other than
/// the window (a worker thread, a timer, application code calling send())
/// leaves the model changed and dirty_ set — and then the loop blocks for a
/// window event that may never arrive. The frame sits unrendered and the app
/// looks frozen until the user happens to jiggle the mouse.
void test_never_sleeps_dirty() {
    section("responsiveness");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const auto before = rt.window().frames_presented();

        // A state change that did NOT come from a window event.
        rt.send(TestApp::Msg{TestApp::Inc{}});
        check(rt.model().count == 1, "send() updated the model");

        // One tick must be enough to get it on screen. If the runtime chose
        // to block here, this frame would never be presented.
        rt.tick();
        check(rt.window().frames_presented() > before,
              "a repaint owed after send() is presented on the very next tick");
    });

    // And once the screen is caught up, the runtime must go back to sleep
    // rather than spinning — the opposite failure, equally bad.
    run_headless<TestApp>(kCfg, [](auto& rt) {
        for (int i = 0; i < 5; ++i) rt.tick();      // settle
        const auto settled = rt.window().frames_presented();
        for (int i = 0; i < 20; ++i) rt.tick();
        check(rt.window().frames_presented() == settled,
              "an idle runtime presents nothing (" +
              std::to_string(rt.window().frames_presented() - settled) + " extra frames)");
    });
}

/// Hover motion that changes nothing must not force a repaint.
///
/// macOS delivers a mouse-moved event per pixel of cursor travel — 117 across
/// 200 polls in a measurement. Repainting on each turned idle mouse movement
/// into a 650 fps spin that pinned a core.
void test_motion_does_not_spin() {
    section("motion economy");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        // Park the cursor over empty space and settle.
        rt.window().move_to({460, 300});
        rt.tick();
        for (int i = 0; i < 3; ++i) rt.tick();
        const auto settled = rt.window().frames_presented();

        // Wiggle WITHIN the same empty region: hover state never changes.
        for (int i = 0; i < 30; ++i) {
            rt.window().move_to({460.0f + static_cast<float>(i % 3), 300.0f});
            rt.tick();
        }
        check(rt.window().frames_presented() == settled,
              "motion that does not change hover state does not repaint (" +
              std::to_string(rt.window().frames_presented() - settled) + " frames)");

        // Moving ONTO a widget must repaint, or hover styling would not work.
        rt.window().move_to(rt.center_of("hoverme"));
        rt.tick();
        check(rt.window().frames_presented() > settled,
              "but crossing into a widget does repaint");
    });
}

void test_render_economy() {
    section("render economy");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const auto after_boot = rt.window().frames_presented();
        check(after_boot >= 1, "boot paints one frame");

        // Ticks with no events and no animation must not repaint.
        for (int i = 0; i < 10; ++i) rt.tick();
        check(rt.window().frames_presented() == after_boot,
              "idle ticks do not repaint (" +
              std::to_string(rt.window().frames_presented() - after_boot) + " extra)");

        rt.click("inc");
        check(rt.window().frames_presented() > after_boot, "a click does repaint");
    });
}

}  // namespace

int main() {
    std::printf("mayag app runtime\n=================");

    test_update_purity();
    test_functor_laws();
    test_interaction();
    test_click_counts();
    test_drag_capture();
    test_effects();
    test_subscriptions();
    test_determinism();
    test_view_purity();
    test_focus();
    test_never_sleeps_dirty();
    test_motion_does_not_spin();
    test_render_economy();

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
