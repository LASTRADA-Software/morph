# Followup: testkit cross-test isolation bug, newly observable in `ladder_kanban_tests`

**Status:** open finding, not yet triaged into a tracked issue. Not a
blocker for the kanban rung-4 completion plan (`2026-08-18-kanban-rung4-completion.md`)
— recorded here so it is not silently dropped.

## Symptom

Building `ladder_kanban_tests` and running the raw executable directly
(`build/clangcl-release/target/ladder_kanban_tests.exe`, invoked standalone —
**not** via `ctest`) crashes reproducibly. The crash location shifts
depending on Catch2's test-run order (Catch2 randomizes/varies ordering
across runs unless a fixed `--order` is passed), which is the signature of
state leaking across `TEST_CASE`s within one process rather than a bug in
any single test.

Reproduction:

1. Build the `ladder_kanban_tests` target.
2. Run `target/ladder_kanban_tests.exe` directly (no `ctest`, no
   `--order lex` or other determinism flag).
3. Observe a crash. Re-running shows the crash occurring at a different
   point in the test sequence from run to run.

## Why this is believed pre-existing, not a regression from this branch

The prime suspect is `examples/common/testkit/db_fixture.hpp`'s `DbFixture`:
its `ensureConnectionConfigured()` gates one-time process-wide setup
(`SqlConnection::SetDefaultConnectionString` + `MigrationManager::CreateMigrationHistory()`)
behind a function-local `static const bool once = [...]` lambda — a
process-wide singleton, initialized exactly once per process, by whichever
`TEST_CASE` happens to construct the first `DbFixture`. `MigrationManager`
itself (`::Lightweight::SqlMigration::MigrationManager::GetInstance()`) is
also process-wide by construction. Neither is reset between `TEST_CASE`s
run in the same process.

This file is untouched by this plan's commits: `git log -- examples/common/testkit/db_fixture.hpp`
shows no commit on `ladder-kanban-impl` touching it; its last modification
(`557b892`, "ladder: shared infrastructure, docs, and framework prerequisites
(rung 0)") predates this plan entirely. What this branch *did* do is roughly
double the number of `TEST_CASE`s compiled into the single `ladder_kanban_tests`
binary (new attachment/rules/GUI/offline-stack coverage across Phases 1-7),
which raises the odds that some pair of tests now collide over the same
process-wide singleton state in a way that was numerically less likely to
surface before. The bug itself is not new; the branch just made it easier to
trigger by adding enough `TEST_CASE`s to the same binary.

## Why CI is unaffected

`cmake/morph_add_rung.cmake` registers `ladder_<rung>_tests` with CTest via:

```cmake
include(Catch)
...
catch_discover_tests(ladder_${_rung}_tests
    DISCOVERY_MODE POST_BUILD
    DL_PATHS "${_qt_bin_dir}"
    PROPERTIES LABELS ladder TIMEOUT 120 RESOURCE_LOCK morph_ladder_test_db
)
```

(`cmake/morph_add_rung.cmake`, the `catch_discover_tests(ladder_${_rung}_tests ...)`
call, in the block that builds each rung's `ladder_<rung>_tests` binary.)

`catch_discover_tests` (Catch2's CMake integration module) queries the built
binary for its full list of `TEST_CASE` names and registers **one CTest test
entry per `TEST_CASE`**, each of which CTest then launches as its own,
separate OS process (passing Catch2 a name/tag filter selecting just that
one case). Because every `TEST_CASE` genuinely runs in its own fresh
process under `ctest`, the process-wide `DbFixture`/`MigrationManager`
singleton state never survives from one `TEST_CASE` to the next in CI —
each process starts clean, does its one-time init, runs its one test, and
exits. The cross-test leakage this finding describes can only manifest when
multiple `TEST_CASE`s share a process, which happens only when the raw
`.exe` is invoked directly instead of through `ctest`.

## Suggested next step

Bisect which specific `DbFixture`/`MigrationManager` process-wide state
leaks across `TEST_CASE`s when the binary is run standalone: most likely
candidates are `SqlConnection`'s default-connection-string singleton and/or
`MigrationManager::GetInstance()`'s applied-migrations bookkeeping, neither
of which `DbFixture`'s constructor/destructor resets per-test (only the
schema's own tables are dropped and recreated per `DbFixture` instance; the
migration-history/connection-registration singleton is deliberately
initialized exactly once per *process*, per its own doc comment). A fix
would need to either make that state safely re-initializable per
`TEST_CASE`, or make `DbFixture` reset whatever piece of it a later test can
observably depend on. This is testkit infrastructure shared by every rung,
not kanban-specific, so it deserves its own scoped investigation and plan
rather than a fix folded into this (or any single rung's) completion pass.

## Non-goals for this note

This document exists to hand the finding to a human for triage into a
tracked issue (GitHub issue creation is not available from this session). It
deliberately does not attempt to fix the underlying testkit bug: this plan
is scoped to kanban, not testkit infrastructure, and a subtle cross-test
global-state bug deserves its own focused investigation rather than a
rushed fix appended to an unrelated plan's final wave.
