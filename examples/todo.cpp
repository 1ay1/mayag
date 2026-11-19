// examples/todo.cpp — a real application
//
// The point of this example is not the todo list. It is that a working app
// with a text field, a scrolling list, filtering, selection, and keyboard
// shortcuts is ~200 lines of pure functions, and every piece of state is
// ordinary data you could serialise, restore, or assert on in a test.
//
//   ./mayag_todo              open a window
//   ./mayag_todo --png o.png  render one frame
//   ./mayag_todo --headless   scripted, asserted, for CI

#include "harness.hpp"
#include <mayag/widget/layout.hpp>
#include <mayag/widget/text_field.hpp>
#include <mayag/widget/virtual_list.hpp>

using namespace mayag;
using namespace mayag::dsl;

struct Todo {
    // ── state ───────────────────────────────────────────────────────────

    struct Item {
        std::string text;
        bool done = false;
    };

    enum class Filter { all, active, done };

    struct Model {
        std::vector<Item> items;
        TextEditState     input;
        ScrollState       list;
        Filter            filter = Filter::all;
        int               selected = -1;   ///< keyboard cursor into the view

        /// The filter underline FOLLOWS the selected chip's real rect.
        ///
        /// Not a number that approximates where the chip is. This used to be
        /// `AnimatedFloat * 52.0f`, guessing the chips were 52px apart and
        /// 46px wide — they are at x=293/333/385 with widths 34/51/47, so the
        /// underline drifted further wrong with every tab.
        Tracked           indicator{};
        /// Which row's context menu is open; 0 = none.
        std::uint64_t     menu_for = 0;
        int               menu_index = -1;
    };

    // ── messages ────────────────────────────────────────────────────────

    struct Typed   { std::string s; };
    struct Composing { std::string s; std::uint32_t caret, sel0, sel1; };
    struct ComposeEnd {};
    struct KeyDown { KeyEvent k; };
    struct Add     {};
    struct Toggle  { int index; };
    struct Remove  { int index; };
    struct SetFilter { Filter f; };
    struct Scrolled  { float dx, dy; };   // plain floats: Vec2 isn't Sendable
    struct FocusInput {};
    struct Tick      { double dt; };
    struct OpenMenu  { int index; };
    struct CloseMenu {};
    struct Quit    {};

    using Msg = std::variant<Typed, Composing, ComposeEnd, KeyDown, Add, Toggle, Remove, SetFilter,
                             Scrolled, FocusInput, Tick, OpenMenu, CloseMenu, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title, focus>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model& m) {
        m.items = {
            {"Ship the tiled rasteriser", true},
            {"Fix Retina contentsScale", true},
            {"Make bad layout impossible", true},
            {"Replace the double-click hack", true},
            {"Add scrolling and text input", false},
            {"Write the Metal backend", false},
            {"Virtualised lists for 100k rows", false},
            {"Animation: springs and tweens", false},
            {"Command palette", false},
            {"Undo / redo", false},
            {"Accessibility tree", false},
            {"WebGPU backend for the browser", false},
        };
        return SetTitle{"mayag — todo"};
    }

    /// Indices of the items the current filter shows.
    static std::vector<int> visible(const Model& m) {
        std::vector<int> out;
        for (int i = 0; i < static_cast<int>(m.items.size()); ++i) {
            const bool done = m.items[static_cast<std::size_t>(i)].done;
            if (m.filter == Filter::active && done) continue;
            if (m.filter == Filter::done && !done) continue;
            out.push_back(i);
        }
        return out;
    }

    // ── update: pure ────────────────────────────────────────────────────

    static Cmd update(Model& m, const Typed& e) {
        m.input.cancel_preedit();   // a commit replaces the composition
        m.input.insert(e.s);
        return {};
    }
    // Input-method composition (fcitx5/ibus on Linux): shown in the field,
    // underlined, until the IME commits it as a Typed.
    static Cmd update(Model& m, const Composing& e) {
        m.input.set_preedit(e.s, e.caret, e.sel0, e.sel1);
        return {};
    }
    static Cmd update(Model& m, ComposeEnd) { m.input.cancel_preedit(); return {}; }
    static Cmd update(Model& m, const KeyDown& e) {
                // The text field's own key map handles motion, selection and
                // editing. Only what it declines reaches the app.
                if (m.input.handle_key(e.k.key, e.k.mods)) return {};
                if (e.k.key == Key::enter) return update(m, Add{});
                // Arrow keys move the list cursor when the field is empty.
                const auto vis = visible(m);
                if (e.k.key == Key::down && !vis.empty()) {
                    m.selected = num::min(m.selected + 1, static_cast<int>(vis.size()) - 1);
                } else if (e.k.key == Key::up && !vis.empty()) {
                    m.selected = num::max(m.selected - 1, 0);
                } else if (e.k.key == Key::space && m.selected >= 0 &&
                           m.selected < static_cast<int>(vis.size())) {
                    return update(m, Toggle{vis[static_cast<std::size_t>(m.selected)]});
                }
                return {};
    }
    static Cmd update(Model& m, Add) {
                if (!m.input.text.empty()) {
                    m.items.push_back(Item{m.input.text, false});
                    m.input.clear();
                    m.list.scroll_to_bottom();
                }
                return {};
    }
    static Cmd update(Model& m, Toggle e) {
                if (e.index >= 0 && e.index < static_cast<int>(m.items.size())) {
                    m.items[static_cast<std::size_t>(e.index)].done ^= true;
                }
                return {};
    }
    static Cmd update(Model& m, Remove e) {
                if (e.index >= 0 && e.index < static_cast<int>(m.items.size())) {
                    m.items.erase(m.items.begin() + e.index);
                }
                m.menu_for = 0;
                m.menu_index = -1;
                return {};
    }
    static Cmd update(Model& m, SetFilter e) {
        m.filter = e.f;
        m.selected = -1;
        m.list.scroll_to_top();
        return {};
    }
    static Cmd update(Model& m, const Scrolled& e) { m.list.scroll_by(Vec2{e.dx, e.dy}); return {}; }
    static Cmd update(Model&, FocusInput) { return FocusNode{node_id("input")}; }
    // The host steps tracked motion itself; Tick only keeps frames coming.
    static Cmd update(Model&, Tick) { return {}; }
    static Cmd update(Model& m, OpenMenu e) {
        m.menu_for = node_id("row", static_cast<std::size_t>(e.index));
        m.menu_index = e.index;
        return {};
    }
    static Cmd update(Model& m, CloseMenu) {
        m.menu_for = 0;
        m.menu_index = -1;
        return {};
    }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    // ── view: pure ──────────────────────────────────────────────────────

    static Node view(const Model& m) {
        const Ctx& c = ctx();
        const Theme& t = c.theme;
        const auto vis = visible(m);

        int done_count = 0;
        for (const auto& it : m.items) done_count += it.done;

        // ---- one row ----
        auto row = [&](int slot, int index) {
            const Item& it = m.items[static_cast<std::size_t>(index)];
            const auto nid = node_id("row", static_cast<std::size_t>(index));
            const bool cursor = (slot == m.selected);
            const bool hot    = c.hovered(nid);

            return (h(box() | size(16, 16)
                            | bg(it.done ? t.success : t.surface_raised)
                            | border(1, it.done ? t.success : t.border_strong)
                            | radius(4),
                      text_owned(it.text)
                        | font(13)
                        | fg(it.done ? t.text_disabled : t.text_primary)
                        | when(it.done, strikethrough)
                        | ellipsis
                        | grow())
                    | gap(10) | align(Align::center)
                    | pad(9, 12)
                    | width(pct(100))
                    | bg(cursor ? t.accent.fade(0.16f)
                       : hot    ? t.surface_raised
                                : t.surface.fade(0.0f))
                    | border(1, cursor ? t.accent : t.border.fade(0.0f))
                    | radius(t.radius_small)
                    // Announced as "checkbox, Ship the rasteriser, checked"
                    // rather than as an anonymous box.
                    | role(a11y::Role::checkbox)
                    | checked(it.done)
                    | label(it.text)
                    | id_of_raw(nid)).build();
        };

        // VIRTUALISED: only the rows on screen are built. The list would
        // behave identically with a million items, because the node count
        // depends on the viewport, not on the data.
        //
        // The row height is MEASURED from a prototype rather than hardcoded.
        // A guessed height makes the spacers disagree with the real rows, so
        // the scrollbar lies and the content drifts — the same class of bug
        // as a hand-computed widget height.
        auto rows_node = virtual_list_measured(
            m.list, static_cast<int>(vis.size()),
            [&](int slot) { return row(slot, vis[static_cast<std::size_t>(slot)]); },
            c.measurer(), 4.0f);

        // ---- filter chips ----
        auto chip = [&](const char* label, Filter f) {
            const auto nid = node_id(std::string{"filter-"} + label);
            const bool on = (m.filter == f);
            return (h(text_owned(label) | font(11)
                        | fg(on ? t.on_accent : t.text_secondary))
                    | center | pad(5, 12)
                    | bg(on ? t.accent : c.hovered(nid) ? t.surface_raised
                                                        : t.surface.fade(0.0f))
                    | border(1, on ? t.accent : t.border)
                    | pill
                    | role(a11y::Role::tab)
                    | selected(on)
                    | id_of(std::string{"filter-"} + label)).build();
        };

        // Follow the selected chip. `track` records the node id now and the
        // runtime observes its real rect after layout, so the spring always
        // chases geometry rather than an assumption.
        const char* selected_chip =
            m.filter == Filter::all ? "filter-all"
          : m.filter == Filter::active ? "filter-active" : "filter-done";
        c.track(m.indicator, node_id(selected_chip));

        // A context menu, posted as an OVERLAY: it escapes the list's scroll
        // clip, paints above everything, and swallows the click that
        // dismisses it. None of that is possible with an in-flow child.
        if (m.menu_for != 0 && m.menu_index >= 0) {
            const auto entry = [&](const char* label, Color<Srgb> tone, const char* id_name) {
                return (h(text_owned(label) | font(12) | fg(tone))
                        | pad(8, 14) | width(pct(100))
                        | bg(c.hovered(node_id(id_name)) ? t.surface_raised
                                                         : t.surface.fade(0.0f))
                        | radius(4)
                        | id_of(id_name)).build();
            };
            c.overlay(Overlay{
                .content = (v(node(entry("Toggle", t.text_primary, "menu-toggle")),
                              node(entry("Delete", t.danger, "menu-delete")))
                            | gap(2) | pad(4) | width(140)
                            | bg(t.overlay) | border(1, t.border_strong)
                            | radius(t.radius_medium) | elevation(16)).build(),
                .id = node_id("row-menu"),
                .anchor_id = m.menu_for,
                .placement = Placement::below_start,
                .dismiss = Dismiss::on_click_outside,
            });
        }

        return v(// header
                 split(v(text<"Todo"> | font(26) | bold | fg(t.text_primary)
                           | role(a11y::Role::heading),
                         text_owned(std::to_string(m.items.size() - static_cast<std::size_t>(done_count)) +
                                    " of " + std::to_string(m.items.size()) + " remaining")
                           | font(11) | fg(t.text_secondary)) | gap(2),
                       v(h(node(chip("all", Filter::all)),
                           node(chip("active", Filter::active)),
                           node(chip("done", Filter::done))) | gap(6),
                         // The underline follows the SELECTED CHIP's measured
                         // rect. No stride, no width constant — it cannot
                         // drift, because it never had a number of its own.
                         // Nothing to follow until layout has measured the
                         // chip at least once, so frame zero draws no
                         // underline rather than a zero-width sliver.
                         node(m.indicator.ready()
                              ? (box()
                                 | size(m.indicator.rect().width(), 2)
                                 | bg(t.accent) | radius(1)
                                 | absolute(m.indicator.rect().left(),
                                            m.indicator.rect().bottom() + 2.0f)).build()
                              : Node{}))
                       | gap(4)),

                 // input
                 node((text_field(t, m.input, c.focused(node_id("input")),
                                  node_id("input"), "What needs doing?")
                       | role(a11y::Role::text_field)
                       | label("New todo")).build()),

                 // the scrolling list
                 rows_node | scroll(m.list) | grow() | width(pct(100)) | dsl::id<"list">,

                 // footer
                 h(text<"enter"> | font(10) | fg(t.text_disabled),
                   text<"add"> | font(10) | fg(t.text_disabled),
                   spacer(),
                   text<"click"> | font(10) | fg(t.text_disabled),
                   text<"toggle"> | font(10) | fg(t.text_disabled),
                   spacer(),
                   text<"esc"> | font(10) | fg(t.text_disabled),
                   text<"quit"> | font(10) | fg(t.text_disabled))
                 | gap(6) | width(pct(100)))
             | gap(14) | pad(22)
             | width(pct(100)) | height(pct(100))
             | bg(t.background);
    }

    // ── input: pure ─────────────────────────────────────────────────────

    static std::optional<Msg> on(const Model&, const TextEvent& e) { return Typed{e.text}; }
    static std::optional<Msg> on(const Model&, const ComposeEvent& e) {
        return Composing{e.text, e.caret, e.selection_start, e.selection_end};
    }
    static std::optional<Msg> on(const Model&, const ComposeEndEvent&) { return ComposeEnd{}; }
    static std::optional<Msg> on(const Model& m, const KeyEvent& k) {
        if (k.key == Key::escape && k.mods == Mods{})
            return m.menu_for != 0 ? Msg{CloseMenu{}} : Msg{Quit{}};
        return KeyDown{k};
    }
    static std::optional<Msg> on(const Model&, const mayag::Scrolled& s) {
        if (is(s, "list")) return Scrolled{s.delta.x, s.delta.y};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const Clicked& c) {
        if (is(c, "input") && c.count == 1) return FocusInput{};
        // Menu entries, live only while the menu is open.
        if (m.menu_index >= 0 && c.count == 1) {
            if (is(c, "menu-toggle")) return Toggle{m.menu_index};
            if (is(c, "menu-delete")) return Remove{m.menu_index};
        }
        if (is(c, "filter-all"))    return SetFilter{Filter::all};
        if (is(c, "filter-active")) return SetFilter{Filter::active};
        if (is(c, "filter-done"))   return SetFilter{Filter::done};
        // A click toggles; a DOUBLE click opens the row's context menu.
        for (int i = 0; i < static_cast<int>(m.items.size()); ++i) {
            if (c.node != node_id("row", static_cast<std::size_t>(i))) continue;
            if (c.count == 1) return Toggle{i};
            if (c.count == 2) return OpenMenu{i};
            return std::nullopt;
        }
        return std::nullopt;
    }
    // Clicking outside sends Left for the dismissed overlay.
    static std::optional<Msg> on(const Model& m, const Left& l) {
        if (m.menu_index >= 0 && is(l, "row-menu")) return CloseMenu{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    // ── subscribe: pure ─────────────────────────────────────────────────

    // The indicator only requests frames while it is actually moving, so a
    // settled UI goes back to 0% CPU with no explicit stop.
    static Sub subscribe(const Model& m) {
        return when(m.indicator.animating(),
                    Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    }
};

static_assert(mayag::Program<Todo>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{460, 620});
    auto fonts = demo::make_fonts(opts);
    demo::print_fonts(fonts.get());

    AppConfig cfg{
        .title = "mayag — todo",
        .size  = opts.size,
        .theme = themes::midnight,
        .fonts = fonts,
        .debug_bounds = opts.debug_bounds,
    };

    return demo::run<Todo>(opts, cfg, [](auto& rt) {
        std::printf("todo headless\n");
        int fails = 0;
        const auto ok = [&](bool c, std::string_view what) {
            std::printf("  %s  %.*s\n", c ? "ok  " : "FAIL",
                        static_cast<int>(what.size()), what.data());
            if (!c) ++fails;
        };

        const auto initial = rt.model().items.size();

        // ── input methods ──────────────────────────────────────────────────────────
        //
        // Focusing the field tells the platform a text field is active and
        // where its caret is (the IME's candidate window opens there); a
        // composition shows in the field and only the commit lands in it.
        rt.click("input");
        rt.tick();
        ok(rt.window().text_input_active(), "focusing the field enables the input method");
        if (const auto r = rt.node_rect("input")) {
            const Rect c = rt.window().text_input_caret();
            ok(r->contains(c.origin) && c.size.y > 4.0f, "and the caret rect is inside the field");
        }
        rt.window().compose("こん");
        rt.tick();
        ok(rt.model().input.preedit == "こん" && rt.model().input.text.empty(),
           "a composition is preedit, not text");
        rt.window().commit("今日");
        rt.tick();
        ok(rt.model().input.text == "今日" && rt.model().input.preedit.empty(),
           "the commit lands in the field");
        rt.window().press_key(Key::a, Mods::cmd());
        rt.tick();
        rt.window().press_key(Key::backspace);
        rt.tick();
        rt.click("filter-all");
        rt.tick();
        ok(!rt.window().text_input_active(), "focus elsewhere disables it");

        // Typing goes into the text field's state.
        rt.window().type("Write more tests");
        rt.tick();
        ok(rt.model().input.text == "Write more tests", "typing reaches the field");

        // Enter commits it.
        rt.window().press_key(Key::enter);
        rt.tick();
        ok(rt.model().items.size() == initial + 1, "enter adds an item");
        ok(rt.model().input.text.empty(), "and clears the field");

        // Editing keys are handled by the field, not the app.
        rt.window().type("abc");
        rt.tick();
        rt.window().press_key(Key::backspace);
        rt.tick();
        ok(rt.model().input.text == "ab", "backspace edits the field");

        const Mods primary = Mods::cmd();
        rt.window().press_key(Key::a, primary);
        rt.tick();
        ok(rt.model().input.selected() == "ab", "select-all works");

        // Filters.
        rt.click("filter-done");
        ok(rt.model().filter == Todo::Filter::done, "filter chip switches");
        const auto shown = Todo::visible(rt.model()).size();
        ok(shown > 0 && shown < rt.model().items.size(), "and actually filters");

        rt.click("filter-all");
        ok(rt.model().filter == Todo::Filter::all, "back to all");

        // A click toggles.
        const bool was = rt.model().items[0].done;
        rt.click(node_id("row", 0));
        ok(rt.model().items[0].done != was, "clicking a row toggles it");

        // Scrolling.
        const float before = rt.model().list.offset.y;
        if (auto r = rt.node_rect("list")) {
            rt.window().scroll(r->center(), {0, -120});
            rt.tick();
        }
        ok(rt.model().list.offset.y > before, "the wheel scrolls the list");
        ok(rt.model().list.max_offset.y > 0.0f, "and the list knows it overflows");

        // ── virtualisation ──────────────────────────────────────────────
        //
        // The node count must depend on the VIEWPORT, not the data.
        {
            const std::size_t nodes_now = rt.tree().count();
            for (int i = 0; i < 400; ++i) {
                rt.send(Todo::Msg{Todo::Typed{"x"}});
                rt.send(Todo::Msg{Todo::Add{}});
            }
            rt.tick();
            ok(rt.model().items.size() > 400, "the list holds 400+ items");
            const std::size_t nodes_after = rt.tree().count();
            ok(nodes_after < nodes_now * 3,
               "but the tree stayed small: " + std::to_string(nodes_now) + " -> " +
               std::to_string(nodes_after) + " nodes");
        }

        // ── animation follows real geometry ─────────────────────────────
        //
        // The underline must land EXACTLY on the selected chip, at its real
        // x and its real width. This used to be `indicator * 52.0f` against
        // chips at x=293/333/385 with widths 34/51/47, so it drifted further
        // wrong with every tab — visibly, in a screenshot.
        {
            for (const char* name : {"filter-active", "filter-done", "filter-all"}) {
                rt.click(name);

                // Let the spring settle.
                rt.window().drive_frames(true);
                for (int i = 0; i < 300 && rt.model().indicator.animating(); ++i) rt.tick();
                rt.window().drive_frames(false);
                rt.tick();

                const auto chip = rt.node_rect(name);
                const Rect ind = rt.model().indicator.rect();
                ok(chip.has_value(), std::string{"chip "} + name + " is laid out");
                if (!chip) continue;

                ok(num::abs(chip->left() - ind.left()) < 1.5f,
                   std::string{"underline x matches "} + name + " (" +
                   std::to_string(static_cast<int>(ind.left())) + " vs " +
                   std::to_string(static_cast<int>(chip->left())) + ")");
                ok(num::abs(chip->width() - ind.width()) < 1.5f,
                   std::string{"underline WIDTH matches "} + name + " (" +
                   std::to_string(static_cast<int>(ind.width())) + " vs " +
                   std::to_string(static_cast<int>(chip->width())) + ")");
            }

            ok(!rt.model().indicator.animating(), "the spring settles on its own");
        }

        // ── overlay ─────────────────────────────────────────────────────
        {
            rt.click("filter-all");
            ok(rt.model().menu_for == 0, "no menu initially");

            const Vec2 row = rt.center_of(node_id("row", 0));
            rt.window().double_click(row);
            rt.tick();
            ok(rt.model().menu_for != 0, "double-clicking a row opens its menu");

            // A click well outside must DISMISS, and must not toggle whatever
            // is underneath.
            const bool done_before = rt.model().items[0].done;
            rt.window().click({5, 5});
            rt.tick();
            ok(rt.model().menu_for == 0, "clicking outside dismisses the menu");
            ok(rt.model().items[0].done == done_before,
               "and that click is CONSUMED, not passed through");
        }

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
