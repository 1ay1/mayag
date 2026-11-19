#pragma once
// mayag::render::shaders — LEGACY hand-written shader text (Metal only)
//
// ⚠ DO NOT ADD TO THIS FILE, AND DO NOT "KEEP IT IN SYNC" WITH ANYTHING.
//
// This header used to hold four hand-maintained shader dialects beside the C++
// kernel, with a comment asking whoever edited one to remember to edit the
// others. That convention failed repeatedly and silently — the GPU antialiased
// glyphs differently from the CPU, encoded sRGB at a different point in the
// pipeline (making the hardware blend run in gamma space), and never had the
// stem-darkening correction at all, so GPU text could not match the reference
// at any tolerance. None of it was visible to the test suite.
//
// Rendering is now centralised in render/kernel.hpp, and every backend's shader
// is MECHANICALLY TRANSPILED from it by tools/gen_shaders.py as a build step
// (output: render/shader_generated.hpp). The Vulkan backend consumes that.
//
// What survives here is the Metal (MSL) text, which has not been moved onto the
// generator yet because this project cannot build or test Metal on Linux and
// shipping an untested rewrite of a backend's shading would reintroduce exactly
// the class of bug the kernel was built to end. Porting Metal to the generator
// is the remaining step; the scaffolding in gen_shaders.py already anticipates
// it (`translate(..., "msl")`).
//
// Until then: to change how something LOOKS, edit render/kernel.hpp. If the
// change must also reach Metal before that port lands, mirror it here AND say
// so in the commit — this file is a known, documented exception, not a pattern.

#include <string_view>

namespace mayag::render::shaders {

/// The shape kernel, in GLSL 4.50 / Vulkan flavour.
/// Mirrors: sdf::rounded_box, sdf::ring, sdf::segment, sdf::arc,
///          sdf::coverage_smooth, sdf::shadow_coverage.
// Only the Metal (MSL) text remains. The GLSL/WGSL/HLSL that used to sit
// here is gone: Vulkan now consumes render/shader_generated.hpp, produced
// from render/kernel.hpp, and no other backend is wired up. Deleting them
// removes three more copies of the shading model that could drift.

inline constexpr std::string_view msl_kernel = R"MSL(
#include <metal_stdlib>
using namespace metal;

inline float mg_rounded_box(float2 p, float2 b, float4 radii) {
    float r = (p.x > 0.0) ? ((p.y > 0.0) ? radii.z : radii.y)
                          : ((p.y > 0.0) ? radii.w : radii.x);
    r = min(r, min(b.x, b.y));
    float2 q = abs(p) - b + float2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0))) - r;
}

inline float mg_ring(float2 p, float r, float t) {
    return abs(length(p) - r) - t * 0.5;
}

inline float mg_segment(float2 p, float2 a, float2 b, float thickness) {
    float2 pa = p - a, ba = b - a;
    float  h  = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6f), 0.0f, 1.0f);
    return length(pa - ba * h) - thickness * 0.5;
}

inline float mg_outline(float d, float w) { return abs(d) - w * 0.5; }

inline float mg_coverage(float d, float px) {
    float w = max(px, 1e-6f) * 0.5f;
    return smoothstep(w, -w, d);
}

// Text coverage composited as if in gamma space: kernel::apply_text_gamma,
// verbatim. The Metal path is the one every macOS window draws with, and it
// went without this correction (so on-screen text was lighter and bolder than
// the CPU reference) until test_metal compared text pixel by pixel.
inline float mg_srgb_to_linear(float c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
inline float mg_linear_to_srgb(float c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
inline float mg_box_halfplane(float d, float nx, float ny) {
    float a = min(abs(nx), abs(ny));
    float b = max(max(abs(nx), abs(ny)), 1e-6);
    float t = d + 0.5 * (a + b);
    if (t <= 0.0) return 0.0;
    if (t >= a + b) return 1.0;
    if (a < 1e-4) return clamp(t / b, 0.0, 1.0);
    if (t < a) return t * t / (2.0 * a * b);
    if (t <= b) return (t - 0.5 * a) / b;
    float u = a + b - t;
    return 1.0 - u * u / (2.0 * a * b);
}
// kernel::sdf_glyph_coverage, verbatim.
inline float mg_sdf_glyph(float raw, float rx, float ry, float4 params) {
    float band = params.x > 0.0 ? params.x : 0.09;
    float per_px = 2.0 * band;
    float gx = rx - raw, gy = ry - raw;
    float gl = sqrt(gx * gx + gy * gy);
    if (gl > 1e-6) { gx /= gl; gy /= gl; } else { gx = 0.0; gy = 1.0; }
    float d = (raw - 0.5) / per_px + (params.y / per_px) * (abs(gx) + abs(gy));
    return mg_box_halfplane(d, gx, gy);
}
inline float mg_text_gamma(float cov, float4 premul) {
    if (cov <= 0.0 || cov >= 1.0) return cov;
    float a = premul.w > 1e-4 ? premul.w : 1.0;
    float lum = clamp((0.2126 * premul.x + 0.7152 * premul.y + 0.0722 * premul.z) / a, 0.0, 1.0);
    float enc = mg_linear_to_srgb(lum);
    float on_black = lum > 1e-4 ? mg_srgb_to_linear(cov * enc) / lum : cov;
    float on_white = lum < 0.9999
        ? (1.0 - mg_srgb_to_linear(1.0 - cov * (1.0 - enc))) / (1.0 - lum) : cov;
    return clamp(mix(on_white, on_black, smoothstep(0.35, 0.65, enc)), 0.0, 1.0);
}
// kernel::apply_text_gamma(cov, color, backdrop), verbatim.
inline float mg_text_gamma_on(float cov, float4 premul, float4 backdrop) {
    if (backdrop.w < 0.5) return mg_text_gamma(cov, premul);
    if (cov <= 0.0 || cov >= 1.0) return cov;
    float a = premul.w > 1e-4 ? premul.w : 1.0;
    float s = clamp((0.2126 * premul.x + 0.7152 * premul.y + 0.0722 * premul.z) / a, 0.0, 1.0);
    float d = clamp(0.2126 * backdrop.x + 0.7152 * backdrop.y + 0.0722 * backdrop.z, 0.0, 1.0);
    if (abs(s - d) < 0.02) return mg_text_gamma(cov, premul);
    float es = mg_linear_to_srgb(s), ed = mg_linear_to_srgb(d);
    float want = mg_srgb_to_linear(ed + (es - ed) * cov);
    return clamp((want - d) / (s - d), 0.0, 1.0);
}

inline float mg_shadow(float d, float blur) {
    if (blur <= 0.0f) return d <= 0.0f ? 1.0f : 0.0f;
    return smoothstep(blur, -blur, d);
}

// Perceptual gradient interpolation, mirroring Software::lerp_oklch.
// POLAR, not Cartesian: a straight Oklab line between opposite hues passes
// near the neutral axis and desaturates the midpoint almost as badly as RGB.
inline float3 mg_linear_to_oklab(float3 c) {
    float l = 0.4122214708*c.r + 0.5363325363*c.g + 0.0514459929*c.b;
    float m = 0.2119034982*c.r + 0.6806995451*c.g + 0.1073969566*c.b;
    float s = 0.0883024619*c.r + 0.2817188376*c.g + 0.6299787005*c.b;
    float3 v = pow(max(float3(l, m, s), float3(0.0)), float3(1.0/3.0));
    return float3(0.2104542553*v.x + 0.7936177850*v.y - 0.0040720468*v.z,
                  1.9779984951*v.x - 2.4285922050*v.y + 0.4505937099*v.z,
                  0.0259040371*v.x + 0.7827717662*v.y - 0.8086757660*v.z);
}

inline float3 mg_oklab_to_linear(float3 lab) {
    float l_ = lab.x + 0.3963377774*lab.y + 0.2158037573*lab.z;
    float m_ = lab.x - 0.1055613458*lab.y - 0.0638541728*lab.z;
    float s_ = lab.x - 0.0894841775*lab.y - 1.2914855480*lab.z;
    float3 c = float3(l_*l_*l_, m_*m_*m_, s_*s_*s_);
    return max(float3( 4.0767416621*c.x - 3.3077115913*c.y + 0.2309699292*c.z,
                      -1.2684380046*c.x + 2.6097574011*c.y - 0.3413193965*c.z,
                      -0.0041960863*c.x - 0.7034186147*c.y + 1.7076147010*c.z), float3(0.0));
}

inline float4 mg_mix_oklch(float4 a, float4 b, float t) {
    float3 ca = a.a > 1e-6 ? a.rgb / a.a : float3(0.0);
    float3 cb = b.a > 1e-6 ? b.rgb / b.a : float3(0.0);
    float3 la = mg_linear_to_oklab(ca);
    float3 lb = mg_linear_to_oklab(cb);

    float ch_a = length(la.yz), ch_b = length(lb.yz);
    float ha = atan2(la.z, la.y), hb = atan2(lb.z, lb.y);

    float dh = hb - ha;
    if (dh >  3.14159265) dh -= 6.28318531;
    if (dh < -3.14159265) dh += 6.28318531;

    float L   = mix(la.x, lb.x, t);
    float ch  = mix(ch_a, ch_b, t);
    float hue = ha + dh * t;
    float alpha = mix(a.a, b.a, t);

    float3 lin = mg_oklab_to_linear(float3(L, ch * cos(hue), ch * sin(hue)));
    return float4(lin * alpha, alpha);
}
)MSL";

/// The Metal vertex and fragment stages.
///
/// Concatenated after `msl_kernel`, which supplies the SDF functions these
/// call — the same functions `render/sdf.hpp` implements in C++, so the GPU
/// and software paths cannot diverge without the shared source changing.
inline constexpr std::string_view msl_shaders = R"MSL(

struct Instance {
    float4 rect;
    float4 radii;
    float4 color;
    float4 color2;
    float4 axis;
    float4 uv;
    float4 params;
    uint   kind;
    uint   flags;
    uint   blend;
    uint   texture_slot;
};

struct Varyings {
    float4 position [[position]];
    float2 local;
    float2 norm;
    float2 half_size;
    float2 uv;
    float4 color;
    float4 color2;
    float4 radii;
    float4 axis;
    float4 params;
    uint   kind  [[flat]];
    uint   flags [[flat]];
};

// One quad per instance, generated from the vertex id alone: no vertex
// buffer, no index buffer, nothing to bind but the instance array.
vertex Varyings mayag_vertex(uint vid [[vertex_id]],
                             uint iid [[instance_id]],
                             const device Instance* instances [[buffer(0)]],
                             constant float2& viewport [[buffer(1)]]) {
    const device Instance& I = instances[iid];

    float2 corner = float2((vid << 1) & 2, vid & 2) * 0.5;

    // Shadows need the quad grown so the blur is not clipped at the edge.
    float pad = (I.kind == 6u && (I.flags & 32u) == 0u) ? I.params.x * 3.0 + 2.0 : 1.0;

    float2 half_size = I.rect.zw * 0.5 + float2(pad);
    float2 centre    = I.rect.xy + I.rect.zw * 0.5;
    float2 pos       = centre + (corner * 2.0 - 1.0) * half_size;

    Varyings v;
    v.local     = pos - centre;
    v.norm      = (pos - I.rect.xy) / max(I.rect.zw, float2(1e-6));
    v.half_size = I.rect.zw * 0.5;
    // uv is measured against the UNPADDED rect, like norm. Interpolating the
    // glyph's corner uvs across the grown quad stretched every glyph by
    // `pad` px per side: Metal text was blurred and drifted ~0.4 px from the
    // CPU's, invisible to the ink-count check, caught comparing pixels. The
    // Vulkan template has had this fix since text sat a row high there.
    v.uv        = mix(I.uv.xy, I.uv.zw, v.norm);
    v.color     = I.color;
    v.color2    = I.color2;
    v.radii     = I.radii;
    v.axis      = I.axis;
    v.params    = I.params;
    v.kind      = I.kind;
    v.flags     = I.flags;

    // Clip space, y down to match mayag's screen convention.
    float2 ndc = pos / viewport * 2.0 - 1.0;
    v.position = float4(ndc.x, -ndc.y, 0.0, 1.0);
    return v;
}

fragment float4 mayag_fragment(Varyings v [[stage_in]],
                               texture2d<float> atlas [[texture(0)]],
                               texture2d<float> color_atlas [[texture(1)]],
                               texture2d<float> image [[texture(2)]],
                               sampler smp [[sampler(0)]]
#if MG_FB_FETCH
                               , float4 dst [[color(0)]]
#endif
                               ) {
    constexpr uint KIND_ROUNDED_BOX = 0u, KIND_RING = 2u, KIND_LINE = 3u;
    constexpr uint KIND_GLYPH = 4u, KIND_TEXTURE = 5u, KIND_SHADOW = 6u, KIND_ARC = 7u;
    constexpr uint KIND_COLOR_GLYPH = 8u;
    constexpr uint FLAG_GRADIENT = 1u, FLAG_RADIAL = 2u, FLAG_ANGULAR = 4u;
    constexpr uint FLAG_SRGB = 8u, FLAG_STROKE = 16u, FLAG_INSET = 32u, FLAG_GLYPH_SDF = 128u;

    // Colour glyphs (emoji): straight RGBA from the colour plane, tinted only
    // by opacity (kernel::shade's first case, verbatim). Without this case
    // they fell through to the rectangle path and drew as white boxes.
    if (v.kind == KIND_COLOR_GLYPH) {
        float4 t = color_atlas.sample(smp, v.uv);
        float a = t.a * v.color.a;
        if (a <= 0.001) discard_fragment();
        return float4(t.rgb * a, a);
    }

    // Pixel footprint from screen-space derivatives — what makes the
    // antialiasing correct under any transform or zoom.
    float px = max(length(fwidth(v.local)), 1e-6);

    float cov;
    if (v.kind == KIND_ROUNDED_BOX || v.kind == KIND_TEXTURE) {
        float d = mg_rounded_box(v.local, v.half_size, v.radii);
        if ((v.flags & FLAG_STROKE) != 0u) d = mg_outline(d + v.params.x * 0.5, v.params.x);
        cov = mg_coverage(d, px);
    } else if (v.kind == KIND_RING) {
        cov = mg_coverage(mg_ring(v.local, min(v.half_size.x, v.half_size.y), v.params.x), px);
    } else if (v.kind == KIND_LINE) {
        cov = mg_coverage(mg_segment(v.local + (v.axis.xy + v.axis.zw) * 0.5,
                                     v.axis.xy, v.axis.zw, v.params.x), px);
    } else if (v.kind == KIND_SHADOW) {
        if ((v.flags & FLAG_INSET) != 0u) {
            float clip = mg_coverage(mg_rounded_box(v.local, v.half_size, v.radii), px);
            float2 inner = max(v.half_size - v.params.y, float2(0.0));
            float d = mg_rounded_box(v.local - v.params.zw, inner, v.radii);
            cov = (1.0 - mg_shadow(d, max(v.params.x, 0.01))) * clip;
        } else {
            float d = mg_rounded_box(v.local, v.half_size, v.radii);
            cov = mg_shadow(d, max(v.params.x, 0.01));
        }
    } else if (v.kind == KIND_GLYPH) {
        float raw = atlas.sample(smp, v.uv).r;
        // A bitmap entry IS coverage; an SDF entry becomes exact box
        // coverage from the field and its gradient one display pixel away
        // (params.zw = uv step of a display pixel), as kernel.hpp does.
        if ((v.flags & FLAG_GLYPH_SDF) != 0u) {
            float rx = atlas.sample(smp, v.uv + float2(v.params.z, 0.0)).r;
            float ry = atlas.sample(smp, v.uv + float2(0.0, v.params.w)).r;
            cov = mg_sdf_glyph(raw, rx, ry, v.params);
        } else {
            cov = raw;
        }
#if MG_FB_FETCH
        // CoreGraphics' text blend, exactly (Framebuffer::blend_text): per
        // channel a lerp in ENCODED values, enc(dst) + (enc(src) - enc(dst))
        // * cov * alpha. The sRGB target hands us linear dst and encodes what
        // we return; over an opaque destination we return the result with
        // alpha 1, which the (1, 1-srcA) blend stores verbatim.
        if (dst.a >= 0.999 && v.color.a > 0.0) {
            if (cov <= 0.001) discard_fragment();
            float t = clamp(cov * v.color.a, 0.0, 1.0);
            float3 s = v.color.rgb / v.color.a;
            float3 r;
            for (int i = 0; i < 3; ++i) {
                float ed = mg_linear_to_srgb(clamp(dst[i], 0.0, 1.0));
                float es = mg_linear_to_srgb(clamp(s[i], 0.0, 1.0));
                r[i] = mg_srgb_to_linear(ed + (es - ed) * t);
            }
            return float4(r, 1.0);
        }
#endif
        cov = mg_text_gamma_on(cov, v.color, v.color2);
    } else {
        cov = mg_coverage(mg_rounded_box(v.local, v.half_size, v.radii), px);
    }

    if (cov <= 0.001) discard_fragment();

    float4 color = v.color;
    if ((v.flags & FLAG_GRADIENT) != 0u) {
        float t;
        if ((v.flags & FLAG_RADIAL) != 0u) {
            t = clamp(length(v.norm - v.axis.xy) / max(v.params.z, 1e-4), 0.0, 1.0);
        } else if ((v.flags & FLAG_ANGULAR) != 0u) {
            float2 d = v.norm - v.axis.xy;
            t = clamp((atan2(d.y, d.x) + 3.14159265) / 6.28318531, 0.0, 1.0);
        } else {
            float2 ab = v.axis.zw - v.axis.xy;
            t = clamp(dot(v.norm - v.axis.xy, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        }
        color = ((v.flags & FLAG_SRGB) != 0u)
              ? mix(v.color, v.color2, t)
              : mg_mix_oklch(v.color, v.color2, t);
    }

    // A user image (the batch's own texture, an _SRGB format: the hardware
    // decodes to linear), modulated by the node's colour as kernel.hpp does.
    if (v.kind == KIND_TEXTURE) color *= image.sample(smp, v.uv);

    return color * cov;
}
)MSL";

/// WebGPU Shading Language kernel — same again, WGSL spelling.
}  // namespace mayag::render::shaders
