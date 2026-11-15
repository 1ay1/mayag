#pragma once
// mayag::widget file_icon — a small coloured glyph for a file type
//
// Not an icon font (mayag has no dependency to ship one): a letter mark in
// the language's conventional colour, which is what makes a tab bar or a
// file tree scannable at a glance.

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

#include <string>
#include <string_view>

namespace mayag::dsl {

struct FileKind { const char* mark; std::uint32_t colour; };

[[nodiscard]] inline FileKind file_kind(std::string_view name, bool dir = false, bool open = false) {
    if (dir) return {open ? "\u25BE" : "\u25B8", 0x8B95A7};
    const auto dot = name.rfind('.');
    const std::string_view e = dot == std::string_view::npos ? name : name.substr(dot + 1);
    if (e == "cpp" || e == "cc" || e == "cxx") return {"C+", 0x659AD2};
    if (e == "hpp" || e == "hh" || e == "h" || e == "hxx") return {"H", 0xA074C4};
    if (e == "c") return {"C", 0x659AD2};
    if (e == "rs") return {"R", 0xDEA584};
    if (e == "py") return {"Py", 0xF7CA3F};
    if (e == "ts" || e == "tsx") return {"TS", 0x3178C6};
    if (e == "js" || e == "jsx" || e == "mjs") return {"JS", 0xF1E05A};
    if (e == "go") return {"Go", 0x00ADD8};
    if (e == "json") return {"{}", 0xCBCB41};
    if (e == "toml" || e == "yml" || e == "yaml") return {"\u2699", 0x9CA3AF};
    if (e == "md" || e == "markdown") return {"M\u2193", 0x519ABA};
    if (e == "sh" || e == "zsh" || e == "bash") return {"$", 0x89E051};
    if (name == "CMakeLists.txt" || e == "cmake") return {"\u25B3", 0x6D8086};
    if (name == "LICENSE") return {"\u00A7", 0xD0BF41};
    if (e == "png" || e == "jpg" || e == "svg") return {"\u25A3", 0xA074C4};
    return {"\u2261", 0x8B95A7};
}

[[nodiscard]] inline auto file_icon(const Theme& t, std::string_view name, float px = 0.0f,
                                    bool dir = false, bool open = false) {
    (void)t;
    const FileKind k = file_kind(name, dir, open);
    const float s = px > 0.0f ? px : 12.0f;
    const auto col = rgb8(static_cast<std::uint8_t>(k.colour >> 16), static_cast<std::uint8_t>(k.colour >> 8),
                          static_cast<std::uint8_t>(k.colour));
    return h(text_owned(k.mark) | font(s * 0.78f) | bold | fg(col)) | size(s * 1.35f, s * 1.35f) | center;
}

}  // namespace mayag::dsl
