#pragma once
// Internal fragment: included by vulkan.hpp after the Vulkan ABI it uses;
// include <mayag/backend/vulkan.hpp>, not this file.
// mayag::backend::VulkanDevice — the render device
//
// Split from vulkan.hpp (the ABI) so the mechanical part and the interesting
// part read separately. This is the interesting part: bring up an instance and
// device, build the one pipeline, and turn a `DrawList` into a frame with one
// instanced draw per batch.
//
// The public shape matches MetalDevice deliberately, so the window code that
// drives one drives the other with a different type and nothing else:
//
//   init_offscreen()          bring up device with no surface (CI, readback)
//   attach_wayland(dpy, surf) bring up device + swapchain on a wl_surface
//   valid()                   did bring-up succeed
//   resize(logical, dpi)      rebuild the swapchain
//   submit(list, clear)       render + present one frame
//   sync_atlas(atlas)         upload dirty glyph rects
//   render_offscreen(...)     render to a Framebuffer (for tests)

#include "../render/draw_list.hpp"
#include "../render/frame_plan.hpp"
#include "../core/startup.hpp"
#include <jaal/core/sendable.hpp>   // sendable_opt_in, specialised below
#include "../render/shader_generated.hpp"
#include "backend.hpp"   // AtlasView / view_of
#include "reclaim.hpp"
#include "software.hpp"

// dup()/close(): a dmabuf fd is borrowed, and Vulkan takes ownership of the
// descriptor it is handed, so import duplicates first. The Windows CRT
// spells them _dup/_close (the fd-import paths never run there, but they
// must compile).
#if defined(_WIN32)
#include <io.h>
namespace mayag::backend::detail {
inline int fd_dup(int fd) noexcept { return ::_dup(fd); }
inline int fd_close(int fd) noexcept { return ::_close(fd); }
}  // namespace mayag::backend::detail
#else
#include <unistd.h>
namespace mayag::backend::detail {
inline int fd_dup(int fd) noexcept { return ::dup(fd); }
inline int fd_close(int fd) noexcept { return ::close(fd); }
}  // namespace mayag::backend::detail
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <map>
#include <vector>

#if defined(__linux__) || defined(__FreeBSD__) || defined(_WIN32)

namespace mayag::backend {

class VulkanDevice {
    using A = vk::Api;

  public:
    VulkanDevice() = default;
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    ~VulkanDevice() { destroy(); }

    // ── bring-up ─────────────────────────────────────────────────────────

    /// Device with no surface: offscreen rendering for tests and readback.
    [[nodiscard]] bool init_offscreen() {
        if (!create_instance(/*want_surface=*/false)) return false;
        if (!pick_device_and_queue(nullptr)) return false;
        if (!create_device()) return false;
        if (!create_command_infra()) return false;
        if (!create_sampler_and_atlas()) return false;
        // sRGB attachment: the fragment shader outputs LINEAR premultiplied
        // colour, and the hardware (a) linearises the destination before the
        // alpha blend and (b) sRGB-encodes the result on store. So compositing
        // happens in LINEAR light — byte-for-byte the same model as the software
        // rasteriser (linear framebuffer, single sRGB encode at the end). An
        // antialiased edge blends correctly; there is no gamma-space blending
        // and no double-encode.
        off_format_ = vk::VK_FORMAT_R8G8B8A8_SRGB;
        if (!create_pipeline(off_format_, /*for_present=*/false)) return false;
        offscreen_ = true;
        valid_ = true;
        return true;
    }

    /// Everything that doesn't need a window: instance, device, queues,
    /// command buffers, sampler. Slow (driver init, ~80 ms on some), so a
    /// window can run it on a worker while it connects, then attach.
    [[nodiscard]] bool prepare() {
        if (prepared_) return true;
        if (!create_instance(/*want_surface=*/true)) return false;
        startup::mark("  vulkan instance");
        if (!pick_device_and_queue(nullptr)) return false;
        startup::mark("  vulkan device picked");
        if (!create_device()) return false;
        startup::mark("  vulkan device created");
        if (!create_command_infra()) return false;
        if (!create_sampler_and_atlas()) return false;
        prepared_ = true;
        return true;
    }

    /// Device + swapchain on a live Wayland surface. Calls prepare() first if
    /// nobody did.
    [[nodiscard]] bool attach_wayland(void* display, void* surface,
                                      Vec2 logical, float dpi) {
        if (!prepare()) return false;
        if (api_->CreateWaylandSurfaceKHR == nullptr) return false;

        vk::VkWaylandSurfaceCreateInfoKHR sci{};
        sci.sType = vk::VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        sci.display = display;
        sci.surface = surface;
        if (api_->CreateWaylandSurfaceKHR(instance_, &sci, nullptr, &surface_) != vk::VK_SUCCESS) {
            return false;
        }
        // The device was picked without a surface; make sure its queue can
        // present to this one.
        vk::Bool32 present = 0;
        api_->GetPhysicalDeviceSurfaceSupportKHR(phys_, queue_family_, surface_, &present);
        if (!present) return false;

        dpi_ = dpi;
        viewport_ = logical;
        if (!create_swapchain()) return false;
        startup::mark("  vulkan swapchain");
        if (!create_pipeline(swap_format_, /*for_present=*/true)) return false;
        startup::mark("  vulkan pipeline");
        // Framebuffers reference the render pass, so they must be built AFTER
        // the pipeline creates it — not inside create_swapchain(), which runs
        // first to discover the surface format the pipeline needs.
        make_framebuffers();
        valid_ = true;
        return true;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    void resize(Vec2 logical, float dpi) {
        if (!valid_ || offscreen_) return;
        dpi_ = dpi;
        viewport_ = logical;
        api_->DeviceWaitIdle(device_);
        rebuild_swapchain_ = true;
    }

    // ── the atlas ────────────────────────────────────────────────────────

    /// Upload the glyph atlas's dirty region. The atlas rasterises coverage on
    /// the CPU; this mirrors the dirty rect to an R8 image the shader samples,
    /// so glyphs batch into the same instanced draw as every shape.
    ///
    /// Takes the type-erased AtlasView so the font engine's types never reach a
    /// backend. `sync_atlas` is the convenience wrapper for direct callers.
    void sync_atlas_view(const AtlasView& atlas) {
        if (!valid_) return;
        const int w = atlas.width, h = atlas.height;
        if (w <= 0 || h <= 0 || atlas.coverage == nullptr) return;

        // (Re)create the coverage image if the size changed, forcing a full
        // re-upload.
        bool full = false;
        if (w != atlas_w_ || h != atlas_h_) {
            // The atlas grew. The frame in flight still samples the old
            // image: let it finish before destroying it.
            if (atlas_image_) api_->WaitForFences(device_, 1, &in_flight_, 1, ~0ull);
            recreate_atlas_image(w, h);
            full = true;
        }
        if (full || atlas.coverage_dirty) {
            const std::size_t bytes = static_cast<std::size_t>(w) * h;
            ensure_staging(bytes);
            std::memcpy(staging_ptr_, atlas.coverage, bytes);
            upload_plane(atlas_image_, w, h, /*channels=*/1);
            atlas.ack_coverage();
            content_changed_ = true;
        }

        // The colour plane (emoji) is uploaded the same way, into its own RGBA
        // image, and only exists once a colour glyph has been cached.
        if (atlas.has_color && atlas.color != nullptr) {
            bool color_full = false;
            if (w != color_w_ || h != color_h_) {
                if (color_image_) api_->WaitForFences(device_, 1, &in_flight_, 1, ~0ull);
                recreate_color_image(w, h);
                color_full = true;
            }
            if (color_full || atlas.color_dirty) {
                const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
                ensure_staging(bytes);
                std::memcpy(staging_ptr_, atlas.color, bytes);
                upload_plane(color_image_, w, h, /*channels=*/4);
                atlas.ack_color();
                content_changed_ = true;
            }
        }
    }

    template <typename AtlasT>
    void sync_atlas(AtlasT& atlas) { sync_atlas_view(view_of(atlas)); }

    /// Human-readable adapter name. Vulkan exposes this via
    /// vkGetPhysicalDeviceProperties, which this loader does not currently
    /// pull in; the api tag is the honest answer until it does.
    [[nodiscard]] std::string adapter_name() const { return "vulkan"; }

    /// Copy the current staging buffer into `image`, wrapping the transfer in
    /// the layout transitions a sampled image needs.
    void upload_plane(vk::VkImage image, int w, int h, int /*channels*/) {
        one_shot([&](vk::VkCommandBuffer cb) {
            barrier_image(cb, image, vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          vk::VK_ACCESS_SHADER_READ_BIT, vk::VK_ACCESS_TRANSFER_WRITE_BIT,
                          vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, vk::VK_PIPELINE_STAGE_TRANSFER_BIT);
            vk::VkBufferImageCopy region{};
            region.imageSubresource = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
            api_->CmdCopyBufferToImage(cb, staging_buf_.buf, image,
                                       vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier_image(cb, image, vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          vk::VK_ACCESS_TRANSFER_WRITE_BIT, vk::VK_ACCESS_SHADER_READ_BIT,
                          vk::VK_PIPELINE_STAGE_TRANSFER_BIT, vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
    }

    // ── the frame ────────────────────────────────────────────────────────

    /// Render and present one frame to the swapchain.
    void submit(const DrawList& list, Color<Srgb> clear) {
        if (!valid_ || offscreen_) return;
        if (rebuild_swapchain_) { rebuild_swapchain(); rebuild_swapchain_ = false; }
        if (swapchain_ == nullptr) { acquire_failed_ = true; return; }

        api_->WaitForFences(device_, 1, &in_flight_, 1, ~0ull);

        // That wait is the completion clock. It returned, so the frame
        // submitted last time is finished, and anything retired during it is
        // now safe to free -- without a DeviceWaitIdle, and without freeing
        // anything the GPU might still be reading.
        //
        // Doing it HERE rather than at the point of destruction is the whole
        // trick: destruction sites do not have to know whether the GPU is
        // busy, and the frame loop does not have to know what was destroyed.
        ++frames_completed_;
        reclaim_.collect(frames_completed_);

        std::uint32_t image_index = 0;
        VkResultCheck acq = api_->AcquireNextImageKHR(device_, swapchain_, ~0ull,
                                                      image_available_, nullptr, &image_index);
        if (acq == vk::VK_ERROR_OUT_OF_DATE_KHR) { rebuild_swapchain(); acquire_failed_ = true; return; }
        if (acq != vk::VK_SUCCESS && acq != vk::VK_SUBOPTIMAL_KHR) { acquire_failed_ = true; return; }
        acquire_failed_ = false;

        api_->ResetFences(device_, 1, &in_flight_);
        upload_instances(list);
        if (list_has_backdrop(list)) {
            ensure_capture_image(static_cast<int>(swap_extent_.width),
                                 static_cast<int>(swap_extent_.height), swap_format_);
        }

        vk::VkCommandBuffer cb = frame_cb_;
        api_->ResetCommandBuffer(cb, 0);
        begin_cb(cb);
        record_pass(cb, framebuffers_[image_index], swap_images_[image_index],
                    list, clear, swap_extent_.width, swap_extent_.height,
                    image_index < text_framebuffers_.size() ? text_framebuffers_[image_index] : nullptr);
        api_->EndCommandBuffer(cb);

        const vk::Flags wait_stage = vk::VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        vk::VkSubmitInfo si{};
        si.sType = vk::VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &image_available_;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &render_done_;
        api_->QueueSubmit(queue_, 1, &si, in_flight_);

        vk::VkPresentInfoKHR pi{};
        pi.sType = vk::VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &render_done_;
        pi.swapchainCount = 1; pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &image_index;
        VkResultCheck pr = api_->QueuePresentKHR(queue_, &pi);
        if (pr == vk::VK_ERROR_OUT_OF_DATE_KHR || pr == vk::VK_SUBOPTIMAL_KHR) {
            rebuild_swapchain_ = true;
        }
        last_presented_ = image_index;
        ++frames_;
        content_changed_ = false;
    }

    /// The screen would change even for an identical draw list: a resize or
    /// rebuild is pending, or a texture's pixels changed (a video frame).
    [[nodiscard]] bool needs_present() const noexcept {
        return rebuild_swapchain_ || content_changed_ || acquire_failed_;
    }

    /// Run `f` once every frame submitted so far has finished on the GPU:
    /// the moment memory those frames sampled may be written again. Collected
    /// at the next frame or by poll_retired(), whichever comes first.
    void after_gpu(std::function<void()> f) { reclaim_.retire(frames_completed_ + 1, std::move(f)); }

    /// Free what is retired, if the last frame has finished, WITHOUT waiting.
    ///
    /// Retirement is normally collected when the next frame starts. A
    /// compositor cannot wait for that: a client whose old buffer was
    /// replaced is blocked until that buffer is released, and if nothing else
    /// changes on screen there is no next frame. So the host polls this
    /// whenever it is idle; it costs one vkGetFenceStatus.
    /// Returns whether anything is still waiting to be freed.
    bool poll_retired() {
        if (!valid_ || reclaim_.pending() == 0) return false;
        if (api_->GetFenceStatus != nullptr && api_->GetFenceStatus(device_, in_flight_) == vk::VK_SUCCESS) {
            // The fence is signalled: the last submit is done, so everything
            // retired up to and including it is free.
            reclaim_.collect(frames_completed_ + 1);
        }
        return reclaim_.pending() != 0;
    }

    /// Render to an offscreen framebuffer and read it back as RGBA8. Used by
    /// the GPU-vs-software equivalence test, so the thing CI checks is the
    /// thing the window runs.
    [[nodiscard]] bool render_offscreen(const DrawList& list, int w, int h,
                                        Color<Srgb> clear,
                                        std::vector<std::uint8_t>& out_rgba) {
        if (!valid_ || !offscreen_) return false;
        if (w <= 0 || h <= 0) return false;

        // Offscreen rendering submits and WAITS (one_shot below), so by the
        // time this returns the frame is complete. Advancing the clock here
        // keeps retirement working on a device that never calls submit() --
        // otherwise a headless or screenshot device would hold every retired
        // resource until teardown, which is a leak in everything but name.
        ++frames_completed_;
        reclaim_.collect(frames_completed_);

        if (w != off_w_ || h != off_h_) { recreate_offscreen_target(w, h); }
        viewport_ = Vec2{static_cast<float>(w), static_cast<float>(h)};
        dpi_ = 1.0f;
        upload_instances(list);

        // If the frame has frosted-glass panels, size the capture image before
        // recording begins — it issues its own one_shot, which must not nest
        // inside the recording below.
        if (list_has_backdrop(list)) {
            ensure_capture_image(w, h, off_format_);
        }

        one_shot([&](vk::VkCommandBuffer cb) {
            record_pass(cb, off_fb_, off_image_, list, clear,
                        static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), off_text_fb_);
            // colour attachment -> transfer src, then copy to readback buffer
            barrier_image(cb, off_image_, vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          vk::VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, vk::VK_ACCESS_TRANSFER_READ_BIT,
                          vk::VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::VK_PIPELINE_STAGE_TRANSFER_BIT);
            vk::VkBufferImageCopy region{};
            region.imageSubresource = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
            api_->CmdCopyImageToBuffer(cb, off_image_, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback_buf_.buf, 1, &region);
        });

        out_rgba.resize(static_cast<std::size_t>(w) * h * 4);
        std::memcpy(out_rgba.data(), readback_ptr_, out_rgba.size());
        return true;
    }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return frames_; }
    [[nodiscard]] const char* device_name() const noexcept { return device_name_; }
    /// Prefer tearing IMMEDIATE presentation over MAILBOX (call before the
    /// swapchain is built; takes effect on the next rebuild otherwise).
    void set_low_latency(bool on) noexcept {
        if (on != low_latency_) { low_latency_ = on; if (swapchain_) rebuild_swapchain_ = true; }
    }
    /// True when glyphs blend in encoded space, exactly as CoreGraphics (and
    /// the software reference) draws text: the device can view its target
    /// as UNORM. False: the linear-light coverage approximation.
    [[nodiscard]] bool encoded_text() const noexcept { return text_pipeline_ != nullptr; }

    /// Read back the image that was ACTUALLY PRESENTED, as RGBA8.
    ///
    /// This is deliberately not the same thing as re-rendering the last draw
    /// list offscreen, which is what the screenshot path used to do. That
    /// re-render goes through a different image format, a different render
    /// pass and no swapchain, so it is blind to every bug that lives in
    /// presentation itself -- exactly the class of bug where the window shows
    /// something different from what the renderer produced. Copying the real
    /// swapchain image is the only way to observe what the user sees.
    ///
    /// Returns false when there is no swapchain (offscreen device) or nothing
    /// has been presented yet.
    [[nodiscard]] bool read_swapchain(std::vector<std::uint8_t>& out_rgba) {
        if (!valid_ || offscreen_ || swapchain_ == nullptr) return false;
        if (frames_ == 0) return false;

        const int w = static_cast<int>(swap_extent_.width);
        const int h = static_cast<int>(swap_extent_.height);
        if (w <= 0 || h <= 0) return false;
        if (last_presented_ >= swap_images_.size()) return false;

        // The presented image is still in PRESENT_SRC layout; move it to
        // TRANSFER_SRC, copy, and put it back so presentation can continue.
        if (api_->DeviceWaitIdle) api_->DeviceWaitIdle(device_);
        ensure_readback(w, h);

        vk::VkImage img = swap_images_[last_presented_];
        one_shot([&](vk::VkCommandBuffer cb) {
            barrier_image(cb, img, vk::VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          0, vk::VK_ACCESS_TRANSFER_READ_BIT,
                          vk::VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          vk::VK_PIPELINE_STAGE_TRANSFER_BIT);
            vk::VkBufferImageCopy region{};
            region.imageSubresource = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {static_cast<std::uint32_t>(w),
                                  static_cast<std::uint32_t>(h), 1};
            api_->CmdCopyImageToBuffer(cb, img, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback_buf_.buf, 1, &region);
            barrier_image(cb, img, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          vk::VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          vk::VK_ACCESS_TRANSFER_READ_BIT, 0,
                          vk::VK_PIPELINE_STAGE_TRANSFER_BIT,
                          vk::VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        });

        out_rgba.resize(static_cast<std::size_t>(w) * h * 4);
        std::memcpy(out_rgba.data(), readback_ptr_, out_rgba.size());

        // The swapchain may be BGRA (it is on most Linux drivers), while every
        // other surface in mayag -- PNG output, the software rasteriser, the
        // offscreen target -- is RGBA. Normalise here so a caller comparing a
        // live capture against an offscreen render is comparing pixels, not
        // channel orders.
        if (swap_format_ == vk::VK_FORMAT_B8G8R8A8_SRGB ||
            swap_format_ == vk::VK_FORMAT_B8G8R8A8_UNORM) {
            for (std::size_t i = 0; i + 3 < out_rgba.size(); i += 4) {
                std::swap(out_rgba[i], out_rgba[i + 2]);
            }
        }
        return true;
    }

    /// Upload an RGBA8 image as texture `id` (an image() node's id). Each id
    /// is its own image and its own set-1 descriptor, so any number of
    /// textures are live and each batch samples its own. Re-uploading an id
    /// at the same size reuses its image; at a new size the old one is
    /// retired, not destroyed, because a frame in flight may sample it.
    std::uint32_t upload_texture(std::uint32_t id, const std::uint8_t* rgba, int w, int h) {
        if (!valid_ || rgba == nullptr || w <= 0 || h <= 0 || id == 0) return 0;
        auto it = textures_.find(id);
        if (it != textures_.end() && (it->second.imported || it->second.w != w || it->second.h != h)) {
            retire_texture(it->second, {});
            textures_.erase(it);
            it = textures_.end();
        }
        if (it == textures_.end()) {
            Texture t;
            t.w = w;
            t.h = h;
            make_sampled_image(w, h, vk::VK_FORMAT_R8G8B8A8_UNORM, t.image, t.memory, t.view);
            if (t.image == nullptr) return 0;
            t.set = allocate_tex_set();
            if (t.set == nullptr) {
                retire_texture(t, {});
                return 0;
            }
            write_tex_set(t.set, t.view);
            it = textures_.emplace(id, t).first;
        }

        const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
        ensure_staging(bytes);
        std::memcpy(staging_ptr_, rgba, bytes);
        upload_plane(it->second.image, w, h, /*channels=*/4);
        content_changed_ = true;
        return id;
    }

    /// The old single-texture call: id 1.
    std::uint32_t upload_texture(const std::uint8_t* rgba, int w, int h) { return upload_texture(1, rgba, w, h); }

    /// Drop texture `id`. Retired with the frame that last used it.
    void release_texture(std::uint32_t id) {
        auto it = textures_.find(id);
        if (it == textures_.end()) return;
        retire_texture(it->second, {});
        textures_.erase(it);
        content_changed_ = true;
    }

    /// Import a dmabuf as a sampleable image WITHOUT copying its pixels.
    ///
    /// This is the operation a compositor is built on. A client hands over an
    /// fd; the driver maps that same physical memory as a VkImage, and the
    /// bytes never travel. The alternative — read the fd, convert, upload —
    /// costs ~8 MB and ~6 ms per 1080p frame and is what makes naive
    /// compositors stutter.
    ///
    /// Three things must line up or the import silently samples garbage:
    ///   * the FORMAT (DRM fourcc -> VkFormat)
    ///   * the MODIFIER, which describes the tiling; a mismatch is not an
    ///     error, it is wrong pixels
    ///   * the per-plane offset/stride, passed explicitly rather than guessed
    ///
    /// The fd is BORROWED: dup() before handing it to Vulkan, which takes
    /// ownership of what it is given. See render/buffer.hpp for why the
    /// borrow lasts exactly one call.
    [[nodiscard]] ImportResult import_dmabuf(const Dmabuf& d, int w, int h) {
        if (!valid_ || !dmabuf_import_) {
            return std::unexpected(ImportError::not_supported);
        }
        if (d.n_planes <= 0 || d.planes[0].fd < 0 || w <= 0 || h <= 0) {
            return std::unexpected(ImportError::invalid_buffer);
        }
        const int vk_format = vk_format_of(d.format);
        if (vk_format == 0) {
            return std::unexpected(ImportError::unsupported_format);
        }

        // Describe the layout EXPLICITLY. The driver must be told the
        // modifier and each plane's offset and stride; asking it to infer
        // them is how tiled buffers come out scrambled.
        std::array<vk::VkSubresourceLayout, 4> layouts{};
        for (int i = 0; i < d.n_planes; ++i) {
            layouts[static_cast<std::size_t>(i)].offset = d.planes[static_cast<std::size_t>(i)].offset;
            layouts[static_cast<std::size_t>(i)].rowPitch = d.planes[static_cast<std::size_t>(i)].stride;
        }
        vk::VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info{};
        mod_info.sType = vk::VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        mod_info.drmFormatModifier = static_cast<std::uint64_t>(d.modifier);
        mod_info.drmFormatModifierPlaneCount = static_cast<std::uint32_t>(d.n_planes);
        mod_info.pPlaneLayouts = layouts.data();

        vk::VkExternalMemoryImageCreateInfo ext_info{};
        ext_info.sType = vk::VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext_info.handleTypes = vk::VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        ext_info.pNext = &mod_info;

        vk::VkImageCreateInfo ici{};
        ici.sType = vk::VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext = &ext_info;
        ici.imageType = 1;                    // VK_IMAGE_TYPE_2D
        ici.format = vk_format;
        ici.extent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = 1;
        ici.tiling = vk::VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ici.usage = vk::VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = 0;
        ici.initialLayout = 0;                // VK_IMAGE_LAYOUT_UNDEFINED

        vk::VkImage image = nullptr;
        if (api_->CreateImage(device_, &ici, nullptr, &image) != vk::VK_SUCCESS ||
            image == nullptr) {
            return std::unexpected(ImportError::unsupported_format);
        }

        // Which memory types can back THIS fd? Asking the driver rather than
        // reusing the requirements of a normal image matters: an imported
        // buffer may live in memory an ordinary allocation would never pick.
        vk::VkMemoryFdPropertiesKHR fd_props{};
        fd_props.sType = vk::VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        if (api_->GetMemoryFdPropertiesKHR == nullptr ||
            api_->GetMemoryFdPropertiesKHR(
                device_, vk::VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                d.planes[0].fd, &fd_props) != vk::VK_SUCCESS) {
            api_->DestroyImage(device_, image, nullptr);
            return std::unexpected(ImportError::invalid_buffer);
        }

        vk::VkMemoryRequirements req{};
        api_->GetImageMemoryRequirements(device_, image, &req);
        const std::uint32_t usable = req.memoryTypeBits & fd_props.memoryTypeBits;
        // Imported memory is device-local by nature; ask for no host
        // properties so any matching type qualifies.
        const std::uint32_t type = find_memory(usable, 0);
        if (usable == 0) {
            api_->DestroyImage(device_, image, nullptr);
            return std::unexpected(ImportError::unsupported_format);
        }

        // dup() because Vulkan CLOSES the fd it is given, on success and on
        // failure. Handing over the caller's fd would close a descriptor the
        // client still owns -- a bug that shows up much later as a client
        // whose next frame is silently corrupt.
        //
        // Only plane 0's fd is imported. Multi-planar formats (NV12, YUV420)
        // usually share ONE fd across planes at different offsets, which this
        // handles because the offsets are in the layout above. The rarer case
        // of genuinely separate fds per plane needs one VkDeviceMemory per
        // plane and disjoint binding, and is rejected rather than imported
        // wrongly -- a wrong import does not fail, it samples garbage.
        if (d.n_planes > 1) {
            bool shared_fd = true;
            for (int i = 1; i < d.n_planes; ++i) {
                if (d.planes[static_cast<std::size_t>(i)].fd != d.planes[0].fd) {
                    shared_fd = false;
                    break;
                }
            }
            if (!shared_fd) {
                api_->DestroyImage(device_, image, nullptr);
                return std::unexpected(ImportError::unsupported_format);
            }
        }
        const int dup_fd = detail::fd_dup(d.planes[0].fd);
        if (dup_fd < 0) {
            api_->DestroyImage(device_, image, nullptr);
            return std::unexpected(ImportError::out_of_memory);
        }

        vk::VkImportMemoryFdInfoKHR import_info{};
        import_info.sType = vk::VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import_info.handleType = vk::VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        import_info.fd = dup_fd;

        // A dedicated allocation, which most drivers require for imported
        // images and none are harmed by.
        vk::VkMemoryDedicatedAllocateInfo ded{};
        ded.sType = vk::VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        ded.image = image;
        import_info.pNext = &ded;

        vk::VkMemoryAllocateInfo mai{};
        mai.sType = vk::VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &import_info;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;

        vk::VkDeviceMemory mem = nullptr;
        if (api_->AllocateMemory(device_, &mai, nullptr, &mem) != vk::VK_SUCCESS) {
            detail::fd_close(dup_fd);              // Vulkan did not take it after all
            api_->DestroyImage(device_, image, nullptr);
            return std::unexpected(ImportError::out_of_memory);
        }
        if (api_->BindImageMemory(device_, image, mem, 0) != vk::VK_SUCCESS) {
            api_->FreeMemory(device_, mem, nullptr);
            api_->DestroyImage(device_, image, nullptr);
            return std::unexpected(ImportError::invalid_buffer);
        }

        vk::VkImageView view = nullptr;
        vk::VkImageViewCreateInfo vci{};
        vci.sType = vk::VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image;
        vci.viewType = 1;                     // VK_IMAGE_VIEW_TYPE_2D
        vci.format = vk_format;
        // xrgb/xbgr: the fourth byte is padding, often 0. Read it as opaque,
        // or an X-format window composites as fully transparent.
        // (VkComponentSwizzle: IDENTITY = 0, ONE = 2.)
        if (d.format == formats::xrgb8888 || d.format == formats::xbgr8888) vci.components = {0, 0, 0, 2};
        vci.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (api_->CreateImageView(device_, &vci, nullptr, &view) != vk::VK_SUCCESS) {
            api_->FreeMemory(device_, mem, nullptr);
            api_->DestroyImage(device_, image, nullptr);
            return std::unexpected(ImportError::invalid_buffer);
        }

        // No barrier here: ownership is taken from the client's queue in the
        // frame that samples it (foreign_barriers), every frame, so an import
        // costs no submit and no wait.

        const std::uint32_t slot = next_import_slot_++;
        Texture t;
        t.image = image;
        t.view = view;
        t.memory = mem;
        t.w = w;
        t.h = h;
        t.imported = true;
        t.set = allocate_tex_set();
        if (t.set == nullptr) {
            retire_texture(t, {});
            return std::unexpected(ImportError::invalid_buffer);
        }
        write_tex_set(t.set, t.view);
        textures_.emplace(slot, t);
        content_changed_ = true;
        return TextureHandle{slot};
    }

    /// import_dmabuf, as texture `id` (an image() node's id), replacing what
    /// that id showed. The compositor path: a client's new buffer becomes
    /// what its window draws, zero copies, and the old buffer is released
    /// (on_free) only when the GPU has finished sampling it.
    [[nodiscard]] ImportResult import_dmabuf_as(std::uint32_t id, const Dmabuf& d, int w, int h,
                                                std::function<void()> on_old_free = {}) {
        auto r = import_dmabuf(d, w, h);
        if (!r) return r;
        auto node = textures_.extract(r->slot);
        if (auto old = textures_.find(id); old != textures_.end()) {
            retire_texture(old->second, std::move(on_old_free));
            textures_.erase(old);
        } else if (on_old_free) {
            on_old_free();
        }
        node.key() = id;
        textures_.insert(std::move(node));
        return TextureHandle{id};
    }

    /// Drop an imported texture, reporting when it is genuinely free.
    ///
    /// Retired, not freed: a frame in flight may still be sampling it. This
    /// is the hottest destruction path in a compositor -- every client that
    /// closes a window, resizes, or swaps its buffer chain arrives here --
    /// so a device-wide stall per call would be felt directly as jank.
    ///
    /// `on_free` runs at the same moment the resources are destroyed, which
    /// is the moment the GPU provably finished with the client's buffer.
    /// That is exactly what wl_buffer.release needs, and the Reclaimer
    /// already knew it -- it simply had nobody to tell.
    void release_import(std::uint32_t slot, std::function<void()> on_free = {}) {
        const auto it = textures_.find(slot);
        if (it == textures_.end()) {
            // Unknown slot: nothing is holding the buffer, so it is free now.
            // Reporting rather than staying silent means a caller never has
            // to guess whether its callback was dropped.
            if (on_free) on_free();
            return;
        }
        retire_texture(it->second, std::move(on_free));
        textures_.erase(it);
        content_changed_ = true;
    }

    // ── the texture table ───────────────────────────────────────────────────────────────

    /// One user texture: uploaded (we own the memory) or imported (a
    /// client's dmabuf), and the set-1 descriptor that samples it.
    struct Texture {
        vk::VkImage image = nullptr;
        vk::VkImageView view = nullptr;
        vk::VkDeviceMemory memory = nullptr;
        vk::VkDescriptorSet set = nullptr;
        int w = 0, h = 0;
        bool imported = false;
    };

    /// Destroy `t` once the frame that may be sampling it has completed, and
    /// recycle its descriptor set. `on_free` runs at that moment: the moment
    /// the GPU provably finished with the memory (wl_buffer.release).
    void retire_texture(const Texture& t, std::function<void()> on_free) {
        auto* api = api_;
        auto dev = device_;
        reclaim_.retire(frames_completed_ + 1, [this, api, dev, t, on_free = std::move(on_free)] {
            if (t.view) api->DestroyImageView(dev, t.view, nullptr);
            if (t.image) api->DestroyImage(dev, t.image, nullptr);
            if (t.memory) api->FreeMemory(dev, t.memory, nullptr);
            if (t.set) free_tex_sets_.push_back(t.set);
            if (on_free) on_free();
        });
    }

    [[nodiscard]] vk::VkDescriptorSet allocate_tex_set() {
        if (!free_tex_sets_.empty()) {
            const auto s = free_tex_sets_.back();
            free_tex_sets_.pop_back();
            return s;
        }
        vk::VkDescriptorSetAllocateInfo a{};
        a.sType = vk::VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        a.descriptorPool = desc_pool_;
        a.descriptorSetCount = 1;
        a.pSetLayouts = &tex_set_layout_;
        vk::VkDescriptorSet s = nullptr;
        if (api_->AllocateDescriptorSets(device_, &a, &s) != vk::VK_SUCCESS) return nullptr;
        return s;
    }

    void write_tex_set(vk::VkDescriptorSet set, vk::VkImageView view) {
        if (set == nullptr || view == nullptr) return;
        vk::VkDescriptorImageInfo info{sampler_, view, vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        vk::VkWriteDescriptorSet w{};
        w.sType = vk::VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = vk::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        api_->UpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    /// The set a batch with this texture id binds. 0 and the atlas
    /// sentinels (the shader does not sample u_texture for them) and unknown
    /// ids all get the complete fallback set.
    [[nodiscard]] vk::VkDescriptorSet tex_set_for(std::uint32_t id) const {
        if (auto it = textures_.find(id); it != textures_.end() && it->second.set) return it->second.set;
        return fallback_tex_set_;
    }

    /// Whether this device can import at all. False on a driver without the
    /// external-memory extensions, which is a normal configuration.
    [[nodiscard]] bool can_import() const noexcept { return dmabuf_import_; }

    /// What this device can do about synchronisation.
    [[nodiscard]] SyncCaps sync_caps() const noexcept {
        SyncCaps c;
        c.timeline = timeline_sync_;
        c.import_syncobj = timeline_sync_ && api_->ImportSemaphoreFdKHR != nullptr;
        return c;
    }

    /// Wait for a client's timeline point WITHOUT stalling the device.
    ///
    /// The whole reason explicit sync exists. A client hands over a
    /// drm_syncobj fd and a value meaning "my drawing is finished at this
    /// point"; this imports it as a semaphore and blocks until it passes.
    ///
    /// Blocking here rather than making the GPU wait is the conservative
    /// half: a fully pipelined compositor would pass the semaphore to
    /// QueueSubmit so the GPU schedules around the dependency and the CPU
    /// never waits at all. That needs the wait to be attached to a specific
    /// submission, which is what `pending_waits_` below does for the frame
    /// path. This entry point exists for the cases that genuinely need a CPU
    /// answer -- deciding whether a buffer is ready before choosing a plane,
    /// where the answer changes what is submitted.
    ///
    /// A timeout rather than an infinite wait, because a client can die
    /// holding a point that will never be signalled, and a compositor that
    /// waits forever on one client freezes the whole desktop.
    [[nodiscard]] bool wait_timeline(const TimelinePoint& p,
                                     std::uint64_t timeout_ns = 1'000'000'000ull) {
        if (!timeline_sync_ || !p.valid()) return false;
        if (api_->ImportSemaphoreFdKHR == nullptr ||
            api_->WaitSemaphores == nullptr) return false;

        vk::VkSemaphore sem = create_timeline_semaphore(0);
        if (sem == nullptr) return false;

        // The fd is BORROWED, and Vulkan takes ownership of what it is
        // given -- the same contract as a dmabuf, for the same reason.
        const int dup_fd = detail::fd_dup(p.fd);
        if (dup_fd < 0) { api_->DestroySemaphore(device_, sem, nullptr); return false; }

        vk::VkImportSemaphoreFdInfoKHR info{};
        info.sType = vk::VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        info.semaphore = sem;
        info.handleType = vk::VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        info.fd = dup_fd;
        if (api_->ImportSemaphoreFdKHR(device_, &info) != vk::VK_SUCCESS) {
            detail::fd_close(dup_fd);
            api_->DestroySemaphore(device_, sem, nullptr);
            return false;
        }

        const std::uint64_t value = p.value;
        vk::VkSemaphoreWaitInfo wi{};
        wi.sType = vk::VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1;
        wi.pSemaphores = &sem;
        wi.pValues = &value;
        const bool ok = api_->WaitSemaphores(device_, &wi, timeout_ns) == vk::VK_SUCCESS;

        // Retired rather than destroyed: the wait returning says the CLIENT
        // finished, not that our own queue is done with the semaphore.
        auto* api = api_;
        auto dev = device_;
        reclaim_.retire(frames_completed_ + 1,
                        [api, dev, sem] { api->DestroySemaphore(dev, sem, nullptr); });
        return ok;
    }

    /// Export a point a client can wait on: "the compositor has finished
    /// reading your buffer".
    ///
    /// The other half of explicit sync, and the half people forget. Without
    /// it a client cannot know when its buffer is reusable, so it either
    /// allocates a new one every frame or guesses and tears.
    ///
    /// Returns -1 when unavailable. The caller OWNS the returned fd.
    [[nodiscard]] int export_release_point() {
        if (!timeline_sync_ || api_->GetSemaphoreFdKHR == nullptr) return -1;
        vk::VkSemaphore sem = create_timeline_semaphore(frames_completed_,
                                                        /*exportable=*/true);
        if (sem == nullptr) return -1;

        vk::VkSemaphoreGetFdInfoKHR gi{};
        gi.sType = vk::VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        gi.semaphore = sem;
        gi.handleType = vk::VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        const bool ok = api_->GetSemaphoreFdKHR(device_, &gi, &fd) == vk::VK_SUCCESS;

        auto* api = api_;
        auto dev = device_;
        reclaim_.retire(frames_completed_ + 1,
                        [api, dev, sem] { api->DestroySemaphore(dev, sem, nullptr); });
        return ok ? fd : -1;
    }

    /// Format+modifier pairs this driver will accept for import.
    ///
    /// A compositor advertises this list to every client that binds
    /// zwp_linux_dmabuf_v1, so it is asked constantly and never changes.
    /// Querying the driver rather than hardcoding LINEAR is the difference
    /// between clients allocating tiled buffers the GPU samples directly and
    /// clients allocating linear ones that force a detiling blit every frame.
    ///
    /// Built EAGERLY at device creation, not lazily on first call.
    ///
    /// Lazy was the obvious choice and it is a race: this is a const method,
    /// and the one place a compositor calls it is from a client connection,
    /// which is not the render thread. Two clients binding at once would both
    /// see an unbuilt cache and both fill it. Nothing about the signature
    /// warns you -- it looks like a pure getter.
    ///
    /// Building it once at creation makes the method genuinely const and
    /// genuinely thread-safe: after init nothing writes, so any number of
    /// threads may read. That is the same answer mayag reaches everywhere
    /// else -- remove the shared mutable state rather than lock it.
    [[nodiscard]] std::span<const FormatModifier> importable_formats() const {
        return importable_;
    }

    /// Drop every texture: an app dropping its last image, and teardown
    /// (which then calls collect_all() once the device is idle).
    void release_texture() {
        for (auto& [id, t] : textures_) retire_texture(t, {});
        textures_.clear();
        content_changed_ = true;
    }

    /// The swapchain's pixel format, so callers can report it.
    [[nodiscard]] int swapchain_format() const noexcept { return swap_format_; }

    /// The swapchain's extent in pixels, or {0,0} when there is no swapchain.
    [[nodiscard]] Vec2 swapchain_extent() const noexcept {
        return {static_cast<float>(swap_extent_.width),
                static_cast<float>(swap_extent_.height)};
    }

    /// Tear down all GPU resources. Idempotent, and public because the window
    /// must call it explicitly while the wl_surface/display it wraps are still
    /// alive — the destructor calls it too, harmlessly, if the window did not.
    void destroy() {
        if (device_ == nullptr || api_ == nullptr) return;
        if (api_->DeviceWaitIdle) api_->DeviceWaitIdle(device_);

        // Live textures join the retired ones, so the collect below frees
        // them too (their descriptor sets die with the pool).
        release_texture();

        // The device really is idle now, so everything retired is safe to
        // free regardless of which frame it belonged to. This is the ONE
        // place a blocking wait is the right answer: the device is going away
        // and nothing may be left holding its memory.
        reclaim_.collect_all();

        destroy_swapchain_objects();
        if (swapchain_) api_->DestroySwapchainKHR(device_, swapchain_, nullptr);
        if (off_text_fb_) api_->DestroyFramebuffer(device_, off_text_fb_, nullptr);
        if (off_text_view_) api_->DestroyImageView(device_, off_text_view_, nullptr);
        if (off_fb_) api_->DestroyFramebuffer(device_, off_fb_, nullptr);
        if (off_view_) api_->DestroyImageView(device_, off_view_, nullptr);
        if (off_image_) api_->DestroyImage(device_, off_image_, nullptr);
        if (off_mem_) api_->FreeMemory(device_, off_mem_, nullptr);
        if (readback_buf_.buf) destroy_buffer(readback_buf_);
        if (instance_buf_.buf) destroy_buffer(instance_buf_);
        if (staging_buf_.buf) destroy_buffer(staging_buf_);
        if (atlas_view_) api_->DestroyImageView(device_, atlas_view_, nullptr);
        if (atlas_image_) api_->DestroyImage(device_, atlas_image_, nullptr);
        if (atlas_mem_) api_->FreeMemory(device_, atlas_mem_, nullptr);
        if (color_view_) api_->DestroyImageView(device_, color_view_, nullptr);
        if (color_image_) api_->DestroyImage(device_, color_image_, nullptr);
        if (color_mem_) api_->FreeMemory(device_, color_mem_, nullptr);
        if (sampler_) api_->DestroySampler(device_, sampler_, nullptr);
        if (pipeline_) api_->DestroyPipeline(device_, pipeline_, nullptr);
        if (text_pipeline_) api_->DestroyPipeline(device_, text_pipeline_, nullptr);
        if (text_render_pass_) api_->DestroyRenderPass(device_, text_render_pass_, nullptr);
        if (text_render_pass_last_) api_->DestroyRenderPass(device_, text_render_pass_last_, nullptr);
        if (layout_) api_->DestroyPipelineLayout(device_, layout_, nullptr);
        if (desc_pool_) api_->DestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (set_layout_) api_->DestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        if (tex_set_layout_) api_->DestroyDescriptorSetLayout(device_, tex_set_layout_, nullptr);
        tex_set_layout_ = nullptr;
        free_tex_sets_.clear();
        fallback_tex_set_ = nullptr;
        if (render_pass_) api_->DestroyRenderPass(device_, render_pass_, nullptr);
        if (render_pass_load_) api_->DestroyRenderPass(device_, render_pass_load_, nullptr);
        if (render_pass_clear_mid_) api_->DestroyRenderPass(device_, render_pass_clear_mid_, nullptr);
        if (render_pass_load_mid_) api_->DestroyRenderPass(device_, render_pass_load_mid_, nullptr);
        if (capture_view_) api_->DestroyImageView(device_, capture_view_, nullptr);
        if (capture_image_) api_->DestroyImage(device_, capture_image_, nullptr);
        if (capture_mem_) api_->FreeMemory(device_, capture_mem_, nullptr);
        if (in_flight_) api_->DestroyFence(device_, in_flight_, nullptr);
        if (oneshot_fence_) api_->DestroyFence(device_, oneshot_fence_, nullptr);
        if (image_available_) api_->DestroySemaphore(device_, image_available_, nullptr);
        if (render_done_) api_->DestroySemaphore(device_, render_done_, nullptr);
        if (cmd_pool_) api_->DestroyCommandPool(device_, cmd_pool_, nullptr);
        api_->DestroyDevice(device_, nullptr);
        if (surface_) api_->DestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) api_->DestroyInstance(instance_, nullptr);
        device_ = nullptr; instance_ = nullptr; surface_ = nullptr;
        swapchain_ = nullptr; valid_ = false;
    }

  private:
    using VkResultCheck = vk::VkResult;

    // ── instance / device ────────────────────────────────────────────────

    [[nodiscard]] bool create_instance(bool want_surface) {
        const vk::Api& a = vk::api();
        if (!a.ok) return false;
        api_ = &a;

        vk::VkApplicationInfo app{};
        app.sType = vk::VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "mayag";
        app.pEngineName = "mayag";
        // Vulkan 1.1.
        //
        // 1.0 would do for everything mayag draws, but the DRM format
        // modifier query is vkGetPhysicalDeviceFormatProperties2, which is
        // 1.1 CORE -- under a 1.0 instance the loader resolves it to null and
        // the driver's modifier list comes back empty, so zero-copy import
        // silently degrades to "unsupported" on hardware that supports it
        // perfectly well. That failure is invisible: no error, just no
        // formats.
        //
        // 1.1 is from 2018 and is present on every driver that has the
        // external-memory extensions in the first place, so this costs no
        // compatibility. The instance falls back below if creation fails.
        app.apiVersion = (1u << 22) | (1u << 12);   // VK_API_VERSION_1_1

        std::vector<const char*> exts;
        if (want_surface) {
            exts.push_back("VK_KHR_surface");
#if defined(__linux__) || defined(__FreeBSD__)
            exts.push_back("VK_KHR_wayland_surface");
#elif defined(_WIN32)
            exts.push_back("VK_KHR_win32_surface");
#endif
        }

        vk::VkInstanceCreateInfo ci{};
        ci.sType = vk::VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();

        if (a.CreateInstance(&ci, nullptr, &instance_) != vk::VK_SUCCESS) return false;
        load_instance_procs();
        return api_->EnumeratePhysicalDevices != nullptr;
    }

    void load_instance_procs() {
        auto g = [&](const char* n) { return api_ ? mutable_api().GetInstanceProcAddr(instance_, n) : nullptr; };
        A& m = mutable_api();
        #define MG_LOAD(field, name) m.field = reinterpret_cast<decltype(m.field)>(g(name))
        MG_LOAD(EnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
        MG_LOAD(GetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
        MG_LOAD(GetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
        // Needed before device creation to decide whether zero-copy import
        // can be requested at all.
        MG_LOAD(EnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
        MG_LOAD(GetPhysicalDeviceFormatProperties2, "vkGetPhysicalDeviceFormatProperties2");
        MG_LOAD(CreateDevice, "vkCreateDevice");
        MG_LOAD(DestroyInstance, "vkDestroyInstance");
        MG_LOAD(GetDeviceProcAddr, "vkGetDeviceProcAddr");
        MG_LOAD(DestroySurfaceKHR, "vkDestroySurfaceKHR");
        MG_LOAD(GetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR");
        MG_LOAD(GetPhysicalDeviceSurfaceCapabilitiesKHR, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        MG_LOAD(GetPhysicalDeviceSurfaceFormatsKHR, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        MG_LOAD(GetPhysicalDeviceSurfacePresentModesKHR, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        MG_LOAD(CreateWaylandSurfaceKHR, "vkCreateWaylandSurfaceKHR");
        #undef MG_LOAD
    }

    /// Query the driver's importable format+modifier pairs, once.
    ///
    /// Called from create_device() so importable_formats() is a plain read of
    /// immutable state afterwards. See that method for why this is not lazy.
    void build_importable_formats() {
        if (!dmabuf_import_ ||
            api_->GetPhysicalDeviceFormatProperties2 == nullptr) return;

        for (const Fourcc f : {formats::argb8888, formats::xrgb8888,
                               formats::abgr8888, formats::xbgr8888}) {
            const int vkf = vk_format_of(f);
            if (vkf == 0) continue;

            // Two-call idiom: ask for the count, then the values.
            vk::VkDrmFormatModifierPropertiesListEXT list{};
            list.sType = vk::VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
            vk::VkFormatProperties2 props{};
            props.sType = vk::VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
            props.pNext = &list;
            api_->GetPhysicalDeviceFormatProperties2(phys_, vkf, &props);
            if (list.drmFormatModifierCount == 0) continue;

            std::vector<vk::VkDrmFormatModifierPropertiesEXT>
                mods(list.drmFormatModifierCount);
            list.pDrmFormatModifierProperties = mods.data();
            api_->GetPhysicalDeviceFormatProperties2(phys_, vkf, &props);

            for (const auto& m : mods) {
                // Only advertise what can actually be SAMPLED. A modifier the
                // driver lists but cannot sample would have clients allocate
                // buffers that fail to import later, which reads as a broken
                // client rather than a bad advertisement.
                constexpr vk::Flags kSampled = 0x00000001;  // SAMPLED_IMAGE_BIT
                if ((m.drmFormatModifierTilingFeatures & kSampled) == 0) continue;
                importable_.push_back(FormatModifier{
                    f, static_cast<Modifier>(m.drmFormatModifier),
                    // Scanout capability is a KMS question, not a Vulkan one:
                    // it depends on the display hardware, not the renderer.
                    // Left false until an Output can answer it, rather than
                    // guessed here.
                    false});
            }
        }
    }

    /// Create a timeline semaphore starting at `initial`.
    ///
    /// `exportable` requests a handle a CLIENT can wait on. It must be asked
    /// for at creation -- an ordinary semaphore cannot be exported later, and
    /// GetSemaphoreFdKHR simply fails on one, which reads as a missing
    /// extension rather than a missing flag.
    [[nodiscard]] vk::VkSemaphore create_timeline_semaphore(
            std::uint64_t initial, bool exportable = false) {
        if (api_->CreateSemaphore == nullptr) return nullptr;
        vk::VkSemaphoreTypeCreateInfo ti{};
        ti.sType = vk::VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        ti.semaphoreType = vk::VK_SEMAPHORE_TYPE_TIMELINE;
        ti.initialValue = initial;

        vk::VkExportSemaphoreCreateInfo ei{};
        if (exportable) {
            ei.sType = vk::VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
            ei.handleTypes = vk::VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            ti.pNext = &ei;
        }

        vk::VkSemaphoreCreateInfo ci{};
        ci.sType = vk::VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        ci.pNext = &ti;
        vk::VkSemaphore sem = nullptr;
        if (api_->CreateSemaphore(device_, &ci, nullptr, &sem) != vk::VK_SUCCESS) {
            return nullptr;
        }
        return sem;
    }

    /// Is `name` among the physical device's supported extensions?
    ///
    /// Asked before requesting anything optional: enabling an unsupported
    /// device extension makes vkCreateDevice fail outright, which would turn
    /// "this GPU cannot do zero-copy" into "mayag does not start".
    [[nodiscard]] bool has_device_extension(const char* name) const {
        if (api_->EnumerateDeviceExtensionProperties == nullptr) return false;
        std::uint32_t n = 0;
        api_->EnumerateDeviceExtensionProperties(phys_, nullptr, &n, nullptr);
        if (n == 0) return false;
        std::vector<vk::VkExtensionProperties> props(n);
        api_->EnumerateDeviceExtensionProperties(phys_, nullptr, &n, props.data());
        for (const auto& p : props)
            if (std::strcmp(p.extensionName, name) == 0) return true;
        return false;
    }

    void load_device_procs() {
        A& m = mutable_api();
        auto g = [&](const char* n) { return m.GetDeviceProcAddr(device_, n); };
        #define MG_LOAD(field, name) m.field = reinterpret_cast<decltype(m.field)>(g(name))
        MG_LOAD(DestroyDevice, "vkDestroyDevice");
        MG_LOAD(GetDeviceQueue, "vkGetDeviceQueue");
        MG_LOAD(CreateCommandPool, "vkCreateCommandPool");
        MG_LOAD(DestroyCommandPool, "vkDestroyCommandPool");
        MG_LOAD(AllocateCommandBuffers, "vkAllocateCommandBuffers");
        MG_LOAD(BeginCommandBuffer, "vkBeginCommandBuffer");
        MG_LOAD(EndCommandBuffer, "vkEndCommandBuffer");
        MG_LOAD(ResetCommandBuffer, "vkResetCommandBuffer");
        MG_LOAD(QueueSubmit, "vkQueueSubmit");
        MG_LOAD(QueueWaitIdle, "vkQueueWaitIdle");
        MG_LOAD(DeviceWaitIdle, "vkDeviceWaitIdle");
        MG_LOAD(CreateBuffer, "vkCreateBuffer");
        MG_LOAD(DestroyBuffer, "vkDestroyBuffer");
        MG_LOAD(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
        MG_LOAD(AllocateMemory, "vkAllocateMemory");
        MG_LOAD(FreeMemory, "vkFreeMemory");
        MG_LOAD(BindBufferMemory, "vkBindBufferMemory");
        MG_LOAD(MapMemory, "vkMapMemory");
        MG_LOAD(UnmapMemory, "vkUnmapMemory");
        MG_LOAD(CreateImage, "vkCreateImage");
        MG_LOAD(DestroyImage, "vkDestroyImage");
        MG_LOAD(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
        // Zero-copy import. Null when the extensions were not enabled, which
        // import() checks before doing anything.
        MG_LOAD(GetMemoryFdPropertiesKHR, "vkGetMemoryFdPropertiesKHR");
        MG_LOAD(GetImageSubresourceLayout, "vkGetImageSubresourceLayout");
        // Explicit sync. Timeline entry points are 1.2 core, so try the core
        // name first and fall back to the KHR alias on older drivers.
        MG_LOAD(CreateSemaphore, "vkCreateSemaphore");
        MG_LOAD(DestroySemaphore, "vkDestroySemaphore");
        MG_LOAD(ImportSemaphoreFdKHR, "vkImportSemaphoreFdKHR");
        MG_LOAD(GetSemaphoreFdKHR, "vkGetSemaphoreFdKHR");
        MG_LOAD(WaitSemaphores, "vkWaitSemaphores");
        if (m.WaitSemaphores == nullptr) {
            MG_LOAD(WaitSemaphores, "vkWaitSemaphoresKHR");
        }
        MG_LOAD(BindImageMemory, "vkBindImageMemory");
        MG_LOAD(CreateImageView, "vkCreateImageView");
        MG_LOAD(DestroyImageView, "vkDestroyImageView");
        MG_LOAD(CreateSampler, "vkCreateSampler");
        MG_LOAD(DestroySampler, "vkDestroySampler");
        MG_LOAD(CreateShaderModule, "vkCreateShaderModule");
        MG_LOAD(DestroyShaderModule, "vkDestroyShaderModule");
        MG_LOAD(CreatePipelineLayout, "vkCreatePipelineLayout");
        MG_LOAD(DestroyPipelineLayout, "vkDestroyPipelineLayout");
        MG_LOAD(CreateRenderPass, "vkCreateRenderPass");
        MG_LOAD(DestroyRenderPass, "vkDestroyRenderPass");
        MG_LOAD(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
        MG_LOAD(DestroyPipeline, "vkDestroyPipeline");
        MG_LOAD(CreateFramebuffer, "vkCreateFramebuffer");
        MG_LOAD(DestroyFramebuffer, "vkDestroyFramebuffer");
        MG_LOAD(CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
        MG_LOAD(DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
        MG_LOAD(CreateDescriptorPool, "vkCreateDescriptorPool");
        MG_LOAD(DestroyDescriptorPool, "vkDestroyDescriptorPool");
        MG_LOAD(AllocateDescriptorSets, "vkAllocateDescriptorSets");
        MG_LOAD(UpdateDescriptorSets, "vkUpdateDescriptorSets");
        MG_LOAD(CreateFence, "vkCreateFence");
        MG_LOAD(DestroyFence, "vkDestroyFence");
        MG_LOAD(WaitForFences, "vkWaitForFences");
        MG_LOAD(ResetFences, "vkResetFences");
        MG_LOAD(GetFenceStatus, "vkGetFenceStatus");
        MG_LOAD(CreateSemaphore, "vkCreateSemaphore");
        MG_LOAD(DestroySemaphore, "vkDestroySemaphore");
        MG_LOAD(CmdBeginRenderPass, "vkCmdBeginRenderPass");
        MG_LOAD(CmdEndRenderPass, "vkCmdEndRenderPass");
        MG_LOAD(CmdBindPipeline, "vkCmdBindPipeline");
        MG_LOAD(CmdBindVertexBuffers, "vkCmdBindVertexBuffers");
        MG_LOAD(CmdBindDescriptorSets, "vkCmdBindDescriptorSets");
        MG_LOAD(CmdPushConstants, "vkCmdPushConstants");
        MG_LOAD(CmdSetViewport, "vkCmdSetViewport");
        MG_LOAD(CmdSetScissor, "vkCmdSetScissor");
        MG_LOAD(CmdDraw, "vkCmdDraw");
        MG_LOAD(CmdCopyBufferToImage, "vkCmdCopyBufferToImage");
        MG_LOAD(CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
        MG_LOAD(CmdCopyImage, "vkCmdCopyImage");
        MG_LOAD(CmdBlitImage, "vkCmdBlitImage");
        MG_LOAD(CmdPipelineBarrier, "vkCmdPipelineBarrier");
        MG_LOAD(CreateSwapchainKHR, "vkCreateSwapchainKHR");
        MG_LOAD(DestroySwapchainKHR, "vkDestroySwapchainKHR");
        MG_LOAD(GetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
        MG_LOAD(AcquireNextImageKHR, "vkAcquireNextImageKHR");
        MG_LOAD(QueuePresentKHR, "vkQueuePresentKHR");
        #undef MG_LOAD
    }

    [[nodiscard]] bool pick_device_and_queue(vk::VkSurfaceKHR surface) {
        std::uint32_t count = 0;
        api_->EnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) return false;
        std::vector<vk::VkPhysicalDevice> devices(count);
        api_->EnumeratePhysicalDevices(instance_, &count, devices.data());

        // Prefer a discrete GPU; accept any device with a graphics (and, if a
        // surface is given, present-capable) queue. llvmpipe is a valid
        // fallback — a software Vulkan device still beats mayag's own CPU path
        // for large frames and keeps the "GPU by default" promise honest.
        for (auto dev : devices) {
            std::uint32_t qcount = 0;
            api_->GetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
            std::vector<vk::VkQueueFamilyProperties> props(qcount);
            api_->GetPhysicalDeviceQueueFamilyProperties(dev, &qcount, props.data());
            for (std::uint32_t i = 0; i < qcount; ++i) {
                if ((props[i].queueFlags & vk::VK_QUEUE_GRAPHICS_BIT) == 0) continue;
                if (surface != nullptr) {
                    vk::Bool32 present = 0;
                    api_->GetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
                    if (!present) continue;
                }
                phys_ = dev;
                queue_family_ = i;
                api_->GetPhysicalDeviceMemoryProperties(dev, &mem_props_);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool create_device() {
        const float priority = 1.0f;
        vk::VkDeviceQueueCreateInfo qci{};
        qci.sType = vk::VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queue_family_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;

        std::vector<const char*> dev_exts;
        if (!offscreen_ || surface_ != nullptr) dev_exts.push_back("VK_KHR_swapchain");

        // Zero-copy dmabuf import, if the driver has it.
        //
        // Requested opportunistically rather than required: a device without
        // these still runs every app, it just cannot import a client's buffer
        // without copying. import() reports that honestly instead of failing
        // to create a device at all. Together these are what let a compositor
        // sample a client's window contents with the bytes never leaving the
        // GPU.
        if (has_device_extension("VK_EXT_image_drm_format_modifier") &&
            has_device_extension("VK_KHR_external_memory_fd") &&
            has_device_extension("VK_EXT_external_memory_dma_buf")) {
            dev_exts.push_back("VK_KHR_external_memory");
            dev_exts.push_back("VK_KHR_external_memory_fd");
            dev_exts.push_back("VK_EXT_external_memory_dma_buf");
            dev_exts.push_back("VK_EXT_image_drm_format_modifier");
            // Required by image_drm_format_modifier.
            dev_exts.push_back("VK_KHR_image_format_list");
            dev_exts.push_back("VK_KHR_sampler_ycbcr_conversion");
            dev_exts.push_back("VK_KHR_bind_memory2");
            dev_exts.push_back("VK_KHR_get_memory_requirements2");
            dmabuf_import_ = true;
            // The right queue family to acquire a client's buffer from.
            if (has_device_extension("VK_EXT_queue_family_foreign")) {
                dev_exts.push_back("VK_EXT_queue_family_foreign");
                foreign_queue_ = true;
            }
        }

        // Explicit synchronisation, likewise opportunistic.
        //
        // A compositor without this must guess when a client has finished
        // drawing, which means either a conservative stall or a torn frame.
        // With it, the GPU schedules around the dependency and neither
        // happens. Timeline semaphores are Vulkan 1.2 core, so the extension
        // is only needed on older drivers -- but the fd import always is.
        if (has_device_extension("VK_KHR_external_semaphore_fd")) {
            dev_exts.push_back("VK_KHR_external_semaphore");
            dev_exts.push_back("VK_KHR_external_semaphore_fd");
            if (has_device_extension("VK_KHR_timeline_semaphore")) {
                dev_exts.push_back("VK_KHR_timeline_semaphore");
            }
            timeline_sync_ = true;
        }

        // Mutable-format swapchain images, so the sRGB colour target can also
        // be rendered through a UNORM view: that is how glyphs blend in
        // ENCODED space, exactly as CoreGraphics draws text (see
        // kernel::encoded_text_source). Opportunistic like the rest: without
        // it, text falls back to the coverage-reshaping approximation.
        // image_format_list is core in 1.2 but this device runs at 1.1.
        bool has_swapchain_ext = false;
        for (const char* e : dev_exts) has_swapchain_ext = has_swapchain_ext || std::string_view{e} == "VK_KHR_swapchain";
        if (has_swapchain_ext && has_device_extension("VK_KHR_swapchain_mutable_format") &&
            has_device_extension("VK_KHR_image_format_list")) {
            bool listed = false;
            for (const char* e : dev_exts) listed = listed || std::string_view{e} == "VK_KHR_image_format_list";
            if (!listed) dev_exts.push_back("VK_KHR_image_format_list");
            dev_exts.push_back("VK_KHR_swapchain_mutable_format");
            swapchain_mutable_ = true;
        }

        vk::VkDeviceCreateInfo dci{};
        dci.sType = vk::VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        // Timeline semaphores are a FEATURE, not just an extension: the
        // driver has to be told to turn them on, or every create fails with
        // an error that reads like the extension is missing.
        vk::VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature{};
        if (timeline_sync_) {
            timeline_feature.sType =
                vk::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
            timeline_feature.timelineSemaphore = 1;
            dci.pNext = &timeline_feature;
        }
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<std::uint32_t>(dev_exts.size());
        dci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();

        if (api_->CreateDevice(phys_, &dci, nullptr, &device_) != vk::VK_SUCCESS) return false;
        load_device_procs();
        if (api_->GetDeviceQueue == nullptr) return false;
        api_->GetDeviceQueue(device_, queue_family_, 0, &queue_);
        // Now that the procs exist, ask the driver what it can import. Done
        // once, here, so the query is immutable afterwards and safe to read
        // from any thread -- a compositor asks it per client connection.
        build_importable_formats();
        return queue_ != nullptr;
    }

    [[nodiscard]] bool create_command_infra() {
        vk::VkCommandPoolCreateInfo pci{};
        pci.sType = vk::VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = vk::VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queue_family_;
        if (api_->CreateCommandPool(device_, &pci, nullptr, &cmd_pool_) != vk::VK_SUCCESS) return false;

        vk::VkCommandBufferAllocateInfo ai{};
        ai.sType = vk::VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = cmd_pool_;
        ai.level = vk::VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (api_->AllocateCommandBuffers(device_, &ai, &frame_cb_) != vk::VK_SUCCESS) return false;
        if (api_->AllocateCommandBuffers(device_, &ai, &oneshot_cb_) != vk::VK_SUCCESS) return false;

        vk::VkFenceCreateInfo fci{};
        fci.sType = vk::VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = vk::VK_FENCE_CREATE_SIGNALED_BIT;
        api_->CreateFence(device_, &fci, nullptr, &in_flight_);
        fci.flags = 0;
        api_->CreateFence(device_, &fci, nullptr, &oneshot_fence_);

        vk::VkSemaphoreCreateInfo sci{};
        sci.sType = vk::VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        api_->CreateSemaphore(device_, &sci, nullptr, &image_available_);
        api_->CreateSemaphore(device_, &sci, nullptr, &render_done_);
        return true;
    }

    // ── memory helpers ───────────────────────────────────────────────────

    [[nodiscard]] std::uint32_t find_memory(std::uint32_t type_bits, vk::Flags props) const {
        for (std::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) &&
                (mem_props_.memoryTypes[i].propertyFlags & props) == props) {
                return i;
            }
        }
        return 0;
    }

    struct Buffer { vk::VkBuffer buf = nullptr; vk::VkDeviceMemory mem = nullptr; void* mapped = nullptr; };

    [[nodiscard]] Buffer make_buffer(vk::DeviceSize size, vk::Flags usage, bool host_visible) {
        Buffer b;
        vk::VkBufferCreateInfo ci{};
        ci.sType = vk::VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size = size; ci.usage = usage; ci.sharingMode = vk::VK_SHARING_MODE_EXCLUSIVE;
        api_->CreateBuffer(device_, &ci, nullptr, &b.buf);

        vk::VkMemoryRequirements req{};
        api_->GetBufferMemoryRequirements(device_, b.buf, &req);
        vk::VkMemoryAllocateInfo ai{};
        ai.sType = vk::VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = find_memory(req.memoryTypeBits,
            host_visible ? (vk::VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | vk::VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                         : vk::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        api_->AllocateMemory(device_, &ai, nullptr, &b.mem);
        api_->BindBufferMemory(device_, b.buf, b.mem, 0);
        if (host_visible) api_->MapMemory(device_, b.mem, 0, size, 0, &b.mapped);
        return b;
    }

    void destroy_buffer(Buffer& b) {
        if (b.mapped) { api_->UnmapMemory(device_, b.mem); b.mapped = nullptr; }
        if (b.buf) { api_->DestroyBuffer(device_, b.buf, nullptr); b.buf = nullptr; }
        if (b.mem) { api_->FreeMemory(device_, b.mem, nullptr); b.mem = nullptr; }
    }

    template <typename Fn>
    void one_shot(Fn&& record) {
        api_->ResetCommandBuffer(oneshot_cb_, 0);
        begin_cb(oneshot_cb_);
        record(oneshot_cb_);
        api_->EndCommandBuffer(oneshot_cb_);
        vk::VkSubmitInfo si{};
        si.sType = vk::VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &oneshot_cb_;
        api_->ResetFences(device_, 1, &oneshot_fence_);
        api_->QueueSubmit(queue_, 1, &si, oneshot_fence_);
        api_->WaitForFences(device_, 1, &oneshot_fence_, 1, ~0ull);
    }

    void begin_cb(vk::VkCommandBuffer cb) {
        vk::VkCommandBufferBeginInfo bi{};
        bi.sType = vk::VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = vk::VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        api_->BeginCommandBuffer(cb, &bi);
    }

    void barrier_image(vk::VkCommandBuffer cb, vk::VkImage image, int old_layout, int new_layout,
                       vk::Flags src_access, vk::Flags dst_access,
                       vk::Flags src_stage, vk::Flags dst_stage) {
        vk::VkImageMemoryBarrier b{};
        b.sType = vk::VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
        b.oldLayout = old_layout; b.newLayout = new_layout;
        b.srcQueueFamilyIndex = vk::VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = vk::VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        api_->CmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    void ensure_staging(std::size_t bytes) {
        if (bytes <= staging_size_) return;
        if (staging_buf_.buf) destroy_buffer(staging_buf_);
        staging_buf_ = make_buffer(bytes, vk::VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        staging_ptr_ = staging_buf_.mapped;
        staging_size_ = bytes;
    }

    // ── sampler + atlas image ────────────────────────────────────────────

    [[nodiscard]] bool create_sampler_and_atlas() {
        vk::VkSamplerCreateInfo sci{};
        sci.sType = vk::VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = vk::VK_FILTER_LINEAR; sci.minFilter = vk::VK_FILTER_LINEAR;
        sci.mipmapMode = vk::VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = vk::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0.0f;
        sci.maxLod = 16.0f;   // only the backdrop capture has mips; 1-level images clamp
        if (api_->CreateSampler(device_, &sci, nullptr, &sampler_) != vk::VK_SUCCESS) return false;

        // A 1x1 placeholder so the descriptor is always valid, even before the
        // first glyph is uploaded. Coverage, colour, and backdrop-capture.
        recreate_atlas_image(1, 1);
        recreate_color_image(1, 1);
        ensure_capture_image(1, 1, vk::VK_FORMAT_R8G8B8A8_UNORM);
        return true;
    }

    void recreate_atlas_image(int w, int h) {
        if (atlas_view_) api_->DestroyImageView(device_, atlas_view_, nullptr);
        if (atlas_image_) api_->DestroyImage(device_, atlas_image_, nullptr);
        if (atlas_mem_) api_->FreeMemory(device_, atlas_mem_, nullptr);

        make_sampled_image(w, h, vk::VK_FORMAT_R8_UNORM, atlas_image_, atlas_mem_, atlas_view_);
        atlas_w_ = w; atlas_h_ = h;
        atlas_dirty_binding_ = true;
    }

    /// The RGBA colour-glyph atlas (emoji). Same size as the coverage atlas so
    /// one UV space indexes both; created lazily on the first colour glyph.
    void recreate_color_image(int w, int h) {
        if (color_view_) api_->DestroyImageView(device_, color_view_, nullptr);
        if (color_image_) api_->DestroyImage(device_, color_image_, nullptr);
        if (color_mem_) api_->FreeMemory(device_, color_mem_, nullptr);

        make_sampled_image(w, h, vk::VK_FORMAT_R8G8B8A8_UNORM, color_image_, color_mem_, color_view_);
        color_w_ = w; color_h_ = h;
        atlas_dirty_binding_ = true;
    }

    /// Create a sampled image + backing memory + view, and transition it to
    /// SHADER_READ_ONLY so the first frame can sample it before any upload.
    void make_sampled_image(int w, int h, int format, vk::VkImage& image,
                            vk::VkDeviceMemory& mem, vk::VkImageView& view) {
        vk::VkImageCreateInfo ici{};
        ici.sType = vk::VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = vk::VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = vk::VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = vk::VK_IMAGE_TILING_OPTIMAL;
        ici.usage = vk::VK_IMAGE_USAGE_SAMPLED_BIT | vk::VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = vk::VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = vk::VK_IMAGE_LAYOUT_UNDEFINED;
        api_->CreateImage(device_, &ici, nullptr, &image);

        vk::VkMemoryRequirements req{};
        api_->GetImageMemoryRequirements(device_, image, &req);
        vk::VkMemoryAllocateInfo mai{};
        mai.sType = vk::VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory(req.memoryTypeBits, vk::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        api_->AllocateMemory(device_, &mai, nullptr, &mem);
        api_->BindImageMemory(device_, image, mem, 0);

        vk::VkImageViewCreateInfo vci{};
        vci.sType = vk::VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image; vci.viewType = vk::VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        api_->CreateImageView(device_, &vci, nullptr, &view);

        one_shot([&](vk::VkCommandBuffer cb) {
            barrier_image(cb, image, vk::VK_IMAGE_LAYOUT_UNDEFINED,
                          vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          0, vk::VK_ACCESS_SHADER_READ_BIT,
                          vk::VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
    }

    // ── pipeline ─────────────────────────────────────────────────────────

    [[nodiscard]] vk::VkShaderModule make_module(const std::uint32_t* code, std::size_t words) {
        vk::VkShaderModuleCreateInfo ci{};
        ci.sType = vk::VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = words * sizeof(std::uint32_t);
        ci.pCode = code;
        vk::VkShaderModule m = nullptr;
        api_->CreateShaderModule(device_, &ci, nullptr, &m);
        return m;
    }

    [[nodiscard]] bool create_pipeline(int color_format, bool for_present) {
        // Two descriptor sets.
        //
        //   set 0  what every draw shares: binding 0 the coverage atlas (R8),
        //          1 the colour atlas (RGBA, emoji), 2 the backdrop capture
        //   set 1  ONE user texture (an image(), a video frame, a compositor
        //          client's buffer), bound per batch
        //
        // Set 1 used to be binding 3 of set 0, holding "the current user
        // texture": one image for the whole frame, so two image() nodes both
        // sampled whichever was uploaded last. A compositor shows many
        // windows at once; each needs its own.
        vk::VkDescriptorSetLayoutBinding bindings[3]{};
        for (int i = 0; i < 3; ++i) {
            bindings[i].binding = static_cast<std::uint32_t>(i);
            bindings[i].descriptorType = vk::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = vk::VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        vk::VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = vk::VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 3; dslci.pBindings = bindings;
        if (api_->CreateDescriptorSetLayout(device_, &dslci, nullptr, &set_layout_) != vk::VK_SUCCESS) return false;

        vk::VkDescriptorSetLayoutBinding tex_binding = bindings[0];
        vk::VkDescriptorSetLayoutCreateInfo tslci = dslci;
        tslci.bindingCount = 1; tslci.pBindings = &tex_binding;
        if (api_->CreateDescriptorSetLayout(device_, &tslci, nullptr, &tex_set_layout_) != vk::VK_SUCCESS) return false;

        // Set 0, plus a pool of set-1 descriptors that are RECYCLED (a
        // released texture's set goes back on a free list once its frame
        // retires) rather than freed: no FREE_DESCRIPTOR_SET pool, no
        // fragmentation, and the count bounds how many textures can be live.
        vk::VkDescriptorPoolSize pool_size{vk::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 + kMaxTextures};
        vk::VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = vk::VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1 + kMaxTextures; dpci.poolSizeCount = 1; dpci.pPoolSizes = &pool_size;
        if (api_->CreateDescriptorPool(device_, &dpci, nullptr, &desc_pool_) != vk::VK_SUCCESS) return false;

        vk::VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = vk::VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = desc_pool_; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &set_layout_;
        api_->AllocateDescriptorSets(device_, &dsai, &desc_set_);

        // Set 1 for texture 0: the colour atlas, so a batch with no user
        // texture still binds a COMPLETE set (an unwritten one is UB).
        fallback_tex_set_ = allocate_tex_set();

        // push constant: viewport vec2
        vk::VkPushConstantRange push{};
        push.stageFlags = vk::VK_SHADER_STAGE_VERTEX_BIT;
        push.offset = 0; push.size = sizeof(float) * 2;
        const vk::VkDescriptorSetLayout set_layouts[2] = {set_layout_, tex_set_layout_};
        vk::VkPipelineLayoutCreateInfo plci{};
        plci.sType = vk::VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 2; plci.pSetLayouts = set_layouts;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &push;
        if (api_->CreatePipelineLayout(device_, &plci, nullptr, &layout_) != vk::VK_SUCCESS) return false;

        // render pass
        vk::VkAttachmentDescription att{};
        att.format = color_format; att.samples = vk::VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = vk::VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = vk::VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = vk::VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = vk::VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = vk::VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = for_present ? vk::VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                      : vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vk::VkAttachmentReference ref{0, vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        vk::VkSubpassDescription sub{};
        sub.pipelineBindPoint = vk::VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        vk::VkSubpassDependency dep{};
        dep.srcSubpass = vk::VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask = vk::VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = vk::VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        // A frame can span several passes over one target (backdrop
        // segments, the encoded-space text pass), each LOADING and blending
        // onto what the previous one wrote: make those writes visible.
        dep.srcAccessMask = vk::VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = vk::VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            vk::VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vk::VkRenderPassCreateInfo rpci{};
        rpci.sType = vk::VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1; rpci.pAttachments = &att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1; rpci.pDependencies = &dep;
        if (api_->CreateRenderPass(device_, &rpci, nullptr, &render_pass_) != vk::VK_SUCCESS) return false;

        // A second, content-preserving render pass for the backdrop segment.
        // Identical, except loadOp=LOAD and the initial layout is whatever the
        // first segment left (colour attachment or present-src), so the second
        // draw composites on top of the already-rendered background.
        vk::VkAttachmentDescription att2 = att;
        att2.loadOp = vk::VK_ATTACHMENT_LOAD_OP_LOAD;
        att2.initialLayout = vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vk::VkRenderPassCreateInfo rpci2 = rpci;
        rpci2.pAttachments = &att2;
        if (api_->CreateRenderPass(device_, &rpci2, nullptr, &render_pass_load_) != vk::VK_SUCCESS) return false;

        // Mid-frame segments (before a backdrop is captured) must end in the
        // attachment layout, not present-src: the capture copies from there.
        vk::VkAttachmentDescription att3 = att;
        att3.finalLayout = vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vk::VkRenderPassCreateInfo rpci3 = rpci;
        rpci3.pAttachments = &att3;
        if (api_->CreateRenderPass(device_, &rpci3, nullptr, &render_pass_clear_mid_) != vk::VK_SUCCESS) return false;
        vk::VkAttachmentDescription att4 = att2;
        att4.finalLayout = vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vk::VkRenderPassCreateInfo rpci4 = rpci;
        rpci4.pAttachments = &att4;
        if (api_->CreateRenderPass(device_, &rpci4, nullptr, &render_pass_load_mid_) != vk::VK_SUCCESS) return false;

        // shaders
        vk::VkShaderModule vert = make_module(render::spirv::vertex,
                                              sizeof(render::spirv::vertex) / sizeof(std::uint32_t));
        vk::VkShaderModule frag = make_module(render::spirv::fragment,
                                              sizeof(render::spirv::fragment) / sizeof(std::uint32_t));
        if (vert == nullptr || frag == nullptr) return false;
        std::array<vk::VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = vk::VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = vk::VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
        stages[1].sType = vk::VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = vk::VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

        // vertex input: one per-instance binding = the Instance struct, 8 attrs
        vk::VkVertexInputBindingDescription bind{};
        bind.binding = 0; bind.stride = sizeof(Instance); bind.inputRate = vk::VK_VERTEX_INPUT_RATE_INSTANCE;
        std::array<vk::VkVertexInputAttributeDescription, 8> attrs{};
        auto set_attr = [&](int i, int fmt, std::size_t off) {
            attrs[i].location = static_cast<std::uint32_t>(i); attrs[i].binding = 0;
            attrs[i].format = fmt; attrs[i].offset = static_cast<std::uint32_t>(off);
        };
        set_attr(0, vk::VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, rect));
        set_attr(1, vk::VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, radii));
        set_attr(2, vk::VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, color));
        set_attr(3, vk::VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, color2));
        set_attr(4, vk::VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, axis));
        set_attr(5, vk::VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, uv));
        set_attr(6, vk::VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, params));
        set_attr(7, vk::VK_FORMAT_R32G32B32A32_UINT, offsetof(Instance, kind));
        vk::VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = vk::VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = 8; vi.pVertexAttributeDescriptions = attrs.data();

        vk::VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = vk::VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        vk::VkPipelineViewportStateCreateInfo vp{};
        vp.sType = vk::VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1; vp.scissorCount = 1;

        vk::VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = vk::VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = vk::VK_POLYGON_MODE_FILL; rs.cullMode = vk::VK_CULL_MODE_NONE;
        rs.frontFace = vk::VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        vk::VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = vk::VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = vk::VK_SAMPLE_COUNT_1_BIT;

        // Premultiplied alpha over: src + dst*(1-srcA). Matches the software
        // rasteriser's blend so GPU and CPU output are pixel-comparable.
        vk::VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = 1;
        cba.srcColorBlendFactor = vk::VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = vk::VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = vk::VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = vk::VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = vk::VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = vk::VK_BLEND_OP_ADD;
        cba.colorWriteMask = vk::VK_COLOR_COMPONENT_R_BIT | vk::VK_COLOR_COMPONENT_G_BIT |
                             vk::VK_COLOR_COMPONENT_B_BIT | vk::VK_COLOR_COMPONENT_A_BIT;
        vk::VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = vk::VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        const int dyn[2] = {vk::VK_DYNAMIC_STATE_VIEWPORT, vk::VK_DYNAMIC_STATE_SCISSOR};
        vk::VkPipelineDynamicStateCreateInfo ds{};
        ds.sType = vk::VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        vk::VkGraphicsPipelineCreateInfo gp{};
        gp.sType = vk::VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages.data();
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
        gp.layout = layout_; gp.renderPass = render_pass_; gp.subpass = 0;
        VkResultCheck r = api_->CreateGraphicsPipelines(device_, nullptr, 1, &gp, nullptr, &pipeline_);

        // The text pipeline: same program with MG_TEXT_PASS = true, drawing
        // into a UNORM view of this very target so the blend runs on the
        // ENCODED bytes. Built only when that view can exist: an sRGB target,
        // and (for a swapchain) mutable-format images. Without it text keeps
        // the coverage-reshaping approximation in the main pipeline.
        const int text_format = unorm_of(color_format);
        // MAYAG_TEXT_BLEND=linear keeps glyphs in the main pipeline (the
        // coverage-reshaping approximation), for A/B comparison.
        const char* tb = std::getenv("MAYAG_TEXT_BLEND");
        const bool want_encoded = tb == nullptr || std::string_view{tb} != "linear";
        if (r == vk::VK_SUCCESS && want_encoded && text_format != 0 && (!for_present || swapchain_mutable_)) {
            vk::VkAttachmentDescription ta = att4;          // LOAD, attachment -> attachment
            ta.format = text_format;
            vk::VkRenderPassCreateInfo trp = rpci;
            trp.pAttachments = &ta;
            // The frame's LAST pass must leave the image as the main passes
            // would (present-src for a swapchain): a second, compatible pass.
            vk::VkAttachmentDescription tl = ta;
            tl.finalLayout = att.finalLayout;
            vk::VkRenderPassCreateInfo trl = trp;
            trl.pAttachments = &tl;
            if (api_->CreateRenderPass(device_, &trl, nullptr, &text_render_pass_last_) != vk::VK_SUCCESS)
                text_render_pass_last_ = nullptr;
            if (text_render_pass_last_ != nullptr &&
                api_->CreateRenderPass(device_, &trp, nullptr, &text_render_pass_) == vk::VK_SUCCESS) {
                const vk::Bool32 on = 1;
                const vk::VkSpecializationMapEntry entry{0, 0, sizeof(vk::Bool32)};
                const vk::VkSpecializationInfo spec{1, &entry, sizeof(on), &on};
                std::array<vk::VkPipelineShaderStageCreateInfo, 2> tstages = stages;
                tstages[1].pSpecializationInfo = &spec;
                vk::VkGraphicsPipelineCreateInfo tgp = gp;
                tgp.pStages = tstages.data();
                tgp.renderPass = text_render_pass_;
                if (api_->CreateGraphicsPipelines(device_, nullptr, 1, &tgp, nullptr, &text_pipeline_) != vk::VK_SUCCESS) {
                    text_pipeline_ = nullptr;
                    api_->DestroyRenderPass(device_, text_render_pass_, nullptr);
                    text_render_pass_ = nullptr;
                }
            } else {
                text_render_pass_ = nullptr;
            }
            if (text_render_pass_ == nullptr && text_render_pass_last_ != nullptr) {
                api_->DestroyRenderPass(device_, text_render_pass_last_, nullptr);
                text_render_pass_last_ = nullptr;
            }
        }
        api_->DestroyShaderModule(device_, vert, nullptr);
        api_->DestroyShaderModule(device_, frag, nullptr);
        return r == vk::VK_SUCCESS;
    }

    void update_atlas_descriptor() {
        vk::VkDescriptorImageInfo info[3]{};
        info[0].sampler = sampler_; info[0].imageView = atlas_view_;
        info[0].imageLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info[1].sampler = sampler_; info[1].imageView = color_view_;
        info[1].imageLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info[2].sampler = sampler_; info[2].imageView = capture_view_;
        info[2].imageLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        vk::VkWriteDescriptorSet w[3]{};
        for (int i = 0; i < 3; ++i) {
            w[i].sType = vk::VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = desc_set_;
            w[i].dstBinding = static_cast<std::uint32_t>(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType = vk::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &info[i];
        }
        api_->UpdateDescriptorSets(device_, 3, w, 0, nullptr);
        // The fallback texture set points at the colour atlas, which a resize
        // of the atlas replaces: rewrite it with the rest.
        write_tex_set(fallback_tex_set_, color_view_);
        atlas_dirty_binding_ = false;
    }

    // ── swapchain ────────────────────────────────────────────────────────

    [[nodiscard]] bool create_swapchain() {
        vk::VkSurfaceCapabilitiesKHR caps{};
        api_->GetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);

        std::uint32_t fmt_count = 0;
        api_->GetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmt_count, nullptr);
        std::vector<vk::VkSurfaceFormatKHR> formats(fmt_count);
        api_->GetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmt_count, formats.data());

        // Prefer an _SRGB surface format. The fragment shader outputs LINEAR
        // premultiplied colour; an _SRGB view makes the hardware linearise the
        // destination before the alpha blend and sRGB-encode on store, so the
        // compositing happens in LINEAR light and antialiased edges are exact —
        // identical to the software rasteriser and to the offscreen path. Only
        // fall back to UNORM if the surface offers no sRGB format at all (rare;
        // then edge blending is gamma-space, but the image is still readable).
        swap_format_ = formats.empty() ? vk::VK_FORMAT_B8G8R8A8_SRGB : formats[0].format;
        int color_space = formats.empty() ? vk::VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : formats[0].colorSpace;
        bool picked = false;
        for (const auto& f : formats) {
            if (f.format == vk::VK_FORMAT_B8G8R8A8_SRGB || f.format == vk::VK_FORMAT_R8G8B8A8_SRGB) {
                swap_format_ = f.format; color_space = f.colorSpace; picked = true; break;
            }
        }
        if (!picked) {
            for (const auto& f : formats) {
                if (f.format == vk::VK_FORMAT_B8G8R8A8_UNORM || f.format == vk::VK_FORMAT_R8G8B8A8_UNORM) {
                    swap_format_ = f.format; color_space = f.colorSpace; break;
                }
            }
        }

        const std::uint32_t pw = static_cast<std::uint32_t>(viewport_.x * dpi_ + 0.5f);
        const std::uint32_t ph = static_cast<std::uint32_t>(viewport_.y * dpi_ + 0.5f);
        swap_extent_ = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent
                     : VkExtentClamp(caps, pw, ph);
        if (swap_extent_.width == 0 || swap_extent_.height == 0) return false;

        std::uint32_t images = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && images > caps.maxImageCount) images = caps.maxImageCount;

        // Present mode: MAILBOX when the driver has it, FIFO otherwise.
        //
        // FIFO blocks vkQueuePresentKHR on vsync and queues frames behind it,
        // so with 3 images a frame reaches the screen 2-3 refreshes after it
        // was drawn, and the loop sits in present instead of reading input.
        // Measured on reactive: present took ~16 ms of every frame and the
        // trail visibly lagged the pointer.
        //
        // MAILBOX never blocks and shows the newest frame at the next vsync.
        // It needs a pacing source or an animating app free-runs (that once
        // ran at ~11k fps); the window host is that source now: it draws
        // animation on its own frame clock and sleeps in jaal's reactor in
        // between, and draws input-driven frames only when input arrives.
        //
        // MAYAG_PRESENT=fifo forces FIFO, for comparing the two; =immediate
        // (or set_low_latency(true), the window's vsync=false) prefers
        // IMMEDIATE: no wait for vblank at all, tearing allowed.
        int present_mode = vk::VK_PRESENT_MODE_FIFO_KHR;
        {
            const char* force = std::getenv("MAYAG_PRESENT");
            const bool want_fifo = force != nullptr && std::string_view{force} == "fifo";
            const bool want_immediate = low_latency_ || (force != nullptr && std::string_view{force} == "immediate");
            std::uint32_t n = 0;
            api_->GetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
            std::vector<int> modes(n);
            if (n > 0) api_->GetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, modes.data());
            for (int m : modes)
                if (m == vk::VK_PRESENT_MODE_MAILBOX_KHR && !want_fifo) present_mode = m;
            if (want_immediate && !want_fifo)
                for (int m : modes)
                    if (m == vk::VK_PRESENT_MODE_IMMEDIATE_KHR) present_mode = m;
        }
        present_mode_ = present_mode;

        vk::VkSwapchainCreateInfoKHR ci{};
        ci.sType = vk::VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface_; ci.minImageCount = images;
        ci.imageFormat = swap_format_; ci.imageColorSpace = color_space;
        ci.imageExtent = swap_extent_; ci.imageArrayLayers = 1;
        // COLOR_ATTACHMENT to render into, TRANSFER_SRC so the presented
        // image can be copied back. That readback is what makes it possible to
        // see WHAT THE USER SEES: the previous screenshot path re-rendered the
        // last draw list offscreen, which is a different pipeline (different
        // image format, no swapchain, no compositor) and therefore could not
        // observe a bug in presentation itself.
        ci.imageUsage = vk::VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        vk::VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = vk::VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = vk::VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        ci.compositeAlpha = vk::VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = present_mode;
        ci.clipped = 1;
        ci.oldSwapchain = swapchain_;
        // Mutable format: the same images are also rendered through a UNORM
        // view by the text pass (see create_pipeline).
        const int view_formats[2] = {swap_format_, unorm_of(swap_format_)};
        vk::VkImageFormatListCreateInfo fmt_list{};
        if (swapchain_mutable_ && view_formats[1] != 0) {
            fmt_list.sType = vk::VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
            fmt_list.viewFormatCount = 2; fmt_list.pViewFormats = view_formats;
            ci.pNext = &fmt_list;
            ci.flags |= vk::VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        }
        vk::VkSwapchainKHR new_swap = nullptr;
        if (api_->CreateSwapchainKHR(device_, &ci, nullptr, &new_swap) != vk::VK_SUCCESS) return false;
        if (swapchain_) destroy_swapchain_objects();
        swapchain_ = new_swap;

        std::uint32_t count = 0;
        api_->GetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
        swap_images_.resize(count);
        api_->GetSwapchainImagesKHR(device_, swapchain_, &count, swap_images_.data());

        swap_views_.resize(count);
        framebuffers_.resize(count);
        text_views_.assign(count, nullptr);
        text_framebuffers_.assign(count, nullptr);
        for (std::uint32_t i = 0; i < count; ++i) {
            vk::VkImageViewCreateInfo vci{};
            vci.sType = vk::VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = swap_images_[i]; vci.viewType = vk::VK_IMAGE_VIEW_TYPE_2D;
            vci.format = swap_format_;
            vci.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            api_->CreateImageView(device_, &vci, nullptr, &swap_views_[i]);
            if (swapchain_mutable_ && unorm_of(swap_format_) != 0) {
                vci.format = unorm_of(swap_format_);
                api_->CreateImageView(device_, &vci, nullptr, &text_views_[i]);
            }
        }
        return true;
    }

    static vk::VkExtent2D VkExtentClamp(const vk::VkSurfaceCapabilitiesKHR& caps,
                                        std::uint32_t w, std::uint32_t h) {
        vk::VkExtent2D e{w, h};
        if (e.width < caps.minImageExtent.width) e.width = caps.minImageExtent.width;
        if (e.width > caps.maxImageExtent.width) e.width = caps.maxImageExtent.width;
        if (e.height < caps.minImageExtent.height) e.height = caps.minImageExtent.height;
        if (e.height > caps.maxImageExtent.height) e.height = caps.maxImageExtent.height;
        return e;
    }

    void make_framebuffers() {
        for (std::size_t i = 0; i < swap_views_.size(); ++i) {
            vk::VkFramebufferCreateInfo fci{};
            fci.sType = vk::VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = render_pass_;
            fci.attachmentCount = 1; fci.pAttachments = &swap_views_[i];
            fci.width = swap_extent_.width; fci.height = swap_extent_.height; fci.layers = 1;
            api_->CreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]);
            if (text_render_pass_ != nullptr && i < text_views_.size() && text_views_[i] != nullptr) {
                fci.renderPass = text_render_pass_;
                fci.pAttachments = &text_views_[i];
                api_->CreateFramebuffer(device_, &fci, nullptr, &text_framebuffers_[i]);
            }
        }
    }

    void rebuild_swapchain() {
        api_->DeviceWaitIdle(device_);
        if (!create_swapchain()) return;
        make_framebuffers();
    }

    void destroy_swapchain_objects() {
        for (auto fb : text_framebuffers_) if (fb) api_->DestroyFramebuffer(device_, fb, nullptr);
        for (auto v : text_views_) if (v) api_->DestroyImageView(device_, v, nullptr);
        text_framebuffers_.clear(); text_views_.clear();
        for (auto fb : framebuffers_) if (fb) api_->DestroyFramebuffer(device_, fb, nullptr);
        for (auto v : swap_views_) if (v) api_->DestroyImageView(device_, v, nullptr);
        framebuffers_.clear(); swap_views_.clear();
    }

    // ── offscreen target ─────────────────────────────────────────────────

    void recreate_offscreen_target(int w, int h) {
        if (off_text_fb_) api_->DestroyFramebuffer(device_, off_text_fb_, nullptr);
        if (off_text_view_) api_->DestroyImageView(device_, off_text_view_, nullptr);
        off_text_fb_ = nullptr; off_text_view_ = nullptr;
        if (off_fb_) api_->DestroyFramebuffer(device_, off_fb_, nullptr);
        if (off_view_) api_->DestroyImageView(device_, off_view_, nullptr);
        if (off_image_) api_->DestroyImage(device_, off_image_, nullptr);
        if (off_mem_) api_->FreeMemory(device_, off_mem_, nullptr);
        if (readback_buf_.buf) destroy_buffer(readback_buf_);

        vk::VkImageCreateInfo ici{};
        ici.sType = vk::VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = vk::VK_IMAGE_TYPE_2D; ici.format = off_format_;
        ici.extent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
        ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = vk::VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = vk::VK_IMAGE_TILING_OPTIMAL;
        ici.usage = vk::VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | vk::VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = vk::VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = vk::VK_IMAGE_LAYOUT_UNDEFINED;
        // Mutable so the text pass can render through a UNORM view (core in
        // Vulkan 1.0 for images; no extension needed offscreen).
        if (text_pipeline_ != nullptr) ici.flags |= vk::VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        api_->CreateImage(device_, &ici, nullptr, &off_image_);
        vk::VkMemoryRequirements req{};
        api_->GetImageMemoryRequirements(device_, off_image_, &req);
        vk::VkMemoryAllocateInfo mai{};
        mai.sType = vk::VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory(req.memoryTypeBits, vk::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        api_->AllocateMemory(device_, &mai, nullptr, &off_mem_);
        api_->BindImageMemory(device_, off_image_, off_mem_, 0);

        vk::VkImageViewCreateInfo vci{};
        vci.sType = vk::VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = off_image_; vci.viewType = vk::VK_IMAGE_VIEW_TYPE_2D;
        vci.format = off_format_;
        vci.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        api_->CreateImageView(device_, &vci, nullptr, &off_view_);

        vk::VkFramebufferCreateInfo fci{};
        fci.sType = vk::VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = render_pass_; fci.attachmentCount = 1; fci.pAttachments = &off_view_;
        fci.width = static_cast<std::uint32_t>(w); fci.height = static_cast<std::uint32_t>(h); fci.layers = 1;
        api_->CreateFramebuffer(device_, &fci, nullptr, &off_fb_);

        if (text_pipeline_ != nullptr) {
            vci.format = unorm_of(off_format_);
            api_->CreateImageView(device_, &vci, nullptr, &off_text_view_);
            fci.renderPass = text_render_pass_; fci.pAttachments = &off_text_view_;
            api_->CreateFramebuffer(device_, &fci, nullptr, &off_text_fb_);
        }

        readback_buf_ = make_buffer(static_cast<vk::DeviceSize>(w) * h * 4,
                                    vk::VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        readback_ptr_ = readback_buf_.mapped;
        readback_size_ = static_cast<vk::DeviceSize>(w) * h * 4;
        off_w_ = w; off_h_ = h;
    }

    /// Make sure the mapped readback buffer can hold a w*h RGBA8 image.
    /// Shared by the offscreen path and by read_swapchain(), which needs it
    /// without allocating an offscreen colour target.
    void ensure_readback(int w, int h) {
        const vk::DeviceSize need = static_cast<vk::DeviceSize>(w) * h * 4;
        if (readback_buf_.buf != nullptr && readback_size_ >= need) return;
        if (readback_buf_.buf) destroy_buffer(readback_buf_);
        readback_buf_ = make_buffer(need, vk::VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        readback_ptr_ = readback_buf_.mapped;
        readback_size_ = need;
    }

    // ── the pass ─────────────────────────────────────────────────────────

    void upload_instances(const DrawList& list) {
        const auto& insts = list.instances();
        const std::size_t bytes = insts.size() * sizeof(Instance);
        if (bytes == 0) return;
        if (bytes > instance_capacity_) {
            api_->DeviceWaitIdle(device_);
            if (instance_buf_.buf) destroy_buffer(instance_buf_);
            instance_capacity_ = bytes * 2;
            instance_buf_ = make_buffer(instance_capacity_,
                                        vk::VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        }
        std::memcpy(instance_buf_.mapped, insts.data(), bytes);
    }

    [[nodiscard]] static bool list_has_backdrop(const DrawList& list) {
        for (const auto& in : list.instances()) {
            if (static_cast<ShapeKind>(in.kind) == ShapeKind::backdrop) return true;
        }
        return false;
    }

    void record_pass(vk::VkCommandBuffer cb, vk::VkFramebuffer fb, vk::VkImage target,
                     const DrawList& list,
                     Color<Srgb> clear, std::uint32_t vw, std::uint32_t vh,
                     vk::VkFramebuffer text_fb = nullptr) {
        if (atlas_dirty_binding_) update_atlas_descriptor();
        // Client buffers this frame samples: acquired from the foreign queue
        // now, released back to it after the last pass (foreign_barriers).
        const auto foreign = foreign_textures(list);
        foreign_barriers(cb, foreign, /*acquire=*/true);

        const auto& insts = list.instances();
        const auto& batches = list.batches();
        const bool encoded = text_pipeline_ != nullptr && text_fb != nullptr;
        const render::FramePlan& plan = planner_.plan(list, vw, vh, encoded);

        // The colour attachment is _SRGB, so the hardware sRGB-encodes on
        // store — including the clear. Give it a LINEAR value so the stored
        // background matches a shape of the same colour (which the shader also
        // emits in linear and the hardware encodes identically). `clear` is in
        // the Srgb space, so linearise it here.
        const auto lin = clear.template to<Linear>();
        vk::VkClearValue cv{};
        cv.color.float32[0] = lin.c0; cv.color.float32[1] = lin.c1;
        cv.color.float32[2] = lin.c2; cv.color.float32[3] = 1.0f;
        const float viewport_push[2] = {static_cast<float>(vw), static_cast<float>(vh)};

        auto begin = [&](vk::VkRenderPass rp, vk::VkFramebuffer target_fb, vk::VkPipeline pipe) {
            vk::VkRenderPassBeginInfo bi{};
            bi.sType = vk::VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            bi.renderPass = rp; bi.framebuffer = target_fb;
            bi.renderArea = {{0, 0}, {vw, vh}};
            bi.clearValueCount = 1; bi.pClearValues = &cv;
            api_->CmdBeginRenderPass(cb, &bi, vk::VK_SUBPASS_CONTENTS_INLINE);
            vk::VkViewport vp{0, 0, static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f};
            api_->CmdSetViewport(cb, 0, 1, &vp);
            api_->CmdBindPipeline(cb, vk::VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            api_->CmdBindDescriptorSets(cb, vk::VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                        0, 1, &desc_set_, 0, nullptr);
            api_->CmdPushConstants(cb, layout_, vk::VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(viewport_push), viewport_push);
            if (!insts.empty()) {
                const vk::DeviceSize o0 = 0;
                api_->CmdBindVertexBuffers(cb, 0, 1, &instance_buf_.buf, &o0);
            }
        };

        // The plan (render/frame_plan.hpp) is the frame in painter order:
        // shape passes into the sRGB view, text passes into the UNORM view
        // (encoded-space blending), and backdrop captures between segments.
        // The first pass clears, the last leaves the final layout; a frame
        // whose plan is empty still clears.
        std::size_t last_pass = plan.steps.size();
        for (std::size_t s = plan.steps.size(); s-- > 0;)
            if (plan.steps[s].kind != render::PassStep::Kind::capture) { last_pass = s; break; }

        bool started = false;
        for (std::size_t s = 0; s < plan.steps.size(); ++s) {
            const render::PassStep& step = plan.steps[s];
            if (step.kind == render::PassStep::Kind::capture) {
                if (!started) {   // nothing drawn yet: the capture still needs the clear
                    begin(render_pass_clear_mid_, fb, pipeline_);
                    api_->CmdEndRenderPass(cb);
                    started = true;
                }
                capture_region(cb, target, backdrop_region(insts[step.capture_instance], vw, vh));
                continue;
            }
            const bool last = s == last_pass;
            if (step.kind == render::PassStep::Kind::text) {
                if (!started) {   // a frame that starts with text: clear first
                    begin(render_pass_clear_mid_, fb, pipeline_);
                    api_->CmdEndRenderPass(cb);
                    started = true;
                }
                begin(last ? text_render_pass_last_ : text_render_pass_, text_fb, text_pipeline_);
            } else if (!started) {
                begin(last ? render_pass_ : render_pass_clear_mid_, fb, pipeline_);
            } else {
                begin(last ? render_pass_load_ : render_pass_load_mid_, fb, pipeline_);
            }
            started = true;
            // Set 1 follows the batch's texture; rebinding only when it
            // changes keeps the common case (no images) at one bind.
            vk::VkDescriptorSet bound_tex = nullptr;
            for (std::uint32_t r = step.runs_begin; r < step.runs_end; ++r) {
                const render::DrawRun& run = plan.runs[r];
                const vk::VkDescriptorSet want = tex_set_for(batches[run.batch].texture);
                if (want != bound_tex) {
                    api_->CmdBindDescriptorSets(cb, vk::VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &want, 0,
                                                nullptr);
                    bound_tex = want;
                }
                vk::VkRect2D scissor = clamp_scissor(batches[run.batch].clip, vw, vh);
                api_->CmdSetScissor(cb, 0, 1, &scissor);
                api_->CmdDraw(cb, 4, run.count, 0, run.first);
            }
            api_->CmdEndRenderPass(cb);
        }
        if (!started || last_pass == plan.steps.size()) {
            // Empty frame, or one that ended on a capture: finish with a pass
            // that leaves the image in its final layout.
            begin(started ? render_pass_load_ : render_pass_, fb, pipeline_);
            api_->CmdEndRenderPass(cb);
        }
        foreign_barriers(cb, foreign, /*acquire=*/false);
    }

    /// The imported (client-owned) images a draw list samples, once each.
    [[nodiscard]] std::vector<vk::VkImage> foreign_textures(const DrawList& list) const {
        std::vector<vk::VkImage> out;
        for (const auto& b : list.batches()) {
            const auto it = textures_.find(b.texture);
            if (it == textures_.end() || !it->second.imported) continue;
            if (std::find(out.begin(), out.end(), it->second.image) == out.end()) out.push_back(it->second.image);
        }
        return out;
    }

    /// Queue-family ownership of client buffers, per frame, the way a
    /// compositor must hold them: ACQUIRED from the foreign queue (from
    /// GENERAL, so the driver keeps the contents the client wrote -- with a
    /// compressed modifier, UNDEFINED would discard them) before sampling,
    /// and RELEASED back after, so the client's next write is ordered after
    /// our reads. Recorded into the frame itself: no extra submit, no wait.
    void foreign_barriers(vk::VkCommandBuffer cb, const std::vector<vk::VkImage>& images, bool acquire) {
        if (images.empty()) return;
        constexpr vk::Flags kShaderRead = 0x00000020;   // ACCESS_SHADER_READ_BIT
        constexpr vk::Flags kTop = 0x00000001, kFragment = 0x00000080, kBottom = 0x00002000;
        constexpr int kGeneral = 1;
        const std::uint32_t foreign = foreign_queue_ ? 0xFFFF'FFFDu : 0xFFFF'FFFEu;
        std::vector<vk::VkImageMemoryBarrier> bs(images.size());
        for (std::size_t i = 0; i < images.size(); ++i) {
            auto& b = bs[i];
            b.sType = vk::VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.image = images[i];
            b.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (acquire) {
                b.srcAccessMask = 0; b.dstAccessMask = kShaderRead;
                b.oldLayout = kGeneral; b.newLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcQueueFamilyIndex = foreign; b.dstQueueFamilyIndex = queue_family_;
            } else {
                b.srcAccessMask = kShaderRead; b.dstAccessMask = 0;
                b.oldLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.newLayout = kGeneral;
                b.srcQueueFamilyIndex = queue_family_; b.dstQueueFamilyIndex = foreign;
            }
        }
        api_->CmdPipelineBarrier(cb, acquire ? kTop : kFragment, acquire ? kFragment : kBottom, 0, 0, nullptr, 0,
                                 nullptr, static_cast<std::uint32_t>(bs.size()), bs.data());
    }

    /// Pixels a backdrop reads: its rect, plus the blur taps (+-r) and the
    /// footprint of the mip they're read from.
    [[nodiscard]] static vk::VkRect2D backdrop_region(const Instance& in, std::uint32_t vw, std::uint32_t vh) {
        const Rect area = Rect{in.rect.x, in.rect.y, in.rect.z, in.rect.w}.expand(kernel::backdrop_reach(in.params.x));
        return clamp_scissor(area, vw, vh);
    }

    /// Copy `reg` of the colour target into the capture image and rebuild its
    /// mips over that region. Leaves the target ready to render into again.
    void capture_region(vk::VkCommandBuffer cb, vk::VkImage target, vk::VkRect2D reg) {
        if (reg.extent.width == 0 || reg.extent.height == 0) reg = {{0, 0}, {1, 1}};
        barrier_image(cb, target, vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      vk::VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, vk::VK_ACCESS_TRANSFER_READ_BIT,
                      vk::VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier_mips(cb, capture_image_, 0, capture_mips_, vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vk::VK_ACCESS_SHADER_READ_BIT,
                     vk::VK_ACCESS_TRANSFER_WRITE_BIT, vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     vk::VK_PIPELINE_STAGE_TRANSFER_BIT);
        {
            vk::VkImageCopy region{};
            region.srcSubresource = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffset = region.dstOffset = {reg.offset.x, reg.offset.y, 0};
            region.extent = {reg.extent.width, reg.extent.height, 1};
            api_->CmdCopyImage(cb, target, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture_image_, vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        // Each level from the one above, over the same region (rounded out).
        std::int32_t x0 = reg.offset.x, y0 = reg.offset.y;
        std::int32_t x1 = x0 + static_cast<std::int32_t>(reg.extent.width);
        std::int32_t y1 = y0 + static_cast<std::int32_t>(reg.extent.height);
        for (std::uint32_t lvl = 1; lvl < capture_mips_; ++lvl) {
            barrier_mips(cb, capture_image_, lvl - 1, 1, vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk::VK_ACCESS_TRANSFER_WRITE_BIT,
                         vk::VK_ACCESS_TRANSFER_READ_BIT, vk::VK_PIPELINE_STAGE_TRANSFER_BIT,
                         vk::VK_PIPELINE_STAGE_TRANSFER_BIT);
            const std::int32_t pw = std::max(capture_w_ >> (lvl - 1), 1), ph = std::max(capture_h_ >> (lvl - 1), 1);
            const std::int32_t cw = std::max(capture_w_ >> lvl, 1), chh = std::max(capture_h_ >> lvl, 1);
            const std::int32_t sx0 = std::min(x0 & ~1, pw), sy0 = std::min(y0 & ~1, ph);
            const std::int32_t sx1 = std::min((x1 + 1) & ~1, pw), sy1 = std::min((y1 + 1) & ~1, ph);
            const std::int32_t dx0 = sx0 / 2, dy0 = sy0 / 2;
            const std::int32_t dx1 = std::min(std::max(sx1 / 2, dx0 + 1), cw), dy1 = std::min(std::max(sy1 / 2, dy0 + 1), chh);
            vk::VkImageBlit b{};
            b.srcSubresource = {vk::VK_IMAGE_ASPECT_COLOR_BIT, lvl - 1, 0, 1};
            b.dstSubresource = {vk::VK_IMAGE_ASPECT_COLOR_BIT, lvl, 0, 1};
            b.srcOffsets[0] = {sx0, sy0, 0}; b.srcOffsets[1] = {std::max(sx1, sx0 + 1), std::max(sy1, sy0 + 1), 1};
            b.dstOffsets[0] = {dx0, dy0, 0}; b.dstOffsets[1] = {dx1, dy1, 1};
            api_->CmdBlitImage(cb, capture_image_, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture_image_, vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                               vk::VK_FILTER_LINEAR);
            x0 = dx0; y0 = dy0; x1 = dx1; y1 = dy1;
        }
        // All but the last level end in TRANSFER_SRC, the last in TRANSFER_DST.
        if (capture_mips_ > 1)
            barrier_mips(cb, capture_image_, 0, capture_mips_ - 1, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, vk::VK_ACCESS_TRANSFER_READ_BIT,
                         vk::VK_ACCESS_SHADER_READ_BIT, vk::VK_PIPELINE_STAGE_TRANSFER_BIT,
                         vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        barrier_mips(cb, capture_image_, capture_mips_ - 1, 1, vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, vk::VK_ACCESS_TRANSFER_WRITE_BIT,
                     vk::VK_ACCESS_SHADER_READ_BIT, vk::VK_PIPELINE_STAGE_TRANSFER_BIT,
                     vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        barrier_image(cb, target, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      vk::VK_ACCESS_TRANSFER_READ_BIT, vk::VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      vk::VK_PIPELINE_STAGE_TRANSFER_BIT, vk::VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    void barrier_mips(vk::VkCommandBuffer cb, vk::VkImage image, std::uint32_t base, std::uint32_t count,
                      int old_layout, int new_layout, vk::Flags src_access, vk::Flags dst_access,
                      vk::Flags src_stage, vk::Flags dst_stage) {
        vk::VkImageMemoryBarrier b{};
        b.sType = vk::VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
        b.oldLayout = old_layout; b.newLayout = new_layout;
        b.srcQueueFamilyIndex = vk::VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = vk::VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1};
        api_->CmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    /// (Re)create the capture image at the frame size, matching the colour
    /// target's format so an image copy is valid. Must be called OUTSIDE an
    /// active command-buffer recording, because it issues its own one_shot to
    /// transition the fresh image — nesting that inside another recording
    /// corrupts the shared one-shot buffer.
    void ensure_capture_image(int w, int h, int format) {
        if (capture_image_ && capture_w_ == w && capture_h_ == h && capture_format_ == format) return;
        // A frame in flight may still sample the old capture.
        if (capture_image_ && api_->DeviceWaitIdle) api_->DeviceWaitIdle(device_);
        if (capture_view_) api_->DestroyImageView(device_, capture_view_, nullptr);
        if (capture_image_) api_->DestroyImage(device_, capture_image_, nullptr);
        if (capture_mem_) api_->FreeMemory(device_, capture_mem_, nullptr);
        capture_format_ = format;
        // Mips feed wide blurs; 6 levels covers a 64 px radius.
        capture_mips_ = 1;
        while (capture_mips_ < 7 && (std::max(w, h) >> capture_mips_) > 0) ++capture_mips_;
        vk::VkImageCreateInfo ici{};
        ici.sType = vk::VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = vk::VK_IMAGE_TYPE_2D; ici.format = format;
        ici.extent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
        ici.mipLevels = capture_mips_; ici.arrayLayers = 1; ici.samples = vk::VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = vk::VK_IMAGE_TILING_OPTIMAL;
        ici.usage = vk::VK_IMAGE_USAGE_SAMPLED_BIT | vk::VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    vk::VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = vk::VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = vk::VK_IMAGE_LAYOUT_UNDEFINED;
        api_->CreateImage(device_, &ici, nullptr, &capture_image_);
        vk::VkMemoryRequirements req{};
        api_->GetImageMemoryRequirements(device_, capture_image_, &req);
        vk::VkMemoryAllocateInfo mai{};
        mai.sType = vk::VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory(req.memoryTypeBits, vk::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        api_->AllocateMemory(device_, &mai, nullptr, &capture_mem_);
        api_->BindImageMemory(device_, capture_image_, capture_mem_, 0);
        vk::VkImageViewCreateInfo vci{};
        vci.sType = vk::VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = capture_image_; vci.viewType = vk::VK_IMAGE_VIEW_TYPE_2D; vci.format = format;
        vci.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, capture_mips_, 0, 1};
        api_->CreateImageView(device_, &vci, nullptr, &capture_view_);
        capture_w_ = w; capture_h_ = h;
        one_shot([&](vk::VkCommandBuffer c) {
            barrier_mips(c, capture_image_, 0, capture_mips_, vk::VK_IMAGE_LAYOUT_UNDEFINED,
                         vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         0, vk::VK_ACCESS_SHADER_READ_BIT,
                         vk::VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
        if (desc_set_) update_capture_descriptor();
    }

    /// Point descriptor binding 2 at the current capture view.
    void update_capture_descriptor() {
        vk::VkDescriptorImageInfo info{};
        info.sampler = sampler_; info.imageView = capture_view_;
        info.imageLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vk::VkWriteDescriptorSet w{};
        w.sType = vk::VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = desc_set_; w.dstBinding = 2; w.descriptorCount = 1;
        w.descriptorType = vk::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        api_->UpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    /// The UNORM format sharing an sRGB format's bytes (0 when there is none,
    /// or the target is not sRGB): the view the text pass renders through.
    [[nodiscard]] static constexpr int unorm_of(int format) noexcept {
        if (format == vk::VK_FORMAT_B8G8R8A8_SRGB) return vk::VK_FORMAT_B8G8R8A8_UNORM;
        if (format == vk::VK_FORMAT_R8G8B8A8_SRGB) return vk::VK_FORMAT_R8G8B8A8_UNORM;
        return 0;
    }

    static vk::VkRect2D clamp_scissor(const Rect& clip, std::uint32_t vw, std::uint32_t vh) {
        // An unbounded clip (the common no-scissor case) covers the frame.
        if (clip.size.x >= 1e8f || clip.size.y >= 1e8f || clip.size.x <= 0.0f || clip.size.y <= 0.0f) {
            return {{0, 0}, {vw, vh}};
        }
        int x0 = static_cast<int>(clip.left());
        int y0 = static_cast<int>(clip.top());
        int x1 = static_cast<int>(clip.right() + 0.999f);
        int y1 = static_cast<int>(clip.bottom() + 0.999f);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > static_cast<int>(vw)) x1 = static_cast<int>(vw);
        if (y1 > static_cast<int>(vh)) y1 = static_cast<int>(vh);
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        return {{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
    }

    // ── teardown ─────────────────────────────────────────────────────────

    // (the public `destroy()` above does the work; ~VulkanDevice calls it)

    // libvulkan's function table needs a non-const home so the device-level
    // procs can be resolved after CreateDevice. `api()` returns a const&; the
    // resolved pointers are stored in a per-device copy.
    A& mutable_api() {
        if (!have_local_api_) { local_api_ = *api_; api_ = &local_api_; have_local_api_ = true; }
        return local_api_;
    }

    // ── state ────────────────────────────────────────────────────────────

    /// Every user texture, uploaded or imported, by id. Ordered so iteration
    /// (teardown) is deterministic.
    std::map<std::uint32_t, Texture> textures_;
    /// Ids for import() (no caller id): a high range, so they can never
    /// collide with an app's image() ids or the atlas sentinels just below
    /// 0xFFFF'FFFE.
    std::uint32_t next_import_slot_ = 0x8000'0000u;
    bool          dmabuf_import_ = false;
    bool          foreign_queue_ = false;   ///< VK_EXT_queue_family_foreign enabled
    /// Whether timeline semaphores and syncobj import are available.
    bool          timeline_sync_ = false;

    /// Frees GPU resources once the frame that last used them completes, so
    /// destroying a texture does not stall the device. See reclaim.hpp.
    Reclaimer     reclaim_;
    /// Counts frames the GPU has FINISHED, not frames submitted. Bumped in
    /// submit() right after the in-flight fence returns, which is the only
    /// moment that fact is known.
    std::uint64_t frames_completed_ = 0;
    /// Built at device creation; see importable_formats() for why not lazily.
    std::vector<FormatModifier> importable_;

    /// DRM fourcc -> VkFormat.
    ///
    /// Returns 0 for anything unmapped, which import() turns into
    /// unsupported_format. Deliberately small: these four cover almost every
    /// desktop client, and adding YUV means adding sampler-ycbcr conversion,
    /// which is a separate piece of work rather than another table row.
    ///
    /// Note the byte-order flip. DRM names channels in LITTLE-ENDIAN word
    /// order and Vulkan names them in memory order, so DRM's ARGB8888 is
    /// Vulkan's B8G8R8A8. Getting this backwards swaps red and blue -- the
    /// classic "why is everything teal" compositor bug.
    [[nodiscard]] static int vk_format_of(Fourcc f) noexcept {
        constexpr int kB8G8R8A8_UNORM = 44;
        constexpr int kR8G8B8A8_UNORM = 37;
        if (f == formats::argb8888 || f == formats::xrgb8888) return kB8G8R8A8_UNORM;
        if (f == formats::abgr8888 || f == formats::xbgr8888) return kR8G8B8A8_UNORM;
        return 0;
    }

    const A* api_ = nullptr;
    bool content_changed_ = true;   ///< pixels changed since the last present (texture upload)
    bool acquire_failed_ = false;   ///< the last submit drew nothing
    bool prepared_ = false;
    A local_api_{};
    bool have_local_api_ = false;

    vk::VkInstance instance_ = nullptr;
    vk::VkPhysicalDevice phys_ = nullptr;
    vk::VkDevice device_ = nullptr;
    vk::VkQueue queue_ = nullptr;
    std::uint32_t queue_family_ = 0;
    vk::VkPhysicalDeviceMemoryProperties mem_props_{};
    char device_name_[256] = "vulkan";
    int  present_mode_ = 2;   // VK_PRESENT_MODE_FIFO_KHR until a swapchain exists
    bool low_latency_ = false;   ///< prefer IMMEDIATE (the window's vsync = false)

    vk::VkCommandPool cmd_pool_ = nullptr;
    vk::VkCommandBuffer frame_cb_ = nullptr, oneshot_cb_ = nullptr;
    vk::VkFence in_flight_ = nullptr, oneshot_fence_ = nullptr;
    vk::VkSemaphore image_available_ = nullptr, render_done_ = nullptr;

    vk::VkRenderPass render_pass_ = nullptr;
    // A second render pass identical to render_pass_ but with loadOp=LOAD, used
    // for the segment that draws backdrops on top of the captured background.
    vk::VkRenderPass render_pass_load_ = nullptr;
    // Glass splits a frame at each backdrop; these segments end in the
    // attachment layout so the capture can copy from the target.
    vk::VkRenderPass render_pass_clear_mid_ = nullptr;
    vk::VkRenderPass render_pass_load_mid_ = nullptr;
    vk::VkPipelineLayout layout_ = nullptr;
    vk::VkPipeline pipeline_ = nullptr;
    // Encoded-space text (kernel::encoded_text_source): a LOAD pass on a
    // UNORM view of the colour target, and the glyph-only pipeline for it.
    // Null when the target cannot be viewed as UNORM; text then blends in
    // the main pipeline with the coverage-reshaping approximation.
    vk::VkRenderPass text_render_pass_ = nullptr;
    vk::VkRenderPass text_render_pass_last_ = nullptr;
    vk::VkPipeline text_pipeline_ = nullptr;
    bool swapchain_mutable_ = false;
    std::vector<vk::VkImageView> text_views_;
    std::vector<vk::VkFramebuffer> text_framebuffers_;
    vk::VkImageView off_text_view_ = nullptr;
    vk::VkFramebuffer off_text_fb_ = nullptr;
    render::FramePlanner planner_;
    vk::VkDescriptorSetLayout set_layout_ = nullptr;
    vk::VkDescriptorSetLayout tex_set_layout_ = nullptr;   ///< set 1: one user texture
    vk::VkDescriptorPool desc_pool_ = nullptr;
    vk::VkDescriptorSet desc_set_ = nullptr;
    vk::VkDescriptorSet fallback_tex_set_ = nullptr;       ///< set 1 for "no texture"
    std::vector<vk::VkDescriptorSet> free_tex_sets_;       ///< recycled, ready to rewrite
    static constexpr std::uint32_t kMaxTextures = 1024;
    vk::VkSampler sampler_ = nullptr;

    // atlas
    vk::VkImage atlas_image_ = nullptr; vk::VkDeviceMemory atlas_mem_ = nullptr;
    vk::VkImageView atlas_view_ = nullptr;
    int atlas_w_ = 0, atlas_h_ = 0;
    // Colour-glyph (emoji) atlas: RGBA, bound at descriptor slot 1.
    vk::VkImage color_image_ = nullptr; vk::VkDeviceMemory color_mem_ = nullptr;
    vk::VkImageView color_view_ = nullptr;
    int color_w_ = 0, color_h_ = 0;
    // Backdrop capture: a snapshot of the colour target taken mid-frame so a
    // frosted-glass panel can sample and blur what is behind it. Bound at
    // descriptor slot 2, sized to the frame.
    vk::VkImage capture_image_ = nullptr; vk::VkDeviceMemory capture_mem_ = nullptr;
    vk::VkImageView capture_view_ = nullptr;
    int capture_w_ = 0, capture_h_ = 0;
    std::uint32_t capture_mips_ = 1;
    int capture_format_ = vk::VK_FORMAT_R8G8B8A8_UNORM;
    bool atlas_dirty_binding_ = false;

    // instance + staging
    Buffer instance_buf_{}; std::size_t instance_capacity_ = 0;
    Buffer staging_buf_{}; void* staging_ptr_ = nullptr; std::size_t staging_size_ = 0;

    // surface / swapchain
    vk::VkSurfaceKHR surface_ = nullptr;
    vk::VkSwapchainKHR swapchain_ = nullptr;
    int swap_format_ = vk::VK_FORMAT_B8G8R8A8_UNORM;
    vk::VkExtent2D swap_extent_{};
    /// Index of the image handed to the last QueuePresentKHR, so read_swapchain
    /// copies the frame the compositor actually got.
    std::uint32_t last_presented_ = 0;
    std::vector<vk::VkImage> swap_images_;
    std::vector<vk::VkImageView> swap_views_;
    std::vector<vk::VkFramebuffer> framebuffers_;
    bool rebuild_swapchain_ = false;

    // offscreen
    bool offscreen_ = false;
    int off_format_ = vk::VK_FORMAT_R8G8B8A8_SRGB;
    vk::VkImage off_image_ = nullptr; vk::VkDeviceMemory off_mem_ = nullptr;
    vk::VkImageView off_view_ = nullptr; vk::VkFramebuffer off_fb_ = nullptr;
    Buffer readback_buf_{}; void* readback_ptr_ = nullptr;
    vk::DeviceSize readback_size_ = 0;
    int off_w_ = 0, off_h_ = 0;

    Vec2 viewport_{};
    float dpi_ = 1.0f;
    bool valid_ = false;
    std::uint64_t frames_ = 0;
};

}  // namespace mayag::backend

// A device owns its handles outright; moving it to the worker that prepares
// it (and back) leaves nothing the sender can still reach.
template <> inline constexpr bool jaal::sendable_opt_in<mayag::backend::VulkanDevice> = true;

#endif  // platform guard
