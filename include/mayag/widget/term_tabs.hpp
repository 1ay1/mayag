#pragma once
// mayag::widget term_tabs — terminal tab bar
//
//   term_tabs(c, "tabs", tabs, active)
//
// What a terminal tab has that an editor tab does not: the running process
// (with a spinner-free activity dot when output arrived in a background tab),
// a bell marker, the last command's exit status, an OSC 9;4 progress bar
// under the title, the tab number (cmd-1..9), a "+" button and a profile
// dropdown chevron beside it.
//
// Interaction:
//   Clicked on part_id(id, i)              -> activate tab i
//   Clicked on part_id(id, part_close + i) -> close tab i
//   Clicked on part_id(id, part_inc)       -> new tab
//   Clicked on part_id(id, part_menu)      -> new-tab menu (profiles)

#include "control.hpp"
#include "agent_state.hpp"
#include "sheet.hpp"
#include "scroll_view.hpp"   // part_bar
#include "ssh.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mayag::dsl {

struct TermTab {
    std::string title;             ///< the shell's title (OSC 0/2) or the cwd
    std::string process;           ///< foreground process: "zsh", "vim", "cargo"
    bool        activity = false;  ///< output since last viewed
    bool        bell = false;
    int         exit_code = -1;    ///< last command's status; -1 unknown/running
    int         progress = -1;     ///< 0..100, -1 none
    bool        progress_error = false;
    bool        running = false;   ///< a command is executing
    bool        readonly = false;
    std::uint32_t color = 0;       ///< tab accent (0 none)
    AgentInfo   agent;             ///< a coding agent running in the tab
    std::optional<SshHost> host;   ///< the remote end of an ssh tab
};

inline constexpr std::size_t term_part_close = part_dec + 4096;

/// `grab_mods...` are responders piped onto the strip's empty grab handle
/// (part_bar): `on_press(Move{})` for a client-decorated window.
template <ControlContext C, Responder... Grab>
[[nodiscard]] inline Node term_tabs(const C& c, std::string_view id, const std::vector<TermTab>& tabs, int active,
                                    double time = 0.0, Grab... grab_mods) {
    const Theme& t = c.theme;
    std::vector<Node> items;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const TermTab& tb = tabs[i];
        const bool on = static_cast<int>(i) == active;
        const auto pid = part_id(id, i), cid = part_id(id, term_part_close + i);
        const bool hov = c.hovered(pid) || c.hovered(cid);
        const Color<Srgb> accent = tb.color ? Color<Srgb>{static_cast<float>((tb.color >> 16) & 255) / 255.0f,
                                                          static_cast<float>((tb.color >> 8) & 255) / 255.0f,
                                                          static_cast<float>(tb.color & 255) / 255.0f, 1.0f} : t.accent;
        // Status glyph left of the title: an agent's state outranks everything.
        Node status;
        if (tb.agent.state != AgentState::none)
            status = (text_owned(agent_glyph(tb.agent.state, time)) | font(t.font_size * 0.85f) | bold
                      | fg(agent_tone(t, tb.agent.state))).build();
        else if (tb.bell) status = (text<"\u25B2"> | font(t.font_size * 0.7f) | fg(var::warning)).build();
        else if (tb.running) status = (text_owned(spinner_frame(time)) | font(t.font_size * 0.9f) | fg(var::accent)).build();
        else if (tb.exit_code > 0) status = (text_owned(std::to_string(tb.exit_code)) | font(t.font_size * 0.72f) | bold
                                             | fg(var::danger) | pad(0, 4) | border(t.stroke(1), t.danger.fade(0.6f))
                                             | radius(t.rounded(3))).build();
        else if (tb.activity && !on) status = (text<"\u25CF"> | font(t.font_size * 0.7f) | fg(var::warning)).build();
        else status = (text_owned(i < 9 ? std::to_string(i + 1) : std::string{})
                       | font(t.font_size * 0.78f) | fg(var::text_disabled)).build();
        // The close cross exists where it can be aimed at: the active tab
        // and the hovered one. It always takes its space, so titles never
        // shift under the pointer.
        Node close = (h(text<"\u00D7"> | font(t.font_size) | fg(c.hovered(cid) ? t.text_primary : t.text_secondary))
                      | size(18, 18) | center | radius(t.rounded(4))
                      | bg(c.hovered(cid) ? t.surface_raised : t.surface.fade(0.0f))
                      | opacity(hov || on ? 1.0f : 0.0f) | transition()
                      | pointer_cursor | label("close tab") | id_of(id, term_part_close + i)).build();
        const std::string label_text = tb.process.empty() || tb.title.find(tb.process) != std::string::npos
                                           ? tb.title : tb.process + " \u2014 " + tb.title;
        Node bar = tb.progress >= 0
            ? (z(box() | width(pct(static_cast<float>(tb.progress))) | height(2) | bg(tb.progress_error ? t.danger : t.info))
               | width(pct(100)) | height(2) | bg(var::border) | absolute(0, 32)).build()
            : Node{};
        // An ssh tab is coloured by its environment: a prod tab is red, always.
        const bool remote = tb.host.has_value();
        const Color<Srgb> stripe = remote && tb.host->env != SshHost::Env::none ? env_tone(t, tb.host->env)
                                 : wants_attention(tb.agent.state) ? agent_tone(t, tb.agent.state) : accent;
        const bool tinted = tb.color || (remote && tb.host->env != SshHost::Env::none) || wants_attention(tb.agent.state);
        // The active mark: the Sheet's `mark` lights it when the tab (its
        // group) is selected; an environment/agent tint shows on all tabs.
        auto mark = box() | size(1e4f, 2) | absolute(0, 0);
        Node mark_node = tinted ? (std::move(mark) | bg(on ? stripe : stripe.fade(0.55f))).build()
                                : (std::move(mark) | with_look(sheet_of(t).mark, t)).build();
        items.push_back((z(h(node(std::move(status)) ,
                             node(remote ? host_badge(c, *tb.host, true) : Node{}),
                             text_owned(label_text) | font(t.font_size) | (on ? semibold : weight(FontWeight::regular))
                                 | ellipsis | grow() | min_size(0, 0),
                             text_owned(tb.readonly ? "\U0001F512" : "") | font(t.font_size * 0.8f),
                             node(std::move(close)))
                               | gap(t.spaced(8)) | align(Align::center) | pad(0, t.spaced(8), 0, t.spaced(12)) | width(pct(100)) | height(pct(100)),
                           node(std::move(mark_node)),
                           node(std::move(bar)))
                         | width(remote ? 240.0f : 200.0f) | height(34) | shrink() | min_size(90, 34)
                         | with_look(sheet_of(t).tab, t)
                         | when(wants_attention(tb.agent.state) && !on, bg(t.tint(agent_tone(t, tb.agent.state), 0.08f)))
                         | group() | clip | pointer_cursor
                         | role(a11y::Role::tab) | selected(on) | label(label_text)
                         | id_of(id, i)).build());
    }
    const auto plus = part_id(id, part_inc), menu = part_id(id, part_menu);
    items.push_back((h(text<"+"> | font(t.font_size * 1.3f) | fg(c.hovered(plus) ? t.text_primary : t.text_secondary))
                     | size(32, 34) | center | bg(c.hovered(plus) ? t.surface_raised : t.surface.fade(0.0f)) | transition()
                     | pointer_cursor | label("new tab") | id_of(id, part_inc)).build());
    items.push_back((h(text<"\u2304"> | font(t.font_size) | fg(c.hovered(menu) ? t.text_primary : t.text_disabled))
                     | size(22, 34) | center | pointer_cursor | label("new tab menu") | id_of(id, part_menu)).build());
    // The empty rest of the strip is the window's grab handle (part_bar):
    // a host without server decorations moves the window from there.
    Node handle = (box() | grow() | height(pct(100)) | min_size(24, 0) | mayag::dsl::cursor(CursorShape::grab)
                   | label("move window") | id_of(id, part_bar)).build();
    if constexpr (sizeof...(Grab) > 0) handle = (node(std::move(handle)) | ... | grab_mods).build();
    // The tabs shrink-wrap; the handle is their SIBLING in the strip so it
    // takes all the width that is left.
    return (h(h(h_list(std::move(items))) | shrink() | min_size(0, 0) | height(pct(100)) | align(Align::center) | clip,
              node(std::move(handle)))
            | height(34) | width(pct(100)) | align(Align::center) | clip
            | with_look(sheet_of(t).strip, t)).build();
}

}  // namespace mayag::dsl
