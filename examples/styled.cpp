// examples/styled.cpp — a whole UI styled the way CSS styles a page
//
// Every look in this file is a CLASS: a constexpr value holding base style,
// state variants, theme tokens and a transition. The view never asks "am I
// hovered?" — it says what each element looks like in every state, and the
// cascade (style/cascade.hpp) decides which state it is in, every frame.
//
//   inheritance   `| fg(...) | font(13)` on a container styles all text in it
//   variants      hovered(...), pressed(...), focus_visible(...), when(sel, ...)
//   tokens        var::accent, var::tint(var::accent, 0.2) — late-bound to
//                 the theme, so the theme switch below rebuilds nothing
//   islands       vars(var::accent = ...) re-accents one subtree
//   groups        a row's chevron brightens when the ROW is hovered
//   responsive    when(state::narrower(720), direction(Axis::vertical))
//   motion        transition() springs between resolved styles;
//                 starting(...) is an entry animation
//
// Build:  cmake --build build && ./build/examples/mayag_styled
// (--headless self-checks without a window; --png writes a screenshot)

#include <mayag/mayag.hpp>

#include "harness.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <variant>

using namespace mayag;
using namespace mayag::dsl;

// ── the stylesheet ───────────────────────────────────────────────────────
//
// Plain constexpr values. Compose them like CSS classes; every member is
// capability-checked against the element it lands on.

namespace sheet {

constexpr auto motion = transition();   // critically damped: quick, no bounce

constexpr auto button = styles(
    pad(9, 16), radius(10), bg(var::surface_raised), border(1, var::border),
    fg(var::text_primary), semibold, font(14),
    hovered(bg(var::tint(var::accent, 0.22f)), border_color(var::accent)),
    pressed(bg(var::accent), fg(var::on_accent), scale(0.96f)),
    focus_visible(border(2, var::focus_ring)),
    when(state::disabled, opacity(0.4f)),
    motion);

constexpr auto primary = styles(
    button, bg(var::accent), border_color(var::accent), fg(var::on_accent),
    hovered(bg(var::accent_hover), border_color(var::accent_hover)),
    pressed(bg(var::accent_active)));

constexpr auto card = styles(
    pad(18), gap(10), radius(16), bg(var::surface), border(1, var::border),
    hovered(border_color(var::border_strong), elevation(8)),
    starting(opacity(0.0f), translate(0, 12)),   // fades and rises in
    motion);

constexpr auto row = styles(
    pad(10, 12), gap(10), radius(10), align(Align::center),
    hovered(bg(var::tint(var::accent, 0.10f))),
    when(state::selected, bg(var::tint(var::accent, 0.20f))),
    when(state::selected && state::hover, bg(var::tint(var::accent, 0.28f))),
    group(), motion);

constexpr auto chevron = styles(
    fg(var::text_disabled), group_hovered(fg(var::accent), translate(3, 0)), motion);

constexpr auto muted = styles(fg(var::text_secondary), font(12.5f));

constexpr auto layout = styles(
    gap(18), when(state::narrower(720), direction(Axis::vertical)));

}  // namespace sheet

// ── the app ──────────────────────────────────────────────────────────────

struct Styled {
    static constexpr std::array<const char*, 4> kItems{"Inbox", "Drafts", "Sent", "Archive"};

    struct Model {
        int  selected = 0;
        bool dark = true;
        int  saves = 0;
        bool armed = true;
    };

    struct Select { int i; }; struct Toggle {}; struct Save {}; struct Arm {}; struct Quit {};
    using Msg = std::variant<Select, Toggle, Save, Arm, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model&) { return SetTitle{"mayag — styled"}; }
    static Cmd update(Model& m, Select s) { m.selected = s.i; return {}; }
    static Cmd update(Model& m, Toggle)   { m.dark = !m.dark; return {}; }
    static Cmd update(Model& m, Save)     { if (m.armed) ++m.saves; return {}; }
    static Cmd update(Model& m, Arm)      { m.armed = !m.armed; return {}; }
    static Cmd update(Model&, Quit)       { return Cmd::quit(); }

    /// The theme is DATA in the model; the view only names roles.
    static Theme theme(const Model& m) { return m.dark ? themes::midnight : themes::daylight; }

    static Node nav(const Model& m) {
        std::vector<Node> rows;
        for (std::size_t i = 0; i < kItems.size(); ++i) {
            rows.push_back(h(text_owned(kItems[i]) | grow(),
                             text<"\u203A"> | sheet::chevron | font(16))
                           | sheet::row | selected(m.selected == static_cast<int>(i))
                           | id_of("nav", i));
        }
        return v_list(std::move(rows), 2.0f) | width(220) | rigid;
    }

    static Node detail(const Model& m) {
        return v(text<"Release notes"> | font(22) | bold,
                 text<"Everything you see is a class: base, states, tokens, motion."> | sheet::muted,
                 h(h(text<"Save">) | sheet::primary | disabled(!m.armed) | dsl::id<"save">,
                   h(text<"Theme">) | sheet::button | dsl::id<"theme">,
                   h(text_owned(m.armed ? "Lock" : "Unlock")) | sheet::button | dsl::id<"arm">,
                   spacer(),
                   text_owned("saved " + std::to_string(m.saves)) | sheet::muted)
                     | gap(8) | align(Align::center),
                 // An island: same classes, re-accented, no new styles.
                 v(text<"Danger zone"> | semibold,
                   text<"Same button class; this card rebinds var::accent."> | sheet::muted,
                   h(h(text<"Delete project">) | sheet::primary | dsl::id<"delete">))
                     | sheet::card | vars(var::accent = var::danger,
                                         var::accent_hover = var::danger,
                                         var::accent_active = var::danger) | dsl::id<"danger">)
             | sheet::card | grow() | dsl::id<"detail">;
    }

    static Node view(const Model& m) {
        return v(h(node(nav(m)), node(detail(m))) | sheet::layout | grow())
             | pad(24) | width(pct(100)) | height(pct(100))
             | bg(var::background) | fg(var::text_primary) | font(14);
    }

    static std::optional<Msg> on(const Model&, const Clicked& c) {
        for (std::size_t i = 0; i < kItems.size(); ++i)
            if (is(c, "nav", i)) return Select{static_cast<int>(i)};
        if (is(c, "theme")) return Toggle{};
        if (is(c, "save"))  return Save{};
        if (is(c, "arm"))   return Arm{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (k.key == Key::escape) return Quit{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }
};

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, {900, 560});
    AppConfig cfg;
    cfg.title = "mayag — styled";
    cfg.size  = opts.size;
    cfg.theme = Styled::theme(Styled::Model{});

    return demo::run<Styled>(opts, std::move(cfg), [](auto& rt) {
        int fails = 0;
        const auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            if (!c) ++fails;
        };
        const auto node = [&](std::uint64_t nid) { return rt.host().tree().find(nid); };
        const auto centre = [&](std::uint64_t nid) {
            const Node* n = node(nid);
            return n ? n->frame().center() : Vec2{};
        };
        const Theme& th = themes::midnight;

        rt.tick();
        const Node* save = node(node_id("save"));
        ok(save && save->style().fill.color == th.accent, "primary resolves var::accent");
        const Node* del = node(node_id("delete"));
        ok(del && del->style().fill.color == th.danger, "the island rebinds the same class to danger");

        // Hover a nav row: its chevron (a descendant) reacts via the group.
        rt.window().move_to(centre(node_id("nav", 2)));
        rt.window().drive_frames(true);
        for (int i = 0; i < 60; ++i) rt.tick();
        rt.window().drive_frames(false);
        const Node* row2 = node(node_id("nav", 2));
        ok(row2 && row2->children().size() == 2 &&
           row2->children()[1].style().text.color == th.accent,
           "group hover restyles the row's chevron");

        rt.window().click(centre(node_id("nav", 2)));
        rt.tick();
        ok(rt.model().selected == 2, "clicking a row selects it");

        rt.window().click(centre(node_id("arm")));
        rt.tick();
        rt.window().click(centre(node_id("save")));
        rt.tick();
        ok(rt.model().saves == 0, "a disabled() button looks disabled; the app decides it does nothing");

        // The theme is model state: one click, every token follows. Elements
        // with transition() CROSSFADE to the new theme rather than cutting.
        rt.window().click(centre(node_id("theme")));
        rt.tick();
        ok(!rt.model().dark, "the theme button toggles the model");
        const Node* mid = node(node_id("detail"));
        ok(mid && mid->style().fill.color != th.surface &&
           mid->style().fill.color != themes::daylight.surface,
           "a transitioned card crossfades between themes");
        rt.window().drive_frames(true);
        for (int i = 0; i < 240 && rt.host().transitioning(); ++i) rt.tick();
        rt.window().drive_frames(false);
        const Node* detail = node(node_id("detail"));
        ok(detail && detail->style().fill.color == themes::daylight.surface,
           "and lands on the new theme's var::surface, with no view change");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        return fails;
    });
}
