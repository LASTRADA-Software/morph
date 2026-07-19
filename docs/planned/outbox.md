# Transactional outbox — journal + store atomicity (planned)

> **Status: planned — not yet implemented.** This spec extends
> [journal.md](../spec/journal/journal.md). It closes the "no transactional outbox" limitation:
> the action log and a model's own durable store commit as two independent
> writes and can diverge on a crash. See [todo.md](../todo.md).

## The gap

`morph::journal` records every loggable action to an `IActionLog` after the
action succeeds (`recordIfAttached`, see [journal.md](../spec/journal/journal.md)). A model that
*also* owns a durable store (a SQL database, a file) therefore performs **two
independent writes** per action:

1. `Model::execute` mutates and commits the model's own store.
2. The dispatcher runner appends a `LogEntry` to the action log.

Nothing ties these into one atomic unit. A crash *between* them leaves the two
out of sync:

- Crash after (1) before (2): the store advanced, the log did not — the audit
  trail is **behind** reality, and a replay-based reconstruction would miss the
  action.
- Crash after (2) before the store commit is durable: the log is **ahead** of the
  store — replay would re-apply an action the store never persisted.

`examples/bank` demonstrates exactly this: it writes SQLite and the audit log as
two separate steps. `journal.md` names it explicitly ("There is no outbox tying
the two into one atomic write; recovery must reconcile them out of band").

For an audit trail this is a correctness problem: the journal claims to be "the
source of truth for what happened," but it can silently disagree with the store
whose state it is supposed to explain.

## Goal

Provide a pattern (and the minimal framework seam it needs) so a model with its
own transactional store can record the `LogEntry` **in the same transaction** as
its state mutation — the classic *transactional outbox*. Either both commit or
neither does; recovery is deterministic, not "reconcile out of band."

This is opt-in: models with no durable store of their own, or that accept
at-least-once/reconcilable logging, keep the current fire-after-success behavior
unchanged.

## Design

### The core idea

Today the log append is driven by the *framework*, after `Model::execute`
returns. An outbox inverts control for stores that need atomicity: the *model*
writes the log entry into an **outbox table in its own store, inside its own
transaction**, and a separate relay moves committed outbox rows to the real
`IActionLog`.

```
Model::execute(action):
    BEGIN TXN (model's own store)
        mutate business tables
        INSERT into outbox(payload, result, entityKey, principal, ts, idempotencyKey)
    COMMIT TXN                    <-- one atomic write: state + intent-to-log

Relay (async, at-least-once):
    for each committed, unrelayed outbox row:
        actionLog.append(row -> LogEntry)   // durable sink (FileActionLog, etc.)
        mark row relayed (or delete)         // in the model's store
```

The store commit is the single source of truth. The relay is idempotent and
crash-safe: a crash before "mark relayed" re-relays the row, and the log sink
dedups on `LogEntry`/`idempotencyKey` (see below). No window exists where the
store and the *intent* to log disagree.

### The framework seam

Two small additions, both opt-in:

1. **Suppress the framework's automatic append for outbox models.** A model that
   manages its own outbox must be able to tell the dispatcher runner *not* to
   also append (which would double-log and re-introduce the two-write split). The
   cleanest seam is a per-action or per-holder opt-out:

   - Per-instance: `IModelHolder::attachActionLog` gains a mode, or a companion
     `setOutboxManaged(true)`, so `recordIfAttached` becomes a no-op for that
     instance (the model owns logging).
   - This reuses the existing attach seam rather than adding a new dispatch path;
     `hasActionLog()` stays the gate, plus an "outbox-managed" flag that means
     "attached, but the model relays it, don't auto-append."

2. **A relay helper** — `journal::OutboxRelay` — that a host wires to its store:

```cpp
// namespace morph::journal
struct OutboxRelay {
    /// Pull committed-but-unrelayed rows from the model's store.
    std::function<std::vector<LogEntry>()> drainOutbox;
    /// Mark rows relayed (by seq/idempotencyKey) in the model's store.
    std::function<void(std::span<const LogEntry>)> markRelayed;
    /// The durable audit sink rows are forwarded to.
    std::shared_ptr<IActionLog> sink;

    /// Move all committed outbox rows to `sink`, then mark them relayed.
    /// Idempotent and crash-safe: safe to call repeatedly; a crash between
    /// append and markRelayed re-relays (sink dedups on idempotencyKey).
    SyncResultLike relay();
};
```

The relay contains only the move-then-mark loop; the store-specific
`drainOutbox`/`markRelayed` are injected, exactly as `ReconnectCoordinator::Deps`
and `SyncWorker::ReplayFunction` inject their side effects. morph never touches
the model's database.

### Dedup ties into the existing idempotency key

`LogEntry` does not carry an idempotency key today, but `QueueItem` does
(`offline.md`) and the durable-queue spec generalises it. The outbox relay reuses
the **same** notion: each outbox row carries a stable `idempotencyKey`, the sink
records applied keys, and a re-relayed row (after a crash between append and
mark) is skipped. This is the same at-most-once mechanism the offline queue and
journal replay share — the outbox is a third producer into the same dedup
contract, not a new one.

> Prerequisite: this assumes `LogEntry` carries (or is extended to carry) a
> stable dedup key. If [durable_queue.md](durable_queue.md)'s idempotency work
> lands first, reuse it; otherwise the outbox spec adds the key to `LogEntry`.

### Relationship to `SessionLog`/`replay`

`SessionLog` and `replay()` (`journal.md`) already treat "state = initial +
ordered actions replayed." The outbox makes that identity *hold across crashes*
for a store-backed model: because the log can no longer be ahead of or behind the
store, a replay-based reconstruction agrees with the store's actual state. Undo
and checkpoint semantics are unchanged — they operate on the (now
crash-consistent) log.

## What this does *not* do

- **No database driver ships.** Like the durable `IOfflineQueue`, morph provides
  the *pattern and the relay seam*, not a SQL/outbox-table implementation. The
  `drainOutbox`/`markRelayed` callables are the host's, against its own store.
- **Not distributed transactions.** This is a single-store outbox (the model's
  own DB + its own outbox table in one local transaction). It does not do 2PC
  across the model store and a remote log service; the relay to a *remote* sink
  is deliberately at-least-once + dedup, not atomic.
- **Not required for non-store models.** A model with no durable state of its own
  has nothing to be atomic *with*; it keeps the automatic fire-after-success
  append. The outbox is only for models that own committed state the log must
  agree with.
- **Does not change the wire or the local/remote dispatch paths.** Recording
  still happens at the same two call sites (`journal.md`); the only change is that
  an outbox-managed instance suppresses the auto-append and relays instead.

## Testing (planned)

- A model writing state + outbox row in one (simulated) transaction, with a
  crash injected between the store commit and the relay: after restart the relay
  moves the row to the sink exactly once; store and log agree.
- A crash injected between store commit and outbox insert (i.e. the model's own
  transaction did *not* commit the outbox row): neither the state nor the log
  advanced — no divergence.
- Re-relay after a crash between `append` and `markRelayed`: the sink dedups on
  `idempotencyKey`, so the entry appears once.
- A non-outbox model still auto-appends after success (backward compatibility).
- `replay()` over an outbox-relayed log reconstructs state that matches the
  store.

## Cross-references

- [journal.md](../spec/journal/journal.md) — `LogEntry`, `IActionLog`, `recordIfAttached`, the
  two recording call sites, `SessionLog`/`replay`, and the "no transactional
  outbox" limitation this closes.
- [durable_queue.md](durable_queue.md) — the `idempotencyKey` dedup contract the
  outbox relay reuses; both are producers into the same at-most-once mechanism.
- [offline.md](../spec/offline/offline.md) — `QueueItem::idempotencyKey` (the original dedup
  token) and the injected-side-effect pattern (`ReconnectCoordinator::Deps`,
  `SyncWorker`) that `OutboxRelay` follows.
- [registry.md](../spec/core/registry.md) — `ActionDispatcher`'s runner and
  `IModelHolder::attachActionLog`/`recordIfAttached`, where the auto-append
  suppression opt-out lives.
- `examples/bank` — the concrete two-write divergence this pattern fixes.
