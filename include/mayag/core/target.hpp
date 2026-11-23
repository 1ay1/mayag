#pragma once
// mayag::target — the OS this is compiled for, in mayag's vocabulary.
//
// jaal/meta/target.hpp reads the compiler's OS macros, once, for the
// whole stack; this header re-exports its answers as MAYAG_OS_* so mayag and
// the app above it never name jaal. Never test _WIN32 / __APPLE__ /
// __linux__ directly (lint: one_os); where MAYAG_OS_* may be tested at all is
// one_look's rule (platform/, backend/, the font system files, the keymap).

#include <jaal/meta/target.hpp>

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
#define MAYAG_OS_FREEDESKTOP JAAL_OS_FREEDESKTOP

namespace mayag::target {
using os_kind = jaal::platform::os_kind;
inline constexpr os_kind os = jaal::platform::os;
}  // namespace mayag::target
