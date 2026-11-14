#pragma once
// mayag::widget card — surfaces: card, panel, glass

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

/// A card: the workhorse container. Note it returns a *container*, so the
/// caller can still add children conceptually via composition and still use
/// `gap`, `center`, etc.
template <Element... Kids>
[[nodiscard]] constexpr auto card(const Theme& t, Kids... kids) {
    return v(kids...)
         | pad(t.space(2))
         | gap(t.space(1))
         | bg(t.surface)
         | border(1, t.border)
         | radius(t.radius_medium)
         | elevation(6.0f);
}

/// A panel — flatter than a card, for sidebars and toolbars.
template <Element... Kids>
[[nodiscard]] constexpr auto panel(const Theme& t, Kids... kids) {
    return v(kids...)
         | pad(t.space(1.5f))
         | gap(t.space(1))
         | bg(t.surface)
         | border(1, t.border)
         | radius(t.radius_small);
}

/// Frosted-glass overlay — modals, command palettes, popovers.
template <Element... Kids>
[[nodiscard]] constexpr auto glass(const Theme& t, Kids... kids) {
    return v(kids...)
         | pad(t.space(2))
         | gap(t.space(1))
         | bg(t.overlay.fade(0.72f))
         | border(1, t.border_strong.fade(0.6f))
         | radius(t.radius_large)
         | backdrop_blur(24.0f)
         | elevation(20.0f);
}

}  // namespace mayag::dsl
