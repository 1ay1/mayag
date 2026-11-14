#pragma once
// mayag::widget checkbox — a tick box with its label
//
// Interaction: Clicked / Activated on node_id(id) -> flip the bool. The
// label is part of the hit target, as it is on every native platform.

#include "control.hpp"

#include <string>
#include <string_view>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline auto checkbox(const C& c, std::string_view id, bool on, std::string_view caption_text) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool hov = c.hovered(nid);
    return h(z(text<"\u2713"> | font(12) | bold | fg(t.on_accent.fade(on ? 1.0f : 0.0f)))
                 | size(18, 18) | center
                 | bg(on ? t.accent : t.surface)
                 | border(1.5f, detail::edge(t, c.focused(nid), hov, on ? t.accent : t.border_strong))
                 | radius(5),
             text_owned(std::string{caption_text}) | font(t.font_size) | fg(t.text_primary))
         | gap(10) | align(Align::center)
         | pointer_cursor
         | role(a11y::Role::checkbox) | checked(on) | label(caption_text)
         | id_of(id);
}

}  // namespace mayag::dsl
