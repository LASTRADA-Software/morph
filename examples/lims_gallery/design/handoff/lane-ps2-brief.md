# Lane P-S2: batch P screen items after the framework (S11–S19)

Read $S/lg-p-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-e2`, branch `lg-ps`, already rebased onto `lg-pf` (P-F's framework: P4 equal grid columns, P2 `captionWidth`, P1 `control.touchTextSize`, P3 `button.smallHeight`/`smallTouchHeight`, P5 `caption` role, P6). Rebuild build/linux-everything first, under the lock. Only edit examples/lims_gallery.
Do items S11–S19 of "Lane P-S: screen items" in $S/lg-p-design.md (as revised; read "Review round 2"), one commit per item. P-F's handoff:
- S11: `captionWidth` 12 (e-signature record box), 17 (instrument details), 13 (stability).
- S12: gallery theme `button.smallHeight` 40, `button.smallTouchHeight` 36; plate's compact run line `padding(1, 1)`. Measure the phone row pitch (45 px target, rows with and without Assign) and the wells at 1000 (about 35 × 35, round).
- S13: gallery theme `control.touchTextSize` 14; re-grab ALL ten screens at 390x844; no row of buttons may wrap or clip — report any that does.
- S14, S16–S18: `.role = "caption"` on review's self-review note, console's KPI notes and control-chart note, review's "Approving needs your signature".
- S15: receipt column 2 at 563 ± 2 (1215) and 916 ± 2 (1920); re-grab every screen with a `columns` grid (receipt's field rows, stability, e-signature's fields, the two phone `columns` grids) at 1215, 1000 and 390 for wraps.
- S19 as the design says (instrument's phone stacked captions, P6).
Grabs in $S/lg-ps2/, Read them, compare with $S/lg-cmp/mock/*.png, report before/after px. Gallery tests and render smoke green; update test strings that change; one mutation control. clang-format 22 and clang-tidy on changed files. Do not push.
