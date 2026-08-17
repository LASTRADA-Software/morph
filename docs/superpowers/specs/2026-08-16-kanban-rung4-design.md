# kanban (rung 4) — implementation design

Status: approved for implementation. This document resolves the design
questions `examples/kanban/README.md` leaves open, in writing, per the
[application ladder](../../../examples/LADDER.md)'s own discipline rule
("design questions... must be resolved in writing before the next rung
starts"). It does not restate the README — read that first for scope,
reference implementations, build order, and the Definition of Done.

**Scope**: steps 1–5 and 7 of the README's build order (CRUD + `GetBoard`,
`MoveTaskPosition`, WIP limits, per-project RBAC, activity stream, offline
drag-a-card). Steps 6 (automatic actions) and 8 (task attachments) are
explicitly deferred per the README's own "Deferred within this rung" section
and are out of scope for this spec.

## 1. Exactly-once semantics (`MoveTaskPosition`)

**Decision**: generalize `bookmarks::ImportBookmarks`' existing client-op-id +
server-side applied-ops-ledger pattern
(`examples/bookmarks/src/models/bookmark_model.cpp`, `ImportedOpRecord`),
with one adaptation: the ledger stores the **full serialized result**, not a
placeholder.

- `MoveTaskPosition` carries a client-generated `opId` (an opaque newtype,
  same shape as `bookmarks::ImportOpId` — `std::optional<std::string>`,
  `hasValue()`, `fromOptional`). Generated once per logical user drag, reused
  verbatim on every `SyncWorker` redelivery of the same offline-queued item.
- `BoardModel` (backed by SQLite via `Lightweight`, per the ladder-wide
  convention) keeps a `board_applied_ops` table: `(board_id, op_id)` unique,
  storing the JSON-encoded `GetBoardResult` the original call produced (same
  serialize-a-DTO-to-a-text-column idiom as
  `polls::db::VoteHistoryRecord::previousVotesJson`).
- `execute(MoveTaskPosition)`: **`requireRole` (§3) runs first, unconditionally,
  before the ledger lookup** — a role check is an identity/authorization gate,
  not a re-validation of the *move itself*, so "no re-validation" below refers
  to the action's own business-rule checks (WIP limit, option-belongs-to-board),
  not to authorization. This is a deliberate choice, stated explicitly because
  either answer was defensible and only one keeps the exactly-once contract
  honest: checking the role *after* the ledger hit would let a caller who was
  demoted between the original call and the replay retrieve a stored result
  their current role could not have produced — the opposite of "the same
  answer every time" if that answer is now supposed to be unreachable. Then:
  look up `(boardDbId, opId)` in the ledger. **Hit** → decode and return the
  stored result verbatim; no re-validation, no re-application, no second
  WIP-limit check, no second position-renumbering pass. **Miss** → validate →
  check WIP limit → apply the move (renumber positions) → serialize the
  resulting `GetBoardResult` → write it to the ledger → commit, all inside one
  `SqlTransaction`.

**Why the full result, not a placeholder**: `ImportBookmarks`' replay returns
a cheap `{imported: 0, skipped: 0}` because the whole ladder's own convention
(every mutating action returns the full rebuilt state — see
`polls::PollModel::applyVotes`'s `return buildState(...)` on every path) means
a client's `SyncWorker` retry of `MoveTaskPosition` needs the *real* resulting
board state to reconcile against, not a cheap constant — a placeholder would
silently desync the retrying client from the board's actual position layout.
Storage cost is bounded by one board's serialized state (the same size
already sent over the wire per poll/`GetEventsSince` tick), an app-level
sizing concern, not a blocking design question.

**Why not re-derive fresh state instead of storing it**: board state is not
a pure function of `board_applied_ops` rows alone once other clients have
moved other tasks between the original call and the replay — the *other*
tasks' positions may have changed for reasons unrelated to this op, and the
stored result is a point-in-time snapshot the retrying client is entitled to
see reconciled against (its own `GetEventsSince`/poll cycle picks up
everything since). Storing the actual result, not attempting to recompute
"what would this call have returned now," is the only way to guarantee
exactly-once *semantics* (the same answer every time), not just exactly-once
*application* (the write happens once).

**`board_applied_ops` retention is unbounded** (each row holds a full
serialized `GetBoardResult`) — the same category of concern as morph#112
(the client-side queue's unbounded growth), just on the server side. Not
filed as a separate framework issue, since this table is entirely app-owned
(kanban's own schema, not a framework primitive): a follow-up retention
policy (e.g. prune rows older than some window, once no in-flight
`SyncWorker` could plausibly still replay against them) is deferred as a
known, stated limitation of this rung's first pass, not silently absent.

### `GetEventsSince` is a real table, not the activity-stream journal

The README names a `GetEventsSince`-equivalent read action for step 1's
`GetBoard`-adjacent polling; §4 below derives the *activity stream*
specifically from the journal. These are two different features with two
different durability requirements, and conflating them was an unstated gap
in the original draft:

- The **activity stream** (§4) is read fresh on every poll — there is no
  cursor to persist across a restart, so `IActionLog::entries(entityKey)`'s
  process-local `LogEntry::seq` (verified: `docs/spec/journal/journal.md`
  states plainly that `seq` "is not a cross-restart unique key" and is
  "fresh per process, not resumed from disk") is fine, because nothing needs
  it to survive one.
- **`GetEventsSince`** needs the opposite property by definition: "every
  event since cursor X," where X must remain meaningful across a
  shared-instance destruction/rebirth (a client polling with a stale cursor
  after the server process restarts must still get every real event since,
  never silently nothing). A process-local `seq` cannot serve as that
  cursor.

**Decision**: `GetEventsSince` uses `polls::db::PollEventRecord`'s exact
precedent — a genuine `board_events` table with a table-wide
`ServerSideAutoIncrement` primary key as the wire cursor, populated by an
explicit `mapper->Create(event)` call inside the same transaction as each
mutating action (matching `poll_model.cpp`'s own write-the-event-row pattern
in `applyVotes()`/`execute(AddComment)`/`execute(FinalizePoll)`). This is a
parallel table, and step 5's "derive from the journal, not a parallel table"
guidance does not contradict it: that guidance is scoped to the *activity
stream* specifically (an already-existing README instruction this spec is
honoring, not re-litigating), not to `GetEventsSince`'s polling cursor,
which has never had a journal-backed answer available to it in this
codebase — `LADDER.md`'s own strain-point language ("a client holding
`lastEventId=42`... sees nothing new forever, silently" — the exact bug a
durable, never-reused sequence id prevents) is written about a real table's
autoincrement id, not the journal's `seq`.

## 2. Strand ordering, WIP limits, position renumbering (steps 1–3)

**Decision**: rely entirely on the framework's existing strand-per-instance
guarantee; no new locking, no version counters.

- `BoardModel` is keyed by `projectId`
  (`BRIDGE_MODEL_KEY(kanban::BoardModel, kanban::OpenBoard, &kanban::OpenBoard::projectId)`,
  mirroring `polls::PollModel`'s exact shape) and constructed via the
  `AllowShared`-tagged `BridgeHandler<BoardModel, AllowShared>` at the client
  wiring layer (the same "shared instance" property `PollModel`'s own
  `AllowShared` handler has — `AllowShared` is a `BridgeHandler` template
  tag on the *client* side, not a model-registration-layer property; there is
  no `BRIDGE_REGISTER_MODEL`-level "shared" flag) — every viewer of a board
  attaches to the same server-side instance.
- `docs/spec/core/shared_instances.md`'s own stated guarantee — "one strand
  per instance already gives a shared instance the serialisation it needs" —
  is the entire concurrency mechanism `MoveTaskPosition` needs *within
  `BoardModel`*: two users dragging tasks on the same board concurrently
  dispatch through the same instance's strand, so one `execute()` runs to
  completion before the next starts. No optimistic version counter, no
  row-level lock, no CAS: the strand already makes "check WIP limit, then
  write" atomic with respect to every other `BoardModel` call against the
  same board.
- WIP limit enforcement: a plain in-`execute()` check — count current tasks
  in the target column, throw a typed `Conflict` if the move would exceed the
  column's limit — mirroring `polls::PollModel`'s `Conflict{"poll is
  finalized"}` pattern for a state-dependent rejection. The client renders the
  typed error (per the README's own line).
- Position renumbering: dense integer positions per `(columnId, swimlaneId)`,
  rewritten via the same delete-then-recreate idiom
  `polls::PollModel::applyVotes()` uses for vote replacement — never an
  in-place index shuffle — so the strand's serialization is what prevents two
  interleaved moves from ever producing a gap or duplicate position, and the
  stress test (DoD) asserts exactly that invariant.

**`ProjectAdminModel`'s write surface is a separate strand — drawn explicitly
here, not left implicit.** The README names `ProjectAdminModel` alongside
`BoardModel` with no further elaboration; its own concurrency story cannot
be "the strand handles it" by default, because it *is* a different strand
(a different keyed/shared instance, or possibly a plain per-caller model —
either way, not the same instance as the board it administers). **Decision**:
`ProjectAdminModel` owns column/swimlane/WIP-limit *structural* changes
(create/delete/rename a column, change a WIP limit, add/remove a project
member's role) and project-level lifecycle (`CreateProject`, archive);
`BoardModel` owns everything that mutates task/vote/comment rows and reads
board state. A structural change concurrent with a `MoveTaskPosition` on the
same project (the README's own "column deleted while offline" scenario, and
its live-session cousin: a column deleted mid-drag) is **not** covered by
`BoardModel`'s strand, and must be handled the same way any cross-model
foreign-key-shaped inconsistency is: `BoardModel::execute(MoveTaskPosition)`
re-validates the target column still exists (the FK-shaped-but-not-FK-enforced
check `polls::PollModel::requireOptionBelongsToPoll` already establishes the
precedent for) immediately before applying the move, inside its own
transaction — so a column deleted between `GetBoard` and `MoveTaskPosition`
surfaces as a typed `NotFound`/`Conflict` on the move itself, not a silent
write into an orphaned row. This does not need cross-strand coordination;
it needs the same "trust nothing read before this call, re-check inside the
transaction" discipline every mutating action in this codebase already
follows.

## 3. Per-project RBAC (step 4)

**Decision**: enforced entirely inside `BoardModel::execute()` via a
`requireRole(Role)` helper querying `project_has_roles` directly — not a
change to `IAuthorizer`. Already recorded in `examples/kanban/README.md`
step 4's own updated wording; restated here for spec completeness.

Grounded in two existing, citable design decisions (not invented for this
rung):

- `docs/spec/core/shared_instances.md`: "the alternative — teaching
  `authorizeInstance` about a set of owners — makes a simple, shipped,
  verified hook substantially more complex to serve a case the model layer
  can handle," and "an application that needs per-instance ownership on a
  shared model must enforce it inside the model."
- `docs/spec/security.md`'s local-path note: `authorizeInstance` never runs
  for `LocalBackend` callers at all, so an `IAuthorizer`-only RBAC check would
  silently not exist for local callers (which every rung's dual-mode test
  convention, `examples/TESTING.md`, exercises) — `BoardModel` needs the real
  check regardless of what the authorizer does, making a framework interface
  change pure duplicated surface with zero coverage gain.

`polls::PollModel::requireAdmin()` is the direct precedent for the
*mechanics* of the in-model check: a private member function, called at the
top of a role-gated `execute()` overload, throwing `Forbidden` before the
poll row's *state* is inspected (so a caller without the right token learns
nothing about whether the poll happens to already be finalized) — this
ordering argument is `FinalizePoll`'s own doc comment, and it carries over
to kanban unchanged, though for `requireRole` it means Forbidden-before-the-
state-dependent-`Conflict` check specifically, not literally "before any
check the method makes" (loading the target row to know which project's role
table to query still has to happen first).

### Identity: `PollsAuthorizer`'s shape is the wrong precedent here (corrected)

**This section originally cited `PollsAuthorizer`** (`AllowAllAuthorizer`-
derived, `authenticate()` inherits the `nullopt` default) as kanban's
authorizer shape. That is wrong, and it does not merely under-specify —
it silently breaks step 4 over the socket. `docs/spec/security.md`'s
documented behavior: when `authenticate(ctx)` returns `nullopt`,
`dispatchExecute` **clears `env.session.principal` to the empty string**
before dispatch, precisely so an authorizer that "does not authenticate...
[including] the default `AllowAllAuthorizer`" never hands model code an
unverified claim dressed up as authoritative. `requireRole(Role)` has
nothing to key its `project_has_roles` lookup on if `Context::principal` is
unconditionally empty on every remote call — every role check would either
always deny (correct-looking, wrong reason) or need to be stubbed out
entirely for `Socket` mode, silently diverging from `Local` mode (where a
test can hand-populate `Context::principal` directly) in exactly the
dual-mode matrix `examples/TESTING.md` mandates every rung run its tests
through.

**Corrected precedent: `bookmarks::auth::BookmarksAuthorizer`**, which derives
from `::morph::session::SigningAuthorizer` (a real, verifying authorizer,
not `AllowAllAuthorizer`) for exactly this reason — any model that reads
`session::current()->principal` to make an authorization-relevant decision
needs a trustworthy principal, and only a verifying authorizer supplies one.
`KanbanAuthorizer` should therefore mirror `BookmarksAuthorizer`'s shape
(`SigningAuthorizer`-derived, hooks left at their permissive defaults except
where kanban needs a carve-out — `BookmarksAuthorizer`'s own header
documents which of its hooks are genuine overrides versus inherited), not
`PollsAuthorizer`'s.

This pulls in bookmarks' own login/token machinery as a dependency, not a
new design: `AuthModel::execute(const Login&)` (`examples/bookmarks/src/
models/auth_model.cpp`) mints a session token via an installed `TokenIssuer`
for any syntactically valid principal — no separate registration step, the
principal *is* the login. Kanban reuses this shape as-is (a `Login` action,
a `TokenIssuer` wired the same way `App::App()` wires bookmarks').

**Who seeds the first `manager` role on a project**: `CreateProject`'s
caller (`Context::principal` at the time of the call, now trustworthy) is
written into `project_has_roles` as that project's `manager` in the same
transaction that creates the row — the same shape as `CreatePoll` returning
its caller-scoped `adminToken`, adapted to a durable per-user role row
instead of a bearer token, since kanban's roles are per-authenticated-user
rather than per-poll-bearer-secret.

## 4. Activity stream (step 5)

**Decision**: derive from `IActionLog::entries(entityKey)` — no new storage.
**Corrected from the original draft**: no rung actually established an
"`attachActionLog()` convention for keyed models" to mirror — grepping
pastebin/bookmarks/polls confirms none of them calls `attachActionLog` at
all; only `examples/bank` and `examples/concepts/journal_and_outbox.cpp` do.
Polls, the one keyed precedent, derives its event stream from its own
`PollEventRecord` table — the parallel-table shape the kanban README's step
5 explicitly tells this rung *not* to use ("derive the stream *from the
morph journal* instead of a parallel table"). So kanban is the **first**
rung to attach a journal to a keyed/shared model and read it back for a
feature, not a follower of an established pattern. `BoardModel` calls
`attachActionLog(log, /*entityKey=*/projectId)` itself (the mechanism
exists and is documented in `docs/spec/journal/journal.md`'s "Attaching a
log to remote instances" section even though no rung has exercised it yet);
`LogEntry` already carries every field an activity view needs (`actionType`,
`payload`, `result`, `principal`, `timestampMs`) — `BoardModel` maps
`entries(projectId)` into `ActivityEvent` view objects filtered/formatted
per `actionType` (e.g. `"MoveTaskPosition"` → "Alice moved Task X to Done").

**Plumbing this rung must actually decide (not yet resolved by precedent,
since none exists)**:

- **`LocalBackend` has no `LogProvider`.** `RemoteServer::setLogProvider`
  attaches a log to a remotely-constructed holder; the ladder's own dual-mode
  test convention (`examples/TESTING.md`) requires `Local`/`LocalSingleThread`
  rig modes too, where no such attach path exists today. This rung needs to
  either extend `AppContext`'s `Local` mode to attach a log at construction
  (a small, `LocalBackend`-side addition, scoped to this rung's own
  bootstrap code — not a framework interface change) or accept that the
  activity stream is a `Socket`-mode-only feature for this rung's test
  matrix, stated explicitly rather than silently absent from `Local` runs.
- **The read path needs the same log instance the write path appended to.**
  A registry-constructed `BoardModel` is default-constructed; `entries()`
  must be called against the *same* `IActionLog` the executing holder
  attached, not a fresh one. `BoardModel` holds a `std::shared_ptr<IActionLog>`
  member (set via the same attach call as above) rather than reaching for a
  process-global default — mirrors how `_pollId` is `PollModel`'s own
  per-instance cached state, not a global.
- **`entries()` re-reads the whole file per call** for `FileActionLog`
  (`LADDER.md`'s own journal-honesty note on this cost). `GetActivity` is a
  polled action (same cadence as `GetEventsSince`), so this is read
  amplification on every poll tick, not a one-time cost. Acceptable for this
  rung's scale (per-board activity, not global) but worth a one-line note in
  the model's own doc comment so a future rung at bigger scale doesn't
  assume the same approach is free.
- **Ledger hits (§1) must not double-journal — verified empirically, not
  assumed.** A live `FileActionLog` was captured during real `RemoteServer`
  dispatch (`test_app.cpp`'s own raw re-read of the on-disk file) and showed
  **exactly one** `BoardModel` journal entry per dispatched action, including
  across a ledger-hit replay: the framework's auto-append
  (`include/morph/core/registry.hpp`'s `ActionExecuteRegistry::
  registerAction` runner → `IModelHolder::recordIfAttached`) does **not**
  produce a second entry on this path. The original draft of this section
  reasoned from the runner's source alone and concluded the opposite —
  that a wrong premise. The actual duplicate seen in earlier testing was
  `BoardModel`'s own doing: `execute(MoveTaskPosition)`'s ledger-hit branch
  called `logAction(action, replayed)` before returning, claiming
  `outcome: Succeeded` for an operation the call didn't perform this time (it
  only returned a previously-stored result). **Fix**: that call was deleted —
  a ledger hit performs nothing new and journals nothing. `GetActivity` no
  longer needs (and no longer has) a read-side collapse: it maps
  `entries(projectId)` to `ActivityEvent` directly, one entry per journal
  row, with no consecutive-duplicate suppression. The earlier collapse
  approach was also independently wrong on its own terms — it dropped *any*
  two consecutive entries with identical `actionType`+`payload`, which would
  have silently under-reported two genuinely distinct identical actions
  (e.g. the same comment body submitted twice on purpose), not just replays.

## 5. Offline drag-a-card (step 7)

**Decision**: compose existing, shipped infrastructure — `SqliteOfflineQueue`
(`MORPH_BUILD_OFFLINE_SQLITE`), `SyncWorker`, `ReconnectCoordinator`,
`NetworkMonitor` — no new framework primitive beyond the one gap filed below.

- The client enqueues `MoveTaskPosition{opId, ...}` JSON via
  `SqliteOfflineQueue::enqueue(payload, idempotencyKey = opId)` — the queue's
  own enqueue-time dedup (a re-enqueue of the same `opId` while offline is a
  no-op) composes cleanly with the server-side ledger from §1: client-side
  dedup prevents queue bloat from a UI double-submit, server-side ledger
  handles the "already applied, replay is safe" case regardless of how many
  times `SyncWorker` actually calls back.
- `SyncWorker::ReplayFunction` receives the raw payload, dispatches it through
  the normal `Bridge`/`BridgeHandler` path exactly as an online client would
  — the server-side ledger check in §1 is what makes this replay-safe, not
  anything `SyncWorker` itself needs to know about.
- **Verified, not assumed**: `ReconnectCoordinator::Deps::shouldContinue` is
  "polled before each reconnect attempt and once more before replay" — i.e.
  a reconnect flap genuinely cannot preempt a replay already in progress
  (`include/morph/offline/reconnect_coordinator.hpp:113-116`). This is
  existing, documented framework behavior — the README's "five flaky
  reconnects dead-letter every queued move" strain point is a real,
  testable consequence of this shape, not a gap to fix.
- **Enqueue trigger**: the client enqueues on a **failed dispatch attempt**,
  not on `NetworkMonitor`'s offline signal alone — a `MoveTaskPosition` that
  the presenter tries to send and that fails (connection genuinely down, or
  a transient socket error) is what queues; `NetworkMonitor` going offline by
  itself does not retroactively queue anything already in flight or already
  succeeded. This matches `ReconnectCoordinator`'s own division of labor
  (`activateLocal`/`activatePrimary` switch the *active* backend;
  `SyncWorker` only ever drains what got enqueued, it does not decide *what*
  gets enqueued) and avoids inventing a second enqueue path alongside the
  one every offline-capable presenter method already needs (an `.onError`
  handler that queues instead of showing a failure).
- **`DeadLetterSink` wiring (a named DoD item, previously unaddressed)**:
  kanban installs a `DeadLetterSink` on its `SyncWorker` that appends a typed
  `DeadLetteredMove` entry to a small in-memory (desktop-process-lifetime)
  list the presenter surfaces as "N changes could not be synced" — the exact
  wording the DoD names. On a fresh reconnect the list is *not* automatically
  cleared (a dead-lettered move is gone for good, per `SyncWorker`'s own "no
  redo" contract — the item was already removed from the queue when the sink
  fired), so the GUI's count only clears on explicit user acknowledgement.
- **Conflict-on-replay does not need special-casing against the retry
  budget.** A `Conflict` (e.g. the column-deleted-while-offline scenario from
  §2's cross-strand note) surfacing on replay is exactly what
  `SyncWorker::ReplayFunction`'s documented contract already handles: return
  `false` (or let the thrown exception propagate — "same path as returning
  `false`"), the item's attempt counter increments, and it either retries (if
  the underlying cause is transient — unlikely for a genuine `Conflict`, but
  the worker does not need to know the difference) or exhausts its 5-attempt
  budget and dead-letters. No new "consume immediately on `Conflict`" path is
  needed: a `Conflict` that will never succeed burns its retry budget in the
  same 5 attempts a transient failure would, converging on dead-letter either
  way, which is the correct outcome for "this queued move can never apply."
- **Observability**: the DoD names asserting `morph::observe`'s `queueDepth`
  and reconnect attempt/outcome metrics — this is the framework's existing
  `morph::observe::MetricSink` (already wired for other rungs per
  `LADDER.md`'s cross-cutting stress map), not new instrumentation kanban
  builds; the test obligation is asserting the metric values a scripted
  offline/reconnect sequence produces, via `offline_rig.hpp` (§6).

### Framework gap filed: offline queue overflow policy

**[morph#112](https://github.com/LASTRADA-Software/morph/issues/112)** —
`IOfflineQueue::enqueue()` (and both shipped implementations,
`SqliteOfflineQueue`/`FileOfflineQueue`) accept and grow unconditionally: no
capacity parameter, no depth cap, no overflow signal anywhere in the
interface. Verified by reading `include/morph/offline/offline_queue.hpp`,
`sqlite_offline_queue.hpp`, `file_offline_queue.hpp` — confirmed real, not
merely suspected. Needs a framework-level decision (evict-oldest vs.
reject-newest vs. app-defined policy callback) before this rung's offline
stack can define its own overflow behavior; filed rather than designed
around, since the decision affects `IOfflineQueue`'s public interface and
should not be made unilaterally inside one rung's app code. Marked in
`examples/kanban/README.md`'s own "Expected strain points" section.

## 6. Testkit obligations this rung must build

Two categories, both real scope items for this rung's DoD — not optional,
not something to discover mid-implementation:

**Owned by rung 4, per `examples/TESTING.md`'s own component-ownership
table** (verified: none of these three files exist yet):
- `action_driver.hpp` — `SeededScript`: seeded (`MORPH_STRESS_SEED`, always
  printed on failure) weighted action generator with per-burst invariant
  hooks; `TESTING.md`'s own example names kanban's hook as "positions
  dense/unique" — "all tasks present" is this spec's own addition (from the
  kanban README's "assert the board invariant (positions dense and unique,
  **all tasks present**)" strain-point line), not a second claim from
  `TESTING.md` itself; both invariants belong in the same per-burst hook.
- `process_pool.hpp` — QProcess-based client harness for rung-8 load-script
  scale and client-crash tests (kill mid-execute/mid-attach, assert
  connection-scope reclamation).
- `offline_rig.hpp` — scripted connectivity drop/revive (close/reopen the
  in-test `QtWebSocketServer` on the same port) feeding
  `ReconnectCoordinator`, plus queue-depth inspection.

**Deferred by rung 3, absorbed here** (verified: `TESTING.md`'s own
component table names `client_pool.hpp`/`convergence.hpp` as "first needed
by rung 3," but polls — now merged as PR #91 — never built them; it used
raw multi-client `BackendRig{Mode::Socket, N, ...}` instances directly for
its own lifecycle/ownership tests instead of a reusable convergence
abstraction). This was not a documented, deliberate deferral — polls'
README makes no mention of either file — so it reads as a planning gap
rather than an intentional decision. Rung 4 needs a genuine N-client
convergence assertion (`stateFingerprint()`/`lastEventId()` comparison across
clients, per `TESTING.md`'s own "Canonical state fingerprint" convention) for
its own "two clients' queues replaying interleaved" DoD item, so building
`client_pool.hpp`/`convergence.hpp` here is required regardless of whose
obligation it originally was. `TESTING.md`'s ownership table should be
corrected once these land, to avoid the same discrepancy recurring for a
future rung's planning pass.

## 7. Entities, DTOs, and model registration — conventions confirmed, not new

No new pattern invented here; every shape below is a direct application of
bookmarks'/polls' own established conventions (verified against
`bookmark_entity.hpp`, `poll_entity.hpp`, `bookmark_dto.hpp`, `vote_dto.hpp`,
`poll_model.hpp`, `bookmark_model.cpp`'s `static_assert` block):

- **Strong ids** (`ProjectId`, `ColumnId`, `TaskId`, `SwimlaneId`, `TagId`):
  the `std::optional<std::int64_t>` + `hasValue()` + `operator*` +
  `fromOptional` + `operator<=>` shape (`bookmarks::BookmarkId`'s shape, not
  `polls::OptionId`'s zero-sentinel shape), since every one of these is a
  server-assigned auto-increment surrogate key returned fresh from a
  `Create*` action — the same reason `BookmarkId` chose that shape over
  `OptionId`'s.
- **Entities**: `Light::Field<std::uint64_t, Light::PrimaryKey::
  ServerSideAutoIncrement, Light::SqlRealName{"id"}>` for every primary key;
  `Light::BelongsTo<&ParentRecord::id, Light::SqlRealName{"..._id"}>` for
  every foreign key; zero `HasMany`/`HasManyThrough` (the
  `DataMapper::Update()`/`EnumerateRecordMembers` incompatibility both
  sibling entities' file comments already cite); child-row reads always via
  a plain `Query<T>().Where(FieldNameOf<&T::project>, "=", projectDbId)` call
  in `board_model.cpp`, never an embedded relation field. Bounded free-form
  text (task title, column name, comment body) gets `Light::SqlAnsiString<
  kMax*Bytes>` matching each field's own DTO-level cap, with a
  `static_assert(decltype(db::TaskRecord::title)::ValueType{}.capacity() ==
  kMaxTaskTitleBytes, ...)` pinning the two together, one per bounded column,
  placed in `board_model.cpp` right after the entity include (the exact
  `bookmark_model.cpp`/`poll_model.cpp` pattern). Unbounded fields (comment
  body, if the DTO never caps it) get `Light::SqlMaxDynamicAnsiString`, and
  their DDL column is `NVarchar(0)` — not `Text()` — per the fix already
  applied to both merged rungs (bookmarks PR #90, polls PR #91) for this
  exact DDL/entity mismatch.
- **DTOs**: one canonical `GetBoardResult` read-model, returned by every
  mutating action (`CreateTask`, `MoveTaskPosition`, `AddComment`, etc.) —
  the same "every mutating action returns the full rebuilt state" convention
  polls established, not a bespoke result type per action. `optionalFields`
  arrays name every schema-omittable field, same as `CreateBookmark`'s.
- **Model registration**: `BRIDGE_REGISTER_MODEL(kanban::BoardModel,
  "BoardModel")`, one `BRIDGE_REGISTER_ACTION` line per action (added only
  once its `.cpp` body exists — `poll_model.hpp`'s own documented reason:
  the registrar takes the address of `Model::execute(Action)` and needs a
  linkable definition), `::morph::model::Loggable::No` on read-only/attach
  actions (`OpenBoard`, `GetBoardState`, `GetEventsSince`-equivalent),
  `BRIDGE_MODEL_KEY` immediately after the registration block.

## 8. Test file plan

Following polls' own file organization (kanban is keyed/shared, same as
polls, unlike bookmarks):

`test_kanban_types.cpp`, `test_kanban_schema.cpp`, `test_board_dto.cpp` (+ a
split file per action family if it grows large — e.g. `test_task_dto.cpp`),
`test_board_model.cpp` (one file per model class only if Project/Column/
Task/Swimlane/Tag end up split across multiple model classes — default
assumption is one `BoardModel` serving all of them, per the README's own
"Models: `BoardModel` keyed by project id... `ProjectAdminModel`" line, so
also `test_project_admin_model.cpp`), `test_board_presenter.cpp`,
`test_board_qml_bridges.cpp`, `test_kanban_authorizer.cpp` (mandatory, unlike
the original draft assumed — §3's corrected identity story means
`KanbanAuthorizer` is `SigningAuthorizer`-derived, mirroring
`BookmarksAuthorizer`, not the near-empty `AllowAllAuthorizer` shape
`PollsAuthorizer` uses; this file tests the same shape
`test_bookmarks_authorizer.cpp` does — token verification, the carve-outs
`KanbanAuthorizer` actually overrides), `test_gui_qml_smoke.cpp`,
`test_app.cpp`, and — since `BoardModel` is
`BRIDGE_MODEL_KEY`'d — a mandatory `test_shared_instance_lifecycle.cpp`
covering the keyed-attach backend-mode matrix (`Local`/`LocalSingleThread`/
`Socket`) plus multi-handler shared-instance observation, mirroring
`examples/polls/tests/test_shared_instance_lifecycle.cpp` exactly.

Plus the DoD-mandated stress/offline/contention suites built on the new
testkit pieces from §6: a concurrent-move stress test (ThreadSanitizer, N=4,
`Local` rig mode on `ThreadPoolExecutor`, per `examples/TESTING.md`'s own
kanban-specific note) using the already-shipped **`strand_interleaver.hpp`**
(`examples/common/testkit/strand_interleaver.hpp`) to make the interleaving
between concurrent `MoveTaskPosition` calls deterministic rather than
probabilistic — without it, a position-renumbering bug could pass most runs
by luck and only fail occasionally under real thread scheduling, which is
exactly the flakiness this fixture exists to remove; an exactly-once test
using `FaultProxy::dropReply()` (already shipped,
`examples/common/testkit/fault_proxy.hpp:153`); a kill-the-network-mid-drag
test using the new `offline_rig.hpp` (§6) for its connectivity drop/revive
scripting; and the SQLite contention × pool starvation test using
`DbBusyFixture` (already shipped and already used by bookmarks for the
identical `SQLITE_BUSY`-under-a-short-timeout scenario per that fixture's own
doc comment) — a distinct scenario from the network-connectivity test:
`DbBusyFixture` fakes contention on the *database*, `offline_rig.hpp` fakes
drops on the *transport*.

## 9. Out of scope for this spec (confirmed, not re-litigated)

- Automatic actions (README step 6, deferred) and its cascade-journaling
  divergence decision.
- Task attachments (README step 8, deferred) and its HTTP side-channel
  design.

Both remain named in `examples/kanban/README.md`'s own "Deferred within this
rung" section; nothing in this spec changes that scoping.
