#pragma once
// mayag::Interaction — pointer and focus state machine
//
// The piece that turns a stream of coordinates into semantic gestures.
// It answers the questions every GUI must answer and most get subtly wrong:
//
//   * Which node is under the cursor? (topmost hit, respecting clip rects)
//   * Did a click happen? (press and release on the SAME node — dragging off
//     a button and releasing must NOT fire it)
//   * Which node has keyboard focus, and where does Tab go next?
//   * Is this the second click of a double-click? (time AND distance)
//
// Deliberately a plain value type with a pure `handle()` method: given the
// previous interaction state, a laid-out tree, and an event, produce the new
// state plus a list of gestures. No callbacks, no globals, no ownership. That
// makes the whole interaction model unit-testable without a window.

#include "../scene/node.hpp"
#include "event.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace mayag {

/// A semantic gesture, resolved to a node.
struct Gesture {
    /// Note there is no separate `double_click`: a double click IS a click,
    /// with `click_count == 2`. Emitting both (as an earlier version did)
    /// meant an app handling each fired its single-click action on every
    /// double — a bug the API invited rather than prevented.
    enum class Kind : std::uint8_t {
        click, press, release, enter, leave, drag, scroll,
    };

    Kind          kind = Kind::click;
    std::uint64_t node_id = 0;
    Vec2          position{};        ///< window coordinates
    Vec2          local{};           ///< relative to the node's frame origin
    Vec2          delta{};           ///< drag/scroll amount
    MouseButton   button = MouseButton::left;

    /// How many clicks this one completes: 1 single, 2 double, 3 triple, ...
    ///
    /// Carried on the gesture rather than encoded in `kind` because the two
    /// are orthogonal, and because a UI that only cares about "was clicked"
    /// should not have to enumerate every count. A text field wanting
    /// select-word on double and select-line on triple reads this directly.
    int           click_count = 1;

    friend bool operator==(const Gesture&, const Gesture&) = default;
};

class Interaction {
  public:
    // ── observable state (what `view()` reads to style itself) ──────────

    [[nodiscard]] std::uint64_t hovered() const noexcept { return hovered_; }
    [[nodiscard]] std::uint64_t pressed() const noexcept { return pressed_; }
    [[nodiscard]] std::uint64_t focused() const noexcept { return focused_; }
    [[nodiscard]] Vec2 cursor() const noexcept { return cursor_; }
    [[nodiscard]] bool dragging() const noexcept { return pressed_ != 0 && dragged_; }

    [[nodiscard]] bool is_hovered(std::uint64_t id) const noexcept {
        return id != 0 && hovered_ == id;
    }
    [[nodiscard]] bool is_pressed(std::uint64_t id) const noexcept {
        return id != 0 && pressed_ == id;
    }
    [[nodiscard]] bool is_focused(std::uint64_t id) const noexcept {
        return id != 0 && focused_ == id;
    }

    void set_focus(std::uint64_t id) noexcept { focused_ = id; }

    // ── the state machine ───────────────────────────────────────────────

    /// Process one event against the laid-out tree. Returns the gestures it
    /// produced (usually zero or one; a move across a boundary produces both
    /// a `leave` and an `enter`).
    [[nodiscard]] std::vector<Gesture> handle(const Event& ev, const Node& root,
                                              double now_seconds) {
        std::vector<Gesture> out;

        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<T, MouseMove>) {
                cursor_ = e.position;

                // While a button is held, the pressed node keeps the pointer
                // ("pointer capture"). Without this, dragging a slider stops
                // working the moment the cursor leaves the knob.
                if (pressed_ != 0) {
                    dragged_ = dragged_ || (e.position - press_origin_).length() > drag_threshold;
                    if (dragged_) {
                        if (const Node* n = root.find(pressed_)) {
                            out.push_back(Gesture{Gesture::Kind::drag, pressed_, e.position,
                                                  e.position - n->frame().origin, e.delta,
                                                  press_button_});
                        }
                    }
                    return;
                }

                const std::uint64_t now_over = identify(root, e.position);
                if (now_over != hovered_) {
                    if (hovered_ != 0) {
                        out.push_back(Gesture{Gesture::Kind::leave, hovered_, e.position});
                    }
                    hovered_ = now_over;
                    if (hovered_ != 0) {
                        out.push_back(Gesture{Gesture::Kind::enter, hovered_, e.position});
                    }
                }
            }

            else if constexpr (std::is_same_v<T, MouseDown>) {
                cursor_ = e.position;
                const std::uint64_t hit = identify(root, e.position);

                pressed_           = hit;
                press_origin_      = e.position;
                press_button_      = e.button;
                press_click_count_ = e.click_count;
                dragged_           = false;

                // Clicking anywhere moves keyboard focus there — including to
                // nothing, which is how you dismiss a text field.
                focused_ = hit;

                if (hit != 0) {
                    const Node* n = root.find(hit);
                    const Vec2 local = n ? e.position - n->frame().origin : Vec2{};
                    out.push_back(Gesture{Gesture::Kind::press, hit, e.position, local,
                                          Vec2{}, e.button});
                }
            }

            else if constexpr (std::is_same_v<T, MouseUp>) {
                cursor_ = e.position;
                const std::uint64_t hit = identify(root, e.position);
                const std::uint64_t was = pressed_;

                if (was != 0) {
                    const Node* n = root.find(was);
                    const Vec2 local = n ? e.position - n->frame().origin : Vec2{};
                    out.push_back(Gesture{Gesture::Kind::release, was, e.position, local,
                                          Vec2{}, e.button});

                    // A click requires press AND release on the same node.
                    // Dragging off a button and letting go must not fire it —
                    // that is the affordance that lets a user cancel.
                    //
                    // But note the condition is about WHERE THE RELEASE
                    // HAPPENED, not about whether the pointer ever moved far.
                    // Wandering off a button and coming back before releasing
                    // IS a click, on every platform. An earlier version also
                    // required `!dragged_`, which broke exactly that case.
                    if (hit == was) {
                        // The click COUNT comes from the platform when the
                        // platform knows it.
                        //
                        // macOS, Windows and X11 all track click sequences
                        // themselves, using the user's configured interval
                        // and their own notion of proximity. Re-deriving it
                        // from timestamps means fighting the OS and losing:
                        // the threshold is a system preference, it changes
                        // while the app runs, and accessibility settings can
                        // stretch it dramatically. An earlier version
                        // decoded `clickCount` from NSEvent and then threw
                        // it away to recompute a worse answer.
                        //
                        // The fallback below exists for backends that give
                        // us nothing (a raw framebuffer, a test harness).
                        const int count = press_click_count_ > 0
                            ? press_click_count_
                            : synthesise_count(was, e.position, now_seconds);

                        Gesture g{Gesture::Kind::click, was, e.position, local,
                                  Vec2{}, e.button};
                        g.click_count = count;
                        out.push_back(g);

                        last_click_node_  = was;
                        last_click_time_  = now_seconds;
                        last_click_pos_   = e.position;
                        last_click_count_ = count;
                    }
                }

                pressed_ = 0;
                dragged_ = false;
                hovered_ = hit;
            }

            else if constexpr (std::is_same_v<T, ScrollEvent>) {
                cursor_ = e.position;

                // A wheel event targets the innermost SCROLLABLE node under
                // the cursor, not the innermost named one.
                //
                // The cursor is almost always over a leaf — a row, a label —
                // and reporting that leaf means the scroll subscription on the
                // enclosing list never fires. Every real UI bubbles the wheel
                // to the nearest scrollable ancestor; without it, a list is
                // unscrollable exactly when the pointer is over its content,
                // which is always.
                std::uint64_t target = 0;
                Vec2 local{};
                scroll_target(root, e.position, target, local);

                if (target != 0) {
                    out.push_back(Gesture{Gesture::Kind::scroll, target, e.position, local,
                                          e.delta});
                }
            }

            else if constexpr (std::is_same_v<T, FocusEvent>) {
                // Losing window focus must clear hover/press, or the UI is
                // left with a stuck highlight when you alt-tab away.
                if (!e.focused) {
                    hovered_ = 0;
                    pressed_ = 0;
                    dragged_ = false;
                }
            }
        }, ev);

        return out;
    }

    /// Move keyboard focus to the next (or previous) named node, in tree
    /// order. This is Tab handling, and having it here means every mayag app
    /// gets keyboard navigation without writing any.
    void focus_next(const Node& root, bool backward = false) {
        std::vector<std::uint64_t> order;
        collect_focusable(root, order);
        if (order.empty()) { focused_ = 0; return; }

        auto it = std::find(order.begin(), order.end(), focused_);
        if (it == order.end()) {
            focused_ = backward ? order.back() : order.front();
            return;
        }
        const auto idx = static_cast<std::ptrdiff_t>(it - order.begin());
        const auto n   = static_cast<std::ptrdiff_t>(order.size());
        focused_ = order[static_cast<std::size_t>(((idx + (backward ? -1 : 1)) % n + n) % n)];
    }

    // ── tuning ──────────────────────────────────────────────────────────

    /// Movement beyond this (logical px) turns a press into a drag. Matches
    /// the platform convention that a slightly shaky click is still a click.
    static constexpr float drag_threshold = 4.0f;

    /// Multi-click window, used ONLY when the platform does not report a
    /// click count. Settable so a backend can install the real system value
    /// (macOS: `NSEvent.doubleClickInterval`, Windows: `GetDoubleClickTime`,
    /// X11: the Xt multi-click time) instead of this fallback.
    void set_multi_click_interval(double seconds) noexcept {
        multi_click_seconds_ = seconds > 0.0 ? seconds : 0.5;
    }
    [[nodiscard]] double multi_click_interval() const noexcept { return multi_click_seconds_; }

    /// How far the pointer may move between clicks of a sequence.
    void set_multi_click_slop(float px) noexcept { multi_click_slop_ = px; }

  private:
    /// Innermost node under `p` that is BOTH named and a scroll viewport.
    ///
    /// Falls back to the innermost named node when nothing scrolls, so a
    /// non-scrolling region can still observe the wheel if it subscribes.
    static void scroll_target(const Node& n, Vec2 p,
                              std::uint64_t& best, Vec2& local) noexcept {
        if (!n.frame().contains(p)) return;

        if (n.style().id != 0 && n.style().layout.scroll != nullptr) {
            best  = n.style().id;
            local = p - n.frame().origin;
        }
        for (const auto& kid : n.children()) scroll_target(kid, p, best, local);
    }

    /// Derive a click count from timing and distance.
    ///
    /// Only reached when the platform reports nothing. Deliberately generic:
    /// it counts to any depth rather than stopping at "double", so a text
    /// field can implement select-word / select-line / select-paragraph
    /// without the interaction layer needing to know those concepts exist.
    [[nodiscard]] int synthesise_count(std::uint64_t node, Vec2 p, double now) const noexcept {
        const bool continues =
            node == last_click_node_ &&
            (now - last_click_time_) < multi_click_seconds_ &&
            (p - last_click_pos_).length() < multi_click_slop_;
        return continues ? last_click_count_ + 1 : 1;
    }

    /// Topmost NAMED node containing `p`. Unnamed nodes are transparent to
    /// hit testing: a `v(...)` wrapper should not swallow clicks meant for
    /// the button inside it, and requiring an explicit `id<>` to be clickable
    /// makes that rule predictable.
    [[nodiscard]] static std::uint64_t identify(const Node& root, Vec2 p) {
        std::uint64_t found = 0;
        hit_recurse(root, p, found);
        return found;
    }

    static void hit_recurse(const Node& n, Vec2 p, std::uint64_t& found) {
        if (!n.frame().contains(p)) return;
        // Clipping parents cull their subtree — a scrolled-away child is not
        // clickable even though its frame still says it contains the point.
        if (n.style().clip && !n.frame().contains(p)) return;

        // Children paint on top of parents, so search them first and let the
        // last (topmost) match win.
        for (const auto& kid : n.children()) hit_recurse(kid, p, found);

        if (found == 0 && n.style().id != 0) found = n.style().id;
    }

    static void collect_focusable(const Node& n, std::vector<std::uint64_t>& out) {
        if (n.style().id != 0) out.push_back(n.style().id);
        for (const auto& kid : n.children()) collect_focusable(kid, out);
    }

    std::uint64_t hovered_ = 0;
    std::uint64_t pressed_ = 0;
    std::uint64_t focused_ = 0;
    Vec2          cursor_{};
    Vec2          press_origin_{};
    MouseButton   press_button_ = MouseButton::left;
    bool          dragged_ = false;

    std::uint64_t last_click_node_ = 0;
    double        last_click_time_ = -1.0;
    Vec2          last_click_pos_{};
    int           last_click_count_ = 0;
    /// Click count reported by the platform on the current press; 0 when the
    /// backend does not supply one.
    int           press_click_count_ = 0;

    double        multi_click_seconds_ = 0.5;
    float         multi_click_slop_    = 6.0f;
};

}  // namespace mayag
