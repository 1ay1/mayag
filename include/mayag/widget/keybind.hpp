#pragma once
// mayag::widget keybind — a keyboard-shortcut field
//
// Keycaps at rest, a live prompt while recording.
// Interaction: Clicked on part_id(base, index) -> start recording that row;
// while recording, the next non-modifier KeyEvent is the chord (skip keys
// where is_modifier_key() holds), Escape cancels, Backspace clears.
// `conflict` paints the field as an error.

#include "control.hpp"
#include "../core/event.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

/// A recorded shortcut: a key and the modifiers held with it.
struct Chord {
    Key  key = Key::unknown;
    Mods mods{};
    friend constexpr bool operator==(const Chord&, const Chord&) = default;
    [[nodiscard]] constexpr bool empty() const noexcept { return key == Key::unknown; }
};

/// How a key is printed on a keycap.
[[nodiscard]] inline std::string key_label(Key k) {
    switch (k) {
        case Key::enter: return "\u21A9";        case Key::tab: return "\u21E5";
        case Key::backspace: return "\u232B";    case Key::del: return "\u2326";
        case Key::escape: return "Esc";          case Key::space: return "Space";
        case Key::left: return "\u2190";         case Key::right: return "\u2192";
        case Key::up: return "\u2191";           case Key::down: return "\u2193";
        case Key::home: return "Home";           case Key::end: return "End";
        case Key::page_up: return "PgUp";        case Key::page_down: return "PgDn";
        case Key::insert: return "Ins";
        case Key::minus: return "-";             case Key::equal: return "=";
        case Key::bracket_left: return "[";      case Key::bracket_right: return "]";
        case Key::backslash: return "\\";        case Key::semicolon: return ";";
        case Key::quote: return "'";             case Key::comma: return ",";
        case Key::period: return ".";            case Key::slash: return "/";
        case Key::grave: return "`";
        default: break;
    }
    const auto n = static_cast<int>(k);
    if (k >= Key::a && k <= Key::z) return std::string(1, static_cast<char>('A' + (n - static_cast<int>(Key::a))));
    if (k >= Key::n0 && k <= Key::n9) return std::string(1, static_cast<char>('0' + (n - static_cast<int>(Key::n0))));
    if (k >= Key::f1 && k <= Key::f12) return "F" + std::to_string(n - static_cast<int>(Key::f1) + 1);
    return "?";
}

/// True for keys that only make sense held with another (a shortcut cannot
/// BE Shift). The recorder keeps listening past them.
[[nodiscard]] constexpr bool is_modifier_key(Key k) noexcept {
    return k == Key::shift || k == Key::ctrl || k == Key::alt || k == Key::super ||
           k == Key::caps_lock;
}

/// The keycaps of a chord, in the platform's conventional modifier order.
[[nodiscard]] inline std::vector<std::string> chord_caps(Chord ch) {
    std::vector<std::string> caps_;
#if defined(__APPLE__)
    if (ch.mods.ctrl)  caps_.push_back("\u2303");
    if (ch.mods.alt)   caps_.push_back("\u2325");
    if (ch.mods.shift) caps_.push_back("\u21E7");
    if (ch.mods.super) caps_.push_back("\u2318");
#else
    if (ch.mods.ctrl)  caps_.push_back("Ctrl");
    if (ch.mods.alt)   caps_.push_back("Alt");
    if (ch.mods.shift) caps_.push_back("Shift");
    if (ch.mods.super) caps_.push_back("Super");
#endif
    if (!ch.empty()) caps_.push_back(key_label(ch.key));
    return caps_;
}

template <ControlContext C>
[[nodiscard]] inline auto keybind(const C& c, std::string_view base, std::size_t index, Chord ch,
                                  bool recording, bool conflict = false) {
    const Theme& t = c.theme;
    const auto pid = part_id(base, index);
    const bool hov = c.hovered(pid);
    std::vector<Node> caps_;
    if (recording) {
        caps_.push_back((text<"Press a shortcut\u2026"> | font(t.font_size * 0.9f)
                         | fg(var::accent)).build());
    } else if (ch.empty()) {
        caps_.push_back((text<"Unassigned"> | font(t.font_size * 0.9f) | italic
                         | fg(var::text_disabled)).build());
    } else {
        for (auto& cap : chord_caps(ch)) {
            caps_.push_back((h(text_owned(std::move(cap)) | font(t.font_size * 0.85f)
                                 | semibold | fg(var::text_primary))
                             | pad(t.spaced(2), t.spaced(7))
                             | bg(var::surface_raised) | border(t.stroke(1), t.border_strong)
                             | radius(t.rounded(4))).build());
        }
    }
    return h(h_list(std::move(caps_), 4.0f))
         | align(Align::center) | justify(Justify::end)
         | pad(t.spaced(5), t.spaced(8)) | min_size(150, 32)
         | bg(recording ? t.accent.fade(0.12f) : (hov ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f)))
         | border(t.stroke(1), recording ? t.accent
                               : conflict ? t.danger
                               : detail::edge(t, c.focused(pid), hov, t.border.fade(0.0f)))
         | radius(t.radius_small)
         | pointer_cursor
         | role(a11y::Role::button) | label("shortcut")
         | id_of(base, index);
}

}  // namespace mayag::dsl
