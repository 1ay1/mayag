#pragma once
// mayag::render::paint — scene tree to draw list
//
// The traversal that turns a laid-out `Node` tree into GPU instances. Painting
// order per node is fixed and matches every design tool:
//
//     shadows (outer) -> fill -> children -> inner shadows -> border -> text
//
// Borders paint AFTER children so a clipped child never covers its parent's
// border; inner shadows paint after children for the same reason.

#include "../layout/text_metrics.hpp"
#include "../scene/node.hpp"
#include "draw_list.hpp"

#include <string_view>

namespace mayag::render {

/// How glyphs get to the screen. The painter never knows about fonts; it asks
/// this for quads. A null renderer draws nothing, which is exactly right for
/// layout-only tests.
class GlyphRenderer {
  public:
    virtual ~GlyphRenderer() = default;
    virtual void draw_text(DrawList& dl, std::string_view s, const Rect& box,
                           const TextStyle& st) const = 0;
};

struct PaintOptions {
    float                dpi_scale = 1.0f;
    const GlyphRenderer* glyphs    = nullptr;
    const layout::TextMeasurer* measurer = nullptr;
    bool                 debug_bounds = false;   ///< overlay every node's rect
};

namespace detail {

/// Multiply an alpha through a subtree without a second pass: opacity
/// accumulates down the traversal.
inline Color<Srgb> apply_opacity(Color<Srgb> c, float a) noexcept {
    return c.fade(a);
}

inline void paint_node(const Node& node, DrawList& dl, const PaintOptions& opts,
                       float inherited_opacity) {
    const Style& st = node.style();
    const float alpha = inherited_opacity * st.opacity;
    if (alpha <= 0.001f) return;

    const Rect    frame = node.frame().scale(opts.dpi_scale);
    const Corners radii = st.corners.clamp_to(frame.size);

    // ── outer shadows ───────────────────────────────────────────────────
    for (std::uint8_t i = 0; i < st.shadow_count; ++i) {
        const Shadow& sh = st.shadows[i];
        if (!sh.inset && sh.visible()) {
            Shadow scaled = sh;
            scaled.offset = sh.offset * opts.dpi_scale;
            scaled.blur   = sh.blur * opts.dpi_scale;
            scaled.spread = sh.spread * opts.dpi_scale;
            scaled.color  = apply_opacity(sh.color, alpha);
            dl.shadow(frame, scaled, radii);
        }
    }

    // ── fill ────────────────────────────────────────────────────────────
    if (st.fill.visible()) {
        if (st.fill.kind == FillKind::solid) {
            dl.fill_rect(frame, apply_opacity(st.fill.color, alpha), radii);
        } else {
            Fill f = st.fill;
            for (std::uint8_t i = 0; i < f.stop_count; ++i)
                f.stops[i].color = apply_opacity(f.stops[i].color, alpha);
            dl.fill_gradient(frame, f, radii);
        }
    }

    // ── children ────────────────────────────────────────────────────────
    const bool clipping = st.clip && !node.children().empty();
    if (clipping) dl.push_clip(frame);

    for (const auto& kid : node.children()) {
        paint_node(kid, dl, opts, alpha);
    }

    if (clipping) dl.pop_clip();

    // ── inner shadows ───────────────────────────────────────────────────
    for (std::uint8_t i = 0; i < st.shadow_count; ++i) {
        const Shadow& sh = st.shadows[i];
        if (sh.inset && sh.visible()) {
            Shadow scaled = sh;
            scaled.offset = sh.offset * opts.dpi_scale;
            scaled.blur   = sh.blur * opts.dpi_scale;
            scaled.color  = apply_opacity(sh.color, alpha);
            dl.shadow(frame, scaled, radii);
        }
    }

    // ── border ──────────────────────────────────────────────────────────
    if (st.stroke.visible()) {
        dl.stroke_rect(frame, st.stroke.width * opts.dpi_scale,
                       apply_opacity(st.stroke.color, alpha), radii, st.stroke.align);
    }

    // ── text ────────────────────────────────────────────────────────────
    if (node.kind() == NodeKind::text && opts.glyphs != nullptr && !node.text().empty()) {
        TextStyle ts = st.text;
        ts.size  = st.text.size * opts.dpi_scale;
        ts.color = apply_opacity(st.text.color, alpha);
        opts.glyphs->draw_text(dl, node.text(), deflate(frame, st.layout.padding), ts);
    }

    // ── image ───────────────────────────────────────────────────────────
    if (node.kind() == NodeKind::image && node.texture() != 0) {
        dl.textured(frame, Rect{0, 0, 1, 1}, node.texture(),
                    colors::white.fade(alpha));
    }

    // ── canvas escape hatch ─────────────────────────────────────────────
    if (node.kind() == NodeKind::canvas && node.canvas() != nullptr) {
        node.canvas()(dl, frame, node.canvas_user());
    }

    if (opts.debug_bounds) {
        dl.stroke_rect(frame, 1.0f, rgba<0xFF00FF60>, {}, StrokeAlign::inside);
    }
}

}  // namespace detail

/// Paint a laid-out tree into `dl`. Does not clear the list, so multiple roots
/// (an app plus an overlay layer) can share one frame.
inline void paint(const Node& root, DrawList& dl, const PaintOptions& opts = {}) {
    dl.reserve(root.count() * 3);
    detail::paint_node(root, dl, opts, 1.0f);
}

}  // namespace mayag::render
