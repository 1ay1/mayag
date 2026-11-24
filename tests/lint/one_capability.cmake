# tests/lint/one_capability.cmake — window capabilities: one vocabulary.
#
# platform/concepts.hpp defines each capability as a concept AND a cap:: tag
# (its type), and cap::all is the universe capabilities_of filters. So:
#   1. every capability concept has a tag, and every tag is in cap::all
#      (counts agree) -- a concept without a tag would be invisible to
#      capabilities_of and to every backend's checked statement;
#   2. the host (app/) asks only `has<W, cap::x>`, never a capability
#      concept by name -- one way to ask.
#
#   cmake -DROOT=<mayag>/include/mayag -P one_capability.cmake

set(f ${ROOT}/platform/concepts.hpp)
file(READ ${f} src)
string(REGEX MATCHALL "template <class W> concept [A-Z][A-Za-z]+" concepts "${src}")
string(REGEX MATCHALL "\nMAYAG_CAPABILITY\\(([a-z_]+)" tags "${src}")
string(REGEX MATCH "using all = caps<([^>]*)>" _ "${src}")
string(REGEX MATCHALL "[a-z_]+" all "${CMAKE_MATCH_1}")
list(LENGTH concepts nc)
list(LENGTH tags nt)
list(LENGTH all na)
set(bad 0)
if(NOT nc EQUAL nt OR NOT nt EQUAL na)
    message(SEND_ERROR "one_capability: ${nc} capability concepts, ${nt} cap:: tags, ${na} in cap::all -- "
                       "each concept needs a tag (MAYAG_CAPABILITY) and a place in cap::all")
    math(EXPR bad "${bad}+1")
endif()
string(REGEX MATCHALL "template <class W> concept ([A-Z][A-Za-z]+)" _ "${src}")
foreach(c ${concepts})
    string(REGEX REPLACE "template <class W> concept " "" name "${c}")
    file(GLOB hs ${ROOT}/app/*.hpp)
    foreach(h ${hs})
        file(STRINGS ${h} hits REGEX "platform::${name}<")
        if(hits)
            message(SEND_ERROR "one_capability: ${h} asks platform::${name}<W>; ask platform::has<W, platform::cap::...>")
            math(EXPR bad "${bad}+1")
        endif()
    endforeach()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "one_capability: ${bad} violation(s)")
endif()
message(STATUS "one_capability: ${nt} capabilities, one vocabulary (platform/concepts.hpp)")
