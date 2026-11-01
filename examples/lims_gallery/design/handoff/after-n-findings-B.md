# After batch N: stability, plate, results, review, console (findings B)

New grabs are in `lg-cmp/app-n/` (the brief says `app-l`, but `app-l` holds the after-L grabs; `app-n` is dated 15:17
and has the 1920 grabs). I compared them with `lg-cmp/app-l/` and with the mockups (renders and `.dc.html`). Pixel
values were measured with PIL. Causes marked *read* come from the source in `/home/yaraslau/repo/morph-lims-e1`
(`lg-fold-n`); nothing was built or run.

**The 1000 px check.**
- stability, plate, results, console: nothing clipped, no mid-word break, no lost content.
  - Console's analyte labels now wrap at word boundaries ("Assay · / 95.0–105.0", "Total / impurities · / ≤ 0.50"),
    because the unit is now its own column. This is a fit difference at 785 px, not breakage.
  - Console's Status badges elide ("Resu…", "Queu…"). This is batch N's badge elision, working as designed.
- **review: lost content (regression, below).** Analyte reads "Impu…" and "Assa…", Spec reads "≤ 0.2…" and "95.0…",
  Evaluation reads "Out of sp…" and "Out of tre…", and Change reads "0.12 …". In L, only Change elided.

**The 1920 check.**
- The capped panes stop where they should:
  - stability's summary: 1650–1904, so 254 px;
  - review's queue: 216–499, so about 284 px (37 units).
- Stability's page looks intended. The fields and the table stretch, and the checkbox columns sit at the right.
- Review does not look intended: the Change cell elides ("0.12 → 0.21 · integration co…") while Evaluation keeps
  about 560 px empty. See review below.

Measured pane widths:

| pane | 1000 | 1215 | 1920 | target |
|---|---|---|---|---|
| stability summary | 196 | 252 | 254 | ~250 |
| review queue | 220 | 280 | 284 | 280 |

## Stability

**1215**
- Regressions: none.
- Batch N, closed:
  - the summary is `percent` with a max, now 252 px (it was 200 in L);
  - the head is 85 px, matching the mockup's 84.
- Batch M: nothing specific to this screen.
- Remaining:
  - low, screen fix: First pull still breaks badly. It now reads "03-Dec-2026 (40 °C, 1 / m)", splitting "1 m", and
    the same happens at 1920. In L it read "(40 / °C, 1 m)". The cause is that the label column is about 73 px
    against the mockup's 110, so the value line is long enough to break inside the parenthetical. Fix: U+00A0 in
    "40 °C" and in "1 m", or the date and the parenthetical as two texts. At 1000 the break is correct:
    "03-Dec-2026 / (40 °C, 1 m)".

**1000**
- Regressions: none. Nothing is clipped. The Containers hint takes 3 lines (fit).

**390**
- Regressions: none.
- L item now closed: the check-box rows are 44 px apart (L 48, mockup 42).
- Remaining:
  - low, screen fix: the condition panels are 4 px apart, against 12 in the mockup (panel bottoms at 517, 567 and 617;
    next tops at 522 and 572). Fix: a larger `gap` on the compact column that holds the panels.

## Plate

**1215**
- Regressions: none.
- Batch M, closed: **the well captions are semi-bold/bold again** (per-button caption weight). This closes the L
  regression. The wells now match the legend chips.
- Batch N, closed: the head is 56 px, against the mockup's 55 (L 46).

**1000**
- Regressions: none. Nothing is clipped. The head is 90 px with its subtitle on two lines (fit).

**390**
- Regressions: none.
- Batch M, closed: the STD and QC circles are bold.
- Remaining:
  - low, screen fix: the compact well rows are 57 px apart with 44 px wells, against the mockup's `.wl` (min-height
    44, wells 36 px; rows 45 px apart). Fix: a smaller well size and less row padding in the compact list.
  - low, data (carried over): Rows D–H are listed separately, and A3–A8 is one line.

## Results

**1215**
- Regressions: none.
- Batch M, closed:
  - **the shadow's fade.** Below the dialog, the shade now runs (139,141,146) → (167,169,173), against the mockup's
    (139,140,146) → (167,169,173). It is a near-exact match; L was bluer, starting at (134,137,151).
  - **`color.dialogRule`.** The dialog's rule is (230,233,237), exactly the mockup's. L had (237,240,243).
- Remaining:
  - low, screen fix: the Result inputs are 92 px wide (L 77, mockup 110). Fix: widen the Result column's `fixed(...)`
    again.
  - low, screen fix (carried over): Submit for review is disabled under the dialog; the mockup shows it enabled.

**1000**
- Regressions: none. The dialog overlaps the table by design.

**390**
- Batch M, closed: **the L regression.**
  - There is no Undo or Confirm in the compact layout.
  - Submit for review is green.
  - The gap under the comment box is gone: about 15 px to "Impurity A", against about 22 in the mockup.
- Regressions: none.
- Remaining:
  - low, data: "of 2" (the mockup has 8), so Next is disabled.

## Review

**1215**
- **Regression, medium: worksheet cells elide that were whole in L.** Analyte reads "Impurit…" and "Assay, …", and
  Spec reads "95.0–1…".
  - Cause (*read*, `review.cpp:526-547`): Evaluation became `stretch(3)` with `shrink`. Analyte and Spec are
    `stretch()` with `shrink`, so they give up width first, while Evaluation's tags sit in about 215 px with room to
    spare.
  - Screen fix: make Analyte and Spec `ui::content()`, and let Evaluation and Change be the shrinking columns. The
    mockup keeps Analyte and Spec whole and lets Change run off the pane's edge.
- Batch N, closed:
  - **the queue is 280 px** (`percent(28)`, max 37). The ages sit beside the IDs, and the tags are on one line. The
    self-review note takes 2 lines, as in the mockup. The placeholder is whole.
  - **the queue title is the `heading` role** ("Awaiting review · 4").
  - **the approval note ends beside Approve** (`align`).
  - **no stray table scroll bar.**
  - **the Evaluation cell is one full-text tag.** Rows are 30 px, as in the mockup (L: 48–56).
- Batch M, closed: **`color.placeholder`.** The placeholder's darkest pixel is (132,133,135), identical to the
  mockup's. L had (77,84,97).
- Remaining:
  - **medium, framework gap: stretch columns split by weight and ignore their content.**
    - At 1920 the Change cell is about 180 px and elides "…integration co…", while the Evaluation column, which
      takes 3 shares, is about 560 px with most of it empty. The ratio, 558 to 180, is about 3:1, so the width goes by
      the weights alone (*inferred* from measurement).
    - Missing: a stretch column that starts from its content width and shares only the remainder (CSS `auto`
      columns), or a shrink priority between columns.
    - Screen-only mitigation: weight Change and Evaluation more evenly. That trades one elision for the other at
      1215.
  - low, screen fix: the self-review note ("You entered these results…") is body size and dark. The mockup draws it
    at 12 px, muted. Fix: the `muted` or small text role.
  - low, data: "7 results" and "All 7" (the mockup has 24).

**1000**
- **Regression, medium (lost content):** the same columns elide much further. See the 1000 check above.
  - The worksheet lost about 60 px to the wider queue (220 against 161). Analyte, Spec and Evaluation then shrank.
    Evaluation's badges elide too ("Out of sp…").
  - The same fix applies: content-width Analyte and Spec, with Evaluation and Change shrinking. Counted once with the
    1215 regression.
- The approval note wraps to 2 lines, end-aligned, beside Approve. This is acceptable at 785 px.

**390**
- Regressions: none.
- L item now closed: the gap from the last result to "Review comment" is about 18 px (L 45, mockup 27).
- Batch M, closed: the placeholder colour.
- Remaining:
  - low, screen fix: the placeholder and the comment are 13 px; the mockup's compact textarea is 14 px.
  - accepted (carried over): "Prior lots", and the back link under the tags.

## Console

**1215**
- **Regression, low: Impurity B's result input sits 3 px left of the others.** Its left edge is at 1008, against
  1011 for the other lines; in L all four were at 1039.
  - Cause (*inferred*): the "Out of spec" tag is wider than its `fixed(9)` (`console.cpp:681`), so that line's
    input moves left.
  - Screen fix: `fixed(10)`. If a `fixed` child is meant to hold its width, this is instead a framework sizing gap.
    I did not verify which.
- Batch M (screen fixes from the L list), closed:
  - **Instruments stretches** to the chart's height, ending at about 490 like the chart card.
  - **the units sit beside the inputs**, as their own column.
- Batch N, closed:
  - **the Status badges elide** within their narrow column ("Resu…", "Queu…", "On h…"). The mockup cuts them at the
    pane's edge. The intent matches, and nothing scrolls.
  - **`head.paddingY`:** the head is 55 px, against the mockup's 49.
- Remaining:
  - low, theme or screen: the head is about 6 px taller than the mockup's. Its 32 px controls plus 12 px of padding
    on each side give 56. The mockup's 30 px select with about 10 px of padding gives 49.

**1000**
- Regressions: none beyond the 3 px input offset, which shows here too (793 against 797).
- The labels wrap more than in L, because the unit column takes room (fit, see the 1000 check). Nothing is clipped.

**390**
- Regressions: none.
- L item now closed: the due times read "due 10:30".

## Summary

**Regressions: 2**
- **review, medium (1215 and 1000):** Analyte and Spec elide ("Impurit…", "95.0–1…"; at 1000 also Evaluation,
  "Out of sp…").
  - Cause: Evaluation is `stretch(3)` with shrink, and Analyte and Spec shrink first.
  - Screen fix: Analyte and Spec `content()`.
- **console, low (1215 and 1000):** the out-of-spec line's input is 3 px out of column.
  - Cause: the tag is wider than its `fixed(9)`.
  - Screen fix: `fixed(10)` (*inferred*).

**Batch M and N items visible on these screens: all closed. 0 open.**
- M:
  - the plate wells' caption weight (the L regression);
  - the shadow's fade (results);
  - `color.dialogRule` (results);
  - `color.placeholder` (review);
  - screen fixes for results 390 (Undo and Confirm gone, Submit green), review (one-tag evaluation, title role) and
    console (Instruments stretch, unit column, "due").
- N:
  - `percent` panes with a max: stability's summary is 252 px at 1215 and 254 at 1920; review's queue is 280 and 284;
  - the badge elides in a narrow column (console);
  - `align` on text (review's approval note);
  - no stray table scroll bar (review);
  - `head.paddingY` (plate 56 against 55; stability 85 against 84).

**Remaining differences: 10** (0 high, 1 medium, 9 low).
- Medium, framework gap (1): review's stretch columns split by weight alone.
  - At 1920, Change elides while Evaluation has about 560 px, most of it empty.
  - Missing: content-based stretch (CSS `auto` columns) or a shrink priority.
- Low, screen fixes (7):
  - stability: First pull's break (U+00A0); the compact panel gap of 4 against 12 px;
  - plate: the compact row and well size (57 and 44 against 45 and 36 px);
  - results: the Result input width (92 against 110 px); Submit enabled in the mockup;
  - review: the self-review note's role;
  - console: the head is about 6 px taller.
- Low, other (2):
  - review's compact textarea font, 13 against 14 px;
  - the data items, counted together: plate's row grouping, results' "of 2", review's 7 against 24 results.
