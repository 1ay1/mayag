#pragma once
// mayag::widget picker — the fuzzy modal: command palette, file finder,
// go-to-symbol, theme picker
//
//   picker(c, "palette", query_state, items, selected, PickerStyle{...})
//
// One widget for every "type to narrow a list" modal. Items come pre-ranked
// from the app (rank_items does the fuzzy part); the widget draws the query
// field, the list with matched characters highlighted, a right-aligned hint
// per item (a keybinding, a path, a line number) and a footer. It posts
// itself as a modal OVERLAY anchored to the top of the window.
//
// Interaction:
//   Clicked on part_id(id, i)          -> choose item i
//   Left on part_id(id, part_menu)     -> dismissed by a click outside
//   keys: picker_key(query, key, mods, selected, count) moves, edits the
//         query and returns a PickerIntent (choose / close) for the app

#include "control.hpp"
#include "file_icon.hpp"
#include "fuzzy.hpp"
#include "key_hint.hpp"
#include "text_field.hpp"
#include "../scene/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace mayag::dsl {

/// What a key in the picker asks the app to do.
struct PickerIntent {
    enum class Act : std::uint8_t { none, moved, edited, choose, close };
    Act act = Act::none;
    int index = -1;   ///< choose: the selected item
};

/// Up/down and ctrl-n/ctrl-p move the selection (clamped to `count` items
/// when it's known, >= 0), Enter chooses, Escape closes, and everything else
/// edits the query (which puts the selection back on the first item).
[[nodiscard]] inline PickerIntent picker_key(TextEditState& query, Key key, Mods mods, int& selected,
                                             int count = -1) {
    using A = PickerIntent::Act;
    const auto clamp = [&](int v) {
        if (v < 0) return 0;
        if (count >= 0 && v > count - 1) return count > 0 ? count - 1 : 0;
        return v;
    };
    if (key == Key::escape) return {A::close};
    if (key == Key::enter) return {A::choose, clamp(selected)};
    if (key == Key::down || (mods.ctrl && key == Key::n)) { selected = clamp(selected + 1); return {A::moved, selected}; }
    if (key == Key::up || (mods.ctrl && key == Key::p)) { selected = clamp(selected - 1); return {A::moved, selected}; }
    if (query.handle_key(key, mods)) { selected = 0; return {A::edited, 0}; }
    return {};
}

struct PickerItem {
    std::string label;
    std::string detail;     ///< dimmed after the label (a path, a category)
    std::string hint;       ///< right-aligned (a keybinding)
    std::string icon_name;  ///< a file name for file_icon(), or empty
};

struct PickerStyle {
    std::string placeholder = "Type to search\u2026";
    std::string prefix;            ///< drawn before the query (">", "@", ":")
    float       width = 620.0f;
    int         max_rows = 12;
    std::string empty_text = "No matches";
    std::vector<std::pair<std::string, std::string>> footer;   ///< key hints
};

/// Fuzzy-rank `items` by `query` (stable for equal scores). Returns the item
/// indices in rank order and the matched positions per ranked item.
struct Ranked { std::vector<int> order; std::vector<FuzzyMatch> matches; };
[[nodiscard]] inline Ranked rank_items(const std::vector<PickerItem>& items, std::string_view query,
                                       std::size_t limit = 200) {
    Ranked r;
    std::vector<std::pair<int, FuzzyMatch>> hits;
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto m = fuzzy_match(query, items[i].label);
        if (!m.ok && !items[i].detail.empty()) {   // fall back to matching "detail/label"
            auto m2 = fuzzy_match(query, items[i].detail + "/" + items[i].label);
            if (m2.ok) { m2.score -= 4; m2.positions.clear(); m = std::move(m2); }
        }
        if (m.ok) hits.emplace_back(static_cast<int>(i), std::move(m));
    }
    if (!query.empty())
        std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.second.score > b.second.score; });
    for (auto& [i, m] : hits) {
        if (r.order.size() >= limit) break;
        r.order.push_back(i);
        r.matches.push_back(std::move(m));
    }
    return r;
}

template <ControlContext C>
inline void picker(const C& c, std::string_view id, const TextEditState& query,
                   const std::vector<PickerItem>& items, const Ranked& ranked, int chosen,
                   const PickerStyle& st = {}) {
    const Theme& t = c.theme;
    const int n = static_cast<int>(ranked.order.size());
    const int sel = n == 0 ? -1 : std::clamp(chosen, 0, n - 1);
    // Keep the selection on screen: a window of max_rows around it.
    const int top = std::clamp(sel - st.max_rows + 3, 0, std::max(0, n - st.max_rows));
    std::vector<Node> rows;
    for (int k = top; k < std::min(n, top + st.max_rows); ++k) {
        const PickerItem& it = items[static_cast<std::size_t>(ranked.order[static_cast<std::size_t>(k)])];
        const auto pid = part_id(id, static_cast<std::size_t>(k));
        const bool on = k == sel, hov = c.hovered(pid);
        rows.push_back((h(node(it.icon_name.empty() ? Node{} : file_icon(t, it.icon_name, 12.0f).build()),
                          node(fuzzy_text(it.label, ranked.matches[static_cast<std::size_t>(k)].positions, t.font_size,
                                          t.text_primary, t.accent).build()),
                          text_owned(it.detail) | font(t.font_size * 0.88f) | fg(var::text_disabled) | ellipsis
                              | grow() | min_size(0, 0),
                          node(it.hint.empty() ? Node{} : key_hint(t, it.hint, "", t.font_size * 0.82f).build()))
                        | gap(t.spaced(9)) | align(Align::center) | pad(0, t.spaced(12)) | height(30) | width(pct(100))
                        | bg(on ? t.tint(t.accent, 0.20f) : hov ? t.surface_raised : t.overlay.fade(0.0f))
                        | radius(t.radius_small)
                        | pointer_cursor | role(a11y::Role::menu_item) | selected(on) | label(it.label)
                        | id_of(id, static_cast<std::size_t>(k))).build());
    }
    if (rows.empty())
        rows.push_back((h(text_owned(st.empty_text) | font(t.font_size) | fg(var::text_disabled)) | pad(t.spaced(14), t.spaced(12))).build());

    std::vector<Node> foot;
    for (const auto& [k, a] : st.footer) foot.push_back(key_hint(t, k, a, t.font_size * 0.8f).build());
    foot.push_back((spacer()).build());
    foot.push_back((text_owned(std::to_string(n) + (n == static_cast<int>(items.size()) ? "" : " of " + std::to_string(items.size())))
                    | font(t.font_size * 0.8f) | fg(var::text_disabled)).build());

    const float qsize = t.font_size * 1.08f;
    c.overlay(Overlay{
        .content = (v(h(text_owned(st.prefix) | font(qsize) | fg(var::accent),
                        node((text_field(t, query, true, part_id(id, part_inc), st.placeholder, qsize)
                              | grow() | border(0, t.border) | bg(t.overlay.fade(0.0f))).build()))
                        | gap(t.spaced(4)) | align(Align::center) | pad(t.spaced(6), t.spaced(10)) | width(pct(100)),
                      box() | height(1) | width(pct(100)) | bg(var::border),
                      v(v_list(std::move(rows), 1.0f)) | pad(t.spaced(6)) | width(pct(100)),
                      h(h_list(std::move(foot), 12.0f)) | align(Align::center) | pad(t.spaced(6), t.spaced(14)) | width(pct(100))
                          | bg(var::surface) )
                    | width(st.width) | bg(var::overlay) | border(t.stroke(1), t.border_strong)
                    | radius(t.radius_medium) | clip | elevation(t.lift(24))
                    | role(a11y::Role::dialog) | label(id)).build(),
        .id = part_id(id, part_menu),
        .anchor_id = 0,
        .placement = Placement::cursor,
        .dismiss = Dismiss::on_click_outside,
        .layer = 10,
        .at = [&] {
            Vec2 sz{1200.0f, 800.0f};
            if constexpr (requires { c.c.size; }) sz = c.c.size;
            else if constexpr (requires { c.size; }) sz = c.size;
            return Vec2{std::max(0.0f, (sz.x - st.width) * 0.5f), std::round(sz.y * 0.10f)};
        }(),
    });
}

}  // namespace mayag::dsl
