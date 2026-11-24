# Motion in mayag: time is the view's, frames are asked for

An animation never stalls and never spins, because every moving thing
answers one question in the same place: **when do I look different next?**

## Three kinds of time-driven visuals, one rule each

| what | example | how it is written | frames it costs |
|---|---|---|---|
| **stepped** | spinner glyph, countdown, blinking dots | `stepper(c.time, 12)`: returns the step and schedules the next | exactly `rate` a second |
| **ambient** | busy ring turning, status dot breathing | `ambient_time(c.time)`: time quantised to 30 Hz, next step scheduled | 30 a second, identical frames between |
| **followed** | a dragged tab, a sliding panel, a spring | `request_animation_frame()` while moving (`Ctx::track` springs do this themselves) | every refresh, only while it moves |

All three read **`Ctx::time`**, the frame's own clock. None of them reads a
model clock, starts a timer, or keeps a subscription alive.

A widget does not even TAKE a clock: it reads `frame_time(c)` from its
context (widget/control.hpp). Widgets used to take `double time`, and hand
passed its model clock three times over (tab spinner, sticky header, dialog
pulse) -- a clock that moved only while a frame subscription ran, so each
froze whenever nothing else animated. Lint `one_clock` refuses a widget
signature or props field carrying a clock.

On a foreign loop (macOS: the Cocoa run loop owns the thread) a booked step
must arm that loop's timer too: `window_host::wait_hint` returns it before
the external-loop early-out. It once returned first, and a spinner alone
drew no glyph for a whole second (test_embedded: "a spinner alone keeps its
own time").

## Why

The view is a pure function of the model **and the time of the frame**.
Something that changes only because time passes is not application state,
so the model must not have to carry a clock for it, and no message should
have to arrive for it to move.

The bug this rule removes: hand's tab spinner read `m.time`, a model clock
advanced only by `Tick` messages, and `Tick` was only subscribed while
*other* motion ran. So the spinner turned while a panel slid, then froze,
and it stuttered whenever ticks came irregularly. With the spinner
scheduling its own frames from `Ctx::time`, it turns by itself
(test_simulate: a program with **no subscription and no messages** shows 25
glyph changes in 2 s; before the fix it showed 1).

## How the runtime uses it

During `view()`, `stepper` and `ambient_time` call `request_frame_at(t)`,
and the earliest request wins. After the frame, the host keeps that time
(`change_at_`). `owes_frame()` and `wait_hint()` treat it like any other
deadline, the same wake computation as timers, input and frame
subscriptions. There is no second clock and no animation loop. When nothing
on screen moves, nothing is requested, and the app is idle at 0% CPU.

A memoised subtree that asked for a frame is never stored: a cache hit
would replay a frozen frame and drop its next step (`Ctx::memoize`).

## Model state that animates

Motion that *is* state (a panel opening, a scroll easing to its goal) still
lives in the model and steps on a `Tick` from `Sub::on_frame`, subscribed
only while it is unsettled. That is a different thing: the model changes.
The rule above is for visuals that change with time while the model does
not.

## Every frame changes a pixel

A frame the display did not need is pure cost: CPU, a present, battery, and
a busy loop the next real input waits behind. Two rules keep them out:

- **Still things take no frames.** A notice that shows for 3 s and then goes
  away does not move; it needs one frame to appear and a *timer*
  (`Cmd::after`) to leave, not 180 frames advancing a clock until it
  expires. hand's toast did exactly that (193 frames that changed nothing);
  so did its size HUD.
- **An animation ends where it stops showing.** Settle a fade or glide at
  the step the eye can't see, in the units it is drawn in: a quarter of a
  logical pixel for motion, not `0.004` rows (0.08 px); the pixel distance a
  trail stops being drawn, not a fixed fraction of a cell. Frames for a
  timed effect run exactly as long as it draws (a 0.2 s flash, not 0.45 s).
  Anything under a modal's scrim lands at once.

`hand --motion` (ctest `motion_economy`) holds both: it runs every animation
alone from rest to rest and fails if one never settles or more than a tenth
of its frames changed no pixel, naming who asked for them
(`window_host::frame_reason()`, and the app's own named reasons).
