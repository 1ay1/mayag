#pragma once
// mayag::widget select — a rich, searchable dropdown
//
//   trigger:  ┌──────────────────────────────┐
//             │ ▪▪▪▪▪▪▪▪  Catppuccin Mocha   ▾ │
//             └──────────────────────────────┘
//   menu:     ┌──────────────────────────────┐
//             │ ⌕ moch█                 3/913 │
//             ├──────────────────────────────┤
//             │ ✓ Catppuccin Mocha  ▪▪▪▪▪▪▪▪ │  ← hot (keyboard), live preview
//             │   Mocha Dark        ▪▪▪▪▪▪▪▪ │
//             └──────────────────────────────┘
//
// Options carry an optional preview: colour swatches (themes) and/or a font
// family the label is set in (typefaces). The menu is virtualised, so 900
// options cost as much as ten. Pure view: the caller owns the open state,
// the query, the hot row and the scroll, and gets messages back.
//
//   select_trigger(c, "theme", opts[cur], open, 260) | on_click(...)
//   select_menu(c, "theme", opts, filtered, cur, hot, query, scroll, 320, pick, scrolled)

#include "control.hpp"
#include "knob.hpp"
#include "virtual_list.hpp"
#include "../app/handlers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

struct SelectOption {
    std::string                   label;
    std::array<std::uint32_t, 8>  swatches{};    ///< 0xRRGGBB; all-zero = none
    bool                          has_swatches = false;
    std::optional<FontFamily>     family;        ///< render the label in this face
};

inline constexpr float kSelectRowH = 30.0f;
inline constexpr int   kSelectVisible = 9;

/// Indices of options matching `query` (every word, case-insensitive).
[[nodiscard]] inline std::vector<int> select_filter(const std::vector<SelectOption>& opts, std::string_view query) {
    std::vector<int> out;
    std::string q;
    for (char ch : query) q.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    for (std::size_t i = 0; i < opts.size(); ++i) {
        if (!q.empty()) {
            std::string l;
            for (char ch : opts[i].label) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            bool ok = true;
            for (std::size_t p = 0; p < q.size() && ok;) {
                const std::size_t e = std::min(q.find(' ', p), q.size());
                if (e > p && l.find(q.substr(p, e - p)) == std::string::npos) ok = false;
                p = e + 1;
            }
            if (!ok) continue;
        }
        out.push_back(static_cast<int>(i));
    }
    return out;
}

namespace detail {
[[nodiscard]] inline Color<Srgb> rgb_u32(std::uint32_t v) {
    return rgb8(static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v));
}
/// A scheme preview: its six colours as dots, on a pill of its own paper.
template <ControlContext C>
[[nodiscard]] inline Node swatch_strip(const C& c, const SelectOption& o, float chip) {
    const Theme& t = c.theme;
    std::vector<Node> dots;
    for (std::size_t k = 2; k < o.swatches.size(); ++k)
        dots.push_back((box() | size(chip, chip) | radius(chip * 0.5f) | bg(rgb_u32(o.swatches[k]))).build());
    return (h(std::move(dots)) | gap(3.0f) | align(Align::center) | height(chip + 8.0f) | pad(0, 5.0f)
            | radius((chip + 8.0f) * 0.5f) | bg(rgb_u32(o.swatches[0])) | border(t.stroke(1), t.border)).build();
}
template <ControlContext C>
[[nodiscard]] inline auto option_label(const C&, const SelectOption& o, float size, Color<Srgb> col) {
    auto txt = text_owned(o.label) | font(size) | fg(col) | ellipsis | grow() | min_size(0, 0);
    return o.family ? std::move(txt) | family(*o.family) : std::move(txt) | family(FontFamily::mono);
}
}  // namespace detail

/// The closed control. Clicked on node_id(id) is the caller's "toggle open".
template <ControlContext C>
[[nodiscard]] inline Node select_trigger(const C& c, std::string_view id, const SelectOption& current, bool open,
                                         float width_px) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool hov = c.hovered(nid);
    std::vector<Node> parts;
    if (current.has_swatches) parts.push_back(detail::swatch_strip(c, current, 9.0f));
    parts.push_back(detail::option_label(c, current, t.type(Tier::body), t.text_primary).build());
    parts.push_back((h(text_owned(std::string{ui_glyph(t, open ? "\uF077" : "\uF078", open ? "\u02C4" : "\u02C5")})
                         | font(t.type(Tier::fine)) | fg(open || hov ? t.accent : t.text_secondary))
                     | size(16.0f, 32.0f) | center).build());
    return (h(std::move(parts)) | gap(t.spaced(8)) | align(Align::center) | width(width_px) | height(32.0f)
            | pad(0, t.spaced(8), 0, t.spaced(10)) | radius(t.radius_small)
            | bg(open ? mix(t.background, t.accent, 0.06f) : hov ? mix(t.background, t.text_primary, 0.04f) : t.background)
            | border(t.stroke(1), open ? t.accent.fade(0.85f) : hov ? t.border_strong : t.border)
            | pointer_cursor | role(a11y::Role::button) | label(id) | id_of(id)).build();
}

/// The open list, for an Overlay. `filtered` = select_filter(opts, query);
/// `hot` indexes into it. `pick(i)` (an option index) and `wheel(Vec2)`
/// return the caller's messages.
template <ControlContext C, class Pick, class Wheel>
[[nodiscard]] inline Node select_menu(const C& c, std::string_view id, const std::vector<SelectOption>& opts,
                                      const std::vector<int>& filtered, int current, int hot, std::string_view query,
                                      const ScrollState& sc, float width_px, Pick pick, Wheel wheel) {
    const Theme& t = c.theme;
    const std::string base{id};
    const float fs = t.type(Tier::body);

    auto search = h(text_owned(std::string{ui_glyph(t, "\uF002", "/")}) | font(t.type(Tier::small)) | fg(query.empty() ? t.text_disabled : t.accent),
                    text_owned(query.empty() ? std::string{"type to filter\u2026"} : std::string{query} + "\u258F")
                        | font(fs) | family(FontFamily::mono) | fg(query.empty() ? t.text_disabled : t.text_primary)
                        | grow() | ellipsis | min_size(0, 0),
                    text_owned(std::to_string(filtered.size()) + "/" + std::to_string(opts.size())) | font(t.type(Tier::small))
                        | family(FontFamily::mono) | fg(t.text_disabled))
                | gap(t.spaced(8)) | align(Align::center) | height(36.0f) | pad(0, t.spaced(12)) | width(pct(100))
                | inner_shadow(0.0f, t.border, {0.0f, -1.0f});

    const int n = static_cast<int>(filtered.size());
    const float list_h = kSelectRowH * static_cast<float>(std::clamp(n, 1, kSelectVisible));
    auto rows = virtual_list(sc, n, kSelectRowH, [&](int k) -> Node {
        const int oi = filtered[static_cast<std::size_t>(k)];
        const SelectOption& o = opts[static_cast<std::size_t>(oi)];
        const auto pid = part_id(base, static_cast<std::size_t>(oi));
        const bool is_hot = k == hot, cur = oi == current, hov = c.hovered(pid);
        std::vector<Node> parts;
        parts.push_back((text_owned(cur ? std::string{ui_glyph(t, "\uF00C", "\u2713")} : std::string{}) | font(t.type(Tier::fine))
                         | fg(t.accent) | width(14.0f)).build());
        parts.push_back(detail::option_label(c, o, fs, cur || is_hot ? t.text_primary : t.text_secondary).build());
        if (o.has_swatches) parts.push_back(detail::swatch_strip(c, o, 8.0f));
        return (h(std::move(parts)) | gap(t.spaced(8)) | align(Align::center) | height(kSelectRowH)
                | pad(0, t.spaced(10)) | width(pct(100)) | radius(t.radius_small - 2.0f)
                | bg(is_hot ? t.accent.fade(0.16f) : hov ? t.text_primary.fade(0.05f) : t.overlay.fade(0.0f))
                | border(t.stroke(1), is_hot ? t.accent.fade(0.35f) : t.overlay.fade(0.0f))
                | pointer_cursor | role(a11y::Role::menu_item) | label(o.label) | id_of(base, static_cast<std::size_t>(oi))
                | on_click([m = pick(oi)] { return m; })).build();
    });
    Node list = (v(std::move(rows)) | width(pct(100)) | height(list_h) | grow(1.0f, 1.0f) | min_size(0, kSelectRowH) | scroll(sc) | clip
                 | on_scroll([wheel](const Scrolled& s) { return wheel(s.delta); })).build();
    if (n == 0)
        list = (h(text_owned("nothing matches") | font(fs) | fg(t.text_disabled)) | height(kSelectRowH * 2.0f)
                | width(pct(100)) | center).build();

    auto hints = h(text_owned("\u2191\u2193 preview  \u00B7  enter keep  \u00B7  esc revert") | font(t.type(Tier::fine)) | family(FontFamily::mono)
                       | fg(t.text_disabled))
               | height(26.0f) | pad(0, t.spaced(12)) | align(Align::center) | width(pct(100))
               | inner_shadow(0.0f, t.border, {0.0f, 1.0f});

    return (v(std::move(search), v(std::move(list)) | pad(t.spaced(4)) | width(pct(100)) | grow(1.0f, 1.0f) | min_size(0, 0),
              std::move(hints))
            | width(width_px) | bg(t.overlay) | border(t.stroke(1), t.border_strong) | radius(t.radius_medium) | clip
            | shadow(28.0f, rgba<0x00000080>, {0.0f, 10.0f}) | role(a11y::Role::menu) | label(id)).build();
}

/// Scroll a select menu so row `hot` is visible.
inline void select_reveal(ScrollState& s, int hot) {
    const float y = kSelectRowH * static_cast<float>(hot);
    const float view = kSelectRowH * static_cast<float>(kSelectVisible);
    if (y < s.offset.y) s.offset.y = y;
    else if (y + kSelectRowH > s.offset.y + view) s.offset.y = y + kSelectRowH - view;
}

}  // namespace mayag::dsl
