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

Each finding is a **GitHub issue**. Open it with the repro or spec citation,
what should happen, and what happens instead — and state up front:

- **subsystem** — `core`, `bridge`, `backend`, `offline`, `journal`, `forms`,
  `units`, `session`, `qt` or `wire`, mirrored by the issue's `area:` label.
- **severity** — blocker, major, minor, or paper-cut.
- **source** — the rung, spike or review round that produced it.
- **test** — the path to the failing test, or "spec-cited".

The disposition is the issue's `triage:` label, and the issue's own state is
whether the finding is still open.

## Identity and citation

The finding's id is its **GitHub issue number**, assigned by GitHub. Cite it as
`#NNN`, and in prose name the subject as well as the number — "the async
shared/keyed attach finding (#207)". A bare number is precise but tells a
reader nothing about whether it still says what the citing text claims.

**Findings used to be files** under `docs/findings/`, named
`<ns>-NNN-<kebab-slug>.md` and namespaced by rung. That directory has been
retired and its remaining entries migrated to issues. Two pieces of reasoning
from it are worth keeping, because they are why the file scheme existed and
why it stopped:

- **A single global sequence does not survive parallel branches**, and did not:
  two disjoint series were allocated independently and both merged, so
  `001`–`004` came to mean one thing on one branch and something else on
  another. Every one of those commits followed the obvious rule ("take the next
  unused number") correctly — the rule was the problem, not the discipline.
  *A rule that every violation already satisfies is not a control.* Rung
  namespacing (`r4-001`, `r5-001`) fixed that; issue numbers, allocated
  centrally, remove the question entirely.
- **Ids are never reused, and a stale citation should look stale.** A citation
  that outlives its target must fail to resolve rather than silently point at
  something else. Closed issues stay addressable, which is strictly better than
  a deleted file — but the hazard survives the move: several code comments were
  found citing `finding 004`, which had been *renamed* rather than deleted and
  so resolved, silently, to an unrelated finding.

Historical citations of the form `docs/findings/NNN` or a bare `finding NNN`
resolve to nothing and should be rewritten to name the issue or state the fact
directly.

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
- **promoted** — [`IMPLEMENTATION.md`](IMPLEMENTATION.md)'s promotion rule
  fired (a third rung independently built the same app-layer answer to a
  framework gap): the answer moved into `include/morph`, with its full docs
  tax, and every consuming rung may re-express itself over the promoted
  interface. The rule's other exit is **documented-limitation** under the
  name "app-layer by design" — see [morph#197](https://github.com/LASTRADA-Software/morph/issues/197)'s
  disposition (`docs/spec/offline/offline.md`, "Disposition: app-layer by
  design") for that branch. See "Promoted findings" below for the record.

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
current output. Back-filling them as issues — failing tests where
expressible — is **the first task of rung 0**, before any app
code. The four LADDER prerequisites and the forms-gap ledger entries are
findings 001–0NN.

## Promoted findings

The record the **promoted** disposition above points to — the promotion rule
has fired once so far:

- **Op-id/exactly-once replay ledger.** Seven hand-written, near-identical
  copies of the same idempotency-check-then-set table had accumulated across
  five rungs (`bookmarks`, `crm`, `kanban`, `ledger`, `lims`) with no
  `FINDINGS.md` entry recording it, three rungs past the promotion rule's own
  trigger point ([morph#226](https://github.com/LASTRADA-Software/morph/issues/226)).
  Promoted as `morph::offline::IReplayLedger`
  (`include/morph/offline/replay_ledger.hpp`) plus a conformance suite
  (`tests/replay_ledger_conformance.hpp`); storage stays app-side, per rung,
  because `include/morph` has no dependency on any SQL library and the
  check-then-set must commit in the same transaction as the write it guards
  (see the interface's own doc comment). `bookmarks` is migrated onto it
  (`BookmarksReplayLedger` in `examples/bookmarks/src/models/bookmark_model.cpp`); the
  other four rungs are follow-up work, not required by this promotion.

## Demotion policy (the ladder must never tax the framework)

Once a rung exits, it **demotes** in per-PR CI to compile-only plus one
smoke test; its full matrix moves to the weekly tier (see
[`TESTING.md`](TESTING.md), "Build system and CI") instead of running on
every push; its 100%-coverage gate freezes at its exit commit and does not
bind future framework PRs. The instrument built to motivate framework
change must never become the reason a framework fix is too expensive to
land.
