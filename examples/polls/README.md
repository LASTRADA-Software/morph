# polls — rung 3 of the [application ladder](../LADDER.md)

**Status: shipped** — every rung-3 task is complete; see
[Definition of done](#definition-of-done) for what that does and does not
mean, and ["The client, and its known gaps"](#the-client-and-its-known-gaps--stated-rather-than-smoothed-over)
for what the shipped client cannot reach (there is no native desktop entry
point at all, so the live multi-client demo the DoD asks for has not been
run; the WASM client is written and CI-gated but has never been compiled
here). Design decisions below were resolved in writing before implementation
began, per [`LADDER.md`](../LADDER.md)'s discipline rule.

## Design decisions (resolved before implementation)

Research done ahead of writing this rung's implementation plan surfaced two
places where this README's own framing does not match the framework as it
actually exists, plus decisions the README named but left open. Recorded
here, in writing, before any task starts — the discipline rule this ladder
runs on.

1. **`session::Principal` is not a capability-token mechanism — correction.**
   This README originally described participant identity as "the participant
   token in `session::Context` ... `session::Principal` (added in #34)
   carrying a capability token instead of a user identity." The real
   `session::Principal` (`docs/spec/session/session.md`) is a client-side,
   `Bridge`-scoped UI cache populated *after* login from server-returned
   data — it has no wire representation and does not participate in
   dispatch authorization at all ("Setting a `Principal` does not affect
   `Context` or dispatch behavior in any way"). There is no existing
   framework mechanism for a bare shared-secret-per-entity capability token.
   **Resolved shape**: `Context::token` carries the poll's admin secret;
   `PollModel::execute()` verifies it itself, by comparing against the poll
   row's stored `adminToken` column — the same shape as a
   `SigningAuthorizer`-verified token, but hand-verified in the model rather
   than by an `IAuthorizer`, since no framework authorizer verifies bare
   shared secrets. `Context::principal` carries the free-text
   `participantName` `SubmitVotes` already names as an action field.
   **`UndoLastVoteChange`'s "principal-scoped" therefore means keyed on
   `(pollId, participantName)`**, not a framework-authenticated identity.

   **What shipped, stated exactly** (corrected after the final whole-branch
   review found this section overclaiming): `FinalizePoll` is the *only*
   token-gated action in `PollModel`. `SubmitVotes`, `UpdateVotes`,
   `AddComment`, `UndoLastVoteChange`, `GetPollState`, `GetEventsSince` and
   the keyed `OpenPoll` attach are all reachable by anyone who can name the
   `pollId`, with no token check at all — which is the intended design, not
   a gap: `pollId` is 16 bytes of `std::random_device` entropy in base64url,
   so knowing it *is* the capability (design decision 2 says as much:
   "attaching to a poll by id is meant to be as open as knowing the link").
   A participant gate would add no authority in any case, since one
   participant token is minted per *poll*, not per participant, and every
   voter would present the same secret. `CreatePollResult::participantToken`
   is accordingly generated, stored, returned and shown by
   `CreatePollView.qml` — and **verified by nothing**; it is reserved for a
   later rung wanting a second, separately revocable capability level. An
   earlier draft carried a `PollModel::requireParticipant()` helper with no
   call sites; it was removed rather than left implying a check that does
   not happen.
2. **Registration identity applies to shared/keyed registration too, not
   just plain registration.** `registerModelShared`/`attachModel`'s wire
   form is still a `register` envelope (`docs/spec/core/shared_instances.md`:
   "`register` grows `primary` and `shared`" — additive, same envelope
   kind), and `wire::makeRegisterShared` carries the caller's session, just
   like plain `wire::makeRegister` now does. So `authorizeRegister` *could*
   gate `OpenPoll{pollId}` (the keyed attach) by admin/participant identity
   — this rung deliberately doesn't. **Resolved shape**: `authorizeRegister`
   stays unconditionally permissive for `PollModel` (attaching to a poll by
   id is meant to be as open as knowing the link, by design — this is not a
   regression, and not something the framework forces), and the one action
   that must distinguish admin from participant (`FinalizePoll` — in the
   shipped rung, the only one that does) re-checks the caller's token
   against the poll row's own `adminToken` column inside
   `PollModel::execute()`. This mirrors rung 2's shape for a different
   reason than it originally did: bookmarks' `authorizeInstance` is now
   genuinely enforcing but checks *instance* ownership, which `PollModel`
   (shared/keyed, not per-caller-owned) has no equivalent of at all — so the
   model's own re-check was never standing in for a defeated hook, it is
   simply the only layer that could ever express this distinction.
3. **Undo is entirely app-level; the framework journal contributes nothing
   to it.** `SessionLog::undoLast()` (`docs/spec/journal/journal.md`) "pops
   the most recent entry and replays the remainder against a fresh,
   detached model instance" — no principal filtering, and the returned
   holder cannot be installed into a live shared instance. This is not a
   bug to work around at the call site; the framework's own journal design
   record states plainly that "reversing a checkpointed action durably
   needs a compensating action" at the app level. **Resolved shape**:
   `PollModel` owns a small per-`(pollId, participantName)` vote-history
   table of its own (not the framework's `FileActionLog`/journal), and
   `UndoLastVoteChange` reads and reverses the caller's own most recent
   entry from it via ordinary mutation. The framework journal remains wired
   for audit-trail purposes (same two-independent-write default every
   single-row action in rung 2 used) but is orthogonal to undo.
4. **`GetEventsSince` is genuinely new work, not a `GetChangesSince` port.**
   Rung 2's `GetChangesSince` is a timestamp-diffed-current-state view
   (`WHERE updatedAtMs > since OR (updatedAtMs = since AND id > lastId)` —
   the `id` tie-break is issue #43's fix for the millisecond-boundary case a
   bare `updatedAtMs > since` can silently drop; still a current-state view,
   returning full current rows, not a log) — not the Zulip append-only
   event-log pattern this rung's own "morph subsystems exercised" section
   correctly calls for. **Resolved shape**: a genuine `poll_events` table
   (sequence id + payload per mutation), with a **table-wide monotonic
   autoincrement sequence id, not a timestamp** — rung 2's
   `BulkEdit`/`MergeTags` idempotency-key fix rounds (Tasks 8/9) both hit
   millisecond-collision bugs from timestamp-keyed uniqueness; an
   autoincrement primary key sidesteps that class of bug entirely, and
   the README's own requirement ("a client holding `lastEventId=42`... sees
   nothing new forever, silently") is exactly what a durable, never-reused
   sequence id guarantees. **The "and/or epoch token" alternative the
   original strain-point text offered is resolved to: not needed.** Durable
   SQLite persistence of the event log alone already closes the gap
   (an in-memory-only list dying at refcount zero) the epoch token existed
   to catch; a poll's row-level data plus its event table both survive
   instance rebirth by construction once persisted, so a reborn instance
   naturally continues the same global sequence with no separate epoch
   concept to design, test, or explain. `GetEventsSince{lastEventId}`
   returns every event with `id > lastEventId` for the poll, oldest first;
   an empty poll's-worth of history (a truly stale cursor, e.g. `lastEventId`
   far beyond the table's current max) is handled the same way any
   over-advanced cursor is — see the model task for the exact response
   shape.
5. **`messagesPerSecond` is not a framework gap — already implemented.**
   `QtWebSocketServerConfig::messagesPerSecond` (`docs/spec/core/backend.md`)
   is a real, shipped, separately-tested per-connection token bucket; a
   frame that finds an empty bucket is dropped silently. This rung's own
   "run this rung's harness with `messagesPerSecond` configured ON" is a
   **test-harness configuration decision**, not new framework work — the
   client-side execute-deadline prerequisite below is what actually needs
   building; the rate limiter it must survive already exists.
6. **`CreatePoll` runs from the native/desktop client only — never from a
   WASM tab.** Closing framework prerequisite #1 (below) discovered a
   second, narrower gap it does not close:
   `Bridge::assignHandlerPrimary`'s promote step (filing a freshly-created
   shared instance into the directory under its generated key) has no
   async path — `IBackend::assignPrimary` is still a synchronous `sendSync`
   on `QtWebSocketBackend`, with no `assignPrimaryAsync` anywhere in the
   tree. `CreatePoll` is a result-keyed *creating* action (the instance
   doesn't exist until the call returns and names it), so a WASM tab
   dispatching it would still abort the page at the promote step — filed as
   the *assignPrimary has no async path* finding (recorded on the
   `application-ladder` branch; not present in `docs/findings/` here).
   **Resolved
   shape**: this matches Rallly's own anchor UX exactly (an organizer
   creates via the main app/site; participants open a shared link in
   whatever browser tab they have), so the rung's own design already wants
   this split — `CreatePoll` is native-client-only by design, not merely
   worked around; every WASM tab's role is strictly the participant-attach
   story (`OpenPoll`, payload-keyed, fully covered by the prerequisite work
   below), never poll creation.
7. **`OpenPoll::pollId` (and any field a `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM`
   macro deduces a key type from) must be plain `std::string`, not a strong
   type.** `morph::model::ModelKey`'s concept (`include/morph/core/model_key.hpp`)
   requires an exact `std::same_as<K, std::string>` or `std::integral<K>`
   match — a wrapper type like rung 1/2's `PasteId`/`BookmarkId` does not
   satisfy it, since the macro deduces `PrimaryKey` directly from the
   member's own declared type via `MemberTypeOf`. This is a genuine,
   narrow exception to `IMPLEMENTATION.md` rule 3 ("only `std::string` is a
   permitted plain type"), not a violation of it: `pollId` is a shareable
   link identifier, the same natural-string-identity category rule 3
   already carves out for URLs and titles — it is generated once
   server-side as an unguessable random token (mirroring the admin/
   participant tokens' own generation), never user-typed, and never
   confused with an ordinary integer id precisely because it *is* a
   string. Every other identity field this rung defines (`OptionId`, the
   event log's sequence id) is never the target of a keying macro and
   stays a strong type, per the usual rule.

## Framework prerequisites (built as part of this rung, before the app tasks that depend on them consume them)

Two items `LADDER.md`'s "Framework prerequisites" section names as blocking
this rung specifically, both confirmed still open by direct inspection of
the current framework source (not assumed from the ladder doc alone):

- **Async shared/keyed attach.** `IBackend::registerModelAsync`'s own doc
  comment (`include/morph/core/backend.hpp`) explicitly scopes itself out of
  `registerModelShared`/`attachModel`, which remain synchronous (nest a
  `QEventLoop`) — the very first `OpenPoll` a WASM tab makes aborts the
  page. Built as this rung's first framework-level task, mirroring
  `registerModelAsync`'s existing opt-in/fallback shape (backend returns
  `true` and later invokes exactly one callback, or returns `false` and the
  caller falls back to the synchronous path unaffected) so every backend
  that has not opted in keeps its current behavior.
- **Client-side execute deadline.** No timeout exists anywhere on a
  `Completion` today — a genuinely hung server blocks the calling `Completion`
  forever. (A frame refused by `messagesPerSecond` used to belong here too; it
  no longer does, since the transport now answers it with an `err "rate
  limited"` — morph#225.)
  `Completion<T>::state()` already exposes the underlying
  `CompletionState`, and `CompletionState::setException` is
  idempotent-guarded (`if (ready) return;`), so the fix needs no
  `Completion`/`CompletionState` API changes — only a new client-side timer
  that races a delayed `setException(ClientTimeoutError)` against the real
  reply. Built as this rung's second framework-level task, before the
  polling helper (`GetEventsSince` on a client timer) that is untestable
  without it.

Group scheduling polls, Doodle-style: create a poll with
candidate dates, send one link to participants, everyone votes yes / if-need-be
/ no, the organizer finalizes a date. The first genuinely *concurrent
multi-client* rung: many participants converge on one shared poll instance.

## Reference implementations

- **[Rallly](https://github.com/lukevella/rallly)** (TypeScript, Next.js +
  tRPC + Prisma, AGPL) — the anchor. Its tRPC procedures are already typed
  request/response actions, and the codebase verifiably contains **no
  websockets/SSE/socket.io at all**: concurrent voters see each other's votes
  on refetch. It is living proof this category needs no push. Data model to
  copy (from `packages/database/prisma/models/`): `Poll`, `Option`,
  `Participant`, `Vote (yes|ifNeedBe|no)`, `Comment`. Ignore the SaaS
  billing/licensing packages entirely.
- [Framadate](https://framagit.org/framasoft/framadate/framadate) — archived;
  do not use.

## What to implement

`PollModel` keyed by poll id — **the shared-instance showcase**:

```
BRIDGE_MODEL_KEY(PollModel, OpenPoll, &OpenPoll::pollId);
BridgeHandler<PollModel, AllowShared> handler{bridge, &ui};
```

Actions, in build order:

1. `CreatePoll { title, options[] }` → admin + participant link tokens.
2. `OpenPoll { pollId }` (the keyed action), `GetPollState {}`.
3. `SubmitVotes { participantName, votes[] }` — **anonymous**: participants
   have no account; the participant token in `session::Context` is the whole
   identity. `UpdateVotes`, `AddComment`.
4. `FinalizePoll { optionId }` — admin-token-gated state transition; the poll
   becomes read-only.
5. `UndoLastVoteChange` — user-facing undo, **redesigned per review**: it
   must be *principal-scoped* ("undo *my* last change") and implemented as a
   **compensating action**, not `SessionLog::undoLast()` — which (a) pops
   the newest entry *regardless of principal* (A's undo would kill B's
   vote), and (b) returns a fresh **detached** holder that no API can
   install into the live server registry, so replay-undo cannot mutate a
   shared instance at all. Write the interleaving test first (A votes, B
   votes, A undoes → assert whose vote died) — its outcome is the rung's
   headline design record.
6. **`GetEventsSince { lastEventId }`** — this rung's framework-level
   deliverable: the Zulip-pattern generic polling action (see below).
   **Event storage, resolved (design decision 4 above)**: shared instances
   are destroyed *immediately* at refcount zero, so an in-instance event
   list dies the moment all tabs briefly close (a link shared in chat
   produces exactly this) — solved by persisting events to a genuine
   `poll_events` SQLite table keyed by a table-wide monotonic autoincrement
   sequence id, not an epoch token: a reborn instance reads the same
   durable table and continues the same sequence, so a client holding
   `lastEventId = 42` simply gets every real event after 42, rebirth or
   not. Test: attach N, mutate, detach all (verify destruction via
   `instances()`), attach again, poll with the pre-death cursor.

Persistence: SQLite tables mirroring Rallly's Prisma models, plus the event
log table above.

## morph subsystems exercised

- **Shared instances end-to-end**: N clients (desktop + several WASM tabs)
  attach to one server-side `PollModel` instance; refcounted lifetime when
  tabs close; `handler.instances()` for an organizer dashboard.
- **Anonymous principals**: no framework identity at all — `Context::token`
  carries the poll's admin-or-participant secret, hand-verified by
  `PollModel::execute()` itself against the poll row's own columns (design
  decision 1 above; there is no framework `IAuthorizer` for bare shared
  secrets, so this rung does not add one).
- **Event polling — the pattern the rest of the ladder reuses.** morph has no
  server push and in-process-only subscriptions, so remote clients must ask.
  Implement the [Zulip events-system pattern](https://zulip.readthedocs.io/en/stable/subsystems/events-system.html)
  in miniature: every mutation appends to a per-poll event list (sequence id
  + payload); clients poll `GetEventsSince` on a timer and apply increments;
  a stale client falls back to `GetPollState`. Zulip proves an entire chat
  product ships on exactly this; here it debuts at toy scale.
- **Journal as user feature**: vote-change history and undo, not just audit.

## Expected strain points

- **WASM + shared handlers may not work at all today [framework
  prerequisite]**: the shared/keyed attach path
  (`registerModelShared`/`attachModel`) is synchronous and nests an event
  loop — which **aborts the page on the WASM main thread**;
  `registerModelAsync` covers only the plain path. A WASM tab's very first
  `OpenPoll` hits this. Run the "several WASM tabs" demo literally, before
  any polling logic exists; schedule async attach as a framework issue (see
  [`../LADDER.md`](../LADDER.md) § Framework prerequisites).
- **The polling helper must own a client-side timeout**: a rate-limited
  server drops frames silently and morph has no execute deadline — an
  unwrapped poll call hangs its completion forever. Every later rung
  inherits this helper; get it right here — and **run this rung's harness
  with `messagesPerSecond` configured ON** (a polling app is the abuse case
  the limiter exists for; the helper's timeout is untested until the
  limiter actually drops its frames).
- Poll-interval latency: two voters editing simultaneously see each other
  only on the next tick — measure and document acceptable intervals.
- `subscribe<R>` fan-out is in-process only: verify the documented limit that
  two *remote* clients do not see each other's results without polling, and
  show `GetEventsSince` closing the gap. This rung is also the **first test
  anywhere of `AllowShared` over the real WebSocket transport** — the
  framework itself gains coverage here.
- **Poisoned-instance attach**: opening a stale/mistyped poll link exercises
  the documented shared-instance failure modes (half-hydrated instance,
  eviction only on *next* attach, the failing handler not self-healing);
  also race two attaches against a failing first hydration.
- **Duplicate `SubmitVotes` on retry** must not double-count: the strand
  serializes but does not dedup — participant-token + option uniqueness is
  a model invariant, tested under retry.
- A vote in flight (or queued offline) when `FinalizePoll` lands must
  dead-letter with a user-visible outcome, not vanish.
- Timezone display of candidate dates (`morph::time` is UTC-only;
  per-participant local rendering is GUI logic) — a good dual-mode +
  WASM-parity presenter test.
- **Shared-instance churn soak** (framework-grade, promoted to
  `tests/soak/`): threads racing register-or-attach / deregister /
  closeConnection / execute on one key under TSan — never two live
  instances for a key, attach counts never leak, every completion resolves.

## Definition of done

- Live demo: one organizer + three participant clients on the remote
  backend, votes converging via polling; finalize locks the poll everywhere.
  **Not satisfied.** This rung ships no native desktop entry point
  (`examples/polls/gui/main.cpp` does not exist — no task in its plan wrote
  one), and its only GUI binary, `gui_wasm/main_wasm.cpp`, has never been
  compiled for want of an Emscripten toolchain here. Nothing in this rung has
  therefore been run as an application against a real server. What *is*
  verified is every layer beneath that: `tests/test_app.cpp` drives the
  remote backend end to end, `tests/test_poll_qml_bridges.cpp` drives the
  whole QML-facing adapter including one real `EventPoller` tick, and
  `tests/test_shared_instance_lifecycle.cpp` covers multi-handler
  convergence on one shared poll. Writing the desktop entry point and
  running the demo is named follow-up work, not a claim made here.
- Principal-scoped undo restores the caller's previous vote via a
  compensating action, verified by the two-principal interleaving test --
  "principal-scoped" here means keyed on `(pollId, participantName)` per
  design decision 1, not a framework-authenticated identity; the
  `SessionLog::undoLast` limitation is documented in the rung's design
  record.
  **Confirmed (Task 8):** the interleaving test (A votes, B votes, A undoes)
  passes against a real SQLite-backed `PollModel` -- A's undo restores only
  A's prior (no-vote) state via `UndoLastVoteChange`, and B's vote survives
  completely untouched, the exact outcome `SessionLog::undoLast()`
  (principal-blind, pops the newest entry regardless of who made it) could
  never have produced.
- Event log survives full detach/reattach (instance rebirth) and a stale
  cursor triggers a clean full resync, verified by test.
  **Confirmed (Task 9):** a `BackendRig`-driven test attaches two
  `AllowShared` `PollModel` handlers to the same poll (`instances()` shows
  one live key), drops every handler naming that poll, and confirms via a
  fresh handler's own `instances()` that the shared instance is genuinely
  gone (empty directory, not just "no crash"). A brand-new handler then
  reattaches via `OpenPoll` and calls `GetEventsSince` with the pre-death
  cursor: it gets exactly the events written after that cursor, including
  ones recorded before the instance died -- confirmed independently against
  the real on-disk SQLite file (`sqlite3` inspection of `poll_events`), not
  just the in-memory assertions. No epoch token was needed, exactly as
  design decision 2 above predicts.
- The event-polling helper (with its client-side timeout) is factored so
  [`kanban`](../kanban) can lift it.
  **Confirmed (Task 15):** `morph::ladder::gui::EventPoller<EventT, EventIdT>`
  lives in `examples/common/gui/event_poller.hpp`, not in this rung — it
  names no `polls::` type, taking its event and cursor types as template
  parameters and its backend reach as a caller-supplied `Dispatch` closure,
  which is what lets kanban wire its own feed without re-deriving the
  retry-vs-fatal decision tree. Its behaviour is covered by
  `examples/common/testkit/test_event_poller.cpp` against a synthetic
  dispatch, independently of `polls` entirely; `PollBridge::startPolling` is
  merely its first consumer.

## The client, and its known gaps — stated rather than smoothed over

Task 16 built the GUI shell: `gui_lib/poll_schemas.hpp` (the
`{actionType: schema}` document), `gui_lib/poll_forms_controller.{hpp,cpp}`
(the one `BridgeHandler<PollModel, AllowShared>` every already-open-poll
action shares), `gui_lib/poll_qml_bridges.{hpp,cpp}` (`PollBridge`, the one
QML-facing adapter, wrapping both `PollFormsController` and `PollPresenter`),
and `gui/qml/{Main,CreatePollView,VoteView}.qml`. Three of `PollModel`'s nine
actions are genuinely schema-driven (`AddComment`, `FinalizePoll`,
`UndoLastVoteChange` — all scalar-field DTOs, rendered by the shipped
`MorphForms` `DynamicForm`); the rest are dedicated `PollBridge` invokables,
for the reasons below.

**No `gui/main.cpp`, still.** Task 16's brief scoped the desktop client's
entry point out (`gui/*.cpp` is absent from its file list), no later task in
this rung's plan added one, and the branch's final whole-branch review chose
to name the gap rather than close it. Wiring `ladder_polls_gui` together, and
with it the live end-to-end organizer-plus-participants demo the Definition
of Done above asks for, is follow-up work. The one entry point that *does*
exist is `gui_wasm/main_wasm.cpp` (Task 18), the browser client — which
cannot create polls (`nativeClient: false`) and has never been compiled.
Today `ladder_polls_qml`/`ladder_polls_gui_lib` build and
are proven by the offscreen engine-load smoke test
(`tests/test_gui_qml_smoke.cpp`) and the adapter-layer suite
(`tests/test_poll_qml_bridges.cpp`), including one real end-to-end
`EventPoller` tick (`PollBridge's EventPoller applies a live event and
refreshes state, end to end`) — but nothing here has yet been run as an
actual desktop application against a real server.

Known gaps:

- **`DynamicForm` has no control for a JSON `array` field** (finding 031,
  discovered during rung 2's own GUI shell). `CreatePoll::options` is
  `std::vector<CreatePollOption>` and hits this directly, so `CreatePoll` is
  excluded from `poll_schemas.hpp`'s document entirely and driven instead by
  `gui/qml/CreatePollView.qml`'s own hand-written option-label list editor
  (add/remove rows), submitted through `PollBridge::createPoll(title,
  optionLabels)` — the same shape rung 2's `BulkEdit` workaround established.
  **The same finding also blocks `SubmitVotes`/`UpdateVotes`**, whose one
  required field beyond `participantName` is `std::vector<OneVote>` — not
  called out by name in finding 031 itself (rung 2 has no array-of-struct
  DTO field to have found it with), but the identical rendering gap. Both are
  excluded from the schema document too and driven by
  `gui/qml/VoteView.qml`'s hand-rolled per-option Yes/If-need-be/No radio
  picker, via `PollBridge::submitVotes`/`updateVotes`.
- **`BridgeHandler::executeJson` silently skips the payload-keyed attach
  step on an `AllowShared` handler** — a new finding this task surfaced,
  filed as the *executeJson skips payload-keyed attach for AllowShared
  handlers* finding (recorded on the `application-ladder` branch; not present
  in `docs/findings/` here).
  `ActionExecuteRegistry::registerAction<Model, Action>` (`morph/core/bridge.hpp`)
  closes its stored executor over the *plain* `BridgeHandler<Model>` overload
  of `execute<Action>()`, regardless of the real handler's `Sharing`
  argument, so `kShared` resolves `false` at that call site no matter what —
  dispatching `OpenPoll` (this rung's one payload-keyed action) through
  `executeJson` on an `AllowShared` handler therefore never attaches; it
  dispatches straight to `executeVia` with whatever `currentId` the binding
  already has, failing "handler not bound" on a fresh handler. `OpenPoll` is
  therefore never routed through `submitIfValid`/`executeJson` anywhere in
  this client — `PollFormsController::openPoll(pollId)` calls the templated
  `execute<OpenPoll>()` directly instead, which resolves the real
  `AllowShared` branch at compile time. Every other action `PollModel`
  registers is unkeyed, so it dispatches identically either way and this gap
  never bites them — but it is a real, general framework gap for any future
  keyed `AllowShared` model that tries to schema-drive its own attach action.
- **`PollFormsController` cannot be a verbatim copy of
  `bookmarks::gui::BookmarkFormsController`'s per-model-handler shape.**
  Every one of bookmarks' three models is plain (`NoSharing`), so which
  handler object serves a given call never matters there. `PollModel` is
  `AllowShared` and keyed: an `AllowShared` handler starts unattached and
  only joins the poll's shared instance the first time a payload-keyed
  action dispatches through *that specific handler object* — every other
  action on the same poll must reuse that exact handler. `PollFormsController`
  therefore owns exactly one `BridgeHandler<PollModel, AllowShared>`, shared
  by `openPoll`/`getPollState`/`submitVotes`/`updateVotes`/`getEventsSince`
  and the three schema-driven actions alike, rather than one handler per
  concern. See that class's own doc comment for the full reasoning, and
  `tests/test_poll_qml_bridges.cpp`'s "threads openPoll's attach through
  every later action on the same poll" case for the regression proof.
- **The event-driven results display resyncs on every applied event rather
  than applying a true increment.** `PollEvent{id, kind, summary}` carries no
  vote-tally delta — only a human-readable summary — so
  `PollBridge::onEventApplied` relays it to `eventReceived` (for a live
  activity log) and separately schedules a debounced `refresh()`
  (`GetPollState`) to update the actual tallies. This is simple and correct
  but is one full state refetch per tick that had at least one event, not
  the increment-application the Zulip pattern's `README`-level description
  suggests — acceptable at this rung's toy scale, worth reconsidering if a
  later rung's event volume makes it not.
- **The `CreatePoll` screen is native-client-only by gate, not by absence.**
  `gui/qml/Main.qml`'s `nativeClient` property (default `true`) hides — not
  merely disables — the one button that reaches `CreatePollView.qml` (see
  design decision 6 above for why `CreatePoll` must never run from a WASM
  tab). `gui_wasm/main_wasm.cpp` (Task 18) is what flips it, passing
  `nativeClient: false` as an initial property. The consequence, stated
  plainly: **the browser client cannot create a poll at all.** A WASM
  participant either follows a `?poll=<id>` link or pastes a poll id on the
  landing screen; some organizer on some other client had to create it, and
  today no such client exists (see the next bullet).
- **No native desktop entry point exists, so nothing here has been run as an
  application.** There is no `examples/polls/gui/main.cpp`; no task in this
  rung's plan wrote one, and this fix round deliberately did not add one
  either. `gui_wasm/main_wasm.cpp` is the only GUI client binary this rung
  ships, and it has never been compiled (no Emscripten toolchain here — the
  `ladder-wasm` CI job is a compile gate). Writing `gui/main.cpp` and running
  the organizer-plus-participants demo is named follow-up work.
- **`Bridge::setExecuteDeadline` used to be unusable from a browser tab, and
  the fix is CI-compile-verified only.** `EventPoller`'s constructor calls it
  unconditionally, and it lazily builds a `TimeoutScheduler`, which spawned a
  `std::thread` — impossible in the `wasm_singlethread` Qt build these
  clients target. `include/morph/core/timeout_scheduler.hpp` now selects a
  browser-timer (`emscripten_async_call`) build of itself under
  `__EMSCRIPTEN__ && !__EMSCRIPTEN_PTHREADS__`, so deadlines still fire, on
  the main thread. Neither the original hazard nor the fix has been observed
  on a real Emscripten build; see that header's `@file` comment and
  `docs/spec/core/completion.md`.
- **No admin-token persistence.** `PollBridge::setAdminToken` installs the
  token as the shared `Bridge`'s default session for the remainder of the
  process; nothing writes it to disk or a keychain. Reopening the app (or
  the organizer coming back later) needs the admin token pasted in again —
  `CreatePollView.qml` shows it once, selectable, and says so.
- **The offscreen QML smoke test proves loading, not behavior** — same scope
  note as rung 2's own smoke test (`tests/test_gui_qml_smoke.cpp`'s own
  header comment). The behavioral half is `tests/test_poll_qml_bridges.cpp`
  plus `tests/test_poll_presenter.cpp`.
