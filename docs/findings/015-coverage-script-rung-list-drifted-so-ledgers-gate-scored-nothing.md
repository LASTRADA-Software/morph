---
id: 015
title: scripts/coverage.sh's rung list drifted from CMake's, so ledger's carefully-measured codecov component was scoring a set of files no report contained
subsystem: core
severity: minor
source: lims rung 6, adding this rung's own coverage component
disposition: open
test: spec-cited (verifiable by inspection; fix included)
---

`scripts/coverage.sh` decides which rungs' sources reach the uploaded
coverage report:

```sh
_MORPH_LADDER_RUNGS=(pastebin bookmarks polls kanban)
```

Its own comment says this list "deliberately mirrors examples/CMakeLists.txt's
own `_morph_known_rungs` list". That list is:

```cmake
set(_morph_known_rungs pastebin bookmarks polls kanban ledger lims)
```

**ledger (rung 5) and lims (rung 6) were both missing.** Neither contributed a
line to any uploaded report.

For lims that is merely a gap — the rung had no component yet. For ledger it
is worse, because `codecov.yml` *does* carry a ledger component, with a
target derived from a genuinely careful measurement:

```yaml
    - component_id: ledger
      name: "application ladder rung 5 (examples/ledger models)"
      paths:
        - examples/ledger/src/models/**
        - examples/ledger/include/ledger/models/**
      statuses:
        - type: project
          target: 87%
```

Its comment records the per-file measurement it was derived from
(`895/1012 = 88.44%`) and even documents what measuring honestly discovered
(`rule_model.cpp` at 60.76%, two documented obligations nothing verified).
All of that work is real; none of it is being enforced, because a component
whose `paths` match no file in the report does not fail — it reports nothing.

This is the same shape of defect the script's own comment warns about two
lines above the list ("Nothing fails when a rung is forgotten -- the script
runs, the report uploads, and the figure is simply computed over a shrinking
fraction of the ladder"), reappearing one level up: the *component* was
forgotten rather than the rung.

## Fix

Included: the list now reads
`(pastebin bookmarks polls kanban ledger lims)`.

## What should happen beyond the fix

A list that must mirror another list will drift again. Two cheap options:

1. **Derive it.** The CMake list is the authority; emit it into the build tree
   (a generated `ladder_rungs.txt`, or a `--rungs` argument the CI job passes
   from CMake) and have the script read it instead of restating it.
2. **Fail loudly on an empty component.** A component whose `paths` match zero
   files in the report is almost always a mistake. Codecov does not warn, but
   the coverage job could: after `llvm-cov export`, assert that every
   `component_id` in `codecov.yml` matches at least one file in the report,
   and fail the job if one does not. That catches the general case — a
   renamed directory, a moved model, a rung that stopped building — not just
   this instance.
