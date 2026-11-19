#pragma once
// mayag::widget settings_panel — settings that read like a config file in a terminal
//
//   ┌────────────┬──────────────────────────────────────────────────────────┐
//   │ ❯_ hand    │ ❯ /font▌                       3 matches  2 modified [R] │
//   │            ├──────────────────────────────────────────────────────────┤
//   │ ▸ A font +2│ font {                                  # 3 keys, 2 mod. │
//   │    colors  │ ▌size* ·········· ▌▌▌▌▌▌█▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌   15.0pt  × │
//   │    cursor  │  line_height ···· ▌▌▌▌▌█▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌    1.20×    │
//   │    …       │  family ········· [ JetBrains Mono               ˅ ]     │
//   │            │ }                                                        │
//   ├────────────┴──────────────────────────────────────────────────────────┤
//   │ NORMAL  font.size  text size in points   j/k move  h/l adjust  / q   │
//   └───────────────────────────────────────────────────────────────────────┘
//
// Everything is monospace. Values are syntax-coloured from the theme's
// status roles (numbers warning, booleans success, the cursor accent), so a
// terminal that maps them to its ANSI colours gets a panel in its scheme.
// Controls: bar_knob (numbers), tui_check (bools), tui_choice (few
// options), select (many options, searchable, swatch / typeface previews,
// live keyboard preview).
//
// Data in, ops out. The app describes every setting as a SettingSpec
// (generated from its own table) and owns one SettingsPanelState. Every
// interaction -- mouse or keyboard -- is a SettingsOp the app runs on that
// state; value changes come back out in `state.out` as SettingEdits for the
// app to apply. Controls: range_knob (numbers), switch (bools), segmented
// (few choices), select (many choices, searchable, with swatch / typeface
// previews and live keyboard preview).
//
//   // view
//   settings_panel(c, "settings", specs, m.panel, props, [](SettingsOp op) { return Panel{op}; });
//   // update
//   op(m.panel); for (auto& e : std::exchange(m.panel.out, {})) apply(m.cfg, e);
//   // input
//   if (auto op = settings_panel_key(m.panel, specs, key, mods)) return Panel{*op};
//   if (auto op = settings_panel_text(m.panel, specs, text))     return Panel{*op};
//   if (auto op = settings_panel_left(m.panel, "settings", node)) return Panel{*op};

#include "control.hpp"
#include "key_hint.hpp"
#include "knob.hpp"
#include "scroll_view.hpp"
#include "segmented.hpp"
#include "select.hpp"
#include "toggle.hpp"
#include "../app/handlers.hpp"
#include "../scene/overlay.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

struct SettingSpec {
    enum class Kind : std::uint8_t {
        number,   ///< range_knob over [lo, hi], snapped to `step`
        toggle,   ///< switch
        choice,   ///< a few options: segmented
        cycle,    ///< many options: a searchable select
    };
    std::string section;            ///< group ("font")
    std::string section_icon;       ///< a glyph for the group (Nerd Font / any)
    std::string key;                ///< config key ("line_height"); unique within its section
    std::string label;              ///< display title; empty: derived from key ("Line height")
    std::string hint;
    Kind        kind = Kind::number;
    float       value = 0.0f, lo = 0.0f, hi = 1.0f, step = 0.0f;   ///< also toggle 0/1, option index
    int         decimals = 1;
    std::string unit;               ///< "pt", "%", "×", "ms"
    float       scale = 1.0f;       ///< readout = value * scale
    std::vector<std::string> options;                          ///< choice / cycle
    std::vector<std::array<std::uint32_t, 8>> option_swatches; ///< cycle previews (paper, ink, 6 colours)
    std::vector<FontFamily> option_families;                   ///< cycle: set each label in its face
    bool        modified = false;
};

struct SettingEdit {
    enum class Op : std::uint8_t { set, step, reset, reset_all };
    Op          op = Op::set;
    std::size_t index = 0;
    float       value = 0.0f;
    int         delta = 0;
};

/// Everything the panel remembers; the app owns one.
struct SettingsPanelState {
    int         selected = 0;
    std::string query;
    bool        filtering = false;
    ScrollState scroll{};
    // the open select (-1 = none)
    int         menu = -1;
    std::string menu_query;
    int         menu_hot = 0;
    ScrollState menu_scroll{};
    float       menu_origin = 0.0f;
    // outputs, drained by the app after running an op
    std::vector<SettingEdit> out;
    bool        close = false;
};

/// One interaction, as a value: an owning, Sendable edit of the panel state
/// (mayag::Edit). The app runs it: `op(m.panel)`.
using SettingsOp = Edit<SettingsPanelState>;

struct SettingsPanelProps {
    std::string title = "settings";
    std::string subtitle;           ///< under the title in the sidebar
    std::string brand = "\u276F_";  ///< the logo tile's glyphs
    std::optional<Node> logo;       ///< a drawn logo instead of the glyph tile (any size)
    std::string footer;             ///< e.g. the config file path
    float       width = 900.0f, height = 620.0f;
};

// ── geometry (fixed, so selection can be scrolled into view exactly) ──────

inline constexpr float kSetRowH = 46.0f, kSetHeadH = 34.0f, kSetCloseH = 2.0f, kSetGap = 16.0f, kSetCardPad = 0.0f,
                       kSetTop = 12.0f;
inline constexpr float kSetSidebarW = 180.0f, kSetCtlW = 248.0f, kSetValueW = 58.0f;
inline constexpr float kSetKnobW = kSetCtlW - kSetValueW - 10.0f, kSetSelectW = kSetCtlW;
inline constexpr std::size_t kSetMenuPart = 0x7FFF0000u;

// ── pure helpers ───────────────────────────────────────────────────────────

[[nodiscard]] inline std::string setting_title(const SettingSpec& s) {
    if (!s.label.empty()) return s.label;
    std::string t = s.key;
    for (char& ch : t) if (ch == '_') ch = ' ';
    if (!t.empty()) t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    return t;
}

[[nodiscard]] inline bool setting_matches(const SettingSpec& s, std::string_view query) {
    if (query.empty()) return true;
    const auto low = [](std::string_view v) {
        std::string o;
        for (char ch : v) o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        return o;
    };
    const std::string hay = low(s.section) + "." + low(s.key) + " " + low(setting_title(s)) + " " + low(s.hint);
    const std::string q = low(query);
    for (std::size_t p = 0; p < q.size();) {
        const std::size_t e = std::min(q.find(' ', p), q.size());
        if (e > p && hay.find(q.substr(p, e - p)) == std::string::npos) return false;
        p = e + 1;
    }
    return true;
}

[[nodiscard]] inline std::vector<std::size_t> settings_visible(const std::vector<SettingSpec>& specs, std::string_view q) {
    std::vector<std::size_t> v;
    for (std::size_t i = 0; i < specs.size(); ++i) if (setting_matches(specs[i], q)) v.push_back(i);
    return v;
}

/// Top of row `index` inside the scrolling content.
[[nodiscard]] inline float settings_row_y(const std::vector<SettingSpec>& specs, const std::vector<std::size_t>& vis,
                                          std::size_t index) {
    float y = kSetTop;
    std::size_t k = 0;
    while (k < vis.size()) {
        const std::string& sec = specs[vis[k]].section;
        y += kSetHeadH;
        std::size_t r = 0;
        for (; k < vis.size() && specs[vis[k]].section == sec; ++k, ++r)
            if (vis[k] == index) return y + kSetRowH * static_cast<float>(r);
        y += kSetRowH * static_cast<float>(r) + kSetCloseH + kSetGap;
    }
    return y;
}

inline void settings_reveal(SettingsPanelState& st, const std::vector<SettingSpec>& specs) {
    const auto vis = settings_visible(specs, st.query);
    const float y = settings_row_y(specs, vis, static_cast<std::size_t>(std::max(st.selected, 0)));
    const float view = st.scroll.viewport.y > 0.0f ? st.scroll.viewport.y : 420.0f;
    const bool first_in_section = [&] {
        for (std::size_t k = 0; k < vis.size(); ++k)
            if (vis[k] == static_cast<std::size_t>(st.selected))
                return k == 0 || specs[vis[k - 1]].section != specs[vis[k]].section;
        return false;
    }();
    const float top = first_in_section ? y - kSetHeadH - kSetCardPad : y - 8.0f;   // keep the section title in view
    if (top < st.scroll.offset.y) st.scroll.offset.y = std::max(0.0f, top);
    else if (y + kSetRowH + 8.0f > st.scroll.offset.y + view) st.scroll.offset.y = y + kSetRowH + 8.0f - view;
}

[[nodiscard]] inline float setting_stepped(const SettingSpec& s, int delta) {
    switch (s.kind) {
        case SettingSpec::Kind::number: {
            const float st = s.step > 0.0f ? s.step : (s.hi - s.lo) / 50.0f;
            return std::clamp(s.value + st * static_cast<float>(delta), s.lo, s.hi);
        }
        case SettingSpec::Kind::toggle: return s.value > 0.5f ? 0.0f : 1.0f;
        default: {
            const int n = std::max(static_cast<int>(s.options.size()), 1);
            return static_cast<float>(((static_cast<int>(s.value) + delta) % n + n) % n);
        }
    }
}

[[nodiscard]] inline float setting_from_fraction(const SettingSpec& s, float f) {
    float v = s.lo + std::clamp(f, 0.0f, 1.0f) * (s.hi - s.lo);
    if (s.step > 0.0f) v = s.lo + std::round((v - s.lo) / s.step) * s.step;
    return std::clamp(v, s.lo, s.hi);
}

[[nodiscard]] inline std::string setting_readout(const SettingSpec& s) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f%s", std::max(s.decimals, 0), static_cast<double>(s.value * s.scale), s.unit.c_str());
    return buf;
}

[[nodiscard]] inline std::vector<SelectOption> setting_select_options(const SettingSpec& s) {
    std::vector<SelectOption> o(s.options.size());
    for (std::size_t i = 0; i < o.size(); ++i) {
        o[i].label = s.options[i];
        if (i < s.option_swatches.size()) { o[i].swatches = s.option_swatches[i]; o[i].has_swatches = true; }
        if (i < s.option_families.size()) o[i].family = s.option_families[i];
    }
    return o;
}

// ── ops (what interactions do to the state) ────────────────────────────────

namespace settings_ops {
using Specs = std::shared_ptr<const std::vector<SettingSpec>>;
using E = SettingEdit;

inline void select(SettingsPanelState& st, const std::vector<SettingSpec>& specs, std::size_t i) {
    st.selected = static_cast<int>(i);
    settings_reveal(st, specs);
}
inline void open_menu(SettingsPanelState& st, const std::vector<SettingSpec>& specs, std::size_t i) {
    st.selected = static_cast<int>(i);
    st.menu = static_cast<int>(i);
    st.menu_query.clear();
    st.menu_origin = specs[i].value;
    st.menu_scroll = {};
    st.menu_hot = std::clamp(static_cast<int>(specs[i].value), 0, std::max(static_cast<int>(specs[i].options.size()) - 1, 0));
    st.menu_scroll.offset.y = std::max(0.0f, kSelectRowH * static_cast<float>(st.menu_hot - kSelectVisible / 2));
}
inline void close_menu(SettingsPanelState& st, bool revert) {
    if (st.menu >= 0 && revert) st.out.push_back({E::Op::set, static_cast<std::size_t>(st.menu), st.menu_origin, 0});
    st.menu = -1;
    st.menu_query.clear();
}
inline std::vector<int> menu_filtered(const SettingsPanelState& st, const std::vector<SettingSpec>& specs) {
    if (st.menu < 0 || static_cast<std::size_t>(st.menu) >= specs.size()) return {};
    return select_filter(setting_select_options(specs[static_cast<std::size_t>(st.menu)]), st.menu_query);
}
inline void menu_move(SettingsPanelState& st, const std::vector<SettingSpec>& specs, int d, bool preview) {
    const auto f = menu_filtered(st, specs);
    if (f.empty()) return;
    st.menu_hot = std::clamp(st.menu_hot + d, 0, static_cast<int>(f.size()) - 1);
    select_reveal(st.menu_scroll, st.menu_hot);
    if (preview)   // browsing IS trying: the app applies it live
        st.out.push_back({E::Op::set, static_cast<std::size_t>(st.menu), static_cast<float>(f[static_cast<std::size_t>(st.menu_hot)]), 0});
}
inline void jump_section(SettingsPanelState& st, const std::vector<SettingSpec>& specs, int d) {
    const auto vis = settings_visible(specs, st.query);
    if (vis.empty()) return;
    std::vector<std::size_t> firsts;
    for (std::size_t k = 0; k < vis.size(); ++k)
        if (k == 0 || specs[vis[k]].section != specs[vis[k - 1]].section) firsts.push_back(vis[k]);
    long at = 0;
    for (std::size_t k = 0; k < firsts.size(); ++k)
        if (static_cast<std::size_t>(std::max(st.selected, 0)) >= firsts[k]) at = static_cast<long>(k);
    const long n = static_cast<long>(firsts.size());
    select(st, specs, firsts[static_cast<std::size_t>(((at + d) % n + n) % n)]);
}
inline void ensure_visible_selection(SettingsPanelState& st, const std::vector<SettingSpec>& specs) {
    const auto vis = settings_visible(specs, st.query);
    if (vis.empty()) return;
    if (std::find(vis.begin(), vis.end(), static_cast<std::size_t>(std::max(st.selected, 0))) == vis.end())
        st.selected = static_cast<int>(vis.front());
    st.scroll.offset.y = 0.0f;
}
}  // namespace settings_ops

/// Keys. nullopt: not the panel's (let the app have it).
[[nodiscard]] inline std::optional<SettingsOp> settings_panel_key(const SettingsPanelState& st,
                                                                 const std::vector<SettingSpec>& specs, Key k, Mods m) {
    namespace o = settings_ops;
    using E = SettingEdit;
    auto sp = std::make_shared<const std::vector<SettingSpec>>(specs);
    if (st.menu >= 0) {
        switch (k) {
            case Key::escape: return SettingsOp{[](SettingsPanelState& s) { o::close_menu(s, true); }};
            case Key::enter: case Key::tab:
                return SettingsOp{[sp](SettingsPanelState& s) {
                    const auto f = o::menu_filtered(s, *sp);
                    if (!f.empty())
                        s.out.push_back({E::Op::set, static_cast<std::size_t>(s.menu),
                                         static_cast<float>(f[static_cast<std::size_t>(std::clamp(s.menu_hot, 0, static_cast<int>(f.size()) - 1))]), 0});
                    o::close_menu(s, false);
                }};
            case Key::up: case Key::down: case Key::page_up: case Key::page_down: case Key::home: case Key::end: {
                const int d = k == Key::up ? -1 : k == Key::down ? 1 : k == Key::page_up ? -kSelectVisible
                            : k == Key::page_down ? kSelectVisible : k == Key::home ? -100000 : 100000;
                return SettingsOp{[sp, d](SettingsPanelState& s) { o::menu_move(s, *sp, d, true); }};
            }
            case Key::backspace:
                return SettingsOp{[](SettingsPanelState& s) {
                    if (!s.menu_query.empty()) s.menu_query.pop_back();
                    s.menu_hot = 0; s.menu_scroll.offset.y = 0.0f;
                }};
            default: return SettingsOp{[](SettingsPanelState&) {}};   // the menu has the keyboard
        }
    }
    if (st.filtering) {
        if (k == Key::escape) return SettingsOp{[](SettingsPanelState& s) { s.filtering = false; s.query.clear(); }};
        if (k == Key::enter) return SettingsOp{[](SettingsPanelState& s) { s.filtering = false; }};
        if (k == Key::backspace)
            return SettingsOp{[sp](SettingsPanelState& s) {
                if (!s.query.empty()) s.query.pop_back();
                o::ensure_visible_selection(s, *sp);
            }};
    }
    const auto vis = settings_visible(specs, st.query);
    const auto at_it = std::find(vis.begin(), vis.end(), static_cast<std::size_t>(std::max(st.selected, 0)));
    const long at = at_it == vis.end() ? -1 : static_cast<long>(at_it - vis.begin());
    const auto move = [&](long d) -> std::optional<SettingsOp> {
        if (vis.empty()) return std::nullopt;
        const long n = static_cast<long>(vis.size());
        const std::size_t to = vis[static_cast<std::size_t>(at < 0 ? 0 : std::clamp(at + d, 0L, n - 1))];
        return SettingsOp{[sp, to](SettingsPanelState& s) { o::select(s, *sp, to); }};
    };
    switch (k) {
        case Key::escape:
            return SettingsOp{[](SettingsPanelState& s) {
                if (!s.query.empty()) s.query.clear();
                else s.close = true;
            }};
        case Key::down: return move(1);
        case Key::up: return move(-1);
        case Key::page_down: return move(6);
        case Key::page_up: return move(-6);
        case Key::home: return move(-100000);
        case Key::end: return move(100000);
        case Key::tab: return SettingsOp{[sp, d = m.shift ? -1 : 1](SettingsPanelState& s) { o::jump_section(s, *sp, d); }};
        default: break;
    }
    if (at < 0) return std::nullopt;
    const std::size_t i = vis[static_cast<std::size_t>(at)];
    const SettingSpec& s = specs[i];
    const int mult = m.shift ? 10 : 1;
    switch (k) {
        case Key::right: return SettingsOp{[i, mult](SettingsPanelState& x) { x.out.push_back({E::Op::step, i, 0.0f, mult}); }};
        case Key::left: return SettingsOp{[i, mult](SettingsPanelState& x) { x.out.push_back({E::Op::step, i, 0.0f, -mult}); }};
        case Key::enter: case Key::space:
            if (s.kind == SettingSpec::Kind::cycle) return SettingsOp{[sp, i](SettingsPanelState& x) { o::open_menu(x, *sp, i); }};
            if (s.kind == SettingSpec::Kind::number) return std::nullopt;
            return SettingsOp{[i](SettingsPanelState& x) { x.out.push_back({E::Op::step, i, 0.0f, 1}); }};
        case Key::backspace: case Key::del:
            return SettingsOp{[i](SettingsPanelState& x) { x.out.push_back({E::Op::reset, i, 0.0f, 0}); }};
        default: return std::nullopt;
    }
}

/// Typed text: the filter, the menu's filter, and vi keys.
[[nodiscard]] inline std::optional<SettingsOp> settings_panel_text(const SettingsPanelState& st,
                                                                  const std::vector<SettingSpec>& specs, std::string_view text) {
    namespace o = settings_ops;
    auto sp = std::make_shared<const std::vector<SettingSpec>>(specs);
    if (st.menu >= 0)
        return SettingsOp{[t = std::string{text}](SettingsPanelState& s) {
            s.menu_query += t; s.menu_hot = 0; s.menu_scroll.offset.y = 0.0f;
        }};
    if (st.filtering)
        return SettingsOp{[sp, t = std::string{text}](SettingsPanelState& s) { s.query += t; o::ensure_visible_selection(s, *sp); }};
    const std::string_view t = text;
    if (t == "/") return SettingsOp{[](SettingsPanelState& s) { s.filtering = true; }};
    if (t == "q") return SettingsOp{[](SettingsPanelState& s) { s.close = true; }};
    if (t == "r" && st.selected >= 0)
        return SettingsOp{[i = static_cast<std::size_t>(st.selected)](SettingsPanelState& s) {
            s.out.push_back({SettingEdit::Op::reset, i, 0.0f, 0});
        }};
    if (t == "R") return SettingsOp{[](SettingsPanelState& s) { s.out.push_back({SettingEdit::Op::reset_all, 0, 0.0f, 0}); }};
    const Key k = t == "j" ? Key::down : t == "k" ? Key::up : (t == "h" || t == "H") ? Key::left
                : (t == "l" || t == "L") ? Key::right : t == "g" ? Key::home : t == "G" ? Key::end : Key::unknown;
    if (k != Key::unknown) return settings_panel_key(st, specs, k, Mods{.shift = t == "H" || t == "L"});
    return std::nullopt;
}

/// A click outside the open select (an overlay dismissal, delivered as Left).
[[nodiscard]] inline std::optional<SettingsOp> settings_panel_left(const SettingsPanelState& st, std::string_view id,
                                                                  std::uint64_t node) {
    if (st.menu < 0 || node != part_id(id, kSetMenuPart)) return std::nullopt;
    return SettingsOp{[](SettingsPanelState& s) { settings_ops::close_menu(s, false); }};
}

// ── the view ───────────────────────────────────────────────────────────────
//
// Zed's shape, tightened: a sidebar of sections, a search field, each
// section a rounded card of rows. A row is title + hint on the left and ONE
// fixed control column on the right: wide controls (slider + value, select,
// choices) fill it, a switch sits at its right edge -- every left and right
// edge lines up down the whole panel.

template <ControlContext C, class On>
    requires std::invocable<const On&, SettingsOp>
[[nodiscard]] inline Node settings_panel(const C& c, std::string_view id, const std::vector<SettingSpec>& specs,
                                         const SettingsPanelState& st, const SettingsPanelProps& p, On on) {
    namespace o = settings_ops;
    using E = SettingEdit;
    const Theme& t = c.theme;
    const float fs = t.font_size;
    const std::string base{id};
    const auto vis = settings_visible(specs, st.query);
    auto sp = std::make_shared<const std::vector<SettingSpec>>(specs);
    const auto emit = [](std::size_t i, E::Op op, float v = 0.0f, int d = 0) {
        return [i, op, v, d](SettingsPanelState& s) { s.selected = static_cast<int>(i); s.out.push_back({op, i, v, d}); };
    };
    const bool has_sel = st.selected >= 0 && static_cast<std::size_t>(st.selected) < specs.size();
    const std::string selected_section = has_sel ? specs[static_cast<std::size_t>(st.selected)].section : "";
    std::size_t modified = 0;
    for (const auto& s : specs) modified += s.modified ? 1 : 0;
    const auto line = mix(t.background, t.text_primary, 0.09f);      // hairlines
    const auto card = mix(t.background, t.text_primary, 0.025f);     // section cards
    const auto muted = mix(t.text_primary, t.background, 0.45f);
    const auto cap = [](std::string s) {
        if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        return s;
    };

    // ── sidebar ──────────────────────────────────────────────────────────
    std::vector<Node> nav;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (i > 0 && specs[i].section == specs[i - 1].section) continue;
        const std::string& sec = specs[i].section;
        std::size_t mods = 0;
        for (std::size_t k = i; k < specs.size() && specs[k].section == sec; ++k) mods += specs[k].modified ? 1 : 0;
        const bool on_ = sec == selected_section;
        const bool hov = c.hovered(part_id(base + ".nav", i));
        nav.push_back((h(text_owned(specs[i].section_icon) | font(fs * 0.95f) | width(18.0f) | text_center
                             | fg(on_ ? t.accent : muted),
                         text_owned(cap(sec)) | font(fs) | grow() | fg(on_ || hov ? t.text_primary : t.text_secondary),
                         text_owned(mods ? std::to_string(mods) : std::string{}) | font(fs * 0.78f) | fg(t.accent))
                       | gap(10.0f) | align(Align::center) | height(30.0f) | pad(0, 10.0f) | radius(t.rounded(5.0f))
                       | bg(on_ ? mix(t.background, t.text_primary, 0.08f) : hov ? mix(t.background, t.text_primary, 0.04f)
                                : t.background.fade(0.0f))
                       | pointer_cursor | role(a11y::Role::tab) | selected(on_) | label(sec) | id_of(base + ".nav", i)
                       | on_click([on, sp, i] {
                             return on(SettingsOp{[sp, i](SettingsPanelState& s) {
                                 s.query.clear(); s.filtering = false;
                                 o::select(s, *sp, i);
                                 s.scroll.offset.y = std::max(0.0f, settings_row_y(*sp, settings_visible(*sp, ""), i) - kSetHeadH - kSetTop);
                             }});
                         })).build());
    }
    Node mark = p.logo ? *p.logo
                       : (h(text_owned(p.brand) | font(fs * 0.95f) | semibold | fg(t.on_accent))
                          | size(26.0f, 26.0f) | center | radius(t.rounded(6.0f)) | bg(t.accent)).build();
    auto brand = h(node(std::move(mark)),
                   v(text_owned(p.title) | font(fs * (p.logo ? 1.35f : 1.05f)) | semibold | fg(t.text_primary),
                     text_owned(p.subtitle) | font(fs * 0.8f) | fg(muted))
                       | gap(1.0f))
               | gap(12.0f) | align(Align::center) | height(p.logo ? 76.0f : 52.0f) | pad(0, 14.0f);
    auto sidebar = v(std::move(brand), v(std::move(nav)) | gap(1.0f) | pad(0, 8.0f), spacer(),
                     text_owned(p.footer) | font(fs * 0.75f) | fg(muted) | ellipsis | width(pct(100)) | pad(12.0f, 16.0f))
                 | width(kSetSidebarW) | height(pct(100)) | bg(mix(t.background, t.text_primary, 0.02f))
                 | inner_shadow(0.0f, line, {-1.0f, 0.0f});

    // ── header: search, modified, reset ──────────────────────────────────
    const bool searching = st.filtering || !st.query.empty();
    std::vector<Node> field;
    field.push_back((text_owned(std::string{ui_glyph(t, "\uF002", "/")}) | font(fs * 0.85f) | fg(searching ? t.accent : muted)).build());
    if (st.query.empty() && !st.filtering) field.push_back((text_owned("Search settings") | font(fs) | fg(muted) | grow()).build());
    else {
        field.push_back((text_owned(st.query) | font(fs) | fg(t.text_primary)).build());
        if (st.filtering) field.push_back((box() | size(1.5f, 16.0f) | bg(t.accent)).build());
        field.push_back((box() | grow()).build());
        field.push_back((text_owned(std::to_string(vis.size()) + " found") | font(fs * 0.8f) | fg(muted)).build());
    }
    field.push_back((h(text_owned("/") | font(fs * 0.75f) | fg(muted)) | size(18.0f, 18.0f) | center | radius(t.rounded(4.0f))
                     | border(t.stroke(1), line)).build());
    auto search = h(std::move(field)) | gap(8.0f) | align(Align::center) | height(32.0f) | pad(0, 10.0f) | grow()
                | radius(t.rounded(6.0f)) | bg(mix(t.background, t.text_primary, 0.04f))
                | border(t.stroke(1), st.filtering ? t.accent : line)
                | pointer_cursor | id_of(base + ".search")
                | on_click([on] { return on(SettingsOp{[](SettingsPanelState& s) { s.filtering = true; }}); });
    const auto rid_all = node_id(base + ".reset_all");
    const bool rh = modified && c.hovered(rid_all);
    auto header = h(std::move(search),
                    h(text_owned(modified ? std::to_string(modified) + " changed" : "Defaults") | font(fs * 0.85f)
                          | fg(modified ? t.accent : muted))
                        | height(32.0f) | center,
                    h(text_owned("Reset") | font(fs * 0.85f) | fg(!modified ? muted : rh ? t.text_primary : t.text_secondary))
                        | height(28.0f) | pad(0, 12.0f) | center | radius(t.rounded(6.0f))
                        | bg(rh ? mix(t.background, t.text_primary, 0.08f) : t.background.fade(0.0f))
                        | border(t.stroke(1), modified ? line : line.fade(0.0f))
                        | pointer_cursor | id_of(base + ".reset_all")
                        | on_click([on] { return on(SettingsOp{[](SettingsPanelState& s) { s.out.push_back({E::Op::reset_all, 0, 0.0f, 0}); }}); }))
                | gap(12.0f) | align(Align::center) | width(pct(100)) | height(52.0f) | pad(0, 20.0f);

    // ── body ─────────────────────────────────────────────────────────────
    std::vector<Node> blocks;
    std::size_t k = 0;
    while (k < vis.size()) {
        const SettingSpec& first = specs[vis[k]];
        std::vector<Node> rows;
        for (; k < vis.size() && specs[vis[k]].section == first.section; ++k) {
            const std::size_t i = vis[k];
            const SettingSpec& s = specs[i];
            const bool sel = static_cast<int>(i) == st.selected;
            const bool first_row = rows.empty();
            const std::string rid = base + "." + s.section + "." + s.key;
            const bool hov = c.hovered(part_id(base, i));

            Node ctl;
            switch (s.kind) {
                case SettingSpec::Kind::number: {
                    const float frac = s.hi > s.lo ? (s.value - s.lo) / (s.hi - s.lo) : 0.0f;
                    auto knob = node(range_knob(c, rid, frac, kSetKnobW, {.active = sel, .ticks = 0}))
                              | on_slide([on, i, s](Vec2 at) {
                                    return on(SettingsOp{[i, v = setting_from_fraction(s, range_knob_fraction(at.x, kSetKnobW))](SettingsPanelState& x) {
                                        x.selected = static_cast<int>(i);
                                        x.out.push_back({E::Op::set, i, v, 0});
                                    }});
                                });
                    ctl = (h(std::move(knob),
                             h(text_owned(setting_readout(s)) | font(fs * 0.9f) | monospace
                                   | fg(s.modified ? t.accent : t.text_primary))
                                 | width(kSetValueW) | height(26.0f) | center | radius(t.rounded(5.0f))
                                 | bg(mix(t.background, t.text_primary, 0.05f)) | border(t.stroke(1), line))
                           | gap(10.0f) | align(Align::center) | width(kSetCtlW)).build();
                    break;
                }
                case SettingSpec::Kind::toggle:
                    ctl = (h(box() | grow(),
                             node(switch_knob(c, rid, s.value > 0.5f, {.active = sel}))
                                 | on_click([on, f = emit(i, E::Op::set, s.value > 0.5f ? 0.0f : 1.0f)] { return on(SettingsOp{f}); }))
                           | align(Align::center) | width(kSetCtlW)).build();
                    break;
                case SettingSpec::Kind::choice: {
                    std::vector<Node> segs;
                    for (std::size_t q = 0; q < s.options.size(); ++q) {
                        const bool on_ = static_cast<int>(q) == static_cast<int>(s.value);
                        const bool sh = c.hovered(part_id(rid, q));
                        segs.push_back((h(text_owned(s.options[q]) | font(fs * 0.88f)
                                              | fg(on_ ? t.text_primary : sh ? t.text_primary : t.text_secondary)
                                              | (on_ ? semibold : weight(FontWeight::regular)))
                                        | grow() | height(24.0f) | center | radius(t.rounded(4.0f))
                                        | bg(on_ ? mix(t.background, t.text_primary, 0.13f) : sh ? mix(t.background, t.text_primary, 0.05f)
                                                 : t.background.fade(0.0f))
                                        | shadow(on_ ? 3.0f : 0.0f, colors::black.fade(on_ ? 0.35f : 0.0f), {0.0f, 1.0f})
                                        | pointer_cursor | role(a11y::Role::radio) | checked(on_) | label(s.options[q]) | id_of(rid, q)
                                        | on_click([on, f = emit(i, E::Op::set, static_cast<float>(q))] { return on(SettingsOp{f}); }))
                                           .build());
                    }
                    ctl = (h(std::move(segs)) | gap(2.0f) | pad(3.0f) | width(kSetCtlW) | radius(t.rounded(6.0f))
                           | bg(mix(t.background, t.text_primary, 0.04f)) | border(t.stroke(1), line)
                           | role(a11y::Role::group) | label(rid)).build();
                    break;
                }
                case SettingSpec::Kind::cycle: {
                    const auto opts = setting_select_options(s);
                    const int cur = std::clamp(static_cast<int>(s.value), 0, std::max(static_cast<int>(opts.size()) - 1, 0));
                    const bool open = st.menu == static_cast<int>(i);
                    const std::string tid = rid + ".select";
                    SelectOption shown = opts.empty() ? SelectOption{.label = "\u2014"} : opts[static_cast<std::size_t>(cur)];
                    ctl = (node(select_trigger(c, tid, shown, open, kSetCtlW))
                           | on_click([on, sp, i, open] {
                                 return on(SettingsOp{[sp, i, open](SettingsPanelState& x) {
                                     if (open || x.menu == static_cast<int>(i)) settings_ops::close_menu(x, false);
                                     else settings_ops::open_menu(x, *sp, i);
                                 }});
                             })).build();
                    if (open) {
                        const auto filtered = select_filter(opts, st.menu_query);
                        c.overlay(Overlay{
                            .content = select_menu(c, base + ".menu", opts, filtered, cur, st.menu_hot, st.menu_query,
                                                   st.menu_scroll, kSetCtlW + 60.0f,
                                                   [on, i](int oi) {
                                                       return on(SettingsOp{[i, oi](SettingsPanelState& x) {
                                                           x.out.push_back({E::Op::set, i, static_cast<float>(oi), 0});
                                                           settings_ops::close_menu(x, false);
                                                       }});
                                                   },
                                                   [on](Vec2 d) { return on(SettingsOp{[d](SettingsPanelState& x) { x.menu_scroll.scroll_by(d); }}); }),
                            .id = part_id(id, kSetMenuPart),
                            .anchor_id = node_id(tid),
                            .placement = Placement::below_end,
                            .dismiss = Dismiss::on_click_outside,
                            .offset = 4.0f,
                            .layer = 40});
                    }
                    break;
                }
            }

            const auto xid = part_id(rid, 9);
            auto reset = h(text_owned(s.modified ? std::string{ui_glyph(t, "\uF0E2", "\u21BA")} : std::string{})
                               | font(fs * 0.8f) | fg(c.hovered(xid) ? t.text_primary : muted))
                       | size(22.0f, 22.0f) | center | radius(t.rounded(5.0f))
                       | bg(s.modified && c.hovered(xid) ? mix(t.background, t.text_primary, 0.08f) : t.background.fade(0.0f))
                       | pointer_cursor | id_of(rid, 9)
                       | on_click([on, f = emit(i, E::Op::reset)] { return on(SettingsOp{f}); });

            auto title = h(text_owned(setting_title(s)) | font(fs) | fg(t.text_primary),
                           box() | size(5.0f, 5.0f) | radius(t.rounded(2.5f)) | bg(s.modified ? t.accent : t.accent.fade(0.0f)))
                       | gap(6.0f) | align(Align::center);
            rows.push_back((h(v(std::move(title), text_owned(s.hint) | font(fs * 0.82f) | fg(muted) | ellipsis | width(pct(100)))
                                  | gap(2.0f) | grow() | min_size(0, 0),
                              node(std::move(ctl)), std::move(reset))
                            | gap(14.0f) | align(Align::center) | height(kSetRowH) | width(pct(100))
                            | pad(0, 6.0f, 0, 14.0f)
                            | bg(sel ? mix(card, t.accent, 0.08f) : hov ? mix(card, t.text_primary, 0.025f) : card)
                            | inner_shadow(0.0f, first_row ? line.fade(0.0f) : line, {0.0f, 1.0f})
                            | id_of(base, i)
                            | on_click([on, sp, i] { return on(SettingsOp{[sp, i](SettingsPanelState& x) { settings_ops::select(x, *sp, i); }}); }))
                               .build());
        }
        auto head = h(text_owned(cap(first.section)) | font(fs * 0.9f) | semibold | fg(t.text_secondary))
                  | align(Align::center) | height(kSetHeadH) | pad(0, 4.0f);
        blocks.push_back((v(std::move(head),
                            v(std::move(rows)) | width(pct(100)) | radius(t.rounded(8.0f)) | clip | border(t.stroke(1), line))
                          | width(pct(100))).build());
    }
    if (vis.empty())
        blocks.push_back((v(text_owned("No settings match \u201C" + st.query + "\u201D") | font(fs) | fg(t.text_secondary),
                            text_owned("Press esc to clear the search") | font(fs * 0.85f) | fg(muted))
                          | gap(6.0f) | align(Align::center) | width(pct(100)) | pad(48.0f, 0)).build());
    auto content = v(std::move(blocks)) | gap(kSetGap) | pad(0.0f, 20.0f, 20.0f, 20.0f) | width(pct(100));
    Node body = scroll_view(c, base + ".rows", st.scroll, std::move(content),
                            [on](ScrollOp op) { return on(SettingsOp{[op](SettingsPanelState& s) { op(s.scroll); }}); });

    // ── footer: key hints ────────────────────────────────────────────────
    const auto hint = [&](std::string_view k, std::string_view what) {
        return (h(h(text_owned(std::string{k}) | font(fs * 0.75f) | fg(t.text_secondary))
                      | height(18.0f) | pad(0, 5.0f) | center | radius(t.rounded(4.0f)) | border(t.stroke(1), line),
                  text_owned(std::string{what}) | font(fs * 0.78f) | fg(muted))
                | gap(6.0f) | align(Align::center)).build();
    };
    std::vector<Node> keys;
    if (st.menu >= 0) { keys.push_back(hint("\u2191\u2193", "preview")); keys.push_back(hint("\u23CE", "keep")); keys.push_back(hint("esc", "revert")); }
    else if (st.filtering) { keys.push_back(hint("\u23CE", "done")); keys.push_back(hint("esc", "clear")); }
    else {
        keys.push_back(hint("\u2191\u2193", "move")); keys.push_back(hint("\u2190\u2192", "adjust"));
        keys.push_back(hint("\u23CE", "open")); keys.push_back(hint("/", "search")); keys.push_back(hint("esc", "close"));
    }
    std::string path;
    if (has_sel) { const auto& s = specs[static_cast<std::size_t>(st.selected)]; path = s.section + "." + s.key; }
    auto footer = h(text_owned(path) | font(fs * 0.8f) | monospace | fg(muted) | grow() | ellipsis | min_size(0, 0),
                    h(std::move(keys)) | gap(14.0f) | align(Align::center))
                | gap(12.0f) | align(Align::center) | height(36.0f) | width(pct(100)) | pad(0, 20.0f)
                | inner_shadow(0.0f, line, {0.0f, 1.0f});

    return (h(std::move(sidebar),
              v(std::move(header), node(std::move(body)) | grow() | min_size(0, 0) | width(pct(100)), std::move(footer))
                  | grow() | height(pct(100)) | min_size(0, 0))
            | size(p.width, p.height) | clip | bg(t.background) | border(t.stroke(1), mix(t.background, t.text_primary, 0.16f))
            | radius(t.rounded(10.0f)) | shadow(40.0f, rgba<0x00000090>, {0.0f, 16.0f})
            | role(a11y::Role::dialog) | label(p.title)).build();
}

}  // namespace mayag::dsl
