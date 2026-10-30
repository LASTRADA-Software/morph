# LIMS gallery: where the work stands and how to continue

The whole initiative lands as one PR, #1045, from `feat/lims-gallery`. Do not open separate PRs. Work runs in
side branches, each in its own worktree, and is rebased onto `feat/lims-gallery` when it is folded in.

The live tracker is `status/STATUS.md` in this folder, published as a phone-friendly page (see "The status board"
below). It lists every lane, its commits, the decisions and the follow-ups; read it first.

## Branches

| Branch | Holds | State |
|---|---|---|
| `feat/lims-gallery` | the gallery and batches A–G with their fixes | pushed; the PR's branch |
| `lg-fold-h` (worktree `../morph-lims-e1`) | `feat/lims-gallery` plus batch H (design, H-C and H-S lanes), the Emscripten job pool, and this tracker | verified locally: no warnings, every test, Qt 6.8.1 qmllint and Qt Quick tests, GCC `-Wshadow`; not pushed until clang-tidy-diff is clean |
| `lg-tidy` (`../morph-lims-e2`) | lane T: clears clang-tidy-diff (clang-tidy 22.1.8) over the branch's changed lines | running; fold onto `lg-fold-h` |
| `lg-i-design` (`../morph-lims-e3`) | batch I's design, from the comparison after batch H | being written |

Designs are in `docs/superpowers/specs/2026-10-*-lims-gallery-batch-*-design.md`. The comparison after each batch is
in `visual-review.md`.

## How to continue

Every difference the comparison with the mockups finds is implemented in this branch, as another batch: design (default
model), a design review, then two or three Sonnet lanes with disjoint files, folded here, re-grabbed and compared
again. Batch I is the current one. When the comparison finds nothing left but environmental items (the font), fold lane
T's tidy fixes, push, drive CI to green and merge PR #1045.

**When folding a later branch, expect conflicts** in `include/morph/ui/testing/backend_conformance.hpp`, `tests/test_ui_conformance.cpp`
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
