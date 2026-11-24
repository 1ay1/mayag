#pragma once
// mayag::target — what this build is FOR and what it was built WITH, in one
// place.
//
// Two kinds of platform fact reach mayag, and each is read HERE, once:
//
//   the target   OS, compiler, CPU, vector ISA. Read from the compiler by
//                jaal/meta/target.hpp (the one file in the stack that tests
//                _WIN32 / __clang__ / __SSE2__ ...) and re-exported below as
//                MAYAG_OS_* / MAYAG_CC_* / MAYAG_CPU_* / MAYAG_ISA_*, so mayag
//                and the app above it never name jaal.
//
//   the build    which renderers and which window system this binary carries:
//                CMake's MAYAG_WITH_* options (defined or not) and the
//                MAYAG_PLATFORM_HEADLESS override. Normalised here to
//                MAYAG_GPU_* and MAYAG_WINDOW_*, always defined 0/1, and
//                MAYAG_WINDOW_* is THE window-system decision: exactly one of
//                COCOA / WAYLAND / WIN32 / HEADLESS is 1. platform.hpp,
//                app.hpp and the examples read that answer; none of them
//                re-derives it from "Apple and Cocoa and not headless".
//
// Every name below is always defined, 0 or 1, so code tests it with #if
// (never #ifdef) and a missing include is a -Wundef warning, not a silent
// "no". Lints: one_os (no raw target macro outside jaal/meta/target.hpp),
// one_build (no MAYAG_WITH_* / MAYAG_PLATFORM_HEADLESS outside this file),
// one_look (where in mayag a platform may be tested at all).

#include <jaal/meta/target.hpp>

// ── the target ───────────────────────────────────────────────────────────────
#define MAYAG_OS_WINDOWS     JAAL_OS_WINDOWS
#define MAYAG_OS_APPLE       JAAL_OS_APPLE
#define MAYAG_OS_LINUX       JAAL_OS_LINUX
#define MAYAG_OS_ANDROID     JAAL_OS_ANDROID
#define MAYAG_OS_FREEBSD     JAAL_OS_FREEBSD
#define MAYAG_OS_OPENBSD     JAAL_OS_OPENBSD
#define MAYAG_OS_NETBSD      JAAL_OS_NETBSD
#define MAYAG_OS_EMSCRIPTEN  JAAL_OS_EMSCRIPTEN
#define MAYAG_OS_UNIX        JAAL_OS_UNIX
#define MAYAG_OS_POSIX       JAAL_OS_POSIX
#define MAYAG_OS_FREEDESKTOP JAAL_OS_FREEDESKTOP   // Linux, FreeBSD: Wayland, fontconfig

#define MAYAG_CC_CLANG       JAAL_CC_CLANG
#define MAYAG_CC_GCC         JAAL_CC_GCC
#define MAYAG_CC_MSVC        JAAL_CC_MSVC
#define MAYAG_CC_MSVC_ABI    JAAL_CC_MSVC_ABI
#define MAYAG_CC_APPLE_CLANG JAAL_CC_APPLE_CLANG
#define MAYAG_CC_INT128      JAAL_CC_INT128

#define MAYAG_CPU_ARM64      JAAL_CPU_ARM64
#define MAYAG_CPU_X86_64     JAAL_CPU_X86_64
#define MAYAG_ISA_NEON       JAAL_ISA_NEON
#define MAYAG_ISA_SSE2       JAAL_ISA_SSE2
#define MAYAG_ISA_SSE41      JAAL_ISA_SSE41
#define MAYAG_ISA_AVX2       JAAL_ISA_AVX2

/// Silence one GCC warning for the code between, on GCC only (a false
/// positive, with the reason written next to it):
///   MAYAG_GCC_IGNORE("-Wshadow")  ...  MAYAG_GCC_RESTORE()
/// The one place mayag spells a compiler's pragma, so no file outside this
/// one tests MAYAG_CC_* to quiet a warning.
#define MAYAG_PRAGMA_(x) _Pragma(#x)
#if MAYAG_CC_GCC
#  define MAYAG_GCC_IGNORE(warning) _Pragma("GCC diagnostic push") MAYAG_PRAGMA_(GCC diagnostic ignored warning)
#  define MAYAG_GCC_RESTORE()       _Pragma("GCC diagnostic pop")
#else
#  define MAYAG_GCC_IGNORE(warning)
#  define MAYAG_GCC_RESTORE()
#endif

// ── the build: renderers ─────────────────────────────────────────────────────
#if defined(MAYAG_WITH_VULKAN)
#  define MAYAG_GPU_VULKAN 1
#else
#  define MAYAG_GPU_VULKAN 0
#endif
#if defined(MAYAG_WITH_METAL)
#  define MAYAG_GPU_METAL 1
#else
#  define MAYAG_GPU_METAL 0
#endif
#if defined(MAYAG_WITH_D3D12)
#  define MAYAG_GPU_D3D12 1
#else
#  define MAYAG_GPU_D3D12 0
#endif
#if defined(MAYAG_WITH_WEBGPU)
#  define MAYAG_GPU_WEBGPU 1
#else
#  define MAYAG_GPU_WEBGPU 0
#endif
#if defined(MAYAG_WITH_OPENGL)
#  define MAYAG_GPU_OPENGL 1
#else
#  define MAYAG_GPU_OPENGL 0
#endif

// ── the build: THE window system ─────────────────────────────────────────────
// An explicit MAYAG_PLATFORM_HEADLESS wins; then the native system this OS
// has, if the build carries it; else headless, which every build has (so a
// program builds and runs on a server with no display).
#if defined(MAYAG_PLATFORM_HEADLESS)
#  define MAYAG_WINDOW_COCOA   0
#  define MAYAG_WINDOW_WAYLAND 0
#  define MAYAG_WINDOW_WIN32   0
#elif MAYAG_OS_APPLE && defined(MAYAG_WITH_COCOA)
#  define MAYAG_WINDOW_COCOA   1
#  define MAYAG_WINDOW_WAYLAND 0
#  define MAYAG_WINDOW_WIN32   0
#elif MAYAG_OS_FREEDESKTOP && defined(MAYAG_WITH_WAYLAND)
#  define MAYAG_WINDOW_COCOA   0
#  define MAYAG_WINDOW_WAYLAND 1
#  define MAYAG_WINDOW_WIN32   0
#elif MAYAG_OS_WINDOWS && defined(MAYAG_WITH_WIN32)
#  define MAYAG_WINDOW_COCOA   0
#  define MAYAG_WINDOW_WAYLAND 0
#  define MAYAG_WINDOW_WIN32   1
#else
#  define MAYAG_WINDOW_COCOA   0
#  define MAYAG_WINDOW_WAYLAND 0
#  define MAYAG_WINDOW_WIN32   0
#endif
#define MAYAG_WINDOW_HEADLESS (!MAYAG_WINDOW_COCOA && !MAYAG_WINDOW_WAYLAND && !MAYAG_WINDOW_WIN32)

static_assert(MAYAG_WINDOW_COCOA + MAYAG_WINDOW_WAYLAND + MAYAG_WINDOW_WIN32 + MAYAG_WINDOW_HEADLESS == 1,
              "mayag: exactly one window system");

namespace mayag::target {
using os_kind = jaal::platform::os_kind;
inline constexpr os_kind os = jaal::platform::os;

enum class window_kind : unsigned char { headless, cocoa, wayland, win32 };
/// The window system as a value, for `if constexpr` and names.
inline constexpr window_kind window = MAYAG_WINDOW_COCOA ? window_kind::cocoa
                                    : MAYAG_WINDOW_WAYLAND ? window_kind::wayland
                                    : MAYAG_WINDOW_WIN32 ? window_kind::win32 : window_kind::headless;
}  // namespace mayag::target
