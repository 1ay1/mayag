#pragma once
// mayag::widget file_icon — a small coloured glyph for a file type
//
// In the language's conventional colour, which is what makes a tab bar or a
// file tree scannable at a glance. The GLYPH follows `Theme::icons`: a
// letter mark by default (no font needed), or the Nerd Font devicon when
// the app's stack carries one -- what terminal users expect to see.

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

#include <string>
#include <string_view>

namespace mayag::dsl {

struct FileKind { const char* mark; std::uint32_t colour; const char* nerd = "\uF15B"; };

[[nodiscard]] inline FileKind file_kind(std::string_view name, bool dir = false, bool open = false) {
    if (dir) return {open ? "\u25BE" : "\u25B8", 0x7AA2F7, open ? "\uF07C" : "\uF07B"};
    const auto dot = name.rfind('.');
    const std::string_view e = dot == std::string_view::npos ? name : name.substr(dot + 1);
    if (name == "CMakeLists.txt" || e == "cmake") return {"\u25B3", 0x6D8086, "\uE794"};
    if (name == "Makefile" || name == "makefile") return {"M", 0x6D8086, "\uE779"};
    if (name == ".gitignore" || name == ".gitmodules" || name == ".gitattributes") return {"G", 0xF14E32, "\uE702"};
    if (name == "Dockerfile") return {"D", 0x458EE6, "\uE7B0"};
    if (e == "cpp" || e == "cc" || e == "cxx") return {"C+", 0x659AD2, "\uE61D"};
    if (e == "hpp" || e == "hh" || e == "h" || e == "hxx") return {"H", 0xA074C4, "\uF0FD"};
    if (e == "c") return {"C", 0x659AD2, "\uE61E"};
    if (e == "rs") return {"R", 0xDEA584, "\uE7A8"};
    if (e == "py") return {"Py", 0xF7CA3F, "\uE606"};
    if (e == "ts" || e == "tsx") return {"TS", 0x3178C6, "\uE628"};
    if (e == "js" || e == "jsx" || e == "mjs") return {"JS", 0xF1E05A, "\uE60C"};
    if (e == "go") return {"Go", 0x00ADD8, "\uE627"};
    if (e == "lua") return {"L", 0x51A0CF, "\uE620"};
    if (e == "zig") return {"Z", 0xF69A1B, "\uE6A9"};
    if (e == "sh" || e == "bash" || e == "zsh" || e == "fish") return {"$", 0x89E051, "\uF489"};
    if (e == "html") return {"<>", 0xE44D26, "\uE736"};
    if (e == "css") return {"#", 0x563D7C, "\uE749"};
    if (e == "nix") return {"N", 0x7EBAE4, "\uF313"};
    if (e == "vim") return {"V", 0x019833, "\uE62B"};
    if (e == "png" || e == "jpg" || e == "jpeg" || e == "gif" || e == "webp" || e == "svg") return {"\u25A3", 0xA074C4, "\uF1C5"};
    if (e == "lock") return {"L", 0x8B95A7, "\uF023"};
    if (e == "json") return {"{}", 0xCBCB41, "\uE60B"};
    if (e == "toml" || e == "yml" || e == "yaml" || e == "ini" || e == "conf") return {"\u2699", 0x9CA3AF, "\uE615"};
    if (e == "md" || e == "markdown") return {"M\u2193", 0x519ABA, "\uE609"};
    if (name == "LICENSE") return {"\u00A7", 0xD0BF41, "\uF0E3"};
    return {"\u2261", 0x8B95A7, "\uF15C"};
}

[[nodiscard]] inline auto file_icon(const Theme& t, std::string_view name, float px = 0.0f,
                                    bool dir = false, bool open = false) {
    const FileKind k = file_kind(name, dir, open);
    const float s = px > 0.0f ? px : 12.0f;
    const auto col = rgb8(static_cast<std::uint8_t>(k.colour >> 16), static_cast<std::uint8_t>(k.colour >> 8),
                          static_cast<std::uint8_t>(k.colour));
    const bool nerd = t.icons == IconSet::nerd;
    // A devicon is a full-size glyph; a letter mark is set small and bold.
    return h(text_owned(nerd ? k.nerd : k.mark) | glyph_size(nerd ? s * 1.05f : s * 0.78f)
             | weight(nerd ? FontWeight::regular : FontWeight::bold) | fg(col))
         | size(s * 1.35f, s * 1.35f) | center;
}

}  // namespace mayag::dsl
