#pragma once
// mayag::kernel — THE shading kernel. One implementation, every backend.
//
// ─────────────────────────────────────────────────────────────────────────
//  WHY THIS FILE EXISTS
//
//  mayag used to describe its shape maths in five places: sdf.hpp (C++),
//  and four hand-written shader dialects in shader_source.hpp. The comment
//  above them asked a human to keep them in sync "so a change to one is an
//  obvious diff against the other". That is not a single source of truth,
//  it is a convention — and it failed silently and repeatedly:
//
//    * the GPU antialiased distance-field glyphs with fwidth() while the CPU
//      used the band the glyph emitter computed  -> haloed text;
//    * the GLSL sRGB-encoded in the shader (forcing a UNORM target, so the
//      hardware blend ran in GAMMA space) while the MSL did not -> two GPU
//      backends that disagreed with each other AND with the CPU at every
//      antialiased edge;
//    * the clear colour was linear on one path and sRGB on the other, hidden
//      because the only test that covered it cleared to black;
//    * stem darkening (a real perceptual correction) existed ONLY on the CPU,
//      so GPU text could never match at any tolerance.
//
//  Every one of those is the same bug: rendering was not centralised. Adding
//  a backend meant re-deriving the pixel program, and re-deriving it meant
//  getting it subtly wrong.
//
// ─────────────────────────────────────────────────────────────────────────
//  THE RULE
//
//  This file is the ONLY place a mayag pixel is decided. The software
//  rasteriser #includes it and calls shade() directly. Every GPU backend runs
//  shader text MECHANICALLY TRANSPILED from this file by tools/gen_shaders.py
//  at build time (see render/shader_generated.hpp). No dialect is written by
//  hand, so no dialect can drift. A new backend is a new *transport* — buffers,
//  descriptors, a swapchain — and contains no shading code whatsoever.
//
//  Consequently: if you change how anything LOOKS, you change it here, once,
//  and every backend inherits it in the same build.
//
// ─────────────────────────────────────────────────────────────────────────
//  THE SUBSET (what the transpiler accepts)
//
//  To stay mechanically translatable this file uses a deliberately small
//  language. Keep to it:
//
//    types      f32, u32, vec2, vec4  (aliases below). No pointers, no
//               references except `const Env&`, no templates, no classes with
//               behaviour, no dynamic allocation, no recursion.
//    control    if / else if / else, for with constant bounds, early return.
//    calls      only the functions in this file and the math builtins listed
//               in `mg` below (each maps 1:1 onto a GLSL/MSL/WGSL builtin).
//    state      none. Every function is pure: output depends only on inputs.
//
//  All colour is LINEAR and PREMULTIPLIED, end to end. The kernel never
//  applies a transfer function: the software path composites into a linear
//  framebuffer and encodes once at the end, and the GPU path renders into an
//  _SRGB attachment so the hardware decodes-blends-encodes. Both therefore
//  blend in linear light, which is the only correct way to blend coverage.
// ─────────────────────────────────────────────────────────────────────────

#include "../core/geometry.hpp"
#include "../core/math.hpp"

#include <cstdint>

namespace mayag::kernel {

// ── the subset's type vocabulary ────────────────────────────────────────
using f32  = float;
using u32  = std::uint32_t;
using vec2 = Vec2;
using vec4 = Vec4;

// ── math builtins ───────────────────────────────────────────────────────
// Each of these maps onto exactly one builtin in every target dialect, so the
// transpiler rewrites `mg::foo(` -> `foo(` (or the dialect's spelling) with no
// cleverness. They are thin wrappers on mayag::num so the C++ build is exact.
namespace mg {
[[nodiscard]] constexpr f32 min(f32 a, f32 b) noexcept { return a < b ? a : b; }
[[nodiscard]] constexpr f32 max(f32 a, f32 b) noexcept { return a > b ? a : b; }
[[nodiscard]] constexpr f32 abs(f32 a) noexcept { return a < 0.0f ? -a : a; }
[[nodiscard]] constexpr f32 clamp(f32 x, f32 lo, f32 hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}
[[nodiscard]] constexpr f32 mix(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }
[[nodiscard]] constexpr f32 step(f32 edge, f32 x) noexcept { return x < edge ? 0.0f : 1.0f; }
[[nodiscard]] constexpr f32 dot2(vec2 a, vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] inline f32 length2(vec2 v) noexcept { return num::sqrt(v.x * v.x + v.y * v.y); }
[[nodiscard]] inline f32 sqrt(f32 x) noexcept { return num::sqrt(x); }
[[nodiscard]] inline f32 pow(f32 x, f32 y) noexcept { return num::pow(x, y); }
[[nodiscard]] inline f32 exp(f32 x) noexcept { return num::exp(x); }
[[nodiscard]] inline f32 cos(f32 x) noexcept { return num::cos(x); }
[[nodiscard]] inline f32 sin(f32 x) noexcept { return num::sin(x); }
[[nodiscard]] inline f32 atan2(f32 y, f32 x) noexcept { return num::atan2(y, x); }
[[nodiscard]] inline f32 mod(f32 x, f32 y) noexcept {
    const f32 r = x - y * num::floor(x / y);
    return r;
}
/// Hermite smoothstep. Note the argument order matches GLSL: edge0, edge1, x.
[[nodiscard]] constexpr f32 smoothstep(f32 e0, f32 e1, f32 x) noexcept {
    const f32 t = clamp((x - e0) / ((e1 - e0) != 0.0f ? (e1 - e0) : 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace mg

// ── constants ───────────────────────────────────────────────────────────
inline constexpr f32 k_pi     = 3.14159265f;
inline constexpr f32 k_two_pi = 6.28318531f;

// ShapeKind, mirrored as plain constants so the transpiler emits them as-is.
inline constexpr u32 KIND_ROUNDED_BOX = 0u;
inline constexpr u32 KIND_CIRCLE      = 1u;
inline constexpr u32 KIND_RING        = 2u;
inline constexpr u32 KIND_LINE        = 3u;
inline constexpr u32 KIND_GLYPH       = 4u;
inline constexpr u32 KIND_TEXTURE     = 5u;
inline constexpr u32 KIND_SHADOW      = 6u;
inline constexpr u32 KIND_ARC         = 7u;
inline constexpr u32 KIND_COLOR_GLYPH = 8u;
inline constexpr u32 KIND_BACKDROP    = 9u;

inline constexpr u32 FLAG_GRADIENT  = 1u;
inline constexpr u32 FLAG_RADIAL    = 2u;
inline constexpr u32 FLAG_ANGULAR   = 4u;
inline constexpr u32 FLAG_SRGB      = 8u;
inline constexpr u32 FLAG_STROKE    = 16u;
inline constexpr u32 FLAG_INSET     = 32u;
inline constexpr u32 FLAG_DASHED    = 64u;
inline constexpr u32 FLAG_GLYPH_SDF = 128u;

// ── signed distance fields ──────────────────────────────────────────────
// Negative inside, zero on the boundary, positive outside, unit gradient.
// That last property is why `coverage()` is a correct analytic antialias at
// any scale, and why mayag needs no MSAA and no supersampling.

/// Rounded box with per-corner radii (tl, tr, br, bl). `p` is relative to the
/// box CENTRE, `b` is the half-size. Inigo Quilez's formulation.
[[nodiscard]] constexpr f32 rounded_box(vec2 p, vec2 b, vec4 radii) noexcept {
    f32 r = (p.x > 0.0f) ? ((p.y > 0.0f) ? radii.z : radii.y)
                         : ((p.y > 0.0f) ? radii.w : radii.x);
    r = mg::min(r, mg::min(b.x, b.y));
    const vec2 q = vec2{mg::abs(p.x) - b.x + r, mg::abs(p.y) - b.y + r};
    const vec2 qp = vec2{mg::max(q.x, 0.0f), mg::max(q.y, 0.0f)};
    return mg::min(mg::max(q.x, q.y), 0.0f) + mg::length2(qp) - r;
}

/// Circle of radius `r` centred at the origin.
[[nodiscard]] inline f32 circle(vec2 p, f32 r) noexcept { return mg::length2(p) - r; }

/// Annulus: a ring of radius `r` and thickness `t`.
[[nodiscard]] inline f32 ring(vec2 p, f32 r, f32 t) noexcept {
    return mg::abs(mg::length2(p) - r) - t * 0.5f;
}

/// Capsule between `a` and `b` with the given thickness.
[[nodiscard]] inline f32 segment(vec2 p, vec2 a, vec2 b, f32 thickness) noexcept {
    const vec2 pa = vec2{p.x - a.x, p.y - a.y};
    const vec2 ba = vec2{b.x - a.x, b.y - a.y};
    const f32  h  = mg::clamp(mg::dot2(pa, ba) / mg::max(mg::dot2(ba, ba), 1e-6f), 0.0f, 1.0f);
    return mg::length2(vec2{pa.x - ba.x * h, pa.y - ba.y * h}) - thickness * 0.5f;
}

/// Partial ring. `start`/`sweep` are radians; a full sweep degenerates to ring.
[[nodiscard]] inline f32 arc(vec2 p, f32 r, f32 thickness, f32 start, f32 sweep) noexcept {
    const f32 ring_d = ring(p, r, thickness);
    if (sweep >= k_two_pi) return ring_d;
    const f32 ang = mg::mod(mg::atan2(p.y, p.x) - start, k_two_pi);
    if (ang <= sweep) return ring_d;
    const vec2 e0 = vec2{r * mg::cos(start), r * mg::sin(start)};
    const vec2 e1 = vec2{r * mg::cos(start + sweep), r * mg::sin(start + sweep)};
    const f32  d0 = mg::length2(vec2{p.x - e0.x, p.y - e0.y});
    const f32  d1 = mg::length2(vec2{p.x - e1.x, p.y - e1.y});
    return mg::min(d0, d1) - thickness * 0.5f;
}

/// Hollow out a field: turns a filled shape into a stroke of width `w`.
[[nodiscard]] constexpr f32 outline(f32 d, f32 w) noexcept { return mg::abs(d) - w * 0.5f; }

/// Analytic coverage of a distance field over a pixel footprint `px`.
[[nodiscard]] constexpr f32 coverage(f32 d, f32 px) noexcept {
    const f32 w = mg::max(px, 1e-6f) * 0.5f;
    return mg::smoothstep(w, -w, d);
}

/// A blurred box is the same field, softened — no blur pass, no downsample.
[[nodiscard]] constexpr f32 shadow_coverage(f32 d, f32 blur) noexcept {
    if (blur <= 0.0f) return d <= 0.0f ? 1.0f : 0.0f;
    return mg::smoothstep(blur, -blur, d);
}

// ── perceptual colour ───────────────────────────────────────────────────
// Oklab/Oklch (Björn Ottosson). Gradients interpolate in POLAR Oklch: a
// straight Cartesian line between opposite hues passes near the neutral axis
// and desaturates the midpoint almost as badly as an RGB lerp.

[[nodiscard]] inline vec4 linear_to_oklab(vec4 c) noexcept {
    const f32 l = 0.4122214708f * c.x + 0.5363325363f * c.y + 0.0514459929f * c.z;
    const f32 m = 0.2119034982f * c.x + 0.6806995451f * c.y + 0.1073969566f * c.z;
    const f32 s = 0.0883024619f * c.x + 0.2817188376f * c.y + 0.6299787005f * c.z;
    const f32 l_ = mg::pow(mg::max(l, 0.0f), 1.0f / 3.0f);
    const f32 m_ = mg::pow(mg::max(m, 0.0f), 1.0f / 3.0f);
    const f32 s_ = mg::pow(mg::max(s, 0.0f), 1.0f / 3.0f);
    return vec4{0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
                0.0f};
}

[[nodiscard]] inline vec4 oklab_to_linear(vec4 lab) noexcept {
    const f32 l_ = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
    const f32 m_ = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
    const f32 s_ = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
    const f32 l = l_ * l_ * l_;
    const f32 m = m_ * m_ * m_;
    const f32 s = s_ * s_ * s_;
    return vec4{mg::max(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s, 0.0f),
                mg::max(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s, 0.0f),
                mg::max(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s, 0.0f),
                0.0f};
}

/// Interpolate two LINEAR PREMULTIPLIED colours through Oklch.
[[nodiscard]] inline vec4 mix_oklch(vec4 a, vec4 b, f32 t) noexcept {
    // Un-premultiply; interpolating premultiplied chroma is meaningless.
    const f32 aa = a.w > 1e-6f ? a.w : 1.0f;
    const f32 ab = b.w > 1e-6f ? b.w : 1.0f;
    const vec4 ca = a.w > 1e-6f ? vec4{a.x / aa, a.y / aa, a.z / aa, 0.0f} : vec4{0, 0, 0, 0};
    const vec4 cb = b.w > 1e-6f ? vec4{b.x / ab, b.y / ab, b.z / ab, 0.0f} : vec4{0, 0, 0, 0};

    const vec4 la = linear_to_oklab(ca);
    const vec4 lb = linear_to_oklab(cb);

    const f32 chroma_a = mg::length2(vec2{la.y, la.z});
    const f32 chroma_b = mg::length2(vec2{lb.y, lb.z});
    const f32 ha = mg::atan2(la.z, la.y);
    const f32 hb = mg::atan2(lb.z, lb.y);

    // Shortest way around the hue circle.
    f32 dh = hb - ha;
    if (dh > k_pi) dh = dh - k_two_pi;
    if (dh < -k_pi) dh = dh + k_two_pi;

    const f32 L      = mg::mix(la.x, lb.x, t);
    const f32 chroma = mg::mix(chroma_a, chroma_b, t);
    const f32 hue    = ha + dh * t;
    const f32 alpha  = mg::mix(a.w, b.w, t);

    const vec4 lin = oklab_to_linear(
        vec4{L, chroma * mg::cos(hue), chroma * mg::sin(hue), 0.0f});
    return vec4{lin.x * alpha, lin.y * alpha, lin.z * alpha, alpha};   // re-premultiply
}

// ── text ────────────────────────────────────────────────────────────────
//
// Stem darkening. Coverage-antialiased glyphs look too THIN when light text
// sits on a dark background, because the eye integrates a light stroke on
// dark more generously than the linear coverage says. FreeType calls this
// stem darkening; Apple and Skia apply a comparable gamma. Applied to
// COVERAGE, not colour, so it can never shift hue.
//
// This lives in the kernel — not in one backend — precisely because it is a
// LOOK decision. It used to exist only in the software rasteriser, which is
// why GPU text could never match the reference at any tolerance.
inline constexpr f32 text_gamma_light = 0.80f;
inline constexpr f32 text_gamma_dark  = 1.05f;

[[nodiscard]] inline f32 apply_text_gamma(f32 cov, vec4 premul_color) noexcept {
    if (cov <= 0.0f) return cov;
    if (cov >= 1.0f) return cov;
    const f32 a = premul_color.w > 1e-4f ? premul_color.w : 1.0f;
    const f32 lum = (0.2126f * premul_color.x +
                     0.7152f * premul_color.y +
                     0.0722f * premul_color.z) / a;
    // Blend between the two exponents by how light the text is, so a mid-grey
    // label does not jump discontinuously between regimes.
    const f32 t = mg::clamp(lum * 2.0f, 0.0f, 1.0f);
    const f32 gamma = mg::mix(text_gamma_dark, text_gamma_light, t);
    return mg::pow(cov, gamma);
}

/// Fallback half-width of the SDF threshold band, in normalised field units,
/// used when the glyph emitter supplied none. It must be at least half the
/// field's change across ONE texel or the smoothstep lands entirely between
/// samples and every edge snaps hard. At the default 48 px raster and 6 px
/// spread one texel step changes the field by ~0.082, so this gives a ~2-texel
/// ramp: a clean edge without the blur a wider band would introduce.
inline constexpr f32 sdf_edge_fallback = 0.09f;

// ── the fragment program ────────────────────────────────────────────────

/// Everything the kernel needs about one fragment. The backends fill this in:
/// the software rasteriser per pixel from the Instance, the GPU from
/// interpolated varyings. Same fields, same meanings, same order.
struct Fragment {
    vec2 local{};    ///< position relative to the shape centre
    vec2 norm{};     ///< 0..1 inside the shape (for gradients)
    vec2 frag{};     ///< absolute pixel coordinate (lines, backdrop)
    vec2 half{};     ///< half-size of the shape
    vec2 uv{};       ///< atlas uv for glyph/texture kinds
    vec4 color{};    ///< primary colour, linear premultiplied
    vec4 color2{};   ///< secondary colour (gradient end)
    vec4 radii{};    ///< corner radii
    vec4 axis{};     ///< gradient axis / line endpoints
    vec4 params{};   ///< per-kind parameters
    f32  px = 1.0f;  ///< pixel footprint (derivative-based on GPU)
    u32  kind = 0;
    u32  flags = 0;
};

/// The result of shading one fragment: a LINEAR PREMULTIPLIED colour plus a
/// discard flag. Returning a struct (rather than discarding inline) keeps the
/// kernel free of dialect-specific control flow.
struct Shaded {
    vec4 color{};
    bool discard = false;
};

/// Texture access, abstracted so the identical kernel runs on both paths.
/// The software rasteriser implements these against its CoverageSampler; the
/// transpiler rewrites each call into the dialect's texture fetch.
struct Env {
    /// Coverage atlas (R8): glyph coverage or distance field.
    f32 (*atlas_r)(const void* ctx, vec2 uv) = nullptr;
    /// Colour atlas (RGBA, straight): emoji.
    vec4 (*atlas_rgba)(const void* ctx, vec2 uv) = nullptr;
    /// Mid-frame capture of everything drawn so far (RGBA, linear premul).
    vec4 (*capture)(const void* ctx, vec2 uv) = nullptr;
    /// Size of the capture target in pixels, for texel-sized offsets.
    vec2 capture_size{1.0f, 1.0f};
    const void* ctx = nullptr;
};

/// Coverage for the geometric kinds. Split out so both the full `shade()` and
/// the software rasteriser's fast paths can share it verbatim.
[[nodiscard]] inline f32 shape_coverage(const Fragment& f) noexcept {
    if (f.kind == KIND_ROUNDED_BOX || f.kind == KIND_TEXTURE) {
        f32 d = rounded_box(f.local, f.half, f.radii);
        if ((f.flags & FLAG_STROKE) != 0u) d = outline(d + f.params.x * 0.5f, f.params.x);
        return coverage(d, f.px);
    }
    if (f.kind == KIND_CIRCLE) {
        return coverage(circle(f.local, mg::min(f.half.x, f.half.y)), f.px);
    }
    if (f.kind == KIND_RING) {
        return coverage(ring(f.local, mg::min(f.half.x, f.half.y), f.params.x), f.px);
    }
    if (f.kind == KIND_ARC) {
        return coverage(arc(f.local, mg::min(f.half.x, f.half.y),
                            f.params.x, f.params.y, f.params.z), f.px);
    }
    if (f.kind == KIND_LINE) {
        return coverage(segment(f.frag, vec2{f.axis.x, f.axis.y},
                                vec2{f.axis.z, f.axis.w}, f.params.x), f.px);
    }
    if (f.kind == KIND_SHADOW) {
        const f32 d = rounded_box(f.local, f.half, f.radii);
        const f32 c = shadow_coverage(d, mg::max(f.params.x, 0.01f));
        return (f.flags & FLAG_INSET) != 0u ? 1.0f - c : c;
    }
    return coverage(rounded_box(f.local, f.half, f.radii), f.px);
}

/// Resolve the fill colour, applying any gradient. LINEAR PREMULTIPLIED in,
/// LINEAR PREMULTIPLIED out.
[[nodiscard]] inline vec4 fill_color(const Fragment& f) noexcept {
    if ((f.flags & FLAG_GRADIENT) == 0u) return f.color;

    f32 t = 0.0f;
    if ((f.flags & FLAG_RADIAL) != 0u) {
        const vec2 d = vec2{f.norm.x - f.axis.x, f.norm.y - f.axis.y};
        t = mg::clamp(mg::length2(d) / mg::max(f.params.z, 1e-4f), 0.0f, 1.0f);
    } else if ((f.flags & FLAG_ANGULAR) != 0u) {
        const vec2 d = vec2{f.norm.x - f.axis.x, f.norm.y - f.axis.y};
        t = mg::clamp((mg::atan2(d.y, d.x) + k_pi) / k_two_pi, 0.0f, 1.0f);
    } else {
        const vec2 ab = vec2{f.axis.z - f.axis.x, f.axis.w - f.axis.y};
        const vec2 pa = vec2{f.norm.x - f.axis.x, f.norm.y - f.axis.y};
        t = mg::clamp(mg::dot2(pa, ab) / mg::max(mg::dot2(ab, ab), 1e-6f), 0.0f, 1.0f);
    }

    // Perceptual (Oklch) by default; FLAG_SRGB opts into the naive ramp for
    // designs authored against CSS.
    if ((f.flags & FLAG_SRGB) != 0u) {
        return vec4{mg::mix(f.color.x, f.color2.x, t), mg::mix(f.color.y, f.color2.y, t),
                    mg::mix(f.color.z, f.color2.z, t), mg::mix(f.color.w, f.color2.w, t)};
    }
    return mix_oklch(f.color, f.color2, t);
}

/// Shade one fragment. THE function — every backend's inner loop ends here.
[[nodiscard]] inline Shaded shade(const Fragment& f, const Env& env) noexcept {
    Shaded result;

    // ── colour glyphs (emoji): straight RGBA, tinted only by opacity ──
    if (f.kind == KIND_COLOR_GLYPH) {
        const vec4 t = env.atlas_rgba(env.ctx, f.uv);
        const f32  a = t.w * f.color.w;
        if (a <= 0.001f) { result.discard = true; return result; }
        result.color = vec4{t.x * a, t.y * a, t.z * a, a};
        return result;
    }

    // ── frosted glass: blur the mid-frame capture behind this shape ──
    if (f.kind == KIND_BACKDROP) {
        const f32 bd_cov = coverage(rounded_box(f.local, f.half, f.radii), f.px);
        if (bd_cov <= 0.001f) { result.discard = true; return result; }
        const f32 r = mg::max(f.params.x, 0.0f);
        vec4 sum{0, 0, 0, 0};
        f32  wsum = 0.0f;
        for (int oy = -2; oy <= 2; ++oy) {
            for (int ox = -2; ox <= 2; ++ox) {
                const f32 fx = static_cast<f32>(ox) * 0.5f;
                const f32 fy = static_cast<f32>(oy) * 0.5f;
                const f32 w  = mg::exp(-(fx * fx + fy * fy) * 2.0f);
                const vec2 uv = vec2{
                    (f.frag.x + static_cast<f32>(ox) * r * 0.5f) / env.capture_size.x,
                    (f.frag.y + static_cast<f32>(oy) * r * 0.5f) / env.capture_size.y};
                const vec4 s = env.capture(env.ctx, uv);
                sum.x += s.x * w; sum.y += s.y * w; sum.z += s.z * w;
                wsum += w;
            }
        }
        const f32 inv = 1.0f / mg::max(wsum, 1e-4f);
        vec4 c = vec4{sum.x * inv, sum.y * inv, sum.z * inv, 0.0f};
        const f32 sat = f.params.y, bright = f.params.z;
        const f32 luma = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
        c = vec4{mg::max(mg::mix(luma, c.x, sat) * bright, 0.0f),
                 mg::max(mg::mix(luma, c.y, sat) * bright, 0.0f),
                 mg::max(mg::mix(luma, c.z, sat) * bright, 0.0f), 0.0f};
        result.color = vec4{c.x * bd_cov, c.y * bd_cov, c.z * bd_cov, bd_cov};
        return result;
    }

    // ── coverage ────────────────────────────────────────────────────
    f32 cov;
    if (f.kind == KIND_GLYPH) {
        const f32 raw = env.atlas_r(env.ctx, f.uv);
        if ((f.flags & FLAG_GLYPH_SDF) != 0u) {
            // Distance-field entry: 0.5 is the edge. The half-band comes from
            // the glyph emitter (params.x), which derives it from the SDF
            // spread and the raster->display scale. Using a derivative here
            // instead would over-estimate the ramp on a magnified, bilinearly
            // sampled field and bloom every glyph into a grey halo.
            const f32 band = f.params.x > 0.0f ? f.params.x : sdf_edge_fallback;
            cov = mg::smoothstep(0.5f - band, 0.5f + band, raw);
        } else {
            cov = raw;   // bitmap entry: already coverage
        }
        cov = apply_text_gamma(cov, f.color);
    } else {
        cov = shape_coverage(f);
    }

    if (cov <= 0.001f) { result.discard = true; return result; }

    vec4 color = fill_color(f);

    if (f.kind == KIND_TEXTURE) {
        const vec4 t = env.atlas_rgba(env.ctx, f.uv);
        color = vec4{color.x * t.x, color.y * t.y, color.z * t.z, color.w * t.w};
    }

    // LINEAR PREMULTIPLIED. No transfer function here, ever: the software path
    // composites into a linear framebuffer and encodes once at the end, and the
    // GPU path renders into an _SRGB attachment so the hardware decodes, blends
    // in linear, and encodes on store. Both blend coverage in linear light.
    result.color = vec4{color.x * cov, color.y * cov, color.z * cov, color.w * cov};
    return result;
}

}  // namespace mayag::kernel
