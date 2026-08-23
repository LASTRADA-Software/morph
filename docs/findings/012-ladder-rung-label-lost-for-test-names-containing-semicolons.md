---
id: 012
title: A ladder test whose name contains a semicolon silently never gets its `ladder-<rung>` label, so per-rung ctest selection under-selects
subsystem: core
severity: minor
source: lims rung 6 (found while reconciling `ctest -N` against the test binary's own case count)
disposition: open
test: spec-cited (measured below; reproduces on a pre-existing ledger test)
---

`morph_add_rung()` labels a rung's ctest cases in two steps
(`cmake/morph_add_rung.cmake`): `catch_discover_tests(... PROPERTIES LABELS
ladder ...)` first, then a `file(GENERATE)`d script that re-labels each case
with `"ladder;ladder-<rung>"` (and adds `journey` for journey cases). The
second step iterates:

```cmake
foreach(_ladder_test IN LISTS ladder_${_rung}_tests_TESTS)
    ...
    set_tests_properties("${_ladder_test}" PROPERTIES LABELS "ladder;ladder-${_rung}")
```

`IN LISTS` splits on `;`. A Catch2 test whose *name* contains a semicolon is
therefore split into two fragments, neither of which names a real test, so
`set_tests_properties` silently applies to nothing and the case keeps only the
`ladder` label from step one. Nothing warns: `set_tests_properties` on an
unknown name is not an error in a generated script, and the test still exists
and still runs.

## Measured

Pre-existing case, `examples/ledger/tests/test_ledger_reports.cpp:68`:

```
$ ctest -N --show-only=json-v1   # then, per test:
'SubmitReport returns immediately; GetReportStatus transitions Pending to Done'
  LABELS -> ['ladder']
```

Every sibling ledger case carries `['ladder', 'ladder-ledger']`.

The same was true of two lims cases before they were renamed, which is how
this was found: `ctest -L ladder-lims -N` reported 85 tests while the binary's
own `--list-tests` reported 87. After renaming them the two counts agree.

`grep -rn 'TEST_CASE("[^"]*;' examples/ tests/` finds 14 such names repo-wide;
12 are pre-existing, in `ledger` (1), `bookmarks` (1), `kanban` (4),
`concepts` (1), and the framework's own `tests/` (4). The framework's own
`morph_tests` target is **unaffected** — it uses `catch_discover_tests`
directly with no `file(GENERATE)` re-labelling step, and its semicolon-named
cases register normally.

## Impact, stated precisely

Not "the tests do not run". `.github/workflows/ci.yml`'s `ladder-tests` and
`ladder-sanitizers` jobs both select with `-L ladder`, which these cases do
carry, so they run there today.

What breaks is **per-rung selection**, which is what the label exists for:

- `ctest -L ladder-<rung>` silently omits them. The kanban ThreadSanitizer job
  (`ctest --preset clang-tsan -L ladder-kanban -R ThreadSanitizer`) selects
  that way, and `cmake/morph_add_rung.cmake`'s own comment calls
  `ladder-<rung>` "the CI path-filter unit".
- `examples/FINDINGS.md`'s demotion policy ("Once a rung exits, it **demotes**
  in per-PR CI to compile-only plus one …") is expressed per rung, so any
  future per-rung gate inherits the same hole.
- A developer verifying their own rung with `ctest -L ladder-<rung>` gets a
  count quietly lower than the binary's, which is exactly how a test that
  measures nothing goes unnoticed.

## What should happen

Iterate the discovered list without list-splitting the names. The
straightforward fix is to have the generated script use the `TEST_LIST`
variable's bracket-escaped form, or to set the extra label additively via
`set_property(TEST ... APPEND PROPERTY LABELS ...)` inside
`catch_discover_tests`'s own `PROPERTIES` (which already carries `ladder`
correctly for these names — step one has no such problem, so whatever it does
with the name is the shape step two should copy).

A cheap belt-and-braces addition, independent of the fix: have the generated
script `message(FATAL_ERROR ...)` when a name it is about to label does not
exist as a test, so a future mismatch is loud rather than silent.
