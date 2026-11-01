# After batch K: stability, plate, results, review, console (findings B)

Grabs: `lg-cmp/app-k/` against `lg-cmp/app-j/` and the mockups (renders and `.dc.html`). Every `.log` for these ten
grabs is 0 bytes. Colours and distances were measured from the PNGs with PIL; causes marked *read* come from the source
in `/home/yaraslau/repo/morph-lims-e1` (`lg-fold-k`), and were not run.

The 1000 px check: nothing breaks inside a word on these five screens. Two cells are squeezed. Review's Result
"99.4 % / LC" is broken across a value and its unit. Console's analyte labels break "≤ / 0.10" between an operator and
its number. Console's worklist is now wider than its pane, so it scrolls sideways and elides Due. Details below.

## Stability

**1000**
- Regressions: none. The cards are flat; the conditions table has one line per condition.
- Batch K, closed:
  - K1: the sidebar's selected label is (255,255,255); the others are (201,204,214).
  - K8: under a card is a 1 px border, then the page's (246,247,249). There is no shade.
  - K-S: single-line condition labels.
  - K-S: the table runs edge to edge. Its band and rules run x 232–774, inside the card's border at 231 and 775.
- Remaining:
  - low, screen fix: the Containers per pull hint takes 3 lines where the mockup takes 2, because the field column is narrower at 785 px.
  - low, screen fix: the summary's First pull value breaks as "03-Dec-2026 (40 / °C, 1 m)". The mockup breaks before "(40 °C, 1 m)". Fix: the date and the parenthetical as two texts, or a no-break space in "40 °C".
  - accepted (K9): the date input's calendar button.

**390**
- **Regression (medium, screen fix).** The check boxes in an open condition section now touch the panel's left border: box fill at x 17, border at x 16. The header text is 9 px in.
  - Cause (read): K7 removed the box's 6 px inset, and `conditionSection` (`stability.cpp:624`) gives its panel no `padding`, so the content had no side inset of its own.
  - The mockup's `details` has `padding: 0 12px`. Fix: `.padding = 3` on that panel. The header then sits at 12 px too, since its minimum inset is step 2.
- Batch K, closed:
  - K7: the boxes are at the content's edge.
  - K11: header text to the first box row is 43 px, against the mockup's 42 and J's 47.
  - K12: the head is 8 px shorter.
- Remaining: none beyond the regression.

## Plate

**1000**
- Regressions: none.
- Batch K, closed:
  - K2/K3: the template select, Auto-fill, Save layout and "Dr. A. Meyer" are on the title's line, centred together (y ≈ 41).
  - K10: the subtitle wraps under the title, as "ELISA, host cell protein · worksheet / WS-2026-0415". Nothing runs under the select, and the log is empty.
  - K-S: the selection bar is one line again, with Fill with, the select, Apply and Clear wells grouped at the end (J's regression is closed).
  - K1, K8.
- Remaining:
  - low, accepted at 785 px: the head is about 82 px tall and the subtitle takes two lines. The mockup has one 55 px line, but at its 1000 px page.

**390**
- Regressions: none.
- Batch K, closed:
  - K-S: a rule under the Row A heading (y 180, (237,240,243)); J had none.
  - K-S: the badge reads "STD".
  - K12: the head is shorter.
- Remaining:
  - low, data: rows D–H are listed one by one; the mockup groups them as "Rows D–H".

## Results

**1000**
- Regressions: none.
- Batch K, closed:
  - K5: "System suitability" is on one line.
  - K6: both of the dialog's rules are (237,240,243). J's head rule was (221,225,230).
  - K8: the dialog keeps its shadow, a step under its 1 px border (rows 579–585), over the scrim.
  - K3: "In progress" and the principal are on one line.
  - K1.
- Remaining:
  - low, accepted: "In progress" sits beside the principal, not at the far right.

**390**
- Regressions: none.
- Batch K, closed:
  - K-S: the label reads "Reason for change", with room above it.
  - K12.
- Remaining:
  - low, screen fix: Undo change and Confirm change are drawn inline. The mockup's compact layout has no such buttons.
  - low, screen fix: Impurity A's caption is 30 px under the buttons, against 22 px in the mockup (under the comment) and 15 in J.
  - data: "of 2" against "of 8".

## Review

**1000**
- **Regression (medium): "99.4 % LC" in the Result column wraps to "99.4 % / LC".**
  - Screen fix: K-S narrowed Result from `fixed(9)` to `fixed(8)`. Restore `fixed(9)`.
  - **Framework gap (read, matches the grab):** the column says `.wrap = false`, and the spec says `wrap: false` draws a column's text cells on one line, "such as a value with its unit". But its `cell` is a text with `spans`, and `MorphText.oneLine` is `(singleLine || role === 6) && !hasRuns`. The row sets `singleLine`, which is then ignored for rich text, without a report.
  - This is a candidate library defect. It is vocal-over-silent: either honour `wrap: false` for a spans cell, or report it.
- Batch K, closed:
  - K1: the selected WS id is (28,30,33). In J it was the accent, (23,120,74).
  - K-S: the full evaluation texts are back.
  - K-S: "Prior lot" header.
  - K-S: the note sits beside Approve.
  - K8.
- **Batch K, open (K-S): the queue.** The 21/82 split widened the queue only from about 155 px to 163 px.
  - Still broken: "Awaiting review · 4" wraps; the ages sit under the IDs; the tags stack; the placeholder reads "Worksheet, lot or an…".
  - K-S's own comment (`review.cpp`) claims "room for its title, search placeholder and ages on one line". The grab shows otherwise.
  - Medium, screen fix: about 25/78 splitter units (the mockup's queue is about 280 px of 1000).
- Remaining:
  - low, screen fix: the evaluation is a tag and a muted note wrapped under it ("Out of spec / above 0.20", "Out of trend / −0.7 % vs. / last 5 lots"), so the two rows are 48–56 px tall. The mockup has one tag line per row, "Out of spec: above 0.20".
- Shortened texts:
  - "Out of spec" and "Out of trend" no longer lose anything. "above 0.20" and "−0.7 % vs. last 5 lots" are both shown, at both widths.
  - "Spec." loses nothing.
  - "Prior lot" is unambiguous and replaces J's "Previous". The mockup says "Previous lot".

**390**
- Regressions: none.
- Batch K, closed:
  - K-S: the full tag texts ("Out of spec: above 0.20", "Out of trend: −0.7 % vs. last 5 lots").
  - K-S: no comment card.
  - K12.
- Remaining:
  - low, screen fix: 43 px between the last result and "Review comment", against 27 in the mockup. The `room` spacer in `recordTabs` adds to the page's gap.
  - low: the tab reads "Prior lots" where the mockup has "Previous". It is clearer, so accept it.
  - accepted: the back link sits under the tags.

## Console

**1000**
- **Regression (medium, screen fix): the worklist now scrolls sideways.** A horizontal scroll bar is drawn under the rows (y ≈ 744).
  - Cause (read): K-S made Test `content()`. The table's columns are wider than the pane, and `MorphTable` scrolls and shows a bar in that case, as `backend.md` specifies.
  - The mockup clips Status with no bar. K-S's comment ("the table clips its last column, Status, as the design does") does not hold.
  - Fix: Test `stretch()` with `wrap: false` at a width that fits, or narrower fixed columns.
- **Regression (medium, screen fix):** Due now shows "22 min over…"; J and the mockup show "22 min overdue". Fix: Due back to `fixed(12)`.
- **Regression (low, screen fix):** at `fixed(6)` the Priority tags fill their column. "Routine" touches its tag's right edge with no padding, and "Result entry" and "Registered" do the same in Status. Fix: Priority `fixed(7)`.
- Batch K, closed:
  - K-S: Test is shown in full.
  - K-S: the worklist and its selection bar reach the pane's edges.
  - K1, K8.
  - K3: the principal is on the actions' line.
- **Batch K, open (K-S): one-line analyte labels.**
  - All four labels still take two lines ("Assay · / 95.0–105.0", "Impurity A · ≤ / 0.10", "Impurity B · ≤ / 0.20", "Total impurities · / ≤ 0.50"). Two of them split "≤" from its number. The mockup wraps only Total impurities.
  - Medium, screen fix: the pane is about 300 px against the mockup's ≈ 360. Give it more splitter units once the worklist fits, or put a no-break space in "≤ 0.10".
- Remaining:
  - low, kind unverified: the Priority tags are filled; the mockup draws them outlined (`STAT`, `Urgent`, `Routine` with a border). Present in J too.
  - accepted: the sidebar width and the lab select's position.

**390**
- Regressions: none.
- Batch K, closed:
  - K12: the dark head is 52 px (y 0–51).
  - K1: the selected row's text and id are (28,30,33), not green.
  - K8.
- K13 check: the scan glyph moved 4 px right (bright pixels x 357–365, against 353–361 in J). So K13's "unchanged" verify does not hold literally, but the glyph is now about 29 px from the right edge against the mockup's ≈ 26. That is closer, so it is not a regression.
- Remaining: none.

## Summary

- **Regressions: 5.**
  - medium, screen fix:
    - stability 390: check boxes against the panel border (exposed by K7; `padding` on the condition panel);
    - review 1000: "99.4 % / LC";
    - console 1000: a sideways scroll bar;
    - console 1000: "22 min over…".
  - low, screen fix: console 1000, the Priority and Status tags crowd their edges.
  - Review's wrap also points at a **framework gap**: `wrap: false` is silently ignored for a cell text with `spans` (read, consistent with the grab).
- **Open batch K items: 2.** Both are K-S screen items:
  - review's queue is still squeezed at 21/82;
  - console's analyte labels still take two lines.

  Every framework item visible on these screens is closed: K1, K2/K3, K5, K6, K7, K8, K10, K11, K12. K13's glyph moved 4 px, nearer the mockup.
- **Remaining differences: 13**, besides the open items.
  - 0 high, 0 medium.
  - 13 low:
    - 9 screen fixes: stability's hint lines and First pull break; results 390's inline Undo/Confirm and its caption gap; review's two-line evaluation cells; review 390's gap above the comment and its "Prior lots" label; plate 1000's two-line head (accepted at 785 px); results' "In progress" position (accepted).
    - 1 of unverified kind: console's filled priority tags.
    - 3 data: plate's "Rows D–H"; results' "of 2"; review's "All 7" (against the mockup's 24).
  - The framework gap is counted under the review regression.
