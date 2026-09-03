#pragma once
// mayag::virtual_list — build only what is visible
//
// A list of 100,000 rows must cost the same as a list of 20, because only
// ~20 are on screen. The naive approach builds every row into the tree and
// then relies on clipping to hide the rest: layout walks 100k nodes, paint
// walks 100k nodes, and the frame budget is gone before anything is drawn.
//
// Virtualisation inverts that. Given a uniform row height and a scroll
// offset, the visible range is arithmetic — no measuring required — so the
// view builds ~25 rows and pads with two spacers that stand in for everything
// above and below. Scroll position, scrollbar geometry and keyboard
// navigation all keep working, because the CONTENT height is still correct;
// only the node count changes.
//
//     virtual_list(m.scroll, m.rows.size(), row_height, [&](int i) {
//         return render_row(m.rows[i]);
//     })
//
// The callback is invoked only for visible indices.

#include "dsl.hpp"
#include "../core/scroll_state.hpp"

#include <functional>
#include <vector>

namespace mayag::dsl {

/// Which rows a viewport currently shows.
struct VisibleRange {
    int   first = 0;      ///< inclusive
    int   last  = 0;      ///< exclusive
    float pad_before = 0.0f;
    float pad_after  = 0.0f;

    [[nodiscard]] constexpr int count() const noexcept { return last - first; }
};

/// Compute the visible row range for a uniform-height list.
///
/// `overscan` rows are built beyond each edge so that a fast scroll does not
/// reveal blank space before the next frame catches up. Two is enough at
/// 60 Hz; more just wastes work.
[[nodiscard]] inline VisibleRange visible_rows(const ScrollState& scroll,
                                               int total, float row_height,
                                               float gap = 0.0f, int overscan = 2) {
    VisibleRange r;
    if (total <= 0 || row_height <= 0.0f) return r;

    const float stride = row_height + gap;
    const float top    = scroll.offset.y;
    const float bottom = top + num::max(scroll.viewport.y, 1.0f);

    r.first = num::max(static_cast<int>(top / stride) - overscan, 0);
    r.last  = num::min(static_cast<int>(bottom / stride) + 1 + overscan, total);
    if (r.last < r.first) r.last = r.first;

    r.pad_before = static_cast<float>(r.first) * stride;
    r.pad_after  = static_cast<float>(total - r.last) * stride;
    return r;
}

/// A virtualised vertical list.
///
/// `build(i)` is called only for visible indices. The two spacers preserve
/// the true content height, so the scrollbar reports the real proportion and
/// `scroll_to_bottom()` lands where the user expects.
[[nodiscard]] inline auto virtual_list(const ScrollState& scroll, int total,
                                       float row_height,
                                       const std::function<Node(int)>& build,
                                       float gap = 0.0f, int overscan = 2) {
    const VisibleRange r = visible_rows(scroll, total, row_height, gap, overscan);

    std::vector<Node> children;
    children.reserve(static_cast<std::size_t>(r.count()) + 2);

    // A spacer standing in for every row above the viewport. One node instead
    // of thousands, with exactly the height they would have occupied.
    if (r.pad_before > 0.0f) {
        children.push_back((box() | height(r.pad_before) | width(pct(100))).build());
    }

    for (int i = r.first; i < r.last; ++i) {
        children.push_back(build(i));
    }

    if (r.pad_after > 0.0f) {
        children.push_back((box() | height(r.pad_after) | width(pct(100))).build());
    }

    return list(Axis::vertical, std::move(children), gap);
}

/// How many nodes a virtualised list will build. Useful in tests and in the
/// debug overlay, where the whole point is that this number stays small.
[[nodiscard]] inline int virtual_node_count(const ScrollState& scroll, int total,
                                            float row_height, float gap = 0.0f,
                                            int overscan = 2) {
    const VisibleRange r = visible_rows(scroll, total, row_height, gap, overscan);
    return r.count() + (r.pad_before > 0.0f ? 1 : 0) + (r.pad_after > 0.0f ? 1 : 0);
}

}  // namespace mayag::dsl
