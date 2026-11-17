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

    int loaded = 0, with_outlines = 0, cff = 0, truetype = 0, kerned = 0;

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
            if (gid != 0 && !face->outline(gid).empty()) ++with_outlines;
        }
    }

    std::printf("  loaded %d faces: %d TrueType, %d CFF, %d with kerning, %d rendered\n",
                loaded, truetype, cff, kerned, with_outlines);
    check(loaded > 10, "loaded a meaningful number of system faces");
    check(with_outlines > loaded / 2, "most faces produced real outlines");
    check(cff > 0, "at least one CFF/OpenType face was exercised");
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
    for (std::uint32_t cp = 0; cp < 0x2000; ++cp) {
        const auto g = face->glyph_for(cp);
        if (g != 0) {
            ++mapped;
            check(g < face->num_glyphs(), "glyph id in range for U+" + std::to_string(cp));
            if (failures > 0) break;   // do not spam
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

    // SDF mode: every size shares ONE entry. This is the whole reason the
    // typography page above renders in a single draw call.
    typo::FontStack sdf_stack{typo::FontConfig{.mode = typo::RenderMode::sdf}};
    sdf_stack.add(face);
    for (float size : {8.0f, 12.0f, 16.0f, 24.0f, 48.0f, 96.0f}) {
        (void)sdf_stack.glyph(0, face->glyph_for('A'), size);
    }
    check(sdf_stack.atlas().glyph_count() == 1,
          "SDF mode shares one entry across 6 sizes (got " +
          std::to_string(sdf_stack.atlas().glyph_count()) + ")");
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
    for (std::uint32_t c = 0; c < 9; ++c) {
        const float x = word.x_of_cluster(c);
        check(word.cluster_at(x + 1.0f) == c,
              "cluster " + std::to_string(c) + " round-trips through hit testing");
        if (failures > 0) break;
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
    for (const auto& g : latin.glyphs) {
        check(g.face_index == 0, "Latin comes from the primary face");
        if (failures > 0) break;
    }

    // A mixed string must draw from MORE than one face. This is the property
    // that turns tofu boxes into readable text.
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

        check(dl.size() > 20, "the scene produced instances");
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
        if (f && !f->is_variable() && f->has_glyph(U'H') && f->has_outlines()) { sf = f; break; }
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
        check(dl.instances().size() == 2, "short text keeps all its glyphs, no ellipsis added");
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
    check(v.count == 1 && v.axes[0].value == 13.0f, "to the logical size, rounded to whole points");
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
    // layout pipeline, so measuring the same string at thin vs black yields a
    // wider line at black — no discrete face swap, one variable file.
    {
        typo::FontStack stack;
        stack.add(vf);
        typo::StackMeasurer meas{stack};
        TextStyle thin;  thin.size  = 100.0f; thin.weight  = FontWeight::thin;
        TextStyle black; black.size = 100.0f; black.weight = FontWeight::black;
        const float w_thin  = meas.advance("Hamburgefons", thin);
        const float w_black = meas.advance("Hamburgefons", black);
        check(w_thin > 0.0f, "variable text measures at thin weight");
        check(w_black > w_thin,
              "heavier TextStyle::weight measures wider through StackMeasurer");

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
        check(d_black > d_thin, "DSL weight(number) drives the wght axis");
        check(d_mid > d_thin && d_mid < d_black, "DSL numeric weight interpolates");

        // A font_axis override on wght must win over the FontWeight enum.
        Style ov;
        ov.text.weight = FontWeight::thin;
        font_axis("wght", 900.0f).apply(ov);
        ov.text.size = 100.0f;
        check(meas.advance("Hamburgefons", ov.text) > d_thin,
              "font_axis(\"wght\") overrides the FontWeight enum");
    }

    vf->reset_variations();
}

int main() {
    std::printf("mayag font engine\n=================");

    test_robustness();
    test_all_system_fonts();
    test_cmap();
    test_outlines();
    test_rasterization();
    test_sdf();
    test_atlas();
    test_shaping();
    test_fallback();
    test_line_breaking();
    test_metrics();
    test_rendering();
    test_variations();
    test_optical_size();
    test_caret();
    test_italic();
    test_synthetic_italic();
    test_synthetic_bold();
    test_ellipsis();
    test_justify();
    test_ligatures();
    test_color_glyphs();

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
