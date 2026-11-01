# Lane L-S — screen fixes (worktree /home/yaraslau/repo/morph-lims-e2, branch lg-ls)
Fix in examples/lims_gallery/src/screens only, every finding marked *screen* in both findings files:
- Regressions: stability 390 check boxes at the condition panel border (panel padding); review 1000 "99.4 % LC" split (Result column width); console 1000 worklist horizontal scroll (Test column `content()` → a width that fits without scroll, clipping like the mockup), Due cell cut, Priority tags flush; receipt 1000 session pane message wrap; audit 1000 "In review" wrap; samples 1000 Tests column wrap.
- Not landed: review's queue at 1000 (try about 25/78 so the title, ages and tags fit and the placeholder isn't cut); console analyte labels on one line, "≤" kept with its number (use a non-breaking space U+00A0 or U+202F); audit Reason column really wider (rows ≈88 px like the design); samples rows ≈31 px.
- Low screen items: audit date chip format; audit note beside the chips; samples "Filters · 2" if the mock data has 2 active filters.
Don't touch include/morph or src (other lanes do); if a fix needs the framework, list it in your report instead.
Check every 1000 and 390 grab of the screens you change, before and after; nothing may regress elsewhere on the screen.
