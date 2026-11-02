# Lane P-S: batch P screen items S1–S10

Read $S/lg-p-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-e2` (build/linux-everything is current for `lg-os` d0e2013be). Create branch `lg-ps` from `lg-os` (`git checkout -b lg-ps lg-os`). Only edit examples/lims_gallery.
Do items S1–S10 of "Lane P-S: screen items" in $S/lg-p-design.md, one commit per item (or per screen when items share a function). S11–S15 wait for the framework lane — do not start them.
For each: grab before and after at 1215x830 and 1000x830 (390x844 for S5 and any phone-visible change), put PNGs in $S/lg-ps/, Read them, compare with $S/lg-cmp/mock/*.png, report before/after px. Check 1000 for clipping. Gallery tests and render smoke green, update test strings that change; mutate one item's test to show it is caught. clang-format 22 on changed files; clang-tidy on changed .cpp files. Do not push.
