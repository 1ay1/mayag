#pragma once
// mayag::color — phantom-typed colour spaces
//
// The single most common bug in GPU UI is doing arithmetic in the wrong colour
// space: blending two sRGB values as if they were linear (muddy midtones),
// interpolating a gradient in RGB (grey dead-zone between complements), or
// uploading a linear buffer to an sRGB texture (washed out).
//
// mayag makes that a TYPE error. `Color<Srgb>` and `Color<Linear>` are
// distinct types; the only way between them is an explicit `to<>` that applies
// the real transfer function. Blending is only defined on `Linear`, and
// perceptual interpolation is only defined on `Oklch`. The GPU never sees a
// space it did not ask for.
//
//     constexpr auto brand = rgb<0x5B8CFF>;            // Color<Srgb>
//     constexpr auto lit   = brand.to<Linear>() * 1.4f; // linear-light scale
//     constexpr auto mid   = mix_perceptual(a, b, 0.5f);// via Oklch, no grey

#include "math.hpp"
#include "geometry.hpp"

#include <cstdint>
#include <concepts>

namespace mayag {

// ── space tags ──────────────────────────────────────────────────────────

/// Gamma-encoded sRGB — what a designer types, what a PNG stores. NOT linear:
/// arithmetic on these values is perceptually plausible but physically wrong.
struct Srgb {};

/// Linear-light sRGB primaries. The only space where alpha compositing,
/// additive blending, and light accumulation are correct.
struct Linear {};

/// Oklab — perceptually uniform, Cartesian (L, a, b). Best for gradients and
/// contrast math; equal numeric steps are equal perceptual steps.
struct Oklab {};

/// Oklch — Oklab in cylindrical form (L, C, h). Best for programmatic palettes:
/// rotate `h` for a complementary colour, scale `C` for a muted variant.
struct Oklch {};

template <typename T>
concept ColorSpace = std::same_as<T, Srgb>   || std::same_as<T, Linear> ||
                     std::same_as<T, Oklab>  || std::same_as<T, Oklch>;

// ── transfer functions ──────────────────────────────────────────────────

namespace detail {

/// IEC 61966-2-1 sRGB EOTF. The linear toe below 0.04045 matters: the naive
/// pow(x, 2.2) approximation is visibly wrong in the darkest 5% of the range,
/// which is exactly where UI shadows live.
[[nodiscard]] constexpr float srgb_to_linear(float c) noexcept {
    c = num::saturate(c);
    return c <= 0.04045f ? c / 12.92f
                         : num::pow((c + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] constexpr float linear_to_srgb(float c) noexcept {
    c = num::saturate(c);
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * num::pow(c, 1.0f / 2.4f) - 0.055f;
}

}  // namespace detail

// ── Color<Space> ────────────────────────────────────────────────────────

template <ColorSpace Space>
struct Color {
    // Channel meaning depends on Space:
    //   Srgb/Linear : r, g, b
    //   Oklab       : L, a, b
    //   Oklch       : L, C, h(degrees)
    float c0 = 0.0f;
    float c1 = 0.0f;
    float c2 = 0.0f;
    float a  = 1.0f;   ///< alpha is always linear, never gamma-encoded

    constexpr Color() = default;
    constexpr Color(float x, float y, float z, float alpha = 1.0f) noexcept
        : c0{x}, c1{y}, c2{z}, a{alpha} {}

    friend constexpr bool operator==(Color, Color) = default;

    // -- conversions ------------------------------------------------------

    /// Convert to another space. Identity conversion is free; every other
    /// path routes through Linear, which is the hub of the graph.
    template <ColorSpace To>
    [[nodiscard]] constexpr Color<To> to() const noexcept;

    // -- component ops (space-agnostic, always safe) ----------------------

    [[nodiscard]] constexpr Color with_alpha(float alpha) const noexcept {
        return {c0, c1, c2, alpha};
    }
    /// Multiply alpha — for fading a themed colour without restating it.
    [[nodiscard]] constexpr Color fade(float factor) const noexcept {
        return {c0, c1, c2, a * num::saturate(factor)};
    }
    [[nodiscard]] constexpr bool opaque() const noexcept { return a >= 1.0f; }
    [[nodiscard]] constexpr bool invisible() const noexcept { return a <= 0.0f; }

    [[nodiscard]] constexpr Vec4 as_vec4() const noexcept { return {c0, c1, c2, a}; }
};

// -- scaling: only meaningful where the space is linear in light ---------

/// Scale radiance. Deliberately only defined for Linear — `Color<Srgb> * 2`
/// is a compile error, because "twice as bright" is not multiplication in a
/// gamma-encoded space.
[[nodiscard]] constexpr Color<Linear> operator*(Color<Linear> c, float s) noexcept {
    return {c.c0 * s, c.c1 * s, c.c2 * s, c.a};
}
[[nodiscard]] constexpr Color<Linear> operator*(float s, Color<Linear> c) noexcept { return c * s; }

[[nodiscard]] constexpr Color<Linear> operator+(Color<Linear> x, Color<Linear> y) noexcept {
    return {x.c0 + y.c0, x.c1 + y.c1, x.c2 + y.c2, num::saturate(x.a + y.a)};
}

// ── conversion implementations ──────────────────────────────────────────

namespace detail {

[[nodiscard]] constexpr Color<Linear> srgb_to_linear(Color<Srgb> s) noexcept {
    return {srgb_to_linear(s.c0), srgb_to_linear(s.c1), srgb_to_linear(s.c2), s.a};
}

[[nodiscard]] constexpr Color<Srgb> linear_to_srgb(Color<Linear> l) noexcept {
    return {linear_to_srgb(l.c0), linear_to_srgb(l.c1), linear_to_srgb(l.c2), l.a};
}

/// Björn Ottosson's Oklab. Linear sRGB -> LMS cone response -> cube root ->
/// a fixed 3x3. The cube root is what buys perceptual uniformity.
[[nodiscard]] constexpr Color<Oklab> linear_to_oklab(Color<Linear> c) noexcept {
    const float l = 0.4122214708f * c.c0 + 0.5363325363f * c.c1 + 0.0514459929f * c.c2;
    const float m = 0.2119034982f * c.c0 + 0.6806995451f * c.c1 + 0.1073969566f * c.c2;
    const float s = 0.0883024619f * c.c0 + 0.2817188376f * c.c1 + 0.6299787005f * c.c2;

    const float l_ = num::cbrt(l), m_ = num::cbrt(m), s_ = num::cbrt(s);

    return {0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
            1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
            0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
            c.a};
}

[[nodiscard]] constexpr Color<Linear> oklab_to_linear(Color<Oklab> c) noexcept {
    const float l_ = c.c0 + 0.3963377774f * c.c1 + 0.2158037573f * c.c2;
    const float m_ = c.c0 - 0.1055613458f * c.c1 - 0.0638541728f * c.c2;
    const float s_ = c.c0 - 0.0894841775f * c.c1 - 1.2914855480f * c.c2;

    const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;

    return {num::saturate( 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s),
            num::saturate(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s),
            num::saturate(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s),
            c.a};
}

[[nodiscard]] constexpr Color<Oklch> oklab_to_oklch(Color<Oklab> c) noexcept {
    const float chroma = num::sqrt(c.c1 * c.c1 + c.c2 * c.c2);
    float hue = num::degrees(num::atan2(c.c2, c.c1));
    if (hue < 0.0f) hue += 360.0f;
    return {c.c0, chroma, hue, c.a};
}

[[nodiscard]] constexpr Color<Oklab> oklch_to_oklab(Color<Oklch> c) noexcept {
    const float h = num::radians(c.c2);
    return {c.c0, c.c1 * num::cos(h), c.c1 * num::sin(h), c.a};
}

}  // namespace detail

template <ColorSpace Space>
template <ColorSpace To>
constexpr Color<To> Color<Space>::to() const noexcept {
    if constexpr (std::same_as<Space, To>) {
        return *this;
    }
    // ---- from Srgb ----
    else if constexpr (std::same_as<Space, Srgb>) {
        const auto lin = detail::srgb_to_linear(*this);
        if constexpr (std::same_as<To, Linear>) return lin;
        else                                    return lin.template to<To>();
    }
    // ---- from Linear ----
    else if constexpr (std::same_as<Space, Linear>) {
        if constexpr (std::same_as<To, Srgb>)  return detail::linear_to_srgb(*this);
        else if constexpr (std::same_as<To, Oklab>) return detail::linear_to_oklab(*this);
        else /* Oklch */ return detail::oklab_to_oklch(detail::linear_to_oklab(*this));
    }
    // ---- from Oklab ----
    else if constexpr (std::same_as<Space, Oklab>) {
        if constexpr (std::same_as<To, Oklch>) return detail::oklab_to_oklch(*this);
        else return detail::oklab_to_linear(*this).template to<To>();
    }
    // ---- from Oklch ----
    else {
        const auto lab = detail::oklch_to_oklab(*this);
        if constexpr (std::same_as<To, Oklab>) return lab;
        else return lab.template to<To>();
    }
}

// ── literals & constructors ─────────────────────────────────────────────

/// Hex literal: `rgb<0x5B8CFF>`. A compile-time constant with no parsing at
/// runtime and no chance of a typo'd channel order.
template <std::uint32_t Hex>
inline constexpr Color<Srgb> rgb{
    static_cast<float>((Hex >> 16) & 0xFF) / 255.0f,
    static_cast<float>((Hex >>  8) & 0xFF) / 255.0f,
    static_cast<float>( Hex        & 0xFF) / 255.0f,
    1.0f};

/// Hex with alpha: `rgba<0x5B8CFF80>`.
template <std::uint32_t Hex>
inline constexpr Color<Srgb> rgba{
    static_cast<float>((Hex >> 24) & 0xFF) / 255.0f,
    static_cast<float>((Hex >> 16) & 0xFF) / 255.0f,
    static_cast<float>((Hex >>  8) & 0xFF) / 255.0f,
    static_cast<float>( Hex        & 0xFF) / 255.0f};

[[nodiscard]] constexpr Color<Srgb> rgb8(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                         std::uint8_t alpha = 255) noexcept {
    return {r / 255.0f, g / 255.0f, b / 255.0f, alpha / 255.0f};
}

/// Perceptual constructor: lightness 0..1, chroma 0..~0.37, hue degrees.
[[nodiscard]] constexpr Color<Oklch> oklch(float l, float c, float h, float alpha = 1.0f) noexcept {
    return {l, c, h, alpha};
}

[[nodiscard]] constexpr Color<Srgb> gray(float v, float alpha = 1.0f) noexcept {
    return {v, v, v, alpha};
}

// ── named colours ───────────────────────────────────────────────────────

namespace colors {
inline constexpr Color<Srgb> transparent{0, 0, 0, 0};
inline constexpr auto black   = rgb<0x000000>;
inline constexpr auto white   = rgb<0xFFFFFF>;
inline constexpr auto red     = rgb<0xE5484D>;
inline constexpr auto orange  = rgb<0xF76B15>;
inline constexpr auto amber   = rgb<0xFFB224>;
inline constexpr auto yellow  = rgb<0xF5D90A>;
inline constexpr auto lime    = rgb<0x99D52A>;
inline constexpr auto green   = rgb<0x30A46C>;
inline constexpr auto teal    = rgb<0x12A594>;
inline constexpr auto cyan    = rgb<0x00A2C7>;
inline constexpr auto blue    = rgb<0x0090FF>;
inline constexpr auto indigo  = rgb<0x3E63DD>;
inline constexpr auto violet  = rgb<0x6E56CF>;
inline constexpr auto purple  = rgb<0x8E4EC6>;
inline constexpr auto pink    = rgb<0xD6409F>;
inline constexpr auto slate   = rgb<0x8B8D98>;
}  // namespace colors

// ── blending & interpolation ────────────────────────────────────────────

/// Source-over compositing. Defined ONLY on Linear — this is the type system
/// enforcing that you cannot alpha-blend gamma-encoded values, which is the
/// classic cause of dark halos around antialiased text.
[[nodiscard]] constexpr Color<Linear> over(Color<Linear> src, Color<Linear> dst) noexcept {
    const float sa = num::saturate(src.a);
    const float out_a = sa + dst.a * (1.0f - sa);
    if (out_a <= 0.0f) return {};
    const float inv = 1.0f / out_a;
    return {(src.c0 * sa + dst.c0 * dst.a * (1.0f - sa)) * inv,
            (src.c1 * sa + dst.c1 * dst.a * (1.0f - sa)) * inv,
            (src.c2 * sa + dst.c2 * dst.a * (1.0f - sa)) * inv,
            out_a};
}

/// Component-wise mix in whatever space the colours already are.
template <ColorSpace S>
[[nodiscard]] constexpr Color<S> mix(Color<S> x, Color<S> y, float t) noexcept {
    return {num::lerp(x.c0, y.c0, t), num::lerp(x.c1, y.c1, t),
            num::lerp(x.c2, y.c2, t), num::lerp(x.a, y.a, t)};
}

/// Hue-aware mix for Oklch: takes the SHORT way around the colour wheel, so
/// red -> magenta does not detour through green.
template <>
[[nodiscard]] constexpr Color<Oklch> mix<Oklch>(Color<Oklch> x, Color<Oklch> y, float t) noexcept {
    float dh = y.c2 - x.c2;
    if (dh >  180.0f) dh -= 360.0f;
    if (dh < -180.0f) dh += 360.0f;
    float h = x.c2 + dh * t;
    if (h < 0.0f)     h += 360.0f;
    if (h >= 360.0f)  h -= 360.0f;
    return {num::lerp(x.c0, y.c0, t), num::lerp(x.c1, y.c1, t), h, num::lerp(x.a, y.a, t)};
}

/// Perceptual mix between colours in ANY spaces: routes through Oklch and
/// returns in the source space. This is what gradients should use.
template <ColorSpace A, ColorSpace B>
[[nodiscard]] constexpr Color<A> mix_perceptual(Color<A> x, Color<B> y, float t) noexcept {
    return mix(x.template to<Oklch>(), y.template to<Oklch>(), t).template to<A>();
}

// ── perceptual manipulation ─────────────────────────────────────────────

/// Lighten/darken by a perceptual amount. Unlike an RGB scale this keeps hue
/// and saturation stable, so a "hover" variant of a brand colour still reads
/// as the brand colour.
template <ColorSpace S>
[[nodiscard]] constexpr Color<S> lighten(Color<S> c, float amount) noexcept {
    auto lch = c.template to<Oklch>();
    lch.c0 = num::saturate(lch.c0 + amount);
    return lch.template to<S>();
}

template <ColorSpace S>
[[nodiscard]] constexpr Color<S> darken(Color<S> c, float amount) noexcept {
    return lighten(c, -amount);
}

template <ColorSpace S>
[[nodiscard]] constexpr Color<S> saturate_by(Color<S> c, float factor) noexcept {
    auto lch = c.template to<Oklch>();
    lch.c1 = num::max(lch.c1 * factor, 0.0f);
    return lch.template to<S>();
}

template <ColorSpace S>
[[nodiscard]] constexpr Color<S> rotate_hue(Color<S> c, float degrees) noexcept {
    auto lch = c.template to<Oklch>();
    lch.c2 = num::mod(lch.c2 + degrees, 360.0f);
    return lch.template to<S>();
}

/// Relative luminance (WCAG), computed correctly from linear light.
template <ColorSpace S>
[[nodiscard]] constexpr float luminance(Color<S> c) noexcept {
    const auto l = c.template to<Linear>();
    return 0.2126f * l.c0 + 0.7152f * l.c1 + 0.0722f * l.c2;
}

/// WCAG 2.1 contrast ratio, 1.0 .. 21.0.
template <ColorSpace A, ColorSpace B>
[[nodiscard]] constexpr float contrast_ratio(Color<A> x, Color<B> y) noexcept {
    const float l1 = luminance(x), l2 = luminance(y);
    const float hi = num::max(l1, l2), lo = num::min(l1, l2);
    return (hi + 0.05f) / (lo + 0.05f);
}

/// Pick whichever of black/white is legible on `bg`. Used by the theme so a
/// generated accent colour always gets readable text on top.
template <ColorSpace S>
[[nodiscard]] constexpr Color<Srgb> readable_on(Color<S> bg) noexcept {
    return luminance(bg) > 0.36f ? colors::black : colors::white;
}

// ── packing for upload ──────────────────────────────────────────────────

/// Pack to 0xAABBGGRR — the byte order every GPU API calls RGBA8_UNORM on a
/// little-endian host, so an array of these uploads with zero swizzling.
[[nodiscard]] constexpr std::uint32_t pack_rgba8(Color<Srgb> c) noexcept {
    const auto q = [](float v) -> std::uint32_t {
        return static_cast<std::uint32_t>(num::saturate(v) * 255.0f + 0.5f);
    };
    return q(c.c0) | (q(c.c1) << 8) | (q(c.c2) << 16) | (q(c.a) << 24);
}

// ── compile-time sanity ─────────────────────────────────────────────────

namespace detail {
constexpr bool near(float x, float y, float eps = 2e-3f) { return num::abs(x - y) <= eps; }

// Round-trip through every space must be lossless to display precision.
constexpr auto probe = rgb<0x5B8CFF>;
static_assert(near(probe.to<Linear>().to<Srgb>().c0, probe.c0));
static_assert(near(probe.to<Oklab>().to<Srgb>().c1, probe.c1));
static_assert(near(probe.to<Oklch>().to<Srgb>().c2, probe.c2));

// Mid-grey in sRGB is ~21.4% linear light, not 50%. If this ever reads 0.5
// the transfer function has been replaced by a naive gamma.
static_assert(near(gray(0.5f).to<Linear>().c0, 0.2140f, 1e-3f));

static_assert(pack_rgba8(colors::white) == 0xFFFFFFFF);
static_assert(pack_rgba8(colors::black) == 0xFF000000);
static_assert(contrast_ratio(colors::black, colors::white) > 20.9f);
static_assert(luminance(colors::white) > 0.99f);

// The whole point of Oklch mixing: red -> blue must not pass through grey.
constexpr auto muddy = mix(colors::red.to<Linear>(), colors::blue.to<Linear>(), 0.5f);
constexpr auto vivid = mix_perceptual(colors::red, colors::blue, 0.5f);
static_assert(vivid.to<Oklch>().c1 > muddy.to<Oklch>().c1);
}  // namespace detail

}  // namespace mayag
