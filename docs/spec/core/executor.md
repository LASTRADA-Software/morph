# Executor framework — design

`morph::exec` provides a small set of executor abstractions that control
*where* and *when* posted tasks run. The design is intentionally minimal:
every executor accepts `std::function<void()>` tasks and guarantees they
execute at some point after `post()` returns, but the concurrency model,
threading, and serialisation semantics differ per implementation.

## Contents

- [Type overview](#type-overview)
- [`IExecutor` — the abstract interface](#iexecutor--the-abstract-interface)
- [`ThreadPoolExecutor`](#threadpoolexecutor)
- [`MainThreadExecutor`](#mainthreadexecutor)
- [`QtExecutor`](#qtexecutor)
- [`StrandExecutor` and `ModelId`](#strandexecutor-and-modelid)
- [Lifetime & ownership](#lifetime--ownership)
- [Thread safety](#thread-safety)
- [Failure modes](#failure-modes)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Type overview

There are seven types, split across `morph::exec` (in `executor.hpp`),
`morph::exec::detail` (in `strand.hpp`), and `morph::qt` (in
`qt/qt_executor.hpp`):

| Type | Namespace | Purpose |
|---|---|---|
| `IExecutor` | `morph::exec` | Abstract base: a single pure-virtual `post(task)`. |
| `ThreadPoolExecutor` | `morph::exec` | Fixed-size thread pool, FIFO queue, task exceptions logged (never propagate). |
| `MainThreadExecutor` | `morph::exec` | Collects tasks from any thread, drains on `runFor()` from the owning thread. |
| `QtExecutor` | `morph::qt` | Posts tasks to a Qt event loop; they run on the configured context object's thread (the `QCoreApplication`/GUI thread by default). |
| `ModelId` | `morph::exec::detail` | Opaque 64-bit identifier for a model instance, used as a strand key. |
| `ModelIdHash` | `morph::exec::detail` | Hash functor so `ModelId` can be an `unordered_map` key. |
| `StrandExecutor` | `morph::exec::detail` | Per-key serialising wrapper — tasks with the same `ModelId` never overlap. |

`IExecutor` and the two thread-based concrete executors live in the public
`morph::exec` namespace. `QtExecutor` lives in `morph::qt` (in the separate
`qt/qt_executor.hpp` header) because it depends on Qt; only the GUI/bridge layer
pulls it in. `StrandExecutor`, `ModelId`, and `ModelIdHash` live in
`morph::exec::detail` because they are implementation details of the morph model
framework, not general-purpose utilities.

## `IExecutor` — the abstract interface

A pure-virtual `post(std::function<void()>)` that schedules a callable for
asynchronous execution. Thread-safe. How a task's exception is handled is left
to the implementation; the header's default wording says an exception "is
silently swallowed unless the implementation documents otherwise", and every
concrete executor here *does* document otherwise — the two thread-based ones
catch and log, `QtExecutor` defers to Qt (see [Failure modes](#failure-modes)).
No implementation lets a task exception escape `post()` (the task has not run
yet when `post()` returns).

## `ThreadPoolExecutor`

A fixed-size thread pool. The constructor spawns `n` worker threads; each worker
loops, waiting for tasks on a shared condition variable. Tasks are dispatched in
FIFO order from a single mutex-protected queue.

`n` is **clamped to a minimum of 1**. A pool with zero workers would accept
posted tasks that no thread could ever run, so every `post()` would hang forever
and any `StrandExecutor` built on it would deadlock in its destructor waiting on
`_inFlight`. Passing `0` therefore yields a usable single-worker pool rather than
a silently dead one; values `≥ 1` spawn exactly that many workers.

The destructor signals stop, notifies all workers, and joins every thread. The
workers **drain** the queue before exiting: once `_stop` is set the loop exits
only when `_stop && _q.empty()`, so workers keep popping and running
already-queued tasks (including strand lambdas re-posted from within a running
task) until the queue is empty. The join therefore blocks until every task
queued before destruction has run. The one thing not covered is a task
`post()`ed concurrently with or after destruction: it races the last worker's
exit and may be silently lost. Exceptions from tasks are caught in the worker
`loop` and logged via
`morph::log::logError` (see [Failure modes](#failure-modes)); they never
propagate out of a worker, so one failing task neither kills its thread nor
aborts sibling tasks.

## `MainThreadExecutor`

A single-thread executor that does **not** spawn its own thread. Tasks posted
from any thread are enqueued and only executed when the owning thread calls
`runFor(timeout)`. Useful in tests or event loops that lack a native dispatcher.

`runFor()` runs queued tasks one by one for up to the given wall-clock
duration. It uses a condition-variable `wait_until` on the deadline, so it does
**not** return early when the queue drains — while time remains it keeps waiting
for newly posted tasks and only returns once the deadline is reached. If a task
throws a `std::exception`, the exception is logged (via `morph::log::logError`,
prefixed `"[main-thread] callback threw: "`) and execution continues with the
next task. Note that only `std::exception`-derived exceptions are caught; any
other thrown type propagates out of `runFor()`.

Two additional step-oriented primitives sit alongside `runFor()` for callers
that want deterministic, non-blocking control instead of a timed pump:

- **`runOnce()`** dequeues and runs at most one pending task and returns
  immediately — `true` if a task was found and run (regardless of whether it
  threw), `false` if the queue was empty. Unlike `runFor()`, it never waits for
  a task to appear.
- **`drain()`** repeatedly calls the same dequeue-and-invoke step as `runOnce()`
  until the queue is observed empty, then returns — with no wall-clock timeout
  and no wait for externally posted tasks. A task that posts new work while
  running extends the drain (the new task is still in the queue and gets
  consumed before `drain()` returns), but `drain()` never blocks waiting on
  work from another thread the way `runFor()`'s `wait_until` does.

All three share one dequeue-and-invoke step (pop under `_m`, then run outside
the lock with the same `try`/catch `std::exception` logging), so the exception
handling and locking discipline described above apply identically to
`runOnce()` and `drain()`.

## `QtExecutor`

An `IExecutor` implementation that marshals tasks onto a Qt event loop. Lives
in `morph::qt` (header `morph/qt/qt_executor.hpp`) and is compiled only when Qt
is available; it is the GUI leg of the executor family and the counterpart the
[bridge](bridge.md) hands to backends as their main-thread executor.

The constructor takes an optional `QObject* context`, defaulting to
`QCoreApplication::instance()`. `post()` forwards the callable to
`QMetaObject::invokeMethod(context, fn, Qt::QueuedConnection)`. Because the
connection is queued, the task always runs on the thread that owns `context`
(the application/GUI thread by default), regardless of which thread called
`post()`. `post()` is therefore thread-safe and returns immediately; the task
runs later, once that thread's event loop processes the queued event. The
caller does **not** need to be (or supply) a `QObject` — only the constructor
optionally takes one, to pick the target thread.

### Teardown: queued tasks are dropped, not delivered

`post()` enqueues and returns. The queued Qt event outlives that call, so
joining the worker pool whose task made the call proves only that the
`post()` **happened** — never that the event was **delivered**. A task still
sitting on the Qt queue when its `QtExecutor` is destroyed would otherwise be
delivered against a freed executor and read `_context` off freed memory.

Each queued task therefore carries a weak observer of its executor's lifetime
and does nothing if the executor is already gone.

This is not a theoretical hazard. [`Bridge::executeVia`](bridge.md) chains
three `Completion` objects per dispatched action, each settled from *inside*
the previous one's delivered callback, so a caller waiting only on its own
top-level completion can observe "done" while an intermediate post is still
queued; when that stale event is finally pumped, its body calls `post()` for
the next link. Before the guard this segfaulted ordinary uninstrumented
builds, not merely sanitizer runs (morph#127).

Dropping is the correct outcome rather than the lesser evil: a chain being
torn down has nobody left to observe its result, and
[`Completion`](completion.md)'s orphan logging already covers a completion
that never resolves. Owners consequently do **not** need to drain the event
loop before destroying an executor.

**Boundary.** The check assumes the executor is destroyed on the same thread
that runs its context's event loop, which holds for every owner in this
repository. Destroying one from another thread while its loop is mid-delivery
still needs external synchronisation: this closes the "torn down with events
still queued" hole, not a genuine cross-thread race.

Passing a non-default `context` — e.g. a plain `QObject` that has been
`moveToThread()`'d onto a worker `QThread` with its own event loop — lets a
`QtExecutor` dispatch to that worker thread instead of the GUI thread. This is
the standard way to get a `QtExecutor`-compatible `IExecutor` for a non-main
thread: give the worker thread a live `QObject` and construct the executor with
a pointer to it. `QMetaObject::invokeMethod` reads `context->thread()` at
dispatch time, so if `context` is reparented to a different thread after
construction, subsequently posted tasks follow it to the new thread.

Unlike `MainThreadExecutor`, `QtExecutor` needs no explicit `runFor()` drain —
the running Qt event loop *is* the dispatcher. It is otherwise stateless: it
owns no queue, spawns no thread, and holds only the `context` pointer, so it has
no lifetime or shutdown concerns of its own beyond that pointer's validity.
Because the context object is not owned by `QtExecutor`, Qt drops the queued
invocation if **that object** (or the thread it lives on) is destroyed before
the event is processed — for the default context this means application
shutdown; for a custom worker-thread context, callers must keep the context
`QObject` alive at least as long as tasks may still be posted. Qt has no
visibility into `QObject`s *captured inside* the opaque `std::function`: a task
whose captured widget was deleted still runs and will dereference the dangling
pointer. There is **no** per-task implicit cancellation — callers that capture a
`QObject` must guard it themselves (e.g. `QPointer` or a liveness token) (see
[Limitations](#limitations)).

## `StrandExecutor` and `ModelId`

A per-key serialising executor built on top of any `IExecutor`. Tasks posted
with the same `ModelId` key execute in FIFO order with no overlap, even when the
underlying executor is a thread pool. Tasks with different keys may run
concurrently.

`ModelId` is an opaque 64-bit identifier. Zero is reserved and means "not
bound". Non-zero values are assigned by the backend and are stable for the
lifetime of the model. It supports three-way comparison and can be used as an
`unordered_map` key via `ModelIdHash`.

Internally `StrandExecutor` maintains a map of `ModelId → shared_ptr<Strand>`
(shared state per key). A `Strand` holds a pointer to the base `IExecutor`, a
mutex, a pending queue, and a `running` flag. The executor also tracks an
`_inFlight` counter (guarded by the map mutex) that the destructor waits on.

**`_inFlight` is incremented with the *decision* to dispatch, not lazily.**
`post()` increments `_inFlight` in the same `_mapMtx` critical section that flips
`running` true and decides to schedule, before releasing the lock; the re-arm
step in the strand task likewise increments under the `_mapMtx` it already holds,
before the current run's own decrement. This closes an internal window that would
otherwise exist if the increment were deferred to a later `_mapMtx` acquisition
in `scheduleNext`: between releasing `_mapMtx` in `post()` and re-taking it to
count the dispatch, `~StrandExecutor` could acquire `_mapMtx`, observe
`_inFlight == 0`, and destroy the map before the dispatched lambda touched it.
Because "decided to schedule" and "counted as in-flight" are now atomic under one
lock, and the re-arm's increment precedes the prior run's decrement, `_inFlight`
never dips to a spurious 0 across a scheduling hand-off. (This is distinct from
the caller-discipline rule below, which concerns a `post()` that genuinely
arrives after teardown has begun.)

**The per-key serialisation invariant:** at most one live `Strand` exists per
`ModelId`, and any strand that is (or becomes) `running` is the strand currently
stored in the map for that key. This is what guarantees a key's tasks never
overlap — a single strand runs them one at a time.

Two operations can violate that invariant if they interleave: `post()` pushing a
task and flipping `running` true, and `scheduleNext`'s drain step clearing
`running` and *erasing* the map entry when the queue empties. Holding the
combined `{_mapMtx, strand->mtx}` lock only inside the drain step is **not**
enough: the earlier design took `_mapMtx` in `post()` only long enough to look
the strand up, released it, and then re-armed the strand under `strand->mtx`
alone. A concurrent drain could erase the strand in that gap, orphaning a live
strand — and the next `post(key)` would then create a *second* strand for the
same key, so two strands ran the key's tasks concurrently.

The fix makes **both** sides hold `_mapMtx` across their whole decision. `post()`
takes `_mapMtx`, does the slot lookup/create, and then — *still holding
`_mapMtx`* — takes `strand->mtx` to push the task and set `running`. The drain
step takes the same two locks in the same order. Because the map lookup, the
re-arm, and the erase are all serialised by `_mapMtx`, a strand that becomes
`running` in `post()` is guaranteed to still be the map entry, and the drain
never erases a strand whose `pending` queue is non-empty. The orphaning window is
gone. Lock order is always `_mapMtx` → `strand->mtx`; both sites acquire them as
two sequential `scoped_lock`s in that order (rather than one `scoped_lock` over
the pair, whose `std::lock` back-off can grab them in address order), so a single
consistent order holds everywhere and there is no lock-ordering deadlock.

Each strand task is the point where the model's own code actually runs, so the
task wrapper catches exceptions and logs them via `morph::log::logError` (see
[Failure modes](#failure-modes)) before deciding whether to keep the strand
running. A throw therefore neither stalls the strand nor skips the drain-and-erase
bookkeeping: the next queued task for that key still runs.

The destructor waits for all in-flight tasks to complete (`_inFlight == 0`)
before destroying the strand map.

**Testing per-model ordering without naming `StrandExecutor`/`ModelId` (issue #55).**
`RemoteServer` (see `backend.md`) owns a `StrandExecutor` internally, but every
task it ever dispatches — the top-level `handle()` post and the internal
per-model strand dispatch alike — funnels through the single `IExecutor` the
server was constructed with. A caller that wants a deterministic, hand-stepped
interleaving harness against `RemoteServer`'s real per-model ordering does not
need to touch `morph::exec::detail::StrandExecutor` or
`morph::exec::detail::ModelId` at all: constructing the server against a
single-step, test-controlled `IExecutor` (see `tests/test_support.hpp`'s
`morph::testing::StepExecutor`) and driving it one task at a time is enough —
`RemoteServer`'s own wire replies carry the model id as a plain `uint64_t`
(`wire::Envelope::modelId`), so a test never needs the `ModelId` vocabulary
either.

## Lifetime & ownership

`StrandExecutor` stores a raw pointer to the base `IExecutor` (`_base`, copied
into each `Strand::base`). It does **not** own the base and never extends its
lifetime. Two invariants make the arrangement safe, and violating either is a
latent bug:

1. **The base `IExecutor` must outlive the `StrandExecutor`.** Every strand
   dispatch calls `strand->base->post(...)`, and `~StrandExecutor` blocks until
   the last of those dispatched lambdas has run. So the base must still be alive
   for the whole life of the strand, *including* the destructor's wait.

2. **The base must actually run — not lose — every task the strand posts.**
   `~StrandExecutor` only returns once `_inFlight` reaches 0, and `_inFlight` is
   decremented *inside* the dispatched lambda, after the task runs. `_inFlight`
   is incremented on the strand thread *before* the lambda is handed to
   `base->post()`. If a posted lambda never runs — because it was handed to a
   pool that is already being destroyed or has already joined its workers — that
   decrement never happens and the strand destructor waits forever.

These two combine into the framework's most important ordering rule for these
types. `~ThreadPoolExecutor` **drains** its queue (workers run every
already-queued task before joining), whereas `~StrandExecutor` **blocks** until
`_inFlight == 0`. Draining is not enough to make arbitrary teardown order safe,
because the strand can still be *dispatching* while the pool tears down.
Therefore:

> **Always destroy the `StrandExecutor` before the base pool it wraps.**

If the base `ThreadPoolExecutor` is destroyed first, two things go wrong. A
strand lambda still in flight may call `base->post()` on a pool whose destructor
has run — undefined behaviour (use-after-free on the pool's queue/mutex). Even
absent UB, a lambda posted after the pool's workers have already observed
`_stop && _q.empty()` and exited is never run, so its `--_inFlight` never
happens and the subsequent `~StrandExecutor` deadlocks on its condition variable
forever. With member declaration order this means the pool must be declared
*before* the strand (members destroy in reverse order), or the two must be torn
down explicitly in that order.

A second rule follows from the same wait: **no `post()` may race with or follow
`~StrandExecutor`.** The destructor takes `_mapMtx` and waits for
`_inFlight == 0`, but it does not block new `post()` calls. A `post()` that
arrives concurrently with (or after) destruction can enqueue work and re-arm a
strand after the destructor believed it had quiesced, reintroducing exactly the
data race the `_inFlight` wait exists to prevent. Callers must ensure all task
sources are shut down before the `StrandExecutor` is destroyed.

The strand map is self-cleaning: when a strand drains (its `pending` queue is
empty), `scheduleNext` clears `running` and erases the map entry under the
combined `{_mapMtx, strand->mtx}` lock. Live memory therefore tracks the set of
*currently active* models rather than every model ever seen — there is no
per-model registration to leak. The cost is allocation churn: a model that is
posted to in bursts allocates a fresh `Strand` each time its queue empties and
refills, rather than keeping one long-lived strand per key.

## Thread safety

All four executors' `post()` methods are safe to call from any thread
concurrently.

- `ThreadPoolExecutor` guards its queue and `_stop` flag with a single mutex
  `_m` and coordinates workers on `_cv`. Multiple workers pop under the lock, so
  the FIFO order is a total order across producers; task *execution* is
  concurrent across the `n` workers.
- `MainThreadExecutor` guards its queue with `_m`. `post()` may be called from
  any thread, but `runFor()` must be called only from the single owning
  ("main") thread; concurrent `runFor()` calls are not supported.
- `StrandExecutor` uses two lock levels: `_mapMtx` protects the `_strands` map
  and the `_inFlight` counter, and each `Strand::mtx` protects that strand's
  `pending` queue and `running` flag. Both operations that can break the
  per-key invariant hold `_mapMtx` across their whole decision: `post()` takes
  `_mapMtx`, does the slot lookup/create, and then — still holding `_mapMtx` —
  takes `strand->mtx` to push and re-arm; the drain-and-erase step in
  `scheduleNext` takes the same two locks in the same order. This serialises the
  lookup, the re-arm, and the erase, so a concurrent `post()` can no longer
  re-arm a strand *after* a drain has erased it (which would orphan a live
  strand and let two strands for one key run concurrently). Lock order is always
  `_mapMtx` → `strand->mtx`, acquired as two sequential `scoped_lock`s (not one
  `scoped_lock` over the pair) so a single consistent order holds at every site
  and there is no lock-ordering deadlock. The net guarantee: tasks with the same
  `ModelId` never overlap; tasks with different keys may run in parallel on the
  base pool.
- `QtExecutor` holds only a `QObject*` context pointer; its thread safety is
  entirely Qt's. `QMetaObject::invokeMethod` with `Qt::QueuedConnection` is
  documented as safe to call from any thread, and the queued event is
  dispatched serially by the event loop of whichever thread owns the context
  object (`QCoreApplication`'s thread by default).

## Failure modes

| Executor | What happens when a task throws |
|---|---|
| `ThreadPoolExecutor` | The worker `loop` catches it. `std::exception` is logged as `"[thread-pool] task threw: " + what()`; any other type is logged as `"[thread-pool] task threw unknown exception"`. The worker keeps looping. |
| `StrandExecutor` | The strand task wrapper catches it. `std::exception` is logged as `"[strand] task threw: " + what()`; any other type is logged as `"[strand] task threw unknown exception"`. The strand's drain/erase bookkeeping and `_inFlight` decrement still run, so the next task for the key proceeds. |
| `MainThreadExecutor` | `runFor` catches **only** `std::exception`, logged as `"[main-thread] callback threw: " + what()`, then continues with the next task. **Any non-`std::exception` type propagates out of `runFor()`** and is the caller's problem. |
| `QtExecutor` | No `try`/`catch` of its own. A throwing task propagates into whoever drives the target thread's event loop (`QCoreApplication::exec` by default, or the worker thread's loop for a custom context); Qt's default behaviour is to `std::terminate`. Tasks posted through it must not let exceptions escape. |

All logging goes through `morph::log::logError`. The design principle: a task
failure must never kill a worker/strand or abort sibling tasks, but it must also
never be *invisible*. Previously these exceptions were swallowed silently; they
are now logged. `ThreadPoolExecutor` and `StrandExecutor` catch `(...)` and so
contain every exception type; `MainThreadExecutor` deliberately narrows its
`catch` to `std::exception` (a non-standard throw surfaces on the drain thread
rather than being hidden).

## API reference

### `IExecutor`

| Member | Signature | Notes |
|---|---|---|
| `post` | `virtual void post(std::function<void()> task) = 0` | Thread-safe. Task runs after the call returns. Per-implementation exception handling (both concrete executors log; see Failure modes). |
| dtor | `virtual ~IExecutor() = default` | |

### `ThreadPoolExecutor : IExecutor`

| Member | Signature | Notes |
|---|---|---|
| ctor | `explicit ThreadPoolExecutor(std::size_t n)` | Spawns `max(n, 1)` worker threads. `n == 0` is clamped to 1 (a zero-worker pool would hang every task). |
| dtor | `~ThreadPoolExecutor() override` | Signals stop, then joins all workers, which drain the queue (run every already-queued task) before exiting. Tasks posted concurrently with or after destruction may be lost. |
| `post` | `void post(std::function<void()> task) override` | Enqueues to FIFO; notifies one worker. Thread-safe. Task exceptions caught and logged in the worker loop. |

### `MainThreadExecutor : IExecutor`

| Member | Signature | Notes |
|---|---|---|
| `post` | `void post(std::function<void()> task) override` | Enqueues; notifies waiters. Thread-safe. Not executed until `runFor()`/`runOnce()`/`drain()`. |
| `runFor` | `void runFor(std::chrono::milliseconds timeout)` | Runs tasks until the `timeout` deadline (blocks for new tasks while time remains; does not return early on an empty queue). Must be called from the owning thread. `std::exception`s logged and skipped; other exception types propagate. |
| `runOnce` | `bool runOnce()` | Dequeues and runs at most one pending task; returns immediately, never blocks. Returns `true` if a task ran, `false` if the queue was empty. Must be called from the owning thread. |
| `drain` | `void drain()` | Runs tasks until the queue is empty; no wall-clock timeout, does not wait for externally posted tasks. Must be called from the owning thread. |

### `QtExecutor : IExecutor` (`morph::qt`)

| Member | Signature | Notes |
|---|---|---|
| ctor | `explicit QtExecutor(QObject* context = QCoreApplication::instance())` | Stores `context` as the `invokeMethod` target. Defaults to the application instance (GUI thread). `nullptr` makes `post()` a no-op (matches `QMetaObject::invokeMethod`'s handling of a null target). |
| `post` | `void post(std::function<void()> fn) override` | Posts `fn` to `context`'s event loop via `QMetaObject::invokeMethod(context, ..., Qt::QueuedConnection)`; runs on whichever thread owns `context` at dispatch time. Thread-safe; returns immediately. |

### `ModelId` (`morph::exec::detail`)

| Member | Signature | Notes |
|---|---|---|
| `v` | `uint64_t v{0}` | Raw id. 0 = unbound. |
| `operator<=>` | `auto operator<=>(const ModelId&) const = default` | Three-way comparison. |

### `ModelIdHash`

| Member | Signature | Notes |
|---|---|---|
| `operator()` | `std::size_t operator()(ModelId mid) const noexcept` | Hashes `mid.v`. |

### `StrandExecutor` (`morph::exec::detail`)

| Member | Signature | Notes |
|---|---|---|
| ctor | `explicit StrandExecutor(IExecutor& base)` | Wraps `base`. |
| dtor | `~StrandExecutor()` | Blocks until `_inFlight == 0`. Requires the base to outlive it and to run every posted task — otherwise deadlocks (see Lifetime & ownership). |
| `post` | `void post(ModelId key, std::function<void()> task)` | Enqueues for strand `key`. FIFO per key, concurrent across keys. Thread-safe. Task exceptions caught and logged. Must not race/follow the destructor. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Task signature | `std::function<void()>` | Simple, universal. Every executor accepts the same callable type. No return value, no cancellation. |
| Exception handling | **Caught and logged, never propagated out of a worker/strand** | A task failure must not crash unrelated tasks *or* vanish. `ThreadPoolExecutor` and `StrandExecutor` catch `(...)` and log via `morph::log::logError`; `MainThreadExecutor` narrows its catch to `std::exception` so a non-standard throw surfaces on the synchronous drain thread. See [Failure modes](#failure-modes). |
| ThreadPoolExecutor drain-on-dtor | **Drain the queue, then join** | Workers run every already-queued task before exiting, so a `StrandExecutor`'s in-flight lambdas complete and decrement `_inFlight` as long as the pool outlives the strand. There is no public `waitIdle`/graceful-shutdown API; tasks posted after destruction begins may be lost, so the caller must still synchronise teardown order externally. |
| MainThreadExecutor's `runFor` | **Wall-clock deadline** | Lets the caller batch-process tasks without spinning. The condition-variable wait avoids busy-waiting. |
| MainThreadExecutor's `runOnce`/`drain` | **Thin wrappers sharing `runFor`'s dequeue-and-invoke step, added alongside it** | `runOnce()` steps exactly one task without blocking; `drain()` loops `runOnce()` until the queue is empty. Neither waits on new tasks from other threads, unlike `runFor()`'s deadline-scoped wait — this gives event-loop integrations and tests deterministic, non-blocking single-step control without replacing `runFor()`'s existing behavior. |
| ModelId zero | **Reserved — "not bound"** | A natural sentinel for optional/uninitialised model handles. |
| StrandExecutor in `detail` | **Not a general-purpose utility** | Exists only for the morph model framework's per-model serialisation. The `ModelId` key is specific to model instances. |
| StrandExecutor destructor | **Waits for in-flight tasks** | Without this, a pool thread running `scheduleNext` can access `_strands` after it has been destroyed (TSan: data race on destructor vs erase). |
| Strand per-key invariant | **`post()` *and* drain-and-erase both hold `_mapMtx` across their whole decision** | Serialises lookup, re-arm, and erase so at most one live strand exists per key and any `running` strand is the map's current entry. Holding the combined lock only in the drain step was insufficient — `post()` re-armed under `strand->mtx` alone after releasing `_mapMtx`, so a drain could erase the strand in that gap, orphan it, and let a second strand for the same key run concurrently. |
| No `std::future` / return value | **Fire-and-forget only** | Executors schedule side-effect tasks. Callers that need results use shared state or futures externally. |
| No `std::executor` conformance | **Custom interface, not `std::executor`** | C++26 `std::executor` is not yet widely available. This is a minimal in-house abstraction. |
| `QtExecutor` via `invokeMethod`, not a `QObject` subclass | **Near-stateless free-standing `IExecutor`, target configurable via ctor** | Uses `QMetaObject::invokeMethod(context, fn, Qt::QueuedConnection)`, so callers need no custom `QObject`, event type, or slot — they only optionally supply a `QObject*` to pick the target thread. Defaults to `QCoreApplication::instance()` so existing GUI-thread call sites are unaffected. Keeps the type a drop-in `IExecutor` holding a single pointer, with Qt's event loop as the sole dispatcher. |
| `QtExecutor` in a separate `morph::qt` header | **Isolate the Qt dependency** | The core executor family (`executor.hpp`, `strand.hpp`) stays Qt-free; only the GUI/bridge layer includes `qt/qt_executor.hpp`. Backends depend on `IExecutor`, never on Qt. |

## Limitations

These are honest, known gaps — accepted trade-offs, not bugs:

- **Unbounded queues / no backpressure.** `ThreadPoolExecutor`, `MainThreadExecutor`,
  and each `Strand::pending` are all unbounded `std::queue`s. A producer that
  outruns consumption grows memory without limit; `post()` never blocks or
  rejects. There is no bounded-queue option, no high-water mark, and no way for a
  caller to learn the queue is backing up.
- **No cancellation.** Once posted, a task cannot be cancelled or removed. The
  signature is fire-and-forget `std::function<void()>` with no token, handle, or
  future. There is no implicit cancellation either: by default `QtExecutor`
  posts against `QCoreApplication::instance()`, so Qt drops a queued invocation
  only at application shutdown (or, for a custom `context`, when that object or
  its thread is destroyed) — never because a `QObject` captured *inside* the
  callable was deleted. Such a task still runs against the dangling capture.
  Callers must guard their own captures.
- **No graceful drain / `waitIdle` on `ThreadPoolExecutor`.** The destructor
  drains already-queued tasks but there is no method to wait until the queue is
  empty, to flush pending work before shutdown, or to reject work posted during
  shutdown (such a task may be lost). Callers who need to coordinate around
  in-flight work must synchronise externally. (`StrandExecutor` waits for
  `_inFlight`, but that is a lifetime-safety wait, not a general drain API, and it
  relies on the base pool still running the strand's dispatched lambdas.)
- **Strand allocation churn.** The self-cleaning map (see
  [Lifetime & ownership](#lifetime--ownership)) is good for memory — live entries
  track active models — but a bursty model re-allocates a `Strand` every time its
  queue empties and refills, instead of reusing one long-lived strand per key.

## Cross-references

- [`completion.md`](completion.md) — `Completion<T>` marshals its `.then` /
  `.onError` callbacks through an `IExecutor`; the executor is *how* async
  results land on the right thread.
- `error_handling.md` — the framework-wide error-propagation story that the
  per-task logging here plugs into (currently also summarised under
  *Error propagation* in `../../ARCHITECTURE.md`).
- `concurrency_and_lifetimes.md` — the broader threading and teardown-ordering
  model; the "destroy strand before base pool" rule above is a concrete instance
  of it (see also *Thread safety* in `../../ARCHITECTURE.md`).
- [`bridge.md`](bridge.md) — the bridge wires backends to a GUI executor and a
  strand-backed dispatcher; it is the primary consumer of these types.
