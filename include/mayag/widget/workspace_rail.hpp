#pragma once
// mayag::widget workspace_rail — every workspace, always in view
//
//   workspace_rail(c, "rail", workspaces, active, time, {.expanded = m.rail_open})
//
// A workspace is a named group of tabs (a project, a server fleet, "scratch").
// The rail is the left edge of the window and answers, without a click, the
// question a terminal never answers today: "what is happening everywhere?"
//
// Each workspace shows, in its colour:
//   * a monogram tile with a STATUS RING that aggregates its tabs: turning
//     while anything runs, filling when progress is known, red if any
//     failed, a badge with the count of tabs that finished or need you
//   * expanded: its name, a one-line summary ("2 running · 1 failed"), a
//     per-tab LIGHT ROW (one dot per tab: running pulses, failed red,
//     finished green, idle grey, unread brighter), and a combined output
//     sparkline
// The rail collapses to a 64 px strip of tiles (rings and badges still
// visible) and expands to 248 px. ⌃1-9 (Alt+1-9 off macOS) switch
// workspaces; workspace_key_label() is that label.
//
// Interaction:
//   Clicked on part_id(id, i)          -> switch to workspace i
//   Clicked on part_id(id, part_inc)   -> new workspace
//   Clicked on part_id(id, part_menu)  -> toggle expanded
//   Entered / Left on part_id(id, i)   -> hover (for workspace_peek)

#include "control.hpp"
#include "tab_strip.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace mayag::dsl {

/// "⌃3" on the Mac, "Alt+3" on the PC: the workspace switch key for n.
[[nodiscard]] inline std::string workspace_key_label(int n, Keymap k = native_keymap()) {
    return (k == Keymap::mac ? "\u2303" : "Alt+") + std::to_string(n);
}

struct WorkspaceInfo {
    std::string              name;
    std::uint32_t            hue = 0;        ///< 0: derived from the name
    std::vector<TermTabInfo> tabs;           ///< its tabs (status is read from them)
    std::string              detail;         ///< "~/src/mayag", "3 hosts"
};

/// What a workspace's tabs add up to.
struct WorkspaceSummary {
    int running = 0, failed = 0, finished = 0, unread = 0, bells = 0, remote = 0, danger = 0;
    int progress = -1;        ///< mean of known progress, -1 none
    std::vector<float> activity;   ///< summed output, normalised
    [[nodiscard]] int attention() const noexcept { return failed + finished + bells; }
};

[[nodiscard]] inline WorkspaceSummary summarize(const WorkspaceInfo& w) {
    WorkspaceSummary s;
    int pn = 0, ps = 0;
    std::size_t len = 0;
    for (const auto& t : w.tabs) len = std::max(len, t.activity.size());
    s.activity.assign(len, 0.0f);
    for (const auto& t : w.tabs) {
        if (t.running) { ++s.running; if (t.progress >= 0) { ps += t.progress; ++pn; } }
        else if (t.last_exit > 0 && t.finished_ago < 600) ++s.failed;
        else if (t.last_exit == 0 && t.finished_ago < 60) ++s.finished;
        if (t.unread) ++s.unread;
        if (t.bell) ++s.bells;
        if (!t.host.empty()) ++s.remote;
        if (t.host_danger) ++s.danger;
        const std::size_t off = len - t.activity.size();
        for (std::size_t k = 0; k < t.activity.size(); ++k) s.activity[off + k] += t.activity[k];
    }
    float mx = 0.0f;
    for (float a : s.activity) mx = std::max(mx, a);
    if (mx > 1.0f) for (float& a : s.activity) a /= mx;
    if (pn > 0) s.progress = ps / pn;
    return s;
}

[[nodiscard]] inline Color<Srgb> workspace_hue(const WorkspaceInfo& w) {
    if (w.hue != 0) return rgb_hex(w.hue);
    return oklch(0.72f, 0.13f, static_cast<float>(node_id(w.name) % 360u)).to<Srgb>();
}

/// The selected row's wash: the surface lifted toward the workspace colour.
[[nodiscard]] inline Color<Srgb> linear_gradient_color(const Theme& t, Color<Srgb> hue) {
    return mix(t.surface_raised, hue, 0.10f);
}

[[nodiscard]] inline std::string summary_text(const WorkspaceSummary& s, std::size_t tabs) {
    std::vector<std::string> parts;
    if (s.running) parts.push_back(std::to_string(s.running) + " running");
    if (s.failed) parts.push_back(std::to_string(s.failed) + " failed");
    if (s.finished) parts.push_back(std::to_string(s.finished) + " done");
    if (parts.empty()) parts.push_back(std::to_string(tabs) + (tabs == 1 ? " tab" : " tabs") + " \u00B7 idle");
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? " \u00B7 " : "") + parts[i];
    return out;
}

/// One dot per tab: the status of a whole workspace in one glance.
template <ControlContext C>
[[nodiscard]] inline Node tab_lights(const C& c, const std::vector<TermTabInfo>& tabs, Color<Srgb> hue,
                                     float d = 7.0f) {
    // ambient: 30 Hz, booking its own next step (a raw sin(time) froze whenever
    // nothing else drew a frame)
    const double time = ambient_time(frame_time(c));
    const Theme& t = c.theme;
    std::vector<Node> dots;
    for (const auto& tb : tabs) {
        Color<Srgb> col = t.text_disabled.fade(0.55f);
        float size_ = d;
        if (tb.running) {
            const float pulse = 0.55f + 0.45f * static_cast<float>(0.5 + 0.5 * std::sin(time * 4.0 + static_cast<double>(node_id(tb.title) % 7)));
            col = mix(t.surface, tab_hue(tb), pulse);
        } else if (tb.last_exit > 0 && tb.finished_ago < 600) col = t.danger;
        else if (tb.last_exit == 0 && tb.finished_ago < 60) col = t.success;
        else if (tb.unread) col = mix(t.text_disabled, hue, 0.6f);
        if (tb.bell) { col = t.warning; }
        if (tb.host_danger) size_ = d + 1.0f;
        dots.push_back((box() | size(size_, size_) | bg(col) | radius(tb.host_danger ? 2.0f : size_ * 0.5f)
                        | border(tb.host_danger ? 1.0f : 0.0f, t.danger)).build());
    }
    return (h_list(std::move(dots), 4.0f) | align(Align::center) | wrap | gap(t.spaced(4))).build();
}

/// The monogram tile with its aggregate ring and attention badge.
template <ControlContext C>
[[nodiscard]] inline Node workspace_tile(const C& c, const WorkspaceInfo& w, const WorkspaceSummary& s, bool active,
                                         bool hov, float d = 40.0f) {
    const double time = ambient_time(frame_time(c));
    const Theme& t = c.theme;
    const Color<Srgb> hue = workspace_hue(w);
    std::string mono;
    for (char ch : w.name) if (std::isalnum(static_cast<unsigned char>(ch))) { mono += static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); break; }
    for (std::size_t i = 1; i < w.name.size() && mono.size() < 2; ++i)
        if ((w.name[i - 1] == '-' || w.name[i - 1] == ' ' || w.name[i - 1] == '_' || w.name[i - 1] == '/') && std::isalnum(static_cast<unsigned char>(w.name[i])))
            mono += static_cast<char>(std::toupper(static_cast<unsigned char>(w.name[i])));
    const float inner = d - 8.0f;
    Node ring;
    const Color<Srgb> track = active ? mix(t.surface, hue, 0.22f) : t.surface.fade(0.0f);
    if (s.failed > 0 && s.running == 0) ring = detail::arc_ring(t.danger, t.surface.fade(0.0f), 0.0f, 1.0f, d);
    else if (s.running > 0 && s.progress >= 0) ring = detail::arc_ring(hue, mix(t.surface, hue, 0.2f), 0.0f, static_cast<float>(s.progress) / 100.0f, d);
    else if (s.running > 0) ring = detail::arc_ring(hue, track, static_cast<float>(time * 0.7), 0.28f, d);
    else ring = detail::arc_ring(hue, track, 0.0f, active ? 1.0f : 0.0f, d);
    Node tile = (h(text_owned(mono) | glyph_size(inner * 0.40f) | bold | fg(active ? mix(hue, colors::white, 0.55f) : mix(hue, colors::white, 0.25f)))
                 | size(inner, inner) | center | radius(inner * 0.32f)
                 | linear_gradient(mix(t.surface_raised, hue, active ? 0.42f : hov ? 0.30f : 0.22f), mix(t.surface, hue, active ? 0.26f : 0.14f))
                 | border(t.stroke(1), mix(t.border, hue, active ? 0.6f : 0.25f))
                 | absolute(4, 4)).build();
    const int badge_n = s.attention();
    Node badge = badge_n > 0
        ? (h(text_owned(std::to_string(badge_n)) | glyph_size(9.5f) | bold | fg(colors::white)) | min_size(15, 15) | pad(0, t.spaced(3)) | center | radius(t.rounded(7.5f))
           | bg(s.failed ? t.danger : s.bells ? t.warning : t.success) | border(t.stroke(1.5f), t.surface) | absolute(d - 13.0f, -3.0f)).build()
        : Node{};
    return (z(node(std::move(ring)), node(std::move(tile)), node(std::move(badge))) | size(d, d)).build();
}

struct RailOptions {
    bool  expanded = true;
    float top_inset = 0.0f;    ///< window controls above the rail
};

template <ControlContext C>
[[nodiscard]] inline Node workspace_rail(const C& c, std::string_view id, const std::vector<WorkspaceInfo>& ws, int active,
                                         const RailOptions& o = {}) {
    const double time = ambient_time(frame_time(c));   // the footer ring: ambient, self-booking
    const Theme& t = c.theme;
    std::vector<Node> rows;
    for (std::size_t i = 0; i < ws.size(); ++i) {
        const auto& w = ws[i];
        const auto s = summarize(w);
        const auto pid = part_id(id, i);
        const bool on = static_cast<int>(i) == active, hov = c.hovered(pid);
        const Color<Srgb> hue = workspace_hue(w);
        Node tile = workspace_tile(c, w, s, on, hov);
        if (!o.expanded) {
            rows.push_back((h(box() | size(3, on ? 26.0f : hov ? 12.0f : 0.0f) | bg(hue) | radius(t.rounded(1.5f)) | margin(0, 7, 0, 0),
                              node(std::move(tile)))
                            | align(Align::center) | height(52) | width(pct(100))
                            | pointer_cursor | role(a11y::Role::tab) | selected(on) | label(w.name) | id_of(id, i)).build());
            continue;
        }
        const Color<Srgb> sub_col = s.failed ? t.danger : s.running ? mix(hue, t.text_secondary, 0.25f) : t.text_disabled;
        rows.push_back((h(node(std::move(tile)),
                          v(h(text_owned(w.name) | font(t.type(Tier::body)) | (on ? semibold : weight(FontWeight::medium))
                                  | fg(on ? t.text_primary : t.text_secondary) | ellipsis | grow() | min_size(0, 0),
                              text_owned(i < 9 ? workspace_key_label(static_cast<int>(i) + 1) : std::string{}) | font(t.type(Tier::fine))
                                  | fg(hov ? t.text_secondary : t.text_disabled))
                                | gap(t.spaced(6)) | align(Align::center) | width(pct(100)),
                            text_owned(summary_text(s, w.tabs.size())) | font(t.type(Tier::fine)) | fg(sub_col) | ellipsis,
                            h(node(tab_lights(c, w.tabs, hue)), spacer(),
                              node(s.running > 0 && !s.activity.empty() ? activity_sparkline(s.activity, mix(hue, t.text_secondary, 0.2f), 10.0f, 12) : Node{}))
                                | align(Align::center) | width(pct(100)) | margin(3, 0, 0, 0))
                              | gap(t.spaced(1)) | grow() | min_size(0, 0))
                        | gap(t.spaced(11)) | align(Align::center) | pad(t.spaced(8), t.spaced(10), t.spaced(8), t.spaced(8)) | width(pct(100)) | radius(t.rounded(12))
                        | bg(on ? linear_gradient_color(t, hue) : hov ? mix(t.surface, colors::white, 0.03f) : t.surface.fade(0.0f))
                        | border(t.stroke(1), on ? mix(t.border_strong, hue, 0.35f) : t.border.fade(0.0f))
                        | pointer_cursor | role(a11y::Role::tab) | selected(on) | label(w.name)
                        | id_of(id, i)).build());
    }
    const auto plus = part_id(id, part_inc), tog = part_id(id, part_menu);
    Node add = o.expanded
        ? (h(h(text<"+"> | font(t.type(Tier::large)) | fg(c.hovered(plus) ? t.text_primary : t.text_secondary)) | size(26, 26) | center
                 | radius(t.rounded(8)) | border(t.stroke(1), t.border) | dashed(3, 2),
             text<"New workspace"> | font(t.type(Tier::body)) | fg(c.hovered(plus) ? t.text_primary : t.text_disabled))
           | gap(t.spaced(10)) | align(Align::center) | pad(t.spaced(6), t.spaced(12)) | width(pct(100)) | radius(t.rounded(10))
           | bg(c.hovered(plus) ? mix(t.surface, colors::white, 0.03f) : t.surface.fade(0.0f)) | pointer_cursor | id_of(id, part_inc)).build()
        : (h(h(text<"+"> | font(t.type(Tier::large)) | fg(c.hovered(plus) ? t.text_primary : t.text_secondary)) | size(30, 30) | center
                 | radius(t.rounded(10)) | border(t.stroke(1), t.border) | dashed(3, 2))
           | width(pct(100)) | center | height(44) | pointer_cursor | id_of(id, part_inc)).build();

    // Everywhere at once: the footer totals every workspace.
    int run = 0, fail = 0;
    for (const auto& w : ws) { const auto s = summarize(w); run += s.running; fail += s.failed; }
    Node footer = o.expanded
        ? (h(h(text<"\u2039"> | font(t.type(Tier::body)) | fg(c.hovered(tog) ? t.text_primary : t.text_disabled)) | size(22, 22) | center | radius(t.rounded(6))
                 | bg(c.hovered(tog) ? t.surface_raised : t.surface.fade(0.0f)) | pointer_cursor | label("collapse") | id_of(id, part_menu),
             spacer(),
             node(run ? detail::arc_ring(t.info, t.surface.fade(0.0f), static_cast<float>(time * 0.8), 0.3f, 11.0f) : Node{}),
             text_owned(run ? " " + std::to_string(run) + " running" : std::string{"all quiet"}) | font(t.type(Tier::fine))
                 | fg(run ? t.info : t.text_disabled),
             text_owned(fail ? " \u00B7 " + std::to_string(fail) + " failed" : std::string{}) | font(t.type(Tier::fine)) | fg(var::danger))
           | align(Align::center) | pad(t.spaced(8), t.spaced(10)) | width(pct(100))).build()
        : (h(text<"\u203A"> | font(t.type(Tier::body)) | fg(c.hovered(tog) ? t.text_primary : t.text_disabled)) | size(28, 28) | center | radius(t.rounded(6))
           | bg(c.hovered(tog) ? t.surface_raised : t.surface.fade(0.0f)) | pointer_cursor | label("expand") | id_of(id, part_menu)
           | margin(0, 0, 8, 18)).build();

    return (v(box() | height(o.top_inset + 10.0f),
              node(o.expanded ? (text<"WORKSPACES"> | font(t.type(Tier::fine)) | semibold | tracking(1.1f) | fg(var::text_disabled) | pad(t.spaced(4), t.spaced(12), t.spaced(6), t.spaced(12))).build()
                              : Node{}),
              v(v_list(std::move(rows), 3.0f)) | width(pct(100)) | pad(0, o.expanded ? 6.0f : 0.0f),
              node(std::move(add)) | margin(4, o.expanded ? 6.0f : 0.0f),
              spacer(),
              node(std::move(footer)))
            | width(o.expanded ? 248.0f : 64.0f) | height(pct(100))
            | linear_gradient(mix(t.surface, colors::black, 0.10f), mix(t.surface, colors::black, 0.22f))
            | border(t.stroke(1), t.border)
            | role(a11y::Role::list) | label("workspaces") | id_of(id)).build();
}

}  // namespace mayag::dsl
