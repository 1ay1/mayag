#pragma once
// mayag::sdf — the shape kernel
//
// This file is the single source of truth for what every mayag shape LOOKS
// like. The software rasteriser calls these functions directly; the GPU
// backends get shader source transpiled from the same formulas (see
// render/shader_source.hpp, which keeps the GLSL/MSL/WGSL text next to the C++
// so a change to one is an obvious diff against the other).
//
// Everything is a signed distance: negative inside, zero on the boundary,
// positive outside, and — critically — the gradient has unit magnitude, so
// `smoothstep(-w, w, -d)` is a correct analytic antialias at any scale and any
// zoom level. That property is why mayag has no MSAA, no supersampling, and
// no resolution-dependent artefacts.

#include "../core/geometry.hpp"
#include "../core/math.hpp"

namespace mayag::sdf {

/// Rounded box, per-corner radii. Inigo Quilez's formulation: pick the radius
/// belonging to the quadrant the sample is in, then evaluate the standard
/// rounded-box distance. `p` is relative to the box CENTRE, `b` is the half-size.
[[nodiscard]] constexpr float rounded_box(Vec2 p, Vec2 b, Vec4 radii) noexcept {
    // radii = (tl, tr, br, bl) -> select by quadrant
    float r = (p.x > 0.0f) ? ((p.y > 0.0f) ? radii.z : radii.y)    // right: br : tr
                           : ((p.y > 0.0f) ? radii.w : radii.x);   // left:  bl : tl
    r = num::min(r, num::min(b.x, b.y));

    const Vec2 q = abs(p) - b + Vec2{r, r};
    return num::min(num::max(q.x, q.y), 0.0f) + max(q, Vec2{}).length() - r;
}

[[nodiscard]] constexpr float box(Vec2 p, Vec2 b) noexcept {
    const Vec2 d = abs(p) - b;
    return num::min(num::max(d.x, d.y), 0.0f) + max(d, Vec2{}).length();
}

[[nodiscard]] constexpr float circle(Vec2 p, float r) noexcept {
    return p.length() - r;
}

/// Annulus of radius `r` and total thickness `t` (centred on the radius).
[[nodiscard]] constexpr float ring(Vec2 p, float r, float t) noexcept {
    return num::abs(p.length() - r) - t * 0.5f;
}

/// Capsule: the exact distance to a segment, minus the half-thickness. Gives
/// perfectly round caps for free, at any angle, with no geometry.
[[nodiscard]] constexpr float segment(Vec2 p, Vec2 a, Vec2 b, float thickness) noexcept {
    const Vec2  pa = p - a, ba = b - a;
    const float denom = dot(ba, ba);
    const float h = denom <= 0.0f ? 0.0f : num::saturate(dot(pa, ba) / denom);
    return (pa - ba * h).length() - thickness * 0.5f;
}

/// Circular arc from `start` sweeping `sweep` radians (both in the standard
/// atan2 frame). Used for spinners and radial progress without any tessellation.
[[nodiscard]] constexpr float arc(Vec2 p, float r, float thickness,
                                  float start, float sweep) noexcept {
    const float ring_d = ring(p, r, thickness);
    if (sweep >= num::tau) return ring_d;

    // Rotate so the arc starts at angle 0, then test against the sweep.
    float ang = num::atan2(p.y, p.x) - start;
    ang = num::mod(ang, num::tau);
    if (ang <= sweep) return ring_d;

    // Outside the sweep: distance to the nearer end cap.
    const float half = thickness * 0.5f;
    const Vec2  e0{r * num::cos(start), r * num::sin(start)};
    const Vec2  e1{r * num::cos(start + sweep), r * num::sin(start + sweep)};
    return num::min((p - e0).length(), (p - e1).length()) - half;
}

/// Equilateral triangle pointing up, circumradius `r`. For dropdown carets
/// and disclosure chevrons.
[[nodiscard]] constexpr float triangle(Vec2 p, float r) noexcept {
    constexpr float k = 1.73205081f;   // sqrt(3)
    Vec2 q{num::abs(p.x) - r, p.y + r / k};
    if (q.x + k * q.y > 0.0f) q = Vec2{q.x - k * q.y, -k * q.x - q.y} * 0.5f;
    q.x -= num::clamp(q.x, -2.0f * r, 0.0f);
    return -q.length() * num::sign(q.y);
}

// ── combinators ─────────────────────────────────────────────────────────

[[nodiscard]] constexpr float unite(float a, float b) noexcept { return num::min(a, b); }
[[nodiscard]] constexpr float intersect(float a, float b) noexcept { return num::max(a, b); }
[[nodiscard]] constexpr float subtract(float a, float b) noexcept { return num::max(a, -b); }

/// Smooth union — the "metaball" blend. Makes adjacent shapes merge with a
/// fillet instead of a crease; used for tooltip tails and connected toggles.
[[nodiscard]] constexpr float smooth_unite(float a, float b, float k) noexcept {
    if (k <= 0.0f) return unite(a, b);
    const float h = num::saturate(0.5f + 0.5f * (b - a) / k);
    return num::lerp(b, a, h) - k * h * (1.0f - h);
}

/// Hollow out a shape to a `w`-wide outline. This is exactly what a border is.
[[nodiscard]] constexpr float outline(float d, float w) noexcept {
    return num::abs(d) - w * 0.5f;
}

// ── coverage ────────────────────────────────────────────────────────────

/// Convert a distance to a coverage value in [0,1] with analytic antialiasing.
/// `px` is the size of one pixel in the SDF's units — 1.0 for unscaled screen
/// space. Because the SDF is a true distance field this is correct for any
/// transform, which is why mayag text and shapes stay crisp when zoomed.
[[nodiscard]] constexpr float coverage(float d, float px = 1.0f) noexcept {
    return num::saturate(0.5f - d / num::max(px, 1e-6f));
}

/// Smoothstep variant — marginally softer, matches what most GPU UI code does,
/// and is what the generated shaders use so CPU and GPU output match.
[[nodiscard]] constexpr float coverage_smooth(float d, float px = 1.0f) noexcept {
    const float w = num::max(px, 1e-6f) * 0.5f;
    return num::smoothstep(w, -w, d);
}

/// Approximate a Gaussian blur of a box edge analytically. A real Gaussian
/// convolution of a box is an error function; this is the standard
/// `smoothstep` stand-in, accurate to ~1% and free of any blur pass.
///
/// This one function is why mayag renders shadows without a single offscreen
/// target: the shadow of a rounded box is just its SDF, softened.
[[nodiscard]] constexpr float shadow_coverage(float d, float blur) noexcept {
    if (blur <= 0.0f) return d <= 0.0f ? 1.0f : 0.0f;
    return num::smoothstep(blur, -blur, d);
}

// ── compile-time sanity ─────────────────────────────────────────────────

namespace detail {
constexpr bool near(float a, float b, float eps = 1e-4f) { return num::abs(a - b) <= eps; }

// Centre of a 100x100 box is 50 units inside.
static_assert(near(box(Vec2{}, Vec2{50, 50}), -50.0f));
// A point 10 units right of the right edge is at distance 10.
static_assert(near(box(Vec2{60, 0}, Vec2{50, 50}), 10.0f));
// Zero radii must agree exactly with the sharp box — a discontinuity here
// would make `radius(0)` render differently from no radius at all.
static_assert(near(rounded_box(Vec2{20, 10}, Vec2{50, 50}, Vec4{}), box(Vec2{20, 10}, Vec2{50, 50})));
// Fully rounded box == circle.
static_assert(near(rounded_box(Vec2{30, 0}, Vec2{50, 50}, Vec4{50, 50, 50, 50}), circle(Vec2{30, 0}, 50.0f)));
static_assert(near(segment(Vec2{0, 5}, Vec2{-10, 0}, Vec2{10, 0}, 2.0f), 4.0f));
static_assert(coverage(-10.0f) == 1.0f && coverage(10.0f) == 0.0f);
static_assert(near(coverage(0.0f), 0.5f));
}  // namespace detail

}  // namespace mayag::sdf
