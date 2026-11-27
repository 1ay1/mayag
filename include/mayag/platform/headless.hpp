#pragma once
// mayag::platform::Headless — a window with no window
//
// Satisfies the same `Window` concept as Cocoa or Win32, but renders into a
// memory framebuffer and takes its events from a scripted queue.
//
// This is not a stub or a test double — it is a first-class backend, and it
// is what makes several things possible at once:
//
//   * `run<P>()` compiles and RUNS on a build server with no display
//   * an app is testable end to end: feed clicks, assert on pixels
//   * golden-image regression tests need no GPU and no window manager
//   * `mayag_app --headless --script demo.txt --record frames/` produces
//     documentation GIFs deterministically
//
// Because it is the same code path the real backends use, a test that passes
// here exercises the actual runtime loop, not a parallel fake one.

#include "../core/event.hpp"
#include "../backend/software.hpp"
#include "../backend/tiled.hpp"
#include "../image/png.hpp"
#include "../render/draw_list.hpp"
#include "types.hpp"
#include "concepts.hpp"   // check_capabilities

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::platform {

class Headless {
  public:
    /// Driven by a script: world effects run inline here (cap::scripted).
    static constexpr bool scripted = true;

    [[nodiscard]] static std::optional<Headless> open(const WindowConfig& cfg) {
        Headless w;
        w.size_  = cfg.size;
        w.title_ = cfg.title;
        w.fb_    = backend::Framebuffer{static_cast<int>(cfg.size.x),
                                        static_cast<int>(cfg.size.y)};
        return w;
    }

    void close() { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    [[nodiscard]] Vec2  size() const noexcept { return size_; }
    [[nodiscard]] float dpi_scale() const noexcept { return dpi_; }

    /// Drain the scripted queue. Headless never blocks — a build server must
    /// not be able to hang on a window that will never receive input.
    [[nodiscard]] std::vector<Event> dispatch() {
        // Advance the synthetic clock every poll. Without this the clock is
        // frozen at 0, so EVERY click looks simultaneous with the previous
        // one and the interaction layer correctly-but-uselessly reports an
        // endless double-click. A scripted click should behave like a
        // deliberate human click unless a test explicitly asks otherwise.
        time_ += tick_seconds_;

        std::vector<Event> out;
        while (!queue_.empty()) {
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return out;
    }

    /// Is a test stepping the frame clock by hand (1/60 s per tick)?
    [[nodiscard]] bool driving_frames() const noexcept { return driving_frames_; }

    void present(const DrawList& dl, Color<Srgb> clear) {
        ++frames_presented_;
        if (!raster_) return;
        const int w = static_cast<int>(size_.x * dpi_);
        const int h = static_cast<int>(size_.y * dpi_);
        if (fb_.width() != w || fb_.height() != h) {
            fb_ = backend::Framebuffer{w, h};
        }
        const auto st = backend::Tiled::render(dl, fb_, sampler_, &backend::shared_pool(), clear);
        tiles_total_ += static_cast<std::uint64_t>(st.tiles);
        tiles_reused_ += static_cast<std::uint64_t>(st.tiles_reused);
        // MAYAG_VERIFY_DAMAGE=1: every frame drawn into the reused buffer is
        // checked against the same frame drawn from scratch. Any difference
        // is a damage-tracking bug, reported with the frame number.
        static const bool verify = [] { const char* v = std::getenv("MAYAG_VERIFY_DAMAGE"); return v && *v && *v != '0'; }();
        if (verify) {
            backend::Framebuffer fresh{w, h};
            backend::Tiled::render(dl, fresh, sampler_, &backend::shared_pool(), clear);
            if (fresh.to_rgba8() != fb_.to_rgba8()) {
                ++damage_mismatches_;
                ::mayag::diag::say("mayag: DAMAGE MISMATCH on frame %llu\n", static_cast<unsigned long long>(frames_presented_));
            }
        }

        if (!record_dir_.empty()) {
            const auto path = record_dir_ + "/frame_" + pad6(frames_presented_) + ".png";
            const auto px = fb_.to_rgba8();
            (void)imageio::write_png(path, px, w, h);
        }
    }

    [[nodiscard]] double now() const noexcept { return time_; }
    /// Tiles rendered / reused by damage tracking, and frames whose reused
    /// pixels disagreed with a fresh render (MAYAG_VERIFY_DAMAGE).
    [[nodiscard]] std::uint64_t tiles_total() const noexcept { return tiles_total_; }
    [[nodiscard]] std::uint64_t tiles_reused() const noexcept { return tiles_reused_; }
    [[nodiscard]] std::uint64_t damage_mismatches() const noexcept { return damage_mismatches_; }
    /// The framebuffer's size in pixels (what read_pixels() returns).
    [[nodiscard]] Vec2 pixel_size() const noexcept {
        return Vec2{static_cast<float>(fb_.width()), static_cast<float>(fb_.height())};
    }

    /// Headless reports the conventional default; tests that care set it.
    [[nodiscard]] double double_click_interval() const noexcept { return 0.5; }

    void set_title(std::string_view t) { title_ = std::string{t}; }
    void set_cursor(CursorShape c) { cursor_ = static_cast<int>(c); }
    void set_clipboard(std::string_view s) { clipboard_ = std::string{s}; }
    /// Fullscreen is recorded rather than performed, so a headless test can
    /// assert that an app ASKED for it. Without this the Cmd is unobservable
    /// and a test can only check the app's own bool -- which passes even when
    /// the runtime drops the command on the floor, as it did.
    void set_fullscreen(bool on) { fullscreen_ = on; ++fullscreen_calls_; }
    [[nodiscard]] bool fullscreen() const noexcept { return fullscreen_; }
    /// Window-manager requests, recorded so a test can see they were made.
    void begin_move() { ++moves_; }
    void minimize() { ++minimizes_; }
    void toggle_maximize() { maximized_ = !maximized_; }
    [[nodiscard]] int moves() const noexcept { return moves_; }
    [[nodiscard]] int minimizes() const noexcept { return minimizes_; }
    [[nodiscard]] bool maximized() const noexcept { return maximized_; }
    [[nodiscard]] int fullscreen_calls() const noexcept { return fullscreen_calls_; }

    [[nodiscard]] std::string get_clipboard() { return clipboard_; }

    [[nodiscard]] std::vector<std::uint8_t> read_pixels() { return fb_.to_rgba8(); }

    // ── scripting (headless-only, used by tests and demos) ──────────────

    /// Queue an event for the next poll.
    void push(Event e) { queue_.push_back(std::move(e)); }

    /// Queue a full click at `p`: move, down, up. Three events, because the
    /// interaction state machine must see the same sequence a real mouse
    /// produces — otherwise the test proves nothing about real input.
    /// Queue a full click: move, down, up.
    ///
    /// `click_count` is supplied the way a real platform supplies it, so the
    /// interaction layer exercises its PRIMARY path in tests rather than the
    /// synthesis fallback.
    void click(Vec2 p, MouseButton b = MouseButton::left, int click_count = 1) {
        push(MouseMove{p, Vec2{}, Mods{}});
        push(MouseDown{p, b, Mods{}, click_count});
        push(MouseUp{p, b, Mods{}});
    }

    void move_to(Vec2 p) { push(MouseMove{p, p - last_pos_, Mods{}}); last_pos_ = p; }
    /// The halves of a click, for drags: press, move_to..., release.
    void press(Vec2 p, MouseButton b = MouseButton::left) {
        move_to(p);
        push(MouseDown{p, b, Mods{}, 1});
    }
    /// `n` clicks at `p`, `gap` seconds apart, with NO platform click count
    /// (like Wayland): the interaction layer must recognise the sequence
    /// from timing and place, as it does for a real user. `mods` on every
    /// press (alt-click, shift-click).
    void timed_clicks(Vec2 p, int n, double gap = 0.12, Mods mods = {}, MouseButton b = MouseButton::left) {
        move_to(p);
        for (int i = 0; i < n; ++i) {
            push(MouseDown{p, b, mods, 0, time_ + gap * static_cast<double>(i)});
            push(MouseUp{p, b, mods});
        }
    }
    void release(Vec2 p, MouseButton b = MouseButton::left) {
        move_to(p);
        push(MouseUp{p, b, Mods{}});
    }

    void type(std::string_view s) { push(TextEvent{std::string{s}}); }

    /// What the host told the platform about text entry: a text field has
    /// focus (the input method would be enabled), and where its caret is.
    void set_text_input(bool on, Rect caret) { text_input_ = on; if (on) text_caret_ = caret; }
    [[nodiscard]] bool text_input_active() const noexcept { return text_input_; }
    [[nodiscard]] Rect text_input_caret() const noexcept { return text_caret_; }

    /// Script an input-method composition, as a real IME produces it.
    ///
    /// Being able to drive this headlessly is what makes CJK input testable
    /// at all — otherwise it can only be checked by a human with a Japanese
    /// keyboard, which means in practice it is never checked.
    void compose(std::string_view preedit, std::uint32_t caret = 0) {
        push(ComposeEvent{std::string{preedit}, 0, 0,
                          caret == 0 ? static_cast<std::uint32_t>(preedit.size()) : caret});
    }
    void commit(std::string_view final_text) {
        push(ComposeEndEvent{});
        push(TextEvent{std::string{final_text}});
    }
    void cancel_compose() { push(ComposeEndEvent{}); }

    void press_key(Key k, Mods m = {}) { push(KeyEvent{k, m, false}); }

    void scroll(Vec2 at, Vec2 delta) { push(ScrollEvent{delta, at, Mods{}, false}); }

    void resize(Vec2 s, float dpi = 1.0f) {
        size_ = s; dpi_ = dpi;
        push(ResizeEvent{s, dpi});
    }

    /// Advance the synthetic clock without an event.
    void advance(double seconds) { time_ += seconds; }

    /// Seconds the clock advances per poll. Default is deliberately larger
    /// than the double-click threshold so consecutive scripted clicks read as
    /// separate clicks. Lower it to test double-click handling.
    void set_tick_seconds(double s) { tick_seconds_ = s; }

    /// Queue two clicks close enough in time to register as a double-click.
    void double_click(Vec2 p) {
        const double saved = tick_seconds_;
        tick_seconds_ = 0.01;
        click(p, MouseButton::left, 1);
        click(p, MouseButton::left, 2);
        tick_seconds_ = saved;
    }

    /// An n-click sequence, counts supplied as a platform would.
    void multi_click(Vec2 p, int times) {
        const double saved = tick_seconds_;
        tick_seconds_ = 0.01;
        for (int i = 1; i <= times; ++i) click(p, MouseButton::left, i);
        tick_seconds_ = saved;
    }

    /// Emit a FrameEvent on every poll — for driving animations in a test.
    void drive_frames(bool on) { driving_frames_ = on; }

    /// Skip rasterising in present(), to time the CPU pipeline alone.
    void set_raster(bool on) { raster_ = on; }

    /// Write every presented frame as a PNG into `dir`.
    void record_to(std::string dir) { record_dir_ = std::move(dir); }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return frames_presented_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] int cursor() const noexcept { return cursor_; }
    [[nodiscard]] const backend::Framebuffer& framebuffer() const noexcept { return fb_; }

    /// Sample a pixel in LOGICAL coordinates — the coordinate space the test
    /// wrote its layout in, so assertions do not have to multiply by DPI.
    [[nodiscard]] Color<Srgb> pixel(Vec2 logical) const {
        const int x = static_cast<int>(logical.x * dpi_);
        const int y = static_cast<int>(logical.y * dpi_);
        if (x < 0 || y < 0 || x >= fb_.width() || y >= fb_.height()) return {};
        const Vec4& p = fb_.at(x, y);
        const float a = num::saturate(p.w);
        const float inv = a > 0.0f ? 1.0f / a : 0.0f;
        return Color<Linear>{p.x * inv, p.y * inv, p.z * inv, a}.to<Srgb>();
    }

    void set_coverage_sampler(const backend::CoverageSampler* s) {
        // The image store decorates the font sampler, so the rasteriser sees
        // one sampler that can answer both glyph and image queries.
        images_.set_glyphs(s);
        sampler_ = &images_;
    }

    /// Optional window capability: hand pixels to the renderer for image(id).
    void upload_image(std::uint32_t id, std::shared_ptr<const ImageData> img) {
        images_.upload(id, std::move(img));
    }
    void release_image(std::uint32_t id) { images_.release(id); }

    /// The headless window rasterises on the CPU, which reads the atlas
    /// through the coverage sampler — there is no texture to upload. Present
    /// so the runtime can call it unconditionally instead of branching on
    /// which window it was compiled against.
    template <typename AtlasT>
    void set_atlas_source(AtlasT*) noexcept {}

    [[nodiscard]] static constexpr std::string_view renderer_name() noexcept {
        return "software";
    }
    [[nodiscard]] static constexpr bool gpu_active() noexcept { return false; }

  private:
    [[nodiscard]] static std::string pad6(std::uint64_t n) {
        std::string s = std::to_string(n);
        return std::string(s.size() < 6 ? 6 - s.size() : 0, '0') + s;
    }

    bool  open_ = true;
    Vec2  size_{1024, 640};
    float dpi_ = 1.0f;
    double time_ = 0.0;
    double tick_seconds_ = 0.5;   ///< > the double-click window (0.4 s)
    bool  driving_frames_ = false;
    bool  raster_ = true;

    std::deque<Event>     queue_;
    backend::Framebuffer  fb_{1024, 640};
    const backend::CoverageSampler* sampler_ = nullptr;
    backend::ImageStore images_;

    std::string title_ = "mayag";
    std::string clipboard_;
    bool        fullscreen_ = false;
    int         fullscreen_calls_ = 0;
    std::string record_dir_;
    int         cursor_ = 0;
    int         moves_ = 0, minimizes_ = 0;
    bool        maximized_ = false;
    Vec2        last_pos_{};
    std::uint64_t frames_presented_ = 0;
    std::uint64_t tiles_total_ = 0, tiles_reused_ = 0, damage_mismatches_ = 0;
    bool        text_input_ = false;
    Rect        text_caret_{};
};

// What Headless can do, stated: checked both ways against what its methods
// give it (platform/concepts.hpp) -- losing one or gaining one unstated
// fails the build, naming the capability.
static_assert(check_capabilities<Headless, caps<cap::fullscreens, cap::manages_itself, cap::uploads_images, cap::takes_text_input,
                                                cap::scripted>>());

}  // namespace mayag::platform
