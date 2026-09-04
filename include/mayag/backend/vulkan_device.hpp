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
#include "../render/spirv_generated.hpp"
#include "software.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
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
        // sRGB attachment: the shader outputs LINEAR premultiplied colour and
        // the hardware sRGB-encodes it on store, so the readback is byte-for-
        // byte comparable with the software rasteriser (which sRGB-encodes in
        // software). Without this the readback is linear and every colour is
        // wrong by the transfer curve.
        off_format_ = vk::VK_FORMAT_R8G8B8A8_SRGB;
        if (!create_pipeline(off_format_, /*for_present=*/false)) return false;
        offscreen_ = true;
        valid_ = true;
        return true;
    }

    /// Device + swapchain on a live Wayland surface.
    [[nodiscard]] bool attach_wayland(void* display, void* surface,
                                      Vec2 logical, float dpi) {
        if (!create_instance(/*want_surface=*/true)) return false;
        if (api_->CreateWaylandSurfaceKHR == nullptr) return false;

        vk::VkWaylandSurfaceCreateInfoKHR sci{};
        sci.sType = vk::VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        sci.display = display;
        sci.surface = surface;
        if (api_->CreateWaylandSurfaceKHR(instance_, &sci, nullptr, &surface_) != vk::VK_SUCCESS) {
            return false;
        }
        if (!pick_device_and_queue(surface_)) return false;
        if (!create_device()) return false;
        if (!create_command_infra()) return false;
        if (!create_sampler_and_atlas()) return false;

        dpi_ = dpi;
        viewport_ = logical;
        if (!create_swapchain()) return false;
        if (!create_pipeline(swap_format_, /*for_present=*/true)) return false;
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
    template <typename AtlasT>
    void sync_atlas(AtlasT& atlas) {
        if (!valid_) return;
        const int w = atlas.width(), h = atlas.height();
        if (w <= 0 || h <= 0) return;

        // (Re)create the coverage image if the size changed, forcing a full
        // re-upload.
        bool full = false;
        if (w != atlas_w_ || h != atlas_h_) {
            recreate_atlas_image(w, h);
            full = true;
        }
        if (full || !atlas.dirty_region().empty()) {
            const std::size_t bytes = static_cast<std::size_t>(w) * h;
            ensure_staging(bytes);
            std::memcpy(staging_ptr_, atlas.pixels().data(), bytes);
            upload_plane(atlas_image_, w, h, /*channels=*/1);
            atlas.clear_dirty();
        }

        // The colour plane (emoji) is uploaded the same way, into its own RGBA
        // image, and only exists once a colour glyph has been cached.
        if (atlas.has_color()) {
            bool color_full = false;
            if (w != color_w_ || h != color_h_) {
                recreate_color_image(w, h);
                color_full = true;
            }
            if (color_full || !atlas.color_dirty_region().empty()) {
                const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
                ensure_staging(bytes);
                std::memcpy(staging_ptr_, atlas.color_pixels().data(), bytes);
                upload_plane(color_image_, w, h, /*channels=*/4);
                atlas.clear_color_dirty();
            }
        }
    }

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

    /// Tear down all GPU resources. Idempotent, and public because the window
    /// must call it explicitly while the wl_surface/display it wraps are still
    /// alive — the destructor calls it too, harmlessly, if the window did not.
    void destroy() {
        if (device_ == nullptr || api_ == nullptr) return;
        if (api_->DeviceWaitIdle) api_->DeviceWaitIdle(device_);

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
        app.apiVersion = (1u << 22);   // VK_API_VERSION_1_0

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

        vk::VkDeviceCreateInfo dci{};
        dci.sType = vk::VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<std::uint32_t>(dev_exts.size());
        dci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();

        if (api_->CreateDevice(phys_, &dci, nullptr, &device_) != vk::VK_SUCCESS) return false;
        load_device_procs();
        if (api_->GetDeviceQueue == nullptr) return false;
        api_->GetDeviceQueue(device_, queue_family_, 0, &queue_);
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
        sci.maxLod = 0.0f;
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
        // descriptor set layout: three combined image samplers — binding 0 is
        // the coverage atlas (R8), binding 1 the colour atlas (RGBA, emoji),
        // binding 2 the backdrop capture (RGBA snapshot of the frame).
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

        vk::VkDescriptorPoolSize pool_size{vk::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
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

        // Prefer a UNORM (not _SRGB) format: the shader outputs LINEAR premul
        // colour, and storing it through an sRGB view would double-encode.
        // This matches the software path, which encodes to sRGB itself.
        swap_format_ = formats.empty() ? vk::VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
        int color_space = formats.empty() ? vk::VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : formats[0].colorSpace;
        for (const auto& f : formats) {
            if (f.format == vk::VK_FORMAT_B8G8R8A8_UNORM || f.format == vk::VK_FORMAT_R8G8B8A8_UNORM) {
                swap_format_ = f.format; color_space = f.colorSpace; break;
            }
        }

        const std::uint32_t pw = static_cast<std::uint32_t>(viewport_.x * dpi_ + 0.5f);
        const std::uint32_t ph = static_cast<std::uint32_t>(viewport_.y * dpi_ + 0.5f);
        swap_extent_ = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent
                     : VkExtentClamp(caps, pw, ph);
        if (swap_extent_.width == 0 || swap_extent_.height == 0) return false;

        std::uint32_t images = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && images > caps.maxImageCount) images = caps.maxImageCount;

        // Present mode. FIFO (vsync) is always supported and is the correct
        // default — no tearing, and an idle app that never presents costs
        // nothing regardless. MAILBOX is preferred WHEN available because it
        // presents the newest frame instead of queuing behind the vsync
        // deadline: for a bursty live app (a sample lands, one frame goes out)
        // that shaves up to a refresh interval of latency off each update,
        // still without tearing. It needs one more image, which is why images
        // is bumped when we pick it.
        int present_mode = vk::VK_PRESENT_MODE_FIFO_KHR;
        std::uint32_t pm_count = 0;
        api_->GetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pm_count, nullptr);
        std::vector<int> modes(pm_count);
        api_->GetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pm_count, modes.data());
        for (int mode : modes) {
            if (mode == vk::VK_PRESENT_MODE_MAILBOX_KHR) {
                present_mode = mode;
                std::uint32_t want = caps.minImageCount + 2;   // triple-buffer
                if (caps.maxImageCount == 0 || want <= caps.maxImageCount) images = want;
                break;
            }
        }

        vk::VkSwapchainCreateInfoKHR ci{};
        ci.sType = vk::VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface_; ci.minImageCount = images;
        ci.imageFormat = swap_format_; ci.imageColorSpace = color_space;
        ci.imageExtent = swap_extent_; ci.imageArrayLayers = 1;
        ci.imageUsage = vk::VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
        off_w_ = w; off_h_ = h;
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
        auto draw_batches = [&](bool want_backdrop) {
            for (const auto& batch : list.batches()) {
                const bool is_bd = batch.count > 0 &&
                    static_cast<ShapeKind>(insts[batch.first].kind) == ShapeKind::backdrop;
                if (is_bd != want_backdrop) continue;
                vk::VkRect2D scissor = clamp_scissor(batch.clip, vw, vh);
                api_->CmdSetScissor(cb, 0, 1, &scissor);
                api_->CmdDraw(cb, 4, batch.count, 0, batch.first);
            }
        };

        if (!has_backdrop) {
            begin(render_pass_);
            draw_batches(false);
            api_->CmdEndRenderPass(cb);
            return;
        }

        // Segment 1: everything except backdrops.
        begin(render_pass_);
        draw_batches(false);
        api_->CmdEndRenderPass(cb);

        // Snapshot the colour target into the capture image the backdrop shader
        // samples. The capture was sized before recording began (see the
        // callers), so here we only transition + copy.
        barrier_image(cb, target, vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      vk::VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, vk::VK_ACCESS_TRANSFER_READ_BIT,
                      vk::VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier_image(cb, capture_image_, vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      vk::VK_ACCESS_SHADER_READ_BIT, vk::VK_ACCESS_TRANSFER_WRITE_BIT,
                      vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, vk::VK_PIPELINE_STAGE_TRANSFER_BIT);
        {
            vk::VkImageCopy region{};
            region.srcSubresource = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {vw, vh, 1};
            api_->CmdCopyImage(cb, target, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture_image_, vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        barrier_image(cb, capture_image_, vk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      vk::VK_ACCESS_TRANSFER_WRITE_BIT, vk::VK_ACCESS_SHADER_READ_BIT,
                      vk::VK_PIPELINE_STAGE_TRANSFER_BIT, vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        barrier_image(cb, target, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      vk::VK_ACCESS_TRANSFER_READ_BIT, vk::VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      vk::VK_PIPELINE_STAGE_TRANSFER_BIT, vk::VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        update_capture_descriptor();

        // Segment 2: the backdrops, LOADing (preserving) the background and
        // sampling the capture to blur it.
        begin(render_pass_load_);
        draw_batches(true);
        api_->CmdEndRenderPass(cb);
    }

    /// (Re)create the capture image at the frame size, matching the colour
    /// target's format so an image copy is valid. Must be called OUTSIDE an
    /// active command-buffer recording, because it issues its own one_shot to
    /// transition the fresh image — nesting that inside another recording
    /// corrupts the shared one-shot buffer.
    void ensure_capture_image(int w, int h, int format) {
        if (capture_image_ && capture_w_ == w && capture_h_ == h && capture_format_ == format) return;
        if (capture_view_) api_->DestroyImageView(device_, capture_view_, nullptr);
        if (capture_image_) api_->DestroyImage(device_, capture_image_, nullptr);
        if (capture_mem_) api_->FreeMemory(device_, capture_mem_, nullptr);
        capture_format_ = format;
        vk::VkImageCreateInfo ici{};
        ici.sType = vk::VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = vk::VK_IMAGE_TYPE_2D; ici.format = format;
        ici.extent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
        ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = vk::VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = vk::VK_IMAGE_TILING_OPTIMAL;
        ici.usage = vk::VK_IMAGE_USAGE_SAMPLED_BIT | vk::VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
        vci.subresourceRange = {vk::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        api_->CreateImageView(device_, &vci, nullptr, &capture_view_);
        capture_w_ = w; capture_h_ = h;
        one_shot([&](vk::VkCommandBuffer c) {
            barrier_image(c, capture_image_, vk::VK_IMAGE_LAYOUT_UNDEFINED,
                          vk::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          0, vk::VK_ACCESS_SHADER_READ_BIT,
                          vk::VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
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

    const A* api_ = nullptr;
    A local_api_{};
    bool have_local_api_ = false;

    vk::VkInstance instance_ = nullptr;
    vk::VkPhysicalDevice phys_ = nullptr;
    vk::VkDevice device_ = nullptr;
    vk::VkQueue queue_ = nullptr;
    std::uint32_t queue_family_ = 0;
    vk::VkPhysicalDeviceMemoryProperties mem_props_{};
    char device_name_[256] = "vulkan";

    vk::VkCommandPool cmd_pool_ = nullptr;
    vk::VkCommandBuffer frame_cb_ = nullptr, oneshot_cb_ = nullptr;
    vk::VkFence in_flight_ = nullptr, oneshot_fence_ = nullptr;
    vk::VkSemaphore image_available_ = nullptr, render_done_ = nullptr;

    vk::VkRenderPass render_pass_ = nullptr;
    // A second render pass identical to render_pass_ but with loadOp=LOAD, used
    // for the segment that draws backdrops on top of the captured background.
    vk::VkRenderPass render_pass_load_ = nullptr;
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
    int off_w_ = 0, off_h_ = 0;

    Vec2 viewport_{};
    float dpi_ = 1.0f;
    bool valid_ = false;
    std::uint64_t frames_ = 0;
};

}  // namespace mayag::backend

#endif  // platform guard
