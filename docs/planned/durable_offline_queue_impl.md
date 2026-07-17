# A shipped durable `IOfflineQueue` implementation (planned)

> **Status: planned — not yet implemented.** This spec builds directly on
> [durable_queue.md](durable_queue.md) — it provides the concrete SQLite/file
> store that implements the `QueueItem::attempts` / `setAttempts` /
> `DeadLetterSink` contract that spec defines — and extends
> [offline.md](../spec/offline.md). See [todo.md](../todo.md).

## The gap

Only `InMemoryOfflineQueue` ships. [offline.md](../spec/offline.md) is blunt about
it: "`InMemoryOfflineQueue` loses everything on exit. A durable/SQL-backed
`IOfflineQueue` is the caller's to write (the interface is designed for it, but no
implementation is provided here)."

[durable_queue.md](durable_queue.md) closes the *interface* half of durability —
it adds `QueueItem::attempts`, the `setAttempts` write-back hook, and the
`DeadLetterSink` — but explicitly does **not** ship a store: "No durable
`IOfflineQueue` implementation. As in `offline.md`, only `InMemoryOfflineQueue`
ships. This spec adds the *fields and hooks* a SQL/file-backed queue needs
(`attempts`, `setAttempts`), not the backend itself."

So today every host that wants offline durability re-writes the same SQLite/file
`IOfflineQueue` from scratch, and gets the crash-safety and attempt-persistence
subtleties wrong in slightly different ways. B1/B2 ([durable_queue.md](durable_queue.md),
[outbox.md](outbox.md)) are unusable out of the box without it.

## Goal

Ship one reference durable `IOfflineQueue` that persists `payload`,
`idempotencyKey`, and `attempts` across process restarts, correctly implementing
the [offline.md](../spec/offline.md) queue contract and the
[durable_queue.md](durable_queue.md) `setAttempts` write-back — so B1's
cross-restart dead-lettering actually works without every host re-implementing
the store. It is an **optional** component: `InMemoryOfflineQueue` stays the
default, and the durable queue is a separate header a host opts into.

## Design

### The implemented interface (all EXISTING / from durable_queue.md)

The reference queue implements exactly the `IOfflineQueue` surface confirmed in
`offline_queue.hpp`, with no new virtuals of its own:

| Member | Source | Behavior in the durable queue |
|---|---|---|
| `enqueue(payload)` | EXISTING | Inserts a row `(id, payload, '', 0)`; returns the row id. |
| `enqueue(payload, idempotencyKey)` | EXISTING | Inserts `(id, payload, key, 0)` in one write (overrides the two-arg default so the key is stored atomically, not via a second `setIdempotencyKey` round-trip). |
| `drain()` | EXISTING | `SELECT ... ORDER BY id` — returns every pending row in enqueue order, **without deleting** (non-destructive, as the contract requires). |
| `markDone(itemId)` | EXISTING | `DELETE WHERE id = ?`; no-op if absent. |
| `setIdempotencyKey(itemId, key)` (protected) | EXISTING | `UPDATE ... SET key = ? WHERE id = ?`. |
| `setAttempts(itemId, attempts)` | NEW hook from [durable_queue.md](durable_queue.md) | `UPDATE ... SET attempts = ? WHERE id = ?` — this is the write-back that makes the retry budget survive a restart. |

`QueueItem::attempts` (the field [durable_queue.md](durable_queue.md) adds) is
stored in the row and read back by `drain()`, so `SyncWorker` sees the persisted
count as its starting attempt number after a restart — the exact mechanism
[durable_queue.md](durable_queue.md) specifies.

### The schema

```sql
-- Reference durable queue (SQLite variant).
CREATE TABLE IF NOT EXISTS morph_offline_queue (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,  -- QueueItem::id (queue-local)
    payload         TEXT    NOT NULL,                   -- QueueItem::payload (opaque)
    idempotency_key TEXT    NOT NULL DEFAULT '',        -- QueueItem::idempotencyKey
    attempts        INTEGER NOT NULL DEFAULT 0,         -- QueueItem::attempts (durable)
    enqueued_at     INTEGER NOT NULL                    -- ordering / observability
);
CREATE UNIQUE INDEX IF NOT EXISTS ix_queue_idem
    ON morph_offline_queue(idempotency_key) WHERE idempotency_key <> '';
```

- `id` is `AUTOINCREMENT` so it is **queue-local and may change across restarts**
  — exactly the [offline.md](../spec/offline.md) contract ("Queue-local — not a
  cross-subsystem key"). Cross-restart identity is carried by `idempotency_key`,
  not `id`.
- The partial unique index on a non-empty `idempotency_key` gives the "UNIQUE
  constraint on the payload/key" that [ARCHITECTURE.md](../ARCHITECTURE.md)
  anticipates for a SQL-backed queue — a re-enqueue of the *same* logical op (same
  key) is deduplicated at insert. Empty keys (the default) are exempt, so
  keyless items behave exactly as the in-memory queue does.

### Proposed types (NEW)

```cpp
// namespace morph::offline — NEW, in a separate opt-in header
// (e.g. sqlite_offline_queue.hpp), so the core stays dependency-free.

class SqliteOfflineQueue : public IOfflineQueue {
public:
    /// Opens (or creates) the queue database at `path`. Existing rows are
    /// re-presented by the next drain() — the whole point of durability.
    explicit SqliteOfflineQueue(std::filesystem::path path);

    std::uint64_t enqueue(std::string payload) override;
    std::uint64_t enqueue(std::string payload, std::string idempotencyKey) override;
    std::vector<QueueItem> drain() override;
    void markDone(std::uint64_t itemId) override;
    void setAttempts(std::uint64_t itemId, std::uint32_t attempts) override;  // durable write-back
protected:
    void setIdempotencyKey(std::uint64_t itemId, std::string key) override;
};
```

- **Crash-safety matches the interface contract.** `drain()` never deletes, so a
  crash between `drain()` and `markDone()` loses nothing — items reappear on the
  next `drain()` ([offline.md](../spec/offline.md)'s "`drain` is non-destructive"
  decision). Each write (`enqueue`, `markDone`, `setAttempts`,
  `setIdempotencyKey`) is a committed transaction; SQLite's WAL provides the
  durability.
- **Thread-safety matches `InMemoryOfflineQueue`.** All operations serialise on an
  internal mutex around the connection, so it is safe to share between the
  application's enqueue-on-failure write path and `SyncWorker`'s drain/replay read
  path ([offline.md](../spec/offline.md)'s "Ownership: who enqueues").

### A plain-file variant

For hosts that do not want a SQLite dependency, a second reference implementation
over an append-only NDJSON file (mirroring `FileActionLog`'s shape,
[ARCHITECTURE.md](../ARCHITECTURE.md)) is provided in the same opt-in spirit:

- `enqueue` appends a JSON line `{id, payload, idempotencyKey, attempts}`.
- `markDone` and `setAttempts` are recorded as tombstone/update lines and
  compacted on open (last-write-wins per id), so the file is self-healing across
  restarts and tolerates a torn trailing line the same way `FileActionLog` does.
- Same `IOfflineQueue` surface; the choice between SQLite and file is a host
  decision, not a contract difference.

### How it makes B1 work end to end

With this queue installed as the `IOfflineQueue` behind `SyncWorker`
([durable_queue.md](durable_queue.md)):

1. An item fails a replay → `SyncWorker` increments `item.attempts` and calls
   `setAttempts(id, attempts)` → the count is persisted.
2. The process restarts; `drain()` re-presents the item with a **fresh `id`** but
   the **persisted `attempts`**.
3. `SyncWorker` resumes from the stored count, so the 5-attempt budget is
   cumulative across restarts and the item genuinely dead-letters (invoking the
   `DeadLetterSink`) instead of retrying forever — closing the exact failure mode
   [offline.md](../spec/offline.md) documents.

## What this does not do

- **No new queue interface or semantics.** It implements the *existing*
  `IOfflineQueue` and the [durable_queue.md](durable_queue.md) hooks verbatim; it
  adds no method the interface does not already define. `drain`/`markDone`
  semantics are unchanged.
- **No dependency in the core.** The SQLite variant lives in an opt-in header and
  links SQLite only when the host includes it; the default `morph` build and
  `InMemoryOfflineQueue` are untouched and dependency-free.
- **No dedup enforcement beyond insert.** The unique index deduplicates a
  re-enqueue of the same key at write time, but replay-time at-most-once is still
  the consumer's job via `idempotencyKey` ([offline.md](../spec/offline.md) — "the
  queue only *stores* it"). The queue does not become an exactly-once engine.
- **Not the transactional outbox.** A model with its own store that needs the log
  and its state to commit atomically uses [outbox.md](outbox.md); this queue is
  the *offline write buffer*, a distinct concern.
- **No conflict resolution.** As [offline.md](../spec/offline.md) states, that
  lives in the model's `onBackendChanged()`, not the queue.

## Testing (planned)

- Enqueue items, destroy and re-open the `SqliteOfflineQueue` over the same file:
  `drain()` returns the same payloads and `idempotencyKey`s, with fresh `id`s and
  the persisted `attempts` (durability round-trip).
- A poison item driven through `SyncWorker` across a **simulated restart** (new
  `SyncWorker` over the same persisted queue) reaches 5 cumulative attempts and
  dead-letters — the [durable_queue.md](durable_queue.md) cross-restart test,
  now with a real store behind it.
- A crash between `drain()` and `markDone()` (kill after replay side effect,
  before `markDone`) re-presents the item on the next open; the replay function's
  idempotency prevents double-apply.
- Re-enqueue of the same non-empty `idempotencyKey` is deduplicated by the unique
  index; empty-key items are never deduplicated (parity with in-memory).
- The NDJSON file variant tolerates a torn trailing line on open (skips it,
  matching `FileActionLog`), and compacts tombstones/updates correctly.

## Cross-references

- [durable_queue.md](durable_queue.md) — the `QueueItem::attempts`, `setAttempts`
  write-back hook, and `DeadLetterSink` this queue concretely implements; the
  cross-restart dead-lettering it makes real.
- [offline.md](../spec/offline.md) — `IOfflineQueue`, `QueueItem`,
  `InMemoryOfflineQueue`, `enqueue`/`drain`/`markDone`/`setIdempotencyKey`, the
  non-destructive-`drain` crash-safety contract, `idempotencyKey` dedup, and the
  "only an in-memory queue ships" limitation this closes.
- [outbox.md](outbox.md) — the distinct transactional-outbox concern for
  store-backed models; both reuse the `idempotencyKey` dedup contract.
- [ARCHITECTURE.md](../ARCHITECTURE.md) — the anticipated "SQL-backed
  implementation ... UNIQUE constraint on the payload" and `FileActionLog`'s
  torn-trailing-line tolerance the NDJSON variant mirrors.
