#pragma once
// mayag::typo::ot::indic — the Indic shaper (Bengali, Devanagari, Gujarati,
// Gurmukhi, Kannada, Malayalam, Oriya, Tamil, Telugu), a rule-for-rule port of
// HarfBuzz's hb-ot-shaper-indic.cc and the driver of its Ragel syllable machine
// (tables in ot_shaper_indic_machine.hpp).
//
// Port of HarfBuzz. Original copyright notice:
/*
 * Copyright © 2011,2012  Google, Inc.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Google Author(s): Behdad Esfahbod
 */

#include "ot_shape.hpp"
#include "ot_shaper_indic_machine.hpp"
#include "ot_shaper_indic_table.hpp"
#include "ot_syllabic.hpp"
#include "ot_vowel_constraints.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace mayag::typo::ot::indic {

namespace cat = machine;   // I_Cat(x) == cat::x

enum SyllableType : std::uint8_t {
    consonant_syllable, vowel_syllable, standalone_cluster, symbol_cluster, broken_cluster, non_indic_cluster,
};

/// ot_position_t (hb-ot-shaper-indic.hh).
enum Position : std::uint8_t {
    POS_START = 0, POS_RA_TO_BECOME_REPH = 1, POS_PRE_M = 2, POS_PRE_C = 3, POS_BASE_C = 4,
    POS_AFTER_MAIN = 5, POS_ABOVE_C = 6, POS_BEFORE_SUB = 7, POS_BELOW_C = 8, POS_AFTER_SUB = 9,
    POS_BEFORE_POST = 10, POS_POST_C = 11, POS_AFTER_POST = 12, POS_SMVD = 13, POS_END = 14,
};

enum RephMode : std::uint8_t { REPH_MODE_IMPLICIT, REPH_MODE_EXPLICIT, REPH_MODE_LOG_REPHA };
enum BlwfMode : std::uint8_t { BLWF_MODE_PRE_AND_POST, BLWF_MODE_POST_ONLY };

struct Config {
    ucd::Script script;
    bool has_old_spec;
    char32_t virama;
    std::uint8_t reph_pos;   // a Position
    RephMode reph_mode;
    BlwfMode blwf_mode;
};

inline constexpr Config configs[] = {
    {ucd::Script::Unknown,    false, 0,       POS_BEFORE_POST, REPH_MODE_IMPLICIT,  BLWF_MODE_PRE_AND_POST},
    {ucd::Script::Devanagari, true,  0x094Du, POS_BEFORE_POST, REPH_MODE_IMPLICIT,  BLWF_MODE_PRE_AND_POST},
    {ucd::Script::Bengali,    true,  0x09CDu, POS_AFTER_SUB,   REPH_MODE_IMPLICIT,  BLWF_MODE_PRE_AND_POST},
    {ucd::Script::Gurmukhi,   true,  0x0A4Du, POS_BEFORE_SUB,  REPH_MODE_IMPLICIT,  BLWF_MODE_PRE_AND_POST},
    {ucd::Script::Gujarati,   true,  0x0ACDu, POS_BEFORE_POST, REPH_MODE_IMPLICIT,  BLWF_MODE_PRE_AND_POST},
    {ucd::Script::Oriya,      true,  0x0B4Du, POS_AFTER_MAIN,  REPH_MODE_IMPLICIT,  BLWF_MODE_PRE_AND_POST},
    {ucd::Script::Tamil,      true,  0x0BCDu, POS_AFTER_POST,  REPH_MODE_IMPLICIT,  BLWF_MODE_PRE_AND_POST},
    {ucd::Script::Telugu,     true,  0x0C4Du, POS_AFTER_POST,  REPH_MODE_EXPLICIT,  BLWF_MODE_POST_ONLY},
    {ucd::Script::Kannada,    true,  0x0CCDu, POS_AFTER_POST,  REPH_MODE_IMPLICIT,  BLWF_MODE_POST_ONLY},
    {ucd::Script::Malayalam,  true,  0x0D4Du, POS_AFTER_MAIN,  REPH_MODE_LOG_REPHA, BLWF_MODE_PRE_AND_POST},
};

struct Feature { std::uint32_t tag; std::uint32_t flags; };
inline constexpr Feature features[] = {
    // Basic features: applied in order, one at a time, after initial_reordering.
    {make_tag('n', 'u', 'k', 't'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('a', 'k', 'h', 'n'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('r', 'p', 'h', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('r', 'k', 'r', 'f'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('p', 'r', 'e', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('b', 'l', 'w', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('a', 'b', 'v', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('h', 'a', 'l', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('p', 's', 't', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('v', 'a', 't', 'u'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('c', 'j', 'c', 't'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
    // Other features: applied all at once, after final_reordering.
    {make_tag('i', 'n', 'i', 't'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('p', 'r', 'e', 's'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('a', 'b', 'v', 's'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('b', 'l', 'w', 's'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('p', 's', 't', 's'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('h', 'a', 'l', 'n'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
};
enum : std::uint8_t {
    NUKT_, AKHN_, RPHF, RKRF_, PREF, BLWF, ABVF, HALF, PSTF, VATU_, CJCT_,
    INIT, PRES_, ABVS_, BLWS_, PSTS_, HALN_, NUM_FEATURES, BASIC_FEATURES = INIT,
};

/// hb_indic_would_substitute_feature_t: the lookups of a feature's stage.
struct WouldSubstitute {
    std::size_t begin = 0, end = 0;
    bool zero_context = false;
    bool operator()(const Plan& plan, const Layout& l, std::span<const std::uint16_t> glyphs) const {
        const auto& lk = plan.lookups[table::gsub];
        for (std::size_t i = begin; i < end && i < lk.size(); i++)
            if (would_substitute(l, lk[i].index, glyphs, zero_context)) return true;
        return false;
    }
};

struct PlanData {
    const Config* config = &configs[0];
    ucd::Script script = ucd::Script::Unknown;
    bool is_old_spec = false;
    static constexpr bool uniscribe_bug_compatible = false;
    WouldSubstitute rphf, pref, blwf, pstf, vatu;
    std::uint32_t mask_array[NUM_FEATURES] = {};
};

inline bool is_one_of(const GlyphInfo& g, std::uint32_t flags) noexcept {
    if (g.glyph_props & props::ligated) return false;   // if it ligated, all bets are off
    return g.shaper_cat < 32 && ((1u << g.shaper_cat) & flags);
}
inline constexpr std::uint32_t FL(unsigned c) noexcept { return c < 32 ? 1u << c : 0u; }
inline constexpr std::uint32_t CONSONANT_FLAGS = FL(cat::C) | FL(cat::CS) | FL(cat::Ra) | FL(cat::CM) | FL(cat::V) |
                                                 FL(cat::PLACEHOLDER) | FL(cat::DOTTEDCIRCLE);
inline constexpr std::uint32_t JOINER_FLAGS = FL(cat::ZWJ) | FL(cat::ZWNJ);
inline bool is_consonant(const GlyphInfo& g) noexcept { return is_one_of(g, CONSONANT_FLAGS); }
inline bool is_joiner(const GlyphInfo& g) noexcept { return is_one_of(g, JOINER_FLAGS); }
inline bool is_halant(const GlyphInfo& g) noexcept { return is_one_of(g, FL(cat::H)); }
inline bool flag_has(unsigned v, std::uint32_t flags) noexcept { return v < 32 && ((1u << v) & flags); }
inline bool ligated_and_didnt_multiply(const GlyphInfo& g) noexcept {
    return (g.glyph_props & props::ligated) && !(g.glyph_props & props::multiplied);
}

/// find_syllables_indic: the Ragel machine's driver, kept goto-for-goto.
inline void find_syllables(Buffer& buffer) {
    using namespace machine;
    auto& info = buffer.info;
    unsigned p = 0, pe = static_cast<unsigned>(info.size()), eof = pe, ts = 0, te = 0, act = 0;
    int cs = start;
    int trans = 0;
    unsigned syllable_serial = 1;
    auto found_syllable = [&](SyllableType t) {
        for (unsigned i = ts; i < te; i++)
            info[i].syllable = static_cast<std::uint8_t>((syllable_serial << 4) | t);
        syllable_serial++;
        if (syllable_serial == 16) syllable_serial = 1;
    };

    if (p == pe) goto test_eof;
resume:
    if (from_state_actions[cs] == 10) ts = p;
    {
        const std::uint8_t* keys = trans_keys + (cs << 1);
        const auto* inds = indicies + index_offsets[cs];
        const int slen = key_spans[cs];
        const unsigned c = info[p].shaper_cat;
        trans = inds[slen > 0 && keys[0] <= c && c <= keys[1] ? static_cast<int>(c - keys[0]) : slen];
    }
eof_trans:
    cs = trans_targs[trans];
    if (trans_actions[trans] == 0) goto again;
    switch (trans_actions[trans]) {
        case 2: te = p + 1; break;
        case 11: te = p + 1; found_syllable(non_indic_cluster); break;
        case 14: te = p; p--; found_syllable(consonant_syllable); break;
        case 15: te = p; p--; found_syllable(vowel_syllable); break;
        case 18: te = p; p--; found_syllable(standalone_cluster); break;
        case 20: te = p; p--; found_syllable(symbol_cluster); break;
        case 16: te = p; p--; found_syllable(broken_cluster); break;
        case 17: te = p; p--; found_syllable(non_indic_cluster); break;
        case 1: p = te - 1; found_syllable(consonant_syllable); break;
        case 3: p = te - 1; found_syllable(vowel_syllable); break;
        case 7: p = te - 1; found_syllable(standalone_cluster); break;
        case 8: p = te - 1; found_syllable(symbol_cluster); break;
        case 4: p = te - 1; found_syllable(broken_cluster); break;
        case 6:
            switch (act) {
                case 1: p = te - 1; found_syllable(consonant_syllable); break;
                case 5: p = te - 1; found_syllable(non_indic_cluster); break;
                case 6: p = te - 1; found_syllable(broken_cluster); break;
                case 7: p = te - 1; found_syllable(non_indic_cluster); break;
                default: break;
            }
            break;
        case 19: te = p + 1; act = 1; break;
        case 13: te = p + 1; act = 5; break;
        case 5: te = p + 1; act = 6; break;
        case 12: te = p + 1; act = 7; break;
        default: break;
    }
again:
    if (to_state_actions[cs] == 9) ts = 0;
    if (++p != pe) goto resume;
test_eof:
    if (p == eof && eof_trans[cs] > 0) {
        trans = eof_trans[cs] - 1;
        goto eof_trans;
    }
}

inline void setup_syllables(const Plan&, Buffer& b, void*) { find_syllables(b); }

inline const PlanData* plan_data(void* user, const PauseContext*& pc) {
    pc = static_cast<const PauseContext*>(user);
    return pc && pc->plan ? static_cast<const PlanData*>(pc->plan->data.get()) : nullptr;
}

/// load_virama_glyph (the font's nominal virama; 0 when absent).
inline std::uint16_t virama_glyph(const PlanData& ip, const FontFuncs* font) {
    std::uint16_t g = 0;
    if (!ip.config->virama || !font || !font->nominal || !font->nominal(ip.config->virama, g)) g = 0;
    return g;
}

inline std::uint8_t consonant_position_from_face(const PlanData& ip, const Plan& plan, const Layout& l,
                                                 std::uint16_t consonant, std::uint16_t virama) {
    // For old-spec, the order of glyphs is Consonant,Virama; for new-spec
    // Virama,Consonant. Try both.
    const std::uint16_t glyphs[3] = {virama, consonant, virama};
    const std::span<const std::uint16_t> a(glyphs, 2), b(glyphs + 1, 2);
    if (ip.blwf(plan, l, a) || ip.blwf(plan, l, b) || ip.vatu(plan, l, a) || ip.vatu(plan, l, b)) return POS_BELOW_C;
    if (ip.pstf(plan, l, a) || ip.pstf(plan, l, b)) return POS_POST_C;
    if (ip.pref(plan, l, a) || ip.pref(plan, l, b)) return POS_POST_C;
    return POS_BASE_C;
}

inline void update_consonant_positions(const PlanData& ip, const Plan& plan, const PauseContext& pc, Buffer& b) {
    const std::uint16_t virama = virama_glyph(ip, pc.font);
    if (!virama || !pc.layout) return;
    for (auto& g : b.info)
        if (g.shaper_pos == POS_BASE_C) g.shaper_pos = consonant_position_from_face(ip, plan, *pc.layout, g.gid, virama);
}

/// Moves info[from] to position `to` (to < from), shifting the rest right.
inline void move_back(std::vector<GlyphInfo>& info, std::size_t to, std::size_t from) {
    const GlyphInfo t = info[from];
    for (std::size_t k = from; k > to; k--) info[k] = info[k - 1];
    info[to] = t;
}
/// Moves info[from] to position `to` (to > from), shifting the rest left.
inline void move_fwd(std::vector<GlyphInfo>& info, std::size_t from, std::size_t to) {
    const GlyphInfo t = info[from];
    for (std::size_t k = from; k < to; k++) info[k] = info[k + 1];
    info[to] = t;
}

inline void initial_reordering_consonant_syllable(const PlanData& ip, const Plan& plan, const Layout* l, Buffer& buffer,
                                                  std::size_t start, std::size_t end) {
    auto& info = buffer.info;
    const ucd::Script script = ip.script;

    // https://github.com/harfbuzz/harfbuzz/issues/435#issuecomment-335560167
    // For compatibility with legacy usage in Kannada, Ra+h+ZWJ must behave like Ra+ZWJ+h.
    if (script == ucd::Script::Kannada && start + 3 <= end && is_one_of(info[start], FL(cat::Ra)) &&
        is_one_of(info[start + 1], FL(cat::H)) && is_one_of(info[start + 2], FL(cat::ZWJ))) {
        buffer.merge_clusters(start + 1, start + 3);
        std::swap(info[start + 1], info[start + 2]);
    }

    // 1. Find base consonant.
    std::size_t base = end;
    bool has_reph = false;
    {
        std::size_t limit = start;
        if (ip.mask_array[RPHF] && start + 3 <= end &&
            ((ip.config->reph_mode == REPH_MODE_IMPLICIT && !is_joiner(info[start + 2])) ||
             (ip.config->reph_mode == REPH_MODE_EXPLICIT && info[start + 2].shaper_cat == cat::ZWJ))) {
            // See if it matches the 'rphf' feature.
            const std::uint16_t glyphs[3] = {info[start].gid, info[start + 1].gid,
                                             ip.config->reph_mode == REPH_MODE_EXPLICIT ? info[start + 2].gid
                                                                                        : std::uint16_t{0}};
            if (l && (ip.rphf(plan, *l, std::span<const std::uint16_t>(glyphs, 2)) ||
                      (ip.config->reph_mode == REPH_MODE_EXPLICIT &&
                       ip.rphf(plan, *l, std::span<const std::uint16_t>(glyphs, 3))))) {
                limit += 2;
                while (limit < end && is_joiner(info[limit])) limit++;
                base = start;
                has_reph = true;
            }
        } else if (ip.config->reph_mode == REPH_MODE_LOG_REPHA && info[start].shaper_cat == cat::Repha) {
            limit += 1;
            while (limit < end && is_joiner(info[limit])) limit++;
            base = start;
            has_reph = true;
        }

        // -> starting from the end of the syllable, move backwards
        std::size_t i = end;
        bool seen_below = false;
        do {
            i--;
            // -> until a consonant is found
            if (is_consonant(info[i])) {
                // -> that does not have a below-base or post-base form
                if (info[i].shaper_pos != POS_BELOW_C && (info[i].shaper_pos != POS_POST_C || seen_below)) {
                    base = i;
                    break;
                }
                if (info[i].shaper_pos == POS_BELOW_C) seen_below = true;
                // -> or that is not a pre-base-reordering Ra, or arrive at the first consonant.
                base = i;
            } else {
                // A ZWJ after a Halant stops the base search.
                if (start < i && info[i].shaper_cat == cat::ZWJ && info[i - 1].shaper_cat == cat::H) break;
            }
        } while (i > limit);

        // If the syllable starts with Ra + Halant and has no other consonants, Ra is the base.
        if (has_reph && base == start && limit - base <= 2) has_reph = false;
    }

    // 2. Decompose and reorder Matras: done by normalization.
    // Reorder characters.
    for (std::size_t i = start; i < base; i++) info[i].shaper_pos = std::min<std::uint8_t>(POS_PRE_C, info[i].shaper_pos);
    if (base < end) info[base].shaper_pos = POS_BASE_C;
    // Mark final consonants: handled by the table.
    if (has_reph) info[start].shaper_pos = POS_RA_TO_BECOME_REPH;

    // For old-style Indic script tags, move the first post-base Halant after
    // the last consonant.
    if (ip.is_old_spec) {
        const bool disallow_double_halants = script == ucd::Script::Kannada;
        for (std::size_t i = base + 1; i < end; i++)
            if (info[i].shaper_cat == cat::H) {
                std::size_t j;
                for (j = end - 1; j > i; j--)
                    if (is_consonant(info[j]) || (disallow_double_halants && info[j].shaper_cat == cat::H)) break;
                if (info[j].shaper_cat != cat::H && j > i) move_fwd(info, i, j);
                break;
            }
    }

    // Attach misc marks to the previous char to move with them.
    {
        std::uint8_t last_pos = POS_START;
        for (std::size_t i = start; i < end; i++) {
            if (flag_has(info[i].shaper_cat, JOINER_FLAGS | FL(cat::N) | FL(cat::RS) | FL(cat::CM) | FL(cat::H))) {
                info[i].shaper_pos = last_pos;
                if (info[i].shaper_cat == cat::H && info[i].shaper_pos == POS_PRE_M) {
                    // Uniscribe doesn't move the Halant with Left Matra.
                    for (std::size_t j = i; j > start; j--)
                        if (info[j - 1].shaper_pos != POS_PRE_M) {
                            info[i].shaper_pos = info[j - 1].shaper_pos;
                            break;
                        }
                }
            } else if (info[i].shaper_pos != POS_SMVD) {
                if (info[i].shaper_cat == cat::MPst && i > start && info[i - 1].shaper_cat == cat::SM)
                    info[i - 1].shaper_pos = info[i].shaper_pos;
                last_pos = info[i].shaper_pos;
            }
        }
    }
    // For post-base consonants let them own anything before them since the
    // last consonant or matra.
    {
        std::size_t last = base;
        for (std::size_t i = base + 1; i < end; i++)
            if (is_consonant(info[i])) {
                for (std::size_t j = last + 1; j < i; j++)
                    if (info[j].shaper_pos < POS_SMVD) info[j].shaper_pos = info[i].shaper_pos;
                last = i;
            } else if (flag_has(info[i].shaper_cat, FL(cat::M) | FL(cat::MPst)))
                last = i;
    }

    {
        // Use the syllable() for sort accounting temporarily.
        const std::uint8_t syllable = info[start].syllable;
        for (std::size_t i = start; i < end; i++) info[i].syllable = static_cast<std::uint8_t>(i - start);
        std::stable_sort(info.begin() + static_cast<std::ptrdiff_t>(start), info.begin() + static_cast<std::ptrdiff_t>(end),
                         [](const GlyphInfo& x, const GlyphInfo& y) { return x.shaper_pos < y.shaper_pos; });

        // Find base again; also find left-matra sequence.
        std::size_t first_left_matra = end, last_left_matra = end;
        base = end;
        for (std::size_t i = start; i < end; i++) {
            if (info[i].shaper_pos == POS_BASE_C) {
                base = i;
                break;
            } else if (info[i].shaper_pos == POS_PRE_M) {
                if (first_left_matra == end) first_left_matra = i;
                last_left_matra = i;
            }
        }
        // https://github.com/harfbuzz/harfbuzz/issues/3863
        if (first_left_matra < last_left_matra) {
            // No need to merge clusters, handled later.
            buffer.reverse_range(first_left_matra, last_left_matra + 1);
            // Reverse back nuktas, etc.
            std::size_t i = first_left_matra;
            for (std::size_t j = i; j <= last_left_matra; j++)
                if (flag_has(info[j].shaper_cat, FL(cat::M) | FL(cat::MPst))) {
                    buffer.reverse_range(i, j + 1);
                    i = j + 1;
                }
        }

        // Things are out-of-control for post base positions, they may shuffle
        // around like crazy. In old-spec mode, we move halants around, so in
        // that case merge all clusters after base. Otherwise, check the sort
        // order and merge as needed. For pre-base stuff, we handle cluster
        // issues in final reordering.
        if (ip.is_old_spec || end - start > 127)
            buffer.merge_clusters(base, end);
        else {
            // Note! syllable() is a one-byte field.
            for (std::size_t i = base; i < end; i++)
                if (info[i].syllable != 255) {
                    std::size_t mn = i, mx = i;
                    std::size_t j = start + info[i].syllable;
                    while (j != i) {
                        mn = std::min(mn, j);
                        mx = std::max(mx, j);
                        const std::size_t next = start + info[j].syllable;
                        info[j].syllable = 255;   // so we don't process j later again
                        j = next;
                    }
                    buffer.merge_clusters(std::max(base, mn), mx + 1);
                }
        }
        // Put syllable back in.
        for (std::size_t i = start; i < end; i++) info[i].syllable = syllable;
    }

    // Setup masks now.
    {
        // Reph
        for (std::size_t i = start; i < end && info[i].shaper_pos == POS_RA_TO_BECOME_REPH; i++)
            info[i].mask |= ip.mask_array[RPHF];
        // Pre-base
        std::uint32_t mask = ip.mask_array[HALF];
        if (!ip.is_old_spec && ip.config->blwf_mode == BLWF_MODE_PRE_AND_POST) mask |= ip.mask_array[BLWF];
        for (std::size_t i = start; i < base; i++) info[i].mask |= mask;
        // Base: nothing. Post-base:
        mask = ip.mask_array[BLWF] | ip.mask_array[ABVF] | ip.mask_array[PSTF];
        for (std::size_t i = base + 1; i < end; i++) info[i].mask |= mask;
    }

    if (ip.is_old_spec && script == ucd::Script::Devanagari) {
        // Old-spec eye-lash Ra needs special handling: Ra+H not followed by
        // ZWJ gets 'blwf' (Uniscribe's behaviour).
        for (std::size_t i = start; i + 1 < base; i++)
            if (info[i].shaper_cat == cat::Ra && info[i + 1].shaper_cat == cat::H &&
                (i + 2 == base || info[i + 2].shaper_cat != cat::ZWJ)) {
                info[i].mask |= ip.mask_array[BLWF];
                info[i + 1].mask |= ip.mask_array[BLWF];
            }
    }

    constexpr std::size_t pref_len = 2;
    if (ip.mask_array[PREF] && base + pref_len < end && l) {
        // Find a Halant,Ra sequence and mark it for pre-base-reordering processing.
        for (std::size_t i = base + 1; i + pref_len - 1 < end; i++) {
            const std::uint16_t glyphs[pref_len] = {info[i].gid, info[i + 1].gid};
            if (ip.pref(plan, *l, std::span<const std::uint16_t>(glyphs, pref_len))) {
                for (std::size_t j = 0; j < pref_len; j++) info[i++].mask |= ip.mask_array[PREF];
                break;
            }
        }
    }

    // Apply ZWJ/ZWNJ effects.
    for (std::size_t i = start + 1; i < end; i++)
        if (is_joiner(info[i])) {
            const bool non_joiner = info[i].shaper_cat == cat::ZWNJ;
            std::size_t j = i;
            do {
                j--;
                // ZWJ/ZWNJ should disable CJCT (done by F_MANUAL_JOINERS);
                // ZWNJ also disables HALF.
                if (non_joiner) info[j].mask &= ~ip.mask_array[HALF];
            } while (j > start && !is_consonant(info[j]));
        }
}

inline void initial_reordering_syllable(const PlanData& ip, const Plan& plan, const Layout* l, Buffer& b,
                                        std::size_t start, std::size_t end) {
    switch (b.info[start].syllable & 0x0F) {
        case vowel_syllable:       // vowels look like consonants: call the consonant logic
        case consonant_syllable:
        case broken_cluster:       // dotted circles already inserted (standalone_cluster)
        case standalone_cluster:
            initial_reordering_consonant_syllable(ip, plan, l, b, start, end);
            break;
        default: break;   // symbol_cluster, non_indic_cluster
    }
}

inline void initial_reordering(const Plan& plan, Buffer& b, void* user) {
    const PauseContext* pc = nullptr;
    const PlanData* ip = plan_data(user, pc);
    if (!ip) return;
    update_consonant_positions(*ip, plan, *pc, b);
    syllabic::insert_dotted_circles(b, syllabic::has_broken_syllable(b, broken_cluster), pc->font->nominal,
                                    broken_cluster, cat::DOTTEDCIRCLE, cat::Repha, POS_END);
    syllabic::foreach_syllable(b, [&](std::size_t start, std::size_t end) {
        initial_reordering_syllable(*ip, plan, pc->layout, b, start, end);
    });
}

inline bool gc_letter_mark_or_other(std::uint8_t gc) noexcept {
    // FLAG_RANGE (FORMAT, NON_SPACING_MARK) in HarfBuzz's category order.
    using G = ucd::GeneralCategory;
    switch (static_cast<G>(gc)) {
        case G::Cf: case G::Cn: case G::Co: case G::Cs: case G::Ll: case G::Lm: case G::Lo: case G::Lt:
        case G::Lu: case G::Mc: case G::Me: case G::Mn: return true;
        default: return false;
    }
}

inline void final_reordering_syllable(const PlanData& ip, std::uint16_t virama, Buffer& buffer, std::size_t start,
                                      std::size_t end) {
    auto& info = buffer.info;
    const ucd::Script script = ip.script;

    // This function relies heavily on halant glyphs. Lots of ligation and
    // possibly multiple substitutions happened prior to this phase, and that
    // might have messed up our properties. Recover from a particular case of
    // that where we're fairly sure that a class of OT_H is desired but has
    // been lost.
    if (virama)
        for (std::size_t i = start; i < end; i++)
            if (info[i].gid == virama && (info[i].glyph_props & props::ligated) &&
                (info[i].glyph_props & props::multiplied)) {
                // This will make sure that this glyph passes is_halant() test.
                info[i].shaper_cat = cat::H;
                info[i].glyph_props = static_cast<std::uint16_t>(info[i].glyph_props & ~(props::ligated | props::multiplied));
            }

    // 4. Final reordering. Find base again.
    bool try_pref = ip.mask_array[PREF] != 0;
    std::size_t base;
    for (base = start; base < end; base++)
        if (info[base].shaper_pos >= POS_BASE_C) {
            if (try_pref && base + 1 < end) {
                for (std::size_t i = base + 1; i < end; i++)
                    if ((info[i].mask & ip.mask_array[PREF]) != 0) {
                        if (!((info[i].glyph_props & props::substituted) && ligated_and_didnt_multiply(info[i]))) {
                            // Ok, this was a 'pref' candidate but didn't form any.
                            // Base is around here...
                            base = i;
                            while (base < end && is_halant(info[base])) base++;
                            if (base < end) info[base].shaper_pos = POS_BASE_C;
                            try_pref = false;
                        }
                        break;
                    }
                if (base == end) break;
            }
            // For Malayalam, skip over unformed below- (but NOT post-) forms.
            if (script == ucd::Script::Malayalam) {
                for (std::size_t i = base + 1; i < end; i++) {
                    while (i < end && is_joiner(info[i])) i++;
                    if (i == end || !is_halant(info[i])) break;
                    i++;   // skip halant
                    while (i < end && is_joiner(info[i])) i++;
                    if (i < end && is_consonant(info[i]) && info[i].shaper_pos == POS_BELOW_C) {
                        base = i;
                        info[base].shaper_pos = POS_BASE_C;
                    }
                }
            }
            if (start < base && info[base].shaper_pos > POS_BASE_C) base--;
            break;
        }
    if (base == end && start < base && is_one_of(info[base - 1], FL(cat::ZWJ))) base--;
    if (base < end)
        while (start < base && is_one_of(info[base], FL(cat::N) | FL(cat::H))) base--;

    // o Reorder matras: pre-base matras move to just before the base (or
    //   after the last halant before it).
    if (start + 1 < end && start < base) {
        // If we lost track of base, alas, position before last thingy.
        std::size_t new_pos = base == end ? base - 2 : base - 1;
        // Malayalam / Tamil do not have "half" forms or explicit virama forms.
        if (script != ucd::Script::Malayalam && script != ucd::Script::Tamil) {
        search:
            while (new_pos > start && !is_one_of(info[new_pos], FL(cat::M) | FL(cat::MPst) | FL(cat::H))) new_pos--;
            // If we found no Halant we are done. Otherwise only proceed if the
            // Halant does not belong to the Matra itself!
            if (is_halant(info[new_pos]) && info[new_pos].shaper_pos != POS_PRE_M) {
                if (new_pos + 1 < end) {
                    // -> If ZWJ follows this halant, matra is NOT repositioned after this halant.
                    if (info[new_pos + 1].shaper_cat == cat::ZWJ) {
                        // Keep searching.
                        if (new_pos > start) {
                            new_pos--;
                            goto search;
                        }
                    }
                }
            } else
                new_pos = start;   // no move
        }

        if (start < new_pos && info[new_pos].shaper_pos != POS_PRE_M) {
            // Now go see if there's actually any matras...
            for (std::size_t i = new_pos; i > start; i--)
                if (info[i - 1].shaper_pos == POS_PRE_M) {
                    const std::size_t old_pos = i - 1;
                    if (old_pos < base && base <= new_pos) base--;   // shouldn't actually happen
                    move_fwd(info, old_pos, new_pos);
                    // Note: this merge_clusters() is intentionally *after* the reordering.
                    buffer.merge_clusters(new_pos, std::min(end, base + 1));
                    new_pos--;
                }
        } else {
            for (std::size_t i = start; i < base; i++)
                if (info[i].shaper_pos == POS_PRE_M) {
                    buffer.merge_clusters(i, std::min(end, base + 1));
                    break;
                }
        }
    }

    // o Reorder reph.
    if (start + 1 < end && info[start].shaper_pos == POS_RA_TO_BECOME_REPH &&
        ((info[start].shaper_cat == cat::Repha) ^ ligated_and_didnt_multiply(info[start]))) {
        std::size_t new_reph_pos;
        const std::uint8_t reph_pos = ip.config->reph_pos;
        // 1. If reph should be positioned after post-base consonant forms, proceed to step 5.
        if (reph_pos == POS_AFTER_POST) goto reph_step_5;
        // 2. If the reph repositioning class is not after post-base: target
        //    position is after the first explicit halant glyph between the
        //    first post-reph consonant and last main consonant.
        {
            new_reph_pos = start + 1;
            while (new_reph_pos < base && !is_halant(info[new_reph_pos])) new_reph_pos++;
            if (new_reph_pos < base && is_halant(info[new_reph_pos])) {
                // ->If ZWJ or ZWNJ are following this halant, position is moved after it.
                if (new_reph_pos + 1 < base && is_joiner(info[new_reph_pos + 1])) new_reph_pos++;
                goto reph_move;
            }
        }
        // 3. If reph should be repositioned after the main consonant.
        if (reph_pos == POS_AFTER_MAIN) {
            new_reph_pos = base;
            while (new_reph_pos + 1 < end && info[new_reph_pos + 1].shaper_pos <= POS_AFTER_MAIN) new_reph_pos++;
            if (new_reph_pos < end) goto reph_move;
        }
        // 4. If reph should be positioned before post-base consonant.
        if (reph_pos == POS_AFTER_SUB) {
            new_reph_pos = base;
            while (new_reph_pos + 1 < end &&
                   !flag_has(info[new_reph_pos + 1].shaper_pos, FL(POS_POST_C) | FL(POS_AFTER_POST) | FL(POS_SMVD)))
                new_reph_pos++;
            if (new_reph_pos < end) goto reph_move;
        }
    reph_step_5:
        // 5. If no consonant is found in steps 3 or 4, move reph to a
        //    position immediately before the first post-base matra, syllable
        //    modifier sign or vedic sign (the same logic as step 2).
        {
            new_reph_pos = start + 1;
            while (new_reph_pos < base && !is_halant(info[new_reph_pos])) new_reph_pos++;
            if (new_reph_pos < base && is_halant(info[new_reph_pos])) {
                if (new_reph_pos + 1 < base && is_joiner(info[new_reph_pos + 1])) new_reph_pos++;
                goto reph_move;
            }
        }
        // 6. Otherwise, reorder reph to the end of the syllable.
        {
            new_reph_pos = end - 1;
            while (new_reph_pos > start && info[new_reph_pos].shaper_pos == POS_SMVD) new_reph_pos--;
            // If the Reph is to be ending up after a Matra,Halant sequence,
            // position it before that Halant so it can interact with the Matra.
            if (!PlanData::uniscribe_bug_compatible && is_halant(info[new_reph_pos])) {
                for (std::size_t i = base + 1; i < new_reph_pos; i++)
                    if (flag_has(info[i].shaper_cat, FL(cat::M) | FL(cat::MPst))) new_reph_pos--;   // ok, got it
            }
            goto reph_move;
        }
    reph_move:
        // Move it.
        buffer.merge_clusters(start, new_reph_pos + 1);
        move_fwd(info, start, new_reph_pos);
        if (start < base && base <= new_reph_pos) base--;
    }

    // o Reorder pre-base-reordering consonants.
    if (try_pref && base + 1 < end) {
        for (std::size_t i = base + 1; i < end; i++)
            if ((info[i].mask & ip.mask_array[PREF]) != 0) {
                // 1. Only reorder a glyph produced by substitution during
                //    application of the <pref> feature.
                if (ligated_and_didnt_multiply(info[i])) {
                    // 2. Try to find a target position the same way as for pre-base matra.
                    std::size_t new_pos = base;
                    if (script != ucd::Script::Malayalam && script != ucd::Script::Tamil) {
                        while (new_pos > start && !is_one_of(info[new_pos - 1], FL(cat::M) | FL(cat::MPst) | FL(cat::H)))
                            new_pos--;
                    }
                    if (new_pos > start && is_halant(info[new_pos - 1])) {
                        // -> If ZWJ or ZWNJ follow this halant, position is moved after it.
                        if (new_pos < end && is_joiner(info[new_pos])) new_pos++;
                    }
                    {
                        const std::size_t old_pos = i;
                        buffer.merge_clusters(new_pos, old_pos + 1);
                        move_back(info, new_pos, old_pos);
                        if (new_pos <= base && base < old_pos) base++;
                    }
                }
                break;
            }
    }

    // Apply 'init' to the Left Matra if it's a word start.
    if (info[start].shaper_pos == POS_PRE_M) {
        if (!start || !gc_letter_mark_or_other(info[start - 1].gc)) info[start].mask |= ip.mask_array[INIT];
        // else: unsafe_to_break (mayag does not track glyph flags)
    }
}

inline void final_reordering(const Plan&, Buffer& b, void* user) {
    const PauseContext* pc = nullptr;
    const PlanData* ip = plan_data(user, pc);
    if (!ip || b.info.empty()) return;
    const std::uint16_t virama = virama_glyph(*ip, pc->font);
    syllabic::foreach_syllable(b, [&](std::size_t start, std::size_t end) {
        final_reordering_syllable(*ip, virama, b, start, end);
    });
}

inline void collect_features(Planner& p) {
    auto& m = p.map;
    // Do this before any lookups have been applied.
    m.add_gsub_pause(setup_syllables);
    m.enable_feature(make_tag('l', 'o', 'c', 'l'), F_PER_SYLLABLE);
    // The Indic specs do not require ccmp, but we apply it here since if
    // there is a use of it, it's typically at the beginning.
    m.enable_feature(make_tag('c', 'c', 'm', 'p'), F_PER_SYLLABLE);
    std::size_t i = 0;
    m.add_gsub_pause(initial_reordering);
    for (; i < BASIC_FEATURES; i++) {
        m.add_feature(features[i].tag, features[i].flags);
        m.add_gsub_pause(nullptr);
    }
    m.add_gsub_pause(final_reordering);
    for (; i < NUM_FEATURES; i++) m.add_feature(features[i].tag, features[i].flags);
}

inline void override_features(Planner& p) {
    p.map.disable_feature(make_tag('l', 'i', 'g', 'a'));
    p.map.add_gsub_pause(syllabic::clear_var);   // don't need syllables anymore
}

inline std::shared_ptr<void> data_create(const ShapePlan& sp) {
    auto d = std::make_shared<PlanData>();
    d->script = sp.script;
    for (std::size_t i = 1; i < std::size(configs); i++)
        if (sp.script == configs[i].script) {
            d->config = &configs[i];
            break;
        }
    d->is_old_spec = d->config->has_old_spec && ((sp.map.chosen_script[0] & 0xFFu) != '2');
    // Use zero-context would_substitute() matching for new-spec of the main
    // Indic scripts, and scripts with one spec only, but not for old-specs.
    // The new-spec for all dual-spec scripts says zero-context matching
    // happens. However, testing with Malayalam shows that old and new
    // Windows-installed fonts are the same.
    const bool zero_context = !d->is_old_spec && sp.script != ucd::Script::Malayalam;
    auto init = [&](WouldSubstitute& w, std::uint32_t tag) {
        const auto [a, e] = feature_stage_lookups(sp.map, tag);
        w.begin = a;
        w.end = e;
        w.zero_context = zero_context;
    };
    init(d->rphf, make_tag('r', 'p', 'h', 'f'));
    init(d->pref, make_tag('p', 'r', 'e', 'f'));
    init(d->blwf, make_tag('b', 'l', 'w', 'f'));
    init(d->pstf, make_tag('p', 's', 't', 'f'));
    init(d->vatu, make_tag('v', 'a', 't', 'u'));
    for (std::size_t i = 0; i < NUM_FEATURES; i++)
        d->mask_array[i] = (features[i].flags & F_GLOBAL) ? 0 : sp.map.one_mask_of(features[i].tag);
    return d;
}

inline void preprocess_text(const ShapePlan& sp, Buffer& b, const FontFuncs&) {
    if (!PlanData::uniscribe_bug_compatible) vowel_constraints::preprocess_text(sp.script, b);
}

inline void setup_masks(const ShapePlan&, Buffer& b, const FontFuncs&) {
    // We cannot setup masks here; save the information about characters and
    // setup masks later in a pause callback.
    for (auto& g : b.info) {
        const unsigned type = indic_table::get_categories(g.codepoint);
        g.shaper_cat = static_cast<std::uint8_t>(type & 0xFFu);
        g.shaper_pos = static_cast<std::uint8_t>(type >> 8);
    }
}

inline bool decompose(const detail::NormalizeContext& c, char32_t ab, char32_t& a, char32_t& b) {
    switch (ab) {
        // Don't decompose these.
        case 0x0931u:   // DEVANAGARI LETTER RRA
        // https://github.com/harfbuzz/harfbuzz/issues/779
        case 0x09DCu:   // BENGALI LETTER RRA
        case 0x09DDu:   // BENGALI LETTER RHA
        case 0x0B94u:   // TAMIL LETTER AU
            return false;
        default: break;
    }
    return detail::decompose_unicode(c, ab, a, b);
}

inline bool compose(const detail::NormalizeContext& c, char32_t a, char32_t b, char32_t& ab) {
    // Avoid recomposing split matras.
    if (ucd::is_mark(a)) return false;
    // Composition-exclusion exceptions that we want to recompose.
    if (a == 0x09AFu && b == 0x09BCu) {
        ab = 0x09DFu;
        return true;
    }
    return detail::compose_unicode(c, a, b, ab);
}

inline const ScriptShaper& shaper() {
    static const ScriptShaper s = [] {
        ScriptShaper x;
        x.name = "indic";
        x.collect_features = collect_features;
        x.override_features = override_features;
        x.data_create = data_create;
        x.preprocess_text = preprocess_text;
        x.decompose = decompose;
        x.compose = compose;
        x.setup_masks = setup_masks;
        x.normalization = Normalization::composed_diacritics_no_short_circuit;
        x.zero_marks = ZeroMarks::none;
        x.fallback_position = false;
        return x;
    }();
    return s;
}

}  // namespace mayag::typo::ot::indic
