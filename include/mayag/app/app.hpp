#pragma once
// mayag::run<P>() — the application runtime
//
// This is the answer to "how do I actually ship a GUI".
//
// An application is a `Program`: five pure functions and two types.
//
//     struct Counter {
//         struct Model { int n = 0; };
//         using Msg = std::variant<Inc, Dec, Quit>;
//
//         static Model init();
//         static std::pair<Model, Cmd<Msg>> update(Model, Msg);
//         static Node view(const Model&, const Ctx&);
//         static Sub<Msg> subscribe(const Model&);
//     };
//     int main() { return mayag::run<Counter>({.title = "Counter"}); }
//
// NONE of those functions touch the window, the GPU, the clock, or the file
// system. `update` returns a Cmd describing what should happen; `subscribe`
// returns a Sub describing what to listen for. This runtime is the only code
// in the entire framework that performs I/O, and it is the only code you do
// not write.
//
// What that buys, concretely:
//   * `update()` is testable with `==` — no window, no mocks
//   * the whole app is replayable: same events in, same pixels out
//   * headless CI runs the REAL loop, not a parallel fake one
//   * an idle app blocks and uses 0% CPU, because "am I animating" is
//     derivable from the subscriptions rather than a flag someone forgot

#include "../layout/flex.hpp"
#include "../layout/audit.hpp"
#include "../render/painter.hpp"
#include "../scene/node.hpp"
#include "../scene/overlay.hpp"
#include "../core/motion.hpp"
#include "../style/theme.hpp"
#include "../font/font.hpp"
#include "../font/system.hpp"
#include "cmd.hpp"
#include "event.hpp"
#include "interaction.hpp"
#include "latency.hpp"
#include "platform.hpp"
#include "../platform/waker.hpp"
#include "sub.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <concepts>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mayag {

// ── the context passed to view() ────────────────────────────────────────

/// Everything `view()` may know about the outside world. Deliberately small
/// and deliberately a VALUE: a view is a pure function of (Model, Ctx), so
/// two identical arguments must always produce the same tree.
struct Ctx {
    Vec2  size{};            ///< logical window size
    float dpi_scale = 1.0f;
    double time = 0.0;       ///< seconds since start, for animation
    Theme theme = themes::midnight;

    /// Last frame's measured budget.
    ///
    /// Available to `view()` so an app can DRAW its own latency — which is
    /// the only honest way to claim a framework is fast, and the fastest way
    /// to notice when it stops being.
    const LatencyStats* latency = nullptr;

    /// The text measurer the frame will be laid out with.
    ///
    /// A view needs this whenever it must know a real size before layout runs
    /// — measured virtualisation, custom truncation, a canvas that positions
    /// against text. Exposing it removes the temptation to hardcode a
    /// multiplier, which is the single most common source of "the box is
    /// slightly too small" bugs.
    const layout::TextMeasurer* text_measurer = nullptr;

    [[nodiscard]] const layout::TextMeasurer& measurer() const {
        return text_measurer != nullptr ? *text_measurer : layout::default_measurer();
    }

    /// Where `view()` registers geometry it wants to follow.
    ///
    /// A view calls `c.track(m.underline, node_id("filter-active"))`; after
    /// layout the runtime records where that node ACTUALLY landed and the
    /// spring chases it. The app never invents a coordinate, which is the
    /// whole point — a hardcoded stride is a guess about a layout the app
    /// cannot see, and it drifts the moment a label or font changes.
    TrackedSet* tracked = nullptr;

    /// Follow a node's measured rect.
    void track(const Tracked& t, std::uint64_t node) const {
        t.follow(node);
        if (tracked != nullptr) tracked->register_tracked(&t);
        if (t.animating()) request_animation_frame();
    }

    /// Where `view()` posts menus, modals and tooltips.
    ///
    /// Mutable because a view is a pure function of the MODEL — the overlay
    /// list is an output channel, not state. Two identical models still
    /// produce identical overlays, which is the property that matters.
    OverlayList* overlays = nullptr;

    /// Post an overlay from anywhere in the view.
    void overlay(Overlay o) const {
        if (overlays != nullptr) overlays->add(std::move(o));
    }

    /// Live interaction state, so a view can style its own hover/press
    /// without storing any of it in the Model. This is the GPU-UI equivalent
    /// of CSS `:hover` — transient presentation state that would be noise in
    /// application state.
    const Interaction* input = nullptr;

    [[nodiscard]] bool hovered(std::uint64_t id) const noexcept {
        return input != nullptr && input->is_hovered(id);
    }
    [[nodiscard]] bool pressed(std::uint64_t id) const noexcept {
        return input != nullptr && input->is_pressed(id);
    }
    [[nodiscard]] bool focused(std::uint64_t id) const noexcept {
        return input != nullptr && input->is_focused(id);
    }
    /// By compile-time name, matching `id<"save">` in the DSL.
    template <dsl::fixed_string Name>
    [[nodiscard]] bool hovered() const noexcept {
        return hovered(dsl::node_id(Name.view()));
    }
    template <dsl::fixed_string Name>
    [[nodiscard]] bool pressed() const noexcept {
        return pressed(dsl::node_id(Name.view()));
    }
    template <dsl::fixed_string Name>
    [[nodiscard]] bool focused() const noexcept {
        return focused(dsl::node_id(Name.view()));
    }
};

// ── the Program concept ─────────────────────────────────────────────────

namespace detail {

template <typename P>
concept HasEffectfulInit = requires {
    { P::init() } -> std::same_as<std::pair<typename P::Model, Cmd<typename P::Msg>>>;
};

template <typename P>
concept HasPlainInit = requires {
    { P::init() } -> std::same_as<typename P::Model>;
};

template <typename P>
concept HasSubscribe = requires(const typename P::Model& m) {
    { P::subscribe(m) } -> std::same_as<Sub<typename P::Msg>>;
};

template <typename P>
concept ViewWithCtx = requires(const typename P::Model& m, const Ctx& c) {
    { P::view(m, c) } -> std::convertible_to<Node>;
};

template <typename P>
concept ViewModelOnly = requires(const typename P::Model& m) {
    { P::view(m) } -> std::convertible_to<Node>;
};

}  // namespace detail

/// The contract. Everything else in this header is machinery for honouring it.
template <typename P>
concept Program =
    requires { typename P::Model; typename P::Msg; } &&
    (detail::HasEffectfulInit<P> || detail::HasPlainInit<P>) &&
    (detail::ViewWithCtx<P> || detail::ViewModelOnly<P>) &&
    requires(typename P::Model m, typename P::Msg msg) {
        { P::update(std::move(m), std::move(msg)) }
            -> std::same_as<std::pair<typename P::Model, Cmd<typename P::Msg>>>;
    };

// ── configuration ───────────────────────────────────────────────────────

/// How the runtime trades latency against CPU.
enum class LatencyMode : std::uint8_t {
    /// Render as soon as something changes. Lowest latency, and the right
    /// default: the CPU cost of a frame is ~1 ms, so there is nothing to
    /// save by waiting.
    immediate,
    /// Coalesce for a whole refresh interval before rendering. Fewer frames
    /// on a burst of input, at the cost of up to one refresh of latency.
    /// For battery-sensitive or very heavy views.
    batched,
};

struct AppConfig {
    std::string title = "mayag";
    Vec2        size{1024, 640};
    bool        resizable = true;
    bool        vsync     = true;
    Theme       theme     = themes::midnight;

    /// The font engine: system typefaces, kerning, and complete per-script
    /// fallback. When null, the runtime builds one with
    /// `typo::system::default_stack()` at boot — which discovers the platform
    /// fonts and, failing that, uses a synthesized last-resort face. So text
    /// always renders correctly with no configuration, and there is no second
    /// engine to fall back to.
    std::shared_ptr<typo::FontStack> fonts = nullptr;

    /// Overlay every node's rect. Bound to a key in your own subscribe() if
    /// you want to toggle it at runtime.
    bool debug_bounds = false;

    /// Print the active renderer to stderr at startup.
    ///
    /// On by default because the GPU is selected by fallthrough and a
    /// fallback to software is otherwise silent — the app still works, just
    /// two orders of magnitude slower, which is precisely the failure worth
    /// noticing. One line, once, at boot.
    bool log_renderer = true;

    /// Latency policy. `immediate` by default — see LatencyMode.
    LatencyMode latency = LatencyMode::immediate;

    /// Keep running when `update()` or `view()` throws.
    ///
    /// A UI framework should not turn one bad frame into a lost session. With
    /// this on, an exception from application code is caught, reported, and
    /// the runtime falls back to the last good tree — the user sees a stale
    /// frame instead of a vanished window, and their unsaved work survives.
    ///
    /// Off in tests and debug builds, where a crash at the point of failure
    /// is far more useful than a swallowed one.
    bool catch_exceptions = true;

    /// Called when application code throws. Defaults to stderr.
    void (*on_error)(std::string_view where, std::string_view what) = nullptr;
};

// ── the runtime ─────────────────────────────────────────────────────────

namespace detail {

/// Bundles the three seam adapters a FontStack must supply. Held by value so
/// the pointers handed to layout and the painter stay valid for the runtime's
/// whole lifetime.
struct StackBindings {
    typo::StackMeasurer      measurer;
    typo::StackGlyphRenderer glyphs;
    typo::StackSampler       sampler;

    explicit StackBindings(typo::FontStack& s) : measurer{s}, glyphs{s}, sampler{s} {}
};

/// Thread-safe inbox for messages produced off the UI thread — `Cmd::task`,
/// `Cmd::stream`, or a `send()` from a producer the app owns.
///
/// Posting also WAKES the UI thread. Without that, a message from a background
/// thread would sit in the queue until the next window event, because an idle
/// app is blocked in poll() watching only the display fd. The waker's pipe is
/// in that poll set, so post() interrupts the sleep and the frame renders at
/// once. This is the single change that makes a streaming app feel live.
template <typename Msg>
class Inbox {
  public:
    void set_waker(platform::Waker* w) noexcept { waker_ = w; }

    void post(Msg m) {
        {
            std::lock_guard lock{mutex_};
            queue_.push_back(std::move(m));
        }
        if (waker_ != nullptr) waker_->wake();
    }
    [[nodiscard]] std::vector<Msg> drain() {
        std::lock_guard lock{mutex_};
        std::vector<Msg> out;
        out.reserve(queue_.size());
        while (!queue_.empty()) { out.push_back(std::move(queue_.front())); queue_.pop_front(); }
        return out;
    }
    [[nodiscard]] bool empty() {
        std::lock_guard lock{mutex_};
        return queue_.empty();
    }
  private:
    std::mutex          mutex_;
    std::deque<Msg>     queue_;
    platform::Waker*    waker_ = nullptr;
};

/// A timer created by `Cmd::after` or `Sub::every`.
template <typename Msg>
struct Timer {
    double deadline = 0.0;
    double interval = 0.0;   ///< 0 = one-shot
    Msg    msg;
};

}  // namespace detail

/// The runtime, parameterised on the Program and the platform window. The
/// window is a TEMPLATE parameter, not a virtual base: `run<P>()` on macOS
/// compiles direct calls into the Cocoa backend with no indirection, and the
/// headless test build compiles direct calls into the memory framebuffer.
template <Program P, platform::Window W>
class Runtime {
  public:
    using Model = typename P::Model;
    using Msg   = typename P::Msg;

    explicit Runtime(AppConfig cfg) : cfg_{std::move(cfg)} {}

    /// Shut down cleanly: signal every `Cmd::stream` producer to stop and join
    /// its thread. Streams are the one thing the runtime owns that outlives a
    /// frame, so leaving them detached would mean a producer thread touching a
    /// freed inbox after the app returned. `streams_alive_` flips to false,
    /// well-behaved streams see `keep_running()` become false and return, and
    /// this waits for them.
    ~Runtime() {
        stop_all_sources();
        streams_alive_->store(false, std::memory_order_release);
        waker_.wake();   // nudge any stream sleeping on its own condition
        for (auto& t : stream_threads_) {
            if (t.joinable()) t.join();
        }
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Open the window and evaluate `init()`. Separated from `run()` so a
    /// headless driver can boot the runtime and then step it by hand — the
    /// test drives the SAME code the shipped loop drives.
    [[nodiscard]] bool boot() {
        platform::WindowConfig wc{
            .title = cfg_.title, .size = cfg_.size,
            .resizable = cfg_.resizable, .decorated = true,
            .transparent = false, .vsync = cfg_.vsync,
            .background = cfg_.theme.background,
        };

        auto opened = W::open(wc);
        if (!opened) return false;
        window_ = std::move(*opened);

        // Bind the font engine once, at boot. There is exactly one text path
        // now: a FontStack. When the caller gave none, discover the system
        // fonts (with a synthesized last-resort face as the floor), so text
        // renders correctly on a fresh machine with no configuration.
        if (cfg_.fonts == nullptr) {
            cfg_.fonts = typo::system::default_stack();
        }
        bindings_.emplace(*cfg_.fonts);
        measurer_ = &bindings_->measurer;
        glyphs_   = &bindings_->glyphs;
        sampler_  = &bindings_->sampler;
        // The GPU path samples a texture rather than calling back into the
        // CPU sampler, so it needs the atlas itself to upload.
        window_.set_atlas_source(&cfg_.fonts->atlas());
        window_.set_coverage_sampler(sampler_);

        // Wire the cross-thread waker: the Inbox writes to it on post() and the
        // window watches it in poll(), so a background producer wakes an idle
        // UI thread instead of leaving its message unrendered. This is what
        // makes streaming apps live rather than frozen-until-you-move-the-mouse.
        inbox_.set_waker(&waker_);
        window_.set_waker(&waker_);

        // Adopt the platform's multi-click interval. Only used when a backend
        // reports no click count itself, but a wrong fallback is still a bug.
        input_.set_multi_click_interval(window_.double_click_interval());

        size_ = window_.size();
        dpi_  = window_.dpi_scale();

        // Say which path we are on, once, at startup.
        //
        // GPU selection is a deliberate silent fallthrough — no device, no
        // backend in the build, or a shader that will not compile all end up
        // on the software rasteriser rather than failing. That is correct,
        // and it also means a user cannot tell what they got. One line at
        // boot makes "is this actually using the GPU" answerable instead of
        // assumed, and makes a silent fallback visible the day it happens.
        if (cfg_.log_renderer) {
            std::fprintf(stderr, "mayag: renderer %.*s\n",
                         static_cast<int>(window_.renderer_name().size()),
                         window_.renderer_name().data());
        }

        // ---- pure: initial state -------------------------------------
        Cmd<Msg> boot_cmd = Cmd<Msg>::none();
        if constexpr (detail::HasEffectfulInit<P>) {
            auto [m, c] = P::init();
            model_   = std::move(m);
            boot_cmd = std::move(c);
        } else {
            model_ = P::init();
        }
        interpret(boot_cmd);

        // Paint frame zero, so a driver can assert on pixels before sending
        // any input.
        render();
        return true;
    }

    /// Run to completion. Returns a process exit code.
    int run() {
        if (!boot()) return 1;
        while (window_.is_open() && !quit_) {
            tick();
        }
        return 0;
    }

    /// One iteration: wait, translate, update, render, present. Public so a
    /// test can drive it step by step and assert between frames.
    void tick() {
        const auto subs = current_subs();

        // Reconcile declarative streams (`Sub::source`): start producers that
        // just appeared, stop and join ones the model dropped. Done every tick
        // because the subscription set is a pure function of the model, so a
        // source vanishes the frame its condition goes false — no manual
        // connect/disconnect in `update`.
        sync_sources(subs);

        // How to wait is DERIVED from the subscriptions, not from a flag.
        // No frame subscription and no timer means block — and a blocked app
        // costs nothing.
        auto [wait, timeout] = decide_wait(subs);
        auto events = window_.poll_events(wait, timeout);

        // Take the clock from the WINDOW, every tick.
        //
        // Reading it only from FrameEvent (as an earlier version did) leaves
        // `time_` pinned at 0 for any app that is not animating — and the
        // interaction layer uses this timestamp for double-click detection,
        // so every click looked simultaneous with the previous one and every
        // second click was silently eaten as the tail of a double. A
        // stationary clock is never harmless.
        const double now = window_.now();
        time_ = now;

        fire_due_timers(now);

        // ── coalesce input ──────────────────────────────────────────────
        //
        // Every pending event is processed before ONE render, and redundant
        // pointer motion is collapsed to the newest position.
        //
        // A trackpad delivers ~120 moves/second. Rendering each is 120 frames
        // of work for 60 frames of visible result — and it makes the app FEEL
        // slower, because the frame you finally see is several events stale.
        // The correct behaviour is to render the LATEST state once, which is
        // both less work and lower latency.
        frame_.events_coalesced = static_cast<int>(events.size());

        std::size_t last_move = events.size();
        for (std::size_t i = events.size(); i-- > 0;) {
            if (std::holds_alternative<MouseMove>(events[i])) { last_move = i; break; }
        }

        {
            ScopedTimer t{frame_.input_ms};
            for (std::size_t i = 0; i < events.size(); ++i) {
                // Superseded motion changes nothing observable: only the most
                // recent cursor position can affect hover, and a drag reads
                // the accumulated delta from the position anyway.
                if (std::holds_alternative<MouseMove>(events[i]) && i != last_move) {
                    ++frame_.coalesced_moves;
                    continue;
                }
                handle_event(events[i], subs);
                if (quit_) return;
            }
        }

        // Advance tracked motion. The runtime owns this so an app never has
        // to remember to step it, and — crucially — never has to remember to
        // STOP: when every spring settles, the frame request stops with it.
        if (tracked_.animating()) {
            const double dt = last_motion_time_ > 0.0
                ? num::clamp(now - last_motion_time_, 0.0, 0.1) : 1.0 / 60.0;
            tracked_.step_all(dt);
            dirty_ = true;
        }
        last_motion_time_ = now;

        // Messages that arrived from worker threads.
        for (auto& m : inbox_.drain()) {
            step(std::move(m));
            if (quit_) return;
        }

        render();
    }

    // ── test/driver access ──────────────────────────────────────────────

    [[nodiscard]] W& window() noexcept { return window_; }
    [[nodiscard]] const Model& model() const noexcept { return model_; }

    /// Which backend is actually presenting: "metal" or "software".
    ///
    /// Worth surfacing rather than leaving implicit, because GPU selection
    /// is a silent fallthrough by design — a machine with no device, or a
    /// build without the backend, degrades to software instead of failing.
    /// That is the right behaviour and also the reason "it uses the GPU" is
    /// unverifiable without asking. An app that cares can print this; the
    /// examples do.
    [[nodiscard]] std::string_view renderer_name() const noexcept {
        return window_.renderer_name();
    }
    [[nodiscard]] const Node& tree() const noexcept { return tree_; }
    [[nodiscard]] const Interaction& input() const noexcept { return input_; }

    /// Where this frame's time went. Latency is a number, not a claim — an
    /// app can print this, draw it, or assert on it in CI.
    [[nodiscard]] const LatencyStats& latency() const noexcept { return latency_; }
    [[nodiscard]] bool finished() const noexcept { return quit_ || !window_.is_open(); }

    /// Rect of a named node in the CURRENT tree, or nullopt if it is not in
    /// the view right now.
    ///
    /// Always ask through this rather than holding a `const Node*`: every
    /// render replaces the tree wholesale (that is what makes `view()` pure),
    /// so a pointer captured before a `tick()` dangles afterwards. Returning
    /// a Rect by value makes that mistake unrepresentable.
    [[nodiscard]] std::optional<Rect> node_rect(std::uint64_t id) const {
        if (const Node* n = tree_.find(id)) return n->frame();
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Rect> node_rect(std::string_view name) const {
        return node_rect(dsl::node_id(name));
    }

    /// Centre of a named node — where a test clicks.
    [[nodiscard]] Vec2 center_of(std::string_view name) const {
        if (auto r = node_rect(name)) return r->center();
        return Vec2{-1.0f, -1.0f};   // off-screen: a click there hits nothing
    }

    /// Inject a message directly — how a test exercises `update()` through
    /// the real runtime rather than in isolation.
    void send(Msg m) { step(std::move(m)); }

    /// Click a named widget: move, press, release, then process. The whole
    /// interaction in one call, because a test that hand-rolls the three
    /// events tends to get the ordering subtly wrong.
    void click(std::string_view name) {
        window_.click(center_of(name));
        tick();
    }

  private:
    // ── the pure step ───────────────────────────────────────────────────

    /// Model' = update(Model, Msg); then interpret the returned effects.
    /// This function is the ONLY place the model changes.
    void step(Msg msg) {
        if (!cfg_.catch_exceptions) {
            auto [next, cmd] = P::update(std::move(model_), std::move(msg));
            model_ = std::move(next);
            dirty_ = true;
            interpret(cmd);
            return;
        }

        // A throwing update must not take the app down.
        //
        // `update` takes the model BY VALUE, so a throw leaves `model_` in a
        // moved-from state — which is exactly why the copy below exists. The
        // model is restored and the app carries on with the last good state,
        // having lost one message rather than the whole session.
        Model snapshot = model_;
        try {
            auto [next, cmd] = P::update(std::move(model_), std::move(msg));
            model_ = std::move(next);
            dirty_ = true;
            interpret(cmd);
        } catch (const std::exception& e) {
            model_ = std::move(snapshot);
            report_error("update", e.what());
        } catch (...) {
            model_ = std::move(snapshot);
            report_error("update", "unknown exception");
        }
    }

    void report_error(std::string_view where, std::string_view what) {
        if (cfg_.on_error != nullptr) {
            cfg_.on_error(where, what);
        } else {
            std::fprintf(stderr, "mayag: %.*s threw: %.*s\n",
                         static_cast<int>(where.size()), where.data(),
                         static_cast<int>(what.size()), what.data());
        }
    }

    // ── event -> message ────────────────────────────────────────────────

    void handle_event(const Event& ev, const Sub<Msg>& subs) {
        // Window-level events the runtime owns outright.
        if (const auto* r = std::get_if<ResizeEvent>(&ev)) {
            size_ = r->size;
            dpi_  = r->dpi_scale;
            dirty_ = true;
        }
        if (const auto* f = std::get_if<FrameEvent>(&ev)) {
            time_ = f->time;
        }

        // Tab moves focus. Doing this in the runtime means every mayag app
        // gets keyboard navigation for free instead of each one reinventing it.
        if (const auto* k = std::get_if<KeyEvent>(&ev)) {
            if (k->key == Key::tab && !k->mods.ctrl && !k->mods.super) {
                input_.focus_next(tree_, k->mods.shift);
                dirty_ = true;
            }

            // Enter and Space activate the focused control, exactly as a
            // click would. Every mayag app is keyboard-navigable for free;
            // without this each author writes a parallel key handler per
            // button and most simply do not.
            //
            // The key is CONSUMED when it actually activated something.
            // Otherwise Space would both activate the focused button and fall
            // through to the app's own Space binding — which is how this
            // broke the typography example the moment it was added.
            if ((k->key == Key::enter || k->key == Key::space) &&
                input_.focused() != 0 && k->mods.none()) {
                const Node* n = tree_.find(input_.focused());
                std::vector<Gesture> g{Gesture{Gesture::Kind::activate, input_.focused(),
                                               n ? n->frame().center() : Vec2{}}};

                std::vector<Msg> produced;
                match(subs, ev, g, produced);
                if (!produced.empty()) {
                    for (auto& m : produced) {
                        step(std::move(m));
                        if (quit_) return;
                    }
                    dirty_ = true;
                    return;   // consumed: no fall-through to plain key handlers
                }
            }
        }

        // ── overlays get first refusal on pointer input ─────────────────
        //
        // A modal must swallow clicks aimed at what is behind it, and a click
        // outside a menu should DISMISS it rather than activating whatever
        // happens to be under the cursor. Both are the same rule: the overlay
        // layer sees the event first and may consume it.
        if (!overlays_.empty()) {
            if (const auto pos = event_position(ev)) {
                const bool is_press = std::holds_alternative<MouseDown>(ev);

                if (const Overlay* over = overlays_.hit(*pos)) {
                    // Inside an overlay: route the gesture against the OVERLAY
                    // tree, not the main one.
                    const auto gestures = input_.handle(ev, over->content, time_);
                    if (!gestures.empty()) dirty_ = true;
                    collect_and_step(subs, ev, gestures);
                    return;
                }

                if (is_press) {
                    const auto to_dismiss = overlays_.dismissed_by(*pos);
                    if (!to_dismiss.empty()) {
                        for (std::uint64_t id : to_dismiss) {
                            const auto gestures = std::vector<Gesture>{
                                Gesture{Gesture::Kind::leave, id, *pos}};
                            collect_and_step(subs, ev, gestures);
                        }
                        dirty_ = true;
                        return;   // the click is CONSUMED by the dismissal
                    }
                    if (overlays_.captures_input()) return;   // modal blocks
                }
            }
        }

        // Update the pointer shape from whatever is under the cursor.
        //
        // Walked from the hit node UPWARD, taking the first node that asks
        // for something specific: a label inside a button must not reset the
        // cursor the button set.
        if (std::holds_alternative<MouseMove>(ev)) {
            const CursorShape want = cursor_at(tree_, input_.cursor());
            if (want != current_cursor_) {
                current_cursor_ = want;
                window_.set_cursor(want);
            }
        }

        // Pointer events become semantic gestures against the CURRENT tree.
        //
        // Only repaint when the INTERACTION STATE actually changed. macOS
        // delivers a mouse-moved event for every pixel of cursor travel — 117
        // of them across 200 polls in a measurement — and treating each as a
        // reason to redraw turns idle mouse movement into a 650 fps spin that
        // pins a core. What matters is whether the hovered/pressed/focused
        // node changed, because that is all a view can observe through Ctx.
        const std::uint64_t before_hover   = input_.hovered();
        const std::uint64_t before_pressed = input_.pressed();
        const std::uint64_t before_focus   = input_.focused();

        const auto gestures = input_.handle(ev, tree_, time_);

        if (input_.hovered() != before_hover ||
            input_.pressed() != before_pressed ||
            input_.focused() != before_focus) {
            dirty_ = true;
        }

        // Dispatch: raw event subscriptions first, then gesture ones.
        collect_and_step(subs, ev, gestures);
    }

    void collect_and_step(const Sub<Msg>& sub, const Event& ev,
                          const std::vector<Gesture>& gestures) {
        std::vector<Msg> produced;
        match(sub, ev, gestures, produced);
        for (auto& m : produced) {
            step(std::move(m));
            if (quit_) return;
        }
    }

    /// Walk the subscription tree, emitting a Msg for every match.
    void match(const Sub<Msg>& sub, const Event& ev,
               const std::vector<Gesture>& gestures, std::vector<Msg>& out) {
        using S = Sub<Msg>;
        std::visit([&](const auto& a) {
            using T = std::decay_t<decltype(a)>;

            if constexpr (std::is_same_v<T, typename S::Batch>) {
                for (const auto& s : a.subs) match(s, ev, gestures, out);
            }
            else if constexpr (std::is_same_v<T, typename S::OnEvent>) {
                if (auto m = a.handler(ev)) out.push_back(std::move(*m));
            }
            else if constexpr (std::is_same_v<T, typename S::OnKey>) {
                if (const auto* k = std::get_if<KeyEvent>(&ev)) {
                    if (k->key == a.key && k->mods == a.mods) out.push_back(a.msg);
                }
            }
            else if constexpr (std::is_same_v<T, typename S::OnAnyKey>) {
                if (const auto* k = std::get_if<KeyEvent>(&ev)) {
                    if (auto m = a.handler(*k)) out.push_back(std::move(*m));
                }
            }
            else if constexpr (std::is_same_v<T, typename S::OnText>) {
                if (const auto* t = std::get_if<TextEvent>(&ev)) {
                    if (auto m = a.handler(t->text)) out.push_back(std::move(*m));
                }
            }
            else if constexpr (std::is_same_v<T, typename S::OnCompose>) {
                if (const auto* ce = std::get_if<ComposeEvent>(&ev)) {
                    if (auto m = a.handler(*ce)) out.push_back(std::move(*m));
                }
            }
            else if constexpr (std::is_same_v<T, typename S::OnComposeEnd>) {
                if (std::holds_alternative<ComposeEndEvent>(ev)) out.push_back(a.msg);
            }
            else if constexpr (std::is_same_v<T, typename S::EveryFrame>) {
                if (const auto* f = std::get_if<FrameEvent>(&ev)) out.push_back(a.handler(*f));
            }
            else if constexpr (std::is_same_v<T, typename S::OnResize>) {
                if (const auto* r = std::get_if<ResizeEvent>(&ev)) out.push_back(a.handler(r->size));
            }
            else if constexpr (std::is_same_v<T, typename S::OnClose>) {
                if (std::holds_alternative<CloseRequest>(ev)) out.push_back(a.msg);
            }
            else if constexpr (std::is_same_v<T, typename S::OnFocusChange>) {
                if (const auto* f = std::get_if<FocusEvent>(&ev)) out.push_back(a.handler(f->focused));
            }
            else if constexpr (std::is_same_v<T, typename S::OnNode>) {
                for (const auto& g : gestures) {
                    // The click COUNT is part of the match, so a double click
                    // does not also fire a single-click handler.
                    if (g.node_id == a.node_id && matches(g.kind, a.gesture) &&
                        (g.kind != Gesture::Kind::click || a.accepts(g.click_count))) {
                        out.push_back(a.msg);
                    }
                }
            }
            else if constexpr (std::is_same_v<T, typename S::OnNodeMotion>) {
                for (const auto& g : gestures) {
                    if (g.node_id != a.node_id || !matches(g.kind, a.gesture)) continue;
                    // Drags report LOCAL position (what a slider wants);
                    // scrolls report the delta (what a scroll view wants).
                    out.push_back(a.handler(g.kind == Gesture::Kind::scroll ? g.delta : g.local));
                }
            }
        }, sub.alternative());
    }

    [[nodiscard]] static bool matches(Gesture::Kind k, typename Sub<Msg>::Gesture want) {
        using G = typename Sub<Msg>::Gesture;
        switch (want) {
            case G::click:        return k == Gesture::Kind::click;
            case G::press:        return k == Gesture::Kind::press;
            case G::release:      return k == Gesture::Kind::release;
            case G::enter:        return k == Gesture::Kind::enter;
            case G::leave:        return k == Gesture::Kind::leave;
            case G::drag:         return k == Gesture::Kind::drag;
            case G::scroll:       return k == Gesture::Kind::scroll;
            case G::activate:     return k == Gesture::Kind::activate;
        }
        return false;
    }

    // ── effect interpretation ───────────────────────────────────────────

    /// The one place effects actually happen. Everything above this line is
    /// pure; everything below is I/O.
    void interpret(const Cmd<Msg>& cmd) {
        using C = Cmd<Msg>;
        std::visit([&](const auto& a) {
            using T = std::decay_t<decltype(a)>;

            if constexpr (std::is_same_v<T, typename C::None>) {}
            else if constexpr (std::is_same_v<T, typename C::Quit>) { quit_ = true; }
            else if constexpr (std::is_same_v<T, typename C::Redraw>) { dirty_ = true; }
            else if constexpr (std::is_same_v<T, typename C::Batch>) {
                for (const auto& c : a.cmds) interpret(c);
            }
            else if constexpr (std::is_same_v<T, typename C::After>) {
                const double secs = std::chrono::duration<double>(a.delay).count();
                timers_.push_back(detail::Timer<Msg>{window_.now() + secs, 0.0, a.msg});
            }
            else if constexpr (std::is_same_v<T, typename C::Task>) {
                // Detached worker; its result lands in the inbox and is
                // consumed by the UI thread next tick. The Program never
                // learns a thread was involved.
                auto* inbox = &inbox_;
                std::thread([work = a.work, inbox] {
                    inbox->post(work());
                }).detach();
            }
            else if constexpr (std::is_same_v<T, typename C::Stream>) {
                // A long-lived producer. It runs on its own thread and pushes
                // messages through a sink backed by the inbox+waker, so each
                // one wakes the UI thread and renders at once. The thread
                // outlives this call by design; the runtime signals it to stop
                // via `streams_alive_` at shutdown and joins it in the
                // destructor, so no detached thread survives the app.
                auto* inbox = &inbox_;
                auto  alive = streams_alive_;   // shared_ptr<atomic<bool>>
                typename C::Sink sink = [inbox](Msg m) { inbox->post(std::move(m)); };
                stream_threads_.emplace_back(
                    [work = a.work, sink = std::move(sink), alive] {
                        work(sink, [alive] { return alive->load(std::memory_order_acquire); });
                    });
            }
            else if constexpr (std::is_same_v<T, typename C::Perform>) { a.action(); }
            else if constexpr (std::is_same_v<T, typename C::SetTitle>) {
                window_.set_title(a.title);
            }
            else if constexpr (std::is_same_v<T, typename C::SetCursor>) {
                window_.set_cursor(static_cast<CursorShape>(a.shape));
            }
            else if constexpr (std::is_same_v<T, typename C::SetClipboard>) {
                window_.set_clipboard(a.text);
            }
            else if constexpr (std::is_same_v<T, typename C::ReadClipboard>) {
                inbox_.post(a.then(window_.get_clipboard()));
            }
            else if constexpr (std::is_same_v<T, typename C::Focus>) {
                input_.set_focus(a.node_id);
                dirty_ = true;
            }
            else if constexpr (std::is_same_v<T, typename C::Screenshot>) {
                const auto px = window_.read_pixels();
                (void)image::write_png(a.path, px,
                                       static_cast<int>(size_.x * dpi_),
                                       static_cast<int>(size_.y * dpi_));
            }
        }, cmd.alternative());
    }

    // ── timers ──────────────────────────────────────────────────────────

    void fire_due_timers(double now) {
        // Re-arm interval timers by ADVANCING the deadline rather than
        // resetting it to now+interval, so a slow frame does not make a
        // 60 Hz timer drift into a 55 Hz one.
        std::vector<Msg> due;
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (now >= it->deadline) {
                due.push_back(it->msg);
                if (it->interval > 0.0) {
                    it->deadline += it->interval;
                    ++it;
                } else {
                    it = timers_.erase(it);
                }
            } else {
                ++it;
            }
        }
        for (auto& m : due) {
            step(std::move(m));
            if (quit_) return;
        }
    }

    // ── declarative streams (Sub::source) ───────────────────────────────

    /// A producer thread started by a `Sub::source`, keyed by its id.
    struct RunningSource {
        std::string                        id;
        std::shared_ptr<std::atomic<bool>> alive;
        std::thread                        thread;
    };

    /// Diff the model's sources against the running ones. New ids start a
    /// producer; ids that vanished are signalled to stop and joined. This is
    /// the same declarative contract as interval timers, but for threads: the
    /// live set is a pure function of the model, so there is no stop() to
    /// forget and no thread leaked when a condition goes false.
    void sync_sources(const Sub<Msg>& subs) {
        using S = Sub<Msg>;

        // Gather (id, work) pairs the model wants this frame.
        std::vector<const typename S::Source*> wanted;
        collect_sources(subs, wanted);

        // Stop any running source whose id is no longer wanted.
        for (auto it = sources_.begin(); it != sources_.end();) {
            const bool still = std::any_of(wanted.begin(), wanted.end(),
                [&](const typename S::Source* w) { return w->id == it->id; });
            if (still) { ++it; continue; }
            it->alive->store(false, std::memory_order_release);
            waker_.wake();                      // in case it sleeps on nothing
            if (it->thread.joinable()) it->thread.join();
            it = sources_.erase(it);
        }

        // Start any wanted source not already running.
        for (const typename S::Source* w : wanted) {
            const bool running = std::any_of(sources_.begin(), sources_.end(),
                [&](const RunningSource& r) { return r.id == w->id; });
            if (running || w->work == nullptr) continue;

            auto alive = std::make_shared<std::atomic<bool>>(true);
            auto* inbox = &inbox_;
            auto  work  = w->work;   // shared_ptr keeps the callable alive
            std::thread th([work, inbox, alive] {
                (*work)([inbox](Msg m) { inbox->post(std::move(m)); },
                        [alive] { return alive->load(std::memory_order_acquire); });
            });
            sources_.push_back(RunningSource{std::string{w->id}, std::move(alive),
                                            std::move(th)});
        }
    }

    void collect_sources(const Sub<Msg>& sub,
                         std::vector<const typename Sub<Msg>::Source*>& out) {
        using S = Sub<Msg>;
        std::visit([&](const auto& a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, typename S::Batch>) {
                for (const auto& s : a.subs) collect_sources(s, out);
            } else if constexpr (std::is_same_v<T, typename S::Source>) {
                out.push_back(&a);
            }
        }, sub.alternative());
    }

    /// Stop and join every running source. Called from the destructor so no
    /// producer thread outlives the runtime and touches a freed inbox.
    void stop_all_sources() {
        for (auto& s : sources_) {
            s.alive->store(false, std::memory_order_release);
        }
        waker_.wake();
        for (auto& s : sources_) {
            if (s.thread.joinable()) s.thread.join();
        }
        sources_.clear();
    }

    void sync_interval_timers(const Sub<Msg>& subs) {
        // Interval subscriptions are declarative: the set of live timers is
        // recomputed from the model each frame, so a timer whose condition
        // went false simply stops existing. No stop_timer() to forget.
        std::vector<detail::Timer<Msg>> wanted;
        collect_intervals(subs, wanted);

        // Preserve the phase of timers that already existed.
        for (auto& w : wanted) {
            for (const auto& old : interval_timers_) {
                if (old.interval == w.interval) { w.deadline = old.deadline; break; }
            }
        }
        interval_timers_ = std::move(wanted);
    }

    void collect_intervals(const Sub<Msg>& sub, std::vector<detail::Timer<Msg>>& out) {
        using S = Sub<Msg>;
        std::visit([&](const auto& a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, typename S::Batch>) {
                for (const auto& s : a.subs) collect_intervals(s, out);
            } else if constexpr (std::is_same_v<T, typename S::Every>) {
                const double secs = std::chrono::duration<double>(a.interval).count();
                out.push_back(detail::Timer<Msg>{window_.now() + secs, secs, a.msg});
            }
        }, sub.alternative());
    }

    // ── waiting ─────────────────────────────────────────────────────────

    [[nodiscard]] std::pair<platform::Wait, double> decide_wait(const Sub<Msg>& subs) const {
        // NEVER block while a repaint is owed.
        //
        // The loop is wait -> handle events -> update -> render. If a message
        // arrives from anywhere other than the window (a worker thread's
        // Cmd::task result, a timer, a `send()` from application code) the
        // model changes and `dirty_` is set — but the NEXT thing the loop does
        // is block for a window event that may never come. The new frame sits
        // unrendered and the app looks frozen until the user happens to move
        // the mouse.
        //
        // That is exactly the "pressed a theme chip and it hung" report: the
        // click was handled, the model updated, and then the loop went to
        // sleep still owing the screen a paint.
        if (dirty_) return {platform::Wait::immediate, 0.0};

        // Motion in progress needs frames, exactly like an `every_frame`
        // subscription. Folding it into the SAME decision is what keeps
        // animation from needing a second clock — and what guarantees the app
        // goes back to sleep the moment the last spring settles.
        if (tracked_.animating() || motion_pending_) return {platform::Wait::poll, 0.0};

        if (subs.wants_frames()) return {platform::Wait::poll, 0.0};
        if (auto i = subs.min_interval()) {
            return {platform::Wait::timeout, std::chrono::duration<double>(*i).count()};
        }
        if (!timers_.empty()) {
            double soonest = timers_.front().deadline;
            for (const auto& t : timers_) soonest = std::min(soonest, t.deadline);
            return {platform::Wait::timeout, std::max(soonest - time_, 0.0)};
        }
        return {platform::Wait::block, 0.0};
    }

    /// Deepest node under `p` that requests a specific cursor.
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

    [[nodiscard]] Sub<Msg> current_subs() const {
        if constexpr (detail::HasSubscribe<P>) return P::subscribe(model_);
        else return Sub<Msg>::none();
    }

    // ── the frame ───────────────────────────────────────────────────────

    void render() {
        // view -> layout -> paint -> present. Skipped entirely when nothing
        // changed, which is what keeps an idle window at zero GPU load.
        if (!dirty_) { busy_.idle(); return; }
        dirty_ = false;

        size_ = window_.size();
        dpi_  = window_.dpi_scale();

        if (cfg_.fonts != nullptr) cfg_.fonts->begin_frame(frame_counter_);
        ++frame_counter_;
        frame_.view_ms = frame_.layout_ms = frame_.paint_ms = frame_.render_ms = 0.0;

        // Clear BEFORE the view runs, or we erase the overlays it just
        // posted. The list then outlives this frame: input arrives between
        // renders, and an emptied list would mean a menu visible on screen
        // yet invisible to hit testing.
        overlays_.clear();
        tracked_.clear();
        clear_animation_request();

        Ctx ctx{size_, dpi_, time_, cfg_.theme, &latency_, measurer_, &tracked_, &overlays_, &input_};

        // A throwing view keeps the LAST GOOD TREE on screen. A blank window
        // tells the user nothing; a stale frame at least shows what was there
        // and leaves the app usable enough to save.
        if (cfg_.catch_exceptions) {
            try {
                if constexpr (detail::ViewWithCtx<P>) tree_ = P::view(model_, ctx);
                else                                  tree_ = P::view(model_);
            } catch (const std::exception& e) {
                report_error("view", e.what());
                if (tree_.children().empty()) return;   // nothing good to fall back to
            } catch (...) {
                report_error("view", "unknown exception");
                if (tree_.children().empty()) return;
            }
        } else {
            if constexpr (detail::ViewWithCtx<P>) tree_ = P::view(model_, ctx);
            else                                  tree_ = P::view(model_);
        }

        { ScopedTimer t{frame_.layout_ms}; layout::layout_tree(tree_, size_, *measurer_); }

        // Tracked geometry is resolved AFTER layout, for the same reason
        // overlays are: a node's real rect is not known until the pass ends.
        // This is what lets an animation follow a LAYOUT rather than a
        // hardcoded coordinate.
        tracked_.observe_all(tree_);

        // Overlays are laid out AFTER the main tree, because an overlay
        // positions itself against an anchor whose final rect is not known
        // until layout finishes. That ordering is what makes "attach a menu
        // to this button" work regardless of how deeply the button is nested.
        for (auto& o : overlays_.items()) {
            const Rect anchor = o.anchor_id != 0
                ? (tree_.find(o.anchor_id) ? tree_.find(o.anchor_id)->frame() : Rect{})
                : Rect{Vec2{}, size_};

            // Measure at natural size against the viewport, then place.
            const Vec2 natural = layout::measure(
                o.content, layout::Constraints{size_.x, size_.y}, *measurer_);

            const Rect placed = overlay::resolve(o.placement, anchor, natural, size_,
                                                 o.offset, input_.cursor());
            layout::arrange(o.content, placed, *measurer_);
        }

        // With `--debug`, audit every frame whose tree actually changed and
        // report new faults once. Layout bugs are cheap to fix and expensive
        // to notice, so surfacing them during development is worth more than
        // the microseconds it costs.
        if (cfg_.debug_bounds) {
            const auto issues = layout::audit(tree_, measurer_);
            if (issues.size() != last_issue_count_) {
                last_issue_count_ = issues.size();
                std::fputs(layout::format_issues(issues).c_str(), stderr);
            }
        }

        draws_.clear();
        ScopedTimer paint_timer{frame_.paint_ms};
        render::PaintOptions po{};
        po.dpi_scale    = dpi_;
        po.measurer     = measurer_;
        po.glyphs       = glyphs_;
        po.debug_bounds = cfg_.debug_bounds;
        render::paint(tree_, draws_, po);

        // Overlays paint last, outside every clip, in layer order — which is
        // exactly the three things a menu needs and normal flow cannot give.
        if (!overlays_.empty()) {
            auto sorted = overlays_.items();
            std::stable_sort(sorted.begin(), sorted.end(),
                             [](const Overlay& a, const Overlay& b) { return a.layer < b.layer; });
            for (const auto& o : sorted) {
                if (o.scrim > 0.0f) {
                    draws_.fill_rect(Rect{Vec2{}, size_ * dpi_},
                                     colors::black.fade(o.scrim));
                }
                render::paint(o.content, draws_, po);
            }
        }

        // A widget mid-motion called request_animation_frame() during the
        // view pass. Collected here and folded into the next wait decision.
        motion_pending_ = animation_was_requested() || tracked_.animating();

        frame_.instances  = static_cast<std::uint32_t>(draws_.size());
        frame_.draw_calls = static_cast<std::uint32_t>(draws_.batches().size());

        {
            ScopedTimer t{frame_.render_ms};
            window_.present(draws_, cfg_.theme.background);
        }

        latency_.record(frame_);
        frame_ = FrameTiming{};

        // Warn ONCE about an app that never stops rendering.
        //
        // A continuously-rendering app is not technically broken — the window
        // responds, events flow — but it pins a core and users reasonably
        // report it as "hung". It is far harder to notice than a deadlock,
        // because everything keeps working, so it gets a detector rather than
        // an assertion. mayag's own reactive demo shipped this way.
        busy_.frame_presented(time_);
        if (busy_.busy() && !busy_warned_) {
            busy_warned_ = true;
            std::fprintf(stderr,
                "mayag: rendering continuously at %.0f fps for several seconds.\n"
                "       If this is not a game or a visualiser, something is "
                "requesting frames that should not be:\n"
                "         * a Sub::every_frame whose condition is always true\n"
                "         * an Animated<T> that never settles\n"
                "         * a view that marks itself dirty every frame\n"
                "       An idle mayag app should sit at 0%% CPU.\n",
                busy_.frame_rate());
        }
    }

    AppConfig    cfg_;
    W            window_{};
    Model        model_{};
    Node         tree_{};
    DrawList     draws_{};
    Interaction  input_{};

    // Font binding, resolved once at boot. `bindings_` owns the seam adapters
    // that layout and the painter read through; `cfg_.fonts` owns the stack.
    std::optional<detail::StackBindings> bindings_;
    const layout::TextMeasurer*          measurer_ = nullptr;
    const render::GlyphRenderer*         glyphs_   = nullptr;
    const backend::CoverageSampler*      sampler_  = nullptr;
    std::uint64_t                        frame_counter_ = 0;
    OverlayList                          overlays_;
    TrackedSet                           tracked_;
    bool                                 motion_pending_ = false;
    double                               last_motion_time_ = 0.0;
    CursorShape                          current_cursor_ = CursorShape::arrow;
    FrameTiming                          frame_{};
    LatencyStats                         latency_{};
    BusyLoopDetector                     busy_{};
    bool                                 busy_warned_ = false;
    std::size_t                          last_issue_count_ = static_cast<std::size_t>(-1);

    detail::Inbox<Msg>              inbox_;
    platform::Waker                 waker_;
    // Streaming producers (`Cmd::stream`). `streams_alive_` is shared with the
    // worker threads as their shutdown signal; the runtime joins them in its
    // destructor. A shared_ptr rather than a bare member so a thread that
    // checks it during teardown never races the Runtime's own destruction.
    std::shared_ptr<std::atomic<bool>> streams_alive_ =
        std::make_shared<std::atomic<bool>>(true);
    std::vector<std::thread>        stream_threads_;
    std::vector<RunningSource>       sources_;
    std::vector<detail::Timer<Msg>> timers_;
    std::vector<detail::Timer<Msg>> interval_timers_;

    Vec2   size_{1024, 640};
    float  dpi_  = 1.0f;
    double time_ = 0.0;
    bool   dirty_ = true;
    bool   quit_  = false;
};

// ── entry points ────────────────────────────────────────────────────────

/// Run a Program on the native window. This is `main()` for a mayag app.
template <Program P>
int run(AppConfig cfg = {}) {
    Runtime<P, platform::Native> rt{std::move(cfg)};
    return rt.run();
}

/// Run a Program headlessly, with a driver that scripts input and inspects
/// output. The SAME runtime, so a test exercises real code:
///
///     run_headless<Counter>({}, [](auto& rt) {
///         rt.window().click({100, 40});
///         rt.tick();
///         assert(rt.model().n == 1);
///     });
template <Program P, typename Driver>
int run_headless(AppConfig cfg, Driver&& drive) {
    // Headless is always software and always in CI, so the renderer banner
    // would be noise on every test line.
    cfg.log_renderer = false;
    Runtime<P, platform::Headless> rt{std::move(cfg)};
    if (!rt.boot()) return 1;
    drive(rt);
    return 0;
}

}  // namespace mayag
