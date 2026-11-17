#pragma once
// mayag::typo::var — OpenType Font Variations (fvar / gvar / avar)
//
// A variable font is one file that spans a continuous design space: weight,
// width, optical size, slant — each an AXIS with a range. Instead of shipping
// Regular / Medium / Bold / Black as four files, the font stores one master
// outline plus DELTAS, and any weight (say 437) is an interpolation. This is
// what CoreText, DirectWrite and Chrome do; almost no from-scratch engine
// does, and it is the difference between "renders the default instance" and
// "renders the font".
//
// The pipeline mayag implements here is the TrueType one (gvar over `glyf`):
//
//   1. fvar  — the axes: tag, min/default/max in user units.
//   2. avar  — an optional per-axis remapping of the user->normalised curve.
//   3. gvar  — per-glyph tuple variation store: for each region of the design
//              space, a set of point deltas; the live outline is the base
//              points plus the sum of each region's deltas scaled by how far
//              the requested coordinate is into that region.
//   4. IUP   — deltas are stored only for some points; the rest are inferred
//              by "interpolation of un-referenced points" from their
//              on-contour neighbours, the same rule the spec mandates.
//
// This file is pure parsing + math over spans; it never allocates a GPU
// resource and knows nothing about rendering. GlyphSource calls
// `apply_gvar` after decoding a glyph's base points.

#include "opentype.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mayag::typo::var {

/// One design axis, from fvar. Values are in USER units (e.g. weight 100..900).
struct Axis {
    std::uint32_t tag = 0;
    float min = 0.0f, def = 0.0f, max = 0.0f;
};

/// The five OpenType-registered axis tags, as big-endian 4CCs. Fonts may
/// expose custom axes too; these are just the common ones for convenience.
inline constexpr std::uint32_t tag_wght = 0x77676874;  // 'wght' weight
inline constexpr std::uint32_t tag_wdth = 0x77647468;  // 'wdth' width
inline constexpr std::uint32_t tag_opsz = 0x6F70737A;  // 'opsz' optical size
inline constexpr std::uint32_t tag_ital = 0x6974616C;  // 'ital' italic
inline constexpr std::uint32_t tag_slnt = 0x736C6E74;  // 'slnt' slant

/// A point delta in font units.
struct Delta { float x = 0.0f, y = 0.0f; };

namespace detail {

inline std::uint16_t u16(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    return o + 2 <= d.size() ? static_cast<std::uint16_t>((d[o] << 8) | d[o + 1]) : 0;
}
inline std::int16_t s16(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    return static_cast<std::int16_t>(u16(d, o));
}
inline std::uint32_t u32(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    if (o + 4 > d.size()) return 0;
    return (static_cast<std::uint32_t>(d[o]) << 24) | (static_cast<std::uint32_t>(d[o + 1]) << 16) |
           (static_cast<std::uint32_t>(d[o + 2]) << 8) | static_cast<std::uint32_t>(d[o + 3]);
}
/// 2.14 fixed point (used for normalised axis coordinates and tuples).
inline float f2dot14(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    return static_cast<float>(s16(d, o)) / 16384.0f;
}

}  // namespace detail

// ── fvar: the axes ─────────────────────────────────────────────────────────

[[nodiscard]] inline std::vector<Axis> parse_fvar(std::span<const std::uint8_t> fvar) {
    using namespace detail;
    std::vector<Axis> axes;
    if (fvar.size() < 16) return axes;
    const std::uint16_t axes_off   = u16(fvar, 4);
    const std::uint16_t axis_count = u16(fvar, 8);
    const std::uint16_t axis_size  = u16(fvar, 10);
    for (std::uint16_t i = 0; i < axis_count; ++i) {
        const std::size_t o = axes_off + static_cast<std::size_t>(i) * axis_size;
        if (o + 20 > fvar.size()) break;
        Axis a;
        a.tag = u32(fvar, o);
        a.min = static_cast<float>(static_cast<std::int32_t>(u32(fvar, o + 4))) / 65536.0f;
        a.def = static_cast<float>(static_cast<std::int32_t>(u32(fvar, o + 8))) / 65536.0f;
        a.max = static_cast<float>(static_cast<std::int32_t>(u32(fvar, o + 12))) / 65536.0f;
        axes.push_back(a);
    }
    return axes;
}

/// Normalise a user coordinate on one axis to [-1, 1] per the fvar rule, then
/// (if avar is present) remap it through the axis's segment list.
[[nodiscard]] inline float normalize_axis(const Axis& a, float user,
                                          std::span<const std::uint8_t> avar,
                                          std::size_t axis_index) {
    if (user < a.min) user = a.min;
    if (user > a.max) user = a.max;
    float n;
    if (user < a.def)      n = (a.def - user) != 0.0f ? -(a.def - user) / (a.def - a.min) : 0.0f;
    else if (user > a.def) n =  (user - a.def) != 0.0f ?  (user - a.def) / (a.max - a.def) : 0.0f;
    else                   n = 0.0f;
    if (n < -1.0f) n = -1.0f;
    if (n > 1.0f)  n = 1.0f;

    // avar: piecewise-linear remap of n via this axis's <fromCoord,toCoord>
    // segment map. Absent or too short => identity.
    using namespace detail;
    if (avar.size() >= 8) {
        const std::uint16_t axis_count = u16(avar, 6);
        if (axis_index < axis_count) {
            std::size_t o = 8;
            for (std::size_t ax = 0; ax < axis_count; ++ax) {
                const std::uint16_t pairs = u16(avar, o);
                o += 2;
                if (ax == axis_index) {
                    float prev_from = -1.0f, prev_to = -1.0f;
                    for (std::uint16_t p = 0; p < pairs; ++p) {
                        const float from = f2dot14(avar, o + p * 4);
                        const float to   = f2dot14(avar, o + p * 4 + 2);
                        if (n <= from) {
                            if (from == prev_from) return to;
                            const float t = (n - prev_from) / (from - prev_from);
                            return prev_to + t * (to - prev_to);
                        }
                        prev_from = from; prev_to = to;
                    }
                    return prev_to;   // past the last segment
                }
                o += static_cast<std::size_t>(pairs) * 4;
            }
        }
    }
    return n;
}

/// Inverse of normalize_axis's fvar step: map a normalised [-1,1] coordinate
/// back to USER units. avar (if present) is NOT un-applied — a segment map is
/// not generally invertible — so this round-trips exactly for the common
/// avar-free font and is a close approximation otherwise. Used only to read
/// the current per-axis value back out; rendering never calls it.
[[nodiscard]] inline float denormalize_axis(const Axis& a, float n) noexcept {
    if (n < 0.0f) return a.def + n * (a.def - a.min);
    if (n > 0.0f) return a.def + n * (a.max - a.def);
    return a.def;
}

/// The full set of normalised coordinates for a font, one per axis.
using Coords = std::vector<float>;

[[nodiscard]] inline Coords normalize_all(const std::vector<Axis>& axes,
                                          std::span<const std::uint8_t> avar,
                                          const std::vector<std::pair<std::uint32_t, float>>& user) {
    Coords c(axes.size(), 0.0f);
    for (std::size_t i = 0; i < axes.size(); ++i) {
        float u = axes[i].def;
        for (const auto& [tag, val] : user) if (tag == axes[i].tag) u = val;
        c[i] = normalize_axis(axes[i], u, avar, i);
    }
    return c;
}

// ── gvar: per-glyph point deltas ────────────────────────────────────────────

namespace detail {

// gvar tuple flags.
inline constexpr std::uint16_t embedded_peak    = 0x8000;
inline constexpr std::uint16_t intermediate     = 0x4000;
inline constexpr std::uint16_t private_points    = 0x2000;
inline constexpr std::uint16_t tuple_index_mask = 0x0FFF;
inline constexpr std::uint16_t shared_point_nums = 0x8000;   // in tupleVariationCount
inline constexpr std::uint16_t count_mask       = 0x0FFF;

// Packed point-number run flags.
inline constexpr std::uint8_t points_are_words = 0x80;
inline constexpr std::uint8_t point_run_mask   = 0x7F;
// Packed delta run flags.
inline constexpr std::uint8_t deltas_are_zero  = 0x80;
inline constexpr std::uint8_t deltas_are_words = 0x40;
inline constexpr std::uint8_t delta_run_mask   = 0x3F;

/// Read a packed point-number set (gvar/cvar shared format). `pos` advances.
/// Returns the explicit point indices; an empty result with all_points=true
/// means "all points in the glyph".
inline std::vector<std::uint16_t> read_point_numbers(std::span<const std::uint8_t> d,
                                                     std::size_t& pos, bool& all_points) {
    std::vector<std::uint16_t> pts;
    all_points = false;
    if (pos >= d.size()) return pts;
    std::uint16_t count = d[pos++];
    if (count & 0x80) {
        if (pos >= d.size()) return pts;
        count = static_cast<std::uint16_t>(((count & 0x7F) << 8) | d[pos++]);
    }
    if (count == 0) { all_points = true; return pts; }

    std::uint16_t point = 0;
    while (pts.size() < count && pos < d.size()) {
        const std::uint8_t control = d[pos++];
        const std::uint16_t run = static_cast<std::uint16_t>((control & point_run_mask) + 1);
        if (control & points_are_words) {
            for (std::uint16_t i = 0; i < run && pts.size() < count && pos + 1 < d.size(); ++i) {
                point = static_cast<std::uint16_t>(point + ((d[pos] << 8) | d[pos + 1]));
                pos += 2;
                pts.push_back(point);
            }
        } else {
            for (std::uint16_t i = 0; i < run && pts.size() < count && pos < d.size(); ++i) {
                point = static_cast<std::uint16_t>(point + d[pos++]);
                pts.push_back(point);
            }
        }
    }
    return pts;
}

/// Read `count` packed deltas (gvar delta run format). `pos` advances.
inline std::vector<std::int32_t> read_packed_deltas(std::span<const std::uint8_t> d,
                                                    std::size_t& pos, std::size_t count) {
    std::vector<std::int32_t> out;
    out.reserve(count);
    while (out.size() < count && pos < d.size()) {
        const std::uint8_t control = d[pos++];
        const std::uint16_t run = static_cast<std::uint16_t>((control & delta_run_mask) + 1);
        if (control & deltas_are_zero) {
            for (std::uint16_t i = 0; i < run && out.size() < count; ++i) out.push_back(0);
        } else if (control & deltas_are_words) {
            for (std::uint16_t i = 0; i < run && out.size() < count && pos + 1 < d.size(); ++i) {
                out.push_back(static_cast<std::int16_t>((d[pos] << 8) | d[pos + 1]));
                pos += 2;
            }
        } else {
            for (std::uint16_t i = 0; i < run && out.size() < count && pos < d.size(); ++i) {
                out.push_back(static_cast<std::int8_t>(d[pos++]));
            }
        }
    }
    return out;
}

/// The scalar contribution of one tuple (region) at the requested coords: the
/// product over axes of how far `coord` is into [start,peak,end]. 0 outside.
inline float tuple_scalar(const std::vector<float>& peak,
                          const std::vector<float>& start,
                          const std::vector<float>& end,
                          const Coords& coords, bool has_intermediate) {
    float scalar = 1.0f;
    for (std::size_t a = 0; a < peak.size() && a < coords.size(); ++a) {
        const float p = peak[a];
        if (p == 0.0f) continue;                 // axis not part of this region
        const float c = coords[a];
        if (c == 0.0f) return 0.0f;
        if (!has_intermediate) {
            if ((c < 0.0f) != (p < 0.0f)) return 0.0f;   // wrong side
            if (std::abs(c) > std::abs(p)) { /* beyond peak: full */ scalar *= 1.0f; }
            else scalar *= c / p;
        } else {
            const float s = start[a], e = end[a];
            if (c < s || c > e) return 0.0f;
            if (c < p)      scalar *= (p - s) != 0.0f ? (c - s) / (p - s) : 0.0f;
            else if (c > p) scalar *= (e - p) != 0.0f ? (e - c) / (e - p) : 0.0f;
            // c == p => *1
        }
    }
    return scalar;
}

}  // namespace detail

// ── IUP: infer deltas for un-referenced points ─────────────────────────────
//
// Points without an explicit delta are moved so they keep their relationship
// to the two nearest touched points on the same contour: linear interpolation
// if the point's coordinate is between them, translation by the nearer one's
// delta if outside. Applied per axis, per contour, exactly as the spec says.

/// Interpolate the delta of one untouched point on one axis, given its coord
/// `cp` and the two touched anchors (coords `ca`,`cb`; deltas `da`,`db`).
[[nodiscard]] inline float iup_interp(float cp, float ca, float da, float cb, float db) {
    float lo = ca, hi = cb, dlo = da, dhi = db;
    if (lo > hi) { float tc = lo; lo = hi; hi = tc; float td = dlo; dlo = dhi; dhi = td; }
    if (cp <= lo) return dlo;
    if (cp >= hi) return dhi;
    if (hi == lo) return dlo;
    return dlo + (dhi - dlo) * (cp - lo) / (hi - lo);
}

inline void iup(std::vector<Delta>& deltas, const std::vector<bool>& touched,
                const std::vector<float>& xs, const std::vector<float>& ys,
                const std::vector<std::uint16_t>& ends) {
    std::size_t start = 0;
    for (std::uint16_t last : ends) {
        const std::size_t end = last;
        if (end >= deltas.size() || end < start) break;
        const std::size_t n = end - start + 1;

        std::vector<std::size_t> t;
        for (std::size_t i = start; i <= end; ++i) if (touched[i]) t.push_back(i);

        // Nothing to infer when the contour is fully touched or fully absent.
        if (!t.empty() && t.size() < n) {
            for (std::size_t k = 0; k < t.size(); ++k) {
                const std::size_t a = t[k];
                const std::size_t b = t[(k + 1) % t.size()];
                std::size_t i = (a == end ? start : a + 1);
                while (i != b) {
                    deltas[i].x = iup_interp(xs[i], xs[a], deltas[a].x, xs[b], deltas[b].x);
                    deltas[i].y = iup_interp(ys[i], ys[a], deltas[a].y, ys[b], deltas[b].y);
                    i = (i == end ? start : i + 1);
                }
            }
        }
        start = end + 1;
    }
}

/// Locate a glyph's gvar data range within the gvar table.
struct GvarHeader {
    std::uint16_t axis_count = 0;
    std::uint16_t shared_tuple_count = 0;
    std::uint32_t shared_tuples_off = 0;
    std::uint32_t glyph_count = 0;
    bool          long_offsets = false;
    std::uint32_t data_array_off = 0;
    std::size_t   offsets_off = 0;
    bool          ok = false;
};

[[nodiscard]] inline GvarHeader parse_gvar_header(std::span<const std::uint8_t> gvar) {
    using namespace detail;
    GvarHeader h;
    if (gvar.size() < 20) return h;
    h.axis_count          = u16(gvar, 4);
    h.shared_tuple_count  = u16(gvar, 6);
    h.shared_tuples_off   = u32(gvar, 8);
    h.glyph_count         = u16(gvar, 12);
    const std::uint16_t flags = u16(gvar, 14);
    h.long_offsets        = (flags & 1) != 0;
    h.data_array_off      = u32(gvar, 16);
    h.offsets_off         = 20;
    h.ok = true;
    return h;
}

/// Apply gvar variation to a glyph's decoded points, in place.
///
/// `xs`/`ys` are the base outline points (all points, including the phantom
/// points a full implementation appends; here we vary the contour points and
/// leave metrics to HVAR). `contour_ends` are the last-point index of each
/// contour, needed for IUP. `coords` are the normalised axis coordinates.
inline void apply_gvar(std::span<const std::uint8_t> gvar, const GvarHeader& h,
                       std::uint16_t gid, const Coords& coords,
                       std::vector<float>& xs, std::vector<float>& ys,
                       const std::vector<std::uint16_t>& contour_ends) {
    using namespace detail;
    if (!h.ok || gid >= h.glyph_count || coords.empty()) return;
    const std::size_t n_points = xs.size();
    if (n_points == 0) return;

    // Glyph data range from the offsets array.
    std::uint32_t start, next;
    if (h.long_offsets) {
        start = u32(gvar, h.offsets_off + static_cast<std::size_t>(gid) * 4);
        next  = u32(gvar, h.offsets_off + static_cast<std::size_t>(gid + 1) * 4);
    } else {
        start = static_cast<std::uint32_t>(u16(gvar, h.offsets_off + static_cast<std::size_t>(gid) * 2)) * 2;
        next  = static_cast<std::uint32_t>(u16(gvar, h.offsets_off + static_cast<std::size_t>(gid + 1) * 2)) * 2;
    }
    if (next <= start) return;   // no variation data for this glyph
    const std::size_t gdata = h.data_array_off + start;
    if (gdata + 4 > gvar.size()) return;

    const std::uint16_t tuple_count_raw = u16(gvar, gdata);
    const std::uint16_t tuple_count = tuple_count_raw & count_mask;
    const std::uint16_t data_offset = u16(gvar, gdata + 2);
    std::size_t serialized = gdata + data_offset;   // where packed points+deltas begin
    std::size_t hpos = gdata + 4;                    // tuple headers

    // Optional shared point numbers, at the front of the serialized data.
    std::vector<std::uint16_t> shared_points;
    bool shared_all = false;
    if (tuple_count_raw & shared_point_nums) {
        std::size_t sp = serialized;
        shared_points = read_point_numbers(gvar, sp, shared_all);
        serialized = sp;
    }

    // Accumulate deltas across all tuples (regions), weighted by their scalar.
    std::vector<float> dx(n_points, 0.0f), dy(n_points, 0.0f);

    for (std::uint16_t t = 0; t < tuple_count; ++t) {
        if (hpos + 4 > gvar.size()) break;
        const std::uint16_t var_data_size = u16(gvar, hpos);
        const std::uint16_t idx = u16(gvar, hpos + 2);
        hpos += 4;

        // Peak tuple: embedded, or referenced from the shared tuples array.
        std::vector<float> peak(h.axis_count, 0.0f), rstart(h.axis_count, 0.0f), rend(h.axis_count, 0.0f);
        if (idx & embedded_peak) {
            for (std::uint16_t a = 0; a < h.axis_count; ++a) peak[a] = f2dot14(gvar, hpos + a * 2);
            hpos += static_cast<std::size_t>(h.axis_count) * 2;
        } else {
            const std::uint16_t sidx = idx & tuple_index_mask;
            const std::size_t so = h.shared_tuples_off + static_cast<std::size_t>(sidx) * h.axis_count * 2;
            for (std::uint16_t a = 0; a < h.axis_count; ++a) peak[a] = f2dot14(gvar, so + a * 2);
        }
        const bool inter = (idx & intermediate) != 0;
        if (inter) {
            for (std::uint16_t a = 0; a < h.axis_count; ++a) rstart[a] = f2dot14(gvar, hpos + a * 2);
            hpos += static_cast<std::size_t>(h.axis_count) * 2;
            for (std::uint16_t a = 0; a < h.axis_count; ++a) rend[a] = f2dot14(gvar, hpos + a * 2);
            hpos += static_cast<std::size_t>(h.axis_count) * 2;
        }

        const float scalar = tuple_scalar(peak, rstart, rend, coords, inter);

        // Per-tuple serialized data: optional private points, then deltas.
        std::size_t dpos = serialized;
        std::vector<std::uint16_t> points = shared_points;
        bool all_points = shared_all;
        if (idx & private_points) {
            points = read_point_numbers(gvar, dpos, all_points);
        }
        // "All points" means every outline point PLUS the four phantom
        // points (side bearings / advance) the spec appends. Reading only
        // the outline's count left the four phantom x-deltas unread, so the
        // y run started inside them: every point moved vertically by a
        // neighbour's horizontal delta. Invisible at the default instance,
        // and it broke glyphs like `N` at heavier weights.
        constexpr std::size_t kPhantoms = 4;
        const std::size_t delta_count = all_points ? n_points + kPhantoms : points.size();
        std::vector<std::int32_t> pdx = read_packed_deltas(gvar, dpos, delta_count);
        std::vector<std::int32_t> pdy = read_packed_deltas(gvar, dpos, delta_count);
        serialized += var_data_size;

        if (scalar == 0.0f) continue;

        if (all_points) {
            for (std::size_t i = 0; i < n_points && i < pdx.size() && i < pdy.size(); ++i) {
                dx[i] += static_cast<float>(pdx[i]) * scalar;
                dy[i] += static_cast<float>(pdy[i]) * scalar;
            }
        } else {
            // Sparse deltas: apply to referenced points, then IUP the rest.
            std::vector<Delta> sparse(n_points);
            std::vector<bool> touched(n_points, false);
            for (std::size_t i = 0; i < points.size() && i < pdx.size() && i < pdy.size(); ++i) {
                const std::uint16_t p = points[i];
                if (p < n_points) {
                    sparse[p] = Delta{static_cast<float>(pdx[i]), static_cast<float>(pdy[i])};
                    touched[p] = true;
                }
            }
            iup(sparse, touched, xs, ys, contour_ends);
            for (std::size_t i = 0; i < n_points; ++i) {
                dx[i] += sparse[i].x * scalar;
                dy[i] += sparse[i].y * scalar;
            }
        }
    }

    for (std::size_t i = 0; i < n_points; ++i) { xs[i] += dx[i]; ys[i] += dy[i]; }
}

// == HVAR: advance-width variations ========================================
//
// Without HVAR a variable glyph changes shape but keeps its default advance,
// so heavy weights overlap and light weights leave gaps. HVAR stores per-glyph
// advance deltas in an Item Variation Store (the same delta machinery used by
// MVAR/GDEF), indexed through an optional delta-set index map. We parse it
// lazily and return a float delta to add to the default advance for the
// current axis position.

/// A parsed HVAR table pointing back into the font bytes.
struct HvarTable {
    bool ok = false;
    std::span<const std::uint8_t> data{};
    std::size_t ivs_off = 0;   // Item Variation Store
    std::size_t map_off = 0;   // advance index map (0 = implicit identity)
};

[[nodiscard]] inline HvarTable parse_hvar(std::span<const std::uint8_t> hvar) {
    using namespace detail;
    HvarTable t;
    if (hvar.size() < 12) return t;
    // majorVersion(2) minorVersion(2) itemVariationStoreOffset(4)
    // advanceWidthMappingOffset(4)
    const std::uint32_t ivs = u32(hvar, 4);
    const std::uint32_t map = u32(hvar, 8);
    if (ivs == 0 || ivs >= hvar.size()) return t;
    t.ok = true;
    t.data = hvar;
    t.ivs_off = ivs;
    t.map_off = (map != 0 && map < hvar.size()) ? map : 0;
    return t;
}

namespace detail {

// Scalar for one variation region at the given normalised coords. A region is
// a per-axis (start, peak, end) triple in 2.14; the scalar is the product of
// each axis's tent contribution, matching gvar's tuple_scalar but reading the
// IVS RegionList layout (peak may be 0 => axis ignored).
inline float region_scalar(std::span<const std::uint8_t> d, std::size_t region_off,
                           std::uint16_t axis_count, const Coords& coords) {
    if (region_off + static_cast<std::size_t>(axis_count) * 6 > d.size()) return 0.0f;
    float s = 1.0f;
    for (std::uint16_t a = 0; a < axis_count; ++a) {
        const std::size_t o = region_off + static_cast<std::size_t>(a) * 6;
        const float start = f2dot14(d, o);
        const float peak  = f2dot14(d, o + 2);
        const float end   = f2dot14(d, o + 4);
        const float c = (a < coords.size()) ? coords[a] : 0.0f;
        if (peak == 0.0f) continue;                 // axis not part of region
        if (c < start || c > end) return 0.0f;      // outside the tent
        if (c == peak) continue;
        if (c < peak) {
            if (peak != start) s *= (c - start) / (peak - start);
        } else {
            if (end != peak) s *= (end - c) / (end - peak);
        }
    }
    return s;
}

}  // namespace detail

[[nodiscard]] inline float ivs_delta(std::span<const std::uint8_t> d, std::size_t ivs,
                                     std::uint16_t outer, std::uint16_t inner,
                                     const Coords& coords);

/// The advance-width delta (in font units) for `gid` at `coords`. Returns 0
/// when there is no HVAR, the glyph is unmapped, or the axes sit at default.
[[nodiscard]] inline float hvar_advance_delta(const HvarTable& t, std::uint16_t gid,
                                              const Coords& coords) {
    using namespace detail;
    if (!t.ok || coords.empty()) return 0.0f;
    const auto d = t.data;

    // ---- resolve gid -> (outer, inner) delta-set index ----
    std::uint16_t outer = 0, inner = gid;   // no map => identity, outer 0
    if (t.map_off != 0) {
        if (t.map_off + 4 > d.size()) return 0.0f;
        const std::uint16_t format = u16(d, t.map_off);       // entry format
        const std::uint16_t count  = u16(d, t.map_off + 2);
        const std::uint32_t entry_size =
            static_cast<std::uint32_t>(((format >> 4) & 0x3) + 1);
        const std::uint32_t inner_bits = (format & 0x0F) + 1;
        const std::uint32_t inner_mask = (1u << inner_bits) - 1;
        // Glyphs past the map reuse its last entry (per spec).
        const std::uint32_t idx = gid < count ? gid : (count ? count - 1u : 0u);
        const std::size_t eo = t.map_off + 4 + static_cast<std::size_t>(idx) * entry_size;
        if (eo + entry_size > d.size()) return 0.0f;
        std::uint32_t entry = 0;
        for (std::uint32_t b = 0; b < entry_size; ++b)
            entry = (entry << 8) | d[eo + b];
        inner = static_cast<std::uint16_t>(entry & inner_mask);
        outer = static_cast<std::uint16_t>(entry >> inner_bits);
    }

    return ivs_delta(d, t.ivs_off, outer, inner, coords);
}

/// The delta an Item Variation Store (at byte `ivs` of `d`) holds for the
/// (outer, inner) delta-set index at `coords`, in font units. One store shape
/// serves HVAR advances, GDEF/GPOS kerning and anchor deltas, and MVAR
/// metrics; 0 for out-of-range indices or truncated data.
[[nodiscard]] inline float ivs_delta(std::span<const std::uint8_t> d, std::size_t ivs,
                                     std::uint16_t outer, std::uint16_t inner,
                                     const Coords& coords) {
    using namespace detail;
    if (coords.empty()) return 0.0f;
    if (ivs + 8 > d.size()) return 0.0f;
    // format(2) variationRegionListOffset(4) itemVariationDataCount(2) [offsets32...]
    const std::size_t region_list = ivs + u32(d, ivs + 2);
    const std::uint16_t data_count = u16(d, ivs + 6);
    if (outer >= data_count) return 0.0f;
    if (region_list + 4 > d.size()) return 0.0f;
    const std::uint16_t axis_count   = u16(d, region_list);
    const std::uint16_t region_count = u16(d, region_list + 2);

    const std::size_t data_off = ivs + u32(d, ivs + 8 + static_cast<std::size_t>(outer) * 4);
    if (data_off + 6 > d.size()) return 0.0f;
    // itemCount(2) wordDeltaCount(2) regionIndexCount(2) [regionIndices(2)...] [deltas...]
    const std::uint16_t item_count       = u16(d, data_off);
    const std::uint16_t word_delta_count = u16(d, data_off + 2);
    const std::uint16_t region_idx_count = u16(d, data_off + 4);
    if (inner >= item_count) return 0.0f;

    // Long-word flag (bit 15 of wordDeltaCount) => 32-bit "word" deltas.
    const bool long_words = (word_delta_count & 0x8000) != 0;
    const std::uint16_t words = word_delta_count & 0x7FFF;
    const std::size_t region_indices = data_off + 6;
    const std::size_t deltas_base = region_indices +
                                    static_cast<std::size_t>(region_idx_count) * 2;
    const std::size_t word_sz  = long_words ? 4 : 2;
    const std::size_t short_sz = long_words ? 2 : 1;
    const std::size_t row_size = static_cast<std::size_t>(words) * word_sz +
        (static_cast<std::size_t>(region_idx_count) - words) * short_sz;
    const std::size_t row = deltas_base + static_cast<std::size_t>(inner) * row_size;
    if (row + row_size > d.size()) return 0.0f;

    float total = 0.0f;
    std::size_t p = row;
    for (std::uint16_t r = 0; r < region_idx_count; ++r) {
        const std::uint16_t ri = u16(d, region_indices + static_cast<std::size_t>(r) * 2);
        // Each region is axis_count * 3 f2dot14 values; RegionList body starts
        // after its 4-byte header.
        const std::size_t region_off = region_list + 4 +
            static_cast<std::size_t>(ri) * axis_count * 6;
        std::int32_t dv = 0;
        if (r < words) {
            if (long_words) { for (int b = 0; b < 4; ++b) dv = (dv << 8) | d[p + b]; p += 4; }
            else            { dv = s16(d, p); p += 2; }
        } else {
            if (long_words) { dv = s16(d, p); p += 2; }
            else            { dv = static_cast<std::int8_t>(d[p]); p += 1; }
        }
        if (ri >= region_count || dv == 0) continue;
        total += static_cast<float>(dv) * region_scalar(d, region_off, axis_count, coords);
    }
    return total;
}

}  // namespace mayag::typo::var
