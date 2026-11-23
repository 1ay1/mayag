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
Mods primary() {
#if defined(__APPLE__)
    return Mods{.super = true};
#else
    return Mods{.ctrl = true};
#endif
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
}

}  // namespace

int main() {
    std::printf("-- text_editor\n");
    round_trip();
    indentation();
    editing_and_save();
    find_and_goto();
    multi_cursor();
    motion_and_scopes();
    std::printf("%s  %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
