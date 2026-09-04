#pragma once
// mayag::platform::Waker — cross-thread wakeup for a blocked UI thread
//
// An idle mayag app BLOCKS. That is the whole point of the subscription model:
// `Wait::block` is a real poll() on the window's fd with no timeout, so an app
// that is not animating sits at 0% CPU. But a *live* app receives data from
// somewhere other than the window — a socket, a subprocess, a log tail, a
// worker thread — and that producer runs on its own thread. When it posts a
// message to the runtime's Inbox, the UI thread is asleep in poll(), watching
// only the window fd. Nothing wakes it. The message sits unrendered until the
// user happens to move the mouse, and the app looks frozen.
//
// This closes that gap. A Waker owns one end of a self-pipe. A producer thread
// calls wake(); the window's poll() also watches the read end, so the write
// interrupts the sleep immediately. On the next loop the runtime drains the
// Inbox and the frame the producer asked for actually renders.
//
// A self-pipe rather than eventfd: eventfd is Linux-only, and this must work
// on macOS and the BSDs too. The cost is one extra fd and a one-byte write,
// which is irrelevant next to being able to sleep at 0% CPU instead of polling
// a timer.

#include <atomic>

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__unix__)
#define MAYAG_WAKER_POSIX 1
#include <fcntl.h>
#include <unistd.h>
#else
#define MAYAG_WAKER_POSIX 0
#endif

namespace mayag::platform {

/// A wakeup channel. Constructed by the runtime, shared (via a pointer) with
/// both the Inbox and the window. Thread-safe: wake() may be called from any
/// thread; fd() and drain() are used only by the UI thread inside poll().
class Waker {
  public:
    Waker() {
#if MAYAG_WAKER_POSIX
        int fds[2];
        if (::pipe(fds) == 0) {
            read_fd_ = fds[0];
            write_fd_ = fds[1];
            // Non-blocking both ways: wake() must never block a producer even
            // if the pipe buffer is full (many wakes, one drain), and drain()
            // must never block the UI thread if the pipe is empty.
            set_nonblocking(read_fd_);
            set_nonblocking(write_fd_);
        }
#endif
    }

    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;

    ~Waker() {
#if MAYAG_WAKER_POSIX
        if (read_fd_ >= 0) ::close(read_fd_);
        if (write_fd_ >= 0) ::close(write_fd_);
#endif
    }

    /// Interrupt a UI thread blocked in poll(). Safe from any thread.
    ///
    /// `pending_` is a coalescing guard: many wakes between two drains write at
    /// most one byte, so a burst of a thousand streamed messages costs one
    /// syscall, not a thousand. The UI thread clears it in drain().
    void wake() noexcept {
        if (pending_.exchange(true, std::memory_order_release)) return;
#if MAYAG_WAKER_POSIX
        if (write_fd_ >= 0) {
            const unsigned char byte = 1;
            ssize_t rc = ::write(write_fd_, &byte, 1);
            (void)rc;   // a full pipe already means "wake is pending"
        }
#endif
    }

    /// The read end, for the window to add to its poll set. -1 when the
    /// platform has no self-pipe (then the runtime falls back to a timeout).
    [[nodiscard]] int fd() const noexcept { return read_fd_; }

    /// Drain the pipe on the UI thread after poll() returns. Clears the
    /// coalescing guard so the next wake() writes again.
    void drain() noexcept {
        pending_.store(false, std::memory_order_release);
#if MAYAG_WAKER_POSIX
        if (read_fd_ >= 0) {
            unsigned char buf[64];
            while (::read(read_fd_, buf, sizeof(buf)) > 0) { /* discard */ }
        }
#endif
    }

    [[nodiscard]] bool usable() const noexcept { return read_fd_ >= 0; }

  private:
#if MAYAG_WAKER_POSIX
    static void set_nonblocking(int fd) noexcept {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif

    int read_fd_ = -1;
    int write_fd_ = -1;
    std::atomic<bool> pending_{false};
};

}  // namespace mayag::platform
