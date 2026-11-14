#pragma once
// mayag::widget fuzzy — fuzzy matching and the highlighted text that shows it
//
//   auto fm = fuzzy_match("fsz", "font.size");      // ok, score, positions {0,5,7}
//   fuzzy_text("font.size", fm.positions, 12, t.text_primary, t.accent)
//
// The matcher is the subsequence kind every command palette uses: each
// pattern character must appear in order, case-insensitively. It prefers
// matches at word boundaries (after . _ - / space, or the start) and runs of
// consecutive characters, which is what makes "fsz" rank font.size above
// a key that merely contains those letters somewhere.

#include "../dsl/dsl.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

struct FuzzyMatch {
    bool                       ok = false;
    int                        score = 0;
    std::vector<std::uint16_t> positions;   ///< byte offsets of matched chars, ascending
};

namespace detail {
[[nodiscard]] constexpr char fold(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
[[nodiscard]] constexpr bool is_boundary(std::string_view text, std::size_t i) noexcept {
    if (i == 0) return true;
    const char p = text[i - 1];
    return p == '.' || p == '_' || p == '-' || p == '/' || p == ' ' || p == ':';
}
[[nodiscard]] inline FuzzyMatch fuzzy_pass(std::string_view pattern, std::string_view text,
                                           bool prefer_boundaries) {
    FuzzyMatch m;
    std::size_t from = 0;
    long last = -2;
    for (char pc : pattern) {
        if (pc == ' ') continue;
        const char want = fold(pc);
        std::size_t hit = std::string_view::npos;
        for (std::size_t j = from; j < text.size(); ++j)
            if (fold(text[j]) == want) { hit = j; break; }
        if (hit == std::string_view::npos) return {};
        // A non-consecutive, mid-word hit: look ahead for the same letter at a
        // word boundary, which is almost always what the user meant.
        if (prefer_boundaries && static_cast<long>(hit) != last + 1 && !is_boundary(text, hit)) {
            for (std::size_t j = hit + 1; j < text.size(); ++j)
                if (fold(text[j]) == want && is_boundary(text, j)) { hit = j; break; }
        }
        m.score += 1;
        if (static_cast<long>(hit) == last + 1) m.score += 6;
        if (is_boundary(text, hit)) m.score += 8;
        m.score -= static_cast<int>(hit - from) > 0 ? 1 : 0;
        m.positions.push_back(static_cast<std::uint16_t>(hit));
        last = static_cast<long>(hit);
        from = hit + 1;
    }
    m.score -= static_cast<int>(text.size() / 8);   // shorter keys win ties
    m.ok = true;
    return m;
}
}  // namespace detail

[[nodiscard]] inline FuzzyMatch fuzzy_match(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) return FuzzyMatch{true, 0, {}};
    // Boundary-preferring first; it can jump past a letter a later pattern
    // char needed, so fall back to plain greedy before declaring no match.
    auto m = detail::fuzzy_pass(pattern, text, true);
    return m.ok ? m : detail::fuzzy_pass(pattern, text, false);
}

/// `text` with the matched characters drawn in `hit` (semibold) and the rest
/// in `base`. Runs, not one node per character.
[[nodiscard]] inline auto fuzzy_text(std::string_view text, const std::vector<std::uint16_t>& hits,
                                     float size, Color<Srgb> base, Color<Srgb> hit) {
    std::vector<Node> runs;
    std::size_t i = 0, h = 0;
    while (i < text.size()) {
        const bool on = h < hits.size() && hits[h] == i;
        std::size_t j = i;
        if (on) {
            while (j < text.size() && h < hits.size() && hits[h] == j) { ++j; ++h; }
        } else {
            j = h < hits.size() ? hits[h] : text.size();
        }
        runs.push_back((text_owned(std::string{text.substr(i, j - i)}) | font(size)
                        | (on ? semibold : weight(FontWeight::regular)) | fg(on ? hit : base))
                           .build());
        i = j;
    }
    return h_list(std::move(runs));
}

}  // namespace mayag::dsl
