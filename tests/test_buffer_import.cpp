// Zero-copy buffer import: the substrate a compositor is built on.
//
// A compositor never owns its pixels. A client hands over a dmabuf fd and the
// whole value of the thing is that those bytes are never copied and never
// touched by the CPU. This file tests the contract that makes that possible,
// and especially the parts that are easy to get quietly wrong:
//
//   * the Buffer variant models foreign memory without owning it
//   * import() reports WHY it failed, so a compositor can tell "renegotiate
//     with the client" from "retry later"
//   * the fd is BORROWED: import must not close the caller's descriptor
//   * a device without the extensions degrades honestly instead of crashing
//
// The fd-ownership test is the one that matters most. Getting it wrong means
// closing a descriptor a client still owns, which surfaces much later as a
// client whose next frame is silently corrupt -- the kind of bug that takes
// days to trace back here.

#include <mayag/mayag.hpp>
#include <mayag/backend/tiled.hpp>
#include <mayag/backend/reclaim.hpp>
#include <mayag/render/buffer.hpp>
#include <mayag/render/sync.hpp>

#include <cstdio>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

using namespace mayag;

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %-4s  %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}
void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

/// A real fd to hand around. /dev/null is not a dmabuf, so no driver will
/// import it -- which is exactly what makes it a good probe for the ERROR
/// paths and the ownership contract.
[[nodiscard]] int scratch_fd() { return ::open("/dev/null", O_RDONLY); }

[[nodiscard]] bool fd_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

int main() {
    std::printf("buffer import\n");

    // ── the Buffer variant ───────────────────────────────────────────
    section("Buffer models where pixels live, without owning foreign memory");
    {
        auto img = std::make_shared<ImageData>();
        img->width = 4; img->height = 4;
        img->rgba.assign(4 * 4 * 4, 0xff);
        const Buffer cpu = Buffer::from(img);

        check(cpu.valid(), "a CPU buffer wrapping an ImageData is valid");
        check(!cpu.is_foreign(), "and is not foreign");
        check(cpu.width == 4 && cpu.height == 4, "carrying its dimensions");

        Dmabuf d{};
        d.n_planes = 1;
        d.planes[0] = DmabufPlane{7, 0, 4096};
        d.format = formats::argb8888;
        d.modifier = Modifier::linear;
        const Buffer foreign{d, 1920, 1080};

        check(foreign.valid(), "a dmabuf buffer is valid");
        check(foreign.is_foreign(), "and IS foreign -- mayag does not own it");
        check(sizeof(Dmabuf) < 128,
              "a dmabuf descriptor is small enough to pass by value (" +
                  std::to_string(sizeof(Dmabuf)) + " bytes)");

        // Nothing in the variant owns an fd, so copying a Buffer cannot
        // double-close. That is a property of the type, not of discipline.
        const Buffer copy = foreign;
        check(std::get<Dmabuf>(copy.storage).planes[0].fd == 7,
              "copying a Buffer copies the descriptor, not the memory");
    }

    // ── formats ──────────────────────────────────────────────────────
    section("DRM formats and modifiers");
    {
        check(formats::argb8888 == fourcc('A', 'R', '2', '4'),
              "ARGB8888 matches drm_fourcc.h's DRM_FORMAT_ARGB8888");
        check(formats::rgba8888 == formats::abgr8888,
              "what mayag calls RGBA8 is DRM's ABGR8888 (byte order, not word "
              "order)");
        check(static_cast<std::uint64_t>(Modifier::linear) == 0,
              "DRM_FORMAT_MOD_LINEAR is 0");
        check(static_cast<std::uint64_t>(Modifier::invalid) == 0x00ffffffffffffffULL,
              "DRM_FORMAT_MOD_INVALID matches the header");

        const FormatModifier a{formats::argb8888, Modifier::linear, false};
        const FormatModifier b{formats::argb8888, Modifier::linear, false};
        check(a == b, "format+modifier pairs compare by value");
    }

    // ── a device that cannot import says so ──────────────────────────
    section("a device without the extensions degrades honestly");
    {
        // The software rasteriser has no GPU and cannot import anything. It
        // must SAY that rather than crash or pretend.
        auto dev = backend::SoftwareBackend::create({}, Vec2{64, 64}, 1.0f);
        backend::Device& sw = *dev;

        Dmabuf d{};
        d.n_planes = 1;
        d.planes[0] = DmabufPlane{scratch_fd(), 0, 4096};
        d.format = formats::argb8888;
        const Buffer buf{d, 64, 64};

        const auto r = sw.import(buf);
        check(!r.has_value(), "import fails on a device with no GPU");
        check(r.error() == ImportError::not_supported,
              "and reports not_supported, not a generic failure");
        check(sw.importable_formats().empty(),
              "advertising no importable formats, so no client allocates a "
              "buffer it cannot use");

        // THE OWNERSHIP CONTRACT. The fd is borrowed for the call; a failed
        // import must leave it exactly as it found it.
        check(fd_open(d.planes[0].fd),
              "a failed import does NOT close the caller's fd");
        ::close(d.planes[0].fd);
    }

    // ── error reporting is actionable ────────────────────────────────
    section("import failures are distinguishable");
    {
        auto dev = backend::SoftwareBackend::create({}, Vec2{64, 64}, 1.0f);
        backend::Device& sw = *dev;

        const Buffer empty{};
        const auto r1 = sw.import(empty);
        check(!r1.has_value() && r1.error() == ImportError::invalid_buffer,
              "an empty buffer is invalid_buffer, not unsupported_format");

        Dmabuf d{};
        d.n_planes = 1;
        d.planes[0] = DmabufPlane{-1, 0, 0};      // no fd
        const Buffer bad{d, 64, 64};
        check(!bad.valid(), "a dmabuf with no fd is not valid");

        // These are different because the RESPONSES differ: unsupported_format
        // means renegotiate with the client, out_of_memory means retry, and
        // invalid_buffer means the client sent nonsense and should be told so.
        check(ImportError::unsupported_format != ImportError::out_of_memory &&
              ImportError::out_of_memory != ImportError::invalid_buffer,
              "the error cases a compositor must react to differently are "
              "distinct values");
    }

    // ── CPU buffers are not silently copied by import() ───────────────
    section("import() never hides a copy");
    {
        auto dev = backend::SoftwareBackend::create({}, Vec2{64, 64}, 1.0f);
        backend::Device& sw = *dev;
        auto img = std::make_shared<ImageData>();
        img->width = 2; img->height = 2;
        img->rgba.assign(2 * 2 * 4, 0x80);

        const auto r = sw.import(Buffer::from(img));
        check(!r.has_value(),
              "importing CPU memory fails rather than copying behind a name "
              "that promises not to");
    }

    // ── deferred reclamation ──────────────────────────────────
    //
    // Destroying a GPU resource safely used to mean DeviceWaitIdle: stop the
    // whole GPU to answer a question about one image. A compositor destroys
    // buffers constantly -- every window that closes or resizes -- so that
    // stall is felt directly as jank.
    //
    // Retirement is the same move mayag already made for source teardown on
    // the CPU side: don't block, set the thing aside and collect it once it
    // is genuinely safe. The clock lives OUTSIDE the reclaimer so this is
    // pure bookkeeping and testable with no GPU at all.
    section("resources are freed when their frame completes, not by stalling");
    {
        backend::Reclaimer r;
        int freed = 0;

        // Retired while rendering frame 5: not safe until 5 completes.
        r.retire(5, [&] { ++freed; });
        check(r.pending() == 1, "a retired resource is held, not freed");

        r.collect(4);
        check(freed == 0,
              "and is NOT freed while its frame is still in flight");

        r.collect(5);
        check(freed == 1, "it is freed once that frame completes");
        check(r.pending() == 0, "and dropped from the pending list");

        // Out-of-order retirement must still be safe: a later frame's
        // resource cannot be freed by an earlier collect.
        int a = 0, b = 0;
        r.retire(10, [&] { ++a; });
        r.retire(7,  [&] { ++b; });
        r.collect(8);
        check(b == 1 && a == 0,
              "collect frees only what is due, whatever order it was retired "
              "in");
        r.collect(10);
        check(a == 1, "and the rest once their frames land");

        // Teardown: the device really is idle, so everything goes.
        int late = 0;
        r.retire(9999, [&] { ++late; });
        check(late == 0, "a far-future resource is not freed by collect()");
        r.collect_all();
        check(late == 1,
              "collect_all frees it -- the one case where the device is known "
              "idle");
        check(r.pending() == 0, "leaving nothing behind at teardown");
    }

    // ── sync points travel WITH the buffer ─────────────────────────
    //
    // Same principle as the runtime's message gating: readiness is a property
    // of the thing being used, checked at the point of use -- not a global
    // "is everything done?" asked from somewhere else.
    section("BufferSync describes readiness per buffer");
    {
        const BufferSync implicit{};
        check(implicit.is_implicit(),
              "a default BufferSync means implicit sync -- what mayag does "
              "today, so this type is additive");
        check(!implicit.acquire.has_value(),
              "no acquire point means the pixels are ready NOW");
        check(!implicit.release.has_value(),
              "no release point means nobody wants to be told we finished");

        BufferSync explicit_sync;
        explicit_sync.acquire = TimelinePoint{7, 42};
        check(!explicit_sync.is_implicit(),
              "an acquire point makes it explicit");
        check(explicit_sync.acquire->valid(),
              "and the point carries a real syncobj fd");
        check(explicit_sync.acquire->value == 42,
              "plus the timeline value to wait for");

        const TimelinePoint none{};
        check(!none.valid(),
              "a default timeline point is invalid, so it cannot be waited on "
              "by accident");

        const SyncCaps caps{};
        check(!caps.timeline && !caps.import_syncobj,
              "a device advertises no sync capability until it proves one -- "
              "promising explicit sync and falling back is worse than never "
              "offering it");
    }

    // ── acquire: import if you can, copy if you must ───────────────
    //
    // Found by writing the frame loop. import() is the fast path and fails
    // ROUTINELY -- a software renderer, a modifier the driver refuses, a
    // wl_shm client that never had a dmabuf. Every one of those still has to
    // appear on screen.
    //
    // Before this the fallback was not awkward but impossible: upload_image
    // takes a shared_ptr<const ImageData>, which a Dmabuf or Shm buffer does
    // not have, so a failed import left a layer with no route to the GPU and
    // it silently vanished from the frame.
    section("every buffer reaches the GPU somehow, and says what it cost");
    {
        auto dev = backend::SoftwareBackend::create({}, Vec2{64, 64}, 1.0f);
        backend::Device& sw = *dev;

        auto img = std::make_shared<ImageData>();
        img->width = 8; img->height = 8;
        img->rgba.assign(8 * 8 * 4, 0xAA);

        // This device cannot import at all, so acquire must fall back.
        const auto a = sw.acquire(Buffer::from(img));
        check(a.has_value(),
              "a CPU buffer is acquired even on a device that cannot import");
        check(a->handle.valid(), "and comes back with a usable texture");
        check(a->copied,
              "reporting that it COST A COPY -- the difference between a "
              "compositor that idles and one that burns a core");

        // Ids must not collide. A compositor holds one texture per client
        // surface; two subsystems inventing ids by hand overwrite each
        // other's pixels silently.
        const auto b = sw.acquire(Buffer::from(img));
        check(b.has_value() && b->handle.slot != a->handle.slot,
              "a second acquire gets a DIFFERENT slot, so surfaces cannot "
              "overwrite each other");
        check(a->handle.slot != 0 && b->handle.slot != 0,
              "and never slot 0, which is the glyph atlas");

        // A foreign buffer that cannot be imported cannot be copied either --
        // it would have to be mapped first, which is a performance cliff the
        // caller must choose, not something to do silently inside acquire().
        Dmabuf d{};
        d.n_planes = 1;
        d.planes[0] = DmabufPlane{scratch_fd(), 0, 4096};
        d.format = formats::argb8888;
        const Buffer foreign{d, 64, 64};
        const auto f = sw.acquire(foreign);
        check(!f.has_value(),
              "a dmabuf this device cannot import fails rather than being "
              "mapped and copied behind the caller's back");
        check(fd_open(d.planes[0].fd),
              "and still does not close the caller's fd");
        ::close(d.planes[0].fd);

        const auto bad = sw.acquire(Buffer{});
        check(!bad.has_value() && bad.error() == ImportError::invalid_buffer,
              "an empty buffer is still invalid_buffer");
    }

    // ── wl_shm: the most common client in the world ────────────────
    //
    // A wl_shm client sends a pool fd, an offset, a stride and a DRM format.
    // None of those match what a renderer wants: the format is usually
    // ARGB8888 where mayag needs RGBA8, and the stride is padded to whatever
    // the toolkit's allocator liked.
    //
    // If mayag cannot take that, EVERY protocol implementation writes channel
    // swizzling and stride handling, and one of them gets the byte order
    // wrong and ships a desktop with red and blue swapped.
    section("a wl_shm buffer is usable without the caller touching pixels");
    {
        auto dev = backend::SoftwareBackend::create({}, Vec2{64, 64}, 1.0f);
        backend::Device& sw = *dev;

        // ARGB8888 with stride PADDING, which real toolkits emit constantly.
        const int w = 4, h = 4;
        const std::uint32_t stride = 32;          // 16 tight, padded to 32
        std::vector<std::uint8_t> pool(stride * h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                std::uint8_t* p = pool.data() + y * stride + x * 4;
                p[0] = 0x10; p[1] = 0x20; p[2] = 0x30; p[3] = 0xff;  // B G R A
            }

        Shm shm;
        shm.pixels = std::span<const std::uint8_t>(pool.data(), pool.size());
        shm.stride = stride;
        shm.format = formats::argb8888;
        const Buffer buf{shm, w, h};

        check(buf.valid(), "a wl_shm buffer is a valid Buffer");
        const auto a = sw.acquire(buf);
        check(a.has_value(),
              "and acquire takes it -- no protocol code writes a conversion");
        check(a->copied, "reporting that shm always costs a copy");

        // The conversion itself, checked directly. DRM names channels in
        // little-endian WORD order, so ARGB8888 is B,G,R,A in MEMORY, while
        // ImageData is R,G,B,A. They are reverses, and confusing them is the
        // classic "why is everything teal" compositor bug.
        const auto rgba = to_rgba8(shm, w, h);
        check(rgba != nullptr, "ARGB8888 converts");
        check(rgba->rgba[0] == 0x30 && rgba->rgba[1] == 0x20 &&
              rgba->rgba[2] == 0x10 && rgba->rgba[3] == 0xff,
              "with R and B in the right places -- DRM word order is the "
              "reverse of memory order");
        check(rgba->rgba.size() == static_cast<std::size_t>(w) * h * 4,
              "and the stride padding removed (" +
                  std::to_string(rgba->rgba.size()) + " bytes, tight)");

        // An X format has no alpha: the fourth byte is padding and may hold
        // anything. Reading it as alpha is why some clients render invisible.
        std::vector<std::uint8_t> xpool(stride * h, 0x00);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                std::uint8_t* p = xpool.data() + y * stride + x * 4;
                p[0] = 0x11; p[1] = 0x22; p[2] = 0x33; p[3] = 0x00;  // pad = 0
            }
        Shm xshm;
        xshm.pixels = std::span<const std::uint8_t>(xpool.data(), xpool.size());
        xshm.stride = stride;
        xshm.format = formats::xrgb8888;
        const auto xrgba = to_rgba8(xshm, w, h);
        check(xrgba != nullptr && xrgba->rgba[3] == 0xff,
              "an X format is forced OPAQUE -- its padding byte is not alpha, "
              "and reading it as one renders the client invisible");

        // A client that lies about its buffer size must be refused, not read
        // out of bounds.
        Shm liar = shm;
        liar.pixels = std::span<const std::uint8_t>(pool.data(), 8);
        check(to_rgba8(liar, w, h) == nullptr,
              "a buffer too small for its stated size is refused rather than "
              "read past its end");

        Shm unknown = shm;
        unknown.format = fourcc('N', 'V', '1', '2');
        check(to_rgba8(unknown, w, h) == nullptr,
              "an unconvertible format returns null, so the client is told "
              "rather than shown garbage");
    }

    // ── YUV: what every video client actually sends ────────────────
    //
    // A media player or screen-capture client hands over NV12, not RGB. This
    // belongs in mayag by its own rule -- pixel format handling, wrong in a
    // way that shows as wrong colours -- while the DRM backend that receives
    // it does not.
    section("planar YUV converts, with the right colour range");
    {
        const int w = 4, h = 4;
        // Mid-grey in BT.601 LIMITED range: Y=126 is the middle of 16..235,
        // chroma centred at 128 means no colour.
        std::vector<std::uint8_t> yp(static_cast<std::size_t>(w) * h, 126);
        std::vector<std::uint8_t> uv(static_cast<std::size_t>(w / 2) * (h / 2) * 2, 128);

        const YuvPlane nv12_planes[] = {
            {yp.data(), static_cast<std::uint32_t>(w)},
            {uv.data(), static_cast<std::uint32_t>(w)},
        };
        const auto rgb = yuv_to_rgba8(formats::nv12, nv12_planes, w, h);
        check(rgb != nullptr, "NV12 converts");
        check(rgb->rgba[0] == rgb->rgba[1] && rgb->rgba[1] == rgb->rgba[2],
              "chroma at 128 is neutral grey, with no colour cast");
        check(rgb->rgba[3] == 0xff, "and video is opaque");

        // The range check that matters. Y=16 is BLACK in limited range, not
        // dark grey -- treating limited as full washes blacks out, which is
        // the most common colour bug in video pipelines and invisible unless
        // you compare two players side by side.
        std::vector<std::uint8_t> black(static_cast<std::size_t>(w) * h, 16);
        const YuvPlane black_planes[] = {
            {black.data(), static_cast<std::uint32_t>(w)},
            {uv.data(), static_cast<std::uint32_t>(w)},
        };
        const auto dark = yuv_to_rgba8(formats::nv12, black_planes, w, h);
        check(dark != nullptr && dark->rgba[0] == 0,
              "Y=16 is BLACK, because the range is limited (16..235) not full");

        // And Y=235 is white, not merely bright.
        std::vector<std::uint8_t> white(static_cast<std::size_t>(w) * h, 235);
        const YuvPlane white_planes[] = {
            {white.data(), static_cast<std::uint32_t>(w)},
            {uv.data(), static_cast<std::uint32_t>(w)},
        };
        const auto bright = yuv_to_rgba8(formats::nv12, white_planes, w, h);
        check(bright != nullptr && bright->rgba[0] == 255,
              "and Y=235 is white, so the full range is used");

        // Three-plane I420, where U and V are separate rather than
        // interleaved -- the layout is part of the format.
        std::vector<std::uint8_t> up(static_cast<std::size_t>(w / 2) * (h / 2), 128);
        std::vector<std::uint8_t> vp(static_cast<std::size_t>(w / 2) * (h / 2), 128);
        const YuvPlane i420_planes[] = {
            {yp.data(), static_cast<std::uint32_t>(w)},
            {up.data(), static_cast<std::uint32_t>(w / 2)},
            {vp.data(), static_cast<std::uint32_t>(w / 2)},
        };
        const auto i420 = yuv_to_rgba8(formats::yuv420, i420_planes, w, h);
        check(i420 != nullptr, "YUV420 converts through its three planes");
        check(i420->rgba[0] == rgb->rgba[0],
              "agreeing with NV12 for the same colour, so the plane layout is "
              "handled rather than guessed");

        // Too few planes must be refused, not read out of bounds.
        const YuvPlane one[] = {{yp.data(), static_cast<std::uint32_t>(w)}};
        check(yuv_to_rgba8(formats::nv12, one, w, h) == nullptr,
              "a buffer with too few planes is refused rather than read past "
              "its end");
        check(yuv_to_rgba8(formats::argb8888, nv12_planes, w, h) == nullptr,
              "and an RGB format is not mistaken for planar");
    }

    // ── the threading contract ───────────────────────────────────
    //
    // A Device belongs to ONE thread, like update() does. The two queries a
    // compositor makes from a client connection -- which is NOT the render
    // thread -- are the exceptions, and they are safe because they read state
    // fixed at device creation.
    //
    // This was a real bug: importable_formats() built its cache lazily, so
    // two clients binding zwp_linux_dmabuf_v1 at once would both fill it. It
    // is const and looks like a pure getter, which is exactly why nothing
    // warned. Now the list is built once at creation and only read after.
    section("queries a compositor makes off-thread are immutable");
    {
        auto dev = backend::SoftwareBackend::create({}, Vec2{64, 64}, 1.0f);
        backend::Device& sw = *dev;

        // Reading these from many threads must be safe by construction, not
        // by luck. TSan on the concurrency suite is what proves it; this
        // asserts the values are stable, which is the observable half.
        const auto a = sw.importable_formats();
        const auto b = sw.importable_formats();
        check(a.data() == b.data() && a.size() == b.size(),
              "importable_formats returns the SAME storage every call -- "
              "nothing is built on demand");

        const auto c1 = sw.sync_caps();
        const auto c2 = sw.sync_caps();
        check(c1.timeline == c2.timeline &&
              c1.import_syncobj == c2.import_syncobj,
              "and sync_caps is fixed at creation, so a client connection can "
              "ask without hopping to the render thread");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
