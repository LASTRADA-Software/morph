# Lane O-F: batch O framework

Read $S/lg-o-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) first; its rules bind.
Worktree `/home/yaraslau/repo/morph-lims-lf` (build/linux-everything is current for lg-nf; reuse it). Branch `lg-of` from 227b4f06b.
Design (authoritative, reviewed twice; the "Review round 2" section records the resolutions): $S/lg-o-design.md. Read it whole.
Scope: lane O-F's items in its order — O5 (panel fill follows the frame radius, Fixes #1076), O1b (terminal hidden cell), O4 (terminal raises a badge to its natural width, contract fixed, Fixes #1075), O1 (content-aware stretch table columns, the place() re-entry guard, widthFreeNatural, naturalAdvance, conformance cases). One commit per item with its spec updates, tests and the mutation control the design lists (run each control, say it failed). You own include/morph, src, tests, docs/spec; do not edit examples/.
After O1, build and run `lims_gallery_tests` and the lims-gallery render smokes; write gallery failures to $S/lg-of-handoff.md for lane O-S (it owns test_review.cpp and the gallery), do not fix them.
Qt layout loops are the main risk: after O1 grab review, samples and stability at 1215x830 and 1000x830 with `timeout 60` and confirm each finishes with an empty log (no `Binding loop`, no hang). Report the review column widths you measure.
Issues go in commit messages as `Fixes #1075` / `Fixes #1076` lines, never in code comments.
