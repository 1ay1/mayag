#pragma once
// mayag/app/sources.hpp — what the window REPORTS, and the frame clock.
//
// Input is not a subscription in jaal. A program handles window input with
// one on() overload per kind it cares about, reading the model as it is when
// the event arrives:
//
//     static std::optional<Msg> on(const Model&, const KeyEvent&);
//     static std::optional<Msg> on(const Model&, const Clicked&);
//
// The window host turns raw pointer input into gestures (hover, press,
// click, drag) against the laid-out tree before the program sees it, so a
// button needs one on(const Clicked&) and an id compare, not coordinates.
//
// The frame clock IS a subscription: `on_frame(f)` asks for one Msg per
// display refresh while subscribe() returns it, and nothing otherwise. jaal
// starts and stops it like any source; the host paces it to the display. An
// app that returns no on_frame draws nothing and sits at 0% CPU.

#include "../core/color.hpp"
#include "../core/geometry.hpp"
#include "event.hpp"
#include "interaction.hpp"

#include <jaal/jaal.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mayag {

// ── gestures, as events ─────────────────────────────────────────────────
// What the host derives from the pointer and focus against the laid-out
// tree. Each names the node it happened on. `count` on Clicked is 1 for a
// single click, 2 for a double, ... Enter/Space on the focused node is
// Activated, not Clicked: a program opts into keyboard activation by
// handling it, and the key falls through to on(KeyEvent) when nothing does.

struct Clicked {
    std::uint64_t node = 0;
    Vec2          local{};        ///< relative to the node's frame
    int           count = 1;
    MouseButton   button = MouseButton::left;
};
struct Activated { std::uint64_t node = 0; };
struct Pressed  { std::uint64_t node = 0; Vec2 local{}; MouseButton button = MouseButton::left; };
struct Released { std::uint64_t node = 0; Vec2 local{}; MouseButton button = MouseButton::left; };
struct Entered  { std::uint64_t node = 0; };
struct Left     { std::uint64_t node = 0; };
struct Dragged  { std::uint64_t node = 0; Vec2 local{}; Vec2 delta{}; };
struct Scrolled { std::uint64_t node = 0; Vec2 local{}; Vec2 delta{}; };

/// Everything the window host emits. Raw input (KeyEvent, TextEvent,
/// MouseMove, ...) comes through as-is; gestures come after the raw event
/// that caused them.
using window_events = std::variant<
    KeyEvent, TextEvent, ComposeEvent, ComposeEndEvent,
    MouseMove, MouseDown, MouseUp, ScrollEvent,
    ResizeEvent, FocusEvent, CloseRequest,
    Clicked, Activated, Pressed, Released, Entered, Left, Dragged, Scrolled>;

// ── the frame clock ─────────────────────────────────────────────────────

namespace fx {

namespace detail {
// The parameter type of a non-generic lambda, so a `[](float dt)` frame
// handler gets a float instead of an implicit (warning) narrowing.
template <class M> struct arg_of { using type = double; };
template <class R, class C, class A> struct arg_of<R (C::*)(A) const> { using type = A; };
template <class F> struct frame_arg { using type = double; };
template <class F> requires requires { &F::operator(); }
struct frame_arg<F> { using type = std::remove_cvref_t<typename arg_of<decltype(&F::operator())>::type>; };
}  // namespace detail

/// One Msg per display refresh, carrying the seconds since the last one
/// (clamped to 0.1 so a stall can't blow up a spring). Key: none — there is
/// one frame clock, and two on_frame in a subscribe() are one.
struct on_frame {
    static constexpr std::string_view name = "on_frame";
    template <class Msg> struct type {
        std::function<Msg(double)> make;   // runs on the loop thread only
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        return {[f = std::forward<F>(f), g = std::move(e.make)](double dt) { return f(g(dt)); }};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, type<M> e)
        -> type<std::invoke_result_t<F, const Id&, M>> {
        return {[id, f = std::forward<F>(f), g = std::move(e.make)](double dt) {
            return f(id, g(dt));
        }};
    }

    struct key_type {
        bool operator==(const key_type&) const = default;
    };
    template <class M>
    static key_type key(const type<M>&) { return {}; }

    template <class Self, class Msg> struct ctors {
        /// `Sub::on_frame([](float dt) { return Tick{dt}; })`. Captureless:
        /// jaal keeps a running source across subscribe() calls without
        /// refreshing its payload, so a captured value would go stale.
        template <class F>
            requires std::is_empty_v<F> && std::default_initializable<F>
                  && std::is_invocable_v<F, typename detail::frame_arg<F>::type>
                  && std::is_convertible_v<std::invoke_result_t<F, typename detail::frame_arg<F>::type>, Msg>
        [[nodiscard]] static Self on_frame(F) {
            using A = typename detail::frame_arg<F>::type;
            return Self(type<Msg>{[](double dt) -> Msg { return Msg(F{}(static_cast<A>(dt))); }});
        }
    };
};

}  // namespace fx

using on_frame = fx::on_frame;

/// A program's Sub with the frame clock in it.
template <class Msg, class... Extra>
using Sub = jaal::Sub<Msg, on_frame, Extra...>;

// ── the rest of the program vocabulary ──────────────────────────────────
// Apps spell everything through mayag; jaal stays an implementation detail.

/// A program's effects: jaal's core (quit, after, task, perform, ...) plus the
/// window effects it lists, e.g. `mayag::Cmd<Msg, set_title, upload_image>`.
template <class Msg, class... Extra>
using Cmd = jaal::Cmd<Msg, Extra...>;

/// How a task or stream body talks back to the program (thread safe, weak).
template <class Msg>
using Sink = jaal::Sink<Msg>;

/// An immutable value shared across threads without a copy (decoded pixels).
template <class T>
using Shared = jaal::shared<T>;

/// A key for Cmd::task / Cmd::perform: the newest task with a key replaces
/// the running one.
using TaskKey = jaal::task_key;

/// `when(cond, sub)` keeps subscribe() one expression.
template <class S>
[[nodiscard]] S when(bool cond, S s) {
    return cond ? std::move(s) : S::none();
}

/// A key table for on(const Model&, const KeyEvent&):
///
///     static const keymap<Msg> keys{{Key::space, Toggle{}}, {Key::escape, Quit{}}};
///     return keys(k);
///
/// A row matches only its exact modifiers (none by default), so Ctrl+Space
/// doesn't fire the Space binding.
template <class Msg>
class keymap {
  public:
    struct row { Key key; Msg msg; Mods mods{}; };
    keymap(std::initializer_list<row> rows) : rows_(rows) {}

    [[nodiscard]] std::optional<Msg> operator()(const KeyEvent& k) const {
        for (const auto& r : rows_)
            if (r.key == k.key && r.mods == k.mods) return r.msg;
        return std::nullopt;
    }

  private:
    std::vector<row> rows_;
};

}  // namespace mayag

// mayag's geometry and colour are plain values (floats, no pointers), so a
// Msg may carry them across threads. jaal can't see inside a class with
// constructors, hence the opt-ins.
template <> inline constexpr bool jaal::sendable_opt_in<mayag::Vec2> = true;
template <> inline constexpr bool jaal::frozen_opt_in<mayag::Vec2>   = true;
template <> inline constexpr bool jaal::sendable_opt_in<mayag::Rect> = true;
template <> inline constexpr bool jaal::frozen_opt_in<mayag::Rect>   = true;
template <class S> inline constexpr bool jaal::sendable_opt_in<mayag::Color<S>> = true;
template <class S> inline constexpr bool jaal::frozen_opt_in<mayag::Color<S>>   = true;

template <>
struct std::hash<mayag::fx::on_frame::key_type> {
    std::size_t operator()(const mayag::fx::on_frame::key_type&) const noexcept { return 0; }
};
