#pragma once

/// Plane assignment: deciding which layers the display can scan out directly.
///
/// This is the payoff of the whole output layer, and the place mayag has a
/// structural advantage over a compositor library bolted onto a renderer.
///
/// A display controller can read a handful of buffers straight from memory
/// and blend them in fixed-function hardware — "planes". A layer that lands
/// on a plane costs the GPU NOTHING: no shader, no bandwidth, no power. A
/// fullscreen video on a plane is the difference between a laptop lasting
/// eight hours and five. Most compositors skip this entirely, because
/// deciding what qualifies means knowing the geometry and opacity of every
/// element, and by the time a renderer sees a draw list that information has
/// been flattened away.
///
/// mayag has not flattened it. The scene graph already knows every element's
/// rect, z-order and opacity, because that is what layout produced. So the
/// question "can this be a plane?" is answerable exactly here, from data that
/// already exists, rather than reconstructed by pattern-matching a command
/// stream.
///
/// The rules are hardware's, not ours:
///
///   * a plane draws ONE buffer, unrotated and unscaled (or scaled by a
///     scaler, if the hardware has one to spare)
///   * planes composite bottom-to-top with fixed blending
///   * there are only a few, so they must go to the layers that save the most
///
/// Everything that does not qualify falls back to GPU compositing, which is
/// always correct. That asymmetry matters: a wrong "yes" corrupts the screen,
/// a wrong "no" costs a little power. So every rule below is conservative.

#include "output.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace mayag::output {

/// What the hardware can do, so the planner does not have to guess.
struct PlaneCaps {
    /// Total planes usable for content, excluding the cursor plane.
    int  count = 0;
    /// Can a plane scale its buffer? Without this, only 1:1 layers qualify.
    bool scaling = false;
    /// Can a plane blend with what is beneath it? Without this, only the
    /// bottom layer and opaque layers qualify.
    bool blending = false;
};

/// The planner's answer for one layer.
struct Assignment {
    /// Index into the layers the planner was given.
    std::size_t layer = 0;
    /// Which plane, or -1 for "composite this on the GPU".
    int         plane = -1;

    [[nodiscard]] bool direct() const noexcept { return plane >= 0; }
};

/// Decide which layers can be scanned out directly.
///
/// Returns one Assignment per input layer, in the same order. Layers that
/// cannot use a plane get plane == -1 and must be composited.
///
/// The policy is "biggest opaque layers first". A plane saves the GPU exactly
/// the area it covers, so the largest layer is worth the most, and opacity is
/// required whenever the hardware cannot blend. Sorting by area rather than
/// taking them in z-order is what makes a fullscreen video win a plane even
/// when a small always-on-top overlay exists.
[[nodiscard]] inline std::vector<Assignment>
plan_planes(std::span<const Layer> layers, const PlaneCaps& caps) {
    std::vector<Assignment> out;
    out.reserve(layers.size());
    for (std::size_t i = 0; i < layers.size(); ++i) out.push_back(Assignment{i, -1});
    if (caps.count <= 0 || layers.empty()) return out;

    // Rank candidates by the area they would save.
    struct Candidate { std::size_t index; float area; int z; };
    std::vector<Candidate> ranked;
    ranked.reserve(layers.size());

    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& l = layers[i];

        // A plane reads a real buffer. Nothing else can be scanned out.
        if (!l.buffer.valid()) continue;

        // Scaling: a plane without a scaler shows the buffer at its own size,
        // so any layer drawn at a different size must be composited. Compared
        // exactly rather than with a tolerance -- a half-pixel difference is
        // still a scale, and asking the hardware for one it cannot do is how
        // a commit gets refused.
        if (!caps.scaling) {
            const bool same_w = static_cast<int>(l.dst.size.x) == l.buffer.width;
            const bool same_h = static_cast<int>(l.dst.size.y) == l.buffer.height;
            if (!same_w || !same_h) continue;
        }

        // Blending: without hardware blending only opaque layers qualify,
        // because a transparent one needs to mix with what is underneath and
        // the plane cannot. The bottom-most layer is exempt: there is nothing
        // beneath it to blend with.
        if (!caps.blending && !l.opaque) {
            const bool is_bottom = std::none_of(
                layers.begin(), layers.end(),
                [&](const Layer& other) { return other.z < l.z; });
            if (!is_bottom) continue;
        }

        ranked.push_back(Candidate{i, l.dst.size.x * l.dst.size.y, l.z});
    }

    // Biggest first; ties broken by z so the result is deterministic. A
    // planner that returns different answers for the same scene makes
    // flicker that is impossible to reproduce.
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Candidate& a, const Candidate& b) {
                         if (a.area != b.area) return a.area > b.area;
                         return a.z < b.z;
                     });

    // Assign planes bottom-to-top among the winners, because that is the
    // order the hardware composites them in.
    const std::size_t take = std::min(ranked.size(),
                                      static_cast<std::size_t>(caps.count));
    std::vector<Candidate> chosen(ranked.begin(),
                                  ranked.begin() + static_cast<std::ptrdiff_t>(take));
    std::stable_sort(chosen.begin(), chosen.end(),
                     [](const Candidate& a, const Candidate& b) { return a.z < b.z; });

    int plane = 0;
    for (const Candidate& c : chosen) out[c.index].plane = plane++;
    return out;
}

/// How many of an assignment's layers avoided the GPU.
[[nodiscard]] inline int direct_count(std::span<const Assignment> a) noexcept {
    return static_cast<int>(std::count_if(
        a.begin(), a.end(), [](const Assignment& x) { return x.direct(); }));
}

}  // namespace mayag::output
