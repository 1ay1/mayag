#pragma once
// mayag::widget sheet — every widget's LOOK, as theme data
//
// A widget is two halves:
//
//   behaviour   structure, ids, hit targets, a11y role and state, cursor.
//               Lives in the widget's header. A theme cannot break it.
//   look        colours, sizes, radii, borders, shadows, type, every state
//               variant and its motion. Lives HERE, in a Sheet the theme
//               points at, so a theme can replace any of it.
//
// Restyle every button in an app:
//
//     static const Sheet brutal = [] {
//         Sheet s = Sheet::standard();
//         s.button = look(pad(10, 18), radius(0), border(2, var::text_primary),
//                         shadow(0, var::text_primary, {4, 4}),
//                         hovered(translate(-2, -2)), pressed(translate(4, 4)),
//                         transition(var::motion_emphasis));
//         return s;
//     }();
//     Theme t = themes::midnight;  t.sheet = &brutal;
//
// The standard sheet is written entirely in tokens (var::accent,
// var::space(2), var::radius_md, var::depth(6), transition()), so ANY theme
// restyles it -- colours, density, roundness, depth, motion -- with no sheet
// of its own. A Sheet is only needed to change a widget's SHAPE.
//
// Cost: a Look is a shared, pre-compiled rule set. Attaching one to a node
// is a pointer copy; the cascade applies it like any other class, and it
// was built exactly once, when the sheet was made.

#include "../core/sync.hpp"
#include "../app/context.hpp"
#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <utility>

namespace mayag {

/// A type-erased class: a BASE style plus state variants, built once.
///
/// The base is a pre-applied Style patch: attaching a look copies the fields
/// the look set (and only those) into the element at view time, exactly as
/// piping the modifiers would -- so anything the caller pipes AFTER the
/// widget (`button(...) | bg(red)`) wins, as it should. The variants are a
/// shared, pre-sorted rule set the cascade runs when their state holds.
struct Look {
    std::shared_ptr<const Style>   base{};    ///< base modifiers, applied to a default Style
    std::shared_ptr<const Cascade> rules{};   ///< state variants (null when there are none)
    std::uint64_t                  sets = 0;  ///< which base field groups the look touched

    [[nodiscard]] bool empty() const noexcept { return !base && !rules; }
};

}  // namespace mayag
// A Look may be shared, frozen, by threads (sheets are process-wide): its
// Style and Cascade are built whole by look() and its builders, then only ever
// reached through shared_ptr<const> -- no code holds a non-const alias once
// a Look is made -- and neither Style nor Cascade has a mutable member.
template <> inline constexpr bool jaal::frozen_opt_in<mayag::Look>   = true;
template <> inline constexpr bool jaal::sendable_opt_in<mayag::Look> = true;
namespace mayag {

namespace look_fields {
inline constexpr std::uint64_t fill = 1u << 0, stroke = 1u << 1, corners = 1u << 2, shadows = 1u << 3,
    padding = 1u << 4, margin = 1u << 5, gap = 1u << 6, size = 1u << 7, opacity = 1u << 8,
    transform = 1u << 9, backdrop = 1u << 10, transition = 1u << 11, align = 1u << 12, text = 1u << 13,
    group = 1u << 14, stroke_color = 1u << 15;
}  // namespace look_fields

namespace dsl {

namespace detail {

/// Which field groups of `s` differ from a default Style.
[[nodiscard]] inline std::uint64_t changed_fields(const Style& s) {
    namespace f = look_fields;
    const Style d{};
    std::uint64_t m = 0;
    if (!(s.fill == d.fill)) m |= f::fill;
    // Width and colour separately: `border_color(c)` in an extension must
    // recolour the base's border, not reset its width to zero.
    if (s.stroke.width != d.stroke.width || s.stroke.align != d.stroke.align) m |= f::stroke;
    if (!(s.stroke.color == d.stroke.color)) m |= f::stroke_color;
    if (!(s.corners == d.corners)) m |= f::corners;
    if (s.shadow_count != 0) m |= f::shadows;
    if (!(s.layout.padding == d.layout.padding)) m |= f::padding;
    if (!(s.layout.margin == d.layout.margin)) m |= f::margin;
    if (s.layout.gap != d.layout.gap) m |= f::gap;
    if (!(s.layout.width == d.layout.width) || !(s.layout.height == d.layout.height)) m |= f::size;
    if (s.opacity != d.opacity) m |= f::opacity;
    if (!(s.transform == d.transform)) m |= f::transform;
    if (!(s.backdrop == d.backdrop)) m |= f::backdrop;
    if (!(s.transition == d.transition)) m |= f::transition;
    if (s.layout.align != d.layout.align || s.layout.justify != d.layout.justify) m |= f::align;
    if (s.text.set != 0) m |= f::text;
    if (s.group) m |= f::group;
    return m;
}

/// Copy the touched groups of `from` onto `to`. Text merges by its own
/// set-bits, so a look's `semibold` does not reset the element's font size.
inline void apply_base(Style& to, const Style& from, std::uint64_t sets) {
    namespace f = look_fields;
    if (sets & f::fill) to.fill = from.fill;
    if (sets & f::stroke) {
        const Color<Srgb> keep = to.stroke.color;
        to.stroke = from.stroke;
        if (!(sets & f::stroke_color)) to.stroke.color = keep;
    }
    if (sets & f::stroke_color) to.stroke.color = from.stroke.color;
    if (sets & f::corners) to.corners = from.corners;
    if (sets & f::shadows) {
        for (std::uint8_t i = 0; i < from.shadow_count && to.shadow_count < max_shadows; ++i)
            to.shadows[to.shadow_count++] = from.shadows[i];
    }
    if (sets & f::padding) to.layout.padding = from.layout.padding;
    if (sets & f::margin) to.layout.margin = from.layout.margin;
    if (sets & f::gap) to.layout.gap = from.layout.gap;
    if (sets & f::size) {
        if (!from.layout.width.is_auto()) to.layout.width = from.layout.width;
        if (!from.layout.height.is_auto()) to.layout.height = from.layout.height;
    }
    if (sets & f::opacity) to.opacity = from.opacity;
    if (sets & f::transform) to.transform = from.transform;
    if (sets & f::backdrop) to.backdrop = from.backdrop;
    if (sets & f::transition) to.transition = from.transition;
    if (sets & f::align) { to.layout.align = from.layout.align; to.layout.justify = from.layout.justify; }
    if (sets & f::group) to.group = true;
    if (sets & f::text) {
        namespace t = text_bits;
        const TextStyle& a = from.text;
        TextStyle& b = to.text;
        if (a.set & t::size) b.size = a.size;
        if (a.set & t::weight) b.weight = a.weight;
        if (a.set & t::italic) b.italic = a.italic;
        if (a.set & t::underline) b.underline = a.underline;
        if (a.set & t::strike) b.strikethrough = a.strikethrough;
        if (a.set & t::color) b.color = a.color;
        if (a.set & t::line_height) b.line_height = a.line_height;
        if (a.set & t::tracking) b.letter_spacing = a.letter_spacing;
        if (a.set & t::align) b.align = a.align;
        if (a.set & t::ligatures) b.ligatures = a.ligatures;
        if (a.set & t::variations) b.variations = a.variations;
        if (a.set & t::family) b.family = a.family;
        b.set |= a.set;
    }
}

}  // namespace detail

/// Build a Look from modifiers and responders, exactly like `styles(...)`.
/// Not capability-checked against an element (a look is shared by many):
/// the widget that applies it is the one that knows what it built.
template <typename... Xs>
    requires ((Responder<Xs> || Modifier<Xs>) && ...)
[[nodiscard]] Look look(Xs... xs) {
    auto base = std::make_shared<Style>();
    std::shared_ptr<Cascade> rules;
    const auto add = [&](auto x) {
        using X = decltype(x);
        if constexpr (Responder<X>) {
            x.on_pipe(*base);
            if (!rules) rules = std::make_shared<Cascade>();
            auto self = std::make_shared<const X>(x);
            rules->keep.push_back(self);
            self->attach(*rules, self);
        } else {
            x.apply(*base);
        }
    };
    (add(xs), ...);
    Look l;
    l.sets = detail::changed_fields(*base);
    l.base = std::move(base);
    l.rules = std::move(rules);
    return l;
}

/// Extend a look: everything in `base`, then these. `extend(s.button, ...)`
/// is how a variant (primary, danger) is "button plus differences".
template <typename... Xs>
    requires ((Responder<Xs> || Modifier<Xs>) && ...)
[[nodiscard]] Look extend(const Look& b, Xs... xs) {
    Look more = look(xs...);
    Look out;
    auto st = std::make_shared<Style>(b.base ? *b.base : Style{});
    if (more.base) detail::apply_base(*st, *more.base, more.sets);
    out.sets = b.sets | more.sets;
    out.base = std::move(st);
    if (b.rules || more.rules) {
        auto c = std::make_shared<Cascade>(b.rules ? *b.rules : Cascade{});
        if (more.rules) {
            for (const auto& k : more.rules->keep) c->keep.push_back(k);
            for (auto r : more.rules->rules) c->add(r);
            c->vars.insert(c->vars.end(), more.rules->vars.begin(), more.rules->vars.end());
        }
        out.rules = std::move(c);
    }
    return out;
}

/// Attach a look to an element: its base lands in the style now (under
/// anything piped after it), its variants join the element's rules, ranked
/// below the element's own variants of the same priority.
struct LookMod {
    static constexpr bool is_responder = true;
    static constexpr caps::Set gives = caps::none;
    template <caps::Set>
    static consteval void check() {}

    const Look* l = nullptr;
    /// Set when the widget was handed a theme other than the frame's: its
    /// subtree then resolves against THAT theme (see with_look(l, t)).
    std::shared_ptr<const Theme> own{};

    void on_pipe(Style& s) const {
        if (l != nullptr && l->base) detail::apply_base(s, *l->base, l->sets);
    }
    /// The look's own Cascade, when it can be shared as-is (see RuleSet):
    /// the common case, a widget in the frame's theme, allocates nothing.
    [[nodiscard]] std::shared_ptr<const Cascade> shared_rules() const {
        return (l != nullptr && !own) ? l->rules : nullptr;
    }
    void attach(Cascade& c, const std::shared_ptr<const LookMod>&) const {
        if (own) c.theme = own;
        if (l == nullptr || !l->rules) return;
        const Cascade& src = *l->rules;
        c.keep.push_back(l->rules);   // one refcount keeps every rule alive
        for (const auto& r : src.rules) c.add(r);
        c.vars.insert(c.vars.end(), src.vars.begin(), src.vars.end());
        if (src.theme && !c.theme) c.theme = src.theme;
    }
};
[[nodiscard]] inline LookMod with_look(const Look& l) { return {&l, nullptr}; }

/// A look with nothing in it: for a widget root that only needs the theme.
inline const Look& no_look() {
    static const Look none{};
    return none;
}

/// A widget's ROOT look, drawn in theme `t`. When `t` is the frame's theme
/// (the usual case: one address compare) this is plain `with_look`. When a
/// caller hands the widget another theme -- a settings preview, a wrapper
/// context, a themed island built at view time -- the widget's subtree
/// resolves every token against `t`, so it looks exactly like the theme it
/// was given rather than half like the window's.
[[nodiscard]] inline LookMod with_look(const Look& l, const Theme& t) {
    const Theme& frame = ctx().theme;
    if (&t == &frame || t == frame) return {&l, nullptr};
    return {&l, std::make_shared<const Theme>(t)};
}

}  // namespace dsl

/// The looks of the standard widgets. Copy `Sheet::standard()` and replace
/// any member; point `Theme::sheet` at the result.
struct Sheet {
    Look button, primary, danger, ghost;   ///< button variants
    Look card, panel, glass;               ///< surfaces
    Look track, knob;                      ///< the on/off switch
    Look slider_track, slider_fill, slider_knob;
    Look label;                            ///< button / control text
    // Chrome a terminal or editor is made of. State lives in the variants:
    // a widget only says `selected(on)` / `checked(on)` and names the node.
    Look strip;     ///< a bar of chrome: the tab strip, the status bar
    Look tab;       ///< one tab (hover, `selected` = active)
    Look mark;      ///< the active-indicator bar inside a `group` (tab, row)
    Look segment;   ///< one option of a segmented control (`checked`)
    Look item;      ///< a row of a list / picker / menu (`selected`)
    Look pane;      ///< a split pane's frame (`selected` = focused)
    Look tab_wash;  ///< light falling from the active tab's indicator (inside a `group`)
    Look icon_button, icon_button_danger;   ///< glyph buttons in toolbars and window chrome

    [[nodiscard]] static const Sheet& standard();
};

/// The sheet a theme selects (its own, or the standard one).
[[nodiscard]] inline const Sheet& sheet_of(const Theme& t) {
    return t.sheet != nullptr ? *t.sheet : Sheet::standard();
}

inline const Sheet& Sheet::standard() {
    using namespace dsl;
    static const Sheet s = [] {
        Sheet x;
        x.label = look(font(var::font_md), semibold);

        // ── buttons ─────────────────────────────────────────────────────
        x.button = look(
            pad(var::space(1.25f), var::space(2)), radius(var::radius_sm),
            bg(var::surface_raised), border(var::line, var::border_strong), fg(var::text_primary),
            hovered(bg(var::tint(var::accent, 0.18f)), border_color(var::accent)),
            pressed(bg(var::accent_active), border_color(var::accent_active),
                    fg(var::on_accent), scale(0.97f)),
            focus_visible(border(var::line_focus, var::focus_ring)),
            when(state::disabled, opacity(0.45f)),
            transition());
        x.primary = extend(x.button,
            bg(var::accent), border_color(var::accent), fg(var::on_accent), elevation(var::depth(3)),
            hovered(bg(var::accent_hover), border_color(var::accent_hover)),
            pressed(bg(var::accent_active), border_color(var::accent_active)));
        x.danger = extend(x.button,
            bg(var::danger), border_color(var::danger), fg(var::on_danger), elevation(var::depth(3)),
            hovered(bg(var::danger), border_color(var::danger), opacity(0.92f)),
            pressed(bg(var::danger), border_color(var::danger)));
        x.ghost = extend(x.button,
            bg(var::surface.fade(0.0f)), border_color(var::border),
            hovered(bg(var::tint(var::accent, 0.12f)), border_color(var::border_strong)));

        // ── surfaces ────────────────────────────────────────────────────
        x.card  = look(pad(var::space(2)), gap(var::space(1)), bg(var::surface),
                       border(var::line, var::border), radius(var::radius_md), elevation(var::depth(6)));
        x.panel = look(pad(var::space(1.5f)), gap(var::space(1)), bg(var::surface),
                       border(var::line, var::border), radius(var::radius_sm));
        x.glass = look(pad(var::space(2)), gap(var::space(1)), bg(var::overlay.fade(0.72f)),
                       border(var::line, var::border_strong.fade(0.6f)), radius(var::radius_lg),
                       backdrop_blur(24.0f), elevation(var::depth(20)));

        // ── switch ──────────────────────────────────────────────────────
        x.track = look(
            radius(var::radius_full), bg(var::surface_raised), border(var::line_strong, var::border_strong),
            hovered(border_color(var::border_strong.fade(1.0f)), bg(var::tint(var::accent, 0.10f))),
            when(state::checked, bg(var::accent), border_color(var::accent)),
            when(state::checked && state::hover, bg(var::accent_hover), border_color(var::accent_hover)),
            when(state::checked && state::pressed, bg(var::accent_active)),
            focus_visible(border(var::line_focus, var::focus_ring)),
            when(state::disabled, opacity(0.45f)),
            transition());
        x.knob = look(
            radius(var::radius_full), bg(var::text_secondary), elevation(var::depth(1.5f)),
            when(state::group(state::hover), bg(var::text_primary)),
            when(state::group(state::checked), bg(var::on_accent)),
            transition(var::motion_emphasis));

        // ── slider ──────────────────────────────────────────────────────
        x.slider_track = look(bg(var::surface_raised), radius(var::radius_full));
        x.slider_fill  = look(bg(var::accent), radius(var::radius_full));
        x.slider_knob  = look(
            bg(var::text_primary), radius(var::radius_full), elevation(var::depth(1.5f)),
            border(0.0f, var::accent),
            when(state::group(state::hover), border(var::line, var::accent)),
            when(state::group(state::pressed), border(var::line_strong, var::accent), elevation(var::depth(3))),
            when(state::group(state::focus_visible), border(var::line_focus, var::focus_ring)),
            transition());

        // ── chrome ──────────────────────────────────────────────────────
        // A strip is a band with a hairline FOOT (an inset shadow, so it
        // costs no layout); tabs are cells divided by a hairline on their
        // right; the active cell is the content's colour and has no foot,
        // so it opens into the pane under it.
        x.strip = look(bg(var::surface));
        x.tab = look(
            bg(var::surface.fade(0.0f)), fg(var::text_secondary),
            inner_shadow(0.0f, var::border, {-1.0f, 0.0f}),
            hovered(bg(var::surface_raised.fade(0.5f)), fg(var::text_primary)),
            when(state::selected, bg(var::background), fg(var::text_primary)),
            transition());
        x.mark = look(bg(var::accent.fade(0.0f)),
                      when(state::group(state::selected), bg(var::accent)), transition());
        x.segment = look(
            radius(var::radius_sm), bg(var::surface_raised.fade(0.0f)), fg(var::text_secondary),
            border(var::line, var::border.fade(0.0f)),
            hovered(bg(var::surface_raised.fade(0.45f)), fg(var::text_primary)),
            when(state::checked, bg(var::surface_raised), fg(var::text_primary), border_color(var::border),
                 elevation(var::depth(1))),
            focus_visible(border_color(var::focus_ring)),
            transition());
        x.item = look(
            radius(var::radius_sm), bg(var::overlay.fade(0.0f)), fg(var::text_secondary),
            hovered(bg(var::surface_raised), fg(var::text_primary)),
            when(state::selected, bg(var::tint(var::accent, 0.20f)), fg(var::text_primary)),
            transition());
        // Panes are cells of the split; the gutters (pane_split's divider)
        // draw the lines between them. Focus is the accent rail on the
        // focused pane's title strip (pane_frame), so the frame itself is bare.
        x.pane = look(transition());
        x.icon_button = look(
            radius(var::radius_sm), bg(var::surface_raised.fade(0.0f)), fg(var::text_secondary),
            hovered(bg(var::surface_raised), fg(var::text_primary)),
            pressed(bg(var::border)),
            when(state::selected, fg(var::accent)),
            focus_visible(border(var::line_focus, var::focus_ring)),
            when(state::disabled, opacity(0.4f)), transition());
        x.icon_button_danger = extend(x.icon_button, hovered(bg(var::danger), fg(var::on_danger)),
                                      pressed(bg(var::danger), fg(var::on_danger), opacity(0.85f)));
        return x;
    }();
    return s;
}

// ════════════════════════════════════════════════════════════════════════
// Stock sheets and looks
// ════════════════════════════════════════════════════════════════════════
//
// Three alternative SHAPES for every widget, and complete looks built on
// them. A look is a function of a Seed, so every knob still works:
//
//     Theme t = looks::glass();                                // as designed
//     Theme t = looks::brutal({.accent = rgb<0xFF4D00>});      // your brand
//     Theme t = looks::terminal({.density = 0.8f});            // denser

namespace sheets {

/// Neo-brutalist: square, thick ink outlines, hard offset shadows; buttons
/// lift on hover and press flat into their shadow.
[[nodiscard]] inline const Sheet& brutal() {
    using namespace dsl;
    static const Sheet s = [] {
        Sheet x = Sheet::standard();
        x.label = look(font(var::font_md), bold);
        x.button = look(
            pad(var::space(1.25f), var::space(2)), radius(0.0f),
            bg(var::surface_raised), border(var::line_focus, var::text_primary), fg(var::text_primary),
            shadow(0.5f, var::text_primary, {4.0f, 4.0f}),
            hovered(translate(-2.0f, -2.0f)),
            pressed(translate(3.0f, 3.0f)),
            focus_visible(border_color(var::focus_ring)),
            when(state::disabled, opacity(0.45f)),
            transition(var::motion_emphasis));
        x.primary = extend(x.button, bg(var::accent), fg(var::on_accent));
        x.danger  = extend(x.button, bg(var::danger), fg(var::on_danger));
        x.ghost   = extend(x.button, bg(var::surface));
        x.card  = look(pad(var::space(2)), gap(var::space(1)), bg(var::surface), radius(0.0f),
                       border(var::line_focus, var::text_primary), shadow(0.5f, var::text_primary, {6.0f, 6.0f}));
        x.panel = look(pad(var::space(1.5f)), gap(var::space(1)), bg(var::surface), radius(0.0f),
                       border(var::line_focus, var::text_primary), shadow(0.5f, var::text_primary, {3.0f, 3.0f}));
        x.glass = x.card;
        x.track = look(radius(0.0f), bg(var::surface_raised), border(var::line_focus, var::text_primary),
                       when(state::checked, bg(var::accent)),
                       focus_visible(border_color(var::focus_ring)),
                       when(state::disabled, opacity(0.45f)), transition());
        x.knob  = look(radius(0.0f), bg(var::text_primary), transition(var::motion_emphasis));
        x.slider_track = look(bg(var::surface_raised), radius(0.0f), border(var::line, var::text_primary));
        x.slider_fill  = look(bg(var::accent), radius(0.0f), border(var::line, var::text_primary));
        x.slider_knob  = look(bg(var::accent), radius(0.0f), border(var::line_focus, var::text_primary),
                              shadow(0.5f, var::text_primary, {2.0f, 2.0f}),
                              when(state::group(state::hover), translate(-1.0f, -1.0f)),
                              when(state::group(state::pressed), translate(1.0f, 1.0f)),
                              transition(var::motion_emphasis));
        return x;
    }();
    return s;
}

/// Glass: translucent, blurred surfaces over whatever is behind them, pill
/// controls, and a bright accent.
[[nodiscard]] inline const Sheet& glass() {
    using namespace dsl;
    static const Sheet s = [] {
        Sheet x = Sheet::standard();
        x.button = look(
            pad(var::space(1.25f), var::space(2.25f)), radius(var::radius_full),
            bg(var::text_primary.fade(0.08f)), border(var::line, var::text_primary.fade(0.18f)),
            fg(var::text_primary),
            hovered(bg(var::text_primary.fade(0.16f)), border_color(var::text_primary.fade(0.32f))),
            pressed(bg(var::accent), fg(var::on_accent), scale(0.96f)),
            focus_visible(border(var::line_focus, var::focus_ring)),
            when(state::disabled, opacity(0.45f)),
            transition());
        x.primary = extend(x.button, bg(var::accent.fade(0.9f)), border_color(var::accent), fg(var::on_accent),
                           hovered(bg(var::accent), border_color(var::accent)));
        x.danger  = extend(x.button, bg(var::danger.fade(0.85f)), border_color(var::danger), fg(var::on_danger),
                           shadow(18.0f, var::danger.fade(0.5f), {0.0f, 4.0f}));
        x.ghost   = extend(x.button, bg(var::text_primary.fade(0.0f)), border_color(var::text_primary.fade(0.0f)));
        x.card  = look(pad(var::space(2.25f)), gap(var::space(1)), radius(var::radius_lg),
                       bg(var::surface.fade(0.42f)), backdrop_blur(28.0f, 1.8f),
                       border(var::line, var::text_primary.fade(0.14f)), elevation(var::depth(10)));
        x.panel = x.card;
        x.glass = x.card;
        x.track = look(radius(var::radius_full), bg(var::text_primary.fade(0.10f)),
                       border(var::line, var::text_primary.fade(0.18f)),
                       when(state::checked, bg(var::accent), border_color(var::accent)),
                       focus_visible(border(var::line_focus, var::focus_ring)),
                       when(state::disabled, opacity(0.45f)), transition());
        x.knob  = look(radius(var::radius_full), bg(var::text_primary), elevation(var::depth(2)),
                       when(state::group(state::checked), bg(var::on_accent)), transition(var::motion_emphasis));
        x.slider_track = look(bg(var::text_primary.fade(0.12f)), radius(var::radius_full));
        x.slider_fill  = look(bg(var::accent), radius(var::radius_full));
        x.slider_knob  = look(bg(var::text_primary), radius(var::radius_full), elevation(var::depth(2)),
                              when(state::group(state::hover), scale(1.15f)),
                              when(state::group(state::pressed), scale(1.3f)),
                              transition(var::motion_emphasis));
        return x;
    }();
    return s;
}

/// Terminal: square, outlined in the accent, no fills until
/// you touch something.
[[nodiscard]] inline const Sheet& terminal() {
    using namespace dsl;
    static const Sheet s = [] {
        Sheet x = Sheet::standard();
        x.label = look(font(var::font_sm), bold, tracking(1.2f));
        x.button = look(
            pad(var::space(1), var::space(2)), radius(0.0f),
            bg(var::accent.fade(0.0f)), border(var::line, var::accent), fg(var::accent),
            hovered(bg(var::accent), fg(var::on_accent)),
            pressed(bg(var::accent_active), fg(var::on_accent)),
            focus_visible(border(var::line_focus, var::focus_ring)),
            when(state::disabled, opacity(0.4f)),
            transition());
        x.primary = extend(x.button, bg(var::accent.fade(0.16f)));
        x.danger  = extend(x.button, border_color(var::danger), fg(var::danger),
                           hovered(bg(var::danger), fg(var::on_danger)));
        x.ghost   = extend(x.button, border_color(var::accent.fade(0.3f)));
        x.card  = look(pad(var::space(2)), gap(var::space(1)), radius(0.0f), bg(var::surface),
                       border(var::line, var::accent.fade(0.45f)));
        x.panel = x.card;
        x.glass = x.card;
        x.track = look(radius(0.0f), bg(var::accent.fade(0.0f)), border(var::line, var::accent),
                       when(state::checked, bg(var::accent.fade(0.25f))),
                       focus_visible(border(var::line_focus, var::focus_ring)),
                       when(state::disabled, opacity(0.4f)), transition());
        x.knob  = look(radius(0.0f), bg(var::accent.fade(0.5f)),
                       when(state::group(state::checked), bg(var::accent)),
                       transition(var::motion_emphasis));
        x.slider_track = look(bg(var::accent.fade(0.18f)), radius(0.0f));
        x.slider_fill  = look(bg(var::accent), radius(0.0f));
        x.slider_knob  = look(bg(var::accent), radius(0.0f),
                              when(state::group(state::pressed), scale(1.25f)), transition());

        // Chrome: hairlines, square, and light only where the eye should go.
        x.strip = look(bg(var::surface));
        x.tab = look(
            bg(var::surface.fade(0.0f)), fg(var::text_secondary),
            inner_shadow(0.0f, var::border.fade(0.7f), {-1.0f, 0.0f}),
            hovered(bg(var::accent.fade(0.05f)), fg(var::text_secondary)),
            when(state::selected, bg(var::background), fg(var::text_primary)),
            transition());
        x.mark = look(bg(var::accent.fade(0.0f)),
                      when(state::group(state::selected), bg(var::accent)),
                      transition());
        x.segment = look(
            radius(0.0f), bg(var::accent.fade(0.0f)), fg(var::text_secondary),
            border(var::line, var::accent.fade(0.0f)),
            hovered(bg(var::accent.fade(0.06f)), fg(var::text_primary)),
            when(state::checked, bg(var::accent.fade(0.13f)), fg(var::accent), border_color(var::accent.fade(0.65f)),
                 border_color(var::accent.fade(0.65f))),
            focus_visible(border_color(var::focus_ring)),
            transition());
        x.item = look(
            radius(0.0f), bg(var::accent.fade(0.0f)), fg(var::text_secondary),
            border(var::line, var::accent.fade(0.0f)),
            hovered(bg(var::accent.fade(0.05f)), fg(var::text_primary)),
            when(state::selected, bg(var::accent.fade(0.11f)), fg(var::text_primary), border_color(var::accent.fade(0.45f))),
            transition());
        // Panes are cells of the split; the gutters (pane_split's divider)
        // draw the lines between them. Focus is the accent rail on the
        // focused pane's title strip (pane_frame), so the frame itself is bare.
        x.pane = look(transition());
        return x;
    }();
    return s;
}

}  // namespace sheets

/// Every knob of a widget's LOOK, as data: what `sheets::tuned` builds a
/// Sheet from. Shape (radius, spacing, line width, depth, motion) stays in
/// the Theme's Seed; colour in its roles; these say how the roles are
/// USED -- how strongly selection fills, whether edges are drawn, how much
/// light there is. Every field 0..1 unless noted, 0 meaning "none".
struct SheetKnobs {
    float accent_fill = 0.16f;    ///< selected / checked / active: accent wash strength
    float accent_edge = 0.55f;    ///< ...and their outline in the accent
    float glow        = 0.0f;     ///< accent light around active things (0 = none, keep it low)
    float hover       = 0.5f;     ///< hover wash strength
    float outline     = 0.6f;     ///< resting control borders
    float float_shadow = 0.7f;    ///< shadow under menus, dialogs, panels
    float gradient    = 0.5f;     ///< chrome bars shade from an accent-tinted top
    float label_weight = 500.0f;  ///< control label weight (300..800)
    float label_tracking = 0.0f;  ///< control label letter spacing, px (-0.5..2)
    bool  filled_primary = true;  ///< the primary button is a solid accent block
    float outline_accent = 0.0f;  ///< resting edges and control text drawn in the accent (1 = the terminal look)
    friend constexpr bool operator==(const SheetKnobs&, const SheetKnobs&) = default;

    /// Knobs quantised to 1/40: a slider drag builds a bounded number of
    /// sheets (each is cached for the life of the process, because a theme
    /// in the middle of a morph may still point at it).
    [[nodiscard]] std::uint64_t key() const noexcept {
        const auto q = [](float v, float lo, float hi) {
            return static_cast<std::uint64_t>(std::lround(std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) * 40.0f));
        };
        std::uint64_t k = 0;
        for (const auto v : {q(accent_fill, 0, 1), q(accent_edge, 0, 1), q(glow, 0, 1), q(hover, 0, 1), q(outline, 0, 1),
                             q(float_shadow, 0, 1), q(gradient, 0, 1), q(label_weight, 300, 800), q(label_tracking, -0.5f, 2.0f)})
            k = k * 41 + v;
        k = k * 41 + q(outline_accent, 0, 1);
        return k * 2 + (filled_primary ? 1 : 0);
    }
};

namespace sheets {

namespace detail {
/// The sheet for `k`, built once per (quantised) knob set.
[[nodiscard]] inline Sheet build_tuned(const SheetKnobs& k);
}

/// A Sheet built from knobs (see SheetKnobs). Stable address per knob set,
/// so `Theme::sheet` may point at it.
///
/// Process-wide: two runtimes on two threads (a window, a headless test) may
/// both ask. Each Sheet is built whole, then frozen and kept for the life of
/// the process (a program uses a handful of knob sets), so its address is
/// stable and readers never race; the table itself is a Cell.
[[nodiscard]] inline const Sheet& tuned(const SheetKnobs& k) {
    using Table = std::unordered_map<std::uint64_t, Frozen<Sheet>>;
    static Cell<Table> cache;
    const std::uint64_t key = k.key();
    if (auto hit = cache.read([](const Table& t, std::uint64_t key) -> std::optional<Frozen<Sheet>> {
            if (auto it = t.find(key); it != t.end()) return it->second;
            return std::nullopt;
        }, key))
        return **hit;   // kept alive by the table, forever
    auto made = Frozen<Sheet>::make(detail::build_tuned(k));   // outside the lock: building is slow
    return *cache.with([](Table& t, std::uint64_t key, Frozen<Sheet> f) {
        return t.try_emplace(key, std::move(f)).first->second;   // the first one in wins
    }, key, std::move(made));
}

[[nodiscard]] inline Sheet detail::build_tuned(const SheetKnobs& k) {
    using namespace dsl;
    Sheet x = Sheet::standard();
    const float af = std::clamp(k.accent_fill, 0.0f, 1.0f), ae = std::clamp(k.accent_edge, 0.0f, 1.0f);
    const float gl = std::clamp(k.glow, 0.0f, 1.0f), hv = std::clamp(k.hover, 0.0f, 1.0f);
    const float ol = std::clamp(k.outline, 0.0f, 1.0f), fs = std::clamp(k.float_shadow, 0.0f, 1.0f);
    const float gr = std::clamp(k.gradient, 0.0f, 1.0f);
    // Light from active things, only when asked for, and soft.
    const auto glow = [&](float scale) { return shadow(14.0f * scale, var::accent.fade(0.45f * gl), {0.0f, 0.0f}); };
    const auto float_shadows = [&] {
        return styles(shadow(36.0f, colors::black.fade(0.38f * fs), {0.0f, 14.0f}, -8.0f),
                      shadow(2.0f, colors::black.fade(0.22f * fs), {0.0f, 1.0f}));
    };

    x.label = look(font(var::font_md), weight(k.label_weight), tracking(k.label_tracking));

    // ── buttons ─────────────────────────────────────────────────────────
    const float oa = std::clamp(k.outline_accent, 0.0f, 1.0f);
    // Resting edges: neutral, or (the terminal look) the accent itself.
    const auto edge = oa >= 0.5f ? var::accent.fade(0.30f + 0.70f * ol) : var::border_strong.fade(0.35f + 0.65f * ol);
    const auto ink = oa >= 0.5f ? var::accent : var::text_primary;
    x.button = look(
        pad(var::space(0.75f), var::space(1.5f)), radius(var::radius_sm),
        bg(oa >= 0.5f ? var::surface_raised.fade(0.0f) : var::surface_raised), border(var::line, edge), fg(ink),
        hovered(bg(var::tint(var::accent, 1.0f - 0.16f * hv)), border_color(var::accent.fade(0.4f + 0.6f * ae))),
        pressed(bg(var::accent.fade(0.25f + 0.5f * af)), scale(0.985f)),
        focus_visible(border(var::line_focus, var::focus_ring)),
        when(state::disabled, opacity(0.4f)), transition());
    x.primary = k.filled_primary
        ? extend(x.button, bg(var::accent), border_color(var::accent), fg(var::on_accent),
                 hovered(bg(var::accent_hover), border_color(var::accent_hover), glow(1.0f)),
                 pressed(bg(var::accent_active), border_color(var::accent_active)))
        : extend(x.button, bg(var::accent.fade(af)), border_color(var::accent.fade(0.4f + 0.6f * ae)), fg(var::accent),
                 hovered(bg(var::accent.fade(std::min(1.0f, af + 0.12f * hv))), glow(1.0f)));
    x.danger = extend(x.button, fg(var::danger), border_color(var::danger.fade(0.5f)),
                      hovered(bg(var::danger), border_color(var::danger), fg(var::on_danger)));
    x.ghost = extend(x.button, bg(var::surface_raised.fade(0.0f)), border_color(var::border.fade(0.0f)),
                     fg(var::text_secondary),
                     hovered(bg(var::surface_raised.fade(0.4f + 0.6f * hv)), border_color(var::border), fg(var::text_primary)));

    // ── surfaces ────────────────────────────────────────────────────────
    x.card  = look(pad(var::space(2)), gap(var::space(1)), bg(var::surface),
                   border(var::line, oa >= 0.5f ? var::accent.fade(0.25f + 0.4f * ol) : var::border.fade(0.3f + 0.7f * ol)),
                   radius(var::radius_md));
    x.panel = look(pad(var::space(1.5f)), gap(var::space(1)),
                   linear_gradient(var::tint(var::accent, 0.06f * gr), var::surface),
                   border(var::line, var::border_strong), radius(var::radius_md), float_shadows());
    x.glass = extend(x.panel, bg(var::overlay.fade(0.82f)), backdrop_blur(24.0f));

    // ── switch ──────────────────────────────────────────────────────────
    x.track = look(
        radius(var::radius_full), bg(var::background), border(var::line, edge),
        hovered(border_color(var::text_disabled)),
        when(state::checked, bg(var::accent), border_color(var::accent), glow(0.8f)),
        focus_visible(border(var::line_focus, var::focus_ring)),
        when(state::disabled, opacity(0.4f)), transition());
    x.knob = look(radius(var::radius_full), bg(var::text_secondary),
                  when(state::group(state::hover), bg(var::text_primary)),
                  when(state::group(state::checked), bg(var::on_accent)), transition(var::motion_ui));

    // ── slider ──────────────────────────────────────────────────────────
    x.slider_track = look(bg(var::border), radius(var::radius_full));
    x.slider_fill  = look(bg(var::accent), radius(var::radius_full), glow(0.6f));
    x.slider_knob  = look(bg(var::text_primary), radius(var::radius_full), border(var::line_strong, var::background),
                          when(state::group(state::hover), bg(var::accent_hover)),
                          when(state::group(state::pressed), bg(var::accent), scale(1.12f)),
                          when(state::group(state::focus_visible), border(var::line, var::focus_ring)), transition());

    // ── chrome ──────────────────────────────────────────────────────────
    // gradient 0 = a flat neutral bar (the surface); up to a 10% accent wash at the top
    x.strip = look(linear_gradient(var::tint(var::accent, 0.10f * gr), var::surface));
    x.tab = look(
        bg(var::surface.fade(0.0f)), fg(var::text_secondary),
        inner_shadow(0.0f, var::border.fade(0.4f + 0.6f * ol), {-1.0f, 0.0f}),
        hovered(bg(var::surface_raised.fade(0.3f + 0.6f * hv)), fg(var::text_primary)),
        when(state::selected, bg(var::background), fg(var::text_primary)), transition());
    x.mark = look(bg(var::accent.fade(0.0f)), when(state::group(state::selected), bg(var::accent), glow(0.7f)),
                  transition());
    x.icon_button = look(
        radius(var::radius_sm), bg(var::surface_raised.fade(0.0f)), fg(var::text_secondary),
        hovered(bg(var::surface_raised.fade(0.35f + 0.65f * hv)), fg(var::text_primary)),
        pressed(bg(var::accent.fade(0.15f + 0.4f * af))),
        when(state::selected, fg(var::accent), bg(var::accent.fade(af * 0.6f))),
        focus_visible(border(var::line_focus, var::focus_ring)),
        when(state::disabled, opacity(0.4f)), transition());
    x.icon_button_danger = extend(x.icon_button, hovered(bg(var::danger), fg(var::on_danger)),
                                  pressed(bg(var::danger), fg(var::on_danger), opacity(0.85f)));
    x.tab_wash = look(linear_gradient(var::accent.fade(0.10f + 0.45f * af), var::accent.fade(0.0f)),
                      opacity(0.0f), when(state::group(state::hover), opacity(0.45f)),
                      when(state::group(state::selected), opacity(1.0f)), transition());
    x.segment = look(
        radius(var::radius_sm), bg(var::accent.fade(0.0f)), fg(var::text_secondary),
        border(var::line, var::accent.fade(0.0f)),
        hovered(bg(var::surface_raised.fade(0.5f * hv)), fg(var::text_primary)),
        when(state::checked, bg(var::accent.fade(af)), fg(var::text_primary), border_color(var::accent.fade(ae)),
             glow(0.6f)),
        focus_visible(border_color(var::focus_ring)), transition());
    x.item = look(
        radius(var::radius_sm), bg(var::accent.fade(0.0f)), fg(var::text_secondary),
        hovered(bg(var::surface_raised.fade(0.3f + 0.6f * hv)), fg(var::text_primary)),
        when(state::selected, bg(var::accent.fade(af)), fg(var::text_primary)), transition());
    x.pane = look(transition());
    return x;
}

/// Refined: the look of a serious tool. Flat; depth comes from TONE (each
/// surface a small lightness step from the one behind it) and hairlines,
/// never from glow. The accent marks state -- checked, selected, the
/// primary action, focus -- and nothing else. Focus is a crisp 1px ring.
/// Only things that float (menus, dialogs) cast a shadow, and it is
/// neutral and soft. Every size and radius is a token, so density and
/// roundness knobs reach it.
[[nodiscard]] inline const Sheet& refined() {
    using namespace dsl;
    static const Sheet s = [] {
        Sheet x = Sheet::standard();
        x.label = look(font(var::font_md), weight(FontWeight::medium));

        // ── buttons: quiet by default, one strong primary ────────────────
        x.button = look(
            pad(var::space(0.75f), var::space(1.5f)), radius(var::radius_sm),
            bg(var::surface_raised), border(var::line, var::border), fg(var::text_primary),
            hovered(bg(var::tint(var::text_primary, 0.94f)), border_color(var::border_strong)),
            pressed(bg(var::surface)),
            focus_visible(border(var::line, var::focus_ring)),
            when(state::disabled, opacity(0.4f)),
            transition());
        x.primary = extend(x.button,
            bg(var::accent), border_color(var::accent), fg(var::on_accent),
            hovered(bg(var::accent_hover), border_color(var::accent_hover)),
            pressed(bg(var::accent_active), border_color(var::accent_active)));
        x.danger = extend(x.button, fg(var::danger),
            hovered(bg(var::danger), border_color(var::danger), fg(var::on_danger)));
        x.ghost = extend(x.button,
            bg(var::surface_raised.fade(0.0f)), border_color(var::border.fade(0.0f)), fg(var::text_secondary),
            hovered(bg(var::surface_raised), border_color(var::border), fg(var::text_primary)));

        // ── surfaces ────────────────────────────────────────────────────
        x.card  = look(pad(var::space(2)), gap(var::space(1)), bg(var::surface),
                       border(var::line, var::border), radius(var::radius_md));
        x.panel = look(pad(var::space(1.5f)), gap(var::space(1)), bg(var::surface),
                       border(var::line, var::border_strong), radius(var::radius_md),
                       shadow(32.0f, colors::black.fade(0.28f), {0.0f, 12.0f}, -8.0f),
                       shadow(2.0f, colors::black.fade(0.18f), {0.0f, 1.0f}));
        x.glass = x.panel;

        // ── switch: a recessed track; the knob is a flat disc ─────────
        x.track = look(
            radius(var::radius_full), bg(var::background), border(var::line, var::border_strong),
            hovered(border_color(var::text_disabled)),
            when(state::checked, bg(var::accent), border_color(var::accent)),
            when(state::checked && state::hover, bg(var::accent_hover), border_color(var::accent_hover)),
            focus_visible(border(var::line, var::focus_ring)),
            when(state::disabled, opacity(0.4f)),
            transition());
        x.knob = look(
            radius(var::radius_full), bg(var::text_secondary),
            when(state::group(state::hover), bg(var::text_primary)),
            when(state::group(state::checked), bg(var::on_accent)),
            transition(var::motion_ui));

        // ── slider: a hairline track, the filled part in the accent ──────
        x.slider_track = look(bg(var::border), radius(var::radius_full));
        x.slider_fill  = look(bg(var::accent), radius(var::radius_full));
        x.slider_knob  = look(
            bg(var::text_primary), radius(var::radius_full), border(var::line_strong, var::background),
            when(state::group(state::hover), bg(var::accent_hover)),
            when(state::group(state::pressed), bg(var::accent)),
            when(state::group(state::focus_visible), border(var::line, var::focus_ring)),
            transition());

        // ── chrome ──────────────────────────────────────────────────────
        x.strip = look(bg(var::surface));
        x.tab = look(
            bg(var::surface.fade(0.0f)), fg(var::text_secondary),
            inner_shadow(0.0f, var::border, {-1.0f, 0.0f}),
            hovered(bg(var::surface_raised.fade(0.6f)), fg(var::text_primary)),
            when(state::selected, bg(var::background), fg(var::text_primary)),
            transition());
        x.mark = look(bg(var::accent.fade(0.0f)), when(state::group(state::selected), bg(var::accent)), transition());
        x.segment = look(
            radius(var::radius_sm), bg(var::surface_raised.fade(0.0f)), fg(var::text_secondary),
            border(var::line, var::border.fade(0.0f)),
            hovered(fg(var::text_primary)),
            when(state::checked, bg(var::surface_raised), fg(var::text_primary), border_color(var::border_strong)),
            focus_visible(border_color(var::focus_ring)),
            transition());
        x.item = look(
            radius(var::radius_sm), bg(var::surface_raised.fade(0.0f)), fg(var::text_secondary),
            hovered(bg(var::surface_raised.fade(0.7f)), fg(var::text_primary)),
            when(state::selected, bg(var::surface_raised), fg(var::text_primary)),
            transition());
        x.pane = look(transition());
        return x;
    }();
    return s;
}

}  // namespace sheets

namespace looks {

/// The standard widgets, with `derive`'s knobs.
[[nodiscard]] inline Theme standard(const Seed& s = {}) { return derive(s); }

/// Neo-brutalism: cream paper, ink outlines, hard shadows, loud accent.
[[nodiscard]] inline Theme brutal(Seed s = {.accent = rgb<0xFFD23F>, .mode = Mode::light,
                                            .roundness = 0.0f, .depth = 0.0f, .lines = 1.4f}) {
    Theme t = derive(s);
    if (s.mode == Mode::light) {
        t.background     = rgb<0xF3EEE3>;
        t.surface        = rgb<0xFFFDF8>;
        t.surface_raised = rgb<0xFFFFFF>;
        t.text_primary   = rgb<0x111111>;
        t.text_secondary = rgb<0x3A3A3A>;
        t.border         = rgb<0x111111>;
        t.border_strong  = rgb<0x111111>;
    }
    t.sheet = &sheets::brutal();
    return t;
}

/// Frosted glass over a deep background.
[[nodiscard]] inline Theme glass(Seed s = {.accent = rgb<0x9B6CFF>, .mode = Mode::dark, .roundness = 1.6f,
                                           .depth = 1.4f, .motion = 0.9f}) {
    Theme t = derive(s);
    if (s.mode == Mode::dark) {
        t.background = mix(t.background, colors::black, 0.55f);
        t.surface    = mix(t.surface, t.accent, 0.06f);
    }
    t.sheet = &sheets::glass();
    return t;
}

/// Tuned: the standard shapes, every visual knob from `k` (SheetKnobs).
[[nodiscard]] inline Theme tuned(const Seed& s, const SheetKnobs& k) {
    Theme t = derive(s);
    t.sheet = &sheets::tuned(k);
    return t;
}

/// Refined: neutral, flat, precise. A low-chroma accent by default, so
/// it reads as a tool, not a toy; surfaces stepped in Oklch lightness.
[[nodiscard]] inline Theme refined(Seed s = {.accent = rgb<0x6E8BF5>, .mode = Mode::dark, .roundness = 0.7f,
                                             .density = 0.9f, .depth = 0.0f}) {
    Theme t = derive(s);
    t.sheet = &sheets::refined();
    return t;
}

/// Phosphor terminal: near-black, everything tinted by the accent.
[[nodiscard]] inline Theme terminal(Seed s = {.accent = rgb<0x3DFF8C>, .mode = Mode::dark,
                                              .roundness = 0.0f, .depth = 0.0f, .motion = 1.3f}) {
    Theme t = derive(s);
    const Color<Srgb> ink = t.accent;
    t.background     = mix(colors::black, ink, 0.02f);
    t.surface        = mix(colors::black, ink, 0.05f);
    t.surface_raised = mix(colors::black, ink, 0.09f);
    t.overlay        = mix(colors::black, ink, 0.07f);
    t.text_primary   = mix(colors::white, ink, 0.55f);
    t.text_secondary = mix(t.background, ink, 0.62f);
    t.text_disabled  = mix(t.background, ink, 0.34f);
    t.border         = mix(t.background, ink, 0.22f);
    t.border_strong  = mix(t.background, ink, 0.45f);
    t.focus_ring     = ink;
    t.on_accent      = colors::black;
    t.sheet = &sheets::terminal();
    return t;
}

}  // namespace looks

namespace dsl {
/// The current frame's sheet: `look_of(c, &Sheet::button)`.
template <class C>
[[nodiscard]] LookMod look_of(const C& c, Look Sheet::*member) {
    return with_look(sheet_of(c.theme).*member);
}
}  // namespace dsl

}  // namespace mayag
