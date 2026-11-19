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
#include "hud.hpp"
#include "text_field.hpp"
#include "../scene/overlay.hpp"

#include <string>

namespace mayag::dsl {

template <ControlContext C>
inline void term_search(const C& c, std::string_view id, std::uint64_t anchor, const TextEditState& query,
                        bool case_sensitive, bool regex, int index, int count) {
    const Theme& t = c.theme;
    const float fs = t.font_size * 0.9f;
    const auto dim = mix(t.text_primary, t.background, 0.45f);
    const bool none = !query.text.empty() && count == 0;
    const auto btn = [&](std::size_t part, const char* glyph, bool on, const char* what) {
        const auto pid = part_id(id, part);
        const bool hov = c.hovered(pid);
        return (h(text_owned(glyph) | font(fs) | monospace | fg(on ? t.accent : hov ? t.text_primary : dim))
                | height(pct(100)) | pad(0, fs * 0.35f) | center | bg(hov ? mix(t.background, t.text_primary, 0.1f) : t.background.fade(0.0f))
                | pointer_cursor | label(what) | id_of(id, part)).build();
    };
    const std::string q = query.text;
    const std::string pos = q.empty() ? "" : count == 0 ? "0/0" : std::to_string(index + 1) + "/" + std::to_string(count);
    auto chip = hud_chip(c, {{"/" + q + "\u258F", none ? t.danger : t.text_primary},
                             {pos, none ? t.danger : dim}},
                         {.tag = "FIND", .tone = none ? t.danger : t.warning, .size = fs});
    c.overlay(Overlay{
        .content = (h(std::move(chip) | min_size(240.0f, 0.0f),
                      node(btn(2, "Aa", case_sensitive, "match case")),
                      node(btn(4, ".*", regex, "regex")),
                      node(btn(5, "\u2191", false, "previous")),
                      node(btn(6, "\u2193", false, "next")),
                      node(btn(9, "\u00D7", false, "close")))
                    | align(Align::center) | height(std::round(fs * 1.75f))
                    | bg(mix(t.background, t.text_primary, 0.06f))).build(),
        .id = part_id(id, part_menu),
        .anchor_id = anchor,
        .placement = Placement::below_end,
        .dismiss = Dismiss::never,
        .offset = -40.0f,
        .layer = 6,
    });
}

}  // namespace mayag::dsl
