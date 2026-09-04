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

Diff compare(backend::VulkanDevice& dev, const Node& tree, int w, int h, Color<Srgb> clear) {
    DrawList dl;
    render::paint(tree, dl, {});

    std::vector<std::uint8_t> gpu;
    if (!dev.render_offscreen(dl, w, h, clear, gpu)) return {1e9, 255, 1.0};

    backend::Framebuffer fb{w, h};
    backend::Tiled::render(dl, fb, nullptr, &backend::shared_pool(), clear);
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

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
