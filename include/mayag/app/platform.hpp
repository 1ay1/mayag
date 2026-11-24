#pragma once
#include "../core/target.hpp"
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
// That is the entire `Window` concept (platform/concepts.hpp, with the
// optional capabilities). It is a CONCEPT, not a base class: the concrete
// type is chosen ONCE (core/target.hpp: MAYAG_WINDOW_*) and aliased to
// `platform::Native`, so there is no vtable, no indirect call per frame, and
// the optimiser sees straight through to the platform code.
//
// Every backend is checked against the concept HERE, so a broken port fails
// with "does not satisfy Window" at this line instead of a template error
// three headers deep.

#include "../backend/software.hpp"
#include "../core/geometry.hpp"
#include "../platform/types.hpp"
#include "../platform/concepts.hpp"   // Window, and what a window MAY do
#include "../core/event.hpp"

#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>


// ── backend selection ───────────────────────────────────────────────────
//
// WHICH one is decided once, in core/target.hpp (MAYAG_WINDOW_*: an explicit
// MAYAG_PLATFORM_HEADLESS wins, then the OS's native system if built, then
// headless). Headless is ALWAYS available, which is what makes `run<P>()`
// compile and run on a build server with no display.

#include "../platform/headless.hpp"

#if MAYAG_WINDOW_COCOA
    #include "../platform/macos_window.hpp"
#elif MAYAG_WINDOW_WAYLAND
    #include "../platform/wayland.hpp"
#elif MAYAG_WINDOW_WIN32
    #include "../platform/win32_window.hpp"
#endif

namespace mayag::platform {

#if MAYAG_WINDOW_COCOA
    using Native = MacWindow;
#elif MAYAG_WINDOW_WAYLAND
    using Native = WaylandWindow;
#elif MAYAG_WINDOW_WIN32
    using Native = Win32Window;
#else
    using Native = Headless;
#endif

// The concept check that turns a bad port into a one-line error.
static_assert(Window<Native>,
              "mayag: the selected platform backend does not satisfy the Window concept.");

// What each backend offers is stated next to it (check_capabilities, after
// each window class) and checked against what its methods give it.

[[nodiscard]] constexpr std::string_view native_name() noexcept {
    switch (target::window) {
        case target::window_kind::cocoa: return "cocoa";
        case target::window_kind::wayland: return "wayland";
        case target::window_kind::win32: return "win32";
        case target::window_kind::headless: break;
    }
    return "headless";
}

}  // namespace mayag::platform
