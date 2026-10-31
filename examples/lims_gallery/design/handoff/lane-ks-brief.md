## Lane K-S: the screen fixes left after batch J

Worktree `/home/yaraslau/repo/morph-lims-e3`. Run `git checkout -b lg-ks 300cee8c1` (the worktree is clean on
`lg-jc`; `300cee8c1` is PR #1045's head with batches A–J). Read `AGENTS.md` first. Its `build/linux-everything` was last
built at `lg-jc`; rebuild first.

Two reviewers compared the grabs after batch J with the mockups:
`$S/lg-jr-findings-A.md` and `$S/lg-jr-findings-B.md`
(`S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad`). Grabs:
`$S/lg-cmp/app-j/`; mockup renders `$S/lg-cmp/mock/`, sources `examples/lims_gallery/design/mockups/*.dc.html`.

**Your scope: screen sources only** (`examples/lims_gallery/src/screens/*.cpp`, the gallery's tests and bundle). A
design agent is handling the framework items (the accent selection recolouring content, the head wrapping with the
principal on plate and samples, the principal's alignment in a multi-line head, a switch case's holder gap, results'
"System suitability" wrap, dialog rule colours, check box inset, card shadows, the date input's calendar icon) — do not
work around those in screens.

### Items (every one marked "screen" in the reports, except the two head-wrap items)

- **review**: keep the full evaluation texts ("Out of spec: above 0.20", "Out of trend: −0.7 % vs. last 5 lots") in
  the data at both widths; let the expanded cell wrap (or shorten only the expanded cell's display if it cannot fit).
  Re-balance the queue splitter (the 20/83 squeeze: "Awaiting review · 4" wraps, ages fall under IDs, the search
  placeholder is cut) — try ~25/78, keeping the footer whole. "Previous" header → an unambiguous short label. Footer
  note beside Approve (spacer before the note). 390: drop the comment card the compact mockup does not draw.
- **plate 1000**: the selection bar — Fill with / select / Apply / Clear wells as one group pushed to the end, no
  wrap. 390: a rule under "Row A"; "STD" not "STD1"; rows D–H as "Rows D–H" if the data allows.
- **stability 1000**: condition labels on one line (narrower timepoint columns or summary); the table to the card's
  edges with `.bleed()`.
- **console**: 1000 Test column in full (clip only Status, as the mockup does); the sample pane's analyte labels on one
  line except "Total impurities"; the worklist rows and selection bar to the pane's edges (`.bleed()`). 390: dark head
  ~52 px.
- **results**: 390 "Reason for change" label with a gap under the out-of-spec line.
- **receipt**: 3 grid columns as the design (narrow the session column, ~`fixed(30)`); 12–13 px around the divider
  above Reject/Receive; the "Already received" banner on one line if a screen change does it; the sample heading 14 px
  bold at both widths; 390: "· 2–8 °C" in the label instead of "Required range" under the field.
- **instrument 390**: ~30 px between "Current booking" and its first line.
- **audit 1000**: Reason gets a larger share (rows ~88 px, not 135); the date chip "20–24 Oct 2026" on one line; the
  head count and the pager agree. 390: "Load 25 more" button; Export PDF 16 px from the head's edges.
- **samples 1000**: Product · lot 3 : Tests 2 (or narrower Sample/Due/Specification) so rows approach 31 px; Priority
  tags muted grey. 390: Filters row 8 px under search; Register not flush to the edge; "Filters · 2".
- **e-signature 390**: Signer → Meaning gap — if a screen-level column with `gap` fixes it, do it, and say so (the
  framework may also fix the holder gap; that is fine).

### Verify

- The render smoke (`lims_gallery_render_smoke`) fails on any layout overflow; it must stay green.
- Grab every screen you touch at 1000x830 and 390x844 with
  `QT_QPA_PLATFORM=offscreen build/linux-everything/examples/client/morph_examples_client --app lims-gallery --ui=qt --user "Dr. A. Meyer" --screen lims-gallery.<id> --grab-size WxH --grab $S/lg-ks-<id>-<w>.png`
  and Read them against the mockup. Nothing clips, nothing breaks mid-word, no warning on stderr.
- Build with no warnings; `lims_gallery_tests`, `gallery_tests`, then full `ctest -j10`. Update tests that pin sizes
  or texts.
- clang-tidy 22.1.8 on your changed lines (`/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/clang_tidy/data/bin/clang-tidy -p build/linux-everything -extra-arg=-std=c++23`); read the WHOLE output, not its tail.
  clang-format 22 (`$S/lg-cf22/bin/clang-format -i`) on changed files.
- Do not write `visual-review.md`; report what the grabs show.

### Rules

- Implementation agent: no Agent/Workflow, /code-review, /simplify, `run_in_background`, `&`, `nohup`, `pkill -f`,
  Monitor, or `git stash`. Wrap every build, test, grab batch and clang-tidy run in `flock $S/morph-build.lock`, at
  most `-j10`. Scratch files `$S/lg-ks-*`.
- Comments state what the code does now; no history, no issue numbers.
- Commit per screen, messages saying what changed, trailer `Batch-Item: screen-fixes`, ending with:
  ```
  Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn
  ```
  Do not push, rebase or touch other branches.
- Report: per screen what changed and what the grab shows, tests, and anything left that needs the framework.
