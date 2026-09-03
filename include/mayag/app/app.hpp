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
#include "../style/theme.hpp"
#include "../text/font.hpp"
#include "../font/font.hpp"
#include "cmd.hpp"
#include "event.hpp"
#include "interaction.hpp"
#include "platform.hpp"
#include "sub.hpp"

#include <algorithm>
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

struct AppConfig {
    std::string title = "mayag";
    Vec2        size{1024, 640};
    bool        resizable = true;
    bool        vsync     = true;
    Theme       theme     = themes::midnight;

    /// Built-in stroke font — zero dependencies, used when `fonts` is null.
    const strokefont::Font* font = nullptr;

    /// The real font engine: system typefaces, kerning, script fallback.
    /// Takes precedence over `font` when set.
    typo::FontStack* fonts = nullptr;

    /// Overlay every node's rect. Bound to a key in your own subscribe() if
    /// you want to toggle it at runtime.
    bool debug_bounds = false;
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

/// Thread-safe inbox for messages produced off the UI thread (`Cmd::task`).
template <typename Msg>
class Inbox {
  public:
    void post(Msg m) {
        std::lock_guard lock{mutex_};
        queue_.push_back(std::move(m));
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
    std::mutex        mutex_;
    std::deque<Msg>   queue_;
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

        // Bind the font engine once, at boot. The real engine wins when both
        // are supplied; the stroke font is the always-available fallback.
        if (cfg_.fonts != nullptr) {
            bindings_.emplace(*cfg_.fonts);
            measurer_ = &bindings_->measurer;
            glyphs_   = &bindings_->glyphs;
            sampler_  = &bindings_->sampler;
        } else {
            stroke_font_ = cfg_.font ? cfg_.font : &strokefont::Font::builtin_font();
            measurer_ = &stroke_font_->measurer();
            glyphs_   = &stroke_font_->glyph_renderer();
            sampler_  = &stroke_font_->sampler();
        }
        window_.set_coverage_sampler(sampler_);

        size_ = window_.size();
        dpi_  = window_.dpi_scale();

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

        for (const auto& ev : events) {
            handle_event(ev, subs);
            if (quit_) return;
        }

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
    [[nodiscard]] const Node& tree() const noexcept { return tree_; }
    [[nodiscard]] const Interaction& input() const noexcept { return input_; }
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
        auto [next, cmd] = P::update(std::move(model_), std::move(msg));
        model_ = std::move(next);
        dirty_ = true;
        interpret(cmd);
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
        }

        // Pointer events become semantic gestures against the CURRENT tree.
        const auto gestures = input_.handle(ev, tree_, time_);
        if (!gestures.empty()) dirty_ = true;

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
                    if (g.node_id == a.node_id && matches(g.kind, a.gesture)) out.push_back(a.msg);
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
            case G::double_click: return k == Gesture::Kind::double_click;
            case G::press:        return k == Gesture::Kind::press;
            case G::release:      return k == Gesture::Kind::release;
            case G::enter:        return k == Gesture::Kind::enter;
            case G::leave:        return k == Gesture::Kind::leave;
            case G::drag:         return k == Gesture::Kind::drag;
            case G::scroll:       return k == Gesture::Kind::scroll;
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

    [[nodiscard]] Sub<Msg> current_subs() const {
        if constexpr (detail::HasSubscribe<P>) return P::subscribe(model_);
        else return Sub<Msg>::none();
    }

    // ── the frame ───────────────────────────────────────────────────────

    void render() {
        // view -> layout -> paint -> present. Skipped entirely when nothing
        // changed, which is what keeps an idle window at zero GPU load.
        if (!dirty_) return;
        dirty_ = false;

        size_ = window_.size();
        dpi_  = window_.dpi_scale();

        if (cfg_.fonts != nullptr) cfg_.fonts->begin_frame(frame_counter_);
        ++frame_counter_;

        Ctx ctx{size_, dpi_, time_, cfg_.theme, &input_};

        if constexpr (detail::ViewWithCtx<P>) {
            tree_ = P::view(model_, ctx);
        } else {
            tree_ = P::view(model_);
        }

        layout::layout_tree(tree_, size_, *measurer_);

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
        render::PaintOptions po{};
        po.dpi_scale    = dpi_;
        po.measurer     = measurer_;
        po.glyphs       = glyphs_;
        po.debug_bounds = cfg_.debug_bounds;
        render::paint(tree_, draws_, po);

        window_.present(draws_, cfg_.theme.background);
    }

    AppConfig    cfg_;
    W            window_{};
    Model        model_{};
    Node         tree_{};
    DrawList     draws_{};
    Interaction  input_{};

    // Font binding, resolved once at boot. `bindings_` owns the adapters when
    // the real engine is in use; `stroke_font_` is the built-in path.
    std::optional<detail::StackBindings> bindings_;
    const strokefont::Font*                   stroke_font_ = nullptr;
    const layout::TextMeasurer*          measurer_ = nullptr;
    const render::GlyphRenderer*         glyphs_   = nullptr;
    const backend::CoverageSampler*      sampler_  = nullptr;
    std::uint64_t                        frame_counter_ = 0;
    std::size_t                          last_issue_count_ = static_cast<std::size_t>(-1);

    detail::Inbox<Msg>              inbox_;
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
    Runtime<P, platform::Headless> rt{std::move(cfg)};
    if (!rt.boot()) return 1;
    drive(rt);
    return 0;
}

}  // namespace mayag
