#pragma once
// mayag::type — the type scale. Every text size in mayag comes from here.
//
// THE PROBLEM IT SOLVES
//
// A widget library written one widget at a time grows a size per author's
// whim: `font_size * 0.72`, `* 0.74`, `* 0.76`, `* 0.78`, `* 0.8` ... (mayag
// had 25 distinct multipliers over 404 call sites). Each is defensible alone;
// together they put four sizes 0.3 px apart in one panel, which reads as
// noise -- the eye sees "slightly off", not hierarchy. Making some text
// smaller is right; making it smaller by an amount nobody chose is not.
//
// THE MODEL
//
// Text has a SCALE and a TIER, never a number.
//
//   Scale::ui        interface text: labels, titles, toolbars, hints. Its
//                    body is Theme::font_size (the app's "interface size").
//   Scale::content   what the widget is FOR: a file list, a terminal's own
//                    overlays, code. Its body is Theme::content_size (the
//                    app's "terminal size"), or a body the widget is handed.
//
//   Tier           fine  small  body  large  title  display
//                   -2    -1     0     +1     +2      +3     steps of `ratio`
//
// So a scale has at most six sizes, an app at most twelve, and a widget says
// which ROLE its text plays (`small`: meta beside a name; `fine`: hints,
// counts) -- the theme decides the pixels, the same way for every widget.
// (Which FONT plays the text is separate: family(), monospace, Theme::
// font_family. A scale is a size ramp, not a typeface.)
//
// THE RULES (TypeRules), applied in this order to base * ratio^tier:
//
//   1. snap      to `grid` (half points): no size is ever 0.3 px from another
//   2. floor     never below `floor` -- stepping down twice from a small
//                interface can't reach illegible
//   3. distinct  a tier within `min_step` of the next tier toward body takes
//                that tier's size: two sizes are either the same size or
//                visibly different, never "almost". (At a small interface
//                size `fine` and `small` merge; hierarchy then falls to
//                colour and weight, which is what a designer would do.)
//
// The two app-facing numbers are the two sizes a user sets. `ratio` is the
// theme's one typographic decision (1.2, a minor third: firm hierarchy, no
// shouting); the rest are guards, not knobs.
//
// SPELLING IT
//
//   | type(Tier::small)                          late-bound (a var:: token):
//   | type(Scale::content, Tier::small)          follows a subtree theme and
//   | font(var::small)                           a theme morph
//
//   t.type(Tier::small)                          the same size, now -- for
//   t.type(Scale::content, Tier::fine)           measuring (text_advance) and
//   t.type_at(body, Tier::small)                 for a body a widget is handed
//   t.ramp(Scale::content)[Tier::small]
//
//   | glyph_size(px)                             a glyph fitted to geometry (a
//                                                check in a box): not text
//
// A raw `font(x * 0.8f)` in a widget is a lint error (tests/lint/type_scale.cmake).

#include "../core/math.hpp"

#include <array>
#include <cstdint>

namespace mayag {

/// Which body size a text size steps from.
enum class Scale : std::uint8_t {
    ui,        ///< interface text: Theme::font_size
    content,   ///< the widget's subject (a file list, a terminal's overlays): Theme::content_size
};

/// A text size's role, in steps of the scale's ratio from body.
enum class Tier : std::int8_t {
    fine    = -2,   ///< key hints, counts, footnotes: read when looked for
    small   = -1,   ///< meta beside the main text: ages, sizes, crumbs, captions
    body    = 0,    ///< the text the thing is about: names, labels, entries
    large   = 1,    ///< a section heading, a dialog's question
    title   = 2,    ///< a panel or page title
    display = 3,    ///< a hero number, a splash
};

inline constexpr std::array<Tier, 6> all_tiers{Tier::fine, Tier::small, Tier::body,
                                               Tier::large, Tier::title, Tier::display};

[[nodiscard]] constexpr int tier_index(Tier t) noexcept { return static_cast<int>(t) + 2; }   ///< 0..5

/// The scale's rules. Values are logical pixels (the renderer applies DPI).
struct TypeRules {
    float ratio    = 1.2f;    ///< one tier to the next (1.2 = minor third)
    float floor    = 10.0f;   ///< nothing renders smaller
    float min_step = 1.0f;    ///< tiers closer than this merge
    float grid     = 0.5f;    ///< sizes are multiples of this

    friend constexpr bool operator==(const TypeRules&, const TypeRules&) = default;

    [[nodiscard]] constexpr float snap(float px) const noexcept {
        return grid > 0.0f ? num::round(px / grid) * grid : px;
    }

    /// The size of `tier` stepping from `base`. Pure and constexpr: the
    /// cascade, the widgets and the tests all call exactly this.
    [[nodiscard]] constexpr float size(float base, Tier tier) const noexcept {
        const float b = num::max(snap(base), floor);
        const int k = static_cast<int>(tier);
        if (k == 0) return b;
        // Walk from body one tier at a time, so rule 3 compares neighbours.
        float prev = b;
        float exact = b;
        for (int i = 1; i <= (k < 0 ? -k : k); ++i) {
            exact = k < 0 ? exact / ratio : exact * ratio;
            float s = num::max(snap(exact), floor);
            const float gap = k < 0 ? prev - s : s - prev;
            if (gap < min_step) s = prev;   // too close to its neighbour: the same size
            prev = s;
        }
        return prev;
    }
};

/// One face's six sizes, resolved: `ramp[Tier::small]`.
struct TypeRamp {
    std::array<float, 6> px{};
    [[nodiscard]] constexpr float operator[](Tier t) const noexcept { return px[static_cast<std::size_t>(tier_index(t))]; }
    [[nodiscard]] static constexpr TypeRamp of(float base, const TypeRules& s) noexcept {
        TypeRamp r;
        for (Tier t : all_tiers) r.px[static_cast<std::size_t>(tier_index(t))] = s.size(base, t);
        return r;
    }
};

/// Line height (a multiple of the size) that suits a tier: small text needs
/// more air between lines, display text less.
[[nodiscard]] constexpr float tier_line_height(Tier t) noexcept {
    switch (t) {
        case Tier::fine:    return 1.35f;
        case Tier::small:   return 1.38f;
        case Tier::body:    return 1.4f;
        case Tier::large:   return 1.3f;
        case Tier::title:   return 1.2f;
        case Tier::display: return 1.1f;
    }
    return 1.4f;
}

// The rules hold by construction: checked at compile time for the sizes apps
// actually use (and exhaustively in tests/test_type.cpp).
namespace type_detail {
[[nodiscard]] constexpr bool well_formed(float base, const TypeRules& s) noexcept {
    const TypeRamp r = TypeRamp::of(base, s);
    for (std::size_t i = 0; i < 6; ++i) {
        if (r.px[i] < s.floor) return false;                                    // floor
        if (num::abs(r.px[i] - s.snap(r.px[i])) > 1e-4f) return false;          // snapped
        if (i > 0) {
            const float d = r.px[i] - r.px[i - 1];
            if (d < 0.0f) return false;                                         // monotone
            if (d > 0.0f && d < s.min_step - 1e-4f) return false;               // distinct
        }
    }
    return true;
}
}  // namespace type_detail
static_assert(type_detail::well_formed(14.0f, {}) && type_detail::well_formed(12.5f, {}) &&
              type_detail::well_formed(9.0f, {}) && type_detail::well_formed(28.0f, {}));
static_assert(TypeRules{}.size(14.0f, Tier::body) == 14.0f);
static_assert(TypeRules{}.size(14.0f, Tier::small) == 11.5f);   // 14 / 1.2 = 11.67
static_assert(TypeRules{}.size(14.0f, Tier::fine) == 10.0f);    // 9.72 -> the floor

}  // namespace mayag
