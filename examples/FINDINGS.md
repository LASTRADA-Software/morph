# The finding pipeline

The ladder's product is **findings fixed, not apps shipped**. The holistic
(round-7) review found the "framework-gap ledger" load-bearing in every
governing document yet defined nowhere — so success would have defaulted to
the only thing definitions-of-done measure: apps built. This document
defines the pipeline.

## What a finding is

A finding is one of:

1. **A minimal failing test** checked into `tests/` (preferred — a finding
   that cannot be expressed as a failing test is not yet understood), or
2. **A spec-cited impossibility** — a short write-up citing the spec/header
   that shows the capability structurally cannot exist today (e.g. "no
   holder-swap primitive for in-place undo on a shared instance").

Each finding is a file under `docs/findings/` named
`<ns>-NNN-<kebab-slug>.md` with:

```markdown
---
id: <ns>-NNN
title: <one line>
subsystem: <core|bridge|backend|offline|journal|forms|units|session|qt|wire>
severity: blocker | major | minor | paper-cut
source: <rung / spike / review round that produced it>
disposition: open | fix-scheduled | documented-limitation | wontfix
test: <path to the failing test, or "spec-cited">
---

<repro or spec citation; what should happen; what happens instead.>
```

## Allocating an id

Ids are **namespaced by the rung that produced the finding**, not drawn from one
global sequence: `r5-001-<slug>.md`, `r5-002-<slug>.md`, and so on. The namespace
is the rung's number (`r0`–`r8`); a finding produced outside any rung — a spike,
or a whole-branch review — uses `core`.

The namespace exists because a single global sequence **does not survive parallel
branches**, and did not: two disjoint series were allocated independently and both
merged, so `001`–`004` came to mean one thing on one branch and something else on
another. Every one of those commits followed the obvious rule ("take the next
unused number") correctly — the rule was the problem, not the discipline. A rule
that every violation already satisfies is not a control.

Namespacing fixes that at the source: two rungs are worked by different people at
different times, so their sequences never interleave, and the id a branch picks
cannot be claimed by a branch it has never seen.

**Within a namespace:**

- **Allocation** — take the next unused number in *that namespace*, three digits,
  zero-padded. Only that rung's own files are in scope, so `ls docs/findings/r5-*`
  is the whole question.
- **Gaps are permanent.** A deleted or withdrawn finding leaves its number
  retired; ids are never reused, so a citation that outlives its target fails to
  resolve rather than silently pointing at a different finding.
- **Rebasing never renumbers.** An id is assigned once, when the file is created,
  and is stable for the life of the finding. If two branches in the same namespace
  do collide — the same rung worked twice in parallel — resolve it the way any
  other content conflict is resolved, by renumbering the *later* one at merge time
  and updating its citations, not by renumbering both.
- **Cross-rung references are fine** and read unambiguously: `r3-007` is precise
  from anywhere, which a bare `007` never was.

**Citing a finding.** In prose, name the finding by its slug as well as its id
("the async shared/keyed attach finding (`r3-001`)"). An id alone is precise but
brittle — it is the part that changed when the two series collided, and a reader
looking at the wrong branch resolves it to something else entirely. The slug is
what makes a stale citation *look* stale instead of resolving silently to the
wrong file.

## Triage and dispositions

Every finding gets a disposition within one triage pass (the repo owner
decides; the ladder never self-triages):

- **fix-scheduled** — a framework change is planned; the finding's test
  stays red-listed (tagged `[finding]`, excluded from the green gate) until
  the fix lands, then joins the regression suite permanently.
- **documented-limitation** — the behavior is accepted and the relevant
  `docs/spec/` file is updated to say so; the test asserts the *documented*
  behavior and turns green.
- **wontfix** — recorded with rationale.

## Fix budget

Discovery already outruns repair (the six detail review rounds produced
~40 findings before any rung code existed). The binding ratio: **for every
month of rung construction, at least one week of framework-fix time** is
spent draining `fix-scheduled` findings — including their full docs tax
(spec file, Doxygen, pinned facts). If the open `fix-scheduled` count grows
two rungs in a row, rung construction pauses.

## Rung exit criteria

A rung is **done** when:

1. its README's design questions are resolved in writing,
2. every named strain test exists — passing, or filed as a finding,
3. its findings are triaged (no `open` dispositions left).

**Feature completeness is explicitly not an exit criterion.** A rung may
exit half-built; Kanboard's remaining thirty tables exert no gravity here.

## Back-fill

The ~40 findings from review rounds 1–7 (preserved in the session review
reports and folded into the governing docs) are the program's entire
current output. Back-filling them as `docs/findings/` entries — failing
tests where expressible — is **the first task of rung 0**, before any app
code. The four LADDER prerequisites and the forms-gap ledger entries are
findings 001–0NN.

## Demotion policy (the ladder must never tax the framework)

Once a rung exits, it **demotes** in per-PR CI to compile-only plus one
smoke test; its full matrix moves to the weekly tier (see
[`TESTING.md`](TESTING.md), "Build system and CI") instead of running on
every push; its 100%-coverage gate freezes at its exit commit and does not
bind future framework PRs. The instrument built to motivate framework
change must never become the reason a framework fix is too expensive to
land.
