#pragma once
// mayag::widget context_menu — a right-click / dropdown menu
//
//   context_menu(c, "menu", m.menu_at, items, hovered_index)
//
// Items with shortcut hints, separators, disabled entries, checkmarks and a
// danger tone, posted as an overlay at a point (right-click) or under an
// anchor (a "+ ⌄" button). The terminal uses it for Copy / Paste / Split /
// Clear / profiles.
//
// Interaction: Clicked on part_id(id, i) -> run item i; Left on
// part_id(id, part_menu) -> dismissed by a click outside.

#include "control.hpp"
#include "../scene/overlay.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mayag::dsl {

struct MenuItem {
    std::string label;
    std::string shortcut;
    bool        separator = false;   ///< draws a rule; label ignored
    bool        enabled = true;
    bool        checked = false;
    bool        danger = false;
    std::string section;             ///< a small header above this item
};

template <ControlContext C>
inline void context_menu(const C& c, std::string_view id, const std::vector<MenuItem>& items,
                         std::optional<Vec2> at, std::uint64_t anchor = 0, int keyboard_index = -1) {
    const Theme& t = c.theme;
    std::vector<Node> rows;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const MenuItem& it = items[i];
        if (!it.section.empty())
            rows.push_back((text_owned(it.section) | font(t.font_size * 0.75f) | semibold | tracking(0.6f)
                            | fg(t.text_disabled) | pad(6, 10, 2, 10)).build());
        if (it.separator) { rows.push_back((box() | height(1) | width(pct(100)) | bg(t.border) | margin(4, 0)).build()); continue; }
        const auto pid = part_id(id, i);
        const bool hov = it.enabled && (c.hovered(pid) || static_cast<int>(i) == keyboard_index);
        rows.push_back((h(text_owned(it.checked ? "\u2713" : "") | font(t.font_size * 0.9f) | fg(t.accent) | width(14),
                          text_owned(it.label) | font(t.font_size)
                              | fg(!it.enabled ? t.text_disabled : it.danger ? t.danger : hov ? t.text_primary : t.text_primary)
                              | grow() | min_size(0, 0),
                          text_owned(it.shortcut) | font(t.font_size * 0.85f) | fg(hov ? t.text_secondary : t.text_disabled))
                        | gap(8) | align(Align::center) | pad(0, 10) | height(26) | width(pct(100))
                        | bg(hov ? (it.danger ? t.tint(t.danger, 0.22f) : t.tint(t.accent, 0.22f)) : t.overlay.fade(0.0f))
                        | radius(4) | cursor(it.enabled ? CursorShape::pointer : CursorShape::arrow)
                        | role(a11y::Role::menu_item) | disabled(!it.enabled) | label(it.label)
                        | id_of(id, i)).build());
    }
    c.overlay(Overlay{
        .content = (v(v_list(std::move(rows))) | pad(4) | width(240)
                    | bg(t.overlay) | border(1, t.border_strong) | radius(t.radius_small) | elevation(16)
                    | role(a11y::Role::menu)).build(),
        .id = part_id(id, part_menu),
        .anchor_id = anchor,
        .placement = at ? Placement::cursor : Placement::below_end,
        .dismiss = Dismiss::on_click_outside,
        .layer = 15,
        .at = at,
    });
}

}  // namespace mayag::dsl
