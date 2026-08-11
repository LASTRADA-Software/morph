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
- [Settleable promise seam — `Completion<T>::Promise`](#settleable-promise-seam--completiontpromise)
- [Thread safety](#thread-safety)
- [Failure modes](#failure-modes)
- [Client-side execute deadline](#client-side-execute-deadline)
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
| `onOk` | `std::vector<std::function<void(T)>>` | Stored success callbacks, in attachment order; moved out on dispatch |
| `onErr` | `std::vector<std::function<void(std::exception_ptr)>>` | Stored error callbacks, in attachment order; moved out on dispatch |
| `onErrAttached` | `bool` | Suppresses orphan logging when `true`; set to `(cbExec != nullptr)` — never set on a null-executor state |
| `cbExec` | `::morph::exec::IExecutor*` | Executor for callback dispatch; may be `nullptr` |

**Setting a value or exception.** `setValue(T)` and `setException(exception_ptr)`
are called by the producer. If the state is already `ready`, the call is a no-op
(only the first result wins). When one or more callbacks are already registered
(via `attachThen` / `attachOnError`), a fire-once closure invoking every
registered callback, in attachment order, is built under the lock and posted to
the executor outside the lock, so no callback ever runs under the mutex. The
closure is posted only when `cbExec != nullptr`; with a null executor it is
built but never delivered. Each individual handler invocation inside that
closure is wrapped in its own `try { ... } catch (...) { logError(...); }`, so a
throwing handler is logged and skipped without preventing the handlers attached
after it from running — fan-out means every attached handler gets its turn,
independent of an earlier one misbehaving.

`setException` additionally sets `onErrAttached = (cbExec != nullptr)` — but
only along the branch where at least one `onErr` handler was already
registered. It marks the error handled (suppressing the orphan logger) **only
when an executor exists to actually deliver it**. With a null executor the
handlers are present but the closure is never posted, so `onErrAttached` stays
`false` and the abandoned error still reaches the destructor's orphan logger
rather than vanishing silently.

**Attaching callbacks — composes, does not overwrite.** `attachThen(handler)`
and `attachOnError(handler)` are called by `Completion::then()` / `onError()`.
If the state is already ready with the corresponding kind of result (value for
`then`, exception for `onError`), a fire-now closure for *this* handler is built
and posted to the executor immediately (this handler alone — earlier handlers,
if any, already fired when the state became ready, or will each fire from their
own immediate call). Otherwise, if the state is not yet ready, the handler is
appended to `onOk` / `onErr` — **every** handler attached while the state is
still pending is kept, not just the most recent one. When the result finally
arrives, `setValue`/`setException` invokes all of them, in the order they were
attached, from one posted closure. If the state is already ready with the
*opposite* kind of result (e.g. `attachThen` on an error state, or
`attachOnError` on a value state), neither branch runs: no closure is built and
no handler is stored — the attach is a silent no-op, for that call only (it does
not affect any other handler already stored).

**Copy vs. move of the value on dispatch.** The two dispatch paths handle the
stored value differently, and the difference is observable:

- *Set-after-attach* (`setValue` finds one or more already-registered `onOk`
  handlers): every handler but the last is invoked with a **copy** of the
  value; only the final handler in attachment order receives it **moved**
  (`std::move(savedVal)`). After dispatch, `value` itself holds a moved-from
  `T` only if a subsequent `attachThen()` re-attach reads it (see below) — the
  in-flight closure's own copies are unaffected by that.
- *Attach-after-ready* (`attachThen` fires now against a settled value): the
  value is **copied** (`savedVal = *value`), leaving `value` intact.

The fire-now copy is what makes a repeated `then()` on an already-settled value
state fire again with the same result (see [Failure modes](#failure-modes)); a
move there would hand the second handler a moved-from value. Errors have no such
asymmetry — an `exception_ptr` is cheap to copy and is copied for every handler
on both paths, so `error` is never emptied.

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

## Settleable promise seam — `Completion<T>::Promise`

`Completion<T>::makeSettleable(execPtr)` is a static factory returning a
`std::pair<Completion<T>, Completion<T>::Promise>` that share one freshly
allocated `CompletionState<T>`. It is the public counterpart to hand-building a
`Completion<T>` from a `detail::CompletionState<T>` the way `Bridge` and the
backends do internally (see [Shared state](#shared-state--completionstatet)) —
useful for test code (or any caller outside the framework's own producer code)
that needs a `Completion<T>` it can resolve or reject on demand, without a full
`Bridge`/`IBackend` round trip and without ever naming
`morph::async::detail::CompletionState<T>` (issue #55).

```cpp
auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);
completion.then([](int val) { /* ... */ });
// ... later, from producer code:
promise.resolve(42);   // or promise.reject(someExceptionPtr);
```

`Promise` is move-only, mirroring `Completion<T>`, and exposes exactly two
methods:

- `resolve(T val)` — calls the shared state's `setValue(std::move(val))`.
- `reject(std::exception_ptr exc)` — calls the shared state's `setException(exc)`.

Both are no-ops if the state is already settled (first-result-wins, same as
`CompletionState<T>::setValue`/`setException`) or if this `Promise` was itself
moved from (mirroring `Completion<T>::then()`/`onError()`'s null-state no-op —
see [Empty state](#empty-state)). Both are safe to call from any thread, since
they forward directly to the mutex-guarded `CompletionState<T>` methods.

`Promise` never exposes `CompletionState<T>` in its own interface — its
constructor is private, reachable only via the `friend`ed `makeSettleable()` —
so a caller can settle a `Completion<T>` on demand without the `detail::`
namespace ever appearing in their code.

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

These are the sharp edges of the single-shot design. None of them raise or
throw — they are silent by construction.

- **Fan-out on attach, not overwrite.** Each state holds a `std::vector` of
  success handlers (`onOk`) and a `std::vector` of error handlers (`onErr`). A
  second, third, ... `then()` (or `onError()`) attached while the state is not
  yet ready is *appended*, not swapped in — every handler attached before
  readiness runs when the result arrives, in the order it was attached. This
  closes the earlier "last-writer-wins" foot-gun (issue #59), where a second
  `onError()` on the same still-pending `Completion` silently discarded the
  first handler and, because `onErrAttached` was still set, suppressed the
  orphan logger too — losing the error's diagnostic entirely.

- **Mismatched attach on a ready state is a silent no-op.** `then()` on a state
  that is already `ready` with an *error* does nothing — no closure, no stored
  handler, no error surfaced to the `then` handler. Symmetrically, `onError()`
  on a state that is already `ready` with a *value* does nothing. Only an
  attach that matches the settled outcome (or precedes readiness) has any
  effect. This is per-call: it never removes or otherwise disturbs any handler
  already stored from an earlier, matching attach.

- **Null-executor error drop, but no silencing.** With `cbExec == nullptr`, any
  attached or pending error handlers are never delivered — there is no executor
  to post them on. Crucially, `onErrAttached` is left `false` in that case (it
  is set to `(cbExec != nullptr)`), so the abandoned error still reaches the
  destructor's orphan logger. The error is *undelivered* but never *lost*: it
  surfaces as an `[orphan]` log line instead. (A null-executor **value** is
  simply dropped with no diagnostic — only errors have orphan logging.)

- **Attaching after delivery re-fires against the settled state.** Once every
  stored handler has fired (the vector was moved out on dispatch), a further
  `then()`/`onError()` call is governed by the rules above against the
  now-`ready` state — i.e. a matching-outcome attach fires immediately with the
  settled result, a mismatched one is a no-op. A late `then()` fires with a
  *copy* of the value (the fire-now path copies; see
  [Shared state](#shared-state--completionstatet)), so the value is not
  consumed by any fire-now dispatch. If the value was instead delivered via the
  set-after-attach path (the *last* stored handler received it moved), a
  subsequent `then()` attach still fires, but against the now moved-from
  `value` — since `attachThen`'s fire-now path reads `*value` directly, not
  from the (already-emptied) handler vector.

## Client-side execute deadline

Nothing in `Completion<T>` itself imposes a time limit: a state that no producer
ever settles simply stays pending forever, and its handle's callbacks never
fire. For an in-process `LocalBackend` that is unreachable, but across a wire a
request can genuinely disappear — a frame silently discarded by
`QtWebSocketServerConfig::messagesPerSecond`'s rate limiter, a connection that
dropped between send and reply, or a server that hangs. In every one of those
cases *no reply of any kind* comes back, so no layer below the caller has
anything to resolve the `Completion` with.

`Bridge::setExecuteDeadline(std::chrono::milliseconds)` closes that hole.

**Opt-in, default disabled.** The deadline defaults to
`std::chrono::milliseconds{0}`, which means "no deadline" and reproduces the
pre-existing behavior exactly — a `Bridge` that never calls the setter behaves
as it always did, and spawns no extra thread. The current value is readable via
`Bridge::executeDeadline()`.

**Single-threaded WebAssembly.** `TimeoutScheduler` has a second build,
selected by `#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)`,
that uses the browser's own `setTimeout` (`emscripten_async_call`) instead of a
thread and fires its callbacks on the main thread — the same thread the Qt
event loop and every `QtExecutor`-posted completion callback already run on.
This is not a degradation switch: deadlines still fire, with the same
first-result-wins race and the same `ClientTimeoutError`. It exists because a
`wasm_singlethread` Qt build (what `.github/workflows/wasm-ladder.yml` installs
and what `cmake/morph_add_rung.cmake` builds against, with no `-pthread`) links
Emscripten's non-pthread `pthread_create` stub, so constructing a `std::thread`
throws `std::system_error` at runtime — which would have made
`setExecuteDeadline` unusable from a browser tab, and with it
`examples/common/gui/event_poller.hpp`, whose constructor calls it
unconditionally. Two behavioural differences, both documented in
`timeout_scheduler.hpp`'s own `@file` comment: callbacks are never concurrent
with the caller, and `cancel()` releases the callback immediately but leaves the
underlying browser timer to elapse harmlessly rather than clearing it. **This
build has never been compiled or run in this repository** — no Emscripten
toolchain is available here; its only verification is the `ladder-wasm` CI
compile gate.

**Mechanics.** Every `executeVia()` call made while a non-zero deadline is
installed arms a timer on a `Bridge`-owned
`morph::async::detail::TimeoutScheduler` (a single background thread — or, in a
single-threaded WASM build, a browser timer; see above — created lazily on the
first call that enables a deadline and torn down with the `Bridge`; the same
class `RemoteServer` uses for its server-side `LimitPolicy::executeTimeout`). The timer's callback captures only the typed
`CompletionState` — never the `Bridge` — and resolves it with
`morph::backend::ClientTimeoutError`. The real reply and the timer therefore
race, and **whichever settles the state first wins**, because `setValue` /
`setException` are no-ops once the state is `ready` (see
[Failure modes](#failure-modes) and the *first-result-wins* row in
[Design decisions](#design-decisions)). A real reply that arrives after the
deadline already fired is silently discarded — it is an ordinary late write to
an already-resolved state, not an error condition. Conversely, a reply that
arrives first disarms the timer as the *first* statement of the completion
callback, before any `onResult` / `publishResult` fan-out work, so a slow
subscriber cannot open a window for the timer to fire against a result already
in hand.

The deadline is armed only for real dispatches. `executeVia()`'s fast-fail path
for an unbound handler resolves its `Completion` synchronously before the timer
block is reached, so no timer is created for it.

The disarm is guarded on the same `Bridge` liveness token the rest of the
completion callback uses: the callback can in principle run after `~Bridge()`
(the backend may be co-owned and outlive the `Bridge`). Skipping the disarm in
that case is harmless — `~TimeoutScheduler` drops still-pending entries without
firing them.

**`ClientTimeoutError` vs. `TimeoutError`.** Both live in `morph::backend` and
both derive from `std::runtime_error`, but they report different facts:

| Type | Raised by | Means |
|---|---|---|
| `TimeoutError` | The **server**, as an explicit `err "timeout"` reply when `LimitPolicy::executeTimeout` elapses | The request *was* received and the action *is* running (morph never interrupts an in-flight `Model::execute`); the server chose to stop making the caller wait. |
| `ClientTimeoutError` | The **client**, when `Bridge::setExecuteDeadline`'s duration elapses | Nothing came back at all. Whether the server ever received the request, is still processing it, or replied over a connection that had already dropped is **unknown**. |

The practical consequence for callers: `TimeoutError` confirms the action is
in flight server-side, so a blind retry risks a duplicate. `ClientTimeoutError`
confirms nothing, so a retry must be idempotent (or reconciled) either way.

A deadline bounds the *caller's wait*, never the work. It does not cancel the
request — see [Limitations](#limitations), "No cancellation". The server-side
counterpart is documented in [`backend.md`](backend.md) under `LimitPolicy`.

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
| `makeSettleable(execPtr)` | `static std::pair<Completion<T>, Promise> makeSettleable(IExecutor*)` | Public settleable-promise factory (see [Settleable promise seam](#settleable-promise-seam--completiontpromise)). |

### `Completion<T>::Promise` (namespace `morph::async`)

| Member | Signature | Notes |
|---|---|---|
| move ctor | `Promise(Promise&&) noexcept = default` | Transfers state ownership. |
| move assign | `Promise& operator=(Promise&&) noexcept = default` | Transfers state ownership. |
| copy ctor | `Promise(Promise const&) = delete` | Move-only handle. |
| copy assign | `Promise& operator=(Promise const&) = delete` | Move-only handle. |
| `resolve(val)` | `void resolve(T)` | Settles the paired `Completion<T>` with a value; no-op if already settled or moved-from. |
| `reject(exc)` | `void reject(std::exception_ptr)` | Settles the paired `Completion<T>` with an error; no-op if already settled or moved-from. |

### `CompletionState<T>` (namespace `morph::async::detail`)

| Member | Signature | Notes |
|---|---|---|
| `setValue(T)` | `void setValue(T)` | Producer-side; no-op if already ready. Posts one closure invoking every registered success handler, in attachment order, if any were registered. |
| `setException(exception_ptr)` | `void setException(std::exception_ptr const&)` | Producer-side; no-op if already ready. Posts one closure invoking every registered error handler, in attachment order, if any were registered. |
| `attachThen(function<void(T)>)` | `void attachThen(std::function<void(T)>)` | Consumer-side; fires immediately (this handler only) if ready with value, else appends to the stored handler list. |
| `attachOnError(function<void(exception_ptr)>)` | `void attachOnError(std::function<void(std::exception_ptr)>)` | Consumer-side; fires immediately (this handler only) if ready with error, appends to the stored handler list if not yet ready, no-op if ready with a value. Sets `onErrAttached = (cbExec != nullptr)`, so orphan logging is suppressed only when an executor exists to deliver on. |
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
| Value copy on fire-now | **`attachThen` copies `*value`; `setValue` moves it only into the last handler's invocation** | The set-after-attach path copies the value into every handler but the last (moving only into the final call), so no earlier handler observes a moved-from value and the value is still consumed exactly once overall. The attach-after-ready path must copy so `value` stays intact and a repeated `then()` on a settled state can still fire with the result. |
| Handler fan-out | **`onOk`/`onErr` are `std::vector`s, appended to on each attach** | Fixes issue #59: a second `onError()` (or `then()`) on the same still-pending `Completion` used to silently replace the first handler in a single-slot field. Composing (invoking every attached handler, in order) matches the mental model of an observer list and is what most call sites composing behavior via repeated attach actually expect. |
| Per-handler exception isolation | **Each composed handler invocation is wrapped in its own `try`/`catch (...)`, logged via `logError` and swallowed** | Fan-out means every attached handler should get its turn regardless of what an earlier one does. Without per-handler isolation, one throwing handler would unwind the whole posted closure and silently skip every handler attached after it — turning a single misbehaving consumer into an outage for unrelated ones sharing the same `Completion`. |
| Public settleable-promise seam | **`Completion<T>::Promise`, reachable only via `makeSettleable()`** | Fixes issue #55: test code needing a `Completion<T>` it can resolve/reject on demand had no seam except reaching into `morph::async::detail::CompletionState<T>` directly. `Promise`'s constructor is private and `friend`ed only to `Completion<T>`, so `detail::CompletionState<T>` never has to appear in a caller's own code. |

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
  `Bridge::setExecuteDeadline` (see
  [Client-side execute deadline](#client-side-execute-deadline)) is not an
  exception to this: it bounds how long the *caller* waits by resolving the
  state early, and does nothing to the work still in flight underneath.
- **Single consumer handle, but multiple handlers per outcome.** The
  `Completion<T>` handle itself is move-only — only one owner at a time — but
  each state's `onOk`/`onErr` are vectors, so repeated `then()`/`onError()`
  calls on the same handle (or the `Completion&` it returns for chaining) all
  compose: every handler attached before readiness runs, in attachment order
  (see [Failure modes](#failure-modes)). This is in-process fan-out to
  multiple callbacks on one handle, not multicast to multiple *handles* — there
  is still only one `Completion<T>` per operation.
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
- [`backend.md`](backend.md) — backends resolve the pending `Completion` when a
  response arrives; also `morph::backend::LimitPolicy::executeTimeout`, the
  *server-side* counterpart to
  [the client-side execute deadline](#client-side-execute-deadline), and
  `TimeoutError` / `ClientTimeoutError`.
- [`error_handling.md`](../error_handling.md) — the framework-wide error-propagation
  story; the orphan-logging contract detailed in this file is summarised there
  alongside the executor and backend error paths.
- [`bridge.md`](bridge.md) — `BridgeHandler<M>` produces `Completion<T>` from
  `execute()` and posts callbacks on the GUI executor.
- [Settleable promise seam](#settleable-promise-seam--completiontpromise) —
  `Completion<T>::makeSettleable()`, the public seam test code uses in place of
  a `Bridge`/`IBackend` round trip.
