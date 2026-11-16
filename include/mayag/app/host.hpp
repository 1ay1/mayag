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
#include "../dsl/dsl.hpp"
#include "../font/font.hpp"
#include "../font/system.hpp"
#include "../layout/audit.hpp"
#include "../layout/flex.hpp"
#include "../render/draw_list.hpp"
#include "../render/painter.hpp"
#include "../scene/node.hpp"
#include "../scene/overlay.hpp"
#include "../style/cascade.hpp"
#include "../style/theme.hpp"
#include "effects.hpp"
#include "event.hpp"
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

// ── what view() may know ────────────────────────────────────────────────

/// Everything `view()` may know about the outside world, besides the model.
/// The view reads hover/press/focus from here instead of storing them, and
/// posts overlays and tracked springs into it.
struct Ctx {
    Vec2   size{};
    float  dpi_scale = 1.0f;
    double time = 0.0;          ///< seconds since start
    Theme  theme = themes::midnight;

    /// Last frames' measured budget, so an app can draw its own latency.
    const LatencyStats* latency = nullptr;

    /// Measured input to photon (the compositor's presentation feedback),
    /// when the window can report it.
    platform::PhotonStats photon{};

    const layout::TextMeasurer* text_measurer = nullptr;
    [[nodiscard]] const layout::TextMeasurer& measurer() const {
        return text_measurer != nullptr ? *text_measurer : layout::default_measurer();
    }

    TrackedSet*  tracked  = nullptr;
    OverlayList* overlays = nullptr;
    const Interaction* input = nullptr;

    /// Follow a node's measured rect with a spring.
    void track(const Tracked& t, std::uint64_t node) const {
        t.follow(node);
        if (tracked != nullptr) tracked->register_tracked(&t);
        if (t.animating()) request_animation_frame();
    }
    /// Post a menu, modal or tooltip from anywhere in the view.
    void overlay(Overlay o) const {
        if (overlays != nullptr) overlays->add(std::move(o));
    }

    [[nodiscard]] bool hovered(std::uint64_t id) const noexcept { return input && input->is_hovered(id); }
    [[nodiscard]] bool pressed(std::uint64_t id) const noexcept { return input && input->is_pressed(id); }
    [[nodiscard]] bool focused(std::uint64_t id) const noexcept { return input && input->is_focused(id); }
    template <dsl::fixed_string Name>
    [[nodiscard]] bool hovered() const noexcept { return hovered(dsl::node_id(Name.view())); }
    template <dsl::fixed_string Name>
    [[nodiscard]] bool pressed() const noexcept { return pressed(dsl::node_id(Name.view())); }
    template <dsl::fixed_string Name>
    [[nodiscard]] bool focused() const noexcept { return focused(dsl::node_id(Name.view())); }
};

// ── the program shape ───────────────────────────────────────────────────
//
// jaal's view hook is `static Out view(const Model&)`. The window facts a
// view may read (size, theme, hover, ...) are ambient: `mayag::ctx()` inside
// view() returns the frame's Ctx. Outside a frame it returns a default one,
// so calling view() directly in a test still works.

namespace detail {
inline thread_local const Ctx* current_ctx = nullptr;

/// Installs a Ctx for the duration of one view() call.
struct ctx_scope {
    const Ctx* saved;
    explicit ctx_scope(const Ctx& c) noexcept : saved(current_ctx) { current_ctx = &c; }
    ~ctx_scope() { current_ctx = saved; }
    ctx_scope(const ctx_scope&) = delete;
    ctx_scope& operator=(const ctx_scope&) = delete;
};

/// The three font adapters layout and the painter read through. Held by
/// value so the pointers handed out stay valid for the host's lifetime.
struct StackBindings {
    typo::StackMeasurer      measurer;
    typo::StackGlyphRenderer glyphs;
    typo::StackSampler       sampler;
    explicit StackBindings(typo::FontStack& s) : measurer{s}, glyphs{s}, sampler{s} {}
};
}  // namespace detail

/// The current frame's Ctx. Call it from view().
[[nodiscard]] inline const Ctx& ctx() noexcept {
    static const Ctx fallback{};
    return detail::current_ctx != nullptr ? *detail::current_ctx : fallback;
}

/// A jaal program mayag can draw: `static Node view(const Model&)`.
template <class P>
concept Program = jaal::Program<P> && jaal::Viewable<P, Node>;

/// view() with `c` as the ambient Ctx.
template <Program P>
[[nodiscard]] Node view_of(const typename P::Model& m, const Ctx& c) {
    detail::ctx_scope scope{c};
    return P::view(m);
}

/// Build a subtree in another theme -- ALL of it, every widget included.
///
///     themed(themes::daylight, [&] { return settings_panel(m); })
///     themed(brutal, [&](const Ctx& c) { return slider(c, "vol", m.vol, 120); })
///
/// Inside `build`, `ctx().theme` IS `t`, so widgets that read the theme
/// directly draw in it; and the result is scoped with `| theme(t)`, so every
/// token, sheet look and motion below resolves against it too. The one call
/// that makes "this part of the app looks different" true for everything.
template <class F>
[[nodiscard]] auto themed(const Theme& t, F&& build) {
    Ctx c = ctx();
    c.theme = t;
    Node n = [&] {
        detail::ctx_scope scope{c};
        if constexpr (std::invocable<F&, const Ctx&>) return Node(build(c));
        else return Node(build());
    }();
    return dsl::node(std::move(n)) | dsl::theme(t);
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

/// Which node id is a click on? `is(c, "save")` reads like the button name.
template <class G>
[[nodiscard]] bool is(const G& g, std::string_view name) noexcept {
    return g.node == dsl::node_id(name);
}
/// Indexed twin: `is(c, "row", i)` matches `id_of("row", i)` in the view.
template <class G>
[[nodiscard]] bool is(const G& g, std::string_view base, std::size_t index) noexcept {
    return g.node == dsl::node_id(base, index);
}

// ── configuration ───────────────────────────────────────────────────────

struct AppConfig {
    std::string title = "mayag";
    Vec2        size{1024, 640};
    bool        resizable = true;
    bool        vsync     = true;
    Theme       theme     = themes::midnight;
    /// Animate theme changes: when `P::theme(model)` changes, the WHOLE UI
    /// morphs from the old theme to the new one -- colours in Oklab, radii,
    /// spacing, type size, depth, springs -- on the theme's own motion
    /// (instant when the new theme's motion_scale is 0: reduced motion).
    bool        theme_morph = true;

    /// The font engine. Null: discovered from the system at startup.
    std::shared_ptr<typo::FontStack> fonts = nullptr;

    bool debug_bounds = false;

    /// Print the active renderer to stderr at startup.
    bool log_renderer = true;

    /// Keep running when update() or view() throws: drop the message (the
    /// model is put back) or keep the last good frame. Off: stop with 70.
    bool catch_exceptions = true;

    /// Told about every fault (where, what). Empty: one line to stderr.
    std::function<void(std::string_view where, std::string_view what)> on_error = {};
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
            const int fd = external_loop_ ? -1 : window_.fd();
            if (fd >= 0) {
                auto reg = cx.watch(fd, jaal::interest::read, kWindow);
                if (!reg) { cx.stop(70); return; }
                reg_.emplace(std::move(*reg));
            }
        }
        poll_window_ = !reg_.has_value() && !external_loop_;
        cx_ = [&cx](const window_events& e) { return cx.emit(e); };
        stop_ = [&cx](int code) { cx.stop(code); };
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
    }

    template <class K>
    void present(K& k) {
        const bool poll_only = std::exchange(poll_only_, false);

        // A window with no fd (Cocoa) is read here on a short poll. When the
        // poll was the only reason we were called, draw only if it brought
        // something; the messages it routed are already folded.
        if (poll_window_ && cx_ && now_() >= next_poll_) {
            next_poll_ = now_() + kPollPeriod;
            const auto before = emitted_;
            pump_erased();
            if (poll_only && !dirty_ && emitted_ == before && handled_.empty()) return;
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
        if (external_loop_ && tick_pending_) return true;   // a display-link frame draws now
        if ((dirty_ || draw_owed_) && now >= next_draw_at(now)) return true;
        if (!external_loop_ && animating() && now >= next_frame_) return true;
        // fd-less window: time to look. Remembered so present() can skip the
        // draw when the look finds nothing. (jaal skips this call when the
        // model changed, so a stale flag can't suppress a real frame.)
        poll_only_ = poll_window_ && now >= next_poll_;
        return poll_only_;
    }

    [[nodiscard]] std::optional<std::chrono::nanoseconds> wait_hint() const noexcept {
        using ns = std::chrono::nanoseconds;
        const auto now = now_();
        if (dirty_ || draw_owed_) {
            return std::max(std::chrono::duration_cast<ns>(next_draw_at(now) - now), ns{0});
        }
        // A foreign loop's display link drives animation frames.
        if (external_loop_) return std::nullopt;
        if (!animating()) {
            // Wayland wakes us through its fd. A window with none (Cocoa)
            // has to be asked: poll it often enough that input feels instant.
            if (!poll_window_) return std::nullopt;
            return kPollPeriod;
        }
        // Nanoseconds, so a late-latch wake lands on time, not up to 1 ms late.
        return std::max(std::chrono::duration_cast<ns>(next_frame_ - now), ns{0});
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
    void handle(const FocusNode& e) { input_.set_focus_visible(e.node_id); dirty_ = true; }
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

    void release() { reg_.reset(); cx_ = nullptr; stop_ = nullptr; }

    // ── introspection (tests, examples) ─────────────────────────────────

    [[nodiscard]] W& window() noexcept { return window_; }
    [[nodiscard]] const Interaction& input() const noexcept { return input_; }
    [[nodiscard]] const Node& tree() const noexcept { return tree_; }
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

    [[nodiscard]] bool animating() const noexcept {
        return frames_on_ || motion_pending_ || tracked_.animating() || motion_.animating() || morphing_;
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

        // The close button. A program that doesn't handle it still closes.
        if (std::holds_alternative<CloseRequest>(ev)) {
            if (emit(cx, CloseRequest{}) == 0) cx.stop(0);
            return;
        }

        // Tab moves focus; Enter/Space on the focused node is a click.
        if (const auto* k = std::get_if<KeyEvent>(&ev)) {
            if (k->key == Key::tab && !k->mods.ctrl && !k->mods.super) {
                input_.focus_next(tree_, k->mods.shift);
                dirty_ = true;
            }
            if ((k->key == Key::enter || k->key == Key::space) &&
                input_.focused() != 0 && k->mods.none()) {
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
        if (!overlays_.empty()) {
            if (const auto pos = event_position(ev)) {
                const bool is_press = std::holds_alternative<MouseDown>(ev);
                if (const Overlay* over = overlays_.hit(*pos)) {
                    const auto gestures = input_.handle(ev, over->content, window_.now());
                    if (!gestures.empty()) dirty_ = true;
                    (void)emit(cx, ev);
                    emit_gestures(cx, gestures);
                    return;
                }
                if (is_press) {
                    const auto gone = overlays_.dismissed_by(*pos);
                    if (!gone.empty()) {
                        for (std::uint64_t id : gone) (void)emit(cx, Left{id});
                        dirty_ = true;
                        return;
                    }
                    if (overlays_.captures_input()) return;
                }
            }
        }

        if (std::holds_alternative<MouseMove>(ev)) {
            const CursorShape want = cursor_at(tree_, input_.cursor());
            if (want != current_cursor_) {
                current_cursor_ = want;
                window_.set_cursor(want);
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
    void emit_gestures(Cx& cx, const std::vector<Gesture>& gestures) {
        using K = Gesture::Kind;
        for (const auto& g : gestures) {
            collect_handlers(g);
            switch (g.kind) {
                case K::click:    (void)emit(cx, Clicked{g.node_id, g.local, g.click_count, g.button}); break;
                case K::activate: (void)emit(cx, Activated{g.node_id}); break;
                case K::press:    (void)emit(cx, Pressed{g.node_id, g.local, g.button}); break;
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
    void collect_handlers(const Gesture& g) {
        using K = Gesture::Kind;
        const Node* n = node_by_id(g.node_id);
        if (n == nullptr || n->cascade() == nullptr || n->cascade()->handlers.empty()) return;
        const auto run = [&](On on, const auto& event) {
            for (const auto& h : n->cascade()->handlers)
                if (h.on == on) {
                    HandlerResult r = h.call(h.fn, &event);
                    if (r.msg) handled_.push_back(std::move(r));
                }
        };
        switch (g.kind) {
            case K::click:    run(On::click, Clicked{g.node_id, g.local, g.click_count, g.button}); break;
            case K::activate: run(On::click, Clicked{g.node_id, {}, 1, MouseButton::left}); break;
            case K::press:    run(On::press, Pressed{g.node_id, g.local, g.button}); break;
            case K::release:  run(On::release, Released{g.node_id, g.local, g.button}); break;
            case K::enter:    run(On::enter, Entered{g.node_id}); break;
            case K::leave:    run(On::leave, Left{g.node_id}); break;
            case K::drag:     run(On::drag, Dragged{g.node_id, g.local, g.delta}); break;
            case K::scroll:   run(On::scroll, Scrolled{g.node_id, g.local, g.delta}); break;
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
    [[nodiscard]] static CursorShape cursor_at(const Node& n, Vec2 p) {
        CursorShape found = CursorShape::arrow;
        walk_cursor(n, p, found);
        return found;
    }
    static void walk_cursor(const Node& n, Vec2 p, CursorShape& found) {
        if (!n.frame().contains(p)) return;
        if (n.style().cursor != CursorShape::arrow) found = n.style().cursor;
        for (const auto& c : n.children()) walk_cursor(c, p, found);
    }

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
        {
            ScopedTimer t{frame.view_ms};
            try {
                tree_ = view_of<P>(model, ctx);
            } catch (...) {
                fault(std::current_exception());
                if (tree_.children().empty()) return;   // nothing good to show
            }
        }

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
            CascadeResult cr = cascade(tree_, ci);
            for (auto& o : overlays_.items()) {
                const CascadeResult r = cascade(o.content, ci);
                cr.transitions = cr.transitions || r.transitions;
                cr.starting = cr.starting || r.starting;
            }
            const bool springs = cr.transitions;
            starting_pending_ = cr.starting;

            seen_ids_.clear();
            collect_starting_ids(tree_, seen_ids_);
            for (const auto& o : overlays_.items()) collect_starting_ids(o.content, seen_ids_);
            seen_ids_valid_ = true;

            springs_ = springs;
        }
        { ScopedTimer t{frame.layout_ms}; layout::layout_tree(tree_, size_, measurer); }
        tracked_.observe_all(tree_);

        for (auto& o : overlays_.items()) {
            const Node* a = o.anchor_id != 0 ? tree_.find(o.anchor_id) : nullptr;
            const Rect anchor = o.anchor_id != 0 ? (a ? a->frame() : Rect{}) : Rect{Vec2{}, size_};
            const Vec2 natural = layout::measure(o.content, layout::Constraints{size_.x, size_.y}, measurer);
            const Rect placed = overlay::resolve(o.placement, anchor, natural, size_,
                                                 o.offset, o.at.value_or(input_.cursor()));
            layout::arrange(o.content, placed, measurer);
        }

        // Transitions run AFTER layout. Everything they animate is paint-only
        // (colour, border, radii, shadows, opacity, transform), and layout has
        // already clamped radii to the real box: a pill that becomes a square
        // springs from its true radius (11 px), not from `radius_full`'s 1e9.
        motion_.begin_frame();
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
            render::paint(tree_, draws_, po);
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
        motion_pending_ = animation_was_requested() || tracked_.animating() ||
                          take_scroll_geometry_changed() ||
                          // A `starting(...)` look lasts exactly one frame;
                          // the next one is owed or it would stick forever.
                          starting_pending_;

        frame.instances  = static_cast<std::uint32_t>(draws_.size());
        frame.draw_calls = static_cast<std::uint32_t>(draws_.batches().size());
        { ScopedTimer t{frame.render_ms}; window_.present(draws_, theme_.background); }
        latency_.record(frame);
        // Frames nobody asked for. Frames the user is driving (moving the
        // pointer over a live view) don't count.
        if (input_since_render_) busy_.idle();
        else busy_.frame_presented(time_);
        input_since_render_ = false;
        if (busy_.busy() && !busy_warned_) {
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
    int                 coalesced_moves_ = 0;
    LatencyStats        latency_{};
    BusyLoopDetector    busy_{};
    bool                busy_warned_ = false;
    bool                input_since_render_ = false;
    std::size_t         last_issue_count_ = static_cast<std::size_t>(-1);
};

}  // namespace mayag
