# Ladder testing strategy — GUIs, dual deployment modes, multi-client stress

Every rung of the [application ladder](LADDER.md) ships GUIs that are unit
tested in **both deployment modes** — in-process (GUI + `LocalBackend` in one
process) and client/server (GUI over `QtWebSocketBackend` against a
`RemoteServer`), including **N clients against one server** for stress tests.
This document is the binding convention; rung READMEs reference it instead of
restating it.

**What this machinery actually is (round-7 T4 reframe):** since
[`IMPLEMENTATION.md`](IMPLEMENTATION.md) rule 2 makes presenters
deliberately contentless ("translate and route, never decide"), the
BackendRig / client-pool / convergence stack is not really GUI testing —
it is **a conformance harness for morph's client-side stack** (`Bridge`,
backends, `QtExecutor`, completions, attach/reconnect under a real Qt
event loop), which has zero coverage in the repo today. It is therefore
**owned by the testkit as framework coverage**: the full matrix runs once
per framework surface it conforms, and each rung runs a *thin
instantiation* (its presenters through the rig, one suite per model — not
a per-screen × 3-mode combinatorial matrix). This reframing is also what
keeps the CI cost curve flat. It was derived from what already exists and is proven in the
repo: the recipe in `tests/qt/test_qt_websocket.cpp` (in-test
`QtWebSocketServer` on port 0, `pumpUntil`, N=4 concurrent backends, the
QProcess client harness, the Qt-owning Catch2 `main()`), the pump helpers in
`examples/bank/tests/bank_test_support.hpp`, and the presenter shape of
`examples/bank/gui/controllers/`.

## Current state (verified, 2026-08)

- There are **zero GUI tests** in the repo today. Bank's controllers are
  presenter-shaped but compile only into `bank_gui`, never into `bank_tests`;
  `BankClient` hard-wires `LocalBackend` (`gui/BankClient.cpp`), so the same
  GUI cannot be constructed over a socket; the only GUI check is a
  sleep-pumped screenshot smoke inside `gui/main.cpp`.
- `examples/bank/tests/test_remote.cpp` uses `SimulatedRemoteBackend`, not a
  real socket — and `SimulatedRemoteBackend` dispatches with `ConnectionId 0`
  (no connection scope), so **connection-drop refcounting, `closeConnection`
  semantics, and shared-instance lifetime across disconnect are untestable in
  that mode**. Tests about connection lifetime must run over the real
  WebSocket loopback (or the testkit grows a connection-scoped simulated
  client via `RemoteServer::openConnection()` — a small, recommended
  addition that also makes refcount tests deterministic).
- **No existing test exercises `AllowShared` over the Qt WebSocket
  transport.** The polls rung's harness will be the first — that is itself
  coverage the framework needs.
- Bank is not built in `ci.yml` at all (only `wasm-demo.yml`, tests OFF). The
  ladder needs a `ladder-tests` CI job: `MORPH_BUILD_QT=ON`, rung examples
  on, `QT_QPA_PLATFORM=offscreen ctest` — every mechanism already exists in
  `ci.yml`.

## Presenter architecture (every rung)

1. **Presenters live in a Qt-Core-only static library** —
   `examples/<rung>/gui_lib/` links `Qt6::Core` and morph only; `gui/`
   (QML/Widgets app), `gui_wasm/`, and `tests/` all link `gui_lib`.
   Presenters must instantiate under a plain `QCoreApplication`.
2. **Backend-parameterized app context.** A shared
   `examples/common/gui/AppContext` replaces bank's hard-wired
   `LocalBackend`: `Mode = variant<Local{workers}, Remote{url}>`; it owns
   (in order) the optional worker pool, the `QtExecutor`, and the `Bridge`,
   and exposes `login(principal)` → `setDefaultSession`. Presenters take
   `(Bridge&, IExecutor*)` and **never construct executors or backends
   themselves.** `Remote` is asynchronously connected and exposes
   `ready()`/`onReady(cb)`: presenters (which build `BridgeHandler`s, and a
   `BridgeHandler` constructor registers) **must** be constructed from inside
   `onReady`. `QtWebSocketBackend::registerModelAsync()` queues a
   registration issued before the socket connects and retries it once the
   connection comes up (`docs/spec/core/backend.md`, "Asynchronous
   registration"), so this is no longer the correctness hazard it once was
   — but building presenters/`BridgeHandler`s from inside `onReady` stays the
   simpler ordering to reason about, and is what every rung does.
   `Local` is ready on construction and runs `onReady` inline, so mode-blind
   code can always route through `onReady`.
3. **Observable quiescence.** A common `Presenter` base tracks in-flight
   completions (`track(completion, onOk)` wraps `.then/.onError` in
   begin/end counters) and exposes `bool busy()` + an `idle()` signal.
   Tests never sleep; they wait for `busy() == false`.
4. **Timers live in the view layer.** Presenters expose an explicit
   `poll()`; the QML/Widgets shell owns the `Timer`. Tests call `poll()`
   directly — this is what makes `GetEventsSince` loops deterministic.
5. **Canonical state fingerprint.** Each rung's presenter set exposes
   `stateFingerprint()` (a comparable snapshot) and `lastEventId()`. These
   two hooks are the ladder-wide convention the convergence assertion
   templates over.
6. **QML is bindings-only**; every conditional, format, and validation lives
   in the presenter. Per rung: one offscreen engine-load smoke test (engine
   creates root object, no errors) registered in ctest — not Qt Quick Test,
   and no synthesized-mouse-event flows.
7. **One QML-surface audit per rung** (below). The smoke test in rule 6 loads
   every root with its controller properties null, so it resolves no handler
   name and no delegate key against a real object; the audit is what covers
   that.

## The QML-surface drift guard

`examples/common/testkit/qml_surface.hpp` — `QmlSurfaceAudit`.

QML binds a bridge **by string**. `page.tagController.refresh()` and
`function onListed(rows)` inside a `Connections` block both resolve at run
time, against an object the compiler never sees. A renamed `Q_INVOKABLE`, a
`Q_PROPERTY` whose name changed while its getter did not, a handler for a
signal that no longer exists: none is a compile error, none is a QML warning,
and none is visible to the rule-6 smoke test, which supplies no controllers at
all. The failure is a pane that quietly stays empty.

The audit reads the rung's own `gui/qml/*.qml` from the source tree
(`MORPH_LADDER_SOURCE_ROOT`, already compiled into every rung's test binary)
and makes those files the expectation. It needs no QML engine and no
`Qt6::Quick`, so it runs even in configures built without
`MORPH_BUILD_FORMS_QML`.

```cpp
QmlSurfaceAudit audit{QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/<rung>/gui/qml")};
audit.bind(QStringLiteral("tagController"), tags);          // every file
audit.bindIn(QStringLiteral("LedgerView.qml"),              // one file only
             QStringLiteral("bridge"), ledgerBridge);
const QStringList findings = audit.run();
INFO(findings.join(QStringLiteral("\n")).toStdString());
CHECK(findings.isEmpty());
```

**Covers**, in both directions at once:

- a name QML binds that the bridge does not have — the direction a
  hand-written metaobject checklist structurally *cannot* cover, since its
  expectation is a transcription of the same QML;
- a bridge member no QML binds;
- argument-count disagreement at a call site, and a handler declaring more
  parameters than its signal carries;
- a `Connections` block whose target alias was never bound — i.e. a bridge
  the audit was silently not handed.

**Does not cover:** argument *types* (QML is dynamically typed there);
property-bag keys inside an emitted `QVariantMap` (no metaobject exists for
them — the per-rung "bag shape" cases remain the only guard); QML the rung
does not own, such as the shipped `MorphForms` renderer's; dynamic member
access; and whether the shell wires an alias to the class the test bound.

`allowUnbound(alias, member, reason)` exempts one member, with a required
reason. The exemption list is itself audited — a member that has since been
deleted, an alias nobody bound, or a member QML does bind is a finding — so it
can only shrink deliberately.

Adopted by `bookmarks`, `pastebin`, `polls` and `ledger`; `lims`, `kanban`
and `bank` are tracked in morph#240. The audit's own
mutation suite is `examples/common/testkit/test_qml_surface.cpp`: every case
there drives it against a deliberately broken pair and asserts the specific
finding.

## The dual-mode fixture

`examples/common/testkit/backend_rig.hpp` provides
`BackendRig{Mode, nClients, authorizer, serverConfig}` with three modes,
selected by Catch2 `GENERATE` so **one test body runs in every mode**. The
last two arguments are optional and apply to `Socket` mode only: `authorizer`
is threaded into the `RemoteServer`, `serverConfig` is the
`QtWebSocketServerConfig` handed to the `QtWebSocketServer` (frame-size cap,
connection cap, rate limit, timeouts) — how a rung tests a transport-enforced
limit without standing up a second server beside the rig's own.

- **`Local`** — one `ThreadPoolExecutor{4}`, one
  `Bridge{LocalBackend}`; N "clients" are N presenter sets over the shared
  bridge (morph's in-process multi-handler semantics).
- **`LocalSingleThread`** — `LocalBackend` running models on the GUI
  executor itself: the **WASM constraint-parity mode** (exactly bank's
  `__EMSCRIPTEN__` wiring). Catches models that block the UI thread and
  single-thread re-entrancy bugs in every ordinary test run.
- **`Socket`** — `ThreadPoolExecutor{2–4}` → `RemoteServer` (authorizer
  injectable) → `QtWebSocketServer{*server, 0}` (ephemeral port via
  `.port()`) → per client: `QtWebSocketBackend` + `waitForConnected()` +
  its **own `Bridge`**. All clients on the one Qt main thread — proven at
  N=4 in `tests/qt/test_qt_websocket.cpp`.

Caveats the fixture encodes: only `Socket` mode exercises the server-side
shared-instance directory and connection scopes — tests asserting directory
behavior are tagged `[socket-only]`; N-threads-hosting-backends is not
possible today (`QtExecutor` posts to `QCoreApplication::instance()` only);
true process separation reuses the QProcess pattern
(`tests/qt/qt_test_client_main.cpp`) via `process_pool.hpp`, with each rung
shipping a small headless-client binary that drives its *presenters*, not
raw handlers.

`rig.socketBackend(i)` hands out the raw `QtWebSocketBackend` for a client,
for the handful of transport-level operations that have no `Bridge`-level
equivalent — `negotiateProtocolVersion()` (the `hello` handshake) is the
motivating one. Everything that merely dispatches actions should use
`client<Model>()` / `bridge()` instead.

Teardown order (encoded in `~BackendRig`): presenters → client bridges →
`wsServer.closeGracefully(2s)` → server → **pools, and only then the
client-facing executors**. That last step is load-bearing rather than
cosmetic: in `Local` mode a worker thread resolves a `Completion` by posting
to the client executor, so an executor destroyed while the pool still has
threads running leaves the next completion posting through a dangling
`IExecutor*`. The crash surfaces nowhere near the rig — the stale callback
sits on the Qt event loop and detonates inside whatever later test pumps it.
Any object that owns both a pool and an executor the pool's completions
target (a rung's app bootstrap, for instance) needs the same ordering, plus a
way for a test to observe that its dispatches have *settled* — not merely
that their effect is visible — before it is destroyed.

## Pumping discipline — no sleeps

The Qt event loop is the single pump for GUI tests (`QtWebSocketBackend`
requires the Qt loop thread; `MainThreadExecutor::runFor` blocks for its
full wall-clock step even when idle). `examples/common/testkit/pump.hpp` is
the **only** sanctioned wait surface:

- `pumpUntil(pred, deadline)` — bounded `processEvents` slices; deadline
  defaults to 5 s, scaled by `MORPH_LADDER_DEADLINE_MS`.
- `awaitQt<T>(Completion<T>)` — resolve one completion via the pump,
  rethrow errors.
- `settle(presenter)` — `pumpUntil(!busy())`.

A `sleep_for` outside `pump.hpp` is a review-rejectable defect. The test
binary uses the Qt-owning `main()` (QCoreApplication + `Catch::Session` +
DeferredDelete drain) copied from `tests/qt/test_qt_websocket.cpp`.

`pump.hpp` covers waiting on the *Qt loop*. Waiting on a **background job**
has its own answer, and it is not a wait at all:

- `step_executor.hpp` — `StepExecutor`, an `IExecutor` that queues posted
  tasks and runs them only on `runOne()`/`runAll()`. Substituted for the
  `ThreadPoolExecutor` a model or App would otherwise own, it turns
  submit-then-poll into an exact sequence: submit, `CHECK(pending() == 1)`,
  `runOne()`, assert done. The negative half — "the worker has **not** run
  yet" — is assertable only this way; against a real pool it can only be
  sampled. `runAll()` picks up tasks a running task posts, so a chained job
  runs to completion instead of stranding its own continuation, and is
  bounded so a self-reposting task fails loudly rather than hanging.
  It mirrors `morph::testing::StepExecutor` (`tests/test_support.hpp`), which
  the framework's own suite has always had; the ladder copy exists because
  that header has no reachable include path from `examples/`.

A test that keeps a real `ThreadPoolExecutor` under an async job — because it
is covering the production wiring, or the fact that the worker runs on a
genuinely different thread with no session context — says so at the test case
and pays the retry loop knowingly. `examples/ledger/tests/test_ledger_reports.cpp`
keeps exactly one such case and converts the rest.

## Multi-client stress harness

Testkit components, with the rung that **first needs** each (this ordering
is load-bearing — earlier rungs must not claim later components in their
DoD):

| Component | First needed by |
|---|---|
| `testkit_main.cpp`, `pump.hpp`, `backend_rig.hpp`, `db_fixture.hpp`, `db_fault_fixture.hpp`, **fault proxy + strand interleaver** (pulled forward, round-7) | rung 0/1 |
| `client_pool.hpp`, `convergence.hpp` | rung 3 |
| `action_driver.hpp`, `process_pool.hpp`, `offline_rig.hpp` | rung 4 |
| `step_executor.hpp` | rung 5 |

- `db_fault_fixture.hpp` — holds a real `Lightweight::SqlScopedLock` on a
  second, independent `SqlConnection` to the shared test database, producing
  genuine cross-session contention for code that itself takes the *same
  named* advisory lock on a different connection. **This is not a failing
  ODBC-level driver, and cannot fault an ordinary `DataMapper` call**:
  `Create`/`Update`/`Query`/`Delete` and a plain `SqlTransaction` commit sit
  entirely outside the advisory-lock protocol, so this fixture is
  transparent to them — no `SQLITE_BUSY`, no constraint violation, no
  rollback. There is no injectable seam between Lightweight's `DataMapper`
  and the ODBC driver (no `SqlConnection` interface to substitute, no
  statement hook to fail), so a driver-level fault fixture is not on offer;
  see `IMPLEMENTATION.md` rule 5 for what the 100%-coverage rule actually
  requires instead.
- `db_busy_fixture.hpp` — the `SQLITE_BUSY` answer: a genuine, uncommitted
  `BEGIN IMMEDIATE` write transaction held open on a second `SqlConnection`,
  so a concurrent write from the connection under test collides for real
  and SQLite returns a real `SQLITE_BUSY` — no mock driver, the failure
  happens in the same call path production takes. Two empirically-verified
  gotchas its own doc comment records: `BEGIN IMMEDIATE` is required (a
  plain `Lightweight::SqlTransaction` only flips `SQL_ATTR_AUTOCOMMIT` and
  defers lock acquisition, producing no contention), and Lightweight's
  unconditional `PRAGMA busy_timeout = 60000` in `PostConnect()` means the
  *other* connection must re-issue a small timeout of its own or the
  "failure" is a sixty-second block instead of an immediate error.
  **Store-error coverage is obtained per failure class, through the real
  schema, by whichever fixture can genuinely provoke that class** — not from
  one failing driver. Constraint violations and mid-transaction rollback
  still have no general fixture; extending `db_busy_fixture.hpp`'s pattern
  (a conflicting row for a `UNIQUE`/FK violation, a dropped table for a
  query error) is the next step whenever a rung's model needs that
  coverage.

- `db_fixture.hpp` — one real, on-disk database shared per test *binary*
  (`morph_ladder_test.db` in the binary's working directory, or
  `ODBC_CONNECTION_STRING` if set), reset between test cases by dropping every
  table and re-applying the registered migrations. This mirrors Lightweight's
  own `SqlTestFixture` and bank's `ensureDatabase()`; a `DataMapper` needs a
  real connection, so a per-fixture temp file would buy isolation at the cost
  of re-opening and re-migrating a database per test case. Isolation across
  *binaries* comes from ctest's per-target working directory; isolation within
  a binary comes from the drop-and-reset, which is why the ladder's
  `catch_discover_tests` calls give their tests a `RESOURCE_LOCK` — two
  DB-touching cases from one binary must never run concurrently under
  `ctest -j`.
- `client_pool.hpp` — typed pool constructing each client's presenters
  against `rig.client(i)`; test bodies are mode-blind.
- `convergence.hpp` — `requireConverged(clients, deadline)`: round-robin
  `poll()`, wait all-idle, compare `stateFingerprint()` across clients
  (optionally against an oracle client's server truth); on deadline, dump
  every client's fingerprint diff. **Honesty note**: in `Local`/
  `LocalSingleThread` modes all "clients" share one bridge — there is no
  staleness to converge from, so convergence is effectively
  `[socket-only]` coverage; don't count Local-mode runs. The
  `poll()`/`lastEventId()` hooks it needs exist only from rung 3 on —
  rungs 0–2 use `settle()` + fingerprint equality without event cursors.
- `action_driver.hpp` — `SeededScript`: seed from `MORPH_STRESS_SEED`
  (always printed on failure), weighted action generators, schedule computed
  up front; per-burst invariant hooks (kanban: positions dense/unique;
  ledger: legs sum zero; polls: counts match the event log).
- **N = 4–8 in-process clients** is the meaningful range (beyond ~8 sockets
  on one pumped thread you add queueing latency, not new interleavings);
  scale via `MORPH_LADDER_CLIENTS` / `MORPH_LADDER_ACTIONS` env vars
  (soak-suite convention) — same CI run, no separate schedule. Kanban's
  stress case (`test_kanban_stress.cpp`, `[kanban][stress][tsan]`) runs at
  N=4 — **against a bare `morph::bridge::Bridge`/`morph::backend::
  LocalBackend` on a real `ThreadPoolExecutor`, never `BackendRig`**: the
  repo's CI deliberately keeps Qt stacks out of the `clang-asan`/
  `clang-tsan`/`clang-ubsan` sanitizer legs ("a GUI stack under TSan is
  mostly noise"), and this test's own `Bridge`/`LocalBackend` construction
  has zero Qt frames in its call graph (unlike `BackendRig`'s `Mode::Local`,
  which always builds a real `morph::qt::QtExecutor` for client callback
  delivery — see `docs/superpowers/plans/2026-08-19-kanban-tsan-ci-findings.md`
  for why this test stopped using it), so this test exercises models +
  strands, not sockets or Qt, which lets it run under real ThreadSanitizer
  without pulling Qt/QML into that matrix. A
  dedicated CI job, `kanban-tsan` (`.github/workflows/ci.yml`), builds only
  `MORPH_LADDER_RUNGS=kanban` under the `clang-tsan` preset and runs this one
  test with `ctest -R ThreadSanitizer`, instrumented with `-fsanitize=thread`
  — the real TSan coverage. This same test also still runs, uninstrumented
  for TSan, in two other legs that build the ladder without `AF_SANITIZER`:
  the ordinary `ladder-tests` job (`gcc-debug`; its `-LE stress` filter is a
  no-op since no ctest label named `stress` exists — see below), and the
  `clang-coverage` leg of the `linux-sanitizers` matrix job (unlike its
  `clang-asan`/`clang-tsan`/`clang-ubsan` siblings, `clang-coverage` does
  build the full ladder — `MORPH_LADDER_RUNGS=all` — for coverage numbers,
  and its `ctest` run applies no stress exclusion). Both of those runs are
  harmless and redundant, not sanitizer coverage; only `kanban-tsan`'s run
  is. Server-scale load (hundreds–thousands of sockets) is
  rung 8's load *script*, not a unit test.
- `offline_rig.hpp` — scripted connectivity: drop by closing/destroying the
  in-test `QtWebSocketServer`, revive on the same port (proven pattern);
  hand-cranked signals into `ReconnectCoordinator`; queue inspection.
- `process_pool.hpp` — QProcess clients for rung-8 scale **and for
  client-crash tests**: kill a client process mid-execute / mid-attach and
  assert connection-scope reclamation under abnormal teardown (distinct
  from graceful disconnect). First consumer:
  `kanban/tests/test_kanban_process_separation.cpp`, which hosts the server
  in the test process (so assertions read `RemoteServer::health()` directly
  rather than over IPC) and spawns `ladder_kanban_headless` as its clients.
  Note `ProcessPool::allExited()` is a predicate for `pumpUntil`, not a
  blocking wait: blocking in `QProcess::waitForFinished` with an in-process
  server stops the loop that server needs to answer the very clients being
  waited on.

Per-rung test naming: `test_model_<entity>.cpp` (full mode matrix),
`test_gui_<screen>.cpp` (presenter tests, full matrix),
`test_gui_qml_smoke.cpp`, `test_multiclient.cpp` `[stress]`,
`test_offline.cpp` (rungs 4/6/7).

## The fault-injection wire proxy (and the strand interleaver)

The single highest-yield harness the ladder needs and the repo lacks: an
in-process WebSocket proxy between `QtWebSocketBackend` and
`QtWebSocketServer` with scriptable rules — *drop exactly the reply frame of
call k*, delay, duplicate, kill mid-replay. Exactly-once tests (kanban,
ledger), dead-letter tests, and reconnect-mid-replay tests are demos, not CI
tests, without it. `SimulatedRemoteBackend` is lossless and unscoped; the
soak tests flap a boolean, not a socket. **Built at rung 0–1** (pulled
forward by the round-7 review — it outperforms whole rungs on finding
yield), so rung 1's "duplicate create on retry" test can use true
reply-frame loss from the start; the double-execute approximation is only
the fallback if the proxy slips.

Companion harness from adversarial review: a **deterministic-schedule
strand interleaver** — without it, strand-ordering bugs (kanban's
`MoveTaskPosition` centerpiece) remain probabilistic stress runs rather
than reproducible interleavings.

## End-to-end user journeys

Every layer above verifies a slice and assumes the surrounding *sequence*
away. Authentication is the clearest case: rigs arrive already
authenticated, so a sign-in that fails and is then retried — close to the
most common real interaction there is — appears in no other test.

The stress harness is worth distinguishing explicitly, because it looks
like it covers this and does not. `SeededScript` picks actions by weight
from a seeded RNG to shake out races. That is adversarial fuzzing; it is
deliberately *not* plausible user behaviour, and it asserts structural
invariants rather than whether a workflow produced the outcome a user
would expect.

`testkit/journey.hpp` adds the missing layer: a named, ordered sequence of
user intents with assertions between the steps, run over the whole
`Local`/`LocalSingleThread`/`Socket` matrix and required to produce the
same outcome in each. Server and payloads only — no QML engine.

```cpp
Journey{"sign-in"}
    .step("acting before signing in is rejected, not silently allowed", [&] { ... })
    .step("signing in with a malformed username is rejected", [&] { ... })
    .step("the rejected sign-in left nothing behind", [&] { ... })
    .step("signing out ends the session", [&] { ... })
    .run();
```

A failing step reports *which* step and the trail that reached it, rather
than a bare assertion far into a long body; a step that throws is reported
as that step's failure rather than escaping as an unhandled exception
naming only the test case.

What only a sequence catches: state leaking between steps, a failed step
corrupting what follows, an error path that leaves the client wedged, a
session that outlives sign-out. Running the same journey across modes also
surfaces divergence the per-mode tests cannot see — kanban's own sign-in
journey found that an unauthenticated call is refused by the *model* under
`Local` ("no authenticated principal") but by the server's authorizer under
`Socket` ("unauthorized"), same outcome, different wording.

Journeys live in `examples/<rung>/tests/journeys/` and carry a `journey`
ctest label (from the test name's `Journey: ` prefix), so they can be
selected with `ctest -L journey` or excluded the way `stress` is.

## Scenario files against a running server

A journey is the right *idea* but it is C++: compiled into the test binary,
driving the model through a `Bridge`, with the server started by the fixture.
`scripts/scenario/` is the same idea as data — a plain-text file of steps and
expected outcomes, run by a Python client that connects to a
`ladder_<rung>_server` somebody already started, speaks the wire protocol from
`docs/spec/core/wire.md`, and links against nothing:

```
model PasteModel
client alice
do CreatePaste content="hello world" syntax=plaintext
expect ok capture id=$.id
do GetPaste id=$id
expect ok field content == "hello world"
```

What it adds that neither journeys nor `process_pool.hpp` do: the client's
behaviour is not fixed at build time, so a bug report can arrive *as a file*;
and the protocol is implemented independently of morph's C++, so a defect
symmetric on both sides of morph's own client/server pair is visible to it.
Envelopes can also be hand-built (`send`), which is how a wrong
`protocolVersion` or somebody else's `modelId` gets exercised at all — a typed
C++ client cannot express them.

What it is not: a replacement for any of the above. Model behaviour stays
in-process, and this runs no server lifecycle of its own — see
`scripts/scenario/README.md` for the format and the deliberate omissions.

## WASM reality

Honest position: **WASM GUIs cannot be unit-tested in CI today.** The
three-layer answer, per rung:

1. **`LocalSingleThread` mode natively** — same presenters, WASM-shaped
   wiring, every test run.
2. **Compile gate** — CI builds the rung's client for wasm32-emscripten so
   shared GUI code can't drift. Shipped as `.github/workflows/wasm-ladder.yml`
   (emsdk + a Qt-for-wasm kit, `-DMORPH_CLIENT_ONLY=ON`); the per-rung target
   wiring is `morph_add_rung()`'s `gui_wasm` block, not a per-rung
   `CMakeLists.txt` the way bank's is.
3. **One scripted browser smoke** (emrun + Playwright against the built
   demo) as an optional stage in the same CI run.

Open framework facts every rung must respect (verified):

- A WASM client over `QtWebSocketBackend` has still never been *run* — but it
  is now **compiled** on every qualifying PR. Rung 0 wrote the spike and rung 1
  wrote a real client over it (`examples/pastebin/gui_wasm`); neither could be
  compiled when this bullet was written, because no Emscripten toolchain existed
  in either authoring environment. The compile gate anticipated here has since
  landed: `.github/workflows/wasm-ladder.yml`'s "Build the ladder's WASM
  clients" job builds the spike and rungs 1–3's clients by name under
  `-DMORPH_LADDER_RUNGS=all`, and runs green. So "does it build" is answered;
  "does it work in a browser" is still not.
- The plain registration path is only WASM-safe with
  **`asyncRegistrationEnabled = true`, which is opt-in and off by
  default**; with defaults, the first `registerModel` aborts the page.
- **`waitForConnected()` hangs the page on WASM** — the WASM client must
  use the `setConnectHandler` pattern (#39) instead; the Socket rig's
  `waitForConnected()` recipe is for *native* tests only.
- The **synchronous shared/keyed attach path
  (`registerModelShared`/`attachModel`) nests an event loop that aborts the
  page on WASM** — that part still holds, and a WASM client must not call it.
  What has changed is the remedy: async attach is **no longer a missing
  framework prerequisite**. `IBackend::registerModelSharedAsync` and
  `IBackend::attachModelAsync` (`include/morph/core/backend.hpp`) ship the
  non-blocking counterparts, `Bridge::ensureBoundAsync`/`attachHandlerAsync`
  dispatch to them, and `QtWebSocketBackend` implements both. A rung's WASM
  story uses those rather than waiting on the framework. (The rung-1 coupling
  the pastebin README calls out — burn atomicity via a shared keyed instance —
  is likewise no longer gated on this.)

## Build system and CI (proven by rung 0)

Build wiring (from delivery review; today each example is hand-added in the
root `CMakeLists.txt` — don't repeat that eight times):

- One `examples/CMakeLists.txt`; one `MORPH_BUILD_LADDER` bool plus a
  `MORPH_LADDER_RUNGS` cache list (`"all"` or `"pastebin;kanban"`) — no
  per-rung booleans; the list maps 1:1 to CI path filters.
- **The rung names themselves live in `examples/rungs.txt`, and nowhere
  else.** That invariant above — "maps 1:1 to CI path filters" — was
  documented long before anything enforced it, and it did not hold: the rung
  list was hand-copied into five places, and every copy that was not
  load-bearing eventually drifted. CI's `ladder-tests`/`ladder-sanitizers`
  path filter stopped at `kanban` (rung 4), so a change confined to
  `examples/ledger/` or `examples/lims/` matched nothing and skipped both
  jobs — including the only job in the repository that sanitizer-instruments
  a rung. Nothing reported it: a path filter that matches nothing succeeds
  exactly as loudly as one that correctly found nothing to do (morph#179;
  `scripts/coverage.sh` and `codecov.yml` had drifted the same way in
  morph#141). The list is now structured so it cannot:

  - `examples/rungs.txt` is the single authority: one bare rung name per
    line, ASCII, whole-line `#` comments. A line that is neither is a hard
    error in every reader rather than a line quietly skipped.
  - `examples/CMakeLists.txt` reads it, and **refuses to configure** if any
    `examples/<dir>/CMakeLists.txt` calls `morph_add_rung(NAME <x>)` for an
    `<x>` the file does not list. This is what makes the authority
    load-bearing: a rung cannot exist unlisted.
  - `scripts/ladder_rungs.sh` is the shared reader. `list` prints the rung
    names; `ci-path-regex` prints the whole changed-paths regex, rungs plus
    the non-rung paths that must also trigger the ladder. Both
    `ladder-tests` and `ladder-sanitizers` call it, so their filters are one
    expression and cannot diverge from each other or from the list.
    `scripts/coverage.sh` and `wasm-ladder.yml`'s named-target build loop
    read it too.
  - Two consumers cannot read it, and are checked against it from outside by
    `scripts/check_rung_filters.sh`, run unconditionally by
    `.github/workflows/drift-guard.yml`: `wasm-ladder.yml`'s
    `on.push.paths`/`on.pull_request.paths` (GitHub evaluates these to decide
    whether to start the workflow, before any step exists to generate them)
    and `codecov.yml`'s per-rung components (read by Codecov, not by us).
    The checks are behavioural where the semantics can be reproduced — a
    rung passes only if a real path under its directory actually matches the
    filter — because a grep for the rung's name would pass on a filter that
    had been rewritten into one matching nothing. `scripts/test_check_rung_filters.sh`
    reintroduces each drift into a scratch copy of the tree, one at a time,
    and asserts the gate catches it for the stated reason.

  Adding a rung is therefore: add the name to `examples/rungs.txt`, and add
  two lines to `wasm-ladder.yml` plus a component to `codecov.yml` — the two
  the guard will name explicitly on the same PR if you forget.
- `examples/common/` declares exactly three consumable targets:
  `morph_ladder_testkit` (morph + Catch2 + Qt), `morph_ladder_gui` (STATIC,
  `Qt6::Core` only, **no Catch2**, **no `Qt6::WebSockets`** — presenter rule
  1), and `morph_ladder_app` (STATIC, `AppContext` only: the deployment-mode
  layer, which needs `morph::qt`/`Qt6::WebSockets` for `Remote` and is
  therefore kept out of `morph_ladder_gui`). A rung's `gui_lib` links
  `morph::ladder_gui`; the shells that choose a backend (`gui/`, `gui_wasm/`,
  `tests/`) also link `morph::ladder_app`. Rungs link targets, never paths;
  the testkit never grows per-rung options.
- A `morph_add_rung()` function creates `ladder_<rung>_{lib,gui_lib,gui,
  gui_wasm,tests,headless}` with `catch_discover_tests` + ctest labels
  (`ladder`, `ladder-<rung>` — Catch2's own tags like `[stress]`/`[tsan]`
  are not translated into ctest labels anywhere in this repo; select on
  them with `ctest -R` against the test name instead), warnings and
  sanitizers **applied to all app code** (bank skips both repo-wide because
  its ORM headers aren't `-Werror`-clean — the ladder scopes any such
  relaxation to the `db/` entity targets only, since persistence goes
  through the same Lightweight ORM per
  [`IMPLEMENTATION.md`](IMPLEMENTATION.md)), AUTOMOC, and a TIMEOUT on
  every binary. Sanitizers are opt-in per `AF_SANITIZER` (set by the
  `clang-asan`/`clang-tsan`/`clang-ubsan` presets), applied with the same
  `if(DEFINED AF_SANITIZER) apply_sanitizers(<target> ${AF_SANITIZER})
  endif()` guard `AF_COVERAGE` uses for `apply_coverage()` — every ladder
  target that reaches a rung's models or tests carries this guard, so a
  `--preset clang-tsan` configure of the ladder actually instruments the
  code it builds (`.github/workflows/ci.yml`'s `kanban-tsan` job is the
  first CI leg that exercises this). Lightweight's `FetchContent`
  acquisition is hoisted once into `examples/common`, not repeated per
  rung. One trap when implementing it: `catch_discover_tests` cannot carry
  a **multi-value** `LABELS`. It forwards `PROPERTIES` as a flat list
  through a `-D VAR=a;b;c` command line
  where no escaping survives, so `LABELS "x;y"` does not make a two-label
  test — it shifts every following name/value pair by one, silently dropping
  the rest. `examples/common/CMakeLists.txt` shows the working shape: one
  value per property name in the `catch_discover_tests` call, plus a
  generated `TEST_INCLUDE_FILES` post-pass for the extra labels.
- Do **not** copy bank's `gui_wasm` shadow-header pattern — with the
  `gui_lib` split it is unnecessary, and copying it makes the WASM and
  native builds different programs, silently falsifying the "same client
  code" DoD. One WASM configure builds all rungs' `gui_wasm` targets
  (`.github/workflows/wasm-ladder.yml`, which also builds rung 0's spike; it
  caches emsdk but has no compiler cache yet).

  **What rung 1 learned doing this for real** (the `gui_lib` split is
  necessary but not sufficient): a client's presenters are
  `BridgeHandler<Model>` templates, so a WASM client still *names* its rung's
  model type and therefore still includes its model header. Every rung's
  models acquire their `Lightweight::DataMapper` connection per `execute()`
  call from `Lightweight::GlobalDataMapperPool()` (rather than a model
  owning one via a `WithMapper`-style mixin member — the pattern this
  section used to document before that mixin was removed in favor of the
  pool), so the model *header* itself has no Lightweight/ODBC dependency to
  begin with — only the model's `.cpp` (where the real query/transaction
  bodies live) does. Configure the WASM build with
  **`-DMORPH_CLIENT_ONLY=ON`** (removes the registrars that closure over the
  model's ODBC-backed bodies — `docs/spec/core/registry.md`;
  `morph_add_rung()` fails the configure with that explanation if it is
  missing) and that `.cpp` is never compiled for Emscripten at all
  (`cmake/morph_add_rung.cmake`'s `if(NOT EMSCRIPTEN)` guard around
  `ladder_<rung>_lib`'s own creation) — no header-level stub or branch is
  needed on top of that. `include/morph/core/registry.hpp`'s
  `BRIDGE_REGISTER_ACTION_FOR_CLIENT(M, A, RESULT, NAME, ...)` remains
  available for a client willing to make `M` a declaration-only facade type
  instead, closing the header dependency for cases where a model's own
  entity types still need a persistence-free stand-in on the WASM include
  path (see `polls::db::PollRecord` et al.'s own `#ifndef __EMSCRIPTEN__`
  branch, `poll_entity.hpp`) — no rung's *model* header needs this today.
- **Sanitizer wiring.** Every rung's targets and every `examples/common`
  target carry an `if(DEFINED AF_SANITIZER) apply_sanitizers(<target>
  ${AF_SANITIZER})` block, the same shape and placement as their
  `if(AF_COVERAGE) apply_coverage()` block
  (`cmake/morph_add_rung.cmake`, `examples/common/CMakeLists.txt`). Two CI
  jobs consume it:

  - **`Application ladder / ASan+UBSan`** (`ladder-sanitizers`) configures
    `--preset clang-asan` with
    `-DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all`
    and runs `ctest -L ladder -LE stress`, so every rung's tests run under
    both sanitizers. One preset covers both: `apply_sanitizers(<target>
    asan)` compiles with `-fsanitize=address,undefined`
    (`cmake/compiler_options.cmake`), so a separate ubsan leg for the ladder
    would re-run a strict subset. It shares `ladder-tests`' changed-paths
    filter — not a copy of it, but the same generated expression, from
    `scripts/ladder_rungs.sh ci-path-regex`; the two build the same tree and
    differ only in instrumentation, so the filters must not be able to
    diverge. Being the only job here that instruments a rung is also what
    made this job's share of the drifted filter the costly half: rungs 5 and
    6 ran uninstrumented everywhere while it was skipping them. `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer
    reports allocations Qt's platform plugins and QML engine keep for
    process lifetime; the memory-error and UB checks stay on. The job
    asserts (via `nm`) that each `ladder_<rung>_tests` binary really
    contains `__asan_` references before trusting a green run — an
    uninstrumented sanitizer job passes unconditionally and reads as proof
    when it is the absence of proof.
  - **`Kanban / ThreadSanitizer`** (`kanban-tsan`) runs one test under
    `clang-tsan`.

  **TSan is deliberately not applied to rungs wholesale.** A rung's tests
  drive Qt on every path, and against an uninstrumented system Qt that
  yields warnings bottoming out in Qt-internal frames that cannot be
  classified as real races or false positives from outside a
  TSan-instrumented Qt build — morph#128 hit exactly that, 165 warnings
  deep. Thread-sanitising a rung therefore means writing a test that
  constructs no `QtExecutor` at all — driving the model through a bare
  `morph::bridge::Bridge`/`morph::backend::LocalBackend` on a real
  `morph::exec::ThreadPoolExecutor` — and running just that test under
  `clang-tsan`, which is what `kanban-tsan` does.

- **Coverage wiring (proven by rung 0, on `examples/common`; the same
  recipe applies to every future rung's `src/models/`/`include/<rung>/models/`
  per [`IMPLEMENTATION.md`](IMPLEMENTATION.md) rule 5).** The `clang-coverage`
  CI leg is the only *sanitizer-matrix* leg that installs
  `qt6-base-dev`/`qt6-websockets-dev`/`qt6-tools-dev`/`libgl1-mesa-dev` and
  configures with
  `-DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all`
  (of the sanitizer-matrix legs, asan/tsan/ubsan still never build the
  ladder, so this cost is paid once there);
  its `ctest` invocation runs
  with `QT_QPA_PLATFORM=offscreen` since the runner has no display. Every
  ladder CMake target (`morph_ladder_gui`, `morph_ladder_app`,
  `morph_ladder_testkit`, and each rung's own targets) wraps its definition
  in `if(AF_COVERAGE) apply_coverage(<target>) endif()`, the same guard
  `include/morph`'s own targets use. `scripts/coverage.sh` merges multiple
  instrumented binaries into one report via llvm-cov's `-object` flag: one
  `TEST_EXE` positional (the library's `morph_tests`) plus an `OBJECT_ARGS`
  array populated with every other binary that exists in the build
  (`ladder_common_tests` today; a future rung's own test binary joins the
  same array the same way, guarded the same way — `if [ -x "$BINARY" ]` so
  the script keeps working unchanged for a configure that didn't build
  that rung) — and adds `examples/common` (and, per rung once it ships
  models, `examples/<rung>/src/models` + `include/<rung>/models`) to the
  positional source-path filter alongside `include/morph`. AUTOMOC's
  generated `mocs_compilation.cpp` lives under the build tree, never under a
  source-tree path this filter names, so moc output is excluded for free —
  no separate exclusion mechanism needed. The blocking gate itself lives in
  `codecov.yml`'s `component_management.individual_components`: one
  component per path set, `informational: false`, with its `target:` set
  from the measured ceiling per rule 5's coverage-artifact guidance (not a
  blind 100%) — scoped to that component's paths so it never becomes an
  unverified whole-repo claim, leaving the project-wide default status
  `informational: true` as before.

CI tiers (grounded in the existing workflows; unmanaged, the ladder
dominates CI minutes by rung 3). No separate nightly schedule: everything
below that isn't in the weekly tier runs in the ordinary per-push/per-PR
`ladder-tests` job, same as the rest of this repo's CI — a rung's cost is
managed by path-filtering (`MORPH_LADDER_RUNGS` computed from changed paths:
`examples/<rung>/**` → that rung; `examples/common/**` or
`include/morph/**` → all rungs), not by deferring work to an off-hours run:

1. **CI (every push/PR)**: one `ladder-tests` job (clone of `linux-qt`:
   gcc-debug, offscreen, sccache), path-filtered per the `MORPH_LADDER_RUNGS`
   rule above. `ctest -L ladder -LE stress` — full ladder, all modes. The
   `-LE stress` clause is currently a no-op (no ctest label named `stress`
   exists anywhere in this repo's CMake — Catch2 tags are never translated
   into ctest labels, as noted above), so this job also runs kanban's
   `[tsan]`-tagged stress case, uninstrumented, since `gcc-debug` never sets
   `AF_SANITIZER`: harmless and redundant, but no real TSan coverage. The
   `clang-coverage` leg of the `linux-sanitizers` matrix job (below) also
   builds and runs the full ladder — again without `AF_SANITIZER` — so it
   runs the same test a second uninstrumented time. One Playwright browser
   smoke also runs here. One Windows compile-only build (never 8 rungs × 4
   MSVC presets) runs alongside it. ASan is scoped to changed rungs. Real
   ThreadSanitizer coverage of that same test comes from a separate,
   dedicated job (`kanban-tsan`, sibling to `linux-sanitizers`): it builds
   `MORPH_LADDER_RUNGS=kanban` alone under the `clang-tsan` preset and runs
   only that one test via `ctest -R ThreadSanitizer`, instrumented. So the
   kanban TSan-tagged stress test runs three times today — uninstrumented
   inside `ladder-tests`, uninstrumented inside `linux-sanitizers`'s
   `clang-coverage` leg, and instrumented inside `kanban-tsan` — and only the
   last of these is meaningful sanitizer coverage.

   Two pieces of this live outside that job as shipped, for reasons of
   toolchain rather than design. **The GUI half** — each rung's QML module,
   desktop client and offscreen engine-load smoke test — needs
   `MORPH_BUILD_FORMS_QML=ON`, whose Qt 6.5 floor the `ladder-tests` runner's
   distro Qt (6.4.2) does not clear, so it is the `linux-all-features` job
   (Qt 6.8 via aqtinstall) that configures `MORPH_BUILD_LADDER=ON` together
   with `MORPH_BUILD_FORMS_QML=ON`. `morph_add_rung()` announces every target
   it skips on the leg that cannot build them, so the omission is never
   silent. **The WASM compile gate** needs emsdk plus a Qt-for-wasm kit, and
   lives in its own workflow, `.github/workflows/wasm-ladder.yml`.
2. **Weekly**: rung-8 load script (large runner) only — a genuinely
   separate concern from the rest of this tiering (hundreds–thousands of
   sockets, a large self-hosted-class runner), not something that can run
   on every push. Everything else the ladder needs, including sanitizer and
   fuzz-style coverage, runs in the CI tier above; `ci.yml`'s existing
   `valgrind`/fuzz jobs are themselves triggered on every push/PR today
   (there is no scheduled workflow in this repo yet), so nothing in the
   ladder should assume a cadence the rest of the project doesn't have.

## Framework gaps this strategy exposes (candidate issues)

1. Client-side execute deadline — no timeout on `Completion`; a black-holed
   call hangs forever. Every polling helper must wrap its own timer until the
   framework provides one. (A frame refused by `messagesPerSecond` no longer
   belongs on that list: the transport answers it with an `err "rate limited"`
   addressed to the frame's own `callId`, so the caller's `Completion` fails
   rather than hanging — morph#225.)
2. `Bridge::pendingCalls()` (client-side quiescence observability) — makes
   `settle()` exact; today presenter-level counters substitute.
3. `MainThreadExecutor::runOnce()/drain()` — a step, not a wall-clock pump.
4. `QtExecutor` with an optional `QObject*` context target — per-thread
   affinity for future N-thread client topologies.
5. Connection-scoped simulated client (via `RemoteServer::openConnection()`)
   — deterministic connection-lifetime tests without sockets.
6. Injectable time source usable by *remotely-constructed* (registry
   default-constructed) models — until then, rungs use a process-global
   now-provider set by tests (`examples/common` clock interface).
