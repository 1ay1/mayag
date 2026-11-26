# tests/lint/one_pacing.cmake — frame timing is decided in ONE place.
#
# Every rule for WHEN a frame is drawn and how far motion advances lives in
# include/mayag/app/pacing.hpp, where tests/test_pacing.cpp drives it through
# thousands of jittered refreshes. Each rule there was once a bug that made
# motion jerk while every frame was "on time" (a held draw re-decided at a
# late wake: drags at 20-30 fps; motion stepped by wake-up time: judder).
# A second, private copy of that arithmetic in the host is how such a bug
# comes back untested. So the host may not:
#   - lay out the refresh grid itself  (vblank_ + n * period_, d / period_)
#   - step motion by wall-clock deltas (now - *last_tick_, now - last_draw_)
#   - re-decide a held deadline        (a held draw goes through Hold)
#
#   cmake -DROOT=<mayag>/include/mayag -P one_pacing.cmake

set(host ${ROOT}/app/host.hpp)
file(READ ${host} src)
set(bad 0)
foreach(pat
        "vblank_ \\+ [a-z_]+ \\* period_"
        "\\) / period_[^_]"
        "now - \\*last_tick_"
        "now - last_draw_"
        "now_\\(\\) - \\*last_tick_")
    string(REGEX MATCH "${pat}" hit "${src}")
    if(hit)
        message(SEND_ERROR "one_pacing: app/host.hpp computes frame timing itself (`${hit}`). Use mayag::pacing (app/pacing.hpp) so tests/test_pacing.cpp covers it.")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
# the pacing rules must stay wired: the host's slot math and deadlines go through them
foreach(need "grid\\(\\)\\.one_deep_deadline" "grid\\(\\)\\.full_queue_deadline" "hold_\\.decide" "pacing::tick_step" "tick_target")
    string(REGEX MATCH "${need}" hit "${src}")
    if(NOT hit)
        message(SEND_ERROR "one_pacing: app/host.hpp no longer uses `${need}` -- frame timing must go through app/pacing.hpp")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "one_pacing: ${bad} problem(s)")
endif()
message(STATUS "one_pacing: frame timing is decided in app/pacing.hpp only")
