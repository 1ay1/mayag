#pragma once
// mayag::widget scroll_view — a scrollable region with an auto-hiding bar

#include "../dsl/dsl.hpp"
#include "../core/scroll_state.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

/// A scrollable region with an auto-hiding scrollbar.
///
/// The bar appears only when there is something to scroll, and its thumb is
/// sized by the visible fraction — a long document gets a short thumb, which
/// is the only honest signal of how much is off-screen.
template <Element... Kids>
[[nodiscard]] inline auto scroll_view(const Theme& t, ScrollState& state, Kids... kids) {
    const Rect track{0, 0, 6, state.viewport.y};
    const Rect thumb = state.thumb_v(track);

    return z(node((v(kids...) | scroll(state)).build()),
             // The bar is absolutely positioned so it floats over the content
             // rather than stealing width from it.
             node((box()
                   | size(6, num::max(thumb.height(), 0.0f))
                   | bg(t.text_disabled.fade(state.scrollable_y() ? 0.55f : 0.0f))
                   | radius(3)
                   | absolute(state.viewport.x - 10.0f, thumb.top())).build()))
         | width(pct(100)) | height(pct(100));
}

}  // namespace mayag::dsl
