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
- [Install / export consumability](#install--export-consumability-scriptscheck_install_exportsh)
- [Declined techniques, and why](#declined-techniques-and-why)
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
hold the committed reproducers (two today) and are where any input that
triggers a crash/hang/sanitizer report **in a bug that has since been fixed**
gets committed as a permanent regression case — see "Known findings" below for
inputs discovered but not yet in that state.

**CI-integrated regression check.** `ctest -R fuzz_.*_replay` runs each target
once per file in its corpus + findings directories (libFuzzer's single-run
replay mode — passing individual files, not a directory, so it replays and
exits rather than starting a mutating fuzzing session) and fails if any input
now crashes. This is fast and deterministic, suitable for CI; it is **not** a
fuzzing campaign.

The input globs are deliberately **extension-agnostic** (`*`, not `*.txt`): a
fuzzer input is an arbitrary byte string, and a libFuzzer-minimized reproducer
is conventionally saved as `.bin`. An extension-scoped glob silently skips
whatever it does not match, which is exactly what happened — both committed
reproducers are `.bin`, so the replay tests ran only the seeds and never the
crash inputs, leaving the test that exists to catch a `skip_ws` regression green
if the bug came back. An empty glob is now a configure-time `FATAL_ERROR` too:
a replay invocation with no `FILE` arguments is an unbounded fuzzing run, not a
regression check. CI additionally asserts, after the run, that every file under
`tests/fuzz/findings/` was actually referenced — a guard that never fires is
indistinguishable from one that works.

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

  That fix covered `message` only. The same writer gap applies to **every**
  `Envelope` string — `body`, `modelType`, `actionType`, `contextKey`, `typeId`,
  and the session's `principal`/`token` — which carry caller data that must
  round-trip byte-for-byte, so substitution is the wrong instrument there. Worse
  than invalid output, glaze's chunked write path *corrupts* such a byte when
  the same string also holds a `\` or `"`, emitting two `0x00` bytes in its
  place; the envelope still decodes, so nothing downstream can notice. Now fixed
  at the writer with `wire::detail::EscapingWriteOpts` (glaze's
  `escape_control_characters`), which is lossless in both directions.
  `makeErr` keeps its substitution for a different reason: an err message is
  log-bound text, and a raw `0x1B` in it would carry an ANSI escape into the
  reader's terminal.

  That fix, in turn, covered the *envelope* only. An execute envelope's `body`
  is not written by `wire::encode` at all — it is produced separately by
  `ActionTraits<A>::toJson` / `resultToJson` (registry.hpp's
  `BRIDGE_REGISTER_ACTION` macro), which wrote with plain `glz::write_json` and
  so reproduced the identical gap for every string field of every action and
  result. Action bodies are pure caller data (a paste's content, a chat
  message, a filename), so this is at least as exposed as the envelope was.
  Found from the other end — by the application ladder's rung 1 (pastebin)
  replaying `tests/fuzz/findings/` as *paste content*, which is the round trip
  its README's "hostile content" requirement asks for — and fixed with the same
  instrument one layer down, `model::detail::EscapingWriteOpts` (see
  docs/spec/core/registry.md, "Control bytes in action and result bodies").

These fixes are covered by dedicated regression tests in
`tests/test_wire_hardening.cpp` ("Bug C"/"Bug D"/"Bug E"/"Bug F"/"Bug G") in
addition to the `fuzz_*_replay` findings above.

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

Both `morph_soak` and `morph_bench` are sanitizer-instrumented when
`AF_SANITIZER` is set (morph#542). They are opt-in, so no default sanitizer leg
pays for them; the reason to instrument them rather than exempt them is that
churn over thousands of cycles is exactly the shape of test whose finding is a
leak or a race and not a failed assertion. Under a sanitizer preset the
benchmark's published numbers are not comparable with an ordinary run's and are
not meant to be — there it is a correctness run over the dispatch path.

## Load / latency benchmark (`tests/bench/`)

Built only under `-DMORPH_BUILD_LOAD_TESTS=ON` alongside the soak tests (same
option; different target, `morph_bench`). `bench_dispatch_latency.cpp` drives
a `RemoteServer` directly against a trivial echo model (isolating framework
overhead from business logic):

- **Latency** — 2000 serial (concurrency-1) round trips; reports p50/p95/p99
  wall time in milliseconds (nearest-rank percentile over the sorted sample).
- **Throughput** — a window at each of concurrency 1/2/4/8/16, reporting
  executes/second (`MORPH_BENCH_WINDOW_MS`, default 200 ms).
- **Both phases run `MORPH_BENCH_TRIALS` times (default 5)**, and the run
  reports the best, median and worst trial of every figure rather than one
  number — morph#687, below.
- Writes `bench_dispatch_latency.json` into the build directory. The
  `p50_ms`/`p95_ms`/`p99_ms`/`throughput` keys of the old schema are still
  there and now carry the *best* trial; `trials`,
  `latency_samples_per_trial`, `throughput_window_ms`,
  `p99_ms_median_trial`, `p99_ms_worst_trial`, each throughput point's
  `executes_per_sec_worst_trial`, and a `trial_detail` array with every
  trial's own figures are additive. Successive runs are archived and diffed
  for a regression, and `trial_detail` is what a diff finer than the gates
  below has to read.
- Enforces two coarse regression gates via `CHECK`: `p99 <=
  MORPH_BENCH_P99_MS_MAX` (default 50.0 ms) and concurrency-1 throughput `>=
  MORPH_BENCH_MIN_THROUGHPUT` (default 500.0 executes/sec). Both are
  environment-variable-overridable so CI hardware differences don't need a
  code change, and both read the **best** trial — see below.

**The serial phase used to report the test harness's polling step (morph#687).**
It waited on each reply with `morph::testing::WaitReply`, whose `await()` calls
`waitUntil`, which sleeps 5 ms between predicate checks. Measured on
`e9dad027`, same binary, 20 processes per configuration, Release:

| | idle | 16-way oversubscribed |
| --- | --- | --- |
| p50 | 5.0562 / 5.0714 / 5.0740 ms | 0.0166 / 0.0167 / 0.0216 ms |
| c=1 executes/sec | 171467 / 175928 / 178855 | 311.9 / 1749.9 / 18627.1 |

A **302x** swing in the headline figure, selected by machine load, and neither
mode was the dispatch latency: the same idle processes reported ~176k
executes/sec at concurrency 1, i.e. a round trip of about 5.7 µs. An idle
machine reported one whole sleep step per call; a busy one reported the case
where the reply beat the caller's first predicate check. 311.9 executes/sec is
also *below* the 500/sec floor the file has always enforced, so the gate was
already firing on machine load rather than on morph. The benchmark now uses a
local condition-variable reply sink, and a blocking drain at the end of each
throughput window for the same reason — the old polling drain sat inside the
window's own elapsed time.

**It reports a distribution because a wall-clock figure has no regime to pin.**
`morph_bench_alloc` answers morph#687 by pinning its race, and an allocation
count then comes out exact. Contention is not a mode, it is a tax, so this
benchmark takes morph#687's other option and reports the spread over trials.
The gates read the best trial: contention can only make latency worse and
throughput lower, so the best of N is the least contaminated estimate of what
the code costs, while a real regression moves every trial including the best.
Measured, 20 processes per configuration, best-of-five figures:

| | p99 (ms) | c=1 executes/sec |
| --- | --- | --- |
| Release idle | 0.0090 / 0.0096 / 0.0104 | 173169 / 176282 / 181641 |
| Release loaded | 0.0181 / 0.0210 / 0.0259 | 56825 / 61211 / 63479 |
| Debug idle | 0.0325 / 0.0343 / 0.0360 | 42560 / 43150 / 43996 |
| Debug loaded | 0.0598 / 0.0608 / 1.8717 | 1549 / 22684 / 22990 |

The Debug rows are the ones the defaults must hold for: the only CI leg setting
`MORPH_BUILD_LOAD_TESTS=ON` is `linux-all-features`, on the `gcc-debug` /
`clang-debug` presets, and its `ctest --preset` passes no label filter. **The
two ceilings are therefore unchanged, and that is a measurement rather than
timidity**: Debug-under-load still spans 28x on throughput and 57x on p99 for
the same binary, so no pair of constants separates a regression from a
contended runner. 50 ms is ~27x the worst best-trial p99 measured and 500/sec
~3.1x below the worst best-trial throughput — a dispatch path made three times
slower passes both. That limit is morph#707; setting the floor from a 12-core
box was tried and turned 3 of 20 Debug-under-load processes red.

**The property those ceilings were feared to be leaving ungated is gated
elsewhere, tightly.** morph#707 asks that an allocation gate be weighed before
any wall-clock ceiling is tightened. It already exists: `bench.alloc_budget`
(below) is set by rule at one allocation above the figure the benchmark
measures — not a tolerance band — and that figure came out identical on every
one of 20 processes across three toolchains, idle and loaded alike: a
one-allocation margin, on a quantity machine load cannot move.
**The two numbers are deliberately not restated here.** Both live in
`tests/bench/CMakeLists.txt` — the ceiling as `MORPH_ALLOC_BUDGET_PER_CALL`,
the measured figure in the comment that derives it — and they move together
every time the dispatch path gets cheaper. When this paragraph carried copies
of them they went stale twice in three days (morph#743, morph#758), while the
argument they were quoted for survived both unchanged; so the argument is what
this paragraph keeps, and the file above is where the current values are.
So "dispatch does not get more expensive" is already gated to a resolution no
wall-clock constant on any host can approach. What the two ceilings gate is the
residue: a regression that costs time without costing allocations — a spin, a
syscall, a lock held longer. That is a real class, and it is the only class
they are for.

**Two things a future tightening needs, which the benchmark now produces.**

- **A cross-process distribution** (`bench_dispatch_latency.jsonl`, beside the
  per-process `.json`, overridable with `MORPH_BENCH_LEDGER`, `off` to
  disable). This is morph#687's remaining half: `MORPH_BENCH_TRIALS` gives a
  distribution over trials *within* a process, which mitigates the spread but
  does not record it — one process still prints one triple and cannot say
  where it sits among others. Each run appends one line (`pid`, the headline
  percentiles, concurrency-1 throughput, `load_1m`, `inject_delay_us`) and
  prints its own rank among every line already there, so N unorchestrated runs
  produce the distribution a candidate ceiling would be read off. The load
  average is on the row and not only on stdout, because morph#710's sweeps put
  the same binary 28x apart on throughput between an idle box and a loaded one:
  a figure without its load is not comparable with another figure. Rows are
  appended `O_APPEND` in one write and a row that cannot be parsed is skipped,
  because a diagnostic ledger must never redden a benchmark.
- **An injectable regression** (`MORPH_BENCH_INJECT_DELAY_US`), which spins for
  that many microseconds inside the model's `execute` — inside the round trip
  both phases measure. A spin and not a sleep: a sleep yields the core and
  would flatter the throughput phase. This is what makes "what size of
  regression does this gate catch?" a command rather than an argument, and it
  is what any candidate ceiling should be shown firing against before it lands.
  A run carrying an injection says so on stdout and on its ledger row.

**Both ceilings fire, and the size they fire at is measured.** Release, clang
22.1.8, 12-core x86-64 Linux, `MORPH_BENCH_TRIALS=1`, load average 11.9–12.8:

| injected | p99 (ms) | c=1 executes/sec | p99 gate | throughput gate |
| --- | --- | --- | --- | --- |
| 0 | 0.0142 | 168862 | pass | pass |
| 1000 µs | 1.02685 | 989.2 | pass | pass |
| 2000 µs | 2.03428 | 495.5 | pass | **FAIL** |
| 3000 µs | 3.18618 | 331.2 | pass | **FAIL** |
| 60000 µs | 61.0339 | 16.7 | **FAIL** | **FAIL** |

This is the first time either `CHECK` has been shown firing on anything. The
baseline round trip is ~5.9 µs, so the throughput floor — the tighter of the
two — first speaks at roughly a **340×** regression and the p99 ceiling at
roughly **8500×**, confirming morph#707's ~800× estimate by measurement and
understating it for the p99 half. The ratio, not the time, is the portable
part: reproduce the table on any host with `MORPH_BENCH_INJECT_DELAY_US`.

What is still **not** done, and is why morph#707 stays open: nobody has
characterised the CI runner. Both sets of figures above come from a
workstation, which morph#707 identifies as exactly the misleading
configuration, so the ceilings are not tightened here. Guessing a second time
from the same box would be the first mistake with a different number.

The echo model/action (`BenchEchoModel`/`BenchEchoAction`) are declared at
file scope, not inside the file's anonymous namespace with its other local
helpers — Glaze's reflection needs external linkage to mangle the type name,
the same requirement `tests/fuzz/`'s harness fixtures document.

### Allocation census (`bench_dispatch_allocations.cpp`, target `morph_bench_alloc`)

A second binary under the same option, and deliberately **not** a second case
in `morph_bench`: it replaces the global `operator new`/`delete`, which is
process-wide and would perturb any other measurement sharing the binary. It
counts the heap allocations one `Ping -> Pong` round trip costs through
`LocalBackend` — 50 warm-up calls excluded, every call waited out, no JSON and
no socket — and prints the total, the bytes, and (with `--attribute`) the size
of every allocation in one steady-state call.

It also runs a group of narrower censuses over morph's four registry lookups,
each one taken over ids past libstdc++'s 15-character SSO buffer and again over
ids inside it — the cost is entirely id-length-dependent, so a census over one
side alone either measures zero or overstates the saving, and morph's real ids
straddle the line (`"CreateSwimlane"` is 14 characters, one under):

| census | before | after | pure lookup? |
| --- | --- | --- | --- |
| `ActionDispatcher::coalesce` + `requiredFieldsFor` | 2.00 / 0.00 | 0.00 / 0.00 | yes (morph#572 Part C) |
| `PayloadMigrationRegistry::find` | 2.00 / 0.00 | 0.00 / 0.00 | yes (morph#699) |
| `BridgeHandler::executeJson` | 24.07 / 21.06 | 21.07 / 21.06 | no — a whole round trip (morph#699) |
| `ModelRegistryFactory::create` | 2.00 / 1.00 | unchanged | no — constructs a holder (morph#709, parked) |

The first two decode nothing and execute nothing, so what they allocate is
exactly what looking a registry key up costs. The last two do more than look
up, so their figure is a floor plus the key and what is comparable between runs
is the *difference* between the long-id and short-id columns.

It exists because morph#572 is scoped by a number that three later pull
requests invalidated, and re-deriving such a number from a prose description of
how it was once taken is how a fix ends up built against a figure nobody
re-checked.

**It is both an instrument and a control, and which one it is at any moment
depends on the flags.** Run bare, it asserts nothing and the number it prints
is the whole output; a green run of it in that mode proves nothing. Given
`--budget=<n>` and/or `--lookup-budget=<n>` it fails when the figure exceeds
the ceiling, and it is registered with ctest in exactly that form, as
`bench.alloc_budget`.

The reason it took a per-toolchain budget to get there is that an allocation
count is standard-library specific, not only morph-specific: `std::function`'s
inline buffer and `std::string`'s SSO threshold differ between libstdc++,
libc++ and MSVC's STL. So the ceiling is the cache variable
`MORPH_ALLOC_BUDGET_PER_CALL`, defaulted to a figure measured on the
toolchains that actually build this target in CI (Linux `clang-debug` and
`gcc-debug`), left unset on MSVC where nothing has measured it, and settable
to 0 to disable the gate. `tests/bench/CMakeLists.txt` carries the
measurements the default was chosen from and the headroom argument.

The `--lookup-budget` half takes no headroom at all: after morph#572's Part C
and morph#699 a *pure* registry lookup allocates **nothing**, for ids of any
length, and that is a property which either holds or has regressed. The ctest
case passes `--lookup-budget=0`, and it covers `ActionDispatcher` and
`PayloadMigrationRegistry`.

`--id-length-budget` covers the third registry, `ActionExecuteRegistry`, which
cannot be probed on its own — it dispatches whatever it finds — so what is
gated is the gap between an `executeJson` over long ids and one over short
ones. **0.5 rather than 0, and the 0.5 is measured**: the figure reads 0.01
because of a deterministic two-allocation one-off across a census's 200 calls,
shown to follow census *order* rather than id length by swapping the two
censuses, and identical in all of 10 processes. Reverting either half of
morph#699's change to that path takes the gap to 3.00 (the `Key`'s two
`std::string`s, plus the one `executeJson` built from
`ModelTraits<Model>::typeId()`), so 0.5 separates the residue from the thing
guarded with wide margin on both sides. Like the lookup half it needs no
per-toolchain default: it is a difference between two runs of one build.

All three ceilings were checked against a reverted fix. Removing
`PayloadMigrationRegistry`'s `PairKeyEqual` takes `--lookup-budget` to 2.00 and
turns the case red; restoring `ActionExecuteRegistry`'s `Key{...}` temporary
takes `--id-length-budget` to 2.01; restoring `executeJson`'s
`std::string{typeId()}` alone takes it to 1.01.

**Both halves were checked against a reverted fix rather than only against
themselves** — see morph#572's pull request for the red runs. A budget that has
never been seen to fail is the control-that-measures-nothing this document's
own charter warns about.

**The dispatch census pins a race, and without that pin it is not comparable
between runs.** A dispatch and the handlers attached to it race: win, and each
handler joins a vector the settle drains; lose, and each takes
`CompletionState`'s attach-after-ready path, which costs differently. Before
the benchmark gated its worker thread, that made the headline figure bimodal —
measured, interleaved, 20 processes per configuration: **~16.95 on an idle
machine, ~13.06 with the machine 16-way oversubscribed**, same binary. That is
morph#687's instability with a cause attached. The benchmark now holds the
worker across `execute()` and both attaches, so it always measures the
attach-before-settle regime: the more expensive of the two, and the one a real
GUI client is in.

With the race pinned, measured on `a9cb5649` before morph#572's Parts A and C,
x86-64 Linux, clang 22.1.8 / libstdc++ 16.2.1: **17.06 allocations per local
round trip**, and 2.00 allocations per registry lookup for ids past the SSO
buffer. After: **14.06** and 0.00 per lookup — exactly 3.00 removed, in every
one of 40 processes, idle and loaded, on clang Release, clang Debug and gcc
Debug alike. Note the "before" figure is not morph#572's own 19.2: that was
taken at `4017228d`, before morph#689 changed the strand's map-node handling,
and on an ungated benchmark. See morph#572 for the per-line attribution.

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

## Test type naming: avoid file-scope name collisions

`tests/**/*.cpp` files are written independently, and each is free to declare
its own local model/action types (`OrderModel`, `Widget`, `Report`, …) for
its scenario. A type declared at **file scope** — not inside an anonymous or
named namespace — has external linkage. If two different test files declare
a same-named, differently-defined type at file scope, that is a One
Definition Rule (ODR) violation: undefined behavior that neither the
compiler nor the linker diagnoses, since each translation unit only ever
sees its own definition. Which definition the linker keeps for a given call
site is link-order dependent, so the bug can pass locally and fail in CI (or
the reverse), with no diagnostic pointing at the cause. This happened for
real: see issue #84 — a bare `OrderModel` stub in one file silently won over
another file's real `OrderModel` in some builds, so the real one's
`onBackendChanged()` was never invoked and its offline queue never drained.

**Convention**: wrap local model/action types in an anonymous namespace by
default. Only lift a type to file scope when a macro that requires external
linkage (`BRIDGE_REGISTER_MODEL`, `BRIDGE_REGISTER_ACTION`) needs it — and in
that case, prefer a short, file/feature-specific prefix over a generic name
(`StepILOrderModel`, not `OrderModel`; see `tests/test_remote_step_interleaving.cpp`).
A type declared inside a *named* namespace is also safe, since its linker
symbol is namespace-qualified (e.g. `namespace issue21::models { struct
Report { ... }; }`, used for `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION`
per issue #21).

**Why UndefinedBehaviorSanitizer (`clang-ubsan`, `cmake/compiler_options.cmake`'s
`-fsanitize=undefined`) does not catch this.** UBSan instruments individual
operations *within* a translation unit at compile time — signed overflow, an
invalid enum load, a misaligned access, a bad `vptr` cast — and its runtime
checks fire while that instrumented code executes. An ODR violation across
two TUs has no such moment: each TU's own code is entirely well-formed and
runs exactly as instrumented: `test_conflict_resolution.cpp`'s `OrderModel`
calls its own real `onBackendChanged()` correctly when compiled and run
alone, and `test_remote_step_interleaving.cpp`'s bare stub is equally
well-formed on its own. The defect exists only in what the **linker** does
across the two `.o` files — silently keeping one TU's definition of
`BackendChangedNotifiable<OrderModel>` for both — which happens before any
sanitizer runtime is even loaded. No sanitizer (ASan, UBSan, TSan, MSan) is
designed to catch cross-TU ODR violations; this is a structural blind spot
of the whole compile-time-instrumentation family, not a configuration gap.

The linker's own ordinary duplicate-symbol diagnostics don't fire either,
for a related reason: the affected symbol
(`BackendChangedNotifiable<OrderModel>`, a template instantiation) is
implicitly `inline`, and the linker treats multiple same-mangled-name
`inline`/template definitions across TUs as expected COMDAT folding — the
normal, correct case when two TUs instantiate the same template
identically. It has no way to distinguish that from "two different
definitions that happen to share a mangled name," which is exactly this bug,
so it silently keeps one arbitrarily in both cases. (GNU gold's
`--detect-odr-violations` is the closest built-in linker feature, but it is
gold-specific, heuristic on this exact template-instantiation shape, and not
wired into any preset here.)

This is why the fix is a **source-level static check**
(a gate removed on 2026-09-23, above) rather than a build flag: it is
the only layer that can see both TUs' declarations before they are ever
compiled down to symbols a linker or sanitizer could reason about.

**CI-enforced**: a gate removed on 2026-09-23 scans every
`tests/**/*.cpp` file for file-scope `struct`/`class` declarations (template
specializations, which qualify their own name and specialize an existing
template rather than declaring a new one, are excluded) and fails if the
same simple name appears at file scope in more than one file. Self-tested by
a gate removed on 2026-09-23 against the fixtures in
`tests/lint/test_type_names/` before the real scan runs, following the same
"test the checker first" pattern as the deprecation-marker lint.

## Install / export consumability (`scripts/check_install_export.sh`)

Every other suite in this document builds morph from *inside* the tree, where
`include/` is on the include path because the build put it there. That is how
morph#232 survived: `cmake --install` exited 0 having installed Glaze's headers
and a working `glazeConfig.cmake` — Glaze carries its own install/export rules
and gets them for free through `FetchContent` — while installing zero morph
headers and no `morphConfig.cmake`. No CI leg installed morph, so nothing
noticed. A consumer following the standard CMake workflow got a prefix holding
someone else's dependency and none of the library they meant to install.

**CI-enforced** by a dedicated job (`install-export` in `ci.yml`), which
self-tests the checker first, then runs it. The checker installs morph to a
scratch prefix and *compiles and runs* a consumer project outside the tree —
`find_package(morph CONFIG REQUIRED COMPONENTS net)` plus
`target_link_libraries(consumer PRIVATE morph::morph morph::net)` over one
translation unit including `morph/core/bridge.hpp`, `morph/forms/forms.hpp`,
`morph/util/quantity.hpp`, `morph/util/rational.hpp` and `morph/version.hpp`.

Compiling is the point, not `find_package` succeeding. Three defects in the
install rules produced a prefix that `find_package` accepted and that no
filename inspection would have questioned:

- installing only the declared `FILE_SET HEADERS` omits
  `morph/detail/fixed_string.hpp` and `morph/detail/quantity_equation.hpp`,
  which public headers include, so the installed tree resolves in the build
  directory and nowhere else;
- `install(EXPORT … NAMESPACE morph::)` prefixes the *target* name, so
  `morph_net` arrives as `morph::morph_net` while every in-tree alias says
  `morph::net`;
- without `find_dependency(glaze)` in the package config, `morphTargets.cmake`
  names an imported target nobody created.

The checker also builds `morph_verify_interface_header_sets`, which is not part
of `all` and which nothing else in CI builds. That phase is what keeps
`INTERFACE_HEADER_SETS_TO_VERIFY` honest: the `detail/` header set is
*installed* but deliberately outside the *verified* set, because
`morph/detail/quantity_equation.hpp` is included partway down
`morph/util/quantity.hpp` and is not self-contained. It is the one slow phase,
and `--skip-header-set-verification` drops it for the self-test's fast cases.

Two vacuity guards, because a consumability check that measures nothing looks
exactly like one that passes: the run fails if no optional component was
installed (leaving the exported-name check with nothing to read), and a
consumer asking for a component that was *not* installed must fail at
`find_package` rather than succeeding and breaking at link time.

Self-tested by `scripts/test_check_install_export.sh`, which reintroduces each
defect into a scratch copy of the tree one at a time and requires the checker
to catch each one *for the stated reason* — the same "test the checker first"
pattern as the deprecation-marker and test-type-name lints.

## Declined techniques, and why

Kept from `testing_charter.md`, which was removed on 2026-09-23 along with the
meta-gates it inventoried. The inventory drifted every time a gate moved; these
are scope decisions about what this library is, and they do not.

- **Crash / power-loss simulation.** SQLite ships a storage engine of its own
  and must survive an OS crash mid-write; morph does not own storage — its
  offline queue (`include/morph/offline`) delegates durability to SQLite
  itself (`sqlite_offline_queue.hpp`) or to plain file I/O
  (`file_offline_queue.hpp`), and a crash-durability claim about *those*
  belongs to SQLite's own test suite and the filesystem's fsync contract, not
  to a simulation morph would have to build and maintain. Declined as
  out-of-scope for what this library is, not as a gap.
- **A proprietary, dedicated coverage harness (SQLite's TH3).** SQLite's
  headline coverage figure is produced by TH3, which is not shipped with
  SQLite and is not free to obtain. Citing that number as a model without
  the harness that produces it would make this charter aspirational rather
  than checkable — the constraint this whole document exists to avoid (see
  "The guarantee" above). morph's coverage figures are produced entirely by
  tools already in this repository (`scripts/coverage.sh`,
  `llvm-cov`/`llvm-profdata`), so anyone can reproduce them.
- **100% MC/DC as a stated target.** Named directly in "The guarantee" above:
  this repository measures line and branch coverage, not MC/DC, and does not
  claim to. Adopting the *number* without the instrument that verifies it
  would be exactly the mistake the TH3 point above describes.


## Cross-references

| Spec | Relationship |
|---|---|
| [wire.md](core/wire.md) | `decode`, `kMaxEnvelopeBytes`, and the `body` double-parse / duplicate-key caveats `fuzz_wire_decode` and the adversarial run target. |
| [backend.md](core/backend.md) | `RemoteServer::handle`/`dispatchMessage` (`fuzz_dispatch_execute`'s target), `switchBackend`/`cancelPending` (the switch-backend soak test), the transports the load benchmark drives, and `LimitPolicy`/`QtWebSocketServerConfig` (the resource limits the adversarial run's default-config scenarios exercise). |
| [offline/offline.md](offline/offline.md) | `NetworkMonitor`/`ReconnectCoordinator`/`SyncWorker`, the pipeline `test_soak_reconnect_churn.cpp` drives through thousands of flaps. |
| [security.md](security.md) | The hardening tests (`test_wire_hardening.cpp`, `test_server_limits.cpp`, `test_qt_websocket.cpp`) these suites generalise from single-shot examples to distributions/time/a real adversary. |
| [core/observability.md](core/observability.md) | The `morph::observe` metrics/trace seam already instruments the same dispatch paths these suites exercise; the soak tests deliberately instrument themselves directly rather than depend on a `MetricSink` being installed in the test process — see "Soak tests" above. |
