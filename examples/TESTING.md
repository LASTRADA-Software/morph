# Ladder testing strategy — screens, dual deployment modes, multi-client stress

Every rung of the [application ladder](LADDER.md) serves screens that are
tested headless and in **both deployment modes** — in-process (`LocalBackend`
in one process) and client/server (`QtWebSocketBackend` against a
`RemoteServer`), including **N clients against one server** for stress tests.
This document is the binding convention; rung READMEs reference it instead of
restating it. Bank's model suites do not use the dual-mode rig yet — their
remote coverage is `SimulatedRemoteBackend` plus a scenario corpus — and
[`bank/README.md`](bank/README.md) records that as a known gap.

A screen is deliberately contentless — it names models, queries and mutations
and decides nothing ([`IMPLEMENTATION.md`](IMPLEMENTATION.md) rule 2) — so the
BackendRig / client-pool / convergence stack is not really UI testing: it is
**a conformance harness for morph's client-side stack** (`Bridge`, backends,
`QtExecutor`, completions, attach/reconnect under a real Qt event loop). It is
owned by the testkit as framework coverage: the full matrix runs once per
framework surface it conforms, and each rung runs a *thin instantiation* — its
models through the rig, one suite per model, not a per-screen × 3-mode
combinatorial matrix. That is also what keeps the CI cost curve flat.

## Screens (every rung)

1. **Screens are toolkit-free**, enforced by their target's link set:
   `ladder_<rung>_screens` links morph and the rung's DTOs, and no Qt.
2. **Every registered screen validates** (spec 5 §3) in the server's own
   tests: a screen that does not lower is a refused catalog, not a blank page.
3. **Screen tests run headless** on the harness
   (`examples/common/testkit/screen_harness.hpp`): a document mounted on
   `RecordingBackend` over the multi-mode rig, driven by test id — edit,
   choose, click — and waited on through `testkit/wait.hpp`, never with a
   sleep. No app hand-writes a client rig.
4. **One render smoke test per app** on Qt Quick offscreen, through
   `testkit/render_smoke.hpp`: every screen mounts without a refused
   document, a failed component or a QML warning.
5. Fingerprints, journeys, convergence and stress harnesses drive screens
   through the harness, each simulated client on its own rig bridge, and
   compare what each client shows.

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
  `Bridge{LocalBackend}`; N "clients" are N handler sets over the shared
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
(`tests/qt/qt_test_client_main.cpp`) via `process_pool.hpp`.

`rig.socketBackend(i)` hands out the raw `QtWebSocketBackend` for a client,
for the handful of transport-level operations that have no `Bridge`-level
equivalent — `negotiateProtocolVersion()` (the `hello` handshake) is the
motivating one. Everything that merely dispatches actions should use
`client<Model>()` / `bridge()` instead.

Teardown order (encoded in `~BackendRig`): handlers → client bridges →
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

A `sleep_for` outside `pump.hpp` is a review-rejectable defect. The test
binary uses the Qt-owning `main()` (QCoreApplication + `Catch::Session` +
DeferredDelete drain) copied from `tests/qt/test_qt_websocket.cpp`.

The one sanctioned exception is a test that must not link Qt at all: a
stress test meant to run under ThreadSanitizer, where routing callbacks
through a `QtExecutor` produces warnings that all bottom out in Qt-internal
frames a prebuilt Qt makes unreadable (measured once at 165 warnings for one
such test) — evidence for nothing either way. Such a test owns a small
`waitUntil` over `sleep_for` instead of `pumpUntil`, and still owes the
scaling: `examples/common/testkit/deadline.hpp` holds
`computeDeadlineScale`/`deadlineScale` with **no Qt dependency**, `pump.hpp`
and `wait.hpp` include it rather than defining them, and a Qt-free test
includes only `deadline.hpp`. So `MORPH_LADDER_DEADLINE_MS` moves every
wall-clock budget in `examples/`, Qt-free ones included. No current rung has
such a test.

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
and pays the retry loop knowingly.

The better answer, where the design allows it, is to have no background
worker under the test at all: make the job's work an ordinary action rather
than a task posted to an executor the model owns, so the test needs neither a
retry loop nor a `StepExecutor` — "the job has run" is what the dispatch
returning means, and "nothing has run it yet" is what a job row still pending
means. `StepExecutor` remains the answer for a job that genuinely is posted to
an executor; `examples/common/testkit/test_step_executor.cpp` covers it
directly.

## Multi-client stress harness

Testkit components, all under `examples/common/testkit/` and each covered by
its own test in `ladder_common_tests`. A rung uses the ones its features need;
today bookmarks, bank and lims use the database fixtures, the rig and the
screen harness, and no rung carries a multi-client stress, convergence,
process-pool or offline-rig suite — those harnesses are kept working by their
own tests until a rung needs them again.

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
- `client_pool.hpp` — typed pool constructing each client's handlers
  against `rig.client(i)`; test bodies are mode-blind.
- `convergence.hpp` — `pollUntilConverged(fetchFingerprints, maxAttempts)`:
  fetch one fingerprint per client per attempt, and report whether every
  client agreed within the attempt budget. **Honesty note**: in `Local`/
  `LocalSingleThread` modes all "clients" share one bridge — there is no
  staleness to converge from, so convergence is effectively
  `[socket-only]` coverage; don't count Local-mode runs. A rung without an
  event cursor compares fingerprints after its dispatches settle.
- `action_driver.hpp` — `SeededScript`: seed from `MORPH_STRESS_SEED`
  (always printed on failure), weighted action generators, schedule computed
  up front; per-burst invariant hooks (positions dense and unique, legs
  summing to zero, counts matching an event log).
- **N = 4–8 in-process clients** is the meaningful range (beyond ~8 sockets
  on one pumped thread you add queueing latency, not new interleavings);
  scale via `MORPH_LADDER_CLIENTS` / `MORPH_LADDER_ACTIONS` env vars
  (soak-suite convention) — same CI run, no separate schedule. A stress case
  meant for ThreadSanitizer drives **a bare `morph::bridge::Bridge`/
  `morph::backend::LocalBackend` on a real `ThreadPoolExecutor`, never
  `BackendRig`**: `BackendRig`'s `Mode::Local` always builds a real
  `morph::qt::QtExecutor` for client callback delivery, and the repository
  keeps Qt stacks out of the `clang-asan`/`clang-tsan`/`clang-ubsan` legs ("a
  GUI stack under TSan is mostly noise"). Such a test exercises models and
  strands, not sockets or Qt. Server-scale load (hundreds to thousands of
  sockets) is a load script's job, not a unit test's.
- `offline_rig.hpp` — scripted connectivity: drop by closing/destroying the
  in-test `QtWebSocketServer`, revive on the same port (proven pattern);
  hand-cranked signals into `ReconnectCoordinator`; queue inspection.
- `process_pool.hpp` — QProcess clients for scale **and for
  client-crash tests**: kill a client process mid-execute / mid-attach and
  assert connection-scope reclamation under abnormal teardown (distinct
  from graceful disconnect). A test hosts the server in its own process, so
  assertions read `RemoteServer::health()` directly rather than over IPC.
  Note `ProcessPool::allExited()` is a predicate for `pumpUntil`, not a
  blocking wait: blocking in `QProcess::waitForFinished` with an in-process
  server stops the loop that server needs to answer the very clients being
  waited on.

Per-rung test naming: `test_model_<entity>.cpp` (full mode matrix),
`screens/tests/test_<rung>_screens.cpp` (the screens, on the harness),
`screens/render_smoke/` (the Qt Quick smoke), `test_multiclient.cpp`
`[stress]`, `test_offline.cpp` for a rung with an offline path.

## The fault-injection wire proxy (and the strand interleaver)

The single highest-yield harness the ladder needs and the repo lacks: an
in-process WebSocket proxy between `QtWebSocketBackend` and
`QtWebSocketServer` with scriptable rules — *drop exactly the reply frame of
call k*, delay, duplicate, kill mid-replay. Exactly-once tests, dead-letter
tests and reconnect-mid-replay tests are demos, not CI tests, without it:
`SimulatedRemoteBackend` is lossless and unscoped, and the soak tests flap a
boolean, not a socket. A "duplicate create on retry" test uses true
reply-frame loss through the proxy rather than approximating it by executing
twice.

Companion harness from adversarial review: a **deterministic-schedule
strand interleaver** — without it, strand-ordering bugs (two concurrent
moves reordering one list, say) remain probabilistic stress runs rather than
reproducible interleavings.

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
surfaces divergence the per-mode tests cannot see — an unauthenticated call
refused by the *model* under `Local` ("no authenticated principal") but by
the server's authorizer under `Socket` ("unauthorized"): same outcome,
different wording.

Journeys live in `examples/<rung>/tests/journeys/` and carry a `journey`
ctest label (from the test name's `Journey: ` prefix), so they can be
selected with `ctest -L journey` or excluded the way `stress` is. No current
rung carries one; `journey.hpp` is covered by its own tests.

## Scenario files against a running server

A journey is the right *idea* but it is C++: compiled into the test binary,
driving the model through a `Bridge`, with the server started by the fixture.
`scripts/scenario/` is the same idea as data — a plain-text file of steps and
expected outcomes, run by a Python client that connects to a
`ladder_<rung>_server` somebody already started, speaks the wire protocol from
`docs/spec/core/wire.md`, and links against nothing:

```
client auth model=Bookmarks_AuthModel
do Login username=alice
expect ok capture token=$.token
expect ok capture who=$.principal

client store model=BookmarkModel
session principal=$who token=$token
do CreateBookmark url="https://example.com/keep" title="Keep" tags=["shelf"]
expect ok capture keep=$.id
do ListBookmarks tag=shelf
expect ok field bookmarks ~ "\"id\":$keep,"
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

**Screens are not unit-tested in a browser.** The answer, per rung:

1. **`LocalSingleThread` mode natively** — the same models, WASM-shaped
   wiring, every test run.
2. **Compile gate** — `.github/workflows/wasm-demo.yml` builds the generic
   client for wasm32-emscripten, single-threaded Qt, and publishes it with
   bank in local mode. Under Emscripten the only rung code built is bank's
   in-memory model library (`bank_memory_lib`) for that demo; every other
   rung's screens reach a browser served by its server, not compiled into the
   page.

Open framework facts every WASM client must respect (verified):

- `QtWebSocketBackend` registers only through `bindModel`, which never
  blocks; its synchronous `registerModel` refuses rather than spin a nested
  event loop, so the registration path is WASM-safe by construction.
- **`waitForConnected()` hangs the page on WASM** — a WASM client must
  use the `setConnectHandler` pattern instead; the Socket rig's
  `waitForConnected()` recipe is for *native* tests only.
- A **blocking keyed acquire would nest an event loop that aborts the page on
  WASM**; `QtWebSocketBackend` has none. `IBackend::bindModel`
  (`include/morph/core/backend.hpp`) is the one non-blocking acquire verb —
  a request with a non-empty `primary` is the register-or-attach case, one
  that also carries a non-zero `current` is the re-point — and every keyed
  `BridgeHandler::execute` goes through it.

## Build system and CI

- One `examples/CMakeLists.txt`; one `MORPH_BUILD_LADDER` bool plus a
  `MORPH_LADDER_RUNGS` cache list (`"all"` or `"bookmarks;lims"`) — no
  per-rung booleans. `MORPH_BUILD_BANK_EXAMPLE` also selects bank: with the
  ladder on, the rung loop adds bank when either asks for it, and the root
  `CMakeLists.txt` adds bank itself only when the ladder is off (the
  WebAssembly demo build), so bank is added exactly once.
- **The rung names live in `examples/rungs.txt`, and nowhere else.** A
  hand-copied list drifts in every copy that is not load-bearing: a
  `ladder-tests`/`ladder-sanitizers` path filter that stops short of a rung
  makes a change confined to that rung match nothing and skip both jobs.
  Nothing reports it, because a path filter that matches nothing succeeds
  exactly as loudly as one that correctly found nothing to do;
  `scripts/coverage.sh` and `codecov.yml` drift the same way. The list is
  therefore structured so it cannot:

  - `examples/rungs.txt` is the single authority: one bare rung name per
    line, ASCII, whole-line `#` comments. A line that is neither is a hard
    error in every reader rather than a line quietly skipped.
  - `examples/CMakeLists.txt` reads it, and **refuses to configure** if any
    `examples/<dir>/CMakeLists.txt` calls `morph_add_rung(NAME <x>)` for an
    `<x>` the file does not list. This is what makes the authority
    load-bearing: a rung cannot exist unlisted. (Bank is listed and does not
    call `morph_add_rung`, which the file allows.)
  - `scripts/ladder_rungs.sh` is the shared reader. `list` prints the rung
    names; `ci-path-regex` prints the whole changed-paths regex, rungs plus
    the non-rung paths that must also trigger the ladder. Both
    `ladder-tests` and `ladder-sanitizers` call it, so their filters are one
    expression and cannot diverge from each other or from the list.
    `scripts/coverage.sh` reads it too.
  - Two consumers cannot read it: `codecov.yml`'s per-rung components (read
    by Codecov, not by us), and
    `examples/<rung>/tests/.clang-tidy` (clang-tidy resolves configuration by
    walking up from the file it is analysing, so a per-directory setting can
    only live in that directory).

  Adding a rung is therefore: add the name to `examples/rungs.txt`, add a
  component to `codecov.yml`, and copy an existing
  `examples/*/tests/.clang-tidy` next to the new suite.

- **Every rung's `tests/` carries a `.clang-tidy`.** It subtracts exactly one
  check, `bugprone-chained-comparison`, and keeps every other one through
  `InheritParentConfig: true`. Catch2's `REQUIRE(a == b)` expands to
  `Catch::Decomposer() <= a == b`, which that check reads as the chained
  comparison `v0 <= v1 == v2`; the framework's own `tests/.clang-tidy`
  subtracts it for the same reason, and `examples/common/testkit/` carries
  the same file. There is deliberately **no** `examples/.clang-tidy`: one file
  there would cover every rung's `src/`, `include/` and `screens/` as well,
  where a hand-written `a < b < c` is a real defect, and a gate fails if one
  appears. It also fails if any `.cpp` in one of those directories is not a
  Catch2 translation unit: clang-tidy resolves configuration from the TU's
  path, so a library source living beside the tests would get a suppression
  argued as Catch2 idiom without being Catch2 at all. `morph_ladder_testkit`'s
  own TUs are in `examples/common/testkit_src/` rather than beside their
  headers for exactly that reason. The headers themselves stay in
  `testkit/` and are listed as sources of `morph_ladder_testkit`, which is
  what keeps AUTOMOC seeing `fault_proxy.hpp`'s `Q_OBJECT`.
- `examples/common/` declares the testkit, `morph_ladder_testkit` (morph +
  Catch2 + Qt); the argument reader, the remote and local sources and the
  client shell are the library's `morph::client`. Rungs link targets, never paths; the testkit never grows per-rung
  options.
- A `morph_add_rung()` function creates `ladder_<rung>_{lib,server,screens,
  screen_tests,render_smoke,tests}` with `catch_discover_tests` + ctest labels
  (`ladder`, `ladder-<rung>` — Catch2's own tags like `[stress]`/`[tsan]`
  are not translated into ctest labels anywhere in this repo; select on
  them with `ctest -R` against the test name instead), a `TEST_PREFIX` of
  `<rung>.` on every discovered ctest name (a ctest name is global to the
  build tree, so without the prefix two rungs sharing a `TEST_CASE` name give
  `ctest -L ladder-<rung>` another rung's cases as well as its own;
  `scripts/check_ctest_name_collisions.sh` is the gate that keeps
  the names distinct, and note the prefix reaches the *ctest* entry only, so
  `ctest -R` still matches the bare test name), warnings and
  sanitizers **applied to all app code**, AUTOMOC, and a TIMEOUT on every
  binary. Bank builds the same names by hand in its own `CMakeLists.txt`
  (`ladder_bank_server`, `ladder_bank_tests` with the `bank.` prefix and the
  `ladder-bank` label), without `apply_warnings()`: its targets pull the ORM's
  headers in through its model headers, and those are not `-Werror` clean.
  Sanitizers are opt-in per `AF_SANITIZER` (set by the
  `clang-asan`/`clang-tsan`/`clang-ubsan` presets), applied with the same
  `if(DEFINED AF_SANITIZER) apply_sanitizers(<target> ${AF_SANITIZER})
  endif()` guard `AF_COVERAGE` uses for `apply_coverage()` — every ladder
  target that reaches a rung's models or tests carries this guard, so a
  `--preset clang-asan` configure of the ladder actually instruments the
  code it builds. Lightweight's CPM acquisition is hoisted once into
  `examples/common`, not repeated per rung. One trap when implementing it:
  `catch_discover_tests` cannot carry a **multi-value** `LABELS`. It
  forwards `PROPERTIES` as a flat list through a `-D VAR=a;b;c` command line
  where no escaping survives, so `LABELS "x;y"` does not make a two-label
  test — it shifts every following name/value pair by one, silently dropping
  the rest. `examples/common/CMakeLists.txt` shows the working shape: one
  value per property name in the `catch_discover_tests` call, plus a
  generated `TEST_INCLUDE_FILES` post-pass for the extra labels.
- **Sanitizer wiring.** **`Application ladder / ASan+UBSan`**
  (`ladder-sanitizers`) configures `--preset clang-asan` with
  `-DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all`
  and runs `ctest -L ladder -LE stress`, so every rung's tests, bank's
  included, run under both sanitizers. One preset covers both:
  `apply_sanitizers(<target> asan)` compiles with
  `-fsanitize=address,undefined` (`cmake/compiler_options.cmake`), so a
  separate ubsan leg for the ladder would re-run a strict subset. It shares
  `ladder-tests`' changed-paths filter — not a copy of it, but the same
  generated expression, from `scripts/ladder_rungs.sh ci-path-regex`; the
  two build the same tree and differ only in instrumentation, so the filters
  must not be able to diverge. `ASAN_OPTIONS=detect_leaks=0` because
  LeakSanitizer reports allocations Qt's platform plugins and QML engine keep
  for process lifetime; the memory-error and UB checks stay on. The job runs
  `scripts/check_sanitizer_instrumentation.sh` before trusting a green run —
  an uninstrumented sanitizer job passes unconditionally and reads as proof
  when it is the absence of proof.

  **TSan is deliberately not applied to rungs wholesale.** A rung's tests
  drive Qt on every path, and against an uninstrumented system Qt that
  yields warnings bottoming out in Qt-internal frames that cannot be
  classified as real races or false positives from outside a
  TSan-instrumented Qt build (measured once at 165 warnings deep for one
  rung's stress test). Thread-sanitising a rung therefore means writing a
  test that constructs no `QtExecutor` at all — driving the model through a
  bare `morph::bridge::Bridge`/`morph::backend::LocalBackend` on a real
  `morph::exec::ThreadPoolExecutor` — and running just that test under
  `clang-tsan` in a CI leg of its own. No current rung has such a test, so
  there is no TSan leg over ladder code; the library's own TSan coverage
  (`linux-sanitizers`, `morph_tests`) is separate and unaffected.

- **Coverage wiring.** The `clang-coverage` CI leg builds the full ladder
  (`-DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all`)
  and runs its `ctest` with `QT_QPA_PLATFORM=offscreen`, since the runner has
  no display. Every ladder CMake target (`morph_ladder_testkit`, each rung's
  own targets, and bank's `bank_lib`, `bank_cli`, `ladder_bank_server`,
  `ladder_bank_tests` and screens) wraps its definition in
  `if(AF_COVERAGE) apply_coverage(<target>) endif()`, the same guard
  `include/morph`'s own targets use; the instrumentation is a link option
  too, so every executable that links an instrumented library needs it.
  `scripts/coverage.sh` merges multiple instrumented binaries into one report
  via llvm-cov's `-object` flag: one `TEST_EXE` positional (the library's
  `morph_tests`) plus an `OBJECT_ARGS` array populated with every other test
  binary that exists in the build (`ladder_common_tests` and each
  `ladder_<rung>_tests`, each guarded by `if [ -x "$BINARY" ]` so the script
  keeps working for a configure that did not build that rung), and adds
  `examples/common` and each rung's `include`, `src` and `screens` to the
  positional source-path filter alongside `include/morph`. AUTOMOC's
  generated `mocs_compilation.cpp` lives under the build tree, never under a
  source-tree path this filter names, so moc output is excluded for free.
  `codecov.yml`'s `component_management.individual_components` has one
  component per rung, `informational: true`, with its `target:` set from the
  measured ceiling per [`IMPLEMENTATION.md`](IMPLEMENTATION.md) rule 5's
  coverage-artifact guidance (not a blind 100%).

CI tiers. No separate nightly schedule: the ladder runs in the ordinary
per-push/per-PR jobs, same as the rest of this repo's CI, and its cost is
managed by a changed-paths filter, not by deferring work to an off-hours run.

That filter is a **boolean**, not a per-rung selection: `ladder-tests` and
`ladder-sanitizers` each diff the changed paths and decide whether to run the
ladder *at all*. When they do run, both pass `-DMORPH_LADDER_RUNGS=all` and
build every rung. `MORPH_LADDER_RUNGS` is never computed from changed paths —
every occurrence in `.github/workflows/ci.yml` is the literal `all`.
Per-rung scoping (`examples/<rung>/**` → that rung; `examples/common/**` or
`include/morph/**` → all rungs) has never been built.

- **`ladder-tests`** (every push/PR, gated by the filter above): gcc-debug,
  offscreen, `ctest -L ladder -LE stress` — the full ladder, all modes. The
  `-LE stress` clause selects nothing to exclude today (no ctest label named
  `stress` exists — Catch2 tags are never translated into ctest labels, as
  noted above). The render smoke tests need Qt Quick, so the job installs Qt
  via aqtinstall and configures `MORPH_BUILD_QT_QUICK=ON`. It also runs
  `scripts/check_ctest_name_collisions.sh`, since it is the one configuration
  with every rung at once.
- **`ladder-sanitizers`**: ASan + UBSan over the same tree, above.
- **`linux-coverage`**: the `clang-coverage` leg, above.

## Two coverage numbers

`llvm-cov report`'s line coverage and Codecov's component percentage are not
the same measurement. Codecov reads the uploaded LCOV and scores
`hits / (hits + misses + partials)`, where a *partial* is a line that executed
but carries a branch nothing took. `llvm-cov` counts that line as covered.
Whenever the two are quoted side by side without saying which is which, the
gap looks like a regression or a contradiction; it is neither. **Quote the
Codecov figure when arguing about a component's target, because that is the
one the target is checked against.**

Reproduce either from a coverage run — `cmake --preset clang-coverage` (plus
`-DMORPH_BUILD_{NET,OFFLINE_SQLITE,QT,LADDER}=ON`), `ctest`, then
`bash scripts/coverage.sh`. `llvm-cov`'s figure is in that script's own report;
Codecov's arithmetic runs over `build/clang-coverage/coverage.lcov`. A shared
compiler cache that serves objects built in another worktree drops their
records, whose absolute paths match none of `scripts/coverage.sh`'s relative
filters; `scripts/check_coverage_roots.sh` gates that, so run it before
trusting any local figure.

## Framework gaps this strategy exposed (all since closed)

This list was written as candidate issues to schedule. **Every item on it has
since shipped.** It is kept because it records what the testing strategy pushed
the framework to grow, and why — but nothing here is work to pick up, and each
entry names where the capability now lives so that can be checked rather than
taken on trust.

1. **Client-side execute deadline** → `Bridge::setExecuteDeadline`
   (`include/morph/core/bridge.hpp`), specified in
   [`docs/spec/core/completion.md`](../docs/spec/core/completion.md). A polling
   helper no longer wraps its own timer. (A rate-limited frame no longer
   belongs on this list for a second reason: the transport answers it with an
   `err "rate limited"` addressed to the frame's own `callId`, so the caller's
   `Completion` fails rather than hanging.)
2. **`Bridge::pendingCalls()`** → `include/morph/core/bridge.hpp`, so a
   test's wait for quiescence can be exact rather than counting by hand.
3. **`MainThreadExecutor::runOnce()`/`drain()`** →
   `include/morph/core/executor.hpp` — a step and a full drain, not a
   wall-clock pump.
4. **`QtExecutor` with an optional `QObject*` context target** →
   `QtExecutor(QObject* context = QCoreApplication::instance())`
   (`include/morph/qt/qt_executor.hpp`), giving the per-thread affinity
   N-thread client topologies need.
5. **Connection-scoped simulated client** → `RemoteServer::openConnection()`
   (`include/morph/core/remote.hpp`), for deterministic connection-lifetime
   tests without sockets.
6. **Injectable time source usable by remotely-constructed models** →
   `setNowOverride` / `ScopedNowOverride` (`include/morph/util/datetime.hpp`) —
   the process-global now-provider convention the entry asks for.
