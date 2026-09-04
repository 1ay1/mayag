#pragma once
// mayag::typo::lastresort — a synthesized TrueType face, built in memory
//
// mayag used to ship a second, parallel text engine: `strokefont`, a hardcoded
// set of line segments with its own measurer, its own glyph renderer, and its
// own coverage sampler. It existed for exactly one reason — so that an app on
// a machine with no discoverable fonts still rendered *something* instead of
// nothing. That is a real requirement, but a whole second rendering path is a
// heavy way to meet it: two code paths to keep in sync, two sets of metrics,
// and a permanent "which engine am I on" branch through the runtime.
//
// This replaces it with a single idea: **synthesize a real TrueType font in
// memory** and feed it through the one font engine mayag already has. The
// last-resort face is then just another `Face` — same measurer, same
// rasteriser, same atlas, same SDF — and the entire strokefont path, and the
// branch that selected it, disappears.
//
// The glyph shapes are the old stroke data, re-expressed: each stroke becomes
// a thin quadrilateral contour, so the letters are still recognisable (if
// plain) and now flow through `glyf` → outline → coverage like every other
// font. Coverage is ASCII plus a `.notdef` box, which is all a genuine
// last-resort face needs — its job is to keep an app legible on a broken
// system, not to be beautiful.
//
// The bytes are a valid sfnt: head, maxp, hhea, hmtx, cmap (format 4), loca,
// glyf, name, post, OS/2. The parser in `opentype.hpp` reads it back with no
// special cases.

#include "font.hpp"
#include "opentype.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mayag::typo::lastresort {

// ── the letterforms ──────────────────────────────────────────────────────
//
// Segment lists on a [0,1] em box, y DOWN, matching the old strokefont design
// grid so the shapes are identical. Kept minimal: uppercase, digits, and the
// punctuation a UI actually shows. Everything else maps to `.notdef` (a box),
// which is the correct "I cannot draw this" signal.

struct Seg { float x0, y0, x1, y1; };

namespace grid {
constexpr float T = 0.18f, M = 0.49f, B = 0.80f, D = 0.95f;
constexpr float L = 0.12f, R = 0.60f, C = 0.36f;
}  // namespace grid

struct StrokeGlyph {
    std::array<Seg, 12> segs{};
    std::uint8_t        count = 0;
    constexpr StrokeGlyph() = default;
    constexpr StrokeGlyph(std::initializer_list<Seg> s) {
        for (const Seg& seg : s) if (count < segs.size()) segs[count++] = seg;
    }
};

// A compact stroke alphabet. Uppercase letters, digits, space and the common
// punctuation. Using the grid constants above keeps each definition legible.
[[nodiscard]] constexpr StrokeGlyph stroke_for(char ch) {
    using namespace grid;
    switch (ch) {
        case 'A': return {{L,B, C,T}, {C,T, R,B}, {L+0.09f,M+0.12f, R-0.09f,M+0.12f}};
        case 'B': return {{L,T, L,B}, {L,T, R-0.06f,T}, {R-0.06f,T, R,M-0.06f},
                          {R,M-0.06f, L,M}, {L,M, R,M+0.06f}, {R,M+0.06f, R,B-0.06f},
                          {R,B-0.06f, L,B}};
        case 'C': return {{R,T+0.06f, L+0.06f,T}, {L+0.06f,T, L,M}, {L,M, L+0.06f,B},
                          {L+0.06f,B, R,B-0.06f}};
        case 'D': return {{L,T, L,B}, {L,T, R-0.08f,T+0.06f}, {R-0.08f,T+0.06f, R,M},
                          {R,M, R-0.08f,B-0.06f}, {R-0.08f,B-0.06f, L,B}};
        case 'E': return {{R,T, L,T}, {L,T, L,B}, {L,B, R,B}, {L,M, R-0.10f,M}};
        case 'F': return {{R,T, L,T}, {L,T, L,B}, {L,M, R-0.10f,M}};
        case 'G': return {{R,T+0.06f, L+0.06f,T}, {L+0.06f,T, L,B}, {L,B, R,B},
                          {R,B, R,M}, {R,M, C+0.04f,M}};
        case 'H': return {{L,T, L,B}, {R,T, R,B}, {L,M, R,M}};
        case 'I': return {{C,T, C,B}, {C-0.12f,T, C+0.12f,T}, {C-0.12f,B, C+0.12f,B}};
        case 'J': return {{R,T, R,B-0.06f}, {R,B-0.06f, C,B}, {C,B, L,B-0.10f}};
        case 'K': return {{L,T, L,B}, {R,T, L,M}, {L,M, R,B}};
        case 'L': return {{L,T, L,B}, {L,B, R,B}};
        case 'M': return {{L,B, L,T}, {L,T, C,M}, {C,M, R,T}, {R,T, R,B}};
        case 'N': return {{L,B, L,T}, {L,T, R,B}, {R,B, R,T}};
        case 'O': return {{L,M, L+0.06f,T}, {L+0.06f,T, R-0.06f,T}, {R-0.06f,T, R,M},
                          {R,M, R-0.06f,B}, {R-0.06f,B, L+0.06f,B}, {L+0.06f,B, L,M}};
        case 'P': return {{L,B, L,T}, {L,T, R,T+0.06f}, {R,T+0.06f, R,M-0.06f},
                          {R,M-0.06f, L,M}};
        case 'Q': return {{L,M, L+0.06f,T}, {L+0.06f,T, R-0.06f,T}, {R-0.06f,T, R,M},
                          {R,M, R-0.06f,B}, {R-0.06f,B, L+0.06f,B}, {L+0.06f,B, L,M},
                          {C,M+0.10f, R,B}};
        case 'R': return {{L,B, L,T}, {L,T, R,T+0.06f}, {R,T+0.06f, R,M-0.06f},
                          {R,M-0.06f, L,M}, {C,M, R,B}};
        case 'S': return {{R,T+0.06f, L+0.06f,T}, {L+0.06f,T, L,M-0.04f},
                          {L,M-0.04f, R,M+0.04f}, {R,M+0.04f, R,B-0.06f},
                          {R,B-0.06f, L,B}};
        case 'T': return {{L,T, R,T}, {C,T, C,B}};
        case 'U': return {{L,T, L,B-0.06f}, {L,B-0.06f, C,B}, {C,B, R,B-0.06f},
                          {R,B-0.06f, R,T}};
        case 'V': return {{L,T, C,B}, {C,B, R,T}};
        case 'W': return {{L,T, L+0.10f,B}, {L+0.10f,B, C,M+0.10f},
                          {C,M+0.10f, R-0.10f,B}, {R-0.10f,B, R,T}};
        case 'X': return {{L,T, R,B}, {R,T, L,B}};
        case 'Y': return {{L,T, C,M}, {R,T, C,M}, {C,M, C,B}};
        case 'Z': return {{L,T, R,T}, {R,T, L,B}, {L,B, R,B}};
        case '0': return {{L,M, L+0.06f,T}, {L+0.06f,T, R-0.06f,T}, {R-0.06f,T, R,M},
                          {R,M, R-0.06f,B}, {R-0.06f,B, L+0.06f,B}, {L+0.06f,B, L,M},
                          {L,B, R,T}};
        case '1': return {{L+0.06f,T+0.10f, C,T}, {C,T, C,B}, {L,B, R,B}};
        case '2': return {{L,T+0.06f, R,T+0.06f}, {R,T+0.06f, R,M}, {R,M, L,B},
                          {L,B, R,B}};
        case '3': return {{L,T, R,T}, {R,T, C,M}, {C,M, R,M+0.06f}, {R,M+0.06f, R,B-0.06f},
                          {R,B-0.06f, L,B}};
        case '4': return {{R-0.06f,T, L,M+0.06f}, {L,M+0.06f, R,M+0.06f}, {R-0.06f,T, R-0.06f,B}};
        case '5': return {{R,T, L,T}, {L,T, L,M}, {L,M, R,M+0.04f}, {R,M+0.04f, R,B-0.06f},
                          {R,B-0.06f, L,B}};
        case '6': return {{R,T+0.06f, L+0.06f,T}, {L+0.06f,T, L,B}, {L,B, R,B},
                          {R,B, R,M}, {R,M, L,M}};
        case '7': return {{L,T, R,T}, {R,T, C,B}};
        case '8': return {{L+0.06f,M-0.02f, L,T+0.10f}, {L,T+0.10f, R,T+0.10f},
                          {R,T+0.10f, R-0.06f,M-0.02f}, {R-0.06f,M-0.02f, L+0.06f,M-0.02f},
                          {L+0.06f,M-0.02f, L,B-0.06f}, {L,B-0.06f, R,B-0.06f},
                          {R,B-0.06f, R-0.06f,M-0.02f}};
        case '9': return {{R,B-0.06f, R,T}, {R,T, L,T}, {L,T, L,M}, {L,M, R,M}};
        case '.': return {{C-0.03f,B-0.02f, C+0.03f,B}};
        case ',': return {{C,B-0.04f, C-0.04f,D}};
        case ':': return {{C-0.03f,M-0.06f, C+0.03f,M}, {C-0.03f,B-0.06f, C+0.03f,B}};
        case ';': return {{C-0.03f,M-0.06f, C+0.03f,M}, {C,B-0.02f, C-0.04f,D}};
        case '-': return {{L,M, R,M}};
        case '_': return {{L,B+0.10f, R,B+0.10f}};
        case '+': return {{L+0.06f,M, R-0.06f,M}, {C,M-0.14f, C,M+0.14f}};
        case '=': return {{L+0.06f,M-0.08f, R-0.06f,M-0.08f}, {L+0.06f,M+0.08f, R-0.06f,M+0.08f}};
        case '/': return {{L,B, R,T}};
        case '\\':return {{L,T, R,B}};
        case '(': return {{C+0.04f,T, L+0.02f,M}, {L+0.02f,M, C+0.04f,B}};
        case ')': return {{C-0.04f,T, R-0.02f,M}, {R-0.02f,M, C-0.04f,B}};
        case '[': return {{C+0.04f,T, L+0.06f,T}, {L+0.06f,T, L+0.06f,B}, {L+0.06f,B, C+0.04f,B}};
        case ']': return {{C-0.04f,T, R-0.06f,T}, {R-0.06f,T, R-0.06f,B}, {R-0.06f,B, C-0.04f,B}};
        case '!': return {{C,T, C,B-0.14f}, {C-0.02f,B-0.02f, C+0.02f,B}};
        case '?': return {{L,T+0.06f, R,T+0.04f}, {R,T+0.04f, R,M}, {R,M, C,M+0.08f},
                          {C,M+0.08f, C,B-0.12f}, {C-0.02f,B-0.02f, C+0.02f,B}};
        case '*': return {{C,T+0.04f, C,M}, {L+0.06f,T+0.10f, R-0.06f,M-0.04f},
                          {R-0.06f,T+0.10f, L+0.06f,M-0.04f}};
        case '#': return {{L+0.08f,T+0.06f, L+0.02f,B}, {R-0.08f,T+0.06f, R-0.14f,B},
                          {L,M-0.08f, R,M-0.08f}, {L,M+0.10f, R,M+0.10f}};
        case '%': return {{R,T, L,B}, {L+0.03f,T+0.03f, L+0.03f,T+0.04f},
                          {R-0.03f,B-0.03f, R-0.03f,B-0.02f}};
        case '@': return {{R,M, C,M-0.06f}, {C,M-0.06f, C,M+0.06f}, {C,M+0.06f, R,M},
                          {R,M, R,T}, {R,T, L,T}, {L,T, L,B}, {L,B, R,B}};
        case '&': return {{R,B, L,M}, {L,M, C,T}, {C,T, R-0.10f,M-0.06f},
                          {R-0.10f,M-0.06f, L,B}, {L,B, R,M+0.04f}};
        case '"': return {{C-0.06f,T, C-0.06f,T+0.14f}, {C+0.06f,T, C+0.06f,T+0.14f}};
        case '\'':return {{C,T, C,T+0.14f}};
        case '<': return {{R,T+0.06f, L,M}, {L,M, R,B-0.06f}};
        case '>': return {{L,T+0.06f, R,M}, {R,M, L,B-0.06f}};
        default:  return {};   // space and unknown => blank (still advances)
    }
}

// ── sfnt assembly ────────────────────────────────────────────────────────

namespace detail {

constexpr int    UPEM        = 1000;   // design grid
constexpr int    ASCENT      = 800;
constexpr int    DESCENT     = 200;
constexpr int    ADVANCE     = 600;    // matches the old monospace advance
constexpr int    STROKE_HALF = 34;     // half stroke thickness, font units

inline void put16(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
inline void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
inline void puti16(std::vector<std::uint8_t>& b, int v) {
    put16(b, static_cast<std::uint32_t>(static_cast<std::int16_t>(v)) & 0xFFFF);
}
inline void pad4(std::vector<std::uint8_t>& b) {
    while ((b.size() & 3) != 0) b.push_back(0);
}

/// Convert em-box [0,1] y-down coordinates to font units, y-up.
struct Pt { int x, y; };
[[nodiscard]] inline Pt to_units(float x, float y) {
    return {static_cast<int>(x * UPEM + 0.5f),
            static_cast<int>((1.0f - y) * UPEM) - DESCENT};
}

/// A quadrilateral contour perpendicular to a stroke, of half-width `STROKE_HALF`.
struct Contour { std::vector<Pt> pts; };

[[nodiscard]] inline Contour stroke_quad(const Seg& s) {
    const Pt a = to_units(s.x0, s.y0);
    const Pt b = to_units(s.x1, s.y1);
    double dx = b.x - a.x, dy = b.y - a.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0) { dx = 1.0; dy = 0.0; }
    else           { dx /= len; dy /= len; }
    // perpendicular
    const double px = -dy * STROKE_HALF, py = dx * STROKE_HALF;
    Contour c;
    c.pts = {
        {static_cast<int>(a.x + px), static_cast<int>(a.y + py)},
        {static_cast<int>(b.x + px), static_cast<int>(b.y + py)},
        {static_cast<int>(b.x - px), static_cast<int>(b.y - py)},
        {static_cast<int>(a.x - px), static_cast<int>(a.y - py)},
    };
    return c;
}

/// One glyph's `glyf` bytes (simple glyph, on-curve points only).
struct GlyfEntry { std::vector<std::uint8_t> bytes; int xmin, ymin, xmax, ymax; };

[[nodiscard]] inline GlyfEntry build_glyf(const std::vector<Contour>& contours) {
    GlyfEntry g;
    if (contours.empty()) { g.xmin = g.ymin = g.xmax = g.ymax = 0; return g; }

    int xmin = 1 << 30, ymin = 1 << 30, xmax = -(1 << 30), ymax = -(1 << 30);
    std::vector<std::uint16_t> end_pts;
    std::vector<Pt> all;
    for (const auto& c : contours) {
        for (const auto& p : c.pts) {
            all.push_back(p);
            xmin = p.x < xmin ? p.x : xmin; ymin = p.y < ymin ? p.y : ymin;
            xmax = p.x > xmax ? p.x : xmax; ymax = p.y > ymax ? p.y : ymax;
        }
        end_pts.push_back(static_cast<std::uint16_t>(all.size() - 1));
    }
    g.xmin = xmin; g.ymin = ymin; g.xmax = xmax; g.ymax = ymax;

    auto& b = g.bytes;
    puti16(b, static_cast<int>(contours.size()));  // numberOfContours
    puti16(b, xmin); puti16(b, ymin); puti16(b, xmax); puti16(b, ymax);
    for (std::uint16_t e : end_pts) put16(b, e);
    put16(b, 0);   // instructionLength

    // Flags: every point on-curve (0x01), no repeat, x/y as signed shorts.
    for (std::size_t i = 0; i < all.size(); ++i) b.push_back(0x01);
    int prev = 0;
    for (const auto& p : all) { puti16(b, p.x - prev); prev = p.x; }
    prev = 0;
    for (const auto& p : all) { puti16(b, p.y - prev); prev = p.y; }
    return g;
}

struct Table { std::uint32_t tag; std::vector<std::uint8_t> data; };

[[nodiscard]] inline std::uint32_t checksum(const std::vector<std::uint8_t>& d) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < d.size(); i += 4) {
        std::uint32_t w = 0;
        for (int j = 0; j < 4; ++j) {
            w <<= 8;
            if (i + static_cast<std::size_t>(j) < d.size()) w |= d[i + j];
        }
        sum += w;
    }
    return sum;
}

/// Assemble a complete sfnt from a table set.
[[nodiscard]] inline std::vector<std::uint8_t> assemble(std::vector<Table> tables) {
    std::sort(tables.begin(), tables.end(),
              [](const Table& a, const Table& b) { return a.tag < b.tag; });

    const std::uint16_t n = static_cast<std::uint16_t>(tables.size());
    std::uint16_t search = 1, entry_sel = 0;
    while (static_cast<std::uint32_t>(search) * 2 <= n) { search <<= 1; ++entry_sel; }
    const std::uint16_t range_shift = static_cast<std::uint16_t>(n * 16 - search * 16);

    std::vector<std::uint8_t> out;
    put32(out, 0x00010000);       // sfnt version (TrueType)
    put16(out, n);
    put16(out, static_cast<std::uint16_t>(search * 16));
    put16(out, entry_sel);
    put16(out, range_shift);

    std::uint32_t offset = 12u + 16u * n;
    struct Rec { std::uint32_t tag, csum, off, len; };
    std::vector<Rec> recs;
    for (auto& t : tables) {
        while ((t.data.size() & 3) != 0) t.data.push_back(0);
        recs.push_back({t.tag, checksum(t.data), offset,
                        static_cast<std::uint32_t>(t.data.size())});
        offset += static_cast<std::uint32_t>(t.data.size());
    }
    for (const auto& r : recs) {
        put32(out, r.tag); put32(out, r.csum); put32(out, r.off); put32(out, r.len);
    }
    for (const auto& t : tables) out.insert(out.end(), t.data.begin(), t.data.end());
    return out;
}

/// The full byte-image of the last-resort font. Built once.
[[nodiscard]] inline std::vector<std::uint8_t> build_font_bytes() {
    // Glyph 0 is .notdef (a hollow box). Then a run of printable ASCII.
    constexpr char first = 0x20, last = 0x7E;
    const int printable = last - first + 1;
    const std::uint16_t num_glyphs = static_cast<std::uint16_t>(1 + printable);

    // ── glyf + loca ──────────────────────────────────────────────────────
    std::vector<std::uint8_t> glyf;
    std::vector<std::uint32_t> loca;   // offsets into glyf (byte offsets)

    auto append_glyph = [&](const std::vector<Contour>& contours) {
        loca.push_back(static_cast<std::uint32_t>(glyf.size()));
        GlyfEntry g = build_glyf(contours);
        // pad glyph to even length (TrueType requirement for short loca /2)
        while ((g.bytes.size() & 1) != 0) g.bytes.push_back(0);
        glyf.insert(glyf.end(), g.bytes.begin(), g.bytes.end());
    };

    // .notdef: an open rectangle.
    {
        const int x0 = 80, x1 = ADVANCE - 80, y0 = -DESCENT + 40, y1 = ASCENT - 40;
        Contour outer{{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
        const int t = STROKE_HALF;
        Contour inner{{{x0 + t, y0 + t}, {x0 + t, y1 - t},
                       {x1 - t, y1 - t}, {x1 - t, y0 + t}}};   // reverse winding
        append_glyph({outer, inner});
    }

    int max_pts = 8, max_contours = 2;
    for (char ch = first; ch <= last; ++ch) {
        const StrokeGlyph sg = stroke_for(ch);
        std::vector<Contour> contours;
        for (std::uint8_t i = 0; i < sg.count; ++i) contours.push_back(stroke_quad(sg.segs[i]));
        int pts = 0;
        for (const auto& c : contours) pts += static_cast<int>(c.pts.size());
        max_pts      = pts > max_pts ? pts : max_pts;
        max_contours = static_cast<int>(contours.size()) > max_contours
                           ? static_cast<int>(contours.size()) : max_contours;
        append_glyph(contours);
    }
    loca.push_back(static_cast<std::uint32_t>(glyf.size()));   // end sentinel

    // loca: use long format (offsets are byte offsets) for simplicity.
    std::vector<std::uint8_t> loca_bytes;
    for (std::uint32_t o : loca) put32(loca_bytes, o);

    // ── head ─────────────────────────────────────────────────────────────
    std::vector<std::uint8_t> head;
    put32(head, 0x00010000);          // version
    put32(head, 0x00010000);          // fontRevision
    put32(head, 0);                   // checkSumAdjustment (patched by tools; parser ignores)
    put32(head, 0x5F0F3CF5);          // magic
    put16(head, 0x000B);              // flags
    put16(head, UPEM);                // unitsPerEm
    put32(head, 0); put32(head, 0);   // created
    put32(head, 0); put32(head, 0);   // modified
    puti16(head, 0); puti16(head, -DESCENT);
    puti16(head, ADVANCE); puti16(head, ASCENT);   // bbox
    put16(head, 0);                   // macStyle
    put16(head, 8);                   // lowestRecPPEM
    puti16(head, 2);                  // fontDirectionHint
    puti16(head, 1);                  // indexToLocFormat = 1 (long)
    puti16(head, 0);                  // glyphDataFormat

    // ── maxp ─────────────────────────────────────────────────────────────
    std::vector<std::uint8_t> maxp;
    put32(maxp, 0x00010000);
    put16(maxp, num_glyphs);
    put16(maxp, static_cast<std::uint16_t>(max_pts));
    put16(maxp, static_cast<std::uint16_t>(max_contours));
    put16(maxp, 0); put16(maxp, 0);           // composite max
    put16(maxp, 0); put16(maxp, 0);
    put16(maxp, 1);                            // maxZones
    put16(maxp, 0); put16(maxp, 0); put16(maxp, 0);
    put16(maxp, 0); put16(maxp, 0);

    // ── hhea + hmtx ──────────────────────────────────────────────────────
    std::vector<std::uint8_t> hhea;
    put32(hhea, 0x00010000);
    puti16(hhea, ASCENT); puti16(hhea, -DESCENT); puti16(hhea, 90);   // ascender/descender/lineGap
    put16(hhea, ADVANCE);                        // advanceWidthMax
    puti16(hhea, 0); puti16(hhea, 0);            // min side bearings
    puti16(hhea, ADVANCE);                       // xMaxExtent
    puti16(hhea, 1); puti16(hhea, 0);            // caret slope
    puti16(hhea, 0);
    put16(hhea, 0); put16(hhea, 0); put16(hhea, 0); put16(hhea, 0);   // reserved
    puti16(hhea, 0);                             // metricDataFormat
    put16(hhea, num_glyphs);                     // numberOfHMetrics

    std::vector<std::uint8_t> hmtx;
    for (std::uint16_t i = 0; i < num_glyphs; ++i) { put16(hmtx, ADVANCE); puti16(hmtx, 0); }

    // ── cmap (format 4, one segment covering the printable ASCII run) ─────
    std::vector<std::uint8_t> cmap;
    put16(cmap, 0);        // version
    put16(cmap, 1);        // numTables
    put16(cmap, 3);        // platformID = Windows
    put16(cmap, 1);        // encodingID = Unicode BMP
    put32(cmap, 12);       // offset to subtable
    // subtable format 4
    std::vector<std::uint8_t> sub;
    const std::uint16_t segs = 2;                 // one real + terminator
    put16(sub, 4);                                // format
    put16(sub, 0);                                // length (patched below)
    put16(sub, 0);                                // language
    put16(sub, segs * 2);                         // segCountX2
    std::uint16_t sr = 2, es = 0;
    while (sr * 2u <= segs) { sr <<= 1; ++es; }
    put16(sub, static_cast<std::uint16_t>(sr));   // searchRange
    put16(sub, es);                               // entrySelector
    put16(sub, static_cast<std::uint16_t>(segs * 2 - sr));   // rangeShift
    // endCode[]
    put16(sub, static_cast<std::uint16_t>(last)); put16(sub, 0xFFFF);
    put16(sub, 0);                                // reservedPad
    // startCode[]
    put16(sub, static_cast<std::uint16_t>(first)); put16(sub, 0xFFFF);
    // idDelta[]: gid = char - first + 1 => delta = 1 - first
    puti16(sub, 1 - first); put16(sub, 1);
    // idRangeOffset[]
    put16(sub, 0); put16(sub, 0);
    // patch length
    const std::uint16_t sub_len = static_cast<std::uint16_t>(sub.size());
    sub[2] = static_cast<std::uint8_t>((sub_len >> 8) & 0xFF);
    sub[3] = static_cast<std::uint8_t>(sub_len & 0xFF);
    cmap.insert(cmap.end(), sub.begin(), sub.end());

    // ── name (minimal: family + subfamily + full) ────────────────────────
    auto name_table = [] {
        struct NameRec { std::uint16_t id; std::string val; };
        const std::vector<NameRec> names = {
            {1, "mayag Last Resort"}, {2, "Regular"},
            {4, "mayag Last Resort"}, {6, "mayagLastResort"},
        };
        std::vector<std::uint8_t> strs;
        struct Entry { std::uint16_t id, off, len; };
        std::vector<Entry> entries;
        for (const auto& n : names) {
            const std::uint16_t off = static_cast<std::uint16_t>(strs.size());
            for (char c : n.val) { strs.push_back(0); strs.push_back(static_cast<std::uint8_t>(c)); }
            entries.push_back({n.id, off, static_cast<std::uint16_t>(n.val.size() * 2)});
        }
        std::vector<std::uint8_t> t;
        put16(t, 0);                                            // format
        put16(t, static_cast<std::uint16_t>(entries.size()));   // count
        const std::uint16_t str_off = static_cast<std::uint16_t>(6 + entries.size() * 12);
        put16(t, str_off);
        for (const auto& e : entries) {
            put16(t, 3); put16(t, 1); put16(t, 0x0409);         // Windows / Unicode / en-US
            put16(t, e.id); put16(t, e.len); put16(t, e.off);
        }
        t.insert(t.end(), strs.begin(), strs.end());
        return t;
    }();

    // ── post (version 3: no glyph names) ─────────────────────────────────
    std::vector<std::uint8_t> post;
    put32(post, 0x00030000);
    put32(post, 0);            // italicAngle
    puti16(post, -100); put16(post, 50);   // underline
    put32(post, 0);            // isFixedPitch (0: not claimed; measured elsewhere)
    put32(post, 0); put32(post, 0); put32(post, 0); put32(post, 0);

    // ── OS/2 (version 4) ─────────────────────────────────────────────────
    std::vector<std::uint8_t> os2;
    put16(os2, 4);             // version
    puti16(os2, ADVANCE / 2);  // xAvgCharWidth
    put16(os2, 400);           // usWeightClass
    put16(os2, 5);             // usWidthClass
    put16(os2, 0);             // fsType
    for (int i = 0; i < 10; ++i) puti16(os2, 0);   // subscript/superscript/strikeout/family class
    for (int i = 0; i < 10; ++i) os2.push_back(0); // panose
    put32(os2, 0); put32(os2, 0); put32(os2, 0); put32(os2, 0);   // unicode ranges
    put32(os2, 0x6D617961);    // achVendID 'maya'
    put16(os2, 0x0040);        // fsSelection = regular
    put16(os2, static_cast<std::uint16_t>(first));   // usFirstCharIndex
    put16(os2, static_cast<std::uint16_t>(last));    // usLastCharIndex
    puti16(os2, ASCENT); puti16(os2, -DESCENT); puti16(os2, 90);  // typo asc/desc/gap
    put16(os2, ASCENT); put16(os2, DESCENT);         // win ascent/descent
    put32(os2, 0); put32(os2, 0);                    // code page ranges
    puti16(os2, 500); puti16(os2, 0);                // xHeight / capHeight
    put16(os2, 0); put16(os2, 0); put16(os2, num_glyphs);   // default/break/maxContext

    return assemble({
        {ot::tags::head, std::move(head)},
        {ot::tags::maxp, std::move(maxp)},
        {ot::tags::hhea, std::move(hhea)},
        {ot::tag("hmtx"), std::move(hmtx)},
        {ot::tags::cmap, std::move(cmap)},
        {ot::tags::loca, std::move(loca_bytes)},
        {ot::tags::glyf, std::move(glyf)},
        {ot::tag("name"), std::move(name_table)},
        {ot::tag("post"), std::move(post)},
        {ot::tag("OS/2"), std::move(os2)},
    });
}

}  // namespace detail

/// The synthesized bytes, built once. Handy for tests and for shipping as a
/// resource if a caller wants to.
[[nodiscard]] inline const std::vector<std::uint8_t>& font_bytes() {
    static const std::vector<std::uint8_t> bytes = detail::build_font_bytes();
    return bytes;
}

/// A ready-to-use last-resort Face. Never null on a working allocator, and the
/// only guarantee that `default_stack()` is non-empty on a font-less machine.
[[nodiscard]] inline std::shared_ptr<Face> face() {
    return Face::from_memory(font_bytes(), 0, nullptr);
}

}  // namespace mayag::typo::lastresort
