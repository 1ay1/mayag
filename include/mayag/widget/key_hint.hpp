#pragma once
// mayag::widget key_hint — a keycap and what it does, for status bars
//
//   key_hint(t, "j/k", "move")   ->  [j/k] move
//
// The runtime-string twin of kbd<>: status lines change their hints with the
// mode, so the key cannot be a template argument.

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

#include <string>
#include <string_view>

namespace mayag::dsl {

[[nodiscard]] inline auto key_hint(const Theme& t, std::string_view keys, std::string_view action,
                                   float size = 0.0f) {
    const float s = size > 0.0f ? size : t.font_size * 0.85f;
    return h(h(text_owned(std::string{keys}) | font(s) | semibold | fg(var::text_primary))
                 | pad(t.spaced(1), t.spaced(5)) | bg(var::surface_raised) | border(t.stroke(1), t.border_strong) | radius(t.rounded(3)),
             text_owned(std::string{action}) | font(s) | fg(var::text_secondary))
         | gap(t.spaced(5)) | align(Align::center);
}

}  // namespace mayag::dsl
