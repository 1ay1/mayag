#pragma once
// mayag::typo::ot — script shapers (HarfBuzz's hb-ot-shaper-*.cc, ported)
//
// A script shaper is the knowledge a font does NOT carry: which glyphs of a
// word take initial/medial/final forms (Arabic, Syriac, Mongolian, N'Ko...),
// how an Indic syllable is reordered before its conjuncts form, where Thai
// sara am splits. Fonts are built expecting exactly HarfBuzz's behaviour,
// so each shaper here is a rule-for-rule port, verified against hb-shape.
//
// Including this header registers the shapers with ot_shape.hpp's selector.
//
// Ports of HarfBuzz's hb-ot-shaper-{arabic,hebrew,thai,hangul}.cc and
// hb_ot_shaper_categorize; HarfBuzz is "Old MIT" licensed, see
// THIRD_PARTY_NOTICES.md.

#include "ot_shape.hpp"
#include "ot_shaper_indic.hpp"
#include "ot_shaper_khmer.hpp"
#include "ot_shaper_myanmar.hpp"
#include "ot_shaper_use.hpp"
#include "unicode_data.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace mayag::typo::ot {

// ══ Arabic family (hb-ot-shaper-arabic.cc) ════════════════════════════════

namespace arabic {

enum : std::uint8_t {   // joining classes, the state machine's columns
    JT_U = 0, JT_L = 1, JT_R = 2, JT_D = 3, JG_ALAPH = 4, JG_DALATH_RISH = 5, JT_T = 7,
};
enum : std::uint8_t {   // actions = the feature to apply
    ISOL, FINA, FIN2, FIN3, MEDI, MED2, INIT, NONE, NUM_FEATURES = NONE, STCH_FIXED, STCH_REPEATING,
};

inline constexpr std::uint32_t features[NUM_FEATURES] = {
    make_tag('i', 's', 'o', 'l'), make_tag('f', 'i', 'n', 'a'), make_tag('f', 'i', 'n', '2'),
    make_tag('f', 'i', 'n', '3'), make_tag('m', 'e', 'd', 'i'), make_tag('m', 'e', 'd', '2'),
    make_tag('i', 'n', 'i', 't'),
};

/// Joining class of a codepoint, as HarfBuzz's table: Alaph and Dalath-Rish
/// get their own columns (Syriac final forms depend on them), C joins like
/// D, and unlisted Mn/Me/Cf are transparent.
[[nodiscard]] inline std::uint8_t joining_type(char32_t u) noexcept {
    switch (ucd::joining_group(u)) {
        case ucd::JoiningGroup::ALAPH: return JG_ALAPH;
        case ucd::JoiningGroup::DALATH_RISH: return JG_DALATH_RISH;
        default: break;
    }
    switch (ucd::joining_type(u)) {
        case ucd::JoiningType::R: return JT_R;
        case ucd::JoiningType::L: return JT_L;
        case ucd::JoiningType::D: case ucd::JoiningType::C: return JT_D;
        case ucd::JoiningType::T: return JT_T;
        default: return JT_U;
    }
}

struct Entry { std::uint8_t prev_action, curr_action; std::uint8_t next_state; };
inline constexpr Entry state_table[7][6] = {
    // U               L                R                D                Alaph            DalathRish
    {{NONE,NONE,0}, {NONE,ISOL,2}, {NONE,ISOL,1}, {NONE,ISOL,2}, {NONE,ISOL,1}, {NONE,ISOL,6}},   // 0: start
    {{NONE,NONE,0}, {NONE,ISOL,2}, {NONE,ISOL,1}, {NONE,ISOL,2}, {NONE,FIN2,5}, {NONE,ISOL,6}},   // 1: prev R/U-ish
    {{NONE,NONE,0}, {NONE,ISOL,2}, {INIT,FINA,1}, {INIT,FINA,3}, {INIT,FINA,4}, {INIT,FINA,6}},   // 2: prev D/L (init)
    {{NONE,NONE,0}, {NONE,ISOL,2}, {MEDI,FINA,1}, {MEDI,FINA,3}, {MEDI,FINA,4}, {MEDI,FINA,6}},   // 3: prev D (medi)
    {{NONE,NONE,0}, {NONE,ISOL,2}, {MED2,ISOL,1}, {MED2,ISOL,2}, {MED2,FIN2,5}, {MED2,ISOL,6}},   // 4: prev Alaph-fina
    {{NONE,NONE,0}, {NONE,ISOL,2}, {ISOL,ISOL,1}, {ISOL,ISOL,2}, {ISOL,FIN2,5}, {ISOL,ISOL,6}},   // 5: prev Alaph-fin2
    {{NONE,NONE,0}, {NONE,ISOL,2}, {NONE,ISOL,1}, {NONE,ISOL,2}, {NONE,FIN3,5}, {NONE,ISOL,6}},   // 6: prev DalathRish
};

struct PlanData {
    std::uint32_t mask[NUM_FEATURES + 1] = {};
    bool has_stch = false;
};

inline bool is_syriac_feature(std::uint32_t tag) noexcept {
    const char last = static_cast<char>(tag & 0xFF);
    return last == '2' || last == '3';
}

/// stch: remember which components a multiple substitution produced, so
/// postprocess can stretch (Syriac abbreviation mark).
inline void record_stch(const Plan&, Buffer& b, void* user) {
    const auto* pc = static_cast<const PauseContext*>(user);
    const auto* d = static_cast<const PlanData*>(pc->plan->data.get());
    if (!d || !d->has_stch) return;
    for (auto& g : b.info)
        if (is_multiplied(g)) g.shaper_cat = (lig::comp(g) % 2) ? STCH_REPEATING : STCH_FIXED;
}

inline void collect_features(Planner& p) {
    auto& m = p.map;
    m.enable_feature(make_tag('s', 't', 'c', 'h'));
    m.add_gsub_pause(record_stch);
    m.enable_feature(make_tag('c', 'c', 'm', 'p'), F_MANUAL_ZWJ);
    m.enable_feature(make_tag('l', 'o', 'c', 'l'), F_MANUAL_ZWJ);
    m.add_gsub_pause(nullptr);
    for (std::uint32_t f : features) {
        const bool has_fallback = p.script == ucd::Script::Arabic && !is_syriac_feature(f);
        m.add_feature(f, F_MANUAL_ZWJ | (has_fallback ? F_HAS_FALLBACK : F_NONE));
        m.add_gsub_pause(nullptr);
    }
    m.add_gsub_pause(nullptr);
    m.enable_feature(make_tag('r', 'l', 'i', 'g'), F_MANUAL_ZWJ | F_HAS_FALLBACK);
    if (p.script == ucd::Script::Arabic) m.add_gsub_pause(nullptr);   // (fallback shaping: TODO)
    m.enable_feature(make_tag('c', 'a', 'l', 't'), F_MANUAL_ZWJ);
    m.enable_feature(make_tag('m', 's', 'e', 't'), F_MANUAL_ZWJ);
}

inline std::shared_ptr<void> data_create(const ShapePlan& sp) {
    auto d = std::make_shared<PlanData>();
    d->has_stch = sp.map.one_mask_of(make_tag('s', 't', 'c', 'h')) != 0;
    for (std::size_t i = 0; i < NUM_FEATURES; ++i) d->mask[i] = sp.map.one_mask_of(features[i]);
    return d;
}

/// The joining state machine over a run. Context outside the run (the
/// characters before and after it in the paragraph) is not available here,
/// as with hb_buffer_t without pre/post context.
inline void joining(Buffer& b) {
    std::size_t prev = static_cast<std::size_t>(-1);
    unsigned state = 0;
    for (std::size_t i = 0; i < b.len(); ++i) {
        const std::uint8_t type = joining_type(b.info[i].codepoint);
        if (type == JT_T) { b.info[i].shaper_cat = NONE; continue; }
        const Entry& e = state_table[state][type];
        if (e.prev_action != NONE && prev != static_cast<std::size_t>(-1)) b.info[prev].shaper_cat = e.prev_action;
        b.info[i].shaper_cat = e.curr_action;
        prev = i;
        state = e.next_state;
    }
}

/// setup_masks_arabic_plan (also used by the USE shaper for joining scripts).
inline void setup_masks_plan(const PlanData* d, Buffer& b, ucd::Script script) {
    joining(b);
    if (script == ucd::Script::Mongolian)   // FVS take their base's form
        for (std::size_t i = 1; i < b.len(); ++i) {
            const char32_t u = b.info[i].codepoint;
            if ((u >= 0x180B && u <= 0x180D) || u == 0x180F) b.info[i].shaper_cat = b.info[i - 1].shaper_cat;
        }
    for (auto& g : b.info) g.mask |= d->mask[g.shaper_cat < NONE ? g.shaper_cat : std::uint8_t{NONE}];
}

inline void setup_masks(const ShapePlan& sp, Buffer& b, const FontFuncs&) {
    setup_masks_plan(static_cast<const PlanData*>(sp.data.get()), b, sp.script);
}

/// Modifier combining marks (hamza above/below, noon ghunna...) move to the
/// front of their ccc 220/230 run, so they attach to the base before other
/// marks (Unicode TR53, Arabic Mark Transient Reordering).
inline void reorder_marks(const ShapePlan&, Buffer& b, std::size_t start, std::size_t end) {
    static constexpr char32_t mcm[] = {0x0654, 0x0655, 0x0658, 0x06DC, 0x06E3, 0x06E7, 0x06E8,
                                       0x08CA, 0x08CB, 0x08CD, 0x08CE, 0x08CF, 0x08D3, 0x08F3};
    const auto is_mcm = [](const GlyphInfo& g) {
        for (char32_t u : mcm) if (g.codepoint == u) return true;
        return false;
    };
    auto& info = b.info;
    std::size_t i = start;
    for (unsigned cc = 220; cc <= 230; cc += 10) {
        while (i < end && info[i].mcc < cc) ++i;
        if (i == end) break;
        if (info[i].mcc > cc) continue;
        std::size_t j = i;
        while (j < end && info[j].mcc == cc && is_mcm(info[j])) ++j;
        if (i == j) continue;
        b.merge_clusters(start, j);
        std::rotate(info.begin() + static_cast<std::ptrdiff_t>(start), info.begin() + static_cast<std::ptrdiff_t>(i),
                    info.begin() + static_cast<std::ptrdiff_t>(j));
        const std::size_t new_start = start + (j - i);
        const std::uint8_t new_cc = cc == 220 ? 25 : 26;   // CCC22 / CCC26, modified
        while (start < new_start) info[start++].mcc = new_cc;
        i = j;
    }
}

[[nodiscard]] inline bool is_word_category(std::uint8_t gc) noexcept {
    using G = ucd::GeneralCategory;
    switch (static_cast<G>(gc)) {
        case G::Cn: case G::Co: case G::Lm: case G::Lo: case G::Mc: case G::Me: case G::Mn:
        case G::Nd: case G::Nl: case G::No: case G::Sc: case G::Sk: case G::Sm: case G::So:
            return true;
        default: return false;
    }
}

/// apply_stch: stretch Syriac abbreviation marks across their word.
inline void postprocess_glyphs(const ShapePlan&, Buffer& b, const FontFuncs& font) {
    bool any = false;
    for (const auto& g : b.info) any |= (g.shaper_cat == STCH_FIXED || g.shaper_cat == STCH_REPEATING);
    if (!any) return;
    const bool rtl = b.direction == Direction::rtl;
    if (!rtl) b.reverse();
    const auto is_stch = [](const GlyphInfo& g) { return g.shaper_cat == STCH_FIXED || g.shaper_cat == STCH_REPEATING; };
    std::size_t extra = 0;
    std::vector<GlyphInfo> ni;
    std::vector<GlyphPos> np;
    for (int step = 0; step < 2; ++step) {
        const std::size_t count = b.len();
        const std::size_t new_len = count + extra;
        if (step == 1) { ni.assign(new_len, GlyphInfo{}); np.assign(new_len, GlyphPos{}); }
        std::size_t j = new_len;
        for (std::size_t i = count; i;) {
            if (!is_stch(b.info[i - 1])) {
                if (step == 1) { --j; ni[j] = b.info[i - 1]; np[j] = b.pos[i - 1]; }
                --i;
                continue;
            }
            float w_total = 0, w_fixed = 0, w_repeating = 0;
            int n_repeating = 0;
            const std::size_t end = i;
            while (i && is_stch(b.info[i - 1])) {
                --i;
                const float w = font.advance(b.info[i].gid);
                if (b.info[i].shaper_cat == STCH_FIXED) w_fixed += w;
                else { w_repeating += w; ++n_repeating; }
            }
            const std::size_t start = i;
            std::size_t context = i;
            while (context && !is_stch(b.info[context - 1]) &&
                   (is_default_ignorable(b.info[context - 1]) || is_word_category(b.info[context - 1].gc))) {
                --context;
                w_total += b.pos[context].x_advance;
            }
            int n_copies = 0;
            float w_remaining = w_total - w_fixed;
            if (w_remaining > w_repeating && w_repeating > 0) n_copies = static_cast<int>(w_remaining / w_repeating) - 1;
            float overlap = 0;
            const float shortfall = w_remaining - w_repeating * static_cast<float>(n_copies + 1);
            if (shortfall > 0 && n_repeating > 0) {
                ++n_copies;
                const float excess = static_cast<float>(n_copies + 1) * w_repeating - w_remaining;
                if (excess > 0) { overlap = excess / static_cast<float>(n_copies * n_repeating); w_remaining = 0; }
            }
            if (step == 0) { extra += static_cast<std::size_t>(n_copies * n_repeating); continue; }
            float x_offset = w_remaining / 2;
            for (std::size_t k = end; k > start; --k) {
                const float width = font.advance(b.info[k - 1].gid);
                int repeat = 1;
                if (b.info[k - 1].shaper_cat == STCH_REPEATING) repeat += n_copies;
                b.pos[k - 1].x_advance = 0;
                for (int n = 0; n < repeat; ++n) {
                    if (rtl) { x_offset -= width; if (n > 0) x_offset += overlap; }
                    b.pos[k - 1].x_offset = x_offset;
                    --j;
                    ni[j] = b.info[k - 1];
                    np[j] = b.pos[k - 1];
                    if (!rtl) { x_offset += width; if (n > 0) x_offset -= overlap; }
                }
            }
        }
        if (step == 1) { b.info.swap(ni); b.pos.swap(np); }
    }
    if (!rtl) b.reverse();
}

inline const ScriptShaper& shaper() {
    static const ScriptShaper s = [] {
        ScriptShaper x;
        x.name = "arabic";
        x.collect_features = collect_features;
        x.data_create = data_create;
        x.postprocess_glyphs = postprocess_glyphs;
        x.setup_masks = setup_masks;
        x.reorder_marks = reorder_marks;
        x.zero_marks = ZeroMarks::late;
        x.fallback_position = true;
        return x;
    }();
    return s;
}

}  // namespace arabic

// ══ Hebrew (hb-ot-shaper-hebrew.cc) ════════════════════════════════════════════════════════

namespace hebrew {

/// Presentation forms (U+FB1D..FB4E) for fonts without GPOS mark
/// positioning: dagesh, shin/sin dots and the like as precomposed glyphs.
inline bool compose(const detail::NormalizeContext& c, char32_t a, char32_t b, char32_t& ab) {
    static constexpr char32_t dagesh[0x05EA - 0x05D0 + 1] = {
        0xFB30, 0xFB31, 0xFB32, 0xFB33, 0xFB34, 0xFB35, 0xFB36, 0, 0xFB38, 0xFB39, 0xFB3A, 0xFB3B,
        0xFB3C, 0, 0xFB3E, 0, 0xFB40, 0xFB41, 0, 0xFB43, 0xFB44, 0, 0xFB46, 0xFB47, 0xFB48, 0xFB49, 0xFB4A};
    ab = ucd::canonical_composition(a, b);
    if (ab) return true;
    if (c.plan.has_gpos_mark) return false;
    switch (b) {
        case 0x05B4: if (a == 0x05D9) { ab = 0xFB1D; return true; } break;
        case 0x05B7: if (a == 0x05F2) { ab = 0xFB1F; return true; }
                     if (a == 0x05D0) { ab = 0xFB2E; return true; } break;
        case 0x05B8: if (a == 0x05D0) { ab = 0xFB2F; return true; } break;
        case 0x05B9: if (a == 0x05D5) { ab = 0xFB4B; return true; } break;
        case 0x05BC:
            if (a >= 0x05D0 && a <= 0x05EA) { ab = dagesh[a - 0x05D0]; return ab != 0; }
            if (a == 0xFB2A) { ab = 0xFB2C; return true; }
            if (a == 0xFB2B) { ab = 0xFB2D; return true; }
            break;
        case 0x05BF:
            if (a == 0x05D1) { ab = 0xFB4C; return true; }
            if (a == 0x05DB) { ab = 0xFB4D; return true; }
            if (a == 0x05E4) { ab = 0xFB4E; return true; }
            break;
        case 0x05C1: if (a == 0x05E9) { ab = 0xFB2A; return true; }
                     if (a == 0xFB49) { ab = 0xFB2C; return true; } break;
        case 0x05C2: if (a == 0x05E9) { ab = 0xFB2B; return true; }
                     if (a == 0xFB49) { ab = 0xFB2D; return true; } break;
        default: break;
    }
    return false;
}

/// Patah/qamats after a meteg-like mark goes before it (hb#3209).
inline void reorder_marks(const ShapePlan&, Buffer& b, std::size_t start, std::size_t end) {
    auto& info = b.info;
    for (std::size_t i = start + 2; i < end; ++i) {
        const unsigned c0 = info[i - 2].mcc, c1 = info[i - 1].mcc, c2 = info[i].mcc;
        // modified: CCC17=20, CCC18=21, CCC10=22, CCC14=23, CCC22=25, below=220
        if ((c0 == 20 || c0 == 21) && (c1 == 22 || c1 == 23) && (c2 == 25 || c2 == 220)) {
            b.merge_clusters(i - 1, i + 1);
            std::swap(info[i - 1], info[i]);
            break;
        }
    }
}

inline const ScriptShaper& shaper() {
    static const ScriptShaper s = [] {
        ScriptShaper x;
        x.name = "hebrew";
        x.compose = compose;
        x.reorder_marks = reorder_marks;
        x.gpos_tag = make_tag('h', 'e', 'b', 'r');
        x.zero_marks = ZeroMarks::late;
        x.fallback_position = true;
        return x;
    }();
    return s;
}

}  // namespace hebrew

// ══ Thai / Lao (hb-ot-shaper-thai.cc) ═════════════════════════════════════════════════════

namespace thai {

// PUA fallback tables (for Thai fonts without OpenType Thai shaping).
enum Action : std::uint8_t { NOP, SD, SL, SDL, RD };
struct PuaMap { char32_t u, win, mac; };
inline char32_t pua_shape(char32_t u, Action a, const FontFuncs& font) {
    static constexpr PuaMap sd[] = {{0x0E48, 0xF70A, 0xF88B}, {0x0E49, 0xF70B, 0xF88E}, {0x0E4A, 0xF70C, 0xF891},
                                    {0x0E4B, 0xF70D, 0xF894}, {0x0E4C, 0xF70E, 0xF897}, {0x0E38, 0xF718, 0xF89B},
                                    {0x0E39, 0xF719, 0xF89C}, {0x0E3A, 0xF71A, 0xF89D}};
    static constexpr PuaMap sdl[] = {{0x0E48, 0xF705, 0xF88C}, {0x0E49, 0xF706, 0xF88F}, {0x0E4A, 0xF707, 0xF892},
                                     {0x0E4B, 0xF708, 0xF895}, {0x0E4C, 0xF709, 0xF898}};
    static constexpr PuaMap sl[] = {{0x0E48, 0xF713, 0xF88A}, {0x0E49, 0xF714, 0xF88D}, {0x0E4A, 0xF715, 0xF890},
                                    {0x0E4B, 0xF716, 0xF893}, {0x0E4C, 0xF717, 0xF896}, {0x0E31, 0xF710, 0xF884},
                                    {0x0E34, 0xF701, 0xF885}, {0x0E35, 0xF702, 0xF886}, {0x0E36, 0xF703, 0xF887},
                                    {0x0E37, 0xF704, 0xF888}, {0x0E47, 0xF712, 0xF889}, {0x0E4D, 0xF711, 0xF899}};
    static constexpr PuaMap rd[] = {{0x0E0D, 0xF70F, 0xF89A}, {0x0E10, 0xF700, 0xF89E}};
    std::span<const PuaMap> m;
    switch (a) {
        case NOP: return u;
        case SD: m = sd; break;
        case SDL: m = sdl; break;
        case SL: m = sl; break;
        case RD: m = rd; break;
    }
    for (const PuaMap& e : m)
        if (e.u == u) {
            std::uint16_t g = 0;
            if (font.nominal(e.win, g)) return e.win;
            if (font.nominal(e.mac, g)) return e.mac;
            break;
        }
    return u;
}

enum Cons : std::uint8_t { NC, AC, RC, DC, NOT_CONSONANT };
enum Mark : std::uint8_t { AV, BV, T, NOT_MARK };
inline Cons consonant_type(char32_t u) {
    if (u == 0x0E1B || u == 0x0E1D || u == 0x0E1F) return AC;
    if (u == 0x0E0D || u == 0x0E10) return RC;
    if (u == 0x0E0E || u == 0x0E0F) return DC;
    if (u >= 0x0E01 && u <= 0x0E2E) return NC;
    return NOT_CONSONANT;
}
inline Mark mark_type(char32_t u) {
    if (u == 0x0E31 || (u >= 0x0E34 && u <= 0x0E37) || u == 0x0E47 || (u >= 0x0E4D && u <= 0x0E4E)) return AV;
    if (u >= 0x0E38 && u <= 0x0E3A) return BV;
    if (u >= 0x0E48 && u <= 0x0E4C) return T;
    return NOT_MARK;
}

inline void pua_shaping(Buffer& b, const FontFuncs& font) {
    static constexpr std::uint8_t above_start[5] = {0, 1, 0, 0, 3};
    static constexpr std::uint8_t below_start[5] = {0, 0, 1, 2, 2};
    struct Edge { Action a; std::uint8_t next; };
    static constexpr Edge above[4][3] = {{{NOP, 3}, {NOP, 0}, {SD, 3}}, {{SL, 2}, {NOP, 1}, {SDL, 2}},
                                         {{NOP, 3}, {NOP, 2}, {SL, 3}}, {{NOP, 3}, {NOP, 3}, {NOP, 3}}};
    static constexpr Edge below[3][3] = {{{NOP, 0}, {NOP, 2}, {NOP, 0}}, {{NOP, 1}, {RD, 2}, {NOP, 1}},
                                         {{NOP, 2}, {SD, 2}, {NOP, 2}}};
    std::uint8_t as = above_start[NOT_CONSONANT], bs = below_start[NOT_CONSONANT];
    std::size_t base = 0;
    for (std::size_t i = 0; i < b.len(); ++i) {
        const Mark mt = mark_type(b.info[i].codepoint);
        if (mt == NOT_MARK) {
            const Cons ct = consonant_type(b.info[i].codepoint);
            as = above_start[ct];
            bs = below_start[ct];
            base = i;
            continue;
        }
        const Edge& ae = above[as][mt];
        const Edge& be = below[bs][mt];
        as = ae.next;
        bs = be.next;
        const Action a = ae.a != NOP ? ae.a : be.a;
        if (a == RD) b.info[base].codepoint = pua_shape(b.info[base].codepoint, a, font);
        else b.info[i].codepoint = pua_shape(b.info[i].codepoint, a, font);
    }
}

/// Sara Am (U+0E33/0EB3) decomposes to nikhahit + sara aa, and the
/// nikhahit moves before any above-base marks, so it sits on the consonant.
inline void preprocess_text(const ShapePlan& plan, Buffer& b, const FontFuncs& font) {
    const auto is_sara_am = [](char32_t x) { return (x & ~0x0080u) == 0x0E33u; };
    const auto is_above = [](char32_t x) {
        const char32_t v = x & ~0x0080u;
        return (v >= 0x0E34 && v <= 0x0E37) || (v >= 0x0E47 && v <= 0x0E4E) || v == 0x0E31 || v == 0x0E3B;
    };
    b.clear_output();
    const std::size_t count = b.len();
    for (b.idx = 0; b.idx < count;) {
        const char32_t u = b.cur().codepoint;
        if (!is_sara_am(u)) { b.next_glyph(); continue; }
        // output_glyph(NIKHAHIT) + replace_glyph(SARA AA), on codepoints
        b.out.push_back(b.cur());
        b.out.back().codepoint = u - 0x0E33u + 0x0E4Du;
        b.out.back().uflags |= uflag::continuation;
        b.out.push_back(b.cur());
        b.out.back().codepoint = u - 1;
        ++b.idx;
        const std::size_t end = b.out.size();
        b.out[end - 2].gc = static_cast<std::uint8_t>(ucd::GeneralCategory::Mn);
        std::size_t start = end - 2;
        while (start > 0 && is_above(b.out[start - 1].codepoint)) --start;
        if (start + 2 < end) {
            b.merge_out_clusters(start, end);
            const GlyphInfo t = b.out[end - 2];
            std::copy_backward(b.out.begin() + static_cast<std::ptrdiff_t>(start),
                               b.out.begin() + static_cast<std::ptrdiff_t>(end - 2),
                               b.out.begin() + static_cast<std::ptrdiff_t>(end - 1));
            b.out[start] = t;
        } else if (start) {
            b.merge_out_clusters(start - 1, end);
        }
    }
    b.sync();
    if (plan.script == ucd::Script::Thai && !plan.map.found_script[table::gsub]) pua_shaping(b, font);
}

inline const ScriptShaper& shaper() {
    static const ScriptShaper s = [] {
        ScriptShaper x;
        x.name = "thai";
        x.preprocess_text = preprocess_text;
        x.zero_marks = ZeroMarks::late;
        x.fallback_position = false;
        return x;
    }();
    return s;
}

}  // namespace thai

// ══ Hangul (hb-ot-shaper-hangul.cc) ════════════════════════════════════════════════════════

namespace hangul {

enum : std::uint8_t { NONE_ = 0, LJMO = 1, VJMO = 2, TJMO = 3 };
inline constexpr std::uint32_t features[4] = {0, make_tag('l', 'j', 'm', 'o'), make_tag('v', 'j', 'm', 'o'),
                                              make_tag('t', 'j', 'm', 'o')};
struct PlanData { std::uint32_t mask[4] = {}; };

inline constexpr char32_t LBase = 0x1100, VBase = 0x1161, TBase = 0x11A7, SBase = 0xAC00;
inline constexpr unsigned LCount = 19, VCount = 21, TCount = 28, NCount = VCount * TCount, SCount = LCount * NCount;
inline bool is_combining_l(char32_t u) { return u >= LBase && u < LBase + LCount; }
inline bool is_combining_v(char32_t u) { return u >= VBase && u < VBase + VCount; }
inline bool is_combining_t(char32_t u) { return u > TBase && u < TBase + TCount; }
inline bool is_combined_s(char32_t u) { return u >= SBase && u < SBase + SCount; }
inline bool is_l(char32_t u) { return (u >= 0x1100 && u <= 0x115F) || (u >= 0xA960 && u <= 0xA97C); }
inline bool is_v(char32_t u) { return (u >= 0x1160 && u <= 0x11A7) || (u >= 0xD7B0 && u <= 0xD7C6); }
inline bool is_t(char32_t u) { return (u >= 0x11A8 && u <= 0x11FF) || (u >= 0xD7CB && u <= 0xD7FB); }
inline bool is_tone(char32_t u) { return u >= 0x302E && u <= 0x302F; }

inline void collect_features(Planner& p) {
    for (int i = 1; i < 4; ++i) p.map.add_feature(features[i]);
}
inline void override_features(Planner& p) { p.map.disable_feature(make_tag('c', 'a', 'l', 't')); }
inline std::shared_ptr<void> data_create(const ShapePlan& sp) {
    auto d = std::make_shared<PlanData>();
    for (int i = 0; i < 4; ++i) d->mask[i] = sp.map.one_mask_of(features[i]);
    return d;
}

inline bool has(const FontFuncs& f, char32_t u) { std::uint16_t g = 0; return f.nominal(u, g); }
inline bool is_zero_width(const FontFuncs& f, char32_t u) {
    std::uint16_t g = 0;
    return f.nominal(u, g) && f.advance(g) == 0.0f;
}

/// Compose jamo to precomposed syllables the font has, or decompose
/// syllables to jamo it lacks, marking jamo for ljmo/vjmo/tjmo; tone marks
/// move before their syllable.
inline void preprocess_text(const ShapePlan&, Buffer& b, const FontFuncs& font) {
    b.clear_output();
    std::size_t start = 0, end = 0;
    const std::size_t count = b.len();
    for (b.idx = 0; b.idx < count;) {
        const char32_t u = b.cur().codepoint;
        if (is_tone(u)) {
            if (start < end && end == b.out.size()) {
                b.next_glyph();
                if (!is_zero_width(font, u)) {
                    b.merge_out_clusters(start, end + 1);
                    const GlyphInfo tone = b.out[end];
                    std::copy_backward(b.out.begin() + static_cast<std::ptrdiff_t>(start),
                                       b.out.begin() + static_cast<std::ptrdiff_t>(end),
                                       b.out.begin() + static_cast<std::ptrdiff_t>(end + 1));
                    b.out[start] = tone;
                }
            } else if (has(font, 0x25CC)) {
                const char32_t chars[2] = {is_zero_width(font, u) ? char32_t{0x25CC} : u,
                                           is_zero_width(font, u) ? u : char32_t{0x25CC}};
                b.replace_codepoints(1, chars);
            } else {
                b.next_glyph();
            }
            start = end = b.out.size();
            continue;
        }
        start = b.out.size();
        if (is_l(u) && b.idx + 1 < count) {
            const char32_t l = u, v = b.cur(1).codepoint;
            if (is_v(v)) {
                char32_t t = 0;
                unsigned tindex = 0;
                if (b.idx + 2 < count) {
                    t = b.cur(2).codepoint;
                    if (is_t(t)) tindex = t - TBase; else t = 0;
                }
                if (is_combining_l(l) && is_combining_v(v) && (t == 0 || is_combining_t(t))) {
                    const char32_t s = SBase + (l - LBase) * NCount + (v - VBase) * TCount + tindex;
                    if (has(font, s)) {
                        const char32_t one[1] = {s};
                        b.replace_codepoints(t ? 3 : 2, one);
                        end = start + 1;
                        continue;
                    }
                }
                b.cur().aux = LJMO; b.next_glyph();
                b.cur().aux = VJMO; b.next_glyph();
                if (t) { b.cur().aux = TJMO; b.next_glyph(); end = start + 3; }
                else end = start + 2;
                b.merge_out_clusters(start, end);
                continue;
            }
        } else if (is_combined_s(u)) {
            const char32_t s = u;
            const bool has_glyph = has(font, s);
            const unsigned lindex = (s - SBase) / NCount, nindex = (s - SBase) % NCount;
            const unsigned vindex = nindex / TCount, tindex = nindex % TCount;
            if (!tindex && b.idx + 1 < count && is_combining_t(b.cur(1).codepoint)) {
                const char32_t new_s = s + (b.cur(1).codepoint - TBase);
                if (has(font, new_s)) {
                    const char32_t one[1] = {new_s};
                    b.replace_codepoints(2, one);
                    end = start + 1;
                    continue;
                }
            }
            if (!has_glyph || (!tindex && b.idx + 1 < count && is_t(b.cur(1).codepoint))) {
                const char32_t dec[3] = {LBase + lindex, VBase + vindex, TBase + tindex};
                if (has(font, dec[0]) && has(font, dec[1]) && (!tindex || has(font, dec[2]))) {
                    std::size_t s_len = tindex ? 3 : 2;
                    b.replace_codepoints(1, std::span<const char32_t>{dec, s_len});
                    if (has_glyph && !tindex) { b.next_glyph(); ++s_len; }
                    end = start + s_len;
                    std::size_t i = start;
                    b.out[i++].aux = LJMO;
                    b.out[i++].aux = VJMO;
                    if (i < end) b.out[i++].aux = TJMO;
                    b.merge_out_clusters(start, end);
                    continue;
                }
            }
            if (has_glyph) end = start + 1;
        }
        b.next_glyph();
    }
    b.sync();
}

inline void setup_masks(const ShapePlan& sp, Buffer& b, const FontFuncs&) {
    const auto* d = static_cast<const PlanData*>(sp.data.get());
    for (auto& g : b.info) { g.mask |= d->mask[g.aux < 4 ? g.aux : 0]; g.aux = 0; }
}

inline const ScriptShaper& shaper() {
    static const ScriptShaper s = [] {
        ScriptShaper x;
        x.name = "hangul";
        x.collect_features = collect_features;
        x.override_features = override_features;
        x.data_create = data_create;
        x.preprocess_text = preprocess_text;
        x.setup_masks = setup_masks;
        x.normalization = Normalization::none;
        x.zero_marks = ZeroMarks::none;
        x.fallback_position = false;
        return x;
    }();
    return s;
}

}  // namespace hangul

// @@CONTINUE@@

// ══ selection (hb_ot_shaper_categorize) ═══════════════════════════════════

inline const ScriptShaper* select_shaper(ucd::Script script, Direction dir, std::uint32_t gsub_script) {
    const std::uint32_t iso = ucd::iso15924_tag(script);
    switch (iso) {
        case make_tag('A', 'r', 'a', 'b'): case make_tag('S', 'y', 'r', 'c'):
            // Arabic even without an Arabic GSUB script (fallback forms);
            // Syriac only when the font has a Syriac script. Any horizontal
            // direction.
            if (gsub_script != tag_DFLT || iso == make_tag('A', 'r', 'a', 'b')) return &arabic::shaper();
            return nullptr;
        case make_tag('T', 'h', 'a', 'i'): case make_tag('L', 'a', 'o', 'o'):
            return &thai::shaper();
        case make_tag('H', 'a', 'n', 'g'):
            return &hangul::shaper();
        case make_tag('H', 'e', 'b', 'r'):
            return &hebrew::shaper();
        case make_tag('K', 'h', 'm', 'r'):
            return &khmer::shaper();
        case make_tag('B', 'e', 'n', 'g'): case make_tag('D', 'e', 'v', 'a'): case make_tag('G', 'u', 'j', 'r'):
        case make_tag('G', 'u', 'r', 'u'): case make_tag('K', 'n', 'd', 'a'): case make_tag('M', 'l', 'y', 'm'):
        case make_tag('O', 'r', 'y', 'a'): case make_tag('T', 'a', 'm', 'l'): case make_tag('T', 'e', 'l', 'u'):
            // DFLT/latn -> default; v3 Indic tags -> USE; else Indic.
            if (gsub_script == tag_DFLT || gsub_script == tag_latn) return nullptr;
            if ((gsub_script & 0xFFu) == '3') return &use::shaper();
            return &indic::shaper();
        case make_tag('M', 'y', 'm', 'r'):
            // DFLT/latn fonts and the pre-spec 'mymr' tag use the default shaper.
            if (gsub_script == make_tag('D', 'F', 'L', 'T') || gsub_script == make_tag('l', 'a', 't', 'n') ||
                gsub_script == make_tag('m', 'y', 'm', 'r'))
                return nullptr;
            return &myanmar::shaper();
        default:
            // The long USE list of hb_ot_shaper_categorize; DFLT/latn -> default.
            if (use::is_use_script(script) && gsub_script != tag_DFLT && gsub_script != tag_latn)
                return &use::shaper();
            return nullptr;
    }
    (void)dir;
}

namespace detail {
inline const bool shapers_registered = [] { shaper_selector() = select_shaper; return true; }();
}

}  // namespace mayag::typo::ot
