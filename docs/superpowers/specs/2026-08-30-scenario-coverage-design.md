# Scenario coverage — design

Extend `scripts/scenario/` from three hand-written files into a **measured**
corpus: a coverage report that turns "are we covering everything?" into a
number CI can hold, the scenarios that close the gap it reports, the server
knobs that make the last of it reachable, and breadth across the rungs that
ship a server.

## Contents

- [Why "all use cases" is the wrong target](#why-all-use-cases-is-the-wrong-target)
- [The measurable universe, as it stands today](#the-measurable-universe-as-it-stands-today)
- [Architecture](#architecture)
- [Piece 1 — `scenario_coverage.py`](#piece-1--coveragepy)
- [Piece 2 — the protocol sweep](#piece-2--the-protocol-sweep)
- [Piece 3 — fixture knobs](#piece-3--fixture-knobs)
- [Piece 4 — rung breadth](#piece-4--rung-breadth)
- [CI](#ci)
- [Testing](#testing)
- [Risks](#risks)
- [Out of scope](#out-of-scope)

## Why "all use cases" is the wrong target

The runner's README already draws the line, and it is the right one:

> It does not replace the C++ tests. Model behaviour is tested in-process and
> stays there. This covers the seam those tests assume away.

Chasing every rung's every action would duplicate the in-process suite, grow
without bound, and dilute the one thing scenarios do that nothing else does:
speak the wire protocol from **outside** morph, implemented from the spec
rather than from morph's own C++, so a bug symmetric on both sides of morph's
own client/server pair is visible here and invisible in-process.

So the coverage target is the **protocol surface**, not the feature surface.
That is enumerable from `include/morph/core/remote.hpp`, and therefore
countable, reportable, and gateable.

Rung breadth still matters — a second rung exercises the same protocol against
different models, sessions and sharing modes — but it is breadth *over the same
measured surface*, not a separate coverage axis.

## The measurable universe, as it stands today

Measured at master `82c0d7bc`. These numbers are the baseline the report must
reproduce on its first run; if it does not, the extractor is wrong.

**Envelope kinds `dispatchMessage` handles: 8. Exercised: 4.**

| Exercised | Never sent by any scenario |
|---|---|
| `register`, `execute`, `deregister`, `hello` | `attach`, `assign`, `instances`, `schemas` |

All four gaps are reachable **today**: `send <kind> [field=value …]` already
builds an arbitrary envelope. Nobody has used it for these. They are also
precisely the kinds `docs/spec/core/wire.md` omits (morph#233), so scenarios
are the natural place to pin the real shape.

**Distinct server refusals: 17. Exercised: 4.**

(14 exact strings plus 3 prefix-shaped ones. This number was 20 in an earlier
draft, from a hand-prototyped extractor; the shipped `scenario_coverage.py` is
now the authoritative count, and it is smaller for two good reasons. Five
strings came from `SimulatedRemoteBackend`, which throws them *client*-side on
receiving an err — no WebSocket client can ever assert those. One was a
concatenation fragment, not a message. Both are now excluded by construction
rather than by hand. The one addition runs the other way: `envelope decode
failed: ` is thrown in `wire.hpp`, not `remote.hpp`, and the first draft missed
it entirely.)

| Status | Messages |
|---|---|
| Covered | `protocol version unsupported`, `unknown envelope kind:`, `model not found`, `unauthorized` |
| Reachable, unexercised (11) | `{register,attach,assign,instances,schemas} requires a typeId`; `register failed:`, `attach failed:`, `instances failed:`, `instances decode failed:`, `schemas request failed:`; `envelope decode failed:` |
| Needs a server knob (5) | `server busy`, `too many models`, `timeout`, `server shutting down`, `payload missing required field(s):` |
| Race-only (1) | `connection closed` |

The "needs a server knob" row is a **fixture** gap, not a runner limitation:
no ladder server calls `setLimitPolicy`, `setPayloadCompleteness`, or exposes
`beginShutdown`. Their only environment knobs are database, port and token
secret. Piece 3 closes all five.

`server shutting down` is the path morph#348 fixed; it currently has no
end-to-end coverage at all.

`connection closed` is a different matter and is **allowlisted rather than
chased**. It is emitted from the shared-instance attach path
(`remote.hpp:708`) when `noteScopeAttachLocked` finds the connection's scope
closed *concurrently* with the attach — a genuine race between a `close` on one
socket and an `attach` in flight on it. A scenario has no way to drive that
deterministically, and forcing it with a sleep would reintroduce exactly the
wall-clock flakiness this runner was built to avoid. It gets an allowlist entry
naming that reason.

## Architecture

Two new files beside the two that exist, sharing the runner's parser.

```
scripts/scenario/
  morph_scenario.py     (exists)  drives one scenario
  mutate_scenario.py    (exists)  proves assertions are load-bearing
  scenario_coverage.py  (new)     measures protocol coverage; gates CI
  run_all.sh            (new)     server lifecycle + fan-out; one entry point
  scenarios/*.scenario  (grows)
```

`scenario_coverage.py` imports `morph_scenario` and reuses `Scenario.parse`,
`Step` and `Assertion`. There is no second parser, so a change to the scenario
format cannot silently desync the runner from the thing measuring it.

Standard library only, matching the existing constraint: no build step, no
dependencies, no `pip install`.

## Piece 1 — `scenario_coverage.py`

### What it does

1. **Extract the universe** from `include/morph/core/remote.hpp`:
   - envelope kinds, from `env.kind == "…"` comparisons;
   - refusal strings, from `makeErr("…")`, `rejectAndRelease("…")`, and the
     `runtime_error("…")` throws that `dispatchMessage`'s catch turns into an
     `err` reply.

   Prefix-shaped messages (`"unknown envelope kind: "`, `"attach failed: "`)
   are recorded as prefixes and matched as such, since the runtime value
   carries a suffix the source cannot know.

2. **Extract what the corpus exercises**, by parsing every
   `scenarios/*.scenario` through the runner's own parser:
   - kinds: `client` → `hello` + `register`; `do` → `execute`;
     `deregister` → `deregister`; `send X` → `X`;
   - refusals: every `expect err` assertion whose clause compares `message`,
     matched against the enumerated set.

3. **Report** three buckets — covered, uncovered, allowlisted — and exit
   non-zero if anything is uncovered without an allowlist entry.

### The allowlist

A JSON file, `scripts/scenario/coverage_allowlist.json`, mapping each exempt
item to a **required written reason**. Modelled on
`morph::ladder::testkit::QmlSurfaceAudit::allowUnbound`, which established the
shape in this repository, and checked in **both directions**:

- an entry for an item that is now covered is a failure (the exemption has
  outlived its reason);
- an entry naming an item that no longer exists in `remote.hpp` is a failure
  (a rename left a stale exemption behind).

An allowlist that can only shrink deliberately is the difference between a
recorded decision and a swept-under-the-rug gap.

### Guarding against a report that measures nothing

This repository's most-hit failure mode is a control that reports success while
measuring nothing — an invalid coverage config, a sanitizer job with no
instrumentation, a filter over an invariant body. A regex-based extractor over
someone else's source is squarely in that family: rename `makeErr` and the
report finds zero refusals and cheerfully declares full coverage.

Two guards, both mandatory:

- **Plausibility floor.** The extractor asserts it found at least the counts
  recorded above (8 kinds, 18+ refusals) and **fails loudly** below them. A
  refactor in `remote.hpp` must break this script, not quietly satisfy it.
- **A fixture with a known-wrong answer.** `test_morph_scenario.py` gains cases
  driving `scenario_coverage.py` against a synthetic header and a synthetic scenario
  where the correct output is known, including one where an item is uncovered
  and the exit code must be non-zero.

The second guard is the one that matters: it is the answer to "would this still
pass if the feature did nothing?"

### Output

Human-readable by default; `--json` for machine consumption. The summary line
names the fraction covered per category so a reviewer sees movement without
reading the list.

## Piece 2 — the protocol sweep

Scenarios closing what the report names as reachable-but-unexercised. No runner
changes: `send` already expresses all of it.

- **`schemas`, the `requires a typeId` family, `envelope decode failed:`,
  `unknown envelope kind:` variants** — pastebin, the simplest server. A new
  `pastebin_protocol.scenario` keeps the existing `pastebin.scenario` readable
  as a use-case narrative rather than turning it into a refusal catalogue.
- **`attach`, `assign`, `instances`, and their failure messages** — polls, the
  shared-instances rung (`LADDER.md` rung 3: "Shared instances, anonymous
  principals, undo, event polling"). This is where the kinds mean something
  rather than being sent for the sake of sending them. It therefore depends on
  piece 4's polls work landing first, or arriving with it.

Every new file is subject to `mutate_scenario.py` in CI, so an assertion that
cannot fail is caught in the same run that adds it.

## Piece 3 — fixture knobs

### Limits

A shared helper in `examples/common` reads optional environment variables and
calls `RemoteServer::setLimitPolicy`. **One implementation, not five** — five
hand-written copies of the same parsing is the rule-of-three trigger
`examples/IMPLEMENTATION.md` already watches for, and the ladder has been
caught by it before.

| Variable | Maps to |
|---|---|
| `MORPH_LADDER_MAX_MODELS` | `LimitPolicy::maxLiveModels` |
| `MORPH_LADDER_MAX_INFLIGHT` | `LimitPolicy::maxInFlightExecutes` |
| `MORPH_LADDER_EXECUTE_TIMEOUT_MS` | `LimitPolicy::executeTimeout` |
| `MORPH_LADDER_REQUIRE_DECLARED_FIELDS` | `PayloadCompleteness::RequireDeclaredFields` |

Unset means unset — absent variables leave the policy exactly as it is today,
so no existing behaviour changes. Names are rung-agnostic deliberately: the
policy is a framework concept, and per-rung prefixes would multiply the helper
back into the five copies it exists to avoid.

Unlocks `too many models`, `server busy`, `timeout`, and
`payload missing required field(s):`.

### Shutdown

The same helper installs a `SIGTERM` handler calling `beginShutdown()` and then
`drainedWithin(deadline)`.

This is **not** test-only scaffolding: it is how an orchestrator drains a server
before replacing it, it is the sequence `docs/spec/core/backend.md` documents
under "Graceful shutdown", and it earns its place in the ladder servers
regardless of whether a scenario ever uses it. That it also makes the refusal
testable is the second reason, not the first.

The runner gains one directive to reach it:

```
signal TERM            # requires --server-pid
```

Gated on a new `--server-pid` flag; without it, a `signal` step is a scenario
error, so the directive cannot be used by accident in a run that has no process
to signal. `run_all.sh` supplies the pid it started.

Unlocks `server shutting down`, giving morph#348's fix end-to-end coverage from
outside the process for the first time.

## Piece 4 — rung breadth

Scenarios for the three rungs that ship a server and have none: **polls**,
**kanban**, **ledger**. Each covers that rung's own use-case narrative — what a
user actually does — plus whatever protocol surface only that rung can reach
(polls: shared instances; kanban: RBAC refusals and offline-replay payloads;
ledger: multi-step money flows).

**lims gets an issue, not a server.** Its GUI already supports remote mode —
`examples/lims/gui/main.cpp:78-80` selects `Remote{.url}` when given a server
URL — but the rung ships no `src/server/`, so that path has nothing in-tree to
connect to. Every other rung with a GUI ships the matching server. Unlike crm,
whose absent client is recorded in its own README's "What is not built", this
one is written down nowhere. That is a finding about lims, not about testing,
and per `AGENTS.md` it is filed rather than folded into this work.

crm is out of scope entirely: no client, no server, and its absence is already
a recorded decision.

## CI

One step in the existing `ladder-tests` job, which already builds the ladder
with Qt on and therefore already produces every `ladder_<rung>_server`.

`run_all.sh` per rung:

1. start the server with `PORT=0`, capturing stdout;
2. **poll** for the `listening on port N` line against a bounded deadline;
3. run that rung's scenarios;
4. run `mutate_scenario.py` over each;
5. tear the server down.

Then `scenario_coverage.py` once, over the whole corpus, as the gate.

`scripts/scenario/**` must join that job's changed-paths filter, or a
scenario-only change will not run the thing it changed.

## Testing

| What | How |
|---|---|
| `scenario_coverage.py` extraction | Fixture header with known kinds/refusals; assert the exact extracted set |
| `scenario_coverage.py` gating | Fixture where one item is uncovered; assert non-zero exit |
| Allowlist both-directions check | Fixture with a stale entry, and one with an entry for a covered item; both must fail |
| Plausibility floor | Fixture header with `makeErr` renamed away; must fail, not report full coverage |
| New scenarios | `mutate_scenario.py` in CI — an assertion that cannot fail is a surviving mutant |
| Limit knobs | The scenarios that assert `too many models` / `server busy` / `timeout` are themselves the test |
| SIGTERM path | The scenario asserting `server shutting down`, plus an assertion that an already-registered model's `deregister` still succeeds during the drain window |

## Risks

- **Startup wait reintroduces flakiness.** The README's proudest property is
  "No `sleep`, and no wall-clock waits… a scenario cannot become a source of
  flaky timing the way morph#147 once did". Server *startup* is a wall-clock
  wait. `run_all.sh` must poll for the port line against a bounded deadline and
  fail loudly on timeout — never sleep-and-hope. The scenarios themselves stay
  free of waits; only the harness waits, and only for a process to bind.
- **A regex extractor over `remote.hpp` will rot.** Mitigated by the
  plausibility floor and the known-wrong-answer fixture, not by hoping.
- **The allowlist becomes a dumping ground.** Mitigated by requiring a written
  reason and checking entries in both directions, so it can only shrink
  deliberately.
- **CI time.** Five servers × start/stop × mutation runs is not free. If it
  becomes a problem, the mutation pass is the part to make conditional (e.g.
  only on changed scenario files), because it re-runs each scenario once per
  assertion.

## Out of scope

- **Schema validation of scenario inputs** — morph serves no schemas over the
  wire (morph#171, morph#234). Unchanged.
- **Replacing in-process C++ tests.** Model behaviour stays where it is.
- **crm and lims servers.** lims is filed as an issue; crm's absent client is
  already a recorded decision.
- **Per-rung feature exhaustiveness.** Coverage is measured over the protocol
  surface. Rung scenarios are breadth over that surface, not a second target.
