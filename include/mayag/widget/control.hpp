#pragma once
// mayag::widget control — what every interactive widget shares
//
// The rules the controls in this folder follow:
//
//  1. NO HIDDEN STATE. A control draws a value it is GIVEN and names the node
//     the user will interact with. What a click or drag MEANS is decided in
//     the program's update(), so every value lives in the model where it can
//     be saved, undone and asserted on. Each control documents its
//     interaction contract in its own header.
//
//  2. INTERACTION FEEDBACK IS READ, NOT STORED. Hover, press and focus come
//     from the frame context (`c.hovered(id)` ...), which the host derives
//     from the laid-out tree. Controls are templated on that context so the
//     widget layer does not depend on the app layer; mayag::Ctx satisfies it,
//     and an app can pass its own (e.g. one carrying a different theme).
//
// Ids: a control named "font" owns node_id("font"); its parts are
// part_id("font", i), matched in update() with `is(e, "font", i)`.

#include "../dsl/dsl.hpp"
#include "../style/theme.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mayag::dsl {

/// What a control reads from a frame context. mayag::Ctx models it.
template <class C>
concept ControlContext = requires(const C& c, std::uint64_t id) {
    { c.theme } -> std::convertible_to<const Theme&>;
    { c.hovered(id) } -> std::same_as<bool>;
    { c.pressed(id) } -> std::same_as<bool>;
    { c.focused(id) } -> std::same_as<bool>;
};

/// The part ids of a compound control: `part_id("theme", 2)` is the third
/// segment of the "theme" segmented control, the third row of a radio group,
/// the third option of a dropdown.
[[nodiscard]] constexpr std::uint64_t part_id(std::string_view base, std::size_t i) noexcept {
    return node_id(base, i);
}
/// Parts that are not an option index (a stepper's buttons, a dropdown's
/// menu). Far above any option count, so they can never collide.
inline constexpr std::size_t part_dec  = 1u << 20;
inline constexpr std::size_t part_inc  = part_dec + 1;
inline constexpr std::size_t part_menu = part_dec + 2;

namespace detail {

/// The edge a control wears: the focus ring when focused (a settings form is
/// used with both keyboard and mouse, and the ring is the honest answer to
/// "what will Space toggle?"), a stronger border on hover, else `rest`.
[[nodiscard]] inline Color<Srgb> edge(const Theme& t, bool focused, bool hovered,
                                      Color<Srgb> rest) {
    if (focused) return t.focus_ring;
    if (hovered) return t.border_strong;
    return rest;
}

}  // namespace detail

}  // namespace mayag::dsl
