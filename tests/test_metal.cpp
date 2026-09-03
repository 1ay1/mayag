// tests/test_metal.cpp — does the GPU draw the same picture as the CPU?
//
// The Metal backend's headline number is that it costs 0.04 ms of CPU where
// the software rasteriser costs 4.88 ms. That is a 122x claim, and it has an
// obvious failure mode: a backend that renders NOTHING is also very fast.
// Timing alone cannot tell a working GPU path from a broken one.
//
// So this test renders the same draw list through both backends and compares
// the pixels. It is the check that turns the benchmark from an assertion into
// evidence, and it is the one that will catch the regression when somebody
// changes the instance layout, the blend state, or the colour space.
//
// Exact equality is the WRONG bar and asserting it would make this test
// useless: the two rasterisers legitimately differ. The GPU computes coverage
// from hardware derivatives and the CPU from an analytic footprint, so edge
// pixels land a value or two apart. What must match is the geometry — where
// shapes are, how big they are, what colour their interiors are, and that
// clipping happened. Those are checked exactly; edges are given tolerance.

#include <mayag/mayag.hpp>

#if defined(MAYAG_WITH_METAL)
#include <mayag/backend/metal.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace mayag;

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void section(const char* name) { std::printf("\n%s\n", name); }

/// A scene with everything that can silently break: a clip (proves scissor
/// rects work), a rounded rect (proves the SDF kernel runs), a circle (proves
/// `kind` dispatch works), and flat colour fills (proves the colour space
/// round-trip is right).
DrawList build_scene(float w, float h) {
    DrawList dl;
    dl.fill_rect(Rect{0, 0, w, h}, rgb<0x101418>);
    dl.fill_rect(Rect{40, 40, 160, 100}, rgb<0x3B82F6>, Corners{16, 16, 16, 16});
    dl.circle(Vec2{300, 90}, 40.0f, rgb<0xEF4444>);

    // A shape that EXTENDS past its clip. If the backend ignores the batch's
    // scissor rect, the overflow shows up outside the box and the test fails
    // — which is exactly the bug the first version of `submit` had.
    dl.push_clip(Rect{40, 180, 120, 80});
    dl.fill_rect(Rect{40, 180, 400, 80}, rgb<0x22C55E>);
    dl.pop_clip();
    return dl;
}

struct Rgba { int r, g, b, a; };

Rgba pixel_at(const std::vector<std::uint8_t>& px, int w, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
    if (i + 3 >= px.size()) return {-1, -1, -1, -1};
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

bool near(const Rgba& a, const Rgba& b, int tol) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol &&
           std::abs(a.b - b.b) <= tol && std::abs(a.a - b.a) <= tol;
}

}  // namespace

int main() {
    std::printf("mayag Metal backend test\n========================");

#if !defined(MAYAG_WITH_METAL)
    std::printf("\n\nSKIP  built without MAYAG_WITH_METAL\n");
    return 0;
#else
    constexpr int w = 400, h = 300;

    backend::MetalDevice gpu;
    if (!gpu.init_offscreen()) {
        // No GPU here (a VM, a CI box with no Metal). Skipping is correct:
        // failing would make the suite red for a machine limitation rather
        // than a mayag bug.
        std::printf("\n\nSKIP  no Metal device available on this machine\n");
        return 0;
    }
    std::printf("\ndevice: %s\n", gpu.adapter_name().c_str());

    const DrawList scene = build_scene(w, h);
    const Color<Srgb> clear = rgb<0x101418>;

    // ---- render both ways --------------------------------------------
    const auto gpu_px = gpu.render_offscreen(scene, clear, w, h);

    backend::Framebuffer fb{w, h};
    backend::Tiled::render(scene, fb, nullptr, &backend::shared_pool(), clear);
    const auto cpu_px = fb.to_rgba8();

    section("the GPU produced a frame at all");
    check(gpu_px.size() == static_cast<std::size_t>(w) * h * 4,
          "readback is the expected size");
    check(!gpu_px.empty(), "readback is not empty");

    // The cheapest way to catch "renders nothing": a frame that is entirely
    // one colour drew no shapes, however fast it was.
    {
        bool uniform = true;
        const Rgba first = pixel_at(gpu_px, w, 0, 0);
        for (int y = 0; y < h && uniform; y += 7) {
            for (int x = 0; x < w; x += 7) {
                if (!near(pixel_at(gpu_px, w, x, y), first, 0)) { uniform = false; break; }
            }
        }
        check(!uniform, "the frame is NOT a single flat colour (shapes were drawn)");
    }

    section("shape interiors match the software rasteriser");
    // Sample well inside each shape, where antialiasing cannot reach, so any
    // difference is a real one: wrong colour space, wrong blend, wrong kind.
    struct Probe { int x, y; const char* what; };
    const Probe probes[] = {
        {120,  90, "rounded rect interior"},
        {300,  90, "circle centre"},
        { 80, 220, "clipped rect, inside the clip"},
        {370,  30, "background, top right"},
        { 20, 280, "background, bottom left"},
    };

    for (const auto& p : probes) {
        const Rgba g = pixel_at(gpu_px, w, p.x, p.y);
        const Rgba c = pixel_at(cpu_px, w, p.x, p.y);
        const bool ok = near(g, c, 2);
        if (!ok) {
            std::printf("       gpu(%d,%d,%d,%d) vs cpu(%d,%d,%d,%d) at %s\n",
                        g.r, g.g, g.b, g.a, c.r, c.g, c.b, c.a, p.what);
        }
        check(ok, p.what);
    }

    section("clipping is honoured");
    // The green rect was pushed 400px wide inside a 120px clip. Past the clip
    // edge the pixel must still be background. This is the regression test
    // for a `submit` that ignores per-batch scissor rects.
    {
        const Rgba outside = pixel_at(gpu_px, w, 250, 220);
        const Rgba bg      = pixel_at(gpu_px, w, 370, 30);
        check(near(outside, bg, 2),
              "content past the clip edge did NOT paint");

        const Rgba inside = pixel_at(gpu_px, w, 80, 220);
        check(!near(inside, bg, 8),
              "content inside the clip DID paint");
    }

    section("whole-frame agreement");
    // A global measure, so a difference the fixed probes miss still shows up.
    // Edge pixels are expected to differ slightly; large areas must not.
    {
        std::size_t differing = 0, total = 0;
        double sum_err = 0.0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const Rgba g = pixel_at(gpu_px, w, x, y);
                const Rgba c = pixel_at(cpu_px, w, x, y);
                const int err = std::max({std::abs(g.r - c.r), std::abs(g.g - c.g),
                                          std::abs(g.b - c.b)});
                sum_err += err;
                ++total;
                if (err > 8) ++differing;
            }
        }
        const double pct = 100.0 * static_cast<double>(differing) /
                           static_cast<double>(total);
        const double mean_err = sum_err / static_cast<double>(total);
        std::printf("       %.2f%% of pixels differ by more than 8/255"
                    " (mean error %.2f)\n", pct, mean_err);

        // Antialiased edges are a small fraction of any UI frame. If more
        // than a few percent of pixels disagree, the backends have genuinely
        // diverged rather than merely rounding differently.
        check(pct < 4.0, "fewer than 4% of pixels differ materially");
        check(mean_err < 3.0, "mean per-pixel error is under 3/255");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
#endif
}
