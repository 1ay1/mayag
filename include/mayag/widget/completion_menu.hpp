#pragma once
// mayag::widget completion_menu — code completion at the caret
//
//   completion_menu(c, "complete", items, ranked, selected, caret_pos)
//
// The popup a language server's completions go in: a kind badge per item
// (fn, var, type, kw, snippet ...), the label with the typed prefix
// highlighted, the signature/detail dimmed on the right, and a documentation
// pane beside the selected item. Posted as an overlay at the caret's window
// position (flips above when there is no room below, like every editor).
//
// Interaction:
//   Clicked on part_id(id, i) -> accept item i
//   keys (app): up/down move, tab/enter accept, esc dismiss

#include "control.hpp"
#include "fuzzy.hpp"
#include "../scene/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace mayag::dsl {

struct CompletionItem {
    enum class Kind : std::uint8_t { function, method, variable, field, type, keyword, snippet, module, constant, file };
    std::string label;
    Kind        kind = Kind::variable;
    std::string detail;   ///< signature / type
    std::string doc;      ///< documentation shown beside the list
    std::string insert;   ///< text to insert; label when empty
};

[[nodiscard]] inline std::pair<const char*, std::uint32_t> completion_badge(CompletionItem::Kind k) {
    using K = CompletionItem::Kind;
    switch (k) {
        case K::function: return {"fn", 0x82AAFF};  case K::method:   return {"m", 0x82AAFF};
        case K::variable: return {"v", 0xC8D3F5};   case K::field:    return {"f", 0x4FD6BE};
        case K::type:     return {"T", 0x65BCFF};   case K::keyword:  return {"kw", 0xC099FF};
        case K::snippet:  return {"{}", 0xC3E88D};  case K::module:   return {"mod", 0xFFC777};
        case K::constant: return {"c", 0xFF966C};   case K::file:     return {"\u2261", 0x9AA5CE};
    }
    return {"?", 0x9AA5CE};
}

template <ControlContext C>
inline void completion_menu(const C& c, std::string_view id, const std::vector<CompletionItem>& items,
                            const std::vector<int>& order, const std::vector<FuzzyMatch>& matches,
                            int chosen, Vec2 caret_bottom_left, float line_h, float px = 13.0f) {
    const Theme& t = c.theme;
    const int n = static_cast<int>(order.size());
    if (n == 0) return;
    const int sel = std::clamp(chosen, 0, n - 1);
    constexpr int kRows = 10;
    const int top = std::clamp(sel - kRows + 2, 0, std::max(0, n - kRows));
    std::vector<Node> rows;
    for (int k = top; k < std::min(n, top + kRows); ++k) {
        const CompletionItem& it = items[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])];
        const auto [mark, col] = completion_badge(it.kind);
        const auto badge_col = rgb8(static_cast<std::uint8_t>(col >> 16), static_cast<std::uint8_t>(col >> 8), static_cast<std::uint8_t>(col));
        const bool on = k == sel;
        rows.push_back((h(h(text_owned(mark) | font(px * 0.72f) | bold | fg(badge_col))
                              | size(26, 16) | center | radius(3) | bg(mix(t.overlay, badge_col, 0.14f)),
                          node(fuzzy_text(it.label, matches[static_cast<std::size_t>(k)].positions, px,
                                          t.text_primary, t.accent).build()),
                          text_owned(it.detail) | font(px * 0.86f) | fg(t.text_disabled) | ellipsis
                              | grow() | min_size(0, 0) | text_align(TextAlign::right))
                        | gap(8) | align(Align::center) | pad(0, 8) | height(std::round(px * 1.75f)) | width(pct(100))
                        | bg(on ? t.tint(t.accent, 0.22f) : t.overlay.fade(0.0f)) | radius(4)
                        | pointer_cursor | role(a11y::Role::menu_item) | selected(on) | label(it.label)
                        | id_of(id, static_cast<std::size_t>(k))).build());
    }
    const CompletionItem& cur = items[static_cast<std::size_t>(order[static_cast<std::size_t>(sel)])];
    Node docs = cur.doc.empty() && cur.detail.empty() ? Node{}
        : (v(text_owned(cur.detail) | font(px * 0.9f) | fg(t.accent) | wrap_text | no_ligatures,
             text_owned(cur.doc) | font(px * 0.9f) | fg(t.text_secondary) | wrap_text | line_height(1.45f))
           | gap(8) | pad(10, 12) | width(300)
           | bg(t.overlay) | border(1, t.border_strong) | radius(t.radius_small) | elevation(14)).build();
    const bool scrolled = n > kRows;
    c.overlay(Overlay{
        .content = (h(v(v_list(std::move(rows), 1.0f))
                          | pad(4) | width(420)
                          | bg(t.overlay) | border(1, t.border_strong) | radius(t.radius_small) | elevation(14),
                      node(std::move(docs)))
                    | gap(4) | align(Align::start)
                    | role(a11y::Role::menu) | label(scrolled ? "completions (more)" : "completions")).build(),
        .id = part_id(id, part_menu),
        .anchor_id = 0,
        .placement = Placement::cursor,
        .dismiss = Dismiss::never,          // typing continues in the editor underneath
        .layer = 8,
        .at = Vec2{caret_bottom_left.x - 34.0f, caret_bottom_left.y + 2.0f},
    });
    (void)line_h;
}

}  // namespace mayag::dsl
