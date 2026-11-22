#pragma once
// mayag::widget terminal/remote — a terminal talking to another machine
//
//   remote_guard(c, id, box, props)      a production host: a red hairline round the
//                                        terminal and a PROD user@host tag in its corner
//   connection_lost(c, id, box, props)   the link dropped: a card at the bottom, the
//                                        scrollback still readable above it
//
// Interaction: Clicked on node_id(id + ".reconnect") / node_id(id + ".close").

#include "overlays.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace mayag::dsl {

struct RemoteGuardProps {
    std::string who;   ///< "deploy@prod-db"
};

/// Always in view, never in the way.
template <ControlContext C>
[[nodiscard]] inline std::vector<Node> remote_guard(const C& c, std::string_view id, const TermBox& b, const RemoteGuardProps& p) {
    const Theme& t = c.theme;
    const auto red = t.danger, paper = t.background, ink = t.text_primary;
    const float fs = t.type_at(b.font_size, Tier::small);
    std::vector<Node> out;
    out.push_back((box() | size(b.size.x - 2.0f, b.size.y - 2.0f) | border(1.0f, mix(paper, red, 0.55f)) | absolute(1.0f, 1.0f)).build());
    const float cw = detail::term_cell_width(c, fs, b.family);
    const float tw = (static_cast<float>(term_detail::cells(p.who)) + 7.0f) * cw + fs * 2.4f;
    out.push_back((hud_chip(c, {{p.who, mix(red, ink, 0.35f)}}, {.tag = "PROD", .tone = red, .size = fs})
                   | id_of(id) | absolute(b.size.x - b.right_inset - 6.0f - tw, 6.0f)).build());
    return out;
}

struct ConnectionLostProps {
    std::string alias;      ///< "prod-db"
    std::string command;    ///< "ssh prod-db"
    double      gone = 0.0; ///< seconds since it dropped
    bool        motion = true;
};

template <ControlContext C>
[[nodiscard]] inline Node connection_lost(const C& c, std::string_view id, const TermBox& b, const ConnectionLostProps& p) {
    using term_detail::mono;
    const Theme& t = c.theme;
    const auto red = t.danger, paper = t.background, ink = t.text_primary, dim = mix(ink, paper, 0.45f);
    const float fs = t.type_at(b.font_size, Tier::body);
    const std::string base{id};
    const auto btn = [&](const char* part, std::string label_, std::string key, bool primary) {
        const std::string nid = base + "." + part;
        const bool hov = c.hovered(node_id(nid));
        return (h(mono(std::move(label_), t.type_at(b.font_size, Tier::small), primary ? paper : ink) | semibold,
                  mono(std::move(key), t.type_at(b.font_size, Tier::fine), primary ? mix(paper, ink, 0.25f) : dim))
                | gap(6.0f) | align(Align::center) | pad(0, 9.0f) | height(std::round(fs * 1.9f))
                | bg(primary ? (hov ? mix(ink, paper, 0.1f) : ink) : (hov ? mix(paper, ink, 0.1f) : paper.fade(0.0f)))
                | border(1.0f, primary ? ink : mix(paper, ink, 0.3f)) | pointer_cursor | id_of(nid)).build();
    };
    const std::string when = p.gone < 60.0 ? "just now" : std::to_string(static_cast<int>(p.gone / 60.0)) + "m ago";
    const float w = std::min(620.0f, b.size.x - 32.0f);
    const float e = std::clamp(static_cast<float>(p.gone) * 5.0f, 0.0f, 1.0f);
    const float ease = p.motion ? 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e) : 1.0f;
    return (h(v(h(mono("\u2716", t.type_at(b.font_size, Tier::fine), red), mono("lost " + p.alias, fs, ink) | semibold, mono(when, t.type_at(b.font_size, Tier::fine), dim))
                    | gap(8.0f) | align(Align::center),
                mono(p.command + " exited 255 \u00B7 the connection dropped or was refused", t.type_at(b.font_size, Tier::fine), dim) | ellipsis)
                  | gap(4.0f) | grow() | min_size(0, 0),
              node(btn("close", "close", "esc", false)),
              node(btn("reconnect", "reconnect", "\u23CE", true)))
            | gap(10.0f) | align(Align::center) | pad(10.0f, 14.0f) | width(w)
            | bg(mix(paper, ink, 0.04f)) | border(1.0f, mix(paper, red, 0.55f))
            | shadow(28.0f, colors::black.fade(0.45f), {0.0f, 10.0f}) | role(a11y::Role::dialog) | label("connection lost")
            | opacity(ease) | translate(0.0f, (1.0f - ease) * 12.0f)
            | absolute((b.size.x - w) * 0.5f, b.size.y - std::round(fs * 5.2f) - 18.0f)).build();
}

}  // namespace mayag::dsl
