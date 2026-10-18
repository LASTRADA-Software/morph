# A three-rung ladder — design

The application ladder becomes three applications of clearly different size: **bookmarks** (small), **bank**
(medium) and **lims** (large, and the one that grows). Pastebin, polls, kanban, ledger and crm are removed.

## Why

The ladder was meant to climb from a small application to a large one. Measured on `master` at 003cc536 (non-test
C++ lines, `ui::screen(` calls, `*Model` classes, `execute` overloads):

| Example | Product lines | Screens | Models | Actions |
|---|---|---|---|---|
| pastebin | 1,963 | 1 | 1 | 6 |
| bookmarks | 4,276 | 2 | 4 | 11 |
| polls | 2,695 | 2 | 1 | 9 |
| kanban | 5,771 | 3 | 3 | 22 |
| ledger | 6,471 | 2 | 4 | 19 |
| lims | 5,872 | 5 | 3 | 41 |
| crm | 5,322 | 0 | 7 | 41 |
| bank (not on the ladder) | 6,688 | 6 | 19 | 73 |

From rung 4 up every application is the same size, no rung has more than six screens, and the top rung has no client.
Seven applications of one size cost seven times the maintenance of one and find what one would find. Three that
differ in size find what size finds: a small one keeps the simple path honest, a medium one exercises a real domain,
and a large one is where navigation, permissions and many screens across modules are tested.

## The ladder

| Rung | Application | Role |
|---|---|---|
| 1 | bookmarks | The small application: the shortest path through models, persistence, screens and a client. |
| 2 | bank | The medium application: many models and actions, sign-in, money, the WebAssembly client. |
| 3 | lims | The large application. New screens, modules and models are added here, so the ladder has an application whose size is what a real client faces. |

`examples/rungs.txt` lists `bookmarks`, `bank`, `lims`, in that order, and its comment on bank's absence goes.

## Bank joins

Bank keeps its own `CMakeLists.txt` and does not call `morph_add_rung`. It builds variants a rung does not (the
in-memory model library for WebAssembly, `bank_memory_lib`; the command-line client; its own screens directory),
and the WebAssembly build that serves the GitHub Pages client builds bank without the ladder.

**One add.** The root adds bank only when the ladder is off: `if(MORPH_BUILD_BANK_EXAMPLE AND NOT
MORPH_BUILD_LADDER)`. With the ladder on, the ladder's rung loop adds it, when it is selected by
`MORPH_LADDER_RUNGS` or `MORPH_BUILD_BANK_EXAMPLE` is on. Bank then configures after the root's test setup (Catch2 resolved) and still
before `morph::qt` exists, which is why its server stays gated on the `MORPH_BUILD_QT` option rather than on
`TARGET morph::qt`: link names resolve at generate time. The add-once property is checked by configuring
`linux-everything`, which turns both options on.

**Names the rung consumers key on.** The test target `bank_tests` is renamed `ladder_bank_tests`, the name every
rung's suite has, so `scripts/coverage.sh`, `scripts/check_coverage_objects.sh` and the collision gate find it
without a special case. Its cases get `TEST_PREFIX "bank."` and the label `ladder-bank`, which bank's screen
suites already carry; CTest's `-L` is a regular expression, so `-L ladder` (ladder-tests, ladder-sanitizers) selects
them all. Bank's other targets keep their names.

**Coverage.** `bank_lib`, `ladder_bank_tests` and `bank_cli` get `apply_coverage` under `AF_COVERAGE`: the
instrumentation is a link option too, so every executable linking an instrumented `bank_lib` needs it
(`ladder_bank_server` and `bank_screens` already have it). The `bank_tests` GAP entry in
`check_coverage_objects.sh` goes. `coverage.sh` then adds `examples/bank/{include,src,screens}` as it does for every
rung; the command-line and server `main`s stay out of the report without an exclusion, because neither binary is an
llvm-cov object (neither is a test). `codecov.yml`
gets a `bank` component beside `bookmarks` and `lims`.

**Rule 5.** `examples/IMPLEMENTATION.md`'s 100% model line and branch coverage applies to bank as to every rung.
No gate enforces it (every codecov component is informational), and bank has never been measured against it. The
coverage leg measures it in this change; the measured figure goes into `examples/bank/README.md`, and a shortfall is
left as a known gap there, not closed here: the ladder is a detector, and its rule is a target, not a gate.

Bank's scenario corpus (`scripts/scenario/scenarios/bank/`) already exists and stays.

## What is removed

- `examples/pastebin`, `examples/polls`, `examples/kanban`, `examples/ledger`, `examples/crm`.
- Their scenario corpora under `scripts/scenario/scenarios/`, their `RungSpec`s and port notes in
  `scripts/scenario/run_scenarios.py`, and their entries in `scripts/scenario/coverage_allowlist.json` and
  `scripts/scenario/README.md`.
- Their entries in `examples/client` (`example_apps.cpp`, `CMakeLists.txt`), `codecov.yml`, `.github/workflows/ci.yml`
  (filters, comments, the `kanban-tsan` job), `cmake/tsan.supp`, `examples/common/testkit` comments, and the root
  `CMakeLists.txt` comment that names kanban as the offline SQLite consumer.
- **The `bank-sanitizers` job.** With bank among the rungs, `ladder-sanitizers` (ASan and UBSan, `-L ladder`) builds
  and runs bank's suites, so a second sanitizer job over the same code is redundant.
- **The `kanban-tsan` job.** It is the only ThreadSanitizer run over ladder code, and none of the three remaining
  rungs has a stress suite for it to run. It is dropped, not retargeted. The library's own TSan coverage
  (`linux-sanitizers`, `morph_tests`) is unchanged. A TSan leg over a rung comes back when a rung gains a stress
  suite.

Lint fixtures that name a removed rung as sample text (`tests/lint/automoc_includes/valid/moc_app.cpp`,
`invalid/long_climb/moc_budget_presenter.cpp`, the synthetic strings in `scripts/test_check_*`) are text the checks
never resolve; they stay. Dated design records and plans under `docs/superpowers/`, and `CHANGELOG.md`, describe
past work and are not edited, except for a new changelog entry.

## The documents that change

The rule: every path or present-tense citation of a removed rung that
`grep -rnE 'examples/(pastebin|polls|kanban|ledger|crm)|\b(pastebin|polls|kanban|crm)\b'` finds, outside the
exemptions below, is rewritten, in code comments as in documents. The ones that need more than a rewording:

- `examples/LADDER.md` is rewritten around the three rungs. `examples/IMPLEMENTATION.md`, `examples/TESTING.md`,
  `examples/bank/README.md` (its "not a rung" text) and `examples/lims/README.md` (its new role) follow.
- `docs/GETTING-STARTED.md` walks through pastebin; it is rewritten around bookmarks, the new first rung.
  `README.md` links the tutorial's application; `docs/ARCHITECTURE.md` names the rungs.
- `docs/spec/` passages that cite a removed rung as a present consumer or a source of a measurement:
  `offline/offline.md`, `journal/journal.md`, `error_handling.md`, `core/wire.md`. A passage whose reasoning stands
  without the rung keeps the reasoning and drops the citation; one whose only consumer was the rung says so as a
  current fact.
- `tests/test_extension_bag_spike.cpp` cites `examples/crm/EXTENSION-BAG-SPIKE.md`; the finding's reasoning moves into
  the test's own comment.
- Pastebin's `malformed-envelope` and `broken-on-purpose` scenarios move to bookmarks: the first is the only scenario
  that pins the decode-failure reply, the second is the runner's own control that a failing scenario is reported as
  one.

## What must not be lost

The removed rungs' tests also exercise library code. Two gates read library coverage:

- **Branch floors** (`scripts/check_branch_coverage.py`, per subsystem of `include/morph`, run by `coverage.sh`) fail
  loudly if a subsystem drops below its floor.
- **Line coverage** is informational in `codecov.yml`, so a line lost with a rung drops out silently.

The CI coverage leg publishes an HTML report per run. This change compares the report of its own CI run with
master's at 003cc536 file by file, for `include/morph` and `src`, in lines and branches. Every library line or branch
that was covered and no longer is gets a library test under `tests/` in this change, before it merges; no floor may
drop, and no line may be left to the informational side.

Library tests named after a removed rung that do not include its code (`tests/test_ledger_rational_fuzz.cpp`,
`tests/test_extension_bag_spike.cpp`, the replay-ledger tests, which are about the offline replay ledger) stay.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Which applications stay | bookmarks, bank, lims | The user's choice: three sizes, with lims the one with the most screens and the most room to grow. |
| Bank's build | Its own CMake, listed in `rungs.txt`, added once | Its WebAssembly variant and command-line client do not fit `morph_add_rung`, and the Pages build needs bank without the ladder. |
| Bank's test target | Renamed `ladder_bank_tests` | Every rung consumer keys on that pattern; a rename is one line, a special case in four scripts is four places to rot. |
| The kanban TSan leg | Dropped | No remaining rung has a stress suite; a job over nothing is a control that measures nothing. |
| Removing, not archiving | Delete; history is in git | An archived rung is still read, grepped and kept compiling by whoever finds it. |
| Coverage | Compared file by file against master, every loss filled | The rungs are detectors; the library's coverage must not depend on them. |
