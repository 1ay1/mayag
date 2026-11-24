// examples/player.cpp — a video player, the Elm way.
//
// The point of this example is that playing video needs NO special machinery.
// A decoder is just a Sub::stream producing messages; a frame is just pixels
// handed over with an UploadImage effect; the picture is just image(id) in the
// view. Play/pause is a bool in the Model. Seeking is an integer assignment.
// There is no player object, no callback soup, and no mutable state hidden
// inside a widget -- the same architecture as every other mayag app.
//
// The frames here are SYNTHESISED rather than decoded from a file. A real
// codec is a heavy dependency for a UI framework to demonstrate, and it would
// prove nothing extra: what has to work is decode-thread -> message ->
// upload -> draw, at frame rate, without copying 8 MB per frame or blocking
// the UI. Swapping this generator for libav is an exercise in filling one
// function.
//
// Build:  cmake --build build && ./build/examples/mayag_player
// (--headless self-checks; --png/--gpu write a screenshot)

// GCC 16 at -O2 reports -Wmaybe-uninitialized inside std::shared_ptr's move
// when a Frame (which holds one) travels through std::variant in the runtime's timer
// heap. Every member of Frame is initialised; it is a known false positive
// of the variant-storage analysis (clang and GCC 14 are silent). Scoped to
// this file so -Werror builds stay meaningful everywhere else.
#include <mayag/core/target.hpp>
#if MAYAG_CC_GCC
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include <mayag/mayag.hpp>

#include "harness.hpp"

#include <cmath>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

struct Player {
    static constexpr std::uint32_t kVideoImage = 1;   // the id view() draws
    static constexpr int kFrameW = 320;
    static constexpr int kFrameH = 180;
    static constexpr int kTotalFrames = 300;          // 5s at 60fps

    // ── state ───────────────────────────────────────────────────────────
    struct Model {
        bool  playing   = false;
        bool  seeking   = false;   // a seek decode is in flight
        int   frame     = 0;
        int   decoded   = 0;      // how many the decoder has produced
        float last_ms   = 0.0f;   // decode cost, so the UI can show it
        Animated<float> scrub{0.0f};
    };

    // ── messages ────────────────────────────────────────────────────────
    struct Play {};
    struct Pause {};
    struct Seek { int frame; };
    /// A decoded frame, arriving from the worker. The pixels travel by
    /// mayag::Shared -- 8 MB per message would otherwise be copied twice.
    struct Frame { mayag::Shared<ImageData> pixels; int index = 0; float ms = 0.0f; };
    struct Tick { float dt; };
    struct Quit {};
    using Msg = std::variant<Play, Pause, Seek, Frame, Tick, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title, upload_image>;
    using Sub = mayag::Sub<Msg>;

    // ── the "decoder" ───────────────────────────────────────────────────
    //
    // Stands in for libav. Produces a recognisable moving picture so a
    // screenshot can be checked by eye and by test: a colour field that
    // sweeps with time, plus a bar that marches across the frame.
    static mayag::Shared<ImageData> decode(int index) {
        ImageData img;
        img.width = kFrameW;
        img.height = kFrameH;
        img.rgba.resize(static_cast<std::size_t>(kFrameW) * kFrameH * 4);

        const float t = static_cast<float>(index) / static_cast<float>(kTotalFrames);
        const int bar = static_cast<int>(t * static_cast<float>(kFrameW));

        for (int y = 0; y < kFrameH; ++y) {
            for (int x = 0; x < kFrameW; ++x) {
                const float u = static_cast<float>(x) / kFrameW;
                const float v = static_cast<float>(y) / kFrameH;
                // A smooth field that changes every frame, so a stale upload
                // is visible rather than plausible.
                float r = 0.5f + 0.5f * std::sin((u * 6.0f) + t * 6.28f);
                float g = 0.5f + 0.5f * std::sin((v * 5.0f) + t * 4.0f + 2.0f);
                float b = 0.5f + 0.5f * std::sin(((u + v) * 4.0f) - t * 5.0f);
                if (std::abs(x - bar) < 3) { r = g = b = 1.0f; }   // marching bar
                const std::size_t i =
                    (static_cast<std::size_t>(y) * kFrameW + x) * 4;
                img.rgba[i + 0] = static_cast<std::uint8_t>(r * 255.0f);
                img.rgba[i + 1] = static_cast<std::uint8_t>(g * 255.0f);
                img.rgba[i + 2] = static_cast<std::uint8_t>(b * 255.0f);
                img.rgba[i + 3] = 255;
            }
        }
        return mayag::Shared<ImageData>::make(std::move(img));
    }

    // ── init ────────────────────────────────────────────────────────────
    static Cmd init(Model&) {
        // Show frame 0 immediately, so the window is never blank.
        return Cmd::batch(SetTitle{"mayag — player"}, upload(kVideoImage, decode(0)));
    }

    // ── update ──────────────────────────────────────────────────────────
    // No Cmd needed for Play: the decoder is a SUBSCRIPTION, so it starts
    // simply because `playing` became true. See subscribe().
    static Cmd update(Model& m, Play)  { m.playing = true; return {}; }
    static Cmd update(Model& m, Pause) { m.playing = false; return {}; }

    static Cmd update(Model& m, Seek s) {
        m.frame = num::clamp(s.frame, 0, kTotalFrames - 1);
        m.scrub.to(static_cast<float>(m.frame));
        m.seeking = true;

        // Decode OFF the UI thread (~19 ms at 1080p, more than a 60 Hz
        // frame). The "seek" key makes it latest-wins: a newer seek replaces
        // the running one, so an old frame can never land after a newer one.
        return Cmd::task("seek",
            [](mayag::Sink<Msg> out, int want) {
                const auto t0 = std::chrono::steady_clock::now();
                auto pixels = decode(want);
                const float ms = std::chrono::duration<float, std::milli>(
                                     std::chrono::steady_clock::now() - t0).count();
                out.send(Frame{std::move(pixels), want, ms});
            },
            m.frame);
    }

    static Cmd update(Model& m, Frame f) {
        // A frame from a worker -- the decoder stream or a seek task. No
        // staleness check: mayag drops messages from a replaced task or a
        // stopped stream.
        m.frame   = f.index;
        m.decoded = f.index + 1;
        m.last_ms = f.ms;
        m.seeking = false;
        m.scrub.snap(static_cast<float>(f.index));
        if (f.index + 1 >= kTotalFrames) m.playing = false;
        return upload(kVideoImage, std::move(f.pixels));
    }

    static Cmd update(Model& m, Tick t) { m.scrub.step(static_cast<double>(t.dt)); return {}; }
    static Cmd update(Model&, Quit)     { return Cmd::quit(); }

    /// The decoder, as a SUBSCRIPTION rather than a command. A Sub exists
    /// exactly while the model says so: pausing removes it and mayag stops it,
    /// so no frames keep arriving after pause. The start frame is read only
    /// when the stream starts (the key stays "decoder" while playing).
    static Sub decoder(const Model& m) {
        return Sub::stream("decoder",
            [](mayag::Sink<Msg> out, std::stop_token st, int from) {
                using clk = std::chrono::steady_clock;
                for (int i = from; i < kTotalFrames && !st.stop_requested(); ++i) {
                    const auto t0 = clk::now();
                    auto pixels = decode(i);
                    const float ms = std::chrono::duration<float, std::milli>(
                                         clk::now() - t0).count();
                    if (st.stop_requested()) return;
                    out.send(Frame{std::move(pixels), i, ms});
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
            },
            m.frame);
    }

    // ── view ────────────────────────────────────────────────────────────
    static Node view(const Model& m) {
        const Theme& t = ctx().theme;
        const float progress = static_cast<float>(m.frame + 1) /
                               static_cast<float>(kTotalFrames);

        return (v(
            // THE PICTURE. One element; the pixels arrived via UploadImage.
            image(kVideoImage)
                | width(px(640)) | height(px(360)) | radius(10),

            // Scrub bar: a track with a fill, width driven by the model.
            z(box() | width(px(640)) | height(px(10)) | radius(5)
                    | bg(t.surface_raised),
              box() | width(px(640.0f * progress)) | height(px(10)) | radius(5)
                    | bg(m.seeking ? t.warning : t.accent))
              | width(px(640)) | height(px(10))
              | dsl::id<"track">,

            h(text_owned(m.playing ? "pause" : "play") | font(13) | semibold
                  | fg(t.on_accent)
                  | id_of(m.playing ? "pause" : "play"),
              box() | grow(),
              text_owned(std::to_string(m.frame + 1) + " / " +
                         std::to_string(kTotalFrames)) | font(12)
                  | fg(t.text_secondary),
              text_owned("decode " + fmt_ms(m.last_ms)) | font(12)
                  | fg(t.text_secondary))
              | gap(14) | center | width(px(640)))
            | gap(14) | center | pad(24)
            | bg(t.background)).build();
    }

    static std::string fmt_ms(float ms) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.2f ms", static_cast<double>(ms));
        return buf;
    }

    // ── subscribe ───────────────────────────────────────────────────────
    static Sub subscribe(const Model& m) {
        return Sub::batch(
            // The decoder exists exactly while playing.
            when(m.playing, decoder(m)),
            // Only while the scrub spring is settling; a paused player idles.
            when(m.scrub.animating(),
                 Sub::on_frame([](double dt) -> Msg { return Tick{static_cast<float>(dt)}; })));
    }

    // ── input ───────────────────────────────────────────────────────────
    static std::optional<Msg> on(const Model&, const Clicked& c) {
        if (c.count != 1) return std::nullopt;
        if (is(c, "play"))  return Play{};
        if (is(c, "pause")) return Pause{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const KeyEvent& k) {
        if (k.key == Key::space)  return m.playing ? Msg{Pause{}} : Msg{Play{}};
        if (k.key == Key::escape) return Quit{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }
};

static_assert(mayag::Program<Player>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, {700, 470});

    AppConfig cfg;
    cfg.title = "mayag — player";
    cfg.size  = opts.size;

    return demo::run<Player>(opts, std::move(cfg), [](auto& rt) {
        int fails = 0;
        const auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            if (!c) ++fails;
        };
        std::printf("player headless\n");

        ok(!rt.model().playing, "starts paused");
        ok(rt.model().frame == 0, "showing frame 0");

        // Play: the decoder thread starts and frames flow in.
        //
        // The decoder paces itself at ~60fps in WALL CLOCK, because that is
        // what a player does. Headless tick() returns immediately, so a loop
        // counting ticks would spin through thousands of iterations while the
        // producer had barely started -- the wait has to be in real time.
        rt.click("play");
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(10);
        while (rt.model().frame < 5 &&
               std::chrono::steady_clock::now() < deadline) {
            rt.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok(rt.model().playing, "play starts the decoder");
        ok(rt.model().frame >= 5, "frames arrive from the worker thread");
        ok(rt.model().last_ms > 0.0f, "and carry their decode cost");

        // Pause: the stream disappears and mayag stops it.
        rt.click("pause");
        rt.tick();
        ok(!rt.model().playing, "pause stops playback");

        // Let anything already in flight land, THEN check nothing new
        // arrives. A frame decoded just before the pause is legitimately
        // still on its way; asserting immediately would test a race.
        for (int i = 0; i < 50; ++i) {
            rt.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const int settled = rt.model().frame;
        for (int i = 0; i < 50; ++i) {
            rt.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok(rt.model().frame == settled, "and no further frames arrive");

        // Seek is ASYNCHRONOUS: the decode runs on a worker so the UI thread
        // never blocks. `frame` moves immediately -- that is what keeps the
        // scrub bar glued to the cursor -- so the thing to wait on is
        // `seeking` going false, which happens when the decoded picture
        // actually lands. Waiting on `frame` would pass instantly and assert
        // nothing.
        rt.send(Player::Msg{Player::Seek{120}});
        ok(rt.model().frame == 120, "seek moves the model immediately");
        ok(rt.model().seeking, "and marks a decode in flight");

        const auto seek_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(10);
        while (rt.model().seeking &&
               std::chrono::steady_clock::now() < seek_deadline) {
            rt.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok(!rt.model().seeking, "the decoded frame lands and clears the flag");
        ok(rt.model().frame == 120, "and it is the frame that was asked for");

        ok(rt.window().frames_presented() >= 1, "the player rendered");
        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        return fails;
    });
}
