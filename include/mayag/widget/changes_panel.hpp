#pragma once
// mayag::widget changes_panel — review what an agent changed
//
//   changes_panel(c, "changes", files, selected, diff_lines)
//
// The agent wrote code in a worktree; before merging you read it. Left: the
// changed files with status (M/A/D/R) and +/−; right: the selected file's
// diff (diff_view). Header: branch, totals, and Commit / Discard / Open PR.
// Data comes from `git diff` — the app runs it, this draws it.
//
// Interaction: part_id(id, i) select file i; part_id(id, 4096 + k) for
// changes_action::{commit, discard, pr, stage}.

#include "control.hpp"
#include "diff_view.hpp"
#include "file_icon.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

namespace changes_action { inline constexpr std::size_t commit = 0, discard = 1, pr = 2, stage = 3; }

struct ChangedFile {
    std::string path;
    char        status = 'M';    ///< M A D R
    int         added = 0, removed = 0;
    bool        staged = false;
};

template <ControlContext C>
[[nodiscard]] inline Node changes_panel(const C& c, std::string_view id, std::string_view branch,
                                        const std::vector<ChangedFile>& files, int chosen, const std::vector<DiffLine>& diff) {
    const Theme& t = c.theme;
    int add = 0, rem = 0;
    std::vector<Node> rows;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        add += f.added; rem += f.removed;
        const auto pid = part_id(id, i);
        const bool on = static_cast<int>(i) == chosen;
        const Color<Srgb> st = f.status == 'A' ? t.success : f.status == 'D' ? t.danger : f.status == 'R' ? t.info : t.warning;
        const auto slash = f.path.rfind('/');
        rows.push_back((h(node(file_icon(t, f.path, 11.0f).build()),
                          text_owned(slash == std::string::npos ? f.path : f.path.substr(slash + 1)) | font(t.font_size * 0.88f)
                              | fg(on ? t.text_primary : t.text_secondary) | ellipsis,
                          text_owned(slash == std::string::npos ? std::string{} : f.path.substr(0, slash)) | font(t.font_size * 0.75f)
                              | fg(var::text_disabled) | ellipsis | grow() | min_size(0, 0),
                          text_owned("+" + std::to_string(f.added)) | font(t.font_size * 0.75f) | fg(var::success),
                          text_owned("\u2212" + std::to_string(f.removed)) | font(t.font_size * 0.75f) | fg(var::danger),
                          text_owned(std::string(1, f.status)) | font(t.font_size * 0.75f) | bold | fg(st) | width(10))
                        | gap(t.spaced(6)) | align(Align::center) | pad(t.spaced(4), t.spaced(8)) | width(pct(100)) | radius(t.rounded(4))
                        | bg(on ? t.tint(t.accent, 0.18f) : c.hovered(pid) ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f))
                        | pointer_cursor | role(a11y::Role::list_item) | selected(on) | id_of(id, i)).build());
    }
    const auto btn = [&](std::size_t k, const char* label_, bool primary, bool danger) {
        const auto pid = part_id(id, 4096 + k);
        const bool hov = c.hovered(pid);
        return (h(text_owned(label_) | font(t.font_size * 0.85f) | semibold
                      | fg(primary ? t.on_accent : danger && hov ? t.danger : hov ? t.text_primary : t.text_secondary))
                | pad(t.spaced(3), t.spaced(10)) | radius(t.rounded(4)) | bg(primary ? (hov ? t.accent_hover : t.accent) : hov ? t.surface_raised : t.surface.fade(0.0f))
                | border(t.stroke(1), primary ? t.accent : t.border) | pointer_cursor | id_of(id, 4096 + k)).build();
    };
    return (v(h(text<"\u2387"> | font(t.font_size) | fg(var::text_disabled),
                text_owned(std::string{branch}) | font(t.font_size * 0.95f) | semibold | fg(var::text_primary),
                text_owned(std::to_string(files.size()) + " files") | font(t.font_size * 0.8f) | fg(var::text_disabled),
                text_owned("+" + std::to_string(add)) | font(t.font_size * 0.8f) | semibold | fg(var::success),
                text_owned("\u2212" + std::to_string(rem)) | font(t.font_size * 0.8f) | semibold | fg(var::danger),
                spacer(),
                node(btn(changes_action::discard, "Discard", false, true)),
                node(btn(changes_action::pr, "Open PR", false, false)),
                node(btn(changes_action::commit, "Commit", true, false)))
                  | gap(t.spaced(8)) | align(Align::center) | pad(t.spaced(8), t.spaced(12)) | width(pct(100)) | bg(var::surface) | border(t.stroke(1), t.border),
              h(v(v_list(std::move(rows), 1.0f)) | width(280) | pad(t.spaced(6)) | height(pct(100)) | border(t.stroke(1), t.border),
                v(node(diff.empty() ? (v(text<"No changes"> | font(t.font_size) | fg(var::text_disabled)) | pad(t.spaced(20))).build()
                                    : diff_view(t, diff, t.font_size * 0.92f, 3)))
                    | grow() | min_size(0, 0) | pad(t.spaced(6), 0) | clip)
                  | grow() | width(pct(100)) | min_size(0, 0) | align(Align::stretch))
            | width(pct(100)) | height(pct(100)) | bg(var::background) | role(a11y::Role::group) | label("changes")).build();
}

}  // namespace mayag::dsl
