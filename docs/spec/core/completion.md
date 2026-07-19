# `Completion<T>` — design

`morph::async::Completion<T>` is a move-only handle representing the eventual
result of an asynchronous operation. It delivers a single success value or an
error to callbacks registered via `then()` / `onError()`, posting the
callback to the executor supplied at construction (if any) so it runs on the
intended thread (e.g. the GUI thread). When the executor is `nullptr`,
callbacks are never posted — the handle is a write-only endpoint for the
producer. An undelivered *value* is dropped silently, but an undelivered
*error* is preserved: it surfaces through the destructor's orphan logger rather
than vanishing (see [Failure modes](#failure-modes)).

## Contents

- [Shared state — `CompletionState<T>`](#shared-state--completionstatet)
- [Orphan detection](#orphan-detection)
- [Move-only handle — `Completion<T>`](#move-only-handle--completiont)
- [Thread safety](#thread-safety)
- [Failure modes](#failure-modes)
- [Empty state](#empty-state)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Shared state — `CompletionState<T>`

`detail::CompletionState<T>` is the heap-allocated backing that both the
producer and the consumer reference through `std::shared_ptr`. All mutation is
guarded by `std::mutex mtx`.

| Member | Type | Purpose |
|---|---|---|
| `mtx` | `std::mutex` | Guards all state and callback registration |
| `value` | `std::optional<T>` | The success value, set once |
| `error` | `std::exception_ptr` | The error, set once via `setException` |
| `ready` | `bool` | `true` once either `value` or `error` is set |
| `onOk` | `std::function<void(T)>` | Stored success callback, moved out on dispatch |
| `onErr` | `std::function<void(std::exception_ptr)>` | Stored error callback, moved out on dispatch |
| `onErrAttached` | `bool` | Suppresses orphan logging when `true`; set to `(cbExec != nullptr)` — never set on a null-executor state |
| `cbExec` | `::morph::exec::IExecutor*` | Executor for callback dispatch; may be `nullptr` |

**Setting a value or exception.** `setValue(T)` and `setException(exception_ptr)`
are called by the producer. If the state is already `ready`, the call is a no-op
(only the first result wins). When a callback is already registered (via
`attachThen` / `attachOnError`), a fire-once closure is built under the lock and
posted to the executor outside the lock, so the callback never runs under the
mutex. The closure is posted only when `cbExec != nullptr`; with a null executor
it is built but never delivered.

`setException` additionally sets `onErrAttached = (cbExec != nullptr)` — but
only along the branch where an `onErr` handler was already registered. It marks
the error handled (suppressing the orphan logger) **only when an executor exists
to actually deliver it**. With a null executor the handler is present but the
closure is never posted, so `onErrAttached` stays `false` and the abandoned
error still reaches the destructor's orphan logger rather than vanishing
silently.

**Attaching callbacks.** `attachThen(handler)` and `attachOnError(handler)` are
called by `Completion::then()` / `onError()`. If the state is already ready with
the corresponding kind of result (value for `then`, exception for `onError`), a
fire-now closure is built and posted to the executor. Otherwise, if the state is
not yet ready, the handler is stored in `onOk` / `onErr` for later dispatch. If
the state is already ready with the *opposite* kind of result (e.g. `attachThen`
on an error state, or `attachOnError` on a value state), neither branch runs: no
closure is built and no handler is stored — the attach is a silent no-op.

**Copy vs. move of the value on dispatch.** The two dispatch paths handle the
stored value differently, and the difference is observable:

- *Set-after-attach* (`setValue` finds an already-registered `onOk`): the value
  is **moved** out of `value` into the closure (`std::move(*value)`). After
  dispatch, `value` holds a moved-from `T`.
- *Attach-after-ready* (`attachThen` fires now against a settled value): the
  value is **copied** (`savedVal = *value`), leaving `value` intact.

The fire-now copy is what makes a repeated `then()` on an already-settled value
state fire again with the same result (see [Failure modes](#failure-modes)); a
move there would hand the second handler a moved-from value. Errors have no such
asymmetry — an `exception_ptr` is cheap to copy and is copied on both paths, so
`error` is never emptied.

`attachOnError` sets `onErrAttached = (cbExec != nullptr)` unconditionally on
entry, before inspecting the state. So attaching an error handler on a
null-executor state does **not** suppress orphan logging: the handler will never
be posted, so the error is preserved for the destructor's orphan logger instead
of being both dropped and silenced.

## Orphan detection

If a `CompletionState` is destroyed while `ready == true`, `error` is set, and
`onErrAttached` is `false`, the destructor logs the unhandled exception via
`::morph::log::logError` with the prefix `[orphan]`. The exception is
re-thrown solely to extract a message:

- If it derives from `std::exception`, the log reads
  `[orphan] unhandled exception: <what()>`.
- Otherwise (a `catch (...)` branch), the log reads
  `[orphan] unhandled unknown exception`.

The `logError` call itself is wrapped in a `try { ... } catch (...) {}` (an
empty catch that swallows any exception `logError` might throw), so the
`noexcept` destructor never lets an exception escape. This prevents silent
loss of error information when a `Completion` goes out of scope without an
`onError` handler.

`onErrAttached` is set by both `attachOnError` (unconditionally, on entry) and
`setException` (on the branch where an `onErr` handler was already registered),
but in both cases the value written is `(cbExec != nullptr)`, **not** an
unconditional `true`. The consequences:

- **With an executor:** once an error handler has been attached (or an error has
  been dispatched to an already-registered handler), `onErrAttached` is `true`
  and the destructor treats the error as handled — no orphan is logged.
- **With a null executor:** the handler can never be posted, so `onErrAttached`
  stays `false` and the destructor still logs the orphan. This closes a hole
  where a null-executor error handler used to both drop the error (no executor
  to post on) *and* silence the orphan logger, losing the error entirely.

In short, orphan logging is suppressed precisely when the error has a real
delivery path; if the error can never be delivered, it is never silenced.

## Move-only handle — `Completion<T>`

`Completion<T>` wraps a `shared_ptr<CompletionState<T>>`. Move-only — no copy
construction or copy assignment. The default constructor produces an empty
(no-op) completion with a null state pointer.

The two-argument constructor takes a shared state and an executor pointer,
storing the executor in `state->cbExec`. All subsequent `then()` / `onError()`
calls forward to the state's `attachThen` / `attachOnError`, which use `cbExec`
for posting.

`then()` and `onError()` return `*this` for chaining:

```cpp
completion
    .then([](int val) { /* ... */ })
    .onError([](std::exception_ptr e) { /* ... */ });
```

## Thread safety

- `then()` and `onError()` may be called from any thread — the mutex guards
  `value`, `error`, `ready`, and the callback slots (`onOk` / `onErr`), so
  registration and result-setting race safely.
- Callbacks are never invoked directly from the producing thread. They are
  posted to `cbExec` via `IExecutor::post()` and run on the executor's thread.
- If `cbExec` is `nullptr`, no callback is posted (the fire-now/fire-once
  closure is built but never delivered, and stored callbacks are never
  invoked). See [Failure modes](#failure-modes) for what happens to an
  abandoned error in this case.

**`cbExec` is not mutex-guarded.** The `mtx` protects `value` / `error` /
`ready` / `onOk` / `onErr`, but `cbExec` is read outside the lock (after the
scoped lock is released) in `setValue`, `setException`, `attachThen`, and
`attachOnError`. This is safe only because of a happens-before requirement, not
a lock: the `Completion<T>` handle writes `cbExec` in its constructor, and the
state must be fully constructed (with its executor assigned) **before** it is
published to any producer or consumer thread. Once published, `cbExec` is never
reassigned. There is no atomic and no lock around it — the ordering guarantee is
structural (construct-then-share), not enforced at runtime.

## Failure modes

These are the sharp edges of the single-shot, single-consumer design. None of
them raise or throw — they are silent by construction.

- **Last-writer-wins callback slots.** Each state has exactly one `onOk` slot
  and one `onErr` slot. A second `then()` overwrites the first success handler
  (`onOk = std::move(handler)`); a second `onError()` overwrites the first error
  handler. Only one handler per outcome survives, and it is the most recent one
  registered *while the state was not yet ready*. There is no fan-out to
  multiple consumers.

- **Mismatched attach on a ready state is a silent no-op.** `then()` on a state
  that is already `ready` with an *error* does nothing — no closure, no stored
  handler, no error surfaced to the `then` handler. Symmetrically, `onError()`
  on a state that is already `ready` with a *value* does nothing. Only an
  attach that matches the settled outcome (or precedes readiness) has any
  effect.

- **Null-executor error drop, but no silencing.** With `cbExec == nullptr`, an
  attached or pending error handler is never delivered — there is no executor to
  post it on. Crucially, `onErrAttached` is left `false` in that case (it is set
  to `(cbExec != nullptr)`), so the abandoned error still reaches the
  destructor's orphan logger. The error is *undelivered* but never *lost*: it
  surfaces as an `[orphan]` log line instead. (A null-executor **value** is
  simply dropped with no diagnostic — only errors have orphan logging.)

- **Overwriting a delivered handler has no effect.** Once a handler has fired
  (its slot was moved out on dispatch), re-registering is governed by the rules
  above against the now-`ready` state — i.e. a matching-outcome re-attach fires
  again with the settled result, a mismatched one is a no-op. A re-attached
  `then` fires with a *copy* of the value (the fire-now path copies; see
  [Shared state](#shared-state--completionstatet)), so the value is not consumed
  by the first fire-now dispatch. If the value was instead delivered via the
  set-after-attach path (moved out), a subsequent `then()` re-attach still fires,
  but against the now moved-from `value`.

## Empty state

A default-constructed `Completion` has a null `_state` pointer. `then()` and
`onError()` are no-ops (they check for `nullptr` and return `*this`). The
`state()` accessor returns `nullptr`. This is used for placeholder completions
that will never signal.

## API reference

### `Completion<T>` (namespace `morph::async`)

| Member | Signature | Notes |
|---|---|---|
| default ctor | `Completion() = default` | Empty, no-op completion (null state). |
| value ctor | `Completion(shared_ptr<CompletionState<T>>, IExecutor*)` | Backed by user-supplied state; executor may be `nullptr`. |
| move ctor | `Completion(Completion&&) noexcept = default` | Transfers state ownership. |
| move assign | `Completion& operator=(Completion&&) noexcept = default` | Transfers state ownership. |
| copy ctor | `Completion(Completion const&) = delete` | Move-only handle. |
| copy assign | `Completion& operator=(Completion const&) = delete` | Move-only handle. |
| `then(handler)` | `Completion& then(std::function<void(T)>)` | Registers success callback; returns `*this` for chaining. |
| `onError(handler)` | `Completion& onError(std::function<void(std::exception_ptr)>)` | Registers error callback; returns `*this` for chaining. |
| `state()` | `shared_ptr<CompletionState<T>> state() const` | Returns the underlying shared state (advanced / internal use). |

### `CompletionState<T>` (namespace `morph::async::detail`)

| Member | Signature | Notes |
|---|---|---|
| `setValue(T)` | `void setValue(T)` | Producer-side; no-op if already ready. Posts callback if one was registered. |
| `setException(exception_ptr)` | `void setException(std::exception_ptr const&)` | Producer-side; no-op if already ready. Posts callback if one was registered. |
| `attachThen(function<void(T)>)` | `void attachThen(std::function<void(T)>)` | Consumer-side; fires immediately if ready with value, stores otherwise. |
| `attachOnError(function<void(exception_ptr)>)` | `void attachOnError(std::function<void(std::exception_ptr)>)` | Consumer-side; fires immediately if ready with error, stores if not yet ready, no-op if ready with a value. Sets `onErrAttached = (cbExec != nullptr)`, so orphan logging is suppressed only when an executor exists to deliver on. |
| destructor | `~CompletionState()` | Orphan-detection: logs unhandled exceptions when destroyed with an error and no `onErr` attached. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Callback dispatch | **Posted to `IExecutor`, never direct** | Ensures callbacks run on the intended thread (e.g. GUI/main thread) regardless of which thread completes the operation. |
| Mutex scope | **Lock held only during state access, not during callback invocation** | Callback closures are built under the lock but invoked outside it, preventing callback re-entrancy into the mutex and avoiding deadlock. |
| Orphan detection | **Destructor logs through `logError`** | Prevents silent loss of error information when a `Completion` is destroyed without an `onError` handler. The exception is re-thrown just to extract a message (`what()` for a `std::exception`, a generic string otherwise), which is logged; the `logError` call is itself wrapped in an empty `catch (...)` so the `noexcept` destructor never lets an exception escape. |
| No executor callback | **`cbExec == nullptr` disables posting** | A `Completion` without an executor is a write-only endpoint — the producer can set a value or error, but stored callbacks are never invoked. This is by design for internal patterns where the consumer never attaches. An abandoned *error* is not silenced, though: `onErrAttached` tracks `(cbExec != nullptr)`, so a null-executor error still reaches the destructor's orphan logger. |
| First-result-wins | **`setValue`/`setException` are no-ops after `ready`** | An asynchronous operation should complete exactly once; subsequent calls are silently ignored. |
| Move-only handle | **`Completion` is move-only, `CompletionState` is shared via `shared_ptr`** | The handle is owned by one consumer at a time; the shared state is owned jointly by the producer and any consumer that has moved the handle. |
| Empty completion | **Null state pointer makes `then`/`onError` no-ops** | Default-constructed `Completion` is a safe placeholder that never signals. |
| Value copy on fire-now | **`attachThen` copies `*value`; `setValue` moves it into the closure** | The set-after-attach path moves because the value is consumed exactly once. The attach-after-ready path must copy so `value` stays intact and a repeated `then()` on a settled state can still fire with the result. |

## Limitations

`Completion<T>` is deliberately a **leaf callback primitive**, not a general
future/promise or a monadic async type. Its scope is narrow by design:

- **No transformation, no chaining.** `then()` returns `*this` (the same
  `Completion<T>&`), purely so a `then().onError()` pair reads fluently. It does
  **not** return a new `Completion<U>` for a transformed result — there is no
  `T → U` mapping and no way to chain one asynchronous step onto another. To
  sequence work, the consumer must start a fresh operation from inside the
  handler.
- **No `co_await`.** `Completion<T>` is not an awaitable; it has no coroutine
  promise/awaiter machinery. Consumption is callback-only.
- **No cancellation.** There is no handle to cancel an outstanding operation;
  once started, it runs to completion (or is abandoned).
- **Single consumer, one handler per outcome.** The handle is move-only and each
  state has exactly one `onOk` and one `onErr` slot. There is no multicast /
  fan-out; a later registration overwrites an earlier one (see
  [Failure modes](#failure-modes)).
- **No synchronous blocking.** There is no `wait()` or `get()`.

**Orphan logging fires from `~CompletionState`, not from handle destruction.**
The orphan check lives in `CompletionState::~CompletionState`, which runs when
the *last* `shared_ptr` to the state drops — jointly held by the producer and
any consumer that moved the handle. Destroying a `Completion<T>` handle does not
by itself trigger orphan logging if the producer still holds a reference to the
state; the log is emitted only when the state itself is finally destroyed with a
`ready` error and `onErrAttached == false`.

## Out of scope

- Cancellation — there is no mechanism to cancel an outstanding operation.
- Multiple values — `Completion<T>` is a single-result primitive.
- Synchronous blocking — there is no `wait()` or `get()`; the API is
  callback-only.
- Transformation / composition — see [Limitations](#limitations).

## Cross-references

- [`executor.md`](executor.md) — `IExecutor` and its implementations; `cbExec`
  is the executor on which every callback is posted.
- [`logger.md`](logger.md) — `morph::log::logError`, the error-handling sink
  used by orphan detection when an error is abandoned.
- [`error_handling.md`](../error_handling.md) — the framework-wide error-propagation
  story; the orphan-logging contract detailed in this file is summarised there
  alongside the executor and backend error paths.
- [`bridge.md`](bridge.md) — `BridgeHandler<M>` produces `Completion<T>` from
  `execute()` and posts callbacks on the GUI executor.
- [`backend.md`](backend.md) — backends resolve the pending `Completion` when a
  response arrives.