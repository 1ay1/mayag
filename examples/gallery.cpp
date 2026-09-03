// examples/gallery.cpp — every visual feature mayag has, as a live app
//
//   ./mayag_gallery                  open a window
//   ./mayag_gallery --png out.png    render one frame and exit
//   ./mayag_gallery --headless       scripted, for CI
//
// Interactive: click the theme chips to recolour the whole page, press SPACE
// to start the animated row, TAB to move focus, D to toggle layout debugging.
// Every one of those is a pure `update()` on the model.

#include "harness.hpp"

using namespace mayag;
using namespace mayag::dsl;

struct Gallery {
    // ── state ───────────────────────────────────────────────────────────

    struct Model {
        int    theme_index = 0;
        // Same reasoning as the dashboard: idle by default, SPACE to animate.
        // With a GPU backend bound this would default to true.
        bool   animating   = false;
        double t           = 0.0;
        int    clicks      = 0;
        bool   toggle_a    = true;
        bool   toggle_b    = false;
        float  slider      = 0.62f;
    };

    struct PickTheme { int index; };
    struct ToggleAnimation {};
    struct Tick { double dt; };
    struct Bump {};
    struct FlipA {};
    struct FlipB {};
    struct DragSlider { float x; };
    struct Quit {};

    using Msg = std::variant<PickTheme, ToggleAnimation, Tick, Bump,
                             FlipA, FlipB, DragSlider, Quit>;

    static constexpr std::array<Theme, 6> palette{
        themes::midnight, themes::ember, themes::forest,
        themes::orchid,   themes::daylight, themes::paper,
    };
    static constexpr std::array<const char*, 6> palette_names{
        "midnight", "ember", "forest", "orchid", "daylight", "paper",
    };

    static std::pair<Model, Cmd<Msg>> init() {
        return {Model{}, Cmd<Msg>::set_title("mayag — gallery")};
    }

    // ── update ──────────────────────────────────────────────────────────

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        return std::visit([&](auto&& e) -> std::pair<Model, Cmd<Msg>> {
            using T = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<T, PickTheme>) {
                m.theme_index = num::clamp(e.index, 0, static_cast<int>(palette.size()) - 1);
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, ToggleAnimation>) {
                m.animating = !m.animating;
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Tick>) {
                m.t += e.dt;
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Bump>)  { m.clicks++;            return {m, Cmd<Msg>::none()}; }
            else if constexpr (std::is_same_v<T, FlipA>) { m.toggle_a = !m.toggle_a; return {m, Cmd<Msg>::none()}; }
            else if constexpr (std::is_same_v<T, FlipB>) { m.toggle_b = !m.toggle_b; return {m, Cmd<Msg>::none()}; }
            else if constexpr (std::is_same_v<T, DragSlider>) {
                m.slider = num::saturate(e.x / 260.0f);
                return {m, Cmd<Msg>::none()};
            }
            else { return {m, Cmd<Msg>::quit()}; }
        }, msg);
    }

    // ── view ────────────────────────────────────────────────────────────

    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = palette[static_cast<std::size_t>(m.theme_index)];

        // A uniform cell: fixed column width, fixed swatch row so labels line
        // up, content centred without being resized.
        //
        // The content keeps its OWN size — applying `height(72)` to it (as
        // this used to) silently overrode each swatch's `size(92, 60)`, which
        // the DSL now refuses to compile. The label is `ellipsis`, not the
        // default wrap: a one-word caption in a narrow column must never break
        // across lines.
        auto cell = [&](auto label, auto content) {
            return v(v(content) | height(72) | width(pct(100)) | center,
                     label | font(10) | fg(t.text_secondary)
                           | text_align(TextAlign::center) | ellipsis)
                 | gap(6) | align(Align::center) | width(136);
        };

        // ── shapes ──────────────────────────────────────────────────────
        auto shapes = h(
            cell(text<"radius 0">,    box() | size(92, 60) | bg(t.accent)),
            cell(text<"radius 14">,   box() | size(92, 60) | bg(t.accent) | radius(14)),
            cell(text<"pill">,        box() | size(92, 40) | bg(t.accent) | pill | margin(10, 0)),
            cell(text<"mixed">,       box() | size(92, 60) | bg(t.accent) | radius(24, 4, 24, 4)),
            cell(text<"border">,      box() | size(92, 60) | border(3, t.accent) | radius(10)),
            cell(text<"outside">,     box() | size(92, 60) | bg(t.surface)
                                            | border(3, t.accent, StrokeAlign::outside) | radius(10))
        ) | gap(6);

        // ── gradients ───────────────────────────────────────────────────
        // The oklch/srgb pair is the whole perceptual-colour argument, side
        // by side: same endpoints, one keeps chroma through the middle.
        auto gradients = h(
            cell(text<"oklch">,
                 box() | size(112, 60) | radius(10)
                       | linear_gradient(rgb<0x0090FF>, rgb<0xF76B15>, {0, 0}, {1, 0})),
            cell(text<"srgb (muddy)">,
                 box() | size(112, 60) | radius(10)
                       | linear_gradient(rgb<0x0090FF>, rgb<0xF76B15>, {0, 0}, {1, 0})
                       | srgb_interpolation),
            cell(text<"diagonal">,
                 box() | size(112, 60) | radius(10)
                       | linear_gradient(t.accent, rotate_hue(t.accent, 90.0f), {0, 0}, {1, 1})),
            cell(text<"radial">,
                 box() | size(112, 60) | radius(10)
                       | radial_gradient(rgb<0xFFFFFF>, t.accent, {0.5f, 0.5f}, 0.6f)),
            cell(text<"multi-stop">,
                 box() | size(112, 60) | radius(10)
                       | gradient<GradientStop{0.0f,  rgb<0x6E56CF>},
                                  GradientStop{0.45f, rgb<0xD6409F>},
                                  GradientStop{1.0f,  rgb<0xFFB224>}>({0, 0}, {1, 0}))
        ) | gap(6);

        // ── depth ───────────────────────────────────────────────────────
        auto depth = h(
            cell(text<"elevation 2">,  box() | size(92, 50) | bg(t.surface_raised) | radius(10) | elevation(2)),
            cell(text<"elevation 8">,  box() | size(92, 50) | bg(t.surface_raised) | radius(10) | elevation(8)),
            cell(text<"elevation 24">, box() | size(92, 50) | bg(t.surface_raised) | radius(10) | elevation(24)),
            cell(text<"coloured">,     box() | size(92, 50) | bg(t.accent) | radius(10)
                                             | shadow(20.0f, t.accent.fade(0.6f), {0, 8})),
            cell(text<"inner">,        box() | size(92, 50) | bg(t.background) | radius(10)
                                             | inner_shadow(6.0f, colors::black.fade(0.7f), {0, 3})),
            cell(text<"glow">,         box() | size(92, 50) | bg(t.surface) | radius(10)
                                             | border(1, t.success)
                                             | shadow(16.0f, t.success.fade(0.5f), {0, 0}))
        ) | gap(6);

        // ── animation: a live SDF arc, driven by the model clock ────────
        const float phase = static_cast<float>(m.t);
        const float sweep = 0.35f + 0.3f * (num::sin(phase * 1.7f) * 0.5f + 0.5f);

        auto animated = h(
            cell(text<"pulse">,
                 box() | size(56, 56) | bg(t.accent) | radius(28)
                       | opacity(0.45f + 0.55f * (num::sin(phase * 2.2f) * 0.5f + 0.5f))
                       | margin(8, 0)),
            cell(text<"orbit">,
                 z(box() | size(14, 14) | bg(t.success) | radius(7)
                         | absolute(28.0f + 22.0f * num::cos(phase * 1.4f),
                                    22.0f + 22.0f * num::sin(phase * 1.4f)))
                   | size(72, 72) | bg(t.surface) | radius(36) | border(1, t.border)),
            cell(text<"sweep">,
                 box() | size(72, 72) | bg(t.surface) | radius(36)
                       | border(6, t.warning.fade(sweep), StrokeAlign::inside)),
            cell(text<"breathe">,
                 box() | size(64, 64) | bg(t.danger) | radius(16)
                       | scale(0.75f + 0.25f * (num::cos(phase * 1.9f) * 0.5f + 0.5f))),
            cell(text_of(m.animating ? "running" : "paused"),
                 box() | size(104, 44)
                       | bg(m.animating ? t.success.fade(0.18f) : t.surface_raised)
                       | border(1, m.animating ? t.success : t.border)
                       | radius(8) | margin(14, 0) | dsl::id<"anim">)
        ) | gap(6);

        // ── interactive controls ────────────────────────────────────────
        auto hot = [&](std::uint64_t nid, Color<Srgb> base) {
            return c.pressed(nid) ? darken(base, 0.08f)
                 : c.hovered(nid) ? lighten(base, 0.06f) : base;
        };

        auto controls = h(
            h(text_owned("clicked " + std::to_string(m.clicks)) | font(13) | semibold
                | fg(t.on_accent))
              | center | pad(11, 20) | bg(hot(node_id("bump"), t.accent))
              | radius(t.radius_small) | elevation(3) | dsl::id<"bump">,

            toggle(t, m.toggle_a) | dsl::id<"toggle-a">,
            toggle(t, m.toggle_b) | dsl::id<"toggle-b">,

            // Drag the track: `on_drag` reports node-local coordinates.
            z(box() | size(260.0f * m.slider, 10) | bg(t.accent) | radius(5) | absolute(0, 0))
              | size(260, 10) | bg(t.border) | radius(5) | dsl::id<"slider">,

            text_owned(std::to_string(static_cast<int>(m.slider * 100)) + "%")
              | font(12) | fg(t.text_secondary)
        ) | gap(16) | align(Align::center);

        // ── theme chips ─────────────────────────────────────────────────
        std::vector<Node> chips;
        for (std::size_t i = 0; i < palette.size(); ++i) {
            const bool active = (static_cast<int>(i) == m.theme_index);
            const auto nid = node_id(std::string{"theme-"} + std::to_string(i));
            chips.push_back(
                (h(box() | size(12, 12) | bg(palette[i].accent) | radius(6),
                   text_of(palette_names[i]) | font(11)
                     | fg(active ? t.text_primary : t.text_secondary))
                 | gap(7) | align(Align::center)
                 | pad(7, 12)
                 | bg(active ? t.surface_raised
                     : c.hovered(nid) ? t.surface : t.surface.fade(0.0f))
                 | border(1, active ? t.accent : t.border)
                 | radius(t.radius_small)
                 | id_of(std::string{"theme-"} + std::to_string(i))).build());
        }

        // A runtime-sized list lifted into the compile-time DSL.
        auto chip_row = list(Axis::horizontal, std::move(chips), 8.0f);

        auto section = [&](auto title_, auto content) {
            return v(title_ | font(10) | semibold | fg(t.text_secondary) | tracking(2.0f),
                     content) | gap(10);
        };

        return v(split(v(text<"mayag"> | font(34) | bold | fg(t.text_primary) | tracking(-1.2f),
                         text<"GPU UI for C++26 — every shape is one SDF instance">
                           | font(12) | fg(t.text_secondary)) | gap(2),
                       chip_row),
                 divider(t),
                 section(text<"SHAPES">,     shapes),
                 section(text<"GRADIENTS">,  gradients),
                 section(text<"DEPTH">,      depth),
                 section(text<"ANIMATION">,  animated),
                 section(text<"INTERACTION">, controls),
                 h(text<"space"> | font(10) | fg(t.text_disabled),
                   text<"animate"> | font(10) | fg(t.text_disabled),
                   text<"tab"> | font(10) | fg(t.text_disabled),
                   text<"focus"> | font(10) | fg(t.text_disabled),
                   text<"esc"> | font(10) | fg(t.text_disabled),
                   text<"quit"> | font(10) | fg(t.text_disabled)) | gap(8))
             | gap(20) | pad(32)
             | width(pct(100)) | height(pct(100))
             | bg(t.background);
    }

    // ── subscribe ───────────────────────────────────────────────────────

    static Sub<Msg> subscribe(const Model& m) {
        std::vector<Sub<Msg>> subs{
            Sub<Msg>::on_click<"bump">(Bump{}),
            Sub<Msg>::on_click<"toggle-a">(FlipA{}),
            Sub<Msg>::on_click<"toggle-b">(FlipB{}),
            Sub<Msg>::on_click<"anim">(ToggleAnimation{}),
            Sub<Msg>::on_drag<"slider">([](Vec2 local) { return Msg{DragSlider{local.x}}; }),
            Sub<Msg>::on_key(Key::space, ToggleAnimation{}),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_close(Quit{}),
            // The animation subscription only EXISTS while animating, so a
            // paused gallery drops to 0% CPU with no explicit stop call.
            when(m.animating,
                 Sub<Msg>::every_frame([](FrameEvent f) { return Msg{Tick{f.delta}}; })),
        };
        for (int i = 0; i < static_cast<int>(palette.size()); ++i) {
            subs.push_back(Sub<Msg>::on_click_id(
                node_id(std::string{"theme-"} + std::to_string(i)), PickTheme{i}));
        }
        return Sub<Msg>::batch(std::move(subs));
    }
};

static_assert(Program<Gallery>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{1020, 880});
    auto fonts = demo::make_fonts(opts);
    demo::print_fonts(fonts.get());

    AppConfig cfg{
        .title = "mayag — gallery",
        .size  = opts.size,
        .theme = Gallery::palette[0],
        .fonts = fonts.get(),
        .debug_bounds = opts.debug_bounds,
    };

    return demo::run<Gallery>(opts, cfg, [](auto& rt) {
        std::printf("gallery headless\n");
        int fails = 0;
        const auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            if (!c) ++fails;
        };

        ok(rt.node_rect("bump").has_value(), "widgets laid out");

        rt.click("bump");
        ok(rt.model().clicks == 1, "clicking the button counts");

        rt.click("toggle-a");
        ok(!rt.model().toggle_a, "toggle flips");

        rt.click("theme-2");
        ok(rt.model().theme_index == 2, "theme chip switches the palette");

        // Drag the slider from its left edge to well past its right.
        if (auto s = rt.node_rect("slider")) {
            const Vec2 start{s->left() + 5.0f, s->center().y};
            rt.window().push(MouseMove{start, {}, {}});
            rt.window().push(MouseDown{start, MouseButton::left, {}, 1});
            rt.window().push(MouseMove{{s->right() + 200.0f, s->center().y}, {}, {}});
            rt.tick();
            ok(rt.model().slider > 0.95f, "dragging past the end saturates the slider");
            rt.window().push(MouseUp{{s->right() + 200.0f, s->center().y}, MouseButton::left, {}});
            rt.tick();
        }

        // Animation is OFF by default (the software rasteriser is the CPU),
        // so space starts it and a second press stops it again.
        ok(!rt.model().animating, "animation is idle by default");
        rt.window().press_key(Key::space);
        rt.tick();
        ok(rt.model().animating, "space starts the animation");
        ok(Gallery::subscribe(rt.model()).wants_frames(),
           "and the frame subscription now exists");
        rt.window().press_key(Key::space);
        rt.tick();
        ok(!rt.model().animating, "space stops it again");
        ok(!Gallery::subscribe(rt.model()).wants_frames(),
           "and the subscription disappears, so the app idles");

        ok(rt.window().frames_presented() > 1, "frames were presented");
        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
