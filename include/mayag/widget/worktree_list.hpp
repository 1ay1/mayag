#pragma once
// mayag::widget worktree_list — one row per agent workspace
//
//   worktree_list(c, "trees", trees, active)
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
[[nodiscard]] inline Node worktree_list(const C& c, std::string_view id, const std::vector<Worktree>& trees, int active) {
    const double time = frame_time(c);
    const Theme& t = c.theme;
    std::vector<Node> rows;
    for (std::size_t i = 0; i < trees.size(); ++i) {
        const auto& w = trees[i];
        const auto pid = part_id(id, i), rid = part_id(id, 4096 + i);
        const bool on = static_cast<int>(i) == active, hov = c.hovered(pid) || c.hovered(rid);
        const Color<Srgb> tone = agent_tone(t, w.agent.state);
        Node checks;
        switch (w.checks) {
            case Worktree::Checks::pending: checks = (text<"\u25CB checks"> | font(t.type(Tier::fine)) | fg(var::warning)).build(); break;
            case Worktree::Checks::passing: checks = (text<"\u2713 checks"> | font(t.type(Tier::fine)) | fg(var::success)).build(); break;
            case Worktree::Checks::failing: checks = (text<"\u2716 checks"> | font(t.type(Tier::fine)) | fg(var::danger)).build(); break;
            default: break;
        }
        const bool has_diff = w.added + w.removed > 0;
        rows.push_back((v(h(text_owned(w.agent.state == AgentState::none ? "\u2387" : agent_glyph(w.agent.state, time))
                                | font(t.type(Tier::body)) | bold | fg(w.agent.state == AgentState::none ? t.text_disabled : tone) | width(14),
                            text_owned(w.branch) | font(t.type(Tier::body)) | semibold | fg(on ? t.text_primary : t.text_secondary)
                                | ellipsis | grow() | min_size(0, 0),
                            text_owned(w.pr) | font(t.type(Tier::fine)) | fg(var::accent))
                              | gap(t.spaced(6)) | align(Align::center) | width(pct(100)),
                          h(strut(14),
                            node(w.host ? host_badge(c, *w.host, true) : (text_owned(w.path) | font(t.type(Tier::fine)) | fg(var::text_disabled) | ellipsis).build()),
                            spacer(),
                            node(std::move(checks)))
                              | gap(t.spaced(6)) | align(Align::center) | width(pct(100)),
                          h(strut(14),
                            text_owned(w.agent.state == AgentState::none ? std::string{"no agent"} : w.agent.agent + " \u00B7 " + agent_state_words(w.agent.state))
                                | font(t.type(Tier::fine)) | fg(w.agent.state == AgentState::none ? t.text_disabled : tone),
                            spacer(),
                            node(has_diff ? (h(text_owned("+" + std::to_string(w.added)) | font(t.type(Tier::fine)) | fg(var::success),
                                               text_owned("\u2212" + std::to_string(w.removed)) | font(t.type(Tier::fine)) | fg(var::danger),
                                               text_owned(std::to_string(w.files) + "f") | font(t.type(Tier::fine)) | fg(var::text_disabled))
                                             | gap(t.spaced(4)) | pad(0, t.spaced(5)) | radius(t.rounded(3)) | bg(c.hovered(rid) ? t.surface_raised : t.surface.fade(0.0f))
                                             | pointer_cursor | label("review changes") | id_of(id, 4096 + i)).build() : Node{}),
                            text_owned(w.ahead || w.behind ? "\u2191" + std::to_string(w.ahead) + " \u2193" + std::to_string(w.behind) : "")
                                | font(t.type(Tier::fine)) | fg(var::text_disabled))
                              | gap(t.spaced(6)) | align(Align::center) | width(pct(100)))
                        | gap(t.spaced(3)) | pad(t.spaced(7), t.spaced(8)) | width(pct(100)) | radius(t.radius_small)
                        | bg(on ? t.surface_raised : hov ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f))
                        | border(t.stroke(1), wants_attention(w.agent.state) && !on ? t.tint(tone, 0.45f) : t.border.fade(0.0f))
                        | pointer_cursor | role(a11y::Role::list_item) | selected(on) | label(w.branch)
                        | id_of(id, i)).build());
    }
    const auto nid = part_id(id, part_inc);
    return (v(h(text<"WORKSPACES"> | font(t.type(Tier::fine)) | semibold | tracking(0.8f) | fg(var::text_secondary), spacer(),
                h(text<"+ New agent"> | font(t.type(Tier::small)) | fg(c.hovered(nid) ? t.text_primary : t.accent))
                    | pad(t.spaced(1), t.spaced(6)) | radius(t.rounded(4)) | bg(c.hovered(nid) ? t.surface_raised : t.surface.fade(0.0f)) | pointer_cursor | id_of(id, part_inc))
                  | align(Align::center) | width(pct(100)),
              v(v_list(std::move(rows), 3.0f)) | width(pct(100)))
            | gap(t.spaced(8)) | width(pct(100)) | role(a11y::Role::list) | label("workspaces")).build();
}

}  // namespace mayag::dsl
