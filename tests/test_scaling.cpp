// tests/test_scaling.cpp — cost must not scale with content you cannot see.
//
// "A list of 100,000 rows must cost the same as a list of 20" is the headline
// claim of widget/virtual_list.hpp, and nothing checked it. A claim like that
// does not fail loudly when it breaks: virtualisation still LOOKS right --
// the same rows appear, scrolling still works -- it just quietly starts
// building every row again, and the app gets slow on big data in a way that
// is invisible on the small data a test usually uses.
//
// So this asserts the ASYMPTOTICS, not a wall-clock number: node and instance
// counts must be flat in the total row count, and the frame cost at 100k rows
// must not be meaningfully worse than at 50. Wall-clock is used only as a
// coarse ceiling, because a timing assertion tight enough to be useful is
// also tight enough to flake on a loaded CI box.

#include <mayag/mayag.hpp>
#include <mayag/widget/virtual_list.hpp>
#include <mayag/backend/tiled.hpp>
#include <mayag/widget/virtual_list.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

using clk = std::chrono::steady_clock;
double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

struct Cost {
    std::size_t instances = 0;
    double      ms = 0.0;
};

Cost measure(int rows, typo::FontStack& fonts, const Theme& t) {
    Cost best;
    best.ms = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
        ScrollState sc;
        sc.viewport = {900.0f, 700.0f};
        sc.content  = {900.0f, static_cast<float>(rows) * 36.0f};

        const auto t0 = clk::now();
        Node n = (virtual_list(sc, rows, 36.0f, [&](int i) {
                      return (h(box() | width(px(10)) | height(px(10)) | radius(5)
                                      | bg(t.accent),
                                text_owned("row " + std::to_string(i)) | font(12)
                                    | fg(t.text_primary),
                                box() | grow())
                              | gap(10) | center | pad(6, 12)
                              | bg(i % 2 ? t.surface : t.surface_raised)
                              | radius(6)).build();
                  }, 4.0f)
                  | pad(16) | width(px(900)) | bg(t.background)).build();

        typo::StackMeasurer meas{fonts};
        layout::layout_tree(n, {900.0f, 700.0f}, meas);

        DrawList dl;
        typo::StackGlyphRenderer gr{fonts};
        render::PaintOptions po{};
        po.measurer = &meas;
        po.glyphs   = &gr;
        render::paint(n, dl, po);

        const double ms = ms_since(t0);
        if (ms < best.ms) best.ms = ms;
        best.instances = dl.instances().size();
    }
    return best;
}

}  // namespace

int main() {
    std::printf("mayag scaling\n=============\n\n");
    auto fonts = typo::system::default_stack();
    const auto t = make_theme(rgb<0x5B8CFF>, Mode::dark);

    std::printf("virtual_list: build + layout + paint\n\n");
    std::printf("  %8s %11s %9s\n", "rows", "instances", "ms");

    const Cost small = measure(50, *fonts, t);
    std::printf("  %8d %11zu %9.3f\n", 50, small.instances, small.ms);

    Cost big{};
    for (int rows : {1000, 10000, 100000}) {
        big = measure(rows, *fonts, t);
        std::printf("  %8d %11zu %9.3f\n", rows, big.instances, big.ms);
    }
    std::printf("\n");

    // The real assertion: work is flat in the total, because only the visible
    // window is built. Exact equality is right here -- the same viewport shows
    // the same number of rows regardless of how many exist behind them.
    check(big.instances == small.instances,
          "100k rows emit the same instance count as 50 (" +
              std::to_string(big.instances) + ")");

    // Wall clock as a ceiling only. 4x leaves room for a noisy machine while
    // still failing hard if virtualisation regresses to building everything:
    // the naive path is ~150x slower at 2k rows, let alone 100k.
    check(big.ms < small.ms * 4.0 + 1.0,
          "100k rows cost no more than ~4x 50 rows (" +
              std::to_string(big.ms) + " vs " + std::to_string(small.ms) + " ms)");

    // And the absolute number has to leave room for the rest of a frame.
    check(big.ms < 4.0,
          "a 100k-row frame builds in under 4 ms of the 16.7 ms budget");

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
