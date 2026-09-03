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

#include "../core/color.hpp"
#include "style.hpp"

namespace mayag {

enum class Mode : std::uint8_t { dark, light };

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

    // Metrics — a theme owns spacing and rounding too, so a component library
    // built on it rescales coherently.
    float radius_small  = 6.0f;
    float radius_medium = 10.0f;
    float radius_large  = 16.0f;
    float space_unit    = 8.0f;
    float font_size     = 14.0f;

    [[nodiscard]] constexpr float space(float multiple) const noexcept {
        return space_unit * multiple;
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
[[nodiscard]] constexpr Theme make_theme(Color<Srgb> accent_srgb, Mode mode) noexcept {
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
}  // namespace themes

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
