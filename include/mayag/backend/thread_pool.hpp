#pragma once
// mayag::backend::ThreadPool — tiled rendering's parallel_for, on jaal.
//
// Every thread here is a jaal::kernel::pool worker, and the fork-join is
// jaal::parallel_for(pool&): jaal owns the threads, the shared block, the
// wait and its deadlock checks (it aborts with the reason if the caller holds
// a lock or is one of the pool's own jobs). mayag adds only the sizing and
// the inline path.
//
// The workers are made once and kept warm, so a frame's fork-join costs a few
// microseconds rather than a thread spawn per tile.

#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/scope.hpp>

#include <cstddef>
#include <memory>
#include <thread>

namespace mayag::backend {

class ThreadPool {
  public:
    /// `workers` counts threads IN ADDITION to the caller. Zero runs
    /// everything inline (the deterministic single-threaded path).
    explicit ThreadPool(unsigned workers) : workers_(workers) {
        if (workers_ > 0) pool_ = std::make_unique<::jaal::kernel::pool>(workers_);
    }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_; }

    /// Run `fn(i)` for i in [0, count) across the pool and this thread.
    /// Blocks until every index has run. A throw from `fn` is rethrown here.
    /// `fn` must be safe to call from several threads at once.
    template <typename Fn>
    void parallel_for(std::size_t count, Fn&& fn) {
        if (count == 0) return;
        if (!pool_ || count == 1) {
            for (std::size_t i = 0; i < count; ++i) fn(i);
            return;
        }
        ::jaal::parallel_for(*pool_, count, workers_ + 1, fn);
    }

  private:
    unsigned                               workers_ = 0;
    std::unique_ptr<::jaal::kernel::pool>  pool_;
};

/// The process-wide pool: one fewer worker than cores, because the caller
/// drains too.
[[nodiscard]] inline ThreadPool& shared_pool() {
    static ThreadPool pool{[] {
        const unsigned n = std::thread::hardware_concurrency();
        return n > 1 ? n - 1 : 0u;
    }()};
    return pool;
}

}  // namespace mayag::backend
