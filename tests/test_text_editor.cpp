// tests/test_text_editor.cpp — the text_editor widget: a file in, edits,
// the keymap, find, go to line, and exactly the bytes back out.

#include <mayag/mayag.hpp>
#include <mayag/widget/text_editor.hpp>

#include <cstdio>
#include <cmath>
#include <string>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int checks = 0, failures = 0;
void ok(bool c, const std::string& what) {
    ++checks;
    if (!c) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
}

void key(TextEditorState& s, Key k, Mods m = {}) { if (auto op = text_editor_key(s, k, m)) (*op)(s); }
void type(TextEditorState& s, std::string_view t) { if (auto op = text_editor_text(s, t)) (*op)(s); }
Mods primary() { return Mods::cmd(); }

// Both keyboards' editing conventions, on whatever machine runs the test: the
// keymap is data on the key event, not a compile-time branch.
void both_keymaps() {
    using editor::EditorState;
    auto e = EditorState::from_text("alpha beta gamma", &editor::language_named("Plain Text"));
    e.set_cursor({0, 16});
    (void)e.handle_key(Key::left, Mods{.alt = true, .keymap = Keymap::mac});   // ⌥←: a word
    ok(e.cursor().col == 11, "the Mac: option+left moves a word");
    (void)e.handle_key(Key::left, Mods{.super = true, .keymap = Keymap::mac});   // ⌘←: the line's start
    ok(e.cursor().col == 0, "the Mac: cmd+left goes to the line's start");
    e.set_cursor({0, 16});
    (void)e.handle_key(Key::left, Mods{.ctrl = true, .keymap = Keymap::pc});     // ctrl+←: a word
    ok(e.cursor().col == 11, "the PC: ctrl+left moves a word");
    ok(!Mods{.super = true, .keymap = Keymap::pc}.primary() && Mods{.ctrl = true, .keymap = Keymap::pc}.primary() &&
       Mods{.super = true, .keymap = Keymap::mac}.primary(), "the accelerator is cmd on the Mac, ctrl on the PC");
    ok(chord_text({Key::s, Mods::cmd(false, Keymap::mac)}) == "\u2318S" && chord_text({Key::s, Mods::cmd(false, Keymap::pc)}) == "Ctrl+S",
       "a chord is written each keyboard's way");
}

void round_trip() {
    const std::string lf = "int main() {\n    return 0;\n}\n";
    auto s = text_editor_ops::open(lf, "main.cpp");
    ok(text_editor_ops::file_text(s) == lf, "an untouched file writes back byte for byte (trailing newline kept)");
    ok(s.ed.language->name == "C++", "the language comes from the name");
    const std::string crlf = "a\r\nb\r\n";
    auto w = text_editor_ops::open(crlf, "x.txt");
    ok(w.crlf && text_editor_ops::file_text(w) == crlf, "CRLF files stay CRLF");
    ok(!s.ed.dirty(), "a freshly opened file is clean");
}

void indentation() {
    auto two = text_editor_ops::open("a:\n  b:\n    c: 1\n  d: 2\n", "x.yaml");
    ok(two.ed.soft_tabs && two.ed.tab_width == 2, "a 2-space file indents by 2");
    auto tabs = text_editor_ops::open("f() {\n\tx;\n\tif (y) {\n\t\tz;\n\t}\n}\n", "x.c");
    ok(!tabs.ed.soft_tabs, "a tab-indented file indents with tabs");
    auto four = text_editor_ops::open("def f():\n    return 1\n", "x.py");
    ok(four.ed.soft_tabs && four.ed.tab_width == 4, "a 4-space file indents by 4");
}

void editing_and_save() {
    auto s = text_editor_ops::open("hello\n", "a.txt");
    key(s, Key::end);
    type(s, " world");
    ok(s.ed.line(0) == "hello world" && s.ed.dirty(), "typing edits and dirties");
    key(s, Key::s, primary());
    ok(s.out.size() == 1 && s.out[0].kind == TextEditorAction::Kind::save, "the save key asks the app to save");
    const auto point = s.ed.save_point();
    s.out.clear();
    type(s, "!");   // typed while the write was in flight
    text_editor_ops::saved(s, point);
    ok(s.ed.dirty(), "an edit made during a save stays unsaved");
    key(s, Key::z, primary());
    ok(!s.ed.dirty(), "...and undoing it is back to what is on disk");
    key(s, Key::c, primary());
    ok(!s.out.empty() && s.out.back().kind == TextEditorAction::Kind::copy, "copy hands the app text for the clipboard");
    s.out.clear();
    key(s, Key::v, primary());
    ok(!s.out.empty() && s.out.back().kind == TextEditorAction::Kind::paste, "paste asks the app for the clipboard");
    text_editor_ops::paste(s, "X");
    ok(s.ed.line(0).find('X') != std::string::npos, "the clipboard's text lands at the cursor");
}

void find_and_goto() {
    auto s = text_editor_ops::open("one\ntwo\none two\none\n", "a.txt");
    key(s, Key::f, primary());
    ok(s.find_open && s.find_focused, "find opens with the keyboard in it");
    type(s, "one");
    ok(s.matches.size() == 3, "typing the query finds every match");
    key(s, Key::enter);
    ok(s.ed.primary_sel().start().line > 0 || s.match_i >= 0, "enter steps to a match");
    key(s, Key::escape);
    ok(!s.find_open, "escape closes find");
    key(s, Key::g, Mods{.ctrl = true});
    ok(s.goto_open, "ctrl+G asks for a line");
    type(s, "3");
    key(s, Key::enter);
    ok(!s.goto_open && s.ed.cursor().line == 2, "...and goes there");
}

void multi_cursor() {
    auto s = text_editor_ops::open("ab\nab\nab\n", "a.txt");
    text_editor_ops::press(s, {0, 0}, 1, {});
    text_editor_ops::press(s, {1, 0}, 1, Mods{.alt = true});
    text_editor_ops::press(s, {2, 0}, 1, Mods{.alt = true});
    type(s, "-");
    ok(s.ed.line(0) == "-ab" && s.ed.line(1) == "-ab" && s.ed.line(2) == "-ab", "alt-click adds cursors; typing edits at all");
    text_editor_ops::press(s, {0, 2}, 2, {});
    ok(!s.ed.primary_sel().empty() && s.ed.sels.size() == 1, "a double click selects the word");
}

void motion_and_scopes() {
    std::string src = "int f() {\n";
    for (int i = 0; i < 200; ++i) src += "    if (x) {\n        y();\n    }\n";
    src += "}\n";
    auto s = text_editor_ops::open(src, "a.cpp");
    s.ed.metrics.line_h = 20.0f;
    s.ed.scroll.viewport = {800, 400};
    s.ed.scroll.max_offset = {0, 600 * 20.0f};
    (void)text_editor_ops::step(s, 0.0);
    ok(!text_editor_ops::animating(s), "at rest: nothing moves");
    key(s, Key::down);
    ok(text_editor_ops::animating(s), "a caret move glides");
    for (int i = 0; i < 60; ++i) (void)text_editor_ops::step(s, 1.0 / 60.0);
    ok(!text_editor_ops::animating(s) && s.caret_row == 1.0f, "...and settles exactly on its line");
    key(s, Key::end, primary());   // the file's end: a jump
    ok(s.scroll_goal_y >= 0.0f || s.ed.scroll.offset.y > 0.0f, "a jump to the end slides the view");
    for (int i = 0; i < 120; ++i) (void)text_editor_ops::step(s, 1.0 / 60.0);
    ok(s.scroll_goal_y < 0.0f, "...and arrives");
    (void)text_editor_ops::step(s, 0.0, false);
    ok(!text_editor_ops::animating(s), "motion off: everything snaps");
    // sticky scroll's scopes: line 3 ("        y();") sits in f() and its if
    const auto sc = mayag::dsl::detail::scopes_at(s.ed, 2, 4);
    ok(sc.size() == 2 && sc[0] == 0 && sc[1] == 1, "a line's scopes: the function, then the if");
    ok(mayag::dsl::detail::scopes_at(s.ed, 0, 4).empty(), "the first line has none");
    // the minimap: a press centres that row
    s.ed.metrics.minimap_line_px = 2.0f;
    s.ed.metrics.minimap_top = 0;
    text_editor_ops::minimap_seek(s, 200.0f, false);   // row 100
    ok(std::abs(s.ed.scroll.offset.y - (100 * 20.0f - 200.0f)) < 1.0f, "a minimap press centres the row under it");
    {   // the minimap keeps its rows across edits: always what a fresh build draws
        std::string text;
        for (int i = 0; i < 400; ++i) text += i % 13 == 0 ? "int f" + std::to_string(i) + "() {\n" : "    x = \"s\" + 1; // c\n";
        auto e = editor::EditorState::from_text(text, &editor::language_named("C++"));
        const auto th = CodeTheme::dark(themes::midnight);
        const auto draw = [&](const editor::EditorState& ed) {
            Node n = mayag::dsl::detail::minimap_rows(ed, th, 0, 300, 84.0f, 2.0f, 7);
            DrawList dl;
            const auto walk = [&](auto&& self, const Node& x) -> void {
                if (x.kind() == NodeKind::canvas && x.canvas()) x.canvas()(dl, Rect{0, 0, 84.0f, 600.0f}, x.canvas_user());
                for (const auto& k : x.children()) self(self, k);
            };
            walk(walk, n);
            return dl;
        };
        (void)draw(e);
        std::uint32_t seed = 99;
        const auto rnd = [&](int n) { seed = seed * 1664525u + 1013904223u; return static_cast<int>((seed >> 8) % static_cast<std::uint32_t>(n)); };
        const char* bits[] = {"/*", "*/", "\n", "abc", "\"", "\n\n", "// q", "    "};
        bool same = true;
        for (int step = 0; step < 200 && same; ++step) {
            const int l = rnd(std::min(e.line_count(), 320));
            e.set_cursor({l, rnd(e.line_len(l) + 1)});
            if (rnd(4) == 0) e.backspace(); else e.paste(bits[rnd(8)]);
            const DrawList kept = draw(e);
            auto fresh = e;
            fresh.metrics.minimap_cache.reset();
            same = kept.same_as(draw(fresh));
        }
        ok(same, "the minimap kept across 200 random edits draws what a fresh one does");
    }
}

void completion_and_symbols() {
    auto s = text_editor_ops::open("int render_frame(int x) {\n    return x;\n}\nint main() {\n    \n}\n", "a.cpp");
    text_editor_ops::press(s, {4, 4}, 1, {});
    type(s, "re");
    ok(s.comp_open && !s.comp.empty(), "typing a word offers completions");
    bool has_fn = false, has_kw = false;
    for (const auto& c : s.comp) { has_fn = has_fn || c.word == "render_frame"; has_kw = has_kw || c.word == "return"; }
    ok(has_fn && has_kw, "...from the file's words and the language's keywords");
    type(s, "nd");
    ok(s.comp_open && s.comp[0].word == "render_frame", "narrowing ranks the file's function first");
    key(s, Key::tab);
    ok(s.ed.line(4) == "    render_frame" && !s.comp_open, "tab takes it, replacing the typed prefix");
    type(s, "(");
    ok(!s.comp_open, "a non-word character ends the word");
    {   // the pool is cached while you type on one line; an edit elsewhere
        // must show up the next time, and so must a word on this very line
        auto t = text_editor_ops::open("int alpha_one = 1;\n\nint main() {\n    \n}\n", "b.cpp");
        text_editor_ops::press(t, {3, 4}, 1, {});
        type(t, "al");
        ok(t.comp_open, "completion opens (the pool is built)");
        key(t, Key::escape);
        // an edit elsewhere with no completion there (a paste, a tool): the
        // pool cached for line 3 must notice it
        t.ed.set_cursor({1, 0});
        t.ed.paste("int alpine_two;");
        t.ed.set_cursor({3, 6});
        key(t, Key::backspace); key(t, Key::backspace);
        type(t, "al");
        bool two = false;
        for (const auto& c : t.comp) two = two || c.word == "alpine_two";
        ok(two, "a word typed on another line is offered (the cached pool notices the edit)");
        key(t, Key::escape);
        type(t, "x albatross_z ");
        type(t, "alb");
        bool same_line = false;
        for (const auto& c : t.comp) same_line = same_line || c.word == "albatross_z";
        ok(same_line, "...and a word earlier on the caret's own line");
    }
    // go to symbol
    key(s, Key::o, Mods{.shift = true, .super = true});
    key(s, Key::o, Mods{.shift = true, .ctrl = true});
    ok(s.sym_open, "the symbol key opens go-to-symbol");
    type(s, "main");
    key(s, Key::enter);
    ok(!s.sym_open && s.ed.cursor().line == 3, "choosing a symbol goes to its line");
    // rainbow brackets: depth at each line's start, strings ignored
    auto b = text_editor_ops::open("f(a, \"(\") {\n  g([1]);\n}\n", "a.c");
    ok(b.ed.bracket_depth_at(1) == 1 && b.ed.bracket_depth_at(2) == 1 && b.ed.bracket_depth_at(3) == 0,
       "bracket depth counts code brackets, not ones in strings");
    {   // ...the same answer as summing code_brackets line by line, on a mixed
        // file, and still after edits above the line (the depth rides the
        // syntax cache's single pass now)
        std::string src;
        for (int i = 0; i < 600; ++i)
            src += i % 7 == 0 ? "/* ( { [ */ int f" + std::to_string(i) + "() {\n"
                 : i % 7 == 1 ? "  s = \"))]\"; g(h(1), [2]);\n"
                 : i % 7 == 2 ? "  // }}}} ((\n"
                 : i % 7 == 6 ? "}\n" : "  x[i] = (y + z);\n";
        auto e = text_editor_ops::open(src, "big.c");
        const auto reference = [&](int l) {
            int d = 0;
            for (int k = 0; k < l; ++k)
                for (const auto& br : e.ed.code_brackets(k)) d = std::max(0, d + (br.second ? 1 : -1));
            return d;
        };
        bool same = true;
        for (int l : {0, 1, 7, 50, 333, 599}) same = same && e.ed.bracket_depth_at(l) == reference(l);
        e.ed.set_cursor({3, 0});
        e.ed.insert_text("{ ( ");   // two opens above everything below
        for (int l : {3, 4, 300, 599}) same = same && e.ed.bracket_depth_at(l) == reference(l);
        ok(same, "bracket depth equals the per-line count everywhere, and after an edit above");
    }
    // the file changed on disk
    text_editor_ops::disk_text(b, "g();\n");
    ok(b.ed.line(0) == "g();" && !b.disk_changed, "an unedited file takes the new text");
}

}  // namespace

int main() {
    std::printf("-- text_editor\n");
    both_keymaps();
    round_trip();
    indentation();
    editing_and_save();
    find_and_goto();
    multi_cursor();
    motion_and_scopes();
    completion_and_symbols();
    std::printf("%s  %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
