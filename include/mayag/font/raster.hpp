#pragma once
// mayag::typo::raster — outline to coverage, and coverage to SDF
//
// Two rendering paths, because a UI needs both:
//
//   `rasterize()`   Exact analytic coverage. Every pixel gets the true area
//                   of the glyph inside it — not N supersamples. This is what
//                   makes small text crisp: at 11 px the difference between
//                   16x supersampling and exact area is plainly visible.
//
//   `make_sdf()`    A true Euclidean signed distance field, computed with the
//                   8SSEDT two-pass algorithm. One texture renders a glyph at
//                   ANY size, which is what lets mayag put text through the
//                   same SDF shader as every other shape and keep the whole
//                   frame in one draw call.
//
// The coverage rasteriser is a signed-area accumulator (the approach FreeType
// and font-rs use): walk each edge once, deposit its exact area contribution
// into a scanline accumulator, then prefix-sum. It is O(edges + pixels), has
// no sorting, no active-edge list, and no per-scanline allocation.

#include "outline.hpp"
#include "../core/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <vector>

namespace mayag::typo {

// ════════════════════════════════════════════════════════════════════════
// Bitmap
// ════════════════════════════════════════════════════════════════════════

/// An 8-bit single-channel image. Coverage bitmaps and SDFs both live here.
struct Bitmap {
    std::vector<std::uint8_t> pixels;
    int width  = 0;
    int height = 0;

    Bitmap() = default;
    Bitmap(int w, int h) : pixels(static_cast<std::size_t>(std::max(w, 0)) * std::max(h, 0), 0),
                           width{std::max(w, 0)}, height{std::max(h, 0)} {}

    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }

    [[nodiscard]] std::uint8_t at(int x, int y) const noexcept {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
    void set(int x, int y, std::uint8_t v) noexcept {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        pixels[static_cast<std::size_t>(y) * width + x] = v;
    }

    /// Bilinear sample in normalised [0,1] coordinates — how the atlas is
    /// read back during rendering.
    [[nodiscard]] float sample(float u, float v) const noexcept {
        if (empty()) return 0.0f;
        const float fx = num::clamp(u * static_cast<float>(width)  - 0.5f, 0.0f,
                                    static_cast<float>(width  - 1));
        const float fy = num::clamp(v * static_cast<float>(height) - 0.5f, 0.0f,
                                    static_cast<float>(height - 1));
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);

        const float a = num::lerp(at(x0, y0) / 255.0f, at(x1, y0) / 255.0f, tx);
        const float b = num::lerp(at(x0, y1) / 255.0f, at(x1, y1) / 255.0f, tx);
        return num::lerp(a, b, ty);
    }
};

// ════════════════════════════════════════════════════════════════════════
// Curve flattening
// ════════════════════════════════════════════════════════════════════════

namespace detail {

/// Subdivision count for a quadratic, from its flatness. Using the control
/// polygon's deviation rather than a fixed count means a nearly-straight
/// curve costs 1 segment and a tight bowl costs as many as it needs.
[[nodiscard]] inline int quad_steps(Vec2 p0, Vec2 c, Vec2 p1, float tolerance) {
    const Vec2 d = p0 - c * 2.0f + p1;          // second difference
    const float dev = d.length();
    if (dev <= tolerance * 4.0f) return 1;
    // Error of n-segment approximation ~ dev / (8 n^2)
    const int n = static_cast<int>(std::ceil(std::sqrt(dev / (8.0f * tolerance))));
    return num::clamp(n, 1, 64);
}

[[nodiscard]] inline int cubic_steps(Vec2 p0, Vec2 c0, Vec2 c1, Vec2 p1, float tolerance) {
    const Vec2 d0 = p0 - c0 * 2.0f + c1;
    const Vec2 d1 = c0 - c1 * 2.0f + p1;
    const float dev = std::max(d0.length(), d1.length());
    if (dev <= tolerance * 4.0f) return 1;
    const int n = static_cast<int>(std::ceil(std::sqrt(dev * 3.0f / (4.0f * tolerance))));
    return num::clamp(n, 1, 96);
}

[[nodiscard]] constexpr Vec2 quad_at(Vec2 p0, Vec2 c, Vec2 p1, float t) noexcept {
    const float mt = 1.0f - t;
    return p0 * (mt * mt) + c * (2.0f * mt * t) + p1 * (t * t);
}

[[nodiscard]] constexpr Vec2 cubic_at(Vec2 p0, Vec2 c0, Vec2 c1, Vec2 p1, float t) noexcept {
    const float mt = 1.0f - t;
    return p0 * (mt * mt * mt) + c0 * (3.0f * mt * mt * t) +
           c1 * (3.0f * mt * t * t) + p1 * (t * t * t);
}

}  // namespace detail

/// A negative `tolerance` passed to flatten()/rasterize() selects
/// CoreGraphics' glyph flattening (aa_quadto / aa_cubeto, read from
/// CoreGraphics): a quadratic is halved until every component of its second
/// difference P0 - 2C + P1 is at most 0.8 device px (so it becomes 2^k equal
/// chords), a cubic is halved adaptively (de Casteljau) until both second
/// differences are at most 4/15 px per component, depth <= 22. CoreText's
/// glyph coverage is the exact area of THAT polygon — it deliberately
/// under-flattens, so an SF 'o' at 24 px holds 4% less ink than its true
/// outline, and matching macOS means flattening the same way.
/// Any negative value -s flattens the CoreGraphics way with both thresholds
/// scaled by s, for flattening in a space s times finer than device px (a
/// distance field rasterised larger than it is drawn).
inline constexpr float cg_glyph_flattening = -1.0f;

namespace detail {
inline void cg_cubic(Vec2 a, Vec2 b, Vec2 c, Vec2 d, int depth, std::vector<std::pair<Vec2, Vec2>>& edges, float s = 1.0f) {
    const Vec2 d1 = a - b * 2.0f + c, d2 = b - c * 2.0f + d;
    const float t = 4.0f / 15.0f * s;
    if (depth >= 22 || (std::abs(d1.x) <= t && std::abs(d1.y) <= t && std::abs(d2.x) <= t && std::abs(d2.y) <= t)) {
        edges.emplace_back(a, d);
        return;
    }
    const Vec2 ab = (a + b) * 0.5f, bc = (b + c) * 0.5f, cd = (c + d) * 0.5f;
    const Vec2 abc = (ab + bc) * 0.5f, bcd = (bc + cd) * 0.5f, m = (abc + bcd) * 0.5f;
    cg_cubic(a, ab, abc, m, depth + 1, edges, s);
    cg_cubic(m, bcd, cd, d, depth + 1, edges, s);
}
[[nodiscard]] inline int cg_quad_steps(Vec2 p0, Vec2 c, Vec2 p1, float s = 1.0f) noexcept {
    const Vec2 dd = p0 - c * 2.0f + p1;
    float m = std::max(std::abs(dd.x), std::abs(dd.y));
    int n = 1;
    for (int k = 0; k < 22 && m > 0.8f * s; ++k) { m *= 0.25f; n *= 2; }
    return std::min(n, 1 << 12);
}
}  // namespace detail

/// Flatten an outline into line segments under an affine transform.
/// `tolerance` is the maximum deviation in DESTINATION pixels, so the
/// subdivision adapts automatically to the render size; a negative value
/// (cg_glyph_flattening) selects CoreGraphics' glyph flattening instead.
inline void flatten(const Outline& outline, const Affine& xf, float tolerance,
                    std::vector<std::pair<Vec2, Vec2>>& edges) {
    const bool cg = tolerance < 0.0f;
    const float cg_scale = cg ? -tolerance : 1.0f;
    for (const auto& contour : outline.contours) {
        if (contour.empty()) continue;

        Vec2 cursor = xf.apply(contour.start);
        const Vec2 first = cursor;

        for (const auto& seg : contour.segments) {
            const Vec2 to = xf.apply(seg.to);
            switch (seg.kind) {
                case Segment::Kind::line:
                    edges.emplace_back(cursor, to);
                    break;

                case Segment::Kind::quad: {
                    const Vec2 c = xf.apply(seg.c0);
                    const int n = cg ? detail::cg_quad_steps(cursor, c, to, cg_scale) : detail::quad_steps(cursor, c, to, tolerance);
                    Vec2 prev = cursor;
                    for (int i = 1; i <= n; ++i) {
                        const Vec2 p = i == n ? to : detail::quad_at(cursor, c, to,
                                                       static_cast<float>(i) / static_cast<float>(n));
                        edges.emplace_back(prev, p);
                        prev = p;
                    }
                    break;
                }

                case Segment::Kind::cubic: {
                    const Vec2 c0 = xf.apply(seg.c0);
                    const Vec2 c1 = xf.apply(seg.c1);
                    if (cg) { detail::cg_cubic(cursor, c0, c1, to, 0, edges, cg_scale); break; }
                    const int n = detail::cubic_steps(cursor, c0, c1, to, tolerance);
                    Vec2 prev = cursor;
                    for (int i = 1; i <= n; ++i) {
                        const Vec2 p = detail::cubic_at(cursor, c0, c1, to,
                                                        static_cast<float>(i) / static_cast<float>(n));
                        edges.emplace_back(prev, p);
                        prev = p;
                    }
                    break;
                }
            }
            cursor = to;
        }

        // Every contour is implicitly closed.
        if (cursor != first) edges.emplace_back(cursor, first);
    }
}

// ════════════════════════════════════════════════════════════════════════
// Analytic coverage rasteriser
// ════════════════════════════════════════════════════════════════════════

/// Exact-area scanline fill.
///
/// The idea: for each edge, walk the pixels it crosses and accumulate two
/// quantities per pixel — the signed AREA it covers within that pixel, and
/// the signed HEIGHT it contributes to everything to its right. A prefix sum
/// along each scanline then turns those into exact coverage, because the
/// winding contribution of an edge to all pixels right of it is constant.
///
/// This yields the true analytic area, not an approximation, and touches each
/// pixel a bounded number of times.
class Rasterizer {
  public:
    Rasterizer(int width, int height)
        : w_{std::max(width, 0)}, h_{std::max(height, 0)},
          acc_(static_cast<std::size_t>(w_ + 1) * std::max(h_, 0), 0.0f) {}

    /// Deposit one line segment. Coordinates are in pixel space with the
    /// origin at the bitmap's top-left, y increasing downward.
    void add_edge(Vec2 p0, Vec2 p1) {
        if (p0.y == p1.y) return;                 // horizontal edges contribute nothing

        float dir = 1.0f;
        if (p0.y > p1.y) { std::swap(p0, p1); dir = -1.0f; }

        // Clip vertically to the bitmap. Edges above the top still matter for
        // winding, so we clamp rather than discard, preserving the crossing.
        const float y_top = std::max(p0.y, 0.0f);
        const float y_bot = std::min(p1.y, static_cast<float>(h_));
        if (y_top >= y_bot) return;

        const float dxdy = (p1.x - p0.x) / (p1.y - p0.y);

        int y = static_cast<int>(std::floor(y_top));
        float y_cursor = y_top;
        float x_cursor = p0.x + (y_top - p0.y) * dxdy;

        while (y_cursor < y_bot && y < h_) {
            const float y_next = std::min(static_cast<float>(y + 1), y_bot);
            const float dy     = y_next - y_cursor;
            if (dy <= 0.0f) { ++y; y_cursor = static_cast<float>(y); continue; }

            const float x_next = x_cursor + dy * dxdy;
            scanline_span(y, x_cursor, x_next, dy * dir);

            x_cursor = x_next;
            y_cursor = y_next;
            ++y;
        }
    }

    void add_edges(const std::vector<std::pair<Vec2, Vec2>>& edges) {
        for (const auto& [a, b] : edges) add_edge(a, b);
    }

    /// Resolve accumulated area into a coverage bitmap using the NON-ZERO
    /// winding rule (what TrueType and CFF both specify).
    [[nodiscard]] Bitmap resolve() const {
        Bitmap bmp{w_, h_};
        for (int y = 0; y < h_; ++y) {
            const float* row = &acc_[static_cast<std::size_t>(y) * (w_ + 1)];
            float running = 0.0f;
            for (int x = 0; x < w_; ++x) {
                running += row[x];
                // |winding| clamped to 1 is the non-zero rule; overlapping
                // contours of the same direction do not double-darken.
                const float cov = num::saturate(std::fabs(running));
                bmp.pixels[static_cast<std::size_t>(y) * w_ + x] =
                    static_cast<std::uint8_t>(cov * 255.0f + 0.5f);
            }
        }
        return bmp;
    }

  private:
    /// Distribute one edge's contribution across the pixels it crosses within
    /// a single scanline. `height` is the signed vertical extent covered.
    void scanline_span(int y, float x0, float x1, float height) {
        if (height == 0.0f) return;
        if (x1 < x0) std::swap(x0, x1);

        float* row = &acc_[static_cast<std::size_t>(y) * (w_ + 1)];

        // Fully left of the bitmap: the whole height applies to every pixel.
        if (x1 <= 0.0f) { row[0] += height; return; }
        // Fully right: contributes nothing visible.
        if (x0 >= static_cast<float>(w_)) return;

        x0 = std::max(x0, 0.0f);
        x1 = std::min(x1, static_cast<float>(w_));

        const int ix0 = static_cast<int>(x0);
        const int ix1 = static_cast<int>(std::min(x1, static_cast<float>(w_ - 1)));

        if (ix0 == ix1) {
            // The span stays inside one pixel: split the height between this
            // pixel (by the area to its right) and the next.
            const float mid = (x0 + x1) * 0.5f;
            const float right_frac = static_cast<float>(ix0 + 1) - mid;
            row[ix0]     += height * right_frac;
            row[ix0 + 1] += height * (1.0f - right_frac);
            return;
        }

        // The span crosses pixel boundaries: distribute proportionally to the
        // horizontal extent within each pixel.
        const float inv_dx = 1.0f / (x1 - x0);

        // First (partial) pixel.
        const float first_edge = static_cast<float>(ix0 + 1);
        const float first_frac = (first_edge - x0) * inv_dx;
        const float first_mid  = (x0 + first_edge) * 0.5f;
        float carried = height * first_frac;
        row[ix0]     += carried * (first_edge - first_mid);
        row[ix0 + 1] += carried * (1.0f - (first_edge - first_mid));

        // Interior pixels, each fully spanned.
        for (int x = ix0 + 1; x < ix1; ++x) {
            const float frac = inv_dx;   // exactly one pixel of width
            carried = height * frac;
            row[x]     += carried * 0.5f;
            row[x + 1] += carried * 0.5f;
        }

        // Last (partial) pixel.
        const float last_edge = static_cast<float>(ix1);
        const float last_frac = (x1 - last_edge) * inv_dx;
        const float last_mid  = (last_edge + x1) * 0.5f;
        carried = height * last_frac;
        const float right_frac = static_cast<float>(ix1 + 1) - last_mid;
        row[ix1]     += carried * right_frac;
        row[ix1 + 1] += carried * (1.0f - right_frac);
    }

    int                w_ = 0;
    int                h_ = 0;
    std::vector<float> acc_;   ///< (w+1) per row: the extra column absorbs spill
};

// ════════════════════════════════════════════════════════════════════════
// The rasterisation entry point
// ════════════════════════════════════════════════════════════════════════

struct RasterResult {
    Bitmap bitmap;
    /// Where the bitmap sits relative to the glyph origin, in pixels, y-down.
    /// The renderer adds this to the pen position.
    Vec2   offset{};
};

namespace detail {

/// Replace a polygon soup with the boundary of its NONZERO union, row by
/// row: within every pixel row, split at edge endpoints and edge crossings
/// into bands where the edges' left-to-right order is fixed, and keep only
/// the band pieces where the winding number changes between 0 and nonzero.
/// Accumulating those gives the exact union area per pixel, where summing
/// every edge and clamping over-counts pixels that two overlapping contours
/// both partly cover (0.6 + 0.6 -> 1.0 instead of the true union, e.g. 0.8).
/// CoreGraphics' glyph rasteriser resolves overlaps exactly, and HVF glyphs
/// (PingFang) and variable fonts are full of overlapping contours.
/// Edges are in bitmap pixels, y down; `h` rows.
inline void union_boundary(const std::vector<std::pair<Vec2, Vec2>>& in, int h,
                           std::vector<std::pair<Vec2, Vec2>>& out) {
    struct E { double xa, ya, xb, yb; int dir; };   // ya < yb; dir: +1 if original pointed down
    std::vector<std::vector<std::uint32_t>> rows(static_cast<std::size_t>(std::max(h, 0)));
    std::vector<E> es;
    es.reserve(in.size());
    for (const auto& [a, b] : in) {
        if (a.y == b.y) continue;
        const bool down = b.y > a.y;
        const auto d = [](float f) { return static_cast<double>(f); };
        const E e{d(down ? a.x : b.x), d(down ? a.y : b.y), d(down ? b.x : a.x), d(down ? b.y : a.y), down ? 1 : -1};
        const int r0 = std::max(0, static_cast<int>(std::floor(e.ya)));
        const int r1 = std::min(h - 1, static_cast<int>(std::ceil(e.yb)) - 1);
        for (int r = r0; r <= r1; ++r) rows[static_cast<std::size_t>(r)].push_back(static_cast<std::uint32_t>(es.size()));
        es.push_back(e);
    }
    const auto x_at = [](const E& e, double y) { return e.xa + (e.xb - e.xa) * (y - e.ya) / (e.yb - e.ya); };
    std::vector<double> ev;
    std::vector<std::pair<double, std::uint32_t>> order;
    struct Span { double x0, x1; std::uint32_t i; };
    std::vector<Span> spans;
    for (int r = 0; r < h; ++r) {
        const auto& ids = rows[static_cast<std::size_t>(r)];
        if (ids.empty()) continue;
        const double top = r, bot = r + 1.0;
        ev.assign({top, bot});
        for (const auto i : ids) {
            const E& e = es[i];
            if (e.ya > top && e.ya < bot) ev.push_back(e.ya);
            if (e.yb > top && e.yb < bot) ev.push_back(e.yb);
        }
        // Crossings: only edges whose x-spans within this row overlap can
        // cross. Sort by span start and sweep (all-pairs made HVF glyphs,
        // dozens of overlapping parts, cost ~50 us each).
        spans.clear();
        for (const auto i : ids) {
            const E& e = es[i];
            const double lo = std::max(top, e.ya), hi = std::min(bot, e.yb);
            const double xa = x_at(e, lo), xb = x_at(e, hi);
            spans.push_back({std::min(xa, xb), std::max(xa, xb), i});
        }
        std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.x0 < b.x0; });
        for (std::size_t p = 0; p < spans.size(); ++p)
            for (std::size_t q = p + 1; q < spans.size() && spans[q].x0 <= spans[p].x1; ++q) {
                const E& a = es[spans[p].i];
                const E& b = es[spans[q].i];
                const double lo = std::max({top, a.ya, b.ya}), hi = std::min({bot, a.yb, b.yb});
                if (hi <= lo) continue;
                const double da = x_at(a, lo) - x_at(b, lo), db = x_at(a, hi) - x_at(b, hi);
                if ((da < 0.0) != (db < 0.0) && da != 0.0 && db != 0.0) ev.push_back(lo + (hi - lo) * da / (da - db));
            }
        std::sort(ev.begin(), ev.end());
        for (std::size_t k = 0; k + 1 < ev.size(); ++k) {
            const double y0 = ev[k], y1 = ev[k + 1];
            if (y1 - y0 < 1e-9) continue;
            const double ym = 0.5 * (y0 + y1);
            order.clear();
            for (const auto i : ids) {
                const E& e = es[i];
                if (e.ya <= y0 + 1e-12 && e.yb >= y1 - 1e-12) order.emplace_back(x_at(e, ym), i);
            }
            if (order.empty()) continue;
            std::sort(order.begin(), order.end());
            int w = 0;
            for (const auto& [xm, i] : order) {
                const E& e = es[i];
                const int before = w;
                w += e.dir;
                if ((before == 0) == (w == 0)) continue;
                const Vec2 p0{static_cast<float>(x_at(e, y0)), static_cast<float>(y0)};
                const Vec2 p1{static_cast<float>(x_at(e, y1)), static_cast<float>(y1)};
                if (e.dir > 0) out.emplace_back(p0, p1); else out.emplace_back(p1, p0);
            }
        }
    }
}

}  // namespace detail

/// Render an outline at `scale` (pixels per font unit), with `padding` pixels
/// of empty margin on every side.
///
/// The outline arrives y-UP in font units; the bitmap is y-DOWN in pixels.
/// That flip lives here, in exactly one place.
[[nodiscard]] inline RasterResult rasterize(const Outline& outline, float scale,
                                            int padding = 0, float tolerance = 0.1f) {
    RasterResult result;
    if (outline.empty() || scale <= 0.0f) return result;

    const Rect bounds = outline.control_bounds();

    // Pixel bounds, expanded outward: a partially covered edge pixel must be
    // inside the bitmap or the glyph gets clipped.
    const float x0 = std::floor(bounds.left()  * scale) - static_cast<float>(padding);
    const float y0 = std::floor(-bounds.bottom() * scale) - static_cast<float>(padding);
    const float x1 = std::ceil (bounds.right() * scale) + static_cast<float>(padding);
    const float y1 = std::ceil (-bounds.top()  * scale) + static_cast<float>(padding);

    const int w = static_cast<int>(x1 - x0) + 1;
    const int h = static_cast<int>(y1 - y0) + 1;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return result;

    // font units (y-up) -> bitmap pixels (y-down), translated so the glyph's
    // bounding box lands at the bitmap origin.
    const Affine xf{scale, 0.0f, 0.0f, -scale, -x0, -y0};

    std::vector<std::pair<Vec2, Vec2>> edges;
    edges.reserve(outline.point_count() * 3);
    flatten(outline, xf, tolerance, edges);
    // Exact nonzero union: contours overlapping each other AND single
    // contours overlapping themselves (SF's 'e' draws its bar as a loop over
    // both strokes, winding 2 there; summing and clamping put 1.00 where
    // CoreGraphics' exact union gives 0.79).
    {
        std::vector<std::pair<Vec2, Vec2>> u;
        u.reserve(edges.size() * 2);
        detail::union_boundary(edges, h, u);
        edges.swap(u);
    }

    Rasterizer r{w, h};
    r.add_edges(edges);

    result.bitmap = r.resolve();
    result.offset = Vec2{x0, y0};
    return result;
}

// ════════════════════════════════════════════════════════════════════════
// Signed distance field
// ════════════════════════════════════════════════════════════════════════

namespace detail {

/// 8-point sequential Euclidean distance transform (Danielsson / Leymarie).
/// Two passes over the grid propagating the nearest-seed VECTOR rather than a
/// scalar distance, which is what makes the result Euclidean instead of the
/// chamfer approximation most SDF generators settle for.
struct DistPoint {
    float dx = 1e6f, dy = 1e6f;
    [[nodiscard]] constexpr float dist2() const noexcept { return dx * dx + dy * dy; }
};

inline void edt_compare(std::vector<DistPoint>& g, int w, int h,
                        int x, int y, int ox, int oy) {
    const int nx = x + ox, ny = y + oy;
    if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;

    DistPoint other = g[static_cast<std::size_t>(ny) * w + nx];
    other.dx += static_cast<float>(ox);
    other.dy += static_cast<float>(oy);

    DistPoint& cur = g[static_cast<std::size_t>(y) * w + x];
    if (other.dist2() < cur.dist2()) cur = other;
}

inline void edt_pass(std::vector<DistPoint>& g, int w, int h) {
    // Forward: propagate from above and from the left.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            edt_compare(g, w, h, x, y, -1,  0);
            edt_compare(g, w, h, x, y,  0, -1);
            edt_compare(g, w, h, x, y, -1, -1);
            edt_compare(g, w, h, x, y,  1, -1);
        }
        for (int x = w - 1; x >= 0; --x) {
            edt_compare(g, w, h, x, y, 1, 0);
        }
    }
    // Backward: propagate from below and from the right.
    for (int y = h - 1; y >= 0; --y) {
        for (int x = w - 1; x >= 0; --x) {
            edt_compare(g, w, h, x, y,  1, 0);
            edt_compare(g, w, h, x, y,  0, 1);
            edt_compare(g, w, h, x, y, -1, 1);
            edt_compare(g, w, h, x, y,  1, 1);
        }
        for (int x = 0; x < w; ++x) {
            edt_compare(g, w, h, x, y, -1, 0);
        }
    }
}

}  // namespace detail

/// Convert a coverage bitmap into a signed distance field.
///
/// Encoding: 128 is the outline, values above are inside, below are outside,
/// with `spread` pixels mapping to the full 0..255 range. That matches what
/// the SDF shader in `render/sdf.hpp` expects, so a glyph is shaded by the
/// same `coverage_smooth()` as every other mayag shape.
[[nodiscard]] inline Bitmap make_sdf(const Bitmap& coverage, float spread);

/// Faux-bold: thicken a coverage bitmap by `strength` pixels, for fonts that
/// can't reach the requested weight themselves (a static regular, or a
/// variable font past its wght-axis max). Grows the bitmap by a 1px border and
/// takes, at each pixel, the max coverage in a small horizontal+vertical
/// neighbourhood weighted by the fractional strength — a cheap morphological
/// dilation that fattens every stem the way FreeType's embolden does, without
/// touching outlines or the GPU path.
[[nodiscard]] inline Bitmap embolden(const Bitmap& src, float strength) {
    if (src.empty() || strength <= 0.0f) return src;
    const int pad = static_cast<int>(num::ceil(strength));
    Bitmap out{src.width + pad * 2, src.height + pad * 2};
    const int w = out.width, h = out.height;

    const int radius = static_cast<int>(num::ceil(strength));
    const float frac = strength - num::floor(strength);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Sample the source (offset by pad) over a square neighbourhood,
            // taking the max so strokes grow rather than blur.
            float best = 0.0f;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int sx = x - pad + dx;
                    const int sy = y - pad + dy;
                    float v = static_cast<float>(src.at(sx, sy)) / 255.0f;
                    // Feather the outermost ring by the fractional strength so
                    // sub-pixel bold values interpolate smoothly.
                    if ((std::abs(dx) == radius || std::abs(dy) == radius) && frac > 0.0f)
                        v *= frac;
                    best = num::max(best, v);
                }
            }
            out.set(x, y, static_cast<std::uint8_t>(num::clamp(best, 0.0f, 1.0f) * 255.0f + 0.5f));
        }
    }
    return out;
}

/// Signed-distance field from a coverage bitmap. Sub-pixel accuracy comes from
/// seeding the transform with the coverage value itself: a pixel that is 30%
/// covered sits 0.2 px outside the edge, not a whole pixel. Without that
/// correction SDF text has visible stair-stepping at large sizes.
[[nodiscard]] inline Bitmap make_sdf(const Bitmap& coverage, float spread = 4.0f) {
    if (coverage.empty()) return {};

    const int w = coverage.width, h = coverage.height;
    const std::size_t n = static_cast<std::size_t>(w) * h;

    std::vector<detail::DistPoint> inside(n), outside(n);

    // Seed the two fields.
    //
    // The rule that matters: a pixel is a SEED in exactly one field and
    // UNKNOWN (infinitely far) in the other. Seeding both — which is the
    // natural-looking mistake — leaves every pixel at distance zero in both
    // fields, nothing propagates, and the result is a flat grey image that
    // still faintly resembles the glyph. Ask how that got caught: the SDF
    // range test.
    //
    // The sub-pixel term is what buys accuracy below one pixel: a pixel that
    // is 70% covered has its true edge 0.2 px from its centre, not 0.
    for (std::size_t i = 0; i < n; ++i) {
        const float a = coverage.pixels[i] / 255.0f;
        if (a >= 0.5f) {
            // Inside: zero distance in the inside field, unknown outside.
            inside[i]  = detail::DistPoint{a - 0.5f, 0.0f};
            outside[i] = detail::DistPoint{};
        } else {
            // Outside: zero distance in the outside field, unknown inside.
            outside[i] = detail::DistPoint{0.5f - a, 0.0f};
            inside[i]  = detail::DistPoint{};
        }
    }

    detail::edt_pass(inside,  w, h);
    detail::edt_pass(outside, w, h);

    Bitmap sdf{w, h};
    for (std::size_t i = 0; i < n; ++i) {
        // Positive inside, negative outside. Each field holds the distance to
        // the NEAREST OPPOSITE pixel, so subtracting gives a signed distance
        // that is continuous across the boundary.
        const float d = std::sqrt(outside[i].dist2()) - std::sqrt(inside[i].dist2());
        const float normalized = num::clamp(d / spread, -1.0f, 1.0f);
        sdf.pixels[i] = static_cast<std::uint8_t>((normalized * 0.5f + 0.5f) * 255.0f + 0.5f);
    }
    return sdf;
}

/// The previous SDF route: rasterise coverage at `supersample`x and run the
/// 8SSEDT over it. Kept for comparison; its seeds carry their sub-pixel
/// distance along x only, which biases every field horizontally (~0.75
/// raster px on 'H'), so rasterize_sdf() below computes distances exactly.
[[nodiscard]] inline RasterResult rasterize_sdf_edt(const Outline& outline, float scale,
                                                    float spread = 4.0f, int supersample = 2,
                                                    float embolden_px = 0.0f) {
    const int   ss     = num::clamp(supersample, 1, 4);
    const int   pad    = static_cast<int>(std::ceil(spread)) + 1;
    const float hi_res = scale * static_cast<float>(ss);

    // Flatten curves FINER than the default 0.1 px.
    //
    // `rasterize`'s tolerance is a deviation in the pixels it is rendering
    // into, which is the right measure for a bitmap glyph: it is drawn at the
    // size it was rasterised at, so 0.1 px of chord error is 0.1 px on screen
    // and invisible. An SDF entry is different. It is rasterised ONCE and then
    // magnified -- mayag's 48 px field routinely drives 190 px display text --
    // and the flattening error magnifies with it. At 4x, 0.1 px of chord error
    // becomes 0.4 px of visible faceting, which reads as a lumpy, wobbling
    // outline on large text no matter how good the field sampling is. The
    // error is baked into the stored field, so nothing downstream can undo it.
    //
    // 0.02 px keeps the outline smooth out to ~5x magnification for the cost
    // of a few more line segments during a one-off rasterisation.
    constexpr float sdf_tolerance = 0.02f;

    RasterResult hi = rasterize(outline, hi_res, pad * ss, sdf_tolerance);
    if (hi.bitmap.empty()) return {};

    // Faux-bold at SDF sizes: thicken the high-res coverage before the field
    // is built, so every stem grows uniformly and the field stays exact. The
    // strength scales with the supersample so the effect matches the coverage
    // path once downsampled.
    if (embolden_px > 0.0f) {
        const float s = embolden_px * static_cast<float>(ss);
        const int   grow = static_cast<int>(std::ceil(s));
        hi.bitmap = embolden(hi.bitmap, s);
        hi.offset -= Vec2{static_cast<float>(grow), static_cast<float>(grow)};
    }

    Bitmap hi_sdf = make_sdf(hi.bitmap, spread * static_cast<float>(ss));

    if (ss == 1) {
        RasterResult out;
        out.bitmap = std::move(hi_sdf);
        out.offset = hi.offset;
        return out;
    }

    // Downsample by box-filtering the SDF. Averaging distances is valid
    // because the field is locally linear — which is exactly the property
    // that makes SDFs interpolate well in the first place.
    const int w = (hi_sdf.width  + ss - 1) / ss;
    const int h = (hi_sdf.height + ss - 1) / ss;
    Bitmap out_bmp{w, h};

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum = 0, count = 0;
            for (int dy = 0; dy < ss; ++dy) {
                for (int dx = 0; dx < ss; ++dx) {
                    const int sx = x * ss + dx, sy = y * ss + dy;
                    if (sx < hi_sdf.width && sy < hi_sdf.height) {
                        sum += hi_sdf.at(sx, sy);
                        ++count;
                    }
                }
            }
            out_bmp.set(x, y, static_cast<std::uint8_t>(count ? sum / count : 128));
        }
    }

    RasterResult out;
    out.bitmap = std::move(out_bmp);
    out.offset = hi.offset / static_cast<float>(ss);
    return out;
}

/// Squared L-infinity distance from (px, py) to segment ab: the minimum over
/// t of max(|dx(t)|, |dy(t)|), squared. dx and dy are linear in t, so the
/// minimum is at an end or where |dx| = |dy| (dx = dy or dx = -dy).
[[nodiscard]] inline float linf2_to_segment(float px, float py, Vec2 a, Vec2 b) noexcept {
    const float ax = a.x - px, ay = a.y - py, ex = b.x - a.x, ey = b.y - a.y;
    const auto at = [&](float t) { return num::max(std::abs(ax + t * ex), std::abs(ay + t * ey)); };
    float m = num::min(at(0.0f), at(1.0f));
    if (const float den = ex - ey; std::abs(den) > 1e-12f) {          // ax + t ex = ay + t ey
        const float t = (ay - ax) / den;
        if (t > 0.0f && t < 1.0f) m = num::min(m, at(t));
    }
    if (const float den = ex + ey; std::abs(den) > 1e-12f) {          // ax + t ex = -(ay + t ey)
        const float t = -(ax + ay) / den;
        if (t > 0.0f && t < 1.0f) m = num::min(m, at(t));
    }
    return m * m;
}

/// Render straight to an EXACT signed distance field.
///
/// Every texel centre stores its true Euclidean distance to the outline
/// (flattened at 0.02 px, see below), signed by the nonzero winding rule at
/// that centre: positive inside, encoded as 0.5 + d / (2 * spread). Nothing
/// is seeded from a coverage bitmap, so there is no supersampling, no seed
/// quantisation and no directional bias, and bilinear sampling between texels
/// reconstructs the edge to within the field's linearity. Faux-bold
/// (`embolden_px`) grows the outline by exactly that distance.
///
/// `supersample` is accepted for API compatibility and ignored.
///
/// `chebyshev` measures L-infinity distance (max(|dx|, |dy|)) instead of
/// Euclidean. Both are exact AT the outline, so the 0.5 iso-line is the same
/// glyph; but a threshold shift of g then dilates by a SQUARE kernel, every
/// edge moving g * (|nx| + |ny|), which is CoreGraphics' font-smoothing
/// growth (see Outline::embolden's `square`). Glyph SDFs use it so text
/// keeps CoreText's weight on curves and diagonals.
[[nodiscard]] inline RasterResult rasterize_sdf(const Outline& outline, float scale,
                                                float spread = 4.0f, int supersample = 2,
                                                float embolden_px = 0.0f, bool chebyshev = false,
                                                float flatten_tolerance = 0.02f) {
    (void)supersample;
    RasterResult result;
    if (outline.empty() || scale <= 0.0f || spread <= 0.0f) return result;
    embolden_px = num::max(embolden_px, 0.0f);
    const float reach = spread + embolden_px;       // distances beyond this clamp
    const int   pad   = static_cast<int>(std::ceil(reach)) + 1;

    const Rect bounds = outline.control_bounds();
    const float x0 = std::floor(bounds.left()  * scale) - static_cast<float>(pad);
    const float y0 = std::floor(-bounds.bottom() * scale) - static_cast<float>(pad);
    const float x1 = std::ceil (bounds.right() * scale) + static_cast<float>(pad);
    const float y1 = std::ceil (-bounds.top()  * scale) + static_cast<float>(pad);
    const int w = static_cast<int>(x1 - x0) + 1;
    const int h = static_cast<int>(y1 - y0) + 1;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return result;

    // Fine flattening: the field is magnified at draw time (48 px drives
    // 190 px text), so chord error must stay far below a display pixel.
    // Fine flattening by default: the field is magnified at draw time, so
    // chord error must stay far below a display pixel. A negative value
    // selects CoreGraphics' flattening (see cg_glyph_flattening) so the field
    // describes the same polygon macOS fills.
    const float sdf_tolerance = flatten_tolerance;
    const Affine xf{scale, 0.0f, 0.0f, -scale, -x0, -y0};
    std::vector<std::pair<Vec2, Vec2>> edges;
    edges.reserve(outline.point_count() * 4);
    flatten(outline, xf, sdf_tolerance, edges);
    if (edges.empty()) return result;

    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

    // Overlapping contours (variable fonts keep them; HVF glyphs are built
    // from overlapping parts) leave edges INSIDE the filled area. They are
    // not boundary, and measuring distance to them would draw a seam at
    // coverage 0.5 along every overlap. So split edges into short pieces and
    // keep only those with winding 0 on one side (nonzero rule), measured at
    // the piece midpoint +- a small step along its normal with an exact
    // point winding query over the edges crossing that row.
    std::vector<std::vector<std::uint32_t>> bucket(static_cast<std::size_t>(h));
    for (std::uint32_t e = 0; e < edges.size(); ++e) {
        const auto& [a, b] = edges[e];
        if (a.y == b.y) continue;
        const int r0 = num::max(0, static_cast<int>(std::floor(num::min(a.y, b.y))));
        const int r1 = num::min(h - 1, static_cast<int>(std::floor(num::max(a.y, b.y))));
        for (int r = r0; r <= r1; ++r) bucket[static_cast<std::size_t>(r)].push_back(e);
    }
    const auto winding_at = [&](float px, float py) {
        if (py < 0.0f || py >= static_cast<float>(h)) return 0;
        int wn = 0;
        for (const std::uint32_t e : bucket[static_cast<std::size_t>(py)]) {
            const auto& [a, b] = edges[e];
            const bool up = b.y > a.y;
            const float ylo = up ? a.y : b.y, yhi = up ? b.y : a.y;
            if (py < ylo || py >= yhi) continue;
            const float cx = a.x + (py - a.y) / (b.y - a.y) * (b.x - a.x);
            if (cx <= px) wn += up ? 1 : -1;
        }
        return wn;
    };
    std::vector<std::pair<Vec2, Vec2>> boundary;
    boundary.reserve(edges.size());
    {
        constexpr float piece = 0.25f, step = 0.05f;
        for (const auto& [a, b] : edges) {
            const float ex = b.x - a.x, ey = b.y - a.y;
            const float len = std::sqrt(ex * ex + ey * ey);
            if (len <= 0.0f) continue;
            const float nx = -ey / len * step, ny = ex / len * step;
            const int k = num::max(1, static_cast<int>(std::ceil(len / piece)));
            int run = -1;   // first piece of the current kept run
            for (int i = 0; i <= k; ++i) {
                bool keep = false;
                if (i < k) {
                    const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(k);
                    const float mx = a.x + t * ex, my = a.y + t * ey;
                    keep = winding_at(mx + nx, my + ny) == 0 || winding_at(mx - nx, my - ny) == 0;
                }
                if (keep && run < 0) run = i;
                if (!keep && run >= 0) {
                    const float t0 = static_cast<float>(run) / static_cast<float>(k);
                    const float t1 = static_cast<float>(i) / static_cast<float>(k);
                    boundary.emplace_back(Vec2{a.x + t0 * ex, a.y + t0 * ey}, Vec2{a.x + t1 * ex, a.y + t1 * ey});
                    run = -1;
                }
            }
        }
    }
    // Unsigned distance: each edge updates only the texels within `reach` of
    // it (its bounding box, grown), so cost is O(edges * reach^2), not
    // O(edges * texels).
    const float far2 = (reach + 1.0f) * (reach + 1.0f);
    std::vector<float> d2(n, far2);
    for (const auto& [a, b] : boundary) {
        const float ex = b.x - a.x, ey = b.y - a.y;
        const float len2 = ex * ex + ey * ey;
        const float inv = len2 > 0.0f ? 1.0f / len2 : 0.0f;
        const int bx0 = num::max(0, static_cast<int>(std::floor(num::min(a.x, b.x) - reach - 0.5f)));
        const int bx1 = num::min(w - 1, static_cast<int>(std::ceil(num::max(a.x, b.x) + reach - 0.5f)));
        const int by0 = num::max(0, static_cast<int>(std::floor(num::min(a.y, b.y) - reach - 0.5f)));
        const int by1 = num::min(h - 1, static_cast<int>(std::ceil(num::max(a.y, b.y) + reach - 0.5f)));
        for (int y = by0; y <= by1; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            float* row = d2.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
            for (int x = bx0; x <= bx1; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                float dd;
                if (chebyshev) {
                    dd = linf2_to_segment(px, py, a, b);
                } else {
                    const float t = num::clamp(((px - a.x) * ex + (py - a.y) * ey) * inv, 0.0f, 1.0f);
                    const float dx = px - (a.x + t * ex), dy = py - (a.y + t * ey);
                    dd = dx * dx + dy * dy;
                }
                if (dd < row[x]) row[x] = dd;
            }
        }
    }

    // Sign: nonzero winding at each texel centre, by scanline. An edge counts
    // on rows whose centre lies in [min y, max y) (half-open, so a vertex
    // shared by two edges is counted once).
    std::vector<std::vector<std::pair<float, int>>> rows(static_cast<std::size_t>(h));
    for (const auto& [a, b] : edges) {
        if (a.y == b.y) continue;
        const int dir = b.y > a.y ? 1 : -1;
        const float ylo = num::min(a.y, b.y), yhi = num::max(a.y, b.y);
        const int r0 = num::max(0, static_cast<int>(std::ceil(ylo - 0.5f)));
        const int r1 = num::min(h - 1, static_cast<int>(std::ceil(yhi - 0.5f)) - 1);
        for (int y = r0; y <= r1; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            const float t = (py - a.y) / (b.y - a.y);
            rows[static_cast<std::size_t>(y)].emplace_back(a.x + t * (b.x - a.x), dir);
        }
    }

    Bitmap sdf{w, h};
    for (int y = 0; y < h; ++y) {
        auto& cr = rows[static_cast<std::size_t>(y)];
        std::sort(cr.begin(), cr.end(), [](const auto& p, const auto& q) { return p.first < q.first; });
        std::size_t ci = 0;
        int winding = 0;
        for (int x = 0; x < w; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            while (ci < cr.size() && cr[ci].first <= px) winding += cr[ci++].second;
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            const float dist = std::sqrt(d2[i]);
            const float sd = (winding != 0 ? dist : -dist) + embolden_px;
            const float normalized = num::clamp(sd / spread, -1.0f, 1.0f);
            sdf.pixels[i] = static_cast<std::uint8_t>((normalized * 0.5f + 0.5f) * 255.0f + 0.5f);
        }
    }
    result.bitmap = std::move(sdf);
    result.offset = Vec2{x0, y0};
    return result;
}

}  // namespace mayag::typo
