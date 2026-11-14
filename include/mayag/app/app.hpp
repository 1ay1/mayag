#pragma once
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
#include "event.hpp"
#include "host.hpp"
#include "interaction.hpp"
#include "latency.hpp"
#include "platform.hpp"
#include "sources.hpp"

#include <jaal/jaal.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
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
        .resizable = cfg.resizable, .decorated = true,
        .transparent = false, .vsync = cfg.vsync,
        .background = cfg.theme.background,
    };
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

/// Run P on the native window. This is main() for a mayag app.
template <Program P>
int run(AppConfig cfg = {}) {
    auto opened = platform::Native::open(detail::window_config(cfg));
    if (!opened) {
        std::fprintf(stderr, "mayag: could not open a window\n");
        return 1;
    }
    platform::Native window = std::move(*opened);
    const bool log = cfg.log_renderer;
    jaal::run_options opt;
    opt.kernel = detail::kernel_options(cfg);
    window_host<P, platform::Native> host{window, std::move(cfg)};
    if (log) {
        const auto name = host.renderer_name();
        std::fprintf(stderr, "mayag: renderer %.*s\n", static_cast<int>(name.size()), name.data());
    }
    return jaal::run<P>(host, std::move(opt));
}

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

    explicit headless_app(AppConfig cfg)
        : host_cfg_(cfg),
          window_(open_window(cfg)),
          host_(window_, std::move(cfg)),
          fwd_{host_},
          kernel_(kernel_type::start(fwd_, jaal::platform::steady_clock{},
                                     detail::kernel_options(host_cfg_), wake())) {
        host_.set_manual_clock(true);
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
        if (window_.driving_frames()) host_.tick_frame(1.0 / 60.0);
        const auto t = step();
        if (t.model_changed || host_.owes_frame() || window_.driving_frames()) draw();
    }

    /// Deliver a message, as if from outside, then draw.
    void send(msg_type m) {
        kernel_.dispatch(std::move(m));
        tick();
    }

    /// Fold until nothing more arrives within `budget`: for a test that
    /// waits on a background task or stream. Returns as soon as `done` holds.
    template <class Pred>
    bool settle(Pred done, std::chrono::milliseconds budget = std::chrono::milliseconds(2000)) {
        const auto end = std::chrono::steady_clock::now() + budget;
        while (!done(kernel_.model())) {
            if (std::chrono::steady_clock::now() >= end) return false;
            wait_for_news(std::chrono::milliseconds(5));
            tick();
            if (finished()) return done(kernel_.model());
        }
        return true;
    }

    void click(std::string_view name) { window_.click(host_.center_of(name)); tick(); }
    void click(std::uint64_t id)      { window_.click(host_.center_of(id)); tick(); }
    void type(std::string_view s)     { window_.type(s); tick(); }
    void press_key(Key k, Mods m = {}) { window_.press_key(k, m); tick(); }

    // ── state ───────────────────────────────────────────────────────────

    [[nodiscard]] const model_type& model() const noexcept { return kernel_.model(); }
    [[nodiscard]] platform::Headless& window() noexcept { return window_; }
    [[nodiscard]] host_type& host() noexcept { return host_; }
    [[nodiscard]] kernel_type& kernel() noexcept { return kernel_; }
    [[nodiscard]] const Interaction& input() const noexcept { return host_.input(); }
    [[nodiscard]] const Node& tree() const noexcept { return host_.tree(); }
    [[nodiscard]] const LatencyStats& latency() const noexcept { return host_.latency(); }
    [[nodiscard]] bool busy_loop_detected() const noexcept { return host_.busy_loop_detected(); }
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
            return app.kernel_.route(jaal::detail::run::to_kernel_event<event_type>(ev), app.fwd_);
        }
        void stop(int code) { app.kernel_.stop(code); }
    };

    static platform::Headless open_window(AppConfig& cfg) {
        auto w = platform::Headless::open(detail::window_config(cfg));
        return std::move(*w);   // headless always opens
    }

    jaal::kernel::turn step() { return kernel_.step(fwd_); }

    void draw() {
        host_.draw(kernel_.model(), [&](std::exception_ptr e) { kernel_.view_faulted(std::move(e)); });
    }

    std::function<void()> wake() {
        return [w = woken_] { w->store(true, std::memory_order_release); };
    }
    void wait_for_news(std::chrono::milliseconds most) {
        const auto end = std::chrono::steady_clock::now() + most;
        while (!woken_->exchange(false, std::memory_order_acq_rel) &&
               std::chrono::steady_clock::now() < end) {
            std::this_thread::yield();
        }
    }

    AppConfig                                   host_cfg_{};
    std::shared_ptr<std::atomic<bool>>          woken_ = std::make_shared<std::atomic<bool>>(false);
    platform::Headless                          window_;
    host_type                                   host_;
    jaal::detail::run::forward_host<host_type, event_type> fwd_;
    kernel_type                                 kernel_;
    std::optional<context>                      cx_;
};

/// Run P headlessly under a driver. The renderer banner is off: headless is
/// always software and always in CI.
template <Program P, class Driver>
int run_headless(AppConfig cfg, Driver&& drive) {
    cfg.log_renderer = false;
    headless_app<P> app{std::move(cfg)};
    drive(app);
    return 0;
}

}  // namespace mayag
