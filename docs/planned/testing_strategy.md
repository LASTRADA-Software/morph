# Load / soak / fuzz testing strategy (planned)

> **Status: planned — not yet implemented.** This spec defines the load, soak,
> and fuzz testing that the current unit suite does not cover. It gates
> confidence in the §A hardening milestone ([validation.md](validation.md),
> [instance_authorization.md](instance_authorization.md),
> [transport_limits.md](transport_limits.md)) and exercises the untrusted-input
> boundaries in [wire.md](../spec/core/wire.md) and [backend.md](../spec/core/backend.md).
> See [todo.md](../todo.md).

## The gap

Unit coverage is strong but narrow in *shape*. [security.md](../spec/security.md)
lists real hardening tests — `test_wire_hardening.cpp`, `test_server_limits.cpp`,
`test_session_auth.cpp`, `test_policy_hardening.cpp` — and
[todo.md](../todo.md) records 2734 assertions. But every one is a
single-shot, in-process, deterministic unit test. Missing:

- **No fuzzing.** `wire::decode` is the untrusted-input boundary
  ([wire.md](../spec/core/wire.md)) and the action codec re-parses the opaque `body` a
  second time, yet nothing throws *randomised, malformed, adversarial* input at
  either. `test_server_limits.cpp` checks a *fixed* 5000-deep nesting and a lone
  continuation byte — hand-picked cases, not a coverage-guided search.
- **No soak test.** `switchBackend`, reconnect backoff, and
  `ReconnectCoordinator`/`SyncWorker` churn ([offline.md](../spec/offline/offline.md),
  [backend.md](../spec/core/backend.md)) are tested for a single transition, not for
  thousands of cycles over hours where a slow leak, a missed `cancelPending`, or a
  strand backlog would surface.
- **No load/throughput benchmark.** There is no measurement of dispatch latency
  or sustained throughput, so the `LimitPolicy` knobs
  ([transport_limits.md](transport_limits.md)) have no baseline to be set
  against, and regressions in the hot path go unnoticed.
- **No adversarial cross-process run.** The TLS cross-process test
  (`test_qt_websocket.cpp`) is cooperative; nothing drives a *hostile* client
  across a real socket.

The framework's untrusted-input claims ([security.md](../spec/security.md)'s "a
malformed payload is a defined outcome rather than undefined behaviour") are
asserted on examples, not proven over a distribution of inputs.

## Goal

Add three test categories — fuzz, soak, load — plus one adversarial cross-process
run, as opt-in CI/CD targets (not part of the fast unit build), so the hardening
milestone's guarantees are demonstrated over input distributions and time, not
just point cases. Nothing here changes library code; it is test/harness work.

## Design

### 1. Fuzz harness over the wire and dispatch boundaries (NEW)

A libFuzzer/AFL++-style harness (guarded behind a build option, e.g.
`MORPH_BUILD_FUZZERS=ON`, so it never affects the normal build) targeting the two
untrusted-input entry points:

```cpp
// tests/fuzz/fuzz_wire_decode.cpp — NEW.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string_view input{reinterpret_cast<const char*>(data), size};
    try {
        auto env = morph::wire::decode(input);   // must never crash/UB
        // if it decoded, feed body through a registered action's fromJson
        // to exercise the second (inner) parse of the opaque body.
    } catch (const std::runtime_error&) {
        // a thrown decode error is the DEFINED outcome — acceptable.
    }
    return 0;
}
```

Targets:

- **`wire::decode`** — the outer parse. The invariant under fuzzing: every input
  either decodes to an `Envelope` or throws `std::runtime_error`; it never
  crashes, hangs, or exhibits UB. The `kMaxEnvelopeBytes` cap
  ([wire.md](../spec/core/wire.md)) bounds the input the harness need supply.
- **The inner `body` re-parse** — feed a decoded `body` through a representative
  action's `ActionTraits::fromJson`, since [wire.md](../spec/core/wire.md) warns the
  inner parse "needs its own limits" and the outer parse never walks `body`. This
  is where the double-parse hazard would detonate; the fuzzer proves it detonates
  *safely* (defined error, no crash).
- **`dispatchExecute`** — a harness that hands hand-built `execute` envelopes to
  `RemoteServer::handle` and asserts every input yields an `ok`/`err` reply, never
  a crash — the coverage-guided generalisation of `test_server_limits.cpp`.

A seed corpus is drawn from the existing hardening tests' known payloads; a
findings corpus is committed so regressions are reproducible.

### 2. Soak test for reconnect / switchBackend churn (NEW)

A long-running test (opt-in target, minutes-to-hours, not in the fast suite) that
cycles the backend and connectivity machinery under continuous execute load:

- Repeatedly `switchBackend` between a `LocalBackend` and a
  `SimulatedRemoteBackend`/`QtWebSocketBackend` while executes are in flight, asserting
  every in-flight `Completion` resolves (via result or `BackendChangedError`) and
  none leak — exercising the stage-all-then-commit atomicity and `cancelPending`
  ([backend.md](../spec/core/backend.md), [ARCHITECTURE.md](../ARCHITECTURE.md)).
- Drive `NetworkMonitor` → `ReconnectCoordinator` → `SyncWorker` through thousands
  of offline/online flaps ([offline.md](../spec/offline/offline.md)), asserting the
  offline queue drains, attempt counts behave, and no unbounded growth occurs.
- Measured for **resource stability**: RSS, live `ModelId` count, pending-map size,
  and strand queue depth must be flat across the run (a leak or a stuck strand
  shows as monotonic growth). The [observability.md](observability.md) metrics
  seam, once it exists, is the natural instrument.

### 3. Load / throughput + latency benchmark (NEW)

A benchmark target that measures the hot path so `LimitPolicy` defaults
([transport_limits.md](transport_limits.md)) can be chosen from data:

- **Throughput** — sustained executes/second through `RemoteServer` over the
  simulated and (optionally) socket transports, at increasing concurrency, until
  saturation.
- **Latency distribution** — per-dispatch wall time (p50/p95/p99), reported so a
  regression is visible run-to-run. This is the baseline `executeLatencyMs`
  ([observability.md](observability.md)) reports on in production.
- Run against a trivial model (measures framework overhead, not business logic)
  and recorded as a tracked artifact so CI can flag a regression beyond a
  threshold.

### 4. Adversarial cross-process run (NEW)

Extend the cross-process TLS test ([security.md](../spec/security.md)'s
`test_qt_websocket.cpp`) with a *hostile* client process that: sends oversized
frames (past `kMaxEnvelopeBytes` and past a tighter transport cap), floods
connections/messages (exercising [transport_limits.md](transport_limits.md)),
sends duplicate-key envelopes ([wire.md](../spec/core/wire.md)'s smuggling caveat), and
opens-then-stalls connections. The server must stay up and serving honest clients
throughout — the concrete demonstration that the limits and the wire bounds hold
against a real adversary, not just a unit assertion.

## Non-goals

- **No library code changes.** This is test/harness/CI work; it exercises existing
  and planned behavior, it does not add product features. (It *depends on*
  [transport_limits.md](transport_limits.md) and
  [observability.md](observability.md) to have something to measure and bound, but
  does not implement them.)
- **Not part of the fast unit build.** Fuzz/soak/load targets are opt-in
  (`MORPH_BUILD_FUZZERS`, dedicated CI jobs) so ordinary `ctest` stays fast;
  they run on a schedule / pre-release, not per-commit.
- **Not a substitute for the unit suite.** The 2734-assertion suite remains the
  correctness baseline; these add distributional and temporal coverage on top.
- **No third-party fuzzing service dependency.** The harness uses the compiler's
  built-in fuzzing (libFuzzer/AFL++) so it runs anywhere the toolchain does.

## Testing (planned)

This spec *is* the testing plan; its own acceptance criteria are:

- The fuzz targets build under `MORPH_BUILD_FUZZERS=ON`, run against the seed
  corpus, and find no crash/hang/UB in `wire::decode`, the inner `body` parse, or
  `dispatchExecute` over an extended run; any finding is added to the committed
  corpus as a regression case.
- The soak target completes its cycle count with flat resource metrics and every
  `Completion` accounted for (resolved, not leaked).
- The load benchmark produces a recorded throughput + latency profile; a
  configured regression threshold fails CI when breached.
- The adversarial cross-process run keeps the server serving honest clients while
  a hostile peer attacks size/rate/connection/duplicate-key vectors.

## Cross-references

- [wire.md](../spec/core/wire.md) — `decode`, `kMaxEnvelopeBytes`, the `body`
  double-parse and duplicate-key caveats the fuzzers target.
- [backend.md](../spec/core/backend.md) — `RemoteServer::handle`/`dispatchExecute`, the
  `switchBackend`/`cancelPending` lifecycle the soak test churns, and the
  transports the load benchmark drives.
- [security.md](../spec/security.md) — the existing hardening tests
  (`test_wire_hardening.cpp`, `test_server_limits.cpp`, `test_qt_websocket.cpp`)
  these generalise, and the untrusted-input claims they prove over distributions.
- [transport_limits.md](transport_limits.md) — the `LimitPolicy` the load
  benchmark baselines and the adversarial run exercises.
- [observability.md](observability.md) — the metrics the soak/load runs instrument
  themselves with.
- [validation.md](validation.md) / [instance_authorization.md](instance_authorization.md)
  — the §A hardening this testing gates confidence in.
