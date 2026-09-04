// examples/reactive.cpp — a highly reactive app, with its latency on screen
//
// The claim "mayag is low latency" is worth nothing without a number, so this
// example draws its own frame budget while you interact with it.
//
// What it demonstrates:
//   * a 60 Hz continuously-animating scene that still idles at 0% CPU when
//     you stop touching it
//   * pointer tracking with no perceptible lag — the cursor trail shows
//     exactly how far behind the render is
//   * input coalescing: a burst of trackpad events becomes ONE frame
//   * the whole budget, measured and displayed: view, layout, paint, render
//
//   ./mayag_reactive              a window
//   ./mayag_reactive --headless   asserts the latency budget in CI

#include "harness.hpp"

using namespace mayag;
using namespace mayag::dsl;

struct Reactive {
    // ── state ───────────────────────────────────────────────────────────

    static constexpr int trail_length = 24;
    static constexpr int bars = 48;

    struct Model {
        Vec2   cursor{};
        /// A short history of cursor positions. Each lags the one before it
        /// by a spring, so the visual gap between the head and the tail is a
        /// direct picture of the frame pipeline's depth.
        std::array<Vec2, trail_length> trail{};

        std::array<float, bars> spectrum{};
        double t = 0.0;

        /// Off by default.
        ///
        /// A demo that animates from launch pins a core forever and looks
        /// like a hang — which is exactly what it did. An app should be
        /// still until asked to move. Press SPACE.
        bool   running = false;

        /// Frames still owed after the pointer stopped, so the trail settles
        /// instead of freezing mid-flight. Counted DOWN, so the app returns
        /// to idle on its own rather than animating forever.
        int    settle_frames = 0;

        /// Rolling stats, copied out of the runtime each frame so the view
        /// can render them like any other model data.
        double cpu_ms = 0.0;
        double worst_ms = 0.0;
        int    coalesced = 0;
        int    missed = 0;
    };

    struct Tick { double dt; };
    struct Moved { Vec2 p; };
    struct Toggle {};
    struct Stats { double cpu; double worst; int coalesced; int missed; };
    struct Quit {};

    using Msg = std::variant<Tick, Moved, Toggle, Stats, Quit>;

    static std::pair<Model, Cmd<Msg>> init() {
        return {Model{}, Cmd<Msg>::set_title("mayag — reactive")};
    }

    // ── update ──────────────────────────────────────────────────────────

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        return std::visit([&](auto&& e) -> std::pair<Model, Cmd<Msg>> {
            using T = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<T, Moved>) {
                m.cursor = e.p;
                // Pointer motion earns a short burst of frames so the trail
                // can catch up, then the app goes quiet again. Without a
                // BUDGET this would animate forever after the first move.
                m.settle_frames = 45;
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Tick>) {
                m.t += e.dt;
                if (m.settle_frames > 0) --m.settle_frames;
                // The measurements the runtime just took are available on the
                // NEXT frame; a one-frame-old number is fine for a readout
                // and keeps the model a pure function of its messages.


                // The trail: each point chases the one ahead of it. A stiff
                // follow means the tail is only a few frames behind, which is
                // exactly what the eye reads as "responsive".
                const float k = num::saturate(static_cast<float>(e.dt) * 26.0f);
                m.trail[0] = lerp(m.trail[0], m.cursor, num::min(k * 2.0f, 1.0f));
                for (int i = 1; i < trail_length; ++i) {
                    m.trail[static_cast<std::size_t>(i)] =
                        lerp(m.trail[static_cast<std::size_t>(i)],
                             m.trail[static_cast<std::size_t>(i - 1)], k);
                }

                // A cheap synthetic spectrum, so the scene has real per-frame
                // work rather than being trivially fast.
                for (int i = 0; i < bars; ++i) {
                    const float phase = static_cast<float>(m.t) * 2.2f +
                                        static_cast<float>(i) * 0.37f;
                    const float env = 0.35f + 0.65f *
                        num::saturate(1.0f - num::abs(static_cast<float>(i) / bars - 0.5f) * 1.6f);
                    m.spectrum[static_cast<std::size_t>(i)] =
                        (num::sin(phase) * 0.5f + 0.5f) * env;
                }
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Toggle>) {
                m.running = !m.running;
                return {std::move(m), Cmd<Msg>::none()};
            }
            else if constexpr (std::is_same_v<T, Stats>) {
                m.cpu_ms = e.cpu; m.worst_ms = e.worst;
                m.coalesced = e.coalesced; m.missed = e.missed;
                return {std::move(m), Cmd<Msg>::none()};
            }
            else {
                return {std::move(m), Cmd<Msg>::quit()};
            }
        }, msg);
    }

    // ── view ────────────────────────────────────────────────────────────

    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = c.theme;

        // ---- the spectrum ----
        std::vector<Node> bar_nodes;
        bar_nodes.reserve(bars);
        for (int i = 0; i < bars; ++i) {
            const float v = m.spectrum[static_cast<std::size_t>(i)];
            const float h = 6.0f + v * 110.0f;
            const auto tone = mix_perceptual(t.accent, t.success, v);
            bar_nodes.push_back(
                (box() | size(6, h) | bg(tone.fade(0.35f + v * 0.65f)) | radius(3)).build());
        }

        // ---- the cursor trail ----
        //
        // Drawn as an overlay so it escapes every clip and always sits on
        // top: a latency indicator that can be occluded is useless.
        for (int i = trail_length - 1; i >= 0; --i) {
            const float f = static_cast<float>(i) / trail_length;
            const float r = 3.0f + (1.0f - f) * 9.0f;
            const Vec2 p = m.trail[static_cast<std::size_t>(i)];
            c.overlay(Overlay{
                .content = (box() | size(r * 2.0f, r * 2.0f)
                                  | bg(mix_perceptual(t.accent, t.danger, f)
                                        .fade(0.85f * (1.0f - f)))
                                  | radius(r)).build(),
                .id = node_id("trail-" + std::to_string(i)),
                .anchor_id = 0,
                .placement = Placement::cursor,
                .dismiss = Dismiss::never,
                .offset = 0.0f,
                .layer = trail_length - i,
            });
        }

        auto stat = [&](auto label, std::string value, Color<Srgb> tone) {
            return v(label | font(9) | fg(t.text_secondary) | tracking(1.2f),
                     text_owned(std::move(value)) | font(17) | bold | fg(tone))
                 | gap(1) | width(96);
        };

        // Read the runtime's own measurements. A view is still a pure
        // function of (model, ctx) — the timings are an input, like the theme.
        const double cpu_ms   = c.latency ? c.latency->mean_cpu_ms() : 0.0;
        const double worst_ms = c.latency ? c.latency->worst_cpu_ms() : 0.0;
        const int coalesced   = c.latency ? c.latency->last().coalesced_moves : 0;
        const int missed      = c.latency ? c.latency->missed_frames() : 0;
        const bool over_budget = cpu_ms > 16.6;

        return v(split(v(text<"Reactive"> | font(24) | bold | fg(t.text_primary)
                           | role(a11y::Role::heading),
                         text<"latency measured, not claimed"> | font(11)
                           | fg(t.text_secondary)) | gap(2),
                       (h(text_owned(m.running ? "running" : "paused") | font(11)
                            | semibold | fg(m.running ? t.on_accent : t.text_secondary))
                        | center | pad(6, 14)
                        | bg(m.running ? t.accent : t.surface_raised)
                        | border(1, m.running ? t.accent : t.border)
                        | pill
                        | role(a11y::Role::button) | label("Toggle animation")
                        | cursor(CursorShape::pointer)
                        | dsl::id<"toggle">)),

                 divider(t),

                 // The numbers. This is the point of the example.
                 h(stat(text<"CPU / FRAME">,
                        std::to_string(static_cast<int>(cpu_ms * 1000)) + " us",
                        over_budget ? t.danger : t.success),
                   stat(text<"WORST">,
                        std::to_string(static_cast<int>(worst_ms * 1000)) + " us",
                        worst_ms > 16.6 ? t.warning : t.text_primary),
                   stat(text<"COALESCED">, std::to_string(coalesced), t.text_primary),
                   stat(text<"MISSED">, std::to_string(missed),
                        missed > 0 ? t.danger : t.success))
                 | gap(10),

                 // Headroom against a 60 Hz refresh.
                 v(h(text<"60 Hz budget"> | font(9) | fg(t.text_secondary) | tracking(1.2f),
                     spacer(),
                     text_owned(std::to_string(static_cast<int>(
                         num::saturate(cpu_ms / 16.667) * 100.0)) + "%")
                       | font(9) | fg(t.text_secondary))
                   | width(pct(100)),
                   // A minimum width so the bar exists at frame zero, before
                   // any timing has been recorded — a zero-width node is
                   // invisible AND (correctly) flagged as collapsed.
                   z(box() | width(pct(num::max(
                                 num::saturate(static_cast<float>(cpu_ms) / 16.667f) * 100.0f,
                                 1.5f)))
                           | height(6)
                           | bg(over_budget ? t.danger : t.success) | radius(3))
                   | width(pct(100)) | height(6) | bg(t.border) | radius(3) | clip)
                 | gap(4) | width(pct(100)),

                 divider(t),

                 node((list(Axis::horizontal, std::move(bar_nodes), 2.0f)
                       | align(Align::end)
                       | height(130)
                       | width(pct(100))).build()),

                 h(text<"move the pointer"> | font(10) | fg(t.text_disabled),
                   spacer(),
                   text<"space"> | font(10) | fg(t.text_disabled),
                   text<"pause"> | font(10) | fg(t.text_disabled),
                   text<"esc"> | font(10) | fg(t.text_disabled))
                 | gap(6) | width(pct(100)))
             | gap(14) | pad(20)
             | width(pct(100)) | height(pct(100))
             | bg(t.background);
    }

    // ── subscribe ───────────────────────────────────────────────────────

    static Sub<Msg> subscribe(const Model& m) {
        return Sub<Msg>::batch(
            Sub<Msg>::on_event([](const Event& e) -> std::optional<Msg> {
                if (const auto* mv = std::get_if<MouseMove>(&e)) return Moved{mv->position};
                return std::nullopt;
            }),
            Sub<Msg>::on_click<"toggle">(Toggle{}),
            Sub<Msg>::on_key(Key::space, Toggle{}),
            Sub<Msg>::on_key(Key::escape, Quit{}),
            Sub<Msg>::on_close(Quit{}),
            // Frames are requested only while running — pause it and the app
            // drops to 0% CPU with no explicit stop.
            // Frames are requested while the spectrum is running OR the
            // trail is still catching up — and NOT otherwise, so an
            // untouched window sits at 0% CPU instead of looking hung.
            when(m.running || m.settle_frames > 0,
                 Sub<Msg>::every_frame([](FrameEvent f) { return Msg{Tick{f.delta}}; })));
    }

    /// Copy the runtime's measurements into the model so `view()` can render
    /// them like any other data. Called from the driver loop; a real app
    /// would do this in a `Cmd::perform` or simply not display them.
    static void pump_stats(Model& m, const LatencyStats& L) {
        m.cpu_ms    = L.mean_cpu_ms();
        m.worst_ms  = L.worst_cpu_ms();
        m.coalesced = L.last().coalesced_moves;
        m.missed    = L.missed_frames();
    }
};

static_assert(Program<Reactive>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{560, 480});
    auto fonts = demo::make_fonts(opts);
    demo::print_fonts(fonts.get());

    AppConfig cfg{
        .title = "mayag — reactive",
        .size  = opts.size,
        .theme = themes::midnight,
        .fonts = fonts,
        .debug_bounds = opts.debug_bounds,
    };

    return demo::run<Reactive>(opts, cfg, [](auto& rt) {
        std::printf("reactive headless\n");
        int fails = 0;
        const auto ok = [&](bool c, std::string_view what) {
            std::printf("  %s  %.*s\n", c ? "ok  " : "FAIL",
                        static_cast<int>(what.size()), what.data());
            if (!c) ++fails;
        };

        // ── input coalescing ────────────────────────────────────────────
        //
        // A trackpad burst must become ONE frame. Rendering each event is
        // more work AND higher latency, because the frame you see is several
        // events stale.
        const auto before = rt.window().frames_presented();
        for (int i = 0; i < 60; ++i) {
            rt.window().push(MouseMove{{100.0f + i, 200.0f}, {1, 0}, {}});
        }
        rt.tick();
        const auto frames = rt.window().frames_presented() - before;
        ok(frames == 1, "60 pointer moves coalesce into 1 frame (got " +
                        std::to_string(frames) + ")");
        ok(rt.latency().last().coalesced_moves >= 58,
           "and the superseded moves are dropped (" +
           std::to_string(rt.latency().last().coalesced_moves) + ")");

        // ── the budget ──────────────────────────────────────────────────
        rt.window().drive_frames(true);
        for (int i = 0; i < 200; ++i) {
            rt.window().push(MouseMove{{200.0f + static_cast<float>(i % 60),
                                        250.0f + static_cast<float>(i % 40)}, {1, 1}, {}});
            rt.tick();
        }
        rt.window().drive_frames(false);

        const auto& L = rt.latency();
        std::printf("  budget over %zu frames: mean %.3f ms, p99 %.3f ms, worst %.3f ms\n",
                    L.size(), L.mean_cpu_ms(), L.percentile_cpu_ms(0.99), L.worst_cpu_ms());

        // A continuously animating scene must fit a 60 Hz frame with room to
        // spare, or the app cannot also do the work it exists for.
        ok(L.mean_cpu_ms() < 8.0,
           "mean frame is under half the 60 Hz budget (" +
           std::to_string(L.mean_cpu_ms()) + " ms)");
        ok(L.percentile_cpu_ms(0.99) < 16.6,
           "p99 fits inside one refresh (" +
           std::to_string(L.percentile_cpu_ms(0.99)) + " ms)");
        ok(L.missed_frames() == 0,
           "no missed frames (" + std::to_string(L.missed_frames()) + ")");

        // ── idles unless something is moving ────────────────────────────
        //
        // The failure this guards: a demo that animates from launch pins a
        // core forever and looks exactly like a hang. It should be still
        // until asked to move.
        ok(!rt.model().running, "the app starts still, not animating");

        rt.window().press_key(Key::space);
        rt.tick();
        ok(rt.model().running, "space starts it");
        ok(Reactive::subscribe(rt.model()).wants_frames(),
           "and a running app requests frames");

        rt.window().press_key(Key::space);
        rt.tick();
        ok(!rt.model().running, "space stops it again");

        // Let any trail settle-budget expire.
        rt.window().drive_frames(true);
        for (int i = 0; i < 120; ++i) rt.tick();
        rt.window().drive_frames(false);

        ok(!Reactive::subscribe(rt.model()).wants_frames(),
           "and a stopped app requests NO frames, so it idles at 0% CPU");

        // Pointer motion earns a burst of frames, then quiet again.
        rt.window().push(MouseMove{{300, 300}, {1, 1}, {}});
        rt.tick();
        ok(Reactive::subscribe(rt.model()).wants_frames(),
           "moving the pointer wakes it so the trail can catch up");

        rt.window().drive_frames(true);
        for (int i = 0; i < 120; ++i) rt.tick();
        rt.window().drive_frames(false);
        ok(!Reactive::subscribe(rt.model()).wants_frames(),
           "and it goes back to sleep on its own");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
