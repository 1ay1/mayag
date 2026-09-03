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
option(MAYAG_WITH_VULKAN    "Build the Vulkan backend"     OFF)

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
foreach(backend VULKAN METAL D3D12 WEBGPU OPENGL COCOA)
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

if(MAYAG_WITH_VULKAN)
    find_package(Vulkan REQUIRED)
    target_link_libraries(mayag INTERFACE Vulkan::Vulkan)
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

message(STATUS "")
message(STATUS "mayag ${PROJECT_VERSION}")
message(STATUS "  standard    C++${CMAKE_CXX_STANDARD}")
message(STATUS "  compiler    ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type  ${CMAKE_BUILD_TYPE}")
message(STATUS "  renderers   ${MAYAG_ENABLED_BACKENDS}")
message(STATUS "  windowing   ${MAYAG_WINDOWING}")
message(STATUS "")
