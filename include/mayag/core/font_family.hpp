#pragma once
// mayag::FontFamily — which typeface a run of text is set in, as a ROLE
//
// A UI that mixes prose and code needs several faces at once: a proportional
// face for labels, a monospace face for code and terminal cells, maybe a
// display face for headings. Naming the face by role instead of by file
// keeps the tree a pure value (a role is one byte, so TextStyle stays
// trivially copyable) and lets the font stack decide WHICH face plays it:
//
//     text_of(cmd) | monospace                    // the stack's mono face
//     stack->add_family(FontFamily::mono, face)   // the app picks it, once
//
// Beyond the three named roles an app may register numbered ones,
// `custom_family(n)`. That is how a typeface PICKER stays pure: preload the
// candidates as custom roles at startup, and the view chooses among them by
// writing a different role byte -- no font is loaded or swapped while the
// app runs, so the choice is undoable, replayable and animates like any
// other style change.
//
// `ui` is the stack's ordinary fallback chain, so text that names no family
// renders exactly as before. A role with no face assigned falls back to it.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mayag {

enum class FontFamily : std::uint8_t {
    ui      = 0,   ///< the primary face and its fallback chain
    mono    = 1,   ///< fixed pitch: code, terminal cells, key caps
    display = 2,   ///< headings and large numerals, when the app sets one
    // 3 .. font_family_count-1: custom_family(n)
};

inline constexpr std::size_t font_family_count = 32;   // 3 named + 29 an app preloads (two typeface pickers fit)
inline constexpr std::size_t custom_family_count = font_family_count - 3;

/// The n-th app-defined role (n < custom_family_count; larger n wraps).
[[nodiscard]] constexpr FontFamily custom_family(std::size_t n) noexcept {
    return static_cast<FontFamily>(3 + n % custom_family_count);
}

[[nodiscard]] constexpr std::size_t index_of(FontFamily f) noexcept {
    const auto i = static_cast<std::size_t>(f);
    return i < font_family_count ? i : 0;
}

[[nodiscard]] constexpr std::string_view name_of(FontFamily f) noexcept {
    switch (f) {
        case FontFamily::ui:      return "ui";
        case FontFamily::mono:    return "mono";
        case FontFamily::display: return "display";
    }
    return "custom";
}

}  // namespace mayag
