# Lane O-S2: batch O screen follow-up (after O-F)

Read $S/lg-o-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-e2`, branch `lg-os`, already rebased onto `lg-of` (O-F's framework: O5 panel fill radius, O1b, O4 terminal badge, O1 content-aware stretch columns). Head bbc37400f. Only edit examples/lims_gallery. Rebuild first (build/linux-everything), under the flock.

Read $S/lg-o-design.md sections R1–R4 (table near line 499) and $S/lg-of-handoff.md. Do, one commit per item:
1. **R1 review columns**: Analyte `stretch()` wrap false + `shrink`; Spec. `content()` wrap false; Evaluation `stretch()` wrap false, no shrink; Change `stretch()` wrap false + `shrink`. Queue `percent(27)` max 37. Comments state why (current behaviour only). Update `test_review.cpp` sizing strings.
2. **R1 end-to-end case** in `test_lims_gallery_render.cpp`: review at 1215 and 1000 — no sideways scroll at 1000, Evaluation tag whole at 1215. Run its control once against O1 reverted (revert 0012dc684's framework part locally, rebuild, see it fail, restore, touch files, rebuild).
3. **R3**: e-signature summary box corner pixels after O5 — the fill no longer shows square past the rounded border (grab, sample a corner pixel, report).
4. **control.height 30 trial**: set the gallery theme's `control.height` to 30 (mockup's). Re-grab ALL 10 screens at 1215x830 and 1000x830, compare with $S/lg-cmp/mock/*.png. Keep it if console's head and samples' bulk bar move toward the mockup and nothing breaks (text clipped in controls, tests); else revert and report why. Fix tests it changes.
5. **R2 re-grabs**: review in both views at 1215/1000/1920/390; samples at 1215/1000 and once with a search filter; stability at 1000 (breakage only); audit spot check at 1000. Also check review's muted self-review note colour vs mockup and the phone textarea height (13 vs 14 px) — fix if a screen change can.
Put grabs in $S/lg-os2/. Report before/after px for each item. Then run the full gallery tests, the render smoke, clang-format 22 on changed files, and clang-tidy on changed .cpp files. Do not push.
