I compared stability, plate, results, review and console at both widths against their mockups and the batch-I grabs. The 1000 px clipping and mid-word breaks the batch-I comparison found on these five screens are gone, and every `.err` log is empty (0 lines), including the overflow report and lane W's warnings. All batch J items visible on these screens are closed except J5 on plate, and I found six new regressions, none high.

Nothing is cut off at 1000 px on any of the five screens: stability's Pulls column shows, review's "Approve and sign…" is whole, and the console's Status column is whole. One caveat: I am inferring, not verifying, that a `morph.layout` warning would actually reach `.err`; the batch-I logs did carry Qt warnings.

## Stability
**1000**
- **Regressions:** none. The table now fits and the condition names wrap between words.
- **Batch J, closed:** J2 (Dr. A. Meyer shows in the head), J9 (gaps between sidebar sections), J10 (selected nav item filled, white text), J8 (inputs 4 px further left).
- **Remaining:**
  - low, screen fix: condition labels take 2 lines where the mockup uses 1. Narrower timepoint columns or summary would fix it.
  - low, screen fix: the table's header band and rules stop 8 px inside the card; the mockup runs them edge to edge. Fix: `.bleed()` on the table, which J1 now makes work.
  - low, kind unverified: the date input draws a calendar icon the mockup doesn't have.

**390**
- **Regressions:** none.
- **Batch J, closed:** J6 (white page), "Continue" (compact footer label), J8.
- **Remaining:** none beyond the calendar icon.

## Plate
**1000**
- **Regression (medium, screen fix):** the range no longer breaks ("A3–A8" is whole), but the selection bar now wraps the other way. "Clear wells" sits alone on a second line, "Fill with" follows the range with no space instead of sitting at the right, and the bar is about 82 px tall instead of about 45. The stretch spacer collapses on the wrapped line (`plate.cpp:510`). Fix: put Fill with / select / Apply / Clear in one non-wrapping group pushed to the end, or narrow the select below `fixed(22)`.
- **Batch J, open (medium): J5.** The template select, Auto-fill and Save layout still drop to a second line, so J5's own "Verify" fails. The J5 code is in `shell_frame.hpp`. My inferred cause is that the 32-unit lead minimum, the trail (about 460 px) and the principal (about 90 px, new in this grab) together exceed the 785 px page. That is not measured. Fix: a narrower template select.
- **Batch J, closed:** J2, J9, J10.

**390**
- **Regressions:** none.
- **Batch J, closed:** J6 (white page); the well rows are now divided.
- **Remaining (all low):**
  - screen fix: no rule under the "Row A" heading.
  - screen fix: the badge reads "STD1" where the mockup reads "STD".
  - data: rows D–H are listed one by one; the mockup groups them as "Rows D–H".

## Results
**1000**
- **Regression (medium, probably a framework defect, weak evidence):** "System suitability" now wraps onto two lines. The Pass chips sit at exactly the same x positions as in `app-i`, so the label's slot didn't shrink; the text just wraps inside about 100 px it used to fit. My unconfirmed guess is J3's content-minimum or rounding change.
- **Batch J, closed:**
  - J7: the dialog's rules have space around them.
  - J8: the comment text is now 8 px in, matching the mockup.
  - J2, J9, J10.
  - The scrim still covers the sidebar.
- **Remaining (low):** "In progress" now sits left of the principal instead of at the far right.

**390**
- **Regressions:** none.
- **Batch J, closed:** J6, J8.
- **Remaining (all low):**
  - screen fix: the label reads "Reason" where the mockup reads "Reason for change", and it sits tight under the out-of-spec line with no gap.
  - Undo/Confirm still draw inline (already there before this batch).
  - data: "of 2" against the mockup's "of 8".

## Review
**1000**
- **Regression (medium, screen fix): the queue is squeezed.** It went from about 222 px to about 155 px when the splitter changed to 20/83:
  - "Awaiting review · 4" wraps;
  - the ages fall under the IDs instead of sitting right-aligned;
  - the tags stack;
  - the search placeholder is cut to "Worksheet, lot or a…".

  The footer now fits, so there is room to try about 25/78.
- **Regression (medium, screen fix): the shortened evaluation text loses information.** It is shortened in the shared data (`review.cpp:130,141`), so it affects both widths. "Out of trend" drops "−0.7 % vs. last 5 lots", which appears nowhere else on the screen. "Out of spec" drops "above 0.20", which you can work out from the Spec. column. Fix: keep the full text as data and shorten only the expanded cell, or let that cell wrap.
- **The shortened headers:** "Spec." loses nothing. "Previous" is slightly ambiguous (previous lot or previous value?), low.
- **Regression (medium, framework):** the selected queue entry's WS id now draws in the accent green. The mockup's `.qi.sel` is only a tint plus a bar, with no colour change on the text. The cause is J10's `contentScheme`: under the `accent` look it recolours the item's content text. The same thing shows on console 390.
- **Batch J, closed:**
  - J3: the footer is whole, the Evaluation and Change columns show, and "0.12 …" is cut off as the mockup's own Change column is.
  - J8, J9, J10's fill, J2.
  - The summary tags fit on one line again.
- **Remaining (low):**
  - screen fix: "Approving needs your signature" sits after Reject. The mockup has it beside Approve, so the spacer should go before the note.
  - The shell title above the queue is an accepted residual.

**390**
- **Regression (medium):** the evaluation tags shrank to "Out of spec" / "Out of trend". `app-i` and the mockup both show the full text, and there is room for it. Same cause as at 1000.
- **Batch J, closed:**
  - J11: the 72 px gap and the 5 px band are gone.
  - All/Flagged now stretches (open after batch I).
  - J6.
- **Remaining (low):**
  - screen fix: a comment card shows that the mockup's compact layout doesn't have.
  - The back link sits under the tags (accepted residual).

## Console
**1000**
- **Regression (low, screen fix):** the narrower sample pane wraps every analyte label onto two lines ("Assay · / 95.0–105.0"). The mockup wraps only "Total impurities".
- **Batch J, closed:** J2, J9, J10.
- **Remaining:**
  - medium, screen fix: Test shows about 9 characters ("Assay, im…", "TOC, con…"); the mockup shows it in full and clips only Status.
  - low, screen fix: the worklist rows and the selection bar stop 8 px inside the pane; the mockup runs them edge to edge. Fix: `.bleed()`, now possible under J1/J11.
  - The sidebar width (215 px against 176) and the lab select's position are accepted residuals.

**390**
- **Regression (medium, framework):** the selected row's text "Assay, impurities · Paracetamol 500 mg" is now green, from the same J10 cause as review. The mockup draws it dark.
- **Batch J:** the page stays grey, which is correct here (mockup 10 has no white compact window).
- **Remaining (low, screen fix):** the dark head is about 60 px against 52.

## Summary
- **Regressions: 6.** None high.
  - 5 medium:
    - plate selection bar (screen fix);
    - results "System suitability" wrap (framework, inferred);
    - review queue squeeze (screen fix);
    - evaluation text loss at both widths (screen fix);
    - selected item's content drawn in accent at review 1000 and console 390 (framework, J10).
  - 1 low: console sample-pane labels wrap (screen fix).
- **Open batch J items: 1.** J5 on plate 1000: the trail still wraps. Closed and seen on these screens: J2, J3, J6, J7, J8, J9, J10's fill, J11, and review 390's All/Flagged stretch.
- **Remaining differences: 13.**
  - 1 medium, screen fix: console Test column.
  - 12 low:
    - 10 screen fixes: stability condition wrap; the stability table and console worklist not reaching their card or pane edges (`.bleed()`, 2 items); plate Row A rule; "STD1"; results "Reason" label and gap; review footer note position; review "Previous" header; review 390 comment card; console 390 head height.
    - 1 of unverified kind: the stability date-input icon.
    - 1 data: plate's rows D–H listed one by one.

Three findings carry most of the weight. J10's accent look recolours list content against mockup 09 (`.qi.sel` has no `color`), which is a framework issue. The shortened evaluation texts drop real information. And review's 20/83 splitter overcorrected: it fixed the footer and broke the queue.

I didn't edit, build or commit anything. The grabs I read are in `/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad/lg-cmp/app-j/` and `/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad/lg-cmp/app-i/`, from branch `lg-fold-j` at `300cee8c1` in `/home/yaraslau/repo/morph-lims-e1`.