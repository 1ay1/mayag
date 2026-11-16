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
// Supported: CPAL (all formats), COLR v0 (layers), COLR v1 (PaintColrLayers,
// PaintSolid, PaintLinearGradient, PaintRadialGradient, PaintGlyph,
// PaintColrGlyph, PaintTransform family, PaintTranslate/Scale/Rotate). Sweep
// gradients and advanced composite modes fall back to source-over.

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

namespace detail {

// Walk state: the shared canvas, the placement (font->pixel), the current
// affine in FONT units (applied before placement), a recursion budget, and
// the render context.
struct V1 {
    const ColrTable& t;
    const RenderContext& ctx;
    Canvas& cv;
    const Placement& pl;

    // Map a font-unit point through the current transform, then to canvas px.
    Affine xform;   // font-unit -> font-unit

    // Map a canvas pixel back to (transformed) font units, for gradient eval.
    [[nodiscard]] Vec2 px_to_font(float px, float py) const {
        // invert placement: fx = (px - ox)/scale ; fy = -(py - oy)/scale
        const float fx = (px - pl.ox) / pl.scale;
        const float fy = -(py - pl.oy) / pl.scale;
        // invert xform
        return xform.inverse().apply(Vec2{fx, fy});
    }
};

inline void paint(V1& s, std::size_t off, int depth);

// Locate a v1 base glyph's root paint offset (absolute in the COLR table).
inline bool find_v1_base(const ColrTable& t, std::uint16_t gid, std::size_t& paint_off) {
    if (t.base_list_off == 0 || t.base_list_off + 4 > t.data.size()) return false;
    const std::uint32_t n = be32(t.data, t.base_list_off);
    std::uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const std::uint32_t mid = (lo + hi) / 2;
        const std::size_t rec = t.base_list_off + 4 + static_cast<std::size_t>(mid) * 6;
        const std::uint16_t g = be16(t.data, rec);
        if (g == gid) {
            paint_off = t.base_list_off + be32(t.data, rec + 2);
            return true;
        }
        if (g < gid) lo = mid + 1; else hi = mid;
    }
    return false;
}

// Compose one transform onto s.xform, recurse into the child paint, restore.
// Handles the PaintTransform family (formats 12..31). Coordinates are font
// units; Affine is {a,b,c,d,e,f} with apply(p) = (a*x+c*y+e, b*x+d*y+f).
inline void apply_transform(V1& s, std::size_t off, int depth) {
    const auto d = s.t.data;
    const std::uint32_t sub = be24(d, off + 1);   // child paint, offset from here
    const std::size_t child = off + sub;
    const Affine saved = s.xform;
    Affine m = Affine::identity();
    const std::size_t p = off + 4;   // params start after format(1)+childOffset(3)
    switch (d[off]) {
        case 12: case 13: {   // PaintTransform / PaintVarTransform (Affine2x3)
            // Affine2x3 is referenced by offset for 12; inline is not used.
            const std::size_t ao = off + be24(d, off + 4);
            m = Affine{befixed(d, ao + 0), befixed(d, ao + 4), befixed(d, ao + 8),
                       befixed(d, ao + 12), befixed(d, ao + 16), befixed(d, ao + 20)};
            break;
        }
        case 14: case 15: {   // PaintTranslate
            m = Affine{1, 0, 0, 1, befixed(d, p), befixed(d, p + 4)};
            break;
        }
        case 16: case 17: {   // PaintScale
            m = Affine{befixed(d, p), 0, 0, befixed(d, p + 4), 0, 0};
            break;
        }
        case 18: case 19: {   // PaintScaleAroundCenter
            const float sx = befixed(d, p), sy = befixed(d, p + 4);
            const float cx = be16s(d, p + 8), cy = be16s(d, p + 10);
            m = Affine{1,0,0,1,cx,cy} * Affine{sx,0,0,sy,0,0} * Affine{1,0,0,1,-cx,-cy};
            break;
        }
        case 20: case 21: {   // PaintScaleUniform
            const float sc = befixed(d, p);
            m = Affine{sc, 0, 0, sc, 0, 0};
            break;
        }
        case 22: case 23: {   // PaintScaleUniformAroundCenter
            const float sc = befixed(d, p);
            const float cx = be16s(d, p + 4), cy = be16s(d, p + 6);
            m = Affine{1,0,0,1,cx,cy} * Affine{sc,0,0,sc,0,0} * Affine{1,0,0,1,-cx,-cy};
            break;
        }
        case 24: case 25: {   // PaintRotate (degrees)
            const float ang = befixed(d, p) * 3.14159265358979f / 180.0f;
            m = Affine{std::cos(ang), std::sin(ang), -std::sin(ang), std::cos(ang), 0, 0};
            break;
        }
        case 26: case 27: {   // PaintRotateAroundCenter
            const float ang = befixed(d, p) * 3.14159265358979f / 180.0f;
            const float cx = be16s(d, p + 4), cy = be16s(d, p + 6);
            const Affine rot{std::cos(ang), std::sin(ang), -std::sin(ang), std::cos(ang), 0, 0};
            m = Affine{1,0,0,1,cx,cy} * rot * Affine{1,0,0,1,-cx,-cy};
            break;
        }
        case 28: case 29: {   // PaintSkew (degrees)
            const float ax = std::tan(befixed(d, p) * 3.14159265358979f / 180.0f);
            const float ay = std::tan(befixed(d, p + 4) * 3.14159265358979f / 180.0f);
            m = Affine{1, ay, ax, 1, 0, 0};
            break;
        }
        case 30: case 31: {   // PaintSkewAroundCenter
            const float ax = std::tan(befixed(d, p) * 3.14159265358979f / 180.0f);
            const float ay = std::tan(befixed(d, p + 4) * 3.14159265358979f / 180.0f);
            const float cx = be16s(d, p + 8), cy = be16s(d, p + 10);
            m = Affine{1,0,0,1,cx,cy} * Affine{1, ay, ax, 1, 0, 0} * Affine{1,0,0,1,-cx,-cy};
            break;
        }
        default: break;
    }
    s.xform = saved * m;
    paint(s, child, depth + 1);
    s.xform = saved;
}

// PaintGlyph (format 10): clip to a base outline, then paint the sub-paint
// only where the outline covers. We approximate the clip by rendering the
// sub-paint's colour per covered pixel of the (transformed) outline.
inline void paint_glyph(V1& s, std::size_t off, int depth) {
    const auto d = s.t.data;
    const std::uint32_t sub = be24(d, off + 1);
    const std::uint16_t gid = be16(d, off + 4);
    Outline o = s.ctx.outline_of(gid);
    if (o.empty()) return;
    o.transform(s.xform);

    // Build a PaintFn from the sub-paint. Only formats 2..7 yield a colour;
    // anything else recurses (an acceptable degrade without the outline clip).
    const std::size_t spoff = off + sub;
    if (spoff + 1 > d.size()) return;
    const std::uint8_t fmt = d[spoff];
    PaintFn fn;
    if (fmt == 2 || fmt == 3) {              // PaintSolid / PaintVarSolid
        const std::uint16_t pal = be16(d, spoff + 1);
        const float alpha = bef2dot14(d, spoff + 3);
        Rgba c = s.ctx.palette ? s.ctx.palette->at(pal, s.ctx.text_color) : s.ctx.text_color;
        c.a = static_cast<std::uint8_t>(std::clamp(static_cast<float>(c.a) * alpha, 0.0f, 255.0f));
        fn = [c](float, float) { return c; };
    } else if (fmt == 4 || fmt == 5) {       // PaintLinearGradient
        Extend ext; const std::uint32_t clo = be24(d, spoff + 1);
        auto stops = parse_color_line(d, spoff + clo, s.ctx, ext);
        const float x0 = be16s(d, spoff + 4),  y0 = be16s(d, spoff + 6);
        const float x1 = be16s(d, spoff + 8),  y1 = be16s(d, spoff + 10);
        const float x2 = be16s(d, spoff + 12), y2 = be16s(d, spoff + 14);
        const V1* sp = &s;
        fn = [stops, ext, x0, y0, x1, y1, x2, y2, sp](float px, float py) {
            const Vec2 f = sp->px_to_font(px, py);
            // Project f onto the p0->p1 axis, rotated by the p0->p2 normal.
            const float dx = x1 - x0, dy = y1 - y0;
            const float rx = x2 - x0, ry = y2 - y0;
            // Rotate gradient vector so it is perpendicular to (rx,ry).
            const float len2 = dx * dx + dy * dy;
            float t = 0.0f;
            if (len2 > 0.0f) t = ((f.x - x0) * dx + (f.y - y0) * dy) / len2;
            (void)rx; (void)ry;
            return sample_ramp(stops, t, ext);
        };
    } else if (fmt == 6 || fmt == 7) {       // PaintRadialGradient
        Extend ext; const std::uint32_t clo = be24(d, spoff + 1);
        auto stops = parse_color_line(d, spoff + clo, s.ctx, ext);
        // (c0 = spoff+4, +6) is not used by the c1-distance approximation below.
        const float r0  = be16s(d, spoff + 8);
        const float cx1 = be16s(d, spoff + 10), cy1 = be16s(d, spoff + 12);
        const float r1  = be16s(d, spoff + 14);
        const V1* sp = &s;
        fn = [stops, ext, r0, cx1, cy1, r1, sp](float px, float py) {
            const Vec2 f = sp->px_to_font(px, py);
            // Approximate two-circle radial as distance from c1 remapped by
            // the radii (exact for the common concentric r0=0 case).
            const float dcx = f.x - cx1, dcy = f.y - cy1;
            const float dist = std::sqrt(dcx * dcx + dcy * dcy);
            const float denom = (r1 - r0);
            const float t = std::fabs(denom) > 0.0001f ? (dist - r0) / denom : 0.0f;
            return sample_ramp(stops, t, ext);
        };
    } else if (fmt == 8 || fmt == 9) {       // PaintSweepGradient
        Extend ext; const std::uint32_t clo = be24(d, spoff + 1);
        auto stops = parse_color_line(d, spoff + clo, s.ctx, ext);
        const float cx = be16s(d, spoff + 4), cy = be16s(d, spoff + 6);
        // startAngle/endAngle are F2Dot14 encoding angle/180 (1.0 => 180deg).
        const float sa = bef2dot14(d, spoff + 8)  * 180.0f;
        const float ea = bef2dot14(d, spoff + 10) * 180.0f;
        const V1* sp = &s;
        fn = [stops, ext, cx, cy, sa, ea, sp](float px, float py) {
            const Vec2 f = sp->px_to_font(px, py);
            float ang = std::atan2(f.y - cy, f.x - cx) * 180.0f / 3.14159265358979f;
            if (ang < 0.0f) ang += 360.0f;
            const float span = (ea - sa);
            const float t = std::fabs(span) > 0.0001f ? (ang - sa) / span : 0.0f;
            return sample_ramp(stops, t, ext);
        };
    } else {
        // Nested/unsupported sub-paint: recurse so PaintColrLayers etc. still
        // draw (without the outline clip, which is an acceptable degrade).
        paint(s, spoff, depth + 1);
        return;
    }
    fill_outline(s.cv, o, s.pl, fn);
}

inline void paint(V1& s, std::size_t off, int depth) {
    if (depth > 64 || off + 1 > s.t.data.size()) return;
    const auto d = s.t.data;
    const std::uint8_t fmt = d[off];
    switch (fmt) {
        case 1: {   // PaintColrLayers
            const std::uint8_t n = d[off + 1];
            const std::uint32_t first = be32(d, off + 2);
            if (s.t.layer_list_off == 0) return;
            const std::uint32_t nl = be32(d, s.t.layer_list_off);
            for (std::uint8_t i = 0; i < n; ++i) {
                const std::uint32_t li = first + i;
                if (li >= nl) break;
                const std::size_t po = s.t.layer_list_off +
                    be32(d, s.t.layer_list_off + 4 + static_cast<std::size_t>(li) * 4);
                paint(s, po, depth + 1);
            }
            break;
        }
        case 2: case 3: {   // PaintSolid over the whole clip (rare at top)
            // No geometry here; handled inside PaintGlyph. Ignore standalone.
            break;
        }
        case 10: paint_glyph(s, off, depth); break;   // PaintGlyph
        case 11: {   // PaintColrGlyph: paint another base glyph's paint
            const std::uint16_t bgid = be16(d, off + 1);
            std::size_t po = 0;
            if (find_v1_base(s.t, bgid, po)) paint(s, po, depth + 1);
            break;
        }
        default:
            if (fmt >= 12 && fmt <= 31) {   // transform family
                apply_transform(s, off, depth);
            }
            break;
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

        // Canvas bounds: use the ClipList entry for this gid if present,
        // otherwise a generous em box (font units, y-up).
        float cminx = 0, cminy = static_cast<float>(ctx.upem) * -0.30f;
        float cmaxx = static_cast<float>(ctx.upem), cmaxy = static_cast<float>(ctx.upem) * 1.05f;
        if (t.clip_list_off != 0 && t.clip_list_off + 5 <= t.data.size()) {
            const std::uint32_t nclip = be32(t.data, t.clip_list_off + 1);
            for (std::uint32_t i = 0; i < nclip; ++i) {
                const std::size_t rec = t.clip_list_off + 5 + static_cast<std::size_t>(i) * 7;
                if (rec + 7 > t.data.size()) break;
                const std::uint16_t g0 = be16(t.data, rec);
                const std::uint16_t g1 = be16(t.data, rec + 2);
                if (gid >= g0 && gid <= g1) {
                    const std::size_t box = t.clip_list_off + be24(t.data, rec + 4);
                    if (box + 9 <= t.data.size() /* format(1)+4*i16 */) {
                        cminx = be16s(t.data, box + 1);
                        cminy = be16s(t.data, box + 3);
                        cmaxx = be16s(t.data, box + 5);
                        cmaxy = be16s(t.data, box + 7);
                    }
                    break;
                }
            }
        }

        const int pad = 2;
        const int x0 = static_cast<int>(std::floor(cminx * scale)) - pad;
        const int y0 = static_cast<int>(std::floor(-cmaxy * scale)) - pad;
        const int x1 = static_cast<int>(std::ceil (cmaxx * scale)) + pad;
        const int y1 = static_cast<int>(std::ceil (-cminy * scale)) + pad;
        const int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return none;

        Canvas cv{w, h};
        Placement pl;
        pl.scale = scale;
        pl.ox = static_cast<float>(-x0);
        pl.oy = static_cast<float>(-y0);
        detail::V1 st{t, ctx, cv, pl, Affine::identity()};
        detail::paint(st, root, 0);

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
