# After batch O: stability, plate, results, review, console (findings B)

**Method.** I read every grab in `lg-cmp/app-o/` for these five screens (1000, 1215 and 390, plus 1920 for review and
stability) and set each against `app-n/` and the mockup renders. Pixel values come from PIL on those PNGs.

**Sources.** Code was read from `lg-fold-o` (`74bb31a79`) with `git show`. The `morph-lims-e1` working tree has a
detached HEAD at `27926fd2c`, which is older than the branch tip, so its files are not batch O's. Causes marked *read*
come from reading the code, not from a run. Nothing was built or run.

**What changed since N.** A pixel diff against `app-n` shows that results, review and console at 390 are
byte-identical. Every other grab changed, mostly through the 30 px controls.

**The 1000 px check.**
- stability, plate, console: nothing is clipped, nothing breaks mid-word and no content is lost. Console's analyte
  labels wrap at word boundaries, which is a fit difference.
- results: no breakage. Analyte now wraps ("Assay, prep / 1", "Total impurities / calculated") because the Result
  column is wider. This is a fit difference at 785 px, not breakage, and is noted below without being counted.
- **review: content is still lost.**
  - Analyte reads "Impu…" and "Assa…".
  - Evaluation reads "Out of …" and "Out of t…".
  - Change reads "0.12 …".
  - Spec. is now whole; in N it read "≤ 0.2…".
  - This is no worse than N, so it is not a regression. See review below.

**The 1920 check.**
- Stability's summary measures 1650–1904, so 254 px, at its cap. The page looks intended.
- Review's queue measures 215–499, so 284 px, at its cap (`maxWidth(37)`).
- Review now looks intended at 1920. Every column is whole, Change reads "0.12 → 0.21 · integration corrected", and the
  three stretch columns share what is left. This closes N's medium framework gap (stretch columns ignored their
  content).

## Stability

| width | finding |
|---|---|
| 1215 | Batch O, **closed**: "1 m" no longer breaks. |
| 1215 | Remaining, **low, screen fix**: First pull breaks after the comma, "03-Dec-2026 (40 °C, / 1 m)". The mockup breaks before the parenthesis, "03-Dec-2026 / (40 °C, 1 m)". Same at 1920. Fix: a no-break space after the comma too, so the parenthetical is one unit, or the date and the parenthetical as two texts. |
| 1000 | No breakage. The break falls before the parenthesis, as in the mockup. |
| 390 | Batch O, **closed**: the condition panels are 12 px apart. Measured: bottoms at 517 and 575, next tops at 530 and 588. |
| 390 | Remaining, **low, screen fix**: Add condition and the Summary panel are still 8 px apart (633→642), against about 12 in the mockup. Cause (*read*): the outer column is `gap = 2`, and only the `forEach` got `gap = 3`. Fix: `gap = 3` on that column, or the panels, Add and Summary grouped in a `gap = 3` column. |
| all | Regressions: none. Table rows are 30 px against the mockup's 29. |

## Plate

| width | finding |
|---|---|
| 390 | Batch O, **partly closed**: rows are now 45 px apart (rules at 239, 284, 329, 374). The wells are still 44 px against the mockup's 36 (`.wl .well`). With no row padding, each well fills its row: the rings touch the row rules and the neighbouring wells. The selected A3–A8 ring sits on both rules. |
| 390 | Remaining, **medium, framework gap**: the compact well cannot be 36 px. `ButtonSize::Small` "keeps the control height" (`control_shape.hpp:30`), and the compact control height is 44. Missing: a button size or diameter that is independent of `control.height`. I did not verify whether `.sizing(fixed, fixed)` on a round button is honoured; if it is, this becomes a screen fix. |
| 1215 | **Regression, low**: the wells are 30 px (rows 34 px apart), against 32 in N (rows 36 apart). This is a side effect of `control.height: 30`, since the wells are Small buttons. The mockup's `.well` is 40 px. This shares the framework gap above, which also blocks 40 px on the desktop. |
| 1215 | Head: 53 px, against 55 in the mockup and 55 in N. Fine. |
| 1000 | No breakage. The selected rings in A3–A8 touch each other (fit). |
| 390 | Remaining, **low, data** (carried over): rows D–H are listed separately, and A3–A8 is one line where the mockup has A3, A4 and A5–A8. |

## Results

| width | finding |
|---|---|
| 1215 | Batch O, **closed**: the Result inputs are about 108 px (705–812), against the mockup's 110. Rows are 42 px against 39 (N had 44). |
| 1215 | Remaining, **low, screen fix** (carried over): Submit for review is disabled under the dialog; the mockup shows it enabled. |
| 1215 | Remaining, **low** (carried over): the dialog's comment breaks at the hyphen, "SOP- / CHR-012". The mockup wraps before "SOP-CHR-012". Fix: U+2011 in the code. |
| 1000 | Fit note, not counted: Analyte now wraps to two lines. This is the cost of the 14-unit Result column at 785 px. Nothing is clipped. |
| 390 | Unchanged from N. Remaining, **low, data**: "of 2" (the mockup has 8). |
| all | Regressions: none at 1215 or 390. |

## Review

| width | finding |
|---|---|
| 1215 | Batch O, **closed**: the N regression. Analyte, Result, Spec. and Prior lot are whole, and the Evaluation tag "Out of trend: −0.7 % vs. last 5 lots" is whole. Change elides ("0.12 …"); the mockup cuts it at the pane edge. |
| 1215 | Batch O, **closed**: the queue is 27 %, so 270 px. The mockup's queue is about 280 px; batch O narrowed it on purpose so the tag stays whole. Accepted trade, not counted. |
| 1920 | Batch O, **closed**: N's framework gap. A stretch column starts from its content, and nothing elides. |
| 1215 | Remaining, **low, screen fix**: the comment card sits about 1 px under the table; the mockup leaves about 11. Cause (*read*): the `room` spacer is only between the comment cards and Review comment. Fix: the same spacer, or a page gap, between `resultTable` and `commentCards`. |
| 1215 | Remaining, **low, screen fix** (carried over): the self-review note is the body size. The mockup draws it at 12 px. Fix: the 12 px `caption` size, kept muted (`.role = "caption"` if that role exists). |
| 1000 | Remaining, **medium, lost content** (not a regression; no worse than N): Analyte and Evaluation elide. The worksheet is about 550 px, so the tag cannot fit beside the fixed and content columns. Batch O chose elision over a sideways scroll. Missing for a real fix (weak, *inferred*): a column priority, so Change and Prior lot drop when the table is short. A screen-level option is to let Evaluation wrap (`.wrap = true`), if a tag cell wraps (not verified). |
| 390 | Unchanged from N. Known open, **low, framework gap**: the textarea's text is 13 px against 14 (a text input has no size option). Accepted and carried over: "Prior lots", and the back link under the tags. |
| all | Remaining, **low, data**: "7 results" and "All 7" (the mockup has 24). Regressions: none. |

## Console

| width | finding |
|---|---|
| 1215 | Batch O, **closed**: N's regression. All four result inputs start at x = 1004 (N: 1011, 1011, 1008, 1011). |
| 1215 | Head: 53 px, against 55 in N and 49 in the mockup. **Low, theme or screen**: the remaining 4 px is `head.paddingY` 12 against about 10. Fix: `head.paddingY: 10`, which affects every head; check plate and stability, which match today. |
| 1215 | Remaining, **low, screen fix**: the result inputs are about 62 px against the mockup's 75 (816–891). Fix: widen the input column's `fixed(...)`. |
| 1215 | Remaining, **low, screen fix**: the Instruments rows are 22 px apart against 26 in the mockup. Fix: a larger `gap` in that list. |
| 1000 | No breakage. The analyte labels wrap at word boundaries (fit). Chain of custody continues below the fold, as in N. |
| 390 | Unchanged from N. No findings. |
| all | Regressions: none. |

## Summary

**Regressions: 1**
- plate 1215, low: the wells shrank from 32 to 30 px with `control.height: 30`. The mockup has 40. Cause: the wells
  are Small buttons, which keep the control height.

**Not counted:**
- results 1000: Analyte wraps, a fit difference at 785 px.
- review 1215: the queue is 270 against 280 px, a deliberate trade.

**Batch O items visible on these screens: 9 closed, 1 partly closed.**
- Closed:
  - stability: "1 m" unbroken; phone panel gap 12;
  - results: inputs about 108 px;
  - review: tag whole at 1215; no sideways scroll at 1000; queue 27 %; stretch columns start from content, which also
    closes N's 1920 gap;
  - console: inputs aligned;
  - controls 30 px.
- Partly closed: plate's phone rows are 45 px, but the wells stay 44 px and fill the rows. That is the medium framework
  gap below.

**Remaining differences: 13** (0 high, 2 medium, 11 low).

- **Medium (2):**
  - plate 390, framework gap: a well cannot be smaller (or, on the desktop, larger) than `control.height`.
  - review 1000: Analyte and Evaluation elide. It is a framework gap (column priority, weak evidence) or accepted at
    785 px.
- **Low, screen fixes (8):**
  - stability: First pull's break point; the 8 px gaps around Add condition.
  - results: Submit disabled under the dialog; the hyphen break in "SOP-CHR-012".
  - review: the gap from the table to the comment card; the self-review note's size.
  - console: the input width; the Instruments row pitch.
- **Low, theme (1):** console's head is 4 px taller (`head.paddingY`).
- **Low, framework gap (1):** review's compact textarea is 13 against 14 px (known open).
- **Low, data (counted as 1):** plate's row grouping, results' "of 2", review's 7 against 24 results.

The wells' desktop size (30 against 40 px) is the plate regression above. It has the same cause as the medium plate
gap, so it is counted there, not again here.
