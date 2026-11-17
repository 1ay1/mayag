// include/mayag/font/bidi.hpp — the Unicode Bidirectional Algorithm (UAX #9)
//
// A complete, conformant implementation of UAX #9 for Unicode 18, verified
// against BidiCharacterTest.txt and BidiTest.txt (tests/test_unicode.cpp).
//
// Pipeline, by rule:
//   P1      split at paragraph separators (B); each paragraph is resolved alone
//   P2/P3   paragraph level from the first strong char, skipping isolates
//   X1–X8   explicit embeddings/overrides/isolates on a 125-deep stack with
//           overflow-isolate / overflow-embedding / valid-isolate counters
//   X9      embeddings, overrides, PDF and BN are "removed" (kept in the
//           arrays but skipped by every later rule)
//   X10     level runs chained into isolating run sequences (BD13), sos/eos
//   W1–W7   weak types, N0 bracket pairs (BD16, 63-entry stack, U+2329/232A
//           ≡ U+3008/3009, NSM-after-bracket), N1–N2 neutrals, I1–I2 levels
//   L1      per line: S/B, trailing whitespace and isolate formatters reset
//   L2      visual reordering
//
// Removed characters still receive a level (the preceding character's, or the
// paragraph level) so that every logical index appears exactly once in
// visual_order(). Callers that render them simply draw nothing.
//
// The core works on bidi classes; the codepoint entry points look classes up in
// unicode_data.hpp and additionally provide bracket pairs for N0. A class-only
// entry (resolve_classes) exists for BidiTest.txt and for callers who already
// classified text.

#pragma once

#include <mayag/font/unicode_data.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mayag::typo::bidi {

using ucd::BidiClass;

enum class Direction : std::uint8_t { auto_detect, ltr, rtl };

struct Paragraph {
    std::uint8_t base_level = 0;          // level of the FIRST paragraph
    std::vector<std::uint8_t> levels;     // one per input cp (before L1)
    // Paragraph boundaries (P1): paragraph k spans [ends[k-1], ends[k]) with
    // base level bases[k]. Text without B has exactly one entry.
    std::vector<std::size_t> para_ends;
    std::vector<std::uint8_t> para_bases;
};

namespace detail {

inline constexpr int max_depth = 125;

[[nodiscard]] constexpr bool is_isolate_init(BidiClass c) noexcept {
    return c == BidiClass::LRI || c == BidiClass::RLI || c == BidiClass::FSI;
}
[[nodiscard]] constexpr bool is_removed(BidiClass c) noexcept {
    return c == BidiClass::LRE || c == BidiClass::RLE || c == BidiClass::LRO || c == BidiClass::RLO ||
           c == BidiClass::PDF || c == BidiClass::BN;
}
[[nodiscard]] constexpr bool is_ni(BidiClass c) noexcept {
    return c == BidiClass::B || c == BidiClass::S || c == BidiClass::WS || c == BidiClass::ON ||
           is_isolate_init(c) || c == BidiClass::PDI;
}
[[nodiscard]] constexpr std::uint8_t u8(int v) noexcept { return static_cast<std::uint8_t>(v); }

// P2/P3 over [b, e): 0 for L, 1 for R/AL, -1 if no strong char. Isolate
// contents are skipped; stops at B. For FSI, e is the matching PDI.
[[nodiscard]] inline int first_strong(std::span<const BidiClass> cls, std::size_t b, std::size_t e) noexcept {
    int depth = 0;
    for (std::size_t i = b; i < e; ++i) {
        const BidiClass c = cls[i];
        if (is_isolate_init(c)) ++depth;
        else if (c == BidiClass::PDI) {
            if (depth > 0) --depth;  // unmatched PDI at depth 0 is ignored
        } else if (c == BidiClass::B) return -1;
        else if (depth == 0) {
            if (c == BidiClass::L) return 0;
            if (c == BidiClass::R || c == BidiClass::AL) return 1;
        }
    }
    return -1;
}

// Per-position bracket info for N0: canonical pair id and type (0 none).
struct BracketInfo {
    char32_t id = 0;       // canonical OPENING bracket of the pair
    std::uint8_t type = 0; // 1 open, 2 close
};

[[nodiscard]] constexpr char32_t canon_bracket(char32_t c) noexcept {
    if (c == 0x2329) return 0x3008;
    if (c == 0x232A) return 0x3009;
    return c;
}

// Strong direction for N0/N1 (EN/AN count as R); -1 if not strong.
[[nodiscard]] constexpr int strong_dir(BidiClass c) noexcept {
    if (c == BidiClass::L) return 0;
    if (c == BidiClass::R || c == BidiClass::AL || c == BidiClass::EN || c == BidiClass::AN) return 1;
    return -1;
}

// W1..I2 over one isolating run sequence s (indices into the paragraph).
inline void resolve_sequence(std::span<const BidiClass> cls, std::span<const BracketInfo> br,
                             const std::vector<BidiClass>& t, const std::vector<int>& emb, std::vector<int>& lev,
                             const std::vector<std::size_t>& s, int base) {
    const std::size_t m = s.size();
    const std::size_t n = cls.size();
    const int level = emb[s.front()];
    // sos / eos (X10)
    int prev = base;
    for (std::size_t i = s.front(); i-- > 0;)
        if (!is_removed(cls[i])) { prev = emb[i]; break; }
    int next = base;
    if (!is_isolate_init(cls[s.back()])) {
        for (std::size_t i = s.back() + 1; i < n; ++i)
            if (!is_removed(cls[i])) { next = emb[i]; break; }
    }
    const BidiClass sos = (std::max(prev, level) & 1) ? BidiClass::R : BidiClass::L;
    const BidiClass eos = (std::max(next, level) & 1) ? BidiClass::R : BidiClass::L;

    std::vector<BidiClass> tt(m);
    for (std::size_t k = 0; k < m; ++k) tt[k] = t[s[k]];

    // W1
    for (std::size_t k = 0; k < m; ++k) {
        if (tt[k] != BidiClass::NSM) continue;
        if (k == 0) tt[k] = sos;
        else if (is_isolate_init(tt[k - 1]) || tt[k - 1] == BidiClass::PDI) tt[k] = BidiClass::ON;
        else tt[k] = tt[k - 1];
    }
    // W2, W3
    {
        BidiClass last = sos;
        for (auto& c : tt) {
            if (c == BidiClass::L || c == BidiClass::R || c == BidiClass::AL) last = c;
            else if (c == BidiClass::EN && last == BidiClass::AL) c = BidiClass::AN;
        }
        for (auto& c : tt)
            if (c == BidiClass::AL) c = BidiClass::R;
    }
    // W4
    for (std::size_t k = 1; k + 1 < m; ++k) {
        const BidiClass a = tt[k - 1], z = tt[k + 1];
        if (tt[k] == BidiClass::ES && a == BidiClass::EN && z == BidiClass::EN) tt[k] = BidiClass::EN;
        else if (tt[k] == BidiClass::CS && a == z && (a == BidiClass::EN || a == BidiClass::AN)) tt[k] = a;
    }
    // W5
    for (std::size_t k = 0; k < m;) {
        if (tt[k] != BidiClass::ET) { ++k; continue; }
        std::size_t j = k;
        while (j < m && tt[j] == BidiClass::ET) ++j;
        const bool en = (k > 0 && tt[k - 1] == BidiClass::EN) || (j < m && tt[j] == BidiClass::EN);
        if (en)
            for (std::size_t q = k; q < j; ++q) tt[q] = BidiClass::EN;
        k = j;
    }
    // W6
    for (auto& c : tt)
        if (c == BidiClass::ES || c == BidiClass::ET || c == BidiClass::CS) c = BidiClass::ON;
    // W7
    {
        BidiClass last = sos;
        for (auto& c : tt) {
            if (c == BidiClass::L || c == BidiClass::R) last = c;
            else if (c == BidiClass::EN && last == BidiClass::L) c = BidiClass::L;
        }
    }

    const int e = level & 1;
    const BidiClass edir = e ? BidiClass::R : BidiClass::L;
    // N0 (BD16)
    if (!br.empty()) {
        struct Pair { std::size_t o, c; };
        std::vector<Pair> pairs;
        struct Open { char32_t id; std::size_t k; };
        std::vector<Open> st;
        for (std::size_t k = 0; k < m; ++k) {
            const BracketInfo bi = br[s[k]];
            if (bi.type == 0 || tt[k] != BidiClass::ON) continue;
            if (bi.type == 1) {
                if (st.size() == 63) break;
                st.push_back({bi.id, k});
            } else {
                for (std::size_t q = st.size(); q-- > 0;) {
                    if (st[q].id == bi.id) {
                        pairs.push_back({st[q].k, k});
                        st.resize(q);
                        break;
                    }
                }
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.o < y.o; });
        for (const Pair& p : pairs) {
            bool same = false, opp = false;
            for (std::size_t k = p.o + 1; k < p.c; ++k) {
                const int d = strong_dir(tt[k]);
                if (d == e) { same = true; break; }
                if (d >= 0) opp = true;
            }
            BidiClass res = BidiClass::ON;
            if (same) res = edir;
            else if (opp) {
                int ctx = sos == BidiClass::R ? 1 : 0;
                for (std::size_t k = p.o; k-- > 0;) {
                    const int d = strong_dir(tt[k]);
                    if (d >= 0) { ctx = d; break; }
                }
                res = ctx != e ? (ctx ? BidiClass::R : BidiClass::L) : edir;
            }
            if (res == BidiClass::ON) continue;
            for (std::size_t k : {p.o, p.c}) {
                tt[k] = res;
                for (std::size_t q = k + 1; q < m && cls[s[q]] == BidiClass::NSM; ++q) tt[q] = res;
            }
        }
    }
    // N1, N2
    for (std::size_t k = 0; k < m;) {
        if (!is_ni(tt[k])) { ++k; continue; }
        std::size_t j = k;
        while (j < m && is_ni(tt[j])) ++j;
        const int before = k == 0 ? (sos == BidiClass::R) : strong_dir(tt[k - 1]);
        const int after = j == m ? (eos == BidiClass::R) : strong_dir(tt[j]);
        const BidiClass r = (before == after && before >= 0) ? (before ? BidiClass::R : BidiClass::L) : edir;
        for (std::size_t q = k; q < j; ++q) tt[q] = r;
        k = j;
    }
    // I1, I2
    for (std::size_t k = 0; k < m; ++k) {
        const BidiClass c = tt[k];
        int& l = lev[s[k]];
        if (!e) {
            if (c == BidiClass::R) l += 1;
            else if (c == BidiClass::AN || c == BidiClass::EN) l += 2;
        } else if (c == BidiClass::L || c == BidiClass::EN || c == BidiClass::AN) l += 1;
    }
}

// Resolve one paragraph [b, e) (X1..I2). cls: original classes; br: bracket
// info (may be empty). Writes lv[b..e).
inline void resolve_paragraph(std::span<const BidiClass> cls, std::span<const BracketInfo> br, std::size_t b,
                              std::size_t e, int base, std::vector<std::uint8_t>& lv) {
    const std::size_t n = e - b;
    if (n == 0) return;
    std::vector<BidiClass> t(cls.begin() + static_cast<std::ptrdiff_t>(b), cls.begin() + static_cast<std::ptrdiff_t>(e));
    std::vector<int> lev(n, base);

    // BD9: matching PDI for each isolate initiator (n = none).
    std::vector<std::size_t> match(n, n);
    {
        std::vector<std::size_t> st;
        for (std::size_t i = 0; i < n; ++i) {
            if (is_isolate_init(t[i])) st.push_back(i);
            else if (t[i] == BidiClass::PDI && !st.empty()) {
                match[st.back()] = i;
                st.pop_back();
            }
        }
    }

    // X1..X8
    struct Entry { int level; BidiClass ovr; bool isolate; };
    std::vector<Entry> stack;
    stack.reserve(max_depth + 2);
    stack.push_back({base, BidiClass::ON, false});
    int over_iso = 0, over_emb = 0, valid_iso = 0;
    auto next_level = [](int l, bool rtl) { return rtl ? ((l + 1) | 1) : ((l + 2) & ~1); };
    for (std::size_t i = 0; i < n; ++i) {
        const BidiClass c = t[i];
        switch (c) {
        case BidiClass::RLE: case BidiClass::LRE: case BidiClass::RLO: case BidiClass::LRO: {
            lev[i] = stack.back().level;
            const bool rtl = c == BidiClass::RLE || c == BidiClass::RLO;
            const int nl = next_level(stack.back().level, rtl);
            if (nl <= max_depth && over_iso == 0 && over_emb == 0) {
                const BidiClass o = c == BidiClass::RLO ? BidiClass::R : c == BidiClass::LRO ? BidiClass::L : BidiClass::ON;
                stack.push_back({nl, o, false});
            } else if (over_iso == 0) ++over_emb;
            break;
        }
        case BidiClass::RLI: case BidiClass::LRI: case BidiClass::FSI: {
            lev[i] = stack.back().level;
            if (stack.back().ovr != BidiClass::ON) t[i] = stack.back().ovr;
            bool rtl = c == BidiClass::RLI;
            if (c == BidiClass::FSI) {
                const std::size_t end = match[i] == n ? n : match[i];
                rtl = first_strong(std::span<const BidiClass>(cls.data() + b, n), i + 1, end) == 1;
            }
            const int nl = next_level(stack.back().level, rtl);
            if (nl <= max_depth && over_iso == 0 && over_emb == 0) {
                ++valid_iso;
                stack.push_back({nl, BidiClass::ON, true});
            } else ++over_iso;
            break;
        }
        case BidiClass::PDI:
            if (over_iso > 0) --over_iso;
            else if (valid_iso > 0) {
                over_emb = 0;
                while (!stack.back().isolate) stack.pop_back();
                stack.pop_back();
                --valid_iso;
            }
            lev[i] = stack.back().level;
            if (stack.back().ovr != BidiClass::ON) t[i] = stack.back().ovr;
            break;
        case BidiClass::PDF:
            lev[i] = stack.back().level;
            if (over_iso > 0) {}
            else if (over_emb > 0) --over_emb;
            else if (!stack.back().isolate && stack.size() >= 2) stack.pop_back();
            break;
        case BidiClass::B:
            lev[i] = base;
            break;
        case BidiClass::BN:
            lev[i] = stack.back().level;
            break;
        default:
            lev[i] = stack.back().level;
            if (stack.back().ovr != BidiClass::ON) t[i] = stack.back().ovr;
            break;
        }
    }

    // X9/X10: level runs over non-removed chars, chained into isolating run
    // sequences (BD13).
    std::vector<std::vector<std::size_t>> runs;
    {
        int cur = -1;
        for (std::size_t i = 0; i < n; ++i) {
            if (is_removed(cls[b + i])) continue;
            if (runs.empty() || lev[i] != cur) { runs.emplace_back(); cur = lev[i]; }
            runs.back().push_back(i);
        }
    }
    std::vector<std::size_t> run_of(n, 0);
    for (std::size_t r = 0; r < runs.size(); ++r)
        for (std::size_t i : runs[r]) run_of[i] = r;
    std::vector<std::vector<std::size_t>> seqs;
    std::vector<bool> consumed(runs.size(), false);
    for (std::size_t r = 0; r < runs.size(); ++r) {
        if (consumed[r]) continue;
        std::vector<std::size_t> s;
        std::size_t rr = r;
        for (;;) {
            s.insert(s.end(), runs[rr].begin(), runs[rr].end());
            const std::size_t last = runs[rr].back();
            if (!is_isolate_init(cls[b + last]) || match[last] == n) break;
            rr = run_of[match[last]];
            if (runs[rr].front() != match[last]) break;
            consumed[rr] = true;
        }
        seqs.push_back(std::move(s));
    }

    const std::vector<int> emb = lev;  // explicit levels, before I1/I2
    for (const auto& s : seqs)
        resolve_sequence(cls.subspan(b, n), br.empty() ? br : br.subspan(b, n), t, emb, lev, s, base);

    // removed chars take the preceding char's level (or the paragraph level)
    for (std::size_t i = 0; i < n; ++i) {
        if (is_removed(cls[b + i])) lev[i] = i == 0 ? base : lev[i - 1];
        lv[b + i] = u8(lev[i]);
    }
}

} // namespace detail

// Resolve from bidi classes (no bracket info => N0 sees no pairs unless br
// is given). Handles multiple paragraphs (P1).
[[nodiscard]] inline Paragraph resolve_classes(std::span<const BidiClass> cls, Direction dir,
                                               std::span<const detail::BracketInfo> br = {}) {
    Paragraph p;
    const std::size_t n = cls.size();
    p.levels.assign(n, 0);
    std::size_t b = 0;
    bool first = true;
    while (b < n || first) {
        std::size_t e = b;
        while (e < n && cls[e] != BidiClass::B) ++e;
        if (e < n) ++e;  // B belongs to its paragraph
        int base = dir == Direction::rtl ? 1 : 0;
        if (dir == Direction::auto_detect) base = detail::first_strong(cls, b, e) == 1 ? 1 : 0;
        detail::resolve_paragraph(cls, br, b, e, base, p.levels);
        if (first) p.base_level = detail::u8(base);
        p.para_ends.push_back(e);
        p.para_bases.push_back(detail::u8(base));
        first = false;
        b = e;
    }
    return p;
}

[[nodiscard]] inline Paragraph resolve(std::span<const char32_t> text, Direction dir) {
    std::vector<BidiClass> cls(text.size());
    std::vector<detail::BracketInfo> br(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        cls[i] = ucd::bidi_class(text[i]);
        const ucd::Bracket bk = ucd::bidi_bracket(text[i]);
        if (bk.type == ucd::BracketType::open) br[i] = {detail::canon_bracket(text[i]), 1};
        else if (bk.type == ucd::BracketType::close) br[i] = {detail::canon_bracket(bk.pair), 2};
    }
    return resolve_classes(cls, dir, br);
}

// L1 on classes for the line [begin, end).
[[nodiscard]] inline std::vector<std::uint8_t> line_levels_classes(std::span<const BidiClass> cls, const Paragraph& p,
                                                                    std::size_t begin, std::size_t end) {
    std::vector<std::uint8_t> out(p.levels.begin() + static_cast<std::ptrdiff_t>(begin),
                                  p.levels.begin() + static_cast<std::ptrdiff_t>(end));
    auto base_at = [&](std::size_t i) {
        for (std::size_t k = 0; k < p.para_ends.size(); ++k)
            if (i < p.para_ends[k]) return p.para_bases[k];
        return p.base_level;
    };
    auto resettable = [](BidiClass c) {
        return c == BidiClass::WS || detail::is_isolate_init(c) || c == BidiClass::PDI || detail::is_removed(c);
    };
    // walk backwards: trailing run at end of line, and runs before S/B
    bool trailing = true;
    for (std::size_t i = end; i-- > begin;) {
        const BidiClass c = cls[i];
        if (c == BidiClass::S || c == BidiClass::B) {
            out[i - begin] = base_at(i);
            trailing = true;
        } else if (trailing && resettable(c)) {
            out[i - begin] = base_at(i);
        } else trailing = false;
    }
    return out;
}

[[nodiscard]] inline std::vector<std::uint8_t> line_levels(std::span<const char32_t> text, const Paragraph& p,
                                                           std::size_t begin, std::size_t end) {
    std::vector<BidiClass> cls(text.size());
    for (std::size_t i = begin; i < end; ++i) cls[i] = ucd::bidi_class(text[i]);
    return line_levels_classes(cls, p, begin, end);
}

// L2: result[i] = logical index at visual position i.
[[nodiscard]] inline std::vector<std::uint32_t> visual_order(std::span<const std::uint8_t> levels) {
    const std::size_t n = levels.size();
    std::vector<std::uint32_t> ord(n);
    for (std::size_t i = 0; i < n; ++i) ord[i] = static_cast<std::uint32_t>(i);
    if (n == 0) return ord;
    std::uint8_t hi = 0, lo_odd = 255;
    for (auto l : levels) {
        hi = std::max(hi, l);
        if (l & 1) lo_odd = std::min(lo_odd, l);
    }
    for (int lvl = hi; lvl >= lo_odd && lvl > 0; --lvl) {
        for (std::size_t i = 0; i < n;) {
            if (levels[ord[i]] < lvl) { ++i; continue; }
            std::size_t j = i;
            while (j < n && levels[ord[j]] >= lvl) ++j;
            std::reverse(ord.begin() + static_cast<std::ptrdiff_t>(i), ord.begin() + static_cast<std::ptrdiff_t>(j));
            i = j;
        }
    }
    return ord;
}

[[nodiscard]] inline bool is_trivially_ltr(std::span<const char32_t> text) noexcept {
    for (char32_t c : text) {
        if (c < 0x0590) continue;  // nothing below Hebrew is R/AL/AN/explicit
        switch (ucd::bidi_class(c)) {
        case BidiClass::R: case BidiClass::AL: case BidiClass::AN:
        case BidiClass::LRE: case BidiClass::LRO: case BidiClass::RLE: case BidiClass::RLO:
        case BidiClass::PDF: case BidiClass::LRI: case BidiClass::RLI: case BidiClass::FSI: case BidiClass::PDI:
            return false;
        default: break;
        }
    }
    return true;
}

} // namespace mayag::typo::bidi
