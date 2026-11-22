#pragma once
// mayag::state and mayag::var — what a style may depend on
//
// Two small vocabularies that the cascade (scene/cascade.hpp) resolves at
// render time, so a view never has to:
//
//   state::   STATE. A selector is a conjunction of conditions on the node
//          (hovered, pressed, focused, selected, ...), on its nearest `group`
//          ancestor, on the theme (dark/light) and on the viewport width.
//
//              when(state::hover && !state::selected, bg(var::surface_raised))
//              when(state::group(state::hover), fg(var::text_primary))
//              when(state::wider(720), axis_row)
//
//   var::  TOKENS. Colours that name a theme role instead of a value. They
//          are resolved against the frame's Theme (and any `vars(...)`
//          override on an ancestor), so switching themes restyles the tree
//          without the view knowing.
//
//              bg(var::accent) | fg(var::on_accent)
//              vars(var::accent = rgb<0xF7768E>)      // a red-accented subtree
//
// Both are plain literal values: they fold into constexpr widget trees.

#include "../core/color.hpp"
#include "../core/math.hpp"
#include "type.hpp"

#include <bit>
#include <cstdint>
#include <string_view>

namespace mayag {

// ════════════════════════════════════════════════════════════════════════
// States and selectors
// ════════════════════════════════════════════════════════════════════════

namespace state {

using Mask = std::uint32_t;

namespace bit {
inline constexpr Mask hover         = 1u << 0;   ///< pointer is over the node
inline constexpr Mask pressed       = 1u << 1;   ///< press began here and the pointer is still over it
inline constexpr Mask focus         = 1u << 2;   ///< has keyboard focus
inline constexpr Mask focus_visible = 1u << 3;   ///< has focus AND it arrived by keyboard
inline constexpr Mask focus_within  = 1u << 4;   ///< focus is on this node or a descendant
inline constexpr Mask disabled      = 1u << 5;   ///< a11y disabled, here or on an ancestor
inline constexpr Mask selected      = 1u << 6;
inline constexpr Mask checked       = 1u << 7;
inline constexpr Mask expanded      = 1u << 8;
inline constexpr Mask starting      = 1u << 9;   ///< the first frame this node exists (entry animation)
inline constexpr Mask dark          = 1u << 10;  ///< the theme is dark
inline constexpr Mask flag0         = 1u << 16;  ///< 8 app-defined flags: 16..23

inline constexpr Mask pointer = hover | pressed;
inline constexpr Mask focusing = focus | focus_visible | focus_within;
}  // namespace bit

/// A conjunction of conditions. `&&` combines; `!` negates one condition.
struct Sel {
    Mask  need  = 0;            ///< states the node must have
    Mask  deny  = 0;            ///< states it must not have
    Mask  gneed = 0;            ///< states its nearest `group` ancestor must have
    Mask  gdeny = 0;
    float min_w = 0.0f;         ///< viewport width >= min_w
    float max_w = num::inf;     ///< viewport width <  max_w

    friend constexpr bool operator==(const Sel&, const Sel&) = default;

    [[nodiscard]] constexpr int conditions() const noexcept {
        return std::popcount(need) + std::popcount(deny) + std::popcount(gneed) +
               std::popcount(gdeny) + (min_w > 0.0f ? 1 : 0) + (max_w < num::inf ? 1 : 0);
    }

    [[nodiscard]] constexpr bool matches(Mask self, Mask group, bool has_group,
                                         float viewport_w) const noexcept {
        if ((self & need) != need || (self & deny) != 0) return false;
        if (gneed != 0 || gdeny != 0) {
            if (!has_group) return false;
            if ((group & gneed) != gneed || (group & gdeny) != 0) return false;
        }
        return viewport_w >= min_w && viewport_w < max_w;
    }

    friend constexpr Sel operator&&(Sel a, Sel b) noexcept {
        return {a.need | b.need, a.deny | b.deny, a.gneed | b.gneed, a.gdeny | b.gdeny,
                num::max(a.min_w, b.min_w), num::min(a.max_w, b.max_w)};
    }
};

namespace detail {
// Called only from a constant evaluation that must fail: its NAME is the
// diagnostic the compiler prints.
inline void negating_a_compound_selector_is_unsupported_negate_each_condition_instead() {}
}  // namespace detail

/// Negate ONE condition: `!state::selected`, `!state::group(state::hover)`.
///
/// Negating a conjunction would need disjunction (De Morgan), which the
/// cascade deliberately does not have: an OR of states is two rules, and two
/// rules are easier to read than one clever one. consteval, so the misuse is
/// a compile error rather than a selector that silently matches the wrong set.
consteval Sel operator!(Sel a) {
    if (a.conditions() != 1)
        detail::negating_a_compound_selector_is_unsupported_negate_each_condition_instead();
    Sel r{};
    r.need = a.deny;   r.deny = a.need;
    r.gneed = a.gdeny; r.gdeny = a.gneed;
    if (a.min_w > 0.0f)      r.max_w = a.min_w;    // !wider(w)   == narrower(w)
    if (a.max_w < num::inf)  r.min_w = a.max_w;    // !narrower(w) == wider(w)
    return r;
}

inline constexpr Sel always{};
inline constexpr Sel hover{bit::hover};
inline constexpr Sel pressed{bit::pressed};
inline constexpr Sel focus{bit::focus};
inline constexpr Sel focus_visible{bit::focus_visible};
inline constexpr Sel focus_within{bit::focus_within};
inline constexpr Sel disabled{bit::disabled};
inline constexpr Sel selected{bit::selected};
inline constexpr Sel checked{bit::checked};
inline constexpr Sel expanded{bit::expanded};
inline constexpr Sel starting{bit::starting};
inline constexpr Sel dark{bit::dark};
inline constexpr Sel light{0, bit::dark};

/// App-defined flag N (0..7), set on a node with `| flag<N>(on)`.
template <int N>
    requires (N >= 0 && N < 8)
inline constexpr Sel flag{bit::flag0 << N};

/// The same conditions, asked of the nearest ancestor marked `| group`.
[[nodiscard]] constexpr Sel group(Sel s) noexcept {
    Sel r = s;
    r.gneed |= r.need; r.gdeny |= r.deny;
    r.need = 0; r.deny = 0;
    return r;
}

/// Responsive conditions on the viewport width (logical px).
[[nodiscard]] constexpr Sel wider(float w) noexcept { Sel s; s.min_w = w; return s; }
[[nodiscard]] constexpr Sel narrower(float w) noexcept { Sel s; s.max_w = w; return s; }

/// Which rule wins when several match.
///
/// Not "specificity" in the CSS sense: a fixed ranking of how strongly a
/// state speaks, so the outcome never depends on the order rules were
/// written. `pressed(...)` beats `hover(...)` (a pressed node is also
/// hovered) and `disabled(...)` beats both, whichever was declared first.
/// Within one rank, the selector with more conditions wins; then the later
/// declaration.
///
///   environment (dark/light, viewport) < group < app states
///   (selected, checked, expanded, flags) < hover < focus_within < focus
///   < focus_visible < pressed < disabled < starting
[[nodiscard]] constexpr int rank_of(Mask m) noexcept {
    int r = 0;
    const auto at_least = [&](Mask bits, int v) { if ((m & bits) != 0 && r < v) r = v; };
    at_least(bit::dark, 1);
    at_least(bit::selected | bit::checked | bit::expanded | (0xFFu << 16), 3);
    at_least(bit::hover, 4);
    at_least(bit::focus_within, 5);
    at_least(bit::focus, 6);
    at_least(bit::focus_visible, 7);
    at_least(bit::pressed, 8);
    at_least(bit::disabled, 9);
    at_least(bit::starting, 10);
    return r;
}

[[nodiscard]] constexpr int priority(const Sel& s) noexcept {
    int r = rank_of(s.need | s.deny);
    if ((s.gneed | s.gdeny) != 0 && r < 2) r = 2;
    if ((s.min_w > 0.0f || s.max_w < num::inf) && r < 1) r = 1;
    return r * 64 + s.conditions();
}

static_assert(priority(pressed) > priority(hover));
static_assert(priority(disabled) > priority(pressed));
static_assert(priority(hover && selected) > priority(hover));
static_assert((!selected).deny == bit::selected);

}  // namespace state

// ════════════════════════════════════════════════════════════════════════
// Colour tokens
// ════════════════════════════════════════════════════════════════════════
//
// A token travels through the ordinary `Color<Srgb>` slots (bg, fg, border,
// shadow, gradient stops) as a TAGGED colour: c0 holds a sentinel no real
// colour can have, c1/c2 say which role, alpha is a multiplier. That keeps
// every existing modifier working unchanged and keeps Style a literal type.
// The cascade replaces tags with real colours before layout, so the painter
// and the GPU never see one.

namespace var {

inline constexpr float kTag = -1.0e30f;

/// Theme roles, in `Theme` field order.
enum class Role : std::uint8_t {
    background, surface, surface_raised, overlay,
    text_primary, text_secondary, text_disabled,
    border, border_strong, focus_ring,
    accent, accent_hover, accent_active, on_accent,
    success, warning, danger, info,
    on_success, on_warning, on_danger,
    count_
};

struct TokenDef;

/// A late-bound colour. Converts to `Color<Srgb>` (tagged), so it goes
/// anywhere a colour does — `bg(var::accent)`, `border(1, var::border)`.
///
/// Operations that need the real value at view time (`mix`, `contrast_ratio`)
/// take `Color<S>` templates, which a Token does not deduce to: misusing one
/// is a compile error instead of arithmetic on a sentinel.
struct Token {
    std::uint32_t code  = 0;      ///< Role index, or a 32-bit name hash
    bool          named = false;
    float         alpha = 1.0f;   ///< multiplier, from fade()
    float         tint  = -1.0f;  ///< >= 0: mixed toward `background` by this much

    [[nodiscard]] constexpr Color<Srgb> encode() const noexcept {
        if (named) {
            return {kTag, static_cast<float>(64u + (code >> 16)),
                    static_cast<float>(code & 0xFFFFu), alpha};
        }
        return {kTag, static_cast<float>(code), tint >= 0.0f ? -2.0f - tint : -1.0f, alpha};
    }
    constexpr operator Color<Srgb>() const noexcept { return encode(); }   // NOLINT: by design

    /// Same role, alpha multiplied — `var::accent.fade(0.3)`.
    [[nodiscard]] constexpr Token fade(float f) const noexcept {
        Token t = *this;
        t.alpha *= num::saturate(f);
        return t;
    }

    /// Bind a value to this token for a subtree: `vars(var::accent = c)`.
    /// A const assignment operator that returns a definition rather than
    /// assigning: it is what makes the override read like CSS.
    constexpr TokenDef operator=(Color<Srgb> v) const noexcept;   // NOLINT

    friend constexpr bool operator==(const Token&, const Token&) = default;
};

struct TokenDef {
    Token       token;
    Color<Srgb> value;
};

constexpr TokenDef Token::operator=(Color<Srgb> v) const noexcept { return {*this, v}; }

[[nodiscard]] constexpr Token role(Role r) noexcept { return {static_cast<std::uint32_t>(r)}; }

inline constexpr Token background     = role(Role::background);
inline constexpr Token surface        = role(Role::surface);
inline constexpr Token surface_raised = role(Role::surface_raised);
inline constexpr Token overlay        = role(Role::overlay);
inline constexpr Token text_primary   = role(Role::text_primary);
inline constexpr Token text_secondary = role(Role::text_secondary);
inline constexpr Token text_disabled  = role(Role::text_disabled);
inline constexpr Token border         = role(Role::border);
inline constexpr Token border_strong  = role(Role::border_strong);
inline constexpr Token focus_ring     = role(Role::focus_ring);
inline constexpr Token accent         = role(Role::accent);
inline constexpr Token accent_hover   = role(Role::accent_hover);
inline constexpr Token accent_active  = role(Role::accent_active);
inline constexpr Token on_accent      = role(Role::on_accent);
inline constexpr Token success        = role(Role::success);
inline constexpr Token warning        = role(Role::warning);
inline constexpr Token danger         = role(Role::danger);
inline constexpr Token info           = role(Role::info);
inline constexpr Token on_success     = role(Role::on_success);
inline constexpr Token on_warning     = role(Role::on_warning);
inline constexpr Token on_danger      = role(Role::on_danger);

/// A theme role mixed toward the background — `Theme::tint`, late-bound.
/// The hover wash every list row and ghost button wants.
[[nodiscard]] constexpr Token tint(Token t, float amount) noexcept {
    t.tint = num::saturate(amount);
    return t;
}

/// An app-defined token, bound by `vars(var::named("brand") = c)` on an
/// ancestor. Unbound, it renders magenta: a missing definition should be
/// impossible to miss rather than quietly falling back to something plausible.
[[nodiscard]] constexpr Token named(std::string_view name) noexcept {
    std::uint32_t h = 2166136261u;
    for (unsigned char ch : name) { h ^= ch; h *= 16777619u; }
    return {h, true};
}

[[nodiscard]] constexpr bool is_token(const Color<Srgb>& c) noexcept { return c.c0 == kTag; }

/// Decode a tagged colour back to its token.
[[nodiscard]] constexpr Token decode(const Color<Srgb>& c) noexcept {
    Token t{};
    t.alpha = c.a;
    if (c.c1 >= 64.0f) {
        t.named = true;
        t.code = ((static_cast<std::uint32_t>(c.c1) - 64u) << 16) | static_cast<std::uint32_t>(c.c2);
    } else {
        t.code = static_cast<std::uint32_t>(c.c1);
        if (c.c2 <= -2.0f) t.tint = -2.0f - c.c2;
    }
    return t;
}

/// Two tokens name the same slot (alpha and tint are applied on top).
[[nodiscard]] constexpr bool same_slot(const Token& a, const Token& b) noexcept {
    return a.named == b.named && a.code == b.code;
}

static_assert(is_token(accent));
static_assert(decode(accent.fade(0.5f)).alpha == 0.5f);
static_assert(same_slot(decode(named("brand")), named("brand")));
static_assert(decode(tint(accent, 0.25f)).tint == 0.25f);

// ── numeric tokens ───────────────────────────────────────────────────────────────────
//
// Spacing, radii, line widths, type sizes and elevation, named by role and
// resolved against the theme by the cascade -- so a theme owns the whole
// LOOK, not just its colours:
//
//     pad(var::space(1.25f), var::space(2)) | radius(var::radius_md)
//         | border(var::line, var::border) | font(var::font_sm)
//         | elevation(var::depth(3))
//
// The same trick as colour tokens: a `Num` converts to `float` as a
// sentinel no real length can have (<= -1e6), so every float modifier takes
// one with no new overloads, elements stay constexpr, and the cascade finds
// them with a single min() per node. Arithmetic on a token is deleted: the
// value does not exist until the theme is known. Scale with the token
// itself, `var::space(3)`, not `var::space(1) * 3`.

inline constexpr float kNumBase  = -1.0e6f;
inline constexpr float kNumLimit = -999999.5f;   ///< v <= this: a token

enum class NumKind : std::uint8_t { space, font, radius, line, depth };

namespace num_code {
inline constexpr int space  = 0;        ///< + round(k * 16), k in [0, 512)
inline constexpr int font   = 10000;    ///< + scale * 8 + tier index (style/type.hpp)
inline constexpr int radius = 20100;    ///< + role
inline constexpr int line   = 20200;    ///< + role
inline constexpr int depth  = 30000;    ///< + round(dp * 16)
}  // namespace num_code

struct Num {
    float v = kNumBase;
    constexpr operator float() const noexcept { return v; }   // NOLINT: by design

    // A token has no value until the theme is known; computing with one
    // would bake the sentinel into garbage. Compile errors instead.
    friend Num operator+(Num, float) = delete;
    friend Num operator+(float, Num) = delete;
    friend Num operator-(Num, float) = delete;
    friend Num operator-(float, Num) = delete;
    friend Num operator*(Num, float) = delete;
    friend Num operator*(float, Num) = delete;
    friend Num operator/(Num, float) = delete;
    friend Num operator-(Num) = delete;
};

[[nodiscard]] constexpr Num num_of(int code) noexcept { return {kNumBase - static_cast<float>(code)}; }
[[nodiscard]] constexpr bool is_num(float v) noexcept { return v <= kNumLimit; }
[[nodiscard]] constexpr int code_of(float v) noexcept {
    return static_cast<int>(kNumBase - v + 0.5f);
}

/// `k` spacing units (theme.space_unit each). Quarter steps are exact.
[[nodiscard]] constexpr Num space(float k) noexcept {
    return num_of(num_code::space + static_cast<int>(num::clamp(k, 0.0f, 511.0f) * 16.0f + 0.5f));
}
/// A text size: `face`'s `tier`, resolved by the cascade against the theme
/// in force (so a subtree theme or a morph re-sizes it). There is no
/// arbitrary multiple: a size is a role on the type scale (style/type.hpp).
[[nodiscard]] constexpr Num type(Scale scale, Tier tier) noexcept {
    return num_of(num_code::font + static_cast<int>(scale) * 8 + tier_index(tier));
}
[[nodiscard]] constexpr Num type(Tier tier) noexcept { return type(Scale::ui, tier); }

// Interface text by tier: `font(var::small)`.
inline constexpr Num fine    = type(Tier::fine);
inline constexpr Num small   = type(Tier::small);
inline constexpr Num body    = type(Tier::body);
inline constexpr Num large   = type(Tier::large);
inline constexpr Num title   = type(Tier::title);
inline constexpr Num display = type(Tier::display);
// The older t-shirt names, now tiers (they were 0.75 / 0.85 / 1 / 1.15 /
// 1.6 / 2.2 times the base: six sizes no rule related).
inline constexpr Num font_xs  = fine;
inline constexpr Num font_sm  = small;
inline constexpr Num font_md  = body;
inline constexpr Num font_lg  = large;
inline constexpr Num font_xl  = title;
inline constexpr Num font_2xl = display;

inline constexpr Num radius_sm   = num_of(num_code::radius + 0);
inline constexpr Num radius_md   = num_of(num_code::radius + 1);
inline constexpr Num radius_lg   = num_of(num_code::radius + 2);
inline constexpr Num radius_full = num_of(num_code::radius + 3);   ///< a pill / circle

inline constexpr Num line        = num_of(num_code::line + 0);     ///< hairline borders
inline constexpr Num line_strong = num_of(num_code::line + 1);     ///< control outlines
inline constexpr Num line_focus  = num_of(num_code::line + 2);     ///< focus rings

/// Elevation of `dp` standard units, scaled by the theme (`depth_scale`;
/// 0 makes a flat theme with no shadows at all). For `elevation(...)`.
[[nodiscard]] constexpr Num depth(float dp) noexcept {
    return num_of(num_code::depth + static_cast<int>(num::clamp(dp, 0.0f, 400.0f) * 16.0f + 0.5f));
}

/// The springs a theme defines, for `transition(...)`. `transition()` with
/// no argument is `motion_ui`. The theme's `motion_scale` speeds up or
/// slows every one of them; 0 turns motion off (reduced motion).
enum class Motion : std::uint8_t { ui = 1, emphasis = 2, gentle = 3 };
inline constexpr Motion motion_ui       = Motion::ui;
inline constexpr Motion motion_emphasis = Motion::emphasis;
inline constexpr Motion motion_gentle   = Motion::gentle;

static_assert(is_num(space(2)) && code_of(space(2)) == 32);
static_assert(code_of(small) == num_code::font + 1 && code_of(type(Scale::content, Tier::body)) == num_code::font + 10);
static_assert(!is_num(-500000.0f) && !is_num(1.0e9f));

}  // namespace var

}  // namespace mayag
