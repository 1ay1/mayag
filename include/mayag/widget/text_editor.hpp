#pragma once
// mayag::widget text_editor — a file, open for editing
//
//   text_editor(c, "doc", st, props, [](TextEditorOp op) { return Msg{EditorOp{op}}; })
//
// ┌ src › app › pane.cpp › update ───────────────────────── ● ┐   where you are
// │ ⌕ query  Aa ab .*   3 of 17  ↑ ↓  ×                        │   find (⌘F / ctrl+F)
// │  12 │ App::Cmd App::update(Model& m, PaneOp p) {            │
// │  13 │     using A = FilePaneAction;▏                        │   the code
// │ Ln 13, Col 30 · 2 cursors        C++ · Spaces: 4 · LF · UTF-8│   the status
// └──────────────────────────────────────────────────────────────┘
//
// Everything an editor does to TEXT is editor::EditorState (multiple
// cursors, word/line motion, auto-indent and pairs, undo that coalesces
// typing, folding, comment toggling, line moves, find with case / word /
// regex, replace). This widget is the rest: the keymap, the pointer, find
// and go-to-line, and the chrome -- so an app opens a file with
// text_editor_ops::open(text, path) and does nothing else but carry out the
// actions it asks for.
//
// Plain data in the app's model, like every mayag widget: TextEditorState.
// Input becomes TextEditorOp (an edit of that state, folded by update):
//
//   text_editor_key(st, key, mods)   KeyEvent  -> op (every key: none leak)
//   text_editor_text(st, text)       TextEvent -> op
//   the pointer                      handlers inside the widget
//
// What only the app can do arrives in `st.out` after an op:
//
//   save    write st.ed.text() (with st.crlf line endings); then
//           text_editor_ops::saved(st, point) with the point it wrote
//   copy    put `text` on the clipboard
//   paste   read the clipboard; then text_editor_ops::paste(st, text)
//
// Keys (⌘ on macOS is ctrl elsewhere -- Mods::primary()):
//   ⌘S save · ⌘F find · ⌘⌥F / ctrl+H replace · ⌘G / F3 next (⇧ previous)
//   ctrl+G go to line · ⌘C ⌘X ⌘V (ctrl+⇧+C / V too) · ⌘Z ⌘⇧Z undo, redo
//   ⌘D next occurrence · ⌘⇧L all · ⌘/ comment · ⌥↑↓ move lines · ⌘⌥↑↓ add cursor
//   alt-click: another cursor · double / triple click: word / line

#include "breadcrumbs.hpp"
#include "code_view.hpp"
#include "control.hpp"
#include "completion_menu.hpp"
#include "find_bar.hpp"
#include "fuzzy.hpp"
#include "picker.hpp"
#include "status_bar.hpp"
#include "text_field.hpp"
#include "../app/handlers.hpp"
#include "../core/text_edit.hpp"
#include "../editor/buffer.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mayag::dsl {

/// What the app must do for the editor (it can't touch the world itself).
struct TextEditorAction {
    enum class Kind : std::uint8_t { save, copy, paste, reload };   ///< reload: read the file from disk again
    Kind        kind = Kind::save;
    std::string text;   ///< copy: what to put on the clipboard
};

/// One completion: a word the file already uses, or the language's.
struct EditorCompletion {
    std::string word;
    enum class From : std::uint8_t { file, keyword, type, symbol } from = From::file;
    int uses = 0;                  ///< how often the file uses it (ranks ties)
    std::string detail;            ///< "fn", "struct", ... for a symbol; "kw" ...
};

struct TextEditorState {
    editor::EditorState ed;
    bool crlf = false;                    ///< the file had \r\n: written back the same way
    // find (and replace)
    bool find_open = false;
    bool find_replace = false;            ///< the replace row
    bool find_focused = false;            ///< the keyboard is in the find fields
    int  find_field = 0;                  ///< 0 query, 1 replacement
    TextEditState find_q, find_r;
    editor::FindOptions find_opts;
    std::vector<editor::Range> matches;
    int  match_i = -1;
    // go to line
    bool goto_open = false;
    TextEditState goto_q;
    // motion: what is DRAWN eases toward what is TRUE. The caret glides to
    // where it went; the view slides to where it scrolled (a jump to a find
    // match, a page, go-to-line) instead of cutting.
    float caret_col = -1.0f, caret_row = -1.0f;   ///< drawn caret (display col, visual row); < 0: not yet placed
    float scroll_goal_y = -1.0f;                  ///< where a jump is sliding to; < 0: none
    int   center_line = -1;                       ///< centre this line once the view is measured (a jump into a fresh file)
    // completion: the words of the file and its language, ranked against the
    // word being typed; Tab / Enter takes one, Esc puts it away
    bool comp_open = false;
    int  comp_sel = 0;
    std::vector<EditorCompletion> comp;           ///< ranked, best first
    std::vector<std::vector<std::uint16_t>> comp_hits;   ///< matched chars, per entry
    // go to symbol (⌘⇧O): the file's functions, types, ... as a picker
    bool sym_open = false;
    TextEditState sym_q;
    int  sym_sel = 0;
    // the file changed on disk while open
    bool disk_changed = false;
    std::vector<TextEditorAction> out;    ///< for the app: drain after every op
};

using TextEditorOp = Edit<TextEditorState>;

struct TextEditorProps {
    std::string path;                     ///< where it lives ("" = untitled)
    std::string shown_path;               ///< how to show it ("~/src/app/pane.cpp")
    float       font_size = 13.0f;        ///< the code's size (exact: it is a cell grid)
    float       line_height = 1.45f;
    FontFamily  family = FontFamily::mono;
    bool        ligatures = false;
    CodeTheme   theme{};
    bool        focused = true;           ///< the editor has the keyboard (draws the caret)
    bool        cursor_on = true;         ///< blink phase, the app's clock
    bool        saving = false;           ///< a write is in flight
    std::string error;                    ///< the last save failed: why
    std::string host;                     ///< the machine the file is on ("" = this one): a chip before the path
    bool        host_danger = false;      ///< ...a production machine: the chip is red
    bool        minimap = true;           ///< the file at a glance, beside the code
    bool        sticky = true;            ///< enclosing scopes pinned at the top
    bool        motion = true;            ///< the caret glides, jumps slide (off: everything snaps)
};

// ── the operations: pure functions of the state ─────────────────────────────

namespace text_editor_ops {

/// How a file indents: tabs, or spaces in steps of 2, 4 or 8 -- read from
/// the lines that indent, so a 2-space file keeps being 2-space.
inline void detect_indent(editor::EditorState& ed) {
    int tabs = 0, spaced = 0;
    int hist[9] = {};
    int prev = 0;
    for (const auto& l : ed.lines) {
        if (l.empty()) continue;
        if (l[0] == '\t') { ++tabs; continue; }
        int n = 0;
        while (n < static_cast<int>(l.size()) && l[static_cast<std::size_t>(n)] == ' ') ++n;
        if (n == static_cast<int>(l.size())) continue;   // blank
        if (n > 0) ++spaced;
        const int d = n > prev ? n - prev : 0;           // what one level adds
        if (d >= 2 && d <= 8) ++hist[d];
        prev = n;
    }
    if (tabs > spaced) { ed.soft_tabs = false; return; }
    if (spaced == 0) return;   // nothing to learn: the default stays
    ed.soft_tabs = true;
    int best = 4, votes = 0;
    for (int w : {2, 4, 8}) if (hist[w] > votes) { best = w; votes = hist[w]; }
    ed.tab_width = best;
}

/// A file's text, ready to edit: its language from the name, its line
/// endings and indentation from its content.
[[nodiscard]] inline TextEditorState open(std::string_view text, std::string_view path) {
    TextEditorState s;
    s.crlf = text.find("\r\n") != std::string_view::npos;
    s.ed = editor::EditorState::from_text(text, &editor::language_for(path));
    detect_indent(s.ed);
    return s;
}

/// The bytes to write: the buffer, in the file's own line endings.
[[nodiscard]] inline std::string file_text(const TextEditorState& s) {
    std::string t = s.ed.text();
    if (!s.crlf) return t;
    std::string out;
    out.reserve(t.size() + t.size() / 32);
    for (char ch : t) { if (ch == '\n') out += '\r'; out += ch; }
    return out;
}

inline void refind(TextEditorState& s) {
    s.matches.clear();
    s.match_i = -1;
    if (!s.find_open || s.find_q.text.empty()) return;
    s.matches = s.ed.find_all(s.find_q.text, s.find_opts);
    const editor::Pos c = s.ed.primary_sel().start();
    for (std::size_t i = 0; i < s.matches.size(); ++i)
        if (!(s.matches[i].start < c)) { s.match_i = static_cast<int>(i); break; }
    if (s.match_i < 0 && !s.matches.empty()) s.match_i = 0;
}

/// The next (dir > 0) or previous match from the caret, wrapping.
inline void goto_match(TextEditorState& s, int dir) {
    if (s.matches.empty()) return;
    const editor::Selection c = s.ed.primary_sel();
    const int n = static_cast<int>(s.matches.size());
    if (dir > 0) {
        int k = 0;
        while (k < n && !(c.end() <= s.matches[static_cast<std::size_t>(k)].start)) ++k;
        s.match_i = k % n;
    } else {
        int k = n - 1;
        while (k >= 0 && !(s.matches[static_cast<std::size_t>(k)].end <= c.start())) --k;
        s.match_i = (k + n) % n;
    }
    s.ed.select_range(s.matches[static_cast<std::size_t>(s.match_i)]);
}

/// ⌘F: the selection (one line of it) becomes the query.
inline void open_find(TextEditorState& s, bool replace) {
    s.goto_open = false;
    s.find_open = true;
    s.find_focused = true;
    s.find_field = 0;
    if (replace) s.find_replace = true;
    const editor::Selection& p = s.ed.primary_sel();
    if (!p.empty() && p.start().line == p.end().line) s.find_q.set(s.ed.text_in(p.range()));
    s.find_q.select_all();
    refind(s);
}

inline void close_find(TextEditorState& s) {
    s.find_open = false;
    s.find_focused = false;
    refind(s);
}

inline void replace_one(TextEditorState& s) {
    if (s.match_i < 0 || s.match_i >= static_cast<int>(s.matches.size())) return;
    const editor::Range r = s.matches[static_cast<std::size_t>(s.match_i)];
    s.ed.replace_ranges({r}, s.find_r.text, s.find_opts.regex ? std::string_view{s.find_q.text} : std::string_view{},
                        s.find_opts.case_sensitive);
    refind(s);
    goto_match(s, +1);
}

inline void replace_all(TextEditorState& s) {
    if (s.matches.empty()) return;
    (void)s.ed.replace_ranges(s.matches, s.find_r.text, s.find_opts.regex ? std::string_view{s.find_q.text} : std::string_view{},
                              s.find_opts.case_sensitive);
    refind(s);
}

inline void go_to_line(TextEditorState& s) {
    int n = 0;
    for (char ch : s.goto_q.text) if (ch >= '0' && ch <= '9') n = n * 10 + (ch - '0');
    s.goto_open = false;
    if (n <= 0) return;
    const int l = std::min(n, s.ed.line_count()) - 1;
    s.ed.select_range(editor::Range{{l, 0}, {l, 0}});
}

/// The clipboard's text, at every cursor.
inline void paste(TextEditorState& s, std::string_view text) {
    if (s.goto_open) { for (char ch : text) if (ch >= '0' && ch <= '9') s.goto_q.insert(std::string(1, ch)); return; }
    if (s.find_open && s.find_focused) {
        std::string one{text};
        std::erase(one, '\n'); std::erase(one, '\r');
        (s.find_field == 0 ? s.find_q : s.find_r).insert(one);
        if (s.find_field == 0) refind(s);
        return;
    }
    s.ed.paste(text);
    refind(s);
}

/// A save of the state at `point` landed.
inline void saved(TextEditorState& s, std::uint64_t point) { s.ed.mark_saved(point); }

// ── completion: the file's words and its language's ───────────────────────

/// The word being typed: [start, caret) on the caret's line, or empty.
[[nodiscard]] inline std::string word_before_caret(const TextEditorState& s, int* start = nullptr) {
    const editor::Pos c = s.ed.cursor();
    const std::string& l = s.ed.line(c.line);
    int a = c.col;
    while (a > 0 && editor::detail::is_word(static_cast<unsigned char>(l[static_cast<std::size_t>(a - 1)]))) --a;
    if (start) *start = a;
    return l.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(c.col - a));
}

/// Rank completions for the word at the caret. Opens the popup when the word
/// is 2+ characters and something other than the word itself matches.
inline void complete(TextEditorState& s, bool force = false) {
    int start = 0;
    const std::string pre = word_before_caret(s, &start);
    const editor::Pos c = s.ed.cursor();
    if ((!force && pre.size() < 2) || s.ed.sels.size() > 1 || !s.ed.primary_sel().empty()) { s.comp_open = false; return; }
    // the file's words, counted (the one being typed excluded)
    std::vector<EditorCompletion> pool;
    {
        std::vector<std::pair<std::string, int>> words;
        const int lo = std::max(0, c.line - 4000), hi = std::min(s.ed.line_count(), c.line + 4000);   // bounded on huge files
        for (int l = lo; l < hi; ++l) {
            const std::string& t = s.ed.line(l);
            for (std::size_t i = 0; i < t.size();) {
                if (!editor::detail::is_word(static_cast<unsigned char>(t[i])) || (t[i] >= '0' && t[i] <= '9')) { ++i; continue; }
                std::size_t j = i;
                while (j < t.size() && editor::detail::is_word(static_cast<unsigned char>(t[j]))) ++j;
                if (j - i >= 3 && !(l == c.line && static_cast<int>(i) == start)) {
                    std::string w = t.substr(i, j - i);
                    auto it = std::find_if(words.begin(), words.end(), [&](const auto& p) { return p.first == w; });
                    if (it == words.end()) words.emplace_back(std::move(w), 1); else ++it->second;
                }
                i = j;
                if (words.size() > 6000) break;
            }
        }
        for (auto& [w, n] : words) pool.push_back({std::move(w), EditorCompletion::From::file, n, {}});
    }
    for (const auto& sym : s.ed.symbols())
        for (auto& p : pool) if (p.word == sym.name) { p.from = EditorCompletion::From::symbol; p.detail = sym.kind; }
    const auto add_lang = [&](const std::vector<std::string_view>& v, EditorCompletion::From from, const char* d) {
        for (auto w : v)
            if (std::none_of(pool.begin(), pool.end(), [&](const auto& p) { return p.word == w; }))
                pool.push_back({std::string{w}, from, 0, d});
    };
    add_lang(s.ed.language->keywords, EditorCompletion::From::keyword, "keyword");
    add_lang(s.ed.language->types, EditorCompletion::From::type, "type");
    // rank: fuzzy score, then a prefix match, then how often the file uses it
    struct Ranked { int score; std::size_t i; std::vector<std::uint16_t> hits; };
    std::vector<Ranked> r;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (pool[i].word == pre) continue;
        auto m = fuzzy_match(pre, pool[i].word);
        if (!m.ok) continue;
        const bool prefix = pool[i].word.size() >= pre.size() &&
            std::equal(pre.begin(), pre.end(), pool[i].word.begin(), [](char a, char b) { return detail::fold(a) == detail::fold(b); });
        int score = m.score + (prefix ? 30 : 0) + std::min(pool[i].uses, 12) +
                    (pool[i].from == EditorCompletion::From::symbol ? 6 : 0);
        r.push_back({score, i, std::move(m.positions)});
    }
    std::stable_sort(r.begin(), r.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
    // A scattered match ("up" in nullptr) is noise beside real ones: keep it
    // only when nothing starts with the typed word or meets it at a word start.
    const auto strong = [&](const Ranked& x) {
        const auto& w = pool[x.i].word;
        if (x.hits.empty() || x.hits.size() != pre.size()) return false;
        if (x.hits[0] == 0) return true;   // a prefix (or one that starts there)
        const unsigned char before = static_cast<unsigned char>(w[x.hits[0] - 1]);
        const bool camel = w[x.hits[0]] >= 'A' && w[x.hits[0]] <= 'Z' && !(before >= 'A' && before <= 'Z');
        return before == '_' || camel;   // at a word inside it: snake_case or camelCase
    };
    if (std::any_of(r.begin(), r.end(), strong)) std::erase_if(r, [&](const Ranked& x) { return !strong(x); });
    if (r.size() > 40) r.resize(40);
    s.comp.clear(); s.comp_hits.clear();
    for (auto& x : r) { s.comp.push_back(pool[x.i]); s.comp_hits.push_back(std::move(x.hits)); }
    s.comp_open = !s.comp.empty();
    s.comp_sel = 0;
}

/// Take completion `i`: it replaces the word being typed.
inline void accept_completion(TextEditorState& s, int i) {
    if (!s.comp_open || i < 0 || i >= static_cast<int>(s.comp.size())) return;
    int start = 0;
    (void)word_before_caret(s, &start);
    const editor::Pos c = s.ed.cursor();
    s.ed.select_range(editor::Range{{c.line, start}, c});
    s.ed.insert_text(s.comp[static_cast<std::size_t>(i)].word);
    s.comp_open = false;
    refind(s);
}

// ── go to symbol ───────────────────────────────────────────────────────────

/// The file's symbols matching the query, ranked: (symbol index, hits).
[[nodiscard]] inline std::vector<std::pair<int, FuzzyMatch>> symbols_matching(const TextEditorState& s) {
    std::vector<std::pair<int, FuzzyMatch>> out;
    const auto& syms = s.ed.symbols();
    for (std::size_t i = 0; i < syms.size(); ++i) {
        auto m = fuzzy_match(s.sym_q.text, syms[i].name);
        if (m.ok) out.emplace_back(static_cast<int>(i), std::move(m));
    }
    if (!s.sym_q.text.empty())
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second.score > b.second.score; });
    return out;
}

inline void open_symbols(TextEditorState& s) {
    s.sym_open = true; s.sym_q.set({}); s.sym_sel = 0;
    s.find_focused = false; s.goto_open = false; s.comp_open = false;
    // start on the symbol the caret is in
    const auto& syms = s.ed.symbols();
    for (std::size_t i = 0; i < syms.size(); ++i) if (syms[i].line <= s.ed.cursor().line) s.sym_sel = static_cast<int>(i);
}

inline void go_to_symbol(TextEditorState& s, int ranked_index) {
    const auto r = symbols_matching(s);
    s.sym_open = false;
    if (ranked_index < 0 || ranked_index >= static_cast<int>(r.size())) return;
    const auto& sym = s.ed.symbols()[static_cast<std::size_t>(r[static_cast<std::size_t>(ranked_index)].first)];
    const int col = static_cast<int>(s.ed.line(sym.line).find(sym.name));
    s.ed.select_range(editor::Range{{sym.line, std::max(col, 0)}, {sym.line, std::max(col, 0) + static_cast<int>(sym.name.size())}});
}

// ── the file on disk ───────────────────────────────────────────────────────

/// The file changed on disk. Unedited: take the new text quietly (the caret
/// and scroll stay where they were). Edited: say so, and let the user choose.
inline void disk_text(TextEditorState& s, std::string_view text) {
    s.disk_changed = false;
    const editor::Pos c = s.ed.cursor();
    const auto scroll = s.ed.scroll.offset;
    const bool crlf = text.find("\r\n") != std::string_view::npos;
    s.ed.set_text(text);
    s.crlf = crlf;
    s.ed.set_cursor({std::min(c.line, s.ed.line_count() - 1), 0});
    s.ed.set_cursor({s.ed.cursor().line, std::min(c.col, s.ed.line_len(s.ed.cursor().line))});
    s.ed.scroll.scroll_to(scroll);
    s.caret_row = -1.0f;
    refind(s);
}

/// One key. Every key is the editor's: nothing it doesn't use reaches anyone.
inline void key(TextEditorState& s, Key k, Mods md) {
    const bool cmd = md.primary(), sh = md.shift, alt = md.alt;
    const bool copy_key  = (cmd && !sh && !alt && k == Key::c) || (md.ctrl && sh && !alt && k == Key::c);
    const bool paste_key = (cmd && !sh && !alt && k == Key::v) || (md.ctrl && sh && !alt && k == Key::v) || (sh && k == Key::insert);
    const bool goto_key  = md.ctrl && !md.super && !alt && !sh && k == Key::g;   // ctrl+G everywhere (⌘G is find next)

    if (s.sym_open) {   // go to symbol has the keyboard
        const int n = static_cast<int>(symbols_matching(s).size());
        const auto in = picker_key(s.sym_q, k, md, s.sym_sel, n);
        if (in.act == PickerIntent::Act::close) s.sym_open = false;
        else if (in.act == PickerIntent::Act::choose) go_to_symbol(s, in.index);
        return;
    }
    if (s.comp_open) {   // the popup takes the keys that steer it; the rest edit and re-rank
        if (k == Key::escape) { s.comp_open = false; return; }
        if (k == Key::down || (md.ctrl && k == Key::n)) { s.comp_sel = (s.comp_sel + 1) % static_cast<int>(s.comp.size()); return; }
        if (k == Key::up || (md.ctrl && k == Key::p)) { s.comp_sel = (s.comp_sel + static_cast<int>(s.comp.size()) - 1) % static_cast<int>(s.comp.size()); return; }
        if ((k == Key::tab || k == Key::enter) && md.none()) { accept_completion(s, s.comp_sel); return; }
    }
    if (s.disk_changed && k == Key::r && cmd && sh) { s.out.push_back({TextEditorAction::Kind::reload, {}}); return; }
    if (cmd && sh && !alt && k == Key::o) { open_symbols(s); return; }
    if (md.ctrl && !md.super && !alt && k == Key::space) { complete(s, true); return; }   // ask for completions

    if (s.goto_open) {
        if (k == Key::escape) { s.goto_open = false; return; }
        if (k == Key::enter) { go_to_line(s); return; }
        if (paste_key) { s.out.push_back({TextEditorAction::Kind::paste, {}}); return; }
        s.goto_q.handle_key(k, md);
        return;
    }
    if (s.find_open && s.find_focused) {
        TextEditState& f = s.find_field == 0 ? s.find_q : s.find_r;
        if (k == Key::escape) { close_find(s); return; }
        if (k == Key::enter) { if (s.find_field == 0) goto_match(s, sh ? -1 : +1); else replace_one(s); return; }
        if (k == Key::tab && s.find_replace) { s.find_field = 1 - s.find_field; return; }
        if (alt && !cmd && k == Key::c) { s.find_opts.case_sensitive = !s.find_opts.case_sensitive; refind(s); return; }
        if (alt && !cmd && k == Key::w) { s.find_opts.whole_word = !s.find_opts.whole_word; refind(s); return; }
        if (alt && !cmd && k == Key::r) { s.find_opts.regex = !s.find_opts.regex; refind(s); return; }
        if ((cmd && k == Key::g) || k == Key::f3) { goto_match(s, sh ? -1 : +1); return; }
        if (cmd && !alt && k == Key::f) { f.select_all(); return; }
        if (cmd && alt && k == Key::f) { s.find_replace = true; s.find_field = 1; return; }
        if (cmd && !sh && !alt && k == Key::s) { s.out.push_back({TextEditorAction::Kind::save, {}}); return; }
        if (copy_key) {
            const std::size_t a = std::min(f.anchor, f.caret), b = std::max(f.anchor, f.caret);
            if (b > a) s.out.push_back({TextEditorAction::Kind::copy, f.text.substr(a, b - a)});
            return;
        }
        if (paste_key) { s.out.push_back({TextEditorAction::Kind::paste, {}}); return; }
        if (k == Key::down && s.find_replace && s.find_field == 0) { s.find_field = 1; return; }
        if (k == Key::up && s.find_field == 1) { s.find_field = 0; return; }
        f.handle_key(k, md);
        if (s.find_field == 0) refind(s);
        return;
    }
    if (cmd && !sh && !alt && k == Key::s) { s.out.push_back({TextEditorAction::Kind::save, {}}); return; }
    if (cmd && !sh && k == Key::f) { open_find(s, alt); return; }
    if (md.ctrl && !md.super && !alt && !sh && k == Key::h) { open_find(s, true); return; }
    if (goto_key) { s.find_focused = false; s.goto_open = true; s.goto_q.set({}); return; }
    // find next / previous: ⌘G on the Mac (ctrl+G is go to line there and
    // everywhere), F3 on every platform
    if ((md.super && !md.ctrl && k == Key::g) || k == Key::f3) {
        if (!s.find_open && !s.find_q.text.empty()) { s.find_open = true; refind(s); }
        goto_match(s, sh ? -1 : +1);
        return;
    }
    if (copy_key) { const std::string t = s.ed.copy(); if (!t.empty()) s.out.push_back({TextEditorAction::Kind::copy, t}); return; }
    if (cmd && !sh && !alt && k == Key::x) {
        const std::string t = s.ed.cut();
        if (!t.empty()) s.out.push_back({TextEditorAction::Kind::copy, t});
        refind(s);
        return;
    }
    if (paste_key) { s.out.push_back({TextEditorAction::Kind::paste, {}}); return; }
    if (k == Key::escape && s.find_open && s.ed.sels.size() == 1 && s.ed.primary_sel().empty()) { close_find(s); return; }
    const std::uint64_t v0 = s.ed.version();
    (void)s.ed.handle_key(k, md);
    if (s.ed.version() != v0) refind(s);
    // backspace keeps the popup current; a move away puts it away
    if (s.comp_open) { if (k == Key::backspace) complete(s); else s.comp_open = false; }
}

/// Typed text (after the input method).
inline void text(TextEditorState& s, std::string_view t) {
    if (s.sym_open) { s.sym_q.insert(t); s.sym_sel = 0; return; }
    if (s.goto_open) { for (char ch : t) if (ch >= '0' && ch <= '9') s.goto_q.insert(std::string(1, ch)); return; }
    if (s.find_open && s.find_focused) {
        (s.find_field == 0 ? s.find_q : s.find_r).insert(t);
        if (s.find_field == 0) refind(s);
        return;
    }
    s.ed.insert_text(t);
    refind(s);
    // a word character keeps completing; anything else ends the word
    if (!t.empty() && editor::detail::is_word(static_cast<unsigned char>(t.back()))) complete(s);
    else s.comp_open = false;
}

/// A press on the code at `p`: `count` 2 a word, 3 the line; alt adds a
/// cursor, shift extends.
inline void press(TextEditorState& s, editor::Pos p, int count, Mods md) {
    s.find_focused = false;
    s.goto_open = false;
    s.comp_open = false;
    s.sym_open = false;
    if (count == 2) s.ed.select_word_at(p);
    else if (count >= 3) s.ed.select_line_at(p.line);
    else if (md.alt) s.ed.add_cursor_at(p);
    else s.ed.set_cursor(p, md.shift);
}

/// A press on the minimap at `y` px from its top: the view centres there,
/// sliding (a drag follows the pointer directly).
inline void minimap_seek(TextEditorState& s, float y, bool slide) {
    const auto& mt = s.ed.metrics;
    if (mt.minimap_line_px <= 0.0f) return;
    const int row = mt.minimap_top + static_cast<int>(y / mt.minimap_line_px);
    const float goal = std::max(0.0f, static_cast<float>(row) * mt.line_h - s.ed.scroll.viewport.y * 0.5f);
    if (slide) s.scroll_goal_y = std::min(goal, s.ed.scroll.max_offset.y);
    else { s.scroll_goal_y = -1.0f; s.ed.scroll.scroll_to({s.ed.scroll.offset.x, goal}); }
}

/// A pinned scope header was clicked: go to its line, sliding.
inline void go_to_scope(TextEditorState& s, int line) {
    s.ed.set_cursor(editor::Pos{line, s.ed.indent_of(line)});
    const float goal = std::max(0.0f, static_cast<float>(s.ed.visual_row_of(line)) * s.ed.metrics.line_h - s.ed.metrics.line_h * 2.0f);
    s.scroll_goal_y = std::min(goal, std::max(s.ed.scroll.max_offset.y, goal));
}

/// Is anything still easing? (The app keeps frames coming while it is.)
[[nodiscard]] inline bool animating(const TextEditorState& s) noexcept {
    if (s.scroll_goal_y >= 0.0f || s.center_line >= 0) return true;
    if (s.caret_row < 0.0f) return false;
    const editor::Pos c = s.ed.cursor();
    const float row = static_cast<float>(s.ed.visual_row_of(c.line)), col = static_cast<float>(s.ed.display_col(c.line, c.col));
    return std::abs(row - s.caret_row) > 0.01f || std::abs(col - s.caret_col) > 0.01f;
}

/// Advance the motion by `dt` seconds (critically damped: fast, no overshoot).
/// `motion` false: snap. Returns whether anything is still moving.
inline bool step(TextEditorState& s, double dt, bool motion = true) {
    // a jump waiting for the view's size: centre it now (a fresh file snaps
    // there; one already on screen slides)
    if (s.center_line >= 0 && s.ed.scroll.viewport.y > 0.0f) {
        const float lh = s.ed.metrics.line_h;
        const float goal = std::clamp(static_cast<float>(s.ed.visual_row_of(s.center_line)) * lh - s.ed.scroll.viewport.y * 0.38f,
                                      0.0f, s.ed.scroll.max_offset.y);
        if (s.caret_row < 0.0f || !motion) { s.ed.scroll.scroll_to({0.0f, goal}); s.scroll_goal_y = -1.0f; }
        else s.scroll_goal_y = goal;
        s.center_line = -1;
    }
    const editor::Pos c = s.ed.cursor();
    const float row = static_cast<float>(s.ed.visual_row_of(c.line)), col = static_cast<float>(s.ed.display_col(c.line, c.col));
    const float k = motion ? 1.0f - std::exp(-static_cast<float>(dt) * 28.0f) : 1.0f;
    if (s.caret_row < 0.0f || !motion) { s.caret_row = row; s.caret_col = col; }
    else {
        // a long jump (a page, a search hit) arrives fast: the glide is a
        // flourish, never a wait
        const float far = std::abs(row - s.caret_row) > 40.0f ? 0.7f : 0.0f;
        const float kk = std::max(k, far);
        s.caret_row += (row - s.caret_row) * kk;
        s.caret_col += (col - s.caret_col) * kk;
        if (std::abs(row - s.caret_row) < 0.01f) s.caret_row = row;
        if (std::abs(col - s.caret_col) < 0.01f) s.caret_col = col;
    }
    if (s.scroll_goal_y >= 0.0f) {
        const float y = s.ed.scroll.offset.y;
        const float ks = motion ? 1.0f - std::exp(-static_cast<float>(dt) * 18.0f) : 1.0f;
        float ny = y + (s.scroll_goal_y - y) * ks;
        if (std::abs(s.scroll_goal_y - ny) < 0.5f) { ny = s.scroll_goal_y; s.scroll_goal_y = -1.0f; }
        s.ed.scroll.scroll_to({s.ed.scroll.offset.x, ny});
    }
    return animating(s);
}

/// Turn an instant scroll the editor just made (reveal after a jump) into a
/// slide from where the view was: call with the offset before the op.
inline void slide_from(TextEditorState& s, float before_y, bool motion) {
    const float after = s.ed.scroll.offset.y;
    if (!motion || std::abs(after - before_y) < s.ed.metrics.line_h * 3.0f) return;   // a nudge: no slide
    s.scroll_goal_y = after;
    s.ed.scroll.scroll_to({s.ed.scroll.offset.x, before_y});
}

/// Go to `r` from outside the editor (a project search hit, a link): select
/// it and bring its line to the upper middle of the view -- the reading
/// position -- sliding there when the file is already showing.
inline void jump_to(TextEditorState& s, editor::Range r) {
    const auto keep = s.ed.scroll.offset;
    r.start.line = std::clamp(r.start.line, 0, s.ed.line_count() - 1);
    r.end.line = std::clamp(r.end.line, 0, s.ed.line_count() - 1);
    r.start.col = std::clamp(r.start.col, 0, s.ed.line_len(r.start.line));
    r.end.col = std::clamp(r.end.col, 0, s.ed.line_len(r.end.line));
    s.ed.select_range(r);
    s.ed.scroll.scroll_to(keep);   // the centring decides where the view goes
    s.center_line = r.start.line;
    s.comp_open = s.sym_open = s.goto_open = s.find_focused = false;
}

}  // namespace text_editor_ops

/// A key, as an op (empty only for a bare modifier, which does nothing).
[[nodiscard]] inline std::optional<TextEditorOp> text_editor_key(const TextEditorState&, Key k, Mods md) {
    if (k == Key::ctrl || k == Key::shift || k == Key::alt || k == Key::super || k == Key::caps_lock) return std::nullopt;
    return TextEditorOp{[k, md](TextEditorState& s) {
        // a jump the keys made (find next, go to line, page, the file's ends)
        // slides there; a step within the screen just scrolls
        // mid-slide: the next jump slides on from where the view IS now;
        // a key that doesn't move the view leaves the slide going
        const float shown = s.ed.scroll.offset.y, going = s.scroll_goal_y;
        if (going >= 0.0f) s.ed.scroll.scroll_to({s.ed.scroll.offset.x, going});   // reveal works against the destination
        const float before = s.ed.scroll.offset.y;
        s.scroll_goal_y = -1.0f;
        text_editor_ops::key(s, k, md);
        if (going >= 0.0f && std::abs(s.ed.scroll.offset.y - before) < 0.5f) {   // no new jump: keep sliding
            s.scroll_goal_y = going;
            s.ed.scroll.scroll_to({s.ed.scroll.offset.x, shown});
            return;
        }
        text_editor_ops::slide_from(s, going >= 0.0f ? shown : before, true);
    }};
}

[[nodiscard]] inline std::optional<TextEditorOp> text_editor_text(const TextEditorState&, std::string_view text) {
    if (text.empty()) return std::nullopt;
    return TextEditorOp{[t = std::string{text}](TextEditorState& s) { text_editor_ops::text(s, t); }};
}

// ── the widget ──────────────────────────────────────────────────────────────

/// A click on one of the editor's OVERLAYS (they float outside its tree, so
/// its handlers can't see them): the symbol picker's rows, or a click outside
/// it. Call from the app's on(Clicked) / on(Left) with the editor's id.
[[nodiscard]] inline std::optional<TextEditorOp> text_editor_clicked(const TextEditorState& st, std::string_view id, std::uint64_t node) {
    if (!st.sym_open) return std::nullopt;
    const std::string sid = std::string{id} + ".symbols";
    if (node == part_id(sid, part_menu)) return TextEditorOp{[](TextEditorState& s) { s.sym_open = false; }};
    const int n = static_cast<int>(text_editor_ops::symbols_matching(st).size());
    for (int k = 0; k < n; ++k)
        if (node == part_id(sid, static_cast<std::size_t>(k)))
            return TextEditorOp{[k](TextEditorState& s) { text_editor_ops::go_to_symbol(s, k); }};
    return std::nullopt;
}

template <ControlContext C, class On>
    requires std::invocable<const On&, TextEditorOp>
[[nodiscard]] inline Node text_editor(const C& c, std::string_view id, const TextEditorState& st, const TextEditorProps& p, On on) {
    using R = std::invoke_result_t<const On&, TextEditorOp>;
    const Theme& t = c.theme;
    const CodeTheme& th = p.theme;
    const std::string base{id};
    const std::string code_id = base + ".code", find_id = base + ".find", goto_id = base + ".goto";
    const editor::EditorState& ed = st.ed;
    const auto op = [on](auto f) -> std::optional<R> { return on(TextEditorOp{std::move(f)}); };
    // the chrome sits on the code's own paper, a step toward the ink
    const Color<Srgb> paper = th.bg, ink = th.tok[static_cast<std::size_t>(editor::Tok::text)];
    const Color<Srgb> chrome = mix(paper, ink, 0.035f), rule = mix(paper, ink, 0.09f);
    const Color<Srgb> muted = mix(ink, paper, 0.42f), faint = mix(ink, paper, 0.6f);

    // ── where you are: the path, the file, the symbol around the caret ─────
    std::vector<Crumb> crumbs;
    {
        std::string sp = p.shown_path.empty() ? p.path : p.shown_path;
        std::vector<std::string> parts;
        std::size_t a = 0;
        for (std::size_t i = 0; i <= sp.size(); ++i)
            if (i == sp.size() || sp[i] == '/' || sp[i] == '\\') { if (i > a) parts.push_back(sp.substr(a, i - a)); a = i + 1; }
        // the last few folders say where; the whole path is the tooltip's job
        const std::size_t keep = 3;
        if (parts.size() > keep + 1) { parts.erase(parts.begin(), parts.end() - static_cast<std::ptrdiff_t>(keep + 1)); parts.insert(parts.begin(), "\u2026"); }
        for (std::size_t i = 0; i < parts.size(); ++i) crumbs.push_back({parts[i], i + 1 == parts.size(), false});
        const editor::Symbol* inner = nullptr;
        for (const auto& s : ed.symbols()) if (s.line <= ed.cursor().line) inner = &s;
        if (inner != nullptr) crumbs.push_back({inner->name, false, true});
    }
    const bool dirty = ed.dirty();
    Node state_mark = p.saving ? (text<"saving\u2026"> | font(t.type(Tier::small)) | fg(muted)).build()
                    : dirty    ? (h(text<"\u25CF"> | font(t.type(Tier::small)) | fg(t.warning)) | tooltip("Unsaved changes \u2014 \u2318S saves")).build()
                               : Node{};
    Node host_chip;
    if (!p.host.empty()) {   // a file on another machine says so first, always
        const Color<Srgb> tone = p.host_danger ? t.danger : t.info;
        host_chip = (h(text<"\u21C4"> | font(t.type(Tier::small)) | fg(tone),
                       text_owned(p.host) | font(t.type(Tier::small)) | semibold | fg(p.host_danger ? tone : ink),
                       node(p.host_danger ? (text<"PROD"> | font(t.type(Tier::fine)) | bold | tracking(0.6f) | fg(tone)).build() : Node{}))
                     | gap(t.spaced(5)) | align(Align::center) | pad(t.spaced(2), t.spaced(8)) | radius(t.rounded(4))
                     | bg(mix(chrome, tone, 0.14f)) | border(t.stroke(1), mix(chrome, tone, 0.35f))
                     | tooltip(p.host_danger ? "A production machine: saves land on it" : "On " + p.host + ", over ssh")).build();
    }
    Node head = (h(node(std::move(host_chip)), node(breadcrumbs(c, base + ".crumbs", crumbs)), spacer(), node(std::move(state_mark)))
                 | align(Align::center) | gap(t.spaced(8)) | pad(t.spaced(5), t.spaced(12)) | width(pct(100))
                 | bg(chrome) | inner_shadow(0.0f, rule, {0.0f, -1.0f}) | tooltip(p.path)).build();

    // ── find, or go to line ────────────────────────────────────────────────
    Node bar;
    if (st.find_open) {
        bar = (node(find_bar(c, find_id, st.find_q, st.find_r, st.find_opts, st.match_i, static_cast<int>(st.matches.size()),
                             st.find_replace, st.find_focused ? st.find_field : -1))
               | on_click([op, fid = find_id](const Clicked& ck) -> std::optional<R> {
                     namespace fp = find_part;
                     for (std::size_t i = 0; i <= fp::expand; ++i) {
                         if (ck.node != part_id(fid, i)) continue;
                         return op([i](TextEditorState& s) {
                             namespace o = text_editor_ops;
                             switch (i) {
                                 case fp::query:   s.find_focused = true; s.find_field = 0; break;
                                 case fp::replace: s.find_focused = true; s.find_field = 1; break;
                                 case fp::case_:   s.find_opts.case_sensitive = !s.find_opts.case_sensitive; o::refind(s); break;
                                 case fp::word:    s.find_opts.whole_word = !s.find_opts.whole_word; o::refind(s); break;
                                 case fp::regex:   s.find_opts.regex = !s.find_opts.regex; o::refind(s); break;
                                 case fp::prev:    o::goto_match(s, -1); break;
                                 case fp::next:    o::goto_match(s, +1); break;
                                 case fp::replace_one: o::replace_one(s); break;
                                 case fp::replace_all: o::replace_all(s); break;
                                 case fp::close:   o::close_find(s); break;
                                 case fp::expand:  s.find_replace = !s.find_replace; if (!s.find_replace) s.find_field = 0; break;
                                 default: break;
                             }
                         });
                     }
                     return std::nullopt;
                 })).build();
    } else if (st.goto_open) {
        const std::string range = "1\u2013" + std::to_string(ed.line_count());
        bar = (h(text<"Go to line"> | font(t.type(Tier::small)) | fg(muted),
                 node((text_field(t, st.goto_q, true, part_id(goto_id, 0), range) | width(140)).build()),
                 text_owned("\u23CE go \u00B7 esc") | font(t.type(Tier::small)) | fg(faint))
               | gap(t.spaced(8)) | align(Align::center) | pad(t.spaced(6), t.spaced(12)) | width(pct(100))
               | bg(var::surface) | border(t.stroke(1), t.border)).build();
    }

    // ── the code ───────────────────────────────────────────────────────────
    CodeViewOptions o;
    o.font_size = p.font_size;
    o.line_height = p.line_height;
    o.focused = p.focused && !st.find_focused && !st.goto_open;
    o.cursor_on = p.cursor_on;
    o.find_matches = st.find_open ? &st.matches : nullptr;
    o.find_current = st.match_i;
    o.family = p.family;
    o.ligatures = p.ligatures;
    o.rulers = {};
    o.minimap = p.minimap;
    o.sticky = p.sticky;
    if (p.motion) { o.caret_col = st.caret_col; o.caret_row = st.caret_row; }
    const std::uint64_t surface = node_id(code_id), bar_id = part_id(code_id, part_bar), mini_id = part_id(code_id, part_minimap);
    // The pointer resolves against the state when the op is FOLDED, not when
    // the tree was built: the metrics the hit test reads are the ones on screen.
    Node code = (node(code_view(c, code_id, ed, th, o))
                 | grow() | min_size(0, 0) | width(pct(100))
                 | on_press([op, surface, bar_id, mini_id](const Pressed& pr) -> std::optional<R> {
                       if (pr.button != MouseButton::left) return std::nullopt;
                       if (pr.node == bar_id) return op([y = pr.local.y](TextEditorState& s) { scrollbar_press(s.ed.scroll, y); });
                       if (pr.node == mini_id) return op([y = pr.local.y](TextEditorState& s) { text_editor_ops::minimap_seek(s, y, true); });
                       if (pr.node != surface) return std::nullopt;
                       return op([at = pr.local, n = pr.count, md = pr.mods](TextEditorState& s) {
                           text_editor_ops::press(s, code_view_hit(s.ed, at), n, md);
                       });
                   })
                 | on_drag([op, surface, bar_id, mini_id](const Dragged& dr) -> std::optional<R> {
                       if (dr.node == bar_id) return op([tr = dr.travel](TextEditorState& s) { s.scroll_goal_y = -1.0f; scrollbar_drag(s.ed.scroll, tr); });
                       if (dr.node == mini_id) return op([y = dr.local.y](TextEditorState& s) { text_editor_ops::minimap_seek(s, y, false); });
                       if (dr.node != surface) return std::nullopt;
                       return op([at = dr.local](TextEditorState& s) { s.ed.drag_to(code_view_hit(s.ed, at)); });
                   })
                 | on_click([op, code_id, surface, bar_id, mini_id](const Clicked& ck) -> std::optional<R> {
                       if (ck.node == surface || ck.node == bar_id || ck.node == mini_id) return std::nullopt;
                       // a fold chevron or a pinned scope: whichever line's it is
                       return op([code_id, n = ck.node](TextEditorState& s) {
                           for (int l = 0; l < s.ed.line_count(); ++l)
                               if (n == part_id(code_id, part_sticky_base + static_cast<std::size_t>(l))) { text_editor_ops::go_to_scope(s, l); return; }
                           for (const int l : s.ed.visual_lines())
                               if (n == part_id(code_id, part_fold_base + static_cast<std::size_t>(l))) { s.ed.toggle_fold(l); return; }
                       });
                   })
                 | on_scroll([op](const Scrolled& sc) -> std::optional<R> {
                       return op([d = sc.delta](TextEditorState& s) { s.scroll_goal_y = -1.0f; s.ed.scroll.scroll_by(d); });
                   })).build();

    // ── the status line ────────────────────────────────────────────────────
    using T = StatusItem::Tone;
    std::vector<StatusItem> left, right;
    const editor::Pos cur = ed.cursor();
    left.push_back({"Ln " + std::to_string(cur.line + 1) + ", Col " + std::to_string(ed.display_col(cur.line, cur.col) + 1), "", T::normal, false, false, "line, column"});
    if (ed.sels.size() > 1) left.push_back({std::to_string(ed.sels.size()) + " cursors", "", T::accent, false, false, "esc: back to one"});
    else if (!ed.primary_sel().empty()) {
        const std::string sel = ed.text_in(ed.primary_sel().range());
        const int lines = ed.primary_sel().end().line - ed.primary_sel().start().line + 1;
        left.push_back({std::to_string(sel.size()) + " selected" + (lines > 1 ? " (" + std::to_string(lines) + " lines)" : std::string{}), "", T::dim, false, false, ""});
    }
    if (!p.error.empty()) left.push_back({"not saved: " + p.error, "", T::error, false, false, p.error});
    right.push_back({std::string{ed.language->name}, "", T::dim, false, false, "language, from the file name"});
    right.push_back({ed.soft_tabs ? "Spaces: " + std::to_string(ed.tab_width) : std::string{"Tabs"}, "", T::dim, false, false, "indentation, read from the file"});
    right.push_back({st.crlf ? "CRLF" : "LF", "", T::dim, false, false, "line endings, kept as the file had them"});
    right.push_back({"UTF-8", "", T::dim, false, false, ""});
    Node status = (node(status_bar(c, base + ".status", left, right)) | width(pct(100))).build();

    // ── completion: a popup at the caret, in the code's own colours ────────
    if (st.comp_open && p.focused && !st.comp.empty()) {
        const float px = p.font_size;
        const float row_h = std::round(px * 1.7f);
        const Color<Srgb> pop = mix(paper, ink, 0.06f), edge = mix(paper, ink, 0.16f);
        constexpr int kRows = 8;
        const int n = static_cast<int>(st.comp.size());
        const int sel = std::clamp(st.comp_sel, 0, n - 1);
        const int top = std::clamp(sel - kRows + 2, 0, std::max(0, n - kRows));
        std::vector<Node> rows;
        for (int k = top; k < std::min(n, top + kRows); ++k) {
            const EditorCompletion& it = st.comp[static_cast<std::size_t>(k)];
            const bool on = k == sel;
            using F = EditorCompletion::From;
            // a kind mark in the colour its kind has in the code
            const Color<Srgb> kc = it.from == F::keyword ? th.of(editor::Tok::keyword)
                                 : it.from == F::type ? th.of(editor::Tok::type)
                                 : it.from == F::symbol ? th.of(editor::Tok::function) : muted;
            const char* mark = it.from == F::keyword ? "kw" : it.from == F::type ? "T" : it.from == F::symbol ? "\u0192" : "ab";
            const auto hits = st.comp_hits[static_cast<std::size_t>(k)];
            std::vector<Node> word;
            {   // the word, typed letters bright
                std::size_t i = 0, h = 0;
                while (i < it.word.size()) {
                    const bool hit = h < hits.size() && hits[h] == i;
                    std::size_t j = i;
                    if (hit) { while (j < it.word.size() && h < hits.size() && hits[h] == j) { ++j; ++h; } }
                    else j = h < hits.size() ? hits[h] : it.word.size();
                    word.push_back((text_owned(it.word.substr(i, j - i)) | font(px) | family(p.family)
                                    | fg(hit ? ink : mix(ink, paper, 0.25f)) | (hit ? semibold : weight(FontWeight::regular))).build());
                    i = j;
                }
            }
            const auto rid = part_id(base + ".comp", static_cast<std::size_t>(k));
            rows.push_back((h(h(text_owned(mark) | font(t.type_at(px, Tier::small)) | semibold | fg(kc))
                                  | size(22, 16) | center | radius(t.rounded(3)) | bg(mix(pop, kc, 0.16f)),
                              h(std::move(word)) | grow() | min_size(0, 0) | clip,
                              text_owned(it.from == F::file ? (it.uses > 1 ? std::to_string(it.uses) + "\u00D7" : std::string{}) : it.detail)
                                  | font(t.type_at(px, Tier::small)) | fg(faint))
                            | gap(t.spaced(8)) | align(Align::center) | pad(0, t.spaced(8)) | height(row_h) | width(pct(100))
                            | radius(t.rounded(4)) | bg(on ? mix(pop, th.cursor, 0.2f) : pop.fade(0.0f))
                            | pointer_cursor | id_of_raw(rid)
                            | on_click([op, k](const Clicked&) -> std::optional<R> {
                                  return op([k](TextEditorState& s) { text_editor_ops::accept_completion(s, k); });
                              })).build());
        }
        rows.push_back((h(text<"\u21E5 / \u23CE accept  \u00B7  \u2191\u2193 choose  \u00B7  esc"> | font(t.type(Tier::fine)) | fg(faint))
                        | pad(t.spaced(4), t.spaced(9)) | width(pct(100)) | inner_shadow(0.0f, edge, {0.0f, 1.0f})).build());
        c.overlay(Overlay{
            .content = (v(std::move(rows)) | gap(1.0f) | pad(t.spaced(4), t.spaced(4), 0.0f, t.spaced(4)) | width(std::max(260.0f, px * 24.0f))
                        | bg(pop) | border(t.stroke(1), edge) | radius(t.rounded(7)) | elevation(var::depth(10))
                        | role(a11y::Role::menu) | label("completions")).build(),
            .id = part_id(base + ".comp", part_menu),
            .anchor_id = part_id(code_id, part_caret),
            .placement = Placement::below_start,
            .dismiss = Dismiss::never,   // typing goes on in the editor
            .offset = 2.0f,
            .shift = Vec2{-30.0f, 0.0f},   // the word column lines up with the typed word
            .layer = 8,
        });
    }

    // ── go to symbol: the file's outline as a picker ───────────────────────
    if (st.sym_open) {
        const auto ranked = text_editor_ops::symbols_matching(st);
        std::vector<PickerItem> items;
        Ranked rk;
        const auto& syms = ed.symbols();
        for (const auto& [i, m] : ranked) {
            const auto& sym = syms[static_cast<std::size_t>(i)];
            rk.order.push_back(static_cast<int>(items.size()));
            items.push_back({.label = sym.name, .detail = std::string(static_cast<std::size_t>(sym.depth) * 2, ' ') + sym.kind,
                             .hint = std::to_string(sym.line + 1)});
            rk.matches.push_back(m);
        }
        picker(c, base + ".symbols", st.sym_q, items, rk, st.sym_sel,
               PickerStyle{.placeholder = "Go to a symbol in this file", .prefix = "@", .width = 560.0f,
                           .empty_text = syms.empty() ? "No symbols found in this file" : "No matches",
                           .footer = {{"\u23CE", "go"}, {"esc", "close"}}});
    }

    // ── the file changed on disk ───────────────────────────────────────────
    Node banner;
    if (st.disk_changed) {
        const auto rid = node_id(base + ".reload");
        const bool hov = c.hovered(rid);
        banner = (h(text<"\u21BB"> | font(t.type(Tier::body)) | fg(t.warning),
                    text<"This file changed on disk."> | font(t.type(Tier::body)) | semibold | fg(ink),
                    text<"Your edits are kept until you choose."> | font(t.type(Tier::small)) | fg(muted) | grow() | min_size(0, 0) | ellipsis,
                    h(text<"Reload from disk"> | font(t.type(Tier::small)) | semibold | fg(t.warning),
                      text<"\u2318\u21E7R"> | font(t.type(Tier::fine)) | fg(muted))
                        | gap(t.spaced(6)) | align(Align::center) | pad(t.spaced(3), t.spaced(10)) | radius(t.rounded(5))
                        | bg(hov ? t.warning.fade(0.22f) : t.warning.fade(0.12f)) | pointer_cursor | id_of_raw(rid)
                        | on_click([op](const Clicked&) -> std::optional<R> {
                              return op([](TextEditorState& s) { s.out.push_back({TextEditorAction::Kind::reload, {}}); });
                          }))
                  | gap(t.spaced(8)) | align(Align::center) | pad(t.spaced(6), t.spaced(12)) | width(pct(100))
                  | bg(mix(paper, t.warning, 0.08f)) | inner_shadow(0.0f, mix(paper, t.warning, 0.3f), {0.0f, -1.0f})).build();
    }

    std::vector<Node> parts;
    parts.push_back(std::move(head));
    if (st.disk_changed) parts.push_back(std::move(banner));
    if (st.find_open || st.goto_open) parts.push_back(std::move(bar));
    parts.push_back(std::move(code));
    parts.push_back(std::move(status));
    return (v(std::move(parts)) | width(pct(100)) | height(pct(100)) | min_size(0, 0) | bg(paper) | id_of(id)).build();
}

}  // namespace mayag::dsl
