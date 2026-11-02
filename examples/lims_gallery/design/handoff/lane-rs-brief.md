# Lane R-S: batch R screen items T1–T4

Read $S/lg-r-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-e2` (build/linux-everything current for `lg-qs` 689731075). Create branch `lg-rs` from `lg-qs`. Only edit examples/lims_gallery. Build ALL targets (including `lims_gallery_render_smoke`).
Do T1–T4 of the R-S table in $S/lg-r-design.md, one commit each, exactly as the table says (including its revert/fallback conditions). Items after T4 wait for the framework lane — do not start them.
For each: grab before and after at 1215x830 and 1000x830 (1920x1080 for audit), PNGs in $S/lg-rs/, Read them, compare with $S/lg-cmp/mock/*.png, report before/after px. Check 1000 for clipping and mid-word breaks. Full ctest green; one mutation control; clang-format 22 and clang-tidy on changed files. Do not push.
