#pragma once
// mayag::typo::ot::syllabic — helpers shared by the syllable-based shapers
// (Indic, Khmer, Myanmar, USE): HarfBuzz's hb-ot-shaper-syllabic.cc.
//
// insert_dotted_circles() gives every broken syllable (a mark with nothing to
// sit on) a U+25CC base, after any leading repha. It runs inside a GSUB pause,
// so the inserted info carries the GLYPH id in `gid` (codepoint = U+25CC).
// The shaper category lives in GlyphInfo::shaper_cat, the position
// ("auxiliary" in HarfBuzz) in GlyphInfo::shaper_pos.
//
// Port of HarfBuzz. Original copyright notice:
/*
 * Copyright © 2021  Behdad Esfahbod.
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
 */

#include "ot_layout.hpp"

#include <cstdint>
#include <functional>

namespace mayag::typo::ot::syllabic {

/// hb_syllabic_insert_dotted_circles. `has_broken_syllable` stands in for
/// HB_BUFFER_SCRATCH_FLAG_HAS_BROKEN_SYLLABLE; `nominal` maps U+25CC.
/// repha_category / dottedcircle_position: -1 = none.
inline bool insert_dotted_circles(Buffer& b, bool has_broken_syllable,
                                  const std::function<bool(char32_t, std::uint16_t&)>& nominal,
                                  unsigned broken_syllable_type, unsigned dottedcircle_category,
                                  int repha_category = -1, int dottedcircle_position = -1) {
    if (!has_broken_syllable) return false;
    std::uint16_t glyph = 0;
    if (!nominal || !nominal(0x25CCu, glyph)) return false;

    GlyphInfo dottedcircle{};
    dottedcircle.codepoint = 0x25CCu;
    dottedcircle.gid = glyph;
    dottedcircle.shaper_cat = static_cast<std::uint8_t>(dottedcircle_category);
    if (dottedcircle_position != -1) dottedcircle.shaper_pos = static_cast<std::uint8_t>(dottedcircle_position);

    b.clear_output();
    b.idx = 0;
    unsigned last_syllable = 0;
    while (b.idx < b.len() && b.successful) {
        const unsigned syllable = b.cur().syllable;
        if (last_syllable != syllable && (syllable & 0x0F) == broken_syllable_type) {
            last_syllable = syllable;
            GlyphInfo ginfo = dottedcircle;
            ginfo.cluster = b.cur().cluster;
            ginfo.mask = b.cur().mask;
            ginfo.syllable = b.cur().syllable;
            if (repha_category != -1)
                while (b.idx < b.len() && b.successful && last_syllable == b.cur().syllable &&
                       b.cur().shaper_cat == static_cast<unsigned>(repha_category))
                    b.next_glyph();
            b.out.push_back(ginfo);
        } else {
            b.next_glyph();
        }
    }
    b.sync();
    return true;
}

/// hb_syllabic_clear_var: a pause that marks the end of syllable use.
inline void clear_var(const Plan&, Buffer&, void*) {}
inline void clear_syllables(const Plan& p, Buffer& b, void* u) { clear_var(p, b, u); }

/// _hb_next_syllable: end of the syllable starting at `start`.
inline std::size_t next_syllable(const Buffer& b, std::size_t start) noexcept {
    const std::size_t count = b.info.size();
    const std::uint8_t s = b.info[start].syllable;
    while (++start < count && s == b.info[start].syllable) {}
    return start;
}

/// foreach_syllable(buffer, start, end) as a callback loop: f(start, end).
template <class F>
inline void foreach_syllable(Buffer& b, F&& f) {
    for (std::size_t start = 0; start < b.len();) {
        const std::size_t end = next_syllable(b, start);
        f(start, end);
        start = end;
    }
}

/// True if any syllable in the buffer has type `broken_syllable_type`
/// (stands in for HB_BUFFER_SCRATCH_FLAG_HAS_BROKEN_SYLLABLE).
inline bool has_broken_syllable(const Buffer& b, unsigned broken_syllable_type) noexcept {
    for (const auto& g : b.info)
        if ((g.syllable & 0x0Fu) == broken_syllable_type) return true;
    return false;
}

}  // namespace mayag::typo::ot::syllabic
