#pragma once
// mayag::widget marks — app marks drawn from shapes, crisp at any size
//
//   hand_mark(c, 48, {.cursor = true})   a hand, palm out, a cursor in the palm
//
// Coloured from the theme (accent -> info gradient), so a mark sits in
// whatever scheme the app wears.

#include "control.hpp"

#include <vector>

namespace mayag::dsl {

struct HandMarkProps {
    bool  cursor = true;     ///< the cursor in the palm (blink it by toggling)
    float roundness = 1.0f;  ///< the tile's corner, as a multiple of the default
};

template <ControlContext C>
[[nodiscard]] inline Node hand_mark(const C& c, float px, const HandMarkProps& p = {}) {
    const Theme& t = c.theme;
    const float k = px / 52.0f;
    const auto c1 = t.accent, c2 = mix(t.accent, t.info, 0.55f);
    const auto skin = mix(colors::white, c1, 0.04f);   // opaque: overlaps leave no seams
    std::vector<Node> parts;
    parts.push_back((box() | size(px, px) | radius(12.0f * k * p.roundness)
                     | linear_gradient(mix(c1, colors::white, 0.08f), c2, {0.0f, 0.0f}, {1.0f, 1.0f})).build());
    parts.push_back((box() | size(px, px * 0.5f) | radius(12.0f * k * p.roundness, 12.0f * k * p.roundness, 0.0f, 0.0f)
                     | linear_gradient(colors::white.fade(0.16f), colors::white.fade(0.0f))).build());   // a sheen
    const auto cap = [&](float x, float y, float w, float h_) {
        parts.push_back((box() | size(w * k, h_ * k) | radius(w * k * 0.5f) | bg(skin) | absolute(x * k, y * k)).build());
    };
    cap(15.5f, 10.0f, 7.0f, 22.0f);   // index
    cap(23.5f, 7.5f, 7.0f, 24.0f);    // middle
    cap(31.5f, 9.5f, 7.0f, 22.0f);    // ring
    cap(39.0f, 14.5f, 6.0f, 17.0f);   // little
    parts.push_back((box() | size(30.0f * k, 20.0f * k) | radius(6.0f * k, 6.0f * k, 11.0f * k, 11.0f * k) | bg(skin)
                     | absolute(15.0f * k, 23.0f * k)).build());   // palm
    for (int i = 0; i <= 8; ++i) {   // the thumb: a stroke angled out from the palm
        const float f = static_cast<float>(i) / 8.0f;
        const float x = 18.0f + (8.5f - 18.0f) * f, y = 34.0f + (22.5f - 34.0f) * f;
        parts.push_back((box() | size(7.5f * k, 7.5f * k) | radius(3.75f * k) | bg(skin)
                         | absolute((x - 3.75f) * k, (y - 3.75f) * k)).build());
    }
    if (p.cursor) parts.push_back((box() | size(5.0f * k, 9.0f * k) | radius(1.0f * k) | bg(c1) | absolute(27.5f * k, 29.0f * k)).build());
    return (z(std::move(parts)) | size(px, px)).build();
}

}  // namespace mayag::dsl
