#pragma once
// mayag::geom — vectors, rects, affine transforms
//
// GPU UI is a geometry problem before it is a pixel problem. Every type here
// is a literal type with constexpr arithmetic, so a whole scene can be laid
// out and transformed inside a `constexpr` evaluation and baked into the
// binary as a plain array of vertices.
//
// Conventions (chosen once, enforced everywhere):
//   * Y grows DOWNWARD  — matches every windowing system and image format.
//   * Rects are stored as {origin, size}, never {min, max}; size is never
//     negative (constructors normalise).
//   * Transforms are 2x3 affine, row-major, applied as p' = M * [p, 1].

#include "math.hpp"

#include <cstddef>
#include <cstdint>

namespace mayag {

// ── Vec2 ────────────────────────────────────────────────────────────────

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) noexcept : x{x_}, y{y_} {}
    explicit constexpr Vec2(float s) noexcept : x{s}, y{s} {}

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, Vec2 b) noexcept { return {a.x * b.x, a.y * b.y}; }
    friend constexpr Vec2 operator/(Vec2 a, Vec2 b) noexcept { return {a.x / b.x, a.y / b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, float s) noexcept { return {a.x * s, a.y * s}; }
    friend constexpr Vec2 operator*(float s, Vec2 a) noexcept { return {a.x * s, a.y * s}; }
    friend constexpr Vec2 operator/(Vec2 a, float s) noexcept { return {a.x / s, a.y / s}; }
    constexpr Vec2 operator-() const noexcept { return {-x, -y}; }

    constexpr Vec2& operator+=(Vec2 b) noexcept { x += b.x; y += b.y; return *this; }
    constexpr Vec2& operator-=(Vec2 b) noexcept { x -= b.x; y -= b.y; return *this; }
    constexpr Vec2& operator*=(float s) noexcept { x *= s; y *= s; return *this; }

    friend constexpr bool operator==(Vec2, Vec2) = default;

    [[nodiscard]] constexpr float length_squared() const noexcept { return x * x + y * y; }
    [[nodiscard]] constexpr float length() const noexcept { return num::sqrt(length_squared()); }

    [[nodiscard]] constexpr Vec2 normalized() const noexcept {
        const float l = length();
        return l == 0.0f ? Vec2{} : Vec2{x / l, y / l};
    }

    /// Perpendicular (90 degrees clockwise in screen space).
    [[nodiscard]] constexpr Vec2 perp() const noexcept { return {-y, x}; }
};

[[nodiscard]] constexpr float dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] constexpr float cross(Vec2 a, Vec2 b) noexcept { return a.x * b.y - a.y * b.x; }
[[nodiscard]] constexpr Vec2 min(Vec2 a, Vec2 b) noexcept { return {num::min(a.x, b.x), num::min(a.y, b.y)}; }
[[nodiscard]] constexpr Vec2 max(Vec2 a, Vec2 b) noexcept { return {num::max(a.x, b.x), num::max(a.y, b.y)}; }
[[nodiscard]] constexpr Vec2 abs(Vec2 v) noexcept { return {num::abs(v.x), num::abs(v.y)}; }
[[nodiscard]] constexpr Vec2 lerp(Vec2 a, Vec2 b, float t) noexcept { return a + (b - a) * t; }
[[nodiscard]] constexpr float distance(Vec2 a, Vec2 b) noexcept { return (b - a).length(); }

// ── Vec4 (colour payloads, shader uniforms, corner radii) ───────────────

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) noexcept : x{x_}, y{y_}, z{z_}, w{w_} {}
    explicit constexpr Vec4(float s) noexcept : x{s}, y{s}, z{s}, w{s} {}

    friend constexpr Vec4 operator+(Vec4 a, Vec4 b) noexcept { return {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; }
    friend constexpr Vec4 operator-(Vec4 a, Vec4 b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}; }
    friend constexpr Vec4 operator*(Vec4 a, float s) noexcept { return {a.x*s, a.y*s, a.z*s, a.w*s}; }
    friend constexpr bool operator==(Vec4, Vec4) = default;

    [[nodiscard]] constexpr float max_component() const noexcept {
        return num::max(num::max(x, y), num::max(z, w));
    }
};

[[nodiscard]] constexpr Vec4 lerp(Vec4 a, Vec4 b, float t) noexcept { return a + (b - a) * t; }

// ── Rect ────────────────────────────────────────────────────────────────

struct Rect {
    Vec2 origin{};   ///< top-left
    Vec2 size{};     ///< always non-negative

    constexpr Rect() = default;
    constexpr Rect(Vec2 o, Vec2 s) noexcept
        : origin{o}, size{num::max(s.x, 0.0f), num::max(s.y, 0.0f)} {}
    constexpr Rect(float x, float y, float w, float h) noexcept
        : Rect{Vec2{x, y}, Vec2{w, h}} {}

    [[nodiscard]] static constexpr Rect from_bounds(Vec2 a, Vec2 b) noexcept {
        const Vec2 lo = mayag::min(a, b), hi = mayag::max(a, b);
        return Rect{lo, hi - lo};
    }

    [[nodiscard]] static constexpr Rect centered(Vec2 c, Vec2 s) noexcept {
        return Rect{c - s * 0.5f, s};
    }

    [[nodiscard]] constexpr float left()   const noexcept { return origin.x; }
    [[nodiscard]] constexpr float top()    const noexcept { return origin.y; }
    [[nodiscard]] constexpr float right()  const noexcept { return origin.x + size.x; }
    [[nodiscard]] constexpr float bottom() const noexcept { return origin.y + size.y; }
    [[nodiscard]] constexpr float width()  const noexcept { return size.x; }
    [[nodiscard]] constexpr float height() const noexcept { return size.y; }
    [[nodiscard]] constexpr Vec2  min()    const noexcept { return origin; }
    [[nodiscard]] constexpr Vec2  max()    const noexcept { return origin + size; }
    [[nodiscard]] constexpr Vec2  center() const noexcept { return origin + size * 0.5f; }
    [[nodiscard]] constexpr Vec2  half()   const noexcept { return size * 0.5f; }
    [[nodiscard]] constexpr float area()   const noexcept { return size.x * size.y; }
    [[nodiscard]] constexpr bool  empty()  const noexcept { return size.x <= 0.0f || size.y <= 0.0f; }

    [[nodiscard]] constexpr bool contains(Vec2 p) const noexcept {
        return p.x >= left() && p.x < right() && p.y >= top() && p.y < bottom();
    }

    [[nodiscard]] constexpr bool intersects(const Rect& o) const noexcept {
        return left() < o.right() && o.left() < right() &&
               top()  < o.bottom() && o.top() < bottom();
    }

    /// Set intersection; empty (zero-size) when disjoint.
    [[nodiscard]] constexpr Rect intersect(const Rect& o) const noexcept {
        const Vec2 lo = mayag::max(min(), o.min());
        const Vec2 hi = mayag::min(max(), o.max());
        return (hi.x <= lo.x || hi.y <= lo.y) ? Rect{lo, Vec2{}} : Rect{lo, hi - lo};
    }

    /// Bounding union. An empty rect is the identity, so folding a list works.
    [[nodiscard]] constexpr Rect unite(const Rect& o) const noexcept {
        if (empty()) return o;
        if (o.empty()) return *this;
        return from_bounds(mayag::min(min(), o.min()), mayag::max(max(), o.max()));
    }

    [[nodiscard]] constexpr Rect inset(float d) const noexcept {
        return Rect{origin + Vec2{d, d}, size - Vec2{d * 2.0f, d * 2.0f}};
    }

    [[nodiscard]] constexpr Rect inset(float top_, float right_, float bottom_, float left_) const noexcept {
        return Rect{origin + Vec2{left_, top_},
                    size - Vec2{left_ + right_, top_ + bottom_}};
    }

    [[nodiscard]] constexpr Rect expand(float d) const noexcept { return inset(-d); }
    [[nodiscard]] constexpr Rect translate(Vec2 d) const noexcept { return Rect{origin + d, size}; }
    [[nodiscard]] constexpr Rect scale(float s) const noexcept { return Rect{origin * s, size * s}; }

    /// Round outward to integer pixel bounds — the correct rounding for a
    /// scissor/clip rect, where cutting a partially covered pixel is a bug.
    [[nodiscard]] constexpr Rect pixel_snap_out() const noexcept {
        const Vec2 lo{num::floor(left()), num::floor(top())};
        const Vec2 hi{num::ceil(right()), num::ceil(bottom())};
        return Rect{lo, hi - lo};
    }

    friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

// ── Edge insets (padding / margin / border widths) ──────────────────────

struct Insets {
    float top = 0.0f, right = 0.0f, bottom = 0.0f, left = 0.0f;

    constexpr Insets() = default;
    explicit constexpr Insets(float all) noexcept
        : top{all}, right{all}, bottom{all}, left{all} {}
    constexpr Insets(float vertical, float horizontal) noexcept
        : top{vertical}, right{horizontal}, bottom{vertical}, left{horizontal} {}
    constexpr Insets(float t, float r, float b, float l) noexcept
        : top{t}, right{r}, bottom{b}, left{l} {}

    [[nodiscard]] constexpr float horizontal() const noexcept { return left + right; }
    [[nodiscard]] constexpr float vertical()   const noexcept { return top + bottom; }
    [[nodiscard]] constexpr Vec2  total()      const noexcept { return {horizontal(), vertical()}; }

    friend constexpr Insets operator+(Insets a, Insets b) noexcept {
        return {a.top + b.top, a.right + b.right, a.bottom + b.bottom, a.left + b.left};
    }
    friend constexpr bool operator==(Insets, Insets) = default;
};

[[nodiscard]] constexpr Rect deflate(const Rect& r, Insets i) noexcept {
    return r.inset(i.top, i.right, i.bottom, i.left);
}

// ── Corner radii ────────────────────────────────────────────────────────

struct Corners {
    float tl = 0.0f, tr = 0.0f, br = 0.0f, bl = 0.0f;

    constexpr Corners() = default;
    explicit constexpr Corners(float all) noexcept : tl{all}, tr{all}, br{all}, bl{all} {}
    constexpr Corners(float tl_, float tr_, float br_, float bl_) noexcept
        : tl{tl_}, tr{tr_}, br{br_}, bl{bl_} {}

    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return tl == 0.0f && tr == 0.0f && br == 0.0f && bl == 0.0f;
    }
    [[nodiscard]] constexpr float largest() const noexcept {
        return num::max(num::max(tl, tr), num::max(br, bl));
    }

    /// Clamp so opposing radii can never exceed the box — the CSS rule. Without
    /// this a `radius(999)` pill on a short box produces an inverted SDF and
    /// renders as a bowtie.
    [[nodiscard]] constexpr Corners clamp_to(Vec2 size) const noexcept {
        const float lim = num::min(size.x, size.y) * 0.5f;
        return Corners{num::min(tl, lim), num::min(tr, lim),
                       num::min(br, lim), num::min(bl, lim)};
    }

    [[nodiscard]] constexpr Vec4 as_vec4() const noexcept { return {tl, tr, br, bl}; }
    friend constexpr bool operator==(Corners, Corners) = default;
};

// ── Affine 2x3 transform ────────────────────────────────────────────────
//
//   | a  c  tx |
//   | b  d  ty |
//   | 0  0   1 |

struct Affine {
    float a = 1.0f, b = 0.0f;
    float c = 0.0f, d = 1.0f;
    float tx = 0.0f, ty = 0.0f;

    [[nodiscard]] static constexpr Affine identity() noexcept { return {}; }

    [[nodiscard]] static constexpr Affine translation(Vec2 t) noexcept {
        return {1, 0, 0, 1, t.x, t.y};
    }
    [[nodiscard]] static constexpr Affine scaling(Vec2 s) noexcept {
        return {s.x, 0, 0, s.y, 0, 0};
    }
    [[nodiscard]] static constexpr Affine rotation(float radians) noexcept {
        const float s = num::sin(radians), co = num::cos(radians);
        return {co, s, -s, co, 0, 0};
    }
    /// Rotation about an arbitrary pivot — the form UI code actually wants.
    [[nodiscard]] static constexpr Affine rotation_about(float radians, Vec2 pivot) noexcept {
        return translation(pivot) * rotation(radians) * translation(-pivot);
    }
    [[nodiscard]] static constexpr Affine skewing(float ax, float ay) noexcept {
        return {1, num::tan(ay), num::tan(ax), 1, 0, 0};
    }

    /// Composition: (A * B) applied to p equals A(B(p)).
    friend constexpr Affine operator*(const Affine& m, const Affine& n) noexcept {
        return {
            m.a * n.a + m.c * n.b,
            m.b * n.a + m.d * n.b,
            m.a * n.c + m.c * n.d,
            m.b * n.c + m.d * n.d,
            m.a * n.tx + m.c * n.ty + m.tx,
            m.b * n.tx + m.d * n.ty + m.ty,
        };
    }

    [[nodiscard]] constexpr Vec2 apply(Vec2 p) const noexcept {
        return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty};
    }
    /// Transform a direction — ignores translation.
    [[nodiscard]] constexpr Vec2 apply_vector(Vec2 v) const noexcept {
        return {a * v.x + c * v.y, b * v.x + d * v.y};
    }

    [[nodiscard]] constexpr float determinant() const noexcept { return a * d - b * c; }

    [[nodiscard]] constexpr bool invertible() const noexcept {
        return num::abs(determinant()) > 1e-12f;
    }

    /// Inverse; returns identity for a singular matrix rather than producing
    /// infinities that would poison an entire hit-test tree.
    [[nodiscard]] constexpr Affine inverse() const noexcept {
        const float det = determinant();
        if (num::abs(det) < 1e-12f) return identity();
        const float inv = 1.0f / det;
        return {
             d * inv, -b * inv,
            -c * inv,  a * inv,
            (c * ty - d * tx) * inv,
            (b * tx - a * ty) * inv,
        };
    }

    [[nodiscard]] constexpr bool is_identity() const noexcept {
        return a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f && tx == 0.0f && ty == 0.0f;
    }

    /// True when the matrix maps axis-aligned rects to axis-aligned rects.
    /// The renderer takes a much cheaper path when this holds.
    [[nodiscard]] constexpr bool is_axis_aligned() const noexcept {
        return b == 0.0f && c == 0.0f;
    }

    /// Axis-aligned bounding box of the transformed rect.
    [[nodiscard]] constexpr Rect apply(const Rect& r) const noexcept {
        const Vec2 p0 = apply(r.min());
        const Vec2 p1 = apply(Vec2{r.right(), r.top()});
        const Vec2 p2 = apply(r.max());
        const Vec2 p3 = apply(Vec2{r.left(), r.bottom()});
        return Rect::from_bounds(mayag::min(mayag::min(p0, p1), mayag::min(p2, p3)),
                                 mayag::max(mayag::max(p0, p1), mayag::max(p2, p3)));
    }

    friend constexpr bool operator==(const Affine&, const Affine&) = default;
};

// ── compile-time sanity ─────────────────────────────────────────────────

static_assert(Rect(0, 0, 10, 10).intersect(Rect(5, 5, 10, 10)) == Rect(5, 5, 5, 5));
static_assert(Rect(0, 0, 10, 10).intersect(Rect(20, 20, 5, 5)).empty());
static_assert(Rect(0, 0, -5, -5).size == Vec2{});
static_assert(Corners{100.0f}.clamp_to({20, 10}).tl == 5.0f);
static_assert(Affine::identity().is_identity());
static_assert((Affine::translation({3, 4}) * Affine::identity()).apply(Vec2{}) == Vec2(3, 4));
static_assert(Affine::scaling({2, 2}).inverse().apply(Vec2{4, 4}) == Vec2(2, 2));

}  // namespace mayag
