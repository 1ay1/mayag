#pragma once
// mayag::widget typography — text by role, on the theme's type scale
// (style/type.hpp). Each is a tier, never a size: the theme decides pixels.

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

template <fixed_string S>
[[nodiscard]] constexpr auto title(const Theme&) {
    return text<S> | type(Tier::title) | bold | fg(var::text_primary) | tracking(-0.3f);
}

template <fixed_string S>
[[nodiscard]] constexpr auto heading(const Theme&) {
    return text<S> | type(Tier::large) | semibold | fg(var::text_primary);
}

template <fixed_string S>
[[nodiscard]] constexpr auto body(const Theme&) {
    return text<S> | type(Tier::body) | fg(var::text_primary);
}

template <fixed_string S>
[[nodiscard]] constexpr auto caption(const Theme&) {
    return text<S> | type(Tier::small) | fg(var::text_secondary);
}

template <fixed_string S>
[[nodiscard]] constexpr auto fine_print(const Theme&) {
    return text<S> | type(Tier::fine) | fg(var::text_secondary);
}

}  // namespace mayag::dsl
