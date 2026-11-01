Reviewer A, after batch K: esignature, receipt, instrument, audit and samples, at both widths. I compared each grab with
the mockup render and source, and with the app-j grabs. Colours and positions were measured with PIL on the PNGs;
anything else is marked as read or inferred. Worktree `lg-fold-k` at `c6957e257`.

Every batch K item that shows on these screens is closed. There are four low regressions, three from K-S's column
re-fits and one from K12's compact bar. Two K-S screen items are not achieved: audit's Reason column is no wider, and
samples' rows still wrap.

Logs: all 20 `app-k/*.log` are 0 bytes. The grab command in `visual-review.md` now sets
`QT_FORCE_STDERR_LOGGING=1`, so I take this as "no warnings" — assuming the grabs used that command, which I did not
check.

Severity is H/M/L. *screen* means the vocabulary already expresses the fix; *framework* means ui/1 cannot.

## E-signature

**1000**
- **Regressions:** none. The only pixel changes from app-j are the sidebar label and the head rule.
- **Batch K:**
  - K6 closed: the head rule and the footer rule both measure (237,240,243). They were (221,225,230) and (237,240,243).
  - K1 closed: under the scrim, the selected label peaks at (173,174,177) against (138,141,150) for the other labels, so it is white under the scrim.
  - K8: the dialog keeps a shadow (`elevation.dialog`).
- **Remaining:**
  - L, framework: the dialog shadow is a hard 2 px slab, (154,156,159) on the scrim's (167,169,173). The token's 10 px blur is ignored, and the design asks for `0 18px 48px` at 28 %. This is "a shadow drawn blurred", which the batch K design lists as not in this batch.
  - L, framework: both rules are `color.divider` = #EDF0F3, which is the design's table-row rule. The design draws the dialog rules and the summary border #E6E9ED. One token cannot hold both values.

**390**
- **Regressions:** none.
- **Batch K:**
  - K4 closed. Signer box bottom 297 → Meaning label 317, and Meaning box bottom 370 → Reason label 390. That is the same 20 px between every field; app-j had 8 px under Signer.
  - K6 closed: both rules are (237,240,243).
- **Remaining:** L, framework: the rule colour, as at 1000.

## Receipt

**1000**
- **Regressions:**
  - L, screen: the session pane went from `fixed(34)` to `fixed(30)` (753–983, was 723–983). "S-26-104228 rejected: seal broken." now wraps to two lines; the design has one. Fix: `fixed(31–32)`, or let the pane's text wrap at a better point.
- **Batch K and K-S, all closed:**
  - three grid columns, in the design's field order;
  - 12 px around the decision rule (469 / 482 / 495);
  - the sample heading at section size;
  - the banner on one line;
  - K7: each box starts at x 244, the group label's edge (it was 250);
  - K8: the card is flat, with no shadow rows under its border at 544 (app-j had 607–609);
  - K1: the selected sidebar label measures (255,255,255).
- **Clipping:** no clipping or mid-word breaks. "Release, finished product" and "24-Oct-2026 09:41" with the calendar button fit their fields with little room. The calendar button stays by K9's decision and is not counted.

**390**
- **Regressions:** none.
- **Closed:**
  - the range is in the label;
  - the sample heading is about 14 px;
  - K7: each box starts at x 16, the page edge;
  - the inputs are 44 px (231–274), as the design's `.input.lg`.
- **Remaining:** L, framework: the label reads "Temperature on arrival · 2–8 °C \*", and the design has "Temperature on arrival \* · 2–8 °C". A field's caption cannot carry text after its required marker.

## Instrument

**1000**
- **Regressions:** none.
- **Batch K:**
  - K3 closed: the principal sits at y≈23, on the actions' line. It was at y≈38.
  - K8 closed: the cards are flat.
  - K1 closed.
- **Remaining:** none beyond the cross-screen items below.

**390**
- **Regressions:** L, framework (K12 side effect): the head's ink starts at y 8 (it was 12), with 4 px of bar padding. The design's card starts the eyebrow about 20 px down. See "Compact bar inset" below.
- **Batch K:**
  - K11 closed: from the "Current booking" header text to the first content line is about 31 px. It was 43; the design has about 30.
  - The folded panels are 44 px tall; the design has 46.
  - K8 closed: no shadows.
- **Remaining:** only the compact bar inset.

## Audit

**1000**
- **Regressions:** L, screen: "In review" in Old value now wraps to two lines. It was one.
  - Cause (read, `e25ca5104`): the weights went from 2:2:3 to 3:4:5.
- **Batch K:**
  - K7 closed: the boxes are at x 227, the rail's edge.
  - K8 closed: the table card is flat.
  - K1 and K2 closed.
  - K-S live entry count: closed. "4 entries" matches the pager's "1–4 of 4".
  - K-S note above the chips: done. The chips now hold one line.
- **K-S "wider Reason column": not closed.**
  - The new weights give Reason 5/12 = 41.7 % of the stretch, against 3/7 = 42.9 % before. So it got slightly narrower: x 890–980, about 90 px.
  - Rows still wrap to 5 lines. Row 1 is 136 px tall; the design's row is about 88.
  - M, screen: give Reason the larger share, e.g. Old/New `stretch(2)` and Reason `stretch(5)`, or Old/New `fixed(9)`.
- **Remaining:**
  - L, screen: the date chip reads "20-Oct-2026 – 24-Oct-2026"; the design has "20–24 Oct 2026".
  - L, screen: the note sits above the chips; the design has it beside them, on one line. This is an acceptable trade-off at 785 px.

**390**
- **Regressions:** L, framework (K12 side effect): Export PDF's top is now at y 4 (it was 8). The design's head has `padding: 12px 16px`.
- **Batch K:**
  - K13 closed: Export PDF ends at x 373, 17 px from the edge. It was 381.
  - K-S "Load 25 more": present, full width.
- **Remaining:** M, framework (a library defect to file): the load button is meant to be disabled, but it draws exactly like an enabled one.
  - Measured: its label's darkest pixel is (28,30,33), the same as Export CSV's.
  - Read: `audit.listed` = 25 is not less than 4 shown entries, so `.enabled` is false.
  - Read: `MorphButton` sets `palette.buttonText` with no disabled branch, and neither `MorphButton.qml` nor `MorphButtonBackground.qml` reads `enabled`. So no button in `src/qt_quick` has a disabled look.
  - Not run: I did not probe whether the item's `enabled` really is false in this grab.

## Samples

**1000**
- **Regressions:** L, screen: Tests now wraps ("Assay, impurities", "TOC, conductivity"); it was one line. Product went to `stretch(6)` and Tests to `stretch(4)`. The rows are still about 48 px, so the wrapping moved from one column to the other rather than going away.
- **Batch K:**
  - K2 closed: the head is one line, with the principal at the end.
  - K1 and K8 closed.
  - K-S routine priority: done, but it doesn't match the design (below).
- **K-S "wider Product column": partly closed.**
  - Rows are 47–48 px; the design's are 31, with `td{white-space:nowrap}`.
  - M, screen: narrow the Sample column (`fixed(11)` against about 80 px of text) and Due and Spec, so that Product and Tests both fit on one line.
  - Inferred: the design's eight no-wrap columns need about 965 px, so one-line rows may not be reachable on a 785 px page at all.
- **Remaining:** L, framework: the Routine priority.
  - The design draws an outlined tag with muted text: `.p-rou` is #4D5461 on a #C3C9D2 border.
  - At 1000 we draw muted text with no outline.
  - At 390 we draw an outline (195,201,210) with `color.text` (28,30,33).
  - A neutral outline badge takes `color.text` (`MorphBadge.plain`), and no tone gives it muted text.

**390**
- **Regressions:** L, framework (K12 side effect): Register's top is at y 4 (it was 8); the design has 12.
- **Batch K:**
  - K13 closed: Register ends at x 374, 16 px from the edge.
  - K-S spacing closed: the search box ends at 103 and Filters starts at 112, a gap of 8 px, as in the design.
  - K7: the card boxes are at x 25.
- **Remaining:** L, screen: "Filters · 0" against the design's "· 2". The count leaves out the default "Status: open".

## Across these screens

- **Compact bar inset** (L, framework). K12 set the compact bar's padding to step 1 (4 px) for every head, taking the value from console's mockup 10. Mockups 03–05 pad the phone head 12–16 px. As a result:
  - instrument's eyebrow is 8 px from the top;
  - audit's and samples' framed buttons are 4 px from the top.

  The bar has one inset; there is no way to give a bar with a framed action or a multi-line head the larger one.
- **Filled-button caption weight** (L, framework). Filled captions draw at `font.weight.strong` (700). The design's `.btn` is 500 for every button, and its headings are heavier. No token separates a caption's weight from the strong weight.
- **Shortened texts:** none of "Out of spec", "Out of trend", "Spec." or "Previous" are on these screens. Audit's "Export PDF for inspection" is whole.
- **1000 px clipping check:** no clipping, mid-word breaks or squeezed-to-nothing columns. The squeezed columns are audit's Reason and samples' Product and Tests, listed above. "24-" / "Oct-2026" in audit's New value breaks at the hyphen, as the design's own render does.

## Summary

- **Regressions:** 4, all low:
  - receipt's rejection line wraps (screen);
  - audit's Old value wraps (screen);
  - samples' Tests wraps (screen);
  - the compact bar is tighter on instrument, audit and samples at 390 (framework, a K12 side effect).
- **Batch K items on these screens:**
  - closed: K1, K2, K3, K4, K6, K7, K8, K11 and K13;
  - K9 kept by decision;
  - closed K-S items: receipt ×4, audit note, count and Load-more, samples head and phone spacing.
- **Open:** 2.
  - Audit's wider Reason column is not achieved; the weights made it narrower.
  - Samples' Product column is only partly fixed: the wrap moved to Tests and rows are still about 48 px.
- **Remaining differences:** 12, not counting the regressions.
  - **Medium, 3:**
    - audit's Reason column (screen);
    - samples' row wrapping (screen);
    - a disabled button that looks enabled (framework, a library defect).
  - **Low, 9:**
    - 3 screen: audit's date-chip format, audit's note beside the chips, samples' Filters count;
    - 6 framework:
      - the dialog shadow's blur;
      - one divider token for the dialog rules and the table rules;
      - the required marker before a caption suffix;
      - no muted outline tag;
      - the compact bar inset;
      - the filled-caption weight.
- **Library defect to file:**
  - `MorphButton` has no disabled appearance. Inferred from reading and from one measured grab; the disabled state was not probed at runtime.
  - Not new: the shadow blur, which batch K already lists.
