#pragma once
// mayag::widget toast — transient notifications, stacked bottom-right
//
//   toasts(c, "toasts", m.toasts)    // the app expires them on its own clock
//
// Interaction: Clicked on part_id(id, i) -> dismiss toast i (or run its action).

#include "control.hpp"
#include "../scene/overlay.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct Toast {
    std::string title;
    std::string body;
    enum class Kind : std::uint8_t { info, success, warning, error } kind = Kind::info;
    std::string action;   ///< a button label, or empty
};

template <ControlContext C>
inline void toasts(const C& c, std::string_view id, const std::vector<Toast>& items, Vec2 window_size) {
    if (items.empty()) return;
    const Theme& t = c.theme;
    std::vector<Node> cards;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Toast& k = items[i];
        using K = Toast::Kind;
        const Color<Srgb> tone = k.kind == K::error ? t.danger : k.kind == K::warning ? t.warning
                               : k.kind == K::success ? t.success : t.info;
        cards.push_back((h(box() | size(3, 34) | bg(tone) | radius(t.rounded(2)),
                           v(text_owned(k.title) | font(t.font_size) | semibold | fg(var::text_primary) | ellipsis,
                             text_owned(k.body) | font(t.font_size * 0.88f) | fg(var::text_secondary) | wrap_text)
                               | gap(t.spaced(3)) | grow() | min_size(0, 0),
                           text_owned(k.action.empty() ? std::string{"\u00D7"} : k.action) | font(t.font_size * 0.9f)
                               | fg(k.action.empty() ? t.text_disabled : t.accent))
                         | gap(t.spaced(10)) | align(Align::center) | pad(t.spaced(10), t.spaced(12)) | width(340)
                         | bg(var::overlay) | border(t.stroke(1), t.border_strong) | radius(t.radius_small) | elevation(t.lift(12))
                         | pointer_cursor | id_of(id, i)).build());
    }
    const float h_est = static_cast<float>(items.size()) * 66.0f;
    c.overlay(Overlay{
        .content = v_list(std::move(cards), 8.0f).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::cursor,
        .dismiss = Dismiss::never,
        .layer = 20,
        .at = Vec2{window_size.x - 356.0f, window_size.y - 40.0f - h_est},
    });
}

}  // namespace mayag::dsl
