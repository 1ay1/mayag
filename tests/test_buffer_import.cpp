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

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
