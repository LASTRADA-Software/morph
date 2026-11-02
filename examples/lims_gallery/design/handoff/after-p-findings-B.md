# After batch P: stability, plate, results, review, console (findings B)

**Method.** I read every `app-p/` grab for these five screens: 1000, 1215 and 390, plus 1920 for stability and review.
I set each one against its `app-o/` grab and its mockup render. Pixel values are PIL measurements on those PNGs.

**Sources.** Code was read with `git show lg-fold-p:<path>`. Causes marked *read* come from that code, not from a run.
Nothing was built or run.

**What changed.** All 17 grabs differ from O. The Qt logs are empty (0 bytes) for all 17.

**The 1000 px check.**
- stability: no breakage.
  - The summary is wider (29 units), so "30 °C / 65 % RH · / intermediate" and "accelerated" now wrap, at word
    boundaries. This is a fit difference and is not counted.
  - The summary's own values break only at word boundaries ("Paracetamol / 500 mg", as in the mockup).
- plate: no breakage. The wells are 36 px at a 39 px pitch. The selected A3–A8 rings meet edge to edge (see plate).
- results: no breakage. Analyte wraps under the dialog, which is a fit difference carried over from O.
- review: Analyte and Evaluation elide, which is accepted (A1). The footer no longer wraps "Approving needs your
  signature", so it is better than O.
- **console: a number breaks in the middle (regression).** The pane's first line reads "Assay · / 95.0–105. / 0". O
  had "Assay · / 95.0–105.0". See console.

**The 1920 check.**
- Stability's summary is capped at 1650–1904 (254 px). The grid and the stepper fill the rest, and the page looks
  intended.
- Review's queue is capped at 215–499. The stretch columns share what is left, Change is whole ("0.12 → 0.21 ·
  integration corrected") and nothing elides. The page looks intended.

## Stability

| width | finding |
|---|---|
| 1215 | Batch P, **closed** (S4): First pull reads "03‑Dec‑2026 / (40 °C, 1 m)", breaking before the parenthesis as the mockup does. Same at 1000 and 1920. |
| 1215 | Batch P, **closed** (S11): the caption column is about 115 px (956–1071), against the mockup's 110 (784–894). "180 needed, 150 in / stock" wraps, as in the mockup. |
| 1000 | Batch P, **closed** (summary at least 29 units): no value breaks inside a word. |
| 390 | Batch P, **closed** (S5): the 30 °C and 40 °C panels, Add condition and the Summary are 12 px apart. Measured edges: 575→588, 633→646, 689→702. In O, Add condition to the Summary was 8 px. Add condition is 44 px tall (646–689), as in the mockup. |
| 390 | Batch P, **closed** (S13): the inputs, Add condition, Back and Continue draw 14 px text. |
| all | Regressions: none. Remaining: none beyond the accepted rows (30 against 29 px). |

## Plate

| width | finding |
|---|---|
| 1215 | Batch P, **closed** (S12, desktop): the wells are 40 × 40 (569–608 across, 198–237 down) at a 44 px row pitch. The mockup's `.well` is 40 px with a 4 px gap. This also closes O's regression (30 px wells). |
| 390 | Batch P, **closed** (S12, phone): the wells are 36 × 36 (20–55, 199–234). The rule pitch is 45 for every A line, with or without Assign: rules at 194, 239, 284, 329 and 374. |
| 390 | Batch P, **closed** (S13): Auto-fill samples, Save layout and Assign… draw 14 px text. |
| 1000 | Batch P, **partly closed** (S12, the round rule at 1000): the wells are 36 px at a 39 px pitch, but the 3 px selected rings of A3–A8 meet edge to edge. For example, A3's right ring is at 621–623 and A4's left ring starts there. The design asked that no ring overlap its neighbour. **Low.** They touch without overlapping, and this is no worse than O. |
| 390 | Remaining, **low, screen fix**: the well sits 5 px from its label (56→61); the mockup has 10 (`.wl` gap 10px). Fix: `.gap = 2` or `3` on the run line's row (`plate.cpp:678`, `.gap = 1` now). |
| 390 | Remaining, **low, screen fix (*inferred*)**: the collapsed "Row B … Row H" lines are 50 px apart (424, 474, 524 …); the mockup's are 45. The cause was not traced. |
| 1215 | Remaining, **low, framework gap (*inferred*)**: the legend chips are outline badges, small stadiums about 18 px tall. The mockup's are 26 px round wells (`.lg-i span.well`). Missing: a badge diameter or size. Not reported after O; unchanged since then. |
| 390 | Remaining, **low, data** (carried over): rows D–H are listed separately, and A3–A8 is one line. |

## Results

| width | finding |
|---|---|
| 1215 | Batch P, **closed** (S6): Submit for review is enabled under the reason dialog, as in the mockup. |
| 1215 | Batch P, **closed** (S7): the comment wraps "…reintegrated per / SOP‑CHR‑012.", the same at 1000 and 390. |
| 390 | Batch P, **closed** (S13): the inputs, the select, the textarea and the four buttons draw 14 px text. "Save draft" is 61 px wide, against 53 in O. |
| 390 | **Regression, low, framework gap**: the two stretch buttons in a row are less equal than in O. Save draft is 16–185 and Submit is 189–374, so about 170 against 186 (O: 176 against 180). Previous and Next behave the same way. The mockup's pairs are equal, 174 and 174 with an 8 px gap. Cause (*inferred*): Qt shares the leftover space between `stretch()` siblings from their content width, so larger text widens the gap between them. CSS `flex: 1` shares from zero. Screen workaround: `ui::grid(…, {.columns = 2})`, now equal on Qt (P4). The same applies to plate's phone Auto-fill / Save layout (16–197 against 202–374). |
| 390 | Remaining, **low, screen fix**: those pairs are 4 px apart against 8. Fix: `.gap = 2` (`results.cpp:650`, and the Previous/Next row). Review's phone footer (4 against about 8) and plate's (5 against 8) are the same. |
| 390 | Remaining, **low, data** (carried over): "of 2" (the mockup has 8). |

## Review

| width | finding |
|---|---|
| 1215 | Batch P, **closed** (S8): the comment card starts 12 px under the table (291→303). It was 1 px in O. The mockup has about 11. |
| 1215 | Batch P, **closed** (S14): the self-review note is a 12 px muted caption, and it now wraps on two lines as in the mockup. |
| 1215 | Batch P, **closed** (S18): "Approving needs your signature" is a 12 px caption. At 1000 it no longer wraps, where O wrapped it onto two lines. |
| 390 | Batch P, **closed** (S13): the textarea placeholder and Return… / Reject… / Approve… are 14 px. This closes the known-open 13 against 14 px textarea gap. |
| 1215 | Remaining, **low, screen fix (unverified)**: the comment card's title "Comment from M. Kaur · …" uses the muted `section` role. The mockup draws it in the body colour (`<b style="font-size:12px">`). Fix: a 12 px semi-bold text in the body colour, for example a strong span with the caption role. I did not check that the two combine. |
| 1000 | Accepted (A1): Analyte and Evaluation elide. Not counted. |
| all | Remaining, **low, data** (carried over): 7 results, not 24. Regressions: none. |

## Console

| width | finding |
|---|---|
| 1215 | Batch P, **closed** (S9): the result inputs are 76 px (989–1065), against the mockup's 75. |
| 1215 | Batch P, **closed** (S10): the Instruments rows are 26 px apart (264, 290, 316, 342, 368), as in the mockup. |
| 1215 | Batch P, **closed** (S16, S17): the KPI notes and "mean 100.0 % · SD 0.4" are 12 px muted captions. Their ink is 2 px shorter than in O, and the colour is (77, 84, 97). |
| 390 | Batch P, **closed** (S16): "18 on hold" and "2 STAT" are captions. |
| 1000 | **Regression, medium, screen fix**: the first result line reads "Assay · / 95.0–105. / 0", which splits a specification limit inside the number. A reader can take it as 105. The cause is the wider input (S9: `fixed(8)` to `fixed(10)`, `console.cpp:676`), which narrows the stretch label to about 64 px. That is less than the width of "95.0–105.0", so Qt breaks it anywhere. The design said 1000 must not clip. Fix options: allow a break after the en dash (U+200B after "–" in the spec), or let the label take the line above the input on a narrow pane. |
| 390 | Batch P, **open** (S13), **low, framework gap**: "Open run" in the banner is still 13 px. Its ink is 47 px wide in both O and P, while every ordinary button on these screens grew. Cause (*inferred*): the banner's action button (`BannerActionStyle::Button`, `console.cpp:467`) does not take `control.touchTextSize`. This is a defect in the banner's control, not in the screen. |
| 1215 | Accepted (A3): the head is 53 px against 49. Not counted. |

## Summary

**Regressions: 2**
- **console 1000, medium, screen fix**: the spec "95.0–105.0" breaks inside the number, "105. / 0". This is a side
  effect of S9's wider inputs.
- **results and plate 390, low, framework gap**: the paired `stretch()` buttons became more unequal (about 170 against
  186 px) once the text grew to 14 px. The mockup's pairs are equal.

**Not counted (fit at 785 px):** stability's condition labels wrap at 1000; results' Analyte wraps at 1000.

**Batch P items on these screens: 15 closed, 1 partly closed, 1 open.**
- **Closed:**
  - stability: S4, S5, S11, the 29-unit summary, and S13 at 390.
  - plate: S12 desktop 40 px; S12 phone 36 px with a 45 px pitch.
  - results: S6, S7, and S13 at 390.
  - review: S8, S14, S18, and S13 at 390.
  - console: S9 at 1215; S10; S16 and S17.
- **Partly closed:** S12 at 1000. The wells are 36 px, but the selected rings meet edge to edge.
- **Open:** S13 on console 390. The banner's "Open run" is still 13 px.

**Remaining differences: 10** (0 high, 1 medium, 9 low). The two regressions are counted here.

- **Medium, screen fix (1):** console 1000, the spec breaks mid-number.
- **Low, screen fixes (4):**
  - plate 390: the well-to-label gap is 5 against 10.
  - plate 390: the collapsed row lines are 50 against 45 px (*inferred*).
  - phone button pairs are 4 against 8 px apart, on results, review and plate.
  - review: the comment-card title is muted where the mockup uses the body colour.
- **Low, framework gaps (3):**
  - phone pairs of `stretch()` buttons are unequal on Qt, on results and plate.
  - the banner action button ignores `control.touchTextSize`, on console.
  - a badge has no diameter for plate's 26 px legend wells (*inferred*).
- **Low, not ranked by kind (1):** plate 1000's selected rings touch.
- **Low, data (counted as 1):** plate's row grouping, results' "of 2", and review's 7 against 24.
