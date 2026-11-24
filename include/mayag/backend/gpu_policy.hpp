#pragma once
// mayag::backend::gpu_policy — what the user asked of the GPU, read ONCE.
//
// Every window and device used to read the GPU environment knobs itself:
// MAYAG_BACKEND seven times (macos_window, wayland, win32_window three times,
// win32_gpu, backend.hpp), each with its own idea of what a value meant --
// "gpu" let Wayland's Vulkan run but nothing else heard of it; "software"
// was honoured by Win32 by string compare and by Cocoa only as "not metal";
// MAYAG_GPU_WAIT_MS was parsed twice with two different defaults, and
// MAYAG_PRESENT three times. Now each knob has ONE reader here and one
// meaning, and every backend asks a question:
//
//   gpu_policy::forced()              the backend MAYAG_BACKEND names, if any
//   gpu_policy::allows(api)           may this GPU backend be tried at all?
//   gpu_policy::prefers(api)          was it asked for by name (try it first)?
//   gpu_policy::first_frame_wait(d)   how long frame one waits for a GPU
//                                     still coming up (MAYAG_GPU_WAIT_MS)
//   gpu_policy::present_mode()        fifo / immediate / the device's choice
//
//   MAYAG_BACKEND   software | vulkan | metal | d3d11 | d3d12 | webgpu | opengl
//                   | gpu (any GPU, the platform's order) ; unset = the same
//   MAYAG_GPU_WAIT_MS   0 = draw on the CPU first and switch; default 3000
//                       for Vulkan, 1000 for D3D11 / Metal
//   MAYAG_GPU_ASYNC=0   wait for the GPU however long it takes
//   MAYAG_PRESENT   fifo | immediate ; unset = the device's low-latency choice
//
// Read on each call, not cached: a bench flips MAYAG_BACKEND between runs in
// one process (examples/present_bench.cpp), and these are read once per
// window or device, never per frame.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace mayag::backend::gpu_policy {

/// What MAYAG_BACKEND can name: the renderers, the Windows D3D11 path, and
/// "any GPU". Its own enum (not backend::Api) so this header depends on
/// nothing and every device header can include it.
enum class Choice : std::uint8_t { any_gpu, software, vulkan, metal, d3d11, d3d12, webgpu, opengl };

[[nodiscard]] constexpr std::optional<Choice> parse(std::string_view s) noexcept {
    if (s == "gpu") return Choice::any_gpu;
    if (s == "software" || s == "cpu") return Choice::software;
    if (s == "vulkan") return Choice::vulkan;
    if (s == "metal") return Choice::metal;
    if (s == "d3d11") return Choice::d3d11;
    if (s == "d3d12") return Choice::d3d12;
    if (s == "webgpu") return Choice::webgpu;
    if (s == "opengl") return Choice::opengl;
    return std::nullopt;   // unknown: ignored, as if unset
}

/// Where knob values come from: the real environment, or a test's.
using EnvLookup = const char* (*)(const char*);
[[nodiscard]] inline const char* real_env(const char* k) { return std::getenv(k); }

[[nodiscard]] inline std::optional<Choice> forced(EnvLookup env = real_env) {
    const char* v = env("MAYAG_BACKEND");
    if (v == nullptr || *v == '\0') return std::nullopt;
    return parse(v);
}

/// May GPU backend `api` be tried? Yes unless the user named something else
/// (another renderer, or software).
[[nodiscard]] inline bool allows(Choice api, EnvLookup env = real_env) {
    const auto f = forced(env);
    return !f || *f == Choice::any_gpu || *f == api;
}
/// Was `api` asked for by name? (Try it before the platform's default.)
[[nodiscard]] inline bool prefers(Choice api, EnvLookup env = real_env) {
    const auto f = forced(env);
    return f && *f == api;
}
/// Software was asked for: no GPU at all.
[[nodiscard]] inline bool software_only(EnvLookup env = real_env) {
    const auto f = forced(env);
    return f && *f == Choice::software;
}

/// How long the FIRST frame waits for a GPU that is still coming up, for a
/// device of kind `device`. A frame on the wrong renderer followed by a
/// switch is worse than waiting a moment. A cold Vulkan start on a hybrid
/// laptop can take seconds (3000 ms); D3D11 / Metal come up in well under
/// one (1000 ms). MAYAG_GPU_WAIT_MS overrides both (0: draw on the CPU first
/// and switch when ready); MAYAG_GPU_ASYNC=0 waits however long it takes.
[[nodiscard]] inline std::chrono::milliseconds first_frame_wait(Choice device, EnvLookup env = real_env) {
    if (const char* a = env("MAYAG_GPU_ASYNC"); a != nullptr && std::string_view{a} == "0")
        return std::chrono::milliseconds{1 << 30};
    if (const char* e = env("MAYAG_GPU_WAIT_MS"); e != nullptr && *e != '\0')
        return std::chrono::milliseconds{std::max(0, std::atoi(e))};
    return std::chrono::milliseconds{device == Choice::vulkan ? 3000 : 1000};
}

/// The swapchain's present mode the user forced, if any.
enum class Present : std::uint8_t { device_default, fifo, immediate };
[[nodiscard]] inline Present present_mode(EnvLookup env = real_env) {
    const char* v = env("MAYAG_PRESENT");
    if (v == nullptr) return Present::device_default;
    const std::string_view s{v};
    return s == "fifo" ? Present::fifo : s == "immediate" ? Present::immediate : Present::device_default;
}

}  // namespace mayag::backend::gpu_policy
