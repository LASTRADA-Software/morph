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
- [Strands: `ModelStrands` and `ModelId`](#strands-modelstrands-and-modelid)
- [Lifetime & ownership](#lifetime--ownership)
- [Thread safety](#thread-safety)
- [Failure modes](#failure-modes)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Type overview

There are nine types, split across `morph::exec` (in `executor.hpp`),
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
| `ModelStrands` | `morph::exec::detail` | One strand per `ModelId` over an `IExecutor`: core-cpp's `core::async::KeyedStrands`, with what morph adds. Tasks with the same `ModelId` never overlap. |
| `CoreExecutorOver` | `morph::exec::detail` | A `core::async::IExecutor` over a morph `IExecutor`: how a core-cpp strand's pump reaches it. |
| `TaskResumer` | `morph::exec::detail` | A Task handler's resumer: the current executor while the handler runs, queuing its resumptions on its model's strand (see [`coroutines.md`](coroutines.md)). |

`IExecutor` and the two thread-based concrete executors live in the public
`morph::exec` namespace. `QtExecutor` lives in `morph::qt` (in the separate
`qt/qt_executor.hpp` header) because it depends on Qt; only the GUI/bridge layer
pulls it in. `ModelStrands`, `CoreExecutorOver`, `TaskResumer`, `ModelId` and
`ModelIdHash` live in `morph::exec::detail` because they are implementation
details of the morph model framework, not general-purpose utilities.

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
and any strands built on it would never drain. Passing `0` therefore yields a usable single-worker pool rather than
a silently dead one; values `≥ 1` spawn exactly that many workers.

The destructor signals stop, notifies all workers, and joins every thread. The
workers **drain** the queue before exiting: once `_stop` is set the loop exits
only when `_stop && _q.empty()`, so workers keep popping and running
already-queued tasks (including a strand's next turn, queued from within a
running task) until the queue is empty. The join therefore blocks until every task
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
the next link. Without the guard this segfaults ordinary uninstrumented
builds, not merely sanitizer runs.

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
the running Qt event loop *is* the dispatcher. It owns no queue and spawns no
thread, but it is **not** stateless and does have a shutdown concern of its own:
besides the `context` pointer it holds an `_alive` control block, and a task
still queued when the executor is destroyed is dropped rather than run — see
[Teardown: queued tasks are dropped, not delivered](#teardown-queued-tasks-are-dropped-not-delivered)
above.
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

## Strands: `ModelStrands` and `ModelId`

A strand per model instance over any `IExecutor`. Tasks posted with the same
`ModelId` key execute in FIFO order with no overlap, even when the underlying
executor is a thread pool. Tasks with different keys may run concurrently.

`ModelId` is an opaque 64-bit identifier. Zero is reserved and means "not
bound". Non-zero values are assigned by the backend and are stable for the
lifetime of the model. It supports three-way comparison and can be used as an
`unordered_map` key via `ModelIdHash`.

The strands are core-cpp's `core::async::KeyedStrands<ModelId, ModelIdHash>`
(`<core/async/KeyedStrands.hpp>`, since core-cpp 0.4.0), which was written after
the `StrandExecutor` morph had here and replaces it. What it guarantees, core-cpp
specifies and tests:

- **One strand per key, made when the key gets work and retired when it runs
  out,** under the registry's lock, so a post that races a retirement never
  leaves two strands for one key. Up to 32 retired strands are kept, with their
  pump frames, queue room and map nodes, for the next key that needs one: in the
  steady state a post costs one allocation (the callable) and nothing else. A
  kept strand and a kept map node each still hold the key they last served, so
  up to 64 `ModelId`s outlive their work; a `ModelId` is eight bytes.
- **A pump per strand, a batch per turn.** A strand queues itself on the base
  once however many tasks arrive while it is busy, runs at most
  `StrandOptions::batch` (32) tasks per turn, and hands the base back.
- **A current executor per batch.** A strand states itself with
  `core::async::ExecutorScope` while it runs, so `runningHere(key)` answers
  inside its tasks, and an awaitable that resumes on the current executor
  brings a coroutine back to it (see [`coroutines.md`](coroutines.md)).

What `ModelStrands` adds:

- **The base adapter.** `CoreExecutorOver` turns the morph `IExecutor` into a
  `core::async::IExecutor`. A strand hands its base one bare coroutine handle
  per turn, which the adapter posts as a lambda holding that handle alone:
  trivially copyable, so it fits `std::function`'s small buffer and a turn costs
  no allocation. The lambda does not refer to the adapter.
- **A throw is logged, not propagated.** `post(key, fn)` wraps `fn` in
  `LoggedTask`, which catches what it throws and logs it: `std::exception` as
  `"[strand] task threw: " + what()`, any other type as `"[strand] task threw
  unknown exception"`. The next task for the key runs as usual. A core-cpp strand
  would propagate the throw to whoever resumed its pump, and under MSVC's `cl`
  end the process.
- **`runOnStrand(key, fn)`** runs `fn` at once when the calling thread is inside
  a task of `key`'s strand, posts it there otherwise, and runs it at once when
  the strands are closed. It is how a Task handler's end reaches its strand from
  wherever the handler finished.
- **`drain()`** blocks until nothing is queued or running on any strand,
  including work posted while it waits; not from one of the strands' own tasks,
  which a debug build asserts. The single-threaded WebAssembly build has no
  other thread to finish the work and allows no blocking wait, so there it
  returns at once.
- **`close()`** closes every strand: queued work is dropped, a task running on
  another thread is waited for, and a later `post` is dropped too.
- **`seal()`** (core-cpp 0.4.1) refuses the try-forms and keeps running what
  is queued: `trySubmit` and `runOnStrand`'s post are refused, so their
  callers run the work inline, while a plain `post` -- and a coroutine coming
  back through a plain submit -- is still queued until the close.
- **`teardown(stopHandlers, order)`** is the whole sequence: stop the Task
  handlers and seal, in `order`, then `drain()`, then `close()`. Where threads
  exist the order is stop, `drain()`, seal, so a stopped handler still unwinds
  on its strand, and a handler's end that arrives from a socket's loop while
  its instance's tasks drain is queued behind them rather than run inline
  beside one, which would enter the action gate on two threads at once. On
  the single-threaded build it is seal then stop, so a stopped
  handler's resumption is refused and runs inline in the stop, since nothing
  else could run it. Once sealed, a resumption or a handler's end that arrives
  -- between the drain and the close included -- runs inline where it arrives,
  instead of reaching a strand the close would drop.
- **The Task handler's context.** `enroll(key, resumer)` installs a Task
  handler's session and resumer around every task of `key`'s strand until
  `withdraw(key)`, through the strands' keyed around-task hook. The hook costs
  one atomic load per task while no handler is enrolled.

`ModelStrands` is held by `std::shared_ptr`: a `TaskResumer` shares it, so a
handler suspended past its backend's destruction can still ask whether the
strands are closed.

**Testing per-model ordering without naming `ModelStrands`/`ModelId`.**
`RemoteServer` (see `backend.md`) owns its strands internally, but every
task it ever dispatches — the top-level `handle()` post and every strand's turn
alike — funnels through the single `IExecutor` the server was constructed with.
A caller that wants a deterministic, hand-stepped interleaving harness against
`RemoteServer`'s real per-model ordering does not need to touch
`morph::exec::detail::ModelStrands` or `morph::exec::detail::ModelId` at all:
constructing the server against a single-step, test-controlled `IExecutor` (see
`tests/test_support.hpp`'s `morph::testing::StepExecutor`) and driving it one
task at a time is enough — `RemoteServer`'s own wire replies carry the model id
as a plain `uint64_t` (`wire::Envelope::modelId`), so a test never needs the
`ModelId` vocabulary either.

## Lifetime & ownership

`ModelStrands` holds its base `IExecutor` by reference (inside
`CoreExecutorOver`) and does not own it. Two rules follow:

1. **The base `IExecutor` must outlive the strands and keep running tasks until
   `drain()` has returned.** Every strand's turn is posted to it, and `drain()`
   waits for the turns that are queued. A pool destroyed first would drop them,
   and the drain would wait forever.
2. **What the strands posted must still run, or be dropped by the base.** A
   strand's pump that the base runs after the strands were closed finds its
   strand closed and ends. One the base drops unrun (a `MainThreadExecutor`
   destroyed with its queue) leaks that strand's state: close the strands, and
   pump the base, before destroying it.

Destroying the strands, or calling `close()`, drops what is still queued and
waits for a task running on another thread. It does not wait for the task it is
called from: a task may release the last reference to the strands' owner, as a
`RemoteServer`'s may. A backend whose queued work must run tears its strands
down with `teardown()` instead: `~LocalBackend` and `~SynchronousBackendAdapter`
do.

> **Destroy the backend before the base pool it runs on.**

With member declaration order this means the pool must be declared *before*
the backend (members destroy in reverse order), or the two must be torn down
explicitly in that order.

The strand registry is self-cleaning: a key's strand is retired when its queue
runs out, so live strands track the model instances with work, not every model
ever seen. Retired strands kept for reuse are bounded at 32.

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
- `ModelStrands`' members are callable from any thread. Its locking is
  core-cpp's: the registry's lock is taken before any strand's own, so a post
  and a retirement for one key are serialised (see `KeyedStrands.hpp`). The
  enrolled-handler table has a mutex of its own, taken by the around-task hook
  only while a handler is enrolled. The net guarantee: tasks with the same
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
| `ModelStrands` | `LoggedTask`, around every posted callable, catches it. `std::exception` is logged as `"[strand] task threw: " + what()`; any other type is logged as `"[strand] task threw unknown exception"`. The next task for the key proceeds. |
| `MainThreadExecutor` | `runFor` catches **only** `std::exception`, logged as `"[main-thread] callback threw: " + what()`, then continues with the next task. **Any non-`std::exception` type propagates out of `runFor()`** and is the caller's problem. |
| `QtExecutor` | No `try`/`catch` of its own. A throwing task propagates into whoever drives the target thread's event loop (`QCoreApplication::exec` by default, or the worker thread's loop for a custom context); Qt's default behaviour is to `std::terminate`. Tasks posted through it must not let exceptions escape. |

All logging goes through `morph::log::logError`. The design principle: a task
failure must never kill a worker/strand or abort sibling tasks, but it must also
never be *invisible*. Previously these exceptions were swallowed silently; they
are now logged. `ThreadPoolExecutor` and `ModelStrands` catch `(...)` and so
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

### `ModelStrands` (`morph::exec::detail`)

| Member | Signature | Notes |
|---|---|---|
| ctor | `explicit ModelStrands(IExecutor& base, core::async::StrandOptions options = {})` | Strands over `base`. `options.aroundTask` must be unset. |
| dtor | `~ModelStrands()` | Closes the strands: drops what is queued, waits for a task running on another thread. |
| `post` | `template <typename F> void post(ModelId key, F&& task)` | Queues on `key`'s strand. FIFO per key, concurrent across keys. Thread-safe. One allocation; the throw is logged. Dropped once closed. |
| `runOnStrand` | `template <typename F> void runOnStrand(ModelId key, F task)` | Runs here on `key`'s strand, posts off it, runs here once closed. |
| `trySubmit` | `bool trySubmit(ModelId key, std::coroutine_handle<> handle)` | Queues a resumption unless closed. |
| `runningHere` / `runningAnyHere` | `bool runningHere(ModelId key) const noexcept` / `bool runningAnyHere() const noexcept` | Whether the calling thread is inside a task of `key`'s strand / of any. |
| `idle` | `bool idle() const` | Nothing queued or running. |
| `drain` | `void drain()` | Blocks until idle, where threads exist. Not from a task of these strands. |
| `close` | `void close()` | As the destructor. Idempotent. |
| `seal` | `void seal()` | Refuses `trySubmit` and `runOnStrand`'s post; queued work still runs, and `post` is still admitted. Idempotent. |
| `teardown` | `template <typename Stop> void teardown(Stop&& stopHandlers, TeardownOrder order = buildTeardownOrder)` | Stop and seal in `order`, then `drain`, then `close`. |
| `enroll` / `withdraw` | `void enroll(ModelId key, const std::shared_ptr<TaskResumer>&)` / `void withdraw(ModelId key)` | Install / remove a Task handler's session and resumer around `key`'s tasks. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Task signature | `std::function<void()>` | Simple, universal. Every executor accepts the same callable type. No return value, no cancellation. |
| Exception handling | **Caught and logged, never propagated out of a worker/strand** | A task failure must not crash unrelated tasks *or* vanish. `ThreadPoolExecutor` and `ModelStrands` catch `(...)` and log via `morph::log::logError`; `MainThreadExecutor` narrows its catch to `std::exception` so a non-standard throw surfaces on the synchronous drain thread. See [Failure modes](#failure-modes). |
| ThreadPoolExecutor drain-on-dtor | **Drain the queue, then join** | Workers run every already-queued task before exiting, so a strand's queued turn still runs, and finds its strand closed, as long as the pool outlives the strands. There is no public `waitIdle`/graceful-shutdown API; tasks posted after destruction begins may be lost, so the caller must still synchronise teardown order externally. |
| MainThreadExecutor's `runFor` | **Wall-clock deadline** | Lets the caller batch-process tasks without spinning. The condition-variable wait avoids busy-waiting. |
| MainThreadExecutor's `runOnce`/`drain` | **Thin wrappers sharing `runFor`'s dequeue-and-invoke step, added alongside it** | `runOnce()` steps exactly one task without blocking; `drain()` loops `runOnce()` until the queue is empty. Neither waits on new tasks from other threads, unlike `runFor()`'s deadline-scoped wait — this gives event-loop integrations and tests deterministic, non-blocking single-step control without replacing `runFor()`'s existing behavior. |
| ModelId zero | **Reserved — "not bound"** | A natural sentinel for optional/uninitialised model handles. |
| The strand | **core-cpp's `KeyedStrands`**, not morph's own | morph's `StrandExecutor` became core-cpp's `Strand` and `KeyedStrands` in 0.4.0, which the other Contour Terminal projects share; morph keeps only what is morph's: the adapter, the throw policy and the Task handler's context. Its two fixed races (a post racing the drain, a recycled strand under the wrong key) are core-cpp's to keep fixed now, in its own race tests. |
| `ModelStrands` in `detail` | **Not a general-purpose utility** | Exists only for the morph model framework's per-model serialisation. The `ModelId` key is specific to model instances. |
| Closing drops, `drain` waits | **Seal, drain, close, in `teardown`** | core-cpp's strands drop queued work when closed, so that a strand can be destroyed from one of its own tasks. A backend whose queued work must run seals first, so that nothing arriving after the drain is queued only to be dropped, and drains before it closes. |
| No `std::future` / return value | **Fire-and-forget only** | Executors schedule side-effect tasks. Callers that need results use shared state or futures externally. |
| No `std::executor` conformance | **Custom interface, not `std::executor`** | C++26 `std::executor` is not yet widely available. This is a minimal in-house abstraction. |
| `QtExecutor` via `invokeMethod`, not a `QObject` subclass | **Near-stateless free-standing `IExecutor`, target configurable via ctor** | Uses `QMetaObject::invokeMethod(context, fn, Qt::QueuedConnection)`, so callers need no custom `QObject`, event type, or slot — they only optionally supply a `QObject*` to pick the target thread. Defaults to `QCoreApplication::instance()` so existing GUI-thread call sites are unaffected. Keeps the type a drop-in `IExecutor` holding a single pointer, with Qt's event loop as the sole dispatcher. |
| `QtExecutor` in a separate `morph::qt` header | **Isolate the Qt dependency** | The core executor family (`executor.hpp`, `strand.hpp`) stays Qt-free; only the GUI/bridge layer includes `qt/qt_executor.hpp`. Backends depend on `IExecutor`, never on Qt. |

## Limitations

These are honest, known gaps — accepted trade-offs, not bugs:

- **Unbounded queues / no backpressure.** `ThreadPoolExecutor`, `MainThreadExecutor`,
  and each strand's queue are all unbounded. A producer that
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
  in-flight work must synchronise externally. (`ModelStrands::drain` waits for
  the strands, but it relies on the base pool still running their turns.)

## Lifetime annotations

`ModelStrands`' and `CoreExecutorOver`'s constructors mark their
`IExecutor& base` `MORPH_LIFETIMEBOUND` (`morph/attributes.hpp`), so Clang
diagnoses a call site that hands one a base executor which does not outlive it —
the hang described above, caught at compile time instead of at teardown. See [concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#morph_lifetimebound--the-must-outlive-rules-told-to-the-compiler).

## Cross-references

- [`completion.md`](completion.md) — `Completion<T>` marshals its `.then` /
  `.onError` callbacks through an `IExecutor`; the executor is *how* async
  results land on the right thread.
- `error_handling.md` — the framework-wide error-propagation story that the
  per-task logging here plugs into (currently also summarised under
  *Error propagation* in `../../ARCHITECTURE.md`).
- `concurrency_and_lifetimes.md` — the broader threading and teardown-ordering
  model; the "destroy the backend before the base pool" rule above is a concrete instance
  of it (see also *Thread safety* in `../../ARCHITECTURE.md`).
- [`bridge.md`](bridge.md) — the bridge wires backends to a GUI executor and a
  strand-backed dispatcher; it is the primary consumer of these types.
