// examples/gallery.cpp — every visual feature mayag has, on one canvas
//
// This doubles as a manual regression sheet: if something in the shape kernel
// or the colour pipeline breaks, it is visible here immediately.

#include <mayag/mayag.hpp>

#include <cstdio>

using namespace mayag;
using namespace mayag::dsl;

int main() {
    constexpr Theme t = themes::midnight;

    // A labelled cell, so each demo below is self-documenting in the output.
    auto cell = [&](auto label, auto content) {
        return v(content | height(70),
                 label | font(10) | fg(t.text_secondary) | text_align(TextAlign::center))
             | gap(6) | align(Align::center) | width(130);
    };

    // ── row 1: corner radii and strokes ─────────────────────────────────
    auto row_shapes = h(
        cell(text<"radius 0">,   box() | size(90, 60) | bg(t.accent)),
        cell(text<"radius 14">,  box() | size(90, 60) | bg(t.accent) | radius(14)),
        cell(text<"pill">,       box() | size(90, 40) | bg(t.accent) | pill | margin(10, 0)),
        cell(text<"mixed radii">,box() | size(90, 60) | bg(t.accent) | radius(24, 4, 24, 4)),
        cell(text<"border">,     box() | size(90, 60) | border(3, t.accent) | radius(10)),
        cell(text<"outside">,    box() | size(90, 60) | bg(t.surface)
                                       | border(3, t.accent, StrokeAlign::outside) | radius(10))
    ) | gap(6);

    // ── row 2: gradients — the perceptual claim, made visible ───────────
    auto row_gradients = h(
        cell(text<"oklch ramp">,
             box() | size(110, 60) | radius(10)
                   | linear_gradient(rgb<0x0090FF>, rgb<0xF76B15>, {0, 0}, {1, 0})),
        cell(text<"srgb ramp">,
             box() | size(110, 60) | radius(10)
                   | linear_gradient(rgb<0x0090FF>, rgb<0xF76B15>, {0, 0}, {1, 0})
                   | srgb_interpolation),
        cell(text<"diagonal">,
             box() | size(110, 60) | radius(10)
                   | linear_gradient(t.accent, rotate_hue(t.accent, 90.0f), {0, 0}, {1, 1})),
        cell(text<"radial">,
             box() | size(110, 60) | radius(10)
                   | radial_gradient(rgb<0xFFFFFF>, t.accent, {0.5f, 0.5f}, 0.6f)),
        cell(text<"multi-stop">,
             box() | size(110, 60) | radius(10)
                   | gradient<GradientStop{0.0f,  rgb<0x6E56CF>},
                              GradientStop{0.45f, rgb<0xD6409F>},
                              GradientStop{1.0f,  rgb<0xFFB224>}>({0, 0}, {1, 0}))
    ) | gap(6);

    // ── row 3: shadows and elevation ────────────────────────────────────
    auto row_depth = h(
        cell(text<"elevation 2">,  box() | size(90, 50) | bg(t.surface_raised) | radius(10) | elevation(2)),
        cell(text<"elevation 8">,  box() | size(90, 50) | bg(t.surface_raised) | radius(10) | elevation(8)),
        cell(text<"elevation 24">, box() | size(90, 50) | bg(t.surface_raised) | radius(10) | elevation(24)),
        cell(text<"coloured">,     box() | size(90, 50) | bg(t.accent) | radius(10)
                                         | shadow(20.0f, t.accent.fade(0.6f), {0, 8})),
        cell(text<"inner">,        box() | size(90, 50) | bg(t.background) | radius(10)
                                         | inner_shadow(6.0f, colors::black.fade(0.7f), {0, 3})),
        cell(text<"glow">,         box() | size(90, 50) | bg(t.surface) | radius(10)
                                         | border(1, t.success)
                                         | shadow(16.0f, t.success.fade(0.5f), {0, 0}))
    ) | gap(6);

    // ── row 4: typography ───────────────────────────────────────────────
    auto row_type = h(
        v(text<"Display 28">   | font(28) | bold      | fg(t.text_primary),
          text<"Heading 18">   | font(18) | semibold  | fg(t.text_primary),
          text<"Body 13">      | font(13)             | fg(t.text_primary),
          text<"Caption 10">   | font(10)             | fg(t.text_secondary)) | gap(5),
        v(text<"regular">      | font(14) | fg(t.text_primary),
          text<"semibold">     | font(14) | semibold | fg(t.text_primary),
          text<"bold">         | font(14) | bold     | fg(t.text_primary),
          text<"underline">    | font(14) | underline | fg(t.text_primary),
          text<"strikethrough">| font(14) | strikethrough | fg(t.text_secondary)) | gap(5),
        v(text<"0123456789">   | font(14) | fg(t.accent),
          text<"!@#$%&*()[]{}">| font(14) | fg(t.warning),
          text<"+-=<>/\\|~^">  | font(14) | fg(t.success),
          text<"tracking wide">| font(12) | tracking(2.0f) | fg(t.text_secondary)) | gap(5)
    ) | gap(28);

    // ── row 5: components, all from the public DSL ──────────────────────
    auto row_widgets = h(
        button<"Primary">(t),
        button<"Secondary">(t, ButtonVariant::secondary),
        button<"Ghost">(t, ButtonVariant::ghost),
        button<"Delete">(t, ButtonVariant::danger),
        badge<"stable">(t, t.success),
        badge<"beta">(t, t.warning),
        kbd<"K">(t),
        toggle(t, true),
        toggle(t, false),
        avatar(t, rgb<0x8E4EC6>, 34.0f)
    ) | gap(10) | align(Align::center);

    // ── row 6: opacity and blending ─────────────────────────────────────
    auto swatch = [&](float a) {
        return box() | size(52, 52) | bg(t.accent) | radius(8) | opacity(a);
    };
    auto row_alpha = h(swatch(1.0f), swatch(0.8f), swatch(0.6f), swatch(0.4f),
                       swatch(0.2f), swatch(0.08f),
                       box() | size(52, 52) | radius(8) | bg(t.success) | blend(BlendMode::additive),
                       box() | size(52, 52) | radius(8) | bg(t.danger) | blend(BlendMode::screen))
                   | gap(8) | align(Align::center);

    // ── assemble ────────────────────────────────────────────────────────
    auto section = [&](auto title_, auto content) {
        return v(title_ | font(12) | semibold | fg(t.text_secondary) | tracking(1.0f),
                 content) | gap(10);
    };

    auto page = v(
        v(text<"mayag">          | font(34) | bold | fg(t.text_primary) | tracking(-1.0f),
          text<"GPU UI for C++26 — every shape below is one SDF instance">
                                 | font(12) | fg(t.text_secondary)) | gap(2),
        divider(t),
        section(text<"SHAPES">,     row_shapes),
        section(text<"GRADIENTS">,  row_gradients),
        section(text<"DEPTH">,      row_depth),
        section(text<"TYPE">,       row_type),
        section(text<"COMPONENTS">, row_widgets),
        section(text<"ALPHA + BLEND">, row_alpha)
    ) | gap(22) | pad(32) | bg(t.background);

    RenderOptions opts;
    opts.background = t.background;
    opts.font       = &fonts::Font::builtin_font();
    opts.dpi_scale  = 2.0f;

    const Vec2 viewport{860, 900};

    if (!render_to_png(page, viewport, "mayag_gallery.png", opts)) {
        std::fprintf(stderr, "mayag: failed to write PNG\n");
        return 1;
    }

    Node root = page.build();
    layout::layout_tree(root, viewport, opts.font->measurer());
    DrawList dl;
    render::PaintOptions po{};
    po.dpi_scale = opts.dpi_scale;
    po.glyphs    = &opts.font->glyph_renderer();
    render::paint(root, dl, po);

    std::printf("mayag_gallery.png  %dx%d\n",
                static_cast<int>(viewport.x * opts.dpi_scale),
                static_cast<int>(viewport.y * opts.dpi_scale));
    std::printf("  nodes      %zu\n", root.count());
    std::printf("  instances  %zu\n", dl.size());
    std::printf("  draw calls %zu\n", dl.batches().size());
    return 0;
}
