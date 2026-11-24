# mayag's platform layer

mayag draws the same pixels on macOS, Linux and Windows. What differs
between them is decided **once**, carried as **types and constants**, and
**checked by the build**. Nothing is re-derived in a second place.

## The four layers

| question | answered in | read as |
|---|---|---|
| what is this compiled **for**? (OS, compiler, CPU, vector unit) | `jaal/meta/target.hpp`, the only reader of `_WIN32`, `__clang__`, `__SSE2__` and the like | `MAYAG_OS_*` `MAYAG_CC_*` `MAYAG_CPU_*` `MAYAG_ISA_*`, `target::os` (`core/target.hpp`) |
| what was it built **with**? (renderers, window system) | `core/target.hpp`, the only reader of CMake's `MAYAG_WITH_*` | `MAYAG_GPU_*`; `MAYAG_WINDOW_*`, exactly one of cocoa, wayland, win32 or headless; `target::window` |
| what does the **user** ask of the GPU? | `backend/gpu_policy.hpp`, the only reader of `MAYAG_BACKEND`, `MAYAG_GPU_WAIT_MS`, `MAYAG_GPU_ASYNC` and `MAYAG_PRESENT` | `gpu_policy::allows(api)`, `prefers`, `software_only`, `first_frame_wait`, `present_mode` |
| what can **this window** do? | `platform/concepts.hpp`: `Window`, and 19 capabilities as concepts and as `cap::` types | `has<W, cap::x>` |

The build asks the same headers. CMake never branches on `WIN32` or
`APPLE`. It compiles a one-line check against `jaal/meta/target.hpp`
(`jaal_target_is`) and reads `MAYAG_SYSTEM`.

## Per-OS data is a table, not an `#if`

Anything that only *differs* by OS is a function of `target::os_kind`.
Every row compiles on every build, and a test checks every row from any
machine:

- `backend::preferred_order(os)`: Metal, D3D12, Vulkan or WebGPU first, software last
- `font::system::detail::font_directories(os, env)`, `cache_path(os, env)`
- `ui_font_families(os)`, `mono_…`, `emoji_…`, `cjk_…`, `desktop_script_conventions(os)`
- `typo::os_raster`: the OS's own glyph rasteriser (DirectWrite), declared on
  every platform, with `os_raster::available` a `constexpr bool`

An `#if` is reserved for code that **cannot compile** elsewhere (an OS
header, an OS call). It lives in `platform/`, `backend/` or a named seam
file, never in graphics code (lint `one_look`).

## Capabilities are types

```cpp
// the host asks one way, for any window:
if constexpr (platform::has<W, platform::cap::moves_image_rows>) window_.move_image_rows(...);

// a window's set is COMPUTED from its methods (exact signatures):
platform::capabilities_of<MacWindow>        // caps<cap::replaces_queued_frames, ...>
platform::capabilities_of<MacWindow>::names // {"replaces_queued_frames", ...}

// each backend STATES its set, one line after its class, and the build checks
// it both ways:
static_assert(check_capabilities<MacWindow, caps<cap::uploads_images, ...>>());
```

If a backend loses a method it states, or gains one it doesn't, the build
fails and names it:

```
mayag: a window lost a capability it states -- missing: moves_image_rows
mayag: a window has a capability it does not state -- unstated: repeats_keys
```

A method with the right name but the wrong signature is not the
capability. Before this, a drifted signature made `if constexpr` go false
and the feature silently vanished on one platform.

## Shared machines, not copies

| what every window does | written once in |
|---|---|
| keep `image(id)` on the CPU always, mirror it to the GPU device that presents | `platform/images.hpp`: `images::upload`, `release`, `replay` |
| prepare the GPU on a worker, let frame one wait a budget, take it over later | `platform/gpu_bringup.hpp`: `GpuBringUp<Device, Prepare>` |
| silence one compiler's false positive | `MAYAG_GCC_IGNORE` / `MAYAG_GCC_RESTORE` (`core/target.hpp`) |

## Adding a window backend

1. Write `platform/<name>_window.hpp` with a class that satisfies `Window`
   (it has `open`, `dispatch`, `present` and the rest; see `concepts.hpp`).
2. Give it whichever capability methods the system has, with the exact
   signatures in `concepts.hpp`.
3. State its set after the class:
   `static_assert(check_capabilities<NameWindow, caps<...>>());`
4. Keep images through `images::`, and bring the GPU up through
   `GpuBringUp<YourDevice, YourPrepare>`.
5. Add the system to `core/target.hpp` (`MAYAG_WINDOW_*`) and to the
   backend table in `CMakeLists.txt` (`_mayag_platform_backends`).

Nothing in the host, the widgets or the app changes.

## The lints that hold it

| lint | rule |
|---|---|
| `one_os` | no raw target macro outside the readers; no `MAYAG_WITH_*` outside `core/target.hpp`; a file testing a platform fact includes its header |
| `one_os_cmake` | no `if(WIN32 / APPLE / UNIX / CMAKE_SYSTEM_NAME)` in any CMakeLists |
| `one_look` | no platform conditional of any kind in graphics code |
| `one_gpu_policy` | the GPU knobs are read only in `gpu_policy.hpp` |
| `one_image_policy` | windows call `images::`, never a device's texture verbs |
| `one_gpu_bringup` | no window runs its own GPU worker |
| `one_capability` | every capability concept has a `cap::` tag in `cap::all`; the host asks only `has<>` |
| `one_list` | a widget that calls `scroll_view` virtualises (`visible_rows` / `virtual_list`) or is granted as bounded, with the reason |

Each lint is mutation-tested: a planted violation of each kind is caught.

## A frame is O(screen), never O(data)

Every lag bug so far had one shape: a frame whose cost grew with the data.
The file pane built every row of a folder (21 ms a scroll step at 3000
files). DrawList rescanned every instance for each glyph, then on every
backdrop restore. The editor rebuilt the file's symbol list on every key.
Three rules hold this shut:

- **Lists are virtual.** Uniform rows go through `visible_rows` /
  `virtual_list` (widget/virtual_list.hpp); lint `one_list`.
- **Proven by structure, not by feel.** `test_widget_scaling` builds each
  scrolling widget at 100 and 10000 items with the same viewport, and the
  node count must not grow. It also checks that DrawList stays linear under
  nested backdrops. Putting back the old file pane or the old restore fails
  it (100044 nodes; 49x the time for 8x the work).
- **Derived data is cached where it lives**, keyed on what it depends on.
  Examples: `FilePaneState::view_memo` for a sort, the syntax cache for
  bracket depth, `EditorState` for symbols, patched in place on an edit.
  It is never recomputed from all the data each frame.

And the UI thread asks the OS for interactive priority before any thread
starts (`platform/priority.hpp`), so background work on the machine cannot
starve input-to-frames.
