#pragma once
// mayag::typo::ot::use — the Universal Shaping Engine (hb-ot-shaper-use.cc,
// ported rule for rule; https://docs.microsoft.com/typography/script-development/use)
//
// Design: each character gets a USE category from HarfBuzz's generated table
// (ot_shaper_use_table.hpp) in GlyphInfo::shaper_cat. A GSUB pause runs the
// Ragel syllable machine (tables in ot_shaper_use_machine.hpp, driver below,
// matching the generated C exactly), sets rphf / topographical masks, and
// later pauses record repha / pref substitutions and reorder each syllable.
// Joining scripts reuse the Arabic shaper's joining (arabic::setup_masks_plan).
//
// Port of HarfBuzz. Original copyright notice:
/*
 * Copyright © 2015  Mozilla Foundation.
 * Copyright © 2015  Google, Inc.
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
 * Mozilla Author(s): Jonathan Kew
 * Google Author(s): Behdad Esfahbod
 */

#include "ot_shape.hpp"
#include "ot_shaper_use_machine.hpp"
#include "ot_shaper_use_table.hpp"
#include "ot_syllabic.hpp"
#include "ot_vowel_constraints.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace mayag::typo::ot {
// Defined in ot_shapers.hpp, which includes this header after the Arabic shaper.
namespace arabic {
struct PlanData;
inline std::shared_ptr<void> data_create(const ShapePlan& sp);
inline void setup_masks_plan(const PlanData* d, Buffer& b, ucd::Script script);
}  // namespace arabic
}  // namespace mayag::typo::ot

namespace mayag::typo::ot::use {

namespace M = machine;

enum SyllableType : std::uint8_t {
    virama_terminated_cluster, sakot_terminated_cluster, standard_cluster,
    number_joiner_terminated_cluster, numeral_cluster, symbol_cluster,
    hieroglyph_cluster, broken_cluster, non_cluster,
};

inline constexpr std::uint32_t basic_features[] = {
    make_tag('r', 'k', 'r', 'f'), make_tag('a', 'b', 'v', 'f'), make_tag('b', 'l', 'w', 'f'),
    make_tag('h', 'a', 'l', 'f'), make_tag('p', 's', 't', 'f'), make_tag('v', 'a', 't', 'u'),
    make_tag('c', 'j', 'c', 't'),
};
inline constexpr std::uint32_t topographical_features[] = {
    make_tag('i', 's', 'o', 'l'), make_tag('i', 'n', 'i', 't'), make_tag('m', 'e', 'd', 'i'),
    make_tag('f', 'i', 'n', 'a'),
};
enum JoiningForm : std::uint8_t { FORM_ISOL, FORM_INIT, FORM_MEDI, FORM_FINA, FORM_NONE };
inline constexpr std::uint32_t other_features[] = {
    make_tag('a', 'b', 'v', 's'), make_tag('b', 'l', 'w', 's'), make_tag('h', 'a', 'l', 'n'),
    make_tag('p', 'r', 'e', 's'), make_tag('p', 's', 't', 's'),
};

/// has_arabic_joining (hb-ot-shaper-arabic-joining-list.hh).
inline bool has_arabic_joining(ucd::Script s) noexcept {
    using S = ucd::Script;
    switch (s) {
        case S::Adlam: case S::Arabic: case S::Chorasmian: case S::Hanifi_Rohingya:
        case S::Mandaic: case S::Manichaean: case S::Mongolian: case S::Nko:
        case S::Old_Uyghur: case S::Phags_Pa: case S::Psalter_Pahlavi: case S::Sogdian:
        case S::Syriac:
            return true;
        default:
            return false;
    }
}

/// The scripts hb_ot_shaper_categorize sends to USE (Myanmar Zawgyi aside).
inline bool is_use_script(ucd::Script s) noexcept {
    using S = ucd::Script;
    switch (s) {
        case S::Tibetan:
        case S::Mongolian:
        case S::Sinhala:
        case S::Buhid:
        case S::Hanunoo:
        case S::Tagalog:
        case S::Tagbanwa:
        case S::Limbu:
        case S::Tai_Le:
        case S::Buginese:
        case S::Kharoshthi:
        case S::Syloti_Nagri:
        case S::Tifinagh:
        case S::Balinese:
        case S::Nko:
        case S::Phags_Pa:
        case S::Cham:
        case S::Kayah_Li:
        case S::Lepcha:
        case S::Rejang:
        case S::Saurashtra:
        case S::Sundanese:
        case S::Egyptian_Hieroglyphs:
        case S::Javanese:
        case S::Kaithi:
        case S::Meetei_Mayek:
        case S::Tai_Tham:
        case S::Tai_Viet:
        case S::Batak:
        case S::Brahmi:
        case S::Mandaic:
        case S::Chakma:
        case S::Miao:
        case S::Sharada:
        case S::Takri:
        case S::Duployan:
        case S::Grantha:
        case S::Khojki:
        case S::Khudawadi:
        case S::Mahajani:
        case S::Manichaean:
        case S::Modi:
        case S::Pahawh_Hmong:
        case S::Psalter_Pahlavi:
        case S::Siddham:
        case S::Tirhuta:
        case S::Ahom:
        case S::Multani:
        case S::Adlam:
        case S::Bhaiksuki:
        case S::Marchen:
        case S::Newa:
        case S::Masaram_Gondi:
        case S::Soyombo:
        case S::Zanabazar_Square:
        case S::Dogra:
        case S::Gunjala_Gondi:
        case S::Hanifi_Rohingya:
        case S::Makasar:
        case S::Medefaidrin:
        case S::Old_Sogdian:
        case S::Sogdian:
        case S::Elymaic:
        case S::Nandinagari:
        case S::Nyiakeng_Puachue_Hmong:
        case S::Wancho:
        case S::Chorasmian:
        case S::Dives_Akuru:
        case S::Khitan_Small_Script:
        case S::Yezidi:
        case S::Cypro_Minoan:
        case S::Old_Uyghur:
        case S::Tangsa:
        case S::Toto:
        case S::Vithkuqi:
        case S::Kawi:
        case S::Nag_Mundari:
        case S::Garay:
        case S::Gurung_Khema:
        case S::Kirat_Rai:
        case S::Ol_Onal:
        case S::Sunuwar:
        case S::Todhri:
        case S::Tulu_Tigalari:
            return true;
        default:
            return false;
    }
}

struct PlanData {
    std::uint32_t rphf_mask = 0;
    std::shared_ptr<void> arabic_plan;
};

inline const PlanData& plan_data(void* user) {
    return *static_cast<const PlanData*>(static_cast<const PauseContext*>(user)->plan->data.get());
}

/// find_syllables_use: the Ragel machine over the glyphs that are neither
/// CGJ nor a ZWNJ followed by a mark, exactly as the generated driver runs.
inline void find_syllables(Buffer& b) {
    const std::size_t len = b.len();
    auto& info = b.info;
    auto not_ccs_default_ignorable = [&](std::size_t i) { return info[i].shaper_cat != M::CGJ; };
    std::vector<std::size_t> orig;   // filtered index -> buffer index, plus sentinel
    orig.reserve(len + 1);
    for (std::size_t i = 0; i < len; ++i) {
        if (!not_ccs_default_ignorable(i)) continue;
        bool keep = true;
        if (info[i].shaper_cat == M::ZWNJ)
            for (std::size_t j = i + 1; j < len; ++j)
                if (not_ccs_default_ignorable(j)) { keep = !is_unicode_mark(info[j]); break; }
        if (keep) orig.push_back(i);
    }
    const std::size_t n = orig.size();
    orig.push_back(len);

    std::size_t p = 0, pe = n, eof = n, ts = 0, te = 0;
    unsigned act = 0;
    int cs = M::start;
    unsigned syllable_serial = 1;
    auto found = [&](SyllableType t) {
        for (std::size_t i = orig[ts]; i < orig[te]; ++i)
            info[i].syllable = static_cast<std::uint8_t>((syllable_serial << 4) | t);
        if (++syllable_serial == 16) syllable_serial = 1;
    };
    bool eof_trans = false;
    int trans = 0;
    if (p == pe) goto test_eof;
    for (;;) {
        if (!eof_trans) {
            if (M::from_state_actions[cs] == 3) ts = p;
            const std::uint8_t* keys = M::trans_keys + (cs << 1);
            const std::uint8_t* inds = M::indicies + M::index_offsets[cs];
            const int slen = M::key_spans[cs];
            const unsigned c = info[orig[p]].shaper_cat;
            trans = inds[slen > 0 && keys[0] <= c && c <= keys[1] ? static_cast<int>(c - keys[0]) : slen];
        }
        eof_trans = false;
        cs = M::trans_targs[trans];
        if (M::trans_actions[trans] != 0) {
            switch (M::trans_actions[trans]) {
                case 7: te = p + 1; break;
                case 16: te = p + 1; found(virama_terminated_cluster); break;
                case 14: te = p + 1; found(sakot_terminated_cluster); break;
                case 12: te = p + 1; found(standard_cluster); break;
                case 20: te = p + 1; found(number_joiner_terminated_cluster); break;
                case 18: te = p + 1; found(numeral_cluster); break;
                case 10: te = p + 1; found(symbol_cluster); break;
                case 25: te = p + 1; found(hieroglyph_cluster); break;
                case 5: te = p + 1; found(broken_cluster); break;
                case 4: te = p + 1; found(non_cluster); break;
                case 15: te = p; --p; found(virama_terminated_cluster); break;
                case 13: te = p; --p; found(sakot_terminated_cluster); break;
                case 11: te = p; --p; found(standard_cluster); break;
                case 19: te = p; --p; found(number_joiner_terminated_cluster); break;
                case 17: te = p; --p; found(numeral_cluster); break;
                case 9: te = p; --p; found(symbol_cluster); break;
                case 24: te = p; --p; found(hieroglyph_cluster); break;
                case 21: te = p; --p; found(broken_cluster); break;
                case 23: te = p; --p; found(non_cluster); break;
                case 1: p = te - 1; found(symbol_cluster); break;
                case 22:
                    switch (act) {
                        case 8: p = te - 1; found(non_cluster); break;
                        case 9: p = te - 1; found(broken_cluster); break;
                        default: break;
                    }
                    break;
                case 6: te = p + 1; act = 8; break;
                case 8: te = p + 1; act = 9; break;
                default: break;
            }
        }
        if (M::to_state_actions[cs] == 2) ts = 0;
        // `--p` above may wrap below zero; ++p brings it back (Ragel's p-- then ++p).
        if (++p != pe) continue;
    test_eof:
        if (p == eof && M::eof_trans[cs] > 0) {
            trans = M::eof_trans[cs] - 1;
            eof_trans = true;
            continue;
        }
        break;
    }
}

inline void setup_rphf_mask(const PlanData& d, Buffer& b) {
    const std::uint32_t mask = d.rphf_mask;
    if (!mask) return;
    syllabic::foreach_syllable(b, [&](std::size_t start, std::size_t end) {
        const std::size_t limit = b.info[start].shaper_cat == M::R ? 1 : std::min<std::size_t>(3, end - start);
        for (std::size_t i = start; i < start + limit; ++i) b.info[i].mask |= mask;
    });
}

inline void setup_topographical_masks(const Plan& plan, const PlanData& d, Buffer& b) {
    if (d.arabic_plan) return;
    std::uint32_t masks[4], all_masks = 0;
    for (unsigned i = 0; i < 4; ++i) {
        masks[i] = plan.one_mask_of(topographical_features[i]);
        if (masks[i] == plan.global_mask()) masks[i] = 0;
        all_masks |= masks[i];
    }
    if (!all_masks) return;
    const std::uint32_t other_masks = ~all_masks;
    std::size_t last_start = 0;
    JoiningForm last_form = FORM_NONE;
    auto& info = b.info;
    syllabic::foreach_syllable(b, [&](std::size_t start, std::size_t end) {
        switch (static_cast<SyllableType>(info[start].syllable & 0x0F)) {
            case hieroglyph_cluster:
            case non_cluster:
                last_form = FORM_NONE;   // these don't join
                break;
            case virama_terminated_cluster: case sakot_terminated_cluster: case standard_cluster:
            case number_joiner_terminated_cluster: case numeral_cluster: case symbol_cluster:
            case broken_cluster: {
                const bool join = last_form == FORM_FINA || last_form == FORM_ISOL;
                if (join) {   // fix up the previous syllable's form
                    last_form = last_form == FORM_FINA ? FORM_MEDI : FORM_INIT;
                    for (std::size_t i = last_start; i < start; ++i)
                        info[i].mask = (info[i].mask & other_masks) | masks[last_form];
                }
                last_form = join ? FORM_FINA : FORM_ISOL;
                for (std::size_t i = start; i < end; ++i)
                    info[i].mask = (info[i].mask & other_masks) | masks[last_form];
                break;
            }
        }
        last_start = start;
    });
}

inline void setup_syllables(const Plan& plan, Buffer& b, void* user) {
    const PlanData& d = plan_data(user);
    find_syllables(b);
    setup_rphf_mask(d, b);
    setup_topographical_masks(plan, d, b);
}

/// _hb_clear_substitution_flags.
inline void clear_substitution_flags(const Plan&, Buffer& b, void*) {
    for (auto& g : b.info) g.glyph_props = static_cast<std::uint16_t>(g.glyph_props & ~props::substituted);
}

inline void record_rphf(const Plan&, Buffer& b, void* user) {
    const std::uint32_t mask = plan_data(user).rphf_mask;
    if (!mask) return;
    syllabic::foreach_syllable(b, [&](std::size_t start, std::size_t end) {
        // Mark a substituted repha as USE(R).
        for (std::size_t i = start; i < end && (b.info[i].mask & mask); ++i)
            if (b.info[i].glyph_props & props::substituted) { b.info[i].shaper_cat = M::R; break; }
    });
}

inline void record_pref(const Plan&, Buffer& b, void*) {
    syllabic::foreach_syllable(b, [&](std::size_t start, std::size_t end) {
        // Mark a substituted pref as VPre, as they behave the same way.
        for (std::size_t i = start; i < end; ++i)
            if (b.info[i].glyph_props & props::substituted) { b.info[i].shaper_cat = M::VPre; break; }
    });
}

inline bool is_halant(const GlyphInfo& g) noexcept {
    return (g.shaper_cat == M::H || g.shaper_cat == M::HVM || g.shaper_cat == M::IS) &&
           !(g.glyph_props & props::ligated);
}

[[nodiscard]] constexpr std::uint64_t flag64(unsigned x) noexcept { return x < 64 ? std::uint64_t{1} << x : 0; }

inline void reorder_syllable(Buffer& b, std::size_t start, std::size_t end) {
    const unsigned syllable_type = b.info[start].syllable & 0x0F;
    // Only a few syllable types need reordering.
    constexpr unsigned reorder_types = (1u << virama_terminated_cluster) | (1u << sakot_terminated_cluster) |
                                       (1u << standard_cluster) | (1u << symbol_cluster) | (1u << broken_cluster);
    if (!((1u << syllable_type) & reorder_types)) return;
    auto& info = b.info;
    constexpr std::uint64_t post_base_flags64 =
        flag64(M::FAbv) | flag64(M::FBlw) | flag64(M::FPst) | flag64(M::FMAbv) | flag64(M::FMBlw) |
        flag64(M::FMPst) | flag64(M::MAbv) | flag64(M::MBlw) | flag64(M::MPst) | flag64(M::MPre) |
        flag64(M::VAbv) | flag64(M::VBlw) | flag64(M::VPst) | flag64(M::VPre) | flag64(M::VMAbv) |
        flag64(M::VMBlw) | flag64(M::VMPst) | flag64(M::VMPre);

    // Move things forward.
    if (info[start].shaper_cat == M::R && end - start > 1) {
        // Got a repha. Reorder it towards the end, but before the first post-base glyph.
        for (std::size_t i = start + 1; i < end; ++i) {
            const bool is_post_base_glyph = (flag64(info[i].shaper_cat) & post_base_flags64) || is_halant(info[i]);
            if (is_post_base_glyph || i == end - 1) {
                // If we hit a post-base glyph, move before it; otherwise move to the end.
                if (is_post_base_glyph) --i;
                b.merge_clusters(start, i + 1);
                const GlyphInfo t = info[start];
                std::move(info.begin() + static_cast<std::ptrdiff_t>(start) + 1,
                          info.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                          info.begin() + static_cast<std::ptrdiff_t>(start));
                info[i] = t;
                break;
            }
        }
    }

    // Move things back.
    std::size_t j = start;
    for (std::size_t i = start; i < end; ++i) {
        const std::uint64_t flag = flag64(info[i].shaper_cat);
        if (is_halant(info[i])) {
            // If we hit a halant, move after it; otherwise move to the beginning.
            j = i + 1;
        } else if ((flag & (flag64(M::VPre) | flag64(M::VMPre))) &&
                   // Only move the first component of a MultipleSubst.
                   lig::comp(info[i]) == 0 && j < i) {
            b.merge_clusters(j, i + 1);
            const GlyphInfo t = info[i];
            std::move_backward(info.begin() + static_cast<std::ptrdiff_t>(j),
                               info.begin() + static_cast<std::ptrdiff_t>(i),
                               info.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            info[j] = t;
        }
    }
}

inline void reorder(const Plan&, Buffer& b, void* user) {
    const auto& pc = *static_cast<const PauseContext*>(user);
    syllabic::insert_dotted_circles(b, syllabic::has_broken_syllable(b, broken_cluster), pc.font->nominal,
                                    broken_cluster, M::B, M::R);
    syllabic::foreach_syllable(b, [&](std::size_t start, std::size_t end) { reorder_syllable(b, start, end); });
}

inline void collect_features(Planner& p) {
    PlanBuilder& m = p.map;
    // Do this before any lookups have been applied.
    m.add_gsub_pause(setup_syllables);
    // "Default glyph pre-processing group"
    m.enable_feature(make_tag('l', 'o', 'c', 'l'), F_PER_SYLLABLE);
    m.enable_feature(make_tag('c', 'c', 'm', 'p'), F_PER_SYLLABLE);
    m.enable_feature(make_tag('n', 'u', 'k', 't'), F_PER_SYLLABLE);
    m.enable_feature(make_tag('a', 'k', 'h', 'n'), F_MANUAL_ZWJ | F_PER_SYLLABLE);
    // "Reordering group"
    m.add_gsub_pause(clear_substitution_flags);
    m.add_feature(make_tag('r', 'p', 'h', 'f'), F_MANUAL_ZWJ | F_PER_SYLLABLE);
    m.add_gsub_pause(record_rphf);
    m.add_gsub_pause(clear_substitution_flags);
    m.enable_feature(make_tag('p', 'r', 'e', 'f'), F_MANUAL_ZWJ | F_PER_SYLLABLE);
    m.add_gsub_pause(record_pref);
    // "Orthographic unit shaping group"
    for (std::uint32_t t : basic_features) m.enable_feature(t, F_MANUAL_ZWJ | F_PER_SYLLABLE);
    m.add_gsub_pause(reorder);
    m.add_gsub_pause(syllabic::clear_var);
    // "Topographical features"
    for (std::uint32_t t : topographical_features) m.add_feature(t);
    m.add_gsub_pause(nullptr);
    // "Standard typographic presentation"
    for (std::uint32_t t : other_features) m.enable_feature(t, F_MANUAL_ZWJ);
}

inline std::shared_ptr<void> data_create(const ShapePlan& sp) {
    auto d = std::make_shared<PlanData>();
    d->rphf_mask = sp.map.one_mask_of(make_tag('r', 'p', 'h', 'f'));
    if (has_arabic_joining(sp.script)) d->arabic_plan = arabic::data_create(sp);
    return d;
}

inline void setup_masks(const ShapePlan& sp, Buffer& b, const FontFuncs&) {
    const auto* d = static_cast<const PlanData*>(sp.data.get());
    // Do this before allocating use_category().
    if (d->arabic_plan)
        arabic::setup_masks_plan(static_cast<const arabic::PlanData*>(d->arabic_plan.get()), b, sp.script);
    for (auto& g : b.info) g.shaper_cat = table::get_category(g.codepoint);
}

inline void preprocess_text(const ShapePlan& sp, Buffer& b, const FontFuncs&) {
    vowel_constraints::preprocess_text(sp.script, b);
}

inline bool compose(const detail::NormalizeContext& c, char32_t a, char32_t b, char32_t& ab) {
    // Avoid recomposing split matras.
    if (detail::gc_is_mark(static_cast<std::uint8_t>(ucd::general_category(a)))) return false;
    return detail::compose_unicode(c, a, b, ab);
}

inline const ScriptShaper& shaper() {
    static const ScriptShaper s = [] {
        ScriptShaper x;
        x.name = "use";
        x.collect_features = collect_features;
        x.data_create = data_create;
        x.preprocess_text = preprocess_text;
        x.compose = compose;
        x.setup_masks = setup_masks;
        x.normalization = Normalization::composed_diacritics_no_short_circuit;
        x.zero_marks = ZeroMarks::early;
        x.fallback_position = false;
        return x;
    }();
    return s;
}

}  // namespace mayag::typo::ot::use

// The Arabic joining code USE reuses (declared above) lives here.
#include "ot_shapers.hpp"
