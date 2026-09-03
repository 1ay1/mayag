#pragma once
// mayag::backend::ThreadPool — persistent workers for tiled rendering
//
// Rendering a frame is an embarrassingly parallel problem once the screen is
// split into tiles: each tile owns its pixels, so there is no sharing and no
// locking on the hot path.
//
// What matters is not having a thread pool but avoiding the costs that make
// naive ones useless at 60 Hz:
//
//   * Threads are created ONCE, not per frame. Spawning 8 threads costs
//     ~100 us on macOS — more than a whole tile's work.
//   * Work is claimed with a single atomic fetch_add on a shared counter,
//     so a thread that finishes a cheap tile immediately steals the next one
//     instead of idling. UI tiles vary enormously in cost (a blank margin vs
//     a paragraph of text), and static partitioning would leave cores idle
//     waiting for the slowest stripe.
//   * The calling thread participates rather than blocking, so an N-core
//     machine uses N cores, not N+1 threads fighting over N cores.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mayag::backend {

class ThreadPool {
  public:
    /// `workers` counts threads IN ADDITION to the caller. Zero means the
    /// pool runs everything inline, which is what the single-threaded and
    /// deterministic-test paths want.
    explicit ThreadPool(unsigned workers) {
        threads_.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) {
            threads_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard lock{mutex_};
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept { return threads_.size(); }

    /// Run `fn(i)` for i in [0, count), across the pool plus this thread.
    /// Blocks until every index has completed.
    ///
    /// The dynamic claim is the important part: `next_` is a single atomic
    /// counter, so a worker that finishes an empty tile immediately picks up
    /// the next one. With static ranges a single text-heavy tile would stall
    /// the whole frame while seven cores sat idle.
    template <typename Fn>
    void parallel_for(std::size_t count, Fn&& fn) {
        if (count == 0) return;

        if (threads_.empty() || count == 1) {
            for (std::size_t i = 0; i < count; ++i) fn(i);
            return;
        }

        job_ = [&fn](std::size_t i) { fn(i); };
        total_.store(count, std::memory_order_relaxed);
        next_.store(0, std::memory_order_relaxed);
        remaining_.store(count, std::memory_order_release);

        {
            std::lock_guard lock{mutex_};
            ++generation_;
        }
        wake_.notify_all();

        // The calling thread is a worker too.
        drain();

        // Spin briefly, then sleep. Tiles are short, so a spin usually wins;
        // the yield keeps a mis-predicted wait from burning a core.
        while (remaining_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }

        job_ = nullptr;
    }

  private:
    void worker_loop() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock{mutex_};
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            drain();
        }
    }

    /// Claim and run indices until the job is exhausted.
    void drain() {
        const std::size_t total = total_.load(std::memory_order_relaxed);
        for (;;) {
            const std::size_t i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= total) return;
            if (job_) job_(i);
            remaining_.fetch_sub(1, std::memory_order_release);
        }
    }

    std::vector<std::thread>          threads_;
    std::function<void(std::size_t)>  job_;
    std::atomic<std::size_t>          next_{0};
    std::atomic<std::size_t>          total_{0};
    std::atomic<std::size_t>          remaining_{0};

    std::mutex                mutex_;
    std::condition_variable   wake_;
    std::uint64_t             generation_ = 0;
    bool                      stop_ = false;
};

/// The process-wide pool, sized to the machine.
///
/// One fewer than hardware_concurrency because the calling thread
/// participates; on an 8-core M1 that is 7 workers + the caller.
[[nodiscard]] inline ThreadPool& shared_pool() {
    static ThreadPool pool{[] {
        const unsigned n = std::thread::hardware_concurrency();
        return n > 1 ? n - 1 : 0u;
    }()};
    return pool;
}

}  // namespace mayag::backend
