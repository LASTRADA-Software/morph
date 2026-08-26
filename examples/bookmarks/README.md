# bookmarks — rung 2 of the [application ladder](../LADDER.md)

**Status: shipped** — every rung-2 task is complete; see
[Definition of done](#definition-of-done) for what that does and does not
mean, and ["The client, and its known gaps"](#the-client-and-its-known-gaps--stated-rather-than-smoothed-over)
for what the shipped client cannot reach (pagination is not reachable from the
GUI, and a fetched title only appears on a manual refresh; the native stack is
verified end to end, the WASM client is written and CI-gated but has never
been compiled here). A multi-user bookmark manager: save URLs, tag them,
search, bulk-edit, archive, share with other users. The first "small but real"
app: several related entities, real authorization, and the first background
jobs.

## Running it

```bash
# One-time configure (Qt 6.5+, an ODBC SQLite3 driver, MORPH_BUILD_FORMS_QML
# for the schema-driven forms):
cmake -S . -B build -G Ninja \
    -DMORPH_BUILD_QT=ON -DMORPH_BUILD_FORMS_QML=ON \
    -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=bookmarks

# Server (owns the database, the signing secret, the action journal, the
# metadata-fetch worker and the outbox relay). The secret is required and has
# no default: it signs every token the server mints and verifies every token
# it is shown, so a built-in fallback would be a published signing key.
BOOKMARKS_TOKEN_SECRET="pick-something-real" \
BOOKMARKS_DB="DRIVER=SQLite3;Database=bookmarks.db;Timeout=5000" \
BOOKMARKS_PORT=8766 ./build/examples/bookmarks/ladder_bookmarks_server

# Desktop client, either deployment mode:
./build/examples/bookmarks/ladder_bookmarks_gui                          # in-process
./build/examples/bookmarks/ladder_bookmarks_gui --server ws://127.0.0.1:8766
```

Sign in with any username (dev-mode login, no password — see
`include/bookmarks/dto/auth_dto.hpp` for exactly what that does and does not
mean). Run two clients with two usernames against one server to see the
isolated collections and the shared feed.

`Local` mode is deliberately the smaller deployment: it hosts the models in
the client process, so it journals nothing, runs no metadata worker and no
outbox relay, and — because `LocalBackend` runs no authorizer at all — is
single-user by construction. The two-user isolation this rung is *about* is
only meaningful against the server.

## Reference implementations

- **[linkding](https://github.com/sissbruecker/linkding)** (Python/Django,
  MIT, SQLite by default, ~11k LOC app + ~23k LOC tests) — the anchor.
  Probably the cleanest small schema in its class (9 Django models in
  `bookmarks/models.py`), a complete REST API, and an exceptional test suite
  to steal test cases from.
- [Shaarli](https://github.com/shaarli/Shaarli) (PHP, flat-file, no DB) —
  secondary reference: proof that single-user bookmarking needs no database
  at all; its whole-datastore-in-memory design is literally morph's
  in-process model. Good for the local-backend-only variant.

## What to implement

Models: `BookmarkModel` (per-user collection), `TagModel`, later
`SharedFeedModel`. Follow linkding's schema: `Bookmark` (url, title,
description, notes, unread, archived, timestamps), `Tag`, many-to-many
bookmark↔tag, `UserProfile`.

Actions, in build order:

1. Bookmark CRUD + archive/unarchive + tag assignment.
2. Search/list with filters (tag, unread, archived, text) and pagination.
3. **Bulk operations** — `BulkEdit { ids, addTags, removeTags, archive }`:
   the first multi-entity atomic action; all-or-nothing against SQLite.
4. Tag rename/merge (cascades across bookmarks).
5. Netscape HTML import/export — large payload through the wire protocol;
   measure where message-size bounds (`docs/spec/security.md`) bite.
6. **Sharing**: mark bookmarks shared, other users read a merged shared feed.

## morph subsystems exercised

- **Sessions & authorization** for the first time: every action carries a
  `session::Context`; an `IAuthorizer` scopes users to their own collections;
  shared feeds are the first cross-principal read. Per review, adopt **real
  signed-token authentication here**, not hand-waved principals: the shipped
  `SigningAuthorizer` + `authenticate()` hook
  (`include/morph/session/session_auth.hpp`, `docs/spec/session/session.md`)
  are essentially untested at app scale — more precisely than originally
  framed: `examples/bank/tests/test_remote.cpp`'s `NoCloseAuthorizer`
  authenticates by trusting `ctx.principal` outright with **no signature
  verification at all**, and says so in its own comment. **Bookmarks is the
  first rung to wire real signed-token auth end-to-end**, not merely the
  first to touch `IAuthorizer`. This rung's server mints and verifies tokens
  with `SigningAuthorizer`'s default `hmacSha256` MAC (not
  `MORPH_REQUIRE_VETTED_HMAC`'s stricter injected-MAC mode — that flag is a
  hardened-deployment concern for a later rung to pick up; this one exercises
  the ordinary path). `authorizeRegister`/`authorizeInstance` were intended
  to be exercised for real (see "Design decisions" below), with
  `tests/test_policy_hardening.cpp`'s `OwnershipAuthorizer` as the framework
  precedent for per-user instance ownership. `authorizeInstance` is now
  genuinely reachable and enforcing — see the "Instance-level ownership is
  now real, but is not the layer that protects a user's data" bullet under
  "Design decisions" for exactly what it does and does not catch.
  `authorizeRegister` remains unconditionally permissive by choice. What is
  wired end-to-end and genuinely exercised regardless is the part that
  matters most: signed tokens minted by the
  server, verified on every single `execute`, with the verified principal
  made authoritative before any model runs. The local backend genuinely
  never authorizes (`LocalBackend::registerModel`/`registerModelShared`
  consult no `IAuthorizer` anywhere in `backend.hpp`) — models re-check
  `Context::principal` themselves regardless of backend, per rule 1.
- **The background-job pattern** (this rung's framework-level deliverable):
  linkding auto-fetches title/favicon/preview after save
  (`bookmarks/services/tasks.py`) — work *triggered* by an action that
  completes later and mutates the model outside any client request.
  **Resolved: internal-client pattern, no new framework seam.** A typed
  in-process path already exists and is sufficient —
  `SimulatedRemoteBackend` is a shipped public backend routing through the
  complete server pipeline (authorizer, journal log provider, per-instance
  strand). `examples/pastebin/src/app/app.cpp`'s `App`/`_sweepBridge`
  already proves the pattern working end-to-end (a `shared_ptr`-captured
  `BridgeHandler` kept alive across every dispatched call's
  `.then()`/`.onError()`, closing the real race a plain local handler would
  hit against `RemoteServer`'s async dispatch); this rung's metadata-fetch
  worker reuses that shape unchanged. One part of the original framing was
  overstated and is corrected here: `handleInline` does reject `"execute"`
  (a real, documented restriction — its reply would write into a stack
  buffer already gone by the time the async reply lands), but
  `SimulatedRemoteBackend::execute()` never calls `handleInline` — it calls
  the async 2-argument `handle()`, so the rejection never fires for the
  internal-client path; it was never actually a blocker.
  **Service-principal convention (defined here, for every later rung that
  reuses this pattern):** the worker mints its own signed token via a
  `TokenIssuer` sharing the server's `SigningAuthorizer` secret, with
  `principal = "system:metadata-fetcher"`, and attaches it to every call via
  `Bridge::setDefaultSession()`. Its calls then authenticate and authorize
  exactly like a real user's — fully auditable in the journal via
  `session::current()->principal` inside the model — with zero framework
  changes. This rung's worker constructs its backend with the one-argument
  `SimulatedRemoteBackend(RemoteServer&)`, so its calls carry `ConnectionId 0`
  — the server's unscoped sentinel — and nothing it registers is ever
  reclaimed by `closeConnection`. That is this rung's *choice*, not a
  framework limitation: `SimulatedRemoteBackend` also ships a
  `(RemoteServer&, ConnectionId)` constructor taking a scope from
  `RemoteServer::openConnection()`, and a scoped worker's registrations would
  be reclaimed exactly as a dropped socket's are
  (`examples/TESTING.md`'s closed-gap list, entry 5). The unscoped shape is
  kept for two reasons. It needs no reclamation to be correct: the worker's
  handlers are created per pass and released only once every dispatch that
  pass issued has settled, and `App`'s shutdown-drain contract — the same
  manual lifetime-ownership discipline rung 1 established, reused verbatim —
  is what ends the last of them. And a scope has nowhere correct to be closed
  here: `closeConnection` reclaims every instance registered under it, so
  calling it from `~App`'s *body* would run while `_fetchBridge` and any
  outstanding pass are still alive — the same "model not found" race
  `fetchMetadataOnce`'s own comment exists to close — while the point where it
  would be safe (after `_fetchBridge`'s destruction) is not reachable from a
  destructor body at all. Scoping the worker is therefore a change to this
  class's teardown design, not a constructor swap.

  The GUI sees results on a later poll: this rung's DoD includes a **minimal
  `GetChangesSince` poll action** as the event-pattern preview (rung 3
  formalizes the full event-queue design) — there is no existing
  polling/event-sequencing precedent anywhere in the framework to reuse; this
  rung builds it from a `ChangesCursor` query (a millisecond timestamp paired
  with a same-instant id tie-break, not a bare `Timestamp` — issue #43's fix
  for the boundary case a timestamp-only cursor can silently drop),
  deliberately minimal otherwise. The action exists and is tested; the shipped
  client does not dispatch it (see the client-gaps list).
- **Journal**: tag renames and bulk edits give the first multi-row entries.
  Two separate decisions, both resolved:
  (a) **store/log atomicity — split by blast radius.** `BulkEdit`
  (`BookmarkModel`) and `MergeTags` (`TagModel`) are the two actions that
  mutate more than one row *and* need the journal entry to be atomic with the
  mutation, so each writes its own `bookmark_outbox` row inside the same
  `SqlTransaction` as the mutation, and `journal::OutboxRelay` drains that
  table into the durable `IActionLog` in a separate pass
  (`App::relayOutboxOnce`). A crash mid-mutation can therefore never leave the
  store *and* the journal disagreeing about a partially-applied bulk change.
  `examples/concepts/journal_and_outbox.cpp` is the worked pattern this
  follows, and remains the repo's only other consumer of `OutboxRelay`.

  The **opt-out mechanism is per action, not per instance**: both are
  registered `Loggable::No` (`models/bookmark_model.hpp`,
  `models/tag_model.hpp`), which suppresses the framework's own auto-append
  for exactly those two action types so it cannot double-log alongside the
  model's outbox write. The framework's other opt-out,
  `IModelHolder::setOutboxManaged(true)`, is not used here and would be the
  wrong tool: it is a property of a model *instance*, so it silences
  `recordIfAttached` for **every** action that instance serves
  (`include/morph/core/model.hpp`) — including the single-row CRUD that
  deliberately keeps the default. Per-action `Loggable::No` is the finer
  instrument, and it is the one this split needs.

  `RenameTag` is **not** outbox-managed and is registered plain-loggable: it
  updates one row (`src/models/tag_model.cpp`) and writes no outbox row at
  all. It sits with the plain single-row bookmark CRUD
  (create/edit/archive/delete), which keeps the framework's default
  two-independent-write behavior — the same choice rung 1 made for
  `PasteModel`, but only ever *implicitly*; here it is explicit: a crash
  between the store commit and the journal append can lose that one action's
  journal entry, but can never corrupt the store, and a single-row loss
  carries none of a partially-applied bulk edit's ambiguity.
  (b) **Undo: no generic undo**, consistent with the ladder-wide position
  [`LADDER.md`](../LADDER.md)'s "Journal honesty" section already recorded at
  rung 1 — `journal::undoLast()` returns a *detached* holder with no API to
  reinstall it into a live server registry, so in-place undo of a shared
  instance is not possible today, full stop. `DeleteBookmark` is a hard
  delete with no compensating action (mirroring rung 1's `DeletePaste`);
  `unarchive` is an ordinary domain action that happens to reverse `archive`
  in effect, not journal-level undo, and needed no special framework
  support to write.

## Design decisions

Three further decisions this rung's README named or implied but didn't yet
resolve in writing:

- **Model topology and the shared feed — corrected after deeper research
  (see below), superseding the paragraph this bullet originally had.**
  `BookmarkModel`, `TagModel`, and `SharedFeedModel` are **all registered
  plain** — no `BRIDGE_MODEL_KEY`/`AllowShared` anywhere in this rung.
  The original plan was framework-`shared` instances "keyed by principal,"
  with ownership enforced through `authorizeInstance`; that design does not
  work. `RemoteServer::acquireSharedInstance()`
  (`include/morph/core/remote.hpp`) records every shared instance as
  `_owners[fresh] = std::string{};  // shared instances are ownerless, by
  design`, and its own doc comment says why: a shared instance's owner is
  *always* recorded empty, specifically so `authorizeInstance`'s
  `ownerPrincipal == ctx.principal` check does not reject the second,
  third, ... client who attaches to it. That makes the ownership check a
  **no-op** for any `AllowShared` model — exactly backwards from what per-user
  ownership needs. The mechanism that actually records a real owner is *plain*
  (non-shared) registration: the `register` branch of
  `RemoteServer::dispatchMessage()` stamps
  `_owners[mid] = std::move(env.session.principal)` from the verified,
  authenticated caller.
  So `BookmarkModel`/`TagModel` are registered plain, exactly like
  `pastebin::PasteModel` — each client's own `register` call gets its own
  fresh instance, and `authorizeInstance` genuinely denies a different
  principal from touching that specific instance. Nothing about "one
  collection per user" is lost by dropping the shared-instance framing: a
  model instance carries no meaningful in-memory state here — all real
  state is the database, partitioned by an `ownerPrincipal` column — so
  every registration by the same user, from any device, reads and writes
  the identical rows regardless of how many separate instances exist for
  them. `SharedFeedModel` is *also* registered plain, for a different
  reason: `AllowShared` requires a keyed action
  (`BRIDGE_MODEL_KEY`/`ActionKeyTraits`) to converge multiple clients onto
  the *same* instance, machinery built for genuine multi-client convergence
  that buys nothing here — every `SharedFeedModel` instance reads the
  identical `WHERE shared = 1` rows regardless of how many instances exist,
  so there is nothing to converge. One `BookmarksAuthorizer`
  (`ownerPrincipal.empty() || ownerPrincipal == ctx.principal`, the
  `OwnershipAuthorizer` shape from `tests/test_policy_hardening.cpp`) covers
  all three model types without branching: plain-registered
  `BookmarkModel`/`TagModel` get a real, non-empty owner check;
  `SharedFeedModel`'s own `execute()` never uses `ownerPrincipal` to filter
  anything, so the same check being trivially permissive there is harmless
  — its actual protection is `authorizeRegister`'s "must be authenticated"
  gate. Ownership is enforced twice regardless, per rule 1: server-side via
  the authorizer, and again inside the model itself against
  `Context::principal`, since the local backend enforces neither.
- **Instance-level ownership is now real, but is not the layer that
  protects a user's data.** `register`/`attach`/`assign`/`deregister`
  envelopes carry the caller's authenticated session, so `RemoteServer`
  records a real, non-empty owner for each of `BookmarkModel`/`TagModel`'s
  plain-registered instances, and `authorizeInstance`'s ownership comparison
  genuinely denies a different principal's `execute`/`deregister` naming
  that instance's `modelId` directly — confirmed empirically (a test
  authorizer logged `ctx.principal`/`ownerPrincipal` for both alice's and
  mallory's own instances during development). `authorizeRegister` stays
  unconditionally permissive, by choice rather than necessity (see its own
  doc comment).
  **What this does not do is protect one user's row from another's**, and it
  never could, fixed or not: `BridgeHandler<Model>` (this rung's only
  shipped client) never names another connection's `modelId` — each client
  only ever dispatches through its own registered instance — so a normal
  client's cross-user access attempt (`GetBookmark{id}` naming another
  user's row through the caller's *own*, legitimately-owned instance) never
  touches `authorizeInstance`'s check at all; it would pass regardless. That
  is caught only by the model's own row-level re-check
  (`tests/test_bookmark_model.cpp`'s "denied by the model's own ownership
  re-check ... not by authorizeInstance" case, confirmed by the propagated
  error message: `"bookmark belongs to a different principal"`, not
  `authorizeInstance`'s `"unauthorized"`). Every `execute` also still goes
  through `SigningAuthorizer::authorize()` (a real signature and expiry
  check, on a token an unauthenticated caller cannot produce), and
  `RemoteServer` still overwrites `Context::principal` with the verified
  identity before the model runs. Three layers in total, each catching a
  different thing: token validity (`authorize`), instance ownership
  (`authorizeInstance`, real but narrow), and row ownership (the model
  itself, the one that actually matters for user isolation). The one action
  that deliberately does not scope by row owner, `RecordMetadata`, checks in
  its own body that the caller *is* the metadata-fetch service principal —
  `authorizeInstance` cannot express that either, since the worker's own
  instance is exactly what it is authorized to use — and `AuthModel` refuses
  to mint a token in the reserved `system:` namespace, so that authority
  cannot be requested from outside.
- **Bookmark↔tag many-to-many.** Lightweight's `DataMapper` ships
  `HasManyThrough<ReferencedRecord, ThroughRecord>`
  (`.../DataMapper/HasManyThrough.hpp`), but it cannot be used as an embedded
  member on `BookmarkRecord`/`TagRecord` here: `DataMapper::Update()`'s
  non-reflection path calls `IsModified()` on every record member via
  `EnumerateRecordMembers`, and neither `HasMany<T>` nor
  `HasManyThrough<T,U>` declares that method — a record type that embeds
  either fails to compile the moment `Update()` is instantiated for it
  (verified directly against Lightweight's vendored
  `DataMapper.hpp`/`Description.hpp`; independently confirmed by
  `examples/bank/include/bank/db/account_entity.hpp`'s own doc comment
  making the identical argument for `HasMany`). So: `BookmarkRecord`/
  `TagRecord` carry **zero** relation-typed members. The many-to-many is
  still a real junction entity, `BookmarkTagRecord` (`BelongsTo` the
  bookmark, `BelongsTo` the tag, its own surrogate primary key) — but tag
  reads go through a plain `Query<BookmarkTagRecord>().Where(...)` call in
  the model, never an embedded relation field. `BookmarkTagRecord` itself
  never needs `Update()` (only `Create`/delete), so this doesn't affect it.
  Tag assignment/removal is a direct `Create`/delete of `BookmarkTagRecord`
  rows by the model — this was always true regardless of the
  `HasManyThrough` question, since its own `Loader` is read-only
  (`count`/`all`/`each`, no `Add`/`Remove`) — consistent with `HasMany`'s
  own documented limitations elsewhere in the ladder (rule 4's "Lightweight's
  own documented idioms" clause). No new sanctioned-escape-tier entry is
  needed: a plain `Query<>()` call is ordinary `DataMapper` usage, not an
  escape.
- **Bulk-write mechanics.** `BulkEdit`'s per-item mutations are heterogeneous
  (some ids get tags added, others removed, some archived) — `SqlStatement::
  ExecuteBatch` only fits a homogeneous single-statement batch, so it is not
  the right tool here. `BulkEdit` (and tag rename/merge) use N individual
  statements inside one `Lightweight::SqlTransaction{mapper().Connection(),
  SqlTransactionMode::ROLLBACK}`, the same all-or-nothing pattern
  `PasteModel::execute(GetPaste)`/`execute(EditPaste)` already proved out in
  rung 1 — any unhandled throw mid-batch rolls back automatically, and
  `transaction.Commit()` is reached only once every item in the batch has
  applied.

Every decision above was verified against real source before being written
here, not assumed from a doc comment: `SigningAuthorizer`,
`SimulatedRemoteBackend`, `OutboxRelay`, and `OwnershipAuthorizer` were all
read in `include/morph/` and `tests/` directly, and `HasManyThrough`'s
read-only `Loader` shape was confirmed against Lightweight's own vendored
source and test entities, alongside the `examples/pastebin`/
`examples/concepts` precedents cited inline above.

## Expected strain points

- Background fetches racing user edits on the same bookmark — strand
  serialization should make this safe; write the test that proves it.
- **Cross-model rename race**: `TagModel` renames a tag while a
  `BookmarkModel` `BulkEdit` adds the old name — two strands, no
  cross-instance transactions, and the strand *cannot* fix it. The test
  documents where consistency becomes app responsibility.
- **Local mode has no authorization at all** (the local backend never
  authorizes): the first multi-user rung must demonstrate this with a test
  and document the mitigation — models re-checking `Context::principal`
  themselves, per `docs/spec/security.md`.
- **Unicode tags**: NFC/NFD and case — SQLite `NOCASE` is ASCII-only, so
  the C++ comparison, the SQLite unique index, and the GUI display can
  disagree; pick a normalization point and test it.
- Favicon/preview blobs: store paths in SQLite, bytes on disk; do not send
  them through the action protocol.
- Import of thousands of bookmarks: chunked actions; a connection drop
  between chunks must resume without duplicating (idempotency keys) and
  without a phantom half-import in the journal.

## Definition of done

- Two users on the remote backend with isolated collections and a working
  shared feed; authorization enforced server-side, not by the client. This
  originally read "specifically via the shipped `authorizeRegister` and
  `authorizeInstance` hooks … not only model-level checks", on the reasoning
  that leaving them untested here means they stay untested forever. Task 12
  exercised them against a real `RemoteServer` and found that neither hook
  could see a caller's identity, because `register` envelopes carried no
  session — filed as a finding, since fixed: envelopes now carry the
  caller's authenticated session, and `authorizeInstance` is genuinely
  enforcing for plain-registered instances (see "Instance-level ownership is
  now real" above). The criterion reads: server-side enforcement via
  `SigningAuthorizer::authorize()` on every action, `authorizeInstance`'s
  now-real instance-ownership check, and the models' own verified-principal,
  row-level scoping — three layers, with the last doing the work that
  actually protects one user's data from another's, since instance-level
  ownership alone was never the layer that could.
- Metadata auto-fetch demonstrably running as a background job: bookmark
  appears immediately; title/favicon arrive later, and the minimal
  `GetChangesSince` poll action (the rung-3 preview) is what a client asks for
  them with. This criterion is met at the model and presenter level, where
  `GetChangesSince` is implemented, tested and exposed. The **shipped client
  does not dispatch it** — see the client-gaps list below.
- Bulk edit is atomic under injected mid-batch failure.
- The background-job design record (internal-client vs. framework seam,
  service principal, journaling of job mutations) written in this README.

## Known gaps this rung ships with

Everything below is a real gap, stated here rather than left for a reader to
discover. Gaps in the *client* specifically have their own list further down;
these are the domain- and test-coverage ones.

- **Unicode tag normalization is unaddressed.** "Expected strain points"
  above asks this rung to pick a normalization point (NFC/NFD, case) and
  test it. It does not: tag names are compared and indexed as raw bytes, so
  a `café` typed as NFC and one typed as NFD are two different tags, and
  SQLite's ASCII-only `NOCASE` does not close it. No test covers this.
- **Chunked import is correct but never tested at scale.** Idempotency per
  `opId` is tested, and a chunk over `kMaxImportChunkBytes` is refused with
  `TooLarge` — deliberately not by `ImportBookmarks::validate()` itself,
  since every real dispatch path (`Bridge::executeVia`, `RemoteServer`)
  consults `validate()` before `BookmarkModel::execute` is ever reached, so
  a `validate()`-level rejection would always surface as the untyped
  `ValidationError`, never as `TooLarge`. The distinction is only
  observable in-process (a direct call, or `Local`/`LocalSingleThread`
  dispatch through `Bridge`): over `Socket`/remote transport,
  `RemoteServer` encodes every server-side exception as an opaque
  `wire::makeErr(exc.what())` string and the client reconstructs a generic
  `std::runtime_error`, discarding the original type — a framework-wide
  property of every model's typed errors, not specific to this rung.
  Nothing here imports thousands of bookmarks across many chunks, and no
  test drops a connection mid-sequence.
- **The transport's own message-size bound is not measured by this rung.**
  `kMaxImportChunkBytes` is set "well under" it, but that relationship is
  asserted, not verified: there is no bookmarks equivalent of pastebin's
  "An oversized `CreatePaste` is refused by the transport" test. If the
  transport bound ever drops below 64 KiB, this rung's own chunk limit stops
  being the one that bites and nothing here would notice.
- **`is_unread` is write-once at creation — nothing ever clears it.** Every
  bookmark is created unread and no action (there is no `MarkRead`/
  `MarkUnread`) ever flips the column. So `ReadFilter::ReadOnly` always
  returns an empty page, and `ReadFilter::UnreadOnly` is behaviorally
  identical to `ReadFilter::Any`. The column, the enum and the filter are all
  wired end to end and would work the moment a mutating action exists; there
  simply isn't one.
- **The GUI never leaves the first page.** `BookmarkBridge::refresh()`
  discards the `nextCursor` every list/feed response carries, and no QML
  binding asks for a further page. The shipped client therefore shows at most
  the first ~20 bookmarks (and the first ~20 shared-feed entries) with no way
  to reach the rest. Pagination is fully implemented and tested at the model
  level — the keyset cursor works — it is only the client that does not use
  it.

## The client, and its known gaps — stated rather than smoothed over

The desktop client (`gui/`, `gui_lib/`) is schema-driven throughout
(`../IMPLEMENTATION.md` rule 2): `Login`, `CreateBookmark`, `EditBookmark`,
`ImportBookmarks`, `RenameTag` and `MergeTags` all render from
`morph::forms::schemaJson<A>()` through the shipped `MorphForms`
`DynamicForm`, including the login screen — there is **no hand-built username
field**, and no hand-built input widget anywhere. Each of the six declares
`explicitSubmit = true`, so its schema carries `"x-submitMode": "explicit"`
and the renderer supplies its own gated Submit button
(`docs/spec/forms/forms.md`, "Explicit submit mode"): there is no hand-built
submit button either, and every form is bound to the live controller. The one
non-form input on the whole screen is the per-row selection checkbox, which
types nothing.

Two pieces of glue carry their own written justification, per rule 2's "(b)
pure glue with no domain logic" clause:

- `gui::BookmarkFormsController` — this rung's copy of
  `morph::qt::forms::FormsControllerCore`, composed over an injected
  `Bridge&`/`IExecutor*` rather than constructing its own `LocalBackend`. The
  shipped core's own composing constructor now supports this directly (the
  same justification `pastebin::gui::PasteFormsController` carries), plus
  one genuinely new part this rung's own controller still owns — routing an
  action-type string to whichever of the three form-serving models owns it,
  which the shipped core (templated over a single model) has no equivalent
  for.
- `gui::FormsBridge::onLoginSucceeded` — installs the token the server
  returned as the shared `Bridge`'s default session, so every subsequent
  action carries it. Infrastructure wiring, not business logic: it decides
  nothing, and both the token and the principal it announces are the
  server's, never the client's claim.

Known gaps:

- **Array fields are typed as strings, whatever the schema's `items` says.**
  `DynamicForm` renders a JSON `array` field with a dedicated
  comma-separated-with-validation control (`src/qt/forms/qml/DynamicForm.qml`'s
  `isArray` descriptor and `fieldJsonLiteral`/`arrayJsonLiteral`, covered by
  `src/qt/forms/tests/tst_DynamicFormArrayField.qml`) and encodes it as a
  genuine JSON array literal, but every entry in that array is a JSON
  **string**. Array-of-string is therefore the fully supported case and
  array-of-anything-else is not (`docs/spec/forms/forms.md`, "Array fields").
  `CreateBookmark::tags`/`EditBookmark::tags` are `std::vector<std::string>`,
  reaching the renderer as `{"type":"array","items":{"type":"string"}}` —
  exactly the supported shape — so tagging works from the create and edit
  forms with no special-casing in `BookmarkListView.qml`, which renders every
  field the schema declares. Not independently re-verified end to end against
  this rung's own `MORPH_BUILD_FORMS_QML` build, but the schema shape is
  identical to the one the framework test above exercises and this rung's
  forms apply no exclusion.
- **`BulkEdit` is not a form**, and the reason is the *typing* half above
  rather than a missing control: its one required member is
  `std::vector<BookmarkId>`, whose schema is
  `{"type":"array","items":{"$ref":"#/$defs/BookmarkId"}}` with `BookmarkId`
  defined as `{"type":["integer","null"], …}`. Typing `1, 2` would submit
  `["1","2"]`, an array of strings that does not decode back into
  `std::vector<BookmarkId>`. The GUI drives it from the list's own
  multi-selection through `BookmarkBridge::bulkArchive` instead, where no
  typing is involved.
- **The shipped client never polls for background-job results.** The metadata
  worker fills a title in some seconds after a bookmark is created, and
  `GetChangesSince` is the action a client asks for that with — implemented in
  `BookmarkModel`, exposed as `BookmarkPresenter::getChangesSince`, and tested
  at both levels. `BookmarkBridge` deliberately does not relay it
  (`gui_lib/bookmark_qml_bridges.hpp`) and no QML binding asks for it, so a
  fetched title appears only on the next manual **Refresh**. There is no
  `Timer` anywhere in this rung's QML.
- **Six model instances per client, not four.** `app.cpp`'s `kMaxLiveModels`
  comment budgets "roughly one instance per model type it uses (four in this
  rung)". The shipped client registers six: the forms controller owns an
  `AuthModel`, a `BookmarkModel` and a `TagModel` handler, and the three
  presenters own a `BookmarkModel`, a `TagModel` and a `SharedFeedModel`
  handler. `BridgeHandler<Model>` is a template over one model type and both
  classes take `(Bridge&, IExecutor*)` by presenter rule 2, so sharing one
  handler between them is not expressible today. At the 256 cap that is ~42
  concurrent clients rather than ~64.
- **Registration timing.** `BookmarkListView`'s three list controllers each
  expose a `bound` signal (`Presenter::trackBound()`, backed by
  `Bridge::whenBound()`) that settles once their registration round trip
  lands; the view gates its bootstrap `refresh()` calls on it instead of
  retrying on a timer. The login submit has no such gate, because it is
  user-initiated: a click that lands before registration settles reports
  "handler not bound" and the next click works. Measured against a real
  server, registration settles well inside the time it takes to type a
  username, so this was never observed in practice — but it is reachable, and
  a server that never answers leaves the login button failing forever, since
  `Remote` mode has no connect timeout at all.
- **No `--seed`.** `LADDER.md` asks every rung for one; this rung's server
  ships none, deliberately — see `src/server/main.cpp`'s file comment for the
  argument (seeding by direct model call would need
  `morph::session::detail::ScopedContext`, the exact detail-namespace reach the
  *testkit reaches into four detail namespaces* finding objects to (recorded on
  the `application-ladder` branch; not present in `docs/findings/` here), and
  the internal-client alternative is rung 4's `action_driver`
  work). Demo data is created through the client.
- **The offscreen QML smoke test proves loading, not behavior** — see
  `tests/test_gui_qml_smoke.cpp`'s own header comment for exactly what it
  does and does not cover. The behavioral half is the presenter suites plus
  the manual end-to-end run.

### Two bugs the first real client run found

Both were invisible to every test that existed, because every test drove the
models or the presenters directly and none drove *the client*:

1. **Login was unreachable over a real server.**
   `SigningAuthorizer::authorize()` verifies `Context::token` on every
   `execute` and rejects when there is none — including for `Login`, the only
   way to obtain a token. A fresh client got `err "unauthorized"` for
   everything it could possibly send. `BookmarksAuthorizer::authorize` now
   carves out exactly `AuthModel`/`Login` and nothing else; see its doc
   comment for why that gives nothing away, and
   `tests/test_bookmarks_authorizer.cpp` for the unit-level and
   over-the-wire regression tests.
2. **`CreateBookmark::title` was schema-`required`.** It was missing from
   `optionalFields`, so the generated create form refused to submit without a
   title — making it impossible to create from the GUI the very title-less
   bookmark the background metadata fetch exists to complete, which is one of
   this rung's own definition-of-done items. `title` is now optional in both
   `CreateBookmark` and `EditBookmark`, matching what `validate()` and the
   member's own doc comment always said.
