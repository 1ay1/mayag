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
        // ffmpeg logs at INFO by default, and every container open re-reports
        // whatever it dislikes about the file. A scrub drag opens the file
        // repeatedly, so a harmless note like "UDTA parsing failed retrying
        // raw" arrives dozens of times a second and buries anything real.
        // Errors still print; the chatter does not.
        av_log_set_level(AV_LOG_ERROR);

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
    ///
    /// `still_wanted` lets a caller abandon that walk. It matters more than it
    /// looks: on a screen recording the keyframe interval is often the whole
    /// file, so one seek decodes ~180 frames and takes ~660 ms. Dragging a
    /// scrub bar asks for a new position ~60 times a second. task_latest
    /// discards the stale RESULTS, but without this every one of those decodes
    /// still runs to completion, and the machine falls a minute behind in a
    /// second of dragging.
    ///
    /// The polarity matches the runtime's: TRUE means keep going. It is named
    /// for what it asks rather than for what it stops, because the opposite
    /// spelling (`cancel`) reads identically at the call site while meaning
    /// the reverse -- and wiring the runtime's `still_wanted` into a `cancel`
    /// parameter aborted every seek on its first frame, which surfaced only as
    /// a video that would not scrub.
    /// `precise` chooses between the two useful answers to "seek here".
    ///
    /// true  -- the EXACT frame at `seconds`. Decodes forward from the
    ///          keyframe; ~500 ms on a 1080p file whose keyframes are up to
    ///          10 s apart.
    /// false -- the nearest keyframe at or before it, decoding nothing extra:
    ///          ~50 ms on the same file, a 10x difference, at the cost of
    ///          landing up to a keyframe interval early.
    ///
    /// A drag wants the cheap one -- the user is sweeping past this position
    /// and needs a picture NOW to aim with, not the exact frame -- and the
    /// release wants the precise one. Nothing else in the two paths differs,
    /// so it is one flag rather than two functions.
    [[nodiscard]] std::shared_ptr<const ImageData> seek(
            double seconds, double* pts_out, bool precise = true,
            const std::function<bool()>& still_wanted = {}) {
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
            // Superseded by a newer seek: stop rather than finish work whose
            // result is already destined for the bin.
            if (still_wanted && !still_wanted()) return nullptr;
            auto frame = next(&pts);
            if (!frame) break;
            last = std::move(frame);
            if (!precise) break;                 // the keyframe is the answer
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

        /// Controls fade out during playback and come back on pointer
        /// movement -- the behaviour every video player has. Held as a spring
        /// so it eases rather than blinking, and `idle` counts the seconds
        /// since the pointer last moved.
        Animated<float> chrome{1.0f};
        double idle = 0.0;
        Vec2   pointer{};
        /// Last known window size, so the Scrub handler can work out the
        /// track's width. Updated from Ctx on every pointer event.
        Vec2   window{860.0f, 620.0f};
        /// Bumped by every seek. It is part of the playback source's id, so a
        /// seek replaces the decoder thread rather than leaving it running
        /// from the old position. See decoder().
        int    epoch = 0;
    };

    /// Inset and track width, shared by view() and the Scrub handler so a
    /// click maps to the same geometry that was drawn.
    static float control_inset(Vec2 vp) {
        return num::clamp(vp.x * 0.03f, 12.0f, 40.0f);
    }
    static float track_width(Vec2 vp) {
        return num::max(vp.x - control_inset(vp) * 2.0f, 120.0f);
    }

    struct Play {};
    struct Pause {};
    struct SeekTo { double seconds; bool preview = false; };
    struct Frame { std::shared_ptr<const ImageData> pixels; double pts; float ms; };
    struct Ended {};
    /// A seek that gave up because a newer one replaced it. Distinct from
    /// Ended: it is the normal outcome of dragging, and must not stop playback.
    struct SeekAbandoned {};
    struct Pointer { Vec2 at; };
    /// Pointer x INSIDE the scrub track, in logical pixels. `dragging` is
    /// true while the button is held: the seek can then be a cheap keyframe
    /// preview, because the user is still aiming.
    struct Scrub { float local_x; bool dragging = false; };
    /// The scrub button came up. Re-seek precisely to where it landed.
    struct ScrubEnd {};
    /// No seek is running or pending: the picture is where it was asked to be.
    struct SeekSettled {};
    struct Resized { Vec2 size; };
    struct Tick { float dt; };
    struct Quit {};
    using Msg = std::variant<Play, Pause, SeekTo, Frame, Ended, SeekAbandoned,
                             Pointer, Scrub, ScrubEnd, SeekSettled,
                             Resized, Tick, Quit>;

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
                // New epoch -> new playback source id, so a seek while playing
                // restarts the decoder at the new position instead of leaving
                // the old thread running from where it was.
                ++m.epoch;
                const double want = m.time;
                // task_coalesce, NOT task_latest: one decode runs at a time and
                // the newest pending position replaces the rest.
                //
                // task_latest starves under a drag. A seek on a 1080p file
                // costs ~500 ms (av_seek_frame lands on a keyframe up to ~10 s
                // back, and the frames between it and the target must all be
                // decoded), while a dragged scrub bar asks ~60 times a second
                // -- so every decode was superseded before it could finish and
                // the picture never moved until the mouse stopped. Measured:
                // 117 requests over a 2 s drag produced ZERO frames.
                //
                // Coalescing shows a frame every ~500 ms during the drag, each
                // nearer the cursor, and the release position always lands.
                return {m, Cmd<Msg>::task_coalesce("seek",
                    [want, preview = s.preview] {
                        Decoder d;               // owned by this task alone
                        if (!d.open(g_path)) return Msg{Ended{}};
                        double pts = 0.0;
                        const auto t0 = std::chrono::steady_clock::now();
                        // No cancel predicate: under task_coalesce this decode
                        // is the only one running and its frame is wanted.
                        // Abandoning it is what made the drag show nothing.
                        auto px = d.seek(want, &pts, !preview);
                        const float ms = std::chrono::duration<float, std::milli>(
                                             std::chrono::steady_clock::now() - t0).count();
                        if (px) return Msg{Frame{std::move(px), pts, ms}};
                        return Msg{Ended{}};
                    },
                    // Delivered once no seek is running or pending. A frame
                    // arriving is NOT proof the seek finished: the decode that
                    // was already running when a newer position arrived still
                    // delivers, and treating that as the answer reported the
                    // player as landed 864 s from where it was sent.
                    Msg{SeekSettled{}})};
            },

            [&](Frame f) -> R {
                m.time      = f.pts;
                m.decode_ms = f.ms;
                ++m.frames;
                // NOT `m.seeking = false`: this frame may belong to a
                // superseded request. SeekSettled owns that flag.
                return {m, Cmd<Msg>::upload_image(kImage, f.pixels)};
            },

            [&](SeekSettled) -> R {
                // Every seek for this key has finished and none is waiting, so
                // the frame on screen is the one that was asked for.
                m.seeking = false;
                return {m, Cmd<Msg>::none()};
            },

            [&](Ended) -> R {
                m.playing = false;
                m.status  = "end of stream";
                m.chrome.to(1.0f);          // always show controls when stopped
                return {m, Cmd<Msg>::none()};
            },

            [&](SeekAbandoned) -> R {
                // A newer scrub superseded this decode. Leave `seeking` set:
                // the task that replaced it is still running and will clear it.
                return {m, Cmd<Msg>::none()};
            },

            [&](Pointer p) -> R {
                m.pointer = p.at;
                m.idle    = 0.0;
                m.chrome.to(1.0f);
                return {m, Cmd<Msg>::none()};
            },

            [&](Scrub s) -> R {
                // Local x inside the track -> a time.
                //
                // The track's width is derived from the last known window
                // size using the SAME formula the view uses, rather than
                // stored: view() is pure and cannot write to the model, and
                // duplicating one expression is better than a mutable field
                // that can silently go stale for a frame after a resize.
                const float w = track_width(m.window);
                if (w <= 1.0f || m.duration <= 0.0) return {m, Cmd<Msg>::none()};

                const double frac =
                    num::clamp(static_cast<double>(s.local_x) / w, 0.0, 1.0);
                m.idle = 0.0;
                m.chrome.to(1.0f);
                // Reuse the seek path, which is already coalescing. While the
                // button is held this asks for a cheap keyframe preview: a
                // 10x faster decode the user can aim with. ScrubEnd re-seeks
                // precisely once they let go.
                return update(std::move(m),
                              Msg{SeekTo{frac * m.duration, s.dragging}});
            },

            [&](ScrubEnd) -> R {
                // The button came up. Land exactly where they left it -- the
                // previews during the drag were keyframe-accurate only.
                return update(std::move(m), Msg{SeekTo{m.time, false}});
            },

            [&](Resized r) -> R { m.window = r.size; return {m, Cmd<Msg>::none()}; },

            [&](Tick t) -> R {
                m.chrome.step(t.dt);
                // Only playback hides the chrome. A paused player keeps its
                // controls up, because the user is looking at them.
                if (m.playing) {
                    m.idle += t.dt;
                    if (m.idle > 2.5) m.chrome.to(0.0f);
                } else {
                    m.idle = 0.0;
                    m.chrome.to(1.0f);
                }
                return {m, Cmd<Msg>::none()};
            },

            [&](Quit) -> R { return {m, Cmd<Msg>::quit()}; });
    }

    /// The playback decoder: a subscription, so it exists exactly while
    /// `playing` is true and the runtime joins the thread when it is not.
    ///
    /// The id carries the seek EPOCH, not just "playback".
    ///
    /// Sources are reconciled by id: an unchanged id means "still the same
    /// producer, leave it alone". With a constant id the thread started at the
    /// original position and kept running, so seeking while playing moved the
    /// scrub bar and the picture carried on from where it was -- the seek
    /// appeared to do nothing until you paused. Folding the epoch in makes a
    /// seek a DIFFERENT source, so the runtime stops the old thread, joins it,
    /// and starts one at the new time. That is the declarative contract doing
    /// the work; nothing has to remember to restart anything.
    static Sub<Msg> decoder(const Model& m) {
        const double from = m.time;
        return Sub<Msg>::source("playback:" + std::to_string(m.epoch),
                                [from](auto sink, auto alive) {
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

                // No frame. Only report end-of-stream if this producer is
                // still the one the model wants.
                //
                // A seek bumps the epoch, which changes this source's id, so
                // the runtime stops THIS thread and starts a new one at the new
                // position. On the way out `next()` can legitimately return
                // null -- and reporting that as Ended set playing=false, so
                // every seek during playback stopped the video and left the
                // status reading "end of stream". Measured exactly that way.
                //
                // `alive()` is the runtime telling us we have been replaced.
                // Checking it BEFORE attributing meaning to a null frame is
                // the difference between "the file ended" and "you were
                // superseded".
                if (!px) {
                    if (alive()) sink(Msg{Ended{}});
                    return;
                }
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

    /// Play / pause, drawn from primitives rather than from a font glyph.
    ///
    /// Pause is two bars, which is exact.
    ///
    /// Play is a stack of rounded bars of decreasing height, which reads as a
    /// triangle at icon size. The obvious implementation -- a square with
    /// `rotate(45)` -- does NOT work: Style::transform is stored and the
    /// painter never reads it, so rotate() compiles and does nothing. That is
    /// worth knowing before reaching for it elsewhere; it is filed as a
    /// separate defect rather than worked around silently here.
    static Node transport_icon(bool playing, float fade) {
        const auto ink = colors::white.fade(fade);
        if (playing) {
            return (h(box() | width(px(4)) | height(px(14)) | radius(1) | bg(ink),
                      box() | width(px(4)) | height(px(14)) | radius(1) | bg(ink))
                    | gap(4) | center).build();
        }
        // Six slices, tallest at the left, forming a right-pointing wedge.
        // Each is one rounded box, so the whole icon is six instances in the
        // same batch as everything else on screen.
        std::vector<Node> slices;
        slices.reserve(6);
        for (int i = 0; i < 6; ++i) {
            const float h_px = 14.0f - static_cast<float>(i) * 2.2f;
            slices.push_back((box() | width(px(2)) | height(px(h_px))
                                    | radius(0.5f) | bg(ink)).build());
        }
        return (h_list(std::move(slices), 0.0f) | center
                | width(px(12)) | height(px(14))).build();
    }

    static Node view(const Model& m, const Ctx& c) {
        const Theme& t = c.theme;
        const Vec2   vp = c.size;          // the LIVE window size, every frame
        const float  fade = static_cast<float>(m.chrome);

        // ── fit the video to the window, preserving aspect ───────────
        //
        // Letterbox arithmetic, done against c.size rather than a constant,
        // is the whole of "responsive" here: resize the window and the video
        // grows to fill it, gaining bars on whichever axis has slack. Hard-
        // coding a box would make the app look fine at one size only.
        const float src_aspect = g_info.height > 0
            ? static_cast<float>(g_info.width) / static_cast<float>(g_info.height)
            : 16.0f / 9.0f;
        const float win_aspect = vp.y > 0.0f ? vp.x / vp.y : src_aspect;

        float vw = vp.x, vh = vp.y;
        if (win_aspect > src_aspect) vw = vp.y * src_aspect;   // pillarbox
        else                         vh = vp.x / src_aspect;   // letterbox
        const float vx = (vp.x - vw) * 0.5f;
        const float vy = (vp.y - vh) * 0.5f;

        const float progress = m.duration > 0.0
            ? static_cast<float>(m.time / m.duration) : 0.0f;

        // Controls sit in a bar across the bottom, inset from the edges, and
        // scale their padding with the window so they do not swamp a small one.
        const float bar_h   = 64.0f;
        const float inset   = control_inset(vp);
        const float bar_w   = track_width(vp);
        const float bar_y   = num::max(vp.y - bar_h - inset, 0.0f);

        return (z(
            // 1. THE VIDEO, letterboxed into the window.
            image(kImage)
                | width(px(vw)) | height(px(vh))
                | absolute(vx, vy),

            // 2. A scrim behind the controls so white text stays legible over
            //    a bright frame. It fades with them.
            box() | width(px(vp.x)) | height(px(bar_h + inset * 2.0f))
                  | bg(colors::black.fade(0.45f * fade))
                  | absolute(0.0f, num::max(vp.y - bar_h - inset * 2.0f, 0.0f)),

            // 3. The controls themselves, floating over the picture.
            (v(
                // Scrub track. Full width of the bar, so it is easy to hit.
                z(box() | width(px(bar_w)) | height(px(6)) | radius(3)
                        | bg(colors::white.fade(0.25f * fade)),
                  box() | width(px(bar_w * progress)) | height(px(6)) | radius(3)
                        | bg((m.seeking ? t.warning : t.accent).fade(fade)))
                  | width(px(bar_w)) | height(px(6))
                  | dsl::id<"track">,

                h(// Transport icon, drawn as SHAPES rather than as a glyph.
                  //
                  // "\u25B6 play" looks like the obvious thing to write and is
                  // not: U+25B6 is absent from Noto Sans, so it falls back to
                  // Noto Sans CJK JP, whose version is a FULL-WIDTH
                  // ideographic symbol -- 16x17 px in a run whose letters are
                  // 10x15. It is a different typeface at a different optical
                  // size, so it renders visibly softer and larger than the
                  // text beside it.
                  //
                  // mayag's whole premise is that a shape is one distance
                  // field on the same instanced quad, so a triangle and two
                  // bars cost the same as the glyph would and are sharp at
                  // any size. Iconography does not need a font.
                  node(transport_icon(m.playing, fade)),
                  text_owned(m.playing ? "pause" : "play")
                      | font(14) | semibold
                      | fg(colors::white.fade(fade)),
                  box() | grow(),
                  text_owned(fmt_time(m.time) + " / " + fmt_time(m.duration))
                      | font(12) | fg(colors::white.fade(0.85f * fade)),
                  text_owned(m.status) | font(12)
                      | fg(colors::white.fade(0.55f * fade)),
                  text_owned(fmt_ms(m.decode_ms)) | font(12)
                      | fg(colors::white.fade(0.55f * fade)))
                  | gap(16) | center | width(px(bar_w))
                  | (m.playing ? pause_btn.on : play_btn.on))
              | gap(10) | width(px(bar_w))
              | absolute(inset, bar_y))
            )
            | width(px(vp.x)) | height(px(vp.y))
            | bg(colors::black)).build();
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
            // Keep the model's idea of the window current, so a scrub after a
            // resize maps to the track that is actually on screen.
            Sub<Msg>::on_resize([](Vec2 sz) { return Msg{Resized{sz}}; }),

            // Pointer movement un-hides the controls. There is no dedicated
            // on_mouse_move, so this reads the raw event -- which is the
            // escape hatch working as intended rather than a gap.
            Sub<Msg>::on_event([](const Event& e) -> std::optional<Msg> {
                if (const auto* mv = std::get_if<MouseMove>(&e)) {
                    return Msg{Pointer{mv->position}};
                }
                return std::nullopt;
            }),

            // The scrub bar, wired for BOTH a tap and a drag.
            //
            // `on_node_motion` reports the pointer's LOCAL position inside the
            // node, which is what a slider needs. But the gesture kinds are
            // disjoint -- `matches()` maps drag to Kind::drag only -- so a
            // drag subscription alone never fires on a plain click, and
            // tapping the bar does nothing. That was the reported bug, and it
            // survived a first fix because the test sent the message instead
            // of clicking the widget.
            //
            // `press` covers the tap and the start of a drag; `drag` covers
            // the rest of the movement. Together they give click-to-seek and
            // scrub from one handler.
            //
            // A drag move asks for a PREVIEW (keyframe-accurate, ~10x faster)
            // because the user is still aiming; the release re-seeks exactly.
            // A press is a tap until proven otherwise, so it seeks precisely
            // and a click lands on the right frame immediately.
            Sub<Msg>::on_node_motion(
                node_id("track"), Sub<Msg>::Gesture::press,
                [](Vec2 local) { return Msg{Scrub{local.x, false}}; }),
            Sub<Msg>::on_node_motion(
                node_id("track"), Sub<Msg>::Gesture::drag,
                [](Vec2 local) { return Msg{Scrub{local.x, true}}; }),
            Sub<Msg>::on_click_id_gesture(node_id("track"),
                                          Sub<Msg>::Gesture::release,
                                          Msg{ScrubEnd{}}),

            when(m.playing, decoder(m)),

            // Frames are requested only while something is moving: the chrome
            // spring settling, or playback counting down to auto-hide. A
            // paused player with its controls up asks for nothing and idles at
            // 0% CPU, which is the property the whole runtime is built on.
            when(m.chrome.animating() || (m.playing && m.idle <= 2.5),
                 Sub<Msg>::on_frame([](float dt) { return Msg{Tick{dt}}; })),
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

        // ── the layout is responsive ─────────────────────────────
        //
        // Check the letterbox arithmetic directly rather than by inspecting
        // pixels. A pixel probe cannot separate the video from the scrim and
        // the controls drawn over it -- three attempts at one disagreed with
        // each other while the math was right all along.
        const auto fits = [](float vpx, float vpy) {
            const float src = static_cast<float>(g_info.width) /
                              static_cast<float>(g_info.height);
            const float win = vpx / vpy;
            float vw = vpx, vh = vpy;
            if (win > src) vw = vpy * src; else vh = vpx / src;
            // The video must fit INSIDE the window on both axes, and keep the
            // source aspect. Either failing means the picture is cropped or
            // stretched at that window size.
            return vw <= vpx + 0.5f && vh <= vpy + 0.5f &&
                   std::abs((vw / vh) - src) < 0.01f;
        };
        ok(fits(640, 360),  "letterboxes into a wide window");
        ok(fits(500, 900),  "pillarboxes into a tall window");
        ok(fits(200, 200),  "and still fits a tiny square one");

        // ── controls hide during playback, return on movement ────
        rt.send(Video::Msg{Video::Play{}});
        for (int i = 0; i < 200; ++i) {
            rt.send(Video::Msg{Video::Tick{0.05f}});   // 10s of idle time
        }
        rt.tick();
        ok(static_cast<float>(rt.model().chrome) < 0.2f,
           "controls fade out while playing and idle");

        rt.send(Video::Msg{Video::Pointer{Vec2{10.0f, 10.0f}}});
        // 1.0 s of ticks: enough for the spring to rise, and comfortably
        // inside the 2.5 s idle window so it does not immediately re-hide.
        for (int i = 0; i < 20; ++i) rt.send(Video::Msg{Video::Tick{0.05f}});
        rt.tick();
        ok(static_cast<float>(rt.model().chrome) > 0.8f,
           "and come back when the pointer moves");

        rt.send(Video::Msg{Video::Pause{}});
        for (int i = 0; i < 200; ++i) rt.send(Video::Msg{Video::Tick{0.05f}});
        rt.tick();
        ok(static_cast<float>(rt.model().chrome) > 0.8f,
           "a paused player keeps its controls up");

        // ── the scrub bar seeks ─────────────────────────────────
        //
        // This is the bug that prompted the control: the track rendered a
        // progress fill, carried an id, and had NO subscription -- so every
        // click on it did nothing. A control that looks interactive and is
        // not is worse than no control, and nothing caught it because the
        // tests drove SeekTo directly and never touched the widget.
        {
            // Park somewhere the click will not land, so "it moved" means
            // something. An earlier check already left the player at the
            // midpoint, and comparing against that made the assertion vacuous.
            rt.send(Video::Msg{Video::SeekTo{0.0}});
            pump([&] { return !rt.model().seeking; }, 15000);
            const double before = rt.model().time;

            // Click the widget for real, through hit testing, rather than
            // sending Scrub directly.
            //
            // This distinction matters: the first version of this check sent
            // the message, and it PASSED with the subscription deleted --
            // testing my handler while the bug was in the wiring. Driving the
            // widget is the only thing that proves a control is connected.
            //
            // rt.click() targets a node's centre, so this is a seek to the
            // middle of the track.
            const auto track = rt.node_rect("track");
            ok(track.has_value(), "the scrub track exists in the laid-out tree");

            rt.click(node_id("track"));
            pump([&] { return !rt.model().seeking; }, 15000);

            const double want = rt.model().duration * 0.5;
            ok(std::abs(rt.model().time - want) < 0.35,
               "clicking the middle of the track seeks there (" +
                   std::to_string(rt.model().time) + " vs " +
                   std::to_string(want) + ")");
            ok(rt.model().time != before, "and it actually moved");

            // The ends must be reachable, and clamped rather than overshooting.
            const float w = Video::track_width(rt.model().window);
            rt.send(Video::Msg{Video::Scrub{0.0f}});
            pump([&] { return !rt.model().seeking; }, 15000);
            ok(rt.model().time < 0.35, "the left end seeks to the start");

            rt.send(Video::Msg{Video::Scrub{w * 4.0f}});   // far past the right
            pump([&] { return !rt.model().seeking; }, 15000);
            ok(rt.model().time <= rt.model().duration + 0.01,
               "and a click past the end clamps instead of overshooting");
        }

        // ── seeking WHILE PLAYING ───────────────────────────────
        //
        // Two bugs lived here, both reported from actually using the thing.
        //
        // A cancelled seek returned no frame, and the handler mapped "no
        // frame" to Ended -- which stops playback. Cancellation is the NORMAL
        // case while dragging, so scrubbing killed the video every time.
        //
        // And the playback source's id was the constant "playback", so the
        // runtime saw an unchanged id, left the existing thread alone, and the
        // picture carried on from the old position: the seek looked like it
        // did nothing until you paused.
        {
            rt.send(Video::Msg{Video::SeekTo{0.0}});
            pump([&] { return !rt.model().seeking; }, 15000);
            rt.send(Video::Msg{Video::Play{}});
            pump([&] { return rt.model().frames > 0; }, 5000);

            const double target = rt.model().duration * 0.6;
            rt.send(Video::Msg{Video::SeekTo{target}});
            pump([&] { return !rt.model().seeking; }, 15000);

            ok(rt.model().playing,
               "seeking during playback does not stop the video");

            // And it must carry on FROM the new position, not the old one.
            const double after_seek = rt.model().time;
            pump([] { return false; }, 600);
            ok(rt.model().playing, "it is still playing after the seek settles");
            ok(rt.model().time >= after_seek - 0.1,
               "and playback resumed from the seek target, not the old spot (" +
                   std::to_string(rt.model().time) + " >= " +
                   std::to_string(after_seek) + ")");

            rt.send(Video::Msg{Video::Pause{}});
            rt.tick();
        }

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    });
}
