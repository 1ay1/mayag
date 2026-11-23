#pragma once
// mayag::widget code_view — the editor surface
//
//   code_view(c, "editor", m.ed, opts)
//
// Draws an EditorState: gutter (line numbers, git marks, fold chevrons,
// diagnostic dots), syntax-highlighted text, current-line highlight,
// selections, every caret, bracket match, occurrence highlights of the word
// under the caret, find matches, indent guides, inline diagnostics, rulers,
// and scrollbars with an overview of what is where in the file.
//
// Only the VISIBLE rows are built — a 100k-line file costs the same per
// frame as a 50-line one. Each line is one text node per highlighted run;
// carets and selections are absolute boxes computed from the monospace cell
// width, measured once from the real font.
//
// Interaction (handled by the app, translated with code_view_hit):
//   Pressed on node_id(id)   -> code_view_hit(ed, local) -> set_cursor /
//                               add_cursor_at (alt) / extend (shift);
//                               click_count 2 = word, 3 = line
//   Dragged on node_id(id)   -> ed.drag_to(code_view_hit(ed, local))
//   Clicked on part_id(id, part_fold_base + line) -> ed.toggle_fold(line)
//   Scrolled on node_id(id)  -> ed.scroll.scroll_by(delta) (both axes)
//   Pressed/Dragged on part_id(id, part_bar)  -> scrollbar_press/drag(ed.scroll)

#include "control.hpp"
#include "scroll_view.hpp"
#include "../editor/buffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace mayag::dsl {

/// The colours of the code surface. One per token kind, plus chrome.
struct CodeTheme {
    std::array<Color<Srgb>, editor::kTokCount> tok{};
    Color<Srgb> bg, gutter_bg, line_no, line_no_active, current_line, selection, selection_inactive,
                cursor, bracket, occurrence, find_match, find_current, indent_guide,
                indent_guide_active, ruler, whitespace, fold_marker, folded_bg,
                git_added, git_modified, git_deleted, error, warning, info, hint;

    [[nodiscard]] Color<Srgb> of(editor::Tok k) const { return tok[static_cast<std::size_t>(k)]; }

    /// A dark theme derived from the app Theme's accent, tuned like the
    /// One/Tokyo families: muted chrome so code carries the colour.
    [[nodiscard]] static CodeTheme dark(const Theme& t) {
        using editor::Tok;
        CodeTheme c;
        const auto set = [&](Tok k, std::uint32_t v) {
            c.tok[static_cast<std::size_t>(k)] = rgb8(static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v));
        };
        set(Tok::text, 0xC8D3F5);  set(Tok::keyword, 0xC099FF); set(Tok::type, 0x65BCFF);
        set(Tok::function, 0x82AAFF); set(Tok::number, 0xFF966C); set(Tok::string, 0xC3E88D);
        set(Tok::comment, 0x636DA6); set(Tok::op, 0x89DDFF); set(Tok::punct, 0x9AA5CE);
        set(Tok::constant, 0xFF966C); set(Tok::macro, 0xFC7B7B); set(Tok::property, 0x4FD6BE);
        set(Tok::special, 0xFF757F); set(Tok::heading, 0x82AAFF);
        c.bg = mix(t.background, colors::black, 0.15f);
        c.gutter_bg = c.bg;
        c.line_no = t.text_disabled.fade(0.75f);
        c.line_no_active = t.text_primary;
        c.current_line = mix(c.bg, colors::white, 0.035f);
        c.selection = mix(c.bg, t.accent, 0.28f);
        c.selection_inactive = mix(c.bg, t.text_secondary, 0.14f);
        c.cursor = t.accent;
        c.bracket = mix(c.bg, colors::white, 0.18f);
        c.occurrence = mix(c.bg, t.text_secondary, 0.16f);
        c.find_match = mix(c.bg, t.warning, 0.22f);
        c.find_current = mix(c.bg, t.warning, 0.55f);
        c.indent_guide = mix(c.bg, colors::white, 0.06f);
        c.indent_guide_active = mix(c.bg, colors::white, 0.16f);
        c.ruler = mix(c.bg, colors::white, 0.05f);
        c.whitespace = mix(c.bg, colors::white, 0.14f);
        c.fold_marker = t.text_secondary;
        c.folded_bg = mix(c.bg, t.accent, 0.12f);
        c.git_added = t.success; c.git_modified = t.info; c.git_deleted = t.danger;
        c.error = t.danger; c.warning = t.warning; c.info = t.info; c.hint = t.text_secondary;
        return c;
    }

    /// Code in a TERMINAL's colours: the 16 ANSI colours plus its ink, paper,
    /// cursor and selection (0xRRGGBB). An editor beside a shell then reads as
    /// the same surface in the same scheme -- any scheme, light or dark, since
    /// every tint is a mix between the scheme's own ink and paper.
    [[nodiscard]] static CodeTheme from_palette(const Theme& t, const std::array<std::uint32_t, 16>& ansi, std::uint32_t ink,
                                                std::uint32_t paper, std::uint32_t caret, std::uint32_t selection) {
        using editor::Tok;
        const auto C = [](std::uint32_t v) {
            return rgb8(static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v));
        };
        const auto A = [&](int i) { return C(ansi[static_cast<std::size_t>(i)]); };
        CodeTheme c;
        const Color<Srgb> fg = C(ink), bg = C(paper);
        const auto set = [&](Tok k, Color<Srgb> v) { c.tok[static_cast<std::size_t>(k)] = v; };
        set(Tok::text, fg);
        set(Tok::keyword, A(5));                    // magenta
        set(Tok::type, A(3));                       // yellow
        set(Tok::function, A(4));                   // blue
        set(Tok::string, A(2));                     // green
        set(Tok::number, mix(A(1), A(3), 0.5f));    // orange: between red and yellow
        set(Tok::constant, mix(A(1), A(3), 0.5f));
        set(Tok::comment, mix(A(8), fg, 0.12f));    // bright black, lifted a touch
        set(Tok::op, mix(fg, A(6), 0.6f));
        set(Tok::punct, mix(fg, bg, 0.3f));
        set(Tok::macro, A(1));
        set(Tok::property, A(6));                   // cyan
        set(Tok::special, A(1));
        set(Tok::heading, A(4));
        c.bg = bg;
        c.gutter_bg = bg;
        c.line_no = mix(fg, bg, 0.66f);
        c.line_no_active = mix(fg, bg, 0.15f);
        c.current_line = mix(bg, fg, 0.045f);
        c.selection = C(selection);
        c.selection_inactive = mix(bg, C(selection), 0.5f);
        c.cursor = C(caret);
        c.bracket = mix(bg, fg, 0.2f);
        c.occurrence = mix(bg, fg, 0.11f);
        c.find_match = mix(bg, A(3), 0.24f);
        c.find_current = mix(bg, A(3), 0.55f);
        c.indent_guide = mix(bg, fg, 0.07f);
        c.indent_guide_active = mix(bg, fg, 0.18f);
        c.ruler = mix(bg, fg, 0.05f);
        c.whitespace = mix(bg, fg, 0.2f);
        c.fold_marker = mix(fg, bg, 0.4f);
        c.folded_bg = mix(bg, A(4), 0.12f);
        c.git_added = A(2); c.git_modified = A(4); c.git_deleted = A(1);
        c.error = A(1); c.warning = A(3); c.info = A(4); c.hint = mix(fg, bg, 0.4f);
        (void)t;
        return c;
    }
};

struct CodeViewOptions {
    float font_size = 13.0f;
    float line_height = 1.45f;           ///< multiple of font size
    bool  focused = true;
    bool  line_numbers = true;
    bool  relative_numbers = false;
    bool  indent_guides = true;
    bool  current_line = true;
    bool  occurrences = true;            ///< highlight other instances of the word at the caret
    bool  show_whitespace = false;       ///< · for spaces in selections, → for tabs everywhere
    bool  cursor_on = true;              ///< blink phase, owned by the app
    int   cursor_style = 0;              ///< 0 beam, 1 block, 2 underline
    std::vector<int> rulers{};           ///< columns
    const std::vector<editor::Range>*      find_matches = nullptr;
    int                                    find_current = -1;
    const std::vector<editor::Diagnostic>* diagnostics = nullptr;
    const std::vector<editor::GitMark>*    git = nullptr;
    const editor::Range*                   inline_hint_range = nullptr;   ///< e.g. a pending completion's ghost text
    std::string                            ghost_text{};                  ///< drawn after the caret (inline completion)
    /// The face role code is set in: monospace by default, so the grid the
    /// caret math assumes is the grid the glyphs actually sit on.
    FontFamily                             family = FontFamily::mono;
    bool                                   ligatures = false;             ///< -> != ===, when the face has them
    /// The file at a glance, right of the code: every line as coloured
    /// blocks, the visible part as a slider (press or drag to go there).
    bool                                   minimap = false;
    /// The scopes around the top line (function, class, block) pinned to
    /// the top as you scroll through them; a click goes to that line.
    bool                                   sticky = false;
    /// Where the primary caret is DRAWN, in (display column, visual row):
    /// the app eases it toward the real position so it glides between
    /// places instead of jumping. < 0: drawn exactly where it is.
    float                                  caret_col = -1.0f;
    float                                  caret_row = -1.0f;
};

/// Parts under the code view's id (beside part_bar).
inline constexpr std::size_t part_minimap = part_dec + 7;
/// part_sticky_base + line: a pinned scope header (far above the fold ids).
inline constexpr std::size_t part_sticky_base = part_dec + (std::size_t{1} << 26);

inline constexpr std::size_t part_fold_base = part_dec + 1000;

namespace detail {

/// One monospace cell, measured from the real font (so the caret lands on
/// the glyph edge whatever the face). Falls back to 0.6 em.
template <class C>
[[nodiscard]] float cell_width(const C& c, float size, FontFamily fam = FontFamily::mono) {
    if constexpr (requires { c.c.measurer(); }) {
        TextStyle st; st.size = size; st.family = fam;
        const float w = c.c.measurer().advance("0000000000", st) / 10.0f;
        if (w > 0.0f) return w;
    } else if constexpr (requires { c.measurer(); }) {
        TextStyle st; st.size = size; st.family = fam;
        const float w = c.measurer().advance("0000000000", st) / 10.0f;
        if (w > 0.0f) return w;
    }
    return size * 0.6f;
}

/// The lines that open the scopes enclosing line `l` (outermost first): each
/// is the nearest line above with less indentation that has something nested
/// under it -- a function, a class, an if. What sticky scroll pins.
[[nodiscard]] inline std::vector<int> scopes_at(const editor::EditorState& ed, int l, int max_depth) {
    std::vector<int> out;
    if (l <= 0 || l >= ed.line_count()) return out;
    int j = l;
    while (j < ed.line_count() && ed.blank(j)) ++j;   // a blank line belongs to what follows
    if (j >= ed.line_count()) return out;
    int need = ed.display_col(j, ed.indent_of(j));
    for (int k = l - 1; k >= 0 && need > 0; --k) {
        if (ed.blank(k)) continue;
        const int ind = ed.display_col(k, ed.indent_of(k));
        if (ind >= need) continue;
        // a closing line (`}`, `end`) is not a header
        const char ch = ed.line(k)[static_cast<std::size_t>(ed.indent_of(k))];
        if (ch != '}' && ch != ')' && ch != ']') out.push_back(k);
        need = ind;
    }
    std::reverse(out.begin(), out.end());
    if (static_cast<int>(out.size()) > max_depth) out.erase(out.begin(), out.end() - max_depth);
    return out;
}

/// The minimap: every visual row from `top` as blocks of its syntax colours,
/// 2 px a row, a cell 1 px wide (VS Code's "render characters: false").
/// Rebuilt only when the text, the window onto it or the colours change.
template <class Th>
[[nodiscard]] inline Node minimap_rows(const editor::EditorState& ed, const Th& th, int top, int rows, float w, float row_px) {
    std::vector<Node> blocks;
    const auto& vis = ed.visual_lines();
    const float cell = 1.0f;
    const int max_cols = static_cast<int>(w / cell);
    for (int r = 0; r < rows && top + r < static_cast<int>(vis.size()); ++r) {
        const int l = vis[static_cast<std::size_t>(top + r)];
        const std::string& s = ed.line(l);
        if (s.empty()) continue;
        const float y = static_cast<float>(r) * row_px;
        const auto spans = ed.syntax.spans(*ed.language, ed.lines, l);
        // one block per run of non-space text, in its token's colour
        const auto run = [&](std::uint32_t a, std::uint32_t b, Color<Srgb> col) {
            std::uint32_t i = a;
            while (i < b) {
                while (i < b && (s[i] == ' ' || s[i] == '\t')) ++i;
                std::uint32_t e = i;
                while (e < b && s[e] != ' ' && s[e] != '\t') ++e;
                if (e > i) {
                    const int c0 = ed.display_col(l, static_cast<int>(i)), c1 = ed.display_col(l, static_cast<int>(e));
                    if (c0 >= max_cols) return;
                    const float x = static_cast<float>(c0) * cell;
                    const float bw = static_cast<float>(std::min(c1, max_cols) - c0) * cell;
                    blocks.push_back((box() | size(bw, std::max(row_px - 0.5f, 1.0f)) | bg(col.fade(0.75f)) | absolute(x, y)).build());
                }
                i = e;
            }
        };
        std::uint32_t at = 0;
        for (const auto& sp : spans) {
            if (sp.start > at) run(at, sp.start, th.of(editor::Tok::text));
            run(sp.start, sp.end, th.of(sp.kind));
            at = sp.end;
        }
        if (at < s.size()) run(at, static_cast<std::uint32_t>(s.size()), th.of(editor::Tok::text));
    }
    return (z(h_list(std::move(blocks))) | size(w, static_cast<float>(rows) * row_px)).build();
}

/// Expand tabs, so a run's text sits on the cell grid exactly.
[[nodiscard]] inline std::string expand_tabs(std::string_view s, int start_col, int tab, bool visible) {
    std::string out;
    int col = start_col;
    for (char ch : s) {
        if (ch == '\t') {
            const int n = (col / tab + 1) * tab - col;
            if (visible) { out += "\u2192"; out.append(static_cast<std::size_t>(n - 1), ' '); }
            else out.append(static_cast<std::size_t>(n), ' ');
            col += n;
        } else {
            out += ch;
            if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++col;
        }
    }
    return out;
}

}  // namespace detail

/// A click at `local` (relative to the code view node) to a buffer position.
[[nodiscard]] inline editor::Pos code_view_hit(const editor::EditorState& ed, Vec2 local) {
    const auto& mt = ed.metrics;
    const float y = local.y - mt.text_dy + ed.scroll.offset.y;
    const auto& vis = ed.visual_lines();
    const int row = std::clamp(static_cast<int>(std::floor(y / mt.line_h)), 0, static_cast<int>(vis.size()) - 1);
    const int line = vis[static_cast<std::size_t>(row)];
    const float x = local.x - mt.gutter_w + ed.scroll.offset.x;
    const float dcol = x / mt.cell_w;
    return {line, ed.byte_at_display_col(line, dcol)};
}

template <ControlContext C>
[[nodiscard]] inline Node code_view(const C& c, std::string_view id, const editor::EditorState& ed,
                                    const CodeTheme& th, const CodeViewOptions& o = {}) {
    const Theme& t = c.theme;   // geometry follows the theme's knobs (t.rounded / t.stroke)
    using namespace editor;
    const float fs = o.font_size;   // the editor's cells: exact, like the terminal grid
    const float lh = std::round(fs * o.line_height);
    const float cw = detail::cell_width(c, fs, o.family);
    const int digits = std::max(3, static_cast<int>(std::to_string(ed.line_count()).size()));
    const float gutter = o.line_numbers ? static_cast<float>(digits) * cw + 44.0f : 28.0f;
    const auto& vis = ed.visual_lines();
    const int rows = static_cast<int>(vis.size());

    // Geometry the app's update() will need (page size, hit testing, reveal).
    ed.metrics.line_h = lh;
    ed.metrics.cell_w = cw;
    ed.metrics.gutter_w = gutter;
    ed.metrics.text_dy = 0.0f;
    ed.metrics.page_lines = std::max(1, static_cast<int>(ed.scroll.viewport.y / lh) - 2);

    const float vp_h = std::max(ed.scroll.viewport.y, lh * 30.0f);
    const float vp_w = std::max(ed.scroll.viewport.x, 400.0f);
    const int first = std::clamp(static_cast<int>(ed.scroll.offset.y / lh) - 1, 0, std::max(rows - 1, 0));
    const int last = std::clamp(static_cast<int>((ed.scroll.offset.y + vp_h) / lh) + 2, 0, rows);
    const float sx = ed.scroll.offset.x;
    const float sy = ed.scroll.offset.y;
    const float text_x = gutter - sx;            // x of display column 0
    const float content_w = std::max(vp_w, gutter + static_cast<float>(ed.max_display_cols() + 8) * cw);
    const float content_h = static_cast<float>(rows) * lh + std::max(vp_h - lh * 3.0f, 0.0f);   // scroll past end

    const Pos cur = ed.cursor();
    const auto row_y = [&](int r) { return static_cast<float>(r) * lh - sy; };
    const auto col_x = [&](int line, int byte) { return text_x + static_cast<float>(ed.display_col(line, byte)) * cw; };
    const auto vrow = [&](int line) { return ed.visual_row_of(line); };

    std::vector<Node> layers;   // all absolute, painted in order
    const auto rect = [&](float x, float y, float w, float h, Color<Srgb> col, float r = 0.0f) {
        layers.push_back((box() | size(std::max(w, 0.0f), std::max(h, 0.0f)) | bg(col) | radius(r) | absolute(x, y)).build());
    };

    // ── backgrounds under the text ──────────────────────────────────────
    if (o.current_line && ed.sels.size() == 1 && ed.sels[0].empty() && o.focused)
        rect(0, row_y(vrow(cur.line)), content_w + sx, lh, th.current_line);

    if (o.rulers.size() > 0)
        for (int col : o.rulers) rect(text_x + static_cast<float>(col) * cw, 0, 1, vp_h + lh, th.ruler);

    // Find matches and occurrences of the word under the caret.
    const auto mark_range = [&](const Range& r, Color<Srgb> col, float rad) {
        if (r.start.line != r.end.line) return;
        const int vr = vrow(r.start.line);
        if (vr < first || vr >= last || ed.hidden(r.start.line)) return;
        const float x0 = col_x(r.start.line, r.start.col), x1 = col_x(r.end.line, r.end.col);
        rect(x0, row_y(vr) + 1, x1 - x0, lh - 2, col, rad);
    };
    if (o.find_matches != nullptr)
        for (std::size_t i = 0; i < o.find_matches->size(); ++i)
            mark_range((*o.find_matches)[i], static_cast<int>(i) == o.find_current ? th.find_current : th.find_match, 2);
    if (o.occurrences && ed.sels.size() == 1 && (o.find_matches == nullptr || o.find_matches->empty())) {
        const Selection& s = ed.sels[0];
        Range w = s.empty() ? ed.word_at(cur) : s.range();
        const std::string word = w.start.line == w.end.line ? ed.text_in(w) : std::string{};
        const bool wordy = !word.empty() && std::all_of(word.begin(), word.end(), [](char ch) {
            return editor::detail::is_word(static_cast<unsigned char>(ch));
        });
        if (wordy && word.size() < 64) {
            // Only the visible rows are searched: cheap on any file size.
            for (int r = first; r < last; ++r) {
                const int l = vis[static_cast<std::size_t>(r)];
                const std::string& s2 = ed.line(l);
                for (std::size_t at = s2.find(word); at != std::string::npos; at = s2.find(word, at + word.size())) {
                    const Range m{{l, static_cast<int>(at)}, {l, static_cast<int>(at + word.size())}};
                    const char b = ed.char_before(m.start), a = ed.char_at(m.end);
                    if (editor::detail::is_word(static_cast<unsigned char>(b)) || editor::detail::is_word(static_cast<unsigned char>(a))) continue;
                    if (s.empty() || m != s.range()) mark_range(m, th.occurrence, 2);
                }
            }
        }
    }

    // Selections: one box per covered row segment, rounded like Zed's.
    for (const auto& s : ed.sels) {
        if (s.empty()) continue;
        const Pos a = s.start(), b = s.end();
        for (int l = a.line; l <= b.line; ++l) {
            if (ed.hidden(l)) continue;
            const int vr = vrow(l);
            if (vr < first || vr >= last) continue;
            const int c0 = l == a.line ? a.col : 0;
            const int c1 = l == b.line ? b.col : ed.line_len(l);
            const float x0 = col_x(l, c0);
            float x1 = col_x(l, c1);
            if (l != b.line) x1 += cw * 0.6f;   // the newline is selected too
            rect(x0, row_y(vr), x1 - x0, lh, o.focused ? th.selection : th.selection_inactive, 3);
        }
    }

    // Bracket pair at the caret.
    if (ed.sels.size() == 1 && o.focused)
        if (const auto bm = ed.match_bracket(cur)) {
            for (const Pos p : {bm->first, bm->second}) {
                const int vr = vrow(p.line);
                if (vr < first || vr >= last || ed.hidden(p.line)) continue;
                const float x = col_x(p.line, p.col);
                layers.push_back((box() | size(cw, lh - 2) | bg(th.bracket) | border(t.stroke(1), th.indent_guide_active) | radius(t.rounded(2))
                                  | absolute(x, row_y(vr) + 1)).build());
            }
        }

    // Indent guides: one per indent level of each line (blank lines inherit
    // the deeper of their neighbours, so guides do not break on empty lines).
    if (o.indent_guides) {
        const int tw = std::max(ed.tab_width, 1);
        const auto level_of = [&](int l) {
            if (!ed.blank(l)) return ed.display_col(l, ed.indent_of(l)) / tw;
            int up = 0, dn = 0;
            for (int j = l - 1; j >= 0; --j) if (!ed.blank(j)) { up = ed.display_col(j, ed.indent_of(j)) / tw; break; }
            for (int j = l + 1; j < ed.line_count(); ++j) if (!ed.blank(j)) { dn = ed.display_col(j, ed.indent_of(j)) / tw; break; }
            return std::min(up, dn);
        };
        const int active = ed.blank(cur.line) ? level_of(cur.line) : ed.display_col(cur.line, ed.indent_of(cur.line)) / tw;
        for (int r = first; r < last; ++r) {
            const int l = vis[static_cast<std::size_t>(r)];
            const int lv = level_of(l);
            for (int k = 0; k < lv; ++k) {
                const bool act = k == active - 1 && std::abs(l - cur.line) < 200;
                rect(text_x + static_cast<float>(k * tw) * cw, row_y(r), 1, lh,
                     act ? th.indent_guide_active : th.indent_guide);
            }
        }
    }

    // ── text ────────────────────────────────────────────────────────────
    const auto& lang = *ed.language;
    for (int r = first; r < last; ++r) {
        const int l = vis[static_cast<std::size_t>(r)];
        const std::string& s = ed.line(l);
        if (s.empty() && !ed.folds.contains(l)) continue;
        const auto spans = ed.syntax.spans(lang, ed.lines, l);
        std::vector<Node> runs;
        int col = 0;
        std::uint32_t at = 0;
        const auto push_run = [&](std::uint32_t a, std::uint32_t b, Color<Srgb> col_, bool italic_ = false) {
            if (b <= a) return;
            const std::string seg = detail::expand_tabs(std::string_view{s}.substr(a, b - a), col, ed.tab_width, o.show_whitespace);
            col = ed.display_col(l, static_cast<int>(b));
            Node n = (text_owned(seg) | font(fs) | family(o.family) | fg(col_) | ligatures(o.ligatures) | clip_text).build();
            if (italic_) n.style().text.italic = true;
            runs.push_back(std::move(n));
        };
        for (const auto& sp : spans) {
            if (sp.start > at) push_run(at, sp.start, th.of(Tok::text));
            push_run(sp.start, sp.end, th.of(sp.kind), sp.kind == Tok::comment);
            at = sp.end;
        }
        if (at < s.size()) push_run(at, static_cast<std::uint32_t>(s.size()), th.of(Tok::text));
        if (ed.folds.contains(l)) {
            const int hidden_n = ed.fold_end(l).value_or(l) - l;
            runs.push_back((h(text_owned(" \u22EF " + std::to_string(hidden_n) + " lines ") | font(t.type_at(o.font_size, Tier::small)) | family(o.family) | fg(th.fold_marker))
                            | bg(th.folded_bg) | radius(t.rounded(3)) | pad(0, t.spaced(2)) | margin(0, 0, 0, 6)
                            | id_of(id, part_fold_base + static_cast<std::size_t>(l))).build());
        }
        if (runs.empty()) continue;
        layers.push_back((h(h_list(std::move(runs))) | height(lh) | align(Align::center)
                          | absolute(text_x, row_y(r))).build());
    }

    // Ghost text (inline completion) after the primary caret.
    if (!o.ghost_text.empty() && ed.sels.size() == 1 && ed.sels[0].empty()) {
        const int vr = vrow(cur.line);
        if (vr >= first && vr < last)
            layers.push_back((h(text_owned(o.ghost_text) | font(fs) | family(o.family) | italic | fg(th.of(Tok::comment)) | no_ligatures)
                              | height(lh) | align(Align::center)
                              | absolute(col_x(cur.line, cur.col), row_y(vr))).build());
    }

    // Inline diagnostics: a squiggle-like underline and the message at line end.
    if (o.diagnostics != nullptr) {
        std::vector<int> shown;
        for (const auto& d : *o.diagnostics) {
            const Color<Srgb> col = d.severity == editor::Diagnostic::Severity::error ? th.error
                                  : d.severity == editor::Diagnostic::Severity::warning ? th.warning
                                  : d.severity == editor::Diagnostic::Severity::info ? th.info : th.hint;
            const int l = d.range.start.line;
            if (ed.hidden(l)) continue;
            const int vr = vrow(l);
            if (vr < first || vr >= last) continue;
            const int c1 = d.range.end.line == l ? d.range.end.col : ed.line_len(l);
            const float x0 = col_x(l, d.range.start.col);
            const float x1 = std::max(col_x(l, c1), x0 + cw);
            // A dashed underline reads as a squiggle at this size.
            layers.push_back((box() | size(x1 - x0, 0.0f) | border(t.stroke(1.5f), col) | dashed(2.5f, 1.5f)
                              | absolute(x0, row_y(vr) + lh - 3)).build());
            if (std::find(shown.begin(), shown.end(), l) != shown.end()) continue;
            shown.push_back(l);
            const float ex = col_x(l, ed.line_len(l)) + cw * 3.0f;
            layers.push_back((h(text_owned(d.message) | font(t.type_at(o.font_size, Tier::small)) | fg(col) | ellipsis)
                              | height(lh) | align(Align::center) | pad(0, t.spaced(8)) | max_size(560, lh)
                              | bg(mix(th.bg, col, 0.10f)) | radius(t.rounded(3))
                              | absolute(ex, row_y(vr))).build());
        }
    }

    // Carets, drawn last so they sit over everything. The primary one is
    // drawn where the app's eased position says (it glides between places),
    // with a soft halo -- the one thing on screen that moves as you type.
    for (std::size_t i = 0; i < ed.sels.size(); ++i) {
        const auto& s = ed.sels[i];
        const bool prim = static_cast<int>(i) == ed.primary;
        if (!o.focused || (!o.cursor_on && !(prim && o.caret_row >= 0.0f && std::abs(o.caret_row - static_cast<float>(vrow(s.head.line))) > 0.02f))) {
            if (!o.focused) break;
            continue;
        }
        const Pos p = s.head;
        if (ed.hidden(p.line)) continue;
        const int vr = vrow(p.line);
        if (vr < first || vr >= last) continue;
        float x = col_x(p.line, p.col);
        float y = row_y(vr);
        if (prim && o.caret_row >= 0.0f) {   // the glide
            x = text_x + o.caret_col * cw;
            y = o.caret_row * lh - sy;
        }
        const Color<Srgb> col = prim ? th.cursor : th.cursor.fade(0.7f);
        if (prim && o.cursor_style == 0)
            layers.push_back((box() | size(2, lh - 4) | bg(col.fade(0.0f)) | radius(1.0f)
                              | shadow(5.0f, col.fade(0.35f), Vec2{0.0f, 0.0f}) | absolute(x - 1, y + 2)).build());
        if (o.cursor_style == 1) rect(x, y + 1, cw, lh - 2, col.fade(0.55f), 1);
        else if (o.cursor_style == 2) rect(x, y + lh - 3, cw, 2, col);
        else rect(x - 1, y + 2, 2, lh - 4, col, 1);
    }

    // ── gutter (drawn over the text, so horizontal scroll slides under it) ──
    std::vector<Node> gut;
    gut.push_back((box() | size(gutter, vp_h + lh * 4) | bg(th.gutter_bg) | absolute(0, 0)).build());
    std::vector<int> diag_lines;
    if (o.diagnostics != nullptr) for (const auto& d : *o.diagnostics) diag_lines.push_back(d.range.start.line);
    for (int r = first; r < last; ++r) {
        const int l = vis[static_cast<std::size_t>(r)];
        const float y = row_y(r);
        const bool active = l == cur.line;
        if (o.line_numbers) {
            const int shown_no = o.relative_numbers && !active ? std::abs(l - cur.line) : l + 1;
            gut.push_back((h(text_owned(std::to_string(shown_no)) | font(fs) | family(o.family)
                                 | fg(active ? th.line_no_active : th.line_no) | text_align(TextAlign::right) | width(static_cast<float>(digits) * cw))
                           | height(lh) | align(Align::center) | absolute(12, y)).build());
        }
        // Diagnostic dot left of the number.
        if (std::find(diag_lines.begin(), diag_lines.end(), l) != diag_lines.end()) {
            Color<Srgb> col = th.hint;
            for (const auto& d : *o.diagnostics)
                if (d.range.start.line == l) { col = d.severity == editor::Diagnostic::Severity::error ? th.error : d.severity == editor::Diagnostic::Severity::warning ? th.warning : th.info; break; }
            gut.push_back((box() | size(6, 6) | bg(col) | radius(t.rounded(3)) | absolute(3, y + (lh - 6) * 0.5f)).build());
        }
        // Fold chevron.
        if (ed.foldable(l) || ed.folds.contains(l)) {
            const bool folded = ed.folds.contains(l);
            const auto fid = part_id(id, part_fold_base + static_cast<std::size_t>(l));
            const bool hov = c.hovered(fid);
            gut.push_back((h(text_owned(folded ? "\u203A" : "\u2304") | font(fs * (folded ? 1.1f : 0.85f))
                                 | fg(hov || folded ? th.line_no_active : th.line_no.fade(c.hovered(node_id(id)) ? 1.0f : 0.0f)))
                           | size(16, lh) | center | pointer_cursor | absolute(gutter - 26, y)
                           | id_of(id, part_fold_base + static_cast<std::size_t>(l))).build());
        }
    }
    if (o.git != nullptr) {
        for (const auto& g : *o.git) {
            if (ed.hidden(g.line)) continue;
            const int vr = vrow(g.line);
            if (vr < first || vr >= last) continue;
            const Color<Srgb> col = g.kind == editor::GitMark::Kind::added ? th.git_added
                                  : g.kind == editor::GitMark::Kind::deleted ? th.git_deleted : th.git_modified;
            if (g.kind == editor::GitMark::Kind::deleted) rect(gutter - 7, row_y(vr) + lh - 3, 6, 4, col, 1);
            else rect(gutter - 6, row_y(vr), 3, lh, col, 1);
        }
    }
    for (auto& n : gut) layers.push_back(std::move(n));
    // Re-draw git marks over the gutter background.
    if (o.git != nullptr) {
        for (const auto& g : *o.git) {
            if (ed.hidden(g.line)) continue;
            const int vr = vrow(g.line);
            if (vr < first || vr >= last) continue;
            const Color<Srgb> col = g.kind == editor::GitMark::Kind::added ? th.git_added
                                  : g.kind == editor::GitMark::Kind::deleted ? th.git_deleted : th.git_modified;
            if (g.kind == editor::GitMark::Kind::deleted)
                layers.push_back((box() | size(8, 3) | bg(col) | radius(t.rounded(1)) | absolute(gutter - 10, row_y(vr) - 1)).build());
            else
                layers.push_back((box() | size(3, lh) | bg(col) | radius(t.rounded(1)) | absolute(gutter - 8, row_y(vr))).build());
        }
    }

    // ── sticky scroll: the scopes around the top line, pinned ───────────
    // The function you are scrolling through stays named at the top; nested
    // blocks stack under it (at most 4). Each header is its line, drawn as
    // the line, so it reads as the code it is; a click goes there.
    const Color<Srgb> ink = th.of(Tok::text);
    ed.metrics.sticky_rows = 0;
    if (o.sticky && rows > 0) {
        const int top_row = std::clamp(static_cast<int>(sy / lh), 0, rows - 1);
        // The headers cover rows: what they pin is the scope of the first
        // row visible BELOW them. Grow n until it holds (at most 4 rounds).
        std::vector<int> scopes;
        for (int n = 0, round = 0; round < 5; ++round) {
            const int probe = std::min(top_row + n, rows - 1);
            auto all = detail::scopes_at(ed, vis[static_cast<std::size_t>(probe)], 4);
            // header k sits in pinned slot k: it is pinned while its own row
            // has scrolled to (or past) that slot
            std::vector<int> s2;
            for (const int l : all) if (vrow(l) <= top_row + static_cast<int>(s2.size())) s2.push_back(l);
            scopes = std::move(s2);
            if (static_cast<int>(scopes.size()) == n) break;
            n = static_cast<int>(scopes.size());
        }
        const int n = static_cast<int>(scopes.size());
        if (n > 0) {
            ed.metrics.sticky_rows = n;
            const Color<Srgb> paper = mix(th.bg, ink, 0.03f);
            std::vector<Node> rows_n;
            for (int k = 0; k < n; ++k) {
                const int l = scopes[static_cast<std::size_t>(k)];
                const std::string& s = ed.line(l);
                const auto spans = ed.syntax.spans(lang, ed.lines, l);
                std::vector<Node> runs;
                int col = 0;
                std::uint32_t at = 0;
                const auto push = [&](std::uint32_t a, std::uint32_t b, Color<Srgb> cl) {
                    if (b <= a) return;
                    runs.push_back((text_owned(detail::expand_tabs(std::string_view{s}.substr(a, b - a), col, ed.tab_width, false))
                                    | font(fs) | family(o.family) | fg(cl) | clip_text).build());
                    col = ed.display_col(l, static_cast<int>(b));
                };
                for (const auto& sp : spans) { if (sp.start > at) push(at, sp.start, ink); push(sp.start, sp.end, th.of(sp.kind)); at = sp.end; }
                if (at < s.size()) push(at, static_cast<std::uint32_t>(s.size()), ink);
                const auto sid = part_id(id, part_sticky_base + static_cast<std::size_t>(l));
                const bool hov = c.hovered(sid);
                rows_n.push_back((z(box() | size(content_w + sx, lh) | bg(hov ? mix(paper, ink, 0.05f) : paper),
                                    (h(text_owned(std::to_string(l + 1)) | font(fs) | family(o.family) | fg(th.line_no)
                                           | text_align(TextAlign::right) | width(static_cast<float>(digits) * cw))
                                     | height(lh) | align(Align::center) | absolute(12, 0)),
                                    h(std::move(runs)) | height(lh) | align(Align::center) | absolute(text_x, 0))
                                  | size(content_w + sx, lh) | absolute(0, static_cast<float>(k) * lh) | pointer_cursor | id_of(id, part_sticky_base + static_cast<std::size_t>(l))).build());
            }
            // a soft edge below, so the code slides under the pinned lines
            rows_n.push_back((box() | size(content_w + sx, 6) | absolute(0, static_cast<float>(n) * lh)
                              | shadow(6.0f, colors::black.fade(contrast_ratio(th.bg, colors::black) < 3.0f ? 0.35f : 0.12f), Vec2{0.0f, 1.0f})
                              | bg(paper.fade(0.0f))).build());
            rows_n.push_back((box() | size(content_w + sx, 1) | bg(mix(paper, ink, 0.1f)) | absolute(0, static_cast<float>(n) * lh - 1)).build());
            for (auto& r : rows_n) layers.push_back(std::move(r));
        }
    }

    // ── the scrollbar: a slim thumb over an overview of the file ────────
    // Ink on the paper (never a hard-coded white), so it fits light schemes.
    // At rest a 4 px capsule; under the pointer it widens and brightens.
    // Behind it, the overview: git edits, find matches, diagnostics and the
    // carets, each where it is in the file -- so the bar says where to go.
    const bool active_bar = c.hovered(part_id(id, part_bar)) || c.pressed(part_id(id, part_bar));
    const bool near_bar = active_bar || c.hovered(node_id(id));
    const Rect track = detail::bar_track(ed.scroll);
    const Rect thumb = ed.scroll.thumb_v(track);
    std::vector<Node> marks;
    const float doc_h = std::max(content_h, 1.0f);
    const auto mark = [&](int line, Color<Srgb> col, float x, float w, float h = 2.0f) {
        const float y = track.top() + static_cast<float>(vrow(line)) * lh / doc_h * track.height();
        marks.push_back((box() | size(w, h) | bg(col) | radius(t.rounded(1)) | absolute(x, y)).build());
    };
    if (o.git != nullptr) for (const auto& g : *o.git) mark(g.line, g.kind == editor::GitMark::Kind::added ? th.git_added : g.kind == editor::GitMark::Kind::deleted ? th.git_deleted : th.git_modified, 0, 3, 3);
    if (o.find_matches != nullptr)
        for (std::size_t i = 0; i < o.find_matches->size(); ++i)
            mark((*o.find_matches)[i].start.line, static_cast<int>(i) == o.find_current ? th.find_current : th.find_current.fade(0.7f), 3, 6, 2);
    if (o.diagnostics != nullptr) for (const auto& d : *o.diagnostics) mark(d.range.start.line, d.severity == editor::Diagnostic::Severity::error ? th.error : th.warning, 8, 4, 3);
    for (const auto& s : ed.sels) mark(s.head.line, th.cursor, 0, detail::kBarWidth, 2);
    const float thumb_w = active_bar ? 8.0f : near_bar ? 6.0f : 4.0f;
    const Color<Srgb> thumb_col = active_bar ? mix(th.bg, ink, c.pressed(part_id(id, part_bar)) ? 0.42f : 0.32f)
                                             : mix(th.bg, ink, near_bar ? 0.22f : 0.14f);
    auto bar = z(z(h_list(std::move(marks))),
                 box() | size(thumb_w, std::max(thumb.height(), 0.0f))
                       | bg(ed.scroll.scrollable_y() ? thumb_col : thumb_col.fade(0.0f))
                       | radius(thumb_w * 0.5f) | absolute((detail::kBarWidth - thumb_w) * 0.5f, thumb.top())
                       | transition(transition_props::color | transition_props::shape))
             | width(detail::kBarWidth) | height(pct(100))
             | bg(active_bar ? mix(th.bg, ink, 0.04f) : th.bg.fade(0.0f))
             | inner_shadow(0.0f, active_bar ? mix(th.bg, ink, 0.08f) : th.bg.fade(0.0f), {1.0f, 0.0f})
             | id_of(id, part_bar);

    // Horizontal scrollbar: a hairline capsule, only when there is something to scroll.
    Node hbar;
    if (ed.scroll.max_offset.x > 0.5f) {
        const float tw = ed.scroll.viewport.x - gutter;
        const float frac = std::clamp(tw / std::max(content_w - gutter, 1.0f), 0.05f, 1.0f);
        const float len = std::max(tw * frac, 30.0f);
        const float x = gutter + (tw - len) * (ed.scroll.offset.x / ed.scroll.max_offset.x);
        hbar = (box() | size(len, 4) | bg(mix(th.bg, ink, 0.16f)) | radius(2.0f) | absolute(x, std::max(ed.scroll.viewport.y - 6, 0.0f))).build();
    }
    layers.push_back(std::move(hbar));

    // ── the minimap: the whole file, small, beside the code ─────────────
    // 2 px a row: up to a screenful of minimap; past that it scrolls with
    // the code so the slider stays put relative to it (VS Code's model).
    Node mini;
    ed.metrics.minimap_line_px = 0.0f;
    if (o.minimap && ed.scroll.viewport.y > 0.0f) {
        constexpr float kRowPx = 2.0f, kW = 96.0f;
        const float H = ed.scroll.viewport.y;
        const int fit = std::max(1, static_cast<int>(H / kRowPx));
        const int visible_rows = std::max(1, static_cast<int>(ed.scroll.viewport.y / lh));
        const float frac = ed.scroll.max_offset.y > 0.0f ? std::clamp(sy / ed.scroll.max_offset.y, 0.0f, 1.0f) : 0.0f;
        const int top = rows <= fit ? 0 : static_cast<int>(std::round(frac * static_cast<float>(rows - fit)));
        ed.metrics.minimap_top = top;
        ed.metrics.minimap_line_px = kRowPx;
        // the blocks: rebuilt only when the text, the window onto it or the colours change
        const auto colour_key = [&] {
            std::uint64_t k = 0;
            for (const auto& cl : th.tok) { std::uint32_t u; const float f = cl.c0 + cl.c1 * 3.1f + cl.c2 * 7.3f; std::memcpy(&u, &f, 4); k = memo_mix(k, u); }
            return k;
        };
        const std::uint64_t key = memo_mix(memo_mix(memo_mix(memo_mix(ed.version(), static_cast<std::uint64_t>(top)),
                                                             static_cast<std::uint64_t>(fit)), ed.fold_version()), colour_key());
        Node blocks = memo(key, [&] { return detail::minimap_rows(ed, th, top, fit, kW - 12.0f, kRowPx); });
        const float slider_y = static_cast<float>(static_cast<int>(sy / lh) - top) * kRowPx;
        const bool mhov = c.hovered(part_id(id, part_minimap)) || c.pressed(part_id(id, part_minimap));
        std::vector<Node> m_layers;
        m_layers.push_back((node(std::move(blocks)) | absolute(8, 0)).build());
        for (const auto& s : ed.sels) {   // the carets, as a line across
            const float y = static_cast<float>(vrow(s.head.line) - top) * kRowPx;
            if (y >= 0.0f && y < H) m_layers.push_back((box() | size(kW - 8.0f, 2) | bg(th.cursor.fade(0.8f)) | absolute(4, y)).build());
        }
        if (o.find_matches != nullptr)
            for (const auto& r : *o.find_matches) {
                const float y = static_cast<float>(vrow(r.start.line) - top) * kRowPx;
                if (y >= 0.0f && y < H) m_layers.push_back((box() | size(kW - 8.0f, 2) | bg(th.find_current.fade(0.55f)) | absolute(4, y)).build());
            }
        m_layers.push_back((box() | size(kW, static_cast<float>(visible_rows) * kRowPx)
                            | bg(mix(th.bg, ink, mhov ? 0.16f : 0.11f).fade(mhov ? 0.8f : 0.7f))
                            | inner_shadow(0.0f, mix(th.bg, ink, mhov ? 0.2f : 0.1f), {0.0f, 0.0f})
                            | absolute(0, slider_y) | transition(transition_props::color)).build());
        mini = (z(std::move(m_layers)) | width(kW) | height(pct(100)) | clip
                | bg(th.bg) | inner_shadow(0.0f, mix(th.bg, ink, 0.06f), {1.0f, 0.0f})
                | pointer_cursor | id_of(id, part_minimap)).build();
    }

    // The scroll node: an invisible spacer gives the content its real size
    // so ScrollState measures it; everything visible is absolute.
    auto surface = z(box() | size(content_w, content_h),
                     z(h_list(std::move(layers))) | size(vp_w, vp_h) | absolute(sx, sy))
                 | scroll(ed.scroll, ScrollAxis::both)
                 | grow() | height(pct(100)) | min_size(0, 0) | clip
                 | cursor(CursorShape::text)
                 | role(a11y::Role::text_field) | label(id)
                 | id_of(id);

    return (h(std::move(surface), std::move(mini), std::move(bar)) | width(pct(100)) | grow() | min_size(0, 0) | bg(th.bg)).build();
}

}  // namespace mayag::dsl
