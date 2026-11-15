#pragma once
// mayag::widget worktree_list — one row per agent workspace
//
//   worktree_list(c, "trees", trees, active, time)
//
// The way to run several coding agents without them trampling each other is
// a git worktree (or a remote box) per agent. This sidebar lists them: the
// branch, where it lives (local path or ssh host), the agent running there
// and its state, the diff it has produced (+/−, files), ahead/behind, and a
// PR/check status if the app knows it. "+ New agent" opens a worktree and
// launches an agent in it.
//
// Interaction: part_id(id, i) focus workspace i; part_id(id, part_inc) new;
//              part_id(id, 4096 + i) review its changes.

#include "control.hpp"
#include "agent_state.hpp"
#include "ssh.hpp"   // host_badge

#include <optional>
#include <string>
#include <vector>

namespace mayag::dsl {

struct Worktree {
    std::string branch;            ///< "fix/cell-px"
    std::string path;              ///< "~/src/mayag-wt/fix-cell-px"
    std::optional<SshHost> host;   ///< remote workspace
    AgentInfo   agent;
    int         added = 0, removed = 0, files = 0;
    int         ahead = 0, behind = 0;
    enum class Checks : std::uint8_t { none, pending, passing, failing } checks = Checks::none;
    std::string pr;                ///< "#128"
};

template <ControlContext C>
[[nodiscard]] inline Node worktree_list(const C& c, std::string_view id, const std::vector<Worktree>& trees, int active, double time) {
    const Theme& t = c.theme;
    std::vector<Node> rows;
    for (std::size_t i = 0; i < trees.size(); ++i) {
        const auto& w = trees[i];
        const auto pid = part_id(id, i), rid = part_id(id, 4096 + i);
        const bool on = static_cast<int>(i) == active, hov = c.hovered(pid) || c.hovered(rid);
        const Color<Srgb> tone = agent_tone(t, w.agent.state);
        Node checks;
        switch (w.checks) {
            case Worktree::Checks::pending: checks = (text<"\u25CB checks"> | font(t.font_size * 0.72f) | fg(t.warning)).build(); break;
            case Worktree::Checks::passing: checks = (text<"\u2713 checks"> | font(t.font_size * 0.72f) | fg(t.success)).build(); break;
            case Worktree::Checks::failing: checks = (text<"\u2716 checks"> | font(t.font_size * 0.72f) | fg(t.danger)).build(); break;
            default: break;
        }
        const bool has_diff = w.added + w.removed > 0;
        rows.push_back((v(h(text_owned(w.agent.state == AgentState::none ? "\u2387" : agent_glyph(w.agent.state, time))
                                | font(t.font_size * 0.9f) | bold | fg(w.agent.state == AgentState::none ? t.text_disabled : tone) | width(14),
                            text_owned(w.branch) | font(t.font_size * 0.92f) | semibold | fg(on ? t.text_primary : t.text_secondary)
                                | ellipsis | grow() | min_size(0, 0),
                            text_owned(w.pr) | font(t.font_size * 0.75f) | fg(t.accent))
                              | gap(6) | align(Align::center) | width(pct(100)),
                          h(strut(14),
                            node(w.host ? host_badge(c, *w.host, true) : (text_owned(w.path) | font(t.font_size * 0.75f) | fg(t.text_disabled) | ellipsis).build()),
                            spacer(),
                            node(std::move(checks)))
                              | gap(6) | align(Align::center) | width(pct(100)),
                          h(strut(14),
                            text_owned(w.agent.state == AgentState::none ? std::string{"no agent"} : w.agent.agent + " \u00B7 " + agent_state_words(w.agent.state))
                                | font(t.font_size * 0.75f) | fg(w.agent.state == AgentState::none ? t.text_disabled : tone),
                            spacer(),
                            node(has_diff ? (h(text_owned("+" + std::to_string(w.added)) | font(t.font_size * 0.75f) | fg(t.success),
                                               text_owned("\u2212" + std::to_string(w.removed)) | font(t.font_size * 0.75f) | fg(t.danger),
                                               text_owned(std::to_string(w.files) + "f") | font(t.font_size * 0.75f) | fg(t.text_disabled))
                                             | gap(4) | pad(0, 5) | radius(3) | bg(c.hovered(rid) ? t.surface_raised : t.surface.fade(0.0f))
                                             | pointer_cursor | label("review changes") | id_of(id, 4096 + i)).build() : Node{}),
                            text_owned(w.ahead || w.behind ? "\u2191" + std::to_string(w.ahead) + " \u2193" + std::to_string(w.behind) : "")
                                | font(t.font_size * 0.72f) | fg(t.text_disabled))
                              | gap(6) | align(Align::center) | width(pct(100)))
                        | gap(3) | pad(7, 8) | width(pct(100)) | radius(t.radius_small)
                        | bg(on ? t.surface_raised : hov ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f))
                        | border(1, wants_attention(w.agent.state) && !on ? t.tint(tone, 0.45f) : t.border.fade(0.0f))
                        | pointer_cursor | role(a11y::Role::list_item) | selected(on) | label(w.branch)
                        | id_of(id, i)).build());
    }
    const auto nid = part_id(id, part_inc);
    return (v(h(text<"WORKSPACES"> | font(t.font_size * 0.72f) | semibold | tracking(0.8f) | fg(t.text_secondary), spacer(),
                h(text<"+ New agent"> | font(t.font_size * 0.8f) | fg(c.hovered(nid) ? t.text_primary : t.accent))
                    | pad(1, 6) | radius(4) | bg(c.hovered(nid) ? t.surface_raised : t.surface.fade(0.0f)) | pointer_cursor | id_of(id, part_inc))
                  | align(Align::center) | width(pct(100)),
              v(v_list(std::move(rows), 3.0f)) | width(pct(100)))
            | gap(8) | width(pct(100)) | role(a11y::Role::list) | label("workspaces")).build();
}

}  // namespace mayag::dsl
