// Time each part of a swapchain resize on this machine's driver.
#include <mayag/app/platform.hpp>
#include <mayag/render/draw_list.hpp>
#include <chrono>
#include <cstdio>
#include <windows.h>
#include <psapi.h>

int main() {
    using namespace mayag;
    using clk = std::chrono::steady_clock;
    platform::WindowConfig cfg{};
    cfg.size = Vec2{1000, 700};
    auto w = platform::Native::open(cfg);
    if (!w) return 1;
    DrawList dl;
    dl.fill_rect(Rect{{10, 10}, {200, 100}}, Color<Srgb>{0.3f, 0.4f, 0.8f, 1.0f});
    const Color<Srgb> bg{0.05f, 0.05f, 0.08f, 1.0f};
    while (!w->gpu_active()) { (void)w->dispatch(); w->present(dl, bg); ::Sleep(5); }
    for (int i = 0; i < 10; ++i) { (void)w->dispatch(); w->present(dl, bg); }

    HWND hwnd = ::FindWindowW(L"mayag.window", nullptr);
    double sum = 0, worst = 0;
    const int N = 60;
    for (int i = 0; i < N; ++i) {
        ::SetWindowPos(hwnd, nullptr, 100, 100, 700 + (i % 2) * 300 + i, 500 + (i % 3) * 50, SWP_NOZORDER | SWP_NOACTIVATE);
        (void)w->dispatch();    // marks the swapchain for rebuild
        const auto a = clk::now();
        dl.clear();
        dl.fill_rect(Rect{{10.f + i, 10}, {200, 100}}, Color<Srgb>{0.3f, 0.4f, 0.8f, 1.0f});
        w->present(dl, bg);     // the rebuild + first frame at the new size
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - a).count();
        sum += ms;
        if (ms > worst) worst = ms;
    }
    std::printf("resize + present: mean %.2f ms, worst %.2f ms over %d steps\n", sum / N, worst, N);

    // Leak check: commit charge before/after many more resizes.
    const auto commit = [] {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof pmc;
        ::K32GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
        return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    };
    const double before = commit();
    for (int i = 0; i < 600; ++i) {
        ::SetWindowPos(hwnd, nullptr, 100, 100, 700 + (i % 7) * 40, 500 + (i % 5) * 30, SWP_NOZORDER | SWP_NOACTIVATE);
        (void)w->dispatch();
        w->present(dl, bg);
    }
    std::printf("private bytes   : %.1f MB -> %.1f MB after 600 more resizes\n", before, commit());
}
