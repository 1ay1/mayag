#pragma once
// mayag::widget resize_hud — the "SIZE 120×40" chip shown while resizing
//
//   if (m.resizing) resize_hud(c, "hud", cols, rows);
//
// Every terminal shows the grid size while the window is dragged, because
// the pixel size means nothing to the programs inside. Centred, non-modal,
// the app hides it ~600 ms after the last resize.

#include "control.hpp"
#include "hud.hpp"
#include "../scene/overlay.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

/// `px`: the pixel size, shown dimmed after the grid size ("" = none).
template <ControlContext C>
inline void resize_hud(const C& c, std::string_view id, int cols, int rows, std::string_view extra = {}) {
    const Theme& t = c.theme;
    const auto dim = mix(t.text_primary, t.background, 0.5f);
    std::vector<HudSeg> segs{{std::to_string(cols) + "\u00D7" + std::to_string(rows), t.text_primary, true}};
    if (!extra.empty()) segs.push_back({std::string{extra}, dim});
    c.overlay(Overlay{
        .content = (hud_chip(c, segs, {.tone = t.accent, .size = t.type(Tier::small)})).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::centered,
        .dismiss = Dismiss::never,
        .layer = 25,
        .passthrough = true,   // never eats the click it is floating over
    });
}

}  // namespace mayag::dsl
