# Threads in mayag: race-free and deadlock-free by construction

mayag never starts a thread or takes a lock of its own. Every thread, wait and
lock comes from jaal, and each kind of sharing has one type that makes the
unsafe version fail to compile, or fail on its first attempt. Nothing depends
on a test happening to hit a rare interleaving. (async-hazards.md covers the
other half: results that are no longer wanted.)

## Who runs where

| thread | runs | shares with others |
|--------|------|--------------------|
| the loop (one per runtime) | `update`, `view`, layout, paint, the atlas | nothing mutable: the model is the loop's alone |
| task / stream bodies (jaal pool) | app work | Sendable arguments in, messages out through a weak `Sink` |
| render helpers (`jaal::parallel_for`) | tile raster, encode | the frame being drawn, borrowed for one call |
| font worker (startup), GPU prep (Wayland) | discovery, device bring-up | one result, handed over once (`job_result`) |

Two runtimes may run at once, each on its own thread (a window and a headless
test). The ambient per-frame state (`ctx()`, the animation request, the
scroll-geometry flag, the theme memo, the `Style` free list) is per thread for
that reason. `tests/test_concurrency.cpp` proves two runtimes rendering at once
each get exactly their own pixels.

## The six rules

1. **Messages are Sendable.** Anything crossing threads (a message, a task
   or stream argument) must hold nothing the sender can still reach. That
   rules out pointers, views and `shared_ptr`. jaal checks it at compile time
   by walking every field; a value type with public fields (`Vec2`, `Rect`,
   `Color`) is proven field by field, with no claim needed. A type jaal can't
   see into opts in with `MAYAG_SENDABLE` / `MAYAG_FROZEN` / `MAYAG_SYNC`,
   beside the type, with the reason.

2. **Messages with hidden captures are loop-only.** `mayag::Edit` carries a
   callable whose captures jaal can't see. It may be built and folded on the
   loop, but `jaal::loop_only` keeps it off every Sink:
   - sending one from a task or stream does not compile;
   - sending a Msg that holds one aborts on the spot.
   
   So a `[&]` built on a worker can never reach the model.

3. **Shared data is frozen or Sync.** A value many threads read is a
   `Frozen<T>` (jaal::shared): built once, then const forever, never null.
   An object many threads call is a `CoOwned<T>` of a `Sync` T.

4. **Every lock is a leaf `Cell<T>` or a ranked `Guarded<T, Rank>`.**
   - A `Cell` (jaal::cell) has a captureless body whose arguments and result
     are Sendable, so it can only reach its own value. Taking any other lock
     inside it aborts on the first attempt, every time.
   - A `Guarded` lock needs a capability proving the caller holds only
     lower-ranked locks, which the compiler checks.
   
   Together that is a total lock order. A deadlock needs two locks taken in
   opposite orders, and neither type can express that. Blocking waits
   (`parallel_for`, `job_result::get_at_root`) abort if the caller holds a
   lock or is waiting on itself.

5. **Raw primitives are banned.** jaal's banlist runs over
   `include/mayag` (`mayag_runtime_banlist`). `std::thread`, mutexes,
   atomics, `thread_local`, futures and `const_cast` are allowed only where
   `tests/lint/allow_include.txt` grants them with a reason. On Linux what is
   left is the per-thread frame state above and three `const_cast`s at C APIs.
   `mayag_runtime_allowlist_tight` fails when a grant stops being needed.

6. **No hidden shared state.** A mutable `static` is shared by every runtime
   and thread in the process. Read-only data is `static const`, a shared
   cache is a `static Cell<T>` (the sheet cache is one: its sheets are
   `Frozen`, built whole and kept). Anything else is listed in
   `tests/lint/allow_statics.txt` with why. `mayag_shared_statics` fails on an
   unlisted static and on a stale grant. Before this rule, `sheets::tuned`
   kept an unguarded map that two runtimes raced on (56 TSan reports).

## Fonts, the one shared subsystem

A `FontStack` is shared by everything that measures and draws text.

- **Faces are immutable once added.** A run at another weight or width gets
  an instance of the variable face at that position (`instance_for`). It is
  built once, frozen, and kept. The shared face never moves, so threads
  shaping at different weights cannot see each other's position. (Before
  this, 366 of 800 results were wrong under contention.)
- **Lazy caches are Cells:** outlines, advances, shaped runs, plans, instances.
  Each entry is a pure function of its key, so racing fills agree.
- **The atlas has one drawer at a time.** It hands out pointers into itself,
  so it cannot be shared. A renderer holds a `DrawLease`, and a second one
  aborts at once with the fix. `stack.fork()` gives a second renderer its own
  atlas over the same faces. `render_to_pixels` borrows pooled forks, so it is
  safe from any thread.

## How it is checked

- `mayag_font_threads_tests`: two threads at two weights get exactly their own
  glyphs; renders through forks match a render alone; a second drawer aborts.
- `mayag_concurrency_tests`: fan-in, ordering, stale stream messages, keyed
  tasks, shutdown under load, and two runtimes rendering at once from a COLD
  start (process-wide caches filled concurrently).
- `mayag_*_tsan`: both of the above under ThreadSanitizer (on by default on
  Linux, `MAYAG_TSAN_TESTS`); any report fails the test.
- jaal's own suite: the `cell`, `guarded` and `loop_only` rules, each misuse
  pinned as a compile failure or a deterministic abort.

## Memory safety

Races are one kind of undefined behaviour; the others are checked too:

- **`MAYAG_SANITIZE=ON`** builds mayag and its whole test suite with
  AddressSanitizer + UBSan: use-after-free, out-of-bounds, leaks and
  undefined behaviour. Every test runs with the sanitizers halting on the
  first report. Leaks are suppressed only inside system libraries that keep
  their allocations to process exit (dbus, the Vulkan driver), by library, in
  `tests/lsan.supp`. A leak through mayag's own code is always reported.
- What it found, and is now fixed:
  - `DrawList::same_as` called `memcmp` on two empty vectors with null
    pointers. That is UB even for 0 bytes.
  - The shape-cache hash called `memcpy` from an empty view's (possibly
    null) data.
  - The Wayland window leaked the `wp_presentation` global and any
    presentation feedback still pending at teardown. Feedbacks are now
    owned by the window, so a late reply can never reach a freed window.
- hand runs the same check over its whole self-test, every screenshot scene,
  the flood and the resize repros (`cmake --preset asan`). It found a
  use-after-free in a screenshot script: a reference into a vector held
  across a `push_back`.
