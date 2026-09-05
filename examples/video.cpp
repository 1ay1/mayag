// examples/video.cpp — play a real video file.
//
// The point of this example is NOT that mayag decodes video. It does not, and
// should not: a UI framework that hard-depends on ffmpeg is a worse UI
// framework. The point is that decoding is ORDINARY APPLICATION WORK, and the
// framework needs no special support for it.
//
// Everything here is the same architecture as a counter:
//
//   the decoder        a Sub::source, so it exists exactly while playing and
//                      the runtime stops and joins the thread on pause
//   a decoded frame    an ordinary Msg carrying a shared_ptr, so an 8 MB
//                      1080p frame moves by pointer, not by copy
//   showing it         Cmd::upload_image + image(id) in the view
//   seeking            Cmd::task_latest, so dragging the scrub bar cannot
//                      deliver an old frame after a newer one
//
// There is no player object, no callback soup, and no mutable state hidden in
// a widget. Play/pause is a bool in the Model.
//
// ffmpeg is OPTIONAL. This example is only built when libav* is found, and
// nothing in mayag itself links against it -- the framework's only contract is
// "hand me RGBA8 pixels".
//
// Build:  cmake --build build && ./build/examples/mayag_video FILE.mp4

#include <mayag/mayag.hpp>

#include "harness.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <cstring>
}

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

// ── the decoder ─────────────────────────────────────────────────────────
//
// Deliberately a plain class with no mayag types in its interface: it takes a
// path and produces RGBA8 frames. That is the whole seam. Swapping ffmpeg for
// anything else means replacing this file's top half and nothing above it.
namespace {

struct VideoInfo {
    int    width = 0;
    int    height = 0;
    double duration = 0.0;     ///< seconds
    double fps = 0.0;
    std::string codec;
};

class Decoder {
  public:
    ~Decoder() { close(); }

    [[nodiscard]] bool open(const std::string& path) {
        close();
        if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
            error_ = "cannot open " + path;
            return false;
        }
        if (avformat_find_stream_info(fmt_, nullptr) < 0) {
            error_ = "no stream info";
            return false;
        }
        const AVCodec* codec = nullptr;
        stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (stream_ < 0 || codec == nullptr) {
            error_ = "no video stream";
            return false;
        }
        ctx_ = avcodec_alloc_context3(codec);
        if (ctx_ == nullptr) { error_ = "no codec context"; return false; }
        avcodec_parameters_to_context(ctx_, fmt_->streams[stream_]->codecpar);
        // Decoding is the slow part and it is embarrassingly parallel; let
        // ffmpeg use the cores it wants rather than starving it on one.
        ctx_->thread_count = 0;
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            error_ = "cannot open codec";
            return false;
        }

        frame_ = av_frame_alloc();
        rgb_   = av_frame_alloc();
        pkt_   = av_packet_alloc();
        if (frame_ == nullptr || rgb_ == nullptr || pkt_ == nullptr) {
            error_ = "allocation failed";
            return false;
        }

        info_.width    = ctx_->width;
        info_.height   = ctx_->height;
        info_.codec    = codec->name != nullptr ? codec->name : "?";
        const AVStream* st = fmt_->streams[stream_];
        info_.duration = fmt_->duration > 0
                       ? static_cast<double>(fmt_->duration) / AV_TIME_BASE : 0.0;
        info_.fps = st->avg_frame_rate.den > 0
                  ? av_q2d(st->avg_frame_rate) : 0.0;
        return true;
    }

    [[nodiscard]] const VideoInfo& info() const noexcept { return info_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] bool ok() const noexcept { return ctx_ != nullptr; }

    /// Decode the next frame as RGBA8. Returns null at end of stream.
    ///
    /// The buffer is freshly allocated per frame and handed over as a
    /// shared_ptr, which is what lets it travel to the renderer without a
    /// copy. `pts_out` is the presentation time in seconds, so the caller can
    /// pace playback against a real clock rather than assuming a frame rate.
    [[nodiscard]] std::shared_ptr<const ImageData> next(double* pts_out) {
        if (ctx_ == nullptr) return nullptr;

        for (;;) {
            int rc = avcodec_receive_frame(ctx_, frame_);
            if (rc == 0) {
                if (pts_out != nullptr) {
                    const AVStream* st = fmt_->streams[stream_];
                    const int64_t pts = frame_->best_effort_timestamp;
                    *pts_out = pts == AV_NOPTS_VALUE
                             ? 0.0 : static_cast<double>(pts) * av_q2d(st->time_base);
                }
                return to_rgba();
            }
            if (rc != AVERROR(EAGAIN)) return nullptr;   // EOF or hard error

            // Need more input.
            av_packet_unref(pkt_);
            if (av_read_frame(fmt_, pkt_) < 0) {
                avcodec_send_packet(ctx_, nullptr);      // flush
                if (avcodec_receive_frame(ctx_, frame_) == 0) return to_rgba();
                return nullptr;
            }
            if (pkt_->stream_index == stream_) {
                avcodec_send_packet(ctx_, pkt_);
            }
        }
    }

    /// Jump to `seconds` and decode the frame there.
    ///
    /// av_seek_frame with AVSEEK_FLAG_BACKWARD lands on the nearest KEYFRAME
    /// at or before the target, which can be seconds earlier -- inter-frames
    /// are not independently decodable, so that is the only place decoding can
    /// restart. Returning that frame directly means seeking to 3.0 s shows the
    /// picture from 0.0 s, which measured exactly that way: seek(1.0) and
    /// seek(3.0) both returned pts 0.000.
    ///
    /// So decode forward from the keyframe until reaching the requested time.
    /// The bounded loop matters: a corrupt or unusual file must not spin here,
    /// and a target past the end simply returns the last frame decoded.
    [[nodiscard]] std::shared_ptr<const ImageData> seek(double seconds,
                                                        double* pts_out) {
        if (ctx_ == nullptr) return nullptr;
        const AVStream* st = fmt_->streams[stream_];
        const int64_t ts = static_cast<int64_t>(seconds / av_q2d(st->time_base));
        if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0) return nullptr;
        avcodec_flush_buffers(ctx_);

        std::shared_ptr<const ImageData> last;
        double pts = 0.0;
        // A keyframe interval is rarely more than a few hundred frames; the
        // cap is generous enough to cross one and small enough to bound the
        // work if the file lies about its timestamps.
        for (int guard = 0; guard < 600; ++guard) {
            auto frame = next(&pts);
            if (!frame) break;
            last = std::move(frame);
            if (pts >= seconds - 0.001) break;   // reached the target
        }
        if (pts_out != nullptr) *pts_out = pts;
        return last;
    }

  private:
    [[nodiscard]] std::shared_ptr<const ImageData> to_rgba() {
        const int w = ctx_->width, h = ctx_->height;
        sws_ = sws_getCachedContext(sws_, w, h, ctx_->pix_fmt,
                                    w, h, AV_PIX_FMT_RGBA,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_ == nullptr) return nullptr;

        // Let ffmpeg size and align the destination.
        //
        // Hand-computing w*h*4 with a w*4 stride looks obviously right and is
        // not: sws_scale's SIMD paths write in aligned blocks and can touch
        // past the last row of a tightly-packed buffer. On a real 924x916
        // H.264 file that corrupted the heap -- "malloc(): corrupted top size"
        // -- while ASan stayed silent, because the overflow landed inside a
        // neighbouring allocation rather than in a redzone.
        //
        // av_image_get_buffer_size / av_image_fill_arrays with an explicit
        // alignment is the supported way to ask how much space the library
        // actually needs, so the scaler cannot write outside it.
        constexpr int kAlign = 32;
        const int need = av_image_get_buffer_size(AV_PIX_FMT_RGBA, w, h, kAlign);
        if (need <= 0) return nullptr;

        auto img = std::make_shared<ImageData>();
        img->width = w;
        img->height = h;
        // The buffer is at least `need`; the tail beyond w*h*4 is scratch the
        // scaler is allowed to touch.
        img->rgba.resize(static_cast<std::size_t>(need));

        std::uint8_t* dst[4]{};
        int stride[4]{};
        if (av_image_fill_arrays(dst, stride, img->rgba.data(),
                                 AV_PIX_FMT_RGBA, w, h, kAlign) < 0) {
            return nullptr;
        }
        sws_scale(sws_, frame_->data, frame_->linesize, 0, h, dst, stride);

        // mayag's ImageData is tightly packed, so drop any row padding ffmpeg
        // asked for before handing the pixels over.
        const std::size_t tight = static_cast<std::size_t>(w) * 4;
        if (static_cast<std::size_t>(stride[0]) != tight) {
            for (int y = 1; y < h; ++y) {
                std::memmove(img->rgba.data() + y * tight,
                             img->rgba.data() + static_cast<std::size_t>(y) * stride[0],
                             tight);
            }
        }
        img->rgba.resize(tight * static_cast<std::size_t>(h));
        return img;
    }

    void close() {
        if (sws_ != nullptr)   { sws_freeContext(sws_); sws_ = nullptr; }
        if (pkt_ != nullptr)   { av_packet_free(&pkt_); }
        if (rgb_ != nullptr)   { av_frame_free(&rgb_); }
        if (frame_ != nullptr) { av_frame_free(&frame_); }
        if (ctx_ != nullptr)   { avcodec_free_context(&ctx_); }
        if (fmt_ != nullptr)   { avformat_close_input(&fmt_); }
    }

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext*  ctx_ = nullptr;
    AVFrame*         frame_ = nullptr;
    AVFrame*         rgb_ = nullptr;
    AVPacket*        pkt_ = nullptr;
    SwsContext*      sws_ = nullptr;
    int              stream_ = -1;
    VideoInfo        info_;
    std::string      error_;
};

/// The file being played. A raw path rather than a shared decoder, because a
/// decoder is not shareable: an AVCodecContext carries per-stream position and
/// an SwsContext is mutated on every scale, so two threads inside one Decoder
/// corrupt each other's state.
///
/// The first version of this example kept function-local `static Decoder`s
/// behind a mutex. That looked safe and was not -- a static in a lambda is one
/// object shared by every invocation, so a seek task and the playback thread
/// took turns mutating the same swscale context and the heap. It crashed with
/// "malloc(): corrupted top size" on a real file.
///
/// The fix is ownership, not locking: each worker constructs its OWN Decoder,
/// opens the file, and destroys it when done. Opening a file is cheap relative
/// to decoding, and nothing is shared, so nothing can race.
std::string  g_path;
VideoInfo    g_info;

}  // namespace

// ── the app ─────────────────────────────────────────────────────────────

struct Video {
    static constexpr std::uint32_t kImage = 1;

    struct Model {
        bool   playing = false;
        bool   seeking = false;
        double time    = 0.0;      ///< current PTS, seconds
        double duration = 0.0;
        int    frames  = 0;
        float  decode_ms = 0.0f;
        std::string status;
    };

    struct Play {};
    struct Pause {};
    struct SeekTo { double seconds; };
    struct Frame { std::shared_ptr<const ImageData> pixels; double pts; float ms; };
    struct Ended {};
    struct Quit {};
    using Msg = std::variant<Play, Pause, SeekTo, Frame, Ended, Quit>;

    static inline const auto play_btn  = action(node_id("play"), Msg{Play{}});
    static inline const auto pause_btn = action(node_id("pause"), Msg{Pause{}});

    static std::pair<Model, Cmd<Msg>> init() {
        Model m;
        m.duration = g_info.duration;
        m.status   = g_info.codec + "  " + std::to_string(g_info.width) + "x" +
                     std::to_string(g_info.height);

        // Decode frame zero SYNCHRONOUSLY, unlike every later frame.
        //
        // This is the one place blocking is right. A screenshot renders a
        // single frame and exits, so an asynchronous first decode never
        // arrives and --png/--gpu produce an empty window -- which looks like
        // a rendering bug rather than a race with a worker. It also means a
        // live window is never blank on open.
        //
        // One decode of one keyframe, once, at startup. Every frame after this
        // is off the UI thread, which is what actually matters for latency.
        Decoder d;
        std::shared_ptr<const ImageData> first;
        double pts = 0.0;
        if (d.open(g_path)) first = d.seek(0.0, &pts);

        std::vector<Cmd<Msg>> boot;
        boot.push_back(Cmd<Msg>::set_title("mayag — " + g_path));
        if (first) {
            m.frames = 1;
            m.time   = pts;
            boot.push_back(Cmd<Msg>::upload_image(kImage, std::move(first)));
        }
        return {m, Cmd<Msg>::batch(std::move(boot))};
    }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        using R = std::pair<Model, Cmd<Msg>>;
        return match(msg,
            [&](Play) -> R {
                m.playing = true;
                return {m, Cmd<Msg>::none()};   // the Sub starts the decoder
            },
            [&](Pause) -> R { m.playing = false; return {m, Cmd<Msg>::none()}; },

            [&](SeekTo s) -> R {
                m.time = num::clamp(s.seconds, 0.0, m.duration);
                m.seeking = true;
                const double want = m.time;
                // task_latest: dragging the bar fires one of these per move
                // and only the newest can deliver, so the picture cannot jump
                // backwards to a frame the user already scrubbed past.
                return {m, Cmd<Msg>::task_latest("seek", [want] {
                    Decoder d;               // owned by this task alone
                    if (!d.open(g_path)) return Msg{Ended{}};
                    double pts = 0.0;
                    const auto t0 = std::chrono::steady_clock::now();
                    auto px = d.seek(want, &pts);
                    const float ms = std::chrono::duration<float, std::milli>(
                                         std::chrono::steady_clock::now() - t0).count();
                    if (!px) return Msg{Ended{}};
                    return Msg{Frame{std::move(px), pts, ms}};
                })};
            },

            [&](Frame f) -> R {
                m.time      = f.pts;
                m.decode_ms = f.ms;
                m.seeking   = false;
                ++m.frames;
                return {m, Cmd<Msg>::upload_image(kImage, f.pixels)};
            },

            [&](Ended) -> R {
                m.playing = false;
                m.status  = "end of stream";
                return {m, Cmd<Msg>::none()};
            },
            [&](Quit) -> R { return {m, Cmd<Msg>::quit()}; });
    }

    /// The playback decoder: a subscription, so it exists exactly while
    /// `playing` is true and the runtime joins the thread when it is not.
    static Sub<Msg> decoder(const Model& m) {
        const double from = m.time;
        return Sub<Msg>::source("playback", [from](auto sink, auto alive) {
            Decoder d;                       // owned by this thread alone
            if (!d.open(g_path)) { sink(Msg{Ended{}}); return; }
            double ignored = 0.0;
            if (from > 0.05) (void)d.seek(from, &ignored);

            using clk = std::chrono::steady_clock;
            const auto started = clk::now();
            double first_pts = -1.0;

            while (alive()) {
                double pts = 0.0;
                const auto t0 = clk::now();
                std::shared_ptr<const ImageData> px = d.next(&pts);
                if (!px) { if (alive()) sink(Msg{Ended{}}); return; }
                const float ms = std::chrono::duration<float, std::milli>(
                                     clk::now() - t0).count();
                if (first_pts < 0.0) first_pts = pts;

                // Pace against the PTS, not a fixed sleep: a variable-frame-
                // rate file plays at its own speed, and a slow decode does not
                // accumulate drift.
                const double target = pts - first_pts;
                const double elapsed =
                    std::chrono::duration<double>(clk::now() - started).count();
                if (target > elapsed) {
                    std::this_thread::sleep_for(
                        std::chrono::duration<double>(target - elapsed));
                }
                if (!alive()) return;
                sink(Msg{Frame{std::move(px), pts, ms}});
            }
        });
    }

    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = c.theme;
        const float progress = m.duration > 0.0
                             ? static_cast<float>(m.time / m.duration) : 0.0f;

        // Fit the video into a fixed box, preserving aspect.
        const float box_w = 800.0f;
        const float aspect = g_info.height > 0
                           ? static_cast<float>(g_info.width) / g_info.height : 1.6f;
        const float box_h = box_w / (aspect > 0.01f ? aspect : 1.6f);

        return (v(
            image(kImage) | width(px(box_w)) | height(px(box_h)) | radius(8),

            z(box() | width(px(box_w)) | height(px(8)) | radius(4)
                    | bg(t.surface_raised),
              box() | width(px(box_w * progress)) | height(px(8)) | radius(4)
                    | bg(m.seeking ? t.warning : t.accent))
              | width(px(box_w)) | height(px(8)),

            h(text_owned(m.playing ? "pause" : "play") | font(13) | semibold
                  | fg(t.on_accent)
                  | (m.playing ? pause_btn.on : play_btn.on),
              box() | grow(),
              text_owned(fmt_time(m.time) + " / " + fmt_time(m.duration))
                  | font(12) | fg(t.text_secondary),
              text_owned(m.status) | font(12) | fg(t.text_secondary),
              text_owned("decode " + fmt_ms(m.decode_ms)) | font(12)
                  | fg(t.text_secondary))
              | gap(16) | center | width(px(box_w)))
            | gap(14) | center | pad(24) | bg(t.background)).build();
    }

    static std::string fmt_time(double s) {
        char b[32];
        std::snprintf(b, sizeof b, "%d:%05.2f", static_cast<int>(s) / 60,
                      s - 60.0 * (static_cast<int>(s) / 60));
        return b;
    }
    static std::string fmt_ms(float ms) {
        char b[32];
        std::snprintf(b, sizeof b, "%.1f ms", static_cast<double>(ms));
        return b;
    }

    static Sub<Msg> subscribe(const Model& m) {
        return Sub<Msg>::batch({
            play_btn.click(),
            pause_btn.click(),
            Sub<Msg>::on_key(Key::space, m.playing ? Msg{Pause{}} : Msg{Play{}}),
            Sub<Msg>::on_key(Key::escape, Msg{Quit{}}),
            Sub<Msg>::on_close(Msg{Quit{}}),
            when(m.playing, decoder(m)),
        });
    }
};

int main(int argc, char** argv) {
    // Find the file argument: anything that is not a harness flag.
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (!a.empty() && a[0] != '-') { g_path = std::string{a}; break; }
    }
    if (g_path.empty()) {
        std::fprintf(stderr,
            "usage: mayag_video FILE [--headless]\n"
            "       plays a real video file through mayag's image() element\n");
        return 2;
    }

    // Probe once up front so the window can be sized to the video and the
    // app can fail with a clear message rather than an empty window.
    {
        Decoder probe;
        if (!probe.open(g_path)) {
            std::fprintf(stderr, "mayag_video: %s\n", probe.error().c_str());
            return 1;
        }
        g_info = probe.info();
    }
    std::printf("%s: %s %dx%d, %.2fs, %.2f fps\n", g_path.c_str(),
                g_info.codec.c_str(), g_info.width, g_info.height,
                g_info.duration, g_info.fps);

    const auto opts = demo::parse_args(argc, argv, {860, 620});
    AppConfig cfg;
    cfg.title = "mayag — video";
    cfg.size  = opts.size;

    return demo::run<Video>(opts, std::move(cfg), [](auto& rt) {
        int fails = 0;
        const auto ok = [&](bool c, const std::string& what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what.c_str());
            if (!c) ++fails;
        };
        std::printf("video headless\n");

        const auto pump = [&](auto pred, int ms) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(ms);
            while (!pred() && std::chrono::steady_clock::now() < deadline) {
                rt.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };

        // init() decodes frame 0 on a worker. Real decoding of a real file:
        // opening the container, finding the stream and pulling the first
        // keyframe is tens of milliseconds, not microseconds, so every wait
        // here is in WALL time with generous headroom.
        pump([&] { return rt.model().frames > 0; }, 15000);
        ok(rt.model().frames > 0, "the first frame decodes from the real file");
        ok(rt.model().duration > 0.0, "duration was read from the container");

        // Play. The decoder paces itself against the presentation timestamps,
        // so frames arrive at the file's own rate -- waiting on a frame COUNT
        // rather than a tick count is the only thing that means anything.
        const int before = rt.model().frames;
        // Drive the message rather than the pixel. A headless click has to
        // hit a laid-out rect, and this button's label is sized by the font
        // stack -- testing the transport through a hit test would be testing
        // text metrics, not playback.
        rt.send(Video::Msg{Video::Play{}});
        pump([&] { return rt.model().frames > before + 10; }, 15000);
        ok(rt.model().playing, "play starts the decoder");
        ok(rt.model().frames > before + 10,
           "frames arrive from the decode thread (" +
               std::to_string(rt.model().frames) + ")");
        ok(rt.model().time > 0.0, "and carry a real presentation timestamp");

        // Pause must stop it. Let anything already decoded land first: a
        // frame produced just before the pause is legitimately still in
        // flight, and asserting immediately would test a race.
        rt.send(Video::Msg{Video::Pause{}});
        rt.tick();
        pump([] { return false; }, 400);
        const int settled = rt.model().frames;
        pump([] { return false; }, 400);
        ok(!rt.model().playing, "pause stops playback");
        ok(rt.model().frames == settled, "and no further frames arrive");

        // Seek somewhere concrete. The tolerance is TIGHT on purpose: a
        // loose one hid a real bug. Returning the keyframe rather than
        // decoding forward to the target made seek(3.0) land at 0.0, and a
        // 5-second tolerance called that a pass.
        const double target = rt.model().duration * 0.5;
        rt.send(Video::Msg{Video::SeekTo{target}});
        pump([&] { return !rt.model().seeking; }, 15000);
        ok(!rt.model().seeking, "a seek completes");
        ok(std::abs(rt.model().time - target) < 0.25,
           "and lands near the requested time (" +
               std::to_string(rt.model().time) + " vs " +
               std::to_string(target) + ")");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    });
}
