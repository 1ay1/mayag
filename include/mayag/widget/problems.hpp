#pragma once
// mayag::widget problems — the diagnostics panel
//
//   problems_panel(c, "problems", groups, selected, m.problems_scroll)
//
// Diagnostics grouped by file, each row with severity, message, source and
// line:col; a file header row with its counts. Rows are virtualised.
//
// Interaction: Clicked on part_id(id, i) -> jump to row i (headers too: the
// app decides what a header click does, typically collapse).

#include "control.hpp"
#include "file_icon.hpp"
#include "virtual_list.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct ProblemRow {
    bool        header = false;
    std::string file;         ///< header: the file; row: unused
    std::string message;
    std::string source;
    int         line = 0, col = 0;
    int         severity = 0; ///< 0 error, 1 warning, 2 info, 3 hint
    int         errors = 0, warnings = 0;   ///< header counts
};

template <ControlContext C>
[[nodiscard]] inline Node problems_panel(const C& c, std::string_view id, const std::vector<ProblemRow>& rows,
                                         int chosen, const ScrollState& list_scroll) {
    const Theme& t = c.theme;
    constexpr float rh = 24.0f;
    auto body = virtual_list(list_scroll, static_cast<int>(rows.size()), rh, [&](int i) -> Node {
        const ProblemRow& r = rows[static_cast<std::size_t>(i)];
        const auto pid = part_id(id, static_cast<std::size_t>(i));
        const bool sel = i == chosen, hov = c.hovered(pid);
        const auto bgc = sel ? t.tint(t.accent, 0.20f) : hov ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f);
        if (r.header) {
            return (h(node(file_icon(t, r.file, 12.0f).build()),
                      text_owned(r.file) | font(t.font_size) | fg(t.text_primary) | grow() | ellipsis | min_size(0, 0),
                      text_owned(r.errors ? std::to_string(r.errors) + " \u2716" : "") | font(t.font_size * 0.85f) | fg(t.danger),
                      text_owned(r.warnings ? std::to_string(r.warnings) + " \u26A0" : "") | font(t.font_size * 0.85f) | fg(t.warning))
                    | gap(8) | align(Align::center) | pad(0, 12) | height(rh) | width(pct(100)) | bg(bgc)
                    | id_of(id, static_cast<std::size_t>(i))).build();
        }
        const Color<Srgb> tone = r.severity == 0 ? t.danger : r.severity == 1 ? t.warning : r.severity == 2 ? t.info : t.text_secondary;
        const char* mark = r.severity == 0 ? "\u2716" : r.severity == 1 ? "\u26A0" : "\u24D8";
        return (h(strut(18),
                  text_owned(mark) | font(t.font_size * 0.85f) | fg(tone) | width(16),
                  text_owned(r.message) | font(t.font_size * 0.95f) | fg(t.text_primary) | ellipsis | grow() | min_size(0, 0),
                  text_owned(r.source) | font(t.font_size * 0.85f) | fg(t.text_disabled),
                  text_owned(std::to_string(r.line + 1) + ":" + std::to_string(r.col + 1)) | font(t.font_size * 0.85f)
                      | fg(t.text_disabled) | width(60) | text_align(TextAlign::right))
                | gap(8) | align(Align::center) | pad(0, 12) | height(rh) | width(pct(100)) | bg(bgc)
                | pointer_cursor | role(a11y::Role::list_item) | selected(sel)
                | id_of(id, static_cast<std::size_t>(i))).build();
    });
    return (v(std::move(body)) | scroll(list_scroll) | grow() | width(pct(100)) | min_size(0, 0) | id_of(id)).build();
}

}  // namespace mayag::dsl
