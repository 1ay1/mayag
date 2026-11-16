#pragma once
// mayag::widget badge — a pill-shaped label

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

/// A pill-shaped label. `tone` drives the whole appearance; the label is
/// the tone itself where that reads, and is darkened/lightened toward
/// legibility where it does not (a yellow accent on a cream page).
template <fixed_string Label>
[[nodiscard]] constexpr auto badge(const Theme& t, Color<Srgb> tone) {
    // The wash the label actually sits on: the tone at 16% over the surface.
    const Color<Srgb> wash = mix(t.surface, Color<Srgb>{tone.c0, tone.c1, tone.c2, 1.0f}, 0.16f * tone.a);
    const Color<Srgb> label = contrast_ratio(tone, wash) >= 3.0f ? tone : mayag::detail::contrasting_on(wash, 4.5f, 0.6f);
    // Single-word content: never wrap, and reserve room for at least the
    // first glyph so a crowded row cannot crush it into a sliver.
    return h(text<Label> | font(t.font_size * 0.8f) | semibold | fg(label)
                         | ellipsis | min_size(t.font_size * 0.7f, 0.0f))
         | center
         | pad(t.spaced(3.0f), t.spaced(9.0f))
         | bg(tone.fade(0.16f))
         | border(t.stroke(1), tone.fade(0.35f))
         | radius(t.capsule(t.font_size * 0.8f * 1.3f + t.spaced(6.0f)));
}

}  // namespace mayag::dsl
