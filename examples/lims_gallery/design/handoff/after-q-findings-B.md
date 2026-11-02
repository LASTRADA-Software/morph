# After batch Q: stability, plate, results, review, console (findings B)

**Method.** I read every `app-q/` grab for these five screens (1000, 1215, 390; 1920 for stability and review), set
each against its `app-p/` grab and the mockup render. Pixel values are PIL measurements on those PNGs (helper:
`$S/qrB/m.py`). Causes marked *inferred* come from reading `lg-fold-q` sources; nothing was built or run.

**The 1000 px check (clipping, mid-word breaks, lost content).**
- stability: none. Values break only between words; the conditions heading and two condition labels wrap (fit).
- plate: none. The head subtitle wraps onto two lines between words (fit). Rings touch (accepted A9, unchanged: row A
  pixel runs identical to P).
- results: none. "Assay, prep / 1" wraps between words (fit, carried).
- review: none beyond accepted A1 (Analyte and Evaluation elide). "Approving needs your / signature" wraps again
  (P had it on one line). Word boundary, so it is fit and not counted.
- console: **closed**: "Assay · / 95.0–105.0" is whole (P: "105. / 0"). Other spec labels wrap between words. Nothing is clipped.

**The 1920 check.** Stability's summary is capped at 1650–1904. The grid and the stepper fill the rest. Review's queue is
capped at 215–499, Change is whole and nothing elides. Both pages look intended, and both are unchanged from P.

## Stability

| width | finding |
|---|---|
| 1000 | Batch Q, **closed** (summary without a pane minimum): panel 758–984, no value breaks inside a word. |
| 1215 | Body at 13 px: "Paracetamol 500 / mg" now wraps as the mockup does. P had it on one line. Not a regression. |
| 1215, 1000 | Remaining, **low, framework gap**: a key-value caption is vertically centred on a two-line value ("Product", "First pull", "Containers"). In the mockup the caption sits on the value's first line. `MorphKeyValue.qml` sets no `Layout.alignment`, so the caption is centred, and `KeyValueOptions` has no key for this. Present in P too; not reported before. |
| 390 | Remaining, **low, framework gap**: Back 16–80 (65 px) and Continue 85–373 are 4 px apart. The mockup has an 8 px gap and a 90 px Back. The stepper footer's `row(…, {.gap = 1})` is fixed in `stepperParts` (`author.hpp`), and no option reaches it. Unchanged from P. |
| all | Regressions: none. |

## Plate

| width | finding |
|---|---|
| 1215 | **Regression, medium, screen fix** (*inferred*): the head subtitle wraps to "ELISA, host cell protein · worksheet / WS-2026-0415". The head grows from 52 to 57 px. P and the mockup have it on one line. Cause: the 13 px body plus the shell's user name (not in the mockup) beside a 185 px template select and two buttons. Fix: narrow the template head tool (`plate.cpp:555`, for example `.sizing(ui::fixed(n))`; the mockup's select clips its own label at about 200 px), or shorten the subtitle. Not verified. |
| 390 | Batch Q, **closed** (S2): well 20–55, "A1" ink from 65 (about 8 px of layout gap; the mockup has 10). |
| 390 | Batch Q, **closed** (S9/Q5): rules at 414, 459, 504, 549 … 684, a 45 px pitch for Row A→B and B→H (P: 50). |
| 390 | Batch Q, **closed** (S3/Q1): Auto-fill 16–190 and Save layout 199–373, 175 / 175 px, 8 px apart (P: 182 / 173, 4 apart). |
| 1000 | Accepted A9 (rings touch). Unchanged. |
| 1215 | Accepted A8 (legend chips). Not counted. |
| 390 | Remaining, **low, data** (carried): A3–A8 is one line, and rows D–H are listed separately. |

## Results

| width | finding |
|---|---|
| 390 | Batch Q, **closed** (S3/Q1): Previous / Next and Save draft / Submit are 16–190 and 199–373, equal at 175 px and 8 apart (P: about 170 / 186, 4 apart). |
| 1215 | Footer and dialog buttons at 13 px, as in the mockup. The dialog is 444 px wide against 440. No change in fit. |
| 1215 | Remaining, **low, cause not traced**: table rows are 42 px (182, 224, 266 …) against the mockup's 39 (246, 285, 324). Unchanged from P; not reported before. |
| 390 | Remaining, **low, data** (carried): "of 2" (the mockup has 8). |
| all | Regressions: none. |

## Review

| width | finding |
|---|---|
| 1215 | Batch Q, **closed** (S5/Q4): the comment-card title's darkest ink is (28, 30, 33), the text colour (P: (77, 84, 97)), and it is strong. |
| 390 | Batch Q, **closed** (S3, 1:1:2): Return 16–103 (88), Reject 112–197 (86), Approve 206–373 (168), 8 px gaps. Return is raised 2 px to its label. The caption ink is unchanged: "Return…" is 53 px (33–86). |
| 1215 | **Regression, medium, screen fix** (*inferred*): Analyte elides "Assay, me…" (ink 611–681). P had "Assay, mean" whole (to 683), and so does the mockup. The 13 px Result, Spec. and Prior lot columns widen by about 4 px. Analyte and Change are both `shrink`, and Evaluation keeps about 10 px of slack. Fix: drop `.shrink` on Analyte (`review.cpp:531`) so that Change gives first. Not verified. A1 accepts the elision at 1000 only. |
| 1000 | "Approving needs your signature" wraps again (fit, not counted). |
| 390 | Remaining, **low, framework gap** (*inferred*): the shell's title "Assay and impurities · Paracetamol 500 mg" runs to x = 382. Its column ends at 374, so the text eats 8 px of the 16 px gutter. The title neither wraps nor elides, and nothing is reported. Same in P. |
| all | Remaining, **low, data** (carried): 7 results, not 24. |

## Console

| width | finding |
|---|---|
| 1000 | Batch Q, **closed** (Q6b): "95.0–105.0" is whole. |
| 1215 | Batch Q, **closed** (Q1, 17:10): the panels are 231–837 and 842–1198, 607 / 357 px (expected 606 / 357). At 1000 they are 472 / 277. |
| 390 | Batch Q, **closed** (Q3/S8): "Open run" ink is 58 px wide (166–223), against 50 in P. It is now 14 px. |
| 1215 | **Regression, low, screen fix**: the result inputs are 70 px (996–1065), against 77 in P and 75 in the mockup. This reopens P's S9. It comes from `fixed(9)` (`console.cpp:676`, commit 32336d246, which was made to stop the 1000 overflow). Fix candidate: a width that gives between 9 and 10 units, such as `stretch()` with `.minWidth(9).maxWidth(10)`. Not verified: the stretch would compete with the label. |
| 1215 | Accepted A3 (head 53 against 49). Not counted. |
| all | Nothing else regressed. The KPI, chart and instrument panels match P. |

## Summary

**Regressions: 3**
- plate 1215: the head subtitle wraps and the head grows by 5 px. **Medium, screen fix.**
- review 1215: Analyte elides "Assay, me…", where P and the mockup have it whole. **Medium, screen fix.**
- console 1215: the result inputs are 70 px, against 77 in P and 75 in the mockup. **Low, screen fix.** This is a
  trade-off for the 1000 fit.
- Not counted (fit at 785 px): review 1000's footer caption wraps again.

**Batch Q items on these screens: 10 closed, 0 open.**
- stability: the summary has no floor and no mid-word break.
- plate: S2 well to label; S9/Q5 45 px rows; S3 + Q1 pair.
- results: S3 + Q1 for both pairs.
- review: S5/Q4 title colour; S3 + 1:1:2 decision row.
- console: Q6b "95.0–105.0" whole; Q1 17:10; Q3 "Open run" at 14 px.

**1000 px breakage: none.** No clipping, no mid-word break and no lost content on any of the five screens. **1920:** the
capped panes stop where they should.

**Remaining differences: 8, plus data.** 0 high, 2 medium, 6 low.
- **Medium, screen fix (2):**
  - plate 1215: the subtitle wraps.
  - review 1215: Analyte elides.
- **Low, screen fix (1):** console 1215: the input width.
- **Low, framework gap (3):**
  - keyValue centres a caption on a multi-line value; it should sit on the value's first line (stability).
  - the stepper footer's 4 px gap cannot be set (stability 390).
  - the shell title runs into the gutter and the overflow is not reported (review 390).
- **Low, cause not traced (1):** results' table rows are 42 against 39 px.
- **Low, accepted or unchanged (1):** plate 1000's rings touch (A9).
- **Low, data (counted as 1):** plate's row grouping, results' "of 2", review's 7 against 24.

The three framework gaps are in `include/morph`/`src`, so the filing rule in AGENTS.md applies to them. I did not
file them: this review is read-only. The keyValue alignment and the shell title overflow were present in P as well.
