# After batch R: stability, plate, results, review, console (findings B)

**Method.** I read every `app-r/` grab for these five screens: 1000, 1215 and 390, plus 1920 for stability and review.
I set each grab against its `app-q/` grab and the mockup render. A pixel diff between Q and R located every change.
Unchanged, with no pixel over a difference of 30: plate 1000 and 390, all three results grabs, console 390 and
review 1920. Pixel values come from PIL reads of the PNGs, using `$S/qrB/m.py`. Causes marked *read* come from
`git show lg-fold-r:` sources. Nothing was built or run.

**The 1000 px check (clipping, mid-word breaks, lost content): none on any of the five screens.**
- Stability: the conditions heading, two condition labels and the summary's values wrap between words.
- Plate: the head subtitle wraps under the title, as in Q. The rings still touch (accepted A9).
- Results: "Assay, prep / 1" wraps between words. The dialog covers the grid, as in Q.
- Review: Analyte and Evaluation elide (accepted A1). "Approving needs your / signature" wraps (accepted, fit). Every
  queue entry is whole, "2 h 10 min" included.
- Console: "Total / impurities · / ≤ 0.50" wraps between words. "95.0–105.0" is whole.
- Console 1000 and 1215: the chain of custody runs past the 830 px viewport, inside the pane's scroll, as in Q.

**The 1920 check.**
- Stability: the summary is capped at about 1650–1904, close to the mockup's 250 px. The grid and the stepper fill the
  rest.
- Review: the queue is capped at 215–499, and Change is whole ("0.12 → 0.21 · integration corrected"). The grab is
  identical to Q.
- Both pages look intended.

## Stability

| width | finding |
|---|---|
| 1215, 1000, 1920 | **R4: closed.** Each caption's top ink now sits on its value's first line. Product: caption 138, value 138 (Q: 147 against 138). First pull: 250 against 252 (Q: 256 against 248). Containers: 315 against 315 (Q: 319 against 312). |
| 390 | **R3/T6: closed.** Back is 16–80 and Continue 89–373: 8 px apart (Q: 4). Back is still 65 px (accepted A12). |
| 1215 | Footer: "Save draft" moved 4 px left, so its ink ends 12 px before Continue (Q: 8). The mockup has about 14. This moves toward the mockup, so it is not a regression. |
| 1215, 1000 | Remaining, **low, cause not traced**: the conditions table's header row has a grey band. The mockup's stability header is white above a rule; review's and results' mockup headers are grey. |
| all | Remaining, **low, cause not traced**: the date input draws a "▦" picker glyph. The mockup's date field has none. Present in Q. |
| all | Regressions: none. |

## Plate

| width | finding |
|---|---|
| 1215 | **T1: closed.** The subtitle "ELISA, host cell protein · worksheet WS-2026-0415" is on one line. The head measures 52 px again (Q: 57). The select elides its own label to "Template: HCP 96, 8 sta…"; the mockup clips it at "8 standa". |
| 1215 | Remaining, **low, framework gap** (*inferred*; measured, cause not read to the line): the head's select, Auto-fill and Save layout are 4 px apart (select to 884, Auto-fill 889–1015, Save 1020). The mockup has 8 (778→786, 916→925). These are shell head tools and actions (`screen.tool`/`screen.action`). They do not use `actionBar`, so `actions.gap` does not reach them, and no screen key spaces them. |
| 390 | Remaining, **low, cause not traced**: the "Assign…" buttons end at x 369. The row's rule ends at 373. The mockup puts them flush with the row's end. Present in Q. |
| 1000 | Accepted A9 (rings touch); the grab is identical to Q. |
| 390 | Remaining, **data** (carried): A3–A8 is one line, and rows D–H are listed separately. |
| all | Regressions: none. |

## Results

| width | finding |
|---|---|
| all | The grabs are identical to Q. The phone pairs were already 8 px apart in Q, and they still are as `actionBar`s (T5). |
| 1215, 1000 | Remaining, **low, screen fix**: the expanded footer's Save draft and Submit for review are 4 px apart (Save to 1061, Submit from 1066). The mockup has 8 (to 879, from 888). `results.cpp:566-569` is a `ui::row(…, {.gap = 1})`. Fix: `ui::actionBar({oos message (stretch), saveButton(), submitButton(sheet)})`. The dialog's footer is already 8 px (Undo to 682, Confirm from 691). |
| 1215 | Accepted A13: rows are 42 against 39 px. |
| 390 | **Data** (carried): "of 2" (the mockup has 8). |
| all | Regressions: none. |

## Review

| width | finding |
|---|---|
| 1215 | **T2: closed.** "Assay, mean" is whole, where Q had "Assay, me…". Every queue entry is whole. Change still elides ("0.12 →…"); the mockup also cuts Change at its 1000 px page. |
| 1000 | **T2 check:** A1 is unchanged (Analyte and Evaluation elide), and every queue entry is whole. The queue is 8 px narrower. Its self-review note now wraps to three lines (fit). |
| 390 | **R2/T8: closed, as accepted A14.** The right ink of the eyebrow is 362, the title 355 and the tags 260, all within 374. The title wraps "… 500 / mg", and "Suitability passed" takes a line of its own. The head grows by about 46 px. Accepted; not counted. |
| 390 | Decision bar: an `actionBar` (T5). Its pixels are unchanged from Q, at 8 px. |
| 1215, 1000 | Remaining, **low, screen fix**: the expanded footer's Return to analyst… (497–639) and Reject… (644–725) are 4 px apart. The mockup has 8 (to 485, from 494). `review.cpp` `worksheetPane` is a `ui::row(…, {.gap = 1})`. Fix: make it a `ui::actionBar` that keeps the stretch caption. Present in Q. |
| all | **Data** (carried): 7 results, not 24. |
| all | Regressions: none. |

## Console

| width | finding |
|---|---|
| 1215 | **T3: closed.** The result inputs are 989–1065, 77 px (Q: 70; mockup: 75). All four right edges are at 1065. |
| 1000 | **T3 check:** the inputs are 780–850, 71 px (Q: 70). The right edges line up, and "95.0–105.0" is whole. |
| 1215, 1000 | Remaining, **low, framework gap** (same cause as plate's head, *inferred*): the head's select, the search and Connected are 4 px apart (826→831, 1045→1050). The mockup puts about 9 px between Scan and Connected. |
| 1215 | Note, not counted: the worklist's Status badges elide ("Resu…", "Queu…"). The mockup's worklist cuts its Status column at the pane edge as well. |
| 1215 | Accepted A3: the head is 53 against 49 px. |
| all | Regressions: none. The KPI, chart, instrument and worklist panels match Q. |

## Summary

**Regressions: 0.**
- Not regressions:
  - Review 390's taller head is accepted A14.
  - Stability's Save draft moved 4 px toward the mockup.

**Batch R items on these screens: 7 closed, 0 open.**
- Stability: R4 key-value captions on the first baseline; R3/T6 footer 8 px.
- Plate: T1 one-line subtitle, head 52 px.
- Review: T2 Analyte whole at 1215 (A1 unchanged at 1000, queue whole); R2/T8 head inside 374 (A14).
- Console: T3 inputs 77 px, edges aligned.
- T5 `actionBar`s on results 390 and review 390 hold at 8 px. They are not counted separately; Q already had them at 8.

**1000 px breakage: none.** **1920:** the capped panes stop where they should (stability's summary at about 254 px,
review's queue at 284 px). The rest of each page looks intended.

**Remaining differences: 6, plus carried data and accepted items.** 0 high, 0 medium, 6 low.
- **Low, screen fix (2):** the expanded footers are 4 px apart where the mockup has 8.
  - `results.cpp` and `review.cpp` `worksheetPane`: both are `ui::row(…, {.gap = 1})`.
  - Fix: make each a `ui::actionBar`, now that it mounts in place.
  - These are the same hand-set gap in two screens. Batch R converted only the phone rows.
- **Low, framework gap (1, two screens):** the shell head's tools and actions are 4 px apart, against 8 in the mockup
  (plate and console 1215).
  - They are not an action row, so the theme's `actions.gap` does not reach them, and no key spaces them.
  - Inferred from measurement; the QML that spaces the head slot was not read.
- **Low, cause not traced (3):**
  - stability's grey table header against the mockup's white one;
  - stability's "▦" date-picker glyph;
  - plate 390's Assign… buttons, which end 4 px before the row's end.
- **Carried data:** plate's row grouping, results' "of 2", review's 7 against 24.
- **Accepted, unchanged:**
  - A1 (review 1000 elision);
  - A3 (console head);
  - A9 (plate rings);
  - A12 (stability's Back width);
  - A13 (results' rows);
  - A14 (review 390's head).

The head-spacing gap is in `include/morph`/`src`, so AGENTS.md's filing rule applies once someone confirms it from the
source. I did not file it: this review is read-only.
