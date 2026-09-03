#pragma once
// mayag::overlay — content that escapes its parent
//
// A menu, a modal, a tooltip and a dropdown all need the same three things,
// none of which normal flow layout provides:
//
//   1. ESCAPE THE CLIP. A dropdown inside a scrolling list must not be cut
//      off by that list's viewport. Ordinary children cannot do this — clip
//      rects are hierarchical by design.
//   2. PAINT ON TOP. Above every sibling, regardless of tree position. A
//      menu attached to the first toolbar button must cover the toolbar.
//   3. CAPTURE INPUT. A modal must swallow clicks aimed at what is behind it,
//      and clicking outside a menu should dismiss it rather than activating
//      whatever happens to be under the cursor.
//
// mayag models these as a separate LAYER rather than as flags on a node.
// Overlays are collected during the view pass and laid out afterwards against
// the viewport, so their geometry never depends on where in the tree they
// were declared — which is the property that makes "attach a menu to this
// button" work no matter how deeply that button is nested.

#include "../scene/node.hpp"
#include "../core/geometry.hpp"

#include <cstdint>
#include <vector>

namespace mayag {

/// Where an overlay sits relative to its anchor.
enum class Placement : std::uint8_t {
    below_start,   ///< dropdown: under the anchor, left edges aligned
    below_end,
    above_start,
    above_end,
    right_start,   ///< submenu
    left_start,
    centered,      ///< modal: centred in the viewport, anchor ignored
    cursor,        ///< context menu: at the pointer
};

/// How an overlay interacts with input.
enum class Dismiss : std::uint8_t {
    /// Clicking outside closes it, and that click is CONSUMED — a menu should
    /// not both close and activate whatever was behind it.
    on_click_outside,
    /// Clicks outside pass through untouched (tooltips, toasts).
    never,
    /// Modal: everything outside is blocked but nothing is dismissed except
    /// by the app.
    modal,
};

struct Overlay {
    Node          content;
    std::uint64_t id = 0;

    /// Node this is attached to; 0 means "viewport" (modals, toasts).
    std::uint64_t anchor_id = 0;

    Placement placement = Placement::below_start;
    Dismiss   dismiss   = Dismiss::on_click_outside;

    /// Gap between the anchor and the overlay.
    float offset = 4.0f;

    /// Painted in ascending order, so a submenu declared later covers its
    /// parent menu without the caller managing z explicitly.
    int layer = 0;

    /// Dim everything behind. Modal dialogs want this; menus do not.
    float scrim = 0.0f;
};

/// The overlay layer for one frame.
///
/// Collected by `view()` and resolved by the runtime AFTER the main tree is
/// laid out — that ordering is what lets an overlay position itself against
/// an anchor whose final rect is not known until layout finishes.
class OverlayList {
  public:
    void clear() noexcept { items_.clear(); }
    void add(Overlay o) { items_.push_back(std::move(o)); }

    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] const std::vector<Overlay>& items() const noexcept { return items_; }
    [[nodiscard]] std::vector<Overlay>& items() noexcept { return items_; }

    /// True when any overlay blocks input to the content beneath it.
    [[nodiscard]] bool captures_input() const noexcept {
        for (const auto& o : items_) {
            if (o.dismiss != Dismiss::never) return true;
        }
        return false;
    }

    /// The topmost overlay that should receive a click at `p`, or nullptr.
    [[nodiscard]] const Overlay* hit(Vec2 p) const noexcept {
        const Overlay* best = nullptr;
        for (const auto& o : items_) {
            if (o.content.frame().contains(p) && (best == nullptr || o.layer >= best->layer)) {
                best = &o;
            }
        }
        return best;
    }

    /// Overlays that a click at `p` should dismiss.
    [[nodiscard]] std::vector<std::uint64_t> dismissed_by(Vec2 p) const {
        std::vector<std::uint64_t> out;
        for (const auto& o : items_) {
            if (o.dismiss == Dismiss::on_click_outside && !o.content.frame().contains(p)) {
                out.push_back(o.id);
            }
        }
        return out;
    }

  private:
    std::vector<Overlay> items_;
};

namespace overlay {

/// Position an overlay against its anchor, keeping it on screen.
///
/// The clamping matters more than the placement: a dropdown near the bottom
/// of the window must flip above rather than hang off the edge, and one near
/// the right must slide left rather than be half-invisible. Every real menu
/// system does this and it is the first thing users notice missing.
[[nodiscard]] inline Rect resolve(Placement placement, const Rect& anchor,
                                  Vec2 size, Vec2 viewport, float gap,
                                  Vec2 cursor = {}) {
    Vec2 pos{};

    switch (placement) {
        case Placement::below_start: pos = {anchor.left(), anchor.bottom() + gap}; break;
        case Placement::below_end:   pos = {anchor.right() - size.x, anchor.bottom() + gap}; break;
        case Placement::above_start: pos = {anchor.left(), anchor.top() - size.y - gap}; break;
        case Placement::above_end:   pos = {anchor.right() - size.x, anchor.top() - size.y - gap}; break;
        case Placement::right_start: pos = {anchor.right() + gap, anchor.top()}; break;
        case Placement::left_start:  pos = {anchor.left() - size.x - gap, anchor.top()}; break;
        case Placement::cursor:      pos = cursor; break;
        case Placement::centered:
            pos = {(viewport.x - size.x) * 0.5f, (viewport.y - size.y) * 0.5f};
            break;
    }

    // ---- flip when there is no room ----
    //
    // Only for the anchored placements, and only when flipping actually
    // helps: flipping into an even smaller gap would be worse than clamping.
    if (placement == Placement::below_start || placement == Placement::below_end) {
        const float below_room = viewport.y - (anchor.bottom() + gap);
        const float above_room = anchor.top() - gap;
        if (size.y > below_room && above_room > below_room) {
            pos.y = anchor.top() - size.y - gap;
        }
    } else if (placement == Placement::above_start || placement == Placement::above_end) {
        const float above_room = anchor.top() - gap;
        const float below_room = viewport.y - (anchor.bottom() + gap);
        if (size.y > above_room && below_room > above_room) {
            pos.y = anchor.bottom() + gap;
        }
    } else if (placement == Placement::right_start) {
        if (pos.x + size.x > viewport.x && anchor.left() - size.x - gap >= 0.0f) {
            pos.x = anchor.left() - size.x - gap;
        }
    } else if (placement == Placement::left_start) {
        if (pos.x < 0.0f && anchor.right() + size.x + gap <= viewport.x) {
            pos.x = anchor.right() + gap;
        }
    }

    // ---- slide back on screen ----
    pos.x = num::clamp(pos.x, 0.0f, num::max(viewport.x - size.x, 0.0f));
    pos.y = num::clamp(pos.y, 0.0f, num::max(viewport.y - size.y, 0.0f));

    return Rect{pos, size};
}

}  // namespace overlay

}  // namespace mayag
