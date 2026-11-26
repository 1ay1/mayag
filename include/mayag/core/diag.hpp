#pragma once
// mayag::diag — diagnostics that can never cost a frame.
//
// A frame path must not wait on I/O, and stderr IS I/O: a pipe a parent stops
// reading, a file a virus scanner holds, a console that scrolls. Measured:
// MAYAG_PRESENTSTATS lines written from inside the frame blocked the UI
// thread 35-190 ms each time -- the very stats meant to catch late frames
// were making them (keystrokes a refresh late, "judder" that vanished with
// the stats off).
//
// So every diagnostic line in mayag goes through say(): it is formatted on
// the caller's thread (microseconds, no syscall) and handed to ONE jaal
// worker that writes it, in order. The UI thread never touches the stream.
// tests/lint/one_diag.cmake forbids writing to stderr/stdout anywhere else
// in include/mayag/. fatal() is the one synchronous path, for a process
// about to exit (there is no frame left to protect, and the line must land).

#include <jaal/kernel/pool.hpp>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <stop_token>

namespace mayag::diag {

namespace detail {
/// One writer, so lines stay in the order they were said.
[[nodiscard]] inline ::jaal::kernel::pool& writer() {
    static ::jaal::kernel::pool p{1};
    return p;
}
inline std::string vformat(const char* fmt, std::va_list ap) {
    char buf[512];
    std::va_list again;
    va_copy(again, ap);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    std::string s;
    if (n < 0) { va_end(again); return s; }
    if (static_cast<std::size_t>(n) < sizeof buf) {
        s.assign(buf, static_cast<std::size_t>(n));
    } else {
        s.resize(static_cast<std::size_t>(n) + 1);
        (void)std::vsnprintf(s.data(), s.size(), fmt, again);
        s.resize(static_cast<std::size_t>(n));
    }
    va_end(again);
    return s;
}
}  // namespace detail

/// printf-style: format here, write on the diagnostics worker. Never blocks
/// on the stream; safe from a frame, a WndProc, a GPU callback.
[[gnu::format(printf, 1, 2)]] inline void say(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::string line = detail::vformat(fmt, ap);
    va_end(ap);
    if (line.empty()) return;
    detail::writer().post(+[](std::stop_token, std::string s) {
        std::fputs(s.c_str(), stderr);
        std::fflush(stderr);
    }, std::move(line));
}

/// Synchronous, for a process about to exit (could not open a window...).
/// Never from a frame path.
[[gnu::format(printf, 1, 2)]] inline void fatal(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fflush(stderr);
}

}  // namespace mayag::diag
