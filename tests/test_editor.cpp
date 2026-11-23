// tests/test_editor.cpp — the code editor's text model and syntax layer.
//
// Pure data, no window: every behaviour a user would notice (multi-cursor
// edits, auto-pairs, undo grouping, folding, find, highlighting) asserted
// against exact text and positions.

#include <mayag/editor/buffer.hpp>
#include <mayag/widget/fuzzy.hpp>
#include <mayag/widget/diff_view.hpp>
#include <mayag/widget/diff_view.hpp>
#include <mayag/widget/fuzzy.hpp>

#include <cstdio>
#include <string>

using namespace mayag;
using namespace mayag::editor;

static int failures = 0, checks = 0;
static void check(bool c, const std::string& what) {
    ++checks;
    if (!c) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
static void section(const char* s) { std::printf("%s\n", s); }

static EditorState ed(std::string_view text, std::string_view lang = "C++") {
    auto e = EditorState::from_text(text, &language_named(lang));
    return e;
}

static void test_basic_editing() {
    section("editing");
    auto e = ed("hello");
    e.set_cursor({0, 5});
    e.insert_text(" world");
    check(e.text() == "hello world", "insert at end");
    e.newline();
    check(e.line_count() == 2 && e.cursor() == Pos{1, 0}, "enter splits the line");
    e.backspace();
    check(e.text() == "hello world" && e.cursor() == Pos{0, 11}, "backspace at col 0 joins lines");

    auto f = ed("    int x;");
    f.set_cursor({0, 10});
    f.newline();
    check(f.line(1) == "    " && f.cursor() == Pos{1, 4}, "enter keeps indentation");

    auto g = ed("");
    g.insert_text("{");
    check(g.text() == "{}" && g.cursor() == Pos{0, 1}, "{ auto-closes");
    g.newline();
    check(g.text() == "{\n    \n}" && g.cursor() == Pos{1, 4}, "enter between {} opens an indented block");
    g.insert_text("f");
    g.insert_text("(");
    check(g.line(1) == "    f()", "( auto-closes (got '" + g.line(1) + "')");
    g.insert_text(")");
    check(g.line(1) == "    f()" && g.cursor() == Pos{1, 7}, "typing ) over the auto closer moves past it");
    g.set_cursor({1, 6});
    g.backspace();
    check(g.line(1) == "    f", "backspace inside () deletes the pair");

    auto q = ed("x = ");
    q.set_cursor({0, 4});
    q.insert_text("\"");
    check(q.text() == "x = \"\"", "quote pairs after a space");
    auto q2 = ed("don");
    q2.set_cursor({0, 3});
    q2.insert_text("'");
    check(q2.text() == "don'", "apostrophe after a letter does not pair");

    auto w = ed("value");
    w.select_all();
    w.insert_text("(");
    check(w.text() == "(value)", "( wraps a selection");

    auto t = ed("x");
    t.set_cursor({0, 0});
    t.tab();
    check(t.text() == "    x", "tab inserts to the next stop");
    t.backspace();
    check(t.text() == "x", "backspace in leading spaces removes a whole indent level");

    auto h = ed("    int x;");
    h.set_cursor({0, 10});
    h.move_home(false);
    check(h.cursor() == Pos{0, 4}, "home goes to the first non-blank");
    h.move_home(false);
    check(h.cursor() == Pos{0, 0}, "home again goes to column 0");

    auto u = ed("a\xC3\xA9z");   // aéz
    u.set_cursor({0, 1});
    u.move_right(false);
    check(u.cursor() == Pos{0, 3}, "right steps over a whole UTF-8 character");
    u.backspace();
    check(u.text() == "az", "backspace deletes the whole character");
}

static void test_words_and_lines() {
    section("words and lines");
    auto e = ed("foo.bar_baz(1, 2)");
    e.set_cursor({0, 0});
    e.move_word_right(false);
    check(e.cursor() == Pos{0, 3}, "word right stops at punctuation");
    e.move_word_right(false);
    check(e.cursor() == Pos{0, 4}, "then over the punctuation");
    e.move_word_right(false);
    check(e.cursor() == Pos{0, 11}, "underscore is part of a word");
    e.delete_word(false);
    check(e.text() == "foo.(1, 2)", "alt-backspace deletes the word");

    auto l = ed("one\ntwo\nthree");
    l.set_cursor({1, 1});
    l.move_lines(-1);
    check(l.text() == "two\none\nthree" && l.cursor() == Pos{0, 1}, "alt-up moves the line, caret follows");
    l.move_lines(+1);
    l.move_lines(+1);
    check(l.text() == "one\nthree\ntwo", "alt-down twice");
    l.duplicate_lines();
    check(l.text() == "one\nthree\ntwo\ntwo", "duplicate line");
    l.delete_lines();
    check(l.text() == "one\nthree\ntwo", "delete line");
    l.set_cursor({0, 0});
    l.join_lines();
    check(l.line(0) == "one three", "join lines with one space");

    auto c = ed("int a;\n  int b;\n\nint c;");
    c.select_all();
    c.toggle_comment();
    check(c.line(0) == "// int a;" && c.line(1) == "//   int b;" && c.line(2).empty(),
          "toggle comment at the shallowest indent, skipping blanks");
    c.toggle_comment();
    check(c.text() == "int a;\n  int b;\n\nint c;", "toggle comment again restores the text exactly");
    c.indent();
    check(c.line(0) == "    int a;" && c.line(2).empty(), "indent skips blank lines");
    c.outdent();
    check(c.text() == "int a;\n  int b;\n\nint c;", "outdent reverses it");
}

static void test_multicursor() {
    section("multiple cursors");
    auto e = ed("let a = foo;\nlet b = foo;\nlet c = foo;");
    e.set_cursor({0, 9});
    e.select_next_occurrence();
    check(e.sels.size() == 1 && e.text_in(e.primary_sel().range()) == "foo", "cmd-d selects the word");
    e.select_next_occurrence();
    e.select_next_occurrence();
    check(e.sels.size() == 3, "cmd-d twice more adds the next two occurrences");
    e.insert_text("bar");
    check(e.text() == "let a = bar;\nlet b = bar;\nlet c = bar;", "typing replaces every selection");
    e.undo();
    check(e.text() == "let a = foo;\nlet b = foo;\nlet c = foo;" && e.sels.size() == 3,
          "one undo restores the text and all three selections");

    auto v = ed("aaa\nbbb\nccc");
    v.set_cursor({0, 1});
    v.add_cursor_vertical(+1);
    v.add_cursor_vertical(+1);
    check(v.sels.size() == 3, "cmd-alt-down adds carets below");
    v.insert_text("X");
    check(v.text() == "aXaa\nbXbb\ncXcc", "each caret types");
    v.backspace();
    check(v.text() == "aaa\nbbb\nccc", "each caret deletes");

    auto all = ed("x y x z x");
    all.set_cursor({0, 0});
    all.select_all_occurrences();
    check(all.sels.size() == 3, "select all occurrences");

    auto ov = ed("abcdef");
    ov.set_cursor({0, 2});
    ov.add_cursor_at({0, 2});
    check(ov.sels.size() == 1, "alt-click on an existing caret removes it instead of duplicating");
    ov.add_cursor_at({0, 4});
    ov.move_right(true);
    ov.move_right(true);
    check(ov.sels.size() == 1, "selections that grow into each other merge");

    auto pc = ed("a\nb\nc");
    pc.set_cursor({0, 1});
    pc.add_cursor_vertical(1);
    pc.add_cursor_vertical(1);
    pc.paste("1\n2\n3");
    check(pc.text() == "a1\nb2\nc3", "a 3-line paste into 3 carets distributes one line each");
}

static void test_undo() {
    section("undo");
    auto e = ed("");
    e.insert_text("h"); e.insert_text("e"); e.insert_text("y");
    e.insert_text(" ");
    e.insert_text("y"); e.insert_text("o");
    check(e.text() == "hey yo", "typed");
    e.undo();
    check(e.text() == "hey ", "undo removes the last word, not the last letter");
    e.undo();
    check(e.text() == "hey", "the space was its own step");
    e.undo();
    check(e.text().empty(), "then the first word");
    e.redo(); e.redo(); e.redo();
    check(e.text() == "hey yo", "redo replays it all");
    check(e.dirty(), "an edited buffer is dirty");
    e.mark_saved();
    check(!e.dirty(), "saved is clean");
    e.insert_text("!");
    check(e.dirty(), "edit after save is dirty");
    e.undo();
    check(!e.dirty(), "undo back to the saved state is clean again");
    e.set_cursor({0, 0});
    e.insert_text("x");
    e.set_cursor({0, 5});
    e.insert_text("z");
    e.undo();
    check(e.text() == "xhey yo", "moving the caret ends an undo group");
}

static void test_folding() {
    section("folding");
    auto e = ed("fn a() {\n    one;\n    two;\n}\nfn b() {}", "Rust");
    check(e.foldable(0) && e.fold_end(0) == 2, "a block folds by indentation");
    check(!e.foldable(4), "a one-line block does not");
    e.set_cursor({1, 2});
    e.fold_at_cursor();
    check(e.folds.contains(0) && e.cursor().line == 0, "fold at caret folds the enclosing block, caret moves out");
    check(e.visual_lines() == std::vector<int>({0, 3, 4}), "hidden lines leave the display");
    e.move_vertical(1, false);
    check(e.cursor().line == 3, "down skips the folded lines");
    e.set_cursor({2, 0});
    check(e.folds.empty(), "putting the caret inside a fold opens it");
    e.toggle_fold(0);
    e.set_cursor({4, 0});
    e.insert_text("x");
    check(e.folds.contains(0), "an edit below a fold keeps it");
    e.set_cursor({0, 0});
    e.newline();
    check(e.folds.contains(1) && e.visual_lines()[1] == 1, "an edit above shifts the fold with its lines");
}

static void test_find() {
    section("find");
    auto e = ed("Foo foo food\nfoo_bar foo");
    check(e.find_all("foo").size() == 5, "plain find is case-insensitive by default");
    check(e.find_all("foo", {.case_sensitive = true}).size() == 4, "case-sensitive");
    check(e.find_all("foo", {.whole_word = true}).size() == 3, "whole word skips food and foo_bar");
    check(e.find_all("fo+d", {.regex = true}).size() == 1, "regex");
    check(e.find_all("(", {.regex = true}).empty(), "an invalid regex matches nothing instead of throwing");
    const auto hits = e.find_all("foo", {.case_sensitive = true, .whole_word = true});
    e.replace_ranges(hits, "baz");
    check(e.text() == "Foo baz food\nfoo_bar baz", "replace all");
    e.undo();
    check(e.text() == "Foo foo food\nfoo_bar foo", "replace all is one undo step");
    auto r = ed("a1 b22 c333");
    r.replace_ranges(r.find_all("([a-z])(\\d+)", {.regex = true}), "$2$1", "([a-z])(\\d+)");
    check(r.text() == "1a 22b 333c", "regex replace with capture groups");
}

static void test_brackets() {
    section("brackets");
    auto e = ed("f(a[1], {b});");
    const auto m = e.match_bracket({0, 1});
    check(m && m->second == Pos{0, 11}, "( matches its )");
    const auto n = e.match_bracket({0, 10});
    check(n && n->second == Pos{0, 8}, "the caret just after } finds {");
}

static void test_syntax() {
    section("syntax");
    const auto kinds = [](const Language& l, std::string_view s) {
        std::vector<Span> sp;
        highlight_line(l, s, {}, sp);
        std::string out;
        for (auto& x : sp) out += std::to_string(static_cast<int>(x.kind)) + " ";
        return std::pair{sp, out};
    };
    const auto& cpp = language_named("C++");
    auto [sp, _] = kinds(cpp, "constexpr int foo(Bar b) { return b.size + 42; } // done");
    const auto kind_of = [&](std::string_view src, std::string_view word) {
        for (auto& x : sp) if (src.substr(x.start, x.end - x.start) == word) return x.kind;
        return Tok::count;
    };
    const std::string_view src = "constexpr int foo(Bar b) { return b.size + 42; } // done";
    check(kind_of(src, "constexpr") == Tok::keyword, "keyword");
    check(kind_of(src, "int") == Tok::type, "builtin type");
    check(kind_of(src, "foo") == Tok::function, "name( is a function");
    check(kind_of(src, "Bar") == Tok::type, "Capitalised is a type");
    check(kind_of(src, "size") == Tok::property, ".name is a property");
    check(kind_of(src, "42") == Tok::number, "number");
    check(kind_of(src, "// done") == Tok::comment, "line comment");

    std::vector<Span> s2;
    const LineState st = highlight_line(cpp, "int x; /* start", {}, s2);
    check(st.block_comment == 1, "an open block comment carries to the next line");
    s2.clear();
    const LineState st2 = highlight_line(cpp, "still */ int y;", st, s2);
    check(st2.block_comment == 0 && s2.front().kind == Tok::comment && s2.back().kind == Tok::punct,
          "and ends where it closes");

    const auto& py = language_named("Python");
    std::vector<Span> s3;
    const LineState ps = highlight_line(py, "x = \"\"\"doc", {}, s3);
    check(ps.string != 0, "python triple-quoted strings span lines");

    const auto& rs = language_named("Rust");
    std::vector<Span> s4;
    highlight_line(rs, "fn f<'a>(x: &'a str) { println!(\"{x}\"); }", {}, s4);
    bool lifetime = false, mac = false;
    const std::string_view r = "fn f<'a>(x: &'a str) { println!(\"{x}\"); }";
    for (auto& x : s4) {
        if (r.substr(x.start, x.end - x.start) == "'a") lifetime |= x.kind == Tok::special;
        if (r.substr(x.start, x.end - x.start) == "println!") mac |= x.kind == Tok::macro;
    }
    check(lifetime && mac, "rust lifetimes and macros");

    check(language_for("src/main.rs").name == "Rust" && language_for("a.tsx").name == "TypeScript" &&
          language_for("README").name == "Plain Text", "language from file extension");
    check(language_for("/etc/nginx/nginx.conf").name == "Config" && language_for("deploy/k8s.yaml").name == "YAML" &&
          language_for("x/CMakeLists.txt").name == "CMake" && language_for("Dockerfile.dev").name == "Dockerfile" &&
          language_for("Makefile").name == "Makefile" && language_for(".env").name == "Config" &&
          language_for("notes.txt").name == "Plain Text", "config dialects by whole name, extension, then stem");
    {
        const auto& y = language_named("YAML");
        bool key = false, url_key = false;
        for (const auto& x : kinds(y, "  - name: web  # svc").first) key = key || (x.kind == Tok::property && x.end - x.start == 4);
        for (const auto& x : kinds(y, "http://x").first) url_key = url_key || x.kind == Tok::property;
        check(key && !url_key, "yaml: `- key:` is a property, a URL is not");
    }

    // Incremental cache: an edit far down does not need the top relexed.
    auto e = ed("/*\nx\n*/\nint y;");
    check(e.syntax.state_at(*e.language, e.lines, 2).block_comment == 1, "cache carries state");
    e.set_cursor({0, 0});
    e.delete_forward(); e.delete_forward();
    check(e.syntax.state_at(*e.language, e.lines, 2).block_comment == 0, "and is invalidated by an edit above");

    const auto syms = outline(rs, std::vector<std::string>{"struct Point {", "    x: f32,", "}", "impl Point {",
                                                           "    fn len(&self) -> f32 {", "    }", "}"});
    check(syms.size() == 3 && syms[0].name == "Point" && syms[2].name == "len" && syms[2].depth == 1,
          "outline finds struct, impl and nested fn");
    const auto csyms = outline(cpp, std::vector<std::string>{"int main(int argc, char** argv) {", "    if (x) {",
                                                             "    return foo(1);", "}"});
    check(csyms.size() == 1 && csyms[0].name == "main", "C++ outline finds a function definition, not calls");
    const auto nsyms = outline(cpp, std::vector<std::string>{"namespace hand {", "using namespace mayag;", "struct Fwd;",
                                                             "struct Real {", "};", "}"});
    check(nsyms.size() == 2 && nsyms[0].name == "hand" && nsyms[1].name == "Real",
          "C++ outline skips `using namespace` and forward declarations");
    check(outline(rs, std::vector<std::string>{"struct Unit;"}).size() == 1, "a Rust unit struct is a definition");
}

static void test_fuzzy_and_diff() {
    section("fuzzy + diff");
    using namespace mayag::dsl;
    check(fuzzy_match("fsz", "font.size").ok, "subsequence");
    check(!fuzzy_match("zz", "font.size").ok, "no match");
    check(fuzzy_match("bm", "buffer_model.cpp").score > fuzzy_match("bm", "abmx.cpp").score,
          "word boundaries outrank mid-word hits");
    const auto fm = fuzzy_match("fs", "font.size");
    check(fm.positions == std::vector<std::uint16_t>({0, 5}), "matched positions prefer the boundary s");
    const auto d = line_diff({"a", "b", "c"}, {"a", "B", "c", "d"});
    const auto st = diff_stats(d);
    check(st.added == 2 && st.removed == 1, "LCS diff counts");
}

int main() {
    test_basic_editing();
    test_words_and_lines();
    test_multicursor();
    test_undo();
    test_folding();
    test_find();
    test_brackets();
    test_syntax();
    test_fuzzy_and_diff();
    std::printf("%s  %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
