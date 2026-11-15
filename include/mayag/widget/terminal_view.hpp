#pragma once
// mayag::widget terminal_view — draws a terminal's cell grid
//
//   TermGrid g = my_emulator.snapshot();          // the app's emulator fills it
//   terminal_view(c, "term", g, TermTheme::from(t), {.focused = true})
//
// UI only: this header knows nothing about escape sequences or ptys. It
// draws a grid of styled cells, the way Ghostty/WezTerm/Kitty draw theirs:
//
//   * cells on an exact monospace grid (cell size measured from the real
//     font), runs of equal style merged into one text node per row segment
//   * fg/bg per cell, bold / dim / italic / strike / inverse / hidden,
//     underline styles (single, double, curly, dotted, dashed) in their own
//     colour, wide (2-cell) characters
//   * cursor: block / beam / underline, hollow when the pane is unfocused,
//     drawn over the cell with the glyph re-inverted under a block
//   * selection, search matches (current one emphasised), quick-select hint
//     labels, the hovered hyperlink underlined, IME preedit at the cursor
//   * prompt marks in the left margin (shell integration), a visual-bell
//     flash, dimming of an unfocused split
//
// Interaction (translate with term_hit):
//   Pressed / Dragged on node_id(id) -> term_hit(g, metrics, local) -> cell
//   Scrolled on node_id(id)          -> the app scrolls its viewport

#include "control.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace mayag::dsl {

// ── the data the widget draws ───────────────────────────────────────────

namespace term_attr {
inline constexpr std::uint16_t bold = 1 << 0, dim = 1 << 1, italic = 1 << 2, inverse = 1 << 3,
                               hidden = 1 << 4, strike = 1 << 5, overline = 1 << 6, link = 1 << 7;
}
enum class TermUnderline : std::uint8_t { none, single, double_, curly, dotted, dashed };
enum class TermCursorShape : std::uint8_t { block, beam, underscore };
enum class TermMark : std::uint8_t { none, prompt, ok, error, running };

struct TermCell {
    std::string   text = " ";          ///< UTF-8 grapheme (may carry combining marks); "" for a wide char's right half
    std::uint32_t fg = 0, bg = 0;      ///< 0xRRGGBB; 0 with `default_fg/bg` set means the theme default
    bool          default_fg = true, default_bg = true;
    std::uint16_t flags = 0;
    TermUnderline ul_style = TermUnderline::none;
    std::uint32_t ul_color = 0;        ///< 0: same as fg
    std::uint8_t  width = 1;           ///< 2 left half of a wide char, 0 right half
    std::uint16_t link = 0;            ///< hyperlink id, 0 none
};

struct TermRowView {
    std::vector<TermCell> cells;
    TermMark              mark = TermMark::none;   ///< shell-integration mark drawn in the margin
};

struct TermSpan { int row = 0, col = 0, len = 1; };     ///< a run of cells on one visible row

struct TermGrid {
    int cols = 80, rows = 24;
    std::vector<TermRowView> lines;    ///< `rows` visible rows, top to bottom
    // cursor
    int             cursor_row = 0, cursor_col = 0;
    bool            cursor_visible = true;
    TermCursorShape cursor_shape = TermCursorShape::block;
    // overlays, all in visible-row coordinates
    std::vector<TermSpan> selection;   ///< selected runs
    std::vector<TermSpan> matches;     ///< search matches
    int                   current_match = -1;
    struct HintLabel { TermSpan at; std::string label; };
    std::vector<HintLabel> hints;      ///< quick select
    std::string            hint_typed; ///< prefix already typed: matching labels highlight it
    std::uint16_t          hover_link = 0;
    std::string            preedit;    ///< IME composition at the cursor
};

/// Colours of the terminal surface.
struct TermTheme {
    std::uint32_t fg = 0xC0CAF5, bg = 0x1A1B26, cursor = 0xC0CAF5, cursor_text = 0x1A1B26;
    std::uint32_t selection_bg = 0x33467C, selection_fg = 0;   ///< 0: keep the cell's fg
    std::uint32_t match_bg = 0x3D59A1, match_current_bg = 0xE0AF68, match_current_fg = 0x1A1B26;
    std::uint32_t hint_bg = 0xE0AF68, hint_fg = 0x1A1B26, hint_typed_fg = 0x8C4351;
    std::uint32_t mark_prompt = 0x565F89, mark_ok = 0x9ECE6A, mark_error = 0xF7768E, mark_running = 0x7AA2F7;
    std::uint32_t link = 0x7DCFFF;
    float         bold_brighten = 0.0f;   ///< >0: bold text is mixed this far toward white
};

struct TermViewOptions {
    float font_size = 13.0f;
    float line_height = 1.25f;      ///< multiple of the font size
    float padding = 8.0f;
    bool  focused = true;
    bool  cursor_on = true;         ///< blink phase (the app's clock)
    float dim_unfocused = 0.0f;     ///< 0..1 darken when not focused (splits)
    float bell = 0.0f;              ///< 0..1 visual-bell flash strength
    bool  marks = true;             ///< draw shell-integration marks in a margin
    float opacity = 1.0f;           ///< background opacity (translucent windows)
};

/// Geometry of the last draw, for hit testing and for sizing the pty
/// (cols/rows that fit). Written by the view, like ScrollState.
struct TermMetrics {
    float cell_w = 8.0f, cell_h = 16.0f;
    float origin_x = 8.0f, origin_y = 8.0f;   ///< first cell, relative to the widget
    int   fit_cols = 80, fit_rows = 24;       ///< what the widget's size can hold
};

[[nodiscard]] constexpr Color<Srgb> rgb_of(std::uint32_t v, float a = 1.0f) noexcept {
    return Color<Srgb>{static_cast<float>((v >> 16) & 255) / 255.0f, static_cast<float>((v >> 8) & 255) / 255.0f,
                       static_cast<float>(v & 255) / 255.0f, a};
}

/// A local point (relative to the widget) to the cell under it.
[[nodiscard]] inline TermSpan term_hit(const TermGrid& g, const TermMetrics& m, Vec2 local) {
    const int col = std::clamp(static_cast<int>(std::floor((local.x - m.origin_x) / m.cell_w)), 0, std::max(g.cols - 1, 0));
    const int row = std::clamp(static_cast<int>(std::floor((local.y - m.origin_y) / m.cell_h)), 0, std::max(g.rows - 1, 0));
    return {row, col, 1};
}

namespace detail {
template <class C>
[[nodiscard]] float term_cell_width(const C& c, float size) {
    TextStyle st; st.size = size;
    if constexpr (requires { c.c.measurer(); }) { const float w = c.c.measurer().advance("MMMMMMMMMM", st) / 10.0f; if (w > 0) return w; }
    else if constexpr (requires { c.measurer(); }) { const float w = c.measurer().advance("MMMMMMMMMM", st) / 10.0f; if (w > 0) return w; }
    return size * 0.6f;
}
[[nodiscard]] inline bool in_spans(const std::vector<TermSpan>& v, int row, int col, int* which = nullptr) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].row == row && col >= v[i].col && col < v[i].col + v[i].len) { if (which) *which = static_cast<int>(i); return true; }
    return false;
}
}  // namespace detail

template <ControlContext C>
[[nodiscard]] inline Node terminal_view(const C& c, std::string_view id, const TermGrid& g, const TermTheme& th,
                                        const TermViewOptions& o, TermMetrics* out_metrics = nullptr) {
    const float cw = detail::term_cell_width(c, o.font_size);
    const float ch = std::round(o.font_size * o.line_height);
    const float margin = o.marks ? 10.0f : 0.0f;
    const float ox = o.padding + margin, oy = o.padding;
    if (out_metrics != nullptr) {
        out_metrics->cell_w = cw; out_metrics->cell_h = ch;
        out_metrics->origin_x = ox; out_metrics->origin_y = oy;
    }
    const auto col_x = [&](int col) { return ox + static_cast<float>(col) * cw; };
    const auto row_y = [&](int row) { return oy + static_cast<float>(row) * ch; };
    const auto bright = [&](Color<Srgb> col, bool is_bold) {
        return is_bold && th.bold_brighten > 0.0f ? mix(col, colors::white, th.bold_brighten) : col;
    };

    std::vector<Node> layers;
    const auto rect = [&](float x, float y, float w, float h, Color<Srgb> col, float r = 0.0f) {
        layers.push_back((box() | size(std::max(w, 0.0f), std::max(h, 0.0f)) | bg(col) | radius(r) | absolute(x, y)).build());
    };

    const int nrows = std::min(g.rows, static_cast<int>(g.lines.size()));
    for (int r = 0; r < nrows; ++r) {
        const auto& row = g.lines[static_cast<std::size_t>(r)];
        const int ncols = std::min(g.cols, static_cast<int>(row.cells.size()));
        const float y = row_y(r);

        // ── shell-integration mark ──
        if (o.marks && row.mark != TermMark::none) {
            const std::uint32_t mc = row.mark == TermMark::error ? th.mark_error : row.mark == TermMark::ok ? th.mark_ok
                                   : row.mark == TermMark::running ? th.mark_running : th.mark_prompt;
            rect(o.padding, y + 2, 3, ch - 4, rgb_of(mc), 1.5f);
        }

        // ── backgrounds: merged runs of equal colour ──
        int run_start = 0;
        Color<Srgb> run_bg{};
        bool run_on = false;
        const auto cell_bg = [&](int col) -> std::pair<bool, Color<Srgb>> {
            const TermCell& cell = row.cells[static_cast<std::size_t>(col)];
            int mi = -1;
            if (detail::in_spans(g.matches, r, col, &mi))
                return {true, rgb_of(mi == g.current_match ? th.match_current_bg : th.match_bg)};
            if (detail::in_spans(g.selection, r, col)) return {true, rgb_of(th.selection_bg)};
            const bool inv = (cell.flags & term_attr::inverse) != 0;
            const bool def = inv ? cell.default_fg : cell.default_bg;
            if (def && !inv) return {false, {}};
            return {true, rgb_of(inv ? (cell.default_fg ? th.fg : cell.fg) : cell.bg)};
        };
        for (int col = 0; col <= ncols; ++col) {
            const auto [has, colr] = col < ncols ? cell_bg(col) : std::pair<bool, Color<Srgb>>{false, {}};
            const bool same = has == run_on && (!has || colr == run_bg);
            if (!same) {
                if (run_on) rect(col_x(run_start), y, static_cast<float>(col - run_start) * cw, ch, run_bg);
                run_start = col; run_bg = colr; run_on = has;
            }
        }

        // ── glyphs: merged runs of equal style ──
        std::vector<Node> runs;
        int c0 = 0;
        while (c0 < ncols) {
            const TermCell& a = row.cells[static_cast<std::size_t>(c0)];
            const auto fg_of = [&](const TermCell& cell, int col) {
                int mi = -1;
                if (detail::in_spans(g.matches, r, col, &mi) && mi == g.current_match) return rgb_of(th.match_current_fg);
                if (detail::in_spans(g.selection, r, col) && th.selection_fg != 0) return rgb_of(th.selection_fg);
                const bool inv = (cell.flags & term_attr::inverse) != 0;
                Color<Srgb> f = inv ? (cell.default_bg ? rgb_of(th.bg) : rgb_of(cell.bg))
                                    : (cell.default_fg ? rgb_of(th.fg) : rgb_of(cell.fg));
                f = bright(f, (cell.flags & term_attr::bold) != 0);
                if ((cell.flags & term_attr::dim) != 0) f = f.fade(0.6f);
                if (cell.link != 0 && cell.link == g.hover_link) f = rgb_of(th.link);
                return f;
            };
            const Color<Srgb> fa = fg_of(a, c0);
            int c1 = c0 + 1;
            while (c1 < ncols) {
                const TermCell& b = row.cells[static_cast<std::size_t>(c1)];
                if (b.flags != a.flags || b.ul_style != a.ul_style || b.link != a.link || !(fg_of(b, c1) == fa) ||
                    b.width == 2 || a.width == 2) break;
                ++c1;
            }
            // Text of the run; skip right halves of wide characters.
            std::string text;
            bool visible = false;
            for (int k = c0; k < c1; ++k) {
                const TermCell& cell = row.cells[static_cast<std::size_t>(k)];
                if (cell.width == 0) continue;
                text += cell.text.empty() ? std::string{" "} : cell.text;
                if (cell.text != " " && !cell.text.empty()) visible = true;
            }
            const bool hidden = (a.flags & term_attr::hidden) != 0;
            if (visible && !hidden) {
                auto t = text_owned(text) | font(o.font_size) | fg(fa) | no_ligatures | clip_text
                       | (((a.flags & term_attr::bold) != 0) ? bold : weight(FontWeight::regular));
                Node n = t.build();
                if ((a.flags & term_attr::italic) != 0) n.style().text.italic = true;
                if ((a.flags & term_attr::strike) != 0) n.style().text.strikethrough = true;
                // A wide char's node spans both of its cells.
                const float w = static_cast<float>((a.width == 2 ? 2 : c1 - c0)) * cw;
                layers.push_back((h(node(std::move(n))) | size(w + cw, ch) | align(Align::center)
                                  | absolute(col_x(c0), y)).build());
            }
            // Decorations.
            const Color<Srgb> ulc = a.ul_color != 0 ? rgb_of(a.ul_color) : fa;
            const float run_w = static_cast<float>(c1 - c0) * cw;
            const bool link_hover = a.link != 0 && a.link == g.hover_link;
            const TermUnderline ul = link_hover ? TermUnderline::single : a.ul_style;
            if (ul != TermUnderline::none) {
                const float uy = y + ch - std::max(2.0f, ch * 0.12f);
                switch (ul) {
                    case TermUnderline::double_:
                        rect(col_x(c0), uy - 2, run_w, 1, ulc); rect(col_x(c0), uy + 1, run_w, 1, ulc); break;
                    case TermUnderline::curly:
                        // A zig-zag of short dashes alternating height reads as a wave at text size.
                        for (float x = 0; x < run_w; x += 4.0f)
                            rect(col_x(c0) + x, uy + (static_cast<int>(x / 4.0f) % 2 ? 1.0f : -1.0f), 4.0f, 1.3f, ulc);
                        break;
                    case TermUnderline::dotted:
                        for (float x = 0; x < run_w; x += 3.0f) rect(col_x(c0) + x, uy, 1.5f, 1.5f, ulc);
                        break;
                    case TermUnderline::dashed:
                        for (float x = 0; x < run_w; x += 6.0f) rect(col_x(c0) + x, uy, 4.0f, 1.2f, ulc);
                        break;
                    default: rect(col_x(c0), uy, run_w, 1.2f, ulc); break;
                }
            }
            if ((a.flags & term_attr::overline) != 0) rect(col_x(c0), y + 1, run_w, 1, fa);
            c0 = c1;
        }
        (void)runs;
    }

    // ── cursor ──
    if (g.cursor_visible && g.cursor_row >= 0 && g.cursor_row < nrows && (o.cursor_on || !o.focused)) {
        const float x = col_x(g.cursor_col), y = row_y(g.cursor_row);
        const TermCell* under = g.cursor_col < static_cast<int>(g.lines[static_cast<std::size_t>(g.cursor_row)].cells.size())
                                    ? &g.lines[static_cast<std::size_t>(g.cursor_row)].cells[static_cast<std::size_t>(g.cursor_col)] : nullptr;
        const float w = (under && under->width == 2 ? 2.0f : 1.0f) * cw;
        const Color<Srgb> cc = rgb_of(th.cursor);
        if (!o.focused) {   // hollow box: the pane is not receiving keys
            layers.push_back((box() | size(w, ch) | border(1.2f, cc) | absolute(x, y)).build());
        } else if (g.cursor_shape == TermCursorShape::beam) {
            rect(x, y + 1, 2, ch - 2, cc);
        } else if (g.cursor_shape == TermCursorShape::underscore) {
            rect(x, y + ch - 3, w, 2.5f, cc);
        } else {
            rect(x, y, w, ch, cc);
            if (under != nullptr && !under->text.empty() && under->text != " ")
                layers.push_back((h(text_owned(under->text) | font(o.font_size) | fg(rgb_of(th.cursor_text)) | no_ligatures
                                    | (((under->flags & term_attr::bold) != 0) ? bold : weight(FontWeight::regular)))
                                  | size(w + cw, ch) | align(Align::center) | absolute(x, y)).build());
        }
        // IME preedit: underlined composition at the cursor.
        if (!g.preedit.empty()) {
            layers.push_back((h(text_owned(g.preedit) | font(o.font_size) | fg(rgb_of(th.fg)) | underline)
                              | height(ch) | align(Align::center) | pad(0, 2) | bg(rgb_of(th.bg))
                              | border(1, rgb_of(th.cursor)) | absolute(x, y)).build());
        }
    }

    // ── quick-select hint labels ──
    for (const auto& hnt : g.hints) {
        const bool typed = !g.hint_typed.empty() && hnt.label.rfind(g.hint_typed, 0) == 0;
        if (!g.hint_typed.empty() && !typed) continue;
        std::vector<Node> parts;
        if (typed) parts.push_back((text_owned(g.hint_typed) | font(o.font_size * 0.92f) | bold | fg(rgb_of(th.hint_typed_fg))).build());
        parts.push_back((text_owned(hnt.label.substr(typed ? g.hint_typed.size() : 0)) | font(o.font_size * 0.92f) | bold
                         | fg(rgb_of(th.hint_fg))).build());
        layers.push_back((h(h_list(std::move(parts))) | height(ch) | align(Align::center) | pad(0, 3)
                          | bg(rgb_of(th.hint_bg)) | radius(3) | absolute(col_x(hnt.at.col), row_y(hnt.at.row))).build());
    }

    if (o.bell > 0.0f) rect(0, 0, 1e5f, 1e5f, colors::white.fade(0.12f * std::clamp(o.bell, 0.0f, 1.0f)));
    if (!o.focused && o.dim_unfocused > 0.0f) rect(0, 0, 1e5f, 1e5f, colors::black.fade(0.35f * o.dim_unfocused));

    // Fit: how many cells the given size holds. The view does not know its
    // size until layout, so it reports from the last measured frame via the
    // app's Tracked rect; here only the cell metrics are known.
    return (z(h_list(std::move(layers))) | grow() | width(pct(100)) | height(pct(100)) | min_size(0, 0) | clip
            | bg(rgb_of(th.bg, o.opacity)) | cursor(CursorShape::text)
            | role(a11y::Role::text_field) | label("terminal") | id_of(id)).build();
}

/// How many columns/rows fit a pane of `size` (logical px), for resizing the
/// pty after a layout change.
[[nodiscard]] inline std::pair<int, int> term_fit(Vec2 size, const TermMetrics& m, bool marks = true, float padding = 8.0f) {
    const float margin = marks ? 10.0f : 0.0f;
    const int cols = std::max(2, static_cast<int>((size.x - 2.0f * padding - margin) / m.cell_w));
    const int rows = std::max(1, static_cast<int>((size.y - 2.0f * padding) / m.cell_h));
    return {cols, rows};
}

}  // namespace mayag::dsl
