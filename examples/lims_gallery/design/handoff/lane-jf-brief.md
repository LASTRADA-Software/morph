## Lane J-F: fit the LIMS gallery's screens to the 785 px page, and the remaining screen fixes

Worktree `/home/yaraslau/repo/morph-lims-e1`. Create branch `lg-jf` from `lg-fold-i` (`git checkout -b lg-jf lg-fold-i`;
the worktree is clean on `lg-iv`, which is already contained in `lg-fold-i`). Read `AGENTS.md` first.

Batch I made the sidebar show at 1000 px (`expanded` from 108 units), so the page beside it is about 785 px wide. Several
screens were laid out for 1000 px and now clip or squeeze. Two reviewers compared the grabs after batch I with the
mockups; their reports are `$S/lg-ir-findings-A.md` and `$S/lg-ir-findings-B.md`
(`S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad`). Mockup renders are in
`$S/lg-cmp/mock/`, mockup sources in `examples/lims_gallery/design/mockups/*.dc.html`, the after-batch-I grabs in
`$S/lg-cmp/app-i/`.

**Your scope: screen sources only** (`examples/lims_gallery/src/screens/*.cpp`, the gallery's own tests and theme
bundle). Do not change `include/morph`, `src/` or QML: a separate design is handling the framework gaps (compact white
page, inline-lead wrap order, silent clipping, `bleed` inside a `panel`, dialog rule spacing, text area padding, field
clipping, overline spacing, nav selection fill, principal in the head, review 390's gap and All/Flagged stretch). Lane
W, on `lg-fold-i` in another worktree, is fixing the `darkHead` `color.track` warning and the `nav.*` icon warning —
leave both alone.

### Items

High, at 1000 (fit the 785 px page; the mockup's proportions are the target, not its pixel widths):
1. **audit**: the trail runs past the page edge, Reason hidden, "Approve|d" breaks, the chip row is cut off. `wrap` on
   the chip row; narrower fixed columns and/or facets so the table fits.
2. **samples**: Product · lot squeezed to ~70 px, rows 66–83 px tall (design 31). Rebalance the columns (e.g. Tests
   stretches, Sample/Due/Specification narrower).
3. **receipt**: Sample type and Received at show the end of their text. Narrow the `sessionTable` column (`fixed(36)`)
   or use 2 grid columns at this width. The "Already received" banner wraps (low).
4. **stability**: the conditions table drops Pulls and breaks condition names. A minimum for Condition, narrower
   timepoint columns, or a narrower summary.
5. **review**: the footer overflows; "Approve and sign…" cut off; Evaluation tags clipped; summary tags wrap.
6. **console**: worklist Test truncated to ~4 characters, Status clipped. Re-size the splitter or columns (the mockup
   clips only Status).
7. **instrument**: Recent events wrap; left pane 405 px with the right column `fixed(44)` — try ~`fixed(36)`.
8. **plate**: "6 wells selected · A3–A8" breaks inside the range (keep the count text whole). The head's trail wrapping
   is a framework item — leave it.

Low / medium screen fixes:
- stability 390: footer "Continue" on compact, "Continue to tests" on expanded.
- plate 390: dividers between the well rows (`divided` forEach).
- console 390: worklist rows in a white card with dividers (`panel` around a divided forEach).
- e-signature 390: "Approved" plain on compact (compact branch of the "Signing at…" spans).
- receipt: a rule above Reject / Receive (`divided` column); the "Sample S-…" heading 14 px bold (`role`).
- instrument: Recent events `divided`; compact Current booking one line "WS-2026-0412 · M. Kaur", the time, no progress
  bar.
- samples 390: tag row about 6 px under the text line (gap on the card column).
- audit 1000: check box rows 22 px apart (they are 18) if a screen-level gap does it.

### Verify

- Grab every screen you touch at 1000x830 and 390x844 and Read the PNGs, comparing with the mockup:
  `QT_QPA_PLATFORM=offscreen build/linux-everything/examples/client/morph_examples_client --app lims-gallery --ui=qt --screen lims-gallery.<id> --grab-size 1000x830 --grab $S/lg-jf-<id>-1000.png`.
  Nothing clips, nothing breaks mid-word at 1000, and 390 does not regress.
- Build `build/linux-everything` (configure from the `linux-everything` preset if missing) with no warnings; run
  `lims_gallery_tests` and `gallery_tests`, then full `ctest -j10` at the end. Update the gallery tests that pin sizes.
- clang-tidy 22.1.8 on your changed lines (binary
  `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/clang_tidy/data/bin/clang-tidy`, `-p build/linux-everything
  -extra-arg=-std=c++23`), clang-format 22 (`$S/lg-cf22/bin/clang-format -i`) on changed files.
- Update the "After batch I" section of `examples/lims_gallery/design/visual-review.md`: correct its two false claims
  (the receipt session table's header band does not reach the card's edges; the principal does not show in the grabs)
  and record what you fixed.

### Rules

- Implementation agent: no Agent/Workflow, /code-review, /simplify, `run_in_background`, `&`, `nohup`, `pkill -f`,
  Monitor, or `git stash` (the stash stack is shared across worktrees). Wrap every build, test, grab batch and
  clang-tidy run in `flock $S/morph-build.lock`, at most `-j10`. Scratch files are named `$S/lg-jf-*`.
- Comments state what the code does now; no history, no issue numbers.
- Commit per screen (or a few screens per commit), messages saying what changed, ending with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn
  ```
  Do not push, rebase or touch other branches.
- Report: per screen what changed and what the grab now shows, test results, and anything left that needs the
  framework.
