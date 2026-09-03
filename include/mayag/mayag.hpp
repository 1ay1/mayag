#pragma once
// ─────────────────────────────────────────────────────────────────────────
//  mayag — a GPU UI framework for C++26
//
//  #include <mayag/mayag.hpp> is the entire public surface.
//
//      using namespace mayag;
//      using namespace mayag::dsl;
//
//      constexpr auto card =
//          v(text<"Deploy"> | font(20) | bold | fg(colors::white),
//            text<"3 services"> | font(13) | fg(colors::slate))
//          | gap(6) | pad(20)
//          | bg(rgb<0x16181D>)
//          | border(1, rgb<0x2A2E37>)
//          | radius(14)
//          | elevation(8);
//
//      auto png = mayag::render_to_png(card, {320, 120});
//
//  Design commitments:
//    * Type-state DSL — invalid widget configurations do not compile, and the
//      error message is a sentence, not a template dump.
//    * Colour spaces are types. sRGB, Linear, Oklab and Oklch are distinct;
//      blending is only defined where it is physically correct.
//    * One shape kernel. Every primitive is a signed distance field, so a
//      frame is one draw call and antialiasing is analytic at any zoom.
//    * Backend-agnostic. Vulkan / Metal / D3D12 / WebGPU / OpenGL, plus a
//      pure-CPU reference rasteriser so mayag runs with no GPU at all.
// ─────────────────────────────────────────────────────────────────────────

// ── core ────────────────────────────────────────────────────────────────
#include <mayag/core/math.hpp>
#include <mayag/core/geometry.hpp>
#include <mayag/core/color.hpp>

// ── style & scene ───────────────────────────────────────────────────────
#include <mayag/style/style.hpp>
#include <mayag/style/theme.hpp>
#include <mayag/scene/node.hpp>

// ── the DSL ─────────────────────────────────────────────────────────────
#include <mayag/dsl/dsl.hpp>
#include <mayag/dsl/widgets.hpp>

// ── layout ──────────────────────────────────────────────────────────────
#include <mayag/layout/text_metrics.hpp>
#include <mayag/layout/flex.hpp>

// ── render ──────────────────────────────────────────────────────────────
#include <mayag/render/sdf.hpp>
#include <mayag/render/draw_list.hpp>
#include <mayag/render/painter.hpp>
#include <mayag/render/shader_source.hpp>

// ── backends ────────────────────────────────────────────────────────────
#include <mayag/backend/backend.hpp>
#include <mayag/backend/software.hpp>

// ── io ──────────────────────────────────────────────────────────────────
#include <mayag/image/png.hpp>
#include <mayag/text/font.hpp>

#include <string>
#include <vector>

namespace mayag {

/// Options for the one-call rendering helpers.
struct RenderOptions {
    Color<Srgb> background = rgb<0x0B0D10>;
    float       dpi_scale  = 1.0f;
    bool        debug_bounds = false;
    const fonts::Font* font = nullptr;   ///< null = metric-only layout, no glyphs
};

/// Lay out, paint, and rasterise a UI into an RGBA8 buffer.
/// Accepts either a DSL expression or an already-built `Node`.
template <typename Ui>
[[nodiscard]] std::vector<std::uint8_t> render_to_pixels(const Ui& ui, Vec2 viewport,
                                                         const RenderOptions& opts = {}) {
    Node root = [&] {
        if constexpr (std::same_as<std::remove_cvref_t<Ui>, Node>) return ui;
        else return ui.build();
    }();

    const layout::TextMeasurer& measurer =
        opts.font ? opts.font->measurer() : layout::default_measurer();

    layout::layout_tree(root, viewport, measurer);

    DrawList dl;
    render::PaintOptions po{};
    po.dpi_scale    = opts.dpi_scale;
    po.measurer     = &measurer;
    po.debug_bounds = opts.debug_bounds;
    po.glyphs       = opts.font ? &opts.font->glyph_renderer() : nullptr;
    render::paint(root, dl, po);

    const int w = static_cast<int>(viewport.x * opts.dpi_scale);
    const int h = static_cast<int>(viewport.y * opts.dpi_scale);
    backend::Framebuffer fb{w, h};
    fb.clear(opts.background);
    backend::Software::render(dl, fb, opts.font ? &opts.font->sampler() : nullptr);
    return fb.to_rgba8();
}

/// Render straight to a .png file. The fastest way to see a mayag UI.
template <typename Ui>
bool render_to_png(const Ui& ui, Vec2 viewport, const std::string& path,
                   const RenderOptions& opts = {}) {
    const auto pixels = render_to_pixels(ui, viewport, opts);
    return image::write_png(path, pixels,
                            static_cast<int>(viewport.x * opts.dpi_scale),
                            static_cast<int>(viewport.y * opts.dpi_scale));
}

}  // namespace mayag
