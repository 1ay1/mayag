#pragma once
// mayag::History<Model> — undo/redo for free
//
// mayag's Model is a plain value, which makes undo almost trivial: keep the
// old values. Every app would otherwise reimplement this, and most would get
// the same two things wrong.
//
//     struct Model { History<Doc> doc; };
//
//     m.doc.edit([](Doc& d) { d.items.push_back(item); });   // records
//     m.doc.undo();
//     m.doc.redo();
//     m.doc.current().items;                                  // read
//
// The two things:
//
//   1. COALESCING. Typing ten characters should be ONE undo, not ten. Without
//      it, undo is unusable in a text field — the user presses Cmd-Z and
//      loses a letter. Edits tagged with the same `group` inside a short
//      window merge.
//
//   2. A BOUND. An unbounded history of a large model is a memory leak that
//      only shows up in long sessions, which is exactly when losing work
//      hurts most. The depth is explicit and old states are dropped from the
//      front.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace mayag {

/// Undo/redo over any copyable value.
template <typename T>
class History {
  public:
    History() = default;
    explicit History(T initial) : current_{std::move(initial)} {}

    // ── reading ─────────────────────────────────────────────────────────

    [[nodiscard]] const T& current() const noexcept { return current_; }
    [[nodiscard]] const T& operator*() const noexcept { return current_; }
    [[nodiscard]] const T* operator->() const noexcept { return &current_; }

    [[nodiscard]] bool can_undo() const noexcept { return !past_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !future_.empty(); }
    [[nodiscard]] std::size_t undo_depth() const noexcept { return past_.size(); }
    [[nodiscard]] std::size_t redo_depth() const noexcept { return future_.size(); }

    // ── editing ─────────────────────────────────────────────────────────

    /// Apply a change and record it.
    ///
    /// `group` coalesces: consecutive edits sharing a non-empty group merge
    /// into one undo step. Pass "typing" while a user types and "" (or a new
    /// group) at a boundary — a space, a paste, a click elsewhere.
    template <typename Fn>
    void edit(Fn&& fn, std::string_view group = {}) {
        const bool coalesce = !group.empty() && group == last_group_ && !past_.empty();

        if (!coalesce) {
            past_.push_back(current_);
            if (past_.size() > max_depth_) past_.pop_front();
        }

        // Any edit invalidates the redo branch. Keeping it would let a user
        // redo into a future that no longer follows from the present.
        future_.clear();
        last_group_ = std::string{group};

        fn(current_);
    }

    /// Replace wholesale, recording the previous value.
    void set(T value, std::string_view group = {}) {
        edit([&](T& v) { v = std::move(value); }, group);
    }

    /// Change without recording — for state that should not be undoable
    /// (a scroll position, a hover flag) but happens to live in the model.
    template <typename Fn>
    void edit_transient(Fn&& fn) { fn(current_); }

    bool undo() {
        if (past_.empty()) return false;
        future_.push_front(std::move(current_));
        current_ = std::move(past_.back());
        past_.pop_back();
        // Break coalescing, or the next edit would merge into a step the
        // user just undid past.
        last_group_.clear();
        return true;
    }

    bool redo() {
        if (future_.empty()) return false;
        past_.push_back(std::move(current_));
        current_ = std::move(future_.front());
        future_.pop_front();
        last_group_.clear();
        return true;
    }

    /// Forget history, keeping the current value. For "document saved".
    void clear_history() {
        past_.clear();
        future_.clear();
        last_group_.clear();
    }

    /// Start a new undo step even if the next edit shares a group. Call at a
    /// natural boundary — the user clicked away, or paused typing.
    void break_coalescing() noexcept { last_group_.clear(); }

    /// How many states to retain. Bounded on purpose: an unbounded history of
    /// a large model is a leak that only bites in long sessions.
    void set_max_depth(std::size_t n) {
        max_depth_ = n > 0 ? n : 1;
        while (past_.size() > max_depth_) past_.pop_front();
    }
    [[nodiscard]] std::size_t max_depth() const noexcept { return max_depth_; }

  private:
    T                 current_{};
    std::deque<T>     past_;
    std::deque<T>     future_;
    std::string       last_group_;
    std::size_t       max_depth_ = 100;
};

}  // namespace mayag
