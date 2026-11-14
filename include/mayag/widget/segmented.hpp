#pragma once
// mayag::widget segmented — the compact one-of-N for 2-5 short options
//
// Interaction: Clicked / Activated on part_id(id, i) -> select option i.

#include "control.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline auto segmented(const C& c, std::string_view id,
                                    const std::vector<std::string_view>& options, int chosen) {
    const Theme& t = c.theme;
    std::vector<Node> segs;
    segs.reserve(options.size());
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto pid = part_id(id, i);
        const bool on  = static_cast<int>(i) == chosen;
        const bool hov = c.hovered(pid);
        segs.push_back((h(text_owned(std::string{options[i]})
                            | font(t.font_size * 0.92f)
                            | (on ? semibold : weight(FontWeight::regular))
                            | fg(on ? t.text_primary : hov ? t.text_primary : t.text_secondary))
                        | pad(5, 12) | center
                        | bg(on ? t.surface_raised : (hov ? t.surface_raised.fade(0.45f)
                                                          : t.surface_raised.fade(0.0f)))
                        | border(1, c.focused(pid) ? t.focus_ring
                                                   : (on ? t.border : t.border.fade(0.0f)))
                        | radius(t.radius_small - 2.0f)
                        | elevation(on ? 1.0f : 0.0f)
                        | pointer_cursor
                        | role(a11y::Role::radio) | checked(on) | label(options[i])
                        | id_of(id, i)).build());
    }
    return h(h_list(std::move(segs), 2.0f))
         | pad(3)
         | bg(t.surface)
         | border(1, t.border)
         | radius(t.radius_small)
         | role(a11y::Role::group) | label(id);
}

}  // namespace mayag::dsl
