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
};

inline constexpr std::size_t term_part_close = part_dec + 4096;

template <ControlContext C>
[[nodiscard]] inline Node term_tabs(const C& c, std::string_view id, const std::vector<TermTab>& tabs, int active) {
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
        // Status glyph left of the title.
        Node status;
        if (tb.bell) status = (text<"\U0001F514"> | font(t.font_size * 0.8f)).build();
        else if (tb.running) status = (text<"\u25CF"> | font(t.font_size * 0.7f) | fg(t.info)).build();
        else if (tb.exit_code > 0) status = (text<"\u2716"> | font(t.font_size * 0.75f) | fg(t.danger)).build();
        else if (tb.activity && !on) status = (text<"\u25CF"> | font(t.font_size * 0.7f) | fg(t.warning)).build();
        else status = (text_owned(i < 9 ? std::string{"\u2318"} + std::to_string(i + 1) : std::string{})
                       | font(t.font_size * 0.78f) | fg(t.text_disabled)).build();
        Node close = hov
            ? (h(text<"\u00D7"> | font(t.font_size) | fg(c.hovered(cid) ? t.text_primary : t.text_secondary))
               | size(18, 18) | center | radius(4) | bg(c.hovered(cid) ? t.surface_raised : t.surface.fade(0.0f))
               | pointer_cursor | label("close tab") | id_of(id, term_part_close + i)).build()
            : (box() | size(18, 18)).build();
        const std::string label_text = tb.process.empty() || tb.title.find(tb.process) != std::string::npos
                                           ? tb.title : tb.process + " \u2014 " + tb.title;
        Node bar = tb.progress >= 0
            ? (z(box() | width(pct(static_cast<float>(tb.progress))) | height(2) | bg(tb.progress_error ? t.danger : t.info))
               | width(pct(100)) | height(2) | bg(t.border) | absolute(0, 32)).build()
            : Node{};
        items.push_back((z(h(node(std::move(status)) ,
                             text_owned(label_text) | font(t.font_size) | fg(on ? t.text_primary : t.text_secondary)
                                 | ellipsis | grow() | min_size(0, 0),
                             text_owned(tb.readonly ? "\U0001F512" : "") | font(t.font_size * 0.8f),
                             node(std::move(close)))
                               | gap(8) | align(Align::center) | pad(0, 8, 0, 12) | width(pct(100)) | height(pct(100)),
                           box() | size(1e4f, 2) | bg(on ? accent : (tb.color ? accent.fade(0.5f) : t.surface.fade(0.0f))) | absolute(0, 0),
                           node(std::move(bar)))
                         | width(200) | height(34) | shrink() | min_size(90, 34)
                         | bg(on ? t.background : hov ? mix(t.surface, t.background, 0.5f) : t.surface)
                         | border(1, t.border) | clip
                         | role(a11y::Role::tab) | selected(on) | label(label_text)
                         | id_of(id, i)).build());
    }
    const auto plus = part_id(id, part_inc), menu = part_id(id, part_menu);
    items.push_back((h(text<"+"> | font(t.font_size * 1.3f) | fg(c.hovered(plus) ? t.text_primary : t.text_secondary))
                     | size(32, 34) | center | bg(c.hovered(plus) ? t.surface_raised : t.surface.fade(0.0f))
                     | pointer_cursor | label("new tab") | id_of(id, part_inc)).build());
    items.push_back((h(text<"\u2304"> | font(t.font_size) | fg(c.hovered(menu) ? t.text_primary : t.text_disabled))
                     | size(22, 34) | center | pointer_cursor | label("new tab menu") | id_of(id, part_menu)).build());
    return (h(h_list(std::move(items))) | height(34) | width(pct(100)) | align(Align::center) | clip
            | bg(t.surface)).build();
}

}  // namespace mayag::dsl
