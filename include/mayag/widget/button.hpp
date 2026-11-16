#pragma once
// mayag::widget button — primary / secondary / ghost / danger
//
// Behaviour here, look in the theme's Sheet (widget/sheet.hpp): every
// colour, size, radius, border, shadow, state variant and spring of every
// button comes from `Sheet::button` / `primary` / `danger` / `ghost`, so a
// theme restyles them all -- and anything piped after the call wins:
//
//     button<"Deploy">(t)                            the theme's primary
//     button<"Cancel">(t, ButtonVariant::ghost)
//     button<"Delete">(t) | bg(var::danger)          one-off override

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"
#include "sheet.hpp"

namespace mayag::dsl {

enum class ButtonVariant { primary, secondary, ghost, danger };

namespace detail {
[[nodiscard]] inline const Look& button_look(const Theme& t, ButtonVariant v) {
    const Sheet& s = sheet_of(t);
    switch (v) {
        case ButtonVariant::primary:   return s.primary;
        case ButtonVariant::danger:    return s.danger;
        case ButtonVariant::ghost:     return s.ghost;
        case ButtonVariant::secondary: break;
    }
    return s.button;
}
}  // namespace detail

/// A button. Its capabilities stay live (fill, stroke, shadow), so a caller
/// may pipe `| border_color(...)` or `| bg(...)` on top.
template <fixed_string Label>
[[nodiscard]] inline auto button(const Theme& t, ButtonVariant variant = ButtonVariant::primary) {
    return h(text<Label> | with_look(sheet_of(t).label))
         | center | surface_caps
         | with_look(detail::button_look(t, variant), t)
         | pointer_cursor
         | role(a11y::Role::button);
}

}  // namespace mayag::dsl
