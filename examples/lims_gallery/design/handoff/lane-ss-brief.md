# Lane S-S: batch S screen items U1–U3

Read $S/lg-s-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-e2` (build current for `lg-rs` 0d4b8a54e). Create branch `lg-ss` from `lg-rs`. Only edit examples/lims_gallery. Build ALL targets.
Do U1–U3 of $S/lg-s-design.md, one commit each, as the design says (including U3's fallback). U4+ wait for the framework lane.
Grab before/after at 1215x830, 1000x830, 390x844 as relevant into $S/lg-ss/, Read them, compare with $S/lg-cmp/mock/*.png, report px. Full ctest green (the render smoke may rarely segfault in Qt's GC, #1083 — rerun it alone and report the count); one mutation control; clang-format 22, clang-tidy on changed files. Do not push.
