#pragma once
// mayag::widget attention_queue — the panes waiting for you
//
//   attention_queue(c, "queue", items, time)
//
// With five agents running in five panes, the terminal's job is to tell you
// which one needs you, in what order, and get you there in one key. Each
// item: the agent and its state, where it runs (tab/pane, worktree or host),
// the agent's own words (its OSC 9 notification: "needs permission to run
// rm -rf build"), how long it has waited. Approvals first, then questions,
// then finished; oldest first within each.
//
// Interaction: part_id(id, i) jump to item i's pane; part_id(id, 4096 + i)
// dismiss it. The app maps ⌘J to "jump to the first item".

#include "control.hpp"
#include "agent_state.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace mayag::dsl {

struct AttentionItem {
    AgentInfo   agent;
    std::string where;     ///< "tab 2 · mayag/fix-render", "prod-db-01"
    int         pane = 0;  ///< the app's pane id
};

/// Order a queue: approval > input > error > done, longest waiting first.
inline void sort_attention(std::vector<AttentionItem>& v) {
    const auto rank = [](AgentState s) {
        switch (s) { case AgentState::needs_approval: return 0; case AgentState::needs_input: return 1;
                     case AgentState::error: return 2; case AgentState::done: return 3; default: return 4; }
    };
    std::stable_sort(v.begin(), v.end(), [&](const AttentionItem& a, const AttentionItem& b) {
        if (rank(a.agent.state) != rank(b.agent.state)) return rank(a.agent.state) < rank(b.agent.state);
        return a.agent.since > b.agent.since;
    });
}

[[nodiscard]] inline std::string waited(double s) {
    if (s < 60) return std::to_string(static_cast<int>(s)) + "s";
    if (s < 3600) return std::to_string(static_cast<int>(s / 60)) + "m";
    return std::to_string(static_cast<int>(s / 3600)) + "h";
}

template <ControlContext C>
[[nodiscard]] inline Node attention_queue(const C& c, std::string_view id, const std::vector<AttentionItem>& items, double time,
                                          int keyboard = -1) {
    const Theme& t = c.theme;
    std::vector<Node> rows;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        const auto pid = part_id(id, i), did = part_id(id, 4096 + i);
        const bool hov = c.hovered(pid) || c.hovered(did) || static_cast<int>(i) == keyboard;
        const Color<Srgb> tone = agent_tone(t, it.agent.state);
        rows.push_back((h(box() | width(3) | bg(tone),
                          v(h(text_owned(agent_glyph(it.agent.state, time)) | font(t.font_size * 0.85f) | bold | fg(tone),
                              text_owned(it.agent.agent) | font(t.font_size * 0.88f) | semibold | fg(var::text_primary),
                              text_owned(agent_state_words(it.agent.state)) | font(t.font_size * 0.8f) | fg(tone),
                              spacer(),
                              text_owned(waited(it.agent.since)) | font(t.font_size * 0.75f) | fg(var::text_disabled),
                              node(hov ? (h(text<"\u00D7"> | font(t.font_size * 0.85f) | fg(c.hovered(did) ? t.text_primary : t.text_disabled))
                                          | pointer_cursor | label("dismiss") | id_of(id, 4096 + i)).build() : Node{}))
                                | gap(t.spaced(6)) | align(Align::center) | width(pct(100)),
                            text_owned(it.agent.message) | font(t.font_size * 0.85f) | fg(var::text_secondary) | wrap_text | clamp_lines(2),
                            text_owned(it.where) | font(t.font_size * 0.75f) | fg(var::text_disabled) | ellipsis)
                              | gap(t.spaced(3)) | pad(t.spaced(7), t.spaced(10)) | grow() | min_size(0, 0))
                        | align(Align::stretch) | width(pct(100)) | radius(t.radius_small)
                        | bg(hov ? t.surface_raised : t.surface) | border(t.stroke(1), hov ? t.tint(tone, 0.5f) : t.border)
                        | clip | pointer_cursor | role(a11y::Role::list_item) | label(it.agent.agent + " " + agent_state_words(it.agent.state))
                        | id_of(id, i)).build());
    }
    const int urgent = static_cast<int>(std::count_if(items.begin(), items.end(), [](const auto& x) {
        return x.agent.state == AgentState::needs_approval || x.agent.state == AgentState::needs_input; }));
    Node empty = items.empty() ? (v(text<"Nothing is waiting for you."> | font(t.font_size * 0.88f) | fg(var::text_disabled))
                                  | pad(t.spaced(10), t.spaced(4))).build() : Node{};
    return (v(h(text<"WAITING ON YOU"> | font(t.font_size * 0.72f) | semibold | tracking(0.8f) | fg(var::text_secondary),
                spacer(),
                node(urgent > 0 ? (h(text_owned(std::to_string(urgent)) | font(t.font_size * 0.75f) | bold | fg(var::on_accent))
                                   | pad(0, t.spaced(6)) | radius(t.rounded(8)) | bg(var::warning)).build() : Node{}),
                text<"\u2318J"> | font(t.font_size * 0.72f) | fg(var::text_disabled))
                  | gap(t.spaced(6)) | align(Align::center) | width(pct(100)),
              node(std::move(empty)),
              v(v_list(std::move(rows), 6.0f)) | width(pct(100)))
            | gap(t.spaced(8)) | width(pct(100)) | role(a11y::Role::list) | label("attention queue")).build();
}

}  // namespace mayag::dsl
