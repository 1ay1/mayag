#pragma once
// mayag::widget hud — compact monospace chips that float over a terminal
//
//   hud_chip(c, {{"120", ink}, {"×", dim}, {"40", ink}})      →  [120×40]
//   hud_line(c, "search", {...})                              →  a one-line bar
//
// The terminal's own vocabulary: square corners, a hairline border, cells
// of monospace text in the theme's colours, no shadow, no glow. A chip is a
// row of SEGMENTS (text + colour), optionally with a coloured tag on the
// left ("[ok]", "SEARCH") -- one widget for every transient readout an app
// floats over its content: resize, scroll position, notifications, modes.

#include "control.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct HudSeg {
    std::string text;
    Color<Srgb> color{};          ///< alpha 0: the theme's text colour
    bool        bold = false;
};

struct HudProps {
    std::string tag;              ///< a mode/status cell on the left, inverse video ("" = none)
    Color<Srgb> tone{};           ///< the tag's colour, and the border's tint (alpha 0: neutral)
    float       size = 0.0f;      ///< font px (0: theme size × 0.9)
    bool        hot = false;      ///< hovered: the border lights up
};

template <ControlContext C>
[[nodiscard]] inline auto hud_chip(const C& c, const std::vector<HudSeg>& segs, const HudProps& p = {}) {
    const Theme& t = c.theme;
    const float fs = t.type_at(p.size > 0.0f ? p.size : t.font_size, Tier::body);
    const bool toned = p.tone.a > 0.0f;
    const auto paper = t.background, ink = t.text_primary;
    std::vector<Node> parts;
    if (!p.tag.empty())
        parts.push_back((h(text_owned(p.tag) | font(fs) | monospace | semibold | fg(paper))
                         | height(pct(100)) | pad(0, fs * 0.5f) | center | radius(0.0f) | bg(toned ? p.tone : t.accent)).build());
    std::vector<Node> words;
    for (const auto& s : segs)
        words.push_back((text_owned(s.text) | font(fs) | monospace | (s.bold ? semibold : weight(FontWeight::regular))
                         | fg(s.color.a > 0.0f ? s.color : ink)).build());
    parts.push_back((h(std::move(words)) | gap(fs * 0.6f) | align(Align::center) | pad(0, fs * 0.6f)
                     | height(pct(100))).build());
    const auto edge = toned ? mix(paper, p.tone, p.hot ? 0.9f : 0.5f) : mix(paper, ink, p.hot ? 0.45f : 0.22f);
    return h(std::move(parts)) | align(Align::center) | height(std::round(fs * 1.75f))
         | bg(mix(paper, ink, 0.06f)) | border(1.0f, edge) | radius(0.0f);   // square, always
}

}  // namespace mayag::dsl
