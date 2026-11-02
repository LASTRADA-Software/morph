# Lane P-F: batch P framework items

Read $S/lg-p-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-lf` (currently on `lg-of`, clean). Create branch `lg-pf` from `ad3cc392d` (batch O fold, `git checkout -b lg-pf ad3cc392d`), rebuild build/linux-everything under the lock.
You own include/morph, src, tests, docs/spec — not examples/lims_gallery (lane P-S owns it; examples/gallery and forms examples only if a test there must change, and say so).

Implement the framework items of $S/lg-p-design.md as revised (read "Review round 2" and $S/lg-p-review.md), one commit per item in the design's order: P4 (grid equal columns; Fixes #1077 in the commit message), P2 (`captionWidth`), P1 (`control.touchTextSize`, `Theme.controlFontOf`), P3 (`button.smallHeight`/`smallTouchHeight`, round button never taller than wide), P5 (`caption` role), P6 (stacked captions use the caption font). Every mutation control the design lists must be run and reported as failing.
Before reporting: every suite with a touched test, the three conformance runners, the docs target (clang, WARN_AS_ERROR), clang-tidy on touched TUs, qmllint, clang-format 22, the branch-coverage allowlist check, and ui coverage ≥ 92% if you can measure it. Hand back the grabs the design asks for (receipt 1215/1920, e-signature, plate 1215/390, review 390, the gallery's Containers page, a forms dialog) in $S/lg-pf/, and write gallery-test failures you cannot fix in your own scope to $S/lg-pf-handoff.md. Do not push.
