# Durable offline queue & cross-restart dead-lettering (planned)

> **Status: planned — not yet implemented.** This spec extends
> [offline.md](../spec/offline.md). It closes the "retry counter is in-memory and resets
> on restart" and "dead-lettering is log-only" limitations documented there. See
> [todo.md](../todo.md).

## The gap

`SyncWorker` retries each queued item up to 5 attempts, then dead-letters it
(drops + logs at error level). But the attempt counter lives in a plain member:

```cpp
std::unordered_map<uint64_t, int> _attempts;   // SyncWorker, keyed by QueueItem::id
```

Two consequences, both documented in `offline.md`'s Failure modes:

1. **The counter is lost on restart.** A durable (SQL-backed) `IOfflineQueue`
   re-presents a poison item after a restart with a **fresh** `QueueItem::id` and
   the counter back at zero — so it can never actually dead-letter across
   restarts. It retries forever, 5 attempts per process lifetime.
2. **Dead-lettering is log-only.** A poisoned item is `markDone`-d (dropped) and
   written to `morph::log` at error level. There is no dead-letter queue, no
   callback, no way to inspect or requeue it. If the log sink drops it, it is
   gone.

The root cause is the same: the retry budget is process state, but durability
requires it to be **queue** state, and the current `QueueItem` (id + payload +
idempotencyKey) carries no attempt count.

## Goal

Make the retry budget survive restarts, and give dead-lettered items a
programmatic fate instead of a log line — both **opt-in** so the in-memory queue
and existing `SyncWorker` behavior are unchanged by default.

## Design

### Part 1 — attempt count in `QueueItem`

Add a durable attempt counter to the item so a persistent queue can store it:

```cpp
struct QueueItem {
    std::uint64_t id{};              // queue-local, may change across restarts
    std::string   payload;
    std::string   idempotencyKey;    // stable across restarts (existing)
    std::uint32_t attempts{0};       // NEW: durable retry count
};
```

- **`attempts` is authoritative when the queue persists it.** `SyncWorker` reads
  `item.attempts` as the starting count instead of consulting its in-memory
  `_attempts` map, increments it on a failed replay, and **writes it back through
  the queue** so the next `drain()` (this run or after a restart) sees the
  updated value.
- The write-back is a new optional `IOfflineQueue` hook, mirroring the
  `setIdempotencyKey` pattern already used for the two-arg `enqueue`:

```cpp
/// @brief Persists an updated attempt count for an item. Default: no-op.
///
/// A durable queue overrides this to store the count so the retry budget
/// survives a restart. `InMemoryOfflineQueue` overrides it to update the
/// in-deque item. A queue that does not override it keeps the pre-existing
/// process-local retry behavior (SyncWorker's own map).
virtual void setAttempts(std::uint64_t itemId, std::uint32_t attempts) {}
```

- **Backward compatibility.** If a queue does not override `setAttempts`,
  `SyncWorker` falls back to its existing in-memory `_attempts` map — today's
  behavior exactly. `InMemoryOfflineQueue` may override it (updating the deque)
  or leave the fallback; either way non-durable queues behave as before.
  `QueueItem::attempts` defaults to `0`, so an item enqueued by old code decodes
  with a zero count.

### Part 2 — a dead-letter sink

Replace the log-only terminal step with an optional callback so a host can
capture, persist, or requeue a poisoned item:

```cpp
/// @brief Invoked when an item exhausts its retry budget, just before markDone.
///
/// Receives the exhausted item (payload, idempotencyKey, final attempt count).
/// If unset, the framework keeps today's behavior: log at error level and drop.
using DeadLetterSink = std::function<void(const QueueItem& poisoned)>;
```

- `SyncWorker` gains an optional `DeadLetterSink` (constructor argument or
  setter). On the attempt that exhausts the budget, before `markDone`, it invokes
  the sink (if set) with the full `QueueItem`; then removes the item as today.
- **The sink still runs, then the item is `markDone`-d.** Dead-lettering remains
  terminal for the primary queue — the sink is the hand-off, not a veto. A host
  that wants to *requeue* re-enqueues into a separate dead-letter queue from
  inside the sink.
- If the sink itself throws, the throw is caught and logged (the item is still
  removed) — a failing dead-letter sink must not wedge the drain, consistent with
  the framework's swallow-and-continue policy (`error_handling.md`).
- **Backward compatibility.** No sink set → the current log-at-error-and-drop
  path runs verbatim.

### The retry budget stays a fixed 5

`offline.md`'s decision "SyncWorker retry count hard-coded at 5, no public knob"
is unchanged. Durability is about *where the count lives*, not *what the limit
is*. A host needing different math still wraps or replaces `SyncWorker`.

## Interaction with idempotency

`QueueItem::idempotencyKey` (existing, `offline.md`) and durable `attempts` are
complementary:

- `idempotencyKey` prevents an item from being **double-applied** across the
  queue and journal replay paths (the replay consumer dedups on it).
- Durable `attempts` prevents an item from being **retried forever** across
  restarts.

A durable queue that stores both gives genuine at-most-once-with-bounded-retry
across process restarts — the combination `offline.md` today can only approximate
within a single process lifetime. Neither is enforced by the queue itself: the
queue *stores* the key and the count; `SyncWorker` and the replay consumer act on
them.

## What still does not ship here

- **No durable `IOfflineQueue` implementation.** As in `offline.md`, only
  `InMemoryOfflineQueue` ships. This spec adds the *fields and hooks* a
  SQL/file-backed queue needs (`attempts`, `setAttempts`), not the backend
  itself.
- **No built-in dead-letter queue.** The `DeadLetterSink` is the seam; a
  concrete dead-letter store (another `IOfflineQueue`, a table, a file) is the
  host's to provide.
- **No change to `drain`/`markDone` semantics.** `drain()` stays non-destructive;
  `markDone` still removes only after a successful replay (or after
  dead-lettering). Crash-safety between `drain()` and `markDone()` is unchanged.

## Testing (planned)

- A queue that persists `attempts`: a poison item fails 5 times **across a
  simulated restart** (a fresh `SyncWorker` over the same persisted items) and is
  dead-lettered on the 5th cumulative attempt, not retried afresh.
- A queue that does **not** override `setAttempts`: retry behavior is identical
  to today (in-memory count, resets on a new `SyncWorker`).
- A `DeadLetterSink` set: it receives the exhausted `QueueItem` (payload +
  idempotencyKey + final count) exactly once before the item is removed; a
  throwing sink is caught and the item is still removed.
- No `DeadLetterSink` set: the log-at-error-and-drop path is unchanged.
- `QueueItem::attempts` defaults to `0` and round-trips through enqueue/drain.

## Cross-references

- [offline.md](../spec/offline.md) — `IOfflineQueue`, `QueueItem`, `InMemoryOfflineQueue`,
  `SyncWorker`, the `setIdempotencyKey` hook this mirrors, and the two Failure
  modes ("retry counter resets on restart", "dead-lettering is log-only") this
  closes.
- [journal.md](../spec/journal.md) — the permanent audit trail, distinct from the
  transient queue; `idempotencyKey` is the shared token that lets a replay
  consumer dedup across both.
- [error_handling.md](../spec/error_handling.md) — the swallow-and-treat-as-failure
  policy the dead-letter sink's own error handling follows.
