#pragma once
#include "../core/target.hpp"
#include "../core/diag.hpp"
// mayag::run<P>() — run a program in a window.
//
// jaal is the runtime: the loop, update, Cmd, Sub, timers, tasks, streams,
// faults and the OS waiting. mayag draws. A program is an ordinary jaal
// program with a view():
//
//     struct Counter {
//         struct Model { int n = 0; };
//         struct Inc {}; struct Quit {};
//         using Msg = std::variant<Inc, Quit>;
//         using Cmd = jaal::Cmd<Msg>;
//
//         static Cmd update(Model& m, Inc)  { ++m.n; return {}; }
//         static Cmd update(Model&,   Quit) { return Cmd::quit(); }
//
//         static Node view(const Model& m, const Ctx& c);
//         static std::optional<Msg> on(const Model&, const Clicked& c) {
//             if (is(c, "inc")) return Inc{};
//             return std::nullopt;
//         }
//         static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }
//     };
//     int main() { return mayag::run<Counter>({.title = "counter"}); }
//
// The window host (host.hpp) owns the window and the presentation state;
// jaal owns everything that changes the model. run_headless<P>() drives the
// same host and the same jaal kernel without a window, for tests.

#include "effects.hpp"
#include "../core/event.hpp"
#include "host.hpp"
#include "interaction.hpp"
#include "latency.hpp"
#include "platform.hpp"
#include "../platform/priority.hpp"
#include "sources.hpp"

#include <jaal/jaal.hpp>

// Cocoa owns the main thread's run loop: the app is driven from inside it
// (platform/macos_loop.hpp). Every other window system is polled by jaal.
#if MAYAG_WINDOW_COCOA
#  include "../platform/macos_loop.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mayag {

namespace detail {

[[nodiscard]] inline platform::WindowConfig window_config(const AppConfig& cfg) {
    return platform::WindowConfig{
        .title = cfg.title, .size = cfg.size,
        .resizable = cfg.resizable, .decorated = cfg.decorated,
        .transparent = cfg.transparent, .vsync = cfg.vsync,
        .background = cfg.theme.background, .option_as_alt = cfg.option_as_alt,
    };
}

/// MAYAG_VSYNC=0/1 overrides AppConfig::vsync for any app (latency A/B).
inline void apply_env_overrides(AppConfig& cfg) {
    if (const char* v = std::getenv("MAYAG_VSYNC"); v != nullptr && *v != '\0') cfg.vsync = *v != '0';
}

[[nodiscard]] inline jaal::kernel::options kernel_options(const AppConfig& cfg) {
    jaal::kernel::options o;
    o.on_fault = cfg.catch_exceptions ? jaal::fault_policy::skip : jaal::fault_policy::stop;
    if (cfg.on_error) {
        o.faults = [f = cfg.on_error](const jaal::fault& e) { f(jaal::to_string(e.site), e.what); };
    }
    return o;
}

}  // namespace detail

/// A fresh model and its first Cmd, as the runtime would start P.
template <Program P>
[[nodiscard]] std::pair<typename P::Model, typename P::Cmd> init_of() {
    return jaal::prog::init<P>();
}

/// Fold one message into the model, as the runtime would; returns its Cmd.
template <Program P>
[[nodiscard]] typename P::Cmd update_of(typename P::Model& m, typename P::Msg msg) {
    return jaal::prog::update<P>(m, std::move(msg));
}

/// Run P on the native window. This is main() for a mayag app.
template <Program P>
int run(AppConfig cfg = {}, jaal::prog::flags_t<P> flags = {});

/// Run P on a window the caller opened and keeps.
template <Program P, class W>
int run_on(W& window, AppConfig cfg = {}, jaal::prog::flags_t<P> flags = {});

// ── headless driving ────────────────────────────────────────────────────

/// A program on the headless window, stepped by hand. The SAME window host
/// and the same jaal kernel run<P>() uses; only the waiting is replaced by
/// explicit tick() calls, so a test can assert between frames.
///
///     run_headless<Counter>(cfg, [](auto& app) {
///         app.click("inc");
///         assert(app.model().n == 1);
///     });
template <Program P>
class headless_app {
  public:
    using host_type   = window_host<P, platform::Headless>;
    using event_type  = jaal::kernel_event_t<host_type>;
    using kernel_type = jaal::kernel::kernel<P, event_type, jaal::platform::steady_clock>;
    using model_type  = typename P::Model;
    using msg_type    = typename P::Msg;

    explicit headless_app(AppConfig cfg, const jaal::prog::flags_t<P>& flags = {})
        : host_cfg_(cfg),
          reactor_(make_reactor()),
          window_(open_window(cfg)),
          host_(window_, std::move(cfg)),
          fwd_{host_},
          kernel_(kernel_type::start(fwd_, jaal::platform::steady_clock{},
                                     detail::kernel_options(host_cfg_),
                                     [w = reactor_.waker()] { w.wake(); }, {}, flags)) {
        cx_.emplace(*this);
        step();
        draw();
    }

    headless_app(const headless_app&)            = delete;
    headless_app& operator=(const headless_app&) = delete;

    ~headless_app() {
        try { host_.release(); } catch (...) {}
    }

    // ── driving ─────────────────────────────────────────────────────────

    /// One frame: read scripted input, fold everything waiting (task
    /// results, timers that are due), advance the frame clock if it is on,
    /// and draw if anything changed.
    void tick() {
        host_.pump(*cx_);
        host_.flush_handled(kernel_);
        if (window_.driving_frames()) host_.tick_frame(kernel_, 1.0 / 60.0);
        const auto t = step();
        if (t.model_changed || host_.owes_frame() || window_.driving_frames()) draw();
    }

    /// Deliver a message, as if from outside, then draw.
    void send(msg_type m) {
        kernel_.dispatch(std::move(m));
        tick();
    }
    /// Deliver a message without drawing: the next tick() folds and draws it
    /// (one input, one frame -- what a benchmark wants to time).
    void post(msg_type m) { kernel_.dispatch(std::move(m)); }

    /// Fold until nothing more arrives within `budget`: for a test that
    /// waits on a background task or stream. Returns as soon as `done` holds.
    /// Waits in jaal's reactor exactly as jaal::run does: woken by the
    /// kernel's mailbox, or by its next timer deadline.
    template <class Pred>
    bool settle(Pred done, std::chrono::milliseconds budget = std::chrono::milliseconds(2000)) {
        using clk = jaal::platform::steady_clock;
        const auto end = clk{}.now() + budget;
        while (!done(kernel_.model())) {
            const auto now = clk{}.now();
            if (now >= end) return false;
            auto timeout = jaal::kernel::timeout_from<clk>(now, kernel_.next_deadline());
            const auto left = std::chrono::ceil<std::chrono::nanoseconds>(end - now);
            if (!timeout || *timeout > left) timeout = left;
            (void)reactor_.wait(timeout);
            tick();
            if (finished()) return done(kernel_.model());
        }
        return true;
    }

    void click(std::string_view name) { window_.click(host_.center_of(name)); tick(); }
    void click(std::uint64_t id)      { window_.click(host_.center_of(id)); tick(); }
    /// Click the control showing `label`: the innermost NAMED node (an id<>
    /// or a handler) around the first text equal to it. Tests read like the
    /// UI: `rt.click_text("Save")`. Overlays are searched first, topmost
    /// layer first -- a modal's "Save" is the one a user's click would hit.
    /// Returns false (and clicks nothing) when no such text is on screen.
    bool click_text(std::string_view label) {
        const Node* hit = nullptr;
        const auto find = [&](const auto& self, const Node& n, const Node* named) -> bool {
            if (n.style().id != 0 && !n.style().hit_only) named = &n;
            if (n.kind() == NodeKind::text && n.text() == label) { hit = named ? named : &n; return true; }
            for (const auto& k : n.children()) if (self(self, k, named)) return true;
            return false;
        };
        std::vector<const Overlay*> layers;
        for (const auto& o : host_.overlay_list().items()) layers.push_back(&o);
        std::stable_sort(layers.begin(), layers.end(), [](const Overlay* a, const Overlay* b) { return a->layer > b->layer; });
        bool found = false;
        for (const Overlay* o : layers) if ((found = find(find, o->content, nullptr))) break;
        if (!found) found = find(find, host_.tree(), nullptr);
        if (!found || hit == nullptr) return false;
        window_.click(hit->frame().center());
        tick();
        return true;
    }
    void type(std::string_view s)     { window_.type(s); tick(); }
    void press_key(Key k, Mods m = {}) { window_.press_key(k, m); tick(); }

    // ── state ───────────────────────────────────────────────────────────

    [[nodiscard]] const model_type& model() const noexcept { return kernel_.model(); }
    [[nodiscard]] platform::Headless& window() noexcept { return window_; }
    [[nodiscard]] host_type& host() noexcept { return host_; }
    [[nodiscard]] const Interaction& input() const noexcept { return host_.input(); }
    [[nodiscard]] const Node& tree() const noexcept { return host_.tree(); }
    [[nodiscard]] const LatencyStats& latency() const noexcept { return host_.latency(); }
    [[nodiscard]] bool busy_loop_detected() const noexcept { return host_.busy_loop_detected(); }
    /// Updates that ran over jaal's fold budget (options::fold_budget_time):
    /// each one was a frame the user waited for. A self-test asserts 0.
    [[nodiscard]] std::uint64_t slow_folds() const noexcept { return kernel_.slow_folds(); }
    [[nodiscard]] bool finished() const noexcept { return kernel_.quitting() || !window_.is_open(); }
    [[nodiscard]] std::optional<Rect> node_rect(std::uint64_t id) const { return host_.node_rect(id); }
    [[nodiscard]] std::optional<Rect> node_rect(std::string_view n) const { return host_.node_rect(n); }
    [[nodiscard]] Vec2 center_of(std::string_view n) const { return host_.center_of(n); }
    [[nodiscard]] Vec2 center_of(std::uint64_t id) const { return host_.center_of(id); }

  private:
    // The context the host emits into: routes straight into the kernel,
    // exactly as jaal::run's host_context does.
    struct context {
        headless_app& app;
        std::size_t emit(const window_events& ev) {
            return app.kernel_.route(jaal::to_kernel_event<event_type>(ev), app.fwd_);
        }
        void stop(int code) { app.kernel_.stop(code); }
    };

    static platform::Headless open_window(AppConfig& cfg) {
        auto w = platform::Headless::open(detail::window_config(cfg));
        return std::move(*w);   // headless always opens
    }

    static jaal::platform::native_reactor make_reactor() {
        auto r = jaal::platform::native_reactor::create();
        if (!r) throw std::runtime_error("mayag: can't create jaal reactor");
        return std::move(*r);
    }

    jaal::kernel::turn step() { return kernel_.step(fwd_); }

    void draw() {
        host_.draw(kernel_.model(), [&](std::exception_ptr e) { kernel_.view_faulted(std::move(e)); });
    }

    AppConfig                                   host_cfg_{};
    jaal::platform::native_reactor              reactor_;   // before kernel_: it holds the waker
    platform::Headless                          window_;
    host_type                                   host_;
    jaal::forward_host<host_type, event_type> fwd_;
    kernel_type                                 kernel_;
    std::optional<context>                      cx_;
};

// ── driven from a loop mayag doesn't own ──────────────────────────────

/// What a foreign event loop (Cocoa's run loop) gives the program.
struct loop_hooks {
    /// From ANY thread: run turn() on the loop thread soon. Cheap, safe to
    /// call often.
    std::function<void()> wake;
    /// Loop thread: call turn() after this long (nullopt: no timer).
    std::function<void(std::optional<std::chrono::nanoseconds>)> arm;
    /// Loop thread: start (true) or stop (false) calling vsync() each refresh.
    std::function<void(bool)> frames;
    /// Loop thread: the program finished; leave the loop.
    std::function<void()> stopped;
};

/// P driven by someone else's loop. jaal's kernel never owns the thread (jaal
/// D4), so on macOS the run loop owns it and calls in here:
///
///   turn()      when woken, when the armed timer fires, after window input
///   vsync(dt)   each display refresh, only while frames are wanted
///
/// Everything runs on the loop thread; only `wake` is called from others.
template <Program P, class W>
class embedded_app {
  public:
    using host_type   = window_host<P, W>;
    using event_type  = jaal::kernel_event_t<host_type>;
    using kernel_type = jaal::kernel::kernel<P, event_type, jaal::platform::steady_clock>;

    embedded_app(W& window, const AppConfig& cfg, loop_hooks hooks, const jaal::prog::flags_t<P>& flags = {})
        : hooks_(std::move(hooks)),
          host_(window, cfg),
          fwd_{host_},
          kernel_(kernel_type::start(fwd_, jaal::platform::steady_clock{},
                                     detail::kernel_options(cfg), hooks_.wake, {}, flags)) {
        host_.set_external_loop(true);
        cx_.emplace(*this);
        host_.attach(*cx_);
    }

    embedded_app(const embedded_app&)            = delete;
    embedded_app& operator=(const embedded_app&) = delete;

    ~embedded_app() {
        try { host_.release(); } catch (...) {}
    }

    /// Read input, fold everything waiting, draw if needed, re-arm the loop.
    /// A call made while a turn is running (a callback fired from inside it)
    /// is folded into that turn instead of nesting.
    void turn() {
        if (done_) return;
        if (in_turn_) { again_ = true; return; }
        in_turn_ = true;
        for (int guard = 0; guard < 64; ++guard) {
            again_ = false;
            host_.pump(*cx_);
            host_.flush_handled(kernel_);
            const auto t = kernel_.step(fwd_);
            if (t.quit() || kernel_.quitting()) { finish(t.exit.value_or(0)); break; }
            if (first_ || t.model_changed || host_.owes_frame()) {
                first_ = false;
                host_.present(kernel_);
                ++draws_;
            }
            if (!again_ && !kernel_.has_pending()) break;
        }
        ++turns_;
        in_turn_ = false;
        if (!done_) rearm();
    }

    /// One display refresh, `dt` seconds after the last.
    void vsync(double dt) {
        if (done_) return;
        host_.vsync(kernel_, dt);
        turn();
    }

    /// A signal (SIGINT, ...). The program's on(sig) sees it; one nobody
    /// handles that means "stop" stops the program, as jaal::run does.
    void signal(jaal::sig s) {
        if (done_) return;
        const auto n = kernel_.route(event_type{s}, fwd_);
        if (n == 0 && (s == jaal::sig::interrupt || s == jaal::sig::terminate ||
                       s == jaal::sig::hangup)) {
            kernel_.stop(jaal::exit_code_for(s));
        }
        turn();
    }

    [[nodiscard]] bool finished() const noexcept { return done_; }
    [[nodiscard]] int exit_code() const noexcept { return exit_code_; }
    [[nodiscard]] const typename P::Model& model() const noexcept { return kernel_.model(); }
    [[nodiscard]] host_type& host() noexcept { return host_; }
    [[nodiscard]] std::uint64_t turns() const noexcept { return turns_; }
    [[nodiscard]] std::uint64_t draws() const noexcept { return draws_; }

  private:
    struct context {
        embedded_app& app;
        std::size_t emit(const window_events& ev) {
            return app.kernel_.route(jaal::to_kernel_event<event_type>(ev), app.fwd_);
        }
        void stop(int code) { app.kernel_.stop(code); }
        // attach() watches fds through this; a foreign loop has none.
        template <class... A> jaal::result<jaal::platform::native_reactor::registration> watch(A&&...) {
            return std::unexpected(jaal::error::make(std::errc::not_supported, "embedded: no reactor"));
        }
    };

    void rearm() {
        const bool want = host_.wants_frames();
        if (want != frames_on_) {
            frames_on_ = want;
            if (hooks_.frames) hooks_.frames(want);
        }
        if (hooks_.arm) {
            auto d = jaal::kernel::timeout_from<jaal::platform::steady_clock>(
                kernel_.clock().now(), kernel_.next_deadline());
            // A draw held for the late latch is a deadline like a timer:
            // without this it would wait for the next input or tick.
            if (const auto h = host_.wait_hint()) d = d ? std::min(*d, *h) : *h;
            hooks_.arm(d);
        }
    }

    void finish(int code) {
        done_ = true;
        exit_code_ = code;
        if (frames_on_ && hooks_.frames) hooks_.frames(false);
        frames_on_ = false;
        if (hooks_.arm) hooks_.arm(std::nullopt);
        if (hooks_.stopped) hooks_.stopped();
    }

    loop_hooks                                             hooks_;
    host_type                                              host_;
    jaal::forward_host<host_type, event_type> fwd_;
    kernel_type                                            kernel_;
    std::optional<context>                                 cx_;
    bool          in_turn_ = false, again_ = false, first_ = true, done_ = false;
    bool          frames_on_ = false;
    int           exit_code_ = 0;
    std::uint64_t turns_ = 0, draws_ = 0;
};

/// Run P headlessly under a driver. The renderer banner is off: headless is
/// always software and always in CI.
template <Program P, class Driver>
int run_headless(AppConfig cfg, Driver&& drive) {
    return run_headless<P>(std::move(cfg), jaal::prog::flags_t<P>{}, std::forward<Driver>(drive));
}
/// ...starting P from `flags` (P::Flags: the data its init takes).
template <Program P, class Driver>
int run_headless(AppConfig cfg, jaal::prog::flags_t<P> flags, Driver&& drive) {
    cfg.log_renderer = false;
    headless_app<P> app{std::move(cfg), flags};
    // A driver may return its verdict (an int, 0 = pass, or a bool, true =
    // pass). It used to be dropped, so an example whose self-check printed
    // FAIL still exited 0 and ctest called it green.
    using R = std::invoke_result_t<Driver&, headless_app<P>&>;
    if constexpr (std::same_as<R, bool>) return drive(app) ? 0 : 1;
    else if constexpr (std::is_integral_v<R>) return static_cast<int>(drive(app)) == 0 ? 0 : 1;
    else { drive(app); return 0; }
}

/// Start the GPU's window-independent bring-up now, on a worker, so it
/// overlaps everything an app does before it opens its window (config,
/// fonts, the shell). Call it first thing in main(); run() adopts the
/// result. A no-op where the window brings its GPU up itself.
namespace detail {
template <class W>
void gpu_prestart_on() {
    if constexpr (platform::has<W, platform::cap::prestarts_gpu>) W::gpu_prestart();
}
}  // namespace detail
inline void gpu_prestart() {
    platform::raise_ui_thread();   // before the first thread starts: they inherit it
    detail::gpu_prestart_on<platform::Native>();
}

namespace detail {
inline void log_renderer(std::string_view name) {
    ::mayag::diag::say("mayag: renderer %.*s\n", static_cast<int>(name.size()), name.data());
}
}  // namespace detail

namespace detail {
/// `MAYAG_PHOTON=1`: print the measured input-to-photon distribution when
/// the window closes, so latency can be checked from a script without a HUD.
template <class W>
void log_photon(W& window) {
    if constexpr (platform::has<W, platform::cap::measures_photons>) {
        const char* on = std::getenv("MAYAG_PHOTON");
        if (on == nullptr || *on == '\0' || *on == '0') return;
        const auto s = window.photon_stats();
        ::mayag::diag::say("mayag: input->photon  best %.2f  p50 %.2f  mean %.2f  p95 %.2f  worst %.2f ms  (%d frames)",
                     s.best_ms, s.p50_ms, s.mean_ms, s.p95_ms, s.worst_ms, s.samples);
        ::mayag::diag::say("\n  stages (mean): to us %.2f  in us %.2f  compositor %.2f ms",
                     s.stage_ms[0], s.stage_ms[1], s.stage_ms[2]);
        if constexpr (platform::has<W, platform::cap::counts_latch_misses>) {
            const auto [missed, tried] = window.latch_misses();
            ::mayag::diag::say("  latch margin %.2f ms, %llu/%llu late draws missed",
                         static_cast<double>(window.latch_margin().count()) / 1e6,
                         static_cast<unsigned long long>(missed),
                         static_cast<unsigned long long>(tried));
        }
        ::mayag::diag::say("\n");
    }
}
}  // namespace detail

/// Open a window and run P in it, with flags built from the FONTS once they
/// are loaded: `make(stack)` returns P::Flags. Fonts load on a worker while
/// the window opens (they don't depend on each other), so a program whose
/// starting data includes its font set gets it without a global and without
/// losing that overlap.
template <Program P, class Make>
    requires std::invocable<Make&, std::shared_ptr<typo::FontStack>> &&
             std::convertible_to<std::invoke_result_t<Make&, std::shared_ptr<typo::FontStack>>, jaal::prog::flags_t<P>>
int run(AppConfig cfg, Make make);

/// Open a window and run P in it, starting from `flags` when P has Flags.
template <Program P>
int run(AppConfig cfg, jaal::prog::flags_t<P> flags) {
    detail::apply_env_overrides(cfg);
    // Fonts and the window/GPU don't depend on each other: start the font
    // scan on a worker, open the window here, then collect it.
    std::optional<typo::system::FontLoad> fonts;
    if (cfg.fonts == nullptr) {
        if (cfg.font_loader != nullptr) fonts.emplace(cfg.font_loader);
        else fonts.emplace();
    }
    auto opened = platform::Native::open(detail::window_config(cfg));
    if (!opened) {
        ::mayag::diag::fatal("mayag: could not open a window\n");
        return 1;
    }
    platform::Native window = std::move(*opened);
    if (fonts) {
        cfg.fonts = fonts->get();
        startup::mark("fonts ready");
    }
    return run_on<P>(window, std::move(cfg), std::move(flags));
}

template <Program P, class Make>
    requires std::invocable<Make&, std::shared_ptr<typo::FontStack>> &&
             std::convertible_to<std::invoke_result_t<Make&, std::shared_ptr<typo::FontStack>>, jaal::prog::flags_t<P>>
int run(AppConfig cfg, Make make) {
    detail::apply_env_overrides(cfg);
    std::optional<typo::system::FontLoad> fonts;
    if (cfg.fonts == nullptr) {
        if (cfg.font_loader != nullptr) fonts.emplace(cfg.font_loader);
        else fonts.emplace();
    }
    auto opened = platform::Native::open(detail::window_config(cfg));
    if (!opened) {
        ::mayag::diag::fatal("mayag: could not open a window\n");
        return 1;
    }
    platform::Native window = std::move(*opened);
    if (fonts) {
        cfg.fonts = fonts->get();
        startup::mark("fonts ready");
    }
    jaal::prog::flags_t<P> flags = make(cfg.fonts);
    return run_on<P>(window, std::move(cfg), std::move(flags));
}

/// Run P on a window the caller opened and keeps (to read it back after,
/// say). Returns when the program quits.
template <Program P, class W>
int run_on(W& window, AppConfig cfg, jaal::prog::flags_t<P> flags) {
    platform::raise_ui_thread();   // input-to-frames beats background work (platform/priority.hpp)
    detail::apply_env_overrides(cfg);
#if MAYAG_WINDOW_COCOA
    if constexpr (std::same_as<W, platform::MacWindow>) {
    // macOS: AppKit owns the main thread; the run loop drives the program.
    window.set_pull(false);
    platform::cocoa::RunLoop loop{window};
    loop_hooks hooks;
    hooks.wake    = [&loop] { loop.wake(); };   // any thread
    hooks.arm     = [&loop](std::optional<std::chrono::nanoseconds> d) { loop.arm(d); };
    hooks.frames  = [&loop](bool on) { loop.frames(on); };
    hooks.stopped = [&loop] { loop.stop(); };
    embedded_app<P, W> app{window, cfg, std::move(hooks), flags};
    if (cfg.log_renderer) detail::log_renderer(app.host().renderer_name());

    // Declared after `app`, so they are torn down before it: ^C works again
    // as soon as the program starts shutting down.
    auto sigs = jaal::platform::native_signals::install(jaal::all_signals);
    if (sigs) {
        loop.watch_signals(sigs->handle());
        loop.on_signal = [&](int) {
            for (auto s : sigs->take()) app.signal(s);
        };
    }
    loop.on_turn  = [&app] { app.turn(); };
    loop.on_vsync = [&app](double dt) { app.vsync(dt); };

    app.turn();          // first frame, first subscriptions
    if (!app.finished()) loop.run();
    detail::log_photon(window);
    return app.exit_code();
    } else
#endif
    {
    const bool log = cfg.log_renderer;
    jaal::run_options opt;
    opt.kernel = detail::kernel_options(cfg);
    window_host<P, W> host{window, std::move(cfg)};
    startup::mark("host constructed");
    if (log) detail::log_renderer(host.renderer_name());
    const int code = jaal::run<P>(host, std::move(opt), jaal::durable<P>{}, std::move(flags));
    detail::log_photon(window);
    return code;
    }
}

}  // namespace mayag
