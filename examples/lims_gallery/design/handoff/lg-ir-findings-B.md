I reviewed stability, plate, results, review and console at both widths. Two things in the brief needed correcting first:

- **Grab folder:** the brief points to `app-hh` for both the new and the previous grabs. The after-batch-I grabs are in `lg-cmp/app-i` (14:23, matching the latest branch commit `c3f9ce751`); `app-hh` (10:13) is the after-H baseline. I compared `app-i` against `app-hh`.
- **The lane's "After batch I" section has two problems:**
  - It says "the principal (Dr. A. Meyer) closes every expanded head". It appears in none of the five 1000 px grabs, nor in receipt's, and the sidebar foot shows only "Sign out". The likely cause is an empty principal in the grab session (`shell.cpp:470` passes `_signedIn ? principal : ""`). That is inferred from the code, not verified.
  - It understates two regressions: review 390's gap and the clipping caused by the narrower page.

The sidebar now shows at 1000 px (G-h). Most of the regressions below come from the page being about 785 px wide instead of 1000. The G-h re-check of fixed sizes was supposed to catch this and missed it on stability, review and console.

## Stability
**1000**
- **Regression (high):** the conditions table no longer fits. The Condition column is squeezed to about 60 px, so the labels break mid-word ("intermed/iate", "accelera/ted"), and the **Pulls column is cut off entirely**. The cause is nine fixed(4) columns (about 47 px drawn each) plus Pulls in a roughly 490 px panel beside the fixed(33) summary. Screen fix: a minimum width on the Condition column, narrower timepoint columns, or a narrower/stacked summary. It also suggests a framework gap: a table wider than its panel clips silently instead of scrolling or reporting it.
- **Batch I, closed:** G-o (connectors stand off the steps), G-r (link inside the banner), G-n (stepper joins the head), field row `align: start`, timepoint headers centred, "Continue to tests".
- **Remaining:** none beyond the regression.

**390**
- **Regression (low):** the compact footer now reads "Continue to tests"; mockup 06 compact says "Continue". Screen fix: make `continueLabel` depend on width.
- **Batch I, closed:** G-j (no chevrons), G-l (22 px boxes), G-r, the per-pull error spanning both fields, R1 (title at x=56).
- **Remaining (medium):** the page is grey; the mockup's compact window is white (`background:#fff` on 06, 07, 08 and 09 compact). This repeats across screens, so it is a framework gap: a compact page surface token, or a theme switch for it.

## Plate
**1000**
- **Regression (medium):** the trail (template, Auto-fill, Save layout) wraps to a second line and the head grows from about 58 to 84 px. The spec for `leadPlacement: inline` says the inline lead wraps "before the trail does"; here the trail wrapped. That is a framework defect against the spec.
- **Regression (medium):** "6 wells selected · A3–A8" breaks inside the range ("A3–/A8"). Screen fix: let the count text keep its natural width so the bar wraps between items.
- **Batch I, closed:** G-i/G-a (subtitle inline), bold count only, "To place" dividers, centred "+ 9 more", sources fixed(30) fits.
- **Remaining (low, framework):** the sidebar's section overlines sit about 4 px under the search box (the mockup leaves about 30 px). This shows on all five screens.

**390**
- **Regressions:** none.
- **Batch I, closed:** buttons joined to the head block (G-n), "· 14 samples to place" in the subtitle, inline "**A1** · …" well rows, no chevrons, R1.
- **Remaining:** grey page instead of white (medium, framework gap as on stability); no dividers between well rows (low, screen fix: `divided` on the rows' forEach).

## Results
**1000**
- **Regressions:** none. The modal scrim does cover the sidebar (measured 31→27).
- **Batch I, closed:** R2/G-a ("In progress" at the right of the eyebrow line), G-d (dialog rules edge to edge), the Sample column, bold mono group rows.
- **Remaining (low):** the comment text area's text is about 16 px in versus the mockup's 8.

**390**
- **Regressions:** none.
- **Batch I, closed:** R1 (no title wrap), "Sample 2 of N · S-…" title, pager joined to the head (G-n), the out-of-spec line above the buttons.
- **Remaining:** grey page (medium, framework gap as above); "of 2" against the mockup's "of 8" (low, data); inline Undo/Confirm buttons the mockup doesn't draw (low, already there before H).

## Review
**1000**
- **Regression (high):** the footer overflows the worksheet pane and **"Approve and sign…" is cut off** at the window edge. Screen fix: a narrower queue in the splitter or hide "Approving needs your signature" here. Same silent-clip framework gap as stability.
- **Regression (medium):** the Evaluation tags are clipped ("Out of spec:", "Out of trend"); in the mockup they are fully visible.
- **Regression (low):** the summary tags wrap to two lines.
- **Batch I, closed:** R3 (12 px entry padding), age right-aligned, mono WS id, result column bold, G-p (grip shows).
- **Remaining (low):** the shell title "Review and approval" above the queue; the spec already lists this as out of scope.

**390**
- **Regression (medium):** measured 72 px of empty grey between the tab strip's rule (y=177) and All/Flagged (about y=250); in `app-hh` it was about 12 px. Also a 5 px grey band (y=129–133) between the head block and the tab strip, so the strip isn't joined to the head as 09 draws it.
- **Batch I, closed:** head carries the eyebrow, title and tags; flagged rows edge to edge (G-f); "Show" caption dropped.
- **Batch I, open (low):** All/Flagged doesn't stretch (about 130 px wide).
- **Remaining (low):** back link under the tags (accepted residual).

## Console
**1000**
- **Regression (medium):** the worklist's Test column is truncated to about 4 characters ("Assa…", "Wate…") and Status is clipped. The lane calls this "as the mockups' own are", but the mockup clips only Status and shows Test in full. Screen fix: re-size the splitter or the columns.
- **Batch I, closed:** G-a ("Connected" at the end), "Open run" as a framed button, the two bottom panes as one card, "6 most urgent" removed, G-m chevrons.
- **Not visible:** principal name (see above).
- **Remaining (low, framework):** the sidebar is 215 px against 176 (accepted residual); the selected nav item draws an accent bar and blue text where the mockup fills it with white text.

**390**
- **Regressions:** none.
- **Batch I, closed:** G-g (dark head, white icons, icon-only Scan), "Connected" hidden, stretched framed "Open run", G-j, R1.
- **Remaining:** the worklist rows sit on the grey page with no card or dividers; the mockup draws a white card with dividers (medium, screen fix: `panel` around a divided forEach). Dark head about 60 px against 52 (low).

## Logs (`app-i/*.err`)
- **darkHead contrast warnings:** every grab logs "surface darkHead: color.track … 1.21:1 / 1.38:1", 20 times each. Low; the fix is to declare `color.track` on the gallery's `darkHead` surface.
- **nav.back/forward warnings:** console 1000, audit 1000 and samples 1000 log "icon nav.back/forward is not in the theme's icon set", although the built-in chevrons draw. Low framework issue: a warning for an icon that has a built-in fallback. Inferred from the log plus the grab.

## Summary
- **Regressions: 10.** 2 high (stability Pulls column cut off, review "Approve and sign…" cut off), 6 medium (plate head wrap, plate range break, review evaluation clip, review 390 gap, console Test column, the plate inline-lead wrap order is the same item as the head wrap so counted once; plus the review 5 px band), 2 low (stability compact "Continue to tests", review tags wrap).
- **Open batch I items: 1** (review 390 All/Flagged stretch). The principal-in-head claim can't be verified from the grabs. Everything else listed for these five screens is closed.
- **Remaining differences: 10.**
  - Medium: 3 framework (compact white page surface, counted once across stability, plate and results; inline lead wraps after the trail against the spec; tables and footers clip silently in a narrow pane), 1 screen fix (console 390 worklist card).
  - Low: 2 framework (sidebar overline spacing; spurious nav.* warning), 4 screen fixes (plate well dividers, results text area inset, darkHead `color.track`, console 390 head height).

Most of the high and medium regressions share one cause: the batch I sidebar takes about 215 px at 1000 px, and the fixed-size layouts weren't re-checked. Before more vocabulary work, stability's matrix, review's footer and console's worklist need re-sizing.