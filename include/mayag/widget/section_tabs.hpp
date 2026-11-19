#pragma once
// mayag::widget section_tabs — the top tabs of a panel's sections
//
//   section_tabs(c, "settings.tabs", {{"\uF53F", "theme"}, {"\uF2D0", "chrome", "3"}}, active, pick)
//
// A row of labels with an accent underline under the active one, on a
// hairline that runs under the whole row. Each tab may carry a small count
// (e.g. how many settings in it are modified). Not editor tabs (tab_bar) or
// terminal tabs (term_tabs): these never close or reorder; they divide one
// surface into destinations.
//
// Interaction: Clicked / Activated on part_id(id, i) -> go to tab i. With
// `pick`, each tab carries `pick(i)` as its click handler.

#include "control.hpp"
#include "sheet.hpp"
#include "segmented.hpp"   // detail::NoPick
#include "../app/handlers.hpp"

#include <algorithm>
#include <concepts>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

struct SectionTab {
    std::string icon;    ///< a glyph, or "" for none
    std::string title;
    std::string count;   ///< "" = none
};

namespace detail {
template <ControlContext C, class Pick>
[[nodiscard]] inline Node section_tabs_impl(const C& c, std::string_view id, const std::vector<SectionTab>& tabs,
                                            std::size_t active, const Pick& pick) {
    const Theme& t = c.theme;
    const float bar = std::max(t.mark, 2.0f);
    std::vector<Node> items;
    items.reserve(tabs.size());
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const auto pid = part_id(id, i);
        const bool on = i == active, hov = c.hovered(pid);
        const auto ink = on || hov ? t.text_primary : t.text_secondary;
        const SectionTab& tb = tabs[i];
        std::vector<Node> parts;
        if (!tb.icon.empty())
            parts.push_back((text_owned(tb.icon) | font(t.font_size * 0.95f) | fg(on ? t.accent : ink)).build());
        parts.push_back((text_owned(tb.title) | font(t.font_size) | fg(ink)
                         | (on ? semibold : weight(FontWeight::regular))).build());
        if (!tb.count.empty())
            parts.push_back((text_owned(tb.count) | font(t.font_size * 0.72f) | monospace | fg(t.accent) | pad(1, 5)
                             | radius(t.capsule(14)) | bg(t.accent.fade(0.14f))).build());
        auto row = h(std::move(parts)) | gap(t.spaced(7))
                 | align(Align::center) | pad(t.spaced(9), t.spaced(14), t.spaced(8), t.spaced(14));
        auto tab = v(std::move(row),
                     box() | height(bar) | radius(bar * 0.5f)
                         | bg(on ? t.accent : (hov ? t.text_disabled.fade(0.5f) : t.accent.fade(0.0f))) | transition())
                 | align(Align::stretch) | pointer_cursor | role(a11y::Role::tab) | selected(on) | label(tb.title) | id_of(id, i);
        if constexpr (std::same_as<Pick, NoPick>) items.push_back(std::move(tab).build());
        else items.push_back((std::move(tab) | on_click([m = pick(i)] { return m; })).build());
    }
    return (h(std::move(items)) | gap(t.spaced(2))
            | width(pct(100)) | inner_shadow(0.0f, t.border, {0.0f, -1.0f})
            | role(a11y::Role::group) | label(id)).build();
}
}  // namespace detail

template <ControlContext C>
[[nodiscard]] inline auto section_tabs(const C& c, std::string_view id, const std::vector<SectionTab>& tabs,
                                       std::size_t active) {
    return node(detail::section_tabs_impl(c, id, tabs, active, detail::NoPick{}));
}

/// With its interaction attached: `pick(i)` is the message choosing tab i.
template <ControlContext C, class Pick>
    requires std::invocable<const Pick&, std::size_t>
[[nodiscard]] inline auto section_tabs(const C& c, std::string_view id, const std::vector<SectionTab>& tabs,
                                       std::size_t active, Pick pick) {
    return node(detail::section_tabs_impl(c, id, tabs, active, pick));
}

}  // namespace mayag::dsl
