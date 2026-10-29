# morph · feat/lims-gallery — status

The run's tracker, committed so another agent can pick it up; `next.md` says how to update and publish it. Times are local, from `date`, to the second.

**Last updated:** 2026-10-29 01:24:28

## Current plan: LIMS gallery and the declarative-UI batches it found

Ten real LIMS screens (`examples/lims_gallery`, no backend, green theme) test what ui/1 can express, and their gap log
becomes five framework batches (A–E), each designed, reviewed once and implemented on this one branch. It all lands
as one PR, #1045 (draft), which leaves draft when every batch is in.

**Decisions:**
- One branch and one PR for the whole initiative; no separate PRs.
- The gallery has no backend: sample data lives in screen state.
- Green (`#17784A`) is the main colour.
- Framework changes land in batches, not one gap at a time.
- Each design gets one review by a separate agent on the default model, never Fable.
- Run 2–3 agents at a time, each in its own worktree and build tree, with builds taking turns through one lock; branches are rebased onto `feat/lims-gallery` and folded in.
- Batch B owns the `mono` text role and the `font.mono` token; batch C cites them.

| Lane | Tasks done | State | Where it is |
|---|---|---|---|
| Gallery: ten LIMS designs | 10/10 | done | 01–10 on the branch, each with compact and expanded layouts |
| Batch A: defects and responsive layout | 15/15 | done | 14 fixes and the gallery cleanup, `b6581747a`..`1e4c9cf34`; closes #1046–#1052, #1054, #1055 |
| Batch B part 1: columns and the selection | 6/6 | done | `27c072205`..`7c7f9d708` on `feat/lims-gallery`, pushed; G2, G47, G29 (closes #1053), G43 (closes #1059), G30 and the gallery cleanup |
| Batch B part 2: the row pipeline | 7/7 | done | `8215ca989`..`cdb41fe9f` on `feat/lims-gallery`, pushed: G41 selected button, G31 sort, G28 paging, G44 `runs`, G46 group headings, the `showHeader` flag fix, and the gallery cleanup; all six suites pass |
| Batch C: text, fields and small controls | 16/16 | done | 13 commits on `feat/lims-gallery-c` (pushed): plain text (#1058), spans, key-value, required mark, help text, dialog description, input group, unit (#1060, #1062), outlined and segmented controls (#1061), multi-line list entries, the date pattern, the focal scanner field, gallery cleanup; all suites and qmllint pass |
| Rebasing D parts 3–4 onto the batch C fold, and the final check | 1/1 | done | `feat/lims-gallery` now holds every batch. The sheet and C's dialog description share one dialog; the app document carries C's date patterns and D4's navigation counts. Three fixes on the way: the node-kinds array size in each D commit that adds a kind, the e-signature's note, and clang-format 22 over 54 files plus four Doxygen gaps from C. Build with no warnings; `morph_tests` 3275 (1 failing as expected), qt 188, tui 367, client 38, gallery 54, render smoke, qmllint and Doxygen pass. 54 conformance and 16 layout cases |
| Folding batch C onto `feat/lims-gallery` | 1/1 | done | `feat/lims-gallery-c` (`85ddd58b8`, pushed) is `feat/lims-gallery` plus C's 13 commits. Build clean; `morph_tests` 3260 (1 failing as expected), qt 186, tui 365, client 34, gallery 53, render smoke and qmllint pass. 54 conformance and 15 layout cases |
| Batch D design | 2/2 | done | written and reviewed, `84d3b53cc` (D4 fixes #1063 and #1064) |
| Batch D part 1: following the data | 8/8 | done | on `feat/lims-gallery`. on `feat/lims-gallery-e1` (pushed) over B part 1 and E1: #1063, G51, G20, G32, G35, G16, G38 and the cleanup; I built it: no warnings, all six suites pass |
| Batch D part 2: layout and controls | 4/4 | done | on `feat/lims-gallery`. `572fcc7ec`..`e96f52973` on `feat/lims-gallery-e1` (pushed): G23 grid column widths, G18 menu drawn as one button, G27 removable badges as filter chips, the gallery cleanup, and a qmllint fix for part 1's tab bar; all suites and qmllint pass |
| Folding E1, D parts 1–2 and the chart onto `feat/lims-gallery` | 1/1 | done | `feat/lims-gallery` now holds batches A, B, E1, E2 and D parts 1–2 (`620ad5378`, pushed). I built it with no warnings; all six suites and qmllint pass. 49 conformance and 15 layout cases |
| Batch D part 3: the head and overlays | 4/4 | done | `17d45396c`..`f1cb577fe` on `feat/lims-gallery-d3` (pushed): G18 the head's own menu, G26 search and select in the head, G4 a dialog as a sheet (layout case 16), the gallery cleanup; all suites and qmllint pass |
| Batch D part 4: navigation | 3/3 | done | `7f84dadca`..`9771a177c` on `feat/lims-gallery-d4` (pushed): G50 navigate as a dialog that answers its opener (closes #1064, reproduced first: the call was never answered and a later mount answered it with its own value), G56 navigation counts and the compact shell, the gallery cleanup; all suites, qmllint and Doxygen pass |
| Batch E design | 2/2 | done | written and reviewed, `36d08c929` |
| Batch E1: expressions | 7/7 | done | on `feat/lims-gallery`. on `feat/lims-gallery-e1` under D part 1; closes #1066 and #1065 |
| Batch E2: the chart | 2/2 | done | on `feat/lims-gallery`. on `feat/lims-gallery-e2` (pushed); it uses E1's checked rounding, so it goes in after `feat/lims-gallery-e1`. A first try on batch B part 2 alone failed to compile; that resolution is kept on local branch `lg-fold-e2-on-b2` (drop its stray `backend_conformance.hpp.orig`). Tidy 22 on `chart_scale.hpp`: 0 warnings |

**Now:** every batch is folded into `feat/lims-gallery` and verified locally; PR #1045 is out of draft with its final body, and its CI is running.
**Next:** watch CI, fix what fails in the commit that caused it, merge on green.

## Time spent

From the time log, stamped as each event happened. *Implement* runs from a task's dispatch to its first commit or report, *review & fixes* from there to done, and *wall time* from the first dispatch to the last completion: less than their sum where work ran in parallel. Work still running counts to 2026-10-29 07:39:19.

| Lane or phase | Finished | Wall time | Implement | Review & fixes | Mean per task | Idle |
|---|---|---|---|---|---|---|
| B1 · Batch B part 1: columns and the selection | 1 | 4h 27m | 4h 27m | 0m 00s | 4h 27m | — |
| B2 · Batch B part 2: the row pipeline | 1 | 16h 49m | 16h 49m | 0m 00s | 16h 49m | 6h 38m |
| C · Batch C: text, fields and small controls | 1 | 71h 41m | 71h 41m | 0m 00s | 71h 41m | 8h 10m |
| D · Batch D: layout, overlays and navigation | 2 | 3h 00m | 35m 00s | 0m 11s | 17m 36s | — |
| E · Batch E: expressions and charts | 2 | 44m 03s | 44m 03s | 0m 00s | 22m 02s | — |
| E1 · Batch E1: expressions | 1 | 2h 58m | 2h 57m | 1m 16s | 2h 58m | — |
| D1 · Batch D part 1: following the data | 1 | 15h 12m | 15h 12m | 0m 00s | 15h 12m | 7h 05m |
| E2 · Batch E2: the chart | 1 | 4h 41m | 4h 41m | 0m 00s | 4h 41m | 35m 09s |
| D2 · Batch D part 2: layout and controls | 1 | 6h 14m | 6h 14m | 0m 00s | 6h 14m | 8m 06s |
| FOLD · Folding the side branches (running) | 2 | 63h 47m | 61h 19m | 5m 46s | 30h 42m | 1h 31m |
| D3 · Batch D part 3: the head and overlays | 1 | 2h 41m | 2h 41m | 0m 00s | 2h 41m | — |
| D4 · Batch D part 4: navigation | 1 | 2h 17m | 2h 17m | 0m 00s | 2h 17m | — |
| **All** (running) | 15 | 91h 27m | 189h 42m | 7m 13s | 12h 39m | 24h 09m |

### B1 · Batch B part 1: columns and the selection

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| B1.1 | Batch B part 1: finish G2, then G47, G29, G43, G30 and the gallery cleanup | 2026-10-25 14:42:13 | 4h 27m | 0m 00s | 4h 27m | — | 0 |

### B2 · Batch B part 2: the row pipeline

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| B2.1 | Batch B part 2: G41, G31, G28, G44, G46, the showHeader flag and the gallery cleanup | 2026-10-25 19:45:03 | 16h 49m | 0m 00s | 16h 49m | 6h 38m | 0 |

### C · Batch C: text, fields and small controls

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| C.1 | Batch C: plain text (#1058) first, then the design's order, and the gallery cleanup | 2026-10-25 23:06:07 | 71h 41m | 0m 00s | 71h 41m | 8h 10m | 0 |

### D · Batch D: layout, overlays and navigation

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D.1 | Write the batch D design (resumed from its research) | 2026-10-25 12:12:03 | 4m 39s | 0m 11s | 4m 50s | — | 0 |
| D.2 | Review the batch D design | 2026-10-25 14:42:13 | 30m 21s | 0m 00s | 30m 21s | — | 1 |

### E · Batch E: expressions and charts

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| E.1 | Write the batch E design | 2026-10-25 14:42:13 | 23m 32s | 0m 00s | 23m 32s | — | 0 |
| E.2 | Review the batch E design | 2026-10-25 15:05:45 | 20m 31s | 0m 00s | 20m 31s | — | 1 |

### E1 · Batch E1: expressions

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| E1.1 | Batch E1: #1066, G36, #1065, half-up, G45, G37 and the gallery cleanup | 2026-10-25 15:26:16 | 2h 57m | 1m 16s | 2h 58m | — | 0 |

### D1 · Batch D part 1: following the data

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D1.1 | Batch D part 1: G51, G20, G32, G35, G16, G38, plus the app document's mount (#1063) | 2026-10-25 18:24:42 | 15h 12m | 0m 00s | 15h 12m | 7h 05m | 1 |

### E2 · Batch E2: the chart

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| E2.1 | Batch E2: G55 chart on Qt Quick and the terminal, and the gallery cleanup | 2026-10-25 18:24:42 | 4h 41m | 0m 00s | 4h 41m | 35m 09s | 0 |

### D2 · Batch D part 2: layout and controls

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D2.1 | Batch D part 2: G23, G18's menu node, G27 and the gallery cleanup | 2026-10-26 09:37:10 | 6h 14m | 0m 00s | 6h 14m | 8m 06s | 0 |

### FOLD · Folding the side branches

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| FOLD.1 | Rebase E1 and D parts 1-2 onto batch B part 2, then the chart | 2026-10-26 15:51:30 | 52h 27m | 5m 46s | 52h 33m | 1h 31m | 1 |
| FOLD.2 | Rebase batch C onto feat/lims-gallery | 2026-10-28 22:47:49 | 8h 51m | 0m 00s | 8h 51m | — | — |
| FOLD.3 | Rebase D parts 3-4 onto the C fold, verify everything once (running) | 2026-10-29 07:39:12 | 0m 08s | — | 0m 08s | — | — |

### D3 · Batch D part 3: the head and overlays

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D3.1 | Batch D part 3: G18 head menu, G26, G4 and the gallery cleanup | 2026-10-28 20:25:05 | 2h 41m | 0m 00s | 2h 41m | — | 0 |

### D4 · Batch D part 4: navigation

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D4.1 | Batch D part 4: G50 (#1064), G56 and the gallery cleanup | 2026-10-28 23:07:04 | 2h 17m | 0m 00s | 2h 17m | — | 0 |

### Idle stalls

| Where | From | To | Idle | What stalled |
|---|---|---|---|---|
| Task D1.1 | 2026-10-25 19:10:01 | 2026-10-25 19:45:10 | 35m 09s | session limit; resumed at the reset |
| Task E2.1 | 2026-10-25 19:10:01 | 2026-10-25 19:45:10 | 35m 09s | session limit; resumed at the reset |
| Task B2.1 | 2026-10-26 00:40:00 | 2026-10-26 07:10:50 | 6h 30m | session limit, then waiting overnight to be resumed (counted from the 00:40 reset; the stop time before it is not recorded) |
| Task C.1 | 2026-10-26 00:40:00 | 2026-10-26 07:10:50 | 6h 30m | session limit, then waiting overnight to be resumed (counted from the 00:40 reset; the stop time before it is not recorded) |
| Task D1.1 | 2026-10-26 00:40:00 | 2026-10-26 07:10:50 | 6h 30m | session limit, then waiting overnight to be resumed (counted from the 00:40 reset; the stop time before it is not recorded) |
| Task B2.1 | 2026-10-26 12:10:00 | 2026-10-26 12:18:06 | 8m 06s | session limit; resumed after the 12:10 reset (the stop time before it is not recorded) |
| Task C.1 | 2026-10-26 12:10:00 | 2026-10-26 12:18:06 | 8m 06s | session limit; resumed after the 12:10 reset (the stop time before it is not recorded) |
| Task D2.1 | 2026-10-26 12:10:00 | 2026-10-26 12:18:06 | 8m 06s | session limit; resumed after the 12:10 reset (the stop time before it is not recorded) |
| Task C.1 | 2026-10-28 18:00:00 | 2026-10-28 19:31:08 | 1h 31m | weekly usage limit; resumed after the Oct 28 18:00 reset (stopped on Oct 26 around 16:00; the exact stop time is not recorded) |
| Task FOLD.1 | 2026-10-28 18:00:00 | 2026-10-28 19:31:08 | 1h 31m | weekly usage limit; resumed after the Oct 28 18:00 reset (stopped on Oct 26 around 16:00; the exact stop time is not recorded) |

## Key rulings

- A part's new conformance and layout cases are appended to the lists and described in `backend_contract.md`'s numbered lists, so the design's case numbers do not carry over.
- Batch E: reference lines replace the chart's axis ticks only when there are 3 or more.
- Batch E: chart series colours `#0072B2 #C25400 #00805E #A8508A`, each with its own marker shape.
- Batch E: SD is the sample SD (n−1).
- Batch E2's chart takes layout case L16, since batch D part 1 takes L15.
- Independent work runs in a second worktree and is rebased onto `feat/lims-gallery`; one PR still.
- The tracker and its time log are committed in `examples/lims_gallery/design/status/`, so another agent can continue; `next.md` says how.

- Batch D: the workspace scrolls a screen whose root doesn't stretch, so no screen is cut off.
- Batch D: `as: window` is reported once at info.
- Batch D: the shell switches to its compact form below `expanded`.
- Batch D: a navigation that mounts nothing cancels its call, so the caller is answered exactly once.

- Enter in a dialog: a text input with its own `onSubmit` takes Enter; one without lets it reach the dialog's
  default; a checkbox never takes Enter.
- Width classes are in units: compact below 80, medium below 160; an 80-column terminal is medium.
- Touch-sized controls follow the width class, not touch detection.
- A plain click, Shift-click and Shift+arrow replace the whole table selection, other pages included; Ctrl-click,
  Space and Ctrl+A extend it.
- A table's `sort` is a list of `{column, direction}`; `[]` is the source order.
- Strings sort case-folded, ties by bytes, matching the model-backed tables.
- `sort`, `slice` and `runs` join `expr/1` in place.
- Spans may carry plain `text` beside them as an older client's fallback.
- One application date pattern applies to every date style; a `+02:00` offset token, no zone names.

## Follow-ups

- From batch C: a check box toggle and a select choice now mark the field touched, so a required group's message shows under the default policy (new behaviour, in `interpreter.md`); the Doxygen build was not run on C's new public symbols.
- From D part 2: the plate's well grid uses stretch tracks, and on Qt Quick at 1280 × 900 "STD1" is elided to "S…"; the old equal-column grid let a column take its widest cell's width, which tracked grids do not. Whether stretch tracks should is a framework decision (a visible regression in the gallery).
- From D part 1: stability's Continue is not pinned on compact, because `stepper` has no option to pin its footer; the rule refusing a focus into two switches cannot tell that two cases under one width switch are never mounted together.
- E2's `chart_scale.hpp`: the editor's clang-tidy flagged `chartTicks` at cognitive complexity 31 and an identical-branch conditional; the lane reported tidy clean, so re-run tidy on it when folding.
- From E1: the parser accepts a JSON key `""` beside `fn` and `get` (read from the code, not reproduced); `expressions.md` still says a new form needs a new expression version, against the in-place `expr/1` decision; `plate.cpp` and `review.cpp` keep the map-then-first workaround.

- Open defects found by the gallery, to be fixed in its batches: #1058 (Qt Quick draws data text as markup), #1059
  (Qt Quick tables unreachable from the keyboard), #1060 (multiline input with an icon loads, then mount throws),
  #1061 (unknown select style loads silently), #1062 (dialog title and field unit hidden from screen readers).
- Issue #1063: the generic client never mounts the app document, so `app.*` reads nothing; batch D's navigator counts
  depend on it.
- Issue #1064 (weak evidence): `navigate` with `onResult` to an already open screen may never be answered.
- Issue #1065: `mean` and `decimal` return a saturated value on overflow instead of reporting it (reproduced).
- Issue #1066: a numeric path segment past 2^64 wraps and reads the wrong element (reproduced).
- Table column labels are evaluated once, as tab labels were: batch B part 2 checks it.
- A document table's `"showHeader": false` is ignored (the loader stores a flag, the interpreter reads a prop): read from the code; fixed in batch B part 2, not filed, since the fix is one line.
- Issue #1067 (weak evidence): default error and info tones fall below 3:1 on the dark sidebar surface.
- Issue #1056: env `locale`, `platform` and `online` are never set.
- The gallery's render smoke does not fail on a property write Qt refuses; fix it inline in the example.
