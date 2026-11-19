#pragma once
// mayag::widget nav_item — a sidebar destination
//
// Interaction: Clicked / Activated on part_id(base, index) -> go there.
// `trailing` is right-aligned (a count, a shortcut); `trailing_hot` draws it
// in the accent colour (e.g. "2/6" when two settings there are modified).

#include "control.hpp"
#include "sheet.hpp"

#include <string>
#include <string_view>

namespace mayag::dsl {

template <ControlContext C>
[[nodiscard]] inline Node nav_item(const C& c, std::string_view base, std::size_t index,
                                   std::string_view icon, std::string_view name, bool active,
                                   std::string_view trailing = {}, bool trailing_hot = false) {
    const Theme& t = c.theme;

    // Look from the theme's Sheet: `item` (rows of a list), with its `mark`
    // bar lit on the active destination.
    return (h(box() | size(2, 14) | with_look(sheet_of(t).mark, t),
              text_owned(std::string{icon}) | font(t.font_size) | fg(active ? t.accent : t.text_secondary)
                  | text_align(TextAlign::center) | width(18),
              text_owned(std::string{name}) | font(t.font_size)
                  | (active ? semibold : weight(FontWeight::regular)) | grow(),
              text_owned(std::string{trailing}) | font(t.font_size * 0.9f)
                  | fg(trailing_hot ? t.accent : t.text_disabled))
            | gap(t.spaced(8)) | align(Align::center) | pad(t.spaced(7), t.spaced(10), t.spaced(7), t.spaced(4))
            | width(pct(100)) | with_look(sheet_of(t).item, t) | group()
            | pointer_cursor
            | role(a11y::Role::tab) | selected(active) | label(name)
            | id_of(base, index)).build();
}

}  // namespace mayag::dsl
