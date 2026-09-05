// examples/player.cpp — a video player, the Elm way.
//
// The point of this example is that playing video needs NO special machinery.
// A decoder is just a Cmd::stream producing messages; a frame is just pixels
// handed over with Cmd::upload_image; the picture is just image(id) in the
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
    /// shared_ptr -- 8 MB per message would otherwise be copied twice.
    struct Frame { std::shared_ptr<const ImageData> pixels; int index; float ms; };
    struct Tick { float dt; };
    struct Quit {};
    using Msg = std::variant<Play, Pause, Seek, Frame, Tick, Quit>;

    static inline const auto play_btn = action(node_id("play"), Msg{Play{}});
    static inline const auto pause_btn = action(node_id("pause"), Msg{Pause{}});

    // ── the "decoder" ───────────────────────────────────────────────────
    //
    // Stands in for libav. Produces a recognisable moving picture so a
    // screenshot can be checked by eye and by test: a colour field that
    // sweeps with time, plus a bar that marches across the frame.
    static std::shared_ptr<const ImageData> decode(int index) {
        auto img = std::make_shared<ImageData>();
        img->width = kFrameW;
        img->height = kFrameH;
        img->rgba.resize(static_cast<std::size_t>(kFrameW) * kFrameH * 4);

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
                img->rgba[i + 0] = static_cast<std::uint8_t>(r * 255.0f);
                img->rgba[i + 1] = static_cast<std::uint8_t>(g * 255.0f);
                img->rgba[i + 2] = static_cast<std::uint8_t>(b * 255.0f);
                img->rgba[i + 3] = 255;
            }
        }
        return img;
    }

    // ── init ────────────────────────────────────────────────────────────
    static std::pair<Model, Cmd<Msg>> init() {
        // Show frame 0 immediately, so the window is never blank.
        Model m;
        return {m, Cmd<Msg>::batch({
            Cmd<Msg>::set_title("mayag — player"),
            Cmd<Msg>::upload_image(kVideoImage, decode(0)),
        })};
    }

    // ── update ──────────────────────────────────────────────────────────
    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Play) -> R {
                if (m.playing) return {m, Cmd<Msg>::none()};
                m.playing = true;
                // The decoder runs while `playing` is true and stops when it
                // is not -- the runtime joins the thread for us.
                return {m, decoder_stream(m.frame)};
            },
            [&](Pause) -> R { m.playing = false; return {m, Cmd<Msg>::none()}; },

            [&](Seek s) -> R {
                m.frame = num::clamp(s.frame, 0, kTotalFrames - 1);
                m.scrub.to(static_cast<float>(m.frame));
                // Seeking while paused still updates the picture: decode one
                // frame inline. It is 320x180; at 1080p this would be a
                // Cmd::task so the UI thread never waits.
                return {m, Cmd<Msg>::upload_image(kVideoImage, decode(m.frame))};
            },

            [&](Frame f) -> R {
                // A frame arrived from the worker. Hand it to the renderer
                // and remember what we are showing. This is the whole of
                // "display a video frame".
                m.frame   = f.index;
                m.decoded = f.index + 1;
                m.last_ms = f.ms;
                m.scrub.snap(static_cast<float>(f.index));
                if (f.index + 1 >= kTotalFrames) m.playing = false;
                return {m, Cmd<Msg>::upload_image(kVideoImage, f.pixels)};
            },

            [&](Tick t) -> R { m.scrub.step(t.dt); return {m, Cmd<Msg>::none()}; },
            [&](Quit) -> R { return {m, Cmd<Msg>::quit()}; });
    }

    /// The decoder thread: produce frames at ~60fps until told to stop.
    static Cmd<Msg> decoder_stream(int from) {
        return Cmd<Msg>::stream([from](auto sink, auto alive) {
            using clk = std::chrono::steady_clock;
            for (int i = from; i < kTotalFrames && alive(); ++i) {
                const auto t0 = clk::now();
                auto pixels = decode(i);
                const float ms = std::chrono::duration<float, std::milli>(
                                     clk::now() - t0).count();
                if (!alive()) return;
                sink(Msg{Frame{std::move(pixels), i, ms}});
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });
    }

    // ── view ────────────────────────────────────────────────────────────
    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = c.theme;
        const float progress = static_cast<float>(m.frame + 1) /
                               static_cast<float>(kTotalFrames);

        return (v(
            // THE PICTURE. One element; the pixels arrived via Cmd.
            image(kVideoImage)
                | width(px(640)) | height(px(360)) | radius(10),

            // Scrub bar: a track with a fill, width driven by the model.
            z(box() | width(px(640)) | height(px(6)) | radius(3)
                    | bg(t.surface_raised),
              box() | width(px(640.0f * progress)) | height(px(6)) | radius(3)
                    | bg(t.accent))
              | width(px(640)) | height(px(6)),

            h(text_owned(m.playing ? "pause" : "play") | font(13) | semibold
                  | fg(t.on_accent)
                  | (m.playing ? pause_btn.on : play_btn.on),
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
    static Sub<Msg> subscribe(const Model& m) {
        return Sub<Msg>::batch({
            play_btn.click(),
            pause_btn.click(),
            Sub<Msg>::on_key(Key::space, m.playing ? Msg{Pause{}} : Msg{Play{}}),
            Sub<Msg>::on_key(Key::escape, Msg{Quit{}}),
            Sub<Msg>::on_close(Msg{Quit{}}),
            // Only while the scrub spring is settling; a paused player idles.
            when(m.scrub.animating(),
                 Sub<Msg>::on_frame([](float dt) { return Msg{Tick{dt}}; })),
        });
    }
};

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
        rt.click("play");
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(10);
        while (rt.model().frame < 5 &&
               std::chrono::steady_clock::now() < deadline) {
            rt.tick();
        }
        ok(rt.model().playing, "play starts the decoder");
        ok(rt.model().frame >= 5, "frames arrive from the worker thread");
        ok(rt.model().last_ms > 0.0f, "and carry their decode cost");

        // Pause: the source disappears, the runtime joins the thread.
        rt.click("pause");
        rt.tick();
        const int at_pause = rt.model().frame;
        for (int i = 0; i < 40; ++i) rt.tick();
        ok(!rt.model().playing, "pause stops playback");
        ok(rt.model().frame == at_pause, "and no further frames arrive");

        // Seek while paused still updates the picture.
        rt.send(Player::Msg{Player::Seek{120}});
        rt.tick();
        ok(rt.model().frame == 120, "seek moves to the requested frame");

        ok(rt.window().frames_presented() >= 1, "the player rendered");
        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    });
}
