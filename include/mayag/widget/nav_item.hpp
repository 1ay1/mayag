#pragma once
// mayag::widget nav_item — a sidebar destination
//
// Interaction: Clicked / Activated on part_id(base, index) -> go there.

#include "control.hpp"

#include <string>
#include <string_view>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline Node nav_item(const C& c, std::string_view base, std::size_t index,
                                   std::string_view icon, std::string_view name, bool active) {
    const Theme& t = c.theme;
    const auto pid = part_id(base, index);
    const bool hov = c.hovered(pid);
    return (h(text_owned(std::string{icon}) | font(t.font_size) | fg(active ? t.accent : t.text_secondary)
                  | text_align(TextAlign::center) | width(18),
              text_owned(std::string{name}) | font(t.font_size)
                  | (active ? semibold : weight(FontWeight::regular))
                  | fg(active || hov ? t.text_primary : t.text_secondary))
            | gap(10) | align(Align::center) | pad(7, 10) | width(pct(100))
            | bg(active ? t.surface_raised : (hov ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f)))
            | border(1, c.focused(pid) ? t.focus_ring : t.border.fade(0.0f))
            | radius(t.radius_small)
            | pointer_cursor
            | role(a11y::Role::tab) | selected(active) | label(name)
            | id_of(base, index)).build();
}

}  // namespace mayag::dsl
