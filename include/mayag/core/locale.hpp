#pragma once
// mayag/core/locale.hpp — the user's locale, read ONE way
//
// Two consumers need it: Compose (xkbcommon wants the raw locale name, e.g.
// "en_US.UTF-8") and font fallback (Han region from the language, "zh-tw").
// Both follow POSIX precedence for the character-type category: LC_ALL, then
// LC_CTYPE, then LANG. The language preference list LANGUAGE (GNU) applies to
// messages and to font language, not to Compose. "C"/"POSIX" mean none.

#include <cctype>
#include <cstdlib>
#include <string>

namespace mayag::locale {

/// The raw LC_CTYPE locale name ("en_US.UTF-8"), or "C" when unset: what
/// xkb_compose_table_new_from_locale expects.
[[nodiscard]] inline std::string ctype_name() {
    for (const char* var : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* v = std::getenv(var);
        if (v != nullptr && *v != '\0') return v;
    }
    return "C";
}

/// The user's language for text, lowercase BCP-47-ish ("zh-tw", "ja",
/// "en-us"): LANGUAGE's first entry, then the LC_CTYPE locale. Empty when
/// there is no preference.
[[nodiscard]] inline std::string language() {
    std::string s;
    if (const char* l = std::getenv("LANGUAGE"); l != nullptr && *l != '\0') s = l;
    else s = ctype_name();
    if (const auto colon = s.find(':'); colon != std::string::npos) s.resize(colon);
    if (const auto dot = s.find_first_of(".@"); dot != std::string::npos) s.resize(dot);
    if (s == "C" || s == "POSIX") return {};
    for (char& c : s) c = c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace mayag::locale
