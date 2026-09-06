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
#include <cmath>
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

    // ── gaps found by actually writing a compositor frame loop ──────
    //
    // The abstractions above were designed, then used. These three are what
    // the using found -- each one blocked a line of the loop, and none was
    // visible from the design alone.
    section("what a compositor advertises is the INTERSECTION of two lists");
    {
        // The GPU decides what it can sample; the display decides what it can
        // scan out. They are different hardware. A client wants a buffer that
        // satisfies both, and neither list alone answers that.
        const FormatModifier gpu[] = {
            {formats::argb8888, Modifier::linear, false},
            {formats::abgr8888, static_cast<Modifier>(0x1234), false},
            {formats::xrgb8888, Modifier::linear, false},
        };
        const FormatModifier display[] = {
            {formats::argb8888, Modifier::linear, false},
            {formats::xrgb8888, static_cast<Modifier>(0x9999), false},
        };

        const auto both = common_formats(gpu, display);
        check(both.size() == 1,
              "only pairs on BOTH lists survive (" +
                  std::to_string(both.size()) + ")");
        check(both[0].format == formats::argb8888 &&
              both[0].modifier == Modifier::linear,
              "and it is the pair both sides named");
        check(both[0].scanout,
              "marked scanout, so a caller need not intersect again to know it "
              "is on the fast path");

        // The MODIFIER is part of the match, not decoration. Same format,
        // different tiling, is a different buffer layout entirely -- treating
        // them as equal would advertise a pair the display cannot scan out.
        const FormatModifier same_fmt[] = {
            {formats::xrgb8888, Modifier::linear, false},
        };
        const auto none = common_formats(same_fmt, display);
        check(none.empty(),
              "a format both support with DIFFERENT modifiers does not match");
    }

    section("an imported buffer can be composited, not only scanned out");
    {
        // Planes are few, so most layers lose and must be composited. Import
        // returned a TextureHandle, DrawList indexed textures by id, and
        // nothing joined the two -- zero-copy could feed direct scanout and
        // nothing else, which is exactly backwards: the fallback is the path
        // that must never be missing.
        DrawList list;
        const TextureHandle imported{7};
        list.imported(Rect{{0, 0}, {800, 600}}, imported);
        check(list.instances().size() == 1,
              "an imported buffer can be drawn");
        check(list.batches().size() == 1 && list.batches()[0].texture == 7,
              "and batches on its own texture id, like any other image");

        // An invalid handle draws nothing rather than sampling slot 0, which
        // is the glyph atlas -- a compositor that lost an import would
        // otherwise paint text atlas garbage over a client's window.
        DrawList empty;
        empty.imported(Rect{{0, 0}, {10, 10}}, TextureHandle{});
        check(empty.instances().empty(),
              "an invalid handle draws NOTHING, rather than sampling the "
              "atlas in slot 0");
    }

    section("a frame reports when the NEXT one is due");
    {
        HeadlessOutput out;   // 60 Hz
        const auto r = out.commit({});
        check(r.has_value(), "a commit with no layers still presents");
        check(r->next_vblank > r->time,
              "and predicts the next vblank AFTER this one");

        const double period = r->next_vblank - r->time;
        check(std::abs(period - 1.0 / 60.0) < 1e-9,
              "one refresh period ahead at 60 Hz (" +
                  std::to_string(period * 1000.0) + " ms)");

        // Without this a client can only pace to when its LAST frame landed,
        // which drifts and under load collapses into rendering frames nobody
        // sees.
        const auto r2 = out.commit({});
        check(r2->time >= r->next_vblank - 1e-9,
              "and the next frame lands at the predicted time");
    }

    // ── what a protocol implementation must NOT have to write ───────
    //
    // The seam is: protocol code deals with protocol, mayag deals with
    // pixels. Each of these was something a compositor author would
    // otherwise reimplement -- and get subtly wrong, because they are all
    // coordinate or byte-order problems with a plausible wrong answer.
    section("transforms are a graphics concern, not a protocol one");
    {
        check(swaps_axes(Transform::rotate_90),
              "a 90-degree buffer swaps width and height");
        check(swaps_axes(Transform::flipped_270),
              "and so does a flipped 270");
        check(!swaps_axes(Transform::rotate_180),
              "180 does not");
        check(!swaps_axes(Transform::flipped),
              "nor does a plain flip");

        Layer l;
        check(l.transform == Transform::normal,
              "a layer is untransformed until said otherwise, so a compositor "
              "that ignores rotation still works on unrotated displays");
    }

    section("opacity is a REGION, so rounded windows keep their fast path");
    {
        // A window with rounded corners is opaque everywhere except four
        // small arcs. With opacity as a bool it must declare itself fully
        // transparent, and the compositor redraws everything behind it every
        // frame for the sake of a few dozen pixels.
        Layer rounded = make_layer(800, 600, Rect{{0, 0}, {800, 600}}, 0, false);
        rounded.opaque_region.add(Rect{{0, 0}, {800, 600}});

        Layer on_top = make_layer(400, 300, Rect{{0, 0}, {400, 300}}, 1, false);
        const Layer layers[] = {rounded, on_top};

        const auto plan = plan_planes(layers, PlaneCaps{4, false, false});
        check(plan[0].direct(),
              "a layer whose OPAQUE REGION covers it can take a plane even "
              "though its opaque flag is false");
        check(!plan[1].direct(),
              "while one with no opaque region still cannot");

        Region r;
        check(r.empty(), "an empty region is empty");
        r.add(Rect{{10, 10}, {100, 50}});
        r.add(Rect{{200, 0}, {50, 200}});
        check(r.bounds().left() == 10.0f && r.bounds().right() == 250.0f,
              "and bounds() spans every rect in it");
        check(r.contains(Vec2{20, 20}) && !r.contains(Vec2{150, 100}),
              "contains() asks the rects, not the bounds");

        Region zero;
        zero.add(Rect{{0, 0}, {0, 50}});
        check(zero.empty(),
              "a degenerate rect is not added -- a zero-area damage rect from "
              "a client must not look like real damage");
    }

    section("damage defaults to EVERYTHING, which is the safe direction");
    {
        Layer l;
        check(l.damage.empty(),
              "a layer with no damage means 'all of it changed'");
        // The asymmetry is deliberate: believing nothing changed shows a
        // stale frame, while redrawing too much only wastes power.
    }

    section("the cursor moves without costing a frame");
    {
        HeadlessOutput out;
        check(out.has_cursor_plane(), "a display reports its cursor plane");
        check(out.max_cursor_size().x > 0.0f,
              "and the largest cursor it accepts");

        const auto before = out.sequence();
        Cursor c;
        c.buffer = make_layer(64, 64, Rect{{0, 0}, {64, 64}}, 0, true).buffer;
        c.hotspot = Vec2{4, 4};

        // Twenty pointer motions.
        bool all_moved = true;
        for (int i = 0; i < 20; ++i) {
            c.position = Vec2{static_cast<float>(i * 10), 100.0f};
            if (!out.set_cursor(c).has_value()) all_moved = false;
        }
        check(all_moved, "twenty pointer motions all succeed");
        check(out.cursor_updates() == 20, "all twenty moves landed");
        check(out.sequence() == before,
              "and NOT ONE cost a frame -- compositing the cursor instead is "
              "the most visible latency mistake a compositor can make");
        check(out.cursor().hotspot.x == 4.0f,
              "the hotspot travels with it, so the tip lands on the pointer "
              "position rather than the corner");

        // A cursor too big for the plane must be REFUSED, not silently
        // scaled, so the compositor knows to composite it instead.
        Cursor big;
        big.buffer = make_layer(512, 512, Rect{{0, 0}, {512, 512}}, 0, true).buffer;
        const auto r = out.set_cursor(big);
        check(!r.has_value() && r.error() == CommitError::buffer_unusable,
              "an oversized cursor is refused so the compositor can fall back");

        out.set_max_cursor_size(Vec2{0, 0});
        check(!out.has_cursor_plane(),
              "a display with no cursor plane says so, rather than failing "
              "mysteriously");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
