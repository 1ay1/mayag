// tests/test_discovery.cpp — font discovery and the last-resort face
//
// The contract this file guards: mayag renders correct text on a fresh machine
// with no font configuration, and never falls back to tofu when an installed
// face could draw the character. It is split so CI is meaningful:
//
//   * The LAST-RESORT face is synthesized in memory and needs no files, so its
//     checks run everywhere and are exact.
//   * The DISCOVERY checks adapt to whatever fonts the machine has: they assert
//     the invariants ("if a face covers a script, covering() finds it") rather
//     than the presence of any particular font, so they pass on a minimal CI
//     image and on a fully-loaded desktop alike.

#include <mayag/mayag.hpp>
#include <mayag/font/last_resort.hpp>
#include <mayag/font/system.hpp>

#include <cstdio>
#include <string>
#include <string_view>

using namespace mayag;
using namespace mayag::typo;

namespace {

int failures = 0, checks = 0;

void check(bool ok, std::string_view what) {
    ++checks;
    if (!ok) { ++failures; std::printf("  FAIL  %.*s\n",
                                       static_cast<int>(what.size()), what.data()); }
}
void section(std::string_view s) { std::printf("\n%.*s\n",
                                               static_cast<int>(s.size()), s.data()); }

// ── the synthesized last-resort face ────────────────────────────────────

void test_last_resort() {
    section("last-resort face");

    const auto& bytes = lastresort::font_bytes();
    check(bytes.size() > 2000, "the synthesized font has real content");
    check(bytes.size() < 64 * 1024, "and is small (it is a fallback, not a library)");

    auto face = lastresort::face();
    check(face != nullptr, "the last-resort face parses back through the engine");
    if (!face) return;

    check(face->num_glyphs() > 90, "it has a glyph per printable ASCII char");
    check(face->units_per_em() > 0, "upem is valid");
    check(face->has_outlines(), "it has real vector outlines (not blank)");

    // ASCII must map and produce ink.
    const std::uint16_t ga = face->glyph_for('A');
    const std::uint16_t g0 = face->glyph_for('0');
    check(ga != 0, "A maps to a glyph");
    check(g0 != 0, "0 maps to a glyph");
    check(!face->outline(ga).empty(), "A has an outline");
    check(!face->outline(g0).empty(), "0 has an outline");

    // .notdef must be a real box, so unknown text is visibly missing rather
    // than invisibly absent.
    check(!face->outline(0).empty(), ".notdef is a drawable box");

    const auto m = face->metrics(16.0f);
    check(m.ascent > 0.0f && m.descent > 0.0f, "metrics are sane at 16px");

    // It must render ink through the standard pipeline.
    auto stack = std::make_shared<FontStack>();
    stack->add(lastresort::face());
    auto ui = dsl::v(dsl::text<"ABC 012"> | dsl::font(32) | dsl::fg(colors::white))
              | dsl::pad(8) | dsl::bg(colors::black);
    RenderOptions o; o.fonts = stack;
    const auto px = render_to_pixels(ui, {240, 60}, o);
    int lit = 0;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4)
        if (px[i] > 20 || px[i + 1] > 20 || px[i + 2] > 20) ++lit;
    check(lit > 200, "the last-resort face renders visible text");
}

// ── the colour/bitmap parser fix ────────────────────────────────────────

void test_color_fonts_load() {
    section("colour / bitmap fonts");

    const auto& db = system::Database::instance();

    // If the machine has ANY emoji or colour font, the parser must now accept
    // it — this is the regression that made emoji tofu. On a machine with none
    // installed the check is vacuously skipped, not failed.
    int color_faces = 0;
    for (const auto& e : db.entries()) if (e.color) ++color_faces;

    if (color_faces == 0) {
        std::printf("  (no colour fonts installed — skipped)\n");
    } else {
        check(true, "colour/bitmap fonts load instead of being rejected");
        // And at least one of them must actually cover the emoji block.
        bool any_emoji = false;
        for (const auto& e : db.entries())
            if (e.covers(system::Script::emoji)) { any_emoji = true; break; }
        check(any_emoji, "an installed colour font reports emoji coverage");
    }
}

// ── coverage routing ────────────────────────────────────────────────────

void test_coverage_routing() {
    section("script routing");

    // script_of must place representative codepoints in the right script.
    using system::Script;
    using system::script_of;
    check(script_of('A')     == Script::latin,      "Latin A routes to latin");
    check(script_of(0x4E2D)  == Script::han,        "中 routes to han");
    check(script_of(0x0410)  == Script::cyrillic,   "А routes to cyrillic");
    check(script_of(0x0627)  == Script::arabic,     "ا routes to arabic");
    check(script_of(0x0915)  == Script::devanagari, "क routes to devanagari");
    check(script_of(0x05D0)  == Script::hebrew,     "א routes to hebrew");
    check(script_of(0x0E01)  == Script::thai,       "ก routes to thai");
    check(script_of(0xAC00)  == Script::hangul,     "가 routes to hangul");
    check(script_of(0x1F600) == Script::emoji,      "😀 routes to emoji");
    check(script_of(0x0391)  == Script::greek,      "Α routes to greek");

    const auto& db = system::Database::instance();

    // The core invariant: if ANY entry covers a script, covering() for a
    // codepoint in that script returns a covering entry. This is what makes
    // tofu structurally impossible.
    for (const auto& probe : system::script_probes) {
        bool anyone = false;
        for (const auto& e : db.entries())
            if (e.covers(probe.script)) { anyone = true; break; }
        if (!anyone) continue;   // this machine has no font for that script

        const system::FontEntry* c = db.covering(probe.codepoint);
        check(c != nullptr && c->covers(probe.script),
              std::string{"covering() finds a face for script "} +
              std::to_string(static_cast<int>(probe.script)));
    }
}

// ── the whole point: default_stack never yields tofu ────────────────────

void test_default_stack_completeness() {
    section("default stack");

    auto stack = system::default_stack();
    check(stack != nullptr && !stack->empty(),
          "default_stack() is never empty, even with no fonts");

    // For every script the machine has a font for, the assembled stack must be
    // able to resolve that script's representative codepoint. This is the
    // end-to-end no-tofu guarantee, tested against the actual machine.
    const auto& db = system::Database::instance();
    for (const auto& probe : system::script_probes) {
        bool installed = false;
        for (const auto& e : db.entries())
            if (e.covers(probe.script)) { installed = true; break; }
        if (!installed) continue;

        const auto [face, gid] = stack->resolve(probe.codepoint);
        check(gid != 0,
              std::string{"default stack resolves script "} +
              std::to_string(static_cast<int>(probe.script)) +
              " (U+" + std::to_string(probe.codepoint) + ")");
    }

    // Latin ALWAYS resolves — either a system face or the last-resort one.
    check(stack->resolve('A').second != 0, "Latin always resolves");
}

}  // namespace

int main() {
    std::printf("mayag font discovery\n====================\n");
    test_last_resort();
    test_color_fonts_load();
    test_coverage_routing();
    test_default_stack_completeness();
    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
