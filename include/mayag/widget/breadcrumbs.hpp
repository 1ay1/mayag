#pragma once
// mayag::widget breadcrumbs — path › to › file › symbol
//
// The strip under the tabs: where you are in the project and in the code.
// Interaction: Clicked on part_id(id, i) -> the app jumps to crumb i (open
// the folder in the tree, or the symbol picker scoped to that symbol).

#include "control.hpp"
#include "file_icon.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct Crumb {
    std::string text;
    bool        file = false;     ///< draw a file icon before it
    bool        symbol = false;   ///< a code symbol (accent)
};

template <ControlContext C>
[[nodiscard]] inline Node breadcrumbs(const C& c, std::string_view id, const std::vector<Crumb>& crumbs,
                                      Node trailing = Node{}) {
    const Theme& t = c.theme;
    std::vector<Node> parts;
    for (std::size_t i = 0; i < crumbs.size(); ++i) {
        if (i > 0) parts.push_back((text<"\u203A"> | font(t.type(Tier::body)) | fg(var::text_disabled)).build());
        const auto pid = part_id(id, i);
        const bool hov = c.hovered(pid);
        const Crumb& k = crumbs[i];
        parts.push_back((h(node(k.file ? file_icon(t, k.text, 11.0f).build() : Node{}),
                           text_owned(k.text) | font(t.type(Tier::body))
                               | fg(hov ? t.text_primary : k.symbol ? t.accent : i + 1 == crumbs.size() ? t.text_primary : t.text_secondary))
                         | gap(t.spaced(4)) | align(Align::center) | pad(t.spaced(1), t.spaced(4)) | radius(t.rounded(3))
                         | bg(hov ? t.surface_raised : t.surface.fade(0.0f)) | pointer_cursor
                         | id_of(id, i)).build());
    }
    return (h(h(h_list(std::move(parts), 2.0f)) | grow() | min_size(0, 0) | clip, node(std::move(trailing)))
            | align(Align::center) | pad(0, t.spaced(12)) | height(28) | width(pct(100))
            | bg(var::background) | border(t.stroke(1), t.border.fade(0.6f))).build();
}

}  // namespace mayag::dsl
