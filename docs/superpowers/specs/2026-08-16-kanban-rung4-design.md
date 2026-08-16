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
- `execute(MoveTaskPosition)`: look up `(boardDbId, opId)` in the ledger
  first. **Hit** → decode and return the stored result verbatim; no
  re-validation, no re-application, no second WIP-limit check, no second
  position-renumbering pass. **Miss** → validate → check WIP limit → apply
  the move (renumber positions) → serialize the resulting `GetBoardResult` →
  write it to the ledger → commit, all inside one `SqlTransaction`.

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

## 2. Strand ordering, WIP limits, position renumbering (steps 1–3)

**Decision**: rely entirely on the framework's existing strand-per-instance
guarantee; no new locking, no version counters.

- `BoardModel` is keyed by `projectId`
  (`BRIDGE_MODEL_KEY(kanban::BoardModel, kanban::OpenBoard, &kanban::OpenBoard::projectId)`,
  mirroring `polls::PollModel`'s exact shape), registered `AllowShared` at the
  wiring layer — every viewer of a board attaches to the same server-side
  instance.
- `docs/spec/core/shared_instances.md`'s own stated guarantee — "one strand
  per instance already gives a shared instance the serialisation it needs" —
  is the entire concurrency mechanism `MoveTaskPosition` needs: two users
  dragging tasks on the same board concurrently dispatch through the same
  instance's strand, so one `execute()` runs to completion before the next
  starts. No optimistic version counter, no row-level lock, no CAS: the
  strand already makes "check WIP limit, then write" atomic with respect to
  every other call against the same board.
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

`polls::PollModel::requireAdmin()` is the direct precedent: a private
member function, called at the top of any role-gated `execute()` overload,
throwing `Forbidden` before any state-dependent check runs (so a caller
without the right role learns nothing about the board's state — same ordering
argument `FinalizePoll`'s own doc comment makes). `KanbanAuthorizer` (if one
exists at all) stays unconditionally permissive at the `IAuthorizer` layer,
matching `PollsAuthorizer`'s shape.

## 4. Activity stream (step 5)

**Decision**: derive from `IActionLog::entries(entityKey)` — no new storage,
no framework gap. `BoardModel` attaches its action log with `entityKey =
projectId` (mirroring every keyed model's existing `attachActionLog()`
convention). `LogEntry` already carries every field an activity view needs:
`actionType`, `payload` (JSON request), `result` (JSON response),
`principal`, `timestampMs` — `BoardModel` maps `entries(projectId)` into
`ActivityEvent` view objects filtered/formatted per `actionType` (e.g.
`"MoveTaskPosition"` → "Alice moved Task X to Done").

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
  hooks; kanban's own hook asserts "positions dense/unique, all tasks
  present" per `TESTING.md`'s explicit kanban example.
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
`test_board_qml_bridges.cpp`, `test_kanban_authorizer.cpp` (only if kanban
ships its own `IAuthorizer` at all — per §3, it may not need to override
anything beyond `AllowAllAuthorizer`, in which case this file may not exist),
`test_gui_qml_smoke.cpp`, `test_app.cpp`, and — since `BoardModel` is
`BRIDGE_MODEL_KEY`'d — a mandatory `test_shared_instance_lifecycle.cpp`
covering the keyed-attach backend-mode matrix (`Local`/`LocalSingleThread`/
`Socket`) plus multi-handler shared-instance observation, mirroring
`examples/polls/tests/test_shared_instance_lifecycle.cpp` exactly.

Plus the DoD-mandated stress/offline/contention suites built on the new
testkit pieces from §6: a concurrent-move stress test (ThreadSanitizer, N=4,
`Local` rig mode on `ThreadPoolExecutor`, per `examples/TESTING.md`'s own
kanban-specific note), an exactly-once test using `FaultProxy::dropReply()`
(already shipped, `examples/common/testkit/fault_proxy.hpp:153`), a
kill-the-network-mid-drag test using the new `offline_rig.hpp` (§6) for its
connectivity drop/revive scripting, and the SQLite contention × pool
starvation test using `DbBusyFixture` (already shipped and already used by
bookmarks for the identical `SQLITE_BUSY`-under-a-short-timeout scenario per
that fixture's own doc comment) — a distinct scenario from the
network-connectivity test: `DbBusyFixture` fakes contention on the
*database*, `offline_rig.hpp` fakes drops on the *transport*.

## 9. Out of scope for this spec (confirmed, not re-litigated)

- Automatic actions (README step 6, deferred) and its cascade-journaling
  divergence decision.
- Task attachments (README step 8, deferred) and its HTTP side-channel
  design.

Both remain named in `examples/kanban/README.md`'s own "Deferred within this
rung" section; nothing in this spec changes that scoping.
