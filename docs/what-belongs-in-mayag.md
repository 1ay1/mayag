# What belongs in mayag, and what does not

mayag has an unusual property worth protecting: **it has no build-time
dependencies.** Vulkan, Wayland and libxkbcommon are all resolved with
`dlopen()` at runtime, so a mayag binary built anywhere runs anywhere, and
falls back rather than failing when something is absent.

Every compositor-substrate addition so far has kept that. The whole of
`render/buffer.hpp`, `render/sync.hpp`, `backend/reclaim.hpp` and `output/`
includes nothing but the standard library.

That is not an accident, and it is the test for what comes next.

---

## The rule

**mayag owns pixels and the decisions about them. It does not own devices.**

A thing belongs in mayag when:

1. it is a question about **pixels, geometry or GPU resources**, and
2. getting it wrong is a **rendering** bug — a seam, a stall, a corrupted
   frame, a wasted watt, and
3. it can be **decided** without opening a device node.

A thing belongs elsewhere when it needs an fd from the kernel, a session, or
a protocol connection.

### Why "decided" is the operative word

The distinction that matters is not layering for its own sake — it is that
*deciding* and *doing* have completely different testability.

`plan_planes()` decides which layers can be scanned out. It is pure: layers
in, assignments out. It has a right answer, that answer is subtle (rank by
area, be conservative about blending), and it can be exhaustively tested with
no hardware at all.

`drmModeAtomicCommit()` does the deed. It needs `/dev/dri/card0`, a DRM
master lease, and a real display. It has no interesting logic — it packs a
struct and calls an ioctl.

Putting the *decision* in mayag and the *ioctl* outside gives the compositor
project the hard, subtle, testable part for free, and leaves it the part that
is only a matter of correctly calling a kernel API.

---

## Where each piece landed, and why

| Piece | In mayag? | Because |
|---|---|---|
| `Buffer{Cpu,Dmabuf,Shm}` | **yes** | describes pixels; an fd is an `int` here, never opened |
| `Device::import/acquire` | **yes** | GPU resource question, decided by the driver mayag already loads |
| `to_rgba8()` | **yes** | pixel format conversion is the definition of graphics work |
| `Reclaimer` | **yes** | GPU resource lifetime; pure bookkeeping, no device |
| `TimelinePoint`, `wait_timeline` | **yes** | GPU synchronisation, through the Vulkan mayag already dlopens |
| `Layer`, `Transform`, `Region` | **yes** | geometry and opacity — pixel decisions |
| `plan_planes()` | **yes** | the subtle, testable decision |
| `frame_damage()`, `occlude()` | **yes** | pure geometry with a right answer |
| `Output` **concept** | **yes** | the vocabulary a display speaks |
| **DRM/KMS backend** | **NO** | opens `/dev/dri/*`, needs libdrm + GBM + libseat |
| **libinput** | **NO** | opens `/dev/input/*`, needs a session |
| **Wayland protocols** | **NO** | a wire protocol, not a picture |
| **XWayland** | **NO** | process management |

The `Output` concept is in mayag; every *implementation* of it that touches
hardware is not. `HeadlessOutput` lives here because it is a test double with
no dependencies, and because a concept with one implementation is not a
concept.

---

## What this means for the compositor project

It gets a library that has already decided everything hard about pixels:

```cpp
// The compositor project writes this -- protocol and ioctls:
Buffer buf = buffer_from_wl_resource(client_buffer);   // protocol
auto tex   = device.acquire(buf);                      // mayag decides

std::vector<Layer> layers = surfaces_to_layers();      // protocol
auto plan   = plan_planes(layers, caps);               // mayag decides
auto damage = occlude(frame_damage(layers, prev),
                      layers);                          // mayag decides

drmModeAtomicCommit(fd, req, flags, nullptr);          // compositor does
```

Everything on the "mayag decides" side has a right answer, is unit-tested,
and is wrong in ways that produce seams and stalls. Everything on the other
side is a matter of calling a kernel or protocol API correctly.

### The one thing that must NOT drift

mayag must never gain a build-time dependency on libdrm, GBM, libseat or
libinput. The moment it does:

- a mayag app -- an ordinary GUI program -- starts requiring compositor
  libraries it will never call
- the headless test path, which is what makes any of this testable, acquires
  a hardware dependency
- the "builds anywhere, runs anywhere" property is gone, and it does not come
  back

If a future piece seems to need one, that is the signal it belongs on the
other side of the seam.

---

## The remaining three, judged by the rule

**DRM/KMS backend** — outside. It implements `Output`, which mayag defines.
mayag already gave it the plane planner and damage tracking, which is the
part with a right answer.

**libinput** — outside. mayag's `Event` type is the target; producing events
from `/dev/input/*` is device work.

**YUV sampling** — *inside*. This one is genuinely mayag's: it is pixel format
handling, it goes through the Vulkan mayag already loads
(`sampler_ycbcr_conversion`), and getting it wrong shows as wrong colours.
It is the only one of the three that passes the rule.
