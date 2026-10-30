I've covered all five of my screens at both widths, reading every grab against its mockup and against the batch-H grabs. The brief gives the new grabs as `lg-cmp/app-hh/`, but those are the batch-H grabs (taken at 10:13). The batch-I grabs are in `lg-cmp/app-i/` (taken at 14:22), so I compared `app-i` against `app-hh`.

**The main finding:** the sidebar now shows at 1000 px (G-h), which leaves 785 px for the page. Three screens were never re-fitted to that width. Audit and samples are clearly broken at 1000, and receipt shows truncated fields. The lane's "After batch I" section doesn't mention any of this.

## E-signature
**1000.** No regressions.
- **Batch I, closed:** dialog rules now span the dialog edge to edge (G-d; rules at y 230 and 613 run x 255–746). "Signing at…" is in the small text role.
- **Remaining:**
  - Medium, framework gap: the body starts 5 px under the head rule (design 14), and the actions sit 6 px under the footer rule (design 12). `MorphDialog` puts no spacing around the rules, and no key reaches it.
  - Low, framework gap (theme): the Reason text area's text is about 16 px from its left edge, against 8–9 px in the design. The text area's inner padding can't be set.

**390.** No regressions.
- **Batch I, closed:**
  - The sheet's title is centred, with a full-width rule at y 65 (G-e).
  - The banner carries the whole "Password incorrect." sentence, and the field no longer shows its own error.
  - The summary's record ID is bold and mono.
  - The footer rule spans the width.
- **Remaining:**
  - Medium, framework gap: the same spacing problem as at 1000. The summary box is 6 px under the head rule (design 14), and the button is 6 px under the footer rule (design 12).
  - Low, screen fix: "Approved" is bold on compact, but the design's compact line is plain. Add a compact branch to the "Signing at…" spans.

## Receipt
**1000.**
- **Regressions:**
  - **High:** the narrower page clips the start of two fields. Sample type reads "ease, finished product" and Received at reads "4-Oct-2026 09:41". The fix on the screen is to make the `sessionTable` column (`fixed(36)`) narrower, or to give the sample grid 2 columns at this width. The clipping also suggests a framework defect: a read-only or unfocused field narrower than its text shows the end of the text, not the start. That cause is inferred from the image only.
  - Low: the "Already received" banner now wraps to two lines.
- **Batch I, closed:**
  - The session line and counts sit at the end of the title line (G-a).
  - The sample grid's first row is top-aligned.
  - Check-box rows are 21 px apart (G-l).
- **Batch I, NOT closed — G-k:** the "This session" header band and row rules run from x 716 to 974, inside a card that spans 708–983. That is the same 8 px inset as in `app-hh` (686–975 inside 678–983). `receipt.cpp:388` does call `.bleed()`, so `bleed` has no measurable effect on a table inside a `panel`. The lane's "header band reaches its card's edges" claim is false. This looks like a library defect.
- **Remaining:**
  - Low, screen fix: there is no rule above the Reject / Receive buttons. A `divided` column would draw it.
  - Low, screen fix: the "Sample S-…" heading is smaller than the design's 14 px bold.

**390.** No regressions; R1 is fixed (the title is back at x 56).
- **Batch I, closed:**
  - The counts are on the title line, and the head is one line, centred (G-a, G-b).
  - Check boxes are 22 px.
  - Reject is 108 px wide.
- **Remaining:** nothing significant.

## Instrument
**1000.**
- **Regressions:**
  - Medium: the Recent events entries now wrap ("…limit 2.0. Run held." spills to a second line). The left pane is 405 px against the design's 627, while the right column keeps `fixed(44)`. Screen fix: a narrower right column, around `fixed(36)`.
  - Low, framework: the selected navigator row's highlight stops short at the badge (ends at x 180, against 203 for a row without a badge).
- **Batch I, closed:**
  - Tabs are the control height, with their labels centred (G-c).
  - There is no rule between the head and the tab strip (G-n).
  - The page starts 17 px under the strip, as the design's 16.
- **Remaining:** Low, screen fix: Recent events has no rules between entries. Make its `forEach` `divided: true`.

**390.**
- **Regressions:** none; R1 is fixed (the badges fit on one line and the head is shorter).
- **Batch I, closed:** the collapsibles have no ▸/▾ markers (G-j), and the head is joined.
- **Remaining:** Low, screen fix: Current booking shows "Worksheet WS-…" plus a progress bar. The design's compact card has one line, "WS-2026-0412 · M. Kaur", then the time, and no progress bar.

## Audit
**1000.**
- **Regressions:**
  - **High:**
    - The trail runs past the page's right edge. The Reason column can't be seen, "Approve|d" breaks mid-word, the chip row is cut off at "20-Oct-2026 – 2…", and rows are 190–245 px tall.
    - Arithmetic: 785 px, minus the facets column (`fixed(29)`, about 222 px), minus 32 px padding, leaves about 530 px. That holds the fixed columns (45 units, about 345 px) and a chip row that does not wrap (about 640 px).
    - Screen fixes: `wrap: true` on the chip row, and narrower fixed table columns or facets.
    - The rows being sized for a narrow Reason column while the table draws past its container also suggests a framework defect: a row whose content is wider than its stretch parent is not constrained. That is inferred, not reproduced.
  - Low: check-box rows are 18 px apart, now tighter than the design's 22.
- **Batch I, closed:**
  - The facets sit in a full-height white pane with a divider (G-q).
  - The pager draws real chevrons (G-m).
- **Remaining:** everything else is hidden behind the overflow.

**390.** No regressions.
- **Batch I, closed:**
  - The title is centred in the head (G-b).
  - Filters, Export CSV and the entry count sit in the head's white block.
  - Entries run edge to edge on white, with dividers.
- **Remaining:** none.

## Samples
**1000.**
- **Regressions:**
  - **High:** the Product · lot column has shrunk to about 70 px. It breaks words ("Paracetam|ol 500 mg · L-26-037|7"), and rows are 66–83 px tall against 31 in the design. The fixed columns add up to 75 units (about 575 px) plus the check-box column, which leaves the stretch column almost nothing at 785 px. Screen fix: make Tests stretch, and shrink the Sample, Due and Specification columns.
- **Batch I, closed:**
  - The bulk bar's text is 12 px from its border, with buttons about 5 px from the top.
  - "·" now precedes "Select all 312".
  - The head is centred.
- **Not judged:** the pager, which is below the fold.

**390.** No regressions.
- **Batch I, closed:** search, Filters and Sort are in the joined white head block; the head is centred; check boxes are 22 px.
- **Remaining:** Low, screen fix: the tag row sits about 2 px under the text line, against about 6 px in the design. Add a gap to the card column.

## Checking the lane's "After batch I" section
- **"Session table's header band reaches its card's edges" is false.** I measured an 8 px inset, unchanged from batch H (G-k above).
- **"The principal closes every expanded head" isn't visible in any grab.** No "Dr. A. Meyer" appears in the heads of receipt, instrument, audit, samples, e-signature or console at 1000, and the sidebar's foot shows only "Sign out". `principalPlacement: "head"` is set in `lims_gallery.cpp:149`. Either the grab session has no principal, or this item isn't working. I didn't verify which.
- **The section is silent on the 785 px breakage.** Audit and samples at 1000, plus the clipped receipt fields, are not mentioned. The spec said audit's `fixed(29)` had been "rechecked against the 785 px page", and that check evidently failed.

## Warnings in the run logs
- The audit and samples logs say "icon nav.back / nav.forward is not in the theme's icon set", even though G-m makes both built-in chevrons and they draw correctly. This is a false warning from library code. Low; probably worth filing.
- Every screen's log says "surface darkHead: color.track: color.text is 1.21:1 … below 4.5:1". This comes from the gallery's own `darkHead` surface file. Low; it should be fixed in place.

## Summary
- **Regressions:** 7.
  - High: audit 1000 overflow, samples 1000 squeezed column, receipt 1000 clipped fields.
  - Medium: instrument 1000 wrapping.
  - Low: receipt banner wrap, audit check-box spacing, navigator highlight.
  - Every one of them comes from the 785 px page at 1000.
- **Open batch I items:** 1 not closed (G-k bleed, contrary to the lane's claim), plus 1 claim not visible (the principal in the head). Everything else checked on these screens is closed: G-a, G-b, G-c, G-d, G-e, G-j, G-l, G-m, G-n, G-q, R1 and the screen fixes.
- **Remaining differences (excluding the regressions):** 9 in all, 3 medium and 6 low.
  - **Medium:** 3 — the dialog/sheet rule spacing at each width (framework gap) and G-k's `bleed` having no effect (framework; looks like a library defect).
  - **Low:** 6 — 5 screen fixes (e-signature "Approved", receipt divider and heading, instrument event dividers and compact booking) and 1 framework gap (the text area's inner padding).
  - The samples compact tag gap is polish below that count.
  - On top of these, the two warnings above, both low: one library (nav chevrons), one gallery theme (darkHead).
  - **Library defects worth filing under AGENTS.md:** `bleed` not taking effect inside a `panel`, and the false `nav.*` icon warning. Possibly also the table/row overflow and the field-shows-its-end clipping, but those two are inferred from the grabs, not reproduced.