#pragma once
// mayag::widget card — surfaces: card, panel, glass
//
// Containers whose look is the theme's (Sheet::card / panel / glass): pad,
// gap, fill, border, radius, elevation and blur all follow the theme, and
// anything piped after the call wins. Children: anything `v(...)` takes.

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"
#include "sheet.hpp"

namespace mayag::dsl {

/// A card: the workhorse container.
template <Child... Kids>
[[nodiscard]] inline auto card(const Theme& t, Kids... kids) {
    return v(std::move(kids)...) | surface_caps | with_look(sheet_of(t).card, t);
}

/// A panel: flatter than a card, for sidebars and toolbars.
template <Child... Kids>
[[nodiscard]] inline auto panel(const Theme& t, Kids... kids) {
    return v(std::move(kids)...) | surface_caps | with_look(sheet_of(t).panel, t);
}

/// Frosted-glass overlay: modals, command palettes, popovers.
template <Child... Kids>
[[nodiscard]] inline auto glass(const Theme& t, Kids... kids) {
    return v(std::move(kids)...) | surface_caps | with_look(sheet_of(t).glass, t);
}

}  // namespace mayag::dsl
