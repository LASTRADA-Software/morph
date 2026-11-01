# Review of batch N (`lg-n-design.md`)

Checked against `/home/yaraslau/repo/morph-lims-e1` at `a30f51c93` (`lg-fold-m`), by reading the code and measuring the
`lg-ms-*.png` grabs with PIL. Nothing was built or run. "Design:NN" means a line of `lg-n-design.md`. Every other path
is relative to the worktree.

Verdict: **4 blockers and 14 should-fixes.** N1's arithmetic holds. Its holder rule, its Qt loop guard and its tests do
not. N2, N4 and N5 are sound apart from the should-fixes below. N3 has a test that cannot tell the feature from its
absence. The lane split is clean.

---

## Blockers

### B1. The holder rule for a switch case makes the mount throw and applies the share twice

Design:155-166 and design:243-244 move a single case node's percent onto `lazy`'s one-row `forEach` and lower the node
as `stretch(1)`. The code does not have the shape that rule assumes:

- **The case's `forEach` is not a stack's child.** It is mounted under the switch's **`SlotWidget`**:
  - `mountKind(Switch)` creates a slot (`include/morph/ui/mount.hpp:1056-1057`);
  - `mountContent(…, widget)` mounts the case into that slot (`mount.hpp:1044-1051`, `1066`).
  - Design:146-147 has the mount throw for a percent node whose parent is not a stack or a splitter. So every
    percent switch case throws at mount.
  - The interpreter-only test at design:243 never mounts, so it would pass while every real use throws.
- **There are three holders, not two.**
  - `lowerSwitch` passes `caseHints` (`holderHints`) to `lazy` (`include/morph/ui/interpreter.hpp:8811-8831`).
  - The case body is then built by `holding()` → `listColumn()` (`interpreter.hpp:6893-6899`, `6859-6870`), which runs
    `holderSizing` again.
  - Under the design's rule, both the `forEach` and the `holding` column take `percent(30)`, so the node gets 30 % of
    30 %.
- **The share is not visible to the stack.** The switch's slot is the stack's actual child. Its sizing is the switch
  node's own, from `commonOf(node, …)` at `interpreter.hpp:8851`, not the case's. A share passed down to the `forEach`
  never reaches the row.
- The same holds for a dialog's content (`lazy` at `interpreter.hpp:8714`), whose parent is the dialog, which is
  refused anyway.

**Fix.**

- Apply the holder rule only to a `listColumn` whose parent is a stack (inline node lists, `forEach` bodies).
- For a switch case and for dialog content, report a percent node once ("size the `switch` itself; a case fills it")
  and lower it as `stretch(1)`. The author puts `percent` on the `switch` node, whose slot is the row's child.
- Replace the test at design:243 with:
  - a **mounted** test: a row holding a `switch` sized `percent(30)`, whose case has one node. Assert the slot's width,
    on the recording backend and on Qt.
  - a test that a percent case node is reported and does not throw.

### B2. The Qt loop guard, as written, still loops

Design:189-193 and design:566-569 say that `MorphStack` counts a percent child at its natural length "in the same way
its `between` path already overrides `layout.implicitWidth`". That path is not free of the share:

- **The `between` path reads the share.**
  - `MorphStack.shownLengths` reads `child.Layout.preferredWidth` (`src/qt_quick/qml/MorphStack.qml:136`).
  - `naturalLength` (`:145`) is built from it, and that is the `between` path's `implicitWidth` (`:42`).
  - The share is `amount/100 × the stack's width`, so a content-sized stack under `justify: between` binds its
    implicit width to its own width. Copying that path copies the loop.
- **The `GridLayout` path cannot be told to ignore the share.**
  - `implicitWidth`/`implicitHeight` is `layout.implicitWidth`/`implicitHeight` (`MorphStack.qml:43`, `:46`).
  - Qt computes those from every child's `Layout.preferredWidth` (`MorphWidget.qml:245`).
  - So a stack with any shown percent child must compute its own implicit size by hand, on **both** axes: a percent
    width in a column is the column's cross axis (design:142-143).
- **The child must not read `parent`.**
  - A child's `parent` is the `GridLayout` (`MorphStack.qml:389`).
  - In a packed row, that layout's `width` is its own `implicitWidth` (`:396`), which includes the share. Reading it is
    a direct loop.
  - "The parent stack's width inside its padding" (design:189) must be spelled out as the `MorphStack` two levels up,
    minus `2 × insetX`.

**Fix.** Rewrite the Qt bullets to say:

- `MorphWidget`: for kind 3, the preferred width is `(parent?.parent as MorphStack)` width − 2·`insetX`, or −1 when
  there is no such stack. Height is the same with `insetY`.
- `MorphStack`: while any shown child has kind 3, `implicitWidth`/`implicitHeight` are computed by hand as the sum
  along the axis, and the maximum across it, of `naturalWidth(cell)`/`naturalHeight(cell)` (`MorphStack.qml:189-203`,
  where a percent counts as `implicit`), plus gaps and insets. `shownLengths` gives the natural length for a percent
  child when it feeds `naturalLength`, and the share when it feeds `betweenSpacing`.
- Tests: extend the loop test (design:232-233) to three cases:
  - a content-sized row under `justify: between`;
  - a content-sized column with a `percent(50)` width child (the cross axis);
  - a row with a `percent` *height* child, or refuse heights (see S4).

### B3. N1 changes the backend contract, but no conformance case drives both frontends

- N1 changes the contract in two ways:
  - a new `Sizing::Kind` that every backend must honour;
  - the splitter's placement without `sizes`, which changes `backend_contract.md:148-150`, where equal shares are the
    contract today.
- Its tests are per frontend only (design:216-249).
- `include/morph/ui/testing/backend_conformance.hpp` already has geometry cases of exactly this shape
  (`maximumCapsStretchShare`, `:1828-1843`, a `fixed(60)` row measured on both backends). It has **no splitter case at
  all**.
- The risks promise a conformance case for `setAlign` and `setHeadBar` (design:574-576), but N3's and N5's Tests
  sections (design:339-351, design:425-439) do not list one.

**Fix.** Add these conformance cases:

- `percentSharesItsRow`: a `fixed(60)` row of `percent(25)` and `stretch()` gives 15 and 45;
- `percentAcrossColumn`: a `fixed(40)` column with a `percent(50)` child gives 20;
- `splitterPlacesPanesBySizing`: a `fixed(41)` splitter of `percent(50)` and `content` gives 20 and 20. Without
  sizing, both panes `content` give an equal share. With matching `sizes`, the sizes win;
- the `setAlign` and `setHeadBar` cases the risks already promise, listed in N3's and N5's Tests.

Register them in `tests/tui/test_tui_conformance.cpp` and in the Qt conformance runner, so that both frontends run
them.

### B4. N3's Qt test measures a quantity that alignment does not move

- **The measure is fixed.** Design:344-348 asserts that the last glyph ends within 1 px of the right edge, measured as
  `label.x + label.contentWidth`. A `Text`'s `contentWidth` is the extent of its text whatever its
  `horizontalAlignment`. The label is `controlWidth` wide (`MorphText.qml:150-154`), so `label.x + contentWidth` is
  about 200 px both with and without `setAlign`.
- **So neither check works.**
  - The positive check fails *with* the feature.
  - The control ("ink ends near x = contentWidth") holds both ways.
  - `label.lastLineEnd` is no better: it is `line.x + line.implicitWidth` from `lineLaidOut` (`MorphText.qml:170-174`),
    taken before alignment is applied.

**Fix.**

- Measure ink:
  - `grabToImage` the text;
  - assert that the rightmost non-background column is within 2 px of `label.width`, and that the leftmost is more
    than `label.width − contentWidth − 2`;
  - under `LayoutMirroring`, assert the mirror image.
- Keep the `horizontalAlignment == AlignRight` check as a second, cheaper assertion.
- The control (no `setAlign`) must fail the ink check.

---

## Should-fixes

### S1. N1: "its content does not raise it" is not what Qt does, and the 28 % can be squeezed

- **Qt raises a container child to its content.**
  - `Layout.minimumWidth` falls back to `contentMinimumWidth` (`MorphWidget.qml:247-248`).
  - That property is set for every stack (`MorphStack.qml:81`) and every panel (`MorphPanel.qml:41`).
  - So on Qt a percent stack or panel is raised to its content minimum, as a fixed one is today.
- **The terminal does not.** `solvedAreas` builds its items from the hints' bounds alone (`src/tui/widget.cpp:701-708`).
- **So design:131-132 states a rule that one frontend breaks.** `backend_contract.md:264-266` says the Qt behaviour is
  the contract ("a stack or a panel is never laid out narrower than … its children's minimums").
- **The `GridLayout` can also shrink the share.** When the stretch sibling's preferred width (its `implicitWidth`) is
  more than the remainder, the `GridLayout` can shrink the percent child below its share. M-S's 39-unit receipt grab
  shows a fixed child that held (the pane is 684–984 = 300 px in `lg-ms-receipt-1000x830.png`), so this is **inferred
  and weak**, but no test covers it.

**Fix.**

- Restate design:131-132 as: "as a fixed child: on Qt Quick a container's content minimum still raises it, and a
  table, a scroll or a leaf takes its own overflow".
- Or deliberately set `Layout.minimumWidth` to the share when the node has no `minWidth`, and say so in
  `backend_contract.md`.
- Add a Qt test: a 1000 px row of `percent(28)` and a `stretch` child whose `implicitWidth` is 900. The first child is
  280 ± 0.5.

### S2. N1: a share grows without bound on a wide window, and a LIMS rail should not

- **Percent fits the judged widths.** Design:100-108 and the arithmetic check out (see S12).
- **But it scales up as well as down.** In a 1920 px window the page is about 1705 px:
  - review's queue becomes 477 px;
  - audit's rail becomes 375 px.
- **A worklist queue or a facet rail is chrome.** On a wide monitor the reviewer wants the extra room in the worksheet
  and the table, not in the rail.
- **What the mockups mean is a clamp.** "280 px beside a `flex: 1` sibling" is a fixed preferred width that gives way
  on a narrow page. That is CSS's `clamp(min, n%, max)`, and percent with `minWidth`/`maxWidth` expresses it, since
  design:128-129 clamps the share into the bounds.

**Fix.**

- Keep `percent`, and say in `document.md` that a pane is usually `percent(n)` with a `maxWidth`.
- Have S2 and S5 set a `maxWidth` of the mockup width in units (review 37, receipt 39, audit 29, stability 33).
- Add a 1920 px grab to N-S's acceptance.

This needs S3.

### S3. N1: a splitter pane's bounds are not honoured on either frontend

Design:128-129 says every sizing is clamped into its bounds. A splitter's panes are not:

- On Qt, `MorphSplitter.apply()` (`src/qt_quick/qml/MorphSplitter.qml:20-36`) never reads `minWidthUnits` or
  `maxWidthUnits`.
- On the terminal, `SplitterImpl::layoutExtents` (`src/tui/region_widgets.cpp:149-171`) ignores the hints.

**Fix.**

- Qt Quick: bind each pane's `SplitView.minimumWidth`/`maximumWidth` (or height) from its bounds. This also bounds the
  user's drag, which is what a LIMS user expects from a rail.
- Terminal: pass the bounds into the `layout::Item`s that design:203-205 already builds, and clamp a drag
  (`region_widgets.cpp:229-249`) into them.
- Test both frontends.

### S4. N1: limit percent to width

- **Every placement the batch needs is a width.**
  - review's queue: a horizontal splitter;
  - receipt, audit and stability: along a row.
- **A percent height is risky and has no use here.**
  - It adds a second set of loop paths: `implicitHeight` (`MorphStack.qml:44-46`) and the `heightKind === 2` sites in
    `MorphPanel.qml:22`, `MorphSlot.qml:50,60`, `MorphRoots.qml:24` and `MorphCollapsible.qml:21`.
  - A share of a content-sized page's height means little.

**Fix.** Refuse `percent` in a height at load (literal) and at mount, as a grid track is refused (design:151-152):
"percent sizes a width; nothing needs a height share yet". This removes the height half of B2.

### S5. N1: the grep in the risks misses every QML site

- **The risks only look for `Sizing::Kind`.** Design:570-573 has N-F grep that name in C++.
- **QML compares the integer instead.** An unknown kind 3 falls into whatever the `else` branch does. These sites need
  a deliberate decision:
  - `MorphWidget.qml:233-246`;
  - `MorphStack.qml:117`, `180-186`, `210`, `273`, `319`;
  - `MorphSplitter.qml:26`;
  - `MorphSelect.qml:270`, `309` (a percent segmented select draws at its implicit width, as a fixed one does; state
    it);
  - `MorphGrid.qml:43`, `51`, `139`, `149`;
  - `MorphTable.qml:107` (an unknown kind becomes a stretch column);
  - `MorphDialog.qml:86`, `132` and `MorphDrawer.qml:83`;
  - `MorphPanel.qml:22`, `MorphSlot.qml:50`, `60`, `MorphRoots.qml:24`, `MorphCollapsible.qml:21`.
- **The C++ to-integer writers count too:**
  - `src/qt_quick/widget.cpp:165,167`;
  - `src/qt_quick/table.cpp:46`;
  - `src/qt_quick/containers.cpp:91`.
- **The terminal has sites that compare to `Fixed` or `Stretch` only:**
  - `src/tui/widget.cpp:613-618` (named), `697`, `719`, `789`;
  - `src/tui/container_widgets.cpp:288`, `381`;
  - `src/tui/layout.cpp:53`, `145`, `179`, `226`;
  - `src/tui/table_widget.cpp:242`.
- **The terminal's property-test oracle repeats the rule.** `tests/tui/test_tui_layout.cpp:377-379` (`requestOf`) and
  `:395-423` restate it. Unless the oracle and its generator learn percent, the property test never covers the new
  kind.

**Fix.**

- Put this list in N-F's brief, with "grep `widthKind`, `heightKind` and `\.kind ===` in `src/qt_quick/qml` as well".
- Add percent to the oracle and to the generator.

### S6. N1: splitter details

- **No document splitter reports a drag.** Design:172-173 and design:177 say `onResize` reports `sizes`. The
  interpreter's splitter has **no** `onResize`, so a document's drag stays in the widget
  (`interpreter.hpp:8090`), and a remount re-applies the shares. State that.
- **The new report must not fire on today's screens.** Design:173-175 adds a report when a pane's sizing is not
  `content` while `sizes` matches. Before landing, check that it fires on none of console (`console.cpp:813-818`), the
  explorer, or review before S2. Otherwise the grab logs, which must be empty, gain lines.
- **The drag test proves only the existing gate.** The "simulated drag" at design:234-235 sets `userSized` and resizes.
  That only proves the `userSized` gate (`MorphSplitter.qml:100-107`), which exists today. Set the pane's
  `SplitView.preferredWidth` to 350 first, then expect 350 after the resize to 785. With percent re-applied regardless,
  it would be 220.
- **A share can round to no cell.** On the terminal, `distribute` does not guarantee a pane a cell: `percent(1)` of 50
  cells is 0, and the drag code assumes at least 1 (`region_widgets.cpp:241`). Keep `max(1, ·)` after `distribute`,
  as design:204 says, and test it.
- **The design names the wrong test file.** The terminal splitter's tests live in `tests/tui/test_tui_regions.cpp:76-110`,
  not in `test_tui_containers.cpp` (design:224). Its existing equal-share case (`:87`, two content texts) must still
  give `{10, 10}`.

### S7. N2: a badge in a stack versus "a container never lays a child out below its minimum"

- **The contract says a badge does not shrink.**
  - A badge is a label-width kind whose `shrinkWidth` is −1, meaning it "cannot be narrower than its natural width"
    (`MorphWidget.qml:45-48`).
  - The contract says a container is never laid out narrower than its children's minimums
    (`backend_contract.md:264-266`).
- **The design does the opposite.** It makes a badge that a stack's `GridLayout` shrinks elide (design:273-274). That
  silently changes the stack rule, and only the "Table cells" paragraph is updated (design:281-282).
- **Where the defect actually is.** In a stack it is that the badge leaves `Layout.minimumWidth` unset, which a
  `GridLayout` reads as 0.

**Fix.**

- Elide where a container places the badge at a width of its own: a table cell, `MorphTableRow.qml:93`.
- In a stack, give the badge `Layout.minimumWidth: naturalWidth` (a `honoursContentMinimum` badge), so the container's
  minimum rises, as the contract says.
- Or keep the design's behaviour and rewrite `backend_contract.md:263-268` to say that a label-width child elides
  where it is squeezed. Either way, say which.

### S8. N2, terminal: a removable chip's click target and the elide helper

- **The remove target stays where the full caption ended.** `BadgeImpl::click` places the remove target at
  `displayWidth(caption()) + 2` (`src/tui/display_widgets.cpp:287-291`). Once the caption is elided, the `×` is drawn
  earlier, so the click misses it and may land on a cell that is not the button. Hit-test against the caption as drawn,
  and add a test: a removable chip in 8 cells is removed by a click on its `×`.
- **The badge cannot reach `elidedLine`.** It sits in an anonymous namespace in `src/tui/leaf_widgets.cpp:88-91`, so
  `display_widgets.cpp` cannot call it. Move it to `src/tui/widget.hpp`/`widget.cpp`, beside `displayWidth`.

### S9. N3, terminal: the premise about the helper is wrong, and the runs, the mark and the table are not covered

- **The helper is already shared.** `alignedOffset` is declared in `src/tui/widget.hpp:580` and defined in
  `src/tui/widget.cpp:774`. It is not local to `table_widget.cpp` (design:328-329, design:619-620). Drop the move.
- **"Offsets each line" covers only the plain path** (`leaf_widgets.cpp:191-198`):
  - Runs are painted by `paintRuns`, which flows run after run with no per-line width (`widget.cpp:837-866`). It needs
    a pass that measures each line before painting.
  - The required mark is placed from column 0 (`leaf_widgets.cpp:199-206`).
- **On the terminal, a text's own align cannot win inside a table cell.**
  - `TextImpl` is placed across its column (`leaf_widgets.hpp:30`).
  - The table moves the *widget*: it gives the widget its natural width at the column's start, centre or end
    (`widget.cpp:760-766`, from `places` at `table_widget.cpp:424`).
  - So design:324 ("`TextImpl` keeps its own alignment … in the same way") has nothing to act on, and the only table
    test is Qt's (design:349).

**Fix.**

- In `columnAreas`, a text that has its own align gets the whole column width, so its offset places it.
- Add the terminal twin of design:349.
- Specify the runs and the mark.

### S10. N3: the author API can throw where a type would do

`TextOptions::align` as `std::optional<Align>`, refusing `Stretch` with `std::invalid_argument` (design:316-317), is a
runtime error that a type removes.

**Fix.** Add `enum class TextAlign { Start, Center, End }` to `author.hpp`, or reuse `Justify`'s first three values
under a new name. Map it to `Align` at lowering.

### S11. N4: fix the cause, then keep the tolerance

- **The rounding error is in the share loop.** `MorphTable.columnWidths` hands each pending column
  `left × weight / weights` (`src/qt_quick/qml/MorphTable.qml:116-121`). Floating-point shares can sum over `left`, and
  `interactive` is `columnsWidth > width` (`:346`).
- **A tolerance treats only the symptom.** The terminal shares whole cells and hands out the remainder
  (`src/tui/layout.cpp:62-81`).

**Fix.**

- The last pending column that is not floored takes `left − Σ(other shares)`, so the sum is `total` exactly.
- Then keep `>= 1` as the documented rule.
- Add `Σ widths + lead == width` at every width of the sweep at design:383-384 to the test. It would fail today, and is
  therefore a sharper control than "interactive somewhere".

The probe gate (design:364-368) stays as written.

### S12. N1's arithmetic holds, but two of M-S's "limits" are last-known-good values

**Measured** on the grabs:

- **Review.** The splitter runs from x 215 to the window edge: 1000 px at 1215, 785 at 1000. The queue is 215–430 at
  both (`lg-ms-review-*`). 0.28 × 1000 = 280 and 0.28 × 785 = 219.8.
- **Receipt.** The row is 231–1199 = 968 px at 1215 and 231–984 = 753 at 1000. 0.31 × 968 = 300.1 and
  0.31 × 753 = 233.4.
- **Audit.** The rail starts at the page edge (215, `lg-ms-audit-1000x830.png`), so the row is the page. 220 and 172.7.
- **Stability.** The row is the same 968 and 753 as receipt, giving 251.7 and 195.8.

All of design:103-108 checks out.

What does not check out is the "M-S's limit" column:

- For review, 28 units (215 px) is the last **good** value and 36 (276 px) the first bad. 220 lies between them and is
  untested, not "5 px over the limit".
- The same holds for audit: 173 px lies between the good 22 units (169) and the bad 28 (215). The design gives audit no
  fallback.
- The provided `lg-ms-receipt-*` grabs are M-S's **39-unit trial**, not HEAD: the pane is 899–1199 and 684–984, both
  300 px, and the 1000 grab clips "Release, finished prod". P1's 236 px comes from findings A.

**Fix.**

- Retitle the column "last good / first bad".
- Give audit the same fallback and acceptance as review: no crushed Reason column at 1000, else `percent(21)`.
- Note that the receipt grabs predate `a30f51c93`.

### S13. N5: a simpler route, and a token that can only grow

- **The token cannot go below the gap.** `max(0, headPaddingY − gapLength)` (design:412-413) can only add.
  `head.paddingY: 4px` still pads by 8, which contradicts "in place of the step-2 gap" (design:404-405).
- **The bar's gap does nothing but pad.**
  - `_topBar`'s only children are `_topAbove`, `_topLine` and `_topBelow` (`include/morph/ui/shell_frame.hpp:73-76`).
  - So the "gap between the bar's lines" that design:405 keeps at step 2 does not exist.
- **The simpler route.** With `headBar` set, `MorphStack.gapLength` (`MorphStack.qml:144`) is the token itself:
  `headBar && Theme.hasHeadPaddingY ? Theme.headPaddingY : spaceOf(gap)`. There is no `insetY` arithmetic, and any
  value is honoured exactly.
- **"No default" needs no new machinery.** The `font.size.*` tokens already use "0 means derived"
  (`include/morph/ui/theme.hpp:1269-1271`). With default 0 meaning "the bar's step gap":
  - `fillValue` (`theme.hpp:1551-1555`) is unchanged;
  - the token count stays `size() - 4` (`tests/test_ui_theme.cpp:209`);
  - only `kNonNegativePaths` (`theme.hpp:998`, `std::array<…, 1>`) grows to 2.
- `setHeadBar` itself is the smallest seam that works:
  - the bar's surface cannot identify it, because a screen's `headSurface` can name any surface
    (`shell_frame.hpp:467`);
  - the frame cannot pass pixels.

  Keep it, and implement it on Qt's `TableRowImpl` too, the other `StackWidget` (`src/qt_quick/kinds.hpp:610-616`), as
  a no-op.

### S14. N5/S6: one 12 px value does not match every head, and only plate is checked

Applying +4 px a side to P5's numbers (design:58-64) gives:

| Screen | Result | Mockup |
|---|---|---|
| plate | 55 | 55 |
| results | 65 | 64 |
| stability | 85 | 85 |
| receipt | 46 | 49 (pads 14) |
| samples | 48 | 45, 3 px **taller** than the mockup |

S6's acceptance re-measures plate alone (design:450).

**Fix.**

- Re-measure all five against the mockups, at ±3 px.
- Record receipt and samples as known deltas, since the mockups pad by 12 or 14.
- Check that the joined head (`toolbarJoined`, `shell_frame.hpp:463-468`) still meets its page without a seam.

---

## The lane split

**Confirmed clean.**

- The file sets do not overlap:
  - N-F owns `include/morph`, `src`, `tests` and `docs/spec`;
  - N-S owns `examples/lims_gallery`, which includes its tests under `examples/lims_gallery/tests/` (design:507-508,
    design:551-553).
- No design item has N-S edit a framework file, and none has N-F edit the gallery.
- N4's probe (design:364-365) builds and grabs the gallery in N-F's own worktree without editing it.
- The gallery's design notes are the coordinator's.

**Two gaps in the hand-off, which are not file conflicts:**

1. **N-F's changes can break N-S's tests.**
   - N1 changes the default placement of a splitter's panes.
   - N5 makes `ShellFrame` call `setHeadBar` on every expanded frame, and `RecordingBackend` records it.
   - Either can change `examples/lims_gallery/tests/` (`test_review.cpp`, `test_console.cpp`), which N-F cannot edit.

   N-F should run the gallery's test target after each of N1 and N5 and hand any failure to N-S, not fix it.
2. **The suite list is short.** "The six touched test files" (design:538) is closer to fourteen: the N1 to N5 lists
   plus the conformance runners from B3. Say "every suite whose file the item touches, plus
   `test_tui_conformance` and the Qt conformance runner".
