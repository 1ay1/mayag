// tests/test_app.cpp — the window host on jaal
//
// jaal runs the program (update, Cmd, Sub, timers, tasks); jaal's own suite
// tests that. These tests drive mayag's window host through headless_app<P>,
// the same host and the same jaal kernel run<P>() uses on a real window, and
// check what mayag adds: input becomes gestures on the right node, window
// effects reach the window, frames are only drawn when something changed,
// and the busy-loop warning fires only for real runaways.

#include <mayag/mayag.hpp>
#include <mayag/widget/text_field.hpp>
#include <mayag/widget/typography.hpp>

#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

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
// async work, and a drag.
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
    struct Hovered {}; struct Quit {};

    using Msg = std::variant<Inc, Dec, Animate, Tick, Typed, Slide, Load, Loaded, Hovered, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title, write_clipboard>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model&) { return SetTitle{"test-app"}; }

    static Cmd update(Model& m, Inc) { ++m.count; return WriteClipboard{std::to_string(m.count)}; }
    static Cmd update(Model& m, Dec) { --m.count; return {}; }
    static Cmd update(Model& m, Animate) { m.animating = true; m.phase = 0.0; return {}; }
    static Cmd update(Model& m, Tick t) {
        m.phase += t.dt;
        if (m.phase >= 1.0) m.animating = false;
        return {};
    }
    static Cmd update(Model& m, const Typed& t) { m.text += t.s; return {}; }
    static Cmd update(Model& m, Slide s) { m.slider = s.v; return {}; }
    static Cmd update(Model&, Load) {
        return Cmd::perform<Loaded>([] { return 42; });
    }
    static Cmd update(Model& m, Loaded l) { m.loaded = true; m.count = l.value; return {}; }
    static Cmd update(Model& m, Hovered) { ++m.hovers; return {}; }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    static Node view(const Model& m) {
        const Theme& t = ctx().theme;
        return v(h(box() | size(80, 40) | bg(t.accent)   | dsl::id<"inc">,
                   box() | size(80, 40) | bg(t.danger)   | dsl::id<"dec">,
                   box() | size(80, 40) | bg(t.warning)  | dsl::id<"hoverme">) | gap(10),
                 box() | size(200, 24) | bg(t.surface) | dsl::id<"slider">,
                 text_owned(std::to_string(m.count)) | font(16) | fg(t.text_primary))
             | gap(10) | pad(10)
             | width(pct(100)) | height(pct(100))
             | bg(m.animating ? t.success : t.background);
    }

    static std::optional<Msg> on(const Model&, const Clicked& c) {
        if (c.count != 1) return std::nullopt;
        if (is(c, "inc")) return Inc{};
        if (is(c, "dec")) return Dec{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Entered& e) {
        if (e.node == node_id("hoverme")) return Hovered{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Dragged& d) {
        if (d.node != node_id("slider")) return std::nullopt;
        return Slide{num::clamp(d.local.x / 200.0f, 0.0f, 1.0f)};
    }
    static std::optional<Msg> on(const Model&, const TextEvent& t) { return Typed{t.text}; }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{{Key::space, Animate{}}, {Key::l, Load{}},
                                      {Key::escape, Quit{}}};
        return keys(k);
    }

    static Sub subscribe(const Model& m) {
        return when(m.animating, Sub::on_frame([](double dt) { return Tick{dt}; }));
    }
};
static_assert(mayag::Program<TestApp>);

const AppConfig kCfg{.title = "test-app", .size = {480, 320}};

/// Animates forever: the thing the busy-loop detector exists to catch.
struct Spinner {
    struct Model { double t = 0.0; };
    struct Tick { double dt; };
    using Msg = std::variant<Tick>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;
    static Cmd update(Model& m, Tick t) { m.t += t.dt; return {}; }
    static Node view(const Model& m) {
        const float w = 40.0f + static_cast<float>(static_cast<int>(m.t * 60.0) % 40);
        return v(box() | size(w, 40) | bg(ctx().theme.accent));
    }
    static Sub subscribe(const Model&) { return Sub::on_frame([](double dt) { return Tick{dt}; }); }
};

/// Like examples/reactive.cpp: pointer motion earns a short burst of frames.
struct Follower {
    struct Model { Vec2 p{}; int settle = 0; };
    struct Moved { Vec2 p; };
    struct Tick { double dt; };
    using Msg = std::variant<Moved, Tick>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;
    static Cmd update(Model& m, Moved mv) { m.p = mv.p; m.settle = 45; return {}; }
    static Cmd update(Model& m, Tick) { if (m.settle > 0) --m.settle; return {}; }
    static Node view(const Model& m) {
        return v(box() | size(m.p.x + 1.0f, 40) | bg(ctx().theme.accent));
    }
    static std::optional<Msg> on(const Model&, const MouseMove& e) { return Moved{e.position}; }
    static Sub subscribe(const Model& m) {
        return when(m.settle > 0, Sub::on_frame([](double dt) { return Tick{dt}; }));
    }
};

// ════════════════════════════════════════════════════════════════════════

/// update() changes the model and nothing else; a sequence composes with no
/// window at all.
void test_update_purity() {
    section("update() purity");

    TestApp::Model a{};
    TestApp::Model b{};
    (void)mayag::update_of<TestApp>(a, TestApp::Msg{TestApp::Inc{}});
    (void)mayag::update_of<TestApp>(b, TestApp::Msg{TestApp::Inc{}});
    check(a.count == b.count && a.count == 1, "same message, same model");

    const auto cmd = mayag::update_of<TestApp>(a, TestApp::Msg{TestApp::Dec{}});
    check(cmd.is_none(), "an update with no effect returns an empty Cmd");

    TestApp::Model m{};
    for (int i = 0; i < 10; ++i) (void)mayag::update_of<TestApp>(m, TestApp::Msg{TestApp::Inc{}});
    for (int i = 0; i < 3;  ++i) (void)mayag::update_of<TestApp>(m, TestApp::Msg{TestApp::Dec{}});
    check(m.count == 7, "a sequence of updates composes without a window");
}

/// A program's Cmd lifts into a parent's Msg type; window effects survive it.
void test_functor_laws() {
    section("Cmd functor");

    using C = mayag::Cmd<int, set_title>;
    struct Wrapped { int v; };

    C child = C::batch(C::quit(), C(SetTitle{"child"}));
    auto lifted = std::move(child).map([](int v) { return Wrapped{v}; });
    check(!lifted.is_none(), "a child Cmd lifts into the parent's Msg type");

    C title{SetTitle{"t"}};
    auto same = std::move(title).map([](int v) { return v; });
    const auto* t = std::get_if<SetTitle>(&same.inner);
    check(t != nullptr && t->title == "t", "map(id) keeps a window effect intact");

    check(C::batch(C::batch(C::none(), C::none()), C::none()).is_none(),
          "batching only `none` collapses to `none`");
}

/// The interaction state machine, through the host.
void test_interaction() {
    section("interaction");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        check(rt.node_rect("inc").has_value(), "named nodes are addressable after layout");

        rt.click("inc");
        check(rt.model().count == 1, "click fires on the node under the cursor");

        rt.click("dec");
        check(rt.model().count == 0, "click targets the correct sibling");

        rt.window().click({470, 310});
        rt.tick();
        check(rt.model().count == 0, "clicking outside every widget fires nothing");

        // Press, drag away, release: must not click.
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

        // Entered fires once per boundary crossing, not per motion.
        const int before = rt.model().hovers;
        const Vec2 h = rt.center_of("hoverme");
        rt.window().move_to(h);
        rt.tick();
        check(rt.model().hovers == before + 1, "entering a node fires Entered once");

        rt.window().move_to(h + Vec2{2, 2});
        rt.tick();
        check(rt.model().hovers == before + 1, "moving WITHIN a node does not re-fire");

        rt.window().move_to({470, 310});
        rt.tick();
        rt.window().move_to(h);
        rt.tick();
        check(rt.model().hovers == before + 2, "leaving and re-entering fires again");

        // Consecutive deliberate clicks are separate clicks, not doubles.
        const int base = rt.model().count;
        rt.click("inc");
        rt.click("inc");
        rt.click("inc");
        check(rt.model().count == base + 3,
              "three deliberate clicks fire three times (got " +
              std::to_string(rt.model().count - base) + ")");
    });
}

/// ONE click gesture per click, carrying its count.
void test_click_counts() {
    section("click counts");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const int base = rt.model().count;
        rt.window().double_click(rt.center_of("inc"));
        rt.tick();
        check(rt.model().count == base + 1,
              "a double click fires a single-click handler once, not twice");

        const int b2 = rt.model().count;
        rt.click("inc");
        rt.click("inc");
        check(rt.model().count == b2 + 2, "deliberate clicks are separate");
    });

    // Counts come from the interaction layer; test the depths directly.
    Node root = (v(box() | size(100, 50) | bg(colors::red) | dsl::id<"b">)).build();
    layout::layout_tree(root, {200, 100}, layout::default_measurer());
    const Vec2 c = root.children()[0].frame().center();

    const auto run = [&](auto pos_of, auto time_of, int reported, int n) {
        Interaction in;
        std::vector<int> counts;
        for (int i = 0; i < n; ++i) {
            const Vec2 p = pos_of(i);
            const double t = time_of(i);
            (void)in.handle(MouseDown{p, MouseButton::left, {}, reported ? i + 1 : 0}, root, t);
            for (const auto& g : in.handle(MouseUp{p, MouseButton::left, {}}, root, t))
                if (g.kind == Gesture::Kind::click) counts.push_back(g.click_count);
        }
        return counts;
    };
    const auto here = [&](int) { return c; };

    check(run(here, [](int i) { return (i + 1) * 0.1; }, 1, 4) == std::vector<int>({1, 2, 3, 4}),
          "platform-reported click counts pass through unchanged");
    check(run(here, [](int i) { return i * 0.05; }, 0, 4) == std::vector<int>({1, 2, 3, 4}),
          "synthesised counts reach triple and beyond, not just double");
    check(run(here, [](int i) { return i * 2.0; }, 0, 3) == std::vector<int>({1, 1, 1}),
          "slow clicks each count as 1");
    const auto moved = run([&](int i) { return c + Vec2{static_cast<float>(i) * 40.0f, 0.0f}; },
                           [](int i) { return i * 0.05; }, 0, 3);
    bool all_one = !moved.empty();
    for (int n : moved) all_one = all_one && n == 1;
    check(all_one, "rapid clicks at different positions do not chain");

    Interaction in;
    int clicks = 0;
    (void)in.handle(MouseDown{c, MouseButton::left, {}, 2}, root, 0.1);
    for (const auto& g : in.handle(MouseUp{c, MouseButton::left, {}}, root, 0.1))
        if (g.kind == Gesture::Kind::click) ++clicks;
    check(clicks == 1, "a double click emits ONE click gesture, not two");
}

/// A drag keeps targeting the node it started on.
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

        const Vec2 far{slider->right() + 300.0f, slider->center().y + 200.0f};
        rt.window().push(MouseMove{far, far - start, {}});
        rt.tick();
        check(rt.model().slider > 0.9f,
              "drag keeps targeting the pressed node after leaving its bounds");

        rt.window().push(MouseUp{far, MouseButton::left, {}});
        rt.tick();
        const float held = rt.model().slider;
        rt.window().move_to({slider->left() + 5.0f, slider->center().y});
        rt.tick();
        check(rt.model().slider == held, "release ends the capture");
    });
}

/// Window effects reach the window; jaal tasks come back to update().
void test_effects() {
    section("effects");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        check(rt.window().title() == "test-app", "SetTitle from init reached the window");

        rt.click("inc");
        check(rt.window().get_clipboard() == "1", "WriteClipboard reached the window");
        rt.click("inc");
        check(rt.window().get_clipboard() == "2", "and again with the new value");

        rt.press_key(Key::l);
        { const bool settled_ = rt.settle([](const auto& m) { return m.loaded; });
          check(settled_, "Cmd::perform delivered its result back to update()"); }
        check(rt.model().count == 42, "the async value reached the model");

        rt.press_key(Key::escape);
        check(rt.finished(), "Cmd::quit stops the program");
    });
}

/// The frame clock is a subscription: it starts and stops with the model.
void test_subscriptions() {
    section("subscriptions");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        check(TestApp::subscribe(rt.model()).is_none(), "an idle model requests no frames");
        check(!rt.host().frames_on(), "and the host's frame clock is off");

        rt.press_key(Key::space);
        check(rt.model().animating, "space starts the animation");
        check(rt.host().frames_on(), "the frame clock starts with it");

        rt.window().drive_frames(true);
        for (int i = 0; i < 120 && rt.model().animating; ++i) rt.tick();
        rt.window().drive_frames(false);

        check(!rt.model().animating, "the animation ends from its own state");
        check(!rt.host().frames_on(), "and the frame clock stops with it");

        rt.type("hi");
        check(rt.model().text == "hi", "text events reach on(TextEvent)");
    });
}

/// Sub::every, gated on the model.
struct IntervalApp {
    struct Model { int ticks = 0; bool running = true; };
    struct Tick {}; struct Stop {};
    using Msg = std::variant<Tick, Stop>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;
    static Cmd update(Model& m, Tick) { ++m.ticks; return {}; }
    static Cmd update(Model& m, Stop) { m.running = false; return {}; }
    static Node view(const Model&) { return dsl::box().build(); }
    static Sub subscribe(const Model& m) {
        return when(m.running, Sub::every(std::chrono::milliseconds(10), Tick{}));
    }
};
static_assert(mayag::Program<IntervalApp>);

void test_interval_timers() {
    section("interval timers");

    run_headless<IntervalApp>(kCfg, [](auto& rt) {
        { const bool settled_ = rt.settle([](const auto& m) { return m.ticks >= 5; }, std::chrono::milliseconds(1000));
          check(settled_, "Sub::every fires (" + std::to_string(rt.model().ticks) + " ticks)"); }

        rt.send(IntervalApp::Stop{});
        const int after_stop = rt.model().ticks;
        // Give a stray timer every chance to fire: 5 intervals.
        (void)rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(50));
        check(rt.model().ticks == after_stop,
              "an interval whose condition went false stops firing");
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
            rt.type("abc");
            rt.press_key(Key::space);
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

/// Hover lives in the host (Ctx), not the model.
void test_view_purity() {
    section("view purity");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const auto before = rt.model();
        rt.window().move_to(rt.center_of("inc"));
        rt.tick();
        check(rt.input().is_hovered(node_id("inc")), "the host tracks hover");
        check(rt.model().count == before.count, "hovering did not change the count");

        const Ctx c1{{480, 320}, 1.0f, 0.0, themes::midnight};
        const Node a = view_of<TestApp>(before, c1);
        const Node b = view_of<TestApp>(before, c1);
        check(a.count() == b.count(), "view is deterministic");
    });
}

/// Keyboard navigation comes from the host, not from each app.
void test_focus() {
    section("focus");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        rt.press_key(Key::tab);
        const auto first = rt.input().focused();
        check(first != 0, "tab moves focus to a named node");

        rt.press_key(Key::tab);
        check(rt.input().focused() != first, "tab advances");

        rt.press_key(Key::tab, Mods{.shift = true});
        check(rt.input().focused() == first, "shift-tab goes back");

        rt.click("inc");
        check(rt.input().focused() == node_id("inc"), "clicking focuses the target");

        rt.window().click({470, 310});
        rt.tick();
        check(rt.input().focused() == 0, "clicking empty space clears focus");

        // Space on a focused button: TestApp doesn't handle Activated, so the
        // key falls through to its own Space binding.
        rt.click("inc");
        rt.press_key(Key::space);
        check(rt.model().animating, "an unhandled activation falls through to on(KeyEvent)");
    });
}

/// A change from outside the window is drawn on the next tick, and an idle
/// app draws nothing.
void test_never_sleeps_dirty() {
    section("responsiveness");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const auto before = rt.window().frames_presented();
        rt.send(TestApp::Msg{TestApp::Inc{}});
        check(rt.model().count == 1, "send() updated the model");
        check(rt.window().frames_presented() > before,
              "the change is presented without waiting for window input");
    });

    run_headless<TestApp>(kCfg, [](auto& rt) {
        for (int i = 0; i < 5; ++i) rt.tick();
        const auto settled = rt.window().frames_presented();
        for (int i = 0; i < 20; ++i) rt.tick();
        check(rt.window().frames_presented() == settled,
              "an idle app presents nothing (" +
              std::to_string(rt.window().frames_presented() - settled) + " extra frames)");
    });
}

/// Pointer motion that changes no hover state draws nothing.
void test_motion_does_not_spin() {
    section("motion economy");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        rt.window().move_to({460, 300});
        rt.tick();
        for (int i = 0; i < 3; ++i) rt.tick();
        const auto settled = rt.window().frames_presented();

        for (int i = 0; i < 30; ++i) {
            rt.window().move_to({460.0f + static_cast<float>(i % 3), 300.0f});
            rt.tick();
        }
        check(rt.window().frames_presented() == settled,
              "motion that does not change hover state does not repaint (" +
              std::to_string(rt.window().frames_presented() - settled) + " frames)");

        rt.window().move_to(rt.center_of("hoverme"));
        rt.tick();
        check(rt.window().frames_presented() > settled,
              "but crossing into a widget does repaint");
    });
}

void test_accessibility() {
    section("accessibility");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const auto tree = a11y::snapshot(rt.tree(), rt.input().focused());
        check(tree.find_by_role(a11y::Role::text) != nullptr,
              "text nodes appear in the semantic tree automatically");
        check(!tree.interactive().empty(),
              "interactive elements are enumerable (" +
              std::to_string(tree.interactive().size()) + ")");
        const auto depth = [](auto&& self, const a11y::Element& e) -> int {
            int d = 0;
            for (const auto& c : e.children) d = num::max(d, self(self, c));
            return d + 1;
        };
        check(depth(depth, tree.root) <= 5,
              "the semantic tree is shallow (depth " +
              std::to_string(depth(depth, tree.root)) + ")");
    });

    constexpr Theme th = themes::midnight;
    auto ui = v(text<"Settings"> | font(20) | role(a11y::Role::heading),
                h(text<"Save"> | font(12)) | pad(6, 12) | bg(th.accent)
                  | role(a11y::Role::button) | label("Save document")
                  | dsl::id<"save">,
                box() | size(16, 16) | role(a11y::Role::checkbox)
                      | checked(true) | label("Enable sync") | dsl::id<"sync">,
                box() | size(80, 6) | role(a11y::Role::progress)
                      | a11y_value(0.4f) | label("Upload"))
            | gap(6) | pad(8);

    Node n = ui.build();
    layout::layout_tree(n, {200, 200}, layout::default_measurer());
    const auto tree = a11y::snapshot(n);

    const auto* save = tree.find_by_label("Save document");
    check(save != nullptr && save->role == a11y::Role::button,
          "an explicit label wins over the node's own text");
    const auto* sync = tree.find_by_label("Enable sync");
    check(sync != nullptr && sync->state.checked, "semantic state is carried (checked)");
    const auto* prog = tree.find_by_role(a11y::Role::progress);
    check(prog != nullptr && num::abs(prog->state.value - 0.4f) < 0.01f,
          "a value-bearing role reports its value");
    check(tree.find_by_role(a11y::Role::heading) != nullptr, "headings are found");
    int save_text_children = 0;
    if (save != nullptr)
        for (const auto& c : save->children)
            if (c.label == save->label) ++save_text_children;
    check(save_text_children == 0, "a control does not repeat its own label as a child");
}

/// One bad message or frame must not end the session (fault_policy::skip).
struct Fragile {
    struct Model { int count = 0; bool poison = false; };
    struct Inc {}; struct Explode {}; struct Poison {};
    using Msg = std::variant<Inc, Explode, Poison>;
    using Cmd = mayag::Cmd<Msg>;

    static Cmd update(Model& m, Inc) { ++m.count; return {}; }
    static Cmd update(Model& m, Explode) {
        m.count = -100;   // half-done work the kernel must roll back
        throw std::runtime_error("update failed");
    }
    static Cmd update(Model& m, Poison) { m.poison = true; return {}; }

    static Node view(const Model& m) {
        if (m.poison) throw std::runtime_error("view failed");
        return (v(text_owned(std::to_string(m.count)) | font(14)
                    | fg(ctx().theme.text_primary)) | pad(8)).build();
    }
};
static_assert(mayag::Program<Fragile>);

void test_error_boundary() {
    section("error boundary");

    AppConfig cfg{.size = {200, 120}};
    int reported = 0;
    cfg.on_error = [&](std::string_view, std::string_view) { ++reported; };

    run_headless<Fragile>(cfg, [&](auto& rt) {
        rt.send(Fragile::Msg{Fragile::Inc{}});
        rt.send(Fragile::Msg{Fragile::Inc{}});
        check(rt.model().count == 2, "normal messages work");

        rt.send(Fragile::Msg{Fragile::Explode{}});
        check(!rt.finished(), "a throwing update does not end the app");
        check(rt.model().count == 2, "and the half-changed model is put back");
        check(reported == 1, "the fault is reported through on_error");

        rt.send(Fragile::Msg{Fragile::Inc{}});
        check(rt.model().count == 3, "the app keeps processing messages");

        const std::size_t good_nodes = rt.tree().count();
        rt.send(Fragile::Msg{Fragile::Poison{}});
        check(!rt.finished(), "a throwing view does not end the app either");
        check(rt.tree().count() == good_nodes, "and the last good frame stays on screen");
    });

    AppConfig strict{.size = {200, 120}};
    strict.catch_exceptions = false;
    strict.on_error = [](std::string_view, std::string_view) {};
    run_headless<Fragile>(strict, [](auto& rt) {
        rt.send(Fragile::Msg{Fragile::Explode{}});
        check(rt.finished(), "with catch_exceptions off, a throw stops the program");
    });
}

void test_ime_composition() {
    section("input method");

    TextEditState f;
    f.set_preedit("こん");
    check(f.text.empty(), "preedit does not enter the document");
    check(f.display_text() == "こん", "but the user sees it");
    check(f.composing(), "and the field reports it is composing");

    f.set_preedit("こんにちは");
    check(f.text.empty(), "growing the preedit still commits nothing");
    check(f.display_text() == "こんにちは", "and replaces the previous preedit");

    f.commit_preedit();
    check(f.text == "こんにちは", "commit inserts the composed text");
    check(!f.composing(), "and composition ends");

    f.set_preedit("あ");
    check(f.display_text() == "こんにちはあ", "a new composition appends at the caret");
    f.cancel_preedit();
    check(f.text == "こんにちは", "cancelling leaves the document unchanged");
    check(f.display_text() == "こんにちは", "and the preedit disappears");

    f.set_preedit("か");
    check(!f.handle_key(Key::left, Mods{}), "arrows are not consumed while composing");
    check(!f.handle_key(Key::backspace, Mods{}), "nor is backspace");
    f.cancel_preedit();
    check(f.handle_key(Key::left, Mods{}), "but they work again once composition ends");

    TextEditState g;
    g.insert("hello");
    g.select_all();
    g.set_preedit("に");
    check(g.text.empty(), "starting a composition replaces the selection");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        rt.window().compose("こん");
        rt.tick();
        rt.window().commit("今日は");
        rt.tick();
        check(rt.model().text == "今日は", "a committed composition reaches on(TextEvent)");
    });
}

void test_history() {
    section("undo/redo");

    History<std::string> h{""};
    for (char c : std::string{"hello"}) h.edit([&](std::string& s) { s += c; }, "typing");
    check(*h == "hello", "edits apply");
    check(h.undo_depth() == 1, "and coalesce into one step (" + std::to_string(h.undo_depth()) + ")");

    h.undo();
    check(*h == "", "one undo reverts the whole run");
    h.redo();
    check(*h == "hello", "redo restores it");

    h.edit([](std::string& s) { s += " world"; }, "paste");
    check(h.undo_depth() == 2, "a new group is a new step");
    h.undo();
    check(*h == "hello", "and undoes independently");

    h.edit([](std::string& s) { s += "!"; }, "");
    check(!h.can_redo(), "editing after undo clears the redo branch");

    History<int> b{0};
    b.set_max_depth(5);
    for (int i = 1; i <= 20; ++i) b.set(i);
    check(b.undo_depth() == 5, "history is bounded");
    int steps = 0;
    while (b.undo()) ++steps;
    check(steps == 5 && *b == 15, "and drops the oldest states");
}

void test_cursor() {
    section("cursor");

    constexpr Theme th = themes::midnight;
    TextEditState field;
    auto ui = v(box() | size(100, 40) | bg(th.surface),
                node(text_field(th, field, false, node_id("f")).build()))
            | gap(4) | pad(8);
    Node n = ui.build();
    layout::layout_tree(n, {200, 200}, layout::default_measurer());

    const Node* f = n.find(node_id("f"));
    check(f != nullptr, "the field is in the tree");
    if (f != nullptr)
        check(f->style().cursor == CursorShape::text, "a text field asks for an I-beam, not an arrow");
    check(n.children()[0].style().cursor == CursorShape::arrow,
          "and a plain box inherits rather than overriding");
}

/// The busy-loop warning: real runaways only.
void test_busy_loop_detection() {
    section("busy loop");

    BusyLoopDetector d;
    for (int i = 0; i < 400; ++i) d.frame_presented(i / 60.0);
    check(d.busy(), "sustained rendering is detected");
    check(d.frame_rate() > 50.0 && d.frame_rate() < 70.0,
          "and the rate is reported (" + std::to_string(static_cast<int>(d.frame_rate())) + ")");
    d.idle();
    check(!d.busy(), "going idle clears the warning");

    BusyLoopDetector b;
    for (int i = 0; i < 30; ++i) b.frame_presented(i / 60.0);
    check(!b.busy(), "a short burst of animation is not flagged");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        rt.press_key(Key::space);
        rt.window().drive_frames(true);
        for (int i = 0; i < 200 && rt.model().animating; ++i) rt.tick();
        rt.window().drive_frames(false);
        check(!rt.host().frames_on(), "an animation that ends stops the frame clock");

        const auto settled = rt.window().frames_presented();
        for (int i = 0; i < 30; ++i) rt.tick();
        check(rt.window().frames_presented() == settled, "so the app presents nothing while idle");
        check(!rt.busy_loop_detected(), "and is never flagged");
    });

    run_headless<Spinner>(kCfg, [](auto& rt) {
        rt.window().drive_frames(true);
        for (int i = 0; i < 300; ++i) rt.tick();
        check(rt.busy_loop_detected(), "a view that animates forever is flagged");
    });

    // Moving the pointer over a live view earns frames; that is not a busy loop.
    run_headless<Follower>(kCfg, [](auto& rt) {
        rt.window().drive_frames(true);
        const auto before = rt.window().frames_presented();
        for (int i = 0; i < 300; ++i) {
            rt.window().move_to(Vec2{10.0f + static_cast<float>(i % 200), 50.0f});
            rt.tick();
        }
        check(rt.window().frames_presented() - before > 250, "pointer motion keeps it rendering");
        check(!rt.busy_loop_detected(), "and that is not reported as a busy loop");
    });
}

void test_render_economy() {
    section("render economy");

    run_headless<TestApp>(kCfg, [](auto& rt) {
        const auto after_boot = rt.window().frames_presented();
        check(after_boot >= 1, "boot paints one frame");

        for (int i = 0; i < 10; ++i) rt.tick();
        check(rt.window().frames_presented() == after_boot,
              "idle ticks do not repaint (" +
              std::to_string(rt.window().frames_presented() - after_boot) + " extra)");

        rt.click("inc");
        check(rt.window().frames_presented() > after_boot, "a click does repaint");
    });
}

/// An unhandled close still closes; a handled one is up to the program.
struct Closable {
    struct Model { bool asked = false; };
    struct Ask {};
    using Msg = std::variant<Ask>;
    using Cmd = mayag::Cmd<Msg>;
    static Cmd update(Model& m, Ask) { m.asked = true; return {}; }
    static Node view(const Model&) { return dsl::box().build(); }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Ask{}; }
};

void test_close() {
    section("close");

    run_headless<IntervalApp>(kCfg, [](auto& rt) {
        rt.window().push(CloseRequest{});
        rt.tick();
        check(rt.finished(), "a program with no on(CloseRequest) closes");
    });
    run_headless<Closable>(kCfg, [](auto& rt) {
        rt.window().push(CloseRequest{});
        rt.tick();
        check(rt.model().asked && !rt.finished(), "a program that handles it decides itself");
    });
}

}  // namespace

int main() {
    std::printf("mayag window host\n=================");

    test_update_purity();
    test_functor_laws();
    test_interaction();
    test_click_counts();
    test_drag_capture();
    test_effects();
    test_subscriptions();
    test_interval_timers();
    test_determinism();
    test_view_purity();
    test_focus();
    test_never_sleeps_dirty();
    test_motion_does_not_spin();
    test_accessibility();
    test_error_boundary();
    test_ime_composition();
    test_history();
    test_cursor();
    test_busy_loop_detection();
    test_render_economy();
    test_close();

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
