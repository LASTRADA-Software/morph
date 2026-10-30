# morph · feat/lims-gallery — status

The run's tracker, committed so another agent can pick it up; `next.md` says how to update and publish it. Times are local, from `date`, to the second.

**Last updated:** 2026-10-29 01:24:28

## Current plan: LIMS gallery and the declarative-UI batches it found

Ten real LIMS screens (`examples/lims_gallery`, no backend, green theme) test what ui/1 can express, and their gap log
becomes five framework batches (A–E), each designed, reviewed once and implemented on this one branch. It all lands
as one PR, #1045, out of draft since every batch is in.

**Decisions:**
- One branch and one PR for the whole initiative; no separate PRs.
- The gallery has no backend: sample data lives in screen state.
- Green (`#17784A`) is the main colour.
- Framework changes land in batches, not one gap at a time.
- Each design gets one review by a separate agent on the default model, never Fable.
- Implementation agents run on Sonnet 5.5; designs and reviews stay on the default model.
- Run 2–3 agents at a time, each in its own worktree and build tree, with builds taking turns through one lock; branches are rebased onto `feat/lims-gallery` and folded in.
- Batch B owns the `mono` text role and the `font.mono` token; batch C cites them.
- The goal is that ui/1 can produce each mockup's look itself: a visual difference is fixed in the vocabulary or the frontend, and in the screen only when the vocabulary already expresses the design.

| Lane | Tasks done | State | Where it is |
|---|---|---|---|
| Batch H: the gaps left after G | 6/7 | running | Your call: the gaps the last comparison found get built now, not left for later. A design agent (default model) is writing batch H from `c4df1b3e9`: pinned bars that span the window on compact (H1), a square glyph button for pagers (H2), a control inside a selectable entry so samples' cards keep their checkbox and the accent bar (H3), runs in a head's title (H4), an eyebrow line above a head's title and small-capital section labels (H5), plus any other framework gap in the reviews. Then a review round and two Sonnet 5.5 lanes. PR #1045 stays open until H lands. **Design written** (`cee2b3e32`, `...batch-h-design.md`): a screen can drop the shell's page inset, `pinned` insets its body and spans its bars (H1); a `square` shape with the glyph at heading size (H2); `selected` on a column or row draws the accent bar, the checkbox keeps focus (H3); `titleSpans` (H4); `eyebrow` and an `overline` text role (H5); plus two it found: the compact head wraps Scan too early (H6) and an all-hidden pinned bar still draws a strip (H7). Two lanes: H-C (stacks and controls, audit, console) then H-S (shell and head, the other seven screens). Your decisions: `pinned`'s inset on by default; the square head menus and small-capital navigator headings switched by a theme token (off by default, on in the gallery); the checkbox alone selects; heading-size glyphs. Reviewed: 2 blockers (`padding: [2, 4]` is refused by the expression parser, it must be a `lit` list; the stack widgets that need the new calls are four, not three) and 8 should-fixes (a doubled inset, review's queue pane, the workspace tab strip, the pager glyph at body size strong as the mockup has it, the selected card's screen-reader role, the title runs not following a change, the small-capital font lost under tabular figures, an untested eyebrow mount); plus one more gap: instrument's tabs inside the white head block. **Revised** (`bed3d7989`): every finding taken; `pinned` marks its page `pageEdge` and the shell drops its own inset there, so no screen needs a key; two theme switches (`shape.iconOnly`, `font.overline.case`), off by default and on in the gallery; H8: `tabs` gains `inset` and `stripSurface` for instrument's head-block tabs. Defaults taken on the three small choices left (pinned pages keep the inset while the workspace shows its tab strip; a `keyword` token type; instrument's 1 px head rule stays). **Implementing:** H-C (`lg-hc`, e2) and H-S (`lg-hs`, e3), Sonnet 5.5, in parallel. **H-C done:** 7 commits (theme switches, H1/H7 stack half, H3, H2, overline, H8 tabs inset and strip surface, the gallery theme with square audit/console pagers); no warnings, Qt Quick/terminal/client/gallery tests pass, qmllint clean, mutation controls on every item. **H-S done** apart from instrument's tabs, which wait on H8: H6, H4, H5 eyebrow, H1 page half, navigator overlines, and the plate, instrument/results/stability, review and samples screens; 5149/5149 tests, no warnings, qmllint clean, mutation controls on H6/H4/H5/H1 and the screens. It also fixed the terminal's line layout, which let a stretch item's minimum squeeze Scan out at 40 columns **Folded and verified** (`lg-fold-h`, e1): no warnings, 5154/5154 tests, GCC `-Wshadow` clean over every ui header, Qt 6.8.1 qmllint clean and its 253 Qt Quick tests pass (offscreen, as CI runs them). All 20 grabs re-taken (`lg-cmp/app-hh`). **Lane T** (Sonnet, e2) is clearing clang-tidy-diff. **Comparison after H:** batch H's items are closed wherever they show (H8 only partly on expanded instrument); 3 regressions — the square ☰ grows to 44 px on compact and pushes titles right (wraps instrument's badge and results' title), results' badge left alone under its title, review's queue entries tighter — and 63 remaining differences, none high: 22 medium, 41 low; 16 framework gaps (a status slot at the title line's end, the compact title centred against its action, the tab strip's height, dialog rules edge to edge, the compact sheet head, one pinned child bleeding, a dark shell head, the sidebar at 1000 px, …) and about 40 screen fixes. Per your rule these become batch I. |
| Final comparison with the mockups, and its fixes | 3/3 | done | Two reviewers compared all ten screens after batch G. Close to the mockup: receipt, plate, and samples (desktop), audit and console (compact); stability's stepper and pinned footer, plate's round wells and tags, banners and mono IDs all landed. Regressions: dialog footer buttons moved to the left (results, e-signature), and a table row keeps its wrapped height (review). Medium: collapsed section cards ~58 px with the title near the top and no header inset; compact pinned bars inset 16 px with a faint separator; audit's pager row has no inset; console's Test column wraps and its result inputs misalign; review's right pane has no inset; samples' selected compact cards have no accent bar; stability's fields misalign. Two Sonnet 5.5 lanes from `79baf82f8`: GX1 framework (`lg-gx1`, e2), GX2 screens (`lg-gx2`, e3). Not in scope: the eyebrow line above a head's title, small-caps section labels, and the mono face (JetBrains Mono isn't installed here, so mono renders as Nimbus Mono). **GX2 done** (9 commits, one per screen): console's Test column no longer wraps and its inputs line up; review's right pane inset; stability's fields top-aligned, summary IDs mono, full-width Add condition; samples bolds only "3 selected"; results' compact tags and mono head IDs; plate's grid fills again and the ring isn't clipped; e-signature's password message once; audit's compact record line mono. Not expressible yet: a checkbox inside a selectable card (samples' accent bar), `titleSpans` (plate's mono PL-0193); the Scan icon was a framework drop, passed to GX1. **CI's fifth run** failed 5 jobs on two causes plus a runner shutdown (WASM): GCC 15's `-Wshadow` on a local `struct Text` in `theme.hpp` (4 jobs), and Qt 6.8.1's qmllint on three QML files — including `Accessible.expandable`/`expanded`, which Qt 6.8 lacks, so the section header would not even load there. Fixed on `lg-ci5` (e3): the struct renamed (a GCC check reproduces the error before and not after), the expanded state set only where Qt has it, with a test that pins it on Qt 6.12; verified: full `ctest` 5107/5107, and on a local Qt 6.8.1 build qmllint and 253 Qt Quick tests pass (qmllint fails on the old QML, the control). **GX1 done** (4 commits): dialog actions at the right again, a folded section is its header row with the header inset kept, tabs fit Maintenance before More, a head action draws its icon (compact Scan). Review's tall row was its change column wrapping off-screen, fixed in the screen, as is audit's pager inset. Left for later, as framework additions: full-bleed compact bars, a square pager button, a checkbox inside a selectable card, runs in a head's title |
| Batch I: the gaps left after H | 0/4 | running | Design (default model) from the comparison after H: the 3 regressions, 16 framework gaps and the screen fixes; then a review round, your decisions, and Sonnet lanes. |
| Batch G: component looks and text | 3/3 | done | Three Sonnet 5.5 agents from `7ddb82627`, landing in the order G-C → G-B → G-A. G-C (`lg-gc`, e2): the drawn stepper (G2), `pinned` bars with a surface and separator (G3), collapsible sections as cards (G5), tabs overflowing into More ▾ (G10); no screen code. G-B (`lg-gb`, e3): toned text and runs (G9), banners and the dialog's title and footer rule (G4), cells that keep IDs whole (G7), table marks and select boxes (G6), then the screen fixes for console, instrument, results, review, samples, audit, e-signature. G-A (`lg-ga`, gallery worktree): tags (G1), the plate's round wells (G8), mono and aligned inputs (G11), the select's field and dropdown (G12), then stability, plate, receipt and the theme bundle. Lanes cherry-pick what their screens need from the lane before. Progress: **G-A done**, 18 commits — tags and chips (G1), round, dense and mono buttons and badges with dashed outlines (G8: round wells that keep their colours, dashed empty wells, ringed selection), mono, aligned and prefixed inputs (G11), the select's field and list (G12), and screen fixes for stability's units, receipt's mono IDs and plate's list; every suite passes, mutations caught. Its follow-ups on G-B are done too (receipt's mono ID column `fixed(11)`; console's and results' value inputs end-aligned with the unit inside); then stability on the drawn stepper with its footer in `pinned`'s bottom bar and a full-width Continue on compact, compact sections as cards (opening one closes the other, tested), and plate's compact rows as a divided list of sections. Every suite passes; the plate marker ring may still look flat at its left edge. **G-B done**, 7 commits plus G-C's — toned text and runs (G9), rounded toned banners with a green action, a heading-size dialog title and a footer rule (G4), mono cells that never wrap mid-ID (G7), selected rows with the accent bar and toned rows tinted, themed select boxes (G6), and screen fixes for console, instrument, results, review, samples, audit, e-signature; every suite passes, mutations caught. Then on G-C: console's and instrument's compact sections as collapsible panels (a click test opens and closes Qualification), samples' compact cards as one divided list, the bulk bar white with a hairline. Every suite passes, no warnings. **G-C done** (`0704c4a26`, one commit for G2, G3, G5, G10 after the first agent stalled): numbered stepper circles with full-width connectors, pinned bars with a surface and separator, 44 px strong section headers with chevrons, tabs that don't fit go into More ▾. The new agent fixed an undefined `headerSlot` in the panel (broke qmllint and every render smoke) and a conformance probe; `ctest` 5068/5068. G-B and G-A resumed for the screen work that needed it |
| Batch F regressions found by the re-grab | 2/2 | done | Two reviewers compared all ten screens after batch F with the mockups and the earlier grabs. Batch F's items show fixed everywhere they target (white cards, visible checkboxes, green ghost buttons, the scrim, compact bottom bars at the window bottom, review's panes filling, console's tab bar under its header). Regressions: **high** — review's selected queue entry is blank (the new opaque selection fill is drawn over the entry's content; hover likely the same) and receipt's sample heading breaks one character per line (a wrapping row inside a row); **medium** — the shell bar lost its gutter and padding, heads wrap with room to spare (receipt 3 rows), cell padding narrows fixed columns so "% LC" and "22 min overdue" wrap, console's page doesn't fill. Two Sonnet 5.5 lanes fix them from `a9d8a3120`: FX1 (`lg-fx1`, e2) wrapping, shell bar, head, grab tooltip; FX2 (`lg-fx2`, e3) the selection fill, column padding, console fill. New low items go to batch G and its screen fixes. **FX2 done** (3 commits): an item's content column was never reparented into its button, so the background painted over it — the selected queue entry shows its text again (a pixel test fails on the old code); a column's declared width now adds the cell padding ("22 min overdue" on one line again); console's switch stretches and its page fills the window at 1000×830 and 1000×1200. Every suite and qmllint pass. Stretch columns (review's analyte) still wrap: G7. **FX1 done** (4 commits): a wrapping row now places its cells after their bindings settle, so receipt's heading stays on one line; the head's subtitle and badges share one wrapping line, and the head wraps only when out of room; the top bar's gutter and padding are back and its line is centred; a grab clears the hover first (no tooltip). Folded and pushed (`7ddb82627` + tracker): no warnings, `ctest` 5059/5059, clang-format clean, all 20 grabs clean |
| Batch F: layout, theme and containers | 3/3 | done | Three Sonnet 5.5 agents, each in its own worktree and branch from `81ee4fdfc`. **F-T done** (`lg-ft`, 5 commits): tone parts load and a token with members is refused (#1070); `hover`, `track`, `controlSurface`, `controlBorder`, `divider`, `elevation.popup` in every built-in scheme; text below 4.5:1 (muted 3:1) reported; the small and display type steps; ghost buttons in the accent, the active tab in text; the scrim; table cell padding. Full build with no warnings, every suite passes; six QML mutations each caught by their test. **F-L done** (`lg-fl`, e2, 3 commits): a case's, an overlay's and a node list's stretch reach the slot (#1069), so the compact receipt's Receive bar sits at the window bottom; rows and horizontal forEach can wrap (tag rows in samples, console, receipt); panel, dialog, collapsible and drawer bodies space their children, and a divided column draws rules. Full `ctest` 5033/5033, every new test has a hand-broken control. Not yet: re-grabs of stability, results, review. **F-S done** (`lg-fs`, 8 commits): a panel's content starts under its header (#1072); a splitter fills the page and a stretching child scrolls below its minimum height; menus, selects and the date picker open inside the window, ending at their trigger when they'd overflow, in one shared popup frame (#1071); panels draw as white cards; the head shares the top line with the title, and sign-out moved to the sidebar's foot (☰ on compact); console KPIs in the display size. Every suite and qmllint pass; each item has a control that fails without it |
| Batches F and G: drawing the mockups in ui/1 | 6/6 | done | CI's fourth run passed every job on `8a16448e5`; the merge waits for these. Batch F's design is written (`docs/superpowers/specs/2026-10-29-lims-gallery-batch-f-design.md`, three lanes: layout, theme, shell and containers) and reviewed: 2 blockers (F1's stretch fix stops at a second wrapper, so compact screens would stay cut; the new colour roles would break the dark sidebar) and 10 should-fixes, all taken in the revision, committed as `81ee4fdfc`. Your decisions: sign-out moves to the sidebar's foot (☰ on compact); `color.neutral` is the neutral tone only, with new `hover` and `track` tokens; panels default to gap 2 and the card surface, dialogs to gap 3; contrast below 4.5:1 (muted 3:1) is reported, not refused. Batch G's design is written too (`...batch-g-design.md`: tags, a drawn stepper split into parts for `pinned`, a `bar` surface, banners, section headers, table marks, mono that elides, round and small buttons, toned text and spans, a More ▾ tab menu, mono inputs) and reviewed: 1 blocker (round plate wells would still draw square and lose their colours) and 14 should-fixes, all taken in the revision, committed as `d467798a9`, with G12 rewritten to extend batch F's shared popup frame; its five small choices are taken at its defaults. G12 added from your note: the select's dropdown drawn as the mockups' input and menu. Both reviews feed one revision each, then the implementation runs three Sonnet 5.5 agents at a time |
| Comparing every screen with its mockup | 3/3 | done | All your reports confirmed and traced to code. The union is in `examples/lims_gallery/design/visual-review.md`: batch F (layout, theme and container defects, F1–F14) and batch G (component looks and text, G1–G11), plus nine screen fixes. Four library defects filed: #1069 (a `switch` case drops its stretch, so compact pinned pages are cut off at y≈390), #1070 (the theme reader drops tone backgrounds), #1071 (menus leave the window), #1072 (a stretched panel centres its content). The client takes `--grab-size` for this |
| Fixing PR #1045's third CI run | 3/3 | done | 4 jobs failed on three causes. ASan, TSan and clang-cl: a table's selection list read after its temporary was freed, in this branch's row lowering (Valgrind shows the invalid reads before the fix). MSVC and clang-cl: `DateTime` built from components sums days, hours and minutes in the STL's 32-bit minutes, so 9999-12-31 23:30 read as 1833; it now converts to milliseconds first. TSan: mounting every design at both widths ran past the 120 s timeout (84 s under ASan); now 600 s. All in `81e2ed69b`, with a `DateTime` test for year 9999 and one allowlist hint moved. Build with no warnings; every suite passes, and Valgrind is clean on the table test |
| Fixing PR #1045's second CI run | 4/4 | done | 9 of 14 jobs went green; the other 5 had new causes, fixed by three agents in parallel. Qt 6.8.1 (`b827a67d2`): three QML forms 6.8's qmllint rejects; a tooltip 6.8 closes when a modal opens; a click that put the cursor mid-number under CI's DejaVu font (the test now presses End). Toolchain (`e1615aa7b`): the clang-cl link came from a test's native 128-bit divide, now skipped on MSVC; GCC 15's maybe-uninitialized was a false positive in copying `Prop`, which now builds its alternative in place; 15 allowlist line hints moved. Coverage (`5e311b939`): tests only, `include/morph/ui` branches 90.21% → 92.57% measured locally (clang 21), 10 mutations each caught; it found #1068 (a state declared `any` with an init takes the init's type), kept as a `[!shouldfail]` test. And the e-signature test renamed in ASCII (`5eb8af721`). Build with no warnings; every suite, the ten render smokes, qmllint and Doxygen pass |
| Fixing PR #1045's first CI run | 3/3 | done | 14 jobs failed on six causes, fixed by three agents in parallel, plus a seventh the fixes uncovered. Library (`2237cfb2e`): GCC `-Wshadow` in `interpreter.hpp`, `std::iota` for the WASM libc++, the navigator's `const_cast`, the TUI overrides made protected, NOLINTs with reasons in `kinds.hpp`. Tests (`9c2e77058`): MSVC narrowing, a `ranges::to` pipe libstdc++ 14 rejects, 18 tidy findings. Examples (`6120873ed`): a chart in `examples/gallery`, `layout.hpp` renamed to `screen_layout.hpp`, gallery tidy. Then `5382eee17`: an empty date input threw in its calendar (QML read an empty QVariant as undefined), caught by the gallery's render smoke, with a test that fails without it. Build with no warnings; every suite, the ten render smokes and qmllint pass |
| Gallery: ten LIMS designs | 10/10 | done | 01–10 on the branch, each with compact and expanded layouts |
| Batch A: defects and responsive layout | 15/15 | done | 14 fixes and the gallery cleanup, `b6581747a`..`1e4c9cf34`; closes #1046–#1052, #1054, #1055 |
| Batch B part 1: columns and the selection | 6/6 | done | `27c072205`..`7c7f9d708` on `feat/lims-gallery`, pushed; G2, G47, G29 (closes #1053), G43 (closes #1059), G30 and the gallery cleanup |
| Batch B part 2: the row pipeline | 7/7 | done | `8215ca989`..`cdb41fe9f` on `feat/lims-gallery`, pushed: G41 selected button, G31 sort, G28 paging, G44 `runs`, G46 group headings, the `showHeader` flag fix, and the gallery cleanup; all six suites pass |
| Batch C: text, fields and small controls | 16/16 | done | 13 commits on `feat/lims-gallery-c` (pushed): plain text (#1058), spans, key-value, required mark, help text, dialog description, input group, unit (#1060, #1062), outlined and segmented controls (#1061), multi-line list entries, the date pattern, the focal scanner field, gallery cleanup; all suites and qmllint pass |
| Rebasing D parts 3–4 onto the batch C fold, and the final check | 1/1 | done | `feat/lims-gallery` now holds every batch. The sheet and C's dialog description share one dialog; the app document carries C's date patterns and D4's navigation counts. Three fixes on the way: the node-kinds array size in each D commit that adds a kind, the e-signature's note, and clang-format 22 over 54 files plus four Doxygen gaps from C. Build with no warnings; `morph_tests` 3275 (1 failing as expected), qt 188, tui 367, client 38, gallery 54, render smoke, qmllint and Doxygen pass. 54 conformance and 16 layout cases |
| Folding batch C onto `feat/lims-gallery` | 1/1 | done | `feat/lims-gallery-c` (`85ddd58b8`, pushed) is `feat/lims-gallery` plus C's 13 commits. Build clean; `morph_tests` 3260 (1 failing as expected), qt 186, tui 365, client 34, gallery 53, render smoke and qmllint pass. 54 conformance and 15 layout cases |
| Batch D design | 2/2 | done | written and reviewed, `84d3b53cc` (D4 fixes #1063 and #1064) |
| Batch D part 1: following the data | 8/8 | done | on `feat/lims-gallery`. on `feat/lims-gallery-e1` (pushed) over B part 1 and E1: #1063, G51, G20, G32, G35, G16, G38 and the cleanup; I built it: no warnings, all six suites pass |
| Batch D part 2: layout and controls | 4/4 | done | on `feat/lims-gallery`. `572fcc7ec`..`e96f52973` on `feat/lims-gallery-e1` (pushed): G23 grid column widths, G18 menu drawn as one button, G27 removable badges as filter chips, the gallery cleanup, and a qmllint fix for part 1's tab bar; all suites and qmllint pass |
| Folding E1, D parts 1–2 and the chart onto `feat/lims-gallery` | 1/1 | done | `feat/lims-gallery` now holds batches A, B, E1, E2 and D parts 1–2 (`620ad5378`, pushed). I built it with no warnings; all six suites and qmllint pass. 49 conformance and 15 layout cases |
| Batch D part 3: the head and overlays | 4/4 | done | `17d45396c`..`f1cb577fe` on `feat/lims-gallery-d3` (pushed): G18 the head's own menu, G26 search and select in the head, G4 a dialog as a sheet (layout case 16), the gallery cleanup; all suites and qmllint pass |
| Batch D part 4: navigation | 3/3 | done | `7f84dadca`..`9771a177c` on `feat/lims-gallery-d4` (pushed): G50 navigate as a dialog that answers its opener (closes #1064, reproduced first: the call was never answered and a later mount answered it with its own value), G56 navigation counts and the compact shell, the gallery cleanup; all suites, qmllint and Doxygen pass |
| Batch E design | 2/2 | done | written and reviewed, `36d08c929` |
| Batch E1: expressions | 7/7 | done | on `feat/lims-gallery`. on `feat/lims-gallery-e1` under D part 1; closes #1066 and #1065 |
| Batch E2: the chart | 2/2 | done | on `feat/lims-gallery`. on `feat/lims-gallery-e2` (pushed); it uses E1's checked rounding, so it goes in after `feat/lims-gallery-e1`. A first try on batch B part 2 alone failed to compile; that resolution is kept on local branch `lg-fold-e2-on-b2` (drop its stray `backend_conformance.hpp.orig`). Tidy 22 on `chart_scale.hpp`: 0 warnings |

**Now:** batch H is folded and verified; a design agent (default model) is writing batch I from the comparison after H (3 regressions, 16 framework gaps, ~40 screen fixes); lane T is clearing clang-tidy-diff on the fold.
**Next:** review batch I's design, take your decisions on it, implement it in 2–3 Sonnet lanes, fold with lane T, re-grab and compare; then push, CI to green, and merge PR #1045.

## Time spent

From the time log, stamped as each event happened. *Implement* runs from a task's dispatch to its first commit or report, *review & fixes* from there to done, and *wall time* from the first dispatch to the last completion: less than their sum where work ran in parallel. Work still running counts to 2026-10-30 10:31:08.

| Lane or phase | Finished | Wall time | Implement | Review & fixes | Mean per task | Idle |
|---|---|---|---|---|---|---|
| B1 · Batch B part 1: columns and the selection | 1 | 4h 27m | 4h 27m | 0m 00s | 4h 27m | — |
| B2 · Batch B part 2: the row pipeline | 1 | 16h 49m | 16h 49m | 0m 00s | 16h 49m | 6h 38m |
| C · Batch C: text, fields and small controls | 1 | 71h 41m | 71h 41m | 0m 00s | 71h 41m | 8h 10m |
| D · Batch D: layout, overlays and navigation | 2 | 3h 00m | 35m 00s | 0m 11s | 17m 36s | — |
| E · Batch E: expressions and charts | 2 | 44m 03s | 44m 03s | 0m 00s | 22m 02s | — |
| E1 · Batch E1: expressions | 1 | 2h 58m | 2h 57m | 1m 16s | 2h 58m | — |
| D1 · Batch D part 1: following the data | 1 | 15h 12m | 15h 12m | 0m 00s | 15h 12m | 7h 05m |
| E2 · Batch E2: the chart | 1 | 4h 41m | 4h 41m | 0m 00s | 4h 41m | 35m 09s |
| D2 · Batch D part 2: layout and controls | 1 | 6h 14m | 6h 14m | 0m 00s | 6h 14m | 8m 06s |
| FOLD · Folding the side branches | 3 | 64h 22m | 61h 53m | 5m 46s | 20h 39m | 1h 31m |
| D3 · Batch D part 3: the head and overlays | 1 | 2h 41m | 2h 41m | 0m 00s | 2h 41m | — |
| D4 · Batch D part 4: navigation | 1 | 2h 17m | 2h 17m | 0m 00s | 2h 17m | — |
| CI · Fixing PR #1045's first CI run | 3 | 1h 11m | 3h 05m | — | 1h 01m | — |
| CI2 · Fixing PR #1045's second CI run | 4 | 2h 06m | 4h 38m | — | 1h 09m | — |
| CI3 · Fixing PR #1045's third CI run | 3 | 9m 09s | 27m 09s | — | 9m 03s | — |
| VC · Comparing every screen with its mockup | 3 | 13m 47s | 30m 01s | — | 10m 00s | — |
| FG · Batches F and G: drawing the mockups in ui/1 | 6 | 36m 20s | 1h 11m | — | 11m 55s | — |
| F · Batch F: layout, theme and containers | 6 | 5h 11m | 10h 37m | — | 1h 46m | — |
| G · Batch G: component looks and text | 4 | 4h 14m | 11h 32m | — | 2h 53m | — |
| X · Final comparison and fixes | 3 | 55m 45s | 1h 51m | — | 37m 10s | — |
| H · Batch H: the gaps left after G (running) | 7 | 3h 47m | 6h 35m | — | 52m 13s | — |
| I · Batch I: the gaps left after H (running) | 0 | 0m 00s | 0m 00s | — | — | — |
| **All** (running) | 55 | 118h 19m | 230h 45m | 7m 13s | 4h 11m | 24h 09m |

### B1 · Batch B part 1: columns and the selection

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| B1.1 | Batch B part 1: finish G2, then G47, G29, G43, G30 and the gallery cleanup | 2026-10-25 14:42:13 | 4h 27m | 0m 00s | 4h 27m | — | 0 |

### B2 · Batch B part 2: the row pipeline

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| B2.1 | Batch B part 2: G41, G31, G28, G44, G46, the showHeader flag and the gallery cleanup | 2026-10-25 19:45:03 | 16h 49m | 0m 00s | 16h 49m | 6h 38m | 0 |

### C · Batch C: text, fields and small controls

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| C.1 | Batch C: plain text (#1058) first, then the design's order, and the gallery cleanup | 2026-10-25 23:06:07 | 71h 41m | 0m 00s | 71h 41m | 8h 10m | 0 |

### D · Batch D: layout, overlays and navigation

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D.1 | Write the batch D design (resumed from its research) | 2026-10-25 12:12:03 | 4m 39s | 0m 11s | 4m 50s | — | 0 |
| D.2 | Review the batch D design | 2026-10-25 14:42:13 | 30m 21s | 0m 00s | 30m 21s | — | 1 |

### E · Batch E: expressions and charts

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| E.1 | Write the batch E design | 2026-10-25 14:42:13 | 23m 32s | 0m 00s | 23m 32s | — | 0 |
| E.2 | Review the batch E design | 2026-10-25 15:05:45 | 20m 31s | 0m 00s | 20m 31s | — | 1 |

### E1 · Batch E1: expressions

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| E1.1 | Batch E1: #1066, G36, #1065, half-up, G45, G37 and the gallery cleanup | 2026-10-25 15:26:16 | 2h 57m | 1m 16s | 2h 58m | — | 0 |

### D1 · Batch D part 1: following the data

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D1.1 | Batch D part 1: G51, G20, G32, G35, G16, G38, plus the app document's mount (#1063) | 2026-10-25 18:24:42 | 15h 12m | 0m 00s | 15h 12m | 7h 05m | 1 |

### E2 · Batch E2: the chart

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| E2.1 | Batch E2: G55 chart on Qt Quick and the terminal, and the gallery cleanup | 2026-10-25 18:24:42 | 4h 41m | 0m 00s | 4h 41m | 35m 09s | 0 |

### D2 · Batch D part 2: layout and controls

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D2.1 | Batch D part 2: G23, G18's menu node, G27 and the gallery cleanup | 2026-10-26 09:37:10 | 6h 14m | 0m 00s | 6h 14m | 8m 06s | 0 |

### FOLD · Folding the side branches

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| FOLD.1 | Rebase E1 and D parts 1-2 onto batch B part 2, then the chart | 2026-10-26 15:51:30 | 52h 27m | 5m 46s | 52h 33m | 1h 31m | 1 |
| FOLD.2 | Rebase batch C onto feat/lims-gallery | 2026-10-28 22:47:49 | 8h 51m | 0m 00s | 8h 51m | — | — |
| FOLD.3 | Rebase D parts 3-4 onto the C fold, verify everything once | 2026-10-29 07:39:12 | 34m 52s | 0m 00s | 34m 52s | — | 3 |

### D3 · Batch D part 3: the head and overlays

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D3.1 | Batch D part 3: G18 head menu, G26, G4 and the gallery cleanup | 2026-10-28 20:25:05 | 2h 41m | 0m 00s | 2h 41m | — | 0 |

### D4 · Batch D part 4: navigation

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| D4.1 | Batch D part 4: G50 (#1064), G56 and the gallery cleanup | 2026-10-28 23:07:04 | 2h 17m | 0m 00s | 2h 17m | — | 0 |

### CI · Fixing PR #1045's first CI run

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| CI.1 | Library: GCC shadow, WASM iota, tidy in include and src | 2026-10-29 10:26:55 | 56m 35s | — | 56m 35s | — | — |
| CI.2 | Tests: MSVC narrowing and clang-tidy | 2026-10-29 10:26:55 | 56m 53s | — | 56m 53s | — | — |
| CI.3 | Examples: the gallery's chart, a header name, clang-tidy | 2026-10-29 10:26:55 | 1h 11m | — | 1h 11m | — | — |

### CI2 · Fixing PR #1045's second CI run

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| CI2.1 | Qt 6.8.1: qmllint and two qt_quick tests | 2026-10-29 13:23:02 | 1h 15m | — | 1h 15m | — | — |
| CI2.2 | Toolchain: clang-cl 128-bit division, GCC 15 maybe-uninitialized, coverage line hints | 2026-10-29 13:23:02 | 1h 16m | — | 1h 16m | — | — |
| CI2.3 | ui branch coverage back over its 92% floor | 2026-10-29 13:23:02 | 2h 06m | — | 2h 06m | — | — |
| CI2.4 | A test name ctest on Windows cannot select | 2026-10-29 13:23:02 | 0m 00s | — | 0m 00s | — | — |

### CI3 · Fixing PR #1045's third CI run

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| CI3.1 | A dangling selection list in table rows (ASan, TSan, clang-cl) | 2026-10-29 17:23:26 | 9m 09s | — | 9m 09s | — | — |
| CI3.2 | DateTime wraps past year 6053 on MSVC | 2026-10-29 17:23:35 | 9m 00s | — | 9m 00s | — | — |
| CI3.3 | The gallery mount test's timeout under TSan | 2026-10-29 17:23:35 | 9m 00s | — | 9m 00s | — | — |

### VC · Comparing every screen with its mockup

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| VC.1 | Screens 1–4: e-signature, receipt, instrument, audit | 2026-10-29 19:19:46 | 13m 47s | — | 13m 47s | — | — |
| VC.2 | Screens 5–7: samples, stability, plate | 2026-10-29 19:19:46 | 7m 37s | — | 7m 37s | — | — |
| VC.3 | Screens 8–10: results, review, console | 2026-10-29 19:19:46 | 8m 37s | — | 8m 37s | — | — |

### FG · Batches F and G: drawing the mockups in ui/1

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| FG.1 | Batch F design: layout, theme and containers | 2026-10-29 19:38:06 | 16m 09s | — | 16m 09s | — | — |
| FG.2 | Batch G design: component looks and text | 2026-10-29 19:38:06 | 20m 28s | — | 20m 28s | — | — |
| FG.3 | Batch F design review | 2026-10-29 19:54:15 | 9m 51s | — | 9m 51s | — | — |
| FG.4 | Batch G design review | 2026-10-29 19:58:34 | 9m 05s | — | 9m 05s | — | — |
| FG.5 | Batch F design revision | 2026-10-29 20:04:06 | 9m 10s | — | 9m 10s | — | — |
| FG.6 | Batch G design revision | 2026-10-29 20:07:39 | 6m 47s | — | 6m 47s | — | — |

### F · Batch F: layout, theme and containers

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| F.L | F-L layout: F1, F4, F7 | 2026-10-29 20:13:16 | 3h 01m | — | 3h 01m | — | — |
| F.T | F-T theme: F8–F14, table padding | 2026-10-29 20:13:16 | 1h 42m | — | 1h 42m | — | — |
| F.S | F-S shell and containers: F2, F6, F3, F10, F5 | 2026-10-29 20:13:16 | 4h 04m | — | 4h 04m | — | — |
| F.I | Fold batch F onto the branch, verify, re-grab | 2026-10-30 00:17:38 | 11m 07s | — | 11m 07s | — | — |
| F.X1 | FX1: wrap row in a row, shell bar gutter, head wrapping, grab tooltip | 2026-10-30 00:28:45 | 56m 30s | — | 56m 30s | — | — |
| F.X2 | FX2: selected fill over content, padding narrowing columns, console fill | 2026-10-30 00:28:45 | 40m 53s | — | 40m 53s | — | — |

### G · Batch G: component looks and text

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| G.C | G-C structure: G2, G3, G5, G10 | 2026-10-30 01:25:16 | 3h 19m | — | 3h 19m | — | — |
| G.B | G-B text and tables: G9, G4, G7, G6, screen fixes | 2026-10-30 01:25:16 | 3h 59m | — | 3h 59m | — | — |
| G.A | G-A controls: G1, G8, G11, G12, screen fixes | 2026-10-30 01:25:16 | 3h 59m | — | 3h 59m | — | — |
| G.I | Fold batch G, verify, re-grab | 2026-10-30 05:25:02 | 14m 17s | — | 14m 17s | — | — |

### X · Final comparison and fixes

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| X.1 | GX1 framework: dialog footer, row height, section cards, bar bleed, pager, tab overflow | 2026-10-30 05:43:13 | 55m 45s | — | 55m 45s | — | — |
| X.2 | GX2 screens: console, review, stability, samples, results, receipt, plate, e-signature, audit | 2026-10-30 05:43:13 | 32m 02s | — | 32m 02s | — | — |
| X.3 | CI fifth run: GCC shadow, Qt 6.8 qmllint and Accessible | 2026-10-30 06:15:16 | 23m 42s | — | 23m 42s | — | — |

### H · Batch H: the gaps left after G

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| H.D | Batch H design | 2026-10-30 06:43:15 | 22m 26s | — | 22m 26s | — | — |
| H.R | Batch H design review | 2026-10-30 07:05:41 | 4m 23s | — | 4m 23s | — | — |
| H.V | Batch H design revision | 2026-10-30 07:10:04 | 7m 10s | — | 7m 10s | — | — |
| H.C | H-C controls, stacks and theme | 2026-10-30 07:17:14 | 2h 27m | — | 2h 27m | — | — |
| H.S | H-S shell and head | 2026-10-30 07:17:14 | 2h 17m | — | 2h 17m | — | — |
| H.S2 | H-S: instrument's tabs on H8 | 2026-10-30 09:44:37 | 16m 18s | — | 16m 18s | — | — |
| H.F | Fold and verify batch H | 2026-10-30 10:00:55 | 30m 12s | — | 30m 12s | — | — |
| H.T | Lane T: clang-tidy-diff to zero (running) | 2026-10-30 10:00:55 | 30m 13s | — | 30m 13s | — | — |

### I · Batch I: the gaps left after H

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| I.D | Batch I design (running) | 2026-10-30 10:31:08 | 0m 00s | — | 0m 00s | — | — |

### Idle stalls

| Where | From | To | Idle | What stalled |
|---|---|---|---|---|
| Task D1.1 | 2026-10-25 19:10:01 | 2026-10-25 19:45:10 | 35m 09s | session limit; resumed at the reset |
| Task E2.1 | 2026-10-25 19:10:01 | 2026-10-25 19:45:10 | 35m 09s | session limit; resumed at the reset |
| Task B2.1 | 2026-10-26 00:40:00 | 2026-10-26 07:10:50 | 6h 30m | session limit, then waiting overnight to be resumed (counted from the 00:40 reset; the stop time before it is not recorded) |
| Task C.1 | 2026-10-26 00:40:00 | 2026-10-26 07:10:50 | 6h 30m | session limit, then waiting overnight to be resumed (counted from the 00:40 reset; the stop time before it is not recorded) |
| Task D1.1 | 2026-10-26 00:40:00 | 2026-10-26 07:10:50 | 6h 30m | session limit, then waiting overnight to be resumed (counted from the 00:40 reset; the stop time before it is not recorded) |
| Task B2.1 | 2026-10-26 12:10:00 | 2026-10-26 12:18:06 | 8m 06s | session limit; resumed after the 12:10 reset (the stop time before it is not recorded) |
| Task C.1 | 2026-10-26 12:10:00 | 2026-10-26 12:18:06 | 8m 06s | session limit; resumed after the 12:10 reset (the stop time before it is not recorded) |
| Task D2.1 | 2026-10-26 12:10:00 | 2026-10-26 12:18:06 | 8m 06s | session limit; resumed after the 12:10 reset (the stop time before it is not recorded) |
| Task C.1 | 2026-10-28 18:00:00 | 2026-10-28 19:31:08 | 1h 31m | weekly usage limit; resumed after the Oct 28 18:00 reset (stopped on Oct 26 around 16:00; the exact stop time is not recorded) |
| Task FOLD.1 | 2026-10-28 18:00:00 | 2026-10-28 19:31:08 | 1h 31m | weekly usage limit; resumed after the Oct 28 18:00 reset (stopped on Oct 26 around 16:00; the exact stop time is not recorded) |

## Key rulings

- A part's new conformance and layout cases are appended to the lists and described in `backend_contract.md`'s numbered lists, so the design's case numbers do not carry over.
- Batch E: reference lines replace the chart's axis ticks only when there are 3 or more.
- Batch E: chart series colours `#0072B2 #C25400 #00805E #A8508A`, each with its own marker shape.
- Batch E: SD is the sample SD (n−1).
- Batch E2's chart takes layout case L16, since batch D part 1 takes L15.
- Independent work runs in a second worktree and is rebased onto `feat/lims-gallery`; one PR still.
- The tracker and its time log are committed in `examples/lims_gallery/design/status/`, so another agent can continue; `next.md` says how.

- Batch D: the workspace scrolls a screen whose root doesn't stretch, so no screen is cut off.
- Batch D: `as: window` is reported once at info.
- Batch D: the shell switches to its compact form below `expanded`.
- Batch D: a navigation that mounts nothing cancels its call, so the caller is answered exactly once.

- Enter in a dialog: a text input with its own `onSubmit` takes Enter; one without lets it reach the dialog's
  default; a checkbox never takes Enter.
- Width classes are in units: compact below 80, medium below 160; an 80-column terminal is medium.
- Touch-sized controls follow the width class, not touch detection.
- A plain click, Shift-click and Shift+arrow replace the whole table selection, other pages included; Ctrl-click,
  Space and Ctrl+A extend it.
- A table's `sort` is a list of `{column, direction}`; `[]` is the source order.
- Strings sort case-folded, ties by bytes, matching the model-backed tables.
- `sort`, `slice` and `runs` join `expr/1` in place.
- Spans may carry plain `text` beside them as an older client's fallback.
- One application date pattern applies to every date style; a `+02:00` offset token, no zone names.

## Follow-ups

- From batch C: a check box toggle and a select choice now mark the field touched, so a required group's message shows under the default policy (new behaviour, in `interpreter.md`); the Doxygen build was not run on C's new public symbols.
- From D part 2: the plate's well grid uses stretch tracks, and on Qt Quick at 1280 × 900 "STD1" is elided to "S…"; the old equal-column grid let a column take its widest cell's width, which tracked grids do not. Whether stretch tracks should is a framework decision (a visible regression in the gallery).
- From D part 1: stability's Continue is not pinned on compact, because `stepper` has no option to pin its footer; the rule refusing a focus into two switches cannot tell that two cases under one width switch are never mounted together.
- E2's `chart_scale.hpp`: the editor's clang-tidy flagged `chartTicks` at cognitive complexity 31 and an identical-branch conditional; the lane reported tidy clean, so re-run tidy on it when folding.
- From E1: the parser accepts a JSON key `""` beside `fn` and `get` (read from the code, not reproduced); `expressions.md` still says a new form needs a new expression version, against the in-place `expr/1` decision; `plate.cpp` and `review.cpp` keep the map-then-first workaround.

- Open defects found by the gallery, to be fixed in its batches: #1058 (Qt Quick draws data text as markup), #1059
  (Qt Quick tables unreachable from the keyboard), #1060 (multiline input with an icon loads, then mount throws),
  #1061 (unknown select style loads silently), #1062 (dialog title and field unit hidden from screen readers).
- Issue #1063: the generic client never mounts the app document, so `app.*` reads nothing; batch D's navigator counts
  depend on it.
- Issue #1064 (weak evidence): `navigate` with `onResult` to an already open screen may never be answered.
- Issue #1065: `mean` and `decimal` return a saturated value on overflow instead of reporting it (reproduced).
- Issue #1066: a numeric path segment past 2^64 wraps and reads the wrong element (reproduced).
- Table column labels are evaluated once, as tab labels were: batch B part 2 checks it.
- A document table's `"showHeader": false` is ignored (the loader stores a flag, the interpreter reads a prop): read from the code; fixed in batch B part 2, not filed, since the fix is one line.
- Issue #1067 (weak evidence): default error and info tones fall below 3:1 on the dark sidebar surface.
- Issue #1056: env `locale`, `platform` and `online` are never set.
- The gallery's render smoke does not fail on a property write Qt refuses; fix it inline in the example.
