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
#include "../style/theme.hpp"
#include "effects.hpp"
#include "event.hpp"
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

/// Which node id is a click on? `is(c, "save")` reads like the button name.
template <class G>
[[nodiscard]] bool is(const G& g, std::string_view name) noexcept {
    return g.node == dsl::node_id(name);
}

// ── configuration ───────────────────────────────────────────────────────

struct AppConfig {
    std::string title = "mayag";
    Vec2        size{1024, 640};
    bool        resizable = true;
    bool        vsync     = true;
    Theme       theme     = themes::midnight;

    /// The font engine. Null: discovered from the system at startup.
    std::shared_ptr<typo::FontStack> fonts = nullptr;

    bool debug_bounds = false;

    /// Print the active renderer to stderr at startup.
    bool log_renderer = true;

    /// Keep running when update() or view() throws: drop the message (the
    /// model is put back) or keep the last good frame. Off: stop with 70.
    bool catch_exceptions = true;

    /// Told about every fault (where, what). Empty: one line to stderr.
    std::function<void(std::string_view where, std::string_view what)> on_error;
};

// ── the host ────────────────────────────────────────────────────────────

/// One display refresh. The host paces frames itself when nothing better is
/// available (headless, or a compositor that doesn't send frame callbacks).
inline constexpr std::chrono::nanoseconds kFramePeriod{16'666'667};

template <Program P, class W>
class window_host {
  public:
    using event_type = window_events;
    using clock      = std::chrono::steady_clock;

    window_host(W& window, AppConfig cfg) : window_(window), cfg_(std::move(cfg)) {
        if (cfg_.fonts == nullptr) cfg_.fonts = typo::system::default_stack();
        bindings_.emplace(*cfg_.fonts);
        window_.set_atlas_source(&cfg_.fonts->atlas());
        window_.set_coverage_sampler(&bindings_->sampler);
        input_.set_multi_click_interval(window_.double_click_interval());
        size_ = window_.size();
        dpi_  = window_.dpi_scale();
        start_ = clock::now();
    }

    window_host(const window_host&)            = delete;
    window_host& operator=(const window_host&) = delete;

    // ── jaal protocol ───────────────────────────────────────────────────

    template <class Cx>
    void attach(Cx& cx) {
        if constexpr (requires { window_.fd(); }) {
            const int fd = window_.fd();
            if (fd >= 0) {
                auto reg = cx.watch(fd, jaal::interest::read, kWindow);
                if (!reg) { cx.stop(70); return; }
                reg_.emplace(std::move(*reg));
            }
        }
        poll_window_ = !reg_.has_value();
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
        // A window with no fd is read here, on a short poll; one with an fd
        // was already read in on_ready.
        if (poll_window_ && cx_) {
            next_poll_ = clock::now() + kPollPeriod;
            pump_erased();
        }
        draw(k.model(), [&](std::exception_ptr e) { k.view_faulted(std::move(e)); });
    }

    /// Owed when the frame clock is running and its tick is due, a spring is
    /// moving, or presentation state (hover, focus) changed under the model.
    [[nodiscard]] bool owes_frame() const noexcept {
        if (dirty_) return true;
        const auto now = clock::now();
        if (animating() && now >= next_frame_) return true;
        return poll_window_ && now >= next_poll_;   // fd-less window: time to look
    }

    [[nodiscard]] std::optional<std::chrono::milliseconds> wait_hint() const noexcept {
        using std::chrono::milliseconds;
        if (dirty_) return milliseconds{0};
        if (!animating()) {
            // Wayland wakes us through its fd. A window with none (Cocoa)
            // has to be asked: poll it often enough that input feels instant.
            if (!poll_window_) return std::nullopt;
            return std::chrono::ceil<milliseconds>(kPollPeriod);
        }
        // Rounded UP so the loop never wakes before the frame is due.
        const auto left = std::chrono::ceil<milliseconds>(next_frame_ - clock::now());
        return std::max(left, milliseconds{0});
    }

    // The frame clock. One per program; jaal starts it when subscribe()
    // returns on_frame and stops it when it doesn't.
    template <class Msg>
    void start_source(const fx::on_frame::type<Msg>& p, const fx::on_frame::key_type&,
                      jaal::Sink<Msg> sink) {
        frame_sink_ = [make = p.make, sink = std::move(sink)](double dt) mutable {
            (void)sink.send(make(dt));
        };
        frames_on_ = true;
        last_tick_.reset();
        next_frame_ = clock::now();
    }
    void stop_source(std::type_identity<fx::on_frame>, const fx::on_frame::key_type&) {
        frames_on_ = false;
        frame_sink_ = nullptr;
    }

    void handle(const SetTitle& e)       { window_.set_title(e.title); }
    void handle(const WriteClipboard& e) { window_.set_clipboard(e.text); }
    void handle(const SetCursor& e)      { window_.set_cursor(e.shape); }
    void handle(const SetFullscreen& e) {
        if constexpr (requires { window_.set_fullscreen(e.on); }) window_.set_fullscreen(e.on);
    }
    void handle(const FocusNode& e) { input_.set_focus(e.node_id); dirty_ = true; }
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
    [[nodiscard]] const LatencyStats& latency() const noexcept { return latency_; }
    [[nodiscard]] bool busy_loop_detected() const noexcept { return busy_warned_; }
    [[nodiscard]] std::string_view renderer_name() const noexcept { return window_.renderer_name(); }
    [[nodiscard]] bool frames_on() const noexcept { return frames_on_; }

    [[nodiscard]] std::optional<Rect> node_rect(std::uint64_t id) const {
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

    /// Fire the frame clock now with this dt. A headless driver uses it to
    /// step animations deterministically.
    void tick_frame(double dt) {
        if (frames_on_ && frame_sink_) frame_sink_(dt);
        time_ += dt;
        if (tracked_.animating()) { tracked_.step_all(dt); dirty_ = true; }
    }

    /// Time moves only through tick_frame (tests).
    void set_manual_clock(bool on) noexcept { manual_clock_ = on; }

    /// Frame bookkeeping for the real loop: called from present() so the
    /// tick lands right before the frame it animates. A headless driver steps
    /// the clock itself (tick_frame) and turns this off.
    void advance_clock() {
        if (manual_clock_) return;
        const auto now = clock::now();
        time_ = std::chrono::duration<double>(now - start_).count();
        if (!animating() || now < next_frame_) return;
        const double dt = last_tick_
            ? std::min(std::chrono::duration<double>(now - *last_tick_).count(), 0.1)
            : 1.0 / 60.0;
        last_tick_ = now;
        // Keep phase: whole periods, resync after a stall.
        next_frame_ += kFramePeriod;
        if (next_frame_ <= now) next_frame_ = now + kFramePeriod;
        if (frames_on_ && frame_sink_) frame_sink_(dt);
        if (tracked_.animating()) { tracked_.step_all(dt); dirty_ = true; }
    }

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
        return frames_on_ || motion_pending_ || tracked_.animating();
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
        const auto gestures = input_.handle(ev, tree_, window_.now());
        if (input_.hovered() != before_hover || input_.pressed() != before_pressed ||
            input_.focused() != before_focus) {
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
        if constexpr (std::is_constructible_v<window_events, E>) return cx.emit(window_events{e});
        else return 0;
    }

    template <class Cx>
    void emit_gestures(Cx& cx, const std::vector<Gesture>& gestures) {
        using K = Gesture::Kind;
        for (const auto& g : gestures) {
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

    template <class OnFault>
    void draw_impl(const typename P::Model& model, OnFault&& fault) {
        advance_clock();
        dirty_ = false;
        size_ = window_.size();
        dpi_  = window_.dpi_scale();

        cfg_.fonts->begin_frame(frame_counter_++);
        FrameTiming frame{};
        frame.coalesced_moves = std::exchange(coalesced_moves_, 0);

        overlays_.clear();
        tracked_.clear();
        clear_animation_request();

        Ctx ctx{size_, dpi_, time_, cfg_.theme, &latency_, &bindings_->measurer,
                &tracked_, &overlays_, &input_};
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
        { ScopedTimer t{frame.layout_ms}; layout::layout_tree(tree_, size_, measurer); }
        tracked_.observe_all(tree_);

        for (auto& o : overlays_.items()) {
            const Node* a = o.anchor_id != 0 ? tree_.find(o.anchor_id) : nullptr;
            const Rect anchor = o.anchor_id != 0 ? (a ? a->frame() : Rect{}) : Rect{Vec2{}, size_};
            const Vec2 natural = layout::measure(o.content, layout::Constraints{size_.x, size_.y}, measurer);
            const Rect placed = overlay::resolve(o.placement, anchor, natural, size_,
                                                 o.offset, input_.cursor());
            layout::arrange(o.content, placed, measurer);
        }

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

        // A widget mid-motion asked for another frame during the view.
        motion_pending_ = animation_was_requested() || tracked_.animating();

        frame.instances  = static_cast<std::uint32_t>(draws_.size());
        frame.draw_calls = static_cast<std::uint32_t>(draws_.batches().size());
        { ScopedTimer t{frame.render_ms}; window_.present(draws_, cfg_.theme.background); }
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
    clock::time_point   next_poll_{};
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
    clock::time_point   start_{};
    bool                dirty_ = true;
    bool                motion_pending_ = false;
    bool                manual_clock_ = false;

    bool                frames_on_ = false;
    std::function<void(double)> frame_sink_;
    clock::time_point   next_frame_{};
    std::optional<clock::time_point> last_tick_;

    std::uint64_t       frame_counter_ = 0;
    int                 coalesced_moves_ = 0;
    LatencyStats        latency_{};
    BusyLoopDetector    busy_{};
    bool                busy_warned_ = false;
    bool                input_since_render_ = false;
    std::size_t         last_issue_count_ = static_cast<std::size_t>(-1);
};

}  // namespace mayag
