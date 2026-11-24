#pragma once
// mayag::platform::images — a window's images: ONE policy for every backend.
//
// image(id) in a view must draw on every path a window has. So the CPU copy
// is ALWAYS kept (the software path samples it, read_pixels and screenshots
// need it, and a GPU that fails or comes up late must not lose a picture),
// and it is mirrored to the GPU device while one is presenting. Cocoa
// (Metal), Wayland (Vulkan) and Win32 (D3D11) each wrote this out for
// themselves, with each device spelling the verbs its own way; now they call
// these, and every device answers to the same two verbs (GpuImages).

#include "../backend/software.hpp"   // ImageStore
#include "../render/draw_list.hpp"   // ImageData

#include <concepts>
#include <cstdint>
#include <memory>

namespace mayag::platform::images {

/// What a GPU device offers for images: the same verbs on Metal, Vulkan and
/// D3D11 (backend/metal.hpp, vulkan_device.hpp, d3d11.hpp).
template <class D>
concept GpuImages = requires(D& d, std::uint32_t id, const std::uint8_t* rgba, int n) {
    d.upload_texture(id, rgba, n, n);
    d.release_texture(id);
};

/// The device of a window built without a GPU path: there is never one, so
/// the mirror below is only ever the CPU copy.
struct NoGpu {
    void upload_texture(std::uint32_t, const std::uint8_t*, int, int) noexcept {}
    void release_texture(std::uint32_t) noexcept {}
};
static_assert(GpuImages<NoGpu>);

/// Keep `img` as image(id): on the CPU always, on `gpu` when it is non-null
/// (the device presenting now).
template <GpuImages D>
void upload(backend::ImageStore& cpu, D* gpu, std::uint32_t id, std::shared_ptr<const ImageData> img) {
    if (!img) return;
    if (gpu != nullptr) (void)gpu->upload_texture(id, img->rgba.data(), img->width, img->height);
    cpu.upload(id, std::move(img));
}

/// Forget image(id) everywhere it is kept.
template <GpuImages D>
void release(backend::ImageStore& cpu, D* gpu, std::uint32_t id) {
    cpu.release(id);
    if (gpu != nullptr) gpu->release_texture(id);
}

/// A device that starts presenting after images were uploaded (a GPU adopted
/// late, a device rebuilt after loss): give it every image the window keeps.
template <GpuImages D>
void replay(const backend::ImageStore& cpu, D& gpu) {
    for (const auto& [id, img] : cpu.all()) (void)gpu.upload_texture(id, img->rgba.data(), img->width, img->height);
}

}  // namespace mayag::platform::images
