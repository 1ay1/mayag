# tests/lint/type_scale.cmake — every text size is a tier of the type scale.
#
# mayag once had 25 distinct font-size multipliers over 404 call sites
# (font_size * 0.72, * 0.74, * 0.76, ...): a panel showed four sizes 0.3 px
# apart and read as noise. Sizes are now ROLES (style/type.hpp): a face and a
# tier -- `| type(Tier::small)`, `font(t.type(Tier::small))`,
# `font(t.type_at(body, Tier::fine))` -- and the theme decides the pixels.
#
# So in widget/, a `font(...)` whose argument does arithmetic or is a bare
# literal is an error. A glyph fitted to geometry (a check in a box, a
# monogram, a powerline wedge) says so with `glyph_size(px)`, which is the one
# sizing modifier allowed to compute.

include(${CMAKE_CURRENT_LIST_DIR}/lines.cmake)
set(bad 0)
file(GLOB_RECURSE fs RELATIVE ${ROOT}/include/mayag ${ROOT}/include/mayag/widget/*.hpp)
foreach(rel ${fs})
    lint_lines(${ROOT}/include/mayag/${rel} lines)   # file(STRINGS) merged lines at a `[` (ssh_manager: 468 of 1407)
    set(n 0)
    foreach(l IN LISTS lines)
        math(EXPR n "${n}+1")
        lint_unescape(l)
        string(REGEX REPLACE "//.*" "" code "${l}")
        # font( ... ) with no nested call: catch `font(x * 0.8f)`, `font(11)`
        string(REGEX MATCHALL "[^a-z_]font\\([^()]*\\)" calls "${code}")
        foreach(c IN LISTS calls)
            if(c MATCHES "[*/+]" OR c MATCHES "font\\( *[0-9]")
                message(SEND_ERROR "type_rules: ${rel}:${n}: ${c} -- a text size is a tier "
                                   "(type(Tier::...), t.type(...), t.type_at(body, ...)); a glyph "
                                   "fitted to geometry is glyph_size(px)")
                math(EXPR bad "${bad}+1")
            endif()
        endforeach()
        # font(t.type(...) * k) and the like: arithmetic around a tier
        string(REGEX MATCHALL "[^a-z_]font\\(t\\.type[a-z_]*\\([^()]*\\) *[*/+-][^)]*\\)" around "${code}")
        foreach(c IN LISTS around)
            message(SEND_ERROR "type_rules: ${rel}:${n}: ${c} -- arithmetic on a tier is a new size")
            math(EXPR bad "${bad}+1")
        endforeach()
        # the theme's raw base, scaled, anywhere: a size that dodges the scale
        if(code MATCHES "font_size *\\* *0\\.[0-9]")
            message(SEND_ERROR "type_rules: ${rel}:${n}: font_size * 0.x -- use a tier")
            math(EXPR bad "${bad}+1")
        endif()
    endforeach()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "type_rules: ${bad} text size(s) off the scale")
endif()
