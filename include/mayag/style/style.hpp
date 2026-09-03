#pragma once
// mayag::style — the paint description
//
// A Style is a plain literal aggregate: no pointers, no allocations, fully
// constexpr-constructible. That is what lets the DSL fold an entire widget
// tree at compile time and lets the renderer memcpy style data straight into
// a GPU instance buffer.
//
// Everything here is expressed the way a GPU wants it (SDF parameters, packed
// vectors, premultiplied intent) rather than the way a CPU rasteriser wants it
// (paths, scanlines). A rounded box with a border, an inner shadow and a
// gradient fill is ONE quad and ONE instance — not four draw calls.

#include "../core/color.hpp"
#include "../core/geometry.hpp"

#include <array>
#include <cstdint>

namespace mayag {

// ── fills ───────────────────────────────────────────────────────────────

enum class FillKind : std::uint8_t {
    none,
    solid,
    linear_gradient,
    radial_gradient,
    angular_gradient,   ///< conic sweep — loading spinners, hue wheels
};

/// Gradients interpolate in Oklch by default. That is not a stylistic choice:
/// RGB interpolation between complements passes through desaturated grey, and
/// every "why does my blue-to-orange gradient look muddy in the middle" bug
/// is that. `interpolate_srgb` exists only for matching legacy designs.
inline constexpr std::size_t max_gradient_stops = 8;

struct GradientStop {
    float        position = 0.0f;   ///< 0..1 along the gradient axis
    Color<Srgb>  color{};
    friend constexpr bool operator==(const GradientStop&, const GradientStop&) = default;
};

struct Fill {
    FillKind    kind = FillKind::none;
    Color<Srgb> color{};             ///< solid fill

    std::array<GradientStop, max_gradient_stops> stops{};
    std::uint8_t stop_count = 0;

    /// Gradient geometry, in normalised box coordinates (0,0 = top-left,
    /// 1,1 = bottom-right) so a gradient survives layout resizing.
    Vec2  from{0.0f, 0.0f};
    Vec2  to{0.0f, 1.0f};
    float radius = 0.5f;             ///< radial/angular extent
    bool  interpolate_srgb = false;  ///< opt out of perceptual interpolation

    friend constexpr bool operator==(const Fill&, const Fill&) = default;

    [[nodiscard]] constexpr bool visible() const noexcept {
        if (kind == FillKind::none) return false;
        if (kind == FillKind::solid) return color.a > 0.0f;
        return stop_count > 0;
    }

    /// Evaluate the fill at a normalised parameter t along its axis.
    /// Shared by the software rasteriser and the shader codegen, so the two
    /// paths cannot drift.
    [[nodiscard]] constexpr Color<Srgb> sample(float t) const noexcept {
        if (kind == FillKind::solid || stop_count == 0) return color;
        t = num::saturate(t);

        if (t <= stops[0].position) return stops[0].color;
        const std::size_t n = stop_count;
        if (t >= stops[n - 1].position) return stops[n - 1].color;

        for (std::size_t i = 1; i < n; ++i) {
            if (t <= stops[i].position) {
                const auto& lo = stops[i - 1];
                const auto& hi = stops[i];
                const float local = num::unlerp(lo.position, hi.position, t);
                return interpolate_srgb ? mix(lo.color, hi.color, local)
                                        : mix_perceptual(lo.color, hi.color, local);
            }
        }
        return stops[n - 1].color;
    }
};

[[nodiscard]] constexpr Fill solid_fill(Color<Srgb> c) noexcept {
    return Fill{.kind = FillKind::solid, .color = c};
}

// ── strokes ─────────────────────────────────────────────────────────────

/// Where the stroke sits relative to the shape boundary. `inside` is the
/// default because it is the only alignment that never grows a widget's
/// visual bounds past its layout bounds — borders that overflow their box are
/// the source of most "why is there a 1px gap in my grid" reports.
enum class StrokeAlign : std::uint8_t { inside, center, outside };

struct Stroke {
    float        width = 0.0f;
    Color<Srgb>  color{};
    StrokeAlign  align = StrokeAlign::inside;

    /// Dash pattern in pixels: 0 length means solid.
    float dash_length = 0.0f;
    float dash_gap    = 0.0f;

    friend constexpr bool operator==(const Stroke&, const Stroke&) = default;
    [[nodiscard]] constexpr bool visible() const noexcept { return width > 0.0f && color.a > 0.0f; }
    [[nodiscard]] constexpr bool dashed()  const noexcept { return dash_length > 0.0f; }
};

// ── shadows ─────────────────────────────────────────────────────────────

/// A shadow is a second SDF evaluation of the same rounded box, offset and
/// blurred with an analytic Gaussian approximation. No blur passes, no
/// offscreen targets, no downsample chain: one extra instance.
struct Shadow {
    Vec2        offset{0.0f, 2.0f};
    float       blur   = 0.0f;
    float       spread = 0.0f;
    Color<Srgb> color  = rgba<0x00000040>;
    bool        inset  = false;   ///< inner shadow (pressed / inlaid look)

    friend constexpr bool operator==(const Shadow&, const Shadow&) = default;
    [[nodiscard]] constexpr bool visible() const noexcept {
        return color.a > 0.0f && (blur > 0.0f || spread > 0.0f ||
                                  offset.x != 0.0f || offset.y != 0.0f);
    }

    /// Bounds growth caused by this shadow, so the renderer can expand the
    /// instance quad enough that the blur is never clipped.
    [[nodiscard]] constexpr Insets bounds_growth() const noexcept {
        if (inset || !visible()) return Insets{};
        const float r = blur + spread;
        return Insets{num::max(r - offset.y, 0.0f), num::max(r + offset.x, 0.0f),
                      num::max(r + offset.y, 0.0f), num::max(r - offset.x, 0.0f)};
    }
};

inline constexpr std::size_t max_shadows = 4;

// ── text ────────────────────────────────────────────────────────────────

enum class FontWeight : std::uint16_t {
    thin = 100, extra_light = 200, light = 300, regular = 400,
    medium = 500, semi_bold = 600, bold = 700, extra_bold = 800, black = 900,
};

enum class TextAlign : std::uint8_t { left, center, right, justify };
enum class TextOverflow : std::uint8_t { clip, ellipsis, wrap };

struct TextStyle {
    float        size        = 16.0f;
    FontWeight   weight      = FontWeight::regular;
    bool         italic      = false;
    bool         underline   = false;
    bool         strikethrough = false;
    Color<Srgb>  color       = colors::white;
    float        line_height = 1.4f;    ///< multiple of size
    float        letter_spacing = 0.0f;
    TextAlign    align       = TextAlign::left;
    TextOverflow overflow    = TextOverflow::wrap;

    friend constexpr bool operator==(const TextStyle&, const TextStyle&) = default;
    [[nodiscard]] constexpr float line_advance() const noexcept { return size * line_height; }
};

// ── effects ─────────────────────────────────────────────────────────────

/// Backdrop blur ("frosted glass"). Distinct from shadow blur: this samples
/// what is already in the framebuffer behind the widget.
struct Backdrop {
    float blur       = 0.0f;
    float saturation = 1.0f;
    float brightness = 1.0f;
    friend constexpr bool operator==(const Backdrop&, const Backdrop&) = default;
    [[nodiscard]] constexpr bool active() const noexcept {
        return blur > 0.0f || saturation != 1.0f || brightness != 1.0f;
    }
};

enum class BlendMode : std::uint8_t {
    normal, multiply, screen, overlay, additive, subtract, difference,
};

// ── layout ──────────────────────────────────────────────────────────────

enum class Axis : std::uint8_t { horizontal, vertical };

enum class Justify : std::uint8_t {
    start, center, end, space_between, space_around, space_evenly,
};

enum class Align : std::uint8_t { start, center, end, stretch, baseline };

enum class Positioning : std::uint8_t {
    flow,      ///< participates in the parent's flex layout
    absolute,  ///< positioned against the parent's padding box, out of flow
    fixed,     ///< positioned against the viewport
};

/// A length that can be absolute, a fraction of the parent, or content-derived.
/// Keeping this as one 8-byte literal type (instead of an optional<float> plus
/// flags) is what keeps LayoutStyle trivially copyable.
struct Length {
    enum class Unit : std::uint8_t { automatic, pixels, percent } unit = Unit::automatic;
    float value = 0.0f;

    friend constexpr bool operator==(Length, Length) = default;

    [[nodiscard]] constexpr bool is_auto() const noexcept { return unit == Unit::automatic; }

    /// Resolve against an available extent; `automatic` yields `fallback`.
    [[nodiscard]] constexpr float resolve(float available, float fallback) const noexcept {
        switch (unit) {
            case Unit::pixels:  return value;
            case Unit::percent: return available * value * 0.01f;
            case Unit::automatic: break;
        }
        return fallback;
    }
};

[[nodiscard]] constexpr Length px(float v) noexcept { return {Length::Unit::pixels, v}; }
[[nodiscard]] constexpr Length pct(float v) noexcept { return {Length::Unit::percent, v}; }
inline constexpr Length auto_length{};

struct LayoutStyle {
    Axis    axis    = Axis::vertical;
    Justify justify = Justify::start;
    /// CSS flexbox defaults `align-items` to `stretch`, and so does mayag.
    /// The alternative (`start`) collapses any auto-sized child to its
    /// intrinsic cross size, which makes a `box() | grow()` inside a column
    /// render as a zero-width sliver — technically consistent, but wrong
    /// often enough that it would be a permanent papercut.
    Align   align   = Align::stretch;
    float   gap     = 0.0f;

    Insets padding{};
    Insets margin{};

    Length width{};
    Length height{};
    Length min_width{};
    Length min_height{};
    Length max_width{};
    Length max_height{};

    float grow   = 0.0f;   ///< share of leftover main-axis space
    float shrink = 1.0f;   ///< share of overflow to absorb
    bool  wrap   = false;

    Positioning position = Positioning::flow;
    Vec2        offset{};  ///< used by absolute/fixed, and as a paint nudge in flow

    friend constexpr bool operator==(const LayoutStyle&, const LayoutStyle&) = default;
};

// ── the composite style ─────────────────────────────────────────────────

struct Style {
    LayoutStyle layout{};

    Fill    fill{};
    Stroke  stroke{};
    Corners corners{};

    std::array<Shadow, max_shadows> shadows{};
    std::uint8_t shadow_count = 0;

    TextStyle text{};
    Backdrop  backdrop{};
    BlendMode blend = BlendMode::normal;

    float   opacity = 1.0f;
    bool    clip    = false;    ///< clip children to this box's rounded rect
    Affine  transform = Affine::identity();

    /// Stable identity for hit-testing and animation matching. Zero means
    /// "anonymous"; the DSL assigns one when you name a node.
    std::uint64_t id = 0;

    friend constexpr bool operator==(const Style&, const Style&) = default;

    [[nodiscard]] constexpr bool paints_anything() const noexcept {
        return fill.visible() || stroke.visible() || shadow_count > 0 || backdrop.active();
    }

    /// How far this node's paint extends beyond its layout rect.
    [[nodiscard]] constexpr Insets paint_overflow() const noexcept {
        Insets acc{};
        for (std::uint8_t i = 0; i < shadow_count; ++i) {
            const Insets g = shadows[i].bounds_growth();
            acc = Insets{num::max(acc.top, g.top), num::max(acc.right, g.right),
                         num::max(acc.bottom, g.bottom), num::max(acc.left, g.left)};
        }
        if (stroke.align == StrokeAlign::outside) {
            acc = acc + Insets{stroke.width};
        } else if (stroke.align == StrokeAlign::center) {
            acc = acc + Insets{stroke.width * 0.5f};
        }
        return acc;
    }
};

// ── compile-time sanity ─────────────────────────────────────────────────

static_assert(std::is_trivially_copyable_v<Style>);
static_assert(px(10).resolve(200.0f, 0.0f) == 10.0f);
static_assert(pct(50).resolve(200.0f, 0.0f) == 100.0f);
static_assert(auto_length.resolve(200.0f, 7.0f) == 7.0f);
static_assert(solid_fill(colors::red).visible());
static_assert(!Fill{}.visible());

}  // namespace mayag
