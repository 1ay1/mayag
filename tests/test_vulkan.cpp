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

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
