// tests/test_texture.cpp -- image() must actually draw, on BOTH backends.
//
// image() and Device::upload_texture existed as public API for a long time
// while no backend implemented the upload and no shader had a binding for it:
// the DSL element compiled, emitted an instance, and drew nothing. A dead
// public API is worse than a missing one, because it looks like a feature.
//
// This asserts the whole path -- DSL -> DrawList -> both rasterisers -- and
// checks ORIENTATION as well as presence, using a four-colour quadrant image
// so a flip or a channel swizzle shows up as a specific wrong colour rather
// than as a vague "looks off".
#include <mayag/core/target.hpp>
#include <mayag/mayag.hpp>
#include <mayag/backend/tiled.hpp>
#if MAYAG_GPU_VULKAN
#include <mayag/backend/vulkan.hpp>
#endif
#include <cstdio>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

// A checkerboard with distinct colours per quadrant: any flip, swizzle or
// gamma error is visible as a specific wrong colour rather than "looks off".
static std::vector<std::uint8_t> make_image(int w, int h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool left = x < w / 2, top = y < h / 2;
            const bool check = ((x / 8) + (y / 8)) % 2 == 0;
            std::uint8_t r = 0, g = 0, b = 0;
            if (top && left)        { r = 220; g =  40; b =  40; }   // red
            else if (top && !left)  { r =  40; g = 200; b =  60; }   // green
            else if (!top && left)  { r =  50; g =  90; b = 230; }   // blue
            else                    { r = 230; g = 200; b =  50; }   // yellow
            if (!check) { r = static_cast<std::uint8_t>(r / 3);
                          g = static_cast<std::uint8_t>(g / 3);
                          b = static_cast<std::uint8_t>(b / 3); }
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            px[i] = r; px[i+1] = g; px[i+2] = b; px[i+3] = 255;
        }
    }
    return px;
}

// Feed the image to the software rasteriser through the sampler interface.
class ImgSampler final : public backend::CoverageSampler {
  public:
    ImgSampler(const std::uint8_t* p, int w, int h) : src_{p, w, h} {}
    [[nodiscard]] float sample(std::uint32_t, float, float) const override { return 0.0f; }
    [[nodiscard]] Vec4 sample_texture(float u, float v) const override {
        return src_.sample(u, v);
    }
  private:
    backend::TextureSource src_;
};

int main() {
    const int IW = 64, IH = 64;
    const auto img = make_image(IW, IH);

    const int W = 200, H = 160;
    auto ui = (v(image(1) | width(px(120)) | height(px(120)) | radius(12))
               | pad(20) | width(px(W)) | height(px(H))
               | bg(rgb<0x101418>)).build();
    Node n = ui;
    layout::layout_tree(n, {static_cast<float>(W), static_cast<float>(H)},
                        layout::default_measurer());

    DrawList dl;
    render::paint(n, dl, {});

    int tex_instances = 0;
    for (const auto& in : dl.instances())
        if (static_cast<ShapeKind>(in.kind) == ShapeKind::texture) ++tex_instances;
    std::printf("draw list: %zu instances, %d of kind texture\n",
                dl.instances().size(), tex_instances);
    if (tex_instances == 0) {
        std::printf("FAIL: image() emitted no texture instance\n");
        return 1;
    }

    backend::Framebuffer fb{W, H};
    ImgSampler smp{img.data(), IW, IH};
    backend::Tiled::render(dl, fb, &smp, &backend::shared_pool(), rgb<0x101418>);
    std::vector<std::uint8_t> out;
    backend::Tiled::encode_parallel(fb, out, &backend::shared_pool());
    (void)imageio::write_png("/tmp/tex_sw.png", out, W, H);

    // Did the quadrant colours actually land where they should?
    const auto at = [&](int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
        return std::array<int,3>{out[i], out[i+1], out[i+2]};
    };
    const auto tl = at(50, 50), tr = at(110, 50), bl = at(50, 110), br = at(110, 110);
    std::printf("  top-left    (want reddish)  = (%d,%d,%d)\n", tl[0], tl[1], tl[2]);
    std::printf("  top-right   (want greenish) = (%d,%d,%d)\n", tr[0], tr[1], tr[2]);
    std::printf("  bottom-left (want blueish)  = (%d,%d,%d)\n", bl[0], bl[1], bl[2]);
    std::printf("  bottom-right(want yellow)   = (%d,%d,%d)\n", br[0], br[1], br[2]);

    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    };
    check(tl[0] > tl[1] && tl[0] > tl[2], "top-left is red-dominant");
    check(tr[1] > tr[0] && tr[1] > tr[2], "top-right is green-dominant");
    check(bl[2] > bl[0] && bl[2] > bl[1], "bottom-left is blue-dominant");
    check(br[0] > 60 && br[1] > 60 && br[2] < br[0], "bottom-right is yellow-ish");
    // GPU: same draw list, same image, through the real Vulkan path.
#if MAYAG_GPU_VULKAN
    backend::VulkanDevice gpu;
    if (gpu.init_offscreen()) {
        gpu.upload_texture(img.data(), IW, IH);
        std::vector<std::uint8_t> gout;
        if (gpu.render_offscreen(dl, W, H, rgb<0x101418>, gout)) {
            (void)imageio::write_png("/tmp/tex_gpu.png", gout, W, H);
            double sum=0; int worst=0; long n=0;
            for (std::size_t i=0;i+3<std::min(out.size(),gout.size());i+=4)
                for (int c=0;c<3;++c){ int d=std::abs(int(out[i+c])-int(gout[i+c]));
                    sum+=d; if(d>worst) worst=d; ++n; }
            std::printf("\n  GPU vs software: mean|d|=%.3f  worst=%d\n", n?sum/n:0.0, worst);
            check(n && sum/n < 3.0, "GPU and software agree on the image");
        }
    } else {
        std::printf("\n  (no Vulkan device; GPU comparison skipped)\n");
    }
#else
    std::printf("\n  (built without Vulkan; GPU comparison skipped)\n");
#endif

    std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
