# Scenario workflows — design

A second coverage axis for `scripts/scenario/`: **domain workflows**, measured
the way the protocol surface already is.

Companion to
[2026-08-30-scenario-coverage-design.md](2026-08-30-scenario-coverage-design.md),
which measures the *protocol* surface (envelope kinds and refusals). This one
measures whether the corpus exercises each rung's actual work — a user signing
in, creating something, editing it, running a report, and closing out — rather
than firing isolated one-shot calls.

## Contents

- [Why a second axis](#why-a-second-axis)
- [The baseline, measured](#the-baseline-measured)
- [What counts as a workflow](#what-counts-as-a-workflow)
- [How many, per rung](#how-many-per-rung)
- [Where the workflows come from](#where-the-workflows-come-from)
- [Layout](#layout)
- [Measurement and gating](#measurement-and-gating)
- [Known limits](#known-limits)
- [Out of scope](#out-of-scope)

## Why a second axis

The protocol axis asks *"can the corpus reach every envelope kind and every
refusal?"*. It is fully satisfiable by scenarios that never do anything a user
would recognise — one `execute` and a handful of hand-built error envelopes
covers most of it.

It says nothing about whether `CreateTask` was ever followed by `MoveTaskPosition`
against the id it returned. That sequencing is where the interesting bugs live:
strand ordering, exactly-once replay, stale reads, authorization that only
bites on the *second* call. It is also the only shape that resembles what the
software is for.

Two axes, then, and neither substitutes for the other:

| Axis | Question | Source of truth |
|---|---|---|
| Protocol | Can we reach every kind and refusal? | `remote.hpp`, `wire.hpp` |
| **Workflow** | Do we exercise each rung's real work, in sequence? | `BRIDGE_REGISTER_ACTION`, rung READMEs |

## The baseline, measured

Measured at master, by extracting every `BRIDGE_REGISTER_ACTION(Model, Action,
"Name")` and diffing against every `do <Action>` in the corpus:

| Rung | Actions exercised | Registered |
|---|---|---|
| pastebin | 3 | 6 |
| bookmarks | 3 | 17 |
| polls | **0** | 9 |
| kanban | 1 (`Login` only) | 22 |
| ledger | 1 (`Login` only) | 17 |
| **total** | **8** | **71** |

And **zero** of those eight sit in a threaded sequence: the three shipped files
are short runs of independent calls. Two rungs that ship a server have no
scenario at all.

## What counts as a workflow

"More than one call" has to be mechanical, or the gate is an opinion. A
scenario counts as a workflow when **a later step consumes a value an earlier
step captured**:

```
do CreatePaste content="draft"
expect ok capture id=$.id

do GetPaste id=$id          # <- consumes $id: this is a workflow
expect ok field content == "draft"
```

Threading state is what separates a journey from a list. A file that dispatches
ten actions with no `$capture` reference between them is ten one-shot calls
sharing a socket, and the metric must say so.

The workflow metric therefore records, per scenario file: how many distinct
actions it dispatches, and how many steps consume a captured value. A file
qualifies at **three or more distinct actions with at least two capture
consumptions** — enough to rule out `create → read` alone counting as a
journey, without demanding every file be long.

## How many, per rung

Scaled to how finite the space actually is. A rung with six actions has a
countable set of meaningful journeys; one with twenty-two does not, and the
target there is the *important* ones rather than the closure.

| Rung | Actions | Workflow floor | Rationale |
|---|---|---|---|
| pastebin | 6 | **8** | Small enough to approach exhaustive: every action in at least one journey, plus the burn/expiry/private variants |
| polls | 9 | **10** | Shared instances and anonymous principals multiply the interesting paths |
| bookmarks | 17 | **12** | Multi-entity CRUD, bulk actions, sessions |
| ledger | 17 | **15** | Money, reports, undo, budgets — long journeys by nature |
| kanban | 22 | **20** | Largest surface: RBAC, offline replay, cascades, activity |

Floors, not quotas: a rung may carry more. The point of a floor is that
deleting workflows is a deliberate act that fails CI, not a silent erosion.

Alongside the floor, **every registered action must appear in at least one
workflow**, per rung. That is the 71/71 target, and it is what stops a rung
satisfying its floor with twenty variations on the same two actions.

## Where the workflows come from

Not invented. Each rung's README already enumerates them:

- **"What to implement"** — the numbered action inventory, with the
  interesting semantics called out (pastebin's `GetPaste` "is the interesting
  one": a read that is a write).
- **"Definition of done"** — names journeys directly. Pastebin's is literally
  *"create → list → open → burn → delete"*.
- **"Expected strain points"** — the edge cases each rung was built to hit.
  These become the negative and adversarial workflows.
- **kanban's `tests/journeys/test_kanban_journeys.cpp`** — already encodes two
  journeys in C++ ("a rejected sign-in, a successful retry, and a session that
  ends at sign-out"; "open a board, add work, move it, and have a second client
  converge"). The scenario versions cover the same ground out-of-process.

Where a README names a journey, the scenario file cites it in a comment, so a
reader can check the workflow still matches what the rung claims to do.

## Layout

One file per workflow, in per-rung directories:

```
scripts/scenario/scenarios/
  pastebin/create-read-burn.scenario
  pastebin/private-paste-is-not-listed.scenario
  ledger/open-account-transact-report-close.scenario
  ...
```

The three existing files move into their rung directories. `load_scenarios`
gains recursion; the rung a scenario belongs to is its parent directory name,
which is how per-rung action coverage is attributed.

Per-rung directories are also what lets `run_all.sh` start one server, run
that rung's whole set against it, and tear it down.

## Measurement and gating

`scenario_coverage.py` grows a second report section and two more gate
conditions. Exit codes are unchanged in meaning: `0` clean, `1` a real gap,
`2` the tool is broken.

New extraction: every `BRIDGE_REGISTER_ACTION(Model, Action, "Name")` under
`examples/<rung>/`, giving the per-rung action universe.

New gates:

1. Every registered action is dispatched by at least one scenario in its rung's
   directory.
2. Each rung meets its workflow floor, counting only files that qualify as
   workflows under the definition above.

Both are subject to the same allowlist discipline the protocol axis already
uses: an action that genuinely cannot be driven from a scenario gets an entry
with a written reason, checked in both directions so it cannot outlive its
justification.

At least one such entry is already known: `ledger::RunReportJob` refuses any
principal but the report runner's, so a client scenario cannot dispatch it.
The workflow that submits a report asserts `Pending` and asserts the refusal
when a user client tries to run it — which pins the authorization boundary —
and the action itself is allowlisted with that reason.

**The same anti-vacuity discipline applies as on the protocol axis.** The
extractor carries a plausibility floor (71 actions today) and the tests drive
it against fixtures with known-wrong answers, including a fixture where a
"workflow" is really a flat list and must not qualify.

## Known limits

- **Asynchronous server work cannot be driven to completion.** The runner has
  no waits by design. Any journey whose next step depends on a server-side
  timer, poller or background worker stops at the state it can assert
  synchronously. Ledger's report runner and kanban's event poller are both in
  this category; their workflows assert the submitted state and the
  authorization boundary, not the eventual result.
- **A workflow proves sequencing, not concurrency.** Two clients converging is
  expressible (each `client` is its own socket) but their interleaving is not
  controlled. Genuine race coverage stays in the C++ suite.
- **The floors are judgement, not derivation.** They encode "roughly this many
  journeys matter here", scaled by action count and rung complexity. They are
  defensible starting points to be raised as scenarios land, not a computed
  optimum.

## Out of scope

- **Replacing the rungs' C++ journey tests.** kanban's stay; the scenario
  versions cover the same ground from outside the process, which is the point.
- **Workflow coverage for `lims` and `crm`** — neither ships a server. lims's
  missing server is filed separately.
- **Generating workflows automatically** from the action graph. The rungs'
  READMEs already say which journeys matter; a generator would produce
  syntactically valid journeys nobody cares about.
