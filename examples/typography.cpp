// examples/typography.cpp — the font engine, rendering real typefaces
//
// Everything here comes from fonts already installed on the machine. mayag
// parses the TTF/OTF itself, shapes with real kerning, rasterises with
// analytic coverage, packs into an SDF atlas, and draws the whole page in a
// handful of instanced quads.
//
// Build:  cmake --build build && ./build/examples/mayag_typography

#include "harness.hpp"

using namespace mayag;
using namespace mayag::dsl;

/// A specimen page. No state beyond the theme, so `update` is trivial and the
/// interest is entirely in what the font engine renders.
struct Typography {
    struct Model { int theme_index = 0; };
    struct NextTheme {}; struct Quit {};
    using Msg = std::variant<NextTheme, Quit>;

    static constexpr std::array<Theme, 4> palette{
        themes::midnight, themes::ember, themes::daylight, themes::paper};

    static std::pair<Model, Cmd<Msg>> init() {
        return {Model{}, Cmd<Msg>::set_title("mayag — typography")};
    }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Quit>(msg)) return {m, Cmd<Msg>::quit()};
        m.theme_index = (m.theme_index + 1) % static_cast<int>(palette.size());
        return {m, Cmd<Msg>::none()};
    }

    static Sub<Msg> subscribe(const Model&) {
        return Sub<Msg>::batch(
            Sub<Msg>::on_key(Key::space, NextTheme{}),
            Sub<Msg>::on_click<"cycle">(NextTheme{}),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_close(Quit{}));
    }

    static Node view(const Model& model, const Ctx& ctx) {
    const Theme& t = palette[static_cast<std::size_t>(model.theme_index)];

    // ── specimen rows ───────────────────────────────────────────────────

    auto row = [&](auto label, auto sample, float size, FontWeight w) {
        return h(text_of(label) | font(10) | fg(t.text_disabled) | width(52),
                 text_of(sample) | font(size) | weight(w) | fg(t.text_primary))
             | gap(14) | align(Align::baseline);
    };

    auto scale_section =
        v(text<"TYPE SCALE"> | font(10) | semibold | fg(t.text_secondary) | tracking(2.0f),
          row("48", "Typography", 48.0f, FontWeight::bold),
          row("32", "Typography", 32.0f, FontWeight::semi_bold),
          row("24", "Typography", 24.0f, FontWeight::medium),
          row("17", "Typography", 17.0f, FontWeight::regular),
          row("13", "Typography", 13.0f, FontWeight::regular),
          row("11", "Typography", 11.0f, FontWeight::regular))
        | gap(10);

    // Kerning is the difference between typeset text and a character grid.
    // These pairs are the classic offenders.
    auto kerning_section =
        v(text<"KERNING"> | font(10) | semibold | fg(t.text_secondary) | tracking(2.0f),
          h(text<"AVATAR"> | font(30) | fg(t.text_primary),
            text<"To Wave"> | font(30) | fg(t.accent),
            text<"P.J. Yo"> | font(30) | fg(t.success)) | gap(24))
        | gap(10);

    // Per-codepoint fallback: three faces, one string, one atlas, one draw.
    auto scripts_section =
        v(text<"SCRIPT FALLBACK"> | font(10) | semibold | fg(t.text_secondary) | tracking(2.0f),
          text_of("Latin  ABCdefg 0123") | font(20) | fg(t.text_primary),
          text_of("Accents  café naïve Ærø") | font(20) | fg(t.text_primary),
          text_of("Cyrillic  Здравствуйте") | font(20) | fg(t.text_primary),
          text_of("Greek  Ελληνικά") | font(20) | fg(t.text_primary),
          text_of("CJK  日本語 中文 한국어") | font(20) | fg(t.text_primary),
          text_of("Symbols  ← → ↑ ↓ ∑ ∫ √ ≈ ∞") | font(20) | fg(t.text_primary))
        | gap(7);

    auto weights_section =
        v(text<"WEIGHT + STYLE"> | font(10) | semibold | fg(t.text_secondary) | tracking(2.0f),
          h(text<"Light"> | font(22) | weight(FontWeight::light) | fg(t.text_primary),
            text<"Regular"> | font(22) | fg(t.text_primary),
            text<"Medium"> | font(22) | weight(FontWeight::medium) | fg(t.text_primary),
            text<"Bold"> | font(22) | bold | fg(t.text_primary)) | gap(18),
          h(text<"Underline"> | font(18) | underline | fg(t.accent),
            text<"Strikethrough"> | font(18) | strikethrough | fg(t.text_secondary),
            text<"Tracked out"> | font(18) | tracking(3.0f) | fg(t.warning)) | gap(18))
        | gap(10);

    // Wrapping honours real break opportunities, not just spaces.
    auto paragraph_section =
        v(text<"WRAPPING + ALIGNMENT"> | font(10) | semibold | fg(t.text_secondary) | tracking(2.0f),
          h(v(text_of("Every glyph on this page was parsed out of a TrueType or "
                      "OpenType file by mayag itself, shaped with kerning from the "
                      "font's own GPOS table, and rasterised with exact analytic "
                      "coverage.")
                | font(13) | fg(t.text_secondary) | line_height(1.5f) | wrap_text)
              | width(pct(48)),
            v(text_of("Text at or above the SDF threshold shares one scale-free "
                      "atlas entry across every size it appears at, so a heading "
                      "and a caption cost one glyph between them.")
                | font(13) | fg(t.text_secondary) | line_height(1.5f)
                | text_align(TextAlign::right))
              | width(pct(48)))
          | gap(16) | justify(Justify::space_between))
        | gap(10);

    auto page =
        v(v(text<"mayag"> | font(40) | bold | fg(t.text_primary) | tracking(-1.5f),
            text<"a from-scratch OpenType engine — no FreeType, no HarfBuzz, no deps">
              | font(12) | fg(t.text_secondary)) | gap(3),
          h(text<"cycle theme"> | font(11) | semibold | fg(t.on_accent))
            | center | pad(7, 14) | bg(t.accent) | radius(t.radius_small)
            | dsl::id<"cycle">,
          divider(t),
          scale_section,
          divider(t),
          kerning_section,
          divider(t),
          scripts_section,
          divider(t),
          weights_section,
          divider(t),
          paragraph_section)
        | gap(20) | pad(36)
        | bg(t.background);

    return (page | width(pct(100)) | height(pct(100))).build();
    }
};

static_assert(Program<Typography>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{980, 900});
    auto fonts = demo::make_fonts(opts);
    demo::print_fonts(fonts.get());

    AppConfig cfg{
        .title = "mayag — typography",
        .size  = opts.size,
        .theme = Typography::palette[0],
        .fonts = fonts.get(),
        .debug_bounds = opts.debug_bounds,
    };

    return demo::run<Typography>(opts, cfg, [](auto& rt) {
        std::printf("typography headless\n");
        int fails = 0;
        const auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            if (!c) ++fails;
        };
        ok(rt.window().frames_presented() >= 1, "specimen rendered");
        rt.click("cycle");
        ok(rt.model().theme_index == 1, "clicking cycles the theme");
        rt.window().press_key(Key::space);
        rt.tick();
        ok(rt.model().theme_index == 2, "space cycles the theme");
        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
