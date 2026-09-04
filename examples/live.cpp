// live.cpp — a real-time monitor built on Cmd::stream
//
// The point of this example is the shape of a LIVE app: data arrives from a
// source that is not the window — here a background thread sampling the
// system — and the UI updates as it flows in, while an idle frame still costs
// nothing. That is what `Cmd::stream` plus the cross-thread waker buy you: the
// producer runs on its own thread, each sample wakes the UI thread, and
// between samples the app blocks at 0% CPU.
//
// Everything else is the same pure Model/Msg/update/view mayag always is. The
// only new idea is one Cmd.
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

    static std::pair<Model, Cmd<Msg>> init() {
        return {Model{.streaming = true}, Cmd<Msg>::set_title("mayag — live")};
    }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        return std::visit([&](auto&& e) -> std::pair<Model, Cmd<Msg>> {
            using T = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<T, Sample>) {
                m.current = num::clamp(e.load, 0.0f, 1.0f);
                m.history[m.head] = m.current;
                m.head = (m.head + 1) % kHistory;
                if (m.count < kHistory) ++m.count;
                m.peak = num::max(m.peak, m.current);
                ++m.samples;
                return {m, Cmd<Msg>::none()};
            }
            else {
                return {m, Cmd<Msg>::quit()};
            }
        }, msg);
    }

    static Node view(const Model& m, const Ctx& c) {
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

    static Sub<Msg> subscribe(const Model& m) {
        // No frame subscription: the app is NOT animating. It renders only when
        // a sample arrives, and blocks at 0% CPU in between — which is exactly
        // the property that makes a streaming app cheap.
        //
        // The producer is a DECLARATIVE Sub::source: it runs precisely while
        // this subscription is returned, so "stream while m.streaming" needs no
        // start/stop plumbing in update() and leaks no thread when the flag
        // flips. The runtime starts it on first appearance and joins it when it
        // vanishes (here, at shutdown).
        auto feed = Sub<Msg>::source("cpu", [](std::function<void(Msg)> sink,
                                               std::function<bool()> alive) {
            CpuSampler sampler;
            sampler.sample();   // prime the delta
            while (alive()) {
                std::this_thread::sleep_for(100ms);
                if (alive()) sink(Sample{static_cast<float>(sampler.sample())});
            }
        });

        return Sub<Msg>::batch(
            m.streaming ? std::move(feed) : Sub<Msg>::none(),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_close(Quit{}));
    }
};

static_assert(Program<Live>, "Live must satisfy the Program concept");

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
        ok(rt.model().samples >= 3, "background samples arrived via Sub::source ("
                                    + std::to_string(rt.model().samples) + ")");
        ok(rt.model().count > 0, "the history filled from streamed data");
        // Runtime destructor joins the producer; if it hung, this line never
        // prints.
        std::printf("live: %d failure(s)\n", fails);
        (void)ok;
    });
}
