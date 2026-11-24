// tests/test_font_threads.cpp — one FontStack, shared by threads.
//
// A FontStack is shared: render_to_pixels keeps one per process, and two
// runtimes (a headless test, a window) may render on different threads. So
// shaping through one stack from several threads at once must give each
// caller exactly what it would get alone -- for variable fonts too, where
// each call asks for its own weight.
//
// Checks the RESULT (glyph advances), not only that nothing crashed: a
// "pin the face's weight, then shape" race is a wrong answer, not a crash.
// Built a second time under -fsanitize=thread (mayag_font_threads_tsan).

#include <mayag/mayag.hpp>

#include <cstdio>
#include <csignal>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using namespace mayag;

namespace {
int fails = 0;
void ok(bool c, const char* what) {
    std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
    fails += c ? 0 : 1;
}

/// The width of `text` at `weight`, glyph by glyph (summed advances).
std::vector<float> advances(const typo::FontStack& s, const std::string& text, float weight) {
    typo::ShapeParams p;
    p.size_px = 24.0f;
    p.weight = weight;
    std::vector<float> out;
    const typo::ShapeResult r = s.shape_uncached(text, p);   // every call shapes: no cache hides the race
    for (const auto& g : r.glyphs) out.push_back(g.advance);
    return out;
}
}  // namespace

int main() {
    const char* candidates[] = {"/usr/share/fonts/inter/InterVariable.ttf",
                                "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf"};
    std::string path;
    for (const char* c : candidates)
        if (std::filesystem::exists(c)) { path = c; break; }
    if (path.empty()) {
        std::printf("SKIP  no variable font on this machine\n");
        return 0;
    }
    auto face = typo::Face::from_file(path);
    if (face == nullptr || !face->is_variable()) {
        std::printf("SKIP  %s is not a variable face\n", path.c_str());
        return 0;
    }
    typo::FontStack stack;
    stack.add(face);

    const std::string text = "Wavy Hamburgefonstiv 0123456789";
    const auto light = advances(stack, text, 300.0f);
    const auto heavy = advances(stack, text, 800.0f);
    {
        float sl = 0, sh = 0;
        for (float a : light) sl += a;
        for (float a : heavy) sh += a;
        std::printf("    %s: %zu axes, wght(300)=%g wght(800)=%g, widths %.2f vs %.2f\n", path.c_str(), face->axes().size(),
                    static_cast<double>(face->wght_for(300.0f)), static_cast<double>(face->wght_for(800.0f)),
                    static_cast<double>(sl), static_cast<double>(sh));
    }
    ok(!light.empty() && light != heavy, "a variable face: weight 300 and 800 shape differently");

    // Each runtime renders on its own thread, through the SAME stack.
    constexpr int kRounds = 400;
    int wrong_light = 0, wrong_heavy = 0;   // each written by one thread only
    {
        std::jthread a([&] {
            for (int i = 0; i < kRounds; ++i) wrong_light += advances(stack, text, 300.0f) != light ? 1 : 0;
        });
        std::jthread b([&] {
            for (int i = 0; i < kRounds; ++i) wrong_heavy += advances(stack, text, 800.0f) != heavy ? 1 : 0;
        });
    }
    std::printf("    wrong results: %d of %d at 300, %d of %d at 800\n", wrong_light, kRounds, wrong_heavy, kRounds);
    ok(wrong_light == 0 && wrong_heavy == 0, "two threads at different weights each get their own answer");

    // ...and drawing: two renders on two threads, through the same stack
    {
        const auto ui = dsl::v(dsl::text<"Hamburgefonstiv 0123456789"> | dsl::font(28),
                               dsl::text<"Wavy text, two threads"> | dsl::font(18) | dsl::bold)
                        | dsl::pad(8) | dsl::bg(colors::black);
        RenderOptions o;
        o.fonts = std::shared_ptr<typo::FontStack>(&stack, [](typo::FontStack*) {});
        const auto alone = render_to_pixels(ui, Vec2{360, 120}, o);
        int da = 0, db = 0;   // one counter per thread
        {   // each renderer draws through its own fork: same faces, own atlas
            RenderOptions oa = o, ob = o;
            oa.fonts = stack.fork();
            ob.fonts = stack.fork();
            std::jthread a([&] { for (int i = 0; i < 40; ++i) da += render_to_pixels(ui, Vec2{360, 120}, oa) != alone ? 1 : 0; });
            std::jthread b([&] { for (int i = 0; i < 40; ++i) db += render_to_pixels(ui, Vec2{360, 120}, ob) != alone ? 1 : 0; });
        }
        const int differ = da + db;
        std::printf("    renders that differed from a render alone: %d of 80\n", differ);
        ok(differ == 0, "two threads rendering through forks of one stack each get the same pixels");
        RenderOptions none;   // no stack given: each call borrows a fork from the pool
        int pa = 0, pb = 0;
        const auto base = render_to_pixels(ui, Vec2{360, 120}, none);
        {
            std::jthread a([&] { for (int i = 0; i < 20; ++i) pa += render_to_pixels(ui, Vec2{360, 120}, none) != base ? 1 : 0; });
            std::jthread b([&] { for (int i = 0; i < 20; ++i) pb += render_to_pixels(ui, Vec2{360, 120}, none) != base ? 1 : 0; });
        }
        const int differ_pool = pa + pb;
        ok(differ_pool == 0, "render_to_pixels with no stack is safe from two threads (pooled forks)");
        // two renderers on ONE atlas: refused at once, with the reason
        const pid_t pid = ::fork();
        if (pid == 0) {
            std::freopen("/dev/null", "w", stderr);
            const typo::FontStack::DrawLease first{stack};
            const typo::FontStack::DrawLease second{stack};
            std::_Exit(0);
        }
        int st = 0;
        ::waitpid(pid, &st, 0);
        ok(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "a second renderer on one stack's atlas aborts at once (never corrupts)");
    }

    std::printf(fails ? "FAIL\n" : "PASS\n");
    return fails;
}
