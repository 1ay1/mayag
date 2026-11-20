// Probe: open a real Win32 window and measure what a user feels.
//
//   time to first frame   process start -> first frame presented (revealed)
//   refresh               what the window reports to the host's frame clock
//   frame cost            present() wall time, vsync off, every frame unique
//
// MAYAG_BACKEND=software forces the CPU path; MAYAG_PRESENT=fifo|immediate
// picks the present mode.
#include <mayag/app/platform.hpp>
#include <mayag/render/draw_list.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <windows.h>

static double since_process_start_ms() {
    FILETIME c{}, e{}, k{}, u{};
    ::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u);
    FILETIME now{};
    ::GetSystemTimePreciseAsFileTime(&now);
    const auto q = [](FILETIME f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(q(now) - q(c)) / 10'000.0;
}

int main() {
    using namespace mayag;

    platform::WindowConfig cfg{};
    cfg.title = "mayag probe";
    cfg.size = Vec2{1280, 800};
    auto opened = platform::Native::open(cfg);
    if (!opened) { std::printf("FAIL: could not open a window\n"); return 1; }
    auto& win = *opened;
    const double t_open = since_process_start_ms();

    DrawList dl;
    const auto build = [&](float phase) -> const DrawList& {
        dl.clear();
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 80; ++x) {
                const float t = phase + static_cast<float>(x + y) * 0.05f;
                dl.fill_rect(Rect{{x * 16.0f, y * 20.0f}, {15.0f, 19.0f}},
                             Color<Srgb>{0.1f + 0.4f * (0.5f + 0.5f * std::sin(t)), 0.2f, 0.35f, 1.0f});
            }
        return dl;
    };
    const Color<Srgb> bg{0.05f, 0.05f, 0.08f, 1.0f};

    (void)win.dispatch();
    win.present(build(0.0f), bg);
    const double t_first = since_process_start_ms();
    const std::string first_renderer{win.renderer_name()};

    // Keep presenting (as an app would) until the GPU takes over.
    double t_gpu = -1.0;
    const bool want_gpu = std::getenv("MAYAG_BACKEND") == nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (want_gpu && !win.gpu_active() && std::chrono::steady_clock::now() < deadline) {
        (void)win.dispatch();
        win.present(build(0.5f), bg);   // identical: cheap, and still polls adoption
        ::Sleep(1);
    }
    if (win.gpu_active()) t_gpu = since_process_start_ms();

    std::printf("renderer        : %s (first frame on %s)\n", std::string(win.renderer_name()).c_str(),
                first_renderer.c_str());
    if (const char* dn = win.device_name()) std::printf("device          : %s\n", dn);
    std::printf("size            : %.0fx%.0f @ %.2fx\n", win.size().x, win.size().y, win.dpi_scale());
    if (auto t = win.display_timing())
        std::printf("refresh         : %.2f Hz (from DWM / monitor mode)\n",
                    1e9 / static_cast<double>(t->period.count()));
    else
        std::printf("refresh         : unknown (host falls back to 60 Hz)\n");
    std::printf("window open     : %.1f ms after process start\n", t_open);
    std::printf("first frame     : %.1f ms after process start\n", t_first);
    if (t_gpu >= 0) std::printf("gpu took over   : %.1f ms after process start\n", t_gpu);

    for (int i = 0; i < 20; ++i) { (void)win.dispatch(); win.present(build(i * 0.1f), bg); }

    constexpr int kFrames = 600;
    std::vector<double> ms;
    ms.reserve(kFrames);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        (void)win.dispatch();
        const auto a = std::chrono::steady_clock::now();
        win.present(build(static_cast<float>(i) * 0.03f), bg);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::sort(ms.begin(), ms.end());
    std::printf("frames          : %d of 3200 quads in %.2fs (%.0f fps)\n", kFrames, total, kFrames / total);
    std::printf("present p50/p99 : %.3f / %.3f ms   max %.3f ms\n",
                ms[ms.size() / 2], ms[ms.size() * 99 / 100], ms.back());

    // An identical frame must cost nothing: it is skipped, not re-rendered.
    const auto& same = build(1.0f);
    win.present(same, bg);
    const auto s0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) win.present(same, bg);
    std::printf("identical frame : %.4f ms each (skipped)\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count() / 1000.0);
    return 0;
}
