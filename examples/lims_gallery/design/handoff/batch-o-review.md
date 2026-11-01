# Review of batch O design (`lg-o-design.md`)

I checked the design against `/home/yaraslau/repo/morph-lims-e1` at `227b4f06b` (`lg-fold-n`), reading only. I did not
build, run or edit anything there. Paths below are relative to that worktree unless they start with `$S/`. Pixel
numbers are **measured** with PIL on `$S/lg-cmp/app-n/`. Everything else is **read** from code, or **inferred** where I
say so.

**Verdict.** O1 is the right idea, and making it the default (not opt-in) is the right call, with S6's conditions. As
written, though, it is not ready:
- the Qt loop analysis models the echo wrongly (B1, B2);
- the probe that decides `naturalAdvance` runs on the wrong Qt (B3);
- two of the three conformance cases cannot pass as specified (B4);
- review's acceptance cannot be met in the "All" view, even with the fallback (B5).

O2 is right. It has one measured side defect (S10). O3's contract contradiction is real (S11).

**Counts: 5 blockers, 12 should-fixes.**

---

## Premises I verified

| Design claim | Status | Evidence |
|---|---|---|
| P1: table 517–1198 at 1215, stretch shares about 1:1:3:1 | measured, holds | header bg (250,251,252) from x 517 to 1197; column starts from header ink: Analyte 612, Spec. 757, Evaluation 912, Change 1127, so Eval is 215 and Change 71; L ≈ 430 |
| 1 unit ≈ 7.7 px (so the 8-unit floor ≈ 61.7 px) | measured | samples-1215: Priority `fixed(7)` 819→889 = 70 = 7u+16; Status `fixed(10)` 889→982 = 93 = 10u+16 (`samples.cpp:514-520`) |
| Evaluation tag width | measured | amber fill (255,241,214) from 920 to 1107 = 188 px at 1215, so n ≈ 204 |
| P1 at 1000: table 457–983 | measured | 527 px, L ≈ 276 |
| P4 Qt: only content columns read the cache; stretch takes a weighted share with floors; exact sum | holds | `MorphTable.qml:89-150`, `:173-178`, `:239-244`; `reportCellWidth` has no kind check, `:215-226` |
| P4 terminal: stretch starts at 0, minimum header or `min(h,4)`, deficit from last over all columns | holds | `table_widget.cpp:234-249`, `:39`; `layout.cpp:161-182`, `:121-130` |
| P5: table `implicitWidth` follows its width | holds | `MorphTable.qml:326`; header `Row` of label widths `:418-426`; row `implicitWidth = Σwidths`, `MorphTableRow.qml:110,144` |
| P6: the terminal never word-wraps (plain or runs) | holds | `leaf_widgets.cpp:146-185`; `paintRuns(multiline)` only honours line feeds, `widget.cpp:895-925` |
| P9 / O1b: terminal hidden cell widens its column | holds (read) | `table_widget.cpp:220-229` has no `shown()` check on cells; Qt skips at `MorphTable.qml:191` |
| One-stretch table unchanged | holds on both | step 1 gives `L`; steps 2–4 give `max(L, f)`, as `MorphTable.qml:121-127` and `distribute` do today |
| No other example has a multi-stretch table | holds | an absent table-column width is `content()` (`interpreter.hpp:9206`); `lims_screens.cpp:193` gives none |
| O2 premises P7, P8 | hold | border (221,225,230) at (600,238) of esignature-1215; `esignature.cpp:92`; `lims_gallery.cpp:48`, pane and selected surfaces |
| O3 contradiction | holds (read) | `backend_contract.md:263` vs `:278`; `widget.cpp:613-623`, `:701-708`; badge natural `display_widgets.cpp:261-263`, painted unelided outside a table `:265-269` |

---

## Blockers

### B1. The echo is synchronous re-entry of `place()`, not polish passes, and re-entry leaves stale positions

The design calls follower convergence "polish passes" (design :221, :340). On Qt it is synchronous recursion. Every
link is a `DirectConnection` or an eager binding:
1. `place()` sets `cell.width` (`MorphTableRow.qml:92` or `:96`);
2. the follower's `implicitWidthChanged` re-enters `place` (`table.cpp:348-353`) and calls `reportCellWidth`
   (`table.cpp:366-373`);
3. that reassigns `columnCache` (`MorphTable.qml:221`);
4. `widths` re-evaluates (`:77`), and so does the row's `widths` (`MorphTableRow.qml:25`);
5. `onWidthsChanged: place()` runs (`:132`), nested inside the first `place`.

The outer frame then resumes with stale locals:
- `const w` was read before the recursion (`:88`);
- `x += w` (`:107`) moves every later cell by the old width;
- `cells.implicitWidth = x` (`:110`) overwrites the inner, correct result.

The outermost frame writes last, so cells end up out of line with the header. Today this cannot happen: a content
column's width equals its cell's width plus the padding, a fixed point, and stretch reports change nothing in
`columnWidths`. Under O1, any report that changes a stretch column's natural or least width while `place` runs changes
`widths` mid-loop. That includes the `singleLine` flip at `:102`, which changes `MorphText.implicitWidth` and
`wrapMinimumWidth` (`MorphText.qml:21,52-76`) the first time each runs cell is placed. Depth grows with the pass
count, and the QV4 call-depth limit is finite.

**Fix.**
- Make `place()` re-entrancy-safe: set a `placing` guard and a `dirty` flag, then loop while dirty. Or snapshot
  `widths` once and re-run if it changed by the end.
- Better still, move the report out of the placement call stack: `Qt.callLater` in `reportCellWidth`, or a
  `QueuedConnection` in `table.cpp:366`. Each echo pass then becomes a real event-loop pass that a test can count.
- State this in `qt_quick/backend.md`.

### B2. The "bounded echo" guard settles, but on a history-dependent layout, and its test cannot tell rounding from no rounding

**History.** Under the four steps, a follower's natural *is* its last width (`rescanColumn` reads `implicitWidth`,
`MorphTable.qml:192,206`), so:
- in step 1 the follower absorbs the whole spare;
- after a narrowing, step 3 takes the deficit from the *other* columns first, last to first, down to their floors,
  while the follower keeps its echoed width.

The result is exactly what O1 exists to remove: siblings elide while one column holds empty space, and it persists
until the rows change. Opening the window narrow gives a different layout.

**Test.** Without `Math.ceil`, the echo still terminates when `s·f` falls under half an ULP of `n`. For two equal
weights (f = 0.5) and s ≈ 500 px, that is about log2(500 / 5.7e-14) ≈ 53 passes, well inside the proposed 200. The
"control: without whole-pixel rounding the test reaches its pass limit" (design :343) therefore fails to fail. There is
also no pass counter to read, because of B1.

**Wrong follower.** A `QQmlExpression` binding `implicitWidth` to `width` is the synchronous kind. Batch N's hang came
through a layout pass (`git show 1f14614f6`: `MorphSplitter.paneExtents`). A nested `MorphTable` follows through
its header `Row`, a positioner. The test does not exercise that path.

**Fix.**
- Remove the follower at its source for the known case. Add a read-only `naturalWidth` to `MorphTable`: `lead` plus
  the sum of each column's natural, computed from the caches and not from `width`. Have the cell report read it.
  `MorphTable.implicitWidth` is untouched, so the `GridLayout` risk of Decision 8 does not arise.
- Generalise `naturalAdvance` into one `MorphWidget` property. Name it, for example, `widthFreeNatural`: -1 when
  unknown. `MorphText`, `MorphTable`, `MorphStack` and `MorphPanel` (propagating) set it, and the report prefers it.
- Keep whole pixels as defence in depth, not as the argument.
- Test with a nested `MorphTable` cell in a stretch column, widened and then narrowed. Assert the sibling stretch
  columns get the same widths as a table built at the final width. That check fails under the current design.
- Count passes with a test-only counter, as `rowScans` does (`MorphTable.qml:76`).

### B3. The `naturalAdvance` probe runs where the follower cannot appear

The comment the design rests on says the label follows its width "on Qt 6.8" (`MorphText.qml:45-47`). CI is Qt 6.8.1
(`.github/workflows/ci.yml:155`, `nightly-slow-checks.yml:47`). This machine has Qt 6.12.0 (`qt6-base 6.12.0-3`).
Design :214-216 drops `naturalAdvance` if a local probe shows no follow. That measures the feature on a configuration
where it cannot appear (AGENTS.md, "Verify rather than assert").

**Fix.** Keep `naturalAdvance` (or B2's generic property) unconditionally. It is correct by construction and cheap.
If the probe is kept, run it on Qt 6.8.1 (the CI toolchain image) and report both versions. The "does not change when
its width is halved" test is then evidence only on 6.8.

### B4. Least widths ignore containers, and the conformance cases are built on containers

The design gives a container cell least = natural (design :140). The conformance cases use `ui::column({text})`
cells (design :300-301). This breaks them in three ways:
- **`wrappingColumnGivesFirst` cannot pass.** Column 1 then has l = n, so step 2 never runs. Step 3 takes the deficit
  from the last column, column 2, and its text elides: the opposite of the case's assertion. Worked on the terminal:
  L = 39 and Σl = Σn = 64, so column 2 drops to its header.
- **`wrap: false` never reaches a text inside a container,** on either frontend. Qt sets `singleLine` only on the
  cell itself (`MorphTableRow.qml:102`), and the terminal calls `setElided` only on a direct `TextImpl` or
  `BadgeImpl` (`table_widget.cpp:437-442`). The "text is not elided" assertions would measure a wrapped text, not
  elision.
- **Real screens have container cells of wrapping text** that would never take part in step 2: audit's "what" column
  (`audit.cpp:350-352`) and results' `analyteCell`.

The framework already has the least width: `MorphWidget.shrinkWidth`, "the narrowest the widget can be laid out
without losing content" (`MorphWidget.qml:46-49`). `MorphText` binds it (`MorphText.qml:77`) and `MorphStack`
aggregates it (`MorphStack.qml:62`).

**Fix.**
- **Qt:** least = `shrinkWidth >= 0 ? shrinkWidth : natural`, decided in `MorphTable` from `column.wrap`, so that
  `wrap: false` gives least = natural. Do not read the cell's current `oneLine`, which `place()` sets late (B1).
  Connect the generic `shrinkWidthChanged`, not a per-type `wrapMinimumWidth`.
- **Terminal:** a `virtual int leastWidth()` on `WidgetBase`, defaulting to `requestedSize().width`, overridden by
  `TextImpl` and `StackImpl` (a column takes its widest child's least, a row the sum). Decide on `column.wrap`, not on
  `TextImpl::_elided`, which `arrangeRows` sets only *after* `columnWidths` runs (`table_widget.cpp:439`, `:452`).
- **Conformance:** use direct text cells for the elision assertions, and measure a column by the next cell's `x`.
  Alternatively, keep container cells only for the width assertions, with least read through the stack.

### B5. Review's arithmetic: the Flagged result is deterministic, and the "All 7" view is not covered

**Flagged (the grabbed view).** Change goes to its floor whatever its natural, so the ±10 on Change (design :33,
:446, :553) has no effect. Evaluation's loss is `n(Analyte) + n(Spec.) + n(Eval) + f(Change) − L`, which is
90 + 80 + 204 + 61.7 − 430 ≈ **6 px**. With Analyte's text whole, the advance is a pixel or two more. The tag *will*
elide, and acceptance (design :446) fails by construction. With `percent(27)`, +10 px, it is whole with about 4 px to
spare.

**All 7, one click away** (`review.cpp:509-516`). The worksheet also shows "Assay, prep 1", "Assay, prep 2" and
"Total impurities" (`review.cpp:149-200`). "Assay, prep 1" inks 75 px (measured, results-1215 at y 198–212).
"Total impurities" is about 90 px (inferred: inked to the dialog's edge at x 385, 3 letters left). So n(Analyte) ≈
106 and the tag loses about **22 px** at 1215. The fallback leaves it about 12 px short. Toggling Flagged and All
also moves Analyte by about 16 px and Evaluation by about 16 px.

**At 1000 (Flagged, verified).** L ≈ 276 and Σf ≈ 247, so nothing scrolls:
- Change and Evaluation sit at 61.7, so the tag reads "Out o…", worse than today's "Out of sp…";
- Spec. gives 17 px, so "95.0–1…".

The design's numbers here are right.

**Fix.** Rewrite R1's Expected and Acceptance:
- state the Flagged loss as about 6 px and take `percent(27)` up front;
- add "All 7" at 1215 to the grabs and to acceptance, with its expected loss;
- pick a screen answer for All. Options: drop Change below a width class; carry the change note in the Evaluation
  tag's tooltip or as a second line under Analyte (the mockup cuts Change to "Ch" anyway, `09-Review.png`); or accept
  an elided tag in All and say so.

Do not leave the lane to discover this.

---

## Should-fixes

### S1. The list of existing tests that change is wrong

- `test_qt_quick_table.cpp:175`, `:800`, `:857` and `:884` have one stretch column each, so they are unchanged by
  construction.
- `:199` asserts only the exact sum and no scrolling, which the new rule keeps.
- The `rowScans` tests (`:361-445`) use `{.label = "A"}` columns, whose `view::TableColumn::width` defaults to
  `content()` (`view.hpp:1178`). Their counts do not change, so the Risks line "the counts in existing tests change"
  (design :559-560) is false.
- No conformance case has a stretch column (`backend_conformance.hpp:287-296`, the entry table), and nor does
  `test_tui_table.cpp:737-780`.

The real changes are the gallery's sizing strings (`examples/lims_gallery/tests/test_review.cpp:49-52`, and samples
only if retuned, `test_samples.cpp:43-44`).

**Fix:** correct the list, and say in the commit that none of the named Qt tests changed. An unchanged test that
"needed re-deriving" invites a silent edit.

### S2. Tests that would pass if O1 did nothing

- **The sweep (design :332-337).** Monotonic, exact sum and not interactive all hold under today's rule. Keep it as a
  guard and label it so. Add one discriminating assertion: at a width in step 1, each stretch column is at least its
  natural width; at a width in step 2, the wrapping column is narrower than its natural width and the eliding column
  is not.
- **"A plain text's `naturalAdvance` equals its `implicitWidth`" (design :348).** Trivially true if it is an alias.
- **The gallery sizing-string tests (design :375-376).** Structural only.
- **"Grab stability at 1215" (design :526, R2).** That grab shows step 2, the conditions matrix with a single stretch
  column (`stability.cpp:257-262`). The Test/Method table (`stability.cpp:486-495`) is on step 3, which no grab shows
  and no mockup draws (`$S/lg-cmp/mock/06-Stability.png` is step 2).

**Fix:**
- Add an end-to-end Qt assertion in `examples/lims_gallery/tests/test_lims_gallery_render.cpp`. It already mounts
  each screen offscreen and fails on any Qt warning (`:1-9`), so it also catches `Binding loop` and a QV4 stack error.
  Open review expanded, find the `results` table, and assert Analyte, Spec., Result and Prior lot are not elided and
  `hflick.interactive` is false.
- Grab stability at step 3, or drop it from R2's "judged" list and say why.

### S3. Terminal parity: the column the terminal cuts silently is made the first to give

On the terminal, a `wrap: true` text is cut *without* an ellipsis (`tui/frontend.md:95`). A `wrap: false` text gets
one. Decision 4 gives the wrapping column least = widest word, so in step 2 the terminal narrows the silently cut
column first and protects the one that would at least show "…". That inverts the cost ordering step 2 is justified
by ("costs height only", design :122). On the terminal it costs content, silently. Samples' Product and Tests
(`samples.cpp:507-513`) are the gallery case. That conflicts with the batch rules "the terminal keeps its cells" and
"vocal over silent" (design :10-11).

**Fix (pick one, and state it in `tui/frontend.md`):**
- (a) the terminal's least for a text it cannot wrap is its natural width, an accepted, documented width divergence;
  or
- (b) the terminal word-wraps a text cell in a `wrap: true` column. `rowHeight` then needs a height for a width,
  `table_widget.cpp:47`.

(a) is the small change.

### S4. LIMS priority: a specification limit should not elide before free text

At 1000 the design leaves Spec. as "95.0–1…". A truncated limit can be misread as a different range, which is worse
than a truncated note or name; Analyte and Change are text and have tooltips. Order-based priority makes Spec. give
before Analyte only because it is further right.

**Fix (screen, R1):** make Spec. `content()`. Checked at P1's numbers:
- **1000:** L' = 276 − 82 = 194 against Σf = 185.1, so nothing scrolls, with about 9 px to spare. Change and
  Evaluation go to their floors and Analyte gives about 19 px.
- **1215:** the tag's loss is unchanged (about 8 px). Spec. is whole at every width where the table fits.

Record the residual risk: about 9 px of margin at 1000.

### S5. Stretch widths now move with the shown rows

Samples pages by 25 and filters as the user types (`samples.cpp:540-541`, the search box). Review toggles All and
Flagged (B5). Today's stretch columns are the stable ones; under O1, Product and Tests shift with every keystroke and
page. CSS tables behave the same, and content columns already do. Say it in Risks, and have O-S grab samples once with
a filter applied.

### S6. Default over opt-in: agree, with three conditions

**Why the default is right:**
- the weight-only rule has no use in the repository;
- a one-stretch table is unchanged;
- morph is 0.1.0 (`include/morph/version.hpp:33`).

**The conditions:**
1. **Versioning.** Under `docs/spec/VERSIONING.md:80-90` this changes behaviour documented at
   `backend_contract.md:269` ("a stretch column takes what the others leave"). 0.x allows it, but record it in the
   spec's Status.
2. **The lost capability.** "A document that wants columns of a set proportion … says `fixed`" (design :110) is
   wrong. `fixed` is absolute units, and a literal `percent` table column is refused (`document.hpp:4102-4108`), so
   equal, width-scaling columns can no longer be expressed. LIMS comparison tables (lot A, lot B, lot C) want them.
   State that this is deliberately dropped, or keep `percent` refused and say `fixed` is the substitute.
3. **The overloaded `shrink`.** It now means "may go under the header" *and* "gives first". A column that may go under
   its header but should keep its content late has no spelling. Note it in `document.md`.

### S7. Conformance mechanics the design omits

- **Registration.** The new cases belong in `layoutConformanceCases()` (`backend_conformance.hpp:3415`), not
  `conformanceCases()`.
- **The third runner.** `tests/test_ui_conformance.cpp:386-395` requires every layout case to fail on the recording
  probe with "reads no bounds". Each case must read geometry before any other check.
- **Units.** `stretchDeficitFromShrinkLast` on Qt adds about 2.1 units of padding per column (16 px / 7.7). With
  20-character cells in `fixed(55)`, the deficit is about 11 units, outside the 3–10 window (design :314). Compute it
  as Σ(20u + 2.1u) − 55u, not "about 6 units", before the lane starts.

### S8. Virtualized monotonic caches: "do not jitter" is overstated

One stretch column's monotonic growth narrows its siblings (step 1's spare falls, step 3's deficit grows) as rows
scroll in. Today's content columns do the same, so it is acceptable, but the design should say it moves other columns
once per new maximum. The least cache must also be monotonic under the same rule (`MorphTable.qml:206`).

### S9. The sweep's monotonic check needs a tolerance

The last column takes `L − others`, so a column can drop by about 1e-13 between widths. Assert `w[i] ≥ prev[i] − 1e-6`.
The exact-sum check stays exact, as today (`test_qt_quick_table.cpp:221`).

### S10. O2: the panel draws a square surface fill under a rounded frame, a library defect

**Measured** on esignature-1215: pixel (379,238) is (246,247,249), the box's fill, *outside* the rounded border; the
arc runs through (382..385,238). The frame is a `Rectangle` with `radius: Theme.radiusCard` and a transparent fill
(`MorphPanel.qml:50-55`). The fill comes from the widget's surface rectangle, whose radius is 0 for any surface but
`card` (`MorphWidget.qml:81`, `:470-477`). The mockup's box has `border-radius: 6px`
(`examples/lims_gallery/design/mockups/Main.dc.html:53`, `:85`).
- **O2 is still right.** `recordWell` would draw the same corners as `page`.
- **But its acceptance passes with the corners wrong.**

**Fix:**
- add "the corner pixel outside the arc is the dialog's white" to O2's acceptance;
- file the defect against `src/qt_quick`: a panel naming a theme-drawn surface clips its fill to the frame's radius.
  It is inside the bar (AGENTS.md table).

Also note O2's terminal effect: none, since the terminal reads no theme.

### S11. O3: the screen fix does not fix the terminal, and "Qt says nothing" is in the contract

- Console's `fixed(10)` still cuts "[Out of spec]" on the terminal: 13 cells in 10 (`display_widgets.cpp:261-263`,
  `console.cpp:681`). The filed issue should name the gallery's console as a reproducer and record that the canvas
  clipping is read, not run.
- Qt raising a fixed child to its content minimum is *documented* (`backend_contract.md:278`). Calling it silent
  under "vocal over silent" (design :430) contradicts the contract the issue asks to pick from. List it as option 3 of
  the issue's "what would close it", not as a defect.

### S12. Smaller accuracy fixes in the design

- The doc-comment lines are `view.hpp:1178` (width) and `:1196-1200` (shrink), and `author.hpp:4458` and
  `:4478-4480`. Design :268-269 cites `:1201` and `:4480`.
- P5's second follower ("a `MorphText` with runs that wraps") applies only to *runs*. A plain wrapping text already
  reads advances (`MorphText.qml:52-56`). Say so, so that `naturalAdvance` is scoped to runs.
- `naturalAdvance` for runs splits at line feeds *inside* runs. Spell out that a run's text can contain `\n`
  (`plainOf`, `MorphText.qml:87-89`), and keep `wrapSlack`.

---

## Answers to the focus questions

| Question | Answer |
|---|---|
| Binding loops on Qt | No QML binding loop: the caches are written outside `widths`. The real loop is B1's synchronous re-entry plus B2's echo. Fix both before implementing |
| Lazy and virtualized rows | Sound in principle. Needs S8's note and a monotonic least cache. The virtualized test discriminates (it fails if stretch ignores content) |
| Exact-sum rule | Correct for steps 1–3 on both frontends. I checked the terminal's round-down plus first-remainder for steps 1 and 2: no column exceeds n. The four planColumns examples are right: [13,17], [18,12], [12,12,6], [8,12], control [8,22]. Needs S9's tolerance |
| Tests and screens that change | S1 (the Qt list is wrong). Screens: review (R1), samples (1215 Product 289 / Tests 170 against today's 275 / 184; 1000 about 144 / 100, as today), stability's step-3 table (not grabbed, S2) |
| Mockup at 1215 and whole at 1000 | Samples is mixed but not worse. Review Flagged: the tag elides about 6 px, so take the fallback now. Review All: about 22 px, and the fallback is not enough (B5). 1000: nothing scrolls, but Eval reads "Out o…" and Spec. "95.0–1…" (S4) |
| Terminal parity | Widths match, content does not: S3 |
| Default or opt-in | Default, with S6's conditions |
| Would every test fail if O1 did nothing | No: S2 (sweep, `naturalAdvance` equality, gallery strings, stability grab), and B2 (the rounding control) |
| Conformance case needed | Yes: the rule lives in `backend_contract.md`. Use B4's cell structure and S7's registration |
| O2 | Correct and minimal. It adds S10's corner check and files S10's defect |
| TUI badge claim | Holds as read: the contract contradicts itself (`:263` against `:278`). File it, with S11's corrections |
