// tests/test_mayag.cpp — mayag's test suite
//
// Three layers, matching the three ways this framework can be wrong:
//
//   1. NUMERIC   — colour conversions, SDFs, and math kernels, checked against
//                  known-good values. Most of these are already `static_assert`
//                  in the headers; these are the ones needing runtime tolerance.
//   2. LAYOUT    — flexbox arithmetic against hand-computed expected rects.
//   3. PIXEL     — render a scene and assert on actual sampled pixels. This is
//                  the layer that catches "compiles, lays out, draws nothing".

#include <mayag/mayag.hpp>
#include <mayag/backend/tiled.hpp>
#include <mayag/layout/audit.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int failures = 0;
int checks   = 0;

void check(bool cond, const std::string& what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void near(float got, float want, float eps, const std::string& what) {
    ++checks;
    if (num::abs(got - want) > eps) {
        ++failures;
        std::printf("  FAIL  %s: got %.4f want %.4f (eps %.4f)\n",
                    what.c_str(), got, want, eps);
    }
}

void section(const char* name) { std::printf("\n%s\n", name); }

// ── pixel helpers ───────────────────────────────────────────────────────

struct Image {
    std::vector<std::uint8_t> px;
    int w = 0, h = 0;

    [[nodiscard]] Color<Srgb> at(int x, int y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        return rgb8(px[i], px[i + 1], px[i + 2], px[i + 3]);
    }
};

template <typename Ui>
Image render(const Ui& ui, Vec2 size, const RenderOptions& o = {}) {
    Image img;
    img.px = render_to_pixels(ui, size, o);
    img.w  = static_cast<int>(size.x * o.dpi_scale);
    img.h  = static_cast<int>(size.y * o.dpi_scale);
    return img;
}

/// Perceptual distance — the right metric for "is this the colour I asked for".
float delta(Color<Srgb> a, Color<Srgb> b) {
    const auto x = a.to<Oklab>(), y = b.to<Oklab>();
    return Vec2{x.c0 - y.c0, x.c1 - y.c1}.length() + num::abs(x.c2 - y.c2);
}

void same_color(Color<Srgb> got, Color<Srgb> want, const std::string& what) {
    ++checks;
    const float d = delta(got, want);
    if (d > 0.05f) {
        ++failures;
        std::printf("  FAIL  %s: got (%.3f %.3f %.3f) want (%.3f %.3f %.3f) dE=%.3f\n",
                    what.c_str(), got.c0, got.c1, got.c2, want.c0, want.c1, want.c2, d);
    }
}

// ════════════════════════════════════════════════════════════════════════

void test_color() {
    section("color");

    // Round-trips through every space.
    constexpr auto c = rgb<0xE5484D>;
    same_color(c.to<Linear>().to<Srgb>(), c, "srgb -> linear -> srgb");
    same_color(c.to<Oklab>().to<Srgb>(),  c, "srgb -> oklab -> srgb");
    same_color(c.to<Oklch>().to<Srgb>(),  c, "srgb -> oklch -> srgb");

    // The transfer function must have the linear toe, not a naive 2.2 gamma.
    near(gray(0.5f).to<Linear>().c0, 0.2140f, 1e-3f, "sRGB 0.5 -> linear");
    near(gray(0.5f).to<Linear>().c0, num::pow(0.5f, 2.2f), 0.02f, "close to gamma 2.2");
    check(num::abs(gray(0.5f).to<Linear>().c0 - num::pow(0.5f, 2.2f)) > 1e-4f,
          "but NOT identical to gamma 2.2 (real sRGB curve in use)");

    // Perceptual mixing must beat naive linear mixing on chroma preservation.
    const auto naive  = mix(colors::red, colors::blue, 0.5f);
    const auto smart  = mix_perceptual(colors::red, colors::blue, 0.5f);
    check(smart.to<Oklch>().c1 > naive.to<Oklch>().c1,
          "oklch mix keeps more chroma than srgb mix");

    // Hue-shortest-path: red(29 deg) -> magenta(~350 deg) must not cross green.
    const auto a = Color<Oklch>{0.6f, 0.15f,  10.0f};
    const auto b = Color<Oklch>{0.6f, 0.15f, 350.0f};
    const auto m = mix(a, b, 0.5f);
    check(m.c2 > 340.0f || m.c2 < 20.0f, "hue interpolation takes the short way");

    // Contrast solving.
    for (std::uint32_t hue = 0; hue < 360; hue += 30) {
        const auto accent = Color<Oklch>{0.65f, 0.16f, static_cast<float>(hue)}.to<Srgb>();
        const Theme th = make_theme(accent, Mode::dark);
        check(contrast_ratio(th.on_accent, th.accent) >= 4.5f,
              "generated theme meets AA at hue " + std::to_string(hue));
        check(contrast_ratio(th.text_primary, th.background) >= 4.5f,
              "generated body text meets AA at hue " + std::to_string(hue));
    }

    check(pack_rgba8(colors::white) == 0xFFFFFFFFu, "pack white");
    check(pack_rgba8(rgb<0xFF0000>) == 0xFF0000FFu, "pack red as ABGR");
}

void test_sdf() {
    section("sdf");

    near(sdf::box({}, {50, 50}), -50.0f, 1e-4f, "box centre distance");
    near(sdf::box({60, 0}, {50, 50}), 10.0f, 1e-4f, "box outside distance");
    near(sdf::circle({30, 40}, 50.0f), 0.0f, 1e-4f, "point on circle");

    // Zero radii must agree EXACTLY with the sharp box, else radius(0)
    // renders differently from no radius.
    for (float x : {0.0f, 10.0f, 49.9f, 50.1f, 80.0f}) {
        near(sdf::rounded_box({x, 12.0f}, {50, 50}, {}),
             sdf::box({x, 12.0f}, {50, 50}), 1e-4f,
             "rounded_box(r=0) == box at x=" + std::to_string(x));
    }

    // Fully rounded == circle.
    near(sdf::rounded_box({30, 0}, {50, 50}, {50, 50, 50, 50}),
         sdf::circle({30, 0}, 50.0f), 1e-4f, "max radius == circle");

    // The unit-gradient property: |grad| == 1 makes AA scale-correct.
    const float eps = 0.01f;
    for (Vec2 p : {Vec2{20, 5}, Vec2{55, 30}, Vec2{-40, -40}}) {
        const float dx = (sdf::rounded_box(p + Vec2{eps, 0}, {50, 40}, {8, 8, 8, 8}) -
                          sdf::rounded_box(p - Vec2{eps, 0}, {50, 40}, {8, 8, 8, 8})) / (2 * eps);
        const float dy = (sdf::rounded_box(p + Vec2{0, eps}, {50, 40}, {8, 8, 8, 8}) -
                          sdf::rounded_box(p - Vec2{0, eps}, {50, 40}, {8, 8, 8, 8})) / (2 * eps);
        near(Vec2{dx, dy}.length(), 1.0f, 0.05f, "sdf gradient is unit length");
    }

    near(sdf::segment({0, 5}, {-10, 0}, {10, 0}, 2.0f), 4.0f, 1e-4f, "capsule distance");
    near(sdf::coverage(0.0f), 0.5f, 1e-4f, "coverage at boundary");
    check(sdf::coverage(-5.0f) == 1.0f && sdf::coverage(5.0f) == 0.0f, "coverage saturates");
}

void test_layout() {
    section("layout");

    const auto& tm = layout::default_measurer();

    // Row: 50 + gap10 + spacer + gap10 + 30, in 400 with pad 8.
    {
        auto ui = h(box() | size(50, 20), spacer(), box() | size(30, 20)) | gap(10) | pad(8);
        Node n = ui.build();
        layout::layout_tree(n, {400, 100}, tm);
        near(n.children()[0].frame().left(), 8.0f, 0.01f, "first child at padding");
        near(n.children()[2].frame().right(), 392.0f, 0.01f, "last child at right padding");
        near(n.children()[1].frame().width(), 284.0f, 0.01f, "spacer absorbs remainder");
    }

    // justify: space_between on three fixed items.
    {
        auto ui = h(box() | size(40, 10), box() | size(40, 10), box() | size(40, 10))
                | justify(Justify::space_between) | width(400);
        Node n = ui.build();
        layout::layout_tree(n, {400, 50}, tm);
        near(n.children()[0].frame().left(), 0.0f, 0.01f, "space_between first at 0");
        near(n.children()[1].frame().left(), 180.0f, 0.01f, "space_between middle centred");
        near(n.children()[2].frame().right(), 400.0f, 0.01f, "space_between last at end");
    }

    // grow shares proportionally: 1 : 3 over 400.
    {
        auto ui = h(box() | grow(1.0f), box() | grow(3.0f)) | width(400);
        Node n = ui.build();
        layout::layout_tree(n, {400, 50}, tm);
        near(n.children()[0].frame().width(), 100.0f, 0.01f, "grow 1 of 4");
        near(n.children()[1].frame().width(), 300.0f, 0.01f, "grow 3 of 4");
    }

    // align: center on the cross axis.
    {
        auto ui = h(box() | size(20, 20)) | align(Align::center) | size(200, 100);
        Node n = ui.build();
        layout::layout_tree(n, {200, 100}, tm);
        near(n.children()[0].frame().top(), 40.0f, 0.01f, "cross-axis centred");
    }

    // stretch fills the cross axis.
    {
        auto ui = v(box() | height(20)) | align(Align::stretch) | size(200, 100);
        Node n = ui.build();
        layout::layout_tree(n, {200, 100}, tm);
        near(n.children()[0].frame().width(), 200.0f, 0.01f, "stretch fills cross axis");
    }

    // Percentage lengths resolve against the parent.
    {
        auto ui = v(box() | width(pct(50)) | height(10)) | size(300, 100);
        Node n = ui.build();
        layout::layout_tree(n, {300, 100}, tm);
        near(n.children()[0].frame().width(), 150.0f, 0.01f, "pct(50) of 300");
    }

    // min/max clamping wins over grow.
    {
        auto ui = h(box() | grow() | max_size(60, 20), box() | grow()) | width(400);
        Node n = ui.build();
        layout::layout_tree(n, {400, 50}, tm);
        near(n.children()[0].frame().width(), 60.0f, 0.01f, "max_size clamps a grown child");
    }

    // Absolutely positioned children leave the flow entirely.
    {
        auto ui = h(box() | size(50, 20),
                    box() | size(10, 10) | absolute(5, 5)) | width(400);
        Node n = ui.build();
        layout::layout_tree(n, {400, 100}, tm);
        near(n.children()[1].frame().left(), 5.0f, 0.01f, "absolute child at its offset");
        near(n.children()[0].frame().left(), 0.0f, 0.01f, "flow sibling unaffected");
    }

    // REGRESSION: cross-axis default must be `stretch`, not `start`.
    // With `start`, an auto-sized child inside a column collapses to zero
    // width and the panel renders as an invisible sliver. This was a real
    // bug, found by sampling pixels and getting the background colour back
    // where a surface colour was expected.
    {
        auto ui = v(box() | grow() | bg(colors::white)) | size(200, 100);
        Node n = ui.build();
        layout::layout_tree(n, {200, 100}, tm);
        near(n.children()[0].frame().width(), 200.0f, 0.01f,
             "auto child stretches across the cross axis by default");
        near(n.children()[0].frame().height(), 100.0f, 0.01f,
             "grow fills the main axis");
    }

    // `pill` must clamp to the box rather than invert the SDF.
    {
        auto ui = box() | size(120, 30) | pill | bg(colors::red);
        Node n = ui.build();
        layout::layout_tree(n, {200, 60}, tm);
        near(n.style().corners.clamp_to(n.frame().size).tl, 15.0f, 0.01f,
             "pill radius clamps to half the short side");
    }
}

void test_draw_list() {
    section("draw list");

    DrawList dl;
    dl.fill_rect({0, 0, 10, 10}, colors::red);
    dl.fill_rect({10, 0, 10, 10}, colors::blue);
    check(dl.batches().size() == 1, "same state merges into one batch");
    check(dl.size() == 2, "two instances");

    dl.push_clip({0, 0, 5, 5});
    dl.fill_rect({0, 0, 2, 2}, colors::green);
    check(dl.batches().size() == 2, "clip change opens a new batch");
    dl.pop_clip();

    // Nested clips intersect rather than replace.
    DrawList d2;
    d2.push_clip({0, 0, 100, 100});
    d2.push_clip({50, 50, 100, 100});
    check(d2.clip() == Rect(50, 50, 50, 50), "nested clips intersect");
    d2.pop_clip();
    check(d2.clip() == Rect(0, 0, 100, 100), "pop restores the outer clip");

    // Colours must reach the GPU premultiplied and linear.
    DrawList d3;
    d3.fill_rect({0, 0, 10, 10}, colors::white.fade(0.5f));
    const auto& inst = d3.instances()[0];
    near(inst.color.w, 0.5f, 1e-3f, "alpha preserved");
    near(inst.color.x, 0.5f, 1e-3f, "rgb premultiplied by alpha");
}

void test_pixels() {
    section("pixels");

    // A solid red square on black: centre must be red, corner must be black.
    {
        auto ui = v(box() | size(50, 50) | bg(colors::red)) | pad(25);
        RenderOptions o; o.background = colors::black;
        const Image img = render(ui, {100, 100}, o);
        same_color(img.at(50, 50), colors::red, "fill centre is the fill colour");
        same_color(img.at(2, 2), colors::black, "outside the shape is background");
    }

    // Rounded corners must actually cut the corner.
    {
        auto ui = v(box() | size(80, 80) | bg(colors::white) | radius(20)) | pad(10);
        RenderOptions o; o.background = colors::black;
        const Image img = render(ui, {100, 100}, o);
        same_color(img.at(50, 50), colors::white, "rounded box centre filled");
        same_color(img.at(12, 12), colors::black, "rounded box corner cut away");
        same_color(img.at(50, 12), colors::white, "mid-edge still filled");
    }

    // Antialiasing: a CURVED edge must produce intermediate coverage.
    // Note the row matters: at the circle's vertical midline the edge is
    // vertical and lands exactly on an integer pixel boundary, where a hard
    // 0->1 transition is the CORRECT answer. Sampling there tests nothing,
    // so scan the whole disc and count genuinely partial pixels.
    {
        auto ui = v(box() | size(60, 60) | bg(colors::white) | radius(30)) | pad(20);
        RenderOptions o; o.background = colors::black;
        const Image img = render(ui, {100, 100}, o);
        int intermediates = 0;
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 100; ++x) {
                const float lum = luminance(img.at(x, y));
                if (lum > 0.02f && lum < 0.98f) ++intermediates;
            }
        }
        // A 60px-diameter circle has ~190px of circumference; most of it is
        // partially covered.
        check(intermediates >= 50, "circle edge is antialiased (found " +
                                   std::to_string(intermediates) + " partial pixels)");

        // And the AA must be a smooth ramp, not dithering: along a diagonal
        // the coverage should increase monotonically into the shape.
        const float a = luminance(img.at(28, 28));
        const float b = luminance(img.at(30, 30));
        const float c = luminance(img.at(32, 32));
        check(a <= b + 0.01f && b <= c + 0.01f, "AA ramp is monotonic into the shape");
    }

    // A border must paint on the edge and NOT in the middle.
    {
        auto ui = v(box() | size(60, 60) | border(4, colors::white)) | pad(20);
        RenderOptions o; o.background = colors::black;
        const Image img = render(ui, {100, 100}, o);
        same_color(img.at(50, 50), colors::black, "border does not fill the interior");
        check(luminance(img.at(21, 50)) > 0.5f, "border paints on the left edge");
        check(luminance(img.at(78, 50)) > 0.5f, "border paints on the right edge");
    }

    // Gradients must vary along their axis.
    {
        auto ui = v(box() | size(80, 80)
                          | linear_gradient(colors::black, colors::white, {0, 0}, {1, 0}))
                | pad(10);
        RenderOptions o; o.background = colors::slate;
        const Image img = render(ui, {100, 100}, o);
        const float left  = luminance(img.at(15, 50));
        const float right = luminance(img.at(85, 50));
        check(right > left + 0.3f, "horizontal gradient ramps left to right");
    }

    // REGRESSION: gradients must interpolate in Ok**LCH** (polar), not Oklab
    // (Cartesian) and not linear RGB. Both of the wrong choices desaturate
    // through the middle of a hue sweep; only the polar path holds chroma.
    //
    // This was a real bug twice over: first the `srgb_interpolation` flag was
    // ignored entirely, then the fix used Cartesian Oklab and came out LESS
    // vivid than plain linear RGB.
    {
        constexpr auto from = rgb<0x0090FF>;   // blue
        constexpr auto to   = rgb<0xF76B15>;   // orange (near-opposite hue)

        auto perceptual = box() | size(100, 20)
                                | linear_gradient(from, to, {0, 0}, {1, 0});
        auto naive      = box() | size(100, 20)
                                | linear_gradient(from, to, {0, 0}, {1, 0})
                                | srgb_interpolation;

        RenderOptions o; o.background = colors::black;
        const Image a = render(perceptual, {100, 20}, o);
        const Image b = render(naive,      {100, 20}, o);

        // The midpoint is where the difference is largest.
        const float chroma_perceptual = a.at(50, 10).to<Oklch>().c1;
        const float chroma_naive      = b.at(50, 10).to<Oklch>().c1;
        check(chroma_perceptual > chroma_naive + 0.05f,
              "oklch gradient keeps more chroma at the midpoint than srgb");

        // Chroma must stay roughly FLAT across a same-chroma sweep, rather
        // than sagging in the middle.
        float lo = 1.0f, hi = 0.0f;
        for (int x = 5; x < 95; ++x) {
            const float c = a.at(x, 10).to<Oklch>().c1;
            lo = num::min(lo, c);
            hi = num::max(hi, c);
        }
        check(hi - lo < 0.03f,
              "oklch ramp holds chroma flat end to end (spread " +
              std::to_string(hi - lo) + ")");

        // And the rasteriser must agree with the reference in color.hpp.
        same_color(a.at(50, 10), mix_perceptual(from, to, 0.5f),
                   "rasterised gradient midpoint == mix_perceptual()");

        // The srgb_interpolation flag must actually do something.
        check(delta(a.at(50, 10), b.at(50, 10)) > 0.05f,
              "srgb_interpolation changes the rendered result");
    }

    // Opacity composites IN LINEAR LIGHT. 50% black over white must land at
    // linear 0.5 — which encodes to sRGB ~0.735 (188/255), NOT sRGB 0.5.
    // Getting 128 here would mean the blend happened in gamma space.
    {
        auto ui = z(box() | size(100, 100) | bg(colors::white),
                    box() | size(100, 100) | bg(colors::black) | opacity(0.5f))
                | size(100, 100);
        RenderOptions o; o.background = colors::white;
        const Image img = render(ui, {100, 100}, o);
        near(luminance(img.at(50, 50)), 0.5f, 0.02f,
             "50% black over white is linear 0.5");
        near(img.at(50, 50).c0, 0.735f, 0.02f,
             "...which encodes to sRGB 0.735, proving linear-space blending");
    }

    // Clipping must actually cut children.
    {
        auto ui = v(box() | size(200, 200) | bg(colors::red))
                | size(50, 50) | clip | bg(colors::black);
        RenderOptions o; o.background = colors::blue;
        const Image img = render(ui, {100, 100}, o);
        same_color(img.at(25, 25), colors::red, "inside the clip the child paints");
        same_color(img.at(75, 75), colors::blue, "outside the clip it does not");
    }

    // Text must put ink on the canvas.
    {
        auto ui = v(text<"Hello mayag"> | font(24) | fg(colors::white)) | pad(10);
        RenderOptions o;
        o.background = colors::black;
        o.font = &strokefont::Font::builtin_font();
        const Image img = render(ui, {240, 60}, o);
        int lit = 0;
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
                if (luminance(img.at(x, y)) > 0.4f) ++lit;
        check(lit > 100, "text renders visible glyph pixels (" + std::to_string(lit) + ")");
    }

    // Shadows must darken outside the box without touching the far corner.
    {
        auto ui = v(box() | size(40, 40) | bg(colors::white) | shadow(12.0f, colors::black, {0, 0}))
                | pad(30);
        RenderOptions o; o.background = colors::white;
        const Image img = render(ui, {100, 100}, o);
        check(luminance(img.at(26, 50)) < 0.95f, "shadow darkens just outside the box");
        check(luminance(img.at(2, 2)) > 0.95f, "shadow has fallen off by the far corner");
    }
}

void test_determinism() {
    section("determinism");

    auto ui = card(themes::midnight,
                   text<"Title"> | font(16) | bold | fg(colors::white),
                   text<"Body">  | font(12) | fg(colors::slate));

    RenderOptions o;
    o.font = &strokefont::Font::builtin_font();
    const auto a = render_to_pixels(ui, {200, 100}, o);
    const auto b = render_to_pixels(ui, {200, 100}, o);
    check(a == b, "rendering is deterministic across runs");

    // DPI scaling must produce proportional output, not a resample.
    RenderOptions o2 = o;
    o2.dpi_scale = 2.0f;
    const auto hi = render_to_pixels(ui, {200, 100}, o2);
    check(hi.size() == a.size() * 4, "2x dpi yields 4x the pixels");
}

void test_png() {
    section("png");

    // A 2x2 known image must survive the encoder intact.
    const std::vector<std::uint8_t> px{
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    const auto bytes = image::encode_png(px, 2, 2);
    check(bytes.size() > 40, "png has content");
    check(bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G',
          "png signature");
    // IHDR immediately follows the 8-byte signature + 4-byte length.
    check(bytes[12] == 'I' && bytes[13] == 'H' && bytes[14] == 'D' && bytes[15] == 'R',
          "IHDR chunk present");
    const std::string tail(reinterpret_cast<const char*>(bytes.data() + bytes.size() - 8), 4);
    check(tail == "IEND", "IEND chunk terminates the file");
}

void test_batching() {
    section("batching");

    // The headline claim: a whole UI collapses into a handful of draws.
    constexpr Theme t = themes::midnight;
    auto ui = v(card(t, text<"A"> | fg(t.text_primary), text<"B"> | fg(t.text_primary)),
                card(t, text<"C"> | fg(t.text_primary), text<"D"> | fg(t.text_primary)),
                h(button<"OK">(t), button<"Cancel">(t, ButtonVariant::secondary)) | gap(8))
            | gap(12) | pad(16);

    Node n = ui.build();
    const auto& font = strokefont::Font::builtin_font();
    layout::layout_tree(n, {400, 300}, font.measurer());

    DrawList dl;
    render::PaintOptions po;
    po.glyphs = &font.glyph_renderer();
    render::paint(n, dl, po);

    check(dl.size() > 20, "the scene produced real instances");
    check(dl.batches().size() <= 4,
          "whole UI batches into <= 4 draw calls (got " +
          std::to_string(dl.batches().size()) + ")");
}

}  // namespace

/// The tiled parallel renderer must be BIT-IDENTICAL to the reference.
///
/// That equivalence is what makes it safe as the default: any divergence
/// would be a rendering bug that only appears on some machines, at some core
/// counts, in some tiles — the worst class of bug to chase. So it is checked
/// mechanically on scenes that stress every path.
void test_tiled_equivalence() {
    section("tiled renderer");

    std::printf("  simd=%s  threads=%zu\n",
                backend::simd::backend_name(),
                backend::shared_pool().worker_count() + 1);

    constexpr Theme th = themes::midnight;

    struct Scene { const char* name; Node root; };
    std::vector<Scene> scenes;

    // Shapes, shadows, gradients, clipping, blending, transparency — each is
    // a different branch in the shading kernel.
    scenes.push_back({"boxes + radii",
        (v(box() | size(200, 90) | bg(th.accent) | radius(16),
           box() | size(200, 90) | bg(th.success),
           box() | size(200, 40) | bg(th.warning) | pill) | gap(10) | pad(12)).build()});

    scenes.push_back({"shadows",
        (v(box() | size(160, 80) | bg(th.surface) | radius(12) | elevation(24),
           box() | size(160, 80) | bg(th.accent)  | radius(12)
                 | shadow(28.0f, th.accent.fade(0.7f), {0, 10})) | gap(30) | pad(40)).build()});

    scenes.push_back({"gradients",
        (v(box() | size(240, 100) | radius(12)
                 | linear_gradient(rgb<0x0090FF>, rgb<0xF76B15>, {0, 0}, {1, 0}),
           box() | size(240, 100) | radius(12)
                 | radial_gradient(colors::white, th.accent, {0.5f, 0.5f}, 0.6f))
         | gap(10) | pad(12)).build()});

    scenes.push_back({"clip + alpha",
        (v(box() | size(400, 200) | bg(th.danger) | opacity(0.55f))
         | size(120, 90) | clip | bg(th.surface) | radius(10) | pad(6)).build()});

    scenes.push_back({"borders + blend",
        (h(box() | size(90, 90) | border(6, th.accent) | radius(12),
           box() | size(90, 90) | bg(th.success) | blend(BlendMode::additive),
           box() | size(90, 90) | bg(th.warning) | blend(BlendMode::multiply))
         | gap(8) | pad(10)).build()});

    for (auto& sc : scenes) {
        for (float scale : {1.0f, 2.0f}) {
            const Vec2 viewport{320, 260};
            Node root = sc.root;
            layout::layout_tree(root, viewport, layout::default_measurer());

            DrawList dl;
            render::PaintOptions po;
            po.dpi_scale = scale;
            render::paint(root, dl, po);

            const int w = static_cast<int>(viewport.x * scale);
            const int h = static_cast<int>(viewport.y * scale);

            backend::Framebuffer ref{w, h}, tiled{w, h};
            ref.clear(th.background);
            backend::Software::render(dl, ref, nullptr);
            backend::Tiled::render(dl, tiled, nullptr, &backend::shared_pool(), th.background);

            const auto a = ref.to_rgba8();
            const auto b = tiled.to_rgba8();

            std::size_t differing = 0;
            int worst = 0;
            for (std::size_t i = 0; i < a.size(); ++i) {
                const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
                if (d != 0) { ++differing; worst = num::max(worst, d); }
            }
            check(differing == 0,
                  std::string{"tiled == reference: "} + sc.name + " @" +
                  std::to_string(static_cast<int>(scale)) + "x (" +
                  std::to_string(differing) + " bytes differ, max " +
                  std::to_string(worst) + ")");
        }
    }

    // Parallel encode must match the serial one too.
    {
        backend::Framebuffer fb{200, 200};
        fb.clear(rgb<0x336699>);
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 200; ++x) {
                fb.at(x, y) = Vec4{x / 200.0f, y / 200.0f, 0.5f, (x + y) / 400.0f};
            }
        }
        std::vector<std::uint8_t> serial, parallel;
        fb.write_rgba8(serial);
        backend::Tiled::encode_parallel(fb, parallel, &backend::shared_pool());
        check(serial == parallel, "parallel encode == serial encode");
    }

    // A pool with zero workers must still produce the same answer.
    {
        backend::ThreadPool solo{0};
        std::atomic<int> sum{0};
        solo.parallel_for(100, [&](std::size_t i) { sum += static_cast<int>(i); });
        check(sum == 4950, "an inline pool runs every index exactly once");

        std::atomic<int> par{0};
        backend::shared_pool().parallel_for(10000, [&](std::size_t) { ++par; });
        check(par == 10000, "the shared pool runs every index exactly once");
    }
}

/// The layout guarantees mayag promises.
///
/// Each of these encodes a specific way UIs break in practice. They exist
/// because mayag's own dashboard shipped with a sidebar squeezed from 196px
/// to 63px, labels wrapping one character per line, and a header pushed to
/// x=2976 in a 980px window — all from flex defaults inherited from CSS.
void test_layout_guarantees() {
    section("layout guarantees");

    const auto& tm = layout::default_measurer();

    // ── an explicit size is a GUARANTEE, not a suggestion ───────────────
    {
        auto ui = h(box() | width(196) | height(100),
                    v(box() | width(800) | height(100)) | grow())
                | width(pct(100));
        Node n = ui.build();
        layout::layout_tree(n, {500, 200}, tm);
        near(n.children()[0].frame().width(), 196.0f, 0.01f,
             "a fixed sidebar keeps its width beside an oversized sibling");
    }

    // The same, several levels deep — the real dashboard shape.
    {
        auto ui = h(v(box() | height(20), box() | height(20)) | width(196) | pad(16),
                    v(box() | width(900) | height(50)) | grow() | pad(24))
                | width(pct(100)) | height(pct(100));
        Node n = ui.build();
        layout::layout_tree(n, {600, 300}, tm);
        near(n.children()[0].frame().width(), 196.0f, 0.01f,
             "and keeps it when nested inside padded containers");
    }

    // ── grow() must also SHRINK ─────────────────────────────────────────
    //
    // A node that fills leftover space is the one that should compress when
    // there is none. `grow()` alone silently overflowed and pushed content
    // hundreds of pixels off-screen.
    {
        auto ui = h(box() | width(200) | height(50),
                    v(box() | width(600) | height(50)) | grow())
                | width(pct(100));
        Node n = ui.build();
        layout::layout_tree(n, {400, 100}, tm);
        const Rect grown = n.children()[1].frame();
        check(grown.right() <= 401.0f,
              "grow() compresses rather than overflowing (right edge " +
              std::to_string(static_cast<int>(grown.right())) + " of 400)");
    }

    // ── opting OUT is still possible ────────────────────────────────────
    {
        auto ui = h(box() | width(300) | height(50) | rigid,
                    box() | width(300) | height(50) | rigid)
                | width(pct(100));
        Node n = ui.build();
        layout::layout_tree(n, {400, 100}, tm);
        near(n.children()[0].frame().width(), 300.0f, 0.01f,
             "rigid children refuse to compress, and overflow visibly instead");
    }

    // ── text never wraps into an unreadable ribbon ──────────────────────
    {
        TextStyle ts;
        ts.size = 13.0f;
        ts.overflow = TextOverflow::wrap;

        const float sane = tm.measure("Overview Pipelines", ts, 200.0f).y;
        for (float w : {12.0f, 4.0f, 1.0f, 0.0f}) {
            const float h = tm.measure("Overview Pipelines", ts, w).y;
            check(h <= sane * 4.0f,
                  "a " + std::to_string(static_cast<int>(w)) +
                  "px box does not wrap to one char per line");
        }
    }

    // ── the auditor finds real faults ───────────────────────────────────
    {
        // A deliberately broken tree: a rigid child far wider than its
        // clipping parent.
        auto bad = v(box() | size(500, 50) | bg(colors::red) | rigid)
                 | size(100, 60) | clip | bg(colors::black);
        Node n = bad.build();
        layout::layout_tree(n, {200, 100}, tm);

        const auto issues = layout::audit(n, &tm);
        bool found = false;
        for (const auto& i : issues) {
            if (i.kind == layout::Issue::Kind::overflow) found = true;
        }
        check(found, "audit() reports children overflowing a clipped parent");
    }

    // ── dead grow() is reported ─────────────────────────────────────────
    //
    // `grow()` on an element whose MAIN-axis size is pinned is dead code. The
    // DSL cannot catch it (which axis is "main" is the PARENT's property, and
    // an element cannot see its parent), so it belongs in the auditor.
    {
        auto ui = h(box() | width(200) | height(20) | grow(),
                    box() | height(20) | grow()) | width(pct(100));
        Node n = ui.build();
        layout::layout_tree(n, {500, 100}, tm);

        int dead = 0;
        for (const auto& i : layout::audit(n, &tm)) {
            if (i.kind == layout::Issue::Kind::dead_grow) ++dead;
        }
        check(dead == 1, "audit() reports grow() with a pinned main-axis size");
    }

    // The same size on the CROSS axis is legitimate and must stay silent.
    {
        auto ui = h(box() | height(20) | grow()) | width(pct(100));
        Node n = ui.build();
        layout::layout_tree(n, {500, 100}, tm);
        for (const auto& i : layout::audit(n, &tm)) {
            check(i.kind != layout::Issue::Kind::dead_grow,
                  "a pinned CROSS-axis size does not trigger dead_grow");
        }
    }

    // ── impossible constraints ──────────────────────────────────────────
    {
        auto ui = v(box() | min_size(200, 20) | max_size(100, 100) | height(20));
        Node n = ui.build();
        layout::layout_tree(n, {400, 100}, tm);

        bool found = false;
        for (const auto& i : layout::audit(n, &tm)) {
            if (i.kind == layout::Issue::Kind::contradiction) found = true;
        }
        check(found, "audit() reports min_width > max_width");
    }

    {
        // A healthy tree must be silent, or the auditor is useless.
        auto good = v(box() | size(80, 20) | bg(colors::blue),
                      box() | size(80, 20) | bg(colors::green))
                  | gap(8) | pad(10) | size(120, 90) | bg(colors::black);
        Node n = good.build();
        layout::layout_tree(n, {200, 150}, tm);
        check(layout::audit(n, &tm).empty(), "audit() is silent on a healthy tree");
    }

    // ── DEFAULTS that make the failure mode impossible ──────────────────
    //
    // Four bugs shipped in mayag's own examples, all the same shape: an API
    // whose DEFAULT was the dangerous choice, so getting it right required
    // remembering to opt out. The fix each time was to flip the default, not
    // to patch the call site.
    {
        // Text defaults to ellipsis, not wrap. A wrapping label in a tight
        // row becomes an unreadable vertical ribbon that looks like the text
        // engine failed.
        TextStyle st;
        check(st.overflow == TextOverflow::ellipsis,
              "text defaults to ellipsis, so labels degrade gracefully");

        auto para = text<"x"> | wrap_text;
        check(para.style.text.overflow == TextOverflow::wrap,
              "paragraphs opt IN to wrapping");
    }

    {
        // z() sizes to its FIRST child. Making every child absolute leaves
        // the stack nothing to size from, so it collapses — and since
        // z-stacks are usually clipped, it crops its own contents.
        // Nested, because a root element fills its viewport by definition;
        // what matters is that the stack takes its size from the first child
        // when it is free to.
        auto ui = v(z(box() | size(100, 40), box() | size(20, 20)));
        Node n = ui.build();
        layout::layout_tree(n, {300, 200}, tm);

        const Node& stack = n.children()[0];
        near(stack.children()[0].frame().height(), 40.0f, 0.01f,
             "z() keeps its first child in flow");
        near(stack.frame().height(), 40.0f, 0.01f,
             "so the stack sizes to it rather than collapsing");
    }

    {
        // A clipping parent CROPS absolutely positioned children too, so
        // they must be part of the overflow check. Excluding them hid the
        // exact bug that shipped: a text field 19.6px tall around 22.4px of
        // text, and the auditor said nothing.
        auto bad = z(box() | size(10, 10),
                     box() | size(200, 60) | absolute(0, 0))
                 | size(100, 20) | clip;
        Node n = bad.build();
        layout::layout_tree(n, {300, 200}, tm);

        bool found = false;
        for (const auto& i : layout::audit(n, &tm)) {
            if (i.kind == layout::Issue::Kind::overflow) found = true;
        }
        check(found, "audit() reports absolute children cropped by a clipping parent");
    }

    {
        // Text taller than its box is reported in EVERY overflow mode, not
        // just clip. The common fault is a parent that reserved a height
        // using its own font size while the text renders at another.
        auto bad = v(text<"Hello"> | font(16) | height(10));
        Node n = bad.build();
        layout::layout_tree(n, {200, 100}, tm);

        bool found = false;
        for (const auto& i : layout::audit(n, &tm)) {
            if (i.kind == layout::Issue::Kind::text_clipped) found = true;
        }
        check(found, "audit() reports text taller than its own box");
    }

    {
        // A scroll viewport is SUPPOSED to hold more than it shows, so
        // overflow there is not a fault. An auditor that cries wolf on the
        // most common container gets switched off.
        ScrollState sc;
        std::vector<Node> rows;
        for (int i = 0; i < 20; ++i) rows.push_back((box() | size(80, 30)).build());
        auto ui = list(Axis::vertical, std::move(rows)) | size(100, 100) | scroll(sc);
        Node n = ui.build();
        layout::layout_tree(n, {200, 200}, tm);

        for (const auto& i : layout::audit(n, &tm)) {
            check(i.kind != layout::Issue::Kind::overflow,
                  "a scroll viewport is not reported as overflowing");
        }
    }

    // ── every shipped example must audit clean ──────────────────────────
    //
    // This is the test that would have caught the broken dashboard before it
    // ever reached a screenshot.
    {
        constexpr Theme th = themes::midnight;
        auto page = v(h(v(text<"mayag"> | font(16) | bold | fg(th.text_primary),
                          text<"Overview"> | font(13) | fg(th.text_secondary),
                          text<"Pipelines"> | font(13) | fg(th.text_secondary))
                        | gap(6) | pad(16) | width(196) | bg(th.surface),
                        v(text<"Overview"> | font(22) | bold | fg(th.text_primary),
                          progress(th, 0.6f),
                          h(badge<"live">(th, th.success), kbd<"T">(th)) | gap(8))
                        | gap(12) | pad(24) | grow())
                      | height(pct(100)))
                    | width(pct(100)) | height(pct(100)) | bg(th.background);

        Node n = page.build();
        layout::layout_tree(n, {900, 500}, tm);
        const auto issues = layout::audit(n, &tm);
        check(issues.empty(),
              "a realistic app layout audits clean:\n" + layout::format_issues(issues));
    }
}

/// The GPU instance layout must match the shader's struct EXACTLY.
///
/// This is the one place a mistake is both silent and total: the buffer is
/// uploaded raw, so a single wrong offset makes every frame garbage with no
/// error anywhere. The MSL struct is 7 x float4 followed by 4 x uint.
void test_gpu_layout() {
    section("gpu layout");

    static_assert(sizeof(Instance) == 128,
                  "Instance must stay 128 bytes to match the shader struct");
    static_assert(alignof(Instance) >= 16, "float4 fields need 16-byte alignment");

    check(offsetof(Instance, rect)   ==   0, "rect at 0");
    check(offsetof(Instance, radii)  ==  16, "radii at 16");
    check(offsetof(Instance, color)  ==  32, "color at 32");
    check(offsetof(Instance, color2) ==  48, "color2 at 48");
    check(offsetof(Instance, axis)   ==  64, "axis at 64");
    check(offsetof(Instance, uv)     ==  80, "uv at 80");
    check(offsetof(Instance, params) ==  96, "params at 96");
    check(offsetof(Instance, kind)   == 112, "kind at 112, after the float4 block");
    check(offsetof(Instance, flags)  == 116, "flags at 116");
    check(sizeof(Instance) == 128, "the whole instance is 128 bytes");

    // The shader reads these as literals, so a renumbering would silently
    // change what every instance draws.
    check(static_cast<std::uint32_t>(ShapeKind::rounded_box) == 0, "rounded_box is kind 0");
    check(static_cast<std::uint32_t>(ShapeKind::glyph)  == 4, "glyph is kind 4");
    check(static_cast<std::uint32_t>(ShapeKind::shadow) == 6, "shadow is kind 6");
    check(instance_flags::gradient  == 1u,   "gradient flag is bit 0");
    check(instance_flags::stroke_only == 16u, "stroke flag is bit 4");
    check(instance_flags::glyph_sdf == 128u, "glyph_sdf flag is bit 7");

    // Both shader stages must exist in the source the backend compiles.
    const std::string msl = std::string{render::shaders::msl_kernel} +
                            std::string{render::shaders::msl_shaders};
    check(msl.find("mayag_vertex") != std::string::npos, "MSL declares mayag_vertex");
    check(msl.find("mayag_fragment") != std::string::npos, "MSL declares mayag_fragment");
    check(msl.find("mg_rounded_box") != std::string::npos,
          "and shares the SDF kernel with the CPU path");
}

int main() {
    std::printf("mayag test suite\n================");

    test_color();
    test_sdf();
    test_layout();
    test_draw_list();
    test_pixels();
    test_determinism();
    test_png();
    test_batching();
    test_tiled_equivalence();
    test_layout_guarantees();
    test_gpu_layout();

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
