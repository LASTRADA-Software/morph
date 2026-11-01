Reviewer A, after batch L: esignature, receipt, instrument, audit and samples, at 1000, 1215 and 390. Worktree
`lg-fold-l` at `24d939686`. Each grab was compared with its mockup render and source and with `app-k`. Pixel values were
measured with PIL, and the k→l changed regions were found with an image diff. Anything else is marked as read.
All 15 `app-l/*.log` files for these screens are 0 bytes.

Short version: **no regressions**. Every batch L item that shows on these five screens is closed, except samples'
"Filters · 0". No 1000 grab has clipping, overlap, a mid-word break or lost content. What remains is low polish.

Severity is H/M/L. *screen* means the vocabulary already expresses the fix; *framework* means ui/1 cannot.

## E-signature

| Width | Finding |
|---|---|
| all | **Regressions:** none. The k→l diff covers only the dialog shadow and the button captions. |
| 1000/1215 | **L4 closed.** The shadow is now blurred and offset downward. It falls from (131,135,149) just under the dialog to the scrim's (167,169,173) about 66 px below, about 40 px at the sides and about 28 px above. That matches `0 18px 48px`. |
| 1000/1215 | **L8 closed.** "Sign and approve", "Cancel" and the head's "Sign…" draw at medium weight. **L9 closed:** they are upright. |
| 390 | **L8 closed:** "Sign and approve" is medium. Nothing else changed. |
| 1000/1215 | L, framework: the shadow's outermost step is a hard 3-level edge, 164 → 167. It shows as a faint rounded ring about 60 px outside the dialog, so the blur looks layered rather than continuous. The fix is a smoother falloff, or dithering at the shadow's tail. |
| all | L, framework (still open from K): both dialog rules are `color.divider` (237,240,243). The design draws them, and the summary border, in #E6E9ED. One token cannot hold both values. |

## Receipt

| Width | Finding |
|---|---|
| all | **Regressions:** none. |
| 1000 | **K regression closed** (`fixed(31)`): "S-26-104228 rejected: seal broken." is on one line. Clipping check: "Release, finished product" and "24-Oct-2026 09:41" with the calendar button still fit, with a few px to spare. |
| 390 | **L6 closed:** the label reads "Temperature on arrival \* · 2–8 °C", as in the design (`labelHint`). **L8 closed:** "Receive, next" is medium. |
| 1215 | L, screen: the session pane stays 236 px wide (962–1198); the design's is 300. The card takes the extra width instead. A fix is a ratio, e.g. card `stretch(2)` and pane `stretch(1)`, which still leaves about 256 px at the 785 px page, or a minimum width. |
| 1000/1215 | L, framework: the scan field's placeholder "Waiting for scan…" draws at (77,84,97), in the input's 600 mono weight, so it reads like typed content. The design's placeholder is a lighter grey: its darkest pixel is about (132,133,135). Every input sets `placeholderTextColor: scheme.muted` (read: `MorphTextInput.qml:87` and the others), and there is no placeholder token. |

## Instrument

| Width | Finding |
|---|---|
| all | **Regressions:** none. At 1000 only the Book caption changed. |
| 390 | **L3 closed:** the eyebrow ink starts at y 16 (it was 8); the design has about 20. The tabs and the body moved down by the same 8 px, and nothing is clipped. The "Calibration due in 3 days" badge ends at x 378. |
| 1000/1215 | **L8/L9 closed:** Book and Log maintenance are medium and upright. |
| 1215 | L, screen: the right column (Qualification, Current booking) is 230 px wide (968–1198); the design's is 320. Its sizing could take a ratio or a larger `fixed`. |
| 1215 | L, screen: the Recent events rows are about 23 px apart; the design's are 29, with a rule between them. A larger row `gap` or padding would fix it. |

## Audit

| Width | Finding |
|---|---|
| all | **Regressions:** none. |
| 1000/1215 | **L10 closed:** the date chip reads "20–24 Oct 2026" (`dateRange`). |
| 1000/1215 | **L-S note beside the chips: closed.** "4 entries · times in CEST (UTC+2)" sits on the chips' line at both widths. |
| 1000/1215 | **K regression closed:** "In review" is on one line (Old `fixed(7)`). |
| 1215 | **L-S wider Reason: closed.** Reason is 888–1198, about 310 px. Rows are 2–3 lines, 48–66 px, which is tighter than the design's 88 px, because the page here is wider than the design's table. |
| 1000 | Reason is about 90 px and rows wrap to 5–6 lines, up to 138 px tall. That is fit on the 785 px page, not breakage. "24-/Oct-2026" breaks at the hyphen, as the design's own render does. No clipping. |
| 390 | **L2 closed:** "Load 25 more" draws disabled, with a grey fill and a muted label. **L3 closed:** Export PDF's top is at y 12 (it was 4); the design has 12. |
| 1000/1215 | **L2 visible:** both pager arrows draw disabled, which is correct for "1–4 of 4". |
| 1000/1215 | L, screen: the entries note comes after the chips; the design puts it before them ("57 entries · …" then the chips). Fix: move `entriesNote` to the front of the row in `chips()`. |
| 1215 | L, screen: the filter rail is 170 px; the design's is 220. The rail's `fixed` width could grow. |

## Samples

| Width | Finding |
|---|---|
| all | **Regressions:** none. |
| 1215 | **L-S rows ≈31 px: closed.** Rows are one line, about 30 px, with Product and Tests unwrapped, as in the design. |
| 1000 | Product and Tests still wrap and rows are about 48 px. That is fit on the 785 px page; nothing is clipped or broken mid-word. |
| 1000/1215/390 | **L7 closed:** Routine is a quiet outlined tag. At 1215 it has a (195,201,210) border and (77,84,97) text, the same values as the design's `.p-rou`. At 390 it changed from `color.text` to muted. |
| 390 | **L3 closed:** Register's top is at y 12 (it was 4); the design has 12. |
| 390 | **Batch L item open.** L, screen: "Filters · 0"; the design has "· 2". The `active` count leaves out the defaults "Status: open" and "Priority: any", and the lane did not change it. Whether those defaults count is a product decision. If they do, count every set facet. |

## Summary

- **Regressions:** 0. All three K regressions on these screens are closed: receipt's rejection wrap, audit's "In review" wrap and the compact bar inset.
- **Batch L items on these screens:**
  - **Closed:** L2, L3 (×3 screens), L4, L6, L7, L8, L9 and L10. Of the L-S screen items, receipt's pane, audit's Old value, Reason width and note beside the chips, and samples' one-line rows at 1215 are closed.
  - **Open:** 1. Samples' "Filters · 0" (low, screen) was in the L-S brief only conditionally, and it is unchanged.
- **Remaining differences:** 9, all low. None is high or medium.
  - **Screen, 6:**
    - receipt's session pane width at 1215;
    - instrument's right column width;
    - instrument's event row spacing;
    - audit's note order;
    - audit's rail width;
    - samples' Filters count (this is the open L item).

    The receipt pane and instrument's column take the same fix: a ratio instead of `fixed`.
  - **Framework, 3:**
    - the shadow's hard outer step (a faint ring);
    - one divider token for the dialog rules and the table rules (carried over from K);
    - no placeholder colour token, since placeholders draw `muted` at the input's weight.
- **1000 px clipping check:** none on any of the five screens.
- **Library defects to file:** none new.
  - The placeholder colour is a design-token gap, not a defect.
  - The shadow ring is polish. It was observed in one grab on this machine and inferred to be a layered blur; I did not read the shadow's QML.
