#pragma once
// mayag::widget terminal/overlays — what a terminal draws over its own cells
//
//   TermBox                         where the cells are (a terminal_view's geometry)
//   copy_flash(c, box, spans, a)    the copied text lights up and fades
//   copy_cursor(c, box, props)      copy mode: its own cursor and a mode line
//   selection_chip(c, id, box, p)   SEL 1L 21ch  copy  find  open  ×  beside a selection
//   hint_bar(c, id, props)          HINT 12 targets  a-z copy/open  A-Z type  esc
//   bell_shake(node, t)             the damped wobble of a visual bell
//
// All of it is absolute-positioned in the terminal's coordinates, built from
// the theme (accent, success, warning, info, danger) and terminal type: mono,
// square, a hairline -- a terminal's vocabulary, not a web page's.
//
// Interaction: Clicked on node_id(id + ".copy" / ".find" / ".open" / ".clear").

#include "../control.hpp"
#include "../hud.hpp"
#include "../terminal_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace mayag::dsl {

/// Where a terminal_view's cells are, in its own coordinates.
struct TermBox {
    Vec2  size{};
    float origin_x = 0.0f, origin_y = 0.0f;   ///< the first cell's top-left
    float cell_w = 8.0f, cell_h = 16.0f;
    float y_offset = 0.0f;                    ///< a smooth-scroll glide in progress
    float font_size = 13.0f;
    float padding = 8.0f;
    float right_inset = 0.0f;                 ///< something docked on the right edge (a minimap)
    FontFamily family = FontFamily::mono;
    /// The top of view row `r` (fractional rows allowed).
    [[nodiscard]] float row_y(float r) const noexcept { return origin_y + y_offset + r * cell_h; }
    [[nodiscard]] float col_x(float col) const noexcept { return origin_x + col * cell_w; }
};

namespace term_detail {
[[nodiscard]] inline auto mono(std::string s, float size, Color<Srgb> col) {
    return text_owned(std::move(s)) | font(size) | monospace | fg(col);
}
/// Width of `s` in cells (codepoints, not bytes).
[[nodiscard]] inline std::size_t cells(std::string_view s) {
    std::size_t n = 0;
    for (unsigned char b : s) n += (b & 0xC0) != 0x80 ? 1 : 0;
    return n;
}
}  // namespace term_detail

/// What you copied lights up in the accent and fades out (`a` 1 -> 0).
template <ControlContext C>
[[nodiscard]] inline std::vector<Node> copy_flash(const C& c, const TermBox& b, const std::vector<TermSpan>& spans, float a) {
    std::vector<Node> out;
    for (const auto& sp : spans)
        out.push_back((box() | size(static_cast<float>(sp.len) * b.cell_w, b.cell_h) | radius(2.0f)
                       | bg(c.theme.accent.fade(0.55f * a * a))
                       | absolute(b.col_x(static_cast<float>(sp.col)), b.row_y(static_cast<float>(sp.row)))).build());
    return out;
}

/// Copy mode, vim-style: the mode's cursor and its mode line.
struct CopyCursorProps {
    float       row = 0.0f;            ///< view row of the copy cursor (may be off screen)
    int         col = 0;
    int         line = 0;              ///< absolute line, for the readout (0-based)
    int         visual = 0;            ///< 0 none, 1 v (chars), 2 V (lines), 3 ^v (block)
    int         sel_lines = 0, sel_chars = 0;
    float       lift = 0.0f;           ///< raise the mode line above something docked at the bottom
};

template <ControlContext C>
[[nodiscard]] inline std::vector<Node> copy_cursor(const C& c, const TermBox& b, const CopyCursorProps& p) {
    const Theme& t = c.theme;
    std::vector<Node> out;
    const float y = b.row_y(p.row);
    if (y > -b.cell_h && y < b.size.y)
        out.push_back((box() | size(b.cell_w, b.cell_h) | bg(t.warning.fade(0.35f)) | border(2.0f, t.warning)
                       | absolute(b.col_x(static_cast<float>(p.col)), y)).build());
    const float fs = b.font_size * 0.85f;
    const auto dim = mix(t.text_primary, t.background, 0.45f);
    const char* mode = p.visual == 1 ? "VISUAL" : p.visual == 2 ? "V-LINE" : p.visual == 3 ? "V-BLOCK" : "COPY";
    std::vector<HudSeg> segs{{"L" + std::to_string(p.line + 1) + ":" + std::to_string(p.col + 1), t.text_primary, true}};
    if (p.visual != 0) segs.push_back({std::to_string(p.sel_lines) + "L " + std::to_string(p.sel_chars) + "ch", t.warning});
    segs.push_back({p.visual ? "y yank  o other end  esc" : "hjkl w b e 0 $ { }  v V ^v  y yank  / find  q", dim});
    out.push_back((hud_chip(c, segs, {.tag = mode, .tone = p.visual ? t.info : t.warning, .size = fs})
                   | absolute(b.padding + 10.0f, b.size.y - std::round(fs * 1.75f) - 6.0f - p.lift)).build());
    return out;
}

/// What a selection is, for the chip beside it.
struct SelectionChipProps {
    int   lines = 0, chars = 0;
    bool  block = false;
    bool  openable = false;   ///< a URL or a path: offer "open"
    int   c0 = 0, c1 = 0;     ///< its first / last column
    float row0 = 0.0f, row1 = 0.0f;   ///< view rows of its first / last line
    int   end_used = 0;       ///< non-blank cells on its last line (the chip goes after them)
};

/// SEL 1L 21ch  copy  find  open  ×  -- beside the selection's end when the
/// row has room (covering no text), else under it, or above its start.
/// `placed` receives where it went (empty when off screen).
template <ControlContext C>
[[nodiscard]] inline Node selection_chip(const C& c, std::string_view id, const TermBox& b, const SelectionChipProps& p,
                                         Rect* placed = nullptr) {
    using term_detail::mono;
    const Theme& t = c.theme;
    const auto ink = t.text_primary, paper = t.background, dim = mix(ink, paper, 0.45f);
    const float fs = b.font_size * 0.85f;
    const float fcw = detail::term_cell_width(c, fs, b.family);
    const std::string base{id};
    const std::string size_s = std::to_string(p.lines) + "L " + std::to_string(p.chars) + "ch" + (p.block ? " \u25A6" : "");
    const auto button = [&](const char* part, const char* label_, Color<Srgb> tone) {
        const std::string nid = base + "." + part;
        const bool hot = c.hovered(node_id(nid));
        return (h(mono(label_, fs, hot ? paper : tone)) | height(pct(100)) | pad(0, fs * 0.55f) | center
                | bg(hot ? tone : tone.fade(0.0f)) | pointer_cursor | id_of(nid)).build();
    };
    std::vector<Node> parts;
    parts.push_back((h(mono("SEL", fs, paper) | semibold) | height(pct(100)) | pad(0, fs * 0.5f) | center | bg(t.accent)).build());
    parts.push_back((h(mono(size_s, fs, ink)) | height(pct(100)) | pad(0, fs * 0.5f) | center).build());
    parts.push_back(button("copy", "copy", t.success));
    parts.push_back(button("find", "find", t.warning));
    if (p.openable) parts.push_back(button("open", "open", t.info));
    parts.push_back(button("clear", "\u00D7", dim));
    const float chip_h = std::round(fs * 1.75f);
    const std::size_t chars = 3 + term_detail::cells(size_s) + 4 + 4 + (p.openable ? 4 : 0) + 1;
    const float chip_w = static_cast<float>(chars) * fcw + fs * (1.0f + 1.0f + 1.1f * static_cast<float>(p.openable ? 4 : 3));
    const float after = b.col_x(static_cast<float>(std::max(p.end_used, p.c1 + 1) + 2));
    const float right_edge = b.size.x - b.right_inset - 6.0f;
    float x, y;
    if (after + chip_w < right_edge) {
        x = after;
        y = b.row_y(p.row1) + (b.cell_h - chip_h) * 0.5f;
    } else {
        const float below = b.row_y(p.row1) + b.cell_h + 4.0f, above = b.row_y(p.row0) - chip_h - 4.0f;
        y = below + chip_h < b.size.y - 4.0f ? below : above;
        x = std::clamp(b.col_x(static_cast<float>(p.block || p.lines == 1 ? p.c0 : 0)), 4.0f, std::max(right_edge - chip_w, 4.0f));
    }
    if (placed) *placed = Rect{};
    if (y <= -chip_h || y >= b.size.y) return Node{};
    if (placed) *placed = Rect{Vec2{x, y}, Vec2{chip_w, chip_h}};
    return (h(std::move(parts)) | align(Align::center) | height(chip_h)
            | bg(mix(paper, ink, 0.07f)) | border(1.0f, mix(paper, t.accent, 0.55f)) | absolute(x, y) | id_of(base)).build();
}

/// Hint mode's status line, bottom-left: what can be picked, what the keys do.
struct HintBarProps {
    std::size_t count = 0;
    std::string typed;
    float       font_size = 13.0f;
    float       y = 0.0f;   ///< window y of the bar's top
};

template <ControlContext C>
inline void hint_bar(const C& c, std::string_view id, const HintBarProps& p) {
    const Theme& t = c.theme;
    const auto dim = mix(t.text_primary, t.background, 0.45f);
    const std::string n = std::to_string(p.count) + " targets";
    c.overlay(Overlay{.content = hud_chip(c, {{p.typed.empty() ? n : p.typed + "\u258F", t.text_primary, true},
                                              {"a-z copy/open", dim}, {"A-Z type", dim}, {"esc", dim}},
                                          {.tag = "HINT", .tone = t.warning, .size = p.font_size * 0.85f}).build(),
                      .id = node_id(id), .placement = Placement::cursor, .dismiss = Dismiss::never,
                      .offset = 0.0f, .layer = 5, .at = Vec2{16.0f, p.y}, .passthrough = true});
}

/// A visual bell's shake: a damped wobble over 0.4 s (`t` seconds since the bell).
[[nodiscard]] inline Node bell_shake(Node n, float t) {
    if (t < 0.0f || t >= 0.4f) return n;
    const float k = (1.0f - t / 0.4f) * (1.0f - t / 0.4f);
    return (node(std::move(n)) | translate(7.0f * k * std::sin(t * 62.0f), 2.0f * k * std::sin(t * 47.0f))).build();
}

}  // namespace mayag::dsl
