// tests/test_font.cpp — the font engine
//
// Tested against the REAL fonts installed on the machine, not fixtures. A
// font parser that only works on the files its author tried is worthless;
// these tests sweep every face the system has and assert invariants that must
// hold for all of them.
//
// Layers, in order of what breaks worst when wrong:
//   1. binary parsing   — bounds safety on hostile input
//   2. cmap             — the right glyph for the right codepoint
//   3. outlines         — glyf, composites, CFF charstrings
//   4. rasterisation    — coverage correctness and antialiasing
//   5. SDF              — scale independence
//   6. atlas            — packing and eviction
//   7. shaping          — kerning, clusters, fallback
//   8. integration      — text actually reaches the framebuffer

#include <mayag/mayag.hpp>
#include <mayag/widget/text_field.hpp>

#include "font_fingerprint.hpp"

#include <fstream>

#include <cstdio>
#include <filesystem>
#include <random>
#include <set>
#include <string>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int failures = 0;
int checks   = 0;

void check(bool cond, const std::string& what) {
    ++checks;
    if (!cond) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void near(float got, float want, float eps, const std::string& what) {
    ++checks;
    if (num::abs(got - want) > eps) {
        ++failures;
        std::printf("  FAIL  %s: got %.4f want %.4f\n", what.c_str(), static_cast<double>(got), static_cast<double>(want));
    }
}
void section(const char* n) { std::printf("\n%s\n", n); }

/// Values measured from CoreText or from specific font files on the Mac the
/// tests were written on only hold for those very files: another macOS
/// release ships other builds at the same paths (CJK Symbols Fallback's Bold
/// moves, PingFang's paths change, the fallback cascade differs).
/// tests/data/fonts.txt pins them (tools/font_fingerprints.py).
[[nodiscard]] bool captured(const std::string& path) {
    static const mayag::test::FontManifest manifest{std::string{MAYAG_SOURCE_DIR} + "/tests/data/fonts.txt"};
    if (manifest.matches(path)) return true;
    std::printf("  note: %s is not the build the expectations were measured on; skipped\n", path.c_str());
    return false;
}

// ── helpers ─────────────────────────────────────────────────────────────

[[nodiscard]] bool ot_parse(const std::vector<std::uint8_t>& data) {
    return typo::ot::FontFile::parse(data, 0, nullptr).has_value();
}

[[nodiscard]] std::string first_face_path() {
    static const char* candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    for (const char* p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    for (const auto& e : typo::system::Database::instance().entries()) {
        if (e.has_latin() && !e.color) return e.path;
    }
    return {};
}

/// A face we can rely on being present, chosen per platform.
[[nodiscard]] std::shared_ptr<typo::Face> test_face() {
    static const char* candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    for (const char* p : candidates) {
        if (std::filesystem::exists(p)) {
            if (auto f = typo::Face::from_file(p)) return f;
        }
    }
    // Last resort: whatever the database found first with Latin coverage.
    for (const auto& e : typo::system::Database::instance().entries()) {
        if (e.has_latin() && !e.color) {
            if (auto f = typo::Face::from_file(e.path, e.face_index)) return f;
        }
    }
    return nullptr;
}

// ════════════════════════════════════════════════════════════════════════

/// A font file is untrusted input. Truncated, corrupted, and adversarial
/// files must produce an error, never a crash or an overread.
void test_robustness() {
    section("robustness");

    // Empty and tiny inputs.
    check(!ot_parse({}), "empty input is rejected");
    check(!ot_parse({0x00}), "1-byte input is rejected");
    check(!ot_parse(std::vector<std::uint8_t>(11, 0)), "11-byte input is rejected");

    // Valid signature, nothing else.
    std::vector<std::uint8_t> fake{0x00, 0x01, 0x00, 0x00, 0x00, 0x10,
                                   0x00, 0x80, 0x00, 0x03, 0x00, 0x00};
    check(!ot_parse(fake), "sfnt header with no tables is rejected");

    auto face = test_face();
    check(face != nullptr, "a system font is available for testing");
    if (!face) return;

    // Load the real file, then corrupt it in a thousand places. Every
    // mutation must either parse or fail cleanly — never crash, never hang.
    std::ifstream in(first_face_path(), std::ios::binary | std::ios::ate);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::uint8_t> original(size);
    in.read(reinterpret_cast<char*>(original.data()), static_cast<std::streamsize>(size));

    std::mt19937 rng{12345};
    std::uniform_int_distribution<std::size_t> pos{0, original.size() - 1};
    std::uniform_int_distribution<int> byte{0, 255};

    int survived = 0;
    for (int trial = 0; trial < 400; ++trial) {
        auto mutated = original;
        // Corrupt a handful of bytes, biased toward the header and table
        // directory where the structural fields live.
        for (int k = 0; k < 8; ++k) {
            const std::size_t p = (trial % 3 == 0)
                ? pos(rng) % num::min<std::size_t>(4096, mutated.size())
                : pos(rng);
            mutated[p] = static_cast<std::uint8_t>(byte(rng));
        }
        if (auto f = typo::ot::FontFile::parse(mutated, 0, nullptr)) {
            // If it parsed, exercise it: every accessor must stay in bounds.
            typo::GlyphSource src{*f};
            for (std::uint32_t cp = 0x20; cp < 0x80; ++cp) {
                const auto gid = f->glyph_for(cp);
                (void)f->advance(gid);
                (void)f->lsb(gid);
                const auto o = src.load(gid);
                (void)o.control_bounds();
            }
            ++survived;
        }
    }
    check(true, "400 corrupted fonts parsed or rejected without crashing (" +
                std::to_string(survived) + " parsed)");

    // Truncation at every scale.
    for (std::size_t frac : {2u, 4u, 8u, 16u, 64u, 256u}) {
        std::vector<std::uint8_t> truncated(original.begin(),
                                            original.begin() + static_cast<std::ptrdiff_t>(original.size() / frac));
        if (auto f = typo::ot::FontFile::parse(truncated, 0, nullptr)) {
            typo::GlyphSource src{*f};
            for (std::uint16_t g = 0; g < num::min<std::uint16_t>(f->num_glyphs(), 200); ++g) {
                (void)src.load(g);
            }
        }
    }
    check(true, "truncated fonts do not crash the outline loader");

    // Every glyph id in range, plus out-of-range ids.
    typo::GlyphSource src{face->file()};
    for (std::uint32_t g = 0; g < 70000; g += 97) {
        (void)src.load(static_cast<std::uint16_t>(g));
    }
    check(true, "out-of-range glyph ids return empty outlines");
}

/// Sweep every font on the machine. The bar: parse without crashing, and if
/// it parses, produce sane metrics.
void test_all_system_fonts() {
    section("system font sweep");

    const auto& db = typo::system::Database::instance();
    check(db.size() > 0, "the font database found faces (" + std::to_string(db.size()) + ")");

    int loaded = 0, with_outlines = 0, probed = 0, cff = 0, truetype = 0, kerned = 0;

    for (const auto& e : db.entries()) {
        auto face = typo::Face::from_file(e.path, e.face_index);
        if (!face) continue;
        ++loaded;

        // Invariants that must hold for ANY valid face.
        check(face->units_per_em() > 0, "upem is nonzero: " + e.family);
        check(face->num_glyphs() > 0,   "has glyphs: " + e.family);

        const auto m = face->metrics(16.0f);
        if (m.ascent + m.descent <= 0.0f) {
            std::printf("  note: %s has degenerate metrics\n", e.family.c_str());
        }

        face->file().is_cff() ? ++cff : ++truetype;
        if (face->has_kerning()) ++kerned;

        // Render a representative glyph from whichever script it covers.
        const std::uint32_t probe = e.has_latin() ? 'B' : e.has_cjk() ? 0x4E2D : 0;
        if (probe != 0) {
            const auto gid = face->glyph_for(probe);
            if (gid != 0) {
                ++probed;
                if (!face->outline(gid).empty()) ++with_outlines;
            }
        }
    }

    std::printf("  loaded %d faces: %d TrueType, %d CFF, %d with kerning, %d rendered\n",
                loaded, truetype, cff, kerned, with_outlines);
    check(loaded > 10, "loaded a meaningful number of system faces");
    // Faces covering neither Latin nor Han (Noto Sans Tamil, ...) have no
    // probe glyph, and colour/bitmap faces have none to outline: "most" is
    // of the faces that were probed.
    check(with_outlines > probed / 2, "most faces produced real outlines (" + std::to_string(with_outlines) +
                                          " of " + std::to_string(probed) + " probed)");
    if (cff > 0)
        check(true, "at least one CFF/OpenType face was exercised");
    else
        std::printf("  note: no CFF/OpenType faces installed, CFF parsing not exercised here\n");
}

void test_cmap() {
    section("cmap");

    auto face = test_face();
    if (!face) return;

    check(face->glyph_for('A') != 0, "ASCII maps");
    check(face->glyph_for(' ') != 0, "space maps");
    check(face->glyph_for('A') != face->glyph_for('B'), "distinct chars, distinct glyphs");

    // Latin-1 and Latin Extended, which format 4 must handle via segments.
    check(face->glyph_for(0x00E9) != 0, "e-acute maps (Latin-1)");
    check(face->glyph_for(0x00FC) != 0, "u-diaeresis maps");

    // Unassigned planes must return .notdef rather than garbage.
    check(face->glyph_for(0x10FFFF) == 0, "unassigned codepoint returns notdef");
    check(face->glyph_for(0x0FFFFF) == 0, "unassigned plane returns notdef");

    // Every mapped glyph id must be within the font's glyph count. A cmap
    // that points past `numGlyphs` is the classic route to an OOB read.
    int mapped = 0;
    const int failures_before = failures;
    for (std::uint32_t cp = 0; cp < 0x2000; ++cp) {
        const auto g = face->glyph_for(cp);
        if (g != 0) {
            ++mapped;
            check(g < face->num_glyphs(), "glyph id in range for U+" + std::to_string(cp));
            if (failures > failures_before) break;   // do not spam
        }
    }
    check(mapped > 100, "a useful number of codepoints map (" + std::to_string(mapped) + ")");
}

void test_outlines() {
    section("outlines");

    auto face = test_face();
    if (!face) return;

    // 'A' has an outer contour and one counter.
    const auto a = face->outline(face->glyph_for('A'));
    check(a.contours.size() >= 2, "'A' has an outer contour plus a counter");
    check(!a.control_bounds().empty(), "'A' has real bounds");

    // 'o' is two nested rings.
    const auto o = face->outline(face->glyph_for('o'));
    check(o.contours.size() == 2, "'o' has exactly two contours");

    // Space is legitimately empty — not an error.
    check(face->outline(face->glyph_for(' ')).empty(), "space has no contours");

    // A composite glyph: e-acute is 'e' plus the accent, so it has strictly
    // more contours than 'e' alone. This is the composite decoder working.
    const auto e     = face->outline(face->glyph_for('e'));
    const auto eacute = face->outline(face->glyph_for(0x00E9));
    if (!eacute.empty() && !e.empty()) {
        check(eacute.contours.size() > e.contours.size(),
              "composite e-acute has more contours than plain e");
        check(eacute.control_bounds().height() > e.control_bounds().height(),
              "and is taller, because the accent sits above");
    }

    // Contours must be closed: the last segment returns to the start.
    for (const auto& c : a.contours) {
        check(!c.segments.empty(), "contour has segments");
        if (!c.segments.empty()) {
            const Vec2 end = c.segments.back().to;
            near((end - c.start).length(), 0.0f, 1.0f, "contour is closed");
        }
    }

    // Descenders go below the baseline; ascenders above.
    check(face->outline(face->glyph_for('g')).control_bounds().top() < 0.0f ||
          face->outline(face->glyph_for('p')).control_bounds().top() < 0.0f,
          "descenders extend below the baseline");
}

void test_rasterization() {
    section("rasterization");

    auto face = test_face();
    if (!face) return;

    const float scale = face->scale_for(64.0f);
    const auto o = face->outline(face->glyph_for('H'));
    const auto r = typo::rasterize(o, scale, 1);

    check(!r.bitmap.empty(), "'H' rasterises to a non-empty bitmap");
    check(r.bitmap.width > 10 && r.bitmap.height > 10, "at a plausible size");

    // Ink exists.
    int lit = 0, opaque = 0, partial = 0;
    for (auto p : r.bitmap.pixels) {
        if (p > 0)   ++lit;
        if (p == 255) ++opaque;
        if (p > 10 && p < 245) ++partial;
    }
    check(lit > 50, "the glyph has ink");
    check(opaque > 20, "with fully covered interior pixels");
    // ANTIALIASING is the point of analytic coverage: a hard binary bitmap
    // would have zero partial pixels.
    check(partial > 10, "and antialiased edges (" + std::to_string(partial) + " partial px)");

    // 'H' is two stems and a bar: the middle row must be lit across, and a
    // row above the bar must have a gap in the centre.
    const int mid_y = r.bitmap.height / 2;
    check(r.bitmap.at(r.bitmap.width / 2, mid_y) > 100, "'H' crossbar is filled at mid-height");

    const int top_y = r.bitmap.height / 6;
    check(r.bitmap.at(r.bitmap.width / 2, top_y) < 100, "'H' has a gap above the crossbar");
    check(r.bitmap.at(1, top_y) > 100 || r.bitmap.at(2, top_y) > 100,
          "'H' left stem is present at the top");

    // Scaling up must produce proportionally more pixels.
    const auto small = typo::rasterize(o, face->scale_for(16.0f), 1);
    const auto large = typo::rasterize(o, face->scale_for(64.0f), 1);
    check(large.bitmap.width > small.bitmap.width * 2,
          "4x the size gives roughly 4x the width");

    // Degenerate inputs.
    check(typo::rasterize(o, 0.0f).bitmap.empty(), "zero scale yields nothing");
    check(typo::rasterize(typo::Outline{}, scale).bitmap.empty(), "empty outline yields nothing");
}

void test_sdf() {
    section("sdf");

    auto face = test_face();
    if (!face) return;

    const auto o = face->outline(face->glyph_for('o'));
    const float spread = 6.0f;
    const auto sdf = typo::rasterize_sdf(o, face->scale_for(32.0f), spread, 2);
    check(!sdf.bitmap.empty(), "SDF generated");

    // The field must actually SPAN a range — a flat image around 128 means
    // the distance transform never propagated. That was a real bug: seeding
    // both the inside and outside fields at every pixel left everything at
    // distance zero, and the result still faintly resembled the glyph.
    std::uint8_t lo = 255, hi = 0;
    for (auto p : sdf.bitmap.pixels) { lo = num::min(lo, p); hi = num::max(hi, p); }
    check(hi - lo > 100, "the field spans a wide range, i.e. distances propagated (" +
                         std::to_string(hi - lo) + ")");
    check(lo < 20, "reaching the clamped floor far outside the glyph");

    // How far INSIDE the field reaches is bounded by the glyph's own stem
    // width, not by the spread: an 'o' at 32 px has ~3 px stems, so the
    // deepest interior point is only ~1.5 px from an edge. Encoded against a
    // 6 px spread that is 128 + 1.5/6 * 127 ~ 160, and expecting 200+ would
    // be asking the glyph to be thicker than it is.
    check(hi > 128 + static_cast<int>(127.0f * 1.0f / spread),
          "and reaching at least 1 px inside the stems");

    // Thick shapes DO reach deep, which confirms the ceiling above is the
    // glyph's geometry rather than a clamp in the transform.
    const auto block = face->outline(face->glyph_for('M'));
    const auto block_sdf = typo::rasterize_sdf(block, face->scale_for(96.0f), spread, 2);
    std::uint8_t block_hi = 0;
    for (auto p : block_sdf.bitmap.pixels) block_hi = num::max(block_hi, p);
    check(block_hi > 230, "a heavy glyph at 96 px saturates the inside of the field");

    // Monotonicity: walking from outside the glyph toward its stem, the field
    // must increase. That property is what makes an SDF scale-independent.
    const int y = sdf.bitmap.height / 2;
    int increasing_runs = 0;
    for (int x = 1; x < sdf.bitmap.width / 3; ++x) {
        if (sdf.bitmap.at(x, y) >= sdf.bitmap.at(x - 1, y)) ++increasing_runs;
    }
    check(increasing_runs > sdf.bitmap.width / 6,
          "the field increases monotonically toward the glyph edge");

    // Reconstructing at 4x from a 32px field must still be a closed ring:
    // sample around the centre and confirm we cross the boundary exactly
    // twice on a horizontal line through the bowl.
    int crossings = 0;
    bool was_inside = false;
    for (int x = 0; x < sdf.bitmap.width * 4; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(sdf.bitmap.width * 4);
        const float v = 0.5f;
        const bool inside = sdf.bitmap.sample(u, v) > 0.5f;
        if (x > 0 && inside != was_inside) ++crossings;
        was_inside = inside;
    }
    check(crossings == 4, "upscaled 'o' crosses its outline 4 times (got " +
                          std::to_string(crossings) + ")");

    // The field is exact: thresholding it at 0.5 must put the glyph where
    // coverage puts it. The old EDT route (seeded from a supersampled
    // coverage bitmap) shifted 'H' ~0.85 raster px right, which magnified
    // SDF text then drew 3 px off at 200 px.
    for (const char32_t cp : {U'H', U'O', U'l'}) {
        const auto g = face->outline(face->glyph_for(static_cast<std::uint32_t>(cp)));
        const float sc = face->scale_for(48.0f);
        const auto cov = typo::rasterize(g, sc, 1);
        const auto f = typo::rasterize_sdf(g, sc, spread, 2);
        double s = 0, cx = 0, s2 = 0, cx2 = 0;
        for (int py = 0; py < cov.bitmap.height; ++py)
            for (int px = 0; px < cov.bitmap.width; ++px) {
                const double a = cov.bitmap.at(px, py) / 255.0;
                s += a; cx += a * (px + 0.5 + static_cast<double>(cov.offset.x));
            }
        constexpr int U = 8;
        for (int py = 0; py < f.bitmap.height * U; ++py)
            for (int px = 0; px < f.bitmap.width * U; ++px) {
                const float u = (static_cast<float>(px) + 0.5f) / static_cast<float>(f.bitmap.width * U);
                const float v = (static_cast<float>(py) + 0.5f) / static_cast<float>(f.bitmap.height * U);
                if (f.bitmap.sample(u, v) >= 0.5f) {
                    s2 += 1.0; cx2 += (px + 0.5) / U + static_cast<double>(f.offset.x);
                }
            }
        const double bias = cx2 / s2 - cx / s;
        check(std::abs(bias) < 0.15, std::string("exact SDF centroid matches coverage for '") + char(cp) +
                                         "' (bias " + std::to_string(bias) + " px)");
        check(std::abs(s2 / (U * U) - s) / s < 0.03, std::string("exact SDF area matches coverage for '") + char(cp) + "'");
    }
}

void test_atlas() {
    section("atlas");

    typo::SkylinePacker packer{256, 256};
    int x = 0, y = 0;

    check(packer.pack(100, 50, x, y), "first rect packs");
    check(x == 0 && y == 0, "at the origin");

    check(packer.pack(100, 50, x, y), "second rect packs");
    check(x == 100 && y == 0, "beside the first");

    check(!packer.pack(300, 10, x, y), "an over-wide rect is rejected");
    check(!packer.pack(10, 300, x, y), "an over-tall rect is rejected");
    check(!packer.pack(0, 10, x, y), "a zero-width rect is rejected");

    // Fill it up and confirm it reports full rather than overlapping.
    typo::SkylinePacker small{64, 64};
    int packed = 0;
    while (small.pack(16, 16, x, y)) ++packed;
    check(packed == 16, "a 64x64 atlas holds exactly 16 16x16 rects (got " +
                        std::to_string(packed) + ")");

    // Placements must not overlap. Pack random sizes and verify pairwise.
    typo::SkylinePacker rnd{512, 512};
    std::vector<Rect> placed;
    std::mt19937 rng{7};
    std::uniform_int_distribution<int> dim{4, 40};
    for (int i = 0; i < 200; ++i) {
        const int w = dim(rng), h = dim(rng);
        if (!rnd.pack(w, h, x, y)) break;
        placed.push_back(Rect{static_cast<float>(x), static_cast<float>(y),
                              static_cast<float>(w), static_cast<float>(h)});
    }
    bool overlap = false;
    for (std::size_t i = 0; i < placed.size() && !overlap; ++i) {
        for (std::size_t k = i + 1; k < placed.size(); ++k) {
            if (placed[i].intersects(placed[k])) { overlap = true; break; }
        }
    }
    check(!overlap, "no two packed rects overlap (" + std::to_string(placed.size()) + " rects)");

    // The full cache path.
    auto face = test_face();
    if (!face) return;

    typo::FontStack stack{typo::FontConfig{.mode = typo::RenderMode::bitmap}};
    stack.add(face);

    const auto* g1 = stack.glyph(0, face->glyph_for('A'), 24.0f);
    check(g1 != nullptr && g1->size.x > 0, "glyph rasterises into the atlas");

    const auto count_after_first = stack.atlas().glyph_count();
    const auto* g2 = stack.glyph(0, face->glyph_for('A'), 24.0f);
    check(stack.atlas().glyph_count() == count_after_first, "a repeat lookup is cached");
    check(g1->atlas_rect == g2->atlas_rect, "and returns the same slot");

    // Different size, bitmap mode: a distinct entry.
    (void)stack.glyph(0, face->glyph_for('A'), 48.0f);
    check(stack.atlas().glyph_count() > count_after_first,
          "a different size is a different bitmap entry");

    // SDF mode: sizes up to the base raster share ONE entry (this is why a
    // page of type sizes renders in a single draw call); larger sizes use
    // sqrt(2)-spaced tiers so a field is minified rather than magnified, so
    // six sizes from 8 to 96 px make three entries (48, 68, 96 px fields).
    typo::FontStack sdf_stack{typo::FontConfig{.mode = typo::RenderMode::sdf}};
    sdf_stack.add(face);
    for (float size : {8.0f, 12.0f, 16.0f, 24.0f, 48.0f}) {
        (void)sdf_stack.glyph(0, face->glyph_for('A'), size);
    }
    check(sdf_stack.atlas().glyph_count() == 1,
          "SDF mode shares one entry across sizes up to the base raster (got " +
          std::to_string(sdf_stack.atlas().glyph_count()) + ")");
    for (float size : {60.0f, 64.0f, 96.0f}) (void)sdf_stack.glyph(0, face->glyph_for('A'), size);
    check(sdf_stack.atlas().glyph_count() == 3,
          "larger sizes add one entry per sqrt(2) tier (got " +
          std::to_string(sdf_stack.atlas().glyph_count()) + ")");
    near(sdf_stack.sdf_raster_px(200.0f), 192.0f, 0.01f, "tiers stop at 4x the base raster");
    near(sdf_stack.sdf_raster_px(60.0f), 68.0f, 0.01f, "60 px text uses the 68 px tier (minified)");
}

void test_shaping() {
    section("shaping");

    auto face = test_face();
    if (!face) return;

    typo::FontStack stack{};
    stack.add(face);

    const typo::ShapeParams p{32.0f, 0.0f, true, true};

    // One glyph per ASCII character.
    const auto hello = stack.shape("Hello", p);
    check(hello.glyphs.size() == 5, "5 chars produce 5 glyphs");
    check(hello.width > 0.0f, "with positive width");

    // Clusters are BYTE offsets into the source, which is what cursor
    // movement and selection need.
    check(hello.glyphs[0].cluster == 0, "first cluster at byte 0");
    check(hello.glyphs[4].cluster == 4, "fifth cluster at byte 4");

    // Multi-byte UTF-8: one glyph, cluster spans the encoded length.
    const auto accented = stack.shape("café", p);
    check(accented.glyphs.size() == 4, "4 characters despite 5 bytes");
    check(accented.glyphs[3].cluster == 3, "the accented char starts at byte 3");

    // Malformed UTF-8 must not hang or crash.
    const auto bad = stack.shape("\xFF\xFE\x80 ok", p);
    check(!bad.glyphs.empty(), "invalid UTF-8 shapes to replacement glyphs");

    // Kerning. If the face has a kern/GPOS table, 'AV' must be tighter than
    // the sum of its advances.
    if (face->has_kerning()) {
        const typo::ShapeParams unkerned{32.0f, 0.0f, false, true};
        const float with    = stack.shape("AV", p).width;
        const float without = stack.shape("AV", unkerned).width;
        check(with < without, "kerning tightens 'AV' (" +
                              std::to_string(with) + " vs " + std::to_string(without) + ")");

        // And a pair with no kern pair must be unchanged.
        near(stack.shape("ll", p).width, stack.shape("ll", unkerned).width, 0.01f,
             "'ll' is unaffected by kerning");
    }

    // Letter spacing adds exactly n * spacing.
    const typo::ShapeParams spaced{32.0f, 5.0f, true, true};
    near(stack.shape("abc", spaced).width - stack.shape("abc", p).width, 15.0f, 0.01f,
         "letter spacing adds spacing per glyph");

    // Hit testing round-trips.
    const auto word = stack.shape("Hamburger", p);
    const int failures_before_hit = failures;
    for (std::uint32_t c = 0; c < 9; ++c) {
        const float x = word.x_of_cluster(c);
        check(word.cluster_at(x + 1.0f) == c,
              "cluster " + std::to_string(c) + " round-trips through hit testing");
        if (failures > failures_before_hit) break;
    }

    // Scripts requiring complex shaping are DETECTED, so a caller can route
    // them to HarfBuzz instead of rendering them wrongly and silently.
    check(stack.shape("مرحبا", p).has_rtl, "Arabic is flagged as RTL");
    check(stack.shape("مرحبا", p).needs_complex, "Arabic is flagged as complex");
    check(!stack.shape("Hello", p).has_rtl, "Latin is not flagged");
}

void test_fallback() {
    section("fallback");

    auto stack = typo::system::default_stack();
    check(stack->face_count() >= 1, "the default stack has at least one face");

    if (stack->face_count() < 2) {
        std::printf("  note: only one face available, skipping fallback checks\n");
        return;
    }

    const typo::ShapeParams p{16.0f};

    // Latin resolves to the primary face.
    const auto latin = stack->shape("Hello", p);
    const int failures_before_latin = failures;
    for (const auto& g : latin.glyphs) {
        check(g.face_index == 0, "Latin comes from the primary face");
        if (failures > failures_before_latin) break;
    }

    // A mixed string must draw from MORE than one face. This is the property
    // that turns tofu boxes into readable text. Only meaningful when some
    // installed face has Han (CI runners ship none).
    bool has_han = false;
    for (std::size_t i = 0; i < stack->face_count() && !has_han; ++i) has_han = stack->face_at(i)->glyph_for(0x65E5) != 0;
    if (!has_han) {
        std::printf("  note: no installed face covers Han, skipping CJK fallback checks\n");
        return;
    }
    const auto mixed = stack->shape("Hi 日本", p);
    bool saw_fallback = false;
    for (const auto& g : mixed.glyphs) {
        if (g.face_index > 0) saw_fallback = true;
    }
    check(saw_fallback, "CJK falls back to a different face");

    // And nothing should be missing.
    int missing = 0;
    for (const auto& g : mixed.glyphs) if (g.missing) ++missing;
    check(missing == 0, "every character in the mixed string resolved");
}

void test_line_breaking() {
    section("line breaking");

    auto face = test_face();
    if (!face) return;

    typo::FontStack stack{};
    stack.add(face);
    typo::StackMeasurer measurer{stack};

    TextStyle st;
    st.size = 16.0f;
    st.overflow = TextOverflow::wrap;

    // An unwrapped single line.
    const Vec2 one = measurer.measure("Hello world", st, num::inf);
    check(one.x > 0.0f, "single line has width");

    // Narrow the box: it must wrap and get taller.
    const Vec2 wrapped = measurer.measure("Hello world foo bar baz qux", st, 60.0f);
    check(wrapped.y > one.y, "wrapping increases the height");
    check(wrapped.x <= 62.0f, "and respects the max width");

    // Explicit newlines are honoured even without wrapping.
    const Vec2 explicit_break = measurer.measure("a\nb\nc", st, num::inf);
    check(explicit_break.y > one.y * 2.0f, "newlines create lines");

    // A single word longer than the line must break rather than overflow.
    const Vec2 longword = measurer.measure("supercalifragilistic", st, 40.0f);
    check(longword.x <= 44.0f, "an over-long word breaks mid-word");

    // Empty string still occupies one line's height.
    const Vec2 empty = measurer.measure("", st, 100.0f);
    check(empty.x == 0.0f && empty.y > 0.0f, "empty text has height but no width");
}

void test_metrics() {
    section("metrics");

    auto face = test_face();
    if (!face) return;

    const auto m = face->metrics(32.0f);
    check(m.ascent  > 0.0f, "ascent is positive");
    check(m.descent > 0.0f, "descent is positive (stored as magnitude)");
    check(m.line_height >= m.ascent + m.descent, "line height covers ascent + descent");
    check(m.x_height > 0.0f && m.x_height < m.ascent, "x-height is between baseline and ascent");
    check(m.cap_height > m.x_height, "cap height exceeds x-height");

    // Metrics scale linearly with size — a 2x size is a 2x ascent.
    const auto m2 = face->metrics(64.0f);
    near(m2.ascent, m.ascent * 2.0f, 0.1f, "metrics scale linearly");

    // Advances are positive for visible glyphs and match hmtx.
    check(face->advance(face->glyph_for('M'), 32.0f) > 0.0f, "'M' has advance");
    check(face->advance(face->glyph_for(' '), 32.0f) > 0.0f, "space has advance");
    check(face->advance(face->glyph_for('M'), 32.0f) >
          face->advance(face->glyph_for('i'), 32.0f),
          "'M' is wider than 'i' in a proportional face");
}

/// The end-to-end claim: real text reaches real pixels.
void test_rendering() {
    section("rendering");

    auto stack = typo::system::default_stack(
        typo::FontConfig{.mode = typo::RenderMode::bitmap});
    if (stack->empty()) return;

    // Text renders ink.
    {
        auto ui = v(text<"Hello"> | font(32) | fg(colors::white)) | pad(10);
        RenderOptions o;
        o.background = colors::black;
        o.fonts = stack;

        const auto px = render_to_pixels(ui, {200, 60}, o);
        int lit = 0;
        for (std::size_t i = 0; i < px.size(); i += 4) if (px[i] > 128) ++lit;
        check(lit > 100, "text puts ink on the canvas (" + std::to_string(lit) + " px)");
    }

    // Bigger text makes more ink.
    {
        const auto measure_ink = [&](float size) {
            auto ui = v(text<"W"> | font(size) | fg(colors::white)) | pad(4);
            RenderOptions o; o.background = colors::black; o.fonts = stack;
            const auto px = render_to_pixels(ui, {200, 120}, o);
            int lit = 0;
            for (std::size_t i = 0; i < px.size(); i += 4) if (px[i] > 128) ++lit;
            return lit;
        };
        check(measure_ink(48.0f) > measure_ink(16.0f) * 3,
              "3x the point size gives much more ink");
    }

    // Colour is honoured.
    {
        auto ui = v(text<"XXXX"> | font(40) | fg(colors::red)) | pad(6);
        RenderOptions o; o.background = colors::black; o.fonts = stack;
        const auto px = render_to_pixels(ui, {200, 70}, o);

        int reddish = 0;
        for (std::size_t i = 0; i < px.size(); i += 4) {
            if (px[i] > 100 && px[i] > px[i + 1] + 40 && px[i] > px[i + 2] + 40) ++reddish;
        }
        check(reddish > 50, "red text renders red (" + std::to_string(reddish) + " px)");
    }

    // REGRESSION: glyphs must stay ANTIALIASED in every render mode.
    //
    // The sampler used to decide "is this a distance field?" from the
    // stack's mode, but hybrid mode mixes bitmap and SDF entries within one
    // frame. So small bitmap text was run through the SDF threshold, which
    // snapped every partial pixel to 0 or 1 and destroyed the antialiasing —
    // text looked eroded and ragged. The flag now travels per instance.
    //
    // Separately, the threshold band was NARROWER than one atlas texel
    // (0.04 against a measured ~0.082 per-texel slope), so SDF edges snapped
    // too. Both are covered here.
    {
        for (auto mode : {typo::RenderMode::bitmap,
                          typo::RenderMode::hybrid,
                          typo::RenderMode::sdf}) {
            auto s = typo::system::default_stack(typo::FontConfig{.mode = mode});
            if (s->empty()) continue;

            const char* label = mode == typo::RenderMode::bitmap ? "bitmap"
                              : mode == typo::RenderMode::hybrid ? "hybrid" : "sdf";

            // A UI-sized run, the case that looked worst.
            auto ui = v(text<"Recent deploys"> | font(13) | fg(colors::white)) | pad(4);
            RenderOptions o;
            o.background = colors::black;
            o.fonts = s;

            const auto px = render_to_pixels(ui, {160, 26}, o);
            int lit = 0, partial = 0;
            for (std::size_t i = 0; i < px.size(); i += 4) {
                const float l = luminance(rgb8(px[i], px[i + 1], px[i + 2]));
                if (l > 0.12f) { ++lit; if (l < 0.85f) ++partial; }
            }

            check(lit > 50, std::string{"text renders in "} + label + " mode");
            // Hard-edged text sits near 0% partial; real antialiasing at this
            // size is well over half.
            check(lit > 0 && (100 * partial / lit) > 40,
                  std::string{"glyphs are antialiased in "} + label + " mode (" +
                  std::to_string(lit ? 100 * partial / lit : 0) + "% partial)");
        }
    }

    // And the SDF edge itself must be a RAMP, not a cliff.
    {
        auto s = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::sdf});
        if (!s->empty()) {
            auto ui = v(text<"H"> | font(40) | fg(colors::white)) | pad(6);
            RenderOptions o;
            o.background = colors::black;
            o.fonts = s;

            const int W = 50, H = 60;
            const auto px = render_to_pixels(ui, {static_cast<float>(W), static_cast<float>(H)}, o);

            int intermediate = 0;
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    const int v8 = px[(static_cast<std::size_t>(y) * W + x) * 4];
                    if (v8 > 25 && v8 < 230) ++intermediate;
                }
            }
            check(intermediate > 40,
                  "SDF glyph edges are a gradient, not a cliff (" +
                  std::to_string(intermediate) + " intermediate px)");
        }
    }

    // The batching claim.
    {
        constexpr Theme t = themes::midnight;
        auto ui = v(text<"Title"> | font(28) | bold | fg(t.text_primary),
                    box() | size(100, 4) | bg(t.accent) | radius(2),
                    text<"Body text here"> | font(14) | fg(t.text_secondary),
                    h(box() | size(40, 20) | bg(t.surface) | radius(4),
                      text<"Label"> | font(12) | fg(t.text_primary)) | gap(6))
                | gap(8) | pad(12) | bg(t.background);

        Node root = ui.build();
        typo::StackMeasurer measurer{*stack};
        typo::StackGlyphRenderer glyphs{*stack};
        layout::layout_tree(root, {300, 160}, measurer);

        DrawList dl;
        render::PaintOptions po;
        po.measurer = &measurer;
        po.glyphs   = &glyphs;
        render::paint(root, dl, po);

        check(dl.size() > 20, "the scene produced instances (" + std::to_string(dl.size()) + ")");
        // Text and shapes INTERLEAVE here, so this only passes because the
        // glyph atlas is a permanently-bound slot rather than a batch key.
        check(dl.batches().size() == 1,
              "interleaved text and shapes batch into ONE draw call (got " +
              std::to_string(dl.batches().size()) + ")");
    }
}

}  // namespace

/// Measure-accurate caret placement + hit-testing for text fields. The bar:
/// caret_from_x is the exact inverse of the caret's x for every boundary
/// (round-trips), so a click lands where the caret would be drawn — pixel
/// correct under a proportional font, not a monospace guess.
void test_caret() {
    section("caret hit-testing");

    auto face = test_face();
    if (!face) { check(true, "no font — skipping"); return; }
    typo::FontStack stack;
    stack.add(face);
    typo::StackMeasurer meas{stack};
    TextStyle st; st.size = 20.0f;
    const auto measure = [&](std::string_view s) { return meas.advance(s, st); };

    // A string mixing wide and narrow glyphs so a monospace model would fail.
    const std::string text = "Wave illiW";
    const auto x_at = [&](std::size_t b) {
        return b == 0 ? 0.0f : measure(std::string_view{text.data(), b});
    };

    // Round-trip: every caret boundary -> its x -> back to the same caret.
    int exact = 0, total = 0;
    for (std::size_t c = 0; c <= text.size(); ++c) {
        ++total;
        if (dsl::caret_from_x(text, x_at(c), measure) == c) ++exact;
    }
    check(exact == total, "caret_from_x round-trips every boundary exactly");

    // Proportional truth: 'W' is wider than 'i', so the caret after "W" must
    // sit further right than a fixed-width model would place it.
    check(x_at(1) > 0.0f, "caret advances past the first glyph");
    check(measure("W") > measure("i"), "measurer reflects proportional widths");

    // A click past the end clamps to the last boundary; before the start to 0.
    check(dsl::caret_from_x(text, 1e6f, measure) == text.size(), "click past end -> end caret");
    check(dsl::caret_from_x(text, -5.0f, measure) == 0, "click before start -> caret 0");

    // Without a measurer the helper still works (monospace fallback), never
    // returning an out-of-range or mid-codepoint index.
    const std::size_t fb = dsl::caret_from_x(text, 3.0f, {});
    check(fb <= text.size(), "fallback hit-test stays in range");
}

/// Italic on a variable font drives its own slant: `st.italic` moves the slnt
/// axis (degrees) or ital axis (0..1) so the glyph is a real oblique from the
/// font's masters, not a synthetic shear. Skips unless a slnt/ital variable
/// font is present (only checked opportunistically at /tmp).
void test_italic() {
    section("italic (variable slnt/ital)");

    std::shared_ptr<typo::Face> vf;
    for (const char* p : {"/tmp/slnttest.ttf"}) {
        if (std::filesystem::exists(p)) { vf = typo::Face::from_file(p); if (vf) break; }
    }
    if (!vf || !vf->is_variable()) {
        // Look for any installed variable font with a slnt or ital axis.
        for (const auto& e : typo::system::Database::instance().entries()) {
            auto f = typo::Face::from_file(e.path, e.face_index);
            if (!f || !f->is_variable()) continue;
            bool has = false;
            for (const auto& a : f->axes())
                if (a.tag == typo::var::tag_slnt || a.tag == typo::var::tag_ital) has = true;
            if (has) { vf = f; break; }
        }
    }
    if (!vf) { check(true, "no slnt/ital variable font — skipping (not a failure)"); return; }

    typo::FontStack stack;
    stack.add(vf);

    const std::uint16_t g = vf->glyph_for('H');
    // Upright.
    { typo::ShapeParams p; p.size_px = 100.0f; (void)stack.shape("H", p); }
    const Rect up = vf->outline(g).control_bounds();
    // Italic.
    { typo::ShapeParams p; p.size_px = 100.0f; p.italic = true; (void)stack.shape("H", p); }
    const Rect it = vf->outline(g).control_bounds();

    check(up.size.x > 0.0f && it.size.x > 0.0f, "glyph has outline in both states");
    // A slant leaves height alone but shifts/widens the control box.
    check(num::abs(it.size.y - up.size.y) < up.size.y * 0.05f, "italic keeps the cap height");
    check(num::abs(it.origin.x - up.origin.x) > 1.0f || num::abs(it.size.x - up.size.x) > 1.0f,
          "italic slants the glyph geometry");
}

/// Synthetic italic: a STATIC font (no slnt/ital axis) still slants via a
/// raster-time shear, so `italic` works everywhere. The sheared glyph is wider
/// but the same height, and caches in its own atlas bucket.
void test_synthetic_italic() {
    section("synthetic italic");

    // Find a static (non-variable) face — the common desktop case.
    std::shared_ptr<typo::Face> sf;
    for (const auto& e : typo::system::Database::instance().entries()) {
        auto f = typo::Face::from_file(e.path, e.face_index);
        if (f && !f->is_variable() && !f->is_italic() && f->has_glyph(U'H') && f->has_outlines()) { sf = f; break; }
    }
    if (!sf) { check(true, "no static outline font — skipping"); return; }
    check(!sf->has_slant_axis(), "static face has no slant axis (needs synthesis)");

    typo::FontStack stack;
    stack.add(sf);
    const std::uint16_t g = sf->glyph_for('H');
    const typo::CachedGlyph* up = stack.glyph(0, g, 64.0f, 400.0f, {}, /*italic*/false);
    if (up == nullptr) { check(false, "upright glyph rasterises"); return; }
    const Vec2 up_size = up->size;   // copy before the next insert may realloc
    const bool up_valid = up->valid;
    const typo::CachedGlyph* it = stack.glyph(0, g, 64.0f, 400.0f, {}, /*italic*/true);
    if (it == nullptr) { check(false, "italic glyph rasterises"); return; }

    check(it->size.x > up_size.x, "synthetic italic widens the raster (shear)");
    check(num::abs(it->size.y - up_size.y) <= 1.5f, "synthetic italic keeps the height");
    check(up_valid && it->valid, "both upright and italic cache independently");
}

/// Synthetic bold: a font that can't reach the requested weight (a static
/// face, or a variable font past its wght-axis max) is emboldened by dilating
/// the coverage, so `bold` works everywhere. The bold glyph is thicker and
/// caches in its own bucket.
void test_synthetic_bold() {
    section("synthetic bold");

    std::shared_ptr<typo::Face> sf;
    for (const auto& e : typo::system::Database::instance().entries()) {
        // A REGULAR static face: a real Bold already reaches 700 and is
        // (rightly) not emboldened again.
        if (e.weight > 500) continue;
        auto f = typo::Face::from_file(e.path, e.face_index);
        if (f && !f->is_variable() && f->has_glyph(U'o') && f->has_outlines()) { sf = f; break; }
    }
    if (!sf) { check(true, "no regular static outline font — skipping"); return; }
    check(!sf->can_reach_weight(700.0f), "static face can't reach bold on its own");

    // Force the coverage path (SDF faux-bold is a separate, deferred concern).
    typo::FontConfig cfg; cfg.mode = typo::RenderMode::bitmap;
    typo::FontStack stack{cfg};
    stack.add(sf);
    const std::uint16_t g = sf->glyph_for('o');
    const typo::CachedGlyph* reg = stack.glyph(0, g, 32.0f, 400.0f, {}, false);
    if (reg == nullptr) { check(false, "regular glyph rasterises"); return; }
    const Vec2 reg_size = reg->size;
    const typo::CachedGlyph* heavy = stack.glyph(0, g, 32.0f, 700.0f, {}, false);
    if (heavy == nullptr) { check(false, "heavy glyph rasterises"); return; }

    check(heavy->size.x > reg_size.x, "synthetic heavy thickens the glyph (wider)");
    check(heavy->size.y >= reg_size.y, "synthetic heavy does not shrink the glyph");

    // A weight the face CAN'T reach embolds; a normal weight does not.
    const typo::CachedGlyph* reg2 = stack.glyph(0, g, 32.0f, 400.0f, {}, false);
    check(reg2 != nullptr, "regular weight still caches its own slim glyph");

    // SDF path (large text) must embolden too, so heavy looks the same on both
    // sides of the SDF threshold.
    typo::FontConfig sdf_cfg; sdf_cfg.mode = typo::RenderMode::sdf;
    typo::FontStack sdf{sdf_cfg};
    sdf.add(sf);
    const typo::CachedGlyph* sreg = sdf.glyph(0, g, 64.0f, 400.0f, {}, false);
    if (sreg == nullptr) { check(false, "sdf regular rasterises"); return; }
    const Vec2 sreg_size = sreg->size;
    const bool sreg_is_sdf = sreg->is_sdf;
    const typo::CachedGlyph* sbold = sdf.glyph(0, g, 64.0f, 700.0f, {}, false);
    if (sbold == nullptr) { check(false, "sdf heavy rasterises"); return; }
    check(sreg_is_sdf && sbold->is_sdf, "both are distance-field glyphs");
    check(sbold->size.x > sreg_size.x, "synthetic heavy thickens the SDF field too");
}

/// Ellipsis truncation — the DEFAULT overflow. A label too long for its box is
/// cut to the glyphs that fit plus “…”, reaching the box edge but not past it,
/// instead of clipping mid-glyph or overflowing.
void test_ellipsis() {
    section("ellipsis truncation");

    auto face = test_face();
    if (!face) { check(true, "no font — skipping"); return; }
    auto stack = std::make_shared<typo::FontStack>();
    stack->add(face);
    typo::StackMeasurer measurer{*stack};
    typo::StackGlyphRenderer glyphs{*stack};

    check(stack->ellipsis_glyph() != 0, "primary face resolves an ellipsis glyph");

    const std::string longtext = "This is a very long label that will not fit in the box";
    const float box_w = 120.0f;

    const auto paint_glyphs = [&](TextOverflow ov) {
        auto ui = text_owned(longtext) | font(18) | width(box_w);
        Node root = ui.build();
        root.style().text.overflow = ov;
        layout::layout_tree(root, {box_w, 40.0f}, measurer);
        DrawList dl;
        render::PaintOptions po; po.measurer = &measurer; po.glyphs = &glyphs;
        render::paint(root, dl, po);
        std::size_t n = 0; float maxx = 0.0f;
        for (const auto& inst : dl.instances()) { ++n; maxx = num::max(maxx, inst.rect.x + inst.rect.z); }
        return std::pair<std::size_t, float>{n, maxx};
    };

    const auto [clip_n, clip_r]   = paint_glyphs(TextOverflow::clip);
    const auto [ell_n,  ell_r]    = paint_glyphs(TextOverflow::ellipsis);

    check(clip_n > 0 && ell_n > 0, "both modes paint glyphs");
    check(ell_n < clip_n, "ellipsis truncates to fewer glyphs than clip");
    check(ell_r <= box_w + 2.0f, "ellipsis keeps the line within the box");
    check(clip_r > box_w, "clip overflows the box (the thing ellipsis fixes)");

    // Text that already fits is left untouched by ellipsis.
    {
        auto ui = text_owned("ok") | font(18) | width(box_w);
        Node root = ui.build();
        root.style().text.overflow = TextOverflow::ellipsis;
        layout::layout_tree(root, {box_w, 40.0f}, measurer);
        DrawList dl;
        render::PaintOptions po; po.measurer = &measurer; po.glyphs = &glyphs;
        render::paint(root, dl, po);
        // Arial merges overlapping glyphs into one bitmap run (as CoreText
        // draws it), so count drawn ink width, not instances: "ok" must be
        // drawn whole, with no ellipsis instance after it.
        float right = 0.0f; std::size_t n = 0;
        for (const auto& inst : dl.instances()) { ++n; right = num::max(right, inst.rect.x + inst.rect.z); }
        const float ok_w = measurer.measure("ok", root.style().text, num::inf).x;
        check(n >= 1 && n <= 2 && right <= ok_w + 3.0f && right >= ok_w - 3.0f,
              "short text keeps all its glyphs, no ellipsis added");
    }

    // Multi-line ellipsis (line-clamp): a long paragraph clamped to 2 lines
    // measures 2 lines tall and draws far fewer glyphs than the full block,
    // with the last line ellipsised.
    {
        const std::string prose =
            "This is a fairly long paragraph of body text that wraps across "
            "several lines in a narrow column and then keeps going for a while";
        const float w = 140.0f, lh = 16.0f * 1.4f;   // size * default line_height

        TextStyle full; full.size = 16.0f; full.overflow = TextOverflow::wrap;
        const float full_h = measurer.measure(prose, full, w).y;

        TextStyle clamp2 = full; clamp2.max_lines = 2;
        const float clamp_h = measurer.measure(prose, clamp2, w).y;

        check(full_h > lh * 2.5f, "unclamped prose is several lines tall");
        check(clamp_h <= lh * 2.0f + 1.0f, "clamp(2) measures at most two lines tall");

        // Render count: clamped draws strictly fewer glyphs.
        const auto count = [&](std::uint16_t maxl) {
            auto ui = text_owned(prose) | font(16) | wrap_text | width(w);
            Node root = ui.build();
            root.style().text.max_lines = maxl;
            layout::layout_tree(root, {w, 300.0f}, measurer);
            DrawList dl;
            render::PaintOptions po; po.measurer = &measurer; po.glyphs = &glyphs;
            render::paint(root, dl, po);
            return dl.instances().size();
        };
        check(count(2) < count(0), "clamp(2) draws fewer glyphs than the full block");
    }
}

/// Justified text: every line but the last is stretched to the box edge by
/// spreading slack across inter-word spaces. The bar: a wrapped justified
/// paragraph paints glyphs reaching closer to the right edge than the same
/// text left-aligned, and never past it.
void test_justify() {
    section("justified text");

    auto face = test_face();
    if (!face) { check(true, "no font — skipping"); return; }
    auto stack = std::make_shared<typo::FontStack>();
    stack->add(face);
    typo::StackMeasurer measurer{*stack};
    typo::StackGlyphRenderer glyphs{*stack};

    const std::string prose =
        "The quick brown fox jumps over the lazy dog again and again today";
    const float box_w = 220.0f;

    // Rightmost painted glyph x for a given alignment.
    const auto rightmost = [&](TextAlign align) {
        auto ui = text_owned(prose) | font(16) | wrap_text | text_align(align) | width(box_w);
        Node root = ui.build();
        layout::layout_tree(root, {box_w, 200.0f}, measurer);
        DrawList dl;
        render::PaintOptions po; po.measurer = &measurer; po.glyphs = &glyphs;
        render::paint(root, dl, po);
        float maxx = 0.0f;
        for (const auto& inst : dl.instances())
            maxx = num::max(maxx, inst.rect.x + inst.rect.z);   // rect = (x,y,w,h)
        return maxx;
    };

    const float left_r    = rightmost(TextAlign::left);
    const float justify_r = rightmost(TextAlign::justify);

    check(left_r > 0.0f && justify_r > 0.0f, "both alignments paint glyphs");
    // Justified lines fill more of the width than ragged-right ones.
    check(justify_r >= left_r - 0.5f, "justify never pulls text left of ragged");
    check(justify_r > box_w * 0.9f, "justified lines reach near the box edge");
    check(justify_r <= box_w + 2.0f, "justified text stays within the box");

    // The DSL `justify_text` modifier also opts into wrapping on its own.
    {
        using namespace mayag::dsl;
        Style s; justify_text.apply(s);
        check(s.text.align == TextAlign::justify, "justify_text sets justify align");
        check(s.text.overflow == TextOverflow::wrap, "justify_text implies wrapping");
    }
}

/// GSUB ligatures. The bar: on a font with a `liga` feature, fi/ffi/fl fold
/// into single glyphs (fewer glyphs, greedy longest-match), the toggle turns
/// it off, and clusters still map the ligature back to its first character.
/// Skips cleanly where no ligature font is installed.
void test_ligatures() {
    section("ligatures (GSUB)");

    // A face whose GSUB really ligates Latin f-ligatures. `has_ligatures()`
    // only says a liga feature exists; many faces use it for other scripts or
    // contextual forms, and the first such face on a system proved nothing.
    // Selected by behaviour; if ligature faces exist but NONE folds fi/fl/ff,
    // that is an engine bug and the check below fails.
    std::shared_ptr<typo::Face> lf;
    bool any_liga = false;
    for (const auto& e : typo::system::Database::instance().entries()) {
        auto f = typo::Face::from_file(e.path, e.face_index);
        if (!(f && f->has_ligatures() && f->has_glyph(U'f') && f->has_glyph(U'i'))) continue;
        any_liga = true;
        typo::FontStack probe;
        probe.add(f);
        typo::ShapeParams p; p.size_px = 32.0f;
        bool folds = false;
        for (const char* w : {"fi", "fl", "ff"}) {
            p.ligatures = false; const auto off = probe.shape(w, p).glyphs.size();
            p.ligatures = true;  const auto on  = probe.shape(w, p).glyphs.size();
            folds = folds || on < off;
        }
        if (folds) { lf = f; break; }
    }
    if (!lf && any_liga) {
        check(false, "at least one fi/fl/ffi ligature folds into fewer glyphs (no installed face ligates)");
        return;
    }
    if (!lf) {
        check(true, "no ligature font installed — skipping (not a failure)");
        return;
    }
    check(lf->has_ligatures(), "font reports GSUB ligatures");

    typo::FontStack stack;
    stack.add(lf);

    const auto count = [&](const char* s, bool liga) {
        typo::ShapeParams p; p.size_px = 32.0f; p.ligatures = liga;
        return stack.shape(s, p).glyphs.size();
    };

    // At least one common ligature must fold. Different fonts ship different
    // sets, so accept any of the classic Latin ligatures.
    bool folded = false;
    for (const char* w : {"fi", "fl", "ffi", "ffl", "ff"}) {
        const std::size_t off = count(w, false);
        const std::size_t on  = count(w, true);
        if (on < off) folded = true;
        // With ligatures off, nothing may fold.
        check(off >= on, "ligatures never ADD glyphs");
    }
    check(folded, "at least one fi/fl/ffi ligature folds into fewer glyphs");

    // Greedy longest-match: 'ffi' should fold at least as much as 'ff'.
    if (lf->has_glyph(U'f')) {
        const std::size_t ffi_on = count("ffi", true);
        check(ffi_on <= count("ff", true) + 1, "ffi uses the longest available ligature");
    }

    // The toggle is real: identical text, opposite flag, different glyph count
    // for a font that actually ligates 'fi'.
    if (count("fi", true) < count("fi", false))
        check(count("fi", true) == 1, "fi ligates to a single glyph when enabled");

    // Cluster mapping: the ligature keeps the first component's cluster so a
    // caret at the start still resolves onto it.
    {
        typo::ShapeParams p; p.size_px = 32.0f; p.ligatures = true;
        const auto r = stack.shape("fi", p);
        if (!r.glyphs.empty())
            check(r.glyphs.front().cluster == 0, "ligature keeps its first component's cluster");
    }
}

/// COLR/CPAL vector colour glyphs. The bar: parse the palette, detect colour
/// glyphs, and render them to a non-empty RGBA bitmap that actually uses more
/// than one colour (palette or gradient). Skips cleanly if no COLR font is
/// installed — so it never fails a bare CI box, but exercises the real path
/// wherever an emoji/icon/notation font exists.
void test_color_glyphs() {
    section("colour glyphs (COLR)");

    std::shared_ptr<typo::Face> cf;
    for (const char* p : {
            "/usr/share/fonts/noto/NotoZnamennyMusicalNotation-Regular.ttf",
            "/tmp/colrv1test.ttf"}) {
        if (std::filesystem::exists(p)) { cf = typo::Face::from_file(p); if (cf) break; }
    }
    if (!cf) {
        for (const auto& e : typo::system::Database::instance().entries()) {
            auto f = typo::Face::from_file(e.path, e.face_index);
            if (!f) continue;
            for (std::uint16_t g = 1; g < 400; ++g)
                if (f->has_colr(g)) { cf = f; break; }
            if (cf) break;
        }
    }
    if (!cf) {
        check(true, "no COLR font installed — skipping (not a failure)");
        return;
    }

    // Find a colour glyph and render it.
    int rendered = 0;
    std::set<std::uint32_t> palette_colors;   // distinct colours across glyphs
    bool gradient_in_one = false;             // >1 colour within a single glyph
    for (std::uint16_t g = 1; g < 1200; ++g) {
        if (!cf->has_colr(g)) continue;
        const typo::ColorBitmap cb = cf->render_colr(g, 64.0f, typo::colr::Rgba{0, 0, 0, 255});
        if (!cb.valid()) continue;
        int opaque = 0;
        std::uint32_t first = 0xFFFFFFFFu;
        for (std::size_t i = 0; i + 3 < cb.rgba.size(); i += 4) {
            if (cb.rgba[i + 3] > 8) {
                ++opaque;
                const std::uint32_t c = (static_cast<std::uint32_t>(cb.rgba[i]) << 16) |
                                        (static_cast<std::uint32_t>(cb.rgba[i + 1]) << 8) |
                                        cb.rgba[i + 2];
                palette_colors.insert(c);
                if (first == 0xFFFFFFFFu) first = c; else if (c != first) gradient_in_one = true;
            }
        }
        if (opaque > 0) ++rendered;
        // Enough evidence gathered? stop early.
        if (rendered >= 4 && (palette_colors.size() > 1 || gradient_in_one)) break;
    }
    check(rendered > 0, "at least one COLR glyph renders to non-empty RGBA");
    // The palette must actually be applied: a real COLR render is either
    // multi-coloured (v0 palette spread or a v1 gradient) or, for a mono-
    // palette font, a single NON-BLACK colour — black would mean we silently
    // fell back to the text colour instead of reading CPAL.
    const bool non_black = std::any_of(palette_colors.begin(), palette_colors.end(),
                                       [](std::uint32_t c) { return c != 0; });
    check(palette_colors.size() > 1 || gradient_in_one || non_black,
          "COLR rendering applies real palette colours (not the black fallback)");
}

/// Variable fonts: one file spanning a continuous design space (weight,
/// optical size, ...). We must parse fvar, apply gvar deltas via IUP, and
/// re-interpolate outlines as the axes move. The bar: heavier weight lays
/// down strictly more ink, and the motion is smooth and reversible.
/// Optical sizing (font-optical-sizing: auto). A variable font with an opsz
/// axis must be drawn with the design for the LOGICAL size. Before this, SF
/// rendered every UI size with its opsz-28 default, the narrow Display cut:
/// 13 px body text was 11% narrower than CoreText's and visibly cramped.
void test_optical_size() {
    section("optical size");

    FontVariations v;
    v.auto_optical_size(13.4f);
    check(v.has(FontVariations::tag_opsz), "auto optical size pins opsz");
    check(v.count == 1 && v.axes[0].value == 13.4f, "to the logical size, continuously (CoreText: 16.5 pt sets halfway between 16 and 17)");
    FontVariations chosen;
    chosen.set(FontVariations::tag_opsz, 40.0f);
    chosen.auto_optical_size(12.0f);
    check(chosen.axes[0].value == 40.0f, "an explicit optical_size() is never overridden");

    const auto params = typo::detail::shape_params_of([] { TextStyle t; t.size = 12.0f; return t; }());
    bool found = false;
    for (std::uint8_t i = 0; i < params.axis_count; ++i)
        if (params.axes[i].tag == FontVariations::tag_opsz) found = params.axes[i].value == 12.0f;
    check(found, "measuring a 12 px run selects opsz 12");

    // On a real opsz font, small text measured with auto optical size is
    // WIDER than the default (display) instance, as the design intends.
    std::shared_ptr<typo::Face> sf;
    float def_opsz = 0.0f;
    const auto try_face = [&](std::shared_ptr<typo::Face> f) {
        if (!f || !f->is_variable() || !f->has_glyph(U'n')) return false;
        for (const auto& a : f->axes())
            if (a.tag == typo::var::tag_opsz && a.min <= 13.0f && a.def >= 20.0f) {
                sf = std::move(f); def_opsz = a.def; return true;
            }
        return false;
    };
    // SF (macOS) first: its default is the Display cut, the case that bit.
    for (const auto& e : typo::system::Database::instance().entries())
        if (e.family == "System Font" && try_face(typo::Face::from_file(e.path, e.face_index))) break;
    if (!sf)
        for (const auto& e : typo::system::Database::instance().entries())
            if (try_face(typo::Face::from_file(e.path, e.face_index))) break;
    if (!sf) { check(true, "no font with a small-size opsz axis installed - skipping"); return; }
    typo::FontStack stack;
    stack.add(sf);
    typo::StackMeasurer m{stack};
    TextStyle auto_opsz; auto_opsz.size = 13.0f;
    TextStyle display = auto_opsz;
    display.variations.set(FontVariations::tag_opsz, def_opsz);   // the old behaviour: default instance
    const float w_auto = m.measure("Login shell to run in new tabs", auto_opsz, 1e9f).x;
    const float w_disp = m.measure("Login shell to run in new tabs", display, 1e9f).x;
    check(w_auto > w_disp, sf->family() + ": 13 px text uses its text design, not the default (" +
                           std::to_string(w_auto) + " vs " + std::to_string(w_disp) + " px wide)");
}

// CoreGraphics' vertical autohinter (cg_autohint.hpp): values captured from
// the live evaluate_autohint_transform under lldb for SF at 12 and 13 px.
void test_cg_autohint() {
    section("CoreGraphics autohint");
    // SF: 'o' top 1097, 'X' top 1443, head.yMax 1960, no registry stem.
    const auto t12 = typo::cg_autohint::create(12.0, 2048.0, 1097.0, 1443.0, 1960.0, 0.0, 0.0696);
    check(t12.has_value(), "12 px is hinted");
    if (t12) {
        check(t12->w_xh == 7 && t12->w_cap == 9, "12 px targets: x-height 7, cap 9");
        near(static_cast<float>(t12->bot_hi), 1.08327f, 1e-4f, "12 px baseline band top");
        near(static_cast<float>(t12->s1), 1.07999f, 1e-4f, "12 px lower slope");
        near(static_cast<float>(t12->s2), 0.96873f, 1e-4f, "12 px middle slope");
        near(static_cast<float>(t12->s3), 0.86607f, 1e-4f, "12 px upper slope");
        near(static_cast<float>(t12->apply(12.0 * 1443.0 / 2048.0 + 0.0696)), 8.9304f, 1e-3f, "cap height lands on 9 - dilation");
    }
    const auto t13 = typo::cg_autohint::create(13.0, 2048.0, 1097.0, 1443.0, 1960.0, 0.0, 0.0754);
    check(t13.has_value() && t13->s3 == 1.0, "13 px keeps the cap where it is (stretch guard)");
    check(!typo::cg_autohint::create(12.002, 2048.0, 1097.0, 1443.0, 1960.0, 0.0, 0.0).has_value(), "non-integer ppem is not hinted");
    check(!typo::cg_autohint::create(19.0, 2048.0, 1097.0, 1443.0, 1960.0, 0.0, 0.0).has_value(), "19 px is not hinted");
    check(!typo::cg_autohint::create(6.0, 2048.0, 1097.0, 1443.0, 1960.0, 0.0, 0.0).has_value(), "6 px is not hinted");

    // The registry's stemH for a CFF font is its StdHW: a user-installed
    // OpenType/CFF face (not in apple_stems.hpp) gets the same stem.
    int cff = 0, agree = 0;
    for (const auto& dir : {"/System/Library/Fonts", "/System/Library/Fonts/Supplemental"}) {
        if (!std::filesystem::exists(dir)) continue;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.path().extension() != ".otf") continue;
            auto f = typo::Face::from_file(e.path().string());
            if (!f || f->cff_std_hw() <= 0.0f) continue;
            const auto want = typo::apple::autohint_stem(f->file().postscript_name());
            if (want == 0) continue;
            ++cff;
            agree += std::lround(f->cff_std_hw() * 1000.0f / f->units_per_em()) == want;
        }
    }
    if (cff > 0)
        check(agree == cff, "CFF StdHW reproduces the registry stem (" + std::to_string(agree) + "/" +
                                std::to_string(cff) + ")");
}

// Exact nonzero union: two overlapping contours each covering 0.6 of a
// pixel column must give the union (0.8), not min(1, 0.6 + 0.6). Plus
// CoreGraphics' glyph flattening.
void test_union_coverage() {
    section("overlap union coverage");
    const auto rect = [](float x0, float y0, float x1, float y1) {
        typo::Contour c; c.start = {x0, y0};
        for (const Vec2 q : {Vec2{x0, y1}, Vec2{x1, y1}, Vec2{x1, y0}, Vec2{x0, y0}}) {
            typo::Segment s; s.kind = typo::Segment::Kind::line; s.to = q; c.segments.push_back(s);
        }
        return c;
    };
    typo::Outline o;
    o.contours.push_back(rect(10.1f, 0.0f, 10.7f, 10.0f));
    o.contours.push_back(rect(10.3f, 0.0f, 10.9f, 10.0f));
    const auto r = typo::rasterize(o, 1.0f, 1);
    double total = 0.0;
    for (const auto v : r.bitmap.pixels) total += v / 255.0;
    near(static_cast<float>(total), 8.0f, 0.1f, "overlapping bars cover their union (8 px^2), not the clamped sum");

    typo::Outline q;
    typo::Contour c; c.start = {0.0f, 0.0f};
    typo::Segment s; s.kind = typo::Segment::Kind::quad; s.c0 = {10.0f, 6.4f}; s.to = {20.0f, 0.0f};
    c.segments.push_back(s);
    typo::Segment l; l.kind = typo::Segment::Kind::line; l.to = {0.0f, 0.0f}; c.segments.push_back(l);
    q.contours.push_back(c);
    std::vector<std::pair<Vec2, Vec2>> e;
    typo::flatten(q, Affine{}, typo::cg_glyph_flattening, e);
    // aa_quadto halves until |P0 - 2C + P1| <= 0.8 px per component:
    // 12.8 -> 3.2 -> 0.8, i.e. 4 chords (+ the closing line).
    check(e.size() == 5, "a quad with |dd| 12.8 px flattens to 4 chords + the closing line (got " + std::to_string(e.size()) + ")");

    // A single contour overlapping itself (SF's 'e' draws its bar as a loop
    // over both strokes): the loop counts once.
    typo::Outline self;
    typo::Contour sc; sc.start = {10.1f, 0.0f};
    for (const Vec2 qv : {Vec2{10.1f, 10.0f}, Vec2{10.7f, 10.0f}, Vec2{10.7f, 0.0f}, Vec2{10.3f, 0.0f}, Vec2{10.3f, 10.0f},
                          Vec2{10.9f, 10.0f}, Vec2{10.9f, 0.0f}, Vec2{10.1f, 0.0f}}) {
        typo::Segment sg; sg.kind = typo::Segment::Kind::line; sg.to = qv; sc.segments.push_back(sg);
    }
    self.contours.push_back(sc);
    const auto rs = typo::rasterize(self, 1.0f, 1);
    double st = 0.0;
    for (const auto v : rs.bitmap.pixels) st += v / 255.0;
    near(static_cast<float>(st), 8.0f, 0.1f, "a self-overlapping contour covers its union once");

    // Exact box coverage of a half-plane, used for distance-field glyphs.
    near(kernel::box_halfplane_coverage(0.0f, 1.0f, 0.0f), 0.5f, 1e-5f, "edge through the centre covers half");
    near(kernel::box_halfplane_coverage(0.25f, 1.0f, 0.0f), 0.75f, 1e-5f, "axis-aligned edge is linear");
    const float r2 = std::sqrt(0.5f);
    near(kernel::box_halfplane_coverage(0.0f, r2, r2), 0.5f, 1e-5f, "45-degree edge through the centre covers half");
    near(kernel::box_halfplane_coverage(r2 * 0.5f, r2, r2), 0.875f, 1e-4f, "45-degree edge a quarter-diagonal in covers 7/8");
    near(kernel::box_halfplane_coverage(-r2, r2, r2), 0.0f, 1e-5f, "45-degree edge outside the corner covers nothing");
}

// Large text through the distance-field path must match the coverage path
// (which matches CoreText) to well under a percent.
void test_sdf_matches_bitmap() {
    section("SDF vs coverage at large sizes");
    // Both stacks use mayag's OWN outline raster: this checks that the field
    // reconstructs the coverage raster, so on Windows the native
    // (DirectWrite, hinted) path is switched off for the comparison.
    typo::FontConfig bm; bm.mode = typo::RenderMode::bitmap; bm.native_raster = false;
    typo::FontConfig hy; hy.native_raster = false;
    auto sb = typo::system::default_stack(bm);
    auto sh = typo::system::default_stack(hy);
    if (!sb || sb->face_count() == 0) return;
    const auto render = [](std::shared_ptr<typo::FontStack> st) {
        RenderOptions ro; ro.background = rgb8(0xFA, 0xFA, 0xFA); ro.fonts = st;
        auto ui = dsl::v(dsl::text_owned("Hamburg") | dsl::font(72) | dsl::fg(rgb8(0x1C, 0x1F, 0x24))) | dsl::pad(24.0f, 4.0f) |
                  dsl::width(600) | dsl::height(160);
        return render_to_pixels(ui, Vec2{600, 160}, ro);
    };
    const auto a = render(sb), b = render(sh);
    if (a.empty() || a.size() != b.size()) { check(false, "both renders produced images"); return; }
    double ink = 0.0, diff = 0.0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        const double ca = (250.0 - a[i]) / 222.0, cb = (250.0 - b[i]) / 222.0;
        ink += std::max(ca, 0.0);
        diff += std::abs(ca - cb);
    }
    // 1.2% is the macOS look with SF; the portable look with DejaVu Sans (the
    // Linux runners' default face) measures 1.21-1.27%, its thin joins
    // reconstruct slightly softer from the field.
    const double bound = typo::coregraphics_text_default ? 0.012 : 0.015;
    check(diff / ink < bound, "72 px SDF text within " + std::to_string(bound * 100.0).substr(0, 3) +
                                  "% of coverage text (got " + std::to_string(100.0 * diff / ink) + "%)");
}

// CoreText draws runs of 'connected language' faces as one bitmap run
// (max-combined); others glyph by glyph.
void test_merge_mode() {
    section("glyph merge mode");
    const auto mode = [](const char* p) {
        auto f = typo::Face::from_file(p);
        return f ? f->merge_mode() : typo::Face::MergeMode::none;
    };
    if ((std::filesystem::exists("/System/Library/Fonts/SFArabic.ttf") && captured("/System/Library/Fonts/SFArabic.ttf")))
        check(mode("/System/Library/Fonts/SFArabic.ttf") == typo::Face::MergeMode::all, "SF Arabic merges its runs (dlng Arab)");
    if ((std::filesystem::exists("/System/Library/Fonts/Supplemental/Arial.ttf") && captured("/System/Library/Fonts/Supplemental/Arial.ttf")))
        check(mode("/System/Library/Fonts/Supplemental/Arial.ttf") == typo::Face::MergeMode::all, "Arial merges (OS/2 Arabic range, no dlng)");
    if ((std::filesystem::exists("/System/Library/Fonts/Supplemental/Georgia.ttf") && captured("/System/Library/Fonts/Supplemental/Georgia.ttf")))
        check(mode("/System/Library/Fonts/Supplemental/Georgia.ttf") == typo::Face::MergeMode::none, "Georgia does not merge");
    if ((std::filesystem::exists("/System/Library/Fonts/SFNS.ttf") && captured("/System/Library/Fonts/SFNS.ttf")))
        check(mode("/System/Library/Fonts/SFNS.ttf") == typo::Face::MergeMode::table, "SF uses its MERG table");
    if ((std::filesystem::exists("/System/Library/Fonts/SFIndia.ttc") && captured("/System/Library/Fonts/SFIndia.ttc"))) {
        const auto at = [](unsigned i) {
            auto f = typo::Face::from_file("/System/Library/Fonts/SFIndia.ttc", i);
            return f ? f->merge_mode() : typo::Face::MergeMode::none;
        };
        check(at(0) == typo::Face::MergeMode::all, "SF Bangla merges (dlng lists the language 'bn')");
        check(at(7) == typo::Face::MergeMode::none, "SF Tamil does not (dlng 'ta')");
    }
}

// CoreText truncates trak values toward zero: SF at 19.75 pt interpolates
// to -46.5 units, drawn as -46 (flooring gave -47, so every glyph was one
// unit tight between 17 and 24 pt).
void test_tracking_rounding() {
    section("trak rounding");
    auto sf = captured("/System/Library/Fonts/SFNS.ttf") ? typo::Face::from_file("/System/Library/Fonts/SFNS.ttf") : nullptr;
    if (!sf) { check(true, "no SF — skipping"); return; }
    const float upem = static_cast<float>(sf->units_per_em());
    const auto units = [&](float pt) { return sf->tracking(pt, upem); };   // size_px = upem: 1 px per unit
    near(units(19.75f), -46.0f, 0.01f, "SF 19.75 pt tracks -46 units (-46.5 truncated, not floored)");
    near(units(18.5f), -49.0f, 0.01f, "SF 18.5 pt tracks -49 units (-49.0 exact)");
    near(units(20.0f), -46.0f, 0.01f, "SF 20 pt tracks -46 units (table value)");
    near(units(10.5f), 18.0f, 0.01f, "SF 10.5 pt tracks 18 units (positive values truncate too)");
}

// CoreGraphics font smoothing dilates with a SQUARE kernel: an edge moves
// by d (|nx| + |ny|), so a 45-degree edge moves d*sqrt(2), not d.
void test_square_embolden() {
    section("square-kernel embolden");
    const auto diamond = [] {
        typo::Outline o;
        typo::Contour c;
        c.start = {0.0f, 100.0f};
        for (const Vec2 q : {Vec2{100.0f, 0.0f}, Vec2{0.0f, -100.0f}, Vec2{-100.0f, 0.0f}, Vec2{0.0f, 100.0f}}) {
            typo::Segment s;
            s.kind = typo::Segment::Kind::line;
            s.to = q;
            c.segments.push_back(s);
        }
        o.contours.push_back(c);
        return o;
    };
    auto round = diamond(), square = diamond();
    round.embolden(5.0f);
    square.embolden(5.0f, true);
    const float a0 = num::abs(diamond().contours[0].signed_area()) * 0.5f;
    const float ar = num::abs(round.contours[0].signed_area()) * 0.5f;
    const float as = num::abs(square.contours[0].signed_area()) * 0.5f;
    // Square vs round on 45-degree edges: sqrt(2) times the growth.
    const float gr = ar - a0, gs = as - a0;
    check(gr > 0.0f, "round kernel grows the diamond");
    near(gs / gr, std::sqrt(2.0f), 0.05f, "square kernel moves a 45-degree edge sqrt(2)x further than round");
}

// CoreText draws a CSS weight at the face's NAMED instance of that class:
// CJK Symbols Fallback's "Regular" sits at wght 542, so weight 400 must pin
// 542 there, while SF (Regular at 400) keeps the requested value verbatim.
void test_named_weights() {
    section("named-instance weights");
    const char* cjk = "/System/Library/Fonts/CJKSymbolsFallback.ttc";
    if (!std::filesystem::exists(cjk) || !captured(cjk)) { check(true, "no captured CJK Symbols Fallback — skipping"); return; }
    auto f = typo::Face::from_file(cjk, 0);
    check(f != nullptr && f->is_variable(), "CJK Symbols Fallback loads as a variable face");
    if (!f) return;
    near(f->wght_for(400.0f), 542.0f, 0.01f, "Regular (400) -> wght 542");
    near(f->wght_for(700.0f), 725.0f, 0.01f, "Bold (700) -> wght 725");
    near(f->wght_for(100.0f), 250.0f, 0.01f, "Thin (100) -> wght 250");
    near(f->wght_for(450.0f), 569.0f, 0.01f, "between named weights interpolates");
    if (auto sf = captured("/System/Library/Fonts/SFNS.ttf") ? typo::Face::from_file("/System/Library/Fonts/SFNS.ttf") : nullptr) {
        // CoreText resolves NSFontWeight* to each face's named instance:
        // SF Medium is wght 510, Semibold 590, Ultralight (CSS 100) 30.9.
        near(sf->wght_for(400.0f), 400.0f, 0.01f, "SF Regular stays at 400");
        near(sf->wght_for(500.0f), 510.0f, 0.01f, "SF Medium (500) -> its named instance, wght 510");
        near(sf->wght_for(600.0f), 590.0f, 0.01f, "SF Semibold (600) -> wght 590");
        near(sf->wght_for(100.0f), 30.925f, 0.01f, "CSS 100 is Apple's Ultralight, the lighter of the two");
        near(sf->wght_for(200.0f), 110.725f, 0.01f, "CSS 200 is Apple's Thin");
    }
    if (auto ar = captured("/System/Library/Fonts/SFArabic.ttf") ? typo::Face::from_file("/System/Library/Fonts/SFArabic.ttf") : nullptr) {
        near(ar->wght_for(700.0f), 711.111f, 0.01f, "SF Arabic Bold sits at wght 711.1, not SF's 700");
    }
    if (auto it = captured("/System/Library/Fonts/SFNSItalic.ttf") ? typo::Face::from_file("/System/Library/Fonts/SFNSItalic.ttf") : nullptr) {
        // SF Italic's weights also move YAXS: Bold Italic is YAXS 430.
        float yaxs = 0.0f;
        it->weight_axes_for(700.0f, [&](std::uint32_t tag, float v) { if (tag == typo::ot::tag("YAXS")) yaxs = v; });
        near(yaxs, 430.0f, 0.01f, "SF Bold Italic pins YAXS 430 with its weight");
        near(it->wght_for(400.0f), 400.0f, 0.01f, "'Regular Italic' is the 400 instance");
        // gvar output stays fractional (CoreText: 'o' top 1127.25 at
        // Bold Italic, opsz 17), never rounded to font units.
        const std::pair<std::uint32_t, float> bi[] = {{typo::var::tag_wght, 700.0f}, {typo::var::tag_opsz, 17.0f}, {typo::ot::tag("YAXS"), 430.0f}};
        it->set_variations(bi);
        const auto o = it->outline(it->glyph_for('o'));
        float top = -1e9f;
        for (const auto& c : o.contours) {
            top = std::max(top, c.start.y);
            for (const auto& sg : c.segments) top = std::max(top, sg.to.y);
        }
        near(top, 1127.25f, 0.01f, "varied glyf points keep their fraction (SF Bold Italic 'o' top 1127.25)");
    }
    // Italic SF uses the real italic design (SFNSItalic.ttf), as CoreText
    // does, not a sheared upright.
    if (auto stack = typo::system::default_stack(typo::FontConfig::coregraphics());
        stack && stack->face_count() > 0 && stack->primary()->family() == "System Font") {
        TextStyle ts;
        ts.size = 16.0f;
        ts.italic = true;
        const auto r = stack->shaped("Hamburg", typo::detail::shape_params_of(ts));
        check(!r->glyphs.empty() && stack->face_at(r->glyphs[0].face_index)->is_italic(),
              "italic SF text is set in SF Italic");
    }
}

// Apple HVF ('hvgl') outlines: PingFang UI Display, 沙 (gid 1669) at wght 700.
// The water radical's strokes rotate from extremum deltas, where libhvf moves
// the translation along the rotation's arc (a screw) and composes columns as
// rigid motions; reference points are CTFontCreatePathForGlyph's, in font
// units. The previous linear composition was off by 1.44 units here.
void test_hvgl() {
    section("hvgl outlines");
    const char* pf = "/System/Library/PrivateFrameworks/FontServices.framework/Resources/Reserved/PingFangUI.ttc";
    if (!std::filesystem::exists(pf) || !captured(pf)) { check(true, "no captured PingFang UI — skipping"); return; }
    auto f = typo::Face::from_file(pf, 5);
    check(f != nullptr, "PingFang UI Display loads (hvgl outlines)");
    if (!f) return;
    f->set_variation(typo::var::tag_wght, 700.0f);
    const auto o = f->outline(1669);
    std::vector<Vec2> pts;
    for (const auto& c : o.contours) {
        pts.push_back(c.start);
        for (const auto& s : c.segments) { pts.push_back(s.c0); pts.push_back(s.to); }
    }
    check(pts.size() == 91, "沙 has CoreText's 91 path points");
    if (pts.size() != 91) return;
    const std::pair<std::size_t, Vec2> ref[] = {
        {0, {575.312f, 819.318f}}, {13, {375.573f, 247.717f}}, {26, {723.819f, 676.324f}},
        {39, {823.555f, 374.450f}}, {53, {53.297f, 722.263f}}, {54, {132.930f, 806.773f}},
        {60, {238.048f, 572.907f}}, {90, {234.800f, 72.494f}}};
    float err = 0.0f;
    for (const auto& [i, p] : ref) err = std::max({err, num::abs(pts[i].x - p.x), num::abs(pts[i].y - p.y)});
    check(err < 0.01f, "沙 bold matches CoreText within 0.01 units (got " + std::to_string(err) + ")");

    // SDF glyphs from a fallback face with ANOTHER units-per-em than the
    // primary (PingFang UI Display 1064 behind Arial's 2048) must draw at the
    // drawn size: the quad scale used to come from face 0's upem, so they
    // came out half size from the SDF threshold up. Compare the quad of the
    // same PingFang glyph drawn via a stack where it IS face 0.
    const auto quad_h = [&](std::vector<std::shared_ptr<typo::Face>> faces) {
        auto stack = std::make_shared<typo::FontStack>();
        for (auto& fc : faces) stack->add(fc);
        typo::StackMeasurer measurer{*stack};
        typo::StackGlyphRenderer glyphs{*stack};
        Node root = (text_owned("\xE4\xB8\xAD") | font(120)).build();   // 中
        layout::layout_tree(root, {400, 300}, measurer);
        DrawList dl;
        render::PaintOptions po; po.measurer = &measurer; po.glyphs = &glyphs;
        render::paint(root, dl, po);
        float h = 0.0f;
        for (const auto& inst : dl.instances()) h = num::max(h, inst.rect.w);
        return h;
    };
    auto pf_display = typo::Face::from_file(pf, 5);
    const float alone = quad_h({pf_display});
    const float behind = quad_h({test_face(), pf_display});
    check(alone > 60.0f, "120 px 中 draws a full-size SDF quad (" + std::to_string(alone) + " px)");
    near(behind, alone, 0.5f, "SDF quad size does not depend on the primary face's upem");
}

void test_variations() {
    section("variable fonts");

    // Find any variable font: prefer Adwaita (the design target), else scan
    // the system database for a face that exposes fvar axes.
    std::shared_ptr<typo::Face> vf;
    for (const char* p : {"/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf"}) {
        if (std::filesystem::exists(p)) { vf = typo::Face::from_file(p); break; }
    }
    // A PROPORTIONAL face: the sweep below asserts that heavier weights set
    // wider, which a monospace variable font (SF Mono, the first variable
    // face on macOS) correctly never does -- every weight keeps one advance.
    const auto proportional = [](const typo::Face& f) {
        const auto gi = f.glyph_for(U'i'), gm = f.glyph_for(U'M');
        return gi != 0 && gm != 0 && f.advance(gi, 100.0f) != f.advance(gm, 100.0f);
    };
    if (!vf || !vf->is_variable()) {
        for (const auto& e : typo::system::Database::instance().entries()) {
            auto f = typo::Face::from_file(e.path, e.face_index);
            if (f && f->is_variable() && f->has_glyph(U'H') && proportional(*f)) { vf = f; break; }
        }
    }
    if (!vf || !vf->is_variable()) {
        check(true, "no variable font installed — skipping (not a failure)");
        return;
    }
    check(vf->is_variable(), "variable font reports fvar axes");
    check(!vf->axes().empty(), "axis list is non-empty");

    // Locate the weight axis.
    const typo::var::Axis* wght = nullptr;
    for (const auto& a : vf->axes()) if (a.tag == typo::var::tag_wght) wght = &a;
    if (wght == nullptr) {
        check(true, "font has no wght axis — skipping weight sweep");
        return;
    }
    check(wght->min < wght->max, "wght axis has a real range");

    const auto ink = [&](char32_t cp) {
        auto o = vf->outline(vf->glyph_for(static_cast<std::uint32_t>(cp)));
        float a = 0.0f;
        for (const auto& c : o.contours) a += num::abs(c.signed_area());
        return a;
    };

    // Sweep the weight axis; a bar-heavy glyph must gain ink monotonically.
    const float lo = wght->min, hi = wght->max, mid = (lo + hi) * 0.5f;
    vf->set_variation(typo::var::tag_wght, lo);
    const float ink_lo = ink(U'H');
    near(vf->variation(typo::var::tag_wght), lo, 0.01f, "axis reads back exactly (min)");

    vf->set_variation(typo::var::tag_wght, mid);
    const float ink_mid = ink(U'H');

    vf->set_variation(typo::var::tag_wght, hi);
    const float ink_hi = ink(U'H');
    near(vf->variation(typo::var::tag_wght), hi, 0.01f, "axis reads back exactly (max)");

    check(ink_lo > 0.0f, "thin weight produces a real outline");
    check(ink_hi > ink_lo * 1.5f, "heavy weight lays down much more ink than thin");
    check(ink_mid > ink_lo && ink_mid < ink_hi, "mid weight interpolates between the two");

    // Reversibility: returning to the default must restore the default glyph
    // byte-for-byte (deterministic, replayable variation state).
    vf->set_variation(typo::var::tag_wght, wght->def);
    const float ink_def_a = ink(U'H');
    vf->set_variation(typo::var::tag_wght, hi);
    (void)ink(U'H');
    vf->reset_variations();
    const float ink_def_b = ink(U'H');
    near(ink_def_b, ink_def_a, 0.5f, "reset_variations restores the default instance");
    near(vf->variation(typo::var::tag_wght), wght->def, 0.01f, "reset returns wght to its default");

    // Clamping: out-of-range requests pin to the axis bounds.
    vf->set_variation(typo::var::tag_wght, hi + 10000.0f);
    near(vf->variation(typo::var::tag_wght), hi, 0.01f, "over-max request clamps to max");
    vf->set_variation(typo::var::tag_wght, lo - 10000.0f);
    near(vf->variation(typo::var::tag_wght), lo, 0.01f, "under-min request clamps to min");

    // HVAR: advance width should track the axis too (heavy != thin spacing),
    // otherwise heavy text overlaps. Not every variable font ships HVAR, so a
    // zero difference is only a failure when the outline itself changed a lot.
    const auto adv = [&](char32_t cp) {
        return vf->advance(vf->glyph_for(static_cast<std::uint32_t>(cp)), 100.0f);
    };
    vf->set_variation(typo::var::tag_wght, lo);
    const float adv_lo = adv(U'H');
    vf->set_variation(typo::var::tag_wght, hi);
    const float adv_hi = adv(U'H');
    check(adv_lo > 0.0f && adv_hi > 0.0f, "advance width is positive at both extremes");
    // Adwaita (and most quality VFs) widen the advance with weight; assert the
    // spacing moved at all when the shape moved a lot.
    if (ink_hi > ink_lo * 3.0f)
        check(num::abs(adv_hi - adv_lo) > 0.01f,
              "advance width varies with weight (HVAR applied)");

    // End-to-end: TextStyle::weight must drive the axis through the whole
    // layout pipeline. Measured by width where the font's advances follow
    // weight (Adwaita), and by INK where they do not: Bahnschrift -- the
    // variable face Windows ships -- keeps every weight on the same advances
    // by design (HVAR varies them with wdth only), so a width check there
    // proves nothing about the axis.
    {
        typo::FontStack stack;
        stack.add(vf);
        typo::StackMeasurer meas{stack};
        TextStyle thin;  thin.size  = 100.0f; thin.weight  = FontWeight::thin;
        TextStyle black; black.size = 100.0f; black.weight = FontWeight::black;
        const float w_thin  = meas.advance("Hamburgefons", thin);
        const float w_black = meas.advance("Hamburgefons", black);
        check(w_thin > 0.0f, "variable text measures at thin weight");
        const bool width_tracks_weight = num::abs(adv_hi - adv_lo) > 0.01f;
        if (width_tracks_weight)
            check(w_black > w_thin, "heavier TextStyle::weight measures wider through StackMeasurer");
        // The axis itself must move with the style, whatever the font does
        // with its advances: read it back after shaping each weight.
        const auto axis_after = [&](const TextStyle& s) {
            (void)meas.advance("H", s);
            return vf->variation(typo::var::tag_wght);
        };
        check(axis_after(black) > axis_after(thin), "TextStyle::weight drives the wght axis");

        // Content-addressed id: the same weight twice yields the same face id,
        // so glyph atlases hit across frames instead of re-rasterising.
        vf->set_variation(typo::var::tag_wght, hi);
        const std::uint32_t id_a = vf->id();
        vf->set_variation(typo::var::tag_wght, lo);
        vf->set_variation(typo::var::tag_wght, hi);
        check(vf->id() == id_a, "returning to a weight restores its stable face id");

        // The DSL numeric weight modifier drives the axis continuously: a
        // style built with weight(150) must measure narrower than weight(850),
        // and a mid value must land between — proving TextStyle::variations +
        // effective_weight() flow through measurement.
        using namespace mayag::dsl;
        Style thin_s, mid_s, black_s;
        weight(150).apply(thin_s);  thin_s.text.size  = 100.0f;
        weight(500).apply(mid_s);   mid_s.text.size   = 100.0f;
        weight(850).apply(black_s); black_s.text.size = 100.0f;
        const float d_thin  = meas.advance("Hamburgefons", thin_s.text);
        const float d_mid   = meas.advance("Hamburgefons", mid_s.text);
        const float d_black = meas.advance("Hamburgefons", black_s.text);
        if (width_tracks_weight) {
            check(d_black > d_thin, "DSL weight(number) drives the wght axis");
            check(d_mid > d_thin && d_mid < d_black, "DSL numeric weight interpolates");
        } else {
            const auto ax = [&](const TextStyle& s) { (void)meas.advance("H", s); return vf->variation(typo::var::tag_wght); };
            const float a_thin = ax(thin_s.text), a_mid = ax(mid_s.text), a_black = ax(black_s.text);
            check(a_black > a_thin, "DSL weight(number) drives the wght axis");
            check(a_mid >= a_thin && a_mid <= a_black, "DSL numeric weight interpolates");
        }

        // A font_axis override on wght must win over the FontWeight enum.
        Style ov;
        ov.text.weight = FontWeight::thin;
        font_axis("wght", 900.0f).apply(ov);
        ov.text.size = 100.0f;
        if (width_tracks_weight) {
            check(meas.advance("Hamburgefons", ov.text) > d_thin,
                  "font_axis(\"wght\") overrides the FontWeight enum");
        } else {
            (void)meas.advance("H", ov.text);
            check(vf->variation(typo::var::tag_wght) >= hi - 0.5f, "font_axis(\"wght\") overrides the FontWeight enum");
        }
    }

    vf->reset_variations();
}

/// SF (the macOS system font) is the hardest Latin font a UI meets: a
/// variable font whose kerning lives in GPOS extension lookups with
/// per-instance variation deltas, and whose spacing comes from Apple's `trak`
/// table. Three separate bugs each moved glyphs by up to 7 px from where
/// CoreText puts them; nothing failed, the text just looked subtly off.
/// The reference positions are CoreText's own (CTRunGetPositions, system UI
/// font), captured on macOS 26; mayag must agree to 1/100 px.
void test_coretext_parity() {
    section("CoreText parity (SF)");
    auto stack = typo::system::default_stack();
    const typo::Face* f = stack ? stack->face_at(0) : nullptr;
    // SF names itself "System Font" (SFNS.ttf); newer builds may say "SF Pro".
    const bool is_sf = f != nullptr && f->is_variable() &&
                       (f->family() == "System Font" || f->family().starts_with("SF Pro"));
    if (!is_sf || !captured("/System/Library/Fonts/SFNS.ttf")) {
        std::printf("  (skipped: SF is not the primary UI face here)\n");
        return;
    }
    // Exercises the comma-space pair (a format-1 PairPos whose variation
    // delta is addressed from the PairSet), AV/LA/VA class kerning, and the
    // size-dependent trak tracking (13 pt loosens, 26 pt tightens).
    const char* s = "Typography: Yes, LAVA";
    const float at13[] = {0.000f, 7.338f, 14.320f, 22.179f, 29.783f, 37.629f, 42.504f, 49.601f,
                          57.459f, 64.778f, 71.760f, 75.543f, 79.124f, 86.487f, 93.837f,
                          100.566f, 104.349f, 107.929f, 115.235f, 123.094f, 130.952f};
    const float at26[] = {0.000f, 12.528f, 25.522f, 40.164f, 54.322f, 68.950f, 77.118f, 90.502f,
                          105.144f, 119.446f, 132.441f, 138.436f, 144.164f, 158.012f, 171.685f,
                          184.137f, 188.978f, 194.706f, 208.659f, 223.486f, 238.209f};
    for (const auto& [size, want] : {std::pair{13.0f, at13}, std::pair{26.0f, at26}}) {
        TextStyle st;
        st.size = size;
        const auto run = stack->shaped(s, typo::detail::shape_params_of(st));
        float x = 0.0f, worst = 0.0f;
        for (std::size_t i = 0; i < run->glyphs.size() && i < 21; ++i) {
            worst = num::max(worst, num::abs(x - want[i]));
            x += run->glyphs[i].advance;
        }
        check(run->glyphs.size() == 21 && worst < 0.01f,
              std::to_string(static_cast<int>(size)) + " px glyph positions match CoreText (worst " +
              std::to_string(worst) + " px)");
    }
    TextStyle bold_style;
    bold_style.size = 13.0f;
    bold_style.weight = FontWeight::bold;
    near(stack->shaped(s, typo::detail::shape_params_of(bold_style))->width, 149.963f, 0.01f,
         "bold kerning deltas follow the weight axis (line width matches CoreText)");

    // Tracking goes on every advancing glyph and BEFORE mark attachment
    // (CTRunGetPositions for SF Devanagari at 16 px): the u-sign under da
    // sits at 8.992, not 9.36, and the line is 35.904 px wide.
    TextStyle deva;
    deva.size = 16.0f;
    const auto dr = stack->shaped("\u0926\u0941\u0928\u093F\u092F\u093E", typo::detail::shape_params_of(deva));
    if (dr && dr->glyphs.size() == 7 && stack->face_at(dr->glyphs[0].face_index)->file().postscript_name() == ".SFDevanagari-Regular") {
        near(dr->glyphs[0].advance + dr->glyphs[1].offset.x, 8.992f, 0.002f, "Devanagari mark stays on its anchor under trak tracking");
        near(dr->width, 35.904f, 0.002f, "tracked Devanagari line width matches CoreText");
    }
}

/// Font smoothing and subpixel positioning: what makes the rasteriser draw
/// what CoreText draws instead of a thin, grid-snapped approximation.
void test_smoothing() {
    section("smoothing and subpixel phases");
    // The macOS look is the platform default only on Apple; ask for it
    // explicitly so these checks hold everywhere.
    auto stack = typo::system::default_stack(typo::FontConfig::coregraphics());
    if (stack == nullptr || stack->face_count() == 0) return;
    {
        constexpr typo::FontConfig dflt{};
        constexpr auto cg = typo::FontConfig::coregraphics();
        constexpr auto pt = typo::FontConfig::portable();
        check(cg.smoothing > 0.0f && cg.cg_flattening && pt.smoothing == 0.0f && !pt.cg_flattening,
              "coregraphics() and portable() presets select the two looks");
        check((dflt.smoothing > 0.0f) == typo::coregraphics_text_default &&
                  dflt.cg_flattening == typo::coregraphics_text_default,
              "FontConfig{} follows MAYAG_COREGRAPHICS_TEXT (the macOS look by default)");
    }
    const std::uint16_t gid = stack->face_at(0)->glyph_for('l');
    if (gid == 0) return;

    const auto ink = [](const typo::CachedGlyph* g, const typo::FontStack& fs) {
        double sum = 0.0;
        const auto& plane = fs.atlas();
        const int x0 = static_cast<int>(g->atlas_rect.left());
        const int y0 = static_cast<int>(g->atlas_rect.top());
        for (int y = 0; y < static_cast<int>(g->size.y); ++y)
            for (int x = 0; x < static_cast<int>(g->size.x); ++x)
                sum += plane.pixels()[static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(plane.width()) +
                                      static_cast<std::size_t>(x0 + x)];
        return sum / 255.0;
    };

    // Smoothing grows the outline: white text (level 4) carries more ink
    // than with smoothing off, and black text (level 0, which macOS grows
    // least) sits between the two.
    typo::FontConfig off = stack->config();
    off.smoothing = 0.0f;
    auto plain = typo::system::default_stack(off);
    const double light_ink = ink(stack->glyph(0, gid, 13.0f, 400.0f, {}, false, 0, 4), *stack);
    const double dark  = ink(stack->glyph(0, gid, 13.0f, 400.0f, {}, false, 0, 0), *stack);
    const double none  = ink(plain->glyph(0, gid, 13.0f), *plain);
    check(light_ink > none * 1.05, "smoothing thickens light text (" + std::to_string(light_ink) +
                                   " vs " + std::to_string(none) + ")");
    check(dark > none && dark < light_ink, "dark text is grown less than light at 13 px");
    // CoreGraphics' growth: min(0.3, (0.0058 + 0.0025 level) px per px), level =
    // round(4 * linear luminance of the text colour).
    near(stack->smoothing_px(16.0f, 0), 0.0928f, 1e-4f, "black text at 16 px grows 0.0928 px");
    near(stack->smoothing_px(16.0f, 1), 0.1328f, 1e-4f, "level 1 at 16 px grows 0.1328 px");
    near(stack->smoothing_px(60.0f, 4), 0.3f, 1e-4f, "growth caps at 0.3 px");
    check(typo::FontStack::smoothing_level(rgb8(0x1C, 0x1F, 0x24)) == 0 &&
              typo::FontStack::smoothing_level(rgb8(0x0A, 0x64, 0xDC)) == 1 &&
              typo::FontStack::smoothing_level(rgb8(0xA8, 0xA8, 0xA8)) == 2 &&
              typo::FontStack::smoothing_level(rgb8(0x00, 0xFF, 0x00)) == 3 &&
              typo::FontStack::smoothing_level(rgb8(0xF8, 0xF8, 0xF8)) == 4,
          "smoothing level follows the text colour's luminance quarters");

    // Subpixel phases: every quarter-pixel position is its own raster, and
    // the ink is the same at each (the glyph moved, it did not change).
    const double p0 = ink(stack->glyph(0, gid, 13.0f, 400.0f, {}, false, 0), *stack);
    const double p2 = ink(stack->glyph(0, gid, 13.0f, 400.0f, {}, false, 2), *stack);
    check(stack->glyph(0, gid, 13.0f, 400.0f, {}, false, 2) !=
              stack->glyph(0, gid, 13.0f, 400.0f, {}, false, 0),
          "each subpixel phase is a separate atlas entry");
    near(static_cast<float>(p2), static_cast<float>(p0), static_cast<float>(p0) * 0.02f,
         "a phase shift moves the glyph without changing its ink");

    // split_pen: the pen's fraction is FLOORED to the size's phase grid,
    // CoreGraphics' rule (measured): quarters <= 11.1 px, thirds <= 16.7 px,
    // halves <= 33.3 px, whole pixels above, at most 5 below 8.4 px.
    check(typo::FontStack::subpixel_phases_for(11.0f) == 4 && typo::FontStack::subpixel_phases_for(12.0f) == 3 &&
              typo::FontStack::subpixel_phases_for(16.0f) == 3 && typo::FontStack::subpixel_phases_for(17.0f) == 2 &&
              typo::FontStack::subpixel_phases_for(33.0f) == 2 && typo::FontStack::subpixel_phases_for(34.0f) == 1 &&
              typo::FontStack::subpixel_phases_for(6.0f) == 5,
          "subpixel phases per size follow CoreGraphics");
    for (float x : {0.0f, 0.12f, 0.13f, 0.5f, 0.88f, 3.37f, -1.3f})
        for (int n : {1, 2, 3, 4, 5}) {
            const auto sp = typo::FontStack::split_pen(x, n);
            const float back = sp.pixel + static_cast<float>(sp.phase) / static_cast<float>(n);
            check(sp.phase >= 0 && sp.phase < n && back <= x + 1e-3f && x - back < 1.0f / static_cast<float>(n) + 1e-3f,
                  "split_pen(" + std::to_string(x) + ", " + std::to_string(n) + ") floors to the grid");
        }
}

void test_cff2() {
    std::printf("\n-- CFF2 outlines --\n");
    const std::string path = "/System/Library/Fonts/SFIndia.ttc";
    if (!std::filesystem::exists(path)) { std::printf("  (SFIndia.ttc absent, skipped)\n"); return; }
    auto face = typo::Face::from_file(path, 0);
    check(face != nullptr, "SFIndia (CFF2) opens");
    if (!face) return;
    check(face->has_outlines(), "CFF2 face reports outlines");
    int inked = 0;
    for (std::uint16_t g = 1; g < 200; ++g) inked += face->outline(g).empty() ? 0 : 1;
    check(inked > 150, "CFF2 glyphs have outlines (" + std::to_string(inked) + "/199)");
    // Reference values captured from HarfBuzz hb_font_draw_glyph (face 0, gid 10).
    const auto o = face->outline(10);
    const bool exact = !o.empty() && o.contours[0].start.x == 376.0f &&
        o.contours[0].start.y == 616.0f && o.contours[0].segments.size() > 1 &&
        o.contours[0].segments[0].to.x == 449.0f && o.contours[0].segments[0].to.y == 616.0f &&
        o.contours[0].segments[1].c0.x == 449.0f && o.contours[0].segments[1].c0.y == 628.0f;
    check(exact, "CFF2 gid 10 control points match HarfBuzz");
    const Rect b0 = o.control_bounds();
    face->set_variation(typo::var::tag_wght, 900.0f);
    const Rect b1 = face->outline(10).control_bounds();
    check(b0.min().x != b1.min().x || b0.max().x != b1.max().x || b0.max().y != b1.max().y,
          "CFF2 outline changes with wght (blend)");
    face->reset_variations();
}

/// FontStack end to end: itemisation into bidi/script/face runs, OpenType
/// shaping per run, logical storage with levels for visual painting.
void test_complex_text() {
    section("complex scripts and bidi through FontStack");
    auto stack = typo::system::default_stack();
    if (stack == nullptr) return;
    TextStyle st;
    st.size = 16.0f;
    const auto sp = typo::detail::shape_params_of(st);

    // Mixed direction: "abc " LTR, Arabic RTL, " (12) xyz". Levels follow
    // UAX #9 with the paragraph level from the first strong char (L).
    const std::string mixed = "abc \xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a 12 xyz";   // abc عربي 12 xyz
    const auto run = stack->shaped(mixed, sp);
    check(run->has_rtl, "mixed text is flagged RTL");
    bool saw_odd = false, saw_even = false, logical = true;
    std::uint32_t last_cluster = 0;
    for (const auto& g : run->glyphs) {
        saw_odd |= (g.level & 1) != 0;
        saw_even |= (g.level & 1) == 0;
        // Storage is logical: clusters never decrease across the string
        // (within an RTL run glyphs are stored logically too).
        if (g.cluster + 3 < last_cluster) logical = false;
        last_cluster = std::max(last_cluster, g.cluster);
    }
    check(saw_odd && saw_even, "Arabic runs carry odd levels, Latin even");
    check(logical, "glyphs are stored in logical order");

    // Arabic joins: the four letters of عربي shape to contextual forms, so
    // their glyph ids differ from the isolated forms of the same letters.
    const auto word = stack->shaped("\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a", sp);
    int changed = 0, letters = 0;
    for (const char* iso : {"\xd8\xb9", "\xd8\xb1", "\xd8\xa8", "\xd9\x8a"}) {
        const auto one = stack->shaped(iso, sp);
        if (letters < static_cast<int>(word->glyphs.size()) && !one->glyphs.empty() &&
            word->glyphs[static_cast<std::size_t>(letters)].glyph_id != one->glyphs[0].glyph_id)
            ++changed;
        ++letters;
    }
    check(changed >= 2, "Arabic letters take joining forms in a word (" + std::to_string(changed) + " of 4 changed)");

    // Devanagari: क + ि (pre-base matra) is reordered so the matra glyph
    // comes FIRST in visual (= logical, LTR) order.
    const auto ki = stack->shaped("\xe0\xa4\x95\xe0\xa4\xbf", sp);
    if (ki->glyphs.size() == 2) {
        const auto ka = stack->shaped("\xe0\xa4\x95", sp);
        check(!ka->glyphs.empty() && ki->glyphs[1].glyph_id == ka->glyphs[0].glyph_id,
              "Devanagari pre-base matra is reordered before the consonant");
    }
    // A conjunct forms: क्ष shapes to fewer glyphs than its 3 characters
    // (when an installed face covers Devanagari at all).
    const auto ksa = stack->shaped("\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7", sp);
    if (ksa->glyphs.empty() || ksa->glyphs[0].missing)
        std::printf("  note: no installed face covers Devanagari, skipping the conjunct check\n");
    else
        check(ksa->glyphs.size() < 3, "Devanagari conjunct क्ष forms a ligature");

    // Combining marks attach: ễ as e + U+0302 + U+0303 has the marks at
    // zero advance on the base's cluster.
    const auto mk = stack->shaped("e\xcc\x82\xcc\x83", sp);
    float adv = 0.0f;
    for (const auto& g : mk->glyphs) adv += g.advance;
    const auto e = stack->shaped("e", sp);
    check(!e->glyphs.empty() && std::abs(adv - e->glyphs[0].advance) < 0.01f,
          "stacked combining marks add no advance");
}

/// The flicker bug: an atlas that fills mid-frame repacks, moving glyphs
/// whose old uvs are already in that frame's draw list. The atlas must move
/// glyphs between frames (maintain), grow when its working set churns, and
/// report any mid-frame move so the frame is repainted.
void test_atlas_churn() {
    section("atlas under churn (no mid-frame moves)");
    typo::Atlas atlas{256};
    typo::Bitmap bmp{20, 20};
    for (auto& p : bmp.pixels) p = 200;
    std::uint16_t next_gid = 1;
    int mid_frame_moves = 0, frames = 0;
    const std::uint32_t gen0 = atlas.generation();
    // Each frame draws 12 glyphs it drew before and 6 new ones (an animated
    // weight produces new rasters every frame).
    std::vector<typo::GlyphKey> live;
    for (std::uint64_t f = 1; f <= 2000; ++f) {
        atlas.set_frame(f);
        atlas.maintain();
        (void)atlas.take_moved();   // a between-frame move needs no repaint
        for (std::size_t k = live.size() > 12 ? live.size() - 12 : 0; k < live.size(); ++k)
            (void)atlas.find(live[k]);
        for (int n = 0; n < 6; ++n) {
            typo::GlyphKey key;
            key.face_id = 1;
            key.glyph_id = next_gid++;
            key.size_bucket = 64;
            if (atlas.insert(key, bmp, {}, 10.0f, 1.0f, 0.0f, false)) live.push_back(key);
        }
        mid_frame_moves += atlas.take_moved() ? 1 : 0;
        ++frames;
    }
    check(mid_frame_moves == 0, "glyphs never move mid-frame (" + std::to_string(mid_frame_moves) + " frames repainted)");
    check(atlas.width() > 256, "a churning working set grows the atlas (" + std::to_string(atlas.width()) + ")");
    check(atlas.generation() != gen0, "every move is a new generation (GPU gets a fresh texture)");
    // Every live glyph's uvs point inside the texture.
    bool in_bounds = true;
    for (std::size_t k = live.size() - 12; k < live.size(); ++k)
        if (const auto* g = atlas.find(live[k]))
            in_bounds &= g->atlas_rect.right() <= static_cast<float>(atlas.width()) &&
                         g->atlas_rect.bottom() <= static_cast<float>(atlas.height());
    check(in_bounds, "surviving glyphs keep valid uvs after repacks");

    // Forced overflow inside a frame is reported, so the host repaints.
    typo::Atlas tiny{64};
    tiny.set_frame(1);
    typo::Bitmap big{30, 30};
    int inserted = 0;
    for (std::uint16_t g = 1; g <= 8; ++g) {
        typo::GlyphKey key;
        key.face_id = 2;
        key.glyph_id = g;
        if (tiny.insert(key, big, {}, 10.0f, 1.0f, 0.0f, false)) ++inserted;
    }
    check(inserted == 8 && tiny.take_moved(), "a mid-frame overflow grows the atlas and flags a repaint");
    (void)frames;
}

/// Wrapping uses UAX #14: no break before closing punctuation or inside a
/// number, a break after a hyphen, CJK breaks between ideographs but not
/// before 。, and Thai (no spaces) wraps between syllables.
void test_uax14_wrapping() {
    section("UAX #14 wrapping");
    auto stack = typo::system::default_stack();
    if (stack == nullptr) return;
    typo::StackMeasurer m{*stack};
    (void)m;
    TextStyle st;
    st.size = 16.0f;
    const auto lines_of = [&](std::string_view s, float w) {
        const auto shaped = stack->shaped(s, typo::detail::shape_params_of(st));
        const auto lines = typo::StackMeasurer::wrap_lines(s, *shaped, w);
        std::vector<std::string> out;
        for (const auto& l : lines) {
            if (l.begin >= l.end) continue;
            std::size_t b0 = s.size(), b1 = 0;
            for (std::size_t i = l.begin; i < l.end; ++i) {
                b0 = std::min<std::size_t>(b0, shaped->glyphs[i].cluster);
                b1 = std::max<std::size_t>(b1, shaped->glyphs[i].cluster);
            }
            // extend b1 to the end of its code point
            std::size_t e = b1 + 1;
            while (e < s.size() && (static_cast<unsigned char>(s[e]) & 0xC0) == 0x80) ++e;
            std::string t{s.substr(b0, e - b0)};
            while (!t.empty() && t.front() == ' ') t.erase(t.begin());
            out.push_back(t);
        }
        return out;
    };
    const auto thai = lines_of("\xe0\xb8\xa0\xe0\xb8\xb2\xe0\xb8\xa9\xe0\xb8\xb2\xe0\xb9\x84\xe0\xb8\x97\xe0\xb8\xa2"
                               "\xe0\xb8\x99\xe0\xb8\xb1\xe0\xb9\x89\xe0\xb8\x99\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xa2\xe0\xb8\x87\xe0\xb8\xb2\xe0\xb8\xa1"
                               "\xe0\xb8\xa1\xe0\xb8\xb2\xe0\xb8\x81\xe0\xb8\x88\xe0\xb8\xa3\xe0\xb8\xb4\xe0\xb8\x87\xe0\xb9\x86", 60.0f);
    check(thai.size() > 1, "a Thai line with no spaces still wraps (" + std::to_string(thai.size()) + " lines)");
    const auto num = lines_of("total 1,234,567.89 USD", 110.0f);
    bool number_whole = false;
    for (const auto& l : num) number_whole |= l.find("1,234,567.89") != std::string::npos;
    check(number_whole, "a number is never split across lines");
    const auto paren = lines_of("aaaa bbbb (cccc) dddd!", 64.0f);
    bool no_orphan = paren.size() > 1;
    for (const auto& l : paren) no_orphan &= !(l.starts_with(")") || l.starts_with("!"));
    check(no_orphan, "no line starts with closing punctuation");
    const auto cjk = lines_of("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe6\x96\x87\xe7\xab\xa0\xe3\x80\x82\xe6\x94\xb9\xe8\xa1\x8c", 40.0f);
    bool cjk_ok = cjk.size() > 1;
    for (const auto& l : cjk) cjk_ok &= !l.starts_with("\xe3\x80\x82");
    check(cjk_ok, "CJK wraps between ideographs, never before \xe3\x80\x82");
}

/// Carets, hits and selections are VISUAL: in RTL text the caret for the
/// first character is at the RIGHT edge, a selection across a direction
/// change is two rectangles, and a line's trailing space sits at its end.
void test_bidi_editing() {
    section("bidi editing geometry");
    auto stack = typo::system::default_stack();
    if (stack == nullptr) return;
    typo::StackMeasurer m{*stack};
    TextStyle st;
    st.size = 16.0f;
    const std::string ar = "\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a";   // عربي (4 letters, 8 bytes)
    const float w = m.advance(ar, st);
    check(std::abs(m.caret_x(ar, st, 0) - w) < 0.5f, "RTL: the caret before the first letter is at the right edge");
    check(std::abs(m.caret_x(ar, st, ar.size())) < 0.5f, "RTL: the caret at the end is at the left edge");
    check(m.caret_x(ar, st, 2) < m.caret_x(ar, st, 0) && m.caret_x(ar, st, 4) < m.caret_x(ar, st, 2),
          "RTL: carets move LEFT as the offset grows");
    check(m.hit(ar, st, w - 0.5f) == 0 && m.hit(ar, st, 0.5f) == ar.size(), "RTL: clicking the right edge is offset 0");

    // "abc عربي": select "c عر" logically -> two visual spans (LTR part, RTL part).
    const std::string mixed = "abc " + ar;
    const auto spans = m.selection_spans(mixed, st, 2, 4 + 4);
    check(spans.size() == 2, "a selection across a direction change is two rectangles (" + std::to_string(spans.size()) + ")");
    // LTR text: the default prefix-width behaviour still holds exactly.
    const std::string en = "Hello";
    check(std::abs(m.caret_x(en, st, 3) - m.advance("Hel", st)) < 0.6f, "LTR carets are prefix widths");
    check(m.hit(en, st, m.caret_x(en, st, 2) + 0.2f) == 2, "LTR hit-test lands on the nearest caret");

    // Per-line L1: in an LTR paragraph, the space after an RTL word that
    // ends a line goes to the line's END (paragraph level), not between
    // the Arabic glyphs and the line start.
    const std::string line = "x " + ar + " ";
    const auto shaped = stack->shaped(line, typo::detail::shape_params_of(st));
    std::vector<std::size_t> order;
    typo::detail::visual_glyph_order(line, *shaped, 0, shaped->glyphs.size(), order);
    check(!order.empty() && shaped->glyphs[order.back()].cluster == line.size() - 1,
          "L1: a line's trailing space is painted last (visual end)");
}

/// `| lang(...)` reaches the shaper: SF's Serbian language system swaps
/// the Cyrillic б for its Serbian form (HarfBuzz agrees, see the corpus).
void test_language() {
    section("language-specific forms");
    auto stack = typo::system::default_stack();
    const typo::Face* f = stack ? stack->face_at(0) : nullptr;
    if (f == nullptr || f->family() != "System Font") { std::printf("  (skipped: SF not primary)\n"); return; }
    TextStyle ru, sr;
    ru.size = sr.size = 16.0f;
    sr.language = dsl::lang("sr").tag;
    const auto a = stack->shaped("\xd0\xb1", typo::detail::shape_params_of(ru));
    const auto b = stack->shaped("\xd0\xb1", typo::detail::shape_params_of(sr));
    check(!a->glyphs.empty() && !b->glyphs.empty() && a->glyphs[0].glyph_id != b->glyphs[0].glyph_id,
          "lang(\"sr\") selects the Serbian form of \xd0\xb1");
    check(typo::ot::ot_language_tags("zh-Hant-HK").front() == typo::ot::make_tag('Z', 'H', 'H', ' '),
          "zh-Hant-HK maps to ZHH");
    check(typo::ot::ot_language_tags("tr-TR").front() == typo::ot::make_tag('T', 'R', 'K', ' '), "tr-TR maps to TRK");
}

/// The default stack resolves characters to the face CoreText's UI-font
/// fallback picks (CTFontCreateForString, measured), not just any face that
/// covers them.
void test_apple_cascade() {
    section("macOS fallback cascade (vs CoreText)");
#if defined(__APPLE__)
    // apple_cascade.hpp was derived from one macOS release; another ships a
    // different fallback set (macOS 15 picks Arial Unicode MS for symbols
    // that 27 sends to SF Devanagari and SF Tamil).
    if (!captured("/System/Library/Fonts/SFIndia.ttc") || !captured("/System/Library/Fonts/SFNS.ttf")) return;
    auto stack = typo::system::default_stack();
    struct Want { char32_t cp; const char* family; };
    for (const Want w : {Want{0x5D0, ".SF Hebrew"}, Want{0x627, ".SF Arabic"}, Want{0x915, ".SF Devanagari"},
                         Want{0xB95, ".SF Tamil"}, Want{0x531, ".SF Armenian"}, Want{0x10D0, ".SF Georgian"},
                         Want{0xAC00, ".Apple SD Gothic NeoI"}, Want{0x2800 + 0x03, "Apple Braille"},
                         Want{0x1D400, "STIX Two Math"}, Want{0x1F600, ".Apple Color Emoji UI"}}) {
        const auto [fi, g] = stack->resolve(w.cp);
        const auto* f = stack->face_at(fi);
        const std::string got = g != 0 && f != nullptr ? f->family() : "(none)";
        check(got == w.family,
              "U+" + std::to_string(static_cast<unsigned>(w.cp)) + " falls back to " + w.family + " (got " + got + ")");
    }
    // Hebrew points stay with the SF Hebrew base (an exception range).
    const auto [hb, hg] = stack->resolve(0x5B8);
    check(hg != 0 && stack->face_at(hb)->family() == ".SF Hebrew", "Hebrew points come from SF Hebrew, like CoreText");
#else
    check(true, "CoreText cascade is macOS-only: skipped");
#endif
}

/// Emoji land where CoreText puts them. The numbers are CTFontGetBounding-
/// RectsForGlyphs / CTRunGetAdvances for U+1F600 in .AppleColorEmojiUI (the
/// face CTFontCreateUIFontForLanguage falls back to), measured on macOS.
void test_emoji_placement() {
    section("emoji placement (vs CoreText)");
    {
        // scale_rgba is CoreGraphics' kCGInterpolationHigh, weights measured
        // with one-hot rows through CGContextDrawImage: shrinking 20 -> 10,
        // texel 8 lands 0.09 / 0.45 / -0.04 on outputs 3..5; enlarging
        // 10 -> 16, texel 3 gives outputs 4, 5, 6 0.12, 0.94, 0.50.
        const auto row = [](int sw, int dw, int k) {
            std::vector<std::uint8_t> src(static_cast<std::size_t>(sw) * 4, 0);
            for (int i = 0; i < sw; ++i) { src[i * 4 + 0] = i == k ? 228 : 128; src[i * 4 + 3] = 255; }
            const auto o = mayag::typo::scale_rgba(src, sw, 1, dw, 1);
            std::vector<float> w(static_cast<std::size_t>(dw));
            for (int x = 0; x < dw; ++x) w[static_cast<std::size_t>(x)] = (static_cast<float>(o[x * 4]) - 128.0f) / 100.0f;
            return w;
        };
        const auto d = row(20, 10, 8);
        near(d[3], 0.09f, 0.025f, "shrink: Catmull-Rom tap on the near side");
        near(d[4], 0.45f, 0.025f, "shrink: corner-aligned centre weight");
        near(d[5], -0.04f, 0.025f, "shrink: negative lobe");
        const auto u = row(10, 16, 3);
        near(u[4], 0.12f, 0.02f, "enlarge: eased bilinear, 1/8 step");
        near(u[5], 0.94f, 0.02f, "enlarge: eased bilinear, 15/16");
        near(u[6], 0.50f, 0.02f, "enlarge: eased bilinear, half");
    }
#if defined(__APPLE__)
    using namespace mayag::typo;
    auto ui = Face::from_file("/System/Library/Fonts/Apple Color Emoji.ttc", 1);
    if (!ui) { check(true, "Apple Color Emoji not installed: skipped"); return; }
    FontStack stack;
    stack.add(ui);
    const std::uint16_t g = ui->glyph_for(0x1F600);
    struct Want { float pt, w, bottom, adv; };
    // size, image width, image bottom (y-up), advance
    for (const Want w : {Want{12, 15.0f, -3.0f, 17}, Want{13, 16.25f, -3.25f, 19}, Want{20, 22.0f, -3.5f, 25},
                         Want{24, 24.0f, -3.0f, 27}, Want{48, 48.0f, -6.0f, 52}}) {
        const ShapeParams::Axis opsz{ot::tag("opsz"), w.pt};
        const CachedGlyph* cg = stack.glyph(0, g, w.pt, 400.0f, {&opsz, 1});
        check(cg != nullptr && cg->is_color, "\U0001F600 is a colour glyph at " + std::to_string(int(w.pt)) + " pt");
        if (!cg) continue;
        const float bottom = -(cg->bearing.y + cg->size.y);   // y-up
        check(std::fabs(cg->size.x - w.w) <= 1.0f && std::fabs(bottom - w.bottom) <= 1.0f,
              "\U0001F600 at " + std::to_string(int(w.pt)) + " pt: image " + std::to_string(cg->size.x) + " px wide, bottom " +
              std::to_string(bottom) + " (CoreText " + std::to_string(w.w) + ", " + std::to_string(w.bottom) + ")");
        ShapeParams sp; sp.size_px = w.pt; sp.axes[0] = opsz; sp.axis_count = 1;
        const auto r = stack.shape("\U0001F600", sp);
        check(!r.glyphs.empty() && std::fabs(r.glyphs[0].advance - w.adv) < 0.5f,
              "\U0001F600 advance at " + std::to_string(int(w.pt)) + " pt is CoreText's " + std::to_string(int(w.adv)) +
              " (got " + std::to_string(r.glyphs.empty() ? 0.0f : r.glyphs[0].advance) + ")");
    }
    // The emoji's top is ABOVE the baseline (it used to hang a full em below).
    const CachedGlyph* big = stack.glyph(0, g, 48.0f);
    check(big != nullptr && big->bearing.y < -30.0f, "emoji sit on the baseline, not below it");
#else
    check(true, "Apple Color Emoji is macOS-only: skipped");
#endif
}

int main() {
    std::printf("mayag font engine\n=================");

    test_robustness();
    test_cff2();
    test_all_system_fonts();
    test_cmap();
    test_outlines();
    test_rasterization();
    test_sdf();
    test_atlas();
    test_atlas_churn();
    test_shaping();
    test_fallback();
    test_line_breaking();
    test_metrics();
    test_rendering();
    test_variations();
    test_named_weights();
    test_tracking_rounding();
    test_cg_autohint();
    test_union_coverage();
    test_sdf_matches_bitmap();
    test_merge_mode();
    test_square_embolden();
    test_hvgl();
    test_optical_size();
    test_caret();
    test_italic();
    test_synthetic_italic();
    test_synthetic_bold();
    test_ellipsis();
    test_justify();
    test_ligatures();
    test_color_glyphs();
    test_emoji_placement();
    test_apple_cascade();
    test_coretext_parity();
    test_smoothing();
    test_complex_text();
    test_uax14_wrapping();
    test_bidi_editing();
    test_language();

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
