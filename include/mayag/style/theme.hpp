#pragma once
// mayag::Theme — a palette that is generated, not typed
//
// You give mayag one accent colour and a mode. It derives the rest in Oklch,
// where "one step lighter" and "slightly less saturated" are actual
// operations rather than guesses. The result is a scale that stays consistent
// across hues — an amber theme and a violet theme have matching contrast.
//
// The whole thing is constexpr, so a theme costs zero bytes of startup work
// and can be `static_assert`ed against accessibility thresholds.
//
// A theme is EVERYTHING about the look, as one plain value:
//
//   colour   18 roles + on_* labels       var::accent, var::surface, ...
//   shape    radii, line widths           var::radius_md, var::line
//   space    the spacing unit             var::space(2)
//   type     the base size of a scale     var::font_sm, var::font_xl
//   depth    elevation, or flat           var::depth(6)
//   motion   three springs and a scale    transition(), var::motion_emphasis
//   sheet    every widget's look          Sheet (widget/sheet.hpp)
//
// Change any of it per app (AppConfig::theme, or `static Theme theme(const
// Model&)`), per subtree (`| theme(t)`), or derive a whole look from a few
// knobs with `derive({...})`.

#include "../core/animation.hpp"
#include "../core/color.hpp"
#include "style.hpp"

namespace mayag {

/// Widget looks; defined in widget/sheet.hpp. A theme only POINTS at one,
/// so the framework never depends on widgets (and Theme stays constexpr).
struct Sheet;

enum class Mode : std::uint8_t { dark, light };

/// Which glyph set icons are drawn from. `letters` needs no font at all
/// (a coloured letter mark); `nerd` uses Nerd Font devicons, for an app
/// whose font stack carries one (JetBrainsMono Nerd Font, ...).
enum class IconSet : std::uint8_t { letters, nerd };

struct Theme {
    Mode mode = Mode::dark;

    // Surfaces, from furthest back to nearest front.
    Color<Srgb> background{};
    Color<Srgb> surface{};
    Color<Srgb> surface_raised{};
    Color<Srgb> overlay{};

    // Content.
    Color<Srgb> text_primary{};
    Color<Srgb> text_secondary{};
    Color<Srgb> text_disabled{};

    // Lines.
    Color<Srgb> border{};
    Color<Srgb> border_strong{};
    Color<Srgb> focus_ring{};

    // Actions.
    Color<Srgb> accent{};
    Color<Srgb> accent_hover{};
    Color<Srgb> accent_active{};
    Color<Srgb> on_accent{};

    // Status.
    Color<Srgb> success{};
    Color<Srgb> warning{};
    Color<Srgb> danger{};
    Color<Srgb> info{};
    // Labels on status fills (a danger button's text).
    Color<Srgb> on_success{};
    Color<Srgb> on_warning{};
    Color<Srgb> on_danger{};

    // Metrics — a theme owns spacing and rounding too, so a component library
    // built on it rescales coherently.
    float radius_small  = 6.0f;
    float radius_medium = 10.0f;
    float radius_large  = 16.0f;
    float space_unit    = 8.0f;
    float font_size     = 14.0f;

    // Line widths: hairline borders, control outlines, focus rings.
    float line_hairline = 1.0f;
    float line_strong   = 1.5f;
    float line_focus    = 2.0f;

    /// Multiplies every `var::depth(dp)`. 0: a flat theme, no shadows.
    float depth_scale   = 1.0f;

    // Motion. `transition()` is motion_ui; `motion_scale` speeds every
    // spring up (>1) or down (<1); 0 turns transitions off (reduced motion).
    Spring motion_ui       = {400.0f, 40.0f};
    Spring motion_emphasis = {220.0f, 14.0f};
    Spring motion_gentle   = {120.0f, 22.0f};
    float  motion_scale    = 1.0f;

    /// Every widget's look. Null: the standard sheet. Point it at your own
    /// (usually a static) to restyle every widget at once.
    const Sheet* sheet = nullptr;

    /// Icon glyphs for files, folders and chrome (see IconSet).
    IconSet icons = IconSet::letters;

    friend constexpr bool operator==(const Theme&, const Theme&) = default;

    [[nodiscard]] constexpr float space(float multiple) const noexcept {
        return space_unit * multiple;
    }

    // ── theme-relative pixels ─────────────────────────────────────────────────────────────
    //
    // A widget designed at the standard theme's proportions writes its
    // pixels through these, and every theme knob reaches it: `t.rounded(4)`
    // is 4 px of corner at standard roundness, 0 in a square theme, 6.4 in
    // a soft one. Each is EXACTLY its argument in the standard theme (the
    // factor is a ratio of equal floats, i.e. 1.0f), so adopting them is
    // pixel-identical for every app that does not change the knob.

    /// Corner radius `px`, following roundness.
    [[nodiscard]] constexpr float rounded(float px) const noexcept { return px * (radius_small / 6.0f); }
    /// A pill/capsule end on something `height` tall: a full pill at
    /// standard roundness, square corners in a square theme. (A CIRCLE --
    /// an avatar, a status dot -- is a shape, not a corner style, and stays
    /// `radius(d / 2)`.)
    [[nodiscard]] constexpr float capsule(float height) const noexcept {
        return num::min(height * 0.5f, height * 0.5f * (radius_small / 6.0f));
    }
    /// Padding / gap `px`, following density.
    [[nodiscard]] constexpr float spaced(float px) const noexcept { return px * (space_unit / 8.0f); }
    /// Border width `px`, following line weight.
    [[nodiscard]] constexpr float stroke(float px) const noexcept { return px * line_hairline; }
    /// Elevation `dp`, following depth (0: flat themes draw no shadow).
    [[nodiscard]] constexpr float lift(float dp) const noexcept { return dp * depth_scale; }

    /// An OPAQUE wash of `c` over the background, `amount` of the way there in
    /// sRGB (perceptual) terms. For row highlights and diff lines.
    ///
    /// Not `c.fade(amount)`: translucent fills blend in linear light, which is
    /// physically right and perceptually lopsided -- on a near-black
    /// background 4% of a bright amber decodes to ~40/255, a loud band rather
    /// than a hint. Designers pick tints perceptually; so does this.
    [[nodiscard]] constexpr Color<Srgb> tint(Color<Srgb> c, float amount) const noexcept {
        return mix(background, c, amount);
    }
};

namespace detail {

/// Build a surface at a given perceptual lightness, tinted very slightly
/// toward the accent hue. That tint is the difference between a theme that
/// looks designed and one that looks like #1a1a1a.
constexpr Color<Srgb> surface_at(Color<Oklch> accent, float lightness, float chroma) noexcept {
    return Color<Oklch>{lightness, chroma, accent.c2, 1.0f}.to<Srgb>();
}

/// Find a foreground for `bg` that MEETS a contrast target, instead of merely
/// picking black or white and hoping. Walks Oklch lightness away from the
/// background in the more promising direction and stops at the first value
/// that clears `target`; falls back to pure black/white at the extremes.
///
/// This is what makes `make_theme()` safe for an arbitrary user accent: no
/// matter what hue someone passes in, the label on top of it stays legible.
constexpr Color<Srgb> contrasting_on(Color<Srgb> bg, float target,
                                     float chroma_keep = 0.0f) noexcept {
    const auto base = bg.to<Oklch>();
    // Prefer whichever direction has more headroom.
    const bool go_light = base.c0 < 0.55f;

    constexpr int steps = 48;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / steps;
        const float L = go_light ? num::lerp(base.c0, 1.0f, t)
                                 : num::lerp(base.c0, 0.0f, t);
        const auto candidate = Color<Oklch>{L, base.c1 * chroma_keep, base.c2, 1.0f}.to<Srgb>();
        if (contrast_ratio(candidate, bg) >= target) return candidate;
    }
    return go_light ? colors::white : colors::black;
}

}  // namespace detail

/// Derive a full theme from one accent colour.
namespace detail {
[[nodiscard]] constexpr Theme make_theme_uncached(Color<Srgb> accent_srgb, Mode mode) noexcept;
}

/// Pure in (accent, mode), and views call it every frame (theme knobs,
/// previews), so runtime calls go through a small per-thread memo.
[[nodiscard]] constexpr Theme make_theme(Color<Srgb> accent_srgb, Mode mode) noexcept {
    if consteval {
        return detail::make_theme_uncached(accent_srgb, mode);
    } else {
        struct Slot { Color<Srgb> accent; Mode mode; bool used = false; Theme theme; };
        static thread_local Slot slots[16];
        static thread_local unsigned next = 0;
        for (auto& s : slots)
            if (s.used && s.mode == mode && s.accent == accent_srgb) return s.theme;
        Slot& s = slots[next++ % 16];
        s = Slot{accent_srgb, mode, true, detail::make_theme_uncached(accent_srgb, mode)};
        return s.theme;
    }
}

[[nodiscard]] constexpr Theme detail::make_theme_uncached(Color<Srgb> accent_srgb, Mode mode) noexcept {
    const auto accent = accent_srgb.to<Oklch>();
    const bool dark = (mode == Mode::dark);

    Theme t{};
    t.mode = mode;

    if (dark) {
        t.background     = detail::surface_at(accent, 0.16f, 0.012f);
        t.surface        = detail::surface_at(accent, 0.21f, 0.014f);
        t.surface_raised = detail::surface_at(accent, 0.26f, 0.016f);
        t.overlay        = detail::surface_at(accent, 0.31f, 0.018f);
        t.text_primary   = detail::surface_at(accent, 0.97f, 0.004f);
        t.text_secondary = detail::surface_at(accent, 0.74f, 0.010f);
        t.text_disabled  = detail::surface_at(accent, 0.52f, 0.008f);
        t.border         = detail::surface_at(accent, 0.32f, 0.014f);
        t.border_strong  = detail::surface_at(accent, 0.44f, 0.018f);
    } else {
        t.background     = detail::surface_at(accent, 0.99f, 0.003f);
        t.surface        = detail::surface_at(accent, 0.97f, 0.006f);
        t.surface_raised = detail::surface_at(accent, 1.00f, 0.000f);
        t.overlay        = detail::surface_at(accent, 0.94f, 0.010f);
        t.text_primary   = detail::surface_at(accent, 0.22f, 0.012f);
        t.text_secondary = detail::surface_at(accent, 0.48f, 0.014f);
        t.text_disabled  = detail::surface_at(accent, 0.68f, 0.010f);
        t.border         = detail::surface_at(accent, 0.89f, 0.010f);
        t.border_strong  = detail::surface_at(accent, 0.78f, 0.014f);
    }

    t.accent        = accent_srgb;
    t.accent_hover  = dark ? lighten(accent_srgb, 0.06f) : darken(accent_srgb, 0.05f);
    t.accent_active = dark ? darken(accent_srgb, 0.05f)  : darken(accent_srgb, 0.10f);
    t.on_accent     = detail::contrasting_on(accent_srgb, 4.5f);
    t.focus_ring    = accent_srgb.fade(0.55f);

    // Status colours borrow the accent's lightness so they sit in the same
    // visual plane, but keep their own semantic hues.
    const float L = dark ? 0.72f : 0.55f;
    const float C = 0.15f;
    t.success = Color<Oklch>{L, C, 145.0f}.to<Srgb>();
    t.warning = Color<Oklch>{L, C,  75.0f}.to<Srgb>();
    t.danger  = Color<Oklch>{L, C,  27.0f}.to<Srgb>();
    t.info    = Color<Oklch>{L, C, 245.0f}.to<Srgb>();
    t.on_success = detail::contrasting_on(t.success, 4.5f);
    t.on_warning = detail::contrasting_on(t.warning, 4.5f);
    t.on_danger  = detail::contrasting_on(t.danger, 4.5f);

    return t;
}

/// A whole look from a handful of knobs. Each one is a multiplier on the
/// generated defaults, so the result is always internally consistent: the
/// palette is still solved for contrast, radii still nest, springs stay
/// critically damped where they were.
struct Seed {
    Color<Srgb> accent = rgb<0x5B8CFF>;
    Mode        mode   = Mode::dark;
    float roundness = 1.0f;   ///< radii; 0 = square corners everywhere
    float density   = 1.0f;   ///< spacing; 0.75 = compact, 1.25 = airy
    float type      = 1.0f;   ///< the base font size
    float depth     = 1.0f;   ///< elevation; 0 = flat
    float motion    = 1.0f;   ///< spring speed; 0 = no motion
    float lines     = 1.0f;   ///< border and ring widths
};

[[nodiscard]] constexpr Theme derive(const Seed& s) noexcept {
    Theme t = make_theme(s.accent, s.mode);
    t.radius_small  *= s.roundness;
    t.radius_medium *= s.roundness;
    t.radius_large  *= s.roundness;
    t.space_unit    *= s.density;
    t.font_size     *= s.type;
    t.depth_scale    = s.depth;
    t.motion_scale   = s.motion;
    t.line_hairline *= s.lines;
    t.line_strong   *= s.lines;
    t.line_focus    *= s.lines;
    return t;
}

// ── stock themes ────────────────────────────────────────────────────────

namespace themes {
inline constexpr Theme midnight = make_theme(rgb<0x5B8CFF>, Mode::dark);
inline constexpr Theme ember    = make_theme(rgb<0xF76B15>, Mode::dark);
inline constexpr Theme forest   = make_theme(rgb<0x30A46C>, Mode::dark);
inline constexpr Theme orchid   = make_theme(rgb<0x8E4EC6>, Mode::dark);
inline constexpr Theme daylight = make_theme(rgb<0x0090FF>, Mode::light);
inline constexpr Theme paper    = make_theme(rgb<0x8B5E3C>, Mode::light);

// Looks, not just palettes: the same widgets, a different shape.
inline constexpr Theme square  = derive({.accent = rgb<0xE5484D>, .roundness = 0.0f, .depth = 0.0f, .lines = 1.5f});
inline constexpr Theme soft    = derive({.accent = rgb<0x8E4EC6>, .mode = Mode::light, .roundness = 1.8f,
                                         .density = 1.2f, .depth = 1.6f, .motion = 0.8f});
inline constexpr Theme compact = derive({.accent = rgb<0x30A46C>, .roundness = 0.5f, .density = 0.7f,
                                         .type = 0.9f, .motion = 1.4f});
}  // namespace themes

// ── themes as values ───────────────────────────────────────────────────────────────────

namespace detail {
[[nodiscard]] constexpr Color<Srgb> lerp_color(Color<Srgb> a, Color<Srgb> b, float t) noexcept {
    // Through Oklab, like every transition: no grey dip between hues.
    const auto x = a.to<Oklab>(), y = b.to<Oklab>();
    return Color<Oklab>{num::lerp(x.c0, y.c0, t), num::lerp(x.c1, y.c1, t), num::lerp(x.c2, y.c2, t),
                        num::lerp(a.a, b.a, t)}.to<Srgb>();
}
[[nodiscard]] constexpr Spring lerp_spring(Spring a, Spring b, float t) noexcept {
    return {num::lerp(a.stiffness, b.stiffness, t), num::lerp(a.damping, b.damping, t)};
}
}  // namespace detail

/// Blend two themes: every colour (in Oklab), size, radius, line, depth and
/// spring. Drive `t` from an `Animated<float>` in the model and a theme
/// switch morphs the whole app, shape and all, not just its colours. Mode
/// and sheet flip at the midpoint.
[[nodiscard]] constexpr Theme lerp(const Theme& a, const Theme& b, float t) noexcept {
    using detail::lerp_color;
    t = num::saturate(t);
    // Exact at the ends: a settled theme animation must BE the theme, not a
    // float round-trip through Oklab of it (pixel-identical renders).
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    Theme r = t < 0.5f ? a : b;
    Color<Srgb> Theme::*colors_[] = {
        &Theme::background, &Theme::surface, &Theme::surface_raised, &Theme::overlay,
        &Theme::text_primary, &Theme::text_secondary, &Theme::text_disabled,
        &Theme::border, &Theme::border_strong, &Theme::focus_ring,
        &Theme::accent, &Theme::accent_hover, &Theme::accent_active, &Theme::on_accent,
        &Theme::success, &Theme::warning, &Theme::danger, &Theme::info,
        &Theme::on_success, &Theme::on_warning, &Theme::on_danger};
    for (auto m : colors_) r.*m = lerp_color(a.*m, b.*m, t);
    float Theme::*floats_[] = {
        &Theme::radius_small, &Theme::radius_medium, &Theme::radius_large, &Theme::space_unit,
        &Theme::font_size, &Theme::line_hairline, &Theme::line_strong, &Theme::line_focus,
        &Theme::depth_scale, &Theme::motion_scale};
    for (auto m : floats_) r.*m = num::lerp(a.*m, b.*m, t);
    r.motion_ui       = detail::lerp_spring(a.motion_ui, b.motion_ui, t);
    r.motion_emphasis = detail::lerp_spring(a.motion_emphasis, b.motion_emphasis, t);
    r.motion_gentle   = detail::lerp_spring(a.motion_gentle, b.motion_gentle, t);
    return r;
}

/// A legibility problem in a theme: which text role fails on which surface.
struct ThemeIssue {
    const char* text;
    const char* on;
    float       ratio;
    float       needed;
};

/// Check every text role against every surface it is drawn on (WCAG AA:
/// 4.5 body text, 3.0 large/disabled text and focus rings). Hand-edited or
/// user-supplied themes are where contrast regressions come from; the
/// stock themes are static_asserted below. Returns how many issues it wrote.
constexpr int validate(const Theme& t, ThemeIssue* out, int cap) noexcept {
    struct Pair { const char* a; Color<Srgb> fg; const char* b; Color<Srgb> bg; float need; };
    const Pair pairs[] = {
        {"text_primary", t.text_primary, "background", t.background, 4.5f},
        {"text_primary", t.text_primary, "surface", t.surface, 4.5f},
        {"text_primary", t.text_primary, "surface_raised", t.surface_raised, 4.5f},
        {"text_secondary", t.text_secondary, "background", t.background, 4.5f},
        {"text_secondary", t.text_secondary, "surface", t.surface, 4.5f},
        {"text_disabled", t.text_disabled, "surface", t.surface, 2.0f},
        // Labels on fills are semibold control text: WCAG's large/bold bar.
        // (The accent is the user's brand colour; mayag never moves it to
        // hit a number, so the label is what gets solved -- see make_theme.)
        {"on_accent", t.on_accent, "accent", t.accent, 3.0f},
        {"on_danger", t.on_danger, "danger", t.danger, 3.0f},
        {"focus_ring", t.focus_ring, "background", t.background, 3.0f},
    };
    int n = 0;
    for (const auto& p : pairs) {
        const float r = contrast_ratio(p.fg, p.bg);
        if (r < p.need) {
            if (n < cap) out[n] = {p.a, p.b, r, p.need};
            ++n;
        }
    }
    return n;
}
[[nodiscard]] constexpr bool legible(const Theme& t) noexcept { return validate(t, nullptr, 0) == 0; }

static_assert(lerp(themes::midnight, themes::daylight, 0.0f) == themes::midnight);
static_assert(lerp(themes::midnight, themes::square, 1.0f).radius_medium == 0.0f);
// Every stock theme and look passes, at compile time.
static_assert(legible(themes::midnight) && legible(themes::ember) && legible(themes::forest) &&
              legible(themes::orchid) && legible(themes::daylight) && legible(themes::paper) &&
              legible(themes::square) && legible(themes::soft) && legible(themes::compact),
              "mayag theme: a stock theme fails validate(); run it to see which role.");

// ── accessibility, checked at compile time ──────────────────────────────
//
// A generated palette is only useful if it is legible. These assertions mean
// a theme that fails WCAG AA cannot be shipped — the build breaks.

static_assert(contrast_ratio(themes::midnight.text_primary, themes::midnight.background) >= 4.5f,
              "mayag theme: primary text fails WCAG AA against the background.");
static_assert(contrast_ratio(themes::midnight.on_accent, themes::midnight.accent) >= 4.5f,
              "mayag theme: accent text fails WCAG AA against the accent fill.");
static_assert(contrast_ratio(themes::daylight.text_primary, themes::daylight.background) >= 4.5f,
              "mayag theme: light-mode text fails WCAG AA.");
static_assert(contrast_ratio(themes::ember.text_secondary, themes::ember.background) >= 3.0f,
              "mayag theme: secondary text fails WCAG AA for large text.");

}  // namespace mayag
