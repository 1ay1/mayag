#pragma once
// mayag::widget scrollback_bar — a terminal scrollbar that shows history
//
//   scrollback_bar(c, "sb", {.total = 12000, .first = 11800, .visible = 40, .marks = ...})
//
// A terminal's scrollbar is a map of the session: where the prompts were,
// which commands failed, where the search hits are. Rows are abstract line
// indices (0 = oldest kept line), so it works with any scrollback store.
// The thumb only shows when scrolled back or hovered (at the bottom of a
// live terminal it is noise).
//
// Interaction:
//   Pressed / Dragged on node_id(id) carry local.y; the line under it is
//   scrollback_line_at(info, local.y, height)

#include "control.hpp"

#include <vector>

namespace mayag::dsl {

struct ScrollbackMark {
    enum class Kind : std::uint8_t { prompt, error, match, current_match, bookmark };
    int  line = 0;
    Kind kind = Kind::prompt;
};

struct ScrollbackInfo {
    int total = 0;      ///< history + screen rows
    int first = 0;      ///< first visible line
    int visible = 24;   ///< rows on screen
    std::vector<ScrollbackMark> marks;
};

[[nodiscard]] inline int scrollback_line_at(const ScrollbackInfo& s, float y, float height) {
    if (s.total <= 0 || height <= 0) return 0;
    return std::clamp(static_cast<int>(y / height * static_cast<float>(s.total)) - s.visible / 2, 0,
                      std::max(0, s.total - s.visible));
}

template <ControlContext C>
[[nodiscard]] inline Node scrollback_bar(const C& c, std::string_view id, const ScrollbackInfo& s, float height_px) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool hot = c.hovered(nid) || c.pressed(nid);
    const bool scrolled = s.first + s.visible < s.total;
    const float total = static_cast<float>(std::max(s.total, 1));
    std::vector<Node> parts;
    for (const auto& m : s.marks) {
        using K = ScrollbackMark::Kind;
        const float y = static_cast<float>(m.line) / total * height_px;
        const Color<Srgb> col = m.kind == K::error ? t.danger : m.kind == K::current_match ? t.warning
                              : m.kind == K::match ? t.warning.fade(0.6f) : m.kind == K::bookmark ? t.accent : t.text_disabled;
        const float w = m.kind == K::prompt ? 4.0f : 8.0f;
        parts.push_back((box() | size(w, m.kind == K::prompt ? 1.0f : 2.0f) | bg(col) | absolute(10.0f - w - 1.0f, y)).build());
    }
    if (scrolled || hot) {
        const float th = std::max(static_cast<float>(s.visible) / total * height_px, 20.0f);
        const float ty = static_cast<float>(s.first) / total * height_px;
        parts.push_back((box() | size(hot ? 7.0f : 5.0f, th) | bg(colors::white.fade(hot ? 0.25f : 0.14f)) | radius(3)
                         | absolute(hot ? 2.0f : 3.0f, std::min(ty, height_px - th))).build());
    }
    return (z(h_list(std::move(parts))) | width(11) | height(pct(100)) | bg(hot ? colors::white.fade(0.03f) : colors::white.fade(0.0f))
            | role(a11y::Role::slider) | label("scrollback") | id_of(id)).build();
}

}  // namespace mayag::dsl
