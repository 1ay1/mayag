# tests/lint/one_image_policy.cmake — a window's images follow ONE policy.
#
# platform/images.hpp keeps image(id) on the CPU always and mirrors it to the
# GPU device while one presents. Cocoa, Wayland and Win32 each used to write
# that out themselves (and a GPU that came up late had to remember to
# replay). So inside platform/ the devices' texture verbs -- upload_texture,
# release_texture -- are called from images.hpp only; a window calls
# images::upload / release / replay.
#
#   cmake -DROOT=<mayag>/include/mayag -P one_image_policy.cmake

file(GLOB fs RELATIVE ${ROOT} ${ROOT}/platform/*.hpp)
set(bad 0)
foreach(rel ${fs})
    if(rel STREQUAL "platform/images.hpp")
        continue()
    endif()
    file(STRINGS ${ROOT}/${rel} hits REGEX "(->|\\.)(upload_texture|release_texture)[ \t]*\\(")
    if(hits)
        message(SEND_ERROR "one_image_policy: ${rel} calls a device's texture verbs itself; "
                           "use images::upload / release / replay (platform/images.hpp):\n${hits}")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "one_image_policy: ${bad} file(s)")
endif()
message(STATUS "one_image_policy: every window keeps its images one way (platform/images.hpp)")
