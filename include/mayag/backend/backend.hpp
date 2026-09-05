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

#include "../core/color.hpp"
#include "../core/geometry.hpp"
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

/// The order backends are actually TRIED in: an explicit MAYAG_BACKEND request
/// first (so a user can force or exclude a backend without a rebuild), then the
/// platform preference, and software last because it can never fail.
///
/// This is the ONLY place selection policy lives. A new backend joins by
/// registering itself and appearing in `preferred_order()`; it does not need a
/// branch anywhere in the core, the platform layer, or the app runtime.
[[nodiscard]] inline std::vector<Api> selection_order() {
    std::vector<Api> v;
    if (const char* forced = std::getenv("MAYAG_BACKEND")) {
        v.push_back(api_from_name(forced));
    }
    for (Api a : preferred_order()) v.push_back(a);
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

/// A window/surface to render into, in whatever the platform's native terms
/// are. The core never interprets these; it only passes them through to the
/// backend that claimed it can use them.
struct SurfaceHandle {
    /// Wayland: wl_display*. X11: Display*. Win32: HINSTANCE. macOS: unused.
    void* display = nullptr;
    /// Wayland: wl_surface*. X11: Window. Win32: HWND. macOS: NSView*/CALayer*.
    void* surface = nullptr;
};

/// The glyph atlas, type-erased.
///
/// FontStack's atlas is a template parameter, so the core cannot name its type
/// here without dragging the font engine into the backend interface. This hands
/// over the two planes a backend needs to upload, plus the DIRTY FLAGS — the
/// atlas grows by small rects and re-uploading a 1024² texture every frame
/// because the abstraction forgot what changed would be a real cost, so the
/// flags travel with the data.
///
/// A backend uploads a plane when `*_dirty` is set (or when its cached size no
/// longer matches) and then calls `clear_dirty`, which hands the acknowledgement
/// back to the atlas that owns the state.
struct AtlasView {
    const std::uint8_t* coverage = nullptr;   ///< R8 plane, w*h bytes
    const std::uint8_t* color    = nullptr;   ///< RGBA8 plane (emoji), or null
    int  width = 0, height = 0;               ///< both planes share these
    bool coverage_dirty = false;
    bool color_dirty    = false;
    bool has_color      = false;

    /// Acknowledge a successful upload. Cleared through the owner so the atlas
    /// stops reporting the region as dirty on the next frame.
    void (*clear_dirty)(void* owner, bool coverage_plane) = nullptr;
    void* owner = nullptr;

    void ack_coverage() const { if (clear_dirty) clear_dirty(owner, true); }
    void ack_color() const    { if (clear_dirty) clear_dirty(owner, false); }
};

/// Build an AtlasView from any atlas exposing mayag's atlas surface. Keeping
/// this here (rather than in each backend) means the font engine's type never
/// reaches a backend, and a new backend gets the adaptation for free.
template <typename AtlasT>
[[nodiscard]] inline AtlasView view_of(AtlasT& atlas) {
    AtlasView v;
    v.width  = atlas.width();
    v.height = atlas.height();
    v.coverage = atlas.pixels().data();
    v.coverage_dirty = !atlas.dirty_region().empty();
    v.has_color = atlas.has_color();
    if (v.has_color) {
        v.color = atlas.color_pixels().data();
        v.color_dirty = !atlas.color_dirty_region().empty();
    }
    v.owner = &atlas;
    v.clear_dirty = [](void* o, bool coverage_plane) {
        auto* a = static_cast<AtlasT*>(o);
        if (coverage_plane) a->clear_dirty();
        else                a->clear_color_dirty();
    };
    return v;
}

/// What every backend implements.
///
/// Note what is NOT here: no shaders, no pipelines, no descriptor sets, no
/// render passes, and no shading maths of any kind. Those are backend-private
/// because they differ irreconcilably between APIs, and leaking them into a
/// common interface is what makes most abstractions leak. How a pixel is
/// SHADED is not a backend concern at all — it lives once in render/kernel.hpp
/// and every backend runs code generated from it.
///
/// So a backend is a TRANSPORT: get buffers to the GPU, run N instanced draws,
/// get pixels back. That is the whole job.
class Device {
  public:
    virtual ~Device() = default;

    [[nodiscard]] virtual DeviceInfo info() const = 0;

    /// Resize the target surface. Backends recreate swapchains here.
    /// `dpi` is the device pixel ratio; physical size is logical * dpi.
    virtual void resize(Vec2 logical, float dpi) = 0;

    /// Submit one frame's draw list to the attached surface and present it.
    virtual FrameStats submit(const DrawList& list, Color<Srgb> clear) = 0;

    /// Upload the glyph atlas. Called before submit when the atlas changed.
    /// Backends with no texture concept (the software rasteriser) ignore it.
    virtual void sync_atlas(const AtlasView&) {}

    /// Render into an offscreen target and read it back as RGBA8, sRGB-encoded
    /// so it is directly comparable with any other backend's output. This is
    /// the screenshot and golden-test path, and it is REQUIRED: it is how the
    /// parity gate proves a backend agrees with the reference rasteriser.
    [[nodiscard]] virtual bool render_offscreen(const DrawList& list, int w, int h,
                                                Color<Srgb> clear,
                                                std::vector<std::uint8_t>& out_rgba) = 0;

    /// Upload an RGBA8 texture; returns a slot id usable in `Instance`.
    /// Slot 0 is reserved for "no texture".
    virtual std::uint32_t upload_texture(std::span<const std::uint8_t> /*rgba*/,
                                         int /*width*/, int /*height*/) { return 0; }

    /// Upload pixels for `image(id)` to sample. The default does nothing, so
    /// a backend without image support still satisfies the interface and an
    /// app that uploads simply draws nothing rather than failing to build.
    virtual void upload_image(std::uint32_t /*id*/, const ImageData& /*img*/) {}
    virtual void release_image(std::uint32_t /*id*/) {}

    virtual void release_texture(std::uint32_t /*slot*/) {}

    [[nodiscard]] virtual std::uint64_t frames_presented() const noexcept { return 0; }
};

/// A backend factory. Registering one is how a new API is added; nothing in
/// the core changes.
///
/// `create` returns nullptr when the backend cannot run here (no driver, no
/// device, wrong surface type), which is the signal to try the next candidate.
/// `create_offscreen` builds a device with no surface, for screenshots and
/// tests; a backend that cannot do that returns nullptr and is simply skipped
/// by the parity gate.
struct Registration {
    Api api;
    std::unique_ptr<Device> (*create)(const SurfaceHandle&, Vec2 logical, float dpi);
    std::unique_ptr<Device> (*create_offscreen)() = nullptr;
};

/// The registry. Backends append at static-init time.
[[nodiscard]] inline std::vector<Registration>& registry() {
    static std::vector<Registration> r;
    return r;
}

inline void register_backend(Registration reg) { registry().push_back(reg); }

/// Create the best available device for a surface, honouring MAYAG_BACKEND and
/// falling through failures. Returns nullptr only if no registered backend can
/// drive this surface — callers fall back to their own CPU path.
[[nodiscard]] inline std::unique_ptr<Device> create_device(const SurfaceHandle& surface,
                                                           Vec2 logical, float dpi) {
    for (Api want : selection_order()) {
        for (const auto& reg : registry()) {
            if (reg.api != want || reg.create == nullptr) continue;
            if (auto dev = reg.create(surface, logical, dpi)) return dev;
        }
    }
    return nullptr;
}

/// Create a surfaceless device for offscreen rendering (screenshots, tests).
[[nodiscard]] inline std::unique_ptr<Device> create_offscreen_device() {
    for (Api want : selection_order()) {
        for (const auto& reg : registry()) {
            if (reg.api != want || reg.create_offscreen == nullptr) continue;
            if (auto dev = reg.create_offscreen()) return dev;
        }
    }
    return nullptr;
}

/// Create a device for one SPECIFIC api, ignoring preference order. Used by
/// tests that must compare two named backends against each other.
[[nodiscard]] inline std::unique_ptr<Device> create_offscreen_device(Api api) {
    for (const auto& reg : registry()) {
        if (reg.api != api || reg.create_offscreen == nullptr) continue;
        if (auto dev = reg.create_offscreen()) return dev;
    }
    return nullptr;
}

}  // namespace mayag::backend
