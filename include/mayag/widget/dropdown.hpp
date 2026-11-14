#pragma once
// mayag::widget dropdown — a select whose options open in an overlay
//
// The trigger draws in place; when `open`, the option list is posted as an
// OVERLAY, so it escapes a scrolling form's clip and paints above it.
// Interaction:
//   Clicked on node_id(id)            -> toggle open
//   Clicked on part_id(id, i)         -> select i, close
//   Left    on part_id(id, part_menu) -> the click-outside dismissal: close
// The context must also be able to post overlays (mayag::Ctx can).

#include "control.hpp"
#include "../scene/overlay.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline auto dropdown(const C& c, std::string_view id,
                                   const std::vector<std::string>& options, int chosen,
                                   bool open, float width_px = 200.0f) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool hov = c.hovered(nid);
    const std::string current = chosen >= 0 && chosen < static_cast<int>(options.size())
                                    ? options[static_cast<std::size_t>(chosen)]
                                    : std::string{"\u2014"};
    if (open) {
        std::vector<Node> rows;
        rows.reserve(options.size());
        for (std::size_t i = 0; i < options.size(); ++i) {
            const auto pid = part_id(id, i);
            const bool on = static_cast<int>(i) == chosen;
            rows.push_back((h(text_owned(on ? "\u2713" : " ") | font(t.font_size * 0.9f)
                                  | fg(t.accent) | width(16),
                              text_owned(options[i]) | font(t.font_size)
                                  | fg(t.text_primary) | grow())
                            | align(Align::center) | pad(7, 10) | width(pct(100))
                            | bg(c.hovered(pid) ? t.accent.fade(0.18f) : t.overlay.fade(0.0f))
                            | radius(t.radius_small - 2.0f)
                            | pointer_cursor
                            | role(a11y::Role::menu_item) | selected(on)
                            | id_of(id, i)).build());
        }
        c.overlay(Overlay{
            .content = (v(v_list(std::move(rows), 1.0f))
                        | pad(4) | width(width_px)
                        | bg(t.overlay) | border(1, t.border_strong)
                        | radius(t.radius_medium) | elevation(12)
                        | role(a11y::Role::menu)).build(),
            .id = part_id(id, part_menu),
            .anchor_id = nid,
            .placement = Placement::below_start,
            .dismiss = Dismiss::on_click_outside,
        });
    }
    return h(text_owned(current) | font(t.font_size) | fg(t.text_primary) | grow(),
             text<"\u25BE"> | font(t.font_size * 0.85f) | fg(t.text_secondary))
         | align(Align::center) | gap(8) | pad(7, 11) | width(width_px)
         | bg(hov || open ? t.surface_raised : t.surface)
         | border(1, detail::edge(t, c.focused(nid) || open, hov, t.border))
         | radius(t.radius_small)
         | pointer_cursor
         | role(a11y::Role::button) | label(id)
         | id_of(id);
}

}  // namespace mayag::dsl
