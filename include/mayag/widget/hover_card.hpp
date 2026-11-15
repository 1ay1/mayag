#pragma once
// mayag::widget hover_card — documentation / diagnostic popover at a point
//
//   hover_card(c, "hover", pos, sections)
//
// What a language server's hover shows: a code signature block, prose, and
// any diagnostics at that spot, each with its source. Posted as a
// non-modal overlay (Dismiss::never) so moving the mouse simply replaces it.

#include "control.hpp"
#include "../scene/overlay.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct HoverSection {
    enum class Kind : std::uint8_t { code, prose, error, warning, info };
    Kind        kind = Kind::prose;
    std::string text;
    std::string source;   ///< "clangd", "rust-analyzer" ...
};

template <ControlContext C>
inline void hover_card(const C& c, std::string_view id, Vec2 at, const std::vector<HoverSection>& sections,
                       float px = 13.0f) {
    if (sections.empty()) return;
    const Theme& t = c.theme;
    std::vector<Node> parts;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        if (i > 0) parts.push_back((box() | height(1) | width(pct(100)) | bg(t.border)).build());
        using K = HoverSection::Kind;
        const Color<Srgb> tone = s.kind == K::error ? t.danger : s.kind == K::warning ? t.warning
                               : s.kind == K::info ? t.info : t.text_primary;
        if (s.kind == K::code) {
            parts.push_back((v(text_owned(s.text) | font(px) | fg(t.accent) | wrap_text | no_ligatures | line_height(1.4f))
                             | pad(8, 12) | width(pct(100)) | bg(t.surface)).build());
        } else {
            parts.push_back((h(node(s.kind == K::prose ? Node{} : (box() | size(3, px * 1.3f) | bg(tone) | radius(1)).build()),
                               v(text_owned(s.text) | font(px * 0.95f) | fg(s.kind == K::prose ? t.text_secondary : tone)
                                     | wrap_text | line_height(1.45f),
                                 text_owned(s.source) | font(px * 0.8f) | fg(t.text_disabled))
                                   | gap(3) | grow() | min_size(0, 0))
                             | gap(8) | pad(8, 12) | width(pct(100))).build());
        }
    }
    c.overlay(Overlay{
        .content = (v(v_list(std::move(parts))) | width(440) | max_size(440, 320)
                    | bg(t.overlay) | border(1, t.border_strong) | radius(t.radius_small) | clip | elevation(14)).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::cursor,
        .dismiss = Dismiss::never,
        .layer = 9,
        .at = at,
    });
}

}  // namespace mayag::dsl
