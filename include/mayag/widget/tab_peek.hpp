#pragma once
// mayag::widget tab_peek — a live preview of a tab, on hover
//
//   if (m.hovered_tab >= 0 && hover_time > 0.5)
//       tab_peek(c, "peek", part_id("tabs", i), tabs[i], terminal_view(...last rows...), "pid 4182 · up 2h");
//
// Browsers show a hover card; a terminal can do better and show the SCREEN:
// the last lines of the tab's terminal, so you can check on a build or a
// log tail without switching. The preview is any Node (usually a small
// terminal_view); it is made inert so the card never steals clicks.
// Posted under the hovered tab, non-modal: moving away simply removes it.

#include "control.hpp"
#include "tab_strip.hpp"
#include "../scene/overlay.hpp"

#include <string>

namespace mayag::dsl {

template <ControlContext C>
inline void tab_peek(const C& c, std::string_view id, std::uint64_t anchor, const TermTabInfo& tb, Node preview,
                     std::string_view footer, double time = 0.0, float width_px = 420.0f) {
    const Theme& t = c.theme;
    const Color<Srgb> hue = tab_hue(tb);
    detail::make_inert(preview);
    std::string status = tb.running ? "running \u00B7 " + short_duration(tb.elapsed)
                       : tb.last_exit == 0 ? std::string{"\u2713 last command succeeded"}
                       : tb.last_exit > 0 ? "\u2716 last command exited " + std::to_string(tb.last_exit)
                                          : std::string{"idle"};
    const Color<Srgb> status_col = tb.running ? hue : tb.last_exit == 0 ? t.success : tb.last_exit > 0 ? t.danger : t.text_disabled;
    c.overlay(Overlay{
        .content = (v(h(node(tab_icon(t, tb, time, true, 28.0f)),
                        v(text_owned(tb.title) | font(t.type(Tier::body)) | semibold | fg(var::text_primary) | ellipsis,
                          text_owned(!tb.host.empty() ? tb.host + "  \u00B7  " + tb.cwd : tb.cwd) | font(t.type(Tier::small))
                              | fg(tb.host_danger ? t.danger : t.text_secondary) | ellipsis)
                            | gap(t.spaced(2)) | grow() | min_size(0, 0),
                        text_owned(status) | font(t.type(Tier::small)) | fg(status_col))
                        | gap(t.spaced(10)) | align(Align::center) | pad(t.spaced(10), t.spaced(12)) | width(pct(100)),
                      v(node(std::move(preview))) | height(132) | width(pct(100)) | clip
                          | border(t.stroke(1), t.border) | radius(t.rounded(8)) | margin(0, 10),
                      h(text_owned(std::string{footer}) | font(t.type(Tier::fine)) | fg(var::text_disabled) | ellipsis | grow() | min_size(0, 0),
                        node(tb.activity.empty() ? Node{} : activity_sparkline(tb.activity, hue, 12.0f, 28)))
                        | gap(t.spaced(8)) | align(Align::center) | pad(t.spaced(8), t.spaced(12)) | width(pct(100)))
                    | width(width_px) | bg(var::overlay) | border(t.stroke(1), mix(t.border_strong, hue, 0.25f))
                    | radius(t.rounded(12)) | clip | elevation(t.lift(18))
                    | role(a11y::Role::dialog) | label("tab preview")).build(),
        .id = part_id(id, part_menu),
        .anchor_id = anchor,
        .placement = Placement::below_start,
        .dismiss = Dismiss::never,
        .offset = 8.0f,
        .layer = 14,
    });
}

}  // namespace mayag::dsl
