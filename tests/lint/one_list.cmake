# tests/lint/one_list.cmake — a widget that scrolls builds only what is visible.
#
# The file pane built every row of a folder each frame and scrolled at 21 ms
# a step in a 3000-file folder; virtual_list.hpp (visible_rows, virtual_list)
# existed all along. So in widget/ every file that calls scroll_view( must
# also virtualise (visible_rows / virtual_list / virtual_list_measured), or be
# granted in allow_one_list.txt with the reason its content is bounded. The
# scaling test (test_scaling.cpp) checks the virtualised ones really are
# O(viewport): node count at 100 and 10000 items.
#
#   cmake -DROOT=<mayag>/include/mayag -DALLOW=<allow_one_list.txt> -P one_list.cmake

file(STRINGS ${ALLOW} allow_lines)
set(granted "")
foreach(l IN LISTS allow_lines)
    string(REGEX REPLACE "#.*" "" l "${l}")
    string(STRIP "${l}" l)
    if(NOT l STREQUAL "")
        list(APPEND granted ${l})
    endif()
endforeach()

file(GLOB fs RELATIVE ${ROOT} ${ROOT}/widget/*.hpp)
set(bad 0)
foreach(rel ${fs})
    if(rel STREQUAL "widget/scroll_view.hpp" OR rel STREQUAL "widget/virtual_list.hpp")
        continue()
    endif()
    file(READ ${ROOT}/${rel} src)
    string(FIND "${src}" "scroll_view(" scrolls)
    if(scrolls EQUAL -1)
        continue()
    endif()
    string(REGEX MATCH "(visible_rows|virtual_list|virtual_list_measured)\\(" virt "${src}")
    list(FIND granted ${rel} g)
    if(NOT virt AND g EQUAL -1)
        message(SEND_ERROR "one_list: ${rel} scrolls but builds every row; use visible_rows / virtual_list "
                           "(widget/virtual_list.hpp), or grant it in allow_one_list.txt with why it is bounded")
        math(EXPR bad "${bad}+1")
    endif()
    if(virt AND NOT g EQUAL -1)
        message(SEND_ERROR "one_list: ${rel} virtualises now: drop its grant")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
foreach(g ${granted})
    if(NOT EXISTS ${ROOT}/${g})
        message(SEND_ERROR "one_list: a grant for ${g}, which does not exist")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "one_list: ${bad} violation(s)")
endif()
message(STATUS "one_list: every scrolling widget builds only what is visible")
