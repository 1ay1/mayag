#pragma once
// mayag::widget layout — the two layouts every screen types: split, centered

#include "../dsl/dsl.hpp"

namespace mayag::dsl {

/// A horizontal row whose children are pushed to opposite ends — the single
/// most common bar layout (title on the left, actions on the right).
template <Element... Kids>
[[nodiscard]] constexpr auto split(Kids... kids) {
    return h(kids...) | justify(Justify::space_between) | align(Align::center);
}

/// A centred stack that fills whatever space it is given.
template <Element... Kids>
[[nodiscard]] constexpr auto centered(Kids... kids) {
    return v(kids...) | center | width(pct(100)) | height(pct(100));
}

}  // namespace mayag::dsl
