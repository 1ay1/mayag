#pragma once
// mayag::typo::ot::myanmar — the Myanmar shaper, a rule-for-rule port of
// HarfBuzz's hb-ot-shaper-myanmar.cc and the driver of its Ragel-generated
// syllable machine (hb-ot-shaper-myanmar-machine.hh; tables extracted
// verbatim into ot_shaper_myanmar_machine.hpp).
//
// Design: setup_masks stores the Indic-table category in GlyphInfo::shaper_cat;
// a GSUB pause before any lookup finds syllables; after locl/ccmp a second
// pause inserts dotted circles and reorders each syllable by assigning
// positions (GlyphInfo::shaper_pos) and stable-sorting: kinzi moves after
// the base, medial ra and the pre-base vowel move before it. The basic
// features apply per syllable, one at a time; the rest apply globally.
//
// Port of HarfBuzz. Original copyright notice:
/*
 * Copyright © 2011,2012,2013  Google, Inc.
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
#include "ot_shaper_indic_table.hpp"
#include "ot_shaper_myanmar_machine.hpp"
#include "ot_syllabic.hpp"

#include <cstdint>

namespace mayag::typo::ot::myanmar {

namespace cat = machine;   // M_Cat(x) == cat::x

enum SyllableType : std::uint8_t { consonant_syllable, broken_cluster, non_myanmar_cluster };

/// ot_position_t (hb-ot-shaper-indic.hh), the values Myanmar uses.
enum Position : std::uint8_t {
    POS_PRE_M = 2, POS_PRE_C = 3, POS_BASE_C = 4, POS_AFTER_MAIN = 5, POS_BEFORE_SUB = 7,
    POS_BELOW_C = 8, POS_AFTER_SUB = 9, POS_END = 14,
};

inline constexpr std::uint32_t basic_features[] = {   // one at a time, per syllable
    make_tag('r', 'p', 'h', 'f'), make_tag('p', 'r', 'e', 'f'),
    make_tag('b', 'l', 'w', 'f'), make_tag('p', 's', 't', 'f'),
};
inline constexpr std::uint32_t other_features[] = {   // all at once, after syllables
    make_tag('p', 'r', 'e', 's'), make_tag('a', 'b', 'v', 's'),
    make_tag('b', 'l', 'w', 's'), make_tag('p', 's', 't', 's'),
};

inline bool is_consonant(const GlyphInfo& g) noexcept {
    if (g.glyph_props & props::ligated) return false;   // if it ligated, all bets are off
    switch (g.shaper_cat) {
        case cat::C: case cat::CS: case cat::Ra: case cat::IV: case cat::GB: case cat::DOTTEDCIRCLE: return true;
        default: return false;
    }
}

/// find_syllables_myanmar: the Ragel machine's driver, kept goto-for-goto.
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
    if (from_state_actions[cs] == 2) ts = p;
    {
        const std::uint8_t* keys = trans_keys + (cs << 1);
        const std::int8_t* inds = indicies + index_offsets[cs];
        const int slen = key_spans[cs];
        const unsigned c = info[p].shaper_cat;
        trans = inds[slen > 0 && keys[0] <= c && c <= keys[1] ? static_cast<int>(c - keys[0]) : slen];
    }
eof_trans:
    cs = trans_targs[trans];
    if (trans_actions[trans] == 0) goto again;
    switch (trans_actions[trans]) {
        case 8: te = p + 1; found_syllable(consonant_syllable); break;
        case 4: te = p + 1; found_syllable(non_myanmar_cluster); break;
        case 10: te = p + 1; found_syllable(broken_cluster); break;
        case 3: te = p + 1; found_syllable(non_myanmar_cluster); break;
        case 7: te = p; p--; found_syllable(consonant_syllable); break;
        case 9: te = p; p--; found_syllable(broken_cluster); break;
        case 12: te = p; p--; found_syllable(non_myanmar_cluster); break;
        case 11:
            switch (act) {
                case 2: p = te - 1; found_syllable(non_myanmar_cluster); break;
                case 3: p = te - 1; found_syllable(broken_cluster); break;
                default: break;
            }
            break;
        case 6: te = p + 1; act = 2; break;
        case 5: te = p + 1; act = 3; break;
        default: break;
    }
again:
    if (to_state_actions[cs] == 1) ts = 0;
    if (++p != pe) goto resume;
test_eof:
    if (p == eof && eof_trans[cs] > 0) {
        trans = eof_trans[cs] - 1;
        goto eof_trans;
    }
}

inline void setup_syllables(const Plan&, Buffer& b, void*) { find_syllables(b); }

/// hb_buffer_t::sort: stable insertion sort merging clusters of moved glyphs.
inline void sort_by_position(Buffer& b, std::size_t start, std::size_t end) {
    auto& info = b.info;
    for (std::size_t i = start + 1; i < end; i++) {
        std::size_t j = i;
        while (j > start && info[j - 1].shaper_pos > info[i].shaper_pos) j--;
        if (i == j) continue;
        b.merge_clusters(j, i + 1);
        const GlyphInfo t = info[i];
        for (std::size_t k = i; k > j; k--) info[k] = info[k - 1];
        info[j] = t;
    }
}

/// initial_reordering_consonant_syllable (Microsoft's Myanmar rules).
inline void reorder_consonant_syllable(Buffer& buffer, std::size_t start, std::size_t end) {
    auto& info = buffer.info;
    std::size_t base = end;
    bool has_reph = false;
    {
        std::size_t limit = start;
        if (start + 3 <= end && info[start].shaper_cat == cat::Ra && info[start + 1].shaper_cat == cat::As &&
            info[start + 2].shaper_cat == cat::H) {
            limit += 3;
            base = start;
            has_reph = true;
        }
        if (!has_reph) base = limit;
        for (std::size_t i = limit; i < end; i++)
            if (is_consonant(info[i])) { base = i; break; }
    }

    // Reorder!
    {
        std::size_t i = start;
        for (; i < start + (has_reph ? 3 : 0); i++) info[i].shaper_pos = POS_AFTER_MAIN;
        for (; i < base; i++) info[i].shaper_pos = POS_PRE_C;
        if (i < end) { info[i].shaper_pos = POS_BASE_C; i++; }
        std::uint8_t pos = POS_AFTER_MAIN;
        // The following loop may be ugly, but it implements all of Myanmar reordering!
        for (; i < end; i++) {
            const std::uint8_t c = info[i].shaper_cat;
            if (c == cat::MR) { info[i].shaper_pos = POS_PRE_C; continue; }       // pre-base reordering
            if (c == cat::VPre) { info[i].shaper_pos = POS_PRE_M; continue; }     // left matra
            if (c == cat::VS) { info[i].shaper_pos = info[i - 1].shaper_pos; continue; }
            if (pos == POS_AFTER_MAIN && c == cat::VBlw) { pos = POS_BELOW_C; info[i].shaper_pos = pos; continue; }
            if (pos == POS_BELOW_C && c == cat::A) { info[i].shaper_pos = POS_BEFORE_SUB; continue; }
            if (pos == POS_BELOW_C && c == cat::VBlw) { info[i].shaper_pos = pos; continue; }
            if (pos == POS_BELOW_C && c != cat::A) { pos = POS_AFTER_SUB; info[i].shaper_pos = pos; continue; }
            info[i].shaper_pos = pos;
        }
    }

    // Sit tight, rock 'n roll!
    sort_by_position(buffer, start, end);

    // Flip left-matra sequence (harfbuzz#3863).
    std::size_t first_left_matra = end, last_left_matra = end;
    for (std::size_t i = start; i < end; i++)
        if (info[i].shaper_pos == POS_PRE_M) {
            if (first_left_matra == end) first_left_matra = i;
            last_left_matra = i;
        }
    if (first_left_matra < last_left_matra) {
        buffer.reverse_range(first_left_matra, last_left_matra + 1);
        // Reverse back VS, etc.
        std::size_t i = first_left_matra;
        for (std::size_t j = i; j <= last_left_matra; j++)
            if (info[j].shaper_cat == cat::VPre) {
                buffer.reverse_range(i, j + 1);
                i = j + 1;
            }
    }
}

inline void reorder(const Plan&, Buffer& b, void* user) {
    const auto* pc = static_cast<const PauseContext*>(user);
    syllabic::insert_dotted_circles(b, syllabic::has_broken_syllable(b, broken_cluster), pc->font->nominal,
                                    broken_cluster, cat::DOTTEDCIRCLE);
    syllabic::foreach_syllable(b, [&](std::size_t start, std::size_t end) {
        switch (b.info[start].syllable & 0x0F) {
            case broken_cluster:   // dotted circles already inserted
            case consonant_syllable: reorder_consonant_syllable(b, start, end); break;
            default: break;
        }
    });
}

inline void collect_features(Planner& p) {
    auto& m = p.map;
    m.add_gsub_pause(setup_syllables);   // before any lookups have been applied
    m.enable_feature(make_tag('l', 'o', 'c', 'l'), F_PER_SYLLABLE);
    m.enable_feature(make_tag('c', 'c', 'm', 'p'), F_PER_SYLLABLE);
    m.add_gsub_pause(reorder);
    for (std::uint32_t f : basic_features) {
        m.enable_feature(f, F_MANUAL_ZWJ | F_PER_SYLLABLE);
        m.add_gsub_pause(nullptr);
    }
    m.add_gsub_pause(syllabic::clear_var);
    for (std::uint32_t f : other_features) m.enable_feature(f, F_MANUAL_ZWJ);
}

inline void setup_masks(const ShapePlan&, Buffer& b, const FontFuncs&) {
    // No masks, we just save information about characters.
    for (auto& g : b.info) g.shaper_cat = static_cast<std::uint8_t>(indic_table::get_categories(g.codepoint) & 0xFF);
}

inline const ScriptShaper& shaper() {
    static const ScriptShaper s = [] {
        ScriptShaper x;
        x.name = "myanmar";
        x.collect_features = collect_features;
        x.setup_masks = setup_masks;
        x.normalization = Normalization::composed_diacritics_no_short_circuit;
        x.zero_marks = ZeroMarks::early;
        x.fallback_position = false;
        return x;
    }();
    return s;
}

}  // namespace mayag::typo::ot::myanmar
