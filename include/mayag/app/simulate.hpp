#pragma once
// mayag::simulate — deterministic simulation of a whole GUI program
//
// The REAL host (cascade, layout, hit testing, inline handlers, theme morph,
// transitions) and the REAL kernel, with everything nondeterministic put
// under one seed by jaal's simulation world:
//
//   * time is simulated: a 30-second scenario runs in milliseconds, and
//     timers, springs, theme morphs and frame pacing all see that time
//   * task bodies (Cmd::task / Cmd::perform) do not go to a thread pool;
//     each completes at now + a seeded latency, on the loop thread, so
//     results race clicks, timers and each other in a random but EXACTLY
//     REPEATABLE order
//   * tasks can be made to crash or vanish at a given rate, to drive the
//     program's fault paths
//
// Same seed, same run, bit for bit. So a flaky async UI bug becomes a seed
// you can replay, and explore() hunts for one across thousands of seeds:
//
//     auto r = mayag::explore<Search>(cfg, {.first_seed = 1, .runs = 2000}, [](auto& app) {
//         app.type("ma"); app.wait(5ms); app.type("y");
//         app.settle();
//         return app.model().shown_for == app.model().query ? 0 : 1;   // invariant
//     });
//     if (!r.ok()) std::printf("fails with seed %llu\n", r.seed);    // replay it:
//     mayag::run_simulated<Search>(cfg, r.seed, scenario);
//
// The driver is the same shape as run_headless's (click_text, type,
// press_key, model, host, return 0 for pass), plus time control: wait(d)
// lets simulated time pass, settle() runs until nothing is pending.

#include "app.hpp"

#include <jaal/host/sim.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace mayag {

/// Task latency range, fault injection and limits for a simulated run.
using SimOptions = jaal::sim_options;

template <Program P>
class simulated_app {
  public:
    using clock       = jaal::platform::sim_clock;
    using host_type   = window_host<P, platform::Headless, clock>;
    using event_type  = jaal::kernel_event_t<host_type>;
    using kernel_type = jaal::kernel::kernel<P, event_type, clock>;
    using model_type  = typename P::Model;
    using msg_type    = typename P::Msg;
    using duration    = clock::duration;

    /// One frame of simulated time: what tick() advances.
    static constexpr duration frame = std::chrono::microseconds(16'667);

    simulated_app(AppConfig cfg, std::uint64_t seed, SimOptions opt = {})
        : seed_(seed),
          world_(seed, opt),
          host_cfg_(quiet(cfg)),
          window_(open_window(host_cfg_)),
          host_(window_, host_cfg_),
          fwd_{{host_}, &world_, &streams_},
          kernel_(kernel_type::start(fwd_, clock{}, detail::kernel_options(host_cfg_), [] {})) {
        host_.set_clock(kernel_.clock());
        cx_.emplace(*this);
        step();
        draw();
    }

    simulated_app(const simulated_app&)            = delete;
    simulated_app& operator=(const simulated_app&) = delete;
    ~simulated_app() {
        try { host_.release(); } catch (...) {}
    }

    // ── time ────────────────────────────────────────────────────────────

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
    /// Simulated time since the run began.
    [[nodiscard]] duration elapsed() const noexcept { return world_.now - clock::time_point{}; }

    /// One frame: input, then everything due in the next 16.7 ms of
    /// simulated time (task results, timers, the frame clock), then a draw.
    void tick() { advance(frame); }

    /// Let `d` of simulated time pass, a frame at a time, running every task
    /// result and timer at the instant it falls due.
    void wait(duration d) {
        const auto end = world_.now + d;
        while (world_.now < end && !finished()) advance(std::min(frame, end - world_.now));
    }

    /// Run until nothing is pending (no task in flight, no armed timer, no
    /// spring or morph moving) or `limit` of simulated time passes. Returns
    /// true when it went quiet. Never waits on a wall clock.
    bool settle(duration limit = std::chrono::seconds(30)) {
        const auto end = world_.now + limit;
        while (world_.now < end && !finished()) {
            if (quiet_now()) return true;
            advance(frame);
        }
        return quiet_now();
    }
    /// Run until `done(model)` holds (or `limit` passes).
    template <class Pred>
    bool settle_until(Pred done, duration limit = std::chrono::seconds(30)) {
        const auto end = world_.now + limit;
        while (!done(kernel_.model()) && world_.now < end && !finished()) advance(frame);
        return done(kernel_.model());
    }

    // ── input, as a user gives it ───────────────────────────────────────

    void send(msg_type m) { kernel_.dispatch(std::move(m)); tick(); }
    void click(std::string_view name) { window_.click(host_.center_of(name)); tick(); }
    void click(std::uint64_t id) { window_.click(host_.center_of(id)); tick(); }
    bool click_text(std::string_view label) {
        const Node* hit = nullptr;
        const auto find = [&](const auto& self, const Node& n, const Node* named) -> bool {
            if (n.style().id != 0 && !n.style().hit_only) named = &n;
            if (n.kind() == NodeKind::text && n.text() == label) { hit = named ? named : &n; return true; }
            for (const auto& k : n.children()) if (self(self, k, named)) return true;
            return false;
        };
        if (!find(find, host_.tree(), nullptr) || hit == nullptr) return false;
        window_.click(hit->frame().center());
        tick();
        return true;
    }
    void type(std::string_view s) { window_.type(s); tick(); }
    void press_key(Key k, Mods m = {}) { window_.press_key(k, m); tick(); }

    /// The sink of the running stream with this key (closed if none), to
    /// feed a Sub::stream from the scenario.
    [[nodiscard]] Sink<msg_type> stream(std::string_view key) const {
        if (auto it = streams_.find(key); it != streams_.end()) return it->second;
        return {};
    }

    // ── state ───────────────────────────────────────────────────────────

    [[nodiscard]] const model_type& model() const noexcept { return kernel_.model(); }
    [[nodiscard]] platform::Headless& window() noexcept { return window_; }
    [[nodiscard]] host_type& host() noexcept { return host_; }
    [[nodiscard]] const Node& tree() const noexcept { return host_.tree(); }
    [[nodiscard]] const Interaction& input() const noexcept { return host_.input(); }
    [[nodiscard]] bool finished() const noexcept { return kernel_.quitting() || !window_.is_open(); }

  private:
    // The kernel's host: jaal's forwarder plus the seeded executor, so task
    // bodies run in the simulated world instead of on real threads.
    struct sim_host : jaal::forward_host<host_type, event_type> {
        jaal::sim_world* world = nullptr;
        std::map<std::string, Sink<msg_type>, std::less<>>* streams = nullptr;
        template <class Msg>
        auto make_executor(typename jaal::kernel::executor<Msg>::error_fn on_error) {
            return std::make_unique<jaal::sim_executor<Msg>>(*world, std::move(on_error), *streams);
        }
    };

    struct context {
        simulated_app& app;
        std::size_t emit(const window_events& ev) {
            return app.kernel_.route(jaal::to_kernel_event<event_type>(ev), app.fwd_);
        }
        void stop(int code) { app.kernel_.stop(code); }
    };

    static AppConfig quiet(AppConfig c) { c.log_renderer = false; return c; }
    static platform::Headless open_window(const AppConfig& cfg) {
        return std::move(*platform::Headless::open(detail::window_config(cfg)));
    }

    /// Advance simulated time by `d`: every world item (task result) and
    /// kernel timer due inside the window runs at its own instant, in the
    /// seeded order; then input, the frame clock, and one draw.
    void advance(duration d) {
        const auto end = world_.now + d;
        bool changed = false;   // a result folded mid-window must still be drawn
        for (;;) {
            auto next = world_.next_due();
            if (const auto t = kernel_.next_deadline(); t && *t != clock::time_point::min())
                if (!next || *t < *next) next = *t;
            if (!next || *next > end) break;
            world_.now = std::max(world_.now, *next);
            kernel_.clock().set(world_.now);
            if (world_.next_due() && *world_.next_due() <= world_.now) world_.run_one();
            changed = step().model_changed || changed;
        }
        world_.now = end;
        kernel_.clock().set(end);
        window_.advance(std::chrono::duration<double>(d).count());
        host_.pump(*cx_);
        host_.flush_handled(kernel_);
        host_.tick_frame(kernel_, std::chrono::duration<double>(d).count());
        changed = step().model_changed || changed;
        if (changed || host_.owes_frame() || host_.transitioning() || window_.driving_frames()) draw();
    }

    [[nodiscard]] bool quiet_now() const {
        return !world_.next_due() && !kernel_.next_deadline() && !host_.transitioning() &&
               !host_.has_handled();
    }

    jaal::kernel::turn step() { return kernel_.step(fwd_); }
    void draw() {
        host_.draw(kernel_.model(), [&](std::exception_ptr e) { kernel_.view_faulted(std::move(e)); });
    }

    std::uint64_t                                       seed_;
    jaal::sim_world                                     world_;
    std::map<std::string, Sink<msg_type>, std::less<>>  streams_;
    AppConfig                                           host_cfg_;
    platform::Headless                                  window_;
    host_type                                           host_;
    sim_host                                            fwd_;
    kernel_type                                         kernel_;
    std::optional<context>                              cx_;
};

/// Run one simulated scenario from `seed`. The driver returns 0 (or true)
/// for pass, like run_headless's.
template <Program P, class Driver>
int run_simulated(AppConfig cfg, std::uint64_t seed, Driver&& drive, SimOptions opt = {}) {
    simulated_app<P> app{std::move(cfg), seed, opt};
    using R = std::invoke_result_t<Driver&, simulated_app<P>&>;
    if constexpr (std::same_as<R, bool>) return drive(app) ? 0 : 1;
    else if constexpr (std::is_integral_v<R>) return static_cast<int>(drive(app)) == 0 ? 0 : 1;
    else { drive(app); return 0; }
}

struct ExploreOptions {
    std::uint64_t first_seed = 1;
    std::size_t   runs = 500;
    SimOptions    sim{};
};

struct ExploreReport {
    std::size_t   runs = 0;          ///< scenarios run before stopping
    bool          failed = false;
    std::uint64_t seed = 0;          ///< the first failing seed: replay it with run_simulated
    [[nodiscard]] bool ok() const noexcept { return !failed; }
};

/// Run the scenario once per seed and stop at the first that fails. A
/// failing seed is a complete, replayable reproduction.
template <Program P, class Driver>
ExploreReport explore(AppConfig cfg, ExploreOptions o, Driver drive) {
    // One font stack for every run: discovering system fonts is the only
    // expensive part of starting a host, and fonts are not part of what a
    // seed varies. Runs stay independent (their own host, kernel, world).
    if (cfg.fonts == nullptr) cfg.fonts = typo::system::default_stack();
    ExploreReport r;
    for (std::size_t i = 0; i < o.runs; ++i) {
        const std::uint64_t s = o.first_seed + i;
        ++r.runs;
        if (run_simulated<P>(cfg, s, drive, o.sim) != 0) { r.failed = true; r.seed = s; break; }
    }
    return r;
}

}  // namespace mayag
