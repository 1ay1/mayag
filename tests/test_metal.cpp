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

    // ── text ────────────────────────────────────────────────────────
    //
    // Every example is mostly text, and text is the ONE primitive that needs
    // state the GPU does not get for free: the glyph atlas has to be
    // uploaded to a texture and sampled, where the CPU path just calls back
    // into the rasteriser. So a GPU backend can be perfect on boxes and
    // circles and still render every app blank. That failure would be
    // invisible to the shape checks above, which is exactly why it gets its
    // own section.
    section("text renders through the atlas texture");
    {
        auto fonts = typo::system::default_stack(typo::FontConfig{
            .mode = typo::RenderMode::hybrid,
            .sdf_threshold = 26.0f,
            .atlas_size = 1024,
        });

        if (fonts == nullptr || fonts->empty()) {
            std::printf("       (no system fonts; skipping)\n");
        } else {
            typo::StackGlyphRenderer glyphs{*fonts};
            typo::StackSampler       cpu_sampler{*fonts};

            DrawList tdl;
            TextStyle ts{};
            ts.size = 48.0f;
            ts.color = rgb<0xFFFFFF>;
            glyphs.draw_text(tdl, "Hamburgefonstiv", Rect{20, 100, 360, 60}, ts);

            check(tdl.size() > 0, "the text produced glyph instances");

            // Upload whatever the shaping just rasterised, then render.
            gpu.sync_atlas(fonts->atlas());
            const auto tg = gpu.render_offscreen(tdl, clear, w, h);

            backend::Framebuffer tfb{w, h};
            backend::Tiled::render(tdl, tfb, &cpu_sampler, &backend::shared_pool(), clear);
            const auto tc = tfb.to_rgba8();

            // "Did text draw at all" is the question that matters most: a
            // missing atlas upload gives a perfectly clean, perfectly empty
            // frame. Count lit pixels on both sides and compare.
            const auto lit = [&](const std::vector<std::uint8_t>& px) {
                std::size_t n = 0;
                for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
                    if (px[i] > 90 && px[i + 1] > 90 && px[i + 2] > 90) ++n;
                }
                return n;
            };
            const std::size_t gpu_lit = lit(tg), cpu_lit = lit(tc);
            std::printf("       lit pixels: gpu %zu, cpu %zu\n", gpu_lit, cpu_lit);

            check(gpu_lit > 500, "the GPU drew visible glyph coverage");

            // Both rasterise the same glyphs from the same atlas, so the
            // amount of ink must agree closely. A large gap means the
            // texture, the uv rects, or the SDF threshold is wrong.
            if (cpu_lit > 0) {
                const double ratio = static_cast<double>(gpu_lit) /
                                     static_cast<double>(cpu_lit);
                std::printf("       gpu/cpu ink ratio %.3f\n", ratio);
                check(ratio > 0.75 && ratio < 1.35,
                      "GPU and CPU agree on how much ink the text has");
            }
        }
    }

    // ── the rest of the primitive set ────────────────────────────────
    //
    // Boxes, circles and text cover most of a UI but not the parts most
    // likely to diverge. Gradients interpolate through Oklch in BOTH the
    // C++ kernel and the MSL one, and those are separate translations of the
    // same maths — a transcription slip shows up as a subtly wrong ramp that
    // no shape check would notice. Shadows grow their quad in the vertex
    // stage. Rings and strokes take different branches of the SDF.
    //
    // Each of these is a place the two backends could disagree silently, so
    // each gets rendered both ways and compared.
    section("gradients, shadows, rings and strokes agree");
    {
        struct Case {
            const char* name;
            void (*build)(DrawList&);
            int probe_x, probe_y;
        };

        const Case cases[] = {
            {"linear gradient (Oklch)", [](DrawList& d) {
                d.fill_rect(Rect{0, 0, 400, 300}, rgb<0x101418>);
                Fill f{};
                f.kind = FillKind::linear_gradient;
                f.stops[0] = GradientStop{0.0f, rgb<0xEF4444>};
                f.stops[1] = GradientStop{1.0f, rgb<0x3B82F6>};
                f.stop_count = 2;
                f.from = {0.0f, 0.0f};
                f.to   = {1.0f, 0.0f};
                d.fill_gradient(Rect{50, 50, 300, 200}, f);
            }, 200, 150},
            {"linear gradient (sRGB)", [](DrawList& d) {
                d.fill_rect(Rect{0, 0, 400, 300}, rgb<0x101418>);
                Fill f{};
                f.kind = FillKind::linear_gradient;
                f.stops[0] = GradientStop{0.0f, rgb<0xEF4444>};
                f.stops[1] = GradientStop{1.0f, rgb<0x3B82F6>};
                f.stop_count = 2;
                f.from = {0.0f, 0.0f};
                f.to   = {1.0f, 0.0f};
                f.interpolate_srgb = true;
                d.fill_gradient(Rect{50, 50, 300, 200}, f);
            }, 200, 150},
            {"radial gradient", [](DrawList& d) {
                d.fill_rect(Rect{0, 0, 400, 300}, rgb<0x101418>);
                Fill f{};
                f.kind = FillKind::radial_gradient;
                f.stops[0] = GradientStop{0.0f, rgb<0x22C55E>};
                f.stops[1] = GradientStop{1.0f, rgb<0x1E1B4B>};
                f.stop_count = 2;
                f.from = {0.5f, 0.5f};
                f.radius = 0.5f;
                d.fill_gradient(Rect{50, 50, 300, 200}, f);
            }, 200, 150},
            {"drop shadow", [](DrawList& d) {
                d.fill_rect(Rect{0, 0, 400, 300}, rgb<0x101418>);
                Shadow sh{};
                sh.offset = {0.0f, 0.0f};
                sh.blur   = 18.0f;
                sh.color  = Color<Srgb>{0.0f, 0.0f, 0.0f, 0.8f};
                d.shadow(Rect{140, 100, 120, 100}, sh, Corners{12, 12, 12, 12});
            }, 200, 150},
            {"ring", [](DrawList& d) {
                d.fill_rect(Rect{0, 0, 400, 300}, rgb<0x101418>);
                d.ring(Vec2{200, 150}, 60.0f, 12.0f, rgb<0x22C55E>);
            // Mid-BAND, not the edge: the ring spans y=89..96 at this x, and
            // sampling its boundary compares two different antialiasing
            // models rather than the shape they agree on. Verified by
            // scanning the column — both backends put the band in exactly the
            // same place with exactly the same colour, and differ by one
            // step on the final edge pixel.
            }, 200, 92},
            {"stroked rounded rect", [](DrawList& d) {
                d.fill_rect(Rect{0, 0, 400, 300}, rgb<0x101418>);
                d.stroke_rect(Rect{100, 80, 200, 140}, 6.0f, rgb<0xF59E0B>,
                              Corners{20, 20, 20, 20});
            }, 100, 150},
        };

        for (const auto& cs : cases) {
            DrawList d;
            cs.build(d);

            const auto g = gpu.render_offscreen(d, clear, w, h);
            backend::Framebuffer f{w, h};
            backend::Tiled::render(d, f, nullptr, &backend::shared_pool(), clear);
            const auto c = f.to_rgba8();

            const Rgba gp = pixel_at(g, w, cs.probe_x, cs.probe_y);
            const Rgba cp = pixel_at(c, w, cs.probe_x, cs.probe_y);

            // Gradients and shadows are smooth fields, so a couple of levels
            // of difference is honest rounding rather than a bug. What must
            // not happen is a different colour.
            const bool ok = near(gp, cp, 6);
            if (!ok) {
                std::printf("       gpu(%d,%d,%d) vs cpu(%d,%d,%d) at %s\n",
                            gp.r, gp.g, gp.b, cp.r, cp.g, cp.b, cs.name);
            }
            check(ok, cs.name);
        }
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
