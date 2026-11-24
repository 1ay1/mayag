#pragma once
#include "../core/target.hpp"
// mayag::platform::raise_ui_thread — the thread that turns input into frames
// runs ahead of background work.
//
// Measured: hand started from a terminal (no Dock launch, so no interactive
// QoS from the system) sharing the machine with a -j12 build: the 1 Hz clock
// message -- an update that bumps a counter -- "took" 2.5 s, pane messages up
// to 2 s, scrolling stalled. The work was microseconds; the thread was simply
// not scheduled. A UI must win that contest by construction, so a window app
// asks the OS for it, once, before it starts any thread (on macOS a thread's
// QoS is inherited by the threads it creates: jaal's workers, the raster
// pool, the pty readers all follow).
//
//   macOS     QOS_CLASS_USER_INTERACTIVE (what AppKit gives the main thread
//             of an app launched from the Dock)
//   Windows   THREAD_PRIORITY_ABOVE_NORMAL (the foreground boost does the rest)
//   others    nothing: an unprivileged process cannot raise its own priority
//             on Linux; the compositor-driven frame pacing still holds.

#if MAYAG_OS_APPLE
#  include <pthread/qos.h>
#elif MAYAG_OS_WINDOWS
#  include "../core/win32.hpp"
#endif

#include <cstdlib>

namespace mayag::platform {

/// Raise the calling thread to the OS's interactive class. True when the OS
/// granted it; false where there is no such knob. Idempotent.
inline bool raise_ui_thread() noexcept {
    if (const char* e = std::getenv("MAYAG_UI_PRIORITY"); e != nullptr && e[0] == '0') return false;   // A/B: leave it as launched
#if MAYAG_OS_APPLE
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#elif MAYAG_OS_WINDOWS
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != 0;
#else
    return false;
#endif
}

}  // namespace mayag::platform
