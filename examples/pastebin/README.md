# pastebin — rung 1 of the [application ladder](../LADDER.md)

**Status: shipped** — every rung-1 task is complete; see
[Definition of done](#definition-of-done) for what that does and does not
mean (the native stack is verified end to end; the WASM client is written and
CI-gated but has never been compiled here). A minimal pastebin: create a text
snippet, share its URL, let it expire or burn after N reads. The smallest
complete morph application — one entity, one model, SQLite, Qt WASM client.

> **Reading this to learn morph, not to work on the rung?**
> [`docs/GETTING-STARTED.md`](../../docs/GETTING-STARTED.md) walks this
> application layer by layer as a tutorial. This README is the rung's own
> design record — its resolved design questions, its findings and its
> definition of done — and assumes the framework is already familiar.

## Running it

```bash
# One-time configure (Qt 6.5+, an ODBC SQLite3 driver, MORPH_BUILD_FORMS_QML
# for the schema-driven create form):
cmake -S . -B build -G Ninja \
    -DMORPH_BUILD_QT=ON -DMORPH_BUILD_FORMS_QML=ON \
    -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=pastebin

# Server (owns the database, the action journal and the expiry sweep):
PASTEBIN_DB="DRIVER=SQLite3;Database=pastebin.db;Timeout=5000" \
PASTEBIN_PORT=8765 ./build/examples/pastebin/ladder_pastebin_server

# Desktop client, either deployment mode:
./build/examples/pastebin/ladder_pastebin_gui                          # in-process
./build/examples/pastebin/ladder_pastebin_gui --server ws://127.0.0.1:8765
```

The browser client is the same program with a different `main()`
(`gui_wasm/main_wasm.cpp`), built only in an Emscripten configure — which
additionally needs `-DMORPH_CLIENT_ONLY=ON`, since a WASM client names its
model type but must not link the model's ODBC-backed bodies
(`docs/spec/core/registry.md`; `morph_add_rung()` fails the configure with that
explanation if the option is missing). Its server url is baked in at build time
via `-DMORPH_LADDER_PASTEBIN_WASM_SERVER_URL=ws://host:port`. The exact
configure line CI uses is `.github/workflows/wasm-ladder.yml`.

**Scope note (delivery + verification reviews):** review rounds had piled
ladder-wide infrastructure onto this rung until it stopped being small. That
infrastructure is now **rung 0**, delivered *before* the pastebin app: the
testkit subset (`pump.hpp`, `backend_rig.hpp`, `db_fixture.hpp`, Qt-owning
test main), `examples/common/gui` (AppContext + Presenter base), the
`ladder-tests` CI job, and the **WASM-remote spike** — the first-ever
WASM + `QtWebSocketBackend` run, which requires `asyncRegistrationEnabled =
true` (opt-in, off by default) and the `setConnectHandler` pattern instead
of `waitForConnected()` (which hangs the page on WASM), with a written
fallback plan if it bounces off framework work. Rung 1 proper is the app
below plus its design records. Deferred from rung 1: the convergence
assertion (its `poll()`/`lastEventId()` hooks exist only from rung 3) and
the full hostile-content corpus suite (start with a representative subset).

## Reference implementations

- **[MicroBin](https://github.com/szabodanika/microbin)** (Rust, Actix,
  BSD-3-Clause, ~4k LOC) — the anchor. Small enough to read end-to-end in an
  afternoon; its single `Pasta` struct *is* the data model. Supports SQLite or
  a flat JSON file behind a two-backend storage abstraction — directly
  analogous to morph's in-memory vs. SQLite-persisted split.
- [PrivateBin](https://github.com/PrivateBin/PrivateBin) — studied and
  rejected as anchor: its zero-knowledge design makes the server a dumb
  ciphertext store, exercising none of the typed-model machinery. Worth a look
  only for its burn-after-read UX.

## What to implement

One model, `PasteModel`, keyed by paste id (animal-name ids like MicroBin's
are a nice touch), with actions:

1. `CreatePaste { content, syntax, expiresAt, burnAfterReads, isPrivate }`
   → `PasteId`
2. `GetPaste { id }` → `PasteView` — **this is the interesting one**: reading
   increments `read_count` and may delete the paste (burn-after-reads), so a
   read is a *write*.
3. `EditPaste`, `DeletePaste` — plain mutations for editable pastes.
4. `ListPastes {}` → recent public pastes (pagination via cursor field).

Persistence: one Lightweight entity (`PasteRecord`) and one
`LIGHTWEIGHT_SQL_MIGRATION`, per [`../IMPLEMENTATION.md`](../IMPLEMENTATION.md)
— fields modeled on MicroBin's `Pasta` (id, content, extension, private,
editable, created, expiration, last_read, read_count, burn_after_reads).
DTO fields follow the strong-type rule: `PasteId`, `Timestamp`, `enum
class` visibility, a reads `Quantity` — `std::string` only for content and
extension.

Clients: Qt Widgets desktop client and the same code compiled to WASM
(follow [`../bank/gui_wasm`](../bank/gui_wasm)). Local and remote backends
must both work unchanged.

## morph subsystems exercised

- The full local/remote loop end-to-end on a fresh codebase (registration,
  strands, wire protocol, WASM build).
- **Journal**: install `FileActionLog` from day one. Design questions,
  **resolved** below (ladder discipline rule):

  - *Is a state-mutating read an action?* **Resolved: yes — `GetPaste`
    stays the one client-visible, journaled action (default
    `Loggable::Yes`), not split.** The recommended split (a pure, unlogged
    `GetPaste` plus an internally-journaled `RecordRead` mutation) turned
    out to be structurally unavailable: `IModelHolder::recordIfAttached`
    (`include/morph/core/model.hpp:145`) is called only by the two
    built-in dispatch runners, for the one action actually dispatched —
    there is no seam for a model to author a second, independent
    `LogEntry` from inside its own `execute()`. `Bridge::modelFactory`
    constructor injection only reaches `Local`-mode registration; morph
    has since grown `ModelRegistryFactory::registerModel<Model>(modelId,
    factory)` (`include/morph/core/registry.hpp`) as the equivalent seam
    for `Socket`-mode's registry-constructed models, but this rung
    predates it and has not adopted it. **Consequence, accepted
    and documented, not worked around:** replaying `GetPaste`'s entry
    re-runs the real burn/read-count logic against whatever row state
    exists at replay time — for a burn-after-read paste this can
    resurrect content the user was told was destroyed. This is the
    concrete, privacy-shaped example the journal-honesty position below
    generalizes from; it is not unique to `GetPaste` in kind (replaying
    *any* DB-backed mutating action re-touches the live database — see
    that position) but it is the sharpest instance of it, so pastebin's
    UI must never expose a raw "undo"/"replay" affordance over the
    journal, only read-only history rendering.
  - *How does expiry replay?* **Resolved: an explicit, journaled
    `ExpirePaste{id}` action, dispatched by a periodic sweep that is a
    genuinely separate top-level call — not nested inside `GetPaste`'s own
    `execute()`.** `GetPaste`'s own atomic update (the burn-atomicity
    decision, below) already excludes an expired row from its `WHERE`
    clause defensively, so correctness never depends on sweep timing — a
    client asking for an expired paste gets `Expired` regardless of
    whether the sweep has reached that row yet. This is what makes a
    **periodic** sweep (a timer in the app-layer server bootstrap,
    `src/app/`, not model code — it is orchestration, not domain logic;
    typically every few seconds) both simpler than a per-request hook
    (`RemoteServer` has no confirmed pre-dispatch interception seam to
    hang one on) and *more* complete than "on access" alone — it also
    reclaims pastes nobody ever requests again, which an on-access-only
    sweep would leave orphaned forever. The sweep queries
    `expires_at_ms <= now()` directly (a plain, unlogged read — not an
    action) and dispatches `ExpirePaste{id}` for each match through an
    **internal client** — a `Bridge` over `SimulatedRemoteBackend{*server}`
    wrapping the app's own live `RemoteServer` — a first-class client of
    the same server, not a bypass: `SimulatedRemoteBackend::execute()`
    calls `RemoteServer::handle()`, the exact path a real socket client's
    call takes (`dispatchMessage` → `dispatchExecute` →
    `ActionDispatcher::dispatch`), so `ExpirePaste` is authorized,
    dispatched, and auto-journaled exactly like any client-issued action
    — no framework gap, no finding needed for this part. `ExpirePaste`'s
    payload is just `{id}` (never `now()`), so replaying its entry is
    trivially deterministic regardless of when replay runs. Under this
    rung's fail-open default (no authorizer configured),
    `RemoteServer::dispatchExecute` clears any claimed principal before
    the model sees it (`authenticate()` returns `nullopt` by default), so
    `ExpirePaste`'s `LogEntry.principal` reads empty — consistent with
    every other unauthenticated call this rung makes, not a gap.
  - *The ladder-wide journal position paper.* **Resolved:**
    `morph::journal` is an **audit trail** — install it to answer "what
    happened, and when" (render read-only history; `entries()` +
    `LogEntry.timestampMs`/`.principal`/`.outcome`). It is **not**
    event-sourcing and **not** a safe reconstruction mechanism for any
    DB-backed model, pastebin's `PasteModel` included:
    `journal::replay()`/`SessionLog::undoLast()` re-run the recorded
    action's real `execute()` against a freshly created model instance —
    for an in-memory-only model that's an isolated sandbox, but for a
    model whose real state lives in Lightweight/SQLite (every ladder
    model to date), "fresh instance" only isolates the *model object*,
    not the database it immediately reopens and mutates again. Do not
    invoke `replay()`/`undoLast()` against a live install's database;
    they exist for offline forensic reconstruction (a copied-aside
    database file) or for models that are provably pure/in-memory, which
    no ladder rung has shipped yet. Framework growth this rung proposes
    instead of assuming: (1) a documented, opt-in "replay-safe" trait or
    marker distinguishing pure/in-memory models from DB-backed ones, so
    `replay()` can refuse (or clearly warn) against the latter; (2)
    adopting the DI seam noted above, which would let `GetPaste` be split
    as originally hoped — not done in this rung.
- **Shared vs. unshared instance — the burn-atomicity decision. Resolved:
  SQL-atomicity, not a shared keyed instance.** `PasteModel` is registered
  plain (no `BRIDGE_MODEL_KEY`/`AllowShared`), matching bank's
  `NotificationModel` shape, not `AccountModel`'s. Burn-after-read
  atomicity comes from a conditional `UPDATE … WHERE read_count <
  burn_after_reads` issued via Lightweight's raw-query facility
  (`SqlStatement::Prepare`/`Execute`) — the pre-enumerated
  sanctioned-escape-tier answer named in
  [`../IMPLEMENTATION.md`](../IMPLEMENTATION.md) § sanctioned escape tier.
  **As shipped this is the transaction-wrapped two-statement form, not the
  single-statement `… RETURNING …` one originally written here.** The
  sqliteodbc driver accepts `UPDATE … RETURNING`, applies it, and reports
  the returned column count, but the first `FetchRow()` throws SQLSTATE
  24000 "Invalid cursor state"; it never opens a cursor over the returned
  rows — filed upstream as
  [`LASTRADA-Software/Lightweight#545`](https://github.com/LASTRADA-Software/Lightweight/issues/545),
  tracked morph-side as
  [`LASTRADA-Software/morph#58`](https://github.com/LASTRADA-Software/morph/issues/58).
  `PasteModel::execute(const GetPaste&)` therefore runs a
  `SqlTransaction` around (1) the identical conditional `UPDATE` minus its
  `RETURNING` clause, dispatched on `NumRowsAffected()`, and (2) an ordinary
  `DataMapper` read-back by primary key. **The atomicity argument is
  unchanged**: it never rested on `RETURNING`, only on the guard living
  inside the `UPDATE`'s own `WHERE`, which SQLite evaluates and applies
  indivisibly under a write lock — of N clients racing for the last allowed
  read, exactly one gets a non-zero affected-row count. The transaction only
  keeps the read-back consistent with the write it reads back, and folds the
  burn-delete into the same commit. This also avoids the
  shared-instance option's WASM coupling: a shared keyed instance's first
  `GetPaste` would drive the *synchronous* shared-attach path that aborts
  the page, pulling the async-shared-attach framework prerequisite forward
  from rung 3. Revisit sharing at rung 3, per the original recommendation.
- **Lightweight behind a model** at the smallest possible scale — the
  DTO ⇄ entity ⇄ `DataMapper` loop of [`../IMPLEMENTATION.md`](../IMPLEMENTATION.md)
  proven on a one-entity schema before the bigger rungs depend on it.
- **`morph::async::CallbackScope`** — this rung is the ladder's first
  consumer, in `gui_lib/paste_qml_bridges.hpp`'s `FormsBridge`. Its
  `submitIfValid` hands the forms controller two callbacks that capture
  `this` and emit `replyReceived`; a `Completion` always resolves through the
  executor, and both shells own their bridges by `unique_ptr` in `main()`, so
  a reply can land after the bridge is gone. The rung's two neighbours are
  covered by mechanisms that do not reach here — `PastePresenter` through
  `Presenter::track()`'s `QPointer` re-check, `PasteBridge` through Qt's
  signal/slot auto-disconnect — so `FormsBridge` takes the framework's
  general answer: a **last-declared** `CallbackScope` member, with both arms
  wrapped in `_callbacks.guard(...)`
  ([`docs/spec/core/callback_scope.md`](../../docs/spec/core/callback_scope.md)).
  `tests/test_paste_qml_bridges.cpp` drives the window (destroy a bridge
  mid-submit, then let the reply land) and asserts the observable half — the
  dispatch completed after the bridge was gone. The suppression itself has no
  observable signature outside a sanitized build, which is stated there
  rather than dressed up: this was closed by construction, not after a
  measured crash.

**Custom-GUI-element justification (`../IMPLEMENTATION.md` rule 2):** at the
time this rung was built, the shipped `morph::qt::forms::FormsControllerCore
<Model>` hardcoded its own `Bridge`/`LocalBackend`/executor internally, with
no way to compose it over `AppContext`'s `Bridge&`/`IExecutor*` — a direct
conflict with [`../TESTING.md`](../TESTING.md)'s "never construct executors
or backends themselves" presenter rule, and silently untestable in `Socket`
mode. The shipped core's own `(Bridge&, IExecutor*, schemasJson)` constructor
now supports this composition directly, closing the gap framework-side;
`gui_lib/paste_forms_controller.hpp` still owns a thin controller of its own
(this rung predates that constructor).
Pastebin's GUI still renders exclusively from `morph::forms::schemaJson<A>()`
through the real `MorphForms` QML module (justification (b): pure glue, no
domain logic, no hand-rolled widget) — only the backend-wiring seam is
rung-owned: a thin controller exposing the same
`schemaJson()`/`submitIfValid()`/`fetchOptions()` surface, constructed over
the `BridgeHandler<PasteModel>` `AppContext::onReady()` hands it.

## Required tests (from review)

- **Hostile content round-trip**: replay every input in `tests/fuzz/findings/`
  *as paste content* (control bytes, broken UTF-8), both directions, both
  backends — the exact bug class fuzzing already caught once in the wire
  layer.
- **Size-limit UX**: `CreatePaste` bouncing off the server's message-size
  bound; the client renders a typed error. Typed error rendering debuts
  here, not rung 4.
- **Duplicate create on retry**: this rung ships the *inverse* of the
  requirement as originally written, and does so deliberately.
  `tests/test_paste_model.cpp`'s "Two CreatePaste calls with identical
  content mint two distinct pastes at this rung" asserts that two identical
  creates really do produce two pastes, because rung 1's `CreatePaste`
  carries no op-id / idempotency-key field at all — there is nothing for a
  server to deduplicate on, and `LADDER.md` scopes exactly-once delivery to
  rung 4. The case is written to fail loudly the day that discipline lands
  here, rather than letting a guarantee nobody implemented drift into the
  documentation.

  The blocker is the missing key, **not** missing tooling: the
  fault-injection proxy shipped at rung 0–1 as
  `examples/common/testkit/fault_proxy.hpp` (`LADDER.md`'s queued-work list
  records it as *Shipped*), it can drop exactly the reply frame of call *k*,
  and `examples/kanban/tests/test_kanban_offline.cpp` already drives it. A
  genuine lost-reply-frame retry could therefore be staged against this rung
  today; it would simply mint the second paste, which is what the existing
  case already states in the cheaper way. Plus id-collision handling in the
  tiny animal-name keyspace.
- **Expiry edges**: `expiresAt` in the past / at epoch / malformed
  (wire error, not clamped); `GetPaste` against an already-past-`expiresAt`
  row before the periodic sweep has reached it (must still throw `Expired`
  — this is exactly what proves correctness doesn't depend on sweep
  timing); the periodic sweep firing between two pages of a `ListPastes`
  cursor walk.
- **Security posture (per the LADDER matrix)**: this rung deliberately runs
  the *unhardened* fail-open default, with one test that asserts the delta
  (any client can register / execute against a learned id) as executable
  documentation of `docs/spec/security.md`; it also owns the `hello`
  protocol-version-negotiation test — no example exercises negotiation
  today.
- **Store-error branch coverage, per failure class, through the real
  schema — not through one failing driver.** `db_fault_fixture.hpp`'s
  `SqlScopedLock`-based contention cannot fault an ordinary `DataMapper`
  call or the raw conditional update above (there is no injectable seam
  between `DataMapper` and the ODBC driver — see `examples/TESTING.md`'s
  testkit section). This rung provokes two of the three failure classes for
  real instead: `db_busy_fixture.hpp` holds a competing write transaction
  open on a second connection to force a genuine `SQLITE_BUSY`, and (for the
  raw conditional update specifically) a row already at
  `read_count == burn_after_reads` forces the zero-rows-affected branch.
  **Constraint violations are not covered this way**: no fixture forces a
  genuine `UNIQUE`/FK violation through the schema yet.
  `IMPLEMENTATION.md` rule 5's per-line exclusion tag is reserved for
  whatever, after this, still provably can't be reached this way.

## Expected strain points

- Expiry sweeps are a **time-driven background job** — no client action
  triggers them. Keep the rung-1 answer primitive (a plain periodic timer
  in the app-layer bootstrap, dispatching through an internal client — see
  the journal design decision above); the real background-job pattern
  arrives in [`bookmarks`](../bookmarks).
- File attachments (MicroBin supports uploads) are **out of scope** — blobs
  through a JSON protocol are rung 4/8's problem.

## Definition of done

- [x] **Desktop client against local and remote backends.**
  `ladder_pastebin_gui` in both modes, driven manually against a real
  `ladder_pastebin_server` (create → list → open → burn → delete) and by the
  offscreen QML engine-load smoke test in the suite.
- [~] **WASM client, same client code.** `gui_wasm/main_wasm.cpp` is the only
  file that differs from the desktop client: the presenters, the forms
  controller, the QML adapters (`gui_lib/paste_qml_bridges.hpp`), the schema
  document and `gui/qml/Main.qml` are all shared verbatim — no shadow headers,
  no WASM variant of any model/DTO/QML file
  ([`../TESTING.md`](../TESTING.md)'s hard requirement). **It has never been
  compiled.** No Emscripten toolchain existed in the environment it was
  authored in (`emcmake: command not found`), exactly as rung 0's own
  [`../common/wasm_spike`](../common/wasm_spike) records for the spike it rides
  on. What *was* verified locally: every shared translation unit plus
  `main_wasm.cpp` compiles with `__EMSCRIPTEN__` and `MORPH_CLIENT_ONLY`
  defined and the Lightweight/ODBC include paths removed — the client's include
  graph is genuinely persistence-free. What was not: the Qt for WebAssembly
  toolchain, the link, and the browser.
  `.github/workflows/wasm-ladder.yml` is the compile gate that will settle it.
- [x] **`examples/common/testkit` used throughout.** `BackendRig`'s
  Local/Simulated/Socket matrix, `pump`/`pumpUntil` discipline, `DbFixture`
  per test case, `DbBusyFixture` for the `SQLITE_BUSY` branches.
- [x] **Presenter-shaped GUI.** `ladder_pastebin_gui_lib` links `Qt6::Core`
  only (presenter rule 1); `PastePresenter` is tested in all three backend
  modes.
- [x] **Burn-after-read and expiry work**, with the atomicity mechanism, its
  `RETURNING` limitation and the ladder-wide journal position documented above.
- [x] **Model unit tests**, following [`../bank/tests`](../bank/tests)
  conventions: 40 cases (`grep -c '^TEST_CASE' tests/test_paste_model.cpp`)
  covering the burn/expiry edges, the `CreatePaste` validation rules, the
  hostile-content corpus replay, size limits, duplicate create, id
  collisions, the fail-open security delta and `hello` version negotiation.
- [x] **Findings filed rather than worked around** — this rung's actual
  product: ten in total, spanning rung 0 through this rung. Eight have since
  been fixed framework-side; their gaps and fixes are described inline
  throughout this README and this rung's own source comments, not
  re-listed here. Two genuine, still-current limitations remain:
  `db_fault_fixture.hpp`'s `SqlScopedLock`-based contention cannot fault an
  ordinary `DataMapper` call (see "Store-error branch coverage" above), and
  the SQLite ODBC driver's `UPDATE ... RETURNING`/`SQLFetch` combination —
  see "Burn-atomicity" above and this rung's `Lightweight` issue tracking it
  upstream.
  Three framework/testkit bugs found on the way were *fixed*, not merely
  filed: JSON control-byte escaping in the action/result codecs, an
  executor-lifetime bug in the shared testkit, and — found by this rung's own
  QML-adapter suite — `QtDrivenMainThreadExecutor::post()`'s zero-delay drain
  timer capturing a bare `this`, which fired into freed storage one
  `GENERATE` iteration later and aborted the process
  (`examples/common/testkit/backend_rig.hpp`, with a regression case in
  `test_backend_rig.cpp`). A fourth bug — `Completion::onError`'s single-slot
  overwrite — was *worked around* at the time rather than fixed:
  `gui/presenter.hpp`'s `track()` folded a subclass's error-display callback
  and the busy-counter decrement into the one `.onError()` slot `Completion`
  then kept, instead of composing two separate calls. `Completion`'s
  `onOk`/`onErr` are now vectors of handlers (multiple `.then()`/`.onError()`
  attaches fan out instead of overwriting), so `track()`'s fold is no longer
  load-bearing — kept as-is since it still works and nothing forces the
  change.
  The sibling-writer half of the first bug above is also fixed: the same
  missing control-byte escaping in `journal/action_log.hpp`,
  `offline/file_offline_queue.hpp` and `session/session_auth.hpp` was closed
  framework-side.

  **On the finding numbers this section used to cite.** The ten findings above
  were filed under a flat global sequence that no longer exists.
  [`../FINDINGS.md`](../FINDINGS.md) now namespaces every id by the rung that
  produced it (`<ns>-NNN-<kebab-slug>.md`) precisely because the flat sequence
  did not survive parallel branches — two disjoint series were allocated
  independently and both merged, so the same number came to mean different
  things on different branches. `docs/findings/` holds only `r5-001`…`r5-004`
  today, and the bare numbers this rung's prose used to carry (`017`, `018`,
  `021`, `023`, `026`) resolve to no file at all. They have been replaced
  throughout by a description of the gap and a pointer to the code that closes
  it, which is what a reader actually needs and what survives a renumbering.
  No finding file has been invented to make an old citation resolve.

### Known gaps, stated rather than smoothed over

- **Findings triage complete.** [`../FINDINGS.md`](../FINDINGS.md)'s "Rung
  exit criteria" makes a rung done when (1) its README's design questions are
  resolved in writing, (2) every named strain test exists — passing or filed
  as a finding, and (3) its findings are triaged (no `open` dispositions
  left). All three are now met: of the ten findings this rung owned or
  inherited, eight have since been fixed framework-side (the framework fixes
  are described inline throughout this README and this rung's own source
  comments, not re-listed here). `db_fault_fixture.hpp`'s store-error
  coverage gap (see "Store-error branch coverage" above) is a genuine,
  still-current limitation, documented there and in
  `examples/TESTING.md`/`IMPLEMENTATION.md` directly rather than as a
  standalone finding. The sqliteodbc `RETURNING`/`SQLFetch` gap (see
  "Burn-atomicity" above) is filed upstream against
  [`Lightweight`](https://github.com/LASTRADA-Software/Lightweight/issues/545)
  and tracked morph-side as
  [`morph#58`](https://github.com/LASTRADA-Software/morph/issues/58) — a
  genuine third-party ODBC driver limitation, not fixable in morph source.
- The WASM client's verification status, above.
- **`ladder-tests` still builds no GUI.** That job's distro Qt is 6.4.2, below
  the 6.5 floor `MORPH_BUILD_FORMS_QML` requires, so it configures without the
  QML module, the desktop client or the smoke test — `morph_add_rung()`
  announces each skip rather than letting them vanish silently. The
  `linux-all-features` job now enables `MORPH_BUILD_LADDER` alongside
  `MORPH_BUILD_FORMS_QML` (it already installs Qt 6.8), so that is where those
  targets are built and that test runs.
- **Registration timing.** `PasteBridge` exposes a `bound` signal
  (`Presenter::trackBound()`, backed by `Bridge::whenBound()`) that settles
  once the registration round trip lands; both clients' `Main.qml` gates its
  bootstrap `refresh()` on it instead of retrying on a timer. `Remote` mode
  still has no connect timeout, so a server that never answers leaves `bound`
  simply never firing and the list pane empty with no terminal error.
- Deferred by design: the convergence assertion (needs rung 3's
  `poll()`/`lastEventId()`), the full hostile-content corpus (a representative
  subset ships), file attachments. Reply-frame loss is deferred for a
  different reason than it once was: the proxy that stages it
  (`examples/common/testkit/fault_proxy.hpp`) has shipped, so what is missing
  is the `CreatePaste` idempotency key that would make the retry's outcome
  differ from the ordinary double-create this rung already asserts — see
  "Duplicate create on retry" above.
