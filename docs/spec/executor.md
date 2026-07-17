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
- [`StrandExecutor` and `ModelId`](#strandexecutor-and-modelid)
- [Lifetime & ownership](#lifetime--ownership)
- [Thread safety](#thread-safety)
- [Failure modes](#failure-modes)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Type overview

There are six types, split across `morph::exec` (in `executor.hpp`) and
`morph::exec::detail` (in `strand.hpp`):

| Type | Namespace | Purpose |
|---|---|---|
| `IExecutor` | `morph::exec` | Abstract base: a single pure-virtual `post(task)`. |
| `ThreadPoolExecutor` | `morph::exec` | Fixed-size thread pool, FIFO queue, task exceptions logged (never propagate). |
| `MainThreadExecutor` | `morph::exec` | Collects tasks from any thread, drains on `runFor()` from the owning thread. |
| `ModelId` | `morph::exec::detail` | Opaque 64-bit identifier for a model instance, used as a strand key. |
| `ModelIdHash` | `morph::exec::detail` | Hash functor so `ModelId` can be an `unordered_map` key. |
| `StrandExecutor` | `morph::exec::detail` | Per-key serialising wrapper — tasks with the same `ModelId` never overlap. |

`IExecutor` and the two concrete executors live in the public `morph::exec`
namespace. `StrandExecutor`, `ModelId`, and `ModelIdHash` live in
`morph::exec::detail` because they are implementation details of the morph model
framework, not general-purpose utilities.

## `IExecutor` — the abstract interface

A pure-virtual `post(std::function<void()>)` that schedules a callable for
asynchronous execution. Thread-safe. How a task's exception is handled is left
to the implementation; the header's default wording says an exception "is
silently swallowed unless the implementation documents otherwise", and both
concrete executors here *do* document otherwise — see
[Failure modes](#failure-modes). No implementation lets a task exception escape
`post()` (the task has not run yet when `post()` returns).

## `ThreadPoolExecutor`

A fixed-size thread pool. The constructor spawns `n` worker threads; each worker
loops, waiting for tasks on a shared condition variable. Tasks are dispatched in
FIFO order from a single mutex-protected queue.

The destructor signals stop, notifies all workers, and joins every thread.
Remaining queued tasks that have not started are dropped — there is no drain
phase. Exceptions from tasks are caught in the worker `loop` and logged via
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
`_inFlight` counter (guarded by the map mutex) that the destructor waits on. The
drain-and-erase logic in `scheduleNext` acquires both the map lock and the
strand lock together in a single scoped lock to close a race window where a
concurrent `post()` could re-arm a strand being erased.

Each strand task is the point where the model's own code actually runs, so the
task wrapper catches exceptions and logs them via `morph::log::logError` (see
[Failure modes](#failure-modes)) before deciding whether to keep the strand
running. A throw therefore neither stalls the strand nor skips the drain-and-erase
bookkeeping: the next queued task for that key still runs.

The destructor waits for all in-flight tasks to complete (`_inFlight == 0`)
before destroying the strand map.

## Lifetime & ownership

`StrandExecutor` stores a raw pointer to the base `IExecutor` (`_base`, copied
into each `Strand::base`). It does **not** own the base and never extends its
lifetime. Two invariants make the arrangement safe, and violating either is a
latent bug:

1. **The base `IExecutor` must outlive the `StrandExecutor`.** Every strand
   dispatch calls `strand->base->post(...)`, and `~StrandExecutor` blocks until
   the last of those dispatched lambdas has run. So the base must still be alive
   for the whole life of the strand, *including* the destructor's wait.

2. **The base must actually run — not drop — every task the strand posts.**
   `~StrandExecutor` only returns once `_inFlight` reaches 0, and `_inFlight` is
   decremented *inside* the dispatched lambda, after the task runs. If the base
   silently discards a queued lambda instead of running it, that decrement never
   happens and the strand destructor waits forever.

These two combine into the framework's most important ordering rule for these
types. `~ThreadPoolExecutor` **drops** queued-but-unstarted tasks (no drain),
whereas `~StrandExecutor` **blocks** until `_inFlight == 0`. Therefore:

> **Always destroy the `StrandExecutor` before the base pool it wraps.**

Destroying the base `ThreadPoolExecutor` first drops any strand lambdas still
sitting in the pool queue; their `--_inFlight` never runs, and the subsequent
`~StrandExecutor` deadlocks on its condition variable forever. With member
declaration order this means the pool must be declared *before* the strand
(members destroy in reverse order), or the two must be torn down explicitly in
that order.

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

All three executors' `post()` methods are safe to call from any thread
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
  `pending` queue and `running` flag. The drain-and-erase step in `scheduleNext`
  acquires both `{_mapMtx, strand->mtx}` in one `scoped_lock` (deadlock-free via
  `std::lock`'s ordering) to close the race where a concurrent `post()` re-arms a
  strand between clearing `running` and erasing the map entry — which would
  otherwise orphan a live strand and let two strands for one key run
  concurrently. The net guarantee: tasks with the same `ModelId` never overlap;
  tasks with different keys may run in parallel on the base pool.

## Failure modes

| Executor | What happens when a task throws |
|---|---|
| `ThreadPoolExecutor` | The worker `loop` catches it. `std::exception` is logged as `"[thread-pool] task threw: " + what()`; any other type is logged as `"[thread-pool] task threw unknown exception"`. The worker keeps looping. |
| `StrandExecutor` | The strand task wrapper catches it. `std::exception` is logged as `"[strand] task threw: " + what()`; any other type is logged as `"[strand] task threw unknown exception"`. The strand's drain/erase bookkeeping and `_inFlight` decrement still run, so the next task for the key proceeds. |
| `MainThreadExecutor` | `runFor` catches **only** `std::exception`, logged as `"[main-thread] callback threw: " + what()`, then continues with the next task. **Any non-`std::exception` type propagates out of `runFor()`** and is the caller's problem. |

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
| ctor | `explicit ThreadPoolExecutor(std::size_t n)` | Spawns `n` worker threads. `n` must be > 0. |
| dtor | `~ThreadPoolExecutor() override` | Signals stop, joins all workers. Remaining queued tasks are dropped. |
| `post` | `void post(std::function<void()> task) override` | Enqueues to FIFO; notifies one worker. Thread-safe. Task exceptions caught and logged in the worker loop. |

### `MainThreadExecutor : IExecutor`

| Member | Signature | Notes |
|---|---|---|
| `post` | `void post(std::function<void()> task) override` | Enqueues; notifies waiters. Thread-safe. Not executed until `runFor()`. |
| `runFor` | `void runFor(std::chrono::milliseconds timeout)` | Runs tasks until the `timeout` deadline (blocks for new tasks while time remains; does not return early on an empty queue). Must be called from the owning thread. `std::exception`s logged and skipped; other exception types propagate. |

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
| ThreadPoolExecutor drain-on-dtor | **Drop remaining, don't drain** | No way to know which tasks are safe to run during shutdown. The caller must synchronise externally if in-flight tasks matter. |
| MainThreadExecutor's `runFor` | **Wall-clock deadline, not a `runOnce()`** | Lets the caller batch-process tasks without spinning. The condition-variable wait avoids busy-waiting. |
| ModelId zero | **Reserved — "not bound"** | A natural sentinel for optional/uninitialised model handles. |
| StrandExecutor in `detail` | **Not a general-purpose utility** | Exists only for the morph model framework's per-model serialisation. The `ModelId` key is specific to model instances. |
| StrandExecutor destructor | **Waits for in-flight tasks** | Without this, a pool thread running `scheduleNext` can access `_strands` after it has been destroyed (TSan: data race on destructor vs erase). |
| Strand drain-and-erase | **Locks `_mapMtx` and `strand->mtx` atomically** | Prevents a race: a concurrent `post()` re-arms a strand after `running` is cleared but before the map entry is erased, then the erase orphans a live strand, causing two strands for the same key to run concurrently. |
| No `std::future` / return value | **Fire-and-forget only** | Executors schedule side-effect tasks. Callers that need results use shared state or futures externally. |
| No `std::executor` conformance | **Custom interface, not `std::executor`** | C++26 `std::executor` is not yet widely available. This is a minimal in-house abstraction. |

## Limitations

These are honest, known gaps — accepted trade-offs, not bugs:

- **Unbounded queues / no backpressure.** `ThreadPoolExecutor`, `MainThreadExecutor`,
  and each `Strand::pending` are all unbounded `std::queue`s. A producer that
  outruns consumption grows memory without limit; `post()` never blocks or
  rejects. There is no bounded-queue option, no high-water mark, and no way for a
  caller to learn the queue is backing up.
- **No cancellation.** Once posted, a task cannot be cancelled or removed. The
  signature is fire-and-forget `std::function<void()>` with no token, handle, or
  future. The only mitigation anywhere is `QtExecutor`, which drops a callable if
  its target `QObject` has been deleted — that covers only the GUI leg, not the
  thread pool or strands.
- **No graceful drain / `waitIdle` on `ThreadPoolExecutor`.** The destructor drops
  queued-but-unstarted tasks, and there is no method to wait until the queue is
  empty or to flush pending work before shutdown. Callers who need in-flight work
  to finish must synchronise externally. (`StrandExecutor` waits for `_inFlight`,
  but that is a lifetime-safety wait, not a general drain API, and it relies on the
  base pool still running the strand's dispatched lambdas.)
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
  *Error propagation* in `../ARCHITECTURE.md`).
- `concurrency_and_lifetimes.md` — the broader threading and teardown-ordering
  model; the "destroy strand before base pool" rule above is a concrete instance
  of it (see also *Thread safety* in `../ARCHITECTURE.md`).
- [`bridge.md`](bridge.md) — the bridge wires backends to a GUI executor and a
  strand-backed dispatcher; it is the primary consumer of these types.
