// examples/dashboard.cpp — a real mayag UI, rendered to a PNG
//
// Build:  cmake --build build && ./build/examples/mayag_dashboard
//
// Everything below is the public DSL. Note that the entire style tree is
// `constexpr`-friendly: the compiler folds it, and the only runtime work is
// layout, painting, and rasterisation.

#include "harness.hpp"

using namespace mayag;
using namespace mayag::dsl;

struct Dashboard {
    struct Model {
        int  theme_index = 0;
        int  selected    = 0;      ///< which nav row is active
        // Off by default. The software rasteriser costs ~13 ms a frame at
        // this size, so a 60 Hz animation genuinely eats a core; an example
        // should idle at 0% until you ask it to move. Press SPACE.
        bool live        = false;
        double t         = 0.0;
    };

    struct Select { int index; };
    struct NextTheme {};
    struct ToggleLive {};
    struct Tick { double dt; };
    struct Quit {};
    using Msg = std::variant<Select, NextTheme, ToggleLive, Tick, Quit>;

    static constexpr std::array<Theme, 4> palette{
        themes::midnight, themes::forest, themes::orchid, themes::daylight};
    static constexpr std::array<const char*, 4> nav{
        "Overview", "Pipelines", "Registry", "Settings"};

    static std::pair<Model, Cmd<Msg>> init() {
        return {Model{}, Cmd<Msg>::set_title("mayag — dashboard")};
    }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        return std::visit([&](auto&& e) -> std::pair<Model, Cmd<Msg>> {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, Select>)     { m.selected = e.index; return {m, Cmd<Msg>::none()}; }
            else if constexpr (std::is_same_v<T, NextTheme>) {
                m.theme_index = (m.theme_index + 1) % static_cast<int>(palette.size());
                return {m, Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, ToggleLive>) { m.live = !m.live; return {m, Cmd<Msg>::none()}; }
            else if constexpr (std::is_same_v<T, Tick>)  { m.t += e.dt; return {m, Cmd<Msg>::none()}; }
            else { return {m, Cmd<Msg>::quit()}; }
        }, msg);
    }

    static Sub<Msg> subscribe(const Model& m) {
        std::vector<Sub<Msg>> subs{
            Sub<Msg>::on_key(Key::space, ToggleLive{}),
            Sub<Msg>::on_key(Key::t, NextTheme{}),
            Sub<Msg>::on_click<"theme">(NextTheme{}),
            Sub<Msg>::on_click<"live">(ToggleLive{}),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_close(Quit{}),
            when(m.live, Sub<Msg>::every_frame([](FrameEvent f) { return Msg{Tick{f.delta}}; })),
        };
        for (int i = 0; i < static_cast<int>(nav.size()); ++i) {
            subs.push_back(Sub<Msg>::on_click_id(
                node_id(std::string{"nav-"} + std::to_string(i)), Select{i}));
        }
        return Sub<Msg>::batch(std::move(subs));
    }

    static Node view(const Model& m, const Ctx& c) {

        const Theme& t = palette[static_cast<std::size_t>(m.theme_index)];

        // ── the sidebar ─────────────────────────────────────────────────────
        // One row per entry, keyed so `subscribe` can bind clicks to it.
        // Hover styling reads from Ctx, so it costs nothing in the Model.
        auto nav_item = [&](int index) {
            const auto nid = node_id(std::string{"nav-"} + std::to_string(index));
            const bool active = (index == m.selected);
            const bool hot    = c.hovered(nid);
            return (h(dot(active ? t.accent : t.text_disabled, 6.0f),
                      text_of(nav[static_cast<std::size_t>(index)]) | font(13)
                        | fg(active ? t.text_primary : t.text_secondary))
                    | gap(10) | align(Align::center)
                    | pad(8, 12)
                    | width(pct(100))
                    | bg(active ? t.surface_raised
                        : hot   ? t.surface_raised.fade(0.5f)
                                : t.surface.fade(0.0f))
                    | radius(t.radius_small)
                    | id_of(std::string{"nav-"} + std::to_string(index))).build();
        };

        auto sidebar =
            v(h(box() | size(26, 26) | radius(8)
                      | linear_gradient(t.accent, rotate_hue(t.accent, 40.0f), {0, 0}, {1, 1}),
                text<"mayag"> | font(16) | bold | fg(t.text_primary))
              | gap(10) | align(Align::center) | margin(0, 0),

              box() | height(14),

              node(nav_item(0)), node(nav_item(1)),
              node(nav_item(2)), node(nav_item(3)),

              spacer(),

              h(avatar(t, rgb<0x8E4EC6>, 28.0f),
                v(text<"ayush">  | font(12) | semibold | fg(t.text_primary),
                  text<"pro">    | font(10) | fg(t.text_secondary)) | gap(1))
              | gap(9) | align(Align::center))
            | gap(3)
            | pad(16)
            | width(196)
            | height(pct(100))
            | bg(t.surface)
            | border(1, t.border, StrokeAlign::inside);

        // ── stat tiles ──────────────────────────────────────────────────────
        auto stat = [&](auto label, auto value, Color<Srgb> tone) {
            return v(label | font(11) | fg(t.text_secondary) | tracking(0.4f),
                     value | font(26) | bold | fg(t.text_primary),
                     box() | height(3) | width(pct(100)) | bg(tone) | radius(2))
                 | gap(6)
                 | pad(14)
                 | grow()
                 | bg(t.surface)
                 | border(1, t.border)
                 | radius(t.radius_medium)
                 | elevation(4.0f);
        };

        auto stats = h(stat(text<"REQUESTS">, text<"48.2k">, t.accent),
                       stat(text<"P99">,      text<"84ms">,  t.success),
                       stat(text<"ERRORS">,   text<"0.03%">, t.warning))
                   | gap(12);

        // ── the main column ─────────────────────────────────────────────────
        auto header =
            split(v(text<"Overview">           | font(22) | bold | fg(t.text_primary),
                    text<"production cluster"> | font(12) | fg(t.text_secondary)) | gap(2),
                  h(// The live badge is a real toggle, and its colour is a
                    // pure function of the model rather than stored state.
                    (h(dot(m.live ? t.success : t.text_disabled, 6.0f),
                       text_of(m.live ? "live" : "paused") | font(11) | semibold
                         | fg(m.live ? t.success : t.text_secondary))
                     | gap(6) | center | pad(4, 10)
                     | bg((m.live ? t.success : t.text_disabled).fade(0.16f))
                     | border(1, (m.live ? t.success : t.text_disabled).fade(0.35f))
                     | pill | dsl::id<"live">),
                    kbd<"T">(t),
                    (h(text<"theme"> | font(12) | semibold | fg(t.on_accent))
                     | center | pad(9, 16)
                     | bg(c.pressed(node_id("theme")) ? darken(t.accent, 0.08f)
                        : c.hovered(node_id("theme")) ? lighten(t.accent, 0.06f)
                                                      : t.accent)
                     | radius(t.radius_small) | elevation(3)
                     | dsl::id<"theme">))
                  | gap(8) | align(Align::center));

        auto deploy_row = [&](auto name, auto when, Color<Srgb> tone, auto status) {
            return split(h(dot(tone, 7.0f),
                           v(name | font(13) | fg(t.text_primary),
                             when | font(10) | fg(t.text_secondary)) | gap(1))
                         | gap(10) | align(Align::center),
                         status | font(11) | semibold | fg(tone))
                 | pad(10, 12)
                 | bg(t.background.fade(0.5f))
                 | radius(t.radius_small);
        };

        auto activity =
            card(t,
                 text<"Recent deploys"> | font(14) | semibold | fg(t.text_primary),
                 deploy_row(text<"api-gateway">,   text<"2m ago">,  t.success, text<"passed">),
                 deploy_row(text<"auth-service">,  text<"14m ago">, t.success, text<"passed">),
                 deploy_row(text<"web-frontend">,  text<"31m ago">, t.warning, text<"flaky">),
                 deploy_row(text<"batch-worker">,  text<"1h ago">,  t.danger,  text<"failed">))
            | gap(8);

        auto usage =
            card(t,
                 split(text<"Build minutes"> | font(14) | semibold | fg(t.text_primary),
                       text<"72%"> | font(12) | fg(t.text_secondary)),
                 progress(t, 0.72f, 8.0f),
                 h(caption<"4,320 of 6,000 used">(t), spacer(),
                   toggle(t, true)) | align(Align::center))
            | gap(12);

        auto main_column =
            v(header, stats, activity, usage)
            | gap(16)
            | pad(24)
            | grow()
            | height(pct(100));

        auto app = h(sidebar, main_column) | width(pct(100)) | height(pct(100));


        return (app | width(pct(100)) | height(pct(100))).build();
    }
};

static_assert(Program<Dashboard>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{980, 660});
    auto fonts = demo::make_fonts(opts);
    demo::print_fonts(fonts.get());

    AppConfig cfg{
        .title = "mayag — dashboard",
        .size  = opts.size,
        .theme = Dashboard::palette[0],
        .fonts = fonts.get(),
        .debug_bounds = opts.debug_bounds,
    };

    return demo::run<Dashboard>(opts, cfg, [](auto& rt) {
        std::printf("dashboard headless\n");
        int fails = 0;
        const auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            if (!c) ++fails;
        };
        ok(rt.node_rect("nav-1").has_value(), "nav rows laid out");
        rt.click("nav-1");
        ok(rt.model().selected == 1, "clicking a nav row selects it");
        rt.click("theme");
        ok(rt.model().theme_index == 1, "theme button cycles");
        ok(!rt.model().live, "the live feed is idle by default");
        rt.window().press_key(Key::space);
        rt.tick();
        ok(rt.model().live, "space starts the live feed");
        rt.window().press_key(Key::space);
        rt.tick();
        ok(!rt.model().live, "and stops it again");
        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
