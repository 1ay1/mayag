#pragma once

/// Compositing a layer: turning placement and effects into draw calls.
///
/// The renderer has always been able to draw rounded boxes, analytic shadows
/// and backdrop blur -- they are ShapeKinds. What was missing is the bridge:
/// a compositor holds Layers, and nothing turned one into the instances that
/// draw it. So mayag could draw every effect a desktop uses and a compositor
/// could not ask for one.
///
/// This is the bridge, and it is deliberately small. It makes no decisions --
/// plan_planes() already decided what reaches the GPU, and Effects already
/// said what to do -- it only emits, in the order the effects must be drawn.
///
/// That order is the only subtle part, and it is the thing everyone gets
/// wrong at least once:
///
///   1. SHADOW, first, because it falls behind the window
///   2. BACKDROP BLUR, which must sample what is already there -- so after
///      the shadow but before the content
///   3. CONTENT, on top
///
/// Emitting the shadow after the content draws it over the window; emitting
/// the backdrop after the content blurs the window into itself.

#include "../render/draw_list.hpp"
#include "output.hpp"
#include "planes.hpp"   // Assignment

namespace mayag::output {

/// Emit the draw calls for one layer.
///
/// `texture` is the handle from `Device::acquire`. A layer whose texture is
/// invalid still emits its shadow and backdrop, because those do not need the
/// content -- a window whose buffer has not arrived yet should still cast its
/// shadow rather than pop in.
inline void composite_layer(DrawList& out, const Layer& l, TextureHandle texture) {
    const Effects& fx = l.effects;
    const Corners radii{fx.corner_radius};

    // 1. Shadow, behind everything.
    //
    // Analytic: one instance with a closed-form falloff, not a blur pass over
    // a rendered copy. That is what makes a shadow per window affordable --
    // the alternative is a render-to-texture and a two-pass gaussian each.
    //
    // The shadow takes the layer's corner radius, because a shadow with
    // square corners under a rounded window is visible at every corner and
    // reads as a rendering bug rather than a style choice.
    if (fx.shadow_radius > 0.0f) {
        Shadow sh;
        sh.offset = fx.shadow_offset;
        sh.blur   = fx.shadow_radius;
        sh.color  = fx.shadow_color.fade(fx.opacity);
        out.shadow(l.dst, sh, radii);
    }

    // 2. Backdrop blur, which samples the pixels already behind it.
    //
    // Must come after the shadow and before the content: it snapshots the
    // framebuffer, so anything drawn later is not included. Drawing it after
    // the content would blur the window into itself.
    if (fx.backdrop_blur > 0.0f) {
        out.backdrop(l.dst, fx.backdrop_blur, fx.backdrop_saturation,
                     fx.backdrop_brightness, radii);
    }

    // 3. The content.
    if (!texture.valid()) return;
    out.imported(l.dst, texture, Rect{{0, 0}, {1, 1}},
                 colors::white.fade(fx.opacity));
}

/// Emit an entire frame: every layer that did not win a hardware plane.
///
/// Takes the plan rather than recomputing it, because the planner already
/// decided and two answers to the same question is how a compositor ends up
/// drawing a layer twice -- once on a plane and once on the GPU.
inline void composite_frame(DrawList& out, std::span<const Layer> layers,
                            std::span<const Assignment> plan,
                            std::span<const TextureHandle> textures) {
    for (std::size_t i = 0; i < plan.size() && i < layers.size(); ++i) {
        if (plan[i].direct()) continue;      // the hardware is drawing it
        const TextureHandle t =
            i < textures.size() ? textures[i] : TextureHandle{};
        composite_layer(out, layers[i], t);
    }
}

}  // namespace mayag::output
