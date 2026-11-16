#pragma once
// mayag::widget avatar — a circle with an accent ring

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

/// An avatar circle with an accent ring.
[[nodiscard]] constexpr auto avatar(const Theme& t, Color<Srgb> tint, float d = 36.0f) {
    return box() | size(d, d)
                 | bg(tint)
                 | radius(d * 0.5f)
                 | border(t.stroke(2), t.surface, StrokeAlign::outside);
}

}  // namespace mayag::dsl
