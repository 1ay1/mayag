#pragma once
// mayag::typo::shape — text to positioned glyphs
//
// Shaping answers: given a string and a face, WHICH glyphs, and WHERE?
//
// mayag ships a correct, fast shaper for the scripts that need no reordering
// — Latin, Greek, Cyrillic, CJK, and anything else whose glyphs run in
// logical order. That covers the overwhelming majority of UI text, and it
// includes the parts people actually notice: real kerning from `kern` and
// `GPOS`, ligatures, cluster-correct cursor movement, and grapheme
// boundaries so an emoji with a skin-tone modifier is one cursor stop.
//
// It does NOT implement Arabic joining, Indic reordering, or BiDi. Those are
// not "more of the same" — each is a distinct algorithm with a decade of
// conformance work behind it, and a half-implementation is worse than none
// because it fails silently on text the developer cannot read.
//
// So this file defines a `Shaper` interface, provides `SimpleShaper` as the
// dependency-free default, and lets `MAYAG_WITH_HARFBUZZ` swap in HarfBuzz
// for complex scripts. The atlas, the SDF pipeline, and the draw path are
// identical either way.

#include "opentype.hpp"
#include "../core/geometry.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace mayag::typo {

// ════════════════════════════════════════════════════════════════════════
// UTF-8
// ════════════════════════════════════════════════════════════════════════

namespace utf8 {

/// Decode one codepoint. Advances `i`. Malformed sequences yield U+FFFD and
/// consume exactly one byte, which guarantees forward progress on garbage
/// input rather than an infinite loop.
[[nodiscard]] inline std::uint32_t decode(std::string_view s, std::size_t& i) noexcept {
    if (i >= s.size()) return 0;

    const auto b0 = static_cast<unsigned char>(s[i]);

    if (b0 < 0x80) { ++i; return b0; }

    const auto cont = [&](std::size_t k) -> bool {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    const auto at = [&](std::size_t k) -> std::uint32_t {
        return static_cast<unsigned char>(s[i + k]) & 0x3F;
    };

    if ((b0 & 0xE0) == 0xC0 && cont(1)) {
        const std::uint32_t cp = ((b0 & 0x1Fu) << 6) | at(1);
        i += 2;
        return cp < 0x80 ? 0xFFFD : cp;          // reject overlong
    }
    if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        const std::uint32_t cp = ((b0 & 0x0Fu) << 12) | (at(1) << 6) | at(2);
        i += 3;
        // Reject overlongs and lone surrogates.
        return (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) ? 0xFFFD : cp;
    }
    if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        const std::uint32_t cp = ((b0 & 0x07u) << 18) | (at(1) << 12) | (at(2) << 6) | at(3);
        i += 4;
        return (cp < 0x10000 || cp > 0x10FFFF) ? 0xFFFD : cp;
    }

    ++i;
    return 0xFFFD;
}

[[nodiscard]] inline std::size_t count(std::string_view s) noexcept {
    std::size_t n = 0, i = 0;
    while (i < s.size()) { (void)decode(s, i); ++n; }
    return n;
}

inline void encode(std::uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace utf8

// ════════════════════════════════════════════════════════════════════════
// Unicode properties (the subset layout needs)
// ════════════════════════════════════════════════════════════════════════

namespace uni {

/// Combining marks attach to the preceding base character and must not start
/// a new grapheme cluster or a new line.
[[nodiscard]] constexpr bool is_combining_mark(std::uint32_t cp) noexcept {
    return (cp >= 0x0300 && cp <= 0x036F) ||   // combining diacriticals
           (cp >= 0x0483 && cp <= 0x0489) ||   // Cyrillic
           (cp >= 0x0591 && cp <= 0x05BD) ||   // Hebrew points
           (cp >= 0x0610 && cp <= 0x061A) ||   // Arabic
           (cp >= 0x064B && cp <= 0x065F) ||
           (cp >= 0x0670 && cp <= 0x0670) ||
           (cp >= 0x06D6 && cp <= 0x06DC) ||
           (cp >= 0x0900 && cp <= 0x0903) ||   // Devanagari
           (cp >= 0x093A && cp <= 0x094F) ||
           (cp >= 0x0E31 && cp <= 0x0E31) ||   // Thai
           (cp >= 0x0E34 && cp <= 0x0E3A) ||
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20F0) ||   // combining symbols
           (cp >= 0xFE20 && cp <= 0xFE2F);
}

/// Zero-width joiner — glues emoji into a single cluster (👨‍👩‍👧 is ONE grapheme).
[[nodiscard]] constexpr bool is_zwj(std::uint32_t cp) noexcept { return cp == 0x200D; }

/// Emoji skin tone modifiers.
[[nodiscard]] constexpr bool is_emoji_modifier(std::uint32_t cp) noexcept {
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

/// Variation selectors (VS15/VS16 pick text vs emoji presentation).
[[nodiscard]] constexpr bool is_variation_selector(std::uint32_t cp) noexcept {
    return (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

/// Regional indicators pair up into flags.
[[nodiscard]] constexpr bool is_regional_indicator(std::uint32_t cp) noexcept {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

/// CJK, Hangul, Kana — break freely between characters, no spaces needed.
[[nodiscard]] constexpr bool is_ideographic(std::uint32_t cp) noexcept {
    return (cp >= 0x1100  && cp <= 0x11FF)  ||  // Hangul Jamo
           (cp >= 0x2E80  && cp <= 0x2EFF)  ||  // CJK radicals
           (cp >= 0x3000  && cp <= 0x303F)  ||  // CJK punctuation
           (cp >= 0x3040  && cp <= 0x30FF)  ||  // Kana
           (cp >= 0x3400  && cp <= 0x4DBF)  ||  // CJK Ext A
           (cp >= 0x4E00  && cp <= 0x9FFF)  ||  // CJK Unified
           (cp >= 0xAC00  && cp <= 0xD7AF)  ||  // Hangul syllables
           (cp >= 0xF900  && cp <= 0xFAFF)  ||  // compatibility
           (cp >= 0xFF00  && cp <= 0xFF60)  ||  // fullwidth forms
           (cp >= 0x20000 && cp <= 0x2FA1F);    // CJK Ext B..F
}

/// Characters that must not START a line (closing brackets, CJK punctuation).
/// Line breaking that ignores this produces the classic "。at the start of a
/// line" bug in Japanese text.
[[nodiscard]] constexpr bool is_no_break_before(std::uint32_t cp) noexcept {
    switch (cp) {
        case ')': case ']': case '}': case ',': case '.': case ';': case ':':
        case '!': case '?': case '%':
        case 0x3001: case 0x3002:                        // 、。
        case 0x300D: case 0x300F: case 0x3011:           // 」』】
        case 0xFF01: case 0xFF09: case 0xFF0C:           // ！），
        case 0xFF1A: case 0xFF1B: case 0xFF1F:           // ：；？
            return true;
        default:
            return false;
    }
}

/// Characters that must not END a line (opening brackets).
[[nodiscard]] constexpr bool is_no_break_after(std::uint32_t cp) noexcept {
    switch (cp) {
        case '(': case '[': case '{':
        case 0x300C: case 0x300E: case 0x3010:           // 「『【
        case 0xFF08:                                     // （
            return true;
        default:
            return false;
    }
}

[[nodiscard]] constexpr bool is_space(std::uint32_t cp) noexcept {
    return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

/// Right-to-left scripts. mayag detects them so it can WARN rather than
/// silently render them in the wrong order — see `ShapeResult::has_rtl`.
[[nodiscard]] constexpr bool is_rtl(std::uint32_t cp) noexcept {
    return (cp >= 0x0590 && cp <= 0x05FF) ||   // Hebrew
           (cp >= 0x0600 && cp <= 0x06FF) ||   // Arabic
           (cp >= 0x0700 && cp <= 0x074F) ||   // Syriac
           (cp >= 0x0780 && cp <= 0x07BF) ||   // Thaana
           (cp >= 0x08A0 && cp <= 0x08FF) ||   // Arabic Ext-A
           (cp >= 0xFB1D && cp <= 0xFDFF) ||   // Hebrew/Arabic presentation
           (cp >= 0xFE70 && cp <= 0xFEFF);
}

/// Scripts requiring contextual reordering or joining — the ones the simple
/// shaper cannot render correctly.
[[nodiscard]] constexpr bool needs_complex_shaping(std::uint32_t cp) noexcept {
    return (cp >= 0x0600 && cp <= 0x06FF) ||   // Arabic
           (cp >= 0x0900 && cp <= 0x0DFF) ||   // Indic
           (cp >= 0x0E00 && cp <= 0x0E7F) ||   // Thai
           (cp >= 0x1000 && cp <= 0x109F) ||   // Myanmar
           (cp >= 0x1780 && cp <= 0x17FF) ||   // Khmer
           (cp >= 0xFE70 && cp <= 0xFEFF);
}

}  // namespace uni

// ════════════════════════════════════════════════════════════════════════
// Kerning
// ════════════════════════════════════════════════════════════════════════

/// Pair kerning from `kern` (format 0) and `GPOS` (lookup type 2).
///
/// Kerning is what separates typeset text from a monospaced grid: without it
/// "AV" and "To" have visibly wrong gaps. Both table formats are read into
/// one sorted flat map, so lookup is a single binary search regardless of
/// which the font shipped.
class KernTable {
  public:
    KernTable() = default;

    explicit KernTable(const ot::FontFile& f) {
        parse_kern(f);
        if (pairs_.empty()) parse_gpos(f);
        std::sort(pairs_.begin(), pairs_.end(),
                  [](const Pair& a, const Pair& b) { return a.key < b.key; });
    }

    [[nodiscard]] bool empty() const noexcept { return pairs_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return pairs_.size(); }

    /// Kerning adjustment in font units, applied after `left`'s advance.
    [[nodiscard]] float lookup(std::uint16_t left, std::uint16_t right) const noexcept {
        if (pairs_.empty()) return 0.0f;
        const std::uint32_t key = (static_cast<std::uint32_t>(left) << 16) | right;
        auto it = std::lower_bound(pairs_.begin(), pairs_.end(), key,
                                   [](const Pair& p, std::uint32_t k) { return p.key < k; });
        return (it != pairs_.end() && it->key == key) ? it->value : 0.0f;
    }

  private:
    struct Pair {
        std::uint32_t key;     ///< left << 16 | right
        float         value;
    };

    void parse_kern(const ot::FontFile& f) {
        const auto data = f.table(ot::tags::kern);
        if (data.size() < 4) return;

        ot::Reader r{data};
        // Two incompatible headers share this tag: Microsoft's (version 0,
        // u16 counts) and Apple's (version 1.0 as 32-bit fixed). Distinguish
        // by the first u16.
        const std::uint16_t version = r.u16_at(0);
        std::size_t pos;
        std::uint32_t n_tables;

        if (version == 0) {
            n_tables = r.u16_at(2);
            pos = 4;
        } else {
            n_tables = r.u32_at(4);
            pos = 8;
        }

        for (std::uint32_t t = 0; t < n_tables && pos + 6 <= data.size(); ++t) {
            const std::uint32_t length = (version == 0) ? r.u16_at(pos + 2) : r.u32_at(pos + 4);
            const std::uint16_t coverage = (version == 0) ? r.u16_at(pos + 4) : r.u16_at(pos + 8);
            const std::size_t   sub = (version == 0) ? pos + 6 : pos + 8;

            // Format 0 (in the low byte for MS, high byte for Apple) is the
            // only one worth supporting; formats 1-3 are Apple state machines.
            const bool horizontal = (version == 0) ? ((coverage & 0x0001) != 0) : true;
            const std::uint8_t format = (version == 0) ? static_cast<std::uint8_t>(coverage >> 8)
                                                       : static_cast<std::uint8_t>(coverage & 0xFF);

            if (horizontal && format == 0) {
                const std::uint16_t n_pairs = r.u16_at(sub);
                pairs_.reserve(pairs_.size() + n_pairs);
                for (std::uint16_t i = 0; i < n_pairs; ++i) {
                    const std::size_t p = sub + 8 + static_cast<std::size_t>(i) * 6;
                    if (p + 6 > data.size()) break;
                    pairs_.push_back(Pair{
                        (static_cast<std::uint32_t>(r.u16_at(p)) << 16) | r.u16_at(p + 2),
                        static_cast<float>(r.i16_at(p + 4))});
                }
            }

            if (length == 0) break;
            pos += length;
        }
    }

    /// `GPOS` lookup type 2 — the modern kerning path. Handles both format 1
    /// (explicit glyph pairs) and format 2 (class-based, which is how a font
    /// kerns 400 glyph pairs with 20 classes).
    void parse_gpos(const ot::FontFile& f) {
        const auto data = f.table(ot::tags::gpos);
        if (data.size() < 10) return;

        ot::Reader r{data};
        const std::uint16_t lookup_list_off = r.u16_at(8);
        if (lookup_list_off == 0 || lookup_list_off >= data.size()) return;

        const std::uint16_t n_lookups = r.u16_at(lookup_list_off);
        for (std::uint16_t i = 0; i < n_lookups; ++i) {
            const std::size_t off = lookup_list_off + r.u16_at(lookup_list_off + 2 + i * 2);
            if (off + 6 > data.size()) continue;
            if (r.u16_at(off) != 2) continue;         // not a pair adjustment

            const std::uint16_t n_subs = r.u16_at(off + 4);
            for (std::uint16_t s = 0; s < n_subs; ++s) {
                parse_gpos_pair(r, off + r.u16_at(off + 6 + s * 2), data.size());
            }
        }
    }

    void parse_gpos_pair(ot::Reader& r, std::size_t off, std::size_t limit) {
        if (off + 10 > limit) return;

        const std::uint16_t format      = r.u16_at(off);
        const std::uint16_t coverage_off = r.u16_at(off + 2);
        const std::uint16_t value1 = r.u16_at(off + 4);
        const std::uint16_t value2 = r.u16_at(off + 6);

        // We only handle the common case: X advance on the first glyph, and
        // nothing on the second. Anything else is vertical or mark
        // positioning, which this shaper does not do.
        if (value1 != 0x0004 || value2 != 0) return;

        const auto coverage = read_coverage(r, off + coverage_off, limit);
        if (coverage.empty()) return;

        if (format == 1) {
            const std::uint16_t n_sets = r.u16_at(off + 8);
            for (std::uint16_t i = 0; i < n_sets && i < coverage.size(); ++i) {
                const std::size_t set = off + r.u16_at(off + 10 + i * 2);
                if (set + 2 > limit) continue;
                const std::uint16_t n_pairs = r.u16_at(set);
                for (std::uint16_t k = 0; k < n_pairs; ++k) {
                    const std::size_t rec = set + 2 + static_cast<std::size_t>(k) * 4;
                    if (rec + 4 > limit) break;
                    pairs_.push_back(Pair{
                        (static_cast<std::uint32_t>(coverage[i]) << 16) | r.u16_at(rec),
                        static_cast<float>(r.i16_at(rec + 2))});
                }
            }
        }
        else if (format == 2) {
            const std::uint16_t class1_off = r.u16_at(off + 8);
            const std::uint16_t class2_off = r.u16_at(off + 10);
            const std::uint16_t n_class1   = r.u16_at(off + 12);
            const std::uint16_t n_class2   = r.u16_at(off + 14);
            if (n_class1 == 0 || n_class2 == 0) return;

            const auto class1 = read_class_def(r, off + class1_off, limit);
            const auto class2 = read_class_def(r, off + class2_off, limit);

            // Class-based kerning expands to (glyphs in class1) x (class2)
            // pairs. Guard against a font whose classes would produce
            // millions of entries.
            if (static_cast<std::size_t>(n_class1) * n_class2 > 65536) return;

            for (std::uint16_t g : coverage) {
                const auto c1 = class1.count(g) ? class1.at(g) : 0;
                if (c1 >= n_class1) continue;
                for (const auto& [g2, c2] : class2) {
                    if (c2 >= n_class2) continue;
                    const std::size_t rec = off + 16 +
                        (static_cast<std::size_t>(c1) * n_class2 + c2) * 2;
                    if (rec + 2 > limit) continue;
                    const float v = static_cast<float>(r.i16_at(rec));
                    if (v != 0.0f) {
                        pairs_.push_back(Pair{
                            (static_cast<std::uint32_t>(g) << 16) | g2, v});
                    }
                }
            }
        }
    }

    [[nodiscard]] static std::vector<std::uint16_t> read_coverage(const ot::Reader& r,
                                                                  std::size_t off,
                                                                  std::size_t limit) {
        std::vector<std::uint16_t> out;
        if (off + 4 > limit) return out;

        const std::uint16_t format = r.u16_at(off);
        const std::uint16_t count  = r.u16_at(off + 2);

        if (format == 1) {
            out.reserve(count);
            for (std::uint16_t i = 0; i < count; ++i) out.push_back(r.u16_at(off + 4 + i * 2));
        } else if (format == 2) {
            for (std::uint16_t i = 0; i < count; ++i) {
                const std::size_t rec = off + 4 + static_cast<std::size_t>(i) * 6;
                const std::uint16_t start = r.u16_at(rec);
                const std::uint16_t end   = r.u16_at(rec + 2);
                if (end < start || static_cast<std::size_t>(end - start) > 0xFFFF) continue;
                for (std::uint32_t g = start; g <= end; ++g) {
                    out.push_back(static_cast<std::uint16_t>(g));
                }
            }
        }
        return out;
    }

    [[nodiscard]] static std::unordered_map<std::uint16_t, std::uint16_t>
    read_class_def(const ot::Reader& r, std::size_t off, std::size_t limit) {
        std::unordered_map<std::uint16_t, std::uint16_t> out;
        if (off + 4 > limit) return out;

        const std::uint16_t format = r.u16_at(off);
        if (format == 1) {
            const std::uint16_t start = r.u16_at(off + 2);
            const std::uint16_t count = r.u16_at(off + 4);
            for (std::uint16_t i = 0; i < count; ++i) {
                const std::uint16_t c = r.u16_at(off + 6 + i * 2);
                if (c != 0) out[static_cast<std::uint16_t>(start + i)] = c;
            }
        } else if (format == 2) {
            const std::uint16_t n_ranges = r.u16_at(off + 2);
            for (std::uint16_t i = 0; i < n_ranges; ++i) {
                const std::size_t rec = off + 4 + static_cast<std::size_t>(i) * 6;
                const std::uint16_t start = r.u16_at(rec);
                const std::uint16_t end   = r.u16_at(rec + 2);
                const std::uint16_t c     = r.u16_at(rec + 4);
                if (c == 0 || end < start) continue;
                for (std::uint32_t g = start; g <= end; ++g) {
                    out[static_cast<std::uint16_t>(g)] = c;
                }
            }
        }
        return out;
    }

    std::vector<Pair> pairs_;
};

// ════════════════════════════════════════════════════════════════════════
// Shaping results
// ════════════════════════════════════════════════════════════════════════

/// One positioned glyph.
struct ShapedGlyph {
    std::uint16_t glyph_id = 0;
    /// Byte offset in the source string this glyph came from. Multiple glyphs
    /// can share a cluster (a ligature, or a base + combining mark); cursor
    /// movement and selection must operate on clusters, not glyphs.
    std::uint32_t cluster  = 0;
    /// Pen advance after this glyph, in pixels.
    float         advance  = 0.0f;
    /// Positional adjustment from the pen origin, in pixels.
    Vec2          offset{};
    /// Which face in the fallback chain produced this glyph. Index 0 is the
    /// primary font; anything else came from a fallback.
    std::uint8_t  face_index = 0;
    /// True when no face in the chain had this codepoint — drawn as .notdef.
    bool          missing = false;
};

struct ShapeResult {
    std::vector<ShapedGlyph> glyphs;
    /// Total advance width in pixels.
    float width = 0.0f;
    /// True when the text contains right-to-left characters. The simple
    /// shaper renders these in LOGICAL order, which is wrong — this flag lets
    /// a caller detect the situation and enable the HarfBuzz path.
    bool  has_rtl = false;
    /// True when the text contains scripts needing contextual shaping.
    bool  needs_complex = false;

    [[nodiscard]] bool empty() const noexcept { return glyphs.empty(); }

    /// Byte offset of the cluster nearest to `x` pixels — hit testing for
    /// cursor placement.
    [[nodiscard]] std::uint32_t cluster_at(float x) const noexcept {
        float pen = 0.0f;
        for (const auto& g : glyphs) {
            if (x < pen + g.advance * 0.5f) return g.cluster;
            pen += g.advance;
        }
        return glyphs.empty() ? 0 : glyphs.back().cluster + 1;
    }

    /// X position of a byte offset — where to draw the caret.
    [[nodiscard]] float x_of_cluster(std::uint32_t cluster) const noexcept {
        float pen = 0.0f;
        for (const auto& g : glyphs) {
            if (g.cluster >= cluster) return pen;
            pen += g.advance;
        }
        return pen;
    }
};

// ════════════════════════════════════════════════════════════════════════
// Shaper interface
// ════════════════════════════════════════════════════════════════════════

/// Per-run shaping inputs.
struct ShapeParams {
    float size_px       = 16.0f;
    float letter_spacing = 0.0f;
    bool  kerning       = true;
    bool  ligatures     = true;
};

/// The seam. `SimpleShaper` below is the default; a HarfBuzz-backed
/// implementation satisfies the same interface and is selected at build time.
class Shaper {
  public:
    virtual ~Shaper() = default;
    [[nodiscard]] virtual ShapeResult shape(std::string_view text,
                                            const ShapeParams& params) const = 0;
};

}  // namespace mayag::typo
