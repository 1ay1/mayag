#pragma once
// mayag::widget segmented — one of N, as a row of joined segments
//
//   segmented(c, "density", {"compact", "cozy"}, chosen)            // ids only
//   segmented(c, "density", {"compact", "cozy"}, chosen, pick)      // + handlers
//
// Interaction: Clicked / Activated on part_id(id, i) -> choose i. With
// `pick`, each segment carries `pick(i)` (a message or Edit) as its click
// handler, so the app needs no on(Clicked) branch.

#include "control.hpp"
#include "sheet.hpp"
#include "../app/handlers.hpp"

#include <concepts>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

namespace detail {
struct NoPick {};

template <ControlContext C, class Pick>
[[nodiscard]] inline Node segmented_impl(const C& c, std::string_view id, const std::vector<std::string_view>& options,
                                         int chosen, const Pick& pick) {
    const Theme& t = c.theme;
    std::vector<Node> segs;
    segs.reserve(options.size());
    for (std::size_t i = 0; i < options.size(); ++i) {
        const bool on  = static_cast<int>(i) == chosen;
        auto seg = h(text_owned(std::string{options[i]})
                         | font(t.font_size * 0.92f)
                         | (on ? semibold : weight(FontWeight::regular)))
                 | pad(t.spaced(5), t.spaced(12)) | center
                 | with_look(sheet_of(t).segment, t)
                 | pointer_cursor
                 | role(a11y::Role::radio) | checked(on) | label(options[i])
                 | id_of(id, i);
        if constexpr (std::same_as<Pick, NoPick>) segs.push_back(std::move(seg).build());
        else segs.push_back((std::move(seg) | on_click([m = pick(i)] { return m; })).build());
    }
    return (h(h_list(std::move(segs), 2.0f))
            | pad(t.spaced(3)) | bg(var::surface) | border(t.stroke(1), t.border) | radius(t.radius_small)
            | role(a11y::Role::group) | label(id)).build();
}
}  // namespace detail

template <ControlContext C>
[[nodiscard]] inline auto segmented(const C& c, std::string_view id, const std::vector<std::string_view>& options,
                                    int chosen) {
    return node(detail::segmented_impl(c, id, options, chosen, detail::NoPick{}));
}

/// With its interaction attached: `pick(i)` is the message choosing i.
template <ControlContext C, class Pick>
    requires std::invocable<const Pick&, std::size_t>
[[nodiscard]] inline auto segmented(const C& c, std::string_view id, const std::vector<std::string_view>& options,
                                    int chosen, Pick pick) {
    return node(detail::segmented_impl(c, id, options, chosen, pick));
}

}  // namespace mayag::dsl
