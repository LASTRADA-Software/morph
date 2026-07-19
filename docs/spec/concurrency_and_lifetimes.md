# Concurrency & lifetimes

Cross-cutting spec for morph's threading model and its object-ownership /
destruction-ordering rules. The per-type specs each describe their own local
guarantees; this document collects the framework-wide invariants — *which code
runs on which thread* and *who must outlive whom* — in one place, because the
subtle footguns live in the seams **between** subsystems, not inside any one of
them.

Read this before wiring up a `Bridge`, a `RemoteServer`, or a
`ThreadPoolExecutor`/`StrandExecutor` pair, and before changing any teardown
sequence.

## Contents

- [The one rule: everything goes through `IExecutor::post`](#the-one-rule-everything-goes-through-iexecutorpost)
- [Thread roles — what runs where](#thread-roles--what-runs-where)
- [The strand model — one strand per `ModelId`](#the-strand-model--one-strand-per-modelid)
- [Completion callback marshalling](#completion-callback-marshalling)
- [Destruction ordering — who must outlive whom](#destruction-ordering--who-must-outlive-whom)
- [Synchronisation specifics per subsystem](#synchronisation-specifics-per-subsystem)
- [`Completion` / `CompletionState` thread-safety](#completion--completionstate-thread-safety)
- [Quick cheat-sheet](#quick-cheat-sheet)
- [Cross-references](#cross-references)

## The one rule: everything goes through `IExecutor::post`

morph has no ad-hoc threads scattered through the dispatch path. All asynchronous
work is scheduled by calling `IExecutor::post(std::function<void()>)`
(`executor.hpp`). The interface says nothing about *where* the task runs — that
is the concrete executor's job:

| Executor | Thread(s) | Role |
|---|---|---|
| `ThreadPoolExecutor` | N fixed worker threads, FIFO MPMC queue | Runs model work (`Model::execute`) and remote message processing. |
| `MainThreadExecutor` | The thread that calls `runFor()` | Stand-in "GUI" thread in non-Qt tests; pumped manually. |
| `QtExecutor` | The Qt GUI thread | Real GUI executor; posts via `QMetaObject::invokeMethod(Qt::QueuedConnection)`. |
| `StrandExecutor` | *Borrows* a base `IExecutor` (usually the pool) | Serialises tasks per `ModelId` on top of the base executor. It owns no thread. |

Because everything funnels through `post`, the concurrency model is fully
determined by *which executor a task is posted to*. Model code never blocks the
GUI, and the GUI thread never runs model work — the executors enforce the split.

## Thread roles — what runs where

| Work | Runs on | Scheduled by |
|---|---|---|
| `Model::execute(action)` (local mode) | Worker pool, inside a per-`ModelId` strand | `LocalBackend::execute` → `StrandExecutor::post` |
| `Model::onBackendChanged()` (local mode) | Worker pool, inside the model's per-`ModelId` strand (serialised with its `execute`) | `LocalBackend::notifyBackendChanged` → `StrandExecutor::post` |
| `ActionDispatcher::dispatch` → `Model::execute` (remote mode) | `RemoteServer`'s worker pool, inside a per-`ModelId` strand | `RemoteServer::dispatchExecute` → `StrandExecutor::post` |
| Remote message decode / envelope handling | `RemoteServer`'s worker pool | `RemoteServer::handle` → `_pool.post` |
| `Completion::then` / `onError` callbacks | The `cbExec` executor supplied at dispatch (the GUI executor for `BridgeHandler`) | `CompletionState::setValue`/`setException` → `cbExec->post` |
| Subscription result / error sinks (`BridgeHandler::subscribe`) | The handler's `guiExec` | Same as `Completion` callbacks — they *are* completion callbacks |
| Connectivity probe + `onOffline`/`onOnline` callbacks | `NetworkMonitor`'s dedicated **probe thread** | `NetworkMonitor::run` |
| `ReconnectCoordinator::onOnline`/`onOffline` | The caller's thread (host posts it to a worker; **not** the probe thread) | Host wiring |
| `SyncWorker::run` (offline-queue replay) | The caller's thread; concurrent calls serialised | Host wiring / `ReconnectCoordinator::replay` |
| Backend reconnect handler (re-register bindings) | The backend's transport thread | `IBackend::setReconnectHandler` callback |
| Log sink invocation | Whatever thread called `log*()` | `morph::log::detail::log` |

Key consequences:

- **A model author writes single-threaded code.** For a given `ModelId`, the
  strand guarantees `execute()` is never re-entered concurrently, so per-model
  state needs no locking. Different models run in parallel across pool threads.
- **The GUI thread is never blocked by dispatch.** `executeVia` returns a
  `Completion` immediately; the actual work runs on the pool and the result is
  marshalled back to the GUI executor.
- **Probe callbacks must not block** (see below) — they run on the single probe
  thread, and a blocking callback stalls all future probes.

## The strand model — one strand per `ModelId`

`StrandExecutor` (`strand.hpp`) sits on top of an arbitrary base `IExecutor` and
turns it into a set of per-key serial queues:

- `post(ModelId key, task)` appends `task` to the strand for `key`. Tasks with
  the same key run in FIFO order with **no overlap**; tasks with different keys
  may run concurrently on different pool threads. This is what removes the need
  for per-model mutexes.
- Each strand is a `shared_ptr<Strand>` in a map guarded by `_mapMtx`. When a
  strand's queue drains, the map entry is erased. The invariant is: **at most one
  live strand per `ModelId`, and any `running` strand is the one currently in the
  map** — that is what keeps a key's tasks from overlapping. Both sides that can
  break it hold `_mapMtx` across their *whole* decision: `post()` takes `_mapMtx`,
  looks up (or creates) the strand, and — still under `_mapMtx` — takes
  `strand->mtx` to push the task and set `running`; the drain step takes the same
  two locks in the same order to decide "keep running vs. erase". An earlier
  design held the combined lock only inside the drain step while `post()` re-armed
  the strand under `strand->mtx` alone (after releasing `_mapMtx`); a concurrent
  drain could then erase the strand in that gap, orphaning a live strand, and the
  next `post(key)` created a *second* strand for the same key — two strands
  running the model's tasks concurrently (a data race). Serialising the lookup,
  the re-arm, and the erase under `_mapMtx` closes that window: the drain never
  erases a strand whose `pending` queue is non-empty, and a strand that becomes
  `running` in `post()` is guaranteed to still be the map entry. Lock order is
  always `_mapMtx` → `strand->mtx`, acquired as two sequential `scoped_lock`s at
  both sites, so no lock-ordering deadlock.
- `_inFlight` counts strand lambdas currently dispatched to the base executor.
  The destructor waits on `_cv` until `_inFlight == 0` before destroying the
  map, so no pool thread can touch `_strands` after the executor is gone.

`LocalBackend` owns one `StrandExecutor` over the worker pool; `RemoteServer`
owns another over its worker pool. Both post model work keyed by `ModelId`.

## Completion callback marshalling

`Completion<T>` (`completion.hpp`) is the seam between the producing thread (a
pool/strand thread) and the consuming thread (the GUI executor). The invariant:
**`.then` / `.onError` callbacks are always posted to the `cbExec` executor
supplied at construction, never invoked directly on the producing thread.** So a
callback attached from the GUI runs back on the GUI thread even though the value
was produced on a pool thread.

If `cbExec` is `nullptr`, callbacks are never delivered (silently dropped) — and
because delivery is the only thing that discharges an error, a **null `cbExec`
forces orphan logging even when an `onError` handler *was* attached.** Both
`setException` and `attachOnError` set `onErrAttached = (cbExec != nullptr)`, so
with no executor the error still reaches the orphan logger in `~CompletionState`
rather than vanishing into a handler that can never run.

See [`Completion` / `CompletionState` thread-safety](#completion--completionstate-thread-safety)
for the internal locking and the one non-mutex-guarded field.

## Destruction ordering — who must outlive whom

This is the section to read before writing any teardown code. Several of these
rules encode recent fixes to real deadlocks and use-after-frees.

| This… | must outlive / be destroyed after… | Consequence if violated |
|---|---|---|
| base `IExecutor` (e.g. `ThreadPoolExecutor`) | the `StrandExecutor` built on it | **Deadlock** in `~StrandExecutor` (see below) |
| `Bridge` | its `BridgeHandler`s (for normal `execute`/`set` calls) | Fine at teardown (order-independent, see below); a *call* on a handler whose bridge is gone is still UB |
| `RemoteServer` (heap, `make_shared`) | every `SimulatedRemoteBackend`/transport holding `RemoteServer&` | Dangling `RemoteServer&` → use-after-free |
| worker pool | the backend that posts to it (`LocalBackend`, `RemoteServer`) | Same deadlock/UAF family as the strand rule |
| `session::Context` passed to `ScopedContext` | the scope in which the model runs | Dangling thread-local `Context*` |

### base `IExecutor` must outlive its `StrandExecutor` — and keep running

This is the sharpest edge in the framework. `~StrandExecutor` **blocks** until
`_inFlight == 0`, i.e. until every lambda it dispatched to the base executor has
actually run. `~ThreadPoolExecutor` **drains** its queue — after `_stop` is set,
workers keep running already-queued tasks until the queue is empty, then join —
so tasks already queued when destruction begins do run and decrement `_inFlight`.

Draining is not enough to make arbitrary teardown order safe, because the strand
can still be *dispatching* while the pool tears down. If you destroy the pool
**first**, two things go wrong: an in-flight strand lambda may call
`base->post()` on a pool whose destructor has already run (undefined behaviour —
use-after-free on the pool), and a lambda posted after the workers have observed
`_stop && _q.empty()` and exited is never run, so its `--_inFlight` never happens
and `~StrandExecutor` waits forever → **deadlock**. The pool must be destroyed
*after* every `StrandExecutor` (and hence after `LocalBackend` / `RemoteServer`,
which own the strands).

Corollary: **no `post()` may race or follow `~StrandExecutor`.** Once the strand
executor's destructor has started, posting to it is undefined. Stop feeding a
backend before you tear it down.

Correct teardown order (innermost-first):

```
handlers  →  Bridge  →  backend (LocalBackend / SimulatedRemoteBackend)
          →  RemoteServer (if remote)  →  worker pool (ThreadPoolExecutor)
```

Declared as members, list the pool **first** so it is destroyed **last**.

### `Bridge` vs. `BridgeHandler` — teardown is now order-independent

Normal operation still requires the `Bridge` to outlive its handlers: every
`execute` / `set` call dereferences `_bridge`. But **teardown order no longer
matters** (recent fix). Each `BridgeHandler` captures a `weak_ptr<const void>`
liveness token from the `Bridge` (`Bridge::liveness()`). In
`~BridgeHandler`, it locks the token first:

- token still valid → the `Bridge` is alive → deregister normally.
- token expired → the `Bridge` is already gone → **no-op**, skipping the
  deregistration that would otherwise dereference a dangling `Bridge&`.

Previously, destroying the `Bridge` before its handlers was a use-after-free.
It is now defined behaviour (a silent no-op deregistration). Destroying the
bridge first is still discouraged, but it is no longer unsafe.

### `RemoteServer` must be `make_shared` and outlive its transports

`RemoteServer` derives from `enable_shared_from_this` and **must** be created via
`std::make_shared`. `handle()` captures `shared_from_this()` into the pool task,
so the server object survives until that task completes.

**The self-capture must be re-established for the execute strand hop.** An
`execute` envelope does not finish inside the pool task: `dispatchExecute` posts
a *second* task onto the model's strand and returns, at which point `handle()`'s
pool task completes and releases its `shared_from_this()`. If the strand task
did not itself co-own the server, the last external `shared_ptr` dropping right
after `handle()` returned would free the server — and with it the
`_dispatcher`/`_registry` *reference members* the strand task reads — before the
task runs (a use-after-free), or the reply callback would be destroyed with the
server and the client's `Completion` would hang forever. So the `dispatchExecute`
strand task **also** captures `shared_from_this()` (`self = shared_from_this()`)
and reaches the dispatcher through `self->_dispatcher`, never a bare reference
capture. The server is thus kept alive across the pool→strand hop until the
reply fires; only then does the strand task release its self-reference and the
server may be destroyed. This holds even when the owning `shared_ptr` is dropped
while work is in flight — which is why the worker pool can safely outlive the
server *reference*.

`SimulatedRemoteBackend` (and any real transport) stores a bare `RemoteServer&`.
That reference must remain valid for the backend's whole life: the
`RemoteServer` (its owning `shared_ptr`) must outlive every backend/transport
that points at it. The `make_shared` requirement guarantees in-flight *tasks*
are safe; it does **not** rescue a dangling `RemoteServer&` held by a backend.

Model-destruction-mid-flight is safe on both backends: the strand task captures
a `shared_ptr` copy of the `IModelHolder` (`holder = std::move(holder)` in the
`post` lambda), so a concurrent `deregisterModel` that erases the map entry
cannot free the model out from under a running action.

### Synchronous re-entry into a backend from a pool thread

A `BridgeHandler` can be constructed *from inside* a running action (e.g. a
model that registers a sub-model), which means the constructor's
`registerModelWithContext` call may run on the very worker-pool thread that is
executing the action. On the remote path this reaches `RemoteServer` on a pool
thread. That is why control messages (`register` / `deregister`) go through the
**synchronous** `handleInline`, which runs `dispatchMessage` inline instead of
posting to the pool — posting would deadlock if the pool were saturated by the
in-flight action. `handleInline` **rejects `execute`**: an `execute` reply is
produced asynchronously on the model strand, *after* `handleInline` has returned
and destroyed the local reply buffer the deferred callback would write into — a
dangling-write hazard. See [backend.md](core/backend.md) for the exact wiring.

### `cancelPending` — snapshot-then-deliver, weak-ptr tracked

Both backends track their in-flight completions as `weak_ptr<CompletionState>`
in a `_pending` vector guarded by `_pendingMtx`. `cancelPending(exc)` **swaps the
vector out under the lock and delivers `exc` outside it**, so no completion
callback runs while `_pendingMtx` is held — otherwise a callback that re-enters
the backend would deadlock. The `weak_ptr` tracking is what lets an already
resolved-and-destroyed completion be skipped (its state is gone) rather than
resurrected, and `setException` on a still-live-but-already-`ready` state is the
idempotent no-op described below.

## Synchronisation specifics per subsystem

### `Bridge::switchBackend` — atomic, exception-safe; `onBackendChanged` is posted to the model strand

`switchBackend` holds `Bridge::_mtx` for its **entire** duration. Within that
lock it uses a **stage-all-then-commit** protocol (recent fix):

1. **Phase 1** — register every live binding on the new backend, staging
   `(binding, newId)` pairs *without mutating any `currentId`*. If any
   registration throws (a plausible remote/transport failure), it rolls back the
   registrations already made and rethrows, leaving the old backend and every
   `currentId` untouched. The switch is therefore **atomic**: it either fully
   succeeds or is a complete no-op.
2. **Phase 2** — commit: publish the new ids, swap the backend pointer, and call
   `notifyBackendChanged()` (still under `_mtx`).

Cancellation of the outgoing backend's pending completions
(`cancelPending(BackendChangedError)`) happens **outside** `_mtx`, because it
delivers callbacks through the caller's GUI executor and the bridge must never
hold `_mtx` while user code runs.

**`notifyBackendChanged()` runs under `_mtx`, but only posts — it does not run
model code under the lock.** `LocalBackend::notifyBackendChanged` dispatches each
model's `onBackendChanged()` onto that model's own strand (the per-`ModelId`
serial queue) rather than invoking it inline. The cheap `post()` happens under
`_mtx`; the callback body runs later on a pool thread with `_mtx` free. This is
what makes the model-side contract both true and safe:

- **Strand-serialised, no model locking.** `onBackendChanged()` runs on the same
  strand as `execute()` for that model, so the two never overlap — a model
  draining a queue and mutating its own counters there needs no locking. This is
  the guarantee `offline.md`'s conflict-resolution path relies on. (Earlier the
  callback ran inline on the switch caller's thread, which contradicted that
  claim; the conflict-resolution tests only passed because they slept.)
- **`registerHandler` / `deregisterHandler` are safe from `onBackendChanged()`.**
  They re-acquire `_mtx`, but on a *different* thread (the strand) with the lock
  already released by the switch caller — no self-deadlock. This was the
  reentrant-deadlock fix: the old inline-under-`_mtx` design hung here.
- **`switchBackend` from `onBackendChanged()` is still unsupported.** Not because
  of `_mtx` (that is free now) but because the callback runs on the *outgoing*
  backend's strand; a nested switch drops the last reference to that backend when
  it returns, and `~StrandExecutor` blocks until in-flight strand tasks finish —
  including the one calling it — a self-join hang. Re-register or reconcile from
  the callback; do not swap the backend again inside it.
- **`executeVia` IS safe from `onBackendChanged()`.** It never takes `_mtx`; it
  reads a **lock-free snapshot** of the backend `shared_ptr` (via `_backendMtx`,
  a short separate lock) and the binding's `std::atomic currentId`. A concurrent
  switch cannot free the old backend out from under the call because its
  `shared_ptr` refcount is still > 0; the call either succeeds or fails with
  "model not found", both safe.

Lock ordering remains `Bridge::_mtx` before `LocalBackend::_regMtx`.

### Reconnect handler — fires on the transport thread, guarded against a dead `Bridge`

The reconnect handler a `Bridge` installs on its backend
(`installReconnectHandler`) runs on the **backend's transport thread**, not the
GUI or pool thread, and it captures the bare `Bridge* this` (it needs `_mtx`,
`_handlers`, and `loadBackend()`). A backend can be co-owned or otherwise outlive
its `Bridge`, so a reconnect firing after the `Bridge` is destroyed would
dereference freed memory. Two layers prevent this:

- **`~Bridge` clears the active backend's handler** (`setReconnectHandler(nullptr)`)
  before cancelling pending work, and `switchBackend` clears the *outgoing*
  backend's handler after the swap. This removes the callback from the common
  path. The clear is done **outside `Bridge::_mtx`** — `setReconnectHandler` only
  stores a callback and never re-enters the bridge, so there is no lock-ordering
  hazard with the handler body (which does take `_mtx`).
- **The handler guards on the `_liveness` token.** It captures a
  `weak_ptr<const void>` to the bridge's `_liveness` (the same token
  `BridgeHandler` uses) and, on invocation, locks it *before* touching `this`. If
  the token has expired the `Bridge` is gone and the handler returns immediately.
  This covers the race the clear alone cannot: a reconnect already latched on the
  transport thread at the moment `~Bridge` runs. After the liveness check it also
  re-checks `pinned == loadBackend()` to ignore a reconnect for a backend the
  bridge has since switched away from.

### `QtWebSocketBackend::sendSync` — a disconnect must not freeze the Qt thread

`registerModel` is synchronous: `sendSync` sends the envelope and pumps a
**nested `QEventLoop`** on the Qt thread until the reply arrives. Everything runs
on the single Qt thread, so the parked loop is unblocked only by a slot on that
same thread. The rules that keep it from hanging:

- **The `disconnected` slot quits a parked sync loop.** If `_syncLoop` is
  non-null when the socket drops, the slot clears `_pendingReply` and calls
  `_syncLoop->quit()`. Without this, a disconnect while a register reply is
  outstanding would leave the nested loop running forever — freezing the Qt
  thread. On return `sendSync` sees an empty `_pendingReply` and throws
  `"disconnected"`, which `registerModel` reports as
  `"register failed: disconnected"`.
- **`sendSync` fails fast if already disconnected** — it checks `_connected`
  before parking and throws rather than waiting for a reply that will never come.
- **`sendSync` is non-reentrant.** `_syncLoop` is a single pointer; a second sync
  send while one is parked would clobber it and cross the two replies. Register
  calls run on the Qt thread and never re-enter `sendSync` from within a parked
  loop, but the guard throws on reentry rather than corrupting state if that
  assumption is ever violated.

### `NetworkMonitor` — probe-thread callbacks must not block

`onOffline` / `onOnline` run **directly on the probe thread** (`NetworkMonitor::run`).
Constraints:

- **Callbacks must not block.** A blocking callback stalls the probe loop and
  delays or prevents all subsequent probes. The intended body is short — set an
  atomic flag or `post()` to an executor and return.
- **Callbacks must not throw** through the monitor's expectations (the probe
  itself is wrapped in `safeProbe`, which swallows exceptions).
- **`stop()` from within a callback self-detaches.** `stop()` normally joins the
  probe thread, but joining from the probe thread itself would deadlock. It
  detects `this_thread == probe thread` and **detaches** instead; the destructor
  then spin-waits on `_runExited` until the thread exits. `stop()` is idempotent.
- `isOnline()` reads an `std::atomic<bool>` — safe from any thread at any time.

### `ReconnectCoordinator` — mutex held across the whole retry loop

`onOnline()` holds `_mtx` for the **entire** reconnect → activate → bind → replay
loop, including all retry sleeps; `onOffline()` takes the same mutex. So the two
are mutually exclusive and a second concurrent caller blocks until the first
finishes. The coordinator owns no thread and does no I/O — it runs synchronously
on the caller's thread, and the host is expected to post it onto a worker
executor, **not** call it on the probe thread. The strict step order
(reconnect → activatePrimary → bindContext → replay) is an invariant: replay
never runs before context is bound.

### Registries — populated at static-init, then read-only

`ModelRegistryFactory`, `ActionDispatcher` (`registry.hpp`), and
`ActionExecuteRegistry` (`bridge.hpp`) are process-level singletons populated by
the `BRIDGE_REGISTER_MODEL` / `BRIDGE_REGISTER_ACTION` macros at **static-init
time**, single-threaded, before `main` runs. After that they are **read-only**.
This is precisely what makes concurrent dispatch safe without locking them: many
pool threads look up runners/factories concurrently, and concurrent reads of a
never-again-mutated `unordered_map` need no synchronisation. Registering a new
action *after* threads are running is unsupported and would be a data race.

### Logger — lock-free reject, mutex-guarded sink, non-recursive

`morph::log` (`logger.hpp`) has two tiers:

- **Fast path:** the minimum level is an `std::atomic<LogLevel>`. A message below
  the threshold is rejected **lock-free**, before touching the mutex or
  formatting the string (`logFormat` checks the level before `std::format`).
- **Sink path:** the sink is invoked while a global `std::mutex` is held, so sink
  calls are serialised and `setLogger`/`ScopedLoggerOverride` swap safely.

Because that mutex is **non-recursive**, a sink **must not** call back into
`morph::log` — no `log*()`, no `setLogger`, no `ScopedLoggerOverride` — from
inside the sink, or it **self-deadlocks**. Sinks should also not block for long,
since they serialise all logging.

### Thread-local session `Context` — valid only on the dispatch thread

`session::current()` (`session.hpp`) returns a thread-local `Context*` installed
by a `ScopedContext` around the model call — `LocalBackend::execute` on the local
path, `RemoteServer::dispatchExecute` on the remote path. It is valid **only on
the dispatch (strand/pool) thread, only for the duration of that `execute()`**.
It is **not** visible from the GUI thread or from a `Completion` callback, and
its address dangles once the scope exits. Models read it synchronously inside
`execute()`; they must not stash the pointer for later use.

Recent addition: the `Context` also carries the **authoritative principal**. A
verifying authorizer's `authenticate()` (e.g. `SigningAuthorizer`) overwrites
`Context::principal` with the identity it extracted from a valid token
**before** dispatch, so `session::current()->principal` read inside a model is
the authenticated identity, not the client's unverified claim. See
[security.md](security.md).

## `Completion` / `CompletionState` thread-safety

`CompletionState<T>` (`completion.hpp`) is shared between the producing thread
and the attaching thread, so its mutable state is mutex-protected:

| Field | Protection |
|---|---|
| `value`, `error`, `ready`, `onOk`, `onErr`, `onErrAttached` | `mtx` (all reads/writes) |
| `cbExec` | **Not** mutex-guarded — happens-before, see below |

`setValue` / `setException` are idempotent: once `ready` is set, later calls
return immediately. This is what lets a backend `cancelPending(...)` a completion
and then have a late server reply arrive as a harmless no-op.

**`cbExec` is a happens-before requirement, not a lock.** It is written once, in
the `Completion` constructor, before the state is published to any other thread,
and only read afterward. The contract is: *construct the `Completion` handle
(which sets `cbExec`) before the producing thread can call
`setValue`/`setException`.* The backends honour this — they build the
`Completion` object before posting the strand/transport task that resolves the
state. Guarding `cbExec` with `mtx` would be redundant given that ordering, so it
is deliberately left unguarded.

## Quick cheat-sheet

Destroy in this order (or declare members so the reverse holds):

```
BridgeHandler(s)          ← first to go (or any order vs. Bridge, thanks to the liveness token)
  Bridge
    backend               ← LocalBackend / SimulatedRemoteBackend
      RemoteServer         ← only in remote mode; keep its shared_ptr alive this long
        ThreadPoolExecutor ← LAST: it must outlive every StrandExecutor it backs
```

One-liners to remember:

- Never destroy the pool before the strand/backend → `~StrandExecutor` deadlocks.
- Never `post()` to a `StrandExecutor` whose destructor has begun.
- `onBackendChanged()` runs posted on the model's strand (not inline under
  `_mtx`): `registerHandler`/`deregisterHandler`/`executeVia` are safe from it,
  but never call `switchBackend` there (it self-joins the strand it runs on).
- Never block or re-enter from a `NetworkMonitor` callback (probe thread).
- Never log from inside a log sink (non-recursive mutex).
- Never read `session::current()` off the dispatch thread or after `execute()`
  returns.
- Register all models/actions at static-init; treat the registries as read-only
  afterward.

## Cross-references

- [`executor.md`](core/executor.md) — `IExecutor`, `ThreadPoolExecutor`,
  `StrandExecutor`, `ModelId`; the "destroy strand before base pool" rule in
  detail.
- [`completion.md`](core/completion.md) — `Completion<T>` / `CompletionState<T>`
  internals and orphan-error logging.
- [`bridge.md`](core/bridge.md) — `Bridge`, `BridgeHandler`, `switchBackend`,
  `executeVia`, the liveness token.
- [`backend.md`](core/backend.md) — `LocalBackend`, `RemoteServer`,
  `SimulatedRemoteBackend`, `cancelPending`, the `make_shared` requirement.
- [`offline.md`](offline/offline.md) — `NetworkMonitor`, `ReconnectCoordinator`,
  `SyncWorker` wiring and the reconnect ordering guarantee.
- [`registry.md`](core/registry.md) — `ActionDispatcher` / `ModelRegistryFactory` and
  the static-init registration model.
- [`logger.md`](core/logger.md) — the logging fast path and sink contract.
- [`session.md`](session/session.md) — `Context`, the thread-local, and `IAuthorizer`.
- [`security.md`](security.md) — where the authoritative principal comes from and
  the `RemoteServer` enforcement points.
- `error_handling.md` — the framework-wide error-propagation story that the
  per-subsystem exception handling here plugs into (also summarised under
  *Error propagation* in `../ARCHITECTURE.md`).
- *Thread safety* table in `../ARCHITECTURE.md` — the high-level summary this
  spec expands on.
