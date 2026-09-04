// tests/test_wayland.cpp — the Linux backend
//
// Two kinds of check live here, and the split matters because CI has no
// compositor:
//
//   * ALWAYS. The protocol tables, the evdev keymap, the damage arithmetic
//     and the graceful-degradation contract are pure logic. They run on a
//     build server with no Wayland, no GPU and no seat, and they are where the
//     bugs actually were — a wrong signature string or a shifted argument is
//     silent until it corrupts the wire.
//
//   * ONLY WITH A COMPOSITOR. Opening a real window is skipped, not failed,
//     when WAYLAND_DISPLAY is unset. A test that fails on a headless runner
//     teaches people to ignore it.

#include <mayag/mayag.hpp>
#include <mayag/platform/wayland.hpp>
#include <mayag/platform/wayland_protocol.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

using namespace mayag;
using namespace mayag::dsl;
using namespace mayag::platform;

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, std::string_view what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("  FAIL  %.*s\n", static_cast<int>(what.size()), what.data());
    }
}

void section(std::string_view name) {
    std::printf("\n%.*s\n", static_cast<int>(name.size()), name.data());
}

// ── protocol tables ─────────────────────────────────────────────────────
//
// A wl_interface is read by libwayland on every request and event. If the
// method count disagrees with the array, or a signature has the wrong arity,
// the failure is a wire desync inside the compositor — far from the cause.
// These assertions pin the transcription against the .xml.

void test_xdg_tables() {
    section("xdg-shell tables");

    if (!wl::lib().ok) {
        std::printf("  (libwayland absent \u2014 table checks still run)\n");
    }

    const auto& x = wl::xdg();

    check(std::string_view{x.wm_base.name} == "xdg_wm_base", "wm_base is named");
    check(x.wm_base.method_count == 4, "wm_base has 4 requests");
    check(x.wm_base.event_count == 1,  "wm_base has 1 event");
    check(std::string_view{x.wm_base.methods[wl::wm_base_get_xdg_surface].name}
              == "get_xdg_surface", "get_xdg_surface is at opcode 2");
    check(std::string_view{x.wm_base.methods[wl::wm_base_pong].name} == "pong",
          "pong is at opcode 3");
    check(std::string_view{x.wm_base.events[0].name} == "ping", "ping is event 0");

    check(std::string_view{x.surface.name} == "xdg_surface", "xdg_surface is named");
    check(x.surface.method_count == 5, "xdg_surface has 5 requests");
    check(std::string_view{x.surface.methods[wl::xdg_surface_ack_configure].name}
              == "ack_configure", "ack_configure is at opcode 4");

    check(std::string_view{x.toplevel.name} == "xdg_toplevel", "xdg_toplevel is named");
    check(x.toplevel.method_count == 14, "xdg_toplevel has 14 requests");
    check(x.toplevel.event_count == 4,   "xdg_toplevel has 4 events");
    check(std::string_view{x.toplevel.methods[wl::toplevel_set_title].name}
              == "set_title", "set_title is at opcode 2");

    // The version prefixes are the subtle part: without them libwayland parses
    // a v4 event with v1 rules and desyncs the connection.
    check(std::string_view{x.toplevel.events[0].signature} == "iia",
          "toplevel.configure signature is iia");
    check(std::string_view{x.toplevel.events[2].signature} == "4ii",
          "configure_bounds carries its since-version prefix");
    check(std::string_view{x.toplevel.events[3].signature} == "5a",
          "wm_capabilities carries its since-version prefix");

    // Every message must have exactly one `types` slot per argument, or
    // libwayland walks off the end of the array.
    auto arity = [](const char* sig) {
        int n = 0;
        for (const char* p = sig; *p != '\0'; ++p) {
            if (*p == '?') continue;                 // nullable marker
            if (*p >= '0' && *p <= '9') continue;    // since-version prefix
            ++n;
        }
        return n;
    };
    check(arity(x.wm_base.methods[wl::wm_base_get_xdg_surface].signature) == 2,
          "get_xdg_surface takes 2 arguments");
    check(arity(x.surface.methods[wl::xdg_surface_set_window_geometry].signature) == 4,
          "set_window_geometry takes 4 arguments");
    check(arity(x.toplevel.methods[wl::toplevel_resize].signature) == 3,
          "resize takes 3 arguments");

    // Extension protocols.
    check(std::string_view{x.deco_manager.name} == "zxdg_decoration_manager_v1",
          "decoration manager is named");
    check(std::string_view{x.deco.methods[wl::deco_set_mode].name} == "set_mode",
          "set_mode is at opcode 1");
    check(std::string_view{x.frac.events[0].name} == "preferred_scale",
          "fractional scale event is named");
    check(std::string_view{x.cursor_dev.methods[wl::cursor_dev_set_shape].signature}
              == "uu", "cursor set_shape takes (serial, shape)");

    // Interface cross-references must be wired, or a new_id is created with a
    // null interface and the object is unusable.
    check(x.wm_base.methods[wl::wm_base_get_xdg_surface].types[0] == &x.surface,
          "get_xdg_surface constructs an xdg_surface");
    check(x.surface.methods[wl::xdg_surface_get_toplevel].types[0] == &x.toplevel,
          "get_toplevel constructs an xdg_toplevel");
    check(x.deco_manager.methods[wl::deco_manager_get_toplevel].types[1] == &x.toplevel,
          "decoration is requested for a toplevel");
}

// ── keymap ──────────────────────────────────────────────────────────────

void test_evdev_keymap() {
    section("evdev keycodes");

    using wldetail::key_from_evdev;

    // Letters are POSITIONAL. Code 30 is the key left of the home row on every
    // layout; it is 'a' on QWERTY and that is what mayag means by Key::a.
    check(key_from_evdev(30) == Key::a, "code 30 is A");
    check(key_from_evdev(44) == Key::z, "code 44 is Z");
    check(key_from_evdev(16) == Key::q, "code 16 is Q");
    check(key_from_evdev(2)  == Key::n1, "code 2 is 1");
    check(key_from_evdev(11) == Key::n0, "code 11 is 0");

    check(key_from_evdev(1)   == Key::escape,    "code 1 is escape");
    check(key_from_evdev(28)  == Key::enter,     "code 28 is enter");
    check(key_from_evdev(14)  == Key::backspace, "code 14 is backspace");
    check(key_from_evdev(15)  == Key::tab,       "code 15 is tab");
    check(key_from_evdev(57)  == Key::space,     "code 57 is space");
    check(key_from_evdev(111) == Key::del,       "code 111 is delete");

    check(key_from_evdev(103) == Key::up,    "code 103 is up");
    check(key_from_evdev(108) == Key::down,  "code 108 is down");
    check(key_from_evdev(105) == Key::left,  "code 105 is left");
    check(key_from_evdev(106) == Key::right, "code 106 is right");

    // Both physical modifiers of a pair must map to the same logical key, or
    // right-shift silently stops working.
    check(key_from_evdev(42) == Key::shift && key_from_evdev(54) == Key::shift,
          "both shifts map to Key::shift");
    check(key_from_evdev(29) == Key::ctrl && key_from_evdev(97) == Key::ctrl,
          "both ctrls map to Key::ctrl");
    check(key_from_evdev(125) == Key::super && key_from_evdev(126) == Key::super,
          "both supers map to Key::super");

    check(key_from_evdev(59) == Key::f1 && key_from_evdev(88) == Key::f12,
          "function keys span f1..f12");

    // Unknown codes must be inert rather than aliasing a real key.
    check(key_from_evdev(0)     == Key::unknown, "code 0 is unknown");
    check(key_from_evdev(9999)  == Key::unknown, "out-of-range code is unknown");
}

// ── damage arithmetic ───────────────────────────────────────────────────

void test_damage_union() {
    section("damage union");

    using wldetail::union_rect;

    const Rect a{10, 10, 20, 20};
    const Rect b{50, 60, 10, 10};

    // Empty is the identity, so a fresh buffer accumulates correctly.
    check(union_rect(Rect{}, a) == a, "empty union x is x");
    check(union_rect(a, Rect{}) == a, "x union empty is x");

    const Rect u = union_rect(a, b);
    check(u.left() == 10 && u.top() == 10, "union keeps the min corner");
    check(u.right() == 60 && u.bottom() == 70, "union keeps the max corner");

    // Containment must not grow the rect.
    const Rect outer{0, 0, 100, 100};
    check(union_rect(outer, a) == outer, "union with a contained rect is stable");

    // Union is commutative; a damage set must not depend on visit order.
    check(union_rect(a, b) == union_rect(b, a), "union is commutative");
}

// ── graceful degradation ────────────────────────────────────────────────
//
// The contract that makes shipping this safe: a binary built WITH Wayland,
// run WITHOUT one, must return nullopt rather than crashing or aborting, so
// the runtime can fall back to headless.

void test_no_compositor_is_not_a_crash() {
    section("degradation");

    // Point the client at a socket that cannot exist.
    const char* saved = ::getenv("WAYLAND_DISPLAY");
    const std::string keep = saved != nullptr ? saved : "";

    ::setenv("WAYLAND_DISPLAY", "mayag-nonexistent-socket", 1);
    ::unsetenv("WAYLAND_SOCKET");

    WindowConfig cfg;
    cfg.title = "should not open";
    auto w = WaylandWindow::open(cfg);
    check(!w.has_value(), "a missing compositor yields nullopt, not a crash");

    if (!keep.empty()) ::setenv("WAYLAND_DISPLAY", keep.c_str(), 1);
    else               ::unsetenv("WAYLAND_DISPLAY");
}

// ── the real thing ──────────────────────────────────────────────────────

void test_live_window() {
    section("live compositor");

    const char* disp = ::getenv("WAYLAND_DISPLAY");
    if (disp == nullptr || *disp == '\0' || !wl::lib().ok) {
        std::printf("  (no compositor \u2014 skipped)\n");
        return;
    }

    WindowConfig cfg;
    cfg.title = "mayag test";
    cfg.size  = {640, 400};

    auto win = WaylandWindow::open(cfg);
    check(win.has_value(), "a window opens on a live compositor");
    if (!win) return;

    check(win->is_open(), "the window reports open");
    check(win->size().x > 0 && win->size().y > 0, "the window has a size");
    check(win->dpi_scale() > 0.0f, "the scale is positive");
    check(win->now() >= 0.0, "the clock runs");

    // A frame must actually reach the compositor.
    auto ui = v(text<"mayag on wayland"> | font(18) | fg(colors::white))
              | pad(24) | bg(rgb<0x0B0D10>);
    Node tree = ui.build();
    layout::layout_tree(tree, win->size(), layout::default_measurer());

    DrawList dl;
    render::paint(tree, dl, {});
    check(!dl.instances().empty(), "the frame has something in it");

    for (int i = 0; i < 3; ++i) {
        win->present(dl, rgb<0x0B0D10>);
        (void)win->poll_events(Wait::immediate, 0.0);
    }
    check(win->frames_presented() >= 1, "frames were presented");

    // Triple buffering: three presents must not all be dropped.
    check(win->frames_dropped() < 3, "buffers rotate rather than starving");

    // Read-back proves pixels were rendered, not just committed.
    const auto px = win->read_pixels();
    check(px.size() == static_cast<std::size_t>(win->size().x * win->dpi_scale()) *
                       static_cast<std::size_t>(win->size().y * win->dpi_scale()) * 4,
          "read_pixels returns a full RGBA frame");

    bool any_lit = false;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i] > 8 || px[i + 1] > 8 || px[i + 2] > 8) { any_lit = true; break; }
    }
    check(any_lit, "the frame is not blank");

    // Moving the window must not invalidate the listener pointers libwayland
    // holds — this is what the heap-pinned Impl exists to guarantee, and it
    // segfaulted before that fix.
    WaylandWindow moved = std::move(*win);
    check(moved.is_open(), "a moved window is still open");
    moved.present(dl, rgb<0x0B0D10>);
    const auto after = moved.poll_events(Wait::immediate, 0.0);
    (void)after;
    check(moved.frames_presented() >= 1, "a moved window still presents");
}

}  // namespace

int main() {
    std::printf("mayag wayland backend\n");
    std::printf("=====================\n");

    test_xdg_tables();
    test_evdev_keymap();
    test_damage_union();
    test_no_compositor_is_not_a_crash();
    test_live_window();

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
