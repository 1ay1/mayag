#pragma once
// mayag::text — text rendering with no font files and no dependencies
//
// Shipping a UI framework that cannot draw a letter until the user finds a
// TTF is a bad first impression. mayag embeds a compact stroke font covering
// printable ASCII: each glyph is a short list of line segments on a 0..1 grid,
// rendered through the SAME capsule SDF as everything else.
//
// That means built-in text is:
//   * resolution independent — it is geometry, not a bitmap atlas
//   * correctly antialiased by the same kernel as every other shape
//   * weight-aware — `bold` literally thickens the stroke
//   * about 4 KB of static data
//
// For production typography you bind a real font (`Font::from_file`) and this
// becomes the fallback. But nothing is blocked on that.

#include "../layout/text_metrics.hpp"
#include "../render/draw_list.hpp"
#include "../render/painter.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::fonts {

/// A stroke segment on the glyph's em box: coordinates in [0,1], y down.
struct Seg {
    float x0, y0, x1, y1;
};

namespace builtin {

// Glyph design grid: cap height spans y 0.18..0.80, baseline at 0.80,
// descenders to 0.95. x-advance is 0.60 em, matching MonospaceMetrics.
constexpr float T = 0.18f;   // top / cap
constexpr float M = 0.49f;   // middle
constexpr float B = 0.80f;   // baseline
constexpr float D = 0.95f;   // descender
constexpr float L = 0.10f;   // left
constexpr float R = 0.46f;   // right
constexpr float C = 0.28f;   // centre

/// Segment lists per printable ASCII character. Kept intentionally terse —
/// this is data, and its shape is obvious from the constants above.
struct Glyph {
    std::array<Seg, 10> segs{};
    std::uint8_t count = 0;

    constexpr Glyph() = default;
    constexpr Glyph(std::initializer_list<Seg> s) {
        for (const Seg& seg : s) { if (count < segs.size()) segs[count++] = seg; }
    }
};

constexpr Glyph glyph_for(char ch) {
    switch (ch) {
        case 'A': return {{L,B, C,T}, {C,T, R,B}, {L+0.07f,M+0.14f, R-0.07f,M+0.14f}};
        case 'B': return {{L,T, L,B}, {L,T, R-0.04f,T}, {R-0.04f,T, R,T+0.12f},
                          {R,T+0.12f, L,M}, {L,M, R,M+0.13f}, {R,M+0.13f, R-0.04f,B}, {R-0.04f,B, L,B}};
        case 'C': return {{R,T+0.08f, C,T}, {C,T, L,M}, {L,M, C,B}, {C,B, R,B-0.08f}};
        case 'D': return {{L,T, L,B}, {L,T, C+0.06f,T}, {C+0.06f,T, R,M}, {R,M, C+0.06f,B}, {C+0.06f,B, L,B}};
        case 'E': return {{R,T, L,T}, {L,T, L,B}, {L,B, R,B}, {L,M, R-0.06f,M}};
        case 'F': return {{R,T, L,T}, {L,T, L,B}, {L,M, R-0.06f,M}};
        case 'G': return {{R,T+0.08f, C,T}, {C,T, L,M}, {L,M, C,B}, {C,B, R,M+0.10f}, {R,M+0.10f, C+0.06f,M+0.10f}};
        case 'H': return {{L,T, L,B}, {R,T, R,B}, {L,M, R,M}};
        case 'I': return {{C,T, C,B}, {L,T, R,T}, {L,B, R,B}};
        case 'J': return {{R,T, R,B-0.08f}, {R,B-0.08f, C,B}, {C,B, L,B-0.10f}};
        case 'K': return {{L,T, L,B}, {R,T, L,M}, {L,M, R,B}};
        case 'L': return {{L,T, L,B}, {L,B, R,B}};
        case 'M': return {{L,B, L,T}, {L,T, C,M}, {C,M, R,T}, {R,T, R,B}};
        case 'N': return {{L,B, L,T}, {L,T, R,B}, {R,B, R,T}};
        case 'O': return {{C,T, L,M}, {L,M, C,B}, {C,B, R,M}, {R,M, C,T}};
        case 'P': return {{L,B, L,T}, {L,T, R,T+0.10f}, {R,T+0.10f, L,M}};
        case 'Q': return {{C,T, L,M}, {L,M, C,B}, {C,B, R,M}, {R,M, C,T}, {C,M+0.10f, R,B+0.06f}};
        case 'R': return {{L,B, L,T}, {L,T, R,T+0.10f}, {R,T+0.10f, L,M}, {L+0.08f,M, R,B}};
        case 'S': return {{R,T+0.06f, C,T}, {C,T, L,T+0.12f}, {L,T+0.12f, R,M+0.10f},
                          {R,M+0.10f, R,B-0.08f}, {R,B-0.08f, L,B-0.02f}};
        case 'T': return {{L,T, R,T}, {C,T, C,B}};
        case 'U': return {{L,T, L,B-0.08f}, {L,B-0.08f, C,B}, {C,B, R,B-0.08f}, {R,B-0.08f, R,T}};
        case 'V': return {{L,T, C,B}, {C,B, R,T}};
        case 'W': return {{L,T, L+0.08f,B}, {L+0.08f,B, C,M+0.10f}, {C,M+0.10f, R-0.08f,B}, {R-0.08f,B, R,T}};
        case 'X': return {{L,T, R,B}, {R,T, L,B}};
        case 'Y': return {{L,T, C,M+0.04f}, {R,T, C,M+0.04f}, {C,M+0.04f, C,B}};
        case 'Z': return {{L,T, R,T}, {R,T, L,B}, {L,B, R,B}};

        case 'a': return {{L,M, R-0.02f,M}, {R-0.02f,M, R,M+0.08f}, {R,M+0.08f, R,B}, {R,B, L,B},
                          {L,B, L,M+0.16f}, {L,M+0.16f, R,M+0.14f}};
        case 'b': return {{L,T, L,B}, {L,M, R,M+0.06f}, {R,M+0.06f, R,B-0.06f}, {R,B-0.06f, L,B}};
        case 'c': return {{R,M+0.04f, L,M+0.06f}, {L,M+0.06f, L,B-0.06f}, {L,B-0.06f, R,B-0.02f}};
        case 'd': return {{R,T, R,B}, {R,M, L,M+0.06f}, {L,M+0.06f, L,B-0.06f}, {L,B-0.06f, R,B}};
        case 'e': return {{L,M+0.14f, R,M+0.14f}, {R,M+0.14f, R,M+0.04f}, {R,M+0.04f, L,M+0.06f},
                          {L,M+0.06f, L,B-0.06f}, {L,B-0.06f, R,B-0.02f}};
        case 'f': return {{R,T+0.02f, C,T}, {C,T, C,B}, {L,M, R-0.04f,M}};
        case 'g': return {{R,M, L,M+0.06f}, {L,M+0.06f, L,B-0.06f}, {L,B-0.06f, R,B}, {R,M, R,D-0.04f},
                          {R,D-0.04f, L,D}};
        case 'h': return {{L,T, L,B}, {L,M+0.06f, R,M+0.10f}, {R,M+0.10f, R,B}};
        case 'i': return {{C,M, C,B}, {C,T+0.06f, C,T+0.14f}};
        case 'j': return {{R-0.06f,M, R-0.06f,D-0.04f}, {R-0.06f,D-0.04f, L,D}, {R-0.06f,T+0.06f, R-0.06f,T+0.14f}};
        case 'k': return {{L,T, L,B}, {R,M, L,M+0.16f}, {L,M+0.16f, R,B}};
        case 'l': return {{C-0.04f,T, C-0.04f,B-0.04f}, {C-0.04f,B-0.04f, C+0.08f,B}};
        case 'm': return {{L,B, L,M}, {L,M, C,M+0.06f}, {C,M+0.06f, C,B}, {C,M+0.06f, R,M+0.06f}, {R,M+0.06f, R,B}};
        case 'n': return {{L,B, L,M}, {L,M, R,M+0.08f}, {R,M+0.08f, R,B}};
        case 'o': return {{L,M+0.08f, C,M}, {C,M, R,M+0.08f}, {R,M+0.08f, C,B}, {C,B, L,M+0.08f}};
        case 'p': return {{L,M, L,D}, {L,M, R,M+0.06f}, {R,M+0.06f, R,B-0.06f}, {R,B-0.06f, L,B}};
        case 'q': return {{R,M, R,D}, {R,M, L,M+0.06f}, {L,M+0.06f, L,B-0.06f}, {L,B-0.06f, R,B}};
        case 'r': return {{L,B, L,M}, {L,M+0.04f, R,M}};
        case 's': return {{R,M+0.04f, L,M+0.02f}, {L,M+0.02f, R,M+0.16f}, {R,M+0.16f, L,B-0.02f}};
        case 't': return {{C-0.02f,T+0.10f, C-0.02f,B-0.06f}, {C-0.02f,B-0.06f, R,B}, {L,M, R-0.04f,M}};
        case 'u': return {{L,M, L,B-0.06f}, {L,B-0.06f, R,B}, {R,M, R,B}};
        case 'v': return {{L,M, C,B}, {C,B, R,M}};
        case 'w': return {{L,M, L+0.06f,B}, {L+0.06f,B, C,M+0.16f}, {C,M+0.16f, R-0.06f,B}, {R-0.06f,B, R,M}};
        case 'x': return {{L,M, R,B}, {R,M, L,B}};
        case 'y': return {{L,M, C,B}, {R,M, L+0.02f,D}};
        case 'z': return {{L,M, R,M}, {R,M, L,B}, {L,B, R,B}};

        case '0': return {{C,T, L,M}, {L,M, C,B}, {C,B, R,M}, {R,M, C,T}, {L+0.04f,B-0.06f, R-0.04f,T+0.06f}};
        case '1': return {{L+0.06f,T+0.10f, C,T}, {C,T, C,B}, {L,B, R,B}};
        case '2': return {{L,T+0.08f, C,T}, {C,T, R,T+0.10f}, {R,T+0.10f, L,B}, {L,B, R,B}};
        case '3': return {{L,T, R,T}, {R,T, C,M}, {C,M, R,M+0.12f}, {R,M+0.12f, C,B}, {C,B, L,B-0.06f}};
        case '4': return {{R-0.06f,T, L,M+0.10f}, {L,M+0.10f, R,M+0.10f}, {R-0.06f,T, R-0.06f,B}};
        case '5': return {{R,T, L,T}, {L,T, L,M}, {L,M, R,M+0.10f}, {R,M+0.10f, R,B-0.06f}, {R,B-0.06f, L,B}};
        case '6': return {{R,T+0.04f, L,M}, {L,M, L,B-0.06f}, {L,B-0.06f, C,B}, {C,B, R,M+0.14f}, {R,M+0.14f, L,M+0.12f}};
        case '7': return {{L,T, R,T}, {R,T, C-0.02f,B}};
        case '8': return {{C,T, L,T+0.12f}, {L,T+0.12f, C,M}, {C,M, R,T+0.12f}, {R,T+0.12f, C,T},
                          {C,M, L,M+0.14f}, {L,M+0.14f, C,B}, {C,B, R,M+0.14f}, {R,M+0.14f, C,M}};
        case '9': return {{C,M, L,T+0.10f}, {L,T+0.10f, C,T}, {C,T, R,M-0.04f}, {R,M-0.04f, R,B-0.04f},
                          {R,B-0.04f, L,B}, {C,M, R,M-0.02f}};

        case '.': return {{C-0.02f,B-0.03f, C+0.02f,B}};
        case ',': return {{C,B-0.03f, C-0.05f,B+0.10f}};
        case ':': return {{C-0.02f,M, C+0.02f,M+0.03f}, {C-0.02f,B-0.03f, C+0.02f,B}};
        case ';': return {{C-0.02f,M, C+0.02f,M+0.03f}, {C,B-0.03f, C-0.05f,B+0.10f}};
        case '-': return {{L,M+0.10f, R,M+0.10f}};
        case '_': return {{L,B+0.06f, R,B+0.06f}};
        case '+': return {{L,M+0.10f, R,M+0.10f}, {C,M-0.08f, C,M+0.28f}};
        case '=': return {{L,M+0.02f, R,M+0.02f}, {L,M+0.20f, R,M+0.20f}};
        case '*': return {{L,T+0.10f, R,M}, {R,T+0.10f, L,M}, {C,T+0.04f, C,M+0.06f}};
        case '/': return {{L,B, R,T}};
        case '\\':return {{L,T, R,B}};
        case '|': return {{C,T, C,B}};
        case '(': return {{R-0.04f,T, L+0.04f,M}, {L+0.04f,M, R-0.04f,B}};
        case ')': return {{L+0.04f,T, R-0.04f,M}, {R-0.04f,M, L+0.04f,B}};
        case '[': return {{R-0.04f,T, L+0.06f,T}, {L+0.06f,T, L+0.06f,B}, {L+0.06f,B, R-0.04f,B}};
        case ']': return {{L+0.04f,T, R-0.06f,T}, {R-0.06f,T, R-0.06f,B}, {R-0.06f,B, L+0.04f,B}};
        case '{': return {{R-0.04f,T, C-0.02f,T+0.08f}, {C-0.02f,T+0.08f, C-0.02f,M}, {C-0.02f,M, L+0.04f,M+0.06f},
                          {L+0.04f,M+0.06f, C-0.02f,M+0.12f}, {C-0.02f,M+0.12f, C-0.02f,B-0.08f}, {C-0.02f,B-0.08f, R-0.04f,B}};
        case '}': return {{L+0.04f,T, C+0.02f,T+0.08f}, {C+0.02f,T+0.08f, C+0.02f,M}, {C+0.02f,M, R-0.04f,M+0.06f},
                          {R-0.04f,M+0.06f, C+0.02f,M+0.12f}, {C+0.02f,M+0.12f, C+0.02f,B-0.08f}, {C+0.02f,B-0.08f, L+0.04f,B}};
        case '<': return {{R,T+0.10f, L,M+0.10f}, {L,M+0.10f, R,B-0.04f}};
        case '>': return {{L,T+0.10f, R,M+0.10f}, {R,M+0.10f, L,B-0.04f}};
        case '!': return {{C,T, C,M+0.20f}, {C-0.02f,B-0.03f, C+0.02f,B}};
        case '?': return {{L,T+0.08f, C,T}, {C,T, R,T+0.12f}, {R,T+0.12f, C,M+0.16f},
                          {C-0.02f,B-0.03f, C+0.02f,B}};
        case '#': return {{L+0.08f,T, L+0.02f,B}, {R-0.06f,T, R-0.12f,B}, {L,M, R,M-0.04f}, {L,M+0.16f, R,M+0.12f}};
        case '@': return {{R,M+0.10f, C,M}, {C,M, L+0.06f,M+0.10f}, {L+0.06f,M+0.10f, C,B-0.06f},
                          {C,B-0.06f, R,M+0.10f}, {R,M+0.10f, R,T+0.14f}, {R,T+0.14f, C,T}, {C,T, L,M},
                          {L,M, C,B}, {C,B, R,B-0.06f}};
        case '%': return {{R,T, L,B}, {L,T, L+0.10f,T+0.12f}, {L+0.10f,T+0.12f, L,T+0.20f}, {L,T+0.20f, L,T},
                          {R-0.10f,B-0.20f, R,B-0.12f}, {R,B-0.12f, R-0.10f,B}, {R-0.10f,B, R-0.10f,B-0.20f}};
        case '&': return {{R,B, L,M+0.06f}, {L,M+0.06f, C,T}, {C,T, R-0.10f,T+0.10f},
                          {R-0.10f,T+0.10f, L,B-0.06f}, {L,B-0.06f, C+0.06f,B}, {C+0.06f,B, R,M+0.14f}};
        case '$': return {{R,T+0.06f, C,T+0.02f}, {C,T+0.02f, L,T+0.14f}, {L,T+0.14f, R,M+0.10f},
                          {R,M+0.10f, R,B-0.10f}, {R,B-0.10f, L,B-0.04f}, {C,T-0.04f, C,B+0.04f}};
        case '\'':return {{C,T, C,T+0.10f}};
        case '"': return {{C-0.06f,T, C-0.06f,T+0.10f}, {C+0.06f,T, C+0.06f,T+0.10f}};
        case '~': return {{L,M+0.08f, C-0.06f,M+0.02f}, {C-0.06f,M+0.02f, C+0.06f,M+0.14f}, {C+0.06f,M+0.14f, R,M+0.08f}};
        case '^': return {{L,T+0.16f, C,T}, {C,T, R,T+0.16f}};
        case '`': return {{C-0.04f,T, C+0.04f,T+0.08f}};
        default:  return {};   // space and unknowns draw nothing
    }
}

}  // namespace builtin

// ── the font ────────────────────────────────────────────────────────────

/// A font binds three things the rest of mayag needs: a measurer (for layout),
/// a glyph renderer (for painting), and a coverage sampler (for the software
/// backend's atlas path). The built-in stroke font needs no atlas, so its
/// sampler is a stub.
class Font {
  public:
    /// The dependency-free built-in. Always succeeds.
    [[nodiscard]] static const Font& builtin_font() {
        static const Font f{};
        return f;
    }

    [[nodiscard]] const layout::TextMeasurer& measurer() const noexcept { return measurer_; }
    [[nodiscard]] const render::GlyphRenderer& glyph_renderer() const noexcept { return renderer_; }
    [[nodiscard]] const backend::CoverageSampler& sampler() const noexcept { return sampler_; }

  private:
    /// Draws each glyph as capsules through the shared SDF. Stroke weight is
    /// derived from the font weight, so `bold` is a real weight axis rather
    /// than a second set of outlines.
    class StrokeRenderer final : public render::GlyphRenderer {
      public:
        void draw_text(DrawList& dl, std::string_view s, const Rect& box,
                       const TextStyle& st) const override {
            const float em      = st.size;
            const float advance = em * 0.6f + st.letter_spacing;
            const float line_h  = st.line_advance();
            const float weight  = static_cast<float>(st.weight);
            // 400 -> ~7% of em, 700 -> ~11%. Clamped so tiny text stays legible
            // and huge display text does not turn into a blob.
            const float thickness = num::clamp(em * (0.055f + weight * 0.00008f), 0.8f, em * 0.18f);

            // Wrap to the box, mirroring MonospaceMetrics so painted text lands
            // exactly where layout reserved space.
            const auto cols = (st.overflow == TextOverflow::wrap && box.size.x > 0.0f)
                ? num::max(static_cast<int>(box.size.x / num::max(advance, 0.001f)), 1)
                : (1 << 20);

            float pen_x = box.left();
            float pen_y = box.top();
            int   col = 0;

            for (std::size_t i = 0; i < s.size(); ++i) {
                const char ch = s[i];
                if (ch == '\n') { pen_x = box.left(); pen_y += line_h; col = 0; continue; }
                if (col >= cols) { pen_x = box.left(); pen_y += line_h; col = 0; }

                if (ch != ' ') draw_glyph(dl, ch, Vec2{pen_x, pen_y}, em, thickness, st.color);

                pen_x += advance;
                ++col;
            }

            if (st.underline) {
                const float y = pen_y + em * 0.86f;
                dl.line({box.left(), y}, {pen_x, y}, num::max(thickness * 0.7f, 1.0f), st.color);
            }
            if (st.strikethrough) {
                const float y = pen_y + em * 0.52f;
                dl.line({box.left(), y}, {pen_x, y}, num::max(thickness * 0.7f, 1.0f), st.color);
            }
        }

      private:
        static void draw_glyph(DrawList& dl, char ch, Vec2 origin, float em,
                               float thickness, Color<Srgb> color) {
            const auto g = builtin::glyph_for(ch);
            for (std::uint8_t i = 0; i < g.count; ++i) {
                const Seg& sg = g.segs[i];
                dl.line(origin + Vec2{sg.x0 * em, sg.y0 * em},
                        origin + Vec2{sg.x1 * em, sg.y1 * em},
                        thickness, color);
            }
        }
    };

    /// The stroke font paints geometry, never an atlas, so nothing to sample.
    class NullSampler final : public backend::CoverageSampler {
      public:
        [[nodiscard]] float sample(std::uint32_t, float, float) const override { return 0.0f; }
    };

    layout::MonospaceMetrics measurer_{0.6f, 0.8f};
    StrokeRenderer           renderer_{};
    NullSampler              sampler_{};
};

/// Convenience: the built-in stroke font.
[[nodiscard]] inline const Font& builtin_font() { return Font::builtin_font(); }

}  // namespace mayag::fonts
