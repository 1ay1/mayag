#pragma once
// mayag::widget dot — a coloured dot

#include "../dsl/dsl.hpp"

namespace mayag::dsl {

/// A coloured dot — status indicators, legend swatches, unread badges.
[[nodiscard]] constexpr auto dot(Color<Srgb> c, float d = 8.0f) {
    return box() | size(d, d) | bg(c) | radius(d * 0.5f);
}

}  // namespace mayag::dsl
