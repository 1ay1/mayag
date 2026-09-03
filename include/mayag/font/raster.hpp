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

/// Flatten an outline into line segments under an affine transform.
/// `tolerance` is the maximum deviation in DESTINATION pixels, so the
/// subdivision adapts automatically to the render size.
inline void flatten(const Outline& outline, const Affine& xf, float tolerance,
                    std::vector<std::pair<Vec2, Vec2>>& edges) {
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
                    const int n = detail::quad_steps(cursor, c, to, tolerance);
                    Vec2 prev = cursor;
                    for (int i = 1; i <= n; ++i) {
                        const Vec2 p = detail::quad_at(cursor, c, to,
                                                       static_cast<float>(i) / static_cast<float>(n));
                        edges.emplace_back(prev, p);
                        prev = p;
                    }
                    break;
                }

                case Segment::Kind::cubic: {
                    const Vec2 c0 = xf.apply(seg.c0);
                    const Vec2 c1 = xf.apply(seg.c1);
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
///
/// Sub-pixel accuracy comes from seeding the transform with the coverage
/// value itself: a pixel that is 30% covered sits 0.2 px outside the edge,
/// not a whole pixel. Without that correction SDF text has visible stair-
/// stepping at large sizes, which is the whole thing SDFs are supposed to fix.
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

/// Render straight to an SDF. Rasterises at a higher internal resolution than
/// the SDF needs, because the distance transform's accuracy is bounded by the
/// coverage bitmap it is seeded from.
[[nodiscard]] inline RasterResult rasterize_sdf(const Outline& outline, float scale,
                                                float spread = 4.0f, int supersample = 2) {
    const int   ss     = num::clamp(supersample, 1, 4);
    const int   pad    = static_cast<int>(std::ceil(spread)) + 1;
    const float hi_res = scale * static_cast<float>(ss);

    RasterResult hi = rasterize(outline, hi_res, pad * ss);
    if (hi.bitmap.empty()) return {};

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

}  // namespace mayag::typo
