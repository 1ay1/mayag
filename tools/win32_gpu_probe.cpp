// Probe: open a real Win32 window and report which renderer won, then
// measure present() throughput on whichever path is active.
#include <mayag/app/platform.hpp>
#include <mayag/render/draw_list.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

int main() {
    using namespace mayag;

    platform::WindowConfig cfg{};
    cfg.title = "mayag gpu probe";
    cfg.size = Vec2{1280, 800};
    auto opened = platform::Native::open(cfg);
    if (!opened) {
        std::printf("FAIL: could not open a window\n");
        return 1;
    }
    auto& win = *opened;

    std::printf("backend   : %s\n", platform::native_name().data());
    std::printf("renderer  : %s\n", std::string(win.renderer_name()).c_str());
    std::printf("gpu_active: %s\n", win.gpu_active() ? "YES" : "no");
    std::printf("size      : %.0fx%.0f @ %.2fx dpi\n", win.size().x, win.size().y, win.dpi_scale());

    // A draw list with a realistic amount of work: a grid of quads, like a
    // terminal's cell backgrounds plus glyphs.
    DrawList dl;
    const auto build = [&](float phase) -> const DrawList& {
        dl.clear();
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 80; ++x) {
                const float fx = static_cast<float>(x) * 16.0f;
                const float fy = static_cast<float>(y) * 20.0f;
                const float t = phase + static_cast<float>(x + y) * 0.05f;
                dl.fill_rect(Rect{{fx, fy}, {15.0f, 19.0f}},
                             Color<Srgb>{0.1f + 0.4f * (0.5f + 0.5f * std::sin(t)), 0.2f, 0.35f, 1.0f});
            }
        return dl;
    };

    // Warm up: first frames pay for pipeline/swapchain lazies.
    for (int i = 0; i < 10; ++i) {
        (void)win.dispatch();
        win.present(build(static_cast<float>(i) * 0.1f), Color<Srgb>{0.05f, 0.05f, 0.08f, 1.0f});
    }

    constexpr int kFrames = 300;
    const auto t0 = std::chrono::steady_clock::now();
    double worst = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        (void)win.dispatch();
        const auto a = std::chrono::steady_clock::now();
        // Phase changes every frame so no frame is ever skipped as identical.
        win.present(build(static_cast<float>(i) * 0.03f), Color<Srgb>{0.05f, 0.05f, 0.08f, 1.0f});
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        if (ms > worst) worst = ms;
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\n%d frames of 3200 quads in %.2fs\n", kFrames, total);
    std::printf("  mean  %.3f ms/frame  (%.0f fps)\n", total / kFrames * 1000.0, kFrames / total);
    std::printf("  worst %.3f ms\n", worst);
    std::printf("  presented %llu\n", static_cast<unsigned long long>(win.frames_presented()));
    return 0;
}
