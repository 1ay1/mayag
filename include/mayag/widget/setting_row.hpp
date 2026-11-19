#pragma once
// mayag::widget setting_row — form layout: a labelled setting, and a section of them

#include "control.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

/// One setting: a title, an optional explanation, and the control, which is
/// right-aligned and keeps its own size. The text column takes the rest and
/// wraps its description rather than squeezing the control.
template <ControlContext C, Element Control>
[[nodiscard]] inline Node setting_row(const C& c, std::string_view title_text,
                                      std::string_view description_text, Control control) {
    const Theme& t = c.theme;
    // A form row: the title in the body size at medium weight, its hint one
    // step smaller and quieter; the control right-aligned on the row's
    // centre line. The row tints on hover so the eye can track which
    // control belongs to which label across a wide panel.
    return (h(v(text_owned(std::string{title_text}) | font(t.font_size * 0.95f) | weight(FontWeight::medium)
                    | fg(var::text_primary),
                text_owned(std::string{description_text}) | font(t.font_size * 0.82f)
                    | fg(var::text_disabled) | wrap_text | line_height(1.35f))
                | gap(t.spaced(2)) | grow() | min_size(0, 0),
              v(std::move(control)) | align(Align::end))
            | gap(t.spaced(24)) | align(Align::center) | pad(t.spaced(10), t.spaced(14)) | width(pct(100))
            | bg(var::surface_raised.fade(0.0f)) | hovered(bg(var::surface_raised.fade(0.45f))) | transition()).build();
}

/// A titled card of setting rows with hairlines between them.
template <ControlContext C>
[[nodiscard]] inline Node section(const C& c, std::string_view title_text, std::vector<Node> rows) {
    const Theme& t = c.theme;
    std::vector<Node> body;
    body.reserve(rows.size() * 2);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) body.push_back((box() | height(1) | width(pct(100)) | bg(t.border.fade(0.7f))).build());
        body.push_back(std::move(rows[i]));
    }
    std::string caps;
    for (char ch : title_text) caps.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    return (v(text_owned(std::move(caps)) | font(t.font_size * 0.72f) | semibold
                  | tracking(1.1f) | fg(var::text_disabled) | pad(0, t.spaced(2)),
              v(v_list(std::move(body)))
                  | width(pct(100))
                  | bg(var::background) | border(t.stroke(1), t.border) | radius(t.radius_medium) | clip)
            | gap(t.spaced(8)) | width(pct(100))).build();
}

}  // namespace mayag::dsl
