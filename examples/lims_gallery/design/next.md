# LIMS gallery: where the work stands and how to continue

The whole initiative lands as one PR, #1045 (draft), from `feat/lims-gallery`. Do not open separate PRs. Work runs in
side branches, each in its own worktree, and is rebased onto `feat/lims-gallery` when it is folded in.

The live tracker is `status/STATUS.md` in this folder, published as a phone-friendly page (see "The status board"
below). It lists every lane, its commits, the decisions and the follow-ups; read it first.

## Resume here (paused 2026-10-31)

Everything below this section is older history; this section is current.

| Branch | Worktree | Holds | State |
|---|---|---|---|
| `feat/lims-gallery` (PR #1045) | — | batches A–J, verified locally (`300cee8c1`) | pushed; CI failed 8 jobs (see below) |
| `lg-ks` | `../morph-lims-e3` | `300cee8c1` + lane K-S's 10 screen-fix commits | done, 5237/5237 tests |
| `lg-k-design` | `../morph-lims-e2` | `lg-ks` + the batch K design + this tracker | design written, reviewed, **not yet revised** |
| `lg-ci7` | `../morph-lims-e1` | `300cee8c1` + one WIP commit of lane CI (partial clang-tidy fixes) | WIP, unverified, may not compile |

CI on `300cee8c1` (run 41146136081, WASM 41146136019) failed: Application ladder, Build & deploy WASM demo,
Linux / Qt6 WebSockets, Linux / clang-coverage, Linux / gcc-release, Windows / cl-debug, Windows / clangcl-debug,
clang-tidy-diff.

Grab with `QT_FORCE_STDERR_LOGGING=1`: without it Qt sends its warnings to the system journal when stderr is not a
terminal, and an empty log proves nothing.

Next steps, in order:
1. Finish lane CI from `lg-ci7` (brief: `handoff/lane-ci-brief.md`): diagnose all 8 jobs from their logs, fix real
   defects, record evidence for runner or flaky failures. Squash the WIP commit into real commits.
2. Revise `docs/superpowers/specs/2026-10-31-lims-gallery-batch-k-design.md` with its review
   (`handoff/batch-k-review.md`: 2 blockers, 4 should-fixes) and the user's answers: selected text defaults to the
   text colour; narrow the samples and plate actions (keep the principal); keep the calendar button; both dialog
   rules in the divider colour; a new `elevation.dialog` (consider following `elevation.card`). Also investigate four
   leftovers from K-S: instrument 390's panel header-to-content gap (~43 px vs 30), console 390's dark head (60 vs
   52 px), audit 390's Export PDF inset (8 vs 16 px), samples 390's Register flush to the head's edge.
3. Implement batch K's lanes (K-A, K-B, K-C) on Sonnet from `lg-ks`; rules in `handoff/lane-common-brief.md`.
4. Fold K + CI fixes, verify every check, re-grab (with `--user "Dr. A. Meyer"` and `QT_FORCE_STDERR_LOGGING=1`),
   compare with the mockups (two reviewers; the after-J reports are `handoff/after-j-findings-{A,B}.md`).
5. Push to `feat/lims-gallery`, drive CI green, merge once no known gaps remain.

## Branches

| Branch | Holds | State |
|---|---|---|
| `feat/lims-gallery` | the gallery, batches A, B, C, E1, E2 (the chart) and D parts 1–2 | pushed; the PR's branch; built and green |
| `feat/lims-gallery-d3` | batch D part 3 (in progress), from `feat/lims-gallery` | not pushed until its first commits land |
| `feat/lims-gallery-c`, `feat/lims-gallery-e1`, `feat/lims-gallery-e2`, `lg-fold-e2-on-b2` | already folded into `feat/lims-gallery` | kept for reference only |

Worktrees: `../morph-lims-gallery` (`feat/lims-gallery`), `../morph-lims-e1` (`feat/lims-gallery-d3`),
`../morph-lims-e2` (free since batch C was folded in), each with its own build tree `build/linux-everything`.

Designs, all reviewed, are in `docs/superpowers/specs/2026-10-25-lims-gallery-batch-{b,c,d,e}-design.md`.

## Batch B, landed

Part 1 ("columns and the selection"): G2, G47 with `valign`, G29, G43 and G30. Part 2 ("the row pipeline"): G41 (a
selected push button), G31 (sortable columns, `sort` on the table and the `sort` form), G28 (`pageSize` and `page`, the
`slice` function, and the selection kept across pages), G44 (the `runs` form) and G46 (a table's `group` heading rows).
A table over rows now refuses `filters` and `highlightKey`, and reads `showHeader`. The samples screen sorts, pages and
shares its order with the compact cards; the audit trail pages; the results worksheet groups by sample; the plate's
sample wells are selected push buttons, its "to place" list is a slice, and compact lists a plate row's empty wells as
runs. The plate's empty rows are not folded into one section.

**Batch C ("text, fields and small controls") has landed:** plain text on Qt Quick, spans (G2, G3), stacked and
toned key-values (G24, G39), the required mark after the caption (G8), a field's description and tone (G10), a dialog's
description (G6), `group` (G17), a text input's unit (G9), `dangerOutline`, outline badges and segmented selects (G14,
G33, G53), list entries with content (G52), the application's date pattern (G13) and a prominent text input (G11),
with conformance cases 50 to 54. The gallery's app document declares the date patterns, and the client, which mounts
the app document, gives them to every screen.

## How to continue

1. Let batch D part 3 (the head's menu G18, G26, G4) finish on its branch.
2. Rebase it onto `feat/lims-gallery`, build and run the six suites and qmllint, then push.
3. Batch D part 4 (G50 with #1064, G56), per the batch D design.
4. Take the PR out of draft and update its body (the closes list is in the tracker).

**Expect folding conflicts** in `include/morph/ui/testing/backend_conformance.hpp`, `tests/test_ui_conformance.cpp`
and `docs/spec/ui/backend_contract.md`: every batch appends conformance and layout cases at the end of the same lists.
Keep both sides, set the array sizes and the count check to the sum, and number each case's description in
`backend_contract.md`'s lists by its position; the designs' own case numbers do not carry over. Where both sides add
whole functions in one spot, take ours and insert the other commit's added functions from its diff, rather than
concatenating conflict hunks.

## The status board

- Source `status/STATUS.md`, time log `status/times.jsonl`, page URL in `status/board-url.txt`. The tools are the
  contour-workflows `status-board` skill's `time_log.py` and `status_page.py`.
- Log an event, stamped with the current time:
  `time_log.py --log examples/lims_gallery/design/status/times.jsonl start|report|done|idle …`
- Render with `status_page.py --source examples/lims_gallery/design/status/STATUS.md --time-log
  examples/lims_gallery/design/status/times.jsonl --output <scratch>/status-board.html`, then publish that file to the
  URL (the Artifact tool, with `url`).
- On every update, set each running lane's row from its branch's `git log`, not only when an agent reports. Never edit
  "Time spent" by hand; it is generated from the log. Commit the tracker with the work.

## Working rules

- Read `AGENTS.md` first. Specs change in the same commit as the code; comments and specs state current behaviour
  and why, with no history and no issue numbers.
- Every new test has a control: break the feature, see the test fail, restore it (and `touch` the restored file so
  ninja rebuilds).
- Builds from several worktrees share one machine: wrap every build, test and tidy run in one `flock`, `-j10` at most.
- Before pushing, build everything warning-free and run `morph_tests` (one "failed as expected" case is normal),
  `morph_qt_quick_tests`, `morph_tui_tests`, `morph_client_tests`, `lims_gallery_tests` and
  `lims_gallery_render_smoke`, the Qt ones with `QT_QPA_PLATFORM=offscreen`.
- CI runs clang-tidy 22 on changed lines with warnings as errors: identifiers of 3 or more characters, `T const*`,
  `auto` for casts, `contains()`, ranges algorithms, `const` locals, cognitive complexity under 25, `emplace_back`,
  non-trivial parameters by const reference. The local `tidy22` container has no Qt; Qt sources need a separate check.
- Commit messages: an imperative subject, a body saying what and why, `Closes #N` where an item has an issue.
