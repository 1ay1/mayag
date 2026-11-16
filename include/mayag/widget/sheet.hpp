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

#include "../app/host.hpp"
#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

#include <memory>
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
/// controls, and an accent that glows.
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
                           shadow(18.0f, var::accent.fade(0.55f), {0.0f, 4.0f}),
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
        x.slider_fill  = look(bg(var::accent), radius(var::radius_full), shadow(10.0f, var::accent.fade(0.7f)));
        x.slider_knob  = look(bg(var::text_primary), radius(var::radius_full), elevation(var::depth(2)),
                              when(state::group(state::hover), scale(1.15f)),
                              when(state::group(state::pressed), scale(1.3f)),
                              transition(var::motion_emphasis));
        return x;
    }();
    return s;
}

/// Terminal: square, outlined in the accent, phosphor glow, no fills until
/// you touch something.
[[nodiscard]] inline const Sheet& terminal() {
    using namespace dsl;
    static const Sheet s = [] {
        Sheet x = Sheet::standard();
        x.label = look(font(var::font_sm), bold, tracking(1.2f));
        x.button = look(
            pad(var::space(1), var::space(2)), radius(0.0f),
            bg(var::accent.fade(0.0f)), border(var::line, var::accent), fg(var::accent),
            hovered(bg(var::accent), fg(var::on_accent), shadow(14.0f, var::accent.fade(0.6f))),
            pressed(bg(var::accent_active), fg(var::on_accent)),
            focus_visible(border(var::line_focus, var::focus_ring)),
            when(state::disabled, opacity(0.4f)),
            transition());
        x.primary = extend(x.button, bg(var::accent.fade(0.16f)));
        x.danger  = extend(x.button, border_color(var::danger), fg(var::danger),
                           hovered(bg(var::danger), fg(var::on_danger)));
        x.ghost   = extend(x.button, border_color(var::accent.fade(0.3f)));
        x.card  = look(pad(var::space(2)), gap(var::space(1)), radius(0.0f), bg(var::surface),
                       border(var::line, var::accent.fade(0.45f)), shadow(18.0f, var::accent.fade(0.10f)));
        x.panel = x.card;
        x.glass = x.card;
        x.track = look(radius(0.0f), bg(var::accent.fade(0.0f)), border(var::line, var::accent),
                       when(state::checked, bg(var::accent.fade(0.25f))),
                       focus_visible(border(var::line_focus, var::focus_ring)),
                       when(state::disabled, opacity(0.4f)), transition());
        x.knob  = look(radius(0.0f), bg(var::accent.fade(0.5f)),
                       when(state::group(state::checked), bg(var::accent), shadow(10.0f, var::accent.fade(0.8f))),
                       transition(var::motion_emphasis));
        x.slider_track = look(bg(var::accent.fade(0.18f)), radius(0.0f));
        x.slider_fill  = look(bg(var::accent), radius(0.0f), shadow(8.0f, var::accent.fade(0.7f)));
        x.slider_knob  = look(bg(var::accent), radius(0.0f), shadow(12.0f, var::accent.fade(0.8f)),
                              when(state::group(state::pressed), scale(1.25f)), transition());
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

/// Frosted glass over a deep background; glowing accent.
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
