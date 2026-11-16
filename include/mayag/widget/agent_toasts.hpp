#pragma once
// mayag::widget notification_toast — an agent wants you, from another tab
//
//   agent_toasts(c, "notes", notes, window_size, time)
//
// When an agent in a background pane finishes or asks for approval, the
// terminal surfaces it where your eyes are: a stacked toast in the corner
// with the agent's message and actions that act on THAT pane without
// switching to it — Approve (sends y), Deny (n), Go to pane. (The app also
// posts a system notification when the window is not focused.)
//
// Interaction: part_id(id, i * 4 + k) for note_action::{go, approve, deny, dismiss}.

#include "control.hpp"
#include "agent_state.hpp"
#include "../scene/overlay.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

namespace note_action { inline constexpr std::size_t go = 0, approve = 1, deny = 2, dismiss = 3; }

struct AgentNote {
    AgentInfo   agent;
    std::string where;
};

template <ControlContext C>
inline void agent_toasts(const C& c, std::string_view id, const std::vector<AgentNote>& notes, Vec2 window, double time) {
    if (notes.empty()) return;
    const Theme& t = c.theme;
    std::vector<Node> cards;
    for (std::size_t i = 0; i < notes.size() && i < 4; ++i) {
        const auto& n = notes[i];
        const Color<Srgb> tone = agent_tone(t, n.agent.state);
        const auto btn = [&](std::size_t k, const char* label_, bool primary) {
            const auto pid = part_id(id, i * 4 + k);
            const bool hov = c.hovered(pid);
            return (h(text_owned(label_) | font(t.font_size * 0.82f) | semibold | fg(primary ? t.on_accent : hov ? t.text_primary : t.text_secondary))
                    | pad(t.spaced(3), t.spaced(9)) | radius(t.rounded(4)) | bg(primary ? (hov ? mix(tone, colors::white, 0.12f) : tone) : hov ? t.surface_raised : t.surface)
                    | border(t.stroke(1), primary ? tone : t.border) | pointer_cursor | id_of(id, i * 4 + k)).build();
        };
        const auto did = part_id(id, i * 4 + note_action::dismiss);
        const bool approval = n.agent.state == AgentState::needs_approval;
        cards.push_back((h(box() | width(3) | bg(tone),
                           v(h(text_owned(agent_glyph(n.agent.state, time)) | font(t.font_size * 0.85f) | bold | fg(tone),
                               text_owned(n.agent.agent) | font(t.font_size * 0.9f) | semibold | fg(var::text_primary),
                               text_owned(agent_state_words(n.agent.state)) | font(t.font_size * 0.8f) | fg(tone),
                               spacer(),
                               h(text<"\u00D7"> | font(t.font_size * 0.9f) | fg(c.hovered(did) ? t.text_primary : t.text_disabled))
                                   | pointer_cursor | id_of(id, i * 4 + note_action::dismiss))
                                 | gap(t.spaced(6)) | align(Align::center) | width(pct(100)),
                             text_owned(n.agent.message) | font(t.font_size * 0.88f) | fg(var::text_secondary) | wrap_text | clamp_lines(3),
                             text_owned(n.where) | font(t.font_size * 0.75f) | fg(var::text_disabled),
                             h(node(approval ? btn(note_action::approve, "Approve  y", true) : btn(note_action::go, "Go to pane", true)),
                               node(approval ? btn(note_action::deny, "Deny  n", false) : Node{}),
                               node(approval ? btn(note_action::go, "Go to pane", false) : Node{}))
                                 | gap(t.spaced(6)))
                               | gap(t.spaced(5)) | pad(t.spaced(9), t.spaced(11)) | grow() | min_size(0, 0))
                         | align(Align::stretch) | width(360)
                         | bg(var::overlay) | border(t.stroke(1), t.tint(tone, 0.45f)) | radius(t.radius_small) | clip | elevation(t.lift(16))
                         | role(a11y::Role::dialog) | label(n.agent.agent + " " + agent_state_words(n.agent.state))).build());
    }
    const float est = static_cast<float>(std::min<std::size_t>(notes.size(), 4)) * 132.0f;
    c.overlay(Overlay{
        .content = v_list(std::move(cards), 8.0f).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::cursor,
        .dismiss = Dismiss::never,
        .layer = 22,
        .at = Vec2{window.x - 376.0f, std::max(48.0f, window.y - 40.0f - est)},
    });
}

}  // namespace mayag::dsl
