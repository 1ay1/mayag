#pragma once
// mayag::Cmd<Msg> — side effects as data
//
// A pure function cannot perform I/O. But it can return a *description* of
// I/O to be performed later. `Cmd<Msg>` is that description.
//
//     auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
//         return std::visit(overload{
//             [&](Save)  { return pair{m, Cmd<Msg>::write_file("out.txt", m.text)}; },
//             [&](Close) { return pair{m, Cmd<Msg>::quit()}; },
//         }, msg);
//     }
//
// This is what makes `update()` testable: same inputs, same outputs, no
// window, no GPU, no clock. You assert on the returned Cmd instead of
// mocking the world. The runtime is the only thing that interprets them.
//
// Cmd is a functor: `map()` lifts a child component's Cmd into its parent's
// Msg type, so components compose without knowing about their parents.

#include "../core/geometry.hpp"
#include "../platform/types.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mayag {

template <typename Msg>
class Cmd {
  public:
    // ── alternatives ────────────────────────────────────────────────────

    struct None {};
    struct Quit {};

    struct Batch { std::vector<Cmd> cmds; };

    /// Deliver `msg` after a delay. The runtime owns the timer.
    struct After {
        std::chrono::milliseconds delay{};
        Msg msg;
    };

    /// Run `work` on a worker thread; its returned Msg comes back to
    /// `update()` on the UI thread. This is how you do network I/O, file
    /// reads, or anything slow without blocking the frame.
    struct Task { std::function<Msg()> work; };

    /// Fire-and-forget side effect with no message back.
    struct Perform { std::function<void()> action; };

    // -- window control --
    struct SetTitle      { std::string title; };
    struct SetCursor     { int shape = 0; };   ///< see CursorShape
    struct SetClipboard  { std::string text; };
    struct ReadClipboard { std::function<Msg(std::string)> then; };
    struct SetWindowSize { Vec2 size; };
    struct Fullscreen    { bool on = true; };

    /// Force a repaint even if the model did not change (video, external
    /// texture updates).
    struct Redraw {};

    /// Grab or release keyboard focus for a named node.
    struct Focus   { std::uint64_t node_id = 0; };

    /// Write a PNG of the current frame — screenshots and visual tests.
    struct Screenshot { std::string path; };

    using Alt = std::variant<None, Quit, Batch, After, Task, Perform,
                             SetTitle, SetCursor, SetClipboard, ReadClipboard,
                             SetWindowSize, Fullscreen, Redraw, Focus, Screenshot>;

    // ── construction ────────────────────────────────────────────────────

    Cmd() : alt_{None{}} {}
    explicit Cmd(Alt a) : alt_{std::move(a)} {}

    [[nodiscard]] static Cmd none() { return Cmd{}; }
    [[nodiscard]] static Cmd quit() { return Cmd{Alt{Quit{}}}; }
    [[nodiscard]] static Cmd redraw() { return Cmd{Alt{Redraw{}}}; }

    [[nodiscard]] static Cmd after(std::chrono::milliseconds d, Msg m) {
        return Cmd{Alt{After{d, std::move(m)}}};
    }

    template <typename F>
    [[nodiscard]] static Cmd task(F&& f) {
        return Cmd{Alt{Task{std::function<Msg()>(std::forward<F>(f))}}};
    }

    template <typename F>
    [[nodiscard]] static Cmd perform(F&& f) {
        return Cmd{Alt{Perform{std::function<void()>(std::forward<F>(f))}}};
    }

    [[nodiscard]] static Cmd set_title(std::string t) {
        return Cmd{Alt{SetTitle{std::move(t)}}};
    }
    [[nodiscard]] static Cmd set_cursor(int shape) {
        return Cmd{Alt{SetCursor{shape}}};
    }
    [[nodiscard]] static Cmd write_clipboard(std::string s) {
        return Cmd{Alt{SetClipboard{std::move(s)}}};
    }
    template <typename F>
    [[nodiscard]] static Cmd read_clipboard(F&& then) {
        return Cmd{Alt{ReadClipboard{std::function<Msg(std::string)>(std::forward<F>(then))}}};
    }
    [[nodiscard]] static Cmd set_window_size(Vec2 s) {
        return Cmd{Alt{SetWindowSize{s}}};
    }
    [[nodiscard]] static Cmd fullscreen(bool on = true) {
        return Cmd{Alt{Fullscreen{on}}};
    }
    [[nodiscard]] static Cmd focus(std::uint64_t node_id) {
        return Cmd{Alt{Focus{node_id}}};
    }
    [[nodiscard]] static Cmd screenshot(std::string path) {
        return Cmd{Alt{Screenshot{std::move(path)}}};
    }

    /// Combine effects. Flattens, and drops `none` so batching in a loop
    /// does not accumulate garbage.
    [[nodiscard]] static Cmd batch(std::vector<Cmd> cmds) {
        std::vector<Cmd> kept;
        kept.reserve(cmds.size());
        for (auto& c : cmds) {
            if (c.is_none()) continue;
            if (auto* b = std::get_if<Batch>(&c.alt_)) {
                for (auto& inner : b->cmds) kept.push_back(std::move(inner));
            } else {
                kept.push_back(std::move(c));
            }
        }
        if (kept.empty()) return none();
        if (kept.size() == 1) return std::move(kept.front());
        return Cmd{Alt{Batch{std::move(kept)}}};
    }

    template <typename... Cs>
    [[nodiscard]] static Cmd batch(Cs&&... cs) {
        std::vector<Cmd> v;
        v.reserve(sizeof...(Cs));
        (v.push_back(std::forward<Cs>(cs)), ...);
        return batch(std::move(v));
    }

    // ── queries ─────────────────────────────────────────────────────────

    [[nodiscard]] bool is_none() const noexcept {
        return std::holds_alternative<None>(alt_);
    }
    [[nodiscard]] bool is_quit() const noexcept {
        return std::holds_alternative<Quit>(alt_);
    }
    [[nodiscard]] const Alt& alternative() const noexcept { return alt_; }

    // ── functor ─────────────────────────────────────────────────────────

    /// Lift this Cmd into a parent's message type. A child component returns
    /// `Cmd<ChildMsg>`; the parent writes
    /// `child_cmd.map([](ChildMsg m) { return Parent::FromChild{m}; })`
    /// and never learns what the child was doing.
    template <typename F>
    [[nodiscard]] auto map(F f) const -> Cmd<std::invoke_result_t<F, Msg>> {
        using Out = Cmd<std::invoke_result_t<F, Msg>>;
        return std::visit([&f](const auto& a) -> Out {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, None>)   return Out::none();
            else if constexpr (std::is_same_v<T, Quit>)   return Out::quit();
            else if constexpr (std::is_same_v<T, Redraw>) return Out::redraw();
            else if constexpr (std::is_same_v<T, Batch>) {
                std::vector<Out> mapped;
                mapped.reserve(a.cmds.size());
                for (const auto& c : a.cmds) mapped.push_back(c.map(f));
                return Out::batch(std::move(mapped));
            }
            else if constexpr (std::is_same_v<T, After>)
                return Out::after(a.delay, f(a.msg));
            else if constexpr (std::is_same_v<T, Task>)
                return Out::task([work = a.work, f] { return f(work()); });
            else if constexpr (std::is_same_v<T, Perform>)
                return Out::perform(a.action);
            else if constexpr (std::is_same_v<T, ReadClipboard>)
                return Out::read_clipboard([then = a.then, f](std::string s) { return f(then(s)); });
            else if constexpr (std::is_same_v<T, SetTitle>)     return Out::set_title(a.title);
            else if constexpr (std::is_same_v<T, SetCursor>)    return Out::set_cursor(a.shape);
            else if constexpr (std::is_same_v<T, SetClipboard>) return Out::write_clipboard(a.text);
            else if constexpr (std::is_same_v<T, SetWindowSize>)return Out::set_window_size(a.size);
            else if constexpr (std::is_same_v<T, Fullscreen>)   return Out::fullscreen(a.on);
            else if constexpr (std::is_same_v<T, Focus>)        return Out::focus(a.node_id);
            else if constexpr (std::is_same_v<T, Screenshot>)   return Out::screenshot(a.path);
            else return Out::none();
        }, alt_);
    }

  private:
    Alt alt_;
};

}  // namespace mayag
