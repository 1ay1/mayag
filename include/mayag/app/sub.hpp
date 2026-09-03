#pragma once
// mayag::Sub<Msg> — event subscriptions as data
//
// `Cmd` describes effects going OUT. `Sub` describes events coming IN, as a
// pure function of the current state:
//
//     static auto subscribe(const Model& m) -> Sub<Msg> {
//         return Sub<Msg>::batch(
//             Sub<Msg>::on_key(Key::escape, Close{}),
//             Sub<Msg>::on_click<"save-btn">(Save{}),
//             when(m.animating, Sub<Msg>::every_frame([](FrameEvent f) {
//                 return Tick{f.delta};
//             })));
//     }
//
// Because subscriptions are a function of the model, they are DECLARATIVE:
// when `m.animating` goes false the frame subscription simply stops existing,
// and the runtime stops driving the display link. You never write
// `start_timer()` / `stop_timer()` and never leak one.
//
// The GPU-UI-specific part is `on_click<"id">` and friends: widget-level
// subscriptions that reference a node by name. The runtime does the hit
// testing and the press/release pairing, so your `update()` sees a single
// semantic `Save{}` instead of a stream of coordinates.

#include "event.hpp"
#include "../dsl/dsl.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mayag {

template <typename Msg>
class Sub {
  public:
    // ── alternatives ────────────────────────────────────────────────────

    struct None {};

    struct Batch { std::vector<Sub> subs; };

    /// Raw event tap — the escape hatch. Returns nullopt to ignore.
    struct OnEvent { std::function<std::optional<Msg>(const Event&)> handler; };

    /// A specific key chord.
    struct OnKey {
        Key  key = Key::unknown;
        Mods mods{};
        Msg  msg;
    };

    /// Any key, decoded by the handler.
    struct OnAnyKey { std::function<std::optional<Msg>(const KeyEvent&)> handler; };

    /// Committed text (typing, IME, paste).
    struct OnText { std::function<std::optional<Msg>(std::string_view)> handler; };

    /// Wall-clock interval.
    struct Every {
        std::chrono::milliseconds interval{};
        Msg msg;
    };

    /// Every display refresh. This is what drives animation, and the runtime
    /// only runs a display link while such a subscription exists.
    struct EveryFrame { std::function<Msg(FrameEvent)> handler; };

    // ── widget-level (hit-tested by the runtime) ────────────────────────

    enum class Gesture : std::uint8_t {
        click, press, release, enter, leave, drag, scroll,
    };

    struct OnNode {
        std::uint64_t node_id = 0;
        Gesture       gesture = Gesture::click;
        Msg           msg;

        /// Which click counts this subscription accepts.
        ///
        /// `on_click` matches a SINGLE click only, so a double click does not
        /// also fire the single-click action — the bug the old
        /// click-then-double_click pair invited. `on_any_click` opts back in
        /// to "fired regardless of count" for the common case where a button
        /// should activate however eagerly it was clicked.
        int  count = 1;
        bool any_count = false;

        [[nodiscard]] constexpr bool accepts(int n) const noexcept {
            return any_count || n == count;
        }
    };

    /// Widget gesture with the pointer payload — for drags and scrolls.
    struct OnNodeMotion {
        std::uint64_t node_id = 0;
        Gesture       gesture = Gesture::drag;
        std::function<Msg(Vec2)> handler;
    };

    struct OnResize { std::function<Msg(Vec2)> handler; };
    struct OnClose  { Msg msg; };
    struct OnFocusChange { std::function<Msg(bool)> handler; };

    using Alt = std::variant<None, Batch, OnEvent, OnKey, OnAnyKey, OnText,
                             Every, EveryFrame, OnNode, OnNodeMotion,
                             OnResize, OnClose, OnFocusChange>;

    // ── construction ────────────────────────────────────────────────────

    Sub() : alt_{None{}} {}
    explicit Sub(Alt a) : alt_{std::move(a)} {}

    [[nodiscard]] static Sub none() { return Sub{}; }

    template <typename F>
    [[nodiscard]] static Sub on_event(F&& f) {
        return Sub{Alt{OnEvent{std::forward<F>(f)}}};
    }

    [[nodiscard]] static Sub on_key(Key k, Msg m) {
        return Sub{Alt{OnKey{k, Mods{}, std::move(m)}}};
    }
    [[nodiscard]] static Sub on_key(Key k, Mods mods, Msg m) {
        return Sub{Alt{OnKey{k, mods, std::move(m)}}};
    }
    /// Platform-primary accelerator (Cmd on macOS, Ctrl elsewhere).
    [[nodiscard]] static Sub on_shortcut(Key k, Msg m) {
        Mods mods{};
#if defined(__APPLE__)
        mods.super = true;
#else
        mods.ctrl = true;
#endif
        return Sub{Alt{OnKey{k, mods, std::move(m)}}};
    }

    template <typename F>
    [[nodiscard]] static Sub on_any_key(F&& f) {
        return Sub{Alt{OnAnyKey{std::forward<F>(f)}}};
    }

    template <typename F>
    [[nodiscard]] static Sub on_text(F&& f) {
        return Sub{Alt{OnText{std::forward<F>(f)}}};
    }

    [[nodiscard]] static Sub every(std::chrono::milliseconds i, Msg m) {
        return Sub{Alt{Every{i, std::move(m)}}};
    }

    template <typename F>
    [[nodiscard]] static Sub every_frame(F&& f) {
        return Sub{Alt{EveryFrame{std::forward<F>(f)}}};
    }

    // -- widget subscriptions, by compile-time name --

    /// `Sub<Msg>::on_click<"save-btn">(Save{})` — the id is hashed at compile
    /// time with the same FNV-1a the DSL's `id<"save-btn">` uses, so the two
    /// cannot drift.
    template <dsl::fixed_string Name>
    [[nodiscard]] static Sub on_click(Msg m) {
        return Sub{Alt{OnNode{dsl::node_id(Name.view()), Gesture::click, std::move(m)}}};
    }
    /// A double click, and ONLY a double click.
    template <dsl::fixed_string Name>
    [[nodiscard]] static Sub on_double_click(Msg m) {
        return Sub{Alt{OnNode{dsl::node_id(Name.view()), Gesture::click, std::move(m), 2, false}}};
    }

    /// A triple click — select-line in a text field, select-paragraph in an
    /// editor. Falls out of the count model for free.
    template <dsl::fixed_string Name>
    [[nodiscard]] static Sub on_triple_click(Msg m) {
        return Sub{Alt{OnNode{dsl::node_id(Name.view()), Gesture::click, std::move(m), 3, false}}};
    }

    /// An exact click count, for anything deeper.
    template <dsl::fixed_string Name>
    [[nodiscard]] static Sub on_multi_click(int count, Msg m) {
        return Sub{Alt{OnNode{dsl::node_id(Name.view()), Gesture::click, std::move(m),
                              count, false}}};
    }

    /// Fires on a click of ANY count.
    ///
    /// What a plain button wants: clicking it twice quickly should activate
    /// it twice, not activate once and then silently drop the second.
    template <dsl::fixed_string Name>
    [[nodiscard]] static Sub on_any_click(Msg m) {
        return Sub{Alt{OnNode{dsl::node_id(Name.view()), Gesture::click, std::move(m), 0, true}}};
    }
    template <dsl::fixed_string Name>
    [[nodiscard]] static Sub on_press(Msg m) {
        return Sub{Alt{OnNode{dsl::node_id(Name.view()), Gesture::press, std::move(m)}}};
    }
    template <dsl::fixed_string Name>
    [[nodiscard]] static Sub on_release(Msg m) {
        return Sub{Alt{OnNode{dsl::node_id(Name.view()), Gesture::release, std::move(m)}}};
    }
    template <dsl::fixed_string Name>
    [[nodiscard]] static Sub on_enter(Msg m) {
        return Sub{Alt{OnNode{dsl::node_id(Name.view()), Gesture::enter, std::move(m)}}};
    }
    template <dsl::fixed_string Name>
    [[nodiscard]] static Sub on_leave(Msg m) {
        return Sub{Alt{OnNode{dsl::node_id(Name.view()), Gesture::leave, std::move(m)}}};
    }

    /// Drag: the handler receives the cursor position in the node's local
    /// coordinates, which is what a slider or a canvas actually wants.
    template <dsl::fixed_string Name, typename F>
    [[nodiscard]] static Sub on_drag(F&& f) {
        return Sub{Alt{OnNodeMotion{dsl::node_id(Name.view()), Gesture::drag,
                                    std::forward<F>(f)}}};
    }
    template <dsl::fixed_string Name, typename F>
    [[nodiscard]] static Sub on_scroll(F&& f) {
        return Sub{Alt{OnNodeMotion{dsl::node_id(Name.view()), Gesture::scroll,
                                    std::forward<F>(f)}}};
    }

    /// Runtime-computed id, for lists whose items are not known at compile time.
    [[nodiscard]] static Sub on_click_id(std::uint64_t id, Msg m) {
        return Sub{Alt{OnNode{id, Gesture::click, std::move(m), 1, false}}};
    }
    [[nodiscard]] static Sub on_any_click_id(std::uint64_t id, Msg m) {
        return Sub{Alt{OnNode{id, Gesture::click, std::move(m), 0, true}}};
    }

    template <typename F>
    [[nodiscard]] static Sub on_resize(F&& f) {
        return Sub{Alt{OnResize{std::forward<F>(f)}}};
    }
    [[nodiscard]] static Sub on_close(Msg m) {
        return Sub{Alt{OnClose{std::move(m)}}};
    }
    template <typename F>
    [[nodiscard]] static Sub on_focus_change(F&& f) {
        return Sub{Alt{OnFocusChange{std::forward<F>(f)}}};
    }

    [[nodiscard]] static Sub batch(std::vector<Sub> subs) {
        std::vector<Sub> kept;
        kept.reserve(subs.size());
        for (auto& s : subs) {
            if (s.is_none()) continue;
            if (auto* b = std::get_if<Batch>(&s.alt_)) {
                for (auto& inner : b->subs) kept.push_back(std::move(inner));
            } else {
                kept.push_back(std::move(s));
            }
        }
        if (kept.empty()) return none();
        return Sub{Alt{Batch{std::move(kept)}}};
    }

    template <typename... Ss>
    [[nodiscard]] static Sub batch(Ss&&... ss) {
        std::vector<Sub> v;
        v.reserve(sizeof...(Ss));
        (v.push_back(std::forward<Ss>(ss)), ...);
        return batch(std::move(v));
    }

    // ── queries ─────────────────────────────────────────────────────────

    [[nodiscard]] bool is_none() const noexcept {
        return std::holds_alternative<None>(alt_);
    }
    [[nodiscard]] const Alt& alternative() const noexcept { return alt_; }

    /// True if anything here needs a per-refresh callback. The runtime uses
    /// this to decide between a display link and blocking on the event queue
    /// — which is the difference between 0% CPU idle and a spinning app.
    [[nodiscard]] bool wants_frames() const {
        return std::visit([](const auto& a) -> bool {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, EveryFrame>) return true;
            else if constexpr (std::is_same_v<T, Batch>) {
                for (const auto& s : a.subs) if (s.wants_frames()) return true;
                return false;
            }
            else return false;
        }, alt_);
    }

    /// Shortest timer interval requested, if any.
    [[nodiscard]] std::optional<std::chrono::milliseconds> min_interval() const {
        return std::visit([](const auto& a) -> std::optional<std::chrono::milliseconds> {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, Every>) return a.interval;
            else if constexpr (std::is_same_v<T, Batch>) {
                std::optional<std::chrono::milliseconds> best;
                for (const auto& s : a.subs) {
                    if (auto i = s.min_interval()) {
                        best = best ? std::min(*best, *i) : *i;
                    }
                }
                return best;
            }
            else return std::nullopt;
        }, alt_);
    }

    // ── functor ─────────────────────────────────────────────────────────

    template <typename F>
    [[nodiscard]] auto map(F f) const -> Sub<std::invoke_result_t<F, Msg>> {
        using Out = Sub<std::invoke_result_t<F, Msg>>;
        return std::visit([&f](const auto& a) -> Out {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, None>) return Out::none();
            else if constexpr (std::is_same_v<T, Batch>) {
                std::vector<Out> mapped;
                mapped.reserve(a.subs.size());
                for (const auto& s : a.subs) mapped.push_back(s.map(f));
                return Out::batch(std::move(mapped));
            }
            else if constexpr (std::is_same_v<T, OnEvent>)
                return Out::on_event([h = a.handler, f](const Event& e) -> std::optional<decltype(f(std::declval<Msg>()))> {
                    if (auto m = h(e)) return f(*m);
                    return std::nullopt;
                });
            else if constexpr (std::is_same_v<T, OnKey>)
                return Out::on_key(a.key, a.mods, f(a.msg));
            else if constexpr (std::is_same_v<T, OnAnyKey>)
                return Out::on_any_key([h = a.handler, f](const KeyEvent& k) -> std::optional<decltype(f(std::declval<Msg>()))> {
                    if (auto m = h(k)) return f(*m);
                    return std::nullopt;
                });
            else if constexpr (std::is_same_v<T, OnText>)
                return Out::on_text([h = a.handler, f](std::string_view s) -> std::optional<decltype(f(std::declval<Msg>()))> {
                    if (auto m = h(s)) return f(*m);
                    return std::nullopt;
                });
            else if constexpr (std::is_same_v<T, Every>)
                return Out::every(a.interval, f(a.msg));
            else if constexpr (std::is_same_v<T, EveryFrame>)
                return Out::every_frame([h = a.handler, f](FrameEvent e) { return f(h(e)); });
            else if constexpr (std::is_same_v<T, OnNode>)
                return Out::on_click_id_gesture(a.node_id, a.gesture, f(a.msg),
                                                a.count, a.any_count);
            else if constexpr (std::is_same_v<T, OnNodeMotion>)
                return Out::on_node_motion(a.node_id, a.gesture,
                                           [h = a.handler, f](Vec2 v) { return f(h(v)); });
            else if constexpr (std::is_same_v<T, OnResize>)
                return Out::on_resize([h = a.handler, f](Vec2 v) { return f(h(v)); });
            else if constexpr (std::is_same_v<T, OnClose>)
                return Out::on_close(f(a.msg));
            else if constexpr (std::is_same_v<T, OnFocusChange>)
                return Out::on_focus_change([h = a.handler, f](bool b) { return f(h(b)); });
            else return Out::none();
        }, alt_);
    }

    // Internal constructors used by map().
    [[nodiscard]] static Sub on_click_id_gesture(std::uint64_t id, Gesture g, Msg m,
                                                 int count = 1, bool any = false) {
        return Sub{Alt{OnNode{id, g, std::move(m), count, any}}};
    }
    template <typename F>
    [[nodiscard]] static Sub on_node_motion(std::uint64_t id, Gesture g, F&& f) {
        return Sub{Alt{OnNodeMotion{id, g, std::forward<F>(f)}}};
    }

  private:
    Alt alt_;
};

/// `when(cond, sub)` — conditional subscription. Reads better than a ternary
/// and keeps `subscribe()` a single expression.
template <typename Msg>
[[nodiscard]] Sub<Msg> when(bool cond, Sub<Msg> s) {
    return cond ? std::move(s) : Sub<Msg>::none();
}

}  // namespace mayag
