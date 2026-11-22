#pragma once
// mayag::widget agent_state — what a coding agent running in a pane is doing
//
// The terminal does not host the agent: Claude Code, Codex, aider ... run as
// ordinary programs in a pty. The terminal OBSERVES them (process name,
// OSC 9/777 notifications, bell, title, OSC 9;4 progress, output activity)
// and turns that into one state per pane, which every agent-aware widget
// (tabs, attention queue, overview, worktree list) draws the same way.
//
//   agent_badge(c, {.agent = "claude", .state = AgentState::needs_input}, time)

#include "control.hpp"

#include <string>

namespace mayag::dsl {

enum class AgentState : std::uint8_t {
    none,         ///< no agent in this pane
    working,      ///< producing output / running tools
    needs_input,  ///< waiting for the user's next message
    needs_approval,  ///< asked to run a command / edit a file
    done,         ///< finished its task (notification arrived)
    error,        ///< crashed or reported failure
};

struct AgentInfo {
    std::string agent;          ///< "claude", "codex", "aider", "gemini"
    AgentState  state = AgentState::none;
    std::string message;        ///< the last notification: "Claude needs your permission to use Bash"
    double      since = 0.0;    ///< seconds in this state
};

/// A braille spinner frame for `time` seconds.
[[nodiscard]] inline const char* spinner_frame(double time) {
    static constexpr const char* frames[] = {"\u280B", "\u2819", "\u2839", "\u2838", "\u283C",
                                             "\u2834", "\u2826", "\u2827", "\u2807", "\u280F"};
    return frames[static_cast<int>(time * 12.0) % 10];
}

[[nodiscard]] inline Color<Srgb> agent_tone(const Theme& t, AgentState s) {
    switch (s) {
        case AgentState::working: return t.info;
        case AgentState::needs_input: return t.accent;
        case AgentState::needs_approval: return t.warning;
        case AgentState::done: return t.success;
        case AgentState::error: return t.danger;
        default: return t.text_disabled;
    }
}
[[nodiscard]] inline const char* agent_state_words(AgentState s) {
    switch (s) {
        case AgentState::working: return "working";
        case AgentState::needs_input: return "your turn";
        case AgentState::needs_approval: return "needs approval";
        case AgentState::done: return "done";
        case AgentState::error: return "error";
        default: return "";
    }
}
/// True for the states that should pull the user back to the pane.
[[nodiscard]] constexpr bool wants_attention(AgentState s) noexcept {
    return s == AgentState::needs_input || s == AgentState::needs_approval || s == AgentState::done || s == AgentState::error;
}

[[nodiscard]] inline std::string agent_glyph(AgentState s, double time) {
    switch (s) {
        case AgentState::working: return spinner_frame(time);
        case AgentState::needs_input: return "\u25C6";
        case AgentState::needs_approval: return "\u25B2";
        case AgentState::done: return "\u2713";
        case AgentState::error: return "\u2716";
        default: return "";
    }
}

/// Compact pill: glyph + agent name + state words.
template <ControlContext C>
[[nodiscard]] inline Node agent_badge(const C& c, const AgentInfo& a, double time, bool words = true) {
    const Theme& t = c.theme;
    if (a.state == AgentState::none) return Node{};
    const Color<Srgb> tone = agent_tone(t, a.state);
    return (h(text_owned(agent_glyph(a.state, time)) | font(t.type(Tier::small)) | bold | fg(tone),
              text_owned(a.agent) | font(t.type(Tier::small)) | semibold | fg(var::text_primary),
              text_owned(words ? agent_state_words(a.state) : "") | font(t.type(Tier::small)) | fg(tone))
            | gap(t.spaced(5)) | align(Align::center) | pad(t.spaced(1), t.spaced(7)) | radius(t.rounded(9))
            | bg(t.tint(tone, 0.14f)) | border(t.stroke(1), t.tint(tone, 0.32f))).build();
}

}  // namespace mayag::dsl
