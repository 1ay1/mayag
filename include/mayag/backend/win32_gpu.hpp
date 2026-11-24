#pragma once
#include "../core/target.hpp"
#include "gpu_policy.hpp"
// mayag::backend::Win32Gpu — the window's GPU, whichever API drives it.
//
// The Win32 window talks to one object with the transport contract every
// mayag device has (attach, resize, submit, atlas, textures). Behind it is
// Direct3D 11 + DXGI flip model by default -- the native Windows path, and
// measured the faster one on every axis that matters to a window (bring-up
// 130 ms vs Vulkan's 330 ms warm / 2.1 s cold; resize 2.4 ms vs 14.5 ms) --
// or Vulkan when MAYAG_BACKEND=vulkan asks for it. If the preferred API
// cannot come up, the other is tried before the window settles for the CPU.

#if MAYAG_OS_WINDOWS

#include "d3d11.hpp"
#if MAYAG_GPU_VULKAN
#  include "vulkan.hpp"
#  include "vulkan_device.hpp"
#endif

#include <cstdlib>
#include <memory>
#include <string_view>

namespace mayag::backend {

class Win32Gpu {
  public:
    enum class Api : unsigned char { none, d3d11, vulkan };

    Win32Gpu() = default;
    Win32Gpu(const Win32Gpu&) = delete;
    Win32Gpu& operator=(const Win32Gpu&) = delete;
    ~Win32Gpu() { destroy(); }

    void set_low_latency(bool on) noexcept {
        low_latency_ = on;
        if (d3d_) d3d_->set_low_latency(on);   // read at present time: fine after prepare
#if MAYAG_GPU_VULKAN
        if (vk_) vk_->set_low_latency(on);      // read when the swapchain is made
#endif
    }
    void set_transparent(bool on) noexcept {
        transparent_ = on;
#if MAYAG_GPU_VULKAN
        if (vk_) vk_->set_transparent(on);
#endif
    }

    /// Bring a GPU up on `hwnd` in one go (prepare + attach_surface).
    [[nodiscard]] bool attach(HWND hwnd, Vec2 logical, float dpi) {
        return prepare() && attach_surface(hwnd, logical, dpi);
    }

    /// The window-independent part -- device and pipeline, ~all of the cost.
    /// Started before the HWND exists, so it overlaps window creation. Order:
    /// MAYAG_BACKEND if set, else D3D11 then Vulkan. Any thread.
    [[nodiscard]] bool prepare() {
        if (api_ != Api::none) return true;
        // Test hook: pretend no GPU API can come up, to exercise the window's
        // fallback (a no-redirection window recreated for GDI) on a machine
        // that has a perfectly good GPU.
        if (std::getenv("MAYAG_GPU_FAIL") != nullptr) return false;
        const bool vulkan_first = gpu_policy::prefers(gpu_policy::Choice::vulkan);
        for (int pass = 0; pass < 2; ++pass) {
            const bool try_vulkan = (pass == 0) == vulkan_first;
            if (!try_vulkan) {
                auto d = std::make_unique<D3D11Device>();
                d->set_low_latency(low_latency_);
                if (d->prepare()) { d3d_ = std::move(d); api_ = Api::d3d11; return true; }
                d->destroy();
            }
#if MAYAG_GPU_VULKAN
            else {
                auto d = std::make_unique<VulkanDevice>();
                d->set_low_latency(low_latency_);
                d->set_transparent(transparent_);
                if (d->prepare()) { vk_ = std::move(d); api_ = Api::vulkan; return true; }
                d->destroy();
            }
#endif
        }
        return false;
    }

    /// The swapchain on the window (a few ms). Any thread.
    [[nodiscard]] bool attach_surface(HWND hwnd, Vec2 logical, float dpi) {
        bool ok = false;
        if (api_ == Api::d3d11) ok = d3d_->attach_swapchain(hwnd, logical, dpi);
#if MAYAG_GPU_VULKAN
        else if (api_ == Api::vulkan) ok = vk_->attach_win32(::GetModuleHandleW(nullptr), hwnd, logical, dpi);
#endif
        if (!ok) destroy();
        return ok;
    }

    void destroy() {
        if (d3d_) d3d_->destroy();
#if MAYAG_GPU_VULKAN
        if (vk_) vk_->destroy();
#endif
        d3d_.reset();
#if MAYAG_GPU_VULKAN
        vk_.reset();
#endif
        api_ = Api::none;
    }

    [[nodiscard]] Api api() const noexcept { return api_; }
    [[nodiscard]] std::string_view api_name() const noexcept {
        return api_ == Api::d3d11 ? "d3d11" : api_ == Api::vulkan ? "vulkan" : "none";
    }

#if MAYAG_GPU_VULKAN
#  define MG_GPU_CALL(expr_d3d, expr_vk, fallback) \
        (api_ == Api::d3d11 ? (expr_d3d) : api_ == Api::vulkan ? (expr_vk) : (fallback))
#else
#  define MG_GPU_CALL(expr_d3d, expr_vk, fallback) (api_ == Api::d3d11 ? (expr_d3d) : (fallback))
#endif

    [[nodiscard]] const char* device_name() const noexcept {
        return MG_GPU_CALL(d3d_->device_name(), vk_->device_name(), "none");
    }
    [[nodiscard]] bool needs_present() const noexcept {
        return MG_GPU_CALL(d3d_->needs_present(), vk_->needs_present(), false);
    }
    void resize(Vec2 logical, float dpi) {
        if (api_ == Api::d3d11) d3d_->resize(logical, dpi);
#if MAYAG_GPU_VULKAN
        else if (api_ == Api::vulkan) vk_->resize(logical, dpi);
#endif
    }
    void submit(const DrawList& dl, Color<Srgb> clear) {
        if (api_ == Api::d3d11) d3d_->submit(dl, clear);
#if MAYAG_GPU_VULKAN
        else if (api_ == Api::vulkan) vk_->submit(dl, clear);
#endif
    }
    template <typename AtlasT>
    void sync_atlas(AtlasT& atlas) {
        if (api_ == Api::d3d11) d3d_->sync_atlas(atlas);
#if MAYAG_GPU_VULKAN
        else if (api_ == Api::vulkan) vk_->sync_atlas(atlas);
#endif
    }
    std::uint32_t upload_texture(std::uint32_t id, const std::uint8_t* rgba, int w, int h) {
        return MG_GPU_CALL(d3d_->upload_texture(id, rgba, w, h), vk_->upload_texture(id, rgba, w, h), 0u);
    }
    void release_texture(std::uint32_t id) {
        if (api_ == Api::d3d11) d3d_->release_texture(id);
#if MAYAG_GPU_VULKAN
        else if (api_ == Api::vulkan) vk_->release_texture(id);
#endif
    }
#undef MG_GPU_CALL

  private:
    Api api_ = Api::none;
    bool low_latency_ = false, transparent_ = false;
    std::unique_ptr<D3D11Device> d3d_;
#if MAYAG_GPU_VULKAN
    std::unique_ptr<VulkanDevice> vk_;
#endif
};

}  // namespace mayag::backend

template <> inline constexpr bool jaal::sendable_opt_in<mayag::backend::Win32Gpu> = true;

#endif  // _WIN32
