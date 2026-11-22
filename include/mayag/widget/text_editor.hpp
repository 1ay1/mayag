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
#include "find_bar.hpp"
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
    enum class Kind : std::uint8_t { save, copy, paste };
    Kind        kind = Kind::save;
    std::string text;   ///< copy: what to put on the clipboard
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

/// One key. Every key is the editor's: nothing it doesn't use reaches anyone.
inline void key(TextEditorState& s, Key k, Mods md) {
    const bool cmd = md.primary(), sh = md.shift, alt = md.alt;
    const bool copy_key  = (cmd && !sh && !alt && k == Key::c) || (md.ctrl && sh && !alt && k == Key::c);
    const bool paste_key = (cmd && !sh && !alt && k == Key::v) || (md.ctrl && sh && !alt && k == Key::v) || (sh && k == Key::insert);
    const bool goto_key  = md.ctrl && !md.super && !alt && !sh && k == Key::g;   // ctrl+G everywhere (⌘G is find next)

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
}

/// Typed text (after the input method).
inline void text(TextEditorState& s, std::string_view t) {
    if (s.goto_open) { for (char ch : t) if (ch >= '0' && ch <= '9') s.goto_q.insert(std::string(1, ch)); return; }
    if (s.find_open && s.find_focused) {
        (s.find_field == 0 ? s.find_q : s.find_r).insert(t);
        if (s.find_field == 0) refind(s);
        return;
    }
    s.ed.insert_text(t);
    refind(s);
}

/// A press on the code at `p`: `count` 2 a word, 3 the line; alt adds a
/// cursor, shift extends.
inline void press(TextEditorState& s, editor::Pos p, int count, Mods md) {
    s.find_focused = false;
    s.goto_open = false;
    if (count == 2) s.ed.select_word_at(p);
    else if (count >= 3) s.ed.select_line_at(p.line);
    else if (md.alt) s.ed.add_cursor_at(p);
    else s.ed.set_cursor(p, md.shift);
}

}  // namespace text_editor_ops

/// A key, as an op (empty only for a bare modifier, which does nothing).
[[nodiscard]] inline std::optional<TextEditorOp> text_editor_key(const TextEditorState&, Key k, Mods md) {
    if (k == Key::ctrl || k == Key::shift || k == Key::alt || k == Key::super || k == Key::caps_lock) return std::nullopt;
    return TextEditorOp{[k, md](TextEditorState& s) { text_editor_ops::key(s, k, md); }};
}

[[nodiscard]] inline std::optional<TextEditorOp> text_editor_text(const TextEditorState&, std::string_view text) {
    if (text.empty()) return std::nullopt;
    return TextEditorOp{[t = std::string{text}](TextEditorState& s) { text_editor_ops::text(s, t); }};
}

// ── the widget ──────────────────────────────────────────────────────────────

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
    Node head = (h(node(breadcrumbs(c, base + ".crumbs", crumbs)), spacer(), node(std::move(state_mark)))
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
    const std::uint64_t surface = node_id(code_id), bar_id = part_id(code_id, part_bar);
    // The pointer resolves against the state when the op is FOLDED, not when
    // the tree was built: the metrics the hit test reads are the ones on screen.
    Node code = (node(code_view(c, code_id, ed, th, o))
                 | grow() | min_size(0, 0) | width(pct(100))
                 | on_press([op, surface, bar_id](const Pressed& pr) -> std::optional<R> {
                       if (pr.button != MouseButton::left) return std::nullopt;
                       if (pr.node == bar_id) return op([y = pr.local.y](TextEditorState& s) { scrollbar_press(s.ed.scroll, y); });
                       if (pr.node != surface) return std::nullopt;
                       return op([at = pr.local, n = pr.count, md = pr.mods](TextEditorState& s) {
                           text_editor_ops::press(s, code_view_hit(s.ed, at), n, md);
                       });
                   })
                 | on_drag([op, surface, bar_id](const Dragged& dr) -> std::optional<R> {
                       if (dr.node == bar_id) return op([tr = dr.travel](TextEditorState& s) { scrollbar_drag(s.ed.scroll, tr); });
                       if (dr.node != surface) return std::nullopt;
                       return op([at = dr.local](TextEditorState& s) { s.ed.drag_to(code_view_hit(s.ed, at)); });
                   })
                 | on_click([op, code_id, surface, bar_id](const Clicked& ck) -> std::optional<R> {
                       if (ck.node == surface || ck.node == bar_id) return std::nullopt;
                       // a fold chevron: whichever line's it is
                       return op([code_id, n = ck.node](TextEditorState& s) {
                           for (const int l : s.ed.visual_lines())
                               if (n == part_id(code_id, part_fold_base + static_cast<std::size_t>(l))) { s.ed.toggle_fold(l); return; }
                       });
                   })
                 | on_scroll([op](const Scrolled& sc) -> std::optional<R> {
                       return op([d = sc.delta](TextEditorState& s) { s.ed.scroll.scroll_by(d); });
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

    std::vector<Node> parts;
    parts.push_back(std::move(head));
    if (st.find_open || st.goto_open) parts.push_back(std::move(bar));
    parts.push_back(std::move(code));
    parts.push_back(std::move(status));
    return (v(std::move(parts)) | width(pct(100)) | height(pct(100)) | min_size(0, 0) | bg(paper) | id_of(id)).build();
}

}  // namespace mayag::dsl
