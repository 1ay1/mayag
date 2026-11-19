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
[[nodiscard]] inline f32 log2(f32 x) noexcept { return num::log(x) * 1.4426950409f; }
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

// ── instance reach ──────────────────────────────────────────────────────────
// How far past its rect an instance can paint, in px: a 1 px antialiasing
// skirt, or a drop shadow's blur falloff. The GPU vertex shader grows its
// quad by exactly this (transpiled), and the CPU rasteriser, the tile binner
// and the GPU pass planner bound the instance with it, so no copy can drift.
[[nodiscard]] inline f32 quad_pad(u32 kind, u32 flags, f32 param0) noexcept {
    if (kind == KIND_SHADOW && (flags & FLAG_INSET) == 0u) return param0 * 3.0f + 2.0f;
    return 1.0f;
}

/// How far past its rect a backdrop READS (blur taps at +-r plus the mip
/// footprint they sample): the region captured before it draws.
[[nodiscard]] inline f32 backdrop_reach(f32 radius) noexcept {
    return 1.5f * mg::max(radius, 0.0f) + 4.0f;
}

// ── signed distance fields ──────────────────────────────────────────────
// Negative inside, zero on the boundary, positive outside, unit gradient.
// That last property is why `coverage()` is a correct analytic antialias at
// any scale, and why mayag needs no MSAA and no supersampling.

/// Rounded box with per-corner radii (tl, tr, br, bl). `p` is relative to the
/// box CENTRE, `b` is the half-size. Inigo Quilez's formulation.
[[nodiscard]] inline f32 rounded_box(vec2 p, vec2 b, vec4 radii) noexcept {
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

/// sRGB -> linear, the exact piecewise transfer.
///
/// The kernel never ENCODES (the framebuffer does that), but it must DECODE:
/// a user image arrives sRGB-encoded, and multiplying it into a linear
/// premultiplied colour without decoding first is a gamma error that shows up
/// as a tinted image being too bright.
[[nodiscard]] inline f32 srgb_to_linear(f32 c) noexcept {
    if (c <= 0.04045f) return c / 12.92f;
    return mg::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// linear -> sRGB, the exact piecewise transfer. Only text coverage needs
/// it: see apply_text_gamma.
[[nodiscard]] inline f32 linear_to_srgb(f32 c) noexcept {
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * mg::pow(c, 1.0f / 2.4f) - 0.055f;
}

// ── text ────────────────────────────────────────────────────────────────────────
//
// Glyph coverage is composited as if in GAMMA space, the way CoreText,
// DirectWrite and Skia composite text, while everything else blends in
// linear light.
//
// Linear blending is right for shapes and wrong for text. Fonts are drawn,
// hinted and tuned on renderers that blend antialiased edges in sRGB-encoded
// space, so their stems were designed for that weight. Blended in linear
// light, a partially covered pixel of light text on dark comes out much
// brighter, and of dark text on light much paler, so light-on-dark text
// reads bold and blobby and dark-on-light reads thin and washed out: the
// same font changes weight with the theme.
//
// The blend happens in fixed-function hardware against a backdrop the shader
// cannot see, so the encoded-space result is reproduced by reshaping the
// coverage against the backdrop the text is designed against (black under
// light text, white under dark), exactly:
//
//   light on black:  c' = dec(c * enc(L)) / L
//   dark on white:   c' = (1 - dec(1 - c * (1 - enc(L)))) / (1 - L)
//
// with L the text's linear luminance. Mid-grey text cross-fades between the
// two. Against the real backdrop the result is within a few percent of true
// gamma-space blending for any reasonably contrasting pair, and it is a
// function of coverage only, so it cannot shift hue.
//
// This lives in the kernel, not in one backend, because it is a LOOK
// decision; it used to exist only in the software rasteriser, which is why
// GPU text could never match the reference at any tolerance.
[[nodiscard]] inline f32 apply_text_gamma(f32 cov, vec4 premul_color) noexcept {
    if (cov <= 0.0f) return cov;
    if (cov >= 1.0f) return cov;
    const f32 a = premul_color.w > 1e-4f ? premul_color.w : 1.0f;
    const f32 lum = mg::clamp((0.2126f * premul_color.x +
                               0.7152f * premul_color.y +
                               0.0722f * premul_color.z) / a, 0.0f, 1.0f);
    const f32 enc = linear_to_srgb(lum);
    const f32 on_black = lum > 1e-4f ? srgb_to_linear(cov * enc) / lum : cov;
    const f32 on_white = lum < 0.9999f
        ? (1.0f - srgb_to_linear(1.0f - cov * (1.0f - enc))) / (1.0f - lum) : cov;
    const f32 t = mg::smoothstep(0.35f, 0.65f, enc);
    return mg::clamp(mg::mix(on_white, on_black, t), 0.0f, 1.0f);
}

/// CoreGraphics' text blend, exactly, as the SOURCE of a fixed-function
/// `src + dst * (1 - srcA)` blend into an sRGB-ENCODED target.
///
/// CoreGraphics draws a glyph into an sRGB context as a per-channel lerp of
/// ENCODED values, enc(dst) + (enc(src) - enc(dst)) * cov * alpha. With
/// t = cov * alpha that is enc(src) * t + enc(dst) * (1 - t): premultiplied
/// "over", in encoded space. So a backend that lets the blender see the
/// target's encoded bytes (a UNORM view of the sRGB image, Vulkan) gets the
/// exact result from this source and the ordinary blend state; no coverage
/// reshaping, no knowledge of the backdrop. The software rasteriser applies
/// the identical formula itself (Framebuffer::blend_text), so both paths
/// draw the same text pixels by construction.
[[nodiscard]] inline vec4 encoded_text_source(f32 cov, vec4 premul_color) noexcept {
    const f32 a = premul_color.w;
    if (a <= 1e-6f) return vec4{0.0f, 0.0f, 0.0f, 0.0f};
    const f32 t = mg::clamp(cov * a, 0.0f, 1.0f);
    return vec4{linear_to_srgb(mg::clamp(premul_color.x / a, 0.0f, 1.0f)) * t,
                linear_to_srgb(mg::clamp(premul_color.y / a, 0.0f, 1.0f)) * t,
                linear_to_srgb(mg::clamp(premul_color.z / a, 0.0f, 1.0f)) * t,
                t};
}

/// The same reshaping against the ACTUAL backdrop, when the painter knows
/// it (`backdrop` = the opaque linear colour the text sits on, w = 1; w = 0
/// means unknown and falls back to the black/white design backdrops). For a
/// luminance pair (S text, D backdrop) the encoded-space blend gives
/// dec(enc(D) + (enc(S) - enc(D)) c); the linear hardware blend gives
/// D + (S - D) c', so c' = (that - D) / (S - D), exact for that pair. Text
/// on a dark grey (0x15) panel was off by 2.6/255 with the design backdrop.
/// The fallback for GPUs that cannot blend in encoded space
/// (encoded_text_source); exact per luminance, not per channel.
[[nodiscard]] inline f32 apply_text_gamma(f32 cov, vec4 premul_color, vec4 backdrop) noexcept {
    if (backdrop.w < 0.5f) return apply_text_gamma(cov, premul_color);
    if (cov <= 0.0f || cov >= 1.0f) return cov;
    const f32 a = premul_color.w > 1e-4f ? premul_color.w : 1.0f;
    const f32 s = mg::clamp((0.2126f * premul_color.x + 0.7152f * premul_color.y + 0.0722f * premul_color.z) / a,
                            0.0f, 1.0f);
    const f32 d = mg::clamp(0.2126f * backdrop.x + 0.7152f * backdrop.y + 0.0722f * backdrop.z, 0.0f, 1.0f);
    if (mg::abs(s - d) < 0.02f) return apply_text_gamma(cov, premul_color);   // no contrast to solve for
    const f32 es = linear_to_srgb(s), ed = linear_to_srgb(d);
    const f32 want = srgb_to_linear(ed + (es - ed) * cov);
    return mg::clamp((want - d) / (s - d), 0.0f, 1.0f);
}

/// Fallback half-width of the SDF threshold band, in normalised field units,
/// used when the glyph emitter supplied none. It must be at least half the
/// field's change across ONE texel or the smoothstep lands entirely between
/// samples and every edge snaps hard. At the default 48 px raster and 6 px
/// spread one texel step changes the field by ~0.082, so this gives a ~2-texel
/// ramp: a clean edge without the blur a wider band would introduce.
inline constexpr f32 sdf_edge_fallback = 0.09f;

/// Exact box-filtered coverage of a 1x1 pixel by the half-plane whose
/// boundary lies `d` px from the pixel centre (positive inside) with unit
/// normal (nx, ny): the area, piecewise quadratic/linear in d. This is what
/// an exact-area rasteriser (CoreGraphics, mayag's bitmap path) produces for
/// a straight edge; a smoothstep or a linear ramp is off by up to 0.125 at
/// 45 degrees.
[[nodiscard]] inline f32 box_halfplane_coverage(f32 d, f32 nx, f32 ny) noexcept {
    const f32 a = mg::min(mg::abs(nx), mg::abs(ny));
    const f32 b = mg::max(mg::max(mg::abs(nx), mg::abs(ny)), 1e-6f);
    const f32 t = d + 0.5f * (a + b);   // distance from the box's outermost corner
    if (t <= 0.0f) return 0.0f;
    if (t >= a + b) return 1.0f;
    if (a < 1e-4f) return mg::clamp(t / b, 0.0f, 1.0f);
    if (t < a) return t * t / (2.0f * a * b);
    if (t <= b) return (t - 0.5f * a) / b;
    const f32 u = a + b - t;
    return 1.0f - u * u / (2.0f * a * b);
}

/// Coverage of a distance-field glyph texel. `raw` is the field at the pixel
/// centre, `rx`/`ry` the field one display pixel right/down (the gradient
/// gives the edge normal). params.x = half a display pixel in field units
/// (the field is Euclidean, so distance in px = (raw - 0.5) / (2 params.x)),
/// params.y = font-smoothing growth in field units: CoreGraphics dilates
/// with a SQUARE kernel, so an edge with normal n moves g (|nx| + |ny|).
[[nodiscard]] inline f32 sdf_glyph_coverage(f32 raw, f32 rx, f32 ry, vec4 params) noexcept {
    const f32 band = params.x > 0.0f ? params.x : sdf_edge_fallback;
    const f32 per_px = 2.0f * band;
    f32 gx = rx - raw, gy = ry - raw;
    const f32 gl = mg::sqrt(gx * gx + gy * gy);
    if (gl > 1e-6f) { gx = gx / gl; gy = gy / gl; } else { gx = 0.0f; gy = 1.0f; }
    const f32 grow_px = params.y / per_px;
    const f32 d = (raw - 0.5f) / per_px + grow_px * (mg::abs(gx) + mg::abs(gy));
    return box_halfplane_coverage(d, gx, gy);
}

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
    /// The same capture at a mip level (box-filtered), for wide blurs.
    vec4 (*capture_lod)(const void* ctx, vec2 uv, f32 lod) = nullptr;
    /// A user image (RGBA, straight, sRGB-encoded): photos, icons, video
    /// frames. Distinct from the colour atlas, which the font engine owns.
    vec4 (*texture)(const void* ctx, vec2 uv) = nullptr;
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
        if ((f.flags & FLAG_INSET) != 0u) {
            // An INNER shadow lives inside the shape: darkness where the
            // shadow's box -- offset by (params.z, params.w) and shrunk by
            // the spread -- has NOT reached, clipped to the shape itself.
            // (It used to be 1 - drop shadow over the whole grown quad,
            // which painted a dark slab AROUND every inner-shadowed box.)
            const f32 clip = coverage(rounded_box(f.local, f.half, f.radii), f.px);
            const vec2 off{f.params.z, f.params.w};
            const vec2 inner{mg::max(f.half.x - f.params.y, 0.0f), mg::max(f.half.y - f.params.y, 0.0f)};
            const f32 d = rounded_box(f.local - off, inner, f.radii);
            return (1.0f - shadow_coverage(d, mg::max(f.params.x, 0.01f))) * clip;
        }
        const f32 d = rounded_box(f.local, f.half, f.radii);
        return shadow_coverage(d, mg::max(f.params.x, 0.01f));
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

/// Coverage of one glyph fragment, before any blending decision.
[[nodiscard]] inline f32 glyph_coverage(const Fragment& f, const Env& env) noexcept {
    // A bitmap entry IS coverage. A distance-field entry becomes exact box
    // coverage from the field value and its gradient (sdf_glyph_coverage);
    // params.zw = the uv step of one display pixel, so the two extra taps
    // sit one pixel away whatever the scale.
    const f32 raw = env.atlas_r(env.ctx, f.uv);
    if ((f.flags & FLAG_GLYPH_SDF) != 0u) {
        const f32 rx = env.atlas_r(env.ctx, vec2{f.uv.x + f.params.z, f.uv.y});
        const f32 ry = env.atlas_r(env.ctx, vec2{f.uv.x, f.uv.y + f.params.w});
        return sdf_glyph_coverage(raw, rx, ry, f.params);
    }
    return raw;
}

/// Shade one fragment. THE function — every backend's inner loop ends here.
[[nodiscard]] inline Shaded shade(const Fragment& f, const Env& env) noexcept {
    Shaded result;

    // ── colour glyphs (emoji): straight sRGB RGBA, tinted only by opacity ──
    if (f.kind == KIND_COLOR_GLYPH) {
        const vec4 t = env.atlas_rgba(env.ctx, f.uv);
        const f32  a = t.w * f.color.w;
        if (a <= 0.001f) { result.discard = true; return result; }
        // Decode like any image: blending the encoded values washes emoji out.
        result.color = vec4{srgb_to_linear(t.x) * a, srgb_to_linear(t.y) * a, srgb_to_linear(t.z) * a, a};
        return result;
    }

    // ── frosted glass: blur the mid-frame capture behind this shape ──
    if (f.kind == KIND_BACKDROP) {
        const f32 bd_cov = coverage(rounded_box(f.local, f.half, f.radii), f.px);
        if (bd_cov <= 0.001f) { result.discard = true; return result; }
        const f32 r = mg::max(f.params.x, 0.0f);
        // Taps sit r/2 apart. Read them from the mip whose texels are about
        // that wide, so each tap is an average, not one sharp pixel.
        const f32 lod = mg::log2(mg::max(r * 0.5f, 1.0f));
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
                const vec4 s = env.capture_lod(env.ctx, uv, lod);
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
        // Linear-light blending: the coverage is reshaped to approximate
        // CoreGraphics' encoded-space blend, exactly against the backdrop
        // when the painter knows it (color2, w = 1). Only a backend that
        // cannot blend glyphs in encoded space lands here; Vulkan with a
        // UNORM view uses encoded_text_source instead.
        cov = apply_text_gamma(glyph_coverage(f, env), f.color, f.color2);
    } else {
        cov = shape_coverage(f);
    }

    if (cov <= 0.001f) { result.discard = true; return result; }

    vec4 color = fill_color(f);

    if (f.kind == KIND_TEXTURE) {
        // A user image, modulated by the node's colour so `image(id) | fg(c)`
        // tints and `| opacity(a)` fades. The sample is straight (not
        // premultiplied) sRGB-encoded RGBA; the shader's own colour is linear
        // premultiplied, so decode before multiplying or a tinted image comes
        // out with the wrong gamma.
        const vec4 t = env.texture(env.ctx, f.uv);
        const vec4 lin = vec4{srgb_to_linear(t.x), srgb_to_linear(t.y),
                              srgb_to_linear(t.z), t.w};
        color = vec4{color.x * lin.x, color.y * lin.y,
                     color.z * lin.z, color.w * lin.w};
    }

    // LINEAR PREMULTIPLIED. No transfer function here, ever: the software path
    // composites into a linear framebuffer and encodes once at the end, and the
    // GPU path renders into an _SRGB attachment so the hardware decodes, blends
    // in linear, and encodes on store. Both blend coverage in linear light.
    result.color = vec4{color.x * cov, color.y * cov, color.z * cov, color.w * cov};
    return result;
}

}  // namespace mayag::kernel
