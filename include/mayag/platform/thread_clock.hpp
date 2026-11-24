#pragma once
#include "../core/target.hpp"
// mayag::platform::thread_cpu_ms — the time this thread spent ON a CPU.
//
// A frame's wall time is its work plus every moment the OS ran something
// else. On a loaded machine that second part is most of it: the strict
// "no frame over 8 ms" gate failed on a 122-instance frame whose paint
// "took" 11.8 ms -- paint that is 0.07 ms when the thread is scheduled. A gate
// on wall time measures the neighbours; a gate on CPU time measures the code.
// So FrameTiming keeps both: wall time for what the user saw, CPU time for
// what the frame cost.
//
//   macOS, Linux, BSD   clock_gettime(CLOCK_THREAD_CPUTIME_ID): ns, ~0.3 us a call
//   Windows             wall time: GetThreadTimes counts in scheduler quanta
//                       (15.6 ms), coarser than a whole frame, so it would
//                       read every frame as 0 or 15.6 ms.

#if MAYAG_OS_POSIX
#  include <time.h>
#else
#  include <chrono>
#endif

namespace mayag::platform {

/// True where thread_cpu_ms() is the thread's CPU time; false where it is
/// wall time (then a CPU-time gate is no stricter than a wall-time one).
inline constexpr bool thread_clock_is_cpu = MAYAG_OS_POSIX;

/// Milliseconds of CPU this thread has used (an arbitrary origin: take
/// differences).
[[nodiscard]] inline double thread_cpu_ms() noexcept {
#if MAYAG_OS_POSIX
    timespec ts{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) * 1e-6;
#else
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

}  // namespace mayag::platform
