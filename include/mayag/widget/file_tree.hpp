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
//   keyboard: tree_key(key, selected, scroll, rows) does the standard keys
//   and returns a TreeIntent; tree_click(row, count, selected, rows) the same
//   for clicks. The app applies the intent (open the file, flip expanded).
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

// ── behaviour ───────────────────────────────────────────────────────────
//
// The tree's keyboard and click handling, so apps don't each rewrite it.
// They move the selection and scroll themselves and return what the app
// should do with the result (open a file, toggle a folder, give up focus).

/// What a key or click on the tree asks the app to do.
struct TreeIntent {
    enum class Act : std::uint8_t {
        none,
        moved,      ///< selection moved (already applied)
        toggle,     ///< folder at `row`: flip expanded
        expand,     ///< folder at `row`: expand (it is collapsed)
        collapse,   ///< folder at `row`: collapse (it is expanded)
        open,       ///< file at `row`: open and keep it
        preview,    ///< file at `row`: open as a preview, keep focus here
        leave,      ///< hand focus back (Escape)
    };
    Act act = Act::none;
    int row = -1;
};

/// Scroll the tree so `row` is in view.
inline void tree_reveal(ScrollState& scroll, int row) {
    const float y = static_cast<float>(row) * kTreeRowH;
    if (y < scroll.offset.y) scroll.scroll_to({scroll.offset.x, y});
    else if (y + kTreeRowH > scroll.offset.y + scroll.viewport.y)
        scroll.scroll_to({scroll.offset.x, y + kTreeRowH - scroll.viewport.y});
}

/// The row of `row`'s parent folder, or -1 at the top level.
[[nodiscard]] inline int tree_parent(const std::vector<TreeRow>& rows, int row) {
    if (row <= 0 || row >= static_cast<int>(rows.size())) return -1;
    const int depth = rows[static_cast<std::size_t>(row)].depth;
    for (int r = row - 1; r >= 0; --r)
        if (rows[static_cast<std::size_t>(r)].depth < depth) return r;
    return -1;
}

/// Up/down (or j/k) move, right/l expand, left/h collapse or go to the
/// parent, Enter opens (or toggles a folder), Space previews, Escape leaves.
[[nodiscard]] inline TreeIntent tree_key(Key key, int& selected, ScrollState& scroll,
                                         const std::vector<TreeRow>& rows) {
    using A = TreeIntent::Act;
    if (rows.empty()) return {};
    const int last = static_cast<int>(rows.size()) - 1;
    selected = selected < 0 ? 0 : (selected > last ? last : selected);
    const TreeRow& r = rows[static_cast<std::size_t>(selected)];
    const auto move = [&](int to) {
        selected = to < 0 ? 0 : (to > last ? last : to);
        tree_reveal(scroll, selected);
        return TreeIntent{A::moved, selected};
    };
    switch (key) {
        case Key::up: case Key::k: return move(selected - 1);
        case Key::down: case Key::j: return move(selected + 1);
        case Key::right: case Key::l:
            return r.dir && !r.open ? TreeIntent{A::expand, selected} : TreeIntent{};
        case Key::left: case Key::h:
            if (r.dir && r.open) return {A::collapse, selected};
            if (const int p = tree_parent(rows, selected); p >= 0) return move(p);
            return {};
        case Key::enter: return {r.dir ? A::toggle : A::open, selected};
        case Key::space: return {r.dir ? A::toggle : A::preview, selected};
        case Key::escape: return {A::leave, selected};
        default: return {};
    }
}

/// A click on row `row` (`count` 2 for a double click): selects it; a folder
/// toggles, a file previews on a single click and opens on a double.
[[nodiscard]] inline TreeIntent tree_click(int row, int count, int& selected,
                                           const std::vector<TreeRow>& rows) {
    using A = TreeIntent::Act;
    if (row < 0 || row >= static_cast<int>(rows.size())) return {};
    selected = row;
    if (rows[static_cast<std::size_t>(row)].dir) return {A::toggle, row};
    return {count >= 2 ? A::open : A::preview, row};
}

namespace detail {
template <ControlContext C>
[[nodiscard]] inline Node file_tree_build(const C& c, std::string_view id, const std::vector<TreeRow>& rows,
                                          int chosen, const ScrollState& list_scroll, bool focused);
}

/// Memoised: rebuilt only when its rows, selection, focus or scroll change
/// (hover inside it is tracked by the cache).
template <ControlContext C>
[[nodiscard]] inline Node file_tree(const C& c, std::string_view id, const std::vector<TreeRow>& rows,
                                    int chosen, const ScrollState& list_scroll, bool focused = true) {
    MemoKey k;
    k.add(std::string_view{"file_tree"}).add(id).add(chosen).add(focused)
     .add(list_scroll.offset).add(list_scroll.viewport).add(rows.size());
    for (const auto& r : rows)
        k.add(r.name).add(r.depth).add(r.dir).add(r.open).add(r.active).add(r.git).add(r.errors)
         .add(r.tabs).add(r.here).add(r.here_hue).add(r.action);
    return memo_widget(c, k, [&] { return detail::file_tree_build(c, id, rows, chosen, list_scroll, focused); });
}

template <ControlContext C>
[[nodiscard]] inline Node detail::file_tree_build(const C& c, std::string_view id, const std::vector<TreeRow>& rows,
                                                  int chosen, const ScrollState& list_scroll, bool focused) {
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
            dots.push_back((box() | size(6, 6) | radius(t.rounded(3)) | bg(r.tabs[k])).build());
        if (r.tabs.size() > 5)
            dots.push_back((text_owned("+" + std::to_string(r.tabs.size() - 5)) | font(t.font_size * 0.7f) | fg(var::text_disabled)).build());
        Node action = !r.action.empty() && row_hov
            ? (h(text_owned(r.action) | font(t.font_size * 0.78f) | fg(c.hovered(aid) ? t.text_primary : t.text_secondary))
               | pad(t.spaced(1), t.spaced(6)) | radius(t.rounded(4)) | bg(c.hovered(aid) ? t.surface_raised : t.surface.fade(0.0f))
               | border(t.stroke(1), c.hovered(aid) ? t.border_strong : t.border) | pointer_cursor | id_of(id, kTreeAction + static_cast<std::size_t>(i))).build()
            : Node{};
        return (h(box() | size(2, kTreeRowH - 8.0f) | radius(t.rounded(1)) | bg(r.here ? r.here_hue : r.here_hue.fade(0.0f)) | margin(0, 4, 0, -6),
                  h_list(std::move(guides)),
                  node(file_icon(t, r.name, 12.0f, r.dir, r.open).build()),
                  text_owned(r.name) | font(t.font_size) | (r.here ? semibold : weight(FontWeight::regular)) | fg(name_col) | ellipsis | grow() | min_size(0, 0),
                  node(std::move(action)),
                  h_list(std::move(dots), 3.0f) | align(Align::center),
                  text_owned(r.errors > 0 ? std::to_string(r.errors) : r.git ? std::string(1, r.git) : std::string{})
                      | font(t.font_size * 0.85f) | fg(r.errors > 0 ? t.danger : git_colour(r.git)))
                | gap(t.spaced(4)) | align(Align::center) | pad(0, t.spaced(10), 0, t.spaced(8)) | height(kTreeRowH) | width(pct(100))
                | bg(sel ? (focused ? t.tint(t.accent, 0.22f) : t.surface_raised)
                         : r.here ? mix(t.surface, r.here_hue, 0.10f)
                         : r.active ? t.surface_raised.fade(0.6f) : row_hov ? t.surface_raised.fade(0.45f) : t.surface.fade(0.0f))
                | border(t.stroke(1), sel && focused ? t.accent.fade(0.6f) : t.border.fade(0.0f))
                | role(a11y::Role::list_item) | selected(sel) | label(r.name)
                | id_of(id, static_cast<std::size_t>(i))).build();
    });
    return (v(std::move(body)) | scroll(list_scroll) | grow() | width(pct(100)) | min_size(0, 0)
            | role(a11y::Role::list) | id_of(id)).build();
}

}  // namespace mayag::dsl
