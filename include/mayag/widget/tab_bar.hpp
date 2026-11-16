#pragma once
// mayag::widget tab_bar — editor tabs
//
//   tab_bar(c, "tabs", tabs, active)
//
// Each tab: file icon, title, a dot when dirty that turns into × on hover,
// a preview tab in italics (single-click opens a preview that the next
// single-click replaces, as in Zed/VS Code), and the directory when two open
// files share a name.
//
// Interaction:
//   Clicked on part_id(id, i)            -> activate tab i
//   Clicked on part_id(id, part_close+i) -> close tab i
//   Clicked with the middle button on part_id(id, i) -> close tab i

#include "control.hpp"
#include "dot.hpp"
#include "file_icon.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

struct TabItem {
    std::string title;
    std::string detail;        ///< shown when titles collide (the parent dir)
    bool        dirty = false;
    bool        preview = false;
    bool        pinned = false;
    int         errors = 0;
};

inline constexpr std::size_t part_close = part_dec + 4096;

template <ControlContext C>
[[nodiscard]] inline Node tab_bar(const C& c, std::string_view id, const std::vector<TabItem>& tabs,
                                  int active, Node trailing = Node{}) {
    const Theme& t = c.theme;
    std::vector<Node> items;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const TabItem& tb = tabs[i];
        const bool on = static_cast<int>(i) == active;
        const auto pid = part_id(id, i), cid = part_id(id, part_close + i);
        const bool hov = c.hovered(pid) || c.hovered(cid);
        const bool show_x = hov || (on && !tb.dirty);
        Node close = show_x
            ? (h(text<"\u00D7"> | font(t.font_size * 1.05f) | fg(c.hovered(cid) ? t.text_primary : t.text_secondary))
               | size(18, 18) | center | radius(t.rounded(4))
               | bg(c.hovered(cid) ? t.surface_raised : t.surface.fade(0.0f))
               | pointer_cursor | label("close tab") | id_of(id, part_close + i)).build()
            : tb.dirty ? (h(node(dot(t.text_secondary, 7).build())) | size(18, 18) | center).build()
                       : (box() | size(18, 18)).build();
        auto title_node = text_owned(tb.title) | font(t.font_size) | fg(on ? t.text_primary : t.text_secondary)
                        | ellipsis;
        Node tn = title_node.build();
        if (tb.preview) tn.style().text.italic = true;
        if (tb.errors > 0) tn.style().text.color = t.danger;
        items.push_back((h(node(file_icon(t, tb.title, t.font_size).build()),
                           node(std::move(tn)),
                           text_owned(tb.detail) | font(t.font_size * 0.85f) | fg(var::text_disabled) | ellipsis,
                           node(std::move(close)))
                         | gap(t.spaced(7)) | align(Align::center) | pad(0, t.spaced(6), 0, t.spaced(12)) | height(pct(100)) | max_size(260, 1e6f)
                         | bg(on ? t.background : (hov ? mix(t.surface, t.background, 0.5f) : t.surface))
                         | role(a11y::Role::tab) | selected(on) | label(tb.title)
                         | id_of(id, i)).build());
        // Hairline separators; the active tab merges into the editor below.
        items.push_back((box() | size(1, 34) | bg(var::border)).build());
    }
    return (h(h(h_list(std::move(items))) | grow() | min_size(0, 0) | clip,
              node(std::move(trailing)))
            | height(34) | width(pct(100)) | align(Align::center)
            | bg(var::surface) | border(t.stroke(1), t.border)).build();
}

}  // namespace mayag::dsl
