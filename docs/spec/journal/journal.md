# The `journal` subsystem — design

`morph::journal` is a durable, append-only record of every action executed
against a model instance. It is the audit trail and the raw material for
state reconstruction — the journal, not the live model, is the source of truth
for "what happened."

Three concerns live here, spread across three headers (`action_log.hpp`,
`file_action_log.hpp`, and `journal.hpp`):

1. **The log entry format** — `LogEntry`, a flat struct of what one action
   execution produced, plus `toJson`/`fromJson` for wire/file encoding.
2. **The storage interface** — `IActionLog`, plus three implementations:
   `InMemoryActionLog` (`action_log.hpp`), `FileActionLog` (`file_action_log.hpp`),
   and `SessionLog` (`journal.hpp`).
3. **The process-wide default** — `setActionLog`, `defaultActionLog`,
   `ScopedActionLog` (all in `action_log.hpp`), and how model instances
   auto-attach to it. `replay()` and `SessionLog::undoLast()` live in
   `journal.hpp` and depend on `ModelRegistryFactory`/`ActionDispatcher`.

For remote topologies, a fourth attachment path — `RemoteServer::LogProvider`
(declared in `remote.hpp`) — attaches an `IActionLog` to server-owned instances
by `contextKey`; see [Attaching a log to remote instances](#attaching-a-log-to-remote-instances).

## Contents

- [LogEntry — one recorded action execution](#logentry--one-recorded-action-execution)
- [Serialization](#serialization)
- [IActionLog — the storage interface](#iactionlog--the-storage-interface)
- [InMemoryActionLog](#inmemoryactionlog)
- [FileActionLog](#fileactionlog)
- [SessionLog](#sessionlog)
- [replay()](#replay)
- [Process-wide default log](#process-wide-default-log)
- [Attaching a log to remote instances](#attaching-a-log-to-remote-instances)
- [ScopedActionLog](#scopedactionlog)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Invariants](#invariants)
- [Failure modes / durability](#failure-modes--durability)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## LogEntry — one recorded action execution

`LogEntry` is produced automatically by
`morph::model::detail::IModelHolder::recordIfAttached` after every successful
loggable action. Application and model code never construct or append these
directly.

| Field | Type | Meaning |
|---|---|---|
| `seq` | `uint64_t` | Monotonic order assigned by the sink on `append()`. Callers pass `0`. |
| `modelType` | `std::string` | String type-id of the model (`ModelTraits<M>::typeId()`). |
| `entityKey` | `std::string` | Stable identity of the model instance (e.g. account id), stamped from `attachActionLog()`. Empty if none was set. |
| `actionType` | `std::string` | String type-id of the action (`ActionTraits<A>::typeId()`). |
| `payload` | `std::string` | JSON-encoded request (`ActionTraits<A>::toJson`). |
| `result` | `std::string` | JSON-encoded result (`ActionTraits<A>::resultToJson`), captured after successful execution. |
| `principal` | `std::string` | Auth principal from `morph::session::current()`, if any. Empty if unset. |
| `timestampMs` | `int64_t` | Wall-clock time, milliseconds since the Unix epoch. |

`LogEntry` is a plain aggregate — Glaze reflects it without a `glz::meta`
specialisation, the same automatic reflection `BRIDGE_REGISTER_ACTION` relies on.

## Serialization

### `toJson(LogEntry const&) -> std::string`

Encodes a `LogEntry` as JSON via Glaze. Throws `SerializationError` on failure
(not realistically reachable for a flat struct of strings/integers — see
`detail::throwOnGlazeError`).

### `fromJson(std::string_view) -> LogEntry`

Decodes JSON into a `LogEntry`. Throws `SerializationError` if the input is not
valid.

### `SerializationError`

`struct SerializationError : std::runtime_error` — thrown by `toJson`/`fromJson`
when (de)serialisation fails. Inherits `std::runtime_error`'s constructors.

### `detail::throwOnGlazeError`

Shared non-template helper used by both `toJson` and `fromJson` so their error
paths compile through the same branch. `fromJson`'s failure path is easy to
exercise (malformed JSON is everyday input); `toJson`'s is not — Glaze's
buffer-writer has no reachable failure mode for a flat struct like `LogEntry`.

## IActionLog — the storage interface

A pure-virtual interface for durable, append-only storage of action entries.
Entries are never removed by the framework — this is a permanent record, unlike
`morph::offline::IOfflineQueue` (whose `markDone()` deletes items once retried).

| Method | Signature | Purpose |
|---|---|---|
| `append` | `virtual void append(LogEntry)` | Appends an entry. Implementations assign `entry.seq`. |
| `flush` | `virtual void flush()` | Pushes buffered entries to the durable backend. No-op for sinks with nothing to buffer. |
| `entries` | `[[nodiscard]] virtual std::vector<LogEntry> entries(std::string_view entityKey = {}) const` | Returns recorded entries in append order, optionally filtered by `entityKey`. |

## InMemoryActionLog

Thread-safe in-memory implementation of `IActionLog`. Suitable for testing and
applications that do not need cross-process durability. Mirrors
`morph::offline::InMemoryOfflineQueue`'s shape.

- `append`: assigns a monotonically increasing `seq`, pushes to an internal
  vector under a mutex.
- `flush`: no-op.
- `entries`: returns a snapshot under a mutex; filters by `entityKey` if
  non-empty.

## FileActionLog

Append-only, newline-delimited-JSON `IActionLog` backed by a local file. Each
entry is one `toJson`-encoded line. `flush()` flushes the C stdio buffer and
then issues a real `fsync` (POSIX `fsync` / Windows `_commit`), so a crash
immediately after `flush()` returns cannot lose data.

Open (creating if necessary) via `FileActionLog(std::filesystem::path)`.
Throws `std::runtime_error` if the file cannot be opened. Closes the file in
the destructor. Copy and move are deleted.

**Process-local `seq`.** `seq` is assigned fresh per process instance — it does
not resume from the highest `seq` already on disk. Entries remain correctly
ordered on disk (append-only), but `seq` alone is not a cross-restart unique
key; use `entries()`' natural file order for that.

**Thread safety.** All public methods are thread-safe (guarded by an internal
mutex). Safe from multiple threads within one process; not safe for multiple
processes to append to the same path concurrently.

`entries()` re-reads the file from disk and decodes every line. Reads whatever
is currently on disk, including anything written but not yet `flush()`ed if the
platform's stdio buffering has already handed it to the OS. `flush()` first for
a guaranteed-durable view. Strictly-empty lines are skipped before decoding
(the check is `line.empty()`; a whitespace-only line is *not* treated as blank
and is handed to `fromJson`).

**Torn-write tolerance.** A crash between `append()`'s `fwrite` and the next
`flush()` can leave a truncated final line (bytes written, never completed).
`entries()` tolerates it by position, not by cause: if decoding fails on the
**last** non-empty line *for any reason* (truncation is the expected one, but the
catch is over `std::exception` generally), it skips that line, logs a warning via
`morph::log::logWarn` (naming the path and the parse error), and returns
everything before it. A decode failure on any line **mid-file** is treated as
genuine corruption and
the `fromJson` exception is re-thrown — the log is not silently truncated at an
interior tear. So a single trailing torn record is recoverable; interior
damage is fatal and surfaced to the caller.

## SessionLog

Full-fidelity, in-memory log of one model instance's executed actions. Attach
directly via `IModelHolder::attachActionLog` — every successfully executed
loggable action is appended here in order, regardless of any
`ActionLogPolicy::coalesce` setting. This full history is the raw material for
`undoLast()`.

`checkpoint()` is where coalescing happens: entries accumulated since the last
checkpoint are reduced by `(modelType, entityKey, actionType)` — keeping only
the latest occurrence where the action's policy says `coalesce == true`, keeping
every occurrence otherwise — and only that reduced set reaches the durable sink.

All public methods are thread-safe (guarded by an internal mutex).

| Method | Signature | Purpose |
|---|---|---|
| `append` | `void append(LogEntry)` | Always full fidelity — nothing is coalesced or dropped here. |
| `flush` | `void flush()` | No-op — `checkpoint()` is the real commit point. |
| `entries` | `std::vector<LogEntry> entries(std::string_view entityKey = {})` | Full history (or one entity's slice) in append order. |
| `undoLast` | `std::unique_ptr<IModelHolder> undoLast(modelTypeId, registry, dispatcher)` | Drops the most recent entry and replays the remainder against a **fresh, detached** model instance, reconstructing pre-undo state. Returns that new holder — the caller must install/use it (it does not mutate any live instance). No-op (returns a freshly created, un-replayed holder) if the log is empty. |
| `checkpoint` | `void checkpoint(IActionLog& durableSink, ActionDispatcher& dispatcher = defaultDispatcher())` | Coalesces entries since the last checkpoint and forwards the reduced set to `durableSink`; then flushes it. Advances the internal checkpoint watermark (the highest committed entry `seq`) *before* forwarding anything, so it stays advanced regardless of whether the subsequent `durableSink.append()`/`durableSink.flush()` throws — this makes the batch **at-most-once / forward-only** (a throwing sink drops it permanently), NOT the at-least-once its `IOfflineQueue` shape might suggest. The whole body is serialized against other checkpoints (see [Concurrency and ordering](#concurrency-and-ordering)). See [Failure modes / durability](#failure-modes--durability). No-op if nothing has been appended since the last checkpoint. |

### `undoLast()`

No inverse/undo operations are needed on the action types themselves — this
reuses `replay()` over a shorter prefix of the same log. Takes `modelTypeId`
plus optional `registry` and `dispatcher` — `registry` and `dispatcher`
default to the process-level singletons. Dropping the last entry rewinds only
`SessionLog`'s in-memory history; it does **not** touch the checkpoint
watermark. The watermark is a committed-`seq` threshold, not a position in the
mutable `_all` vector (see [checkpoint()](#checkpoint) and the
[undo / checkpoint / coalescing interaction](#undo--checkpoint--coalescing-interaction)),
so removing a tail entry cannot lower it. Because the durable sink is
append-only, `undoLast()` cannot remove an entry a prior `checkpoint()` already
forwarded: undoing a checkpointed entry leaves it durable while the
reconstructed history diverges from it. Durably reversing a checkpointed action
requires a **compensating action**, not `undoLast()`.

`undoLast()` returns a **fresh, detached** holder (created by `replay()`, which
immediately detaches the auto-attached default log — see [replay()](#replay)),
not a mutation of any live model instance. The caller is responsible for
installing or otherwise using the returned holder; the pre-undo instance, if
any, is left untouched. Like `replay()`, `undoLast()` assumes the entries it
walks all belong to one model instance — a `SessionLog` is intended to be
attached per instance, so this holds by construction; if a single `SessionLog`
is ever shared across instances, filter by `entityKey`/model type before
reconstructing (see [Invariants](#invariants)).

### `checkpoint()`

Coalescing is `(modelType, entityKey, actionType)`-keyed: the latest occurrence
overwrites earlier ones (in place, preserving first-seen position) wherever the
dispatcher says the action coalesces (unknown/unregistered `(modelType, actionType)`
pairs default to *not* coalescing, so every such entry is kept); every other
entry is kept as-is. The checkpoint watermark is advanced to the current tail
*before* any entry is forwarded, so it stays advanced regardless of whether
`durableSink.append()` or `durableSink.flush()` then throws.

**This is at-most-once, forward-only — not the at-least-once its `IOfflineQueue`
shape suggests.** Because the watermark advances *before* forwarding, a
`durableSink` that throws on `append()`/`flush()` causes that batch to be
**dropped permanently**: the next `checkpoint()` starts from the already-advanced
watermark and never re-sends it. A checkpoint is a forward-only commit point,
not a transaction that will be retried. If a durable sink can fail transiently,
the caller must treat a throwing `checkpoint()` as data loss for that batch, not
as a retryable no-op. See [Failure modes / durability](#failure-modes--durability).

#### Concurrency and ordering

`checkpoint()` runs its entire body under a dedicated forwarding mutex, distinct
from the mutex that guards `append()`/`entries()`/the history. Two effects:

- **Serialized forwarding.** Concurrent checkpoints never interleave. The
  checkpoint that selects its pending batch and advances the watermark first is
  also the one that forwards to `durableSink` first, and its `durableSink.append()`
  calls all complete before the next checkpoint's begin. Entries therefore reach
  the durable sink in strictly nondecreasing append order — the append-order
  identity the sink relies on holds even under concurrent checkpointing. Without
  this, two checkpoints could each grab a disjoint pending slice under the
  history mutex, release it, then race on the unlocked forward phase, letting
  batches land at the sink out of order.
- **Appends still progress during I/O.** The history mutex is held only briefly —
  to select the pending entries and advance the watermark — and is released
  *before* the sink's `append()`/`flush()` runs. A slow durable sink does not
  block ordinary `append()` calls. The lock order is always forwarding-mutex
  then history-mutex, and no path holds the history mutex while acquiring the
  forwarding mutex, so there is no deadlock.

#### undo / checkpoint / coalescing interaction

The checkpoint watermark is the **highest committed entry `seq`**, not an index
into the mutable `_all` history. This is what makes checkpointing coherent with
both coalescing and `undoLast()`:

- **Coalescing.** `checkpoint()` selects every entry whose `seq` exceeds the
  watermark, coalesces that set, forwards it, and advances the watermark to the
  highest `seq` in the batch. `seq` is assigned once at `append()` and never
  reused, so it is a stable identity even though coalescing forwards *fewer*
  entries than it consumes. An index-based watermark cannot express "these
  specific entries are committed" once coalescing collapses several source
  entries into one.
- **Undo.** `undoLast()` pops the tail of `_all` and never moves the watermark.
  Popping a not-yet-checkpointed tail entry simply means a later `checkpoint()`
  no longer sees it (its `seq` is gone from the history). Popping an
  already-checkpointed entry does not lower the watermark, so a later
  `checkpoint()` never re-forwards a coalesced-away, already-committed entry —
  durable state is monotonic and forward-only. A fresh action appended after an
  undo gets a new, higher `seq` and is forwarded exactly once; it is never
  confused with a removed entry, because identity is by `seq`, not position.
- **Undo of a checkpointed entry is a divergence, not a rollback.** Since the
  durable sink is append-only, undoing an entry a checkpoint already forwarded
  leaves that entry durable while the reconstructed in-memory history no longer
  contains it. `SessionLog` does not attempt to reconcile the two; an
  application that must durably reverse a checkpointed action records a
  compensating action instead.

## replay()

Reconstructs model state by replaying entries, in order, against a freshly
created model instance. Builds on the same `ModelRegistryFactory` /
`ActionDispatcher` machinery `RemoteServer` uses — no separate replay engine.

```cpp
std::unique_ptr<IModelHolder> replay(
    std::string_view modelTypeId,
    const std::vector<LogEntry>& entries,
    ModelRegistryFactory& registry = defaultRegistry(),
    ActionDispatcher& dispatcher = defaultDispatcher());
```

Throws `std::runtime_error` if `modelTypeId` or any entry's action type is
unregistered.

**Reconstruction does not pollute the live audit trail.** `registry.create(...)`
(via `ModelFactory::create`) auto-attaches the process-wide default action log
to the new holder, exactly as for any ordinary model instance. `replay()`
therefore calls `holder->attachActionLog(nullptr, {})` *immediately* after
creating the holder and *before* dispatching any entry, detaching that default.
Without this detach, each replayed dispatch would re-record into the live sink —
corrupting the very audit trail being read from (and, since replay re-runs the
recorded actions, doubling every entry on every reconstruction). As a result,
both `replay()` and `SessionLog::undoLast()` reconstruct state in isolation:
the replayed actions are **not** written back into `defaultActionLog()`. Undo
and reconstruction are read-only with respect to the audit trail.

**Single-instance precondition.** `replay()` creates *one* holder of
`modelTypeId` and dispatches every entry against it in order. The caller is
responsible for passing entries that all belong to that one model instance —
typically obtained by filtering a log with `entries(entityKey)` and by matching
`modelType`. Mixing entries from several instances (or several model types) into
one `replay()` call replays them all onto a single object and produces a
meaningless state. This is a precondition, not something `replay()` validates.

## Process-wide default log

Every model instance created via `ModelFactory::create<Model>()` — every model
registered the ordinary way, whether the active backend is local or remote —
automatically gets the default log attached (with an empty `entityKey`) from
that point on.

### `setActionLog(std::shared_ptr<IActionLog>)`

Installs a log as the process-wide default. Pass `nullptr` to stop
auto-attaching (existing instances keep whatever they already have). Thread-safe.

### `defaultActionLog() -> std::shared_ptr<IActionLog>`

Returns the currently installed default, or `nullptr` if none has been set.
Thread-safe.

The state is a function-local static (`detail::defaultActionLogState()`), not a
namespace-scope global, so it is safe regardless of translation-unit init order.

## Attaching a log to remote instances

The process-wide default and `IModelHolder::attachActionLog` cover local mode,
but for a remote/simulated-remote topology the client never owns the model
instance — `RemoteServer` creates and holds it — so a log attached via the
client's `HandlerBinding` factory is never populated (that factory is not even
invoked server-side). `RemoteServer::LogProvider` closes that gap. It is
declared in `remote.hpp`, not the journal headers, but it is the fourth way a
`journal::IActionLog` gets attached to an instance and is documented here for
completeness.

```cpp
// RemoteServer::LogProvider
using LogProvider = std::function<
    std::shared_ptr<morph::journal::IActionLog>(
        std::string_view modelType, std::string_view contextKey)>;

void RemoteServer::setLogProvider(LogProvider provider);   // thread-safe
```

- **Where `contextKey` comes from.** The client sets
  `HandlerBinding::contextKey`; `SimulatedRemoteBackend::registerModelWithContext`
  (the default `Backend` override drops it) carries it in the `register`
  wire envelope as `wire::Envelope::contextKey` (`wire::makeRegister(typeId,
  contextKey)`), defaulting to empty.
- **When the provider is consulted.** On each `register` envelope whose
  `contextKey` is **non-empty**, `RemoteServer` invokes the provider
  synchronously with `(typeId, contextKey)`. An empty `contextKey` skips the
  provider entirely — the instance is registered with no log. A provider that
  returns `nullptr` also attaches no log; otherwise the returned sink is attached
  via `holder->attachActionLog(log, contextKey)`, so `contextKey` becomes the
  entry `entityKey`.
- **Installing / removing.** `setLogProvider(nullptr)` removes a previously
  installed provider (subsequent registrations get no log). The provider slot is
  guarded by its own mutex, and the provider is copied out under that lock before
  being called, so `setLogProvider` is safe to call concurrently with request
  handling.

This is the only recording path for a genuinely remote topology: recording is
server-side, keyed by the per-instance identity the client chose. See
`bridge.md` and `backend.md` for the surrounding wiring.

## ScopedActionLog

RAII helper that installs a default action log for its lifetime and restores the
previous one on destruction. Mirrors `morph::log::ScopedLoggerOverride`. Intended
for tests and for temporarily redirecting auto-attached logging.

```cpp
{
    morph::journal::ScopedActionLog guard{std::make_shared<morph::journal::InMemoryActionLog>()};
    // models created here auto-attach guard's log
}   // previous default restored
```

Copy and move are deleted.

## API reference

All symbols live in `namespace morph::journal`.

### Log entry and serialization

| Symbol | Kind | Signature / Notes |
|---|---|---|
| `LogEntry` | struct | Flat aggregate: `seq`, `modelType`, `entityKey`, `actionType`, `payload`, `result`, `principal`, `timestampMs`. Glaze-reflected (no `glz::meta`). |
| `toJson` | free function | `std::string toJson(const LogEntry&)` — encodes as JSON. Throws `SerializationError`. |
| `fromJson` | free function | `LogEntry fromJson(std::string_view)` — decodes from JSON. Throws `SerializationError`. |
| `SerializationError` | struct | `: std::runtime_error`. Thrown by `toJson`/`fromJson`. |
| `detail::throwOnGlazeError` | inline function | `void throwOnGlazeError(const glz::error_ctx&, std::string_view)` — shared error path for `toJson`/`fromJson`. |

### IActionLog and implementations

| Symbol | Kind | Notes |
|---|---|---|
| `IActionLog` | abstract struct | `virtual ~IActionLog() = default`; `append(LogEntry)`, `flush()`, `entries(entityKey)`. |
| `InMemoryActionLog` | class | `: IActionLog`. Thread-safe `std::vector`-backed. `flush()` no-op. |
| `FileActionLog` | class | `: IActionLog`. Newline-delimited JSON, fsync on `flush()`. `explicit FileActionLog(std::filesystem::path)`. Copy/move deleted. |
| `SessionLog` | class | `: IActionLog`. Full-fidelity in-memory log + `undoLast()` + `checkpoint()`. |

### Process-wide default

| Symbol | Kind | Notes |
|---|---|---|
| `setActionLog` | free function | `void setActionLog(std::shared_ptr<IActionLog>)`. Thread-safe. |
| `defaultActionLog` | free function | `[[nodiscard]] std::shared_ptr<IActionLog> defaultActionLog()`. Thread-safe. |
| `ScopedActionLog` | class | RAII: saves previous default, restores on destruction. `explicit ScopedActionLog(std::shared_ptr<IActionLog>)`. Copy/move deleted. |

The remote attachment path lives outside this namespace: `morph::backend::RemoteServer::LogProvider`
(a `std::function<std::shared_ptr<IActionLog>(std::string_view modelType, std::string_view contextKey)>`)
and `RemoteServer::setLogProvider(LogProvider)`, declared in `remote.hpp`. See
[Attaching a log to remote instances](#attaching-a-log-to-remote-instances).

### State reconstruction

| Symbol | Kind | Notes |
|---|---|---|
| `replay` | free function | `std::unique_ptr<IModelHolder> replay(modelTypeId, entries, registry, dispatcher)`. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| `LogEntry` is a plain aggregate | **No `glz::meta`** | Same automatic reflection `BRIDGE_REGISTER_ACTION` uses; no manual schema maintenance. |
| Error path sharing | **`detail::throwOnGlazeError` for both `toJson`/`fromJson`** | `fromJson`'s failure is easy to test (malformed input); `toJson`'s is structurally unreachable for `LogEntry`. Routing both through one non-template function means the same compiled branch covers both, so `toJson`'s error path is exercised by `fromJson`'s tests. |
| Entries are never removed | **Append-only, no deletion API** | Permanent audit trail — unlike `IOfflineQueue` whose `markDone()` deletes retried items. |
| Default log is a function-local static | **`detail::defaultActionLogState()` returns a `pair<mutex, shared_ptr>`** | Safe regardless of translation-unit init order, unlike a namespace-scope global. |
| `SessionLog::checkpoint` advances the watermark *before* forwarding | **At-most-once / forward-only** | A checkpoint is a forward-only commit point, not a transaction to retry: the watermark advances first, so a throwing durable sink drops that batch permanently. (`IOfflineQueue`'s retry semantics do *not* carry over — the shared shape is superficial.) |
| Checkpoint watermark is a committed-`seq` threshold, not an `_all` index | **Track committed state by entry identity** | `seq` is assigned once and never reused, so it stays a valid commit marker even as coalescing forwards fewer entries than it consumes and as `undoLast()` pops tail entries. A raw index into the mutable `_all` vector cannot: it silently shifts meaning when entries are removed, which is the root of the undo/coalescing incoherence this replaces. |
| `checkpoint()` forwarding is serialized by a dedicated mutex | **Ordered, non-blocking forwarding** | Holding a forwarding mutex across the whole body keeps concurrent checkpoints from racing on the unlocked forward phase and landing batches at the sink out of append order. It is a *separate* mutex from the history lock (held only briefly for slice + watermark), so a slow sink never blocks `append()`. |
| `SessionLog::undoLast` uses `replay()` | **No inverse operations on actions** | Replays the shorter prefix against a fresh model — no per-action undo logic needed. Undo rewinds only in-memory history and never moves the watermark; reversing a checkpointed action durably needs a compensating action. |
| `FileActionLog::seq` is process-local | **Fresh per process, not resumed from disk** | `seq` is a monotonic order key within one process instance, not a cross-restart durable identifier. On-disk order is append order; `entries()` returns in that order regardless of `seq` gaps. |
| `FileActionLog` uses C stdio + `fsync` | **`fopen`/`fwrite`/`fflush`/`fsync`** | `fwrite` is buffered; `flush()` calls `fflush` then `fsync` (or `_commit` on Windows) for real durability. POSIX `write`/`fsync` would bypass stdio buffering entirely; C stdio gives buffering by default with explicit flush control. |
| `FileActionLog::entries` tolerates a torn trailing line | **Skip + warn on the last line only; re-throw mid-file** | A crash between `append`'s `fwrite` and the next flush can truncate the final line. Skipping it keeps the log readable after a crash; re-throwing on interior damage refuses to silently hide real corruption. |

## Invariants

These hold for every sink and are relied on by `replay()`/`undoLast()`:

- **Only successful, loggable actions are recorded.** A `LogEntry` is produced
  by `IModelHolder::recordIfAttached` *after* an action executes successfully.
  Actions that throw (business-rule failures), drafts rejected by a validator
  (`ActionValidator::validate`), and any action registered `Loggable::No`
  (typically pure queries like `GetAccount`/`ListAccounts`) never appear in the
  log. The log is a record of committed facts, not of attempts.
- **`result` reflects post-execution state.** `payload` is the request JSON;
  `result` is captured only after success, so replaying `payload` re-derives an
  equivalent `result` for a deterministic model.
- **`seq` is sink-local and re-stamped on every forward.** Each sink's
  `append()` overwrites `entry.seq` with its own `++_nextSeq`, ignoring any
  incoming value. When `SessionLog::checkpoint()` forwards entries to a durable
  sink, that sink re-stamps them again. `seq` is therefore an ordering key
  *within one sink instance in one process run* — it is **not** a stable,
  cross-sink or cross-restart identifier. Use `entries()`' natural append order
  for identity/ordering across sinks; do not persist or compare raw `seq`
  values as keys. (`FileActionLog::seq` is likewise fresh per process — it does
  not resume from the highest `seq` on disk.)
- **Reconstruction is single-instance.** `replay()` and `undoLast()` expect
  entries already filtered to a single model instance — filter by `entityKey`
  (via `entries(entityKey)`) and by `modelType` first. Feeding mixed instances
  into one `replay()` call is undefined at the domain level (all entries dispatch
  onto one holder).
- **`undoLast()` yields a detached holder the caller must install.** It does not
  mutate a live instance; the reconstructed pre-undo state lives only in the
  returned holder (whose default log is detached, so using it records nothing
  into the live trail).
- **The checkpoint watermark is monotonic and identity-based.** It is the
  highest committed entry `seq`, never an index into `_all`, and it never
  decreases. `checkpoint()` forwards exactly the entries with `seq` above it and
  then raises it; `undoLast()` never lowers it. Consequently a coalesced-away,
  already-committed entry is never re-forwarded, and durable state advances
  forward-only regardless of interleaved undos and checkpoints.
- **Concurrent checkpoints forward in append order.** `checkpoint()` serializes
  its whole body under a dedicated forwarding mutex, so no two checkpoints
  interleave their `append()` calls at the durable sink; entries reach the sink
  in strictly nondecreasing append order even under concurrent checkpointing.

## Failure modes / durability

The durability contract is narrower than the "durable audit trail" framing
alone implies. Understand these before relying on the log for recovery:

- **`checkpoint()` is at-most-once / forward-only.** The watermark advances to
  the current tail *before* any entry is forwarded to the durable sink. If the
  sink's `append()` or `flush()` throws, the batch is **lost permanently** — the
  next `checkpoint()` resumes past it and never retries. Despite sharing the
  `IOfflineQueue` shape, this is the opposite of `IOfflineQueue`'s at-least-once
  retry-until-`markDone` behavior. Callers that need at-least-once must layer
  their own retry around a sink whose `append`/`flush` are idempotent, or treat
  a throwing `checkpoint()` as an explicit data-loss event.
- **Entries are durable only after `checkpoint()` *and* `flush()`.** A
  `SessionLog`'s history is pure in-memory until `checkpoint()` forwards it to a
  durable sink and that sink's `flush()` returns. For `FileActionLog`, `flush()`
  is what fsyncs; before it, entries may sit in stdio/OS buffers. A crash before
  `checkpoint()` loses the entire uncheckpointed session's history.
- **No transactional link to the model's own store.** The log and a model's own
  durable store (e.g. the SQLite in `examples/bank`) commit as two independent
  steps. A crash can leave the store committed but the log missing the
  corresponding entries (uncheckpointed), or — with a separately-flushed
  file — the log ahead of the store. There is no outbox tying the two into one
  atomic write; recovery must reconcile them out of band.
- **`FileActionLog` torn-write recovery is trailing-only.** A single truncated
  *final* line (from a crash mid-`append`) is skipped with a warning; any
  malformed *interior* line makes `entries()` throw. So the file self-heals from
  the one crash shape it is designed for, and refuses to silently drop data for
  any other.
- **Single-writer file assumption.** `FileActionLog` is thread-safe within one
  process but not safe for concurrent appenders across processes on the same
  path; interleaved writes from two processes can corrupt lines.

## Limitations

Honest boundaries of the current design:

- **Replay re-executes actions.** `replay()`/`undoLast()` reconstruct state by
  *re-running* each recorded action through the dispatcher — not by replaying a
  captured state diff. For a model whose actions have external side effects
  (SQL writes, network calls, RNG, clock reads), replaying re-triggers those
  side effects. Undo and reconstruction are therefore **exact only for pure,
  deterministic, in-memory models**. A model backed by an external store will,
  on replay, attempt to re-apply its writes.
- **No transactional outbox.** As above, nothing ties a log entry to the
  commit of the model's own store. Divergence between the two is possible and
  is the application's problem to detect and reconcile.
- **Unbounded in-memory growth; O(n²) repeated undo.** `SessionLog` retains full
  uncoalesced history for the lifetime of the instance — memory grows without
  bound. Each `undoLast()` replays the entire remaining prefix from scratch, so
  undoing the last *k* actions one at a time is O(n·k) ≈ O(n²) in the history
  length. There is no incremental/snapshot fast-path.
- **No schema version or migration path for persisted NDJSON.** `FileActionLog`
  writes `LogEntry` as Glaze-reflected JSON with no embedded schema version. A
  change to `LogEntry`'s shape has no defined migration story for files written
  by an earlier build; on-disk compatibility rests entirely on the struct
  staying additive-and-compatible under Glaze.

## Cross-references

- **`registry.md`** — `ModelRegistryFactory`/`ActionDispatcher` and
  `ModelFactory::create`, which auto-attach the default log and which `replay()`
  reuses for dispatch. Also the `ActionDispatcher::coalesce` lookup driving
  `checkpoint()`.
- **`bridge.md`** — the two (mutually exclusive) recording call sites
  (`Bridge::executeVia`'s `localOp` for local mode; the `RemoteServer` dispatch
  path for remote/Qt), and `HandlerBinding::contextKey`/`RemoteServer::setLogProvider`
  for per-instance identity.
- **`backend.md`** — how local vs. remote topology decides which recording site
  is live, and why recording is automatically server-side wherever a client/server
  split exists.
- **`error_handling.md`** — `SerializationError` and the failure/validator-rejection
  paths that explain *why* unsuccessful actions never reach the log.