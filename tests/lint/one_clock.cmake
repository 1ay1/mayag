# tests/lint/one_clock.cmake — a widget animates on the frame's own clock.
#
# The bug, three times in hand: a spinner, a busy ring and a dialog pulse
# were handed the APP's clock (m.time) through a widget's `double time`
# parameter. That clock moves only while the app subscribes to frames, so the
# moment nothing else animated, the spinner stopped -- "the tab spinner lags".
# A widget that takes a clock can be handed the wrong one; a widget that reads
# Ctx::time itself (frame_time(c)) and books its next step (stepper /
# ambient_time) cannot.
#
# So: no widget taking a context (`const C& c`) may also take a `double time`
# / `double now` parameter, or carry a `time`/`now` clock field in its props.
# Low-level drawing helpers that take a Theme (spinner_frame, agent_glyph,
# tab_icon) are called FROM widgets, which pass frame_time(c).
#
#   cmake -DROOT=<mayag>/include/mayag -P one_clock.cmake

file(GLOB_RECURSE fs RELATIVE ${ROOT} ${ROOT}/widget/*.hpp)
set(bad 0)
foreach(rel ${fs})
    file(READ ${ROOT}/${rel} src)
    # one signature per match: a template'd function taking `const C& c`, up to its `{` or `;`
    string(REGEX MATCHALL "inline [^;{(]*\\(const C& c[^;{]*[;{]" sigs "${src}")
    foreach(s ${sigs})
        if(s MATCHES "double[ \t\n]+(time|now)[ \t\n]*[,=)]" OR s MATCHES "double[ \t\n]*/\\*[ \t]*(time|now)[ \t]*\\*/")
            string(REGEX MATCH "inline [^(]*\\(" name "${s}")
            message(SEND_ERROR "one_clock: ${rel}: `${name}...` takes a clock. Read frame_time(c) inside and book the next step (stepper / ambient_time); see tests/lint/one_clock.cmake")
            math(EXPR bad "${bad}+1")
        endif()
    endforeach()
    # a clock in a props struct is the same parameter in disguise
    if(src MATCHES "\n[ \t]+double[ \t]+(now|time)[ \t]*=[^\n]*///<")
        message(SEND_ERROR "one_clock: ${rel}: a props struct carries a clock (`double ${CMAKE_MATCH_1}`); the widget reads frame_time(c)")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
list(LENGTH fs n)
if(bad EQUAL 0)
    message(STATUS "one_clock: ${n} widget headers animate on the frame clock")
endif()
