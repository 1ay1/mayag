#pragma once
// mayag::widget status_bar — the editor's bottom strip
//
//   status_bar(c, "status", left_items, right_items)
//
// Clickable segments, each an icon/label pair with an optional tone:
// left for panel toggles, diagnostics counts and the branch; right for
// cursor position, selection size, indentation, encoding, line endings and
// language — the conventional order, so users find things where they expect.
//
// Interaction: Clicked on part_id(id, i) for left items, part_id(id, 100+i)
// for right items.

#include "control.hpp"
#include "sheet.hpp"

#include <string>
#include <vector>

namespace mayag::dsl {

struct StatusItem {
    std::string text;
    std::string icon;
    enum class Tone : std::uint8_t { normal, dim, accent, error, warning, success } tone = Tone::normal;
    bool active = false;   ///< a toggled panel
    /// Clickable: hover wash and a pointer cursor. A plain readout (the
    /// encoding, a size) is neither -- a hand over text that does nothing
    /// is a lie.
    bool interactive = true;
};

template <ControlContext C>
[[nodiscard]] inline Node status_bar(const C& c, std::string_view id, const std::vector<StatusItem>& left,
                                     const std::vector<StatusItem>& right) {
    const Theme& t = c.theme;
    const auto seg = [&](const StatusItem& it, std::size_t part) {
        const auto pid = part_id(id, part);
        const bool hov = c.hovered(pid);
        using T = StatusItem::Tone;
        const Color<Srgb> col = it.tone == T::error ? t.danger : it.tone == T::warning ? t.warning
                              : it.tone == T::success ? t.success : it.tone == T::accent ? t.accent
                              : it.tone == T::dim ? t.text_disabled : t.text_secondary;
        const bool live = it.interactive;
        auto body = h(text_owned(it.icon) | font(t.font_size * t.chrome_type) | fg(it.active ? t.accent : col),
                      text_owned(it.text) | font(t.font_size * t.chrome_type * 0.95f) | fg(live && hov ? t.text_primary : col))
                  | gap(t.spaced(5)) | align(Align::center) | pad(0, t.spaced(7)) | height(pct(100))
                  | bg(live && hov ? t.surface_raised : it.active ? t.surface_raised.fade(0.6f) : t.surface.fade(0.0f))
                  | transition();
        if (!live) return (std::move(body) | role(a11y::Role::text)).build();
        return (std::move(body) | pointer_cursor | role(a11y::Role::button) | label(it.text.empty() ? it.icon : it.text)
                | id_of(id, part)).build();
    };
    std::vector<Node> l, r;
    for (std::size_t i = 0; i < left.size(); ++i) l.push_back(seg(left[i], i));
    for (std::size_t i = 0; i < right.size(); ++i) r.push_back(seg(right[i], 100 + i));
    return (h(h(h_list(std::move(l))) | height(pct(100)), spacer(), h(h_list(std::move(r))) | height(pct(100)))
            | align(Align::center) | pad(0, t.spaced(4)) | height(t.status_height) | width(pct(100))
            | with_look(sheet_of(t).strip, t)
            | inner_shadow(0.0f, t.border, {0.0f, 1.0f})).build();
}

}  // namespace mayag::dsl
