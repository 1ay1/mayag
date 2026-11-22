#pragma once
// mayag::widget find_bar — in-file find and replace
//
//   find_bar(c, "find", query, replacement, opts, match_index, match_count, show_replace)
//
// Zed's search bar: query field, toggle chips for Aa (case), ab (whole
// word) and .* (regex), "3 of 17", prev/next, and an optional replace row
// with Replace / All. The field turns red when the query matches nothing.
//
// Interaction (part ids under `id`):
//   part 0 query field, 1 replace field   -> focus them
//   2 case, 3 word, 4 regex               -> toggle that option
//   5 prev, 6 next, 7 replace one, 8 replace all, 9 close, 10 toggle replace row

#include "control.hpp"
#include "text_field.hpp"
#include "../editor/buffer.hpp"

#include <string>

namespace mayag::dsl {

namespace find_part {
inline constexpr std::size_t query = 0, replace = 1, case_ = 2, word = 3, regex = 4, prev = 5, next = 6,
                             replace_one = 7, replace_all = 8, close = 9, expand = 10;
}

template <ControlContext C>
[[nodiscard]] inline Node find_bar(const C& c, std::string_view id, const TextEditState& query,
                                   const TextEditState& replacement, editor::FindOptions opts,
                                   int match_index, int match_count, bool show_replace, int focus_field = 0) {
    const Theme& t = c.theme;
    const auto chip = [&](std::size_t part, std::string_view glyph, bool on, std::string_view what) {
        const auto pid = part_id(id, part);
        const bool hov = c.hovered(pid);
        return (h(text_owned(std::string{glyph}) | font(t.type(Tier::body)) | semibold
                      | fg(on ? t.on_accent : hov ? t.text_primary : t.text_secondary))
                | size(26, 22) | center | radius(t.rounded(4))
                | bg(on ? t.accent : hov ? t.surface_raised : t.surface.fade(0.0f))
                | pointer_cursor | role(a11y::Role::checkbox) | checked(on) | label(what)
                | id_of(id, part)).build();
    };
    const auto button_ = [&](std::size_t part, std::string_view glyph, bool enabled, std::string_view what) {
        const auto pid = part_id(id, part);
        const bool hov = enabled && c.hovered(pid);
        return (h(text_owned(std::string{glyph}) | font(t.type(Tier::body)) | fg(enabled ? (hov ? t.text_primary : t.text_secondary) : t.text_disabled))
                | size(26, 22) | center | radius(t.rounded(4)) | bg(hov ? t.surface_raised : t.surface.fade(0.0f))
                | cursor(enabled ? CursorShape::pointer : CursorShape::arrow)
                | role(a11y::Role::button) | disabled(!enabled) | label(what) | id_of(id, part)).build();
    };
    const bool none = !query.text.empty() && match_count == 0;
    auto field = [&](const TextEditState& st, std::size_t part, std::string_view ph, bool focused) {
        return (text_field(t, st, focused, part_id(id, part), ph, t.type(Tier::body))
                | width(300) | border(t.stroke(1), none && part == find_part::query ? t.danger : focused ? t.accent : t.border)).build();
    };
    const std::string count = query.text.empty() ? std::string{}
                            : match_count == 0 ? std::string{"No results"}
                            : std::to_string(match_index + 1) + " of " + std::to_string(match_count);
    auto row1 = h(node(button_(find_part::expand, show_replace ? "\u2304" : "\u203A", true, "toggle replace")),
                  node(field(query, find_part::query, "Find", focus_field == 0)),
                  node(chip(find_part::case_, "Aa", opts.case_sensitive, "match case")),
                  node(chip(find_part::word, "ab", opts.whole_word, "whole word")),
                  node(chip(find_part::regex, ".*", opts.regex, "regex")),
                  text_owned(count) | font(t.type(Tier::body)) | fg(none ? t.danger : t.text_secondary) | width(82)
                      | text_align(TextAlign::center),
                  node(button_(find_part::prev, "\u2191", match_count > 0, "previous match")),
                  node(button_(find_part::next, "\u2193", match_count > 0, "next match")),
                  spacer(),
                  node(button_(find_part::close, "\u00D7", true, "close")))
              | gap(t.spaced(4)) | align(Align::center) | width(pct(100));
    Node row2 = show_replace
        ? (h(strut(26), node(field(replacement, find_part::replace, "Replace", focus_field == 1)),
             node(button_(find_part::replace_one, "\u21B5", match_count > 0, "replace")),
             node(button_(find_part::replace_all, "\u21CA", match_count > 0, "replace all")),
             text<"replace / all"> | font(t.type(Tier::small)) | fg(var::text_disabled))
           | gap(t.spaced(4)) | align(Align::center) | width(pct(100))).build()
        : Node{};
    return (v(std::move(row1), node(std::move(row2))) | gap(t.spaced(5)) | pad(t.spaced(6), t.spaced(10)) | width(pct(100))
            | bg(var::surface) | border(t.stroke(1), t.border)).build();
}

}  // namespace mayag::dsl
