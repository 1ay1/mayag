#pragma once
// mayag/app/gestures.hpp — what the pointer and focus did, against the laid-out tree.
//
// Plain data, no runtime: widgets attach handlers to these (handlers.hpp)
// and programs receive them in on(model, gesture). The window host derives
// them (app/host.hpp); nothing here knows that host exists.

#include "../core/event.hpp"
#include "../core/geometry.hpp"

#include <cstdint>

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
    Mods          mods{};         ///< held at the press
};
struct Activated { std::uint64_t node = 0; };
/// `count` is the press's place in a multi-click sequence (2 on the second
/// press of a double click), known AT the press -- so an editor can select
/// the word on the second press, as every editor does, not on its release.
struct Pressed  { std::uint64_t node = 0; Vec2 local{}; MouseButton button = MouseButton::left;
                  int count = 1; Mods mods{}; };
struct Released { std::uint64_t node = 0; Vec2 local{}; MouseButton button = MouseButton::left; };
struct Entered  { std::uint64_t node = 0; };
struct Left     { std::uint64_t node = 0; };
struct Dragged  { std::uint64_t node = 0; Vec2 local{}; Vec2 delta{}; };
struct Scrolled { std::uint64_t node = 0; Vec2 local{}; Vec2 delta{}; };

}  // namespace mayag
