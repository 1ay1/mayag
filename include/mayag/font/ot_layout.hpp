#pragma once
// mayag::typo::ot — the OpenType Layout engine (GSUB, GPOS, GDEF)
//
// This is what turns "a font with GSUB/GPOS tables" into correctly shaped
// text for EVERY script the font supports: ligatures, contextual forms,
// Arabic joining forms, Indic conjuncts, mark stacking, cursive attachment,
// kerning, and all of it at a variable font's current instance.
//
// The bar is HarfBuzz. Fonts are tested against HarfBuzz/Uniscribe/CoreText,
// and they encode their behaviour in the GAPS of the OpenType spec (which
// glyphs a lookup skips, how a ligature inherits marks, what a context lookup
// sees after a nested lookup changed the buffer). So this engine reproduces
// HarfBuzz's semantics rule for rule, and tests/test_ot_layout.cpp compares it
// glyph for glyph and unit for unit against hb-shape.
//
// ── Architecture ──────────────────────────────────────────────────────────
//
//   Layout      the font's GDEF/GSUB/GPOS bytes, read IN PLACE. Building one
//               records three spans; per-lookup coverage digests are built
//               once on first use.
//   PlanBuilder collects features (tag, flags, value) and stage pauses, like
//               hb_ot_map_builder_t, then compile() resolves the script and
//               language system, allocates mask bits, and gathers each
//               feature's lookups into ordered stages.
//   Plan        the result: per table, an ordered lookup list split into
//               stages, each lookup carrying the mask bits that enable it.
//   Buffer      glyphs + positions. GSUB uses HarfBuzz's in/out model: the
//               cursor `idx` walks `info`, and substitutions write to `out`;
//               nested (context) lookups move the boundary with move_to().
//
// Masks: every glyph carries a 32-bit mask; a lookup applies to a glyph only
// if (glyph.mask & lookup.mask). Global features share bit 31; features with
// values or per-glyph application get their own bits. Shapers (Arabic, Indic)
// enable a feature on some glyphs by setting that feature's bits there.
//
// Stages: features added between two pauses form a stage; all lookups of a
// stage run in lookup-list order, then the pause callback runs (Indic uses
// this to reorder glyphs between basic and presentation forms).
//
// The skippy iterator: matching a multi-glyph pattern skips glyphs the lookup
// flags say to ignore (marks, ligatures, base glyphs, marks outside a filter
// set or attachment class) and, when asked, default-ignorables (ZWJ/ZWNJ).
//
// Direction: GSUB and GPOS both run in LOGICAL order. position() leaves the
// buffer in logical order; Shapers reverse RTL runs afterwards, exactly as
// HarfBuzz does in hb_ot_position, so attachment offsets are computed for a
// logical-order buffer with an RTL direction flag.
//
// Units: everything is in FONT UNITS (int32). The caller fills each glyph's
// x_advance (hmtx + HVAR) before position(); GPOS adds to it. Rounding
// follows HarfBuzz at scale == upem (anchors and variation deltas round to
// the nearest unit).
//
// Robustness: every read is bounds-checked (ot::Reader returns 0 past the
// end), nested lookups are capped at 64 levels, and every buffer gets an
// operations budget, so a malicious font cannot hang or crash the shaper.
//
// A port of HarfBuzz's OpenType layout (hb-ot-layout*.cc/hh,
// OT/Layout/GSUB, OT/Layout/GPOS); HarfBuzz is "Old MIT" licensed, see
// THIRD_PARTY_NOTICES.md.

#include "opentype.hpp"
#include "variations.hpp"
#include "unicode_data.hpp"

#include <jaal/kernel/waits_for.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace mayag::typo::ot {

enum class Direction : std::uint8_t { ltr, rtl };

// ── glyph properties (HarfBuzz's values, which match the lookup flags) ────
namespace props {
inline constexpr std::uint16_t base_glyph  = 0x02;   ///< == LookupFlag IgnoreBaseGlyphs
inline constexpr std::uint16_t ligature    = 0x04;   ///< == IgnoreLigatures
inline constexpr std::uint16_t mark        = 0x08;   ///< == IgnoreMarks
inline constexpr std::uint16_t substituted = 0x10;
inline constexpr std::uint16_t ligated     = 0x20;
inline constexpr std::uint16_t multiplied  = 0x40;
inline constexpr std::uint16_t preserve    = substituted | ligated | multiplied;
}  // namespace props

namespace lookup_flag {
inline constexpr std::uint32_t right_to_left          = 0x0001;
inline constexpr std::uint32_t ignore_base_glyphs     = 0x0002;
inline constexpr std::uint32_t ignore_ligatures       = 0x0004;
inline constexpr std::uint32_t ignore_marks           = 0x0008;
inline constexpr std::uint32_t ignore_flags           = 0x000E;
inline constexpr std::uint32_t use_mark_filtering_set = 0x0010;
inline constexpr std::uint32_t mark_attachment_type   = 0xFF00;
}  // namespace lookup_flag

/// Unicode-derived flags a shaper sets on each glyph (GlyphInfo::uflags).
namespace uflag {
inline constexpr std::uint8_t ignorable    = 0x01;   ///< Default_Ignorable_Code_Point
inline constexpr std::uint8_t hidden       = 0x02;   ///< CGJ, Mongolian FVS, TAG chars: skipped even by GPOS
inline constexpr std::uint8_t continuation = 0x04;   ///< grapheme continuation
inline constexpr std::uint8_t zwj          = 0x08;
inline constexpr std::uint8_t zwnj         = 0x10;
}  // namespace uflag

/// One glyph in the buffer.
struct GlyphInfo {
    std::uint32_t codepoint = 0;     ///< the character it came from (shapers read it)
    std::uint32_t cluster = 0;       ///< source index (byte offset) of its cluster
    std::uint32_t mask = 0;          ///< feature bits
    std::uint16_t gid = 0;
    std::uint16_t glyph_props = 0;   ///< props:: bits | mark attachment class << 8
    std::uint8_t  lig_props = 0;     ///< lig id << 5 | is-lig-base 0x10 | component/count
    std::uint8_t  gc = 0;            ///< ucd::GeneralCategory
    std::uint8_t  uflags = 0;        ///< uflag:: bits
    std::uint8_t  mcc = 0;           ///< modified combining class (normaliser)
    std::uint8_t  syllable = 0;      ///< syllable serial for per-syllable features
    std::uint8_t  shaper_cat = 0;    ///< opaque scratch for complex shapers
    std::uint8_t  shaper_pos = 0;
    std::uint8_t  aux = 0;
};

/// One glyph's position, in FONT UNITS. Float, unrounded: HarfBuzz rounds at
/// its working scale, which in real use is pixels x 64, i.e. effectively
/// never at font-unit granularity. At a default instance every value is an
/// integer and matches hb-shape --font-size=upem exactly. attach_* are
/// GPOS-internal.
struct GlyphPos {
    float x_advance = 0, y_advance = 0, x_offset = 0, y_offset = 0;
    std::int16_t attach_chain = 0;
    std::uint8_t attach_type = 0;
};

// ── ligature properties (HarfBuzz layout) ─────────────────────────────────
namespace lig {
inline constexpr std::uint8_t is_lig_base = 0x10;
[[nodiscard]] inline unsigned id(const GlyphInfo& g) noexcept { return g.lig_props >> 5; }
[[nodiscard]] inline bool ligated_internal(const GlyphInfo& g) noexcept { return (g.lig_props & is_lig_base) != 0; }
[[nodiscard]] inline unsigned comp(const GlyphInfo& g) noexcept {
    return ligated_internal(g) ? 0u : (g.lig_props & 0x0Fu);
}
[[nodiscard]] inline unsigned num_comps(const GlyphInfo& g) noexcept {
    return ((g.glyph_props & props::ligature) && ligated_internal(g)) ? (g.lig_props & 0x0Fu) : 1u;
}
inline void set_for_ligature(GlyphInfo& g, unsigned lig_id, unsigned n) noexcept {
    g.lig_props = static_cast<std::uint8_t>((lig_id << 5) | is_lig_base | (n & 0x0F));
}
inline void set_for_mark(GlyphInfo& g, unsigned lig_id, unsigned c) noexcept {
    g.lig_props = static_cast<std::uint8_t>((lig_id << 5) | (c & 0x0F));
}
inline void set_for_component(GlyphInfo& g, unsigned c) noexcept { set_for_mark(g, 0, c); }
}  // namespace lig

[[nodiscard]] inline bool is_mark(const GlyphInfo& g) noexcept { return (g.glyph_props & props::mark) != 0; }
[[nodiscard]] inline bool is_base_glyph(const GlyphInfo& g) noexcept { return (g.glyph_props & props::base_glyph) != 0; }
[[nodiscard]] inline bool is_ligature(const GlyphInfo& g) noexcept { return (g.glyph_props & props::ligature) != 0; }
[[nodiscard]] inline bool is_multiplied(const GlyphInfo& g) noexcept { return (g.glyph_props & props::multiplied) != 0; }
/// HarfBuzz's _hb_glyph_info_is_default_ignorable: a default-ignorable that
/// GSUB replaced is a real glyph now and no longer skipped.
[[nodiscard]] inline bool is_default_ignorable(const GlyphInfo& g) noexcept {
    return (g.uflags & uflag::ignorable) && !(g.glyph_props & props::substituted);
}

/// Glyphs and positions, with HarfBuzz's in/out substitution model.
///
/// During a GSUB lookup `idx` walks `info` and results are appended to `out`
/// (next_glyph copies, replace_glyph substitutes, output_glyph inserts). A
/// nested lookup inside a context rule rewinds or advances that boundary
/// with move_to(), which is how context lookups see the effect of earlier
/// nested substitutions. sync() makes `out` the new `info`.
struct Buffer {
    std::vector<GlyphInfo> info;
    std::vector<GlyphPos>  pos;
    Direction direction = Direction::ltr;

    // ── in/out state (GSUB) ───────────────────────────────────────────────
    std::vector<GlyphInfo> out;
    std::size_t idx = 0;
    bool have_output = false;
    unsigned serial = 0;          ///< ligature id allocator
    std::int64_t max_ops = 0;     ///< operation budget (set by substitute/position)
    std::size_t max_len = 0;      ///< growth cap (set by substitute)
    bool successful = true;       ///< false once a budget ran out: stop, keep what we have
    bool has_attachment = false;  ///< HB_BUFFER_SCRATCH_FLAG_HAS_GPOS_ATTACHMENT, set by AAT cross-stream kern
    std::uint32_t random_state = 1;

    [[nodiscard]] std::size_t len() const noexcept { return info.size(); }
    [[nodiscard]] GlyphInfo& cur(std::size_t i = 0) noexcept { return info[idx + i]; }
    [[nodiscard]] const GlyphInfo& cur(std::size_t i = 0) const noexcept { return info[idx + i]; }
    [[nodiscard]] std::size_t out_len() const noexcept { return have_output ? out.size() : idx; }
    [[nodiscard]] std::size_t backtrack_len() const noexcept { return have_output ? out.size() : idx; }
    [[nodiscard]] std::size_t lookahead_len() const noexcept { return info.size() - idx; }
    /// The glyph `i` in the BACKTRACK buffer: `out` while substituting, else `info`.
    [[nodiscard]] GlyphInfo& out_info(std::size_t i) noexcept { return have_output ? out[i] : info[i]; }

    void clear_output() { have_output = true; out.clear(); out.reserve(info.size() + 8); }
    void remove_output() noexcept { have_output = false; out.clear(); }
    void sync() {
        if (!have_output) return;
        // Whatever the cursor did not reach is copied through unchanged.
        out.insert(out.end(), info.begin() + static_cast<std::ptrdiff_t>(idx), info.end());
        info.swap(out);
        out.clear();
        have_output = false;
        idx = 0;
    }

    void next_glyph() {
        if (have_output) out.push_back(info[idx]);
        ++idx;
    }
    void skip_glyph() noexcept { ++idx; }
    /// hb_buffer_t::replace_glyphs on CODEPOINTS (pre-GSUB shapers): consume
    /// `n_in` glyphs, merged into one cluster, and output `out` copies of
    /// the first, each with the given codepoint.
    void replace_codepoints(std::size_t n_in, std::span<const char32_t> cps) {
        merge_clusters(idx, idx + n_in);
        const GlyphInfo orig = idx < info.size() ? info[idx] : out.back();
        for (char32_t c : cps) { out.push_back(orig); out.back().codepoint = c; }
        idx += n_in;
    }
    void replace_glyph(std::uint16_t g) {
        if (have_output) { out.push_back(info[idx]); out.back().gid = g; }
        else info[idx].gid = g;
        ++idx;
    }
    /// Append a copy of the current glyph as `g` without advancing.
    void output_glyph(std::uint16_t g) {
        if (max_len && out.size() >= max_len) { successful = false; return; }
        out.push_back(info[idx]);
        out.back().gid = g;
    }

    /// Make the output buffer `i` glyphs long, moving glyphs across the
    /// boundary in either direction (hb_buffer_t::move_to).
    bool move_to(std::size_t i) {
        if (!have_output) { idx = std::min(i, info.size()); return true; }
        if (i > out.size() + (info.size() - idx)) return false;
        if (out.size() < i) {
            const std::size_t count = i - out.size();
            out.insert(out.end(), info.begin() + static_cast<std::ptrdiff_t>(idx),
                       info.begin() + static_cast<std::ptrdiff_t>(idx + count));
            idx += count;
        } else if (out.size() > i) {
            const std::size_t count = out.size() - i;
            if (idx < count) {   // not enough room before idx: shift the input forward
                const std::size_t grow = count - idx;
                info.insert(info.begin(), grow, GlyphInfo{});
                idx += grow;
            }
            idx -= count;
            std::copy(out.end() - static_cast<std::ptrdiff_t>(count), out.end(),
                      info.begin() + static_cast<std::ptrdiff_t>(idx));
            out.resize(i);
        }
        return true;
    }

    // ── clusters (HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES) ─────────────
    void merge_clusters(std::size_t start, std::size_t end) {
        if (end - start < 2) return;
        std::uint32_t cl = info[start].cluster;
        for (std::size_t i = start + 1; i < end; ++i) cl = std::min(cl, info[i].cluster);
        if (cl != info[end - 1].cluster)
            while (end < info.size() && info[end - 1].cluster == info[end].cluster) ++end;
        if (cl != info[start].cluster)
            while (idx < start && info[start - 1].cluster == info[start].cluster) --start;
        if (idx == start && info[start].cluster != cl && have_output)
            for (std::size_t i = out.size(); i && out[i - 1].cluster == info[start].cluster; --i)
                out[i - 1].cluster = cl;
        for (std::size_t i = start; i < end; ++i) info[i].cluster = cl;
    }
    /// merge_clusters over an OUTPUT range (used by shapers after GSUB).
    void merge_out_clusters(std::size_t start, std::size_t end) {
        if (end - start < 2) return;
        std::uint32_t cl = out[start].cluster;
        for (std::size_t i = start + 1; i < end; ++i) cl = std::min(cl, out[i].cluster);
        while (start && out[start - 1].cluster == out[start].cluster) --start;
        while (end < out.size() && out[end - 1].cluster == out[end].cluster) ++end;
        if (end == out.size())
            for (std::size_t i = idx; i < info.size() && info[i].cluster == out[end - 1].cluster; ++i)
                info[i].cluster = cl;
        for (std::size_t i = start; i < end; ++i) out[i].cluster = cl;
    }
    /// Delete the current glyph, keeping its cluster alive (hb delete_glyph).
    void delete_glyph() {
        const std::uint32_t cl = info[idx].cluster;
        if ((idx + 1 < info.size() && cl == info[idx + 1].cluster) ||
            (!out.empty() && cl == out.back().cluster)) {
            skip_glyph();
            return;
        }
        if (!out.empty()) {
            if (cl < out.back().cluster) {
                const std::uint32_t old = out.back().cluster;
                for (std::size_t i = out.size(); i && out[i - 1].cluster == old; --i) out[i - 1].cluster = cl;
            }
        } else if (idx + 1 < info.size()) {
            merge_clusters(idx, idx + 2);
        }
        skip_glyph();
    }
    [[nodiscard]] unsigned allocate_lig_id() noexcept {
        unsigned id = ++serial & 7u;
        if (id == 0) id = ++serial & 7u;
        return id;
    }
    void reverse_range(std::size_t start, std::size_t end) {
        std::reverse(info.begin() + static_cast<std::ptrdiff_t>(start), info.begin() + static_cast<std::ptrdiff_t>(end));
        if (pos.size() == info.size())
            std::reverse(pos.begin() + static_cast<std::ptrdiff_t>(start), pos.begin() + static_cast<std::ptrdiff_t>(end));
    }
    void reverse() { reverse_range(0, info.size()); }
};

// ── table primitives ──────────────────────────────────────────────────────────
//
// Offsets are absolute positions inside one table's bytes. A NULL offset
// becomes `npos`, which every Reader read turns into 0: an absent Coverage
// covers nothing and an absent ClassDef puts every glyph in class 0, which
// is exactly how the spec (and HarfBuzz's Null objects) treat them.
inline constexpr std::size_t npos = static_cast<std::size_t>(-1) / 4;

[[nodiscard]] inline std::size_t off16(const Reader& r, std::size_t base, std::size_t at) noexcept {
    const std::uint16_t o = r.u16_at(at);
    return (o == 0 || base == npos) ? npos : base + o;
}
[[nodiscard]] inline std::size_t off32(const Reader& r, std::size_t base, std::size_t at) noexcept {
    const std::uint32_t o = r.u32_at(at);
    return (o == 0 || base == npos) ? npos : base + o;
}

/// Coverage index of `g`, or -1.
[[nodiscard]] inline int coverage(const Reader& r, std::size_t cov, std::uint16_t g) noexcept {
    if (cov == npos) return -1;
    const std::uint16_t fmt = r.u16_at(cov);
    std::size_t lo = 0, hi = r.u16_at(cov + 2);
    if (fmt == 1) {
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            const std::uint16_t v = r.u16_at(cov + 4 + mid * 2);
            if (v == g) return static_cast<int>(mid);
            if (v < g) lo = mid + 1; else hi = mid;
        }
    } else if (fmt == 2) {
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            const std::size_t rec = cov + 4 + mid * 6;
            const std::uint16_t s = r.u16_at(rec), e = r.u16_at(rec + 2);
            if (g < s) hi = mid;
            else if (g > e) lo = mid + 1;
            else return static_cast<int>(r.u16_at(rec + 4) + (g - s));
        }
    }
    return -1;
}

/// ClassDef class of `g` (0 when unlisted).
[[nodiscard]] inline unsigned class_of(const Reader& r, std::size_t cd, std::uint16_t g) noexcept {
    if (cd == npos) return 0;
    const std::uint16_t fmt = r.u16_at(cd);
    if (fmt == 1) {
        const std::uint16_t start = r.u16_at(cd + 2), n = r.u16_at(cd + 4);
        if (g >= start && g - start < n) return r.u16_at(cd + 6 + static_cast<std::size_t>(g - start) * 2);
    } else if (fmt == 2) {
        std::size_t lo = 0, hi = r.u16_at(cd + 2);
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            const std::size_t rec = cd + 4 + mid * 6;
            const std::uint16_t s = r.u16_at(rec), e = r.u16_at(rec + 2);
            if (g < s) hi = mid;
            else if (g > e) lo = mid + 1;
            else return r.u16_at(rec + 4);
        }
    }
    return 0;
}

/// A cheap "might this glyph be covered" filter, HarfBuzz's set digest: three
/// 64-bit masks over different bit slices of the glyph id. A lookup whose
/// digest rejects a glyph is skipped without touching the font, which is
/// most lookups for most glyphs.
struct Digest {
    std::uint64_t m[3]{};
    static constexpr unsigned shift[3] = {4, 0, 9};
    void add(std::uint16_t g) noexcept {
        for (int i = 0; i < 3; ++i) m[i] |= std::uint64_t{1} << ((g >> shift[i]) & 63);
    }
    void add_range(std::uint16_t a, std::uint16_t b) noexcept {
        if (b < a) return;
        for (int i = 0; i < 3; ++i) {
            const unsigned lo = a >> shift[i], hi = b >> shift[i];
            if (hi - lo >= 63) { m[i] = ~std::uint64_t{0}; continue; }
            // Bits lo..hi (mod 64) in one or two masks, no per-bit loop.
            const unsigned s = lo & 63, n = hi - lo + 1;
            const std::uint64_t run = (n >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << n) - 1);
            m[i] |= (run << s) | (s ? run >> (64 - s) : 0);
        }
    }
    void add(const Digest& o) noexcept { for (int i = 0; i < 3; ++i) m[i] |= o.m[i]; }
    void fill() noexcept { for (auto& v : m) v = ~std::uint64_t{0}; }
    void add_coverage(const Reader& r, std::size_t cov) noexcept {
        if (cov == npos) return;
        const std::uint16_t fmt = r.u16_at(cov), n = r.u16_at(cov + 2);
        // A coverage table can't hold more entries than its bytes allow; a
        // count that claims more is corrupt. Past a size where the digest is
        // saturated anyway, stop and say "may have anything": the digest is
        // only a filter, the real coverage test still runs.
        const std::size_t stride = fmt == 1 ? 2 : 6;
        if (cov + 4 + std::size_t{n} * stride > r.size() || n > 2048) { fill(); return; }
        for (std::size_t i = 0; i < n; ++i) {
            if (fmt == 1) add(r.u16_at(cov + 4 + i * 2));
            else if (fmt == 2) add_range(r.u16_at(cov + 4 + i * 6), r.u16_at(cov + 6 + i * 6));
        }
    }
    [[nodiscard]] bool may_have(std::uint16_t g) const noexcept {
        for (int i = 0; i < 3; ++i)
            if (((m[i] >> ((g >> shift[i]) & 63)) & 1) == 0) return false;
        return true;
    }
};

/// One lookup, with extension subtables unwrapped.
struct LookupInfo {
    std::uint16_t type = 0;                 ///< the REAL type (never 7/9 extension)
    std::uint32_t props = 0;                ///< LookupFlag | markFilteringSet << 16
    std::vector<std::size_t> subtables;     ///< absolute offsets
    std::vector<Digest> sub_digest;
    Digest digest;
};

namespace table { inline constexpr int gsub = 0, gpos = 1; }

/// A face's layout tables, read in place.
class Layout {
  public:
    Layout() = default;
    explicit Layout(const FontFile& f) { init(f.table(tags::gdef), f.table(tags::gsub), f.table(tags::gpos)); }
    Layout(std::span<const std::uint8_t> gdef, std::span<const std::uint8_t> gsub,
           std::span<const std::uint8_t> gpos) { init(gdef, gsub, gpos); }

    Layout(const Layout&) = delete;
    Layout& operator=(const Layout&) = delete;

    struct Table {
        std::span<const std::uint8_t> data;
        std::size_t script_list = npos, feature_list = npos, lookup_list = npos, feature_variations = npos;
        std::uint16_t lookup_count = 0;
        [[nodiscard]] Reader reader() const noexcept { return Reader{data}; }
    };

    /// A GSUB/GPOS table is present (HarfBuzz has_data: version != 0), even
    /// with no lookups: an empty GPOS still means "this font positions with
    /// GPOS", which turns off fallback mark positioning.
    [[nodiscard]] bool has_gsub() const noexcept { return !tables_[0].data.empty(); }
    [[nodiscard]] bool has_gpos() const noexcept { return !tables_[1].data.empty(); }
    [[nodiscard]] const Table& tab(int ti) const noexcept { return tables_[ti]; }

    // ── GDEF ────────────────────────────────────────────────────────────────
    [[nodiscard]] bool has_glyph_classes() const noexcept { return glyph_class_def_ != npos; }
    [[nodiscard]] unsigned glyph_class(std::uint16_t g) const noexcept {
        return class_of(Reader{gdef_}, glyph_class_def_, g);
    }
    [[nodiscard]] unsigned mark_attach_class(std::uint16_t g) const noexcept {
        return class_of(Reader{gdef_}, mark_attach_def_, g);
    }
    /// props:: bits for a glyph by its GDEF class (hb GDEF::get_glyph_props).
    [[nodiscard]] std::uint16_t glyph_props(std::uint16_t g) const noexcept {
        switch (glyph_class(g)) {
            case 1: return props::base_glyph;
            case 2: return props::ligature;
            case 3: return static_cast<std::uint16_t>(props::mark | (mark_attach_class(g) << 8));
            default: return 0;
        }
    }
    [[nodiscard]] bool mark_set_covers(unsigned set, std::uint16_t g) const noexcept {
        const Reader r{gdef_};
        if (mark_sets_ == npos || r.u16_at(mark_sets_) != 1 || set >= r.u16_at(mark_sets_ + 2)) return false;
        return coverage(r, off32(r, mark_sets_, mark_sets_ + 4 + std::size_t{set} * 4), g) >= 0;
    }
    /// An item-variation-store delta (font units, unrounded) at `coords`.
    [[nodiscard]] float var_delta(std::uint16_t outer, std::uint16_t inner,
                                  const std::vector<float>& coords) const {
        if (var_store_ == npos || coords.empty()) return 0.0f;
        // Every region's scalar is 0 at the default instance (all-zero
        // coords), so skip the store walk there: it is the common case.
        bool any = false;
        for (float c : coords) any |= (c != 0.0f);
        if (!any) return 0.0f;
        return var::ivs_delta(gdef_, var_store_, outer, inner, coords);
    }

    /// Lookups of one table, extension-unwrapped, with digests. Built on
    /// first use, once (jaal::once: the body takes no lock and never waits),
    /// so opening a face stays free.
    [[nodiscard]] const std::vector<LookupInfo>& lookups(int ti) const {
        return (*lookups_)[static_cast<std::size_t>(ti)].get([&] { return build_lookups(ti); });
    }

  private:
    void init(std::span<const std::uint8_t> gdef, std::span<const std::uint8_t> gsub,
              std::span<const std::uint8_t> gpos) {
        // Known fonts whose GDEF misclassifies spacing glyphs as marks
        // (Times New Roman Italic's '"', Tahoma's IPA, some Courier New and
        // Arial versions): HarfBuzz ignores their GDEF, keyed by the three
        // table sizes, and so must we, or those glyphs lose their width.
        static constexpr std::uint32_t blocklist[][3] = {
            {442, 2874, 42038}, {430, 2874, 40662}, {442, 2874, 39116}, {430, 2874, 39374},
            {490, 3046, 41638}, {478, 3046, 41902}, {898, 12554, 46470}, {910, 12566, 47732},
            {928, 23298, 59332}, {940, 23310, 60732}, {964, 23836, 60072}, {976, 23832, 61456},
            {994, 24474, 60336}, {1006, 24470, 61740}, {1006, 24576, 61346}, {1018, 24572, 62828},
            {1006, 24576, 61352}, {1018, 24572, 62834}, {832, 7324, 47162}, {844, 7302, 45474},
            {180, 13054, 7254}, {192, 12638, 7254}, {192, 12690, 7254}, {188, 248, 3852},
            {188, 264, 3426}, {1058, 47032, 11818}, {1046, 47030, 12600}, {1058, 71796, 16770},
            {1046, 71790, 17862}, {1046, 71788, 17112}, {1058, 71794, 17514}, {1330, 109904, 57938},
            {1330, 109904, 58972}, {1004, 59092, 14836}, {588, 5078, 14418}, {588, 5078, 14238},
            {894, 17162, 33960}, {894, 17154, 34472}, {816, 7868, 17052}, {816, 7868, 17138},
        };
        for (const auto& e : blocklist)
            if (gdef.size() == e[0] && gsub.size() == e[1] && gpos.size() == e[2]) { gdef = {}; break; }
        gdef_ = gdef;
        const Reader g{gdef};
        if (gdef.size() >= 12 && g.u16_at(0) == 1) {
            glyph_class_def_ = off16(g, 0, 4);
            mark_attach_def_ = off16(g, 0, 10);
            if (g.u16_at(2) >= 2) mark_sets_ = off16(g, 0, 12);
            if (g.u16_at(2) >= 3) var_store_ = off32(g, 0, 14);
        }
        const std::span<const std::uint8_t> t[2] = {gsub, gpos};
        for (int i = 0; i < 2; ++i) {
            const Reader r{t[i]};
            if (t[i].size() < 10 || r.u16_at(0) != 1) continue;
            Table& tb = tables_[i];
            tb.data = t[i];
            tb.script_list = off16(r, 0, 4);
            tb.feature_list = off16(r, 0, 6);
            tb.lookup_list = off16(r, 0, 8);
            if (r.u16_at(2) >= 1) tb.feature_variations = off32(r, 0, 10);
            tb.lookup_count = r.u16_at(tb.lookup_list);
            // A lookup count the LookupList's bytes cannot hold is corrupt;
            // clamp to what is really there.
            if (tb.lookup_list != npos) {
                const std::size_t room = tb.lookup_list + 2 <= t[i].size() ? (t[i].size() - tb.lookup_list - 2) / 2 : 0;
                if (tb.lookup_count > room) tb.lookup_count = static_cast<std::uint16_t>(room);
            }
        }
    }

    [[nodiscard]] std::vector<LookupInfo> build_lookups(int ti) const;

    std::span<const std::uint8_t> gdef_;
    std::size_t glyph_class_def_ = npos, mark_attach_def_ = npos, mark_sets_ = npos, var_store_ = npos;
    Table tables_[2];
    // Built on first use, once, then read-only (jaal::lazy): shared by every
    // thread that shapes with this face. In a unique_ptr so Layout can move.
    std::unique_ptr<const std::array<jaal::lazy<std::vector<LookupInfo>>, 2>> lookups_ =
        std::make_unique<const std::array<jaal::lazy<std::vector<LookupInfo>>, 2>>();
};

/// Where a subtable's FIRST-glyph coverage lives (for digests), by real type.
[[nodiscard]] inline std::size_t first_coverage(const Reader& r, int ti, unsigned type, std::size_t s) noexcept {
    const bool context = (ti == table::gsub) ? (type == 5) : (type == 7);
    const bool chain   = (ti == table::gsub) ? (type == 6) : (type == 8);
    const std::uint16_t fmt = r.u16_at(s);
    if (context && fmt == 3) return off16(r, s, s + 6);
    if (chain && fmt == 3) {
        const std::size_t bt = r.u16_at(s + 2);
        return off16(r, s, s + 4 + bt * 2 + 2);
    }
    return off16(r, s, s + 2);
}

inline std::vector<LookupInfo> Layout::build_lookups(int ti) const {
    const Table& tb = tables_[ti];
    std::vector<LookupInfo> out;
    out.resize(tb.lookup_count);
    const Reader r = tb.reader();
    const std::uint16_t ext = (ti == table::gsub) ? 7 : 9;
    for (std::size_t i = 0; i < tb.lookup_count; ++i) {
        const std::size_t lo = off16(r, tb.lookup_list, tb.lookup_list + 2 + i * 2);
        LookupInfo& li = out[i];
        if (lo == npos) continue;
        const std::uint16_t type = r.u16_at(lo), flag = r.u16_at(lo + 2), n = r.u16_at(lo + 4);
        // A subtable offset array longer than the table is corrupt.
        if (lo + 6 + std::size_t{n} * 2 > tb.data.size()) continue;
        li.props = flag;
        if (flag & lookup_flag::use_mark_filtering_set)
            li.props |= std::uint32_t{r.u16_at(lo + 6 + std::size_t{n} * 2)} << 16;
        li.type = type;
        for (std::size_t k = 0; k < n; ++k) {
            std::size_t s = off16(r, lo, lo + 6 + k * 2);
            if (s == npos) continue;
            if (type == ext) {   // {format, extensionLookupType, offset32}
                if (r.u16_at(s) != 1) continue;
                li.type = r.u16_at(s + 2);
                s = off32(r, s, s + 4);
                if (s == npos) continue;
            }
            li.subtables.push_back(s);
            Digest d;
            d.add_coverage(r, first_coverage(r, ti, li.type, s));
            li.sub_digest.push_back(d);
            li.digest.add(d);
        }
    }
    return out;
}

// ── plans ─────────────────────────────────────────────────────────────────────

enum FeatureFlags : std::uint32_t {
    F_NONE = 0, F_GLOBAL = 1, F_HAS_FALLBACK = 2, F_MANUAL_ZWNJ = 4, F_MANUAL_ZWJ = 8,
    F_MANUAL_JOINERS = F_MANUAL_ZWNJ | F_MANUAL_ZWJ, F_GLOBAL_MANUAL_JOINERS = F_GLOBAL | F_MANUAL_JOINERS,
    F_GLOBAL_HAS_FALLBACK = F_GLOBAL | F_HAS_FALLBACK, F_GLOBAL_SEARCH = 16, F_RANDOM = 32,
    F_PER_SYLLABLE = 64,
};
inline constexpr unsigned max_feature_value = 255;   ///< HB_OT_MAP_MAX_VALUE

[[nodiscard]] constexpr std::uint32_t make_tag(char a, char b, char c, char d) noexcept {
    return (std::uint32_t(std::uint8_t(a)) << 24) | (std::uint32_t(std::uint8_t(b)) << 16) |
           (std::uint32_t(std::uint8_t(c)) << 8) | std::uint32_t(std::uint8_t(d));
}
inline constexpr std::uint32_t tag_DFLT = make_tag('D', 'F', 'L', 'T');
inline constexpr std::uint32_t tag_dflt = make_tag('d', 'f', 'l', 't');
inline constexpr std::uint32_t tag_latn = make_tag('l', 'a', 't', 'n');

struct Plan;
/// Runs between two stages. Returns nothing; may rewrite the buffer freely.
using PauseFn = void (*)(const Plan&, Buffer&, void* user);

struct FeatureMap {
    std::uint32_t tag = 0;
    std::uint32_t index[2] = {0xFFFF, 0xFFFF};   ///< feature index per table, 0xFFFF = absent
    unsigned stage[2] = {0, 0};
    unsigned shift = 0;
    std::uint32_t mask = 0, one_mask = 0;
    bool auto_zwnj = true, auto_zwj = true, random = false, per_syllable = false, needs_fallback = false;
};
struct LookupMap {
    std::uint16_t index = 0;
    std::uint32_t mask = 0;
    bool auto_zwnj = true, auto_zwj = true, random = false, per_syllable = false;
    std::uint32_t feature_tag = 0;
};
struct StageMap {
    std::size_t last_lookup = 0;
    PauseFn pause = nullptr;
};

/// A compiled shaping plan for one (font, script, language, direction,
/// feature set, instance). Immutable; cheap to keep per shaped run.
struct Plan {
    std::vector<FeatureMap> features;          ///< sorted by tag
    std::vector<LookupMap> lookups[2];
    std::vector<StageMap> stages[2];
    std::uint32_t global = 0;
    std::uint32_t chosen_script[2] = {0, 0};
    bool found_script[2] = {false, false};
    Direction direction = Direction::ltr;

    [[nodiscard]] const FeatureMap* feature(std::uint32_t tag) const noexcept {
        auto it = std::lower_bound(features.begin(), features.end(), tag,
                                   [](const FeatureMap& f, std::uint32_t t) { return f.tag < t; });
        return (it != features.end() && it->tag == tag) ? &*it : nullptr;
    }
    [[nodiscard]] std::uint32_t mask_of(std::uint32_t tag) const noexcept {
        const FeatureMap* f = feature(tag);
        return f ? f->mask : 0;
    }
    /// The mask that sets the feature to value 1 (what shapers OR in).
    [[nodiscard]] std::uint32_t one_mask_of(std::uint32_t tag) const noexcept {
        const FeatureMap* f = feature(tag);
        return f ? f->one_mask : 0;
    }
    [[nodiscard]] bool needs_fallback(std::uint32_t tag) const noexcept {
        const FeatureMap* f = feature(tag);
        return f && f->needs_fallback;
    }
    [[nodiscard]] std::uint32_t global_mask() const noexcept { return global; }
};

namespace detail {

/// Script record index for `tag`, or -1.
[[nodiscard]] inline int find_script(const Layout::Table& t, std::uint32_t tag) noexcept {
    const Reader r = t.reader();
    const std::uint16_t n = r.u16_at(t.script_list);
    for (std::size_t i = 0; i < n; ++i)
        if (r.u32_at(t.script_list + 2 + i * 6) == tag) return static_cast<int>(i);
    return -1;
}

/// hb_ot_layout_table_select_script: the candidates, then DFLT, dflt, latn.
/// Returns the script record index (-1: none) and sets `chosen`/`found`.
inline int select_script(const Layout::Table& t, std::span<const std::uint32_t> candidates,
                         std::uint32_t& chosen, bool& found) noexcept {
    found = false;
    for (std::uint32_t c : candidates)
        if (const int i = find_script(t, c); i >= 0) { chosen = c; found = true; return i; }
    for (std::uint32_t c : {tag_DFLT, tag_dflt, tag_latn})
        if (const int i = find_script(t, c); i >= 0) { chosen = c; return i; }
    chosen = 0;
    return -1;
}

/// The LangSys table for `language` in a script (its default LangSys when the
/// language is absent, after trying 'dflt'); npos when there is none.
inline std::size_t select_langsys(const Layout::Table& t, int script, std::span<const std::uint32_t> languages) noexcept {
    if (script < 0) return npos;
    const Reader r = t.reader();
    const std::size_t s = off16(r, t.script_list, t.script_list + 2 + std::size_t(script) * 6 + 4);
    if (s == npos) return npos;
    const std::uint16_t n = r.u16_at(s + 2);
    // The requested languages in priority order, then 'dflt' (hb
    // hb_ot_layout_script_select_language), then the default LangSys.
    const auto find = [&](std::uint32_t want) -> std::size_t {
        for (std::size_t i = 0; i < n; ++i)
            if (r.u32_at(s + 4 + i * 6) == want) return off16(r, s, s + 4 + i * 6 + 4);
        return npos;
    };
    for (std::uint32_t want : languages)
        if (want != 0) if (const std::size_t ls = find(want); ls != npos) return ls;
    if (const std::size_t ls = find(tag_dflt); ls != npos) return ls;
    return off16(r, s, s);
}

[[nodiscard]] inline std::uint32_t feature_tag(const Layout::Table& t, std::uint32_t index) noexcept {
    const Reader r = t.reader();
    if (index >= r.u16_at(t.feature_list)) return 0;
    return r.u32_at(t.feature_list + 2 + std::size_t(index) * 6);
}

/// FeatureVariations: the first record whose condition set holds at
/// `coords` (HarfBuzz compares in F2DOT14, so round first). -1 when none.
inline long variations_index(const Layout::Table& t, const std::vector<float>& coords) noexcept {
    if (t.feature_variations == npos) return -1;
    const Reader r = t.reader();
    const std::size_t fv = t.feature_variations;
    const std::uint32_t n = r.u32_at(fv + 4);
    for (std::uint32_t i = 0; i < n && i < 4096; ++i) {
        const std::size_t cs = off32(r, fv, fv + 8 + std::size_t(i) * 8);
        bool ok = true;
        const std::uint16_t nc = (cs == npos) ? 0 : r.u16_at(cs);
        for (std::size_t c = 0; c < nc && ok; ++c) {
            const std::size_t cond = off32(r, cs, cs + 2 + c * 4);
            if (r.u16_at(cond) != 1) { ok = false; break; }
            const std::uint16_t axis = r.u16_at(cond + 2);
            const int lo = r.i16_at(cond + 4), hi = r.i16_at(cond + 6);
            const int v = axis < coords.size() ? static_cast<int>(std::lround(coords[axis] * 16384.0f)) : 0;
            ok = lo <= v && v <= hi;
        }
        if (ok) return static_cast<long>(i);
    }
    return -1;
}

/// The Feature table for `index`, after FeatureVariations substitution.
inline std::size_t feature_table(const Layout::Table& t, std::uint32_t index, long var_index) noexcept {
    const Reader r = t.reader();
    if (var_index >= 0) {
        const std::size_t fv = t.feature_variations;
        const std::size_t sub = off32(r, fv, fv + 8 + std::size_t(var_index) * 8 + 4);
        const std::uint16_t n = r.u16_at(sub + 4);
        for (std::size_t i = 0; i < n; ++i)
            if (r.u16_at(sub + 6 + i * 6) == index) return off32(r, sub, sub + 6 + i * 6 + 2);
    }
    if (index >= r.u16_at(t.feature_list)) return npos;
    return off16(r, t.feature_list, t.feature_list + 2 + std::size_t(index) * 6 + 4);
}

}  // namespace detail

/// Collects features and stage pauses, then compiles a Plan
/// (hb_ot_map_builder_t). Features are added in stages: everything added
/// before the first pause is stage 0, and so on.
class PlanBuilder {
  public:
    PlanBuilder(const Layout& l, std::span<const std::uint32_t> scripts, std::uint32_t language,
                Direction dir)
        : PlanBuilder(l, scripts, std::span<const std::uint32_t>{&language, language ? 1u : 0u}, dir) {}
    PlanBuilder(const Layout& l, std::span<const std::uint32_t> scripts, std::span<const std::uint32_t> languages,
                Direction dir)
        : layout_(l), dir_(dir) {
        for (int ti = 0; ti < 2; ++ti) {
            script_[ti] = detail::select_script(l.tab(ti), scripts, chosen_[ti], found_[ti]);
            langsys_[ti] = detail::select_langsys(l.tab(ti), script_[ti], languages);
        }
    }

    void add_feature(std::uint32_t tag, std::uint32_t flags = F_NONE, unsigned value = 1) {
        if (tag == 0) return;
        Info f;
        f.tag = tag;
        f.seq = static_cast<unsigned>(infos_.size());
        f.max_value = value;
        f.flags = flags;
        f.default_value = (flags & F_GLOBAL) ? value : 0;
        f.stage[0] = stage_[0];
        f.stage[1] = stage_[1];
        infos_.push_back(f);
    }
    void enable_feature(std::uint32_t tag, std::uint32_t flags = F_NONE, unsigned value = 1) {
        add_feature(tag, F_GLOBAL | flags, value);
    }
    void disable_feature(std::uint32_t tag) { add_feature(tag, F_GLOBAL, 0); }
    void add_gsub_pause(PauseFn fn) { pauses_[0].push_back({stage_[0]++, fn}); }
    void add_gpos_pause(PauseFn fn) { pauses_[1].push_back({stage_[1]++, fn}); }

    [[nodiscard]] std::uint32_t chosen_script(int ti) const noexcept { return chosen_[ti]; }
    [[nodiscard]] bool found_script(int ti) const noexcept { return found_[ti]; }

    [[nodiscard]] Plan compile(const std::vector<float>& coords) const;

  private:
    struct Info {
        std::uint32_t tag = 0;
        unsigned seq = 0, max_value = 1, default_value = 0;
        std::uint32_t flags = 0;
        unsigned stage[2] = {0, 0};
    };
    struct Pause { unsigned index; PauseFn fn; };

    void add_lookups(Plan& m, int ti, std::uint32_t feature_index, long var_index, std::uint32_t mask,
                     bool auto_zwnj, bool auto_zwj, bool random, bool per_syllable,
                     std::uint32_t tag) const;

    const Layout& layout_;
    Direction dir_;
    int script_[2] = {-1, -1};
    std::size_t langsys_[2] = {npos, npos};
    std::uint32_t chosen_[2] = {0, 0};
    bool found_[2] = {false, false};
    std::vector<Info> infos_;
    std::vector<Pause> pauses_[2];
    unsigned stage_[2] = {0, 0};
};

inline void PlanBuilder::add_lookups(Plan& m, int ti, std::uint32_t feature_index, long var_index,
                                     std::uint32_t mask, bool auto_zwnj, bool auto_zwj, bool random,
                                     bool per_syllable, std::uint32_t tag) const {
    const Layout::Table& t = layout_.tab(ti);
    const Reader r = t.reader();
    const std::size_t f = detail::feature_table(t, feature_index, var_index);
    if (f == npos) return;
    const std::uint16_t n = r.u16_at(f + 2);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint16_t li = r.u16_at(f + 4 + i * 2);
        if (li >= t.lookup_count) continue;
        m.lookups[ti].push_back({li, mask, auto_zwnj, auto_zwj, random, per_syllable, tag});
    }
}

inline Plan PlanBuilder::compile(const std::vector<float>& coords) const {
    Plan m;
    m.direction = dir_;
    constexpr unsigned global_shift = 31;
    constexpr std::uint32_t global_bit = 1u << global_shift;
    m.global = global_bit;

    std::uint32_t required_index[2] = {0xFFFF, 0xFFFF}, required_tag[2] = {0, 0};
    unsigned required_stage[2] = {0, 0};
    long var_index[2] = {-1, -1};
    // tag -> feature index, per table, from the chosen LangSys (earliest wins)
    std::vector<std::pair<std::uint32_t, std::uint32_t>> available[2];
    for (int ti = 0; ti < 2; ++ti) {
        m.chosen_script[ti] = chosen_[ti];
        m.found_script[ti] = found_[ti];
        const Layout::Table& t = layout_.tab(ti);
        var_index[ti] = detail::variations_index(t, coords);
        const std::size_t ls = langsys_[ti];
        if (ls == npos) continue;
        const Reader r = t.reader();
        required_index[ti] = r.u16_at(ls + 2);
        if (required_index[ti] != 0xFFFF) required_tag[ti] = detail::feature_tag(t, required_index[ti]);
        const std::uint16_t n = r.u16_at(ls + 4);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t fi = r.u16_at(ls + 6 + i * 2);
            const std::uint32_t tag = detail::feature_tag(t, fi);
            bool seen = false;
            for (const auto& [tg, idx] : available[ti]) seen |= (tg == tag);
            if (!seen) available[ti].push_back({tag, fi});
        }
    }

    // Sort by (tag, seq) and merge duplicates, later global settings winning.
    std::vector<Info> f = infos_;
    std::sort(f.begin(), f.end(), [](const Info& a, const Info& b) {
        return a.tag != b.tag ? a.tag < b.tag : a.seq < b.seq;
    });
    if (!f.empty()) {
        std::size_t j = 0;
        for (std::size_t i = 1; i < f.size(); ++i) {
            if (f[i].tag != f[j].tag) { f[++j] = f[i]; continue; }
            if (f[i].flags & F_GLOBAL) {
                f[j].flags |= F_GLOBAL;
                f[j].max_value = f[i].max_value;
                f[j].default_value = f[i].default_value;
            } else {
                if (f[j].flags & F_GLOBAL) f[j].flags ^= F_GLOBAL;
                f[j].max_value = std::max(f[j].max_value, f[i].max_value);
            }
            f[j].flags |= (f[i].flags & F_HAS_FALLBACK);
            f[j].stage[0] = std::min(f[j].stage[0], f[i].stage[0]);
            f[j].stage[1] = std::min(f[j].stage[1], f[i].stage[1]);
        }
        f.resize(j + 1);
    }

    // Allocate mask bits. Bits 0-3 stay free like HarfBuzz's glyph flags, so
    // the same number of features fit.
    unsigned next_bit = 4;
    for (const Info& info : f) {
        const bool uses_global = (info.flags & F_GLOBAL) && info.max_value == 1;
        const unsigned bits = uses_global ? 0 : std::min(8u, static_cast<unsigned>(std::bit_width(info.max_value)));
        if (info.max_value == 0 || next_bit + bits >= global_shift) continue;
        bool found = false;
        std::uint32_t index[2] = {0xFFFF, 0xFFFF};
        for (int ti = 0; ti < 2; ++ti) {
            if (required_tag[ti] == info.tag) required_stage[ti] = info.stage[ti];
            for (const auto& [tg, idx] : available[ti])
                if (tg == info.tag) { index[ti] = idx; found = true; break; }
        }
        if (!found && (info.flags & F_GLOBAL_SEARCH)) {
            for (int ti = 0; ti < 2; ++ti) {
                const Layout::Table& t = layout_.tab(ti);
                const std::uint16_t n = t.reader().u16_at(t.feature_list);
                for (std::uint32_t k = 0; k < n; ++k)
                    if (detail::feature_tag(t, k) == info.tag) { index[ti] = k; found = true; break; }
            }
        }
        if (!found && !(info.flags & F_HAS_FALLBACK)) continue;
        FeatureMap fm;
        fm.tag = info.tag;
        fm.index[0] = index[0];
        fm.index[1] = index[1];
        fm.stage[0] = info.stage[0];
        fm.stage[1] = info.stage[1];
        fm.auto_zwnj = !(info.flags & F_MANUAL_ZWNJ);
        fm.auto_zwj = !(info.flags & F_MANUAL_ZWJ);
        fm.random = (info.flags & F_RANDOM) != 0;
        fm.per_syllable = (info.flags & F_PER_SYLLABLE) != 0;
        if (uses_global) {
            fm.shift = global_shift;
            fm.mask = global_bit;
        } else {
            fm.shift = next_bit;
            fm.mask = (1u << (next_bit + bits)) - (1u << next_bit);
            next_bit += bits;
            m.global |= (info.default_value << fm.shift) & fm.mask;
        }
        fm.one_mask = (1u << fm.shift) & fm.mask;
        fm.needs_fallback = !found;
        m.features.push_back(fm);
    }

    for (int ti = 0; ti < 2; ++ti) {
        // The implicit final pause closes the last stage.
        std::vector<Pause> pauses = pauses_[ti];
        pauses.push_back({stage_[ti], nullptr});
        auto& lookups = m.lookups[ti];
        std::size_t stage_index = 0, last = 0;
        for (unsigned stage = 0; stage <= stage_[ti]; ++stage) {
            if (required_index[ti] != 0xFFFF && required_stage[ti] == stage)
                add_lookups(m, ti, required_index[ti], var_index[ti], global_bit, true, true, false, false, required_tag[ti]);
            for (const FeatureMap& fm : m.features)
                if (fm.stage[ti] == stage && fm.index[ti] != 0xFFFF)
                    add_lookups(m, ti, fm.index[ti], var_index[ti], fm.mask, fm.auto_zwnj, fm.auto_zwj,
                                fm.random, fm.per_syllable, fm.tag);
            // Sort this stage's lookups by index and merge duplicates.
            if (last + 1 < lookups.size()) {
                std::stable_sort(lookups.begin() + static_cast<std::ptrdiff_t>(last), lookups.end(),
                                 [](const LookupMap& a, const LookupMap& b) { return a.index < b.index; });
                std::size_t j = last;
                for (std::size_t i = j + 1; i < lookups.size(); ++i) {
                    if (lookups[i].index != lookups[j].index) { lookups[++j] = lookups[i]; continue; }
                    lookups[j].mask |= lookups[i].mask;
                    lookups[j].auto_zwnj = lookups[j].auto_zwnj && lookups[i].auto_zwnj;
                    lookups[j].auto_zwj = lookups[j].auto_zwj && lookups[i].auto_zwj;
                }
                lookups.resize(j + 1);
            }
            last = lookups.size();
            if (stage_index < pauses.size() && pauses[stage_index].index == stage) {
                m.stages[ti].push_back({last, pauses[stage_index].fn});
                ++stage_index;
            }
        }
    }
    return m;
}

// ── applying lookups ───────────────────────────────────────────────────────────────

inline constexpr unsigned max_nesting = 64;          ///< HB_MAX_NESTING_LEVEL
inline constexpr std::size_t max_context_length = 64; ///< HB_MAX_CONTEXT_LENGTH

namespace detail {

enum class Skip : std::uint8_t { no, yes, maybe };
enum class Match : std::uint8_t { no, yes, maybe };
enum class Step : std::uint8_t { match, not_match, skip };

[[nodiscard]] inline bool intersects(const Digest& a, const Digest& b) noexcept {
    return (a.m[0] & b.m[0]) && (a.m[1] & b.m[1]) && (a.m[2] & b.m[2]);
}

struct ApplyContext;

/// HarfBuzz's skipping iterator: walks forward over `info` or backward over
/// the backtrack buffer, skipping what the lookup flags say to ignore, and
/// matching each remaining glyph against the next value of a pattern.
struct Skippy {
    enum class Kind : std::uint8_t { none, always, glyph, klass, cov };

    ApplyContext* c = nullptr;
    std::size_t idx = 0, end = 0;
    std::uint32_t lookup_props = 0, mask = ~0u;
    bool ignore_zwnj = false, ignore_zwj = false, ignore_hidden = false, per_syllable = false;
    std::uint8_t syllable = 0;
    Kind kind = Kind::none;
    std::size_t data = npos;   ///< position of the next u16 pattern value
    std::size_t aux = npos;    ///< ClassDef (klass) or offset base (cov)

    inline void init(ApplyContext& ctx, bool context_match);
    inline void reset(std::size_t start);
    inline void reset_fast(std::size_t start) noexcept;
    void set_match(Kind k, std::size_t data_pos = npos, std::size_t aux_off = npos) noexcept {
        kind = k; data = data_pos; aux = aux_off;
    }
    void set_lookup_props(std::uint32_t p) noexcept { lookup_props = p; }
    [[nodiscard]] inline Skip may_skip(const GlyphInfo& g) const;
    [[nodiscard]] inline Match may_match(const GlyphInfo& g) const;
    [[nodiscard]] Step match(const GlyphInfo& g) const {
        const Skip s = may_skip(g);
        if (s == Skip::yes) return Step::skip;
        const Match m = may_match(g);
        if (m == Match::yes || (m == Match::maybe && s == Skip::no)) return Step::match;
        if (s == Skip::no) return Step::not_match;
        return Step::skip;
    }
    inline bool next(std::size_t* unsafe_to = nullptr);
    inline bool prev(std::size_t* unsafe_from = nullptr);
};

struct ApplyContext {
    const Layout& layout;
    int ti;
    Reader r;
    const std::vector<LookupInfo>& lookups;
    Buffer& buf;
    const std::vector<float>& coords;
    Direction direction;
    bool has_glyph_classes;
    std::uint32_t lookup_mask = 1, lookup_props = 0;
    unsigned lookup_index = 0;
    bool auto_zwnj = true, auto_zwj = true, random = false, per_syllable = false;
    unsigned nesting_left = max_nesting;
    long last_base = -1;
    std::size_t last_base_until = 0;
    bool has_attachment = false;
    Skippy iter_input, iter_context;

    ApplyContext(const Layout& l, int table_index, Buffer& b, const std::vector<float>& cs)
        : layout(l), ti(table_index), r(l.tab(table_index).reader()), lookups(l.lookups(table_index)),
          buf(b), coords(cs), direction(b.direction), has_glyph_classes(l.has_glyph_classes()) {
        init_iters();
    }

    void init_iters() { iter_input.init(*this, false); iter_context.init(*this, true); }
    void set_lookup_props(std::uint32_t p) { lookup_props = p; init_iters(); }

    /// hb_ot_apply_context_t::check_glyph_property
    [[nodiscard]] bool check_glyph_property(const GlyphInfo& g, std::uint32_t match_props) const {
        const unsigned gp = g.glyph_props;
        if (gp & match_props & lookup_flag::ignore_flags) return false;
        if (gp & props::mark) {
            if (match_props & lookup_flag::use_mark_filtering_set)
                return layout.mark_set_covers(match_props >> 16, g.gid);
            if (match_props & lookup_flag::mark_attachment_type)
                return (match_props & lookup_flag::mark_attachment_type) == (gp & lookup_flag::mark_attachment_type);
        }
        return true;
    }

    void set_glyph_class(std::uint16_t g, std::uint16_t class_guess = 0, bool ligature = false,
                         bool component = false) {
        GlyphInfo& cur = buf.cur();
        std::uint16_t p = cur.glyph_props | props::substituted;
        if (ligature) { p |= props::ligated; p &= static_cast<std::uint16_t>(~props::multiplied); }
        if (component) p |= props::multiplied;
        if (has_glyph_classes) cur.glyph_props = static_cast<std::uint16_t>((p & props::preserve) | layout.glyph_props(g));
        else if (class_guess) cur.glyph_props = static_cast<std::uint16_t>((p & props::preserve) | class_guess);
        else cur.glyph_props = p;
    }
    void replace_glyph(std::uint16_t g) { set_glyph_class(g); buf.replace_glyph(g); }
    void replace_glyph_inplace(std::uint16_t g) { set_glyph_class(g); buf.cur().gid = g; }
    void replace_glyph_with_ligature(std::uint16_t g, std::uint16_t klass) {
        set_glyph_class(g, klass, true);
        buf.replace_glyph(g);
    }
    void output_glyph_for_component(std::uint16_t g, std::uint16_t klass) {
        set_glyph_class(g, klass, false, true);
        buf.output_glyph(g);
    }
    std::uint32_t random_number() noexcept {
        buf.random_state = buf.random_state * 48271u % 2147483647u;   // minstd_rand, u32 like HarfBuzz
        return buf.random_state;
    }

    /// Try every subtable of `l` at the current glyph; true on the first hit.
    inline bool apply_at(const LookupInfo& l);
    /// A nested lookup from a context rule.
    inline bool recurse(unsigned lookup);
};

inline void Skippy::init(ApplyContext& ctx, bool context_match) {
    c = &ctx;
    end = ctx.buf.len();
    kind = Kind::none;
    data = aux = npos;
    lookup_props = ctx.lookup_props;
    // ZWNJ is skipped in GPOS, and in GSUB context when the feature allows;
    // ZWJ when matching context or when the feature allows.
    ignore_zwnj = ctx.ti == table::gpos || (context_match && ctx.auto_zwnj);
    ignore_zwj = context_match || ctx.auto_zwj;
    ignore_hidden = ctx.ti == table::gpos;
    mask = context_match ? ~0u : ctx.lookup_mask;
    per_syllable = ctx.ti == table::gsub && ctx.per_syllable;
    syllable = 0;
}

inline void Skippy::reset_fast(std::size_t start) noexcept {
    idx = start;
    end = c->buf.len();
}

inline void Skippy::reset(std::size_t start) {
    idx = start;
    end = c->buf.len();
    syllable = (start == c->buf.idx && c->buf.idx < c->buf.len()) ? c->buf.cur().syllable : 0;
}

inline Skip Skippy::may_skip(const GlyphInfo& g) const {
    if (!c->check_glyph_property(g, lookup_props)) return Skip::yes;
    if (is_default_ignorable(g) &&
        (ignore_zwnj || !(g.uflags & uflag::zwnj)) &&
        (ignore_zwj || !(g.uflags & uflag::zwj)) &&
        (ignore_hidden || !(g.uflags & uflag::hidden)))
        return Skip::maybe;
    return Skip::no;
}

inline Match Skippy::may_match(const GlyphInfo& g) const {
    if (!(g.mask & mask) || (per_syllable && syllable && syllable != g.syllable)) return Match::no;
    const Reader& r = c->r;
    switch (kind) {
        case Kind::none: return Match::maybe;
        case Kind::always: return Match::yes;
        case Kind::glyph: return g.gid == r.u16_at(data) ? Match::yes : Match::no;
        case Kind::klass: return class_of(r, aux, g.gid) == r.u16_at(data) ? Match::yes : Match::no;
        case Kind::cov: return coverage(r, off16(r, aux, data), g.gid) >= 0 ? Match::yes : Match::no;
    }
    return Match::no;
}

inline bool Skippy::next(std::size_t* unsafe_to) {
    const auto& info = c->buf.info;
    while (idx + 1 < end) {
        ++idx;
        switch (match(info[idx])) {
            case Step::match: if (data != npos) data += 2; return true;
            case Step::not_match: if (unsafe_to) *unsafe_to = idx + 1; return false;
            case Step::skip: continue;
        }
    }
    if (unsafe_to) *unsafe_to = end;
    return false;
}

inline bool Skippy::prev(std::size_t* unsafe_from) {
    while (idx > 0) {
        --idx;
        switch (match(c->buf.out_info(idx))) {
            case Step::match: if (data != npos) data += 2; return true;
            case Step::not_match: if (unsafe_from) *unsafe_from = (idx > 1 ? idx : 1) - 1; return false;
            case Step::skip: continue;
        }
    }
    if (unsafe_from) *unsafe_from = 0;
    return false;
}

// ── matching sequences (hb-ot-layout-gsubgpos.hh) ─────────────────────────────────

using Positions = std::array<std::size_t, max_context_length>;

/// Match `count - 1` input glyphs after the current one. `pattern` is the
/// position of the first u16 value (glyph id, class, or coverage offset).
/// Ligature-id rules: a sequence may not ligate glyphs attached to
/// different components of an earlier ligature (LAM SHADDA LAM FATHA HEH).
inline bool match_input(ApplyContext& c, std::size_t count, Skippy::Kind kind, std::size_t pattern,
                        std::size_t aux, std::size_t& end_position, Positions& pos,
                        unsigned* total_components = nullptr) {
    if (count > max_context_length) return false;
    Buffer& b = c.buf;
    Skippy& it = c.iter_input;
    it.reset(b.idx);
    it.set_match(kind, pattern, aux);

    unsigned total = 0;
    const unsigned first_id = lig::id(b.cur()), first_comp = lig::comp(b.cur());
    enum { not_checked, may_not_skip, may_skip } ligbase = not_checked;

    for (std::size_t i = 1; i < count; ++i) {
        std::size_t unsafe_to = 0;
        if (!it.next(&unsafe_to)) { end_position = unsafe_to; return false; }
        pos[i] = it.idx;
        const GlyphInfo& g = b.info[it.idx];
        const unsigned this_id = lig::id(g), this_comp = lig::comp(g);
        if (first_id && first_comp) {
            if (first_id != this_id || first_comp != this_comp) {
                if (ligbase == not_checked) {
                    bool found = false;
                    std::size_t j = b.out_len();
                    while (j && lig::id(b.out_info(j - 1)) == first_id) {
                        if (lig::comp(b.out_info(j - 1)) == 0) { --j; found = true; break; }
                        --j;
                    }
                    ligbase = (found && it.may_skip(b.out_info(j)) == Skip::yes) ? may_skip : may_not_skip;
                }
                if (ligbase == may_not_skip) return false;
            }
        } else if (this_id && this_comp && this_id != first_id) {
            return false;
        }
        total += lig::num_comps(g);
    }
    end_position = it.idx + 1;
    if (total_components) *total_components = total + lig::num_comps(b.cur());
    pos[0] = b.idx;
    return true;
}

/// Form ligature `lig_glyph` from the matched glyphs, re-homing the marks
/// between (and after) them onto the new ligature's components.
inline void ligate_input(ApplyContext& c, std::size_t count, const Positions& pos,
                         std::size_t match_end, std::uint16_t lig_glyph, unsigned total_components) {
    Buffer& b = c.buf;
    b.merge_clusters(b.idx, match_end);

    // A base + marks ligating stays a base (marks after it can still attach);
    // marks-only stays a mark ligature and keeps its old ligature id.
    bool is_base_lig = is_base_glyph(b.info[pos[0]]);
    bool is_mark_lig = is_mark(b.info[pos[0]]);
    for (std::size_t i = 1; i < count; ++i)
        if (!is_mark(b.info[pos[i]])) { is_base_lig = is_mark_lig = false; break; }
    const bool is_lig = !is_base_lig && !is_mark_lig;

    const std::uint16_t klass = is_lig ? props::ligature : 0;
    const unsigned lig_id = is_lig ? b.allocate_lig_id() : 0;
    unsigned last_lig_id = lig::id(b.cur());
    unsigned last_num_comps = lig::num_comps(b.cur());
    unsigned comps_so_far = last_num_comps;

    if (is_lig) {
        lig::set_for_ligature(b.cur(), lig_id, total_components);
        if (b.cur().gc == static_cast<std::uint8_t>(ucd::GeneralCategory::Mn))
            b.cur().gc = static_cast<std::uint8_t>(ucd::GeneralCategory::Lo);
    }
    c.replace_glyph_with_ligature(lig_glyph, klass);

    for (std::size_t i = 1; i < count; ++i) {
        while (b.idx < pos[i] && b.successful) {
            if (is_lig) {
                unsigned this_comp = lig::comp(b.cur());
                if (this_comp == 0) this_comp = last_num_comps;
                const unsigned nc = comps_so_far - last_num_comps + std::min(this_comp, last_num_comps);
                lig::set_for_mark(b.cur(), lig_id, nc);
            }
            b.next_glyph();
        }
        last_lig_id = lig::id(b.cur());
        last_num_comps = lig::num_comps(b.cur());
        comps_so_far += last_num_comps;
        b.skip_glyph();   // the component itself is consumed
    }

    if (!is_mark_lig && last_lig_id) {
        // Marks FOLLOWING the last component that belonged to it move too.
        for (std::size_t i = b.idx; i < b.len(); ++i) {
            if (last_lig_id != lig::id(b.info[i])) break;
            const unsigned this_comp = lig::comp(b.info[i]);
            if (!this_comp) break;
            const unsigned nc = comps_so_far - last_num_comps + std::min(this_comp, last_num_comps);
            lig::set_for_mark(b.info[i], lig_id, nc);
        }
    }
}

inline bool match_backtrack(ApplyContext& c, std::size_t count, Skippy::Kind kind,
                            std::size_t pattern, std::size_t aux, std::size_t& start) {
    Skippy& it = c.iter_context;
    it.reset(c.buf.backtrack_len());
    it.set_match(kind, pattern, aux);
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t unsafe_from = 0;
        if (!it.prev(&unsafe_from)) { start = unsafe_from; return false; }
    }
    start = it.idx;
    return true;
}

inline bool match_lookahead(ApplyContext& c, std::size_t count, Skippy::Kind kind,
                            std::size_t pattern, std::size_t aux, std::size_t start,
                            std::size_t& end) {
    Skippy& it = c.iter_context;
    it.reset(start - 1);
    it.set_match(kind, pattern, aux);
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t unsafe_to = 0;
        if (!it.next(&unsafe_to)) { end = unsafe_to; return false; }
    }
    end = it.idx + 1;
    return true;
}

/// Run a rule's nested lookups (SequenceLookupRecords at `records`) over
/// the matched positions, tracking how each one grows or shrinks the
/// buffer so later records still land on the right glyphs.
inline void apply_lookup(ApplyContext& c, std::size_t count, Positions& pos, std::size_t n_records,
                         std::size_t records, std::size_t match_end) {
    Buffer& b = c.buf;
    long end;
    {
        const std::size_t bl = b.backtrack_len();
        end = static_cast<long>(bl + match_end) - static_cast<long>(b.idx);
        const long delta = static_cast<long>(bl) - static_cast<long>(b.idx);
        for (std::size_t j = 0; j < count; ++j) pos[j] = static_cast<std::size_t>(static_cast<long>(pos[j]) + delta);
    }
    for (std::size_t i = 0; i < n_records && b.successful; ++i) {
        const std::size_t idx = c.r.u16_at(records + i * 4);
        const unsigned lookup = c.r.u16_at(records + i * 4 + 2);
        if (idx >= count) continue;
        const std::size_t orig_len = b.backtrack_len() + b.lookahead_len();
        if (pos[idx] >= orig_len) continue;
        if (!b.move_to(pos[idx])) break;
        if (b.max_ops <= 0) break;
        if (!c.recurse(lookup)) continue;
        const std::size_t new_len = b.backtrack_len() + b.lookahead_len();
        long delta = static_cast<long>(new_len) - static_cast<long>(orig_len);
        if (!delta) continue;
        end += delta;
        if (end < static_cast<long>(pos[idx])) {
            delta += static_cast<long>(pos[idx]) - end;
            end = static_cast<long>(pos[idx]);
        }
        std::size_t next = idx + 1;
        if (delta > 0) {
            if (static_cast<std::size_t>(delta) + count > max_context_length) break;
        } else {
            delta = std::max(delta, static_cast<long>(next) - static_cast<long>(count));
            next = static_cast<std::size_t>(static_cast<long>(next) - delta);
        }
        const std::size_t dst = static_cast<std::size_t>(static_cast<long>(next) + delta);
        if (dst > next)   // overlapping move, like memmove
            std::copy_backward(pos.begin() + static_cast<std::ptrdiff_t>(next), pos.begin() + static_cast<std::ptrdiff_t>(count),
                               pos.begin() + static_cast<std::ptrdiff_t>(dst + (count - next)));
        else if (dst < next)
            std::copy(pos.begin() + static_cast<std::ptrdiff_t>(next), pos.begin() + static_cast<std::ptrdiff_t>(count),
                      pos.begin() + static_cast<std::ptrdiff_t>(dst));
        next = dst;
        count = static_cast<std::size_t>(static_cast<long>(count) + delta);
        for (std::size_t j = idx + 1; j < next; ++j) pos[j] = pos[j - 1] + 1;
        for (; next < count; ++next) pos[next] = static_cast<std::size_t>(static_cast<long>(pos[next]) + delta);
    }
    (void)b.move_to(static_cast<std::size_t>(std::max(end, 0L)));
}

// ── (chain) context, formats 1-3: identical for GSUB and GPOS ───────────────────────

/// One (Class)SequenceRule at `rule`: glyphCount, seqLookupCount, input[], records[].
inline bool context_rule(ApplyContext& c, std::size_t rule, Skippy::Kind kind, std::size_t aux) {
    const Reader& r = c.r;
    const std::size_t count = r.u16_at(rule), n_records = r.u16_at(rule + 2);
    if (count == 0 || count > max_context_length) return false;
    Positions pos{};
    std::size_t end = 0;
    if (!match_input(c, count, kind, rule + 4, aux, end, pos)) return false;
    apply_lookup(c, count, pos, n_records, rule + 4 + (count - 1) * 2, end);
    return true;
}

/// One ChainRule at `rule`.
inline bool chain_rule(ApplyContext& c, std::size_t rule, Skippy::Kind kind, std::size_t bt_aux,
                       std::size_t in_aux, std::size_t la_aux) {
    const Reader& r = c.r;
    const std::size_t bt = r.u16_at(rule);
    const std::size_t in_at = rule + 2 + bt * 2;
    const std::size_t count = r.u16_at(in_at);
    const std::size_t la_at = in_at + 2 + (count ? count - 1 : 0) * 2;
    const std::size_t la = r.u16_at(la_at);
    const std::size_t rec_at = la_at + 2 + la * 2;
    const std::size_t n_records = r.u16_at(rec_at);
    if (count == 0 || count > max_context_length) return false;
    Positions pos{};
    std::size_t match_end = 0, end_index = c.buf.idx, start_index = c.buf.out_len();
    if (!match_input(c, count, kind, in_at + 2, in_aux, match_end, pos)) return false;
    end_index = match_end;
    if (!match_lookahead(c, la, kind, la_at + 2, la_aux, match_end, end_index)) return false;
    if (!match_backtrack(c, bt, kind, rule + 2, bt_aux, start_index)) return false;
    apply_lookup(c, count, pos, n_records, rec_at + 2, match_end);
    return true;
}

inline bool apply_context(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    const std::uint16_t g = c.buf.cur().gid;
    switch (r.u16_at(s)) {
        case 1: case 2: {
            const int ci = coverage(r, off16(r, s, s + 2), g);
            if (ci < 0) return false;
            const bool by_class = r.u16_at(s) == 2;
            const std::size_t cd = by_class ? off16(r, s, s + 4) : npos;
            const std::size_t list = by_class ? s + 6 : s + 4;
            const unsigned index = by_class ? class_of(r, cd, g) : static_cast<unsigned>(ci);
            if (index >= r.u16_at(list)) return false;
            const std::size_t set = off16(r, s, list + 2 + std::size_t{index} * 2);
            if (set == npos) return false;
            const std::uint16_t n = r.u16_at(set);
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t rule = off16(r, set, set + 2 + i * 2);
                if (rule != npos && context_rule(c, rule, by_class ? Skippy::Kind::klass : Skippy::Kind::glyph, cd))
                    return true;
            }
            return false;
        }
        case 3: {
            const std::size_t count = r.u16_at(s + 2), n_records = r.u16_at(s + 4);
            if (count == 0 || count > max_context_length) return false;
            if (coverage(r, off16(r, s, s + 6), g) < 0) return false;
            Positions pos{};
            std::size_t end = 0;
            if (!match_input(c, count, Skippy::Kind::cov, s + 8, s, end, pos)) return false;
            apply_lookup(c, count, pos, n_records, s + 6 + count * 2, end);
            return true;
        }
        default: return false;
    }
}

inline bool apply_chain_context(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    const std::uint16_t g = c.buf.cur().gid;
    switch (r.u16_at(s)) {
        case 1: case 2: {
            const int ci = coverage(r, off16(r, s, s + 2), g);
            if (ci < 0) return false;
            const bool by_class = r.u16_at(s) == 2;
            std::size_t bt = npos, in = npos, la = npos, list = s + 4;
            unsigned index = static_cast<unsigned>(ci);
            if (by_class) {
                bt = off16(r, s, s + 4);
                in = off16(r, s, s + 6);
                la = off16(r, s, s + 8);
                list = s + 10;
                index = class_of(r, in, g);
            }
            if (index >= r.u16_at(list)) return false;
            const std::size_t set = off16(r, s, list + 2 + std::size_t{index} * 2);
            if (set == npos) return false;
            const std::uint16_t n = r.u16_at(set);
            const auto kind = by_class ? Skippy::Kind::klass : Skippy::Kind::glyph;
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t rule = off16(r, set, set + 2 + i * 2);
                if (rule != npos && chain_rule(c, rule, kind, bt, in, la)) return true;
            }
            return false;
        }
        case 3: {
            const std::size_t bt = r.u16_at(s + 2);
            const std::size_t in_at = s + 4 + bt * 2;
            const std::size_t count = r.u16_at(in_at);
            const std::size_t la_at = in_at + 2 + count * 2;
            const std::size_t la = r.u16_at(la_at);
            const std::size_t rec_at = la_at + 2 + la * 2;
            if (count == 0 || count > max_context_length) return false;
            if (coverage(r, off16(r, s, in_at + 2), g) < 0) return false;
            Positions pos{};
            std::size_t match_end = 0, end_index = c.buf.idx, start_index = c.buf.out_len();
            if (!match_input(c, count, Skippy::Kind::cov, in_at + 4, s, match_end, pos)) return false;
            end_index = match_end;
            if (!match_lookahead(c, la, Skippy::Kind::cov, la_at + 2, s, match_end, end_index)) return false;
            if (!match_backtrack(c, bt, Skippy::Kind::cov, s + 4, s, start_index)) return false;
            apply_lookup(c, count, pos, r.u16_at(rec_at), rec_at + 2, match_end);
            return true;
        }
        default: return false;
    }
}

// ── GSUB ──────────────────────────────────────────────────────────────────────────────

inline bool gsub_single(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    const std::uint16_t g = c.buf.cur().gid;
    const int ci = coverage(r, off16(r, s, s + 2), g);
    if (ci < 0) return false;
    if (r.u16_at(s) == 1) {
        c.replace_glyph(static_cast<std::uint16_t>(g + r.i16_at(s + 4)));
        return true;
    }
    if (r.u16_at(s) == 2) {
        if (static_cast<unsigned>(ci) >= r.u16_at(s + 4)) return false;
        c.replace_glyph(r.u16_at(s + 6 + std::size_t(ci) * 2));
        return true;
    }
    return false;
}

inline bool gsub_multiple(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    const int ci = coverage(r, off16(r, s, s + 2), b.cur().gid);
    if (ci < 0 || r.u16_at(s) != 1 || static_cast<unsigned>(ci) >= r.u16_at(s + 4)) return false;
    const std::size_t seq = off16(r, s, s + 6 + std::size_t(ci) * 2);
    const std::uint16_t n = r.u16_at(seq);
    if (n == 1) { c.replace_glyph(r.u16_at(seq + 2)); return true; }
    if (n == 0) { b.delete_glyph(); return true; }   // spec says no; Uniscribe allows it
    const std::uint16_t klass = is_ligature(b.cur()) ? props::base_glyph : 0;
    const unsigned lig_id = lig::id(b.cur());
    for (std::size_t i = 0; i < n; ++i) {
        if (!lig_id) lig::set_for_component(b.cur(), static_cast<unsigned>(i));
        c.output_glyph_for_component(r.u16_at(seq + 2 + i * 2), klass);
    }
    b.skip_glyph();
    return true;
}

inline bool gsub_alternate(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    const int ci = coverage(r, off16(r, s, s + 2), b.cur().gid);
    if (ci < 0 || r.u16_at(s) != 1 || static_cast<unsigned>(ci) >= r.u16_at(s + 4)) return false;
    const std::size_t set = off16(r, s, s + 6 + std::size_t(ci) * 2);
    const std::uint16_t n = r.u16_at(set);
    if (!n) return false;
    const std::uint32_t lm = c.lookup_mask;
    const unsigned shift = static_cast<unsigned>(std::countr_zero(lm));
    unsigned alt = (lm & b.cur().mask) >> shift;
    if (alt == max_feature_value && c.random) alt = c.random_number() % n + 1;
    if (alt > n || alt == 0) return false;
    c.replace_glyph(r.u16_at(set + 2 + std::size_t(alt - 1) * 2));
    return true;
}

inline bool gsub_ligature(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    const int ci = coverage(r, off16(r, s, s + 2), b.cur().gid);
    if (ci < 0 || r.u16_at(s) != 1 || static_cast<unsigned>(ci) >= r.u16_at(s + 4)) return false;
    const std::size_t set = off16(r, s, s + 6 + std::size_t(ci) * 2);
    const std::uint16_t n = r.u16_at(set);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t lg = off16(r, set, set + 2 + i * 2);
        const std::uint16_t glyph = r.u16_at(lg), count = r.u16_at(lg + 2);
        if (count == 0) continue;
        if (count == 1) { c.replace_glyph(glyph); return true; }   // in place, not "ligated"
        if (count > max_context_length) continue;
        Positions pos{};
        std::size_t end = 0;
        unsigned total = 0;
        if (!match_input(c, count, Skippy::Kind::glyph, lg + 4, npos, end, pos, &total)) continue;
        ligate_input(c, count, pos, end, glyph, total);
        return true;
    }
    return false;
}

inline bool gsub_reverse_chain(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    const int ci = coverage(r, off16(r, s, s + 2), b.cur().gid);
    if (ci < 0 || r.u16_at(s) != 1) return false;
    if (c.nesting_left != max_nesting) return false;   // never through a context lookup
    const std::size_t bt = r.u16_at(s + 4);
    const std::size_t la_at = s + 6 + bt * 2;
    const std::size_t la = r.u16_at(la_at);
    const std::size_t sub_at = la_at + 2 + la * 2;
    if (static_cast<unsigned>(ci) >= r.u16_at(sub_at)) return false;
    std::size_t start = 0, end = 0;
    if (match_backtrack(c, bt, Skippy::Kind::cov, s + 6, s, start) &&
        match_lookahead(c, la, Skippy::Kind::cov, la_at + 2, s, b.idx + 1, end)) {
        c.replace_glyph_inplace(r.u16_at(sub_at + 2 + std::size_t(ci) * 2));
        return true;
    }
    return false;
}

// ── GPOS ──────────────────────────────────────────────────────────────────────────────

inline constexpr std::uint8_t attach_mark = 1, attach_cursive = 2;

/// A Device/VariationIndex delta in font units (rounded like HarfBuzz's
/// em_scalef). Hinting device tables need a ppem and contribute nothing.
[[nodiscard]] inline float device_delta(const ApplyContext& c, std::size_t dev) {
    if (dev == npos || c.coords.empty() || c.r.u16_at(dev + 4) != 0x8000) return 0;
    return c.layout.var_delta(c.r.u16_at(dev), c.r.u16_at(dev + 2), c.coords);
}

[[nodiscard]] inline std::size_t value_size(std::uint16_t vf) noexcept {
    return 2u * static_cast<unsigned>(std::popcount(static_cast<unsigned>(vf & 0xFF)));
}

/// Apply a ValueRecord at `v` (device offsets relative to `base`).
inline void apply_value(const ApplyContext& c, std::uint16_t vf, std::size_t base, std::size_t v,
                        GlyphPos& p) {
    const Reader& r = c.r;
    if (!vf) return;
    if (vf & 0x0001) { p.x_offset += r.i16_at(v); v += 2; }
    if (vf & 0x0002) { p.y_offset += r.i16_at(v); v += 2; }
    if (vf & 0x0004) { p.x_advance += r.i16_at(v); v += 2; }   // horizontal text only
    if (vf & 0x0008) v += 2;                                   // YAdvance: vertical text
    if (!(vf & 0x00F0) || c.coords.empty()) return;
    if (vf & 0x0010) { p.x_offset += device_delta(c, off16(r, base, v)); v += 2; }
    if (vf & 0x0020) { p.y_offset += device_delta(c, off16(r, base, v)); v += 2; }
    if (vf & 0x0040) { p.x_advance += device_delta(c, off16(r, base, v)); v += 2; }
}

/// An Anchor's point in font units (formats 1-3; format 2's contour point
/// only matters when hinting at a ppem, so it reads as format 1).
inline void anchor(const ApplyContext& c, std::size_t a, float& x, float& y) {
    const Reader& r = c.r;
    x = r.i16_at(a + 2);
    y = r.i16_at(a + 4);
    if (r.u16_at(a) == 3 && !c.coords.empty()) {
        x += device_delta(c, off16(r, a, a + 6));
        y += device_delta(c, off16(r, a, a + 8));
    }
}

inline bool gpos_single(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    const int ci = coverage(r, off16(r, s, s + 2), b.cur().gid);
    if (ci < 0) return false;
    const std::uint16_t vf = r.u16_at(s + 4);
    if (r.u16_at(s) == 1) apply_value(c, vf, s, s + 6, b.pos[b.idx]);
    else if (r.u16_at(s) == 2) {
        if (static_cast<unsigned>(ci) >= r.u16_at(s + 6)) return false;
        apply_value(c, vf, s, s + 8 + std::size_t(ci) * value_size(vf), b.pos[b.idx]);
    } else return false;
    ++b.idx;
    return true;
}

inline bool gpos_pair(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    const int ci = coverage(r, off16(r, s, s + 2), b.cur().gid);
    if (ci < 0) return false;
    Skippy& it = c.iter_input;
    it.reset_fast(b.idx);
    it.set_match(Skippy::Kind::none);
    if (!it.next()) return false;
    const std::size_t second = it.idx;
    const std::uint16_t vf1 = r.u16_at(s + 4), vf2 = r.u16_at(s + 6);
    const std::size_t len1 = value_size(vf1), len2 = value_size(vf2);
    std::size_t rec = npos, base = s;
    if (r.u16_at(s) == 1) {
        if (static_cast<unsigned>(ci) >= r.u16_at(s + 8)) return false;
        const std::size_t set = off16(r, s, s + 10 + std::size_t(ci) * 2);
        if (set == npos) return false;
        base = set;
        const std::size_t stride = 2 + len1 + len2;
        std::size_t lo = 0, hi = r.u16_at(set);
        const std::uint16_t g2 = b.info[second].gid;
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            const std::size_t at = set + 2 + mid * stride;
            const std::uint16_t g = r.u16_at(at);
            if (g == g2) { rec = at + 2; break; }
            if (g < g2) lo = mid + 1; else hi = mid;
        }
        if (rec == npos) return false;
    } else if (r.u16_at(s) == 2) {
        const unsigned c1 = class_of(r, off16(r, s, s + 8), b.cur().gid);
        const unsigned c2 = class_of(r, off16(r, s, s + 10), b.info[second].gid);
        const unsigned n1 = r.u16_at(s + 12), n2 = r.u16_at(s + 14);
        if (c1 >= n1 || c2 >= n2) return false;
        rec = s + 16 + (std::size_t(c1) * n2 + c2) * (len1 + len2);
    } else return false;
    apply_value(c, vf1, base, rec, b.pos[b.idx]);
    apply_value(c, vf2, base, rec + len1, b.pos[second]);
    b.idx = len2 ? second + 1 : second;
    return true;
}

/// Detach a cursive chain so `new_parent` can become its root (hb
/// reverse_cursive_minor_offset).
inline void reverse_cursive_minor_offset(std::vector<GlyphPos>& pos, std::size_t i, std::size_t new_parent) {
    const int chain = pos[i].attach_chain;
    const unsigned type = pos[i].attach_type;
    if (!chain || !(type & attach_cursive)) return;
    pos[i].attach_chain = 0;
    const long j = static_cast<long>(i) + chain;
    if (j < 0 || static_cast<std::size_t>(j) == new_parent || static_cast<std::size_t>(j) >= pos.size()) return;
    reverse_cursive_minor_offset(pos, static_cast<std::size_t>(j), new_parent);
    pos[static_cast<std::size_t>(j)].y_offset = -pos[i].y_offset;
    pos[static_cast<std::size_t>(j)].attach_chain = static_cast<std::int16_t>(-chain);
    pos[static_cast<std::size_t>(j)].attach_type = static_cast<std::uint8_t>(type);
}

inline bool gpos_cursive(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    if (r.u16_at(s) != 1) return false;
    const std::size_t cov = off16(r, s, s + 2);
    const int ci = coverage(r, cov, b.cur().gid);
    if (ci < 0 || static_cast<unsigned>(ci) >= r.u16_at(s + 4)) return false;
    const std::size_t entry = off16(r, s, s + 6 + std::size_t(ci) * 4);
    if (entry == npos) return false;
    Skippy& it = c.iter_input;
    it.reset_fast(b.idx);
    it.set_match(Skippy::Kind::none);
    if (!it.prev()) return false;
    const int pi = coverage(r, cov, b.info[it.idx].gid);
    if (pi < 0 || static_cast<unsigned>(pi) >= r.u16_at(s + 4)) return false;
    const std::size_t exit = off16(r, s, s + 6 + std::size_t(pi) * 4 + 2);
    if (exit == npos) return false;

    const std::size_t i = it.idx, j = b.idx;
    auto& pos = b.pos;
    float exit_x, exit_y, entry_x, entry_y;
    anchor(c, exit, exit_x, exit_y);
    anchor(c, entry, entry_x, entry_y);
    if (c.direction == Direction::ltr) {
        pos[i].x_advance = exit_x + pos[i].x_offset;
        const float d = entry_x + pos[j].x_offset;
        pos[j].x_advance -= d;
        pos[j].x_offset -= d;
    } else {
        const float d = exit_x + pos[i].x_offset;
        pos[i].x_advance -= d;
        pos[i].x_offset -= d;
        pos[j].x_advance = entry_x + pos[j].x_offset;
    }
    std::size_t child = i, parent = j;
    float y_offset = entry_y - exit_y;
    if (!(c.lookup_props & lookup_flag::right_to_left)) {
        std::swap(child, parent);
        y_offset = -y_offset;
    }
    reverse_cursive_minor_offset(pos, child, parent);
    pos[child].attach_type = attach_cursive;
    pos[child].attach_chain = static_cast<std::int16_t>(static_cast<long>(parent) - static_cast<long>(child));
    c.has_attachment = true;
    pos[child].y_offset = y_offset;
    if (pos[parent].attach_chain == -pos[child].attach_chain) {
        pos[parent].attach_chain = 0;
        pos[parent].y_offset = 0;
    }
    ++b.idx;
    return true;
}

/// MarkArray.hh resolve_cross_offset (HarfBuzz 15): the glyph's cross-axis
/// (y, horizontal text) offset plus every cursive ancestor's. A mark takes
/// its base's at attach time, so a later lookup moving the base vertically
/// does not drag the mark along.
inline float resolve_cross_offset(const std::vector<GlyphPos>& pos, std::size_t g, bool reversed = false) {
    const std::size_t len = pos.size();
    float offset = pos[g].y_offset;
    for (std::size_t count = 1; count < len && (pos[g].attach_type & attach_cursive); ++count) {
        const int chain = pos[g].attach_chain;
        if (!chain) break;
        const long parent = static_cast<long>(g) + (reversed ? -chain : chain);
        if (parent < 0 || static_cast<std::size_t>(parent) >= len) break;
        g = static_cast<std::size_t>(parent);
        offset += pos[g].y_offset;
    }
    return offset;
}

/// MarkArray::apply: attach the current mark (index `mark_index` in the
/// MarkArray at `marks`) to the glyph at `glyph_pos`, whose anchors are row
/// `row` of the AnchorMatrix at `matrix` (`class_count` columns).
inline bool attach_mark_to(ApplyContext& c, std::size_t marks, unsigned mark_index, std::size_t matrix,
                           unsigned row, unsigned class_count, std::size_t glyph_pos) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    if (mark_index >= r.u16_at(marks)) return false;
    const std::size_t rec = marks + 2 + std::size_t(mark_index) * 4;
    const unsigned mark_class = r.u16_at(rec);
    const std::size_t mark_anchor = off16(r, marks, rec + 2);
    if (row >= r.u16_at(matrix) || mark_class >= class_count) return false;
    const std::size_t glyph_anchor = off16(r, matrix, matrix + 2 + (std::size_t(row) * class_count + mark_class) * 2);
    if (glyph_anchor == npos) return false;   // let the next subtable try
    float mx = 0, my = 0, gx, gy;
    if (mark_anchor != npos) anchor(c, mark_anchor, mx, my);   // a Null anchor is (0, 0)
    anchor(c, glyph_anchor, gx, gy);
    GlyphPos& o = b.pos[b.idx];
    const float base_offset = resolve_cross_offset(b.pos, glyph_pos);
    o.x_offset = gx - mx;
    o.y_offset = gy - my + base_offset;
    o.attach_type = attach_mark;
    o.attach_chain = static_cast<std::int16_t>(static_cast<long>(glyph_pos) - static_cast<long>(b.idx));
    c.has_attachment = true;
    ++b.idx;
    return true;
}

/// The preceding non-mark glyph for mark attachment, with HarfBuzz's
/// incremental cache (no O(n^2) on long mark runs).
inline long find_mark_base(ApplyContext& c, std::size_t base_cov, bool check_multiplied) {
    Buffer& b = c.buf;
    Skippy& it = c.iter_input;
    it.set_lookup_props(lookup_flag::ignore_marks);
    it.set_match(Skippy::Kind::none);
    if (c.last_base_until > b.idx) { c.last_base_until = 0; c.last_base = -1; }
    for (std::size_t j = b.idx; j > c.last_base_until; --j) {
        Step m = it.match(b.info[j - 1]);
        if (m == Step::match && check_multiplied) {
            // Attach only to the first glyph of a MultipleSubst sequence
            // (harfbuzz#740), unless a mark sits inside it (#1020).
            const auto& info = b.info;
            const std::size_t k = j - 1;
            const bool accept = !is_multiplied(info[k]) || lig::comp(info[k]) == 0 || k == 0 ||
                                is_mark(info[k - 1]) || !is_multiplied(info[k - 1]) ||
                                lig::id(info[k]) != lig::id(info[k - 1]) ||
                                lig::comp(info[k]) != lig::comp(info[k - 1]) + 1;
            if (!accept && coverage(c.r, base_cov, info[k].gid) < 0) m = Step::skip;
        }
        if (m == Step::match) { c.last_base = static_cast<long>(j - 1); break; }
    }
    c.last_base_until = b.idx;
    return c.last_base;
}

inline bool gpos_mark_base(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    if (r.u16_at(s) != 1) return false;
    const int mi = coverage(r, off16(r, s, s + 2), b.cur().gid);
    if (mi < 0) return false;
    const std::size_t base_cov = off16(r, s, s + 4);
    const long base = find_mark_base(c, base_cov, true);
    if (base < 0) return false;
    const int bi = coverage(r, base_cov, b.info[static_cast<std::size_t>(base)].gid);
    if (bi < 0) return false;
    return attach_mark_to(c, off16(r, s, s + 8), static_cast<unsigned>(mi), off16(r, s, s + 10),
                          static_cast<unsigned>(bi), r.u16_at(s + 6), static_cast<std::size_t>(base));
}

inline bool gpos_mark_lig(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    if (r.u16_at(s) != 1) return false;
    const int mi = coverage(r, off16(r, s, s + 2), b.cur().gid);
    if (mi < 0) return false;
    const long base = find_mark_base(c, npos, false);
    if (base < 0) return false;
    const std::size_t idx = static_cast<std::size_t>(base);
    const int li = coverage(r, off16(r, s, s + 4), b.info[idx].gid);
    if (li < 0) return false;
    const std::size_t lig_array = off16(r, s, s + 10);
    if (static_cast<unsigned>(li) >= r.u16_at(lig_array)) return false;
    const std::size_t attach = off16(r, lig_array, lig_array + 2 + std::size_t(li) * 2);
    const unsigned comp_count = r.u16_at(attach);
    if (!comp_count) return false;
    const unsigned lig_id = lig::id(b.info[idx]), mark_id = lig::id(b.cur()), mark_comp = lig::comp(b.cur());
    const unsigned comp = (lig_id && lig_id == mark_id && mark_comp > 0)
                              ? std::min(comp_count, mark_comp) - 1 : comp_count - 1;
    return attach_mark_to(c, off16(r, s, s + 8), static_cast<unsigned>(mi), attach, comp, r.u16_at(s + 6), idx);
}

inline bool gpos_mark_mark(ApplyContext& c, std::size_t s) {
    const Reader& r = c.r;
    Buffer& b = c.buf;
    if (r.u16_at(s) != 1) return false;
    const int m1 = coverage(r, off16(r, s, s + 2), b.cur().gid);
    if (m1 < 0) return false;
    Skippy& it = c.iter_input;
    it.reset_fast(b.idx);
    it.set_lookup_props(c.lookup_props & ~lookup_flag::ignore_flags);
    it.set_match(Skippy::Kind::none);
    if (!it.prev()) return false;
    const std::size_t j = it.idx;
    if (!is_mark(b.info[j])) return false;
    const unsigned id1 = lig::id(b.cur()), id2 = lig::id(b.info[j]);
    const unsigned c1 = lig::comp(b.cur()), c2 = lig::comp(b.info[j]);
    const bool good = (id1 == id2) ? (id1 == 0 || c1 == c2) : ((id1 > 0 && !c1) || (id2 > 0 && !c2));
    if (!good) return false;
    const int m2 = coverage(r, off16(r, s, s + 4), b.info[j].gid);
    if (m2 < 0) return false;
    return attach_mark_to(c, off16(r, s, s + 8), static_cast<unsigned>(m1), off16(r, s, s + 10),
                          static_cast<unsigned>(m2), r.u16_at(s + 6), j);
}

// ── dispatch ──────────────────────────────────────────────────────────────────────────

inline bool apply_subtable(ApplyContext& c, unsigned type, std::size_t s) {
    if (c.ti == table::gsub) {
        switch (type) {
            case 1: return gsub_single(c, s);
            case 2: return gsub_multiple(c, s);
            case 3: return gsub_alternate(c, s);
            case 4: return gsub_ligature(c, s);
            case 5: return apply_context(c, s);
            case 6: return apply_chain_context(c, s);
            case 8: return gsub_reverse_chain(c, s);
            default: return false;
        }
    }
    switch (type) {
        case 1: return gpos_single(c, s);
        case 2: return gpos_pair(c, s);
        case 3: return gpos_cursive(c, s);
        case 4: return gpos_mark_base(c, s);
        case 5: return gpos_mark_lig(c, s);
        case 6: return gpos_mark_mark(c, s);
        case 7: return apply_context(c, s);
        case 8: return apply_chain_context(c, s);
        default: return false;
    }
}

inline bool ApplyContext::apply_at(const LookupInfo& l) {
    const std::uint16_t g = buf.cur().gid;
    for (std::size_t k = 0; k < l.subtables.size(); ++k) {
        if (!l.sub_digest[k].may_have(g)) continue;
        if (apply_subtable(*this, l.type, l.subtables[k])) return true;
    }
    return false;
}

inline bool ApplyContext::recurse(unsigned lookup) {
    if (nesting_left == 0 || lookup >= lookups.size() || --buf.max_ops <= 0) return false;
    const LookupInfo& l = lookups[lookup];
    // A nested lookup must not change what the outer lookup skips, but runs
    // with its OWN flags.
    const std::uint32_t saved_props = lookup_props;
    const unsigned saved_index = lookup_index;
    --nesting_left;
    lookup_index = lookup;
    set_lookup_props(l.props);
    const bool hit = apply_at(l);   // HarfBuzz checks neither mask nor flags here
    set_lookup_props(saved_props);
    lookup_index = saved_index;
    ++nesting_left;
    return hit;
}

/// Run one lookup over the buffer in the direction its type requires.
inline void apply_lookup_to_buffer(ApplyContext& c, const LookupMap& lm) {
    const LookupInfo& l = c.lookups[lm.index];
    if (l.subtables.empty()) return;
    Buffer& b = c.buf;
    c.lookup_index = lm.index;
    c.lookup_mask = lm.mask;
    c.auto_zwj = lm.auto_zwj;
    c.auto_zwnj = lm.auto_zwnj;
    c.random = lm.random;
    c.per_syllable = lm.per_syllable;
    c.last_base = -1;
    c.last_base_until = 0;
    c.set_lookup_props(l.props);

    const bool reverse = c.ti == table::gsub && l.type == 8;
    const auto may_apply = [&](const GlyphInfo& g) {
        return (g.mask & c.lookup_mask) && l.digest.may_have(g.gid) && c.check_glyph_property(g, l.props);
    };
    if (!reverse) {
        const bool out_of_place = c.ti == table::gsub;   // GSUB writes out; GPOS is in place
        if (out_of_place) b.clear_output();
        b.idx = 0;
        while (b.idx < b.len() && b.successful) {
            bool applied = false;
            if (may_apply(b.cur())) applied = c.apply_at(l);
            if (!applied) {
                if (out_of_place) b.next_glyph(); else ++b.idx;
            }
        }
        if (out_of_place) b.sync();
        else b.idx = 0;
    } else {
        b.remove_output();
        b.idx = b.len();
        while (b.idx > 0) {
            --b.idx;
            if (may_apply(b.cur())) (void)c.apply_at(l);
        }
        b.idx = 0;
    }
}

}  // namespace detail

/// Fill each glyph's GDEF properties and clear ligature state
/// (hb_ot_layout_substitute_start). Without GDEF glyph classes they are
/// synthesized like HarfBuzz: General_Category Mn is a mark unless it is
/// default-ignorable (CGJ, Mongolian FVS), everything else a base.
inline void set_glyph_props(const Layout& l, Buffer& b) {
    const bool classes = l.has_glyph_classes();
    for (auto& g : b.info) {
        g.lig_props = 0;
        if (classes) g.glyph_props = l.glyph_props(g.gid);
        else g.glyph_props = (g.gc == static_cast<std::uint8_t>(ucd::GeneralCategory::Mn) &&
                              !is_default_ignorable(g)) ? props::mark : props::base_glyph;
    }
}

/// Budget per buffer, HarfBuzz's HB_BUFFER_MAX_OPS_FACTOR (64) / MAX_LEN_FACTOR (32).
inline void set_budget(Buffer& b) {
    b.max_ops = std::max<std::int64_t>(static_cast<std::int64_t>(b.len()) * 64, 16384);
    b.max_len = std::max<std::size_t>(b.len() * 32, 16384);
    b.successful = true;
}

/// Run every GSUB stage of `plan`, with its pauses. glyph_props must be set
/// (set_glyph_props) unless a shaper did it already.
inline void substitute(const Layout& l, const Plan& plan, Buffer& b, void* user = nullptr,
                       const std::vector<float>& coords = {}) {
    if (!l.has_gsub() && plan.stages[table::gsub].empty()) return;
    set_budget(b);
    detail::ApplyContext c{l, table::gsub, b, coords};
    const auto& infos = l.lookups(table::gsub);
    std::size_t i = 0;
    for (const StageMap& st : plan.stages[table::gsub]) {
        for (; i < st.last_lookup && b.successful; ++i) {
            const LookupMap& lm = plan.lookups[table::gsub][i];
            // Skip lookups that cannot touch any glyph in the buffer. Built
            // fresh per lookup: earlier lookups change the glyph set.
            Digest have;
            for (const auto& g : b.info) have.add(g.gid);
            if (!detail::intersects(have, infos[lm.index].digest)) continue;
            detail::apply_lookup_to_buffer(c, lm);
            c.init_iters();
        }
        if (st.pause) { st.pause(plan, b, user); c.init_iters(); }
    }
}

// ── would_substitute (hb_ot_layout_lookup_would_substitute) ───────────────────

namespace detail {

/// would_match_input: `glyphs[1..]` against `count - 1` u16 values at `at`.
/// kind 0 = glyph, 1 = class (ClassDef at `aux`), 2 = coverage (offsets from `aux`).
inline bool would_match_input(const Reader& r, std::span<const std::uint16_t> glyphs, unsigned count,
                              std::size_t at, int kind, std::size_t aux) {
    if (count != glyphs.size()) return false;
    for (unsigned i = 1; i < count; ++i) {
        const std::uint16_t v = r.u16_at(at + (i - 1) * 2);
        const std::uint16_t g = glyphs[i];
        bool ok = false;
        if (kind == 0) ok = g == v;
        else if (kind == 1) ok = class_of(r, aux, g) == v;
        else ok = coverage(r, off16(r, aux, at + (i - 1) * 2), g) >= 0;
        if (!ok) return false;
    }
    return true;
}

/// RuleSet::would_apply for (chain) context formats 1/2.
inline bool would_apply_rule_set(const Reader& r, std::size_t rs, bool chain, std::span<const std::uint16_t> glyphs,
                                 bool zero_context, int kind, std::size_t aux) {
    if (rs == npos) return false;
    const std::uint16_t n = r.u16_at(rs);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t rule = off16(r, rs, rs + 2 + k * 2);
        if (rule == npos) continue;
        if (!chain) {
            if (would_match_input(r, glyphs, r.u16_at(rule), rule + 4, kind, aux)) return true;
            continue;
        }
        const std::size_t bt = r.u16_at(rule);
        const std::size_t ip = rule + 2 + bt * 2;
        const unsigned in_count = r.u16_at(ip);
        const std::size_t la = ip + 2 + (in_count ? in_count - 1 : 0) * 2;
        const std::size_t la_count = r.u16_at(la);
        if (zero_context && (bt || la_count)) continue;
        if (would_match_input(r, glyphs, in_count, ip + 2, kind, aux)) return true;
    }
    return false;
}

inline bool would_apply_subtable(const Reader& r, unsigned type, std::size_t s, std::span<const std::uint16_t> glyphs,
                                 bool zero_context) {
    const std::uint16_t fmt = r.u16_at(s);
    const std::uint16_t g0 = glyphs[0];
    switch (type) {
        case 1: case 2: case 3: case 8:
            return glyphs.size() == 1 && coverage(r, off16(r, s, s + 2), g0) >= 0;
        case 4: {
            const int ci = coverage(r, off16(r, s, s + 2), g0);
            if (ci < 0 || static_cast<unsigned>(ci) >= r.u16_at(s + 4)) return false;
            const std::size_t set = off16(r, s, s + 6 + static_cast<std::size_t>(ci) * 2);
            if (set == npos) return false;
            const std::uint16_t n = r.u16_at(set);
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t lig = off16(r, set, set + 2 + k * 2);
                if (lig == npos) continue;
                if (would_match_input(r, glyphs, r.u16_at(lig + 2), lig + 4, 0, npos)) return true;
            }
            return false;
        }
        case 5: case 6: {
            const bool chain = type == 6;
            if (fmt == 1) {
                const int ci = coverage(r, off16(r, s, s + 2), g0);
                if (ci < 0 || static_cast<unsigned>(ci) >= r.u16_at(s + 4)) return false;
                return would_apply_rule_set(r, off16(r, s, s + 6 + static_cast<std::size_t>(ci) * 2), chain, glyphs,
                                            zero_context, 0, npos);
            }
            if (fmt == 2) {
                const std::size_t icd = off16(r, s, s + (chain ? 6 : 4));
                const std::size_t cnt_at = s + (chain ? 10 : 6);
                const unsigned k = class_of(r, icd, g0);
                if (k >= r.u16_at(cnt_at)) return false;
                return would_apply_rule_set(r, off16(r, s, cnt_at + 2 + std::size_t{k} * 2), chain, glyphs,
                                            zero_context, 1, icd);
            }
            if (fmt == 3) {
                if (!chain) return would_match_input(r, glyphs, r.u16_at(s + 2), s + 6 + 2, 2, s);
                const std::size_t bt = r.u16_at(s + 2);
                const std::size_t ip = s + 4 + bt * 2;
                const unsigned in_count = r.u16_at(ip);
                const std::size_t la_count = r.u16_at(ip + 2 + std::size_t{in_count} * 2);
                if (zero_context && (bt || la_count)) return false;
                return would_match_input(r, glyphs, in_count, ip + 4, 2, s);
            }
            return false;
        }
        default: return false;
    }
}

}  // namespace detail

/// hb_ot_layout_lookup_would_substitute: would GSUB lookup `lookup_index`
/// apply to exactly `glyphs` (ignoring lookup flags, as HarfBuzz does)?
[[nodiscard]] inline bool would_substitute(const Layout& l, unsigned lookup_index,
                                           std::span<const std::uint16_t> glyphs, bool zero_context) {
    const auto& infos = l.lookups(table::gsub);
    if (lookup_index >= infos.size() || glyphs.empty()) return false;
    const LookupInfo& li = infos[lookup_index];
    if (!li.digest.may_have(glyphs[0])) return false;
    const Reader r = l.tab(table::gsub).reader();
    for (std::size_t s : li.subtables)
        if (detail::would_apply_subtable(r, li.type, s, glyphs, zero_context)) return true;
    return false;
}

/// The lookups of the GSUB stage `feature_tag` lives in
/// (map->get_stage_lookups(0, map->get_feature_stage(0, tag))), as
/// [begin, end) indices into plan.lookups[gsub].
inline std::pair<std::size_t, std::size_t> feature_stage_lookups(const Plan& plan, std::uint32_t feature_tag) {
    const FeatureMap* f = plan.feature(feature_tag);
    const std::size_t stage = f ? f->stage[table::gsub] : ~std::size_t{0};
    const auto& st = plan.stages[table::gsub];
    const auto& lk = plan.lookups[table::gsub];
    if (stage > st.size()) return {0, 0};
    const std::size_t a = stage ? st[stage - 1].last_lookup : 0;
    const std::size_t e = stage < st.size() ? st[stage].last_lookup : lk.size();
    return {a, e};
}

/// Resolve attachment chains to absolute offsets (propagate_attachment_offsets).
/// For a logical-order buffer in `dir`.
inline void propagate_attachments(Buffer& b, Direction dir) {
    auto& pos = b.pos;
    const std::size_t len = pos.size();
    const auto propagate = [&](auto&& self, std::size_t i, unsigned depth) -> void {
        const int chain = pos[i].attach_chain;
        const unsigned type = pos[i].attach_type;
        if (!chain) return;
        pos[i].attach_chain = 0;
        const long jl = static_cast<long>(i) + chain;
        if (jl < 0 || static_cast<std::size_t>(jl) >= len || depth > 64) return;
        const std::size_t j = static_cast<std::size_t>(jl);
        self(self, j, depth + 1);
        if (type & detail::attach_cursive) {   // cross-direction only (horizontal text)
            pos[i].y_offset += pos[j].y_offset;
            return;
        }
        // Marks: main axis only; the cross offset was resolved at attach.
        pos[i].x_offset += pos[j].x_offset;
        if (j < i) {
            if (dir == Direction::ltr) for (std::size_t k = j; k < i; ++k) pos[i].x_offset -= pos[k].x_advance;
            else for (std::size_t k = j + 1; k < i + 1; ++k) pos[i].x_offset += pos[k].x_advance;
        } else {
            if (dir == Direction::ltr) for (std::size_t k = i; k < j; ++k) pos[i].x_offset += pos[k].x_advance;
            else for (std::size_t k = i + 1; k < j + 1; ++k) pos[i].x_offset -= pos[k].x_advance;
        }
    };
    // harfbuzz#5514: walk in the logical direction of the text.
    if (dir == Direction::ltr) {
        for (std::size_t i = 0; i < len; ++i)
            if (pos[i].attach_chain) propagate(propagate, i, 0);
    } else {
        for (std::size_t i = len; i-- > 0;)
            if (pos[i].attach_chain) propagate(propagate, i, 0);
    }
}

/// When marks lose their advance (HarfBuzz zero_width_marks): before GPOS
/// (Indic-family shapers), after it (most), or never.
enum class ZeroMarks : std::uint8_t { none, early, late };

/// Zero the advance of every GDEF mark (zero_mark_widths_by_gdef).
inline void zero_mark_advances(Buffer& b, bool adjust_offsets_when_zeroing = false) {
    for (std::size_t i = 0; i < b.len(); ++i) {
        if (!is_mark(b.info[i])) continue;
        if (adjust_offsets_when_zeroing) b.pos[i].x_offset -= b.pos[i].x_advance;
        b.pos[i].x_advance = 0;
        b.pos[i].y_advance = 0;
    }
}

/// Run every GPOS stage of `plan` (hb_ot_position_plan). The caller has
/// filled pos[i].x_advance with each glyph's advance (font units, incl.
/// HVAR). Order matters and is HarfBuzz's: zero marks early, GPOS, zero
/// marks late, zero default-ignorables, then resolve attachments, so mark
/// offsets are measured against the advances that will be drawn.
inline void position(const Layout& l, const Plan& plan, Buffer& b, const std::vector<float>& coords = {},
                     ZeroMarks zero = ZeroMarks::late, bool adjust_offsets_when_zeroing = false,
                     void* user = nullptr, bool apply_gpos = true,
                     const std::function<void(Buffer&)>& after_gpos = {}) {
    if (b.pos.size() != b.info.size()) b.pos.resize(b.info.size());
    for (auto& p : b.pos) { p.attach_chain = 0; p.attach_type = 0; }
    b.has_attachment = false;
    b.remove_output();
    set_budget(b);
    if (zero == ZeroMarks::early) zero_mark_advances(b, adjust_offsets_when_zeroing);
    detail::ApplyContext c{l, table::gpos, b, coords};
    if (apply_gpos && l.has_gpos()) {
        std::size_t i = 0;
        for (const StageMap& st : plan.stages[table::gpos]) {
            for (; i < st.last_lookup && b.successful; ++i) {
                detail::apply_lookup_to_buffer(c, plan.lookups[table::gpos][i]);
                c.init_iters();
            }
            if (st.pause) { st.pause(plan, b, user); c.init_iters(); }
        }
    }
    if (after_gpos) after_gpos(b);
    if (zero == ZeroMarks::late) zero_mark_advances(b, adjust_offsets_when_zeroing);
    for (std::size_t i = 0; i < b.len(); ++i)
        if (is_default_ignorable(b.info[i])) {
            GlyphPos& p = b.pos[i];
            p.x_advance = p.y_advance = p.x_offset = p.y_offset = 0;
        }
    if (c.has_attachment || b.has_attachment) propagate_attachments(b, b.direction);
}

}  // namespace mayag::typo::ot
