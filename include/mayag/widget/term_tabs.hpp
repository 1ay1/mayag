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
    std::string detail;            ///< a second line for the tooltip: the cwd, the command
};

inline constexpr std::size_t term_part_close = part_dec + 4096;

/// `grab_mods...` are responders piped onto the strip's empty grab handle
/// (part_bar): `on_drag(Move{})` for a client-decorated window.
///
/// Shape, height, type and widths come from the theme (Theme::tab_shape,
/// bar_height, chrome_type, tab_width), colours from its Sheet (strip, tab,
/// mark). The ACTIVE tab is drawn in var::background -- the colour of the
/// content under it -- and, in the `block` shape, has no border at its foot,
/// so it reads as the top of the pane it shows rather than as a button.
template <ControlContext C, Responder... Grab>
[[nodiscard]] inline Node term_tabs(const C& c, std::string_view id, const std::vector<TermTab>& tabs, int active,
                                    double time = 0.0, Grab... grab_mods) {
    const Theme& t = c.theme;
    const Sheet& sh = sheet_of(t);
    const float H = t.bar_height;
    const float fs = t.font_size * t.chrome_type;
    const TabShape shape = t.tab_shape;
    const bool nerd = t.icons == IconSet::nerd;
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
            status = (text_owned(agent_glyph(tb.agent.state, time)) | font(fs * 0.92f) | bold
                      | fg(agent_tone(t, tb.agent.state))).build();
        else if (tb.bell) status = (text_owned(nerd ? "\uF0F3" : "\u25B2") | font(fs * 0.8f) | fg(var::warning)).build();
        else if (tb.running) status = (text_owned(spinner_frame(time)) | font(fs) | fg(var::accent)).build();
        else if (tb.exit_code > 0) status = (text_owned(std::to_string(tb.exit_code)) | font(fs * 0.78f) | bold
                                             | fg(var::danger) | pad(0, 4) | border(t.stroke(1), t.danger.fade(0.6f))
                                             | radius(t.rounded(3))).build();
        else if (tb.activity && !on) status = (text<"\u25CF"> | font(fs * 0.76f) | fg(var::warning)).build();
        else if (t.tab_numbers && i < 9) status = (text_owned(std::to_string(i + 1)) | font(fs * 0.85f)
                                                   | fg(on ? var::accent : var::text_disabled)).build();
        // The close cross: always takes its space so titles never shift.
        const bool show_close = !t.tab_close_on_hover || hov || on;
        Node close = (h(text_owned(nerd ? "\uF00D" : "\u00D7") | font(nerd ? fs * 0.82f : fs * 1.05f)
                        | fg(c.hovered(cid) ? t.text_primary : t.text_disabled))
                      | size(H * 0.55f, H * 0.55f) | center | radius(t.rounded(4))
                      | bg(c.hovered(cid) ? t.surface_raised : t.surface.fade(0.0f))
                      | opacity(show_close ? 1.0f : 0.0f) | transition()
                      | pointer_cursor | label("close tab") | tooltip("Close tab", "ctrl shift w") | id_of(id, term_part_close + i)).build();
        const std::string label_text = tb.process.empty() || tb.title.find(tb.process) != std::string::npos
                                           ? tb.title : tb.process + " \u2014 " + tb.title;
        Node bar = tb.progress >= 0
            ? (z(box() | width(pct(static_cast<float>(tb.progress))) | height(2) | bg(tb.progress_error ? t.danger : t.info))
               | width(pct(100)) | height(2) | bg(var::border) | absolute(0, H - 2)).build()
            : Node{};
        // An ssh tab is coloured by its environment: a prod tab is red, always.
        const bool remote = tb.host.has_value();
        const Color<Srgb> stripe = remote && tb.host->env != SshHost::Env::none ? env_tone(t, tb.host->env)
                                 : wants_attention(tb.agent.state) ? agent_tone(t, tb.agent.state) : accent;
        const bool tinted = tb.color || (remote && tb.host->env != SshHost::Env::none) || wants_attention(tb.agent.state);

        // The active mark, by shape. Sheet::mark lights it when the tab (its
        // group) is selected; a tinted tab (env, agent, colour) shows its
        // tint on every tab, brighter when active.
        Node mark_node;
        if (shape == TabShape::underline || shape == TabShape::overline || shape == TabShape::block) {
            const float y = shape == TabShape::underline ? H - t.mark : 0.0f;
            auto mark = box() | size(1e4f, t.mark) | absolute(0, y);
            mark_node = tinted ? (std::move(mark) | bg(on ? stripe : stripe.fade(0.5f))).build()
                               : (std::move(mark) | with_look(sh.mark, t)).build();
        }

        const float inset = shape == TabShape::pill ? t.spaced(5) : 0.0f;
        auto content = h(node(std::move(status)),
                         node(remote ? host_badge(c, *tb.host, true) : Node{}),
                         text_owned(label_text) | font(fs) | (on ? semibold : weight(FontWeight::regular))
                             | ellipsis | grow() | min_size(0, 0),
                         text_owned(tb.readonly ? (nerd ? "\uF023" : "\U0001F512") : "") | font(fs * 0.85f),
                         node(std::move(close)))
                     | gap(t.spaced(7)) | align(Align::center) | pad(0, t.spaced(6), 0, t.spaced(12))
                     | width(pct(100)) | height(pct(100));
        const float w = (remote ? 1.2f : 1.0f) * t.tab_width;
        std::string tip = label_text;
        if (!tb.detail.empty()) tip += "\n" + tb.detail;
        if (tb.exit_code > 0) tip += "\nexited " + std::to_string(tb.exit_code);

        Node tab;
        if (shape == TabShape::pill) {
            // A floating pill inside the strip's height.
            tab = (z(std::move(content))
                   | width(w) | height(H - 2.0f * inset) | shrink() | min_size(90, H - 2.0f * inset)
                   | radius(t.capsule(H - 2.0f * inset))
                   | bg(on ? t.background : (hov ? t.surface_raised.fade(0.55f) : t.surface.fade(0.0f)))
                   | border(t.stroke(1), on ? (tinted ? stripe.fade(0.7f) : t.border) : t.border.fade(0.0f))
                   | transition() | group() | clip | pointer_cursor
                   | role(a11y::Role::tab) | selected(on) | label(label_text) | tooltip(tip) | id_of(id, i)).build();
            items.push_back((h(std::move(tab)) | height(H) | pad(0, 2) | align(Align::center)).build());
            continue;
        }
        if (shape == TabShape::slant) {
            // Powerline: the tab's body, then a wedge in the body's colour
            // pointing into the next tab.
            const Color<Srgb> body = on ? (tinted ? stripe : t.accent) : (hov ? t.surface_raised : t.surface);
            const Color<Srgb> ink = on ? t.on_accent : (hov ? t.text_primary : t.text_secondary);
            auto lit = h(std::move(content)) | fg(ink) | width(w) | height(H) | shrink() | min_size(90, H) | bg(body)
                     | transition() | group() | clip | pointer_cursor
                     | role(a11y::Role::tab) | selected(on) | label(label_text) | tooltip(tip) | id_of(id, i);
            auto wedge = h(text<"\uE0B0"> | font(H * 1.02f) | line_height(1.0f) | fg(body)) | size(H * 0.5f, H) | center
                       | bg(var::surface) | clip;
            items.push_back((h(std::move(lit), std::move(wedge)) | height(H)).build());
            continue;
        }
        // underline / overline / block: a cell of the strip. The active one
        // is the content's colour; block drops its foot border so the tab
        // and the pane below are one surface.
        // block: the active tab paints over the strip's foot hairline in the
        // content's colour -- it opens into the pane below instead of
        // sitting on a line.
        Node foot = shape == TabShape::block && on
            ? Node{} : (box() | size(1e4f, 1.0f) | absolute(0, H - 1.0f) | bg(var::border)).build();
        // The wash: the Sheet's tab_wash, lit on the selected tab, falling
        // away from the indicator's edge (flipped for an underline).
        Node wash = (shape == TabShape::underline || shape == TabShape::overline || shape == TabShape::block)
            ? (box() | size(1e4f, H) | absolute(0, 0) | with_look(sh.tab_wash, t)
               | when(shape == TabShape::underline, rotate(180.0f))).build()
            : Node{};
        tab = (z(node(std::move(wash)), std::move(content), node(std::move(mark_node)), node(std::move(bar)), node(std::move(foot)))
               | width(w) | height(H) | shrink() | min_size(90, H)
               | with_look(sh.tab, t)
               | when(wants_attention(tb.agent.state) && !on, bg(t.tint(agent_tone(t, tb.agent.state), 0.08f)))
               | group() | clip | pointer_cursor
               | role(a11y::Role::tab) | selected(on) | label(label_text)
               | tooltip(tip) | id_of(id, i)).build();
        items.push_back(std::move(tab));
    }
    const auto plus = part_id(id, part_inc), menu = part_id(id, part_menu);
    items.push_back((h(text_owned(nerd ? "\uF067" : "+") | font(nerd ? fs * 0.9f : fs * 1.3f)
                       | fg(c.hovered(plus) ? t.text_primary : t.text_secondary))
                     | size(H, H) | center | bg(c.hovered(plus) ? t.surface_raised : t.surface.fade(0.0f)) | transition()
                     | pointer_cursor | label("new tab") | tooltip("New tab", "ctrl shift t") | id_of(id, part_inc)).build());
    items.push_back((h(text_owned(nerd ? "\uF078" : "\u2304") | font(fs * 0.78f)
                       | fg(c.hovered(menu) ? t.text_primary : t.text_disabled))
                     | size(H * 0.62f, H) | center | pointer_cursor | label("new tab menu") | tooltip("Commands", "ctrl shift p")
                     | id_of(id, part_menu)).build());
    // Everything after the tabs stands on the strip's foot line.
    const auto footed = [&](Node n) {
        return (z(std::move(n), box() | size(1e4f, 1.0f) | absolute(0, H - 1.0f) | bg(var::border)) | height(H) | clip).build();
    };
    { Node mn = std::move(items.back()); items.pop_back(); Node pl = std::move(items.back()); items.pop_back();
      items.push_back(footed(std::move(pl))); items.push_back(footed(std::move(mn))); }
    // The empty rest of the strip is the window's grab handle (part_bar).
    Node handle = (box() | grow() | height(pct(100)) | min_size(24, 0) | mayag::dsl::cursor(CursorShape::grab)
                   | label("move window") | id_of(id, part_bar)).build();
    if constexpr (sizeof...(Grab) > 0) handle = (node(std::move(handle)) | ... | grab_mods).build();
    handle = (z(std::move(handle), box() | size(1e4f, 1.0f) | absolute(0, H - 1.0f) | bg(var::border))
              | grow() | height(pct(100)) | min_size(24, 0) | clip).build();
    // The tabs shrink-wrap; the handle is their SIBLING so it takes the rest.
    return (h(h(h_list(std::move(items))) | shrink() | min_size(0, 0) | height(pct(100)) | align(Align::center) | clip,
              node(std::move(handle)))
            | height(H) | width(pct(100)) | align(Align::center) | clip
            | with_look(sh.strip, t)).build();
}

}  // namespace mayag::dsl
