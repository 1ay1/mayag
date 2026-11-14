#pragma once
// mayag::widget badge — a pill-shaped label

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

/// A pill-shaped label. `tone` drives the whole appearance.
template <fixed_string Label>
[[nodiscard]] constexpr auto badge(const Theme& t, Color<Srgb> tone) {
    // Single-word content: never wrap, and reserve room for at least the
    // first glyph so a crowded row cannot crush it into a sliver.
    return h(text<Label> | font(t.font_size * 0.8f) | semibold | fg(tone)
                         | ellipsis | min_size(t.font_size * 0.7f, 0.0f))
         | center
         | pad(3.0f, 9.0f)
         | bg(tone.fade(0.16f))
         | border(1, tone.fade(0.35f))
         | pill;
}

}  // namespace mayag::dsl
