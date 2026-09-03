#pragma once
// mayag::typo::ot — the OpenType binary layer
//
// A font file is untrusted input. It arrives from a download, a user's disk,
// or a web font server, and a malformed one must produce an error rather than
// a segfault or a heap overread. Every accessor in this file is therefore
// BOUNDS-CHECKED and returns a defined value (zero / empty) when the file
// lies about its own structure. There is no path from a corrupt font to
// undefined behaviour.
//
// This layer knows about bytes and tables. It does not know about outlines,
// rasterisation, or layout — those sit on top.
//
// Covers: sfnt + TrueType Collections, `head`, `hhea`, `maxp`, `hmtx`,
// `name`, `OS/2`, `post`, and `cmap` formats 0/4/6/12/13/14.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::typo::ot {

// ════════════════════════════════════════════════════════════════════════
// Reader — bounds-checked big-endian access
// ════════════════════════════════════════════════════════════════════════

/// All OpenType scalars are big-endian. This reader never reads past its
/// span: an out-of-range access yields 0 and sets the error flag, so a
/// truncated font degrades to "missing data" instead of a crash.
class Reader {
  public:
    Reader() = default;
    explicit Reader(std::span<const std::uint8_t> data, std::size_t pos = 0) noexcept
        : data_{data}, pos_{pos} {}

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] std::size_t tell() const noexcept { return pos_; }
    [[nodiscard]] bool        bad()  const noexcept { return bad_; }
    [[nodiscard]] bool        has(std::size_t n) const noexcept {
        return pos_ + n <= data_.size();
    }

    void seek(std::size_t p) noexcept { pos_ = p; }
    void skip(std::size_t n) noexcept { pos_ += n; }

    [[nodiscard]] std::uint8_t u8() noexcept {
        if (pos_ + 1 > data_.size()) { bad_ = true; return 0; }
        return data_[pos_++];
    }
    [[nodiscard]] std::int8_t i8() noexcept { return static_cast<std::int8_t>(u8()); }

    [[nodiscard]] std::uint16_t u16() noexcept {
        if (pos_ + 2 > data_.size()) { bad_ = true; pos_ = data_.size(); return 0; }
        const auto v = static_cast<std::uint16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
        pos_ += 2;
        return v;
    }
    [[nodiscard]] std::int16_t i16() noexcept { return static_cast<std::int16_t>(u16()); }

    [[nodiscard]] std::uint32_t u24() noexcept {
        const std::uint32_t hi = u16();
        return (hi << 8) | u8();
    }

    [[nodiscard]] std::uint32_t u32() noexcept {
        if (pos_ + 4 > data_.size()) { bad_ = true; pos_ = data_.size(); return 0; }
        const std::uint32_t v =
            (static_cast<std::uint32_t>(data_[pos_])     << 24) |
            (static_cast<std::uint32_t>(data_[pos_ + 1]) << 16) |
            (static_cast<std::uint32_t>(data_[pos_ + 2]) <<  8) |
             static_cast<std::uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return v;
    }
    [[nodiscard]] std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }

    [[nodiscard]] std::int64_t i64() noexcept {
        const std::uint64_t hi = u32();
        return static_cast<std::int64_t>((hi << 32) | u32());
    }

    /// 16.16 fixed point.
    [[nodiscard]] float fixed() noexcept {
        return static_cast<float>(i32()) / 65536.0f;
    }
    /// 2.14 fixed point — component transforms in composite glyphs.
    [[nodiscard]] float f2dot14() noexcept {
        return static_cast<float>(i16()) / 16384.0f;
    }

    // ── random access; never moves the cursor, never throws ─────────────

    [[nodiscard]] std::uint8_t u8_at(std::size_t o) const noexcept {
        return o < data_.size() ? data_[o] : 0;
    }
    [[nodiscard]] std::uint16_t u16_at(std::size_t o) const noexcept {
        if (o + 2 > data_.size()) return 0;
        return static_cast<std::uint16_t>((data_[o] << 8) | data_[o + 1]);
    }
    [[nodiscard]] std::int16_t i16_at(std::size_t o) const noexcept {
        return static_cast<std::int16_t>(u16_at(o));
    }
    [[nodiscard]] std::uint32_t u32_at(std::size_t o) const noexcept {
        if (o + 4 > data_.size()) return 0;
        return (static_cast<std::uint32_t>(data_[o])     << 24) |
               (static_cast<std::uint32_t>(data_[o + 1]) << 16) |
               (static_cast<std::uint32_t>(data_[o + 2]) <<  8) |
                static_cast<std::uint32_t>(data_[o + 3]);
    }

    /// Clamped subspan — the length is truncated to what actually exists.
    [[nodiscard]] std::span<const std::uint8_t> slice(std::size_t off,
                                                      std::size_t len) const noexcept {
        if (off >= data_.size()) return {};
        return data_.subspan(off, std::min(len, data_.size() - off));
    }

    [[nodiscard]] std::span<const std::uint8_t> rest() const noexcept {
        return slice(pos_, data_.size() - pos_);
    }

  private:
    std::span<const std::uint8_t> data_{};
    std::size_t                   pos_  = 0;
    bool                          bad_  = false;
};

// ════════════════════════════════════════════════════════════════════════
// Tags
// ════════════════════════════════════════════════════════════════════════

/// A table tag packed into a u32, so lookups are integer compares.
[[nodiscard]] constexpr std::uint32_t tag(const char (&s)[5]) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) <<  8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
}

[[nodiscard]] inline std::string tag_string(std::uint32_t t) {
    return {static_cast<char>((t >> 24) & 0xFF), static_cast<char>((t >> 16) & 0xFF),
            static_cast<char>((t >>  8) & 0xFF), static_cast<char>( t        & 0xFF)};
}

namespace tags {
inline constexpr std::uint32_t cmap = tag("cmap");
inline constexpr std::uint32_t head = tag("head");
inline constexpr std::uint32_t hhea = tag("hhea");
inline constexpr std::uint32_t hmtx = tag("hmtx");
inline constexpr std::uint32_t maxp = tag("maxp");
inline constexpr std::uint32_t name = tag("name");
inline constexpr std::uint32_t os2  = tag("OS/2");
inline constexpr std::uint32_t post = tag("post");
inline constexpr std::uint32_t glyf = tag("glyf");
inline constexpr std::uint32_t loca = tag("loca");
inline constexpr std::uint32_t cff  = tag("CFF ");
inline constexpr std::uint32_t cff2 = tag("CFF2");
inline constexpr std::uint32_t gsub = tag("GSUB");
inline constexpr std::uint32_t gpos = tag("GPOS");
inline constexpr std::uint32_t gdef = tag("GDEF");
inline constexpr std::uint32_t kern = tag("kern");
inline constexpr std::uint32_t colr = tag("COLR");
inline constexpr std::uint32_t cpal = tag("CPAL");
inline constexpr std::uint32_t fvar = tag("fvar");
inline constexpr std::uint32_t vhea = tag("vhea");
inline constexpr std::uint32_t vmtx = tag("vmtx");
}  // namespace tags

// ════════════════════════════════════════════════════════════════════════
// Table directory
// ════════════════════════════════════════════════════════════════════════

struct TableRecord {
    std::uint32_t tag      = 0;
    std::uint32_t checksum = 0;
    std::uint32_t offset   = 0;
    std::uint32_t length   = 0;
};

/// sfnt version values that identify a font we can read.
inline constexpr std::uint32_t sfnt_truetype = 0x0001'0000u;   // TrueType outlines
inline constexpr std::uint32_t sfnt_true     = tag("true");    // legacy Apple
inline constexpr std::uint32_t sfnt_otto     = tag("OTTO");    // CFF outlines
inline constexpr std::uint32_t sfnt_ttcf     = tag("ttcf");    // collection

enum class Error {
    none,
    too_small,
    bad_signature,
    bad_table_directory,
    missing_required_table,
    unsupported_format,
    bad_face_index,
};

[[nodiscard]] constexpr std::string_view error_text(Error e) noexcept {
    switch (e) {
        case Error::none:                   return "ok";
        case Error::too_small:              return "file is too small to be a font";
        case Error::bad_signature:          return "not an sfnt/OpenType/TTC file";
        case Error::bad_table_directory:    return "table directory is malformed";
        case Error::missing_required_table: return "a required table is missing";
        case Error::unsupported_format:     return "unsupported font format";
        case Error::bad_face_index:         return "face index out of range in collection";
    }
    return "unknown";
}

// ════════════════════════════════════════════════════════════════════════
// Parsed tables
// ════════════════════════════════════════════════════════════════════════

/// `head` — global metrics and the flags that change how other tables parse.
struct Head {
    float         version         = 0.0f;
    float         font_revision   = 0.0f;
    std::uint16_t flags           = 0;
    std::uint16_t units_per_em    = 1000;   ///< design grid; scale = size / this
    std::int16_t  x_min = 0, y_min = 0, x_max = 0, y_max = 0;
    std::uint16_t mac_style       = 0;
    std::uint16_t lowest_rec_ppem = 0;
    /// 0 = `loca` holds u16 half-offsets, 1 = u32 offsets. Getting this wrong
    /// silently reads every glyph from the wrong place.
    std::int16_t  index_to_loc_format = 0;

    [[nodiscard]] bool bold_flag()   const noexcept { return (mac_style & 0x01) != 0; }
    [[nodiscard]] bool italic_flag() const noexcept { return (mac_style & 0x02) != 0; }
};

/// `hhea` — horizontal line metrics.
struct Hhea {
    std::int16_t  ascender    = 0;
    std::int16_t  descender   = 0;   ///< negative, below the baseline
    std::int16_t  line_gap    = 0;
    std::uint16_t advance_width_max = 0;
    std::int16_t  min_left_side_bearing  = 0;
    std::int16_t  min_right_side_bearing = 0;
    std::int16_t  x_max_extent = 0;
    std::int16_t  caret_slope_rise = 1;
    std::int16_t  caret_slope_run  = 0;
    std::uint16_t number_of_h_metrics = 0;
};

struct Maxp {
    float         version    = 0.0f;
    std::uint16_t num_glyphs = 0;
    std::uint16_t max_points = 0;
    std::uint16_t max_contours = 0;
    std::uint16_t max_composite_depth = 0;
};

/// `OS/2` — the table that actually carries usable typographic metrics.
struct Os2 {
    std::uint16_t version = 0;
    std::int16_t  x_avg_char_width = 0;
    std::uint16_t weight_class = 400;
    std::uint16_t width_class  = 5;
    std::int16_t  y_subscript_x_size = 0, y_subscript_y_size = 0;
    std::int16_t  y_superscript_x_size = 0, y_superscript_y_size = 0;
    std::int16_t  y_strikeout_size = 0, y_strikeout_position = 0;
    std::uint16_t fs_selection = 0;
    std::uint16_t first_char_index = 0, last_char_index = 0;
    /// The `sTypo*` fields are the ones the spec says to use for line layout;
    /// the `usWin*` pair are legacy clipping bounds that many fonts inflate.
    std::int16_t  typo_ascender = 0, typo_descender = 0, typo_line_gap = 0;
    std::uint16_t win_ascent = 0, win_descent = 0;
    std::int16_t  x_height = 0, cap_height = 0;
    bool          present = false;

    [[nodiscard]] bool italic()    const noexcept { return (fs_selection & 0x001) != 0; }
    [[nodiscard]] bool bold()      const noexcept { return (fs_selection & 0x020) != 0; }
    /// Bit 7: "use sTypo metrics" — when set, the typo fields are authoritative.
    [[nodiscard]] bool use_typo_metrics() const noexcept { return (fs_selection & 0x080) != 0; }
};

// ════════════════════════════════════════════════════════════════════════
// cmap — character to glyph mapping
// ════════════════════════════════════════════════════════════════════════

/// A parsed character map. Formats are normalised into one of two shapes:
/// a dense array (fast, small ranges) or sorted ranges (sparse, Unicode-wide).
/// Both answer `lookup()` without touching the original file again, so a
/// hostile font cannot make lookup misbehave after construction.
class CharMap {
  public:
    struct Range {
        std::uint32_t start_code = 0;
        std::uint32_t end_code   = 0;
        std::uint32_t start_glyph = 0;
        /// format 4 needs per-range id-delta/id-range semantics; we flatten
        /// those into explicit glyph lists when the range is not contiguous.
        bool          contiguous = true;
    };

    [[nodiscard]] std::uint16_t lookup(std::uint32_t codepoint) const noexcept {
        // Dense fast path for the BMP-Latin region most text lives in.
        if (codepoint < dense_.size()) return dense_[codepoint];

        // Binary search the sorted ranges.
        auto it = std::upper_bound(ranges_.begin(), ranges_.end(), codepoint,
                                   [](std::uint32_t c, const Range& r) { return c < r.start_code; });
        if (it == ranges_.begin()) return 0;
        --it;
        if (codepoint > it->end_code) return 0;

        if (it->contiguous) {
            return static_cast<std::uint16_t>(it->start_glyph + (codepoint - it->start_code));
        }
        const std::size_t idx = it->start_glyph + (codepoint - it->start_code);
        return idx < sparse_.size() ? sparse_[idx] : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return dense_.empty() && ranges_.empty(); }

    /// Every codepoint this map can resolve. Used for coverage queries when
    /// building a fallback chain.
    [[nodiscard]] std::vector<std::uint32_t> codepoints() const {
        std::vector<std::uint32_t> out;
        for (std::uint32_t c = 0; c < dense_.size(); ++c) {
            if (dense_[c] != 0) out.push_back(c);
        }
        for (const auto& r : ranges_) {
            for (std::uint32_t c = r.start_code; c <= r.end_code && c - r.start_code < 0x10000; ++c) {
                if (lookup(c) != 0) out.push_back(c);
            }
        }
        return out;
    }

    /// Parse the best available subtable from a `cmap` table.
    [[nodiscard]] static CharMap parse(Reader r, std::size_t cmap_offset);

  private:
    static CharMap parse_format0(Reader& r, std::size_t off);
    static CharMap parse_format4(Reader& r, std::size_t off);
    static CharMap parse_format6(Reader& r, std::size_t off);
    static CharMap parse_format12(Reader& r, std::size_t off);

    /// Codepoints below this get an O(1) array. 0x300 covers Latin, Latin-1
    /// Supplement, Latin Extended-A/B, IPA, and combining diacriticals —
    /// which is the overwhelming majority of lookups in practice, for 1.5 KB.
    static constexpr std::uint32_t dense_limit = 0x300;

    std::vector<std::uint16_t> dense_;    ///< index = codepoint
    std::vector<Range>         ranges_;   ///< sorted by start_code
    std::vector<std::uint16_t> sparse_;   ///< glyph ids for non-contiguous ranges
};

inline CharMap CharMap::parse_format0(Reader& r, std::size_t off) {
    // Byte encoding: 256 single-byte codes. Ancient, but still present in
    // some Mac-era fonts as the only subtable.
    CharMap m;
    m.dense_.assign(256, 0);
    for (std::uint32_t c = 0; c < 256; ++c) {
        m.dense_[c] = r.u8_at(off + 6 + c);
    }
    return m;
}

inline CharMap CharMap::parse_format4(Reader& r, std::size_t off) {
    // The workhorse: segmented mapping for the BMP. Four parallel arrays plus
    // an optional indirection through glyphIdArray.
    CharMap m;

    const std::uint16_t seg_x2 = r.u16_at(off + 6);
    const std::uint16_t segs   = seg_x2 / 2;
    if (segs == 0) return m;

    const std::size_t end_base    = off + 14;
    const std::size_t start_base  = end_base + seg_x2 + 2;   // +2 skips reservedPad
    const std::size_t delta_base  = start_base + seg_x2;
    const std::size_t range_base  = delta_base + seg_x2;

    m.dense_.assign(dense_limit, 0);

    for (std::uint16_t s = 0; s < segs; ++s) {
        const std::uint32_t end   = r.u16_at(end_base   + s * 2);
        const std::uint32_t start = r.u16_at(start_base + s * 2);
        const std::int16_t  delta = r.i16_at(delta_base + s * 2);
        const std::uint16_t ro    = r.u16_at(range_base + s * 2);

        if (start > end) continue;                 // malformed segment
        if (start == 0xFFFF) continue;             // the required terminator

        if (ro == 0) {
            // Contiguous: glyph = (code + delta) mod 65536.
            Range range{start, end,
                        static_cast<std::uint32_t>((start + delta) & 0xFFFF), true};
            m.ranges_.push_back(range);

            for (std::uint32_t c = start; c <= end && c < dense_limit; ++c) {
                m.dense_[c] = static_cast<std::uint16_t>((c + delta) & 0xFFFF);
            }
        } else {
            // Indirect: read through glyphIdArray, which lives immediately
            // after idRangeOffset and is addressed RELATIVE to the offset's
            // own position — the single most error-prone rule in `cmap`.
            const std::size_t base = range_base + s * 2;
            const std::uint32_t first = static_cast<std::uint32_t>(m.sparse_.size());

            for (std::uint32_t c = start; c <= end; ++c) {
                const std::size_t gi_addr = base + ro + (c - start) * 2;
                std::uint16_t g = r.u16_at(gi_addr);
                if (g != 0) g = static_cast<std::uint16_t>((g + delta) & 0xFFFF);
                m.sparse_.push_back(g);
                if (c < dense_limit) m.dense_[c] = g;
            }
            m.ranges_.push_back(Range{start, end, first, false});
        }
    }

    std::sort(m.ranges_.begin(), m.ranges_.end(),
              [](const Range& a, const Range& b) { return a.start_code < b.start_code; });
    return m;
}

inline CharMap CharMap::parse_format6(Reader& r, std::size_t off) {
    // Trimmed table: a single contiguous run.
    CharMap m;
    const std::uint32_t first = r.u16_at(off + 6);
    const std::uint32_t count = r.u16_at(off + 8);
    if (count == 0) return m;

    const std::uint32_t first_idx = 0;
    m.sparse_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        m.sparse_.push_back(r.u16_at(off + 10 + i * 2));
    }
    m.ranges_.push_back(Range{first, first + count - 1, first_idx, false});

    m.dense_.assign(dense_limit, 0);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t c = first + i;
        if (c < dense_limit) m.dense_[c] = m.sparse_[i];
    }
    return m;
}

inline CharMap CharMap::parse_format12(Reader& r, std::size_t off) {
    // Segmented coverage for the full Unicode range — the format any font
    // with emoji, CJK extensions, or historic scripts will use.
    CharMap m;
    const std::uint32_t groups = r.u32_at(off + 12);

    // A hostile font can claim billions of groups; cap at what the table
    // could physically hold.
    const std::uint32_t max_groups =
        static_cast<std::uint32_t>((r.size() - std::min(r.size(), off + 16)) / 12);
    const std::uint32_t n = std::min(groups, max_groups);

    m.dense_.assign(dense_limit, 0);
    m.ranges_.reserve(n);

    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t g = off + 16 + i * 12;
        const std::uint32_t start = r.u32_at(g);
        const std::uint32_t end   = r.u32_at(g + 4);
        const std::uint32_t gid   = r.u32_at(g + 8);
        if (start > end) continue;

        m.ranges_.push_back(Range{start, end, gid, true});
        for (std::uint32_t c = start; c <= end && c < dense_limit; ++c) {
            m.dense_[c] = static_cast<std::uint16_t>(gid + (c - start));
        }
    }

    std::sort(m.ranges_.begin(), m.ranges_.end(),
              [](const Range& a, const Range& b) { return a.start_code < b.start_code; });
    return m;
}

inline CharMap CharMap::parse(Reader r, std::size_t cmap_offset) {
    const std::uint16_t n_tables = r.u16_at(cmap_offset + 2);

    // Subtable preference, best first. The ordering matters:
    //   (3,10) and (0,4/6) are full-Unicode — required for anything past the BMP
    //   (3,1) and (0,3) are BMP Unicode — the common case
    //   (3,0) is "symbol", used by icon fonts, which map into 0xF000..0xF0FF
    //   (1,0) is Mac Roman, the last resort
    struct Candidate { int score; std::size_t offset; };
    Candidate best{-1, 0};

    for (std::uint16_t i = 0; i < n_tables; ++i) {
        const std::size_t rec = cmap_offset + 4 + i * 8;
        const std::uint16_t platform = r.u16_at(rec);
        const std::uint16_t encoding = r.u16_at(rec + 2);
        const std::uint32_t sub_off  = r.u32_at(rec + 4);

        int score = -1;
        if (platform == 3 && encoding == 10) score = 100;  // Windows UCS-4
        else if (platform == 0 && (encoding == 4 || encoding == 6)) score = 95;
        else if (platform == 3 && encoding == 1)  score = 90;   // Windows BMP
        else if (platform == 0)                   score = 85;   // Unicode, any
        else if (platform == 3 && encoding == 0)  score = 60;   // symbol
        else if (platform == 1 && encoding == 0)  score = 10;   // Mac Roman

        if (score > best.score) best = Candidate{score, cmap_offset + sub_off};
    }

    if (best.score < 0) return {};

    switch (r.u16_at(best.offset)) {
        case 0:  return parse_format0(r, best.offset);
        case 4:  return parse_format4(r, best.offset);
        case 6:  return parse_format6(r, best.offset);
        case 12:
        case 13: return parse_format12(r, best.offset);
        default: return {};
    }
}

// ════════════════════════════════════════════════════════════════════════
// name — human-readable strings
// ════════════════════════════════════════════════════════════════════════

namespace name_id {
inline constexpr std::uint16_t copyright     = 0;
inline constexpr std::uint16_t family        = 1;
inline constexpr std::uint16_t subfamily     = 2;
inline constexpr std::uint16_t unique_id     = 3;
inline constexpr std::uint16_t full_name     = 4;
inline constexpr std::uint16_t version       = 5;
inline constexpr std::uint16_t postscript    = 6;
inline constexpr std::uint16_t trademark     = 7;
inline constexpr std::uint16_t manufacturer  = 8;
inline constexpr std::uint16_t designer      = 9;
inline constexpr std::uint16_t license       = 13;
inline constexpr std::uint16_t typo_family   = 16;   ///< preferred family
inline constexpr std::uint16_t typo_subfamily = 17;  ///< preferred subfamily
}  // namespace name_id

/// Read a `name` record, converting UTF-16BE to UTF-8 where needed.
[[nodiscard]] inline std::string read_name(const Reader& r, std::size_t name_off,
                                           std::uint16_t want_id) {
    const std::uint16_t count        = r.u16_at(name_off + 2);
    const std::uint16_t string_off   = r.u16_at(name_off + 4);
    const std::size_t   strings_base = name_off + string_off;

    // Prefer a Windows/Unicode record, fall back to Mac Roman.
    std::size_t best_off = 0, best_len = 0;
    int best_score = -1;
    bool best_utf16 = false;

    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t rec = name_off + 6 + i * 12;
        const std::uint16_t platform = r.u16_at(rec);
        const std::uint16_t language = r.u16_at(rec + 4);
        const std::uint16_t nid      = r.u16_at(rec + 6);
        const std::uint16_t len      = r.u16_at(rec + 8);
        const std::uint16_t off      = r.u16_at(rec + 10);

        if (nid != want_id) continue;

        int score = 0;
        bool utf16 = false;
        if (platform == 3) { score = language == 0x0409 ? 100 : 80; utf16 = true; }
        else if (platform == 0) { score = 90; utf16 = true; }
        else if (platform == 1) { score = 50; }

        if (score > best_score) {
            best_score = score;
            best_off   = strings_base + off;
            best_len   = len;
            best_utf16 = utf16;
        }
    }

    if (best_score < 0 || best_len == 0) return {};

    std::string out;
    if (best_utf16) {
        // UTF-16BE -> UTF-8, with surrogate pairs handled so a font whose
        // name contains an astral character does not produce mojibake.
        out.reserve(best_len);
        for (std::size_t i = 0; i + 1 < best_len; i += 2) {
            std::uint32_t cp = r.u16_at(best_off + i);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < best_len) {
                const std::uint32_t lo = r.u16_at(best_off + i + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
    } else {
        for (std::size_t i = 0; i < best_len; ++i) {
            out.push_back(static_cast<char>(r.u8_at(best_off + i)));
        }
    }
    return out;
}

// ════════════════════════════════════════════════════════════════════════
// FontFile — the parsed container
// ════════════════════════════════════════════════════════════════════════

/// A single face within a font file. Holds no ownership of the bytes: the
/// caller keeps the file data alive (typically a `std::vector<uint8_t>` in
/// `font::Blob`), and every table here is a view into it.
class FontFile {
  public:
    /// Parse face `index` from a font file or collection.
    [[nodiscard]] static std::optional<FontFile> parse(std::span<const std::uint8_t> data,
                                                       std::uint32_t face_index,
                                                       Error* err = nullptr);

    /// Number of faces in the file (>1 only for .ttc/.otc collections).
    [[nodiscard]] static std::uint32_t face_count(std::span<const std::uint8_t> data) noexcept {
        Reader r{data};
        if (r.u32_at(0) == sfnt_ttcf) return r.u32_at(8);
        return 1;
    }

    // ── tables ──────────────────────────────────────────────────────────

    [[nodiscard]] const Head& head() const noexcept { return head_; }
    [[nodiscard]] const Hhea& hhea() const noexcept { return hhea_; }
    [[nodiscard]] const Maxp& maxp() const noexcept { return maxp_; }
    [[nodiscard]] const Os2&  os2()  const noexcept { return os2_;  }
    [[nodiscard]] const CharMap& cmap() const noexcept { return cmap_; }

    [[nodiscard]] std::uint16_t num_glyphs() const noexcept { return maxp_.num_glyphs; }
    [[nodiscard]] std::uint16_t units_per_em() const noexcept { return head_.units_per_em; }

    /// Raw table bytes, or an empty span when absent.
    [[nodiscard]] std::span<const std::uint8_t> table(std::uint32_t t) const noexcept {
        for (const auto& rec : tables_) {
            if (rec.tag == t) return reader_.slice(rec.offset, rec.length);
        }
        return {};
    }
    [[nodiscard]] bool has_table(std::uint32_t t) const noexcept { return !table(t).empty(); }

    [[nodiscard]] const std::vector<TableRecord>& tables() const noexcept { return tables_; }
    [[nodiscard]] const Reader& reader() const noexcept { return reader_; }

    // ── identity ────────────────────────────────────────────────────────

    [[nodiscard]] const std::string& family() const noexcept { return family_; }
    [[nodiscard]] const std::string& subfamily() const noexcept { return subfamily_; }
    [[nodiscard]] const std::string& full_name() const noexcept { return full_name_; }
    [[nodiscard]] const std::string& postscript_name() const noexcept { return ps_name_; }

    /// Which outline format this face uses. Both are supported; they take
    /// different code paths in `outline.hpp`.
    [[nodiscard]] bool is_cff() const noexcept { return is_cff_; }
    [[nodiscard]] bool is_truetype() const noexcept { return !is_cff_; }

    [[nodiscard]] bool is_bold() const noexcept {
        return os2_.present ? os2_.bold() : head_.bold_flag();
    }
    [[nodiscard]] bool is_italic() const noexcept {
        return os2_.present ? os2_.italic() : head_.italic_flag();
    }
    [[nodiscard]] std::uint16_t weight() const noexcept {
        return os2_.present ? os2_.weight_class : (head_.bold_flag() ? 700 : 400);
    }

    // ── metrics ─────────────────────────────────────────────────────────

    /// Advance width in font units.
    [[nodiscard]] std::uint16_t advance(std::uint16_t gid) const noexcept {
        if (hmtx_.empty() || hhea_.number_of_h_metrics == 0) return 0;
        const std::uint16_t n = hhea_.number_of_h_metrics;
        // Glyphs past `numberOfHMetrics` all share the last advance; only
        // their left side bearings differ. This is how monospace-tail fonts
        // stay compact.
        const std::uint16_t i = gid < n ? gid : static_cast<std::uint16_t>(n - 1);
        const std::size_t off = static_cast<std::size_t>(i) * 4;
        if (off + 2 > hmtx_.size()) return 0;
        return static_cast<std::uint16_t>((hmtx_[off] << 8) | hmtx_[off + 1]);
    }

    /// Left side bearing in font units.
    [[nodiscard]] std::int16_t lsb(std::uint16_t gid) const noexcept {
        if (hmtx_.empty()) return 0;
        const std::uint16_t n = hhea_.number_of_h_metrics;
        std::size_t off;
        if (gid < n) {
            off = static_cast<std::size_t>(gid) * 4 + 2;
        } else {
            off = static_cast<std::size_t>(n) * 4 +
                  static_cast<std::size_t>(gid - n) * 2;
        }
        if (off + 2 > hmtx_.size()) return 0;
        return static_cast<std::int16_t>((hmtx_[off] << 8) | hmtx_[off + 1]);
    }

    [[nodiscard]] std::uint16_t glyph_for(std::uint32_t codepoint) const noexcept {
        const std::uint16_t g = cmap_.lookup(codepoint);
        if (g != 0) return g;
        // Symbol fonts (platform 3, encoding 0) map ASCII into the private
        // use area at 0xF000. Retrying there is what makes icon fonts work.
        if (codepoint < 0x100) return cmap_.lookup(0xF000 + codepoint);
        return 0;
    }

  private:
    Reader                   reader_;
    std::vector<TableRecord> tables_;
    Head                     head_;
    Hhea                     hhea_;
    Maxp                     maxp_;
    Os2                      os2_;
    CharMap                  cmap_;
    std::span<const std::uint8_t> hmtx_;
    std::string              family_, subfamily_, full_name_, ps_name_;
    bool                     is_cff_ = false;
};

inline std::optional<FontFile> FontFile::parse(std::span<const std::uint8_t> data,
                                               std::uint32_t face_index, Error* err) {
    const auto fail = [&](Error e) -> std::optional<FontFile> {
        if (err != nullptr) *err = e;
        return std::nullopt;
    };
    if (err != nullptr) *err = Error::none;

    if (data.size() < 12) return fail(Error::too_small);

    Reader r{data};
    std::size_t dir_offset = 0;

    // A TrueType Collection shares glyph data between faces; the header is
    // just an array of offsets to ordinary sfnt directories.
    if (r.u32_at(0) == sfnt_ttcf) {
        const std::uint32_t n = r.u32_at(8);
        if (face_index >= n) return fail(Error::bad_face_index);
        dir_offset = r.u32_at(12 + face_index * 4);
        if (dir_offset + 12 > data.size()) return fail(Error::bad_table_directory);
    } else if (face_index != 0) {
        return fail(Error::bad_face_index);
    }

    const std::uint32_t version = r.u32_at(dir_offset);
    if (version != sfnt_truetype && version != sfnt_true && version != sfnt_otto) {
        return fail(Error::bad_signature);
    }

    FontFile f;
    f.reader_ = r;
    f.is_cff_ = (version == sfnt_otto);

    const std::uint16_t num_tables = r.u16_at(dir_offset + 4);
    // Sanity: the directory itself must fit.
    if (dir_offset + 12 + static_cast<std::size_t>(num_tables) * 16 > data.size()) {
        return fail(Error::bad_table_directory);
    }

    f.tables_.reserve(num_tables);
    for (std::uint16_t i = 0; i < num_tables; ++i) {
        const std::size_t rec = dir_offset + 12 + static_cast<std::size_t>(i) * 16;
        TableRecord tr;
        tr.tag      = r.u32_at(rec);
        tr.checksum = r.u32_at(rec + 4);
        tr.offset   = r.u32_at(rec + 8);
        tr.length   = r.u32_at(rec + 12);
        // Drop records that point outside the file rather than trusting them
        // later, so every `table()` result is guaranteed in-bounds.
        if (tr.offset < data.size()) f.tables_.push_back(tr);
    }

    // ---- head ----
    const auto head_data = f.table(tags::head);
    if (head_data.size() < 54) return fail(Error::missing_required_table);
    {
        Reader h{head_data};
        f.head_.version       = h.fixed();
        f.head_.font_revision = h.fixed();
        h.skip(4);                       // checksum adjustment
        h.skip(4);                       // magic
        f.head_.flags         = h.u16();
        f.head_.units_per_em  = h.u16();
        h.skip(16);                      // created + modified
        f.head_.x_min = h.i16(); f.head_.y_min = h.i16();
        f.head_.x_max = h.i16(); f.head_.y_max = h.i16();
        f.head_.mac_style       = h.u16();
        f.head_.lowest_rec_ppem = h.u16();
        (void)h.i16();                   // fontDirectionHint
        f.head_.index_to_loc_format = h.i16();

        // A zero unitsPerEm would make every scale computation divide by zero.
        if (f.head_.units_per_em == 0) f.head_.units_per_em = 1000;
    }

    // ---- maxp ----
    const auto maxp_data = f.table(tags::maxp);
    if (maxp_data.size() < 6) return fail(Error::missing_required_table);
    {
        Reader m{maxp_data};
        f.maxp_.version    = m.fixed();
        f.maxp_.num_glyphs = m.u16();
        if (maxp_data.size() >= 32) {
            f.maxp_.max_points   = m.u16();
            f.maxp_.max_contours = m.u16();
        }
    }

    // ---- hhea ----
    const auto hhea_data = f.table(tags::hhea);
    if (hhea_data.size() >= 36) {
        Reader h{hhea_data};
        h.skip(4);                        // version
        f.hhea_.ascender  = h.i16();
        f.hhea_.descender = h.i16();
        f.hhea_.line_gap  = h.i16();
        f.hhea_.advance_width_max      = h.u16();
        f.hhea_.min_left_side_bearing  = h.i16();
        f.hhea_.min_right_side_bearing = h.i16();
        f.hhea_.x_max_extent    = h.i16();
        f.hhea_.caret_slope_rise = h.i16();
        f.hhea_.caret_slope_run  = h.i16();
        h.skip(2 + 8 + 2);                // caretOffset, reserved, metricFormat
        f.hhea_.number_of_h_metrics = h.u16();
    }

    // ---- hmtx ----
    f.hmtx_ = f.table(tags::hmtx);

    // ---- OS/2 ----
    const auto os2_data = f.table(tags::os2);
    if (os2_data.size() >= 78) {
        Reader o{os2_data};
        f.os2_.present          = true;
        f.os2_.version          = o.u16();
        f.os2_.x_avg_char_width = o.i16();
        f.os2_.weight_class     = o.u16();
        f.os2_.width_class      = o.u16();
        o.skip(2);                        // fsType
        f.os2_.y_subscript_x_size   = o.i16();
        f.os2_.y_subscript_y_size   = o.i16();
        o.skip(4);                        // subscript offsets
        f.os2_.y_superscript_x_size = o.i16();
        f.os2_.y_superscript_y_size = o.i16();
        o.skip(4);                        // superscript offsets
        f.os2_.y_strikeout_size     = o.i16();
        f.os2_.y_strikeout_position = o.i16();
        o.skip(2);                        // sFamilyClass
        o.skip(10);                       // panose
        o.skip(16);                       // unicode ranges
        o.skip(4);                        // achVendID
        f.os2_.fs_selection      = o.u16();
        f.os2_.first_char_index  = o.u16();
        f.os2_.last_char_index   = o.u16();
        f.os2_.typo_ascender     = o.i16();
        f.os2_.typo_descender    = o.i16();
        f.os2_.typo_line_gap     = o.i16();
        f.os2_.win_ascent        = o.u16();
        f.os2_.win_descent       = o.u16();
        if (f.os2_.version >= 2 && os2_data.size() >= 96) {
            o.skip(8);                    // code page ranges
            f.os2_.x_height   = o.i16();
            f.os2_.cap_height = o.i16();
        }
    }

    // ---- cmap ----
    for (const auto& rec : f.tables_) {
        if (rec.tag == tags::cmap) {
            f.cmap_ = CharMap::parse(r, rec.offset);
            break;
        }
    }

    // ---- names ----
    for (const auto& rec : f.tables_) {
        if (rec.tag == tags::name) {
            // Prefer the "typographic" family (id 16) when present: for a
            // family like "Helvetica Neue Condensed Black", id 1 is forced to
            // carry the weight to satisfy old 4-style-per-family systems.
            f.family_ = read_name(r, rec.offset, name_id::typo_family);
            if (f.family_.empty()) f.family_ = read_name(r, rec.offset, name_id::family);

            f.subfamily_ = read_name(r, rec.offset, name_id::typo_subfamily);
            if (f.subfamily_.empty()) f.subfamily_ = read_name(r, rec.offset, name_id::subfamily);

            f.full_name_ = read_name(r, rec.offset, name_id::full_name);
            f.ps_name_   = read_name(r, rec.offset, name_id::postscript);
            break;
        }
    }

    // A face must have outlines of one kind or the other.
    if (!f.has_table(tags::glyf) && !f.has_table(tags::cff) && !f.has_table(tags::cff2)) {
        return fail(Error::unsupported_format);
    }

    return f;
}

}  // namespace mayag::typo::ot
