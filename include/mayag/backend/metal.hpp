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

// Public runtime entry points, but not declared by <objc/objc.h> on every SDK.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void  objc_autoreleasePoolPop(void*);

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <optional>
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

/// MTLPixelFormatBGRA8Unorm_sRGB.
///
/// The _sRGB variant, not the plain one, and this is not cosmetic. Every
/// instance colour that reaches a backend is LINEAR premultiplied — that is
/// the contract `render/draw_list.hpp` sets and the software rasteriser
/// honours by running `encode_parallel` (a linear->sRGB transfer) over the
/// framebuffer on the way out.
///
/// A GPU surface has no such pass. With plain BGRA8Unorm the shader's linear
/// output is stored as if it were already encoded, and every colour comes out
/// far too dark: mid-grey 0.216 linear displays as 0.216 sRGB, which is 50%
/// grey rendered as 25% grey. Asking for the _sRGB format makes the hardware
/// do the encode on write and the decode on read, so blending still happens
/// in linear light exactly like the software path — the two backends agree
/// instead of merely resembling each other.
inline constexpr std::uint64_t pixel_format_bgra8_srgb = 81;

/// MTLResourceStorageModeShared: one allocation the CPU writes and the GPU
/// reads. On Apple silicon memory is unified, so there is no upload at all —
/// which is exactly why the instanced design is cheap here.
inline constexpr std::uint64_t storage_mode_shared = 0;

/// MTLLoadActionClear / MTLStoreActionStore.
inline constexpr std::uint64_t load_action_clear = 2;
inline constexpr std::uint64_t store_action_store = 1;

/// MTLPrimitiveTypeTriangleStrip.
///
/// FOUR, not three — three is MTLPrimitiveTypeTriangle. The enum runs
/// point, line, lineStrip, triangle, triangleStrip, so an off-by-one here is
/// both easy to write and nearly invisible: the draw call succeeds, Metal
/// reports no error, and every quad renders as the FIRST HALF of itself.
/// A rectangle comes out as the triangle through its top-left, top-right and
/// bottom-left corners, which at a glance still looks like "something drew".
/// Caught by diffing GPU output against the software rasteriser: the shape
/// was in the right place with the right colour and only 7739 of its 16000
/// pixels.
inline constexpr std::uint64_t primitive_triangle_strip = 4;

/// MTLSamplerMinMagFilterLinear / MTLSamplerAddressModeClampToEdge.
inline constexpr std::uint64_t filter_linear = 1;
inline constexpr std::uint64_t address_clamp_to_edge = 0;

/// A scoped autorelease pool.
///
/// Not optional, and its absence is invisible until it is fatal. Almost every
/// object a frame touches — the drawable, the command buffer, the render pass
/// descriptor — is returned AUTORELEASED. With no pool on the stack there is
/// nothing to drain them into, so a 60 fps app accumulates 180 live objects a
/// second, including drawables the display cannot reclaim. The window's event
/// loop drives the run loop by hand, so it never crosses AppKit's own pool
/// boundary either: this has to be here.
struct AutoreleasePool {
    AutoreleasePool() : token_{objc_autoreleasePoolPush()} {}
    ~AutoreleasePool() { objc_autoreleasePoolPop(token_); }
    AutoreleasePool(const AutoreleasePool&) = delete;
    AutoreleasePool& operator=(const AutoreleasePool&) = delete;
    void* token_;
};

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
        msg<void>(layer_, sel("setPixelFormat:"), pixel_format_bgra8_srgb);
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
        if (!build_sampler()) return false;

        dpi_ = dpi_scale;
        valid_ = true;
        return true;
    }

    /// Bring up a device with NO window, for offscreen rendering.
    ///
    /// `attach` needs a view because it swaps in a CAMetalLayer; a
    /// correctness test needs neither. Splitting them lets CI verify the GPU
    /// output on a headless machine, which is the only place the check will
    /// actually be run often enough to catch a regression.
    [[nodiscard]] bool init_offscreen() {
        using namespace mtl;
        device_ = create_system_default_device();
        if (device_ == nullptr) return false;
        queue_ = msg<id>(device_, sel("newCommandQueue"));
        if (queue_ == nullptr) return false;
        if (!build_pipeline()) return false;
        if (!build_sampler()) return false;
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

    /// Render one frame: one draw call per batch, each with its own scissor.
    ///
    /// The earlier version issued a single draw for the whole list and never
    /// set a scissor rect. That is wrong in a way nothing catches: a scroll
    /// viewport's clip lives in `Batch::clip`, so ignoring it lets list rows
    /// paint straight over the header and off the window. A typical frame is
    /// still 1-3 batches, so honouring the clip costs a draw call, not a pass.
    void submit(const DrawList& list, Color<Srgb> clear) {
        using namespace mtl;
        AutoreleasePool pool;
        if (!valid_ || list.empty()) { present_empty(clear); return; }

        id drawable = msg<id>(layer_, sel("nextDrawable"));
        if (drawable == nullptr) return;   // display disconnected mid-frame

        id texture = msg<id>(drawable, sel("texture"));
        encode_pass(texture, list, clear, viewport_.x * dpi_, viewport_.y * dpi_,
                    drawable, /*wait=*/false);
    }

    /// Encode and submit one render pass. Shared by the window path and the
    /// offscreen path so the thing CI verifies is the thing the window runs —
    /// a correctness test against a parallel implementation proves nothing.
    void encode_pass(id target, const DrawList& list, Color<Srgb> clear,
                     float vw, float vh, id present_drawable, bool wait) {
        using namespace mtl;

        upload_instances(list);

        id desc = msg_cls<id>(objc_getClass("MTLRenderPassDescriptor"),
                              sel("renderPassDescriptor"));
        id attachments = msg<id>(desc, sel("colorAttachments"));
        id color0 = msg<id>(attachments, sel("objectAtIndexedSubscript:"),
                            static_cast<std::uint64_t>(0));

        msg<void>(color0, sel("setTexture:"), target);
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

        struct { float x, y; } vp{vw, vh};
        msg<void>(enc, sel("setVertexBytes:length:atIndex:"),
                  &vp, sizeof(vp), static_cast<std::uint64_t>(1));

        if (atlas_texture_ != nullptr) {
            msg<void>(enc, sel("setFragmentTexture:atIndex:"),
                      atlas_texture_, static_cast<std::uint64_t>(0));
        }
        if (sampler_ != nullptr) {
            msg<void>(enc, sel("setFragmentSamplerState:atIndex:"),
                      sampler_, static_cast<std::uint64_t>(0));
        }

        std::uint32_t draws = 0;
        for (const Batch& batch : list.batches()) {
            if (batch.count == 0) continue;

            // Metal validates scissor rects against the drawable and aborts
            // the process on a violation, so clamping is a hard requirement
            // rather than defensive tidiness: the draw list's default clip is
            // an infinite rect, and an unbounded one resizes to garbage.
            const auto sc = clamp_scissor(batch.clip, vw, vh);
            if (!sc) continue;    // fully offscreen — nothing to draw
            msg<void>(enc, sel("setScissorRect:"), *sc);

            msg<void>(enc, sel("drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:"),
                      primitive_triangle_strip,
                      static_cast<std::uint64_t>(0), static_cast<std::uint64_t>(4),
                      static_cast<std::uint64_t>(batch.count),
                      static_cast<std::uint64_t>(batch.first));
            ++draws;
        }

        msg<void>(enc, sel("endEncoding"));
        if (present_drawable != nullptr) {
            msg<void>(cmd, sel("presentDrawable:"), present_drawable);
        }
        msg<void>(cmd, sel("commit"));
        if (wait) msg<void>(cmd, sel("waitUntilCompleted"));

        last_draw_calls_ = draws;
        last_instances_  = static_cast<std::uint32_t>(list.size());
    }

    /// Upload the glyph atlas. Called when its generation changes.
    ///
    /// Guarded on the DEVICE, not on `valid_`. `valid_` means "attached to a
    /// window", and creating a texture needs no window at all — gating this
    /// on it made every offscreen render silently skip the upload, so text
    /// came out blank while boxes and circles were perfect. Resource
    /// ownership follows the device; only presentation follows the layer.
    void upload_atlas(const std::uint8_t* pixels, int width, int height) {
        using namespace mtl;
        if (device_ == nullptr || pixels == nullptr || width <= 0 || height <= 0) return;

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

    /// Upload only what changed since the last frame.
    ///
    /// Re-uploading a 1024x1024 atlas every frame is 1 MB of pointless
    /// traffic; text mostly does not change. Two signals make that cheap:
    /// `generation()` bumps on reallocation (upload everything), and
    /// `dirty_region()` reports the rect newly rasterised glyphs touched
    /// (upload just that). Both come from the atlas, so this stays correct
    /// without the backend tracking glyph identity.
    ///
    /// Templated on the atlas type so the GPU backend keeps no dependency on
    /// the font engine — backends see pixels, never typefaces.
    template <typename AtlasT>
    void sync_atlas(AtlasT& atlas) {
        if (device_ == nullptr) return;

        const bool realloc = !atlas_uploaded_ ||
                             atlas.generation() != atlas_generation_ ||
                             atlas.width() != atlas_w_ || atlas.height() != atlas_h_;

        if (realloc) {
            upload_atlas(atlas.pixels().data(), atlas.width(), atlas.height());
            atlas_generation_ = atlas.generation();
            atlas_uploaded_   = true;
            atlas.clear_dirty();
            return;
        }

        const Rect& d = atlas.dirty_region();
        if (d.empty()) return;
        upload_region(atlas.pixels().data(), atlas.width(), d);
        atlas.clear_dirty();
    }

    /// Upload a sub-rectangle of an existing atlas texture.
    void upload_region(const std::uint8_t* pixels, int atlas_width, const Rect& r) {
        using namespace mtl;
        if (device_ == nullptr || atlas_texture_ == nullptr || pixels == nullptr) return;

        const int x = std::max(0, static_cast<int>(r.left()));
        const int y = std::max(0, static_cast<int>(r.top()));
        const int w = std::min(static_cast<int>(r.width()),  atlas_w_ - x);
        const int h = std::min(static_cast<int>(r.height()), atlas_h_ - y);
        if (w <= 0 || h <= 0) return;

        // The source is a row of the FULL atlas, so bytesPerRow is the atlas
        // width even though we upload a narrow slice — Metal strides through
        // the original buffer rather than needing a packed copy.
        const std::uint8_t* origin =
            pixels + static_cast<std::size_t>(y) * atlas_width + x;

        struct { std::uint64_t x, y, z, w, h, d; } region{
            static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y), 0,
            static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h), 1};
        msg<void>(atlas_texture_, sel("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"),
                  region, static_cast<std::uint64_t>(0), origin,
                  static_cast<std::uint64_t>(atlas_width));
    }

    /// Render a draw list into an offscreen texture and read it back as
    /// RGBA8 — the same format `Framebuffer::to_rgba8` produces.
    ///
    /// This exists to make the GPU path FALSIFIABLE. "Metal costs 0.04 ms of
    /// CPU" is also exactly what a backend that draws nothing costs, and a
    /// benchmark cannot tell those apart. Rendering offscreen and diffing
    /// against the software rasteriser can: if the two agree pixel-for-pixel
    /// within antialiasing tolerance, the GPU is genuinely doing the work.
    ///
    /// It needs no window and no drawable, so CI can run it headless.
    [[nodiscard]] std::vector<std::uint8_t>
    render_offscreen(const DrawList& list, Color<Srgb> clear, int w, int h) {
        using namespace mtl;
        AutoreleasePool pool;
        if (device_ == nullptr || w <= 0 || h <= 0) return {};

        // MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead.
        constexpr std::uint64_t usage_render_target = 4, usage_shader_read = 1;

        id desc = msg_cls<id>(objc_getClass("MTLTextureDescriptor"),
            sel("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
            pixel_format_bgra8_srgb,
            static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h),
            static_cast<BOOL>(NO));
        if (desc == nullptr) return {};
        msg<void>(desc, sel("setUsage:"), usage_render_target | usage_shader_read);
        // Managed/shared storage, so the CPU can read the result back.
        msg<void>(desc, sel("setStorageMode:"), static_cast<std::uint64_t>(0));

        id target = msg<id>(device_, sel("newTextureWithDescriptor:"), desc);
        if (target == nullptr) return {};

        // The viewport the vertex shader divides by must match this texture,
        // not the window — otherwise every position is scaled wrong.
        const Vec2 saved_vp = viewport_;
        const float saved_dpi = dpi_;
        viewport_ = Vec2{static_cast<float>(w), static_cast<float>(h)};
        dpi_ = 1.0f;

        encode_pass(target, list, clear, static_cast<float>(w), static_cast<float>(h),
                    /*present=*/nullptr, /*wait=*/true);

        viewport_ = saved_vp;
        dpi_ = saved_dpi;

        // Read back and swizzle BGRA -> RGBA.
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * h * 4);
        struct { std::uint64_t x, y, z, w, h, d; } region{
            0, 0, 0, static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h), 1};
        msg<void>(target, sel("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
                  bgra.data(), static_cast<std::uint64_t>(w) * 4, region,
                  static_cast<std::uint64_t>(0));
        msg<void>(target, sel("release"));

        std::vector<std::uint8_t> rgba(bgra.size());
        for (std::size_t i = 0; i < bgra.size(); i += 4) {
            rgba[i + 0] = bgra[i + 2];
            rgba[i + 1] = bgra[i + 1];
            rgba[i + 2] = bgra[i + 0];
            rgba[i + 3] = bgra[i + 3];
        }
        return rgba;
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
    /// Clamp a batch's clip rect to a valid MTLScissorRect, or nothing when
    /// it lies entirely offscreen.
    ///
    /// Three separate hazards, all fatal rather than cosmetic: the draw
    /// list's default clip is an INFINITE rect (float -> uint64 conversion is
    /// undefined), a scroll container can push a clip whose origin is
    /// negative, and Metal terminates the process when a scissor exceeds the
    /// attachment. Clamping into the drawable handles all three.
    struct ScissorRect { std::uint64_t x, y, w, h; };

    [[nodiscard]] static std::optional<ScissorRect>
    clamp_scissor(const Rect& clip, float vw, float vh) {
        const float x0 = std::max(clip.left(),   0.0f);
        const float y0 = std::max(clip.top(),    0.0f);
        const float x1 = std::min(clip.right(),  vw);
        const float y1 = std::min(clip.bottom(), vh);
        if (!(x1 > x0) || !(y1 > y0)) return std::nullopt;

        return ScissorRect{static_cast<std::uint64_t>(x0),
                           static_cast<std::uint64_t>(y0),
                           static_cast<std::uint64_t>(x1 - x0),
                           static_cast<std::uint64_t>(y1 - y0)};
    }

    /// The glyph sampler.
    ///
    /// The fragment shader takes `sampler smp [[sampler(0)]]` and nothing was
    /// ever binding one — text would have sampled with undefined state.
    /// Linear filtering is what the software path's bilinear `Atlas::sample`
    /// does, and clamp-to-edge stops a glyph at the atlas border bleeding in
    /// its neighbour from the opposite side.
    [[nodiscard]] bool build_sampler() {
        using namespace mtl;
        id desc = msg<id>(msg_cls<id>(objc_getClass("MTLSamplerDescriptor"),
                                      sel("alloc")), sel("init"));
        if (desc == nullptr) return false;
        msg<void>(desc, sel("setMinFilter:"), filter_linear);
        msg<void>(desc, sel("setMagFilter:"), filter_linear);
        msg<void>(desc, sel("setSAddressMode:"), address_clamp_to_edge);
        msg<void>(desc, sel("setTAddressMode:"), address_clamp_to_edge);
        sampler_ = msg<id>(device_, sel("newSamplerStateWithDescriptor:"), desc);
        msg<void>(desc, sel("release"));
        return sampler_ != nullptr;
    }

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
        msg<void>(color0, sel("setPixelFormat:"), pixel_format_bgra8_srgb);

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
    id    sampler_ = nullptr;
    id    instance_buffer_ = nullptr;
    id    atlas_texture_ = nullptr;

    std::size_t   instance_capacity_ = 0;
    int           atlas_w_ = 0, atlas_h_ = 0;
    std::uint32_t atlas_generation_ = 0;
    bool          atlas_uploaded_ = false;
    Vec2          viewport_{};
    float         dpi_ = 1.0f;
    bool          valid_ = false;
    std::uint32_t last_instances_ = 0;
    std::uint32_t last_draw_calls_ = 0;
};

}  // namespace mayag::backend
