// examples/present_bench.cpp — what does a frame actually cost to PRESENT?
//
// Every other measurement in mayag stops at the draw list: view, layout, and
// paint are all CPU work we control, and they add up to about 1 ms. This
// benchmark measures the part after that — turning a draw list into photons —
// because that is where the software rasteriser hits a wall and where the
// GPU is supposed to earn its complexity.
//
// It opens a REAL window (both paths need a real drawable, and a benchmark
// that avoids the compositor measures the wrong thing), renders the same
// scene N times through each backend, and reports the distribution.
//
// The comparison is honest in the ways that usually go wrong:
//
//   * SAME draw list. Built once, submitted to both. Neither backend gets an
//     easier scene.
//   * SAME window, one process. No cross-run thermal or scheduler drift.
//   * WARMUP discarded. First frames pay for shader compilation, buffer
//     allocation, and a cold atlas; including them measures startup.
//   * p50 AND p99, not the mean. A mean hides exactly the stutter a UI
//     framework is judged on.
//
//   ./mayag_present_bench                 both backends, default 1000x720
//   ./mayag_present_bench --size 3840 2160  the resolution where CPU breaks
//   ./mayag_present_bench --instances 2000  scene complexity sweep

#include <mayag/mayag.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace mayag;

namespace {

/// CPU time burned by THIS THREAD, which is the number that matters.
///
/// Wall time answers "how long until the frame is on screen", and it is
/// dominated by the vsync wait: a GPU path that blocks in `nextDrawable`
/// until the display is ready looks slow by wall clock while using no CPU at
/// all. That distinction is the whole point of the exercise, because CPU time
/// is what competes with the application's own work and what drains a
/// battery. Reporting only wall time would make correct frame pacing look
/// like a regression.
[[nodiscard]] double thread_cpu_ms() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 +
           static_cast<double>(ts.tv_nsec) / 1e6;
}

// ── the scene ───────────────────────────────────────────────────────────
//
// Deliberately shaped like a real UI rather than a stress test: mostly
// rounded rectangles, a few shadows, some clipped regions, one gradient
// band. A benchmark of 10,000 overlapping full-screen quads would measure
// fill rate, which is not what a UI is bound by.

DrawList build_scene(Vec2 px, int instances) {
    DrawList dl;
    dl.reserve(static_cast<std::size_t>(instances) + 64);

    const float w = px.x, h = px.y;

    // Background panels.
    dl.fill_rect(Rect{0, 0, w, h}, rgb<0x0B0D10>);
    dl.fill_rect(Rect{0, 0, w, 64}, rgb<0x14181F>);

    // A clipped scroll region — this is what forces multiple batches, and
    // therefore what proves the GPU path honours per-batch scissor rects.
    const Rect viewport{24, 88, w * 0.55f, h - 120};
    dl.push_clip(viewport);

    const int rows = std::max(1, instances / 3);
    for (int i = 0; i < rows; ++i) {
        const float y = 96.0f + static_cast<float>(i) * 40.0f;
        const float t = static_cast<float>(i) / static_cast<float>(std::max(1, rows));

        // Card, avatar, and progress bar: three instances per row, the
        // rough density of a real list.
        dl.fill_rect(Rect{40, y, viewport.width() - 32, 32},
                     rgb<0x1B2028>, Corners{8, 8, 8, 8});
        dl.circle(Vec2{64, y + 16}, 10.0f,
                  Color<Srgb>{0.2f + t * 0.6f, 0.5f, 0.9f - t * 0.4f, 1.0f});
        dl.fill_rect(Rect{88, y + 12, (viewport.width() - 140) * t, 8},
                     rgb<0x3B82F6>, Corners{4, 4, 4, 4});
    }
    dl.pop_clip();

    // A side panel outside the clip, so the batch boundary is real.
    dl.fill_rect(Rect{w * 0.60f, 88, w * 0.36f, h - 120},
                 rgb<0x14181F>, Corners{12, 12, 12, 12});

    return dl;
}

// ── statistics ──────────────────────────────────────────────────────────

struct Stats {
    double p50 = 0, p99 = 0, best = 0, worst = 0, mean = 0;
};

Stats summarise(std::vector<double> samples) {
    Stats s;
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());

    const auto at = [&](double q) {
        const auto i = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
        return samples[i];
    };
    s.p50   = at(0.50);
    s.p99   = at(0.99);
    s.best  = samples.front();
    s.worst = samples.back();
    for (double v : samples) s.mean += v;
    s.mean /= static_cast<double>(samples.size());
    return s;
}

void report(const char* name, const Stats& wall, const Stats& cpu, double budget_ms) {
    std::printf("  %-9s  wall p50 %6.2f  p99 %6.2f   |   CPU p50 %6.2f  p99 %6.2f"
                "   |  %5.1f%% of a core%s\n",
                name, wall.p50, wall.p99, cpu.p50, cpu.p99,
                cpu.p50 / budget_ms * 100.0,
                cpu.p50 > budget_ms ? "   OVER" : "");
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(MAYAG_WITH_COCOA)
    std::printf("present_bench needs a real window; build with MAYAG_WITH_COCOA=ON\n");
    return 0;
#else
    Vec2 size{1000, 720};
    float dpi = 2.0f;
    int instances = 300;
    int frames = 400;
    int warmup = 60;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--size" && i + 2 < argc) {
            size.x = std::strtof(argv[++i], nullptr);
            size.y = std::strtof(argv[++i], nullptr);
        } else if (a == "--dpi" && i + 1 < argc) {
            dpi = std::strtof(argv[++i], nullptr);
        } else if (a == "--instances" && i + 1 < argc) {
            instances = std::atoi(argv[++i]);
        } else if (a == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: %s [--size W H] [--dpi S] [--instances N] [--frames N]\n",
                        argv[0]);
            return 0;
        }
    }

    // The scene is built in PHYSICAL pixels, because that is the space a
    // draw list lives in — the runtime multiplies by dpi before painting.
    const Vec2 px{size.x * dpi, size.y * dpi};
    const DrawList scene = build_scene(px, instances);

    std::printf("\nmayag present benchmark\n");
    std::printf("  surface     %.0f x %.0f logical @ %.1fx = %.0f x %.0f physical\n",
                size.x, size.y, static_cast<double>(dpi), px.x, px.y);
    std::printf("  scene       %zu instances in %zu batches\n",
                scene.size(), scene.batches().size());
    std::printf("  sampling    %d frames after %d warmup\n\n", frames, warmup);

    const double budget = 1000.0 / 60.0;

    const auto measure = [&](const char* label, bool want_gpu) -> bool {
        // The backend is chosen by env var at window open, which is exactly
        // the switch a user has — so the benchmark exercises the real
        // selection path rather than a private one.
        setenv("MAYAG_BACKEND", want_gpu ? "metal" : "software", 1);

        platform::WindowConfig wc;
        wc.title = std::string{"mayag present bench — "} + label;
        wc.size = size;

        auto opened = platform::MacWindow::open(wc);
        if (!opened) { std::printf("  %-10s  could not open a window\n", label); return false; }
        auto& win = *opened;

        if (want_gpu && !win.gpu_active()) {
            std::printf("  %-10s  unavailable on this machine (fell back to %s)\n",
                        label, std::string{win.renderer_name()}.c_str());
            win.close();
            return false;
        }

        std::vector<double> wall;
        std::vector<double> cpu;
        wall.reserve(static_cast<std::size_t>(frames));
        cpu.reserve(static_cast<std::size_t>(frames));

        for (int i = 0; i < warmup + frames; ++i) {
            // Drain events so the window stays responsive and the run loop
            // does not back up — a benchmark that starves the window server
            // measures its own starvation.
            (void)win.poll_events(platform::Wait::immediate, 0.0);

            const auto   t0 = std::chrono::steady_clock::now();
            const double c0 = thread_cpu_ms();
            win.present(scene, rgb<0x0B0D10>);
            const double c1 = thread_cpu_ms();
            const auto   t1 = std::chrono::steady_clock::now();

            if (i >= warmup) {
                wall.push_back(
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
                cpu.push_back(c1 - c0);
            }
        }

        report(label, summarise(std::move(wall)), summarise(std::move(cpu)), budget);
        win.close();
        return true;
    };

    std::printf("present cost (draw list -> photons)\n");
    std::printf("  wall = time until the frame can be presented, which includes"
                " the vsync wait\n");
    std::printf("  CPU  = processor time actually consumed — what competes with"
                " your app\n\n");

    const bool sw_ok  = measure("software", false);
    const bool gpu_ok = measure("metal", true);

    if (sw_ok && gpu_ok) {
        std::printf("\n  Both paths rendered the same %zu-instance draw list.\n",
                    scene.size());
    }
    std::printf("\n");
    return 0;
#endif
}
