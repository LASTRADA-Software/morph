# After batch L: stability, plate, results, review, console (findings B)

I compared the grabs in `lg-cmp/app-l/` (1000, 1215 and 390) against `lg-cmp/app-k/` and the mockups (the renders and
the `.dc.html` sources). Pixel values were measured with PIL. Causes marked *read* come from the source in
`/home/yaraslau/repo/morph-lims-e1` (`lg-fold-l`); I did not run them.

**The 1000 px check.** On these five screens nothing is clipped mid-word and nothing overlaps.
- Review's Change cell elides to "0.12 →…", as it did in K.
- Console's Status column is cut at the pane's edge, as the mockup intends.
- Console's chain of custody continues below the fold. The page scrolls (*read*: `console.cpp` sets a least page height).

## Stability

**1215**
- Regressions: none.
- Batch L, closed:
  - L8: "Continue to tests" now has the medium caption, not bold.
  - L9: the captions are upright.
- Remaining:
  - low, screen fix: in the summary, First pull still breaks as "03-Dec-2026 (40 / °C, 1 m)". Fix: a U+00A0 in "40 °C", or the date and the parenthetical as two texts.

**1000**
- Regressions: none. Nothing is clipped.
- Remaining:
  - low: the Containers per pull hint takes 3 lines. This is a fit difference at the 785 px page.

**390**
- Regressions: none.
- Batch L, closed:
  - L-S (the K regression): the condition panel is padded. The box fill starts at x 29 and the border is at x 16, a 12 px inset, which matches the mockup's `padding: 0 12px`. In K the fill touched the border at x 17.
  - L3: the compact bar's eyebrow is at y ≈ 21, against 13 in K and about 20 in the mockup.
  - L8: Continue has the medium caption.
- Remaining:
  - low, screen fix: the check-box rows are 48 px apart, against 42 in the mockup. Fix: a smaller row `gap` in the condition grid.

## Plate

**1215**
- **Regression (medium, framework gap): the well captions are now regular weight.** The mockup draws `.well` at `font: 600`, and K drew them bold.
  - Cause (*read*): a well is a `mono` button (`plate.cpp:367`). Its caption takes `font.weight.button`, which the gallery now sets to 500.
  - Nimbus Mono PS has faces only at 400 and 700. The upright snap (`uprightWeight`) correctly sends 500 to 400.
  - The result is inconsistent on the screen: the legend chips, which are mono badges, are still bold, while the wells beside them are regular.
  - Gap: a button cannot ask for a caption weight other than the theme's one button weight. A per-button weight, or a mono or `strong` caption flag, is missing.
- Batch L, closed:
  - L8: Save layout, Auto-fill and Apply have the medium caption.
  - L9: the captions are upright.
- Remaining:
  - low: the head is one line about 46 px tall, against 55 px in the mockup.
  - The other items match: the To place pane is 230 px, and the template select elides as it does in the mockup.

**1000**
- Regressions: the same well weight (counted once above). Nothing is clipped.
- Remaining: the two-line head, which was accepted at 785 px.

**390**
- Regressions: the same well weight, on the STD and QC circles.
- Batch L, closed:
  - L3: the bar's inset is now 12–16 px.
  - L8.
- Remaining:
  - low, data: Rows D–H are listed one by one. The mockup groups them as "Rows D–H".
  - low, data: A3–A8 is shown as one line, where the mockup has A3, A4 and A5–A8.

## Results

**1215**
- Regressions: none.
- Batch L, closed:
  - L4: the dialog's shadow is blurred. Under the dialog the shade fades from (131,135,149) to the scrim's (167,169,173) over about 50 px. The mockup's `0 18px 48px` fades from (136,139,143) to (164,166,170) over about the same distance. In K the shadow was a hard 2-step band. The scrim matches the mockup's exactly: (167,169,173).
  - L2: "Submit for review" now has the disabled look while the change is open.
  - L8.
- Remaining:
  - low, screen fix: in the mockup, Submit is enabled (green under the scrim). The gallery disables it (*read*: `canSubmit` requires `!sheet.change.open`). Under the modal scrim this hardly matters.
  - low, screen fix: the Result inputs are about 77 px wide, against 110 in the mockup. Fix: widen the Result column's `fixed(...)`.
  - accepted: "In progress" sits beside the principal.

**1000**
- Regressions: none. The dialog covers part of the table as designed. Nothing is clipped.

**390**
- **Regression in look (medium, screen fix): the phone's primary action, "Submit for review", is now grey.**
  - L2 made the existing disabled state visible. *Read*: `canSubmit` is false while the change is open.
  - The mockup's compact layout has no Undo or Confirm. The change stands once the reason and the comment are filled, and Submit is green.
  - Fix: in compact, drop Undo and Confirm, and gate Submit on the reason and the comment being non-empty instead of on `change.open`.
- Batch L, closed:
  - L3: the eyebrow is at y ≈ 21.
  - L2: Next is disabled on the last sample.
- Remaining:
  - low, data: Next is disabled because the data has "of 2", against 8 in the mockup.
  - low, screen fix: the inline Undo and Confirm buttons, carried over from K. The fix is the one above.
  - low, screen fix: the gap to Impurity A's caption is about 27 px, against 22 in the mockup.

## Review

**1215**
- Regressions: none.
- Batch L, closed:
  - L1: "99.4 % LC" is on one line, as are all the Result cells.
  - L-S: "Awaiting review · 4" is on one line.
  - L8: "Approve and sign…" has the medium caption.
  - L12: the logs are empty, so the mount refused nothing.
- **Batch L, open (L-S): the queue is still squeezed (medium, screen fix).**
  - The pane is 161 px (x 215–375) at both 1215 and 1000, against about 280 px in the mockup.
  - Still broken:
    - the ages sit under the IDs;
    - "Assay and impurities · / M. Kaur" wraps;
    - the tags stack;
    - the self-review note takes 4 lines;
    - the placeholder reads "Worksheet, lot or an…".
  - The title was made to fit by dropping it to the `section` role, which makes it smaller than the mockup's 15 px semibold.
  - Cause (*read*): splitter `sizes` are absolute units (`MorphSplitter.qml`: `sizes[i] * Theme.unitWidth`), not proportions. So 21 units is 161 px at every width, and the extra width goes to the worksheet. The comment at `review.cpp:823` ("a fifth of the page, 21 of 103 units") misdescribes this.
  - Fix: about 36 units for the queue (≈ 280 px). Then the title can go back to the `heading` role.
- Remaining:
  - low, screen fix: the queue title is the `section` size. Once the queue is wider, use `heading`.
  - low, screen fix: Evaluation is still a tag with a muted note wrapped under it, so the rows are 48–56 px tall against the mockup's 30. The mockup has one tag, "Out of spec: above 0.20".
  - low, data: "7 results" and "All 7", against 24 in the mockup.

**1000**
- Regressions: none.
- L1 is closed here too: "99.4 % LC" is on one line, where K had "99.4 % / LC".
- The Change cell elides to "0.12 →…". This was already so in K, and the cell elides on purpose.

**390**
- Regressions: none.
- Batch L, closed:
  - L3: the three-line head now has its inset.
  - L8.
- Remaining:
  - low, screen fix: about 45 px between the last result and "Review comment", against 27 in the mockup (the `room` spacer).
  - accepted: "Prior lots", and the back link under the tags.

## Console

**1215**
- Regressions: none.
- Batch L, closed:
  - L-S and L11: there is no horizontal scroll bar. Status is pinned and shrinks, and is cut at the pane's edge like the mockup's.
  - L-S: Due reads "22 min overdue".
  - L7: Routine is the quiet outlined badge, and STAT and Urgent are outlined. This closes K's "filled priority tags" item.
  - L-S: the analyte labels keep "≤" with its number. Assay, Impurity A and Impurity B are on one line.
  - L2: the sample pane's "‹" is disabled on the first sample.
- Remaining:
  - low, screen fix: the Instruments panel is content height and ends at y 367, while the chart card ends at 482. The mockup stretches both to one height. Fix: `.align = Align::Stretch` on that row (`console.cpp:806` has `Align::Start`).
  - low, screen fix: the units are drawn inside the inputs ("98.9 % LC"). The mockup draws them beside the input, as their own column. Fix: a text after the input instead of `unit`.
  - low: the sample pane is about 500 px wide, against the mockup's 360, because the last pane takes the rest. So "Total impurities · ≤ 0.50" fits on one line, where the mockup wraps it. Harmless.

**1000**
- Regressions: none. There is no scroll bar, the Priority tags are padded, and Due is in full.
- Batch L, closed: the analyte labels. Only "Total impurities · / ≤ 0.50" wraps, exactly as in the mockup.
- Status is cut at the pane's edge ("Result entr", "Registere"). This is intended and is not breakage.

**390**
- Regressions: none.
- Batch L, closed:
  - L7: Routine is outlined and muted, and Urgent and STAT are outlined, as in the mockup.
- Remaining:
  - low, screen fix: the entries' due time reads "10:30", against the mockup's "due 10:30".

## Summary

- **Regressions: 2, both medium.**
  - plate, all widths: the well captions are regular weight (mockup 600, K bold). This is a **framework gap**: a button has no caption weight of its own, and `font.weight.button` 500 snaps to 400 on the mono face.
  - results 390: the primary Submit is grey. This is a **screen fix**: L2 exposed the gate on `change.open`, and the compact layout should drop Undo and Confirm.
- **Open batch L items: 1.** Review's queue (L-S) is still 161 px. Splitter `sizes` are absolute units, not proportions, so a fix needs about 36 units (screen fix, medium).
- **Closed batch L items visible on these screens:**
  - L1 (no-wrap with spans), L2 (disabled look), L3 (compact bar inset), L4 (blurred dialog shadow), L7 (quiet badge), L8 (caption weight 500), L9 (upright faces) and L11 (column shrink).
  - L12: there is no refusal in the logs.
  - The L-S items: stability's panel padding; review's Result column and its one-line title; console's scroll bar, Due, Priority padding and one-line analyte labels.
- **Remaining differences: 17**, besides the open item. 0 high, 0 medium, 17 low:
  - 12 screen fixes:
    - stability: First pull's break, the check-box row gap;
    - results: Submit enabled in the mockup, the Result input width, the caption gap;
    - review: the queue title role, the two-line evaluation cells, the gap above the comment;
    - console: the Instruments panel height, the unit beside the input, "due 10:30";
    - plate: the head height.
  - 0 framework gaps besides the plate regression.
  - 5 data or accepted items: plate's Rows D–H and A3–A8 grouping, results' "of 2", review's "7 results" and "All 7", console's sample pane width.
