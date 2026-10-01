# Concurrency & lifetimes

Cross-cutting spec for morph's threading model and its object-ownership /
destruction-ordering rules. The per-type specs each describe their own local
guarantees; this document collects the framework-wide invariants — *which code
runs on which thread* and *who must outlive whom* — in one place, because the
subtle footguns live in the seams **between** subsystems, not inside any one of
them.

Read this before wiring up a `Bridge`, a `RemoteServer`, or a
`ThreadPoolExecutor` and the strands over it, and before changing any teardown
sequence.

## Contents

- [The one rule: everything goes through `IExecutor::post`](#the-one-rule-everything-goes-through-iexecutorpost)
- [Thread roles — what runs where](#thread-roles--what-runs-where)
- [The strand model — one strand per `ModelId`](#the-strand-model--one-strand-per-modelid)
- [Completion callback marshalling](#completion-callback-marshalling)
- [Destruction ordering — who must outlive whom](#destruction-ordering--who-must-outlive-whom)
- [Gating callbacks on a receiver's lifetime — `CallbackScope`](#gating-callbacks-on-a-receivers-lifetime--callbackscope)
- [Synchronisation specifics per subsystem](#synchronisation-specifics-per-subsystem)
- [`Completion` / `CompletionState` thread-safety](#completion--completionstate-thread-safety)
- [`MORPH_LIFETIMEBOUND` — the "must outlive" rules, told to the compiler](#morph_lifetimebound--the-must-outlive-rules-told-to-the-compiler)
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
| `ModelStrands` | *Borrows* a base `IExecutor` (usually the pool) | Serialises tasks per `ModelId` on top of the base executor: core-cpp's `KeyedStrands`, which reaches the base through the base's `coreExecutor()`. It owns no thread. |
| `OwnerStrand` | *Borrows* a base `IExecutor` | One core-cpp `Strand` over the base that is itself a morph executor: the owner of one component's state. `RemoteServer` owns one over its pool (the server strand), `ReconnectCoordinator` one over the executor it is given (the offline strand). It owns no thread. |
| `exec::IoLoop` | One thread natively; host-pumped under single-threaded WebAssembly | The I/O loop (`io_loop.hpp`): a core-cpp `PlatformLoop` that owns every `morph::net` socket, every `TimeoutScheduler` timer and `NetworkMonitor`'s probe. The application constructs one and injects it into each; `IoLoop::post` is the one way in from another thread. |

A strand's own unit of work is a coroutine resumption, not a callable: it
queues its pump on the base once per turn, through the adapter, and a posted
callable is one task of that turn. That is still one `IExecutor::post` per turn.

Because everything funnels through `post`, the concurrency model is fully
determined by *which executor a task is posted to*. Model code never blocks the
GUI, and the GUI thread never runs model work — the executors enforce the split.

## Thread roles — what runs where

| Work | Runs on | Scheduled by |
|---|---|---|
| `Model::execute(action)` (local mode) | Worker pool, inside a per-`ModelId` strand | `LocalBackend::execute` → `ModelStrands::post` |
| `Model::onBackendChanged()` (local mode) | Worker pool, inside the model's per-`ModelId` strand (serialised with its `execute`) | `LocalBackend::notifyBackendChanged` → `ModelStrands::post` |
| `ActionDispatcher::dispatch` → `Model::execute` (remote mode) | `RemoteServer`'s worker pool, inside a per-`ModelId` strand | `RemoteServer::dispatchExecute`, on the server strand → `ModelStrands::post` |
| Remote message decode | The transport's thread (`handle()`'s caller) | — |
| Envelope handling, `execute` admission, the registry, connection scopes, `health()`/`drainedWithin()`/`beginShutdown()` | `RemoteServer`'s server strand, on its worker pool | `RemoteServer::handle` and every public verb → `OwnerStrand::postTask` |
| `Completion::then` / `onError` callbacks | The `cbExec` executor supplied at dispatch (the GUI executor for `BridgeHandler`) | `CompletionState::setValue`/`setException` → `cbExec->post` |
| Subscription result / error sinks (`BridgeHandler::subscribe`) | The handler's `guiExec` | Same as `Completion` callbacks — they *are* completion callbacks |
| Connectivity probe + `onOffline`/`onOnline` callbacks | **The I/O loop** | A loop timer `NetworkMonitor` re-arms every `probeInterval` |
| `morph::net` socket I/O: dial, handshake, reads, writes, accept, reply writes | **The I/O loop** | Flows the components spawn on it; verbs and `RemoteServer` replies post to it |
| `TimeoutScheduler` callbacks (execute deadlines, `delay()`) | **The I/O loop** — the injected one, or the scheduler's own | A loop timer armed by a posted `schedule()` |
| `ReconnectCoordinator::onOnline`/`onOffline` bodies (reconnect → activate → bind → replay, retry sleeps included) | The offline strand, over the executor the coordinator was given (a worker pool; **not** the I/O loop) | `onOnline()`/`onOffline()` post, from any thread |
| `SyncWorker::run`'s drain (offline-queue replay) | The worker's owner — the offline strand when given `ReconnectCoordinator::strand()`; inline when `run()` is already on it | `run()` posts, from any other thread; the coordinator's `replay` step runs it inline |
| Backend reconnect handler (re-bind the bridge's handlers) | The executor it was installed with — the bridge's owner. Never the backend's own thread | The backend posts it after a reconnect (`IBackend::setReconnectHandler(handler, exec)`) |
| Bind and promote replies (`IBackend::bindModel`/`promoteModel`) | The executor the call named — the bridge's owner | The backend settles the `Completion` on its own thread; `Completion` posts the continuation |
| `Bridge`/`BridgeHandler`/backend verbs, and the bridge-side work an action's result triggers (subscription fan-out, a result-keyed promotion) | The bridge's owner | Called there; a result settled elsewhere posts its bridge-side work there |
| Log sink invocation | Whatever thread called `log*()` | `morph::log::detail::log` |

**One I/O loop, injected.** The application constructs one `exec::IoLoop` and
passes it to every component that does I/O or keeps time, the way it passes its
pool to `LocalBackend`; the framework keeps no process-wide loop of its own.
The dependency is then visible in each constructor, a test builds and tears
down its own loop with nothing global left behind, and one thread carries every
socket, timer and probe of a process — the components' state is touched only in
the loop's tasks, so it needs no lock. Each of those components still has a
loop-less constructor that builds a private `IoLoop`: one loop, and one thread,
for a caller with nothing to share it with (`Bridge` and `RemoteServer` build
their `TimeoutScheduler` that way).

Key consequences:

- **A model author writes single-threaded code.** For a given `ModelId`, the
  strand guarantees `execute()` is never re-entered concurrently, so per-model
  state needs no locking. Different models run in parallel across pool threads.
- **A model cannot reach the framework that runs it.** No framework seam hands a
  model a handle to its own `Bridge` or `BridgeHandler`: construction is nullary
  (`ModelFactory::create<Model>()`), dispatch passes only the action
  (`registry.hpp`'s `model.execute(action)`), and the two optional hooks —
  `onBackendChanged()` and `attachActionLog(...)` — pass no handle either. The
  one other thing an `execute()` body can read mid-dispatch, `session::current()`,
  is plain data (`principal`, `token`, `requestId`, `locale`, `metadata`).

  The enforcement is uneven, and the difference is worth knowing:

  - A **reference** member is impossible *by construction*. Declaring
    `BridgeHandler<Self>& handler;` fails to compile through
    `ModelFactory::create<Self>()` — "reference member … uninitialized",
    `model.hpp`'s `ModelHolder` value-initialises the model with no arguments to
    bind it to.
  - A **pointer** member compiles cleanly. Nothing forbids *declaring*
    `BridgeHandler<Self>* handler{nullptr};`. What keeps it inert is that no
    framework-owned path ever assigns a live value into it, so it stays null
    forever unless a caller hand-wires one in — which is itself the violation,
    not a gap in this guarantee.

  So "a model cannot observe its own bridge" is compile-time enforced for a
  reference and true-in-practice for a pointer. Anything stronger for the
  pointer case would need a check the framework does not have.
- **The GUI thread is never blocked by dispatch.** `executeVia` returns a
  `Completion` immediately; the actual work runs on the pool and the result is
  marshalled back to the GUI executor.
- **Nothing on the I/O loop may block** (see below) — probe callbacks,
  `TimeoutScheduler` callbacks and every socket share its one thread, and a
  blocking callback stalls all of them.

## The strand model — one strand per `ModelId`

`ModelStrands` (`strand.hpp`) sits on top of an arbitrary base `IExecutor` and
turns it into a set of per-key serial queues. It is core-cpp's
`core::async::KeyedStrands<ModelId>`, which specifies and tests the strand
itself; [`core/executor.md`](core/executor.md), "Strands", says what morph adds.

- `post(ModelId key, task)` appends `task` to the strand for `key`. Tasks with
  the same key run in FIFO order with **no overlap**; tasks with different keys
  may run concurrently on different pool threads. This is what removes the need
  for per-model mutexes.
- **At most one live strand per `ModelId`.** A key's strand is made when the key
  gets work and retired when its queue runs out, and the retirement and a post
  for the same key are serialised under the registry's lock, so a post never
  finds a strand that has just been retired beside a new one. A coroutine that
  parked on a strand that was retired meanwhile comes back to the key's current
  strand. morph's own `StrandExecutor` had to fix this invariant twice; it is
  core-cpp's to keep now.
- **A strand runs a batch per turn.** It queues itself on the base once however
  many tasks arrive while it is busy, and runs up to 32 before it hands the base
  back. A loaded host adds latency between turns, not more of them.
- **Closing drops; `teardown()` stops, drains, seals, drains again, then
  closes** where threads exist, so the stopped handlers' ends reach their
  strands while those still admit them; on the single-threaded build it seals,
  stops, then closes. `close()`, and
  the destructor, drop what is queued and wait only for a task running on
  another thread. `drain()` blocks until nothing is queued or running on any
  strand, work posted while it waits included. `seal()` refuses the try-forms
  a Task handler's resumer and its end use, so a resumption or an end that
  arrives afterwards runs inline where it arrives; a plain post is still queued
  until the close. `~LocalBackend` and
  `~SynchronousBackendAdapter` call `teardown()`, which does all of it, so
  nothing reaches a strand between its last drain and its close. It must not be
  called from one of the strands' own tasks; a debug build asserts that. The
  single-threaded WebAssembly build has no other thread: there `drain()`
  returns at once, and `teardown()` seals before it stops the Task handlers, so
  each stopped handler unwinds inline.

`LocalBackend` owns one `ModelStrands` over the worker pool; `RemoteServer`
owns another over its worker pool, beside its server strand. Both post model
work keyed by `ModelId`.
Each shares its strands with the resumers of the Task handlers it started (see
[`core/coroutines.md`](core/coroutines.md)).

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
| base `IExecutor` (e.g. `ThreadPoolExecutor`) | the strands built on it, and the backend that owns them | **Hang** in the backend's drain (see below) |
| `Bridge` | its `BridgeHandler`s: handlers before the bridge, on the owner | A *call* on a handler whose bridge is gone is UB; a handler destroyed after its bridge, on the owner, deregisters nothing (see below) |
| `RemoteServer` (heap, `make_shared`) | every `SimulatedRemoteBackend`/transport holding `RemoteServer&` | Dangling `RemoteServer&` → use-after-free |
| `RemoteServer`'s server strand | closed first, in `~RemoteServer`'s body, before any member its tasks touch | Every task on it holds the server, so none is queued by then; the close waits for one still running on another thread |
| worker pool | a `ReconnectCoordinator` and its offline strand; a `SyncWorker`'s owner | The offline strand's tasks never run, and a coordinator's `onOnline()` completion never settles |
| a `SyncWorker` given a coordinator's `strand()` | the `ReconnectCoordinator`: destroy the coordinator first, and call no `run()` after it | A sequence still running on the strand calls `run()` on a destroyed worker (use-after-free); the coordinator's destructor closes the strand, waiting for that sequence and dropping queued ones, so after it nothing reaches the worker. A `run()` after the coordinator is gone posts to a destroyed strand |
| worker pool | the backend that posts to it (`LocalBackend`, `RemoteServer`) | Same deadlock/UAF family as the strand rule |
| `session::Context` passed to `ScopedContext` | the scope in which the model runs | Dangling thread-local `Context*` |
| `exec::IoLoop` | every component built on it: `SocketBackend`, `SocketServer`, `TimeoutScheduler`, `NetworkMonitor` | **Hang** in the component's destructor, which waits for a close the stopped loop never runs; the same rule as the pool and its backends |

### base `IExecutor` must outlive its strands — and keep running

This is the sharpest edge in the framework. `~LocalBackend` and
`~SynchronousBackendAdapter` **block** in `ModelStrands::drain()` until every
turn their strands queued on the base executor has run. `~ThreadPoolExecutor`
**drains** its queue — after `_stop` is set, workers keep running
already-queued tasks until the queue is empty, then join — so turns already
queued when destruction begins do run.

Draining is not enough to make arbitrary teardown order safe, because a strand
can still be *queuing* turns while the pool tears down. If you destroy the pool
**first**, two things go wrong: a strand may post its next turn to a pool whose
destructor has already run (undefined behaviour — use-after-free on the pool),
and a turn posted after the workers have observed `_stop && _q.empty()` and
exited is never run, so the strand never goes idle and the backend's drain
waits forever → **hang**. The pool must be destroyed *after* every backend that
owns strands over it.

Corollary: **stop feeding a backend before you tear it down.** A post that races
the destructor may land after the drain, and is dropped by the close.

Correct teardown order (innermost-first):

```
handlers  →  Bridge  →  backend (LocalBackend / SimulatedRemoteBackend)
          →  RemoteServer (if remote)  →  worker pool (ThreadPoolExecutor)
```

Declared as members, list the pool **first** so it is destroyed **last**.

### `Bridge`, its handlers and its backend — one owner

A `Bridge`, every `BridgeHandler` built on it and the backend installed in it
belong to the executor the `Bridge` was constructed with — its owner — and
are created, called and destroyed there
([bridge.md](core/bridge.md#thread-safety--one-owner)). That one rule is the
whole of their teardown story:

- **Handlers before the bridge, on the owner.** `~BridgeHandler` deregisters
  its binding; `~Bridge` then rejects every call still waiting for a bind,
  clears the backend's reconnect handler and cancels the backend's pending
  calls. The two destructors run on one thread, so they cannot overlap and
  need no gate. A handler that outlives its bridge on the owner checks the
  bridge's `CallbackToken` and deregisters nothing; on one thread that check
  is exact.
- **Nothing reaches the bridge from another thread.** A reply that settles on
  a backend's thread is delivered to the owner, because the bridge hands the
  owner to `IBackend::bindModel`/`promoteModel` as the executor to deliver on;
  the bridge-side work an action's result triggers is posted to the owner; a
  reconnect is posted to the owner by the backend. Every continuation that
  touches the bridge therefore runs on the thread that runs `~Bridge`, checks
  the bridge's `CallbackToken` first, and cannot be interleaved with the
  destructor it checks for.
- **A `BridgeHandler` constructed off the owner** — inside a running action,
  on a pool thread — posts its registration to the owner and returns.

### `RemoteServer` must be `make_shared` and outlive its transports

`RemoteServer` derives from `enable_shared_from_this` and **must** be created via
`std::make_shared`. Every task it posts — to the server strand from `handle()`
and every public verb, to a model's strand from `execute` admission, back to
the server strand from an execute's reply, to its timers — captures
`shared_from_this()`, so the server object survives until that task completes.

**The self-capture is re-established at every hop.** An `execute` does not
finish inside the server strand's task: admission posts a *second* task onto
the model's strand and returns. If that task did not itself co-own the server,
the last external `shared_ptr` dropping right after admission would free the
server — and with it the `_dispatcher`/`_registry` *reference members* the
model strand's task reads — before the task runs (a use-after-free), or the
reply callback would be destroyed with the server and the client's
`Completion` would hang forever. So the model strand's task **also** captures
`shared_from_this()` (`run.self`) and reaches the dispatcher through
`self->_dispatcher`, never a bare reference capture. The server is thus kept
alive across both hops until the reply fires. This holds even when the owning
`shared_ptr` is dropped while work is in flight — which is why the worker pool
can safely outlive the server *reference*.

**Teardown.** Because every task on the server strand holds the server,
`~RemoteServer` runs only once none is queued there. Its body seals and closes
the strand first — the close waits for a task still running on another thread,
and returns at once when the destructor runs inside the server's own last task
— and only then do the members go. The pool, borrowed, must outlive the server
and keep running until then, as for any strand.

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
model that registers a sub-model), on the worker-pool thread executing the
action. That thread is not the bridge's owner, so the constructor touches
neither the bridge nor the backend there: it posts its registration to the
owner and returns ([bridge.md](core/bridge.md#registration-readiness--the-bind-rule)).
The bind then runs on the owner like any other.

`RemoteServer` still has a synchronous entry, `handleInline`, which
`SimulatedRemoteBackend`'s binds use and which host code the server runs (a
model factory, a `LogProvider`) can reach from a pool thread. `handleInline`
posts the control envelope to the server strand and waits for its reply: the
registry is the server strand's, so the envelope cannot run on the caller's
thread. A waiting pool thread holds a model's strand, never the server strand,
and the server strand's tasks never wait on a model — so another pool thread
runs the envelope and the wait ends. When the caller *is* on the server strand,
`handleInline` runs the envelope inline, since waiting would wait on itself.

What it needs is a pool thread other than the caller's: a pool of one thread
calling `handleInline` from inside its own task, or every pool thread blocked in
`handleInline` at the same moment, never gets its reply. `handleInline`
**rejects `execute`**: an `execute` reply is produced asynchronously on the
model strand, *after* `handleInline` has returned and destroyed the local reply
buffer the deferred callback would write into — a dangling-write hazard. See
[backend.md](core/backend.md) for the exact wiring.

### `cancelPending` — take-then-deliver, weak-ptr tracked

`LocalBackend`, `SynchronousBackendAdapter` and the wire backends track their
in-flight completions as `weak_ptr`s (or a pending table) owned by their owner
— the bridge's owner, or the I/O loop / Qt thread for a transport — without a
lock. `cancelPending(exc)` **takes the whole table out before delivering `exc`
to any entry**, so a continuation that re-enters the backend files into an
empty table rather than into the one being iterated. The `weak_ptr` tracking is
what lets an already resolved-and-destroyed completion be skipped (its state is
gone) rather than resurrected, and `setException` on a still-live-but-already-
`ready` state is the idempotent no-op described below.

### Cancellation — what a cancel verb stops

A call is settled **and** its work asked to stop by every cancel path that
fails it, when the work can be stopped — today, a `Bridge` call whose handler
returns a `core::async::Task` on `LocalBackend`, which carries a
`core::async::StopSource`:

| Verb | Settles the call with | Requests stop on |
|---|---|---|
| `CallbackScope::requestStop()` / `reset()` / destruction | Nothing — its callbacks are refused | Every unsettled call a callback was attached to through the scope ([callback_scope.md](core/callback_scope.md#stopping-the-calls-a-scope-owns)) |
| `Bridge::switchBackend` | `BackendChangedError` (the old backend's `cancelPending`) | Every Task handler still running on the old backend |
| `~Bridge` | `BridgeDestroyedError` | Every Task handler still running on the backend |
| `IBackend::cancelPending` on `LocalBackend` | The given error | Every Task handler still running there |
| The client-side execute deadline | `ClientTimeoutError` | That call |

A stopped handler suspended in a stop-aware await resumes with
`OperationCancelled` on its model's strand. A synchronous handler has nothing
to stop and runs to completion; its result is discarded by the settled state. A
call on a remote backend is settled locally only — the wire carries no
cancellation. `tests/test_coroutine_model.cpp` measures the scope's
`requestStop()`, the switch, `cancelPending` and the deadline: a handler
suspended on a long delay records its cancellation after the verb. `~Bridge`
reaches the handler through the same `LocalBackend::cancelPending`; a scope's
`reset()` and destruction through the same stop request as `requestStop()`.

## Gating callbacks on a receiver's lifetime — `CallbackScope`

Destruction *ordering* (above) is about which objects must outlive which. This
section is about the other half: a callback that has already been scheduled and
whose receiver may not survive to see it delivered.

Every `Completion<T>` callback is posted through an executor — even a
`LocalBackend`'s immediate resolution
([Completion callback marshalling](#completion-callback-marshalling)) — so the
receiver can always be destroyed, or lose interest, between attaching a handler
and the handler running. **The framework primitive for this is
`morph::async::CallbackScope`** ([callback_scope.md](core/callback_scope.md)): a
plain data member the receiver holds, from which it hands out weak
`CallbackToken`s that gate delivery.

```cpp
completion.then(_callbacks, [this](Result r) { render(r); });   // gated
_callbacks.requestStop();                                       // alive, but no longer interested
```

Three rules belong here rather than in the type's own spec, because they are
about how it sits in *this* threading model:

- **Declare the scope last.** Members are destroyed in reverse declaration
  order, so a last-declared scope dies first and every gated callback is refused
  before the fields it would have touched are torn down. This replaces the
  per-class "token must stay last-declared" convention this document used to
  teach as an incantation.
- **A destructor that can pump must `requestStop()` first.** Members are
  destroyed *after* the destructor body, so a body that blocks on a nested event
  loop (`sendSync`-style, see
  [`QtWebSocketBackend::sendSync`](#qtwebsocketbackendsendsync--a-disconnect-must-not-freeze-the-qt-thread))
  can still deliver into a half-destroyed receiver.
  `morph::flows::FlowSession` does exactly this.
- **The guarantee is executor-affine; cross-thread stop is advisory.** When the
  scope is destroyed or stopped on the delivery executor's own thread — the
  normal GUI case — check-then-run is atomic with respect to that. Stopping from
  a *different* thread than the one delivering leaves the same check-then-use
  window the hand-rolled `weak_ptr` idiom always had, and external
  synchronisation there is still the caller's job. Neither `requestStop()` nor
  `~CallbackScope` blocks until in-flight callbacks drain: a GUI-thread
  destructor waiting on a pool-thread callback that is itself posting back to
  the GUI executor is exactly the self-join deadlock family this document warns
  about throughout.

Two framework types use it today: `Bridge` (as `_callbacks`, exposed to handlers
and in-flight continuations through `liveness()`) and `morph::flows::FlowSession`.
`morph::qt::QtExecutor`'s own `_alive` token is adjacent but distinct — it guards
against *the executor* being destroyed with events still queued, not against the
receiver going away.

## Synchronisation specifics per subsystem

### `Bridge::switchBackend` — on the owner; `onBackendChanged` is posted to the model strand

`switchBackend` runs on the bridge's owner and takes no lock. It issues a
re-bind for every live binding on the new backend, with the owner as the
delivery executor. The binds the new backend settles before returning decide
it: if any failed, everything acquired is released and the failure is
rethrown with the old backend and every binding untouched. Otherwise it
publishes the settled ids, leaves a binding whose bind is still in flight
holding its calls, swaps the backend, calls `notifyBackendChanged()`, and
cancels the outgoing backend's pending calls with `BackendChangedError`
([bridge.md](core/bridge.md)).

**`notifyBackendChanged()` only posts.** `LocalBackend::notifyBackendChanged`
dispatches each change-aware model's `onBackendChanged()` onto that model's own
strand rather than invoking it inline, so the callback runs later on a pool
thread:

- **Strand-serialised, no model locking.** `onBackendChanged()` runs on the same
  strand as `execute()` for that model, so the two never overlap — a model
  draining a queue and mutating its own counters there needs no locking. This is
  the guarantee `offline.md`'s conflict-resolution path relies on.
- **It is off the owner.** A handler constructed there is a handler constructed
  inside a running action: its registration is posted to the owner. Owner-only
  verbs (`switchBackend`, `deregisterHandler`, a handler's `execute`) are not
  called from there.
- **`switchBackend` from `onBackendChanged()` is unsupported** on two counts: it
  is owner-only, and the callback runs on the *outgoing* backend's strand, so a
  nested switch that drops the last reference to that backend makes
  `~LocalBackend` drain the strand calling it — a self-join hang, asserted in a
  debug build.

### Reconnect handler — posted to the owner, guarded against a dead `Bridge`

A backend never runs the reconnect handler on its own thread: after a
reconnect it posts the handler to the executor `setReconnectHandler` was given,
which for `Bridge` is its owner. The handler's re-binds are therefore ordinary
owner tasks, and their replies are delivered on the owner too. Two guards
cover a backend that outlives its `Bridge`:

- **`~Bridge` clears the active backend's handler** before cancelling pending
  work, and `switchBackend` clears the *outgoing* backend's handler after the
  swap.
- **The handler checks the bridge's `CallbackToken`** before touching `this`.
  A reconnect already posted to the owner when `~Bridge` runs finds it expired
  and does nothing; on the owner that check is exact, because the destructor
  runs there too. It then ignores a reconnect of a backend the bridge has since
  switched away from.

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

### `NetworkMonitor` — callbacks run on the I/O loop and must not block it

The probe, `onOffline` and `onOnline` run **on the I/O loop's thread**, from a
loop timer the monitor re-arms every `probeInterval`. Constraints:

- **The probe and the callbacks must not block.** They share the loop with
  every socket and timer built on it; a blocking one stalls all of them, and
  delays the next probe. The intended body is short — set an atomic flag or
  `post()` to an executor and return.
- **Callbacks must not throw** through the monitor's expectations (the probe
  itself is wrapped in `safeProbe`, which swallows exceptions).
- **`stop()` runs on the loop.** From another thread it posts and waits, so
  once it returns no probe or callback is running or will run. From the probe
  or a callback it runs inline and prevents the re-arm. `stop()` is idempotent.
- **Destroying the monitor from its own probe or callback is supported.** The
  timer that invoked it holds the loop-side state until it returns, so the
  monitor's storage may go while the callback is still on the stack.
- `isOnline()` reads an `std::atomic<bool>` — safe from any thread at any time.

### `ReconnectCoordinator` — one strand runs the whole retry loop

`onOnline()` posts the **entire** reconnect → activate → bind → replay loop,
retry sleeps included, as one task on the coordinator's offline strand;
`onOffline()` posts its activate → bind as another. A strand runs one task at a
time in post order, so the two never overlap, and an `onOffline()` posted while
a reconnect is sleeping between attempts runs after it. The coordinator owns no
thread and does no I/O: the strand runs on the executor it was given, which
should be a worker pool, **not** the I/O loop that runs `NetworkMonitor`'s
callbacks — the retry sleeps block the thread the task is on. The strict step
order (reconnect → activatePrimary → bindContext → replay) is an invariant:
replay never runs before context is bound. A `SyncWorker` given the
coordinator's `strand()` as its owner drains inline inside the `replay` step,
so nothing posted in the meantime can run between bind and replay.

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

## `MORPH_LIFETIMEBOUND` — the "must outlive" rules, told to the compiler

Every rule in this document that reads *X must outlive Y* is, in the code, a
reference or pointer parameter that a constructor stores, or an accessor that
hands out a reference into `*this`. `morph/attributes.hpp` gives that shape a
name:

```cpp
#define MORPH_LIFETIMEBOUND [[clang::lifetimebound]]   // [[msvc::lifetimebound]] on MSVC; empty on GCC
```

Applied to a parameter it says the return value may refer to that parameter's
referent; applied after a member function's parameter list it says the same of
`*this`; applied to a constructor parameter it says the constructed object keeps
referring to the argument. The attribute generates no code and changes no
behaviour — it lets Clang diagnose a call site that breaks a rule this document
already states in prose, and it is what keeps `-Weverything` from asking for the
annotation on every declaration that visibly needs one (Clang 23's
`-Wlifetime-safety-intra-tu-*-suggestions`).

**Where it is not the whole truth.** Three places are worth knowing, because the
attribute is coarser than morph's actual contracts:

- **`BridgeHandler`'s `Bridge&` is annotated, but the contract is narrower.**
  [Above](#bridge-its-handlers-and-its-backend--one-owner), destroying
  the bridge *before* a live handler, on the owner, is defined behaviour — the
  handler's liveness token turns its destructor into a no-op. `lifetimebound` cannot say "except for
  destruction", so it reads as the stricter "must outlive, period". The annotation
  is right about every *use*; the destruction carve-out is still real, and
  `tests/test_switch_backend.cpp` still asserts it.
- **Callbacks taken by value carry the annotation too** — `NetworkMonitor`'s
  `probe`, `SyncWorker`'s `replay`, `SigningAuthorizer`'s `clock`. The
  `std::function` itself is *owned*, not borrowed; what must outlive the object is
  whatever the stored callable refers to. Clang asks for the annotation on these
  because of how libc++ represents `std::function`, and the contract the
  annotation ends up documenting — "anything the callable captures by reference
  must outlive the object it is handed to" — is one these subsystems genuinely
  have, since the callable runs on a probe or replay thread for the object's whole
  life.
- **`ModelHolder`'s forwarding constructor is deliberately *not* annotated.**
  Whether the arguments are borrowed depends entirely on the `Model` being built,
  and one attribute covers every instantiation: annotating it turns
  `ModelHolder<M>(prvalue…)` — the ordinary case, where the model owns everything
  it was handed — into a false dangling report, and Clang rejects the annotation
  outright (`-Wlifetime-safety-lifetimebound-violation`). A model that *does* keep
  a reference states so on its own constructor, and is built before it is handed
  to the holder.

## Quick cheat-sheet

Destroy in this order (or declare members so the reverse holds):

```
BridgeHandler(s)          ← first to go, on the owner, like everything else on this list above the pool
  Bridge
    backend               ← LocalBackend / SimulatedRemoteBackend
      RemoteServer         ← only in remote mode; keep its shared_ptr alive this long
                             (its server strand is closed first, by its own destructor)
        ThreadPoolExecutor ← LAST: it must outlive every strand it backs
```

One-liners to remember:

- Never destroy the pool before the backend → the backend's drain hangs.
- Never `post()` to a backend whose destructor has begun.
- `onBackendChanged()` runs posted on the model's strand, off the owner: a
  handler constructed there posts its registration; never call `switchBackend`
  there (it is owner-only, and self-joins the strand it runs on).
- Call a `Bridge`, its handlers and its backend only on the bridge's owner;
  destroy handlers before the bridge, there.
- Never block from anything the I/O loop runs: a `NetworkMonitor` probe or
  callback, a `TimeoutScheduler` callback.
- Never destroy an `IoLoop` before the components built on it.
- Never log from inside a log sink (non-recursive mutex).
- Never read `session::current()` off the dispatch thread or after `execute()`
  returns.
- Register all models/actions at static-init; treat the registries as read-only
  afterward.
- Never attach a callback capturing a bare `this` to a `Completion` you cannot
  prove outlives nothing — pass a `CallbackScope`, or say `thenDetached` on
  purpose.
- Declare a `CallbackScope` member **last**, and call `requestStop()` first in
  any destructor body that can pump an event loop.

## Cross-references

- [`executor.md`](core/executor.md) — `IExecutor`, `ThreadPoolExecutor`,
  `ModelStrands`, `ModelId`; the "destroy the backend before the base pool" rule in
  detail.
- [`completion.md`](core/completion.md) — `Completion<T>` / `CompletionState<T>`
  internals and orphan-error logging.
- [`callback_scope.md`](core/callback_scope.md) — `CallbackScope` /
  `CallbackToken`: gating a callback on its receiver's liveness *and* on an
  explicit stop, and the exact boundary of that guarantee.
- [`bridge.md`](core/bridge.md) — `Bridge`, `BridgeHandler`, `switchBackend`,
  `executeVia`, the owner every one of them belongs to, and the liveness token
  a handler outliving its bridge consults.
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
