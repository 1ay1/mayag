#pragma once
// mayag::platform keys shared by every window backend.
//
// A character key is the character it types, as in GTK, Qt and AppKit's own
// menus: ctrl+z is the key LABELLED Z, on AZERTY, Dvorak or Colemak alike.
// Each backend asks its layout what the key types without modifiers (an xkb
// keysym on Wayland, charactersIgnoringModifiers on macOS) and maps it here;
// only a key that types no Latin character (a Cyrillic letter, a dead key)
// falls back to its position, so ctrl+c still copies on a Russian layout.

#include "../core/event.hpp"

#include <cstdint>

namespace mayag::platform {

/// The character key a Latin character is (letters either case, digits,
/// space, the punctuation Key names); unknown for anything else.
[[nodiscard]] constexpr Key char_key_of(std::uint32_t c) noexcept {
    if (c >= 'a' && c <= 'z') return static_cast<Key>(static_cast<int>(Key::a) + static_cast<int>(c - 'a'));
    if (c >= 'A' && c <= 'Z') return static_cast<Key>(static_cast<int>(Key::a) + static_cast<int>(c - 'A'));
    if (c >= '0' && c <= '9') return static_cast<Key>(static_cast<int>(Key::n0) + static_cast<int>(c - '0'));
    switch (c) {
        case ' ': return Key::space;
        case '-': return Key::minus;
        case '=': return Key::equal;
        case '[': return Key::bracket_left;
        case ']': return Key::bracket_right;
        case '\\': return Key::backslash;
        case ';': return Key::semicolon;
        case '\'': return Key::quote;
        case ',': return Key::comma;
        case '.': return Key::period;
        case '/': return Key::slash;
        case '`': return Key::grave;
        default: break;
    }
    return Key::unknown;
}

/// Is `k` a character key (a letter, digit, space or punctuation)?
[[nodiscard]] constexpr bool is_char_key(Key k) noexcept {
    return (k >= Key::a && k <= Key::grave) || k == Key::space;
}

/// The key a press is: `pos` is the key by position, `typed` the character
/// the layout puts on it without modifiers (0: unknown). A character Key
/// names wins, wherever the layout put it (a virtual keyboard -- wtype, a
/// remote desktop -- puts characters on arbitrary keys); otherwise the
/// position (named keys, non-Latin letters, dead keys).
[[nodiscard]] constexpr Key layout_key(Key pos, std::uint32_t typed) noexcept {
    if (const Key ch = char_key_of(typed); ch != Key::unknown) return ch;
    return pos;
}

}  // namespace mayag::platform
