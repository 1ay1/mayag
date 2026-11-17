#pragma once
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
#include "../core/startup.hpp"
#include "../render/shader_generated.hpp"
#include "backend.hpp"   // AtlasView / view_of
#include "reclaim.hpp"
#include "software.hpp"

// dup()/close(): a dmabuf fd is borrowed, and Vulkan takes ownership of the
// descriptor it is handed, so import duplicates first.
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
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
            recreate_atlas_image(w, h);
            full = true;
        }
        if (full || atlas.coverage_dirty) {
            const std::size_t bytes = static_cast<std::size_t>(w) * h;
            ensure_staging(bytes);
            std::memcpy(staging_ptr_, atlas.coverage, bytes);
            upload_plane(atlas_image_, w, h, /*channels=*/1);
            atlas.ack_coverage();
        }

        // The colour plane (emoji) is uploaded the same way, into its own RGBA
        // image, and only exists once a colour glyph has been cached.
        if (atlas.has_color && atlas.color != nullptr) {
            bool color_full = false;
            if (w != color_w_ || h != color_h_) {
                recreate_color_image(w, h);
                color_full = true;
            }
            if (color_full || atlas.color_dirty) {
                const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
                ensure_staging(bytes);
                std::memcpy(staging_ptr_, atlas.color, bytes);
                upload_plane(color_image_, w, h, /*channels=*/4);
                atlas.ack_color();
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
        if (swapchain_ == nullptr) return;

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
        if (acq == vk::VK_ERROR_OUT_OF_DATE_KHR) { rebuild_swapchain(); return; }
        if (acq != vk::VK_SUCCESS && acq != vk::VK_SUBOPTIMAL_KHR) return;

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
                    list, clear, swap_extent_.width, swap_extent_.height);
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
                        static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
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

    /// Upload an RGBA8 image and make it the texture that KIND_TEXTURE
    /// instances sample. Returns a slot id (1-based; 0 means "no texture").
    ///
    /// This is deliberately a SINGLE bound texture rather than a slot array:
    /// mayag draws a whole frame in a handful of instanced batches, and a
    /// per-instance texture array would need either bindless descriptors or a
    /// batch break per image. One binding covers the cases that exist today
    /// (an image, a video frame) honestly, and the signature leaves room to
    /// grow without changing callers.
    std::uint32_t upload_texture(const std::uint8_t* rgba, int w, int h) {
        if (!valid_ || rgba == nullptr || w <= 0 || h <= 0) return 0;

        if (w != tex_w_ || h != tex_h_) {
            // The size changed, so the old image must go -- but a frame in
            // flight may still be SAMPLING it, and freeing a resource the GPU
            // is reading is undefined behaviour. In practice it hangs inside
            // the driver with no stack that points back here, which is how
            // this first showed up: "it hung in fullscreen on pause".
            //
            // The obvious fix is DeviceWaitIdle, and it works, and it stalls
            // the entire GPU to answer a question about ONE image. Retire it
            // instead: hand the handles to the reclaimer tagged with the
            // current frame, and they are freed once that frame completes.
            // Same correctness, no stall, and a compositor destroying a
            // buffer per closing window does not stop the world each time.
            const std::uint64_t frame = frames_completed_;
            auto* api = api_;
            auto dev = device_;
            auto old_view = tex_view_;
            auto old_image = tex_image_;
            auto old_mem = tex_mem_;
            reclaim_.retire(frame + 1, [api, dev, old_view, old_image, old_mem] {
                if (old_view) api->DestroyImageView(dev, old_view, nullptr);
                if (old_image) api->DestroyImage(dev, old_image, nullptr);
                if (old_mem) api->FreeMemory(dev, old_mem, nullptr);
            });

            tex_view_ = nullptr; tex_image_ = nullptr; tex_mem_ = nullptr;
            make_sampled_image(w, h, vk::VK_FORMAT_R8G8B8A8_UNORM,
                               tex_image_, tex_mem_, tex_view_);
            tex_w_ = w; tex_h_ = h;
        }
        if (tex_image_ == nullptr) return 0;

        const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
        ensure_staging(bytes);
        std::memcpy(staging_ptr_, rgba, bytes);
        upload_plane(tex_image_, w, h, /*channels=*/4);

        atlas_dirty_binding_ = true;   // rebind so the shader sees it
        return 1;
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
        const int dup_fd = ::dup(d.planes[0].fd);
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
            ::close(dup_fd);              // Vulkan did not take it after all
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
        vci.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (api_->CreateImageView(device_, &vci, nullptr, &view) != vk::VK_SUCCESS) {
            api_->FreeMemory(device_, mem, nullptr);
            api_->DestroyImage(device_, image, nullptr);
            return std::unexpected(ImportError::invalid_buffer);
        }

        // The client wrote these pixels with the GPU, outside any layout mayag
        // knows about, so the image arrives in UNDEFINED and must be moved to
        // a layout a shader can sample.
        one_shot([&](vk::VkCommandBuffer cb) {
            constexpr vk::Flags kShaderRead = 0x00000020;   // ACCESS_SHADER_READ_BIT
            constexpr vk::Flags kTopOfPipe  = 0x00000001;   // STAGE_TOP_OF_PIPE_BIT
            constexpr vk::Flags kFragment   = 0x00000080;   // STAGE_FRAGMENT_SHADER_BIT
            barrier_image(cb, image, 0 /*UNDEFINED*/,
                          vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          0, kShaderRead, kTopOfPipe, kFragment);
        });

        const std::uint32_t slot = next_import_slot_++;
        imports_.push_back(Imported{slot, image, view, mem});
        return TextureHandle{slot};
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
        const auto it = std::find_if(imports_.begin(), imports_.end(),
                                     [&](const Imported& i) { return i.slot == slot; });
        if (it == imports_.end()) {
            // Unknown slot: nothing is holding the buffer, so it is free now.
            // Reporting rather than staying silent means a caller never has
            // to guess whether its callback was dropped.
            if (on_free) on_free();
            return;
        }

        auto* api = api_;
        auto dev = device_;
        auto view = it->view;
        auto image = it->image;
        auto mem = it->memory;
        reclaim_.retire(frames_completed_ + 1,
                        [api, dev, view, image, mem, on_free = std::move(on_free)] {
            if (view) api->DestroyImageView(dev, view, nullptr);
            if (image) api->DestroyImage(dev, image, nullptr);
            if (mem) api->FreeMemory(dev, mem, nullptr);
            if (on_free) on_free();
        });
        imports_.erase(it);
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
        const int dup_fd = ::dup(p.fd);
        if (dup_fd < 0) { api_->DestroySemaphore(device_, sem, nullptr); return false; }

        vk::VkImportSemaphoreFdInfoKHR info{};
        info.sType = vk::VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        info.semaphore = sem;
        info.handleType = vk::VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        info.fd = dup_fd;
        if (api_->ImportSemaphoreFdKHR(device_, &info) != vk::VK_SUCCESS) {
            ::close(dup_fd);
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

    void release_texture() {
        // Retired rather than waited on, like the resize path above. This one
        // runs when an app drops its last image and at teardown; teardown
        // calls collect_all() after the device really is idle, which is the
        // one place a blocking wait is the right answer.
        auto* api = api_;
        auto dev = device_;
        auto view = tex_view_;
        auto image = tex_image_;
        auto mem = tex_mem_;
        reclaim_.retire(frames_completed_ + 1, [api, dev, view, image, mem] {
            if (view) api->DestroyImageView(dev, view, nullptr);
            if (image) api->DestroyImage(dev, image, nullptr);
            if (mem) api->FreeMemory(dev, mem, nullptr);
        });
        tex_view_ = nullptr; tex_image_ = nullptr; tex_mem_ = nullptr;
        tex_w_ = tex_h_ = 0;
        atlas_dirty_binding_ = true;
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

        // The device really is idle now, so everything retired is safe to
        // free regardless of which frame it belonged to. This is the ONE
        // place a blocking wait is the right answer: the device is going away
        // and nothing may be left holding its memory.
        reclaim_.collect_all();

        destroy_swapchain_objects();
        if (swapchain_) api_->DestroySwapchainKHR(device_, swapchain_, nullptr);
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
        if (layout_) api_->DestroyPipelineLayout(device_, layout_, nullptr);
        if (desc_pool_) api_->DestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (set_layout_) api_->DestroyDescriptorSetLayout(device_, set_layout_, nullptr);
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
        // descriptor set layout: four combined image samplers — binding 0 is
        // the coverage atlas (R8), binding 1 the colour atlas (RGBA, emoji),
        // binding 2 the backdrop capture (RGBA snapshot of the frame), and
        // binding 3 the current user texture (RGBA, an image()/video frame).
        vk::VkDescriptorSetLayoutBinding bindings[4]{};
        for (int i = 0; i < 4; ++i) {
            bindings[i].binding = static_cast<std::uint32_t>(i);
            bindings[i].descriptorType = vk::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = vk::VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        vk::VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = vk::VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 4; dslci.pBindings = bindings;
        if (api_->CreateDescriptorSetLayout(device_, &dslci, nullptr, &set_layout_) != vk::VK_SUCCESS) return false;

        vk::VkDescriptorPoolSize pool_size{vk::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
        vk::VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = vk::VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &pool_size;
        if (api_->CreateDescriptorPool(device_, &dpci, nullptr, &desc_pool_) != vk::VK_SUCCESS) return false;

        vk::VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = vk::VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = desc_pool_; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &set_layout_;
        api_->AllocateDescriptorSets(device_, &dsai, &desc_set_);

        // push constant: viewport vec2
        vk::VkPushConstantRange push{};
        push.stageFlags = vk::VK_SHADER_STAGE_VERTEX_BIT;
        push.offset = 0; push.size = sizeof(float) * 2;
        vk::VkPipelineLayoutCreateInfo plci{};
        plci.sType = vk::VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &set_layout_;
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
        dep.dstAccessMask = vk::VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
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
        api_->DestroyShaderModule(device_, vert, nullptr);
        api_->DestroyShaderModule(device_, frag, nullptr);
        return r == vk::VK_SUCCESS;
    }

    void update_atlas_descriptor() {
        vk::VkDescriptorImageInfo info[4]{};
        info[0].sampler = sampler_; info[0].imageView = atlas_view_;
        info[0].imageLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info[1].sampler = sampler_; info[1].imageView = color_view_;
        info[1].imageLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info[2].sampler = sampler_; info[2].imageView = capture_view_;
        info[2].imageLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // Binding 3 falls back to the colour atlas when no user texture is
        // bound: a descriptor set must be COMPLETE before it is used, and an
        // unwritten binding is undefined behaviour rather than a blank sample.
        info[3].sampler = sampler_;
        info[3].imageView = tex_view_ != nullptr ? tex_view_ : color_view_;
        info[3].imageLayout = vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        vk::VkWriteDescriptorSet w[4]{};
        for (int i = 0; i < 4; ++i) {
            w[i].sType = vk::VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = desc_set_;
            w[i].dstBinding = static_cast<std::uint32_t>(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType = vk::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &info[i];
        }
        api_->UpdateDescriptorSets(device_, 4, w, 0, nullptr);
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
        // MAYAG_PRESENT=fifo forces FIFO, for comparing the two.
        int present_mode = vk::VK_PRESENT_MODE_FIFO_KHR;
        {
            const char* force = std::getenv("MAYAG_PRESENT");
            const bool want_fifo = force != nullptr && std::string_view{force} == "fifo";
            std::uint32_t n = 0;
            api_->GetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
            std::vector<int> modes(n);
            if (n > 0) api_->GetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, modes.data());
            for (int m : modes)
                if (m == vk::VK_PRESENT_MODE_MAILBOX_KHR && !want_fifo) present_mode = m;
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
        for (std::uint32_t i = 0; i < count; ++i) {
            vk::VkImageViewCreateInfo vci{};
            vci.sType = vk::VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = swap_images_[i]; vci.viewType = vk::VK_IMAGE_VIEW_TYPE_2D;
            vci.format = swap_format_;
            vci.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            api_->CreateImageView(device_, &vci, nullptr, &swap_views_[i]);
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
        }
    }

    void rebuild_swapchain() {
        api_->DeviceWaitIdle(device_);
        if (!create_swapchain()) return;
        make_framebuffers();
    }

    void destroy_swapchain_objects() {
        for (auto fb : framebuffers_) if (fb) api_->DestroyFramebuffer(device_, fb, nullptr);
        for (auto v : swap_views_) if (v) api_->DestroyImageView(device_, v, nullptr);
        framebuffers_.clear(); swap_views_.clear();
    }

    // ── offscreen target ─────────────────────────────────────────────────

    void recreate_offscreen_target(int w, int h) {
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
                     Color<Srgb> clear, std::uint32_t vw, std::uint32_t vh) {
        if (atlas_dirty_binding_) update_atlas_descriptor();

        // Does the frame contain any frosted-glass panels? If so it renders in
        // two segments: everything up to the backdrops, a mid-frame snapshot
        // of the colour target, then the backdrops sampling it.
        const auto& insts = list.instances();
        bool has_backdrop = false;
        for (const auto& in : insts) {
            if (static_cast<ShapeKind>(in.kind) == ShapeKind::backdrop) { has_backdrop = true; break; }
        }

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

        auto begin = [&](vk::VkRenderPass rp) {
            vk::VkRenderPassBeginInfo bi{};
            bi.sType = vk::VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            bi.renderPass = rp; bi.framebuffer = fb;
            bi.renderArea = {{0, 0}, {vw, vh}};
            bi.clearValueCount = 1; bi.pClearValues = &cv;
            api_->CmdBeginRenderPass(cb, &bi, vk::VK_SUBPASS_CONTENTS_INLINE);
            vk::VkViewport vp{0, 0, static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f};
            api_->CmdSetViewport(cb, 0, 1, &vp);
            api_->CmdBindPipeline(cb, vk::VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
            api_->CmdBindDescriptorSets(cb, vk::VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                        0, 1, &desc_set_, 0, nullptr);
            api_->CmdPushConstants(cb, layout_, vk::VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(viewport_push), viewport_push);
            if (!insts.empty()) {
                const vk::DeviceSize o0 = 0;
                api_->CmdBindVertexBuffers(cb, 0, 1, &instance_buf_.buf, &o0);
            }
        };
        if (!has_backdrop) {
            begin(render_pass_);
            for (const auto& batch : list.batches()) {
                vk::VkRect2D scissor = clamp_scissor(batch.clip, vw, vh);
                api_->CmdSetScissor(cb, 0, 1, &scissor);
                api_->CmdDraw(cb, 4, batch.count, 0, batch.first);
            }
            api_->CmdEndRenderPass(cb);
            return;
        }

        // Glass reads what is drawn BEHIND it, and whatever comes after it in
        // the list (its own fill, its text, later panels) must land on top of
        // the blur. So the frame renders in painter order, split at each
        // backdrop: draw up to it, snapshot the region it reads, blur.
        const auto& batches = list.batches();
        const auto is_bd = [&](const Batch& b) {
            return b.count > 0 && static_cast<ShapeKind>(insts[b.first].kind) == ShapeKind::backdrop;
        };
        const auto draw_range = [&](std::size_t a, std::size_t b) {
            for (std::size_t i = a; i < b; ++i) {
                const auto& batch = batches[i];
                vk::VkRect2D scissor = clamp_scissor(batch.clip, vw, vh);
                api_->CmdSetScissor(cb, 0, 1, &scissor);
                api_->CmdDraw(cb, 4, batch.count, 0, batch.first);
            }
        };
        std::size_t last_bd = 0;
        for (std::size_t i = 0; i < batches.size(); ++i) if (is_bd(batches[i])) last_bd = i;

        bool started = false;
        std::size_t from = 0;
        for (std::size_t i = 0; i < batches.size(); ++i) {
            if (!is_bd(batches[i])) continue;
            // Everything up to this backdrop.
            begin(started ? render_pass_load_mid_ : render_pass_clear_mid_);
            started = true;
            draw_range(from, i);
            api_->CmdEndRenderPass(cb);

            capture_region(cb, target, backdrop_region(insts[batches[i].first], vw, vh));

            // The backdrop itself, then (after the last one) the rest.
            const bool last = i == last_bd;
            begin(last ? render_pass_load_ : render_pass_load_mid_);
            draw_range(i, i + 1);
            if (last) draw_range(i + 1, batches.size());
            api_->CmdEndRenderPass(cb);
            from = i + 1;
        }
    }

    /// Pixels a backdrop reads: its rect, plus the blur taps (+-r) and the
    /// footprint of the mip they're read from.
    [[nodiscard]] static vk::VkRect2D backdrop_region(const Instance& in, std::uint32_t vw, std::uint32_t vh) {
        const float r = num::max(in.params.x, 0.0f);
        const float pad = 1.5f * r + 4.0f;
        const Rect area = Rect{in.rect.x, in.rect.y, in.rect.z, in.rect.w}.expand(pad);
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

    /// A buffer imported from a dmabuf. Kept in a list rather than a single
    /// slot because a compositor holds one per client surface, not one total.
    struct Imported {
        std::uint32_t     slot = 0;
        vk::VkImage       image = nullptr;
        vk::VkImageView   view = nullptr;
        vk::VkDeviceMemory memory = nullptr;
    };
    std::vector<Imported> imports_;
    /// Slot 0 means "invalid handle", so numbering starts at 1.
    std::uint32_t next_import_slot_ = 1;
    bool          dmabuf_import_ = false;
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
    vk::VkDescriptorSetLayout set_layout_ = nullptr;
    vk::VkDescriptorPool desc_pool_ = nullptr;
    vk::VkDescriptorSet desc_set_ = nullptr;
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
    // The single user texture (image(), video frames). See upload_texture.
    vk::VkImage tex_image_ = nullptr; vk::VkDeviceMemory tex_mem_ = nullptr;
    vk::VkImageView tex_view_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
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
