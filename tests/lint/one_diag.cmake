# tests/lint/one_diag.cmake — no diagnostic can cost a frame.
#
# Writing to stderr/stdout is I/O and can block for as long as whatever reads
# the stream likes. Measured: present-stats lines written from the frame path
# blocked the UI thread 35-190 ms each, making the late frames they reported.
# So include/mayag/ writes diagnostics only through core/diag.hpp: say()
# (formatted here, written on a jaal worker) or fatal() (a process about to
# exit). This lint fails on any other write to a standard stream.
#
#   cmake -DROOT=<mayag>/include/mayag -P one_diag.cmake

include(${CMAKE_CURRENT_LIST_DIR}/lines.cmake)
get_filename_component(ROOT "${ROOT}" ABSOLUTE)
file(GLOB_RECURSE fs RELATIVE ${ROOT} ${ROOT}/*.hpp)
list(LENGTH fs nfiles)
if(nfiles LESS 50)
    message(FATAL_ERROR "one_diag: found only ${nfiles} headers under ${ROOT} -- wrong ROOT? (a lint that scans nothing passes nothing)")
endif()
set(bad 0)
set(total 0)
foreach(rel ${fs})
    if(rel STREQUAL "core/diag.hpp")
        continue()
    endif()
    lint_lines(${ROOT}/${rel} lines)   # one element per line (file(STRINGS) merges lines at a `[`)
    set(n 0)
    foreach(line IN LISTS lines)
        math(EXPR n "${n}+1")
        lint_unescape(line)
        string(REGEX REPLACE "//.*$" "" code "${line}")   # comments do not count
        # a stream write, or a write that blocks until the stream drains
        if(code MATCHES "fprintf[ \t]*\\([ \t]*std(err|out)|fputs[ \t]*\\(.*std(err|out)|fflush[ \t]*\\([ \t]*std(err|out)|std::(printf|puts)[ \t]*\\(|std::c(err|out|log)|[^a-z_]printf[ \t]*\\(|OutputDebugString")
            message(SEND_ERROR "one_diag: ${rel}:${n}: writes a standard stream directly -- use mayag::diag::say (or fatal before exiting): ${line}")
            math(EXPR bad "${bad}+1")
        endif()
    endforeach()
    math(EXPR total "${total}+${n}")
endforeach()
if(total LESS 20000)
    message(FATAL_ERROR "one_diag: scanned only ${total} lines -- the line reader is broken (a lint that scans nothing passes nothing)")
endif()
if(bad GREATER 0)
    message(FATAL_ERROR "one_diag: ${bad} direct stream write(s)")
endif()
message(STATUS "one_diag: every mayag diagnostic goes through core/diag.hpp")
