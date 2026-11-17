#pragma once
// mayag::typo::seg — Unicode text segmentation: line breaks and graphemes
//
//   line_breaks()          UAX #14 (Unicode 18), every rule, LB1-LB31
//   grapheme_boundaries()  UAX #29 extended grapheme clusters, GB1-GB999
//
// Both are written as the standard states them, one rule per line, in
// order: each candidate position between two characters is tested against
// the rules top to bottom and the first that matches decides. Rules that
// look further back (LB8 "ZW SP* ÷", LB14 "OP SP* ×", LB25's number
// grammar, LB30a's regional-indicator parity, GB9c's conjunct linkers)
// keep a little state as the scan walks forward, so the whole pass is O(n).
// The point of writing it this way, instead of a compressed pair table, is
// that it can be checked against the standard line by line, and it is: the
// official LineBreakTest.txt and GraphemeBreakTest.txt pass in full
// (tests/test_segment.cpp).
//
// LB1 leaves SA (Thai, Lao, Khmer, Myanmar...: no spaces between words) to
// "criteria outside the scope of this algorithm" — a dictionary. By default
// SA resolves to AL/CM exactly as the conformance tests require, which never
// breaks inside such a run. `Tailoring::sea_syllables` opts into breaking
// between orthographic syllables instead (Thai Character Cluster style: never
// inside a cluster, never after a leading vowel or before a following one),
// so a long Thai line can wrap at all. It is not word-perfect; a dictionary
// would be.

#include "unicode_data.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mayag::typo::seg {

enum class Break : std::uint8_t { none, allowed, mandatory };
enum class Tailoring : std::uint8_t { none, sea_syllables };

namespace detail {

using LB = ucd::LineBreak;
using GC = ucd::GeneralCategory;
using EAW = ucd::EastAsianWidth;

/// LB1: resolve AI, SG, XX -> AL; SA -> CM (Mn/Mc) or AL; CJ -> NS.
[[nodiscard]] inline LB resolve(char32_t cp) noexcept {
    const LB c = ucd::line_break(cp);
    switch (c) {
        case LB::AI: case LB::SG: case LB::XX: return LB::AL;
        case LB::SA: {
            const GC g = ucd::general_category(cp);
            return (g == GC::Mn || g == GC::Mc) ? LB::CM : LB::AL;
        }
        case LB::CJ: return LB::NS;
        default: return c;
    }
}

[[nodiscard]] inline bool east_asian(char32_t cp) noexcept {
    const EAW w = ucd::east_asian_width(cp);
    return w == EAW::F || w == EAW::W || w == EAW::H;
}

/// One character as the rules see it, after LB9/LB10.
struct Ch {
    LB cls = LB::AL;
    char32_t cp = 0;      ///< the base character (LB9: X CM* acts as X)
    bool pi = false, pf = false;   ///< General_Category Pi / Pf
    bool ea = false;      ///< East Asian (F, W, H)
    bool extpict_cn = false;
};

[[nodiscard]] inline Ch make(char32_t cp, LB cls) noexcept {
    Ch c;
    c.cls = cls;
    c.cp = cp;
    const GC g = ucd::general_category(cp);
    c.pi = g == GC::Pi;
    c.pf = g == GC::Pf;
    c.ea = east_asian(cp);
    c.extpict_cn = g == GC::Cn && ucd::is_extended_pictographic(cp);
    return c;
}

/// LB10: a CM/ZWJ with no base acts as U+0041.
[[nodiscard]] inline Ch as_al() noexcept { return make(U'A', LB::AL); }

}  // namespace detail

/// Break opportunities: out[i] is the opportunity BEFORE text[i]
/// (out[0] is always none: LB2). out.size() must equal text.size().
inline void line_breaks(std::span<const char32_t> text, std::span<Break> out,
                        Tailoring tailor = Tailoring::none) {
    using namespace detail;
    const std::size_t n = text.size();
    if (out.size() != n || n == 0) return;
    out[0] = Break::none;

    std::vector<LB> raw(n);
    for (std::size_t i = 0; i < n; ++i) raw[i] = resolve(text[i]);

    // LB9/LB10: each position's effective character. A CM/ZWJ attached to a
    // base (not BK CR LF NL SP ZW) takes the base's class; with no base, AL.
    std::vector<Ch> eff(n);
    std::vector<bool> attached(n, false);   // CM/ZWJ absorbed by LB9
    for (std::size_t i = 0; i < n; ++i) {
        const LB c = raw[i];
        if ((c == LB::CM || c == LB::ZWJ) && i > 0) {
            const LB pb = eff[i - 1].cls;
            const bool base_ok = !(raw[i - 1] == LB::BK || raw[i - 1] == LB::CR || raw[i - 1] == LB::LF ||
                                   raw[i - 1] == LB::NL || raw[i - 1] == LB::SP || raw[i - 1] == LB::ZW);
            if (base_ok) {
                eff[i] = eff[i - 1];
                attached[i] = true;
                (void)pb;
                continue;
            }
        }
        eff[i] = (c == LB::CM || c == LB::ZWJ) ? as_al() : make(text[i], c);
        if (c == LB::ZWJ) eff[i].cls = LB::AL;   // LB10 for a free ZWJ
    }

    // State for lookbehind rules, updated as we pass each character.
    // "before" = the effective character before the position (skipping
    // LB9-attached marks); prev_nonsp = the last non-SP effective char.
    for (std::size_t i = 1; i < n; ++i) {
        const LB a_raw = raw[i - 1], b_raw = raw[i];
        // LB4-LB6 use the raw classes.
        if (a_raw == LB::BK) { out[i] = Break::mandatory; continue; }
        if (a_raw == LB::CR && b_raw == LB::LF) { out[i] = Break::none; continue; }
        if (a_raw == LB::CR || a_raw == LB::LF || a_raw == LB::NL) { out[i] = Break::mandatory; continue; }
        if (b_raw == LB::BK || b_raw == LB::CR || b_raw == LB::LF || b_raw == LB::NL) { out[i] = Break::none; continue; }
        // LB7
        if (b_raw == LB::SP || b_raw == LB::ZW) { out[i] = Break::none; continue; }
        // LB8: ZW SP* ÷
        {
            std::size_t k = i;
            while (k > 0 && raw[k - 1] == LB::SP) --k;
            if (k > 0 && raw[k - 1] == LB::ZW) { out[i] = Break::allowed; continue; }
        }
        // LB8a: ZWJ ×
        if (a_raw == LB::ZWJ) { out[i] = Break::none; continue; }
        // LB9: X × (CM | ZWJ) when attached
        if (attached[i]) { out[i] = Break::none; continue; }

        // From here on, rules see effective characters. `A` is the char
        // before the position (its base if it was an attached mark).
        const Ch& A = eff[i - 1];
        const Ch& B = eff[i];
        const LB a = A.cls, b = B.cls;
        // Last effective char before i, and the char before the run of SPs.
        std::size_t j = i;                        // index of first SP in "X SP* | B"
        while (j > 0 && raw[j - 1] == LB::SP) --j;
        const bool after_sp = j < i;
        const LB before_sp = j > 0 ? eff[j - 1].cls : LB::XX;   // XX = sot
        const bool sot_before_sp = j == 0;

        // LB11
        if (b == LB::WJ || a == LB::WJ) { out[i] = Break::none; continue; }
        // LB12
        if (a == LB::GL) { out[i] = Break::none; continue; }
        // LB12a
        if (b == LB::GL && a != LB::SP && a != LB::HY && a != LB::HH) { out[i] = Break::none; continue; }
        // LB13
        if (b == LB::CL || b == LB::CP || b == LB::EX || b == LB::SY) { out[i] = Break::none; continue; }
        // LB14: OP SP* ×
        if (before_sp == LB::OP && !sot_before_sp) { out[i] = Break::none; continue; }
        // LB15a: (sot | BK | CR | LF | NL | OP | QU | GL | SP | ZW) [Pi&QU] SP* ×
        if (!sot_before_sp && eff[j - 1].cls == LB::QU && eff[j - 1].pi) {
            const std::size_t q = j - 1;
            // q is the QU; find what precedes it (skipping its own attached marks)
            std::size_t p = q;
            while (p > 0 && attached[p]) --p;
            bool ok = p == 0;
            if (!ok) {
                const LB pc = raw[p - 1] == LB::SP ? LB::SP : eff[p - 1].cls;
                ok = pc == LB::BK || pc == LB::CR || pc == LB::LF || pc == LB::NL || pc == LB::OP ||
                     pc == LB::QU || pc == LB::GL || pc == LB::SP || pc == LB::ZW;
            }
            if (ok) { out[i] = Break::none; continue; }
        }
        // LB15b: × [Pf&QU] ( SP | GL | WJ | CL | QU | CP | EX | IS | SY | BK | CR | LF | NL | ZW | eot)
        if (b == LB::QU && B.pf) {
            std::size_t k = i + 1;
            while (k < n && attached[k]) ++k;
            bool ok = k >= n;
            if (!ok) {
                const LB nc = raw[k] == LB::SP ? LB::SP : eff[k].cls;
                ok = nc == LB::SP || nc == LB::GL || nc == LB::WJ || nc == LB::CL || nc == LB::QU ||
                     nc == LB::CP || nc == LB::EX || nc == LB::IS || nc == LB::SY || nc == LB::BK ||
                     nc == LB::CR || nc == LB::LF || nc == LB::NL || nc == LB::ZW;
            }
            if (ok) { out[i] = Break::none; continue; }
        }
        // LB15c: SP ÷ IS NU
        if (a == LB::SP && b == LB::IS) {
            std::size_t k = i + 1;
            while (k < n && attached[k]) ++k;
            if (k < n && eff[k].cls == LB::NU) { out[i] = Break::allowed; continue; }
        }
        // LB15d: × IS
        if (b == LB::IS) { out[i] = Break::none; continue; }
        // LB16: (CL | CP) SP* × NS
        if (b == LB::NS && (before_sp == LB::CL || before_sp == LB::CP) && !sot_before_sp) { out[i] = Break::none; continue; }
        // LB17: B2 SP* × B2
        if (b == LB::B2 && before_sp == LB::B2 && !sot_before_sp) { out[i] = Break::none; continue; }
        // LB18: SP ÷
        if (a == LB::SP) { out[i] = Break::allowed; continue; }
        (void)after_sp;
        // LB19: × [QU - Pi] ; [QU - Pf] ×
        if (b == LB::QU && !B.pi) { out[i] = Break::none; continue; }
        if (a == LB::QU && !A.pf) { out[i] = Break::none; continue; }
        // LB19a
        {
            const auto next_ea = [&](std::size_t from) -> int {   // -1 eot, 0 not EA, 1 EA
                std::size_t k = from;
                while (k < n && attached[k]) ++k;
                if (k >= n) return -1;
                return eff[k].ea ? 1 : 0;
            };
            if (b == LB::QU && !A.ea) { out[i] = Break::none; continue; }
            if (b == LB::QU) { const int nx = next_ea(i + 1); if (nx != 1) { out[i] = Break::none; continue; } }
            if (a == LB::QU && !B.ea) { out[i] = Break::none; continue; }
            if (a == LB::QU) {
                std::size_t p = i - 1;
                while (p > 0 && attached[p]) --p;
                if (p == 0 || !eff[p - 1].ea) { out[i] = Break::none; continue; }
            }
        }
        // LB20: ÷ CB ; CB ÷
        if (b == LB::CB || a == LB::CB) { out[i] = Break::allowed; continue; }
        // LB20a: ( sot | BK | CR | LF | NL | SP | ZW | CB | GL ) ( HY | HH ) × ( AL | HL )
        if ((a == LB::HY || a == LB::HH) && (b == LB::AL || b == LB::HL)) {
            std::size_t p = i - 1;
            while (p > 0 && attached[p]) --p;
            bool ok = p == 0;
            if (!ok) {
                const LB pc = raw[p - 1] == LB::SP ? LB::SP : eff[p - 1].cls;
                ok = pc == LB::BK || pc == LB::CR || pc == LB::LF || pc == LB::NL || pc == LB::SP ||
                     pc == LB::ZW || pc == LB::CB || pc == LB::GL;
            }
            if (ok) { out[i] = Break::none; continue; }
        }
        // LB21
        if (b == LB::BA || b == LB::HH || b == LB::HY || b == LB::NS || a == LB::BB) { out[i] = Break::none; continue; }
        // LB21a: HL (HY | HH) × [^HL]
        if ((a == LB::HY || a == LB::HH) && b != LB::HL) {
            std::size_t p = i - 1;
            while (p > 0 && attached[p]) --p;
            if (p > 0 && eff[p - 1].cls == LB::HL) { out[i] = Break::none; continue; }
        }
        // LB21b
        if (a == LB::SY && b == LB::HL) { out[i] = Break::none; continue; }
        // LB22
        if (b == LB::IN) { out[i] = Break::none; continue; }
        // LB23
        if ((a == LB::AL || a == LB::HL) && b == LB::NU) { out[i] = Break::none; continue; }
        if (a == LB::NU && (b == LB::AL || b == LB::HL)) { out[i] = Break::none; continue; }
        // LB23a
        if (a == LB::PR && (b == LB::ID || b == LB::EB || b == LB::EM)) { out[i] = Break::none; continue; }
        if ((a == LB::ID || a == LB::EB || a == LB::EM) && b == LB::PO) { out[i] = Break::none; continue; }
        // LB24
        if ((a == LB::PR || a == LB::PO) && (b == LB::AL || b == LB::HL)) { out[i] = Break::none; continue; }
        if ((a == LB::AL || a == LB::HL) && (b == LB::PR || b == LB::PO)) { out[i] = Break::none; continue; }
        // LB25
        {
            const auto num_prefix = [&](std::size_t end) -> bool {   // NU (SY|IS)* ending just before `end`
                std::size_t k = end;
                while (k > 0) {
                    std::size_t base = k - 1;
                    while (base > 0 && attached[base]) --base;
                    const LB c = eff[base].cls;
                    if (c == LB::SY || c == LB::IS) { k = base; continue; }
                    return c == LB::NU;
                }
                return false;
            };
            const auto next_is = [&](std::size_t from, LB want) {
                std::size_t k = from;
                while (k < n && attached[k]) ++k;
                return k < n && eff[k].cls == want;
            };
            const auto next_after = [&](std::size_t from) -> std::size_t {
                std::size_t k = from;
                while (k < n && attached[k]) ++k;
                return k;
            };
            bool hit = false;
            if ((b == LB::PO || b == LB::PR) && (a == LB::CL || a == LB::CP)) {
                std::size_t p = i - 1;
                while (p > 0 && attached[p]) --p;
                hit = num_prefix(p);
            }
            if (!hit && (b == LB::PO || b == LB::PR)) hit = num_prefix(i);
            if (!hit && (a == LB::PO || a == LB::PR)) {
                if (b == LB::NU) hit = true;
                else if (b == LB::OP) {
                    const std::size_t k = next_after(i + 1);
                    if (k < n && eff[k].cls == LB::NU) hit = true;
                    else if (k < n && eff[k].cls == LB::IS && next_is(k + 1, LB::NU)) hit = true;
                }
            }
            if (!hit && (a == LB::HY || a == LB::IS) && b == LB::NU) hit = true;
            if (!hit && b == LB::NU) hit = num_prefix(i);
            if (hit) { out[i] = Break::none; continue; }
        }
        // LB26
        if (a == LB::JL && (b == LB::JL || b == LB::JV || b == LB::H2 || b == LB::H3)) { out[i] = Break::none; continue; }
        if ((a == LB::JV || a == LB::H2) && (b == LB::JV || b == LB::JT)) { out[i] = Break::none; continue; }
        if ((a == LB::JT || a == LB::H3) && b == LB::JT) { out[i] = Break::none; continue; }
        // LB27
        if ((a == LB::JL || a == LB::JV || a == LB::JT || a == LB::H2 || a == LB::H3) && b == LB::PO) { out[i] = Break::none; continue; }
        if (a == LB::PR && (b == LB::JL || b == LB::JV || b == LB::JT || b == LB::H2 || b == LB::H3)) { out[i] = Break::none; continue; }
        // LB28
        if ((a == LB::AL || a == LB::HL) && (b == LB::AL || b == LB::HL)) { out[i] = Break::none; continue; }
        // LB28a
        {
            const auto aks = [](const Ch& c) { return c.cls == LB::AK || c.cls == LB::AS || c.cp == 0x25CC; };
            const auto akd = [](const Ch& c) { return c.cls == LB::AK || c.cp == 0x25CC; };
            if (a == LB::AP && aks(B)) { out[i] = Break::none; continue; }
            if (aks(A) && (b == LB::VF || b == LB::VI)) { out[i] = Break::none; continue; }
            if (a == LB::VI && akd(B)) {
                std::size_t p = i - 1;
                while (p > 0 && attached[p]) --p;
                if (p > 0 && aks(eff[p - 1])) { out[i] = Break::none; continue; }
            }
            if (aks(A) && aks(B)) {
                std::size_t k = i + 1;
                while (k < n && attached[k]) ++k;
                if (k < n && eff[k].cls == LB::VF) { out[i] = Break::none; continue; }
            }
        }
        // LB29
        if (a == LB::IS && (b == LB::AL || b == LB::HL)) { out[i] = Break::none; continue; }
        // LB30
        if ((a == LB::AL || a == LB::HL || a == LB::NU) && b == LB::OP && !B.ea) { out[i] = Break::none; continue; }
        if (a == LB::CP && !A.ea && (b == LB::AL || b == LB::HL || b == LB::NU)) { out[i] = Break::none; continue; }
        // LB30a: count RIs before the position (skipping attached marks)
        if (a == LB::RI && b == LB::RI) {
            std::size_t count = 0;
            std::size_t p = i;
            while (p > 0) {
                std::size_t base = p - 1;
                while (base > 0 && attached[base]) --base;
                if (eff[base].cls != LB::RI) break;
                ++count;
                p = base;
            }
            if (count % 2 == 1) { out[i] = Break::none; continue; }
        }
        // LB30b
        if (b == LB::EM && (a == LB::EB || A.extpict_cn)) { out[i] = Break::none; continue; }
        // LB31
        out[i] = Break::allowed;
    }

    // Tailoring: break between Southeast Asian orthographic syllables.
    if (tailor == Tailoring::sea_syllables) {
        for (std::size_t i = 1; i < n; ++i) {
            if (out[i] != Break::none) continue;
            if (ucd::line_break(text[i - 1]) != LB::SA || ucd::line_break(text[i]) != LB::SA) continue;
            const GC g = ucd::general_category(text[i]);
            if (g == GC::Mn || g == GC::Mc) continue;                    // a mark joins its base
            const char32_t p = text[i - 1], c = text[i];
            // Leading vowels attach to what follows (Thai เ แ โ ใ ไ, Lao ເ ແ ໂ ໃ ໄ).
            if ((p >= 0x0E40 && p <= 0x0E44) || (p >= 0x0EC0 && p <= 0x0EC4)) continue;
            // Following vowels and signs attach to what precedes.
            if (c == 0x0E30 || c == 0x0E32 || c == 0x0E33 || c == 0x0E45 || c == 0x0E46 ||
                c == 0x0EB0 || c == 0x0EB2 || c == 0x0EB3 || c == 0x0EC6)
                continue;
            // Khmer/Myanmar: a coeng/virama or its follower stays in the cluster.
            if (p == 0x17D2 || p == 0x1039 || p == 0x103A) continue;
            out[i] = Break::allowed;
        }
    }
}

/// Byte offsets (into `utf8`) of every break opportunity, excluding 0 and
/// including the mandatory ones; `mandatory` (optional) gets the subset that
/// is mandatory.
inline std::vector<std::size_t> line_break_offsets(std::string_view utf8, Tailoring tailor = Tailoring::none,
                                                   std::vector<std::size_t>* mandatory = nullptr) {
    std::vector<char32_t> cps;
    std::vector<std::size_t> at;
    for (std::size_t i = 0; i < utf8.size();) {
        at.push_back(i);
        const auto b0 = static_cast<unsigned char>(utf8[i]);
        const std::size_t len = b0 < 0x80 ? 1 : (b0 >> 5) == 6 ? 2 : (b0 >> 4) == 14 ? 3 : (b0 >> 3) == 30 ? 4 : 1;
        char32_t cp = len == 1 ? b0 : len == 2 ? (b0 & 0x1Fu) : len == 3 ? (b0 & 0x0Fu) : (b0 & 0x07u);
        for (std::size_t k = 1; k < len && i + k < utf8.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        cps.push_back(cp);
        i += len;
    }
    std::vector<Break> br(cps.size());
    line_breaks(cps, br, tailor);
    std::vector<std::size_t> out;
    for (std::size_t i = 1; i < br.size(); ++i)
        if (br[i] != Break::none) {
            out.push_back(at[i]);
            if (mandatory && br[i] == Break::mandatory) mandatory->push_back(at[i]);
        }
    return out;
}

// ── UAX #29 grapheme clusters ─────────────────────────────────────────────

/// out[i] = true when there is a grapheme boundary before text[i]
/// (out[0] = true, GB1). out.size() must equal text.size().
inline void grapheme_boundaries(std::span<const char32_t> text, std::span<bool> out) {
    using GB = ucd::GraphemeBreak;
    using InCB = ucd::IndicConjunctBreak;
    const std::size_t n = text.size();
    if (out.size() != n || n == 0) return;
    out[0] = true;
    std::size_t ri_run = 0;           // consecutive RIs before the position
    bool emoji_zwj = false;           // ExtPict Extend* ZWJ just seen (GB11)
    bool in_ext_pict = false;         // inside ExtPict Extend*
    bool incb_linker = false;         // InCB=Linker InCB=Extend* just seen (GB9c, Unicode 18)
    for (std::size_t i = 1; i <= n; ++i) {
        const char32_t pc = text[i - 1];
        const GB p = ucd::grapheme_break(pc);
        const InCB pin = ucd::indic_conjunct_break(pc);
        // update lookbehind state with text[i-1]
        if (p == GB::Regional_Indicator) ++ri_run; else ri_run = 0;
        if (ucd::is_extended_pictographic(pc)) { in_ext_pict = true; emoji_zwj = false; }
        else if (p == GB::Extend && in_ext_pict) {}
        else if (p == GB::ZWJ && in_ext_pict) { emoji_zwj = true; in_ext_pict = false; }
        else { in_ext_pict = false; emoji_zwj = false; }
        if (pin == InCB::Linker) incb_linker = true;
        else if (pin != InCB::Extend) incb_linker = false;
        if (i == n) break;

        const char32_t cc = text[i];
        const GB c = ucd::grapheme_break(cc);
        bool brk = true;
        if (p == GB::CR && c == GB::LF) brk = false;                                        // GB3
        else if (p == GB::Control || p == GB::CR || p == GB::LF) brk = true;                // GB4
        else if (c == GB::Control || c == GB::CR || c == GB::LF) brk = true;                // GB5
        else if (p == GB::L && (c == GB::L || c == GB::V || c == GB::LV || c == GB::LVT)) brk = false;   // GB6
        else if ((p == GB::LV || p == GB::V) && (c == GB::V || c == GB::T)) brk = false;   // GB7
        else if ((p == GB::LVT || p == GB::T) && c == GB::T) brk = false;                  // GB8
        else if (c == GB::Extend || c == GB::ZWJ) brk = false;                             // GB9
        else if (c == GB::SpacingMark) brk = false;                                        // GB9a
        else if (p == GB::Prepend) brk = false;                                            // GB9b
        else if (ucd::indic_conjunct_break(cc) == InCB::Consonant && incb_linker) brk = false;   // GB9c
        else if (emoji_zwj && ucd::is_extended_pictographic(cc)) brk = false;             // GB11
        else if (p == GB::Regional_Indicator && c == GB::Regional_Indicator && ri_run % 2 == 1) brk = false;   // GB12/13
        out[i] = brk;                                                                       // GB999
    }
}

namespace detail {
/// Decode one code point at byte `i` (advancing it); lenient on bad UTF-8.
[[nodiscard]] inline char32_t decode_at(std::string_view s, std::size_t& i) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    const std::size_t len = b0 < 0x80 ? 1 : (b0 >> 5) == 6 ? 2 : (b0 >> 4) == 14 ? 3 : (b0 >> 3) == 30 ? 4 : 1;
    char32_t cp = len == 1 ? b0 : len == 2 ? (b0 & 0x1Fu) : len == 3 ? (b0 & 0x0Fu) : (b0 & 0x07u);
    for (std::size_t k = 1; k < len && i + k < s.size(); ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    i += len;
    return cp;
}
/// Start of the code point containing byte `i`.
[[nodiscard]] inline std::size_t cp_start(std::string_view s, std::size_t i) noexcept {
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) --i;
    return i;
}
/// Segment [from, to) and return the boundaries (byte offsets, relative to
/// the string) inside it, using only local context.
inline void boundaries_in(std::string_view s, std::size_t from, std::size_t to, std::vector<std::size_t>& out) {
    std::vector<char32_t> cps;
    std::vector<std::size_t> at;
    for (std::size_t i = from; i < to;) { at.push_back(i); cps.push_back(decode_at(s, i)); }
    std::vector<char> raw(cps.size());
    std::span<bool> b{reinterpret_cast<bool*>(raw.data()), raw.size()};
    grapheme_boundaries(cps, b);
    out.clear();
    for (std::size_t k = 0; k < cps.size(); ++k) if (b[k]) out.push_back(at[k]);
}
}  // namespace detail

/// Byte offset of the next grapheme boundary after `pos` in `utf8` (or
/// size() at the end). Looks back only as far as a grapheme can reach (a
/// few dozen code points: RI parity and GB9c/GB11 chains), so stepping a
/// caret through a long string stays O(length of the cluster).
[[nodiscard]] inline std::size_t next_grapheme(std::string_view utf8, std::size_t pos) {
    if (pos >= utf8.size()) return utf8.size();
    constexpr std::size_t reach = 256;   // bytes of context either side
    std::size_t from = detail::cp_start(utf8, pos > reach ? pos - reach : 0);
    std::vector<std::size_t> b;
    for (std::size_t to = std::min(utf8.size(), pos + reach);; to = std::min(utf8.size(), to + reach * 4)) {
        // End the window at a code point start so it never cuts a sequence.
        if (to < utf8.size()) to = detail::cp_start(utf8, to);
        detail::boundaries_in(utf8, from, to, b);
        for (std::size_t o : b) if (o > pos && o < to) return o;
        if (to >= utf8.size()) return utf8.size();
    }
}

/// Byte offset of the grapheme boundary before `pos` (0 at the start).
[[nodiscard]] inline std::size_t prev_grapheme(std::string_view utf8, std::size_t pos) {
    if (pos == 0 || utf8.empty()) return 0;
    pos = std::min(pos, utf8.size());
    constexpr std::size_t reach = 256;
    for (std::size_t back = reach;; back *= 4) {
        const std::size_t from = detail::cp_start(utf8, pos > back ? pos - back : 0);
        std::vector<std::size_t> b;
        detail::boundaries_in(utf8, from, pos, b);
        // The window start is only a true boundary at 0; skip it otherwise.
        for (std::size_t k = b.size(); k-- > 0;)
            if (b[k] < pos && (b[k] > from || from == 0)) return b[k];
        if (from == 0) return 0;
    }
}

}  // namespace mayag::typo::seg
