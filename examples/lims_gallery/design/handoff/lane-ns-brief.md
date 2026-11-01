# Lane N-S: batch N screens

Read $S/lg-n-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-e2` (build/linux-everything exists). Branch `lg-ns` from a30f51c93. Only edit examples/lims_gallery.
Scope: the section "### N-S" of $S/lg-n-design.md — S1 to S7. Start with the items that need no new API (S1 first). Lane N-F works in parallel on branch `lg-nf` (worktree /home/yaraslau/repo/morph-lims-lf), one commit per item in the order N3, N2, N1, N4, N5. When an item you need has landed there (`git log --oneline a30f51c93..lg-nf`), `git rebase lg-nf` (or cherry-pick its commits) and continue; never edit N-F's files. If N-F is not done with what you need, finish everything else first, then wait by checking `git log` every few minutes with plain foreground commands (no sleep loops in background).
Also fix the gallery test failures N-F hands you in $S/lg-nf-handoff.md (check it after each rebase).
Grab every screen you change at 1000x830, 1215x830, 390x844, and 1920x1080 for the panes with a max; Read them and compare with the mockup renders $S/lg-cmp/mock/*.png. Report before/after measurements in px.
