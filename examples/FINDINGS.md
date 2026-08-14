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
`NNN-<kebab-slug>.md` with:

```markdown
---
id: NNN
title: <one line>
subsystem: <core|bridge|backend|offline|journal|forms|units|session|qt|wire>
severity: blocker | major | minor | paper-cut
source: <rung / spike / review round that produced it>
disposition: open | fix-scheduled | documented-limitation | wontfix
test: <path to the failing test, or "spec-cited">
---

<repro or spec citation; what should happen; what happens instead.>
```

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
