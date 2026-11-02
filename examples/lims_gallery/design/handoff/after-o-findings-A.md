Reviewer A, after batch O: esignature, receipt, instrument, audit and samples, at 1000, 1215 and 390, plus receipt and
audit at 1920. Grabs are in `lg-cmp/app-o/` (23:47) and the baseline in `app-n/`. Every log is 0 bytes.

**Source caveat.** The `morph-lims-e1` worktree is checked out *detached at `ae287a95b`*, three commits into batch O,
not at `lg-fold-o` (`74bb31a79`). For example, its `receipt.cpp` still reads `percent(31)`. I read every source with
`git show lg-fold-o:<path>`. The grabs match `lg-fold-o`: the pane is at 32 % and controls are 30 px.

**Method.**
- An n→o image diff with threshold 8.
- PIL pixel reads, compared with the mockup PNG and the `.dc.html`.
- Anything marked *read* comes from code. Anything marked *inferred* was not measured.

**The 390 grabs are pixel-identical to `app-n` on all five screens.** This is expected and does not mean the grab
went stale. The phone sets `touchTargets`, so a control is `max(controlHeight, touchHeight)` (read: `Theme.qml:196`).
The 30 px change therefore cannot show at 390.

Severity is H/M/L. *screen* means the vocabulary already expresses the fix; *framework* means ui/1 cannot.

## E-signature

| Width | Finding |
|---|---|
| all | **Regressions:** none. At 1000 and 1215 the dialog only moved and shortened: controls are 30 px, so it re-centres about 5 px lower. |
| 1000/1215 | **O, record box border #E6E9ED: closed.** Border pixel (230,233,237). At N it was (221,225,230). |
| 1000/1215 | **O, panel fill follows the rounded frame: closed.** At N the corner pixel (379,244) was square border and fill. Now (380,244)=(251,252,252) and (381,244)=(234,237,241), an anti-aliased corner. |
| 1000/1215 | **O, controls 30 px: closed.** The buttons and inputs are 30 px, as in the mockup. |
| 390 | The border was already (230,233,237) at N. The mono record id is bold, which matches the design (`font-weight:600`). |
| 1000/1215 | **L, framework.** The record box's caption column is about 58 px; the design has 96 px (`.kv 96px`). `keyValue` has no caption-width key, only `fieldLayout`. As a result, Flags fits on one line where the design wraps "linked". The same gap appears on instrument (below). |
| 1000 | No clipping, overlap or lost content. |

## Receipt

| Width | Finding |
|---|---|
| all | **Regressions:** none. |
| 1000 | **O, pane 32 %: closed.** The pane is about 744–983 (240 px). The Outcome tags read "Received"/"Rejected" whole, where N showed "Recei…"/"Reject…". "Release, finished product" and "24-Oct-2026 09:41" still fit. |
| 1000 | Fit, not counted: the "Already received" notice now wraps "…Nothing was / changed." because the left column is 7 px narrower. It breaks at a word; nothing is lost. |
| 1215 | The pane is 900–1198 (299 px) against the design's 300, unchanged. |
| 1920 | **The cap holds.** The pane is 1605–1903 (about 300 px). The card's three field columns take the rest (about 440 px each), and the page reads as intended. |
| 1215/1920 | **O, controls 30 px: closed.** The Containers input is 319–348 (30 px); it was 32. |
| 1215 | **L, screen.** The two Condition columns are unequal. Column 2 starts at x 602 at 1215 and x 998 at 1920. The design has `repeat(2, 1fr)`, which puts it at about 568 and 917. The columns share the width in proportion to their content (ratio 1.27 at both widths; unchanged since N). Fix: `.tracks = {{}, {}}` (two `stretch(1)` tracks) instead of `.columns = 2` in `conditions()`. See also the possible library finding in the summary. |
| 1215 | **L, screen.** The "Sample S-26-104230" heading is about 13 px; the design has 14 px/600. It uses `.role = "section"`. Fix: a larger role or weight on that text. |
| 1000/1215 | L, not counted: there is no 3 px focus halo on the scan field (the field is not focused in the grab; inferred, as at N). |
| 390 | No change since N. "This session · 13 scans" sits below the fold under the pinned footer; the page scrolls, so nothing is lost. |

## Instrument

| Width | Finding |
|---|---|
| all | **Regressions:** none. Only the 30 px controls moved the head 1–2 px. 390 is identical. |
| 1000/1215 | **O, controls 30 px: closed.** "Log maintenance", "Book" and "⋯" are 30 px. |
| 1215 | **M, screen.** The Qualification rows are 22 px apart with no rules. The design has 29 px rows (`.q` `padding:6px 0`) with an #EDF0F3 rule between them. Fix: `qualificationList()` → `ui::column(..., {.gap = 2, .divided = true})`, as `eventList` already does. |
| 1215 | **L, screen (still open from L).** The event rows are 27 px apart (350/377/404) against 29. |
| 1000/1215 | **L, framework** (the same `keyValue` caption-width gap as e-signature). Details values start about 90 px after the captions; the design has 130 px (`.kv 130px`). |
| 1215 | L, screen. The panel insets are 8 px; the design has `padding:12px 14px`. All four cards use `panel(..., {.padding = 2})`; `.padding = 3` would come closer. |
| 1000 | No clipping. The right column fits "next 14-Jan-2027". |

## Audit

| Width | Finding |
|---|---|
| all | **Regressions:** none. 390 is identical. |
| 1000 | **O, rail 21 %: closed.** The rail is 165 px. Row 1's Reason keeps "OOS-2026-031" whole, where N broke it as "OOS-2026-03/1". There is no clipping and no sideways scroll. |
| 1215 | **L, accepted trade-off.** The rail is about 210 px (21 % of about 1000); the design has 220 (it was 220 at N with 22 %). No single percent gives 220 here and still keeps the record id whole at 785. Not a gap worth a ticket. |
| 1920 | **The cap holds.** The rail is about 223 px (29 units). Only Reason grows, so "24-Oct-2026" in New value still breaks at its hyphen, as the design also does at 1000. The page reads as intended. |
| 1000/1215 | **O, controls 30 px: closed.** The head rule moved from y 55 to 53. |
| 1000/1215 | **O, table stretch from content:** no visible effect. Reason is the only stretch column, and at 1000 it takes about 104 px with whole words. |

## Samples

| Width | Finding |
|---|---|
| all | **Regressions:** none. 390 is identical. |
| 1000/1215 | **O, Status 11 units: closed.** S-26-104159 reads "Investigation" whole at both widths, where N showed "Investigat…". |
| 1000/1215 | **O, controls 30 px: closed.** "Register samples" is 30 px (12–41); it was 32. |
| 1000 | Fit, not counted. Product · lot now breaks before the separator: "Paracetamol 500 mg / · L-26-0377" (and the same for Lactose, Paracetamol 0376). At N the "·" stayed on line 1. The column is 6 px narrower under the new stretch distribution. It is a word-boundary wrap on the 785 px page; nothing is lost. |
| 1215 | **L, screen/theme (known, now smaller).** The head rule is at y 53 (55 at N). The bulk bar's top is at 70 against the design's about 54. This is the head-padding delta, 2 px smaller than at N. |
| 1215 | Rows are one line, about 30 px. There is no stray sideways scroll. The pager sits below the fold, which is by design: 25 rows per page. |

## Summary

**Regressions: 0.** Every n→o change on these screens is the 30 px controls or an intended batch O fix.

**Batch O items on these screens:**
- **Closed, 8:**
  - e-signature record border #E6E9ED;
  - panel fill follows the rounded frame;
  - receipt pane 32 % (whole Outcome tags at 1000);
  - audit rail 21 % (whole "OOS-2026-031" at 1000);
  - samples Status 11 units ("Investigation" whole);
  - controls 30 px (verified on receipt, samples, instrument and e-signature);
  - receipt and audit caps at 1920;
  - no sideways table scroll.
- **Open:** 0.
- **Not visible here:** the table-stretch change shows no visible effect beyond the 1000-px wrap shifts.

**Remaining differences: 0 H, 1 M, 6 L.**
- **Screen fixes, 5:**
  - M: instrument Qualification rows need `gap 2` + `divided`.
  - L: instrument events 27 vs 29 px.
  - L: instrument panel padding 2 vs about 3.
  - L: receipt Condition columns unequal; use two `stretch(1)` `tracks`.
  - L: the receipt "Sample" heading is 13 vs 14 px.
- **Framework gaps, 1** (it appears on two screens, e-signature and instrument):
  - L: `keyValue` has no caption-column width. The design sets 96 px or 130 px; ui/1 sizes the column to the widest caption.
- **Not counted:**
  - samples head 70 vs 54 (the known head delta);
  - audit rail 210 vs 220 at 1215 (an accepted trade-off);
  - receipt focus halo (a grab artifact);
  - two 1000-px fit wraps: the receipt notice, and samples' "· L-…".

**1000-px check:** no clipping, no overlap, no lost content and no mid-word break on any of the five screens.

**Possible library finding: inferred, not verified; for the dispatcher to decide.** A `grid` with `columns: 2` and no
tracks gives its columns unequal widths on Qt. Column 1 is wider in proportion to its content: a 1.27 ratio, the same
at 1215 and at 1920 (measured on receipt). Several specs say otherwise:
- `docs/spec/tui/frontend.md` says the terminal draws "equal columns".
- `backend_contract.md` says "columns sharing the width".
- The design reading of "columns" is CSS `1fr`.

If equal columns are intended, the Qt `MorphGrid`, which is a `GridLayout` without tracks, diverges from the terminal. I
measured only the pixels. I did not read `MorphGrid.qml` and did not run the terminal.
