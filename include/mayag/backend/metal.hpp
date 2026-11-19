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
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Blocks runtime (libSystem). Declared by hand: Apple's headers need the
// blocks extension, which GCC lacks.
extern "C" void*  _NSConcreteGlobalBlock[32];

#include <time.h>

namespace mayag::backend {
/// Host time in seconds: mach_absolute_time's clock, the one NSEvent
/// timestamps and -[MTLDrawable presentedTime] are expressed in.
[[nodiscard]] inline double host_seconds() noexcept {
    return static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) * 1e-9;
}
}  // namespace mayag::backend

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

// ── presentation feedback ────────────────────────────────────────────────
//
// `-[MTLDrawable addPresentedHandler:]` is the only way to learn when a frame
// actually reached the glass, and it takes an Objective-C BLOCK. GCC has no
// blocks, but the Block ABI is public and frozen (it is what clang emits):
// a header {isa, flags, reserved, invoke, descriptor} followed by captures.
// We build one by hand as a GLOBAL block (Block_copy returns it unchanged,
// Block_release is a no-op), and give it one capture: a pointer back to the
// sink. invoke() receives the block itself, so it can find that pointer.
//
// The handler runs on a Metal-owned thread. It only appends to a locked
// vector; the main thread drains it.
//
// A sink is IMMORTAL, deliberately: Metal Block_copy's the handler and
// Block_release's it after invoking, and _Block_release reads the block's
// flags word -- so the block's memory must outlive every handler Metal might
// still hold, including ones for frames queued when the window closed. One
// small object per window that ever attached Metal is the price; both its
// queues are capped, so a dead window's sink cannot grow.

/// What the caller wants echoed back for a frame. Host-clock seconds
/// (host_seconds(), the clock NSEvent timestamps and presentedTime use).
struct FrameTag {
    double input_s  = -1.0;   ///< newest input this frame shows; < 0 = none
    double target_s = 0.0;    ///< the vblank the frame was drawn to make; 0 = unknown
};

/// One frame's fate.
struct Presented {
    FrameTag tag;
    double   submit_s    = 0.0;   ///< when it was handed to Metal
    double   presented_s = 0.0;   ///< when it hit the display; 0 = dropped (replaced)
};

class PresentSink {
  public:
    static PresentSink* create() { return new PresentSink(); }   // never deleted, see above

    /// The block to hand to addPresentedHandler:.
    [[nodiscard]] id block() noexcept { return reinterpret_cast<id>(&block_); }

    void begin(unsigned long drawable_id, FrameTag tag, double submit_s) {
        std::lock_guard lock{m_};
        // A handler that never fires (a layer torn down mid-flight) must not
        // make this grow without bound.
        if (pending_.size() >= 64) pending_.erase(pending_.begin());
        pending_.push_back({drawable_id, Presented{tag, submit_s, 0.0}});
    }

    [[nodiscard]] std::vector<Presented> take() {
        std::lock_guard lock{m_};
        return std::exchange(done_, {});
    }

  private:
    struct Descriptor {
        unsigned long reserved;
        unsigned long size;
        const char*   signature;
    };
    struct Block {
        void*             isa;
        int               flags;
        int               reserved;
        void            (*invoke)(Block*, id);
        const Descriptor* descriptor;
        PresentSink*      sink;        // the one capture
    };
    static constexpr int kBlockIsGlobal     = 1 << 28;
    static constexpr int kBlockHasSignature = 1 << 30;
    static inline const Descriptor kDescriptor{0, sizeof(Block), "v16@?0@8"};

    PresentSink() {
        block_ = Block{&_NSConcreteGlobalBlock, kBlockIsGlobal | kBlockHasSignature, 0,
                       &invoke, &kDescriptor, this};
    }

    static void invoke(Block* b, id drawable) {
        PresentSink* s = b->sink;
        const double t   = mtl::msg<double>(drawable, mtl::sel("presentedTime"));
        const auto   did = mtl::msg<unsigned long>(drawable, mtl::sel("drawableID"));
        std::lock_guard lock{s->m_};
        for (auto it = s->pending_.begin(); it != s->pending_.end(); ++it) {
            if (it->first != did) continue;
            it->second.presented_s = t;
            if (s->done_.size() < 1024) s->done_.push_back(it->second);
            s->pending_.erase(it);
            break;
        }
    }

    Block                                              block_{};
    std::mutex                                         m_;
    std::vector<std::pair<unsigned long, Presented>>   pending_;
    std::vector<Presented>                             done_;
};

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

        // The other place: display sync. With it ON, every presented drawable
        // queues FIFO behind the ones before it, so a frame drawn late in a
        // refresh to carry the newest input waits behind the early one. OFF,
        // in a window, WindowServer simply composites whichever drawable is
        // newest at its next refresh -- mailbox semantics, no tearing (the
        // compositor owns scanout; only a direct-to-display fullscreen layer
        // could tear). The host paces draws to at most two per refresh, so
        // this never turns into an unthrottled render loop.
        // MAYAG_PRESENT=fifo restores the queued behaviour for comparison.
        const char* present = std::getenv("MAYAG_PRESENT");
        fifo_ = present != nullptr && std::string_view{present} == "fifo";
        msg<void>(layer_, sel("setDisplaySyncEnabled:"), static_cast<BOOL>(fifo_ ? YES : NO));

        // Every pixel is written (the pass clears to an opaque background), so
        // tell Core Animation: an opaque layer skips the blend in WindowServer's
        // composite, and is what makes a fullscreen window eligible for direct
        // scanout.
        msg<void>(layer_, sel("setOpaque:"), static_cast<BOOL>(YES));

        if (sink_ == nullptr) sink_ = PresentSink::create();

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
        struct { double w, h; } size{static_cast<double>(logical.x * dpi_scale), static_cast<double>(logical.y * dpi_scale)};
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
    void submit(const DrawList& list, Color<Srgb> clear, FrameTag tag = {}) {
        using namespace mtl;
        AutoreleasePool pool;
        if (!valid_ || list.empty()) { present_empty(clear, tag); return; }

        id drawable = msg<id>(layer_, sel("nextDrawable"));
        if (drawable == nullptr) return;   // display disconnected mid-frame
        track(drawable, tag);

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
        struct { double r, g, b, a; } c{static_cast<double>(lin.c0), static_cast<double>(lin.c1), static_cast<double>(lin.c2), 1.0};
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
        // The colour plane (emoji). Always bind something: an unbound slot
        // the shader declares samples zeros on some GPUs and faults on others.
        if (id ct = color_texture_ != nullptr ? color_texture_ : atlas_texture_; ct != nullptr) {
            msg<void>(enc, sel("setFragmentTexture:atIndex:"), ct, static_cast<std::uint64_t>(1));
        }
        if (sampler_ != nullptr) {
            msg<void>(enc, sel("setFragmentSamplerState:atIndex:"),
                      sampler_, static_cast<std::uint64_t>(0));
        }
        // Slot 2: the batch's image (image(id)). Bound per batch, only when
        // it changes; something is always bound (an empty slot faults).
        id fallback_image = color_texture_ != nullptr ? color_texture_ : atlas_texture_;
        id bound_image = fallback_image;
        if (bound_image != nullptr)
            msg<void>(enc, sel("setFragmentTexture:atIndex:"), bound_image, static_cast<std::uint64_t>(2));

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
            if (batch.texture != 0) {
                const auto img = images_.find(batch.texture);
                id want = img != images_.end() ? img->second.texture : fallback_image;
                if (want != nullptr && want != bound_image) {
                    msg<void>(enc, sel("setFragmentTexture:atIndex:"), want, static_cast<std::uint64_t>(2));
                    bound_image = want;
                }
            }

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
        if (present_drawable != nullptr) flush_core_animation();
        retire_slot(cmd);
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
    /// Image `key` (image(key) in a view): straight RGBA8, sRGB-encoded, as
    /// mayag's ImageData is. Re-uploading at the same size rewrites it in
    /// place; a new size makes a new texture (frames in flight keep the old
    /// one alive: command buffers retain what they sample).
    void upload_image(std::uint32_t key, const std::uint8_t* rgba, int w, int h) {
        using namespace mtl;
        if (device_ == nullptr || rgba == nullptr || w <= 0 || h <= 0 || key == 0) return;
        auto& img = images_[key];
        if (img.texture == nullptr || img.w != w || img.h != h) {
            if (img.texture != nullptr) msg<void>(img.texture, sel("release"));
            id desc = msg_cls<id>(objc_getClass("MTLTextureDescriptor"),
                sel("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
                static_cast<std::uint64_t>(71),   // MTLPixelFormatRGBA8Unorm_sRGB
                static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h), static_cast<BOOL>(NO));
            img.texture = msg<id>(device_, sel("newTextureWithDescriptor:"), desc);
            img.w = w;
            img.h = h;
        }
        if (img.texture == nullptr) return;
        struct { std::uint64_t x, y, z, w, h, d; } region{
            0, 0, 0, static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h), 1};
        msg<void>(img.texture, sel("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"),
                  region, static_cast<std::uint64_t>(0), rgba, static_cast<std::uint64_t>(w) * 4);
    }

    /// Rewrite rectangles (x, y, w, h) of image `key` (already w x h) from
    /// `packed`: each rect's rows tight, rect after rect. false: no such
    /// image at that size, or sizes that do not add up.
    bool update_image_regions(std::uint32_t key, int w, int h, const std::vector<std::array<int, 4>>& rects,
                              const std::vector<std::uint8_t>& packed) {
        using namespace mtl;
        const auto it = images_.find(key);
        if (it == images_.end() || it->second.texture == nullptr || it->second.w != w || it->second.h != h)
            return false;
        std::size_t need = 0;
        for (const auto& r : rects) {
            if (r[0] < 0 || r[1] < 0 || r[2] <= 0 || r[3] <= 0 || r[0] + r[2] > w || r[1] + r[3] > h) return false;
            need += static_cast<std::size_t>(r[2]) * static_cast<std::size_t>(r[3]) * 4;
        }
        if (need != packed.size()) return false;
        std::size_t at = 0;
        for (const auto& r : rects) {
            struct { std::uint64_t x, y, z, w, h, d; } region{
                static_cast<std::uint64_t>(r[0]), static_cast<std::uint64_t>(r[1]), 0,
                static_cast<std::uint64_t>(r[2]), static_cast<std::uint64_t>(r[3]), 1};
            msg<void>(it->second.texture, sel("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), region,
                      static_cast<std::uint64_t>(0), packed.data() + at, static_cast<std::uint64_t>(r[2]) * 4);
            at += static_cast<std::size_t>(r[2]) * static_cast<std::size_t>(r[3]) * 4;
        }
        return true;
    }

    /// Rows y..y+h (columns x..x+w) of image `key` take what was at row + dy,
    /// on the GPU: the pixels already there move, nothing crosses from the
    /// CPU. Through a scratch texture (a blit cannot read and write
    /// overlapping parts of one texture). Ordered on the queue ahead of the
    /// next frame's draw, and ahead of any replaceRegion after it (waits).
    bool move_image_rows(std::uint32_t key, int x, int y, int w, int h, int dy) {
        using namespace mtl;
        const auto it = images_.find(key);
        if (it == images_.end() || it->second.texture == nullptr || w <= 0 || h <= 0) return false;
        const int iw = it->second.w, ih = it->second.h;
        if (x < 0 || y < 0 || x + w > iw || y + h > ih || y + dy < 0 || y + h + dy > ih) return false;
        if (scratch_ == nullptr || scratch_w_ < w || scratch_h_ < h) {
            if (scratch_ != nullptr) msg<void>(scratch_, sel("release"));
            id desc = msg_cls<id>(objc_getClass("MTLTextureDescriptor"),
                                  sel("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
                                  static_cast<std::uint64_t>(71), static_cast<std::uint64_t>(std::max(w, iw)),
                                  static_cast<std::uint64_t>(std::max(h, ih)), static_cast<BOOL>(NO));
            msg<void>(desc, sel("setStorageMode:"), static_cast<std::uint64_t>(2));  // private: GPU only
            scratch_ = msg<id>(device_, sel("newTextureWithDescriptor:"), desc);
            if (scratch_ == nullptr) return false;
            scratch_w_ = std::max(w, iw);
            scratch_h_ = std::max(h, ih);
        }
        struct Origin { std::uint64_t x, y, z; };
        struct Size { std::uint64_t w, h, d; };
        const Size size{static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h), 1};
        id cmd = msg<id>(queue_, sel("commandBuffer"));
        id blit = msg<id>(cmd, sel("blitCommandEncoder"));
        const SEL copy = sel("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:"
                             "toTexture:destinationSlice:destinationLevel:destinationOrigin:");
        msg<void>(blit, copy, it->second.texture, std::uint64_t{0}, std::uint64_t{0},
                  Origin{static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y + dy), 0}, size, scratch_,
                  std::uint64_t{0}, std::uint64_t{0}, Origin{0, 0, 0});
        msg<void>(blit, copy, scratch_, std::uint64_t{0}, std::uint64_t{0}, Origin{0, 0, 0}, size, it->second.texture,
                  std::uint64_t{0}, std::uint64_t{0},
                  Origin{static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y), 0});
        msg<void>(blit, sel("endEncoding"));
        msg<void>(cmd, sel("commit"));
        // replaceRegion (the tiles after this move) is a CPU write, not on
        // the queue: the move must be done before it lands.
        msg<void>(cmd, sel("waitUntilCompleted"));
        return true;
    }

    void release_image(std::uint32_t key) {
        using namespace mtl;
        const auto it = images_.find(key);
        if (it == images_.end()) return;
        if (it->second.texture != nullptr) msg<void>(it->second.texture, sel("release"));
        images_.erase(it);
    }

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
        using namespace mtl;
        if (device_ == nullptr) return;

        const bool realloc = !atlas_uploaded_ ||
                             atlas.generation() != atlas_generation_ ||
                             atlas.width() != atlas_w_ || atlas.height() != atlas_h_;

        if (realloc) {
            // A new generation means glyphs MOVED. Writing them into the
            // texture the previous frame is still sampling on the GPU (CPU
            // replaceRegion is not ordered against in-flight command
            // buffers) shows that frame with half-moved glyphs: flicker.
            // So a new generation gets a NEW texture; the old one lives until
            // the command buffers that reference it complete (they retain it).
            if (atlas_texture_ != nullptr && atlas_uploaded_) {
                msg<void>(atlas_texture_, sel("release"));
                atlas_texture_ = nullptr;
            }
            upload_atlas(atlas.pixels().data(), atlas.width(), atlas.height());
            atlas_generation_ = atlas.generation();
            atlas_uploaded_   = true;
            atlas.clear_dirty();
            sync_color(atlas, /*full=*/true);
            return;
        }

        const Rect& d = atlas.dirty_region();
        if (!d.empty()) {
            upload_region(atlas.pixels().data(), atlas.width(), d);
            atlas.clear_dirty();
        }
        sync_color(atlas, /*full=*/false);
    }

    /// The RGBA colour plane, mirroring the coverage plane: a new texture per
    /// atlas generation (glyphs moved), dirty sub-rect uploads otherwise.
    template <typename AtlasT>
    void sync_color(AtlasT& atlas, bool full) {
        using namespace mtl;
        if (!atlas.has_color()) return;
        const int w = atlas.width(), h = atlas.height();
        if (color_texture_ == nullptr || color_w_ != w || color_h_ != h || full) {
            if (color_texture_ != nullptr) msg<void>(color_texture_, sel("release"));
            id desc = msg_cls<id>(objc_getClass("MTLTextureDescriptor"),
                sel("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
                static_cast<std::uint64_t>(71),   // MTLPixelFormatRGBA8Unorm_sRGB: emoji are sRGB images; decode to linear on sample
                static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h), static_cast<BOOL>(NO));
            color_texture_ = msg<id>(device_, sel("newTextureWithDescriptor:"), desc);
            color_w_ = w;
            color_h_ = h;
            if (color_texture_ == nullptr) return;
            struct { std::uint64_t x, y, z, w, h, d; } region{0, 0, 0, static_cast<std::uint64_t>(w),
                                                               static_cast<std::uint64_t>(h), 1};
            msg<void>(color_texture_, sel("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), region,
                      static_cast<std::uint64_t>(0), atlas.color_pixels().data(), static_cast<std::uint64_t>(w) * 4);
            atlas.clear_color_dirty();
            return;
        }
        const Rect& r = atlas.color_dirty_region();
        if (r.empty()) return;
        const int x = std::max(0, static_cast<int>(r.left())), y = std::max(0, static_cast<int>(r.top()));
        const int rw = std::min(w - x, static_cast<int>(std::ceil(r.right())) - x);
        const int rh = std::min(h - y, static_cast<int>(std::ceil(r.bottom())) - y);
        if (rw > 0 && rh > 0) {
            struct { std::uint64_t x, y, z, w, h, d; } region{static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y), 0,
                                                               static_cast<std::uint64_t>(rw), static_cast<std::uint64_t>(rh), 1};
            const std::uint8_t* origin = atlas.color_pixels().data() + (static_cast<std::size_t>(y) * w + x) * 4;
            msg<void>(color_texture_, sel("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), region,
                      static_cast<std::uint64_t>(0), origin, static_cast<std::uint64_t>(w) * 4);
        }
        atlas.clear_color_dirty();
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

        const std::string body = std::string{render::shaders::msl_kernel} +
                                 std::string{render::shaders::msl_shaders};

        // Framebuffer fetch ([[color(0)]]) lets glyphs blend exactly as
        // CoreGraphics does (encoded-space lerp). Apple-family GPUs have it;
        // anything else (or a compile failure) uses the coverage-reshaping
        // approximation. MAYAG_METAL_NO_FB_FETCH forces the latter so it can
        // be tested on Apple hardware.
        const bool apple_gpu = msg<BOOL>(device_, sel("supportsFamily:"), static_cast<std::int64_t>(1001)) != NO &&
                               std::getenv("MAYAG_METAL_NO_FB_FETCH") == nullptr;
        id err = nullptr;
        id library = nullptr;
        if (apple_gpu)
            library = msg<id>(device_, sel("newLibraryWithSource:options:error:"),
                              nsstring("#define MG_FB_FETCH 1\n" + body), static_cast<id>(nullptr), &err);
        if (library == nullptr)
            library = msg<id>(device_, sel("newLibraryWithSource:options:error:"),
                              nsstring("#define MG_FB_FETCH 0\n" + body), static_cast<id>(nullptr), &err);
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

        // A RING of instance buffers, one per frame that may be in flight.
        //
        // With one buffer, the CPU's memcpy for frame N+1 overwrote the
        // instances the GPU was still reading for frame N whenever the CPU
        // ran ahead (mailbox presentation, no vsync wait: it does, by
        // design). Glyphs were drawn with the next frame's positions and
        // atlas uvs: text flickered, worst exactly when it was changing.
        // Now each frame writes its own buffer, and before reusing one the
        // CPU waits for the command buffer that last read it. With three
        // slots that wait almost never blocks.
        slot_ = (slot_ + 1) % ring_size;
        Slot& s = ring_[slot_];
        if (s.last_cmd != nullptr) {
            // MTLCommandBufferStatusCompleted = 4, Error = 5
            if (msg<std::uint64_t>(s.last_cmd, sel("status")) < 4)
                msg<void>(s.last_cmd, sel("waitUntilCompleted"));
            msg<void>(s.last_cmd, sel("release"));
            s.last_cmd = nullptr;
        }
        // Grow geometrically, so a frame that adds one instance does not
        // reallocate. Unified memory means "upload" is just a memcpy.
        if (s.buffer == nullptr || bytes > s.capacity) {
            if (s.buffer != nullptr) msg<void>(s.buffer, sel("release"));
            s.capacity = num::max<std::size_t>(bytes * 2, 64 * 1024);
            s.buffer = msg<id>(device_, sel("newBufferWithLength:options:"),
                               static_cast<std::uint64_t>(s.capacity), storage_mode_shared);
        }
        instance_buffer_ = s.buffer;
        instance_capacity_ = s.capacity;
        if (instance_buffer_ == nullptr) return;

        void* dst = msg<void*>(instance_buffer_, sel("contents"));
        if (dst != nullptr) std::memcpy(dst, list.instances().data(), bytes);
    }

    /// Remember which command buffer reads the current ring slot.
    void retire_slot(id cmd) {
        using namespace mtl;
        ring_[slot_].last_cmd = msg<id>(cmd, sel("retain"));
    }

    static constexpr std::size_t ring_size = 3;
    struct Slot { id buffer = nullptr; std::size_t capacity = 0; id last_cmd = nullptr; };
    std::array<Slot, ring_size> ring_{};
    std::size_t slot_ = 0;

  public:
    /// Frames that reached the display (or were replaced) since the last call.
    [[nodiscard]] std::vector<Presented> take_presented() {
        return sink_ != nullptr ? sink_->take() : std::vector<Presented>{};
    }

    [[nodiscard]] bool fifo() const noexcept { return fifo_; }

  private:
    void present_empty(Color<Srgb> clear, FrameTag tag = {}) {
        using namespace mtl;
        if (!valid_) return;
        id drawable = msg<id>(layer_, sel("nextDrawable"));
        if (drawable == nullptr) return;
        track(drawable, tag);

        id desc = msg_cls<id>(objc_getClass("MTLRenderPassDescriptor"), sel("renderPassDescriptor"));
        id color0 = msg<id>(msg<id>(desc, sel("colorAttachments")),
                            sel("objectAtIndexedSubscript:"), static_cast<std::uint64_t>(0));
        msg<void>(color0, sel("setTexture:"), msg<id>(drawable, sel("texture")));
        msg<void>(color0, sel("setLoadAction:"), load_action_clear);
        msg<void>(color0, sel("setStoreAction:"), store_action_store);

        const auto lin = clear.to<Linear>();
        struct { double r, g, b, a; } c{static_cast<double>(lin.c0), static_cast<double>(lin.c1), static_cast<double>(lin.c2), 1.0};
        msg<void>(color0, sel("setClearColor:"), c);

        id cmd = msg<id>(queue_, sel("commandBuffer"));
        id enc = msg<id>(cmd, sel("renderCommandEncoderWithDescriptor:"), desc);
        msg<void>(enc, sel("endEncoding"));
        msg<void>(cmd, sel("presentDrawable:"), drawable);
        msg<void>(cmd, sel("commit"));
        flush_core_animation();
    }

    /// Hand the presented drawable to the window server now. Core Animation
    /// commits layer changes from an AppKit run-loop observer; a program on
    /// its own loop (jaal's kqueue) only spins AppKit when it polls for
    /// input, so without this a frame drawn while idle (a remote window
    /// updating by itself) stayed off screen until the next key or mouse
    /// move.
    static void flush_core_animation() {
        mtl::msg_cls<void>(objc_getClass("CATransaction"), mtl::sel("flush"));
    }

    id    device_ = nullptr;
    id    queue_ = nullptr;
    id    layer_ = nullptr;
    id    pipeline_ = nullptr;
    id    sampler_ = nullptr;
    id    instance_buffer_ = nullptr;
    id    atlas_texture_ = nullptr;
    id    color_texture_ = nullptr;
    int   color_w_ = 0, color_h_ = 0;

    std::size_t   instance_capacity_ = 0;
    int           atlas_w_ = 0, atlas_h_ = 0;
    struct Image {
        id  texture = nullptr;
        int w = 0, h = 0;
    };
    std::map<std::uint32_t, Image> images_;  // image(id) textures
    id            scratch_ = nullptr;          // move_image_rows' staging texture
    int           scratch_w_ = 0, scratch_h_ = 0;
    std::uint32_t atlas_generation_ = 0;
    bool          atlas_uploaded_ = false;
    Vec2          viewport_{};
    float         dpi_ = 1.0f;
    bool          valid_ = false;
    bool          fifo_ = false;
    std::uint32_t last_instances_ = 0;
    std::uint32_t last_draw_calls_ = 0;
    PresentSink*  sink_ = nullptr;   // immortal; copies of the device share it

    /// Ask to be told when this drawable reaches the display. Must happen
    /// before presentDrawable:/commit.
    void track(id drawable, FrameTag tag) {
        if (sink_ == nullptr) return;
        const auto did = mtl::msg<unsigned long>(drawable, mtl::sel("drawableID"));
        sink_->begin(did, tag, host_seconds());
        mtl::msg<void>(drawable, mtl::sel("addPresentedHandler:"), sink_->block());
    }
};

}  // namespace mayag::backend
