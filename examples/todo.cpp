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
    };

    // ── messages ────────────────────────────────────────────────────────

    struct Typed   { std::string s; };
    struct KeyDown { KeyEvent k; };
    struct Add     {};
    struct Toggle  { int index; };
    struct Remove  { int index; };
    struct SetFilter { Filter f; };
    struct Scrolled  { Vec2 delta; };
    struct FocusInput {};
    struct Quit    {};

    using Msg = std::variant<Typed, KeyDown, Add, Toggle, Remove,
                             SetFilter, Scrolled, FocusInput, Quit>;

    static std::pair<Model, Cmd<Msg>> init() {
        Model m;
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
        return {std::move(m), Cmd<Msg>::set_title("mayag — todo")};
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

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        return std::visit([&](auto&& e) -> std::pair<Model, Cmd<Msg>> {
            using T = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<T, Typed>) {
                m.input.insert(e.s);
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, KeyDown>) {
                // The text field's own key map handles motion, selection and
                // editing. Only what it declines reaches the app.
                if (m.input.handle_key(e.k.key, e.k.mods)) {
                    return {std::move(m), Cmd<Msg>::none()};
                }
                if (e.k.key == Key::enter) {
                    return update(std::move(m), Add{});
                }
                // Arrow keys move the list cursor when the field is empty.
                const auto vis = visible(m);
                if (e.k.key == Key::down && !vis.empty()) {
                    m.selected = num::min(m.selected + 1, static_cast<int>(vis.size()) - 1);
                } else if (e.k.key == Key::up && !vis.empty()) {
                    m.selected = num::max(m.selected - 1, 0);
                } else if (e.k.key == Key::space && m.selected >= 0 &&
                           m.selected < static_cast<int>(vis.size())) {
                    return update(std::move(m), Toggle{vis[static_cast<std::size_t>(m.selected)]});
                }
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Add>) {
                if (!m.input.text.empty()) {
                    m.items.push_back(Item{m.input.text, false});
                    m.input.clear();
                    m.list.scroll_to_bottom();
                }
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Toggle>) {
                if (e.index >= 0 && e.index < static_cast<int>(m.items.size())) {
                    m.items[static_cast<std::size_t>(e.index)].done ^= true;
                }
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Remove>) {
                if (e.index >= 0 && e.index < static_cast<int>(m.items.size())) {
                    m.items.erase(m.items.begin() + e.index);
                }
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, SetFilter>) {
                m.filter = e.f;
                m.selected = -1;
                m.list.scroll_to_top();
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Scrolled>) {
                m.list.scroll_by(e.delta);
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, FocusInput>) {
                return {std::move(m), Cmd<Msg>::focus(node_id("input"))};
            }
            else {
                return {std::move(m), Cmd<Msg>::quit()};
            }
        }, msg);
    }

    // ── view: pure ──────────────────────────────────────────────────────

    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = c.theme;
        const auto vis = visible(m);

        int done_count = 0;
        for (const auto& it : m.items) done_count += it.done;

        // ---- one row ----
        auto row = [&](int slot, int index) {
            const Item& it = m.items[static_cast<std::size_t>(index)];
            const auto nid = node_id("row-" + std::to_string(index));
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
                    | id_of("row-" + std::to_string(index))).build();
        };

        std::vector<Node> rows;
        rows.reserve(vis.size());
        for (int slot = 0; slot < static_cast<int>(vis.size()); ++slot) {
            rows.push_back(row(slot, vis[static_cast<std::size_t>(slot)]));
        }

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
                    | id_of(std::string{"filter-"} + label)).build();
        };

        return v(// header
                 split(v(text<"Todo"> | font(26) | bold | fg(t.text_primary),
                         text_owned(std::to_string(m.items.size() - static_cast<std::size_t>(done_count)) +
                                    " of " + std::to_string(m.items.size()) + " remaining")
                           | font(11) | fg(t.text_secondary)) | gap(2),
                       h(node(chip("all", Filter::all)),
                         node(chip("active", Filter::active)),
                         node(chip("done", Filter::done))) | gap(6)),

                 // input
                 node(text_field(t, m.input, c.focused(node_id("input")),
                                 node_id("input"), "What needs doing?").build()),

                 // the scrolling list
                 node((list(Axis::vertical, std::move(rows), 4.0f)
                       | scroll(m.list)
                       | grow()
                       | width(pct(100))
                       | dsl::id<"list">).build()),

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

    // ── subscribe: pure ─────────────────────────────────────────────────

    static Sub<Msg> subscribe(const Model& m) {
        std::vector<Sub<Msg>> subs{
            Sub<Msg>::on_text([](std::string_view s) { return Msg{Typed{std::string{s}}}; }),
            Sub<Msg>::on_any_key([](const KeyEvent& k) { return Msg{KeyDown{k}}; }),
            Sub<Msg>::on_scroll<"list">([](Vec2 d) { return Msg{Scrolled{d}}; }),
            Sub<Msg>::on_click<"input">(FocusInput{}),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_close(Quit{}),
        };

        // A click toggles; a DOUBLE click deletes. The count model makes that
        // two lines instead of a state machine.
        for (int i = 0; i < static_cast<int>(m.items.size()); ++i) {
            const auto nid = node_id("row-" + std::to_string(i));
            subs.push_back(Sub<Msg>::on_click_id(nid, Toggle{i}));
            subs.push_back(Sub<Msg>::on_click_id_gesture(
                nid, Sub<Msg>::Gesture::click, Msg{Remove{i}}, 2, false));
        }

        subs.push_back(Sub<Msg>::on_click_id(node_id("filter-all"),    SetFilter{Filter::all}));
        subs.push_back(Sub<Msg>::on_click_id(node_id("filter-active"), SetFilter{Filter::active}));
        subs.push_back(Sub<Msg>::on_click_id(node_id("filter-done"),   SetFilter{Filter::done}));

        return Sub<Msg>::batch(std::move(subs));
    }
};

static_assert(Program<Todo>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{460, 620});
    auto fonts = demo::make_fonts(opts);
    demo::print_fonts(fonts.get());

    AppConfig cfg{
        .title = "mayag — todo",
        .size  = opts.size,
        .theme = themes::midnight,
        .fonts = fonts.get(),
        .debug_bounds = opts.debug_bounds,
    };

    return demo::run<Todo>(opts, cfg, [](auto& rt) {
        std::printf("todo headless\n");
        int fails = 0;
        const auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            if (!c) ++fails;
        };

        const auto initial = rt.model().items.size();

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

        Mods primary{};
#if defined(__APPLE__)
        primary.super = true;
#else
        primary.ctrl = true;
#endif
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
        rt.click("row-0");
        ok(rt.model().items[0].done != was, "clicking a row toggles it");

        // Scrolling.
        const float before = rt.model().list.offset.y;
        if (auto r = rt.node_rect("list")) {
            rt.window().scroll(r->center(), {0, -120});
            rt.tick();
        }
        ok(rt.model().list.offset.y > before, "the wheel scrolls the list");
        ok(rt.model().list.max_offset.y > 0.0f, "and the list knows it overflows");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
