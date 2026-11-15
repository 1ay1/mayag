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
//   Scrolled on node_id(id)    -> scroll.scroll_by(delta)
//   keyboard (app): up/down select, left collapse/parent, right expand, enter open

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
};

inline constexpr float kTreeRowH = 24.0f;

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
        const Color<Srgb> name_col = r.errors > 0 ? t.danger : r.git ? git_colour(r.git)
                                   : r.active ? t.text_primary : t.text_secondary;
        return (h(h_list(std::move(guides)),
                  node(file_icon(t, r.name, 12.0f, r.dir, r.open).build()),
                  text_owned(r.name) | font(t.font_size) | fg(name_col) | ellipsis | grow() | min_size(0, 0),
                  text_owned(r.errors > 0 ? std::to_string(r.errors) : r.git ? std::string(1, r.git) : std::string{})
                      | font(t.font_size * 0.85f) | fg(r.errors > 0 ? t.danger : git_colour(r.git)))
                | gap(4) | align(Align::center) | pad(0, 10, 0, 8) | height(kTreeRowH) | width(pct(100))
                | bg(sel ? (focused ? t.tint(t.accent, 0.22f) : t.surface_raised)
                         : r.active ? t.surface_raised.fade(0.6f) : hov ? t.surface_raised.fade(0.45f) : t.surface.fade(0.0f))
                | border(1, sel && focused ? t.accent.fade(0.6f) : t.border.fade(0.0f))
                | role(a11y::Role::list_item) | selected(sel) | label(r.name)
                | id_of(id, static_cast<std::size_t>(i))).build();
    });
    return (v(std::move(body)) | scroll(list_scroll) | grow() | width(pct(100)) | min_size(0, 0)
            | role(a11y::Role::list) | id_of(id)).build();
}

}  // namespace mayag::dsl
