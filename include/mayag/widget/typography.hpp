#pragma once
// mayag::widget typography — the type scale: title, heading, body, caption

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

template <fixed_string S>
[[nodiscard]] constexpr auto title(const Theme& t) {
    return text<S> | font(t.font_size * 1.6f) | bold | fg(var::text_primary) | tracking(-0.3f);
}

template <fixed_string S>
[[nodiscard]] constexpr auto heading(const Theme& t) {
    return text<S> | font(t.font_size * 1.15f) | semibold | fg(var::text_primary);
}

template <fixed_string S>
[[nodiscard]] constexpr auto body(const Theme& t) {
    return text<S> | font(t.font_size) | fg(var::text_primary);
}

template <fixed_string S>
[[nodiscard]] constexpr auto caption(const Theme& t) {
    return text<S> | font(t.font_size * 0.85f) | fg(var::text_secondary);
}

}  // namespace mayag::dsl
