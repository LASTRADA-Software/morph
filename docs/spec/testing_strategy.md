# Load, soak, and fuzz testing

`morph`'s unit suite (`tests/CMakeLists.txt`, 7234+ Catch2 assertions as of the
last count) is deterministic, single-shot, and in-process: every test picks a
fixed input and asserts one outcome. Four additional, **opt-in** test
categories complement it by exercising the framework over *distributions* of
input and *time*, rather than hand-picked examples: a fuzz harness, a soak
test, a load/latency benchmark, and an adversarial cross-socket run. None of
these run in the default `ctest` sweep or the fast unit build — each lives
behind its own CMake option, default `OFF`, so an ordinary `cmake --build`
+ `ctest` is unaffected by their existence.

## Contents
- [Fuzz harness](#fuzz-harness-testsfuzz)
- [Soak tests](#soak-tests-testssoak)
- [Load / latency benchmark](#load--latency-benchmark-testsbench)
- [Adversarial cross-socket run](#adversarial-cross-socket-run-testsqttest_qt_websocket_adversarialcpp)
- [Cross-references](#cross-references)

## Fuzz harness (`tests/fuzz/`)

Built only under `-DMORPH_BUILD_FUZZERS=ON`; requires Clang, since it uses
libFuzzer (`-fsanitize=fuzzer`), the compiler-builtin coverage-guided fuzzing
engine — no third-party fuzzing dependency or service. Configuring this option
under a non-Clang compiler fails the configure step with a clear message
rather than silently building nothing.

Two targets:

| Target | Exercises | Invariant |
|---|---|---|
| `fuzz_wire_decode` | `morph::wire::decode` (the outer envelope parse) and, for a decoded `"execute"` envelope with a non-empty `body`, `ActionTraits<FuzzInnerAction>::fromJson(body)` (the inner re-parse — see [wire.md](core/wire.md)'s "the body double-parse hazard") | Every input either parses (at each stage) or throws `std::runtime_error`. Never crashes, trips a sanitizer, or hangs. |
| `fuzz_dispatch_execute` | `morph::backend::RemoteServer::handle`/`dispatchMessage` end to end, against a live server with one pre-registered `Fuzz_DispatchModel` instance | Every input yields a reply that itself decodes as a wire `Envelope` with `kind` `"ok"` or `"err"`. Never crashes; never hangs (bounded by libFuzzer's own `-timeout`, not by anything in the harness). |

Both binaries link `-fsanitize=fuzzer,address` (via the `apply_fuzzer()` CMake
function in `cmake/compiler_options.cmake`), so a crash found while fuzzing is
also an AddressSanitizer memory-safety finding, not just a caught exception.

**Corpus.** `tests/fuzz/corpus/wire_decode/*.txt` and
`tests/fuzz/corpus/dispatch_execute/*.txt` hold hand-picked seeds drawn from
the existing hardening tests (`test_wire_hardening.cpp`, `test_server_limits.cpp`)
— a valid register/execute/deregister envelope, a duplicate-key envelope, a
moderately-nested body, a malformed-UTF-8 body, and plain garbage.
`tests/fuzz/findings/wire_decode/` and `tests/fuzz/findings/dispatch_execute/`
start empty (marked with a tracked `.gitkeep`) and are where any input that
triggers a crash/hang/sanitizer report **in a bug that has since been fixed**
gets committed as a permanent regression case — see "Known findings" below for
inputs discovered but not yet in that state.

**CI-integrated regression check.** `ctest -R fuzz_.*_replay` runs each target
once per file in its corpus + findings directories (libFuzzer's single-run
replay mode — passing individual files, not a directory, so it replays and
exits rather than starting a mutating fuzzing session) and fails if any input
now crashes. This is fast and deterministic, suitable for CI; it is **not** a
fuzzing campaign.

**Running an actual campaign** (manual / scheduled, not CI-per-commit):
```
cmake --preset clang-release -DMORPH_BUILD_FUZZERS=ON
cmake --build build/clang-release --target fuzz_wire_decode fuzz_dispatch_execute
./build/clang-release/tests/fuzz/fuzz_wire_decode -max_total_time=3600 tests/fuzz/corpus/wire_decode
```
Passing the corpus directory as the sole argument makes libFuzzer both seed
from and write newly-discovered coverage-increasing inputs back into that same
directory — review and keep genuinely new, interesting cases; move anything
that crashes into `findings/` instead, once the underlying bug is fixed (see
below).

**Known findings (fixed).** A short campaign run while this harness was built
surfaced two real, reproducible issues in `morph::wire`'s glaze-based parsing.
Both are now fixed, with the original crashing inputs preserved as permanent
regression cases under `tests/fuzz/findings/`:

- **`skip_ws` heap-buffer-overflow** (`tests/fuzz/findings/wire_decode/skip_ws_heap_overflow.bin`).
  `morph::wire::decode` (and the other `glz::read<>` call sites accepting an
  arbitrary `string_view` — `journal::fromJson`, `BRIDGE_REGISTER_ACTION`'s
  generated `fromJson`/`resultFromJson`, `FileOfflineQueue`'s `fromJson`) left
  glaze's `null_terminated` option at its default (`true`), so `skip_ws`
  assumed it could scan past the buffer's real end looking for a terminator
  byte that a `string_view` — unlike a `std::string`, which always has one at
  `data()[size()]` — never guarantees exists. A 5-byte input (`{"{k`, no
  closing quote/brace) was enough to trip an AddressSanitizer
  heap-buffer-overflow, reachable by any peer sending a handful of bytes to a
  `RemoteServer`. Fixed by setting `.null_terminated = false` on every such
  call site — the correct, documented way to tell glaze the buffer isn't
  guaranteed null-terminated; costs nothing beyond disabling an optimization
  that never legitimately applied. `session_auth.hpp`'s token-claims decode
  was checked and needed no change: it parses a `const std::string&` (a real
  owned string with the standard's null-terminator guarantee), not a view.
- **Unescaped control bytes broke the `err`-reply round-trip**
  (`tests/fuzz/findings/dispatch_execute/err_reply_control_char_roundtrip.bin`).
  Glaze's writer escapes `"` and `\` but not ASCII control bytes (0x00-0x1F,
  0x7F); an `err` reply whose `message` echoed untrusted text containing one
  (e.g. an unrecognized `Envelope::kind`, or a caught exception's `what()`,
  either of which can carry attacker-controlled bytes) served syntactically
  invalid JSON that glaze's own reader then rejected on decode — the server
  could emit a reply that wasn't itself valid wire JSON. Fixed in
  `wire::makeErr` (the single choke point every `err` reply goes through):
  `detail::sanitizeControlChars` replaces each control byte with a printable
  `\xHH` placeholder before it reaches the writer. Diagnostic text doesn't
  need byte-for-byte fidelity; guaranteed-valid JSON does.

Both fixes are covered by dedicated regression tests in
`tests/test_wire_hardening.cpp` ("Bug C"/"Bug D") in addition to the
`fuzz_*_replay` findings above.

## Soak tests (`tests/soak/`)

Built only under `-DMORPH_BUILD_LOAD_TESTS=ON` (requires `MORPH_BUILD_TESTS=ON`,
since these link Catch2). Two Catch2 test cases in the `morph_soak` binary:

- **`test_soak_switch_backend.cpp`** — cycles `Bridge::switchBackend()` between
  a `LocalBackend` and a fresh `SimulatedRemoteBackend`/`RemoteServer` pair
  every cycle, firing a burst of `execute()` calls each cycle. Asserts every
  issued completion eventually resolves (value or `BackendChangedError` —
  see [backend.md](core/backend.md)), that the live model-instance count
  settles back down after the run (a leaked `IModelHolder` would show as
  monotonic growth), and — on Linux, where `/proc/self/status` is readable —
  that RSS growth across the run stays under a configurable bound. The final
  iteration always lands on a fresh `LocalBackend` before the last
  `RemoteServer` is dropped, so the bridge's active backend never outlives
  the object it references (see `backend.md`'s "Lifetime & ownership").
- **`test_soak_reconnect_churn.cpp`** — wires a real `NetworkMonitor` →
  `ReconnectCoordinator` → `SyncWorker` pipeline exactly as
  [offline.md](offline/offline.md)'s "End-to-end integration" shows, and flips
  online/offline hundreds of times. Asserts the offline queue is fully
  drained after every flap and every `onOnline()` reconnects (this test's
  `tryReconnect` never fails).

Both are CI-sized by default (a few hundred cycles, seconds to run) and scale
to a real multi-hour soak via environment variables, without touching code:

| Variable | Default | Test |
|---|---|---|
| `MORPH_SOAK_CYCLES` | `200` | switch-backend churn |
| `MORPH_SOAK_EXECUTES_PER_CYCLE` | `20` | switch-backend churn |
| `MORPH_SOAK_RSS_SAMPLE_EVERY` | `20` | switch-backend churn |
| `MORPH_SOAK_RSS_GROWTH_KB_MAX` | `102400` (100 MiB) | switch-backend churn |
| `MORPH_SOAK_FLAP_CYCLES` | `150` | reconnect churn |

A production `morph::observe` metrics seam already exists
([observability.md](core/observability.md)) and instruments `RemoteServer`/
`LocalBackend` dispatch, `SyncWorker::run()`, and
`ReconnectCoordinator::onOnline()` directly. These soak tests deliberately do
not depend on a particular `MetricSink` being installed in the test process,
though: they instrument themselves directly instead (a model-local instance
counter, `/proc/self/status`, and locally-owned atomic call counters), so
their pass/fail signal never depends on how (or whether) a host application
has wired up observability.

## Load / latency benchmark (`tests/bench/`)

Built only under `-DMORPH_BUILD_LOAD_TESTS=ON` alongside the soak tests (same
option; different target, `morph_bench`). `bench_dispatch_latency.cpp` drives
a `RemoteServer` directly against a trivial echo model (isolating framework
overhead from business logic):

- **Latency** — 2000 serial (concurrency-1) round trips; reports p50/p95/p99
  wall time in milliseconds (nearest-rank percentile over the sorted sample).
- **Throughput** — a 500 ms window at each of concurrency 1/2/4/8/16, reporting
  executes/second.
- Writes `bench_dispatch_latency.json` into the build directory (`{"p50_ms":
  ..., "p95_ms": ..., "p99_ms": ..., "throughput": [{"concurrency": ...,
  "executes_per_sec": ...}, ...]}`) so successive runs can be archived and
  diffed for a regression.
- Enforces two coarse regression gates via `REQUIRE`: `p99 <=
  MORPH_BENCH_P99_MS_MAX` (default 50.0 ms) and concurrency-1 throughput `>=
  MORPH_BENCH_MIN_THROUGHPUT` (default 500.0 executes/sec). Both are
  environment-variable-overridable so CI hardware differences don't need a
  code change.

The echo model/action (`BenchEchoModel`/`BenchEchoAction`) are declared at
file scope, not inside the file's anonymous namespace with its other local
helpers — Glaze's reflection needs external linkage to mangle the type name,
the same requirement `tests/fuzz/`'s harness fixtures document.

## Adversarial cross-socket run (`tests/qt/test_qt_websocket_adversarial.cpp`)

Built under the existing `MORPH_BUILD_QT=ON` option (no new option — it's one
more file in the existing `morph_qt_tests` binary). A hostile `QWebSocket`
client, talking to a real `QtWebSocketServer` over loopback TCP (same-process,
like most of `test_qt_websocket.cpp` — see that file's dedicated
"Process separation: ..." cases for genuine OS-process separation), drives
four scenarios:

1. An oversized frame (body past `wire::kMaxEnvelopeBytes`).
2. A rapid flood of 5000 execute messages with no per-message wait.
3. A duplicate-top-level-key envelope (the [wire.md](core/wire.md) smuggling
   caveat, over a real socket).
4. A connection that opens and then never sends a single frame.

After each, an honest `QtWebSocketBackend` client registers a model and
executes an action against the **same** server and must succeed — the
assertion in every scenario is that **the server keeps serving honest
clients**. `RemoteServer::LimitPolicy` and `QtWebSocketServerConfig` (see
[backend.md](core/backend.md#limitpolicy--opt-in-resource-limits)) are
implemented, each with its own dedicated, precise unit coverage
(`tests/test_limit_policy.cpp`, `tests/qt/test_qt_websocket.cpp`) — this file
does not re-derive that coverage. Every server here uses the **default**,
unconfigured `QtWebSocketServerConfig`, under which `maxConnections`/
`messagesPerSecond`/`handshakeTimeout`/`idleTimeout` are all still
unbounded/disabled (`0`), but `maxMessageBytes` defaults to
`wire::kMaxEnvelopeBytes` (not `0`) — so, concretely: scenario 1 (oversized
frame) *is* actively rejected by the default config and this file asserts
that directly (an `err` reply mentioning `maxMessageBytes`), while scenarios
2 and 4 (flood, stall) are not capped by default and this file asserts only
that the server keeps functioning under them, not that they are rejected.

## Cross-references

| Spec | Relationship |
|---|---|
| [wire.md](core/wire.md) | `decode`, `kMaxEnvelopeBytes`, and the `body` double-parse / duplicate-key caveats `fuzz_wire_decode` and the adversarial run target. |
| [backend.md](core/backend.md) | `RemoteServer::handle`/`dispatchMessage` (`fuzz_dispatch_execute`'s target), `switchBackend`/`cancelPending` (the switch-backend soak test), the transports the load benchmark drives, and `LimitPolicy`/`QtWebSocketServerConfig` (the resource limits the adversarial run's default-config scenarios exercise). |
| [offline/offline.md](offline/offline.md) | `NetworkMonitor`/`ReconnectCoordinator`/`SyncWorker`, the pipeline `test_soak_reconnect_churn.cpp` drives through thousands of flaps. |
| [security.md](security.md) | The hardening tests (`test_wire_hardening.cpp`, `test_server_limits.cpp`, `test_qt_websocket.cpp`) these suites generalise from single-shot examples to distributions/time/a real adversary. |
| [core/observability.md](core/observability.md) | The `morph::observe` metrics/trace seam already instruments the same dispatch paths these suites exercise; the soak tests deliberately instrument themselves directly rather than depend on a `MetricSink` being installed in the test process — see "Soak tests" above. |
