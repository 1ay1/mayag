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
#include "../core/scroll_state.hpp"
#include "../scene/a11y_types.hpp"
#include "../platform/types.hpp"

#include <array>
#include <cstdint>
#include <string>

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

/// Material-style elevation as two shadows (key + ambient), from one dp.
/// The ONE definition: `elevation(8)` and `elevation(var::depth(8))` both
/// expand through it, so a literal and a token draw identical pixels.
[[nodiscard]] constexpr Shadow elevation_shadow(float dp, int layer) noexcept {
    if (layer == 0) {
        const float a1 = num::min(0.06f + dp * 0.006f, 0.20f);
        return Shadow{.offset = {0.0f, dp * 0.5f}, .blur = dp * 1.4f, .color = colors::black.fade(a1)};
    }
    const float a2 = num::min(0.10f + dp * 0.010f, 0.36f);
    return Shadow{.offset = {0.0f, dp * 0.15f}, .blur = dp * 0.4f, .color = colors::black.fade(a2)};
}

// ── text ────────────────────────────────────────────────────────────────

enum class FontWeight : std::uint16_t {
    thin = 100, extra_light = 200, light = 300, regular = 400,
    medium = 500, semi_bold = 600, bold = 700, extra_bold = 800, black = 900,
};

enum class TextAlign : std::uint8_t { left, center, right, justify };
enum class TextOverflow : std::uint8_t { clip, ellipsis, wrap };

/// Continuous variable-font axis settings carried on a TextStyle.
///
/// A variable font is a continuous design space; `FontWeight` only names nine
/// points on the `wght` axis. This lets a style request ANY axis at ANY value
/// — wght 372, opsz 28, a custom axis — while staying a small, trivially
/// comparable value (so TextStyle change-detection keeps working). Unset axes
/// fall back to the FontWeight enum (for wght) or the font's default.
struct FontVariations {
    /// One (tag, value) override in USER units. tag is a big-endian 4CC.
    struct Axis {
        std::uint32_t tag = 0;
        float value = 0.0f;
        friend constexpr bool operator==(const Axis&, const Axis&) = default;
    };

    static constexpr std::size_t max_axes = 4;
    std::array<Axis, max_axes> axes{};
    std::uint8_t count = 0;
    /// Numeric wght override (0 = none => use the FontWeight enum instead).
    float wght = 0.0f;

    friend constexpr bool operator==(const FontVariations&, const FontVariations&) = default;

    [[nodiscard]] constexpr bool empty() const noexcept { return count == 0 && wght == 0.0f; }

    /// Set an axis (replacing any existing entry with the same tag). Silently
    /// drops the request past max_axes — four covers wght+wdth+opsz+slnt.
    constexpr void set(std::uint32_t tag, float value) noexcept {
        for (std::uint8_t i = 0; i < count; ++i)
            if (axes[i].tag == tag) { axes[i].value = value; return; }
        if (count < max_axes) axes[count++] = Axis{tag, value};
    }

    [[nodiscard]] constexpr bool has(std::uint32_t tag) const noexcept {
        for (std::uint8_t i = 0; i < count; ++i)
            if (axes[i].tag == tag) return true;
        return false;
    }

    /// 'opsz', the optical-size axis.
    static constexpr std::uint32_t tag_opsz = 0x6F70737A;

    /// Pin opsz to `logical_size` unless the style already chose one.
    ///
    /// This is what CoreText and every browser do (`font-optical-sizing:
    /// auto`): a variable font with an optical-size axis is drawn with the
    /// design meant for the size it is shown at. Without it, SF and its kin
    /// render every UI size with their DEFAULT instance -- for SF that is the
    /// opsz-28 "Display" cut, whose narrow letterforms and tight spacing are
    /// drawn for headlines, so 12 px body text came out cramped and thin.
    /// LOGICAL size, never device pixels: 13 pt on a 2x display is still
    /// 13 pt type, and must not switch to the 26 pt design. Rounded to whole
    /// points: the design does not visibly change within one, and every
    /// distinct value is its own instance (and its own glyph-cache entries).
    constexpr void auto_optical_size(float logical_size) noexcept {
        if (!has(tag_opsz)) set(tag_opsz, static_cast<float>(static_cast<int>(logical_size + 0.5f)));
    }
};

/// Which TextStyle fields a style SET, as opposed to left at their defaults.
///
/// This is what makes text properties inherit: a container that says
/// `| fg(c) | font(13)` sets those bits on its own TextStyle, and the cascade
/// copies every field whose bit is set down to descendants that did not set
/// it themselves. Bits, not optionals, so TextStyle stays a literal type.
namespace text_bits {
inline constexpr std::uint16_t size        = 1u << 0;
inline constexpr std::uint16_t weight      = 1u << 1;
inline constexpr std::uint16_t italic      = 1u << 2;
inline constexpr std::uint16_t underline   = 1u << 3;
inline constexpr std::uint16_t strike      = 1u << 4;
inline constexpr std::uint16_t color       = 1u << 5;
inline constexpr std::uint16_t line_height = 1u << 6;
inline constexpr std::uint16_t tracking    = 1u << 7;
inline constexpr std::uint16_t align       = 1u << 8;
inline constexpr std::uint16_t ligatures   = 1u << 9;
inline constexpr std::uint16_t variations  = 1u << 10;
inline constexpr std::uint16_t all         = (1u << 11) - 1u;
}  // namespace text_bits

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

    /// DEFAULT IS ELLIPSIS, not wrap.
    ///
    /// Wrapping is right for paragraphs and wrong for everything else, and
    /// UIs are mostly everything else: labels, buttons, chips, table cells,
    /// menu entries. A wrapping label in a tight row does not degrade
    /// gracefully — it becomes a vertical ribbon of letters that looks like
    /// the text engine failed, when the real fault is upstream sizing.
    ///
    /// So the safe behaviour is the default and paragraphs opt IN with
    /// `| wrap_text`. Every label in mayag's own examples that needed fixing
    /// needed exactly this, which is the signal that the default was wrong.
    TextOverflow overflow    = TextOverflow::ellipsis;

    /// Continuous variable-font axis overrides. Empty by default — a static
    /// font ignores this, and a variable font uses `weight` (the enum) unless
    /// an axis here overrides it.
    FontVariations variations{};

    /// GSUB `liga` ligatures (fi, ffi, ...). On by default — the correct
    /// typographic default; turn off for code/monospace where each character
    /// must stay distinct.
    bool ligatures = true;

    /// Clamp wrapped text to at most this many lines, ellipsising the last one
    /// (CSS line-clamp). 0 = unlimited. Only meaningful with overflow == wrap.
    std::uint16_t max_lines = 0;

    /// `text_bits` this style set explicitly (see above). Overflow and
    /// line clamping are deliberately NOT inheritable: an inherited `wrap`
    /// is exactly how a label turns into a vertical ribbon of letters.
    std::uint16_t set = 0;

    friend constexpr bool operator==(const TextStyle&, const TextStyle&) = default;
    [[nodiscard]] constexpr float line_advance() const noexcept { return size * line_height; }

    /// The wght value to drive a variable font with: the numeric override if
    /// set, otherwise the numeric value of the FontWeight enum (100..900).
    [[nodiscard]] constexpr float effective_weight() const noexcept {
        return variations.wght != 0.0f ? variations.wght
                                       : static_cast<float>(weight);
    }
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

/// Animate changes to the resolved style instead of jumping.
///
/// A spring, not a duration: a hover that ends halfway through its fade-in
/// reverses from where it is, with its velocity, instead of restarting.
/// `props` selects what moves (`transition_props`); 0 means no transition.
namespace transition_props {
inline constexpr std::uint8_t color     = 1u << 0;   ///< fill, border, text and shadow colours
inline constexpr std::uint8_t shape     = 1u << 1;   ///< border width, radii, shadow geometry, backdrop blur
inline constexpr std::uint8_t opacity   = 1u << 2;
inline constexpr std::uint8_t transform = 1u << 3;   ///< transform and offset
inline constexpr std::uint8_t all       = color | shape | opacity | transform;
}  // namespace transition_props

struct Transition {
    float        stiffness = 0.0f;
    float        damping   = 0.0f;
    std::uint8_t props     = 0;
    /// 0: the spring above. 1..3: the theme's motion_ui / emphasis / gentle,
    /// resolved by the cascade (and scaled by the theme's motion_scale).
    std::uint8_t motion    = 0;
    friend constexpr bool operator==(const Transition&, const Transition&) = default;
    [[nodiscard]] constexpr bool on() const noexcept {
        return props != 0 && (stiffness > 0.0f || motion != 0);
    }
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
    /// Share of overflow this node absorbs.
    ///
    /// DEFAULT IS ZERO, unlike CSS. This is a deliberate divergence.
    ///
    /// With CSS's `flex-shrink: 1` default an explicit `width(196)` is only
    /// a SUGGESTION: place it beside a sibling with wide content and the
    /// sidebar silently becomes 147 px, its labels wrap to one character per
    /// line, and the UI looks broken in a way that points at the wrong file.
    /// That is not hypothetical — it is exactly what happened to mayag's own
    /// dashboard example.
    ///
    /// CSS authors have learned to type `flex-shrink: 0` reflexively. A DSL
    /// that claims layout correctness should not need a ritual to get the
    /// obvious behaviour, so in mayag a size you ASK for is a size you GET,
    /// and shrinking is opt-in via `| shrink()` or `| flexible()`.
    float shrink = 0.0f;
    bool  wrap   = false;

    /// When non-null this node is a SCROLL VIEWPORT: children are laid out
    /// at their natural size, then translated by `-offset`, and the state's
    /// limits are filled in from the measured content.
    ///
    /// A raw pointer into the caller's Model rather than owned state — the
    /// scroll position belongs to the application, so it can be saved,
    /// restored and asserted on. See core/scroll_state.hpp.
    const ScrollState* scroll = nullptr;

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
    /// Applied about the box's centre at paint time (CSS `transform`). Moves
    /// pixels, never layout: siblings do not reflow around a scaled button.
    Affine  transform = Affine::identity();

    // ── cascade ─────────────────────────────────────────────────────────
    //
    // Inputs to style/cascade.hpp. They describe how this style RESPONDS
    // (to state, to time); the painter never reads them.

    /// Descendants may style on this node's state: `state::group(state::hover)`.
    bool          group = false;
    /// The id was synthesised by the cascade so the node can be HOVERED
    /// (it has pointer variants but no `id<>`). Hit-testable, never a Tab stop.
    bool          hit_only = false;
    /// Eight app-defined states, matched by `st::flag<N>`.
    std::uint8_t  flags = 0;
    Transition    transition{};

    // ── accessibility ───────────────────────────────────────────────────
    //
    // Carried on Style so any element can be annotated with the same pipe
    // syntax as everything else. Most nodes need nothing: a text node is
    // announced as its own text, a named leaf as a button. Only ambiguous
    // cases pay.
    a11y::Role  a11y_role = a11y::Role::none;
    std::string a11y_label;
    std::string a11y_description;
    a11y::State a11y_state{};

    /// Pointer shape while hovering this node.
    ///
    /// `arrow` means "inherit": a nested label inside a button should not
    /// reset the cursor the button set. Only nodes that mean something
    /// different override — a text field wants an I-beam, a link a pointer,
    /// a resize handle a double arrow.
    CursorShape cursor = CursorShape::arrow;

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

// Style is no longer trivially copyable: accessibility labels are strings.
//
// That is a deliberate trade. The alternative — interning labels into a table
// and storing an index — keeps Style a POD but makes the common case
// (`| label("Save")`) require a registry, a lifetime, and a lookup on every
// query. Labels are set once per node per frame and read by nothing on the
// hot path, so the copy is not where time goes; the INSTANCE struct that
// reaches the GPU is still trivially copyable, and that is the one that
// matters for upload.
static_assert(std::is_copy_constructible_v<Style>);
static_assert(std::is_trivially_copyable_v<LayoutStyle>);
static_assert(px(10).resolve(200.0f, 0.0f) == 10.0f);
static_assert(pct(50).resolve(200.0f, 0.0f) == 100.0f);
static_assert(auto_length.resolve(200.0f, 7.0f) == 7.0f);
static_assert(solid_fill(colors::red).visible());
static_assert(!Fill{}.visible());

/// A Style behind one owning pointer, null meaning the default Style.
///
/// Elements and nodes hold their style this way so that moving them (every
/// pipe stage, every child append, every vector growth) moves 8 bytes, not
/// ~800, and an element's style becomes its node's without a copy.
class StyleBox {
    // Style blocks are 800 bytes and every styled node gets one each frame,
    // so freed blocks are kept on a small per-thread list and reused instead
    // of going back to malloc. Plain new/delete during constant evaluation.
    struct Pool {
        static constexpr int kMax = 4096;
        void* free_[kMax];
        int   n = 0;
        ~Pool() { while (n > 0) ::operator delete(free_[--n]); }
    };
    static Pool& pool() noexcept {
        static thread_local Pool p;
        return p;
    }
    template <class... A>
    static constexpr Style* make(A&&... a) {
        if consteval { return new Style(std::forward<A>(a)...); }
        Pool& p = pool();
        void* mem = p.n > 0 ? p.free_[--p.n] : ::operator new(sizeof(Style));
        return ::new (mem) Style(std::forward<A>(a)...);
    }
    static constexpr void drop(Style* s) noexcept {
        if (s == nullptr) return;
        if consteval { delete s; return; }
        s->~Style();
        Pool& p = pool();
        if (p.n < Pool::kMax) p.free_[p.n++] = s;
        else ::operator delete(s);
    }

  public:
    constexpr StyleBox() noexcept = default;
    constexpr StyleBox(const Style& s) : p_{make(s)} {}
    constexpr StyleBox(Style&& s) : p_{make(std::move(s))} {}
    constexpr StyleBox(const StyleBox& o) : p_{o.p_ ? make(*o.p_) : nullptr} {}
    constexpr StyleBox(StyleBox&& o) noexcept : p_{o.p_} { o.p_ = nullptr; }
    constexpr StyleBox& operator=(const StyleBox& o) {
        if (this == &o) return *this;
        if (o.p_ == nullptr) reset();
        else if (p_ != nullptr) *p_ = *o.p_;
        else p_ = make(*o.p_);
        return *this;
    }
    constexpr StyleBox& operator=(StyleBox&& o) noexcept {
        if (this != &o) { drop(p_); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    constexpr StyleBox& operator=(const Style& s) {
        if (p_ != nullptr) *p_ = s; else p_ = make(s);
        return *this;
    }
    constexpr StyleBox& operator=(Style&& s) {
        if (p_ != nullptr) *p_ = std::move(s); else p_ = make(std::move(s));
        return *this;
    }
    constexpr ~StyleBox() { drop(p_); }

    [[nodiscard]] const Style& get() const noexcept { return p_ != nullptr ? *p_ : fallback(); }
    /// Writable style, allocated on first write.
    [[nodiscard]] constexpr Style& mut() {
        if (p_ == nullptr) p_ = make();
        return *p_;
    }
    [[nodiscard]] constexpr bool is_default() const noexcept { return p_ == nullptr || *p_ == fallback(); }
    constexpr void reset() noexcept { drop(p_); p_ = nullptr; }
    [[nodiscard]] constexpr bool allocated() const noexcept { return p_ != nullptr; }

    operator const Style&() const noexcept { return get(); }

    friend bool operator==(const StyleBox& a, const StyleBox& b) noexcept {
        return a.p_ == b.p_ || a.get() == b.get();
    }

  private:
    static const Style& fallback() noexcept {
        static const Style s{};
        return s;
    }
    Style* p_ = nullptr;
};

}  // namespace mayag
