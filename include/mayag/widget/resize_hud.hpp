#pragma once
// mayag::widget resize_hud — the "120 × 40" badge shown while resizing
//
//   if (m.resizing) resize_hud(c, "hud", cols, rows);
//
// Every terminal shows the grid size while the window is dragged, because
// the pixel size means nothing to the programs inside. Centred, non-modal,
// the app hides it ~600 ms after the last resize.

#include "control.hpp"
#include "../scene/overlay.hpp"

#include <string>

namespace mayag::dsl {

template <ControlContext C>
inline void resize_hud(const C& c, std::string_view id, int cols, int rows, std::string_view extra = {}) {
    const Theme& t = c.theme;
    c.overlay(Overlay{
        .content = (v(text_owned(std::to_string(cols) + " \u00D7 " + std::to_string(rows)) | font(t.font_size * 1.6f) | bold
                          | fg(var::text_primary) | text_align(TextAlign::center),
                      text_owned(std::string{extra}) | font(t.font_size * 0.85f) | fg(var::text_secondary) | text_align(TextAlign::center))
                    | gap(t.spaced(2)) | pad(t.spaced(10), t.spaced(18)) | align(Align::center)
                    | bg(t.overlay.fade(0.92f)) | border(t.stroke(1), t.border_strong) | radius(t.radius_medium) | elevation(t.lift(16))).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::centered,
        .dismiss = Dismiss::never,
        .layer = 25,
    });
}

}  // namespace mayag::dsl
