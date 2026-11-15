#pragma once
// mayag::widget term_search — search the scrollback
//
//   term_search(c, "search", m.query, m.case_sensitive, m.regex, index, count)
//
// A compact bar that floats over the terminal's top-right corner (posted as
// an overlay anchored to the terminal node, so it never reflows the grid):
// query, Aa / .* toggles, "3/41", up/down, close.
//
// Interaction: parts 0 query, 2 case, 4 regex, 5 prev, 6 next, 9 close
// (the same numbering as find_bar's find_part).

#include "control.hpp"
#include "text_field.hpp"
#include "../scene/overlay.hpp"

#include <string>

namespace mayag::dsl {

template <ControlContext C>
inline void term_search(const C& c, std::string_view id, std::uint64_t anchor, const TextEditState& query,
                        bool case_sensitive, bool regex, int index, int count) {
    const Theme& t = c.theme;
    const auto btn = [&](std::size_t part, const char* glyph, bool on, const char* what) {
        const auto pid = part_id(id, part);
        const bool hov = c.hovered(pid);
        return (h(text_owned(glyph) | font(t.font_size * 0.9f) | semibold
                      | fg(on ? t.on_accent : hov ? t.text_primary : t.text_secondary))
                | size(24, 22) | center | radius(4) | bg(on ? t.accent : hov ? t.surface_raised : t.surface.fade(0.0f))
                | pointer_cursor | label(what) | id_of(id, part)).build();
    };
    const bool none = !query.text.empty() && count == 0;
    c.overlay(Overlay{
        .content = (h(node((text_field(t, query, true, part_id(id, 0), "Search scrollback", t.font_size)
                            | width(220) | border(1, none ? t.danger : t.accent)).build()),
                      node(btn(2, "Aa", case_sensitive, "match case")),
                      node(btn(4, ".*", regex, "regex")),
                      text_owned(query.text.empty() ? std::string{} : count == 0 ? std::string{"0/0"}
                                                     : std::to_string(index + 1) + "/" + std::to_string(count))
                          | font(t.font_size * 0.85f) | fg(none ? t.danger : t.text_secondary) | width(60)
                          | text_align(TextAlign::center),
                      node(btn(5, "\u2191", false, "previous")),
                      node(btn(6, "\u2193", false, "next")),
                      node(btn(9, "\u00D7", false, "close")))
                    | gap(4) | align(Align::center) | pad(6, 8)
                    | bg(t.overlay) | border(1, t.border_strong) | radius(t.radius_small) | elevation(12)).build(),
        .id = part_id(id, part_menu),
        .anchor_id = anchor,
        .placement = Placement::below_end,
        .dismiss = Dismiss::never,
        .offset = -44.0f,
        .layer = 6,
    });
}

}  // namespace mayag::dsl
