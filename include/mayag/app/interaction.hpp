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
    enum class Kind : std::uint8_t {
        click, double_click, press, release, enter, leave, drag, scroll,
    };

    Kind          kind = Kind::click;
    std::uint64_t node_id = 0;
    Vec2          position{};        ///< window coordinates
    Vec2          local{};           ///< relative to the node's frame origin
    Vec2          delta{};           ///< drag/scroll amount
    MouseButton   button = MouseButton::left;

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

                pressed_      = hit;
                press_origin_ = e.position;
                press_button_ = e.button;
                dragged_      = false;

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
                        const bool is_double =
                            (was == last_click_node_) &&
                            (now_seconds - last_click_time_ < double_click_seconds) &&
                            ((e.position - last_click_pos_).length() < double_click_slop);

                        out.push_back(Gesture{is_double ? Gesture::Kind::double_click
                                                        : Gesture::Kind::click,
                                              was, e.position, local, Vec2{}, e.button});

                        // Reset on a double so a triple click is not two doubles.
                        last_click_node_ = is_double ? 0 : was;
                        last_click_time_ = now_seconds;
                        last_click_pos_  = e.position;
                    }
                }

                pressed_ = 0;
                dragged_ = false;
                hovered_ = hit;
            }

            else if constexpr (std::is_same_v<T, ScrollEvent>) {
                cursor_ = e.position;
                // Scroll targets the innermost scrollable ancestor; we report
                // the hit node and let `update()` decide.
                const std::uint64_t hit = identify(root, e.position);
                if (hit != 0) {
                    const Node* n = root.find(hit);
                    const Vec2 local = n ? e.position - n->frame().origin : Vec2{};
                    out.push_back(Gesture{Gesture::Kind::scroll, hit, e.position, local,
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
    static constexpr float  drag_threshold       = 4.0f;
    static constexpr double double_click_seconds = 0.4;
    static constexpr float  double_click_slop    = 6.0f;

  private:
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
};

}  // namespace mayag
