#pragma once
// platform/raw_input.hpp — the seat as the platform delivered it.
//
// Shared by every window backend that can tap input below mayag events
// (Wayland, macOS): key presses AND releases by evdev code, modifier state,
// pointer axis streams. Opt-in per window (set_raw_input).

#include <cstdint>
#include <string>
#include <variant>

#include "../core/event.hpp"

namespace mayag::platform {

// mayag's events are an APP's view of input: key presses with text, one
// coalesced scroll per frame, three mouse buttons. A program that forwards
// input to someone else (a nested compositor, a remote desktop) needs the
// seat itself: every key press AND release by evdev code, the exact xkb
// modifier masks and the keymap they index, every pointer motion at its
// sub-pixel position, every button, and scroll as the axis stream it is
// (smooth, discrete, value120, stop). That is this tap. It is opt-in
// (WaylandWindow::set_raw_input) and costs nothing when unset; mayag's own
// events are delivered exactly as before alongside it.
//
// Fixed-point values are wl_fixed (24.8) raw, as the protocol has them, so
// nothing is lost or re-rounded on the way through.
namespace raw {
struct Key {
    std::uint32_t time;
    std::uint32_t code;     ///< evdev
    bool pressed;
    mayag::Key key;         ///< what mayag would report (keysym-resolved)
    Mods mods;              ///< effective modifiers at this key
};
struct Modifiers { std::uint32_t depressed, latched, locked, group; };
struct Keymap { std::string text; };                   ///< XKB_V1 text
struct Repeat { std::int32_t rate, delay; };
struct KeyboardFocus { bool in; };
struct Motion { std::uint32_t time; std::int32_t x, y; };   ///< surface-local, logical, wl_fixed
struct Button { std::uint32_t time, button; bool pressed; };  ///< evdev BTN_*
struct Axis { std::uint32_t time, axis; std::int32_t value; };  ///< wl_fixed
struct AxisValue120 { std::uint32_t axis; std::int32_t value120; };
struct AxisSource { std::uint32_t source; };
struct AxisStop { std::uint32_t time, axis; };
struct Frame {};
struct PointerFocus { bool in; };
}  // namespace raw
using RawInput = std::variant<raw::Key, raw::Modifiers, raw::Keymap, raw::Repeat, raw::KeyboardFocus, raw::Motion,
                              raw::Button, raw::Axis, raw::AxisValue120, raw::AxisSource, raw::AxisStop, raw::Frame,
                              raw::PointerFocus>;

}  // namespace mayag::platform
