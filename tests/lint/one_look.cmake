# tests/lint/one_look.cmake — graphics are a single source of truth.
#
# A frame is the same pixels on Linux, macOS and Windows: font metrics,
# glyph placement and rasterisation, layout, styling, widgets and the shading
# kernel (render/kernel.hpp, transpiled to SPIR-V / HLSL) are written ONCE.
# Platforms differ only in how a frame reaches the screen and where fonts are
# found: platform/, backend/, font/system.hpp, font/mapped_file.hpp and
# font/dwrite_raster.hpp -- and core/target.hpp, which decides what the
# platform IS (the target, the renderers, the one window system).
#
# So in the files below a platform conditional (the OS, the compiler, the
# CPU or vector unit, the renderer or window system: *_OS_ / _CC_ / _CPU_ /
# _ISA_ / _GPU_ / _WINDOW_, or a raw _WIN32 / __APPLE__ ...) is an error
# unless allow_one_look.txt grants that file a COUNT, with the reason.
# Grants are exact: a new conditional fails the build, and so does a grant
# that is no longer used up.

set(dirs font layout render style dsl widget core scene image output editor)
set(platform_files core/target.hpp font/system.hpp font/mapped_file.hpp font/dwrite_raster.hpp core/win32.hpp core/startup.hpp)

file(STRINGS ${ALLOW} allow_lines)
set(granted "")
foreach(l IN LISTS allow_lines)
    string(REGEX REPLACE "#.*" "" l "${l}")
    string(STRIP "${l}" l)
    if(l STREQUAL "")
        continue()
    endif()
    string(REGEX MATCH "^([^:]+): *([0-9]+)" _ "${l}")
    set(grant_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
    list(APPEND granted ${CMAKE_MATCH_1})
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/lines.cmake)
set(bad 0)
foreach(d ${dirs})
    file(GLOB_RECURSE fs RELATIVE ${ROOT} ${ROOT}/${d}/*.hpp)
    foreach(rel ${fs})
        list(FIND platform_files ${rel} is_platform)
        if(NOT is_platform EQUAL -1)
            continue()
        endif()
        # lint_grep, not file(STRINGS REGEX): a COUNT must not merge two hits at a `[`
        lint_grep(${ROOT}/${rel} "^[ \t]*#[ \t]*(if|elif|ifdef|ifndef).*(_WIN32|__APPLE__|__linux__|TARGET_OS_|(MAYAG|JAAL)_(OS|CC|CPU|ISA|GPU|WINDOW)_)" hits)
        list(LENGTH hits n)
        set(want 0)
        if(DEFINED grant_${rel})
            set(want ${grant_${rel}})
        endif()
        if(NOT n EQUAL want)
            message(SEND_ERROR "one_look: ${rel} has ${n} platform conditional(s), ${want} granted "
                               "(tests/lint/allow_one_look.txt). Graphics are written once for every platform.")
            math(EXPR bad "${bad}+1")
        endif()
    endforeach()
endforeach()
foreach(g ${granted})
    if(NOT EXISTS ${ROOT}/${g})
        message(SEND_ERROR "one_look: a grant for ${g}, which does not exist")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "one_look: ${bad} violation(s)")
endif()
message(STATUS "one_look: graphics are one source on every platform")
