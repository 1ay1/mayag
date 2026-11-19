#pragma once
// mayag::typo — Apple 'hvgl' / 'hvpm' outlines (HVF, Hierarchical Variation
// Fonts), as specified in Apple's TrueType Reference Manual (RM06,
// Chap6hvgl / Chap6hvpm). PingFang UI, the face CoreText draws Chinese and
// Japanese with, stores its glyphs this way and has no glyf/CFF at all.
//
// Model. A glyph is a PART: either a SHAPE (quadratic paths whose segment
// coordinates blend linearly over the shape's own axes) or a COMPOSITE (a
// tree of subparts, each placed by a rotation then a translation, whose axis
// values and transforms are themselves blended over the composite's axes).
// Each axis value is in [-1, 1]; a delta matrix has two columns per axis,
// the first weighted by -v when v < 0, the second by v when v >= 0.
// Composites address every node of their structure tree in depth-first
// order (excluding themselves): the transform rows and axis-value rows of
// nested subparts are inherited downward and added to by each level.
//
// Segment coordinates are (on.x, on.y, off.x, off.y) per segment; a curve-
// type segment (blend 0) stores the on-curve point as a fraction t of the
// way from the previous segment's off-curve point to its own, and a lone
// tangent (blend 2) keeps its on-curve point on that same segment
// (projected); both clamp t to [0, 1]. The rest store the on-curve point
// directly.
//
// The spec does not say how a composite's transform deltas compose. Derived
// against Apple's own libhvf (HVF_render_current_part, which CoreText uses),
// part by part: every tree row is placed by a RIGID MOTION
//     row = master ∘ E_c0 ∘ E_c1 ∘ … ∘ inherited
// where master is the row's master rotation + translation, E_c is the
// extremum column c (ascending) — rotation w·θ and, when that column also
// rotates, a translation that follows the rotation's arc (a screw motion:
// t·(1 − e^{iwθ})/(1 − e^{iθ}), exact at w = 0 and w = 1) — and inherited is
// the motion the ancestors computed for this row. Axis values are NOT
// clamped: a composite may drive a subpart's axes past ±1 and the deltas
// extrapolate.
//
// Accuracy: 1200 random parts × axis vectors match libhvf within 1e-6
// units; 600 random glyph instances match CTFontCreatePathForGlyph within
// 0.006 units (float32 rounding).
//
// All values little-endian; shapes use float64, composites float32. The
// table is untrusted input: every read is bounds-checked, part recursion is
// capped, and malformed parts yield an empty outline.

// Included by outline.hpp after Outline/Contour/Segment are defined.
#include "opentype.hpp"
#include "variations.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace mayag::typo {

class HvglSource {
  public:
    HvglSource() = default;

    explicit HvglSource(const ot::FontFile& f) {
        data_ = f.table(ot::tags::hvgl);
        if (data_.size() < 24 || rd16(0) != 3) { data_ = {}; return; }
        n_parts_ = rd32(8);
        index_ = rd32(12);
        n_glyphs_ = rd32(16);
        if (index_ + std::size_t{4} * n_parts_ > data_.size()) { data_ = {}; return; }
        // 'hvpm': part substitution ranges [start, end] -> destination.
        const auto pm = f.table(ot::tags::hvpm);
        if (pm.size() >= 8) {
            const auto r32 = [&](std::size_t o) { return o + 4 <= pm.size() ? le32(pm.data() + o) : 0u; };
            const std::uint32_t sub = r32(4);
            const std::uint32_t count = r32(sub);
            for (std::uint32_t i = 0; i < count && i < 65536; ++i) {
                const std::size_t e = sub + 4 + std::size_t{12} * i;
                if (e + 12 > pm.size()) break;
                remap_.push_back({r32(e), r32(e + 4), r32(e + 8)});
            }
        }
    }

    [[nodiscard]] bool valid() const noexcept { return !data_.empty(); }
    void set_coords(const var::Coords* c) noexcept { coords_ = c; }

    [[nodiscard]] Outline load(std::uint32_t gid) const {
        Outline out;
        if (!valid() || gid >= n_glyphs_) return out;
        // The root part's axes are the font's fvar axes, in order, already
        // normalised (avar applied) by Face.
        std::vector<double> root(coords_ != nullptr ? coords_->size() : 0, 0.0);
        for (std::size_t i = 0; i < root.size(); ++i) root[i] = static_cast<double>((*coords_)[i]);
        std::size_t budget = 1 << 16;   // parts visited, for hostile trees
        part(gid, root, {}, {}, Motion{}, out, 0, budget);
        return out;
    }

  private:
    /// p -> R(th) p + (x, y)
    struct Motion {
        double th = 0, x = 0, y = 0;
        [[nodiscard]] Vec2 apply(double px, double py) const {
            const double c = std::cos(th), s = std::sin(th);
            return Vec2{static_cast<float>(c * px - s * py + x), static_cast<float>(s * px + c * py + y)};
        }
        [[nodiscard]] Motion then(const Motion& n) const {   // this ∘ n
            const double c = std::cos(th), s = std::sin(th);
            return {th + n.th, x + c * n.x - s * n.y, y + s * n.x + c * n.y};
        }
    };
    struct Remap { std::uint32_t start, end, dest; };
    struct ColDelta {   // one extremum column's contribution to one tree row
        std::size_t row, col;
        double th, x, y;
    };

    std::span<const std::uint8_t> data_;
    std::uint32_t n_parts_ = 0, index_ = 0, n_glyphs_ = 0;
    std::vector<Remap> remap_;
    const var::Coords* coords_ = nullptr;

    static std::uint32_t le32(const std::uint8_t* p) noexcept {
        return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
    }
    [[nodiscard]] std::uint16_t rd16(std::size_t o) const noexcept {
        return o + 2 <= data_.size() ? static_cast<std::uint16_t>(data_[o] | data_[o + 1] << 8) : 0;
    }
    [[nodiscard]] std::uint32_t rd32(std::size_t o) const noexcept { return o + 4 <= data_.size() ? le32(data_.data() + o) : 0; }
    [[nodiscard]] float rdf(std::size_t o) const noexcept { return std::bit_cast<float>(rd32(o)); }
    [[nodiscard]] double rdd(std::size_t o) const noexcept {
        if (o + 8 > data_.size()) return 0.0;
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = v << 8 | data_[o + static_cast<std::size_t>(i)];
        const double x = std::bit_cast<double>(v);
        return std::isfinite(x) ? x : 0.0;
    }
    [[nodiscard]] static double finite(float x) noexcept { return std::isfinite(x) ? static_cast<double>(x) : 0.0; }

    [[nodiscard]] std::uint32_t remapped(std::uint32_t id) const noexcept {
        for (const auto& r : remap_)
            if (id >= r.start && id <= r.end) return id - r.start + r.dest;
        return id;
    }
    [[nodiscard]] std::size_t part_offset(std::uint32_t id) const noexcept {
        id = remapped(id);
        if (id >= n_parts_) return 0;
        const std::size_t o = std::size_t{index_} + rd32(index_ + std::size_t{4} * id);
        return o < data_.size() ? o : 0;
    }

    static void push_weights(std::span<const double> axes, std::size_t naxes, std::vector<double>& w) {
        // Not clamped: composites legitimately drive subpart axes past ±1
        // and libhvf extrapolates the deltas (checked part by part).
        w.assign(2 * naxes, 0.0);
        for (std::size_t a = 0; a < naxes && a < axes.size(); ++a) {
            const double v = axes[a];
            if (v < 0) w[2 * a] = -v;
            else w[2 * a + 1] = v;
        }
    }

    void shape(std::size_t o, std::span<const double> axes, const Motion& xf, Outline& out) const {
        const std::size_t naxes = rd16(o + 2), npaths = rd16(o + 4), nseg = rd16(o + 6);
        std::size_t p = o + 8;
        if (p + 2 * npaths + nseg > data_.size()) return;
        const std::size_t sizes = p;
        p += 2 * npaths;
        const std::size_t blend = p;
        p += nseg;
        p = (p + 7) & ~std::size_t{7};   // float64 alignment (the table itself is 8-aligned)
        const std::size_t rows = 4 * nseg;
        const std::size_t master = p, matrix = p + 8 * rows;
        if (matrix + 8 * rows * 2 * naxes > data_.size()) return;
        std::vector<double> w;
        push_weights(axes, naxes, w);
        std::vector<double> v(rows);
        for (std::size_t r = 0; r < rows; ++r) v[r] = rdd(master + 8 * r);
        for (std::size_t col = 0; col < w.size(); ++col) {
            if (w[col] == 0.0) continue;
            const std::size_t base = matrix + 8 * rows * col;
            for (std::size_t r = 0; r < rows; ++r) v[r] += rdd(base + 8 * r) * w[col];
        }
        std::size_t seg = 0;
        for (std::size_t pi = 0; pi < npaths; ++pi) {
            const std::size_t n = rd16(sizes + 2 * pi);
            if (seg + n > nseg) return;
            if (n >= 3) {
                // on-curve point per segment (curve / tangent rules), then
                // one quadratic per segment: on[k] --off[k]--> on[k+1].
                std::vector<Vec2> on(n), off(n);
                for (std::size_t k = 0; k < n; ++k) {
                    const double* s = &v[4 * (seg + k)];
                    const double* prev = &v[4 * (seg + (k + n - 1) % n)];
                    double ox = s[0], oy = s[1];
                    const std::uint8_t bt = data_[blend + seg + k];
                    if (bt == 0) {   // curve: on-point at fraction t along prev.off -> this.off
                        const double t = std::clamp(s[0], 0.0, 1.0);
                        ox = prev[2] + t * (s[2] - prev[2]);
                        oy = prev[3] + t * (s[3] - prev[3]);
                    } else if (bt == 2) {   // lone tangent: kept on that segment (clamped)
                        const double dx = s[2] - prev[2], dy = s[3] - prev[3], l = dx * dx + dy * dy;
                        if (l > 0) {
                            const double t = std::clamp(((s[0] - prev[2]) * dx + (s[1] - prev[3]) * dy) / l, 0.0, 1.0);
                            ox = prev[2] + t * dx;
                            oy = prev[3] + t * dy;
                        }
                    }
                    on[k] = xf.apply(ox, oy);
                    off[k] = xf.apply(s[2], s[3]);
                }
                Contour c;
                c.start = on[0];
                for (std::size_t k = 0; k < n; ++k)
                    c.segments.push_back(Segment{Segment::Kind::quad, off[k], {}, on[(k + 1) % n]});
                out.contours.push_back(std::move(c));
            }
            seg += n;
        }
    }

    void part(std::uint32_t id, std::span<const double> axes, std::span<const double> inh_axes,
              std::span<const Motion> inherited, const Motion& xf,
              Outline& out, int depth, std::size_t& budget) const {
        if (depth > 32 || budget == 0) return;
        --budget;
        const std::size_t o = part_offset(id);
        if (o == 0 || o + 8 > data_.size()) return;
        if ((rd16(o) & 1) == 0) { shape(o, axes, xf, out); return; }
        if (o + 36 > data_.size()) return;
        std::uint16_t h[18];
        for (int i = 0; i < 18; ++i) h[i] = rd16(o + 2 * static_cast<std::size_t>(i));
        const std::size_t naxes = h[1], nsub = h[2], ntree = h[3], treeaxes = h[4];
        const std::size_t nmax = h[6], nexax = h[7], nmtr = h[8], nmrot = h[9], nextr = h[10], nexrot = h[11];
        if (ntree == 0 || treeaxes < naxes) return;
        const std::size_t rows_ax = treeaxes - naxes, rows_tr = ntree - 1, ncol = 2 * naxes;
        const auto at4 = [&](std::uint16_t q) { return o + 4 * std::size_t{q}; };
        // Per tree row: master motion, then this composite's extremum columns
        // (sparse, ascending column order), composed with the inherited
        // motion — see the header comment.
        std::vector<double> vax(rows_ax, 0.0);
        std::vector<Motion> master(rows_tr);
        for (std::size_t i = 0; i < rows_ax && i < inh_axes.size(); ++i) vax[i] = inh_axes[i];
        std::vector<double> w;
        push_weights(axes, naxes, w);
        // axis values: master (sparse rows) + extremum (column-sparse)
        const std::size_t cs = at4(h[13]);
        const auto colstart = [&](std::size_t c) -> std::size_t { return rd16(cs + 2 * c); };
        const std::size_t mrows = cs + 2 * (ncol + 1), erows = mrows + 2 * nmax;
        for (std::size_t i = 0; i < nmax; ++i) {
            const std::size_t r = rd16(mrows + 2 * i);
            if (r < rows_ax) vax[r] += finite(rdf(at4(h[14]) + 4 * i));
        }
        for (std::size_t c = 0; c < ncol; ++c) {
            if (w[c] == 0.0) continue;
            for (std::size_t i = colstart(c); i < colstart(c + 1) && i < nexax; ++i) {
                const std::size_t r = rd16(erows + 2 * i);
                if (r < rows_ax) vax[r] += finite(rdf(at4(h[15]) + 4 * i)) * w[c];
            }
        }
        // translations: master deltas, extremum deltas, extremum (row,col), master rows
        std::size_t t = at4(h[16]);
        const std::size_t mtd = t, etd = t + 8 * nmtr, eti = etd + 8 * nextr, mti = eti + 4 * nextr;
        for (std::size_t i = 0; i < nmtr; ++i) {
            const std::size_t r = rd16(mti + 2 * i);
            if (r < rows_tr) { master[r].x += finite(rdf(mtd + 8 * i)); master[r].y += finite(rdf(mtd + 8 * i + 4)); }
        }
        std::vector<ColDelta> cols;   // (row, col) -> rotation + translation, this instance's active columns
        const auto col_entry = [&](std::size_t r, std::size_t c) -> ColDelta& {
            for (auto& d : cols) if (d.row == r && d.col == c) return d;
            cols.push_back(ColDelta{r, c, 0.0, 0.0, 0.0});
            return cols.back();
        };
        for (std::size_t i = 0; i < nextr; ++i) {
            const std::size_t r = rd16(eti + 4 * i), c = rd16(eti + 4 * i + 2);
            if (r < rows_tr && c < ncol && w[c] != 0.0) {
                ColDelta& d = col_entry(r, c);
                d.x += finite(rdf(etd + 8 * i));
                d.y += finite(rdf(etd + 8 * i + 4));
            }
        }
        // rotations, same layout with scalars
        const std::size_t mrd = at4(h[17]), erd = mrd + 4 * nmrot, eri = erd + 4 * nexrot, mri = eri + 4 * nexrot;
        for (std::size_t i = 0; i < nmrot; ++i) {
            const std::size_t r = rd16(mri + 2 * i);
            if (r < rows_tr) master[r].th += finite(rdf(mrd + 4 * i));
        }
        for (std::size_t i = 0; i < nexrot; ++i) {
            const std::size_t r = rd16(eri + 4 * i), c = rd16(eri + 4 * i + 2);
            if (r < rows_tr && c < ncol && w[c] != 0.0) col_entry(r, c).th += finite(rdf(erd + 4 * i));
        }
        // Row motions: master ∘ E_c (ascending c) ∘ inherited.
        std::sort(cols.begin(), cols.end(), [](const ColDelta& a, const ColDelta& b) {
            return a.row != b.row ? a.row < b.row : a.col < b.col;
        });
        std::vector<Motion> row(master);
        for (const auto& d : cols) {
            const double ww = w[d.col], th = ww * d.th;
            double tx = ww * d.x, ty = ww * d.y;
            if (d.th != 0.0 && (1.0 - std::cos(d.th)) * (1.0 - std::cos(d.th)) + std::sin(d.th) * std::sin(d.th) > 1e-18) {
                // screw: translation follows the rotation's arc,
                // t * (1 - e^{i w θ}) / (1 - e^{i θ})
                const double nr = 1.0 - std::cos(th), ni = -std::sin(th);
                const double dr = 1.0 - std::cos(d.th), di = -std::sin(d.th);
                const double den = dr * dr + di * di;
                const double fr = (nr * dr + ni * di) / den, fi = (ni * dr - nr * di) / den;
                tx = fr * d.x - fi * d.y;
                ty = fr * d.y + fi * d.x;
            }
            row[d.row] = row[d.row].then(Motion{th, tx, ty});
        }
        for (std::size_t r = 0; r < rows_tr && r < inherited.size(); ++r) row[r] = row[r].then(inherited[r]);
        // subparts
        const std::size_t sub = at4(h[12]);
        for (std::size_t k = 0; k < nsub; ++k) {
            const std::size_t e = sub + 8 * k;
            if (e + 8 > data_.size()) return;
            const std::uint32_t pid = rd32(e);
            const std::size_t tp = rd16(e + 4), ta = rd16(e + 6);
            const std::size_t tp_end = k + 1 < nsub ? rd16(e + 8 + 4) : rows_tr;
            const std::size_t ta_end = k + 1 < nsub ? rd16(e + 8 + 6) : rows_ax;
            if (tp >= rows_tr || tp_end > rows_tr || tp > tp_end || ta > ta_end || ta_end > rows_ax) continue;
            const std::size_t po = part_offset(pid);
            const std::size_t sub_axes = po != 0 ? rd16(po + 2) : 0;
            if (ta + sub_axes > ta_end) continue;
            const std::span<const double> ax{vax.data() + ta, sub_axes};
            const std::span<const double> iax{vax.data() + ta + sub_axes, ta_end - ta - sub_axes};
            const std::span<const Motion> inh{row.data() + tp + 1, tp_end - tp - 1};
            part(pid, ax, iax, inh, xf.then(row[tp]), out, depth + 1, budget);
        }
    }
};

}  // namespace mayag::typo
