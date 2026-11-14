#pragma once
// mayag::widget kbd — a key cap, for shortcut hints

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

/// A key cap — for shortcut hints.
///
/// `min_size` is what keeps a single-character cap from being squeezed to
/// nothing by a crowded toolbar: a 6px-wide "T" is unreadable, and a widget
/// should not depend on its caller leaving enough room.
template <fixed_string Key>
[[nodiscard]] constexpr auto kbd(const Theme& t) {
    // `ellipsis` rather than the default wrap: a key cap holds one or two
    // characters and must never break across lines. Wrapping mode is also
    // what makes a narrow cap look like a text-engine failure rather than a
    // sizing one.
    return h(text<Key> | font(t.font_size * 0.82f) | fg(t.text_secondary)
                       | ellipsis | min_size(t.font_size * 0.7f, 0.0f))
         | center
         | min_size(t.font_size * 1.5f, t.font_size * 1.3f)
         | pad(2.0f, 7.0f)
         | bg(t.background)
         | border(1, t.border_strong)
         | radius(4.0f)
         | inner_shadow(2.0f, colors::black.fade(0.25f), {0.0f, -1.0f});
}

}  // namespace mayag::dsl
