#pragma once
// mayag::backend::ThreadPool — tiled rendering's parallel_for, on jaal.
//
// The workers are jaal::kernel::pool threads: created once, kept warm, shut
// down by jaal. What this adds is the fork-join a frame needs:
//
//   * indices are claimed with one atomic fetch_add, so a worker that finishes
//     a blank tile takes the next one instead of idling behind a text-heavy one
//   * the calling thread drains too, so N cores means N threads, and it never
//     waits on a helper that hasn't started yet
//   * each call is its own block, co-owned by the helpers. A helper that wakes
//     after every index is claimed only touches that block, never `fn` or the
//     next call (the race TSAN found in the old hand-rolled pool).
//
// Same shape as maya's view-side executor (maya/host/executor.hpp).

#include <jaal/kernel/pool.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
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
    template <typename Fn>
    void parallel_for(std::size_t count, Fn&& fn) {
        if (count == 0) return;
        if (!pool_ || count == 1) {
            for (std::size_t i = 0; i < count; ++i) fn(i);
            return;
        }

        const std::function<void(std::size_t)> body = [&fn](std::size_t i) { fn(i); };
        auto job = std::make_shared<Join>();
        job->fn = &body;
        job->n  = count;

        // One helper per worker, not per index: each drains until empty.
        const std::size_t helpers = std::min<std::size_t>(workers_, count - 1);
        for (std::size_t h = 0; h < helpers; ++h)
            ::jaal::kernel::pool_access::post(*pool_, [job](std::stop_token) { job->drain(); });

        job->drain();
        for (std::size_t d = job->done.load(std::memory_order_acquire); d < count;
             d = job->done.load(std::memory_order_acquire))
            job->done.wait(d, std::memory_order_acquire);
        if (job->error) std::rethrow_exception(job->error);
    }

  private:
    struct Join {
        const std::function<void(std::size_t)>* fn = nullptr;   // valid until done == n
        std::size_t                             n  = 0;
        std::atomic<std::size_t>                next{0};
        std::atomic<std::size_t>                done{0};
        std::atomic<bool>                       failed{false};
        std::exception_ptr                      error;           // set once, by whoever flips failed

        void drain() {
            for (;;) {
                const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) return;
                try {
                    (*fn)(i);
                } catch (...) {
                    if (!failed.exchange(true, std::memory_order_acq_rel)) error = std::current_exception();
                }
                if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == n) done.notify_one();
            }
        }
    };

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
