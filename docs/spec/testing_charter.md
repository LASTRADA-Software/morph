# Testing charter

morph runs an unusually wide set of test instruments — coverage, mutation
testing, three sanitizers, fuzzing, Valgrind, a soak suite, compile-time
contract checks — and until this document, nothing said what they were
collectively supposed to guarantee. Gaps were found one at a time by whoever
happened to look (morph#403, morph#404, morph#405 were each exactly that).
This charter states the guarantee in a checkable form, names which SQLite
techniques are adopted or declined and why, and records where each adopted
instrument's reach stops — so a reader deciding what a change needs can look
here instead of rediscovering the same limits.

Modeled on [SQLite's own testing documentation](https://sqlite.org/testing.html),
which states a specific, checkable claim (100% MC/DC branch coverage under its
own harness) rather than an adjective. This document tries to hold the same
standard: every claim below names the check that enforces it, or is marked
unenforced.

## Contents

- [The guarantee](#the-guarantee)
- [Adopted techniques](#adopted-techniques)
- [Declined techniques, and why](#declined-techniques-and-why)
- [Instrument reach: known limits](#instrument-reach-known-limits)
- [What is unenforced](#what-is-unenforced)
- [Cross-references](#cross-references)

## The guarantee

**`include/morph` line coverage, gated in CI:** the `linux-coverage` job
(`scripts/coverage.sh`) fails the build if any coverage-instrumented test
binary is unaccounted for (`scripts/check_coverage_objects.sh`, morph#403), if
any coverage record resolves outside this checkout
(`scripts/check_coverage_roots.sh`, morph#426), or if branch coverage
regresses below its measured floor (`scripts/check_branch_coverage.py`,
morph#404). As of morph#429's measurement: **95.69% lines, 91.19% branches
over 2,588 arms** (179 partial lines, each either covered by a subsequent test
or recorded in `scripts/branch_partial_allowlist.json` with a reason).

That is a floor with a mechanism behind it, not a snapshot: `codecov.yml`
carries a per-subsystem component target derived from a measured ceiling, and
regressing any of them fails the PR check. It is **not** SQLite's 100%
MC/DC claim, and this document does not assert it is: MC/DC (Modified
Condition/Decision Coverage — every condition in a decision independently
shown to affect the outcome) is a stricter property than branch coverage, and
nothing in this repository's toolchain measures it. Line and branch coverage
are what CI enforces; that is the guarantee, not an unstated stronger one.

**What coverage does not guarantee, stated rather than left to be
discovered:** a line coverage figure says a statement executed, never that a
test asserted anything about what it did. morph#405 exists because of exactly
this gap — `rule_model.cpp` sat at 60.76% branch coverage while every line
executed, because the suite called the code and checked little. Coverage is
this repository's floor; mutation testing (below) is what checks whether the
floor means anything.

## Adopted techniques

| Technique | morph's instrument | Where it runs | What fails when it regresses |
|---|---|---|---|
| Statement coverage | `scripts/coverage.sh`, Codecov | `linux-coverage` CI job | Codecov's per-component status checks (`codecov.yml`) |
| Branch coverage | `scripts/check_branch_coverage.py` (aggregated LCOV, morph#404) | `linux-coverage` CI job | The gate itself: `ok: 91.19% over 2588 arms` or a hard failure below the floor |
| Mutation testing | `scripts/mutation.sh` (Mull, morph#405) | Local / on demand (46+ minutes; not yet a CI leg) | Nothing today — see "What is unenforced" below |
| Out-of-memory injection | `tests/oom_injector.{hpp,cpp}` | `tests/` suites that opt in, under plain (non-sanitizer) legs | Those tests' own assertions |
| I/O error injection (ladder only) | `examples/common/testkit/fault_proxy.hpp` | Ladder Qt test suites | Those tests' own assertions |
| Fuzzing | `tests/fuzz/` (`fuzz_wire_decode`, `fuzz_dispatch_execute`), libFuzzer | Local / on demand (`-DMORPH_BUILD_FUZZERS=ON`; not a CI leg) | A crash, hang, or sanitizer trip during a campaign; regression cases preserved under `tests/fuzz/findings/` |
| AddressSanitizer | Compiler instrumentation | `linux-sanitizers` (`clang-asan`), `ladder-sanitizers` | The CI job (nonzero exit on any diagnostic) |
| UndefinedBehaviorSanitizer | Compiler instrumentation | `linux-sanitizers` (`clang-ubsan`), `ladder-sanitizers` | The CI job |
| ThreadSanitizer | Compiler instrumentation | `linux-sanitizers` (`clang-tsan`), `kanban-tsan` | The CI job |
| Valgrind (memcheck) | Runtime instrumentation | `valgrind` CI job | The CI job |
| Long-running / soak | `tests/soak/` | Local / on demand (`-DMORPH_BUILD_LOAD_TESTS=ON`; not a CI leg — see `docs/spec/testing_strategy.md`) | Those tests' own assertions over many cycles |
| Compile-time contract checks | `tests/compile_checks/` | Every configure that reaches `tests/CMakeLists.txt` | `FATAL_ERROR` at configure time |
| Multiple independent harnesses | Catch2 suite + `scripts/scenario/` (wire-level scenario corpus against real rung servers) | `tests/`, local scenario runs | Catch2 assertions; `scripts/scenario/scenario_coverage.py`'s drift gate (self-tested in `drift-guard.yml`) |
| Error-path instrumentation | `scripts/check_error_path_coverage.py` (morph#406) | Local / on demand; self-test only in `drift-guard.yml` today | Its own self-test; not yet wired as a build-blocking gate (see "What is unenforced") |

## Declined techniques, and why

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

## Instrument reach: known limits

Naming a limitation here is what lets a reader trust the technique for what
it does cover, rather than either over-trusting it or discovering the gap by
hand later.

- **`oom_injector` is disabled under ASan, TSan, and Valgrind.** Documented in
  its own header (`tests/oom_injector.hpp`): ASan and TSan's runtimes already
  interpose `operator new`/`operator delete`, so this injector's own
  interposition either double-hooks or is silently bypassed; Valgrind's
  memcheck does the same for allocation tracking. CI excludes the tagged
  cases from those legs (via `ctest -E` and a Catch2 tag filter) rather than
  running them somewhere they cannot work. Consequence: an allocation-failure
  path that would only misbehave under one of these three legs specifically —
  a race the allocator hides without ASan/TSan's own instrumentation, a leak
  memcheck would otherwise catch — has no combined "OOM + sanitizer" leg
  checking it. Nothing in this repository closes that gap today.
- **`fault_proxy` is ladder-only.** It lives under
  `examples/common/testkit/`, built against `QtWebSocketBackend`/
  `QtWebSocketServer`, and gives the application ladder (kanban, ledger, lims,
  crm, polls) per-call fault injection — drop, delay, duplicate, or kill a
  reply frame. `include/morph` itself (the framework's own
  `RemoteServer`/backend layer) has no equivalent seam: its own I/O failure
  paths are exercised only by whatever the transport itself can be made to do
  (closing a real socket, a real timeout), not by a scriptable proxy.
- **No I/O-failure seam for `include/morph/net`.** The network stack
  (`socket_backend.hpp`, `socket_server.hpp`, `net/detail/` — `tcp_socket`,
  `ws_handshake`, `ws_frame`, `sha1`, `base64`) holds 42 of the library's 148
  `throw` sites (morph#406) and has no per-syscall error injection comparable
  to SQLite's VFS-layer fault injection. A `recv`/`send`/`connect` failure
  path in this subsystem is exercised only by whatever a real socket can be
  made to do in a test (closing the peer end, binding a taken port), not by
  injecting a specific `errno` on demand.
- **Mutation testing is local-only — on cost and tooling, not on a compiler
  mismatch.** `scripts/mutation.sh` requires Mull, which is on no runner
  image, whose IR frontend has to be version-matched to the compiler by hand,
  and whose campaign takes 46+ minutes over `core` and `forms` alone. What
  does *not* keep it out is a toolchain difference: CI pins clang 22
  (`.github/workflows/ci.yml`, enforced by `scripts/check_ci_clang_pin.sh`)
  and the 64.06% figure (`scripts/mutation_survivors.json`, morph#429) was
  measured on that same major — clang 22.1.8 — so the mutant set Mull derives
  from LLVM IR is the one the compiler CI itself uses would emit. That figure
  covers `include/morph/core` and `include/morph/forms` only; it has not been
  extended to `net`, `offline`, `journal`, `util`, or `session`, and is not
  re-taken automatically when either subsystem changes.
- **Error-path coverage (morph#406) measures execution, not assertion
  quality.** `scripts/check_error_path_coverage.py` answers "did a test drive
  this specific `throw` statement or enter this specific `catch` arm", cross-
  referencing the aggregated coverage LCOV against every throw/catch site
  under `include/morph`. It cannot tell a `catch (...) { /* swallow */ }`
  that ran because a test drove it apart from one that ran and asserted
  nothing about what it caught — that second question is mutation testing's,
  not this instrument's, and is why both exist rather than one standing in
  for the other.

## What is unenforced

Named honestly rather than folded into the table above as if a check existed:

- **Mutation score has no floor.** `scripts/mutation.sh` reports a number;
  nothing fails a build if it drops. A regression would have to be noticed by
  someone re-running the campaign and comparing by hand.
- **Error-path coverage (morph#406) has no build-blocking gate yet.**
  `scripts/check_error_path_coverage.py --self-test` runs in `drift-guard.yml`
  (proving the *instrument* still detects what it is meant to), but the
  instrument itself is not yet wired into `scripts/coverage.sh` to fail a
  build over an uncovered, unallowlisted throw/catch site. `core`'s 50 sites
  being covered first (morph#406's own stated priority) is the work that
  would make turning this into a hard gate survivable rather than an
  immediate wall of red.
- **Fuzzing has no continuous campaign.** `tests/fuzz/`'s two harnesses run
  locally, on demand, seeded from `tests/fuzz/corpus/`. Nothing runs them
  automatically against new commits (no OSS-Fuzz integration, no scheduled CI
  job), so a regression is only caught by someone choosing to run a campaign.
- **Soak tests are opt-in and not scheduled.** `tests/soak/` requires
  `-DMORPH_BUILD_LOAD_TESTS=ON` and is not part of any CI leg
  (`docs/spec/testing_strategy.md`); a long-running-only defect is only found
  by someone running it by hand.

## Cross-references

| Spec | Relationship |
|---|---|
| [testing_strategy.md](testing_strategy.md) | The opt-in test categories this charter's table cites in detail — fuzz harness, soak tests, load benchmark, adversarial cross-socket run. |
| [error_handling.md](error_handling.md) | The propagation design morph#406's error-path instrument measures test coverage of. |
| `codecov.yml` | The per-subsystem coverage targets and the artifact-audit allowlists (`branch_partial_allowlist.json`, `error_path_allowlist.json`) this charter's guarantee is enforced through. |
| `scripts/mutation_survivors.json` | The triaged survivor list behind this charter's 64.06% mutation-score figure. |
| `tests/oom_injector.hpp` | The OOM-injection limitation this charter states under "Instrument reach". |
| `examples/common/testkit/fault_proxy.hpp` | The ladder-only fault-injection seam this charter states has no `include/morph`-side equivalent. |
