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
- [Line-format version (`v`)](#line-format-version-v)
- [Payload schema fingerprint](#payload-schema-fingerprint)
- [Data-at-rest contract](#data-at-rest-contract)
- [IActionLog — the storage interface](#iactionlog--the-storage-interface)
- [InMemoryActionLog](#inmemoryactionlog)
- [FileActionLog](#fileactionlog)
- [Rotation and retention](#rotation-and-retention)
- [SessionLog](#sessionlog)
- [replay()](#replay)
- [Causal links and replay-mode signaling](#causal-links-and-replay-mode-signaling)
- [Process-wide default log](#process-wide-default-log)
- [Attaching a log to remote instances](#attaching-a-log-to-remote-instances)
- [ScopedActionLog](#scopedactionlog)
- [Transactional outbox (opt-in)](#transactional-outbox-opt-in)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Invariants](#invariants)
- [Failure modes / durability](#failure-modes--durability)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## LogEntry — one recorded action execution

`LogEntry` is produced automatically by
`morph::model::detail::IModelHolder::recordIfAttached` after every loggable
action attempt — **both** a successful `Model::execute` and one that throws
(a validator rejection, a rejected write). Application and model code never
construct or append these directly.

| Field | Type | Meaning |
|---|---|---|
| `seq` | `uint64_t` | Monotonic order assigned by the sink on `append()`. Callers pass `0`. |
| `modelType` | `std::string` | String type-id of the model (`ModelTraits<M>::typeId()`). |
| `entityKey` | `std::string` | Stable identity of the model instance (e.g. account id), stamped from `attachActionLog()`. Empty if none was set. |
| `actionType` | `std::string` | String type-id of the action (`ActionTraits<A>::typeId()`). |
| `payload` | `std::string` | JSON-encoded request (`ActionTraits<A>::toJson`). Always present, regardless of `outcome`. |
| `schema` | `std::string` | Structural fingerprint of the payload's JSON shape at the moment `payload` was encoded (`morph::model::payloadFingerprint<A>()`). Stamped automatically at both execution sites. **Empty means unstamped** — written before this field existed, appended directly by application code, or produced by an action whose `ActionTraits` is hand-written. See [Payload schema fingerprint](#payload-schema-fingerprint). |
| `result` | `std::string` | JSON-encoded result (`ActionTraits<A>::resultToJson`). Populated when `outcome == Outcome::Succeeded`; empty when `Failed`. |
| `outcome` | `Outcome` | `Succeeded` or `Failed`. Defaults to `Succeeded` so a pre-existing on-disk entry (written before this field existed) decodes unchanged — an absent key is indistinguishable from an explicit `Succeeded`. Serialises as the string `"Succeeded"`/`"Failed"` via a `glz::meta<Outcome>` specialisation (the one exception to "`LogEntry` needs no `glz::meta`" below). |
| `error` | `std::string` | `std::exception::what()` from the exception that rejected the action. Empty unless `outcome == Outcome::Failed`. |
| `principal` | `std::string` | Auth principal from `morph::session::current()`, if any. Empty if unset. |
| `timestampMs` | `int64_t` | Wall-clock time, milliseconds since the Unix epoch. |
| `idempotencyKey` | `std::string` | Optional dedup token for outbox-relayed entries. Empty by default; ordinary auto-appended entries never set it. Mirrors `morph::offline::QueueItem::idempotencyKey`'s exact contract. See [Transactional outbox (opt-in)](#transactional-outbox-opt-in). |
| `v` | `std::uint32_t` | Line-format version this entry was written at. Defaults to `kLogFormatVersion`. See [Line-format version (`v`)](#line-format-version-v). |
| `causalParentId` | `std::string` | Identity of the "trigger" entry that caused this entry to be recorded, or empty (the sentinel) if none. Set by application code that journals a cascaded mutation (e.g. an automation rule reacting to one recorded action by executing a further one). See [Causal links and replay-mode signaling](#causal-links-and-replay-mode-signaling). |

`LogEntry` is a plain aggregate — Glaze reflects it without a `glz::meta`
specialisation of its own, the same automatic reflection `BRIDGE_REGISTER_ACTION`
relies on. Both real `Model::execute()` call sites — `ActionDispatcher::registerAction`'s
runner (server/remote topologies) and `Bridge::executeVia`'s `localOp`
(`LocalBackend`) — wrap the call in a `try`/`catch (const std::exception&)`:
the `catch` records a `Failed` entry (`error = exc.what()`, `result` empty)
and rethrows unchanged, so the caller's error handling is unaffected — only
the journal gains an entry it previously lacked.

## Serialization

### `toJson(LogEntry const&) -> std::string`

Encodes a `LogEntry` as JSON via Glaze, writing with `detail::EscapingWriteOpts`
so a raw ASCII control byte (0x00-0x1F) in `entityKey`/`payload`/`error`/
`principal`/`idempotencyKey` round-trips through `fromJson` instead of
producing invalid JSON — or, when the same string also holds an escaped `\`/`"`,
silently corrupted JSON (glaze's chunked writer path rewrites the control byte
as two `0x00` bytes in that case). Mirrors `morph::wire::detail::EscapingWriteOpts`
(`core/wire.hpp`) exactly; duplicated locally rather than shared so this header
stays free of a `core/` dependency. Throws `SerializationError` on failure (not
realistically reachable for a flat struct of strings/integers — see
`detail::throwOnGlazeError`).

### `fromJson(std::string_view) -> LogEntry`

Decodes JSON into a `LogEntry`. Reads leniently —
`glz::read<glz::opts{.error_on_unknown_keys = false}>`, the same stance
`wire::decode` takes ([wire.md](../core/wire.md)) — so an unknown/extra key
(an additive field from a newer writer) is ignored rather than rejected; the
same duplicate-key caveat as `wire::decode` applies (last-wins, not a security
boundary). Syntactically malformed JSON still throws `SerializationError`.
After a successful decode, `fromJson` also enforces the [line-format version
rule](#line-format-version-v): a decoded `v` greater than this build's
`kLogFormatVersion` throws `SerializationError` even though the JSON itself
parsed cleanly.

### `SerializationError`

`struct SerializationError : std::runtime_error` — thrown by `toJson`/`fromJson`
when (de)serialisation fails. Inherits `std::runtime_error`'s constructors.

### `detail::throwOnGlazeError`

Shared non-template helper used by both `toJson` and `fromJson` so their error
paths compile through the same branch. `fromJson`'s failure path is easy to
exercise (malformed JSON is everyday input); `toJson`'s is not — Glaze's
buffer-writer has no reachable failure mode for a flat struct like `LogEntry`.

## Line-format version (`v`)

```cpp
inline constexpr std::uint32_t kLogFormatVersion = 1;  // bumped only on a breaking line-format change
```

`LogEntry::v` (`std::uint32_t`, default `kLogFormatVersion`) records which line
format wrote a persisted entry.

- Every freshly-constructed `LogEntry` — which is every entry `toJson` ever
  writes for a new action execution — carries `v = kLogFormatVersion` via its
  default member initializer; no separate stamping step runs inside `toJson`.
- A **legacy line** (written before `v` existed) has no `v` key; `fromJson`'s
  leniency means that is just an absent-key case like any other, and the
  member's default applies: the entry decodes with `v == kLogFormatVersion`
  (currently `1`). This is intentional, not an approximation — `v = 1` **is**
  today's shape; `kLogFormatVersion` merely gives it a name.
- **Read rule.** `v <= kLogFormatVersion` decodes normally. `v` **greater**
  than the reader's `kLogFormatVersion` throws `SerializationError` — a build
  refuses to guess at a line format it has never seen, rather than silently
  misreading it. This check runs *after* leniency's unknown-key tolerance, so
  the two rules compose: an unrelated new key is ignored, but a `v` bump is
  always fatal to an older reader.
- **The existing torn-line rule is unchanged.** `FileActionLog::entries()`
  still tolerates a decode failure — of any cause, including a too-new `v` —
  only on the file's last line, skipping it with a warning; the same failure
  mid-file is re-thrown. See [FileActionLog](#fileactionlog). Note that
  `FileActionLog`'s constructor itself scans `entries()` to rebuild its
  `idempotencyKey` dedup set, so a too-new `v` on an interior line surfaces as
  a thrown `SerializationError` from **construction**, not only from a later
  explicit `entries()` call — the same behavior any other interior corruption
  already has (see [Sink-side dedup](#sink-side-dedup)).
- `kLogFormatVersion` bumps only on a **breaking** change to the line format;
  additive keys (tolerated by leniency) do not bump it — the same discipline
  applied to protocol-version bumps elsewhere in the wire layer.

## Payload schema fingerprint

`LogEntry::v` versions the **line format**. `LogEntry::schema` versions the
**payload** — the shape of the action struct whose JSON is in `payload`.

The problem it solves is the one the [Data-at-rest
contract](#data-at-rest-contract) below states as a rule but had no way to
enforce. Replay decodes a stored payload with the *current* action struct,
through `ActionTraits<A>::fromJson`'s lenient reader. Rename a field and both
halves of the rename are invisible: the old key is an unknown key, silently
ignored; the new one is an absent key, silently default-constructed. The entry
decodes, the model reconstructs, `replay()` returns normally — and reports a
state that was never recorded. Nothing in the entry said which shape wrote it,
so nothing *could* notice. For rungs 5 and 6, whose definition of done is
"every state X was ever in is reconstructible from the journal alone", that
is the disqualifying case: not a reconstruction that fails, but one that
succeeds and is wrong.

### The decision

**Detect the violation; refuse rather than guess; provide a seam for the
change that has to happen anyway.**

- Every recorded entry carries a fingerprint of the payload shape that wrote
  it (`morph::model::payloadFingerprint<A>()`, `core/payload_schema.hpp`).
- `replay()` compares it against the fingerprint *this* build computes for the
  same action, and **throws `SchemaMismatchError`** when they differ.
- An application can register a migration for a `(actionType, fromSchema)`
  pair, which rewrites the recorded payload JSON in memory before dispatch.
  The stored journal is never rewritten: reading never mutates history.

This does not replace the additive-only rule below — it makes the rule
checkable. The rule stays the recommendation; the fingerprint is what happens
when the recommendation was not followed.

### The fingerprint

`payloadFingerprint<A>()` returns `"<scheme>:<16 hex digits>"` — 18 bytes,
computed once per type per process, stamped by value on each entry.

It is the FNV-1a digest of a **shape rendering** (`payloadShapeString<A>()`),
which is deliberately portable: every tag comes from a `std::` type trait, from
Glaze's reflected key strings, or from a name spelled in this repository's own
sources, never from a compiler-spelled type name. Two builds of the same
sources on different compilers, standard libraries, or platforms therefore
agree, which a `glz::name_v`-derived fingerprint would not — and a journal that
only its own compiler can read is not a durable record.

| C++ type | Rendering |
|---|---|
| `bool` | `b` |
| `char` | `c` |
| signed / unsigned integral | `i`/`u` + `sizeof` (e.g. `i4`) |
| enumeration | `e` + `sizeof` of the underlying type |
| floating point | `f` + `sizeof` |
| string-like | `s` |
| `std::optional<T>` | `?` + T's rendering |
| map | `{` key `>` mapped `}` |
| other range | `[` element `]` |
| reflected object | `(` key-sorted `key:shape` list `)` |
| declares a `PayloadShapeTag` | `x{` name `}`, or `x{` name `:` inner shape `}` |
| anything else | `x` |

So `struct { std::string state; std::int32_t count; }` renders as
`(count:i4,state:s)`, and renaming `state` to `stateCode` renders as
`(count:i4,stateCode:s)` — a different digest.

### Custom-codec types name themselves

A type carrying its own `glz::meta` has no reflected members to decompose, so
it would render as the bare `x` and be indistinguishable from every other such
type. `morph::model::PayloadShapeTag<T>` (`core/payload_shape_tag.hpp`) is the
opt-in seam that closes that: a specialisation declares a short name, spelled
in these sources rather than derived from `glz::name_v`, and the rendering
becomes `x{`name`}`.

| Type | Rendering |
|---|---|
| `math::Rational` | `x{rational}` |
| `time::DateTime` | `x{datetime}` |
| `time::Timestamp` | `x{timestamp}` |
| `units::Quantity<U, Dec>` | `x{quantity.`*unit id*`.`*decimals*`}` |
| `util::Tagged<T, Tag>` | `x{tagged.`*tag*`:`*T's rendering*`}` |

Two of these are worth stating on their own terms:

- **`Quantity` is the only place its own retype can be caught.** Neither the
  unit nor the declared precision travels on the wire — a `Quantity` *is* its
  nullable `Rational` payload — so `Quantity<Gram>` and `Quantity<Litre>`
  produce byte-identical JSON. No decode, on any path, can tell them apart.
  The unit's id comes from `UnitTraits<E>::meta(U).id`, an author-declared
  ascii identifier already part of the protocol vocabulary.
- **`Tagged` renders its wrapped type inside the tag**, because it is a
  transparent wrapper: its JSON simply *is* `T`'s, so hiding `T` behind the
  wrapper's name would lose a real difference (`Tagged<std::string, "acct">`
  versus `Tagged<std::int64_t, "acct">` genuinely changes the recorded bytes).
  The tag text, like the unit id, never travels either.

A specialisation must be visible wherever `payloadShape` is instantiated for
that type, or two translation units would render one payload two ways. In
practice that is automatic: a payload struct with a `Rational` member is only
complete where `morph/util/rational.hpp` has been included, and that header
carries the specialisation.

**Scheme 2.** Rendering a declared name where scheme 1 rendered `x` changes the
fingerprint of every payload with such a member, so
`kPayloadFingerprintScheme` is `2`. That is precisely what the prefix exists
for: an entry stamped `1:…` read by a scheme-2 build reports a mismatch whose
two sides are legibly the product of different *algorithms*, not of different
payloads. Such an entry is handled the same way as any other mismatch — a
migration registered for its `(actionType, fromSchema)` pair, a build whose
fingerprint matches, or a surfaced failure.

**Members are sorted, so reordering a struct is not a schema change.** JSON
objects are unordered and the decode matches by name, so moving a field
changes nothing about which bytes decode where; an order-sensitive fingerprint
would turn a cosmetic edit into a replay break for every retained journal.

### What the fingerprint does not catch

Stated plainly, because the guarantee is only as good as its boundary:

- **A retype between two custom-codec types that have declared no name.** The
  seam above is opt-in, so it is incomplete by construction: a type with its
  own `glz::meta` and no `PayloadShapeTag` specialisation still renders as the
  opaque `x`, and swapping two such types for one another is still invisible.
  Every custom-codec type morph itself ships has declared one; a new one added
  later starts out undeclared. (A custom-codec type swapped for a plain one
  *is* caught either way: `x` versus `s`.) The alternative to declared names
  was a `glz::name_v`-derived tag, which is compiler-dependent; a journal
  readable only by the compiler that wrote it is the worse failure.
- **A retype that changes nothing about the JSON shape** at a nesting depth
  past `detail::kPayloadShapeMaxDepth` (8), where recursion stops and `x` is
  emitted.
- **Anything about an action with a hand-written `ActionTraits`.** The
  fingerprint describes the *reflected* shape, which is what the
  macro-generated codecs read and write. A hand-written codec may map its
  struct to entirely different JSON, so no fingerprint is derived for it and
  its entries are recorded unstamped. Such a specialisation opts in by
  defining its own `static const std::string& payloadSchema()`.
- **Anything an application journals by hand.** `LogEntry::schema` is stamped
  by `ActionDispatcher::registerAction`'s runner and `Bridge::executeVia`'s
  `localOp` — the two sites that execute an action. Code that constructs a
  `LogEntry` itself and appends it to a sink stamps nothing unless it sets the
  field, and its entries replay unverified.

### Unstamped entries — what happens to journals already written

An entry written before this field existed has no `schema` key. Under
`fromJson`'s leniency that is just an absent key, so it decodes with the
member's default: the empty string. Empty is therefore not "verified as
unchanged" — it is **unverifiable**, and no check can be performed on it at
all.

`replay()`'s `UnstampedPayloadPolicy` parameter decides what to do:

| Value | Behaviour |
|---|---|
| `Replay` (default) | Replay it, exactly as every build before this check did. |
| `Refuse` | Throw `SchemaMismatchError` on the first unstamped entry. |

`Replay` is the default because it is the only choice that keeps existing
journals replayable at all: refusing them would make an upgrade a data-loss
event for every retained journal, which is this feature's own failure mode
inverted. Choosing it accepts the *pre-existing* silent-default risk for
*pre-existing* entries and grants no leniency to anything written after the
stamp exists. **This feature is forward-looking by construction** — it protects
entries written by a build that has it, and it cannot retroactively protect
one that was written without it. A caller whose correctness claim is "this
reconstruction is faithful" passes `Refuse` and accepts that pre-fingerprint
entries are outside what it can claim.

### `SchemaMismatchError` — the signal, and who handles it

```cpp
struct SchemaMismatchError : std::runtime_error;  // journal.hpp
```

Thrown out of `replay()` (and therefore out of `SessionLog::undoLast()`,
though that path cannot reach it — see below). The message names the
`modelType/actionType` pair and both fingerprints, so the failure is
actionable without a debugger.

It throws rather than warning or flagging because there is no partially-correct
reconstruction to hand back: the decode gives no signal about how much of the
entry survived it, and a `replay()` that returned a suspect holder alongside a
warning would put the burden of noticing on exactly the code path that
demonstrably did not notice for as long as this defect existed.

The handler is **the caller** — application-level reconstruction code, not the
framework. Its three answers are: register a migration, replay with a build
whose fingerprint matches, or surface the failure. There is deliberately no
fourth.

`SessionLog::undoLast()` replays with the default settings and cannot trip the
gate in practice: the entries it replays were appended by the same process, so
their fingerprints are this build's by construction. Undo is not a
cross-version path.

### Migrations

```cpp
class PayloadMigrationRegistry {
    using Migration = std::function<std::string(std::string_view)>;
    void add(std::string_view actionType, std::string_view fromSchema, Migration);
    const Migration* find(std::string_view actionType, std::string_view fromSchema) const;
    void clear();
    std::size_t size() const noexcept;
};
PayloadMigrationRegistry& defaultPayloadMigrations();
```

A migration is a pure function over the recorded payload JSON: given the bytes
the older build wrote, return the bytes this build's `fromJson` should see. It
is applied in memory, per replayed entry; the sink is untouched. Not
thread-safe (matching `ActionDispatcher`) — register during start-up, before
any `replay()` runs. `replay()` takes the registry as a parameter, so a test or
a one-off reconstruction can pass its own instead of mutating the process-wide
one.

This is what makes "refuse" a livable contract rather than a one-way door: the
breaking change stays possible, it just has to be *written down as code* before
the journal that needs it can be read again.

### Alternatives considered

| Option | Why not chosen |
|---|---|
| **Strict decode on the journal path** (`error_on_unknown_keys = true`) | Catches the rename, but also rejects an *additive* field — the one evolution the [Data-at-rest contract](#data-at-rest-contract) explicitly permits. It would turn every journal written by a newer build into an error for an older reader, and it still cannot see a field that was renamed *away* (the key is simply absent). Cheap, but it breaks the contract it was meant to defend. |
| **Freeze payloads by contract only** (lint / conformance corpus) | This is already the published contract, and its enforcement is the gap. A lint sees one repository at one commit; the journal outlives the deployment that wrote it, and the rename may be years and several builds away from the reader. Useful as a second layer, not as the answer. |
| **Warn and reconstruct anyway** | The failure mode being fixed is *confident wrongness*. Handing back a suspect holder plus a log line reproduces it with extra steps. |
| **Full per-action version numbers with a registered decoder per version** | Strictly more expressive, and strictly more machinery: an author must remember to bump the version, which is the same discipline the additive-only rule already asks for and does not get. A fingerprint is derived, so it cannot be forgotten. Migrations recover the expressiveness where it is actually needed. |

### Relationship to the wire path (#207)

The identical leniency exists on the live wire path, and is **not** addressed
here. It is a different decision with a different answer: `docs/spec/core/wire.md`
publishes an "Action-evolution policy" whose first bullet is additive-only
*within* a deployment window, and the handshake (`kProtocolVersion`) is its
designed defence. The journal's scope is retention, not deployment — a journal
can outlive every peer that ever wrote to it — which is why the two paths get
different mechanisms. See issue #207.

## Data-at-rest contract

One rule with teeth: **an action recorded in a retained journal must stay
decodable for as long as that journal is retained.** This extends the wire
layer's additive-only evolution policy from *deployment* scope (peers upgrade
within a window) to *retention* scope — a journal can outlive every deployment
that ever wrote to it:

- Fields of journal-recorded actions evolve **additive-only** — a new field
  must be optional-or-defaulted so an old payload (recorded before the field
  existed) still decodes into the new struct.
- **Removing or retyping** a recorded action's field requires either every
  retained journal containing it to have expired, or an explicit migration
  pass (below) run first.
- Replay and dispatch share one codec — `ActionTraits<A>::fromJson` — so there
  is exactly one compatibility surface to keep honest; there is no separate
  "archive format" that can drift from the live one.
- **The rule is now checked, not merely stated.** Every recorded entry carries
  a fingerprint of the payload shape that wrote it, and `replay()` refuses an
  entry whose fingerprint disagrees with this build's. Before that, a violation
  of the two rules above produced a confident, wrong reconstruction rather than
  an error. See [Payload schema fingerprint](#payload-schema-fingerprint) —
  including what it cannot see, and what happens to entries already written
  without one.

### The migration recipe (not shipped)

When retention forces a breaking change through despite the above: call
[`rotate()`](#rotation-and-retention) to seal the active file; transform the
sealed segments offline (a host-owned mapping over the recorded payload JSON,
using `entries()` or plain NDJSON tooling); write the result as new segments
stamped with the current `v`. morph guarantees the decode rules above and
ships no transformer — reading never mutates history, and there is no
upgrade-on-read.

For the in-memory alternative — leaving the sealed segments exactly as
recorded and adapting each payload as it is replayed — see
[Migrations](#migrations). That seam does not rewrite anything either; it is
the same "reading never mutates history" rule, applied to a rename that has
already happened.

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

Open (creating if necessary) via `FileActionLog(std::filesystem::path, morph::core::FileIoOps = {})`.
The second parameter is a test-only fault-injection seam (`morph/core/
file_io_ops.hpp`) — the raw `fwrite`/`fflush`/`fsync`/`fopen`/file-open/
`resize_file` calls this class makes, as an injectable strategy defaulting to
the real syscalls, letting a test force the failure branches that otherwise
need a real OS-level I/O error to reach. A normal caller never passes one.
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

**Torn-write repair (on open).** A crash between `append()`'s `fwrite` and the
next `flush()` can leave a truncated final line (bytes written, never
completed). The constructor **truncates** the file to the last newline before
opening it for append. Discarding those bytes is safe by construction: every
complete record is written newline-terminated in a single `fwrite`, so whatever
follows the final newline can only be an incomplete record — never a whole one.

Tolerating the torn line without removing it was not enough. The file is opened
`"a"`, so the next `append()` began writing at the exact byte the truncated JSON
stopped at, with no separating newline: the two merged into one line that
swallowed the new entry. A *further* append then pushed that merged line out of
trailing position, at which point `entries()` threw — and because the
constructor itself calls `entries()`, the journal became permanently unopenable.
`FileOfflineQueue` heals the same damage during `compact()`; this is
`FileActionLog`'s equivalent.

**Torn-write tolerance (on read).** `entries()` additionally tolerates a
malformed final line by position, not by cause: if decoding fails on the
**last** non-empty line *for any reason* (the catch is over `std::exception`
generally), it skips that line, logs a warning via `morph::log::logWarn`
(naming the path and the parse error), and returns everything before it. A
decode failure on any line **mid-file** is treated as genuine corruption and
the `fromJson` exception is re-thrown — the log is not silently truncated at an
interior tear, and the repair above never removes a complete record either. So a
single trailing torn record is recoverable; interior damage is fatal and
surfaced to the caller.

**I/O failures are raised, never swallowed.** `append()` throws on a short
write; `flush()` throws if either `fflush` or the `fsync`/`_commit` fails;
`rotate()` throws if its pre-rotation flush fails, before anything is closed or
renamed. `IActionLog::flush()` returns `void`, so throwing is the only channel
available — and callers depend on it: `OutboxRelay::relay()` calls
`markRelayed()` immediately after `flush()`, and a silently-failed flush would
record rows as relayed in the model's own store while nothing reached the
durable sink, dropping them from the outbox *and* from the log with no error
anywhere.

**Dedup keys follow durability.** An entry's `idempotencyKey` is only recorded
as *seen* once a `flush()` confirms it reached the disk; keys written since the
last successful flush are held separately and discarded if that flush fails, so
a retry writes them again instead of being deduplicated away. A duplicated audit
row is recoverable; a dropped one is not.

**After a failed `rotate()` reopen.** If `rotate()` renames successfully but
cannot reopen the active path, it throws with no file open. `append()`,
`flush()` and a further `rotate()` then all throw with that diagnosis rather
than dereferencing a null handle, and destruction stays safe.

See [Rotation and retention](#rotation-and-retention) for `rotate()`, the seam
a host uses to seal and archive segments of this file.

## Rotation and retention

```cpp
void rotate(const std::filesystem::path& sealedPath);
```

Seals the active file and reopens a fresh, empty one at the same path — the
seam a host uses to implement its own retention policy (archive or delete
sealed segments on whatever schedule or size trigger it chooses; morph ships
the seam, not the policy).

- **What it does.** Flushes (`fflush` + `fsync`/`_commit`) and closes the
  current active file, renames it to `sealedPath`, then reopens a fresh, empty
  active file at the original path. Thread-safe — guarded by the same mutex as
  `append()`/`flush()`/`entries()`, so no in-flight `append()` call is ever
  split across the sealed and the new active file.
- **`entries()` is unchanged.** It keeps reading only the (now-empty, then
  regrowing) active file. Sealed segments are immutable history the host reads
  directly — e.g. by opening its own `FileActionLog` on the sealed path, or
  with plain NDJSON tooling.
- **Full-history reads and replay compose segments oldest → newest, then the
  active file** — a documented recipe, not a new API: concatenate `entries()`
  from each sealed segment in seal order, then the active file's `entries()`,
  and hand the result to [`replay()`](#replay). File order is already the
  cross-restart ordering authority (`seq` stays process-local, unchanged).
- **Crash safety.** The rename is a single atomic filesystem operation. A
  crash before it completes leaves the pre-rotation active file exactly as it
  was — as if `rotate()` had never been called. A crash after leaves the
  sealed file plus a freshly recreated, empty active file. Either way no line
  is ever torn across the two files, so the existing torn-line rule
  ([FileActionLog](#fileactionlog)) keeps applying independently per file.
- **A failed rename does not lose data.** If renaming to `sealedPath` fails
  (e.g. its directory does not exist), `rotate()` reopens the *original*
  active file in place — still holding every entry recorded before the call —
  and throws `std::runtime_error`. The log stays fully usable; the rotation
  simply did not happen.
- **Not `rotate()`'s job.** Choosing *when* to rotate (by size, by time, on a
  host-defined "archive now" action) and what happens to a sealed segment
  afterward (compress, ship, delete) are entirely the host's call — morph
  supplies only the seal-and-reopen primitive. See the [Data-at-rest
  contract](#data-at-rest-contract) for what a sealed segment's contents must
  keep decoding as.

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
    ActionDispatcher& dispatcher = defaultDispatcher(),
    const PayloadMigrationRegistry& migrations = defaultPayloadMigrations(),
    UnstampedPayloadPolicy unstamped = UnstampedPayloadPolicy::Replay);
```

Throws `std::runtime_error` if `modelTypeId` or any entry's action type is
unregistered, and `SchemaMismatchError` if an entry's payload fingerprint
disagrees with this build's (see below).

**`Failed` entries are skipped, not replayed.** A rejected/thrown action never
mutated model state, so there is nothing to reconstruct from it — and
re-dispatching it would likely throw the very same exception again, aborting
reconstruction. `replay()` filters out every entry with `outcome ==
Outcome::Failed` before dispatching; `Succeeded` entries dispatch exactly as
before.

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

**Every entry is checked against this build's payload shape before it is
dispatched.** Equal fingerprints dispatch unchanged; a mismatch consults
`migrations` and otherwise throws `SchemaMismatchError`; an unstamped entry is
governed by `unstamped`. An entry naming an *unregistered* action is not
rejected here — `schemaFor` returns empty for it and it falls through to
`dispatch()`'s "unknown action", the more precise of the two diagnostics. See
[Payload schema fingerprint](#payload-schema-fingerprint).

**`replay()` signals replay mode to executing code for its whole dispatch
loop.** See [Causal links and replay-mode signaling](#causal-links-and-replay-mode-signaling)
below.

## Causal links and replay-mode signaling

A cascaded mutation — one client action that causes further model mutations,
e.g. an automation rule reacting to "task moved to Done" by executing its own
further action — needs two things from the journal that a plain, uncascaded
action does not: a durable link back to what caused it, and a way for the
mutation that *produced* the cascade to avoid re-producing it a second time
when the trigger is replayed. Both are framework primitives, not app-specific
code; the first real consumer is `examples/kanban`'s automation-rules engine
(design spec `docs/superpowers/specs/2026-08-16-kanban-rung4-design.md` §9),
but neither piece is kanban-specific.

### `LogEntry::causalParentId`

A cascaded entry's `causalParentId` is set to the *triggering* entry's own
stable identity, so a reader (an activity-stream view, a replay-aware rules
engine) can recover "what caused this" without guessing from adjacency or
timing. Empty (the sentinel) means "not caused by another entry" — the
overwhelming majority of entries, including every entry recorded today, since
nothing in this codebase journals a cascade yet.

**Must not be a `LogEntry::seq` value.** `seq` is sink-local and re-stamped by
every sink's `append()` — and again by `SessionLog::checkpoint()` when
forwarding to a durable sink (see [Invariants](#invariants)) — so it is an
ordering key within one sink instance in one process run, not a stable,
cross-sink or cross-restart identifier. Application code that journals a
cascade must mint its own opaque/UUID-style identity for the trigger entry at
the point the trigger is created, independent of whatever `seq` any sink later
assigns it, and reuse that same identity as every cascaded entry's
`causalParentId`. `morph::journal` does not mint this identity itself — there
is no framework-side "trigger id" concept beyond the field that carries it;
the scheme for generating and threading it through is entirely the
application's (or, for kanban, the rules engine's) responsibility.

**Additive, per the [data-at-rest contract](#data-at-rest-contract).**
`causalParentId` is optional/defaulted exactly like `idempotencyKey` and every
other evolutionarily-added `LogEntry` field: an old payload recorded before
this field existed has no such key, and `fromJson`'s lenient decode falls back
to the empty default, so a pre-existing journal keeps decoding unchanged. This
does **not** bump `kLogFormatVersion` — the version bump is reserved for
*breaking* changes to the line format, and an additive, defaulted key is by
definition not one (see [Line-format version (`v`)](#line-format-version-v)).

### Replay-mode signaling: `isReplaying()`

`replay()` re-applies every recorded entry — trigger and cascade alike — in
their original recorded order. Without a way to tell "this dispatch is a
replay" apart from an ordinary live dispatch, a rules engine evaluating rules
against the replayed trigger would fire again and re-produce the cascade —
double-applying a mutation that is *also* being replayed from its own recorded
(cascade) entry. `morph::journal::isReplaying()` is the signal that lets
executing model/rule code tell the two cases apart:

```cpp
namespace morph::journal {
[[nodiscard]] bool isReplaying() noexcept;
}
```

Returns `true` while the calling thread is inside `replay()`'s dispatch loop,
`false` otherwise (including for every ordinary, non-replayed dispatch). A
rules engine (or any other model code that reacts to its own actions) checks
this before evaluating a rule; suppressing that evaluation during replay is
the actual mechanism that keeps a cascaded action's replay convergent — the
cascade's own recorded entry supplies the mutation, and rule evaluation
contributes nothing a second time.

**Mechanism: a thread-local flag plus an RAII scope guard**, the same shape
`morph::session::detail::tlsCurrent()`/`ScopedContext` already use to thread a
per-call `Context` through dispatch (`session.hpp`) — a thread-local slot
(`detail::tlsIsReplaying()`) and an RAII guard (`detail::ScopedReplayFlag`)
that sets it `true` on construction and restores the previous value on
destruction. `replay()` installs a `ScopedReplayFlag` immediately before its
dispatch loop, so the flag reads `true` for every entry that loop dispatches
and is restored to its prior value (`false`, for any ordinary top-level
caller) once `replay()` returns — it never leaks into dispatches that happen
after `replay()` completes. Nesting is well-defined for the same reason
`ScopedContext` is: a `replay()` call that itself triggers a nested `replay()`
leaves the flag `true` for the whole nested extent and restores the outer
call's value when the inner guard is destroyed.

**Why a thread-local, not a dispatcher parameter.** Threading a "replay mode"
boolean through `ActionDispatcher::dispatch(...)` and every `Model::execute`
signature would touch every registered action in the codebase, breaking the
existing `Model::execute(const Action&)` calling convention `BRIDGE_REGISTER_ACTION`
relies on. A thread-local, read via a free function, is additive: existing
`Model::execute` overloads compile and behave unchanged, and only code that
explicitly calls `isReplaying()` (the rules engine) observes anything new —
the same reasoning `session::current()` already established for `Context`.

**Scope: signals replay, not identity.** `isReplaying()` says nothing about
*which* entry is being replayed or *which* model instance — a rule reading it
combines it with the dispatched action's own fields (available inside
`Model::execute` the ordinary way) to decide what to suppress. There is no
`currentReplayEntry()` accessor; none of today's consumers need one.

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
- **Also reaches a model-level `attachActionLog`, if the model declares one.**
  `IModelHolder::attachActionLog` forwards to a protected virtual hook,
  `onActionLogAttached`, which `ModelHolder<Model>` overrides to call
  `Model::attachActionLog(log, contextKey)` when `Model` structurally satisfies
  `ModelLevelActionLogAttachable` (`morph/core/model.hpp`) — the same
  "detect the hook structurally, forward only if present" shape
  `onBackendChanged()`/`BackendChangedMixin` already use. This closes a real
  gap for a model that keeps its own model-level `IActionLog` reference to
  read back later (e.g. an activity-stream view over `entries(entityKey)`):
  before this hook existed, `holder->attachActionLog(...)` populated only the
  holder's own `_actionLog`/`_contextKey` (used by `recordIfAttached`'s
  auto-append), never a model instance's own state, since a registry-
  constructed model is always default-constructed and never otherwise
  touched. A model with no `attachActionLog` of its own is unaffected — the
  hook resolves to a no-op for it, exactly as before this existed. First
  exercised by `kanban::BoardModel` (rung 4).
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

## Transactional outbox (opt-in)

A model with its own transactional store (a SQL database, a file) can tie its
state commit and its journal entry into one atomic write instead of the
framework's default two independent writes (mutate-then-auto-append). This is
opt-in — a model that does nothing new keeps the fire-after-success behavior
described above unchanged.

### The pattern

1. **The model writes its own outbox row inside its own transaction.** Alongside
   its business-table mutation, the model inserts a row shaped like `LogEntry`
   (including a stable `idempotencyKey`) into an outbox table in its own store,
   in the same transaction as the mutation. Either both commit or neither does —
   morph does not participate in this transaction and never touches the model's
   database.
2. **The model calls `IModelHolder::setOutboxManaged(true)`** on its own holder
   (typically once, from the same factory closure that calls `attachActionLog`).
   This suppresses `recordIfAttached`'s automatic append for that instance:
   `hasActionLog()` keeps reporting whatever log is attached, but
   `recordIfAttached` becomes a no-op, so the framework's normal
   fire-after-success append does not also record the action (which would
   double-log it).
3. **A separate `journal::OutboxRelay` moves committed rows to the durable
   sink**, asynchronously, on whatever schedule the host chooses (a timer, an
   idle callback, a background thread).

### `IModelHolder::setOutboxManaged` / `isOutboxManaged`

```cpp
void setOutboxManaged(bool outboxManaged) noexcept;
[[nodiscard]] bool isOutboxManaged() const noexcept;
```

- `setOutboxManaged(true)` makes `recordIfAttached` a no-op for that instance,
  regardless of what `attachActionLog` attached. `hasActionLog()` is unaffected —
  it still reports whether a log is attached, independent of whether this
  instance auto-appends to it.
- Defaults to `false`: every instance auto-appends exactly as before unless a
  model explicitly opts in.

### Sink-side dedup

`InMemoryActionLog::append` and `FileActionLog::append` treat a **non-empty**
`idempotencyKey` as a dedup key: if an entry with the same key was already
appended, the second `append()` call is a silent no-op (no duplicate stored, no
`seq` consumed). An **empty** `idempotencyKey` never dedups — every entry with
no key is stored, exactly as before. `FileActionLog`'s dedup set is rebuilt from
the existing on-disk entries every time the file is opened (an O(n) scan of the
current contents, paid once per open, not per append), so the dedup survives a
process restart, not just repeated calls within one run — and, as a consequence,
opening an existing file whose *interior* is corrupted now throws
`SerializationError` from the constructor itself (a malformed *trailing* line is
still tolerated, matching `entries()`).

`SessionLog::append` deliberately does **not** dedup — its documented contract
is full fidelity, nothing coalesced or dropped (see `undoLast()`). Wire
`OutboxRelay::sink` to `InMemoryActionLog`, `FileActionLog`, or a custom
`IActionLog` that dedups on `idempotencyKey`; a `SessionLog` used as the relay's
sink gives no re-relay protection.

### `journal::OutboxRelay`

```cpp
struct NullSinkError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct OutboxRelayResult {
    std::size_t relayed = 0;
};

struct OutboxRelay {
    std::function<std::vector<LogEntry>()> drainOutbox;
    std::function<void(std::span<const LogEntry>)> markRelayed;
    std::shared_ptr<IActionLog> sink;

    OutboxRelayResult relay();
};
```

Declared in `outbox.hpp`. `drainOutbox` and `markRelayed` are injected against
the model's own store, exactly as `morph::offline::ReconnectCoordinator::Deps`
and `morph::offline::SyncWorker::ReplayFunction` inject their side effects —
morph never touches the model's database.

`relay()` drains every currently-unrelayed row, appends each to `sink`, flushes
`sink`, then marks the whole batch relayed via `markRelayed` in one call. A
no-op (`{.relayed = 0}`, `sink`/`markRelayed` untouched) if `drainOutbox()`
returns nothing.

**Crash safety.** Because `markRelayed` runs only after `sink`'s append *and*
flush complete, a crash between them leaves the row still "unrelayed" in the
model's store; the next `relay()` call re-drains and re-appends it, and the
sink's `idempotencyKey` dedup makes that re-append a no-op — the row is marked
relayed exactly once from the outbox table's perspective, and stored exactly
once in the sink. This is at-least-once-plus-dedup, not two-phase commit: `sink`
and the model's own store are never committed as a single distributed
transaction. A crash *before* the model's own outbox-row insert ever committed
means `drainOutbox()` never reports the row in the first place — neither the
state nor the log advanced, so there is no divergence to reconcile; this
guarantee comes from the host's own transaction, not from `OutboxRelay`.

Mirroring `ReconnectCoordinator::Deps`, a null `drainOutbox`/`markRelayed`/`sink`
is logged (via `morph::log::logError`) at the start of every `relay()` call but
does not reject the call by itself — invoking a null `drainOutbox`/`markRelayed`
still throws `std::bad_function_call` as usual (a null `std::function` call). A
null `sink` throws `NullSinkError`, a catchable `std::runtime_error`, once
`drainOutbox()` reports at least one row to relay — thrown before `sink` is
ever dereferenced, so no row is lost or marked relayed. `sink` being null is
not itself rejected when there is nothing to relay: an empty outbox is still a
no-op regardless of `sink`, exactly as it is when `sink` is real.

### What this does not do

- **No database driver ships.** `drainOutbox`/`markRelayed` are the host's
  callables against its own store; morph provides only the relay loop and the
  dedup-capable sinks.
- **Not distributed transactions.** The model's own store commit (business
  tables + outbox row) is one local transaction; the relay to `sink` is a
  separate, at-least-once-plus-dedup step, not 2PC.
- **`examples/bank` is unchanged.** It still demonstrates the two-write
  divergence this section closes for models that opt in; adopting the pattern
  there is not part of this change.

## API reference

All symbols live in `namespace morph::journal`.

### Log entry and serialization

| Symbol | Kind | Signature / Notes |
|---|---|---|
| `LogEntry` | struct | Flat aggregate: `seq`, `modelType`, `entityKey`, `actionType`, `payload`, `schema` (payload fingerprint, empty when unstamped), `result`, `outcome`, `error`, `principal`, `timestampMs`, `idempotencyKey`, `v` (line-format version, default `kLogFormatVersion`), `causalParentId` (identity of the triggering entry, empty by default). Glaze-reflected (no `glz::meta` of its own; `outcome`'s type `Outcome` has one). |
| `Outcome` | `enum class : std::uint8_t` | `Succeeded` (default) or `Failed`. Has a `glz::meta` specialisation so it (de)serialises as the string, not the underlying int. |
| `kLogFormatVersion` | `inline constexpr std::uint32_t` | Current line-format version (`1`). Bumped only on a breaking change to `LogEntry`'s shape. See [Line-format version (`v`)](#line-format-version-v). |
| `toJson` | free function | `std::string toJson(const LogEntry&)` — encodes as JSON with `detail::EscapingWriteOpts` (control-byte escaping). Throws `SerializationError`. |
| `fromJson` | free function | `LogEntry fromJson(std::string_view)` — decodes from JSON leniently (`error_on_unknown_keys = false`). Throws `SerializationError` on malformed JSON or if the decoded `v` exceeds `kLogFormatVersion`. |
| `SerializationError` | struct | `: std::runtime_error`. Thrown by `toJson`/`fromJson`. |
| `detail::throwOnGlazeError` | inline function | `void throwOnGlazeError(const glz::error_ctx&, std::string_view)` — shared error path for `toJson`/`fromJson`. |
| `SchemaMismatchError` | struct | `: std::runtime_error`. Thrown by `replay()` on a payload-fingerprint mismatch with no migration, or on an unstamped entry under `UnstampedPayloadPolicy::Refuse`. Message names the `modelType/actionType` pair and both fingerprints. |
| `UnstampedPayloadPolicy` | `enum class : std::uint8_t` | `Replay` (default) or `Refuse` — what `replay()` does with an entry carrying no fingerprint. |
| `PayloadMigrationRegistry` | class | `(actionType, fromSchema) -> std::function<std::string(std::string_view)>`. `add`/`find`/`clear`/`size`. Not thread-safe; populate at start-up. |
| `defaultPayloadMigrations` | free function | `[[nodiscard]] PayloadMigrationRegistry& defaultPayloadMigrations()` — the process-level registry `replay()` uses by default. |

### IActionLog and implementations

| Symbol | Kind | Notes |
|---|---|---|
| `IActionLog` | abstract struct | `virtual ~IActionLog() = default`; `append(LogEntry)`, `flush()`, `entries(entityKey)`. |
| `InMemoryActionLog` | class | `: IActionLog`. Thread-safe `std::vector`-backed. `flush()` no-op. |
| `FileActionLog` | class | `: IActionLog`. Newline-delimited JSON, fsync on `flush()`. `explicit FileActionLog(std::filesystem::path, morph::core::FileIoOps = {})` — the `FileIoOps` is a test-only fault-injection seam, see above. `void rotate(const std::filesystem::path& sealedPath)` seals the active file and reopens a fresh one — see [Rotation and retention](#rotation-and-retention). Copy/move deleted. |
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
| `replay` | free function | `std::unique_ptr<IModelHolder> replay(modelTypeId, entries, registry, dispatcher, migrations, unstamped)`. Verifies each entry's payload fingerprint before dispatching it. Sets `isReplaying()` to `true` for its dispatch loop — see below. |
| `isReplaying` | free function | `[[nodiscard]] bool isReplaying() noexcept` — `true` while the calling thread is inside `replay()`'s dispatch loop, `false` otherwise. See [Causal links and replay-mode signaling](#causal-links-and-replay-mode-signaling). |
| `detail::tlsIsReplaying` | inline function | `bool& tlsIsReplaying()` — thread-local slot backing `isReplaying()`. Not part of the public API; installed/restored only by `detail::ScopedReplayFlag`. |
| `detail::ScopedReplayFlag` | class | RAII: sets the thread-local replay flag `true`, restores the previous value on destruction. Copy/move deleted. |

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
| `InMemoryActionLog`/`FileActionLog` dedup on `idempotencyKey` | **Non-empty key only; `SessionLog` excluded** | Makes both safe default choices for `OutboxRelay::sink` without changing behavior for callers that never set the key (empty key never dedups). `SessionLog` is excluded because its contract is full fidelity — nothing coalesced or dropped. |
| Payload evolution is **detected**, not prevented | **Fingerprint stamped per entry; `replay()` refuses a mismatch** | The additive-only [data-at-rest contract](#data-at-rest-contract) was already published and already unenforced. Strict decode would reject the additive change the contract permits; a lint sees one commit while a journal outlives the deployment that wrote it. A derived fingerprint cannot be forgotten the way a hand-maintained version number can. |
| A mismatch throws rather than warning | **`SchemaMismatchError` out of `replay()`** | The defect is confident wrongness. A suspect holder plus a log line reproduces it with extra steps, and puts the burden of noticing on the code path that demonstrably did not notice. |
| The fingerprint is order-insensitive and compiler-independent | **Key-sorted shape rendering from `std::` traits and reflected key strings** | Reordering members changes nothing about which JSON bytes decode where, so an order-sensitive digest would break replay for a cosmetic edit. A `glz::name_v`-derived tag would be compiler-spelled, making a journal readable only by the compiler that wrote it. |
| A custom-codec type is distinguished by a name it declares, not one the compiler spells | **Opt-in `PayloadShapeTag<T>` specialisation, defaulting to the opaque `x`** | The portability requirement rules out the only *derived* per-type name available, so the name has to be author-written. Opt-in keeps that cost on the handful of types that need it, at the price of a new type silently starting out undeclared — stated as a boundary rather than assumed away. |
| Unstamped entries replay by default | **`UnstampedPayloadPolicy::Replay`** | Refusing them would make an upgrade a data-loss event for every retained journal — this feature's own failure mode, inverted. `Refuse` exists for callers who would rather have no answer than an unverifiable one. |
| Migrations rewrite in memory, never on disk | **`PayloadMigrationRegistry`, applied per replayed entry** | Reading never mutates history — the same rule the [migration recipe](#the-migration-recipe-not-shipped) states for the offline path. It also keeps the migration reviewable as code rather than as a one-time script someone ran. |
| `fromJson` reads leniently | **`glz::read<{.error_on_unknown_keys = false}>`, not `glz::read_json`** | Matches `wire::decode`'s forward-compatibility contract; without it, adding the `v` key itself (or any future key) would be a reader flag-day for every already-deployed reader. |
| `LogEntry::v` defaults to `kLogFormatVersion` | **Default member initializer, not a `toJson`-time stamp** | A freshly constructed entry (every entry `toJson` ever encodes for a new action) already carries the current version for free; a legacy line missing the key decodes with the same default, so "legacy is v1" falls out of the type rather than being special-cased in code. |
| `v` newer than `kLogFormatVersion` throws | **Fail loud, not guess** | A reader has no way to know the shape a future breaking change introduces; refusing to decode is safer than guessing a superset/subset shape. |
| `rotate()` reopens the active path regardless of rename outcome | **Never leave the log unusable** | A failed rename reopens the pre-rotation file in place (no data lost, rotation simply didn't happen); a successful rename reopens a fresh empty file. Either branch leaves `FileActionLog` in a valid, appendable state. |
| `setOutboxManaged` suppresses `recordIfAttached`, not `hasActionLog()` | **Two independent signals** | A store-backed model needs to stop the auto-append without losing "a log is attached" as a fact holders can still query — the suppression is a separate flag, not a side effect of detaching the log. |
| `causalParentId` is an opaque `std::string`, not a `seq` | **App-minted identity, independent of `seq`** | `seq` is sink-local and re-stamped on every forward (see Invariants below), so it cannot serve as a stable cross-sink/cross-restart causal key. Application code mints its own identity for the trigger entry at creation time and reuses it as the cascade entry's `causalParentId`. |
| Replay-mode signaling is a thread-local flag, not a dispatcher parameter | **Additive, mirrors `session::current()`** | Threading a "replay mode" parameter through `ActionDispatcher::dispatch`/every `Model::execute` signature would touch every registered action; a thread-local read via `isReplaying()` needs no signature change anywhere, the same reasoning that already justifies `morph::session::current()`'s shape for `Context`. |

## Invariants

These hold for every sink and are relied on by `replay()`/`undoLast()`:

- **Every loggable action attempt is recorded, tagged with its outcome.** A
  `LogEntry` is produced by `IModelHolder::recordIfAttached` after both a
  successful `Model::execute` (`outcome = Succeeded`) and one that throws — a
  business-rule failure, a validator rejection (`ActionValidator::validate`)
  — (`outcome = Failed`, `error` set, `result` empty). Any action registered
  `Loggable::No` (typically pure queries like `GetAccount`/`ListAccounts`)
  still never appears in the log either way. The log is a record of every
  attempt against a loggable action, not only the ones that committed.
- **`result` reflects post-execution state; only `Succeeded` entries have one.**
  `payload` is the request JSON, always present. `result` is captured only on
  success, so replaying `payload` re-derives an equivalent `result` for a
  deterministic model. A `Failed` entry has no `result` to derive — see
  `replay()`, next.
- **`replay()`/`undoLast()` skip `Failed` entries.** A failed attempt never
  mutated model state, so there is nothing to reconstruct from it — and
  re-dispatching it would likely throw the same exception again, aborting
  reconstruction. `replay()` filters `outcome == Outcome::Failed` entries out
  before dispatching; `Succeeded` entries replay exactly as before.
- **`seq` is sink-local and re-stamped on every forward.** Each sink's
  `append()` overwrites `entry.seq` with its own `++_nextSeq`, ignoring any
  incoming value. When `SessionLog::checkpoint()` forwards entries to a durable
  sink, that sink re-stamps them again. `seq` is therefore an ordering key
  *within one sink instance in one process run* — it is **not** a stable,
  cross-sink or cross-restart identifier. Use `entries()`' natural append order
  for identity/ordering across sinks; do not persist or compare raw `seq`
  values as keys. (`FileActionLog::seq` is likewise fresh per process — it does
  not resume from the highest `seq` on disk.) This is exactly why
  `LogEntry::causalParentId` must never be a `seq` value — see [Causal links
  and replay-mode signaling](#causal-links-and-replay-mode-signaling).
- **`isReplaying()` is `true` for every dispatch inside one `replay()` call,
  and only there.** `replay()` installs `detail::ScopedReplayFlag` once,
  before its dispatch loop, so the flag reads `true` for that loop's entire
  extent (every entry it dispatches) and is restored to its prior value the
  moment `replay()` returns — an ordinary, non-replayed dispatch always reads
  `false`. `SessionLog::undoLast()` calls `replay()` internally, so the same
  guarantee holds for it.
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
- **A stamped entry never reconstructs under a shape other than the one that
  wrote it.** If `LogEntry::schema` is non-empty and disagrees with this
  build's fingerprint for that action, `replay()` either applies a registered
  migration or throws — it never decodes the payload with the mismatched
  shape. The converse is *not* an invariant: an empty `schema` carries no
  evidence either way, and under the default `UnstampedPayloadPolicy::Replay`
  such an entry decodes exactly as it did before this check existed.

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
- **No transactional link to the model's own store, unless it opts in.** By
  default the log and a model's own durable store (e.g. the SQLite in
  `examples/bank`) commit as two independent steps. A crash can leave the store
  committed but the log missing the corresponding entries (uncheckpointed), or —
  with a separately-flushed file — the log ahead of the store. A model that
  writes its own outbox row in its own transaction and calls
  `setOutboxManaged(true)` closes this gap for itself (see
  [Transactional outbox (opt-in)](#transactional-outbox-opt-in)); a model that
  does not opt in must still reconcile the two out of band.
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
- **Transactional outbox is opt-in, not automatic.** `journal::OutboxRelay` plus
  `IModelHolder::setOutboxManaged` close the store/log divergence gap only for a
  model that actively writes its own outbox row and calls
  `setOutboxManaged(true)` (see [Transactional outbox (opt-in)](#transactional-outbox-opt-in)).
  A model that does not opt in keeps the default two-independent-writes
  behavior, and divergence between the two remains the application's problem to
  detect and reconcile.
- **Unbounded in-memory growth; O(n²) repeated undo.** `SessionLog` retains full
  uncoalesced history for the lifetime of the instance — memory grows without
  bound. Each `undoLast()` replays the entire remaining prefix from scratch, so
  undoing the last *k* actions one at a time is O(n·k) ≈ O(n²) in the history
  length. There is no incremental/snapshot fast-path.
- **No automatic rotation and no shipped migration tool.** `rotate()` is a
  seam, not a policy: nothing in morph decides when to call it, and reading
  never transforms history — a breaking change forced through by retention is
  an explicit, offline, host-owned pass over sealed segments (see [The
  migration recipe](#the-migration-recipe-not-shipped)). Absent host-driven
  `rotate()` calls, the active file still grows without bound.
- **No compaction of sealed history.** `checkpoint()` is the only reducer, and
  it runs *before* entries become durable; once a segment is sealed or
  written, morph never rewrites or drops entries from it.
- **The payload fingerprint is forward-looking, and partial.** It protects
  entries written by a build that stamps them; an entry already on disk without
  one is unverifiable and stays that way (see [Unstamped
  entries](#unstamped-entries--what-happens-to-journals-already-written)). It
  describes the *reflected* shape, so it says nothing about an action with a
  hand-written `ActionTraits`, nothing about a swap between two custom-codec
  types that have declared no name, and nothing about entries an application
  journals by hand rather than through the two framework execution sites. The
  full boundary is in [What the fingerprint does not
  catch](#what-the-fingerprint-does-not-catch).
- **The wire-path skew test is still unwritten.** The journal-path half now
  exists: `tests/compile_checks/journal_skew_probe.cpp` is compiled into two
  executables, one recording a journal and the other replaying it with a
  renamed and an added field, run in order as the `journal_skew_old_build_writes`
  / `journal_skew_new_build_replays` ctest pair. The wire path cannot be tested
  the same way, because nothing mechanically enforces the action-evolution
  policy there yet — a per-action fingerprint exchanged at `hello` is issue
  #207's unimplemented proposal, and until it exists a client/server skew test
  has nothing to assert on.
- **`replay()` refuses an additive change, not only a breaking one.** The gate
  is fingerprint equality, so an entry written before a field was *added*
  throws exactly as a renamed one does, even though the
  [data-at-rest contract](#data-at-rest-contract) permits the addition and
  `fromJson`'s lenient decode still reads the old payload faithfully. The
  caller's answer is a migration — for a pure addition, one that hands the
  payload through unchanged. This is a deliberate consequence of choosing
  equality over a compatibility relation (there is no derived way to tell
  "field added" from "field renamed" by comparing two digests), not an
  oversight; the skew test above pins both halves.

## Cross-references

- **`wire.md`** — `wire::decode`'s `error_on_unknown_keys = false` stance and
  duplicate-key caveat, which `journal::fromJson` now mirrors.
- **`registry.md`** — `ModelRegistryFactory`/`ActionDispatcher` and
  `ModelFactory::create`, which auto-attach the default log and which `replay()`
  reuses for dispatch. Also the `ActionDispatcher::coalesce` lookup driving
  `checkpoint()`, and `IModelHolder::setOutboxManaged`/`isOutboxManaged`, the
  opt-out this outbox section's suppression relies on.
- **`bridge.md`** — the two (mutually exclusive) recording call sites
  (`Bridge::executeVia`'s `localOp` for local mode; the `RemoteServer` dispatch
  path for remote/Qt), and `HandlerBinding::contextKey`/`RemoteServer::setLogProvider`
  for per-instance identity.
- **`backend.md`** — how local vs. remote topology decides which recording site
  is live, and why recording is automatically server-side wherever a client/server
  split exists.
- **`error_handling.md`** — `SerializationError` and the failure/validator-rejection
  paths that explain *why* unsuccessful actions never reach the log.
- **`session.md`** — `morph::session::detail::tlsCurrent()`/`ScopedContext`,
  the thread-local-plus-RAII-guard shape `isReplaying()`/`detail::ScopedReplayFlag`
  mirrors for signaling replay mode instead of a per-call `Context`.