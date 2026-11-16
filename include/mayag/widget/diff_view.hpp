#pragma once
// mayag::widget diff_view — a unified line diff, as a view
//
//   auto lines = line_diff(old_lines, new_lines);
//   diff_view(t, lines, 12)            // hunks with 2 lines of context
//
// line_diff is an exact LCS diff (O(n*m)), meant for config files, short
// documents and settings — hundreds of lines, not a kernel tree.

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace mayag::dsl {

struct DiffLine {
    enum class Op : std::uint8_t { same, add, del };
    Op          op = Op::same;
    std::string text;
    int         old_no = 0;   ///< 1-based line in the old text; 0 for an addition
    int         new_no = 0;   ///< 1-based line in the new text; 0 for a deletion
};

[[nodiscard]] inline std::vector<DiffLine> line_diff(const std::vector<std::string>& a,
                                                     const std::vector<std::string>& b) {
    const std::size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;)
        for (std::size_t j = m; j-- > 0;)
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
    std::vector<DiffLine> out;
    std::size_t i = 0, j = 0;
    const auto num = [](std::size_t k) { return static_cast<int>(k + 1); };
    while (i < n && j < m) {
        if (a[i] == b[j]) { out.push_back({DiffLine::Op::same, a[i], num(i), num(j)}); ++i; ++j; }
        else if (lcs[i + 1][j] >= lcs[i][j + 1]) { out.push_back({DiffLine::Op::del, a[i], num(i), 0}); ++i; }
        else { out.push_back({DiffLine::Op::add, b[j], 0, num(j)}); ++j; }
    }
    for (; i < n; ++i) out.push_back({DiffLine::Op::del, a[i], num(i), 0});
    for (; j < m; ++j) out.push_back({DiffLine::Op::add, b[j], 0, num(j)});
    return out;
}

struct DiffStats { int added = 0, removed = 0; };
[[nodiscard]] inline DiffStats diff_stats(const std::vector<DiffLine>& lines) noexcept {
    DiffStats s;
    for (const auto& l : lines) {
        if (l.op == DiffLine::Op::add) ++s.added;
        else if (l.op == DiffLine::Op::del) ++s.removed;
    }
    return s;
}

/// The changed lines with `context` unchanged lines around each change, old
/// and new line numbers in the gutter, and a ⋯ row where unchanged lines are
/// elided. Empty when nothing changed.
[[nodiscard]] inline Node diff_view(const Theme& t, const std::vector<DiffLine>& lines,
                                    float size = 12.0f, int context = 2) {
    using Op = DiffLine::Op;
    std::vector<bool> show(lines.size(), false);
    for (std::size_t k = 0; k < lines.size(); ++k) {
        if (lines[k].op == Op::same) continue;
        const long lo = std::max(0L, static_cast<long>(k) - context);
        const long hi = std::min(static_cast<long>(lines.size()) - 1, static_cast<long>(k) + context);
        for (long q = lo; q <= hi; ++q) show[static_cast<std::size_t>(q)] = true;
    }
    const float gutter = size * 2.4f;
    std::vector<Node> rows;
    bool elided = false;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        if (!show[k]) { elided = true; continue; }
        if (elided && !rows.empty())
            rows.push_back((h(text<"\u22EF"> | font(size) | fg(var::text_disabled)) | pad(t.spaced(1), t.spaced(6))
                            | width(pct(100))).build());
        elided = false;
        const auto& l = lines[k];
        const Color<Srgb> tone = l.op == Op::add ? t.success : l.op == Op::del ? t.danger : t.text_secondary;
        const char* sign = l.op == Op::add ? "+" : l.op == Op::del ? "-" : " ";
        rows.push_back((h(text_owned(l.old_no ? std::to_string(l.old_no) : "") | font(size * 0.9f)
                              | fg(var::text_disabled) | text_align(TextAlign::right) | width(gutter),
                          text_owned(l.new_no ? std::to_string(l.new_no) : "") | font(size * 0.9f)
                              | fg(var::text_disabled) | text_align(TextAlign::right) | width(gutter),
                          text_owned(sign) | font(size) | bold | fg(tone) | width(size * 1.2f)
                              | text_align(TextAlign::center),
                          text_owned(l.text) | font(size) | no_ligatures
                              | fg(l.op == Op::same ? t.text_secondary : tone) | grow())
                        | gap(t.spaced(6)) | align(Align::center) | pad(t.spaced(1), t.spaced(6)) | width(pct(100))
                        | bg(l.op == Op::same ? t.surface.fade(0.0f) : t.tint(tone, 0.10f))).build());
    }
    return v_list(std::move(rows)).build();
}

}  // namespace mayag::dsl
