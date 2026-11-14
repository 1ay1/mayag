// examples/video.cpp — play a real video file.
//
// The point of this example is NOT that mayag decodes video. It does not, and
// should not: a UI framework that hard-depends on ffmpeg is a worse UI
// framework. The point is that decoding is ORDINARY APPLICATION WORK, and the
// framework needs no special support for it.
//
// Everything here is the same architecture as a counter:
//
//   the decoder        a jaal Sub::stream, so it exists exactly while playing
//                      and is stopped on pause
//   a decoded frame    an ordinary Msg carrying jaal::shared<ImageData>, so an
//                      8 MB 1080p frame moves by pointer, not by copy
//   showing it         the upload_image effect + image(id) in the view
//   seeking            at most one decode running, the newest request waiting
//                      in the model, so a drag still gets frames
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
    /// The buffer is freshly allocated per frame and moved out, so the caller
    /// can wrap it in a jaal::shared and send it without a copy.
    /// `pts_out` is the presentation time in seconds, so the caller can
    /// pace playback against a real clock rather than assuming a frame rate.
    /// `max_w` caps the output width, so the decoder produces pixels at the
    /// size they will be DRAWN rather than the size they were encoded. Zero
    /// means source resolution. It never upscales.
    [[nodiscard]] std::optional<ImageData> next(double* pts_out,
                                                int max_w = 0) {
        if (ctx_ == nullptr) return std::nullopt;

        for (;;) {
            int rc = avcodec_receive_frame(ctx_, frame_);
            if (rc == 0) {
                if (pts_out != nullptr) {
                    const AVStream* st = fmt_->streams[stream_];
                    const int64_t pts = frame_->best_effort_timestamp;
                    *pts_out = pts == AV_NOPTS_VALUE
                             ? 0.0 : static_cast<double>(pts) * av_q2d(st->time_base);
                }
                return to_rgba(max_w);
            }
            if (rc != AVERROR(EAGAIN)) return std::nullopt;   // EOF or hard error

            // Need more input.
            av_packet_unref(pkt_);
            if (av_read_frame(fmt_, pkt_) < 0) {
                avcodec_send_packet(ctx_, nullptr);      // flush
                if (avcodec_receive_frame(ctx_, frame_) == 0) return to_rgba(max_w);
                return std::nullopt;
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
    /// `still_wanted` lets a caller abandon that walk (TRUE means keep going).
    /// One seek can decode ~180 frames, so a caller that no longer wants the
    /// result should not pay for it.
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
    [[nodiscard]] std::optional<ImageData> seek(
            double seconds, double* pts_out, bool precise = true,
            int max_w = 0,
            const std::function<bool()>& still_wanted = {}) {
        if (ctx_ == nullptr) return std::nullopt;
        const AVStream* st = fmt_->streams[stream_];
        const int64_t ts = static_cast<int64_t>(seconds / av_q2d(st->time_base));
        if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0) return std::nullopt;
        avcodec_flush_buffers(ctx_);

        std::optional<ImageData> last;
        double pts = 0.0;
        // A keyframe interval is rarely more than a few hundred frames; the
        // cap is generous enough to cross one and small enough to bound the
        // work if the file lies about its timestamps.
        for (int guard = 0; guard < 600; ++guard) {
            // Superseded by a newer seek: stop rather than finish work whose
            // result is already destined for the bin.
            if (still_wanted && !still_wanted()) return std::nullopt;
            auto frame = next(&pts, max_w);
            if (!frame) break;
            last = std::move(frame);
            if (!precise) break;                 // the keyframe is the answer
            if (pts >= seconds - 0.001) break;   // reached the target
        }
        if (pts_out != nullptr) *pts_out = pts;
        return last;
    }

  private:
    /// Convert the current frame to RGBA, optionally scaling to `max_w`.
    ///
    /// Scaling HERE rather than at draw time is the point: sws_scale is
    /// already running, so a smaller destination makes it cheaper AND makes
    /// the buffer that crosses to the UI thread smaller. Measured on 1080p:
    /// 3.27 ms / 7.91 MB at source, 1.14 ms / 1.59 MB at 860 wide. The
    /// pixels dropped are ones the sampler would have discarded anyway.
    [[nodiscard]] std::optional<ImageData> to_rgba(int max_w = 0) {
        const int sw = ctx_->width, sh = ctx_->height;
        // Never scale UP -- that costs time and adds no detail.
        const bool shrink = max_w > 0 && max_w < sw;
        const int  w = shrink ? max_w : sw;
        const int  h = shrink ? num::max(sh * max_w / sw, 1) : sh;
        sws_ = sws_getCachedContext(sws_, sw, sh, ctx_->pix_fmt,
                                    w, h, AV_PIX_FMT_RGBA,
                                    // A preview is resampled once and looked
                                    // at for a few milliseconds; bilinear's
                                    // quality is not worth its cost here.
                                    shrink ? SWS_FAST_BILINEAR : SWS_BILINEAR,
                                    nullptr, nullptr, nullptr);
        if (sws_ == nullptr) return std::nullopt;

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
        if (need <= 0) return std::nullopt;

        ImageData img;
        img.width = w;
        img.height = h;
        // The buffer is at least `need`; the tail beyond w*h*4 is scratch the
        // scaler is allowed to touch.
        img.rgba.resize(static_cast<std::size_t>(need));

        std::uint8_t* dst[4]{};
        int stride[4]{};
        if (av_image_fill_arrays(dst, stride, img.rgba.data(),
                                 AV_PIX_FMT_RGBA, w, h, kAlign) < 0) {
            return std::nullopt;
        }
        sws_scale(sws_, frame_->data, frame_->linesize, 0, sh, dst, stride);

        // mayag's ImageData is tightly packed, so drop any row padding ffmpeg
        // asked for before handing the pixels over.
        const std::size_t tight = static_cast<std::size_t>(w) * 4;
        if (static_cast<std::size_t>(stride[0]) != tight) {
            for (int y = 1; y < h; ++y) {
                std::memmove(img.rgba.data() + y * tight,
                             img.rgba.data() + static_cast<std::size_t>(y) * stride[0],
                             tight);
            }
        }
        img.rgba.resize(tight * static_cast<std::size_t>(h));
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
        /// True once a DRAG has shown a keyframe preview, so the release knows
        /// it owes a precise seek. False for a plain click, which already
        /// seeked precisely and must not pay for a second decode.
        bool   previewing = false;
        /// The size the picture is actually drawn at, in physical pixels.
        ///
        /// Decoding to this instead of to the source resolution is the
        /// single largest saving in the seek path: a 1080p frame costs
        /// 3.27 ms to scale and 7.91 MB to upload, against 1.14 ms and
        /// 1.59 MB at an 860px window size. The extra pixels are discarded
        /// by the sampler, so they buy nothing.
        ///
        /// Zero means "not known yet" -- the first frame decodes at source
        /// size, and every frame after uses the measured box.
        int    view_w = 0;
        int    view_h = 0;
        /// True while the window is filling the screen. Tracked because the
        /// Cmd is a one-way instruction -- the runtime is told what to do, not
        /// asked what it did -- so the app owns the toggle's state.
        bool   fullscreen = false;
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
        /// Bumped by every seek. It is part of the playback stream's key, so a
        /// seek replaces the decoder rather than leaving it running
        /// from the old position. See decoder().
        int    epoch = 0;
        /// Seek coalescing: at most one seek decode runs; newer requests
        /// collapse into one pending slot, started when the running one ends.
        struct PendingSeek { double seconds; bool preview; int epoch; };
        bool   seek_busy = false;
        std::optional<PendingSeek> seek_pending;
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
    /// A decoded picture, stamped with the seek EPOCH it was produced for.
    ///
    /// The stamp is what makes a frame's provenance checkable. Frames already
    /// posted by a decoder that has since been stopped are still sitting in
    /// the inbox when a seek is processed, and they arrive AFTER it -- so
    /// assigning m.time unconditionally let a frame from the old position
    /// overwrite the seek target. The scrub bar moved, the picture did not,
    /// and you had to click again. Measured 4-6 of 12 clicks lost that way.
    struct Frame {
        jaal::shared<ImageData> pixels;
        double pts;
        float  ms;
        int    epoch = 0;
    };
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
    /// The running seek decode finished (its frame, if any, came first).
    struct SeekSettled {};
    /// Double-click on the picture: fill the screen, or come back.
    struct ToggleFullscreen {};
    struct Resized { Vec2 size; };
    struct Tick { float dt; };
    struct Quit {};
    using Msg = std::variant<Play, Pause, SeekTo, Frame, Ended, SeekAbandoned,
                             Pointer, Scrub, ScrubEnd, SeekSettled,
                             ToggleFullscreen, Resized, Tick, Quit>;
    using Cmd = jaal::Cmd<Msg, set_title, set_fullscreen, upload_image>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model& m) {
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
        std::optional<ImageData> first;
        double pts = 0.0;
        if (d.open(g_path)) first = d.seek(0.0, &pts);

        Cmd title = SetTitle{"mayag — " + g_path};
        if (!first) return title;
        m.frames = 1;
        m.time   = pts;
        return Cmd::batch(std::move(title), upload(kImage, std::move(*first)));
    }

    static Cmd update(Model& m, Play)  { m.playing = true; return {}; }   // the Sub starts the decoder
    static Cmd update(Model& m, Pause) { m.playing = false; return {}; }

    /// Start one seek decode. It sends its Frame (or Ended), then SeekSettled.
    static Cmd start_seek(Model& m, double want, bool preview) {
        m.seek_busy = true;
        return Cmd::task("seek",
            [](jaal::Sink<Msg> out, double at, int epoch, int max_w, bool pv) {
                Decoder d;                   // owned by this task alone
                if (!d.open(g_path)) { out.send(Ended{}); out.send(SeekSettled{}); return; }
                double pts = 0.0;
                const auto t0 = std::chrono::steady_clock::now();
                // No cancel predicate: this decode is the only one running and
                // its frame is wanted. Decode straight to the drawn size.
                auto px = d.seek(at, &pts, !pv, max_w);
                const float ms = std::chrono::duration<float, std::milli>(
                                     std::chrono::steady_clock::now() - t0).count();
                if (px) out.send(Frame{jaal::shared<ImageData>::make(std::move(*px)), pts, ms, epoch});
                else    out.send(Ended{});
                out.send(SeekSettled{});
            },
            want, m.epoch, m.view_w, preview);
    }

    static Cmd update(Model& m, SeekTo s) {
        m.time = num::clamp(s.seconds, 0.0, m.duration);
        m.seeking = true;
        // New epoch -> new playback stream key, so a seek while playing
        // restarts the decoder at the new position. A PREVIEW (pointer drag)
        // does not bump it: restarting the decoder on every move buys nothing,
        // the release fires a precise SeekTo that restarts it once.
        if (!s.preview) ++m.epoch;
        // Coalesce, not latest-wins: one decode runs at a time and finishes;
        // newer requests collapse into one pending slot. A 1080p seek costs
        // ~500 ms and a drag asks ~60x/s, so latest-wins never finished one
        // and the picture froze until the mouse stopped.
        if (m.seek_busy) {
            m.seek_pending = Model::PendingSeek{m.time, s.preview, m.epoch};
            return {};
        }
        return start_seek(m, m.time, s.preview);
    }

    static Cmd update(Model& m, Frame f) {
                // Drop frames produced for a superseded seek.
                //
                // Not defensive coding: the decoder that produced this frame
                // may already have been stopped, its message queued before it
                // noticed. Applying it rewinds m.time to wherever playback had
                // got to. jaal drops most of these already, but a seek task
                // that finishes after a newer seek still delivers.
                if (f.epoch != m.epoch) return {};
                m.time      = f.pts;
                m.decode_ms = f.ms;
                ++m.frames;
                // NOT `m.seeking = false`: this frame may belong to a
                // superseded request. SeekSettled owns that flag.
                return upload(kImage, std::move(f.pixels));
    }

    static Cmd update(Model& m, SeekSettled) {
        // The running decode finished. Start the pending one, if any;
        // otherwise the frame on screen is the one that was asked for.
        m.seek_busy = false;
        if (m.seek_pending) {
            const auto p = *m.seek_pending;
            m.seek_pending.reset();
            return start_seek(m, p.seconds, p.preview);
        }
        m.seeking = false;
        return {};
    }

    static Cmd update(Model& m, Ended) {
                m.playing = false;
                m.status  = "end of stream";
                m.chrome.to(1.0f);          // always show controls when stopped
                return {};
    }

    static Cmd update(Model&, SeekAbandoned) {
        // A newer scrub superseded this decode. Leave `seeking` set.
        return {};
    }

    static Cmd update(Model& m, Pointer p) {
                m.pointer = p.at;
                m.idle    = 0.0;
                m.chrome.to(1.0f);
                return {};
    }

    static Cmd update(Model& m, Scrub s) {
                // Local x inside the track -> a time.
                //
                // The track's width is derived from the last known window
                // size using the SAME formula the view uses, rather than
                // stored: view() is pure and cannot write to the model, and
                // duplicating one expression is better than a mutable field
                // that can silently go stale for a frame after a resize.
                const float w = track_width(m.window);
                if (w <= 1.0f || m.duration <= 0.0) return {};

                const double frac =
                    num::clamp(static_cast<double>(s.local_x) / static_cast<double>(w), 0.0, 1.0);
                m.idle = 0.0;
                m.chrome.to(1.0f);
                // Reuse the seek path, which is already coalescing. While the
                // button is held this asks for a cheap keyframe preview: a
                // 10x faster decode the user can aim with. ScrubEnd re-seeks
                // precisely once they let go.
                if (s.dragging) m.previewing = true;
                return update(m, SeekTo{frac * m.duration, s.dragging});
    }

    static Cmd update(Model& m, ScrubEnd) {
                // Re-seek precisely ONLY if the previews during a drag left the
                // picture keyframe-accurate. A plain click never previewed --
                // press seeks precisely and is already correct -- so firing a
                // seek here anyway made every click decode the same frame
                // TWICE, doubling the latency of the most common interaction
                // (measured 92 ms to 172 ms, 228 ms to 412 ms).
                if (!m.previewing) return {};
                m.previewing = false;
                return update(m, SeekTo{m.time, false});
    }

    static Cmd update(Model& m, ToggleFullscreen) {
                m.fullscreen = !m.fullscreen;
                // Keep the chrome up for a moment on the way in and out: the
                // window is about to resize under the pointer, and controls
                // that vanish at the same time read as the app losing state.
                m.idle = 0.0;
                m.chrome.to(1.0f);
                // The new size arrives as a Resized, which recomputes view_w,
                // so the decode resolution follows the window without this
                // handler knowing anything about it.
                return SetFullscreen{m.fullscreen};
    }

    static Cmd update(Model& m, Resized r) {
                m.window = r.size;
                // The size the picture will be drawn at, derived with the
                // same letterbox rule the view uses. Decoding to this instead
                // of to the source resolution is the largest single saving in
                // the seek path -- 3.27 ms/7.91 MB becomes 1.14 ms/1.59 MB on
                // a 1080p file in an 860px window -- and the pixels dropped
                // are ones the sampler would have thrown away.
                //
                // Derived rather than read back from layout: it is a pure
                // function of the window and the source aspect, so computing
                // it here keeps update() self-contained and avoids depending
                // on a frame having already been laid out.
                const float src = g_info.height > 0
                    ? static_cast<float>(g_info.width) /
                      static_cast<float>(g_info.height)
                    : 16.0f / 9.0f;
                const float win = r.size.y > 0.0f ? r.size.x / r.size.y : src;
                const float vw = win > src ? r.size.y * src : r.size.x;
                // Clamped to the source width: asking for more pixels than
                // the file has costs scaling time and invents no detail. A
                // fullscreen window on a 2560px display would otherwise ask a
                // 1920px file to upscale on every frame.
                m.view_w = num::min(static_cast<int>(vw), g_info.width);
                m.view_h = static_cast<int>(static_cast<float>(m.view_w) / src);
                return {};
    }

    static Cmd update(Model& m, Tick t) {
                m.chrome.step(t.dt);
                // Only playback hides the chrome. A paused player keeps its
                // controls up, because the user is looking at them.
                if (m.playing) {
                    m.idle += static_cast<double>(t.dt);
                    if (m.idle > 2.5) m.chrome.to(0.0f);
                } else {
                    m.idle = 0.0;
                    m.chrome.to(1.0f);
                }
                return {};
    }

    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    /// The playback decoder: a stream, so it exists exactly while `playing`
    /// is true and jaal stops it when it is not.
    ///
    /// The key carries the seek EPOCH. An unchanged key means "same producer,
    /// leave it alone", so with a constant key a seek while playing did
    /// nothing. With the epoch folded in, a seek is a DIFFERENT stream: jaal
    /// stops the old one and starts one at the new time.
    static Sub decoder(const Model& m) {
        return Sub::stream("playback:" + std::to_string(m.epoch),
            [](jaal::Sink<Msg> out, std::stop_token st, double from, int epoch, int max_w) {
            Decoder d;                       // owned by this thread alone
            if (!d.open(g_path)) { out.send(Ended{}); return; }
            double ignored = 0.0;
            if (from > 0.05) (void)d.seek(from, &ignored);

            using clk = std::chrono::steady_clock;
            const auto started = clk::now();
            double first_pts = -1.0;

            while (!st.stop_requested()) {
                double pts = 0.0;
                const auto t0 = clk::now();
                std::optional<ImageData> px = d.next(&pts, max_w);

                // No frame. Report end-of-stream only if we were not stopped:
                // a stopped (superseded) decoder can see a null frame on the
                // way out, and reporting that as Ended stopped playback on
                // every seek.
                if (!px) {
                    if (!st.stop_requested()) out.send(Ended{});
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
                if (st.stop_requested()) return;
                out.send(Frame{jaal::shared<ImageData>::make(std::move(*px)), pts, ms, epoch});
            }
        },
            m.time, m.epoch, m.view_w);
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

    static Node view(const Model& m) {
        const Ctx& c = ctx();
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
            //
            // Named so a double-click can hit it. The controls sit ON TOP in
            // this z-stack, so a double-click on the scrub bar is claimed by
            // the bar and never reaches here -- which is what you want:
            // dragging the scrub bar quickly must not toggle fullscreen.
            image(kImage)
                | width(px(vw)) | height(px(vh))
                | absolute(vx, vy)
                | dsl::id<"picture">,

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
                  | id_of(m.playing ? "pause" : "play"))
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

    // ── input ───────────────────────────────────────────────────────────
    static std::optional<Msg> on(const Model& m, const KeyEvent& k) {
        if (k.key == Key::space) return m.playing ? Msg{Pause{}} : Msg{Play{}};
        // Escape leaves fullscreen before it quits: in fullscreen there is no
        // title bar, so a key that quits outright is a trap.
        if (k.key == Key::escape) return m.fullscreen ? Msg{ToggleFullscreen{}} : Msg{Quit{}};
        if (k.key == Key::f) return ToggleFullscreen{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }
    // Keep the model's idea of the window current, so a scrub after a resize
    // maps to the track that is actually on screen.
    static std::optional<Msg> on(const Model&, const ResizeEvent& r) { return Resized{r.size}; }
    // Pointer movement un-hides the controls.
    static std::optional<Msg> on(const Model&, const MouseMove& mv) { return Pointer{mv.position}; }

    // The scrub bar, wired for BOTH a tap and a drag. A press is a tap until
    // proven otherwise, so it seeks precisely; a drag move asks for a PREVIEW
    // (keyframe-accurate, ~10x faster); the release re-seeks exactly.
    static std::optional<Msg> on(const Model&, const Pressed& p) {
        if (is(p, "track")) return Scrub{p.local.x, false};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Dragged& d) {
        if (is(d, "track")) return Scrub{d.local.x, true};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Released& r) {
        if (is(r, "track")) return ScrubEnd{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Clicked& c) {
        if (c.count == 1 && is(c, "play"))  return Play{};
        if (c.count == 1 && is(c, "pause")) return Pause{};
        // Double-click the picture (not the window) to fill the screen, so a
        // fast double-click on the bar is not mistaken for a toggle.
        if (c.count == 2 && is(c, "picture")) return ToggleFullscreen{};
        return std::nullopt;
    }

    static Sub subscribe(const Model& m) {
        return Sub::batch(
            // The playback decoder runs while playing AND not mid-drag.
            // During a drag the picture comes from the seek task; a decoder
            // still running would race it and overwrite m.time.
            when(m.playing && !m.previewing, decoder(m)),
            // Frames are requested only while something is moving: the chrome
            // spring settling, or playback counting down to auto-hide. A
            // paused player with its controls up idles at 0% CPU.
            when(m.chrome.animating() || (m.playing && m.idle <= 2.5),
                 Sub::on_frame([](double dt) -> Msg { return Tick{static_cast<float>(dt)}; })));
    }
};

static_assert(mayag::Program<Video>);

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

            // A click must cost ONE decode, not two.
            //
            // press seeks precisely and release fired a second precise seek
            // to the same spot, so the most common interaction in the app
            // decoded the same frame twice and took twice as long to respond
            // (measured 92 -> 172 ms, 228 -> 412 ms on a 1080p file). The
            // release only owes a seek if a DRAG left keyframe previews on
            // screen, which a plain click never does.
            {
                const int decodes_before = rt.model().frames;
                rt.click(node_id("track"));
                pump([&] { return !rt.model().seeking; }, 15000);
                // Let a redundant second decode land if one was queued.
                for (int i = 0; i < 400; ++i) {
                    rt.tick();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                ok(rt.model().frames - decodes_before == 1,
                   "a single click decodes exactly one frame (" +
                       std::to_string(rt.model().frames - decodes_before) + ")");
            }

            // ── every click lands, while PLAYING ─────────────────
            //
            // The reported bug: the scrub bar jumps to where you clicked but
            // the video keeps playing from the old spot, so you click again
            // and again before it takes.
            //
            // Cause: the playback decoder posts frames continuously, so when
            // a seek is processed the inbox ALREADY holds frames it queued
            // before being stopped. They are applied after the seek and
            // Frame{} assigned m.time unconditionally, rewinding it. No
            // running thread to blame -- the messages were already in flight.
            //
            // One click hid it: both the press-seek and the stale frame
            // targeted nearby positions. Only REPEATED clicks show it, which
            // is exactly how it was reported. Measured 4-6 of 12 lost.
            {
                rt.send(Video::Msg{Video::Play{}});
                pump([&] { return rt.model().time > 0.5; }, 15000);

                int landed = 0;
                constexpr int kClicks = 6;
                for (int i = 0; i < kClicks; ++i) {
                    const double frac = 0.15 + 0.09 * i;
                    const double want = rt.model().duration * frac;
                    rt.send(Video::Msg{Video::SeekTo{want, false}});
                    // Let the seek land and playback carry on a little.
                    const auto until = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(1200);
                    while (std::chrono::steady_clock::now() < until) {
                        rt.tick();
                        std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    }
                    // Playing, so at or just after the target -- never back
                    // at the position it was playing from before.
                    const double got = rt.model().time;
                    if (got >= want - 1.0 && got < want + 15.0) ++landed;
                }
                ok(landed == kClicks,
                   "every seek lands while playing, no repeat clicks needed (" +
                       std::to_string(landed) + "/" +
                       std::to_string(kClicks) + ")");
                rt.send(Video::Msg{Video::Pause{}});
            }

            // ── double-click the picture to go fullscreen ────────
            //
            // Asserted against the WINDOW, not the model's bool. Cmd::fullscreen
            // existed as a documented API that the runtime never handled, so a
            // test checking only m.fullscreen passed while the command was
            // dropped on the floor -- the app looked correct and nothing
            // happened on screen.
            {
                const auto pic = rt.node_rect("picture");
                ok(pic.has_value(), "the picture is in the laid-out tree");

                const int before = rt.window().fullscreen_calls();
                const Vec2 c = pic->center();
                rt.window().push(MouseMove{c, {}, {}});
                rt.window().push(MouseDown{c, MouseButton::left, {}, 2});
                rt.window().push(MouseUp{c, MouseButton::left, {}});
                rt.tick();

                ok(rt.window().fullscreen_calls() == before + 1,
                   "a double-click reaches the window as a fullscreen request");
                ok(rt.window().fullscreen(), "and it asks to go fullscreen");

                // And it toggles back, rather than being a one-way trip.
                rt.window().push(MouseMove{c, {}, {}});
                rt.window().push(MouseDown{c, MouseButton::left, {}, 2});
                rt.window().push(MouseUp{c, MouseButton::left, {}});
                rt.tick();
                ok(!rt.window().fullscreen(),
                   "a second double-click leaves fullscreen");

                // And again with the count UNREPORTED, which is what Wayland
                // actually sends -- it has no multi-click protocol, so the
                // interaction layer must derive the count from timing.
                //
                // This is the case that was broken in the real app while the
                // test above passed: wayland.hpp hardcoded click_count = 1,
                // which reads as "definitely a single click" and suppressed
                // synthesis entirely. Passing 2 explicitly, as the test above
                // does, exercises the platform-reported path that only macOS
                // takes. Both paths need a test or one of them rots.
                const int synth_before = rt.window().fullscreen_calls();
                // Break the chain from the clicks above first: the count is
                // cumulative, so without a gap this run continues to 3 and 4
                // rather than starting a fresh 1, 2.
                rt.tick();
                rt.window().push(MouseMove{Vec2{5.0f, 5.0f}, {}, {}});
                rt.tick();

                rt.window().set_tick_seconds(0.02);   // inside the 0.4 s window
                for (int i = 0; i < 2; ++i) {
                    rt.window().push(MouseMove{c, {}, {}});
                    rt.window().push(MouseDown{c, MouseButton::left, {}, 0});
                    rt.window().push(MouseUp{c, MouseButton::left, {}});
                    rt.tick();
                }
                rt.window().set_tick_seconds(0.5);
                ok(rt.window().fullscreen_calls() == synth_before + 1,
                   "two quick clicks with an UNREPORTED count synthesise a "
                   "double-click");

                // Going fullscreen while PLAYING changes the decode size, so
                // every uploaded frame takes the texture destroy/recreate
                // path in the backend while a frame may still be in flight.
                // On Vulkan that freed an image the GPU was sampling, which
                // is undefined behaviour and hung the app -- reported as "it
                // hung in fullscreen on pause".
                //
                // Headless renders in software so it cannot catch the Vulkan
                // fault, but it does catch the app-level half: the decode
                // size must follow the window, must never exceed the source,
                // and pausing at the new size must still settle.
                rt.send(Video::Msg{Video::Play{}});
                pump([&] { return rt.model().time > 0.1; }, 15000);
                rt.send(Video::Msg{Video::Resized{Vec2{2560.0f, 1440.0f}}});
                rt.tick();
                ok(rt.model().view_w <= g_info.width,
                   "a fullscreen window never asks the decoder to upscale (" +
                       std::to_string(rt.model().view_w) + " <= " +
                       std::to_string(g_info.width) + ")");

                const int fs_frames = rt.model().frames;
                pump([&] { return rt.model().frames > fs_frames + 2; }, 15000);
                ok(rt.model().frames > fs_frames,
                   "frames keep arriving after the size change");

                rt.send(Video::Msg{Video::Pause{}});
                pump([&] { return !rt.model().playing; }, 5000);
                const int paused_at = rt.model().frames;
                for (int i = 0; i < 60; ++i) {
                    rt.tick();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                ok(rt.model().frames == paused_at,
                   "and pausing at the new size stops cleanly");
                rt.send(Video::Msg{Video::Resized{Vec2{860.0f, 620.0f}}});
                rt.tick();
            }

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
