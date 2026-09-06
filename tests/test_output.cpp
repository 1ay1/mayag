// Outputs: a display mayag drives directly, and which layers can skip the GPU.
//
// The Output concept is where a compositor differs from an app. A Window is
// given to you: fixed size, someone else's compositor, a present that cannot
// fail. An Output is OWNED: you choose the mode, you pick the buffers, and
// the kernel may refuse the whole configuration.
//
// That refusal is the reason commit() returns a result rather than void, and
// these tests exist mostly to pin that down — the fallback path is the
// least-exercised code in any compositor, because real hardware only refuses
// occasionally and never on request.
//
// The plane planner is the other half. A layer on a hardware plane costs the
// GPU nothing at all, which is the difference between a laptop lasting eight
// hours and five. mayag can decide this exactly, from the scene graph's own
// rects and opacity, where a compositor bolted onto a renderer has to
// reconstruct it from a flattened command stream.

#include <mayag/mayag.hpp>
#include <mayag/output/headless_output.hpp>
#include <mayag/output/output.hpp>
#include <mayag/output/planes.hpp>

#include <cstdio>
#include <string>

using namespace mayag;
using namespace mayag::output;

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %-4s  %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}
void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

/// A layer backed by real (CPU) pixels, sized to match its destination.
[[nodiscard]] Layer make_layer(int w, int h, Rect dst, int z, bool opaque) {
    auto img = std::make_shared<ImageData>();
    img->width = w; img->height = h;
    img->rgba.assign(static_cast<std::size_t>(w) * h * 4, 0xff);
    Layer l;
    l.buffer = Buffer::from(img);
    l.dst = dst;
    l.z = z;
    l.opaque = opaque;
    return l;
}

int main() {
    std::printf("output\n");

    // ── modes ────────────────────────────────────────────────────────
    section("a display advertises modes and one is current");
    {
        HeadlessOutput out;
        check(!out.modes().empty(), "a connected display has modes");
        check(out.current_mode().has_value(), "and one of them is set");
        check(out.modes()[0].preferred, "the preferred mode is marked");
        check(out.modes()[0].refresh_hz() == 60.0,
              "refresh is stored in millihertz so 59.94 stays distinct from 60 "
              "(" + std::to_string(out.modes()[0].refresh_hz()) + " Hz)");

        const auto bad = out.set_mode(DisplayMode{1234, 567, 60000, false});
        check(!bad.has_value(), "setting an unsupported mode fails");

        out.disconnect();
        check(!out.current_mode().has_value(), "an unplugged display has no mode");
        const auto after = out.commit({});
        check(!after.has_value() && after.error() == CommitError::disconnected,
              "and committing to it reports disconnected, not a generic error");
    }

    // ── commit can fail, and says why ────────────────────────────────
    section("commit returns a RESULT because refusal is normal");
    {
        HeadlessOutput out;
        auto l = make_layer(1920, 1080, Rect{{0, 0}, {1920, 1080}}, 0, true);
        const Layer layers[] = {l};

        const auto ok1 = out.commit(layers);
        check(ok1.has_value(), "a commit the hardware accepts succeeds");
        check(ok1->sequence == 1, "and reports a frame sequence");
        check(ok1->time > 0.0, "with the time it reached the screen");

        // The case that matters: the kernel refuses a layout. A void present
        // could not express this, and a compositor must respond by retrying
        // with fewer planes rather than treating it as fatal.
        out.fail_next(CommitError::unsupported_layout);
        const auto refused = out.commit(layers);
        check(!refused.has_value(), "a refused layout is reported, not ignored");
        check(refused.error() == CommitError::unsupported_layout,
              "distinguishably from a dead display, so the compositor knows to "
              "retry with a simpler configuration");

        const auto retry = out.commit(layers);
        check(retry.has_value(), "and the retry succeeds -- refusal is transient");

        Layer empty;
        const Layer bad[] = {empty};
        const auto r = out.commit(bad);
        check(!r.has_value() && r.error() == CommitError::buffer_unusable,
              "a layer with no buffer is buffer_unusable");
    }

    // ── plane assignment ─────────────────────────────────────────────
    section("layers that qualify are scanned out, costing the GPU nothing");
    {
        HeadlessOutput out;
        out.set_plane_caps(PlaneCaps{2, false, false});

        // A fullscreen opaque video, 1:1, is the ideal candidate.
        const Layer layers[] = {
            make_layer(1920, 1080, Rect{{0, 0}, {1920, 1080}}, 0, true),
        };
        const auto r = out.commit(layers);
        check(r.has_value() && r->direct_scanout == 1,
              "a fullscreen opaque 1:1 layer goes straight to a plane");

        // Scaled: without a hardware scaler this cannot be a plane.
        const Layer scaled[] = {
            make_layer(1920, 1080, Rect{{0, 0}, {1280, 720}}, 0, true),
        };
        const auto r2 = out.commit(scaled);
        check(r2.has_value() && r2->direct_scanout == 0,
              "a SCALED layer is composited when the hardware has no scaler");

        out.set_plane_caps(PlaneCaps{2, /*scaling=*/true, false});
        const auto r3 = out.commit(scaled);
        check(r3.has_value() && r3->direct_scanout == 1,
              "and goes to a plane once the hardware can scale");
    }

    section("planes go to the layers that save the most work");
    {
        // Only one plane, three candidates. The biggest must win: a plane
        // saves exactly the area it covers, so area is the value.
        const Layer layers[] = {
            make_layer(200, 200,   Rect{{0, 0},   {200, 200}},   2, true),
            make_layer(1920, 1080, Rect{{0, 0},   {1920, 1080}}, 0, true),
            make_layer(400, 300,   Rect{{50, 50}, {400, 300}},   1, true),
        };
        const auto plan = plan_planes(layers, PlaneCaps{1, false, false});
        check(direct_count(plan) == 1, "one plane means one direct layer");
        check(plan[1].direct(),
              "and it is the FULLSCREEN one, not the first in z-order");

        // Determinism: the same scene must plan the same way every time, or
        // the screen flickers in a way that cannot be reproduced.
        const auto again = plan_planes(layers, PlaneCaps{1, false, false});
        check(plan[0].plane == again[0].plane && plan[1].plane == again[1].plane &&
              plan[2].plane == again[2].plane,
              "planning is deterministic for the same scene");
    }

    section("transparency is respected when the hardware cannot blend");
    {
        // A transparent layer above another cannot use a plane without
        // hardware blending -- it would draw over instead of mixing.
        const Layer layers[] = {
            make_layer(800, 600, Rect{{0, 0}, {800, 600}}, 0, true),
            make_layer(400, 300, Rect{{0, 0}, {400, 300}}, 1, /*opaque=*/false),
        };
        const auto plan = plan_planes(layers, PlaneCaps{4, false, /*blending=*/false});
        check(plan[0].direct(), "the opaque bottom layer takes a plane");
        check(!plan[1].direct(),
              "the transparent one above it does NOT -- a wrong yes corrupts "
              "the screen, a wrong no costs a little power");

        const auto blended = plan_planes(layers, PlaneCaps{4, false, /*blending=*/true});
        check(blended[1].direct(),
              "and it does once the hardware can blend");
    }

    section("no planes means everything composites, which is always correct");
    {
        const Layer layers[] = {
            make_layer(1920, 1080, Rect{{0, 0}, {1920, 1080}}, 0, true),
        };
        const auto plan = plan_planes(layers, PlaneCaps{0, false, false});
        check(direct_count(plan) == 0, "with no planes, nothing is direct");
        check(plan.size() == 1,
              "but every layer still gets an answer, so the caller never has "
              "to guess what happened to one");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
