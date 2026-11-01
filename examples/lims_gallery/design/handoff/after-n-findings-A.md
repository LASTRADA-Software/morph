Reviewer A, after batch N: esignature, receipt, instrument, audit and samples, at 1000, 1215 and 390, plus receipt
and audit at 1920. Worktree `lg-fold-n`. The brief points at `app-l/` for the new grabs, but the after-N grabs are in
`lg-cmp/app-n/`, dated 20:20 and with 1920 grabs present. `app-l/` (15:17) is the after-L baseline. All `app-n` logs
for these screens are 0 bytes.

Method:
- Each grab was compared with its mockup render (and the `.dc.html` where needed) and with `app-l`.
- The l→n changed regions were found with an image diff (threshold 8).
- Pixel values were measured with PIL. Anything marked *read* comes from the code, not from a measurement.

Severity is H/M/L. *screen* means the vocabulary already expresses the fix; *framework* means ui/1 cannot.

## E-signature

| Width | Finding |
|---|---|
| all | **Regressions:** none. At 1000 and 1215 only the head and the shadow changed. 390 is pixel-identical. |
| 1000/1215 | **M `color.dialogRule`: closed.** Both dialog rules (y 225 and 622) are now (230,233,237) = #E6E9ED; they were (237,240,243). |
| 1000/1215 | **M shadow fade: closed.** Below the dialog the shadow steps by 1–3 levels from (135,137,142) to the scrim's (167,169,173). There is no 3-level hard tail and no visible ring; at L both were visible. |
| 1000/1215 | **N5 head: visible.** The head rule is at y 46; it was 38. The mockup draws no head here. |
| all | L, framework: the summary box border is (221,225,230) #DDE1E6. It is a `panel().surface("page")`, so it takes the card border. The design draws it in #E6E9ED, like the rules. A panel cannot take the dialog-rule colour (read: `esignature.cpp:92`). |

## Receipt

| Width | Finding |
|---|---|
| 1000 | **Regression, M, screen.** The session pane's Outcome badges are cut to "Recei…" and "Reject…". The pane is 750–983 (234 px); at L it was 746–983 (238 px), from `percent(31)` against L's `fixed(31)`. Now that a tag elides in a table cell (N2), the 4 px it lost cut the word. Fix: `percent(32)` (about 241 at 1000 and still capped at 300), or Sample `fixed(10)`. Then re-check that the card's "Release, finished product" and "24-Oct-2026 09:41" still fit. |
| 1215 | **N1/S5 receipt pane: closed.** The pane is 900–1198, 299 px against the design's 300. |
| 1920 | **N1 cap: closed.** The pane is 1604–1903, 300 px, so it stops at 39 units. The card takes the rest; its three field columns are about 440 px each. Nothing breaks, and the page reads as intended. |
| 1000/1215/390 | **M `color.placeholder`: closed.** "Waiting for scan…" and "Optional" have their darkest pixel at (132,133,135); at L it was (77,84,97). That matches the design's measured grey. |
| 1215 | **N5 head: closed within tolerance.** The rule is at 46 against the mockup's 49, the recorded known delta. |
| 1000/1215 | L, not a gallery gap: the scan field has no 3 px #CDEBD8 halo; the design always draws one. `MorphInputBackground` draws its prominent ring only while the field is focused (read: `toneRing` `visible`). The grab's field had no active focus, so this is inferred to be a grab artifact, not verified. |
| 390 | No change apart from the placeholders. No clipping. |

## Instrument

| Width | Finding |
|---|---|
| all | **Regressions:** none. At 1000 and 1215 only the head band moved down 8 px (N5) and the column widths changed (M). 390 is pixel-identical. |
| 1215 | **M right column: closed.** The column is 885–1198, 314 px against 320; the design drops this as fine. At L it was 230. |
| 1215 | **M event spacing: mostly closed.** The event baselines are 27 px apart (ink rows 347, 374, 401), with a rule between them. At L they were 23; the design has 29. L, screen: a row `gap`/padding of 1 more px per side would match. |
| 1215 | N5 head: the tab rule is at y 115 against the mockup's 121 below its card top. That is within the head's 12 px padding plus the design's 22 px top inset; not a finding. |
| 1000 | No clipping. The right column is 234 px and "next 14-Jan-2027" fits. |

## Audit

| Width | Finding |
|---|---|
| 1000 | **Regression, M, screen: a mid-word break.** Row 1's Reason now wraps "OOS-2026-03 / 1 closed as lab error." At L, "OOS-2026-031" was whole on its own line. The rail is 173 px (`percent(22)` of 785); it was 169. The spec's S5 acceptance ("no crushed Reason at 1000; else `percent(21)`") failed and the fallback was not taken. Fix: `percent(21)` (165 at 1000, 210 at 1215, as at L). |
| 1215 | **N1/S5 rail: closed.** The rail is 215–435, 220 px; the design has 220. Reason gets about 260 px and rows are 2–3 lines. |
| 1920 | **N1 cap: closed.** The rail is 223 px, at its 29-unit max. Reason runs on one line; the page looks intended. |
| 1000/1215 | **M note before the chips: closed.** "4 entries · times in CEST (UTC+2)" leads the chips, as in the design. |
| 1000/1215 | **N4 / no stray scroll bar:** none under the table at any width. |
| 390 | Pixel-identical to L. No clipping. |

## Samples

| Width | Finding |
|---|---|
| 1000/1215 | **Regression, M, screen.** S-26-104159's Status tag reads "Investigat…"; at L it read "Investigation" at both widths. The badge's fill is the same 73 px in both grabs, so the column did not shrink. Status is `fixed(10)`, and its room was already narrower than the tag's natural width; at L the label simply ran past the room into the cell inset. N2's elision now cuts it. Fix: Status `fixed(11)`. *Inferred* from `MorphBadge.qml`/`MorphTableRow.place()` and the unchanged fill width; the room was not measured in Qt. |
| 390 | **M Filters count: closed.** It reads "Filters · 2", as in the design. The placeholder is the new grey. |
| 1215 | Rows stay one line, about 31 px. No stray table scroll bar. |
| 1215 | L, screen/theme: the head rule is at y 55 (47 at L). The bulk bar's top is at y 72, against the design's 54 below the window top: the head holds 32 px controls plus 2×12 padding, and the rule and gap come on top. The spec expected 48; it did not count the controls. A known delta that grew, not breakage. |
| 1000 | Product and Tests wrap (fit on the 785 px page). Apart from "Investigat…", nothing is clipped or broken mid-word. |

## Summary

- **Regressions: 3, all medium and all screen fixes.** Each follows from batch N's own changes:
  - **Receipt 1000:** "Recei…"/"Reject…". The pane is 4 px narrower under `percent(31)`, and N2 now elides. Fix: `percent(32)` or Sample `fixed(10)`.
  - **Audit 1000:** the mid-word break "OOS-2026-03/1". The S5 acceptance failed and the fallback `percent(21)` was not taken.
  - **Samples 1000/1215:** "Investigat…". `fixed(10)` Status was already too narrow, and N2 made it visible. Fix: `fixed(11)`.

  N2 changed how a table cell treats a tag that overflows: it now elides instead of drawing past. Every table with a tag cell should be re-checked for this, which the lane did not do.
- **Batch M/N items on these screens:**
  - **Closed:**
    - `color.dialogRule` and the shadow fade (esignature);
    - `color.placeholder` (receipt, samples);
    - receipt pane 300 and audit rail 220 at 1215, and both caps at 1920 (N1/S5);
    - instrument's right column, audit's note order and samples' "Filters · 2" (M screen fixes);
    - N5 heads, within ±3 where the spec set a target (receipt 46/49);
    - no stray table scroll bar.
  - **Partly open:** instrument's event spacing is 27 against 29 (L).
  - **Failed acceptance:** audit's S5 rail at 1000; this is the regression above.
- **Remaining differences:**
  - **By severity:** 0 H, 3 M (the regressions), 3 L.
  - **Screen, 5:** the 3 regressions; instrument's event spacing; samples' taller head (the head-padding delta).
  - **Framework, 1:** the e-signature summary border cannot take the dialog-rule colour.
  - **Not counted:** receipt's missing focus halo, which is inferred to be a grab artifact (the field had no focus).
- **1000 px check:** clipping, lost content and mid-word breaks were found only at the three regressions above. There is no overlap.
- **Library defects to file:** none. Each elision is N2 working as designed on a column that was too narrow.
