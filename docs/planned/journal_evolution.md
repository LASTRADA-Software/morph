# Journal persistence — format versioning, retention, replay across releases (planned)

> **Status: planned — not yet implemented.** This spec is the **data-at-rest**
> counterpart of [protocol_versioning.md](protocol_versioning.md): that spec
> governs peers exchanging envelopes *now*; this one governs a process reading
> NDJSON journal lines written months of releases ago. It extends
> [journal.md](../spec/journal.md) (`LogEntry`, `FileActionLog`, `replay()`)
> and closes two of its documented limitations — no format version on
> persisted lines, unbounded file growth. See [todo.md](../todo.md).

## The gap

- **No version stamp.** A `FileActionLog` line is a bare `toJson(LogEntry)`
  ([journal.md](../spec/journal.md)); nothing records which line format (or
  which era of action struct) wrote it. A reader confronted with an
  incompatible file cannot even *detect* it, let alone say what wrote it.
- **The reader is strict where the wire is lenient.** `journal::fromJson`
  decodes with glaze's default options — unknown keys are an **error** —
  unlike `wire::decode`, which explicitly reads with
  `error_on_unknown_keys = false` ([wire.md](../spec/wire.md)). Today that
  means the journal format cannot gain *any* new key without every older
  reader throwing `SerializationError` on the whole file: adding the version
  stamp itself would be a flag-day. Reader leniency has to land first.
- **Replay meets old data with no contract.**
  [protocol_versioning.md](protocol_versioning.md)'s additive-only policy and
  deprecation window are *deployment-scoped* (peers upgrade within a window),
  and its non-goals explicitly leave stored data out. But a journal outlives
  deployments: `journal::replay()` decodes recorded payloads with today's
  `ActionTraits<A>::fromJson`, so a field removed after its deprecation
  window still breaks replay of any retained journal that recorded it.
  Nothing states what must hold for old entries to stay replayable.
- **No retention story.** The file is append-only forever; `SessionLog`'s
  `checkpoint()` coalesces *before* the sink, but the durable file only
  accretes. There is no rotation seam, so a host cannot archive or expire
  history without hand-editing a live log file.

## Design

### 0. Reader leniency first (prerequisite, NEW)

`journal::fromJson` switches to the same explicit stance as the wire:
`glz::opts{.error_on_unknown_keys = false}`, for the same reason — a newer
writer may add a key an older reader does not know. The duplicate-key
caveat transfers verbatim ([wire.md](../spec/wire.md): last-wins, not a
security boundary). This must ship at least one release **before** any new
key is written, so downgraded or side-by-side readers never hit the flag-day;
the sequencing is the point of doing it now, ahead of need.

### 1. A line-format version stamp (NEW)

```cpp
// namespace morph::journal — NEW.
inline constexpr std::uint32_t kLogFormatVersion = 1;  // bumped on a breaking line-format change

struct LogEntry {
    // ... existing fields (seq, modelType, entityKey, actionType, payload,
    //     result, principal) unchanged ...
    std::uint32_t v = kLogFormatVersion;  // NEW: line-format version
};
```

- `toJson` stamps the current version on every new line. A legacy line has no
  `v` key and decodes with the member's default — i.e. legacy **is** v1,
  which is correct: v1 is today's shape.
- Read rule: `v <= kLogFormatVersion` decodes normally; `v` **greater** than
  the reader's version throws `SerializationError` ("written by a newer
  morph") — fail loud rather than guess at a shape this build has never seen.
  The existing positional torn-line rule is unchanged: a failing **last**
  line is still skipped with a warning whatever the failure's cause, an
  interior failure still throws ([journal.md](../spec/journal.md)).
- `kLogFormatVersion` bumps only on a *breaking* change to the line format;
  additive keys (tolerated by step 0) do not bump it — mirroring
  `kProtocolVersion`'s discipline
  ([protocol_versioning.md](protocol_versioning.md)). Both constants are
  [drift_guard.md](drift_guard.md) pinnables.

### 2. The data-at-rest contract (documented rule, NEW)

One sentence with teeth: **an action recorded in a retained journal must stay
decodable for as long as that journal is retained.** Consequences, extending
[protocol_versioning.md](protocol_versioning.md)'s additive-only policy from
deployment-window to retention-window:

- Fields of journal-recorded actions evolve **additive-only**; a new field
  must be optional-or-defaulted so old payloads (which lack it) decode into
  new structs. This already falls out of the wire policy — the journal makes
  its *duration* explicit.
- **Removing or retyping** a recorded action's field requires one of: every
  retained journal containing it has expired, or the host runs a migration
  pass (below). The wire's deprecation window is necessary but not
  sufficient.
- Replay and dispatch share one codec (`ActionTraits<A>::fromJson`), so there
  is exactly one compatibility surface to keep honest — no separate "archive
  format" to drift.

### 3. A rotation seam on `FileActionLog` (NEW)

```cpp
// namespace morph::journal — NEW on FileActionLog.
/// Seal the active file: flush + fsync, close, rename it to @p sealedPath,
/// and reopen a fresh, empty active file at the original path. Thread-safe
/// (same mutex as append/flush/entries); no line is ever split across files.
void rotate(const std::filesystem::path& sealedPath);
```

- `entries()` keeps reading the **active** file only — unchanged semantics.
  Sealed segments are immutable history the host archives or deletes per its
  retention policy; morph ships the seam, not the policy.
- Full-history reads and replay compose segments **oldest → newest, then the
  active file** — documented recipe, no new API. File order is already the
  cross-restart ordering authority (`seq` stays process-local, unchanged —
  [journal.md](../spec/journal.md)).
- Crash safety: the rename is a single atomic filesystem operation; a crash
  around it leaves either the old active file or the sealed file plus a
  recreated-empty active — never a split line, so the torn-line rule keeps
  applying per file.

### 4. The migration recipe (documented, deliberately not shipped)

Consistent with [protocol_versioning.md](protocol_versioning.md)'s "no
automatic migration of stored data": when retention forces a breaking change
through, the procedure is — `rotate()` to seal; transform the sealed segments
offline (host-owned mapping over the payload JSON, with `entries()` or plain
NDJSON tooling); write the result as new segments stamped with the current
`v`. morph guarantees the decode rules above and ships no transformer.

### Interplay

`SessionLog`'s `checkpoint()`/`undoLast()` are untouched (they operate before
the sink, in memory). The durability track's persisted payloads
([durable_queue.md](durable_queue.md),
[durable_offline_queue_impl.md](durable_offline_queue_impl.md),
[outbox.md](outbox.md)) carry action JSON with the same at-rest exposure —
their stores should adopt this contract's terms when they land.

## Non-goals

- **No shipped migration tool and no upgrade-on-read.** Reading never mutates
  history; transformation is an explicit, offline, host-owned pass.
- **No compaction of sealed history.** `checkpoint()` is the reducer, applied
  *before* entries become durable; once written, the audit trail is immutable
  ("entries are never removed by the framework",
  [journal.md](../spec/journal.md)).
- **No multi-process appenders.** The existing single-process constraint on
  `FileActionLog` stands; rotation does not change it.
- **No encryption or signing of segments.** At-rest protection of archived
  history is the host's storage concern.

## Testing (planned)

- Leniency (step 0): a line with an unknown extra key decodes; the whole-file
  regression corpus from before the change still decodes byte-identically.
- Stamp round-trip: new lines carry `v = kLogFormatVersion`; a legacy line
  (no `v`) decodes as v1; a mid-file line with `v = kLogFormatVersion + 1`
  throws `SerializationError`; the same line **last** in the file is skipped
  with a warning (positional rule preserved).
- `rotate()`: the active file is empty after, the sealed file decodes fully,
  concurrent `append` during rotation is safe, and replay over
  segments-then-active equals replay over the never-rotated file (state
  equality via the reconstructed holder).
- Additive evolution: a payload recorded before a new optional field existed
  replays into the new struct through `ActionTraits<A>::fromJson`.

## Cross-references

- [journal.md](../spec/journal.md) — `LogEntry`, `toJson`/`fromJson`,
  `FileActionLog` (NDJSON, fsync, torn-line rule, process-local `seq`),
  `replay()`, and the two limitations this closes.
- [wire.md](../spec/wire.md) — the `error_on_unknown_keys = false` precedent
  and duplicate-key caveat the journal reader adopts.
- [protocol_versioning.md](protocol_versioning.md) — the additive-only
  policy and deprecation window this extends to retention scope; the
  stored-data non-goal this spec is the answer to.
- [error_handling.md](../spec/error_handling.md) — `SerializationError`, the
  failure type all decode rules surface through.
- [durable_queue.md](durable_queue.md) /
  [durable_offline_queue_impl.md](durable_offline_queue_impl.md) /
  [outbox.md](outbox.md) — sibling persisted-payload stores this contract
  extends to.
- [api_stability.md](api_stability.md) — the compatibility surface that
  gains a retention-scoped clause.
- [drift_guard.md](drift_guard.md) — `kLogFormatVersion` as a pinned fact.
- [todo.md](../todo.md) — roadmap placement (§B durability & data-integrity).
