#pragma once
// mayag::widget button — primary / secondary / ghost / danger

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

enum class ButtonVariant { primary, secondary, ghost, danger };

/// A button. All four variants share one body; the variant only chooses
/// colours, which is exactly the property that keeps a design system coherent.
template <fixed_string Label>
[[nodiscard]] constexpr auto button(const Theme& t, ButtonVariant variant = ButtonVariant::primary) {
    const bool primary_ = (variant == ButtonVariant::primary);
    const bool danger_  = (variant == ButtonVariant::danger);
    const bool ghost_   = (variant == ButtonVariant::ghost);

    const Color<Srgb> face =
        primary_ ? t.accent : danger_ ? t.danger : ghost_ ? t.surface.fade(0.0f) : t.surface_raised;
    const Color<Srgb> label_color =
        primary_ ? t.on_accent : danger_ ? readable_on(t.danger) : t.text_primary;
    const Color<Srgb> edge = ghost_ ? t.border : (primary_ || danger_) ? face : t.border_strong;

    return h(text<Label> | font(t.font_size) | semibold | fg(label_color))
         | center
         | pad(t.space(1.25f), t.space(2))
         | bg(face)
         | border(1, edge)
         | radius(t.radius_small)
         | when(primary_ || danger_, elevation(3.0f));
}

}  // namespace mayag::dsl
