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
#include "../core/sync.hpp"   // Vec2 / Rect / Color are Sendable and Frozen
#include "../core/event.hpp"
#include "gestures.hpp"
#include "handlers.hpp"
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

// ── child windows ─────────────────────────────────────────────────────────
//
// A program can own more windows than its main one -- a settings window, an
// inspector, a detached panel. They are DECLARED, like everything else that
// follows from the model:
//
//     static std::vector<mayag::WindowSpec> windows(const Model& m) {
//         if (!m.settings_open) return {};
//         return {{.id = kSettings, .title = "Settings", .size = {860, 620}}};
//     }
//     static Node window_view(const Model& m, std::uint64_t id);   // its content
//     static std::optional<Msg> on(const Model&, const mayag::WindowEvent&);
//
// The host opens a window when its id appears, retitles it when the title
// changes and closes it when the id goes away. Each child is a full
// surface: its own layout, hover, overlays (menus open INSIDE it), inline
// handlers and transitions, drawn in the program's theme. Gestures in it
// (Clicked, Pressed, Dragged, Left, ...) arrive exactly like the main
// window's -- node ids are the program's own -- and its RAW input (keys,
// text, scrolls, resize, focus, the close button) arrives wrapped in a
// WindowEvent naming the window. The close button only reports; the window
// goes when the program stops declaring it.

/// One child window, as the program declares it.
struct WindowSpec {
    std::uint64_t id = 0;          ///< the program's name for it (non-zero, stable)
    std::string   title;
    Vec2          size{820.0f, 600.0f};   ///< initial logical size
    bool          resizable = true;
    friend bool operator==(const WindowSpec&, const WindowSpec&) = default;
};

/// Raw input from a child window: `event` is any Event (KeyEvent,
/// TextEvent, ScrollEvent, MouseMove, ResizeEvent, FocusEvent,
/// CloseRequest ...), `window` the WindowSpec id it came from.
struct WindowEvent {
    std::uint64_t window = 0;
    Event         event;
};

/// Everything the window host emits. Raw input (KeyEvent, TextEvent,
/// MouseMove, ...) comes through as-is; gestures come after the raw event
/// that caused them.
using window_events = std::variant<
    KeyEvent, TextEvent, ComposeEvent, ComposeEndEvent,
    MouseMove, MouseDown, MouseUp, ScrollEvent,
    ResizeEvent, FocusEvent, CloseRequest,
    Clicked, Activated, Pressed, Released, Entered, Left, Dragged, Scrolled, WindowEvent>;

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

/// An OS signal, delivered to `on(const Model&, Signal)`: interrupt,
/// terminate, hangup, resize, child.
using Signal = jaal::sig;

/// Proof that the current thread holds no lock. A task or stream body may
/// take `const Unlocked&` as its first parameter; the runtime passes one,
/// and blocking waits (locks, joins) require it.
using Unlocked = jaal::unlocked;

/// State several threads use at once, owned jointly (every field must be
/// safe to share). The one way task bodies share mutable state.
template <class T>
using CoOwned = jaal::co_owned<T>;

// Sendable / Shareable / Sync live in core/sync.hpp (lower layers state
// their own proofs with them).

/// Data reachable only under its lock, in a lock order the compiler
/// checks: `Guarded<Cache>` (a leaf lock), or `Guarded<Cache, Rank<20,
/// "cache">>` to nest it under higher-ranked locks.
template <std::uint16_t V, jaal::meta::fixed_string Name>
using Rank = jaal::rank<V, Name>;
template <class T, class R = jaal::leaf>
using Guarded = jaal::guarded<T, R>;

/// A custom effect with no message in it, for your own Cmd:
///     struct Beep { int hz; };
///     using beep = mayag::Effect<Beep, "beep">;
///     using Cmd  = mayag::Cmd<Msg, beep>;
template <class Payload, jaal::meta::fixed_string Name>
using Effect = jaal::pure_fx<Payload, Name>;

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

/// An Edit owns its callable (cloned on copy), but the callable's CAPTURES are
/// invisible to jaal: built on a worker, `[&]` could point at the worker's
/// stack. So an Edit may sit in a Msg and be folded on the loop that made it
/// (an input gesture, a test), but it is LOOP-ONLY: no Sink carries one --
/// sending it from a task or stream does not compile, and sending a Msg
/// that holds one aborts on the spot. A task sends a plain message instead.
/// (No Sendable claim: an Edit is NOT Sendable -- it can't be a task argument
/// or be shared. Being loop-only is what lets it sit in a Msg: jaal::Postable.)
template <class Model>
inline constexpr bool jaal::loop_only<mayag::Edit<Model>> = true;


// mayag's geometry and colour are plain values (floats, no pointers), so a
// Msg may carry them across threads. jaal can't see inside a class with
// constructors, hence the opt-ins.
// (Vec2, Rect and Color are declared Sendable and Frozen in core/sync.hpp,
// beside the types, so every layer that shares them sees the claim.)

/// Declare that a type of YOURS may travel between threads in a Msg or a
/// task argument (it owns everything it holds), at global scope:
///     MAYAG_SENDABLE(my::Handle);
/// Every use is a claim a human made; keep them rare and say why beside it.
#define MAYAG_SENDABLE(...) \
    template <> inline constexpr bool jaal::sendable_opt_in<__VA_ARGS__> = true
/// Declare a type of yours immutable once built, so it may be shared without
/// a copy (`Shared<T>`):  MAYAG_FROZEN(my::Glyphs);
#define MAYAG_FROZEN(...) \
    template <> inline constexpr bool jaal::frozen_opt_in<__VA_ARGS__> = true
/// Declare that every member function of a type of yours is safe to call from
/// several threads at once, so threads may share ONE of it (`CoOwned<T>`):
///     MAYAG_SYNC(my::Wakeup);   // signal() is one syscall; only its owner waits
/// The strongest claim of the three: say beside it why it holds.
#define MAYAG_SYNC(...) \
    template <> inline constexpr bool jaal::sync_opt_in<__VA_ARGS__> = true

template <>
struct std::hash<mayag::fx::on_frame::key_type> {
    std::size_t operator()(const mayag::fx::on_frame::key_type&) const noexcept { return 0; }
};
