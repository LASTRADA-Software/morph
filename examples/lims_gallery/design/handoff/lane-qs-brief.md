# Lane Q-S: batch Q screen items S1–S4

Read $S/lg-q-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-e2` (build/linux-everything current for `lg-ps` e8b601a75; remove the untracked `Testing/` dir if it is a ctest leftover). Create branch `lg-qs` from `lg-ps`. Only edit examples/lims_gallery.
Do S1–S4 of the screen-item table in $S/lg-q-design.md (instrument card gap, plate well-to-label gap, phone button-pair gaps, console spec break at 1000), one commit each. S5+ wait for the framework lane — do not start them.
For each: grab before and after at 1215x830, 1000x830 and 390x844 as relevant, PNGs in $S/lg-qs/, Read them, compare with $S/lg-cmp/mock/*.png, report before/after px; check 1000 for clipping and mid-word breaks. Gallery tests and render smoke green; one mutation control; clang-format 22 and clang-tidy on changed files. Do not push.
