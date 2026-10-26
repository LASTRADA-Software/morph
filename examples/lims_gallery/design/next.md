# LIMS gallery: where the work stands and how to continue

The whole initiative lands as one PR, #1045 (draft), from `feat/lims-gallery`. Do not open separate PRs. Work runs in
side branches, each in its own worktree, and is rebased onto `feat/lims-gallery` when it is folded in.

The live tracker is `status/STATUS.md` in this folder, published as a phone-friendly page (see "The status board"
below). It lists every lane, its commits, the decisions and the follow-ups; read it first.

## Branches

| Branch | Holds | State |
|---|---|---|
| `feat/lims-gallery` | the gallery, batches A, B part 1 and B part 2 | pushed; the PR's branch |
| `feat/lims-gallery-e1` | E1 (expressions), D part 1, D part 2 (in progress), rebased onto B part 1 | pushed up to its last commit |
| `feat/lims-gallery-e2` | E2 (the chart), on the old E1 base | pushed; **needs E1**: it calls E1's checked rounding |
| `feat/lims-gallery-c` | batch C (in progress), from B part 1 | pushed up to its last commit |
| `lg-fold-e2-on-b2` | a first try at putting the chart on B part 2: it does not compile without E1, but its conformance merge can be reused; drop its stray `backend_conformance.hpp.orig` | pushed for reference only |

Worktrees: `../morph-lims-gallery` (`feat/lims-gallery`), `../morph-lims-e1` (`feat/lims-gallery-e1`),
`../morph-lims-e2` (`feat/lims-gallery-c`), each with its own build tree `build/linux-everything`.

Designs, all reviewed, in `docs/superpowers/specs/`: `2026-10-25-lims-gallery-batch-{b,c}-design.md` on this branch;
`…-batch-{d,e}-design.md` on `feat/lims-gallery-e1`.

## Batch B, landed

Part 1 ("columns and the selection"): G2, G47 with `valign`, G29, G43 and G30. Part 2 ("the row pipeline"): G41 (a
selected push button), G31 (sortable columns, `sort` on the table and the `sort` form), G28 (`pageSize` and `page`, the
`slice` function, and the selection kept across pages), G44 (the `runs` form) and G46 (a table's `group` heading rows).
A table over rows now refuses `filters` and `highlightKey`, and reads `showHeader`. The samples screen sorts, pages and
shares its order with the compact cards; the audit trail pages; the results worksheet groups by sample; the plate's
sample wells are selected push buttons, its "to place" list is a slice, and compact lists a plate row's empty wells as
runs. The plate's empty rows are not folded into one section.

## How to continue

1. Let batch D part 2 (G23, G18's menu node, G27, cleanup) and batch C finish on their branches.
2. Fold onto `feat/lims-gallery`, in this order: `feat/lims-gallery-e1`; E2's three commits; batch C. Build and run
   the six suites once after the fold, then push.
3. Batch D part 3 (the head's menu G18, G26, G4) and part 4 (G50 with #1064, G56), per the batch D design.
4. Take the PR out of draft and update its body (the closes list is in the tracker).

**Expect folding conflicts** in `include/morph/ui/testing/backend_conformance.hpp`, `tests/test_ui_conformance.cpp`
and `docs/spec/ui/backend_contract.md`: every batch appends conformance and layout cases at the end of the same lists.
Keep both sides, set the array sizes and the count check to the sum, and number each case's description in
`backend_contract.md`'s lists by its position; the designs' own case numbers do not carry over. Where both sides add
whole functions in one spot, take ours and insert the other commit's added functions from its diff, rather than
concatenating conflict hunks.

Two corrections to `gaps.md` from the batch C design, for when batch C lands: G10 says a field is outlined while it has
errors, but no Qt Quick input is outlined for errors at all; G13's date formatting hook is never set by any client
(related to #1056).

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
