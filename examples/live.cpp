// live.cpp — a real-time monitor built on a Sub::stream
//
// The point of this example is the shape of a LIVE app: data arrives from a
// source that is not the window — here a background thread sampling the
// system — and the UI updates as it flows in, while an idle frame still costs
// nothing. The producer runs on its own thread, each sample wakes the
// loop, and between samples the app sleeps at 0% CPU.
//
// Everything else is the same pure Model/Msg/update/view mayag always is. The
// only new idea is one Sub::stream.
//
//   window:   ./mayag_live
//   headless: ./mayag_live --headless    (streams a few samples, asserts, exits)

#include <mayag/mayag.hpp>

#include "harness.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>

using namespace mayag;
using namespace mayag::dsl;
using namespace std::chrono_literals;

namespace {

// Sample total CPU busy fraction from /proc/stat (Linux). Returns a value in
// [0,1], or a synthetic sine when /proc is unavailable, so the example is
// live on every platform.
struct CpuSampler {
    unsigned long long prev_idle = 0, prev_total = 0;
    double phase = 0.0;

    double sample() {
        std::ifstream f("/proc/stat");
        std::string cpu;
        if (f >> cpu && cpu == "cpu") {
            unsigned long long v, idle = 0, total = 0;
            std::array<unsigned long long, 10> fields{};
            int n = 0;
            while (n < 10 && (f >> v)) { fields[static_cast<std::size_t>(n)] = v; ++n; }
            for (int i = 0; i < n; ++i) total += fields[static_cast<std::size_t>(i)];
            idle = fields[3] + (n > 4 ? fields[4] : 0);   // idle + iowait
            const auto dt = total - prev_total;
            const auto di = idle - prev_idle;
            prev_total = total; prev_idle = idle;
            if (dt == 0) return 0.0;
            return 1.0 - static_cast<double>(di) / static_cast<double>(dt);
        }
        // Fallback: a smooth synthetic load.
        phase += 0.25;
        return 0.5 + 0.45 * static_cast<double>(num::sin(static_cast<float>(phase)));
    }
};

}  // namespace

struct Live {
    static constexpr std::size_t kHistory = 64;

    struct Model {
        std::array<float, kHistory> history{};   // ring of recent samples
        std::size_t                 head = 0;
        std::size_t                 count = 0;
        float                       current = 0.0f;
        float                       peak = 0.0f;
        std::uint64_t               samples = 0;
        bool                        streaming = false;
    };

    // The one interesting message: a sample arrived from the background thread.
    struct Sample { float load; };
    struct Quit {};
    using Msg = std::variant<Sample, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model& m) { m.streaming = true; return SetTitle{"mayag — live"}; }

    static Cmd update(Model& m, const Sample& e) {
        m.current = num::clamp(e.load, 0.0f, 1.0f);
        m.history[m.head] = m.current;
        m.head = (m.head + 1) % kHistory;
        if (m.count < kHistory) ++m.count;
        m.peak = num::max(m.peak, m.current);
        ++m.samples;
        return {};
    }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    static Node view(const Model& m) {
        const Ctx& c = ctx();
        const Theme& t = c.theme;

        // A live bar chart of the sample history, oldest to newest. Each bar's
        // colour shifts from calm to hot with load — pure function of state.
        auto bars = [&] {
            std::vector<Node> cols;
            cols.reserve(kHistory);
            for (std::size_t i = 0; i < kHistory; ++i) {
                // Walk the ring oldest-first.
                const std::size_t idx =
                    (m.head + kHistory - m.count + i) % kHistory;
                const float v = (i < m.count) ? m.history[idx] : 0.0f;
                const float h = 4.0f + v * 116.0f;
                const Color<Srgb> tone =
                    v > 0.85f ? t.danger :
                    v > 0.6f  ? t.warning :
                    v > 0.3f  ? t.accent  : t.success;
                cols.push_back(
                    box() | size(5, h) | bg(tone.fade(0.35f + 0.65f * v))
                          | radius(2));
            }
            return cols;
        }();

        auto chart =
            list(Axis::horizontal, std::move(bars), 2.0f)
                 | align(Align::end)
                 | size(kHistory * 7.0f, 128)
                 | pad(8) | bg(t.surface) | radius(t.radius_medium)
                 | border(1, t.border);

        const int pct_now  = static_cast<int>(m.current * 100.0f + 0.5f);
        const int pct_peak = static_cast<int>(m.peak * 100.0f + 0.5f);

        auto stat = [&](std::string label, std::string value, Color<Srgb> tone) {
            return v(text_owned(std::move(value)) | font(28) | bold | fg(tone),
                     text_owned(std::move(label)) | font(10) | fg(t.text_secondary)
                                                   | tracking(1.5f))
                 | gap(2);
        };

        return v(h(text<"LIVE CPU"> | font(12) | semibold | fg(t.text_secondary)
                                    | tracking(3.0f),
                   spacer(),
                   text_owned(m.streaming ? "streaming" : "idle")
                       | font(10) | fg(m.streaming ? t.success : t.text_disabled))
                 | width(kHistory * 7.0f),
                 chart,
                 h(stat("NOW",  std::to_string(pct_now) + "%",  t.text_primary),
                   spacer(),
                   stat("PEAK", std::to_string(pct_peak) + "%", t.warning),
                   spacer(),
                   stat("SAMPLES", std::to_string(m.samples), t.text_secondary))
                 | width(kHistory * 7.0f))
             | gap(16) | pad(28) | center
             | width(pct(100)) | height(pct(100)) | bg(t.background);
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (k.key == Key::escape) return Quit{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    static Sub subscribe(const Model& m) {
        // No frame subscription: the app is NOT animating. It renders only when
        // a sample arrives, and blocks at 0% CPU in between.
        //
        // The producer is a Sub::stream: mayag runs it exactly while this
        // subscription is returned and stops it (via the stop_token) when it
        // vanishes, so "stream while m.streaming" needs no start/stop plumbing.
        return when(m.streaming, Sub::stream("cpu",
            [](mayag::Sink<Msg> out, std::stop_token st) {
                CpuSampler sampler;
                sampler.sample();   // prime the delta
                while (!st.stop_requested()) {
                    std::this_thread::sleep_for(100ms);
                    if (!st.stop_requested())
                        out.send(Sample{static_cast<float>(sampler.sample())});
                }
            }));
    }
};

static_assert(mayag::Program<Live>, "Live must satisfy the Program concept");

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{520, 300});
    auto fonts = demo::make_fonts(opts);
    demo::print_fonts(fonts.get());

    AppConfig cfg{
        .title = "mayag — live",
        .size  = opts.size,
        .theme = themes::midnight,
        .fonts = fonts,
        .debug_bounds = opts.debug_bounds,
    };

    return demo::run<Live>(opts, cfg, [](auto& rt) {
        std::printf("live headless\n");
        int fails = 0;
        const auto ok = [&](bool c, std::string_view what) {
            std::printf("  %s  %.*s\n", c ? "ok  " : "FAIL",
                        static_cast<int>(what.size()), what.data());
            if (!c) ++fails;
        };

        // Pump the runtime until the stream has delivered a few samples. This
        // exercises the real streaming path: a background thread feeding
        // update() through the inbox, driven by the same loop the window uses.
        for (int i = 0; i < 200 && rt.model().samples < 3; ++i) {
            std::this_thread::sleep_for(5ms);
            rt.tick();
        }

        ok(rt.model().streaming, "the stream started");
        ok(rt.model().samples >= 3, "background samples arrived via Sub::stream ("
                                    + std::to_string(rt.model().samples) + ")");
        ok(rt.model().count > 0, "the history filled from streamed data");
        // Destroying the app stops and joins the producer; if it hung, this
        // line never prints.
        std::printf("live: %d failure(s)\n", fails);
        (void)ok;
    });
}
