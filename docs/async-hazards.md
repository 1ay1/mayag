# Async state hazards, and which primitive fixes each

Every bug in this document was found in mayag's video example, reproduced with
a measurement, and fixed. They are written up together because they look
identical from the outside — "the UI doesn't update" / "I have to click twice"
— and have completely different causes.

The framing is deliberately not "concurrency bugs". Locks fix none of these.
Every one is about a question no lock can answer: **is this result still
wanted?**

---

## The four hazards

| # | Hazard | Question it answers wrongly | Fix |
|---|--------|-----------------------------|-----|
| 1 | Starvation under a gesture | "should the running task be cancelled?" | `Cmd::task_coalesce` |
| 2 | Out-of-order results | "does this result belong to my latest request?" | generation stamp + `settled` |
| 3 | Stale in-flight messages | "was this producer still alive when it posted?" | apply-time gating |
| 4 | Blocking teardown | "must I wait for the old worker to notice?" | retire + reap |

---

## 1. Starvation: cancelling the *running* task

`task_latest` means "newest wins": a new request supersedes the running one.
That is right for a **discrete** request — a search box, a save — where only
the final answer matters.

It is catastrophic for a **continuous** gesture. If requests arrive faster
than the work completes, every task is superseded before finishing, so
*nothing is ever delivered*:

    117 seek requests across a 2 s drag  ->  0 frames shown
    picture caught up 827 ms AFTER the mouse was released

Cancellation made it worse, not better: it killed each decode sooner.

The fix is a different policy — at most one in flight, the running one
**allowed to finish**, newer requests collapsing into a single pending slot:

```cpp
Cmd<Msg>::task_coalesce("seek", [t] { return decode_at(t); })
```

Results then land as fast as the work can sustain, and the last request always
lands because it is never discarded. Measured: 74 requests, 11 tasks ran, 11
results delivered, request #73 landed.

### This is the RxJS flattening taxonomy

The same four policies appear wherever async work is driven by a stream, most
explicitly in RxJS's higher-order mapping operators. The names differ; the
decision is identical:

| Policy | RxJS | mayag | Use for |
|---|---|---|---|
| Cancel the running one | `switchMap` | `Cmd::task_latest` | typeahead, discrete requests |
| Let it finish, queue newest | *(no direct equivalent)* | `Cmd::task_coalesce` | **drags, scrubs, live previews** |
| Run everything in parallel | `mergeMap` | `Cmd::task` | independent fire-and-forget |
| Queue all, preserve order | `concatMap` | — | ordered writes |
| Ignore new while busy | `exhaustMap` | — | submit-button debounce |

The Angular University guide puts the general rule well: *"choosing the wrong
operator might accidentally lead to subtle issues in our programs"*. The
subtle issue here is that `switchMap`/`task_latest` is the usual default and
is exactly wrong for gestures, because a gesture emits faster than the work
completes **by definition**.

`task_coalesce` is closest to `exhaustMap` + "remember the last one" — the
policy you actually want for a scrub bar and which no mainstream operator
expresses in one step.

---

## 2. Out-of-order results: coalescing breaks an invariant

Every other task policy upholds an assumption apps rely on without noticing:
*a result belongs to your most recent request*.

Coalescing breaks it. The running task is allowed to finish, so its result
arrives **after** a newer request was already recorded. An app that treats
each result as authoritative adopts a stale value:

    seek to the middle, then immediately to the start
    -> the middle's frame arrives first
    -> player reports it landed 864 s from where it was sent

Two fixes, both needed:

**Stamp results with the generation they were produced for.** The runtime
cannot do this for you — only the app knows what makes one result obsolete:

```cpp
struct Frame { ...; int epoch; };          // stamped by the producer

[&](Frame f) -> R {
    if (f.epoch != m.epoch) return {m, Cmd<Msg>::none()};   // drop
    ...
}
```

**Ask the runtime when a key has actually settled**, rather than inferring it
from "a result arrived":

```cpp
Cmd<Msg>::task_coalesce("seek", work, Msg{SeekSettled{}})
```

`settled` fires when the key has nothing running *and* nothing pending.

This is the same shape as React's `ignore` flag:

```js
useEffect(() => {
  let ignore = false;
  fetchData(query).then(r => { if (!ignore) setData(r); });
  return () => { ignore = true; };
}, [query]);
```

React scopes the flag to an effect closure; mayag stamps the message. Both
answer *"is this result still current?"* — which is the actual question, and
is not the same as *"was it cancelled?"*.

---

## 3. Stale in-flight messages: check-then-act cannot be fixed by checking

The deepest one, and the reason "click several times before it seeks".

A producer that checks a liveness flag and then posts has done **two** things,
and the model can change in between:

    T1  worker    alive() -> true
    T2  UI        model changes, this producer is stopped
    T3  worker    post(msg)      <-- passed the check, posts anyway
    T4  UI        applies a message from a producer that no longer exists

No amount of checking on the worker closes this. The window is *between* the
check and the post, and it is never zero. Measured with a tight producer loop:

    messages applied from an ALREADY-STOPPED source: 2,554,454

In the video example this was the whole bug: the playback decoder's queued
frames were applied *after* the seek in the same drain, and the frame handler
assigned `m.time` unconditionally — so a frame from the old position
overwrote the seek target. The bar moved, the picture did not.

    SeekTo set 518.77  ->  after 1 tick 0.63   <-- queued stale frame won

### The fix: gate at apply time, not at post time

The gate travels **with** the message and is evaluated on the UI thread when
the message is delivered, where it is atomic with respect to the model:

```cpp
inbox->post_gated(std::move(m), alive);   // worker: no check needed

// drain(), on the UI thread:
if (e.gate && !e.gate->load()) continue;  // exact, whatever the interleaving
```

Result: **2,554,454 -> 0**. A stale message stops being a race to be narrowed
and becomes a value that is dropped.

This is the general principle worth remembering:

> A validity check must happen at the point of **use**, on the thread that
> owns the state. Checking at the point of **production** is always a race,
> because production and delivery are separated in time.

Swift's concurrency model makes the same admission explicitly: cancellation is
*cooperative*, `Task.isCancelled` is advisory, and a cancelled task **still
returns a value** unless you check again. Kotlin's `ensureActive()` and Go's
`ctx.Done()` are the same contract. Cancellation is a *request*, never a
guarantee — so correctness has to come from validating results, not from
trusting that cancelled work stayed silent.

---

## 4. Blocking teardown

Reconciliation stopped a dropped source by setting its flag and then
**joining** the thread inline. The join is on the frame: the producer only
notices at its next poll, and one mid-work can take hundreds of milliseconds.

    370 ms of frozen UI per stop, with a 400 ms producer

An app that folds state into a source id — the documented pattern — pays this
on *every* change. Fix: signal, retire to a list, and reap on a later tick
once the thread has set a `done` flag, so only already-finished threads are
ever joined. Measured after: **0.5 ms**.

The contract this establishes is worth stating, because a test asserted the
old one: a source **stops promptly and leaks nothing**. It is *not* joined
before `tick()` returns. Teardown still joins everything, where blocking is
correct because the inbox is about to be freed.

---

## Two adjacent traps

**Computed ids must be owned.** `Sub::Source::id` was a `string_view`, but the
documented way to give a source identity is to compute it — always a
temporary. The runtime diffed freed memory, so two different epochs compared
*equal* (ASan: `stack-use-after-return`), reconciliation saw no change, and
the old producer was never replaced. An API whose own documented usage dangles
is the bug.

**Reconcile after the model changes, not before.** The live source set is a
pure function of the model, and `tick()` computed it at the top — before
draining the messages that change the model. A source dropped *this* frame
survived a whole extra frame, posting all the while.

---

## The checklist

When wiring async work to a UI, answer these four questions explicitly:

1. **Is the trigger discrete or continuous?** Continuous ⇒ never
   `switchMap`/`task_latest`; you will starve.
2. **Can a result outlive its request?** If yes, stamp it and drop
   non-current ones. Do not infer "done" from "a result arrived".
3. **Where is validity checked?** It must be at the point of use, on the
   thread that owns the state. A check on the producer is a race.
4. **Does teardown block a frame?** Signal and reap; never join inline.

Every one of these is a *state ownership* question wearing a concurrency
costume. That is why locks do not help and why the fixes live in the
scheduling policy and the delivery path, not in a mutex.

---

## Sources

- Angular University, *Comprehensive Guide to Higher-Order RxJs Mapping
  Operators* — the `switchMap`/`mergeMap`/`concatMap`/`exhaustMap` taxonomy.
- Max Rozen, *Fixing Race Conditions in React with useEffect*, and the React
  docs' `ignore`-flag cleanup pattern.
- Majid Jabrayilov, *Task Cancellation in Swift Concurrency* — cooperative
  cancellation; a cancelled task still returns a value.
- Kolesnichenko, Nanz & Meyer, *How to Cancel a Task* (ETH Zürich) — why
  cooperative cancellation is the only safe general model.
- elm/compiler#1776, elm/core#993 — subscriptions reconciled against a model
  that a command has already changed.
