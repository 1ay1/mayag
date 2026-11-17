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
#include "variations.hpp"
#include "../core/geometry.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <array>
#include <string_view>
#include <unordered_map>
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
/// "AV" and "To" have visibly wrong gaps. Lookups read the font's own tables
/// in place (binary searches over its sorted arrays), so a face costs nothing
/// to open: class kerning used to be expanded into a million sorted pairs.
/// The table bytes belong to the Face, which outlives this.
class KernTable {
  public:
    KernTable() = default;

    /// GPOS wins over `kern` when it has pair kerning, as in HarfBuzz and
    /// CoreText: modern fonts carry a legacy `kern` only for old software,
    /// and it lacks class pairs and variation deltas.
    explicit KernTable(const ot::FontFile& f) {
        find_gpos(f);
        if (lookups_.empty()) kern_ = find_kern(f);
    }

    [[nodiscard]] bool empty() const noexcept { return kern_.empty() && lookups_.empty(); }

    /// Kerning adjustment in font units, applied after `left`'s advance.
    /// `coords` (normalised axis position) selects a variable font's
    /// instance: SF and its kin store kerning as a default value plus a
    /// per-instance delta, so the same pair kerns differently at 13 pt and
    /// at 28 pt, and at Regular and at Bold.
    [[nodiscard]] float lookup(std::uint16_t left, std::uint16_t right,
                               const std::vector<float>& coords = {}) const noexcept {
        if (!kern_.empty()) return static_cast<float>(lookup_kern(left, right));
        const ot::Reader r{gpos_};
        float total = 0.0f;
        // Every kern lookup applies; within one, the first subtable that
        // covers the pair decides.
        for (const auto& subs : lookups_) {
            for (const Sub& s : subs) {
                const auto ci = coverage_index(r, s.off + r.u16_at(s.off + 2), left);
                if (ci < 0) continue;
                std::size_t rec  = 0;       // byte offset of value1, 0 = no pair here
                std::size_t base = s.off;   // what the record's device offsets count from
                if (r.u16_at(s.off) == 1) {
                    rec = pair_set_record(r, s, static_cast<std::uint16_t>(ci), right, base);
                    if (rec == 0) continue;   // covered, but not this pair: next subtable
                } else {
                    const std::uint16_t n1 = r.u16_at(s.off + 12), n2 = r.u16_at(s.off + 14);
                    const std::uint16_t c1 = class_of(r, s.off + r.u16_at(s.off + 8), left);
                    const std::uint16_t c2 = class_of(r, s.off + r.u16_at(s.off + 10), right);
                    if (c1 >= n1 || c2 >= n2) break;
                    rec = s.off + 16 + (static_cast<std::size_t>(c1) * n2 + c2) * (s.size1 + s.size2);
                }
                total += static_cast<float>(r.i16_at(rec + s.x_adv));
                if (s.x_adv_dev != 0 && !coords.empty()) {
                    const std::uint16_t dev = r.u16_at(rec + s.x_adv_dev);
                    if (dev != 0) total += device_delta(r, base + dev, coords);
                }
                break;
            }
        }
        return total;
    }

  private:
    /// One usable PairPos subtable and where XAdvance sits in its records.
    struct Sub {
        std::size_t   off = 0;        ///< subtable offset within GPOS
        std::uint8_t  size1 = 0;      ///< bytes in value record 1
        std::uint8_t  size2 = 0;      ///< bytes in value record 2
        std::uint8_t  x_adv = 0;      ///< XAdvance offset inside value record 1
        std::uint8_t  x_adv_dev = 0;  ///< XAdvDevice offset inside record 1; 0 = absent
    };

    // `kern` format 0: pairs are sorted by (left, right) per the spec.
    std::span<const std::uint8_t> kern_;      ///< the pair array, 6 bytes each
    std::span<const std::uint8_t> gpos_;      ///< the whole GPOS table
    std::vector<std::vector<Sub>> lookups_;   ///< kern-feature PairPos lookups, in lookup order
    std::span<const std::uint8_t> gdef_;      ///< GDEF, home of the variation store
    std::size_t                   ivs_ = 0;   ///< item variation store offset in GDEF; 0 = none

    [[nodiscard]] static std::span<const std::uint8_t> find_kern(const ot::FontFile& f) {
        const auto data = f.table(ot::tags::kern);
        if (data.size() < 4) return {};
        const ot::Reader r{data};
        // Microsoft's header (version 0, u16 counts) or Apple's (32-bit 1.0).
        const std::uint16_t version = r.u16_at(0);
        std::size_t pos = version == 0 ? 4 : 8;
        const std::uint32_t n_tables = version == 0 ? r.u16_at(2) : r.u32_at(4);
        for (std::uint32_t t = 0; t < n_tables && pos + 6 <= data.size(); ++t) {
            const std::uint32_t length = version == 0 ? r.u16_at(pos + 2) : r.u32_at(pos + 4);
            const std::uint16_t coverage = version == 0 ? r.u16_at(pos + 4) : r.u16_at(pos + 8);
            const std::size_t sub = version == 0 ? pos + 6 : pos + 8;
            const bool horizontal = version == 0 ? (coverage & 0x0001) != 0 : true;
            const std::uint8_t format = version == 0 ? static_cast<std::uint8_t>(coverage >> 8)
                                                     : static_cast<std::uint8_t>(coverage & 0xFF);
            // Format 0 is the only one worth supporting; 1-3 are Apple state machines.
            if (horizontal && format == 0 && sub + 8 <= data.size()) {
                const std::size_t n = r.u16_at(sub);
                const std::size_t first = sub + 8;
                const std::size_t avail = (data.size() - first) / 6;
                return data.subspan(first, std::min(n, avail) * 6);
            }
            if (length == 0) break;
            pos += length;
        }
        return {};
    }

    [[nodiscard]] std::int16_t lookup_kern(std::uint16_t left, std::uint16_t right) const noexcept {
        const ot::Reader r{kern_};
        const std::uint32_t key = std::uint32_t{left} << 16 | right;
        std::size_t lo = 0, hi = kern_.size() / 6;
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            const std::uint32_t k = std::uint32_t{r.u16_at(mid * 6)} << 16 | r.u16_at(mid * 6 + 2);
            if (k == key) return r.i16_at(mid * 6 + 4);
            if (k < key) lo = mid + 1; else hi = mid;
        }
        return 0;
    }

    /// Collect the pair-adjustment lookups the `kern` feature names. Only
    /// value record 1's XAdvance (plus its variation delta) is applied: that
    /// is horizontal kerning, and it is all a pair lookup in a `kern`
    /// feature means in practice. Lookups wrapped in extension lookups (type
    /// 9) are unwrapped; big fonts such as SF put ALL their kerning there.
    void find_gpos(const ot::FontFile& f) {
        const auto data = f.table(ot::tags::gpos);
        if (data.size() < 10) return;
        const ot::Reader r{data};
        const std::size_t list = r.u16_at(8);
        if (list == 0 || list >= data.size()) return;
        const std::uint16_t n_lookups = r.u16_at(list);

        // Which lookups do `kern` features reference? A font's other pair
        // lookups (CJK proportional widths, capital spacing) are opt-in
        // features and must not apply by default.
        std::vector<bool> wanted(n_lookups, false);
        bool any_feature = false;
        if (const std::size_t feats = r.u16_at(6); feats != 0 && feats + 2 <= data.size()) {
            const std::uint16_t n_feats = r.u16_at(feats);
            for (std::uint16_t i = 0; i < n_feats; ++i) {
                const std::size_t recd = feats + 2 + static_cast<std::size_t>(i) * 6;
                if (recd + 6 > data.size() || r.u32_at(recd) != ot::tag("kern")) continue;
                const std::size_t feat = feats + r.u16_at(recd + 4);
                const std::uint16_t n = r.u16_at(feat + 2);
                for (std::uint16_t k = 0; k < n; ++k) {
                    const std::uint16_t li = r.u16_at(feat + 4 + static_cast<std::size_t>(k) * 2);
                    if (li < n_lookups) { wanted[li] = true; any_feature = true; }
                }
            }
        }

        for (std::uint16_t i = 0; i < n_lookups; ++i) {
            if (any_feature && !wanted[i]) continue;
            const std::size_t off = list + r.u16_at(list + 2 + static_cast<std::size_t>(i) * 2);
            if (off + 6 > data.size()) continue;
            const std::uint16_t type = r.u16_at(off);
            if (type != 2 && type != 9) continue;
            std::vector<Sub> subs;
            const std::uint16_t n_subs = r.u16_at(off + 4);
            for (std::uint16_t s = 0; s < n_subs; ++s) {
                std::size_t sub = off + r.u16_at(off + 6 + static_cast<std::size_t>(s) * 2);
                if (type == 9) {   // ExtensionPos: {format, lookup type, offset32}
                    if (sub + 8 > data.size() || r.u16_at(sub + 2) != 2) continue;
                    sub += r.u32_at(sub + 4);
                }
                if (sub + 16 > data.size()) continue;
                const std::uint16_t format = r.u16_at(sub);
                const std::uint16_t vf1 = r.u16_at(sub + 4), vf2 = r.u16_at(sub + 6);
                if ((format != 1 && format != 2) || (vf1 & 0x0004) == 0) continue;
                const auto bytes = [](std::uint16_t vf) {
                    return static_cast<std::uint8_t>(2 * std::popcount(static_cast<unsigned>(vf & 0xFF)));
                };
                Sub d;
                d.off   = sub;
                d.size1 = bytes(vf1);
                d.size2 = bytes(vf2);
                d.x_adv = bytes(vf1 & 0x0003);
                d.x_adv_dev = (vf1 & 0x0040) ? bytes(vf1 & 0x003F) : std::uint8_t{0};
                subs.push_back(d);
            }
            if (!subs.empty()) lookups_.push_back(std::move(subs));
        }
        if (lookups_.empty()) return;
        gpos_ = data;

        // GDEF 1.3+ holds the item variation store that device offsets index.
        const auto gdef = f.table(ot::tags::gdef);
        const ot::Reader g{gdef};
        if (gdef.size() >= 18 && g.u16_at(0) == 1 && g.u16_at(2) >= 3) {
            const std::uint32_t ivs = g.u32_at(14);
            if (ivs != 0 && ivs < gdef.size()) { gdef_ = gdef; ivs_ = ivs; }
        }
    }

    /// A ValueRecord device offset that is really a VariationIndex table
    /// (deltaFormat 0x8000): the delta the GDEF store holds for `coords`.
    /// Classic hinting Device tables are ppem-specific and ignored.
    [[nodiscard]] float device_delta(const ot::Reader& r, std::size_t dev,
                                     const std::vector<float>& coords) const noexcept {
        if (ivs_ == 0 || r.u16_at(dev + 4) != 0x8000) return 0.0f;
        return var::ivs_delta(gdef_, ivs_, r.u16_at(dev), r.u16_at(dev + 2), coords);
    }

    /// Index of `g` in a Coverage table, or -1.
    [[nodiscard]] static int coverage_index(const ot::Reader& r, std::size_t off, std::uint16_t g) noexcept {
        const std::uint16_t format = r.u16_at(off), count = r.u16_at(off + 2);
        std::size_t lo = 0, hi = count;
        if (format == 1) {
            while (lo < hi) {
                const std::size_t mid = (lo + hi) / 2;
                const std::uint16_t v = r.u16_at(off + 4 + mid * 2);
                if (v == g) return static_cast<int>(mid);
                if (v < g) lo = mid + 1; else hi = mid;
            }
        } else if (format == 2) {
            while (lo < hi) {
                const std::size_t mid = (lo + hi) / 2;
                const std::size_t rec = off + 4 + mid * 6;
                const std::uint16_t start = r.u16_at(rec), end = r.u16_at(rec + 2);
                if (g < start) hi = mid;
                else if (g > end) lo = mid + 1;
                else return r.u16_at(rec + 4) + (g - start);
            }
        }
        return -1;
    }

    /// Class of `g` in a ClassDef table; 0 when unlisted.
    [[nodiscard]] static std::uint16_t class_of(const ot::Reader& r, std::size_t off, std::uint16_t g) noexcept {
        const std::uint16_t format = r.u16_at(off);
        if (format == 1) {
            const std::uint16_t start = r.u16_at(off + 2), count = r.u16_at(off + 4);
            return g >= start && g - start < count ? r.u16_at(off + 6 + (g - start) * 2) : 0;
        }
        if (format == 2) {
            std::size_t lo = 0, hi = r.u16_at(off + 2);
            while (lo < hi) {
                const std::size_t mid = (lo + hi) / 2;
                const std::size_t rec = off + 4 + mid * 6;
                const std::uint16_t start = r.u16_at(rec), end = r.u16_at(rec + 2);
                if (g < start) hi = mid;
                else if (g > end) lo = mid + 1;
                else return r.u16_at(rec + 4);
            }
        }
        return 0;
    }

    /// Format 1: the PairSet for coverage index `ci`, searched for `right`.
    /// Returns the offset of the pair's value record 1, or 0 if absent, and
    /// sets `base` to the PairSet: a format-1 record's device offsets count
    /// from its PairSet, not from the subtable (as HarfBuzz and CoreText read
    /// it). Counting from the subtable read SF's comma-space delta from the
    /// wrong bytes and set every ", " 100 units tight at text sizes.
    [[nodiscard]] static std::size_t pair_set_record(const ot::Reader& r, const Sub& s,
                                                     std::uint16_t ci, std::uint16_t right,
                                                     std::size_t& base) noexcept {
        if (ci >= r.u16_at(s.off + 8)) return 0;
        const std::size_t set = s.off + r.u16_at(s.off + 10 + static_cast<std::size_t>(ci) * 2);
        base = set;
        const std::size_t stride = 2u + s.size1 + s.size2;
        std::size_t lo = 0, hi = r.u16_at(set);
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            const std::size_t rec = set + 2 + mid * stride;
            const std::uint16_t g = r.u16_at(rec);
            if (g == right) return rec + 2;
            if (g < right) lo = mid + 1; else hi = mid;
        }
        return 0;
    }
};

// ══ trak: size-dependent tracking ═════════════════════════════════

/// Apple's tracking table: how much to loosen or tighten spacing at each
/// point size. SF draws ONE outline for every size and relies on this to
/// space 11 pt captions open and 28 pt headlines tight. CoreText applies it
/// to every glyph; ignoring it set SF text 1.7 px too wide over a 23-glyph
/// 13 pt line and 5 px too narrow at 26 pt.
class TrackTable {
  public:
    TrackTable() = default;
    explicit TrackTable(const ot::FontFile& f) {
        const auto data = f.table(ot::tag("trak"));
        const ot::Reader r{data};
        if (data.size() < 12 || r.u32_at(0) != 0x00010000 || r.u16_at(4) != 0) return;
        const std::size_t horiz = r.u16_at(6);
        if (horiz == 0 || horiz + 8 > data.size()) return;
        const std::uint16_t n_tracks = r.u16_at(horiz);
        const std::uint16_t n_sizes  = r.u16_at(horiz + 2);
        const std::size_t   sizes    = r.u32_at(horiz + 4);
        if (n_sizes == 0 || sizes + std::size_t{n_sizes} * 4 > data.size()) return;
        for (std::uint16_t t = 0; t < n_tracks; ++t) {
            const std::size_t rec = horiz + 8 + std::size_t{t} * 8;
            if (rec + 8 > data.size() || r.u32_at(rec) != 0) continue;   // the NORMAL track (0.0)
            const std::size_t vals = r.u16_at(rec + 6);
            if (vals + std::size_t{n_sizes} * 2 > data.size()) return;
            for (std::uint16_t s = 0; s < n_sizes; ++s) {
                const auto fixed = static_cast<std::int32_t>(r.u32_at(sizes + std::size_t{s} * 4));
                points_.push_back({static_cast<float>(fixed) / 65536.0f,
                                   static_cast<float>(r.i16_at(vals + std::size_t{s} * 2))});
            }
            return;
        }
    }

    [[nodiscard]] bool empty() const noexcept { return points_.empty(); }

    /// Tracking in font units per glyph at `point_size`: linear between the
    /// table's sizes, held flat past either end, and FLOORED to whole font
    /// units. CoreText floors (measured: 67.3 -> 67, 52.7 -> 52, 3.5 -> 3),
    /// and matching it is what makes a line's width agree to the 1/1000 px.
    [[nodiscard]] float at(float point_size) const noexcept {
        if (points_.empty()) return 0.0f;
        if (point_size <= points_.front().size) return points_.front().value;
        for (std::size_t i = 1; i < points_.size(); ++i) {
            const Point& a = points_[i - 1];
            const Point& b = points_[i];
            if (point_size <= b.size) {
                const float t = (point_size - a.size) / (b.size - a.size);
                return std::floor(a.value + (b.value - a.value) * t + 1e-4f);
            }
        }
        return points_.back().value;
    }

  private:
    struct Point { float size = 0.0f; float value = 0.0f; };
    std::vector<Point> points_;
};

// ══ GSUB ligatures ════════════════════════════════════════════════

/// Ligature substitution from GSUB (`liga`, lookup type 4): fi -> ﬁ, ffi -> ﬃ,
/// and any font-defined multi-glyph -> single-glyph rule. The default `liga`
/// feature is on in virtually every text font, so this is the correct default,
/// not an opt-in nicety. Parsed once per face; applied as a greedy left-to-
/// right longest-match pass over a shaped run.
class LigatureTable {
  public:
    LigatureTable() = default;
    explicit LigatureTable(const ot::FontFile& f) { parse(f); }

    [[nodiscard]] bool empty() const noexcept { return sets_.empty(); }

    /// One ligature rule: the trailing components (after the first, which is
    /// the map key) and the glyph they combine into.
    struct Rule {
        std::vector<std::uint16_t> rest;   // components 2..N
        std::uint16_t lig = 0;
    };

    /// Longest ligature starting at glyphs[i] (gids), or 0 with count unset.
    /// On a hit, `consumed` is how many input glyphs the ligature replaces.
    [[nodiscard]] std::uint16_t match(const std::vector<std::uint16_t>& gids,
                                      std::size_t i, std::size_t& consumed) const {
        auto it = sets_.find(gids[i]);
        if (it == sets_.end()) return 0;
        std::uint16_t best_lig = 0;
        std::size_t   best_len = 0;
        for (const auto& rule : it->second) {
            const std::size_t need = rule.rest.size();
            if (i + 1 + need > gids.size()) continue;   // not enough glyphs left
            bool ok = true;
            for (std::size_t k = 0; k < need; ++k)
                if (gids[i + 1 + k] != rule.rest[k]) { ok = false; break; }
            if (ok && need + 1 > best_len) { best_len = need + 1; best_lig = rule.lig; }
        }
        if (best_lig != 0) { consumed = best_len; return best_lig; }
        return 0;
    }

  private:
    void parse(const ot::FontFile& f) {
        const auto data = f.table(ot::tags::gsub);
        if (data.size() < 10) return;
        ot::Reader r{data, 0};
        const std::uint16_t feature_list_off = r.u16_at(6);
        const std::uint16_t lookup_list_off  = r.u16_at(8);
        if (feature_list_off == 0 || lookup_list_off == 0) return;

        // Collect lookup indices referenced by any 'liga' feature.
        std::vector<std::uint16_t> liga_lookups;
        const std::uint16_t n_features = r.u16_at(feature_list_off);
        for (std::uint16_t i = 0; i < n_features; ++i) {
            const std::size_t rec = feature_list_off + 2 + static_cast<std::size_t>(i) * 6;
            const std::uint32_t ftag = r.u32_at(rec);   // FeatureRecord tag
            if (ftag != tag4('l', 'i', 'g', 'a')) continue;
            const std::size_t foff = feature_list_off + r.u16_at(rec + 4);
            const std::uint16_t n_idx = r.u16_at(foff + 2);
            for (std::uint16_t k = 0; k < n_idx; ++k)
                liga_lookups.push_back(r.u16_at(foff + 4 + k * 2));
        }
        if (liga_lookups.empty()) return;

        const std::uint16_t n_lookups = r.u16_at(lookup_list_off);
        for (std::uint16_t li : liga_lookups) {
            if (li >= n_lookups) continue;
            const std::size_t loff = lookup_list_off + r.u16_at(lookup_list_off + 2 + li * 2);
            const std::uint16_t type = r.u16_at(loff);
            const std::uint16_t n_subs = r.u16_at(loff + 4);
            for (std::uint16_t s = 0; s < n_subs; ++s) {
                const std::size_t sub = loff + r.u16_at(loff + 6 + s * 2);
                if (type == 4) parse_ligature_subst(r, sub, data.size());
                // (Extension lookup type 7 -> forwards to the real subtable.)
                else if (type == 7) {
                    const std::uint16_t ext_fmt = r.u16_at(sub);
                    const std::uint16_t ext_type = r.u16_at(sub + 2);
                    if (ext_fmt == 1 && ext_type == 4) {
                        const std::uint32_t delta = r.u32_at(sub + 4);
                        parse_ligature_subst(r, sub + delta, data.size());
                    }
                }
            }
        }
    }

    void parse_ligature_subst(ot::Reader& r, std::size_t off, std::size_t limit) {
        if (off + 6 > limit) return;
        const std::uint16_t format = r.u16_at(off);
        if (format != 1) return;
        const std::uint16_t cov_off = r.u16_at(off + 2);
        const std::uint16_t n_sets  = r.u16_at(off + 4);
        const auto cov = read_coverage(r, off + cov_off, limit);
        for (std::uint16_t i = 0; i < n_sets && i < cov.size(); ++i) {
            const std::size_t set = off + r.u16_at(off + 6 + i * 2);
            if (set + 2 > limit) continue;
            const std::uint16_t n_lig = r.u16_at(set);
            const std::uint16_t first = cov[i];
            for (std::uint16_t k = 0; k < n_lig; ++k) {
                const std::size_t lig = set + r.u16_at(set + 2 + k * 2);
                if (lig + 4 > limit) continue;
                const std::uint16_t lig_glyph = r.u16_at(lig);
                const std::uint16_t comp_count = r.u16_at(lig + 2);
                if (comp_count < 1 || comp_count > 32) continue;
                Rule rule;
                rule.lig = lig_glyph;
                // componentGlyphIDs lists components 2..N (first is `first`).
                for (std::uint16_t c = 1; c < comp_count; ++c)
                    rule.rest.push_back(r.u16_at(lig + 4 + (c - 1) * 2));
                sets_[first].push_back(std::move(rule));
            }
        }
    }

    [[nodiscard]] static std::uint32_t tag4(char a, char b, char c, char d) noexcept {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8)  |
                static_cast<std::uint32_t>(static_cast<unsigned char>(d));
    }

    [[nodiscard]] static std::vector<std::uint16_t> read_coverage(const ot::Reader& r,
                                                                  std::size_t off,
                                                                  std::size_t limit) {
        std::vector<std::uint16_t> out;
        if (off + 4 > limit) return out;
        const std::uint16_t format = r.u16_at(off);
        const std::uint16_t count  = r.u16_at(off + 2);
        if (format == 1) {
            for (std::uint16_t i = 0; i < count; ++i)
                out.push_back(r.u16_at(off + 4 + static_cast<std::size_t>(i) * 2));
        } else if (format == 2) {
            for (std::uint16_t i = 0; i < count; ++i) {
                const std::size_t rec = off + 4 + static_cast<std::size_t>(i) * 6;
                const std::uint16_t start = r.u16_at(rec);
                const std::uint16_t end   = r.u16_at(rec + 2);
                for (std::uint32_t g = start; g <= end && out.size() < 65536; ++g)
                    out.push_back(static_cast<std::uint16_t>(g));
            }
        }
        return out;
    }

    std::unordered_map<std::uint16_t, std::vector<Rule>> sets_;
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
    /// Unwrapped extent, filled by FontStack's cache: the widest line between
    /// explicit newlines, and how many lines there are. 0 lines = not filled.
    float widest_line = 0.0f;
    int   line_count = 0;

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
    float weight        = 400.0f;   ///< drives a variable font's wght axis
    bool  italic        = false;    ///< drive slnt/ital axis (variable fonts)

    /// Extra variable-font axis overrides beyond weight (opsz, wdth, slnt,
    /// custom). Kept as a tiny inline array so ShapeParams stays a value type
    /// with no dependency on the style layer.
    struct Axis { std::uint32_t tag = 0; float value = 0.0f; };
    static constexpr std::size_t max_axes = 4;
    std::array<Axis, max_axes> axes{};
    std::uint8_t axis_count = 0;
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
