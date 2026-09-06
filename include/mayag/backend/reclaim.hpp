#pragma once

/// Deferred destruction: freeing a GPU resource when the GPU is done with it.
///
/// The naive way to destroy a VkImage safely is `DeviceWaitIdle` — stop the
/// entire GPU, then free. It is correct, trivial, and wrong for the same
/// reason a global lock is wrong: it answers "is ALL work finished?" when the
/// question was "is THIS image still being read?".
///
/// mayag paid for that twice already. Destroying a texture on resize without
/// waiting freed an image the GPU was sampling and hung inside the driver
/// with no stack pointing back at the code; the fix was a DeviceWaitIdle,
/// which fixed the hang and stalled every frame that changes size. A
/// compositor cannot afford either: it destroys buffers constantly, as
/// clients close windows and swap sizes, and a device-wide stall per event is
/// not a rounding error.
///
/// So: don't wait, and don't free either. RETIRE the resource — record which
/// frame last touched it, and reclaim it once that frame is known complete.
///
/// This is the same shape as the runtime's source retirement, deliberately.
/// Stopping a Sub::source used to join its thread inline, which blocked the
/// UI thread for however long the producer took to notice (measured at 370 ms
/// per stop). It now signals, moves the thread to a retiring_ list, and reaps
/// it on a later tick once it has actually finished. Same rule here, one
/// layer down: signal, set aside, collect when it is genuinely safe. The GPU
/// version is easier, because a frame counter is a perfect completion clock —
/// there is no ambiguity about whether frame N is done.

#include <cassert>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace mayag::backend {

/// Frees GPU resources once the frame that last used them has completed.
///
/// The contract is one number: `frames_completed`. Anything retired while
/// rendering frame N is safe to free once the GPU has finished N, and the
/// caller is the only one who knows when that is. Keeping the clock outside
/// this class is what stops it needing a device, a queue, or a fence -- it is
/// pure bookkeeping and can be unit-tested without a GPU at all.
///
/// THREADING: single-threaded, and deliberately not made otherwise.
///
/// Every call must happen on the thread that owns the device. That is not a
/// limitation to work around, it is the same rule the runtime already keeps
/// for update(): one thread owns the state, so there is nothing to race and
/// nothing to lock. A mutex here would be worse than useless -- it would make
/// the vector safe while leaving the actual hazard untouched, because the
/// DELETERS call into Vulkan, and a VkDevice may not be used concurrently
/// from two threads either. Locking the bookkeeping would hide that.
///
/// The rule is checked in debug builds rather than documented and hoped for:
/// the first call binds the owning thread and every later one asserts. A
/// compositor that releases buffers off the render thread finds out
/// immediately, instead of via a corrupted free list under load.
class Reclaimer {
  public:
    /// Retire `free_it`, to be called once frame `used_in` has completed.
    ///
    /// The deleter is a closure rather than a handle+type pair because the
    /// resources being freed are heterogeneous -- image, view, memory, and
    /// they must be destroyed in that order -- and expressing that as a
    /// tagged union buys nothing over letting the caller say it directly.
    void retire(std::uint64_t used_in, std::function<void()> free_it) {
        check_thread();
        pending_.push_back(Entry{used_in, std::move(free_it)});
    }

    /// Free everything whose frame has completed.
    ///
    /// Called once per frame, on the thread that owns the device. Cheap when
    /// there is nothing to do, which is the common case: the vector is empty
    /// and this is a branch.
    void collect(std::uint64_t frames_completed) {
        check_thread();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->used_in > frames_completed) { ++it; continue; }
            if (it->free_it) it->free_it();
            it = pending_.erase(it);
        }
    }

    /// Free EVERYTHING, regardless of frame.
    ///
    /// Only correct after the device really is idle -- teardown, where a
    /// blocking wait is the right answer because the device is going away and
    /// nothing can be left holding its memory.
    void collect_all() {
        check_thread();
        for (auto& e : pending_)
            if (e.free_it) e.free_it();
        pending_.clear();
    }

    [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }

  private:
    struct Entry {
        std::uint64_t         used_in = 0;
        std::function<void()> free_it;
    };
    std::vector<Entry> pending_;

    /// Bind the owning thread on first use, then assert on every call.
    ///
    /// Debug-only: in a release build this compiles to nothing, because the
    /// check exists to catch a programming error during development, not to
    /// defend against one at runtime. A wrong-thread call is a bug in the
    /// compositor, and the earliest possible crash is the kindest outcome.
    void check_thread() const noexcept {
#ifndef NDEBUG
        const auto self = std::this_thread::get_id();
        if (owner_ == std::thread::id{}) {
            owner_ = self;
        } else {
            assert(owner_ == self &&
                   "Reclaimer is single-threaded: every call must be on the "
                   "thread that owns the device. Releasing a buffer off the "
                   "render thread is the usual cause.");
        }
#endif
    }
#ifndef NDEBUG
    mutable std::thread::id owner_{};
#endif
};

}  // namespace mayag::backend
