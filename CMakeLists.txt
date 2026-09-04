cmake_minimum_required(VERSION 3.24)
project(mayag VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "A GPU UI framework for C++26")

# ─────────────────────────────────────────────────────────────────────────
# Standard
#
# mayag uses C++26 for user-generated static_assert messages (P2741), which
# is what turns a DSL misuse into an English sentence instead of a template
# dump. MSVC's C++26 mode is not there yet, so it falls back to C++23 and the
# diagnostics degrade to a generic message — everything else works.
# ─────────────────────────────────────────────────────────────────────────
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

# ─────────────────────────────────────────────────────────────────────────
# Options
# ─────────────────────────────────────────────────────────────────────────
option(MAYAG_BUILD_EXAMPLES "Build the examples"           ON)
option(MAYAG_BUILD_TESTS    "Build the test suite"         ON)
# Vulkan is the default GPU backend everywhere except Apple (which uses Metal).
# Like the Wayland backend it links NOTHING: libvulkan is dlopen'd at runtime
# and the ABI is transcribed by hand, so there is no Vulkan SDK build
# dependency and a binary built with it still runs where Vulkan is absent (it
# falls back to the software rasteriser). ON by default so the GPU path is the
# default on every platform.
if(APPLE)
    option(MAYAG_WITH_VULKAN "Build the Vulkan backend" OFF)
else()
    option(MAYAG_WITH_VULKAN "Build the Vulkan backend" ON)
endif()

# mayag is a GPU framework, so the GPU backend is ON wherever the platform
# has one. The software rasteriser does not go away — it is the reference
# every GPU result is diffed against, and the fallback when a machine has no
# device — but it stops being what apps get by DEFAULT.
#
# The measured difference is not marginal. Same 303-instance draw list, same
# process, M1: 4.96 ms of CPU at 2000x1440 and 14.07 ms at 4K on the CPU
# path, versus 0.04 ms on Metal at both. Shipping the 350x-slower default
# because it is the safer-sounding one would be the wrong trade for a
# framework whose whole design — one SDF kernel, one instance struct, one
# draw call per batch — exists to feed a GPU.
if(APPLE)
    option(MAYAG_WITH_METAL "Build the Metal backend"      ON)
else()
    option(MAYAG_WITH_METAL "Build the Metal backend"      OFF)
endif()

option(MAYAG_WITH_D3D12     "Build the D3D12 backend"      OFF)
option(MAYAG_WITH_WEBGPU    "Build the WebGPU backend"     OFF)
option(MAYAG_WITH_OPENGL    "Build the OpenGL backend"     OFF)

# Windowing. Without a window backend mayag still builds and RUNS — the
# headless platform is always available, which is what lets the full app
# runtime be tested on a build server with no display. Default ON where a
# native windowing system exists.
if(APPLE)
    option(MAYAG_WITH_COCOA "Build the macOS window backend" ON)
else()
    option(MAYAG_WITH_COCOA "Build the macOS window backend" OFF)
endif()

# Wayland is the native Linux/BSD backend. It is ON by default because it
# costs nothing to enable: the client library is resolved with dlopen() at
# RUNTIME, so there is no build-time dependency, nothing to find_package, and
# a binary built with it still runs on a machine with no Wayland at all (the
# loader fails, open() returns nullopt, and the app falls back to headless).
if(CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD")
    option(MAYAG_WITH_WAYLAND "Build the Wayland window backend" ON)
else()
    option(MAYAG_WITH_WAYLAND "Build the Wayland window backend" OFF)
endif()

# `-march=native` bakes the BUILD host's microarchitecture into the binary and
# SIGILLs on any older chip. Fine for local dev, never for a shipped artifact,
# so it is opt-in rather than a Release default.
option(MAYAG_NATIVE_TUNING  "Tune for the build host (NOT portable)" OFF)

# ─────────────────────────────────────────────────────────────────────────
# The library
#
# mayag is header-only. There is no .cpp to link, no ABI to match, and no
# build-order dependency — you can drop `include/` into a project and be done.
# ─────────────────────────────────────────────────────────────────────────
add_library(mayag INTERFACE)
add_library(mayag::mayag ALIAS mayag)

target_include_directories(mayag INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(mayag INTERFACE
    $<IF:$<CXX_COMPILER_ID:MSVC>,cxx_std_23,cxx_std_26>)

# ── backend feature macros ───────────────────────────────────────────────
foreach(backend VULKAN METAL D3D12 WEBGPU OPENGL COCOA WAYLAND)
    if(MAYAG_WITH_${backend})
        target_compile_definitions(mayag INTERFACE MAYAG_WITH_${backend})
    endif()
endforeach()

# ── windowing ────────────────────────────────────────────────────────────
#
# The Cocoa backend talks to AppKit through the Objective-C runtime C API
# rather than through an .mm file, which is what keeps mayag header-only.
# It links Cocoa + CoreGraphics; both ship with every macOS.
if(MAYAG_WITH_COCOA)
    if(NOT APPLE)
        message(FATAL_ERROR "mayag: MAYAG_WITH_COCOA requires macOS.")
    endif()
    find_library(COCOA_LIB Cocoa REQUIRED)
    find_library(COREGRAPHICS_LIB CoreGraphics REQUIRED)
    target_link_libraries(mayag INTERFACE ${COCOA_LIB} ${COREGRAPHICS_LIB} objc)
endif()

# The Wayland backend links libdl and libpthread only — NOT libwayland-client.
# Every Wayland symbol is resolved through dlopen/dlsym at startup, so mayag
# keeps its "depends on nothing" property: no wayland-scanner, no generated
# protocol sources, no version skew between the headers on the build machine
# and the .so on the user's.
if(MAYAG_WITH_WAYLAND)
    if(NOT CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD")
        message(FATAL_ERROR "mayag: MAYAG_WITH_WAYLAND requires Linux or FreeBSD.")
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(mayag INTERFACE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# ── warnings ─────────────────────────────────────────────────────────────
if(MSVC)
    set(MAYAG_WARNINGS /W4 /permissive-)
else()
    set(MAYAG_WARNINGS
        -Wall -Wextra -Wpedantic
        -Wshadow                 # a shadowed `size` in layout code is a real bug
        -Wconversion             # float/int narrowing matters in a geometry lib
        -Wno-sign-conversion     # too noisy against std::size_t indexing
        -Wnon-virtual-dtor
        -Wdouble-promotion)      # accidental float->double in a hot pixel loop
endif()

if(MAYAG_NATIVE_TUNING AND NOT MSVC)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" MAYAG_HAS_MARCH_NATIVE)
    if(MAYAG_HAS_MARCH_NATIVE)
        target_compile_options(mayag INTERFACE $<$<CONFIG:Release>:-march=native>)
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────
# Platform backends
# ─────────────────────────────────────────────────────────────────────────
if(MAYAG_WITH_METAL)
    if(NOT APPLE)
        message(FATAL_ERROR "mayag: MAYAG_WITH_METAL requires Apple platforms.")
    endif()
    find_library(METAL_LIB Metal REQUIRED)
    find_library(QUARTZCORE_LIB QuartzCore REQUIRED)
    find_library(FOUNDATION_LIB Foundation REQUIRED)
    target_link_libraries(mayag INTERFACE ${METAL_LIB} ${QUARTZCORE_LIB} ${FOUNDATION_LIB})
endif()

# The Vulkan backend links libdl only — NOT libvulkan. Every Vulkan symbol is
# resolved through dlopen/dlsym at startup and the ABI is redeclared by hand,
# so there is no Vulkan SDK build dependency, no header/loader version skew,
# and a Vulkan-enabled binary still runs on a box with no Vulkan (it falls
# back to software). The one shader is compiled to SPIR-V at configure time
# when glslc is available; the result is checked in, so a build with no shader
# compiler still has a working GPU path.
if(MAYAG_WITH_VULKAN)
    if(APPLE)
        message(FATAL_ERROR "mayag: use MAYAG_WITH_METAL on Apple, not Vulkan.")
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(mayag INTERFACE ${CMAKE_DL_LIBS} Threads::Threads)

    find_program(MAYAG_GLSLC NAMES glslc glslcValidator)
    if(MAYAG_GLSLC)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            message(STATUS "mayag: regenerating SPIR-V with ${MAYAG_GLSLC}")
            execute_process(
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py
                        ${MAYAG_GLSLC}
                        ${CMAKE_CURRENT_SOURCE_DIR}/include/mayag/render/spirv_generated.hpp
                RESULT_VARIABLE MAYAG_SPIRV_RC
                OUTPUT_VARIABLE MAYAG_SPIRV_OUT)
            if(MAYAG_SPIRV_RC EQUAL 0)
                message(STATUS "mayag: ${MAYAG_SPIRV_OUT}")
            else()
                message(WARNING "mayag: SPIR-V regeneration failed; using checked-in header")
            endif()
        endif()
    endif()
endif()

if(MAYAG_WITH_D3D12)
    if(NOT WIN32)
        message(FATAL_ERROR "mayag: MAYAG_WITH_D3D12 requires Windows.")
    endif()
    target_link_libraries(mayag INTERFACE d3d12 dxgi d3dcompiler)
endif()

if(MAYAG_WITH_OPENGL)
    find_package(OpenGL REQUIRED)
    target_link_libraries(mayag INTERFACE OpenGL::GL)
endif()

# ─────────────────────────────────────────────────────────────────────────
# Examples
# ─────────────────────────────────────────────────────────────────────────
if(MAYAG_BUILD_EXAMPLES)
    file(GLOB MAYAG_EXAMPLES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
    foreach(example_src ${MAYAG_EXAMPLES})
        get_filename_component(example_name ${example_src} NAME_WE)
        add_executable(mayag_${example_name} ${example_src})
        target_link_libraries(mayag_${example_name} PRIVATE mayag)
        # Examples share examples/harness.hpp, which gives each of them
        # --window / --png / --headless from one main().
        target_include_directories(mayag_${example_name}
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
        target_compile_options(mayag_${example_name} PRIVATE ${MAYAG_WARNINGS})
        set_target_properties(mayag_${example_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
    endforeach()
endif()

# ─────────────────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────────────────
if(MAYAG_BUILD_TESTS)
    enable_testing()

    # Rendering: numeric kernels, layout arithmetic, and real pixels.
    add_executable(mayag_tests tests/test_mayag.cpp)
    target_link_libraries(mayag_tests PRIVATE mayag)
    target_compile_options(mayag_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_tests COMMAND mayag_tests)
    set_tests_properties(mayag_tests PROPERTIES TIMEOUT 120)

    # The application runtime, driven through the headless platform. This is
    # the SAME Runtime<P, W> that run<P>() instantiates on a real window, so
    # these tests are evidence about shipped behaviour.
    add_executable(mayag_app_tests tests/test_app.cpp)
    target_link_libraries(mayag_app_tests PRIVATE mayag)
    target_compile_options(mayag_app_tests PRIVATE ${MAYAG_WARNINGS})
    # Force headless even on a machine with a window server: a test must never
    # depend on a display being present.
    target_compile_definitions(mayag_app_tests PRIVATE MAYAG_PLATFORM_HEADLESS)
    add_test(NAME mayag_app_tests COMMAND mayag_app_tests)

    # The font engine, exercised against the REAL fonts on the machine:
    # ~1300 system faces swept, plus 400 fuzzed/corrupted files that must be
    # rejected without crashing.
    add_executable(mayag_font_tests tests/test_font.cpp)
    target_link_libraries(mayag_font_tests PRIVATE mayag)
    target_compile_options(mayag_font_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_font_tests COMMAND mayag_font_tests)

    # Font discovery + the synthesized last-resort face. The last-resort checks
    # need no files and run everywhere; the discovery checks assert invariants
    # ("if a face covers a script, covering() finds it") rather than the
    # presence of any particular font, so they hold on a minimal CI image too.
    add_executable(mayag_discovery_tests tests/test_discovery.cpp)
    target_link_libraries(mayag_discovery_tests PRIVATE mayag)
    target_compile_options(mayag_discovery_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_discovery_tests COMMAND mayag_discovery_tests)

    # Live / streaming: the cross-thread waker and Cmd::stream. The waker and
    # the stream lifecycle are pure logic and run everywhere; the end-to-end
    # "a blocked window wakes" check needs a compositor and skips without one.
    if(MAYAG_WITH_WAYLAND)
        add_executable(mayag_live_tests tests/test_live.cpp)
        target_link_libraries(mayag_live_tests PRIVATE mayag)
        target_compile_options(mayag_live_tests PRIVATE ${MAYAG_WARNINGS})
        add_test(NAME mayag_live_tests COMMAND mayag_live_tests)
    endif()

    # The GPU backend, checked against the software rasteriser pixel by pixel.
    # A timing benchmark cannot distinguish a fast backend from one that draws
    # nothing; this can, so the performance claim stays falsifiable. Skips
    # itself cleanly when the machine has no Metal device.
    if(MAYAG_WITH_METAL)
        add_executable(mayag_metal_tests tests/test_metal.cpp)
        target_link_libraries(mayag_metal_tests PRIVATE mayag)
        target_compile_options(mayag_metal_tests PRIVATE ${MAYAG_WARNINGS})
        add_test(NAME mayag_metal_tests COMMAND mayag_metal_tests)
    endif()

    # The Vulkan backend, checked against the software rasteriser exactly like
    # Metal. Skips itself when no Vulkan device (nor llvmpipe) is present, so a
    # GPU-less CI runner stays green while a machine with a GPU verifies the
    # cross-platform default renderer for real.
    if(MAYAG_WITH_VULKAN)
        add_executable(mayag_vulkan_tests tests/test_vulkan.cpp)
        target_link_libraries(mayag_vulkan_tests PRIVATE mayag)
        target_compile_options(mayag_vulkan_tests PRIVATE ${MAYAG_WARNINGS})
        add_test(NAME mayag_vulkan_tests COMMAND mayag_vulkan_tests)
    endif()

    # The Linux backend. The protocol tables, the evdev keymap and the damage
    # arithmetic are pure logic and run everywhere — they are where the real
    # bugs were, since a wrong signature or a shifted argument is invisible
    # until it corrupts the wire. The live-window half skips itself cleanly
    # when there is no compositor, so CI stays green on a headless runner.
    if(MAYAG_WITH_WAYLAND)
        add_executable(mayag_wayland_tests tests/test_wayland.cpp)
        target_link_libraries(mayag_wayland_tests PRIVATE mayag)
        target_compile_options(mayag_wayland_tests PRIVATE ${MAYAG_WARNINGS})
        add_test(NAME mayag_wayland_tests COMMAND mayag_wayland_tests)
    endif()

    # Every example doubles as an end-to-end test. `--headless` drives the
    # SAME Program the windowed build runs, so a green test here is evidence
    # about the shipped app rather than about a parallel harness.
    if(MAYAG_BUILD_EXAMPLES)
        foreach(demo counter gallery dashboard typography todo reactive)
            if(TARGET mayag_${demo})
                add_test(NAME mayag_${demo}_headless
                         COMMAND mayag_${demo} --headless)
            endif()
        endforeach()
    endif()

    # Negative compilation tests: each of these MUST fail to compile. A
    # type-state DSL whose invalid states are only *supposed* to be
    # unrepresentable is worth nothing — this proves they are.
    set(MAYAG_MUST_FAIL
        border_color_without_border
        bold_on_a_box
        gap_without_children
        grow_on_absolute
        srgb_interp_without_gradient
        duplicate_width
        size_then_height
        dangling_text)

    foreach(case ${MAYAG_MUST_FAIL})
        add_executable(mayag_nc_${case} EXCLUDE_FROM_ALL tests/negative/${case}.cpp)
        target_link_libraries(mayag_nc_${case} PRIVATE mayag)
        add_test(NAME mayag_rejects_${case}
                 COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                          --target mayag_nc_${case} --config $<CONFIG>)
        # These drive a NESTED cmake build. Two of them running at once (or
        # one running while the outer build is still going) contend for the
        # same ninja lock and can stall for minutes. RUN_SERIAL keeps them
        # one-at-a-time; each takes ~2 s.
        set_tests_properties(mayag_rejects_${case} PROPERTIES
                             WILL_FAIL TRUE
                             RUN_SERIAL TRUE
                             TIMEOUT 120)
    endforeach()
endif()

# ─────────────────────────────────────────────────────────────────────────
# Install
# ─────────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS mayag EXPORT mayagTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT mayagTargets
        FILE mayagTargets.cmake
        NAMESPACE mayag::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mayag)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mayagConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/mayagConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mayag)

# ─────────────────────────────────────────────────────────────────────────
set(MAYAG_ENABLED_BACKENDS "software")
foreach(backend vulkan metal d3d12 webgpu opengl)
    string(TOUPPER ${backend} BACKEND_UC)
    if(MAYAG_WITH_${BACKEND_UC})
        string(APPEND MAYAG_ENABLED_BACKENDS " ${backend}")
    endif()
endforeach()

set(MAYAG_WINDOWING "headless")
if(MAYAG_WITH_COCOA)
    set(MAYAG_WINDOWING "cocoa + headless")
endif()
if(MAYAG_WITH_WAYLAND)
    set(MAYAG_WINDOWING "wayland + headless")
endif()

message(STATUS "")
message(STATUS "mayag ${PROJECT_VERSION}")
message(STATUS "  standard    C++${CMAKE_CXX_STANDARD}")
message(STATUS "  compiler    ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type  ${CMAKE_BUILD_TYPE}")
message(STATUS "  renderers   ${MAYAG_ENABLED_BACKENDS}")
message(STATUS "  windowing   ${MAYAG_WINDOWING}")
message(STATUS "")
