#pragma once
// mayag::typo::ot::khmer — the Khmer shaper, a rule-for-rule port of
// HarfBuzz's hb-ot-shaper-khmer.cc and the driver of its Ragel-generated
// syllable machine (hb-ot-shaper-khmer-machine.hh; the tables were extracted
// verbatim into ot_shaper_khmer_machine.hpp).
//
// Design: setup_masks stores each character's Indic-table category in
// GlyphInfo::shaper_cat. A first GSUB pause runs the syllable machine; a
// second inserts dotted circles into broken clusters and reorders each
// syllable (coeng+ro and pre-base vowels move to the front, masks for the
// per-syllable features are set). The basic features apply per syllable,
// then the presentation features apply globally.
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
#include "ot_shaper_indic_table.hpp"
#include "ot_shaper_khmer_machine.hpp"
#include "ot_syllabic.hpp"

#include <cstdint>
#include <memory>

namespace mayag::typo::ot::khmer {

namespace cat = machine;   // K_Cat(x) == cat::x

enum SyllableType : std::uint8_t { consonant_syllable, broken_cluster, non_khmer_cluster };

struct Feature { std::uint32_t tag; std::uint32_t flags; };
inline constexpr Feature features[] = {
    // Basic features: applied all at once, before reordering, per syllable.
    {make_tag('p', 'r', 'e', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('b', 'l', 'w', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('a', 'b', 'v', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('p', 's', 't', 'f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    {make_tag('c', 'f', 'a', 'r'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
    // Other features: applied all at once after clearing syllables.
    {make_tag('p', 'r', 'e', 's'), F_GLOBAL_MANUAL_JOINERS},
    {make_tag('a', 'b', 'v', 's'), F_GLOBAL_MANUAL_JOINERS},
    {make_tag('b', 'l', 'w', 's'), F_GLOBAL_MANUAL_JOINERS},
    {make_tag('p', 's', 't', 's'), F_GLOBAL_MANUAL_JOINERS},
};
enum : std::uint8_t { PREF, BLWF, ABVF, PSTF, CFAR, PRES_, ABVS_, BLWS_, PSTS_, NUM_FEATURES, BASIC_FEATURES = PRES_ };

struct PlanData { std::uint32_t mask_array[NUM_FEATURES] = {}; };

/// find_syllables_khmer: the Ragel machine's driver, kept goto-for-goto.
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
    if (from_state_actions[cs] == 7) ts = p;
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
        case 2: te = p + 1; break;
        case 8: te = p + 1; found_syllable(non_khmer_cluster); break;
        case 10: te = p; p--; found_syllable(consonant_syllable); break;
        case 11: te = p; p--; found_syllable(broken_cluster); break;
        case 12: te = p; p--; found_syllable(non_khmer_cluster); break;
        case 1: p = te - 1; found_syllable(consonant_syllable); break;
        case 3: p = te - 1; found_syllable(broken_cluster); break;
        case 5:
            switch (act) {
                case 2: p = te - 1; found_syllable(broken_cluster); break;
                case 3: p = te - 1; found_syllable(non_khmer_cluster); break;
                default: break;
            }
            break;
        case 4: te = p + 1; act = 2; break;
        case 9: te = p + 1; act = 3; break;
        default: break;
    }
again:
    if (to_state_actions[cs] == 6) ts = 0;
    if (++p != pe) goto resume;
test_eof:
    if (p == eof && eof_trans[cs] > 0) {
        trans = eof_trans[cs] - 1;
        goto eof_trans;
    }
}

inline void setup_syllables(const Plan&, Buffer& b, void*) {
    find_syllables(b);
    // (unsafe_to_break per syllable: mayag does not track glyph flags.)
}

/// reorder_consonant_syllable (Microsoft's Khmer rules).
inline void reorder_consonant_syllable(const PlanData& kp, Buffer& buffer, std::size_t start, std::size_t end) {
    auto& info = buffer.info;
    // Setup masks: post-base.
    const std::uint32_t mask = kp.mask_array[BLWF] | kp.mask_array[ABVF] | kp.mask_array[PSTF];
    for (std::size_t i = start + 1; i < end; i++) info[i].mask |= mask;

    unsigned num_coengs = 0;
    for (std::size_t i = start + 1; i < end; i++) {
        // COENG + RO is reordered to immediately before the base (type 2
        // subscript); other subscripts stay put.
        if (info[i].shaper_cat == cat::H && num_coengs <= 2 && i + 1 < end) {
            num_coengs++;
            if (info[i + 1].shaper_cat == cat::Ra) {
                for (std::size_t j = 0; j < 2; j++) info[i + j].mask |= kp.mask_array[PREF];
                buffer.merge_clusters(start, i + 2);
                const GlyphInfo t0 = info[i], t1 = info[i + 1];
                for (std::size_t k = i; k-- > start;) info[k + 2] = info[k];
                info[start] = t0;
                info[start + 1] = t1;
                // Mark the subsequent stuff with 'cfar'.
                if (kp.mask_array[CFAR])
                    for (std::size_t j = i + 2; j < end; j++) info[j].mask |= kp.mask_array[CFAR];
            }
        } else if (info[i].shaper_cat == cat::VPre) {
            // Move to the start.
            buffer.merge_clusters(start, i + 1);
            const GlyphInfo t = info[i];
            for (std::size_t k = i; k-- > start;) info[k + 1] = info[k];
            info[start] = t;
        }
    }
}

inline void reorder(const Plan&, Buffer& b, void* user) {
    const auto* pc = static_cast<const PauseContext*>(user);
    const auto* kp = static_cast<const PlanData*>(pc->plan->data.get());
    if (!kp) return;
    syllabic::insert_dotted_circles(b, syllabic::has_broken_syllable(b, broken_cluster), pc->font->nominal,
                                    broken_cluster, cat::DOTTEDCIRCLE, -1);
    syllabic::foreach_syllable(b, [&](std::size_t start, std::size_t end) {
        switch (b.info[start].syllable & 0x0F) {
            case broken_cluster:   // dotted circles already inserted
            case consonant_syllable: reorder_consonant_syllable(*kp, b, start, end); break;
            default: break;
        }
    });
}

inline void collect_features(Planner& p) {
    auto& m = p.map;
    // Do this before any lookups have been applied.
    m.add_gsub_pause(setup_syllables);
    m.add_gsub_pause(reorder);
    // Uniscribe does NOT pause between basic features (harfbuzz#974).
    m.enable_feature(make_tag('l', 'o', 'c', 'l'), F_PER_SYLLABLE);
    m.enable_feature(make_tag('c', 'c', 'm', 'p'), F_PER_SYLLABLE);
    std::size_t i = 0;
    for (; i < BASIC_FEATURES; i++) m.add_feature(features[i].tag, features[i].flags);
    m.add_gsub_pause(syllabic::clear_var);   // harfbuzz#3531
    for (; i < NUM_FEATURES; i++) m.add_feature(features[i].tag, features[i].flags);
}

inline void override_features(Planner& p) {
    // The Khmer spec has 'clig' among the required shaping features.
    p.map.enable_feature(make_tag('c', 'l', 'i', 'g'));
    p.map.disable_feature(make_tag('l', 'i', 'g', 'a'));
}

inline std::shared_ptr<void> data_create(const ShapePlan& sp) {
    auto d = std::make_shared<PlanData>();
    for (std::size_t i = 0; i < NUM_FEATURES; i++)
        d->mask_array[i] = (features[i].flags & F_GLOBAL) ? 0 : sp.map.one_mask_of(features[i].tag);
    return d;
}

inline void setup_masks(const ShapePlan&, Buffer& b, const FontFuncs&) {
    // Masks are set later, in the reorder pause; save the categories now.
    for (auto& g : b.info) g.shaper_cat = static_cast<std::uint8_t>(indic_table::get_categories(g.codepoint) & 0xFF);
}

inline bool decompose(const detail::NormalizeContext& c, char32_t ab, char32_t& a, char32_t& b) {
    switch (ab) {   // split matras without Unicode decompositions
        case 0x17BE: case 0x17BF: case 0x17C0: case 0x17C4: case 0x17C5: a = 0x17C1; b = ab; return true;
        default: break;
    }
    return detail::decompose_unicode(c, ab, a, b);
}

inline bool compose(const detail::NormalizeContext& c, char32_t a, char32_t b, char32_t& ab) {
    // Avoid recomposing split matras.
    if (ucd::is_mark(a)) return false;
    return detail::compose_unicode(c, a, b, ab);
}

inline const ScriptShaper& shaper() {
    static const ScriptShaper s = [] {
        ScriptShaper x;
        x.name = "khmer";
        x.collect_features = collect_features;
        x.override_features = override_features;
        x.data_create = data_create;
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

}  // namespace mayag::typo::ot::khmer
