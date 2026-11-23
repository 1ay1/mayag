#pragma once
#include "target.hpp"
// mayag::startup — where launch time goes.
//
// MAYAG_STARTUP=1 prints a line per phase, in ms since the process started,
// so a slow launch can be blamed on fonts, the GPU, or the first frame.

#include <chrono>
#include <cstdio>
#include <cstdlib>

#if MAYAG_OS_LINUX
#  include <time.h>
#  include <unistd.h>
#elif MAYAG_OS_WINDOWS
// NOT <windows.h>: this header is included almost everywhere, and windows.h
// defines IN, OUT, near, far... which collide with ordinary identifiers (the
// font engine's LineBreak::IN). The three kernel32 calls are declared by
// hand, with the same linkage, calling convention and layout as the SDK.
extern "C" {
struct _FILETIME;
__declspec(dllimport) int __stdcall GetProcessTimes(void*, _FILETIME*, _FILETIME*, _FILETIME*, _FILETIME*);
__declspec(dllimport) void __stdcall GetSystemTimePreciseAsFileTime(_FILETIME*);
__declspec(dllimport) void* __stdcall GetCurrentProcess(void);
}
#endif

namespace mayag::startup {

namespace detail {
/// Milliseconds since the process was created (Linux: from /proc, so dynamic
/// loading and static init count too); elsewhere since first use.
[[nodiscard]] inline double now_ms() noexcept {
#if MAYAG_OS_LINUX
    static const double start_ms = [] {
        // Field 22 of /proc/self/stat: start time in clock ticks since boot.
        double ticks = 0;
        if (std::FILE* f = std::fopen("/proc/self/stat", "r")) {
            char buf[1024];
            const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            buf[n] = 0;
            const char* p = buf;
            for (const char* q = buf; *q; ++q) if (*q == ')') p = q;   // comm may hold spaces
            int field = 2;
            for (; *p && field < 22; ++p) if (*p == ' ') ++field;
            ticks = std::strtod(p, nullptr);
        }
        return ticks * 1000.0 / static_cast<double>(::sysconf(_SC_CLK_TCK));
    }();
    timespec ts{};
    ::clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6 - start_ms;
#elif MAYAG_OS_WINDOWS
    // From the process's creation time, like /proc on Linux, so the loader,
    // DLL init and static constructors are on the timeline too. FILETIME is
    // two 32-bit halves; read it as such through the opaque declaration.
    struct ft { unsigned long lo, hi; };
    const auto q = [](const ft& f) { return (static_cast<unsigned long long>(f.hi) << 32) | f.lo; };
    static const unsigned long long created = [&] {
        ft c{}, e{}, k{}, u{};
        GetProcessTimes(GetCurrentProcess(), reinterpret_cast<_FILETIME*>(&c), reinterpret_cast<_FILETIME*>(&e),
                        reinterpret_cast<_FILETIME*>(&k), reinterpret_cast<_FILETIME*>(&u));
        return q(c);
    }();
    ft now{};
    GetSystemTimePreciseAsFileTime(reinterpret_cast<_FILETIME*>(&now));
    return static_cast<double>(q(now) - created) / 10'000.0;
#else
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
#endif
}
}  // namespace detail

[[nodiscard]] inline bool enabled() noexcept {
    static const bool on = [] {
        const char* e = std::getenv("MAYAG_STARTUP");
        return e != nullptr && *e != '\0' && *e != '0';
    }();
    return on;
}

/// Record that a phase finished.
inline void mark(const char* what) noexcept {
    if (!enabled()) return;
    std::fprintf(stderr, "mayag: startup %8.1f ms  %s\n", detail::now_ms(), what);
}

}  // namespace mayag::startup
