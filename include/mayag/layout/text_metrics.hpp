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
#include <utility>
#include <vector>

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

    /// Nonzero inside a layout_tree() pass: lets measure() memoise per node.
    [[nodiscard]] virtual std::uint32_t layout_pass() const noexcept { return 0; }

    // ── editing: carets, hit-testing, selections on one unwrapped line ──
    //
    // Positions are byte offsets at grapheme boundaries. They are VISUAL
    // questions: in right-to-left or mixed text the caret for offset k is not
    // the width of the first k bytes, a click maps to whichever glyph is under
    // it, and a logical selection can be several disjoint visual rectangles.
    // The defaults are the left-to-right answers (prefix widths); a measurer
    // that shapes text overrides them with the bidi-aware ones.

    /// X of the caret before byte `offset` (0 = line start, size = end).
    [[nodiscard]] virtual float caret_x(std::string_view s, const TextStyle& st, std::size_t offset) const {
        return advance(s.substr(0, offset < s.size() ? offset : s.size()), st);
    }
    /// Byte offset (a caret position) nearest to x.
    [[nodiscard]] virtual std::size_t hit(std::string_view s, const TextStyle& st, float x) const {
        std::size_t best = 0;
        float best_d = x < 0.0f ? -x : x;
        for (std::size_t i = 1; i <= s.size(); ++i) {
            if (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) continue;
            const float d = caret_x(s, st, i) - x;
            const float ad = d < 0.0f ? -d : d;
            if (ad < best_d) { best_d = ad; best = i; }
        }
        return best;
    }
    /// The visual [x0, x1) spans covering the logical selection [a, b).
    [[nodiscard]] virtual std::vector<std::pair<float, float>> selection_spans(std::string_view s, const TextStyle& st,
                                                                               std::size_t a, std::size_t b) const {
        if (b < a) { const std::size_t t = a; a = b; b = t; }
        if (a == b) return {};
        return {{caret_x(s, st, a), caret_x(s, st, b)}};
    }
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
        // Below ~2.5 glyph widths, wrapping cannot produce more than one or
        // two characters per line. Such a box must NOT wrap: that produces an unreadable vertical ribbon,
        // arbitrarily tall, which looks like the text engine failed when the
        // real fault is upstream sizing. Overflowing is the lesser evil — it
        // is obviously wrong and points at the actual problem.
        const bool wrapping = st.overflow == TextOverflow::wrap && max_width >= cw * 2.5f &&
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
