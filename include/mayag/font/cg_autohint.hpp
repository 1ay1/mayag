// =============================================================================
// mayag/font/cg_autohint.hpp — CoreGraphics' vertical autohinter, ported
// =============================================================================
//
// macOS hints text vertically at INTEGER device ppem 7..18 — every font,
// hinted or not, smoothing on or off, at 1x and 2x alike (a size of 12.002
// px is drawn unhinted). The x-height and the cap height are pulled onto
// whole pixels by a piecewise-linear map of y, so 12 px SF draws its
// capitals 9 px tall (8.46 unhinted) and its x-height 7 px (6.43).
//
// Reverse-engineered from CoreGraphics (macOS 26): CGFontAutohintTransform-
// Create picks the integer targets, evaluate_autohint_transform builds the
// map, CGFontAutohintTransformApply applies it to every path point. Verified
// against the live functions under lldb on 240 cases (10 fonts x 12 sizes x
// light/dark text): every field of the transform matches to 1e-4 px.
//
// Inputs, in font units: xh = the top of 'o' (overshoot included), cap = the
// top of 'X', asc = the font bbox top (head.yMax), stem = FontParser's stemH
// metadata scaled to a 1000-unit em (0 when unknown, as for SF, Arial and
// every font without registry metadata: the cap band then defaults to its
// cap-based bound). `dilation` is the font-smoothing growth in device px,
// applied to the outline BEFORE the map (and the glyph shifted up by the
// same afterwards), exactly as CoreGraphics orders it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "outline.hpp"

namespace mayag::typo::cg_autohint {

/// The y map. All values device px, y up, relative to the baseline.
struct Transform {
    double bot_hi = 0, xh_src = 0, xh_dst = 0, capb_src = 0, capb_dst = 0, cap_src = 0, cap_dst = 0;
    double s1 = 0, s2 = 0, s3 = 0;
    int    w_xh = 0, w_cap = 0;   ///< the pixel targets chosen

    [[nodiscard]] double apply(double y) const noexcept {
        if (y < bot_hi) return y;                                       // baseline band: fixed
        if (y <= xh_src) return bot_hi + (y - bot_hi) * s1;             // up to the x-height
        if (y <= capb_src) return xh_dst + (y - xh_src) * s2;           // x-height to cap band
        if (y <= cap_src) return y + (capb_dst - capb_src);             // cap band: translated
        return cap_dst + (y - cap_src) * s3;                            // above: to the bbox top
    }
};

/// evaluate_autohint_transform: the map for targets (w1, w2) with dilation d1.
[[nodiscard]] inline Transform evaluate(double stem, double d1, double xh, double cap, double s, double asc,
                                        int w1, int w2) noexcept {
    double band = stem != 0.0 ? stem : cap;
    band = std::fmin(band, xh * 0.333);
    band = std::fmin(band, (cap - xh) * 0.5);
    const double d7 = s * band;
    Transform t;
    t.bot_hi = d1 + d7;
    t.xh_src = xh * s + d1;
    t.xh_dst = static_cast<double>(w1) - d1;
    t.capb_src = cap * s - d7 - d1;
    t.capb_dst = static_cast<double>(w2) - d1 - d7 - 2.0 * d1;
    t.cap_src = cap * s + d1;
    t.cap_dst = static_cast<double>(w2) - d1;
    const double asc_src = asc * s + d1;
    t.s1 = t.xh_src != t.bot_hi ? (t.xh_dst - t.bot_hi) / (t.xh_src - t.bot_hi) : 0.0;
    t.s2 = t.capb_src != t.xh_src ? (t.capb_dst - t.xh_dst) / (t.capb_src - t.xh_src) : 0.0;
    t.s3 = asc_src != t.cap_src ? (asc_src - t.cap_dst) / (asc_src - t.cap_src) : 0.0;
    if (t.s2 < 0.75 || t.s2 - t.s1 > 0.3) {
        // Snapping the cap height would stretch the x-height-to-cap band too
        // far: keep the cap where it is and only snap the x-height.
        t.capb_dst = t.cap_src - d7 - 2.0 * d1;
        t.cap_dst = t.cap_src;
        t.s2 = t.capb_src != t.xh_src ? (t.capb_dst - t.xh_dst) / (t.capb_src - t.xh_src) : 0.0;
        t.s3 = 1.0;
    }
    t.w_xh = w1; t.w_cap = w2;
    return t;
}

/// CoreGraphics font smoothing, the one definition: growth per side in
/// device px for a glyph at `ppem` and smoothing level (0 black .. 4 white),
/// min(kSmoothingMax, (kSmoothingBase + kSmoothingPerLevel * level) * ppem),
/// as read from CGGlyphBuilder under lldb.
inline constexpr double kSmoothingMax      = 0.3;
inline constexpr double kSmoothingBase     = 0.0058;
inline constexpr double kSmoothingPerLevel = 0.0025;
[[nodiscard]] inline double smoothing_growth(double ppem, double level) noexcept {
    return std::fmin(kSmoothingMax, (kSmoothingBase + kSmoothingPerLevel * level) * std::fmax(ppem, 0.0));
}

/// The light-text dilation CoreGraphics uses when choosing targets
/// (FontSmoothing for light text: min(0.3, 0.015125 px per px)).
[[nodiscard]] inline double light_dilation(double ppem) noexcept { return std::fmin(ppem * 0.015125, kSmoothingMax); }

/// How the pixel heights are chosen. `coregraphics`: as CoreGraphics does
/// (from the smoothing-GROWN heights, so they round up a pixel more often:
/// glyphs sit high). `nearest`: the font's own heights to the nearest pixel,
/// FreeType's light hinting -- glyphs centred where the font put them.
enum class Targets : std::uint8_t { coregraphics, nearest };

/// CGFontAutohintTransformCreate: nothing unless ppem is an integer in
/// 7..18; otherwise choose the targets and build the map for `dilation`.
[[nodiscard]] inline std::optional<Transform> create(double ppem, double upem, double xh, double cap, double asc,
                                                     double stem, double dilation,
                                                     Targets targets = Targets::coregraphics) noexcept {
    if (!(ppem > 0.0) || !(upem > 0.0) || !(xh > 0.0) || !(cap > xh)) return std::nullopt;
    const double r = std::trunc(ppem);
    if (r != ppem || r < 7.0 || r > 18.0) return std::nullopt;
    const double s = ppem / upem;
    const double dl = light_dilation(ppem);
    // Candidate A rounds the DILATED heights, candidate B the plain ones; B
    // wins only when its map stays closer to identity (sum |slope - 1| over
    // the light-dilated and undilated maps).
    int w1 = static_cast<int>(xh * s + 2.0 * dl + 0.5), w2 = static_cast<int>(cap * s + 2.0 * dl + 0.5);
    const int xb = static_cast<int>(xh * s + 0.5);
    if (targets == Targets::nearest) {
        w1 = xb;
        w2 = static_cast<int>(cap * s + 0.5);
    } else if (xb != w1) {
        const int cb = static_cast<int>(cap * s + 0.5);
        const auto dev = [](const Transform& t) { return std::fabs(t.s1 - 1.0) + std::fabs(t.s2 - 1.0); };
        const double a = dev(evaluate(stem, dl, xh, cap, s, asc, w1, w2)) + dev(evaluate(stem, 0.0, xh, cap, s, asc, w1, w2));
        const double b = dev(evaluate(stem, dl, xh, cap, s, asc, xb, cb)) + dev(evaluate(stem, 0.0, xh, cap, s, asc, xb, cb));
        if (a > b) { w1 = xb; w2 = cb; }
    }
    Transform t = evaluate(stem, dilation, xh, cap, s, asc, w1, w2);
    // Scripts whose "x-height" is nearly the cap height (CJK, where 'o' and
    // 'X' come from a Latin fallback set) are left alone when the middle
    // band would collapse.
    if (xh / cap > 0.85 && t.s2 < 0.33) return std::nullopt;
    return t;
}

// ── font-smoothing dilation (path_iterator_dilator) ─────────────────────
//
// CoreGraphics does not offset the outline. After normalising every contour
// to counter-clockwise (y up), it turns quadratics into cubics, splits cubics
// at their x and then y extrema, and TRANSLATES each piece by a corner of the
// box [-wx, wx] x [0, 2wy]: x by sign(dy)*wx, y by (1 - sign(dx))*wy, with
// signs of the piece's end-minus-start (0 when equal). Consecutive pieces
// are joined by straight lines, and the contour's close adds one more
// (zero-length) line at its start. Read from addLine / addCube /
// addCubeMonotonic / addClosepath in CoreGraphics. Input and output are in
// device px, y up.

namespace detail {
struct Piece { bool cubic; Vec2 p0, c1, c2, p3; };

inline void split_cubic(const Piece& c, double t, Piece& a, Piece& b) {
    const auto lerp = [](Vec2 u, Vec2 v, double s) {
        const double ux = static_cast<double>(u.x), uy = static_cast<double>(u.y),
                     vx = static_cast<double>(v.x), vy = static_cast<double>(v.y);
        return Vec2{static_cast<float>(ux + (vx - ux) * s), static_cast<float>(uy + (vy - uy) * s)};
    };
    const Vec2 ab = lerp(c.p0, c.c1, t), bc = lerp(c.c1, c.c2, t), cd = lerp(c.c2, c.p3, t);
    const Vec2 abc = lerp(ab, bc, t), bcd = lerp(bc, cd, t), m = lerp(abc, bcd, t);
    a = {true, c.p0, ab, abc, m};
    b = {true, m, bcd, cd, c.p3};
}

/// Roots of d/dt of one cubic coordinate in (1e-10, 1 - 1e-10), ascending.
inline int extrema(double x0, double x1, double x2, double x3, double out[2]) {
    const double a = (x3 - x0) + 3.0 * (x1 - x2);
    const double b = x2 + x0 - 2.0 * x1;
    const double c = x1 - x0;
    double r[2]; int n = 0;
    if (a == 0.0) {
        if (b == 0.0) return 0;
        r[n++] = -c / (2.0 * b);
    } else {
        const double disc = b * b - a * c;
        if (disc < 0.0) return 0;
        const double q = -(b + (b < 0.0 ? -1.0 : 1.0) * std::sqrt(disc));
        r[n++] = q / a;
        if (q != 0.0) r[n++] = c / q;
    }
    if (n == 2 && r[1] < r[0]) std::swap(r[0], r[1]);
    int k = 0;
    for (int i = 0; i < n; ++i)
        if (r[i] >= 1e-10 && r[i] <= 0.9999999999 && (k == 0 || r[i] > out[k - 1])) out[k++] = r[i];
    return k;
}

/// Split at the extrema of one coordinate (axis 0 = x, 1 = y).
inline void split_monotonic(const Piece& c, int axis, std::vector<Piece>& out) {
    const auto co = [axis](Vec2 v) { return static_cast<double>(axis == 0 ? v.x : v.y); };
    double t[2];
    const int n = extrema(co(c.p0), co(c.c1), co(c.c2), co(c.p3), t);
    Piece rest = c; double done = 0.0;
    for (int i = 0; i < n; ++i) {
        Piece a, b;
        split_cubic(rest, (t[i] - done) / (1.0 - done), a, b);
        out.push_back(a);
        rest = b; done = t[i];
    }
    out.push_back(rest);
}
}  // namespace detail

[[nodiscard]] inline Outline dilate(const Outline& in, float wx, float wy) {
    // Orientation: CCW (positive area, y up) for the glyph as a whole.
    double area = 0.0;
    const auto cross = [](Vec2 a, Vec2 b) {
        return static_cast<double>(a.x) * static_cast<double>(b.y) - static_cast<double>(b.x) * static_cast<double>(a.y);
    };
    for (const auto& c : in.contours) {
        Vec2 p = c.start;
        for (const auto& s : c.segments) {
            if (s.kind != Segment::Kind::line) { area += cross(p, s.c0); p = s.c0; }
            if (s.kind == Segment::Kind::cubic) { area += cross(p, s.c1); p = s.c1; }
            area += cross(p, s.to); p = s.to;
        }
        area += cross(p, c.start);
    }
    const bool reverse = area < 0.0;

    Outline out;
    std::vector<detail::Piece> pieces, mono, mono2;
    for (const auto& c : in.contours) {
        if (c.segments.empty()) continue;
        // Explicit pieces (quads as cubics), closed back to the start.
        pieces.clear();
        Vec2 p = c.start;
        for (const auto& s : c.segments) {
            if (s.kind == Segment::Kind::line) pieces.push_back({false, p, {}, {}, s.to});
            else if (s.kind == Segment::Kind::quad) {
                const Vec2 c1{p.x / 3.0f + 2.0f * s.c0.x / 3.0f, p.y / 3.0f + 2.0f * s.c0.y / 3.0f};
                const Vec2 c2{s.to.x / 3.0f + 2.0f * s.c0.x / 3.0f, s.to.y / 3.0f + 2.0f * s.c0.y / 3.0f};
                pieces.push_back({true, p, c1, c2, s.to});
            } else pieces.push_back({true, p, s.c0, s.c1, s.to});
            p = s.to;
        }
        if (!(p == c.start)) pieces.push_back({false, p, {}, {}, c.start});
        if (reverse) {
            std::reverse(pieces.begin(), pieces.end());
            for (auto& q : pieces) { std::swap(q.p0, q.p3); std::swap(q.c1, q.c2); }
        }
        // The close: a zero-length line at the contour start.
        pieces.push_back({false, pieces.back().p3, {}, {}, pieces.back().p3});

        Contour oc;
        bool first = true;
        const auto emit = [&](const detail::Piece& q) {
            const float dx = q.p3.x - q.p0.x, dy = q.p3.y - q.p0.y;
            const Vec2 sh{dy < 0.0f ? -wx : (dy > 0.0f ? wx : 0.0f),
                          (dx < 0.0f ? wy : (dx > 0.0f ? -wy : 0.0f)) + wy};
            const Vec2 s0 = q.p0 + sh;
            if (first) { oc.start = s0; first = false; }
            else {
                Segment j; j.kind = Segment::Kind::line; j.to = s0;
                oc.segments.push_back(j);
            }
            Segment g;
            g.to = q.p3 + sh;
            if (q.cubic) { g.kind = Segment::Kind::cubic; g.c0 = q.c1 + sh; g.c1 = q.c2 + sh; }
            else g.kind = Segment::Kind::line;
            oc.segments.push_back(g);
        };
        for (const auto& q : pieces) {
            if (!q.cubic) { emit(q); continue; }
            mono.clear();
            detail::split_monotonic(q, 0, mono);
            for (const auto& m : mono) {
                mono2.clear();
                detail::split_monotonic(m, 1, mono2);
                for (const auto& m2 : mono2) emit(m2);
            }
        }
        // close back to the first emitted point
        if (!(oc.segments.empty()) && !(oc.segments.back().to == oc.start)) {
            Segment j; j.kind = Segment::Kind::line; j.to = oc.start;
            oc.segments.push_back(j);
        }
        out.contours.push_back(std::move(oc));
    }
    return out;
}

}  // namespace mayag::typo::cg_autohint
