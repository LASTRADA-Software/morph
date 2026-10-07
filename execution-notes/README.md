# Execution notes — declarative UI program

The working record of executing the plans in `docs/superpowers/plans/2026-10-04-declarative-ui-tui*.md` on
`feature/declarative-ui`. Not meant to merge: these are logs, and `docs/superpowers/` holds present-tense reference
docs only. They exist so whoever continues the work sees every decision made so far.

## How the work runs

Each part of the master plan is executed task by task. For every task an implementer works from the task's text in
the plan, then an independent reviewer checks the diff against the plan, the specs, the shared interface contract and
the code already built, usually by probing with small programs under AddressSanitizer. Findings go back to the
implementer as fix rounds until the review is clean. Before a part starts, a pre-flight scan checks its plan against
the code that actually exists and proposes rulings; after a part ends, a whole-part review looks across its tasks.
Each part is then squashed into its one commit, whose message states every deviation from the plan and why.

The plans are deliberately not edited during execution. When a plan turns out wrong, the code and its `docs/spec/`
page are made right, the decision is recorded here as a ruling, and the squashed commit states it.

## Files

| File | What it holds |
|---|---|
| `program-notes.md` | Obligations a ruling in one part places on a later part. Every pre-flight scan reads it. **Read this first.** |
| `part-N-*/ledger.md` | Per part: each task's commits, review verdicts, fix rounds, deferred minor findings, and every `Ruling:` line (what was decided — why — what it costs if wrong). `(→ TN)` marks an item handed to a later task. |
| `part-N-*/preflight.md` | Per part (from Part 1 on): the pre-flight conflict table and the numbered rulings it proposed (R1, R2, …), which the ledger then accepts or refines. Part 0's scan is inside its ledger. |

Paths like `scratchpad/...` name throw-away probe programs that were kept locally and are not published.

## Where it stands

Parts 0–2 and Part 3's `ioloop` group are squashed on `feature/declarative-ui`; Part 3's `tui` group is at Task 17
of 20. Pending: four core-cpp issue candidates found during Part 3 (listed at the end of `part-3-tui/ledger.md`),
to be filed upstream after confirmation.
