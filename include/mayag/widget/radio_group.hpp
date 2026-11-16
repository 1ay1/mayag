#pragma once
// mayag::widget radio_group — one of N, as a vertical list
//
// Interaction: Clicked / Activated on part_id(id, i) -> select option i.

#include "control.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline auto radio_group(const C& c, std::string_view id,
                                      const std::vector<std::string_view>& options, int chosen) {
    const Theme& t = c.theme;
    std::vector<Node> rows;
    rows.reserve(options.size());
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto pid = part_id(id, i);
        const bool on  = static_cast<int>(i) == chosen;
        const bool hov = c.hovered(pid);
        rows.push_back((h(z(box() | size(8, 8) | bg(t.on_accent.fade(on ? 1.0f : 0.0f)) | radius(t.rounded(4)))
                              | size(18, 18) | center
                              | bg(on ? t.accent : t.surface)
                              | border(t.stroke(1.5f), detail::edge(t, c.focused(pid), hov,
                                                          on ? t.accent : t.border_strong))
                              | radius(t.rounded(9)),
                            text_owned(std::string{options[i]}) | font(t.font_size)
                              | fg(var::text_primary))
                        | gap(t.spaced(10)) | align(Align::center)
                        | pointer_cursor
                        | role(a11y::Role::radio) | checked(on) | label(options[i])
                        | id_of(id, i)).build());
    }
    return v(v_list(std::move(rows), 10.0f)) | role(a11y::Role::group) | label(id);
}

}  // namespace mayag::dsl
