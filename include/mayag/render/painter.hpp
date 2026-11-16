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

/// The painter's coordinate frame: physical pixels, with every ancestor's
/// `transform` folded in. Kept AXIS-ALIGNED (scale + translate): that is the
/// part of an affine the SDF instance format can express exactly, and it
/// covers the transforms UI state wants (press-to-shrink, slide-in, hover
/// lift). A rotation or skew contributes its scale magnitude and translation
/// and is otherwise not drawn rotated -- documented, not silent.
struct Frame2 {
    float sx = 1.0f, sy = 1.0f, tx = 0.0f, ty = 0.0f;

    [[nodiscard]] bool identity() const noexcept {
        return sx == 1.0f && sy == 1.0f && tx == 0.0f && ty == 0.0f;
    }
    [[nodiscard]] Rect map(const Rect& r) const noexcept {
        if (identity()) return r;
        return Rect{Vec2{r.origin.x * sx + tx, r.origin.y * sy + ty}, Vec2{r.size.x * sx, r.size.y * sy}};
    }
    [[nodiscard]] float len() const noexcept { return num::min(num::abs(sx), num::abs(sy)); }
    /// Radii are LOGICAL lengths, like every other length in a Style, so
    /// they scale with DPI and with the node's transform. (They used to
    /// skip DPI: on a 2x display every literal radius drew at half size and
    /// `radius(h / 2)` gave a rounded square instead of a circle.)
    [[nodiscard]] Corners radii(Corners c, float dpi) const noexcept {
        const float k = dpi * len();
        if (k == 1.0f) return c;
        return Corners{c.tl * k, c.tr * k, c.br * k, c.bl * k};
    }
    /// Compose a node's transform about its (physical, already mapped)
    /// centre: this * T(c) * M * T(-c).
    [[nodiscard]] Frame2 then(const Affine& m, Vec2 pivot, float dpi) const noexcept {
        const float msx = num::sqrt(m.a * m.a + m.b * m.b);
        const float msy = num::sqrt(m.c * m.c + m.d * m.d);
        // Pivot and translation in this frame's (physical) units.
        const float px = pivot.x, py = pivot.y;
        const float ltx = m.tx * dpi, lty = m.ty * dpi;
        // Local map: p -> pivot + S(p - pivot) + t
        Frame2 local{msx, msy, px - msx * px + ltx, py - msy * py + lty};
        return Frame2{sx * local.sx, sy * local.sy, sx * local.tx + tx, sy * local.ty + ty};
    }
};

inline void paint_node(const Node& node, DrawList& dl, const PaintOptions& opts,
                       float inherited_opacity, Frame2 xf) {
    const Style& st = node.style();
    const float alpha = inherited_opacity * st.opacity;
    if (alpha <= 0.001f) return;

    const Rect unmapped = node.frame().scale(opts.dpi_scale);
    if (!(st.transform == Affine::identity())) {
        xf = xf.then(st.transform, unmapped.center(), opts.dpi_scale);
    }
    const Rect    frame = xf.map(unmapped);
    const Corners radii = xf.radii(st.corners, opts.dpi_scale).clamp_to(frame.size);
    const float   k = opts.dpi_scale * xf.len();   // logical px -> painted px

    // ── outer shadows ───────────────────────────────────────────────────
    for (std::uint8_t i = 0; i < st.shadow_count; ++i) {
        const Shadow& sh = st.shadows[i];
        if (!sh.inset && sh.visible()) {
            Shadow scaled = sh;
            scaled.offset = sh.offset * k;
            scaled.blur   = sh.blur * k;
            scaled.spread = sh.spread * k;
            scaled.color  = apply_opacity(sh.color, alpha);
            dl.shadow(frame, scaled, radii);
        }
    }

    // ── backdrop (frosted glass) ────────────────────────────────────────
    //
    // Emitted after the outer shadows and BEFORE the fill: it blurs whatever
    // is already on the framebuffer behind this node, then the (usually
    // translucent) fill composites over the blurred result -- the vibrancy
    // look. Blur is a pixel radius, so it scales with DPI like shadow blur.
    if (st.backdrop.active()) {
        dl.backdrop(frame, st.backdrop.blur * k,
                    st.backdrop.saturation, st.backdrop.brightness, radii);
    }

    // ── fill ────────────────────────────────────────────────────────────────────────
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

    // ── children ─────────────────────────────────────────────────────────────────────
    const bool clipping = st.clip && !node.children().empty();
    if (clipping) dl.push_clip(frame);

    for (const auto& kid : node.children()) {
        paint_node(kid, dl, opts, alpha, xf);
    }

    if (clipping) dl.pop_clip();

    // ── inner shadows ───────────────────────────────────────────────────
    for (std::uint8_t i = 0; i < st.shadow_count; ++i) {
        const Shadow& sh = st.shadows[i];
        if (sh.inset && sh.visible()) {
            Shadow scaled = sh;
            scaled.offset = sh.offset * k;
            scaled.blur   = sh.blur * k;
            scaled.color  = apply_opacity(sh.color, alpha);
            dl.shadow(frame, scaled, radii);
        }
    }

    // ── border ──────────────────────────────────────────────────────────────────────
    if (st.stroke.visible()) {
        dl.stroke_rect(frame, st.stroke.width * k,
                       apply_opacity(st.stroke.color, alpha), radii, st.stroke.align);
    }

    // ── text ────────────────────────────────────────────────────────────────────────
    if (node.kind() == NodeKind::text && opts.glyphs != nullptr && !node.text().empty()) {
        TextStyle ts = st.text;
        ts.variations.auto_optical_size(st.text.size);   // logical, before DPI scaling
        ts.size  = st.text.size * opts.dpi_scale * xf.sy;
        ts.color = apply_opacity(st.text.color, alpha);
        opts.glyphs->draw_text(dl, node.text(), xf.map(deflate(unmapped, st.layout.padding)), ts);
    }

    // ── image ───────────────────────────────────────────────────────────────────────
    if (node.kind() == NodeKind::image && node.texture() != 0) {
        dl.textured(frame, Rect{0, 0, 1, 1}, node.texture(),
                    colors::white.fade(alpha));
    }

    // ── canvas escape hatch ─────────────────────────────────────────────
    if (node.kind() == NodeKind::canvas && node.canvas() != nullptr) {
        // No user data: the callback gets the node's own Style, so a stateless
        // function can still be parameterised (and nothing can dangle when
        // the tree is copied).
        node.canvas()(dl, frame, node.canvas_user() != nullptr ? node.canvas_user() : static_cast<const void*>(&st));
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
    detail::paint_node(root, dl, opts, 1.0f, detail::Frame2{});
}

}  // namespace mayag::render
