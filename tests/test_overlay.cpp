// tests/test_overlay.cpp — menus, modals and tooltips must land on screen.
//
// The overlay layer is implemented across 10 headers and had NO tests. It is
// also the subsystem where bugs are least visible in review and most obvious
// to a user: a dropdown that hangs off the bottom of the window, a submenu
// that opens off the right edge, a modal that is not centred, a menu that
// stops dismissing when clicked outside.
//
// The geometry is the part worth pinning. Placement is a preference; what
// must ALWAYS hold is that the overlay ends up inside the viewport, because
// content the user cannot reach is worse than content in the wrong corner.

#include <mayag/mayag.hpp>

#include <cmath>
#include <cstdio>
#include <string>

using namespace mayag;

namespace {

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

constexpr Vec2 kViewport{800.0f, 600.0f};
constexpr float kGap = 4.0f;

bool inside(const Rect& r, Vec2 viewport) {
    return r.left() >= -0.01f && r.top() >= -0.01f &&
           r.right() <= viewport.x + 0.01f &&
           r.bottom() <= viewport.y + 0.01f;
}

}  // namespace

int main() {
    std::printf("mayag overlays\n==============\n");

    // ── the invariant that matters ───────────────────────────────────
    section("an overlay always lands inside the viewport");
    {
        // Sweep an anchor across the whole viewport, including hard against
        // every edge and corner, with overlays both small and awkwardly
        // large. Every result must be reachable.
        const Vec2 sizes[] = {{120, 90}, {300, 240}, {40, 20}, {200, 400}};
        const Placement places[] = {
            Placement::below_start, Placement::below_end,
            Placement::above_start, Placement::above_end,
            Placement::right_start, Placement::left_start,
            Placement::centered,    Placement::cursor,
        };

        int off_screen = 0, total = 0;
        Vec2 worst_pos{}, worst_size{};
        Placement worst_place = Placement::below_start;

        for (float ax = 0.0f; ax <= kViewport.x; ax += 97.0f) {
            for (float ay = 0.0f; ay <= kViewport.y; ay += 71.0f) {
                const Rect anchor{ax, ay, 60.0f, 24.0f};
                for (const Vec2 size : sizes) {
                    for (const Placement p : places) {
                        const Rect r =
                            overlay::resolve(p, anchor, size, kViewport, kGap, {ax, ay});
                        ++total;
                        if (!inside(r, kViewport)) {
                            ++off_screen;
                            worst_pos = r.origin; worst_size = size; worst_place = p;
                        }
                    }
                }
            }
        }

        if (off_screen != 0) {
            std::printf("    worst: placement=%d size=%.0fx%.0f -> pos=%.1f,%.1f\n",
                        static_cast<int>(worst_place), double(worst_size.x), double(worst_size.y),
                        double(worst_pos.x), double(worst_pos.y));
        }
        check(off_screen == 0,
              "all " + std::to_string(total) +
                  " placements land on screen (" + std::to_string(off_screen) +
                  " off)");
    }

    // ── flipping, which is the point of the clamp ────────────────────
    section("a dropdown with no room below opens above");
    {
        // An anchor near the bottom: below would overflow, above fits.
        const Rect anchor{100.0f, kViewport.y - 40.0f, 60.0f, 24.0f};
        const Vec2 size{120.0f, 200.0f};
        const Rect r = overlay::resolve(Placement::below_start, anchor, size,
                                        kViewport, kGap, {});
        check(r.bottom() <= kViewport.y + 0.01f,
              "it fits vertically");
        check(r.top() < anchor.top(),
              "and it opened ABOVE the anchor rather than being squashed");
    }

    section("a submenu with no room right opens left");
    {
        const Rect anchor{kViewport.x - 50.0f, 100.0f, 40.0f, 24.0f};
        const Vec2 size{200.0f, 120.0f};
        const Rect r = overlay::resolve(Placement::right_start, anchor, size,
                                        kViewport, kGap, {});
        check(r.right() <= kViewport.x + 0.01f, "it fits horizontally");
        check(r.left() < anchor.left(), "and it opened to the LEFT");
    }

    section("a modal is centred regardless of its anchor");
    {
        const Vec2 size{300.0f, 200.0f};
        const Rect far_anchor{700.0f, 550.0f, 20.0f, 20.0f};
        const Vec2 a = overlay::resolve(Placement::centered, far_anchor, size,
                                        kViewport, kGap, {}).origin;
        const Vec2 b = overlay::resolve(Placement::centered, Rect{0, 0, 10, 10}, size,
                                        kViewport, kGap, {}).origin;
        check(a.x == b.x && a.y == b.y, "the anchor does not move it");
        check(std::abs(a.x - (kViewport.x - size.x) * 0.5f) < 0.01f &&
              std::abs(a.y - (kViewport.y - size.y) * 0.5f) < 0.01f,
              "and it is actually centred");
    }

    // ── the degenerate case ──────────────────────────────────────────
    section("an overlay larger than the viewport is still reachable");
    {
        // Nothing can make this fit. What must NOT happen is a negative
        // origin, which would put the top-left -- the part with the title and
        // the close button -- outside the window entirely.
        const Vec2 size{1000.0f, 900.0f};
        const Rect anchor{400.0f, 300.0f, 60.0f, 24.0f};
        for (const Placement p : {Placement::below_start, Placement::centered,
                                  Placement::right_start}) {
            const Rect r = overlay::resolve(p, anchor, size, kViewport, kGap, {});
            check(r.left() >= -0.01f && r.top() >= -0.01f,
                  "oversized overlay keeps its origin on screen (placement " +
                      std::to_string(static_cast<int>(p)) + ")");
        }
    }

    section("a context menu opens at the cursor");
    {
        const Vec2 cursor{250.0f, 180.0f};
        const Vec2 size{140.0f, 100.0f};
        const Rect r = overlay::resolve(Placement::cursor, Rect{0, 0, 0, 0}, size,
                                        kViewport, kGap, cursor);
        check(std::abs(r.left() - cursor.x) < 0.01f &&
              std::abs(r.top() - cursor.y) < 0.01f,
              "with room, it sits exactly at the pointer");

        // Near the corner it must slide back in, not hang off.
        const Vec2 corner{kViewport.x - 10.0f, kViewport.y - 10.0f};
        const Rect cr = overlay::resolve(Placement::cursor, Rect{0, 0, 0, 0}, size,
                                        kViewport, kGap, corner);
        check(inside(cr, kViewport),
              "and near a corner it slides back on screen");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
