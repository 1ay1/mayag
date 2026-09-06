#pragma once

/// A display that exists only in memory.
///
/// Two jobs. It proves the `Output` concept is implementable by something
/// that is not DRM/KMS — an abstraction with exactly one implementation is
/// not an abstraction — and it makes compositor logic testable without
/// hardware, which is the gap that let a freed-while-sampled VkImage reach a
/// release: every headless test rendered in software and could not see it.
///
/// So this records what was committed rather than displaying it, and lets a
/// test say "refuse the next commit" to exercise the fallback path that real
/// hardware only takes occasionally and never on demand.

#include "output.hpp"
#include "planes.hpp"

#include <string>
#include <vector>

namespace mayag::output {

class HeadlessOutput {
  public:
    explicit HeadlessOutput(std::string name = "headless-1",
                            DisplayMode mode = DisplayMode{1920, 1080, 60000, true})
        : name_{std::move(name)}, modes_{mode}, current_{mode} {}

    // ── Output ───────────────────────────────────────────────────────────

    [[nodiscard]] std::span<const DisplayMode> modes() const noexcept { return modes_; }
    [[nodiscard]] std::optional<DisplayMode> current_mode() const noexcept { return current_; }

    [[nodiscard]] CommitResult set_mode(const DisplayMode& m) {
        const bool known = std::find(modes_.begin(), modes_.end(), m) != modes_.end();
        if (!known) return std::unexpected(CommitError::unsupported_layout);
        current_ = m;
        return Presented{time_, sequence_, 0};
    }

    /// Record a commit, honouring whatever failure a test has armed.
    [[nodiscard]] CommitResult commit(std::span<const Layer> layers) {
        if (armed_error_) {
            const CommitError e = *armed_error_;
            armed_error_.reset();          // one-shot, like real hardware
            ++refused_;
            return std::unexpected(e);
        }
        if (!current_) return std::unexpected(CommitError::disconnected);

        for (const Layer& l : layers) {
            if (!l.buffer.valid()) return std::unexpected(CommitError::buffer_unusable);
        }

        const auto plan = plan_planes(layers, caps_);
        last_ = std::vector<Layer>{layers.begin(), layers.end()};
        last_plan_ = plan;
        ++sequence_;
        time_ += current_->refresh_hz() > 0.0 ? 1.0 / current_->refresh_hz() : 0.0;
        return Presented{time_, sequence_, direct_count(plan)};
    }

    [[nodiscard]] std::span<const FormatModifier> scanout_formats() const noexcept {
        return scanout_;
    }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] Vec2 physical_size_mm() const noexcept { return physical_mm_; }

    // ── test control ─────────────────────────────────────────────────────

    /// Make the NEXT commit fail with `e`.
    ///
    /// Real hardware refuses a layout occasionally and unpredictably, which
    /// means the fallback path is the least-tested code in any compositor.
    /// Arming a failure makes it reachable on purpose.
    void fail_next(CommitError e) { armed_error_ = e; }

    void set_plane_caps(PlaneCaps c) noexcept { caps_ = c; }
    [[nodiscard]] const PlaneCaps& plane_caps() const noexcept { return caps_; }

    void set_modes(std::vector<DisplayMode> m) { modes_ = std::move(m); }
    void set_scanout_formats(std::vector<FormatModifier> f) { scanout_ = std::move(f); }
    void disconnect() noexcept { current_.reset(); }

    [[nodiscard]] std::span<const Layer> last_commit() const noexcept { return last_; }
    [[nodiscard]] std::span<const Assignment> last_plan() const noexcept { return last_plan_; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] int refused() const noexcept { return refused_; }

  private:
    std::string                 name_;
    std::vector<DisplayMode>           modes_;
    std::optional<DisplayMode>         current_;
    std::vector<FormatModifier> scanout_;
    PlaneCaps                   caps_{};
    Vec2                        physical_mm_{600.0f, 340.0f};

    std::vector<Layer>          last_;
    std::vector<Assignment>     last_plan_;
    std::uint64_t               sequence_ = 0;
    double                      time_ = 0.0;
    int                         refused_ = 0;
    std::optional<CommitError>  armed_error_;
};

static_assert(Output<HeadlessOutput>,
              "HeadlessOutput must satisfy the Output concept -- it exists to "
              "prove the concept is implementable by more than DRM/KMS");

}  // namespace mayag::output
