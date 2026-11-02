# Lane R-S2: batch R screen items after the framework (T5–T10)

Read $S/lg-r-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-e2`, branch `lg-rs`, already rebased onto `lg-rf` (R-F: R4 key-value baseline, R3 `actions.gap`/`actionBar` in place, R2 `HeadTrailEnd`, R1 input ellipsis, R5 `grid.minColumnWidth`), with T2, T3 and a MorphText whole-pixel fix on top. Rebuild ALL targets under the lock. Only edit examples/lims_gallery. Read R-F's handoff $S/lg-rf-handoff.md first.
Do T5–T10 of the R-S table in $S/lg-r-design.md (as revised), one commit per item that changes code; measuring items get reported. T1 is solved by the MorphText fix (verify plate's head is one line at 1215 and 1920, 52 px tall vs the mockup); T4 was reverted and the 8 px rail inset is accepted.
T10 re-grabs every screen at 1215, 1000 and 390 (1920 for receipt, audit, stability, review): report every new wrap, clip, overflow log, mid-word break or "…" in an input; fix what is breakage.
Note: the render smoke can segfault inside Qt's GC under load (#1083, being fixed by another lane): if it crashes, rerun it alone; a crash is not your failure, but report how often you saw it.
Grabs in $S/lg-rs2/; compare with $S/lg-cmp/mock/*.png and $S/lg-cmp/app-q/; report before/after px. Full ctest green; one mutation control; clang-format 22 and clang-tidy on changed files. Do not push.
