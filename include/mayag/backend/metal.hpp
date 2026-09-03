#pragma once
// mayag::backend::Metal — the GPU path
//
// Why this exists, in numbers. The software rasteriser, measured on an M1
// with 200 instances (a modest UI):
//
//     800x600      1.20 ms    832 fps
//     1440x900     2.93 ms    342 fps
//     1600x1200    3.87 ms    258 fps
//     2880x1800    9.22 ms    108 fps
//     3840x2160   13.61 ms     73 fps   <- 82% of a 60 Hz budget
//
// ...and that is BEFORE the CGImage blit, which is another ~45% of present
// because it copies the whole surface on the CPU. A 5K display is already
// past the budget for a UI that does almost nothing. The cost is per-PIXEL
// and no amount of CPU tuning changes that — a GPU does the same work in
// microseconds because it has thousands of lanes instead of eight.
//
// The design is the one the whole framework was built around: every primitive
// is a rounded-box SDF on one instanced quad, so a frame is ONE draw call of
// N instances and the fragment shader is the same kernel `render/sdf.hpp`
// implements in C++. No tessellation, no per-shape pipelines, no state
// changes. The MSL lives in `render/shader_source.hpp` next to the C++ it
// mirrors, so the two cannot drift silently.
//
// Written against the Objective-C runtime rather than as an .mm file, for the
// same reason the Cocoa window is: mayag stays header-only and buildable with
// GCC, which has no Objective-C++ support and is the only compiler on macOS
// with C++26.

#include "../render/draw_list.hpp"
#include "../render/shader_source.hpp"
#include "backend.hpp"

#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace mayag::backend {

namespace mtl {

/// Typed objc_msgSend. Same reasoning as the Cocoa window: the variadic
/// declaration has the wrong ABI for anything but integer arguments on
/// arm64, so the cast to an exact signature is mandatory, not stylistic.
template <typename Ret, typename... Args>
inline Ret msg(id self, SEL op, Args... args) {
    using Fn = Ret (*)(id, SEL, Args...);
    return reinterpret_cast<Fn>(&objc_msgSend)(self, op, args...);
}
template <typename Ret, typename... Args>
inline Ret msg_cls(Class cls, SEL op, Args... args) {
    using Fn = Ret (*)(Class, SEL, Args...);
    return reinterpret_cast<Fn>(&objc_msgSend)(cls, op, args...);
}

inline SEL sel(const char* n) { return sel_registerName(n); }

inline id nsstring(const char* s) {
    return msg_cls<id>(objc_getClass("NSString"), sel("stringWithUTF8String:"), s);
}
inline id nsstring(const std::string& s) { return nsstring(s.c_str()); }

/// MTLPixelFormatBGRA8Unorm — what a CAMetalLayer uses by default.
inline constexpr std::uint64_t pixel_format_bgra8 = 80;

/// MTLResourceStorageModeShared: one allocation the CPU writes and the GPU
/// reads. On Apple silicon memory is unified, so there is no upload at all —
/// which is exactly why the instanced design is cheap here.
inline constexpr std::uint64_t storage_mode_shared = 0;

/// MTLLoadActionClear / MTLStoreActionStore.
inline constexpr std::uint64_t load_action_clear = 2;
inline constexpr std::uint64_t store_action_store = 1;

/// MTLPrimitiveTypeTriangleStrip.
inline constexpr std::uint64_t primitive_triangle_strip = 3;

}  // namespace mtl

/// A Metal rendering device bound to a CAMetalLayer.
///
/// Implements the same four-method `Device` interface as the software
/// backend, so selecting it changes nothing above the seam.
class MetalDevice {
  public:
    /// Attach to a view's layer. Returns false when Metal is unavailable —
    /// a VM, a headless session, an unsupported GPU — and the caller falls
    /// through to software.
    [[nodiscard]] bool attach(id view, float dpi_scale) {
        using namespace mtl;

        device_ = create_system_default_device();
        if (device_ == nullptr) return false;

        queue_ = msg<id>(device_, sel("newCommandQueue"));
        if (queue_ == nullptr) return false;

        // Swap the view's layer for a CAMetalLayer.
        //
        // This is what removes the CPU blit entirely: instead of rasterising
        // into a buffer, wrapping it in a CGImage and handing that to Core
        // Animation, the GPU renders directly into a drawable the compositor
        // already owns.
        layer_ = msg_cls<id>(objc_getClass("CAMetalLayer"), sel("layer"));
        if (layer_ == nullptr) return false;

        msg<void>(layer_, sel("setDevice:"), device_);
        msg<void>(layer_, sel("setPixelFormat:"), pixel_format_bgra8);
        msg<void>(layer_, sel("setFramebufferOnly:"), static_cast<BOOL>(YES));
        msg<void>(layer_, sel("setContentsScale:"), static_cast<double>(dpi_scale));

        // `presentsWithTransaction = NO` lets the drawable be presented
        // immediately rather than waiting for the next Core Animation
        // transaction — one of the two places a full refresh of latency
        // hides.
        msg<void>(layer_, sel("setPresentsWithTransaction:"), static_cast<BOOL>(NO));

        msg<void>(view, sel("setWantsLayer:"), static_cast<BOOL>(YES));
        msg<void>(view, sel("setLayer:"), layer_);

        if (!build_pipeline()) return false;

        dpi_ = dpi_scale;
        valid_ = true;
        return true;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    void resize(Vec2 logical, float dpi_scale) {
        if (!valid_) return;
        dpi_ = dpi_scale;
        struct { double w, h; } size{logical.x * dpi_scale, logical.y * dpi_scale};
        mtl::msg<void>(layer_, mtl::sel("setDrawableSize:"), size);
        mtl::msg<void>(layer_, mtl::sel("setContentsScale:"), static_cast<double>(dpi_scale));
        viewport_ = logical;
    }

    /// Render one frame. The whole draw list becomes ONE instanced draw call.
    void submit(const DrawList& list, Color<Srgb> clear) {
        using namespace mtl;
        if (!valid_ || list.empty()) { present_empty(clear); return; }

        id drawable = msg<id>(layer_, sel("nextDrawable"));
        if (drawable == nullptr) return;   // display disconnected mid-frame

        upload_instances(list);

        // ---- render pass ----
        id desc = msg_cls<id>(objc_getClass("MTLRenderPassDescriptor"),
                              sel("renderPassDescriptor"));
        id attachments = msg<id>(desc, sel("colorAttachments"));
        id color0 = msg<id>(attachments, sel("objectAtIndexedSubscript:"),
                            static_cast<std::uint64_t>(0));

        id texture = msg<id>(drawable, sel("texture"));
        msg<void>(color0, sel("setTexture:"), texture);
        msg<void>(color0, sel("setLoadAction:"), load_action_clear);
        msg<void>(color0, sel("setStoreAction:"), store_action_store);

        // Clear in LINEAR light, because the surface is sRGB-encoded on
        // write. Passing the sRGB value would double-encode the background.
        const auto lin = clear.to<Linear>();
        struct { double r, g, b, a; } c{lin.c0, lin.c1, lin.c2, 1.0};
        msg<void>(color0, sel("setClearColor:"), c);

        id cmd = msg<id>(queue_, sel("commandBuffer"));
        id enc = msg<id>(cmd, sel("renderCommandEncoderWithDescriptor:"), desc);

        msg<void>(enc, sel("setRenderPipelineState:"), pipeline_);
        msg<void>(enc, sel("setVertexBuffer:offset:atIndex:"),
                  instance_buffer_, static_cast<std::uint64_t>(0), static_cast<std::uint64_t>(0));

        struct { float x, y; } vp{viewport_.x * dpi_, viewport_.y * dpi_};
        msg<void>(enc, sel("setVertexBytes:length:atIndex:"),
                  &vp, sizeof(vp), static_cast<std::uint64_t>(1));

        if (atlas_texture_ != nullptr) {
            msg<void>(enc, sel("setFragmentTexture:atIndex:"),
                      atlas_texture_, static_cast<std::uint64_t>(0));
        }

        // ONE draw call. Four vertices, N instances — the entire frame.
        msg<void>(enc, sel("drawPrimitives:vertexStart:vertexCount:instanceCount:"),
                  primitive_triangle_strip,
                  static_cast<std::uint64_t>(0), static_cast<std::uint64_t>(4),
                  static_cast<std::uint64_t>(list.size()));

        msg<void>(enc, sel("endEncoding"));
        msg<void>(cmd, sel("presentDrawable:"), drawable);
        msg<void>(cmd, sel("commit"));

        last_draw_calls_ = 1;
        last_instances_  = static_cast<std::uint32_t>(list.size());
    }

    /// Upload the glyph atlas. Called when its generation changes.
    void upload_atlas(const std::uint8_t* pixels, int width, int height) {
        using namespace mtl;
        if (!valid_ || pixels == nullptr || width <= 0 || height <= 0) return;

        if (atlas_texture_ == nullptr || atlas_w_ != width || atlas_h_ != height) {
            id desc = msg_cls<id>(objc_getClass("MTLTextureDescriptor"),
                sel("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
                static_cast<std::uint64_t>(10),   // MTLPixelFormatR8Unorm
                static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height),
                static_cast<BOOL>(NO));
            atlas_texture_ = msg<id>(device_, sel("newTextureWithDescriptor:"), desc);
            atlas_w_ = width;
            atlas_h_ = height;
        }
        if (atlas_texture_ == nullptr) return;

        struct { std::uint64_t x, y, z, w, h, d; } region{
            0, 0, 0, static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height), 1};
        msg<void>(atlas_texture_, sel("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"),
                  region, static_cast<std::uint64_t>(0), pixels,
                  static_cast<std::uint64_t>(width));
    }

    [[nodiscard]] std::uint32_t last_instances() const noexcept { return last_instances_; }
    [[nodiscard]] std::uint32_t last_draw_calls() const noexcept { return last_draw_calls_; }
    [[nodiscard]] std::string adapter_name() const {
        if (device_ == nullptr) return "none";
        id name = mtl::msg<id>(device_, mtl::sel("name"));
        const char* utf8 = mtl::msg<const char*>(name, mtl::sel("UTF8String"));
        return utf8 != nullptr ? utf8 : "Metal";
    }

  private:
    [[nodiscard]] static id create_system_default_device() {
        // MTLCreateSystemDefaultDevice is a C function, not a class method.
        using Fn = id (*)();
        auto* fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice"));
        return fn != nullptr ? fn() : nullptr;
    }

    /// Compile the shader and build the pipeline state.
    ///
    /// The MSL is assembled from `render/shader_source.hpp`, which holds the
    /// same kernel the CPU rasteriser runs. Sharing the source is what keeps
    /// GPU and software output identical rather than merely similar.
    [[nodiscard]] bool build_pipeline() {
        using namespace mtl;

        const std::string source = std::string{render::shaders::msl_kernel} +
                                   std::string{render::shaders::msl_shaders};

        id err = nullptr;
        id library = msg<id>(device_, sel("newLibraryWithSource:options:error:"),
                             nsstring(source), static_cast<id>(nullptr), &err);
        if (library == nullptr) return false;

        id vfn = msg<id>(library, sel("newFunctionWithName:"), nsstring("mayag_vertex"));
        id ffn = msg<id>(library, sel("newFunctionWithName:"), nsstring("mayag_fragment"));
        if (vfn == nullptr || ffn == nullptr) return false;

        id desc = msg<id>(msg_cls<id>(objc_getClass("MTLRenderPipelineDescriptor"),
                                      sel("alloc")), sel("init"));
        msg<void>(desc, sel("setVertexFunction:"), vfn);
        msg<void>(desc, sel("setFragmentFunction:"), ffn);

        id attachments = msg<id>(desc, sel("colorAttachments"));
        id color0 = msg<id>(attachments, sel("objectAtIndexedSubscript:"),
                            static_cast<std::uint64_t>(0));
        msg<void>(color0, sel("setPixelFormat:"), pixel_format_bgra8);

        // Premultiplied source-over. The CPU already premultiplies every
        // instance colour, so the blend factors are (1, 1-srcA) and the GPU
        // does no per-pixel conversion at all.
        msg<void>(color0, sel("setBlendingEnabled:"), static_cast<BOOL>(YES));
        msg<void>(color0, sel("setSourceRGBBlendFactor:"), static_cast<std::uint64_t>(1));
        msg<void>(color0, sel("setSourceAlphaBlendFactor:"), static_cast<std::uint64_t>(1));
        msg<void>(color0, sel("setDestinationRGBBlendFactor:"), static_cast<std::uint64_t>(5));
        msg<void>(color0, sel("setDestinationAlphaBlendFactor:"), static_cast<std::uint64_t>(5));

        pipeline_ = msg<id>(device_, sel("newRenderPipelineStateWithDescriptor:error:"),
                            desc, &err);
        return pipeline_ != nullptr;
    }

    void upload_instances(const DrawList& list) {
        using namespace mtl;
        const std::size_t bytes = list.size() * sizeof(Instance);

        // Grow geometrically, so a frame that adds one instance does not
        // reallocate. Unified memory means "upload" is just a memcpy.
        if (instance_buffer_ == nullptr || bytes > instance_capacity_) {
            instance_capacity_ = num::max<std::size_t>(bytes * 2, 64 * 1024);
            instance_buffer_ = msg<id>(device_, sel("newBufferWithLength:options:"),
                                       static_cast<std::uint64_t>(instance_capacity_),
                                       storage_mode_shared);
        }
        if (instance_buffer_ == nullptr) return;

        void* dst = msg<void*>(instance_buffer_, sel("contents"));
        if (dst != nullptr) std::memcpy(dst, list.instances().data(), bytes);
    }

    void present_empty(Color<Srgb> clear) {
        using namespace mtl;
        if (!valid_) return;
        id drawable = msg<id>(layer_, sel("nextDrawable"));
        if (drawable == nullptr) return;

        id desc = msg_cls<id>(objc_getClass("MTLRenderPassDescriptor"), sel("renderPassDescriptor"));
        id color0 = msg<id>(msg<id>(desc, sel("colorAttachments")),
                            sel("objectAtIndexedSubscript:"), static_cast<std::uint64_t>(0));
        msg<void>(color0, sel("setTexture:"), msg<id>(drawable, sel("texture")));
        msg<void>(color0, sel("setLoadAction:"), load_action_clear);
        msg<void>(color0, sel("setStoreAction:"), store_action_store);

        const auto lin = clear.to<Linear>();
        struct { double r, g, b, a; } c{lin.c0, lin.c1, lin.c2, 1.0};
        msg<void>(color0, sel("setClearColor:"), c);

        id cmd = msg<id>(queue_, sel("commandBuffer"));
        id enc = msg<id>(cmd, sel("renderCommandEncoderWithDescriptor:"), desc);
        msg<void>(enc, sel("endEncoding"));
        msg<void>(cmd, sel("presentDrawable:"), drawable);
        msg<void>(cmd, sel("commit"));
    }

    id    device_ = nullptr;
    id    queue_ = nullptr;
    id    layer_ = nullptr;
    id    pipeline_ = nullptr;
    id    instance_buffer_ = nullptr;
    id    atlas_texture_ = nullptr;

    std::size_t   instance_capacity_ = 0;
    int           atlas_w_ = 0, atlas_h_ = 0;
    Vec2          viewport_{};
    float         dpi_ = 1.0f;
    bool          valid_ = false;
    std::uint32_t last_instances_ = 0;
    std::uint32_t last_draw_calls_ = 0;
};

}  // namespace mayag::backend
