#pragma once
// mayag::widget tab_launcher — the new-tab menu
//
//   tab_launcher(c, "launch", part_id("tabs", part_menu), profiles, recent_dirs, keyboard_index)
//
// What the ⌄ beside "+" opens: shell profiles (each with its colour, the
// command it runs and its shortcut), recent directories to open a tab in,
// and a hint row. Arrow keys move, Enter opens, ⌘⏎ opens as a split.
//
// Interaction: part_id(id, k) profile k; part_id(id, 1000 + j) recent dir j;
//              Left on part_id(id, part_menu) -> dismissed by a click outside.

#include "control.hpp"
#include "key_hint.hpp"
#include "tab_strip.hpp"
#include "../scene/overlay.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct LaunchProfile {
    std::string   name;          ///< "zsh", "fish", "prod-db-01"
    std::string   command;       ///< "/bin/zsh -l", "ssh deploy@10.0.4.21"
    std::string   process;       ///< for the icon: "zsh", "ssh", "python"
    std::uint32_t hue = 0;
    std::string   shortcut;      ///< "\u2318T"
    bool          danger = false;
    bool          is_default = false;
};

template <ControlContext C>
inline void tab_launcher(const C& c, std::string_view id, std::uint64_t anchor, const std::vector<LaunchProfile>& profiles,
                         const std::vector<std::string>& recent, int keyboard = -1) {
    const Theme& t = c.theme;
    std::vector<Node> rows;
    const auto header = [&](const char* s) {
        rows.push_back((text_owned(s) | font(t.font_size * 0.7f) | semibold | tracking(1.0f) | fg(t.text_disabled) | pad(8, 10, 4, 10)).build());
    };
    header("PROFILES");
    for (std::size_t k = 0; k < profiles.size(); ++k) {
        const auto& p = profiles[k];
        TermTabInfo look_as;
        look_as.process = p.process;
        look_as.title = p.name;
        look_as.hue = p.hue;
        if (p.process == "ssh") look_as.host = p.name;
        const Color<Srgb> hue = tab_hue(look_as);
        const auto pid = part_id(id, k);
        const bool on = c.hovered(pid) || keyboard == static_cast<int>(k);
        rows.push_back((h(node(tab_icon(t, look_as, 0.0, on, 26.0f)),
                          v(h(text_owned(p.name) | font(t.font_size) | semibold | fg(p.danger ? t.danger : t.text_primary),
                              text_owned(p.is_default ? "default" : "") | font(t.font_size * 0.7f) | fg(t.text_disabled))
                                | gap(6) | align(Align::center),
                            text_owned(p.command) | font(t.font_size * 0.78f) | fg(t.text_secondary) | ellipsis | no_ligatures)
                              | gap(1) | grow() | min_size(0, 0),
                          text_owned(p.shortcut) | font(t.font_size * 0.8f) | fg(on ? t.text_secondary : t.text_disabled))
                        | gap(10) | align(Align::center) | pad(6, 10) | width(pct(100)) | radius(8)
                        | bg(on ? mix(t.overlay, hue, 0.16f) : t.overlay.fade(0.0f))
                        | border(1, on ? mix(t.border_strong, hue, 0.4f) : t.border.fade(0.0f))
                        | pointer_cursor | role(a11y::Role::menu_item) | label(p.name) | id_of(id, k)).build());
    }
    if (!recent.empty()) {
        header("OPEN IN");
        for (std::size_t j = 0; j < recent.size(); ++j) {
            const auto rid = part_id(id, 1000 + j);
            const bool on = c.hovered(rid) || keyboard == static_cast<int>(profiles.size() + j);
            TermTabInfo dir;
            dir.cwd = recent[j];
            const Color<Srgb> hue = tab_hue(dir);
            rows.push_back((h(box() | size(8, 8) | radius(4) | bg(hue) | margin(0, 9, 0, 9),
                              text_owned(recent[j]) | font(t.font_size * 0.9f) | fg(on ? t.text_primary : t.text_secondary) | ellipsis
                                  | grow() | min_size(0, 0))
                            | align(Align::center) | pad(5, 10) | width(pct(100)) | radius(8)
                            | bg(on ? mix(t.overlay, hue, 0.12f) : t.overlay.fade(0.0f))
                            | pointer_cursor | role(a11y::Role::menu_item) | label(recent[j]) | id_of(id, 1000 + j)).build());
        }
    }
    c.overlay(Overlay{
        .content = (v(v_list(std::move(rows), 2.0f) | pad(4) | width(pct(100)),
                      h(node(key_hint(t, "\u23CE", "tab").build()), node(key_hint(t, "\u2318\u23CE", "split").build()),
                        node(key_hint(t, "\u2325\u23CE", "window").build()))
                          | gap(12) | align(Align::center) | pad(8, 12) | width(pct(100)) | bg(t.surface))
                    | width(330) | bg(t.overlay) | border(1, t.border_strong) | radius(12) | clip | elevation(20)
                    | role(a11y::Role::menu) | label("new tab")).build(),
        .id = part_id(id, part_menu),
        .anchor_id = anchor,
        .placement = Placement::below_start,
        .dismiss = Dismiss::on_click_outside,
        .offset = 6.0f,
        .layer = 15,
    });
}

}  // namespace mayag::dsl
