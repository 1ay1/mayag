// examples/dashboard.cpp — a real mayag UI, rendered to a PNG
//
// Build:  cmake --build build && ./build/examples/mayag_dashboard
//
// Everything below is the public DSL. Note that the entire style tree is
// `constexpr`-friendly: the compiler folds it, and the only runtime work is
// layout, painting, and rasterisation.

#include <mayag/mayag.hpp>

#include <cstdio>

using namespace mayag;
using namespace mayag::dsl;

int main() {
    constexpr Theme t = themes::midnight;

    // ── the sidebar ─────────────────────────────────────────────────────
    auto nav_item = [&](auto label, bool active) {
        return h(dot(active ? t.accent : t.text_disabled, 6.0f),
                 label | font(13) | fg(active ? t.text_primary : t.text_secondary))
             | gap(10) | align(Align::center)
             | pad(8, 12)
             | width(pct(100))
             | bg(active ? t.surface_raised : t.surface.fade(0.0f))
             | radius(t.radius_small);
    };

    auto sidebar =
        v(h(box() | size(26, 26) | radius(8)
                  | linear_gradient(t.accent, rotate_hue(t.accent, 40.0f), {0, 0}, {1, 1}),
            text<"mayag"> | font(16) | bold | fg(t.text_primary))
          | gap(10) | align(Align::center) | margin(0, 0),

          box() | height(14),

          nav_item(text<"Overview">,  true),
          nav_item(text<"Pipelines">, false),
          nav_item(text<"Registry">,  false),
          nav_item(text<"Settings">,  false),

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
              h(badge<"live">(t, t.success),
                kbd<"R">(t),
                button<"Deploy">(t)) | gap(8) | align(Align::center));

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
             progress(t, 0.72f, 999.0f, 8.0f) | width(pct(100)),
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

    // ── render ──────────────────────────────────────────────────────────
    RenderOptions opts;
    opts.background = t.background;
    opts.font       = &fonts::Font::builtin_font();
    opts.dpi_scale  = 2.0f;   // retina

    const Vec2 viewport{900, 620};

    if (!render_to_png(app, viewport, "mayag_dashboard.png", opts)) {
        std::fprintf(stderr, "mayag: failed to write PNG\n");
        return 1;
    }

    // Report what the frame actually cost on the GPU side.
    Node root = app.build();
    layout::layout_tree(root, viewport, opts.font->measurer());
    DrawList dl;
    render::PaintOptions po{};
    po.dpi_scale = opts.dpi_scale;
    po.glyphs    = &opts.font->glyph_renderer();
    render::paint(root, dl, po);

    std::printf("mayag_dashboard.png  %dx%d\n",
                static_cast<int>(viewport.x * opts.dpi_scale),
                static_cast<int>(viewport.y * opts.dpi_scale));
    std::printf("  nodes      %zu\n", root.count());
    std::printf("  instances  %zu\n", dl.size());
    std::printf("  draw calls %zu\n", dl.batches().size());
    return 0;
}
