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
   `onReady`. Registering before the socket connects fails permanently, with
   no retry — see
   [`017-async-registration-fails-before-connect.md`](../docs/findings/017-async-registration-fails-before-connect.md).
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

## Multi-client stress harness

Testkit components, with the rung that **first needs** each (this ordering
is load-bearing — earlier rungs must not claim later components in their
DoD):

| Component | First needed by |
|---|---|
| `testkit_main.cpp`, `pump.hpp`, `backend_rig.hpp`, `db_fixture.hpp`, `db_fault_fixture.hpp`, **fault proxy + strand interleaver** (pulled forward, round-7) | rung 0/1 |
| `client_pool.hpp`, `convergence.hpp` | rung 3 |
| `action_driver.hpp`, `process_pool.hpp`, `offline_rig.hpp` | rung 4 |

- `db_fault_fixture.hpp` — a failing ODBC-level driver for exercising
  store-error branches (`SQLITE_BUSY`, constraint violations, rollback)
  that the 100%-coverage rule requires (see
  [`IMPLEMENTATION.md`](IMPLEMENTATION.md) rule 5); wire-level faults are
  the proxy's job, database faults are this fixture's. **As shipped in rung
  0 this promise is not yet satisfiable**: the fixture holds a real
  `SqlScopedLock` on a second connection, so it can only fault code that
  takes the same named advisory lock — not an ordinary `DataMapper`
  `Create`/`Update`/`Query` or a `SqlTransaction`. Closing that gap (extend
  the fixture, or narrow this promise) is
  [`018-db-fault-fixture-cannot-fault-datamapper.md`](../docs/findings/018-db-fault-fixture-cannot-fault-datamapper.md),
  owned by whichever rung first needs store-error branch coverage.
- `db_busy_fixture.hpp` — rung 1's answer to the paragraph above, for the
  `SQLITE_BUSY` class specifically: a genuine, uncommitted `BEGIN IMMEDIATE`
  write transaction held open on a second `SqlConnection`, so a concurrent
  write from the connection under test collides for real. **Store-error
  coverage is obtained per failure class, through the real schema, by
  whichever fixture can genuinely provoke that class** — not from one failing
  driver. Constraint violations and mid-transaction rollback still have no
  general fixture. Finding 018 is triaged `documented-limitation` on exactly
  that reading; its closing section is the authoritative account of what
  shipped, and the two `db_fault_fixture` promises (here and in
  [`IMPLEMENTATION.md`](IMPLEMENTATION.md) rule 5) are the part now known to
  be inaccurate.

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
  stress case runs under ThreadSanitizer
  at N=4 — **in `Local` rig mode on `ThreadPoolExecutor`**: the repo's CI
  deliberately keeps Qt stacks out of the sanitizer matrix ("a GUI stack
  under TSan is mostly noise"), so the TSan leg exercises models + strands,
  not sockets. Server-scale load (hundreds–thousands of sockets) is rung
  8's load *script*, not a unit test.
- `offline_rig.hpp` — scripted connectivity: drop by closing/destroying the
  in-test `QtWebSocketServer`, revive on the same port (proven pattern);
  hand-cranked signals into `ReconnectCoordinator`; queue inspection.
- `process_pool.hpp` — QProcess clients for rung-8 scale **and for
  client-crash tests**: kill a client process mid-execute / mid-attach and
  assert connection-scope reclamation under abnormal teardown (distinct
  from graceful disconnect).

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

- Bank's WASM build is **local-only** — a WASM client over
  `QtWebSocketBackend` has still never been *run*. Rung 0 wrote the spike and
  rung 1 wrote a real client over it (`examples/pastebin/gui_wasm`), but
  neither was ever compiled: no Emscripten toolchain existed in either
  authoring environment. The compile gate above is what will change this
  sentence; until it has run green, treat both as unverified.
- The plain registration path is only WASM-safe with
  **`asyncRegistrationEnabled = true`, which is opt-in and off by
  default**; with defaults, the first `registerModel` aborts the page.
- **`waitForConnected()` hangs the page on WASM** — the WASM client must
  use the `setConnectHandler` pattern (#39) instead; the Socket rig's
  `waitForConnected()` recipe is for *native* tests only.
- The **synchronous shared/keyed attach path
  (`registerModelShared`/`attachModel`) nests an event loop that aborts the
  page on WASM** — `registerModelAsync` does not cover it. Async attach is
  a framework prerequisite for rung 3's WASM story — **and pulls forward to
  rung 1 if pastebin resolves burn atomicity via a shared keyed instance**
  (the coupling is called out in the pastebin README).

## Build system and CI (proven by rung 0)

Build wiring (from delivery review; today each example is hand-added in the
root `CMakeLists.txt` — don't repeat that eight times):

- One `examples/CMakeLists.txt`; one `MORPH_BUILD_LADDER` bool plus a
  `MORPH_LADDER_RUNGS` cache list (`"all"` or `"pastebin;kanban"`) — no
  per-rung booleans; the list maps 1:1 to CI path filters.
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
  (`ladder`, `ladder-<rung>`, `stress`, `socket-only`), warnings and
  sanitizers **applied to all app code** (bank skips both repo-wide because
  its ORM headers aren't `-Werror`-clean — the ladder scopes any such
  relaxation to the `db/` entity targets only, since persistence goes
  through the same Lightweight ORM per
  [`IMPLEMENTATION.md`](IMPLEMENTATION.md)), AUTOMOC, and a TIMEOUT on
  every binary. Lightweight's `FetchContent` acquisition is hoisted once
  into `examples/common`, not repeated per rung. One trap when implementing
  it: `catch_discover_tests` cannot carry a **multi-value** `LABELS`. It
  forwards `PROPERTIES` as a flat list through a `-D VAR=a;b;c` command line
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
  model type and therefore still includes its model header — and rule 4 puts
  `Lightweight::DataMapper` in that header's include graph, via the
  `WithMapper` mixin. Two things close the gap, and every rung needs both:
  configure the WASM build with **`-DMORPH_CLIENT_ONLY=ON`** (removes the
  registrars that closure over the model's ODBC-backed bodies —
  `docs/spec/core/registry.md`; `morph_add_rung()` fails the configure with
  that explanation if it is missing), and give the rung's `db_model.hpp` a
  persistence-free `WithMapper` under `__EMSCRIPTEN__` with **no `mapper()`**,
  so any attempt to reach a database from a browser build is a compile error.
  That is a two-branch mixin inside the file that already owns the ODBC
  dependency — not a shadow header tree, and not a second copy of any model,
  DTO, presenter or QML file. See
  [`../docs/findings/025-client-only-still-needs-model-persistence-headers.md`](../docs/findings/025-client-only-still-needs-model-persistence-headers.md).
- **Coverage wiring (proven by rung 0, on `examples/common`; the same
  recipe applies to every future rung's `src/models/`/`include/<rung>/models/`
  per [`IMPLEMENTATION.md`](IMPLEMENTATION.md) rule 5).** The `clang-coverage`
  CI leg is the only *sanitizer-matrix* leg that installs
  `qt6-base-dev`/`qt6-websockets-dev`/`qt6-tools-dev`/`libgl1-mesa-dev` and
  configures with
  `-DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all`
  (asan/tsan/ubsan never build the ladder at all, so this cost is paid once);
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
   rule above. `ctest -L ladder` — full ladder, all modes, including
   `[stress]` (scaled via `MORPH_LADDER_CLIENTS`/`ACTIONS` on the affected
   rungs), the kanban TSan leg (Local mode), and one Playwright browser smoke.
   One Windows compile-only build (never 8 rungs × 4 MSVC presets) runs
   alongside it. ASan is scoped to changed rungs.

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

1. Client-side execute deadline — no timeout on `Completion`; a
   rate-limited/black-holed call hangs forever (`messagesPerSecond` drops
   frames silently). Every polling helper must wrap its own timer until the
   framework provides one.
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
