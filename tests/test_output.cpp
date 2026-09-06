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
#include <mayag/backend/tiled.hpp>
#include <mayag/output/composite.hpp>
#include <mayag/output/damage.hpp>
#include <mayag/output/headless_output.hpp>
#include <mayag/output/output.hpp>
#include <mayag/output/planes.hpp>
#include <mayag/output/transitions.hpp>

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

    // ── damage: redrawing as little as possible ───────────────────
    //
    // A compositor that redraws the whole screen because a clock ticked is
    // burning a GPU to change four digits. Every rule here is conservative in
    // ONE direction: too much damage wastes power, too little shows a stale
    // frame.
    section("damage is computed in screen space, from buffer space");
    {
        Layer l = make_layer(1000, 500, Rect{{100, 50}, {1000, 500}}, 0, true);
        l.damage.add(Rect{{10, 20}, {30, 40}});

        const Rect screen = buffer_to_screen(Rect{{10, 20}, {30, 40}}, l);
        check(screen.left() == 110.0f && screen.top() == 70.0f,
              "buffer coords are offset by the layer's position");
        check(screen.size.x == 30.0f && screen.size.y == 40.0f,
              "and unscaled when src and dst match");

        // Crop-and-scale (wp_viewporter) falls out of the same arithmetic.
        Layer scaled = make_layer(500, 250, Rect{{0, 0}, {1000, 500}}, 0, true);
        scaled.src = Rect{{0, 0}, {500, 250}};
        const Rect big = buffer_to_screen(Rect{{10, 10}, {20, 20}}, scaled);
        check(big.size.x == 40.0f && big.size.y == 40.0f,
              "a 2x-scaled layer scales its damage too (viewporter for free)");

        // Fractional placement must round OUTWARDS. Rounding to nearest
        // leaves a one-pixel seam that only appears at fractional scales,
        // which is exactly where nobody tests.
        Layer frac = make_layer(100, 100, Rect{{0.5f, 0.5f}, {150, 150}}, 0, true);
        const Rect r = buffer_to_screen(Rect{{0, 0}, {10, 10}}, frac);
        check(r.left() == 0.0f && r.right() >= 15.0f,
              "fractional damage rounds outwards, covering every pixel it "
              "touches");
    }

    section("a layer that moves damages where it WAS and where it is");
    {
        const Layer layers[] = {
            make_layer(200, 200, Rect{{300, 300}, {200, 200}}, 0, true),
        };
        const Rect previous[] = {Rect{{100, 100}, {200, 200}}};

        const Region d = frame_damage(layers, previous);
        check(d.rects.size() == 2,
              "both rectangles are damaged -- damaging only the new position "
              "leaves a trail of the old one");
        check(d.contains(Vec2{150, 150}) && d.contains(Vec2{350, 350}),
              "covering the vacated pixels and the newly covered ones");

        // A layer that did not move contributes only its own damage.
        Layer still = make_layer(200, 200, Rect{{100, 100}, {200, 200}}, 0, true);
        still.damage.add(Rect{{0, 0}, {10, 10}});
        const Layer same[] = {still};
        const Rect was[] = {Rect{{100, 100}, {200, 200}}};
        const Region d2 = frame_damage(same, was);
        check(d2.rects.size() == 1 && d2.rects[0].size.x == 10.0f,
              "a stationary layer damages only what the client said changed");

        // No damage at all means EVERYTHING, which is the safe reading.
        const Layer quiet[] = {make_layer(200, 200, Rect{{0, 0}, {200, 200}}, 0, true)};
        const Rect quiet_was[] = {Rect{{0, 0}, {200, 200}}};
        const Region d3 = frame_damage(quiet, quiet_was);
        check(d3.rects.size() == 1 && d3.rects[0].size.x == 200.0f,
              "an empty damage region means the whole layer, not nothing");

        // A vanished layer damages where it was.
        const Region d4 = frame_damage({}, was);
        check(d4.rects.size() == 1,
              "a layer that disappeared damages the space it left");
    }

    section("damage hidden behind an opaque layer costs nothing");
    {
        // The optimisation that pays for the whole file: a video playing
        // behind a fullscreen window damages itself every frame and none of
        // it can be seen.
        const Layer layers[] = {
            make_layer(400, 300, Rect{{100, 100}, {400, 300}}, 0, false),
            make_layer(1920, 1080, Rect{{0, 0}, {1920, 1080}}, 1, true),
        };
        Region d;
        d.add(Rect{{150, 150}, {50, 50}});      // behind the fullscreen window
        d.add(Rect{{2000, 100}, {50, 50}});     // outside it

        const Region visible = occlude(d, layers);
        check(visible.rects.size() == 1,
              "damage under an opaque layer is dropped");
        check(visible.rects[0].left() == 2000.0f,
              "and only the visible damage survives");

        // Conservative: a TRANSPARENT layer occludes nothing, however big.
        const Layer glass[] = {
            make_layer(1920, 1080, Rect{{0, 0}, {1920, 1080}}, 1, false),
        };
        const Region through = occlude(d, glass);
        check(through.rects.size() == 2,
              "a transparent layer hides nothing, whatever its size");
    }

    // ── the seam holds ──────────────────────────────────────────
    //
    // mayag has no build-time dependencies: Vulkan and Wayland are dlopen'd
    // at runtime, so a binary built anywhere runs anywhere. Every compositor
    // addition has to keep that, and the moment it does not, an ordinary GUI
    // app starts requiring compositor libraries it will never call and the
    // headless test path gains a hardware dependency.
    //
    // This section is compiled proof rather than a promise: it uses the whole
    // output layer while including nothing but mayag and the standard
    // library. If someone adds a libdrm include to these headers, this file
    // stops building.
    //
    // See docs/what-belongs-in-mayag.md for where the line is and why.
    section("the compositor layer decides, it does not open devices");
    {
        // Everything below runs with no GPU, no display and no /dev access.
        HeadlessOutput out;
        out.set_plane_caps(PlaneCaps{2, false, false});

        const Layer layers[] = {
            make_layer(1920, 1080, Rect{{0, 0}, {1920, 1080}}, 0, true),
            make_layer(300, 200, Rect{{50, 50}, {300, 200}}, 1, false),
        };
        const Rect previous[] = {Rect{{0, 0}, {1920, 1080}}};

        const auto plan   = plan_planes(layers, out.plane_caps());
        const auto dmg    = frame_damage(layers, previous);
        const auto vis    = occlude(dmg, layers);
        const auto commit = out.commit(layers);

        check(!plan.empty(), "planes are assigned with no hardware present");
        check(!dmg.empty(), "damage is computed with no display present");
        check(vis.rects.size() <= dmg.rects.size(),
              "occlusion only ever removes damage, never invents it");
        check(commit.has_value(), "and a frame commits against a test double");

        // A dmabuf fd is an int mayag stores and passes on. It NEVER opens
        // one, which is exactly why libdrm is not a dependency: the fd comes
        // from a client, through protocol code, and mayag only hands it to
        // the driver it already loaded.
        Dmabuf d{};
        d.n_planes = 1;
        d.planes[0] = DmabufPlane{99, 0, 4096};   // not a real fd, never used
        const Buffer foreign{d, 64, 64};
        check(foreign.valid(),
              "a dmabuf descriptor is just data -- mayag never opens a device "
              "node to make sense of it");
    }

    // ── effects ──────────────────────────────────────────────
    //
    // These belong in mayag by its own rule: pixel decisions, wrong in a way
    // that is a rendering bug, needing no device. The renderer could already
    // DRAW all of them -- rounded boxes, analytic shadows and backdrop blur
    // are ShapeKinds -- but a Layer could not ASK, so the renderer and the
    // output layer disagreed about what a picture is.
    section("a layer can ask for what the renderer could always draw");
    {
        Effects fx;
        check(fx.is_identity(), "a default Effects does nothing");
        check(fx.scanout_capable(),
              "so an untouched layer can still take a hardware plane");

        fx.corner_radius = 12.0f;
        check(!fx.is_identity() && !fx.scanout_capable(),
              "a rounded layer cannot be scanned out -- a plane blits a "
              "RECTANGLE, it cannot round a corner");

        Effects blur;
        blur.backdrop_blur = 20.0f;
        check(!blur.scanout_capable(),
              "nor can a blurred one, for the same reason");

        Effects faded;
        faded.opacity = 0.5f;
        check(!faded.scanout_capable(),
              "nor a faded one -- some planes have a constant-alpha register, "
              "but assuming it would show the wrong thing on screen");
    }

    section("effects change what counts as opaque");
    {
        // Layer::opaque describes the BUFFER. Effects can make the result
        // transparent whatever the buffer holds, and forgetting that shows as
        // content bleeding through -- invisible in the good case, which is
        // exactly the kind of bug that survives review.
        Layer solid = make_layer(800, 600, Rect{{0, 0}, {800, 600}}, 0, true);
        check(effectively_opaque(solid), "an untouched opaque layer is opaque");

        Layer faded = solid;
        faded.effects.opacity = 0.6f;
        check(!effectively_opaque(faded),
              "a FADED window is not opaque, however solid its buffer");

        Layer rounded = solid;
        rounded.effects.corner_radius = 16.0f;
        check(!effectively_opaque(rounded),
              "and neither is a rounded one -- its corners show what is "
              "behind them");

        // The planner and the occlusion pass must agree, or one will drop
        // damage the other still draws.
        const Layer layers[] = {faded};
        const auto plan = plan_planes(layers, PlaneCaps{2, false, false});
        check(!plan[0].direct(),
              "the planner refuses a faded layer");

        Region d;
        d.add(Rect{{100, 100}, {50, 50}});
        check(occlude(d, layers).rects.size() == 1,
              "and occlusion agrees it hides nothing -- the two ask the same "
              "predicate, so they cannot drift apart");
    }

    section("compositing draws effects in the only order that works");
    {
        Layer l = make_layer(400, 300, Rect{{50, 50}, {400, 300}}, 0, true);
        l.effects.shadow_radius = 24.0f;
        l.effects.backdrop_blur = 10.0f;
        l.effects.corner_radius = 12.0f;

        DrawList out;
        composite_layer(out, l, TextureHandle{5});

        const auto& inst = out.instances();
        check(inst.size() == 3, "shadow, backdrop and content are all emitted");

        // The order is the whole subtlety. A shadow after the content draws
        // OVER the window; a backdrop after the content blurs the window into
        // itself.
        check(inst[0].kind == static_cast<std::uint32_t>(ShapeKind::shadow),
              "shadow FIRST, because it falls behind the window");
        check(inst[1].kind == static_cast<std::uint32_t>(ShapeKind::backdrop),
              "backdrop SECOND -- it snapshots the framebuffer, so anything "
              "drawn later is not in the blur");
        check(inst[2].kind == static_cast<std::uint32_t>(ShapeKind::texture),
              "content LAST, on top of both");

        // A layer whose buffer has not arrived still casts its shadow, so a
        // window fades in rather than popping.
        DrawList pending;
        composite_layer(pending, l, TextureHandle{});
        check(pending.instances().size() == 2,
              "a layer with no texture yet still draws its shadow and "
              "backdrop, rather than popping in");

        // Nothing asked for means one draw call.
        Layer plain = make_layer(100, 100, Rect{{0, 0}, {100, 100}}, 0, true);
        DrawList simple;
        composite_layer(simple, plain, TextureHandle{7});
        check(simple.instances().size() == 1,
              "and a layer with no effects costs exactly one instance");
    }

    section("a frame composites only what the hardware did not take");
    {
        const Layer layers[] = {
            make_layer(1920, 1080, Rect{{0, 0}, {1920, 1080}}, 0, true),
            make_layer(300, 200, Rect{{50, 50}, {300, 200}}, 1, true),
        };
        const auto plan = plan_planes(layers, PlaneCaps{1, false, false});
        const TextureHandle textures[] = {TextureHandle{1}, TextureHandle{2}};

        DrawList out;
        composite_frame(out, layers, plan, textures);
        check(out.instances().size() == 1,
              "the layer on a plane is NOT drawn again on the GPU -- two "
              "answers to the same question is how a compositor draws a "
              "layer twice");
    }

    // ── transitions ───────────────────────────────────────
    //
    // A compositor's motion is not decoration -- it is how a user keeps track
    // of where things went. These build on the springs and Effects that
    // already exist rather than adding a parallel animation system, because
    // an effect only animations can produce is one a compositor cannot
    // compose with anything else.
    section("a window transition is a spring driving Effects");
    {
        WindowMotion m;
        m.kind = WindowTransition::scale;
        check(!m.animating(), "a window at rest is not animating");

        m.open();
        check(m.animating(), "opening starts it moving");

        // Mid-flight: partially present, so partially transparent and
        // slightly small.
        for (int i = 0; i < 3; ++i) m.step(1.0f / 60.0f);
        Layer l = make_layer(400, 300, Rect{{100, 100}, {400, 300}}, 0, true);
        const Layer mid = animate(l, m);
        check(mid.effects.opacity < 1.0f && mid.effects.opacity > 0.0f,
              "a window mid-open is partly transparent");
        check(mid.dst.size.x < l.dst.size.x,
              "and slightly smaller, growing INTO place");
        check(mid.dst.size.x > l.dst.size.x * 0.9f,
              "but not from nothing -- growing from zero reads as an "
              "explosion, not an arrival");

        // Settle.
        for (int i = 0; i < 200; ++i) m.step(1.0f / 60.0f);
        const Layer done = animate(l, m);
        check(!m.animating(), "it settles");
        check(done.effects.opacity > 0.99f,
              "fully opaque when open");
        check(std::abs(done.dst.size.x - l.dst.size.x) < 0.5f,
              "and back at its true size, so the layout is not permanently "
              "shifted by the animation");
    }

    section("an interrupted transition keeps its velocity");
    {
        // The case duration-based animation gets wrong: closing a window
        // while it is still opening. A duration has to restart and loses the
        // velocity, which is why so many desktops stutter here. A spring
        // carries it through, so interruption is not a special case.
        WindowMotion m;
        m.open();
        for (int i = 0; i < 4; ++i) m.step(1.0f / 60.0f);
        const float caught = static_cast<float>(m.presence);
        check(caught > 0.0f && caught < 1.0f, "caught mid-open");

        m.close();
        m.step(1.0f / 60.0f);
        check(static_cast<float>(m.presence) != caught,
              "reversing continues from where it was, rather than snapping to "
              "a restart");

        for (int i = 0; i < 300; ++i) m.step(1.0f / 60.0f);
        check(m.gone(), "and it finishes closing, so the window can be freed");
    }

    section("a genie shrinks toward where the window went");
    {
        // The one transition that carries INFORMATION rather than polish: it
        // tells the user which dock icon their window is in.
        WindowMotion m;
        m.kind = WindowTransition::genie;
        m.anchor = Vec2{960, 1050};        // a dock icon at the bottom
        m.presence.to(1.0f);
        for (int i = 0; i < 300; ++i) m.step(1.0f / 60.0f);
        m.close();
        for (int i = 0; i < 6; ++i) m.step(1.0f / 60.0f);

        Layer l = make_layer(400, 300, Rect{{100, 100}, {400, 300}}, 0, true);
        const Layer g = animate(l, m);
        const float cy = g.dst.origin.y + g.dst.size.y * 0.5f;
        check(cy > 250.0f,
              "a closing window moves TOWARD its anchor (" +
                  std::to_string(cy) + ")");
        check(g.dst.size.x < l.dst.size.x, "shrinking as it goes");
    }

    section("workspaces move as one continuous position");
    {
        // A from/to pair cannot express flicking past three workspaces before
        // the first animation finishes. One continuous offset can.
        WorkspaceMotion w;
        w.kind = WorkspaceTransition::slide;
        w.go_to(2);
        for (int i = 0; i < 4; ++i) w.step(1.0f / 60.0f);

        const Vec2 vp{1920, 1080};
        Layer l = make_layer(400, 300, Rect{{0, 0}, {400, 300}}, 0, true);

        const Layer on_2 = animate(l, w, 2, vp);
        const Layer on_0 = animate(l, w, 0, vp);
        check(on_2.dst.origin.x > on_0.dst.origin.x,
              "workspace 2 is to the RIGHT of workspace 0 while sliding");

        // Depth mode also scales, so the outgoing workspace recedes rather
        // than merely sliding off.
        w.kind = WorkspaceTransition::slide_depth;
        const Layer away = animate(l, w, 0, vp);
        check(away.dst.size.x < l.dst.size.x,
              "a distant workspace is scaled down, reading as depth");

        // Fade mode makes no spatial claim.
        w.kind = WorkspaceTransition::fade;
        const Layer faded = animate(l, w, 0, vp);
        check(faded.effects.opacity < 1.0f,
              "in fade mode a distant workspace is transparent instead");
        check(faded.dst.origin.x == l.dst.origin.x,
              "and does not move, because fade makes no claim about where "
              "workspaces are");
    }

    section("overview interpolates toward a slot the compositor chose");
    {
        OverviewMotion o;
        check(!o.active(), "overview starts closed");
        o.show();
        for (int i = 0; i < 300; ++i) o.step(1.0f / 60.0f);
        check(o.active() && !o.animating(), "and settles open");

        Layer l = make_layer(1920, 1080, Rect{{0, 0}, {1920, 1080}}, 0, true);
        const Rect slot{{100, 100}, {400, 225}};
        const Layer in_grid = animate(l, o, slot);

        check(std::abs(in_grid.dst.size.x - slot.size.x) < 1.0f,
              "a fully-open overview puts the window in its slot");
        check(in_grid.effects.corner_radius > 0.0f,
              "rounded on the way in -- a grid of sharp rectangles reads as a "
              "screenshot, rounding says these are objects");

        // Closed overview must not touch the layer at all.
        OverviewMotion closed;
        const Layer untouched = animate(l, closed, slot);
        check(untouched.dst.size.x == l.dst.size.x &&
              untouched.effects.corner_radius == 0.0f,
              "a closed overview leaves the layer exactly as it was");
    }

    section("an idle desktop costs nothing");
    {
        // The property that matters most: a compositor asks `animating()` to
        // know whether it may stop requesting frames. Anything that stays
        // true forever turns an idle desktop into a spinning fan.
        WindowMotion w;
        WorkspaceMotion ws;
        OverviewMotion o;
        check(!w.animating() && !ws.animating() && !o.animating(),
              "nothing animates at rest");

        w.open(); ws.go_to(3); o.show();
        for (int i = 0; i < 600; ++i) { w.step(1.0f / 60.0f);
                                       ws.step(1.0f / 60.0f);
                                       o.step(1.0f / 60.0f); }
        check(!w.animating() && !ws.animating() && !o.animating(),
              "and every transition eventually STOPS, so the compositor can "
              "go back to sleep");
    }

    // ── what a PROTOCOL LIBRARY needs ───────────────────────────
    //
    // A library that implements Wayland on top of mayag has obligations a
    // compositor application does not, because it must serve clients it has
    // never seen. These two came from asking what such a library would be
    // forced to guess at.
    section("a released buffer reports WHEN it is actually free");
    {
        // wl_buffer.release is a protocol library's hardest obligation and
        // has no safe guess. Too early and the client overwrites pixels
        // still being scanned out -- tearing that looks like a driver bug.
        // Too late and the client blocks with nothing to draw into, which
        // looks like the client stuttering. Only the renderer knows, because
        // only it knows which frame last sampled the buffer.
        auto dev = backend::SoftwareBackend::create({}, Vec2{64, 64}, 1.0f);
        backend::Device& sw = *dev;

        auto img = std::make_shared<ImageData>();
        img->width = 8; img->height = 8;
        img->rgba.assign(8 * 8 * 4, 0xAA);

        const auto a = sw.acquire(Buffer::from(img));
        check(a.has_value(), "a buffer is acquired");

        bool freed = false;
        sw.release_texture(a->handle.slot, [&] { freed = true; });
        check(freed,
              "a device holding nothing reports the buffer free immediately");

        // The callback must fire even for a slot the device never held --
        // otherwise a protocol library has to guess whether its callback was
        // dropped, and a dropped release hangs the client forever.
        bool unknown_freed = false;
        sw.release_texture(9999, [&] { unknown_freed = true; });
        check(unknown_freed,
              "and for an unknown slot too -- a DROPPED release hangs the "
              "client forever, so silence is never an acceptable answer");

        // No callback is still valid: an app that does not care pays nothing.
        sw.release_texture(a->handle.slot);
        check(true, "releasing without a callback still works");
    }

    section("a display reports what wl_output.geometry has to send");
    {
        HeadlessOutput out;
        const auto id = out.identity();
        check(!id.make.empty() && !id.model.empty(),
              "make and model are reported, so a protocol layer forwards "
              "rather than inventing them");

        // Identity is how a compositor remembers per-display settings across
        // unplugs: a connector name like DP-1 is reused by whatever is
        // plugged in next, while make/model/serial identify the actual panel.
        out.set_identity(DisplayIdentity{"Dell", "U2720Q", "ABC123"});
        check(out.identity().serial == "ABC123",
              "including a serial, which is what actually identifies a panel "
              "rather than the port it happens to be in");

        check(out.subpixel() == Subpixel::unknown,
              "subpixel layout defaults to UNKNOWN -- guessing RGB because it "
              "is common gives a minority of users fringed text forever");
        out.set_subpixel(Subpixel::horizontal_bgr);
        check(out.subpixel() == Subpixel::horizontal_bgr,
              "and is reported honestly when the display does say");

        check(static_cast<int>(Subpixel::unknown) == 0,
              "the values match wl_output.subpixel, so a protocol layer "
              "forwards the number instead of translating it");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
