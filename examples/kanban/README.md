# kanban — rung 4 of the [application ladder](../LADDER.md)

**Status: planned — committed scope, and the ladder's designated
showcase.** A multi-project kanban board: columns, swimlanes, tasks,
drag-and-drop moves, WIP limits, comments, per-project roles, an activity
stream, and automation rules. The mid-tier flagship: the first app where
concurrency, authorization, offline, and the journal are all load-bearing at
once. As the one polished showcase (round-7 audience decision), this rung
alone may spend effort on visual presentation; every other rung stays
deliberately unstyled.

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
   cascades into further model mutations. **Review sharpened the decision —
   both naive answers diverge on replay**: unjournaled cascades make replay
   incomplete, but journaled cascades *double-apply* when replay re-executes
   the trigger and the rules re-fire. Choose one of: journal cascades with
   a causal parent-id and suppress rule evaluation during replay, or don't
   journal cascades and require rule determinism (which breaks when rules
   are edited — see [`ledger`](../ledger)'s rule-versioning). State the
   choice in writing with a divergence test; note morph today provides
   neither replay-mode signaling nor causal links [framework gap].
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
   replay on reconnect; conflicts (column deleted while offline) surface
   through the model's `onBackendChanged` reconciliation, not silently.
8. Task attachments — first blob answer: bytes over a side channel (plain
   HTTP endpoint next to the WebSocket server), metadata through actions.

## morph subsystems exercised

Strand ordering under real contention (2), typed server-side validation (3),
authorization at Kanboard's granularity (4), journal-derived activity + undo
(5, 6), the full offline stack (7), shared board instances throughout.

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
- **Offline queue growth is unbounded**: no depth bound exists on any
  shipped queue — define an overflow policy
  [framework gap, filed as morph#112]. (Scope
  correction from verification: the linear-scan/quadratic enqueue applies
  to `FileOfflineQueue` only; this rung's `SqliteOfflineQueue` dedups via
  an index. Measure depth growth on the SQLite queue; the 10⁴–10⁵-item
  enqueue-latency measurement belongs to `FileOfflineQueue` as the
  alternative-queue comparison.)
- Attachment bytes must bypass the JSON protocol; only metadata is an
  action — and the side channel is **the largest new attack surface in the
  ladder** (a hand-written HTTP server beside the WebSocket server): it
  must reuse `TokenVerifier` (same secret, same clock), enforce its own
  size bound, and its request parser joins the fuzz corpus. Test the
  upload dying after metadata commit (dangling row).

## Deferred within this rung (delivery review)

Steps 6 (automation rules) and 8 (attachments) are each independently
large, and the attachments answer is duplicated at forge phase 2. They move
to a "later" bucket: steps 1–5 + 7 deliver every DoD bullet except the
cascade divergence test — and [`ledger`](../ledger) needs only the
cascade-journaling *decision*, which is written from a spike, not from a
full rules engine.

## Definition of done

- Concurrent-move stress test green under ThreadSanitizer (N=4, seeded
  scripts, run in **Local rig mode on `ThreadPoolExecutor`** — the repo's
  CI deliberately keeps Qt stacks out of the sanitizer matrix; see
  [`../TESTING.md`](../TESTING.md)).
- Exactly-once proven under reply-frame loss (fault-injection proxy in the
  testkit by this rung).
- Kill the network mid-drag: client keeps queuing, reconnect replays, board
  converges; the five-flap dead-letter path surfaces in the GUI; demo
  scripted. The offline tests assert the framework's own
  `morph::observe` metrics (`queueDepth`, reconnect attempt/outcome) — the
  observability seam gains its first app-scale coverage here.
- Activity stream rendered from the journal, with the cascade-journaling
  decision recorded and its divergence test green.
