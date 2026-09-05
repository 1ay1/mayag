// examples/live_capture.cpp — capture what the WINDOW actually presents.
//
// Everything else in this repo verifies rendering offscreen. That is a
// different pipeline from the one a user looks at: different image format,
// different render pass, no swapchain, no compositor. So a bug that lives in
// presentation is invisible to every screenshot, every golden test and every
// --gpu render -- and one shipped exactly that way.
//
// This opens a real window, lets it present, copies the PRESENTED swapchain
// image back, renders the same frame offscreen, and diffs them. If those two
// disagree, the live path is lying and this prints where.
//
// Build:  cmake --build build && ./build/examples/mayag_live_capture
//   --out PATH     write the live capture (default /tmp/live.png)
//   --ref PATH     write the offscreen reference (default /tmp/ref.png)
//   --frames N     present N frames before capturing (default 30)

#include <mayag/mayag.hpp>

#include "harness.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

// A scene with the features most likely to differ between paths: a gradient
// wash, translucency, a blurred glow, text, and hard-edged shapes.
struct Probe {
    struct Model { float t = 0.0f; };
    struct Tick { float dt; };
    struct Quit {};
    using Msg = std::variant<Tick, Quit>;

    static std::pair<Model, Cmd<Msg>> init() { return {Model{}, Cmd<Msg>::none()}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Tick t) -> R { m.t += t.dt; return {m, Cmd<Msg>::none()}; },
            [&](Quit)   -> R { return {m, Cmd<Msg>::quit()}; });
    }

    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = c.theme;
        const auto teal = rgb<0x17A2A2>;
        const auto warm = rgb<0xE0653C>;
        const float x = 60.0f + 120.0f * (0.5f + 0.5f * std::sin(m.t * 2.0f));

        return (v(
            box() | width(pct(100)) | height(px(120))
                  | linear_gradient(teal, warm, {0, 0}, {1, 0}),
            h(box() | width(px(70)) | height(px(70)) | radius(35) | bg(teal),
              box() | width(px(70)) | height(px(70)) | radius(35)
                    | bg(warm.fade(0.55f)),
              box() | width(px(70)) | height(px(70)) | radius(12)
                    | bg(t.surface) | border(2, teal)) | gap(12),
            text<"live vs offscreen"> | font(18) | semibold | fg(t.text_primary),
            text<"if these differ, presentation is lying"> | font(12)
                | fg(t.text_secondary),
            box() | width(px(40)) | height(px(40)) | radius(20)
                  | bg(colors::white) | offset(x, 0.0f))
            | gap(14) | pad(20) | width(pct(100)) | height(pct(100))
            | bg(t.background)).build();
    }

    static Sub<Msg> subscribe(const Model&) {
        return Sub<Msg>::batch({
            Sub<Msg>::on_frame([](float dt) { return Tick{dt}; }),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_close(Quit{})});
    }
};

namespace {

// Mean absolute difference and the worst offending region, so a failure says
// WHERE rather than just "they differ".
void report(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
            int w, int h) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n == 0) { std::printf("  (nothing to compare)\n"); return; }

    double sum = 0.0;
    int    worst = 0, wx = 0, wy = 0;
    long   over8 = 0, px = 0;
    for (std::size_t i = 0; i + 3 < n; i += 4) {
        int d = 0;
        for (int c = 0; c < 3; ++c) {
            d = std::max(d, std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])));
        }
        sum += d;
        ++px;
        if (d > 8) ++over8;
        if (d > worst) {
            worst = d;
            const std::size_t p = i / 4;
            wx = static_cast<int>(p % static_cast<std::size_t>(w));
            wy = static_cast<int>(p / static_cast<std::size_t>(w));
        }
    }
    const double mean = px ? sum / static_cast<double>(px) : 0.0;
    std::printf("  mean|d| = %.3f/255   worst = %d at (%d,%d)   >8/255 = %.2f%%\n",
                mean, worst, wx, wy,
                px ? 100.0 * static_cast<double>(over8) / static_cast<double>(px) : 0.0);

    if (worst > 24) {
        const std::size_t i = (static_cast<std::size_t>(wy) * w + wx) * 4;
        std::printf("    live      = (%d,%d,%d)\n", a[i], a[i + 1], a[i + 2]);
        std::printf("    offscreen = (%d,%d,%d)\n", b[i], b[i + 1], b[i + 2]);
        std::printf("\n  VERDICT: the window is NOT showing what the renderer produced.\n");
    } else {
        std::printf("\n  VERDICT: live and offscreen agree.\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string out = "/tmp/live.png", ref = "/tmp/ref.png";
    int frames = 30;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--ref" && i + 1 < argc) ref = argv[++i];
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    }

    AppConfig cfg;
    cfg.title = "mayag — live capture";
    cfg.size  = {520, 400};

    std::printf("opening a real window, presenting %d frames...\n", frames);

    int captured_w = 0, captured_h = 0;
    std::vector<std::uint8_t> live;

    // Drive the REAL window, not the headless one. This is the whole point:
    // platform::Native is the path a user runs, and the only one that can
    // exhibit a presentation bug.
    Runtime<Probe, platform::Native> rt{cfg};
    if (!rt.boot()) {
        std::printf("could not open a window (no compositor?)\n");
        return 1;
    }
    for (int i = 0; i < frames && !rt.finished(); ++i) rt.tick();

    live = rt.window().read_pixels();

    // Ask the WINDOW for the shape of what it gave us, rather than computing
    // logical*dpi and hoping. The compositor owns the surface size.
    const Vec2 pix = rt.window().pixel_size();
    captured_w = static_cast<int>(pix.x + 0.5f);
    captured_h = static_cast<int>(pix.y + 0.5f);
    const float dpi = cfg.size.x > 0.0f ? pix.x / cfg.size.x : 1.0f;

    if (live.size() != static_cast<std::size_t>(captured_w) * captured_h * 4) {
        std::printf("capture is %zu bytes, which is not %dx%d RGBA -- "
                    "refusing to guess its shape\n",
                    live.size(), captured_w, captured_h);
        return 1;
    }

    if (live.empty()) {
        std::printf("no live pixels captured (no GPU window?)\n");
        return 1;
    }
    (void)image::write_png(out, live, captured_w, captured_h);
    std::printf("wrote %s  (%dx%d)\n", out.c_str(), captured_w, captured_h);

    // The same frame, offscreen, at the SAME physical resolution.
    Probe::Model m{};
    Ctx c;
    c.size = cfg.size;
    c.dpi_scale = dpi;
    c.theme = cfg.theme;
    Node n = Probe::view(m, c);

    RenderOptions ro;
    ro.background = cfg.theme.background;
    ro.dpi_scale  = dpi;
    const auto off = render_to_pixels(n, cfg.size, ro);
    (void)image::write_png(ref, off, captured_w, captured_h);
    std::printf("wrote %s  (offscreen reference)\n\n", ref.c_str());

    std::printf("live capture vs offscreen render:\n");
    report(live, off, captured_w, captured_h);
    return 0;
}
