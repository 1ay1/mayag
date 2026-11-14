#pragma once
// mayag::widget text_field — a single-line text field with caret and selection

#include "../dsl/dsl.hpp"
#include "../core/text_edit.hpp"
#include "../style/theme.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace mayag::dsl {

/// Map a click x-offset (px from the field's left edge) to the caret byte
/// index it should land on — the inverse of text_field's caret placement, so
/// mouse positioning is as font-accurate as the caret it produces. Walks
/// UTF-8 boundaries and picks the boundary whose measured advance is closest
/// to x; `measure(prefix)` returns the advance of the text before a boundary.
/// Pass the SAME measure callback you give text_field.
[[nodiscard]] inline std::size_t caret_from_x(
        std::string_view text, float x,
        const std::function<float(std::string_view)>& measure) {
    if (text.empty() || x <= 0.0f) return 0;
    std::size_t best = 0;
    float best_d = x;   // distance from x to boundary 0 (advance 0)
    std::size_t i = 0;
    while (i < text.size()) {
        // Advance to the next UTF-8 boundary (skip continuation bytes).
        std::size_t j = i + 1;
        while (j < text.size() && (static_cast<unsigned char>(text[j]) & 0xC0) == 0x80) ++j;
        const float adv = measure ? measure(std::string_view{text.data(), j})
                                  : static_cast<float>(j);
        const float d = num::abs(adv - x);
        if (d < best_d) { best_d = d; best = j; }
        i = j;
    }
    return best;
}

/// A single-line text field with caret and selection.
///
/// Sizes to its own TEXT. There is no hand-computed height here, and that is
/// the point: a widget cannot know the font it will be rendered with, so any
/// multiplier it invents is a guess that goes wrong the moment a caller uses
/// a different size. The text sits in flow and defines the height; the caret
/// and selection float over it.
///
/// This shipped the other way once — `height(font_size * 1.4)` with every
/// child absolute — and produced a 19.6 px field around 22.4 px of text,
/// visibly clipping its own placeholder.
///
/// `focused` is passed in rather than read from a Ctx, because widgets sit
/// BELOW the app layer in the include graph. Callers write
/// `text_field(t, m.name, c.focused<"name">(), node_id("name"))`.
///
/// Pass `measure` — e.g. `[&](std::string_view s){ return c.measurer().advance(
/// s, st); }` — to place the caret and selection at the TRUE advance width of
/// the text before them. Without it the field falls back to a monospace
/// approximation, which drifts under a proportional font as you type; the
/// measured path is pixel-correct for any font, weight, or variable axis.
[[nodiscard]] inline auto text_field(const Theme& t, const TextEditState& state,
                                     bool focused, std::uint64_t nid,
                                     std::string_view placeholder = {},
                                     float text_size = 0.0f,
                                     const std::function<float(std::string_view)>& measure = {}) {
    const float font_size = text_size > 0.0f ? text_size : t.font_size;
    const bool showing_placeholder = state.text.empty() && !placeholder.empty();

    // Caret and selection track the text. When a measurer is supplied the
    // positions are the exact advance of the substring before each boundary
    // (correct for any font); otherwise a monospace estimate keeps it close.
    const float approx_w = font_size * 0.55f;
    const auto x_at = [&](std::size_t byte) -> float {
        if (byte == 0) return 0.0f;
        const std::string_view upto{state.text.data(), num::min(byte, state.text.size())};
        if (measure) return measure(upto);
        return static_cast<float>(byte) * approx_w;
    };
    const float caret_x = x_at(state.caret);
    const float sel_x0  = x_at(state.selection_start());
    const float sel_x1  = x_at(state.selection_end());
    const float sel_w   = num::max(sel_x1 - sel_x0, 0.0f);

    return z(// FIRST child is in flow and therefore sizes the field.
             text_owned(showing_placeholder ? std::string{placeholder} : state.text)
               | font(font_size)
               | fg(showing_placeholder ? t.text_disabled : t.text_primary)
               | ellipsis
               | width(pct(100)),
             // Selection highlight and caret float over it.
             node((box() | size(num::max(sel_w, 0.0f), font_size * 1.25f)
                         | bg(t.accent.fade(state.has_selection() ? 0.30f : 0.0f))
                         | radius(2)
                         | absolute(sel_x0, 0)).build()),
             node((box() | size(1.5f, font_size * 1.2f)
                         | bg(t.accent.fade(focused ? 1.0f : 0.0f))
                         | absolute(caret_x, 0)).build()))
         | pad(9, 11)
         | bg(t.surface)
         | border(1, focused ? t.accent : t.border)
         | radius(t.radius_small)
         | clip
         | cursor(CursorShape::text)
         | id_of_raw(nid);
}

}  // namespace mayag::dsl
