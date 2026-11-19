# tests/lint/layering.cmake — mayag's layers, bottom to top, enforced.
#
#   jaal  (the runtime; never includes mayag — checked by jaal's own lint)
#   core          vocabulary: geometry, colour, events, a11y types, keys
#   style image   what things look like; decoded pixels
#   scene         the Node tree and the cascade over it
#   layout dsl editor
#   render        draw lists, the shading kernel, the painter
#   backend output
#   font
#   platform      windows: Wayland, Cocoa, headless
#   app           the host: mayag <-> jaal
#   widget        ready-made components
#
# A header in include/mayag/<A>/ may include ../<B>/ only when B is STRICTLY
# lower than A. Upward or sideways includes are how cycles start (core used to
# include app/ and scene/, style included scene/ and platform/). Move shared
# vocabulary DOWN instead.
#
# Run with: cmake -DROOT=<mayag source dir> -P layering.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(rank_core 0)
set(rank_style 1)
set(rank_image 1)
set(rank_scene 2)
set(rank_layout 3)
set(rank_dsl 3)
set(rank_editor 3)
set(rank_render 4)
set(rank_backend 5)
set(rank_output 5)
set(rank_font 6)
set(rank_platform 7)
set(rank_app 8)
set(rank_widget 9)

set(hits "")
file(GLOB dirs LIST_DIRECTORIES true ${ROOT}/include/mayag/*)
foreach(d IN LISTS dirs)
    if(NOT IS_DIRECTORY ${d})
        continue()
    endif()
    get_filename_component(layer ${d} NAME)
    if(NOT DEFINED rank_${layer})
        list(APPEND hits "include/mayag/${layer}/: not a known layer (add it to tests/lint/layering.cmake)")
        continue()
    endif()
    file(GLOB files ${d}/*.hpp)
    foreach(f IN LISTS files)
        file(STRINGS ${f} lines REGEX "^[ \t]*#[ \t]*include[ \t]*\"\\.\\./[a-z_]+/")
        foreach(l IN LISTS lines)
            string(REGEX MATCH "\\.\\./([a-z_]+)/" _ "${l}")
            set(target ${CMAKE_MATCH_1})
            if(NOT DEFINED rank_${target} OR target STREQUAL layer)
                continue()
            endif()
            # A forwarding header kept at an old path may point anywhere: it
            # holds no code of its own.
            file(READ ${f} body)
            if(body MATCHES "This path is kept so existing includes keep working")
                continue()
            endif()
            if(NOT rank_${target} LESS rank_${layer})
                get_filename_component(name ${f} NAME)
                string(STRIP "${l}" s)
                list(APPEND hits "${layer}/${name}: ${s}   (${layer}=${rank_${layer}} may not include ${target}=${rank_${target}})")
            endif()
        endforeach()
    endforeach()
endforeach()

if(hits)
    list(JOIN hits "\n  " msg)
    message(FATAL_ERROR "mayag layering violated:\n  ${msg}\n\n"
        "Move the shared piece DOWN to the lower layer (as core/event.hpp, "
        "core/cursor.hpp and core/a11y_types.hpp were), or invert the dependency "
        "(a template parameter, as TrackedSet::observe_all takes any tree).")
endif()
message(STATUS "mayag layering: ok")
