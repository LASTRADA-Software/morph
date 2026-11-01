# Lane O-S: batch O screen fixes

Read $S/lg-o-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-e2` (build/linux-everything is current for lg-ns, nearly the same code). Branch `lg-os` from 227b4f06b. Only edit examples/lims_gallery.

Fix every *screen fix* and regression in $S/lg-nr-findings-A.md and $S/lg-nr-findings-B.md, one commit per screen. Among them:
- Regressions: receipt's Outcome tags elide at 1000 (pane `percent(32)` or Sample `fixed(10)`, pick by grab); audit's Reason breaks "OOS-2026-031" at 1000 (`percent(21)`); samples' Status "Investigat…" (`fixed(11)`); console's out-of-spec input 3 px left (`fixed(10)` on the tag column, verify the cause); review's Analyte/Spec elide at 1215 (make them `content()` and let Evaluation/Change shrink — a better content-aware stretch is coming from a framework lane; do the best the current vocabulary allows and say what remains).
- Then the low items: stability no-break spaces ("1 m", "40 °C") and phone panel gap 12; plate phone well rows; results Result inputs ~110 px and whether Submit should be enabled in the mockup's state; review self-review note in the muted small text role and phone textarea; console head height; samples bulk bar y; instrument recent events 29 px apart.
- **Re-check every table with a tag column** on every screen at 1000 and 1215 for words cut by the new eliding rule (badges elide only when a table column is narrower than the tag); widen the column where a mockup shows the whole word.
Skip data-only differences. Grab every changed screen at 1000x830, 1215x830, 390x844 (1920x1080 for review, receipt, audit, stability), Read the PNGs, compare with $S/lg-cmp/mock/*.png and report before/after px. Mutate one pane/column test to show the gallery tests catch it.
