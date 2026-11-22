#pragma once
// mayag::editor buffer — the text model of a code editor
//
//   EditorState ed = EditorState::from_text(file_contents);
//   ed.handle_key(k.key, k.mods);      // the whole standard keymap
//   ed.insert_text(text_event.text);   // typing, with auto-pairs
//
// Everything a modern editor does to text lives here, as plain data in the
// application's model (the mayag rule: no hidden widget state):
//
//   * multiple cursors and selections — every edit applies to all of them,
//     overlapping ones merge, and the newest is the "primary"
//   * add cursor above/below, select next/all occurrences (cmd-d / cmd-shift-l)
//   * word, line, page and document motion with a remembered goal column
//   * auto-indent, smart home, auto-closing pairs, pair-aware backspace,
//     soft-tab backspace, indent/outdent, toggle comment, move/duplicate/
//     delete/join lines
//   * operation-based undo/redo (cheap on huge files) with typing coalesced
//     into words, and selections restored with the text
//   * indentation-based folding, bracket matching, find (plain, case, word,
//     regex) and replace
//   * a dirty flag that survives undo back to the saved state
//
// Positions are (line, byte column). Display columns (tabs expanded) are
// derived, for the view and for vertical motion.

#include "syntax.hpp"
#include "../core/event.hpp"
#include "../core/scroll_state.hpp"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::editor {

struct Pos {
    int line = 0;
    int col = 0;   ///< byte offset into the line
    friend constexpr auto operator<=>(const Pos&, const Pos&) = default;
};

struct Range {
    Pos start, end;
    [[nodiscard]] constexpr bool empty() const noexcept { return start == end; }
    friend constexpr bool operator==(const Range&, const Range&) = default;
};

struct Selection {
    Pos anchor, head;
    int goal = -1;   ///< display column kept across vertical motion; -1 = unset
    [[nodiscard]] constexpr bool empty() const noexcept { return anchor == head; }
    [[nodiscard]] constexpr Pos start() const noexcept { return std::min(anchor, head); }
    [[nodiscard]] constexpr Pos end() const noexcept { return std::max(anchor, head); }
    [[nodiscard]] constexpr Range range() const noexcept { return {start(), end()}; }
};

/// What the code view measured last frame, for update()-side arithmetic
/// (page size, reveal, hit testing). Written by the view, like ScrollState.
struct ViewMetrics {
    float line_h = 20.0f;
    float cell_w = 8.0f;
    float gutter_w = 48.0f;
    float text_dy = 0.0f;
    int   page_lines = 30;
};

struct FindOptions {
    bool case_sensitive = false;
    bool whole_word = false;
    bool regex = false;
    friend constexpr bool operator==(FindOptions, FindOptions) = default;
};

/// Annotations the view draws over the text. They come from outside (a
/// language server, git, a linter), so they are plain data the app owns.
struct Diagnostic {
    enum class Severity : std::uint8_t { error, warning, info, hint };
    Range       range;
    Severity    severity = Severity::error;
    std::string message;
    std::string source;
};
struct GitMark {
    enum class Kind : std::uint8_t { added, modified, deleted };
    int  line = 0;
    Kind kind = Kind::modified;
};

namespace detail {

[[nodiscard]] constexpr bool is_cont(unsigned char c) noexcept { return (c & 0xC0) == 0x80; }
[[nodiscard]] constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }
[[nodiscard]] constexpr bool is_word(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}
/// 0 space, 1 word, 2 punctuation: word motion stops where the class changes.
[[nodiscard]] constexpr int char_class(char c) noexcept {
    return is_space(c) ? 0 : is_word(static_cast<unsigned char>(c)) ? 1 : 2;
}
[[nodiscard]] constexpr char closer_of(char c) noexcept {
    switch (c) { case '(': return ')'; case '[': return ']'; case '{': return '}';
                 case '"': return '"'; case '\'': return '\''; case '`': return '`'; default: return 0; }
}
[[nodiscard]] constexpr bool is_closer(char c) noexcept { return c == ')' || c == ']' || c == '}'; }

/// Position of `p` after an edit replaced [a, b) with text ending at e.
[[nodiscard]] constexpr Pos transform(Pos p, Pos a, Pos b, Pos e) noexcept {
    if (p < a) return p;
    if (p < b) return e;                                  // inside the replaced text
    if (p.line == b.line) return {e.line, e.col + (p.col - b.col)};
    return {p.line + (e.line - b.line), p.col};
}

}  // namespace detail

class EditorState {
  public:
    std::vector<std::string> lines{std::string{}};
    std::vector<Selection>   sels{Selection{}};
    int                      primary = 0;      ///< index into sels: the newest cursor
    int                      tab_width = 4;
    bool                     soft_tabs = true;
    bool                     auto_pairs = true;
    std::string              line_comment = "//";
    const Language*          language = &language_named("Plain Text");
    ScrollState              scroll{};
    std::set<int>            folds;            ///< folded region start lines

    mutable ViewMetrics      metrics{};
    mutable SyntaxCache      syntax{};

    // ── construction ────────────────────────────────────────────────────

    [[nodiscard]] static EditorState from_text(std::string_view text, const Language* lang = nullptr) {
        EditorState e;
        e.set_text(text);
        if (lang != nullptr) e.set_language(*lang);
        return e;
    }
    void set_language(const Language& l) {
        language = &l;
        line_comment = l.line_comment.empty() ? std::string{"#"} : std::string{l.line_comment};
        syntax.reset();
    }
    void set_text(std::string_view text) {
        lines.clear();
        std::size_t start = 0;
        for (std::size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                std::string l{text.substr(start, i - start)};
                if (!l.empty() && l.back() == '\r') l.pop_back();
                lines.push_back(std::move(l));
                start = i + 1;
            }
        }
        if (lines.empty()) lines.emplace_back();
        sels = {Selection{}};
        primary = 0;
        undo_.clear(); redo_.clear();
        folds.clear();
        saved_id_ = 0;
        ++version_;
        syntax.reset();
    }
    [[nodiscard]] std::string text() const {
        std::string s;
        for (std::size_t i = 0; i < lines.size(); ++i) { if (i) s += '\n'; s += lines[i]; }
        return s;
    }

    // ── queries ─────────────────────────────────────────────────────────

    [[nodiscard]] int line_count() const noexcept { return static_cast<int>(lines.size()); }
    [[nodiscard]] const std::string& line(int i) const { return lines[static_cast<std::size_t>(i)]; }
    [[nodiscard]] int line_len(int i) const { return static_cast<int>(line(i).size()); }
    [[nodiscard]] Pos doc_end() const { return {line_count() - 1, line_len(line_count() - 1)}; }
    [[nodiscard]] const Selection& primary_sel() const { return sels[static_cast<std::size_t>(primary)]; }
    [[nodiscard]] Pos cursor() const { return primary_sel().head; }
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

    /// The buffer's symbol outline. Cached until the text or language changes,
    /// since views ask for it every frame (breadcrumbs, go to symbol). Edits
    /// go through version_; `lines` is public, so its shape is checked too.
    [[nodiscard]] const std::vector<Symbol>& symbols() const {
        if (symbols_version_ != version_ || symbols_lang_ != language || symbols_tab_ != tab_width ||
            symbols_lines_ != lines.data() || symbols_count_ != lines.size()) {
            symbols_ = outline(*language, lines, tab_width);
            symbols_version_ = version_;
            symbols_lang_ = language;
            symbols_tab_ = tab_width;
            symbols_lines_ = lines.data();
            symbols_count_ = lines.size();
        }
        return symbols_;
    }

    [[nodiscard]] Pos clamp(Pos p) const {
        p.line = std::clamp(p.line, 0, line_count() - 1);
        p.col = std::clamp(p.col, 0, line_len(p.line));
        while (p.col > 0 && p.col < line_len(p.line) &&
               detail::is_cont(static_cast<unsigned char>(line(p.line)[static_cast<std::size_t>(p.col)])))
            --p.col;
        return p;
    }
    [[nodiscard]] std::string text_in(Range r) const {
        if (r.start.line == r.end.line)
            return line(r.start.line).substr(static_cast<std::size_t>(r.start.col),
                                             static_cast<std::size_t>(r.end.col - r.start.col));
        std::string s = line(r.start.line).substr(static_cast<std::size_t>(r.start.col));
        for (int l = r.start.line + 1; l < r.end.line; ++l) { s += '\n'; s += line(l); }
        s += '\n';
        s += line(r.end.line).substr(0, static_cast<std::size_t>(r.end.col));
        return s;
    }
    [[nodiscard]] char char_at(Pos p) const {
        if (p.line < 0 || p.line >= line_count() || p.col < 0 || p.col >= line_len(p.line)) return '\0';
        return line(p.line)[static_cast<std::size_t>(p.col)];
    }
    [[nodiscard]] char char_before(Pos p) const { return p.col > 0 ? char_at({p.line, p.col - 1}) : '\0'; }

    /// Columns on screen, tabs expanded, one per codepoint.
    [[nodiscard]] int display_col(int l, int byte) const {
        const std::string& s = line(l);
        int col = 0;
        for (int i = 0; i < byte && i < static_cast<int>(s.size()); ++i) {
            const auto c = static_cast<unsigned char>(s[static_cast<std::size_t>(i)]);
            if (c == '\t') col = (col / tab_width + 1) * tab_width;
            else if (!detail::is_cont(c)) ++col;
        }
        return col;
    }
    /// The byte offset whose display column is nearest `dcol` (rounding to
    /// the closer side of a character, as a click does).
    [[nodiscard]] int byte_at_display_col(int l, float dcol) const {
        const std::string& s = line(l);
        int col = 0;
        for (int i = 0; i < static_cast<int>(s.size()); ++i) {
            const auto c = static_cast<unsigned char>(s[static_cast<std::size_t>(i)]);
            if (detail::is_cont(c)) continue;
            const int next = c == '\t' ? (col / tab_width + 1) * tab_width : col + 1;
            if (dcol < (static_cast<float>(col) + static_cast<float>(next)) * 0.5f) return i;
            col = next;
        }
        return static_cast<int>(s.size());
    }
    [[nodiscard]] int indent_of(int l) const {
        const std::string& s = line(l);
        int i = 0;
        while (i < static_cast<int>(s.size()) && detail::is_space(s[static_cast<std::size_t>(i)])) ++i;
        return i;
    }
    [[nodiscard]] bool blank(int l) const { return indent_of(l) == line_len(l); }
    [[nodiscard]] std::string indent_unit() const {
        return soft_tabs ? std::string(static_cast<std::size_t>(tab_width), ' ') : std::string{"\t"};
    }
    /// Widest line in display columns, for horizontal scrolling. Cached.
    [[nodiscard]] int max_display_cols() const {
        if (max_cols_version_ != version_) {
            int m = 0;
            for (int l = 0; l < line_count(); ++l) m = std::max(m, display_col(l, line_len(l)));
            max_cols_ = m;
            max_cols_version_ = version_;
        }
        return max_cols_;
    }

    // ── selections ──────────────────────────────────────────────────────

    void set_cursor(Pos p, bool extend = false) {
        p = clamp(p);
        Selection s = primary_sel();
        s.head = p;
        if (!extend) s.anchor = p;
        s.goal = -1;
        sels = {s};
        primary = 0;
        occurrence_whole_word_ = false;
        break_group();
        ensure_visible(p.line);
    }
    /// Alt-click: another caret, which becomes the primary one.
    void add_cursor_at(Pos p) {
        p = clamp(p);
        for (std::size_t i = 0; i < sels.size(); ++i) {
            if (sels.size() > 1 && sels[i].empty() && sels[i].head == p) {   // alt-click a caret removes it
                sels.erase(sels.begin() + static_cast<long>(i));
                primary = static_cast<int>(sels.size()) - 1;
                return;
            }
        }
        sels.push_back(Selection{p, p});
        primary = static_cast<int>(sels.size()) - 1;
        normalize();
    }
    [[nodiscard]] Range word_at(Pos p) const {
        p = clamp(p);
        const std::string& s = line(p.line);
        int a = p.col, b = p.col;
        const int cls = a < static_cast<int>(s.size()) ? detail::char_class(s[static_cast<std::size_t>(a)])
                      : a > 0 ? detail::char_class(s[static_cast<std::size_t>(a - 1)]) : 0;
        while (a > 0 && detail::char_class(s[static_cast<std::size_t>(a - 1)]) == cls) --a;
        while (b < static_cast<int>(s.size()) && detail::char_class(s[static_cast<std::size_t>(b)]) == cls) ++b;
        return {{p.line, a}, {p.line, b}};
    }
    void select_word_at(Pos p) {
        const Range r = word_at(p);
        sels = {Selection{r.start, r.end}};
        primary = 0;
        drag_unit_ = DragUnit::word;
        drag_origin_ = r;
    }
    void select_line_at(int l) {
        l = std::clamp(l, 0, line_count() - 1);
        const Pos end = l + 1 < line_count() ? Pos{l + 1, 0} : Pos{l, line_len(l)};
        sels = {Selection{{l, 0}, end}};
        primary = 0;
        drag_unit_ = DragUnit::line;
        drag_origin_ = {{l, 0}, end};
    }
    /// A press: where a drag that follows will extend from, and by what.
    /// count 1 places a caret (char drag), 2 selects the word (word drag),
    /// 3+ the line (line drag) -- the behaviour of every desktop editor.
    void press_at(Pos p, int count, bool extend = false, bool add = false) {
        if (count >= 3) { select_line_at(clamp(p).line); return; }
        if (count == 2) { select_word_at(p); return; }
        drag_unit_ = DragUnit::character;
        if (add) add_cursor_at(p); else set_cursor(p, extend);
    }
    /// Drag: move the primary head, keeping its anchor -- by characters, or
    /// after a double/triple press by whole words/lines, always keeping the
    /// word/line the drag started on selected.
    void drag_to(Pos p) {
        p = clamp(p);
        Selection& s = sels[static_cast<std::size_t>(primary)];
        if (drag_unit_ == DragUnit::character) {
            s.head = p;
        } else {
            const Range here = drag_unit_ == DragUnit::word
                ? word_at(p)
                : Range{{p.line, 0}, p.line + 1 < line_count() ? Pos{p.line + 1, 0} : Pos{p.line, line_len(p.line)}};
            if (p < drag_origin_.start) { s.anchor = drag_origin_.end; s.head = here.start; }
            else { s.anchor = drag_origin_.start; s.head = std::max(here.end, drag_origin_.end); }
        }
        normalize();
    }
    void select_all() { sels = {Selection{{0, 0}, doc_end()}}; primary = 0; }
    /// Escape: back to one caret.
    bool collapse() {
        if (sels.size() > 1) { const Selection s = primary_sel(); sels = {Selection{s.head, s.head}}; primary = 0; return true; }
        if (!sels[0].empty()) { sels[0].anchor = sels[0].head; return true; }
        return false;
    }

    // ── motion ──────────────────────────────────────────────────────────

    void move_left(bool extend)  { move_each([&](Selection& s) {
        if (!extend && !s.empty()) return s.start();
        return prev_pos(s.head); }, extend); }
    void move_right(bool extend) { move_each([&](Selection& s) {
        if (!extend && !s.empty()) return s.end();
        return next_pos(s.head); }, extend); }
    void move_word_left(bool extend)  { move_each([&](Selection& s) { return word_left(s.head); }, extend); }
    void move_word_right(bool extend) { move_each([&](Selection& s) { return word_right(s.head); }, extend); }
    void move_home(bool extend) {
        move_each([&](Selection& s) {
            const int first = indent_of(s.head.line);
            return Pos{s.head.line, s.head.col == first ? 0 : first};   // smart home
        }, extend);
    }
    void move_end(bool extend) { move_each([&](Selection& s) { return Pos{s.head.line, line_len(s.head.line)}; }, extend); }
    void move_doc_start(bool extend) { move_each([](Selection&) { return Pos{0, 0}; }, extend); }
    void move_doc_end(bool extend) { const Pos e = doc_end(); move_each([e](Selection&) { return e; }, extend); }
    void move_vertical(int lines_, bool extend) {
        for (auto& s : sels) {
            if (!extend && !s.empty() && lines_ != 0) {   // collapse toward the motion first
                const Pos edge = lines_ < 0 ? s.start() : s.end();
                s.head = s.anchor = edge;
            }
            if (s.goal < 0) s.goal = display_col(s.head.line, s.head.col);
            int l = s.head.line;
            int steps = std::abs(lines_);
            const int dir = lines_ < 0 ? -1 : 1;
            bool hit_edge = false;
            while (steps-- > 0) {
                int nl = l + dir;
                while (nl >= 0 && nl < line_count() && hidden(nl)) nl += dir;
                if (nl < 0 || nl >= line_count()) { hit_edge = true; break; }
                l = nl;
            }
            Pos p{l, byte_at_display_col(l, static_cast<float>(s.goal))};
            if (hit_edge && l == s.head.line) p = dir < 0 ? Pos{l, 0} : Pos{l, line_len(l)};
            s.head = p;
            if (!extend) s.anchor = p;
        }
        normalize();
    }
    void page(int dir, bool extend) {
        const int n = std::max(1, metrics.page_lines);
        move_vertical(dir * n, extend);
        scroll.offset.y = std::max(0.0f, scroll.offset.y + static_cast<float>(dir * n) * metrics.line_h);
    }

    /// Cmd-alt-up/down: a new caret on the line above/below the outermost one.
    void add_cursor_vertical(int dir) {
        const auto it = dir < 0 ? std::min_element(sels.begin(), sels.end(), [](auto& a, auto& b) { return a.head < b.head; })
                                : std::max_element(sels.begin(), sels.end(), [](auto& a, auto& b) { return a.head < b.head; });
        const Selection from = *it;
        const int goal = from.goal >= 0 ? from.goal : display_col(from.head.line, from.head.col);
        int l = from.head.line + dir;
        while (l >= 0 && l < line_count() && hidden(l)) l += dir;
        if (l < 0 || l >= line_count()) return;
        const Pos p{l, byte_at_display_col(l, static_cast<float>(goal))};
        Selection s{p, p, goal};
        sels.push_back(s);
        primary = static_cast<int>(sels.size()) - 1;
        normalize();
    }

    /// Cmd-d: select the word at the caret, then each next occurrence.
    void select_next_occurrence() {
        Selection& pr = sels[static_cast<std::size_t>(primary)];
        if (pr.empty()) {
            const Range r = word_at(pr.head);
            if (r.empty()) return;
            pr.anchor = r.start; pr.head = r.end;
            occurrence_whole_word_ = true;
            return;
        }
        const std::string needle = text_in(pr.range());
        if (needle.find('\n') != std::string::npos) return;
        const auto all = find_all(needle, FindOptions{true, occurrence_whole_word_, false});
        if (all.empty()) return;
        Pos from = std::max_element(sels.begin(), sels.end(), [](auto& a, auto& b) { return a.end() < b.end(); })->end();
        auto taken = [&](const Range& r) {
            return std::any_of(sels.begin(), sels.end(), [&](const Selection& s) { return s.range() == r; });
        };
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& r : all) {
                if ((pass == 0 && r.start < from) || taken(r)) continue;
                sels.push_back(Selection{r.start, r.end});
                primary = static_cast<int>(sels.size()) - 1;
                ensure_visible(r.start.line);
                normalize();
                return;
            }
            from = Pos{};
        }
    }
    /// Cmd-shift-l: every occurrence at once.
    void select_all_occurrences() {
        if (primary_sel().empty()) select_next_occurrence();
        const std::string needle = text_in(primary_sel().range());
        if (needle.empty() || needle.find('\n') != std::string::npos) return;
        const auto all = find_all(needle, FindOptions{true, occurrence_whole_word_, false});
        if (all.empty()) return;
        sels.clear();
        for (const auto& r : all) sels.push_back(Selection{r.start, r.end});
        primary = static_cast<int>(sels.size()) - 1;
        normalize();
    }
    /// Cmd-l: the whole line, then the next one.
    void select_line() {
        for (auto& s : sels) {
            const Pos st{s.start().line, 0};
            Pos en = s.end();
            if (en.col > 0 || en == st || !s.empty()) {
                en = en.line + 1 < line_count() ? Pos{en.line + 1, 0} : Pos{en.line, line_len(en.line)};
                if (s.empty()) en = s.head.line + 1 < line_count() ? Pos{s.head.line + 1, 0} : Pos{s.head.line, line_len(s.head.line)};
            }
            s.anchor = st; s.head = en; s.goal = -1;
        }
        normalize();
    }

    // ── editing ─────────────────────────────────────────────────────────

    /// Typing (or a paste of one line). Auto-closes pairs, types over
    /// closers, wraps a selection in brackets/quotes.
    void insert_text(std::string_view s) {
        if (s.empty()) return;
        std::string clean;
        for (char c : s) if (c == '\n' || c == '\t' || static_cast<unsigned char>(c) >= 0x20) clean += c;
        if (clean.empty()) return;
        if (clean.find('\n') != std::string::npos) { paste(clean); return; }
        const bool single = clean.size() == 1;
        const char ch = clean[0];
        const bool word_boundary = single && (detail::is_space(ch) || !detail::is_word(static_cast<unsigned char>(ch)));
        replace_each([&](const Selection& sel) -> Change {
            if (single && auto_pairs) {
                const char next = char_at(sel.head), prev = char_before(sel.head);
                // type over the closer we inserted
                if (sel.empty() && (detail::is_closer(ch) || ch == '"' || ch == '\'' || ch == '`') && next == ch)
                    return Change{{sel.head, {sel.head.line, sel.head.col + 1}}, std::string(1, ch), 1};
                const char close = detail::closer_of(ch);
                const bool quote = ch == '"' || ch == '\'' || ch == '`';
                if (close && !sel.empty() && sel.start().line == sel.end().line) {
                    const std::string inner = text_in(sel.range());
                    return Change{sel.range(), std::string(1, ch) + inner + close, 1 + inner.size(), 1};
                }
                if (close && sel.empty()) {
                    const bool next_ok = next == '\0' || detail::is_space(next) || detail::is_closer(next) ||
                                         next == ',' || next == ';' || next == ':' || (quote ? false : next == '.');
                    const bool prev_ok = !quote || (!detail::is_word(static_cast<unsigned char>(prev)) && prev != ch);
                    if (next_ok && prev_ok) return Change{sel.range(), std::string{ch, close}, 1};
                }
            }
            return Change{sel.range(), clean, clean.size()};
        }, word_boundary ? "" : "type");
        if (word_boundary) break_group();
        reveal();
    }
    /// Enter: keep the indentation, indent after an opener, and split a pair
    /// `{|}` onto three lines with the caret in the middle.
    void newline() {
        replace_each([&](const Selection& sel) -> Change {
            const int l = sel.start().line;
            const std::string ind = line(l).substr(0, static_cast<std::size_t>(std::min(indent_of(l), sel.start().col)));
            const char prev = char_before(sel.start());
            const char next = char_at(sel.end());
            const bool opens = prev == '{' || prev == '[' || prev == '(' || (prev == ':' && language->name == "Python");
            if (opens) {
                const std::string inner = "\n" + ind + indent_unit();
                if (next == detail::closer_of(prev) && next != 0)
                    return Change{sel.range(), inner + "\n" + ind, inner.size()};
                return Change{sel.range(), inner, inner.size()};
            }
            const std::string t = "\n" + ind;
            return Change{sel.range(), t, t.size()};
        });
        break_group();
        reveal();
    }
    /// Cmd-enter / cmd-shift-enter: open a line below / above.
    void open_line(bool above) {
        for (auto& s : sels) {
            const int l = s.head.line;
            const Pos p = above ? Pos{l, 0} : Pos{l, line_len(l)};
            s.anchor = s.head = p;
        }
        normalize();
        replace_each([&](const Selection& sel) -> Change {
            const std::string ind = line(sel.head.line).substr(0, static_cast<std::size_t>(indent_of(sel.head.line)));
            return above ? Change{sel.range(), ind + "\n", ind.size()} : Change{sel.range(), "\n" + ind, ind.size() + 1};
        });
        break_group();
        reveal();
    }
    void backspace() {
        replace_each([&](const Selection& sel) -> Change {
            if (!sel.empty()) return Change{sel.range(), {}, 0};
            const Pos h = sel.head;
            if (h.col == 0) {
                if (h.line == 0) return Change{sel.range(), {}, 0};
                return Change{{{h.line - 1, line_len(h.line - 1)}, h}, {}, 0};
            }
            // between an auto-inserted pair: delete both
            const char prev = char_before(h), next = char_at(h);
            if (auto_pairs && detail::closer_of(prev) != 0 && next == detail::closer_of(prev))
                return Change{{{h.line, h.col - 1}, {h.line, h.col + 1}}, {}, 0};
            // in leading spaces: back to the previous tab stop
            if (soft_tabs && h.col <= indent_of(h.line) && prev == ' ') {
                const int dc = display_col(h.line, h.col);
                const int target = ((dc - 1) / tab_width) * tab_width;
                int c = h.col;
                while (c > 0 && display_col(h.line, c) > target && char_before({h.line, c}) == ' ') --c;
                return Change{{{h.line, c}, h}, {}, 0};
            }
            return Change{{prev_pos(h), h}, {}, 0};
        }, "delete");
        reveal();
    }
    void delete_forward() {
        replace_each([&](const Selection& sel) -> Change {
            if (!sel.empty()) return Change{sel.range(), {}, 0};
            return Change{{sel.head, next_pos(sel.head)}, {}, 0};
        }, "delete");
        reveal();
    }
    void delete_word(bool forward) {
        replace_each([&](const Selection& sel) -> Change {
            if (!sel.empty()) return Change{sel.range(), {}, 0};
            return forward ? Change{{sel.head, word_right(sel.head)}, {}, 0}
                           : Change{{word_left(sel.head), sel.head}, {}, 0};
        });
        break_group();
        reveal();
    }
    void delete_to_line_start() {
        replace_each([&](const Selection& sel) -> Change {
            if (!sel.empty()) return Change{sel.range(), {}, 0};
            const Pos h = sel.head;
            const int first = indent_of(h.line);
            return Change{{{h.line, h.col > first ? first : 0}, h}, {}, 0};
        });
        break_group();
        reveal();
    }
    /// Tab: indent selected lines, or insert to the next tab stop.
    void tab() {
        const bool multi_line = std::any_of(sels.begin(), sels.end(), [](const Selection& s) {
            return s.start().line != s.end().line;
        });
        if (multi_line) { indent(); return; }
        replace_each([&](const Selection& sel) -> Change {
            if (!soft_tabs) return Change{sel.range(), "\t", 1};
            const int dc = display_col(sel.start().line, sel.start().col);
            const std::string sp(static_cast<std::size_t>(tab_width - dc % tab_width), ' ');
            return Change{sel.range(), sp, sp.size()};
        });
        break_group();
        reveal();
    }
    void indent() {
        std::vector<Change> ch;
        for (int l : selected_lines()) if (!blank(l)) ch.push_back(Change{{{l, 0}, {l, 0}}, indent_unit(), 0});
        apply_and_transform(std::move(ch), "");
    }
    void outdent() {
        std::vector<Change> ch;
        for (int l : selected_lines()) {
            const std::string& s = line(l);
            int n = 0;
            if (!s.empty() && s[0] == '\t') n = 1;
            else while (n < tab_width && n < static_cast<int>(s.size()) && s[static_cast<std::size_t>(n)] == ' ') ++n;
            if (n > 0) ch.push_back(Change{{{l, 0}, {l, n}}, {}, 0});
        }
        apply_and_transform(std::move(ch), "");
    }
    /// Cmd-/: comment or uncomment the selected lines, at the shallowest indent.
    void toggle_comment() {
        const auto ls = selected_lines();
        const std::string tok = line_comment + " ";
        bool all = true;
        int min_ind = 1 << 30;
        for (int l : ls) {
            if (blank(l)) continue;
            min_ind = std::min(min_ind, indent_of(l));
            if (line(l).compare(static_cast<std::size_t>(indent_of(l)), line_comment.size(), line_comment) != 0) all = false;
        }
        if (min_ind == (1 << 30)) return;
        std::vector<Change> ch;
        for (int l : ls) {
            if (blank(l)) continue;
            const int at = indent_of(l);
            if (all) {
                const bool sp = line(l).compare(static_cast<std::size_t>(at), tok.size(), tok) == 0;
                ch.push_back(Change{{{l, at}, {l, at + static_cast<int>(sp ? tok.size() : line_comment.size())}}, {}, 0});
            } else {
                ch.push_back(Change{{{l, min_ind}, {l, min_ind}}, tok, 0});
            }
        }
        apply_and_transform(std::move(ch), "");
    }
    /// Cmd-shift-d / alt-shift-down: duplicate the selected lines below.
    void duplicate_lines() {
        std::vector<Change> ch;
        for (const auto& [a, b] : line_blocks()) {
            std::string block;
            for (int l = a; l <= b; ++l) { block += '\n'; block += line(l); }
            ch.push_back(Change{{{b, line_len(b)}, {b, line_len(b)}}, block, 0});
        }
        apply_and_transform(std::move(ch), "");
        reveal();
    }
    /// Alt-up/down: move the selected lines.
    void move_lines(int dir) {
        const auto blocks = line_blocks();
        if (blocks.empty()) return;
        if (dir < 0 && blocks.front().first == 0) return;
        if (dir > 0 && blocks.back().second == line_count() - 1) return;
        std::vector<Change> ch;
        for (const auto& [a, b] : blocks) {
            std::string block;
            for (int l = a; l <= b; ++l) { if (l > a) block += '\n'; block += line(l); }
            if (dir < 0) {
                const std::string above = line(a - 1);
                ch.push_back(Change{{{a - 1, 0}, {b, line_len(b)}}, block + "\n" + above, 0});
            } else {
                const std::string below = line(b + 1);
                ch.push_back(Change{{{a, 0}, {b + 1, line_len(b + 1)}}, below + "\n" + block, 0});
            }
        }
        auto saved = sels;
        apply_changes(ch, "");
        for (auto& s : saved) { s.anchor.line += dir; s.head.line += dir; }
        sels = std::move(saved);
        undo_.back().after = sels;
        normalize();
        reveal();
    }
    /// Cmd-shift-k: delete the selected lines.
    void delete_lines() {
        std::vector<Change> ch;
        for (const auto& [a, b] : line_blocks()) {
            if (b + 1 < line_count()) ch.push_back(Change{{{a, 0}, {b + 1, 0}}, {}, 0});
            else if (a > 0) ch.push_back(Change{{{a - 1, line_len(a - 1)}, {b, line_len(b)}}, {}, 0});
            else ch.push_back(Change{{{a, 0}, {b, line_len(b)}}, {}, 0});
        }
        apply_and_transform(std::move(ch), "");
        for (auto& s : sels) { s.head = clamp(s.head); s.anchor = s.head; }
        normalize();
        reveal();
    }
    /// Ctrl-j: join the next line onto this one with a single space.
    void join_lines() {
        std::vector<Change> ch;
        for (const auto& [a, b] : line_blocks()) {
            const int last = std::max(b, a + (a == b ? 1 : 0));
            for (int l = a; l < last && l + 1 < line_count(); ++l) {
                ch.push_back(Change{{{l, line_len(l)}, {l + 1, indent_of(l + 1)}}, blank(l + 1) ? "" : " ", 0});
            }
        }
        apply_and_transform(std::move(ch), "");
    }

    // ── clipboard ───────────────────────────────────────────────────────

    /// The selected text; with no selection, the whole line (and paste then
    /// inserts it as a line, the convention every editor follows).
    [[nodiscard]] std::string copy() const {
        std::string out;
        bool first = true;
        const bool single = sels.size() == 1;
        for (const auto& s : sorted()) {
            if (!first) out += '\n';
            first = false;
            out += s.empty() ? line(s.head.line) + (single ? "\n" : "") : text_in(s.range());
        }
        if (sels.size() == 1) whole_line_clip_ = sels[0].empty();
        return out;
    }
    std::string cut() {
        const std::string clip = copy();
        if (std::all_of(sels.begin(), sels.end(), [](const Selection& s) { return s.empty(); })) {
            delete_lines();
        } else {
            replace_each([](const Selection& s) -> Change { return Change{s.range(), {}, 0}; });
            break_group();
        }
        return clip;
    }
    void paste(std::string_view text) {
        std::string t{text};
        std::erase(t, '\r');
        // One clipboard line per caret: distribute.
        std::vector<std::string> parts;
        {
            std::size_t st = 0;
            for (std::size_t i = 0; i <= t.size(); ++i)
                if (i == t.size() || t[i] == '\n') { parts.emplace_back(t.substr(st, i - st)); st = i + 1; }
        }
        if (parts.size() > 1 && parts.back().empty()) parts.pop_back();
        const bool distribute = sels.size() > 1 && parts.size() == sels.size();
        const bool as_line = whole_line_clip_ && sels.size() == 1 && sels[0].empty() && !t.empty() && t.back() == '\n';
        const int col0 = sels[0].head.col;
        std::size_t k = 0;
        replace_each([&](const Selection& s) -> Change {
            if (as_line) {
                const Pos at{s.head.line, 0};
                return Change{{at, at}, t, t.size()};
            }
            const std::string piece = distribute ? parts[k++] : t;
            return Change{s.range(), piece, piece.size()};
        });
        if (as_line) {   // the caret keeps its column on the line that moved down
            sels[0].head = clamp({sels[0].head.line, col0});
            sels[0].anchor = sels[0].head;
            undo_.back().after = sels;
        }
        break_group();
        reveal();
    }

    // ── undo ────────────────────────────────────────────────────────────

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    bool undo() {
        if (undo_.empty()) return false;
        Transaction tx = std::move(undo_.back());
        undo_.pop_back();
        for (auto it = tx.edits.rbegin(); it != tx.edits.rend(); ++it)
            replace_raw(it->at, end_of(it->at, it->inserted), it->removed);
        sels = tx.before;
        primary = std::min(primary, static_cast<int>(sels.size()) - 1);
        redo_.push_back(std::move(tx));
        last_group_.clear();
        reveal();
        return true;
    }
    bool redo() {
        if (redo_.empty()) return false;
        Transaction tx = std::move(redo_.back());
        redo_.pop_back();
        for (const auto& e : tx.edits) replace_raw(e.at, end_of(e.at, e.removed), e.inserted);
        sels = tx.after;
        primary = std::min(primary, static_cast<int>(sels.size()) - 1);
        undo_.push_back(std::move(tx));
        last_group_.clear();
        reveal();
        return true;
    }
    /// The next edit starts a new undo step even if it would coalesce.
    void break_group() noexcept { last_group_.clear(); }

    [[nodiscard]] bool dirty() const noexcept { return top_id() != saved_id_; }
    void mark_saved() noexcept { saved_id_ = top_id(); }
    /// The state a save writes: hand it back to mark_saved(point) once the
    /// write has landed. Edits made while the write was in flight stay dirty
    /// -- the file on disk is the state at `point`, not the one now.
    [[nodiscard]] std::uint64_t save_point() const noexcept { return top_id(); }
    void mark_saved(std::uint64_t point) noexcept { saved_id_ = point; }

    // ── find ────────────────────────────────────────────────────────────

    /// Every match of `query`, in document order. Single-line matches.
    [[nodiscard]] std::vector<Range> find_all(std::string_view query, FindOptions o = {}) const {
        std::vector<Range> out;
        if (query.empty()) return out;
        if (o.regex) {
            try {
                auto flags = std::regex::ECMAScript;
                if (!o.case_sensitive) flags |= std::regex::icase;
                const std::regex re{std::string{query}, flags};
                for (int l = 0; l < line_count(); ++l) {
                    const std::string& s = line(l);
                    for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) {
                        if (it->length(0) == 0) continue;
                        const int a = static_cast<int>(it->position(0));
                        const Range r{{l, a}, {l, a + static_cast<int>(it->length(0))}};
                        if (!o.whole_word || whole_word_at(r)) out.push_back(r);
                    }
                }
            } catch (const std::regex_error&) { /* an incomplete pattern matches nothing */ }
            return out;
        }
        const auto fold = [&](char c) {
            return (!o.case_sensitive && c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        std::string q{query};
        for (auto& c : q) c = fold(c);
        for (int l = 0; l < line_count(); ++l) {
            std::string s = line(l);
            if (!o.case_sensitive) for (auto& c : s) c = fold(c);
            for (std::size_t at = s.find(q); at != std::string::npos; at = s.find(q, at + q.size())) {
                const Range r{{l, static_cast<int>(at)}, {l, static_cast<int>(at + q.size())}};
                if (!o.whole_word || whole_word_at(r)) out.push_back(r);
            }
        }
        return out;
    }
    /// Select a match (find next / previous): one caret, revealed, unfolded.
    void select_range(Range r) {
        sels = {Selection{r.start, r.end}};
        primary = 0;
        ensure_visible(r.start.line);
        reveal();
    }
    /// Replace the given ranges (all at once: one undo step). `with` may use
    /// $1.. when `regex_query` is given.
    int replace_ranges(const std::vector<Range>& ranges, std::string_view with,
                       std::string_view regex_query = {}, bool case_sensitive = false) {
        std::vector<Change> ch;
        for (const auto& r : ranges) {
            std::string rep{with};
            if (!regex_query.empty()) {
                try {
                    auto flags = std::regex::ECMAScript;
                    if (!case_sensitive) flags |= std::regex::icase;
                    rep = std::regex_replace(text_in(r), std::regex{std::string{regex_query}, flags}, std::string{with},
                                             std::regex_constants::format_first_only);
                } catch (const std::regex_error&) {}
            }
            ch.push_back(Change{r, rep, 0});
        }
        apply_and_transform(std::move(ch), "");
        return static_cast<int>(ranges.size());
    }

    // ── folding ─────────────────────────────────────────────────────────

    /// The last line of the block starting at `l`, by indentation; nullopt if
    /// nothing is nested under it.
    [[nodiscard]] std::optional<int> fold_end(int l) const {
        if (l < 0 || l >= line_count() || blank(l)) return std::nullopt;
        const int base = display_col(l, indent_of(l));
        int last = -1;
        for (int j = l + 1; j < line_count(); ++j) {
            if (blank(j)) continue;
            if (display_col(j, indent_of(j)) <= base) break;
            last = j;
        }
        if (last < 0) return std::nullopt;
        return last;
    }
    /// O(1)-ish: only looks at the next non-blank line (the gutter asks this
    /// for every visible row, every frame).
    [[nodiscard]] bool foldable(int l) const {
        if (l < 0 || l >= line_count() || blank(l)) return false;
        for (int j = l + 1; j < line_count(); ++j)
            if (!blank(j)) return display_col(j, indent_of(j)) > display_col(l, indent_of(l));
        return false;
    }
    void toggle_fold(int l) {
        if (folds.erase(l) == 0 && foldable(l)) {
            folds.insert(l);
            // Carets inside the hidden block move to its header.
            const int e = *fold_end(l);
            for (auto& s : sels) if (s.head.line > l && s.head.line <= e) { s.head = s.anchor = {l, line_len(l)}; }
            normalize();
        }
        ++fold_version_;
    }
    void fold_at_cursor() {
        int l = cursor().line;
        while (l >= 0 && !(foldable(l) && *fold_end(l) >= cursor().line)) --l;
        if (l >= 0 && !folds.contains(l)) toggle_fold(l);
    }
    void unfold_at_cursor() {
        const int l = cursor().line;
        if (folds.contains(l)) toggle_fold(l);
    }
    void unfold_all() { folds.clear(); ++fold_version_; }
    [[nodiscard]] bool hidden(int l) const {
        for (int f : folds) {
            if (f >= l) break;
            if (const auto e = fold_end(f); e && l <= *e) return true;
        }
        return false;
    }
    /// Make line `l` visible by opening any fold that hides it.
    void ensure_visible(int l) {
        bool changed = false;
        for (auto it = folds.begin(); it != folds.end();) {
            const auto e = fold_end(*it);
            if (*it < l && e && l <= *e) { it = folds.erase(it); changed = true; }
            else ++it;
        }
        if (changed) ++fold_version_;
    }
    /// Buffer lines in display order (folded blocks skipped). Cached.
    [[nodiscard]] const std::vector<int>& visual_lines() const {
        if (visual_version_ != version_ || visual_fold_version_ != fold_version_ || visual_.empty()) {
            visual_.clear();
            visual_.reserve(lines.size());
            for (int l = 0; l < line_count(); ++l) {
                visual_.push_back(l);
                if (folds.contains(l)) if (const auto e = fold_end(l)) l = *e;
            }
            visual_version_ = version_;
            visual_fold_version_ = fold_version_;
        }
        return visual_;
    }
    [[nodiscard]] int visual_row_of(int l) const {
        const auto& v = visual_lines();
        const auto it = std::upper_bound(v.begin(), v.end(), l);
        return static_cast<int>(it - v.begin()) - 1;
    }

    // ── brackets ────────────────────────────────────────────────────────

    /// The bracket matching the one at or just before `p`.
    [[nodiscard]] std::optional<std::pair<Pos, Pos>> match_bracket(Pos p) const {
        const auto try_at = [&](Pos at) -> std::optional<std::pair<Pos, Pos>> {
            const char c = char_at(at);
            constexpr std::string_view open = "([{", close = ")]}";
            const auto oi = open.find(c), ci = close.find(c);
            if (oi == std::string_view::npos && ci == std::string_view::npos) return std::nullopt;
            const bool fwd = oi != std::string_view::npos;
            const char me = c, other = fwd ? close[oi] : open[ci];
            int depth = 0, budget = 200000;
            Pos q = at;
            while (budget-- > 0) {
                const char d = char_at(q);
                if (d == me) ++depth;
                else if (d == other && --depth == 0) return std::pair{at, q};
                if (fwd) { if (q == doc_end()) break; q = next_pos(q); }
                else { if (q == Pos{}) break; q = prev_pos(q); }
            }
            return std::nullopt;
        };
        if (auto r = try_at(p)) return r;
        if (p.col > 0) return try_at({p.line, p.col - 1});
        return std::nullopt;
    }

    // ── keymap ──────────────────────────────────────────────────────────

    /// The standard editing keymap (macOS: alt=word, cmd=line/document;
    /// elsewhere: ctrl=word, home/end). Returns true when the key was used.
    /// Clipboard keys are NOT handled here: the app owns the clipboard.
    bool handle_key(Key k, Mods m) {
        const bool sh = m.shift;
#if defined(__APPLE__)
        const bool word = m.alt && !m.super && !m.ctrl;
        const bool linewise = m.super;
#else
        const bool word = m.ctrl && !m.alt;
        const bool linewise = false;
#endif
        const bool cmd = m.primary();
        switch (k) {
            case Key::left:
                if (linewise) move_home(sh); else if (word) move_word_left(sh); else if (m.none() || m == Mods{.shift = true}) move_left(sh); else return false;
                break;
            case Key::right:
                if (linewise) move_end(sh); else if (word) move_word_right(sh); else if (m.none() || m == Mods{.shift = true}) move_right(sh); else return false;
                break;
            case Key::up:
                if (cmd && m.alt) add_cursor_vertical(-1);
                else if (m.alt && sh && !cmd) { duplicate_lines(); }
                else if (m.alt && !cmd) move_lines(-1);
                else if (linewise) move_doc_start(sh);
                else move_vertical(-1, sh);
                break;
            case Key::down:
                if (cmd && m.alt) add_cursor_vertical(+1);
                else if (m.alt && sh && !cmd) { duplicate_lines(); move_vertical(1, false); }
                else if (m.alt && !cmd) move_lines(+1);
                else if (linewise) move_doc_end(sh);
                else move_vertical(+1, sh);
                break;
            case Key::home: if (cmd) move_doc_start(sh); else move_home(sh); break;
            case Key::end:  if (cmd) move_doc_end(sh); else move_end(sh); break;
            case Key::page_up:   page(-1, sh); break;
            case Key::page_down: page(+1, sh); break;
            case Key::backspace:
                if (linewise) delete_to_line_start(); else if (word) delete_word(false); else backspace();
                break;
            case Key::del:
                if (word) delete_word(true); else delete_forward();
                break;
            case Key::enter:
                if (cmd) open_line(sh); else newline();
                break;
            case Key::tab:
                if (cmd || m.ctrl) return false;
                if (sh) outdent(); else tab();
                break;
            case Key::escape: return collapse();
            case Key::a: if (!cmd || sh) return false; select_all(); break;
            case Key::d: if (!cmd) return false; if (sh) duplicate_lines(); else select_next_occurrence(); break;
            case Key::l: if (!cmd) return false; if (sh) select_all_occurrences(); else select_line(); break;
            case Key::k: if (!(cmd && sh)) return false; delete_lines(); break;
            case Key::j: if (!(m.ctrl && !m.alt && !m.super && !sh)) return false; join_lines(); break;
            case Key::slash: if (!cmd) return false; toggle_comment(); break;
            case Key::bracket_right:
                if (cmd && m.alt) unfold_at_cursor();
                else if (cmd && !sh) indent();
                else return false;
                break;
            case Key::bracket_left:
                if (cmd && m.alt) fold_at_cursor();
                else if (cmd && !sh) outdent();
                else return false;
                break;
            case Key::z: if (!cmd) return false; if (sh) redo(); else undo(); break;
            case Key::y: if (!(m.ctrl && !m.super && !m.alt && !sh)) return false; redo(); break;
            default: return false;
        }
        reveal();
        return true;
    }

    /// Scroll so the primary caret is on screen with a 3-line margin.
    void reveal() {
        const ViewMetrics& mt = metrics;
        const float vp_h = scroll.viewport.y, vp_w = scroll.viewport.x;
        if (vp_h <= 0.0f) return;
        const Pos h = cursor();
        const float y = static_cast<float>(visual_row_of(h.line)) * mt.line_h;
        const float margin = std::min(3.0f * mt.line_h, std::max(0.0f, (vp_h - mt.line_h) * 0.5f));
        if (y - margin < scroll.offset.y) scroll.offset.y = std::max(0.0f, y - margin);
        else if (y + mt.line_h + margin > scroll.offset.y + vp_h) scroll.offset.y = y + mt.line_h + margin - vp_h;
        const float text_w = vp_w - mt.gutter_w;
        if (text_w > mt.cell_w * 4.0f) {
            const float x = static_cast<float>(display_col(h.line, h.col)) * mt.cell_w;
            const float hm = mt.cell_w * 4.0f;
            if (x - hm < scroll.offset.x) scroll.offset.x = std::max(0.0f, x - hm);
            else if (x + hm > scroll.offset.x + text_w) scroll.offset.x = x + hm - text_w;
        }
    }

    // ── low level ───────────────────────────────────────────────────────

    /// A replacement of `r` with `text`. After it, the selection becomes
    /// [anchor_in, caret_in) as byte offsets into `text`.
    struct Change {
        Range       r;
        std::string text;
        std::size_t caret_in = 0;
        std::size_t anchor_in = static_cast<std::size_t>(-1);   ///< -1: same as caret
    };

  private:
    struct Edit { Pos at; std::string removed, inserted; };
    struct Transaction {
        std::vector<Edit>      edits;
        std::vector<Selection> before, after;
        std::string            group;
        std::uint64_t          id = 0;
    };

    std::vector<Transaction> undo_, redo_;
    std::string              last_group_;
    std::uint64_t            next_id_ = 1;
    std::uint64_t            saved_id_ = 0;
    std::uint64_t            version_ = 1;
    std::uint64_t            fold_version_ = 0;
    bool                     occurrence_whole_word_ = false;
    mutable bool             whole_line_clip_ = false;
    mutable int              max_cols_ = 0;
    mutable std::uint64_t    max_cols_version_ = 0;
    mutable std::vector<int> visual_;
    mutable std::uint64_t    visual_version_ = 0, visual_fold_version_ = 0;
    mutable std::vector<Symbol> symbols_;
    mutable std::uint64_t    symbols_version_ = ~std::uint64_t{0};
    mutable const Language*  symbols_lang_ = nullptr;
    enum class DragUnit : std::uint8_t { character, word, line };
    DragUnit drag_unit_ = DragUnit::character;
    Range    drag_origin_{};
    mutable int              symbols_tab_ = 0;
    mutable const std::string* symbols_lines_ = nullptr;
    mutable std::size_t      symbols_count_ = 0;

    [[nodiscard]] std::uint64_t top_id() const noexcept { return undo_.empty() ? 0 : undo_.back().id; }

    [[nodiscard]] Pos next_pos(Pos p) const {
        if (p.col >= line_len(p.line)) return p.line + 1 < line_count() ? Pos{p.line + 1, 0} : p;
        int c = p.col + 1;
        while (c < line_len(p.line) && detail::is_cont(static_cast<unsigned char>(line(p.line)[static_cast<std::size_t>(c)]))) ++c;
        return {p.line, c};
    }
    [[nodiscard]] Pos prev_pos(Pos p) const {
        if (p.col == 0) return p.line > 0 ? Pos{p.line - 1, line_len(p.line - 1)} : p;
        int c = p.col - 1;
        while (c > 0 && detail::is_cont(static_cast<unsigned char>(line(p.line)[static_cast<std::size_t>(c)]))) --c;
        return {p.line, c};
    }
    [[nodiscard]] Pos word_right(Pos p) const {
        const std::string& s = line(p.line);
        if (p.col >= static_cast<int>(s.size())) return next_pos(p);
        int c = p.col;
        while (c < static_cast<int>(s.size()) && detail::is_space(s[static_cast<std::size_t>(c)])) ++c;
        if (c < static_cast<int>(s.size())) {
            const int cls = detail::char_class(s[static_cast<std::size_t>(c)]);
            while (c < static_cast<int>(s.size()) && detail::char_class(s[static_cast<std::size_t>(c)]) == cls) ++c;
        }
        return {p.line, c};
    }
    [[nodiscard]] Pos word_left(Pos p) const {
        const std::string& s = line(p.line);
        if (p.col == 0) return prev_pos(p);
        int c = p.col;
        while (c > 0 && detail::is_space(s[static_cast<std::size_t>(c - 1)])) --c;
        if (c > 0) {
            const int cls = detail::char_class(s[static_cast<std::size_t>(c - 1)]);
            while (c > 0 && detail::char_class(s[static_cast<std::size_t>(c - 1)]) == cls) --c;
        }
        return {p.line, c};
    }
    [[nodiscard]] bool whole_word_at(Range r) const {
        const char b = char_before(r.start), a = char_at(r.end);
        return !detail::is_word(static_cast<unsigned char>(b)) && !detail::is_word(static_cast<unsigned char>(a));
    }

    template <class F>
    void move_each(F&& f, bool extend) {
        for (auto& s : sels) {
            const Pos p = clamp(f(s));
            s.head = p;
            if (!extend) s.anchor = p;
            s.goal = -1;
        }
        occurrence_whole_word_ = false;
        normalize();
        break_group();
    }

    [[nodiscard]] std::vector<Selection> sorted() const {
        auto v = sels;
        std::sort(v.begin(), v.end(), [](const Selection& a, const Selection& b) { return a.start() < b.start(); });
        return v;
    }
    /// Lines touched by any selection, ascending, unique. A selection ending
    /// at column 0 does not include that last line.
    [[nodiscard]] std::vector<int> selected_lines() const {
        std::set<int> ls;
        for (const auto& s : sels) {
            int last = s.end().line;
            if (!s.empty() && s.end().col == 0 && last > s.start().line) --last;
            for (int l = s.start().line; l <= last; ++l) ls.insert(l);
        }
        return {ls.begin(), ls.end()};
    }
    /// Contiguous runs of selected lines.
    [[nodiscard]] std::vector<std::pair<int, int>> line_blocks() const {
        std::vector<std::pair<int, int>> out;
        for (int l : selected_lines()) {
            if (!out.empty() && out.back().second + 1 >= l) out.back().second = l;
            else out.emplace_back(l, l);
        }
        return out;
    }

    /// Sort, then merge selections that overlap (or carets that coincide).
    void normalize() {
        if (sels.empty()) { sels = {Selection{}}; primary = 0; return; }
        const Pos ph = sels[static_cast<std::size_t>(std::clamp(primary, 0, static_cast<int>(sels.size()) - 1))].head;
        for (auto& s : sels) { s.anchor = clamp(s.anchor); s.head = clamp(s.head); }
        std::stable_sort(sels.begin(), sels.end(), [](const Selection& a, const Selection& b) { return a.start() < b.start(); });
        std::vector<Selection> out;
        for (const auto& s : sels) {
            if (!out.empty()) {
                Selection& p = out.back();
                const bool overlap = s.start() < p.end() ||
                                     (s.start() == p.end() && !s.empty() && !p.empty()) ||
                                     (s.empty() && p.empty() && s.head == p.head);
                if (overlap) {
                    const Pos st = std::min(p.start(), s.start()), en = std::max(p.end(), s.end());
                    const bool rev = p.head < p.anchor;
                    p.anchor = rev ? en : st;
                    p.head = rev ? st : en;
                    continue;
                }
            }
            out.push_back(s);
        }
        sels = std::move(out);
        primary = 0;
        for (std::size_t i = 0; i < sels.size(); ++i)
            if (sels[i].start() <= ph && ph <= sels[i].end()) { primary = static_cast<int>(i); break; }
    }

    [[nodiscard]] static Pos end_of(Pos at, std::string_view text) noexcept {
        Pos e = at;
        for (char c : text) { if (c == '\n') { ++e.line; e.col = 0; } else ++e.col; }
        return e;
    }
    [[nodiscard]] static Pos offset_pos(Pos at, std::string_view text, std::size_t k) noexcept {
        return end_of(at, text.substr(0, std::min(k, text.size())));
    }

    /// The primitive every edit goes through: replace [a, b) with `ins`.
    Pos replace_raw(Pos a, Pos b, std::string_view ins) {
        const std::size_t ac = static_cast<std::size_t>(a.col), bc = static_cast<std::size_t>(b.col);
        std::string head = lines[static_cast<std::size_t>(a.line)].substr(0, ac);
        std::string tail = lines[static_cast<std::size_t>(b.line)].substr(bc);
        std::vector<std::string> parts;
        std::size_t st = 0;
        for (std::size_t i = 0; i <= ins.size(); ++i)
            if (i == ins.size() || ins[i] == '\n') { parts.emplace_back(ins.substr(st, i - st)); st = i + 1; }
        const Pos e = end_of(a, ins);
        parts.front().insert(0, head);
        parts.back() += tail;
        const int removed_lines = b.line - a.line;
        const int delta = static_cast<int>(parts.size()) - 1 - removed_lines;
        lines.erase(lines.begin() + a.line, lines.begin() + b.line + 1);
        lines.insert(lines.begin() + a.line, std::make_move_iterator(parts.begin()), std::make_move_iterator(parts.end()));
        // Folds below the edit shift with their lines; folds it touched open.
        if (!folds.empty()) {
            std::set<int> moved;
            for (int f : folds) {
                if (f < a.line || (f == a.line && a.col > 0)) moved.insert(f);   // edit after the header starts
                else if (f > b.line || (f == a.line && a.col == 0 && a == b)) moved.insert(f + delta);
            }
            if (moved != folds) { folds = std::move(moved); ++fold_version_; }
        }
        syntax.invalidate_from(a.line);
        ++version_;
        return e;
    }

    /// Apply changes (sorted, non-overlapping) as one transaction; returns
    /// the inserted range of each, in final coordinates.
    std::vector<Range> apply_changes(std::vector<Change>& ch, std::string_view group) {
        std::sort(ch.begin(), ch.end(), [](const Change& x, const Change& y) { return x.r.start < y.r.start; });
        const bool coalesce = !group.empty() && group == last_group_ && !undo_.empty() && redo_.empty();
        if (!coalesce) {
            Transaction tx;
            tx.before = sels;
            tx.group = std::string{group};
            tx.id = next_id_++;
            undo_.push_back(std::move(tx));
            if (undo_.size() > 2000) undo_.erase(undo_.begin());
        }
        redo_.clear();
        last_group_ = std::string{group};
        Transaction& tx = undo_.back();
        std::vector<std::tuple<Pos, Pos, Pos>> done;
        std::vector<Range> out;
        for (auto& c : ch) {
            Pos a = c.r.start, b = c.r.end;
            for (const auto& [da, db, de] : done) { a = detail::transform(a, da, db, de); b = detail::transform(b, da, db, de); }
            const std::string removed = text_in({a, b});
            if (removed.empty() && c.text.empty()) { out.push_back({a, a}); continue; }
            const Pos e = replace_raw(a, b, c.text);
            tx.edits.push_back(Edit{a, removed, c.text});
            done.emplace_back(a, b, e);
            // The ranges already recorded shift with later edits too.
            for (auto& r : out) { r.start = detail::transform(r.start, a, b, e); r.end = detail::transform(r.end, a, b, e); }
            out.push_back({a, e});
        }
        last_done_ = std::move(done);
        return out;
    }
    std::vector<std::tuple<Pos, Pos, Pos>> last_done_;

    /// Changes that are not per-selection (indent, comment...): selections
    /// follow the text through the edits.
    void apply_and_transform(std::vector<Change> ch, std::string_view group) {
        if (ch.empty()) return;
        auto saved = sels;
        apply_changes(ch, group);
        for (auto& s : saved)
            for (const auto& [a, b, e] : last_done_) {
                s.anchor = detail::transform(s.anchor, a, b, e);
                s.head = detail::transform(s.head, a, b, e);
            }
        sels = std::move(saved);
        normalize();
        undo_.back().after = sels;
    }

    /// One change per selection; the new selections come from caret_in /
    /// anchor_in of each change.
    template <class F>
    void replace_each(F&& make, std::string_view group = {}) {
        auto ordered = sorted();
        std::vector<Change> ch;
        ch.reserve(ordered.size());
        for (const auto& s : ordered) ch.push_back(make(s));
        // apply_changes sorts by range start; selections are already in that order.
        const auto ranges = apply_changes(ch, group);
        std::vector<Selection> next;
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            const Change& c = ch[i];
            const Pos caret = offset_pos(ranges[i].start, c.text, c.caret_in);
            const Pos anchor = c.anchor_in == static_cast<std::size_t>(-1) ? caret : offset_pos(ranges[i].start, c.text, c.anchor_in);
            next.push_back(Selection{anchor, caret});
        }
        const Pos ph = primary_sel().head;
        int pi = 0;
        for (std::size_t i = 0; i < ordered.size(); ++i) if (ordered[i].head == ph) pi = static_cast<int>(i);
        sels = std::move(next);
        primary = std::min(pi, static_cast<int>(sels.size()) - 1);
        normalize();
        undo_.back().after = sels;
    }
};

}  // namespace mayag::editor
