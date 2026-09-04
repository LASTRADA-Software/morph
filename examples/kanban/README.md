# kanban — rung 4 of the [application ladder](../LADDER.md)

**Status: shipped** — every rung-4 task is complete, including automation
rules and attachments (originally deferred, now implemented); see
[Definition of done](#definition-of-done) for what that does and does not
mean (one disclosed gap: no `--seed` demo path yet, per `LADDER.md`'s
ladder-wide convention). A multi-project kanban board: columns, swimlanes,
tasks, drag-and-drop moves, WIP limits, comments, per-project roles, an
activity stream, and automation rules. The mid-tier flagship: the first
app where concurrency, authorization, offline, and the journal are all
load-bearing at once. As the one polished showcase (round-7 audience
decision), this rung alone may spend effort on visual presentation; every
other rung stays deliberately unstyled.

## Reference implementations

- **[Kanboard](https://github.com/kanboard/kanboard)** (PHP, MIT, SQLite
  first-class, maintenance-mode = a reference that won't shift under you) —
  the anchor, for two exceptional properties:
  - Its official API is **JSON-RPC 2.0** — a documented catalog of named,
    permission-checked procedures (`createTask`, `moveTaskPosition`,
    `assignTask`, …) that is effectively a pre-written, battle-tested typed
    action vocabulary. Transliterate it into morph actions nearly 1:1:
    <https://docs.kanboard.org/v1/api/>
  - Its full SQLite schema is checked in at `app/Schema/Sql/sqlite.sql`
    (40+ tables) — copy the core subset.
- [Focalboard](https://github.com/mattermost-community/focalboard) (Go,
  SQLite default; unmaintained — study, don't depend) — secondary: its
  "everything is a block with JSON props" model and its
  broadcast-is-only-an-optimization WebSocket design confirm last-writer-wins
  CRUD + polling is enough for boards.
  [Planka](https://github.com/plankanban/planka) is the maintained equivalent.

## What to implement

Models: `BoardModel` keyed by project id (shared instance — every viewer of a
board attaches to the same server-side instance), `ProjectAdminModel`.
Entities (Kanboard subset): project, column (+ WIP limit), swimlane, task,
subtask, comment, tag, user/role (`project_has_users`), automatic action,
activity event.

Build order:

1. Project/column/task CRUD + `GetBoard` (lift `GetEventsSince` polling from
   [`polls`](../polls)).
2. **`MoveTaskPosition { taskId, columnId, position, swimlaneId }`** — the
   centerpiece. Two users dragging tasks on the same board concurrently is a
   precise test of per-model strand ordering: actions serialize, positions
   stay consistent, both clients converge on the next poll. Write the
   many-clients stress test around exactly this action.
3. WIP limit enforcement — server-side validation rejecting a move; the
   client renders the typed error.
4. Per-project RBAC (viewer/member/manager), enforced **inside
   `BoardModel::execute()`** by a `requireRole(Role)` helper querying
   `project_has_roles` directly — mirroring `polls::PollModel::requireAdmin()`'s
   exact precedent, not a change to `IAuthorizer`.
   `docs/spec/core/shared_instances.md` already settles this for shared
   instances generally ("the alternative — teaching `authorizeInstance` about
   a set of owners — makes a simple, shipped, verified hook substantially more
   complex to serve a case the model layer can handle"; "an application that
   needs per-instance ownership on a shared model must enforce it inside the
   model"), and `docs/spec/security.md` requires model-level enforcement
   regardless, since `authorizeInstance` never runs on the `LocalBackend` path
   at all — an `IAuthorizer`-only check would silently not exist for local
   callers. `requireRole` needs a *trustworthy* `Context::principal` to key
   its lookup on, though, which a permissive `AllowAllAuthorizer`-derived
   authorizer cannot supply (`security.md`: an authorizer that never
   authenticates has its principal cleared to empty before dispatch) — so
   `KanbanAuthorizer` is `SigningAuthorizer`-derived, mirroring
   `bookmarks::auth::BookmarksAuthorizer`'s shape, not `PollsAuthorizer`'s.
   Kanboard enforces permissions per procedure, so this rung mirrors that per
   action, at the model layer, on top of a verified identity.
5. Activity stream — Kanboard's `project_activities` table is a journal
   cousin: derive the stream *from the morph journal* instead of a parallel
   table.
6. **Automatic actions** — Kanboard's event→condition→mutation rules (e.g.
   "task moved to Done ⇒ assign to closer, add tag"). One client action
   cascades into further model mutations. **Cascades are journaled with a
   causal parent-id, and rule evaluation is suppressed during replay.** A
   cascaded mutation's `LogEntry` carries `causalParentId` set to the
   triggering entry's identity, so the activity feed can render "caused by
   task move X," and `replay()` re-applies every recorded entry — trigger
   and cascade alike — without re-running rule evaluation, so a rule firing
   again on the replayed trigger can never double-apply the cascade. This
   requires `morph::journal` to carry a causal-parent-id field and to signal
   replay mode to executing code — both are new, general-purpose framework
   capabilities (`docs/spec/journal/journal.md`), not kanban-specific code;
   see the divergence test this rung adds once the rules engine lands.
   `causalParentId` must key on a stable id independent of `LogEntry::seq`
   (sink-local, re-stamped on every forward — see
   `docs/spec/journal/journal.md`'s Invariants section, and this rung's
   design spec §9 for the full reasoning). [`ledger`](../ledger) reuses
   this same answer.
7. **Offline drag-a-card** — this rung's framework-level deliverable, with
   a **scope correction from review: the offline stack does not run on WASM
   today.** `NetworkMonitor` is a background probe thread (WASM build is
   single-threaded) and `SqliteOfflineQueue` needs a durable filesystem
   (Emscripten = async IDBFS). So: offline is **desktop-first** here using
   `SqliteOfflineQueue` (`MORPH_BUILD_OFFLINE_SQLITE`), `NetworkMonitor`,
   `SyncWorker`, `ReconnectCoordinator`; a browser-native equivalent
   (IndexedDB-backed `IOfflineQueue`, online/offline DOM events feeding
   the coordinator) is a stretch goal, explicitly not assumed — and per
   round-7 T5 it is **framework-candidate code**: an `IOfflineQueue`
   implementation belongs in morph or nowhere, never as app code in this
   rung. Queued moves
   replay on reconnect; conflicts surface as typed errors from the model,
   not silently — and not through a reconciliation hook: neither
   `BoardModel` nor `ProjectAdminModel` implements `onBackendChanged()`, and
   nothing in this rung uses it. `execute(MoveTaskPosition)` re-checks the
   whole destination inside its own transaction and throws the rung's typed
   errors — `NotFound` for a column or swimlane that no longer belongs to
   the project (`src/models/board_model.cpp:966`, the
   `requireColumnBelongsToProject` call, and `:998`, the swimlane half's
   inline re-check), `Conflict` for a target column already at its WIP limit
   (`:984`, the `throw Conflict`). Each citation names what is on the line as
   well as the number: nothing here verifies a line number —
   `scripts/check_spec_citations.sh` checks cited paths and section headings
   only — so the name is what a reader follows once the line has moved.
   A replay that keeps
   throwing is retried by `SyncWorker` up to its 5-attempt cumulative cap
   and then dead-lettered, and `BoardBridge`'s `DeadLetterSink` turns that
   into a `syncStatusChanged(queueDepth, deadLettered)` emission the GUI
   renders.
8. Task attachments — first blob answer: bytes over a side channel (plain
   HTTP endpoint next to the WebSocket server), metadata through actions.

## morph subsystems exercised

Strand ordering under real contention (2), typed server-side validation (3),
authorization at Kanboard's granularity (4), journal-derived activity + undo
(5, 6), the full offline stack (7), shared board instances throughout.

**`morph::forms`: every form on every screen.** Six of this rung's inputs are
rendered from `morph::forms::schemaJson<A>()` through the shipped `MorphForms`
`DynamicForm`, each submitting through the renderer's own explicit Submit
button — the same shape `bookmarks`, `polls`, `lims` and `pastebin` ship:

| Form | Action | Screen |
|---|---|---|
| Sign in | `Login` | `gui/qml/LoginView.qml` |
| Create project | `CreateProject` | `gui/qml/ProjectListView.qml` |
| Add member | `SetMemberRole` | `gui/qml/MembersView.qml` |
| Add column | `CreateColumn` | `gui/qml/BoardView.qml` |
| Add swimlane | `CreateSwimlane` | `gui/qml/BoardView.qml` |
| New task | `CreateTask` | `gui/qml/BoardView.qml`, one instance per column |
| Add comment | `AddComment` | `gui/qml/TaskDetailPopup.qml` |
| Add rule | `CreateRule` | `gui/qml/RulesView.qml` |

Not one of them has a hand-written field or a hand-written submit button; if any
of those eight actions grows a member, no `.qml` file changes. Both QML bridges
expose the one schema document (`gui_lib/kanban_schemas.hpp`) and the
`submitIfValid` controller contract the renderer calls, and each routes only the
actions its own models serve — an action a controller does not serve is reported
back on `replyReceived`, never dropped.

Three of those actions carry ids that are *context rather than input* — which
project a member is added to, which column a task is created in, which task a
comment lands on. They are declared `hidden` in the DTO's own `fieldMetadata`
(rule 3's "the DTO *is* the form definition"), so the renderer draws no control
for them and the view that owns the form supplies them with `setFieldValue`.
That keeps "type a title in the column you want it in" working without a raw
row id ever appearing on screen. A fourth, `CreateRule::triggerColumnId`, is a
user-chosen foreign key rather than context — it stays visible, as a
`morph::forms::Choice<…, "GetBoardState">` combo box the renderer fetches its
own options for via `BoardBridge::fetchOptions`, the shape rule 3 prescribes
for exactly this case.

**This is rendered, not merely generated.** `tests/test_gui_forms_render.cpp`
loads the shipped `.qml` into a real QML engine with real bridges over a real
backend, and for each of the seven non-login forms asserts the controls the
renderer drew, the body it assembled, and the row the model ended up with. It
was mutation-checked twice: dropping the hidden-field seeding leaves `CreateTask`
permanently `!ready`, and dropping `hidden` from `CreateTask::columnId` makes the
raw id column visible.

### What is still hand-built, and why

[`IMPLEMENTATION.md`](../IMPLEMENTATION.md)'s rule 2 forbids hand-built input
widgets by default and allows exactly two justifications, each of which has to
be written here. Two elements remain, both rule 2(a) — "the generated UI
*cannot* express the interaction":

| Hand-built input | Action | Rule 2 justification |
|---|---|---|
| `gui/qml/BoardView.qml`'s drag-and-drop board | `MoveTaskPosition` | **(a).** A drag is a gesture, not a form. See below. |
| `gui/qml/MembersView.qml`'s per-row role `ComboBox` (`:78`) | `SetMemberRole` | **(a).** Edits one field of an existing row in place, on selection change, with no Submit gesture — not the create-a-new-thing shape `DynamicForm` renders. See below. |

**The enum-rendering gap this table used to cite is closed.**
`morph::forms::schemaJson` describes a C++ `enum class` completely — `Role`
emits as `{"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"},…]}` —
and `DynamicForm` now draws that as a combo box, refusing a value outside the
set ([#386](https://github.com/LASTRADA-Software/morph/issues/386)). Both
`SetMemberRole` and `CreateRule` render as ordinary schema-driven forms as a
result ([#393](https://github.com/LASTRADA-Software/morph/issues/393)):
`gui/qml/MembersView.qml`'s "add member" row and `gui/qml/RulesView.qml`'s
"add rule" row are both `DynamicForm`s now, and
`CreateRule::triggerColumnId` is a `morph::forms::Choice<…, "GetBoardState">`
(rule 3's shape for a user-chosen foreign key), fetched by the renderer itself
through `BoardBridge::fetchOptions`. Only `MembersView`'s **per-row** role
picker — editing an existing member in place, not adding one — stays
hand-built, on different grounds than the enum gap: it is an inline-edit
interaction, not a form, the same reasoning the drag-and-drop board below
already gives for its own gesture.

**The drag-and-drop board is not a subsystem gap, and this README should not
pretend otherwise.** `schemaJson<MoveTaskPosition>()` produces a perfectly good
schema — `taskId`/`columnId`/`swimlaneId`/`position` required, `opId` honoured
as optional — and `DynamicForm` would render it without complaint. What is
missing is not an expressive capacity; it is that nobody moves a card by typing
four row ids into number fields and pressing Submit. The destination column,
swimlane and position are *produced by the gesture itself* (`DropArea.onDropped`
computes them from the drop's own geometry), so the form has no input left to
collect. A form renderer is the wrong shape for a direct-manipulation gesture,
and asking `morph::forms` to grow one would be asking it to become a different
kind of library. Rule 2(a) applies on those grounds — the interaction, not the
schema — which is why nothing is filed against `morph::forms` for it.

Two smaller findings fell out of rendering these forms for real, both filed
rather than folded in: every `DynamicForm` warns about `onOptionsReceived`
against any controller with no `Choice` field
([#387](https://github.com/LASTRADA-Software/morph/issues/387)), and the
renderer mistypes array-valued schema keys when `schema` is assigned as a
`QVariant` instead of bound
([#388](https://github.com/LASTRADA-Software/morph/issues/388)).

## Expected strain points

- Position renumbering under interleaved moves — the classic ordering bug;
  the strand should prevent it, the stress test must prove it.
- **Exactly-once has no owner in the stack [this rung establishes the
  pattern]**: the wire `Envelope` carries no idempotency key (only an
  ephemeral per-connection `callId`). Precision from verification: the
  durable queues *do* dedup at **enqueue time** on a non-empty
  `idempotencyKey` (SQLite partial unique index / file-queue scan) — what
  nothing provides is **replay-time exactly-once**: the *server* cannot
  recognize a replayed operation, so a reply frame lost *after* the server
  committed makes `SyncWorker` retry → double-apply.
  `MoveTaskPosition` is non-idempotent even replayed verbatim once another
  client's move interleaves. Answer: an op-id inside the action payload +
  a server-side applied-ops ledger in the model. Test with the
  fault-injection proxy ([`../TESTING.md`](../TESTING.md)): drop exactly
  the reply frame of one execute; assert exactly-once semantics.
- **Dead-letter is user-facing, not a log line**: the `SyncWorker` retry cap
  is a hard-coded 5 *cumulative* attempts, durable across restarts, and a
  reconnect flap cannot preempt a running replay — five flaky reconnects
  dead-letter every queued move while the server never saw them. Extend the
  kill-the-network demo to "kill it during each replay, five times"; wire a
  `DeadLetterSink` and show "N changes could not be synced" in the GUI.
- **Two clients' queues replaying interleaved**: assert the board invariant
  (positions dense and unique, all tasks present), not any specific final
  order.
- **Permission revocation while attached**: a member demoted mid-session
  gets their next move rejected (authorization is per-execute), but nothing
  detaches them and their `GetEventsSince` keeps returning board contents
  unless the authorizer distinguishes reads. Test that reads are cut off
  and the GUI degrades gracefully.
- **SQLite contention × pool starvation — the sharpest data-corruption test
  in the ladder**: K writing board models = K connections contending for
  SQLite's single writer; each `SQLITE_BUSY` wait pins a pool thread; a
  2–4-thread pool starves, `executeTimeout` fires "timeout" while the
  models *eventually commit anyway* → clients retry → double-apply. Test:
  pool=4, 32 boards writing concurrently, WAL on and off; measure
  throughput collapse; assert no timeout-then-committed double-apply.
- **The queue depth bound exists in the framework; this rung has not
  adopted it.** The framework gap this rung filed as morph#112 is closed:
  `IOfflineQueue` enforces a reject-newest overflow policy — `maxDepth()`
  (`include/morph/offline/offline_queue.hpp:192`) reports the configured
  capacity, `enqueue()` throws `OfflineQueueFullError` (`:80`) rather than
  evicting, and a rejection emits the `queueOverflow` counter
  (`include/morph/core/observability.hpp:34`). All three shipped queues
  take the capacity as a constructor argument and enforce it:
  `InMemoryOfflineQueue` (`offline_queue.hpp:235`, `:250`),
  `FileOfflineQueue` (`file_offline_queue.hpp:154`, `:199`), and
  `SqliteOfflineQueue` (`sqlite_offline_queue.hpp:107`, `:370-379`). The
  policy and the per-implementation ordering against dedup are specified in
  [`docs/spec/offline/offline.md`](../../docs/spec/offline/offline.md)
  ("Depth bound and overflow policy").
  What is still true here is narrower and is this rung's own gap, not the
  framework's: `BoardBridge::enableOfflineQueue` constructs its
  `SqliteOfflineQueue` from the path alone, passing no `maxDepth`, so
  kanban's queue is unbounded in the shipped build. Adopting the bound is
  not a one-line change — the enqueue happens inside a `Q_INVOKABLE` move
  handler that has no failure path today, so the rung needs a
  user-visible answer to "this change could not be queued" before it can
  turn the bound on. (Scope correction from verification, unchanged: the
  linear-scan/quadratic enqueue applies to `FileOfflineQueue` only; this
  rung's `SqliteOfflineQueue` dedups via an index. Measure depth growth on
  the SQLite queue; the 10⁴–10⁵-item enqueue-latency measurement belongs to
  `FileOfflineQueue` as the alternative-queue comparison.)
- Attachment bytes must bypass the JSON protocol; only metadata is an
  action — and the side channel is **the largest new attack surface in the
  ladder** (a hand-written HTTP server beside the WebSocket server): it
  reuses `TokenVerifier` (same secret, same clock), enforces its own
  size bound, and — since no `MORPH_BUILD_FUZZERS` harness targets HTTP
  parsing — its request parser is instead proven against a dedicated
  adversarial test (`test_attachment_server.cpp`'s garbage-input case:
  truncated/malformed/negative-length/binary-garbage requests, asserting
  only "does not crash or hang," the same bar a fuzz harness would set).
  Tests the upload dying after metadata commit (dangling row), and
  authorizes reads by the caller's project role, not bearer-token validity
  alone (a validly-signed token for a different project gets 404, same as
  a nonexistent key).

## Steps 6 and 8: implemented

Steps 6 (automation rules) and 8 (attachments) were originally deferred to a
"later" bucket (each is independently large, and the attachments answer is
duplicated at forge phase 2) — both are now implemented. Automation rules
(tag add/remove, triggered on move-to-column) are scoped to the two mutation
kinds this rung's schema actually supports; the README's own illustrative
"assign to closer" example is not implemented, since no "closer" concept
exists anywhere in this rung and inventing one would be ungrounded scope
creep — a genuine follow-up if a future rung wants it. Attachments are a
hand-written HTTP side channel next to the WebSocket server, authorizing
reads by the caller's project role (not bearer-token validity alone).
[`ledger`](../ledger) reuses this rung's cascade-journaling decision.

## Findings

Filed as GitHub issues per [`FINDINGS.md`](../FINDINGS.md) (this rung's
findings were originally `r4-001`/`r4-002` under the retired
`docs/findings/` directory):

- [#343](https://github.com/LASTRADA-Software/morph/issues/343)
  — the replay-attempt budget cannot tell an undelivered replay from a
  rejected one, so reconnect flaps dead-letter work the server never saw.
- [#344](https://github.com/LASTRADA-Software/morph/issues/344) —
  the flagship GUI was hand-built with no `morph::forms` usage and no rule-2
  justification. Seven forms now render through the shipped renderer and the
  two remaining hand-built elements each carry their written rule-2(a)
  justification (see "morph subsystems exercised").
- [#386](https://github.com/LASTRADA-Software/morph/issues/386) —
  `DynamicForm` rendered a C++ `enum class` as a free-text field, which is
  what kept `MembersView`/`RulesView`'s add-member/add-rule forms hand-built
  until it was fixed; both now render through the shipped renderer
  ([#393](https://github.com/LASTRADA-Software/morph/issues/393)).
- [#387](https://github.com/LASTRADA-Software/morph/issues/387) —
  every `DynamicForm` warns about `onOptionsReceived` on a controller with no
  `Choice` field.
- [#388](https://github.com/LASTRADA-Software/morph/issues/388) —
  `DynamicForm` mistypes array-valued schema keys when `schema` is assigned as
  a `QVariant` rather than bound.

Not filed here, deliberately: the applied-ops ledger that "Exactly-once has
no owner in the stack" (below) forces every rung to rebuild is already
morph#226, which records the pattern as past
[`IMPLEMENTATION.md`](../IMPLEMENTATION.md)'s rule-of-three threshold — this
rung's `AppliedOpRecord` is one of its occurrences, not a separate gap. The
ladder-wide sweep that prompted this README's truth pass is morph#304.

## Definition of done

- Concurrent-move stress test green under ThreadSanitizer (N=4, seeded
  scripts, run in **Local rig mode on `ThreadPoolExecutor`** — the repo's
  CI deliberately keeps Qt stacks out of the sanitizer matrix; see
  [`../TESTING.md`](../TESTING.md)).
- Exactly-once proven under reply-frame loss, in
  `tests/test_kanban_offline.cpp`'s "Dropping `MoveTaskPosition`'s reply
  frame and retrying is exactly-once" case. The fault-injection proxy it
  drives is not this rung's to build: `examples/common/testkit/fault_proxy.hpp`
  shipped with rung 0's shared infrastructure, and this rung consumes it.
- Kill the network mid-drag: client keeps queuing, reconnect replays, board
  converges; the five-flap dead-letter path surfaces in the GUI — proven by
  test (`test_board_offline_bridge.cpp`), not yet by a runnable `--seed`
  demo walkthrough (`LADDER.md`'s ladder-wide "every rung ships a `--seed`
  path" convention is not yet implemented for this rung — a real, separate
  gap, tracked but not yet closed). The offline tests assert the
  framework's own `morph::observe` metrics (`queueDepth`, reconnect
  attempt/outcome) — the observability seam gains its first app-scale
  coverage here.
- Activity stream rendered from the journal, with the cascade-journaling
  decision recorded and its divergence test green.
