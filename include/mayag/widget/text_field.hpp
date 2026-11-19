#pragma once
// mayag::widget text_field — a single-line text field with caret and selection

#include "../app/context.hpp"
#include "../dsl/dsl.hpp"
#include "../core/text_edit.hpp"
#include "../style/theme.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    // Caret and selection are VISUAL positions on the shaped line: the
    // frame's measurer (the font stack's in a window) answers them with
    // bidi and contextual shaping, so an Arabic caret moves right-to-left,
    // a mixed selection is several rectangles, and a ligature's caret
    // splits it. An explicit `measure` (prefix widths) is still honoured
    // for callers that pass one, for left-to-right text.
    const layout::TextMeasurer& mm = ctx().measurer();
    TextStyle ts{};
    ts.size = font_size;
    // While an input method composes, the field shows committed text with
    // the preedit spliced in at the caret (TextEditState::display_text),
    // underlined, and the caret sits inside the preedit where the IME says.
    const std::string shown = state.display_text();
    const std::string_view text = shown;
    const auto x_at = [&](std::size_t byte) -> float {
        if (byte == 0 && text.empty()) return 0.0f;
        if (measure) return byte == 0 ? 0.0f : measure(text.substr(0, num::min(byte, text.size())));
        return mm.caret_x(text, ts, num::min(byte, text.size()));
    };
    const float caret_x = x_at(state.display_caret());
    std::vector<std::pair<float, float>> spans;
    if (state.has_selection() && !state.composing()) {
        if (measure) spans.push_back({x_at(state.selection_start()), x_at(state.selection_end())});
        else spans = mm.selection_spans(text, ts, state.selection_start(), state.selection_end());
    }
    std::vector<Node> layers;
    layers.push_back((text_owned(showing_placeholder ? std::string{placeholder} : shown)
                        | font(font_size)
                        | fg(showing_placeholder ? t.text_disabled : t.text_primary)
                        | ellipsis
                        | width(pct(100))).build());
    for (const auto& [x0, x1] : spans)
        layers.push_back((box() | size(num::max(x1 - x0, 0.0f), font_size * 1.25f)
                                | bg(t.accent.fade(0.30f))
                                | radius(t.rounded(2))
                                | absolute(x0, 0)).build());
    layers.push_back((box() | size(1.5f, font_size * 1.2f)
                            | bg(t.accent.fade(focused ? 1.0f : 0.0f))
                            | absolute(caret_x, 0)).build());
    if (state.composing()) {
        // The preedit's underline (and the IME's highlighted clause, thicker).
        const std::size_t p0 = num::min(state.caret, state.text.size());
        const float u0 = x_at(p0), u1 = x_at(p0 + state.preedit.size());
        layers.push_back((box() | size(num::max(u1 - u0, 1.0f), 1.0f)
                                | bg(t.text_primary) | absolute(num::min(u0, u1), font_size * 1.18f)).build());
        if (state.preedit_sel_end > state.preedit_sel_start) {
            const float s0 = x_at(p0 + state.preedit_sel_start), s1 = x_at(p0 + state.preedit_sel_end);
            layers.push_back((box() | size(num::max(s1 - s0, 1.0f), 2.0f)
                                    | bg(t.accent) | absolute(num::min(s0, s1), font_size * 1.18f)).build());
        }
    }

    // Where the caret is, for the input method's candidate window: the
    // field's padding plus the caret's position on the line.
    const Rect caret_box{Vec2{t.spaced(11) + caret_x, t.spaced(9)}, Vec2{1.5f, font_size * 1.2f}};

    // FIRST child is in flow and therefore sizes the field; the selection
    // highlight(s) and caret float over it.
    return z(std::move(layers))
         | pad(t.spaced(9), t.spaced(11))
         | bg(var::surface)
         | border(t.stroke(1), focused ? t.accent : t.border)
         | radius(t.radius_small)
         | clip
         | cursor(CursorShape::text)
         | role(a11y::Role::text_field)
         | caret_at(caret_box)
         | id_of_raw(nid);
}

}  // namespace mayag::dsl
