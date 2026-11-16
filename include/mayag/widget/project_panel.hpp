#pragma once
// mayag::widget project_panel — a workspace's folder, its worktrees, and where
// every tab is sitting in it
//
//   project_panel(c, "proj", project, rows, tree_sel, tree_scroll, time)
//
// A terminal workspace is usually backed by a folder, and usually that folder
// is a git repo with a few worktrees (main, a fix branch, one per agent). This
// panel puts that on screen next to the terminal:
//
//   * a header: the repo's name, its root, and the branch the focused tab is on
//   * WORKTREES: one compact row each, with branch, path tail, ahead/behind,
//     a dirty count, and one dot per tab whose cwd is inside it. The worktree
//     the tree shows is highlighted; the focused tab's worktree gets a bar in
//     its colour. Hover: "+ tab" opens a shell there.
//   * the FILE TREE of the shown worktree (file_tree), with tab dots on each
//     folder and the focused tab's folder marked "here"
//
// The tree FOLLOWS the focused tab: switch to a tab in another worktree and
// the panel shows that worktree, its cwd revealed. That rule lives in the
// app's update() (expanding ancestors is model state); the panel just draws.
//
// Interaction:
//   Clicked on part_id(id, i)                -> show worktree i
//   Clicked on part_id(id, kTreeAction + i)  -> new tab in worktree i
//   Clicked on part_id(id, part_inc)         -> new worktree
//   rows: see file_tree (its id is `<id>.tree`, i.e. tree_id(id))
//   Scrolled on tree_id(id)                  -> tree scroll

#include "control.hpp"
#include "file_tree.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct ProjectWorktree {
    std::string branch;                 ///< "main", "fix/cell-px"
    std::string path;                   ///< "~/src/mayag-wt/cell-px"
    int         ahead = 0, behind = 0;
    int         dirty = 0;              ///< changed files
    std::vector<Color<Srgb>> tabs;      ///< one dot per tab inside it
    bool        here = false;           ///< the focused tab is in it
    Color<Srgb> here_hue{};
};

struct ProjectInfo {
    std::string name;                   ///< "mayag"
    std::string root;                   ///< "~/src/mayag"
    bool        git = true;
    std::vector<ProjectWorktree> worktrees;
    int         shown = 0;              ///< the worktree whose tree is listed
    std::string empty;                  ///< shown instead when there is no folder ("ssh workspace")
};

/// The id of the panel's file tree (route its clicks and wheel here).
[[nodiscard]] inline std::string tree_id(std::string_view id) { return std::string(id) + ".tree"; }

template <ControlContext C>
[[nodiscard]] inline Node project_panel(const C& c, std::string_view id, const ProjectInfo& p,
                                        const std::vector<TreeRow>& rows, int tree_sel, const ScrollState& tree_scroll,
                                        bool focused = false, float w = 264.0f) {
    const Theme& t = c.theme;
    const float fs = t.font_size;
    const auto caption = [&](std::string s) {
        return text_owned(std::move(s)) | font(fs * 0.72f) | semibold | tracking(0.8f) | fg(var::text_disabled);
    };
    const auto dots_of = [&](const std::vector<Color<Srgb>>& cs) {
        std::vector<Node> d;
        for (std::size_t k = 0; k < cs.size() && k < 4; ++k) d.push_back((box() | size(6, 6) | radius(t.rounded(3)) | bg(cs[k])).build());
        if (cs.size() > 4) d.push_back((text_owned("+" + std::to_string(cs.size() - 4)) | font(fs * 0.7f) | fg(var::text_disabled)).build());
        return (h_list(std::move(d), 3.0f) | align(Align::center)).build();
    };

    if (!p.empty.empty() || p.root.empty()) {
        return (v(caption("FOLDER"),
                  text_owned(p.empty.empty() ? std::string{"No folder"} : p.empty) | font(fs * 0.9f) | fg(var::text_secondary) | wrap_text,
                  text<"Open a tab in a folder, or drop one here, to see its files."> | font(fs * 0.8f) | fg(var::text_disabled) | wrap_text)
                | gap(t.spaced(8)) | pad(t.spaced(14), t.spaced(14)) | width(w) | height(pct(100)) | bg(mix(t.surface, colors::black, 0.06f))
                | border(t.stroke(1), t.border) | id_of(id)).build();
    }

    // ── header ──────────────────────────────────────────────────────────
    const ProjectWorktree* cur = nullptr;
    for (const auto& wt : p.worktrees) if (wt.here) cur = &wt;
    const ProjectWorktree& shown = p.worktrees[static_cast<std::size_t>(std::clamp(p.shown, 0, static_cast<int>(p.worktrees.size()) - 1))];
    Node header = (v(h(node(file_icon(t, p.name, 13.0f, true, true).build()),
                       text_owned(p.name) | font(fs * 1.05f) | semibold | fg(var::text_primary) | ellipsis | grow() | min_size(0, 0),
                       text_owned(p.git ? "\u2387 " + (cur ? cur->branch : shown.branch) : std::string{})
                           | font(fs * 0.78f) | fg(cur ? cur->here_hue : t.text_secondary) | ellipsis)
                         | gap(t.spaced(6)) | align(Align::center) | width(pct(100)),
                     text_owned(p.root) | font(fs * 0.75f) | fg(var::text_disabled) | ellipsis)
                   | gap(t.spaced(3)) | pad(t.spaced(12), t.spaced(12), t.spaced(8), t.spaced(12)) | width(pct(100))).build();

    // ── worktrees ───────────────────────────────────────────────────────
    std::vector<Node> wts;
    if (p.git) {
        for (std::size_t i = 0; i < p.worktrees.size(); ++i) {
            const auto& wt = p.worktrees[i];
            const auto pid = part_id(id, i), aid = part_id(id, kTreeAction + i);
            const bool on = static_cast<int>(i) == p.shown, hov = c.hovered(pid) || c.hovered(aid);
            const std::string tail = wt.path.substr(wt.path.rfind('/') == std::string::npos ? 0 : wt.path.rfind('/') + 1);
            std::string sync;
            if (wt.ahead) sync += "\u2191" + std::to_string(wt.ahead);
            if (wt.behind) sync += (sync.empty() ? "" : " ") + std::string("\u2193") + std::to_string(wt.behind);
            Node act = hov ? (h(text<"+ tab"> | font(fs * 0.74f) | fg(c.hovered(aid) ? t.text_primary : t.text_secondary))
                              | pad(0, t.spaced(5)) | radius(t.rounded(4)) | border(t.stroke(1), c.hovered(aid) ? t.border_strong : t.border)
                              | bg(c.hovered(aid) ? t.surface_raised : t.surface.fade(0.0f))
                              | pointer_cursor | label("new tab here") | id_of(id, kTreeAction + i)).build()
                           : Node{};
            wts.push_back((h(box() | size(2, 18.0f) | radius(t.rounded(1)) | bg(wt.here ? wt.here_hue : wt.here_hue.fade(0.0f)),
                             text<"\u2387"> | font(fs * 0.85f) | fg(on ? t.accent : t.text_disabled) | width(12),
                             v(text_owned(wt.branch) | font(fs * 0.88f) | (on ? semibold : weight(FontWeight::regular))
                                   | fg(on ? t.text_primary : t.text_secondary) | ellipsis,
                               text_owned(tail) | font(fs * 0.7f) | fg(var::text_disabled) | ellipsis)
                                 | gap(t.spaced(1)) | grow() | min_size(0, 0),
                             node(std::move(act)),
                             node(dots_of(wt.tabs)),
                             text_owned(wt.dirty ? std::to_string(wt.dirty) : std::string{}) | font(fs * 0.74f) | fg(var::warning),
                             text_owned(sync) | font(fs * 0.7f) | fg(var::text_disabled))
                           | gap(t.spaced(6)) | align(Align::center) | pad(t.spaced(4), t.spaced(8), t.spaced(4), t.spaced(6)) | width(pct(100)) | radius(t.radius_small)
                           | bg(on ? t.surface_raised : hov ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f))
                           | pointer_cursor | role(a11y::Role::list_item) | selected(on) | label(wt.branch)
                           | id_of(id, i)).build());
        }
    }
    const auto nid = part_id(id, part_inc);
    Node wt_section = p.git
        ? (v(h(caption("WORKTREES"), spacer(),
               h(text<"+"> | font(fs * 0.95f) | fg(c.hovered(nid) ? t.text_primary : t.text_disabled))
                   | size(18, 18) | center | radius(t.rounded(4)) | bg(c.hovered(nid) ? t.surface_raised : t.surface.fade(0.0f))
                   | pointer_cursor | label("new worktree") | id_of(id, part_inc))
                 | align(Align::center) | pad(0, t.spaced(6), 0, t.spaced(4)) | width(pct(100)),
             v(v_list(std::move(wts), 1.0f)) | width(pct(100)))
           | gap(t.spaced(4)) | pad(0, t.spaced(6), t.spaced(8), t.spaced(6)) | width(pct(100)) | role(a11y::Role::list) | label("worktrees")).build()
        : Node{};

    const std::string tid = tree_id(id);
    return (v(node(std::move(header)),
              node(std::move(wt_section)),
              box() | height(1) | width(pct(100)) | bg(var::border),
              h(caption(p.git ? shown.branch == "main" || p.worktrees.size() < 2 ? "FILES" : "FILES \u00B7 " + shown.branch : "FILES"))
                  | pad(t.spaced(8), t.spaced(12), t.spaced(4), t.spaced(12)) | width(pct(100)),
              v(node(file_tree(c, tid, rows, tree_sel, tree_scroll, focused))) | grow() | min_size(0, 0) | width(pct(100)))
            | width(w) | height(pct(100)) | bg(mix(t.surface, colors::black, 0.06f))
            | border(t.stroke(1), t.border) | role(a11y::Role::group) | label("project") | id_of(id)).build();
}

}  // namespace mayag::dsl
