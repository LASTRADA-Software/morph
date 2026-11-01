# Lane M-S — screen fixes (worktree /home/yaraslau/repo/morph-lims-e2, branch lg-ms from 24d939686)
Every item marked *screen fix* in both findings files. Judge desktop fit on 1215x830 grabs (page ≈1000 px like the
mockups); at 1000x830 nothing may break (clip, overlap, lose content). Notably:
- Results 390: the compact layout has no Undo/Confirm in the mockup and Submit is enabled (green) once the reason and comment are filled — make the screen match (Submit enabled per the mockup's state).
- Review: splitter sizes are absolute units, not proportions; queue ≈36 units (≈280 px), title back to the heading size; fix the wrong comment near review.cpp:823. Check 1000 still doesn't break (table may need to scroll/elide — report what happens).
- Receipt card:pane stretch(2):stretch(1); instrument right column ≈320 px; instrument recent-event rows ≈29 px apart; audit note before the chips; audit rail ≈220 px; samples "Filters · 2" (decision: count every filter that has a value, defaults included, as the mockup does); stability "(40 / °C" break; console Instruments panel stretches to the chart's height; console units beside inputs; review's two-line evaluation cells; any other low screen item in the reports.
Don't touch include/morph or src (lane M-F does). List anything that needs the framework in your report.
