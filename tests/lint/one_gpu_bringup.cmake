# tests/lint/one_gpu_bringup.cmake — windows bring their GPU up ONE way.
#
# "Prepare on a worker, wait a budget for frame one, take it over later"
# is platform/gpu_bringup.hpp. Wayland and Win32 each wrote it out (a pool,
# a job_result, a pending flag), and Win32 a third time for its prestart.
# So inside platform/ a jaal worker pool or job result outside that file is
# an error: hold a GpuBringUp.
#
#   cmake -DROOT=<mayag>/include/mayag -P one_gpu_bringup.cmake

file(GLOB fs RELATIVE ${ROOT} ${ROOT}/platform/*.hpp)
set(bad 0)
foreach(rel ${fs})
    if(rel STREQUAL "platform/gpu_bringup.hpp")
        continue()
    endif()
    file(STRINGS ${ROOT}/${rel} hits REGEX "jaal::kernel::(pool|job_result)[^a-z_]")
    if(hits)
        message(SEND_ERROR "one_gpu_bringup: ${rel} runs its own GPU worker; hold a GpuBringUp "
                           "(platform/gpu_bringup.hpp):\n${hits}")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "one_gpu_bringup: ${bad} file(s)")
endif()
message(STATUS "one_gpu_bringup: every window brings its GPU up one way (platform/gpu_bringup.hpp)")
