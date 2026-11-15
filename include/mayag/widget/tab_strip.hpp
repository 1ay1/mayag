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
    std::vector<float> activity;       ///< recent output rate per sample, 0..1, oldest first
    std::uint32_t hue = 0;             ///< 0xRRGGBB override; 0 = derived from cwd
    bool        pinned = false;        ///< icon-only
};

struct TabStripMotion {
    Tracked pill;    ///< the active tab's rect
    Tracked strip;   ///< the strip's own rect (pill coordinates are relative to it)
    [[nodiscard]] bool animating() const noexcept { return pill.animating(); }
};

struct TabStripOptions {
    float inset_left = 0.0f;       ///< room for window controls (macOS traffic lights: 78)
    bool  show_numbers = false;    ///< ⌘ held: show ⌘1..9
    float max_tab = 236.0f;
    float min_tab = 96.0f;
    float height = 46.0f;
};

inline constexpr std::size_t tab_part_close = part_dec + 6144;

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
    if (p == "vim" || p == "nvim" || p == "vi" || p == "hx" || p == "helix" || p == "nano" || p == "emacs" || p == "micro") return "\u270E";
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
    const Color<Srgb> hue = tab_hue(tb);
    const Color<Srgb> chip = mix(t.surface, hue, active ? 0.34f : 0.20f);
    const float inner = d - 5.0f;
    const char* g = process_glyph(tb.process, !tb.host.empty());
    const bool two = std::string_view{g}.size() == 2 && static_cast<unsigned char>(g[0]) < 0x80;
    Node core = (h(text_owned(g) | font(two ? inner * 0.42f : inner * 0.56f) | bold | fg(mix(hue, colors::white, active ? 0.35f : 0.12f)))
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

// ── the strip ───────────────────────────────────────────────────────────

template <ControlContext C>
[[nodiscard]] inline Node tab_strip(const C& c, std::string_view id, const std::vector<TermTabInfo>& tabs, int active,
                                    const TabStripMotion* motion, double time, const TabStripOptions& o = {}) {
    const Theme& t = c.theme;
    const float tab_h = o.height - 10.0f;
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
    const Color<Srgb> active_hue = active >= 0 && active < static_cast<int>(tabs.size()) ? tab_hue(tabs[static_cast<std::size_t>(active)]) : t.accent;
    const Color<Srgb> pill_base = mix(t.surface_raised, active_hue, 0.07f);
    const Color<Srgb> pill_top = mix(pill_base, colors::white, 0.045f);
    const Color<Srgb> pill_edge = mix(t.border_strong, active_hue, 0.30f);

    std::vector<Node> items;
    items.push_back((box() | size(o.inset_left, 1)).build());
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const TermTabInfo& tb = tabs[i];
        const bool on = static_cast<int>(i) == active;
        const auto pid = part_id(id, i), cid = part_id(id, tab_part_close + i);
        const bool hov = c.hovered(pid) || c.hovered(cid);
        const Color<Srgb> hue = tab_hue(tb);

        // Result flash: fades over three seconds after a command ends.
        const float flash = !tb.running && tb.last_exit >= 0 && tb.finished_ago < 3.0
                                ? 1.0f - static_cast<float>(tb.finished_ago / 3.0) : 0.0f;
        const Color<Srgb> flash_col = tb.last_exit == 0 ? t.success : t.danger;

        // Trailing slot: close on hover, else the most useful signal.
        Node trailing;
        if (hov && !tb.pinned) {
            trailing = (h(text<"\u00D7"> | font(t.font_size * 1.05f) | fg(c.hovered(cid) ? t.text_primary : t.text_secondary))
                        | size(20, 20) | center | radius(6) | bg(c.hovered(cid) ? mix(t.surface_raised, colors::white, 0.06f) : t.surface.fade(0.0f))
                        | pointer_cursor | label("close tab") | id_of(id, tab_part_close + i)).build();
        } else if (o.show_numbers && i < 9) {
            trailing = (h(text_owned("\u2318" + std::to_string(i + 1)) | font(t.font_size * 0.78f) | semibold | fg(t.text_secondary))
                        | pad(1, 5) | radius(5) | bg(t.surface_raised)).build();
        } else if (tb.bell) {
            trailing = (text<"\U0001F514"> | font(t.font_size * 0.8f)).build();
        } else if (!tb.running && tb.last_exit > 0) {
            trailing = (h(text_owned("\u2716 " + std::to_string(tb.last_exit)) | font(t.font_size * 0.74f) | semibold | fg(t.danger))
                        | pad(0, 5) | radius(5) | bg(t.tint(t.danger, 0.14f))).build();
        } else if (recently_active(tb.activity)) {
            trailing = activity_sparkline(tb.activity, on ? hue : mix(hue, t.text_secondary, 0.35f));
        }

        if (tb.pinned) {
            items.push_back((h(node(tab_icon(t, tb, time, on, 24.0f)),
                               node(tb.unread && !on ? (box() | size(5, 5) | bg(hue) | radius(2.5f) | absolute(27, 6)).build() : Node{}))
                             | size(40, tab_h) | center | radius(10)
                             | bg(on && !slide ? pill_base : hov ? mix(t.surface, colors::white, 0.04f) : flash > 0 ? t.tint(flash_col, 0.25f * flash) : t.surface.fade(0.0f))
                             | pointer_cursor | role(a11y::Role::tab) | selected(on) | label(tb.title)
                             | id_of(id, i)).build());
            continue;
        }

        std::string line2 = !tb.host.empty() ? tb.host : tb.cwd;
        if (tb.running) line2 = short_duration(tb.elapsed) + (line2.empty() ? "" : "  \u00B7  " + line2);
        const Color<Srgb> title_col = on ? t.text_primary : tb.unread || hov ? mix(t.text_secondary, t.text_primary, 0.6f) : t.text_secondary;
        auto title_row = h(node(tb.unread && !on ? (box() | size(5, 5) | bg(hue) | radius(2.5f)).build() : Node{}),
                           text_owned(tb.title.empty() ? tb.process : tb.title) | font(t.font_size * 0.98f)
                               | (on ? semibold : weight(FontWeight::medium)) | fg(title_col) | ellipsis | grow() | min_size(0, 0))
                       | gap(5) | align(Align::center) | width(pct(100));
        Node sub = (text_owned(line2) | font(t.font_size * 0.78f)
                    | fg(!tb.host.empty() && tb.host_danger ? t.danger : tb.running ? mix(hue, t.text_secondary, 0.3f) : t.text_disabled)
                    | ellipsis).build();

        items.push_back((h(node(tab_icon(t, tb, time, on)),
                           v(std::move(title_row), node(std::move(sub))) | gap(1) | grow() | min_size(0, 0),
                           h(node(std::move(trailing))) | min_size(20, 0) | align(Align::center) | justify(Justify::end))
                         | gap(9) | align(Align::center) | pad(0, 7, 0, 9)
                         | width(0) | grow() | shrink() | min_size(o.min_tab, tab_h) | max_size(o.max_tab, tab_h) | height(tab_h)
                         | radius(10)
                         | bg(on && !slide ? pill_base
                              : flash > 0 ? t.tint(flash_col, 0.28f * flash)
                              : hov ? mix(t.surface, colors::white, 0.035f) : t.surface.fade(0.0f))
                         | border(1, on && !slide ? pill_edge : flash > 0 ? t.tint(flash_col, 0.55f * flash) : t.border.fade(0.0f))
                         | pointer_cursor | role(a11y::Role::tab) | selected(on) | label(tb.title)
                         | id_of(id, i)).build());
    }

    // New tab + profile menu, as one rounded group.
    const auto plus = part_id(id, part_inc), menu = part_id(id, part_menu);
    items.push_back((h(h(text<"+"> | font(t.font_size * 1.35f) | fg(c.hovered(plus) ? t.text_primary : t.text_secondary))
                           | size(30, 30) | center | radius(8) | bg(c.hovered(plus) ? mix(t.surface, colors::white, 0.06f) : t.surface.fade(0.0f))
                           | pointer_cursor | label("new tab") | id_of(id, part_inc),
                       h(text<"\u2304"> | font(t.font_size * 0.9f) | fg(c.hovered(menu) ? t.text_primary : t.text_disabled))
                           | size(20, 30) | center | radius(8) | bg(c.hovered(menu) ? mix(t.surface, colors::white, 0.06f) : t.surface.fade(0.0f))
                           | pointer_cursor | label("new tab menu") | id_of(id, part_menu))
                     | gap(1) | align(Align::center) | margin(0, 0, 0, 4)).build());
    items.push_back((spacer()).build());

    // The sliding pill, behind the tabs, in strip coordinates.
    std::vector<Node> layers;
    if (slide && active >= 0) {
        const Rect r = motion->pill.rect();
        const Vec2 o2 = motion->strip.rect().origin;
        const float x = r.left() - o2.x, y = r.top() - o2.y;
        layers.push_back((box() | size(r.width(), r.height()) | linear_gradient(pill_top, pill_base) | border(1, pill_edge)
                          | radius(10) | elevation(4) | absolute(x, y)).build());
        layers.push_back((box() | size(18, 2.5f) | bg(active_hue) | radius(1.25f)
                          | absolute(x + r.width() * 0.5f - 9.0f, y + r.height() - 2.5f)).build());
    }
    layers.push_back((box() | size(1e4f, 1) | bg(t.border) | absolute(0, o.height - 1.0f)).build());
    layers.push_back((h_list(std::move(items), 4.0f) | align(Align::center) | height(o.height) | width(pct(100)) | pad(0, 6) | clip).build());

    return (z(h_list(std::move(layers))) | width(pct(100)) | height(o.height)
            | linear_gradient(mix(t.surface, colors::white, 0.015f), t.surface)
            | role(a11y::Role::list) | label("tabs") | id_of(id)).build();
}

}  // namespace mayag::dsl
