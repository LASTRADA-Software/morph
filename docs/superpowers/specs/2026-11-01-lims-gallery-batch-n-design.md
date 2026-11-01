# LIMS gallery batch N: panes sized as a share of the page, tags that elide in a table, text placed at its end, a table that fits, and the head's padding

Batch N closes what batch M's screen lane (M-S) could not reach from the screens, and the framework gaps left in the
findings after batch L (`lg-lr-findings-A.md`, `lg-lr-findings-B.md`) that M-F and M-S did not close. Every
gallery-to-mockup gap that needs ui/1 to change is designed here, so none is left for later. The screen work that
follows from it is lane N-S's work. N-S also takes the remaining screen items that need no new API.

The aim is unchanged: ui/1 produces each mockup's look directly, and a screen never works around the framework. The
rules of batches G to M hold:

- a look is a token or a rule, never a screen's choice of pixels;
- a key means the same thing on every kind that takes it;
- a look that changes every application's shell is the theme's to switch;
- the terminal keeps its cells, and a purely visual change says what it leaves unchanged there;
- vocal over silent: where the framework cannot do what a document asks, it says so.

The gallery is judged on the 1215×830 grab, where the page beside the 215 px sidebar is about 1000 px, as the
mockups' window is. On the 1000×830 grab (a page of about 785 px) nothing may clip, overlap or lose content.

Every claim was checked against `lg-fold-m` (`24d939686` plus the M-F and M-S commits, head `a30f51c93`) by reading
the code. The grabs are M-S's `lg-ms-*.png`, the batch-L grabs are in `lg-cmp/app-l/`, and the mockup renders are in
`lg-cmp/mock/`. Each item says whether its premise was **measured** (with PIL, on those grabs) or **read** (inferred
from the code). Nothing was built or run for this design.

An independent review (`lg-n-review.md`) found 4 blockers and 14 should-fixes. This revision takes all of them; see
"Review round 2".

## Probes

- **P1. Pane widths** (measured; receipt's from findings A, see the note). The mockup's pane is `width: Npx;
  flex: none` beside a `flex: 1` sibling, in a 1000 px window. The containers measure as follows:
  - review's splitter runs from x 215 to the window's edge: 1000 px at 1215 and 785 at 1000;
  - receipt's and stability's rows are 231–1199 (968 px) at 1215 and 231–984 (753 px) at 1000;
  - audit's rail starts at the page's edge, so its row is the page.

  | Screen | Pane | Now at 1215 | Mockup | How it is sized now | M-S at 785: last good / first bad |
  |---|---|---|---|---|---|
  | review | queue | 215 px (x 215–430) at both widths | 280 | splitter `sizes {28, 75}` | 28 units (215 px) / 36 units (276 px): the worksheet table scrolls |
  | receipt | session | 236 px (findings A) | 300 | row child `fixed(31)` | 31 units (236) / 39 units (300): the sample card's fields clip |
  | audit | filter rail | 170 px (findings A) | 220 | row child `fixed(22)` | 22 units (169) / 28 units (215): Reason is crushed |
  | stability | summary | 199 px (1000–1198) | 250 (770–1018) | row child `fixed(26)` | — |
  | instrument | right column | 314 px (885–1198) | 320 | `stretch(2)` : `stretch(1)`, which is close enough | — |

  The `lg-ms-receipt-*` grabs are M-S's 39-unit trial, which predates `a30f51c93`. Their pane is 300 px at both
  widths, and the 1000 grab clips "Release, finished prod". HEAD's 31 units is not grabbed here. A unit is
  `Theme.unitWidth`, about 7.67 px, and it follows the font.
- **P2. Review's worksheet table scrolls sideways at 1215** (measured on `lg-ms-review-1215x830.png`). A scroll bar
  is drawn under the rows. Its handle spans x 454–1196 of a track whose rule runs 452–1198, so the columns are wider
  than the table by a fraction of the track, a pixel or two at most. The 1000 grab has no bar.
- **P3. A tag that is narrower than its text** (read).
  - `MorphTableRow.place()` gives a label-width cell `min(implicitWidth, room)` (`MorphTableRow.qml:93`).
  - `MorphBadge` draws its `label` at its implicit width in a `Row`, with neither `elide` nor `clip`, so the text
    runs past the tag's fill into the next column.
  - On the terminal, `BadgeImpl::paint` writes `[` + caption + `]`, and the canvas cuts it at the widget's edge,
    closing bracket included.
- **P4. Text has no horizontal alignment** (read).
  - The `text` node has no `align` key. `MorphText.textAlign` exists, but only `MorphTableRow.place()` writes it.
  - The review note is a `stretch` text, so it draws at the start of its slot, beside Reject: "Approving needs your
    signature" starts at x 672 in `lg-ms-review-1215x830.png`. The mockup ends it at Approve.
- **P5. Head heights** (measured: the first full-width rule under the head; the app on `app-l/*-1215x830`, the
  mockups from `lg-cmp/mock`).

  | Screen | App | Mockup | Mockup head padding (CSS) |
  |---|---|---|---|
  | plate | 47 | 55 | `12px 16px` |
  | receipt | 38 | 49 | `14px 18px` |
  | samples | 40 | 45 | `14px 16px` (window) |
  | results | 57 | 64 | `12px 16px` |
  | stability | 77 | 85 | `14px 20px` |

  The app's expanded bar is the column `_topAbove`, `_topLine`, `_topBelow` (`shell_frame.hpp:73-76`), with a gap of
  step 2 (8 px). That gap only pads. The mockups pad by 12 to 14 px, so every head is 5 to 11 px short.
- **P6. Results 390, the gap above Impurity A's caption** (measured).
  - App: the comment box's bottom border is at y 448 and the caption's ink starts at y 473, a gap of 25 px.
  - Mockup: the same edges are at 485 and 502, a gap of 17 px.
  - The extra 8 px is the bottom half of `inlineReason`'s `padding(2, 0)` (read: `results.cpp:583`).
- **P7. Already fine or accepted** (measured on the M-S grabs):
  - review 390: about 19 px from the last result to "Review comment", against the mockup's 22;
  - stability 1000: the Containers per pull hint takes 3 lines on the 785 px page and 2 at 1215, as in the mockup.
    That is fit;
  - instrument: the right column is 314 px against 320.

## The items

| Item | Verdict | Where it lands | Lane |
|---|---|---|---|
| N1 a pane sized as a share of its row | **Measured** (P1), **read** (`MorphSplitter.apply`, `layout::distribute`) | a width sizing `percent(n)` along a row and for a horizontal splitter's pane, with the splitter's panes now honouring their bounds | N-F |
| N2 a tag narrower than its table column | **Read** (P3); M-S reported the overlap; morph#1073 | a badge placed by a table at less than its natural width elides its text | N-F |
| N3 a text placed at the end of its slot | **Read** (P4) | a `text` key `align` (`start`, `center`, `end`) | N-F |
| N4 review's table scrolls sideways at 1215 | **Measured** (P2); the cause is **inferred**; morph#1074 | probe first; shares that sum exactly; a table scrolls only at 1 px or more | N-F |
| N5 every head is short of the mockups | **Measured** (P5) | a theme dimension `head.paddingY`: the expanded head bar's gap on Qt Quick | N-F (the gallery's value: N-S) |
| S1 to S7 the screens | P1, P2, P5 and P6 | below | N-S |

## N1: `percent(n)`, a width that is a share of its row

**Problem.**

- `Sizing` has three kinds (`view.hpp`), and none says what the mockups mean by a pane of 280 px beside a `flex: 1`
  sibling.
  - `fixed(n)` is in units. A unit follows the font, and the size does not follow the page.
  - `stretch(w)` shares the *leftover* space by weight, after each sibling's natural length, so a ratio depends on
    the siblings' content. Receipt's came out at about 450 px, and instrument's at 314.
  - A splitter's `sizes` are units as well.
- **What the mockups mean** is CSS's `clamp(min, n %, max)`: the mockup's width on the judged page, giving way on a
  narrower one, and not growing past the mockup's width on a wider one. Percent with a `maxWidth` says that. The
  shares (measured on P1's containers):

  | Screen | Share and `maxWidth` | 1000 px page (judged) | 785 px page | M-S at 785: last good / first bad |
  |---|---|---|---|---|
  | review queue | 28 % of the splitter, max 37 units | 280 | 219.8 | 215 / 276 |
  | receipt session | 31 % of the row, max 39 units | 300.1 | 233.4 | 236 / 300 |
  | audit rail | 22 % of the row, max 29 units | 220 | 172.7 | 169 / 215 |
  | stability summary | 26 % of the row, max 33 units | 251.7 | 195.8 | — |

  Review's 220 and audit's 173 lie between M-S's last good and first bad values, so neither is tested. Both get a
  fallback (S2, S5). In a 1920 px window (a page of about 1705 px) the maxima stop the queue at 284 px and the rail at
  222, where 28 % and 22 % alone would give 477 and 375.

**Options considered.**

- **A minimum, preferred and maximum triple** (CSS `flex-shrink`). Rejected: the sibling never asks for room. The
  worksheet's table takes its own overflow and does not raise its container's minimum (`backend_contract.md`,
  "Content that does not fit"), so the pane would never give way. It would also keep font-dependent units.
- **A size per breakpoint.** A screen can already do this with its width handle and `cond`. That gives two
  font-dependent values, not a curve, and it is a screen workaround.
- **A fractional share, `percent(n)`,** clamped by the node's existing bounds. Chosen. It says the mockup's intent
  in one declaration, it is font-independent, it means the same on the terminal (a share of the cells), and it needs
  no signal from the sibling.

**Decision.**

- **The kind.** `Sizing::Kind::Percent`, made by `Sizing::percent(int percent)`, with an amount from 1 to 100.
  - The width it gives is `percent / 100` of the container's width inside its padding. Gaps and a splitter's handles
    are not taken off first, as in CSS.
  - The result is clamped into the node's `minWidth`/`maxWidth`, as every sizing is. A pane is usually
    `percent(n)` with a `maxWidth`, and `document.md` says so.
- **Widths only, and only where a share has a container length to be taken of:**
  - along a row that does not wrap;
  - as the pane width of a **horizontal** splitter.

  Everywhere else a percent is refused, as `alignSelf` is (`checkPlacement`):
  - The loader refuses a literal `{"percent": n}` in each of these cases:
    - a height;
    - a node whose parent the document writes as a column, a grid, a table column's `width`, a panel, a scroll, a
      tab page or the root;
    - a wrapping row;
    - a vertical splitter's pane;
    - a dialog or a drawer;
    - an amount outside 1 to 100.
  - The mount throws `std::logic_error` for the same cases on a C++ view tree:
    - in `checkLayout` (a height, an amount out of range);
    - in `checkPlacement` (the parent is not a non-wrapping horizontal stack or a horizontal splitter);
    - in `checkOverlayLayout`;
    - in the table's and the grid's column checks.
  - A computed sizing that yields a percent out of range, or a percent where none is allowed, is reported once at its
    path and taken as `content`.
  - The message: "`percent` sizes a width along a row or a horizontal splitter; nothing here takes a share".
  - Heights are left out because nothing in the batch needs one, and a share of a content-sized page's height means
    little. This also removes every height loop path (`MorphStack.implicitHeight`, and the `heightKind === 2` sites of
    `MorphPanel`, `MorphSlot`, `MorphRoots` and `MorphCollapsible`).
- **What resolves it, on each frontend.** A percent child is a fixed child whose width is computed from its container,
  and each frontend treats it exactly as it treats a fixed child today:
  - **Qt Quick** honours `Layout.minimumWidth`. That falls back to `contentMinimumWidth` for a stack or a panel
    (`MorphWidget.qml:247-248`, `MorphStack.qml:81`, `MorphPanel.qml:41`). So a percent stack or panel is raised to
    its content's minimum, as `backend_contract.md` ("a stack or a panel is never laid out narrower than … its
    children's minimums") already says. A table, a scroll or a leaf takes its own overflow.
  - **The terminal** clamps by the node's bounds alone (`solvedAreas`, `widget.cpp:701-708`), as it does for a fixed
    child.

  This is the smaller change: it describes the existing fixed-child behaviour and changes no frontend.
  `backend_contract.md` says once that a percent child is placed as a fixed child, per frontend.
  - Only a `stretch` sibling takes the spare width.
  - In a row with no `stretch` child, `justify` packs a percent child as it packs a fixed one.
- **Its part in its container's own size.** A percent child adds its content's natural width (within its bounds) to
  the row's natural width, and its `minWidth` (none: 0) to the row's minimum. The share a row hands out therefore
  never depends on the row's own natural width. In a content-sized row, the share is taken of the row's natural
  width.
- **Holders.** The interpreter's `listColumn` passes a node's sizing on through `holderSizing`.
  - **The rule applies only to a `listColumn` that sits directly in a row:** an inline list of nodes, or a `forEach`
    body. A holder of **one** node takes that node's percent, and the node is lowered with its width as `stretch(1)`,
    so it fills the holder rather than taking the share twice. Among several nodes, a percent child is reported once
    ("a percent among several nodes is a share of their holder") and is refused as a column's child would be.
  - **A switch case's node and a dialog's content.** These are mounted into the switch's `SlotWidget`
    (`mount.hpp:1044-1066`) or into the dialog, through `lazy` and `holding`. A percent there is reported once ("size
    the `switch` itself; a case fills it") and lowered as `stretch(1)`. It does not throw.
  - The author puts `percent` on the `switch` node. Its slot, sized from the switch's own `commonOf`
    (`interpreter.hpp:8851`), is the row's child, so the share reaches the row.
- **The splitter.**
  - **Placement without matching `sizes`.** Each pane of a horizontal splitter is placed by its own width sizing:
    - `fixed(n)` is n units;
    - `percent(n)` is a share of the splitter's width;
    - `stretch(w)` shares what is left by weight;
    - `content` counts as `stretch(1)`.

    So all-content panes, and all `stretch(1)` panes, still share equally: `examples/gallery`'s two texts are
    unchanged, and so is `test_tui_regions.cpp:87`'s `{10, 10}`. A vertical splitter keeps its equal share.
  - **Bounds.** Every pane's `minWidth`/`maxWidth` (or height, for a vertical splitter) now bound it, on both
    frontends, and a user's drag too:
    - On Qt Quick, each pane's `SplitView.minimumWidth`/`maximumWidth` (`minimumHeight`/`maximumHeight`) are bound
      from its `minWidthUnits`/`maxWidthUnits` (or height) times the unit.
    - On the terminal, `layoutExtents` passes the bounds into the `layout::Item`s it builds, and the drag
      (`region_widgets.cpp:229-249`) clamps into them.

    Today neither frontend reads a pane's bounds (`MorphSplitter.qml:20-36`, `region_widgets.cpp:149-171`). That is
    what lets a rail say `percent(28)` with a `maxWidth`.
  - **Matching `sizes`** still win, bounds still apply. A pane whose width is not `content` while a literal `sizes`
    matches is reported once: "this splitter's `sizes` places its panes; pane N's sizing is not used". Before
    landing, N-F checks that this fires on none of the tree's splitters (console, explorer, review before S2), so the
    grab logs stay empty.
  - **A drag.** The interpreter's splitter has no `onResize` (`interpreter.hpp:8090`), so a document's drag stays in
    the widget. Until the user drags, the shares are applied again whenever the splitter's width changes. After a
    drag the panes keep the dragged sizes, and a remount applies the shares again.
  - **A pane's least width.** On the terminal, a pane is at least one cell after `distribute`: `max(1, ·)`, since
    `percent(1)` of 50 cells is 0 and the drag assumes 1 (`region_widgets.cpp:241`).
- **API surface.**
  - `view.hpp`: `Sizing::Kind::Percent`, `Sizing::percent(int)`, and the doc of `amount`.
  - `author.hpp`: `ui::percent(int)`, which writes `{"lit": {"percent": n}}`. Its doc says it sizes a width.
  - `interpreter.hpp`: `sizingOf`, `isSizing`, `holderSizing` and its parent rule, the switch and `lazy` reports, and
    the splitter's report.
  - `document.hpp`: `sizingDimension` and the refusals.
  - `mount.hpp`: `checkLayout`, `checkPlacement`, `checkOverlayLayout`, and the table and grid checks.
  - `testing/recording_backend.hpp`: `formatSizing` prints `percent(n)`.
- **Qt Quick.**
  - `MorphWidget`: for `widthKind === 3`, `Layout.preferredWidth` is `amount / 100 × (stack.width − 2 × stack.insetX)`,
    where `stack` is `parent?.parent as MorphStack`, or −1 when there is none, and `Layout.fillWidth` is false. The
    child reads the `MorphStack` two levels up, never `parent`: `parent` is the `GridLayout`, whose width in a packed
    row is its own `implicitWidth`, which includes the share.
  - `MorphStack`: while any shown child has `widthKind === 3`, `implicitWidth` is computed by hand. It is the sum
    along a row of `naturalWidth(cell)` (`MorphStack.qml:189-203`, where a percent counts as `implicit`), plus the gaps
    and `2 × insetX`, rather than `layout.implicitWidth`, which Qt builds from every child's `Layout.preferredWidth`.
  - In `shownLengths`, a percent child gives its natural width where the list feeds `naturalLength` (the `between`
    path's `implicitWidth`), and its share where it feeds `betweenSpacing`.
  - No stretch factors: `GridLayout`'s are not proportional when the preferred widths differ, which is what M-S hit
    on receipt.
  - `MorphSplitter.apply()` computes each pane's preferred width from its kind when `sizes` does not match, gives
    the largest-weight stretch pane `SplitView.fillWidth`, and binds the bounds. `paneExtents` counts a percent pane at
    its implicit width.
- **Terminal.**
  - `layout::requestOf` takes the track's length: a percent item asks for `length × n / 100`, rounded down, within its
    bounds.
  - `requestedSize` counts a percent width at its natural width.
  - `SplitterImpl::layoutExtents` builds `layout::Item`s with bounds from its panes' hints when `_sizes` does not
    match, calls `layout::distribute`, keeps every pane at least one cell, and lets the last take the remainder.
- **Every site that reads the kind** (N-F's brief).
  - Grep `Sizing::Kind` in C++, and `widthKind`, `heightKind` and `\.kind ===` in `src/qt_quick/qml`. Each site
    decides on kind 3 deliberately; an unknown kind must never fall into an `else`.
  - QML:
    - `MorphWidget.qml:233-246`;
    - `MorphStack.qml:117`, `180-186`, `210`, `273`, `319`;
    - `MorphSplitter.qml:26`;
    - `MorphSelect.qml:270`, `309`: a percent segmented select draws at its implicit width, as a fixed one does, and
      `qt_quick/backend.md` says so;
    - `MorphGrid.qml:43`, `51`, `139`, `149`, which are unreachable since a percent is refused there;
    - `MorphTable.qml:107`: an unknown kind must not become a stretch column, and is refused before it gets there;
    - `MorphDialog.qml:86`, `132` and `MorphDrawer.qml:83`;
    - `MorphPanel.qml:22`, `MorphSlot.qml:50`, `60`, `MorphRoots.qml:24`, `MorphCollapsible.qml:21`.
  - The C++ writers: `src/qt_quick/widget.cpp:165`, `167`, `src/qt_quick/table.cpp:46` and
    `src/qt_quick/containers.cpp:91`.
  - The terminal:
    - `src/tui/widget.cpp:613-618`, `697`, `719`, `789`;
    - `src/tui/container_widgets.cpp:288`, `381`;
    - `src/tui/layout.cpp:53`, `145`, `179`, `226`;
    - `src/tui/table_widget.cpp:242`.

**Spec.**
- `view_tree.md`: the `layout` row, and Splitter's `sizes` and bounds.
- `document.md`: `sizing` (widths only; a pane is usually `percent(n)` with a `maxWidth`), and the `splitter` row.
- `author.md`: the sizing helpers.
- `interpreter.md`: the splitter, holders, and switch cases.
- `backend_contract.md`:
  - Splitters (placement by sizing, bounds, a cell's least width);
  - Content that does not fit (a percent child is placed as a fixed one, per frontend).
- `qt_quick/backend.md`: Layout, Splitter, Select.
- `tui/frontend.md`: Splitter, and the stack's distribution.

**Tests.**

- `include/morph/ui/testing/backend_conformance.hpp`. These are new cases, registered in
  `tests/tui/test_tui_conformance.cpp` and `tests/qt_quick/test_qt_quick_conformance.cpp`, so both frontends run them:
  - `percentSharesItsRow`: a `fixed(60)` row of `percent(25)` and `stretch()` gives 15 and 45;
  - `percentClampedByMaximum`: the same row with `maxWidth(10)` on the first child gives 10 and 50;
  - `splitterPlacesPanesBySizing`:
    - a `fixed(41)` horizontal splitter of `percent(50)` and `content` gives 20 and 20 (the handle is not taken off
      first);
    - both panes `content` give equal shares;
    - matching `sizes` win;
  - `splitterHonoursPaneBounds`: a pane `stretch()` with `maxWidth(8)` beside a `stretch()` pane in a `fixed(41)`
    splitter is 8.
  - **Control:** with `Percent` read as `Content`, `percentSharesItsRow` fails. With the bounds unread,
    `splitterHonoursPaneBounds` fails.
- `tests/tui/test_tui_layout.cpp`:
  - `distribute([percent(30), stretch(1)], {length 100, gap 2})` gives `[30, 68]`;
  - with `maxWidth 20` on the first item, `[20, 78]`;
  - `percent(33)` of 10 cells gives 3;
  - the property-test oracle's `requestOf` (`:377-379`) and its model (`:395-423`) learn percent, and the generator
    draws `percent(1..100)` items, so the property test covers the new kind.
  - **Control:** with `Percent` read as `Content` (natural 5), the first case gives 5 and fails.
- `tests/tui/test_tui_regions.cpp`:
  - a horizontal splitter of 101 cells with panes `percent(28)` and `content` gives 28 and 72;
  - `percent(1)` of 51 cells gives the first pane 1 cell;
  - a pane with `maxWidth 10` is 10, and a drag past it stops at 10;
  - `sizes {10, 90}` wins;
  - the existing equal-share case (`:87`) still gives `{10, 10}`.
- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - a row 1000 px wide holding `percent(28)` and `stretch` children: the first is 280 ± 0.5. Set to 785 it is
    219.8 ± 0.5;
  - the same row whose `stretch` child has an `implicitWidth` of 900: the first child is still 280 ± 0.5, so the
    `GridLayout` does not shrink the share;
  - `percent(28)` with `maxWidth(37)` in a 1705 px row is 37 units;
  - **no binding loop, two cases**, each with an installed message handler that asserts no `Binding loop` message and
    finite widths:
    - a content-sized row under `justify: between` holding a `percent(30)` child;
    - a content-sized row inside a row holding a `percent(30)` child;
  - a splitter whose panes are `percent(28)` and `stretch`:
    - at 1000 px the first pane is 280 ± 1, and after a resize to 785 it is 220 ± 1;
    - set the first pane's `SplitView.preferredWidth` to 350 and `userSized`, then resize to 785: it is 350. If the
      share were applied again regardless, it would be 220;
    - with `maxWidth(20)` on the first pane, a drag past it stops at 20 units.
  - **Control:** the first case and the splitter case fail with `Percent` mapped like `Content`.
- `tests/test_ui_document.cpp` and `tests/test_ui_interpreter_detail.cpp`:
  - `{"percent": 28}` lowers to `Sizing::percent(28)`;
  - each of these is refused at load:
    - the literal amounts 0, 101 and 2.5;
    - a percent height;
    - a percent child of a column, of a grid and of a wrapping row;
    - a percent table column;
    - a percent dialog;
    - a percent pane of a vertical splitter;
  - a computed 150 is reported once and lowers to content;
  - a `listColumn` of one `percent(30)` node in a row gives the holder `percent(30)` and the node `stretch(1)`;
  - a splitter with literal `sizes` and a `percent` pane reports once;
  - the console and review documents (lowered as in the gallery's tests) report nothing new.
- `tests/test_ui_mount.cpp`:
  - a row holding a `switch` sized `percent(30)` with a one-node case mounts on the recording backend, and the
    switch's slot has `percent(30)`;
  - a `percent(30)` node inside a switch case is reported once and does not throw;
  - a percent child of a grid, a column, a table column, a dialog, and a percent height each throw;
  - a percent child of a row does not throw.
- `tests/qt_quick/test_qt_quick_interpreter.cpp`: the mounted switch case above, on Qt Quick. The slot is 30 % of a
  600 px row.
- `tests/test_ui_recording_backend.cpp`: `formatSizing` gives `percent(28)`.

## N2: a tag narrower than its table column elides its text (morph#1073)

**Problem.** P3. In a table, a badge cell placed at less than its natural width keeps drawing its whole label, which
runs over the next column. M-S hit this on review's Evaluation column with `shrink`. The mockups' tags never wrap.
Where a cell is cut, its text elides, as review's Change cell does ("0.12 →…").

**Decision.**

- **Only in a table cell.** A badge elides only where a table places it at a width of its own, which is the one
  container whose cells do not raise it (`honoursContentMinimum: false`). Everywhere else the contract stands:
  - a badge cannot be narrower than its natural width;
  - a container is never laid out narrower than its children's minimums (`backend_contract.md:264-266`).
- **The Qt minimum is set.** On Qt Quick a badge leaves `Layout.minimumWidth` unset today, which a `GridLayout` reads
  as 0. It now sets `Layout.minimumWidth: naturalWidth` (through `contentMinimumWidth`, so `honoursContentMinimum`
  governs it). A stack's minimum then rises with its badges, as the contract says.
- **Inside a container cell**, a badge keeps its natural width, and the cell is cut at its column as today. S3 uses
  a direct badge cell.
- **What it draws when elided.**
  - The label is elided at the end with "…", and the badge keeps its fill, border, padding, icon and remove button.
  - Its accessible name stays the full text.
  - While elided, the full text is its tooltip unless the node gives one, which is the mono text's existing rule.
  - Below its padding plus one character and "…", the label is empty and the frame stays.
- **Qt Quick.**
  - `MorphBadge` gets `property bool elideToWidth: false`. `MorphTableRow.place()` sets it on a label-width cell,
    beside `singleLine`.
  - While it is true, the `label` has `elide: Text.ElideRight` and a `width` of
    `min(implicitWidth, root.width − padding − icon − remove − spacing)`, and the `Row` is clipped.
  - `naturalWidth` reads `label.implicitWidth`, the icon, the remove button and the spacing, not the `Row`'s
    `implicitWidth`. The `Row`'s sums its children's widths, so it would feed the elided width back.
  - The tooltip binds to `label.truncated`.
- **Terminal.**
  - `BadgeImpl` gets `setElided(bool)`, like `TextImpl`'s. `TableImpl::arrangeRows` sets it on a badge cell, and
    the table places a label-width cell at most its column's width.
  - While elided, `paint` cuts the caption so that the closing `)` or `]` stays in the last cell.
  - `elidedLine` moves out of `leaf_widgets.cpp`'s anonymous namespace to `src/tui/widget.hpp` and `widget.cpp`,
    beside `displayWidth`, so that `display_widgets.cpp` can call it.
  - `BadgeImpl::click` hit-tests the `×` where it is drawn (the drawn caption's width + 2), not where the full
    caption would put it.

**Spec.**
- `backend_contract.md`: Table cells ("a label-width cell is placed at its natural width, at most the column's; a
  badge placed narrower elides its text"); Content that does not fit (a badge's minimum is its natural width).
- `qt_quick/backend.md`: Badge.
- `tui/frontend.md`: Badge.
- `view_tree.md`: Badge, the tooltip.

**Tests.**

- `tests/qt_quick/test_qt_quick_table.cpp`: a table with a stretch `shrink` column of badge cells "Out of trend: −0.7 %
  vs. last 5 lots", made narrower than its columns.
  - Each badge's right edge, mapped to the row, is at most the next column's left edge.
  - `label.truncated` is true, the tooltip is the full text, and `Accessible.name` is the full text.
  - **Control:** without the elision, `label.width > badge.width`.
- `tests/qt_quick/test_qt_quick_widgets.cpp`: a badge in a 60 px column (not a table) keeps its natural width, is not
  truncated, and the column's `contentMinimumWidth` is at least the badge's `naturalWidth`. The overflow report
  names the column. This is the contract kept.
- `tests/tui/test_tui_table.cpp`: an "Out of trend" badge cell in a 9-cell column reads `[Out of…]`, with `]` in its
  last cell. **Control:** today's paint gives `[Out of t`.
- `tests/tui/test_tui_display.cpp`: an elided removable chip in 8 cells is removed by a click on its drawn `×`, and a
  click on its first cell does not remove it.

## N3: `align` on a text

**Problem.** P4. A text that stretches, or that fills its column, draws its lines at the start. The review note must
end at Approve and wrap when the pane is narrow.

A spacer with a content-sized note would not do. On the 785 px page the note would not wrap, and on the terminal
`distribute` takes a deficit from the last child first, which is Approve.

**Decision.**

- **The key.** The `text` node takes `align`, a literal name: `start` (the default), `center` or `end`, named as a
  table column's `align` is.
  - A name of no known value is refused at load, and so is a computed one.
  - It places each line within the text's own width. RTL mirrors `start` and `end`.
  - It shows nothing on a text that is as wide as its content (a content-sized text in a row), and the spec says so.
- **API surface.**
  - `view.hpp`: `enum class TextAlign : std::uint8_t { Start, Center, End }`. No value can be refused, so the author
    API cannot throw.
  - `author.hpp`: `TextOptions::align` (`std::optional<TextAlign>`).
  - `view_tree`'s `Text` gains `align` (`TextAlign`, set once). The mount tells the widget only `Center` or `End`.
  - `TextWidget::setAlign(TextAlign)` joins the contract, and every backend implements it. `RecordingBackend`
    records it.
- **In a table, the text cell's own `align` wins** over its column's `align`. The column's still places a default
  cell and a text that gives no `align`. `backend_contract.md`'s Table cells says so.
  - On Qt Quick, `MorphText` gets `ownAlign` (set by `setAlign`), and `MorphTableRow.place()` writes `textAlign` only
    on a cell without it.
  - On the terminal, the table moves the *widget* across its column (`columnAreas`, `widget.cpp:760-766`, from
    `places`). A text with its own align is given the whole column's width there, so its own offset places its lines.
- **Qt Quick.** `setAlign` writes `MorphText.textAlign` (`Qt.AlignLeft`, `Qt.AlignHCenter`, `Qt.AlignRight`), which
  the label's `horizontalAlignment` already binds. A `Text` mirrors it under `LayoutMirroring`.
- **Terminal.** `alignedOffset` is already shared (`widget.hpp:580`, `widget.cpp:774`). `TextImpl::paintControl`
  places each laid-out line by it, on all three of its paths:
  - **Plain text** (`leaf_widgets.cpp:191-198`): each line is offset by its own width.
  - **Runs** (`paintRuns`, `widget.cpp:837-866`): it gains a first pass that breaks the runs into lines and measures
    each line's width. The second pass paints each line's runs from that line's offset.
  - **The required mark** (`leaf_widgets.cpp:199-206`) is placed after the end of its line's text, not from column 0.

**Spec.**
- `document.md`: `text`.
- `view_tree.md`: Text.
- `backend_contract.md`: Alignment; Table cells (the text's own align wins).
- `author.md`: `TextOptions`, `TextAlign`.
- `qt_quick/backend.md`: Text.
- `tui/frontend.md`: Text.

**Tests.**

- `include/morph/ui/testing/backend_conformance.hpp`: `textAlignEnd`, a `fixed(20)` text "abc" with `End`, whose
  probed text extent starts at 17 units from the left on both frontends. It is registered in both conformance
  runners. **Control:** without `setAlign` it starts at 0.
- `tests/test_ui_document.cpp` and `tests/test_ui_interpreter_detail.cpp`:
  - `align: "end"` lowers to `Text::align == TextAlign::End`;
  - `"right"` and a computed align are refused at load.
- `tests/qt_quick/test_qt_quick_widgets.cpp`: a `stretch` text "Approving needs your signature" in a 600 px row,
  `align: end`.
  - **Ink:** `grabToImage` the text. The rightmost non-background column is within 2 px of `label.width`, and the
    leftmost is greater than `label.width − label.contentWidth − 2`. Under `LayoutMirroring.enabled`, the leftmost is
    within 2 px of 0 and the rightmost is less than `contentWidth + 2`.
  - The cheaper second check: `label.horizontalAlignment === Qt.AlignRight`.
  - **Control:** without `setAlign` the ink check fails, since the ink ends near `contentWidth`, about 200 px.
- `tests/qt_quick/test_qt_quick_table.cpp`: a text cell with `align: start` in an `end` column keeps `AlignLeft`.
- `tests/tui/test_tui_widgets.cpp`:
  - "abc" with `End` in 20 cells is painted from cell 17;
  - a two-line plain text places each line at the end;
  - a text with runs ("a **b** c", one line) is painted from `20 − 5`;
  - a required text's `*` follows its text at the end.
  - **Control:** without the offset, each starts at cell 0.
- `tests/tui/test_tui_table.cpp`: a text cell with `align: start` in an `end` column starts at the column's left edge
  (the terminal twin of the Qt case).

## N4: a table that fits does not scroll sideways (morph#1074)

**Problem.** P2. At 1215, review's worksheet table draws a sideways scroll bar whose handle spans nearly the whole
track.

**Inferred cause** (not reproduced). `MorphTable.columnWidths` shares the spare width among the pending stretch
columns as floating-point quotients, `left × weight / weights` (`MorphTable.qml:116-121`). `hflick.interactive` is
`columnsWidth > width` (`:346`). Three equal shares of 100 sum to 100.00000000000001, which makes the table
interactive, draws the always-on bar, and raises its height. The terminal shares whole cells and hands out the
remainder (`layout.cpp:62-81`).

**Decision.**

- **Probe first.** The lane adds a temporary `console.warn` of `columnsWidth - width` to `MorphTable`, grabs review
  at 1215, then removes the probe: `git checkout`, `touch`, rebuild, and re-grab with no probe line.
  - Under 1 px: the cause is the one above, and the fix goes in.
  - 1 px or more: the cause is elsewhere (a content column's cache, the lead, the bar). The lane reports it, updates
    morph#1074, and ships neither change as a fix.
- **The cause.** In each pass of the share loop, the last pending column that is not floored takes
  `left − Σ(the others' shares)`. The stretch columns then sum to `left`, and the columns plus the lead to `total`,
  exactly.
- **The rule.** A table scrolls sideways only when its columns are at least 1 px wider than it:
  `interactive: root.columnsWidth - width >= 1`, with `contentWidth` unchanged. That is the table's form of the
  layout's "a smaller shortfall is rounding". `qt_quick/backend.md` states it, so a future rounding source cannot
  bring the bar back.

**Spec.** `qt_quick/backend.md`, Table ("whose columns are wider than it by 1 px or more scrolls sideways; the
stretch columns sum to the width that is left exactly").

**Tests.** `tests/qt_quick/test_qt_quick_table.cpp`: a table of three `stretch(1)` columns and one `fixed(5)`,
resized through every width from 300 to 900 px in 1 px steps.
- At each width, `Σ widths + lead == width` exactly, and `hflick.interactive` is false.
- A table whose fixed columns exceed it by 1 px is interactive.
- **Control:** today's loop fails the exact-sum assertion at some width in the sweep, which is a sharper check than
  `interactive` alone.

## N5: the head's padding is the theme's

**Problem.** P5. Every expanded head is 5 to 11 px shorter than its mockup.

- The bar's gap (step 2, 8 px) is its padding: its only children are two zero-height spacers around the line
  (`shell_frame.hpp:73-76`).
- That look is shared by every screen, so it is the theme's to switch.
- **Why not step 3 in the frame:** on the terminal a vertical step 3 is one row (`stepCells`), so every terminal head
  would gain two rows.
- **Why not a frame option:** it would be an application's choice of pixels.

**Decision.**

- **The token.** `head.paddingY`, a theme dimension in px, with a default of 0, which means the bar's step gap. That
  follows the `font.size.*` tokens' "0 means derived" (`theme.hpp:1269-1271`).
  - `fillValue` is unchanged, and the token-count check stays `size() - 4` (`test_ui_theme.cpp:209`).
  - `kNonNegativePaths` (`theme.hpp:998`) grows to 2 entries and refuses a negative value.
- **What it does.** On Qt Quick, a head bar's gap is the token: `gapLength` is
  `headBar && Theme.headPaddingY > 0 ? Theme.headPaddingY : Theme.spaceOf(gap)`. The bar has no other gap, so the
  token is exactly the space above and below the line, and any value is honoured, smaller ones included.
- **How the frame says which stack is the head bar.** `StackWidget::setHeadBar(bool)`, a new contract method (any
  time, default false). The bar's surface cannot identify it, because a screen's `headSurface` can name any surface
  (`shell_frame.hpp:467`).
  - `ShellFrame` sets it true on `_topBar` while expanded and false while compact. K12's compact bar is unchanged.
  - Qt's `TableRowImpl`, the other `StackWidget` (`kinds.hpp:610-616`), implements it as a no-op.
  - The terminal records it and changes nothing: it reads no dimension token, so its head keeps its rows.
- **The joined head.** `toolbarJoined` (`shell_frame.hpp:463-468`) is the same bar on another surface. Its gap changes
  in the same way, and the page that continues it starts at its bottom edge, so it meets the page without a seam.
- **The gallery.** Its base sets `head.paddingY: 12px` (S6).

**Spec.**
- `theme.md`: the token in the closed set (0 = the bar's step gap).
- `shell_frame.md`: the bar row.
- `view_tree.md` and `backend_contract.md`: `setHeadBar`.
- `qt_quick/backend.md`: Layout (the head bar's gap).
- `tui/frontend.md`: ignored.

**Tests.**

- `include/morph/ui/testing/backend_conformance.hpp`: `headBarKeepsItsChildren`. A stack with `setHeadBar(true)`,
  toggled false and true again, keeps its children's order and visibility on both frontends, and both runners register
  it. It proves that the method exists everywhere and is harmless. The pixel effect is Qt's alone and is tested below.
- `tests/test_ui_theme.cpp`:
  - the token is in `kThemeTokens` with default 0;
  - `-1px` is refused;
  - the count check is unchanged at `size() - 4`.
- `tests/test_ui_shell_frame.cpp` (`RecordingBackend`): `shell_topbar` is a head bar while expanded and is not one
  while compact; toggling twice restores it.
- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - with `head.paddingY` at 12, the expanded bar's line starts 12 px below the bar's top, and the bar is the line
    plus 24;
  - at 4 px, the line starts 4 px below and the bar is the line plus 8;
  - unset, the line starts 8 px below and the bar is the line plus 16, as today;
  - compact with the token set is still 52 px with 44 px targets;
  - a joined head (`setHeadJoined`) with the token set has no gap between the bar's bottom and the page's first rule.
  - **Control:** if the frame does not call `setHeadBar`, the 12 px case gives 8 and fails.
- `tests/tui/test_tui_shell_frame.cpp`: the bar's row count is the same with the head-bar flag on and off.

## Screen items (lane N-S)

| Item | Status against the grabs | Needs | Work |
|---|---|---|---|
| S1 results 390: the gap above Impurity A's caption | **open**, 25 px against 17 (P6) | nothing | `inlineReason` drops `padding(2, 0)` and starts with a shown zero-height `ui::spacer()`. In its gap-2 column that gives step-2 room above the reason and none below, the shell's own pattern. Re-measure 17 ± 2, and check that the room above "Reason for change" is unchanged |
| S2 review: the queue | **open**, 215 px against 280 (P1) | N1 | drop `sizes`. The queue is `.sizing(ui::percent(28)).maxWidth(37)` and the worksheet `stretch()`. Fix the comment at `review.cpp:823`. **Acceptance:** at 1000 the worksheet table does not scroll sideways; else `percent(27)` (270 px at 1000), and report which |
| S3 review: one full-text tag per evaluation cell | **open**: the cells are a tag over a muted note, 48–56 px rows against the mockup's 30 | N2 | the cell is `layout::tag(row("evaluation"), row("tone"))`, a direct badge cell. The column is `stretch(3)` with `shrink`, beside Analyte, Spec. and Change at `stretch(1)`: about 200 px at 1215, so "Out of trend: −0.7 % vs. last 5 lots" (about 190 px) shows whole, and it elides at 1000. Drop `verdict` and `note` from `resultList`. **Acceptance:** one-line rows (about 30 px) at 1215, and no sideways scroll at 1000; tune the weights and report if they miss |
| S4 review: the approval note at the end | **open** (P4) | N3 | the note keeps `stretch()` and gains `align: end` |
| S5 receipt pane, audit rail, stability summary | **open** (P1) | N1 | receipt `percent(31)` max 39, audit `percent(22)` max 29, stability `percent(26)` max 33. Each comment states the share, the max, and both page widths. **Audit acceptance**, as for review: no crushed Reason at 1000; else `percent(21)` (210 at 1215, 165 at 1000), and report which |
| S6 the heads | **open** (P5) | N5 | `lims_gallery.cpp`: the base sets `head.paddingY: 12px`. Re-measure every head of P5 against its mockup at 1215, within ±3 px: plate 55, results 65 (64), stability 85. Receipt (46 against 49) and samples (48 against 45) are recorded as known deltas, since their mockups pad by 14. Check that the joined heads meet their pages without a seam |
| S7 the review table's bar at 1215 | **open** (P2) | N4 | none. Re-grab and confirm that there is no bar |
| stability 1000 hint | fine: fit on the 785 px page (P7) | — | dropped |
| review 390 gap above the comment | fine, 19 px against 22 (P7) | — | dropped |
| instrument right column | fine, 314 against 320 (P7) | — | dropped |

**Grabs.** Each changed screen at 1215×830, 1000×830 and 390×844, plus review and audit at **1920×1080**, where the
queue must stop at its 37-unit maximum and the rail at 29. Each is taken with `--user "Dr. A. Meyer"` and
`QT_FORCE_STDERR_LOGGING=1`, and every line of each `.log` is reported. The receipt grabs are taken fresh, since
`lg-ms-receipt-*` is the 39-unit trial.

## Other open gaps in the findings

Against findings A and B:
- M-F closed the per-button caption weight, the shadow's fade, `color.dialogRule` and `color.placeholder`.
- M-S closed every screen item in its brief (`eb076e6a3..a30f51c93`) except the pane widths, which are N1, S2 and S5.

The rest are data, not framework, and stay out:
- plate 390's Rows D–H grouping and its A3–A8 split;
- results' "of 2";
- review's "7 results" and "All 7";
- console's sample pane width.

P2 (N4) and P5 (N5) are new; neither report named them.

## Decisions

1. **Percent over a min/preferred/max triple and over breakpoints.** A shrinking pane needs a signal that a table
   never gives. Breakpoints are a screen workaround in font-dependent units. A share needs no signal, does not depend
   on the font, and means the same on the terminal. With a `maxWidth` it is CSS's `clamp`: right at the judged width,
   giving way on a narrower page, and not growing past the mockup on a wide one.
2. **Widths only: along a non-wrapping row, and for a horizontal splitter's pane.** These are the only placements
   the batch needs. A height share brings a second set of loop paths for no use. Everything else is refused or
   reported, never read as content silently.
3. **A share of the width inside the padding,** with gaps and handles not taken off, as CSS `%` does it.
4. **A percent child is placed as a fixed child on each frontend.** On Qt Quick that means a stack's or panel's
   content minimum still raises it; on the terminal its bounds alone apply. This describes existing behaviour rather
   than changing a frontend, which is the smaller change. Its natural width (not its share) feeds its row's natural
   width.
5. **Holders.** A single-node holder takes the percent only when it sits directly in a row. A switch case and dialog
   content report it, and the author sizes the `switch`.
6. **A splitter places panes by their own sizing when `sizes` does not match** (content counts as `stretch(1)`), and
   honours its panes' bounds on both frontends, during a drag too.
7. **A tag elides only in a table cell.** That keeps the contract that a container is never narrower than its
   children's minimums. On Qt, a badge now states its minimum.
8. **`align` is a `text` key, typed `TextAlign`, and a text cell's own `align` wins over its column's** on both
   frontends. The coordinator's "the cell's alignment wins" is read as the text cell's own alignment, which is
   reviewer S9's fix.
9. **N4 fixes the sum and keeps the 1 px rule,** after a probe that can stop the change.
10. **N5 is a theme gap of default 0** that replaces the head bar's step gap on Qt Quick, carried by `setHeadBar`.
    No new token machinery is needed.
11. **Fallbacks:** review `percent(27)` and audit `percent(21)`, if their 785 px acceptance fails.
12. **Dropped as already fine:** stability's 1000 hint, review 390's comment gap, and instrument's column.

## Defects this batch fixes

- **N2, morph#1073.** A badge narrower than its table column overlaps the next column. This is shipped behaviour in
  `src/qt_quick` and `src/tui`. Reported by M-S; the cause is read (P3).
- **N4, morph#1074.** A table that fits scrolls sideways. This is shipped behaviour in `src/qt_quick`. Measured (P2);
  the cause is inferred and is probed first.

The issue numbers stay in this design and in the commit messages, never in code comments (`AGENTS.md`). N1, N3 and N5
are vocabulary additions, not defects.

## Lanes

There are two Sonnet lanes. They run in parallel in separate worktrees, each from `lg-fold-m`'s head, and they share
no file:
- **N-F** owns `include/morph`, `src`, `tests` and `docs/spec`;
- **N-S** owns `examples/lims_gallery`, including `examples/lims_gallery/tests/`.

Every build, test and tidy run goes under `flock <scratchpad>/morph-build.lock`, with `-j10` at most.

### N-F: framework, one commit per item, in this order

1. **N3**, which unblocks S4:
   - `view.hpp` (`TextAlign`, Text), `backend.hpp` (`TextWidget::setAlign`), `author.hpp`, `document.hpp`,
     `interpreter.hpp`, `mount.hpp`, `testing/recording_backend.hpp`, `testing/backend_conformance.hpp`;
   - `MorphText.qml`, `MorphTableRow.qml`, the Qt Text impl;
   - `src/tui/leaf_widgets.*`, `widget.*` (`paintRuns`, `columnAreas`).
2. **N2:**
   - `MorphBadge.qml`, `MorphTableRow.qml`;
   - `src/tui/display_widgets.*`, `widget.*` (`elidedLine`), `leaf_widgets.cpp`, `table_widget.cpp`.
3. **N1:**
   - `view.hpp`, `author.hpp`, `document.hpp`, `interpreter.hpp`, `mount.hpp`, `recording_backend.hpp`,
     `backend_conformance.hpp`;
   - every QML and C++ site in N1's list;
   - `src/tui/layout.*`, `widget.cpp`, `container_widgets.cpp`, `region_widgets.cpp`, `table_widget.cpp`.

   Then N-F runs `lims_gallery_tests` and hands any failure to N-S without fixing it.
4. **N4:** the probe, then `MorphTable.qml`.
5. **N5:**
   - `theme.hpp` (the token, `kNonNegativePaths`), `backend.hpp` (`StackWidget::setHeadBar`), `shell_frame.hpp`,
     `backend_conformance.hpp`;
   - `src/qt_quick/theme.cpp`, `Theme.qml`, `MorphStack.qml`, the Qt stack impl and `TableRowImpl` (a no-op);
   - the terminal's stack impl (a no-op), `recording_backend.hpp`.

   Then N-F runs `lims_gallery_tests` again and hands any failure to N-S.

Each item also updates its spec files.

**Test files N-F touches:**
- `tests/test_ui_document.cpp`;
- `tests/test_ui_interpreter_detail.cpp`;
- `tests/test_ui_mount.cpp`;
- `tests/test_ui_recording_backend.cpp`;
- `tests/test_ui_theme.cpp`;
- `tests/test_ui_shell_frame.cpp`;
- `tests/tui/test_tui_layout.cpp`;
- `tests/tui/test_tui_regions.cpp`;
- `tests/tui/test_tui_table.cpp`;
- `tests/tui/test_tui_display.cpp`;
- `tests/tui/test_tui_widgets.cpp`;
- `tests/tui/test_tui_shell_frame.cpp`;
- `tests/tui/test_tui_conformance.cpp`;
- `tests/qt_quick/test_qt_quick_containers.cpp`;
- `tests/qt_quick/test_qt_quick_widgets.cpp`;
- `tests/qt_quick/test_qt_quick_table.cpp`;
- `tests/qt_quick/test_qt_quick_interpreter.cpp`;
- `tests/qt_quick/test_qt_quick_conformance.cpp`;
- `include/morph/ui/testing/backend_conformance.hpp`, the shared cases.

N-F commits each item on its branch as it lands and tells the coordinator, so that N-S can rebase onto it. The
branches share one repository, and nothing is pushed.

Before reporting done, N-F re-runs:
- every suite that contains a file above;
- both conformance runners;
- `lims_gallery_tests`;
- the docs target (Doxygen `WARN_AS_ERROR`, checked under clang);
- clang-tidy on the touched TUs.

N-F makes **no gallery commit**.

### N-S: screens, S1 to S7

- **First, needing no new API:** S1, and the `resultList` data change behind the current cell, for S3.
- **Then, rebased onto N-F's commits as each lands:**
  - S4 after N3;
  - S3 after N2;
  - S2 and S5 after N1, together with any `lims_gallery_tests` failure that N-F hands over;
  - S7 (a re-grab) after N4;
  - S6 after N5.
- **Files:**
  - `examples/lims_gallery/src/screens/{results,review,receipt,audit,stability}.cpp`;
  - `examples/lims_gallery/src/lims_gallery.cpp` (S6);
  - `examples/lims_gallery/tests/*`, where a test id, a cell's shape, a pane's sizing or the frame's head-bar record
    changes.

The coordinator folds N-F, then N-S, onto the next fold branch, and writes the "After batch N" comparison from one
set of re-grabs.

## Risks

- **Review at 28 % and audit at 22 % are untested at 785:** 220 and 173 px lie between M-S's last good and first bad
  values. The fallbacks are 27 % and 21 %.
- **Qt binding loops.** These are guarded by the hand-computed `implicitWidth` and by the child reading the
  `MorphStack` two levels up. Two loop tests cover the guards.
- **Missed kind sites.** A QML or terminal site that treats kind 3 as `else`. N1 lists them, and N-F greps three
  patterns.
- **Two new contract methods** (`setAlign`, `setHeadBar`). Every backend, `RecordingBackend`, Qt's `TableRowImpl` and
  any test double must implement them. The conformance cases drive both frontends.
- **The splitter.**
  - Its placement changes where `sizes` is empty and the panes are not all `content` or all `stretch(1)`. No splitter
    in the tree is like that.
  - Its panes' bounds now apply. No tree splitter sets them today.
  - The `sizes`-versus-sizing report must stay silent on the tree's splitters.
- **N4's cause is inferred.** If the probe shows 1 px or more, N4 becomes a report, and S7 stays open.
- **A maximum in units follows the font.** 37 units is about 284 px with the gallery's font. A different font moves
  the cap, which is acceptable for a cap.
- **The heads:** receipt and samples stay 3 px off their mockups (S6).
- **Recurring CI traps:**
  - Doxygen on clang catches a stranded `@param` that GCC misses;
  - `document.hpp`'s coverage allowlist hints must be moved for lines added above them;
  - MSVC needs `typename` in a dependent default template argument;
  - no Qt keyword (`emit`, `signals`, `slots`) may be an identifier in `include/morph`.
- **The machine is shared with CI's runners.** Grabs and timing tests are affected, so the lanes hold the build lock.

## Review response (round 1, self-review)

- Holders cover `lazy` as well as `listColumn`. Round 2 superseded this.
- Wrapping rows refuse a percent.
- The badge's `naturalWidth` reads the label's `implicitWidth`.
- S3 weights the Evaluation column `stretch(3)`.
- S1 uses a zero-height spacer.
- The theme rule for an empty default. Round 2 superseded this.

## Review round 2

Every item of `lg-n-review.md` is taken, with the coordinator's choices.

| Item | Resolution |
|---|---|
| **B1** the switch-case holder throws and doubles the share | The holder rule applies only to a `listColumn` directly in a row. A percent on a switch case's node or on dialog content is reported once ("size the `switch` itself") and lowered as `stretch(1)`, without throwing. The author sizes the `switch`, whose slot is the row's child. The interpreter-only test is replaced by a mounted test, on the recording backend (`test_ui_mount.cpp`) and on Qt (`test_qt_quick_interpreter.cpp`), plus a reported-not-thrown test (N1). |
| **B2** the Qt loop guard still loops | The child reads the `MorphStack` two levels up, minus `2 × insetX`, never `parent`. While a percent child is shown, `MorphStack.implicitWidth` is computed by hand from `naturalWidth(cell)`. `shownLengths` gives the natural width to `naturalLength` and the share to `betweenSpacing`. Heights are refused (S4), which removes the height half. There are two loop tests: `between`, and nested content (N1). |
| **B3** no conformance case | New shared cases, registered in both runners: `percentSharesItsRow`, `percentClampedByMaximum`, `splitterPlacesPanesBySizing`, `splitterHonoursPaneBounds`, `textAlignEnd` and `headBarKeepsItsChildren`. `percentAcrossColumn` is dropped, since a percent is now a row's main-axis width only, and the mount test refuses it instead. |
| **B4** N3's Qt test cannot see alignment | A `grabToImage` ink test: the rightmost ink is within 2 px of `label.width`, the leftmost is greater than `label.width − contentWidth − 2`, and the mirror under RTL. `horizontalAlignment` is kept as a cheaper second check, and the control fails the ink check (N3). |
| **S1** "content does not raise it" | Restated per frontend. A percent child is placed as a fixed one: on Qt a container's content minimum raises it, and on the terminal its bounds alone apply. This is the smaller change, describing existing behaviour. A test checks that a 900 px-implicit stretch sibling does not squeeze the share (N1, Decision 4). |
| **S2** shares grow without bound | Panes are `percent(n)` with a `maxWidth` of the mockup's width: review 37, receipt 39, audit 29, stability 33. `document.md` says a pane is usually so. A 1920 px grab is added to N-S's acceptance (N1, S2, S5). |
| **S3** splitter bounds unread | Pane bounds are bound on Qt (`SplitView.minimumWidth`/`maximumWidth`) and passed to the terminal's `layout::Item`s, with the drag clamped. Conformance and frontend tests were added (N1). |
| **S4** limit to width | Widths only, along a non-wrapping row or a horizontal splitter's pane. A height is refused at load and at mount, and a computed one is reported (N1). |
| **S5** the grep misses QML | N1 lists every QML, Qt C++ and terminal site the reviewer named, plus the grep patterns. The terminal's property-test oracle and generator learn percent. `MorphSelect`'s percent behaviour is stated. |
| **S6** splitter details | The missing `onResize` and remount behaviour are stated. The report is checked to be silent on the tree's splitters. The drag test sets `preferredWidth` 350 first. The terminal keeps `max(1, ·)`, with a `percent(1)` test. The tests move to `test_tui_regions.cpp`, keeping `:87`'s `{10, 10}`. |
| **S7** badge in a stack against the minimum contract | The coordinator's choice: elide only in table cells (`elideToWidth` and `setElided`, set by the table). On Qt a badge now sets `Layout.minimumWidth: naturalWidth`, so stacks keep the contract. There is a test that a stack's minimum rises (N2). |
| **S8** terminal chip and `elidedLine` | `click` hit-tests the drawn `×`, with a test. `elidedLine` moves to `widget.hpp` and `widget.cpp` (N2). |
| **S9** terminal alignment premise, runs, mark, table | The wrong "move `alignedOffset`" is dropped, since it is already shared. Runs get a measuring pass, the required mark follows its line, and `columnAreas` gives a text with its own align the whole column. Terminal tests were added, including the table twin (N3). |
| **S10** an author API that throws | `enum class TextAlign { Start, Center, End }` in `view.hpp`, used by `TextOptions` and `Text` (N3). |
| **S11** fix the cause | The last unfloored pending column takes the remainder, so shares sum exactly. The 1 px rule is kept and documented. The sweep asserts `Σ widths + lead == width`, and the probe still runs first (N4). |
| **S12** limits are last-known-good | P1 and N1's tables are retitled "last good / first bad". Review and audit get fallbacks (27 %, 21 %). The note that the receipt grabs predate `a30f51c93` is added (P1, S2, S5). |
| **S13** N5's simpler route | On Qt the head bar's gap is the token (0 = the step gap), so any value is exact. The default is 0, following `font.size.*`. `fillValue` and the token count are unchanged, and `kNonNegativePaths` grows to 2. `setHeadBar` is kept, with a no-op on Qt's `TableRowImpl`. A 4 px test proves that the token can shrink the head (N5). |
| **S14** one value does not fit every head | S6 re-measures all five heads within ±3 px, records receipt and samples as known deltas, and checks the joined head's seam, with a Qt test for it (N5, S6). |
| Lane hand-off 1 | N-F runs `lims_gallery_tests` after N1 and after N5, and hands any failure to N-S. |
| Lane hand-off 2 | The full list of test files N-F touches is in the lanes, and its final run covers both conformance runners and `lims_gallery_tests`. |
