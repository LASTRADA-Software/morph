# `morph::offline` — offline support

`morph::offline` provides the building blocks for a network-aware application
that degrades gracefully when the backend is unreachable. It covers four
concerns:

1. **Detecting** the connectivity state (`NetworkMonitor`).
2. **Queuing** actions that could not be delivered (`IOfflineQueue`,
   `InMemoryOfflineQueue`).
3. **Replaying** queued actions on reconnect, with retry and dead-letter
   semantics (`SyncWorker`).
4. **Orchestrating** the reconnect → activate → bind → replay sequence
   (`ReconnectCoordinator`, `ReconnectOutcome`, `ReconnectCoordinatorConfig`).

All types live in `morph::offline`.

## Type overview

| Type | Header | Role |
|---|---|---|
| `NetworkMonitor` / `NetworkMonitorConfig` | `network_monitor.hpp` | Background probe thread + online/offline state machine. |
| `QueueItem`, `IOfflineQueue`, `InMemoryOfflineQueue` | `offline_queue.hpp` | Passive store of undelivered actions (opaque payloads). |
| `SyncWorker` / `SyncResult` | `sync_worker.hpp` | Drains + replays a queue with retry/dead-letter. |
| `ReconnectCoordinator`, `ReconnectOutcome`, `ReconnectCoordinatorConfig`, `ReconnectCoordinator::Deps` | `reconnect_coordinator.hpp` | Orders reconnect → activate → bind → replay, with abort checks. |

## Contents

- [NetworkMonitor](#networkmonitor)
- [NetworkMonitor callback constraint](#networkmonitor-callback-constraint)
- [Offline queue](#offline-queue)
- [Ownership: who enqueues](#ownership-who-enqueues)
- [SyncWorker](#syncworker)
- [ReconnectCoordinator](#reconnectcoordinator)
- [End-to-end integration](#end-to-end-integration)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Design decisions](#design-decisions)
- [Cross-references](#cross-references)

## NetworkMonitor

A background thread calls a user-supplied probe function at regular intervals.
The monitor starts *online* and transitions to *offline* only after
`failureThreshold` consecutive failures. It returns to *online* after
`onlineThreshold` consecutive successes. Callbacks fire on the probe thread.

The monitor is non-copyable and non-movable. Destroy it to stop monitoring.

### `NetworkMonitorConfig`

| Field | Type | Default | Purpose |
|---|---|---|---|
| `probeInterval` | `std::chrono::milliseconds` | `5s` | Time between probe calls. |
| `failureThreshold` | `int` | `3` | Consecutive failures before going offline. |
| `onlineThreshold` | `int` | `1` | Consecutive successes before going online. |

Declared outside `NetworkMonitor` so its default member initialisers are fully
parsed before any constructor default argument evaluates — a nested incomplete
type breaks constructor-default-argument lookup on clang/GCC.

### `NetworkMonitor` API

| Member | Signature | Notes |
|---|---|---|
| `ProbeFunction` | `std::function<bool()>` | Returns `true` when the network is reachable. |
| `Callback` | `std::function<void()>` | Called on state change. |
| `Config` | `NetworkMonitorConfig` | Alias for the config struct. |
| ctor | `NetworkMonitor(ProbeFunction, Callback onOffline, Callback onOnline, Config = {})` | Launches the probe thread immediately. |
| dtor | `~NetworkMonitor()` | Calls `stop()` then spin-waits on `_runExited` to handle the case where `stop()` was called from within a probe callback (avoiding deadlock on `join()`). |
| `isOnline()` | `bool isOnline() const noexcept` | Reads an atomic flag; safe from any thread. |
| `stop()` | `void stop()` | Signals the thread to stop. Idempotent. If called from the probe thread itself, detaches instead of joining. |

**Probe exceptions are swallowed** — a throwing probe is treated as a failed
probe (`safeProbe` catches everything and returns `false`).

## NetworkMonitor callback constraint

**`onOffline` and `onOnline` run on the probe thread, inline inside the probe
loop.** Look at `run()`: it waits on the condition variable for `probeInterval`,
calls `safeProbe`, then calls `handleProbeResult`, which invokes the callback
*before* the loop can circle back to wait for the next interval. There is no
executor, no second thread, and no queue between the probe result and the
callback — whatever the callback does, the probe thread does.

Consequences:

- **A blocking callback stalls all probes.** While the callback runs, the next
  `wait_for` has not started, so no further connectivity checks happen. A
  callback that blocks for 30s means 30s of connectivity blindness.
- **Running the coordinator or `SyncWorker` inline is a mistake.** A
  `ReconnectCoordinator::onOnline()` can spin for up to
  `maxAttempts * retryDelay` (≈20s at defaults) of retry-and-sleep, and a
  `SyncWorker::run()` executes arbitrarily long replay work. Doing either
  directly inside a callback runs *seconds of retry loop on the probe thread*,
  which is exactly the thread that is supposed to be watching the network.
- **The safe shape is: set an atomic, or post to an executor, and return.**
  The callback should do O(1) work — flip a flag, `post()` a lambda onto a
  worker executor — and let the heavy sequencing run elsewhere. This is why
  `ReconnectCoordinator::onOnline()`/`onOffline()` are documented as
  "posted onto a worker executor by the host, not called on the probe thread."

Calling `stop()` from within a callback is supported (it detaches rather than
joins to avoid a self-deadlock — see the dtor/`stop()` notes above), but it is
still a callback running on the probe thread and must not block first.

See `concurrency_and_lifetimes.md` for the framework-wide rule that
notification callbacks marshal work off the thread that raised them.

## Offline queue

### `QueueItem`

| Field | Type | Purpose |
|---|---|---|
| `id` | `uint64_t` | Stable identifier assigned at enqueue time. |
| `payload` | `std::string` | Opaque serialised representation of the queued action. |

The payload format is the caller's choice — JSON, binary-hex, plain text, etc.

### `IOfflineQueue`

Minimal interface for durable storage of undelivered actions. Accepts items
while offline; `SyncWorker` drains and replays them on reconnect.

| Member | Signature | Notes |
|---|---|---|
| `enqueue` | `uint64_t enqueue(std::string payload)` | Appends payload. Returns a stable id. |
| `drain` | `std::vector<QueueItem> drain()` | Returns all pending items in enqueue order, without removing them. Safe to call multiple times — items survive between `drain()` and the corresponding `markDone()`. |
| `markDone` | `void markDone(uint64_t itemId)` | Removes the item identified by `itemId`. No-op if not found. |

### `InMemoryOfflineQueue`

Thread-safe in-memory implementation of `IOfflineQueue`. Items live in a
`std::deque<QueueItem>` protected by a `std::mutex`. Ids are monotonically
increasing. Suitable for testing and applications that do not require
persistence across restarts.

## Ownership: who enqueues

**The queue is passive.** `IOfflineQueue` exposes `enqueue` / `drain` /
`markDone` and nothing else — it has no notion of a backend, a transport, or a
"failed request." It never fills itself. The framework supplies no transport
layer that would notice a write failed and drop it into the queue, so
**detecting an offline/failed `execute()` and calling `enqueue()` is the
application's job.**

The seam is on the *write path*, not inside `morph::offline`. A host that wants
offline durability wraps its own dispatch:

```cpp
// Application code — the framework does not write this for you.
void submit(const MyAction& action) {
    if (!monitor.isOnline()) {              // known offline: don't even try
        queue.enqueue(serialise(action));
        return;
    }
    try {
        bridge.execute(action);             // attempt delivery
    } catch (const std::exception&) {       // delivery failed at the edge
        queue.enqueue(serialise(action));   // trap it into the queue
    }
}
```

`SyncWorker` closes the loop on the *read path*: on reconnect it `drain()`s the
same queue and replays each payload. The two halves share one `IOfflineQueue`
instance (see [End-to-end integration](#end-to-end-integration)) — the
application owns the "enqueue on failure" half, the framework owns the "drain
and replay" half. Neither `NetworkMonitor` nor `ReconnectCoordinator` enqueues
anything; they only *observe* and *sequence*.

Because the framework never calls `enqueue`, the serialisation format is
entirely the caller's (`QueueItem::payload` is an opaque `std::string`), and it
is the caller's responsibility that the same format round-trips through the
`SyncWorker::ReplayFunction`.

## SyncWorker

Replays queued actions from an `IOfflineQueue` on reconnect. Drains the queue
and calls a caller-supplied `ReplayFunction` for each item.

### `SyncResult`

| Field | Type | Default | Purpose |
|---|---|---|---|
| `successful` | `int` | `0` | Items replayed and removed from the queue. |
| `failed` | `int` | `0` | Items that failed and remain in the queue for retry. |
| `deadLettered` | `int` | `0` | Items that exhausted their retry budget and were dropped (logged at `morph::log::LogLevel::error`). |

### `SyncWorker` API

| Member | Signature | Notes |
|---|---|---|
| `ReplayFunction` | `std::function<bool(const std::string&)>` | Return `true` → success, `false` → failure. Throwing is treated as failure. |
| ctor | `SyncWorker(IOfflineQueue&, ReplayFunction)` | References the queue and the replay callable. |
| `run()` | `SyncResult run()` | Drains the queue and replays each item. Concurrent calls are serialised by an internal mutex. Returns immediately if `stop()` was called before acquiring the lock. |
| `stop()` | `void stop()` | Signals an in-progress `run()` to stop after the current item. One-shot — the flag resets at the start of the next `run()`. |

**Retry & dead-letter (hard-coded defaults):**

- Each item is retried up to **5 attempts** across successive `run()` calls.
- Items that fail their 5th attempt are dropped and logged at
  `morph::log::LogLevel::error` (the payload appears in the log line).
- Items that succeed implicitly reset their attempt counter (they are removed).
- There are intentionally no public knobs — the framework guarantees obvious,
  safe defaults.

The per-item attempt counter lives in a `std::unordered_map<uint64_t, int>`
keyed by `QueueItem::id`.

## ReconnectCoordinator

Sequences the reconnect → activate → bind → replay steps when the network comes
back. All side effects are injected via `Deps`; the coordinator contains only
the retry loop, the ordering guarantees, and the abort checks. It performs no
I/O and owns no thread — `onOnline()` / `onOffline()` run synchronously on the
calling thread.

### `ReconnectOutcome`

| Enumerator | Meaning |
|---|---|
| `Reconnected` | Backend reopened, made active, context bound, queue replay invoked. |
| `GaveUp` | Exhausted `maxAttempts` without a successful reconnect; stayed offline. |
| `Aborted` | `shouldContinue()` returned false before any reconnect attempt. |

### `ReconnectCoordinatorConfig`

| Field | Type | Default | Purpose |
|---|---|---|---|
| `maxAttempts` | `int` | `10` | Max reconnect attempts per `onOnline()` call. |
| `retryDelay` | `std::chrono::milliseconds` | `2s` | Delay between failed attempts. |

### `ReconnectCoordinator` API

| Member | Signature | Notes |
|---|---|---|
| `Config` | `ReconnectCoordinatorConfig` | Alias. |
| `Deps` | struct | Injected side-effect callbacks (see below). |
| ctor | `explicit ReconnectCoordinator(Deps, Config = {})` | Non-copyable, non-movable. Null `Deps` members are logged in debug builds. |

#### `Deps` struct

| Field | `std::function` signature | Purpose |
|---|---|---|
| `tryReconnect` | `bool()` | Attempt to (re)open the primary backend. Throwing → failed attempt. |
| `activatePrimary` | `void()` | Make the freshly-reconnected primary the active backend. Called once per successful `onOnline()`, after `tryReconnect()` succeeds. |
| `activateLocal` | `void()` | Switch the active backend to the local/offline one. Called by `onOffline()`. |
| `bindContext` | `void()` | Rebind per-connection/per-session context to the active backend. Called after every `activate*` step. Must not throw. |
| `replay` | `void()` | Replay the offline queue against the now-active primary. Typically wraps `SyncWorker::run()`. Called last in `onOnline()`. |
| `shouldContinue` | `bool()` | Return `false` to abort the current `onOnline()` early (e.g. monitor reports offline mid-retry). Polled before each attempt and once more before replay. |
| `sleep` | `void(std::chrono::milliseconds)` | Sleep between failed attempts. Tests substitute a no-op/counter; hosts wire to `std::this_thread::sleep_for`. |

#### Ordering guarantees (the reason this class exists)

Within a successful `onOnline()`, the steps run in strict order:

1. `tryReconnect()` returns `true`.
2. `activatePrimary()` — make primary the active backend.
3. `bindContext()` — rebind per-connection/per-session state.
4. `replay()` — drain + replay the offline queue.

Step 4 MUST NOT run before step 3, and step 3 MUST NOT run before step 2.

#### `onOnline()`

```cpp
ReconnectOutcome onOnline();
```

Synchronous. Runs the retry loop. For each attempt:

1. Check `shouldContinue()` — abort if false.
2. Call `tryReconnect()` — skip to sleep if false.
3. On success: `activatePrimary()`, `bindContext()`, re-check
   `shouldContinue()` before `replay()`, return `Reconnected`.
4. Sleep `retryDelay` (except after the final attempt).
5. After `maxAttempts` failures, log a warning and return `GaveUp`.

#### `onOffline()`

```cpp
void onOffline();
```

Calls `activateLocal()` then `bindContext()`. Idempotent — safe to call when
already local.

#### Thread safety

`onOnline()` and `onOffline()` are mutually serialised by an internal mutex.
They are intended to be posted onto a worker executor by the host, not called
directly on the probe thread.

## End-to-end integration

The four types compose into one pipeline. The rule that ties them together:
**the probe callback does no work of its own — it posts, and the coordinator
does the sequencing on a worker executor, and the coordinator's `replay`
dependency wraps `SyncWorker::run()` over the same queue the application
enqueues into.**

```cpp
morph::offline::InMemoryOfflineQueue queue;   // shared by both halves
morph::exec::SomeExecutor worker;             // host's worker executor

morph::offline::SyncWorker sync{
    queue,
    [&](const std::string& payload) { return deliver(payload); }  // ReplayFunction
};

morph::offline::ReconnectCoordinator coordinator{{
    .tryReconnect    = [&] { return backend.reopen(); },
    .activatePrimary = [&] { bridge.switchBackend(makePrimary()); },
    .activateLocal   = [&] { bridge.switchBackend(makeLocal()); },
    .bindContext     = [&] { session.rebind(); },
    .replay          = [&] { sync.run(); },          // <-- SyncWorker over the shared queue
    .shouldContinue  = [&] { return monitor.isOnline(); },
    .sleep           = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); },
}};

// Callbacks run on the probe thread, so they ONLY post — never run the
// coordinator inline (see "NetworkMonitor callback constraint").
morph::offline::NetworkMonitor monitor{
    [] { return tcpProbe(); },                                   // ProbeFunction: bool()
    [&] { worker.post([&] { coordinator.onOffline(); }); },      // onOffline
    [&] { worker.post([&] { coordinator.onOnline();  }); },      // onOnline
};
```

Flow: the `bool()` probe drives `NetworkMonitor`'s state machine → on a
transition the callback *only* posts a lambda to `worker` (it must not run
reconnect logic inline on the probe thread) → the worker runs
`ReconnectCoordinator::onOffline()` / `onOnline()` → a successful `onOnline()`
calls `activatePrimary` → `bindContext` → `replay`, and `replay` runs
`SyncWorker::run()`, which drains and replays the `queue` the application filled
on the write path ([Ownership: who enqueues](#ownership-who-enqueues)).

### Reconciling with ARCHITECTURE.md's direct wiring

`ARCHITECTURE.md` shows a simpler wiring where the callbacks call
`bridge.switchBackend(...)` directly:

```cpp
morph::offline::NetworkMonitor monitor{
    myTcpProbe,
    [&] { bridge.switchBackend(std::make_unique<LocalBackend>(localPool)); },
    [&] { bridge.switchBackend(std::make_unique<SimulatedRemoteBackend>(server)); }
};
```

Both are legitimate; they are different points on a spectrum:

- **Direct `switchBackend` — the minimal path.** No retry, no ordered
  replay, no abort-on-flap. `switchBackend` is a bounded mutex operation (it is
  not a seconds-long retry loop), so calling it inline on the probe thread is
  acceptable *as a minimal demo*. It does not replay a queue and has no
  `shouldContinue` guard.
- **`ReconnectCoordinator` — the ordered, tested path.** Use it when reconnect
  can *fail and need retries*, when replay must run strictly *after* activate +
  bind, and when a mid-retry flap-back-offline must abort cleanly. This is the
  path with the ordering invariant and the guarantees this file documents. Its
  own callbacks must be posted off the probe thread precisely because the retry
  loop can run for seconds.

Rule of thumb: a demo or a backend switch with no pending writes can use direct
`switchBackend`; anything that must not lose queued writes on a flaky link uses
the coordinator, with `replay` wrapping `SyncWorker::run()`.

## Failure modes

The pipeline has several sharp edges that callers must design around. None are
bugs — they are consequences of the deliberately minimal contracts.

### No head-of-line blocking in replay

`SyncWorker::run()` does **not** stop at the first failing item. When
`_replay` returns `false` (or throws) it increments that item's attempt counter
and *continues to the next item*, replaying and `markDone`-ing later items that
succeed. Therefore **"enqueue order is preserved" holds only when every item
succeeds.** If item #2 fails and item #3 succeeds, #3 is delivered and removed
while #2 stays queued for a later `run()` — the backend sees #3 before a
subsequent retry of #2. Callers that need strict ordering across failures must
enforce it themselves (e.g. a replay function that refuses to process #3 until
#2 lands).

### Retry counter is in-memory and resets on restart

The per-item attempt count lives in `SyncWorker::_attempts`
(`std::unordered_map<uint64_t,int>`), a plain member — **not** in the queue.
It is lost when the process exits. A queue that survives restarts (a SQL-backed
`IOfflineQueue`) will therefore re-present a poison item with its counter back
at zero after every restart, so it can never actually dead-letter across
restarts. **Durable dead-lettering requires storing the attempt count in the
queue**, which the current `QueueItem` (id + payload only) does not carry.

### `Reconnected` can be returned without replaying

`onOnline()` returns `ReconnectOutcome::Reconnected` after a successful
`tryReconnect` + `activatePrimary` + `bindContext`, but it re-checks
`shouldContinue()` *once more before `replay()`*. If that final check is false
(the backend went away again during activate/bind), **`replay()` is skipped and
the outcome is still `Reconnected`.** `Reconnected` means "we reconnected and
bound," not "the queue was replayed." A caller that keys off the outcome to
decide whether the queue is drained will be wrong in this window.

### First offline report is delayed, and `onOnline` never fires at startup

`NetworkMonitor::run()` `wait_for`s `probeInterval` *before* the first probe, so
the first probe is at `t = probeInterval`, and `failureThreshold` consecutive
failures are needed to flip offline. **The earliest an `onOffline` can fire is
`probeInterval * failureThreshold`** — ≈15s at defaults (5s × 3). An app that is
offline from the very start still reports online for that whole window.
Separately, the monitor **starts in the online state**, and callbacks fire only
on *transitions*, so **`onOnline` never fires at startup** — there is no
online→online edge. Startup activation is the host's job (call `onOnline()` /
`activatePrimary` explicitly at boot if the backend is expected up).

### Null `Deps` construct successfully then crash

`ReconnectCoordinator`'s constructor only *logs* null `Deps` members (in debug
builds, via `assertDepsNonNull`); it does not throw. A coordinator built with a
null `tryReconnect`/`replay`/etc. constructs fine and later crashes when
`onOnline()`/`onOffline()` invokes the null `std::function`. Treat the debug log
line as the only warning you get.

### `onOnline()` holds the mutex for the entire retry loop

`onOnline()` takes `_mtx` at entry and holds it across the whole loop —
including every `sleep(retryDelay)` — for up to `maxAttempts * retryDelay`
(≈20s at defaults). Because `onOffline()` shares that mutex, **a
flap-back-offline cannot preempt an in-progress `onOnline()` by acquiring the
lock**; it can only take effect through `shouldContinue()` returning `false` at
the next poll. Wire `shouldContinue` to the live monitor state
(`monitor.isOnline()`) so a flap is actually observed, rather than to a stale
snapshot.

## Limitations

Honest boundaries of what ships today:

- **Opaque `std::string` payload discards the typed-codec machinery.** The rest
  of morph moves typed actions through the wire codec (`wire.md`); the offline
  queue stores an opaque blob and hands an opaque blob to the replay function.
  Serialisation, versioning, and type-safety across the enqueue→replay boundary
  are entirely on the caller — the compiler will not catch a format mismatch.
- **No idempotency contract, but replay must be idempotent anyway.** `drain()`
  is non-destructive and `markDone` runs only *after* a successful replay, so a
  crash (or a `false` return) *after* the side effect has committed re-invokes
  `replay` on the same payload on the next `run()`. Retries and post-commit
  failures both re-run the payload. The framework provides no dedup or
  "exactly-once" guarantee — **the replay function MUST be idempotent** and the
  spec cannot enforce it.
- **Only an in-memory queue ships.** `InMemoryOfflineQueue` loses everything on
  exit. A durable/SQL-backed `IOfflineQueue` is the caller's to write (the
  interface is designed for it, but no implementation is provided here).
- **Dead-lettering is log-only.** A poison item that exhausts its 5 attempts is
  `markDone`-d (dropped) and written to `morph::log` at error level. There is
  **no recovery hook** — no dead-letter queue, no callback, no way to inspect or
  requeue it programmatically. If the log sink drops it, it is gone.
- **Null `Deps` are not rejected at construction** (see Failure modes) — a
  misconfigured coordinator is a latent crash, not a constructor error.
- **`onOnline()` serialises the whole retry loop under one mutex**, so
  responsiveness to a mid-retry state change is bounded only by the
  `shouldContinue()` poll cadence, not by lock hand-off.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Monitor probe interval | **Caller-chosen, default 5s** | Tunable per application; 5s is polite for most backends. |
| State transitions use thresholds | **`failureThreshold` / `onlineThreshold`** | A single failed probe does not flip state — hysteresis avoids flapping on transient blips. |
| Probe exceptions | **Swallowed, treated as `false`** | A crashing probe should not tear down the monitor; the host fixes the probe. |
| Queue interface | **Minimal virtual interface (`IOfflineQueue`)** | Lets callers swap in SQLite, file-backed, or test queues without framework changes. |
| `drain` is non-destructive | **Items survive between `drain()` and `markDone()`** | Crash safety: a crash after `drain()` but before `markDone()` does not lose items. |
| SyncWorker retry count | **Hard-coded at 5, no public knob** | The framework guarantees obvious, safe defaults; apps that need different math wrap or replace `SyncWorker`. |
| SyncWorker thread safety | **Internal mutex serialises `run()`** | Second caller blocks — safe to fire from multiple executors. |
| Reconnect retry loop | **Synchronous, no background thread** | The host owns the executor; the coordinator is pure orchestration with no hidden threads. |
| Reconnect step ordering | **Explicit in the `onOnline()` body** | The strict order (reconnect → activate → bind → replay) is the class's reason to exist — callers should never have to get it right themselves. |
| `onOnline()` / `onOffline()` serialised | **Same internal mutex** | Prevents a race where a concurrent `onOffline()` replays into a local backend during an in-progress `onOnline()`. |
| `shouldContinue` re-checked before replay | **Second poll after bind** | The backend may have gone away during `activatePrimary()` / `bindContext()` — never replay into a backend that just became unreachable. |
| No sleep after final attempt | **`retryDelay` skipped on last iteration** | Wasting 2s after we already know we're giving up serves no purpose. |

## Cross-references

- **`bridge.md`** — `Bridge::switchBackend` is the mechanism the coordinator's
  `activatePrimary`/`activateLocal` dependencies drive (re-registers live
  handlers on the new backend, fires `onBackendChanged`). ARCHITECTURE.md's
  minimal wiring calls `switchBackend` straight from the monitor callback; see
  [Reconciling with ARCHITECTURE.md's direct wiring](#reconciling-with-architecturemds-direct-wiring).
- **`journal.md`** — the action log is a permanent, append-only audit/replay
  trail; `IOfflineQueue` is transient (holds pending writes, deletes them on
  delivery). The two are distinct: the journal's ordering is authoritative and
  never dropped by the framework, whereas offline replay ordering only holds
  when every item succeeds (see [Failure modes](#failure-modes)). Do not conflate
  the offline queue's replay with journal replay.
- **`concurrency_and_lifetimes.md`** — the framework-wide rule that notification
  callbacks marshal work off the raising thread (the reason
  [NetworkMonitor callbacks must only post](#networkmonitor-callback-constraint)),
  plus the monitor's teardown/`stop()`-from-callback contract.
- **`error_handling.md`** — how a failed `execute()` surfaces to the application
  (the signal that drives [enqueue-on-failure](#ownership-who-enqueues)), and
  the framework's swallow-and-treat-as-failure policy that this file mirrors in
  `safeProbe`, `SyncWorker`'s replay `try/catch`, and the coordinator's
  `callTryReconnect`/`callShouldContinue`.
