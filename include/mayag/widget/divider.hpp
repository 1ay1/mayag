#pragma once
// mayag::widget divider — a horizontal rule

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

/// A horizontal rule that spans its parent.
[[nodiscard]] constexpr auto divider(const Theme& t, float thickness = 1.0f) {
    return box() | height(t.stroke(thickness)) | width(pct(100)) | bg(var::border);
}

}  // namespace mayag::dsl
