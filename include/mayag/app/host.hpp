#pragma once
// mayag/app/host.hpp — window_host: mayag as a jaal host.
//
// jaal runs the program: model, update, Cmd, Sub, timers, tasks, streams,
// faults, and the waiting. mayag draws it. This is the class where they meet,
// the GUI twin of maya's terminal_host.
//
// The host protocol (jaal/kernel/run.hpp), each step one window call:
//
//   attach(cx)       watch the window's fd (the Wayland display) with jaal's
//                    reactor. A window with no fd (headless) skips this.
//   on_ready(cx, r)  window.dispatch() -> raw events; turn pointer input into
//                    gestures against the laid-out tree; emit each one, and
//                    jaal hands it to the program's on(model, event).
//   present(k)       view(model, ctx) -> layout -> paint -> window.present().
//   owes_frame()     a frame is due: on_frame is subscribed, a spring is
//   wait_hint()      still moving, or hover changed. jaal wakes for those and
//                    sleeps otherwise, so an idle app costs nothing.
//   start/stop_source(on_frame)   the frame clock, while subscribe() asks.
//   handle(effect)   window effects: title, clipboard, images, focus, ...
//
// The host holds presentation state only (the laid-out tree, hover/press/
// focus, overlays, tracked springs). The model lives in the jaal kernel.

#include "../backend/software.hpp"
#include "../core/motion.hpp"
#include "../core/startup.hpp"
#include "../dsl/dsl.hpp"
#include "../font/font.hpp"
#include "../font/system.hpp"
#include "../layout/audit.hpp"
#include "../layout/flex.hpp"
#include "../render/draw_list.hpp"
#include "../render/painter.hpp"
#include "../scene/memo.hpp"
#include "../scene/node.hpp"
#include "../scene/overlay.hpp"
#include "context.hpp"
#include "../scene/cascade.hpp"
#include "../style/theme.hpp"
#include "effects.hpp"
#include "../core/event.hpp"
#include "handlers.hpp"
#include "interaction.hpp"
#include "latency.hpp"
#include "sources.hpp"

#include <jaal/jaal.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mayag {

namespace detail {
/// The three font adapters layout and the painter read through. Held by
/// value so the pointers handed out stay valid for the host's lifetime.
struct StackBindings {
    typo::FontStack::DrawLease lease;   ///< this renderer draws through the stack (one at a time)
    typo::StackMeasurer        measurer;
    typo::StackGlyphRenderer   glyphs;
    typo::StackSampler         sampler;
    explicit StackBindings(typo::FontStack& s) : lease{s}, measurer{s}, glyphs{s}, sampler{s} {}
};
}  // namespace detail

/// A jaal program mayag can draw: `static Node view(const Model&)`.
template <class P>
concept Program = jaal::Program<P> && jaal::Viewable<P, Node>;

/// view() with `c` as the ambient Ctx.
template <Program P>
[[nodiscard]] Node view_of(const typename P::Model& m, const Ctx& c) {
    detail::ctx_scope scope{c};
    return P::view(m);
}

/// The theme a program renders in for `m`: its `static Theme theme(const
/// Model&)` when it has one, else `fallback` (AppConfig::theme). The live
/// host and one-shot renders both use this, so a screenshot is the app.
template <Program P>
[[nodiscard]] Theme theme_of(const typename P::Model& m, const Theme& fallback) {
    if constexpr (requires { { P::theme(m) } -> std::convertible_to<Theme>; }) return P::theme(m);
    else return fallback;
}

/// view() run to a fixed point, for one-shot renders (screenshots, tests).
///
/// A view reads what LAST frame's layout measured (scrollbar extents live in
/// the model's ScrollState). The live host gets there by drawing one more
/// frame; a single offscreen render has no next frame, so it re-runs view
/// after layout until those measurements stop changing (two passes in
/// practice). The returned tree is resolved against `c.theme` (at rest) and
/// laid out against `size`.
template <Program P>
[[nodiscard]] Node settled_view(const typename P::Model& m, const Ctx& c, Vec2 size,
                                const layout::TextMeasurer& measurer) {
    (void)take_scroll_geometry_changed();
    Node root = view_of<P>(m, c);
    for (int pass = 0; pass < 4; ++pass) {
        cascade_static(root, c.theme, size);
        layout::layout_tree(root, size, measurer);
        if (!take_scroll_geometry_changed()) break;
        root = view_of<P>(m, c);
    }
    return root;
}

// ── configuration ───────────────────────────────────────────────────────

struct AppConfig {
    std::string title = "mayag";
    Vec2        size{1024, 640};
    bool        resizable = true;
    bool        vsync     = true;   ///< false: async presents (tearing), input drawn at once: lowest latency
    /// Ask the compositor for server-side decorations. An app that draws its
    /// own title bar (a terminal, an editor) turns this off; a tiling WM
    /// never decorates either way.
    bool        decorated   = true;
    /// An alpha-capable surface, for apps whose background is translucent.
    bool        transparent = false;
    /// macOS: ⌥ is Alt for the app instead of typing characters (see
    /// platform::WindowConfig::option_as_alt).
    bool        option_as_alt = false;
    Theme       theme     = themes::midnight;
    /// Animate theme changes: when `P::theme(model)` changes, the WHOLE UI
    /// morphs from the old theme to the new one -- colours in Oklab, radii,
    /// spacing, type size, depth, springs -- on the theme's own motion
    /// (instant when the new theme's motion_scale is 0: reduced motion).
    bool        theme_morph = true;

    /// The font engine. Null: discovered from the system at startup.
    std::shared_ptr<typo::FontStack> fonts = nullptr;
    /// Or: a function that BUILDS the app's font engine. run() calls it on a
    /// worker while the window opens and the GPU comes up, so an app with
    /// its own font setup (hand: ~100 ms of faces and families) pays for it
    /// in parallel instead of before the window exists. Used when `fonts`
    /// is null. A plain function, so it can cross to the worker.
    std::shared_ptr<typo::FontStack> (*font_loader)() = nullptr;

    bool debug_bounds = false;

    /// Print the active renderer to stderr at startup.
    bool log_renderer = true;

    /// Keep running when update() or view() throws: drop the message (the
    /// model is put back) or keep the last good frame. Off: stop with 70.
    bool catch_exceptions = true;

    /// Told about every fault (where, what). Empty: one line to stderr.
    std::function<void(std::string_view where, std::string_view what)> on_error = {};

    /// The app animates on purpose, every frame, for as long as it runs (a
    /// visualiser, a screensaver, a game). Silences the busy-loop warning,
    /// which otherwise flags a frame subscription that never stops. An
    /// ordinary UI should leave this off and idle at 0% CPU.
    bool continuous_animation = false;
};

// ── the host ────────────────────────────────────────────────────────────

/// One display refresh. The host paces frames itself when nothing better is
/// available (headless, or a compositor that doesn't send frame callbacks).
inline constexpr std::chrono::nanoseconds kFramePeriod{16'666'667};

/// How long before a vblank the late-latch draw starts. Covers view, layout,
/// paint and the GPU submit (~0.5 ms measured) with room to spare.
inline constexpr std::chrono::nanoseconds kLatchMargin{2'000'000};

template <Program P, class W, class Clock = jaal::platform::steady_clock>
class window_host {
  public:
    using event_type = window_events;
    /// The runtime's clock, the same one the kernel steps on: real time in an
    /// app, jaal::platform::sim_clock under simulation (set_clock), so a
    /// simulated run paces frames, springs and morphs in simulated time.
    using clock      = Clock;
    using time_point = typename Clock::time_point;
    using duration_t = typename Clock::duration;

    window_host(W& window, AppConfig cfg) : window_(window), cfg_(std::move(cfg)) {
        if (cfg_.fonts == nullptr) cfg_.fonts = typo::system::default_stack();
        bindings_.emplace(*cfg_.fonts);
        window_.set_atlas_source(&cfg_.fonts->atlas());
        window_.set_coverage_sampler(&bindings_->sampler);
        input_.set_multi_click_interval(window_.double_click_interval());
        size_ = window_.size();
        dpi_  = window_.dpi_scale();
        start_ = now_();
        vblank_ = start_;
    }

    window_host(const window_host&)            = delete;
    window_host& operator=(const window_host&) = delete;

    // ── jaal protocol ───────────────────────────────────────────────────

    template <class Cx>
    void attach(Cx& cx) {
        if constexpr (requires { window_.fd(); }) {
            // `fd()` is the reactor's handle type, not necessarily an int:
            // an fd on Wayland, a HANDLE on Windows. Compare against the
            // platform's "nothing here" value rather than assuming >= 0.
            using H = typename Cx::reactor::handle;
            const H h = external_loop_ ? H{} : static_cast<H>(window_.fd());
            const bool valid = [&] {
                if constexpr (std::is_pointer_v<H>) return h != nullptr;
                else return h >= 0;
            }();
            if (valid) {
                auto reg = cx.watch(h, jaal::interest::read, kWindow);
                if (!reg) { cx.stop(70); return; }
                reg_.emplace(std::move(*reg));
            }
        }
        poll_window_ = !reg_.has_value() && !external_loop_;
        cx_ = [&cx](const window_events& e) { return cx.emit(e); };
        stop_ = [&cx](int code) { cx.stop(code); };
        // A window whose OS can trap the thread in a modal loop (Win32 during
        // a drag-resize or move: DispatchMessage does not return until the
        // mouse is released) gets a way to draw from in there. Without it the
        // content freezes, then snaps, for the whole drag. The callback pumps
        // the window's events into the program and draws immediately, outside
        // the frame pacing: the user is watching this frame land.
        if constexpr (requires { window_.set_live_draw(std::function<void()>{}); }) {
            window_.set_live_draw([this, &cx] {
                if (!cx_ || !live_kernel_) return;
                pump(cx);
                live_kernel_();
            });
        }
        // Anything the window queued while opening (a configure, a resize).
        pump(cx);
    }

    template <class Cx>
    void on_ready(Cx& cx, const jaal::readiness& r) {
        if (r.token != kWindow) return;
        if (r.hangup || r.error) { cx.stop(0); return; }
        pump(cx);
    }

    /// Read whatever the window has queued and hand it to the program. Also
    /// what a headless driver calls after scripting input.
    template <class Cx>
    void pump(Cx& cx) {
        std::vector<Event> events = window_.dispatch();
        // Only the newest pointer position can change hover, so earlier moves
        // in one batch are dropped (a trackpad sends dozens per frame).
        std::size_t last_move = events.size();
        for (std::size_t i = events.size(); i-- > 0;)
            if (std::holds_alternative<MouseMove>(events[i])) { last_move = i; break; }
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (std::holds_alternative<MouseMove>(events[i]) && i != last_move) {
                ++coalesced_moves_;
                continue;
            }
            handle_event(cx, events[i]);
        }
        if (!window_.is_open()) cx.stop(0);
        pump_children(cx);
    }

    template <class K>
    void present(K& k) {
        // Remember how to draw this kernel's model, for a window that has to
        // draw from inside an OS modal loop (see attach()). Bound once: the
        // kernel outlives the host's attachment.
        if (!live_kernel_) {
            live_kernel_ = [this, &k] {
                flush_handled(k);
                const auto now = now_();
                last_draw_ = now;
                drawn_slot_ = slot_of(now);
                draw_owed_ = false;
                draw(k.model(), [&](std::exception_ptr e) { k.view_faulted(std::move(e)); });
            };
        }
        const bool poll_only = std::exchange(poll_only_, false);

        // A window with no fd (Cocoa) is read here on a short poll. When the
        // poll was the only reason we were called, draw only if it brought
        // something; the messages it routed are already folded.
        if ((poll_window_ || !children_.empty()) && cx_ && now_() >= next_poll_) {
            next_poll_ = now_() + kPollPeriod;
            const auto before = emitted_;
            pump_erased();
            if (poll_only && !dirty_ && emitted_ == before && handled_.empty()) return;
        } else if (poll_only && cx_) {
            // A key-repeat wake: read the repeats the window generates.
            const auto before = emitted_;
            pump_erased();
            if (!dirty_ && emitted_ == before && handled_.empty()) return;
        }
        // Handler messages from this pump: into the kernel now. jaal folds
        // them on the very next turn (a message is pending), which draws.
        flush_handled(k);

        // The frame tick goes straight into the kernel on this thread (no
        // mailbox, no wake syscall) and is folded on the next turn, which
        // jaal runs at once because a message is pending. So the frame is
        // drawn AFTER its tick, not one tick late.
        const auto now = now_();
        time_ = std::chrono::duration<double>(now - start_).count();
        sync_display();
        if (const auto dt = take_frame()) {
            if (tracked_.animating()) { tracked_.step_all(*dt); dirty_ = true; }
            if (motion_.animating() || morphing_) { motion_dt_ += *dt; morph_dt_ += *dt; dirty_ = true; }
            if (frames_on_ && frame_make_) {
                k.dispatch(frame_make_(*dt));
                tick_pending_ = true;    // the next present draws it
                return;
            }
        } else if (!std::exchange(tick_pending_, false)) {
            // Not a frame tick: input or a message changed something. The
            // first change in a refresh slot draws NOW (the swapchain shows
            // the newest frame at the next vblank, so nothing waits). More
            // changes in the same slot are held and drawn once, just before
            // the vblank, so that frame carries the freshest input. The draw
            // is owed, never dropped.
            if (now < next_draw_at(now)) {
                draw_owed_ = true;
                return;
            }
        }
        const auto slot = slot_of(now);
        latched_ = (slot == drawn_slot_);   // second draw of a slot: the late one
        draw_owed_ = false;
        last_draw_ = now;
        drawn_slot_ = slot;
        draw(k.model(), [&](std::exception_ptr e) { k.view_faulted(std::move(e)); });
    }

    /// The earliest a non-tick draw may happen: now in a fresh slot; the
    /// slot's late-latch point after its first draw; the next slot after its
    /// late one. So at most two frames per refresh, the second carrying the
    /// freshest input just before the vblank.
    [[nodiscard]] time_point next_draw_at() const noexcept { return next_draw_at(now_()); }
    [[nodiscard]] time_point next_draw_at(time_point now) const noexcept {
        // vsync off (async/tearing presents): a frame is shown the moment it
        // is submitted, so holding it for a vblank only adds latency.
        if (!cfg_.vsync) return now;
        if (slot_of(now) != drawn_slot_) return now;
        if (latched_) return slot_start(drawn_slot_ + 1);
        return std::max(now, latch_point(drawn_slot_));
    }

    // ── the display clock ───────────────────────────────────────────────
    // Refresh slots are counted from a vblank the compositor reported; with
    // no report yet, from start-up at 60 Hz. Slot n runs [vblank + n*period,
    // vblank + (n+1)*period).

    [[nodiscard]] std::int64_t slot_of(time_point t) const noexcept {
        const auto d = t - vblank_;
        return d >= duration_t::zero() ? d / period_ : -((-d) / period_) - 1;
    }
    [[nodiscard]] time_point slot_start(std::int64_t n) const noexcept {
        return vblank_ + n * period_;
    }
    /// Late enough to carry the newest input, early enough to make the vblank
    /// that ends slot n.
    [[nodiscard]] time_point latch_point(std::int64_t n) const noexcept {
        return slot_start(n + 1) - latch_margin();
    }
    /// The window's learned margin when it measures one (macOS, capped there
    /// at half a refresh), else the fixed default capped at a quarter.
    [[nodiscard]] duration_t latch_margin() const noexcept {
        if constexpr (requires { window_.latch_margin(); })
            return std::min<duration_t>(
                std::chrono::duration_cast<duration_t>(window_.latch_margin()), period_ / 2);
        return std::min<duration_t>(std::chrono::duration_cast<duration_t>(kLatchMargin),
                                         period_ / 4);
    }
    void sync_display() {
        // A window reports vblanks on the REAL clock; under a simulated
        // clock they are meaningless, and the 60 Hz default grid is used.
        if constexpr (requires(W& w) { { w.display_timing()->vblank } -> std::convertible_to<time_point>; }) {
            const auto t = window_.display_timing();
            if (!t || t->seq == display_seq_) return;   // nothing new
            display_seq_ = t->seq;
            // Re-express the last draw's slot against the new anchor, so a
            // re-anchor can't make an already-drawn slot look fresh.
            const auto drawn_at = drawn_slot_ >= 0 ? slot_start(drawn_slot_) : time_point{};
            vblank_ = t->vblank;
            if (t->period.count() > 0) period_ = std::chrono::duration_cast<duration_t>(t->period);
            if (drawn_slot_ >= 0) drawn_slot_ = slot_of(drawn_at + period_ / 2);
        }
    }

    /// Owed when the frame clock is running and its tick is due, a spring is
    /// moving, or presentation state (hover, focus) changed under the model.
    [[nodiscard]] bool owes_frame() const noexcept {
        const auto now = now_();
        if (window_wants_redraw()) return true;
        if (external_loop_ && tick_pending_) return true;   // a display-link frame draws now
        if ((dirty_ || draw_owed_) && now >= next_draw_at(now)) return true;
        if (!external_loop_ && animating() && now >= next_frame_) return true;
        // fd-less window: time to look. Remembered so present() can skip the
        // draw when the look finds nothing. (jaal skips this call when the
        // model changed, so a stale flag can't suppress a real frame.)
        poll_only_ = (poll_window_ || !children_.empty()) && now >= next_poll_;
        // A held key's repeat is due (Wayland repeats client-side): pump.
        if (window_repeat_in() == 0.0) poll_only_ = true;
        return poll_only_;
    }

    /// Seconds until the window's next key repeat; negative when none.
    [[nodiscard]] double window_repeat_in() const noexcept {
        if constexpr (requires { window_.seconds_until_repeat(); }) return window_.seconds_until_repeat();
        return -1.0;
    }

    [[nodiscard]] std::optional<std::chrono::nanoseconds> wait_hint() const noexcept {
        using ns = std::chrono::nanoseconds;
        const auto hint = wait_hint_inner();
        const double rep = window_repeat_in();
        if (rep < 0.0) return hint;
        const ns r{static_cast<std::int64_t>(rep * 1e9)};
        return hint ? std::min(*hint, r) : r;
    }

    [[nodiscard]] std::optional<std::chrono::nanoseconds> wait_hint_inner() const noexcept {
        using ns = std::chrono::nanoseconds;
        const auto now = now_();
        if (dirty_ || draw_owed_) {
            return std::max(std::chrono::duration_cast<ns>(next_draw_at(now) - now), ns{0});
        }
        // The window is waiting on something (the GPU coming up) and will
        // want a redraw when it lands; look again soon.
        if (window_redraw_pending()) return ns{2'000'000};
        // A foreign loop's display link drives animation frames.
        if (external_loop_) return std::nullopt;
        if (!animating()) {
            // Wayland wakes us through its fd. A window with none (Cocoa)
            // has to be asked: poll it often enough that input feels instant.
            if (!poll_window_ && children_.empty()) return std::nullopt;
            return kPollPeriod;
        }
        // Nanoseconds, so a late-latch wake lands on time, not up to 1 ms late.
        return std::max(std::chrono::duration_cast<ns>(next_frame_ - now), ns{0});
    }

    // Optional window hooks: wants_redraw() (redraw now: its renderer
    // changed) and redraw_pending() (it will want one soon).
    [[nodiscard]] bool window_wants_redraw() const noexcept {
        if constexpr (requires(const W& w) { { w.wants_redraw() } -> std::convertible_to<bool>; })
            return window_.wants_redraw();
        return false;
    }
    [[nodiscard]] bool window_redraw_pending() const noexcept {
        if constexpr (requires(const W& w) { { w.redraw_pending() } -> std::convertible_to<bool>; })
            return window_.redraw_pending();
        return false;
    }

    // The frame clock. One per program; jaal starts it when subscribe()
    // returns on_frame and stops it when it doesn't.
    template <class Msg>
    void start_source(const fx::on_frame::type<Msg>& p, const fx::on_frame::key_type&,
                      jaal::Sink<Msg>) {
        frame_make_ = p.make;
        frames_on_ = true;
        last_tick_.reset();
        sync_display();
        next_frame_ = now_();   // the first tick at once, then on vblank
    }
    void stop_source(std::type_identity<fx::on_frame>, const fx::on_frame::key_type&) {
        frames_on_ = false;
        frame_make_ = nullptr;
    }

    void handle(const SetTitle& e)       { window_.set_title(e.title); }
    void handle(const WriteClipboard& e) { window_.set_clipboard(e.text); }
    template <class Msg>
    Msg handle(const ReadClipboard<Msg>& e) { return e.make(window_.get_clipboard()); }
    void handle(const SetCursor& e)      { window_.set_cursor(e.shape); }
    void handle(const SetFullscreen& e) {
        if constexpr (requires { window_.set_fullscreen(e.on); }) window_.set_fullscreen(e.on);
    }
    void handle(const WindowAction& e) {
        using K = WindowAction::Kind;
        switch (e.kind) {
            case K::move:
                // The press that started it no longer drags inside the app.
                if constexpr (requires { window_.begin_move(); }) { window_.begin_move(); input_.cancel_press(); }
                break;
            case K::minimize:
                if constexpr (requires { window_.minimize(); }) window_.minimize();
                break;
            case K::toggle_maximize:
                if constexpr (requires { window_.toggle_maximize(); }) window_.toggle_maximize();
                break;
        }
    }
    void handle(const FocusNode& e) { input_.set_focus_visible(e.node_id); dirty_ = true; }
    void handle(const PatchImage& e) {
        const auto& p = e.patch.get();
        bool done = false;
        // Nothing to patch in place (a keyframe, a frame that changed most of
        // the image): it is the whole picture. update_image_regions() of no
        // rects "succeeds", which skipped the upload and left every refresh
        // and video frame after the first one off screen.
        const bool partial = !p.rects.empty() || p.move;
        bool moved = !p.move;
        if constexpr (requires { window_.move_image_rows(e.id, 0, 0, 1, 1, 0); })
            if (p.move) moved = window_.move_image_rows(e.id, (*p.move)[0], (*p.move)[1], (*p.move)[2], (*p.move)[3], (*p.move)[4]);
        if constexpr (requires { window_.update_image_regions(e.id, 1, 1, p.rects, p.pixels); })
            if (partial && moved) done = window_.update_image_regions(e.id, p.width, p.height, p.rects, p.pixels);
        if (!done && p.whole) {
            if constexpr (requires { window_.upload_image(e.id, std::shared_ptr<const ImageData>{}); })
                window_.upload_image(e.id, std::shared_ptr<const ImageData>(
                                               std::make_shared<jaal::shared<ImageData>>(*p.whole), &p.whole->get()));
        }
        dirty_ = true;
    }
    void handle(const UploadImage& e) {
        if constexpr (requires { window_.upload_image(e.id, std::shared_ptr<const ImageData>{}); }) {
            // shared<T> holds a shared_ptr<const T>; alias it without a copy.
            window_.upload_image(e.id, std::shared_ptr<const ImageData>(
                std::make_shared<jaal::shared<ImageData>>(e.image), &e.image.get()));
            dirty_ = true;
        }
    }
    void handle(const ReleaseImage& e) {
        if constexpr (requires { window_.release_image(e.id); }) {
            window_.release_image(e.id);
            dirty_ = true;
        }
    }

    void release() { reg_.reset(); cx_ = nullptr; stop_ = nullptr; children_.clear(); }

    // ── child windows ────────────────────────────────────────────────────────────
    //
    // A program that declares `windows(model)` gets one Surface per
    // WindowSpec: a window of the same platform type, with its own tree,
    // overlays, interaction state and transitions. See sources.hpp.

    static constexpr bool has_children = requires(const typename P::Model& m) {
        { P::windows(m) } -> std::convertible_to<std::vector<WindowSpec>>;
        { P::window_view(m, std::uint64_t{}) } -> std::convertible_to<Node>;
    };

    struct Surface {
        WindowSpec    spec;
        W             win;
        Node          tree{};
        DrawList      draws{};
        Interaction   input{};
        OverlayList   overlays;
        TrackedSet    tracked;
        StyleMotion   motion{};
        std::unordered_set<std::uint64_t> seen{};
        bool          seen_valid = false;
        std::optional<std::uint64_t> press_layer;
        CursorShape   cursor = CursorShape::arrow;
        Vec2          size{};
        float         dpi = 1.0f;
        bool          dirty = true;
        bool          moving = false;   ///< transitions / tracked springs in flight
        double        motion_dt = 0.0;
        Rect          atlas_owed{}, color_owed{};   ///< glyph uploads this window's GPU still needs
        std::uint32_t atlas_gen = 0;
        explicit Surface(WindowSpec s, W w) : spec(std::move(s)), win(std::move(w)) {}
    };

    [[nodiscard]] std::size_t child_count() const noexcept { return children_.size(); }
    /// A child window by its WindowSpec id (tests, screenshots).
    [[nodiscard]] W* child_window(std::uint64_t id) noexcept {
        for (auto& s : children_) if (s->spec.id == id) return &s->win;
        return nullptr;
    }
    [[nodiscard]] std::optional<Rect> child_node_rect(std::uint64_t id, std::uint64_t node) const {
        for (const auto& s : children_) {
            if (s->spec.id != id) continue;
            for (const auto& o : s->overlays.items()) if (const Node* n = o.content.find(node)) return n->frame();
            if (const Node* n = s->tree.find(node)) return n->frame();
        }
        return std::nullopt;
    }
    [[nodiscard]] bool children_animating() const noexcept {
        for (const auto& s : children_) if (s->moving) return true;
        return false;
    }

  private:
    /// Open / retitle / close child windows to match the model.
    void reconcile_children(const typename P::Model& model) {
        if constexpr (has_children) {
            const std::vector<WindowSpec> want = P::windows(model);
            std::erase_if(children_, [&](const std::unique_ptr<Surface>& s) {
                const bool keep = std::ranges::any_of(want, [&](const WindowSpec& w) { return w.id == s->spec.id; });
                if (!keep) s->win.close();
                return !keep;
            });
            for (const auto& w : want) {
                if (w.id == 0) continue;
                auto it = std::ranges::find_if(children_, [&](const auto& s) { return s->spec.id == w.id; });
                if (it != children_.end()) {
                    if ((*it)->spec.title != w.title) { (*it)->win.set_title(w.title); (*it)->spec.title = w.title; }
                    continue;
                }
                auto opened = W::open(platform::WindowConfig{.title = w.title, .size = w.size, .resizable = w.resizable,
                                                             .decorated = true, .transparent = false, .vsync = cfg_.vsync,
                                                             .background = theme_.background, .child = true});
                if (!opened) continue;
                auto s = std::make_unique<Surface>(w, std::move(*opened));
                // the main window's loop reads the app's event queue and hands
                // each event to the window it belongs to
                if constexpr (requires { window_.enroll(); s->win.set_pull(false); }) {
                    window_.enroll();
                    s->win.set_pull(false);
                    s->win.enroll();
                }
                s->win.set_atlas_source(&cfg_.fonts->atlas());
                s->win.set_coverage_sampler(&bindings_->sampler);
                s->input.set_multi_click_interval(s->win.double_click_interval());
                s->size = s->win.size();
                s->dpi = s->win.dpi_scale();
                // a new GPU texture: the whole atlas is owed
                const auto& a = cfg_.fonts->atlas();
                s->atlas_owed = Rect{0, 0, static_cast<float>(a.width()), static_cast<float>(a.height())};
                s->color_owed = s->atlas_owed;
                s->atlas_gen = a.generation();
                children_.push_back(std::move(s));
                dirty_ = true;
            }
        }
    }

    /// Child-window input: gestures against its tree, raw events wrapped.
    template <class Cx>
    void pump_children(Cx& cx) {
        if constexpr (has_children) {
            for (std::size_t k = 0; k < children_.size(); ++k) {
                Surface* s = children_[k].get();
                std::vector<Event> evs = s->win.dispatch();
                std::size_t last_move = evs.size();
                for (std::size_t i = evs.size(); i-- > 0;)
                    if (std::holds_alternative<MouseMove>(evs[i])) { last_move = i; break; }
                const std::uint64_t id = s->spec.id;
                for (std::size_t i = 0; i < evs.size(); ++i) {
                    if (std::holds_alternative<MouseMove>(evs[i]) && i != last_move) continue;
                    // the program may close it from inside a handler: look it up again
                    auto it = std::ranges::find_if(children_, [&](const auto& c) { return c->spec.id == id; });
                    if (it == children_.end()) break;
                    child_event(cx, **it, evs[i]);
                }
            }
        }
    }

    template <class Cx>
    void child_event(Cx& cx, Surface& s, const Event& ev) {
        if (const auto* r = std::get_if<ResizeEvent>(&ev)) { s.size = r->size; s.dpi = r->dpi_scale; s.dirty = true; }
        if (std::holds_alternative<FrameEvent>(ev)) return;
        const std::uint64_t id = s.spec.id;
        const auto wrap = [&](const Event& e) { (void)emit(cx, WindowEvent{id, e}); };
        if (!event_position(ev)) { wrap(ev); dirty_ = s.dirty = true; return; }
        // pointer: the layer that took the press keeps it; overlays first
        const Node* layer = &s.tree;
        const bool held = s.input.pressed() != 0 && s.press_layer.has_value();
        if (held && !std::holds_alternative<MouseDown>(ev)) {
            if (*s.press_layer != 0)
                for (const auto& o : s.overlays.items()) if (o.id == *s.press_layer) { layer = &o.content; break; }
        } else {
            if (std::holds_alternative<MouseDown>(ev)) s.press_layer = 0;
            const auto pos = *event_position(ev);
            if (const Overlay* over = s.overlays.empty() ? nullptr : s.overlays.hit(pos)) {
                layer = &over->content;
                if (std::holds_alternative<MouseDown>(ev)) s.press_layer = over->id;
            } else if (std::holds_alternative<MouseDown>(ev) && !s.overlays.empty()) {
                auto gone = s.overlays.dismissed_by(pos);
                if (!gone.empty()) {
                    for (std::uint64_t oid : gone) (void)emit(cx, Left{oid});
                    s.dirty = dirty_ = true;
                    return;
                }
                if (s.overlays.captures_input()) return;
            }
        }
        const auto h0 = s.input.hovered(), p0 = s.input.pressed(), f0 = s.input.focused();
        const auto gestures = s.input.handle(ev, *layer, s.win.now());
        if (!gestures.empty() || s.input.hovered() != h0 || s.input.pressed() != p0 || s.input.focused() != f0)
            s.dirty = dirty_ = true;
        wrap(ev);
        emit_gestures(cx, gestures, &s.tree, &s.overlays);
        if (std::holds_alternative<MouseUp>(ev)) s.press_layer.reset();
        // the pointer's shape over this window
        const CursorShape want = [&] {
            if (const Overlay* o = s.overlays.empty() ? nullptr : s.overlays.hit(s.input.cursor())) return cursor_at(o->content, s.input.cursor());
            return cursor_at(s.tree, s.input.cursor());
        }();
        if (want != s.cursor) { s.cursor = want; s.win.set_cursor(want); }
    }

    /// Draw every child window that changed (or animates).
    void draw_children(const typename P::Model& model, double dt) {
        if constexpr (has_children) {
            for (auto& sp : children_) {
                Surface& s = *sp;
                if (!s.dirty && !s.moving && !dirty_children_) continue;
                s.dirty = false;
                s.size = s.win.size();
                s.dpi = s.win.dpi_scale();
                s.overlays.clear();
                s.tracked.clear();
                Ctx ctx{s.size, s.dpi, time_, theme_, &latency_, {}, &bindings_->measurer, &s.tracked, &s.overlays, &s.input};
                try {
                    detail::ctx_scope scope{ctx};
                    s.tree = P::window_view(model, s.spec.id);
                } catch (...) {
                    continue;
                }
                const auto& measurer = bindings_->measurer;
                CascadeInput ci{};
                ci.theme = &theme_;
                ci.viewport = s.size;
                ci.hovered = s.input.hovered();
                ci.pressed = s.input.pressed();
                ci.press_inside = s.input.press_inside();
                ci.focused = s.input.focused();
                ci.focus_visible = s.input.focus_visible();
                ci.previous_ids = s.seen_valid ? &s.seen : nullptr;
                CascadeResult cr = cascade(s.tree, ci);
                for (auto& o : s.overlays.items()) {
                    const CascadeResult r = cascade(o.content, ci);
                    cr.transitions = cr.transitions || r.transitions;
                }
                s.seen.clear();
                collect_starting_ids(s.tree, s.seen);
                for (const auto& o : s.overlays.items()) collect_starting_ids(o.content, s.seen);
                s.seen_valid = true;
                layout::layout_tree(s.tree, s.size, measurer);
                s.tracked.observe_all(s.tree);
                std::vector<const Node*> placed;
                auto& items = s.overlays.items();
                std::stable_sort(items.begin(), items.end(), [](const Overlay& a, const Overlay& b) { return a.layer < b.layer; });
                for (auto& o : items) {
                    const Node* a = o.anchor_id != 0 ? s.tree.find(o.anchor_id) : nullptr;
                    for (std::size_t k = 0; a == nullptr && o.anchor_id != 0 && k < placed.size(); ++k) a = placed[k]->find(o.anchor_id);
                    const Rect anchor = o.anchor_id != 0 ? (a ? a->frame() : Rect{}) : Rect{Vec2{}, s.size};
                    const Vec2 natural = layout::measure(o.content, layout::Constraints{s.size.x, s.size.y}, measurer);
                    Rect placed_r = overlay::resolve(o.placement, anchor, natural, s.size, o.offset, o.at.value_or(s.input.cursor()));
                    placed_r.origin = placed_r.origin + o.shift;
                    layout::arrange(o.content, placed_r, measurer);
                    placed.push_back(&o.content);
                }
                s.motion.begin_frame();
                if (cr.transitions || s.motion.size() > 0) {
                    const double mdt = std::exchange(s.motion_dt, 0.0) + dt;
                    s.motion.visit_root(s.tree, mdt);
                    std::uint64_t salt = 0;
                    for (auto& o : items) s.motion.visit_root(o.content, mdt, o.id != 0 ? o.id : ++salt);
                }
                s.motion.end_frame();
                if (s.tracked.animating()) s.tracked.step_all(dt);
                s.moving = s.motion.animating() || s.tracked.animating();
                // re-resolve hover against the new layout (a menu closed, a list scrolled)
                {
                    const Overlay* over = s.overlays.empty() ? nullptr : s.overlays.hit(s.input.cursor());
                    (void)s.input.rehover(over ? over->content : s.tree);
                }
                const auto paint_all = [&] {
                    s.draws.clear();
                    render::PaintOptions po{};
                    po.dpi_scale = s.dpi;
                    po.measurer = &measurer;
                    po.glyphs = &bindings_->glyphs;
                    po.clear_color = theme_.background;
                    render::paint(s.tree, s.draws, po);
                    for (const auto& o : items) {
                        if (o.scrim > 0.0f) s.draws.fill_rect(Rect{Vec2{}, s.size * s.dpi}, colors::black.fade(o.scrim));
                        render::paint(o.content, s.draws, po);
                    }
                };
                paint_all();
                if (cfg_.fonts->atlas_moved()) paint_all();
                present_child(s);
            }
            dirty_children_ = false;
        }
    }

    /// One shared glyph atlas, one texture per GPU window: each window
    /// presents with exactly the regions it has not uploaded yet.
    void present_child(Surface& s) {
        auto& a = cfg_.fonts->atlas();
        const Rect own = a.dirty_region(), own_c = a.color_dirty_region();
        if (a.generation() != s.atlas_gen) {
            s.atlas_gen = a.generation();
            s.atlas_owed = s.color_owed = Rect{0, 0, static_cast<float>(a.width()), static_cast<float>(a.height())};
        }
        a.mark_dirty(s.atlas_owed);
        a.mark_color_dirty(s.color_owed);
        s.win.present(s.draws, theme_.background);
        s.atlas_owed = s.color_owed = Rect{};
        // what the main window has not uploaded yet stays owed to it
        a.clear_dirty();
        a.clear_color_dirty();
        a.mark_dirty(own);
        a.mark_color_dirty(own_c);
    }

    /// After the main window presented: whatever it uploaded is owed to every child.
    void note_main_uploads(const Rect& cov, const Rect& col) {
        for (auto& s : children_) {
            if (!cov.empty()) s->atlas_owed = s->atlas_owed.empty() ? cov : s->atlas_owed.unite(cov);
            if (!col.empty()) s->color_owed = s->color_owed.empty() ? col : s->color_owed.unite(col);
        }
    }

    std::vector<std::unique_ptr<Surface>> children_;
    bool dirty_children_ = false;   ///< redraw every child (the model changed)
    double child_time_ = 0.0;

  public:

    // ── introspection (tests, examples) ─────────────────────────────────

    [[nodiscard]] W& window() noexcept { return window_; }
    [[nodiscard]] const Interaction& input() const noexcept { return input_; }
    [[nodiscard]] const Node& tree() const noexcept { return tree_; }
    /// This frame's overlays (menus, modals, toasts), as posted.
    [[nodiscard]] const OverlayList& overlay_list() const noexcept { return overlays_; }
    /// Read time from `c` (the kernel's clock) instead of a default one.
    void set_clock(const Clock& c) noexcept { clock_ = &c; }
    [[nodiscard]] typename Clock::time_point now_() const noexcept {
        return clock_ != nullptr ? clock_->now() : own_clock_.now();
    }
    /// A `transition(...)` spring is still moving (the host is drawing for it).
    [[nodiscard]] bool transitioning() const noexcept { return motion_.animating() || morphing_; }
    /// A theme morph is running (see AppConfig::theme_morph).
    [[nodiscard]] bool morphing() const noexcept { return morphing_; }
    /// The theme the last frame was drawn in (mid-morph: the blend).
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] const LatencyStats& latency() const noexcept { return latency_; }
    [[nodiscard]] bool busy_loop_detected() const noexcept { return busy_warned_; }
    [[nodiscard]] std::string_view renderer_name() const noexcept { return window_.renderer_name(); }
    [[nodiscard]] bool frames_on() const noexcept { return frames_on_; }

    /// Is `p` on the node an overlay is anchored to (its trigger)?
    [[nodiscard]] bool on_anchor(const Overlay& o, Vec2 p) const {
        if (o.anchor_id == 0) return false;
        const Node* a = tree_.find(o.anchor_id);
        for (std::size_t k = 0; a == nullptr && k < overlays_.items().size(); ++k)
            if (&overlays_.items()[k] != &o) a = overlays_.items()[k].content.find(o.anchor_id);
        return a != nullptr && a->frame().contains(p);
    }

    [[nodiscard]] std::optional<Rect> node_rect(std::uint64_t id) const {
        // Overlays first, topmost layer first: a node in an open menu is what
        // a click at its centre reaches, so that is the rect a test wants.
        const Overlay* best = nullptr;
        const Node* hit = nullptr;
        for (const auto& o : overlays_.items()) {
            if (const Node* n = o.content.find(id); n && (best == nullptr || o.layer >= best->layer)) {
                best = &o;
                hit = n;
            }
        }
        if (hit != nullptr) return hit->frame();
        if (const Node* n = tree_.find(id)) return n->frame();
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Rect> node_rect(std::string_view name) const {
        return node_rect(dsl::node_id(name));
    }
    [[nodiscard]] Vec2 center_of(std::string_view name) const {
        if (auto r = node_rect(name)) return r->center();
        return Vec2{-1.0f, -1.0f};
    }
    [[nodiscard]] Vec2 center_of(std::uint64_t id) const {
        if (auto r = node_rect(id)) return r->center();
        return Vec2{-1.0f, -1.0f};
    }

    /// Fire the frame clock now with this dt (headless driver: 1/60 per tick).
    template <class K>
    void tick_frame(K& k, double dt) {
        flush_handled(k);
        if (frames_on_ && frame_make_) k.dispatch(frame_make_(dt));
        time_ += dt;
        if (tracked_.animating()) { tracked_.step_all(dt); dirty_ = true; }
        if (motion_.animating()) dirty_ = true;
        motion_dt_ += dt;
        morph_dt_ += dt;
        if (morphing_) dirty_ = true;
    }

    // ── driven from a foreign loop (Cocoa) ───────────────────────────────

    /// The loop pushes window input and owns frame timing (a display link),
    /// so the host neither polls the window nor paces frames itself.
    void set_external_loop(bool on) noexcept { external_loop_ = on; }

    /// Does anything want display refreshes right now? The loop runs its
    /// display link exactly while this is true.
    [[nodiscard]] bool wants_frames() const noexcept { return animating(); }

    /// One display refresh. `dt` is the time since the previous one.
    template <class K>
    void vsync(K& k, double dt) {
        if (!animating()) return;
        dt = std::min(std::max(dt, 0.0), 0.1);
        time_ = std::chrono::duration<double>(now_() - start_).count();
        if (tracked_.animating()) tracked_.step_all(dt);
        if (frames_on_ && frame_make_) k.dispatch(frame_make_(dt));
        motion_dt_ += dt;
        morph_dt_ += dt;
        if (morphing_) dirty_ = true;
        dirty_ = true;          // springs and widget motion redraw even without a tick
        tick_pending_ = true;   // a display-link frame draws at once, unpaced
    }

  private:
    /// The dt of a frame that is due now, advancing the frame clock; nullopt
    /// when nothing is animating or the next frame isn't due yet.
    std::optional<double> take_frame() {
        if (external_loop_) return std::nullopt;   // vsync() does this
        const auto now = now_();
        if (!animating() || now < next_frame_) return std::nullopt;
        const double dt = last_tick_
            ? std::min(std::chrono::duration<double>(now - *last_tick_).count(), 0.1)
            : std::chrono::duration<double>(period_).count();
        last_tick_ = now;
        // The next tick at the next late-latch point (just before a vblank):
        // locked to the display, at its real rate (60, 120, 144 Hz ...).
        // drawn_slot_ is only set by an actual draw.
        const auto slot = slot_of(now);
        next_frame_ = now < latch_point(slot) ? latch_point(slot) : latch_point(slot + 1);
        return dt;
    }

  public:

    /// Draw the model. `fault` reports a throwing view to the kernel.
    template <class OnFault>
    void draw(const typename P::Model& model, OnFault&& fault) {
        draw_impl(model, std::forward<OnFault>(fault));
    }

  private:
    static constexpr std::uint64_t kWindow = 1;
    static constexpr std::chrono::nanoseconds kPollPeriod{8'000'000};

    // For fd-less windows: pump through the context attach() captured.
    struct erased_cx {
        window_host& h;
        std::size_t emit(const window_events& e) { return h.cx_(e); }
        void stop(int code) { h.stop_(code); }
    };
    void pump_erased() {
        erased_cx cx{*this};
        pump(cx);
    }

    /// Tell the platform whether a text field has keyboard focus, and where
    /// its caret is: that enables the input method (fcitx5/ibus on Wayland)
    /// and puts its candidate window at the caret. A node counts as text
    /// entry by role (`text_field`, set by text_field / code_view /
    /// terminal_view, or by `| role(...)` on a custom one).
    void sync_text_input() {
        if constexpr (requires { window_.set_text_input(true, Rect{}); }) {
            const std::uint64_t f = input_.focused();
            const Node* n = f != 0 ? tree_.find(f) : nullptr;
            if (n == nullptr && f != 0) {
                for (const auto& o : overlays_.items())
                    if (const Node* m = o.content.find(f)) { n = m; break; }
            }
            const bool entry = n != nullptr && n->style().a11y_role == a11y::Role::text_field;
            Rect caret{};
            if (entry) {
                const Rect fr = n->frame();   // logical px, like the protocol
                const Rect& c = n->style().caret_rect;
                caret = c.size.y > 0.0f
                    ? Rect{Vec2{fr.origin.x + c.origin.x, fr.origin.y + c.origin.y}, c.size}
                    : Rect{fr.origin, Vec2{1.0f, fr.size.y}};
            }
            window_.set_text_input(entry, caret);
        }
    }

    [[nodiscard]] bool animating() const noexcept {
        return frames_on_ || motion_pending_ || tracked_.animating() || motion_.animating() || morphing_ || children_animating();
    }

    // ── input → gestures ────────────────────────────────────────────────

    template <class Cx>
    void handle_event(Cx& cx, const Event& ev) {
        const bool from_user = !std::holds_alternative<ResizeEvent>(ev) &&
                               !std::holds_alternative<FocusEvent>(ev) &&
                               !std::holds_alternative<CloseRequest>(ev) &&
                               !std::holds_alternative<FrameEvent>(ev);
        if (from_user) input_since_render_ = true;

        if (const auto* r = std::get_if<ResizeEvent>(&ev)) {
            size_ = r->size;
            dpi_  = r->dpi_scale;
            dirty_ = true;
        }
        if (std::holds_alternative<FrameEvent>(ev)) return;   // the host owns the clock now
        if (std::holds_alternative<MouseDown>(ev) || std::holds_alternative<KeyEvent>(ev) ||
            std::holds_alternative<ScrollEvent>(ev) || std::holds_alternative<TextEvent>(ev))
            block_tooltip();

        // The close button. A program that doesn't handle it still closes.
        if (std::holds_alternative<CloseRequest>(ev)) {
            if (emit(cx, CloseRequest{}) == 0) cx.stop(0);
            return;
        }

        // Tab moves focus; Enter/Space on the focused node is a click --
        // EXCEPT in a text-entry node (a terminal, a code editor, a field:
        // role text_field), which owns those keys the way a browser input
        // owns Tab-in-a-textarea. A terminal that loses Tab to focus
        // traversal cannot complete a path; one that turns Enter into a
        // click cannot run a command.
        if (const auto* k = std::get_if<KeyEvent>(&ev)) {
            const Node* fnode = node_by_id(input_.focused());
            const bool text_entry = fnode != nullptr && fnode->style().a11y_role == a11y::Role::text_field;
            if (k->key == Key::tab && !k->mods.ctrl && !k->mods.super && !text_entry) {
                input_.focus_next(tree_, k->mods.shift);
                dirty_ = true;
            }
            if ((k->key == Key::enter || k->key == Key::space) &&
                input_.focused() != 0 && k->mods.none() && !text_entry) {
                // An inline on_click on the focused node answers first: an
                // element that says what a click does needs no on(Activated).
                const auto before = handled_.size();
                collect_handlers(Gesture{Gesture::Kind::activate, input_.focused()});
                if (handled_.size() > before) {
                    (void)emit(cx, Activated{input_.focused()});
                    dirty_ = true;
                    return;
                }
                // Consumed only when the program handled it; otherwise the
                // key falls through to its own Space/Enter binding.
                if (emit(cx, Activated{input_.focused()}) > 0) {
                    dirty_ = true;
                    return;
                }
            }
        }

        // Overlays see pointer input first: a modal swallows what's behind
        // it, and a click outside a menu dismisses the menu.
        //
        // POINTER CAPTURE spans layers: while a button is held, every move
        // and the release go to the layer that took the press -- a scrollbar
        // dragged across a toast, a slider dragged off a dialog's edge, must
        // keep dragging.
        const bool held = input_.pressed() != 0 && press_layer_.has_value();
        if (held && event_position(ev) && !std::holds_alternative<MouseDown>(ev)) {
            const Node* layer = &tree_;
            if (*press_layer_ != 0)
                for (const auto& o : overlays_.items()) if (o.id == *press_layer_) { layer = &o.content; break; }
            const auto gestures = input_.handle(ev, *layer, window_.now());
            if (!gestures.empty()) dirty_ = true;
            (void)emit(cx, ev);
            emit_gestures(cx, gestures);
            update_cursor();
            if (std::holds_alternative<MouseUp>(ev)) press_layer_.reset();
            return;
        }
        if (std::holds_alternative<MouseDown>(ev)) press_layer_ = 0;   // the main tree, unless an overlay takes it
        if (!overlays_.empty()) {
            if (const auto pos = event_position(ev)) {
                const bool is_press = std::holds_alternative<MouseDown>(ev);
                if (const Overlay* over = overlays_.hit(*pos)) {
                    if (is_press) {
                        // A press inside a lower overlay (a modal panel) still closes
                        // the menus stacked above it -- then the press goes through,
                        // so the click lands on what it was aimed at (one click, not two).
                        bool closed = false;
                        for (const auto& o : overlays_.items())
                            if (o.layer > over->layer && o.dismiss == Dismiss::on_click_outside &&
                                !o.content.frame().contains(*pos) && !on_anchor(o, *pos)) {
                                (void)emit(cx, Left{o.id});
                                closed = true;
                            }
                        if (closed) dirty_ = true;
                        press_layer_ = over->id;
                    }
                    const auto gestures = input_.handle(ev, over->content, window_.now());
                    if (!gestures.empty()) dirty_ = true;
                    (void)emit(cx, ev);
                    emit_gestures(cx, gestures);
                    update_cursor();
                    return;
                }
                if (is_press) {
                    auto gone = overlays_.dismissed_by(*pos);
                    // a press on a menu's own trigger is the trigger's (it toggles), not "outside"
                    std::erase_if(gone, [&](std::uint64_t oid) {
                        for (const auto& o : overlays_.items()) if (o.id == oid) return on_anchor(o, *pos);
                        return false;
                    });
                    if (!gone.empty()) {
                        for (std::uint64_t id : gone) (void)emit(cx, Left{id});
                        dirty_ = true;
                        return;
                    }
                    if (overlays_.captures_input()) return;
                }
                if (std::holds_alternative<MouseMove>(ev)) update_cursor();
            }
        }


        // Repaint only when hover/press/focus changed: that is all a view can
        // see of the pointer through Ctx.
        const auto before_hover   = input_.hovered();
        const auto before_pressed = input_.pressed();
        const auto before_focus   = input_.focused();
        const bool before_vis     = input_.focus_visible();
        const bool before_inside  = input_.press_inside();
        const auto gestures = input_.handle(ev, tree_, window_.now());
        if (input_.hovered() != before_hover || input_.pressed() != before_pressed ||
            input_.focused() != before_focus || input_.focus_visible() != before_vis ||
            input_.press_inside() != before_inside) {
            dirty_ = true;
        }

        (void)emit(cx, ev);
        emit_gestures(cx, gestures);
        if (event_position(ev)) update_cursor();
    }

    // A raw window event, unpacked so the program's on(const KeyEvent&) etc.
    // sees the concrete type. FrameEvent isn't a window_event and is dropped.
    template <class Cx>
    std::size_t emit(Cx& cx, const Event& ev) {
        return std::visit([&](const auto& e) { return emit(cx, e); }, ev);
    }

    template <class Cx, class E>
    std::size_t emit(Cx& cx, const E& e) {
        if constexpr (std::is_constructible_v<window_events, E>) {
            const std::size_t n = cx.emit(window_events{e});
            emitted_ += n;
            return n;
        } else {
            return 0;
        }
    }

    template <class Cx>
    void emit_gestures(Cx& cx, const std::vector<Gesture>& gestures, const Node* root = nullptr,
                       const OverlayList* ovs = nullptr) {
        using K = Gesture::Kind;
        for (const auto& g : gestures) {
            collect_handlers(g, root, ovs);
            switch (g.kind) {
                case K::click:    (void)emit(cx, Clicked{g.node_id, g.local, g.click_count, g.button, g.mods}); break;
                case K::activate: (void)emit(cx, Activated{g.node_id}); break;
                case K::press:    (void)emit(cx, Pressed{g.node_id, g.local, g.button, g.click_count, g.mods}); break;
                case K::release:  (void)emit(cx, Released{g.node_id, g.local, g.button}); break;
                case K::enter:    (void)emit(cx, Entered{g.node_id}); break;
                case K::leave:    (void)emit(cx, Left{g.node_id}); break;
                case K::drag:     (void)emit(cx, Dragged{g.node_id, g.local, g.delta}); break;
                case K::scroll:   (void)emit(cx, Scrolled{g.node_id, g.local, g.delta}); break;
            }
        }
    }

    // ── inline handlers (`| on_click(Save{})`) ─────────────────────────

    /// The node a gesture landed on, in the main tree or an overlay.
    [[nodiscard]] const Node* node_by_id(std::uint64_t id) const {
        if (id == 0) return nullptr;
        if (const Node* n = tree_.find(id)) return n;
        for (const auto& o : overlays_.items())
            if (const Node* n = o.content.find(id)) return n;
        return nullptr;
    }

    /// Run the handlers on the gesture's node; their messages wait in
    /// handled_ until a kernel is at hand (flush_handled).
    void collect_handlers(const Gesture& g, const Node* root = nullptr, const OverlayList* ovs = nullptr) {
        using K = Gesture::Kind;
        const Node& in_tree = root != nullptr ? *root : tree_;
        const OverlayList& in_ovs = ovs != nullptr ? *ovs : overlays_;
        // BUBBLING, as in the DOM: a gesture runs the handlers of the node
        // it landed on, else of its nearest ancestor that handles that kind
        // of gesture. A widget can wrap a control and own its interaction
        // (an editor wrapping code_view, a panel wrapping a list) without
        // reaching into the control's children; the nearest handler wins,
        // so an inner control's own handler is never doubled by an outer one.
        // Enter/leave are about one node and do not bubble.
        std::vector<const Node*> path;
        const auto find_path = [&](const auto& self, const Node& n) -> bool {
            path.push_back(&n);
            if (n.style().id == g.node_id) return true;
            for (const auto& k : n.children()) if (self(self, k)) return true;
            path.pop_back();
            return false;
        };
        if (!find_path(find_path, in_tree)) {
            for (const auto& o : in_ovs.items()) {
                path.clear();
                if (find_path(find_path, o.content)) break;
            }
            if (path.empty() || path.back()->style().id != g.node_id) return;
        }
        const bool bubbles = g.kind != K::enter && g.kind != K::leave;
        const On on = [&] {
            switch (g.kind) {
                case K::click: case K::activate: return On::click;
                case K::press:   return On::press;
                case K::release: return On::release;
                case K::enter:   return On::enter;
                case K::leave:   return On::leave;
                case K::drag:    return On::drag;
                case K::scroll:  return On::scroll;
            }
            return On::click;
        }();
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            const Node& n = **it;
            const bool has = n.cascade() != nullptr &&
                std::ranges::any_of(n.cascade()->handlers, [&](const auto& h) { return h.on == on; });
            if (!has) { if (!bubbles) return; continue; }
            // `local` stays relative to the node the gesture HIT (the DOM's
            // offsetX): a bubbled scrollbar press still knows where on the
            // bar it landed. `node` names that target too.
            const Vec2 local = g.local;
            const auto run = [&](const auto& event) {
                for (const auto& h : n.cascade()->handlers)
                    if (h.on == on) {
                        HandlerResult r = h.call(h.fn, &event);
                        if (r.msg) handled_.push_back(std::move(r));
                    }
            };
            switch (g.kind) {
                case K::click:    run(Clicked{g.node_id, local, g.click_count, g.button, g.mods}); break;
                case K::activate: run(Clicked{g.node_id, {}, 1, MouseButton::left}); break;
                case K::press:    run(Pressed{g.node_id, local, g.button, g.click_count, g.mods}); break;
                case K::release:  run(Released{g.node_id, local, g.button}); break;
                case K::enter:    run(Entered{g.node_id}); break;
                case K::leave:    run(Left{g.node_id}); break;
                case K::drag:     run(Dragged{g.node_id, local, g.delta}); break;
                case K::scroll:   run(Scrolled{g.node_id, local, g.delta}); break;
            }
            break;
        }
        if (!handled_.empty()) dirty_ = true;
    }

  public:
    /// Hand queued handler messages to the kernel, in gesture order. Called
    /// by every loop before it folds, so a click's message is folded on the
    /// same turn as the click.
    template <class K>
    void flush_handled(K& k) {
        if (handled_.empty()) return;
        auto batch = std::exchange(handled_, {});
        for (const auto& r : batch) {
            if (auto m = to_program_msg<typename P::Msg>(r)) {
                k.dispatch(std::move(*m));
            } else if (!handler_type_warned_) {
                handler_type_warned_ = true;
                std::fputs("mayag: an on_click/on_* handler produced a message that is not one of the "
                           "program's Msg alternatives; it was dropped.\n", stderr);
            }
        }
        dirty_ = true;
    }
    [[nodiscard]] bool has_handled() const noexcept { return !handled_.empty(); }

  private:
    // ── tooltips ─────────────────────────────────────────────────────────
    //
    // Desktop behaviour, owned here so no widget needs a timer:
    //   * shown when the pointer has RESTED on a tooltipped node (or inside
    //     one) for `tip_delay`;
    //   * "warm": after one was shown, moving to the next tooltipped
    //     control shows its tooltip at once (scanning a toolbar), until the
    //     pointer has been on untipped ground for `tip_cooldown`;
    //   * hidden by a press, a key or a scroll, and stays hidden until the
    //     pointer moves to another control (it already did its job);
    //   * placed below the control (above when there is no room), never
    //     under the pointer, and transparent to input.
    static constexpr double tip_delay = 0.55, tip_cooldown = 0.6;
    /// The layer (overlay id, 0 = the main tree) holding the pointer while a
    /// button is down; see handle_event.
    std::optional<std::uint64_t> press_layer_;
    std::uint64_t tip_anchor_ = 0;
    double        tip_since_ = 0.0, tip_last_ = -10.0;
    bool          tip_shown_ = false, tip_blocked_ = false, tip_waiting_ = false;

    /// The nearest node on the path to `id` (inclusive) carrying a tooltip.
    [[nodiscard]] static const Node* tooltip_owner(const Node& root, std::uint64_t id) {
        const Node* best = nullptr;
        const auto walk = [&](const auto& self, const Node& n, const Node* inherited) -> bool {
            const Node* here = n.style().tooltip.empty() ? inherited : &n;
            if (n.style().id == id) { best = here; return true; }
            for (const auto& k : n.children()) if (self(self, k, here)) return true;
            return false;
        };
        (void)walk(walk, root, nullptr);
        return best;
    }

    void block_tooltip() {
        if (tip_shown_) { tip_shown_ = false; tip_last_ = time_; dirty_ = true; }
        tip_blocked_ = true;
    }

    void post_tooltip() {
        using namespace dsl;
        tip_waiting_ = false;
        const std::uint64_t hov = input_.pressed() != 0 ? 0 : input_.hovered();
        const Node* owner = nullptr;
        if (hov != 0) {
            owner = tooltip_owner(tree_, hov);
            if (owner == nullptr)
                for (const auto& o : overlays_.items())
                    if ((owner = tooltip_owner(o.content, hov)) != nullptr) break;
        }
        const std::uint64_t anchor = owner ? (owner->style().id != 0 ? owner->style().id : hov) : 0;
        if (anchor != tip_anchor_) {
            if (tip_shown_) tip_last_ = time_;
            tip_shown_ = false;
            tip_blocked_ = false;
            tip_anchor_ = anchor;
            tip_since_ = time_;
        }
        if (owner == nullptr || tip_blocked_) return;
        const bool warm = time_ - tip_last_ < tip_cooldown;
        if (!tip_shown_ && time_ - tip_since_ < (warm ? 0.0 : tip_delay)) { tip_waiting_ = true; return; }
        tip_shown_ = true;
        tip_last_ = time_;
        const Theme& t = theme_;
        const Style& st = owner->style();
        Node body = (h(text_owned(st.tooltip) | font(t.font_size * 0.86f) | fg(var::text_primary) | wrap_text
                           | line_height(1.3f) | shrink() | min_size(0, 0),
                       when(!st.tooltip_keys.empty(),
                            node((text_owned(st.tooltip_keys) | font(t.font_size * 0.78f) | monospace
                                  | fg(var::text_disabled) | pad(1, 5) | border(t.stroke(1), t.border)
                                  | radius(t.rounded(3))).build())))
                     | gap(t.spaced(8)) | align(Align::center) | pad(t.spaced(5), t.spaced(9))
                     | max_size(380.0f, 200.0f)
                     | bg(var::overlay) | border(t.stroke(1), t.border_strong) | radius(t.radius_small)
                     | shadow(14.0f, colors::black.fade(0.30f), {0.0f, 5.0f}, -3.0f)
                     | starting(opacity(0.0f), translate(0.0f, -3.0f)) | transition()
                     | role(a11y::Role::text)).build();
        overlays_.add(Overlay{.content = std::move(body), .id = 0x7F00'0000'0000'0001ull, .anchor_id = anchor,
                              .placement = Placement::below_start, .dismiss = Dismiss::never, .offset = 6.0f,
                              .layer = 1000, .passthrough = true});
    }

    // ── the pointer's shape ──────────────────────────────────────────────
    //
    // CSS semantics, with the hit rules clicks use:
    //
    //   * the TOPMOST node under the pointer decides (back-to-front, like
    //     Interaction::identify), and its cursor is inherited from the
    //     nearest ancestor that set one (`arrow` means "inherit");
    //   * an unnamed node with no cursor of its own is transparent, exactly
    //     as it is to clicks -- a decorative layer painted over a button
    //     must not turn the button's hand back into an arrow;
    //   * a disabled control shows not_allowed;
    //   * while a press is held, the PRESSED node keeps its cursor (a split
    //     divider dragged past its 5px strip stays a resize arrow; a text
    //     selection dragged over a toolbar stays an I-beam), grab -> grabbing;
    //   * over an overlay, the overlay decides; behind a modal, it's an arrow;
    //   * resolved after every pointer event AND every frame, so a view that
    //     changes under a still pointer is reflected without a mouse move.

    /// Resolve within `n`: true when `n` claims the point; `out` then holds
    /// the cursor, `arrow` meaning "nobody on the path set one yet".
    static bool claim_cursor(const Node& n, Vec2 p, CursorShape& out) {
        if (!n.frame().contains(p)) return false;
        const Style& s = n.style();
        for (auto it = n.children().rbegin(); it != n.children().rend(); ++it) {
            if (claim_cursor(*it, p, out)) {
                if (out == CursorShape::arrow) out = own_cursor(s);
                return true;
            }
        }
        if (s.id == 0 && s.cursor == CursorShape::arrow) return false;   // transparent
        out = own_cursor(s);
        return true;
    }
    [[nodiscard]] static CursorShape own_cursor(const Style& s) noexcept {
        if (s.a11y_state.disabled && s.id != 0) return CursorShape::not_allowed;
        return s.cursor;
    }
    [[nodiscard]] static CursorShape cursor_at(const Node& n, Vec2 p) {
        CursorShape found = CursorShape::arrow;
        (void)claim_cursor(n, p, found);
        return found;
    }

    [[nodiscard]] CursorShape wanted_cursor() const {
        const Vec2 p = input_.cursor();
        if (const std::uint64_t held = input_.pressed(); held != 0) {
            if (const Node* n = node_by_id(held)) {
                const CursorShape c = own_cursor(n->style());
                if (c == CursorShape::grab) return CursorShape::grabbing;
                if (c != CursorShape::arrow && c != CursorShape::pointer) return c;
            }
        }
        if (!overlays_.empty()) {
            if (const Overlay* o = overlays_.hit(p)) return cursor_at(o->content, p);
            for (const auto& o : overlays_.items())
                if (o.dismiss == Dismiss::modal) return CursorShape::arrow;
        }
        return cursor_at(tree_, p);
    }

    void update_cursor() {
        const CursorShape want = wanted_cursor();
        if (want != current_cursor_) {
            current_cursor_ = want;
            window_.set_cursor(want);
        }
    }
  public:
    /// The pointer shape currently shown (for tests).
    [[nodiscard]] CursorShape current_cursor() const noexcept { return current_cursor_; }
  private:

    // ── the frame ───────────────────────────────────────────────────────

    /// Follow the program's theme, morphing between themes on a spring.
    /// Mid-morph the frame's theme is `lerp(from, to, x)`, and since view()
    /// runs every frame with it, EVERYTHING follows: widgets that read
    /// concrete theme values, layout (spacing and type reflow smoothly),
    /// tokens, sheets. At rest the theme is the target exactly (lerp's ends
    /// are exact), so a settled frame is pixel-identical to no morph at all.
    void advance_theme(const Theme& target, double dt) {
        if (!theme_seen_) {
            theme_ = theme_from_ = theme_to_ = target;
            theme_seen_ = true;
            return;
        }
        if (!(target == theme_to_)) {
            theme_from_ = theme_;            // from wherever the eye is now
            theme_to_   = target;
            if (cfg_.theme_morph && target.motion_scale > 0.0f) {
                morphing_ = true;
                morph_x_ = 0.0f;
                morph_v_ = 0.0f;
                dt = 1.0 / 120.0;            // feedback on the frame the change lands
            } else {
                morphing_ = false;
                theme_ = target;
                return;
            }
        }
        if (!morphing_) { theme_ = theme_to_; return; }
        // Critically damped, sped up by the theme's motion_scale.
        const float k = theme_to_.motion_scale;
        const float stiff = 170.0f * k * k, damp = 26.0f * k;
        float rem = num::clamp(static_cast<float>(dt), 0.0f, 0.25f);
        while (rem > 0.0f) {
            const float h = num::min(rem, 1.0f / 240.0f);
            rem -= h;
            morph_v_ += (stiff * (1.0f - morph_x_) - damp * morph_v_) * h;
            morph_x_ += morph_v_ * h;
        }
        if (num::abs(1.0f - morph_x_) < 1e-3f && num::abs(morph_v_) < 1e-2f) {
            morphing_ = false;
            morph_x_ = 1.0f;
            theme_ = theme_to_;
            return;
        }
        theme_ = lerp(theme_from_, theme_to_, morph_x_);
    }

    template <class OnFault>
    void draw_impl(const typename P::Model& model, OnFault&& fault) {
        dirty_ = false;
        size_ = window_.size();
        dpi_  = window_.dpi_scale();

        if (frame_counter_ == 0) startup::mark("first draw begins");
        cfg_.fonts->begin_frame(frame_counter_++);
        FrameTiming frame{};
        frame.coalesced_moves = std::exchange(coalesced_moves_, 0);

        overlays_.clear();
        tracked_.clear();
        clear_animation_request();

        // The theme is application state when the program says so:
        // `static Theme theme(const Model&)`. Then a theme switch is an
        // ordinary update(); every var:: token, the Ctx theme and the window's
        // clear colour follow it on the same frame. Otherwise AppConfig's.
        advance_theme(theme_of<P>(model, cfg_.theme), std::exchange(morph_dt_, 0.0));

        Ctx ctx{size_, dpi_, time_, theme_, &latency_, {}, &bindings_->measurer,
                &tracked_, &overlays_, &input_};
        if constexpr (requires { window_.photon_stats(); }) ctx.photon = window_.photon_stats();
        // Pull last frame's memo() subtrees out of the tree view() replaces.
        memos_.begin_frame(frame_counter_);
        ctx.memos = memo_on_ ? &memos_ : nullptr;
        interaction_reads_ = 0;
        ctx.interaction_reads = &interaction_reads_;
        {
            ScopedTimer t{frame.view_ms};
            try {
                tree_ = view_of<P>(model, ctx);
            } catch (...) {
                fault(std::current_exception());
                if (tree_.children().empty()) return;   // nothing good to show
            }
        }
        post_tooltip();

        const auto& measurer = bindings_->measurer;
        {
            // The cascade: inheritance, state variants, tokens. Before layout,
            // because a variant may change padding or size.
            ScopedTimer t{frame.cascade_ms};
            CascadeInput ci{};
            ci.theme         = &theme_;
            ci.viewport      = size_;
            ci.hovered       = input_.hovered();
            ci.pressed       = input_.pressed();
            ci.press_inside  = input_.press_inside();
            ci.focused       = input_.focused();
            ci.focus_visible = input_.focus_visible();
            ci.previous_ids  = seen_ids_valid_ ? &seen_ids_ : nullptr;
            ci.memos         = memo_on_ ? &memos_ : nullptr;
            CascadeResult cr = cascade(tree_, ci);
            ci.memos = nullptr;   // overlays are rebuilt into a fresh list each frame
            for (auto& o : overlays_.items()) {
                const CascadeResult r = cascade(o.content, ci);
                cr.transitions = cr.transitions || r.transitions;
                cr.starting = cr.starting || r.starting;
            }
            const bool springs = cr.transitions;
            starting_pending_ = cr.starting;

            seen_ids_.clear();
            collect_starting_ids(tree_, seen_ids_, &memos_);
            for (const auto& o : overlays_.items()) collect_starting_ids(o.content, seen_ids_);
            seen_ids_valid_ = true;

            springs_ = springs;
        }
        { ScopedTimer t{frame.layout_ms}; layout::layout_tree(tree_, size_, measurer); }
        tracked_.observe_all(tree_);

        // Lay overlays out lowest layer first, so an overlay anchored INSIDE
        // another (a dropdown in a modal panel) finds its anchor already
        // placed. Anchors are looked up in the main tree, then in every
        // overlay arranged so far.
        std::vector<std::size_t> order(overlays_.items().size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return overlays_.items()[a].layer < overlays_.items()[b].layer;
        });
        std::vector<const Node*> placed_roots;
        for (std::size_t oi : order) {
            auto& o = overlays_.items()[oi];
            const Node* a = o.anchor_id != 0 ? tree_.find(o.anchor_id) : nullptr;
            for (std::size_t k = 0; a == nullptr && o.anchor_id != 0 && k < placed_roots.size(); ++k)
                a = placed_roots[k]->find(o.anchor_id);
            const Rect anchor = o.anchor_id != 0 ? (a ? a->frame() : Rect{}) : Rect{Vec2{}, size_};
            const Vec2 natural = layout::measure(o.content, layout::Constraints{size_.x, size_.y}, measurer);
            Rect placed = overlay::resolve(o.placement, anchor, natural, size_,
                                           o.offset, o.at.value_or(input_.cursor()));
            placed.origin = placed.origin + o.shift;
            layout::arrange(o.content, placed, measurer);
            placed_roots.push_back(&o.content);
        }

        // Transitions run AFTER layout. Everything they animate is paint-only
        // (colour, border, radii, shadows, opacity, transform), and layout has
        // already clamped radii to the real box: a pill that becomes a square
        // springs from its true radius (11 px), not from `radius_full`'s 1e9.
        motion_.begin_frame();
        motion_.set_memos(&memos_);
        if (springs_ || motion_.size() > 0) {
            const double dt = std::exchange(motion_dt_, 0.0);
            motion_.visit_root(tree_, dt);
            std::uint64_t salt = 0;
            for (auto& o : overlays_.items()) motion_.visit_root(o.content, dt, o.id != 0 ? o.id : ++salt);
        } else {
            motion_dt_ = 0.0;
        }
        motion_.end_frame();

        if (cfg_.debug_bounds) {
            const auto issues = layout::audit(tree_, &measurer);
            if (issues.size() != last_issue_count_) {
                last_issue_count_ = issues.size();
                std::fputs(layout::format_issues(issues).c_str(), stderr);
            }
        }

        draws_.clear();
        {
            ScopedTimer t{frame.paint_ms};
            render::PaintOptions po{};
            po.dpi_scale    = dpi_;
            po.measurer     = &measurer;
            po.glyphs       = &bindings_->glyphs;
            po.debug_bounds = cfg_.debug_bounds;
            po.clear_color  = theme_.background.fade(theme_.window_opacity);
            render::paint(tree_, draws_, po);
            // The atlas had to move glyphs mid-paint (it overflowed): uvs
            // recorded before the move are stale. Paint again; the second
            // pass finds every glyph resident at its new place.
            if (cfg_.fonts && cfg_.fonts->atlas_moved()) {
                draws_.clear();
                render::paint(tree_, draws_, po);
                (void)cfg_.fonts->atlas_moved();
            }
            memos_.end_frame();
            if (!overlays_.empty()) {
                auto sorted = overlays_.items();
                std::stable_sort(sorted.begin(), sorted.end(),
                                 [](const Overlay& a, const Overlay& b) { return a.layer < b.layer; });
                for (const auto& o : sorted) {
                    if (o.scrim > 0.0f)
                        draws_.fill_rect(Rect{Vec2{}, size_ * dpi_}, colors::black.fade(o.scrim));
                    render::paint(o.content, draws_, po);
                }
            }
        }

        // A widget mid-motion asked for another frame during the view, or
        // layout changed a scroll region's size (a scrollbar drawn from last
        // frame's numbers is now stale and needs one more pass).
        update_cursor();   // the view may have changed under a still pointer
        // ...and so may HOVER: a closed menu, a scrolled list, a rebuilt row.
        // Re-resolve it against the new layout so the right thing is lit
        // without a mouse wiggle; enter/leave handlers (on_enter/on_leave)
        // run, queued like any handler message.
        {
            const Vec2 p = input_.cursor();
            const Overlay* over = overlays_.empty() ? nullptr : overlays_.hit(p);
            bool modal_blocks = false;
            if (over == nullptr) for (const auto& o : overlays_.items()) modal_blocks = modal_blocks || o.dismiss == Dismiss::modal;
            static const Node empty{};
            const auto changed = input_.rehover(over ? over->content : (modal_blocks ? empty : tree_));
            if (!changed.empty()) {
                for (const auto& g : changed) collect_handlers(g);
                dirty_ = true;
            }
        }
        motion_pending_ = animation_was_requested() || tracked_.animating() ||
                          tip_waiting_ ||
                          take_scroll_geometry_changed() ||
                          // A `starting(...)` look lasts exactly one frame;
                          // the next one is owed or it would stick forever.
                          starting_pending_;

        frame.instances  = static_cast<std::uint32_t>(draws_.size());
        frame.draw_calls = static_cast<std::uint32_t>(draws_.batches().size());
        { ScopedTimer t{frame.render_ms};
          // what the main window's GPU is about to upload, so every child window gets it too
          const Rect cov = cfg_.fonts->atlas().dirty_region(), col = cfg_.fonts->atlas().color_dirty_region();
          if (frame_counter_ == 1) startup::mark("first frame: painted, presenting");
          window_.present(draws_, theme_.background.fade(theme_.window_opacity));
          note_main_uploads(cov, col); }
        // child windows follow the model on the same frame
        reconcile_children(model);
        if (!children_.empty()) {
            dirty_children_ = true;
            const double cdt = std::clamp(time_ - std::exchange(child_time_, time_), 0.0, 0.1);
            draw_children(model, cdt);
        }
        sync_text_input();
        if (frame_counter_ == 1 && startup::enabled()) {
            std::fprintf(stderr, "mayag: startup first frame: view %.1f cascade %.1f layout %.1f paint %.1f render %.1f ms\n",
                         frame.view_ms, frame.cascade_ms, frame.layout_ms, frame.paint_ms, frame.render_ms);
            startup::mark("first frame presented");
        }
        latency_.record(frame);
        if (framestats_) report_frames();
        // Frames nobody asked for. Frames the user is driving (moving the
        // pointer over a live view) don't count.
        if (input_since_render_) busy_.idle();
        else busy_.frame_presented(time_);
        input_since_render_ = false;
        if (busy_.busy() && !busy_warned_ && !cfg_.continuous_animation) {
            busy_warned_ = true;
            std::fprintf(stderr,
                "mayag: rendering continuously at %.0f fps for several seconds.\n"
                "       Something keeps asking for frames: an on_frame subscription\n"
                "       that is always on, or a spring that never settles.\n"
                "       An idle mayag app should sit at 0%% CPU.\n",
                busy_.frame_rate());
        }
        if (!animating()) busy_.idle();
    }

    W&                  window_;
    AppConfig           cfg_;
    std::optional<jaal::platform::native_reactor::registration> reg_;
    bool                poll_window_ = false;   // attached window has no fd (Cocoa)
    bool                external_loop_ = false; // a foreign loop drives input and frames
    mutable bool        poll_only_ = false;     // owes_frame() said yes only to poll
    std::uint64_t       emitted_ = 0;           // input events that produced a Msg
    time_point   next_poll_{};
    std::function<std::size_t(const window_events&)> cx_;
    std::function<void(int)> stop_;
    std::function<void()> live_kernel_;   // draw now, from an OS modal loop

    std::optional<detail::StackBindings> bindings_;
    Node                tree_{};
    DrawList            draws_{};
    Interaction         input_{};
    OverlayList         overlays_;
    TrackedSet          tracked_;
    CursorShape         current_cursor_ = CursorShape::arrow;

    Vec2                size_{};
    float               dpi_ = 1.0f;
    double              time_ = 0.0;
    // The cascade's frame state: transitions in flight, the time they owe,
    // and which nodes existed last frame (for `starting(...)`).
    const Clock*        clock_ = nullptr;   // the kernel's, when it has state (sim)
    Clock               own_clock_{};
    StyleMotion         motion_{};
    std::vector<HandlerResult> handled_{};
    bool                handler_type_warned_ = false;
    bool                springs_ = false;
    Theme               theme_{};        // what this frame is drawn in
    Theme               theme_from_{};   // a morph runs from here...
    Theme               theme_to_{};     // ...to here (the program's theme)
    bool                theme_seen_ = false;
    bool                morphing_ = false;
    float               morph_x_ = 1.0f, morph_v_ = 0.0f;
    double              morph_dt_ = 0.0;
    double              motion_dt_ = 0.0;
    std::unordered_set<std::uint64_t> seen_ids_{};
    bool                seen_ids_valid_ = false;
    bool                starting_pending_ = false;
    time_point   start_{};
    bool                dirty_ = true;
    bool                draw_owed_ = false;     // a draw was held back for pacing
    bool                tick_pending_ = false;  // a frame tick was dispatched; draw it next
    time_point   last_draw_{};
    time_point   vblank_{};                 // a refresh boundary (set at start)
    duration_t     period_ = std::chrono::duration_cast<duration_t>(kFramePeriod);
    std::int64_t        drawn_slot_ = -1;                       // slot of the last draw
    bool                latched_ = false;                       // that slot had its late draw
    std::uint64_t       display_seq_ = 0;
    bool                motion_pending_ = false;

    bool                frames_on_ = false;
    std::function<typename P::Msg(double)> frame_make_;
    time_point   next_frame_{};
    std::optional<time_point> last_tick_;

    std::uint64_t       frame_counter_ = 0;
    // MAYAG_FRAMESTATS=1: print per-stage means about once a second.
    bool framestats_ = [] { const char* e = std::getenv("MAYAG_FRAMESTATS"); return e && *e && *e != '0'; }();
    double framestats_at_ = 0.0;
    std::size_t framestats_frames_ = 0;
    void report_frames() {
        ++framestats_frames_;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (framestats_at_ == 0.0) framestats_at_ = now;
        if (now - framestats_at_ < 1.0) return;
        const FrameTiming m = latency_.mean(framestats_frames_);
        std::uint64_t skipped = 0;
        if constexpr (requires { window_.frames_skipped(); }) skipped = window_.frames_skipped();
        std::fprintf(stderr, "mayag: %zu frames  view %.3f  cascade %.3f  layout %.3f  paint %.3f  render %.3f  = %.3f ms  (%u instances, %u draws, %llu identical skipped)\n",
                     framestats_frames_, m.view_ms, m.cascade_ms, m.layout_ms, m.paint_ms, m.render_ms, m.cpu_ms(),
                     m.instances, m.draw_calls, static_cast<unsigned long long>(skipped));
        framestats_at_ = now;
        framestats_frames_ = 0;
    }
    int                 coalesced_moves_ = 0;
    LatencyStats        latency_{};
    MemoCache           memos_{};
    // MAYAG_MEMO=0 turns memo() off (to check it changes nothing).
    bool memo_on_ = [] { const char* e = std::getenv("MAYAG_MEMO"); return e == nullptr || *e != '0'; }();
    std::uint64_t       interaction_reads_ = 0;
    BusyLoopDetector    busy_{};
    bool                busy_warned_ = false;
    bool                input_since_render_ = false;
    std::size_t         last_issue_count_ = static_cast<std::size_t>(-1);
};

}  // namespace mayag
