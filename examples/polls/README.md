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
   Participant identity is *not* "the participant token in
   `session::Context` ... `session::Principal` carrying a capability token
   instead of a user identity", which is the easy misreading. The real
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
   is accordingly generated, stored, returned and shown when the poll is
   created — and **verified by nothing**; it is reserved for a
   later rung wanting a second, separately revocable capability level. An
   earlier draft carried a `PollModel::requireParticipant()` helper with no
   call sites; it was removed rather than left implying a check that does
   not happen.
2. **Registration identity applies to shared/keyed registration too, not
   just plain registration.** A keyed `bindModel`'s wire
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
   the `id` tie-break covers the millisecond-boundary case a bare
   `updatedAtMs > since` silently drops; still a current-state view,
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
   is a real, shipped, separately-tested per-connection token bucket. A frame
   that finds an empty bucket is **refused, and answered**: it never reaches
   `RemoteServer`, and the sender receives an `err "rate limited"` addressed
   to that frame's own `callId` (`src/qt/qt_websocket_server.cpp`'s
   `makeErr("rate limited", …)` call, and
   `include/morph/qt/qt_websocket_server.hpp`'s own doc comment). Dropping it
   silently is what would make a rate-limited call hang until the client-side
   deadline fired. This rung's own
   "run this rung's harness with `messagesPerSecond` configured ON" is a
   **test-harness configuration decision**, not new framework work — the
   client-side execute-deadline prerequisite below is what actually needs
   building; the rate limiter it must survive already exists.
6. **Any client may create a poll.** `polls.create` is a screen like any
   other. Rallly's anchor UX — an organizer creates via the main app,
   participants open a shared link — is a product convention, not a
   framework property: `CreatePoll` is unkeyed and dispatches on a plain
   `NoSharing` handler, so it never takes the keyed promote path, and
   `OpenPoll` (payload-keyed, `AllowShared`) takes the attach path, which
   never blocks.
7. **`OpenPoll::pollId` is a plain `std::string` — and that is now this
   rung's own unmigrated state, not a framework restriction.**

   This entry originally read: "`OpenPoll::pollId` (and any field a
   `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM` macro deduces a key type from)
   **must** be plain `std::string`, not a strong type", because
   `morph::model::ModelKey` "requires an exact `std::same_as<K,
   std::string>` or `std::integral<K>` match". **The framework no longer
   says that**, and stating it here asserted that the framework forbids
   what another rung does. `include/morph/core/model_key.hpp` defines
   `WrappedModelKey` alongside `RawModelKey` — "key types that *wrap* a
   `RawModelKey` — the ladder's strong ids", deduced structurally from
   `hasValue()`, an `operator*` yielding a raw key, and constructibility
   back from that raw value. `ModelKey` is the union of the two. `lims`
   keys `SampleModel` on `lims::SampleId` through exactly that path
   (`BRIDGE_MODEL_KEY(lims::SampleModel, lims::OpenSample,
   &lims::OpenSample::sampleId)`), and the widening is what lets three rungs
   carry no hand-written `ModelKeyTraits` specialisations at all.

   **Current state, stated as such:** `polls::OpenPoll::pollId` is still a
   plain `std::string`, and this rung has not been migrated to a
   `polls::PollId` strong id. Nothing prevents it any more. Until it
   happens, the field sits in the same natural-string-identity category
   `IMPLEMENTATION.md` rule 3 already carves out for URLs and titles — a
   shareable link identifier generated once server-side as an unguessable
   random token (mirroring the admin/participant tokens' own generation),
   never user-typed. That is an accurate description of a plain field, not
   a justification for keeping it plain: **migrating `pollId` to a
   `polls::PollId` strong id is named follow-up work.** Every other
   identity field this rung defines (`OptionId`, `PollEventId`) is already
   a strong type, per the usual rule.


## Server-defined screens

[`screens/polls_screens.cpp`](screens/polls_screens.cpp) declares polls' screens as UI documents (spec 4):
- **`polls.create`** is a form over `CreatePoll`, whose option list the forms engine edits. The new poll opens.
- **`polls.vote`** (`pollId`):
  - the keyed `PollModel` instance, opened by a `latest` mutation and re-opened on a backend switch;
  - the options with their counts;
  - `SubmitVotes` for the first vote, then `UpdateVotes`. The picks are the form's draft, which a reconnect keeps;
  - comments, and the activity feed, which refreshes on every vote, comment and finalization.

`morph_examples_client --app polls` runs them in process (`--db`, default `polls.db`).
## Framework prerequisites (built as part of this rung, before the app tasks that depend on them consume them)

Two items `LADDER.md`'s "Framework prerequisites" section named as blocking
this rung specifically. Both were open when this rung started and **both have
since shipped** — this rung built them, as the heading above says. They are
kept here because they explain the rung's task order, with a pointer to where
each now lives:

- **Async shared/keyed attach.** *Shipped:* `IBackend::bindModel`
  (`include/morph/core/backend.hpp`), dispatched to by
  every keyed `BridgeHandler::execute` and implemented natively by
  `QtWebSocketBackend`, which has no blocking acquire path at all (one would
  nest a `QEventLoop` and abort the page on the WASM main thread), so the very
  first `OpenPoll` a WASM tab makes is not blocked on the framework. Built as
  this rung's first framework-level task. Not a pair of optional non-blocking
  twins a backend returns `true` or `false` from with the caller falling back to
  the synchronous verb on `false`: there is one verb, so there is no opt-in to
  decline and no fallback path, and the synchronous verbs survive only as what
  the *default* `bindModel` runs.
- **Client-side execute deadline.** *Shipped:*
  `Bridge::setExecuteDeadline` (`include/morph/core/bridge.hpp`), specified in
  `docs/spec/core/completion.md`. Without it a genuinely hung server blocked
  the calling `Completion` forever. (A frame refused by `messagesPerSecond`
  does *not* belong here: the transport answers it with an
  `err "rate limited"`.)
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
   have no account, and there is no participant *authentication* of any
   kind. The identity that matters is the action's own free-text
   `participantName`, scoped to the poll — knowing the `pollId` is the
   capability, and `CreatePollResult::participantToken` is verified by
   nothing (design decision 1 above, and `PollModel`'s own "What is
   actually gated, stated exactly" doc comment). `UpdateVotes`,
   `AddComment`.
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
- **The framework journal, as audit only.** `FileActionLog` is installed
  process-wide (`app/app.hpp`) and every mutating action is `Loggable`, which
  is the whole of what the journal does here. The rung's *user-facing*
  vote-change history and undo are **not** built on it: `SessionLog::undoLast()`
  is principal-blind and returns a detached holder no API can install into a
  live shared instance, so `UndoLastVoteChange` is a compensating action over
  this rung's own `db::VoteHistoryRecord` table instead — design decision 3
  above, which resolved this before implementation started. The original
  "journal as user feature: vote-change history and undo" framing was the
  ladder's pre-implementation guess and is what design decision 3 overturned.

## Expected strain points

- **WASM + shared handlers**: the *synchronous* shared/keyed attach path
  (a blocking `QtWebSocketBackend::bindModel`) nests an event loop and **aborts the
  page on the WASM main thread**, so a WASM tab's very first `OpenPoll` must
  not go through it. This is no longer a framework prerequisite:
  `IBackend::bindModel` carries the keyed acquire without blocking
  (see § Framework prerequisites above). Still worth running the "several WASM
  tabs" demo literally — the non-blocking path's *behaviour in a browser* has
  never been observed, only its compilation.
- **The polling helper must own a client-side timeout**: an unwrapped poll
  call against a hung server hangs its completion forever, which is what
  `Bridge::setExecuteDeadline` now exists to bound. A rate-limited server no
  longer drops frames silently — it answers them with `err "rate limited"`
  — so the limiter is no longer the case that motivates the
  timeout; a genuinely unresponsive server is. **Done:** this rung's harness
  does run with `messagesPerSecond` configured ON (a polling app is the abuse
  case the limiter exists for), and
  `tests/test_shared_instance_lifecycle.cpp`'s "The real rate limiter refuses
  an over-budget call without hanging it" proves the *answered* property end
  to end — the over-budget call settles on its own, with the deadline no
  longer being what saves it.
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
  per-participant local rendering is client logic) — a good dual-mode +
  WASM-parity screen test.
- **Shared-instance churn soak** (framework-grade, promoted to
  `tests/soak/`): threads racing register-or-attach / deregister /
  closeConnection / execute on one key under TSan — never two live
  instances for a key, attach counts never leak, every completion resolves.

## Definition of done

- Live demo: one organizer + three participant clients on the remote
  backend, votes converging via polling; finalize locks the poll everywhere.
  The server serves the screens, and the generic client opens them against
  it (`morph_examples_client --server ws://127.0.0.1:8767`). Beneath that,
  `tests/test_app.cpp` drives the remote backend end to end, the screen
  tests drive both screens on the harness, and
  `tests/test_shared_instance_lifecycle.cpp` covers multi-handler
  convergence on one shared poll. A four-client demo run by hand is not
  claimed here.
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
  design decision 4 above predicts.
- The activity feed refreshes on every vote, comment and finalization
  (`refreshOn`), so a client sees other participants' votes without a
  hand-written poller.
