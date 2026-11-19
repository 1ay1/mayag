#pragma once
// mayag::widget icon_button — a glyph button for toolbars and window chrome
//
//   icon_button(c, "win.close", "\uF00D", {.tooltip = "Close window", .keys = "ctrl shift q", .danger = true})
//       | on_click(Close{})
//
// Square by default, as tall as the theme's bar (Theme::bar_height), so a
// row of them lines up with the tab strip and the status bar. The glyph is
// set in the chrome type size; any string works (a Nerd Font icon, a
// Unicode symbol, a short label).
//
// Behaviour here: id, role, pointer cursor, tooltip (shown by the host),
// `active` (a toggled tool) as the `selected` state. Look in the Sheet:
// Sheet::icon_button and Sheet::icon_button_danger, every state variant.
//
// Interaction: Clicked / Activated on node_id(id); pipe on_click(...) to
// carry the message.

#include "control.hpp"
#include "sheet.hpp"

#include <string>
#include <string_view>

namespace mayag::dsl {

struct IconButtonProps {
    std::string_view tooltip{};   ///< what it does; also its accessible label
    std::string_view keys{};      ///< the shortcut, shown in the tooltip
    float width = 0.0f;           ///< 0: square
    float height = 0.0f;          ///< 0: Theme::bar_height
    float glyph = 1.0f;           ///< glyph size, a multiple of the chrome type size
    bool  danger = false;         ///< destructive (close window): the danger look
    bool  active = false;         ///< a toggled tool that is on
};

template <ControlContext C>
[[nodiscard]] inline auto icon_button(const C& c, std::string_view id, std::string_view glyph,
                                      const IconButtonProps& p = {}) {
    const Theme& t = c.theme;
    const Sheet& s = sheet_of(t);
    const float hgt = p.height > 0.0f ? p.height : t.bar_height;
    const float w = p.width > 0.0f ? p.width : hgt;
    return h(text_owned(std::string{glyph}) | font(t.font_size * t.chrome_type * 1.1f * p.glyph))
         | size(w, hgt) | center
         | with_look(p.danger ? s.icon_button_danger : s.icon_button, t)
         | selected(p.active)
         | pointer_cursor | role(a11y::Role::button) | label(p.tooltip.empty() ? id : p.tooltip)
         | tooltip(p.tooltip, p.keys)
         | id_of(id);
}

}  // namespace mayag::dsl
