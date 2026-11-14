#pragma once
// mayag::widget setting_row — form layout: a labelled setting, and a section of them

#include "control.hpp"

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
    return (h(v(text_owned(std::string{title_text}) | font(t.font_size) | fg(t.text_primary),
                text_owned(std::string{description_text}) | font(t.font_size * 0.86f)
                    | fg(t.text_secondary) | wrap_text | line_height(1.35f))
                | gap(3) | grow() | min_size(0, 0),
              v(control) | align(Align::end))
            | gap(24) | align(Align::center) | pad(14, 18) | width(pct(100))).build();
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
    return (v(text_owned(std::string{title_text}) | font(t.font_size * 0.78f) | semibold
                  | tracking(0.8f) | fg(t.text_secondary) | pad(0, 4),
              v(v_list(std::move(body)))
                  | width(pct(100))
                  | bg(t.surface) | border(1, t.border) | radius(t.radius_medium))
            | gap(8) | width(pct(100))).build();
}

}  // namespace mayag::dsl
