#pragma once
// mayag::cascade — resolve a view's tree into the styles it paints with
//
// The pass that sits between view() and layout. A view describes how things
// look in EVERY state; this decides which state each node is in right now and
// writes the final style in place:
//
//   1. INHERIT   text properties a container set explicitly (`| fg(c)`,
//                `| font(13)`) flow to every descendant that did not set its
//                own. Overflow and line clamping never inherit.
//   2. MATCH     each node's rules (`| hovered(...)`, `| when(sel, ...)`) are
//                tested against its state: hover / press / focus from the
//                pointer and keyboard, disabled / selected / checked /
//                expanded from its a11y state, app flags, dark/light from the
//                theme, the viewport width, and the state of its nearest
//                `| group` ancestor. Matching rules are piped onto the base
//                style lowest priority first (state::priority), so pressed
//                beats hover and disabled beats both, whatever order they
//                were written in.
//   3. BIND      colour tokens (`var::accent`) become real colours, from the
//                theme or from the nearest `| vars(...)` binding.
//   4. ANIMATE   a node with `| transition(...)` springs toward its new
//                resolved style instead of jumping (StyleMotion, below).
//
// Variants run BEFORE layout, so a variant may change padding or size and the
// tree reflows: that is what makes `when(state::narrower(600), v_axis)` work.
//
// Everything here is a pure function of (tree, CascadeInput) plus the
// StyleMotion cache. It runs headless, in tests, in render_to_png, identically.

#include "../core/animation.hpp"
#include "../core/color.hpp"
#include "../scene/memo.hpp"
#include "../scene/node.hpp"
#include "state.hpp"
#include "style.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mayag {

/// Interaction facts the cascade needs. The host fills this from its
/// `Interaction`; a test fills it by hand.
struct CascadeInput {
    const Theme*  theme    = nullptr;   ///< null: themes::midnight
    Vec2          viewport{};
    std::uint64_t hovered  = 0;         ///< the hit node (innermost named)
    std::uint64_t pressed  = 0;         ///< the node the press began on
    bool          press_inside = true;  ///< pointer still over `pressed`
    std::uint64_t focused  = 0;
    bool          focus_visible = false;///< focus arrived by keyboard
    /// Node ids that existed last frame; a node absent from it is `starting`.
    /// Null: nothing is starting (one-shot renders).
    const std::unordered_set<std::uint64_t>* previous_ids = nullptr;
    /// Last frames' memo() subtrees. Null: no reuse (one-shot renders).
    MemoCache* memos = nullptr;
};

// ════════════════════════════════════════════════════════════════════════
// Token resolution
// ════════════════════════════════════════════════════════════════════════

inline void assign_hit_ids(Node& n, std::uint64_t path);

namespace cascade_detail {

[[nodiscard]] inline Color<Srgb> role_color(const Theme& t, std::uint32_t r) noexcept {
    using R = var::Role;
    switch (static_cast<R>(r)) {
        case R::background:     return t.background;
        case R::surface:        return t.surface;
        case R::surface_raised: return t.surface_raised;
        case R::overlay:        return t.overlay;
        case R::text_primary:   return t.text_primary;
        case R::text_secondary: return t.text_secondary;
        case R::text_disabled:  return t.text_disabled;
        case R::border:         return t.border;
        case R::border_strong:  return t.border_strong;
        case R::focus_ring:     return t.focus_ring;
        case R::accent:         return t.accent;
        case R::accent_hover:   return t.accent_hover;
        case R::accent_active:  return t.accent_active;
        case R::on_accent:      return t.on_accent;
        case R::success:        return t.success;
        case R::warning:        return t.warning;
        case R::danger:         return t.danger;
        case R::info:           return t.info;
        case R::on_success:     return t.on_success;
        case R::on_warning:     return t.on_warning;
        case R::on_danger:      return t.on_danger;
        case R::count_:         break;
    }
    return rgb<0xFF00FF>;
}

// ── numeric tokens ──────────────────────────────────────────────────────────────────

[[nodiscard]] inline float num_value(const Theme& t, int code) noexcept {
    namespace c = var::num_code;
    if (code < c::font)   return t.space_unit * static_cast<float>(code - c::space) * (1.0f / 16.0f);
    if (code < c::radius) return t.font_size * static_cast<float>(code - c::font) * 0.01f;
    if (code < c::line) {
        switch (code - c::radius) {
            case 0: return t.radius_small;
            case 1: return t.radius_medium;
            case 2: return t.radius_large;
            default: return 1.0e9f;   // full: clamped to a pill at layout
        }
    }
    if (code < c::depth) {
        switch (code - c::line) {
            case 0: return t.line_hairline;
            case 1: return t.line_strong;
            default: return t.line_focus;
        }
    }
    return t.depth_scale * static_cast<float>(code - c::depth) * (1.0f / 16.0f);
}

/// Resolve one float in place. The branch is a single compare on a value
/// already in a register; the slow path only runs for an actual token.
inline void fix(float& v, const Theme& t) noexcept {
    if (var::is_num(v)) [[unlikely]] v = num_value(t, var::code_of(v));
}
inline void fix(Length& l, const Theme& t) noexcept {
    if (l.unit == Length::Unit::pixels) fix(l.value, t);
}

/// An `elevation(var::depth(dp))` shadow pair whose geometry waits for the
/// theme: its shadows carry the depth token in `spread` and a slot index in
/// `offset.x` (see dsl Elevation). Expanded with exactly the formula
/// `elevation(float)` uses, so a token and a literal of the same resolved
/// value draw identical pixels.
inline void expand_elevation(Shadow& sh, float dp) noexcept {
    const int layer = sh.offset.x > 0.5f ? 1 : 0;
    if (dp <= 0.0f) { sh = Shadow{{0.0f, 0.0f}, 0.0f, 0.0f, colors::transparent}; return; }   // flat theme
    sh = elevation_shadow(dp, layer);
}

/// Does any animatable/layout float in `s` hold a token? Folded with min():
/// branch-free over ~40 floats, so the 99% of nodes with no numeric token
/// pay a handful of cycles and skip the field-by-field pass entirely.
[[nodiscard]] inline bool has_num_token(const Style& s) noexcept {
    const LayoutStyle& L = s.layout;
    float m = num::min(num::min(L.padding.top, L.padding.right), num::min(L.padding.bottom, L.padding.left));
    m = num::min(m, num::min(num::min(L.margin.top, L.margin.right), num::min(L.margin.bottom, L.margin.left)));
    m = num::min(m, num::min(L.gap, num::min(L.width.value, L.height.value)));
    m = num::min(m, num::min(num::min(L.min_width.value, L.min_height.value),
                             num::min(L.max_width.value, L.max_height.value)));
    m = num::min(m, num::min(num::min(s.corners.tl, s.corners.tr), num::min(s.corners.br, s.corners.bl)));
    m = num::min(m, num::min(s.stroke.width, s.text.size));
    for (std::uint8_t i = 0; i < s.shadow_count; ++i)
        m = num::min(m, num::min(s.shadows[i].blur, s.shadows[i].spread));
    return var::is_num(m);
}

inline void resolve_numbers(Style& s, const Theme& t) noexcept {
    if (!has_num_token(s)) [[likely]] return;
    LayoutStyle& L = s.layout;
    fix(L.padding.top, t); fix(L.padding.right, t); fix(L.padding.bottom, t); fix(L.padding.left, t);
    fix(L.margin.top, t);  fix(L.margin.right, t);  fix(L.margin.bottom, t);  fix(L.margin.left, t);
    fix(L.gap, t);
    fix(L.width, t); fix(L.height, t);
    fix(L.min_width, t); fix(L.min_height, t); fix(L.max_width, t); fix(L.max_height, t);
    fix(s.corners.tl, t); fix(s.corners.tr, t); fix(s.corners.br, t); fix(s.corners.bl, t);
    fix(s.stroke.width, t);
    fix(s.text.size, t);
    for (std::uint8_t i = 0; i < s.shadow_count; ++i) {
        Shadow& sh = s.shadows[i];
        if (var::is_num(sh.spread)) {
            const float v = num_value(t, var::code_of(sh.spread));
            if (var::code_of(sh.spread) >= var::num_code::depth) expand_elevation(sh, v);
            else sh.spread = v;
        }
        fix(sh.blur, t);
    }
}

/// `transition()` with a motion role takes the theme's spring, scaled.
inline void resolve_motion(Transition& tr, const Theme& t) noexcept {
    if (tr.motion == 0 || tr.props == 0) return;
    const Spring sp = tr.motion == 2 ? t.motion_emphasis : tr.motion == 3 ? t.motion_gentle : t.motion_ui;
    const float k = t.motion_scale;
    if (k <= 0.0f) { tr.props = 0; tr.stiffness = 0.0f; return; }   // reduced motion: snap
    // Scaling time by k scales stiffness by k² and damping by k, which keeps
    // the damping RATIO: a faster theme is the same spring, played faster.
    tr.stiffness = sp.stiffness * k * k;
    tr.damping   = sp.damping * k;
}

/// The variable scope: a chain of `vars(...)` bindings up the tree.
struct Scope {
    const Theme&                    theme;
    std::span<const var::TokenDef>  defs;
    const Scope*                    parent = nullptr;
    bool                            explicit_theme = false;   ///< under a `| theme(...)`

    [[nodiscard]] Color<Srgb> slot(const var::Token& want) const noexcept {
        for (const Scope* s = this; s != nullptr; s = s->parent) {
            // Last binding wins within one `vars(...)`, like CSS.
            for (auto it = s->defs.rbegin(); it != s->defs.rend(); ++it) {
                if (var::same_slot(it->token, want)) {
                    // A binding may itself name a token: resolve it from the
                    // scope ABOVE this one, so `accent = var::danger` works
                    // and cannot recurse into itself.
                    if (var::is_token(it->value)) {
                        return s->parent ? s->parent->resolve(it->value)
                                         : Scope{theme, {}, nullptr}.resolve(it->value);
                    }
                    return it->value;
                }
            }
        }
        // Unbound named tokens are loud on purpose.
        return want.named ? rgb<0xFF00FF> : role_color(theme, want.code);
    }

    [[nodiscard]] Color<Srgb> resolve(const Color<Srgb>& c) const noexcept {
        if (!var::is_token(c)) return c;
        const var::Token t = var::decode(c);
        var::Token base = t;
        base.alpha = 1.0f;
        base.tint = -1.0f;
        Color<Srgb> v = slot(base);
        if (t.tint >= 0.0f) v = mix(slot(var::background), v, t.tint);
        return v.fade(t.alpha);
    }
};

inline void resolve_colors(Style& s, const Scope& scope) {
    s.fill.color = scope.resolve(s.fill.color);
    for (std::uint8_t i = 0; i < s.fill.stop_count; ++i)
        s.fill.stops[i].color = scope.resolve(s.fill.stops[i].color);
    s.stroke.color = scope.resolve(s.stroke.color);
    for (std::uint8_t i = 0; i < s.shadow_count; ++i)
        s.shadows[i].color = scope.resolve(s.shadows[i].color);
    s.text.color = scope.resolve(s.text.color);
}

/// Copy the inheritable text fields `from` set explicitly into `to`, where
/// `to` did not set them itself.
inline void inherit_text(TextStyle& to, const TextStyle& from) noexcept {
    namespace f = text_bits;
    const std::uint16_t take = from.set & static_cast<std::uint16_t>(~to.set);
    if (take == 0) return;
    if (take & f::size)        to.size = from.size;
    if (take & f::weight)      to.weight = from.weight;
    if (take & f::italic)      to.italic = from.italic;
    if (take & f::underline)   to.underline = from.underline;
    if (take & f::strike)      to.strikethrough = from.strikethrough;
    if (take & f::color)       to.color = from.color;
    if (take & f::line_height) to.line_height = from.line_height;
    if (take & f::tracking)    to.letter_spacing = from.letter_spacing;
    if (take & f::align)       to.align = from.align;
    if (take & f::ligatures)   to.ligatures = from.ligatures;
    if (take & f::variations)  to.variations = from.variations;
    to.set |= take;
}

[[nodiscard]] inline state::Mask own_state(const Node& n, const CascadeInput& in,
                                           bool parent_disabled) noexcept {
    using namespace state::bit;
    const Style& s = n.style();
    state::Mask m = static_cast<state::Mask>(s.flags) << 16;
    const std::uint64_t id = s.id;
    if (id != 0) {
        if (id == in.hovered) m |= hover;
        if (id == in.pressed && in.press_inside) m |= pressed;
        if (id == in.focused) {
            m |= focus;
            if (in.focus_visible) m |= focus_visible;
        }
        if (in.previous_ids != nullptr && !in.previous_ids->contains(id)) m |= starting;
    }
    if (s.a11y_state.disabled || parent_disabled) m |= disabled;
    if (s.a11y_state.selected) m |= selected;
    if (s.a11y_state.checked)  m |= checked;
    if (s.a11y_state.expanded) m |= expanded;
    return m;
}

/// Hover propagates to ancestors the way CSS :hover does: a card containing
/// the hovered button is hovered too. The hit test only knows the innermost
/// NAMED node, so a named ancestor of it is marked here (pressed likewise).
/// Returns this subtree's contribution: hover, pressed, focus_within.
inline state::Mask subtree_state(Node& n, const CascadeInput& in,
                                 std::unordered_map<const Node*, state::Mask>& out) {
    using namespace state::bit;
    // A reused memo subtree: what it contributes to its ancestors comes from
    // its id list. (If it gets re-cascaded, visit_memo walks it fresh.)
    if (n.memo_reused() && in.memos != nullptr) {
        if (const MemoCache::Entry* e = in.memos->find(n.memo_key()); e != nullptr && e->has_cascaded) {
            const auto has = [&](std::uint64_t id) {
                return id != 0 && std::binary_search(e->ids.begin(), e->ids.end(), id);
            };
            state::Mask m = 0;
            if (has(in.hovered)) m |= hover;
            if (has(in.pressed) && in.press_inside) m |= pressed;
            if (has(in.focused)) m |= focus_within;
            return m;
        }
    }
    state::Mask below = 0;
    for (auto& k : n.children()) below |= subtree_state(k, in, out);
    const std::uint64_t id = n.style().id;
    state::Mask here = below;
    if (id != 0) {
        if (id == in.hovered) here |= hover;
        if (id == in.pressed && in.press_inside) here |= pressed;
        if (id == in.focused) here |= focus_within;
    }
    // Inherited hover/press only matter to nodes with rules; keep the map small.
    if (here != 0 && n.cascade() != nullptr) out[&n] = here & (hover | pressed | focus_within);
    return here & (hover | pressed | focus_within);
}

struct Walk {
    const CascadeInput& in;
    const Theme&        theme;
    std::unordered_map<const Node*, state::Mask> deep;   // inherited pointer state
    bool                dark;
    bool                any_transition = false;
    bool                any_starting = false;
    int                 memo_depth = 0;    // inside a memo subtree being cascaded fresh
    const Theme*        hashed_theme = nullptr;
    std::uint64_t       theme_hash = 0;

    /// Theme and var bindings a node resolves against, hashed.
    [[nodiscard]] std::uint64_t scope_hash(const Scope& sc) {
        if (&sc.theme != hashed_theme) {
            hashed_theme = &sc.theme;
            static_assert(std::is_trivially_copyable_v<Theme>);
            std::uint64_t h = 0xcbf29ce484222325ull;
            const auto* b = reinterpret_cast<const unsigned char*>(&sc.theme);
            for (std::size_t i = 0; i < sizeof(Theme); ++i) h = (h ^ b[i]) * 0x100000001b3ull;
            theme_hash = h;
        }
        std::uint64_t h = theme_hash ^ (sc.explicit_theme ? 0x9E3779B97F4A7C15ull : 0);
        for (const Scope* s = &sc; s != nullptr; s = s->parent) {
            for (const auto& d : s->defs) {
                const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
                std::uint32_t u;
                mix(d.token.code | (std::uint64_t{d.token.named} << 32));
                std::memcpy(&u, &d.token.alpha, 4); mix(u);
                std::memcpy(&u, &d.token.tint, 4); mix(u);
                std::memcpy(&u, &d.value.c0, 4); mix(u);
                std::memcpy(&u, &d.value.c1, 4); mix(u);
                std::memcpy(&u, &d.value.c2, 4); mix(u);
                std::memcpy(&u, &d.value.a, 4); mix(u);
            }
            mix_level(h);
        }
        return h;
    }
    static void mix_level(std::uint64_t& h) noexcept { h = (h ^ 0xA5u) * 0x100000001b3ull; }

    void visit(Node& n, const TextStyle* parent_text, const Scope& scope,
               state::Mask group_state, bool has_group, bool parent_disabled) {
        if (n.memo_key() != 0) {
            visit_memo(n, parent_text, scope, group_state, has_group, parent_disabled);
            return;
        }
        visit_plain(n, parent_text, scope, group_state, has_group, parent_disabled);
    }

    // A memo() root: reuse last frame's resolved subtree when what it would
    // resolve against is unchanged; otherwise cascade it and remember.
    void visit_memo(Node& n, const TextStyle* parent_text, const Scope& scope,
                    state::Mask group_state, bool has_group, bool parent_disabled) {
        MemoCache* cache = in.memos;
        MemoCache::Entry* e = cache != nullptr ? cache->find(n.memo_key()) : nullptr;
        MemoCache::Env env{};
        env.has_parent_text = parent_text != nullptr;
        if (parent_text != nullptr) env.parent_text = *parent_text;
        env.scope = scope_hash(scope);
        env.group_state = group_state;
        env.has_group = has_group;
        env.parent_disabled = parent_disabled;
        env.path = n.memo_path();

        if (n.memo_reused()) {
            if (e != nullptr && memo_depth == 0 && e->has_cascaded) {
                MemoCache::interaction_inside(e->ids, in.hovered, in.pressed, in.focused, in.press_inside,
                                              in.focus_visible, env);
                if (env == e->env && !e->animating) { cache->live(&n); return; }   // reuse as is
            }
            // The outside changed (or it's nested): start over from the
            // subtree as built, with the hit ids it would have had.
            const std::uint64_t key = n.memo_key();
            if (e == nullptr) { n.set_memo_key(0); n.set_memo_reused(false); visit_plain(n, parent_text, scope, group_state, has_group, parent_disabled); return; }
            n = e->built;
            n.set_memo_key(key);
            n.set_memo_reused(false);
            n.set_memo_path(env.path);
            assign_hit_ids(n, env.path);
            (void)subtree_state(n, in, deep);
        }

        // Nested memos are cascaded but not kept, and so is a subtree on a
        // frame where something in it is starting (its enter look is
        // momentary). Transitions are fine: the motion pass flags a subtree
        // whose springs are moving, and it's re-resolved while they do.
        if (cache == nullptr || e == nullptr || memo_depth > 0) {
            if (memo_depth > 0) n.set_memo_key(0);
            visit_plain(n, parent_text, scope, group_state, has_group, parent_disabled);
            return;
        }
        const bool t0 = any_transition, s0 = any_starting;
        any_transition = any_starting = false;
        ++memo_depth;
        visit_plain(n, parent_text, scope, group_state, has_group, parent_disabled);
        --memo_depth;
        const bool starting = any_starting;
        any_transition = any_transition || t0;
        any_starting = any_starting || s0;
        if (starting) { cache->forget(n.memo_key()); n.set_memo_key(0); return; }
        cache->cascaded(n, env, in.hovered, in.pressed, in.focused, in.press_inside, in.focus_visible);
    }

    void visit_plain(Node& n, const TextStyle* parent_text, const Scope& scope,
                     state::Mask group_state, bool has_group, bool parent_disabled) {
        // A plain box with the default style and no rules: resolving a
        // default style changes nothing, so don't make one. Its children
        // still inherit what it would have passed down.
        if (!n.has_own_style() && n.cascade() == nullptr && n.kind() != NodeKind::text &&
            !deep.contains(&n)) {
            TextStyle pass_down = std::as_const(n).style().text;
            if (parent_text != nullptr) inherit_text(pass_down, *parent_text);
            const bool disabled_here = (own_state(n, in, parent_disabled) & state::bit::disabled) != 0;
            for (auto& k : n.children()) visit(k, &pass_down, scope, group_state, has_group, disabled_here);
            return;
        }
        Style& s = n.style();
        const Cascade* cc = n.cascade().get();

        // `| theme(t)`: everything below resolves against `t`. A fresh scope
        // (no inherited vars: the new theme IS the binding) for this node on.
        const bool rethemed = cc != nullptr && cc->theme != nullptr;
        const Theme& th = rethemed ? *cc->theme : scope.theme;
        Scope themed{th, {}, nullptr, true};
        const Scope& base = rethemed ? themed : scope;
        const bool rethemed_any = rethemed || scope.explicit_theme;

        state::Mask self = own_state(n, in, parent_disabled);
        if (auto it = deep.find(&n); it != deep.end()) self |= it->second;
        if (th.mode == Mode::dark) self |= state::bit::dark;

        // Variable bindings take effect for this node's own colours too.
        Scope local{th, cc ? std::span<const var::TokenDef>{cc->vars} : std::span<const var::TokenDef>{},
                    &base, rethemed_any};
        const Scope& sc = (cc && !cc->vars.empty()) ? local : base;

        if (cc != nullptr) {
            if ((self & state::bit::starting) != 0 && (cc->uses & state::bit::starting) != 0)
                any_starting = true;
            for (const auto& r : cc->rules) {
                if (r.sel.matches(self, group_state, has_group, in.viewport.x)) r.apply(s, r.data);
            }
        }
        // Inherit AFTER variants: a variant's `fg(...)` is explicit and must
        // beat what the parent says. Explicit always beats inherited.
        if (parent_text != nullptr) inherit_text(s.text, *parent_text);
        // Text no one gave a colour reads in the theme's text_primary -- not
        // literal white, which vanished the moment a light theme was on.
        // (Only when a theme scope is explicit, `| theme(...)` / themed(...),
        // or the frame theme is light: dark default renders stay byte-for-
        // byte what they were, white on dark.)
        if (n.kind() == NodeKind::text && (s.text.set & text_bits::color) == 0 &&
            (rethemed_any || th.mode == Mode::light)) {
            s.text.color = var::text_primary;
            s.text.set |= text_bits::color;
        }
        // Children inherit the UNRESOLVED text style: a token travels down
        // as a token and resolves in the scope where it lands, so a
        // `vars(...)` island (an inverted card) recolours the text it
        // inherits. Late binding, deliberately unlike CSS, where var() is
        // fixed where it is declared and an island would need restating.
        const TextStyle pass_down = s.text;
        resolve_colors(s, sc);
        resolve_numbers(s, th);
        resolve_motion(s.transition, th);
        if (s.transition.on()) any_transition = true;

        const bool disabled_here = (self & state::bit::disabled) != 0;
        const state::Mask g = s.group ? self : group_state;
        const bool hg = s.group || has_group;
        for (auto& k : n.children()) visit(k, &pass_down, sc, g, hg, disabled_here);
    }
};

}  // namespace cascade_detail

/// Give every node that asks about pointer state (hover / press) but has no
/// `id<>` a stable, structural id so the hit test can find it. Without this
/// `box() | hovered(bg(c))` would never hover, and a rule that silently
/// never fires is the worst kind of styling bug.
///
/// The id is the parent's id (or path) mixed with the child index: stable
/// across frames as long as the tree's SHAPE is, which is what hover needs.
/// Marked `hit_only`, so these nodes are never Tab stops and never take
/// focus on click; they are not controls, they only light up.
inline void assign_hit_ids(Node& n, std::uint64_t path) {
    // A memo root remembers the path it sits at; a reused one keeps the ids
    // it was built with (the cascade redoes it if the path moved).
    if (n.memo_key() != 0) n.set_memo_path(path);
    if (n.memo_reused()) return;
    constexpr state::Mask pointer = state::bit::hover | state::bit::pressed;
    const Cascade* cc = n.cascade().get();
    const std::uint64_t own = std::as_const(n).style().id;   // reads must not allocate
    if (own == 0 && cc != nullptr && !cc->handlers.empty()) {
        // An element with handlers IS a control: it takes clicks, focus and
        // Enter/Space. A structural id makes it one without an id<>.
        n.style().id = path | 1u;
    } else if (own == 0 && cc != nullptr && (cc->uses & pointer) != 0) {
        n.style().id = path | 1u;   // never 0
        n.style().hit_only = true;
    }
    const std::uint64_t id = std::as_const(n).style().id;
    const std::uint64_t base = id != 0 ? id : path;
    std::uint64_t i = 0;
    for (auto& k : n.children()) {
        std::uint64_t h = base ^ (0x9E3779B97F4A7C15ull + ++i);
        h *= 1099511628211ull;
        assign_hit_ids(k, h);
    }
}

/// Hit ids are assigned at BUILD time of the frame, before the interaction
/// layer sees the tree, so hover state can name them. Call on the root of
/// every tree the host lays out (main tree and overlays).
inline void prepare_for_input(Node& root) { assign_hit_ids(root, 0xCBF29CE484222325ull); }

/// What one cascade pass found that the host must act on.
struct CascadeResult {
    bool transitions = false;   ///< some node asked for `transition(...)`: run StyleMotion
    bool starting    = false;   ///< some node was `starting`: one more frame is owed
    explicit operator bool() const noexcept { return transitions; }
};

/// Resolve `root` in place. Idempotent: a tree already cascaded is left alone.
inline CascadeResult cascade(Node& root, const CascadeInput& in) {
    if (root.cascaded()) return {};
    prepare_for_input(root);
    const Theme& theme = in.theme != nullptr ? *in.theme : themes::midnight;
    cascade_detail::Walk w{in, theme, {}, theme.mode == Mode::dark};
    (void)cascade_detail::subtree_state(root, in, w.deep);
    const cascade_detail::Scope top{theme, {}, nullptr};
    w.visit(root, nullptr, top, 0, false, false);
    root.set_cascaded(true);
    return {w.any_transition, w.any_starting};
}

/// For one-shot renders and tests: cascade with no pointer, only the theme
/// and viewport. Everything resolves to its rest state.
inline void cascade_static(Node& root, const Theme& theme, Vec2 viewport) {
    CascadeInput in{};
    in.theme = &theme;
    in.viewport = viewport;
    (void)cascade(root, in);
}

// ════════════════════════════════════════════════════════════════════════
// StyleMotion — transitions between resolved styles
// ════════════════════════════════════════════════════════════════════════
//
// A cache of springs keyed by node id. Each frame, after the cascade, every
// node with `| transition(...)` has its animatable properties replaced by
// a spring's current value that is chasing the freshly resolved target.
// Colours move in Oklab, so hover washes stay vivid instead of greying.
//
// Only NAMED nodes animate: identity across frames is what makes "the same
// button, now hovered" a transition rather than a new button. An unnamed node
// with a transition is resolved but snaps (and the audit could flag it).

class StyleMotion {
  public:
    /// Apply springs to every transitioning node in ONE root and sweep. `dt`
    /// is the time since the previous call (0 on a draw that was not a frame
    /// tick: retarget only). For several roots in a frame use begin_frame /
    /// visit_root / end_frame.
    void apply(Node& root, double dt) {
        begin_frame();
        visit_root(root, dt);
        end_frame();
    }
    /// Spring every transitioning node under `root`. Any number of roots per
    /// frame (the main tree, then each overlay); give each a distinct `salt`.
    void visit_root(Node& root, double dt, std::uint64_t salt = 0) {
        visit(root, dt, 0xCBF29CE484222325ull ^ (salt * 0x9E3779B97F4A7C15ull));
    }

    [[nodiscard]] bool animating() const noexcept { return moving_; }
    [[nodiscard]] std::size_t size() const noexcept { return springs_.size(); }
    /// Where memo roots report their motion (the host's cache).
    void set_memos(MemoCache* c) noexcept { memos_ = c; }
    void clear() { springs_.clear(); moving_ = false; }

    /// The animatable state of a style, flattened to floats. Colours in
    /// Oklab (perceptual) with alpha; geometry in logical px.
    struct Vec {
        static constexpr int kColors = 3 + max_shadows + max_gradient_stops;
        static constexpr int N = kColors * 4 + 1 /*border w*/ + 4 /*radii*/
                                 + max_shadows * 4 /*offset, blur, spread*/ + 1 /*backdrop*/
                                 + 1 /*opacity*/ + 6 /*affine*/;
        float v[N]{};
        std::uint8_t props = 0;
    };

  private:
    /// Exactly what a transition can move, compared with == (no floats are
    /// converted). Layout and text metrics are not animated, so they are not
    /// here, and the snapshot stays a fraction of a Style.
    struct Look {
        Fill fill{};
        Stroke stroke{};
        Color<Srgb> text{};
        std::array<Shadow, max_shadows> shadows{};
        std::uint8_t shadow_count = 0;
        Corners corners{};
        Backdrop backdrop{};
        float opacity = 1.0f;
        Affine transform{};
        Transition transition{};
        friend bool operator==(const Look&, const Look&) = default;

        static Look of(const Style& s) {
            return {s.fill, s.stroke, s.text.color, s.shadows, s.shadow_count, s.corners,
                    s.backdrop, s.opacity, s.transform, s.transition};
        }
    };

    struct Spring1 {
        Vec x{}, vel{};
        Vec target{};
        bool moving = false;
        std::uint64_t shape_sig = 0;   // gradient kind/stop count: a change snaps
        /// The last visible look of each shadow slot that has no geometry of
        /// its own once its target is gone (inset flag), so a shadow can fade
        /// OUT as well as in.
        std::array<bool, max_shadows> inset{};
        /// The animatable part of the style this spring last settled on. A
        /// resting node whose new resolved style matches needs no work at
        /// all: no Oklab conversion, no integration. That is nearly every
        /// node on nearly every frame, so it is the case that must be free.
        Look  rest{};
        bool  has_rest = false;
        std::uint32_t frame = 0;   ///< last frame this node was in a tree
        bool  primed = false;      ///< x/vel hold real state (built on first motion)
    };

    static void put_color(float*& p, Color<Srgb> c) {
        const auto o = c.to<Oklab>();
        *p++ = o.c0; *p++ = o.c1; *p++ = o.c2; *p++ = c.a;
    }
    static Color<Srgb> get_color(const float*& p) {
        Color<Oklab> o{p[0], p[1], p[2], num::saturate(p[3])};
        p += 4;
        return o.to<Srgb>();
    }

    // Absent things are the invisible version of themselves, so appearing is
    // a transition, not a cut: no fill is a transparent fill, no border is a
    // zero-width border, a missing shadow slot is a transparent, zero-size
    // shadow. That is what lets the most common hover looks animate: a
    // background washing in on a bare row, a focus ring growing, a card's
    // elevation lifting.
    static void flatten(const Style& s, Vec& out) {
        float* p = out.v;
        const bool filled = s.fill.kind == FillKind::solid;
        put_color(p, filled ? s.fill.color : s.fill.color.fade(0.0f));
        put_color(p, s.stroke.width > 0.0f ? s.stroke.color : s.stroke.color.fade(0.0f));
        put_color(p, s.text.color);
        for (std::size_t i = 0; i < max_shadows; ++i)
            put_color(p, i < s.shadow_count ? s.shadows[i].color : s.shadows[i].color.fade(0.0f));
        for (std::size_t i = 0; i < max_gradient_stops; ++i) put_color(p, s.fill.stops[i].color);
        *p++ = s.stroke.width;
        *p++ = s.corners.tl; *p++ = s.corners.tr; *p++ = s.corners.br; *p++ = s.corners.bl;
        for (std::size_t i = 0; i < max_shadows; ++i) {
            const Shadow sh = i < s.shadow_count ? s.shadows[i] : Shadow{{0, 0}, 0, 0, {}, false};
            *p++ = sh.offset.x; *p++ = sh.offset.y; *p++ = sh.blur; *p++ = sh.spread;
        }
        *p++ = s.backdrop.blur;
        *p++ = s.opacity;
        const Affine& a = s.transform;
        *p++ = a.a; *p++ = a.b; *p++ = a.c; *p++ = a.d; *p++ = a.tx; *p++ = a.ty;
        out.props = s.transition.props;
    }

    static void unflatten(const Vec& in, Style& s, const std::array<bool, max_shadows>& inset) {
        namespace tp = transition_props;
        const float* p = in.v;
        const bool col = (in.props & tp::color) != 0, shp = (in.props & tp::shape) != 0;
        const auto c = [&](Color<Srgb>& dst) { const Color<Srgb> v = get_color(p); if (col) dst = v; };
        c(s.fill.color);
        c(s.stroke.color);
        c(s.text.color);
        for (std::size_t i = 0; i < max_shadows; ++i) c(s.shadows[i].color);
        for (std::size_t i = 0; i < max_gradient_stops; ++i) c(s.fill.stops[i].color);
        const auto f = [&](float& dst, bool on) { const float v = *p++; if (on) dst = v; };
        f(s.stroke.width, shp);
        f(s.corners.tl, shp); f(s.corners.tr, shp); f(s.corners.br, shp); f(s.corners.bl, shp);
        for (std::size_t i = 0; i < max_shadows; ++i) {
            Shadow& sh = s.shadows[i];
            f(sh.offset.x, shp); f(sh.offset.y, shp); f(sh.blur, shp); f(sh.spread, shp);
        }
        f(s.backdrop.blur, shp);
        f(s.opacity, (in.props & tp::opacity) != 0);
        const bool tr = (in.props & tp::transform) != 0;
        Affine& a = s.transform;
        f(a.a, tr); f(a.b, tr); f(a.c, tr); f(a.d, tr); f(a.tx, tr); f(a.ty, tr);

        // Something still visible mid-fade must still be DRAWN: a fill whose
        // target is "none" is solid until its alpha reaches zero; a shadow
        // slot fading out keeps the slot open.
        if (col && s.fill.kind == FillKind::none && s.fill.color.a > 0.002f) s.fill.kind = FillKind::solid;
        if (col) {
            std::uint8_t live = s.shadow_count;
            for (std::size_t i = s.shadow_count; i < max_shadows; ++i) {
                if (s.shadows[i].color.a > 0.002f) {
                    live = static_cast<std::uint8_t>(i + 1);
                    s.shadows[i].inset = inset[i];
                }
            }
            s.shadow_count = live;
        }
    }

    /// Only what cannot be interpolated: a gradient's structure.
    [[nodiscard]] static std::uint64_t signature(const Style& s) noexcept {
        const bool gradient = s.fill.kind != FillKind::none && s.fill.kind != FillKind::solid;
        return gradient ? (static_cast<std::uint64_t>(s.fill.kind) << 8) | s.fill.stop_count : 0u;
    }

    /// A colour that appears from nothing should fade in from its OWN hue at
    /// zero alpha, not from whatever the invisible value was (black, for a
    /// default-constructed colour): springing Oklab from (0,0,0,0) visibly
    /// darkens the first frames. Likewise a colour fading out keeps its hue.
    static void heal_invisible(Vec& from, Vec& to) {
        for (int i = 0; i < Vec::kColors; ++i) {
            float* f = from.v + i * 4;
            float* t = to.v + i * 4;
            if (f[3] <= 0.001f) { f[0] = t[0]; f[1] = t[1]; f[2] = t[2]; }
            else if (t[3] <= 0.001f) { t[0] = f[0]; t[1] = f[1]; t[2] = f[2]; }
        }
    }

    // Identity across frames: a node's own id when it has one (so a keyed
    // row keeps its spring when rows are inserted above it), otherwise its
    // structural path. Every transitioning node animates, named or not: a
    // chevron that brightens on group hover needs no id to fade.
    void visit(Node& n, double dt, std::uint64_t path) {
        // A memo root: tell the cache whether anything under it is moving, so
        // next frame it's re-resolved (springs need true targets, and a
        // reused subtree holds last frame's in-between values).
        if (n.memo_key() != 0 && memos_ != nullptr) {
            const bool before = moving_now_;
            moving_now_ = false;
            visit_node(n, dt, path);
            memos_->set_animating(n.memo_key(), moving_now_);
            moving_now_ = moving_now_ || before;
            return;
        }
        visit_node(n, dt, path);
    }
    void visit_node(Node& n, double dt, std::uint64_t path) {
        const Style& cs = std::as_const(n).style();
        const std::uint64_t key = (cs.id != 0 && !cs.hit_only) ? cs.id : path;
        if (cs.transition.on()) {
            Style& s = n.style();
            auto [it, fresh] = springs_.try_emplace(key);
            Spring1& sp = it->second;
            sp.frame = frame_;
            // At rest and nothing changed: the resolved style IS the answer.
            if (!fresh && !sp.moving && sp.has_rest && sp.rest == Look::of(s)) {
                visit_children(n, dt, key);
                return;
            }
            if (fresh) {
                // First sight: nothing to interpolate from. Remember the look
                // and nothing else -- the float state is built lazily, the
                // first time this node actually has to MOVE.
                sp.rest = Look::of(s);
                sp.has_rest = true;
                sp.primed = false;
                for (std::size_t i = 0; i < s.shadow_count; ++i) sp.inset[i] = s.shadows[i].inset;
                sp.shape_sig = signature(s);
                visit_children(n, dt, key);
                return;
            }
            Vec target;
            flatten(s, target);
            const std::uint64_t sig = signature(s);
            if (!sp.primed) {
                // Leaving rest for the first time: the spring starts at the
                // look it last rested on.
                Style from = s;
                apply_look(from, sp.rest);
                flatten(from, sp.x);
                sp.vel = {};
                sp.primed = true;
            }
            for (std::size_t i = 0; i < s.shadow_count; ++i) sp.inset[i] = s.shadows[i].inset;
            if (sp.shape_sig != sig) {
                // A gradient changed structure: nothing meaningful to
                // interpolate. Snap.
                const auto keep = sp.inset;
                sp = Spring1{target, {}, target, false, sig, keep, Look::of(s), true, frame_, true};
            } else {
                heal_invisible(sp.x, target);
                sp.target = target;
                // Leaving rest: take one nominal frame NOW rather than the
                // draw's dt. A draw caused by input has dt 0 (no frame tick
                // ran), which would show the old look for a whole extra
                // frame; an idle frame clock's first tick carries a stale
                // gap that would play most of a fast transition at once.
                // Feedback starts on the frame the input lands; real frame
                // times follow.
                step(sp, s.transition, sp.moving ? dt : 1.0 / 120.0);
                if (sp.moving) {
                    unflatten(sp.x, s, sp.inset);
                } else {
                    // Settled this frame: show the exact target (not the
                    // float round-trip through Oklab) and remember it.
                    sp.rest = Look::of(s);
                    sp.has_rest = true;
                }
            }
            if (sp.moving) moving_now_ = true;
        }
        visit_children(n, dt, key);
    }

    void visit_children(Node& n, double dt, std::uint64_t parent_key) {
        std::uint64_t i = 0;
        for (auto& k : n.children()) {
            std::uint64_t h = parent_key ^ (0x9E3779B97F4A7C15ull + ++i);
            h *= 1099511628211ull;
            visit(k, dt, h);
        }
    }

    static void apply_look(Style& s, const Look& l) {
        s.fill = l.fill;
        s.stroke = l.stroke;
        s.text.color = l.text;
        s.shadows = l.shadows;
        s.shadow_count = l.shadow_count;
        s.corners = l.corners;
        s.backdrop = l.backdrop;
        s.opacity = l.opacity;
        s.transform = l.transform;
    }

    static void step(Spring1& sp, const Transition& tr, double dt) {
        constexpr float fixed = 1.0f / 240.0f;
        float remaining = num::clamp(static_cast<float>(dt), 0.0f, 0.25f);
        while (remaining > 0.0f) {
            const float h = num::min(remaining, fixed);
            remaining -= h;
            for (int i = 0; i < Vec::N; ++i) {
                const float a = tr.stiffness * (sp.target.v[i] - sp.x.v[i]) - tr.damping * sp.vel.v[i];
                sp.vel.v[i] += a * h;
                sp.x.v[i] += sp.vel.v[i] * h;
            }
        }
        // Settled when every component is within what a pixel can show.
        bool moving = false;
        for (int i = 0; i < Vec::N && !moving; ++i) {
            if (num::abs(sp.target.v[i] - sp.x.v[i]) > 1e-3f || num::abs(sp.vel.v[i]) > 1e-2f)
                moving = true;
        }
        if (!moving) sp.x = sp.target, sp.vel = {};
        sp.moving = moving;
    }

  public:
    void begin_frame() noexcept { moving_now_ = false; ++frame_; }
    /// Forget nodes that left the tree (so one that comes back starts fresh
    /// instead of springing from where it was minutes ago), and publish
    /// whether anything is still moving.
    void end_frame() {
        std::erase_if(springs_, [&](const auto& kv) { return kv.second.frame != frame_; });
        moving_ = moving_now_;
    }

  private:
    std::unordered_map<std::uint64_t, Spring1> springs_;
    std::uint32_t frame_ = 0;
    bool moving_ = false;
    bool moving_now_ = false;
    MemoCache* memos_ = nullptr;
};

/// Every node that styles its first frame (`starting(...)`), for
/// `CascadeInput::previous_ids`. Only those: a node without a starting rule
/// never needs to know whether it is new, so the set stays tiny.
inline void collect_starting_ids(const Node& n, std::unordered_set<std::uint64_t>& out,
                                 const MemoCache* memos = nullptr) {
    // A reused memo subtree: its ids were recorded when it was cascaded.
    if (n.memo_reused() && memos != nullptr) {
        if (const auto* e = memos->find(n.memo_key()); e != nullptr && e->has_cascaded) {
            out.insert(e->starting_ids.begin(), e->starting_ids.end());
            return;
        }
    }
    const Cascade* cc = n.cascade().get();
    if (n.style().id != 0 && cc != nullptr && (cc->uses & state::bit::starting) != 0)
        out.insert(n.style().id);
    for (const auto& k : n.children()) collect_starting_ids(k, out, memos);
}

}  // namespace mayag
