# Handoff: batch/519-520 (morph#518 sprint, R core-async lane)

Status as of this commit: **the two committed fixes are done and verified. The
uncommitted "simplify" pass on top of them introduced a real regression and is
NOT safe to merge as-is.** This file plus this commit exist so a fresh session
can pick this up without re-deriving the investigation below.

## What's done and verified (committed, in this branch's history)

- **`b9244703` — fixes #519** (`RemoteServer` deadlock). `ExecuteOrderGate::takeAndPost`
  makes ticket-take and pool-enqueue atomic per model. New regression test in
  `tests/test_remote_execute_ordering.cpp` ("two concurrent handle() callers on
  one modelId with a pool of one do not deadlock") — verified it fails
  (times out) against the pre-fix code and passes against the fix.
- **`6da1dc1d` — fixes #520** (`CompletionState<T>` moved-from value).
  `setValue` now copies from its parameter before mutating any state, with
  the strong exception guarantee documented in the diff. New regression test
  in `tests/test_completion_multi_handler.cpp` — verified it fails (empty
  string) against the pre-fix code and passes against the fix.
- Both individually gated with `/code-review medium --fix` before committing
  (see PR discussion / this session's transcript for the two findings that
  were caught and fixed: a cross-model self-deadlock risk in an earlier
  `_enqueueMtx` design, fixed by moving the lock into the per-model `Gate`;
  and an exception-safety hole in the `setValue` copy, fixed by reordering
  the copy before any state mutation).
- Draft PR open: **https://github.com/LASTRADA-Software/morph/pull/546**
- Full `morph_tests` suite green at this commit (`6da1dc1d`): 1439 test cases,
  1438 passed + 1 pre-existing, unrelated `[!shouldfail]` negative-control
  test (`tests/test_replay_ledger.cpp`), 22087/22088 assertions.

## What's uncommitted and BROKEN (working tree right now)

A whole-branch `/simplify` pass (per `lib/team-protocol.md` §*Review gates*)
made three changes on top of the two commits above:

1. **`include/morph/core/detail/execute_order_gate.hpp`** — factored the
   "look up or create the `Gate` for a `ModelId`" logic (duplicated between
   `take()` and `takeAndPost()`) into a private `getOrCreateGateLocked(mid)`
   helper, used by both.
2. **`include/morph/core/remote.hpp`** — collapsed `handleImpl`'s two
   near-identical `_pool.post([self, msg=..., reply=..., cid, ticket](){...})`
   bodies (one for the `executeMid` branch, one for the no-ticket branch) into
   a single `doPost` closure that captures `msg`/`reply` by value once and is
   called exactly once, from whichever branch applies.
3. **`include/morph/core/completion.hpp`** — tightened a doc comment's wording
   about the exception guarantee (no logic change).

**Symptom**: with these three changes applied, `tests/test_server_limits.cpp`'s
`BENCHMARK("RemoteServer round-trip (echo, 5 bytes)")` (tag `[!benchmark]`,
hidden by default — only surfaces when a tag filter like `[remote]` explicitly
matches it) hangs. Confirmed via `sample`: the main thread is blocked in
`~ThreadPoolExecutor()` (its destructor waits for in-flight work to drain) and
a pool worker thread is parked forever in
`ExecuteOrderGate::awaitTurn` <- `ExecuteTicketGuard::awaitTurn()` <-
`dispatchExecute` <- `dispatchMessage` <- the posted task built by the new
`doPost` closure — i.e. some ticket's predecessor never released.

**What's been ruled out:**
- Not present in the state right after `6da1dc1d` (before the simplify pass):
  ran the identical benchmark against that exact commit and it passed cleanly
  (1439/1439 relevant assertions, ~580us/iteration). So the hang is real and
  was introduced by one of the three changes above, not a pre-existing rare
  race in the `takeAndPost` fix itself — though see the caveat below.
- Read `takeAndPost`'s body before/after the `getOrCreateGateLocked` refactor
  side by side: the effective locking sequence is identical (`_mtx` briefly
  for get-or-create, copy the `shared_ptr<Gate>` out, then `enqueueMtx` held
  across `postFn`, then `_mtx` briefly again for the ticket increment). No
  semantic difference found by inspection.
- Read `handleImpl`'s `doPost` restructuring side by side with the original
  two-branch version: capture lifetimes, move-once semantics, and the
  synchronous-exactly-once call contract all appeared equivalent by
  inspection. No semantic difference found either.

**Caveat / what to check next:** the isolation test (pre-simplify passes,
post-simplify hangs) was each run **once**. `BENCHMARK` blocks in this test
run many repeated iterations internally (Catch2 benchmarking samples), so a
*rare*, low-probability race could in principle exist in `takeAndPost` itself
(pre-simplify) and just not have triggered in that one run — the simplify
diff's timing changes (e.g. one fewer/extra lambda indirection changing
exactly when a thread gets pre-empted) could then make a pre-existing rare
race trigger reliably rather than introducing a new one. This has **not**
been ruled out yet. Next steps, in order:

1. Re-run the pre-simplify benchmark several times (or in a loop) to build
   real confidence it's actually race-free, not just lucky once.
2. If pre-simplify is confirmed solid, bisect the three simplify changes
   individually (revert just the `remote.hpp` `doPost` change first, since
   it's the largest behavioral restructuring; then just the
   `execute_order_gate.hpp` helper if needed) to find which one actually
   introduces the hang.
3. Once isolated, either fix it properly or drop that specific simplify
   change and keep the other two (they're independent).
4. Re-verify with the full suite (including an explicit `[remote]`-tagged run
   to catch hidden `[!benchmark]` tests — the default no-argument run does
   **not** exercise them, which is how this was missed initially) before
   re-attempting the whole-branch `/code-review high --fix` gate, `/rebase`,
   and marking the PR ready.

## How to resume

```sh
cd /Users/yaraslau/repo/morph-518-core-async   # this worktree
git status   # the three files above are modified, uncommitted
cmake --build --preset gcc-debug --target morph_tests -j8
# Reproduce:
./build/gcc-debug/tests/morph_tests "benchmark: in-process execute round-trip"
# Isolate (example — revert remote.hpp only):
git diff -- include/morph/core/remote.hpp   # inspect
git checkout -- include/morph/core/remote.hpp   # try without it, rebuild, retest
```

Board: https://github.com/orgs/LASTRADA-Software/projects/2 (sprint #518,
lane `R core-async`). Tracking issue comment/mirror: morph#518. This batch
covers only #519 and #520 of the lane's 11 tickets — #521-529 remain `Todo`
on the board, untouched, for a future batch.
