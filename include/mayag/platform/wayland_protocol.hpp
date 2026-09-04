#pragma once
// mayag::platform::wl — the Wayland wire protocol, without the dependency
//
// mayag links nothing. It parses its own fonts, encodes its own PNGs, and
// rasterises its own pixels, so a Linux backend that needs `pkg-config
// wayland-client` and a code generator would be the only place in the project
// where "just build it" stops being true.
//
// It does not need to be. libwayland-client.so.0 is present on every Wayland
// system by construction — it is what the compositor's own clients link — so
// this file resolves it with dlopen() at runtime instead of at link time.
// The consequences are worth stating plainly:
//
//   * no CMake find_package, no pkg-config, no wayland-scanner, no generated
//     sources in the build tree, no version skew between the .xml on the
//     build machine and the .so on the user's machine;
//   * a mayag binary built on a Wayland box still RUNS on an X11-only or
//     headless box — the loader fails, `open()` returns nullopt, and the
//     runtime falls back instead of dying at process start with a missing
//     shared object;
//   * one binary works across libwayland 1.19 → 1.26 with no rebuild.
//
// ── the one genuinely hard part ──────────────────────────────────────────
//
// libwayland exports the `wl_interface` descriptor tables for the CORE
// protocol (wl_compositor, wl_shm, wl_seat, ...) but NOT for xdg-shell.
// xdg-shell lives in wayland-protocols as an .xml file that each client is
// expected to run through wayland-scanner and compile into itself. There is
// no .so to dlopen for it.
//
// So this file builds those tables by hand, at runtime, in `XdgTables`. A
// `wl_interface` is a plain C struct — name, version, and arrays of
// `wl_message{name, signature, types}` — and libwayland only ever reads it.
// Writing five of them out is mechanical; what matters is that it is exact,
// because a wrong signature is a wire desync and a wrong opcode is a protocol
// kill. Every one below is transcribed from the stable xdg-shell.xml, and the
// opcode of each request is its index in the methods array, which is why the
// requests we never send are still present as placeholders.
//
// Signature grammar, since it is dense: "u" uint, "i" int, "s" string,
// "o" object, "n" new_id, "a" array, "f" fixed, "h" fd, "?" next arg may be
// null, and a LEADING DIGIT is the protocol version the message appeared in.
// `types` must have exactly one slot per argument; non-object args get null.

#include <cstddef>
#include <cstdint>

#if defined(__linux__) || defined(__FreeBSD__)

#include <dlfcn.h>

namespace mayag::platform::wl {

// ════════════════════════════════════════════════════════════════════════
// The libwayland ABI, redeclared
//
// These four structs are the entire data ABI of libwayland-client and have
// been stable since 1.0. Redeclaring them is what lets this file compile with
// no wayland headers installed.
// ════════════════════════════════════════════════════════════════════════

struct wl_interface;

struct wl_message {
    const char*                 name;
    const char*                 signature;
    const struct wl_interface** types;
};

struct wl_interface {
    const char*       name;
    int               version;
    int               method_count;
    const wl_message* methods;
    int               event_count;
    const wl_message* events;
};

struct wl_array {
    std::size_t size;
    std::size_t alloc;
    void*       data;
};

/// 24.8 signed fixed point — how Wayland sends sub-pixel coordinates.
using fixed_t = std::int32_t;

[[nodiscard]] constexpr double fixed_to_double(fixed_t f) noexcept {
    return static_cast<double>(f) / 256.0;
}

/// Every Wayland object is a `wl_proxy*`. Keeping them as void* rather than
/// inventing per-object C++ types keeps this layer honest: it is a transport,
/// and the type safety belongs one level up in the window class.
using proxy = void;

inline constexpr std::uint32_t marshal_flag_destroy = 1u;

// ── request opcodes ─────────────────────────────────────────────────────
//
// Named rather than inlined as integers: an opcode typo is a silent protocol
// desync that manifests as a crash in the compositor, which is the single
// worst failure mode this file can have.

enum : std::uint32_t {
    display_sync            = 0,
    display_get_registry    = 1,

    registry_bind           = 0,

    compositor_create_surface = 0,
    compositor_create_region  = 1,

    shm_create_pool         = 0,

    shm_pool_create_buffer  = 0,
    shm_pool_destroy        = 1,
    shm_pool_resize         = 2,

    buffer_destroy          = 0,

    surface_destroy             = 0,
    surface_attach              = 1,
    surface_damage              = 2,
    surface_frame               = 3,
    surface_set_opaque_region   = 4,
    surface_set_input_region    = 5,
    surface_commit              = 6,
    surface_set_buffer_transform= 7,
    surface_set_buffer_scale    = 8,
    surface_damage_buffer       = 9,

    region_destroy          = 0,
    region_add              = 1,

    seat_get_pointer        = 0,
    seat_get_keyboard       = 1,
    seat_release            = 3,

    pointer_set_cursor      = 0,
    pointer_release         = 1,

    keyboard_release        = 0,

    output_release          = 3,

    ddm_create_data_source  = 0,
    ddm_get_data_device     = 1,

    data_device_set_selection = 1,
    data_device_release       = 2,

    data_source_offer       = 0,
    data_source_destroy     = 1,

    data_offer_accept       = 0,
    data_offer_receive      = 1,
    data_offer_destroy      = 2,

    // xdg-decoration / fractional-scale / viewporter
    deco_manager_destroy        = 0,
    deco_manager_get_toplevel   = 1,
    deco_destroy                = 0,
    deco_set_mode               = 1,

    frac_manager_destroy        = 0,
    frac_manager_get_scale      = 1,
    frac_destroy                = 0,

    viewporter_destroy          = 0,
    viewporter_get_viewport     = 1,
    viewport_destroy            = 0,
    viewport_set_source         = 1,
    viewport_set_destination    = 2,

    cursor_mgr_destroy          = 0,
    cursor_mgr_get_pointer      = 1,
    cursor_dev_destroy          = 0,
    cursor_dev_set_shape        = 1,

    // xdg-shell
    wm_base_destroy         = 0,
    wm_base_get_xdg_surface = 2,
    wm_base_pong            = 3,

    xdg_surface_destroy             = 0,
    xdg_surface_get_toplevel        = 1,
    xdg_surface_set_window_geometry = 3,
    xdg_surface_ack_configure       = 4,

    toplevel_destroy          = 0,
    toplevel_set_title        = 2,
    toplevel_set_app_id       = 3,
    toplevel_move             = 5,
    toplevel_resize           = 6,
    toplevel_set_max_size     = 7,
    toplevel_set_min_size     = 8,
    toplevel_set_maximized    = 9,
    toplevel_unset_maximized  = 10,
    toplevel_set_fullscreen   = 11,
    toplevel_unset_fullscreen = 12,
    toplevel_set_minimized    = 13,
};

/// wl_shm pixel formats. Both are little-endian packed, so XRGB8888 is the
/// byte order B,G,R,X in memory. ARGB8888 is PREMULTIPLIED, which happens to
/// be exactly how mayag's framebuffer already stores colour.
enum : std::uint32_t {
    shm_format_argb8888 = 0,
    shm_format_xrgb8888 = 1,
};

enum : std::uint32_t { seat_cap_pointer = 1, seat_cap_keyboard = 2 };
enum : std::uint32_t { pointer_button_released = 0, pointer_button_pressed = 1 };
enum : std::uint32_t { keyboard_key_released = 0, keyboard_key_pressed = 1 };
enum : std::uint32_t { axis_vertical = 0, axis_horizontal = 1 };

/// zxdg_toplevel_decoration_v1 modes. Asking for `server_side` is what makes
/// a mayag window wear the compositor's own title bar and shadows instead of
/// being an undecorated rectangle the user cannot move.
enum : std::uint32_t { deco_mode_client_side = 1, deco_mode_server_side = 2 };

/// wp_cursor_shape_device_v1 shapes.
///
/// This protocol is why mayag needs no cursor-theme code at all. The classic
/// path is: load libwayland-cursor, find the XCURSOR_THEME, parse the theme
/// files, pick a size for the output scale, upload the image to a wl_buffer,
/// and attach it to a dedicated cursor surface — several hundred lines that
/// get the theme subtly wrong. Here the compositor already knows the user's
/// theme, so mayag names the shape and the right cursor appears.
enum : std::uint32_t {
    cursor_default = 1,  cursor_pointer = 4,  cursor_progress = 5,
    cursor_wait = 6,     cursor_crosshair = 8, cursor_text = 9,
    cursor_not_allowed = 15, cursor_grab = 16, cursor_grabbing = 17,
    cursor_ew_resize = 26, cursor_ns_resize = 27,
    cursor_nesw_resize = 28, cursor_nwse_resize = 29,
};

/// xdg_toplevel states, from the `configure` event's array.
enum : std::uint32_t {
    toplevel_state_maximized  = 1,
    toplevel_state_fullscreen = 2,
    toplevel_state_resizing   = 3,
    toplevel_state_activated  = 4,
};

// ════════════════════════════════════════════════════════════════════════
// The dynamically-resolved library
// ════════════════════════════════════════════════════════════════════════

struct Lib {
    void* handle = nullptr;
    bool  ok     = false;

    // display
    proxy* (*display_connect)(const char*)                       = nullptr;
    void   (*display_disconnect)(proxy*)                         = nullptr;
    int    (*display_get_fd)(proxy*)                             = nullptr;
    int    (*display_dispatch_pending)(proxy*)                   = nullptr;
    int    (*display_flush)(proxy*)                              = nullptr;
    int    (*display_roundtrip)(proxy*)                          = nullptr;
    int    (*display_prepare_read)(proxy*)                       = nullptr;
    int    (*display_read_events)(proxy*)                        = nullptr;
    void   (*display_cancel_read)(proxy*)                        = nullptr;
    int    (*display_get_error)(proxy*)                          = nullptr;

    // proxy
    /// The modern all-in-one marshaller (libwayland >= 1.19.91). Variadic, so
    /// it is declared exactly as libwayland declares it and called directly at
    /// each request site — there is no portable way to forward a `...`.
    proxy* (*proxy_marshal_flags)(proxy*, std::uint32_t, const wl_interface*,
                                  std::uint32_t, std::uint32_t, ...) = nullptr;
    int          (*proxy_add_listener)(proxy*, void (**)(void), void*) = nullptr;
    void         (*proxy_destroy)(proxy*)                              = nullptr;
    std::uint32_t(*proxy_get_version)(proxy*)                          = nullptr;

    // core interface tables, exported by libwayland
    const wl_interface* i_registry     = nullptr;
    const wl_interface* i_compositor   = nullptr;
    const wl_interface* i_shm          = nullptr;
    const wl_interface* i_shm_pool     = nullptr;
    const wl_interface* i_buffer       = nullptr;
    const wl_interface* i_surface      = nullptr;
    const wl_interface* i_region       = nullptr;
    const wl_interface* i_callback     = nullptr;
    const wl_interface* i_seat         = nullptr;
    const wl_interface* i_pointer      = nullptr;
    const wl_interface* i_keyboard     = nullptr;
    const wl_interface* i_output       = nullptr;
    const wl_interface* i_ddm          = nullptr;
    const wl_interface* i_data_device  = nullptr;
    const wl_interface* i_data_source  = nullptr;
    const wl_interface* i_data_offer   = nullptr;
};

namespace detail {

template <typename T>
inline bool bind_sym(void* h, T& fn, const char* name) noexcept {
    fn = reinterpret_cast<T>(dlsym(h, name));
    return fn != nullptr;
}

inline const wl_interface* bind_iface(void* h, const char* name) noexcept {
    return static_cast<const wl_interface*>(dlsym(h, name));
}

inline Lib load_lib() noexcept {
    Lib l;
    // The SONAME, not the .so symlink: the bare name only exists when the
    // -devel package is installed, which is exactly the dependency being
    // avoided here.
    l.handle = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (l.handle == nullptr) return l;

    void* h = l.handle;
    bool  good = true;
    good &= bind_sym(h, l.display_connect,          "wl_display_connect");
    good &= bind_sym(h, l.display_disconnect,       "wl_display_disconnect");
    good &= bind_sym(h, l.display_get_fd,           "wl_display_get_fd");
    good &= bind_sym(h, l.display_dispatch_pending, "wl_display_dispatch_pending");
    good &= bind_sym(h, l.display_flush,            "wl_display_flush");
    good &= bind_sym(h, l.display_roundtrip,        "wl_display_roundtrip");
    good &= bind_sym(h, l.display_prepare_read,     "wl_display_prepare_read");
    good &= bind_sym(h, l.display_read_events,      "wl_display_read_events");
    good &= bind_sym(h, l.display_cancel_read,      "wl_display_cancel_read");
    good &= bind_sym(h, l.display_get_error,        "wl_display_get_error");
    good &= bind_sym(h, l.proxy_marshal_flags,      "wl_proxy_marshal_flags");
    good &= bind_sym(h, l.proxy_add_listener,       "wl_proxy_add_listener");
    good &= bind_sym(h, l.proxy_destroy,            "wl_proxy_destroy");
    good &= bind_sym(h, l.proxy_get_version,        "wl_proxy_get_version");

    l.i_registry    = bind_iface(h, "wl_registry_interface");
    l.i_compositor  = bind_iface(h, "wl_compositor_interface");
    l.i_shm         = bind_iface(h, "wl_shm_interface");
    l.i_shm_pool    = bind_iface(h, "wl_shm_pool_interface");
    l.i_buffer      = bind_iface(h, "wl_buffer_interface");
    l.i_surface     = bind_iface(h, "wl_surface_interface");
    l.i_region      = bind_iface(h, "wl_region_interface");
    l.i_callback    = bind_iface(h, "wl_callback_interface");
    l.i_seat        = bind_iface(h, "wl_seat_interface");
    l.i_pointer     = bind_iface(h, "wl_pointer_interface");
    l.i_keyboard    = bind_iface(h, "wl_keyboard_interface");
    l.i_output      = bind_iface(h, "wl_output_interface");
    l.i_ddm         = bind_iface(h, "wl_data_device_manager_interface");
    l.i_data_device = bind_iface(h, "wl_data_device_interface");
    l.i_data_source = bind_iface(h, "wl_data_source_interface");
    l.i_data_offer  = bind_iface(h, "wl_data_offer_interface");

    good &= l.i_registry && l.i_compositor && l.i_shm && l.i_shm_pool &&
            l.i_buffer && l.i_surface && l.i_region && l.i_callback &&
            l.i_seat && l.i_pointer && l.i_keyboard && l.i_output;

    l.ok = good;
    return l;
}

}  // namespace detail

/// Resolved once per process. A failed load is cached as a failure, so a box
/// with no Wayland does not pay a dlopen on every window it tries to open.
[[nodiscard]] inline const Lib& lib() noexcept {
    static const Lib l = detail::load_lib();
    return l;
}

// ════════════════════════════════════════════════════════════════════════
// xdg-shell, transcribed
//
// Built lazily into a function-local static. Constructing at runtime rather
// than as constexpr data sidesteps the mutual references between these
// interfaces (wm_base names surface, surface names toplevel) which would
// otherwise be a static-initialisation-order problem in a header-only library.
// ════════════════════════════════════════════════════════════════════════

struct XdgTables {
    // Shared slab of nulls for messages whose arguments are all non-object.
    const wl_interface* null_types[8]{};

    const wl_interface* wm_base_positioner_t[1]{};   // create_positioner
    const wl_interface* wm_base_get_surface_t[2]{};  // get_xdg_surface
    const wl_interface* surf_toplevel_t[1]{};        // get_toplevel
    const wl_interface* surf_popup_t[3]{};           // get_popup
    const wl_interface* top_parent_t[1]{};           // set_parent
    const wl_interface* top_seat_t[1]{};             // show_window_menu
    const wl_interface* top_move_t[2]{};             // move
    const wl_interface* top_resize_t[3]{};           // resize
    const wl_interface* top_output_t[1]{};           // set_fullscreen

    wl_message wm_base_methods[4]{};
    wl_message wm_base_events[1]{};
    wl_message surface_methods[5]{};
    wl_message surface_events[1]{};
    wl_message toplevel_methods[14]{};
    wl_message toplevel_events[4]{};

    // Extension protocols. Optional at runtime: a compositor lacking any of
    // these still gets a correct window, just without that refinement.
    const wl_interface* deco_get_t[2]{};
    const wl_interface* frac_get_t[2]{};
    const wl_interface* viewport_get_t[2]{};
    const wl_interface* cursor_get_t[2]{};

    wl_message deco_manager_methods[2]{};
    wl_message deco_methods[3]{};
    wl_message deco_events[1]{};
    wl_message frac_manager_methods[2]{};
    wl_message frac_methods[1]{};
    wl_message frac_events[1]{};
    wl_message viewporter_methods[2]{};
    wl_message viewport_methods[3]{};
    wl_message cursor_mgr_methods[3]{};
    wl_message cursor_dev_methods[2]{};

    wl_interface wm_base{};
    wl_interface surface{};
    wl_interface toplevel{};
    wl_interface deco_manager{};
    wl_interface deco{};
    wl_interface frac_manager{};
    wl_interface frac{};
    wl_interface viewporter{};
    wl_interface viewport{};
    wl_interface cursor_mgr{};
    wl_interface cursor_dev{};

    XdgTables() {
        const Lib& L = lib();

        wm_base_positioner_t[0]  = nullptr;          // xdg_positioner: never created
        wm_base_get_surface_t[0] = &surface;
        wm_base_get_surface_t[1] = L.i_surface;
        surf_toplevel_t[0]       = &toplevel;
        top_parent_t[0]          = &toplevel;
        top_seat_t[0]            = L.i_seat;
        top_move_t[0]            = L.i_seat;
        top_resize_t[0]          = L.i_seat;
        top_output_t[0]          = L.i_output;

        // ── xdg_wm_base ──────────────────────────────────────────────────
        wm_base_methods[0] = {"destroy",           "",   null_types};
        wm_base_methods[1] = {"create_positioner", "n",  wm_base_positioner_t};
        wm_base_methods[2] = {"get_xdg_surface",   "no", wm_base_get_surface_t};
        wm_base_methods[3] = {"pong",              "u",  null_types};
        wm_base_events[0]  = {"ping",              "u",  null_types};

        wm_base = wl_interface{"xdg_wm_base", 6, 4, wm_base_methods, 1, wm_base_events};

        // ── xdg_surface ──────────────────────────────────────────────────
        surface_methods[0] = {"destroy",             "",     null_types};
        surface_methods[1] = {"get_toplevel",        "n",    surf_toplevel_t};
        surface_methods[2] = {"get_popup",           "n?oo", surf_popup_t};
        surface_methods[3] = {"set_window_geometry", "iiii", null_types};
        surface_methods[4] = {"ack_configure",       "u",    null_types};
        surface_events[0]  = {"configure",           "u",    null_types};

        surface = wl_interface{"xdg_surface", 6, 5, surface_methods, 1, surface_events};

        // ── xdg_toplevel ─────────────────────────────────────────────────
        toplevel_methods[0]  = {"destroy",          "",     null_types};
        toplevel_methods[1]  = {"set_parent",       "?o",   top_parent_t};
        toplevel_methods[2]  = {"set_title",        "s",    null_types};
        toplevel_methods[3]  = {"set_app_id",       "s",    null_types};
        toplevel_methods[4]  = {"show_window_menu", "ouii", top_seat_t};
        toplevel_methods[5]  = {"move",             "ou",   top_move_t};
        toplevel_methods[6]  = {"resize",           "ouu",  top_resize_t};
        toplevel_methods[7]  = {"set_max_size",     "ii",   null_types};
        toplevel_methods[8]  = {"set_min_size",     "ii",   null_types};
        toplevel_methods[9]  = {"set_maximized",    "",     null_types};
        toplevel_methods[10] = {"unset_maximized",  "",     null_types};
        toplevel_methods[11] = {"set_fullscreen",   "?o",   top_output_t};
        toplevel_methods[12] = {"unset_fullscreen", "",     null_types};
        toplevel_methods[13] = {"set_minimized",    "",     null_types};

        // The leading digits are protocol versions, not arguments. Omitting
        // them makes libwayland parse a v4 event as a v1 one and desync.
        toplevel_events[0] = {"configure",        "iia", null_types};
        toplevel_events[1] = {"close",            "",    null_types};
        toplevel_events[2] = {"configure_bounds", "4ii", null_types};
        toplevel_events[3] = {"wm_capabilities",  "5a",  null_types};

        toplevel = wl_interface{"xdg_toplevel", 6, 14, toplevel_methods, 4, toplevel_events};

        // show_window_menu / move / resize take (seat, serial, ...): only the
        // first slot is an object, the rest stay null.
        top_move_t[1]   = nullptr;
        top_resize_t[1] = nullptr;
        top_resize_t[2] = nullptr;

        // ── zxdg_decoration_manager_v1 ───────────────────────────────────
        deco_get_t[0] = &deco;
        deco_get_t[1] = &toplevel;

        deco_manager_methods[0] = {"destroy",                 "",   null_types};
        deco_manager_methods[1] = {"get_toplevel_decoration", "no", deco_get_t};
        deco_manager = wl_interface{"zxdg_decoration_manager_v1", 1, 2,
                                    deco_manager_methods, 0, nullptr};

        deco_methods[0] = {"destroy",    "",  null_types};
        deco_methods[1] = {"set_mode",   "u", null_types};
        deco_methods[2] = {"unset_mode", "",  null_types};
        deco_events[0]  = {"configure",  "u", null_types};
        deco = wl_interface{"zxdg_toplevel_decoration_v1", 1, 3, deco_methods,
                            1, deco_events};

        // ── wp_fractional_scale_manager_v1 ───────────────────────────────
        //
        // Without this, a 1.25x display forces a choice between rendering at
        // 1x and letting the compositor blur it, or rendering at 2x and
        // throwing away a third of the pixels. With it, mayag rasterises at
        // exactly the scale the output uses — which for an SDF renderer that
        // is resolution-independent by construction costs nothing.
        frac_get_t[0] = &frac;
        frac_get_t[1] = L.i_surface;

        frac_manager_methods[0] = {"destroy",              "",   null_types};
        frac_manager_methods[1] = {"get_fractional_scale", "no", frac_get_t};
        frac_manager = wl_interface{"wp_fractional_scale_manager_v1", 1, 2,
                                    frac_manager_methods, 0, nullptr};

        frac_methods[0] = {"destroy",         "",  null_types};
        frac_events[0]  = {"preferred_scale", "u", null_types};
        frac = wl_interface{"wp_fractional_scale_v1", 1, 1, frac_methods,
                            1, frac_events};

        // ── wp_viewporter ────────────────────────────────────────────────
        //
        // The other half of fractional scaling: the buffer is an integer
        // number of device pixels, and the viewport maps it onto a
        // fractionally-sized surface with no resample.
        viewport_get_t[0] = &viewport;
        viewport_get_t[1] = L.i_surface;

        viewporter_methods[0] = {"destroy",      "",   null_types};
        viewporter_methods[1] = {"get_viewport", "no", viewport_get_t};
        viewporter = wl_interface{"wp_viewporter", 1, 2, viewporter_methods,
                                  0, nullptr};

        viewport_methods[0] = {"destroy",         "",     null_types};
        viewport_methods[1] = {"set_source",      "ffff", null_types};
        viewport_methods[2] = {"set_destination", "ii",   null_types};
        viewport = wl_interface{"wp_viewport", 1, 3, viewport_methods, 0, nullptr};

        // ── wp_cursor_shape_manager_v1 ─────────────────────────────────
        cursor_get_t[0] = &cursor_dev;
        cursor_get_t[1] = L.i_pointer;

        cursor_mgr_methods[0] = {"destroy",            "",   null_types};
        cursor_mgr_methods[1] = {"get_pointer",        "no", cursor_get_t};
        cursor_mgr_methods[2] = {"get_tablet_tool_v2", "no", cursor_get_t};
        cursor_mgr = wl_interface{"wp_cursor_shape_manager_v1", 1, 3,
                                  cursor_mgr_methods, 0, nullptr};

        cursor_dev_methods[0] = {"destroy",   "",   null_types};
        cursor_dev_methods[1] = {"set_shape", "uu", null_types};
        cursor_dev = wl_interface{"wp_cursor_shape_device_v1", 1, 2,
                                  cursor_dev_methods, 0, nullptr};
    }
};

[[nodiscard]] inline const XdgTables& xdg() noexcept {
    static const XdgTables t;
    return t;
}

// ════════════════════════════════════════════════════════════════════════
// Request helpers
//
// Thin wrappers so the window code reads like the C API everyone knows,
// rather than like a wall of marshal_flags calls.
// ════════════════════════════════════════════════════════════════════════

[[nodiscard]] inline std::uint32_t version_of(proxy* p) noexcept {
    return lib().proxy_get_version(p);
}

inline int add_listener(proxy* p, const void* listener, void* data) noexcept {
    // The cast is what libwayland's own generated headers do: a listener is
    // an array of function pointers and libwayland indexes it by opcode.
    return lib().proxy_add_listener(
        p, reinterpret_cast<void (**)(void)>(const_cast<void*>(listener)), data);
}

inline void destroy(proxy* p) noexcept {
    if (p != nullptr) lib().proxy_destroy(p);
}

/// A request that creates an object.
///
/// libwayland's variadic marshaller expects the new_id argument to appear in
/// the argument list as a NULL placeholder, which it then fills in with the
/// proxy it creates. That placeholder is supplied HERE, exactly once, rather
/// than at each call site — passing it by hand is an easy mistake that shifts
/// every subsequent argument by one and produces a "null value passed for arg
/// N" marshalling error at runtime rather than a compile error.
template <typename... Args>
[[nodiscard]] inline proxy* request_new(proxy* p, std::uint32_t opcode,
                                       const wl_interface* iface,
                                       std::uint32_t version, Args... args) noexcept {
    return lib().proxy_marshal_flags(p, opcode, iface, version, 0u, nullptr, args...);
}

/// A request with no return value.
template <typename... Args>
inline void request(proxy* p, std::uint32_t opcode, Args... args) noexcept {
    (void)lib().proxy_marshal_flags(p, opcode, nullptr, version_of(p), 0u, args...);
}

/// A request that also destroys the proxy (the `destructor` in the .xml).
template <typename... Args>
inline void request_destroy(proxy* p, std::uint32_t opcode, Args... args) noexcept {
    (void)lib().proxy_marshal_flags(p, opcode, nullptr, version_of(p),
                                    marshal_flag_destroy, args...);
}

/// wl_registry.bind is the one irregular request: its new_id has no interface
/// fixed by the protocol, so the interface name and version go on the wire.
[[nodiscard]] inline proxy* registry_bind_to(proxy* registry, std::uint32_t name,
                                             const wl_interface* iface,
                                             std::uint32_t version) noexcept {
    return lib().proxy_marshal_flags(registry, registry_bind, iface, version, 0u,
                                     name, iface->name, version, nullptr);
}

}  // namespace mayag::platform::wl

#endif  // __linux__ || __FreeBSD__
