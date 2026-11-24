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
