// tests/test_vulkan.cpp — the Vulkan backend
//
// mayag's correctness rule for any GPU backend: its output must match the
// software reference rasteriser, because a timing benchmark cannot tell a fast
// backend from one that draws nothing. This file renders the same scenes
// through VulkanDevice (offscreen) and through the tiled software rasteriser
// and asserts they agree.
//
// The whole file skips itself cleanly when no Vulkan device is available (a CI
// runner with no GPU and no llvmpipe), so it is meaningful where a GPU exists
// and silent where one does not — never a spurious red.

#include <optional>
#include <mayag/mayag.hpp>
#include <mayag/backend/vulkan.hpp>
#include <mayag/backend/tiled.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int failures = 0, checks = 0;

void check(bool ok, std::string_view what) {
    ++checks;
    if (!ok) { ++failures; std::printf("  FAIL  %.*s\n",
                                       static_cast<int>(what.size()), what.data()); }
}
void section(std::string_view s) { std::printf("\n%.*s\n",
                                               static_cast<int>(s.size()), s.data()); }

// Render a laid-out tree both ways and report the mean absolute per-channel
// difference and the fraction of pixels that differ by more than 8/255. The
// two paths are NOT bit-identical — the GPU rasteriser's edge coverage differs
// slightly from the CPU's analytic coverage — but the interiors must match and
// the disagreement must be confined to antialiased edges.
struct Diff { double mean; int max; double frac_over8; };

// Compare one frame rendered both ways.
//
// `stack`, when given, wires a real font engine into BOTH paths: the glyph
// atlas is uploaded to the GPU and handed to the software rasteriser as its
// CoverageSampler. Without it neither path draws any glyphs, so a text bug is
// invisible — which is how a GPU-only missing stem-darkening correction went
// unnoticed. Any parity check that means to cover text MUST pass a stack.
Diff compare(backend::VulkanDevice& dev, const Node& tree, int w, int h, Color<Srgb> clear,
             typo::FontStack* stack = nullptr) {
    DrawList dl;
    render::PaintOptions po{};
    po.clear_color = clear;   // the frame is cleared to it, so text on no fill sits on it
    std::optional<typo::StackMeasurer>      meas;
    std::optional<typo::StackGlyphRenderer> glyphs;
    std::optional<typo::StackSampler>       sampler_store;
    if (stack != nullptr) {
        meas.emplace(*stack);
        glyphs.emplace(*stack);
        sampler_store.emplace(*stack);
        po.measurer = &*meas;
        po.glyphs   = &*glyphs;
    }
    render::paint(tree, dl, po);

    if (stack != nullptr) dev.sync_atlas(stack->atlas());

    std::vector<std::uint8_t> gpu;
    if (!dev.render_offscreen(dl, w, h, clear, gpu)) return {1e9, 255, 1.0};

    backend::Framebuffer fb{w, h};
    const backend::CoverageSampler* sampler =
        sampler_store.has_value()
            ? static_cast<const backend::CoverageSampler*>(&*sampler_store)
            : nullptr;
    backend::Tiled::render(dl, fb, sampler, &backend::shared_pool(), clear);
    std::vector<std::uint8_t> sw;
    backend::Tiled::encode_parallel(fb, sw, &backend::shared_pool());

    long long sum = 0, over = 0; int mx = 0;
    const std::size_t n = std::min(gpu.size(), sw.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int d = std::abs(static_cast<int>(gpu[i]) - static_cast<int>(sw[i]));
        sum += d; if (d > mx) mx = d; if (d > 8) ++over;
    }
    return {static_cast<double>(sum) / static_cast<double>(n), mx,
            static_cast<double>(over) / static_cast<double>(n)};
}

Node laid_out(auto elem, int w, int h) {
    Node n = elem.build();
    layout::layout_tree(n, {static_cast<float>(w), static_cast<float>(h)},
                        layout::default_measurer());
    return n;
}

}  // namespace

int main() {
    std::printf("mayag vulkan backend\n====================\n");

    backend::VulkanDevice dev;
    if (!dev.init_offscreen()) {
        std::printf("\n(no Vulkan device available — skipped)\nPASS  0 checks, 0 failures\n");
        return 0;
    }
    std::printf("device: %s\n", dev.device_name());
    std::printf("text blend: %s\n", dev.encoded_text() ? "encoded (CoreGraphics)" : "linear approximation");

    // ── shapes match the reference ───────────────────────────────────────
    section("shapes");
    {
        const int W = 260, H = 160;
        auto ui = v(h(box() | size(64, 64) | bg(rgb<0xE0403A>) | radius(16),
                      box() | size(64, 64) | bg(rgb<0x40C060>) | radius(32),
                      box() | size(64, 64) | bg(rgb<0x4060E0>)) | gap(10),
                    box() | size(220, 18) | bg(rgb<0xF0A020>) | radius(9))
                  | gap(12) | pad(16) | bg(rgb<0x101418>);
        Node n = laid_out(ui, W, H);
        Diff d = compare(dev, n, W, H, rgb<0x101418>);
        std::printf("  shapes: mean|d|=%.3f max=%d edges>8=%.2f%%\n",
                    d.mean, d.max, d.frac_over8 * 100.0);
        check(d.mean < 3.0, "GPU shapes match the software rasteriser (mean)");
        check(d.frac_over8 < 0.06, "differences are confined to antialiased edges");
    }

    // ── solid interiors are essentially exact ────────────────────────────
    section("solid fill");
    {
        const int W = 120, H = 120;
        auto ui = box() | size(120, 120) | bg(rgb<0x3366CC>);   // fills the frame
        Node n = laid_out(ui, W, H);
        Diff d = compare(dev, n, W, H, colors::black);
        std::printf("  solid: mean|d|=%.3f max=%d\n", d.mean, d.max);
        // A flat interior has no antialiased edges to disagree on, so the two
        // paths should be within a rounding step everywhere.
        check(d.mean < 1.0, "a solid fill is near-identical on GPU and CPU");
    }

    // ── gradients ────────────────────────────────────────────────────────
    section("gradient");
    {
        const int W = 200, H = 80;
        auto ui = box() | size(200, 80)
                       | linear_gradient(rgb<0xE0403A>, rgb<0x4060E0>);
        Node n = laid_out(ui, W, H);
        Diff d = compare(dev, n, W, H, colors::black);
        std::printf("  gradient: mean|d|=%.3f max=%d\n", d.mean, d.max);
        check(d.mean < 4.0, "gradient interpolation agrees within tolerance");
    }

    // ── determinism: two GPU renders of one scene are identical ──────────
    section("determinism");
    {
        const int W = 150, H = 100;
        auto ui = h(box() | size(60, 60) | bg(rgb<0xCC5522>) | radius(12),
                    box() | size(60, 60) | bg(rgb<0x22AA88>) | radius(30))
                  | gap(8) | pad(12) | bg(rgb<0x0B0D10>);
        Node n = laid_out(ui, W, H);
        DrawList dl; render::paint(n, dl, {});
        std::vector<std::uint8_t> a, b;
        const bool ra = dev.render_offscreen(dl, W, H, rgb<0x0B0D10>, a);
        const bool rb = dev.render_offscreen(dl, W, H, rgb<0x0B0D10>, b);
        check(ra && rb && a == b, "two GPU renders of the same scene are bit-identical");
    }

    // ── colour emoji on the GPU ──────────────────────────────────────────
    section("colour emoji");
    {
        auto stack = typo::system::default_stack();
        const auto [fi, gid] = stack->resolve(0x1F600);
        if (gid == 0 || stack->face_at(fi) == nullptr ||
            !stack->face_at(fi)->is_color()) {
            std::printf("  (no colour emoji font — skipped)\n");
        } else {
            const int W = 200, H = 90;
            auto ui = v(text_owned("Hi \xF0\x9F\x98\x80") | font(44) | fg(colors::white))
                      | pad(16) | bg(colors::black);
            Node n = ui.build();
            typo::StackMeasurer meas{*stack};
            layout::layout_tree(n, {static_cast<float>(W), static_cast<float>(H)}, meas);
            DrawList dl; typo::StackGlyphRenderer gr{*stack};
            render::PaintOptions po; po.measurer = &meas; po.glyphs = &gr;
            render::paint(n, dl, po);
            dev.sync_atlas(stack->atlas());   // upload coverage + colour planes
            std::vector<std::uint8_t> px;
            const bool ok = dev.render_offscreen(dl, W, H, colors::black, px);
            int coloured = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
                if (std::abs(int(px[i]) - int(px[i + 1])) > 25 ||
                    std::abs(int(px[i + 1]) - int(px[i + 2])) > 25) ++coloured;
            }
            check(ok && coloured > 150,
                  std::string{"GPU renders colour emoji from the RGBA atlas ("} +
                  std::to_string(coloured) + " px)");
        }
    }

    // ── backdrop blur on the GPU ────────────────────────────────────
    //
    // A frosted panel over a hard red|blue seam blends it. The GPU path
    // snapshots the background mid-frame and the shader blurs the snapshot,
    // so the pixel at the seam under the panel carries both colours — the
    // same result the software rasteriser gives.
    section("backdrop blur");
    {
        const int W = 120, H = 120;
        auto ui = z(h(box() | size(60, 120) | bg(rgb<0xFF3020>),
                      box() | size(60, 120) | bg(rgb<0x2040FF>)),
                    box() | size(120, 120) | backdrop_blur(10.0f, 1.0f))
                  | width(120) | height(120);
        Node n = ui.build();
        layout::layout_tree(n, {static_cast<float>(W), static_cast<float>(H)},
                            layout::default_measurer());
        DrawList dl; render::paint(n, dl, {});
        std::vector<std::uint8_t> px;
        const bool ok = dev.render_offscreen(dl, W, H, colors::black, px);
        auto at = [&](int x, int y) {
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            return std::array<int, 3>{px[i], px[i + 1], px[i + 2]};
        };
        const auto mid = at(60, 60);
        check(ok, "the GPU backdrop frame rendered");
        check(mid[0] > 40 && mid[2] > 40,
              "GPU frosted glass blends the seam (mid pixel carries red AND blue)");
        // The background outside the panel still rendered — the two-segment
        // split did not clobber it.
        const auto edge = at(10, 10);
        check(edge[0] > 150, "the background survives the backdrop split");
    }

    // ── the parity gate ─────────────────────────────────────────────
    //
    // The checks above are shape-by-shape and forgiving. THIS one is the
    // regression gate for the single-source-of-truth property: a realistic
    // frame, on a NON-BLACK background, exercising every failure mode that
    // once slipped through because each was invisible to the older tests.
    //
    //   * non-black clear   — a linear-vs-sRGB clear mismatch is identical to
    //                         a correct one when the clear is black, which is
    //                         precisely why that bug survived so long;
    //   * light text on dark — catches a missing stem-darkening correction and
    //                         any glyph-quad placement drift;
    //   * dark text on light — the opposite polarity, so a gamma applied with
    //                         the wrong sign cannot pass by symmetry;
    //   * a gradient         — catches interpolation-space divergence;
    //   * translucent overlap — catches gamma-space alpha blending, which only
    //                         shows up where coverage is partial.
    //
    // The tolerance is tight on PURPOSE. GPU and CPU rasterisers differ by a
    // hair at edge pixels (different but equally valid coverage arithmetic), so
    // demanding bit-equality would be wrong. Demanding this much says: nothing
    // SYSTEMATIC differs. A whole-image mean above ~1/255, or any pixel off by
    // more than a third of the range, means a backend has re-derived some part
    // of the pixel program instead of inheriting it from render/kernel.hpp.
    section("parity: one realistic frame, both paths");
    {
        const int W = 420, H = 300;
        const auto bg_color = rgb<0x141821>;   // deliberately NOT black

        auto stack = typo::system::default_stack();

        auto ui = v(
            text<"Light on dark"> | font(18) | fg(rgb<0xF2F5FA>),
            box() | size(400, 44) | radius(12)
                  | linear_gradient(rgb<0x0090FF>, rgb<0xF76B15>, {0, 0}, {1, 0}),
            h(box() | size(120, 40) | radius(10) | bg(rgb<0xF2F5FA>),
              box() | size(120, 40) | radius(10) | bg(rgb<0x5B8CFF>)
                    | opacity(0.55f)) | gap(-40),
            box() | size(400, 44) | radius(8) | bg(rgb<0xF2F5FA>),
            text<"Dark on light"> | font(18) | fg(rgb<0x11151C>),
            box() | size(400, 2) | bg(rgb<0x2A3040>)
        ) | gap(8) | pad(16) | width(420) | height(300);

        Node n = ui.build();
        typo::StackMeasurer measurer{*stack};
        layout::layout_tree(n, {static_cast<float>(W), static_cast<float>(H)}, measurer);

        Diff d = compare(dev, n, W, H, bg_color, stack.get());
        std::printf("  parity: mean|d|=%.3f max=%d over8=%.2f%%\n",
                    d.mean, d.max, d.frac_over8 * 100.0);

        // These bounds sit just above the measured steady state (mean ~0.26,
        // over8 ~1.05%), which is pure edge-pixel disagreement between two
        // legitimate rasterisers. They are deliberately snug: dropping the
        // shader's stem-darkening call — the exact bug that shipped — moves
        // these to ~0.30 / ~1.27% and trips the gate. If a change to
        // render/kernel.hpp moves them, that is expected and BOTH paths moved
        // together; if only one moved, a backend has re-derived the pixel
        // program and that is the failure this test exists to catch.
        check(d.mean < 0.29,
              "GPU and CPU agree on a real frame with text (mean < 0.29/255)");
        check(d.max < 85,
              "no pixel differs systematically (max < 85/255)");
        check(d.frac_over8 < 0.0115,
              "fewer than 1.15% of pixels differ by more than 8/255");
    }

    // ── coloured text blends as CoreGraphics does ─────────────────────────
    // The reference draws glyphs with an ENCODED-space lerp
    // (Framebuffer::blend_text). A GPU blending in linear light can only
    // approximate that through coverage reshaping, which is exact for grey
    // text on black or white and off by up to ~60/255 for saturated text on
    // a light or coloured backdrop: blue links on white, accent text on a
    // tinted card. With the encoded text pass (a UNORM view of the target)
    // the GPU's edge pixels must match the reference's.
    section("coloured text: encoded-space blend");
    {
        const int W = 420, H = 220;
        auto stack = typo::system::default_stack();
        auto ui = v(
            h(text<"Links and accents"> | font(20) | fg(rgb<0x0060FF>)) | bg(rgb<0xFFFFFF>) | pad(6),
            h(text<"Mauve on base"> | font(20) | fg(rgb<0xCBA6F7>)) | bg(rgb<0x1E1E2E>) | pad(6),
            h(text<"Peach on surface"> | font(20) | fg(rgb<0xFAB387>)) | bg(rgb<0x313244>) | pad(6),
            h(text<"Red on cream"> | font(20) | fg(rgb<0xD20F39>)) | bg(rgb<0xEFF1F5>) | pad(6),
            h(text<"White on blue"> | font(20) | fg(rgb<0xFFFFFF>)) | bg(rgb<0x0060FF>) | pad(6)
        ) | gap(4) | pad(8) | width(420) | height(220);
        Node n = ui.build();
        typo::StackMeasurer measurer{*stack};
        layout::layout_tree(n, {static_cast<float>(W), static_cast<float>(H)}, measurer);
        Diff d = compare(dev, n, W, H, rgb<0x808080>, stack.get());
        std::printf("  coloured text: mean|d|=%.3f max=%d over8=%.2f%%\n",
                    d.mean, d.max, d.frac_over8 * 100.0);
        // The same boxes without their text: what the shapes alone disagree
        // by (edge antialiasing), so the check isolates the TEXT blend.
        auto bare = v(
            h() | bg(rgb<0xFFFFFF>) | pad(6) | height(36), h() | bg(rgb<0x1E1E2E>) | pad(6) | height(36),
            h() | bg(rgb<0x313244>) | pad(6) | height(36), h() | bg(rgb<0xEFF1F5>) | pad(6) | height(36),
            h() | bg(rgb<0x0060FF>) | pad(6) | height(36)
        ) | gap(4) | pad(8) | width(420) | height(220);
        Node nb = bare.build();
        layout::layout_tree(nb, {static_cast<float>(W), static_cast<float>(H)}, measurer);
        const Diff base = compare(dev, nb, W, H, rgb<0x808080>, stack.get());
        std::printf("  boxes only:    mean|d|=%.3f max=%d over8=%.2f%%\n",
                    base.mean, base.max, base.frac_over8 * 100.0);
        // Measured on NVIDIA and lavapipe: with the encoded blend the text
        // adds NOTHING over the boxes' own edge disagreement (max 30 either
        // way, mean slightly lower: glyphs cover some box edges). The
        // linear-light approximation adds +0.27 mean and a 63/255 max.
        check(d.max <= base.max + 4, "text adds no outlier pixels over its backdrops (max)");
        check(d.mean <= base.mean + 0.05, "coloured text matches the reference (mean)");
    }

    // ── the pass planner keeps painter order ──────────────────────────────
    section("frame planner");
    {
        render::FramePlanner planner;
        const auto glyph_at = [](DrawList& dl, float x, float y) {
            dl.glyph(Rect{Vec2{x, y}, Vec2{8, 12}}, Rect{Vec2{0, 0}, Vec2{0.01f, 0.01f}}, rgb<0xFFFFFF>);
        };
        // Ten cards, each with a label on top: 2 passes, not 20.
        DrawList cards;
        for (int i = 0; i < 10; ++i) {
            const float y = static_cast<float>(i) * 40.0f;
            cards.fill_rect(Rect{Vec2{0, y}, Vec2{200, 36}}, rgb<0x313244>);
            glyph_at(cards, 10, y + 10);
            glyph_at(cards, 20, y + 10);
        }
        const auto& p = planner.plan(cards, 400, 400, true);
        check(p.passes() == 2, "cards with labels: one shape pass, one text pass");
        check(!p.steps.empty() && p.steps.back().kind == render::PassStep::Kind::text,
              "labels draw after their cards");

        // A shape painted OVER text must stay over it.
        DrawList cover;
        glyph_at(cover, 10, 10);
        cover.fill_rect(Rect{Vec2{0, 0}, Vec2{50, 50}}, rgb<0x000000>);
        const auto& q = planner.plan(cover, 100, 100, true);
        check(q.passes() == 2 && q.steps.front().kind == render::PassStep::Kind::text,
              "a shape over text: text pass first, then the shape");

        // Disjoint text and shapes commute: still 2 passes.
        DrawList apart;
        glyph_at(apart, 10, 10);
        apart.fill_rect(Rect{Vec2{200, 200}, Vec2{50, 50}}, rgb<0x000000>);
        glyph_at(apart, 300, 300);
        check(planner.plan(apart, 400, 400, true).passes() == 2, "disjoint items commute");

        // Without a UNORM view everything is one pass, in list order.
        check(planner.plan(cards, 400, 400, false).passes() == 1, "no text view: one pass");

        // Every instance drawn exactly once.
        std::vector<int> seen(cards.size(), 0);
        const auto& c = planner.plan(cards, 400, 400, true);
        for (const auto& r : c.runs) for (std::uint32_t k = 0; k < r.count; ++k) ++seen[r.first + k];
        bool once = true;
        for (int s : seen) once = once && s == 1;
        check(once, "every instance is drawn exactly once");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
