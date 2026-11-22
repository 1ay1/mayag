#pragma once
// mayag::widget toast — transient notifications: compact monospace chips, bottom-right
//
//   toasts(c, "toasts", m.toasts)    // the app expires them on its own clock
//
// Interaction: Clicked on part_id(id, i) -> dismiss toast i (or run its action).

#include "control.hpp"
#include "hud.hpp"
#include "../scene/overlay.hpp"

#include <algorithm>
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
    // one compact monospace line per toast: [tag] title  body  ×
    std::vector<Node> cards;
    const float fs = t.type(Tier::body);
    const auto dim = mix(t.text_primary, t.background, 0.45f);
    float widest = 0.0f;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Toast& k = items[i];
        using K = Toast::Kind;
        const Color<Srgb> tone = k.kind == K::error ? t.danger : k.kind == K::warning ? t.warning
                               : k.kind == K::success ? t.success : t.info;
        const std::string tag = k.kind == K::error ? "ERR" : k.kind == K::warning ? "WARN" : k.kind == K::success ? "OK" : "INFO";
        std::vector<HudSeg> segs{{k.title, t.text_primary, true}};
        if (!k.body.empty()) segs.push_back({k.body.size() > 48 ? k.body.substr(0, 47) + "\u2026" : k.body, dim});
        segs.push_back({k.action.empty() ? std::string{"\u00D7"} : "[" + k.action + "]", k.action.empty() ? dim : t.accent});
        std::size_t chars = tag.size() + 4;
        for (const auto& sg : segs) chars += sg.text.size() + 1;
        widest = std::max(widest, static_cast<float>(chars) * fs * 0.6f);
        cards.push_back((hud_chip(c, segs, {.tag = tag, .tone = tone, .size = fs, .hot = c.hovered(part_id(id, i))})
                         | pointer_cursor | id_of(id, i)).build());
    }
    const float row = std::round(fs * 1.75f) + 4.0f;
    c.overlay(Overlay{
        .content = (v(std::move(cards)) | gap(4.0f) | align(Align::end)).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::cursor,
        .dismiss = Dismiss::never,
        .layer = 20,
        .at = Vec2{std::max(window_size.x - widest - 24.0f, 8.0f), window_size.y - 44.0f - row * static_cast<float>(items.size())},
    });
}

}  // namespace mayag::dsl
