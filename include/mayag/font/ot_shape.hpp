#pragma once
// mayag::typo::ot — the shaping pipeline (hb_ot_shape, rule for rule)
//
// ot_layout.hpp applies OpenType lookups; this file decides WHICH lookups
// run, on WHICH glyphs, in WHAT order, and does everything around them that
// a font does not encode but readers expect:
//
//   unicode props    general category, default-ignorables (ZWJ, ZWNJ, CGJ,
//                    variation selectors), modified combining classes
//   clusters         a base and its marks are one cluster (one cursor stop)
//   normalisation    decompose to what the font has, reorder marks
//                    canonically, recompose when the font prefers it, so
//                    "é" works with fonts that only have "e" + U+0301
//   mirroring        '(' in right-to-left text draws as ')' (rtlm / bidi)
//   fractions        "1/2" gets numr/frac/dnom when the font has them
//   script shapers   Arabic joining, Indic reordering, USE clusters, Thai,
//                    Hangul, Hebrew, Khmer, Myanmar plug in as hooks
//   positioning      advances, GPOS, mark zeroing policy, default-ignorable
//                    hiding, and the visual reversal of right-to-left runs
//
// The order of every step is HarfBuzz's (hb-ot-shape.cc), because fonts are
// built and QA'd against it: running mark reordering after GSUB instead of
// before, or zeroing marks after attachment instead of before, produces text
// that is subtly and script-specifically wrong.
//
// A Shaper is a set of hooks (struct ScriptShaper) chosen per script, like
// hb_ot_shaper_t. The default hooks shape Latin, Greek, Cyrillic, CJK,
// symbols, and every script without contextual reordering.
//
// A port of HarfBuzz's hb-ot-shape.cc, hb-ot-shape-normalize.cc and
// hb-ot-shape-fallback.cc; HarfBuzz is "Old MIT" licensed, see
// THIRD_PARTY_NOTICES.md.

#include "aat_kern.hpp"
#include "aat_kerx.hpp"
#include "ot_layout.hpp"
#include "aat_morx.hpp"
#include "ot_language_table.hpp"
#include "unicode_data.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::typo::ot {

// ── font access the pipeline needs ────────────────────────────────────────

/// What shaping needs from a face, beyond its layout tables. Kept abstract
/// so the pipeline has no dependency on mayag's Face/FontStack.
struct FontFuncs {
    /// Nominal glyph for a codepoint; false if the font does not map it.
    std::function<bool(char32_t, std::uint16_t&)> nominal;
    unsigned num_glyphs = 65536;   ///< maxp numGlyphs (AAT lookup format 0)
    /// Glyph for a codepoint + variation selector (cmap format 14).
    std::function<bool(char32_t, char32_t, std::uint16_t&)> variation;
    /// Horizontal advance in font units, at the current instance.
    std::function<float(std::uint16_t)> advance;
    std::vector<float> coords;   ///< normalised instance coordinates
    std::uint16_t upem = 1000;
    /// Legacy `kern` table pair value in font units (null: font has none).
    std::function<int(std::uint16_t, std::uint16_t)> legacy_kern;
    /// The raw `kern` table: when set, the kern pass runs HarfBuzz's full
    /// KernTable::apply (both dialects, formats 0/2/3, cross-stream, RTL
    /// visual order) instead of the format-0 pair callback above.
    std::span<const std::uint8_t> kern_table;
    /// The raw AAT `kerx` and `ankr` tables (empty when absent).
    std::span<const std::uint8_t> kerx_table, ankr_table;
    /// Tracking added to each grapheme's advance, font units (Apple `trak`;
    /// the caller resolves it for the point size).
    float tracking = 0.0f;
    /// Ink extents in font units, y-up: x_bearing, y_bearing (top),
    /// width, height (negative, downward). Only used to place marks when a
    /// font has no GPOS mark positioning.
    struct Extents { int x_bearing = 0, y_bearing = 0, width = 0, height = 0; };
    std::function<bool(std::uint16_t, Extents&)> extents;
    /// OS/2 sxHeight / sCapHeight in font units (0: unknown). CoreText's
    /// fallback mark placement measures its gaps from them.
    int x_height = 0, cap_height = 0;
};

// ── per-glyph unicode properties (hb_set_unicode_props) ───────────────────

namespace detail {

/// HarfBuzz's modified combining classes: ccc remapped so canonical
/// reordering puts Hebrew, Arabic and Syriac points, and the Thai/Lao/
/// Tibetan vowel marks, in the order fonts expect.
[[nodiscard]] inline std::uint8_t modified_ccc(char32_t u) noexcept {
    if (u == 0x1A60u || u == 0x0FC6u) return 254;   // SAKOT, PADMA
    if (u == 0x0F39u) return 127;                  // TSA -PHRU
    const unsigned c = ucd::combining_class(u);
    switch (c) {
        case 10: return 22; case 11: return 15; case 12: return 16; case 13: return 17;
        case 14: return 23; case 15: return 18; case 16: return 19; case 17: return 20;
        case 18: return 21; case 19: return 14; case 20: return 24; case 21: return 12;
        case 22: return 25; case 23: return 13; case 24: return 10; case 25: return 11;
        case 26: return 26; case 27: return 28; case 28: return 29; case 29: return 30;
        case 30: return 31; case 31: return 32; case 32: return 33; case 33: return 27;
        case 34: return 34; case 35: return 35; case 36: return 36;
        case 84: return 4; case 91: return 5; case 103: return 3;
        case 130: return 132; case 132: return 131;
        default: return static_cast<std::uint8_t>(c);
    }
}

/// HarfBuzz's own default-ignorable set (narrower than the Unicode
/// property: it excludes e.g. Hangul fillers, which fonts draw).
[[nodiscard]] inline bool hb_default_ignorable(char32_t ch) noexcept {
    const char32_t plane = ch >> 16;
    if (plane == 0) {
        switch (ch >> 8) {
            case 0x00: return ch == 0x00AD;
            case 0x03: return ch == 0x034F;
            case 0x06: return ch == 0x061C;
            case 0x17: return ch >= 0x17B4 && ch <= 0x17B5;
            case 0x18: return ch >= 0x180B && ch <= 0x180E;
            case 0x20: return (ch >= 0x200B && ch <= 0x200F) || (ch >= 0x202A && ch <= 0x202E) ||
                              (ch >= 0x2060 && ch <= 0x206F);
            case 0xFE: return (ch >= 0xFE00 && ch <= 0xFE0F) || ch == 0xFEFF;
            case 0xFF: return ch >= 0xFFF0 && ch <= 0xFFF8;
            default: return false;
        }
    }
    if (plane == 1) return ch >= 0x1D173 && ch <= 0x1D17A;
    if (plane == 0xE) return ch >= 0xE0000 && ch <= 0xE0FFF;
    return false;
}

[[nodiscard]] inline bool gc_is_mark(std::uint8_t gc) noexcept {
    using G = ucd::GeneralCategory;
    return gc == std::uint8_t(G::Mn) || gc == std::uint8_t(G::Mc) || gc == std::uint8_t(G::Me);
}

}  // namespace detail

/// Fill gc, uflags and mcc for one glyph from its codepoint.
inline void set_unicode_props(GlyphInfo& g) {
    const char32_t u = g.codepoint;
    g.gc = static_cast<std::uint8_t>(ucd::general_category(u));
    g.uflags = 0;
    g.mcc = 0;
    if (u < 0x80) return;
    if (detail::hb_default_ignorable(u)) {
        g.uflags |= uflag::ignorable;
        if (u == 0x200C) g.uflags |= uflag::zwnj;
        else if (u == 0x200D) g.uflags |= uflag::zwj;
        else if ((u >= 0x180B && u <= 0x180D) || u == 0x180F || (u >= 0xE0020 && u <= 0xE007F) || u == 0x034F)
            g.uflags |= uflag::hidden;
    }
    if (detail::gc_is_mark(g.gc)) {
        g.uflags |= uflag::continuation;
        g.mcc = detail::modified_ccc(u);
    }
}

[[nodiscard]] inline bool is_unicode_mark(const GlyphInfo& g) noexcept { return detail::gc_is_mark(g.gc); }
[[nodiscard]] inline bool is_continuation(const GlyphInfo& g) noexcept { return (g.uflags & uflag::continuation) != 0; }

/// hb_set_unicode_props: per-glyph props plus the grapheme continuations
/// HarfBuzz forms for emoji ZWJ sequences, Regional Indicator pairs, and
/// tag sequences, so a reversed RTL run does not split a grapheme.
inline void set_unicode_props(Buffer& b) {
    auto& info = b.info;
    for (std::size_t i = 0; i < info.size(); ++i) {
        set_unicode_props(info[i]);
        const char32_t u = info[i].codepoint;
        const auto gc = static_cast<ucd::GeneralCategory>(info[i].gc);
        if (gc == ucd::GeneralCategory::So && u >= 0x1F1E6 && u <= 0x1F1FF) {   // Regional Indicators pair
            if (i && info[i - 1].codepoint >= 0x1F1E6 && info[i - 1].codepoint <= 0x1F1FF &&
                !is_continuation(info[i - 1]))
                info[i].uflags |= uflag::continuation;
        } else if (info[i].uflags & uflag::zwj) {   // ZWJ + Extended_Pictographic
            info[i].uflags |= uflag::continuation;
            if (i + 1 < info.size() && ucd::is_extended_pictographic(info[i + 1].codepoint)) {
                ++i;
                set_unicode_props(info[i]);
                info[i].uflags |= uflag::continuation;
            }
        } else if (u >= 0xE0020 && u <= 0xE007F) {   // tag sequences
            info[i].uflags |= uflag::continuation;
        } else if (u >= 0xFF9E && u <= 0xFF9F) {    // halfwidth voiced marks
            info[i].uflags |= uflag::continuation;
        }
    }
}

/// hb_form_clusters (MONOTONE_GRAPHEMES): merge each grapheme into the
/// cluster of its first character.
inline void form_clusters(Buffer& b) {
    const std::size_t n = b.len();
    std::size_t start = 0;
    for (std::size_t i = 1; i <= n; ++i) {
        if (i < n && is_continuation(b.info[i])) continue;
        b.merge_clusters(start, i);
        start = i;
    }
}

// ── normalisation (hb-ot-shape-normalize.cc) ───────────────────────────────────────

enum class Normalization : std::uint8_t { none, decomposed, composed_diacritics, composed_diacritics_no_short_circuit, automatic };

/// Width class a missing space falls back to (hb space_t); kept in aux.
namespace space {
inline constexpr std::uint8_t none = 0, em = 1, em_2 = 2, em_3 = 3, em_4 = 4, em_5 = 5, em_6 = 6,
                              em_16 = 16, em_4_18 = 17, space = 18, figure = 19, punctuation = 20, narrow = 21;
}
[[nodiscard]] inline std::uint8_t space_fallback_type(char32_t u) noexcept {
    switch (u) {
        case 0x00A0: return space::space;
        case 0x2000: return space::em_2; case 0x2001: return space::em;
        case 0x2002: return space::em_2; case 0x2003: return space::em;
        case 0x2004: return space::em_3; case 0x2005: return space::em_4;
        case 0x2006: return space::em_6; case 0x2007: return space::figure;
        case 0x2008: return space::punctuation; case 0x2009: return space::em_5;
        case 0x200A: return space::em_16; case 0x202F: return space::narrow;
        case 0x205F: return space::em_4_18; case 0x3000: return space::em;
        default: return space::none;
    }
}

struct ScriptShaper;
struct ShapePlan;

namespace detail {

struct NormalizeContext {
    const ShapePlan& plan;
    Buffer& b;
    const FontFuncs& font;
    bool (*decompose)(const NormalizeContext&, char32_t ab, char32_t& a, char32_t& b);
    bool (*compose)(const NormalizeContext&, char32_t a, char32_t b, char32_t& ab);
};

inline bool decompose_unicode(const NormalizeContext&, char32_t ab, char32_t& a, char32_t& b) {
    const auto d = ucd::canonical_decomposition(ab);
    if (d.a == 0) return false;
    a = d.a;
    b = d.b;
    return true;
}
inline bool compose_unicode(const NormalizeContext&, char32_t a, char32_t b, char32_t& ab) {
    ab = ucd::canonical_composition(a, b);
    return ab != 0;
}

inline void output_char(Buffer& b, char32_t u, std::uint16_t glyph) {
    b.cur().gid = glyph;
    // output_glyph(unichar) in HarfBuzz copies cur and sets its codepoint.
    b.out.push_back(b.cur());
    b.out.back().codepoint = u;
    set_unicode_props(b.out.back());
}
inline void next_char(Buffer& b, std::uint16_t glyph) {
    b.cur().gid = glyph;
    b.next_glyph();
}

inline unsigned decompose(const NormalizeContext& c, bool shortest, char32_t ab) {
    char32_t a = 0, bb = 0;
    std::uint16_t a_glyph = 0, b_glyph = 0;
    if (!c.decompose(c, ab, a, bb) || (bb && !c.font.nominal(bb, b_glyph))) return 0;
    const bool has_a = c.font.nominal(a, a_glyph);
    if (shortest && has_a) {
        output_char(c.b, a, a_glyph);
        if (bb) { output_char(c.b, bb, b_glyph); return 2; }
        return 1;
    }
    if (const unsigned ret = decompose(c, shortest, a)) {
        if (bb) { output_char(c.b, bb, b_glyph); return ret + 1; }
        return ret;
    }
    if (has_a) {
        output_char(c.b, a, a_glyph);
        if (bb) { output_char(c.b, bb, b_glyph); return 2; }
        return 1;
    }
    return 0;
}

inline void decompose_current_character(const NormalizeContext& c, bool shortest) {
    Buffer& b = c.b;
    const char32_t u = b.cur().codepoint;
    std::uint16_t glyph = 0;
    if (shortest && c.font.nominal(u, glyph)) { next_char(b, glyph); return; }
    if (decompose(c, shortest, u)) { b.skip_glyph(); return; }
    if (!shortest && c.font.nominal(u, glyph)) { next_char(b, glyph); return; }
    if (b.cur().gc == static_cast<std::uint8_t>(ucd::GeneralCategory::Zs)) {
        const std::uint8_t st = space_fallback_type(u);
        std::uint16_t space_glyph = 0;
        if (st != space::none && c.font.nominal(0x0020, space_glyph)) {
            b.cur().aux = st;
            next_char(b, space_glyph);
            return;
        }
    }
    if (u == 0x2011) {   // NON-BREAKING HYPHEN draws as HYPHEN
        std::uint16_t other = 0;
        if (c.font.nominal(0x2010, other)) { next_char(b, other); return; }
    }
    next_char(b, glyph);   // .notdef
}

[[nodiscard]] inline bool is_variation_selector(char32_t u) noexcept {
    return (u >= 0x180B && u <= 0x180D) || u == 0x180F || (u >= 0xFE00 && u <= 0xFE0F) ||
           (u >= 0xE0100 && u <= 0xE01EF);
}

inline void set_glyph(GlyphInfo& g, const FontFuncs& f) {
    std::uint16_t gid = 0;
    (void)f.nominal(g.codepoint, gid);
    g.gid = gid;
}

inline void handle_variation_selector_cluster(const NormalizeContext& c, std::size_t end) {
    Buffer& b = c.b;
    while (b.idx + 1 < end && b.successful) {
        if (is_variation_selector(b.cur(1).codepoint)) {
            std::uint16_t g = 0;
            if (c.font.variation && c.font.variation(b.cur().codepoint, b.cur(1).codepoint, g)) {
                // replace_glyphs(2, 1): one glyph for base+selector, merged cluster
                b.cur().gid = g;
                b.merge_clusters(b.idx, b.idx + 2);
                b.out.push_back(b.cur());
                b.idx += 2;
            } else {
                set_glyph(b.cur(), c.font);
                b.next_glyph();
                set_glyph(b.cur(), c.font);   // the selector itself (stays ignorable)
                b.next_glyph();
            }
            while (b.idx < end && is_variation_selector(b.cur().codepoint)) {
                set_glyph(b.cur(), c.font);
                b.next_glyph();
            }
        } else {
            set_glyph(b.cur(), c.font);
            b.next_glyph();
        }
    }
    if (b.idx < end) {
        set_glyph(b.cur(), c.font);
        b.next_glyph();
    }
}

inline void decompose_multi_char_cluster(const NormalizeContext& c, std::size_t end, bool short_circuit) {
    Buffer& b = c.b;
    for (std::size_t i = b.idx; i < end; ++i)
        if (is_variation_selector(b.info[i].codepoint)) {
            handle_variation_selector_cluster(c, end);
            return;
        }
    while (b.idx < end && b.successful) decompose_current_character(c, short_circuit);
}

}  // namespace detail

// ── script shapers and plans ────────────────────────────────────────────────────────────

/// What a script shaper's collect/override hooks see (hb_ot_shape_planner_t).
struct Planner {
    PlanBuilder map;
    const Layout& layout;
    ucd::Script script;
    Direction direction;
    std::uint32_t chosen_script[2] = {0, 0};
};

/// Hooks for one family of scripts (hb_ot_shaper_t). Any may be null.
struct ScriptShaper {
    const char* name = "default";
    void (*collect_features)(Planner&) = nullptr;
    void (*override_features)(Planner&) = nullptr;
    std::shared_ptr<void> (*data_create)(const ShapePlan&) = nullptr;
    void (*preprocess_text)(const ShapePlan&, Buffer&, const FontFuncs&) = nullptr;
    void (*postprocess_glyphs)(const ShapePlan&, Buffer&, const FontFuncs&) = nullptr;
    bool (*decompose)(const detail::NormalizeContext&, char32_t ab, char32_t& a, char32_t& b) = nullptr;
    bool (*compose)(const detail::NormalizeContext&, char32_t a, char32_t b, char32_t& ab) = nullptr;
    void (*setup_masks)(const ShapePlan&, Buffer&, const FontFuncs&) = nullptr;
    void (*reorder_marks)(const ShapePlan&, Buffer&, std::size_t start, std::size_t end) = nullptr;
    std::uint32_t gpos_tag = 0;   ///< apply GPOS only if the font chose this script for it
    Normalization normalization = Normalization::automatic;
    ZeroMarks zero_marks = ZeroMarks::late;
    bool fallback_position = true;
};

/// A compiled plan: the lookup Plan plus the shaper and the policy flags
/// hb_ot_shape_plan_t derives from the font.
struct ShapePlan {
    Plan map;
    const ScriptShaper* shaper = nullptr;
    ucd::Script script = ucd::Script::Common;
    Direction direction = Direction::ltr;
    std::shared_ptr<void> data;   ///< the shaper's private plan data

    std::uint32_t frac_mask = 0, numr_mask = 0, dnom_mask = 0, rtlm_mask = 0, kern_mask = 0;
    bool has_frac = false;
    bool apply_gpos = false;
    bool apply_kern = false;            ///< legacy `kern` table (no GPOS kern feature)
    bool apply_kerx = false;            ///< AAT `kerx` (runs only when GPOS does not)
    bool zero_marks = false;
    bool has_gpos_mark = false;
    bool adjust_mark_positioning_when_zeroing = false;
    bool fallback_mark_positioning = false;
    /// Fallback marks placed the way CoreText places them on AAT faces
    /// (position_marks_coretext) instead of HarfBuzz's way.
    bool coretext_mark_positioning = false;
    bool fallback_glyph_classes = false;
    bool apply_morx = false;              ///< AAT morx replaces GSUB
    std::span<const std::uint8_t> morx;
    /// For morx faces: the script shaper's compose (the dumber shaper has none).
    bool (*morx_compose)(const detail::NormalizeContext&, char32_t a, char32_t b, char32_t& ab) = nullptr;
};

/// What a GSUB/GPOS pause callback receives through `user`.
struct PauseContext {
    const ShapePlan* plan;
    const FontFuncs* font;
    const Layout* layout = nullptr;
};

/// The default shaper: Latin, Greek, Cyrillic, CJK, symbols, and every script
/// without contextual reordering.
inline const ScriptShaper& default_shaper() {
    static const ScriptShaper s{};
    return s;
}

// ── script tags ────────────────────────────────────────────────────────────────────────────

/// OpenType script tags for a script, in the order fonts are searched
/// (hb_ot_all_tags_from_script): the v3/v2 Indic tags first, then the old.
inline std::vector<std::uint32_t> ot_script_tags(ucd::Script s) {
    const std::uint32_t iso = ucd::iso15924_tag(s);
    std::vector<std::uint32_t> tags;
    std::uint32_t v2 = 0;
    switch (iso) {
        case make_tag('B', 'e', 'n', 'g'): v2 = make_tag('b', 'n', 'g', '2'); break;
        case make_tag('D', 'e', 'v', 'a'): v2 = make_tag('d', 'e', 'v', '2'); break;
        case make_tag('G', 'u', 'j', 'r'): v2 = make_tag('g', 'j', 'r', '2'); break;
        case make_tag('G', 'u', 'r', 'u'): v2 = make_tag('g', 'u', 'r', '2'); break;
        case make_tag('K', 'n', 'd', 'a'): v2 = make_tag('k', 'n', 'd', '2'); break;
        case make_tag('M', 'l', 'y', 'm'): v2 = make_tag('m', 'l', 'm', '2'); break;
        case make_tag('O', 'r', 'y', 'a'): v2 = make_tag('o', 'r', 'y', '2'); break;
        case make_tag('T', 'a', 'm', 'l'): v2 = make_tag('t', 'm', 'l', '2'); break;
        case make_tag('T', 'e', 'l', 'u'): v2 = make_tag('t', 'e', 'l', '2'); break;
        case make_tag('M', 'y', 'm', 'r'): v2 = make_tag('m', 'y', 'm', '2'); break;
        default: break;
    }
    if (v2) {
        if (v2 != make_tag('m', 'y', 'm', '2')) tags.push_back((v2 & 0xFFFFFF00u) | '3');
        tags.push_back(v2);
    }
    std::uint32_t old = iso | 0x20000000u;   // lowercase the first letter
    switch (iso) {
        case make_tag('Z', 'y', 'y', 'y'): case make_tag('Z', 'i', 'n', 'h'): case make_tag('Z', 'z', 'z', 'z'):
            old = 0; break;
        case make_tag('H', 'i', 'r', 'a'): old = make_tag('k', 'a', 'n', 'a'); break;
        case make_tag('L', 'a', 'o', 'o'): old = make_tag('l', 'a', 'o', ' '); break;
        case make_tag('Y', 'i', 'i', 'i'): old = make_tag('y', 'i', ' ', ' '); break;
        case make_tag('N', 'k', 'o', 'o'): old = make_tag('n', 'k', 'o', ' '); break;
        case make_tag('V', 'a', 'i', 'i'): old = make_tag('v', 'a', 'i', ' '); break;
        default: break;
    }
    if (old) tags.push_back(old);
    return tags;
}

/// hb_script_get_horizontal_direction.
[[nodiscard]] inline Direction script_direction(ucd::Script s) noexcept {
    switch (ucd::iso15924_tag(s)) {
        case make_tag('A','r','a','b'): case make_tag('H','e','b','r'): case make_tag('S','y','r','c'):
        case make_tag('T','h','a','a'): case make_tag('C','p','r','t'): case make_tag('K','h','a','r'):
        case make_tag('P','h','n','x'): case make_tag('N','k','o','o'): case make_tag('L','y','d','i'):
        case make_tag('A','v','s','t'): case make_tag('A','r','m','i'): case make_tag('P','h','l','i'):
        case make_tag('P','r','t','i'): case make_tag('S','a','r','b'): case make_tag('O','r','k','h'):
        case make_tag('S','a','m','r'): case make_tag('M','a','n','d'): case make_tag('M','e','r','c'):
        case make_tag('M','e','r','o'): case make_tag('M','a','n','i'): case make_tag('M','e','n','d'):
        case make_tag('N','b','a','t'): case make_tag('N','a','r','b'): case make_tag('P','a','l','m'):
        case make_tag('P','h','l','p'): case make_tag('H','a','t','r'): case make_tag('A','d','l','m'):
        case make_tag('R','o','h','g'): case make_tag('S','o','g','o'): case make_tag('S','o','g','d'):
        case make_tag('E','l','y','m'): case make_tag('C','h','r','s'): case make_tag('Y','e','z','i'):
        case make_tag('O','u','g','r'): case make_tag('G','a','r','a'):
            return Direction::rtl;
        default:
            return Direction::ltr;
    }
}

/// Chooses the shaper for a script (hb_ot_shaper_categorize). Complex
/// shapers register themselves here; until one does, its scripts use the
/// default shaper.
using ShaperSelector = const ScriptShaper* (*)(ucd::Script, Direction, std::uint32_t gsub_script);
inline ShaperSelector& shaper_selector() {
    static ShaperSelector sel = nullptr;
    return sel;
}

// ── language ──────────────────────────────────────────────────────────────────────────

/// OpenType language-system tags for a BCP 47 language tag ("sr", "tr-TR",
/// "zh-Hant-HK"), in priority order (hb_ot_tags_from_language). Fonts
/// key localized forms on these: Serbian and Macedonian italic Cyrillic,
/// Turkish dotted i in smcp, Romanian comma-below, Dutch IJ, Polish kreska,
/// Traditional Chinese punctuation position. Empty: no language system.
inline std::vector<std::uint32_t> ot_language_tags(std::string_view bcp47) {
    std::vector<std::uint32_t> out;
    if (bcp47.empty()) return out;
    std::string lc;
    for (char ch : bcp47) lc += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : (ch == '_' ? '-' : ch);
    const auto has_subtag = [&](std::string_view sub) {
        for (std::size_t p = lc.find('-'); p != std::string::npos; p = lc.find('-', p + 1))
            if (lc.compare(p + 1, sub.size(), sub) == 0 &&
                (p + 1 + sub.size() == lc.size() || lc[p + 1 + sub.size()] == '-'))
                return true;
        return false;
    };
    std::string primary = lc.substr(0, lc.find('-'));
    // The most common multi-subtag cases from hb_ot_tags_from_complex_language.
    if (primary == "zh") {
        if (has_subtag("hk")) return {make_tag('Z', 'H', 'H', ' '), make_tag('Z', 'H', 'T', ' ')};
        if (has_subtag("mo")) return {make_tag('Z', 'H', 'T', 'M'), make_tag('Z', 'H', 'T', ' ')};
        if (has_subtag("hant") || has_subtag("tw")) return {make_tag('Z', 'H', 'T', ' ')};
        return {make_tag('Z', 'H', 'S', ' ')};
    }
    // An extended-language subtag ("zh-yue") stands in for the primary.
    if (const std::size_t d = lc.find('-'); d != std::string::npos) {
        const std::size_t e = lc.find('-', d + 1);
        const std::string ext = lc.substr(d + 1, (e == std::string::npos ? lc.size() : e) - d - 1);
        if (ext.size() == 3 && ext[0] >= 'a' && ext[0] <= 'z') primary = ext;
    }
    if (primary.size() < 2 || primary.size() > 3) return out;
    const std::uint32_t key = make_tag(primary[0], primary[1], primary.size() > 2 ? primary[2] : ' ', ' ');
    const auto* first = std::lower_bound(std::begin(langtags::table), std::end(langtags::table), key,
                                         [](const langtags::Entry& e, std::uint32_t k) { return e.bcp47 < k; });
    for (; first != std::end(langtags::table) && first->bcp47 == key; ++first) out.push_back(first->ot);
    return out;
}

/// A user feature request: tag=value over [start, end) clusters (all text
/// when start == 0 and end == ~0u).
struct Feature {
    std::uint32_t tag = 0;
    std::uint32_t value = 1;
    std::uint32_t start = 0, end = ~0u;
    [[nodiscard]] bool global() const noexcept { return start == 0 && end == ~0u; }
};

/// Compile a ShapePlan (hb_ot_shape_plan_t + hb_ot_shape_collect_features).
inline ShapePlan compile_plan(const Layout& l, ucd::Script script, Direction dir, std::span<const std::uint32_t> languages,
                              std::span<const Feature> user, const FontFuncs& font,
                              bool has_kern_table = false, bool has_kerx_table = false,
                              std::span<const std::uint8_t> morx = {}) {
    const auto tags = ot_script_tags(script);
    Planner p{PlanBuilder{l, tags, languages, dir}, l, script, dir, {}};
    p.chosen_script[0] = p.map.chosen_script(table::gsub);
    p.chosen_script[1] = p.map.chosen_script(table::gpos);

    const ScriptShaper* shaper = &default_shaper();
    if (auto sel = shaper_selector())
        if (const ScriptShaper* s = sel(script, dir, p.chosen_script[0])) shaper = s;
    // _hb_apply_morx: morx wins for horizontal text (we only shape horizontal).
    const bool apply_morx = !morx.empty() && Reader{morx}.u32_at(0) != 0;
    const bool script_zero_marks = shaper->zero_marks != ZeroMarks::none;
    const bool script_fallback_position = shaper->fallback_position;
    const ScriptShaper* script_shaper = shaper;
    if (apply_morx && shaper != &default_shaper()) {   // _hb_ot_shaper_dumber
        static const ScriptShaper dumber{.name = "dumber", .zero_marks = ZeroMarks::none, .fallback_position = false};
        shaper = &dumber;
    }

    auto& m = p.map;
    m.enable_feature(make_tag('r', 'v', 'r', 'n'));
    m.add_gsub_pause(nullptr);
    if (dir == Direction::ltr) {
        m.enable_feature(make_tag('l', 't', 'r', 'a'));
        m.enable_feature(make_tag('l', 't', 'r', 'm'));
    } else {
        m.enable_feature(make_tag('r', 't', 'l', 'a'));
        m.add_feature(make_tag('r', 't', 'l', 'm'));
    }
    m.add_feature(make_tag('f', 'r', 'a', 'c'));
    m.add_feature(make_tag('n', 'u', 'm', 'r'));
    m.add_feature(make_tag('d', 'n', 'o', 'm'));
    m.enable_feature(make_tag('r', 'a', 'n', 'd'), F_RANDOM, max_feature_value);
    if (shaper->collect_features) shaper->collect_features(p);
    for (const auto& [t, f] : {std::pair{make_tag('a','b','v','m'), F_GLOBAL}, {make_tag('b','l','w','m'), F_GLOBAL},
                               {make_tag('c','c','m','p'), F_GLOBAL}, {make_tag('l','o','c','l'), F_GLOBAL},
                               {make_tag('m','a','r','k'), F_GLOBAL_MANUAL_JOINERS}, {make_tag('m','k','m','k'), F_GLOBAL_MANUAL_JOINERS},
                               {make_tag('r','l','i','g'), F_GLOBAL},
                               {make_tag('c','a','l','t'), F_GLOBAL}, {make_tag('c','l','i','g'), F_GLOBAL},
                               {make_tag('c','u','r','s'), F_GLOBAL}, {make_tag('d','i','s','t'), F_GLOBAL},
                               {make_tag('k','e','r','n'), F_GLOBAL_HAS_FALLBACK}, {make_tag('l','i','g','a'), F_GLOBAL},
                               {make_tag('r','c','l','t'), F_GLOBAL}})
        m.add_feature(t, f);
    for (const Feature& f : user) m.add_feature(f.tag, f.global() ? F_GLOBAL : F_NONE, f.value);
    if (shaper->override_features) shaper->override_features(p);

    ShapePlan sp;
    sp.map = m.compile(font.coords);
    sp.shaper = shaper;
    sp.script = script;
    sp.direction = dir;
    sp.frac_mask = sp.map.one_mask_of(make_tag('f', 'r', 'a', 'c'));
    sp.numr_mask = sp.map.one_mask_of(make_tag('n', 'u', 'm', 'r'));
    sp.dnom_mask = sp.map.one_mask_of(make_tag('d', 'n', 'o', 'm'));
    sp.has_frac = sp.frac_mask || (sp.numr_mask && sp.dnom_mask);
    sp.rtlm_mask = sp.map.one_mask_of(make_tag('r', 't', 'l', 'm'));
    sp.kern_mask = sp.map.mask_of(make_tag('k', 'e', 'r', 'n'));
    const FeatureMap* kern = sp.map.feature(make_tag('k', 'e', 'r', 'n'));
    const bool has_gpos_kern = kern && kern->index[table::gpos] != 0xFFFF;
    const bool disable_gpos = shaper->gpos_tag && shaper->gpos_tag != sp.map.chosen_script[table::gpos];
    sp.fallback_glyph_classes = !l.has_glyph_classes();
    const bool gpos = !disable_gpos && l.has_gpos();
    // hb_ot_shape_plan (HarfBuzz 15.1): AAT `kerx` wins when the font lacks
    // GSUB+GPOS together, or when GPOS has no kern feature; the legacy `kern`
    // table runs only for shapers that position by fallback (default,
    // Arabic, Hangul, Hebrew) and only when kerning was requested.
    const bool has_gsub = !apply_morx && l.has_gsub();
    const bool kerx = has_kerx_table && (!(has_gsub && gpos) || !has_gpos_kern);
    sp.apply_gpos = gpos && !(has_kerx_table && !(has_gsub && gpos));
    const bool plan_kern = !kerx && (!has_gpos_kern || !sp.apply_gpos) && has_kern_table && script_fallback_position;
    sp.apply_kern = plan_kern && sp.kern_mask;   // HarfBuzz gates the pass on requested_kerning
    sp.apply_kerx = kerx;
    // A state-machine `kern` positions marks itself; cross-stream kern
    // moves them vertically: either way HarfBuzz leaves marks alone.
    const bool machine_kern = plan_kern && aat::has_machine_kern(font.kern_table);
    const bool cross_kern = plan_kern && aat::has_cross_kern(font.kern_table);
    sp.zero_marks = script_zero_marks && !kerx && !machine_kern;
    sp.has_gpos_mark = sp.map.one_mask_of(make_tag('m', 'a', 'r', 'k')) != 0;
    sp.adjust_mark_positioning_when_zeroing = !sp.apply_gpos && !kerx && !cross_kern;
    sp.fallback_mark_positioning = sp.adjust_mark_positioning_when_zeroing && script_fallback_position;
    sp.apply_morx = apply_morx;
    if (apply_morx) sp.adjust_mark_positioning_when_zeroing = false;   // after fallback_mark_positioning
    // CoreText (Ned Holbrook, harfbuzz#1528): "no mark width zeroing, and no
    // fallback positioning for Hebrew, Thai, or [any] script with Shaping
    // Required". HarfBuzz ports only the zeroing half (the dumber shaper);
    // mayag takes both, so an AAT Hebrew face (Raanana) sets its points
    // where the font draws them, exactly as every macOS app shows them.
    if (apply_morx && shaper != &default_shaper()) sp.fallback_mark_positioning = false;
    // An AAT face is DESIGNED against CoreText; where it leaves marks to the
    // engine (no GPOS `mark`, no kerx), CoreText places them itself, even
    // when a kern-only GPOS is present (Futura, Papyrus). So does mayag.
    if (apply_morx && shaper == &default_shaper() && !sp.has_gpos_mark && !kerx) {
        sp.coretext_mark_positioning = true;
        sp.fallback_mark_positioning = true;
    }
    sp.morx = morx;
    // CoreText composes canonically equivalent PRESENTATION forms the font
    // maps (shin + shin dot -> U+FB2A across a qamats), which is what the
    // script shaper's compose does; HarfBuzz's dumber shaper loses it.
    if (apply_morx) sp.morx_compose = script_shaper->compose;
    // A GPOS `mark` that never runs (morx + kerx faces like Arial Hebrew)
    // must not stop Hebrew presentation forms: CoreText composes them.
    if (apply_morx && !sp.apply_gpos) sp.has_gpos_mark = false;
    if (shaper->data_create) sp.data = shaper->data_create(sp);
    return sp;
}

/// _hb_ot_shape_normalize: decompose (only as far as the font needs),
/// reorder marks canonically by modified combining class, and recompose
/// what the font has precomposed.
inline void normalize(const ShapePlan& plan, Buffer& b, const FontFuncs& font) {
    if (b.info.empty()) return;
    Normalization mode = plan.shaper->normalization;
    if (mode == Normalization::automatic) mode = Normalization::composed_diacritics;
    detail::NormalizeContext c{plan, b, font,
                               plan.shaper->decompose ? plan.shaper->decompose : detail::decompose_unicode,
                               plan.morx_compose ? plan.morx_compose
                               : plan.shaper->compose ? plan.shaper->compose : detail::compose_unicode};
    const bool always_short_circuit = mode == Normalization::none;
    const bool might_short_circuit = always_short_circuit ||
        (mode != Normalization::decomposed && mode != Normalization::composed_diacritics_no_short_circuit);

    // First round: decompose.
    bool all_simple = true;
    {
        b.clear_output();
        const std::size_t count = b.len();
        b.idx = 0;
        do {
            std::size_t end;
            for (end = b.idx + 1; end < count; ++end)
                if (is_unicode_mark(b.info[end])) break;
            if (end < count) --end;   // leave one base for the marks to cluster with
            // From idx to end are simple clusters.
            if (might_short_circuit) {
                for (; b.idx < end && b.successful; ) {
                    std::uint16_t g = 0;
                    if (!font.nominal(b.cur().codepoint, g)) break;
                    b.cur().gid = g;
                    b.next_glyph();
                }
            }
            while (b.idx < end && b.successful) detail::decompose_current_character(c, might_short_circuit);
            if (b.idx == count || !b.successful) break;
            all_simple = false;
            // Find all the marks now.
            for (end = b.idx + 1; end < count; ++end)
                if (!is_unicode_mark(b.info[end])) break;
            detail::decompose_multi_char_cluster(c, end, always_short_circuit);
        } while (b.idx < count && b.successful);
        // Decomposition can introduce marks into otherwise simple input
        // (U+1B08 -> U+1B07 U+1B35 must still be recomposed).
        if (b.successful && b.out_len() != count) all_simple = false;
        b.sync();
    }

    // Second round: reorder marks (stable sort runs of nonzero mcc).
    if (!all_simple) {
        const std::size_t count = b.len();
        // Remember each mark's INPUT rank in its run: CoreText stacks
        // uncomposed marks in input order (position_around_base_coretext).
        for (std::size_t i = 0, rank = 0; i < count; ++i) {
            if (b.info[i].mcc == 0) { rank = 0; continue; }
            if (b.info[i].aux == 0) b.info[i].aux = static_cast<std::uint8_t>(std::min<std::size_t>(++rank, 255));
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (b.info[i].mcc == 0) continue;
            std::size_t end;
            for (end = i + 1; end < count; ++end)
                if (b.info[end].mcc == 0) break;
            if (end - i > 32) { i = end; continue; }   // HB_OT_SHAPE_MAX_COMBINING_MARKS
            // Insertion sort by mcc with cluster merging, like hb_buffer_t::sort.
            for (std::size_t k = i + 1; k < end; ++k) {
                std::size_t j = k;
                while (j > i && b.info[j - 1].mcc > b.info[k].mcc) --j;
                if (j == k) continue;
                b.merge_clusters(j, k + 1);
                const GlyphInfo t = b.info[k];
                std::copy_backward(b.info.begin() + static_cast<std::ptrdiff_t>(j),
                                   b.info.begin() + static_cast<std::ptrdiff_t>(k),
                                   b.info.begin() + static_cast<std::ptrdiff_t>(k + 1));
                b.info[j] = t;
            }
            if (plan.shaper->reorder_marks) plan.shaper->reorder_marks(plan, b, i, end);
            i = end;
        }
    }
    // A CGJ that did not prevent any reordering becomes skippable again.
    for (std::size_t i = 1; i + 1 < b.len(); ++i)
        if (b.info[i].codepoint == 0x034F &&
            (b.info[i + 1].mcc == 0 || b.info[i - 1].mcc <= b.info[i + 1].mcc))
            b.info[i].uflags &= static_cast<std::uint8_t>(~uflag::hidden);

    // Third round: recompose.
    if (!all_simple && (mode == Normalization::composed_diacritics ||
                        mode == Normalization::composed_diacritics_no_short_circuit)) {
        b.clear_output();
        const std::size_t count = b.len();
        std::size_t starter = 0;
        b.next_glyph();
        while (b.idx < count && b.successful) {
            if (is_unicode_mark(b.cur())) {
                char32_t composed = 0;
                std::uint16_t glyph = 0;
                if ((starter == b.out.size() - 1 || b.out.back().mcc < b.cur().mcc) &&
                    c.compose(c, b.out[starter].codepoint, b.cur().codepoint, composed) &&
                    font.nominal(composed, glyph)) {
                    b.next_glyph();
                    b.merge_out_clusters(starter, b.out.size());
                    b.out.pop_back();
                    b.out[starter].codepoint = composed;
                    b.out[starter].gid = glyph;
                    set_unicode_props(b.out[starter]);
                    continue;
                }
            }
            b.next_glyph();
            if (b.out.back().mcc == 0) starter = b.out.size() - 1;
        }
        b.sync();
    }

    // CoreText composes canonically equivalent sequences (a + U+0302 +
    // U+0323 -> U+1EAD in either order) but leaves marks it could NOT compose
    // in INPUT order, and an AAT face's state machines see that order: Times
    // ligates x + dot-below only when they are adjacent in the text
    // (measured against CTLine). Undo the canonical reorder for morx faces.
    if (!all_simple && plan.apply_morx) {
        const std::size_t count = b.len();
        for (std::size_t i = 0; i < count; ++i) {
            if (b.info[i].mcc == 0) continue;
            std::size_t end = i + 1;
            while (end < count && b.info[end].mcc != 0) ++end;
            std::stable_sort(b.info.begin() + static_cast<std::ptrdiff_t>(i),
                             b.info.begin() + static_cast<std::ptrdiff_t>(end),
                             [](const GlyphInfo& x, const GlyphInfo& y) { return x.aux < y.aux; });
            i = end;
        }
    }
}

// ── the pipeline (hb_ot_shape_internal) ──────────────────────────────────────────────

namespace detail {

/// Reverse each grapheme's glyphs... then the whole buffer, so graphemes
/// keep their internal order (_hb_ot_layout_reverse_graphemes).
inline void reverse_graphemes(Buffer& b) {
    const std::size_t n = b.len();
    std::size_t start = 0;
    for (std::size_t i = 1; i <= n; ++i) {
        if (i < n && is_continuation(b.info[i])) continue;
        b.reverse_range(start, i);
        start = i;
    }
    b.reverse();
}

inline void ensure_native_direction(Buffer& b, ucd::Script script) {
    Direction horiz = script_direction(script);
    if (horiz == Direction::rtl && b.direction == Direction::ltr) {
        bool number = false, letter = false, ri = false;
        for (const auto& g : b.info) {
            const auto gc = static_cast<ucd::GeneralCategory>(g.gc);
            if (gc == ucd::GeneralCategory::Nd) number = true;
            else if (gc <= ucd::GeneralCategory::Lo) { letter = true; break; }
            else if (g.codepoint >= 0x1F1E6 && g.codepoint <= 0x1F1FF) ri = true;
        }
        if ((number || ri) && !letter) horiz = Direction::ltr;
    }
    if (b.direction != horiz) {
        reverse_graphemes(b);
        b.direction = (b.direction == Direction::ltr) ? Direction::rtl : Direction::ltr;
    }
}

inline void rotate_chars(const ShapePlan& plan, Buffer& b, Direction target, const FontFuncs& font) {
    if (target != Direction::rtl) return;
    for (auto& g : b.info) {
        const char32_t m = ucd::bidi_mirror(g.codepoint);
        std::uint16_t gid = 0;
        if (m != g.codepoint && font.nominal(m, gid)) g.codepoint = m;
        else g.mask |= plan.rtlm_mask;
    }
}

inline void setup_masks_fraction(const ShapePlan& plan, Buffer& b) {
    if (!plan.has_frac) return;
    const bool forward = b.direction == Direction::ltr;
    const std::uint32_t pre = forward ? (plan.numr_mask | plan.frac_mask) : (plan.frac_mask | plan.dnom_mask);
    const std::uint32_t post = forward ? (plan.frac_mask | plan.dnom_mask) : (plan.numr_mask | plan.frac_mask);
    auto& info = b.info;
    const auto nd = static_cast<std::uint8_t>(ucd::GeneralCategory::Nd);
    for (std::size_t i = 0; i < info.size(); ++i) {
        if (info[i].codepoint != 0x2044) continue;   // FRACTION SLASH
        std::size_t start = i, end = i + 1;
        while (start && info[start - 1].gc == nd) --start;
        while (end < info.size() && info[end].gc == nd) ++end;
        if (start == i || end == i + 1) continue;
        for (std::size_t j = start; j < i; ++j) info[j].mask |= pre;
        info[i].mask |= plan.frac_mask;
        for (std::size_t j = i + 1; j < end; ++j) info[j].mask |= post;
        i = end - 1;
    }
}

inline void set_masks(Buffer& b, std::uint32_t value, std::uint32_t mask, std::uint32_t start, std::uint32_t end) {
    if (!mask) return;
    for (auto& g : b.info)
        if (g.cluster >= start && g.cluster < end) g.mask = (g.mask & ~mask) | (value & mask);
}

/// hb_aat_layout_position: the `kerx` pass, with the kern skippy for pairs.
inline void kerx_pass(const Layout& l, Buffer& b, std::uint32_t kern_mask, const FontFuncs& font) {
    const std::vector<float> no_coords;
    ApplyContext c{l, table::gpos, b, no_coords};
    c.lookup_mask = kern_mask;
    c.set_lookup_props(lookup_flag::ignore_marks);
    Skippy& it = c.iter_input;
    aat::apply_kerx(font.kerx_table, font.ankr_table, b, kern_mask, font.num_glyphs, [&](std::size_t i) -> std::size_t {
        it.reset(i);
        it.set_match(Skippy::Kind::none);
        return it.next() ? it.idx : static_cast<std::size_t>(-1);
    });
}

/// The legacy `kern` table pass (hb_kern_machine_t): pairs skip marks, and
/// each kern value splits between the two glyphs like HarfBuzz.
inline void kern_machine(const Layout& l, Buffer& b, std::uint32_t kern_mask, const FontFuncs& font) {
    if (!font.kern_table.empty()) {
        const std::vector<float> no_coords;
        ApplyContext c{l, table::gpos, b, no_coords};
        c.lookup_mask = kern_mask;
        c.set_lookup_props(lookup_flag::ignore_marks);
        Skippy& it = c.iter_input;
        aat::apply_kern(font.kern_table, b, kern_mask, font.num_glyphs, [&](std::size_t i) -> std::size_t {
            it.reset(i);
            it.set_match(Skippy::Kind::none);
            return it.next() ? it.idx : static_cast<std::size_t>(-1);
        });
        return;
    }
    // A `kern` subtable is read in its own order: forward unless it sets
    // Backwards (no OT subtable can), so an RTL buffer is kerned VISUALLY,
    // reversed and back (KerxTable::apply's `reverse`). Kerning an Arabic
    // run in logical order applied every pair to the wrong neighbours.
    const bool reverse = b.direction == Direction::rtl;
    if (reverse) b.reverse();
    const std::vector<float> no_coords;
    ApplyContext c{l, table::gpos, b, no_coords};
    c.lookup_mask = kern_mask;
    c.set_lookup_props(lookup_flag::ignore_marks);
    Skippy& it = c.iter_input;
    for (std::size_t idx = 0; idx < b.len();) {
        if (!(b.info[idx].mask & kern_mask)) { ++idx; continue; }
        it.reset(idx);
        it.set_match(Skippy::Kind::none);
        if (!it.next()) { ++idx; continue; }
        const std::size_t i = idx, j = it.idx;
        const int kern = font.legacy_kern(b.info[i].gid, b.info[j].gid);
        if (kern) {
            const int k1 = kern >> 1, k2 = kern - k1;
            b.pos[i].x_advance += static_cast<float>(k1);
            b.pos[j].x_advance += static_cast<float>(k2);
            b.pos[j].x_offset += static_cast<float>(k2);
        }
        idx = j;
    }
    if (reverse) b.reverse();
}

/// Space fallback widths (_hb_ot_shape_fallback_spaces), in font units.
inline void fallback_spaces(Buffer& b, const FontFuncs& font) {
    const float upem = font.upem;
    for (std::size_t i = 0; i < b.len(); ++i) {
        const std::uint8_t t = b.info[i].aux;
        if (b.info[i].gc != static_cast<std::uint8_t>(ucd::GeneralCategory::Zs) || t == space::none) continue;
        float& adv = b.pos[i].x_advance;
        switch (t) {
            case space::em: case space::em_2: case space::em_3: case space::em_4:
            case space::em_5: case space::em_6: case space::em_16:
                adv = std::round(upem / static_cast<float>(t)); break;   // +(upem+t/2)/t, rounded
            case space::em_4_18: adv = std::round(upem * 4.0f / 18.0f); break;
            case space::figure: {
                for (char32_t u = '0'; u <= '9'; ++u) {
                    std::uint16_t g = 0;
                    if (font.nominal(u, g)) { adv = font.advance(g); break; }
                }
                break;
            }
            case space::punctuation: {
                std::uint16_t g = 0;
                if (font.nominal('.', g) || font.nominal(',', g)) adv = font.advance(g);
                break;
            }
            case space::narrow: adv = std::round(adv / 2.0f); break;
            default: break;
        }
    }
}

}  // namespace detail

// ── fallback mark positioning (hb-ot-shape-fallback.cc) ────────────────────────────
//
// A font with no GPOS mark lookups still has to show "a" + U+0301 as an
// accented letter. HarfBuzz centres each mark over (or under) its base from
// the glyphs' ink boxes, stacking by combining class; so do we.

namespace fallback {

namespace ccc {
inline constexpr unsigned attached_below_left = 200, attached_below = 202, attached_above = 214,
                          attached_above_right = 216, below_left = 218, below = 220, below_right = 222,
                          left = 224, right = 226, above_left = 228, above = 230, above_right = 232,
                          double_below = 233, double_above = 234;
}

[[nodiscard]] inline unsigned recategorize(char32_t u, unsigned klass) noexcept {
    if (klass >= 200) return klass;
    if ((u & ~0xFFu) == 0x0E00u) {   // Thai / Lao
        if (klass == 0) {
            switch (u) {
                case 0x0E31: case 0x0E34: case 0x0E35: case 0x0E36: case 0x0E37:
                case 0x0E47: case 0x0E4C: case 0x0E4D: case 0x0E4E: klass = ccc::above_right; break;
                case 0x0EB1: case 0x0EB4: case 0x0EB5: case 0x0EB6: case 0x0EB7:
                case 0x0EBB: case 0x0ECC: case 0x0ECD: klass = ccc::above; break;
                case 0x0EBC: klass = ccc::below; break;
                default: break;
            }
        } else if (u == 0x0E3A) {
            klass = ccc::below_right;
        }
    }
    // Modified classes (the values modified_ccc produces) to positions;
    // generated from HarfBuzz's HB_MODIFIED_COMBINING_CLASS_* defines.
    switch (klass) {
        case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24:
        case 25: case 30: case 33: case 118: case 129: case 131:
            return ccc::below;
        case 13: return ccc::attached_above;
        case 10: case 107: return ccc::above_right;
        case 11: case 14: return ccc::above_left;
        case 26: case 27: case 28: case 29: case 31: case 32: case 34: case 35: case 36: case 122: case 132:
            return ccc::above;
        case 3: return ccc::below_right;
        default: break;   // 12 (CCC21) and everything else: unchanged
    }
    return klass;
}

inline void zero_mark_advances(Buffer& b, std::size_t start, std::size_t end, bool adjust) {
    for (std::size_t i = start; i < end; ++i)
        if (b.info[i].gc == static_cast<std::uint8_t>(ucd::GeneralCategory::Mn)) {
            if (adjust) { b.pos[i].x_offset -= b.pos[i].x_advance; b.pos[i].y_offset -= b.pos[i].y_advance; }
            b.pos[i].x_advance = 0;
            b.pos[i].y_advance = 0;
        }
}

inline void position_mark(const FontFuncs& font, Buffer& b, FontFuncs::Extents& base, std::size_t i,
                          unsigned klass) {
    FontFuncs::Extents mark;
    if (!font.extents(b.info[i].gid, mark)) return;
    const int y_gap = font.upem / 16;
    GlyphPos& pos = b.pos[i];
    int xo = 0, yo = 0;
    switch (klass) {
        case ccc::double_below: case ccc::double_above:
            if (b.direction == Direction::ltr) { xo += base.x_bearing + base.width - mark.width / 2 - mark.x_bearing; break; }
            xo += base.x_bearing - mark.width / 2 - mark.x_bearing;
            break;
        default:
        case ccc::attached_below: case ccc::attached_above: case ccc::below: case ccc::above:
            xo += base.x_bearing + (base.width - mark.width) / 2 - mark.x_bearing;
            break;
        case ccc::attached_below_left: case ccc::below_left: case ccc::above_left:
            xo += base.x_bearing - mark.x_bearing;
            break;
        case ccc::attached_above_right: case ccc::below_right: case ccc::above_right:
            xo += base.x_bearing + base.width - mark.width - mark.x_bearing;
            break;
    }
    switch (klass) {
        case ccc::double_below: case ccc::below_left: case ccc::below: case ccc::below_right:
            base.height -= y_gap;
            [[fallthrough]];
        case ccc::attached_below_left: case ccc::attached_below:
            yo = base.y_bearing + base.height - mark.y_bearing;
            if ((y_gap > 0) == (yo > 0)) { base.height -= yo; yo = 0; }
            base.height += mark.height;
            break;
        case ccc::double_above: case ccc::above_left: case ccc::above: case ccc::above_right:
            base.y_bearing += y_gap;
            base.height -= y_gap;
            [[fallthrough]];
        case ccc::attached_above: case ccc::attached_above_right: {
            yo = base.y_bearing - (mark.y_bearing + mark.height);
            if ((y_gap > 0) != (yo > 0)) {
                const int correction = -yo / 2;
                base.y_bearing += correction;
                base.height -= correction;
                yo += correction;
            }
            base.y_bearing -= mark.height;
            base.height += mark.height;
            break;
        }
        default: break;
    }
    pos.x_offset = static_cast<float>(xo);
    pos.y_offset = static_cast<float>(yo);
}

/// CoreText's fallback mark placement (measured against CTLine on Apple's
/// AAT faces; exact to the unit):
///   x  centred on the base's ADVANCE; *_RIGHT classes align ink right edges,
///      *_LEFT ink left edges, DOUBLE_* centre on the base's trailing edge;
///   y  above / below the cluster's ink so far by a gap of capHeight/8 when
///      the cluster reaches cap height, else xHeight/5; ATTACHED_* classes
///      touch (gap 0); overlays (ccc 1) centre on capHeight/2.
inline void position_around_base_coretext(const FontFuncs& font, Buffer& b, std::size_t base, std::size_t end,
                                          bool adjust) {
    FontFuncs::Extents be;
    if (!font.extents || !font.extents(b.info[base].gid, be)) { zero_mark_advances(b, base + 1, end, adjust); return; }
    const float advance = font.advance(b.info[base].gid);
    const float bx0 = static_cast<float>(be.x_bearing), bx1 = bx0 + static_cast<float>(be.width);
    const float xh = static_cast<float>(font.x_height > 0 ? font.x_height : font.upem / 2);
    const float ch = static_cast<float>(font.cap_height > 0 ? font.cap_height : font.upem * 7 / 10);
    float ftop = static_cast<float>(be.y_bearing) + b.pos[base].y_offset;   // cluster ink so far, y-up
    float fbottom = ftop + static_cast<float>(be.height);
    // Pen travelled from the base to each glyph, in buffer order.
    std::vector<float> pen(end - base, 0.0f);
    {
        float x = 0;
        if (b.direction == Direction::ltr) x -= b.pos[base].x_advance;
        for (std::size_t i = base + 1; i < end; ++i) {
            pen[i - base] = x;
            if (!b.info[i].mcc) x += b.direction == Direction::ltr ? -b.pos[i].x_advance : b.pos[i].x_advance;
        }
    }
    // CoreText stacks marks in INPUT order (aux = input rank, set before
    // the canonical reorder), so `x ^ .` and `x . ^` stack differently.
    std::vector<std::size_t> order;
    for (std::size_t i = base + 1; i < end; ++i) if (b.info[i].mcc) order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t c) { return b.info[a].aux < b.info[c].aux; });
    for (const std::size_t i : order) {
        FontFuncs::Extents m;
        b.pos[i].x_advance = 0;
        b.pos[i].y_advance = 0;
        if (!font.extents(b.info[i].gid, m)) continue;
        GlyphPos& pos = b.pos[i];
        const float mx0 = static_cast<float>(m.x_bearing), mx1 = mx0 + static_cast<float>(m.width);
        const float mtop = static_cast<float>(m.y_bearing), mbottom = mtop + static_cast<float>(m.height);
        const unsigned u = b.info[i].mcc;
        float xo;
        switch (u) {
            case 216: case 222: case 232: xo = bx1 - mx1; break;                     // *_RIGHT
            case 200: case 218: case 228: xo = bx0 - mx0; break;                     // *_LEFT
            case 233: case 234: xo = advance - (mx0 + mx1) / 2.0f; break;            // DOUBLE_*
            default: xo = advance / 2.0f - (mx0 + mx1) / 2.0f; break;
        }
        const float gap = ftop >= ch ? ch / 8.0f : xh / 5.0f;
        float yo;
        switch (u) {
            case 1: yo = ch / 2.0f - (mtop + mbottom) / 2.0f; break;                  // overlay
            case 200: case 202: yo = fbottom - mtop; break;                          // attached below
            case 214: case 216: yo = ftop - mbottom; break;                          // attached above
            case 218: case 220: case 222: case 233: case 240: yo = fbottom - gap - mtop; break;
            default: yo = ftop + gap - mbottom; break;                                // above
        }
        pos.x_offset = xo + pen[i - base];
        pos.y_offset = yo;
        if (u != 1) {
            ftop = std::max(ftop, mtop + yo);
            fbottom = std::min(fbottom, mbottom + yo);
        }
    }
}

inline void position_around_base(const ShapePlan& plan, const FontFuncs& font, Buffer& b, std::size_t base,
                                 std::size_t end, bool adjust) {
    if (plan.coretext_mark_positioning) { position_around_base_coretext(font, b, base, end, adjust); return; }
    FontFuncs::Extents be;
    if (!font.extents || !font.extents(b.info[base].gid, be)) { zero_mark_advances(b, base + 1, end, adjust); return; }
    be.y_bearing += static_cast<int>(b.pos[base].y_offset);
    be.x_bearing = 0;   // horizontal advance, not ink, for horizontal placement (hb#1532)
    be.width = static_cast<int>(std::lround(font.advance(b.info[base].gid)));
    const unsigned lig_id = lig::id(b.info[base]);
    const int num_comps = static_cast<int>(lig::num_comps(b.info[base]));
    float x_offset = 0, y_offset = 0;
    if (b.direction == Direction::ltr) { x_offset -= b.pos[base].x_advance; y_offset -= b.pos[base].y_advance; }
    FontFuncs::Extents component = be, cluster = be;
    int last_comp = -1;
    unsigned last_class = 255;
    const Direction horiz = plan.direction;
    for (std::size_t i = base + 1; i < end; ++i) {
        if (b.info[i].mcc) {
            if (num_comps > 1) {
                const unsigned this_id = lig::id(b.info[i]);
                int this_comp = static_cast<int>(lig::comp(b.info[i])) - 1;
                if (!lig_id || lig_id != this_id || this_comp >= num_comps) this_comp = num_comps - 1;
                if (last_comp != this_comp) {
                    last_comp = this_comp;
                    last_class = 255;
                    component = be;
                    if (horiz == Direction::ltr) component.x_bearing += (this_comp * component.width) / num_comps;
                    else component.x_bearing += ((num_comps - 1 - this_comp) * component.width) / num_comps;
                    component.width /= num_comps;
                }
            }
            const unsigned klass = b.info[i].mcc;
            if (last_class != klass) { last_class = klass; cluster = component; }
            position_mark(font, b, cluster, i, klass);
            b.pos[i].x_advance = 0;
            b.pos[i].y_advance = 0;
            b.pos[i].x_offset += x_offset;
            b.pos[i].y_offset += y_offset;
        } else if (b.direction == Direction::ltr) {
            x_offset -= b.pos[i].x_advance;
            y_offset -= b.pos[i].y_advance;
        } else {
            x_offset += b.pos[i].x_advance;
            y_offset += b.pos[i].y_advance;
        }
    }
}

/// _hb_ot_shape_fallback_mark_position, for a logical-order buffer.
inline void position_marks(const ShapePlan& plan, const FontFuncs& font, Buffer& b, bool adjust) {
    const auto cluster = [&](std::size_t start, std::size_t end) {
        if (end - start < 2) return;
        for (std::size_t i = start; i < end; ++i) {
            if (is_unicode_mark(b.info[i])) continue;
            std::size_t j = i + 1;
            for (; j < end; ++j) {
                if ((b.info[j].uflags & uflag::hidden) || is_default_ignorable(b.info[j])) continue;
                if (!is_unicode_mark(b.info[j])) break;
            }
            position_around_base(plan, font, b, i, j, adjust);
            i = j - 1;
        }
    };
    std::size_t start = 0;
    for (std::size_t i = 1; i < b.len(); ++i)
        if (!is_unicode_mark(b.info[i]) && !(b.info[i].uflags & uflag::hidden) && !is_default_ignorable(b.info[i])) {
            cluster(start, i);
            start = i;
        }
    cluster(start, b.len());
}

/// Before GSUB: remap each Mn's mcc to a positioning class.
inline void recategorize_marks(Buffer& b) {
    for (auto& g : b.info)
        if (g.gc == static_cast<std::uint8_t>(ucd::GeneralCategory::Mn))
            g.mcc = static_cast<std::uint8_t>(recategorize(g.codepoint, g.mcc));
}

}  // namespace fallback

/// Shape one run: `b.info` holds the characters (codepoint + cluster) of a
/// single-script, single-direction run in LOGICAL order, `b.direction` the
/// run's direction. On return `b.info`/`b.pos` hold glyphs in VISUAL order
/// (right-to-left runs reversed, like hb_shape) with positions in font units.
inline void shape(const ShapePlan& plan, const Layout& l, const FontFuncs& font, Buffer& b,
                  std::span<const Feature> user = {}, bool begin_of_text = true) {
    const Direction target = b.direction;
    b.remove_output();
    for (auto& g : b.info) { g.mask = plan.map.global_mask(); g.glyph_props = 0; g.lig_props = 0; g.aux = 0; g.syllable = 0; }
    set_unicode_props(b);

    // A mark at the start of text gets a dotted circle to sit on.
    std::uint16_t dotted = 0;
    if (begin_of_text && !b.info.empty() && is_unicode_mark(b.info[0]) && font.nominal(0x25CC, dotted)) {
        GlyphInfo dc{};
        dc.codepoint = 0x25CC;
        set_unicode_props(dc);
        dc.cluster = b.info[0].cluster;
        dc.mask = b.info[0].mask;
        b.info.insert(b.info.begin(), dc);
    }
    form_clusters(b);
    detail::ensure_native_direction(b, plan.script);
    if (plan.shaper->preprocess_text) plan.shaper->preprocess_text(plan, b, font);

    // ── substitute_pre ──
    detail::rotate_chars(plan, b, target, font);
    normalize(plan, b, font);
    detail::setup_masks_fraction(plan, b);
    if (plan.shaper->setup_masks) plan.shaper->setup_masks(plan, b, font);
    if (plan.fallback_mark_positioning) fallback::recategorize_marks(b);
    for (const Feature& f : user)
        if (!f.global()) {
            const FeatureMap* fm = plan.map.feature(f.tag);
            if (fm) detail::set_masks(b, f.value << fm->shift, fm->mask, f.start, f.end);
        }
    set_glyph_props(l, b);   // GDEF classes, or synthesized ones
    PauseContext pc{&plan, &font, &l};
    if (plan.apply_morx) {
        typo::aat::substitute(plan.morx, b, font.num_glyphs);
        if (plan.apply_gpos) typo::aat::remove_deleted_glyphs(b);
    } else {
        substitute(l, plan.map, b, &pc, font.coords);
    }

    // ── position ──
    b.pos.assign(b.len(), GlyphPos{});
    for (std::size_t i = 0; i < b.len(); ++i) b.pos[i].x_advance = font.advance(b.info[i].gid);
    detail::fallback_spaces(b, font);
    const bool adjust = plan.adjust_mark_positioning_when_zeroing && b.direction == Direction::ltr;
    const ZeroMarks zero = plan.zero_marks ? plan.shaper->zero_marks : ZeroMarks::none;
    position(l, plan.map, b, font.coords, zero, adjust, &pc, plan.apply_gpos, [&](Buffer& buf) {
        if (!plan.apply_gpos && plan.apply_kerx && !font.kerx_table.empty()) detail::kerx_pass(l, buf, plan.kern_mask, font);
        if (plan.apply_kern && (font.legacy_kern || !font.kern_table.empty())) detail::kern_machine(l, buf, plan.kern_mask, font);
        if (font.tracking != 0.0f) {   // hb_aat_layout_track: once per grapheme
            const std::size_t n = buf.len();
            std::size_t start = 0;
            for (std::size_t i = 1; i <= n; ++i) {
                if (i < n && is_continuation(buf.info[i])) continue;
                buf.pos[start].x_advance += font.tracking;
                start = i;
            }
        }
    });
    if (plan.fallback_mark_positioning) fallback::position_marks(plan, font, b, adjust);
    if (b.direction == Direction::rtl) b.reverse();

    // ── substitute_post ──
    if (plan.apply_morx && !plan.apply_gpos) typo::aat::remove_deleted_glyphs(b);
    // Variation selectors that found no glyph and default-ignorables draw
    // as the space glyph with zero advance (hb_ot_hide_default_ignorables).
    std::uint16_t invisible = 0;
    if (font.nominal(' ', invisible)) {
        for (auto& g : b.info)
            if (is_default_ignorable(g)) g.gid = invisible;
    } else {
        std::size_t j = 0;
        for (std::size_t i = 0; i < b.len(); ++i) {
            if (is_default_ignorable(b.info[i])) continue;
            b.info[j] = b.info[i];
            b.pos[j] = b.pos[i];
            ++j;
        }
        b.info.resize(j);
        b.pos.resize(j);
    }
    if (plan.shaper->postprocess_glyphs) plan.shaper->postprocess_glyphs(plan, b, font);
    b.direction = target;
}

}  // namespace mayag::typo::ot
