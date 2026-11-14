# Async state hazards, and which primitive fixes each

Every bug in this document was found in mayag's video example, reproduced with
a measurement, and fixed. They are written up together because they look
identical from the outside — "the UI doesn't update" / "I have to click twice"
— and have completely different causes.

mayag now runs on jaal, which owns tasks, streams and teardown. Hazards 3 and 4
are handled by jaal for every app; 1 and 2 are still the app's call, and the
fix lives in the model.

The framing is deliberately not "concurrency bugs". Locks fix none of these.
Every one is about a question no lock can answer: **is this result still
wanted?**

---

## The four hazards

| # | Hazard | Question it answers wrongly | Fix |
|---|--------|-----------------------------|-----|
| 1 | Starvation under a gesture | "should the running task be cancelled?" | one in flight + a pending slot in the model |
| 2 | Out-of-order results | "does this result belong to my latest request?" | generation stamp |
| 3 | Stale in-flight messages | "was this producer still alive when it posted?" | apply-time gating (jaal) |
| 4 | Blocking teardown | "must I wait for the old worker to notice?" | signal, don't join (jaal) |

---

## 1. Starvation: cancelling the *running* task

A keyed jaal task means "newest wins": a new request with the same key
supersedes the running one.
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
struct Model { bool busy = false; std::optional<double> pending; ... };

static Cmd update(Model& m, SeekTo s) {
    if (m.busy) { m.pending = s.t; return {}; }       // newest overwrites
    m.busy = true;
    return Cmd::perform<Decoded>(&decode_at, s.t);
}
static Cmd update(Model& m, Decoded d) {
    m.busy = false;  /* show d */
    if (m.pending) { /* start the next one from *m.pending */ }
}
```

Results then land as fast as the work can sustain, and the last request always
lands because it is never discarded. Measured: 74 requests, 11 tasks ran, 11
results delivered, request #73 landed. tests/test_concurrency.cpp checks it.

### This is the RxJS flattening taxonomy

The same four policies appear wherever async work is driven by a stream, most
explicitly in RxJS's higher-order mapping operators. The names differ; the
decision is identical:

| Policy | RxJS | mayag on jaal | Use for |
|---|---|---|---|
| Cancel the running one | `switchMap` | keyed `Cmd::task` / `Cmd::perform` | typeahead, discrete requests |
| Let it finish, queue newest | *(no direct equivalent)* | busy + pending in the model | **drags, scrubs, live previews** |
| Run everything in parallel | `mergeMap` | `Cmd::task` | independent fire-and-forget |
| Queue all, preserve order | `concatMap` | — | ordered writes |
| Ignore new while busy | `exhaustMap` | — | submit-button debounce |

The Angular University guide puts the general rule well: *"choosing the wrong
operator might accidentally lead to subtle issues in our programs"*. The
subtle issue here is that `switchMap` (a keyed task) is the usual default and
is exactly wrong for gestures, because a gesture emits faster than the work
completes **by definition**.

The pending slot is closest to `exhaustMap` + "remember the last one" — the
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

**Stamp results with the generation they were produced for.** The runtime
cannot do this for you — only the app knows what makes one result obsolete:

```cpp
struct Frame { ...; int epoch; };          // stamped by the producer

static Cmd update(Model& m, const Frame& f) {
    if (f.epoch != m.epoch) return {};     // drop
    ...
}
```

And don't infer "settled" from "a result arrived": it is settled when
`busy` is false and nothing is pending.

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

jaal does this for every stream and keyed task: each run gets its own origin,
and stopping it retires the origin, so messages it already queued are dropped
at drain time on the loop thread. Nothing for the app to write.

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

An app that folds state into a stream key — the documented pattern — would pay
this on *every* change. jaal signals the stream's stop_token and moves on; it
never joins on the loop. Shutdown waits a bounded time for workers. The test
"replacing a slow stream doesn't stall the loop" checks it.

---

## Two adjacent traps

**Computed keys must be owned.** mayag's old source id was a `string_view`, but the
documented way to give a source identity is to compute it — always a
temporary. The runtime diffed freed memory, so two different epochs compared
*equal* (ASan: `stack-use-after-return`), reconciliation saw no change, and
the old producer was never replaced. jaal stream keys are `std::string`.

**Reconcile after the model changes, not before.** The live source set is a
pure function of the model; computing it before folding the messages that
change the model leaves a dropped source alive a whole extra step. jaal
reconciles after every fold.

---

## The checklist

When wiring async work to a UI, answer these four questions explicitly:

1. **Is the trigger discrete or continuous?** Continuous ⇒ never
   a keyed task alone; you will starve.
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
