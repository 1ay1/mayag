#pragma once
// examples/harness.hpp — one main() for every example
//
// Every mayag example should be three things at once:
//
//   * a LIVE APP        `./mayag_gallery`              opens a window
//   * a SCREENSHOT      `./mayag_gallery --png out.png` renders and exits
//   * a CI TEST         `./mayag_gallery --headless`    runs, asserts, exits
//
// Those are the same Program driven by three different platforms, so an
// example cannot drift from what it documents: if the screenshot in the
// README is right, the windowed app is right, because they ran the same code.
//
// This header holds the argument parsing and font setup so the examples
// themselves stay pure UI.

#include <mayag/mayag.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace demo {

using namespace mayag;

struct Options {
    enum class Mode { window, png, headless } mode = Mode::window;
    std::string out_path = "out.png";
    Vec2        size{1000, 720};
    float       dpi = 2.0f;
    bool        use_system_fonts = true;
    bool        debug_bounds = false;
};

[[nodiscard]] inline Options parse_args(int argc, char** argv, Vec2 default_size) {
    Options o;
    o.size = default_size;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];

        if (a == "--png") {
            o.mode = Options::Mode::png;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.out_path = argv[++i];
        }
        else if (a == "--headless") { o.mode = Options::Mode::headless; }
        else if (a == "--window")   { o.mode = Options::Mode::window; }
        else if (a == "--stroke-font") { o.use_system_fonts = false; }
        else if (a == "--debug")    { o.debug_bounds = true; }
        else if (a == "--dpi" && i + 1 < argc) { o.dpi = std::strtof(argv[++i], nullptr); }
        else if (a == "--size" && i + 2 < argc) {
            o.size.x = std::strtof(argv[++i], nullptr);
            o.size.y = std::strtof(argv[++i], nullptr);
        }
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: %s [options]\n"
                "  (no args)         open a window\n"
                "  --png [PATH]      render one frame to a PNG and exit\n"
                "  --headless        run scripted, assert, and exit (CI)\n"
                "  --size W H        viewport size in logical pixels\n"
                "  --dpi SCALE       device pixel ratio (default 2.0)\n"
                "  --stroke-font     use the built-in font, not system fonts\n"
                "  --debug           overlay every node's layout rect\n",
                argv[0]);
            std::exit(0);
        }
    }
    return o;
}

/// The font stack an example should use.
///
/// Returns null when `--stroke-font` was passed or no system font was found;
/// callers pass that straight through to AppConfig, which falls back to the
/// built-in vector font. That is why an example still renders on a machine
/// with no fonts installed at all.
[[nodiscard]] inline std::shared_ptr<typo::FontStack> make_fonts(const Options& o) {
    if (!o.use_system_fonts) return nullptr;

    auto stack = typo::system::default_stack(typo::FontConfig{
        .mode = typo::RenderMode::hybrid,
        .sdf_threshold = 26.0f,
        .atlas_size = 1024,
    });
    if (stack->empty()) return nullptr;
    return stack;
}

inline void print_fonts(const typo::FontStack* fonts) {
    if (fonts == nullptr) {
        std::printf("fonts: built-in stroke font\n");
        return;
    }
    std::printf("fonts:\n");
    for (std::size_t i = 0; i < fonts->face_count(); ++i) {
        const auto* f = fonts->face_at(i);
        std::printf("  [%zu] %-26s %-10s %5u glyphs  %s%s\n",
                    i, f->family().c_str(), f->subfamily().c_str(), f->num_glyphs(),
                    f->file().is_cff() ? "CFF" : "TrueType",
                    f->has_kerning() ? " +kern" : "");
    }
}

/// Report what a frame cost, so every example doubles as a benchmark.
template <typename Ui>
inline void report(const Ui& ui, Vec2 viewport, const RenderOptions& opts,
                   const char* label) {
    Node root = [&] {
        if constexpr (std::same_as<std::remove_cvref_t<Ui>, Node>) return ui;
        else return ui.build();
    }();

    DrawList dl;
    render::PaintOptions po{};
    po.dpi_scale = opts.dpi_scale;

    if (opts.fonts != nullptr) {
        typo::StackMeasurer measurer{*opts.fonts};
        typo::StackGlyphRenderer glyphs{*opts.fonts};
        layout::layout_tree(root, viewport, measurer);
        po.measurer = &measurer;
        po.glyphs   = &glyphs;
        render::paint(root, dl, po);
    } else {
        const auto& f = strokefont::Font::builtin_font();
        layout::layout_tree(root, viewport, f.measurer());
        po.measurer = &f.measurer();
        po.glyphs   = &f.glyph_renderer();
        render::paint(root, dl, po);
    }

    std::printf("\n%s  %dx%d\n", label,
                static_cast<int>(viewport.x * opts.dpi_scale),
                static_cast<int>(viewport.y * opts.dpi_scale));
    std::printf("  nodes       %zu\n", root.count());
    std::printf("  instances   %zu\n", dl.size());
    std::printf("  draw calls  %zu\n", dl.batches().size());
    if (opts.fonts != nullptr) {
        std::printf("  atlas       %d glyphs, %.1f%% of %dx%d\n",
                    static_cast<int>(opts.fonts->atlas().glyph_count()),
                    static_cast<double>(opts.fonts->atlas().occupancy() * 100.0f),
                    opts.fonts->atlas().width(), opts.fonts->atlas().height());
    }
}

/// Run a Program in whichever mode the flags asked for.
///
/// `drive` is the headless script: it receives the runtime and should exercise
/// the app and assert on it. In window and png modes it is never called.
template <Program P, typename Driver>
[[nodiscard]] int run(const Options& o, AppConfig cfg, Driver&& drive) {
    switch (o.mode) {
        case Options::Mode::window:
            return mayag::run<P>(std::move(cfg));

        case Options::Mode::headless:
            return run_headless<P>(std::move(cfg), std::forward<Driver>(drive));

        case Options::Mode::png: {
            // Build the initial view and render one frame. Uses the Program's
            // own init() and view(), so the screenshot is exactly frame zero
            // of the live app rather than a separate code path.
            typename P::Model model = [] {
                if constexpr (detail::HasEffectfulInit<P>) return P::init().first;
                else return P::init();
            }();

            Ctx ctx{o.size, o.dpi, 0.0, cfg.theme, nullptr};
            Node root = [&] {
                if constexpr (detail::ViewWithCtx<P>) return P::view(model, ctx);
                else return P::view(model);
            }();

            RenderOptions ro;
            ro.background   = cfg.theme.background;
            ro.dpi_scale    = o.dpi;
            ro.fonts        = cfg.fonts;
            ro.font         = cfg.font;
            ro.debug_bounds = cfg.debug_bounds;

            if (!render_to_png(root, o.size, o.out_path, ro)) {
                std::fprintf(stderr, "mayag: failed to write %s\n", o.out_path.c_str());
                return 1;
            }
            report(root, o.size, ro, o.out_path.c_str());
            return 0;
        }
    }
    return 0;
}

/// Overload for examples with nothing to script headlessly.
template <Program P>
[[nodiscard]] int run(const Options& o, AppConfig cfg) {
    return run<P>(o, std::move(cfg), [](auto& rt) {
        // Default script: boot, pump a few frames, confirm it drew something.
        for (int i = 0; i < 3; ++i) rt.tick();
        std::printf("headless: %llu frames presented\n",
                    static_cast<unsigned long long>(rt.window().frames_presented()));
    });
}

}  // namespace demo
