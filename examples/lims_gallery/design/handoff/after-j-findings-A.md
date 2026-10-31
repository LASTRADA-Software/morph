I've reviewed esignature, receipt, instrument, audit and samples at both widths, against the mockups and the batch-I grabs. Batch J fixed the 785 px breakage on audit and samples, and all batch J items checkable on these screens are closed. There is one new regression: the samples head at 1000 px now wraps onto two lines.

Severity is high, medium or low; "screen" means the vocabulary already expresses the fix, "framework" means ui/1 can't express it yet.

## E-signature
**1000.** No regressions.
- **Batch J closed:**
  - J7: the summary box sits 12 px under the head rule, and the buttons 12 px under the footer rule (the design has 14 and 12).
  - J8: Signer and Reason text start 8 px from the border; batch I had 12 and 16.
  - J2: the principal shows in the head.
  - J9: sidebar headings stand apart.
  - J10: the selected entry is filled across its badge.
- **Remaining:** Low, framework (theme). The head rule is (221,225,230) and the footer rule (237,240,243). The design uses #E6E9ED for both, so the dialog's two rules use different tokens.

**390.** No regressions.
- **Closed:** J7 (summary box 12 px under the rule; button 12 px under the footer rule, 11 px above the bottom). "Approved" is now plain.
- **Remaining:**
  - Medium, screen: Signer→Meaning gap ~4 px vs ~16 between the other fields and in the design. It was the same in batch I; the earlier comparison missed it.
    - Cause: `byWidth`'s compact case `{signerField(), meaningField()}` becomes a switch case's holder column with gap 0.
    - Fix: wrap the pair in `ui::column(..., {.gap = 2})`.
    - This may be a framework seam: a holder column made from a switch case doesn't take the gap of the column it sits in.
  - Low: the same rule-colour mismatch as at 1000.

## Receipt
**1000.**
- **Regressions:** none. Only Lot, Tablets and the session pane got narrower.
- **Batch J closed:**
  - J1: the session header band runs x 724–982 inside a border at 723 and 983, so it now reaches the frame's edges. Batch I had an 8 px inset.
  - J4 isn't really exercised. With the 2-column grid every field is wider than its text, so Sample type and Received at show their start anyway.
  - J2, J9 and J10 are closed here too.
- **Remaining:**
  - Medium, screen: the sample grid has 2 columns against the design's 3, so Received at moves to row 2 and the field order changes.
    - Now that J4 shows a field's start, 3 columns with a narrower session column (around `fixed(30)`) should be possible.
  - Low, screen: the divider above Reject/Receive has 4 px on each side (note 548, rule 553, buttons 558); the design has about 13. Raise the gap on that `divided` column (it holds only 2 children).
  - Low, screen: the "Already received" banner still wraps to two lines.
  - Low, screen: the sample heading is now the heading role, about 17 px, against the design's 14 px bold.
  - Low, framework: check boxes are inset about 6 px from the group label; in the design they align with it. This is likely the control's own padding.
  - Low: Received at shows a calendar icon that the design doesn't have. It comes from the date-input feature.

**390.**
- **Regressions:** none.
- **Closed:** J6, the page is white.
- **Remaining:**
  - Low, screen: the sample heading is about 18 px, nearly the size of the title, against the design's 14 px.
  - Low, screen: the range shows as "Required range 2–8 °C" under the field; the design puts "· 2–8 °C" in the label.
  - Low, framework: the check box inset, as at 1000.

## Instrument
**1000.**
- **Regressions:** none.
- **Batch J closed:**
  - J10: the selected entry's fill runs x 12–202 across the badge; batch I stopped at 180.
  - The J-F re-fit worked: events are one line each, with dividers.
  - J2 and J9 are closed here too.
- **Remaining:**
  - Low, framework: the principal sits at y≈38, centred on the head's three-line block, while the actions are at y≈23. It doesn't sit on the actions' line.
  - Low, framework (theme): cards have a bottom shadow; the design's cards are flat with a border. This applies to every screen.

**390.**
- **Regressions:** none.
- **Closed:** J6 (white page). The booking card is one line plus the time slot, with no progress bar.
- **Remaining:** Low, screen: about 44 px between "Current booking" and its first line, against about 30 in the design.

## Audit
**1000.**
- **Regressions:** none. The batch-I overflow is fixed: no clipping, no mid-word breaks, and the table ends at x 987.
- **Batch J closed:**
  - The J-F re-fit: the chip row wraps and the rail is 22 units wide.
  - J2, J9 and J10 are closed here too.
- **Remaining:**
  - Medium, screen: the Reason column is squeezed to about 100 px. Rows wrap to 4–6 lines (up to 135 px tall, against about 88 in the design). Narrow the User/Field/Old/New columns or give Reason a larger share.
  - Low, screen: the date chip reads "20-Oct-2026 – 24-Oct-2026" and wraps to its own line. The design says "20–24 Oct 2026" on one line.
  - Low, screen data: the head says "57 entries" but the pager says "1–4 of 4". Unchanged since batch I.

**390.**
- **Regressions:** none.
- **Closed:** J6, the area below the entries is now white.
- **Remaining:**
  - Low, screen: the design's "Load 25 more" button is missing.
  - Low, screen: Export PDF sits about 8 px from the head's right and top edges, against 16 in the design.

## Samples
**1000.**
- **Regressions:**
  - **Medium:** the head now wraps. The title is on one line and the search, Status, Priority and Register controls drop to a second, so the head grows from 48 to 82 px.
    - Cause (my arithmetic, not measured): J2's principal (about 100 px) plus J5's minimum width for the title's block. Together with the controls (about 660 px), the head no longer fits in 785 px.
    - The wrapped line starts 12 px right of the title (x 239 against 227), and the principal floats between the two lines at y≈42.
    - Screen fix: a narrower search box. Framework angle: the J5 minimum reserves width that a short title like "Samples" doesn't use.
- **Batch J closed:** the J-F re-fit. There are no mid-word breaks, and rows are 48–65 px tall (batch I had 66–83).
- **Remaining:**
  - Medium, screen: Product · lot (about 120 px) still wraps to 2–3 lines while Tests has spare room. Weight Product · lot 3 to Tests 2, or narrow the Sample, Due and Specification columns. The design's rows are 31 px.
  - Low, screen: the Priority tags are black text on a dark outline; the design's are muted grey.

**390.**
- **Regressions:** none.
- **Closed:** the tag row now sits one gap under the text line, so cards are 80 px, as in the design.
- **Remaining:** low, screen: the Filters row sits about 4 px under the search box (the design has 8), the Register button is flush to the head's edge, and Filters shows "· 0" against the design's "· 2".

## Logs
All 20 `app-j/*.err` files are 0 bytes. That fits "no overflow reports, and the darkHead and nav.back/nav.forward warnings are gone", but every file is empty, including screens that had warnings before. So I can't tell whether stderr was captured for this grab run. Treat "no warnings" as unverified.

None of the shortened texts named in the brief ("Out of spec", "Spec.", "Previous") are on these screens; they are reviewer B's.

## Summary
- **Regressions:** 1, medium — the samples head wrapping at 1000.
- **Open batch J items on these screens:** none.
  - Closed: J1, J2, J6, J7, J8, J9, J10 and the J-F re-fits on audit, samples, receipt and instrument.
  - J4 is closed in effect, but this screen no longer exercises it.
  - J3's overflow report: no evidence either way, because of the empty logs.
- **Remaining differences:** 18 in all, not counting the regression above.
  - **Medium, 5, all screen fixes:**
    - the e-signature compact Signer→Meaning gap (which may point to a framework seam);
    - receipt's 2-column grid;
    - audit's squeezed Reason column;
    - samples' Product · lot wrapping;
    - samples' head width.
  - **Low, 13:**
    - 9 screen fixes: receipt's divider spacing, banner wrap and heading size at both widths; the audit date chip, pager count and Load-more; the instrument compact gap; the samples priority tags and compact spacing.
    - 4 framework or theme: the dialog's mismatched rule colours, the check box inset, the principal's alignment in a multi-line head, and the card shadow.
- **Library defects to file:** none new and confirmed. Possible but unreproduced: a switch case's holder column ignoring the surrounding gap, and the J5 minimum forcing a wrap on short titles.