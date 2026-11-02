Reviewer A, after batch R: esignature, receipt, instrument, audit and samples, at 1000, 1215 and 390, plus receipt and
audit at 1920. The grabs are in `lg-cmp/app-r/`, and the baseline is `app-q/`. All 17 logs are 0 bytes. Sources were
read with `git show lg-fold-r:<path>` in `morph-lims-e1`.

**Method.** I ran a q→r image diff (PIL `ImageChops` bounding box) on every grab, then read pixels for edges, gaps and
ink extents, and checked them against the mockup PNGs. Crops are in `lgrrA/`. *Measured* means a pixel read. *Inferred*
means read from code or reasoned.

The diff found no change at all on audit 1000, 1215 and 1920, on samples 1000 and 1215, or on esignature 390.

## E-signature

| Width | Finding |
|---|---|
| all | **Regressions:** none. Only the record box's caption column moved, by 1 px. |
| 1215/1000 | **R4, captions on the value's first baseline: closed (measured).** The caption and value ink bottoms are now 261/285/311 for both. At Q the captions were at 262/286/312, against values at 261/285/311. |
| 1215/1000 | Cancel and Sign and approve are 8 px apart (695→704). |
| 1000 | No clipping, no overlap, no mid-word break. |
| — | **Remaining:** none. The 24 px kv pitch is accepted. |

## Receipt

| Width | Finding |
|---|---|
| all | **Regressions:** none. At 1215, the form differs from Q only by sub-pixel glyph shifts in the head and the sample heading (from the whole-pixel text runs). |
| 1000 | **T9 / R5 / R1: closed (measured).** The field grid has 2 columns (244–475, 483–714), and the condition grid has 1 column. "Release, finished product" and "Label legible and matches the request" are both whole, with no "…". The session pane is 736–983 (248 px), and every Received/Rejected badge is whole (902–962). The notice wraps at a word ("Nothing was / changed."). No clipping. |
| 1215 | **T9, Time column: closed (measured).** Time is 908→970, 62 px. It was 55 at Q, and the design has 70. The form is unchanged from Q: 3 field columns and 2 check columns. |
| 1920 | **Closed (measured).** The session pane is capped at 1605–1903 (299 px, against the design's 298). The form fills the rest, with 3 field columns and 2 check columns. This looks as intended. |
| 390 | **R2, compact head ends in a framed element: closed (measured).** The count badges now end at x≈375, a 15–16 px inset. At Q they ended at ≈387. |
| 390 | **Open, L, screen: Reject… and Receive, next are still 4 px apart** (16–123 and 128–373). The design has 8–9 (1214→1223). The pinned footer is a hand-written `ui::row(..., {.gap = 1})` (`receipt.cpp:468-470`), and T5 did not list it. **Fix:** make it a `ui::actionBar` that keeps `fixed(14)`/`stretch()` on its children. |
| 1215 | **Remaining, L, screen:** the Outcome badges end 72 px from the pane's edge, where the design has 33. The Sample column is 96 px (970–1066), where the design has 126, and the stretch Outcome column takes the rest. **Fix:** a wider Sample column (`fixed(11)` → about 14), or let Outcome fit its content and Sample stretch. |

## Instrument

| Width | Finding |
|---|---|
| 390 | **Regression, L: the tags now wrap.** "Calibration due in 3 days" moved to a second line, and the head is 22 px taller. Everything below it is identical to Q, shifted by 22 px (measured by an offset diff). At Q the tag row ran 56–378 on one line, which needs 322 px. The text box now ends at 374 to keep the 16 px end inset (R2, `fb5c73663`), which leaves 318 px. The design fits all three tags on one line in its 356 px box, because its phone head has no menu glyph. This is the same mechanism as A14 (review 390): an R2 side effect, and inferred from the measurements. Nothing on the screen can fix it without shortening the tag text. **Recommend extending A14 to it.** |
| 390 | **T5, Book, Log maintenance and ··· 8 px apart: closed (measured).** They sit at 16–164, 173–321 and 330–373, with gaps of 8 and 8. Book and Log are 149/149 px, against the design's 148/148. |
| 1215/1000 | **R4: closed (measured).** The Details and Recent events captions share their value's first baseline. At 1000, event 1 wraps "Run / held.", and its caption sits on the first line (both ink tops at 370). |
| 1000 | No clipping. "next 14-Jan-2027" is whole. |
| — | **Remaining:** none new. The 23 px kv pitch is accepted. |

## Audit

| Width | Finding |
|---|---|
| 1000/1215/1920 | **Regressions:** none. These grabs are pixel-identical to Q, so T4 kept `padding(3, 2)`, and the 8 px rail inset is accepted. |
| 390 | **T5, Filters · 4 and Export CSV 8 px apart: closed (measured).** They sit at 16–190 and 199–373, against 4 px at Q. Export PDF keeps the 16 px end inset (R2). |
| 1000 | No clipping. The dates and times beside the calendar glyph are whole. Reason wraps at words. "24- / Oct-2026" breaks at its hyphen, as in the mockup. |
| 1920 | The rail is capped at 216–438, and the table fills the rest. This looks as intended. |
| — | **Remaining:** none beyond the accepted items (rail 210, 8 px inset, data, A11). |

## Samples

| Width | Finding |
|---|---|
| 1000/1215 | **Regressions:** none. These grabs are pixel-identical to Q. |
| 390 | **T5, Filters · 2 and Sort 8 px apart: closed (measured).** They sit at 16–190 and 199–373. |
| 390 | **Open, L, screen: the bulk bar's Assign to worksheet and ··· are 4 px apart** (169–325 and 330–373). The design has 8 (1412→1421). `compactBulkBar` is a `ui::row` with `.gap = 1` (`samples.cpp:631-643`). "The samples bulk bar" is an accepted item. If that acceptance covers this gap, drop the finding. Otherwise the fix is to wrap the two buttons in a `ui::actionBar`. |
| 1000 | No clipping. The product and tests cells wrap at spaces and at "·". |
| — | Accepted: the bulk bar, the 30 px rows, and the data. |

## Summary

**Regressions: 1.**
- **L, instrument 390:** the tags wrap to a second line, which adds 22 px to the head. This is an R2 side effect of the A14 kind, and I recommend accepting it as A14.

**Batch R items on these screens:**
- **Closed, 9:**
  - R4 on e-signature and instrument;
  - T9 (R5 and R1) on receipt 1000, 1215 and 1920;
  - R2 on receipt 390 and audit 390;
  - T5 on instrument, audit and samples at 390.
- **Open, 2:** two phone button pairs are still 4 px apart, because they were not converted to `actionBar`:
  - receipt's pinned footer, Reject… and Receive, next;
  - samples' bulk bar, Assign to worksheet and ···. This one is possibly covered by the accepted bulk-bar item.

**Remaining differences: 0 H, 0 M, 3 L, all screen fixes.**
- receipt 390, footer gap: use `actionBar`;
- samples 390, bulk bar gap: use `actionBar`;
- receipt 1215, Outcome badges 72 px from the edge, against 33: widen Sample or stretch it.

There are no framework gaps beyond A14, which applies to the instrument item.

**1000-px check:** no clipping, mid-word breaks, overlap or lost content on any of the five screens. Receipt's two cut
values from Q are now whole.

**1920:** the receipt session pane (299 px) and the audit rail (222 px) stop at the mockup width. The rest of each page
fills as intended.

**Not verified:**
- I built and ran nothing.
- I did not run the hand check from T10 (select, then right-click, in an input).
- The cause of the instrument tag wrap is inferred from the inset arithmetic. I did not trace it in the R2 code.

**What would change the verdicts:**
- If accepted item A14 is meant to cover only review, the instrument wrap stands as a regression.
- If "samples' bulk bar" was accepted including its gap, the samples item drops out.
