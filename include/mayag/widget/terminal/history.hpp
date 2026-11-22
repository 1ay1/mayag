#pragma once
// mayag::widget terminal/history — reading the scrollback without losing your place
//
//   live_tail(c, id, box, p)        while you read history, the live rows stay pinned at the bottom
//   sticky_command(c, id, box, p)   scrolled into a command's output: its header rides on top
//   history_chip(c, id, box, p)     HIST 120↑ 43%  (or NEW +12 ↓) -- click to go live
//   scrollback_peek(c, id, box, p)  hovering the minimap: the command and lines there
//
// Each is absolute-positioned in the terminal's coordinates (TermBox).
// Interaction: Clicked on node_id(id) -- the tail / the chip return to the
// live view, the sticky header flies to its prompt.

#include "overlays.hpp"
#include "../agent_state.hpp"   // spinner_frame

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mayag::dsl {

/// How a shell command went, as the history widgets show it.
enum class CommandState : std::uint8_t { none, running, ok, failed };

namespace term_detail {
[[nodiscard]] inline Color<Srgb> command_tone(const Theme& t, CommandState s) {
    switch (s) {
        case CommandState::running: return t.accent;
        case CommandState::failed:  return t.danger;
        case CommandState::ok:      return t.success;
        default:                    return mix(t.text_primary, t.background, 0.45f);
    }
}
[[nodiscard]] inline std::string command_tag(CommandState s, int exit_code) {
    switch (s) {
        case CommandState::running: return "RUN";
        case CommandState::failed:  return "EXIT " + std::to_string(exit_code);
        case CommandState::ok:      return "OK";
        default:                    return {};
    }
}
}  // namespace term_detail

// ── the live tail ─────────────────────────────────────────────────────────

struct LiveTailProps {
    const TermGrid* grid = nullptr;   ///< the last `rows` live rows
    TermTheme       theme{};
    TermViewOptions view{};           ///< the main view's options (font, family, marks)
    int             rows = 3;
    float           ease = 1.0f;      ///< 0..1: slides up from the bottom edge
    int             fresh = 0;        ///< lines that arrived since you scrolled away
};

/// The pinned live rows plus their LIVE label. `height` receives how much of
/// the bottom edge they take (for anything that sits above them).
template <ControlContext C>
[[nodiscard]] inline std::vector<Node> live_tail(const C& c, std::string_view id, const TermBox& b, const LiveTailProps& p,
                                                 float* height_out = nullptr) {
    const Theme& t = c.theme;
    const auto ink = t.text_primary, paper = t.background;
    const int n = std::max(p.rows, 1);
    const float full = static_cast<float>(n) * b.cell_h + 2.0f * 4.0f + 1.0f;
    const float e = 1.0f - (1.0f - p.ease) * (1.0f - p.ease);
    const float h_ = full * e;
    if (height_out) *height_out = h_;
    std::vector<Node> out;
    if (p.grid == nullptr || p.ease <= 0.0f) return out;
    TermViewOptions to = p.view;
    to.focused = false; to.cursor_trail = false; to.cursor_glow = 0.0f; to.y_offset = 0.0f; to.padding = 4.0f;
    const bool fresh = p.fresh > 0;
    const auto tone = fresh ? t.success : t.accent;
    const bool hot = c.hovered(node_id(id));
    const std::string grid_id = std::string{id} + ".grid";
    out.push_back((v(node(terminal_view(c, grid_id, *p.grid, p.theme, to)) | width(pct(100)) | height(full - 1.0f))
                   | size(b.size.x - b.right_inset, full) | clip
                   | bg(hot ? mix(paper, ink, 0.07f) : mix(paper, ink, 0.035f))
                   | inner_shadow(0.0f, mix(paper, tone, 0.6f), {0.0f, 1.0f})
                   | pointer_cursor | id_of(id) | absolute(0.0f, b.size.y - h_)).build());
    const float fs = t.type_at(b.font_size, Tier::fine);
    out.push_back((hud_chip(c, {{fresh ? "+" + std::to_string(p.fresh) : "\u2193 click to return", fresh ? tone : mix(ink, paper, 0.4f)}},
                            {.tag = "LIVE", .tone = tone, .size = fs})
                   | absolute(b.padding + 10.0f, b.size.y - h_ - std::round(fs * 1.75f) * 0.5f)).build());
    return out;
}

// ── the sticky command header ─────────────────────────────────────────────

struct StickyCommandProps {
    std::string  line;               ///< the command as typed
    std::string  cwd;                ///< where it ran ("~/src/hand")
    CommandState state = CommandState::ok;
    int          exit_code = 0;
    std::string  time;               ///< "41.2s", or the live elapsed time
    std::int64_t at = 1, span = 1;   ///< the view's top line within its output
    double       now = 0.0;          ///< the spinner's clock
    float        ease = 1.0f;        ///< 0..1: slides down from the top edge
    bool         marks = false;      ///< the terminal has a mark gutter (align with the text)
};

template <ControlContext C>
[[nodiscard]] inline Node sticky_command(const C& c, std::string_view id, const TermBox& b, const StickyCommandProps& p) {
    using term_detail::mono;
    const Theme& t = c.theme;
    const auto ink = t.text_primary, paper = t.background;
    const auto dim = mix(ink, paper, 0.45f), faint = mix(paper, ink, 0.12f);
    const auto tone = term_detail::command_tone(t, p.state);
    const float fs = t.type_at(b.font_size, Tier::body);
    const float hh = std::round(b.cell_h + 8.0f);
    const float w = b.size.x - b.right_inset;
    const float through = static_cast<float>(p.at) / static_cast<float>(std::max<std::int64_t>(p.span, 1));
    std::string tag = term_detail::command_tag(p.state, p.exit_code);
    if (p.state == CommandState::running) tag = std::string{spinner_frame(p.now)} + " " + tag;
    const bool hot = c.hovered(node_id(id));
    std::vector<Node> row;
    row.push_back((h(mono(tag, t.type_at(b.font_size, Tier::fine), paper) | semibold) | height(std::round(fs * 1.5f)) | pad(0, fs * 0.45f) | center | bg(tone)).build());
    if (!p.cwd.empty()) row.push_back(mono(p.cwd, fs, t.accent).build());
    row.push_back(mono("\u276F", fs, t.info).build());
    row.push_back((mono(p.line.empty() ? "(command)" : p.line, fs, ink) | semibold | grow() | ellipsis | min_size(0, 0)).build());
    row.push_back(mono(std::to_string(p.at) + "/" + std::to_string(p.span), t.type_at(b.font_size, Tier::fine), dim).build());
    if (!p.time.empty()) row.push_back(mono(p.time, t.type_at(b.font_size, Tier::fine), p.state == CommandState::running ? tone : dim).build());
    row.push_back(mono(hot ? "\u2191 prompt" : "", t.type_at(b.font_size, Tier::fine), tone).build());
    const float e = 1.0f - (1.0f - p.ease) * (1.0f - p.ease);
    return (v(h(std::move(row)) | gap(10.0f) | align(Align::center) | height(hh - 2.0f) | width(pct(100))
                  | pad(0, b.padding + 10.0f, 0, b.padding + (p.marks ? 10.0f : 0.0f)),
              // how far through its output you are: a hairline in its status colour
              h(box() | size(std::max(w * through, 2.0f), 2.0f) | bg(tone.fade(0.85f))) | width(pct(100)) | height(2.0f) | bg(faint))
            | size(w, hh) | clip | bg(hot ? mix(paper, ink, 0.085f) : mix(paper, ink, 0.05f))
            | opacity(e) | pointer_cursor | id_of(id) | absolute(0.0f, -hh * (1.0f - e))).build();
}

// ── back in history: the status chip ──────────────────────────────────────

struct HistoryChipProps {
    int   rows_up = 0;    ///< how far above the live view
    int   percent = 100;  ///< position through the scrollback
    int   fresh = 0;      ///< new lines below (shows NEW +N instead)
    float lift = 0.0f;    ///< sit above something docked at the bottom
};

/// HIST 120↑ 43% bottom-right, like less/vim -- or NEW +12 ↓ while output streams in.
template <ControlContext C>
[[nodiscard]] inline Node history_chip(const C& c, std::string_view id, const TermBox& b, const HistoryChipProps& p) {
    const Theme& t = c.theme;
    const bool fresh = p.fresh > 0;
    const auto acc = fresh ? t.success : t.warning;
    const float fs = t.type_at(b.font_size, Tier::small);
    std::vector<HudSeg> segs = fresh ? std::vector<HudSeg>{{"+" + std::to_string(p.fresh), acc, true}, {"\u2193", acc}}
                                     : std::vector<HudSeg>{{std::to_string(p.rows_up) + "\u2191", t.text_primary, true},
                                                           {std::to_string(std::clamp(p.percent, 0, 100)) + "%", mix(t.text_primary, t.background, 0.45f)}};
    const std::string tag = fresh ? "NEW" : "HIST";
    std::size_t chars = 0;
    for (const auto& s : segs) chars += term_detail::cells(s.text) + 1;
    const float cw = detail::term_cell_width(c, fs, b.family);
    const float pw = (static_cast<float>(chars + tag.size()) + 2.0f) * cw + fs * 0.6f * 3.0f;
    const float ph = std::round(fs * 1.75f);
    return (hud_chip(c, segs, {.tag = tag, .tone = acc, .size = fs, .hot = c.hovered(node_id(id))})
            | pointer_cursor | id_of(id) | absolute(b.size.x - pw - b.right_inset - 6.0f, b.size.y - ph - 6.0f - p.lift)).build();
}

// ── the peek: a window into the scrollback, from the minimap ──────────────

struct PeekProps {
    TermGrid        grid;              ///< the rows around the hovered line
    TermTheme       theme{};
    TermViewOptions view{};
    int             rows = 7;
    int             cols = 72;
    std::int64_t    line = 0, total = 0;
    std::string     command;           ///< the command that made that output ("" = none)
    CommandState    state = CommandState::none;
    int             exit_code = 0;
    std::int64_t    duration_ms = 0;
    float           hover_y = 0.0f;    ///< the pointer on the minimap
    float           ease = 1.0f;
};

/// A card left of the minimap: the command, how it went, and the lines
/// themselves (the hovered one highlighted), with a hairline to the pointer.
template <ControlContext C>
[[nodiscard]] inline std::vector<Node> scrollback_peek(const C& c, std::string_view id, const TermBox& b, const PeekProps& p) {
    const Theme& t = c.theme;
    const auto ink = t.text_primary, paper = t.background, dim = mix(ink, paper, 0.45f);
    const float pfs = t.type_at(b.font_size, Tier::small);
    const float pcw = detail::term_cell_width(c, pfs, p.view.family);
    const float pch = std::round(pfs * p.view.line_height);
    const float cw_ = static_cast<float>(p.cols) * pcw + 10.0f + 12.0f;
    TermViewOptions po = p.view;
    po.font_size = pfs; po.focused = false; po.cursor_trail = false; po.cursor_glow = 0.0f;
    po.y_offset = 0.0f; po.padding = 5.0f; po.dim_unfocused = 0.0f;
    std::vector<HudSeg> head;
    std::string tag = "L" + std::to_string(p.line + 1);
    Color<Srgb> tone = dim;
    if (p.state != CommandState::none) {
        std::string cmd = p.command.empty() ? std::string{"(command)"} : p.command;
        const std::size_t max_chars = static_cast<std::size_t>(std::max(p.cols - 18, 12));
        if (cmd.size() > max_chars) cmd = cmd.substr(0, max_chars - 1) + "\u2026";
        tone = term_detail::command_tone(t, p.state);
        tag = term_detail::command_tag(p.state, p.exit_code);
        head.push_back({"\u276F " + cmd, ink, true});
        if (p.state != CommandState::running && p.duration_ms > 0) {
            char d[24];
            std::snprintf(d, sizeof d, "%.1fs", static_cast<double>(p.duration_ms) / 1000.0);
            head.push_back({d, dim});
        }
    } else {
        head.push_back({"line " + std::to_string(p.line + 1) + " of " + std::to_string(p.total), dim});
    }
    const float card_h = std::round(pfs * 1.75f) + static_cast<float>(p.rows) * pch + 10.0f;
    const float e = 1.0f - (1.0f - p.ease) * (1.0f - p.ease);
    const float cx = b.size.x - b.right_inset - cw_ - 8.0f + (1.0f - e) * 10.0f;   // slides in from the minimap
    const float cy = std::clamp(p.hover_y - card_h * 0.5f, 6.0f, b.size.y - card_h - 6.0f);
    std::vector<Node> out;
    out.push_back((v(hud_chip(c, head, {.tag = tag, .tone = tone, .size = pfs}) | width(pct(100)),
                     node(terminal_view(c, std::string{id} + ".grid", p.grid, p.theme, po)) | width(pct(100)) | grow())
                   | size(cw_, card_h) | clip | bg(paper) | border(1.0f, mix(paper, tone, 0.55f))
                   | opacity(e) | id_of(id) | absolute(cx, cy)).build());
    out.push_back((box() | size(8.0f * e, 1.0f) | bg(mix(paper, tone, 0.7f))
                   | absolute(b.size.x - b.right_inset - 8.0f, std::clamp(p.hover_y, cy, cy + card_h))).build());
    return out;
}

}  // namespace mayag::dsl
