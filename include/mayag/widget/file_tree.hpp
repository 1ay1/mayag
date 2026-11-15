#pragma once
// mayag::widget file_tree — the project panel
//
//   std::vector<TreeRow> rows = flatten(root, expanded);   // the app's data
//   file_tree(c, "tree", rows, selected, m.tree_scroll)
//
// The tree is flattened into rows by the caller (only expanded folders
// contribute children), so the widget is a virtualised list: a 50k-file
// monorepo costs what is on screen. Indent guides, chevrons, git status
// colours, the open file highlighted.
//
// Interaction:
//   Clicked on part_id(id, i)  -> select row i; a folder toggles, a file opens
//                                 (count 1 = preview, 2 = keep open)
//   Clicked on part_id(id, kTreeAction + i) -> the row's hover action (a terminal
//                                 uses it for "new tab here" / "open in $EDITOR")
//   Scrolled on node_id(id)    -> scroll.scroll_by(delta)
//   keyboard (app): up/down select, left collapse/parent, right expand, enter open
//
// Terminal use: `tabs` puts one dot per tab whose working directory is that
// folder (in the tab's colour), and `here` marks the folder the focused tab
// is in, so the tree answers "where is everything running?" at a glance.

#include "control.hpp"
#include "file_icon.hpp"
#include "virtual_list.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct TreeRow {
    std::string name;
    int         depth = 0;
    bool        dir = false;
    bool        open = false;      ///< expanded (dirs)
    bool        active = false;    ///< the file in the focused editor
    char        git = 0;           ///< 'M' modified, 'A' added, 'U' untracked, 'D' deleted, 0 clean
    int         errors = 0;
    std::vector<Color<Srgb>> tabs;   ///< one dot per tab sitting in this folder
    bool        here = false;      ///< the focused tab's working directory
    Color<Srgb> here_hue{};        ///< that tab's colour
    std::string action;            ///< hover button label ("+ tab", "edit"), "" none
};

inline constexpr float kTreeRowH = 24.0f;
inline constexpr std::size_t kTreeAction = 1u << 21;   ///< clear of part_dec/inc/menu

template <ControlContext C>
[[nodiscard]] inline Node file_tree(const C& c, std::string_view id, const std::vector<TreeRow>& rows,
                                    int chosen, const ScrollState& list_scroll, bool focused = true) {
    const Theme& t = c.theme;
    const auto git_colour = [&](char g) {
        switch (g) { case 'M': return t.warning; case 'A': case 'U': return t.success; case 'D': return t.danger; default: return t.text_secondary; }
    };
    auto body = virtual_list(list_scroll, static_cast<int>(rows.size()), kTreeRowH, [&](int i) -> Node {
        const TreeRow& r = rows[static_cast<std::size_t>(i)];
        const auto pid = part_id(id, static_cast<std::size_t>(i));
        const bool sel = i == chosen, hov = c.hovered(pid);
        std::vector<Node> guides;
        for (int d = 0; d < r.depth; ++d)
            guides.push_back((h(box() | size(1, kTreeRowH) | bg(t.border.fade(0.8f))) | width(14) | center).build());
        const auto aid = part_id(id, kTreeAction + static_cast<std::size_t>(i));
        const bool row_hov = hov || c.hovered(aid);
        const Color<Srgb> name_col = r.errors > 0 ? t.danger : r.git ? git_colour(r.git)
                                   : r.active || r.here ? t.text_primary : t.text_secondary;
        std::vector<Node> dots;
        for (std::size_t k = 0; k < r.tabs.size() && k < 5; ++k)
            dots.push_back((box() | size(6, 6) | radius(3) | bg(r.tabs[k])).build());
        if (r.tabs.size() > 5)
            dots.push_back((text_owned("+" + std::to_string(r.tabs.size() - 5)) | font(t.font_size * 0.7f) | fg(t.text_disabled)).build());
        Node action = !r.action.empty() && row_hov
            ? (h(text_owned(r.action) | font(t.font_size * 0.78f) | fg(c.hovered(aid) ? t.text_primary : t.text_secondary))
               | pad(1, 6) | radius(4) | bg(c.hovered(aid) ? t.surface_raised : t.surface.fade(0.0f))
               | border(1, c.hovered(aid) ? t.border_strong : t.border) | pointer_cursor | id_of(id, kTreeAction + static_cast<std::size_t>(i))).build()
            : Node{};
        return (h(box() | size(2, r.here ? kTreeRowH - 8.0f : 0.0f) | radius(1) | bg(r.here_hue) | margin(0, 4, 0, -6),
                  h_list(std::move(guides)),
                  node(file_icon(t, r.name, 12.0f, r.dir, r.open).build()),
                  text_owned(r.name) | font(t.font_size) | (r.here ? semibold : weight(FontWeight::regular)) | fg(name_col) | ellipsis | grow() | min_size(0, 0),
                  node(std::move(action)),
                  h_list(std::move(dots), 3.0f) | align(Align::center),
                  text_owned(r.errors > 0 ? std::to_string(r.errors) : r.git ? std::string(1, r.git) : std::string{})
                      | font(t.font_size * 0.85f) | fg(r.errors > 0 ? t.danger : git_colour(r.git)))
                | gap(4) | align(Align::center) | pad(0, 10, 0, 8) | height(kTreeRowH) | width(pct(100))
                | bg(sel ? (focused ? t.tint(t.accent, 0.22f) : t.surface_raised)
                         : r.here ? mix(t.surface, r.here_hue, 0.10f)
                         : r.active ? t.surface_raised.fade(0.6f) : row_hov ? t.surface_raised.fade(0.45f) : t.surface.fade(0.0f))
                | border(1, sel && focused ? t.accent.fade(0.6f) : t.border.fade(0.0f))
                | role(a11y::Role::list_item) | selected(sel) | label(r.name)
                | id_of(id, static_cast<std::size_t>(i))).build();
    });
    return (v(std::move(body)) | scroll(list_scroll) | grow() | width(pct(100)) | min_size(0, 0)
            | role(a11y::Role::list) | id_of(id)).build();
}

}  // namespace mayag::dsl
