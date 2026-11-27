# tests/lint/lines.cmake — read a source file as ONE list element per line.
#
# file(STRINGS) is not that: CMake list syntax treats `[` `]` and `;` as
# structure, so a line with an unbalanced bracket swallows the lines after it
# (host.hpp came back as 491 elements for 2074 lines). A lint that then strips
# `//.*$` from such an element deletes every line after the first comment --
# and passes code it never saw. one_diag missed a std::fprintf(stderr) that way.
#
#   include(lines.cmake)
#   lint_lines(${file} lines)          # list: one element per source line
#   foreach(l IN LISTS lines)
#       lint_unescape(l)               # back to the source text: lint it as usual
#       ...
#   endforeach()
#
# In the list, `[` `]` `;` `\` are placeholders (any of them would split or
# merge list elements); lint_unescape() restores them. Once a line is in a
# plain variable they are harmless, so every regex sees the real source.

function(lint_lines file out)
    file(READ "${file}" src)
    string(REPLACE "\r" "" src "${src}")
    string(REPLACE ";" "<SC>" src "${src}")
    string(REPLACE "[" "<LB>" src "${src}")
    string(REPLACE "]" "<RB>" src "${src}")
    string(REPLACE "\\" "<BS>" src "${src}")
    string(REPLACE "\n" ";" src "${src}")
    set(${out} "${src}" PARENT_SCOPE)
endfunction()

macro(lint_unescape var)
    string(REPLACE "<SC>" ";" ${var} "${${var}}")
    string(REPLACE "<LB>" "[" ${var} "${${var}}")
    string(REPLACE "<RB>" "]" ${var} "${${var}}")
    string(REPLACE "<BS>" "\\" ${var} "${${var}}")
endmacro()
