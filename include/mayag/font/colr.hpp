// include/mayag/font/colr.hpp — COLR/CPAL vector colour glyphs
//
// COLR is the modern, resolution-independent colour-glyph format: instead of
// baking a PNG per size (CBDT/sbix), a colour glyph is a stack of base-glyph
// outlines each painted with a colour or gradient. COLR v0 layers solid
// palette colours; COLR v1 adds a full paint graph — linear/radial/sweep
// gradients, affine transforms, and Porter-Duff compositing.
//
// This renders a COLR glyph straight to an RGBA ColorBitmap at the requested
// pixel size, so it is crisp at any scale (the whole point of vectors). It
// reuses the coverage rasteriser: each layer is rasterised to an 8-bit mask
// and composited over the accumulator with its resolved paint.
//
// Supported: CPAL (all formats), COLR v0 (layers), and the whole COLR v1
// paint graph (every format 1-32): layers, solids, linear / radial (two-point
// conical) / sweep gradients with pad/repeat/reflect, glyph clips, nested
// COLR glyphs with ClipBoxes, every transform, and PaintComposite with all
// 28 modes -- a port of HarfBuzz 15.1's paint model (see the v1 section).

#pragma once

#include "opentype.hpp"
#include "outline.hpp"
#include "raster.hpp"
#include "color_bitmap.hpp"
#include "../core/geometry.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace mayag::typo {

namespace colr {

using ot::Reader;

// Big-endian span readers (bounds-checked; 0 past the end).
[[nodiscard]] inline std::uint16_t be16(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    if (o + 2 > d.size()) return 0;
    return static_cast<std::uint16_t>((d[o] << 8) | d[o + 1]);
}
[[nodiscard]] inline std::int16_t be16s(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    return static_cast<std::int16_t>(be16(d, o));
}
[[nodiscard]] inline std::uint32_t be32(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    if (o + 4 > d.size()) return 0;
    return (static_cast<std::uint32_t>(d[o]) << 24) | (static_cast<std::uint32_t>(d[o + 1]) << 16) |
           (static_cast<std::uint32_t>(d[o + 2]) << 8) | d[o + 3];
}
[[nodiscard]] inline std::uint32_t be24(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    if (o + 3 > d.size()) return 0;
    return (static_cast<std::uint32_t>(d[o]) << 16) | (static_cast<std::uint32_t>(d[o + 1]) << 8) | d[o + 2];
}
/// F2Dot14 fixed point.
[[nodiscard]] inline float bef2dot14(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    return static_cast<float>(be16s(d, o)) / 16384.0f;
}
/// Fixed 16.16.
[[nodiscard]] inline float befixed(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    return static_cast<float>(static_cast<std::int32_t>(be32(d, o))) / 65536.0f;
}

// ── CPAL: the colour palette ────────────────────────────────────────────

struct Rgba { std::uint8_t r = 0, g = 0, b = 0, a = 255; };

/// A parsed CPAL. Colours are stored straight (un-premultiplied), sRGB.
struct Palette {
    std::vector<Rgba> colors;   ///< first palette only (the default)
    [[nodiscard]] bool ok() const noexcept { return !colors.empty(); }

    /// Palette entry, or `fallback` for the special 0xFFFF "use text colour"
    /// index and any out-of-range index.
    [[nodiscard]] Rgba at(std::uint16_t idx, Rgba fallback) const noexcept {
        if (idx == 0xFFFF || idx >= colors.size()) return fallback;
        return colors[idx];
    }
};

[[nodiscard]] inline Palette parse_cpal(std::span<const std::uint8_t> cpal) {
    Palette p;
    if (cpal.size() < 12) return p;
    Reader r{cpal, 0};
    const std::uint16_t version           = r.u16();
    const std::uint16_t entries_per_pal   = r.u16();
    const std::uint16_t num_palettes      = r.u16();
    const std::uint16_t num_color_records = r.u16();
    const std::uint32_t records_off       = r.u32();
    if (num_palettes == 0 || entries_per_pal == 0) return p;
    (void)version; (void)num_palettes;

    // colorRecordIndices[num_palettes] follows; palette 0 starts at index[0].
    const std::uint16_t first_index = r.u16();
    if (static_cast<std::size_t>(first_index) + entries_per_pal > num_color_records)
        return p;

    p.colors.resize(entries_per_pal);
    for (std::uint16_t i = 0; i < entries_per_pal; ++i) {
        const std::size_t o = records_off +
            static_cast<std::size_t>(first_index + i) * 4;
        if (o + 4 > cpal.size()) { p.colors.clear(); return p; }
        // CPAL stores BGRA.
        p.colors[i] = Rgba{cpal[o + 2], cpal[o + 1], cpal[o + 0], cpal[o + 3]};
    }
    return p;
}

// ── COLR: the colour-glyph table ──────────────────────────────────

/// A parsed COLR pointing back into the font bytes. Cheap to hold; rendering
/// walks the layer list (v0) or paint graph (v1) on demand.
struct ColrTable {
    bool ok = false;
    std::uint16_t version = 0;
    std::span<const std::uint8_t> data{};

    // v0
    std::size_t   base_records_off = 0;   // BaseGlyphRecord[numBaseGlyphRecords]
    std::uint16_t num_base_records = 0;
    std::size_t   layer_records_off = 0;   // LayerRecord[numLayerRecords]
    std::uint16_t num_layer_records = 0;

    // v1
    std::size_t base_list_off = 0;   // BaseGlyphList (0 = none)
    std::size_t layer_list_off = 0;  // LayerList (0 = none)
    std::size_t clip_list_off = 0;   // ClipList (0 = none)
};

[[nodiscard]] inline ColrTable parse_colr(std::span<const std::uint8_t> colr) {
    ColrTable t;
    if (colr.size() < 14) return t;
    Reader r{colr, 0};
    t.version = r.u16();
    const std::uint16_t num_base = r.u16();
    const std::uint32_t base_off = r.u32();
    const std::uint32_t layer_off = r.u32();
    const std::uint16_t num_layer = r.u16();
    t.ok = true;
    t.data = colr;
    t.num_base_records  = num_base;
    t.base_records_off  = base_off;
    t.num_layer_records = num_layer;
    t.layer_records_off = layer_off;
    if (t.version >= 1 && colr.size() >= 26) {
        const std::uint32_t base_list  = r.u32();  // offset32 to BaseGlyphList
        const std::uint32_t layer_list = r.u32();  // offset32 to LayerList
        const std::uint32_t clip_list  = r.u32();  // offset32 to ClipList
        // (itemVariationStore offset follows; variations of COLR deferred.)
        t.base_list_off  = base_list;
        t.layer_list_off = layer_list;
        t.clip_list_off  = clip_list;
    }
    return t;
}

/// Does `gid` have a colour representation (v0 layers or a v1 paint)?
[[nodiscard]] inline bool has_color_glyph(const ColrTable& t, std::uint16_t gid) {
    if (!t.ok) return false;
    // v0 base-glyph records are sorted by gid; binary search.
    std::uint16_t lo = 0, hi = t.num_base_records;
    while (lo < hi) {
        const std::uint16_t mid = static_cast<std::uint16_t>((lo + hi) / 2);
        const std::size_t rec = t.base_records_off + static_cast<std::size_t>(mid) * 6;
        const std::uint16_t g = be16(t.data, rec);
        if (g == gid) return true;
        if (g < gid) lo = static_cast<std::uint16_t>(mid + 1); else hi = mid;
    }
    // v1 BaseGlyphList (also sorted by gid).
    if (t.base_list_off != 0 && t.base_list_off + 4 <= t.data.size()) {
        const std::uint32_t n = be32(t.data, t.base_list_off);
        std::uint32_t l = 0, h = n;
        while (l < h) {
            const std::uint32_t mid = (l + h) / 2;
            const std::size_t rec = t.base_list_off + 4 + static_cast<std::size_t>(mid) * 6;
            const std::uint16_t g = be16(t.data, rec);
            if (g == gid) return true;
            if (g < gid) l = mid + 1; else h = mid;
        }
    }
    return false;
}

// ── rendering ──────────────────────────────────────────────────

/// A premultiplied-alpha RGBA float accumulator we paint COLR layers into,
/// then convert to straight 8-bit RGBA for the atlas.
struct Canvas {
    int w = 0, h = 0;
    std::vector<float> px;   // w*h*4, premultiplied, y-down

    Canvas(int width, int height)
        : w(width), h(height),
          px(static_cast<std::size_t>(width) * height * 4, 0.0f) {}

    // Source-over one pixel: src is premultiplied (r,g,b already *a).
    void over(int x, int y, float sr, float sg, float sb, float sa) noexcept {
        if (x < 0 || y < 0 || x >= w || y >= h || sa <= 0.0f) return;
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        const float ia = 1.0f - sa;
        px[i + 0] = sr + px[i + 0] * ia;
        px[i + 1] = sg + px[i + 1] * ia;
        px[i + 2] = sb + px[i + 2] * ia;
        px[i + 3] = sa + px[i + 3] * ia;
    }

    [[nodiscard]] ColorBitmap to_bitmap(float ppem) const {
        ColorBitmap cb;
        cb.width = w;
        cb.height = h;
        cb.ppem = static_cast<int>(ppem + 0.5f);
        cb.rgba.resize(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t p = 0; p < static_cast<std::size_t>(w) * h; ++p) {
            const float a = px[p * 4 + 3];
            auto unpre = [&](float c) {
                const float v = a > 0.0001f ? c / a : 0.0f;
                return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            cb.rgba[p * 4 + 0] = unpre(px[p * 4 + 0]);
            cb.rgba[p * 4 + 1] = unpre(px[p * 4 + 1]);
            cb.rgba[p * 4 + 2] = unpre(px[p * 4 + 2]);
            cb.rgba[p * 4 + 3] = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        return cb;
    }
};

/// The parameters that map font units (y-up, origin at glyph origin) to canvas
/// pixels (y-down). x_px = fx*scale - ox ; y_px = -fy*scale - oy.
struct Placement {
    float scale = 1.0f;
    float ox = 0.0f;   // pixel of font-unit x=0
    float oy = 0.0f;   // pixel of font-unit y=0
};

// A resolved paint: a function from canvas pixel (px,py) -> straight RGBA.
// COLR v0 layers use a constant colour; v1 gradients vary per pixel.
using PaintFn = std::function<Rgba(float px, float py)>;

/// Rasterise `outline` (font units, y-up) at `pl` and composite `paint`
/// through its coverage into `cv`. This is the one place geometry becomes
/// pixels for COLR, mirroring raster.hpp's flip.
inline void fill_outline(Canvas& cv, const Outline& outline, const Placement& pl,
                         const PaintFn& paint) {
    if (outline.empty() || pl.scale <= 0.0f) return;
    const RasterResult rr = rasterize(outline, pl.scale, /*padding*/1);
    if (rr.bitmap.width <= 0 || rr.bitmap.height <= 0) return;
    // rasterize() returns coverage with its own offset relative to the glyph
    // origin (y-down px). Map that onto our shared canvas.
    // rr.offset is the pixel position of the bitmap's top-left relative to the
    // origin, using the SAME convention rasterize uses internally: the bitmap
    // top-left sits at (floor(left*scale)-pad, floor(-top*scale)-pad).
    const float bx = rr.offset.x + pl.ox;
    const float by = rr.offset.y + pl.oy;
    for (int y = 0; y < rr.bitmap.height; ++y) {
        for (int x = 0; x < rr.bitmap.width; ++x) {
            const float cov = static_cast<float>(rr.bitmap.at(x, y)) / 255.0f;
            if (cov <= 0.0f) continue;
            const int cx = static_cast<int>(bx) + x;
            const int cy = static_cast<int>(by) + y;
            const Rgba c = paint(static_cast<float>(cx) + 0.5f, static_cast<float>(cy) + 0.5f);
            const float a = (static_cast<float>(c.a) / 255.0f) * cov;
            if (a <= 0.0f) continue;
            const float r = static_cast<float>(c.r) / 255.0f * a;
            const float g = static_cast<float>(c.g) / 255.0f * a;
            const float b = static_cast<float>(c.b) / 255.0f * a;
            cv.over(cx, cy, r, g, b, a);
        }
    }
}

// ── top-level render ──────────────────────────────────────────

/// Everything render_glyph needs that lives outside COLR itself: how to fetch
/// a base-glyph outline, the palette, the text colour (for palette index
/// 0xFFFF / "use foreground"), and units-per-em.
struct RenderContext {
    std::function<Outline(std::uint16_t)> outline_of;
    const Palette* palette = nullptr;
    Rgba text_color{0, 0, 0, 255};
    std::uint16_t upem = 1000;
};

// ---- COLR v1 gradient stops ----

enum class Extend : std::uint8_t { pad = 0, repeat = 1, reflect = 2 };

struct Stop { float offset; Rgba color; };

/// Parse a ColorLine (its extend mode + stops) at `off`, resolving palette
/// indices against `ctx`. Alpha in each stop is an F2Dot14 multiplier.
[[nodiscard]] inline std::vector<Stop> parse_color_line(
        std::span<const std::uint8_t> d, std::size_t off,
        const RenderContext& ctx, Extend& extend) {
    std::vector<Stop> stops;
    if (off + 3 > d.size()) return stops;
    // ColorLine = uint8 extend; uint16 numStops; ColorStop[numStops].
    const std::uint8_t ext = d[off] > 2 ? 0 : d[off];
    extend = static_cast<Extend>(ext);
    const std::uint16_t n = be16(d, off + 1);
    for (std::uint16_t i = 0; i < n; ++i) {
        const std::size_t rec = off + 3 + static_cast<std::size_t>(i) * 6;
        if (rec + 6 > d.size()) break;
        const float o = bef2dot14(d, rec);
        const std::uint16_t pal = be16(d, rec + 2);
        const float alpha = bef2dot14(d, rec + 4);
        Rgba c = ctx.palette ? ctx.palette->at(pal, ctx.text_color) : ctx.text_color;
        c.a = static_cast<std::uint8_t>(std::clamp(static_cast<float>(c.a) * alpha, 0.0f, 255.0f));
        stops.push_back(Stop{o, c});
    }
    return stops;
}

/// Sample the gradient ramp at parameter t (pre-extend), applying the extend
/// mode and linearly interpolating between stops. Empty ramp => transparent.
[[nodiscard]] inline Rgba sample_ramp(const std::vector<Stop>& s, float t, Extend ext) {
    if (s.empty()) return Rgba{0, 0, 0, 0};
    if (s.size() == 1) return s.front().color;
    const float lo = s.front().offset, hi = s.back().offset;
    const float span = hi - lo;
    if (span <= 0.0f) return s.front().color;
    // Normalise t into [lo,hi] per the extend mode.
    float u = (t - lo) / span;   // 0..1 across the defined range
    switch (ext) {
        case Extend::pad: u = std::clamp(u, 0.0f, 1.0f); break;
        case Extend::repeat: u = u - std::floor(u); break;
        case Extend::reflect: {
            float m = std::fmod(std::fabs(u), 2.0f);
            u = m > 1.0f ? 2.0f - m : m;
            break;
        }
    }
    const float tt = lo + u * span;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (tt <= s[i].offset) {
            const float seg = s[i].offset - s[i - 1].offset;
            const float f = seg > 0.0f ? (tt - s[i - 1].offset) / seg : 0.0f;
            const Rgba a = s[i - 1].color, b = s[i].color;
            auto lerp = [&](std::uint8_t x, std::uint8_t y) {
                return static_cast<std::uint8_t>(x + (y - x) * f + 0.5f);
            };
            return Rgba{lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
        }
    }
    return s.back().color;
}

namespace detail {

// v0 base-glyph record lookup -> [firstLayer, numLayers).
inline bool v0_layers(const ColrTable& t, std::uint16_t gid,
                      std::uint16_t& first, std::uint16_t& count) {
    std::uint16_t lo = 0, hi = t.num_base_records;
    while (lo < hi) {
        const std::uint16_t mid = static_cast<std::uint16_t>((lo + hi) / 2);
        const std::size_t rec = t.base_records_off + static_cast<std::size_t>(mid) * 6;
        const std::uint16_t g = be16(t.data, rec);
        if (g == gid) {
            first = be16(t.data, rec + 2);
            count = be16(t.data, rec + 4);
            return true;
        }
        if (g < gid) lo = static_cast<std::uint16_t>(mid + 1); else hi = mid;
    }
    return false;
}

}  // namespace detail

// ---- COLR v1 paint graph ----
//
// A port of HarfBuzz 15.1's COLRv1 paint model (OT/Color/COLR/COLR.hh walked
// into hb-raster-paint.cc's surface/clip/group stack), so emoji like Noto
// Color Emoji's flags -- PaintComposite MULTIPLY over SRC_IN -- render, and
// every paint matches the spec rather than an approximation:
//
//   * a GROUP stack: PaintComposite paints its backdrop and source into their
//     own transparent surfaces and blends them with any of the 28 modes
//     (Porter-Duff, separable and HSL blends), then source-overs the result;
//   * a CLIP stack of coverage masks: PaintGlyph clips its child paint to the
//     glyph outline (not "fill outline with paint"), nested clips multiply,
//     PaintColrGlyph clips to its ClipBox;
//   * gradients evaluated per pixel through the inverse transform, colours
//     interpolated in PREMULTIPLIED space; linear gradients reduce the
//     3-point spec to an axis (hb_paint_reduce_linear_anchors), colour lines
//     are normalised (hb_paint_normalize_color_line), radial gradients solve
//     the two-point conical equation for the largest valid root, sweep angles
//     are (F2Dot14 + 1) * pi;
//   * field types as the spec has them: scales and angles F2Dot14 (angles in
//     half-turns), translations and centres FWORD, Affine2x3 Fixed 16.16;
//     skew x is negated like HarfBuzz's push_skew.
// Variation deltas (the Var* formats) read their base values.

namespace detail {

/// A premultiplied RGBA surface plus a coverage mask, the unit of the stacks.
struct Surface {
    int w = 0, h = 0;
    std::vector<float> px;   // premultiplied rgba
    explicit Surface(int width = 0, int height = 0)
        : w(width), h(height), px(static_cast<std::size_t>(width) * height * 4, 0.0f) {}
};

struct V1 {
    const ColrTable& t;
    const RenderContext& ctx;
    int w = 0, h = 0;
    Placement pl;
    V1(const ColrTable& table, const RenderContext& rc, int width, int height, Placement p)
        : t(table), ctx(rc), w(width), h(height), pl(p) {}
    std::vector<Surface> groups;          // back() is the current surface
    std::vector<std::vector<float>> clips;   // back() is the current clip (coverage)
    struct Box { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
    std::vector<Box> clip_boxes;          // pixel bounds of each clip's nonzero coverage
    std::vector<Affine> xforms;           // back() maps paint space -> font units
    int budget = 200000;                  // paint visits (hostile graphs)
    std::vector<std::uint16_t> glyph_stack;   // decycler for PaintColrGlyph
    std::vector<std::uint32_t> layer_stack;   // decycler for PaintColrLayers

    [[nodiscard]] Surface& surface() { return groups.back(); }
    [[nodiscard]] const std::vector<float>& clip() const { return clips.back(); }
    [[nodiscard]] const Affine& xform() const { return xforms.back(); }

    // Canvas pixel centre -> paint-space point (font units, y-up).
    [[nodiscard]] Vec2 to_paint(const Affine& inv, int x, int y) const {
        const float fx = (static_cast<float>(x) + 0.5f - pl.ox) / pl.scale;
        const float fy = -(static_cast<float>(y) + 0.5f - pl.oy) / pl.scale;
        return inv.apply(Vec2{fx, fy});
    }

    /// Fill the current clip with a per-pixel premultiplied colour, source-over.
    template <class ColourAt>
    void fill(ColourAt&& colour_at) {
        Surface& s = surface();
        const auto& c = clip();
        const Box b = clip_boxes.back();
        const Affine inv = xform().inverse();
        for (int y = b.y0; y < b.y1; ++y)
            for (int x = b.x0; x < b.x1; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const float cov = c[i];
                if (cov <= 0.0f) continue;
                float rgba[4];
                colour_at(to_paint(inv, x, y), rgba);
                const float a = rgba[3] * cov;
                if (a <= 0.0f) continue;
                const float ia = 1.0f - a;
                float* d = &s.px[i * 4];
                d[0] = rgba[0] * cov + d[0] * ia;
                d[1] = rgba[1] * cov + d[1] * ia;
                d[2] = rgba[2] * cov + d[2] * ia;
                d[3] = a + d[3] * ia;
            }
    }

    /// Push a clip: the current clip times `outline`'s coverage, the outline
    /// mapped through the current transform (font units) and the placement.
    void push_clip_outline(Outline o) {
        o.transform(xform());
        std::vector<float> m(static_cast<std::size_t>(w) * h, 0.0f);
        Box nb{w, h, 0, 0};
        const Box pb = clip_boxes.back();
        if (!o.empty()) {
            const RasterResult rr = rasterize(o, pl.scale, 1);
            const int bx = static_cast<int>(std::lround(rr.offset.x + pl.ox));
            const int by = static_cast<int>(std::lround(rr.offset.y + pl.oy));
            const auto& prev = clip();
            for (int y = 0; y < rr.bitmap.height; ++y) {
                const int cy = by + y;
                if (cy < pb.y0 || cy >= pb.y1) continue;
                for (int x = 0; x < rr.bitmap.width; ++x) {
                    const int cx = bx + x;
                    if (cx < pb.x0 || cx >= pb.x1) continue;
                    const std::size_t i = static_cast<std::size_t>(cy) * w + cx;
                    const float v = static_cast<float>(rr.bitmap.at(x, y)) / 255.0f * prev[i];
                    if (v <= 0.0f) continue;
                    m[i] = v;
                    nb.x0 = std::min(nb.x0, cx); nb.y0 = std::min(nb.y0, cy);
                    nb.x1 = std::max(nb.x1, cx + 1); nb.y1 = std::max(nb.y1, cy + 1);
                }
            }
        }
        if (nb.x1 <= nb.x0) nb = Box{};
        clips.push_back(std::move(m));
        clip_boxes.push_back(nb);
    }

    void pop_clip() { clips.pop_back(); clip_boxes.pop_back(); }

    /// Push a clip rectangle given in the CURRENT paint space (font units).
    void push_clip_rect(float x0, float y0, float x1, float y1) {
        Outline o;
        Contour c;
        c.start = Vec2{x0, y0};
        c.segments.push_back(Segment{Segment::Kind::line, {}, {}, Vec2{x1, y0}});
        c.segments.push_back(Segment{Segment::Kind::line, {}, {}, Vec2{x1, y1}});
        c.segments.push_back(Segment{Segment::Kind::line, {}, {}, Vec2{x0, y1}});
        c.segments.push_back(Segment{Segment::Kind::line, {}, {}, Vec2{x0, y0}});
        o.contours.push_back(std::move(c));
        push_clip_outline(std::move(o));
    }
};

// ── colours and colour lines ─────────────────────────────────────────────

struct PStop { float offset; float c[4]; };   // premultiplied colour

[[nodiscard]] inline std::array<float, 4> premul(Rgba c, float alpha_mul = 1.0f) {
    const float a = std::clamp(static_cast<float>(c.a) / 255.0f * alpha_mul, 0.0f, 1.0f);
    return {static_cast<float>(c.r) / 255.0f * a, static_cast<float>(c.g) / 255.0f * a,
            static_cast<float>(c.b) / 255.0f * a, a};
}

/// ColorLine (VarColorLine for var formats: 4 more bytes per stop), sorted
/// and normalised to [0, 1] (hb_paint_normalize_color_line). `mn`/`mx` are
/// the original offset range; gradients rescale their geometry by them.
inline std::vector<PStop> color_line(const V1& s, std::size_t off, bool var, Extend& ext, float& mn, float& mx) {
    std::vector<PStop> stops;
    const auto d = s.t.data;
    mn = mx = 0.0f;
    if (off + 3 > d.size()) return stops;
    ext = d[off] > 2 ? Extend::pad : static_cast<Extend>(d[off]);
    const std::uint16_t n = be16(d, off + 1);
    const std::size_t rec_size = var ? 10 : 6;
    for (std::uint16_t i = 0; i < n && i < 1024; ++i) {
        const std::size_t rec = off + 3 + static_cast<std::size_t>(i) * rec_size;
        if (rec + 6 > d.size()) break;
        const std::uint16_t pal = be16(d, rec + 2);
        const Rgba base = (pal == 0xFFFF || s.ctx.palette == nullptr) ? s.ctx.text_color
                                                                      : s.ctx.palette->at(pal, s.ctx.text_color);
        const auto c = premul(base, bef2dot14(d, rec + 4));
        stops.push_back(PStop{bef2dot14(d, rec), {c[0], c[1], c[2], c[3]}});
    }
    if (stops.empty()) return stops;
    std::stable_sort(stops.begin(), stops.end(), [](const PStop& a, const PStop& b) { return a.offset < b.offset; });
    mn = stops.front().offset;
    mx = stops.back().offset;
    if (mn != mx)
        for (auto& st : stops) st.offset = (st.offset - mn) / (mx - mn);
    return stops;
}

/// evaluate_color_line: extend, then interpolate in premultiplied space.
inline void eval_line(const std::vector<PStop>& st, float t, Extend ext, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (st.empty()) return;
    if (st.size() == 1 || !std::isfinite(t)) { std::copy(st[0].c, st[0].c + 4, out); if (std::isfinite(t)) return; t = 0; }
    if (ext == Extend::pad) t = std::clamp(t, 0.0f, 1.0f);
    else if (ext == Extend::repeat) t = t - std::floor(t);
    else { t = std::fmod(std::fabs(t), 2.0f); if (t > 1.0f) t = 2.0f - t; }
    if (t <= st.front().offset) { std::copy(st.front().c, st.front().c + 4, out); return; }
    if (t >= st.back().offset) { std::copy(st.back().c, st.back().c + 4, out); return; }
    std::size_t i = 0;
    while (i + 1 < st.size() && !(t < st[i + 1].offset)) ++i;
    const float range = st[i + 1].offset - st[i].offset;
    const float f = range > 0.0f ? (t - st[i].offset) / range : 0.0f;
    for (int k = 0; k < 4; ++k) out[k] = st[i].c[k] + (st[i + 1].c[k] - st[i].c[k]) * f;
}

// ── compositing (hb-raster-image.cc composite_pixel) ─────────────────────

[[nodiscard]] inline float blend_sep(unsigned mode, float sc, float dc) {
    switch (mode) {
        case 13: return sc + dc - sc * dc;                                                    // screen
        case 14: return dc <= 0.5f ? 2 * sc * dc : 1 - 2 * (1 - sc) * (1 - dc);              // overlay
        case 15: return std::min(sc, dc);                                                     // darken
        case 16: return std::max(sc, dc);                                                     // lighten
        case 17: return dc <= 0 ? 0 : sc >= 1 ? 1 : std::min(1.0f, dc / (1 - sc));          // color dodge
        case 18: return dc >= 1 ? 1 : sc <= 0 ? 0 : 1 - std::min(1.0f, (1 - dc) / sc);      // color burn
        case 19: return sc <= 0.5f ? 2 * sc * dc : 1 - 2 * (1 - sc) * (1 - dc);              // hard light
        case 20: {                                                                            // soft light
            if (sc <= 0.5f) return dc - (1 - 2 * sc) * dc * (1 - dc);
            const float d = dc <= 0.25f ? ((16 * dc - 12) * dc + 4) * dc : std::sqrt(dc);
            return dc + (2 * sc - 1) * (d - dc);
        }
        case 21: return std::fabs(sc - dc);                                                   // difference
        case 22: return sc + dc - 2 * sc * dc;                                                // exclusion
        case 23: return sc * dc;                                                              // multiply
        default: return sc;
    }
}

[[nodiscard]] inline float hsl_lum(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }
inline void hsl_clip(float& r, float& g, float& b) {
    const float l = hsl_lum(r, g, b), n = std::min({r, g, b}), x = std::max({r, g, b});
    if (n < 0 && l - n > 0) { r = l + (r - l) * l / (l - n); g = l + (g - l) * l / (l - n); b = l + (b - l) * l / (l - n); }
    if (x > 1 && x - l > 0) { r = l + (r - l) * (1 - l) / (x - l); g = l + (g - l) * (1 - l) / (x - l); b = l + (b - l) * (1 - l) / (x - l); }
}
inline void hsl_set_lum(float& r, float& g, float& b, float l) {
    const float d = l - hsl_lum(r, g, b); r += d; g += d; b += d; hsl_clip(r, g, b);
}
inline void hsl_set_sat(float& r, float& g, float& b, float sat) {
    float* c[3] = {&r, &g, &b};
    std::sort(c, c + 3, [](float* a, float* bb) { return *a < *bb; });
    if (*c[2] > *c[0]) { *c[1] = (*c[1] - *c[0]) * sat / (*c[2] - *c[0]); *c[2] = sat; }
    else { *c[1] = *c[2] = 0; }
    *c[0] = 0;
}

/// Composite premultiplied `s` onto premultiplied `d` with CompositeMode `mode`.
inline void composite(unsigned mode, const float* s, float* d) {
    const float sa = s[3], da = d[3];
    float o[4];
    switch (mode) {
        case 0: o[0] = o[1] = o[2] = o[3] = 0; break;                                        // clear
        case 1: std::copy(s, s + 4, o); break;                                               // src
        case 2: return;                                                                      // dest
        case 3: for (int k = 0; k < 4; ++k) o[k] = s[k] + d[k] * (1 - sa); break;           // src over
        case 4: for (int k = 0; k < 4; ++k) o[k] = d[k] + s[k] * (1 - da); break;           // dest over
        case 5: for (int k = 0; k < 4; ++k) o[k] = s[k] * da; break;                        // src in
        case 6: for (int k = 0; k < 4; ++k) o[k] = d[k] * sa; break;                        // dest in
        case 7: for (int k = 0; k < 4; ++k) o[k] = s[k] * (1 - da); break;                  // src out
        case 8: for (int k = 0; k < 4; ++k) o[k] = d[k] * (1 - sa); break;                  // dest out
        case 9: for (int k = 0; k < 3; ++k) o[k] = s[k] * da + d[k] * (1 - sa); o[3] = da; break;   // src atop
        case 10: for (int k = 0; k < 3; ++k) o[k] = d[k] * sa + s[k] * (1 - da); o[3] = sa; break;  // dest atop
        case 11: for (int k = 0; k < 4; ++k) o[k] = s[k] * (1 - da) + d[k] * (1 - sa); break;       // xor
        case 12: for (int k = 0; k < 4; ++k) o[k] = std::min(1.0f, s[k] + d[k]); break;             // plus
        default: {
            // Separable / HSL blends (W3C compositing): unpremultiply, blend,
            // then B(cs,cb)*sa*da + cs*sa*(1-da) + cb*da*(1-sa).
            const float us[3] = {sa > 0 ? s[0] / sa : 0, sa > 0 ? s[1] / sa : 0, sa > 0 ? s[2] / sa : 0};
            const float ud[3] = {da > 0 ? d[0] / da : 0, da > 0 ? d[1] / da : 0, da > 0 ? d[2] / da : 0};
            float b[3];
            if (mode >= 24 && mode <= 27) {
                float r = 0, g = 0, bb = 0;
                if (mode == 24) { r = us[0]; g = us[1]; bb = us[2];
                    hsl_set_sat(r, g, bb, std::max({ud[0], ud[1], ud[2]}) - std::min({ud[0], ud[1], ud[2]}));
                    hsl_set_lum(r, g, bb, hsl_lum(ud[0], ud[1], ud[2])); }                    // hue
                else if (mode == 25) { r = ud[0]; g = ud[1]; bb = ud[2];
                    hsl_set_sat(r, g, bb, std::max({us[0], us[1], us[2]}) - std::min({us[0], us[1], us[2]}));
                    hsl_set_lum(r, g, bb, hsl_lum(ud[0], ud[1], ud[2])); }                    // saturation
                else if (mode == 26) { r = us[0]; g = us[1]; bb = us[2];
                    hsl_set_lum(r, g, bb, hsl_lum(ud[0], ud[1], ud[2])); }                    // color
                else { r = ud[0]; g = ud[1]; bb = ud[2];
                    hsl_set_lum(r, g, bb, hsl_lum(us[0], us[1], us[2])); }                    // luminosity
                b[0] = r; b[1] = g; b[2] = bb;
            } else {
                for (int k = 0; k < 3; ++k) b[k] = blend_sep(mode, us[k], ud[k]);
            }
            for (int k = 0; k < 3; ++k) o[k] = sa * da * b[k] + sa * (1 - da) * us[k] + (1 - sa) * da * ud[k];
            o[3] = sa + da - sa * da;
            break;
        }
    }
    for (int k = 0; k < 4; ++k) d[k] = std::clamp(o[k], 0.0f, 1.0f);
}

// ── the walk ─────────────────────────────────────────────────────────────

inline bool find_v1_base(const ColrTable& t, std::uint16_t gid, std::size_t& paint_off) {
    if (t.base_list_off == 0 || t.base_list_off + 4 > t.data.size()) return false;
    const std::uint32_t n = be32(t.data, t.base_list_off);
    std::uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const std::uint32_t mid = (lo + hi) / 2;
        const std::size_t rec = t.base_list_off + 4 + static_cast<std::size_t>(mid) * 6;
        if (rec + 6 > t.data.size()) return false;
        const std::uint16_t g = be16(t.data, rec);
        if (g == gid) { paint_off = t.base_list_off + be32(t.data, rec + 2); return paint_off < t.data.size(); }
        if (g < gid) lo = mid + 1; else hi = mid;
    }
    return false;
}

/// ClipList lookup: the glyph's ClipBox in font units, if it has one.
inline bool clip_box(const ColrTable& t, std::uint16_t gid, float& x0, float& y0, float& x1, float& y1) {
    if (t.clip_list_off == 0 || t.clip_list_off + 5 > t.data.size()) return false;
    const std::uint32_t n = be32(t.data, t.clip_list_off + 1);
    for (std::uint32_t i = 0; i < n && i < 65536; ++i) {
        const std::size_t rec = t.clip_list_off + 5 + static_cast<std::size_t>(i) * 7;
        if (rec + 7 > t.data.size()) return false;
        if (gid >= be16(t.data, rec) && gid <= be16(t.data, rec + 2)) {
            const std::size_t box = t.clip_list_off + be24(t.data, rec + 4);
            if (box + 9 > t.data.size()) return false;
            x0 = be16s(t.data, box + 1); y0 = be16s(t.data, box + 3);
            x1 = be16s(t.data, box + 5); y1 = be16s(t.data, box + 7);
            return true;
        }
    }
    return false;
}

inline void paint(V1& s, std::size_t off, int depth);

/// Recurse into a child paint under an extra transform `m` (paint space).
inline void paint_transformed(V1& s, std::size_t child, const Affine& m, int depth) {
    s.xforms.push_back(s.xform() * m);
    paint(s, child, depth + 1);
    s.xforms.pop_back();
}

inline void paint(V1& s, std::size_t off, int depth) {
    const auto d = s.t.data;
    if (depth > 64 || off >= d.size() || --s.budget < 0) return;
    const std::uint8_t fmt = d[off];
    const std::size_t p = off + 1;
    const auto child_at = [&](std::size_t field) -> std::size_t {
        const std::uint32_t o = be24(d, field);
        return o ? off + o : static_cast<std::size_t>(-1);
    };
    constexpr float pi = 3.14159265358979323846f;
    switch (fmt) {
    case 1: {   // PaintColrLayers
        const std::uint8_t n = d[p];
        const std::uint32_t first = be32(d, p + 1);
        if (s.t.layer_list_off == 0 || s.t.layer_list_off + 4 > d.size()) return;
        for (const std::uint32_t l : s.layer_stack) if (l == first) return;   // cycle
        s.layer_stack.push_back(first);
        const std::uint32_t total = be32(d, s.t.layer_list_off);
        for (std::uint32_t i = first; i < first + n && i < total; ++i) {
            const std::size_t rec = s.t.layer_list_off + 4 + static_cast<std::size_t>(i) * 4;
            if (rec + 4 > d.size()) break;
            paint(s, s.t.layer_list_off + be32(d, rec), depth + 1);
        }
        s.layer_stack.pop_back();
        break;
    }
    case 2: case 3: {   // PaintSolid / PaintVarSolid
        const std::uint16_t pal = be16(d, p);
        const Rgba base = (pal == 0xFFFF || s.ctx.palette == nullptr) ? s.ctx.text_color
                                                                      : s.ctx.palette->at(pal, s.ctx.text_color);
        const auto c = premul(base, bef2dot14(d, p + 2));
        s.fill([&](Vec2, float out[4]) { std::copy(c.begin(), c.end(), out); });
        break;
    }
    case 4: case 5: {   // PaintLinearGradient
        Extend ext = Extend::pad; float mn, mx;
        const auto st = color_line(s, child_at(p), fmt == 5, ext, mn, mx);
        float x0 = be16s(d, p + 3), y0 = be16s(d, p + 5), x1 = be16s(d, p + 7), y1 = be16s(d, p + 9);
        const float x2 = be16s(d, p + 11), y2 = be16s(d, p + 13);
        // hb_paint_reduce_linear_anchors: project P1 onto the normal of P0P2.
        const float q2x = x2 - x0, q2y = y2 - y0, sq = q2x * q2x + q2y * q2y;
        if (sq >= 1e-6f) {
            const float k = (q2x * (x1 - x0) + q2y * (y1 - y0)) / sq;
            x1 -= k * q2x; y1 -= k * q2y;
        }
        // normalised colour line: move the endpoints with it
        const float ax = x0 + mn * (x1 - x0), ay = y0 + mn * (y1 - y0);
        const float bx = x0 + mx * (x1 - x0), by = y0 + mx * (y1 - y0);
        const float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
        s.fill([&](Vec2 q, float out[4]) {
            const float t = len2 > 0 ? ((q.x - ax) * vx + (q.y - ay) * vy) / len2 : 0.0f;
            eval_line(st, t, ext, out);
        });
        break;
    }
    case 6: case 7: {   // PaintRadialGradient (two-point conical)
        Extend ext = Extend::pad; float mn, mx;
        const auto st = color_line(s, child_at(p), fmt == 7, ext, mn, mx);
        const float x0 = be16s(d, p + 3), y0 = be16s(d, p + 5), r0 = be16(d, p + 7);
        const float x1 = be16s(d, p + 9), y1 = be16s(d, p + 11), r1 = be16(d, p + 13);
        const float cx0 = x0 + mn * (x1 - x0), cy0 = y0 + mn * (y1 - y0), cr0 = r0 + mn * (r1 - r0);
        const float cx1 = x0 + mx * (x1 - x0), cy1 = y0 + mx * (y1 - y0), cr1 = r0 + mx * (r1 - r0);
        const float cdx = cx1 - cx0, cdy = cy1 - cy0, dr = cr1 - cr0;
        const float A = cdx * cdx + cdy * cdy - dr * dr;
        s.fill([&](Vec2 q, float out[4]) {
            out[0] = out[1] = out[2] = out[3] = 0;
            const float dx = q.x - cx0, dy = q.y - cy0;
            const float B = -2 * (dx * cdx + dy * cdy + cr0 * dr);
            const float C = dx * dx + dy * dy - cr0 * cr0;
            float t;
            if (std::fabs(A) > 1e-10f) {
                const float disc = B * B - 4 * A * C;
                if (disc < 0) return;
                const float sq = std::sqrt(disc);
                const float t1 = (-B + sq) / (2 * A), t2 = (-B - sq) / (2 * A);
                // the larger root whose radius is non-negative
                if (cr0 + t1 * dr >= 0) t = t1;
                else if (cr0 + t2 * dr >= 0) t = t2;
                else return;
                if (A < 0 && cr0 + std::max(t1, t2) * dr >= 0) t = std::max(t1, t2);
            } else {
                if (std::fabs(B) < 1e-10f) return;
                t = -C / B;
                if (cr0 + t * dr < 0) return;
            }
            eval_line(st, t, ext, out);
        });
        break;
    }
    case 8: case 9: {   // PaintSweepGradient
        Extend ext = Extend::pad; float mn, mx;
        const auto st = color_line(s, child_at(p), fmt == 9, ext, mn, mx);
        const float cx = be16s(d, p + 3), cy = be16s(d, p + 5);
        const float a0 = (bef2dot14(d, p + 7) + 1) * pi, a1 = (bef2dot14(d, p + 9) + 1) * pi;
        // normalised colour line rescales the angle range
        const float s0 = a0 + mn * (a1 - a0), s1 = a0 + mx * (a1 - a0);
        const float range = s1 - s0;
        s.fill([&](Vec2 q, float out[4]) {
            float ang = std::atan2(q.y - cy, q.x - cx);
            if (ang < 0) ang += 2 * pi;
            const float t = range != 0 ? (ang - s0) / range : 0.0f;
            eval_line(st, t, ext, out);
        });
        break;
    }
    case 10: {   // PaintGlyph: clip the child paint to the glyph outline
        const std::uint16_t gid = be16(d, p + 3);
        s.push_clip_outline(s.ctx.outline_of(gid));
        if (const std::size_t c = child_at(p); c != static_cast<std::size_t>(-1)) paint(s, c, depth + 1);
        s.pop_clip();
        break;
    }
    case 11: {   // PaintColrGlyph: another base glyph's graph, inside its ClipBox
        const std::uint16_t gid = be16(d, p);
        for (const std::uint16_t g : s.glyph_stack) if (g == gid) return;   // cycle
        std::size_t root = 0;
        if (!find_v1_base(s.t, gid, root)) return;
        s.glyph_stack.push_back(gid);
        float x0, y0, x1, y1;
        const bool clipped = clip_box(s.t, gid, x0, y0, x1, y1);
        if (clipped) s.push_clip_rect(x0, y0, x1, y1);
        paint(s, root, depth + 1);
        if (clipped) s.pop_clip();
        s.glyph_stack.pop_back();
        break;
    }
    case 12: case 13: {   // PaintTransform: Affine2x3 of Fixed
        const std::size_t a = off + be24(d, p + 3);
        if (a + 24 > d.size()) return;
        const Affine m{befixed(d, a), befixed(d, a + 4), befixed(d, a + 8),
                       befixed(d, a + 12), befixed(d, a + 16), befixed(d, a + 20)};
        if (const std::size_t c = child_at(p); c != static_cast<std::size_t>(-1)) paint_transformed(s, c, m, depth);
        break;
    }
    case 14: case 15:   // PaintTranslate: FWORD dx, dy
        if (const std::size_t c = child_at(p); c != static_cast<std::size_t>(-1))
            paint_transformed(s, c, Affine{1, 0, 0, 1, static_cast<float>(be16s(d, p + 3)), static_cast<float>(be16s(d, p + 5))}, depth);
        break;
    case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: {   // scales (F2Dot14)
        const bool uniform = fmt >= 20, centred = fmt == 18 || fmt == 19 || fmt == 22 || fmt == 23;
        const float sx = bef2dot14(d, p + 3);
        const float sy = uniform ? sx : bef2dot14(d, p + 5);
        const std::size_t cpos = p + (uniform ? 5 : 7);
        Affine m{sx, 0, 0, sy, 0, 0};
        if (centred) {
            const float cx = be16s(d, cpos), cy = be16s(d, cpos + 2);
            m = Affine{1, 0, 0, 1, cx, cy} * m * Affine{1, 0, 0, 1, -cx, -cy};
        }
        if (const std::size_t c = child_at(p); c != static_cast<std::size_t>(-1)) paint_transformed(s, c, m, depth);
        break;
    }
    case 24: case 25: case 26: case 27: {   // rotations: F2Dot14 half-turns
        const float ang = bef2dot14(d, p + 3) * pi;
        Affine m{std::cos(ang), std::sin(ang), -std::sin(ang), std::cos(ang), 0, 0};
        if (fmt >= 26) {
            const float cx = be16s(d, p + 5), cy = be16s(d, p + 7);
            m = Affine{1, 0, 0, 1, cx, cy} * m * Affine{1, 0, 0, 1, -cx, -cy};
        }
        if (const std::size_t c = child_at(p); c != static_cast<std::size_t>(-1)) paint_transformed(s, c, m, depth);
        break;
    }
    case 28: case 29: case 30: case 31: {   // skews: F2Dot14 half-turns, x negated (push_skew)
        const float kx = std::tan(-bef2dot14(d, p + 3) * pi), ky = std::tan(bef2dot14(d, p + 5) * pi);
        Affine m{1, ky, kx, 1, 0, 0};
        if (fmt >= 30) {
            const float cx = be16s(d, p + 7), cy = be16s(d, p + 9);
            m = Affine{1, 0, 0, 1, cx, cy} * m * Affine{1, 0, 0, 1, -cx, -cy};
        }
        if (const std::size_t c = child_at(p); c != static_cast<std::size_t>(-1)) paint_transformed(s, c, m, depth);
        break;
    }
    case 32: {   // PaintComposite: source composited onto backdrop, in a group
        const std::size_t src = child_at(p), backdrop = child_at(p + 4);
        const unsigned mode = d[p + 3];
        s.groups.emplace_back(s.w, s.h);           // backdrop group
        if (backdrop != static_cast<std::size_t>(-1)) paint(s, backdrop, depth + 1);
        s.groups.emplace_back(s.w, s.h);           // source group
        if (src != static_cast<std::size_t>(-1)) paint(s, src, depth + 1);
        Surface source = std::move(s.groups.back());
        s.groups.pop_back();
        Surface& bd = s.groups.back();
        if (mode <= 27)
            for (std::size_t i = 0; i < source.px.size(); i += 4) composite(mode, &source.px[i], &bd.px[i]);
        Surface result = std::move(s.groups.back());
        s.groups.pop_back();
        // pop_group(SRC_OVER) onto the enclosing surface
        Surface& out = s.surface();
        for (std::size_t i = 0; i < result.px.size(); i += 4) composite(3, &result.px[i], &out.px[i]);
        break;
    }
    default: break;
    }
}

}  // namespace detail

// ---- top-level render ----

/// Render a COLR colour glyph to a straight-RGBA ColorBitmap at `size_px`
/// pixels-per-em. Returns an invalid bitmap when `gid` is not a colour glyph
/// or COLR is unusable, so the caller can fall through to the outline path.
[[nodiscard]] inline ColorBitmap render_glyph(const ColrTable& t, std::uint16_t gid,
                                              float size_px, const RenderContext& ctx) {
    ColorBitmap none;
    if (!t.ok || ctx.upem == 0 || !ctx.outline_of) return none;
    const float scale = size_px / static_cast<float>(ctx.upem);

    // Collect the v0 layers (v1 handled in a follow-up); each is a (gid,
    // palette index) pair. If none, this isn't a v0 colour glyph.
    struct Layer { std::uint16_t gid; std::uint16_t pal; };
    std::vector<Layer> layers;
    std::uint16_t first = 0, count = 0;
    if (detail::v0_layers(t, gid, first, count)) {
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::size_t rec = t.layer_records_off +
                static_cast<std::size_t>(first + i) * 4;
            layers.push_back(Layer{be16(t.data, rec), be16(t.data, rec + 2)});
        }
    }
    if (layers.empty()) {
        // No v0 layers — try the v1 paint graph.
        std::size_t root = 0;
        if (!detail::find_v1_base(t, gid, root)) return none;

        // The base glyph's own ClipBox bounds the canvas and clips the
        // paint (HarfBuzz paint_glyph `clip`); without one, a generous em.
        float cminx = 0, cminy = static_cast<float>(ctx.upem) * -0.30f;
        float cmaxx = static_cast<float>(ctx.upem), cmaxy = static_cast<float>(ctx.upem) * 1.05f;
        const bool clipped = detail::clip_box(t, gid, cminx, cminy, cmaxx, cmaxy);

        const int pad = 2;
        const int x0 = static_cast<int>(std::floor(cminx * scale)) - pad;
        const int y0 = static_cast<int>(std::floor(-cmaxy * scale)) - pad;
        const int x1 = static_cast<int>(std::ceil (cmaxx * scale)) + pad;
        const int y1 = static_cast<int>(std::ceil (-cminy * scale)) + pad;
        const int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return none;

        Placement pl;
        pl.scale = scale;
        pl.ox = static_cast<float>(-x0);
        pl.oy = static_cast<float>(-y0);
        detail::V1 st(t, ctx, w, h, pl);
        st.groups.emplace_back(w, h);
        st.clips.emplace_back(static_cast<std::size_t>(w) * h, 1.0f);
        st.clip_boxes.push_back(detail::V1::Box{0, 0, w, h});
        st.xforms.push_back(Affine::identity());
        if (clipped) st.push_clip_rect(cminx, cminy, cmaxx, cmaxy);
        detail::paint(st, root, 0);
        Canvas cv{w, h};
        cv.px = std::move(st.groups.front().px);

        ColorBitmap cb = cv.to_bitmap(size_px);
        cb.bearing_x = static_cast<float>(x0);
        cb.bearing_y = static_cast<float>(-y0);
        return cb;
    }

    // Union the layer outline bounds (font units, y-up) to size the canvas.
    // Work in raw min/max to avoid Rect's screen-oriented top()/bottom().
    float minx = 0, miny = 0, maxx = 0, maxy = 0;
    bool have = false;
    std::vector<Outline> outs;
    outs.reserve(layers.size());
    for (const auto& L : layers) {
        Outline o = ctx.outline_of(L.gid);
        if (!o.empty()) {
            const Rect b = o.control_bounds();
            const float lx = b.origin.x, ly = b.origin.y;
            const float hx = b.origin.x + b.size.x, hy = b.origin.y + b.size.y;
            if (!have) { minx = lx; miny = ly; maxx = hx; maxy = hy; have = true; }
            else {
                minx = std::min(minx, lx); miny = std::min(miny, ly);
                maxx = std::max(maxx, hx); maxy = std::max(maxy, hy);
            }
        }
        outs.push_back(std::move(o));
    }
    if (!have) return none;

    // Canvas pixel box: font units y-up -> pixels y-down (negate y).
    const int pad = 2;
    const int x0 = static_cast<int>(std::floor(minx * scale)) - pad;
    const int y0 = static_cast<int>(std::floor(-maxy * scale)) - pad;
    const int x1 = static_cast<int>(std::ceil (maxx * scale)) + pad;
    const int y1 = static_cast<int>(std::ceil (-miny * scale)) + pad;
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return none;

    Canvas cv{w, h};
    Placement pl;
    pl.scale = scale;
    pl.ox = static_cast<float>(-x0);   // canvas x of font-unit x=0
    pl.oy = static_cast<float>(-y0);   // canvas y of font-unit y=0

    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (outs[i].empty()) continue;
        const Rgba col = ctx.palette ? ctx.palette->at(layers[i].pal, ctx.text_color)
                                     : ctx.text_color;
        fill_outline(cv, outs[i], pl, [col](float, float) { return col; });
    }

    ColorBitmap cb = cv.to_bitmap(size_px);
    // Bearings: top-left of the canvas relative to the pen/baseline, in px.
    cb.bearing_x = static_cast<float>(x0);
    cb.bearing_y = static_cast<float>(-y0);   // px above baseline
    return cb;
}

}  // namespace colr

}  // namespace mayag::typo
