#pragma once

/// Damage: working out how little needs redrawing.
///
/// A compositor that redraws the whole screen because a clock ticked is
/// burning a GPU to change four digits. Damage tracking is what makes an idle
/// desktop cost nothing, and it is the difference between a laptop that
/// sleeps and one that runs warm doing nothing.
///
/// The arithmetic is small but every part of it has a plausible wrong answer,
/// which is why it belongs here rather than in each compositor:
///
///   * damage arrives in BUFFER coordinates and must reach SCREEN coordinates
///     through scale, transform and position
///   * a layer that MOVED damages both where it was and where it now is
///   * a layer that appeared or vanished damages its whole rect
///   * occlusion means damage under an opaque layer costs nothing
///
/// The last is the one that pays. A video playing behind a fullscreen window
/// generates damage every frame and none of it is visible; noticing that is
/// the difference between decoding-and-compositing and decoding alone.
///
/// Everything here is CONSERVATIVE in one direction, deliberately: too much
/// damage wastes power, too little shows a stale frame. Every approximation
/// below rounds towards redrawing.

#include "output.hpp"
#include "planes.hpp"   // covers()

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace mayag::output {

/// Map a rect from buffer space to screen space.
///
/// `src`/`dst` are the layer's crop and placement, so this also handles
/// viewporter's crop-and-scale for free -- the scale factor is just the ratio
/// between them.
[[nodiscard]] inline Rect buffer_to_screen(const Rect& r, const Layer& l) {
    const Rect src = l.src.size.x > 0 && l.src.size.y > 0
                   ? l.src
                   : Rect{{0, 0}, {static_cast<float>(l.buffer.width),
                                   static_cast<float>(l.buffer.height)}};
    if (src.size.x <= 0.0f || src.size.y <= 0.0f) return l.dst;

    const float sx = l.dst.size.x / src.size.x;
    const float sy = l.dst.size.y / src.size.y;

    // Relative to the crop, then scaled, then offset to the destination.
    const float x = (r.left() - src.left()) * sx + l.dst.left();
    const float y = (r.top()  - src.top())  * sy + l.dst.top();
    const float w = r.size.x * sx;
    const float h = r.size.y * sy;

    // Rounded OUTWARDS. A rect that lands on a fractional pixel must cover
    // every pixel it touches -- rounding to nearest leaves a one-pixel seam
    // that only appears at fractional scale factors, which is exactly where
    // nobody tests.
    const float l0 = std::floor(x), t0 = std::floor(y);
    const float r0 = std::ceil(x + w), b0 = std::ceil(y + h);
    return Rect{{l0, t0}, {r0 - l0, b0 - t0}};
}

/// Everything that must be redrawn this frame, in screen coordinates.
///
/// `previous` is where each layer was last frame, in the same order. A layer
/// that moved damages both rectangles; a shorter `previous` means the extra
/// layers are new and damage their whole rect.
[[nodiscard]] inline Region
frame_damage(std::span<const Layer> layers, std::span<const Rect> previous) {
    Region out;

    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& l = layers[i];

        // A layer that MOVED or RESIZED damages both places: the pixels it
        // has left behind, and the ones it now covers. Damaging only the new
        // position leaves a trail of the old one, which is the classic
        // "window drag smears" bug.
        const bool is_new = i >= previous.size();
        const bool moved = !is_new && !(previous[i] == l.dst);
        if (is_new || moved) {
            if (moved) out.add(previous[i]);
            out.add(l.dst);
            continue;
        }

        // Otherwise only what the client says changed. Empty means
        // everything, which is the safe reading.
        if (l.damage.empty()) {
            out.add(l.dst);
        } else {
            for (const Rect& r : l.damage.rects) out.add(buffer_to_screen(r, l));
        }
    }

    // Layers that VANISHED damage where they were.
    for (std::size_t i = layers.size(); i < previous.size(); ++i) {
        out.add(previous[i]);
    }
    return out;
}

/// Drop damage hidden behind opaque layers.
///
/// The optimisation that pays for the whole file. A video playing behind a
/// fullscreen window damages itself every frame and none of it can be seen;
/// noticing that is the difference between decoding-and-compositing and
/// decoding alone.
///
/// Conservative: a damage rect is dropped only when a SINGLE opaque layer
/// covers it entirely. Two adjacent opaque layers that jointly cover it are
/// not detected, because the union of rectangles is not a rectangle and
/// approximating it wrongly would drop visible damage -- the one error this
/// must never make.
[[nodiscard]] inline Region occlude(const Region& damage,
                                   std::span<const Layer> layers) {
    Region out;
    for (const Rect& d : damage.rects) {
        const bool hidden = std::any_of(
            layers.begin(), layers.end(), [&](const Layer& l) {
                if (!l.opaque && l.opaque_region.empty()) return false;
                const Rect bounds = l.opaque_region.bounds();
                const Rect cover =
                    l.opaque ? l.dst
                             : Rect{{l.dst.origin.x + bounds.left(),
                                     l.dst.origin.y + bounds.top()},
                                    bounds.size};
                return covers(cover, d);
            });
        if (!hidden) out.add(d);
    }
    return out;
}

}  // namespace mayag::output
