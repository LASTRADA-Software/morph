Reviewer A, after batch P: esignature, receipt, instrument, audit and samples, at 1000, 1215 and 390, plus receipt and
audit at 1920. Grabs are in `lg-cmp/app-p/`, and the baseline is `app-o/`. All 34 logs are 0 bytes. Sources were read
with `git show lg-fold-p:<path>`.

**Method.**
- An o→p image diff, threshold 8, per grab.
- PIL pixel reads for edges, gaps and text extents, checked against the mockup PNG and the `.dc.html`.
- *Measured* means a pixel read. *Inferred* means read from code or reasoned, not measured.

**o→p diff boxes.**
- **Audit and samples, 1000, 1215 and 1920:** pixel-identical to O.
- **Audit and samples, 390:** only the controls changed (14 px text).
- **E-signature:** only the record box changed (the caption column).
- **Receipt:** only the Condition column 2 changed at the desktop widths. The controls changed at 390.
- **Instrument:** the cards changed at the desktop widths. Only the action buttons changed at 390.

## E-signature

| Width | Finding |
|---|---|
| all | **Regressions:** none. |
| 1000/1215 | **P S11 `captionWidth` 12: closed.** Measured: the caption is at x 388 and the value at 495, 107 px apart. The design has 96 + a 10 px gap = 106. Flags still fits on one line, where the design wraps "linked". The cause is the narrower mono fallback font, so this is not counted. |
| 1000/1215 | **P4 equal grid columns: closed.** Signer is 379–605 and Meaning is 612–838, 226 px each. |
| 390 | **P S13, control text 14 px: closed.** The inputs, the select, the textarea and "Sign and approve" are larger, as in the design. The Reason text now wraps after "Impurity B;", as the design's does. |
| 1000 | No clipping, overlap, lost content or mid-word break. |
| — | **Remaining differences:** none beyond the font face. The expanded record id is regular weight, and the design's `.mono` is 400 there; only the compact id is 600, and ours is bold there. |

## Receipt

| Width | Finding |
|---|---|
| all | **Regressions:** none. |
| 1215 | **P4/S15, Condition column 2 at 563: closed.** Measured: the checkbox is at x 563, which matches the accepted 563 ± 2. The field rows are equal: Sample type, Temperature and Received at are each 205 px. |
| 1920 | **P4/S15, column 2 at 916: closed.** The cap holds: the pane is about 1605–1903. The three field columns are equal at about 441 px, and the page reads as intended. |
| 390 | **P S13, 14 px: closed.** The values, the selects and both footer buttons are larger. **P4:** Containers and Courier are equal, 175 px each (16–190 and 199–373). "Internal, QA sampling" still fits at 14 px. |
| 1000 | No clipping. "Release, finished product" fits its 153 px field. The notice wraps at a word ("…Nothing was / changed."), which is the known fit note. |
| 1215 | **L, framework (new to this review; also present at O; inferred cause).** The "Condition on receipt" legend is 13 px against the design's 12 px `.lbl`. Measured: the cap height is 10 px, against 9 px for "Sample type", "Storage location" and "Note". `ui::group` has no key for the legend's size, and the legend is not drawn in the field-caption font that a field's label uses. **Fix:** draw a group's legend in `Theme.captionFont`, as a field caption is drawn. |
| — | Accepted, not re-reported: the 14 px "Sample" heading, and the 4 px row gap. |

## Instrument

| Width | Finding |
|---|---|
| all | **Regressions:** none. |
| 1000/1215 | **P S1, Qualification rules: closed.** Rules are measured at y 187 and 214. The row pitch is 27 px (174/201/228) against the design's 29, which falls under the accepted "rows 27 vs 29". |
| 1000/1215 | **P S2, event pitch:** still 27 px (362/389/416) against 29. This is accepted (4 px steps), so it is not counted. |
| 1000/1215 | **P S3, card insets 12 px: closed.** "Details" sits 12 px inside its card at x 232→244. The design has 14; the difference is within a step. |
| 1000/1215 | **P S11 `captionWidth` 17: closed.** The caption is at x 245 and the value at 390, 145 px apart. The design has 130 + a 12 px gap = 142. |
| 390 | **P S13, 14 px: closed** on Book, Log maintenance and ⋯. |
| 390 | **P S19 / P6, stacked captions in the caption font: not visible. M, framework or a stale grab (cause not verified).** The region is pixel-identical to O. "Location" is 43 px wide at 390, the same as the inline 13 px body caption at 1215. "Methods" is 44 px at both widths, and the caption and value cap heights are both 9 px. A 12 px caption would be about 3 px narrower. `compactDetails()` uses `keyValue` with `fieldLayout = Stacked`, which maps to `CaptionPlacement::Above` (`interpreter.hpp:8069`), so `MorphKeyValue.qml` should take `Theme.captionFont`, which is 12 px. **Either the grab predates 1da5632ea, or the stacked path does not reach that Label.** Re-grab, or test, before filing. |
| 1000/1215 | **L, screen (new to this review; also present at O).** The cards are 8 px apart, both between Details and Recent events (y 309→318) and between the columns (x 875→884). The design has `gap:14px`. **Fix:** `gap = 3` (12 px) on the two columns and the row in `instrument.cpp:335–343`. |
| 1000 | No clipping. "next 14-Jan-2027" fits. |

## Audit

| Width | Finding |
|---|---|
| 1000/1215/1920 | **Regressions:** none; these grabs are pixel-identical to O. The 1920 cap and the whole "OOS-2026-031" at 1000 still hold. |
| 390 | **Regressions:** none. **P S13, 14 px: closed** on Export PDF, Filters · 4, Export CSV and Load 25 more. |
| 390 | **L, framework (also present at O).** Filters · 4 and Export CSV are both `.sizing(ui::stretch())`, but they are unequal: 179 and 175 px at P, 181 and 173 at O. The design has `flex:1`, equal. See the summary's library finding. |
| 390 | Not counted: "Load 25 more" is grey because it is disabled; the fixture has 4 entries. |
| — | Accepted, not re-reported: the rail at 210 px, and the data. |

## Samples

| Width | Finding |
|---|---|
| 1000/1215 | **Regressions:** none; these grabs are pixel-identical to O. |
| 390 | **Regressions:** none. **P S13, 14 px: closed** on Register, the search, Filters · 2, the Sort select and Assign to worksheet. |
| 390 | **M, framework (also present at O).** Filters · 2 (a stretch button) is 16–173, 158 px. Sort (a stretch select) is 178–373, 196 px. The design draws them equal, at about 173 px each. The widths follow the content because the two siblings share by content, not by weight. This is the same cause as on audit. |
| — | Accepted, not re-reported: the bulk bar at 70, and rows of about 30 px. |

## Summary

**Regressions: 0.**

**Batch P items on these screens:**
- **Closed, 9:**
  - S11 `captionWidth` on e-signature and instrument;
  - P4 equal columns: receipt at 563 and 916, the e-signature fields, and the containers/courier pair at 390;
  - S1 Qualification rules;
  - S3 card insets;
  - S13 phone control text at 14 px, on all five screens.
- **Open, 1:** S19 / P6, instrument's stacked captions. They measure at the body size, and the grab is identical to O. The cause is not verified.
- **Accepted, not counted:** S2, instrument's 27 px pitch (event and qualification rows).

**Remaining differences: 0 H, 2 M, 3 L.**
- **Framework, 3 (2 M, 1 L):**
  - M: two `stretch` siblings in a row share by content on Qt (samples 390, audit 390).
  - M: S19, stacked `keyValue` captions are not drawn at 12 px (pending a re-grab).
  - L: the `group` legend is drawn at 13 px, not the 12 px caption size (receipt).
- **Screen, 1:**
  - L: instrument's cards are 8 px apart against 14. Fix: `gap = 3`.
- **Not counted:** the e-signature Flags wrap (font), audit's disabled Load more (fixture), and the 1000-px fit wraps.

**1000-px check:** no clipping, overlap, lost content or mid-word break on any of the five screens.

**1920:** the receipt pane and the audit rail stop at the mockup width. The rest of each page fills as intended.

**Possible library finding: measured on two screens, cause inferred; for the dispatcher to file.** On Qt, two
`stretch(1)` siblings in a row get unequal widths. The difference tracks their content:
- samples 390: 158 and 196 px;
- audit 390: 179 and 175 px.

`docs/spec/tui/frontend.md` says a stack "shares what is left among Stretch children by weight", after fixed and
content children. That makes equal weights equal on the terminal. Qt appears to grow each stretch child from its
natural width, as CSS `flex: 1 1 auto` does, not as `flex: 1` does. This is the row counterpart of the grid-columns
divergence that P4 fixed. It appears twice in `examples/`, so by AGENTS.md it is a framework gap, not two screen bugs.

What I did not verify:
- I did not read `MorphStack.qml`.
- I did not run the terminal.

**What would close it:** two `stretch(1)` siblings in a Qt row come out equal (±1 px) whatever their content.
