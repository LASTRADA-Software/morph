# Framework Coverage & Mutation Improvement Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Push `include/morph` (the framework, not the example apps) as close to 100% line/branch coverage as an honest ceiling allows, and turn today's mutation-testing survivor list into a small, individually-justified residual instead of an unaudited 352-entry pile.

**Architecture:** This is an audit-then-close loop, not a spec-then-build one — the exact set of uncovered lines/branches/mutants is not knowable until measured, so each phase opens with a concrete audit task (exact commands, concrete artifact) and closes with a per-finding TDD loop (Catch2 case that fails on the gap, passes once it's covered, or a documented reason the gap is real and permanent). This mirrors the pattern already used successfully in this repo's own history (`b54854e8`, `086b54f2`, `d5c455f2`): measure, classify honestly, close what's real, write down what isn't.
**Tech Stack:** C++23, Catch2, llvm-cov (clang source-based coverage), `scripts/coverage.sh` / `scripts/check_branch_coverage.py` / `scripts/aggregate_lcov_branches.py`, Mull 0.34.0 (`scripts/mutation.sh`) for mutation testing, CMake presets.

## Global Constraints

- **Scope is `include/morph` only** (core, forms, net, offline, journal, session, util, qt, detail, render) — the user asked for "the main framework code," and `examples/**` already has its own ladder-component coverage program in `codecov.yml`. Do not touch example-app coverage as part of this plan.
- **Never round a survivor/miss up to "gap" without checking.** Per `AGENTS.md`'s "Verify rather than assert" and the measured false-positive mechanism already on record in `scripts/mutation_survivors.json` (`false_positive_finding`), a coverage miss or mutation survivor is a *candidate*, not a confirmed defect, until hand-checked.
- **A line/branch/mutant that is provably unreachable is not a task failure.** Document it (in `scripts/branch_partial_allowlist.json` for branches, `scripts/mutation_survivors.json`'s `equivalent` class for mutants, or a `codecov.yml`-style comment for lines) with the same rigor already on record there — a bare suppression is treated as a defect in this repo (morph#404's own words), so every exclusion needs a reason a stranger can check.
- **One local build at a time; never park more than 2 background agents.** These builds and the mutation run are CPU-heavy (ninja saturates cores; the mutation run at `core-forms` scope alone takes ~45 minutes on 12 workers). Serialize coverage builds; if parallelizing subsystem work across agents, cap at 2 concurrent and keep them in separate worktrees so they aren't fighting over `build/clang-coverage`.
- **Toolchain note carried from `scripts/coverage.sh` and `scripts/mutation.sh`:** all baseline numbers in this plan were measured on clang 22.1.8; CI pins clang 20. Numbers may drift a fraction of a point on re-measurement — that's expected, not a sign the audit was wrong.
- **File what you find.** Anything discovered during this work that isn't in scope for the current task (a real bug uncovered while writing a coverage test, a defect in the coverage tooling itself) gets filed as its own issue per `AGENTS.md`, not folded into the coverage change.
- **Commit per finding-group, not per line.** Match the repo's own commit granularity (e.g. `c554edd3`, `0d2fc0be`): one commit per file or closely-related cluster of gaps, not one commit per assertion.

---

## Current baseline (measured 2026-09-02/03, recorded in this repo already — re-verify in Phase 0 before trusting)

**Line coverage, `include/morph`, 93.98% overall (7,977/8,488), from `codecov.yml`:**

| subsystem | line % | lines | misses | partials |
|---|---|---|---|---|
| net | 77.61% | 1,157 | 193 | 66 |
| forms | 94.68% | 1,560 | 58 | 25 |
| offline | 91.94% | 633 | 36 | 15 |
| core | 97.35% | 3,249 | 35 | 51 |
| util | 97.83% | 1,012 | 4 | 18 |
| session | 97.19% | 249 | 4 | 3 |
| qt | 97.44% | 39 | 1 | 0 |
| journal | 99.44% | 357 | 1 | 1 |
| detail | 100.00% | 155 | 0 | 0 |
| render | 100.00% | 77 | 0 | 0 |

**Branch coverage, `include/morph`, 91.19% overall, from `scripts/check_branch_coverage.py`'s `FLOORS` table:**

| subsystem | branch % | floor |
|---|---|---|
| net | 75.15% | 72% |
| offline | 86.25% | 83% |
| forms | 92.36% | 89% |
| core | 93.22% | 90% |
| session | 95.83% | 92% |
| util | 93.99% | 91% |
| journal | 99.23% | 96% |
| detail / qt / render | 100.00% | 97% |

**net is the single largest gap on both axes** — 193 of the framework's 332 line misses (58%) and the lowest branch score by 11 points — because it only entered the coverage report at all after morph#403; nobody has ever driven its miss list down. It is Phase 2, immediately after quick wins.

**Mutation, scope `core-forms` (core + forms only; net/offline/util/session/qt/journal have never been mutation-tested), from `scripts/mutation_survivors.json`:** 999 mutants, 352 survived (64.76%) as of the `lane/d-framework` run. **But** a hand-verification (`false_positive_finding` in that file) applied 3 of those "survived" mutants directly to source and rebuilt: all 3 turned the suite red (34, 2, and 2 failing cases respectively). The suspected mechanism is COMDAT/ODR folding — `cxx_remove_void_call` mutants in header-only functions included by many translation units are 148 of the 352 survivors, and `remote.hpp`+`bridge.hpp` (both header-only, both included by most of `morph_tests`' 105 TUs) account for 211 of them. **The 352 figure is not a trustworthy gap count until this mechanism is confirmed or ruled out — that is Phase 6, before any test-writing against the survivor list.**

---

## Phase 0: Re-establish the baseline

### Task 1: Build once, with every subsystem enabled, and capture both coverage artifacts

**Files:**
- Create (scratch, not committed): `build/clang-coverage/coverage.lcov`, `build/clang-coverage/coverage_objects.txt`, `/tmp/branch_report.txt` (or the scratchpad dir)

- [ ] **Step 1: Configure with every optional subsystem on** (matches CI's `clang-coverage` job exactly, so results are comparable to the numbers above):

```bash
cmake --preset clang-coverage \
  -DMORPH_BUILD_NET=ON \
  -DMORPH_BUILD_OFFLINE_SQLITE=ON \
  -DMORPH_BUILD_QT=ON \
  -DMORPH_BUILD_LADDER=ON \
  -DMORPH_LADDER_RUNGS=all
```

- [ ] **Step 2: Build**

```bash
cmake --build --preset clang-coverage
```

- [ ] **Step 3: Run the full test suite under profiling**

```bash
LLVM_PROFILE_FILE="build/clang-coverage/%p.profraw" ctest --preset clang-coverage
```

Expected: every test passes. If any fail, stop and fix or investigate before trusting coverage numbers built on a red suite — do not proceed to Step 4 with failing tests.

- [ ] **Step 4: Produce the aggregated LCOV**

```bash
bash scripts/coverage.sh
```

Expected: writes `build/clang-coverage/coverage.lcov` and prints a summary; no `ERROR:` lines.

- [ ] **Step 5: Produce the branch-coverage report and per-file partial-line backlog**

```bash
python3 scripts/check_branch_coverage.py build/clang-coverage/coverage.lcov \
  --objects build/clang-coverage/coverage_objects.txt | tee /tmp/branch_report.txt
```

Expected: a table matching the shape of the "Branch coverage" table above, plus a "partial lines by file" section. Compare the per-subsystem percentages against the table in this plan — if the framework overall (line or branch) has moved by more than ~1 point in either direction since 2026-09-02/03, note it in Phase 0's commit message before proceeding; the plan's phase ordering assumes net is still the largest gap, and a large unexpected shift means re-checking that assumption first.

- [ ] **Step 6: Do not commit build artifacts.** This task produces no commit — it only establishes the working tree's coverage data, re-used by every phase below until code changes force a re-run (Step 2–5 only, not Step 1, unless a new subsystem is touched).

---

## Phase 1: Quick wins — util, session, qt, journal

These four subsystems are already 92–100% (branch) / 97–100% (line) and together account for only 9 line misses + 22 partials. Closing them first is cheap and derisks the harder phases' tooling (confirms the audit → test → re-measure loop works before spending it on net).

### Task 2: Audit util/session/qt/journal's exact misses and partials

- [ ] **Step 1: List every zero-hit line (`DA:<line>,0`) under these four subsystems**, from the aggregated LCOV produced in Phase 0:

```bash
awk '
  /^SF:/ { file=$0; sub(/^SF:/, "", file); active = (file ~ /include\/morph\/(util|session|qt|journal)\//) }
  active && /^DA:/ { split($0, a, ","); if (a[2] == "0") print file ":" a[1] }
' build/clang-coverage/coverage.lcov
```

- [ ] **Step 2: List every partial branch line** (an `SF:` under these four dirs, with at least one `BRDA:` arm whose taken-count field is `-` or `0`) the same way, or read them off `/tmp/branch_report.txt`'s per-file counts and cross-reference with `llvm-cov show --show-branches=count` for the specific files named.

- [ ] **Step 3: For each finding, read the surrounding code and classify it as one of:**
  - **(a) genuinely untested** — write a test (Step 4 below).
  - **(b) unreachable by construction** — document it: add an entry to `scripts/branch_partial_allowlist.json` (for a partial branch) with a `source`-keyed reason, or a comment in the relevant test file / a short note in this plan's Phase 1 commit explaining why (for a fully-missed line, there's no allowlist mechanism today — follow the precedent in `codecov.yml`'s per-rung comments: name the line, the reason, and what would have to be true for it to become reachable).

### Task 3: Close the genuine gaps found in Task 2

**Files:**
- Modify: the existing per-subsystem test files under `tests/` that already cover the file in question (e.g. a util gap in `include/morph/util/foo.hpp` belongs in whatever `tests/test_*.cpp` already exercises `foo.hpp` — check `grep -rl 'util/foo.hpp' tests/*.cpp` before creating a new file). Only create a new test file if none exists for that header.

For each finding classified "(a) genuinely untested" in Task 2:

- [ ] **Step 1: Write a Catch2 case that exercises the specific uncovered line/branch**, naming the file:line range in the case name or a leading comment (matching `test_coverage_push95.cpp`'s convention: "Coverage-gap tests... Each case names the file:line range it exercises").
- [ ] **Step 2: Build and run just the affected suite** to confirm the new case passes and is not accidentally a no-op:

```bash
cmake --build --preset clang-coverage --target <suite_target>
ctest --preset clang-coverage -R <suite_name>
```

- [ ] **Step 3: Re-run Phase 0 Steps 3–5** (or, cheaper, `llvm-cov show` scoped to just the changed file) to confirm the specific line/branch now shows a hit / all arms taken.
- [ ] **Step 4: Commit** — one commit per file or tightly-related cluster:

```bash
git add tests/<file> [scripts/branch_partial_allowlist.json]
git commit -m "$(cat <<'EOF'
<subsystem>: close the <N> untested <lines/branches> in <file>

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01VpBVRFQwCRBuyMoCbJre37
EOF
)"
```

### Task 4: Re-measure and confirm the four subsystems are at their honest ceiling

- [ ] Re-run Phase 0 Steps 2–5 in full.
- [ ] Confirm util/session/qt/journal now read 100% line, or that every remaining miss has a documented reason (Task 2 Step 3b). There should be zero *undocumented* misses left in these four subsystems when this task ends.

---

### Task 4b: Fix #437 (SocketServer macOS/BSD teardown hang) before net measurement

**Added after Phase 0/1 execution, not part of the original plan text.** Task 1's baseline
build (macOS, no Linux CI access on this machine) found that `morph::net::SocketServer`'s
destructor hangs forever on macOS/BSD: `SocketServer::close()`
(`include/morph/net/socket_server.hpp:84-114`) calls `_listenSocket.shutdownBoth()` —
`::shutdown(fd, SHUT_RDWR)` — specifically to unblock a thread parked in a blocking
`accept()`, then joins that thread. This works on Linux (confirmed via the code's own
comment and this repo's CI history) but `shutdown(2)` on a **listening** (unconnected)
socket is a documented no-op for waking a peer thread blocked in `accept()` on macOS/BSD
kernels — the accept-loop thread never wakes, the join never returns, and the destructor
hangs. Filed as issue #437 (full diagnosis, `sample(1)` stack traces, and scope — 21
affected tests across `tests/net/test_socket_backend.cpp`, `tests/net/test_socket_server.cpp`,
and one interop test — are in the issue). Confirmed unaffected: `morph::qt::QtWebSocketBackend`/
`QtWebSocketServer` (a separate, Qt-event-loop-driven implementation).

This blocks running `tests/net`'s full suite locally on macOS at all, which in turn blocks
getting a trustworthy local coverage baseline for Phase 2 — every test that constructs a
`SocketServer` and lets it go out of scope hits the hang. The user chose to fix this now
(rather than work around it or defer net) so Phase 2 can measure and iterate normally on
this machine.

**Files:**
- Modify: `include/morph/net/socket_server.hpp` (the `close()`/destructor teardown path)
- Modify (if the fix needs it): `include/morph/net/detail/tcp_socket.hpp` (`shutdownBoth()`
  and/or a new `close()`-the-fd primitive, if one doesn't already exist there)
- Test: a new or extended test in `tests/net/test_socket_server.cpp` that reproduces the
  hang scenario with a bounded timeout (e.g. a test-level watchdog/timeout around
  destructing a `SocketServer` whose accept-loop thread is parked in `accept()`) — the
  regression test must actually fail (timeout/hang) against the pre-fix code and pass
  against the fix; a test that merely constructs-and-destroys without ever having a
  parked accept-loop thread would not exercise the bug at all.

**Suggested fix shape** (from the issue's own recommendation — the implementer should
verify and adjust as needed, this is a starting point, not a mandate): `close(2)` the
listening file descriptor itself (not merely `shutdown(2)` it) before or as part of the
teardown, ahead of/instead of relying on `shutdown` alone to unblock `accept()`. Closing a
fd that a thread is blocked in `accept()` on reliably unblocks it with `EBADF` on both
Linux and macOS/BSD (unlike `shutdown`, which is Linux-reliable but BSD-unreliable for this
specific case). The fix must stay correct on Linux (this repo's CI platform) — do not trade
a macOS fix for a Linux regression; if unsure, reason through (or research) both platforms'
`close()`-during-blocking-`accept()` semantics before committing to an approach, and note
in the report what was verified vs. assumed for each platform.

- [ ] **Step 1: Write a regression test** in `tests/net/test_socket_server.cpp` that starts
  a `SocketServer` listening, lets its accept-loop thread block in `accept()`, then
  destructs the server with a bounded wall-clock expectation (the test framework/CI
  environment's own timeout is not a substitute — the test itself should demonstrate the
  hang was real, e.g. by checking destruction completes within a short deadline like a
  few seconds, not relying on ctest's 120s timeout to eventually kill a hung process).
- [ ] **Step 2: Confirm the test reproduces the hang** against the current (unfixed) code —
  run it standalone with a short timeout and confirm it fails/hangs as expected. Do not
  skip this step; a "regression test" that was never seen to fail against the bug it
  claims to catch is not verified.
- [ ] **Step 3: Implement the fix** in `socket_server.hpp` (and `tcp_socket.hpp` if needed).
- [ ] **Step 4: Run the new test** — confirm it now passes, and completes promptly (no
  hang).
- [ ] **Step 5: Run the full previously-excluded set** to confirm the fix generalizes
  beyond the one regression test:

```bash
LLVM_PROFILE_FILE="build/clang-coverage/%p.profraw" ctest --preset clang-coverage \
  -R "^(SocketServer:|SocketBackend:|QtWebSocketBackend interop: connects to a SocketServer)"
```

Expected: all 21 previously-excluded tests now pass (not "excluded" — actually run and
green), with no hang.

- [ ] **Step 6: Run the complete suite** (no exclusion needed anymore) to confirm nothing
  else regressed:

```bash
LLVM_PROFILE_FILE="build/clang-coverage/%p.profraw" ctest --preset clang-coverage
```

- [ ] **Step 7: Commit**, referencing #437.
- [ ] **Step 8: Close or comment on #437** noting the fix (commit hash) once merged —
  or leave it to whoever finishes the branch; at minimum the task report should state
  the fix is ready to close the issue.

This task's own diff should be small and tightly scoped to the teardown mechanism — it is
a bug fix, not a refactor, and should not touch unrelated parts of `socket_server.hpp`/
`tcp_socket.hpp`.

---

## Phase 2: net — the largest gap

net is 77.61% lines / 75.15% branches over 1,949 header lines across `socket_server.hpp`, `socket_backend.hpp`, and `detail/{ws_handshake,sha1,tcp_socket,base64,ws_frame}.hpp`. It has its own test directory (`tests/net/`, one file per header) — gaps belong in the matching existing file, not a new "coverage gap" file, unless a specific header's gaps are numerous enough to warrant grouping (judgment call at Task 5 time; if a single header has >15 genuine gaps, a companion `test_<header>_coverage_gaps.cpp` following the `test_coverage_push95.cpp` convention is reasonable).

### Task 5: Audit net's exact misses and partials, per file

- [ ] **Step 1:** Same `awk` extraction as Phase 1 Task 2 Step 1, scoped to `include/morph/net/`.
- [ ] **Step 2:** Group the output by file (`socket_server.hpp`, `socket_backend.hpp`, `detail/ws_handshake.hpp`, `detail/sha1.hpp`, `detail/tcp_socket.hpp`, `detail/base64.hpp`, `detail/ws_frame.hpp`) and get a rough count per file — this tells you which of the 7 files to tackle first (biggest count = biggest win) and roughly how many sub-tasks Task 6 needs.
- [ ] **Step 3:** For each miss, read the code and classify per Phase 1 Task 2 Step 3's (a)/(b) split. Pay particular attention to error paths — `socket_backend.hpp` and `tcp_socket.hpp` are the kind of code where "the read failed" / "the peer reset the connection" / "the handshake was malformed" branches are exactly the ones nobody bothers driving from a test, and exactly the ones most worth having (these are also the shapes mutation testing in Phase 7 will care about later — closing them now pays twice).
- [ ] **Step 4:** Check whether any net-side integration helpers already exist for fault injection (`grep -rl "FaultProxy\|fault_proxy" tests/net examples/common/testkit`) — `examples/common/testkit/fault_proxy.hpp` exists for the ladder; check whether it (or something like it) is reachable from `tests/net`, since socket-level error-path testing usually needs a way to inject a short read / reset / malformed frame rather than relying on real OS-level races.

### Task 6: Close net's genuine gaps, file by file, in order of miss count

For each of the 7 net headers, repeat the Task 3 loop (write failing-then-passing Catch2 cases in `tests/net/test_<name>.cpp`, build+run that suite, re-measure with `llvm-cov show` scoped to the file, commit per file). Do not batch all 7 files into one commit — the repo's own convention and this plan's "file what you find" constraint both favor small, reviewable, per-file commits, and net is large enough that a single 193-miss commit would be unreviewable.

Sub-tasks, ordered by Task 5 Step 2's ranking (fill in the actual order once Step 2 runs — do not assume `socket_backend.hpp` is worst without checking, though at 665 lines it's the largest file in the subsystem and a reasonable prior):

- [ ] **6a:** `socket_backend.hpp`
- [ ] **6b:** `socket_server.hpp`
- [ ] **6c:** `detail/tcp_socket.hpp`
- [ ] **6d:** `detail/ws_handshake.hpp`
- [ ] **6e:** `detail/ws_frame.hpp`
- [ ] **6f:** `detail/sha1.hpp`
- [ ] **6g:** `detail/base64.hpp`

### Task 7: Re-measure net and confirm the new ceiling

- [ ] Re-run Phase 0 Steps 2–5.
- [ ] Compute net's new measured line% and branch%, list every remaining documented-unreachable line, and stop here for net — do not force it to 100% by weakening a genuine unreachability guard (e.g. don't delete a defensive check just to make its `else` branch reachable; that would be optimizing the metric against the code's own safety, which is exactly what `AGENTS.md`'s "verify rather than assert" section warns against doing to *any* measurement in this repo).

---

## Phase 3: forms

94.68% lines / 92.36% branches, 1,560 lines, 58 misses + 25 partials. Two existing coverage-gap companion files already exist for this pattern (`test_coverage_gaps.cpp`, `test_coverage_push95.cpp` both include `<morph/forms/...>` headers) — check whether new forms gaps belong there or in the forms-specific suites (`tests/test_forms_*.cpp`, of which there are at least 8) before creating anything new.

### Task 8: Audit forms's exact misses and partials

Same procedure as Phase 1 Task 2 / Phase 2 Task 5, scoped to `include/morph/forms/`. `forms.hpp` and `flows.hpp` are the two files mutation testing (baseline, `runs[1]`) already flagged as carrying the most survivors (23 and 13 respectively) — cross-reference the line-coverage misses in these two files against the mutation survivor list now (`scripts/mutation_survivors.json`'s `survivors_by_file`) so Phase 3's coverage work and Phase 7's mutation work aren't duplicating investigation of the same lines later.

### Task 9: Close forms's genuine gaps

Same TDD loop as Task 3/6, grouped by file, one commit per file or tight cluster.

### Task 10: Re-measure forms

Same as Task 7, for forms.

---

## Phase 4: offline

91.94% lines / 86.25% branches, 633 lines, 36 misses + 15 partials.

### Task 11: Audit offline's exact misses and partials

Same procedure, scoped to `include/morph/offline/`. Existing suites: `tests/test_offline_queue.cpp`, `tests/test_offline_integration.cpp`, `tests/test_file_offline_queue.cpp`, `tests/offline_sqlite/`.

### Task 12: Close offline's genuine gaps

Same TDD loop.

### Task 13: Re-measure offline

Same as Task 7.

---

## Phase 5: core

97.35% lines / 93.22% branches, 3,249 lines, 35 misses + **51 partials** — the highest partial-branch count of any subsystem, meaning the residual gap here is disproportionately about untaken *branches* on lines that do run, not about dead code. `remote.hpp` and `bridge.hpp` are core's two largest files and (per the mutation baseline) also carry 126 and 85 mutation survivors respectively — the same cross-reference note as Phase 3 applies here even more strongly.

### Task 14: Audit core's exact misses and partials

Same procedure, scoped to `include/morph/core/`. Given 51 partials on 35-misses-worth of files, expect most of Task 14's findings to be partial branches (an `if` whose one arm never ran) rather than fully-dead lines — read `/tmp/branch_report.txt`'s core row and the per-file partial-line counts first to size this before diving into individual files.

### Task 15: Close core's genuine gaps

Same TDD loop, one commit per file. Given core's size, expect this to be the largest Phase in the coverage half of this plan — consider whether Task 15 itself should fan out into per-file sub-tasks the way Phase 2's Task 6 does, once Task 14's per-file counts are in.

### Task 16: Re-measure core, and the framework as a whole

- [ ] Re-run Phase 0 Steps 2–5.
- [ ] At this point every subsystem has been audited once. Produce a final framework-wide line/branch table in the same shape as this plan's baseline table, and compare against it — this is the number Phase 9 writes back into `codecov.yml` and `scripts/check_branch_coverage.py`'s `FLOORS`.

---

## Phase 6: Verify the mutation false-positive mechanism before trusting any survivor as a gap

**Do not start Phase 7 before this phase concludes.** Writing tests against 352 survivors when 3-for-3 hand-checks showed the tool was wrong would waste effort and could pollute the suite with tests asserting on behavior that was never actually broken to begin with.

### Task 17: Reproduce the COMDAT/ODR-folding hypothesis in isolation

The `false_positive_finding` entry in `scripts/mutation_survivors.json` states the mechanism is "inferred from the shape of the result, not verified": affected sites are header-only functions included by many translation units, where each TU gets its own copy and the linker folds them, so Mull's per-object mutant embedding may not run against the copy the test binary actually calls.

- [ ] **Step 1:** Pick one confirmed-false-positive site from the `false_positive_finding` results (e.g. `include/morph/core/remote.hpp:718`, the `reply(wire::encode(makeOk(...)))` call already hand-verified to fail 34 cases when removed).
- [ ] **Step 2:** Check how many translation units in `morph_tests` actually include `remote.hpp` and how many object files therefore carry a copy of the enclosing function:

```bash
grep -rl "morph/core/remote.hpp" tests/*.cpp | wc -l
nm -C build/clang-coverage/tests/CMakeFiles/morph_tests.dir/*.cpp.o 2>/dev/null \
  | grep -c "<mangled-name-of-the-enclosing-function>"
```

(fill in the actual mangled name from `nm -C` on one object file first — this step is exploratory, not a fixed recipe.)

- [ ] **Step 3:** Build a minimal single-TU reproduction: one `.cpp` that includes only `remote.hpp` and the one test file that exercises the site, mutation-test *that* in isolation (a scoped Mull config naming only that TU's object), and check whether the mutant is killed there when it "survived" in the full 105-TU build. If it's killed in isolation but survived in the full build, the ODR/COMDAT hypothesis is confirmed. If it still survives in isolation, the mechanism is something else — stop and investigate what, rather than proceeding on an unconfirmed guess (this is exactly the "ask whether the check would still pass if the feature did nothing" discipline `AGENTS.md` asks for, applied to the checker itself).
- [ ] **Step 4:** Record the result — confirmed or refuted, with the actual command output — as a new entry appended to `scripts/mutation_survivors.json`'s `false_positive_finding`, following its existing structure (`method`, `results`, `consequences`, `mechanism`).
- [ ] **Step 5: Commit.**

### Task 18: Decide the mutation methodology going forward, based on Task 17's answer

- [ ] **If confirmed (COMDAT folding):** either (a) find or write a Mull config option / build flag that disables the folding for the instrumented build (e.g. forcing `-fno-icf`-equivalent or per-TU internal linkage for instrumentation purposes), and re-measure `core-forms` with it, or (b) if no such fix exists, adopt a standing rule: **before treating any `cxx_remove_void_call` survivor in a header included by more than N test TUs as real, hand-verify it first** (delete the call, rebuild, run the suite, confirm red) — same as the 3 samples already done. Document whichever choice is made in `scripts/mutation.sh`'s own header comment, next to the existing "first score" section.
- [ ] **If refuted:** the 352 survivors are closer to a real number and Phase 7 can proceed against the list mostly as-is, still spot-checking a handful before mass-writing tests.

---

## Phase 7: Triage and close real mutation survivors (core + forms)

### Task 19: Triage the survivor list under whichever methodology Task 18 settled on

For each file in `survivors_by_file` (remote.hpp 126, bridge.hpp 85, forms.hpp 23, registry.hpp 16, completion.hpp 14, backend.hpp 14, flows.hpp 13, wire.hpp 8, payload_schema.hpp 8, executor.hpp 8, strand.hpp 7, views.hpp 6, instance_constraints.hpp 5, timeout_scheduler.hpp 4, observability.hpp 4, model.hpp 4, logger.hpp 3, model_key.hpp 2, callback_scope.hpp 2):

- [ ] Re-run `MULL_PREFIX=... bash scripts/mutation.sh core-forms` to get a current survivor list (baseline may have moved since the `lane/d-framework` run, and Phase 3/5's coverage work may have already killed some).
- [ ] For each survivor, apply Task 18's verified methodology to classify as: **equivalent** (add to `scripts/mutation_survivors.json`'s `equivalent` class with a reason, following the existing 7-entry precedent — e.g. more `reserve()`/hash-mixing sites), **side channel** (logging/metrics nothing asserts on — add to the existing `side_channel_logging`/`side_channel_metrics` classes), **measurement artifact** (per Task 18 — do not write a test, note it and move on), or **real gap** (write a test).

### Task 20: Close real gaps, file by file, in descending survivor-count order

Same TDD loop as the coverage phases, but the "test" here specifically has to assert on the *result* the mutant changed, not merely execute the mutated line (a coverage-only test would already have been counted as a "hit" line and still left the mutant surviving — that's the whole point of mutation testing). For each real gap:

- [ ] Write/extend a Catch2 case asserting on the specific behavior the mutant broke.
- [ ] Re-run `scripts/mutation.sh core-forms` scoped to just that file if Mull supports a narrower re-run, or accept the full ~45-minute re-run at natural checkpoints (e.g. after each file, not after each individual mutant) — checking after every single mutant is not worth 45 minutes each time.
- [ ] Commit per file, same convention as the coverage phases.

Sub-tasks, ordered by survivor count (highest first, re-confirmed by Task 19's fresh run):

- [ ] **20a:** `remote.hpp`
- [ ] **20b:** `bridge.hpp`
- [ ] **20c:** `forms.hpp`
- [ ] **20d:** `registry.hpp`
- [ ] **20e:** `completion.hpp`
- [ ] **20f:** `backend.hpp`
- [ ] **20g:** `flows.hpp`
- [ ] **20h:** remaining files (wire.hpp, payload_schema.hpp, executor.hpp, strand.hpp, views.hpp, instance_constraints.hpp, timeout_scheduler.hpp, observability.hpp, model.hpp, logger.hpp, model_key.hpp, callback_scope.hpp) — group by mutator type if that's faster than by file once the pattern in 20a–20g is established (e.g. all remaining `cxx_gt_to_ge` boundary survivors across files, in one pass).

### Task 21: Re-run `core-forms` mutation once more and record the final score

- [ ] `MULL_PREFIX=... bash scripts/mutation.sh core-forms`, append the result as a new `runs[]` entry in `scripts/mutation_survivors.json` following the existing format exactly (date, scope, driven_by, tool, compiler, mutators, mutants, killed, survived, mutation_score_percent, wall_time, movement_against_baseline, survivors_by_file, survivors_by_mutator).
- [ ] Commit.

---

## Phase 8: Extend mutation coverage to net (and consider offline/util/session)

`scripts/mutation.sh` already supports a `net` scope; it has never been run. Given net is also the weakest subsystem on line/branch coverage (Phase 2), do this *after* Phase 2 closes net's coverage gaps — mutation-testing under-covered code mostly reproduces the same findings coverage already surfaced, at 45 minutes a run.

### Task 22: Run and triage the `net` mutation scope

- [ ] `MULL_PREFIX=... bash scripts/mutation.sh net`.
- [ ] Apply Task 18's verified methodology (net has the same header-only-many-TU shape as core/forms if `socket_backend.hpp` etc. are included broadly — check before assuming the mechanism transfers).
- [ ] Triage and close per Task 19/20's loop.
- [ ] Record the run in `scripts/mutation_survivors.json`.

### Task 23 (optional, scope-permitting): Extend `scripts/mutation.sh` to cover offline/util/session/journal/qt/detail/render

These are small enough (offline 633 lines, the rest under 400 each) that a combined scope might be cheap relative to `core-forms`'s 45 minutes. This task is explicitly lower priority than Phases 1–7 — only take it on if time remains, and treat it as "extend the script's scope options, then repeat the Task 19/20 loop," not as a requirement of this plan's core goal.

---

## Phase 9: Write the new ceiling back into the repo's own bookkeeping

The whole point of `codecov.yml`'s `framework` component and `check_branch_coverage.py`'s `FLOORS` table is that they're supposed to track reality, not aspiration — and both already say the current numbers are provisional. This phase closes that out.

### Task 24: Update `codecov.yml`'s `framework` component

- [ ] Using Phase 0/16's final measured numbers, rewrite the `framework` component's comment block (the "Where 93% comes from" section) with the new measured ceiling, the new per-subsystem table, and a new `target:` a small margin below it — following the exact style already used for the `crm`/`ledger`/`lims` components (measured ceiling stated, margin explained, `informational: true` kept unless there's a separate decision to make it blocking, which is the repo owner's call per the file's own framing, not this plan's).
- [ ] Commit, following the precedent of `eec3d44d` ("codecov: record crm's re-measured figures").

### Task 25: Update `scripts/check_branch_coverage.py`'s `FLOORS` table

- [ ] Update each subsystem's `(floor, measured)` tuple to the new numbers from Phase 0/16, keeping the same "measured minus ~3 points" margin logic the file's own docstring explains (or minus ~1 point for subsystems the docstring says are already at a real 100% floor).
- [ ] Update the docstring's "Measured on" date and configure-flags note.
- [ ] Run the script's self-test to confirm nothing broke:

```bash
python3 scripts/check_branch_coverage.py --self-test
```

- [ ] Commit.

### Task 26: Final full verification pass

- [ ] Re-run Phase 0 Steps 2–5 one more time on a clean tree (after all commits from Phases 1–9 land) to confirm the numbers in `codecov.yml` and `FLOORS` match what a fresh measurement actually produces — not what was true partway through the work.
- [ ] Run the full test suite once more (`ctest --preset clang-coverage`) to confirm every new test added across this entire plan still passes together, not just in isolation per-phase.
- [ ] Run `scripts/check_branch_coverage.py`'s non-self-test mode against the fresh LCOV and confirm no subsystem is *below* the newly-written floor (it should sit at or just above it, by construction, but confirm — a floor written from a number that doesn't reproduce is worse than no floor).

---

## Self-Review Notes

- **Spec coverage:** every subsystem in the baseline table (Phase 1: util/session/qt/journal; Phase 2: net; Phase 3: forms; Phase 4: offline; Phase 5: core) has a phase. Mutation coverage has its own arc (Phase 6 methodology fix → Phase 7 core-forms → Phase 8 net-and-beyond). Bookkeeping (Phase 9) closes the loop back into the files that make the new ceiling visible to future PRs, matching this repo's own stated practice that a number nobody looks at is a number that drifts back down (`codecov.yml`'s bookmarks/polls/kanban history is the cited example of exactly that).
- **Placeholder scan:** every audit task names the exact command; every "close the gap" task specifies the TDD loop, the file convention to follow, and the commit message format. What's *not* specified — the exact test code for each of the ~330+ individual coverage gaps and ~350 mutation survivors — is not a placeholder in the sense the writing-plans skill warns against; it's genuinely unknowable before the audit tasks run, exactly as it was for every "audit the rung's uncovered lines" commit already in this repo's history. Each audit task's output is itself the missing specification for its paired close-the-gap task.
- **Known risk not hidden in a step:** Phase 5 (core) and Phase 7 (mutation on core+forms) are the largest, and Phase 6 is a genuine open investigation with an uncertain outcome — Task 17/18 could go either way. This plan does not pretend otherwise; Task 18 branches explicitly on both outcomes.
