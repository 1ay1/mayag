# Becoming a substrate: what mayag needs before a compositor can sit on top

The goal is not "support the Wayland protocol". mayag's rendering, layout and
event abstractions are already general. The question is whether they are
general enough that a **compositor** — wlroots, or a replacement — can be
implemented *on* mayag rather than beside it.

Today the answer is no, and the reason is precise and fixable. It is not the
protocol surface. It is one assumption baked into the pixel path.

---

## The one blocking assumption

Every pixel in mayag is `ImageData`:

```cpp
struct ImageData {
    std::vector<std::uint8_t> rgba;   // OWNED, CPU-side, tightly packed RGBA8
    int width, height;
};
```

and the only route to the GPU is:

```cpp
virtual std::uint32_t upload_texture(std::span<const std::uint8_t> rgba, ...);
```

So mayag's contract with a pixel producer is: **hand me bytes I own, in RGBA8,
in system memory.** For an app rendering its own UI that is exactly right —
simple, backend-agnostic, hard to misuse.

For a compositor it is fatal. A compositor never owns its pixels. A client
hands it a `dmabuf`: a GPU buffer, in a tiled vendor-specific format, carrying
DRM format modifiers, which the client still owns and may reuse. The entire
value of a modern compositor is that those bytes are **never copied and never
touched by the CPU**.

| Path | Cost per 1080p frame |
|---|---|
| mayag today | ~8 MB copy + format convert + staging upload (measured 3.27 ms convert, 2.8 ms copy) |
| compositor | 0 bytes — import an fd, sample in place |
| best case | 0 bytes, no GPU work — hand the fd to a KMS plane, scan out directly |

A compositor built on today's `upload_texture` would round-trip every client's
frame through system memory. That is not "slower than wlroots"; it is
categorically the wrong shape, and no optimisation inside `upload_texture`
fixes it.

**This is the whole problem.** Everything below follows from it.

---

## What generalises the model

Stop saying "pixels are bytes I own"; say "pixels are a *handle* whose bytes
may live anywhere". `ImageData` becomes one case of a `Buffer`:

```cpp
/// Where a buffer's pixels actually live.
struct Buffer {
    struct Cpu {                        // what ImageData is today
        std::vector<std::uint8_t> rgba;
    };
    struct Dmabuf {                     // a client's GPU buffer, BORROWED
        std::array<int, 4>           fds;         // one per plane
        std::array<std::uint32_t, 4> offsets, strides;
        std::uint64_t                modifier;    // DRM format modifier
        std::uint32_t                fourcc;      // DRM_FORMAT_*
        int                          n_planes;
    };
    struct Shm {                        // wl_shm: mapped, not owned
        std::span<const std::uint8_t> pixels;
        std::uint32_t stride, fourcc;
    };
    std::variant<Cpu, Dmabuf, Shm> storage;
    int width, height;
};
```

and `Device` gains the operation that makes zero-copy expressible:

```cpp
/// Import a buffer WITHOUT copying. Returns nullopt when the format or
/// modifier is unsupported, so the caller can fall back to a copy — which is
/// what makes this safe to adopt incrementally.
virtual std::optional<TextureHandle> import(const Buffer&) = 0;

/// Formats + modifiers this device can import. A compositor advertises these
/// via zwp_linux_dmabuf_v1 so clients allocate buffers the GPU can use.
virtual std::span<const FormatModifier> importable_formats() const = 0;
```

On Vulkan that is `VK_KHR_external_memory_fd` +
`VK_EXT_image_drm_format_modifier` — both widely supported, roughly 500 lines
against the existing device. Nothing about the DrawList, scene graph, layout
or event model changes: a `texture` instance still names a handle. Only the
*origin* of the pixels behind it becomes plural.

---

## The rest, in dependency order

Sizes are honest estimates, against wlroots' ~60k lines.

### 1. Buffer import — ~500 lines — **DONE** (`4bdcfea`)
The unblocker. `render/buffer.hpp` defines `Buffer{Cpu|Dmabuf|Shm}`;
`Device::import()` returns `expected<TextureHandle, ImportError>`. Verified
against the real NVIDIA driver: 28 format/modifier pairs advertised.

Two things only running it could find. The instance requested Vulkan 1.0, so
`vkGetPhysicalDeviceFormatProperties2` — 1.1 core — resolved to null and the
modifier list came back empty, silently degrading zero-copy to "unsupported"
on hardware that supports it. And Vulkan *closes* the fd it is handed, so
`import` must `dup()` first or it closes a descriptor the client still owns.

### 2. Explicit synchronisation — ~400 lines — **DONE** (`612d1e4`)
`backend/reclaim.hpp` retires a resource tagged with the current frame and
frees it once that frame completes, replacing three per-resource
`DeviceWaitIdle` calls. The completion clock is `submit()`'s existing
in-flight fence; keeping it *outside* the Reclaimer means the whole thing is
pure bookkeeping and unit-testable with no GPU.

`render/sync.hpp` adds `TimelinePoint`, `BufferSync{acquire, release}`,
`SyncCaps`. A default `BufferSync` means implicit sync, so the type is
additive.

### 3. Output abstraction — **CONCEPT DONE** (`41e164f`), DRM backend pending
`output/output.hpp` defines the concept, with `commit()` returning a
`CommitResult` because an atomic commit legitimately fails.
`output/planes.hpp` decides which layers can be scanned out directly, ranking
by area since a plane saves exactly what it covers. `HeadlessOutput` proves
the concept has more than one implementation and makes the refusal path
testable via `fail_next()`.

What remains is the DRM/KMS implementation itself — libdrm + GBM + libseat,
connector enumeration, atomic commits, page flips. ~2500 lines.

### 4. Hardware planes / direct scanout — ~800 lines — **planner done**
The policy lives in `planes.hpp` and is tested. What is missing is the half
that needs hardware: asking DRM for the real plane count and capabilities,
and handing buffers to `drmModeAtomicCommit`.

### 5. Input: libinput — ~900 lines
Reading `/dev/input/event*`: pointer acceleration, tablets, touch, switches,
plus seat/session handover so VT-switching works. mayag's `Event` type is
already a good target; the work is the device layer beneath it.

### 6. Protocols — ~3000 lines, growing forever
`xdg-shell`, `wl_seat`, `wl_output`, `linux-dmabuf`, `presentation-time`,
`viewporter`, `fractional-scale`, `ext-session-lock`, `cursor-shape`,
`tearing-control`, XWayland.

**This belongs in a separate library.** It is where wlroots' API churn comes
from — 0.17 → 0.18 → 0.19 each broke compositors — because protocol code and
rendering abstractions have completely different rates of change. Holding both
in one library is wlroots' central design mistake and the one most worth not
repeating.

---

## What "better than wlroots" would actually mean

Being faster is not a strategy; wlroots is not slow. The defensible claims, in
order of achievability:

**1. A stable core.** Distributions ship `wlroots017`, `wlroots018` and
`wlroots019` side by side because the API breaks every release. The cause is
structural. Splitting `mayag` (rendering, scene, output, input — stable) from
`mayag-wayland` (protocols — versioned independently) is a real advantage that
costs nothing but discipline.

**2. Scene-graph-driven plane assignment.** Direct scanout as the default
rather than an afterthought, because the scene graph already knows which quads
qualify. This is where measurable power and latency wins are.

**3. One renderer, many outputs.** The DrawList already abstracts
Vulkan/Metal/software, so a compositor gets a software fallback and headless
rendering for free. wlroots needed a renderer per backend for years.

**4. Testability.** The headless backend and deterministic clock mean a
compositor could have real unit tests, which the ecosystem largely lacks. One
honest caveat, learned the hard way: headless renders in **software** and
cannot catch GPU resource-lifetime bugs — the fullscreen hang was a freed
`VkImage` still being sampled, invisible to every headless test. Making this
claim real needs a validation-layer CI job.

---

## Honest scope

- **Blocking**: buffer import + explicit sync, ~900 lines. Until these exist,
  "compositor" is not something mayag can express.
- **Minimum viable compositor**: + DRM/KMS output + libinput + xdg-shell.
  Realistically 8–10k lines on top of mayag's current 30k.
- **wlroots parity**: ~60k lines and years — and parity is the wrong target;
  the protocol tail is endless and mostly uninteresting.

The strategic read: mayag's rendering, scene and event abstractions really are
general enough already. What is missing is not breadth of features but **one
type change** — from "pixels are bytes I own" to "pixels are a handle to
memory that may be foreign" — and the synchronisation model that follows.

Do those two and a compositor becomes buildable on mayag. Skip them and no
amount of protocol code will help.
