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
#include <mayag/backend/tiled.hpp>

// ── app runtime ─────────────────────────────────────────────────────────
#include <mayag/app/event.hpp>
#include <mayag/app/cmd.hpp>
#include <mayag/app/sub.hpp>
#include <mayag/app/interaction.hpp>
#include <mayag/app/app.hpp>

// ── io ─────────────────────────────────────────────────────────────
#include <mayag/image/png.hpp>

// ── text ───────────────────────────────────────────────────────────
//
// Two engines, one interface. `strokefont::Font` is the built-in stroke font —
// no files, no dependencies, always available. `typo::FontStack` is the real
// engine: TrueType/OpenType parsing, kerning, fallback chains, SDF atlas.
// Both satisfy the same TextMeasurer/GlyphRenderer seam, so a program picks
// one in RenderOptions and nothing else changes.
#include <mayag/text/font.hpp>
#include <mayag/font/font.hpp>
#include <mayag/font/system.hpp>

#include <string>
#include <vector>

namespace mayag {

/// Options for the one-call rendering helpers.
struct RenderOptions {
    Color<Srgb> background = rgb<0x0B0D10>;
    float       dpi_scale  = 1.0f;
    bool        debug_bounds = false;

    /// The built-in stroke font — zero dependencies, always works.
    const strokefont::Font* font = nullptr;

    /// The real font engine. When set, this WINS over `font`: it has actual
    /// typefaces, kerning, and script fallback.
    typo::FontStack* fonts = nullptr;
};

namespace detail {
// `StackBindings` lives in app/app.hpp — the runtime needs it too, and one
// definition serves both the one-call helpers and the event loop.
}  // namespace detail

/// Lay out, paint, and rasterise a UI into an RGBA8 buffer.
/// Accepts either a DSL expression or an already-built `Node`.
template <typename Ui>
[[nodiscard]] std::vector<std::uint8_t> render_to_pixels(const Ui& ui, Vec2 viewport,
                                                         const RenderOptions& opts = {}) {
    Node root = [&] {
        if constexpr (std::same_as<std::remove_cvref_t<Ui>, Node>) return ui;
        else return ui.build();
    }();

    // The real engine takes precedence when both are supplied.
    std::optional<detail::StackBindings> bound;
    if (opts.fonts != nullptr) bound.emplace(*opts.fonts);

    const layout::TextMeasurer& measurer =
        bound            ? static_cast<const layout::TextMeasurer&>(bound->measurer)
      : opts.font        ? opts.font->measurer()
                         : layout::default_measurer();

    layout::layout_tree(root, viewport, measurer);

    DrawList dl;
    render::PaintOptions po{};
    po.dpi_scale    = opts.dpi_scale;
    po.measurer     = &measurer;
    po.debug_bounds = opts.debug_bounds;
    po.glyphs       = bound ? static_cast<const render::GlyphRenderer*>(&bound->glyphs)
                    : opts.font ? &opts.font->glyph_renderer()
                                : nullptr;
    render::paint(root, dl, po);

    const int w = static_cast<int>(viewport.x * opts.dpi_scale);
    const int h = static_cast<int>(viewport.y * opts.dpi_scale);
    backend::Framebuffer fb{w, h};

    const backend::CoverageSampler* sampler =
        bound     ? static_cast<const backend::CoverageSampler*>(&bound->sampler)
      : opts.font ? &opts.font->sampler()
                  : nullptr;

    // Tiled + parallel. Bit-identical to the reference path (asserted by
    // tests), so there is no reason to take the slow one.
    backend::Tiled::render(dl, fb, sampler, &backend::shared_pool(), opts.background);

    std::vector<std::uint8_t> out;
    backend::Tiled::encode_parallel(fb, out, &backend::shared_pool());
    return out;
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
