# Lane N-F: batch N framework

Read $S/lg-n-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-lf` (it has an older build/linux-everything; reuse it). Branch `lg-nf` from a30f51c93.
Scope: the section "### N-F" of $S/lg-n-design.md — N3, N2, N1, N4, N5 in that order, one commit per item, plus the conformance cases, spec updates and tests it lists. You own include/morph, src, tests, docs/spec. Do not edit examples/ (except: none). N4's probe runs first and decides whether its fix ships (report instead if it shows >= 1 px).
After N1 and after N5, build and run `lims_gallery_tests`; any gallery failure is written to $S/lg-nf-handoff.md (append, test name + output) for lane N-S, not fixed by you.
Issues: N2 is morph#1073, N4 is morph#1074 — mention them in commit messages as `Fixes #1073` / `Fixes #1074` lines (not in code comments).
Watch: Qt binding loops (grep test output for `Binding loop`), every Sizing::Kind site incl. QML integer comparisons (the design lists them), Doxygen on every new public symbol, qmllint clean on Qt 6.8.1 (build dir build/lg-qt681-* if one exists in this worktree; otherwise skip and say so).
