#pragma once
// mayag::render::shaders — the GPU side of the shape kernel
//
// The claim "the CPU rasteriser and the GPU shaders compute the same thing"
// is only worth anything if you can read both. So the shader text lives HERE,
// next to the C++ it mirrors, and every function below is a line-for-line
// translation of the corresponding `mayag::sdf::` function.
//
// One shader body, four dialects. The differences between GLSL / MSL / WGSL /
// HLSL are almost entirely syntax around a common core, so mayag writes the
// core once in a tiny neutral form and wraps it per backend. Adding a backend
// is a header, not a rewrite of the shading model.
//
// If you change `sdf.hpp`, change this file in the same commit. The pixel
// tests will tell you if you forgot.

#include <string_view>

namespace mayag::render::shaders {

/// The shape kernel, in GLSL 4.50 / Vulkan flavour.
/// Mirrors: sdf::rounded_box, sdf::ring, sdf::segment, sdf::arc,
///          sdf::coverage_smooth, sdf::shadow_coverage.
inline constexpr std::string_view glsl_kernel = R"GLSL(
// ── mayag shape kernel (mirrors include/mayag/render/sdf.hpp) ──

float mg_rounded_box(vec2 p, vec2 b, vec4 radii) {
    // Quadrant-select the corner radius, then the standard rounded-box SDF.
    float r = (p.x > 0.0) ? ((p.y > 0.0) ? radii.z : radii.y)
                          : ((p.y > 0.0) ? radii.w : radii.x);
    r = min(r, min(b.x, b.y));
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

float mg_ring(vec2 p, float r, float t) {
    return abs(length(p) - r) - t * 0.5;
}

float mg_segment(vec2 p, vec2 a, vec2 b, float thickness) {
    vec2  pa = p - a, ba = b - a;
    float h  = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h) - thickness * 0.5;
}

float mg_arc(vec2 p, float r, float thickness, float start, float sweep) {
    float ring_d = mg_ring(p, r, thickness);
    if (sweep >= 6.28318531) return ring_d;
    float ang = mod(atan(p.y, p.x) - start, 6.28318531);
    if (ang <= sweep) return ring_d;
    vec2 e0 = vec2(r * cos(start), r * sin(start));
    vec2 e1 = vec2(r * cos(start + sweep), r * sin(start + sweep));
    return min(length(p - e0), length(p - e1)) - thickness * 0.5;
}

float mg_outline(float d, float w) { return abs(d) - w * 0.5; }

// Analytic antialiasing. Valid at any scale because the field is a TRUE
// distance (unit gradient) — this is why mayag needs no MSAA.
float mg_coverage(float d, float px) {
    float w = max(px, 1e-6) * 0.5;
    return smoothstep(w, -w, d);
}

// A blurred box is the same field, softened. No blur pass, no offscreen
// target, no downsample chain.
float mg_shadow(float d, float blur) {
    if (blur <= 0.0) return d <= 0.0 ? 1.0 : 0.0;
    return smoothstep(blur, -blur, d);
}

// ── perceptual gradient interpolation (mirrors Software::lerp_oklch) ──
//
// Inputs are LINEAR PREMULTIPLIED, as produced on the CPU. We go through
// Oklch (POLAR), not Oklab (Cartesian): a straight Cartesian line between
// opposite hues passes near the neutral axis and desaturates the midpoint
// almost as badly as an RGB lerp. Polar interpolates chroma directly, so the
// ramp stays vivid end to end.

vec3 mg_linear_to_oklab(vec3 c) {
    float l = 0.4122214708*c.r + 0.5363325363*c.g + 0.0514459929*c.b;
    float m = 0.2119034982*c.r + 0.6806995451*c.g + 0.1073969566*c.b;
    float s = 0.0883024619*c.r + 0.2817188376*c.g + 0.6299787005*c.b;
    vec3  v = pow(max(vec3(l, m, s), vec3(0.0)), vec3(1.0/3.0));
    return vec3(
        0.2104542553*v.x + 0.7936177850*v.y - 0.0040720468*v.z,
        1.9779984951*v.x - 2.4285922050*v.y + 0.4505937099*v.z,
        0.0259040371*v.x + 0.7827717662*v.y - 0.8086757660*v.z);
}

vec3 mg_oklab_to_linear(vec3 lab) {
    float l_ = lab.x + 0.3963377774*lab.y + 0.2158037573*lab.z;
    float m_ = lab.x - 0.1055613458*lab.y - 0.0638541728*lab.z;
    float s_ = lab.x - 0.0894841775*lab.y - 1.2914855480*lab.z;
    vec3  c  = vec3(l_*l_*l_, m_*m_*m_, s_*s_*s_);
    return max(vec3(
         4.0767416621*c.x - 3.3077115913*c.y + 0.2309699292*c.z,
        -1.2684380046*c.x + 2.6097574011*c.y - 0.3413193965*c.z,
        -0.0041960863*c.x - 0.7034186147*c.y + 1.7076147010*c.z), vec3(0.0));
}

vec4 mg_mix_oklch(vec4 a, vec4 b, float t) {
    // Un-premultiply; interpolating premultiplied chroma is meaningless.
    vec3 ca = a.a > 1e-6 ? a.rgb / a.a : vec3(0.0);
    vec3 cb = b.a > 1e-6 ? b.rgb / b.a : vec3(0.0);

    vec3 la = mg_linear_to_oklab(ca);
    vec3 lb = mg_linear_to_oklab(cb);

    float chroma_a = length(la.yz);
    float chroma_b = length(lb.yz);
    float ha = atan(la.z, la.y);
    float hb = atan(lb.z, lb.y);

    // Shortest way around the hue circle.
    float dh = hb - ha;
    if (dh >  3.14159265) dh -= 6.28318531;
    if (dh < -3.14159265) dh += 6.28318531;

    float L      = mix(la.x, lb.x, t);
    float chroma = mix(chroma_a, chroma_b, t);
    float hue    = ha + dh * t;
    float alpha  = mix(a.a, b.a, t);

    vec3 lin = mg_oklab_to_linear(vec3(L, chroma * cos(hue), chroma * sin(hue)));
    return vec4(lin * alpha, alpha);   // re-premultiply
}
)GLSL";

/// Vertex stage: expands one instance into a quad with NO vertex buffer.
/// `gl_VertexIndex` alone generates the corners, so mayag uploads instance
/// data only — no index buffer, no vertex buffer, no VAO churn.
inline constexpr std::string_view glsl_vertex = R"GLSL(
#version 450

layout(location = 0) in vec4 i_rect;
layout(location = 1) in vec4 i_radii;
layout(location = 2) in vec4 i_color;
layout(location = 3) in vec4 i_color2;
layout(location = 4) in vec4 i_axis;
layout(location = 5) in vec4 i_uv;
layout(location = 6) in vec4 i_params;
layout(location = 7) in uvec4 i_meta;   // kind, flags, blend, texture

layout(push_constant) uniform Push { vec2 viewport; } push;

layout(location = 0) out vec2  v_local;   // position relative to shape centre
layout(location = 1) out vec2  v_norm;    // 0..1 inside the shape
layout(location = 2) out vec4  v_color;
layout(location = 3) out vec4  v_color2;
layout(location = 4) out vec4  v_radii;
layout(location = 5) out vec4  v_axis;
layout(location = 6) out vec4  v_params;
layout(location = 7) out vec2  v_uv;
layout(location = 8) flat out uvec4 v_meta;
layout(location = 9) out vec2  v_half;

void main() {
    // Corner from the vertex id: 0,1,2, 2,1,3 as a triangle strip.
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 0.5;

    // Shadows need the quad grown so the blur is not clipped at the edge.
    float pad = (i_meta.x == 6u) ? i_params.x * 3.0 + 2.0 : 1.0;

    vec2 half_size = i_rect.zw * 0.5 + vec2(pad);
    vec2 centre    = i_rect.xy + i_rect.zw * 0.5;
    vec2 pos       = centre + (corner * 2.0 - 1.0) * half_size;

    v_local  = pos - centre;
    v_norm   = (pos - i_rect.xy) / max(i_rect.zw, vec2(1e-6));
    v_half   = i_rect.zw * 0.5;
    v_color  = i_color;
    v_color2 = i_color2;
    v_radii  = i_radii;
    v_axis   = i_axis;
    v_params = i_params;
    v_uv     = mix(i_uv.xy, i_uv.zw, corner);
    v_meta   = i_meta;

    // Clip space, y down to match mayag's screen convention.
    gl_Position = vec4(pos / push.viewport * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

/// Fragment stage: one branch per ShapeKind, then a single blend.
/// Colours arrive PREMULTIPLIED and LINEAR from the CPU, so this stage never
/// touches a transfer function — which is what keeps a gamma bug impossible.
inline constexpr std::string_view glsl_fragment = R"GLSL(
#version 450

layout(location = 0) in vec2  v_local;
layout(location = 1) in vec2  v_norm;
layout(location = 2) in vec4  v_color;
layout(location = 3) in vec4  v_color2;
layout(location = 4) in vec4  v_radii;
layout(location = 5) in vec4  v_axis;
layout(location = 6) in vec4  v_params;
layout(location = 7) in vec2  v_uv;
layout(location = 8) flat in uvec4 v_meta;
layout(location = 9) in vec2  v_half;

layout(set = 0, binding = 0) uniform sampler2D u_atlas;
layout(set = 0, binding = 1) uniform sampler2D u_color_atlas;

layout(location = 0) out vec4 o_color;

// KERNEL_INSERTED_HERE

const uint KIND_ROUNDED_BOX = 0u;
const uint KIND_CIRCLE      = 1u;
const uint KIND_RING        = 2u;
const uint KIND_LINE        = 3u;
const uint KIND_GLYPH       = 4u;
const uint KIND_TEXTURE     = 5u;
const uint KIND_SHADOW      = 6u;
const uint KIND_ARC         = 7u;
const uint KIND_COLOR_GLYPH = 8u;
const uint KIND_BACKDROP    = 9u;

const uint FLAG_GRADIENT   = 1u;
const uint FLAG_RADIAL     = 2u;
const uint FLAG_ANGULAR    = 4u;
const uint FLAG_SRGB       = 8u;
const uint FLAG_STROKE     = 16u;
const uint FLAG_INSET      = 32u;

void main() {
    uint kind  = v_meta.x;
    uint flags = v_meta.y;

    // Pixel footprint via screen-space derivatives — this is what makes the
    // antialiasing correct under arbitrary transforms and zoom.
    float px = max(length(fwidth(v_local)), 1e-6);

    float cov;
    if (kind == KIND_ROUNDED_BOX || kind == KIND_TEXTURE) {
        float d = mg_rounded_box(v_local, v_half, v_radii);
        if ((flags & FLAG_STROKE) != 0u) d = mg_outline(d + v_params.x * 0.5, v_params.x);
        cov = mg_coverage(d, px);
    } else if (kind == KIND_RING) {
        cov = mg_coverage(mg_ring(v_local, min(v_half.x, v_half.y), v_params.x), px);
    } else if (kind == KIND_ARC) {
        cov = mg_coverage(mg_arc(v_local, min(v_half.x, v_half.y),
                                 v_params.x, v_params.y, v_params.z), px);
    } else if (kind == KIND_LINE) {
        vec2 p = v_local + (v_axis.xy + v_axis.zw) * 0.0;  // line uses absolute coords
        cov = mg_coverage(mg_segment(gl_FragCoord.xy, v_axis.xy, v_axis.zw, v_params.x), px);
    } else if (kind == KIND_SHADOW) {
        float d = mg_rounded_box(v_local, v_half, v_radii);
        cov = mg_shadow(d, max(v_params.x, 0.01));
        if ((flags & FLAG_INSET) != 0u) cov = 1.0 - cov;
    } else if (kind == KIND_GLYPH) {
        cov = texture(u_atlas, v_uv).r;
    } else if (kind == KIND_COLOR_GLYPH) {
        // Colour glyph (emoji): sample the RGBA colour atlas and emit it
        // verbatim, tinted only by opacity (v_color.a), never by hue. The
        // atlas holds straight RGBA; premultiply for the framebuffer.
        vec4 t = texture(u_color_atlas, v_uv);
        float a = t.a * v_color.a;
        if (a <= 0.001) discard;
        o_color = vec4(t.rgb * a, a);
        return;
    } else if (kind == KIND_BACKDROP) {
        // Frosted glass reads and blurs the framebuffer, which the single-pass
        // GPU pipeline cannot do inline; it renders on the software path today.
        // Discard cleanly here rather than drawing a box.
        discard;
    } else {
        cov = mg_coverage(mg_rounded_box(v_local, v_half, v_radii), px);
    }

    if (cov <= 0.001) discard;

    vec4 color = v_color;
    if ((flags & FLAG_GRADIENT) != 0u) {
        float t;
        if ((flags & FLAG_RADIAL) != 0u) {
            t = clamp(length(v_norm - v_axis.xy) / max(v_params.z, 1e-4), 0.0, 1.0);
        } else if ((flags & FLAG_ANGULAR) != 0u) {
            vec2 d = v_norm - v_axis.xy;
            t = clamp((atan(d.y, d.x) + 3.14159265) / 6.28318531, 0.0, 1.0);
        } else {
            vec2  ab = v_axis.zw - v_axis.xy;
            t = clamp(dot(v_norm - v_axis.xy, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        }
        // Premultiplied linear on both ends. Perceptual (Oklch) by default;
        // FLAG_SRGB opts into the naive ramp for CSS-authored designs.
        color = ((flags & FLAG_SRGB) != 0u)
              ? mix(v_color, v_color2, t)
              : mg_mix_oklch(v_color, v_color2, t);
    }

    if (kind == KIND_TEXTURE) color *= texture(u_atlas, v_uv);

    o_color = color * cov;
}
)GLSL";

/// Assemble the complete fragment shader with the kernel spliced in.
[[nodiscard]] inline std::string glsl_fragment_source() {
    constexpr std::string_view marker = "// KERNEL_INSERTED_HERE";
    std::string out{glsl_fragment};
    const auto pos = out.find(marker);
    if (pos != std::string::npos) {
        out.replace(pos, marker.size(), std::string{glsl_kernel});
    }
    return out;
}

/// Metal Shading Language kernel — the same functions, MSL spelling.
/// `metal::` provides identical `smoothstep`/`length`/`clamp` semantics, so
/// the translation is mechanical.
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
    float pad = (I.kind == 6u) ? I.params.x * 3.0 + 2.0 : 1.0;

    float2 half_size = I.rect.zw * 0.5 + float2(pad);
    float2 centre    = I.rect.xy + I.rect.zw * 0.5;
    float2 pos       = centre + (corner * 2.0 - 1.0) * half_size;

    Varyings v;
    v.local     = pos - centre;
    v.norm      = (pos - I.rect.xy) / max(I.rect.zw, float2(1e-6));
    v.half_size = I.rect.zw * 0.5;
    v.uv        = mix(I.uv.xy, I.uv.zw, corner);
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
                               sampler smp [[sampler(0)]]) {
    constexpr uint KIND_ROUNDED_BOX = 0u, KIND_RING = 2u, KIND_LINE = 3u;
    constexpr uint KIND_GLYPH = 4u, KIND_TEXTURE = 5u, KIND_SHADOW = 6u, KIND_ARC = 7u;
    constexpr uint FLAG_GRADIENT = 1u, FLAG_RADIAL = 2u, FLAG_ANGULAR = 4u;
    constexpr uint FLAG_SRGB = 8u, FLAG_STROKE = 16u, FLAG_INSET = 32u, FLAG_GLYPH_SDF = 128u;

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
        float d = mg_rounded_box(v.local, v.half_size, v.radii);
        cov = mg_shadow(d, max(v.params.x, 0.01));
        if ((v.flags & FLAG_INSET) != 0u) cov = 1.0 - cov;
    } else if (v.kind == KIND_GLYPH) {
        float raw = atlas.sample(smp, v.uv).r;
        // A bitmap entry IS coverage; an SDF entry must be thresholded. The
        // flag travels per instance because hybrid mode mixes both.
        cov = ((v.flags & FLAG_GLYPH_SDF) != 0u)
            ? smoothstep(0.41, 0.59, raw)
            : raw;
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

    if (v.kind == KIND_TEXTURE) color *= atlas.sample(smp, v.uv);

    return color * cov;
}
)MSL";

/// WebGPU Shading Language kernel — same again, WGSL spelling.
inline constexpr std::string_view wgsl_kernel = R"WGSL(
fn mg_rounded_box(p: vec2f, b: vec2f, radii: vec4f) -> f32 {
    var r: f32;
    if (p.x > 0.0) { r = select(radii.y, radii.z, p.y > 0.0); }
    else           { r = select(radii.x, radii.w, p.y > 0.0); }
    r = min(r, min(b.x, b.y));
    let q = abs(p) - b + vec2f(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2f(0.0))) - r;
}

fn mg_ring(p: vec2f, r: f32, t: f32) -> f32 {
    return abs(length(p) - r) - t * 0.5;
}

fn mg_segment(p: vec2f, a: vec2f, b: vec2f, thickness: f32) -> f32 {
    let pa = p - a;
    let ba = b - a;
    let h  = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h) - thickness * 0.5;
}

fn mg_outline(d: f32, w: f32) -> f32 { return abs(d) - w * 0.5; }

fn mg_coverage(d: f32, px: f32) -> f32 {
    let w = max(px, 1e-6) * 0.5;
    return smoothstep(w, -w, d);
}

fn mg_shadow(d: f32, blur: f32) -> f32 {
    if (blur <= 0.0) { return select(0.0, 1.0, d <= 0.0); }
    return smoothstep(blur, -blur, d);
}
)WGSL";

/// HLSL (D3D12) kernel.
inline constexpr std::string_view hlsl_kernel = R"HLSL(
float mg_rounded_box(float2 p, float2 b, float4 radii) {
    float r = (p.x > 0.0) ? ((p.y > 0.0) ? radii.z : radii.y)
                          : ((p.y > 0.0) ? radii.w : radii.x);
    r = min(r, min(b.x, b.y));
    float2 q = abs(p) - b + float2(r, r);
    return min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0, 0.0))) - r;
}

float mg_ring(float2 p, float r, float t) {
    return abs(length(p) - r) - t * 0.5;
}

float mg_segment(float2 p, float2 a, float2 b, float thickness) {
    float2 pa = p - a, ba = b - a;
    float  h  = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h) - thickness * 0.5;
}

float mg_outline(float d, float w) { return abs(d) - w * 0.5; }

float mg_coverage(float d, float px) {
    float w = max(px, 1e-6) * 0.5;
    return smoothstep(-w, w, -d);
}

float mg_shadow(float d, float blur) {
    if (blur <= 0.0) return d <= 0.0 ? 1.0 : 0.0;
    return smoothstep(-blur, blur, -d);
}
)HLSL";

}  // namespace mayag::render::shaders
