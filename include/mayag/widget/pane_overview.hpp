#pragma once
// mayag::widget pane_overview — every pane at a glance (mission control)
//
//   pane_overview(c, "overview", cards, time)
//
// A grid of live thumbnails: each card shows the last few lines of a pane's
// screen (any Node — usually a small terminal_view), its title, the host
// badge for SSH panes, and the agent badge. Cards that want attention glow
// in their state colour and sort first, so ⌘. → glance → click is how you
// supervise many agents. Posted as a full-window modal overlay.
//
// Interaction: part_id(id, i) open card i; Left on part_id(id, part_menu)
// dismiss; arrow keys / 1-9 in the app keymap.

#include "control.hpp"
#include "agent_state.hpp"
#include "ssh.hpp"
#include "../scene/overlay.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mayag::dsl {

struct PaneCard {
    std::string            title;
    AgentInfo              agent;
    std::optional<SshHost> host;
    Node                   preview;      ///< the last lines of the screen
    bool                   focused = false;
};

template <ControlContext C>
inline void pane_overview(const C& c, std::string_view id, std::vector<PaneCard> cards, double time, int keyboard = -1,
                          int columns = 3) {
    const Theme& t = c.theme;
    std::vector<Node> grid_rows, row;
    for (std::size_t i = 0; i < cards.size(); ++i) {
        auto& k = cards[i];
        detail::make_inert(k.preview);
        const auto pid = part_id(id, i);
        const bool hov = c.hovered(pid) || static_cast<int>(i) == keyboard;
        const bool hot = wants_attention(k.agent.state);
        const Color<Srgb> tone = agent_tone(t, k.agent.state);
        row.push_back((v(h(text_owned(i < 9 ? std::to_string(i + 1) : std::string{}) | font(t.font_size * 0.72f) | fg(var::text_disabled) | width(10),
                           text_owned(k.title) | font(t.font_size * 0.88f) | semibold | fg(var::text_primary) | ellipsis | grow() | min_size(0, 0),
                           node(k.host ? host_badge(c, *k.host, true) : Node{}),
                           node(agent_badge(c, k.agent, time, false)))
                           | gap(t.spaced(6)) | align(Align::center) | pad(t.spaced(6), t.spaced(8)) | width(pct(100)) | bg(var::surface),
                         v(node(std::move(k.preview))) | height(150) | width(pct(100)) | clip,
                         node(hot && !k.agent.message.empty()
                                  ? (h(text_owned(k.agent.message) | font(t.font_size * 0.8f) | fg(tone) | ellipsis)
                                     | pad(t.spaced(4), t.spaced(8)) | width(pct(100)) | bg(t.tint(tone, 0.12f))).build()
                                  : Node{}))
                       | grow() | width(0) | radius(t.radius_small) | clip
                       | border(hot || hov ? 2.0f : 1.0f, hot ? tone : hov ? t.accent : t.border)
                       | elevation(hov ? 10.0f : 2.0f) | pointer_cursor
                       | role(a11y::Role::button) | label(k.title) | id_of(id, i)).build());
        if (static_cast<int>(row.size()) == columns || i + 1 == cards.size()) {
            while (static_cast<int>(row.size()) < columns) row.push_back((box() | grow() | width(0)).build());
            grid_rows.push_back(h_list(std::move(row), 14.0f).build());
            row.clear();
        }
    }
    Vec2 sz{1200.0f, 800.0f};
    if constexpr (requires { c.c.size; }) sz = c.c.size;
    else if constexpr (requires { c.size; }) sz = c.size;
    const int waiting = static_cast<int>(std::count_if(cards.begin(), cards.end(), [](const PaneCard& k) { return wants_attention(k.agent.state); }));
    c.overlay(Overlay{
        .content = (v(h(text<"All panes"> | font(t.font_size * 1.2f) | semibold | fg(var::text_primary),
                        text_owned(std::to_string(cards.size()) + " panes \u00B7 " + std::to_string(waiting) + " waiting on you")
                            | font(t.font_size * 0.88f) | fg(waiting ? t.warning : t.text_secondary),
                        spacer(),
                        text<"1-9 open \u00B7 esc close"> | font(t.font_size * 0.8f) | fg(var::text_disabled))
                        | gap(t.spaced(12)) | align(Align::center) | width(pct(100)),
                      v(v_list(std::move(grid_rows), 14.0f)) | width(pct(100)))
                    | gap(t.spaced(16)) | pad(t.spaced(24), t.spaced(28)) | size(sz.x * 0.9f, sz.y * 0.86f)
                    | bg(var::background) | border(t.stroke(1), t.border_strong) | radius(t.radius_medium) | elevation(t.lift(30))
                    | role(a11y::Role::dialog) | label("pane overview")).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::centered,
        .dismiss = Dismiss::on_click_outside,
        .layer = 20,
        .scrim = 0.55f,
    });
}

}  // namespace mayag::dsl
