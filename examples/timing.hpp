#pragma once
// examples/timing.hpp — whether wall-clock budgets are meaningful in this build
//
// mayag's perf checks assert real budgets ("the frame fits half a 60 Hz
// refresh"). Those hold for optimised code. An -O0 build (the `dev` preset:
// the fast edit-compile loop) is 3-10x slower by construction, so there the
// budgets are REPORTED but not enforced; every structural check (instance
// counts, draw calls, idle-means-idle) still is. GCC and clang both define
// __OPTIMIZE__ at -O1 and above. MAYAG_ENFORCE_TIMING=1 forces enforcement.

#include <mayag/core/target.hpp>

#include <cstdlib>

namespace mayag::testing {

[[nodiscard]] inline bool timing_enforced() noexcept {
    if (const char* e = std::getenv("MAYAG_ENFORCE_TIMING")) return e[0] == '1';
#if defined(__OPTIMIZE__) || (MAYAG_CC_MSVC_ABI && defined(NDEBUG))
    return true;
#else
    return false;
#endif
}

/// A timing check: `ok` when enforced, always true (just reported) when not.
[[nodiscard]] inline bool timing_ok(bool ok) noexcept { return ok || !timing_enforced(); }

}  // namespace mayag::testing
