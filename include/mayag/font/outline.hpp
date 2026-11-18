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
#include "variations.hpp"
#include "../core/geometry.hpp"

#include <cmath>
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

    /// Grow the ink by `d` (font units) on every side: each edge moves `d`
    /// along its outward normal, holes shrink by the same. Every point,
    /// on-curve and control alike, moves along the bisector of its two
    /// neighbouring edge normals by d / cos(half the turn), which shifts both
    /// edges by exactly `d` (FreeType's FT_Outline_EmboldenXY scheme). The
    /// shift is capped by the shorter neighbouring edge so a tiny segment
    /// cannot fold over, and corners sharper than ~160 degrees are left alone
    /// instead of shooting out a spike. Advance and bearings are unchanged:
    /// this is a rendering correction, not a different design.
    ///
    /// `square` selects a SQUARE (axis-aligned) dilation kernel instead of a
    /// round one: an edge with unit normal n moves by d * (|n.x| + |n.y|),
    /// i.e. horizontal and vertical edges by d and a 30-degree edge by 1.37 d.
    /// That is what CoreGraphics font smoothing does (measured on a rotated
    /// SF dash at 120 px: growth 0.294, 0.342, 0.378, 0.403 px at 0, 10, 20,
    /// 30 degrees = 0.294 * (|cos| + |sin|)); with a round kernel curves and
    /// diagonals came out ~0.08 px per side too thin.
    void embolden(float d, bool square = false) {
        if (d <= 0.0f || contours.empty()) return;
        // Outward is decided once for the whole glyph, from its total winding:
        // TrueType winds ink clockwise, CFF counter-clockwise, and holes wind
        // opposite to ink in both, so one sign serves every contour.
        float area = 0.0f;
        std::vector<Vec2*> pts;
        for (auto& c : contours) {
            pts.clear();
            collect(c, pts);
            for (std::size_t i = 0; i < pts.size(); ++i)
                area += cross(*pts[i], *pts[(i + 1) % pts.size()]);
        }
        const float side = area >= 0.0f ? 1.0f : -1.0f;

        std::vector<Vec2> shift;
        for (auto& c : contours) {
            pts.clear();
            collect(c, pts);
            const std::size_t n = pts.size();
            if (n < 3) continue;
            shift.assign(n, Vec2{});
            for (std::size_t i = 0; i < n; ++i) {
                const Vec2 p = *pts[i];
                // Nearest distinct neighbours: coincident points share a shift.
                std::size_t a = (i + n - 1) % n, b = (i + 1) % n;
                for (std::size_t k = 0; k < n && *pts[a] == p; ++k) a = (a + n - 1) % n;
                for (std::size_t k = 0; k < n && *pts[b] == p; ++k) b = (b + 1) % n;
                const Vec2 vin = p - *pts[a], vout = *pts[b] - p;
                const float lin = vin.length(), lout = vout.length();
                if (lin <= 0.0f || lout <= 0.0f) continue;
                const Vec2 in = vin / lin, out = vout / lout;
                const float cosine = dot(in, out);
                if (cosine <= -0.94f) continue;          // a hairpin: no shift
                const float denom = 1.0f + cosine;
                if (square) {
                    // Each neighbouring edge moves by its own distance
                    // d * (|nx| + |ny|); solve n_in.s = d_in, n_out.s = d_out.
                    const Vec2 nin = Vec2{in.y, -in.x} * side, nout = Vec2{out.y, -out.x} * side;
                    const float din = d * (std::abs(nin.x) + std::abs(nin.y));
                    const float dout = d * (std::abs(nout.x) + std::abs(nout.y));
                    const float det = nin.x * nout.y - nin.y * nout.x;
                    Vec2 s;
                    if (std::abs(det) < 1e-3f) {
                        s = (nin + nout) * (0.25f * (din + dout));   // (nearly) collinear
                    } else {
                        s = Vec2{(din * nout.y - dout * nin.y) / det, (nin.x * dout - nout.x * din) / det};
                    }
                    // Same fold-over cap as the round case.
                    const float l = std::min(lin, lout);
                    const float len = s.length();
                    const float cap = 2.0f * l;
                    if (len > cap && len > 0.0f) s = s * (cap / len);
                    shift[i] = s;
                    continue;
                }
                // Sum of the two outward normals; scaled by d/(1+cos) it moves
                // both edges exactly d.
                const Vec2 normal = Vec2{in.y + out.y, -(in.x + out.x)} * side;
                // Cap at concave corners, where the offset edges meet sooner
                // than the shorter neighbour edge: never shift past it.
                const float q = -cross(in, out) * side;   // > 0 at a concave turn
                const float l = std::min(lin, lout);
                const float k = (d * q <= l * denom) ? d / denom : l / q;
                shift[i] = normal * k;
            }
            for (std::size_t i = 0; i < n; ++i) *pts[i] = *pts[i] + shift[i];
        }
    }

  private:
    /// The contour's points in drawing order, as pointers so a shift can be
    /// written back. The closing point is dropped when it repeats the start.
    static void collect(Contour& c, std::vector<Vec2*>& out) {
        out.push_back(&c.start);
        for (auto& s : c.segments) {
            if (s.kind != Segment::Kind::line) out.push_back(&s.c0);
            if (s.kind == Segment::Kind::cubic) out.push_back(&s.c1);
            out.push_back(&s.to);
        }
        if (out.size() > 1 && *out.back() == c.start) out.pop_back();
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
          num_glyphs_{f.num_glyphs()},
          gvar_{f.table(ot::tags::gvar)} {
        if (!gvar_.empty()) gvar_header_ = var::parse_gvar_header(gvar_);
    }

    [[nodiscard]] bool valid() const noexcept { return !glyf_.empty() && !loca_.empty(); }

    /// Point this source at a set of normalised axis coordinates. When set (and
    /// the font has gvar), every subsequent `load` interpolates the outline to
    /// those coordinates — the variable-font instance. A null/empty coords
    /// leaves the default master untouched.
    void set_coords(const var::Coords* coords) noexcept { coords_ = coords; }
    [[nodiscard]] bool is_variable() const noexcept {
        return gvar_header_.ok && gvar_header_.axis_count > 0;
    }

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

    /// The glyph's stored bounding box (xMin, yMin, xMax, yMax), which is
    /// what HarfBuzz and CoreText report as extents at the default instance.
    /// Fonts often pad it past the points (Times' acute claims 100 units more
    /// below), and fallback mark placement measures against THIS box.
    [[nodiscard]] bool header_box(std::uint16_t gid, std::int16_t box[4]) const noexcept {
        if (!valid()) return false;
        const auto [begin, end] = glyph_range(gid);
        if (end < begin + 10) return false;
        ot::Reader r{glyf_, begin + 2};
        for (int i = 0; i < 4; ++i) box[i] = r.i16();
        return !r.bad();
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
            load_simple(r, static_cast<std::uint16_t>(n_contours), gid, out);
        } else {
            load_composite(r, gid, out, depth);
        }
        return out;
    }

  private:
    void load_simple(ot::Reader& r, std::uint16_t n_contours, std::uint16_t gid, Outline& out) const {
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

        // ---- variation: displace points per the requested axis coords ----
        // gvar deltas apply to the base outline before it becomes contours,
        // so any weight/width/optical-size is baked into the geometry that
        // the rasteriser and metrics later see.
        if (coords_ != nullptr && !coords_->empty() && gvar_header_.ok) {
            std::vector<float> fx(n_points), fy(n_points);
            for (std::size_t i = 0; i < n_points; ++i) {
                fx[i] = static_cast<float>(xs[i]);
                fy[i] = static_cast<float>(ys[i]);
            }
            var::apply_gvar(gvar_, gvar_header_, gid, *coords_, fx, fy, ends);
            for (std::size_t i = 0; i < n_points; ++i) {
                xs[i] = static_cast<std::int32_t>(std::lround(fx[i]));
                ys[i] = static_cast<std::int32_t>(std::lround(fy[i]));
            }
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

    void load_composite(ot::Reader& r, std::uint16_t gid, Outline& out, int depth) const {
        struct Comp {
            std::uint16_t gid = 0;
            float dx = 0.0f, dy = 0.0f;
            float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
            bool  args_are_xy = true;
        };
        std::vector<Comp> comps;

        std::uint16_t flags = 0;
        do {
            flags = r.u16();
            Comp cp;
            cp.gid = r.u16();
            cp.args_are_xy = (flags & detail::comp_args_are_xy) != 0;

            if (flags & detail::comp_arg_words) {
                const std::int16_t a1 = r.i16(), a2 = r.i16();
                if (cp.args_are_xy) { cp.dx = a1; cp.dy = a2; }
                // Point-matching (args are point indices) is vanishingly rare
                // and requires the parent's points; treat as no offset.
            } else {
                const std::int8_t a1 = r.i8(), a2 = r.i8();
                if (cp.args_are_xy) { cp.dx = a1; cp.dy = a2; }
            }

            if (flags & detail::comp_has_scale) {
                cp.a = cp.d = r.f2dot14();
            } else if (flags & detail::comp_xy_scale) {
                cp.a = r.f2dot14();
                cp.d = r.f2dot14();
            } else if (flags & detail::comp_two_by_two) {
                cp.a = r.f2dot14(); cp.b = r.f2dot14();
                cp.c = r.f2dot14(); cp.d = r.f2dot14();
            }
            comps.push_back(cp);
        } while ((flags & detail::comp_more) && !r.bad());

        // ---- variation: displace each component's offset ----
        // A composite glyph's gvar "points" are one per component (its offset)
        // followed by four phantom points (metrics). We vary the offsets so
        // built-up glyphs (accented letters) track the axis too.
        if (coords_ != nullptr && !coords_->empty() && gvar_header_.ok && !comps.empty()) {
            const std::size_t n = comps.size();
            std::vector<float> fx(n + 4, 0.0f), fy(n + 4, 0.0f);
            for (std::size_t i = 0; i < n; ++i) { fx[i] = comps[i].dx; fy[i] = comps[i].dy; }
            std::vector<std::uint16_t> ends{static_cast<std::uint16_t>(n + 4 - 1)};
            var::apply_gvar(gvar_, gvar_header_, gid, *coords_, fx, fy, ends);
            for (std::size_t i = 0; i < n; ++i) {
                if (comps[i].args_are_xy) { comps[i].dx = fx[i]; comps[i].dy = fy[i]; }
            }
        }

        for (const auto& cp : comps) {
            if (cp.gid < num_glyphs_) {
                Outline sub = load(cp.gid, depth + 1);
                sub.transform(Affine{cp.a, cp.b, cp.c, cp.d, cp.dx, cp.dy});
                out.append(sub);
            }
        }
    }

    std::span<const std::uint8_t> glyf_{};
    std::span<const std::uint8_t> loca_{};
    bool          long_loca_  = false;
    std::uint16_t num_glyphs_ = 0;

    std::span<const std::uint8_t> gvar_{};
    var::GvarHeader               gvar_header_{};
    const var::Coords*            coords_ = nullptr;
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

        if (b0 == 23) {                       // CFF2 blend: keep defaults+deltas;
            ++i;                              // only unblended keys are consumed
        }
        else if (b0 <= 24) {                  // operator (22 vsindex, 24 vstore: CFF2)
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
// Cff2Source — CFF2 (variable PostScript) outlines
// ════════════════════════════════════════════════════════════════════════

/// Reads CFF2 outlines. Differences from CFF1 per the OpenType spec: a
/// fixed header + Top DICT (no Name/String INDEX), 32-bit INDEX counts, no
/// width operand, no endchar/return (a charstring ends at its last byte),
/// and `vsindex`/`blend` operators that interpolate operands with region
/// scalars from the ItemVariationStore at the current normalised coords.
class Cff2Source {
  public:
    Cff2Source() = default;

    explicit Cff2Source(const ot::FontFile& f) {
        const auto data = f.table(ot::tags::cff2);
        if (!data.empty()) parse(data);
    }

    [[nodiscard]] bool valid() const noexcept { return charstrings_.count() > 0; }
    [[nodiscard]] bool is_variable() const noexcept { return region_count_ > 0; }
    void set_coords(const var::Coords* c) noexcept { coords_ = c; }

    [[nodiscard]] Outline load(std::uint16_t gid) const {
        Outline out;
        if (!valid() || gid >= charstrings_.count()) return out;
        Interp st{};
        const std::uint32_t fd = fd_for(gid);
        if (fd < fds_.size()) {
            st.local   = &fds_[fd].subrs;
            st.vsindex = fds_[fd].vsindex;
        }
        run(charstrings_.at(gid), st, out, 0);
        close_contour(st, out);
        return out;
    }

  private:
    static constexpr int max_stack = 513;

    struct FontDict { cff::Index subrs{}; std::uint16_t vsindex = 0; };

    struct Interp {
        float stack[max_stack]{};
        int   sp = 0;
        Vec2  pos{};
        bool  open = false;
        Contour current{};
        int   n_stems = 0;
        std::uint16_t vsindex = 0;
        int   scalars_for = -1;          // vsindex the cached scalars belong to
        std::vector<float> scalars;
        const cff::Index* local = nullptr;
        void push(float v) noexcept { if (sp < max_stack) stack[sp++] = v; }
        void clear() noexcept { sp = 0; }
    };

    static void close_contour(Interp& st, Outline& out) {
        if (st.open && !st.current.empty()) {
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
        if (st.open) st.current.segments.push_back(Segment{Segment::Kind::line, {}, {}, p});
        st.pos = p;
    }
    static void curve_to(Interp& st, Vec2 c1, Vec2 c2, Vec2 p) {
        if (st.open) st.current.segments.push_back(Segment{Segment::Kind::cubic, c1, c2, p});
        st.pos = p;
    }
    static void rrcurve(Interp& st, const float* a) {
        const Vec2 c1 = st.pos + Vec2{a[0], a[1]};
        const Vec2 c2 = c1 + Vec2{a[2], a[3]};
        curve_to(st, c1, c2, c2 + Vec2{a[4], a[5]});
    }

    /// Region scalars for ItemVariationData `vsi` at the live coords.
    void compute_scalars(Interp& st) const {
        st.scalars.clear();
        st.scalars_for = st.vsindex;
        if (st.vsindex >= ivd_offsets_.size()) return;
        const auto d = vstore_;
        const std::size_t ivd = ivd_offsets_[st.vsindex];
        const std::uint16_t n = var::detail::u16(d, ivd + 4);
        st.scalars.resize(n, 0.0f);
        if (coords_ == nullptr || coords_->empty()) return;
        for (std::uint16_t j = 0; j < n; ++j) {
            const std::uint16_t ri = var::detail::u16(d, ivd + 6 + 2u * j);
            if (ri >= region_count_) continue;
            const std::size_t roff = region_list_ + 4 +
                static_cast<std::size_t>(ri) * axis_count_ * 6;
            st.scalars[j] = var::detail::region_scalar(d, roff, axis_count_, *coords_);
        }
    }

    void blend(Interp& st) const {
        if (st.sp < 1) { st.clear(); return; }
        if (st.scalars_for != st.vsindex) compute_scalars(st);
        const int n = static_cast<int>(st.stack[st.sp - 1]);
        const int k = static_cast<int>(st.scalars.size());
        --st.sp;
        if (n < 0 || n * (k + 1) > st.sp) { st.clear(); return; }
        const int base = st.sp - n * (k + 1);
        const int deltas = base + n;
        for (int i = 0; i < n; ++i) {
            float v = st.stack[base + i];
            for (int j = 0; j < k; ++j) {
                v += st.stack[deltas + i * k + j] * st.scalars[static_cast<std::size_t>(j)];
            }
            st.stack[base + i] = v;
        }
        st.sp = base + n;
    }

    void call(const cff::Index* idx, Interp& st, Outline& out, int depth) const {
        if (st.sp <= 0 || idx == nullptr) { st.clear(); return; }
        const int i = static_cast<int>(st.stack[--st.sp]) + cff::subr_bias(idx->count());
        if (i >= 0 && static_cast<std::uint32_t>(i) < idx->count()) {
            run(idx->at(static_cast<std::uint32_t>(i)), st, out, depth + 1);
        }
    }

    void run(std::span<const std::uint8_t> code, Interp& st, Outline& out, int depth) const {
        if (depth > 10) return;
        float* s = st.stack;
        for (std::size_t i = 0; i < code.size();) {
            const std::uint8_t b0 = code[i];
            if (b0 >= 32 || b0 == 28) {
                if (b0 == 28) {
                    if (i + 2 >= code.size()) return;
                    st.push(static_cast<std::int16_t>((code[i + 1] << 8) | code[i + 2]));
                    i += 3;
                } else if (b0 <= 246) {
                    st.push(static_cast<float>(static_cast<int>(b0) - 139)); ++i;
                } else if (b0 <= 250) {
                    if (i + 1 >= code.size()) return;
                    st.push(static_cast<float>((b0 - 247) * 256 + code[i + 1] + 108)); i += 2;
                } else if (b0 <= 254) {
                    if (i + 1 >= code.size()) return;
                    st.push(static_cast<float>(-(b0 - 251) * 256 - code[i + 1] - 108)); i += 2;
                } else {
                    if (i + 4 >= code.size()) return;
                    const auto v = static_cast<std::int32_t>(
                        (static_cast<std::uint32_t>(code[i + 1]) << 24) |
                        (static_cast<std::uint32_t>(code[i + 2]) << 16) |
                        (static_cast<std::uint32_t>(code[i + 3]) << 8) | code[i + 4]);
                    st.push(static_cast<float>(v) / 65536.0f); i += 5;
                }
                continue;
            }
            ++i;
            const int n = st.sp;
            switch (b0) {
                case 1: case 3: case 18: case 23:
                    st.n_stems += n / 2; st.clear(); break;
                case 19: case 20:
                    st.n_stems += n / 2; st.clear();
                    i += static_cast<std::size_t>((st.n_stems + 7) / 8);
                    break;
                case 15:                                            // vsindex
                    if (n > 0 && s[n - 1] >= 0.0f) st.vsindex = static_cast<std::uint16_t>(s[n - 1]);
                    st.clear(); break;
                case 16: blend(st); break;                          // blend
                case 21: if (n >= 2) move_to(st, out, st.pos + Vec2{s[n - 2], s[n - 1]}); st.clear(); break;
                case 22: if (n >= 1) move_to(st, out, st.pos + Vec2{s[n - 1], 0.0f}); st.clear(); break;
                case 4:  if (n >= 1) move_to(st, out, st.pos + Vec2{0.0f, s[n - 1]}); st.clear(); break;
                case 5:
                    for (int k = 0; k + 1 < n; k += 2) line_to(st, st.pos + Vec2{s[k], s[k + 1]});
                    st.clear(); break;
                case 6: case 7: {
                    bool h = (b0 == 6);
                    for (int k = 0; k < n; ++k) {
                        line_to(st, st.pos + (h ? Vec2{s[k], 0.0f} : Vec2{0.0f, s[k]}));
                        h = !h;
                    }
                    st.clear(); break;
                }
                case 8: for (int k = 0; k + 5 < n; k += 6) rrcurve(st, &s[k]); st.clear(); break;
                case 24: {
                    int k = 0;
                    for (; k + 5 < n - 2; k += 6) rrcurve(st, &s[k]);
                    if (k + 1 < n) line_to(st, st.pos + Vec2{s[k], s[k + 1]});
                    st.clear(); break;
                }
                case 25: {
                    int k = 0;
                    for (; k + 1 < n - 6; k += 2) line_to(st, st.pos + Vec2{s[k], s[k + 1]});
                    if (k + 5 < n) rrcurve(st, &s[k]);
                    st.clear(); break;
                }
                case 26: case 27: {
                    int k = 0; float d1 = 0.0f;
                    if (n % 4 == 1) { d1 = s[0]; k = 1; }
                    const bool v = (b0 == 26);
                    for (; k + 3 < n; k += 4) {
                        const Vec2 c1 = st.pos + (v ? Vec2{d1, s[k]} : Vec2{s[k], d1});
                        const Vec2 c2 = c1 + Vec2{s[k + 1], s[k + 2]};
                        curve_to(st, c1, c2, c2 + (v ? Vec2{0.0f, s[k + 3]} : Vec2{s[k + 3], 0.0f}));
                        d1 = 0.0f;
                    }
                    st.clear(); break;
                }
                case 30: case 31: {
                    bool h = (b0 == 31);
                    for (int k = 0; k + 3 < n; k += 4) {
                        const float last = (n - k == 5) ? s[k + 4] : 0.0f;
                        const Vec2 c1 = st.pos + (h ? Vec2{s[k], 0.0f} : Vec2{0.0f, s[k]});
                        const Vec2 c2 = c1 + Vec2{s[k + 1], s[k + 2]};
                        curve_to(st, c1, c2, c2 + (h ? Vec2{last, s[k + 3]} : Vec2{s[k + 3], last}));
                        h = !h;
                    }
                    st.clear(); break;
                }
                case 10: call(st.local, st, out, depth); break;
                case 29: call(&global_subrs_, st, out, depth); break;
                case 12: {
                    if (i >= code.size()) return;
                    escape(code[i++], st);
                    break;
                }
                default: st.clear(); break;                     // incl. 11/14: invalid in CFF2
            }
        }
    }

    static void escape(std::uint8_t op, Interp& st) {
        const float* s = st.stack;
        switch (op) {
            case 35:
                if (st.sp >= 13) { rrcurve(st, &s[0]); rrcurve(st, &s[6]); }
                break;
            case 34:
                if (st.sp >= 7) {
                    const float y = st.pos.y;
                    const Vec2 c1 = st.pos + Vec2{s[0], 0.0f};
                    const Vec2 c2 = c1 + Vec2{s[1], s[2]};
                    curve_to(st, c1, c2, c2 + Vec2{s[3], 0.0f});
                    const Vec2 c3 = st.pos + Vec2{s[4], 0.0f};
                    const Vec2 c4 = Vec2{c3.x + s[5], y};
                    curve_to(st, c3, c4, Vec2{c4.x + s[6], y});
                }
                break;
            case 36:
                if (st.sp >= 9) {
                    const float y = st.pos.y;
                    const Vec2 c1 = st.pos + Vec2{s[0], s[1]};
                    const Vec2 c2 = c1 + Vec2{s[2], s[3]};
                    curve_to(st, c1, c2, c2 + Vec2{s[4], 0.0f});
                    const Vec2 c3 = st.pos + Vec2{s[5], 0.0f};
                    const Vec2 c4 = c3 + Vec2{s[6], s[7]};
                    curve_to(st, c3, c4, Vec2{c4.x + s[8], y});
                }
                break;
            case 37:
                if (st.sp >= 11) {
                    const Vec2 start = st.pos;
                    float dx = 0.0f, dy = 0.0f;
                    for (int k = 0; k < 10; k += 2) { dx += s[k]; dy += s[k + 1]; }
                    rrcurve(st, &s[0]);
                    const Vec2 c3 = st.pos + Vec2{s[6], s[7]};
                    const Vec2 c4 = c3 + Vec2{s[8], s[9]};
                    Vec2 to = c4;
                    if (std::fabs(dx) > std::fabs(dy)) { to.x += s[10]; to.y = start.y; }
                    else                               { to.y += s[10]; to.x = start.x; }
                    curve_to(st, c3, c4, to);
                }
                break;
            default: break;
        }
        st.clear();
    }

    void parse(std::span<const std::uint8_t> data) {
        ot::Reader r{data};
        if (r.u8_at(0) != 2) return;                         // majorVersion
        const std::size_t hdr = r.u8_at(2);
        const std::size_t top_len = r.u16_at(3);
        const auto top = cff::parse_dict(r.slice(hdr, top_len));
        global_subrs_ = cff::parse_index(r, hdr + top_len, true);
        const auto off = [&](std::uint16_t op) -> std::size_t {
            const auto* v = cff::dict_get(top, op);
            return (v && !v->empty() && v->back() > 0.0f) ? static_cast<std::size_t>(v->back()) : 0;
        };
        if (const std::size_t cs = off(17); cs) charstrings_ = cff::parse_index(r, cs, true);
        if (const std::size_t fda = off(0x0C24); fda) {
            const cff::Index fd_index = cff::parse_index(r, fda, true);
            fds_.resize(fd_index.count());
            for (std::uint32_t k = 0; k < fd_index.count(); ++k) {
                const auto fd = cff::parse_dict(fd_index.at(k));
                const auto* pv = cff::dict_get(fd, 18);
                if (!pv || pv->size() < 2) continue;
                const auto sz = static_cast<std::size_t>((*pv)[pv->size() - 2]);
                const auto of = static_cast<std::size_t>((*pv)[pv->size() - 1]);
                const auto priv = cff::parse_dict(r.slice(of, sz));
                if (const auto* ls = cff::dict_get(priv, 19); ls && !ls->empty() && ls->back() > 0.0f) {
                    fds_[k].subrs = cff::parse_index(r, of + static_cast<std::size_t>(ls->back()), true);
                }
                if (const auto* vi = cff::dict_get(priv, 22); vi && !vi->empty() && vi->back() >= 0.0f) {
                    fds_[k].vsindex = static_cast<std::uint16_t>(vi->back());
                }
            }
        }
        fd_select_ = off(0x0C25);
        reader_ = r;
        if (const std::size_t vs = off(24); vs) {
            const std::size_t len = r.u16_at(vs);
            vstore_ = r.slice(vs + 2, len);
            const auto d = vstore_;
            if (var::detail::u16(d, 0) == 1) {
                region_list_ = var::detail::u32(d, 2);
                axis_count_   = var::detail::u16(d, region_list_);
                region_count_ = var::detail::u16(d, region_list_ + 2);
                const std::uint16_t nivd = var::detail::u16(d, 6);
                for (std::uint16_t k = 0; k < nivd; ++k) {
                    ivd_offsets_.push_back(var::detail::u32(d, 8 + 4u * k));
                }
            }
        }
    }

    [[nodiscard]] std::uint32_t fd_for(std::uint16_t gid) const {
        if (fds_.size() <= 1 || fd_select_ == 0) return 0;
        const std::size_t p = fd_select_;
        const std::uint8_t fmt = reader_.u8_at(p);
        if (fmt == 0) return reader_.u8_at(p + 1 + gid);
        if (fmt == 3) {
            const std::uint16_t nr = reader_.u16_at(p + 1);
            for (std::uint16_t k = 0; k < nr; ++k) {
                const std::size_t rec = p + 3 + k * 3u;
                if (gid >= reader_.u16_at(rec) && gid < reader_.u16_at(rec + 3)) return reader_.u8_at(rec + 2);
            }
        } else if (fmt == 4) {
            const std::uint32_t nr = reader_.u32_at(p + 1);
            for (std::uint32_t k = 0; k < nr && k < 65536; ++k) {
                const std::size_t rec = p + 5 + k * 6u;
                if (gid >= reader_.u32_at(rec) && gid < reader_.u32_at(rec + 6)) return reader_.u16_at(rec + 4);
            }
        }
        return 0;
    }

    ot::Reader               reader_{};
    cff::Index               charstrings_{};
    cff::Index               global_subrs_{};
    std::vector<FontDict>    fds_;
    std::size_t              fd_select_ = 0;
    std::span<const std::uint8_t> vstore_{};
    std::size_t              region_list_ = 0;
    std::uint16_t            axis_count_ = 0;
    std::uint16_t            region_count_ = 0;
    std::vector<std::size_t> ivd_offsets_;
    const var::Coords*       coords_ = nullptr;
};

// ════════════════════════════════════════════════════════════════════════
// GlyphSource — the format-agnostic front door
// ════════════════════════════════════════════════════════════════════════

}  // namespace mayag::typo
#include "hvgl.hpp"
namespace mayag::typo {

/// Loads outlines from whichever format the face uses. Everything downstream
/// depends on THIS, not on glyf/CFF, which is why adding CFF2 or SVG glyphs
/// later will not ripple.
class GlyphSource {
  public:
    GlyphSource() = default;

    explicit GlyphSource(const ot::FontFile& f) : upem_{f.units_per_em()} {
        if (f.has_table(ot::tags::hvgl) && !f.has_table(ot::tags::glyf) && !f.has_table(ot::tags::cff) &&
            !f.has_table(ot::tags::cff2)) {
            hvgl_ = HvglSource{f};
            kind_ = hvgl_.valid() ? Kind::hvgl : Kind::none;
        } else if (f.has_table(ot::tags::cff2) && !f.has_table(ot::tags::glyf)) {
            cff2_ = Cff2Source{f};
            kind_ = cff2_.valid() ? Kind::cff2 : Kind::none;
        } else if (f.is_cff()) {
            cff_ = CffSource{f};
            kind_ = cff_.valid() ? Kind::cff : Kind::none;
        } else {
            glyf_ = GlyfSource{f};
            kind_ = glyf_.valid() ? Kind::glyf : Kind::none;
        }
    }

    [[nodiscard]] bool valid() const noexcept { return kind_ != Kind::none; }
    [[nodiscard]] std::uint16_t units_per_em() const noexcept { return upem_; }

    /// glyf only: the stored bounding box (HarfBuzz's default-instance
    /// extents). CFF has none; callers fall back to outline bounds.
    [[nodiscard]] bool header_box(std::uint16_t gid, std::int16_t box[4]) const noexcept {
        return kind_ == Kind::glyf && glyf_.header_box(gid, box);
    }

    [[nodiscard]] Outline load(std::uint16_t gid) const {
        switch (kind_) {
            case Kind::glyf: return glyf_.load(gid);
            case Kind::cff:  return cff_.load(gid);
            case Kind::cff2: return cff2_.load(gid);
            case Kind::hvgl: return hvgl_.load(gid);
            case Kind::none: break;
        }
        return {};
    }

    /// Point this source at a set of normalized axis coordinates. Only the
    /// glyf/gvar and CFF2 (blend) paths vary. The pointer must outlive every
    /// subsequent load() call.
    void set_coords(const var::Coords* c) noexcept {
        if (kind_ == Kind::glyf) glyf_.set_coords(c);
        if (kind_ == Kind::cff2) cff2_.set_coords(c);
        if (kind_ == Kind::hvgl) hvgl_.set_coords(c);
    }

    /// True when the face carries gvar / CFF2 variation data we can apply.
    [[nodiscard]] bool is_variable() const noexcept {
        return (kind_ == Kind::glyf && glyf_.is_variable()) ||
               (kind_ == Kind::cff2 && cff2_.is_variable()) || kind_ == Kind::hvgl;
    }

  private:
    enum class Kind : std::uint8_t { none, glyf, cff, cff2, hvgl };

    Kind          kind_ = Kind::none;
    GlyfSource    glyf_;
    CffSource     cff_;
    Cff2Source    cff2_;
    HvglSource    hvgl_;
    std::uint16_t upem_ = 1000;
};

}  // namespace mayag::typo
