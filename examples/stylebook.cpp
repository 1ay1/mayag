// examples/stylebook.cpp — every feature of mayag's styling system, on one screen
//
// Nine cards, one feature each. Every look here is a constexpr CLASS in the
// `book` namespace below: base style, state variants, theme tokens and a
// transition in one value. view() describes structure and data only; it
// never asks whether anything is hovered, pressed or focused.
//
//   1 Inheritance     fg / font / tracking / italic on a container
//   2 States          hovered, pressed, focus_visible, disabled; class composition
//   3 Tokens          var:: roles, vars() islands, aliasing, named tokens
//   4 Groups          a row's dot, subtitle and chevron react to the ROW
//   5 Switches        state::checked through a group, a bouncy spring
//   6 Disclosure      state::expanded, and starting(...) entry animation
//   7 Fields          focus_within on a container, flag<N> app state
//   8 Motion          entries spring in; hover nudges; springs, not tweens
//   9 Environment     state::narrower / wider breakpoints, state::dark / light
//
// Try it: hover anything, press and drag off a button, Tab through the
// controls (rings appear for keyboard focus only), resize the window narrow,
// switch the theme.
//
// Build:  cmake --build build && ./build/examples/mayag_stylebook
// (--headless runs the self-checks below; --png writes a screenshot)

#include <mayag/mayag.hpp>
#include <mayag/widget/scroll_view.hpp>
#include <mayag/widget/toast.hpp>
#include <mayag/widget/button.hpp>
#include <mayag/widget/card.hpp>
#include <mayag/widget/slider.hpp>
#include <mayag/widget/toggle.hpp>

#include "harness.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

// ════════════════════════════════════════════════════════════════════════
// The stylesheet
// ════════════════════════════════════════════════════════════════════════

namespace book {

// Motion. transition() is critically damped (quick, no overshoot): right for
// feedback. Things that MOVE get a spring with some life in it.
constexpr auto motion  = transition();
constexpr auto springy = transition(Spring::bouncy());

// Type.
constexpr auto muted   = styles(font(var::font_sm), fg(var::text_secondary));
constexpr auto caption = styles(muted, wrap_text, line_height(1.4f));
constexpr auto h2      = styles(font(var::font_lg), semibold, fg(var::text_primary));

// A card. Lifts on hover: the translate moves paint, not layout, so the
// card never shifts out from under the pointer.
constexpr auto section = styles(
    width(360), pad(var::space(2.25f)), gap(var::space(1.5f)), radius(var::radius_lg),
    bg(var::surface), border(var::line, var::border),
    hovered(border_color(var::border_strong), elevation(var::depth(6)), translate(0, -2)),
    motion);

// Buttons. `primary` and `ghost` are `btn` plus differences.
constexpr auto btn = styles(
    pad(var::space(1), var::space(1.75f)), radius(var::radius_sm), center,
    bg(var::surface_raised), border(var::line, var::border),
    fg(var::text_primary), semibold, font(var::font_sm),
    hovered(bg(var::tint(var::accent, 0.22f)), border_color(var::accent)),
    pressed(bg(var::accent), fg(var::on_accent), scale(0.95f)),
    focus_visible(border(2, var::focus_ring)),      // keyboard focus only
    when(state::disabled, opacity(0.4f), bg(var::surface_raised),
         border_color(var::border), transform(Affine::identity())),
    motion);

constexpr auto primary = styles(
    btn, bg(var::accent), border_color(var::accent), fg(var::on_accent),
    hovered(bg(var::accent_hover), border_color(var::accent_hover)),
    pressed(bg(var::accent_active)),
    when(state::disabled, bg(var::accent), border_color(var::accent)));

constexpr auto ghost = styles(
    btn, bg(colors::transparent), border_color(colors::transparent), fg(var::text_secondary),
    hovered(border_color(colors::transparent), fg(var::text_primary)));

// List rows. `group()` lets the dot, subtitle and chevron inside respond to
// the ROW's state, not just their own.
constexpr auto row = styles(
    pad(var::space(1), var::space(1.25f)), gap(var::space(1.25f)), radius(var::radius_md), align(Align::center),
    hovered(bg(var::tint(var::accent, 0.12f))),
    when(state::selected, bg(var::tint(var::accent, 0.22f))),
    group(), motion);
constexpr auto row_dot = styles(
    size(8, 8), radius(4), bg(var::text_disabled),
    group_hovered(bg(var::text_secondary)),
    when(state::group(state::selected), bg(var::accent)),
    motion);
constexpr auto row_meta = styles(font(12), fg(var::text_disabled), group_hovered(fg(var::text_secondary)), motion);
constexpr auto chevron  = styles(font(16), fg(var::text_disabled),
                                 group_hovered(fg(var::accent), translate(3, 0)), motion);

// A switch: the track carries the state, the knob reads it through the group.
constexpr auto track = styles(
    size(40, 24), pad(3), radius(12), align(Align::center),
    bg(var::surface_raised), border(1, var::border_strong),
    hovered(border_color(var::accent)),
    when(state::checked, bg(var::accent), border_color(var::accent)),
    focus_visible(border(2, var::focus_ring)),
    when(state::disabled, opacity(0.4f)),
    group(), motion);
constexpr auto knob = styles(
    size(18, 18), radius(9), bg(var::text_secondary), elevation(1.5f),
    when(state::group(state::checked), translate(16, 0), bg(var::on_accent)),
    springy);

// Disclosure.
constexpr auto disclosure = styles(
    pad(var::space(1.25f), var::space(1.5f)), gap(var::space(1)), radius(var::radius_md), align(Align::center),
    hovered(bg(var::tint(var::accent, 0.10f))),
    when(state::expanded, bg(var::surface_raised)),
    group(), motion);
constexpr auto caret  = styles(font(16), fg(var::text_disabled),
                               when(state::group(state::expanded), fg(var::accent)), motion);
// Fades and slides in on the first frame it exists.
constexpr auto reveal = styles(starting(opacity(0.0f), translate(0, -6)), motion);

// Fields.
constexpr auto input = styles(
    pad(var::space(1.125f), var::space(1.5f)), radius(var::radius_sm), align(Align::center),
    bg(var::background), border(var::line, var::border),
    hovered(border_color(var::border_strong)),
    focus_within(border_color(var::accent), bg(var::surface)),   // focus is INSIDE
    motion);
constexpr auto doc_field = styles(
    pad(var::space(1.125f), var::space(1.5f)), gap(var::space(1)), radius(var::radius_sm), align(Align::center),
    bg(var::background), border(var::line, var::border),
    when(state::flag<0>, border_color(var::warning), bg(var::tint(var::warning, 0.08f))),
    group(), motion);
constexpr auto dirty_dot = styles(size(8, 8), radius(4), bg(var::warning), opacity(0.0f),
                                  when(state::group(state::flag<0>), opacity(1.0f)), motion);

// Entries.
constexpr auto toast = styles(
    pad(var::space(1), var::space(1.5f)), gap(var::space(1)), radius(var::radius_md), align(Align::center),
    bg(var::surface_raised), border(var::line, var::border),
    starting(opacity(0.0f), translate(0, 10)),
    hovered(border_color(var::accent), translate(4, 0)),
    springy);

// Status pills.
constexpr auto pill = styles(pad(var::space(0.625f), var::space(1.25f)), radius(var::radius_full),
                             bg(var::surface_raised), fg(var::text_disabled), font(var::font_xs), semibold, motion);
constexpr auto pill_on = styles(bg(var::accent), fg(var::on_accent));

// An app-defined token, bound by vars() where it is used.
constexpr auto brand = var::named("brand");

}  // namespace book

// ════════════════════════════════════════════════════════════════════════
// The app
// ════════════════════════════════════════════════════════════════════════

struct Stylebook {
    static constexpr std::array<const char*, 3> kFiles{"cascade.hpp", "state.hpp", "stylebook.cpp"};
    static constexpr std::array<const char*, 3> kMeta{"resolve pass \u00B7 560 lines",
                                                     "selectors and tokens \u00B7 330 lines",
                                                     "this screen \u00B7 500 lines"};
    static constexpr std::array<const char*, 3> kSwitches{"Wi-Fi", "Bluetooth", "Airplane mode (locked)"};
    static constexpr std::array<const char*, 2> kFaqQ{"Why springs, not durations?",
                                                      "Why no specificity?"};
    static constexpr std::array<const char*, 2> kFaqA{
        "A hover that ends half-way reverses from where it is, with its velocity, "
        "instead of restarting a curve.",
        "Rules resolve by a fixed ranking of states, so pressed beats hover and "
        "disabled beats both, whatever order you wrote them in."};

    /// Five complete looks: palette, shape, density, depth, type and motion.
    /// Every card on the page, and every widget, follows the selected one.
    static constexpr std::array<const Theme*, 5> kLooks{&themes::midnight, &themes::daylight,
                                                       &themes::square, &themes::soft, &themes::compact};
    static constexpr std::array<const char*, 5> kLookNames{"midnight", "daylight", "square", "soft", "compact"};

    struct Model {
        int  look = 0;           ///< index into kLooks: the WHOLE look, not just colours
        bool dark = true;        ///< kept in step with the look's mode
        float volume = 0.6f;
        bool  wifi = true;
        int  clicks = 0;
        int  file = 0;
        std::array<bool, 3> sw{true, false, false};
        std::array<bool, 2> faq{false, false};
        bool dirty = false;
        int  next_toast = 0;
        std::vector<int> toasts{};
        ScrollState page{};   // the page scrolls: nine cards are taller than a window
    };

    struct ThemeToggle {}; struct PickLook { int i; }; struct SetVolume { float f; }; struct Wifi {};
    struct Click {}; struct SelectFile { int i; }; struct Flip { int i; };
    struct Faq { int i; }; struct Edit {}; struct Save {}; struct AddToast {}; struct ClearToasts {};
    struct Scroll { float dy; }; struct BarPress { float y; }; struct BarDrag   { DragTravel t; };
    struct Quit {};
    using Msg = std::variant<ThemeToggle, PickLook, SetVolume, Wifi, Click, SelectFile, Flip, Faq, Edit, Save, AddToast,
                             ClearToasts, Scroll, BarPress, BarDrag, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model&) { return SetTitle{"mayag \u2014 stylebook"}; }
    static Cmd update(Model& m, ThemeToggle) {
        m.look = (m.look + 1) % static_cast<int>(kLooks.size());
        m.dark = kLooks[static_cast<std::size_t>(m.look)]->mode == Mode::dark;
        return {};
    }
    static Cmd update(Model& m, PickLook p) {
        m.look = p.i;
        m.dark = kLooks[static_cast<std::size_t>(m.look)]->mode == Mode::dark;
        return {};
    }
    static Cmd update(Model& m, SetVolume v) { m.volume = std::clamp(v.f, 0.0f, 1.0f); return {}; }
    static Cmd update(Model& m, Wifi)        { m.wifi = !m.wifi; return {}; }
    static Cmd update(Model& m, Click)       { ++m.clicks; return {}; }
    static Cmd update(Model& m, SelectFile s){ m.file = s.i; return {}; }
    static Cmd update(Model& m, Flip f)      { m.sw[static_cast<std::size_t>(f.i)] ^= true; return {}; }
    static Cmd update(Model& m, Faq f)       { m.faq[static_cast<std::size_t>(f.i)] ^= true; return {}; }
    static Cmd update(Model& m, Edit)        { m.dirty = true; return {}; }
    static Cmd update(Model& m, Save)        { m.dirty = false; return {}; }
    static Cmd update(Model& m, AddToast) {
        m.toasts.insert(m.toasts.begin(), m.next_toast++);
        if (m.toasts.size() > 3) m.toasts.pop_back();
        return {};
    }
    static Cmd update(Model& m, ClearToasts) { m.toasts.clear(); return {}; }
    static Cmd update(Model& m, Scroll e)    { m.page.scroll_by({0.0f, e.dy}); return {}; }
    static Cmd update(Model& m, BarPress e)  { scrollbar_press(m.page, e.y); return {}; }
    static Cmd update(Model& m, BarDrag e)   { scrollbar_drag(m.page, e.t); return {}; }
    static Cmd update(Model&, Quit)          { return Cmd::quit(); }

    /// The theme is application state. Every var:: token follows it.
    static Theme theme(const Model& m) { return *kLooks[static_cast<std::size_t>(m.look)]; }

    // ── cards ───────────────────────────────────────────────────────────

    static Node card(std::string title, std::string caption, Node demo) {
        return v(text_owned(std::move(title)) | book::h2,
                 text_owned(std::move(caption)) | book::caption,
                 std::move(demo))
             | book::section;
    }

    static Node inheritance() {
        return card("1 \u00B7 Inheritance",
                    "fg, font, tracking and italic set on a container style every text inside it. "
                    "Explicit beats inherited.",
                    v(text<"Size, colour and tracking come from the box">,
                      text<"This line sets nothing of its own">,
                      text<"This one overrides the colour"> | fg(var::accent),
                      v(text<"\u2026and nested containers pass it on">) | italic)
                        | gap(6) | pad(12) | radius(10) | bg(var::background)
                        | fg(var::text_secondary) | font(13) | tracking(0.3f));
    }

    static Node states(const Model& m) {
        return card("2 \u00B7 States",
                    "Hover, press (drag off to cancel), Tab for focus rings. Primary and ghost are "
                    "btn plus differences; disabled beats every other state.",
                    v(h(h(text<"Primary">) | book::primary | on_click(Click{}) | dsl::id<"primary">,
                        h(text<"Default">) | book::btn | on_click(Click{}) | dsl::id<"default">,
                        h(text<"Ghost">) | book::ghost | on_click(Click{}) | dsl::id<"ghost">) | gap(8),
                      h(h(text<"Disabled">) | book::primary | disabled() | dsl::id<"disabled">,
                        textf("clicked {}", m.clicks) | book::muted)
                        | gap(12) | align(Align::center))
                        | gap(12));
    }

    static Node tokens() {
        const auto island = [](const char* label, std::size_t i) {
            return v(text_owned(label) | book::muted,
                     h(text<"Action">) | book::primary | on_click(Click{}) | id_of("island", i))
                 | gap(6) | align(Align::start);
        };
        return card("3 \u00B7 Tokens and islands",
                    "Colours name theme roles and resolve late. vars() rebinds a role for one "
                    "subtree: the same class, three accents.",
                    v(h(island("theme", 0),
                        island("success", 1) | vars(var::accent = var::success,
                                                    var::accent_hover = var::success,
                                                    var::accent_active = var::success),
                        island("danger", 2) | vars(var::accent = var::danger,
                                                   var::accent_hover = var::danger,
                                                   var::accent_active = var::danger))
                        | gap(14),
                      h(box() | size(20, 20) | radius(6) | bg(book::brand),
                        text<"var::named(\"brand\")"> | book::muted)
                        | gap(8) | align(Align::center) | vars(book::brand = rgb<0x00D1B2>),
                      h(box() | size(20, 20) | radius(6) | bg(var::named("unbound")),
                        text<"unbound tokens are magenta, on purpose"> | book::muted)
                        | gap(8) | align(Align::center))
                        | gap(10));
    }

    static Node groups(const Model& m) {
        return card("4 \u00B7 Groups",
                    "Each row is a group(): its dot, subtitle and chevron respond to the ROW's "
                    "hover and selection.",
                    v(each(kFiles, [&](const char* file, std::size_t i) {
                          return h(box() | book::row_dot,
                                   v(text_of(file) | semibold, text_of(kMeta[i]) | book::row_meta)
                                       | gap(2) | grow(),
                                   text<"\u203A"> | book::chevron)
                               | book::row | selected(m.file == static_cast<int>(i)) | on_click(SelectFile{static_cast<int>(i)})
                               | id_of("file", i);
                      })) | gap(2));
    }

    static Node switches(const Model& m) {
        return card("5 \u00B7 Switches",
                    "The track is checked; the knob reads it through the group and moves on a "
                    "bouncy spring.",
                    v(each(kSwitches, [&](const char* name, std::size_t i) {
                          return h(text_of(name) | grow(),
                                   h(box() | book::knob) | book::track
                                       | checked(m.sw[i]) | disabled(i == 2)
                                       | on_click([i]() -> std::optional<Msg> {
                                             if (i == 2) return std::nullopt;   // locked
                                             return Flip{static_cast<int>(i)};
                                         })
                                       | id_of("switch", i))
                               | align(Align::center);
                      })) | gap(10));
    }

    static Node disclosure(const Model& m) {
        return card("6 \u00B7 Disclosure",
                    "state::expanded styles the header; the answer animates in with starting(...).",
                    v(each(kFaqQ, [&](const char* q, std::size_t i) {
                          const bool open = m.faq[i];
                          return v(h(text_of(q) | semibold | grow(),
                                     text_of(open ? "\u2212" : "+") | book::caret)
                                       | book::disclosure | expanded(open) | on_click(Faq{static_cast<int>(i)}) | id_of("faq", i),
                                   when(open, text_of(kFaqA[i]) | book::caption | book::reveal
                                                  | pad(2, 12, 8, 12) | id_of("faq_body", i)));
                      })) | gap(4));
    }

    static Node fields(const Model& m) {
        return card("7 \u00B7 Fields",
                    "Click the search field: its container lights up because focus is WITHIN it. "
                    "Edit sets an app flag the field styles on.",
                    v(h(text<"Search files\u2026"> | fg(var::text_disabled) | grow() | dsl::id<"search">)
                        | book::input,
                      h(box() | book::dirty_dot,
                        text<"notes.md"> | grow(),
                        text_owned(m.dirty ? "unsaved" : "saved") | book::muted)
                        | book::doc_field | flag<0>(m.dirty),
                      h(h(text<"Edit">) | book::btn | on_click(Edit{}) | dsl::id<"edit">,
                        h(text<"Save">) | book::primary | disabled(!m.dirty)
                            | on_click([dirty = m.dirty]() -> std::optional<Msg> {
                                  return dirty ? std::optional<Msg>{Save{}} : std::nullopt; })
                            | dsl::id<"save">)
                        | gap(8))
                        | gap(10));
    }

    static Node entries(const Model& m) {
        return card("8 \u00B7 Motion",
                    "New entries spring in from starting(opacity(0), translate(0, 10)); hover "
                    "nudges them. Springs carry velocity across interruptions.",
                    v(h(h(text<"Add">) | book::primary | on_click(AddToast{}) | dsl::id<"add_toast">,
                        h(text<"Clear">) | book::ghost | on_click(ClearToasts{}) | dsl::id<"clear_toasts">) | gap(8),
                      v(each(m.toasts, [](int n) {
                            return h(box() | size(8, 8) | radius(4) | bg(var::accent),
                                     textf("Notification #{}", n + 1) | grow(),
                                     text<"now"> | book::muted)
                                 | book::toast | id_of("toast", static_cast<std::size_t>(n));
                        }),
                        when(m.toasts.empty(), text<"No notifications"> | book::muted)) | gap(6))
                        | gap(10));
    }

    static Node looks(const Model& m) {
        const Theme& t = theme(m);
        const auto& c = ctx();
        return card("10 \u00B7 Themes",
                    "A theme is the whole look: colour, shape, density, depth, type, motion and every "
                    "widget's style. These widgets restyle with it; the two islands below are the "
                    "SAME widgets under | theme(...).",
                    v(h(each(kLookNames, [&](const char* name, std::size_t i) {
                            return h(text_of(name)) | book::pill
                                 | when(state::selected, book::pill_on)
                                 | selected(m.look == static_cast<int>(i)) | on_click(PickLook{static_cast<int>(i)}) | id_of("look", i);
                        })) | gap(6) | wrap,
                      h(button<"Primary">(t) | dsl::id<"w_primary">,
                        button<"Secondary">(t, ButtonVariant::secondary),
                        button<"Delete">(t, ButtonVariant::danger)) | gap(8),
                      h(switch_control(c, "w_wifi", m.wifi) | on_click(Wifi{}), text<"Wi-Fi"> | grow(),
                        slider(c, "w_volume", m.volume, 120.0f) | on_slide([](Vec2 p) { return SetVolume{p.x / 120.0f}; })) | gap(10) | align(Align::center),
                      h(themed(themes::square, [](const Ctx& ic) {
                            return dsl::card(ic.theme, text<"square"> | semibold, button<"OK">(ic.theme))
                                 | dsl::id<"island_square">;
                        }) | grow(),
                        themed(themes::soft, [](const Ctx& ic) {
                            return dsl::card(ic.theme, text<"soft"> | semibold, button<"OK">(ic.theme))
                                 | dsl::id<"island_soft">;
                        }) | grow())
                        | gap(10))
                        | gap(12));
    }

    static Node environment() {
        return card("9 \u00B7 Environment",
                    "Breakpoints and the theme are states too. Resize the window; switch the theme.",
                    v(h(h(text<"compact">) | book::pill | when(state::narrower(760), book::pill_on),
                        h(text<"regular">) | book::pill
                            | when(state::wider(760) && state::narrower(1240), book::pill_on),
                        h(text<"wide">) | book::pill | when(state::wider(1240), book::pill_on))
                        | gap(6),
                      h(h(text<"dark">) | book::pill | when(state::dark, book::pill_on),
                        h(text<"light">) | book::pill | when(state::light, book::pill_on),
                        spacer(),
                        h(text<"Switch theme">) | book::btn | on_click(ThemeToggle{}) | dsl::id<"theme2">)
                        | gap(6) | align(Align::center))
                        | gap(10));
    }

    static Node view(const Model& m) {

        const auto header =
            h(v(text<"mayag stylebook"> | font(26) | bold,
                text<"Every look on this screen is a constexpr class. view() never asks whether "
                     "anything is hovered."> | book::muted)
                  | gap(4) | grow(),
              h(text<"Tab shows focus rings"> | book::muted,
                h(text<"Switch theme">) | book::btn | on_click(ThemeToggle{}) | dsl::id<"theme">)
                  | gap(12) | align(Align::center))
            | gap(16) | align(Align::center)
            | when(state::narrower(760), direction(Axis::vertical), align(Align::start));

        const Node page = v(header,
                            // Sections that don't read the model are built once and reused.
                            h(memo(1, [] { return Node(inheritance()); }), states(m),
                              memo(2, [] { return Node(tokens()); }), groups(m), switches(m),
                              disclosure(m), fields(m), entries(m),
                              memo(3, [] { return Node(environment()); }), looks(m))
                                | wrap | gap(16))
                        | pad(24) | gap(20);
        return v(scroll_view(ctx(), "page", m.page, node(page)) | grow() | min_size(0, 0))
             | width(pct(100)) | height(pct(100))
             | bg(var::background) | fg(var::text_primary) | font(14) | book::motion;
    }

    // ── input ───────────────────────────────────────────────────────────

    // Clicks, slides and switches are inline handlers in the view; what is
    // left here is input that is not about one element.
    static std::optional<Msg> on(const Model&, const Scrolled& s) {
        if (is(s, "page")) return Scroll{s.delta.y};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Pressed& p) {
        if (is(p, "page", part_bar)) return BarPress{p.local.y};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Dragged& d) {
        if (is(d, "page", part_bar)) return BarDrag{d.travel};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (k.key == Key::escape) return Quit{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }
};

// ════════════════════════════════════════════════════════════════════════
// Headless self-check: every feature, end to end, through the real host
// ════════════════════════════════════════════════════════════════════════

namespace {

/// Path from the root to the first node matching `pred` (inclusive).
std::vector<const Node*> path_to(const Node& n, const std::function<bool(const Node&)>& pred) {
    if (pred(n)) return {&n};
    for (const auto& k : n.children()) {
        auto p = path_to(k, pred);
        if (!p.empty()) { p.insert(p.begin(), &n); return p; }
    }
    return {};
}

bool near(Color<Srgb> a, Color<Srgb> b, float eps = 2e-3f) {
    return num::abs(a.c0 - b.c0) <= eps && num::abs(a.c1 - b.c1) <= eps &&
           num::abs(a.c2 - b.c2) <= eps && num::abs(a.a - b.a) <= eps;
}

}  // namespace

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, {1180, 980});
    AppConfig cfg;
    cfg.title = "mayag \u2014 stylebook";
    cfg.size  = opts.size;
    cfg.theme = Stylebook::theme(Stylebook::Model{});

    return demo::run<Stylebook>(opts, std::move(cfg), [size = opts.size](auto& rt) {
        int fails = 0;
        const auto ok = [&](bool c, const std::string& what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what.c_str());
            if (!c) ++fails;
        };
        const auto tree = [&]() -> const Node& { return rt.host().tree(); };
        const auto by_id = [&](std::uint64_t nid) { return tree().find(nid); };
        const auto by_text = [&](std::string_view s) {
            const auto p = path_to(tree(), [&](const Node& n) {
                return n.kind() == NodeKind::text && n.text() == s;
            });
            return p;   // [root, ..., parent, text]
        };
        const auto centre = [&](std::uint64_t nid) {
            const Node* n = by_id(nid);
            return n ? n->frame().center() : Vec2{};
        };
        // Wheel the page until node `id` is fully on screen, the way a user
        // would reach a card below the fold.
        const auto reveal = [&](std::uint64_t nid) {
            for (int i = 0; i < 80; ++i) {
                const Node* n = by_id(nid);
                if (n == nullptr) return;
                const Rect f = n->frame();
                const float view_h = tree().frame().size.y;
                if (f.top() >= 0.0f && f.bottom() <= view_h) return;
                // Wheel convention: a negative delta reveals content below.
                const float dy = f.top() < 0.0f ? 60.0f : -60.0f;
                rt.window().push(ScrollEvent{{0.0f, dy}, {200.0f, 300.0f}, {}, false});
                rt.tick();
            }
        };
        // Run the frame clock until every transition has landed.
        const auto settle = [&] {
            rt.window().drive_frames(true);
            for (int i = 0; i < 400 && (i < 3 || rt.host().transitioning()); ++i) rt.tick();
            rt.window().drive_frames(false);
        };
        const Theme& th = themes::midnight;
        rt.tick();

        std::printf("1 inheritance\n");
        {
            const auto plain = by_text("This line sets nothing of its own");
            const auto over  = by_text("This one overrides the colour");
            const auto deep  = by_text("\u2026and nested containers pass it on");
            ok(!plain.empty() && plain.back()->style().text.size == 13.0f &&
               near(plain.back()->style().text.color, th.text_secondary),
               "a bare text takes size and colour from its container");
            ok(!over.empty() && near(over.back()->style().text.color, th.accent),
               "an explicit fg beats the inherited one");
            ok(!deep.empty() && deep.back()->style().text.italic &&
               deep.back()->style().text.size == 13.0f,
               "nested containers add to what they pass on");
        }

        std::printf("2 states\n");
        {
            rt.window().move_to(centre(node_id("primary")));
            rt.tick();
            settle();
            ok(near(by_id(node_id("primary"))->style().fill.color, th.accent_hover),
               "hovered: primary's own hover rule");
            rt.window().push(MouseDown{centre(node_id("primary")), MouseButton::left, {}, 1});
            rt.tick();
            settle();
            const Node* p = by_id(node_id("primary"));
            ok(near(p->style().fill.color, th.accent_active) && p->style().transform.a < 1.0f,
               "pressed beats hovered; primary's pressed bg layers on btn's scale");
            rt.window().push(MouseUp{centre(node_id("primary")), MouseButton::left, {}});
            rt.tick();
            ok(rt.model().clicks == 1, "release completes the click");

            rt.window().click(centre(node_id("default")));
            rt.tick();
            settle();
            ok(by_id(node_id("default"))->style().stroke.width == 1.0f,
               "no focus ring after a pointer click");
            rt.window().press_key(Key::tab);
            rt.tick();
            settle();
            const Node* g = by_id(node_id("ghost"));
            ok(rt.input().focused() == node_id("ghost") && g->style().stroke.width == 2.0f &&
               near(g->style().stroke.color, th.focus_ring),
               "Tab moves focus and focus_visible draws the ring");
            ok(num::abs(by_id(node_id("disabled"))->style().opacity - 0.4f) < 1e-3f,
               "disabled dims, whatever else matches");
        }

        std::printf("3 tokens\n");
        {
            ok(near(by_id(node_id("island", 0))->style().fill.color, th.accent), "var::accent from the theme");
            ok(near(by_id(node_id("island", 1))->style().fill.color, th.success), "vars() island: success");
            ok(near(by_id(node_id("island", 2))->style().fill.color, th.danger), "vars() island: danger");
            const auto brand = by_text("var::named(\"brand\")");
            ok(brand.size() >= 2 && near(brand[brand.size() - 2]->children()[0].style().fill.color,
                                         rgb<0x00D1B2>), "a named token binds");
            const auto loose = by_text("unbound tokens are magenta, on purpose");
            ok(loose.size() >= 2 && near(loose[loose.size() - 2]->children()[0].style().fill.color,
                                         rgb<0xFF00FF>), "an unbound one is loud");
        }

        std::printf("4 groups\n");
        {
            reveal(node_id("file", 2));
            rt.window().move_to(centre(node_id("file", 1)));
            rt.tick();
            settle();
            const Node* r = by_id(node_id("file", 1));
            const Style& chev = r->children()[2].style();
            ok(near(chev.text.color, th.accent) && num::abs(chev.transform.tx - 3.0f) < 1e-3f,
               "hovering the row restyles and nudges its chevron");
            rt.window().click(centre(node_id("file", 1)));
            rt.tick();
            settle();
            ok(rt.model().file == 1 &&
               near(by_id(node_id("file", 1))->children()[0].style().fill.color, th.accent),
               "selection reaches the row's dot through the group");
        }

        std::printf("5 switches\n");
        {
            reveal(node_id("switch", 2));
            rt.window().click(centre(node_id("switch", 1)));
            rt.tick();
            float peak = 0.0f;
            rt.window().drive_frames(true);
            for (int i = 0; i < 400 && (i < 3 || rt.host().transitioning()); ++i) {
                rt.tick();
                peak = std::max(peak, by_id(node_id("switch", 1))->children()[0].style().transform.tx);
            }
            rt.window().drive_frames(false);
            const float tx = by_id(node_id("switch", 1))->children()[0].style().transform.tx;
            ok(rt.model().sw[1] && num::abs(tx - 16.0f) < 1e-3f, "checked moves the knob via the group");
            ok(peak > 16.5f, "on a bouncy spring: it overshoots (peak " + std::to_string(peak) + ")");
            rt.window().click(centre(node_id("switch", 2)));
            rt.tick();
            ok(!rt.model().sw[2], "the locked switch looks disabled and stays off");
        }

        std::printf("6 disclosure\n");
        {
            reveal(node_id("faq", 1));
            rt.window().click(centre(node_id("faq", 0)));
            rt.tick();
            const Node* body = by_id(node_id("faq_body", 0));
            ok(rt.model().faq[0] && body != nullptr && body->style().opacity < 0.01f,
               "the answer's first frame is its starting(...) look");
            settle();
            body = by_id(node_id("faq_body", 0));
            ok(body && body->style().opacity == 1.0f && body->style().transform.ty == 0.0f,
               "then it springs to rest");
            ok(near(by_id(node_id("faq", 0))->children()[1].style().text.color, th.accent),
               "state::expanded reaches the caret through the group");
        }

        std::printf("7 fields\n");
        {
            reveal(node_id("save"));
            rt.window().click(centre(node_id("search")));
            rt.tick();
            settle();
            const auto path = path_to(tree(), [](const Node& n) { return n.style().id == node_id("search"); });
            const Node* box = path.size() >= 2 ? path[path.size() - 2] : nullptr;
            ok(box && near(box->style().stroke.color, th.accent),
               "focus inside the field lights its container (focus_within)");
            rt.window().click(centre(node_id("edit")));
            rt.tick();
            settle();
            const auto doc = by_text("notes.md");
            const Node* f = doc.size() >= 2 ? doc[doc.size() - 2] : nullptr;
            ok(rt.model().dirty && f && near(f->style().stroke.color, th.warning) &&
               f->children()[0].style().opacity == 1.0f,
               "flag<0> restyles the field and shows its dot");
        }

        std::printf("8 motion\n");
        {
            reveal(node_id("add_toast"));
            rt.window().click(centre(node_id("add_toast")));
            rt.tick();
            const Node* t = by_id(node_id("toast", 0));
            ok(t && t->style().opacity < 0.01f, "a new entry starts invisible");
            settle();
            t = by_id(node_id("toast", 0));
            ok(t && t->style().opacity == 1.0f && t->style().transform.ty == 0.0f, "and springs in");
            ok(!rt.host().transitioning(), "then everything rests: no frames, 0% CPU");
        }

        std::printf("9 environment\n");
        {
            const auto header_axis = [&] {
                const auto p = by_text("mayag stylebook");
                return p.size() >= 3 ? p[p.size() - 3]->style().layout.axis : Axis::horizontal;
            };
            const auto pill_fill = [&](std::string_view s) {
                const auto p = by_text(s);
                return p.size() >= 2 ? p[p.size() - 2]->style().fill.color : Color<Srgb>{};
            };
            ok(near(pill_fill("regular"), th.accent), "the regular breakpoint at 1180 px");
            rt.window().resize({620, size.y});
            rt.tick();
            settle();
            ok(header_axis() == Axis::vertical, "narrow: the header stacks (layout reflows)");
            ok(near(pill_fill("compact"), th.accent) && !near(pill_fill("regular"), th.accent),
               "and the compact pill lights");
            rt.window().resize(size);
            rt.tick();
            settle();
            ok(header_axis() == Axis::horizontal, "wide again: back to a row");

            reveal(node_id("theme"));
            rt.window().click(centre(node_id("theme")));
            rt.tick();
            rt.window().drive_frames(true);
            for (int i = 0; i < 6; ++i) rt.tick();       // ~0.1 s into the morph
            rt.window().drive_frames(false);
            const auto mid = tree().style().fill.color;
            ok(!near(mid, th.background) && !near(mid, themes::daylight.background),
               "the theme switch crossfades");
            settle();
            ok(near(tree().style().fill.color, themes::daylight.background) &&
               near(pill_fill("light"), themes::daylight.accent),
               "and lands on daylight: every token and state::light follow the model");
        }

        std::printf("10 themes\n");
        {
            // Back to midnight, then pick each look and check the SAME widget.
            reveal(node_id("look", 4));
            rt.window().click(centre(node_id("look", 0)));
            rt.tick();
            settle();
            const auto radius_of = [&](std::string_view nm) { return by_id(node_id(nm))->style().corners.tl; };
            ok(num::abs(radius_of("w_primary") - themes::midnight.radius_small) < 1e-3f,
               "a button's radius is the theme's");
            ok(num::abs(radius_of("island_square")) < 1e-3f &&
               num::abs(radius_of("island_soft") - themes::soft.radius_medium) < 1e-3f,
               "| theme(...) islands restyle the same widgets: square and soft side by side");
            ok(near(by_id(node_id("island_soft"))->style().fill.color, themes::soft.surface),
               "an island's colours are its own theme's");
            for (std::size_t i = 0; i < Stylebook::kLooks.size(); ++i) {
                reveal(node_id("look", i));
                rt.window().click(centre(node_id("look", i)));
                rt.tick();
                settle();
                const Theme& want = *Stylebook::kLooks[i];
                const Node* b = by_id(node_id("w_primary"));
                ok(b && near(b->style().fill.color, want.accent) &&
                   num::abs(b->style().corners.tl - want.radius_small) < 1e-3f &&
                   num::abs(b->style().layout.padding.left - want.space_unit * 2) < 1e-3f,
                   std::string("look '") + Stylebook::kLookNames[i] + "': colour, radius and density follow");
            }
            ok(!rt.host().transitioning(), "every theme change settles: back to 0% CPU");
        }

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        return fails;
    });
}
