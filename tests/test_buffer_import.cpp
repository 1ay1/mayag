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
#include <mayag/render/buffer.hpp>

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

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
