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
- [Value-handling contract](#value-handling-contract)
- [Orphan detection](#orphan-detection)
- [Move-only handle — `Completion<T>`](#move-only-handle--completiont)
- [Settleable promise seam — `Completion<T>::Promise`](#settleable-promise-seam--completiontpromise)
- [Thread safety](#thread-safety)
- [Failure modes](#failure-modes)
- [Client-side execute deadline](#client-side-execute-deadline)
- [Lifetime and stop gating](#lifetime-and-stop-gating)
- [Empty state](#empty-state)
- [API reference](#api-reference)
- [`detail::ISettleSink`](#detailisettlesink--where-a-backend-settles-one-dispatch)
- [`morph/core/async.hpp` — the cheap include](#morphcoreasynchpp--the-cheap-include)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Shared state — `CompletionState<T>`

`detail::CompletionState<T>` is the heap-allocated backing that both the
producer and the consumer reference through `std::shared_ptr`. It has **no
lock**. It belongs to one executor, `cbExec` — its owner — and is split in two:

- **The outcome** (`value`, `error`) is written once, by whichever settle
  claims `settled`, and published by a release store of `ready`. Any thread
  that sees `ready == true` may read it.
- **Everything else** — the handler lists, `delivered`, the stop links — is
  touched only on the owner: by `deliver()`, which the settle posts there, and
  by the consumer's attaches, which run there. The executor's queue is the
  happens-before edge between a settle and the handlers it reaches.

It derives from `std::enable_shared_from_this<CompletionState<T>>`. That is
load-bearing: `deliver()` and a late attach's fire-now closure capture
`shared_from_this()` and read the settled value *in place* rather than carrying
a copy of it, which is what makes the copy budget independent of handler count
and what lets `T` be move-only — the closure stores a refcounted handle, so it
stays copy-constructible and `IExecutor::post`'s `std::function<void()>` needs
no change. It is also what keeps the state, and its orphan logger, alive until
delivery has run. The precondition is that every `CompletionState<T>` is created
by `std::make_shared`, which every site in the tree does
(`Completion<T>::makeSettleable`, `Bridge`, the backends).

| Member | Type | Owner | Purpose |
|---|---|---|---|
| `value` | `std::optional<T>` | the claiming settle | The success value, written once |
| `error` | `std::exception_ptr` | the claiming settle | The error, written once; never null once `ready` |
| `settled` | `std::atomic<bool>` | any thread | Claimed by the first settle with an exchange; later settles return at once |
| `ready` | `std::atomic<bool>` | any thread | Release-stored after the outcome is written: exactly one of `value`/`error` is engaged and final |
| `onOk` | `std::vector<std::function<void(const T&)>>` | `cbExec` | Success callbacks, in attachment order; drained by `deliver()`. Erased as `void(const T&)` so a handler pays for its own copy only if it asks for one — see [Value-handling contract](#value-handling-contract) |
| `onErr` | `std::vector<std::function<void(std::exception_ptr)>>` | `cbExec` | Error callbacks, in attachment order; drained by `deliver()` |
| `delivered` | `std::atomic<bool>` | `cbExec` | Set by `deliver()`; an attach after it posts its handler on its own |
| `onErrAttached` | `std::atomic<bool>` | `cbExec`, or a synchronous taker | Suppresses orphan logging; set by an `onError` attach on a state with an executor, and by `bridge::detail::takeSettled` |
| `presumedOwnerThread`, `deliveredOn` | `std::atomic<std::thread::id>` | — | The debug check of an attach made outside every executor's task; see [Thread safety](#thread-safety) |
| `cbExec` | `::morph::exec::IExecutor*` | set before publication | The owner; may be `nullptr` |
| `stopSource`, `stopLinks` | | `cbExec` | The call's stop source and the scope links relaying to it; released by `deliver()` |

**Setting a value or exception.** `setValue(T)` and `setException(exception_ptr)`
are called by the producer, on any thread. The first claims `settled` with an
atomic exchange; every later call returns before allocating anything, which is
what makes a late server reply after `cancelPending` free. The claimant stores
the outcome, release-stores `ready`, and — when `cbExec` is set — posts
`deliver()` to `cbExec`: **one post per settle, whether or not a handler is
attached yet**, because a handler attached later is attached on the owner and
must find the delivery already applied or still queued ahead of it. With a null
`cbExec` nothing is posted: nothing could ever be delivered.

A `T` whose move throws while being stored releases the claim and leaves the
state unsettled with every handler kept, so the exception propagates to the
settler and nothing looks settled that is not. `value` is stored with
`optional::emplace`, never assigned, so `T` need not be move-assignable.

**A null `exception_ptr` is never stored**: `setException(nullptr)` substitutes
a `std::runtime_error` and settles with that instead, so `ready == true` always
implies exactly one of `value` or `error` is engaged. Storing the null would
publish a state no attach could act on — `deliver()` and a late `onError`
attach test `error`, a late `then` tests `value` — and would hand an attached
handler a null `exception_ptr`, which is undefined behaviour to
`std::rethrow_exception`. The guard lives here rather than at any one producer
because `Completion<T>::Promise::reject()` is public and many sites forward an
`exception_ptr` through untouched (`.onError([state](auto e) {
state->setException(e); })`).

**Delivering.** `deliver()` runs on `cbExec`. It sets `delivered`, releases the
stop links, and invokes every stored handler of the settled kind, in
attachment order, each wrapped in its own `try { ... } catch (...) {
logError(...); }`, so a throwing handler is logged and skipped without
preventing the handlers attached after it from running. Handlers of the other
kind are never invoked.

**Attaching callbacks — composes, does not overwrite.** `attachThen(handler)`
and `attachOnError(handler)` are called by `Completion::then()` / `onError()`,
on the owner. Before delivery, the handler is appended to `onOk` / `onErr` —
**every** handler attached while delivery is pending is kept, and `deliver()`
invokes all of them in the order they were attached. After delivery, a handler
matching the settled outcome is **posted on its own** with the same
`try`/`catch`; a mismatched one (`then` on an error, `onError` on a value) is a
silent no-op for that call. A late handler is posted rather than run inside the
attach: a handler attached later must not overtake one attached earlier, and
the earlier one may still be in a posted `deliver()` that has not run. With a
null `cbExec` both attaches are no-ops.

**Copy vs. move of the value on dispatch.** Both dispatch paths read the stored
value in place, and neither copies nor moves it: `deliver()` invokes each
handler as `handler(*value)`, and a late attach's closure captures
`shared_from_this()` and invokes `handler(*self->value)`.

**The value is observed, never consumed.** No dispatch path can move out of
`value`, so a `then()` attached after delivery still fires against the genuine
result rather than a moved-from husk, and no handler in a fan-out can leave a
husk for its siblings. A handler that wants to consume takes `T` **by value**
and moves out of its own copy. Errors are refcounted `exception_ptr`s, copied
for every handler, so `error` is never emptied either.

## Value-handling contract

**At the type level, `T` need only be move-constructible.**
`std::move_constructible<T>` is the whole requirement, enforced by a
`static_assert` on `CompletionState<T>` so an unusable `T` produces one line
rather than pages from inside `std::function`. `Completion<std::unique_ptr<int>>`
instantiates and fans out.

**Copyability is a per-handler obligation, not a per-type one.** Handlers are
erased as `std::function<void(const T&)>`. One erased type accepts every
spelling a caller already writes, because each is invocable with `const T&`:

| handler | copies when invoked |
|---|---|
| `[](const T& v)` | 0 |
| `[](T v)` | 1 |
| `[](auto v)` (by-value generic) | 1 |
| an existing `std::function<void(T)>` object | 1 |

The copy, when a handler wants one, happens at that handler's own parameter
binding — where the reader of the call site can see it, and where the compiler
diagnoses a `T` that cannot be copied. No trait detection, no `if constexpr`, no
signature introspection.

**Copy budget: exactly one copy per handler that takes `T` by value, and zero
for every handler that takes `const T&`.** Independent of how many handlers are
attached, and of whether they attached before or after the completion settled.
Exactly, not "at most": `tests/test_completion_value_contract.cpp` pins the
count with a copy-counting `T`, because an upper bound quietly absorbs a
regression.

Settling itself moves `T` exactly twice for a prvalue argument — into
`resolve`'s by-value parameter, then into `setValue`'s, then into `value`, with
the first elided — and dispatch adds none: the posted `deliver()` carries the
state, not the value.

The obvious alternative — erasing `onOk` as `std::function<void(T)>` — costs
**N + 2M** copies per settle (N handlers attached before settling, M after) and
additionally forces `T` to be copy-constructible. Every handler is charged a
copy whether or not it wants one, and a fire-now path under that erasure has to
copy `*value` into a local and capture that local *by copy* before moving it
into the handler: two copies where the handler asked for at most one.

**What this costs.** A cheap `T` pays a little more per settle: the delivery
closure holds a `shared_ptr` to the state and so pays an atomic refcount pair
rather than a copy of a small value. Against the JSON encode/decode — and often
a socket write — that surrounds a settle, that is noise, and it buys a large `T`
its per-handler copies back. It is recorded here rather than hidden, because it
is a real cost on the cheap case.

**The large-`T` win requires `const T&` handlers.** The handler signature is the
lever a caller pulls, which is why it is documented as contract rather than left
as an implementation detail.

## Orphan detection

If a `CompletionState` is destroyed while `ready == true`, `error` is set, and
`onErrAttached` is `false`, the destructor logs the unhandled exception via
`::morph::log::logError` with the prefix `[orphan]`. The exception is
re-thrown solely to extract a message:

- If it derives from `std::exception`, the log reads
  `[orphan] unhandled exception: <what()>`.
- Otherwise (a `catch (...)` branch), the log reads
  `[orphan] unhandled unknown exception`.

The `logError` calls need no local guard: `morph::log`'s helpers are
`noexcept` (`logger.md`, "Failure modes"), which is what makes them safe to
call from an implicitly-`noexcept` destructor at all. The **variadic** overload
is used deliberately — it formats inside that guarantee, whereas building the
message by concatenation at the call site would allocate outside it and could
still escape. If the record cannot be emitted, the logging layer counts it in
`morph::log::droppedLogRecords()`.

This prevents silent loss of error information when a `Completion` goes out of
scope without an `onError` handler.

`onErrAttached` is set by an `onError` attach on a state that has an executor,
and by `bridge::detail::takeSettled`, which takes a settled bind's outcome
without attaching and so handles the error itself. An `onError` attach on a
null-executor state is a no-op and sets nothing: the handler can never be
delivered, so the error stays the orphan logger's rather than being both
dropped and silenced. In short, orphan logging is suppressed precisely when the
error has a real delivery path, or was taken.

**The log comes after delivery, exactly once.** The posted `deliver()` holds a
`shared_ptr` to the state, so a state settled with an error and dropped by both
its producer and its consumer is still alive while its delivery is queued. The
destructor runs after `deliver()` — and after any posted attach — had its
chance to hand the error to a handler, and runs once.

## Move-only handle — `Completion<T>`

`Completion<T>` wraps a `shared_ptr<CompletionState<T>>`. Move-only — no copy
construction or copy assignment. The default constructor produces an empty
(no-op) completion with a null state pointer.

The two-argument constructor takes a shared state and an executor pointer,
storing the executor in `state->cbExec` — the completion's owner. All
subsequent `then()` / `onError()` calls forward to the state's `attachThen` /
`attachOnError`, on the owner.

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
`morph::async::detail::CompletionState<T>`.

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
see [Empty state](#empty-state)). Both are safe to call from any thread: the
settle claims the state with an atomic exchange and posts its delivery to the
completion's executor.

`Promise` never exposes `CompletionState<T>` in its own interface — its
constructor is private, reachable only via the `friend`ed `makeSettleable()` —
so a caller can settle a `Completion<T>` on demand without the `detail::`
namespace ever appearing in their code.

## Thread safety

A `Completion<T>` belongs to the executor it was constructed with, `cbExec`.
There is no lock; every touch of the handler side happens on `cbExec`.

- **Settling — any thread.** `setValue` / `setException` (and `Promise`'s
  `resolve` / `reject`) claim the state with an atomic exchange, store the
  outcome, publish it with a release store of `ready`, and post `deliver()` to
  `cbExec`. A second settle — `cancelPending` racing a reply, a deadline racing
  the real result — loses the exchange and returns before allocating.
- **Delivery — on `cbExec`.** Callbacks are never invoked on the producing
  thread, and never inside the call that attached them.
- **Attaching — on `cbExec`.** `then()` / `onError()` must be called on the
  owner: inside one of its tasks, or on the owner's own thread outside them (a
  Qt slot, a QML handler, a test body before it pumps). **Attaching from
  another executor's task is a contract violation**, reported through
  `exec::detail::noteOwner` — a debug-build assertion, or the owner probe a test
  installs. An attach made on a thread running no executor's task is presumed
  to be on the owner's thread; the thread is recorded and checked against the
  thread `deliver()` runs on, in whichever order the two happen, so a bare
  thread attaching to a completion whose executor is a pool is reported at
  delivery. This is a debug check, not a guarantee; the ThreadSanitizer leg is
  the backstop.
- **`inlineExecutor()` completions.** Delivery runs wherever the settle happens,
  so the owner is the settler. Their consumer attaches before handing the
  state to the producer, after it settled, or on the settler's own executor
  (`SocketBackend`'s synchronous verbs attach on the I/O loop that settles
  them). This cannot be checked and is not.
- **Synchronous readers — any thread.** Anything that acquire-loads `ready ==
  true` may read `value` / `error`: `bridge::detail::takeSettled` reads a bind
  a backend settled before returning, without a hop to the owner.
- **`co_await` — any coroutine.** The awaiter attaches directly when the
  coroutine is running on `cbExec`, and otherwise posts its attach there (see
  [`coroutines.md`](coroutines.md#co_await-on-a-completiont)).
- If `cbExec` is `nullptr`, nothing is posted and attaching is a no-op. See
  [Failure modes](#failure-modes) for what happens to an abandoned error.

**`cbExec` is a happens-before requirement, not an atomic.** The `Completion<T>`
handle writes it in its constructor, and the state must be constructed with its
executor assigned **before** it is published to any producer or consumer
thread. Once published, `cbExec` is never reassigned. The backends honour this:
they build the `Completion` before posting the task that settles it. It also
has to precede a settle made on the producer's own thread: a settle sees the
owner it has at that moment, and one made while `cbExec` is still unset posts
no delivery, so its handlers would never run. `executeJson`'s decode and
validation failures settle synchronously, which is why it builds its
`Completion` before decoding.

**Cost.** One post per settle, even with no handler attached yet; a settle with
handlers attached costs the same one post it did when delivery was a closure
built under a lock. A late attach costs one post per handler.

## Failure modes

These are the sharp edges of the single-shot design. None of them raise or
throw — they are silent by construction.

- **Fan-out on attach, not overwrite.** Each state holds a `std::vector` of
  success handlers (`onOk`) and a `std::vector` of error handlers (`onErr`). A
  second, third, ... `then()` (or `onError()`) attached while the state is not
  yet ready is *appended*, not swapped in — every handler attached before
  readiness runs when the result arrives, in the order it was attached. A
  single-slot field instead would be a "last-writer-wins" foot-gun: a second
  `onError()` on the same still-pending `Completion` would silently discard the
  first handler and, because `onErrAttached` is set on attach, suppress the
  orphan logger too — losing the error's diagnostic entirely.

- **Mismatched attach on a ready state is a silent no-op.** `then()` on a state
  that is already `ready` with an *error* does nothing — no closure, no stored
  handler, no error surfaced to the `then` handler. Symmetrically, `onError()`
  on a state that is already `ready` with a *value* does nothing. Only an
  attach that matches the settled outcome (or precedes readiness) has any
  effect. This is per-call: it never removes or otherwise disturbs any handler
  already stored from an earlier, matching attach.

  Both arms silently doing nothing is *only* safe because a ready state always
  has exactly one of `value`/`error` engaged, so at most one arm can mismatch.
  A `ready` state with neither engaged would make **both** arms no-ops, and the
  completion could then never resolve for anybody. `setException(nullptr)`
  substituting a `std::runtime_error` is what keeps that state unreachable (see
  [Setting a value or exception](#setting-a-value-or-exception)); this bullet
  depends on it staying so.

- **Null-executor error drop, but no silencing.** With `cbExec == nullptr`, no
  handler is ever delivered — there is no executor to post on, and attaching is
  a no-op. Crucially, `onErrAttached` is therefore left `false`, so the
  abandoned error still reaches the destructor's orphan logger. The error is *undelivered* but never *lost*: it
  surfaces as an `[orphan]` log line instead. (A null-executor **value** is
  simply dropped with no diagnostic — only errors have orphan logging.)

- **Attaching after delivery re-fires against the settled state.** Once
  `deliver()` has run (the vector was moved out), a further
  `then()`/`onError()` call is governed by the rules above against the
  delivered state — i.e. a matching-outcome attach is posted on its own with
  the settled result, a mismatched one is a no-op. A late `then()` fires against the
  *stored* value, read in place by a closure holding `shared_from_this()` (see
  [Shared state](#shared-state--completionstatet)), so no fire-now dispatch
  consumes it and a handler taking `const T&` pays nothing for it. This holds
  regardless of which dispatch path originally delivered the value: `value` is
  never moved out of (see
  [Value-handling contract](#value-handling-contract)), so a `then()` attached
  after a set-after-attach dispatch fires against the same genuine result the
  earlier handlers saw, not a moved-from husk.

## Client-side execute deadline

Nothing in `Completion<T>` itself imposes a time limit: a state that no producer
ever settles simply stays pending forever, and its handle's callbacks never
fire. For an in-process `LocalBackend` that is unreachable, but across a wire a
request can genuinely disappear — a connection that dropped between send and
reply, or a server that hangs. In those cases *no reply of any kind* comes back,
so no layer below the caller has anything to resolve the `Completion` with.

(A frame refused by `QtWebSocketServerConfig::messagesPerSecond`'s rate limiter
used to belong on that list. It no longer does: the transport answers it with an
`err "rate limited"` addressed to the frame's own `callId`, so the caller's
`Completion` fails rather than hanging. A deadline is still worth arming for the
two cases above, which no reply can cover.)

`Bridge::setExecuteDeadline(std::chrono::milliseconds)` closes that hole.

**Opt-in, default disabled.** The deadline defaults to
`std::chrono::milliseconds{0}`, which means "no deadline" and reproduces the
pre-existing behavior exactly — a `Bridge` that never calls the setter behaves
as it always did, and spawns no extra thread. The current value is readable via
`Bridge::executeDeadline()`.

**Single-threaded WebAssembly.** `TimeoutScheduler` keeps its deadlines as
timers on an `exec::IoLoop` — a core-cpp `core::net::PlatformLoop`; `Bridge`'s
scheduler owns a private one. Natively that loop's one thread runs them. Under
`#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)` there is no
thread: the loop is host-driven, pumped by the browser's own `setTimeout`
(`emscripten_async_call`), and fires its callbacks on the main thread — the
same thread the Qt event loop and every `QtExecutor`-posted completion
callback already run on.
This is not a degradation switch: deadlines still fire, with the same
first-result-wins race and the same `ClientTimeoutError`. It exists because a
`wasm_singlethread` Qt build (what `.github/workflows/wasm-ladder.yml` installs
and what `cmake/morph_add_rung.cmake` builds against, with no `-pthread`) links
Emscripten's non-pthread `pthread_create` stub, so constructing a `std::thread`
throws `std::system_error` at runtime — which would have made
`setExecuteDeadline` unusable from a browser tab, and with it
`examples/common/gui/event_poller.hpp`, whose constructor calls it
unconditionally. One behavioural difference: callbacks are never concurrent
with the caller there, so `cancel()` really does mean "no callback runs after
this returns" — its post runs at the start of the next pump, before any timer
callback of that pump — whereas the threaded build's `cancel()` returns while
an *already-started* callback goes on running on the loop's thread. In both
builds `cancel()` posts to the loop, which retires the timer and releases the
callback. A
caller that must work in both builds gets the weaker of the two: every
scheduled callback has to stay safe to run after its own `cancel()`, which the
deadline callback here does by settling a write-once `CompletionState` it
holds a `shared_ptr` to. The `wasm-ladder` and `wasm-demo` workflows compile
and link this build; no test in this repository runs it.

**Mechanics.** Every `executeVia()` call made while a non-zero deadline is
installed arms a timer on a `Bridge`-owned
`morph::async::detail::TimeoutScheduler` (timers on a private `exec::IoLoop`:
one thread natively, or, in a single-threaded WASM build, a loop the browser's
timer pumps; see above — created lazily on the
first call that enables a deadline and torn down with the `Bridge`; the same
class `RemoteServer` uses for its server-side `LimitPolicy::executeTimeout`). The timer's callback captures only the typed
`CompletionState` — never the `Bridge` — and resolves it with
`morph::backend::ClientTimeoutError`. The real reply and the timer therefore
race, and **whichever settles the state first wins**, because `setValue` /
`setException` are no-ops once a settle has claimed the state (see
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

## Lifetime and stop gating

Because every callback is delivered through an executor, the receiver can be
destroyed — or simply lose interest — between attaching a handler and the
handler running. `Completion<T>` therefore accepts a
[`morph::async::CallbackScope`](callback_scope.md) (or one of its
`CallbackToken`s) as an optional first argument:

```cpp
completion.then(_callbacks, [this](Result r) { render(r); })
          .onError(_callbacks, [this](std::exception_ptr e) { showError(e); });
```

The gated handler runs only if, at delivery time, the scope is **both alive and
not stopped**. The full semantics — the `requestStop()` / `reset()` /
destruction verbs, the `Active` / `Stopped` / `Expired` states, and the
executor-affine-versus-advisory thread-safety boundary — live in
[callback_scope.md](callback_scope.md). What matters here is how the gate
interacts with this file's own machinery:

- **Nothing in `CompletionState<T>` changes.** The gate is wrapped around the
  handler at attach time, so it composes with handler fan-out, with the
  attach-after-delivery fire-now path and with `cbExec` marshalling exactly as
  the ungated form does. The scope's stop link is added on the owner, with the
  attach, and released by `deliver()`.
- **A suppressed error still counts as handled.** `onError(scope, fn)` sets
  `onErrAttached` exactly as `onError(fn)` does, so an error whose delivery the
  scope then refuses does **not** re-arm the destructor's orphan logging
  ([Orphan detection](#orphan-detection)). Suppression is a deliberate act by
  the receiver, not a silently dropped error.
- **A refused handler is destroyed, not leaked.** The wrapper and its captures
  are released with the posted closure, on the delivery executor.
- **The ungated spellings are unchanged and undeprecated.** `then(fn)` /
  `onError(fn)` behave exactly as before; `thenDetached(fn)` /
  `onErrorDetached(fn)` are the same attachments under a name that says the
  omission was deliberate and keeps it greppable in review.

This gates **delivery**, never the work. See
[Limitations](#limitations) for why that distinction is kept sharp.

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
| `then(handler)` | `Completion& then(std::function<void(const T&)>)` | Registers success callback; returns `*this` for chaining. |
| `then(scope, handler)` | `Completion& then(CallbackScope const&, std::function<void(const T&)>)` | As above, gated on the scope's liveness and stop state (see [Lifetime and stop gating](#lifetime-and-stop-gating)). |
| `then(token, handler)` | `Completion& then(CallbackToken, std::function<void(const T&)>)` | Token-taking form of the above; a default-constructed token suppresses unconditionally. |
| `thenDetached(handler)` | `Completion& thenDetached(std::function<void(const T&)>)` | Exactly `then(handler)`, spelled so a deliberately ungated callback is greppable. |
| `onError(handler)` | `Completion& onError(std::function<void(std::exception_ptr)>)` | Registers error callback; returns `*this` for chaining. |
| `onError(scope, handler)` | `Completion& onError(CallbackScope const&, std::function<void(std::exception_ptr)>)` | As above, gated on the scope. Still suppresses orphan logging: a scope-refused error counts as handled. |
| `onError(token, handler)` | `Completion& onError(CallbackToken, std::function<void(std::exception_ptr)>)` | Token-taking form of the above. |
| `onErrorDetached(handler)` | `Completion& onErrorDetached(std::function<void(std::exception_ptr)>)` | Exactly `onError(handler)`, under the deliberate-omission spelling. |
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
| `setValue(T)` | `void setValue(T)` | Producer-side, any thread; no-op if a settle already claimed the state. Stores the value, publishes `ready`, posts `deliver()` to `cbExec` (when set). |
| `setException(exception_ptr)` | `void setException(std::exception_ptr const&)` | Producer-side, any thread; no-op if a settle already claimed the state. Stores the error (a null one replaced), publishes `ready`, posts `deliver()` to `cbExec` (when set). |
| `attachThen(function<void(const T&)>)` | `void attachThen(std::function<void(const T&)>)` | Consumer-side, on `cbExec`; appends before delivery, posts this handler alone after a value was delivered, no-op after an error or with a null `cbExec`. |
| `attachOnError(function<void(exception_ptr)>)` | `void attachOnError(std::function<void(std::exception_ptr)>)` | Consumer-side, on `cbExec`; appends before delivery, posts this handler alone after an error was delivered, no-op after a value. Sets `onErrAttached`; a no-op that sets nothing with a null `cbExec`. |
| destructor | `~CompletionState()` | Orphan-detection: logs unhandled exceptions when destroyed with an error and no `onErr` attached. |

## `detail::ISettleSink` — where a backend settles one dispatch

`IBackend::execute` hands its caller a `Completion<std::shared_ptr<void>>`. For
`Bridge::executeVia` that was never the shape it wanted: it already owns the
typed `CompletionState<R>` its caller holds, so the erased completion existed
only to be forwarded into the typed one by a `.then`/`.onError` pair. The
forwarding cost six heap allocations of the 14.06 a local round trip took —
the erased state, the two closures, their two handler vectors, and one of the
two posted settle tasks.

`ISettleSink` is the narrow half of `CompletionState<std::shared_ptr<void>>`:
the two calls a backend actually makes on the completion it produced.

| Symbol | Kind | Purpose |
|---|---|---|
| `detail::ISettleSink` | abstract class | `settleValue(std::shared_ptr<void>)` / `settleException(const std::exception_ptr&)`. Nothing else. |
| `detail::CompletionSettleSink` | class | The adapter that *is* a `CompletionState<std::shared_ptr<void>>`, for callers that still want the `Completion`-returning shape. `LocalBackend::execute` uses one to serve itself from `executeInto`. |

### Contract for implementers

- **Settle once.** `settleValue` and `settleException` are mutually exclusive
  and each takes effect at most once. An implementation must *tolerate* extra
  calls and ignore them: `IBackend::cancelPending` settles a sink that a reply
  may be racing to settle at the same moment. `CompletionState` is already
  first-result-wins, so forwarding is safe by itself — but anything an
  implementation does **besides** forwarding (decrementing a counter,
  cancelling a timer) needs its own latch. `bridge::detail::BridgeSink` carries
  one, and `test_bridge_pending_calls.cpp` pins it: without the latch, a
  `cancelPending` followed by the real reply decrements `Bridge::pendingCalls()`
  twice and the `std::size_t` counter reads `18446744073709551615`.
- **Deliberately not `noexcept`.** `CompletionState::setValue` is not either —
  it builds a `std::function` for the posted callback — and promising more here
  would turn a `bad_alloc` into a `std::terminate` that today it is not.

See [`backend.md`, `IBackend::executeInto`](backend.md#executeinto--settling-the-callers-own-completion) and
[`bridge.md`, "`BridgeSink`"](bridge.md#bridgesink--the-typed-state-the-backend-settles).

## `morph/core/async.hpp` — the cheap include

`Completion`, `IExecutor`, the strands and `CallbackScope` are usable
without a model, a registry, a wire envelope or a schema, and none of them
reaches glaze. `core/async.hpp` is a facade over the four headers that carry
them — it declares nothing of its own, so including it is exactly equivalent to
including all four.

It exists because the obvious header to reach for is `bridge.hpp`, and that
costs roughly three times as much. Measured with clang
22.1.8, `-O2 -fsyntax-only`, one translation unit per header, best of three:

| header | CPU s | preprocessed lines |
|---|---|---|
| `core/executor.hpp` | 1.16 | 124,855 |
| `core/completion.hpp` | 1.20 | 127,450 |
| `core/strand.hpp` | 1.20 | 127,217 |
| **`core/async.hpp`** (all four) | **1.23** | **127,675** |
| `core/bridge.hpp` | 3.76 | 267,827 |

The whole async surface costs what one of its headers costs, because they
already share almost all of their own includes. See
[`journal.md`, "Why the codec is a separate header"](../journal/journal.md#why-the-codec-is-a-separate-header)
for the same argument applied to the journal codec.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Callback dispatch | **Posted to `IExecutor`, never direct** | Ensures callbacks run on the intended thread (e.g. GUI/main thread) regardless of which thread completes the operation. |
| No lock | **Settle claims atomically and posts its delivery; the handler side is owned by `cbExec`** | Producer and consumer never touch the same mutable field: the outcome is write-once and published by a release store, and the handler lists are touched only on the owner, ordered by its queue. One post per settle is the price, paid even with no handler attached. |
| Settle stores, owner delivers | **The claimant stores the outcome where it settles; only delivery is posted** | A completion settled synchronously — a `LocalBackend` bind on the owner's thread outside its tasks — is readable at once by `takeSettled`, so a handler over such a backend is bound when its constructor returns. Storing on the owner would make it look unsettled until the owner next ran. |
| Late attach | **Posted, never run inside `then()`** | An earlier handler may still be in a queued `deliver()`; running a later one inline would overtake it. |
| Orphan detection | **Destructor logs through `logError`** | Prevents silent loss of error information when a `Completion` is destroyed without an `onError` handler. The exception is re-thrown just to extract a message (`what()` for a `std::exception`, a generic string otherwise), which is logged; the `logError` call is itself wrapped in an empty `catch (...)` so the `noexcept` destructor never lets an exception escape. |
| No executor callback | **`cbExec == nullptr` disables posting and attaching** | A `Completion` without an executor is a write-only endpoint — the producer can set a value or error, but no callback is ever invoked. An abandoned *error* is not silenced, though: an attach sets nothing on such a state, so the error still reaches the destructor's orphan logger. |
| First-result-wins | **`setValue`/`setException` claim `settled` with an atomic exchange; the loser returns** | An asynchronous operation should complete exactly once; subsequent calls are silently ignored, before they allocate a post. |
| Move-only handle | **`Completion` is move-only, `CompletionState` is shared via `shared_ptr`** | The handle is owned by one consumer at a time; the shared state is owned jointly by the producer and any consumer that has moved the handle. |
| Empty completion | **Null state pointer makes `then`/`onError` no-ops** | Default-constructed `Completion` is a safe placeholder that never signals. |
| Value handling on dispatch | **Both paths read `*value` in place; neither copies nor moves it** | Handlers are erased as `std::function<void(const T&)>` and the dispatch closures capture `shared_from_this()`, so the copy budget is exactly one per by-value handler and zero per `const T&` handler, whenever it attached. `value` is never consumed, so a `then()` attached after settling still sees the genuine result, and `T` need only be move-constructible. See [Value-handling contract](#value-handling-contract). |
| Handler fan-out | **`onOk`/`onErr` are `std::vector`s, appended to on each attach** | A single-slot field would let a second `onError()` (or `then()`) on the same still-pending `Completion` silently replace the first handler. Composing (invoking every attached handler, in order) matches the mental model of an observer list and is what call sites composing behaviour via repeated attach expect. |
| Per-handler exception isolation | **Each handler invocation — composed at settlement, or fired alone by a late attach — is wrapped in its own `try`/`catch (...)`, logged via `logError` and swallowed** | Fan-out means every attached handler should get its turn regardless of what an earlier one does. Without per-handler isolation, one throwing handler would unwind the whole posted closure and silently skip every handler attached after it — turning a single misbehaving consumer into an outage for unrelated ones sharing the same `Completion`. |
| Public settleable-promise seam | **`Completion<T>::Promise`, reachable only via `makeSettleable()`** | Without it, test code needing a `Completion<T>` it can resolve/reject on demand has no seam except reaching into `morph::async::detail::CompletionState<T>` directly. `Promise`'s constructor is private and `friend`ed only to `Completion<T>`, so `detail::CompletionState<T>` never has to appear in a caller's own code. |

## Limitations

`Completion<T>` is deliberately a **leaf callback primitive**, not a general
future/promise or a monadic async type. Its scope is narrow by design:

- **No transformation, no chaining.** `then()` returns `*this` (the same
  `Completion<T>&`), purely so a `then().onError()` pair reads fluently. It does
  **not** return a new `Completion<U>` for a transformed result — there is no
  `T → U` mapping and no way to chain one asynchronous step onto another. To
  sequence work, the consumer must start a fresh operation from inside the
  handler.
- **No *work* cancellation.** There is no handle to cancel an outstanding
  operation; once started, it runs to completion (or is abandoned).
  `Bridge::setExecuteDeadline` (see
  [Client-side execute deadline](#client-side-execute-deadline)) is not an
  exception to this: it bounds how long the *caller* waits by resolving the
  state early, and does nothing to the work still in flight underneath.
  **Delivery**, by contrast, *can* be stopped — see
  [Lifetime and stop gating](#lifetime-and-stop-gating). The distinction is
  sharp and deliberate: a `CallbackScope` says "do not hand me this result",
  never "stop producing it". Work-side cancellation is not offered at all.
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

- Work cancellation — there is no mechanism to cancel an outstanding
  *operation*. Stopping *delivery* of its result is
  [`CallbackScope`](callback_scope.md); the two are different things.
- Multiple values — `Completion<T>` is a single-result primitive.
- Synchronous blocking — there is no `wait()` or `get()`; the API is
  callback-only.
- Transformation / composition — see [Limitations](#limitations).

## Lifetime annotations

Every fluent method — `then`, `onError`, their scope- and token-gated overloads,
`thenDetached`, `onErrorDetached` — returns `*this`, and each marks its implicit
object parameter `MORPH_LIFETIMEBOUND` (`morph/attributes.hpp`). Chaining on a
temporary stays legal, since the whole chain runs inside one full-expression;
what the annotation catches is binding the returned reference to something that
outlives the `Completion`. See [concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#morph_lifetimebound--the-must-outlive-rules-told-to-the-compiler).

## Cross-references

- [`coroutines.md`](coroutines.md) — awaiting a `Completion<T>` from a
  coroutine (`operator co_await() &&`), where it resumes, and how a stop
  withdraws the await.
- [`executor.md`](executor.md) — `IExecutor` and its implementations, and
  `runningOn`; `cbExec` is the executor that owns the completion's handler
  side and on which every callback runs.
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
- [`callback_scope.md`](callback_scope.md) — `CallbackScope`/`CallbackToken`,
  the gate behind the `then(scope, fn)` / `onError(scope, fn)` overloads and the
  `thenDetached` / `onErrorDetached` spellings.
- [Settleable promise seam](#settleable-promise-seam--completiontpromise) —
  `Completion<T>::makeSettleable()`, the public seam test code uses in place of
  a `Bridge`/`IBackend` round trip.
