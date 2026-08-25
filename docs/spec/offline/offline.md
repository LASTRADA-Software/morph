# `morph::offline` — offline support

`morph::offline` provides the building blocks for a network-aware application
that degrades gracefully when the backend is unreachable. It covers four
concerns:

1. **Detecting** the connectivity state (`NetworkMonitor`).
2. **Queuing** actions that could not be delivered (`IOfflineQueue`,
   `InMemoryOfflineQueue`, `FileOfflineQueue`, `SqliteOfflineQueue`).
3. **Replaying** queued actions on reconnect, with retry and dead-letter
   semantics (`SyncWorker`).
4. **Orchestrating** the reconnect → activate → bind → replay sequence
   (`ReconnectCoordinator`, `ReconnectOutcome`, `ReconnectCoordinatorConfig`).

All types live in `morph::offline`.

## Type overview

| Type | Header | Role |
|---|---|---|
| `NetworkMonitor` / `NetworkMonitorConfig` | `network_monitor.hpp` | Background probe thread + online/offline state machine. |
| `QueueItem`, `IOfflineQueue`, `InMemoryOfflineQueue` | `offline_queue.hpp` | Passive store of undelivered actions (opaque payloads); durable retry-attempt tracking. |
| `FileOfflineQueue` | `file_offline_queue.hpp` | Reference NDJSON-file-backed durable queue; no extra dependency, ships by default. |
| `SqliteOfflineQueue` | `sqlite_offline_queue.hpp` | Reference SQLite-backed durable queue; opt-in (`MORPH_BUILD_OFFLINE_SQLITE`). |
| `SyncWorker` / `SyncResult` / `SyncWorker::DeadLetterSink` | `sync_worker.hpp` | Drains + replays a queue with durable-attempt-aware retry/dead-letter. |
| `ReconnectCoordinator`, `ReconnectOutcome`, `ReconnectCoordinatorConfig`, `ReconnectCoordinator::Deps` | `reconnect_coordinator.hpp` | Orders reconnect → activate → bind → replay, with abort checks. |

## Contents

- [NetworkMonitor](#networkmonitor)
- [NetworkMonitor callback constraint](#networkmonitor-callback-constraint)
- [Offline queue](#offline-queue)
- [Ownership: who enqueues](#ownership-who-enqueues)
- [SyncWorker](#syncworker)
- [Conflict resolution on replay](#conflict-resolution-on-replay)
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
| `id` | `uint64_t` | Stable identifier assigned at enqueue time. **Queue-local** — not a cross-subsystem key. |
| `payload` | `std::string` | Opaque serialised representation of the queued action. |
| `idempotencyKey` | `std::string` | Optional caller-supplied dedup token, stable across subsystems and restarts for one logical op. Empty by default. |
| `attempts` | `uint32_t` | Durable retry count, authoritative when the queue persists it via `setAttempts()`. Defaults to `0`. |

The payload format is the caller's choice — JSON, binary-hex, plain text, etc.

#### `idempotencyKey`: deduping against the journal

`QueueItem::id` is **queue-local** — a durable queue re-presents the same logical
op with a fresh `id` after a restart, and the journal's `seq` is journal-local,
so the two subsystems share no identity. That is exactly the seam where an op can
be **double-applied**: the offline queue and the journal can each replay the same
logical operation with nothing to recognise it as already-applied.

`idempotencyKey` is the shared dedup token that closes it. It is a caller-minted
value — a stable content hash or a client-generated operation id (e.g. a UUID),
reused if the *same* op is re-enqueued — that stays constant for one logical
operation across the queue, the journal, and process restarts. A replay consumer
that has applied an op records its key and skips any later item (from either
path) carrying the same key.

Set it by enqueuing through the two-argument overload:

```cpp
queue.enqueue(serialise(action), operationId(action));  // payload + idempotency key
```

The queue **stores the key verbatim and never interprets it**. The key is opaque
to `morph::offline`, just like the payload.

Uniqueness enforcement is a **floor, not a prohibition**. The interface does not
*require* an implementation to enforce uniqueness, so a replay consumer must
always dedup on the key itself — a conforming queue may present the same key
twice. An implementation is nonetheless **permitted** to dedup at enqueue time as
a deliberate strengthening: `InMemoryOfflineQueue` never dedups, while
`FileOfflineQueue` (linear scan) and `SqliteOfflineQueue` (partial unique index)
both do.

Where an implementation does dedup, these are the semantics — identical in both,
and pinned for every shipped implementation by
`tests/offline_queue_conformance.hpp`:

- Only a **non-empty** key already carried by a **pending** item is a dedup
  candidate. An empty key is never a dedup token: two empty-key enqueues always
  produce two distinct items, in every implementation.
- The call **succeeds** and returns the **existing** item's id, so the return
  value does not distinguish a store from a hit.
- A hit is **first-write-wins with silent payload loss** — the new payload is
  discarded, the pending item keeps the payload it already had, and no error is
  raised and nothing is reported to the caller. A caller that re-enqueues a
  *corrected* payload under an unchanged key loses the correction; mint a new key
  when the payload changes.
- `markDone()` releases the key: once the pending item is gone, the same key
  enqueues normally again.
- A durable queue still recognises the key after a reopen.

**The dedup contract.** The offline queue replay and the journal replay must be
wired so a logical op is applied **at most once**. There are two ways to satisfy
it, and a host must pick one:

1. **Mutually exclusive replay** — drive replay through exactly one path (the
   `SyncWorker` queue path *or* the journal replay), never both over the same
   ops. This is the existing guidance (see
   [Conflict resolution on replay](#conflict-resolution-on-replay) and the
   journal cross-reference) and needs no key.
2. **Shared idempotency key** — if both paths can replay the same ops, every
   enqueued item and its corresponding journal entry must carry the *same*
   `idempotencyKey`, and the replay consumer must dedup on it (apply a key once,
   skip repeats). The framework provides the *field*; wiring the check into the
   replay/flush path is the host's responsibility (that logic lives in
   `sync_worker.hpp` and the app's journal-replay code, not in the queue).

### `IOfflineQueue`

Minimal interface for durable storage of undelivered actions. Accepts items
while offline; `SyncWorker` drains and replays them on reconnect.

| Member | Signature | Notes |
|---|---|---|
| `enqueue` | `uint64_t enqueue(std::string payload)` | Appends payload with no idempotency key. Returns a stable (queue-local) id. |
| `enqueue` | `uint64_t enqueue(std::string payload, std::string idempotencyKey)` | Appends payload carrying the dedup key (stored on `QueueItem::idempotencyKey`). Virtual with a default that delegates to the one-arg `enqueue` then stamps the key via the protected `setIdempotencyKey`, so existing implementations keep working; the key is dropped by an implementation with no per-item storage that overrides neither. |
| `drain` | `std::vector<QueueItem> drain()` | Returns all pending items in enqueue order, without removing them. Safe to call multiple times — items survive between `drain()` and the corresponding `markDone()`. |
| `markDone` | `void markDone(uint64_t itemId)` | Removes the item identified by `itemId`. No-op if not found. |
| `setAttempts` | `void setAttempts(uint64_t itemId, uint32_t attempts)` | Persists an updated attempt count for an item. **Public** (unlike `setIdempotencyKey`) because `SyncWorker` calls it from outside the queue after every failed replay. Default no-op; `InMemoryOfflineQueue` overrides it to update the in-deque item. A queue that overrides it to store the count durably makes `SyncWorker`'s retry budget survive a process restart. |
| `size` | `std::size_t size() const` | Number of pending items, without removing them. Default calls `drain().size()` — correct but O(n) and allocates a full snapshot to answer a size query; every shipped implementation overrides it with a direct count. |
| `maxDepth` | `std::optional<std::size_t> maxDepth() const` | The capacity `enqueue()` enforces, or `std::nullopt` if unbounded. Default: `std::nullopt` — preserves current behavior for any `IOfflineQueue` subclass written before this method existed. |
| `setIdempotencyKey` (protected) | `void setIdempotencyKey(uint64_t itemId, std::string key)` | Hook the default two-arg `enqueue` uses to stamp the key onto an already-enqueued item. Default no-op; `InMemoryOfflineQueue` records the key directly instead. **A conflicting non-empty key is skipped, never raised** — an implementation that deduplicates leaves the item unkeyed rather than failing, because the default `enqueue` has already inserted by the time it stamps, so the row exists either way and an exception could not undo it. That path therefore yields an extra *unkeyed* item, not a dedup hit; a caller wanting dedup uses the virtual two-arg `enqueue` (see below). |

`drain()` is `const` — it takes a snapshot and mutates nothing, so `size()`'s
default can call it (and so can an application) without needing a non-`const`
reference to the queue.

#### Depth bound and overflow policy

`IOfflineQueue` has no depth bound by default — `maxDepth()` returns
`std::nullopt` and `enqueue()` never rejects an item on capacity grounds
unless a concrete queue is constructed with an explicit bound. Every shipped
implementation (`InMemoryOfflineQueue`, `FileOfflineQueue`,
`SqliteOfflineQueue`) takes an `std::optional<std::size_t> maxDepth =
std::nullopt` as the **last** constructor parameter; passing a value turns on
enforcement for that instance.

**Policy: reject-newest.** Once a bounded queue holds `maxDepth()` items, a
further `enqueue()` throws `OfflineQueueFullError` instead of admitting the
new item — the queue never silently evicts an older item or invokes an
app-defined eviction callback:

```cpp
/// Thrown by enqueue() when the queue is at its configured maxDepth().
struct OfflineQueueFullError : std::runtime_error {
    OfflineQueueFullError(std::size_t maxDepth, std::size_t currentSize);
    std::size_t maxDepth;     // the configured capacity that was reached
    std::size_t currentSize;  // pending items at the time of rejection
};
```

`maxDepth`/`currentSize` are equal for a well-behaved implementation; both are
carried on the exception so a caller can log or branch on the numbers without
re-querying the queue. Immediately before throwing, each implementation emits
the `queueOverflow` counter metric with the rejection-time size as its value
(see [observability.md](../core/observability.md)).

Per-implementation notes:

- **`InMemoryOfflineQueue`** checks capacity under its existing lock, before
  the deque `push_back`. It has no idempotency-key dedup at all, so there is
  no dedup-hit-vs-capacity ordering question here.
- **`FileOfflineQueue`** runs its existing keyed-dedup scan *first*; the
  capacity check sits after it, before `appendPut`. A dedup hit (a re-enqueue
  of an already-pending key) therefore always succeeds and returns the
  existing id, even on a full queue — it inserts nothing new, so there is
  nothing to reject.
- **`SqliteOfflineQueue`** checks capacity (`SELECT COUNT(*)`) before
  attempting either INSERT path (empty-key and keyed). For the keyed path,
  this means the check runs *before* the `INSERT ... ON CONFLICT ... DO
  NOTHING` can resolve to a dedup hit — a re-enqueue of an already-queued key
  can be rejected if the queue happens to be full at that moment, even though
  it would have inserted nothing. This is a deliberate, documented
  conservatism: avoiding it would require a second round trip (insert
  speculatively, then check whether it was actually a no-op conflict) purely
  to special-case a narrow situation (re-enqueuing an already-queued
  idempotency key while the queue is simultaneously full).

`maxDepth` is a per-construction parameter, not persisted in the file or
database — a host that reopens `FileOfflineQueue`/`SqliteOfflineQueue` over
the same path must pass the same `maxDepth` argument again to keep the same
cap enforced; nothing on disk remembers it.

### `InMemoryOfflineQueue`

Thread-safe in-memory implementation of `IOfflineQueue`. Items live in a
`std::deque<QueueItem>` protected by a `std::mutex`. Ids are monotonically
increasing. Overrides `setAttempts` to update the in-deque item, so the
attempt count is current for as long as the queue object lives — but it has
no persistence, so a process restart still resets it to `0`. Suitable for
testing and applications that do not require persistence across restarts.

### `FileOfflineQueue`

Reference append-only, NDJSON-backed `IOfflineQueue` (`file_offline_queue.hpp`)
that persists `payload`, `idempotencyKey`, and `attempts` across process
restarts with **no extra dependency** — it ships in the default `morph` target
alongside `InMemoryOfflineQueue`. Each mutation (`enqueue`, `markDone`,
`setAttempts`, `setIdempotencyKey`) appends one JSON line
(`{"op": "put"|"done", "id", "payload", "idempotencyKey", "attempts"}`) and
immediately `fflush`+`fsync`s it. The line is written with
`detail::EscapingWriteOpts` (mirroring `morph::wire::detail::EscapingWriteOpts`,
`core/wire.hpp`) so a raw ASCII control byte in `payload`/`idempotencyKey`
round-trips instead of producing invalid JSON that breaks replay on the next
open — or, alongside an escaped `\`/`"` in the same string, JSON glaze's
writer silently corrupts before it ever reaches disk. On open, the file is replayed
last-write-wins-per-id and rewritten in compacted form — this both bounds file
growth and heals a torn trailing line left by a crash mid-write, tolerating it
the same way `FileActionLog` does (a malformed *trailing* line is logged and
skipped; a malformed line anywhere else is genuine corruption and is
rethrown). New ids resume from the highest id ever seen in the file (including
tombstoned ones), so a fresh item never collides with an old tombstone — and
because compaction drops every tombstone, that high-water mark is carried across
each rewrite explicitly, as a trailing `"done"` record for the mark itself
(emitted only when it exceeds every surviving id, so it can never delete a row a
`"put"` line above just restored). Recording it as a `"done"` needs no reader
change: `load()` already raises the mark for every id it reads, and erasing an
absent id is a no-op. Without it the mark regressed to the highest *surviving*
id on the second restart — enqueue 1 and 2, `markDone(2)`, restart (compacts to
just id 1), restart again, and the next `enqueue()` reissued id 2, the id of a
completed and acknowledged item. Mutations also raise rather than swallow I/O
failures: a short write or a failed `fflush`/`fsync` throws, since every
mutation is documented as a committed transaction by the time the call returns. A
keyed `enqueue`'s dedup is a linear scan over pending items — fine at modest
queue depths; `SqliteOfflineQueue` is the index-backed alternative for
high-volume keyed enqueues. Not safe for multiple processes to open the same
path concurrently.

The constructor takes an optional second `morph::core::FileIoOps` parameter
(`FileOfflineQueue(std::filesystem::path, morph::core::FileIoOps = {})`) — the
same test-only fault-injection seam `FileActionLog` uses (see
`docs/spec/journal/journal.md`): the raw `fwrite`/`fflush`/`fsync`/`fopen`
calls this class makes, as an injectable strategy defaulting to the real
syscalls. A normal caller never passes one.

### `SqliteOfflineQueue`

Reference SQLite-backed `IOfflineQueue` (`sqlite_offline_queue.hpp`), built
only when the host opts in via the `MORPH_BUILD_OFFLINE_SQLITE` CMake option
(default `OFF`; resolved through CMake's bundled `FindSQLite3` module against a
system SQLite3 package, not through `vcpkg.json`, so the default build and its
dependency graph are unaffected). Backed by one table:

```sql
CREATE TABLE IF NOT EXISTS morph_offline_queue (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    payload         TEXT    NOT NULL,
    idempotency_key TEXT    NOT NULL DEFAULT '',
    attempts        INTEGER NOT NULL DEFAULT 0,
    enqueued_at     INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS ix_queue_idem
    ON morph_offline_queue(idempotency_key) WHERE idempotency_key <> '';
```

`id` is `AUTOINCREMENT`: ids are never reused, and a re-opened queue
re-presents each row under its **stored, stable** id — restarting does not
renumber existing rows. (`QueueItem::id` is still queue-local per the general
contract above; cross-restart identity is carried by `idempotencyKey`, not
`id`.) The partial unique index gives insert-time dedup for a non-empty
`idempotencyKey` — a re-enqueue of the same key is a no-op that returns the
existing row's id; empty keys are exempt and are never deduplicated, matching
`InMemoryOfflineQueue`. `drain()` never deletes, so a crash between `drain()`
and `markDone()` loses nothing; every write is its own committed statement
under `PRAGMA journal_mode=WAL`. All operations serialise on an internal
mutex, so the queue is safe to share between the write and drain/replay paths.

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

### Disposition: app-layer by design (the rule-1 carve-out)

The example above puts domain-adjacent code in a free function at the dispatch
site. `examples/IMPLEMENTATION.md` rule 1 would otherwise forbid exactly that
placement — "nothing domain-shaped may live in presenters, QML, `main()`, or
free functions." The placement is deliberate, and this section is its recorded
disposition (morph#197), so a reader who finds
`if (!monitor.isOnline()) queue.enqueue(...)` outside a model knows it is a
sanctioned exception rather than an oversight.

**Rule 1 is not being overridden here; it fired.** Its final clause is "If
logic can't be expressed in a model, that is a finding," and the framework's
prime directive says the same of the framework itself. This carve-out *is*
that finding's outcome, not an argument that the rule is wrong.

**Why a model cannot host it.** `enqueue()` is the write path's last act before
the wire, taken precisely when the wire is unavailable. In the canonical wiring
this file and `ARCHITECTURE.md` both show, the client keeps an in-process
`LocalBackend`, so a client-side model does exist and could in principle own
the decision. On a **remote deployment** — models behind a server, reached over
a `Bridge` — it does not: the one machine that must decide "queue this instead
of sending it" is the one machine with no model on it. morph offers no seam
there, and none of the framework's own offline types fills the gap: neither
`NetworkMonitor` nor `ReconnectCoordinator` enqueues, and the queue stays
passive by design (above).

**What the carve-out covers, and what it does not.**

| Belongs in the write-path seam | Stays in the model |
|---|---|
| Probing `NetworkMonitor::isOnline()`, or catching a failed `execute()` | Validating and authorizing the action (`Context::principal`) |
| Minting the idempotency key, serialising the payload, calling `enqueue()` | Deduping the replayed op against the journal |
| Surfacing queue depth to the UI | Classifying a stale base version as a conflict |
| Client-local bookkeeping that keeps *this client's own* queued items consistent — e.g. a per-entity version ledger so a second offline edit chains onto the first rather than colliding with it | Everything the payload *means* once it lands: replay re-dispatches it as an ordinary typed action |

The last row of the left column is the sharp edge: it is genuinely
domain-shaped, and the carve-out sanctions it only in a **dedicated app-layer
write-path class** — never in a presenter, a QML bridge, or `main()`. The
domain semantics of the queued action never move out of the model; only the
decision to queue it, and the client-local state that decision needs, live in
the seam.

**Reference shapes in the ladder.**

- `examples/lims/include/lims/offline/field_outbox.hpp` — the reference for the
  domain-shaped half. A plain, non-Qt, app-layer class that stamps each queued
  capture with a base version from its own local ledger and advances that
  ledger on enqueue, so a client's second offline edit chains onto its own
  pending first edit. Replay still goes through the model
  (`SampleModel::execute(QueuedCapture)`), which owns validation, authorization
  and conflict classification.
- `examples/kanban/gui_lib/board_qml_bridge.cpp` — the transport-shaped half
  only: probe, mint an op id, serialise, enqueue, update queue depth. It lives
  in a presenter, which rule 1 names as forbidden for *domain-shaped* code;
  nothing there is domain-shaped, so it stands as glue under rule 2's
  "pure glue with no domain logic" justification. Anything with a domain
  invariant in it belongs in a `FieldOutbox`-shaped class instead.

**Scope, and when to revisit.** This is the "explicitly dispositioned in the
spec as app-layer by design" branch of `examples/IMPLEMENTATION.md`'s promotion
rule, taken at **two** occurrences of the transport-shaped seam (kanban's
bridge, lims' outbox) and **one** of the domain-shaped version chaining (lims'
outbox). No framework primitive is owed yet. A third rung independently growing
its own enqueue-on-offline path is the trigger to reopen the question of a
framework-owned outbox dispatcher — a standing disposition must not become the
reason a third reinvention goes unexamined.

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
| `deadLettered` | `int` | `0` | Items that exhausted their retry budget and were dropped — handed to the `DeadLetterSink` if one is set, otherwise logged at `morph::log::LogLevel::error`. |

### `SyncWorker` API

| Member | Signature | Notes |
|---|---|---|
| `ReplayFunction` | `std::function<bool(const std::string&)>` | Return `true` → success, `false` → failure. Throwing is treated as failure. |
| `DeadLetterSink` | `std::function<void(const QueueItem&)>` | Invoked with the exhausted item, just before it is removed, when an item hits the retry cap. Optional — default unset. |
| ctor | `SyncWorker(IOfflineQueue&, ReplayFunction, DeadLetterSink = nullptr)` | References the queue and the replay callable; the sink is an optional third argument. |
| `run()` | `SyncResult run()` | Drains the queue and replays each item. Concurrent calls are serialised by an internal mutex. Returns immediately if `stop()` was called before acquiring the lock. Emits the `queueDepth` metric once, with the drained item count, before replaying (see [observability.md](../core/observability.md)). |
| `stop()` | `void stop()` | Signals an in-progress `run()` to stop after the current item. One-shot — the flag resets at the start of the next `run()`. |

**Retry & dead-letter (hard-coded cap, durable count):**

- Each item is retried up to **5 cumulative attempts**. The count is seeded
  from the larger of the drained `QueueItem::attempts` and `SyncWorker`'s own
  in-memory count, so a queue that persists `attempts` (via `setAttempts()`)
  makes the budget survive a process restart; a queue that leaves
  `setAttempts()` as the default no-op keeps the count purely in-memory
  (the original behavior — it resets whenever a fresh `SyncWorker` is
  constructed).
- After every failed attempt, the new cumulative count is written back
  through `setAttempts()` (a no-op unless the queue overrides it), so a
  persisting queue's next `drain()` — this run, or after a restart — sees the
  updated value.
- Items that fail their 5th cumulative attempt are dropped. If a
  `DeadLetterSink` is set, it receives the exhausted `QueueItem` (payload,
  idempotencyKey, final `attempts` count) instead of the default log line; if
  unset, the item is logged at `morph::log::LogLevel::error` (the payload
  appears in the log line) — the original behavior, unchanged. A throwing sink
  is caught and logged; the item is still removed.
- Items that succeed implicitly reset their attempt counter (they are removed).
- There are intentionally no public knobs on the retry cap itself — the
  framework guarantees obvious, safe defaults.

The per-item attempt counter lives in a `std::unordered_map<uint64_t, uint32_t>`
keyed by `QueueItem::id`, seeded from and written back to the queue as above.

`QueueItem::attempts` (durable retries) and `QueueItem::idempotencyKey` (dedup)
are complementary, not overlapping: `idempotencyKey` prevents an item from
being **double-applied** across the queue and journal replay paths;
`attempts` prevents an item from being **retried forever** across restarts.
Neither is enforced by the queue itself — `SyncWorker` and the replay
consumer act on them.

## Consistency model

The offline path publishes **no consistency guarantee stronger than the three
properties below**. Stating them is not flattering — comparable sync engines
publish causal+ or eventual consistency — but every one is already implied by
the rest of this document, and leaving them unstated invites a host to assume
more.

1. **Per-queue FIFO, best-effort.** `SyncWorker::run()` drains in enqueue order
   and attempts every item in the batch. A failure does **not** stop the run:
   the failed item is left queued for a later attempt and the loop continues to
   the next one (`sync_worker.hpp`). So there is no head-of-line blocking — and
   equally no guarantee that the order items *land* on the server matches the
   order they were enqueued, once any item has failed.
2. **No read-your-writes for a queued write.** An enqueued write is invisible to
   a subsequent read until it actually reaches the server. The queue holds an
   opaque payload; nothing replays it into the local model's state or shadows
   the read path with pending writes.
3. **No cross-client visibility without a re-read.** `subscribe<R>` fans out
   only to handlers on the same `Bridge` (one client process), so a result
   another client produced does not reach this client's subscribers — it is
   observed on this client's next read. See the README's "Instance
   subscriptions are best-effort and in-process".

None of these is a defect to fix; they are the shape of a queue that stores
opaque payloads and replays them through an ordinary action path. A host needing
stronger guarantees builds them above this layer — for instance by making writes
idempotent (see `idempotencyKey` above) and re-reading once a replay completes.

## Conflict resolution on replay

`morph::offline` has **no conflict-resolution machinery of its own.** Neither
`SyncWorker` nor `ReconnectCoordinator` knows what a payload means, so neither
can detect that a queued write was superseded by a change the backend accepted
while the client was offline. `SyncWorker`'s `ReplayFunction` sees only a
`const std::string&` and returns a `bool` — it has no channel to say "delivered,
but the server had a newer version" or "discarded as stale." **Conflict
detection and merge/discard are the model's responsibility**, and the seam the
framework provides for them is `Model::onBackendChanged()`, not `SyncWorker`.

There are therefore two distinct replay paths over the same `IOfflineQueue`,
and a host picks one:

- **`SyncWorker` path** — the framework replays each payload through an opaque
  `ReplayFunction` with the built-in retry/dead-letter policy above. Fits a
  fire-and-forget queue of writes that either land or are retried; the replay
  function returns only success/failure.
- **Model `onBackendChanged()` path** — when the backend switches, `Bridge`
  reconstructs each model on the new backend and fires `onBackendChanged()` on
  that fresh instance (see `bridge.md`). A model that holds a reference to the
  shared `IOfflineQueue` can `drain()` it inside `onBackendChanged()`, decide
  what each item becomes, and `markDone()` it. This is the path that supports
  **clean-replay / merge / discard** outcomes, because the model — not an
  opaque `bool` callback — decides.

  **Two limits on this seam, both structural.** They do not stop the path
  working, but they decide *when* it can run and what it can consult:

  1. **It fires on the switch, and only `LocalBackend` implements it.**
     `Bridge::switchBackend` calls `notifyBackendChanged()` on the **new**
     backend, and `LocalBackend` (`backend.hpp`) is the only `IBackend` that
     implements it — `SimulatedRemoteBackend` (`remote.hpp`), `SocketBackend`
     (`net/socket_backend.hpp`) and `QtWebSocketBackend`
     (`qt/qt_websocket_backend.hpp`) are all `override {}`. So the hook runs
     when a client switches **to** the local backend — going *offline* — and
     does **not** run on reconnect to a remote backend. A host that wants
     replay to happen on reconnect cannot get it from this hook alone as the
     backends stand; it drains at a moment of its own choosing, or uses the
     `SyncWorker` path.

     This also means "classify each item against the now-reachable backend"
     would be the wrong instruction: at the moment the hook fires, the
     now-reachable backend is the local one.
  2. **It runs with no session.** `LocalBackend::notifyBackendChanged` posts
     the call without a `ScopedContext`, so `session::current()` is `nullptr`
     inside `onBackendChanged()` — see
     [session.hpp](../../../include/morph/session/session.hpp)'s note that a
     `Context` exists only during a dispatch. The model cannot identify who is
     replaying, and no code change to this hook would give it a *verified*
     principal: `LocalBackend` never consults an `IAuthorizer` at all, so the
     most it could ever carry is the client-asserted `Bridge::_defaultSession`.
     Replay on this path is therefore unauthenticated by construction; a host
     that needs an authenticated replay must perform it through an action on a
     remote backend, where `RemoteServer` authenticates.

A model on the `onBackendChanged()` path typically drives each drained item
through two caller-supplied hooks (as the conflict-resolution tests do):

1. A **conflict checker** `bool(const std::string& payload)` — `true` means the
   backend already holds a newer version that supersedes this queued write.
2. A **resolver** `std::string(const std::string& payload)`, consulted only for
   conflicting items — a non-empty result is a **merge** (apply the reconciled
   value), an empty result is a **discard** (drop the stale write). Non-
   conflicting items are a **clean replay**.

**Every drained item is `markDone()`d regardless of outcome** — clean replay,
merge, and discard all remove the item from the queue. Unlike the `SyncWorker`
path there is no retry or dead-letter here: the model handles each item exactly
once per backend switch, so it must not leave an item in a state that needs a
later attempt. This path also inherits `onBackendChanged`'s threading contract:
`Bridge::switchBackend` does not run `onBackendChanged()` inline on the caller's
thread — `LocalBackend::notifyBackendChanged` **posts** it onto the model's own
strand (the same per-`ModelId` serial queue `execute` uses). It therefore runs
single-threaded per model, never overlapping an `execute()` on that model, so a
model draining the queue and mutating its own counters there needs **no locking**
of its own state. Because the drain is posted (asynchronous), it completes some
time *after* `switchBackend` returns; a test or host that must observe the
drained result waits for it (the conflict-resolution tests poll a model counter)
rather than assuming it finished synchronously. See
[bridge.md](../core/bridge.md)'s `switchBackend` for the exact posting mechanism.

The two paths are mutually exclusive per queue — a queue drained inside
`onBackendChanged()` and also handed to a `SyncWorker::run()` would be
double-processed. Choose the model path when replay outcomes are richer than
success/failure (conflicts, merges); choose `SyncWorker` when they are not and
you want the built-in retry budget.

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
| ctor | `explicit ReconnectCoordinator(Deps, Config = {})` | Non-copyable, non-movable. Null `Deps` members are logged via `morph::log::logError` in all builds; construction still succeeds. |

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
2. Emit the `reconnectAttempts` metric, then call `tryReconnect()` — skip to sleep if false.
3. On success: `activatePrimary()`, `bindContext()`, re-check
   `shouldContinue()` before `replay()`, return `Reconnected`.
4. Sleep `retryDelay` (except after the final attempt).
5. After `maxAttempts` failures, log a warning and return `GaveUp`.

Every return path also emits the `reconnectOutcome` metric once, tagged
`outcome` = `"Reconnected"` / `"GaveUp"` / `"Aborted"` (see
[observability.md](../core/observability.md)).

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

### Retry counter is in-memory unless the queue opts into persisting it

`QueueItem::attempts` carries the durable retry count, and `SyncWorker` seeds
its own counter from the larger of that field and its in-memory
`std::unordered_map<uint64_t,uint32_t>`, writing the updated count back
through `IOfflineQueue::setAttempts()` after every failed replay. Whether the
budget survives a restart depends entirely on the queue: `InMemoryOfflineQueue`
overrides `setAttempts()` to update its in-deque item (so a fresh `SyncWorker`
over the *same, still-alive* instance sees the persisted count — used to
simulate a restart in tests), but it does not survive the *process* exiting.
`setAttempts()`'s default is a no-op, so a queue that does not override it
always reports `attempts == 0` on `drain()`, and `SyncWorker`'s in-memory
count is the only thing tracking retries — it resets whenever a fresh
`SyncWorker` is constructed, exactly as before this hook existed.
`FileOfflineQueue` and `SqliteOfflineQueue` both override `setAttempts()` to
write the count to disk, so the retry budget — and therefore dead-lettering —
genuinely survives a process restart when either is used as the
`IOfflineQueue` behind `SyncWorker`.

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

`ReconnectCoordinator`'s constructor only *logs* null `Deps` members (in all
builds, via `assertDepsNonNull` calling `morph::log::logError`); it does not
throw. A coordinator built with a null `tryReconnect`/`replay`/etc. constructs
fine and later crashes when `onOnline()`/`onOffline()` invokes the null
`std::function`. Treat the logged error line as the only warning you get.

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
- **Replay must be idempotent; the queue supplies a key but not enforcement.**
  `drain()` is non-destructive and `markDone` runs only *after* a successful
  replay, so a crash (or a `false` return) *after* the side effect has committed
  re-invokes `replay` on the same payload on the next `run()`. Retries and
  post-commit failures both re-run the payload. `QueueItem::idempotencyKey` gives
  a replay consumer a stable token to dedup on (including against journal
  replay — see [`idempotencyKey`: deduping against the journal](#idempotencykey-deduping-against-the-journal)),
  but the queue only *stores* it: it performs no dedup and gives no
  "exactly-once" guarantee itself. **The replay function MUST still be
  idempotent** — either intrinsically, or by checking the key — and the spec
  cannot enforce it.
- **Two durable reference queues ship, plus the in-memory default.**
  `InMemoryOfflineQueue` loses everything on exit and remains the default.
  `FileOfflineQueue` (NDJSON, no extra dependency, ships by default) and
  `SqliteOfflineQueue` (opt-in via `MORPH_BUILD_OFFLINE_SQLITE`) persist
  `payload`, `idempotencyKey`, and `attempts` across restarts. A host that
  needs a different store (a different SQL engine, a remote queue service)
  still writes its own `IOfflineQueue` — the interface remains the seam.
- **Dead-lettering has an optional recovery hook, but no built-in dead-letter
  store.** A poison item that exhausts its 5 cumulative attempts is
  `markDone`-d (dropped); if the host set a `SyncWorker::DeadLetterSink`, it
  receives the exhausted `QueueItem` (payload, idempotencyKey, final
  `attempts` count) instead of the default log line, so it can persist,
  forward, or re-enqueue the item into a separate dead-letter queue of its
  own. With no sink set, the original log-only behavior is unchanged (written
  to `morph::log` at error level; if the log sink drops it, it is gone).
  Either way, morph ships no concrete dead-letter store — the sink is the
  seam, not a built-in queue.
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
| `QueueItem::attempts` / `setAttempts()` | **Opt-in durable retry count, no-op default** | Lets a durable queue make `SyncWorker`'s retry budget survive a restart without changing `InMemoryOfflineQueue`'s or existing `SyncWorker` call sites' behavior. |
| `DeadLetterSink` | **Optional third constructor arg, replaces (not augments) the log line** | Gives a host a programmatic hand-off for a poisoned item; a throwing sink is caught and logged, the item is still removed — consistent with the framework's swallow-and-continue policy. |
| Two shipped durable queues, split by dependency | **`FileOfflineQueue` in the default target; `SqliteOfflineQueue` opt-in** | A host that cannot add a SQLite dependency still gets restart-durability for free; a host that wants indexed dedup and can accept the dependency opts in via `MORPH_BUILD_OFFLINE_SQLITE`. |
| Idempotency-key dedup strengthened in `SqliteOfflineQueue` only | **Partial unique index on non-empty `idempotency_key`** | The base `IOfflineQueue` contract only stores the key; the SQLite reference implementation additionally enforces insert-time dedup as a deliberate strengthening, not a contract change — `FileOfflineQueue` mirrors the same dedup behavior (linear scan) for parity, but neither is required by the interface. |
| SyncWorker thread safety | **Internal mutex serialises `run()`** | Second caller blocks — safe to fire from multiple executors. |
| Reconnect retry loop | **Synchronous, no background thread** | The host owns the executor; the coordinator is pure orchestration with no hidden threads. |
| Reconnect step ordering | **Explicit in the `onOnline()` body** | The strict order (reconnect → activate → bind → replay) is the class's reason to exist — callers should never have to get it right themselves. |
| `onOnline()` / `onOffline()` serialised | **Same internal mutex** | Prevents a race where a concurrent `onOffline()` replays into a local backend during an in-progress `onOnline()`. |
| `shouldContinue` re-checked before replay | **Second poll after bind** | The backend may have gone away during `activatePrimary()` / `bindContext()` — never replay into a backend that just became unreachable. |
| No sleep after final attempt | **`retryDelay` skipped on last iteration** | Wasting 2s after we already know we're giving up serves no purpose. |
| Conflict resolution lives in the model | **`SyncWorker` has no conflict hook; models reconcile in `onBackendChanged()`** | The framework cannot know whether a payload was superseded — only the domain model can. Keeping `SyncWorker`'s contract a plain `bool` avoids baking a conflict model into the framework; hosts that need merge/discard drain the queue inside `onBackendChanged()` instead (see [Conflict resolution on replay](#conflict-resolution-on-replay)). |

## Cross-references

- **`bridge.md`** — `Bridge::switchBackend` is the mechanism the coordinator's
  `activatePrimary`/`activateLocal` dependencies drive (re-registers live
  handlers on the new backend, fires `onBackendChanged`). ARCHITECTURE.md's
  minimal wiring calls `switchBackend` straight from the monitor callback; see
  [Reconciling with ARCHITECTURE.md's direct wiring](#reconciling-with-architecturemds-direct-wiring).
  `onBackendChanged` on the freshly-reconstructed model is also the seam for the
  model-driven replay path in [Conflict resolution on replay](#conflict-resolution-on-replay)
  — a model can `drain()` the shared `IOfflineQueue` there and reconcile each
  item, instead of (not in addition to) using `SyncWorker`.
- **`journal.md`** — the action log is a permanent, append-only audit/replay
  trail; `IOfflineQueue` is transient (holds pending writes, deletes them on
  delivery). The two are distinct: the journal's ordering is authoritative and
  entries already in a sink are never removed by the framework (append-only),
  though a durable sink that throws during `checkpoint()` can permanently lose
  that batch (watermark-advances-first — see journal.md's failure modes). Offline
  replay ordering only holds when every item succeeds (see
  [Failure modes](#failure-modes)). Do not conflate the offline queue's replay
  with journal replay.
- **`file_action_log.hpp`** — `FileOfflineQueue`'s torn-trailing-line tolerance
  and fsync-per-write durability directly mirror `FileActionLog`'s (see
  `docs/spec/journal/journal.md`); the two differ in that `FileOfflineQueue`
  also tombstones and reuses no id, since (unlike the append-only action log)
  its rows are removed and its retry counts are mutated in place.
- **`concurrency_and_lifetimes.md`** — the framework-wide rule that notification
  callbacks marshal work off the raising thread (the reason
  [NetworkMonitor callbacks must only post](#networkmonitor-callback-constraint)),
  plus the monitor's teardown/`stop()`-from-callback contract.
- **`error_handling.md`** — how a failed `execute()` surfaces to the application
  (the signal that drives [enqueue-on-failure](#ownership-who-enqueues)), and
  the framework's swallow-and-treat-as-failure policy that this file mirrors in
  `safeProbe`, `SyncWorker`'s replay `try/catch`, `SyncWorker`'s
  `DeadLetterSink` `try/catch`, and the coordinator's
  `callTryReconnect`/`callShouldContinue`.
- **`observability.md`** — `SyncWorker::run()`'s `queueDepth` gauge and
  `ReconnectCoordinator::onOnline()`'s `reconnectAttempts`/`reconnectOutcome`
  counters, fed through the same injectable `morph::observe` seam
  `RemoteServer`/`LocalBackend` use.
- **`testing_strategy.md`** — the soak test (`tests/soak/test_soak_reconnect_churn.cpp`)
  that drives this exact `NetworkMonitor` → `ReconnectCoordinator` → `SyncWorker`
  pipeline through hundreds of offline/online flaps and asserts the queue
  always fully drains and every reconnect attempt succeeds.
