#pragma once
// mayag::platform — the window/OS seam
//
// mayag needs exactly six things from an operating system:
//
//   1. a window with a drawable surface
//   2. events out of it
//   3. its size and DPI scale
//   4. present a finished frame
//   5. a clock
//   6. the clipboard and the cursor
//
// That is the entire `Window` concept below. It is a CONCEPT, not a base
// class: the concrete type is chosen by `#if` at compile time and aliased to
// `platform::Native`, so there is no vtable, no indirect call per frame, and
// the optimiser sees straight through to the platform code.
//
// Every backend is checked against the concept HERE, so a broken port fails
// with "does not satisfy Window" at this line instead of a template error
// three headers deep.

#include "../backend/software.hpp"
#include "../core/geometry.hpp"
#include "../platform/types.hpp"
#include "event.hpp"

#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::platform {

// ── the concept ─────────────────────────────────────────────────────────

template <typename W>
concept Window = requires(W w, const W cw, const DrawList& dl, Wait wait,
                          double timeout, std::string_view text) {
    // Lifecycle
    { W::open(WindowConfig{}) } -> std::same_as<std::optional<W>>;
    { w.close() }               -> std::same_as<void>;
    { cw.is_open() }            -> std::same_as<bool>;

    // Geometry
    { cw.size() }      -> std::same_as<Vec2>;    ///< logical
    { cw.dpi_scale() } -> std::same_as<float>;

    // Events: drain everything queued, translating to mayag::Event.
    { w.poll_events(wait, timeout) } -> std::same_as<std::vector<Event>>;

    // Presentation
    { w.present(dl, Color<Srgb>{}) } -> std::same_as<void>;

    // Services
    { cw.now() }               -> std::same_as<double>;   ///< monotonic seconds
    /// The user's configured multi-click interval, in seconds.
    { cw.double_click_interval() } -> std::same_as<double>;
    { w.set_title(text) }      -> std::same_as<void>;
    { w.set_cursor(CursorShape::arrow) } -> std::same_as<void>;
    { w.set_clipboard(text) }  -> std::same_as<void>;
    { w.get_clipboard() }      -> std::same_as<std::string>;

    // Screenshots / golden tests
    { w.read_pixels() } -> std::same_as<std::vector<std::uint8_t>>;
};

}  // namespace mayag::platform

// ── backend selection ───────────────────────────────────────────────────
//
// Order matters: an explicit MAYAG_PLATFORM_* override wins, then the native
// windowing system, then headless. Headless is ALWAYS available, which is
// what makes `run<P>()` compile and run on a build server with no display.

#include "../platform/headless.hpp"

#if defined(MAYAG_PLATFORM_HEADLESS)
    // forced headless
#elif defined(__APPLE__) && defined(MAYAG_WITH_COCOA)
    #include "../platform/macos_window.hpp"
#endif

namespace mayag::platform {

#if defined(MAYAG_PLATFORM_HEADLESS)
    using Native = Headless;
#elif defined(__APPLE__) && defined(MAYAG_WITH_COCOA)
    using Native = MacWindow;
#else
    using Native = Headless;
#endif

// The concept check that turns a bad port into a one-line error.
static_assert(Window<Native>,
              "mayag: the selected platform backend does not satisfy the Window concept.");

[[nodiscard]] constexpr std::string_view native_name() noexcept {
#if defined(MAYAG_PLATFORM_HEADLESS)
    return "headless";
#elif defined(__APPLE__) && defined(MAYAG_WITH_COCOA)
    return "cocoa";
#else
    return "headless";
#endif
}

}  // namespace mayag::platform
