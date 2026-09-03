#pragma once
// mayag::typo::Outline — glyph geometry
//
// One representation, two very different sources:
//
//   `glyf`  TrueType quadratic B-splines, with a point-compression scheme
//           and recursive composite glyphs (é = e + ´ with a transform).
//   `CFF `  PostScript Type 2 charstrings — a stack machine with cubic
//           Béziers, subroutine calls, and implicit width encoding.
//
// Both are decoded into the same `Outline`: a list of closed contours of
// line/quad/cubic segments, in font units, y-up. Everything above this file
// (rasterisation, SDF generation, metrics) works on that one type and never
// learns which format the face used.
//
// Every parser here is bounds-checked and depth-limited. A font that claims
// a composite glyph is its own component, or a charstring that recurses
// forever, produces an empty glyph rather than a stack overflow.

#include "opentype.hpp"
#include "../core/geometry.hpp"

#include <cstdint>
#include <vector>

namespace mayag::typo {

// ════════════════════════════════════════════════════════════════════════
// Outline representation
// ════════════════════════════════════════════════════════════════════════

/// A single path command. Cubics are kept as cubics rather than being
/// pre-flattened, because the flattening tolerance depends on the pixel size
/// we are ultimately rendering at — deciding it here would either over-
/// tessellate small text or produce visible facets on large text.
struct Segment {
    enum class Kind : std::uint8_t { line, quad, cubic };

    Kind kind = Kind::line;
    Vec2 c0{};   ///< first control point  (quad: the control; cubic: control 1)
    Vec2 c1{};   ///< second control point (cubic only)
    Vec2 to{};   ///< endpoint
};

/// A closed contour. Fill uses the non-zero winding rule, so contour
/// direction determines whether it is a hole — which is why we never
/// normalise direction here.
struct Contour {
    Vec2                 start{};
    std::vector<Segment> segments;

    [[nodiscard]] bool empty() const noexcept { return segments.empty(); }

    /// Signed area x2 via the shoelace formula on endpoints. Sign gives
    /// winding direction; used to sanity-check a decoded glyph.
    [[nodiscard]] float signed_area() const noexcept {
        float a = 0.0f;
        Vec2 prev = start;
        for (const auto& s : segments) {
            a += cross(prev, s.to);
            prev = s.to;
        }
        return a + cross(prev, start);
    }
};

class Outline {
  public:
    std::vector<Contour> contours;

    /// Advance width and side bearing, in font units.
    float advance = 0.0f;
    float lsb     = 0.0f;

    [[nodiscard]] bool empty() const noexcept { return contours.empty(); }
    [[nodiscard]] std::size_t point_count() const noexcept {
        std::size_t n = 0;
        for (const auto& c : contours) n += c.segments.size();
        return n;
    }

    /// Tight bounding box of the CONTROL points. A conservative superset of
    /// the true curve bounds, which is exactly what a rasteriser wants for
    /// allocating a bitmap — never too small.
    [[nodiscard]] Rect control_bounds() const noexcept {
        if (contours.empty()) return {};
        Vec2 lo{num::inf, num::inf}, hi{-num::inf, -num::inf};
        const auto add = [&](Vec2 p) { lo = mayag::min(lo, p); hi = mayag::max(hi, p); };
        for (const auto& c : contours) {
            add(c.start);
            for (const auto& s : c.segments) {
                if (s.kind != Segment::Kind::line) add(s.c0);
                if (s.kind == Segment::Kind::cubic) add(s.c1);
                add(s.to);
            }
        }
        if (lo.x > hi.x) return {};
        return Rect::from_bounds(lo, hi);
    }

    /// Apply an affine transform in place — used by composite glyphs and by
    /// synthetic oblique.
    void transform(const Affine& m) {
        for (auto& c : contours) {
            c.start = m.apply(c.start);
            for (auto& s : c.segments) {
                s.c0 = m.apply(s.c0);
                s.c1 = m.apply(s.c1);
                s.to = m.apply(s.to);
            }
        }
    }

    void append(const Outline& other) {
        contours.insert(contours.end(), other.contours.begin(), other.contours.end());
    }
};

// ════════════════════════════════════════════════════════════════════════
// glyf — TrueType outlines
// ════════════════════════════════════════════════════════════════════════

namespace detail {

/// Point flags in a simple glyph.
inline constexpr std::uint8_t flag_on_curve  = 0x01;
inline constexpr std::uint8_t flag_x_short   = 0x02;
inline constexpr std::uint8_t flag_y_short   = 0x04;
inline constexpr std::uint8_t flag_repeat    = 0x08;
inline constexpr std::uint8_t flag_x_same    = 0x10;   ///< or positive, if short
inline constexpr std::uint8_t flag_y_same    = 0x20;

/// Composite component flags.
inline constexpr std::uint16_t comp_arg_words       = 0x0001;
inline constexpr std::uint16_t comp_args_are_xy     = 0x0002;
inline constexpr std::uint16_t comp_round_xy        = 0x0004;
inline constexpr std::uint16_t comp_has_scale       = 0x0008;
inline constexpr std::uint16_t comp_more            = 0x0020;
inline constexpr std::uint16_t comp_xy_scale        = 0x0040;
inline constexpr std::uint16_t comp_two_by_two      = 0x0080;
inline constexpr std::uint16_t comp_instructions    = 0x0100;
inline constexpr std::uint16_t comp_use_my_metrics  = 0x0200;

/// A composite glyph may reference other composites. The spec allows nesting
/// but real fonts rarely exceed 2; a malicious font could describe a cycle.
inline constexpr int max_composite_depth = 8;

}  // namespace detail

/// Reads TrueType outlines out of `glyf` + `loca`.
class GlyfSource {
  public:
    GlyfSource() = default;

    explicit GlyfSource(const ot::FontFile& f)
        : glyf_{f.table(ot::tags::glyf)},
          loca_{f.table(ot::tags::loca)},
          long_loca_{f.head().index_to_loc_format != 0},
          num_glyphs_{f.num_glyphs()} {}

    [[nodiscard]] bool valid() const noexcept { return !glyf_.empty() && !loca_.empty(); }

    /// Byte range of glyph `gid` within `glyf`. An empty range is legal and
    /// means a blank glyph (space) — not an error.
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> glyph_range(std::uint16_t gid) const noexcept {
        if (gid >= num_glyphs_) return {0, 0};
        ot::Reader r{loca_};
        std::uint32_t begin, end;
        if (long_loca_) {
            begin = r.u32_at(static_cast<std::size_t>(gid) * 4);
            end   = r.u32_at(static_cast<std::size_t>(gid) * 4 + 4);
        } else {
            // Short format stores HALF offsets, which is the classic
            // off-by-2x bug if you forget the multiply.
            begin = static_cast<std::uint32_t>(r.u16_at(static_cast<std::size_t>(gid) * 2)) * 2;
            end   = static_cast<std::uint32_t>(r.u16_at(static_cast<std::size_t>(gid) * 2 + 2)) * 2;
        }
        if (end <= begin || begin >= glyf_.size()) return {0, 0};
        return {begin, std::min<std::uint32_t>(end, static_cast<std::uint32_t>(glyf_.size()))};
    }

    [[nodiscard]] Outline load(std::uint16_t gid, int depth = 0) const {
        Outline out;
        if (!valid() || depth > detail::max_composite_depth) return out;

        const auto [begin, end] = glyph_range(gid);
        if (begin == end) return out;   // blank glyph

        ot::Reader r{glyf_, begin};
        const std::int16_t n_contours = r.i16();
        r.skip(8);   // xMin, yMin, xMax, yMax — we recompute from the outline

        if (n_contours >= 0) {
            load_simple(r, static_cast<std::uint16_t>(n_contours), out);
        } else {
            load_composite(r, out, depth);
        }
        return out;
    }

  private:
    void load_simple(ot::Reader& r, std::uint16_t n_contours, Outline& out) const {
        // ---- contour end indices ----
        std::vector<std::uint16_t> ends(n_contours);
        for (std::uint16_t i = 0; i < n_contours; ++i) ends[i] = r.u16();
        if (n_contours == 0) return;

        const std::size_t n_points = static_cast<std::size_t>(ends.back()) + 1;
        // A font can claim a point count larger than the table; refuse rather
        // than allocating gigabytes on its say-so.
        if (n_points > 0x10000 || r.bad()) return;

        // ---- skip hinting bytecode ----
        const std::uint16_t instr_len = r.u16();
        r.skip(instr_len);

        // ---- flags, run-length encoded ----
        std::vector<std::uint8_t> flags;
        flags.reserve(n_points);
        while (flags.size() < n_points && !r.bad()) {
            const std::uint8_t f = r.u8();
            flags.push_back(f);
            if (f & detail::flag_repeat) {
                const std::uint8_t rep = r.u8();
                for (std::uint8_t i = 0; i < rep && flags.size() < n_points; ++i) {
                    flags.push_back(f);
                }
            }
        }
        if (flags.size() < n_points) return;

        // ---- coordinates, delta-encoded with per-axis short/same flags ----
        std::vector<std::int32_t> xs(n_points), ys(n_points);
        std::int32_t v = 0;
        for (std::size_t i = 0; i < n_points; ++i) {
            const std::uint8_t f = flags[i];
            if (f & detail::flag_x_short) {
                const std::int32_t d = r.u8();
                v += (f & detail::flag_x_same) ? d : -d;
            } else if (!(f & detail::flag_x_same)) {
                v += r.i16();
            }
            // else: same as previous, delta 0
            xs[i] = v;
        }
        v = 0;
        for (std::size_t i = 0; i < n_points; ++i) {
            const std::uint8_t f = flags[i];
            if (f & detail::flag_y_short) {
                const std::int32_t d = r.u8();
                v += (f & detail::flag_y_same) ? d : -d;
            } else if (!(f & detail::flag_y_same)) {
                v += r.i16();
            }
            ys[i] = v;
        }

        // ---- build contours ----
        std::size_t start = 0;
        for (std::uint16_t c = 0; c < n_contours; ++c) {
            const std::size_t last = ends[c];
            if (last < start || last >= n_points) break;
            build_contour(flags, xs, ys, start, last, out);
            start = last + 1;
        }
    }

    /// Convert one run of TrueType points into a contour.
    ///
    /// TrueType allows CONSECUTIVE OFF-CURVE points, with an implied on-curve
    /// point at their midpoint. It also allows a contour to *begin* off-curve.
    /// Handling both is what separates a decoder that renders most fonts from
    /// one that renders all of them.
    static void build_contour(const std::vector<std::uint8_t>& flags,
                              const std::vector<std::int32_t>& xs,
                              const std::vector<std::int32_t>& ys,
                              std::size_t first, std::size_t last, Outline& out) {
        const std::size_t n = last - first + 1;
        if (n < 2) return;

        const auto pt = [&](std::size_t i) -> Vec2 {
            const std::size_t k = first + (i % n);
            return Vec2{static_cast<float>(xs[k]), static_cast<float>(ys[k])};
        };
        const auto on = [&](std::size_t i) -> bool {
            return (flags[first + (i % n)] & detail::flag_on_curve) != 0;
        };

        // Find a starting on-curve point. If the contour is entirely
        // off-curve (legal, and used by some CJK fonts), synthesise the start
        // at the midpoint of the last and first control points.
        std::size_t start_idx = 0;
        bool found = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (on(i)) { start_idx = i; found = true; break; }
        }

        Contour contour;
        contour.start = found ? pt(start_idx) : lerp(pt(0), pt(1), 0.5f);
        if (!found) start_idx = 0;

        Vec2 cursor  = contour.start;
        Vec2 pending{};
        bool have_pending = false;

        for (std::size_t k = 1; k <= n; ++k) {
            const std::size_t i = start_idx + k;
            const Vec2 p = pt(i);

            if (on(i)) {
                if (have_pending) {
                    contour.segments.push_back(Segment{Segment::Kind::quad, pending, {}, p});
                    have_pending = false;
                } else {
                    contour.segments.push_back(Segment{Segment::Kind::line, {}, {}, p});
                }
                cursor = p;
            } else {
                if (have_pending) {
                    // Two controls in a row: emit a quad to their midpoint.
                    const Vec2 implied = lerp(pending, p, 0.5f);
                    contour.segments.push_back(Segment{Segment::Kind::quad, pending, {}, implied});
                    cursor = implied;
                }
                pending = p;
                have_pending = true;
            }
        }

        // Close back to the start.
        if (have_pending) {
            contour.segments.push_back(Segment{Segment::Kind::quad, pending, {}, contour.start});
        } else if (cursor != contour.start) {
            contour.segments.push_back(Segment{Segment::Kind::line, {}, {}, contour.start});
        }

        if (!contour.empty()) out.contours.push_back(std::move(contour));
    }

    void load_composite(ot::Reader& r, Outline& out, int depth) const {
        std::uint16_t flags = 0;
        do {
            flags = r.u16();
            const std::uint16_t comp_gid = r.u16();

            float dx = 0.0f, dy = 0.0f;
            if (flags & detail::comp_arg_words) {
                const std::int16_t a1 = r.i16(), a2 = r.i16();
                if (flags & detail::comp_args_are_xy) { dx = a1; dy = a2; }
                // Point-matching (args are point indices) is vanishingly rare
                // and requires the parent's points; treat as no offset.
            } else {
                const std::int8_t a1 = r.i8(), a2 = r.i8();
                if (flags & detail::comp_args_are_xy) { dx = a1; dy = a2; }
            }

            float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
            if (flags & detail::comp_has_scale) {
                a = d = r.f2dot14();
            } else if (flags & detail::comp_xy_scale) {
                a = r.f2dot14();
                d = r.f2dot14();
            } else if (flags & detail::comp_two_by_two) {
                a = r.f2dot14(); b = r.f2dot14();
                c = r.f2dot14(); d = r.f2dot14();
            }

            if (comp_gid < num_glyphs_ && !r.bad()) {
                Outline sub = load(comp_gid, depth + 1);
                sub.transform(Affine{a, b, c, d, dx, dy});
                out.append(sub);
            }
        } while ((flags & detail::comp_more) && !r.bad());
    }

    std::span<const std::uint8_t> glyf_{};
    std::span<const std::uint8_t> loca_{};
    bool          long_loca_  = false;
    std::uint16_t num_glyphs_ = 0;
};

// ════════════════════════════════════════════════════════════════════════
// CFF — PostScript Type 2 charstrings
// ════════════════════════════════════════════════════════════════════════

namespace cff {

/// A CFF INDEX: a count, an offset array, and packed data. Used for the
/// charstrings, subroutines, and string tables.
struct Index {
    std::span<const std::uint8_t> data{};
    std::vector<std::uint32_t>    offsets;   ///< count+1 entries, relative to `data`
    std::size_t                   end = 0;   ///< byte just past this INDEX

    [[nodiscard]] std::uint32_t count() const noexcept {
        return offsets.empty() ? 0 : static_cast<std::uint32_t>(offsets.size() - 1);
    }

    [[nodiscard]] std::span<const std::uint8_t> at(std::uint32_t i) const noexcept {
        if (i + 1 >= offsets.size()) return {};
        const std::uint32_t b = offsets[i], e = offsets[i + 1];
        if (e <= b || e > data.size()) return {};
        return data.subspan(b, e - b);
    }
};

/// Parse an INDEX at `pos`. CFF1 uses 16-bit counts; CFF2 uses 32-bit.
[[nodiscard]] inline Index parse_index(ot::Reader& r, std::size_t pos, bool cff2 = false) {
    Index idx;
    const std::uint32_t count = cff2 ? r.u32_at(pos) : r.u16_at(pos);
    const std::size_t   hdr   = cff2 ? 4 : 2;

    if (count == 0) {
        idx.end = pos + hdr;
        return idx;
    }

    const std::uint8_t off_size = r.u8_at(pos + hdr);
    if (off_size < 1 || off_size > 4) { idx.end = pos + hdr + 1; return idx; }

    const std::size_t off_array = pos + hdr + 1;
    const auto read_off = [&](std::uint32_t i) -> std::uint32_t {
        const std::size_t p = off_array + static_cast<std::size_t>(i) * off_size;
        std::uint32_t v = 0;
        for (std::uint8_t k = 0; k < off_size; ++k) v = (v << 8) | r.u8_at(p + k);
        return v;
    };

    idx.offsets.reserve(count + 1);
    for (std::uint32_t i = 0; i <= count; ++i) {
        // CFF offsets are 1-based into the data block.
        const std::uint32_t o = read_off(i);
        idx.offsets.push_back(o == 0 ? 0 : o - 1);
    }

    const std::size_t data_start = off_array + static_cast<std::size_t>(count + 1) * off_size;
    const std::uint32_t total = idx.offsets.back();
    idx.data = r.slice(data_start, total);
    idx.end  = data_start + total;
    return idx;
}

/// A CFF DICT maps operators to operand lists. Operands are integers or
/// reals; operators are 1 or 2 bytes.
using Dict = std::vector<std::pair<std::uint16_t, std::vector<float>>>;

[[nodiscard]] inline Dict parse_dict(std::span<const std::uint8_t> d) {
    Dict dict;
    std::vector<float> operands;

    for (std::size_t i = 0; i < d.size();) {
        const std::uint8_t b0 = d[i];

        if (b0 <= 21) {                       // operator
            std::uint16_t op = b0;
            ++i;
            if (b0 == 12 && i < d.size()) { op = static_cast<std::uint16_t>(0x0C00 | d[i]); ++i; }
            dict.emplace_back(op, operands);
            operands.clear();
        }
        else if (b0 == 28) {                  // 16-bit integer
            if (i + 2 >= d.size()) break;
            operands.push_back(static_cast<std::int16_t>((d[i + 1] << 8) | d[i + 2]));
            i += 3;
        }
        else if (b0 == 29) {                  // 32-bit integer
            if (i + 4 >= d.size()) break;
            operands.push_back(static_cast<float>(static_cast<std::int32_t>(
                (static_cast<std::uint32_t>(d[i + 1]) << 24) |
                (static_cast<std::uint32_t>(d[i + 2]) << 16) |
                (static_cast<std::uint32_t>(d[i + 3]) <<  8) |
                 static_cast<std::uint32_t>(d[i + 4]))));
            i += 5;
        }
        else if (b0 == 30) {                  // real, packed BCD nibbles
            std::string s;
            ++i;
            bool done = false;
            while (i < d.size() && !done) {
                const std::uint8_t byte = d[i++];
                for (int half = 0; half < 2; ++half) {
                    const std::uint8_t nib = half == 0 ? (byte >> 4) : (byte & 0x0F);
                    if (nib <= 9)      s.push_back(static_cast<char>('0' + nib));
                    else if (nib == 0xA) s.push_back('.');
                    else if (nib == 0xB) s.push_back('E');
                    else if (nib == 0xC) s += "E-";
                    else if (nib == 0xE) s.push_back('-');
                    else if (nib == 0xF) { done = true; break; }
                }
            }
            operands.push_back(s.empty() ? 0.0f : std::strtof(s.c_str(), nullptr));
        }
        else if (b0 >= 32 && b0 <= 246) {     // small integer
            operands.push_back(static_cast<float>(static_cast<int>(b0) - 139));
            ++i;
        }
        else if (b0 >= 247 && b0 <= 250) {    // positive 2-byte
            if (i + 1 >= d.size()) break;
            operands.push_back(static_cast<float>((b0 - 247) * 256 + d[i + 1] + 108));
            i += 2;
        }
        else if (b0 >= 251 && b0 <= 254) {    // negative 2-byte
            if (i + 1 >= d.size()) break;
            operands.push_back(static_cast<float>(-(b0 - 251) * 256 - d[i + 1] - 108));
            i += 2;
        }
        else {
            ++i;                              // reserved
        }
    }
    return dict;
}

[[nodiscard]] inline const std::vector<float>* dict_get(const Dict& d, std::uint16_t op) {
    for (const auto& [k, v] : d) if (k == op) return &v;
    return nullptr;
}

/// Subroutine index bias — Type 2 subroutine numbers are offset by a value
/// that depends on how many subroutines exist.
[[nodiscard]] inline int subr_bias(std::uint32_t count) noexcept {
    if (count < 1240)  return 107;
    if (count < 33900) return 1131;
    return 32768;
}

}  // namespace cff

/// Reads CFF (PostScript) outlines by executing Type 2 charstrings.
class CffSource {
  public:
    CffSource() = default;

    explicit CffSource(const ot::FontFile& f) {
        const auto data = f.table(ot::tags::cff);
        if (data.empty()) return;
        parse(data);
        num_glyphs_ = f.num_glyphs();
    }

    [[nodiscard]] bool valid() const noexcept { return charstrings_.count() > 0; }

    [[nodiscard]] Outline load(std::uint16_t gid) const {
        Outline out;
        if (!valid() || gid >= charstrings_.count()) return out;

        Interp st{};
        st.local  = local_subrs_for(gid);
        st.global = &global_subrs_;
        st.nominal_width = nominal_width_;
        st.default_width = default_width_;

        run(charstrings_.at(gid), st, out, 0);
        close_contour(st, out);

        out.advance = st.width_set ? st.width : default_width_;
        return out;
    }

  private:
    // ── charstring interpreter state ────────────────────────────────────

    struct Interp {
        float stack[64]{};
        int   sp = 0;

        Vec2  pos{};
        bool  open = false;
        Contour current{};

        int   n_stems = 0;
        bool  width_set = false;
        float width = 0.0f;
        float nominal_width = 0.0f;
        float default_width = 0.0f;

        float transient[32]{};

        const cff::Index* local  = nullptr;
        const cff::Index* global = nullptr;

        void push(float v) noexcept { if (sp < 64) stack[sp++] = v; }
        void clear() noexcept { sp = 0; }
    };

    static void close_contour(Interp& st, Outline& out) {
        if (st.open && !st.current.empty()) {
            // Charstring contours are implicitly closed.
            if (st.current.segments.back().to != st.current.start) {
                st.current.segments.push_back(
                    Segment{Segment::Kind::line, {}, {}, st.current.start});
            }
            out.contours.push_back(std::move(st.current));
        }
        st.current = Contour{};
        st.open = false;
    }

    static void move_to(Interp& st, Outline& out, Vec2 p) {
        close_contour(st, out);
        st.current.start = p;
        st.pos = p;
        st.open = true;
    }

    static void line_to(Interp& st, Vec2 p) {
        if (!st.open) return;
        st.current.segments.push_back(Segment{Segment::Kind::line, {}, {}, p});
        st.pos = p;
    }

    static void curve_to(Interp& st, Vec2 c1, Vec2 c2, Vec2 p) {
        if (!st.open) return;
        st.current.segments.push_back(Segment{Segment::Kind::cubic, c1, c2, p});
        st.pos = p;
    }

    /// The odd-argument-count rule: several operators may be preceded by an
    /// extra leading operand that is the glyph's advance width. It is only
    /// present on the FIRST such operator, and only when the count is odd
    /// (or, for endchar/hstem, even+1). Getting this wrong shifts every
    /// coordinate in the glyph by one stack slot.
    static void maybe_width(Interp& st, int even_args) {
        if (!st.width_set) {
            if (st.sp > even_args && ((st.sp - even_args) % 2) == 1) {
                st.width = st.nominal_width + st.stack[0];
            } else if (st.sp % 2 == 1 && even_args < 0) {
                st.width = st.nominal_width + st.stack[0];
            } else {
                st.width = st.default_width;
            }
            st.width_set = true;
        }
    }

    void run(std::span<const std::uint8_t> code, Interp& st, Outline& out, int depth) const {
        if (depth > 10) return;   // subroutine recursion guard

        for (std::size_t i = 0; i < code.size();) {
            const std::uint8_t b0 = code[i];

            // ---- operands ----
            if (b0 >= 32 || b0 == 28) {
                if (b0 == 28) {
                    if (i + 2 >= code.size()) return;
                    st.push(static_cast<std::int16_t>((code[i + 1] << 8) | code[i + 2]));
                    i += 3;
                } else if (b0 <= 246) {
                    st.push(static_cast<float>(static_cast<int>(b0) - 139));
                    ++i;
                } else if (b0 <= 250) {
                    if (i + 1 >= code.size()) return;
                    st.push(static_cast<float>((b0 - 247) * 256 + code[i + 1] + 108));
                    i += 2;
                } else if (b0 <= 254) {
                    if (i + 1 >= code.size()) return;
                    st.push(static_cast<float>(-(b0 - 251) * 256 - code[i + 1] - 108));
                    i += 2;
                } else {   // 255: 16.16 fixed
                    if (i + 4 >= code.size()) return;
                    const std::int32_t v =
                        (static_cast<std::uint32_t>(code[i + 1]) << 24) |
                        (static_cast<std::uint32_t>(code[i + 2]) << 16) |
                        (static_cast<std::uint32_t>(code[i + 3]) <<  8) |
                         static_cast<std::uint32_t>(code[i + 4]);
                    st.push(static_cast<float>(v) / 65536.0f);
                    i += 5;
                }
                continue;
            }

            ++i;   // consume the operator byte

            switch (b0) {
                // ---- hints: we do not hint, but must consume the width ----
                case 1: case 3: case 18: case 23:            // h/vstem, h/vstemhm
                    maybe_width(st, 0);
                    st.n_stems += st.sp / 2;
                    st.clear();
                    break;

                case 19: case 20: {                           // hintmask, cntrmask
                    maybe_width(st, 0);
                    st.n_stems += st.sp / 2;
                    st.clear();
                    // The mask is (n_stems + 7) / 8 bytes of inline data.
                    i += static_cast<std::size_t>((st.n_stems + 7) / 8);
                    break;
                }

                // ---- path construction ----
                case 21: {                                    // rmoveto
                    maybe_width(st, 2);
                    const int base = st.sp > 2 ? st.sp - 2 : 0;
                    if (st.sp >= 2) {
                        move_to(st, out, st.pos + Vec2{st.stack[base], st.stack[base + 1]});
                    }
                    st.clear();
                    break;
                }
                case 22: {                                    // hmoveto
                    maybe_width(st, 1);
                    const int base = st.sp > 1 ? st.sp - 1 : 0;
                    if (st.sp >= 1) move_to(st, out, st.pos + Vec2{st.stack[base], 0.0f});
                    st.clear();
                    break;
                }
                case 4: {                                     // vmoveto
                    maybe_width(st, 1);
                    const int base = st.sp > 1 ? st.sp - 1 : 0;
                    if (st.sp >= 1) move_to(st, out, st.pos + Vec2{0.0f, st.stack[base]});
                    st.clear();
                    break;
                }

                case 5:                                       // rlineto
                    for (int k = 0; k + 1 < st.sp; k += 2) {
                        line_to(st, st.pos + Vec2{st.stack[k], st.stack[k + 1]});
                    }
                    st.clear();
                    break;

                case 6: case 7: {                             // hlineto, vlineto
                    bool horizontal = (b0 == 6);
                    for (int k = 0; k < st.sp; ++k) {
                        line_to(st, st.pos + (horizontal ? Vec2{st.stack[k], 0.0f}
                                                         : Vec2{0.0f, st.stack[k]}));
                        horizontal = !horizontal;
                    }
                    st.clear();
                    break;
                }

                case 8:                                       // rrcurveto
                    for (int k = 0; k + 5 < st.sp; k += 6) rrcurve(st, &st.stack[k]);
                    st.clear();
                    break;

                case 24: {                                    // rcurveline
                    int k = 0;
                    for (; k + 5 < st.sp - 2; k += 6) rrcurve(st, &st.stack[k]);
                    if (k + 1 < st.sp) line_to(st, st.pos + Vec2{st.stack[k], st.stack[k + 1]});
                    st.clear();
                    break;
                }

                case 25: {                                    // rlinecurve
                    int k = 0;
                    for (; k + 1 < st.sp - 6; k += 2) {
                        line_to(st, st.pos + Vec2{st.stack[k], st.stack[k + 1]});
                    }
                    if (k + 5 < st.sp) rrcurve(st, &st.stack[k]);
                    st.clear();
                    break;
                }

                case 26: case 27: {                           // vvcurveto, hhcurveto
                    int k = 0;
                    float d1 = 0.0f;
                    if (st.sp % 4 == 1) { d1 = st.stack[0]; k = 1; }
                    const bool vertical = (b0 == 26);
                    for (; k + 3 < st.sp; k += 4) {
                        const Vec2 c1 = st.pos + (vertical ? Vec2{d1, st.stack[k]}
                                                           : Vec2{st.stack[k], d1});
                        const Vec2 c2 = c1 + Vec2{st.stack[k + 1], st.stack[k + 2]};
                        const Vec2 to = c2 + (vertical ? Vec2{0.0f, st.stack[k + 3]}
                                                       : Vec2{st.stack[k + 3], 0.0f});
                        curve_to(st, c1, c2, to);
                        d1 = 0.0f;
                    }
                    st.clear();
                    break;
                }

                case 30: case 31: {                           // vhcurveto, hvcurveto
                    bool horizontal = (b0 == 31);
                    int k = 0;
                    while (k + 3 < st.sp) {
                        const bool last = (st.sp - k == 5);
                        Vec2 c1, c2, to;
                        if (horizontal) {
                            c1 = st.pos + Vec2{st.stack[k], 0.0f};
                            c2 = c1 + Vec2{st.stack[k + 1], st.stack[k + 2]};
                            to = c2 + Vec2{last ? st.stack[k + 4] : 0.0f, st.stack[k + 3]};
                        } else {
                            c1 = st.pos + Vec2{0.0f, st.stack[k]};
                            c2 = c1 + Vec2{st.stack[k + 1], st.stack[k + 2]};
                            to = c2 + Vec2{st.stack[k + 3], last ? st.stack[k + 4] : 0.0f};
                        }
                        curve_to(st, c1, c2, to);
                        horizontal = !horizontal;
                        k += 4;
                    }
                    st.clear();
                    break;
                }

                // ---- subroutines ----
                case 10: {                                    // callsubr
                    if (st.sp > 0 && st.local != nullptr) {
                        const int idx = static_cast<int>(st.stack[--st.sp]) +
                                        cff::subr_bias(st.local->count());
                        if (idx >= 0 && static_cast<std::uint32_t>(idx) < st.local->count()) {
                            run(st.local->at(static_cast<std::uint32_t>(idx)), st, out, depth + 1);
                        }
                    }
                    break;
                }
                case 29: {                                    // callgsubr
                    if (st.sp > 0 && st.global != nullptr) {
                        const int idx = static_cast<int>(st.stack[--st.sp]) +
                                        cff::subr_bias(st.global->count());
                        if (idx >= 0 && static_cast<std::uint32_t>(idx) < st.global->count()) {
                            run(st.global->at(static_cast<std::uint32_t>(idx)), st, out, depth + 1);
                        }
                    }
                    break;
                }
                case 11:                                      // return
                    return;

                case 14: {                                    // endchar
                    maybe_width(st, 0);
                    close_contour(st, out);
                    return;
                }

                // ---- flex and arithmetic (escape) ----
                case 12: {
                    if (i >= code.size()) return;
                    const std::uint8_t b1 = code[i++];
                    handle_escape(b1, st);
                    break;
                }

                default:
                    st.clear();
                    break;
            }
        }
    }

    static void rrcurve(Interp& st, const float* a) {
        const Vec2 c1 = st.pos + Vec2{a[0], a[1]};
        const Vec2 c2 = c1 + Vec2{a[2], a[3]};
        const Vec2 to = c2 + Vec2{a[4], a[5]};
        curve_to(st, c1, c2, to);
    }

    /// 12 xx operators. The flex family draws two curves that are nearly a
    /// straight line; we render them as the two curves they describe, which
    /// is exactly what a non-hinting rasteriser should do.
    static void handle_escape(std::uint8_t op, Interp& st) {
        switch (op) {
            case 35: {                                        // flex
                if (st.sp >= 13) {
                    rrcurve(st, &st.stack[0]);
                    rrcurve(st, &st.stack[6]);
                }
                st.clear();
                break;
            }
            case 34: {                                        // hflex
                if (st.sp >= 7) {
                    const float y = st.pos.y;
                    const Vec2 c1 = st.pos + Vec2{st.stack[0], 0.0f};
                    const Vec2 c2 = c1 + Vec2{st.stack[1], st.stack[2]};
                    const Vec2 p1 = c2 + Vec2{st.stack[3], 0.0f};
                    curve_to(st, c1, c2, p1);
                    const Vec2 c3 = st.pos + Vec2{st.stack[4], 0.0f};
                    const Vec2 c4 = c3 + Vec2{st.stack[5], y - c3.y};
                    const Vec2 p2 = c4 + Vec2{st.stack[6], 0.0f};
                    curve_to(st, c3, c4, Vec2{p2.x, y});
                }
                st.clear();
                break;
            }
            case 36: {                                        // hflex1
                if (st.sp >= 9) {
                    const float y = st.pos.y;
                    const Vec2 c1 = st.pos + Vec2{st.stack[0], st.stack[1]};
                    const Vec2 c2 = c1 + Vec2{st.stack[2], st.stack[3]};
                    const Vec2 p1 = c2 + Vec2{st.stack[4], 0.0f};
                    curve_to(st, c1, c2, p1);
                    const Vec2 c3 = st.pos + Vec2{st.stack[5], 0.0f};
                    const Vec2 c4 = c3 + Vec2{st.stack[6], st.stack[7]};
                    const Vec2 p2 = c4 + Vec2{st.stack[8], y - c4.y};
                    curve_to(st, c3, c4, Vec2{p2.x, y});
                }
                st.clear();
                break;
            }
            case 37: {                                        // flex1
                if (st.sp >= 11) {
                    const Vec2 start = st.pos;
                    float dx = 0.0f, dy = 0.0f;
                    for (int k = 0; k < 10; k += 2) { dx += st.stack[k]; dy += st.stack[k + 1]; }
                    rrcurve(st, &st.stack[0]);
                    const Vec2 c3 = st.pos + Vec2{st.stack[6], st.stack[7]};
                    const Vec2 c4 = c3 + Vec2{st.stack[8], st.stack[9]};
                    // The final point returns to the start plus the total delta.
                    const Vec2 to = start + Vec2{dx, dy} + Vec2{st.stack[10], 0.0f};
                    curve_to(st, c3, c4, to);
                }
                st.clear();
                break;
            }
            default:
                st.clear();
                break;
        }
    }

    // ── CFF container parsing ───────────────────────────────────────────

    void parse(std::span<const std::uint8_t> data) {
        ot::Reader r{data};

        const std::uint8_t hdr_size = r.u8_at(2);
        std::size_t pos = hdr_size;

        const cff::Index names = cff::parse_index(r, pos);      pos = names.end;
        const cff::Index top   = cff::parse_index(r, pos);      pos = top.end;
        const cff::Index strs  = cff::parse_index(r, pos);      pos = strs.end;
        global_subrs_ = cff::parse_index(r, pos);
        // Keep the backing data alive by copying the span's view; `data` is
        // owned by the FontFile's blob, which outlives us.
        global_data_ = data;

        if (top.count() == 0) return;
        const auto top_dict = cff::parse_dict(top.at(0));

        // CharStrings offset (op 17)
        if (const auto* cs = cff::dict_get(top_dict, 17); cs != nullptr && !cs->empty()) {
            charstrings_ = cff::parse_index(r, static_cast<std::size_t>((*cs)[0]));
        }

        // Private DICT (op 18): [size, offset] -> local subrs + widths
        if (const auto* pv = cff::dict_get(top_dict, 18); pv != nullptr && pv->size() >= 2) {
            const auto size = static_cast<std::size_t>((*pv)[0]);
            const auto off  = static_cast<std::size_t>((*pv)[1]);
            const auto priv = cff::parse_dict(r.slice(off, size));

            if (const auto* dw = cff::dict_get(priv, 20); dw && !dw->empty()) default_width_ = (*dw)[0];
            if (const auto* nw = cff::dict_get(priv, 21); nw && !nw->empty()) nominal_width_ = (*nw)[0];
            if (const auto* ls = cff::dict_get(priv, 19); ls && !ls->empty()) {
                local_subrs_ = cff::parse_index(r, off + static_cast<std::size_t>((*ls)[0]));
            }
        }

        // CID-keyed fonts (op 12 30 = ROS) put subrs per FD; those need the
        // FDArray/FDSelect pair. Handled by `local_subrs_for`.
        if (cff::dict_get(top_dict, 0x0C1E) != nullptr) {
            is_cid_ = true;
            if (const auto* fda = cff::dict_get(top_dict, 0x0C24); fda && !fda->empty()) {
                fd_array_ = cff::parse_index(r, static_cast<std::size_t>((*fda)[0]));
                fd_privs_.resize(fd_array_.count());
                for (std::uint32_t k = 0; k < fd_array_.count(); ++k) {
                    const auto fd = cff::parse_dict(fd_array_.at(k));
                    if (const auto* pv = cff::dict_get(fd, 18); pv && pv->size() >= 2) {
                        const auto sz = static_cast<std::size_t>((*pv)[0]);
                        const auto of = static_cast<std::size_t>((*pv)[1]);
                        const auto priv = cff::parse_dict(r.slice(of, sz));
                        if (const auto* ls = cff::dict_get(priv, 19); ls && !ls->empty()) {
                            fd_privs_[k] = cff::parse_index(r, of + static_cast<std::size_t>((*ls)[0]));
                        }
                    }
                }
            }
            if (const auto* fds = cff::dict_get(top_dict, 0x0C25); fds && !fds->empty()) {
                fd_select_offset_ = static_cast<std::size_t>((*fds)[0]);
            }
            reader_ = r;
        }
    }

    /// Which local subroutine index applies to this glyph. For a CID font
    /// that depends on FDSelect; for a plain font there is only one.
    [[nodiscard]] const cff::Index* local_subrs_for(std::uint16_t gid) const {
        if (!is_cid_ || fd_privs_.empty()) return &local_subrs_;

        // FDSelect maps glyph -> font DICT index, in one of two formats.
        const std::uint8_t fmt = reader_.u8_at(fd_select_offset_);
        std::uint32_t fd = 0;
        if (fmt == 0) {
            fd = reader_.u8_at(fd_select_offset_ + 1 + gid);
        } else if (fmt == 3) {
            const std::uint16_t n_ranges = reader_.u16_at(fd_select_offset_ + 1);
            for (std::uint16_t k = 0; k < n_ranges; ++k) {
                const std::size_t rec = fd_select_offset_ + 3 + k * 3;
                const std::uint16_t first = reader_.u16_at(rec);
                const std::uint16_t next  = reader_.u16_at(rec + 3);
                if (gid >= first && gid < next) { fd = reader_.u8_at(rec + 2); break; }
            }
        }
        return fd < fd_privs_.size() ? &fd_privs_[fd] : &local_subrs_;
    }

    std::span<const std::uint8_t> global_data_{};
    ot::Reader     reader_{};
    cff::Index     charstrings_{};
    cff::Index     global_subrs_{};
    cff::Index     local_subrs_{};
    cff::Index     fd_array_{};
    std::vector<cff::Index> fd_privs_;
    std::size_t    fd_select_offset_ = 0;
    float          default_width_ = 0.0f;
    float          nominal_width_ = 0.0f;
    std::uint16_t  num_glyphs_ = 0;
    bool           is_cid_ = false;
};

// ════════════════════════════════════════════════════════════════════════
// GlyphSource — the format-agnostic front door
// ════════════════════════════════════════════════════════════════════════

/// Loads outlines from whichever format the face uses. Everything downstream
/// depends on THIS, not on glyf/CFF, which is why adding CFF2 or SVG glyphs
/// later will not ripple.
class GlyphSource {
  public:
    GlyphSource() = default;

    explicit GlyphSource(const ot::FontFile& f) : upem_{f.units_per_em()} {
        if (f.is_cff()) {
            cff_ = CffSource{f};
            kind_ = cff_.valid() ? Kind::cff : Kind::none;
        } else {
            glyf_ = GlyfSource{f};
            kind_ = glyf_.valid() ? Kind::glyf : Kind::none;
        }
    }

    [[nodiscard]] bool valid() const noexcept { return kind_ != Kind::none; }
    [[nodiscard]] std::uint16_t units_per_em() const noexcept { return upem_; }

    [[nodiscard]] Outline load(std::uint16_t gid) const {
        switch (kind_) {
            case Kind::glyf: return glyf_.load(gid);
            case Kind::cff:  return cff_.load(gid);
            case Kind::none: break;
        }
        return {};
    }

  private:
    enum class Kind : std::uint8_t { none, glyf, cff };

    Kind          kind_ = Kind::none;
    GlyfSource    glyf_;
    CffSource     cff_;
    std::uint16_t upem_ = 1000;
};

}  // namespace mayag::typo
