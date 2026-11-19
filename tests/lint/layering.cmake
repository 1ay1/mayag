# tests/lint/layering.cmake — mayag's layers, bottom to top, enforced.
#
#   jaal  (the runtime; never includes mayag — checked here and by jaal's own lint)
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
# Three rules:
#
#  1. A header in include/mayag/<A>/ (or any folder under it) may include
#     ../<B>/ only when B is STRICTLY lower than A. Upward or sideways
#     includes are how cycles start. Move shared vocabulary DOWN instead.
#
#  2. Widgets sit on the RUNTIME-FREE part of app/ only: context.hpp (Ctx,
#     memo, themed), gestures.hpp, handlers.hpp, interaction.hpp,
#     latency.hpp. Never the host, the loop, sources or effects -- a widget
#     that could reach the runtime would stop being a pure function of props.
#
#  3. jaal is the runtime and mayag sits on it, never the other way round:
#     jaal never includes mayag, and inside mayag only the runtime glue may
#     include jaal (app/, platform/ loops, and the worker pool users listed
#     below). No widget, dsl, scene, style, layout or render header does.
#
# Run with: cmake -DROOT=<mayag source dir> -P layering.cmake

cmake_minimum_required(VERSION 3.20)
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

# rule 2: what of app/ a widget may see
set(widget_app_ok context.hpp gestures.hpp handlers.hpp interaction.hpp latency.hpp)
# rule 3: the only mayag headers that may include <jaal/...>, and why
set(jaal_ok
    app/app.hpp app/effects.hpp app/host.hpp app/simulate.hpp app/sources.hpp   # the host: mayag <-> jaal
    platform/macos_loop.hpp                                                    # Cocoa's run loop drives jaal's kernel
    platform/wayland.hpp                                                       # GPU bring-up on a jaal worker
    backend/thread_pool.hpp                                                    # the render fork-join is jaal's pool
    backend/vulkan_device.hpp                                                  # a device handle opted into jaal::sendable
    font/system.hpp                                                            # the font scan runs on a jaal worker
    font/ot_layout.hpp)                                                        # lock-free lazy tables (jaal::once)

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
    file(GLOB_RECURSE files ${d}/*.hpp)
    foreach(f IN LISTS files)
        file(RELATIVE_PATH rel ${ROOT}/include/mayag ${f})
        file(STRINGS ${f} lines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
        foreach(l IN LISTS lines)
            string(STRIP "${l}" s)
            # rule 3
            if(l MATCHES "<jaal/")
                if(NOT rel IN_LIST jaal_ok)
                    list(APPEND hits "${rel}: ${s}   (only the runtime glue includes jaal; see rule 3)")
                endif()
                continue()
            endif()
            # rules 1 and 2: the folder a relative include lands in
            if(NOT l MATCHES "\"(\\.\\./)+([a-z_]+)/([a-z_]+\\.hpp)?")
                continue()
            endif()
            set(target ${CMAKE_MATCH_2})
            set(file_ ${CMAKE_MATCH_3})
            if(NOT DEFINED rank_${target} OR target STREQUAL layer)
                continue()
            endif()
            if(NOT rank_${target} LESS rank_${layer})
                list(APPEND hits "${rel}: ${s}   (${layer}=${rank_${layer}} may not include ${target}=${rank_${target}})")
            elseif(layer STREQUAL "widget" AND target STREQUAL "app" AND NOT file_ IN_LIST widget_app_ok)
                list(APPEND hits "${rel}: ${s}   (a widget may only see app/{${widget_app_ok}}; see rule 2)")
            endif()
        endforeach()
    endforeach()
endforeach()

# rule 3, the other direction: jaal knows nothing of mayag
set(jaal_root ${ROOT}/third_party/jaal)
if(IS_DIRECTORY ${jaal_root}/include)
    file(GLOB_RECURSE jfiles ${jaal_root}/include/*.hpp ${jaal_root}/src/*.cpp)
    foreach(f IN LISTS jfiles)
        file(STRINGS ${f} lines REGEX "#[ \t]*include[ \t]*[<\"]mayag")
        foreach(l IN LISTS lines)
            file(RELATIVE_PATH rel ${jaal_root} ${f})
            string(STRIP "${l}" s)
            list(APPEND hits "jaal/${rel}: ${s}   (jaal is below mayag: it never includes it)")
        endforeach()
    endforeach()
endif()

if(hits)
    list(JOIN hits "\n  " msg)
    message(FATAL_ERROR "mayag layering violated:\n  ${msg}\n\n"
        "Move the shared piece DOWN to the lower layer (as core/event.hpp, "
        "core/cursor.hpp and core/a11y_types.hpp were), or invert the dependency "
        "(a template parameter, as TrackedSet::observe_all takes any tree).")
endif()
message(STATUS "mayag layering: ok")
