#pragma once
// mayag::widget progress — a filled progress track

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

/// A filled progress track that fills whatever width it is given.
///
/// The fill is an ABSOLUTE child sized as a percentage of the track, so the
/// widget works at any width without the caller having to know the pixel
/// number. The previous signature took an explicit width, which meant callers
/// passed sentinels like 999 and then overrode it with `width(pct(100))` —
/// two sources of truth, and the loser overflowed its parent.
[[nodiscard]] constexpr auto progress(const Theme& t, float fraction, float h = 6.0f) {
    return z(box() | width(pct(num::saturate(fraction) * 100.0f)) | height(h)
                   | bg(t.accent) | radius(h * 0.5f))
         | width(pct(100)) | height(h)
         | bg(t.border) | radius(h * 0.5f) | clip;
}

}  // namespace mayag::dsl
