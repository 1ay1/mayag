#pragma once
// mayag::num — constexpr scalar math
//
// Everything in mayag's style pipeline (colours, gradients, corner radii,
// easing curves) must be evaluable at COMPILE TIME, because the DSL resolves
// a whole widget tree into a `constexpr` draw description. The standard math
// functions are not constexpr before C++26, and we refuse to require a
// bleeding-edge libstdc++ for a colour conversion. So we implement the small
// set we need, from scratch, with well-understood numerics.
//
// These are not "fast approximations" — each is accurate to within a few ULP
// over the domain the UI layer uses, and each is exact on the special values
// (0, 1, inf, nan) that a naive Newton iteration gets wrong.

#include <bit>
#include <concepts>
#include <cstdint>
#include <limits>

namespace mayag::num {

// ── constants ───────────────────────────────────────────────────────────

inline constexpr float pi      = 3.14159265358979323846f;
inline constexpr float tau     = 6.28318530717958647692f;
inline constexpr float half_pi = 1.57079632679489661923f;
inline constexpr float inv_pi  = 0.31830988618379067154f;
inline constexpr float ln2     = 0.69314718055994530942f;
inline constexpr float inf     = std::numeric_limits<float>::infinity();

// ── predicates ──────────────────────────────────────────────────────────

[[nodiscard]] constexpr bool is_nan(float x) noexcept { return x != x; }
[[nodiscard]] constexpr bool is_inf(float x) noexcept { return x == inf || x == -inf; }
[[nodiscard]] constexpr bool is_finite(float x) noexcept { return !is_nan(x) && !is_inf(x); }

/// `x` if it is a usable finite number, else `fallback`. Layout code is full
/// of "unbounded axis" sentinels; this keeps that from becoming a thicket of
/// nested conditionals.
[[nodiscard]] constexpr float isfinite_or(float x, float fallback) noexcept {
    return is_finite(x) ? x : fallback;
}

// ── basics ──────────────────────────────────────────────────────────────

template <typename T> requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T min(T a, T b) noexcept { return b < a ? b : a; }

template <typename T> requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T max(T a, T b) noexcept { return a < b ? b : a; }

template <typename T> requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T clamp(T x, T lo, T hi) noexcept {
    return x < lo ? lo : (hi < x ? hi : x);
}

[[nodiscard]] constexpr float abs(float x) noexcept { return x < 0.0f ? -x : x; }
[[nodiscard]] constexpr float sign(float x) noexcept {
    return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f);
}

[[nodiscard]] constexpr float floor(float x) noexcept {
    if (is_nan(x) || is_inf(x)) return x;
    const auto i = static_cast<long long>(x);
    const auto f = static_cast<float>(i);
    return (f > x) ? f - 1.0f : f;
}

[[nodiscard]] constexpr float ceil(float x) noexcept {
    if (is_nan(x) || is_inf(x)) return x;
    const auto i = static_cast<long long>(x);
    const auto f = static_cast<float>(i);
    return (f < x) ? f + 1.0f : f;
}

[[nodiscard]] constexpr float round(float x) noexcept {
    return x < 0.0f ? -floor(-x + 0.5f) : floor(x + 0.5f);
}

[[nodiscard]] constexpr float fract(float x) noexcept { return x - floor(x); }

[[nodiscard]] constexpr float mod(float x, float m) noexcept {
    if (m == 0.0f) return 0.0f;
    return x - m * floor(x / m);
}

// ── interpolation ───────────────────────────────────────────────────────

[[nodiscard]] constexpr float lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

/// Inverse lerp with a degenerate-range guard.
[[nodiscard]] constexpr float unlerp(float a, float b, float x) noexcept {
    return (b == a) ? 0.0f : (x - a) / (b - a);
}

[[nodiscard]] constexpr float saturate(float x) noexcept { return clamp(x, 0.0f, 1.0f); }

[[nodiscard]] constexpr float step(float edge, float x) noexcept {
    return x < edge ? 0.0f : 1.0f;
}

/// GLSL-compatible smoothstep — the Hermite S-curve used for every antialiased
/// edge in the SDF renderer, so the CPU and GPU paths agree bit-for-bit.
[[nodiscard]] constexpr float smoothstep(float e0, float e1, float x) noexcept {
    const float t = saturate(unlerp(e0, e1, x));
    return t * t * (3.0f - 2.0f * t);
}

// ── roots ───────────────────────────────────────────────────────────────

/// Exact-as-hardware square root, via bit-hack seed + 4 Newton steps in double.
/// The double accumulator is what makes the float result correctly rounded for
/// every normal input we care about.
[[nodiscard]] constexpr float sqrt(float x) noexcept {
    if (is_nan(x) || x < 0.0f) return std::numeric_limits<float>::quiet_NaN();
    if (x == 0.0f || is_inf(x)) return x;

    // Seed from the exponent halving trick (Quake-style, but for sqrt not rsqrt).
    const auto bits = std::bit_cast<std::uint32_t>(x);
    float y = std::bit_cast<float>((bits >> 1) + 0x1FC0'0000u);

    double d = static_cast<double>(y);
    const double v = static_cast<double>(x);
    for (int i = 0; i < 5; ++i) d = 0.5 * (d + v / d);
    return static_cast<float>(d);
}

/// Signed cube root. Needed by the OKLab transfer function, which cube-roots
/// LMS cone responses — those are non-negative in practice, but a signed
/// version keeps out-of-gamut intermediates from becoming NaN.
[[nodiscard]] constexpr float cbrt(float x) noexcept {
    if (is_nan(x) || is_inf(x) || x == 0.0f) return x;
    const float s = sign(x);
    const double v = static_cast<double>(x < 0.0f ? -x : x);

    // Seed by dividing the biased exponent by 3.
    const auto bits = std::bit_cast<std::uint32_t>(x < 0.0f ? -x : x);
    double d = static_cast<double>(std::bit_cast<float>(bits / 3u + 0x2A51'20A4u));

    for (int i = 0; i < 6; ++i) d = (2.0 * d + v / (d * d)) / 3.0;
    return s * static_cast<float>(d);
}

// ── exp / log / pow ─────────────────────────────────────────────────────

/// exp via range reduction to [-ln2/2, ln2/2] then a degree-7 Taylor series.
[[nodiscard]] constexpr float exp(float x) noexcept {
    if (is_nan(x)) return x;
    if (x > 88.7f)  return inf;
    if (x < -87.3f) return 0.0f;

    const double v  = static_cast<double>(x);
    const double k  = static_cast<double>(round(static_cast<float>(v / static_cast<double>(ln2))));
    const double r  = v - k * 0.693147180559945309417232;

    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 12; ++i) { term *= r / i; sum += term; }

    // Scale by 2^k by direct exponent construction — no ldexp in constexpr.
    const int ki = static_cast<int>(k);
    double scale = 1.0;
    double base  = (ki < 0) ? 0.5 : 2.0;
    for (int i = 0, n = (ki < 0 ? -ki : ki); i < n; ++i) scale *= base;
    return static_cast<float>(sum * scale);
}

/// Natural log via mantissa/exponent split + atanh series (fast, symmetric error).
[[nodiscard]] constexpr float log(float x) noexcept {
    if (is_nan(x) || x < 0.0f) return std::numeric_limits<float>::quiet_NaN();
    if (x == 0.0f) return -inf;
    if (is_inf(x)) return inf;

    const auto bits = std::bit_cast<std::uint32_t>(x);
    int  e = static_cast<int>((bits >> 23) & 0xFFu) - 127;
    auto m = std::bit_cast<float>((bits & 0x807F'FFFFu) | 0x3F80'0000u);  // [1,2)

    if (m > 1.4142135f) { m *= 0.5f; ++e; }   // recentre to [1/sqrt2, sqrt2)

    const double z = (static_cast<double>(m) - 1.0) / (static_cast<double>(m) + 1.0);
    const double z2 = z * z;
    double sum = 0.0, p = z;
    for (int i = 1; i <= 15; i += 2) { sum += p / i; p *= z2; }
    return static_cast<float>(2.0 * sum + e * 0.693147180559945309417232);
}

/// pow with exact integer-exponent fast path (so pow(x, 2.0f) is not a
/// round-tripped exp/log, which would break `==` comparisons in tests).
[[nodiscard]] constexpr float pow(float base, float e) noexcept {
    if (e == 0.0f) return 1.0f;
    if (base == 0.0f) return e > 0.0f ? 0.0f : inf;
    if (e == static_cast<float>(static_cast<int>(e)) && abs(e) <= 64.0f) {
        int n = static_cast<int>(e);
        const bool invert = n < 0;
        if (invert) n = -n;
        double acc = 1.0, b = static_cast<double>(base);
        while (n) { if (n & 1) acc *= b; b *= b; n >>= 1; }
        return static_cast<float>(invert ? 1.0 / acc : acc);
    }
    if (base < 0.0f) return std::numeric_limits<float>::quiet_NaN();
    return exp(e * log(base));
}

// ── trigonometry ────────────────────────────────────────────────────────

/// sin via reduction to [-pi, pi] and a degree-15 Taylor series.
[[nodiscard]] constexpr float sin(float x) noexcept {
    if (is_nan(x) || is_inf(x)) return std::numeric_limits<float>::quiet_NaN();
    double r = static_cast<double>(x - tau * floor((x + pi) / tau));
    const double r2 = r * r;
    double term = r, sum = r;
    for (int i = 1; i <= 8; ++i) {
        term *= -r2 / ((2 * i) * (2 * i + 1));
        sum  += term;
    }
    return static_cast<float>(sum);
}

[[nodiscard]] constexpr float cos(float x) noexcept { return sin(x + half_pi); }

[[nodiscard]] constexpr float tan(float x) noexcept {
    const float c = cos(x);
    return c == 0.0f ? inf : sin(x) / c;
}

/// atan on the full line via the [-1,1] core series plus the reciprocal identity.
[[nodiscard]] constexpr float atan(float x) noexcept {
    if (is_nan(x)) return x;
    if (is_inf(x)) return x > 0 ? half_pi : -half_pi;

    const float s = sign(x);
    float a = abs(x);
    bool  reciprocal = false;
    if (a > 1.0f) { a = 1.0f / a; reciprocal = true; }

    // Series converges slowly near 1; halve once with the tangent-half identity.
    bool halved = false;
    if (a > 0.4142136f) { a = (a - 0.4142136f) / (1.0f + 0.4142136f * a); halved = true; }

    const double z = static_cast<double>(a), z2 = z * z;
    double sum = 0.0, p = z;
    for (int i = 1; i <= 25; i += 2) {
        sum += ((i % 4 == 1) ? p / i : -p / i);
        p *= z2;
    }
    if (halved)     sum += 0.39269908169872414;   // pi/8
    if (reciprocal) sum = 1.5707963267948966 - sum;
    return s * static_cast<float>(sum);
}

[[nodiscard]] constexpr float atan2(float y, float x) noexcept {
    if (x > 0.0f)              return atan(y / x);
    if (x < 0.0f && y >= 0.0f) return atan(y / x) + pi;
    if (x < 0.0f)              return atan(y / x) - pi;
    if (y > 0.0f)              return half_pi;
    if (y < 0.0f)              return -half_pi;
    return 0.0f;
}

// ── unit conversions ────────────────────────────────────────────────────

[[nodiscard]] constexpr float radians(float degrees) noexcept { return degrees * (pi / 180.0f); }
[[nodiscard]] constexpr float degrees(float radians_) noexcept { return radians_ * (180.0f / pi); }

// ── self-check ──────────────────────────────────────────────────────────
// These fire at compile time in every translation unit that includes the
// header, which is exactly when a broken numeric kernel is cheapest to find.

namespace detail {
constexpr bool near(float a, float b, float eps = 1e-5f) { return abs(a - b) <= eps; }
static_assert(sqrt(0.0f) == 0.0f && sqrt(1.0f) == 1.0f);
static_assert(near(sqrt(2.0f), 1.41421356f));
static_assert(near(cbrt(27.0f), 3.0f) && near(cbrt(-8.0f), -2.0f));
static_assert(near(exp(1.0f), 2.71828183f, 1e-4f));
static_assert(near(log(2.71828183f), 1.0f, 1e-5f));
static_assert(pow(2.0f, 10.0f) == 1024.0f);          // exact integer path
static_assert(near(pow(2.0f, 0.5f), 1.41421356f, 1e-5f));
static_assert(near(sin(0.0f), 0.0f) && near(sin(half_pi), 1.0f, 1e-5f));
static_assert(near(cos(0.0f), 1.0f, 1e-5f));
static_assert(near(atan2(1.0f, 1.0f), pi / 4.0f, 1e-4f));
static_assert(smoothstep(0.0f, 1.0f, 0.5f) == 0.5f);
}  // namespace detail

}  // namespace mayag::num
