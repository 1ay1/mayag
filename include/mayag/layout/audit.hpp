#pragma once
// mayag::layout::audit — catch broken layout instead of shipping it
//
// The type-state DSL rejects combinations that are ALWAYS wrong. But some
// layout faults depend on runtime sizes and cannot be decided at compile
// time: a sidebar squeezed to nothing by a viewport nobody anticipated, text
// overflowing a box that was fine until the string got longer, a node that
// collapsed to zero because its parent forgot to size itself.
//
// Those are exactly the faults that produce a screenshot where "the font
// looks broken" but the real cause is three levels up in the tree. So mayag
// makes them REPORTABLE: run the auditor over a laid-out tree and get a list
// of specific, located problems rather than a vague impression.
//
// This is a debug tool, not a runtime cost. Call it in tests, behind a flag,
// or from a key binding while developing.

#include "../scene/node.hpp"
#include "text_metrics.hpp"

#include <string>
#include <vector>

namespace mayag::layout {

struct Issue {
    enum class Kind : std::uint8_t {
        collapsed,        ///< zero or near-zero in an axis while having content
        overflow,         ///< children extend past the parent's box
        shrunk_below_min, ///< an explicit size was not honoured
        text_clipped,     ///< a text node is smaller than its own string
        degenerate_wrap,  ///< a text box so narrow it wraps to ~1 char/line
        nan_geometry,     ///< non-finite numbers reached the frame
        dead_grow,        ///< grow() on an axis that is already pinned
        contradiction,    ///< min > max, or similar impossible constraints
    };

    Kind          kind = Kind::collapsed;
    std::uint64_t node_id = 0;    ///< 0 when the node is anonymous
    Rect          frame{};
    std::string   detail;

    [[nodiscard]] std::string message() const {
        std::string s;
        switch (kind) {
            case Kind::collapsed:        s = "node collapsed to zero size"; break;
            case Kind::overflow:         s = "children overflow their parent"; break;
            case Kind::shrunk_below_min: s = "explicit size was not honoured"; break;
            case Kind::text_clipped:     s = "text does not fit its box"; break;
            case Kind::degenerate_wrap:  s = "text box too narrow to wrap sanely"; break;
            case Kind::nan_geometry:     s = "non-finite geometry"; break;
            case Kind::dead_grow:        s = "grow() has no effect here"; break;
            case Kind::contradiction:    s = "impossible size constraints"; break;
        }
        if (!detail.empty()) s += " — " + detail;
        return s;
    }
};

/// Walk a laid-out tree and report layout faults.
///
/// `measurer` is optional; without it text-fit checks are skipped.
[[nodiscard]] inline std::vector<Issue> audit(const Node& root,
                                              const TextMeasurer* measurer = nullptr) {
    std::vector<Issue> issues;

    const auto visit = [&](auto&& self, const Node& n, const Node* parent) -> void {
        const Rect& f = n.frame();
        const Style& st = n.style();

        // ---- non-finite geometry --------------------------------------
        if (!num::is_finite(f.origin.x) || !num::is_finite(f.origin.y) ||
            !num::is_finite(f.size.x)   || !num::is_finite(f.size.y)) {
            issues.push_back(Issue{Issue::Kind::nan_geometry, st.id, f, {}});
            return;   // everything below is meaningless
        }

        // ---- collapsed ------------------------------------------------
        // A node with paint or children but no area is invisible, and almost
        // always a sizing mistake rather than an intent.
        const bool has_content = !n.children().empty() ||
                                 (n.kind() == NodeKind::text && !n.text().empty()) ||
                                 st.paints_anything();
        if (has_content && (f.width() < 0.5f || f.height() < 0.5f)) {
            issues.push_back(Issue{Issue::Kind::collapsed, st.id, f,
                "size " + std::to_string(static_cast<int>(f.width())) + "x" +
                std::to_string(static_cast<int>(f.height()))});
        }

        // ---- explicit size honoured -----------------------------------
        // With shrink defaulting to 0 this should never fire; it is here to
        // catch a regression in the flex solver, which is the kind of bug
        // that otherwise shows up as "the font looks wrong".
        if (st.layout.width.unit == Length::Unit::pixels && f.width() > 0.0f) {
            const float want = st.layout.width.value;
            if (f.width() < want - 0.5f) {
                issues.push_back(Issue{Issue::Kind::shrunk_below_min, st.id, f,
                    "asked for width " + std::to_string(static_cast<int>(want)) +
                    ", got " + std::to_string(static_cast<int>(f.width()))});
            }
        }
        if (st.layout.height.unit == Length::Unit::pixels && f.height() > 0.0f) {
            const float want = st.layout.height.value;
            if (f.height() < want - 0.5f) {
                issues.push_back(Issue{Issue::Kind::shrunk_below_min, st.id, f,
                    "asked for height " + std::to_string(static_cast<int>(want)) +
                    ", got " + std::to_string(static_cast<int>(f.height()))});
            }
        }

        // ---- dead grow -------------------------------------------------
        //
        // `grow()` on an element whose MAIN-axis size is already pinned is
        // dead code: the flex solver has nothing to distribute into it. The
        // DSL cannot catch this because "which axis is main" is the PARENT's
        // property and an element cannot see its parent — so it lands here,
        // where the tree is assembled and the answer is known.
        if (st.layout.grow > 0.0f && parent != nullptr) {
            const bool horizontal = parent->style().layout.axis == Axis::horizontal;
            const Length main_len = horizontal ? st.layout.width : st.layout.height;
            if (main_len.unit == Length::Unit::pixels) {
                issues.push_back(Issue{Issue::Kind::dead_grow, st.id, f,
                    std::string{"grow() with an explicit "} +
                    (horizontal ? "width" : "height") + " on the main axis"});
            }
        }

        // ---- impossible constraints ------------------------------------
        const auto contradictory = [](Length lo, Length hi) {
            return lo.unit == Length::Unit::pixels && hi.unit == Length::Unit::pixels &&
                   lo.value > hi.value;
        };
        if (contradictory(st.layout.min_width, st.layout.max_width)) {
            issues.push_back(Issue{Issue::Kind::contradiction, st.id, f, "min_width > max_width"});
        }
        if (contradictory(st.layout.min_height, st.layout.max_height)) {
            issues.push_back(Issue{Issue::Kind::contradiction, st.id, f, "min_height > max_height"});
        }

        // ---- text fit --------------------------------------------------
        if (n.kind() == NodeKind::text && !n.text().empty() && measurer != nullptr) {
            const Rect inner = deflate(f, st.layout.padding);

            if (st.text.overflow == TextOverflow::wrap && inner.width() > 0.0f) {
                // A box narrower than a couple of glyphs cannot wrap into
                // anything readable.
                //
                // Whitespace-only content is exempt: it has nothing to wrap,
                // so a narrow box holding a single space is not a fault. That
                // was a false positive worth removing — an auditor that cries
                // wolf gets ignored, which costs more than the check is worth.
                // Count CODEPOINTS, not bytes. A single em-dash is 3 bytes of
                // UTF-8 but one glyph, and a box holding one glyph has nothing
                // to wrap — flagging it is a false positive. An auditor that
                // cries wolf gets ignored, which costs more than the check is
                // worth.
                std::size_t glyphs = 0;
                bool all_space = true;
                for (unsigned char ch : n.text()) {
                    if ((ch & 0xC0) != 0x80) ++glyphs;          // not a continuation byte
                    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch < 0x80) {
                        all_space = false;
                    } else if (ch >= 0x80) {
                        all_space = false;
                    }
                }

                const float one_line = measurer->advance("M", st.text);
                const bool trivial = all_space || glyphs <= 1;
                if (!trivial && inner.width() < one_line * 1.5f) {
                    issues.push_back(Issue{Issue::Kind::degenerate_wrap, st.id, f,
                        "width " + std::to_string(static_cast<int>(inner.width())) +
                        " cannot fit a glyph (" + std::to_string(static_cast<int>(one_line)) + ")"});
                }
            }

            // Text taller than its own box, in ANY overflow mode.
            //
            // Only the `clip` case used to be checked, which missed the far
            // more common fault: a parent that RESERVED a height using its
            // own font size while the text renders at a different one. That
            // is what made the todo app's input field 19.6 px tall around
            // 22.4 px of text — visibly clipped, and completely silent.
            const Vec2 needs = measurer->measure(
                n.text(), st.text,
                st.text.overflow == TextOverflow::wrap ? inner.width() : num::inf);

            if (needs.y > inner.height() + 1.0f) {
                issues.push_back(Issue{Issue::Kind::text_clipped, st.id, f,
                    "text is " + std::to_string(static_cast<int>(needs.y)) +
                    "px tall in a " + std::to_string(static_cast<int>(inner.height())) +
                    "px box"});
            }
            if (st.text.overflow == TextOverflow::clip && needs.x > inner.width() + 1.0f) {
                issues.push_back(Issue{Issue::Kind::text_clipped, st.id, f,
                    "needs " + std::to_string(static_cast<int>(needs.x)) +
                    "px, has " + std::to_string(static_cast<int>(inner.width()))});
            }
        }

        // ---- overflow --------------------------------------------------
        // Only reported for clipping or explicitly sized parents: a node that
        // sizes to its content is SUPPOSED to be as big as its children.
        if (!n.children().empty()) {
            // Only meaningful when the node has a size of its own to
            // overflow. A content-sized box is SUPPOSED to be as big as its
            // children.
            // A SCROLL VIEWPORT is supposed to hold more than it shows —
            // that is what scrolling IS — so overflow there is not a fault.
            const bool bounded = st.layout.scroll == nullptr &&
                                (st.clip ||
                                 st.layout.width.unit  == Length::Unit::pixels ||
                                 st.layout.height.unit == Length::Unit::pixels);
            (void)parent;
            if (bounded) {
                // Absolutely positioned children are included when the
                // parent CLIPS.
                //
                // Out of flow means "does not participate in sizing", not
                // "may be silently cropped". A clipping parent cuts every
                // child regardless of positioning, so excluding them here
                // left the most common overlay bug invisible: a caret or
                // label placed with absolute() inside a container whose
                // height was guessed rather than measured. That is exactly
                // how the todo app shipped an input field 19.6 px tall
                // around 22.4 px of text, and the auditor said nothing.
                Rect union_of_children{};
                for (const auto& c : n.children()) {
                    const bool in_flow = c.style().layout.position == Positioning::flow;
                    if (!in_flow && !st.clip) continue;
                    union_of_children = union_of_children.unite(c.frame());
                }
                const Rect inner = deflate(f, st.layout.padding);
                if (!union_of_children.empty() && !inner.empty()) {
                    const float dx = union_of_children.right()  - inner.right();
                    const float dy = union_of_children.bottom() - inner.bottom();
                    if (dx > 1.0f || dy > 1.0f) {
                        issues.push_back(Issue{Issue::Kind::overflow, st.id, f,
                            "by " + std::to_string(static_cast<int>(num::max(dx, 0.0f))) + "x" +
                            std::to_string(static_cast<int>(num::max(dy, 0.0f))) + "px"});
                    }
                }
            }
        }

        for (const auto& c : n.children()) self(self, c, &n);
    };

    visit(visit, root, nullptr);
    return issues;
}

/// Format an audit as human-readable lines.
[[nodiscard]] inline std::string format_issues(const std::vector<Issue>& issues,
                                               std::size_t limit = 20) {
    if (issues.empty()) return "layout: no issues\n";

    std::string out = "layout: " + std::to_string(issues.size()) + " issue(s)\n";
    for (std::size_t i = 0; i < issues.size() && i < limit; ++i) {
        const auto& is = issues[i];
        out += "  [" + std::to_string(static_cast<int>(is.frame.left())) + "," +
               std::to_string(static_cast<int>(is.frame.top())) + " " +
               std::to_string(static_cast<int>(is.frame.width())) + "x" +
               std::to_string(static_cast<int>(is.frame.height())) + "] " +
               is.message() + "\n";
    }
    if (issues.size() > limit) {
        out += "  ... and " + std::to_string(issues.size() - limit) + " more\n";
    }
    return out;
}

}  // namespace mayag::layout
