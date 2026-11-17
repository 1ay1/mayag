cmake_minimum_required(VERSION 3.24)
project(mayag VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "A GPU UI framework for C++26")

# ─────────────────────────────────────────────────────────────────────────
# Compiler cache (ccache / sccache)
#
# mayag is header-only in the parts that matter, so touching one header in
# include/mayag/app/ recompiles every example and test that includes it —
# and app.hpp is included by all of them. Most of those recompiles produce
# byte-identical preprocessed input, because the edit was in a different
# part of the header. ccache keys on CONTENT, so they become cache hits
# instead of 30-second rebuilds.
#
# Impossible to get wrong: with neither tool installed the launcher is not
# set and the build is exactly as before. Opt out with
# -DMAYAG_COMPILER_CACHE=OFF; a user-set *_COMPILER_LAUNCHER always wins.
# ─────────────────────────────────────────────────────────────────────────
option(MAYAG_COMPILER_CACHE "Use ccache/sccache as the compiler launcher when available" ON)
if(MAYAG_COMPILER_CACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(MAYAG_CCACHE NAMES ccache sccache)
    if(MAYAG_CCACHE)
        set(CMAKE_C_COMPILER_LAUNCHER   "${MAYAG_CCACHE}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${MAYAG_CCACHE}" CACHE STRING "" FORCE)
        message(STATUS "mayag: compiler cache enabled (${MAYAG_CCACHE})")
    else()
        message(STATUS "mayag: no ccache/sccache found — install ccache to make "
                       "header edits rebuild in seconds (pacman/apt/brew install ccache)")
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────
# Standard
#
# mayag uses C++26 for user-generated static_assert messages (P2741), which
# is what turns a DSL misuse into an English sentence instead of a template
# dump. MSVC's C++26 mode is not there yet, so it falls back to C++23 and the
# diagnostics degrade to a generic message — everything else works.
# ─────────────────────────────────────────────────────────────────────────
# A CMake older than the compiler (CMake 3.31 + AppleClang 21) doesn't list
# cxx_std_26 even though the compiler takes -std=c++2c; requiring the
# feature is then a hard configure error. Probe the flag and pass it
# directly instead (the same treatment jaal gives itself).
set(MAYAG_STD_FLAG "")
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
elseif("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    set(CMAKE_CXX_STANDARD 26)
else()
    set(CMAKE_CXX_STANDARD 23)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-std=c++2c" MAYAG_HAS_CXX2C)
    if(MAYAG_HAS_CXX2C)
        set(MAYAG_STD_FLAG "-std=c++2c")
    endif()
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

if(NOT MSVC AND "cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    target_compile_features(mayag INTERFACE cxx_std_26)
else()
    target_compile_features(mayag INTERFACE cxx_std_23)
    if(MAYAG_STD_FLAG)
        target_compile_options(mayag INTERFACE ${MAYAG_STD_FLAG})
    endif()
endif()

# jaal (third_party/jaal) is the runtime: the loop, timers, tasks, streams
# and the OS waiting. mayag is a jaal host that draws.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jaal/CMakeLists.txt")
    message(FATAL_ERROR "mayag: third_party/jaal is missing. Run: git submodule update --init")
endif()
add_subdirectory(third_party/jaal EXCLUDE_FROM_ALL)
target_link_libraries(mayag INTERFACE jaal::jaal)

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
        -Wdouble-promotion       # accidental float->double in a hot pixel loop
        # GCC (unlike clang) warns when a DESIGNATED initializer omits a
        # member that has a default -- `Spec{.key = k, .kind = Kind::choice}`.
        # Omitting defaulted members is the point of designated init, and
        # spelling every one out would make each call site restate the
        # struct. A member WITHOUT a default is still value-initialised.
        -Wno-missing-field-initializers)
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
            # Transpile render/kernel.hpp -> every shader dialect + SPIR-V.
            #
            # This is a BUILD STEP with a real dependency on the kernel, not a
            # configure-time convenience: editing kernel.hpp regenerates every
            # backend's shader on the next build, automatically. The previous
            # arrangement ran once at configure time and only WARNED on
            # failure, which is how the checked-in SPIR-V silently drifted
            # behind its own source and shipped shaders nobody had reviewed.
            #
            # If codegen fails the BUILD fails. A shader that does not match
            # kernel.hpp must never reach a GPU.
            set(MAYAG_SHADER_HEADER
                ${CMAKE_CURRENT_SOURCE_DIR}/include/mayag/render/shader_generated.hpp)
            add_custom_command(
                OUTPUT ${MAYAG_SHADER_HEADER}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_shaders.py
                        ${MAYAG_GLSLC}
                        ${MAYAG_SHADER_HEADER}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/mayag/render/kernel.hpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_shaders.py
                COMMENT "mayag: transpiling render/kernel.hpp -> shaders + SPIR-V"
                VERBATIM)
            add_custom_target(mayag_shaders DEPENDS ${MAYAG_SHADER_HEADER})
            # INTERFACE libraries carry dependencies to their consumers, so
            # every example/test that links mayag regenerates the shaders first.
            add_dependencies(mayag mayag_shaders)
        else()
            message(WARNING "mayag: no Python3; shaders cannot be regenerated "
                            "from kernel.hpp (using the checked-in header)")
        endif()
    else()
        message(STATUS "mayag: no glslc; using the checked-in generated shaders")
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
    # video.cpp needs ffmpeg and is built separately below, only if it is
    # present. mayag itself takes no dependencies; an example is allowed to,
    # as long as its absence costs nothing.
    list(REMOVE_ITEM MAYAG_EXAMPLES "${CMAKE_CURRENT_SOURCE_DIR}/examples/video.cpp")
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

    # ── the one example with a dependency ────────────────────────────
    #
    # video.cpp plays a real file through ffmpeg. It exists to show that
    # decoding is ORDINARY APPLICATION WORK -- a Sub::source producing frames,
    # Cmd::upload_image showing them -- and that mayag needs no special support
    # for it. Nothing in the library links against libav; if it is missing,
    # this example is skipped and everything else builds exactly as before.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
                          libavcodec libavformat libavutil libswscale)
    endif()
    if(TARGET PkgConfig::FFMPEG)
        add_executable(mayag_video "${CMAKE_CURRENT_SOURCE_DIR}/examples/video.cpp")
        target_link_libraries(mayag_video PRIVATE mayag PkgConfig::FFMPEG)
        target_include_directories(mayag_video
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
        target_compile_options(mayag_video PRIVATE ${MAYAG_WARNINGS})
        set_target_properties(mayag_video PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
        message(STATUS "mayag: ffmpeg found, building the video example")
    else()
        message(STATUS "mayag: no ffmpeg, skipping the video example")
    endif()
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

    # The cascade: inheritance, state variants, tokens, transitions. Mostly
    # pure style resolution, plus one live headless app; never a display.
    add_executable(mayag_cascade_tests tests/test_cascade.cpp)
    target_link_libraries(mayag_cascade_tests PRIVATE mayag)
    target_compile_options(mayag_cascade_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_cascade_tests PRIVATE MAYAG_PLATFORM_HEADLESS)
    add_test(NAME mayag_cascade_tests COMMAND mayag_cascade_tests)

    # The widget boundary (mayag.hpp includes no widget; nothing outside
    # widget/ includes one; widget/all.hpp is complete) and the DSL's
    # dynamic children.
    add_executable(mayag_widget_header_tests tests/test_widget_headers.cpp)
    target_link_libraries(mayag_widget_header_tests PRIVATE mayag)
    target_compile_options(mayag_widget_header_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_widget_header_tests PRIVATE MAYAG_PLATFORM_HEADLESS
        MAYAG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME mayag_widget_header_tests COMMAND mayag_widget_header_tests)

    # Inline event handlers (on_click, on_slide, on_enter ...) through the
    # real host and kernel, headless.
    add_executable(mayag_handler_tests tests/test_handlers.cpp)
    target_link_libraries(mayag_handler_tests PRIVATE mayag)
    target_compile_options(mayag_handler_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_handler_tests PRIVATE MAYAG_PLATFORM_HEADLESS)
    add_test(NAME mayag_handler_tests COMMAND mayag_handler_tests)

    # Deterministic simulation: the real host + kernel on jaal's seeded
    # world (simulated time, seeded task latencies, fault injection).
    add_executable(mayag_simulate_tests tests/test_simulate.cpp)
    target_link_libraries(mayag_simulate_tests PRIVATE mayag)
    target_compile_options(mayag_simulate_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_simulate_tests PRIVATE MAYAG_PLATFORM_HEADLESS)
    add_test(NAME mayag_simulate_tests COMMAND mayag_simulate_tests)

    # Themes: numeric tokens, derive/lerp/validate, widget sheets, subtree
    # themes, theme motion, and perf budgets on the hot paths.
    add_executable(mayag_theme_tests tests/test_theme.cpp)
    target_link_libraries(mayag_theme_tests PRIVATE mayag)
    target_compile_options(mayag_theme_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_theme_tests PRIVATE MAYAG_PLATFORM_HEADLESS)
    add_test(NAME mayag_theme_tests COMMAND mayag_theme_tests)

    # The window host on jaal, driven through the headless platform. It is the
    # same host run<P>() uses on a real window.
    add_executable(mayag_app_tests tests/test_app.cpp)
    target_link_libraries(mayag_app_tests PRIVATE mayag)
    target_compile_options(mayag_app_tests PRIVATE ${MAYAG_WARNINGS})
    # Force headless even on a machine with a window server: a test must never
    # depend on a display being present.
    target_compile_definitions(mayag_app_tests PRIVATE MAYAG_PLATFORM_HEADLESS)
    add_test(NAME mayag_app_tests COMMAND mayag_app_tests)

    add_executable(mayag_widget_behaviour_tests tests/test_widget_behaviour.cpp)
    target_link_libraries(mayag_widget_behaviour_tests PRIVATE mayag)
    target_compile_options(mayag_widget_behaviour_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_widget_behaviour_tests PRIVATE MAYAG_PLATFORM_HEADLESS)
    add_test(NAME mayag_widget_behaviour_tests COMMAND mayag_widget_behaviour_tests)

    add_executable(mayag_memo_tests tests/test_memo.cpp)
    target_link_libraries(mayag_memo_tests PRIVATE mayag)
    target_compile_options(mayag_memo_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_memo_tests PRIVATE MAYAG_PLATFORM_HEADLESS)
    add_test(NAME mayag_memo_tests COMMAND mayag_memo_tests)

    # The font engine, exercised against the REAL fonts on the machine:
    # ~1300 system faces swept, plus 400 fuzzed/corrupted files that must be
    # rejected without crashing.
    add_executable(mayag_font_tests tests/test_font.cpp)
    target_link_libraries(mayag_font_tests PRIVATE mayag)
    target_compile_options(mayag_font_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_font_tests COMMAND mayag_font_tests)

    add_executable(mayag_unicode_tests tests/test_unicode.cpp)
    target_link_libraries(mayag_unicode_tests PRIVATE mayag)
    target_compile_options(mayag_unicode_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_unicode_tests PRIVATE MAYAG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME mayag_unicode_tests COMMAND mayag_unicode_tests)

    # OpenType shaping must match HarfBuzz exactly: 500 captured hb-shape
    # outputs across ~200 system fonts and every complex script family.
    add_executable(mayag_shaping_tests tests/test_shaping.cpp)
    target_link_libraries(mayag_shaping_tests PRIVATE mayag)
    target_compile_options(mayag_shaping_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_shaping_tests PRIVATE MAYAG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME mayag_shaping_tests COMMAND mayag_shaping_tests)

    # Font discovery + the synthesized last-resort face. The last-resort checks
    # need no files and run everywhere; the discovery checks assert invariants
    # ("if a face covers a script, covering() finds it") rather than the
    # presence of any particular font, so they hold on a minimal CI image too.
    add_executable(mayag_discovery_tests tests/test_discovery.cpp)
    target_link_libraries(mayag_discovery_tests PRIVATE mayag)
    target_compile_options(mayag_discovery_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_discovery_tests COMMAND mayag_discovery_tests)

    # Live / streaming on jaal: streams, timers, and a real Wayland window
    # that wakes when a background thread posts. The window half skips
    # without a compositor.
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

    # The backend boundary: builds a backend for a nonexistent API using only
    # the public interface, so it fails if adding a backend ever requires
    # touching the core again. Deliberately NOT inside any MAYAG_WITH_* guard —
    # the property it checks is that no such guard is needed.
    # The frame pipeline, with numbers that survive.
    add_executable(mayag_perf_tests tests/test_perf.cpp)
    target_link_libraries(mayag_perf_tests PRIVATE mayag)
    target_compile_options(mayag_perf_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_perf_tests COMMAND mayag_perf_tests)

    # Menus, modals and tooltips must land on screen.
    add_executable(mayag_overlay_tests tests/test_overlay.cpp)
    target_link_libraries(mayag_overlay_tests PRIVATE mayag)
    target_compile_options(mayag_overlay_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_overlay_tests COMMAND mayag_overlay_tests)

    # Editing must operate on graphemes, not codepoints.
    add_executable(mayag_text_edit_tests tests/test_text_edit.cpp)
    target_link_libraries(mayag_text_edit_tests PRIVATE mayag)
    target_compile_options(mayag_text_edit_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_text_edit_tests COMMAND mayag_text_edit_tests)

    # The PNG decoder parses untrusted input: fuzz it like the font parser.
    add_executable(mayag_png_tests tests/test_png.cpp)
    target_link_libraries(mayag_png_tests PRIVATE mayag)
    target_compile_options(mayag_png_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_png_tests COMMAND mayag_png_tests)

    # The same sweep under ASan+UBSan, where an out-of-bounds read in the
    # inflater actually fails instead of quietly returning wrong pixels.
    if(NOT MSVC)
        add_executable(mayag_png_asan EXCLUDE_FROM_ALL tests/test_png.cpp)
        target_link_libraries(mayag_png_asan PRIVATE mayag)
        target_compile_options(mayag_png_asan PRIVATE
                               -fsanitize=address,undefined -O1 -g)
        target_link_options(mayag_png_asan PRIVATE -fsanitize=address,undefined)
    endif()

    # Cmd::task / Cmd::stream / Sub::source under real contention.
    add_executable(mayag_concurrency_tests tests/test_concurrency.cpp)
    target_link_libraries(mayag_concurrency_tests PRIVATE mayag)
    target_compile_options(mayag_concurrency_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_concurrency_tests COMMAND mayag_concurrency_tests)

    # Zero-copy buffer import: the substrate a compositor is built on.
    add_executable(mayag_buffer_import_tests tests/test_buffer_import.cpp)
    target_link_libraries(mayag_buffer_import_tests PRIVATE mayag)
    target_compile_options(mayag_buffer_import_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_buffer_import_tests COMMAND mayag_buffer_import_tests)

    # Outputs and hardware plane assignment: the compositor-side display path.
    add_executable(mayag_output_tests tests/test_output.cpp)
    target_link_libraries(mayag_output_tests PRIVATE mayag)
    target_compile_options(mayag_output_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_output_tests COMMAND mayag_output_tests)

    # A program driven by a loop mayag doesn't own: the macOS path
    # (embedded_app), run against a fake run loop so it's tested everywhere.
    add_executable(mayag_embedded_tests tests/test_embedded.cpp)
    target_link_libraries(mayag_embedded_tests PRIVATE mayag)
    target_compile_options(mayag_embedded_tests PRIVATE ${MAYAG_WARNINGS})
    target_compile_definitions(mayag_embedded_tests PRIVATE MAYAG_PLATFORM_HEADLESS)
    add_test(NAME mayag_embedded_tests COMMAND mayag_embedded_tests)

    # Layering: apps talk to mayag only. jaal is mayag's runtime, never an
    # app's dependency, so no example (and no test, outside comments) may name
    # it. Every runtime concept an app needs has a mayag name (Cmd, Sub,
    # Sink, Shared, TaskKey, Signal, Unlocked, CoOwned, Guarded, Effect,
    # MAYAG_SENDABLE), so there is never a reason to.
    add_test(NAME mayag_examples_use_only_mayag
             COMMAND ${CMAKE_COMMAND} -E env sh -c
                     "! grep -rnE 'jaal' '${CMAKE_CURRENT_SOURCE_DIR}/examples'")
    add_test(NAME mayag_tests_use_only_mayag
             COMMAND ${CMAKE_COMMAND} -E env sh -c
                     "! sed 's|//.*||' '${CMAKE_CURRENT_SOURCE_DIR}'/tests/*.cpp | grep -nE 'jaal(::|/)'")

    # ... and mayag talks to jaal only through jaal's PUBLIC API: no
    # detail:: namespaces, no kernel keys (pool_access, cap_access), no
    # minted capabilities (thread_root). If mayag needs something jaal keeps
    # private, the fix is a public API in jaal, not a reach past it.
    add_test(NAME mayag_uses_jaal_public_api
             COMMAND ${CMAKE_COMMAND} -E env sh -c
                     "! find '${CMAKE_CURRENT_SOURCE_DIR}/include/mayag' -name '*.hpp' -exec sed 's|//.*||' {} + | grep -nE 'jaal::detail|pool_access|cap_access|thread_root|sink_access|loop_key'")

    # Runtime primitives: jaal's own concurrency banlist, pointed at mayag.
    # Raw threads, mutexes, atomics, thread_locals, futures and const_cast
    # are allowed only where tests/lint/allow_include.txt says, with a reason
    # a human signed; apps (examples/) get an EMPTY allowlist. The second
    # half fails when a grant is no longer used, so the list cannot rot.
    set(_MAYAG_BANLIST ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jaal/tests/lint/banlist.cmake)
    set(_MAYAG_PRUNE   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jaal/tests/lint/prune_allowlist.cmake)
    if(EXISTS ${_MAYAG_BANLIST})
        add_test(NAME mayag_runtime_banlist
                 COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include/mayag
                         -DALLOW=${CMAKE_CURRENT_SOURCE_DIR}/tests/lint/allow_include.txt
                         -P ${_MAYAG_BANLIST})
        add_test(NAME mayag_runtime_allowlist_tight
                 COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include/mayag
                         -DALLOW=${CMAKE_CURRENT_SOURCE_DIR}/tests/lint/allow_include.txt
                         -P ${_MAYAG_PRUNE})
        add_test(NAME mayag_examples_banlist
                 COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/examples
                         -DALLOW=${CMAKE_CURRENT_SOURCE_DIR}/tests/lint/allow_examples.txt
                         -P ${_MAYAG_BANLIST})
    endif()

    # The same test under ThreadSanitizer. Not part of the default suite --
    # TSan is ~10x slower and needs a compatible toolchain -- but it is the
    # only thing that finds a race which does not happen to fire today.
    # Build and run explicitly:  ninja mayag_concurrency_tsan && ./mayag_concurrency_tsan
    if(NOT MSVC AND NOT APPLE)
        add_executable(mayag_concurrency_tsan EXCLUDE_FROM_ALL
                       tests/test_concurrency.cpp)
        target_link_libraries(mayag_concurrency_tsan PRIVATE mayag)
        target_compile_options(mayag_concurrency_tsan PRIVATE -fsanitize=thread -O1 -g)
        target_link_options(mayag_concurrency_tsan PRIVATE -fsanitize=thread)
    endif()

    # Virtualised cost must be flat in the row count, not just look right.
    add_executable(mayag_scaling_tests tests/test_scaling.cpp)
    target_link_libraries(mayag_scaling_tests PRIVATE mayag)
    target_compile_options(mayag_scaling_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_scaling_tests COMMAND mayag_scaling_tests)

    # The code editor's text model and syntax layer.
    add_executable(mayag_editor_tests tests/test_editor.cpp)
    target_link_libraries(mayag_editor_tests PRIVATE mayag)
    target_compile_options(mayag_editor_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_editor_tests COMMAND mayag_editor_tests)

    # image() must render on every backend, not just compile.
    add_executable(mayag_texture_tests tests/test_texture.cpp)
    target_link_libraries(mayag_texture_tests PRIVATE mayag)
    target_compile_options(mayag_texture_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_texture_tests COMMAND mayag_texture_tests)

    add_executable(mayag_backend_tests tests/test_backend_api.cpp)
    target_link_libraries(mayag_backend_tests PRIVATE mayag)
    target_compile_options(mayag_backend_tests PRIVATE ${MAYAG_WARNINGS})
    add_test(NAME mayag_backend_tests COMMAND mayag_backend_tests)

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
        foreach(demo counter gallery dashboard typography todo reactive aurora player terminal_settings editor terminal_widgets agent_terminal terminal styled stylebook studio)
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
        dangling_text
        text_prop_on_empty_box
        variant_border_color_without_border
        class_member_checked
        negate_compound_selector)

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

install(TARGETS mayag jaal EXPORT mayagTargets FILE_SET HEADERS)
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

# The package config file `find_package(mayag)` actually looks for. Without it
# a consuming project cannot locate mayag at all, even once installed.
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mayagConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mayagConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mayag)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/mayagConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/mayagConfigVersion.cmake"
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
