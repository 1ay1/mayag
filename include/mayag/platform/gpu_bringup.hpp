#pragma once
// mayag::platform::GpuBringUp — the ONE way a window brings its GPU up.
//
// A device's instance, adapter and pipeline need no window, and they are
// the longest single thing between launch and the first frame (D3D11
// ~100 ms warm; Vulkan 0.3 s warm, ~2 s cold on a hybrid laptop). So every
// window does the same four things, and they used to be written out twice
// (Wayland's prep_pool_ / prep_ / gpu_pending_, Win32's gpu_pool /
// gpu_pool_owned / gpu_prep / Prestart):
//
//   start(device)          prepare it on a one-thread worker, now
//   first_frame(budget)    frame one waits up to `budget` for it, so the
//                          first frame is already a GPU frame
//   poll()                 later frames take it the moment it is ready
//                          (a GPU that missed the budget takes over then)
//   finish()               teardown: wait for the job and hand back the device
//
// Taking the result also frees the worker. What stays per window is what
// really differs: how a device is prepared (Prepare::run) and how a ready
// one is attached to the surface.
//
// The result of first_frame / poll is a three-way answer:
//   nullopt          not ready yet (draw this frame on the CPU)
//   a null pointer   the GPU did not come up (stay on the CPU)
//   a device         attach it and present on it from now on

#include <jaal/kernel/pool.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>

namespace mayag::platform {

/// `Prepare::run(device)` brings a device up off the window's thread and
/// returns it, or null when it cannot run here. A type, not a lambda: the
/// worker's body must be captureless (jaal's pool rule).
template <class P, class Device>
concept GpuPreparer = requires(std::unique_ptr<Device> d) {
    { P::run(std::move(d)) } -> std::same_as<std::unique_ptr<Device>>;
};

template <class Device, GpuPreparer<Device> Prepare>
class GpuBringUp {
  public:
    using Taken = std::optional<std::unique_ptr<Device>>;

    GpuBringUp() = default;
    GpuBringUp(GpuBringUp&&) noexcept = default;
    GpuBringUp& operator=(GpuBringUp&&) noexcept = default;

    /// Prepare `device` on a worker, starting now.
    void start(std::unique_ptr<Device> device) {
        pool_ = std::make_unique<jaal::kernel::pool>(1);
        job_ = pool_->submit([](std::stop_token, std::unique_ptr<Device> d) { return Prepare::run(std::move(d)); },
                             std::move(device));
    }

    /// Started and not yet taken: the window draws on the CPU meanwhile.
    [[nodiscard]] bool pending() const noexcept { return job_.valid(); }
    /// Pending and finished: the next frame can take it.
    [[nodiscard]] bool ready() const { return job_.ready(); }

    /// Frame one: wait up to `budget` (woken the instant the job finishes),
    /// then take the result if there is one. A zero budget never blocks.
    [[nodiscard]] Taken first_frame(std::chrono::milliseconds budget) {
        if (!pending()) return std::nullopt;
        if (budget.count() > 0 && !job_.ready()) (void)job_.wait_for_at_root(budget);
        return take();
    }
    /// Any later frame: take the result if the job has finished. Never blocks.
    [[nodiscard]] Taken poll() { return take(); }

    /// Teardown: wait for a job still running and hand back its device, so
    /// the window can release it on its own terms. Null when there is none.
    [[nodiscard]] std::unique_ptr<Device> finish() {
        std::unique_ptr<Device> d;
        if (pending()) d = job_.get_at_root();
        pool_.reset();
        return d;
    }

  private:
    [[nodiscard]] Taken take() {
        auto d = job_.try_take();   // never waits: the result only if the job has finished
        if (d) pool_.reset();       // the worker is done with
        return d;
    }

    std::unique_ptr<jaal::kernel::pool> pool_;
    jaal::kernel::job_result<std::unique_ptr<Device>> job_;
};

}  // namespace mayag::platform
