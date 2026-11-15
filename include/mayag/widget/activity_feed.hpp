#pragma once
// mayag::widget activity_feed — what just happened, across every workspace
//
//   activity_feed(c, "feed", events, window_size, time)
//
// A terminal knows when commands finish (shell integration), when a bell
// rings, when a long job reports progress. When that happens in a tab or a
// workspace you are NOT looking at, it should reach you: a quiet stack of
// event cards in the corner — "cargo test failed (exit 101) · jaal / tab 2 ·
// 4m12s" — each a click away from the tab it came from, fading on their own.
// Successes are small; failures are louder and stay until seen.
//
// Interaction: part_id(id, i) jump to the event's tab; part_id(id, 4096 + i) dismiss.

#include "control.hpp"
#include "tab_strip.hpp"
#include "../scene/overlay.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct ActivityEvent {
    enum class Kind : std::uint8_t { finished, failed, bell, started_long, disconnected };
    Kind          kind = Kind::finished;
    std::string   command;       ///< "cargo test"
    std::string   where;         ///< "jaal · tab 2"
    std::uint32_t hue = 0;       ///< the workspace colour
    double        duration = 0;  ///< for finished / failed
    int           exit_code = 0;
    double        age = 0;       ///< seconds since it happened (drives the fade)
};

template <ControlContext C>
inline void activity_feed(const C& c, std::string_view id, const std::vector<ActivityEvent>& events, Vec2 window,
                          double /*time*/ = 0.0, float top = 56.0f) {
    if (events.empty()) return;
    const Theme& t = c.theme;
    std::vector<Node> cards;
    for (std::size_t i = 0; i < events.size() && i < 5; ++i) {
        const auto& e = events[i];
        using K = ActivityEvent::Kind;
        const bool loud = e.kind == K::failed || e.kind == K::disconnected;
        const Color<Srgb> tone = e.kind == K::failed || e.kind == K::disconnected ? t.danger : e.kind == K::bell ? t.warning
                               : e.kind == K::finished ? t.success : t.info;
        const float fade = loud ? 1.0f : std::clamp(1.0f - static_cast<float>((e.age - 4.0) / 2.0), 0.0f, 1.0f);
        if (fade <= 0.0f) continue;
        const char* glyph = e.kind == K::failed ? "\u2716" : e.kind == K::finished ? "\u2713" : e.kind == K::bell ? "\U0001F514"
                          : e.kind == K::disconnected ? "\u21AF" : "\u25B6";
        std::string head = e.kind == K::failed ? e.command + " failed" : e.kind == K::finished ? e.command + " finished"
                         : e.kind == K::bell ? "bell in " + e.where : e.kind == K::disconnected ? "connection lost" : e.command + " is still running";
        std::string sub = e.where;
        if (e.kind == K::failed) sub += "  \u00B7  exit " + std::to_string(e.exit_code);
        if (e.duration > 0) sub += "  \u00B7  " + short_duration(e.duration);
        const auto pid = part_id(id, i), did = part_id(id, 4096 + i);
        const bool hov = c.hovered(pid) || c.hovered(did);
        const Color<Srgb> ws = e.hue ? rgb_hex(e.hue) : t.accent;
        cards.push_back((h(h(text_owned(glyph) | font(t.font_size * 0.9f) | bold | fg(tone)) | size(26, 26) | center | radius(13)
                               | bg(t.tint(tone, 0.18f)),
                           v(text_owned(head) | font(t.font_size * 0.92f) | semibold | fg(t.text_primary) | ellipsis,
                             h(box() | size(6, 6) | radius(3) | bg(ws),
                               text_owned(sub) | font(t.font_size * 0.76f) | fg(t.text_secondary) | ellipsis)
                                 | gap(6) | align(Align::center))
                               | gap(2) | grow() | min_size(0, 0),
                           node(hov ? (h(text<"\u00D7"> | font(t.font_size * 0.9f) | fg(c.hovered(did) ? t.text_primary : t.text_disabled))
                                       | pointer_cursor | id_of(id, 4096 + i)).build() : Node{}))
                         | gap(10) | align(Align::center) | pad(8, 10) | width(320) | radius(12)
                         | bg(mix(t.overlay, tone, loud ? 0.06f : 0.0f)) | border(1, loud ? t.tint(tone, 0.45f) : t.border_strong)
                         | elevation(14) | opacity(fade) | pointer_cursor
                         | role(a11y::Role::button) | label(head) | id_of(id, i)).build());
    }
    if (cards.empty()) return;
    c.overlay(Overlay{
        .content = v_list(std::move(cards), 8.0f).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::cursor,
        .dismiss = Dismiss::never,
        .layer = 22,
        .at = Vec2{window.x - 336.0f, top},
    });
}

}  // namespace mayag::dsl
