#pragma once
// mayag::backend — the GPU abstraction
//
// mayag's portability claim rests on a deliberately TINY backend interface.
// A backend does four things: create a device, own a swapchain, upload two
// buffers, and issue N instanced draws. It never sees widgets, layout,
// styles, or colour spaces — those are all resolved before this boundary.
//
// That is why adding a backend is a few hundred lines rather than a rewrite,
// and why the software rasteriser is a peer of Vulkan rather than a special
// case.
//
// Selection at runtime, in order:
//   1. MAYAG_BACKEND env var (vulkan|metal|d3d12|webgpu|opengl|software)
//   2. the platform's native API
//   3. a portable GPU API
//   4. software — always available, never fails
//
// A backend that fails to initialise falls through to the next candidate, so
// a missing driver degrades to a slower frame instead of a crash.

#include "../render/draw_list.hpp"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::backend {

enum class Api : std::uint8_t {
    software,   ///< CPU reference rasteriser — always present
    vulkan,     ///< Linux, Windows, Android
    metal,      ///< macOS, iOS
    d3d12,      ///< Windows
    webgpu,     ///< Web (Emscripten) and native via Dawn/wgpu
    opengl,     ///< the universal fallback for old hardware
};

[[nodiscard]] constexpr std::string_view name_of(Api api) noexcept {
    switch (api) {
        case Api::software: return "software";
        case Api::vulkan:   return "vulkan";
        case Api::metal:    return "metal";
        case Api::d3d12:    return "d3d12";
        case Api::webgpu:   return "webgpu";
        case Api::opengl:   return "opengl";
    }
    return "unknown";
}

[[nodiscard]] constexpr Api api_from_name(std::string_view s) noexcept {
    if (s == "vulkan") return Api::vulkan;
    if (s == "metal")  return Api::metal;
    if (s == "d3d12")  return Api::d3d12;
    if (s == "webgpu") return Api::webgpu;
    if (s == "opengl") return Api::opengl;
    return Api::software;
}

// ── platform detection ──────────────────────────────────────────────────

/// Backends this binary was COMPILED with. Distinct from what is available at
/// runtime — a build can support Vulkan while the running machine has no
/// driver, and conflating the two is the classic source of "works on my
/// machine" GPU bugs.
[[nodiscard]] inline std::vector<Api> compiled_backends() {
    std::vector<Api> v;
#if defined(MAYAG_WITH_VULKAN)
    v.push_back(Api::vulkan);
#endif
#if defined(MAYAG_WITH_METAL)
    v.push_back(Api::metal);
#endif
#if defined(MAYAG_WITH_D3D12)
    v.push_back(Api::d3d12);
#endif
#if defined(MAYAG_WITH_WEBGPU)
    v.push_back(Api::webgpu);
#endif
#if defined(MAYAG_WITH_OPENGL)
    v.push_back(Api::opengl);
#endif
    v.push_back(Api::software);   // unconditional
    return v;
}

/// The platform's preferred order, best first.
[[nodiscard]] inline std::vector<Api> preferred_order() {
    std::vector<Api> v;
#if defined(__APPLE__)
    v = {Api::metal, Api::vulkan, Api::opengl};
#elif defined(_WIN32)
    v = {Api::d3d12, Api::vulkan, Api::opengl};
#elif defined(__EMSCRIPTEN__)
    v = {Api::webgpu, Api::opengl};
#elif defined(__ANDROID__)
    v = {Api::vulkan, Api::opengl};
#else
    v = {Api::vulkan, Api::opengl};
#endif
    v.push_back(Api::software);
    return v;
}

// ── device interface ────────────────────────────────────────────────────

struct DeviceInfo {
    Api         api = Api::software;
    std::string adapter = "CPU";
    bool        discrete = false;
    std::uint32_t max_texture_size = 8192;
};

struct FrameStats {
    std::uint32_t instances  = 0;
    std::uint32_t draw_calls = 0;
    double        cpu_ms     = 0.0;
    double        gpu_ms     = 0.0;
};

/// What every backend implements. Note what is NOT here: no shaders, no
/// pipelines, no descriptor sets, no render passes. Those are backend-private
/// because they differ irreconcilably between APIs, and leaking them into a
/// common interface is what makes most abstractions leak.
class Device {
  public:
    virtual ~Device() = default;

    [[nodiscard]] virtual DeviceInfo info() const = 0;

    /// Resize the target surface. Backends recreate swapchains here.
    virtual void resize(int width, int height) = 0;

    /// Submit one frame's draw list. `clear` is the background colour.
    virtual FrameStats submit(const DrawList& list, Color<Srgb> clear) = 0;

    /// Upload an RGBA8 texture; returns a slot id usable in `Instance`.
    /// Slot 0 is reserved for "no texture".
    virtual std::uint32_t upload_texture(std::span<const std::uint8_t> rgba,
                                         int width, int height) = 0;

    virtual void release_texture(std::uint32_t slot) = 0;

    /// Read the target back as RGBA8 — screenshots and golden-image tests.
    [[nodiscard]] virtual std::vector<std::uint8_t> read_pixels() = 0;
};

/// A backend factory. Registering one is how a new API is added; nothing in
/// the core needs to change.
struct Registration {
    Api  api;
    /// Returns nullptr when this backend cannot initialise on this machine,
    /// which is the signal to try the next candidate.
    std::unique_ptr<Device> (*create)(void* native_window, int width, int height);
};

/// The registry. Backends append at static-init time.
[[nodiscard]] inline std::vector<Registration>& registry() {
    static std::vector<Registration> r;
    return r;
}

inline void register_backend(Registration reg) { registry().push_back(reg); }

/// Create the best available device, honouring MAYAG_BACKEND and falling
/// through failures. Never returns null: the software backend cannot fail.
[[nodiscard]] inline std::unique_ptr<Device> create_device(void* native_window,
                                                           int width, int height) {
    std::vector<Api> order;

    if (const char* forced = std::getenv("MAYAG_BACKEND")) {
        order.push_back(api_from_name(forced));
    }
    for (Api a : preferred_order()) order.push_back(a);

    for (Api want : order) {
        for (const auto& reg : registry()) {
            if (reg.api != want) continue;
            if (auto dev = reg.create(native_window, width, height)) return dev;
        }
    }
    return nullptr;   // only if the software backend was never registered
}

}  // namespace mayag::backend
