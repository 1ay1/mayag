#pragma once
// mayag::widget tab_strip — terminal tabs that show what each tab is doing
//
//   tab_strip(c, "tabs", tabs, active, &m.tab_motion, time, {.inset_left = 78})
//
// A terminal tab is a window onto a running process, so the tab says what
// that process is up to, without switching to it:
//
//   * a process icon in the tab's own colour (stable, derived from its cwd,
//     so the same project is always the same colour)
//   * a STATUS RING around the icon: a turning arc while a command runs, a
//     filling ring when the program reports progress (OSC 9;4), red on error
//   * an OUTPUT SPARKLINE: how much output arrived over the last seconds, so
//     the tab that is spewing logs (or has gone quiet) is visible at a glance
//   * a RESULT FLASH: the tab glows green / red for a few seconds when a
//     background command finishes
//   * two lines: title on top; cwd or ssh host below, elapsed time while
//     running
//   * the active tab is a lifted pill that SLIDES between tabs (spring
//     motion following real geometry), with a short colour bar under it
//   * unread-output dot, bell, last exit code, ⌘N hints while ⌘ is held,
//     close on hover, pinned (icon-only) tabs; tabs share the width equally
//     and shrink before they scroll
//
// Motion: keep a TabStripMotion in the model, pass its address, and while
// `motion.animating()` subscribe to frames (Sub::on_frame) so the pill moves.
//
// Interaction:
//   Clicked on part_id(id, i)                  -> activate tab i (middle button: close)
//   Clicked on part_id(id, tab_part_close + i) -> close tab i
//   Clicked on part_id(id, part_inc)           -> new tab
//   Clicked on part_id(id, part_menu)          -> new-tab menu (tab_launcher)
//   Entered / Left on part_id(id, i)           -> hover, for tab_peek

#include "control.hpp"
#include "agent_state.hpp"   // spinner_frame
#include "../core/motion.hpp"
#include "../render/draw_list.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace mayag::dsl {

struct TermTabInfo {
    std::string title;                 ///< what the shell set (OSC 0/2), or the process
    std::string cwd;                   ///< "~/src/mayag"
    std::string process;               ///< foreground process: "zsh", "nvim", "cargo", "ssh"
    std::string host;                  ///< remote host for ssh tabs ("" = local)
    bool        host_danger = false;   ///< a production machine: the tab says so in red
    bool        running = false;       ///< a command is executing (shell integration)
    double      elapsed = 0.0;         ///< seconds it has been running
    int         progress = -1;         ///< 0..100 from OSC 9;4, -1 none
    bool        progress_error = false;
    int         last_exit = -1;        ///< last command's status, -1 unknown
    double      finished_ago = 1e9;    ///< seconds since it finished (drives the flash)
    bool        bell = false;
    bool        unread = false;        ///< output arrived since the tab was last viewed
    bool        dirty = false;         ///< a file tab with unsaved edits: a dot, even when active
    std::vector<float> activity;       ///< recent output rate per sample, 0..1, oldest first
    std::uint32_t hue = 0;             ///< 0xRRGGBB override; 0 = derived from cwd
    bool        pinned = false;        ///< icon-only
};

struct TabStripMotion {
    Tracked pill;    ///< the active tab's rect
    Tracked strip;   ///< the strip's own rect (pill coordinates are relative to it)
    [[nodiscard]] bool animating() const noexcept { return pill.animating(); }
};

/// How the active tab is marked.
enum class TabStripStyle : std::uint8_t {
    pill,        ///< a lifted card that slides between tabs (default)
    underline,   ///< flat tabs; a bar slides under the active one
    flat,        ///< a filled block with an accent edge on top, no lift
    line,        ///< tmux-like: one mono line, square cells, numbered, no curves (tab_line)
};
/// How the active-tab marker moves to a newly selected tab.
enum class TabMotion : std::uint8_t {
    stretch,   ///< the leading edge races ahead, the trailing edge catches up (elastic)
    slide,     ///< the marker glides as one piece
    instant,   ///< it jumps
};
/// When a tab shows its close button.
enum class TabClose : std::uint8_t { hover, always, never };

struct TabStripOptions {
    float inset_left = 0.0f;       ///< room for window controls (macOS traffic lights: 78)
    bool  show_numbers = false;    ///< ⌘ held: show ⌘1..9
    float max_tab = 236.0f;
    float min_tab = 96.0f;
    float height = 46.0f;
    TabStripStyle style = TabStripStyle::pill;
    float radius = -1.0f;          ///< corner radius, px; < 0: the theme's
    bool  icon = true;             ///< the process icon with its status ring
    bool  subtitle = true;         ///< the second line: directory / host / elapsed
    bool  activity = true;         ///< the output sparkline
    bool  hue = true;              ///< colour each tab by its directory (else: the accent)
    bool  new_button = true;       ///< the + / menu group
    TabClose close = TabClose::hover;
    TabMotion motion = TabMotion::stretch;
    float stretch = 0.6f;          ///< how far the leading edge runs ahead (0..1)
    std::string number_prefix = "\u2318";   ///< before the 1..9 hints ("alt+" off macOS)
    bool  bottom = false;          ///< the strip sits under the content: its hairline goes on top
    float font_scale = 1.0f;
    /// The window has no title bar: the strip is its title area. Draws
    /// minimise / maximise / close at the right end; the strip's empty space
    /// is the place to drag the window from (the app maps a press on
    /// node_id(id) to WindowAction::move, a double click to toggle_maximize).
    bool  window_controls = false;
};

inline constexpr std::size_t tab_part_close = part_dec + 6144;
/// part_id(id, tab_part_window + 0 / 1 / 2): minimise, maximise, close.
inline constexpr std::size_t tab_part_window = part_dec + 6100;

/// The window's own buttons, for a strip that is the title area: three
/// square cells, full strip height, mono glyphs; close lights in danger.
template <ControlContext C>
[[nodiscard]] inline Node window_controls(const C& c, std::string_view id, float height, float fs, Color<Srgb> base) {
    const Theme& t = c.theme;
    const auto ink = t.text_primary;
    const auto dim = mix(ink, base, 0.42f);
    const float w = std::round(height * 1.55f);
    std::vector<Node> cells;
    static constexpr const char* glyphs[] = {"\u2212", "\u25A1", "\u00D7"};
    static constexpr const char* names[] = {"minimise", "maximise", "close window"};
    for (std::size_t k = 0; k < 3; ++k) {
        const auto pid = part_id(id, tab_part_window + k);
        const bool hov = c.hovered(pid), close = k == 2;
        cells.push_back((h(text_owned(glyphs[k]) | glyph_size(close ? fs * 1.15f : fs) | monospace | fg(hov ? (close ? colors::white : ink) : dim))
                         | size(w, height) | center
                         | bg(hov ? (close ? t.danger : mix(base, ink, 0.10f)) : base.fade(0.0f))
                         | pointer_cursor | label(names[k]) | id_of(id, tab_part_window + k)).build());
    }
    return h_list(std::move(cells)).build();
}

// ── pieces, public so other widgets (peek, launcher) look the same ─────

[[nodiscard]] inline Color<Srgb> rgb_hex(std::uint32_t v) noexcept {
    return Color<Srgb>{static_cast<float>((v >> 16) & 255) / 255.0f, static_cast<float>((v >> 8) & 255) / 255.0f,
                       static_cast<float>(v & 255) / 255.0f, 1.0f};
}

/// The tab's colour: stable per directory (or title), pleasant in Oklch.
[[nodiscard]] inline Color<Srgb> tab_hue(const TermTabInfo& tb) {
    if (tb.hue != 0) return rgb_hex(tb.hue);
    const std::string_view key = !tb.host.empty() ? std::string_view{tb.host} : !tb.cwd.empty() ? std::string_view{tb.cwd}
                                                                                                 : std::string_view{tb.title};
    const float deg = static_cast<float>(node_id(key) % 360u);
    return oklch(0.75f, 0.12f, deg).to<Srgb>();
}

/// A one- or two-letter mark for the foreground process.
[[nodiscard]] inline const char* process_glyph(std::string_view p, bool remote = false) {
    if (remote || p == "ssh" || p == "mosh" || p == "et") return "\u21C4";
    if (p == "edit" || p == "vim" || p == "nvim" || p == "vi" || p == "hx" || p == "helix" || p == "nano" || p == "emacs" || p == "micro") return "\u270E";
    if (p == "cargo" || p == "make" || p == "ninja" || p == "cmake" || p == "go" || p == "gradle" || p == "bazel" || p == "zig") return "\u2699";
    if (p == "npm" || p == "node" || p == "pnpm" || p == "yarn" || p == "bun" || p == "deno") return "\u2B22";
    if (p == "python" || p == "python3" || p == "ipython" || p == "uv") return "Py";
    if (p == "htop" || p == "btop" || p == "top" || p == "glances") return "\u2593";
    if (p == "git" || p == "lazygit" || p == "tig" || p == "gitui") return "\u2387";
    if (p == "less" || p == "man" || p == "bat" || p == "more") return "\u2261";
    if (p == "tail" || p == "journalctl" || p == "kubectl" || p == "docker" || p == "k9s") return "\u2195";
    if (p == "psql" || p == "mysql" || p == "sqlite3" || p == "redis-cli" || p == "mongosh") return "\u25A6";
    if (p == "claude" || p == "codex" || p == "aider" || p == "gemini") return "\u2726";
    return "\u276F";   // a shell
}

[[nodiscard]] inline std::string short_duration(double s) {
    char b[24];
    if (s < 60.0) std::snprintf(b, sizeof b, "%ds", static_cast<int>(s));
    else if (s < 3600.0) std::snprintf(b, sizeof b, "%dm %02ds", static_cast<int>(s / 60.0), static_cast<int>(s) % 60);
    else std::snprintf(b, sizeof b, "%dh %02dm", static_cast<int>(s / 3600.0), static_cast<int>(s / 60.0) % 60);
    return b;
}

namespace detail {

/// A crisp ring arc via the renderer's analytic arc SDF. A canvas callback is
/// a plain function pointer, so the arc's parameters ride in the node's own
/// style (unused fields of a canvas): fill colour = arc, stroke colour =
/// track, corners = start / length (turns).
inline void arc_ring_paint(DrawList& dl, const Rect& r, const void* user) {
    const Style& s = *static_cast<const Style*>(user);
    const float d = std::min(r.width(), r.height());
    const float th = std::max(2.0f, d * 0.11f);
    const Vec2 c = r.center();
    const float rad = d * 0.5f - th * 0.5f;
    constexpr float tau = 6.2831853f;
    if (s.stroke.color.a > 0.0f) dl.ring(c, rad, th, s.stroke.color);
    // Angles are y-down: -0.25 turn is 12 o'clock, sweeping clockwise.
    if (s.corners.tr > 0.0f) dl.arc(c, rad, th, (s.corners.tl - 0.25f) * tau, s.corners.tr * tau, s.fill.color);
}

/// `col` over [a0, a0+len) turns, `track` (may be transparent) behind it.
[[nodiscard]] inline Node arc_ring(Color<Srgb> col, Color<Srgb> track, float a0, float len, float d) {
    Style s;
    s.layout.width = px(d);
    s.layout.height = px(d);
    s.fill.color = col;
    s.stroke.color = track;
    s.corners.tl = a0 - std::floor(a0);
    s.corners.tr = std::clamp(len, 0.0f, 1.0f);
    Node n = Node::make_canvas(&arc_ring_paint, nullptr, s);
    return n;
}

}  // namespace detail

/// The process icon with its status ring. `d` is the outer diameter.
[[nodiscard]] inline Node tab_icon(const Theme& t, const TermTabInfo& tb, double time, bool active, float d = 24.0f) {
    const Color<Srgb> hue = tb.host_danger ? t.danger : tab_hue(tb);
    const Color<Srgb> chip = mix(t.surface, hue, active ? 0.34f : 0.20f);
    const float inner = d - 5.0f;
    const char* g = process_glyph(tb.process, !tb.host.empty());
    const bool two = std::string_view{g}.size() == 2 && static_cast<unsigned char>(g[0]) < 0x80;
    Node core = (h(text_owned(g) | glyph_size(two ? inner * 0.42f : inner * 0.56f) | bold | fg(mix(hue, colors::white, active ? 0.35f : 0.12f)))
                 | size(inner, inner) | center | radius(inner * 0.5f) | bg(chip) | absolute(2.5f, 2.5f)).build();
    Node ring;
    const Color<Srgb> track = mix(t.surface, hue, 0.16f);
    if (tb.running && tb.progress >= 0)
        ring = detail::arc_ring(tb.progress_error ? t.danger : hue, track, 0.0f, static_cast<float>(tb.progress) / 100.0f, d);
    else if (tb.running)
        ring = detail::arc_ring(hue, t.surface.fade(0.0f), static_cast<float>(time * 0.85), 0.30f, d);
    else
        ring = (box() | size(d, d)).build();
    return (z(node(std::move(ring)), node(std::move(core))) | size(d, d)).build();
}

/// Output rate over recent samples, as bars.
[[nodiscard]] inline Node activity_sparkline(const std::vector<float>& a, Color<Srgb> col, float h_px = 12.0f, int bars = 14) {
    std::vector<Node> v;
    const int n = static_cast<int>(a.size());
    for (int i = std::max(0, n - bars); i < n; ++i) {
        const float x = std::clamp(a[static_cast<std::size_t>(i)], 0.0f, 1.0f);
        const float age = static_cast<float>(n - 1 - i) / static_cast<float>(bars);   // older bars fade
        v.push_back((box() | size(2.0f, std::max(1.5f, x * h_px)) | bg(col.fade(1.0f - 0.6f * age)) | radius(1)).build());
    }
    return (h_list(std::move(v), 1.0f) | align(Align::end) | height(h_px)).build();
}

[[nodiscard]] inline bool recently_active(const std::vector<float>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = n > 6 ? n - 6 : 0; i < n; ++i) if (a[i] > 0.04f) return true;
    return false;
}

/// A tab's trailing slot: `status` (a glyph, a meter, or nothing) at least
/// `slot` wide, and -- when `show_close` -- the × stacked over it at the
/// same size. The × is in the tree only while shown (a click on a tab's
/// empty slot selects the tab), but the slot's SIZE is the same either way:
/// hovering never reflows the strip, so nothing shifts under the pointer.
template <ControlContext C>
[[nodiscard]] inline Node tab_close_slot(const C& c, std::string_view id, std::size_t i, Node status, bool can_close,
                                         bool show_close, float rad, float fs, Color<Srgb> wash = {}, float slot = 18.0f) {
    const Theme& t = c.theme;
    const auto cid = part_id(id, tab_part_close + i);
    Node base = (h(node(std::move(status)) | opacity(show_close ? 0.0f : 1.0f))
                 | min_size(can_close ? slot : 0.0f, slot) | align(Align::center) | justify(Justify::center)).build();
    if (!show_close) return base;
    const bool xh = c.hovered(cid);
    const Color<Srgb> hover_bg = wash.a > 0.0f ? wash : mix(t.surface, t.text_primary, 0.12f);
    Node x = (h(text<"\u00D7"> | font(fs) | monospace | fg(xh ? t.text_primary : t.text_secondary))
              | width(pct(100)) | height(pct(100)) | center | radius(rad)
              | bg(xh ? hover_bg : t.surface.fade(0.0f))
              | pointer_cursor | label("close tab") | id_of(id, tab_part_close + i)).build();
    return z(std::move(base), std::move(x)).build();
}

// ── the strip ───────────────────────────────────────────────────────────

template <ControlContext C>
[[nodiscard]] inline Node tab_line(const C& c, std::string_view id, const std::vector<TermTabInfo>& tabs, int active,
                                   double time, const TabStripOptions& o);

template <ControlContext C>
[[nodiscard]] inline Node tab_strip(const C& c, std::string_view id, const std::vector<TermTabInfo>& tabs, int active,
                                    const TabStripMotion* motion, double time, const TabStripOptions& o = {}) {
    if (o.style == TabStripStyle::line) return tab_line(c, id, tabs, active, time, o);
    const Theme& t = c.theme;
    using S = TabStripStyle;
    const bool lifted = o.style == S::pill;
    const float tab_h = lifted ? o.height - 10.0f : o.height - 1.0f;
    const float rad = o.radius >= 0.0f ? o.radius : t.rounded(10);
    const float fs = t.type_at(t.font_size * o.font_scale, Tier::body);   // the strip may be scaled; its tiers follow
    // a production host is red whatever the palette says: its icon and pill
    // carry it even when the strip is one line and has no room for the address
    const auto hue_of = [&](const TermTabInfo& tb) { return tb.host_danger ? t.danger : o.hue ? tab_hue(tb) : t.accent; };
    if (motion != nullptr && active >= 0 && active < static_cast<int>(tabs.size())) {
        if constexpr (requires { c.track(motion->pill, std::uint64_t{}); }) {
            c.track(motion->pill, part_id(id, static_cast<std::size_t>(active)));
            c.track(motion->strip, node_id(id));
        } else if constexpr (requires { c.c.track(motion->pill, std::uint64_t{}); }) {
            c.c.track(motion->pill, part_id(id, static_cast<std::size_t>(active)));
            c.c.track(motion->strip, node_id(id));
        }
    }
    const bool slide = motion != nullptr && motion->pill.ready() && motion->strip.ready();
    const Color<Srgb> active_hue = active >= 0 && active < static_cast<int>(tabs.size()) ? hue_of(tabs[static_cast<std::size_t>(active)]) : t.accent;
    const Color<Srgb> pill_base = mix(t.surface_raised, active_hue, 0.07f);
    const Color<Srgb> pill_edge = mix(t.border_strong, active_hue, 0.18f);
    // Two lines only when they actually fit (title + subtitle at their line
    // heights, plus breathing room); otherwise a single, centred title line
    // rather than text crammed against the tab's edges.
    const bool two_lines = o.subtitle && tab_h >= fs * (0.98f + 0.78f) * 1.3f + t.spaced(1) + 8.0f;
    const float icon_d = std::clamp(tab_h - 10.0f, 16.0f, two_lines ? 26.0f : 20.0f);

    std::vector<Node> items;
    items.push_back((box() | size(o.inset_left, 1)).build());
    const auto tab_hov = [&](std::size_t k) {
        return c.hovered(part_id(id, k)) || c.hovered(part_id(id, tab_part_close + k));
    };
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const TermTabInfo& tb = tabs[i];
        const bool on = static_cast<int>(i) == active;
        const auto pid = part_id(id, i), cid = part_id(id, tab_part_close + i);
        const bool hov = c.hovered(pid) || c.hovered(cid);
        // a hairline between two resting tabs gives each its own edge; it
        // steps aside next to the active or hovered one, which has a surface
        if (i > 0) {
            const bool quiet = on || hov || static_cast<int>(i - 1) == active || tab_hov(i - 1);
            items.push_back((box() | size(1, std::round(tab_h * 0.42f))
                             | bg(quiet ? t.border.fade(0.0f) : t.border)).build());
        }
        const Color<Srgb> hue = hue_of(tb);
        // what the active tab looks like before the sliding marker has a rect (first frame)
        const Color<Srgb> on_bg = lifted || o.style == S::flat ? pill_base : t.surface.fade(0.0f);

        // Result: a glyph that fades, never a coloured card over the whole tab.
        const float flash = !tb.running && tb.last_exit >= 0 && tb.finished_ago < 4.0
                                ? 1.0f - static_cast<float>(tb.finished_ago / 4.0) : 0.0f;
        const auto glyph = [&](std::string s, Color<Srgb> col) {
            return (text_owned(std::move(s)) | font(t.type_at(fs, Tier::small)) | monospace | semibold | fg(col)).build();
        };

        // Trailing slot: the status glyph defines its size and is ALWAYS laid
        // out; on hover the × floats over it (and the glyph hides). Hovering
        // therefore changes no size anywhere -- nothing shifts under the
        // pointer, so nothing can flicker between hovered and not.
        Node status;
        if (o.show_numbers && i < 9) {
            status = glyph(o.number_prefix + std::to_string(i + 1), t.text_disabled);
        } else if (!tb.running && tb.last_exit > 0 && tb.finished_ago < 30.0) {
            status = glyph("\u2717" + std::to_string(tb.last_exit), mix(t.danger, t.text_secondary, on ? 0.0f : 0.15f));
        } else if (tb.bell && !on) {
            status = glyph("!", t.warning);
        } else if (flash > 0.0f && tb.last_exit == 0 && !on) {
            status = glyph("\u2713", t.success.fade(flash));
        } else if (o.activity && !on && recently_active(tb.activity)) {
            // background tabs only: the one you are looking at needs no meter
            status = activity_sparkline(tb.activity, mix(hue, t.text_secondary, 0.35f), 10.0f, 8);
        }
        const bool want_close = !tb.pinned && (o.close == TabClose::always || (o.close == TabClose::hover && hov));
        Node trailing = tab_close_slot(c, id, i, std::move(status), o.close != TabClose::never && !tb.pinned, want_close,
                                       std::min(rad, 3.0f), fs);

        if (tb.pinned) {
            items.push_back((h(node(tab_icon(t, tb, time, on, icon_d)),
                               node(tb.unread && !on ? (box() | size(5, 5) | bg(hue) | radius(t.rounded(2.5f)) | absolute(27, 6)).build() : Node{}))
                             | size(40, tab_h) | center | radius(rad)
                             | bg(on ? (slide ? t.surface.fade(0.0f) : on_bg) : hov ? mix(t.surface, t.text_primary, 0.05f) : t.surface.fade(0.0f))
                             | pointer_cursor | role(a11y::Role::tab) | selected(on) | label(tb.title)
                             | id_of(id, i)).build());
            continue;
        }

        std::string line2 = !tb.host.empty() ? tb.host : tb.cwd;
        if (tb.running) line2 = short_duration(tb.elapsed) + (line2.empty() ? "" : " \u00B7 " + line2);
        const Color<Srgb> title_col = on ? t.text_primary : tb.unread || hov ? mix(t.text_secondary, t.text_primary, 0.6f) : t.text_secondary;
        // no icon: a running command still shows, as a pulsing dot
        const bool dot = (tb.unread && !on) || tb.dirty || (!o.icon && tb.running);
        const float pulse = tb.running ? 0.55f + 0.45f * static_cast<float>(std::sin(time * 5.0)) : 1.0f;
        // children only when present: an empty slot still takes a gap, which
        // indents one tab's title against its neighbours'
        std::vector<Node> title_parts;
        if (dot) title_parts.push_back((box() | size(5, 5) | bg((tb.running ? hue : mix(hue, t.text_secondary, 0.45f)).fade(pulse))
                                        | radius(2.5f)).build());
        title_parts.push_back((text_owned(tb.title.empty() ? tb.process : tb.title) | font(fs)
                               | (on ? semibold : weight(FontWeight::medium)) | fg(title_col) | ellipsis | grow() | min_size(0, 0)).build());
        Node title_row = (h_list(std::move(title_parts), t.spaced(5)) | align(Align::center) | width(pct(100))).build();
        std::vector<Node> lines;
        lines.push_back(std::move(title_row));
        if (two_lines)
            lines.push_back((text_owned(line2) | font(t.type_at(fs, Tier::small))
                             | fg(!tb.host.empty() && tb.host_danger ? t.danger : tb.running ? mix(hue, t.text_secondary, 0.3f) : t.text_disabled)
                             | ellipsis | width(pct(100))).build());

        std::vector<Node> row;
        if (o.icon) row.push_back(tab_icon(t, tb, time, on, icon_d));
        row.push_back((v_list(std::move(lines), t.spaced(1)) | justify(Justify::center) | grow() | min_size(0, 0)).build());
        row.push_back(std::move(trailing));
        items.push_back((h_list(std::move(row), t.spaced(two_lines ? 9 : 7))
                         | align(Align::center) | pad(0, t.spaced(7), 0, t.spaced(o.icon ? 6 : 10))
                         | width(0) | grow() | shrink() | min_size(o.min_tab, tab_h) | max_size(o.max_tab, tab_h) | height(tab_h)
                         | radius(lifted ? rad : 0.0f)
                         | bg(on && !slide ? on_bg
                              : !on && hov ? mix(t.surface, t.text_primary, 0.04f) : t.surface.fade(0.0f))
                         | border(t.stroke(1), on && !slide && lifted ? pill_edge : t.border.fade(0.0f))
                         | pointer_cursor | role(a11y::Role::tab) | selected(on) | label(tb.title)
                         | id_of(id, i)).build());
    }

    // New tab + profile menu, as one rounded group.
    const auto plus = part_id(id, part_inc), menu = part_id(id, part_menu);
    const float bh = std::min(tab_h, 26.0f);
    if (o.new_button) items.push_back((h(h(text<"+"> | glyph_size(fs * 1.1f) | monospace | fg(c.hovered(plus) ? t.text_primary : t.text_secondary))
                           | size(bh, bh) | center | radius(std::min(rad, 3.0f)) | bg(c.hovered(plus) ? mix(t.surface, t.text_primary, 0.07f) : t.surface.fade(0.0f))
                           | pointer_cursor | label("new tab") | id_of(id, part_inc),
                       h(text<"\u2304"> | glyph_size(fs * 0.8f) | fg(c.hovered(menu) ? t.text_primary : t.text_disabled))
                           | size(16, bh) | center | radius(std::min(rad, 3.0f)) | bg(c.hovered(menu) ? mix(t.surface, t.text_primary, 0.07f) : t.surface.fade(0.0f))
                           | pointer_cursor | label("new tab menu") | id_of(id, part_menu))
                     | gap(t.spaced(1)) | align(Align::center) | margin(0, 0, 0, 4)).build());
    items.push_back((spacer()).build());
    if (o.window_controls) items.push_back(window_controls(c, id, o.height, fs, t.surface));

    // The sliding pill, behind the tabs, in strip coordinates.
    std::vector<Node> layers;
    // the strip's bottom hairline first: the flat marker covers it (the active
    // tab opens into the terminal), the underline sits on it
    layers.push_back((box() | size(1e4f, 1) | bg(var::border) | absolute(0, o.bottom ? 0.0f : o.height - 1.0f)).build());
    if (slide && active >= 0) {
        // The marker in strip coordinates. Both rects are measured from layout,
        // so use the strip's MEASURED origin (its target), never its spring: a
        // strip that moved (sliding in, a resize) must not drag the marker off.
        Rect r = motion->pill.rect();
        const Vec2 tpos = motion->pill.position.target(), tsize = motion->pill.extent.target();
        if (o.motion == TabMotion::instant) r = Rect{tpos, tsize};
        float left = r.left(), right = r.right();
        if (o.motion == TabMotion::stretch) {   // elastic: the edge facing the destination leads
            const float k = std::clamp(o.stretch, 0.0f, 0.95f);
            if (tpos.x > left + 0.5f) right += (tpos.x + tsize.x - right) * k;
            else if (tpos.x < left - 0.5f) left += (tpos.x - left) * k;
        }
        r = Rect{Vec2{left, r.top()}, Vec2{right - left, r.height()}};
        const Vec2 o2 = motion->strip.position.target();
        const std::string marker_id = std::string{id} + ".marker";
        const float x = r.left() - o2.x, y = r.top() - o2.y;
        switch (o.style) {
            case S::pill:
                // the card alone: its tinted edge and lift say "active"; a
                // tick under it collided with the subtitle and read as noise
                layers.push_back((box() | size(r.width(), r.height()) | bg(pill_base) | border(t.stroke(1), pill_edge)
                                  | radius(rad) | absolute(x, y) | id_of(marker_id)).build());
                break;
            case S::underline: {   // the tab's width, on the strip's edge (where the hairline is)
                const float mh = 2.0f;
                layers.push_back((box() | size(r.width(), mh) | bg(active_hue)
                                  | absolute(x, o.bottom ? 0.0f : o.height - mh) | id_of(marker_id)).build());
                break;
            }
            case S::flat:
                // the active tab is the terminal's own paper, full height: it opens into it
                layers.push_back((box() | size(r.width(), o.height) | bg(t.background) | absolute(x, 0.0f) | id_of(marker_id)).build());
                layers.push_back((box() | size(r.width(), 2.0f) | bg(active_hue)
                                  | absolute(x, o.bottom ? o.height - 2.0f : 0.0f)).build());
                break;
            case S::line: break;   // drawn by tab_line, never here
        }
    }
    layers.push_back((h_list(std::move(items), 4.0f) | align(Align::center) | height(o.height) | width(pct(100)) | pad(0, o.window_controls ? 0.0f : t.spaced(6), 0, t.spaced(6)) | clip).build());

    // the row fills the strip, so the spacer between the tabs and the
    // buttons has room to push them into the corner
    return (z(h_list(std::move(layers)) | width(pct(100)) | height(o.height)) | width(pct(100)) | height(o.height) | bg(t.surface)
            | role(a11y::Role::list) | label("tabs") | id_of(id)).build();
}

/// tmux-like tabs: one line of monospace cells, square, compact.
///
///   1 zsh  2 cargo build ⠹  3 notes ●  4 prod-db                       +
///   ^^^^^ the active one: inverse, the accent; the rest quiet text
///
/// Per tab: its number, its title, then one status glyph -- a spinner
/// while a command runs, ✗N when the last one failed (fading), ✓ when one
/// just finished, ● unread output, ! bell. × replaces it on hover. A prod
/// host's tab is red. Same interaction ids as tab_strip.
template <ControlContext C>
[[nodiscard]] inline Node tab_line(const C& c, std::string_view id, const std::vector<TermTabInfo>& tabs, int active,
                                   double time, const TabStripOptions& o) {
    const Theme& t = c.theme;
    const float fs = t.type_at(t.font_size * o.font_scale, Tier::body);
    const float hgt = std::clamp(o.height, 20.0f, 40.0f);
    const auto paper = t.background, ink = t.text_primary;
    const auto bar = mix(paper, ink, 0.045f);        // the strip: one step off the terminal
    const auto dim = mix(ink, paper, 0.42f), faint = mix(ink, paper, 0.62f);
    const auto mono = [&](std::string s, Color<Srgb> col) { return text_owned(std::move(s)) | font(fs) | monospace | fg(col); };
    std::vector<Node> cells;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const TermTabInfo& tb = tabs[i];
        const bool on = static_cast<int>(i) == active;
        const auto pid = part_id(id, i), cid = part_id(id, tab_part_close + i);
        const bool hov = c.hovered(pid) || c.hovered(cid);
        const Color<Srgb> tone = tb.host_danger ? t.danger : t.accent;
        // active: a tinted cell with a 2px edge in the accent -- marked, not shouting
        const Color<Srgb> cell = mix(bar, tone, tb.host_danger ? 0.30f : 0.20f);
        const Color<Srgb> fg_ = on ? ink : hov ? ink : tb.host_danger ? mix(t.danger, dim, 0.3f) : dim;
        const Color<Srgb> num = on ? tone : faint;
        // the status glyph
        std::string glyph;
        Color<Srgb> gcol = dim;
        if (tb.running) gcol = t.info;   // drawn as a turning arc below, centred on the text
        else if (tb.last_exit > 0 && tb.finished_ago < 30.0) { glyph = "\u2717" + std::to_string(tb.last_exit); gcol = t.danger; }
        else if (tb.last_exit == 0 && tb.finished_ago < 4.0 && !on) { glyph = "\u2713"; gcol = t.success; }
        else if (tb.bell && !on) { glyph = "!"; gcol = t.warning; }
        else if (tb.dirty || (tb.unread && !on)) { glyph = "\u25CF"; gcol = mix(tone, dim, 0.3f); }
        std::vector<Node> bits;
        if (o.show_numbers || i < 9) bits.push_back(mono(std::to_string(i + 1), num).build());
        std::string title = tb.title.empty() ? tb.process : tb.title;
        if (title.size() > 28) title = title.substr(0, 27) + "\u2026";
        bits.push_back((mono(std::move(title), fg_) | (on ? semibold : weight(FontWeight::regular))).build());
        const bool can_close = o.close != TabClose::never;
        const bool show_x = can_close && (hov || o.close == TabClose::always);
        const float ring_d = std::round(fs * 0.72f);
        Node mark = tb.running ? detail::arc_ring(gcol, mix(on ? cell : bar, ink, 0.10f), static_cast<float>(time * 0.85), 0.30f, ring_d)
                  : glyph.empty() ? Node{} : mono(glyph, gcol).build();
        if (can_close || tb.running || !glyph.empty())
            bits.push_back(tab_close_slot(c, id, i, std::move(mark), can_close, show_x,
                                          0.0f, fs, mix(on ? cell : bar, ink, 0.14f), std::round(hgt * 0.62f)));
        cells.push_back((h_list(std::move(bits), fs * 0.5f) | align(Align::center) | height(hgt) | pad(0, fs * 0.75f)
                         | bg(on ? cell : hov ? mix(bar, ink, 0.08f) : bar.fade(0.0f))
                         | when(on, inner_shadow(0.0f, tone, {0.0f, o.bottom ? -2.0f : 2.0f}))
                         | pointer_cursor | role(a11y::Role::tab) | selected(on) | label(tb.title) | id_of(id, i)).build());
        // a 1px rule between two resting cells
        if (i + 1 < tabs.size() && !on && static_cast<int>(i + 1) != active)
            cells.push_back((box() | width(1.0f) | height(std::round(hgt * 0.45f)) | bg(mix(bar, ink, 0.12f))).build());
    }
    std::vector<Node> row;
    row.push_back((box() | width(o.inset_left)).build());
    row.push_back((h_list(std::move(cells)) | align(Align::center) | height(hgt) | min_size(0, 0) | clip).build());
    row.push_back(spacer().build());
    if (o.new_button) {
        const auto nid = part_id(id, part_inc);
        const bool nh = c.hovered(nid);
        row.push_back((h(mono("+", nh ? ink : dim)) | height(hgt) | pad(0, fs * 0.8f) | center
                       | bg(nh ? mix(bar, ink, 0.06f) : bar.fade(0.0f)) | pointer_cursor | tooltip("new tab") | id_of(id, part_inc)).build());
    }
    if (o.window_controls) row.push_back(window_controls(c, id, hgt, fs, bar));
    return (h(std::move(row)) | align(Align::center) | height(hgt) | width(pct(100)) | bg(bar)
            | inner_shadow(0.0f, mix(bar, ink, 0.08f), {0.0f, o.bottom ? 1.0f : -1.0f})
            | role(a11y::Role::list) | label("tabs") | id_of(id)).build();
}

}  // namespace mayag::dsl
