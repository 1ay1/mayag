#pragma once
// mayag::layout::TextMeasurer — the layout/font-engine boundary
//
// Layout must know how big a string is before there is a GPU, a window, or a
// font atlas. So the flex engine depends on this narrow interface, never on a
// concrete font backend. Three consequences:
//
//   * layout is unit-testable with a fake measurer (deterministic, no fonts)
//   * the same layout code serves the real renderer and headless snapshots
//   * swapping HarfBuzz in later touches exactly one class
//
// A default monospace-metric implementation is provided so that mayag works
// out of the box with no font files at all.

#include "../style/style.hpp"
#include "../core/geometry.hpp"

#include <string_view>

namespace mayag::layout {

/// Per-line metrics needed for baseline alignment and underline placement.
struct LineMetrics {
    float ascent   = 0.0f;
    float descent  = 0.0f;
    float advance  = 0.0f;   ///< total width
};

class TextMeasurer {
  public:
    virtual ~TextMeasurer() = default;

    /// Size of `s` when wrapped to `max_width` (may be infinite = no wrap).
    [[nodiscard]] virtual Vec2 measure(std::string_view s, const TextStyle& st,
                                       float max_width) const = 0;

    /// Advance width of a single line, no wrapping.
    [[nodiscard]] virtual float advance(std::string_view s, const TextStyle& st) const = 0;

    /// Ascent above the baseline, for `Align::baseline`.
    [[nodiscard]] virtual float ascent(const TextStyle& st) const = 0;
};

// ── default: metric-only monospace ──────────────────────────────────────

/// Measures as if every character were `advance_ratio * size` wide. Crude for
/// proportional fonts but exact for monospace, deterministic across machines,
/// and dependency-free — which makes it the right default and the right thing
/// for golden-image tests.
class MonospaceMetrics final : public TextMeasurer {
  public:
    explicit MonospaceMetrics(float advance_ratio = 0.6f, float ascent_ratio = 0.8f) noexcept
        : advance_ratio_{advance_ratio}, ascent_ratio_{ascent_ratio} {}

    [[nodiscard]] float advance(std::string_view s, const TextStyle& st) const override {
        return static_cast<float>(codepoints(s)) * char_width(st);
    }

    [[nodiscard]] float ascent(const TextStyle& st) const override {
        return st.size * ascent_ratio_;
    }

    [[nodiscard]] Vec2 measure(std::string_view s, const TextStyle& st,
                               float max_width) const override {
        const float cw = char_width(st);
        const float lh = st.line_advance();
        if (s.empty()) return {0.0f, lh};

        // Greedy wrap at word boundaries; a word longer than the line breaks
        // mid-word rather than overflowing, which is what users expect from
        // a URL in a narrow panel.
        const bool wrapping = st.overflow == TextOverflow::wrap && max_width > 0.0f &&
                              !num::is_inf(max_width);
        const auto cols = wrapping
            ? num::max(static_cast<std::size_t>(max_width / num::max(cw, 0.001f)), std::size_t{1})
            : std::size_t{~0ull};

        std::size_t lines = 1, col = 0, widest = 0, word = 0;

        for (std::size_t i = 0; i <= s.size(); ++i) {
            const bool at_end = (i == s.size());
            const char ch = at_end ? '\n' : s[i];

            if (ch == '\n') {
                col += word;
                widest = num::max(widest, col);
                if (!at_end) { ++lines; col = 0; }
                word = 0;
                continue;
            }
            if (ch == ' ') {
                col += word + 1;
                word = 0;
                if (col > cols) { ++lines; widest = num::max(widest, cols); col = 0; }
                continue;
            }
            if ((static_cast<unsigned char>(ch) & 0xC0) == 0x80) continue;  // UTF-8 continuation

            ++word;
            if (col + word > cols) {
                if (col > 0) { widest = num::max(widest, col); ++lines; col = 0; }
                else         { widest = num::max(widest, cols); ++lines; word = 1; }
            }
        }
        widest = num::max(widest, col);

        return {static_cast<float>(widest) * cw + st.letter_spacing * static_cast<float>(widest),
                static_cast<float>(lines) * lh};
    }

  private:
    [[nodiscard]] float char_width(const TextStyle& st) const noexcept {
        return st.size * advance_ratio_ + st.letter_spacing;
    }

    /// Count UTF-8 codepoints, not bytes — otherwise every emoji is 4 columns.
    [[nodiscard]] static std::size_t codepoints(std::string_view s) noexcept {
        std::size_t n = 0;
        for (unsigned char c : s) n += ((c & 0xC0) != 0x80);
        return n;
    }

    float advance_ratio_;
    float ascent_ratio_;
};

/// Process-wide fallback so `layout_tree(node, size)` works with no setup.
[[nodiscard]] inline const TextMeasurer& default_measurer() {
    static const MonospaceMetrics instance{};
    return instance;
}

}  // namespace mayag::layout
