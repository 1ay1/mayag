# tests/lint/one_gpu_policy.cmake — the GPU knobs have ONE reader.
#
# MAYAG_BACKEND, MAYAG_GPU_WAIT_MS, MAYAG_GPU_ASYNC and MAYAG_PRESENT mean
# what backend/gpu_policy.hpp says they mean. They were read seven, two, two
# and three times, with different meanings ("gpu" was a word only Wayland
# knew, Win32 ignored MAYAG_GPU_ASYNC). So a getenv of any of them outside
# that file is an error; ask gpu_policy instead.
#
#   cmake -DROOT=<mayag>/include/mayag -P one_gpu_policy.cmake

file(GLOB_RECURSE fs RELATIVE ${ROOT} ${ROOT}/*.hpp)
set(bad 0)
foreach(rel ${fs})
    if(rel STREQUAL "backend/gpu_policy.hpp")
        continue()
    endif()
    file(STRINGS ${ROOT}/${rel} hits REGEX "getenv[ \t]*\\([ \t]*\"MAYAG_(BACKEND|GPU_WAIT_MS|GPU_ASYNC|PRESENT)\"")
    if(hits)
        message(SEND_ERROR "one_gpu_policy: ${rel} reads a GPU knob itself; ask backend::gpu_policy:\n${hits}")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "one_gpu_policy: ${bad} file(s)")
endif()
message(STATUS "one_gpu_policy: the GPU knobs are read in one place (backend/gpu_policy.hpp)")
