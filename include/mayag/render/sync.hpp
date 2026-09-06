#pragma once

/// Synchronisation: knowing when a buffer is ready, and when we are done.
///
/// A compositor cannot use the model mayag was built with. `DeviceWaitIdle`
/// answers "is the GPU finished?" — every queue, every submission, all work
/// in flight — when the actual question is always narrower: "is THIS image
/// safe to destroy?", "has THIS client finished drawing?". Standing a global
/// answer in for a specific one is correct and ruinous, exactly the way a
/// mutex around a whole subsystem is.
///
/// This is the same mistake mayag already fixed once, on the CPU side. A
/// worker used to check a liveness flag and then post, and no amount of
/// checking closed the window between the two, because validity was being
/// asked about at the wrong moment. The fix was to make the gate travel WITH
/// the message and be evaluated at the point of use.
///
/// The GPU version is identical in shape. Readiness travels WITH the buffer:
///
///     acquire   the client signals this when its drawing is done, so mayag
///               must WAIT on it before sampling
///     release   mayag signals this when it has finished reading, so the
///               client knows when its buffer is reusable
///
/// Both are timeline points, not fences, because a timeline value can be
/// waited on before it is signalled — which is what lets the compositor
/// submit work for a frame the client has not finished yet, and is the whole
/// reason explicit sync exists.
///
/// The word "explicit" is the point: with implicit sync the driver guesses
/// the dependency from which resources a submission touches, and guesses
/// conservatively. That conservatism is why compositors stutter.

#include <cstdint>
#include <optional>

namespace mayag {

/// A point on a synchronisation timeline: a DRM syncobj plus a value.
///
/// "The work identified by `value` on timeline `handle` is complete." A
/// timeline only ever moves forward, so waiting on a point that has already
/// passed returns immediately and waiting on a future one blocks until it
/// arrives — no ordering rules for the caller to get wrong.
///
/// This is deliberately not a fence. A fence answers "has this finished?"
/// about work that must already have been submitted; a timeline point can be
/// named before the work exists. A compositor needs the latter, because it
/// builds a frame from buffers whose producers are still running.
struct TimelinePoint {
    /// A DRM syncobj fd, borrowed exactly like a dmabuf fd: valid for the
    /// duration of the call it is passed to, and dup()ed by anything that
    /// needs to keep it. See render/buffer.hpp for why one call is the only
    /// borrow contract that cannot be got wrong by accident.
    int           fd    = -1;
    std::uint64_t value = 0;

    [[nodiscard]] bool valid() const noexcept { return fd >= 0; }
};

/// When a buffer becomes readable, and how to say we are done with it.
///
/// Both halves are optional, and their absence means something specific
/// rather than "unset":
///
///   no acquire  the pixels are ready NOW. True for a CPU buffer mayag just
///               filled, and for a client using implicit sync.
///   no release  the producer does not want to be told. True when nobody
///               reuses the buffer, and for implicit-sync clients.
///
/// So a default-constructed BufferSync is exactly the behaviour mayag has
/// today, which is what makes this additive: existing code keeps working
/// without knowing the type exists.
struct BufferSync {
    /// Wait for this before reading the buffer.
    std::optional<TimelinePoint> acquire;
    /// Signal this when the read is complete.
    std::optional<TimelinePoint> release;

    [[nodiscard]] bool is_implicit() const noexcept {
        return !acquire.has_value() && !release.has_value();
    }
};

/// What a device can do about synchronisation.
///
/// Reported rather than assumed, for the same reason `importable_formats()`
/// is: a compositor must know before it advertises. Promising a client
/// explicit sync and then falling back to implicit is worse than never
/// offering it, because the client will have stopped inserting its own
/// barriers.
struct SyncCaps {
    /// Can wait on and signal timeline points (VK_KHR_timeline_semaphore +
    /// external fd import). Without this, mayag falls back to the
    /// conservative waits it does today.
    bool timeline = false;
    /// Can import a DRM syncobj fd as a semaphore — what a Wayland client
    /// actually hands over via linux-drm-syncobj-v1.
    bool import_syncobj = false;
};

}  // namespace mayag
