#pragma once
// mayag::handlers — events where they happen
//
// Say what an element DOES next to what it IS. No ids, no on(Clicked)
// branch somewhere else, no string matching:
//
//     h(text<"Save">) | button_look | on_click(Save{})
//     slider(c, "vol", m.vol, 120) | on_slide([](Vec2 p) { return SetVol{p.x / 120}; })
//     row | on_click([i](const Clicked& c) { return c.count == 2 ? Open{i} : Select{i}; })
//     card | on_enter(Peek{i}) | on_leave(Unpeek{})
//
// A handler is either a MESSAGE (sent as-is) or a FUNCTION of the gesture
// returning a message, a std::optional of one (empty: nothing), or the
// program's Msg itself. The message must be one of the program's Msg
// alternatives; the host matches it by type and dispatches it into update()
// on the same turn as the gesture.
//
// Elements with handlers become controls automatically: a stable id is
// assigned if they have none (so they hover, take focus, and answer Enter
// and Space like a click), and a click handler shows the pointer cursor.
// on(Clicked) still sees every gesture; handlers are purely additive.

#include "../dsl/dsl.hpp"
#include "sources.hpp"

#include <algorithm>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

namespace mayag {

namespace detail::handlers {

template <class T> inline constexpr bool is_optional = false;
template <class T> inline constexpr bool is_optional<std::optional<T>> = true;

template <class R>
[[nodiscard]] HandlerResult wrap(R&& r) {
    using T = std::remove_cvref_t<R>;
    if constexpr (is_optional<T>) {
        if (!r) return {};
        return wrap(*std::forward<R>(r));
    } else {
        return {std::make_shared<const T>(std::forward<R>(r)), &type_tag<T>};
    }
}

/// Call `f` with the gesture if it takes one, else with nothing; a plain
/// value (not callable) IS the message.
template <class F, class E>
[[nodiscard]] HandlerResult call(const void* fn, const void* event) {
    const F& f = *static_cast<const F*>(fn);
    const E& e = *static_cast<const E*>(event);
    if constexpr (std::invocable<const F&, const E&>) return wrap(f(e));
    else if constexpr (std::invocable<const F&>) return wrap(f());
    else return wrap(f);
}

template <class F, class E>
concept HandlerFor = std::invocable<const F&, const E&> || std::invocable<const F&> ||
                     (!std::invocable<const F&> && std::copy_constructible<F>);

}  // namespace detail::handlers

namespace dsl {

/// An inline handler, piped like any responder.
template <On K, class E, class F>
struct OnMod {
    static constexpr bool is_responder = true;
    static constexpr caps::Set gives = caps::none;
    template <caps::Set>
    static consteval void check() {}

    F f;

    void on_pipe(Style& s) const {
        // Clickable things LOOK clickable.
        if constexpr (K == On::click || K == On::press)
            if (s.cursor == CursorShape::arrow) s.cursor = CursorShape::pointer;
    }
    void attach(Cascade& c, const std::shared_ptr<const OnMod>& self) const {
        c.handlers.push_back({K, &mayag::detail::handlers::call<F, E>, &self->f});
    }
};

/// Released over the element after pressing it (or Enter / Space while it
/// has focus).
template <class F> requires mayag::detail::handlers::HandlerFor<F, Clicked>
[[nodiscard]] OnMod<On::click, Clicked, F> on_click(F f) { return {std::move(f)}; }

/// Pointer pressed on the element.
template <class F> requires mayag::detail::handlers::HandlerFor<F, Pressed>
[[nodiscard]] OnMod<On::press, Pressed, F> on_press(F f) { return {std::move(f)}; }

/// Pointer released after pressing on the element (wherever it is now).
template <class F> requires mayag::detail::handlers::HandlerFor<F, Released>
[[nodiscard]] OnMod<On::release, Released, F> on_release(F f) { return {std::move(f)}; }

/// Pointer moved while pressed on the element (keeps reporting off it).
template <class F> requires mayag::detail::handlers::HandlerFor<F, Dragged>
[[nodiscard]] OnMod<On::drag, Dragged, F> on_drag(F f) { return {std::move(f)}; }

/// Wheel or trackpad over the element.
template <class F> requires mayag::detail::handlers::HandlerFor<F, Scrolled>
[[nodiscard]] OnMod<On::scroll, Scrolled, F> on_scroll(F f) { return {std::move(f)}; }

/// Pointer arrived over / left the element.
template <class F> requires mayag::detail::handlers::HandlerFor<F, Entered>
[[nodiscard]] OnMod<On::enter, Entered, F> on_enter(F f) { return {std::move(f)}; }
template <class F> requires mayag::detail::handlers::HandlerFor<F, Left>
[[nodiscard]] OnMod<On::leave, Left, F> on_leave(F f) { return {std::move(f)}; }

template <class F> requires std::invocable<const F&, Vec2>
struct SlideMod {
    static constexpr bool is_responder = true;
    static constexpr caps::Set gives = caps::none;
    template <caps::Set>
    static consteval void check() {}

    F f;

    void on_pipe(Style& s) const {
        if (s.cursor == CursorShape::arrow) s.cursor = CursorShape::pointer;
    }
    template <class E>
    static HandlerResult run(const void* fn, const void* event) {
        return mayag::detail::handlers::wrap((*static_cast<const F*>(fn))(static_cast<const E*>(event)->local));
    }
    void attach(Cascade& c, const std::shared_ptr<const SlideMod>& self) const {
        c.handlers.push_back({On::press, &run<Pressed>, &self->f});
        c.handlers.push_back({On::drag, &run<Dragged>, &self->f});
    }
};

/// A continuous control's whole interaction in one line: `f(local)` on
/// press AND on every drag, `local` relative to the element. Sliders,
/// scrubbers, colour pickers, splitters.
template <class F> requires std::invocable<const F&, Vec2>
[[nodiscard]] SlideMod<F> on_slide(F f) { return {std::move(f)}; }

}  // namespace dsl

// ── bindings: controls that edit the model with no Msg of their own ────────
//
// Forms are mostly "this control edits that field". Writing a message type,
// an update overload and a handler per field is ceremony, so:
//
//     using Msg = std::variant<Save, Edit<Model>>;            // opt in once
//     static Cmd update(Model& m, Edit<Model> e) { e(m); return {}; }
//
//     switch_control(c, "wifi", m.wifi)     | on_click(toggle(&Model::wifi))
//     slider(c, "vol", m.vol, 120)          | on_slide(set_from_x(&Model::vol, 120))
//     h(text<"Reset">)                     | on_click(set(&Model::vol, 0.5f))
//
// An Edit is still a MESSAGE folded by update(): the model changes in one
// place, replay and undo see every edit, and update() may veto or follow up
// (`e(m); if (m.vol > 1) ...`). It is just one message for all fields.

/// A model edit carried as a message. Call it on the model to apply.
///
/// Owns its edit (cloned on copy, never shared), so it is a plain value
/// that may cross threads like any Msg.
template <class Model>
class Edit {
  public:
    Edit() = default;
    template <class F>
        requires std::invocable<const F&, Model&> && (!std::same_as<std::remove_cvref_t<F>, Edit>)
    explicit Edit(F f) : p_(std::make_unique<impl<F>>(std::move(f))) {}
    Edit(const Edit& o) : p_(o.p_ ? o.p_->clone() : nullptr) {}
    Edit& operator=(const Edit& o) { p_ = o.p_ ? o.p_->clone() : nullptr; return *this; }
    Edit(Edit&&) noexcept = default;
    Edit& operator=(Edit&&) noexcept = default;

    void operator()(Model& m) const { if (p_) p_->apply(m); }

  private:
    struct base {
        virtual ~base() = default;
        virtual void apply(Model&) const = 0;
        [[nodiscard]] virtual std::unique_ptr<base> clone() const = 0;
    };
    template <class F>
    struct impl final : base {
        F f;
        explicit impl(F x) : f(std::move(x)) {}
        void apply(Model& m) const override { f(m); }
        [[nodiscard]] std::unique_ptr<base> clone() const override { return std::make_unique<impl>(f); }
    };
    std::unique_ptr<base> p_;
};

/// `field = value`.
template <class Model, class T, class V>
[[nodiscard]] auto set(T Model::*field, V value) {
    return [field, value] {
        return Edit<Model>{[field, value](Model& m) { m.*field = static_cast<T>(value); }};
    };
}
/// `field = !field`.
template <class Model>
[[nodiscard]] auto toggle(bool Model::*field) {
    return [field] { return Edit<Model>{[field](Model& m) { m.*field = !(m.*field); }}; };
}
/// For on_slide: `field = clamp(local.x / width, 0, 1)` mapped onto [lo, hi].
template <class Model, class T>
[[nodiscard]] auto set_from_x(T Model::*field, float width, float lo = 0.0f, float hi = 1.0f) {
    return [field, width, lo, hi](Vec2 local) {
        const float f = std::clamp(local.x / (width > 0.0f ? width : 1.0f), 0.0f, 1.0f);
        const T v = static_cast<T>(lo + f * (hi - lo));
        return Edit<Model>{[field, v](Model& m) { m.*field = v; }};
    };
}

}  // namespace mayag

/// An Edit owns its callable outright (cloned on copy), so moving one to
/// another thread shares nothing: safe as a message.
template <class Model>
inline constexpr bool jaal::sendable_opt_in<mayag::Edit<Model>> = true;

namespace mayag {

/// Turn a handler's message into the program's Msg, or nothing when the
/// type is not one of Msg's alternatives (a programming error the host
/// reports once).
template <class Msg>
[[nodiscard]] std::optional<Msg> to_program_msg(const HandlerResult& r) {
    if (r.msg == nullptr) return std::nullopt;
    if (r.type == &type_tag<Msg>) return *static_cast<const Msg*>(r.msg.get());
    if constexpr (requires { std::variant_size_v<Msg>; }) {
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            std::optional<Msg> out;
            (void)((r.type == &type_tag<std::variant_alternative_t<I, Msg>>
                        ? (out.emplace(std::in_place_index<I>,
                                       *static_cast<const std::variant_alternative_t<I, Msg>*>(r.msg.get())),
                           true)
                        : false) || ...);
            return out;
        }(std::make_index_sequence<std::variant_size_v<Msg>>{});
    } else {
        return std::nullopt;
    }
}

}  // namespace mayag
