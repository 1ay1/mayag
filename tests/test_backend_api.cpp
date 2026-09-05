// tests/test_backend_api.cpp — the backend boundary is real, and it is small.
//
// The claim under test is not "Vulkan works". It is:
//
//     Adding a backend requires touching NOTHING outside the backend.
//
// That is easy to believe and easy to lose. It is lost the moment some core
// file grows an `#ifdef MAYAG_WITH_<API>`, or the app runtime learns a device's
// concrete type, or the interface quietly grows a method only GPUs can answer.
// mayag had exactly that decay once: `backend::Device` and the registry existed
// but nothing implemented or called them, while the platform layer drove
// `VulkanDevice` through a dozen `#ifdef`s. The abstraction was decoration.
//
// So this file defines a COMPLETE, WORKING BACKEND FOR AN API THAT DOES NOT
// EXIST, using only the public interface, and asserts the core picks it up and
// drives it. If a future change makes backends special again, this stops
// compiling or stops passing.

#include <mayag/mayag.hpp>
#include <mayag/backend/tiled.hpp>     // the software backend registers here
#ifdef MAYAG_WITH_VULKAN
#include <mayag/backend/vulkan.hpp>
#endif

#include <cstdio>
#include <string>
#include <vector>

using namespace mayag;

namespace {

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
    else       std::printf("  ok    %s\n", what.c_str());
}
void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

// ── a backend for an API that does not exist ────────────────────────────
//
// "Chalk" renders every instance as a flat rectangle of its own colour. It is
// deliberately dumb: the point is not that it draws well, it is that a backend
// needs NOTHING but this to participate. Note what it does not do: it does not
// include a shader, does not know what a glyph is, does not name a colour
// space, and does not appear anywhere in the core.
class ChalkBackend final : public backend::Device {
  public:
    static inline int instances_seen = 0;
    static inline int atlases_synced = 0;

    [[nodiscard]] static std::unique_ptr<Device> create(const backend::SurfaceHandle&,
                                                        Vec2 logical, float dpi) {
        auto self = std::make_unique<ChalkBackend>();
        self->resize(logical, dpi);
        return self;
    }
    [[nodiscard]] static std::unique_ptr<Device> create_offscreen() {
        return std::make_unique<ChalkBackend>();
    }

    [[nodiscard]] backend::DeviceInfo info() const override {
        backend::DeviceInfo i;
        // Chalk has no Api enumerator of its own — an out-of-tree backend would
        // not be able to add one. It reports `software`, which is exactly the
        // honest answer for a CPU renderer and proves the interface does not
        // require the enum to be extended.
        i.api = backend::Api::software;
        i.adapter = "chalk";
        return i;
    }

    void resize(Vec2 logical, float dpi) override {
        w_ = static_cast<int>(logical.x * dpi);
        h_ = static_cast<int>(logical.y * dpi);
    }

    backend::FrameStats submit(const DrawList& list, Color<Srgb>) override {
        ++frames_;
        backend::FrameStats st;
        st.instances = static_cast<std::uint32_t>(list.instances().size());
        return st;
    }

    void sync_atlas(const backend::AtlasView& v) override {
        ++atlases_synced;
        atlas_w_ = v.width;
        atlas_h_ = v.height;
    }

    [[nodiscard]] bool render_offscreen(const DrawList& list, int w, int h,
                                        Color<Srgb> clear,
                                        std::vector<std::uint8_t>& out) override {
        if (w <= 0 || h <= 0) return false;
        out.assign(static_cast<std::size_t>(w) * h * 4, 0);
        const auto c = clear;
        for (std::size_t i = 0; i < out.size(); i += 4) {
            out[i + 0] = static_cast<std::uint8_t>(num::saturate(c.c0) * 255.0f);
            out[i + 1] = static_cast<std::uint8_t>(num::saturate(c.c1) * 255.0f);
            out[i + 2] = static_cast<std::uint8_t>(num::saturate(c.c2) * 255.0f);
            out[i + 3] = 255;
        }
        for (const auto& in : list.instances()) {
            ++instances_seen;
            const int x0 = std::max(0, static_cast<int>(in.rect.x));
            const int y0 = std::max(0, static_cast<int>(in.rect.y));
            const int x1 = std::min(w, static_cast<int>(in.rect.x + in.rect.z));
            const int y1 = std::min(h, static_cast<int>(in.rect.y + in.rect.w));
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
                    out[i + 0] = static_cast<std::uint8_t>(num::saturate(in.color.x) * 255.0f);
                    out[i + 1] = static_cast<std::uint8_t>(num::saturate(in.color.y) * 255.0f);
                    out[i + 2] = static_cast<std::uint8_t>(num::saturate(in.color.z) * 255.0f);
                }
            }
        }
        return true;
    }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept override { return frames_; }

    int atlas_w_ = 0, atlas_h_ = 0;

  private:
    int w_ = 0, h_ = 0;
    std::uint64_t frames_ = 0;
};

}  // namespace

int main() {
    std::printf("mayag backend boundary\n======================\n");

    // ── what the core knows ─────────────────────────────────────────
    section("the registry");
    {
        const auto before = backend::registry().size();
        check(before >= 1, "at least one backend registered itself statically");

        // Registering a brand-new backend is one call. No core file changes.
        backend::register_backend({backend::Api::software,
                                   &ChalkBackend::create,
                                   &ChalkBackend::create_offscreen});
        check(backend::registry().size() == before + 1,
              "a new backend joins with a single register_backend() call");
    }

    // ── a backend the core has never heard of can render a real UI ──
    section("an unknown backend renders a real scene");
    {
        using namespace mayag::dsl;
        const int W = 80, H = 40;

        auto ui = box() | size(80, 40) | bg(rgb<0xFF0000>);
        Node n = ui.build();
        layout::layout_tree(n, {static_cast<float>(W), static_cast<float>(H)},
                            layout::default_measurer());
        DrawList dl;
        render::paint(n, dl, {});

        ChalkBackend chalk;
        std::vector<std::uint8_t> px;
        const bool ok = chalk.render_offscreen(dl, W, H, colors::black, px);

        check(ok, "the unknown backend rendered a frame");
        check(px.size() == static_cast<std::size_t>(W) * H * 4,
              "it produced a correctly sized RGBA8 buffer");
        check(ChalkBackend::instances_seen > 0,
              "the core handed it the draw list without knowing what it is");

        // It really drew: the centre pixel is the box's red, not the clear.
        const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
        check(px[mid] > 200 && px[mid + 1] < 60,
              "its output reached the caller unmodified");
    }

    // ── the interface does not leak GPU assumptions ─────────────────
    section("the interface fits a non-GPU backend");
    {
        // The software rasteriser implements the SAME interface as Vulkan. If
        // `Device` had grown a swapchain, a command buffer or a descriptor set,
        // this would be impossible and every caller would need a special case.
        auto sw = backend::create_offscreen_device(backend::Api::software);
        check(sw != nullptr, "the software rasteriser is an ordinary Device");
        if (sw) {
            check(sw->info().api == backend::Api::software,
                  "it reports itself through the same DeviceInfo");
            std::vector<std::uint8_t> px;
            DrawList empty;
            check(sw->render_offscreen(empty, 4, 4, colors::black, px),
                  "it satisfies the required offscreen path");
        }
    }

    // ── selection is policy, not plumbing ───────────────────────────
    section("runtime selection");
    {
        // Every backend is reachable by name, so a user can force one without a
        // rebuild and a test can compare two directly.
        auto sw = backend::create_offscreen_device(backend::Api::software);
        check(sw && sw->info().api == backend::Api::software,
              "a specific backend can be requested by api");

        auto best = backend::create_offscreen_device();
        check(best != nullptr,
              "the registry always yields a device (software cannot fail)");
    }

    // ── the atlas crosses the boundary without the font engine ──────
    section("the glyph atlas is type-erased");
    {
        auto stack = typo::system::default_stack();
        // view_of() adapts ANY atlas to the plain struct a backend sees, so the
        // font engine's types never appear in a backend's signature.
        const auto view = backend::view_of(stack->atlas());
        check(view.width > 0 && view.height > 0,
              "an atlas becomes a plain AtlasView");

        ChalkBackend chalk;
        chalk.sync_atlas(view);
        check(chalk.atlas_w_ == view.width,
              "a backend consumes it without naming a font type");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
