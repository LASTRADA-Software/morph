# LIMS gallery batch J: bleeds that reach, pages that fill, overflow that speaks, and the controls' insets

Batch J closes the framework gaps the comparison after batch I found (two reports over the ten screens at 1000×830
and 390×844, against the mockups and the batch-H grabs). Most of that comparison's regressions come from the 785 px
page that batch I's sidebar leaves at 1000 px. Re-fitting the screens to it is lane J-F's work, and is not designed
here. This design takes the eleven items that need ui/1 itself to change, or that had to be settled before a screen
can rely on them.

The aim is still the review's: ui/1 produces each mockup's look directly, and a screen never works around the
framework. The rules of batches G to I hold:

- a look is a token or a rule, never a screen's choice of pixels;
- a key means the same thing on every kind that takes it;
- a look that changes every application's shell is the theme's to switch;
- the terminal keeps its cells. A purely visual change says what it leaves unchanged there.

The user's standing preference adds one rule for this batch: **vocal over silent**. Where the framework cannot do
what a document asks, it says so. It does not quietly cut the result off.

Every claim was checked against `d931a219f` (`lg-fold-i`), with `build/linux-everything` rebuilt at that revision.
Each item says whether its premise was **reproduced** (run or measured here) or **read** (inferred from the code).
The comparison's grabs are in `lg-cmp/app-i`, taken from `c3f9ce751`; between that commit and `d931a219f` only
clang-tidy clean-ups landed. New grabs and probes are in the session scratchpad (`lg-jd-grabs/`, `lg-jd-probes/`).
Neither is in the tree.

## Probes

Two kinds of probe ran.

- **`qml6` (Qt 6.12.0, offscreen)** loaded stock Qt Quick types, or a copy of `src/qt_quick/qml` registered as a
  plain `MorphUi` module.
- **Scratch Catch2 programs** were compiled with the flags of `morph_qt_quick_tests` and `lims_gallery_tests` from
  `compile_commands.json`, and linked against the libraries of the rebuilt tree. They drive:
  - the real Qt Quick backend, directly and through the interpreter mounting a JSON document;
  - the gallery's own harness on `RecordingBackend`.

None of P1 to P7 touches the tree. P8 needed the frontend itself instrumented: it patched two files of the worktree,
rebuilt, measured, and restored them (below).

**P1. A row that cannot shrink, in stock Qt and in morph.** In stock Qt Quick Layouts (`lg-jd-probe1.qml`), a
`ColumnLayout` 300 px wide holds a `RowLayout` of two `Text`s and two 150 px rectangles, none of them filling, plus a
sibling that fills:

```
qml: PROBE col 300 row w 609 row implW 609 rowMin 609 a 0 147 146.78125 b 152 147 r 304 150 p 459 150 fill 609
```

There, a layout's minimum is its children's, so the row's 609 px reaches the column, which lays both children out at
609 px.

**Morph does not behave like that**, as the review's probe showed (`lg-jrev/rev1.qml`, the copied `MorphUi` module):
a `MorphStack` column 300 px wide holding a content-sized row (texts and a fixed spacer) and a sibling text gives

```
PROBE col w 300 colLayout w 300 colLayoutMin 0 | row w 300 rowLayout w 622 rowLayoutMin 622 row implW 622 packed true | sib w 300
```

- A column's children fill its width (`crossFill`, `Layout.fillWidth`), and a `MorphStack` is an `Item` whose own
  `Layout.minimumWidth` is unset, so the row adds 0 to the column's minimum. The row's box is 300 px and its sibling
  stays 300 px.
- Inside the row, a packed row lays its `GridLayout` out at its implicit width, 622 px, past its own box. Whatever
  clips first cuts that off: a card, a splitter pane, the page.
- So the overflow stays inside the stack that has it. Nothing above it can see it, and nothing reports it.

**P2. A field given text from outside** (`lg-jd-probe2.qml`). Three stock `TextField`s, 80 px wide, are each assigned
a 37-character text from script:

```
qml: PROBE plain start x -116.140625 cursor 37 | autoScroll:focus start x 0 | readOnly start x -116.140625 37
```

The cursor goes to the end and the field scrolls to show it. Read-only changes nothing. With `autoScroll` bound to
`activeFocus`, the text starts at x = 0.

**P3. The Basic style's text area** (`lg-jd-probe3.qml`), with `padding: 12`:

```
qml: PROBE textarea padding 12 left 16 right 12 top 12 | textfield left 16
```

The Basic style's `TextArea` and `TextField` set `leftPadding: padding + 4`. `MorphTextInput` sets `leftPadding`
itself; `MorphTextArea` sets only `padding`, so its text starts 4 px further in than an input's.

**P4. A bleeding table in a panel, on the backend** (`lg_jd_bleed.cpp`). A panel with `padding 2` and a title
holds a table with `bleed` and a text, inside a row with `align: start`, as receipt's is:

```
PROBE table x 1 w 274 bleed 1 inset 8 parentInset 8 | text x 1 w 274 inset 8 | panel w 276.188
PROBE headerbg x 1 w 274
```

On the backend, the table bleeds, and its header band starts at the frame's inner edge.

**P5. The same, through a document** (`lg_jd_doc.cpp`). The same panel, written as a ui/1 document and mounted by the
interpreter on the Qt Quick backend:

```
PROBE MorphPanel id= obj=session-panel x=0 w=276.188 bleed=0 inset=0 fillW=0
PROBE     MorphStack id= obj= x=9 w=258 bleed=0 inset=8 fillW=1
PROBE       MorphTable id= obj=session x=9 w=258 bleed=1 inset=0 fillW=1
PROBE            MorphTableHeaderBackground id= obj= x=9 w=258 ...
PROBE       MorphText id= obj= x=9 w=258 bleed=0 inset=0 fillW=1
```

Between the panel and its children sits a `MorphStack` that the document never wrote. It is the column the
interpreter makes to hold a panel's two children. The table's parent is that stack, whose padding is 0, so `bleed`
reaches past nothing.

**P6. A tab page in a tabs node that stretches** (`lg_jd_doc.cpp`, second case). The window is 700×400. A divided
column holds a text, then a `tabs` node (`inset 4`, `pageInset 0`, strip on `bar`, stretching both ways), then a
second text. The Results page is one column padded by 4:

```
PROBE   MorphTabs id= obj=tabs x=0 w=700 ... y=18 h=364
PROBE      MorphSlot id= obj= x=0 w=700 ... y=218 h=74
```

The page's slot is 74 px tall, its content's height. It sits 200 px down a 364 px tabs node: in the middle of the
height that is left under the strip, not under the strip.

**P7. The principal**, from a grab with `--user "Dr. A. Meyer"` (`lg-jd-grabs/console-user.png`). The name closes
the console's top line at 1000 px, after "Connected".

**P8. Every example app's containers, measured** (the review's B1, point 2). To learn where a container's content
minimum would change a layout if it became the container's own minimum, `MorphStack.qml` was given a temporary check
that wrote a Qt warning whenever its content's minimum (`layout.Layout.minimumWidth`, or a wrapping row's widest
basis) exceeded its width inside its padding, and `frontend.cpp` a temporary window size from the environment. The
six example render smokes were rebuilt and run at 1024×768 (their default) and 390×844. Both files were then
restored from their copies and touched, so the next build drops the probe. The counts are in J3; the raw logs are
`lg-jd-measure/`.


## The items

| Item | Verdict | Where it lands | Lane |
|---|---|---|---|
| J1 `bleed` inside a panel | **Reproduced** (P4, P5, a new grab). A library defect: the column the interpreter inserts hides the panel's padding. A tab page's slot does too | a holder column passes its parent's inset on; a tab page pads itself (J11) | J-A |
| J2 the principal in the head | **Reproduced** (P7). The feature works; the grabs had no principal, since no `--user` was passed | the grab command; no framework change | J-B |
| J3 silent clipping | **Reproduced** (P1, and the review's probe) for rows and columns; **measured** across the six example apps (P8). **Read** for tables: their sideways scroll exists, but its bar is hidden at rest | a container's minimum is its content's; a report once per container; a page that scrolls sideways; a table bar that shows | J-A |
| J4 a field shows the end of its text | **Reproduced** (P2, on stock Qt) | a field that is not focused shows its text from the start | J-C |
| J5 the inline lead wraps after the trail | **Read**: the top line is one flow in order, so the last item that does not fit is the one that wraps | the inline lead wraps inside the title's block | J-B |
| J6 a white compact page | **Read** from the mockups' CSS: seven of the nine compact windows set `background: #fff` | the screen's `pageSurface` | J-B |
| J7 the dialog's and sheet's rule spacing | **Read**: the rules are items of a column whose spacing is 5 px | a step-3 margin on each side of each rule | J-C |
| J8 a text area's inner padding | **Reproduced** (P3) | the text area sets its own sides; the token `input.paddingX` | J-C |
| J9 the sidebar's section overlines | **Read**: the navigator's root stacks its headings and its entries with no gap | each group is a column; the groups and the search are step 3 apart | J-B |
| J10 navigator selection | **Read**: the badge is the item's sibling, not part of it | the badge goes inside the item; `list.selection`, resolved per surface | J-B (badge), J-C (look) |
| J11 review 390's gaps; a scroll in a tab page | **Reproduced** (P6). It is one cause: a tab page's slot does not fill the page area | a tab page fills the page area, pads itself and scrolls itself | J-A |

## J1: `bleed` reaches past a panel's and a tab's padding

**Problem (reproduced).** The new receipt grab at 1000 px (`lg-jd-grabs/receipt.png`, row y = 100) shows:

- the card's border at x = 707, and the frame's inside starting at 708;
- the header band starting at x = 716, 8 px in, while the title starts at the same x.

So `bleed` has no effect there, as the comparison found. Two probes split the path:

- **On the backend it works** (P4). That is what batch I's tests exercise: they create a text with `bleed` directly
  under a `createPanel(...)` or a `createTabs(...)`.
- **Through a document it does not** (P5). `ScreenCore::lowerAll`, which lowers a panel's `children`, a tab's
  `children` and a scroll's `children`, wraps two or more nodes in a `Column` (`listColumn`). That column has no
  padding of its own.
  - A bleeding child's direct parent in the view tree is therefore the column, and the column reaches past nothing.
  - The loader's rule, "`bleed` is a child's of a column, a panel or a tab", checks the document's parent, which is
    the panel. The view tree has another parent.
  - A panel with a single child has no wrapper, so a lone child bleeds. Receipt's card holds a table *and* a
    footnote.
- **A tab page is wrapped twice** (read). `Mounter::mountPage` puts each page into a `SlotWidget`. `MorphSlot`
  declares no `childBleedInset`, so a child of a tab page reads 0 whatever `pageInset` says, even when it is the
  page's only child.
  - Review's compact flagged rows bleed only because their direct parent is the page's own padded column, which the
    screen writes itself.
  - Batch I's tabs test creates the bleeding text directly in `createTabs(...)`, so it never meets the slot.

This is a defect in shipped library behaviour (`include/morph/ui/interpreter.hpp`, `src/qt_quick/qml`, `src/tui`),
not a gallery bug.

**Decision.** A container that the mount inserts, and that the document did not write, is transparent to `bleed`: the
parent a document names is the parent whose padding a bleed reaches past, as the batch I spec already says. The
direct-parent rule stands for containers the author wrote: a column with no padding still gives its children nothing
to bleed past.

Two containers are inserted by the mount.

- **The holder column.** `Column` gains `bool holder = false`. `lowerAll` sets it on every column it makes, since
  every caller of `lowerAll` (a panel's, a tab's, a scroll's, a switch case's and a table cell's children) wants the
  same, and only the first two can hold a bleeding child. A `forEach`'s own column, and a column the document wrote,
  are not holders.
- **The tab page.** J11 moves the pages' padding into the page's slot, so a tab page's slot is itself the padded
  parent of the page's children, and passes nothing through. A switch case's slot cannot hold a bleeding child (the
  loader refuses `bleed` under a `switch`), so a slot needs no pass-through.
- **The backend.** `StackWidget::setPassesBleed(bool)` is a new pure virtual. The mount calls it once, only with
  `true`, for a holder column. Every `StackWidget` implements it: `StackImpl` and `TableRowImpl` on Qt Quick
  (`src/qt_quick/kinds.hpp`), their terminal counterparts, and `RecordingBackend`. A table row ignores it; it is never
  a holder.

**Per backend.** The two frontends reach the same result by different means, because one draws children outside
their parent's bounds and the other cannot.

- **Qt Quick** forwards the inset. A layout honours negative margins and does not clip, so a child can lie past its
  holder's edges.
  - `MorphStack`'s layout: `childBleedInset: root.insetX + (root.passesBleed ? root.bleedInset : 0)`, with a new
    `property bool passesBleed: false` that `StackImpl::setPassesBleed` writes.
  - Both are declared as lane W's `bleedInsetOf` reads them (through a `var`), so Qt 6.8.1's qmllint accepts them.
  - A `MorphWidget`'s own `bleedInset` binding is unchanged.
- **The terminal** moves the padding down. core-cpp clips every child to its parent's area (`Screen.cpp`,
  `screenBounds.intersect(parentBounds)`), so a holder that handed its child a span wider than itself would see the
  child cut back to its own edges. So:
  - a panel lays a holder out over the frame's inner span, past its padding, and hands it that padding;
  - the holder places its children inside that padding, except a bleeding child, which gets the holder's full width;
  - a tab page's slot is laid over the whole page area and pads its own content (J11), so it is the padded parent
    there.
  - `bleedAreas` (`src/tui/widget.cpp`) is the one place that applies "full width for a bleeding child", for the
    panel, the stack, the slot and the holder alike. `PanelImpl`'s placement (`container_widgets.cpp`) and
    `TabsImpl::childAreas` (`list_widgets.cpp`) hand down the spans.
- **`RecordingBackend`** records `passesBleed` on the stack's line.

**Spec.**

- `view_tree.md` (`Column::holder`);
- `interpreter.md` (`lowerAll`'s holder column);
- `backend_contract.md` ("Layout hints": the parent a bleed reaches past; `StackWidget::setPassesBleed`);
- `document.md` ("Every node", `bleed`: one sentence saying that the parent is the one the document names);
- `qt_quick/backend.md` and `tui/frontend.md`, the Layout rows.

**Tests.**

- **Conformance, through a document** (`testing/backend_conformance.hpp`), not through the backend alone. That is the
  gap that let this ship.
  - "a bleeding child of a panel with two children reaches the frame's inner edges": a `panel` with padding 2 holding
    a table with `bleed` and a text. The table's left is the frame's inner left and its width the frame's inner
    width; the text is one step-2 inset in.
  - "a bleeding child of a tab page reaches the tabs node's edges": a `tabs` node with `inset 4` and `pageInset 4`,
    one tab holding two texts, the first with `bleed`. The first's left is the tabs node's left; the second's is
    `space.4` in.
  - "a bleeding child of a tab page that scrolls still reaches the edges": the same, with the page's content taller
    than the page. The first text's left is the tabs node's left, it is not clipped, and the page's bar sits at the
    node's right edge (J11).
  - **Control:** a backend whose holder stack passes nothing on, and today's tab page padding. The first two cases
    fail; without the fix, the first measures the same 9 px as P5.
  - The cases run on both frontends, so the terminal's clip is in the loop.
- the interpreter's tests (`tests/test_ui_interpreter_detail.cpp`):
  - a panel with two children lowers to a `Column` with `holder` set;
  - a panel with one child lowers to that child, unwrapped;
  - **Control:** drop the flag from `lowerAll`.
- `tests/test_ui_recording_backend.cpp`: a holder column's line carries `passesBleed`.
- `tests/qt_quick/test_qt_quick_containers.cpp` and `tests/tui/test_tui_containers.cpp`: the backend-level cases from
  batch I stay, each joined by a case through a holder stack.
- **The gallery:** `test_receipt.cpp` checks that the expanded session table's layout records `bleed` and that its
  holder records `passesBleed`. That proves the document side only; the conformance case proves the drawing.

**Verify.** Re-grab receipt at 1000 px. The session table's header band and its row rules run from the frame's inner
left edge to its inner right edge. "This session" and the footnote stay 8 px in.

## J2: the principal in the head

**Problem (reproduced).**

- `ShellFrame::setPrincipal` shows `shell_head_principal` only while the name is not empty
  (`_topPrincipal->setVisible(_principalInHead && !_compact && !_principalName.empty())`).
- The client passes `_source->principal()`. For a local application that is `LocalSetup::user`, the `--user`
  option. The grab commands in `design/visual-review.md` pass no `--user`, so the session's principal is empty.
- Grabbed again with `--user "Dr. A. Meyer"` (P7), the console's top line ends with "Connected" and then "Dr. A.
  Meyer". The feature works.

**Decision.** No framework change.

- **The grabs.** The gallery's grab command in `design/visual-review.md` becomes
  `morph_examples_client --app lims-gallery --user "Dr. A. Meyer" --screen lims-gallery.<id> --grab-size WxH --grab FILE`,
  since every mockup draws a signed-in shell.
- **A signed-in shell with no name** draws no name. That is right, and it is not a misconfiguration: a development
  session without `--user` has no principal to show. So it is not reported either.
- **Not chosen:** a default development principal on `LocalApp`. It would put a framework seam in place for one
  gallery's screenshots, and a real deployment signs in.

**Tests.** None new. `tests/client/test_shell.cpp` already shows the principal in the top bar for
`principalPlacement: "head"`.

**Verify.** Re-grab every screen at 1000 px with the new command. Each expanded head ends with "Dr. A. Meyer". At
390 px no name shows, as `10` compact draws none.

## J3: content that does not fit is reported, reachable, and visibly scrollable

**Problem (rows and columns reproduced; tables read).**

- **A row's content can be wider than the row, and that stays inside the row** (P1). A content-sized child of a row
  does not fill, so its preferred width is its minimum, and a row's `GridLayout` cannot be narrower than its
  children's sum. But a `MorphStack` is an `Item` whose own `Layout.minimumWidth` is unset, and a column's children
  fill its width, so the row's box is given whatever its column has. Its layout then runs past the box, and the
  nearest clip cuts it off: a card, a splitter pane, the page.
  - **Audit's 1000 px trail.** The chip row is about 640 px wide and does not wrap, in a pane about 530 px wide.
  - **Review's footer.** The row holds "Approving needs your signature", a stretching spacer and three buttons; it
    is not packed, so its layout is its box's width, and its minimum is wider. "Approve and sign…" is cut off.
- **A table wider than its columns allow scrolls sideways, invisibly.** `MorphTable` puts the rows in `hflick`,
  which is interactive while `columnsWidth > width`. Its bar is a `MorphScrollBar` declared inline
  (`MorphTable.qml`, `ScrollBar.horizontal: MorphScrollBar {…}`), whose Basic-style `policy` is `AsNeeded`: it shows
  only while active. A table at rest looks cut off: stability's Pulls column, console's Test and Status. The P7 grab
  shows console's worklist this way, its Status column ending mid-word at the pane's edge.
  - The audit trail's Reason column is this case too: the trail table's own sideways scroll, not the page.
- **Nothing reports either case.** The comparison found them only by looking at the grabs.

**Decision.** The user chose both a report and a page that scrolls sideways (Q1). Four parts:

1. a stack's minimum is its content's, so an overflow is seen by its ancestors;
2. a report, once per container, so the author hears it;
3. a page that scrolls sideways, so the user can reach the content;
4. a table bar that shows, so a sideways scroll can be seen.

A container still does not shrink its children on its own. Whether text elides or a row wraps is the document's
choice (`wrap`, `minWidth`, stretch), and the report names where that choice is missing.

### A container's minimum is its content's

`MorphWidget` gains `property real contentMinimumWidth: -1`, which a container binds to its padding plus its content
layout's minimum, and `Layout.minimumWidth` becomes

```
minWidthUnits >= 0 ? minWidthUnits * Theme.unitWidth : honoursContentMinimum ? contentMinimumWidth : -1
```

with `property bool honoursContentMinimum: true`, which a table row turns off for its cells (below).

- `MorphStack`: `2 * insetX + layout.Layout.minimumWidth`; for a wrapping row, `2 * insetX` plus its widest child's
  basis, since a wrapping row can be as narrow as its widest child.
- `MorphPanel`: `2 * Theme.spaceOf(padding) + 2 + inner.Layout.minimumWidth`.
- `MorphTabs`: `2 * pageInsetPx + pages.Layout.minimumWidth`.
- A table, a scroll, a slot and every leaf keep `-1`: a table and a scroll take their own overflow, and a slot is a
  page.
- A document's `minWidth` still wins, as it does today.

So a row of content-sized children that cannot fit raises its column's minimum, and so on up to the page, where the
page's slot sees it.

**What it changes, measured (P8).** It changes a layout only where a container is given less than its content's
minimum today: there, the container now keeps its minimum and its stretching siblings give way, up to the page,
which then scrolls. Where every container already fits, a minimum that is never binding changes nothing. P8 counted
the containers given less than their content's minimum on every screen of the six example applications:

| Smoke | At 1024×768 | At 390×844 |
|---|---|---|
| `bank_render_smoke` | 0 | 0 |
| `forms_render_smoke` | 0 | 1: a packed row in a table cell (`cell_sampleId`), 200 px of content in a 170 px cell |
| `gallery_render_smoke` | 0 | 0 |
| `ladder_lims_render_smoke` | 0 | 1: a packed row on a page, 549 px of content on a 358 px page |
| `ladder_bookmarks_render_smoke` | 0 | 3: a row on `bookmarksPage`, 427 px on 358, and two rows on pages, 362 px on 358 |
| `lims_gallery_render_smoke` (its own widths, 462 and 1382 px) | 7 | 7 |

The seven in the LIMS gallery's own smoke are 1 px apart once rounded (825 on 824, 431 on 430, 199 on 198): layouts
round each child's width, and a fixed width in units is fractional. They are not overflows anybody sees.

So:

- **At the size every other example smoke runs at, nothing changes.** No container in bank, forms, the gallery, the
  lims ladder or bookmarks is given less than its content today, so a minimum that is never binding changes no
  layout, and no smoke sees a report. That last step (a non-binding minimum changes nothing) is inferred from how a
  layout distributes width, not measured.
- **At a phone's width, four rows in two ladder apps change** from cut off to a page that scrolls sideways and a
  report. That is the behaviour the user chose (Q1). Those are example screens, which this repository does not
  track (`AGENTS.md`); the reports will name them for whoever next re-fits them.
- **A table cell is the one place propagation would do harm.** A table places each cell at its column's width; a
  cell whose minimum rose past it would push its row out of line with the header. So a table row's cells do not take
  their content's minimum: `TableRowImpl` offers each cell `honoursContentMinimum: false`, and `MorphWidget` uses
  `contentMinimumWidth` only while that is true. forms' cell stays cut at its column, as today, and is reported by its
  own stack.
- **Rounding.** The check and the page's sideways scroll ignore a shortfall under 2 px, which covers the gallery's
  seven.

So the propagation lands with those two exceptions, and needs no decision from the user beyond Q1.

### The overflow report

- **What is checked.** A stack compares its content's minimum (`layout.Layout.minimumWidth`, or for a wrapping row
  its widest child's basis) with the width inside its padding, `width - 2 * insetX`, not with `layout.width`: a
  packed row lays its layout out at its implicit width, which always equals its minimum. A panel compares
  `inner.Layout.minimumWidth` with `inner.width`. Content wider by 2 px or more overflows (P8's rounding).
- **No splitter check.** A splitter's panes are plain widgets with no layout of their own (`MorphSplitter.qml`); the
  only width to compare would be the implicit one, which reports falsely. Each pane's stack checks itself.
- **When it checks.** Only while the container is visible, its width is above 0 and it has a window. That leaves out
  start-up before the window has its size, hidden tab pages, folded sections and closed overlays. The check runs
  after the layout settles (`Qt.callLater`) on a change of the width, the minimum or the visibility.
- **How often it reports.** Once per container while it stays mounted: the first time it overflows. A container that
  fits again and overflows again later, or overflows at another width while the user drags a splitter, does not
  report again. The pixel values are in the text but not in what identifies the report, so nothing grows with the
  number of widths seen.
- **The channel.** Not `Context::report`: its `ComponentFailure` means a component could not be loaded, and it
  de-duplicates on the whole text. The report is a Qt warning in its own logging category, `morph.layout`, written
  with `qCWarning` from the container's C++ side (`ContainerItemWidget` gains the `overflow` relay once, for every
  container). A Qt warning is what every render smoke already records and fails on, and an application can filter the
  category with `QT_LOGGING_RULES`.
- **The text.** `` row `review-footer` needs 512 px and has 430 px: `approve-hint` and its siblings neither wrap nor
  shrink ``. A container with no test id is named by its kind and the nearest ancestor that has one (`` a column
  inside `trail-pane` ``). The child named is the widest one whose minimum exceeds the width, when there is one.
- **A table** reports too, once, the first time its columns overflow: `` table `conditions` scrolls sideways: its
  columns need 612 px and it has 490 px ``. Its content is reachable, so this is information rather than a fault, but
  a screen's fixed columns rarely mean it.

### The page scrolls sideways

A scrollable `MorphSlot` (a workspace page, a tab page) and a vertical `MorphScroll` lay their content column out at
`Math.max(viewport width, the column's minimum width)`, which the propagated minimums make the content's real need.
J11 gives `MorphSlot`'s full set of bindings for both axes; `MorphScroll` takes the same horizontal part. While the
content is wider than the viewport, the flickable also scrolls sideways and shows a horizontal bar made from the
`scrollBar` part with `policy: ScrollBar.AlwaysOn`. Content that fits is laid out at the viewport's width, exactly as
today, and a page that stretches down keeps stretching.

### The table's bar shows

- `MorphTable` makes its horizontal bar from the `scrollBar` part, as `MorphSlot` and `MorphScroll` do (C++ passes
  `bar` at creation, `src/qt_quick/table.cpp`), in place of the inline `MorphScrollBar`. Today no restyle reaches the
  table's bar; after this one does.
- The table sets `policy: hflick.interactive ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded` on the bar it makes.
- A bar that is always on would cover the last row, so while `hflick.interactive` the table's `implicitHeight` grows
  by the bar's height, and the rows end above it.

**Per backend.**

- **Qt Quick:** as above.
- **Terminal:** this batch neither reports overflow there nor scrolls a page sideways, and content keeps being cut at
  the column. The terminal has no diagnostics channel, and writing to stderr would draw over the screen. That is
  silent, against the vocal-over-silent rule, so it is listed explicitly under **Not in this batch** with the seam it
  needs.

**Spec.**

- `backend_contract.md` ("Layout": a container's minimum is its content's; what a backend does when content does not
  fit);
- `qt_quick/backend.md` (the minimum; the report, its category, text and timing; the page's sideways scroll; the
  table's bar from the part);
- `workspace.md` (a page scrolls sideways when its content cannot be narrower);
- `tui/frontend.md` (the terminal cuts, unreported).

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - **The minimum rises.** A column 300 px wide holding a content-sized row of two texts and a fixed 40-unit spacer:
    the column's `Layout.minimumWidth` is the row's content plus padding.
    - **Control:** today's `MorphWidget`, where it is unset (the review's probe: `colLayoutMin 0`).
  - **Row overflow reported.** The same column as the content of a 300 px window. Exactly one `morph.layout` warning,
    naming the row; resize to 299 px, and none more; widen until it fits and narrow again, and none more.
    - **Control:** a column that fits reports nothing; a packed row compared against `layout.width` reports nothing,
      which is the mutation that must fail this case.
  - **A hidden page does not report.** The same row in a tab that is not shown: no warning until it is shown.
  - **Page scroll.** A scrollable slot 300 px wide holding the same row. Its flickable's `contentWidth` is the row's
    minimum width plus the padding, and the horizontal bar is visible.
    - **Control:** a row that fits. `contentWidth` equals the viewport and no bar shows.
- `tests/qt_quick/test_qt_quick_table.cpp`: **Table bar.** A table 200 px wide whose fixed columns need 400 px. The
  horizontal bar is visible at rest, it is the part's instance (a restyled `scrollBar` part's object name), the last
  row ends above it, and one warning names the table.
  - **Control:** the same table 500 px wide. No bar and no warning.
  - **A cell keeps its column.** A table whose cell holds a packed row wider than its fixed column: the cell's width
    is the column's, and the header and the row stay in line.
    - **Control:** a table row that leaves `honoursContentMinimum` on. The cell is wider than its header.
- **The smokes.** Every render smoke records Qt warnings, so the report reaches all six example smokes with no change
  to them, at their window sizes. `examples/common/testkit/render_smoke.hpp` and the gallery's smoke let the event
  loop run after each resize until the posted checks have run (a `QCoreApplication::processEvents` loop over a short
  settle, as the gallery's smoke does for the width class), so a report is seen before the next screen mounts.
  - `lims_gallery_render_smoke` renders the ten screens at its two widths and at 1000×830 and 390×844, and fails on
    any report. It is the check that turns J-F's re-fits into a guarantee.
  - **Control:** revert one of J-F's fixes (audit's chip row without `wrap`), and the smoke fails naming it.
  - It is the one J3 commit that waits for J-F, since before J-F it fails by design.

**Verify.**

- Re-grab audit, review, stability and console at 1000 px **before** J-F's fixes reach the lane's branch. Each logs
  its overflow, audit's page shows a horizontal bar, and stability's and console's tables show theirs.
- After J-F, no screen logs one.

## J4: a field that is not focused shows its text from the start

**Problem (reproduced on stock Qt, P2; the comparison's grab agrees).**

- Receipt at 1000 px draws Sample type as "ease, finished product" and Received at as "4-Oct-2026 09:41".
- `TextInputImpl::setText` and `DateTimeImpl::show` assign the control's `text`. That moves the cursor to the end,
  and a field narrower than its text scrolls to keep the cursor in view, read-only or not.

**Decision.** A single-line field shows its text from the start while it does not have the focus, and scrolls to its
cursor while it does: `autoScroll: activeFocus` on the `TextField`. That covers `MorphTextInput` and
`MorphDateTimeInput`. A select draws its value as a label and is unaffected.

- Focus from a click puts the cursor where the user clicked, which then scrolls as usual.
- Focus from Tab selects as the style does.
- Leaving the field returns it to the start.

**Per backend.** Qt Quick. The terminal's input is core-cpp's `InputField` (`TextInputImpl::setText` hands it the
text), and how it scrolls a programmatic text was not checked. The lane adds the terminal case below. If that case
shows the end, the fix belongs to core-cpp, and the lane reports it there rather than working around it here.

**Spec.** `qt_quick/backend.md`, the text input row.

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`: a text input 80 px wide whose text is set to 37 characters.
`positionToRectangle(0).x` is the left padding. After `forceActiveFocus()` and `End`, it is negative, and after the
focus leaves it is the left padding again.

- **Control:** the stock `autoScroll`. The first check fails at about −116 px, as in P2.
- The same case on a date-time input.
- `tests/tui/test_tui_fields.cpp`: an input 10 columns wide
  given a 37-character text draws its first 10 characters.

**Verify.** Re-grab receipt at 1000 px. Sample type and Received at begin with their first character, cut at the
end.

## J5: the inline lead wraps under the title before the trail wraps

**Problem (read).**

- `ShellFrame::_topMain` is one wrapping row, in this order:
  - the menu;
  - the lead block (`_eyebrow`, `_title`, `_headLead`);
  - `_headInline`, the inline lead;
  - `_headSlot`, the trail.
- `MorphStack.place()` fills lines in order, and a child that does not fit after the ones already on a line starts
  the next. So when the title, the lead and the trail together are too wide, the trail is the one that wraps.
  Plate at 1000 px does this: template, Auto-fill and Save layout drop to a second line, and the head grows from 58
  to 84 px.
- `document.md` says the inline lead "wraps to a line of its own, before the trail does". A flow in this order
  cannot do that.

**Decision.** The inline lead wraps inside the lead block, under the title, and the block keeps its 32-unit minimum
whatever the placement.

- **The title line.** `_leadBlock` holds `_eyebrow`, then `_titleLine`, a wrapping row (made by `wrappingRow`, gap
  step 2, `align: center`, test id `shell_title_line`) holding `_title` and `_headInline`, then `_headLead`.
  `_headInline` leaves `_topMain`.
- **`setLeadInline(bool)`** shows or hides `_headInline` and changes no sizing.
  - `_leadBlock` always stretches from `kShellHeadLeadMinimum`, as it does today for `below`.
  - `_headInline` is always content-sized.
- **The result.** `_topMain` wraps the trail only when the lead block's 32-unit minimum and the trail together do
  not fit. Above that, the block takes what the trail leaves. Inside it, the title line holds the title and the lead
  while both fit, and puts the lead on its own line under the title when they do not.
- **Where the spare width goes.** The title line packs at its start, so the spare width lies after the lead and
  before the trail. That is batch I's look for a lead that fits.
- **The client** is unchanged: it mounts into `headInlineSlot()`, which still returns `_headInline`.
- G-b's one-line test is unaffected. A head is one line while it has no eyebrow and no lead below it, and an inline
  lead that wrapped makes the head taller without making it "below".

**Per backend.** Both frontends lay the frame out from these stacks. On the terminal a title and an inline lead
wider than the block wrap at a cell, and the trail keeps the title's row.

**Spec.** `shell_frame.md`: the children of the lead block, `headInlineSlot`, `setLeadInline`, and the Shape table's
top line. `document.md`'s `leadPlacement: inline` sentence stays; it is now true.

**Tests.**

- `tests/test_ui_shell_frame.cpp`:
  - `_leadBlock`'s children are `eyebrow`, `shell_title_line`, `shell_head_lead`, in that order;
  - `shell_title_line` holds `shell_title` and `shell_head_inline` and is recorded `wrap`;
  - `setLeadInline(true)` changes no layout string, only the visibility.
  - **Control:** the old tree, and the first check fails.
- `tests/qt_quick/test_qt_quick_containers.cpp`: an expanded `ShellFrame` sized so that the title plus 32 units plus
  the trail fit, but the title plus its inline lead plus the trail do not. The trail's top equals the title's, and
  the inline lead's top is below the title's.
  - **Control:** the old tree. The trail's top is below the title's.
- `tests/tui/test_tui_shell_frame.cpp`: the same at 80 columns. The trail is drawn on the title's row.

**Verify.** Re-grab plate at 1000 px. Template, Auto-fill and Save layout stay on the title's line. The subtitle
follows the title when it fits, and otherwise sits under it.

## J6: a page drawn on a white surface

**Problem (read from the mockups).** Each mockup's compact window is a `div` with its own style:

| Mockup | Compact window background |
|---|---|
| `02`, `03`, `04`, `06`, `07`, `08`, `09` | `background: #fff` |
| `05`, `10` | none, so the grey body shows; their cards sit on it |

The shell draws `_page` on the built-in `page` surface (grey `color.background`), whatever the screen.

**Decision.** A screen member, as G-g's `headSurface` is, not a theme switch. The mockups disagree per screen: samples
and console stay grey.

- **`pageSurface`, a screen member:** an expression of a surface's name. The shell draws the page area in that
  surface while the screen is shown.
  - `null` or absent is `page`.
  - A name that is no surface draws as `page` does, as a node's `surface` does.
  - The app document refuses it, as it refuses every head member.
- **Following it**, exactly as `headSurface` is followed:
  - `ScreenHead::pageSurface()` is a virtual returning `reactive::Computed<std::string> const*`, default `nullptr`.
  - `Screen::pageSurface()` is a tracked read.
  - `Workspace::activePageSurface()` is a tracked read that is empty with no active tab.
  - `ShellFrame::setPageSurface(std::string_view)` sets `_page`'s surface (`page` for empty). The client calls it
    from the `_headLayout` effect.
  - Author: `Scope::pageSurface(AnyHandle)`.
- **The gallery declares the surface it needs**, `plainPage`: `color.surface` `#FFFFFF` and nothing else, so the
  other roles follow the theme's. This is as it declares `darkHead`; no built-in surface is added.
- **Seven screens** ask for it on compact only: `pageSurface: cond(compact, "plainPage", null)` on receipt,
  instrument, audit, stability, plate, results and review.

**Per backend.** The terminal reads no surface colour for the page and is unchanged.

**Spec.**

- `document.md` ("The document": `pageSurface`);
- `host.md` (`ScreenHead::pageSurface`);
- `interpreter.md` (`Screen::pageSurface`);
- `workspace.md` (`activePageSurface`);
- `shell_frame.md` (`setPageSurface`);
- `author.md`;
- `client/client.md`.

**Tests.**

- Corpus: `pageSurface` loads on a screen and is refused on the app document.
- `tests/test_ui_workspace.cpp`: `activePageSurface()` follows a width change that switches the computed name.
- `tests/test_ui_shell_frame.cpp`: `setPageSurface("plainPage")` records `shell_page` with `surface=plainPage`, and
  `""` with `surface=page`.
- `tests/client/test_shell.cpp`: a screen with `pageSurface` sets the page's surface, and switching to a screen
  without it restores `page`.
- **Control:** a client that never calls `setPageSurface`.

**Verify.** Re-grab the seven screens at 390 px: the area around the content is white. Samples and console stay grey.

**Residual.** A panel on a page that is not a card draws its shadow. The mockups' compact cards on white draw a
border and no visible shadow. That is the theme's `elevation.card` to tune, and it is left as it is.

## J7: space around a dialog's and a sheet's rules

**Problem (read).**

- `MorphDialog`'s content is one `ColumnLayout` (`contentColumn`) with the layout's default spacing of 5 px. The head
  rule and the footer rule are items of that column, so the body starts 5 px under the head rule and the actions
  6 px under the footer rule.
- The comparison measured that at both widths. `MorphDrawer` is built the same way.
- The design's sections (`Main.dc.html`):
  - the head is padded `16px 18px 12px`, so its text ends 12 px above the head rule;
  - the body is padded `14px 18px`;
  - the footer is padded `12px 18px`;
  - the sheet's body is padded `14px 16px`.

**Decision.** A rule stands off its neighbours by step 3 on each side, and the overlay's padding under a footer is
step 3.

- **The rules.** `headRule` and `footerRule` get `Layout.topMargin` and `Layout.bottomMargin` of
  `Theme.spaceOf(3) - contentColumn.spacing`, so there is exactly step 3 (12 px) between each rule and its neighbours.
  `contentColumn.spacing` stays 5, so the title, the description and the fields keep their places.
- **The bottom.** While the footer shows, the popup's `bottomPadding` is `Theme.spaceOf(3)`, the design's 12 under
  the actions. Otherwise it stays step 4.
- **The scroll cap.** The scroll area's height cap, which subtracts the rules and their spacing, subtracts the new
  margins as well, so a tall dialog still fits the window.
- **What is left.** That gives 12 px of body under the head rule against the design's 14, and 12 above the footer
  rule against 14. 14 is not a step, and the user chose step 3 for every rule gap (Q5).
- `MorphDrawer` takes the same change.

**Per backend.** The terminal draws a rule as a row of its own with no gap rows. That is unchanged.

**Spec.** `qt_quick/backend.md`, the Dialog and Drawer rows.

**Tests.** `tests/qt_quick/test_qt_quick_overlays.cpp`:

- an open dialog with a description, one field and two actions. The field's top is `headRule`'s bottom plus
  `space.3`; the first action's top is `footerRule`'s bottom plus `space.3`; the action's bottom is the popup's
  bottom less `space.3`;
- the same for a drawer;
- **Control:** today's QML. The gaps are 5 and 6 px.

**Verify.** Re-grab e-signature at both widths. The summary box sits 12 px under the head rule, and the buttons 12 px
under the footer rule, with 12 px under them at 1000 px.

## J8: a text area's inner padding, and the inputs' shared inset

**Problem (reproduced, P3).**

- The Reason text area's text starts about 16 px from its border, and the Signer input's 12 px (`esignature-1000`).
- The design pads `.input` `0 8px` and the text area `6px 8px`.
- The 4 px difference between our two controls is the Basic style's `leftPadding: padding + 4`. `MorphTextInput`
  overrides `leftPadding`; `MorphTextArea` sets only `padding`.
- The 12 px against the design's 8 is our own step 3. `MorphSelect` already insets its value by step 2, 8 px, so our
  own controls disagree with one another.

**Decision.** One token for the inner inset of every input, and a text area that sets its own sides.

| Token | Kind | Default | Read by |
|---|---|---|---|
| `input.paddingX` | dimension | 8 | `MorphTextInput`, `MorphTextArea`, `MorphDateTimeInput`, `MorphSelect` (through `Theme.inputPaddingX`) |

- **What reads it.** Each control's left and right inner padding, and the places its prefix, icon and unit are
  measured from: today `Theme.spaceOf(3)` in the text input and the date input, and in the select its value's
  `leftPadding` and `rightPadding` (`MorphSelect.qml`, lines 80–81) and the chevron's `x` (line 91), which are
  `spaceOf(2)`. The select's popup entries (lines 147–148) and its segmented options (lines 311–312) are not inputs'
  insets and keep their steps.
- **The text area's other sides.** `MorphTextArea` sets `leftPadding` and `rightPadding` from the token, and its
  `topPadding` and `bottomPadding` to `(Theme.controlHeight - line height) / 2`. Its first line then sits where a
  single-line input's text sits: about 8 px at the default 32 px control and a 16 px line, against the design's 6.
- **The default** is 8, the select's value today and the design's, so the stock controls agree. That moves every
  application's text inputs 4 px left; the user confirmed it (Q6).
- `kThemeTokens` grows by one (with J10, from 68 to 70). `src/qt_quick/theme.cpp` maps the token, and
  `include/morph/ui/theme.hpp` lists it.

**Per backend.** The terminal pads its inputs by cells and reads no dimension token. That is unchanged.

**Spec.** `theme.md` (the token), `qt_quick/backend.md` (the input rows).

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`:

- under a theme with `input.paddingX: 10`, the text input's, the text area's, the date input's and the select's text
  each start 10 px plus the 1 px border in;
- the text area's first line is within 1 px of a single-line input's text, both measured from their tops;
- **Control:** today's QML. The text area's text starts 4 px further in than the input's.
- `tests/test_ui_theme.cpp`: the token parses, and a negative value is refused.

**Verify.** Re-grab e-signature and results at 1000 px. The Reason text and the comment start 8 px from the border,
in line with Signer's.

## J9: the sidebar's sections stand apart

**Problem (read).**

- `Navigator`'s root is one vertical stack, with no gap, holding the search, the pin row, and then each group's
  heading and its entries.
- So a heading sits 4 to 9 px under whatever precedes it: the search box, or the previous group's last entry.
- The design (`10`) gives `.navh` `margin: 12px 10px 4px`: 12 px above a heading and 4 px under it.

**Decision.** Each group is a column of its own.

- **A group** is a vertical stack (`navigator_group_<module>/<group>_section`) holding its heading and then its
  entries' rows, gap step 1. The 4 px under the heading is step 1, and the entries' own heights keep them apart, as
  today.
- **The root's gap is step 3** (12 px), between the search, the pins and the groups. A hidden pin row takes no gap.
- **The search hides a group** by hiding its column, so a group with no match takes no gap either. That is what it
  hides today, the heading and the rows, plus the column.
- The test ids of the headings, the rows and the entries are unchanged.

**Per backend.** On the terminal, step 1 is 0 rows and step 3 is 1 row (`layout::stepCells`), so the groups have one
blank row between them and the headings sit directly over their entries. That is a change to the terminal's
sidebar, and a welcome one at its density.

**Spec.** `workspace.md`, "Navigator and pins".

**Tests.** `tests/test_ui_workspace_start.cpp`, where the navigator's tree is tested:

- the root records `gap=3`, and each group's section `gap=1` holding its heading first;
- a search that matches no entry of a group hides its section;
- **Control:** the old tree.

**Verify.** Re-grab any screen at 1000 px. "GALLERY" sits 12 px under the search box, and each later heading 12 px
under the entry above it.

## J10: a navigator entry is selected across its badge, and the selection's look is the surface's

**Problem (read).**

- **The badge.** Each entry is a row holding a stretching `ButtonStyle::Item` button and then its badge, `gap` 1.
  `MorphButtonBackground` draws the selection under the button only, so the highlight stops at the badge. In
  instrument at 1000 px it ends at x = 180, against 203 for an entry without a badge.
- **The look.**
  - A selected list item draws `scheme.selection`, a 3 px accent bar at its leading edge, and its label (and icon) in
    the accent.
  - The design's sidebar (`10`, `.nav.on`) is `background: #2B3038; color: #fff`: a fill, with the label in the
    surface's text colour and no bar.
  - The design's review queue (`09`, `.qi.sel`) is `background: #EAF5EE; box-shadow: inset 3px 0 0 #17784A`: a tint
    and a 3 px bar, which is our `accent` look. Review's queue (`review.cpp`) and console's compact list
    (`console.cpp`) are item-style buttons too.
  - So one application needs both looks at once, one per surface. A theme-wide switch cannot express that; a token
    that a surface may set can, as `border.style` is.

**Decision.**

- **The badge goes inside the item.** The navigator gives each entry's button content, which `ButtonWidget::content()`
  already supports for `ButtonStyle::Item`:
  - a horizontal stack (`nav_content_<screen id>`, gap step 1, `align: center`);
  - inside it, the title as a text that stretches, then the badge (`nav_badge_<screen id>`, as today).

  The button's background then spans the whole entry, the badge with it. The button keeps its label, which names it
  with its count, as it does today. The entry's row is the button alone.
- **A selected item's content follows the look.** While an item that has content is selected, its content's text in
  the `Normal` role draws in the colour the item's label would have. The mechanism is the scheme the button hands its
  content, so nothing in QML overrides what C++ passes down:
  - `MorphButton` declares `readonly property MorphScheme contentScheme`: a `MorphScheme` whose every role is bound
    to the button's `scheme`, except `text`, which is the accent while the item is selected under the `accent` look
    and the scheme's `text` otherwise. `Theme.toolbarJoinedScheme` is the precedent for a scheme bound role by role.
  - `ButtonImpl::scheme()` returns that part in place of `scheme`, so `passSchemeDown` gives the content it once, at
    adoption, and the bindings carry every later change of the selection or the look.
  - `muted`, the tones and every other role are the button's.
- **The token, per surface.**

  | Token | Values (default first) | What it switches | Read by |
  |---|---|---|---|
  | `list.selection` | `accent`, `fill` | A selected list item: `accent` draws the selection colour, a leading accent bar and the label in the accent; `fill` draws the selection colour and the label in the scheme's text colour, with no bar | `MorphButtonBackground`, `MorphButton`, through the scheme's `listSelectionFill` |

  - It is a `keyword` token, as `collapsible.marker` is. Any other value is refused at `addFile` (`list.selection:
    only accent and fill are drawn`).
  - It is resolved per surface, as `border.style` is: `MorphScheme` gains `property bool listSelectionFill`, which
    `theme.cpp` writes for the theme's scheme and for each surface's from that surface's resolved values, so a surface
    that does not set it keeps the theme's.
  - Within a surface it applies to every item-style button: the navigator, menus drawn as lists, and a document's
    list items. A key means one thing on each surface.
  - The default is `accent`, today's look, so no application changes until its theme sets it.
- **The gallery's** `sidebar` surface sets `list.selection: fill` and `color.selection: #2B3038`. The theme's base
  keeps `accent`, so review's queue and console's compact list keep the bar the mockups draw.

**Per backend.**

- **Terminal.** The item's content draws on the entry's row, as an item with content does today. The terminal
  marks a selected item by inverting it and reads no theme token, so `list.selection` does not change it.
- **Qt Quick.** As above.

**Spec.**

- `theme.md` (`list.selection`, and the Surfaces section: a surface may set it);
- `qt_quick/backend.md` (the item button's selected look; the content's scheme while selected);
- `workspace.md`, "Navigator and pins" (the entry: one button with content).

**Tests.**

- `tests/test_ui_workspace_start.cpp`, where the navigator's tree is tested: each entry is a button whose content
  holds `nav_badge_<id>`; `setBadge` shows it there.
  - **Control:** the old row.
- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - a selected item with content of a stretching text and a badge: the background's width is the button's, and the
    badge lies inside it;
  - on a surface whose `list.selection` is `fill`, the bar is hidden and the content's text is the scheme's `text`;
  - on the theme's scheme (`accent`), the bar shows and the content's text is the accent;
  - deselecting the item turns the content's text back to the scheme's `text`, with no re-adoption;
  - **Control:** a `ButtonImpl::scheme()` that returns `scheme`. The content's text stays `text` when selected under
    `accent`.
- `tests/qt_quick/test_qt_quick_theme.cpp`: a theme index whose `sidebar` surface sets `fill` gives
  `sidebarScheme.listSelectionFill` true and the theme's scheme false.
- `tests/test_ui_theme.cpp`: `list.selection: bar` is refused.

**Verify.**

- Re-grab instrument and console at 1000 px. The selected entry's fill runs to the sidebar's inner edge, past the
  badge, with a white label and no bar.
- Re-grab review at 1000 px. The queue's selected entry keeps its tint and its accent bar.

## J11: a tab page fills the tabs node, pads itself, and scrolls itself

**Problem (reproduced, P6; measured in the grab).**

- **What the grab shows** (review 390, `app-i`, read at x = 5 and x = 200):
  - the divider at y = 128;
  - grey at 129–133 (the 5 px band);
  - the white strip at 134–176;
  - the strip's rule at 177;
  - then grey until All/Flagged, about 72 px down.

  The page's content ends about 70 px above the decisions bar. The gaps above and below the content are nearly
  equal: the content is centred in the height that is left.
- **The cause.**
  - `MorphTabs` lays its pages out in `pages`, a `ColumnLayout` that fills the rest of the node.
  - Each page is the `SlotWidget` that `Mounter::mountPage` makes. `TabsImpl` takes the container default for
    `crossFill()`, which fills the width but not the height, so the slot is as tall as its content.
  - A layout cell centres an item that does not fill it, so the page sits in the middle (P6: a 74 px slot 200 px down
    a 364 px node).
  - A `ColumnLayout` with no child that fills also caps its own maximum height at its content's, so `MorphTabs`' own
    column has height left that no child takes, and spreads it around the strip as well. That is the 5 px band above
    the strip. This last step is inferred from the layout's rules and the measurement; the lane's test confirms it.
- **The nested scroll.** Lane I-S's note has the same root. A `scroll` that stretches inside a tab page sits in a
  slot that is only as tall as its content, and `MorphScroll`'s natural height is at most 20 lines. So the scroll is
  20 lines tall. That is why the last commit of batch I dropped the scroll, and why the page then stopped scrolling.
- **The pages' padding is outside the page** (read). `MorphTabs` pads `pages` from outside (`Layout.margins:
  root.pageInsetPx`). Once a page scrolls and so clips, a child that bleeds past `pageInset` is cut at the clip, and
  the page's bar sits `pageInset` in from the node's edge. That is why review pads its own page column with
  `pageInset: 0` today.

**Decision.** A tab page is a page like the workspace's, and it carries its own padding.

- **It fills the page area.** `Mounter::mountPage` sets each page slot's layout to `stretch` both ways, as
  `Workspace::mountPage` does. A tabs node that is content-sized is unchanged, since its pages' area is then its
  content's height.
- **It pads itself.** `pages` loses its margins. Each page slot pads its own content by the node's `pageInset`, inside
  its flickable, and gives that padding as its children's `childBleedInset`. So a page that scrolls clips at the
  node's edges, its bleeding children reach those edges, and its bar sits at the edge. `pageInset: 0` stops being
  needed for a page that scrolls; the comment on `MorphTabs.pageInset` ("a page that scrolls pads its own content")
  is rewritten to say that the page's padding lies inside its scroll.
  - `MorphSlot` gains `property real contentInset: 0`. `MorphTabs` binds each page's to `pageInsetPx` as the page is
    added to `pages`.
  - On the terminal, `TabsImpl::childAreas` lays each page over the whole page area and hands it the padding, and
    `SlotImpl` places its children inside it (a bleeding one at full width, J1).
- **It scrolls itself.** The mount makes each page slot scrollable (`SlotWidget::setScrollable(true)`), as the
  workspace makes its pages.
  - Content that keeps its height scrolls inside the page.
  - Content that stretches fills it, never shorter than its `minHeight`.
  - The screen then needs no `scroll` of its own.
- **A slot scrolls only while its content overflows**, on both frontends. Today a scrollable slot "scrolls" whenever
  its child does not stretch, even when the child fits: on Qt Quick it clips and its flickable takes input over
  content that cannot move; on the terminal `SlotImpl::scrolls` makes the page a Tab stop (`wantsFocus`), so once
  every tab page is scrollable every read-only page would take the focus.
- **`MorphSlot`, one set of bindings for J3 and J11.** With `inset` for `contentInset`, `needX` for the column's
  minimum width (`column.Layout.minimumWidth`, J3) plus `2 * inset`, and `needY` for the column's implicit height
  plus `2 * inset`:

  ```
  overY   = stretchesDown ? stretchFloor > height : needY > height + 0.5
  overX   = needX > width + 2        (J3's rounding allowance)
  scrolls = scrollable && (overX || overY)
  clip:                 scrolls
  flick.interactive:    scrolls
  flickableDirection:   overX && overY ? HorizontalAndVerticalFlick : overX ? HorizontalFlick : VerticalFlick
  flick.contentWidth:   scrollable && overX ? needX : width
  flick.contentHeight:  scrollable && overY ? needY : height
  column.x, column.y:   inset
  column.width:         (scrollable && overX ? needX : width) - 2 * inset
  column.height:        stretchesDown ? max(flick.height, stretchFloor) - 2 * inset : implicitHeight
  vertical bar:         visible while scrollable && overY
  horizontal bar:       made from the `scrollBar` part, as the vertical one is; AlwaysOn while scrollable && overX
  ```

  `reveal()` keeps the focused item in view on both axes. `onScrollsChanged` resets `contentX` as it resets
  `contentY`. A slot that is not scrollable (a switch case) keeps today's bindings, with `inset` 0.
- **The terminal's `SlotImpl::scrolls`** gets the same vertical rule: true only while its content is taller than the
  slot, or a stretching child's `minHeight` is. `wantsFocus` follows it, so a page whose content fits is no Tab stop.
- **Review's All/Flagged** is a screen fix the vocabulary already expresses: `.sizing(ui::stretch())` on the compact
  segmented select, whose segments share a stretched control's width (`MorphSelect`). It belongs to J-F. If J-F
  lands without it, lane J-A's gallery commit makes it.

**Per backend.** As above. The terminal's tabs lay their pages out as a vertical stack in the page area, so a
content-sized page sits at the top there and is not centred; the stretch makes it fill the page area there too.

**Spec.**

- `backend_contract.md` (a tabs node's page: a slot that fills, pads itself by `pageInset`, and scrolls while it
  overflows);
- `interpreter.md`, where the tab page's mount is described;
- `qt_quick/backend.md` (`MorphSlot`'s bindings; `MorphTabs.pageInset`);
- `tui/frontend.md` (a slot scrolls, and takes the focus, only while it overflows).

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`, P6 as a test:
  - a tabs node that stretches, 364 px tall, whose page is one short column. The column's top is the strip's rule
    plus `pageInset`, and the strip's top is the node's top;
  - with the page's content taller than the page, the page scrolls: its flickable's `contentHeight` is the content's
    plus twice the inset, its bar is visible, and the bar's right edge is the node's right edge;
  - **Control:** today's mount and today's `pages` margins. The column's top is mid-page, the strip's top is about
    5 px down, and the bar is `pageInset` in.
- `tests/qt_quick/test_qt_quick_containers.cpp`: a scrollable slot whose content fits has `scrolls` false and `clip`
  false.
  - **Control:** today's binding.
- `tests/tui/test_tui_containers.cpp`: a scrollable slot whose content fits takes no focus on Tab; one whose content
  is taller does.
  - **Control:** today's `SlotImpl::scrolls`.
- `tests/test_ui_mount.cpp` (on `RecordingBackend`): a tab's page slot records `layout=stretch(1)/stretch(1)` and
  `scrollable`. Every existing mount test whose expected dump holds a tabs node gains those two on each page slot;
  the lane updates them, and the diff of those expectations is exactly that.

**Verify.** Re-grab review at 390 px.

- The strip sits directly under the divider, with no grey band.
- All/Flagged sits `space.4` under the strip's rule, and stretches.
- The comment field follows the cards. The page scrolls when the comment section is open, and the flagged rows still
  run edge to edge while it does.

## Theme tokens

| Token | Kind | Values or default | Item |
|---|---|---|---|
| `input.paddingX` | dimension | 8 | J8 |
| `list.selection` | keyword, resolved per surface | `accent` (default), `fill` | J10 |

- `kThemeTokens` grows from 68 to 70.
- `kKeywords` gains `list.selection`.
- A bad value of either is refused at `addFile`.

The gallery's bundle (`lims_gallery.cpp`) takes several of these:

- its `sidebar` surface sets `list.selection: fill` and `color.selection: #2B3038`, and its base keeps `accent`;
- it declares the `plainPage` surface (J6);
- it leaves `input.paddingX` at the default.

## Decisions

The user decided the six questions this design raised:

| | Question | Decision |
|---|---|---|
| Q1 | Overflow: report only, or report and scroll sideways? | Report it, and let the page scroll sideways (J3) |
| Q2 | The white compact page: a screen member or a theme switch? | A per-screen `pageSurface` (J6) |
| Q3 | A selected list item's look | `list.selection` defaults to `accent`, resolved per surface; the gallery's `sidebar` surface sets `fill` (J10) |
| Q4 | How the gallery's grabs show a signed-in shell | The grab command passes `--user` (J2) |
| Q5 | The space around a dialog's and a sheet's rules | 12 px, step 3, on each side (J7) |
| Q6 | `input.paddingX`'s default | 8 px, so every application's text inputs move 4 px left to match its selects (J8) |

## Review response

The design was reviewed at `cef20bbae` (3 blockers, 8 should-fixes, 6 nits). Every finding is taken; none is pushed
back.

| Finding | Where it is taken |
|---|---|
| B1 the overflow measure misses a packed row; the page never scrolls sideways | J3: the stack compares against the width inside its padding; a container's minimum is its content's, measured across the six example apps first (P8), with a table cell's exception and a 2 px rounding allowance; P1 rewritten |
| B2 J1 does not draw on the terminal | J1: on the terminal the padding moves down into the holder, which is laid over its parent's inner span; Qt Quick keeps forwarding the inset, since a layout does not clip there (P4). `list_widgets.cpp` is in J-A's files |
| B3 one theme-wide `list.selection` regresses review's queue | J10: the token is resolved per surface, as `border.style` is; the gallery's `sidebar` sets `fill`, its base keeps `accent` |
| S1 a scrolling tab page clips its bleeding children and insets its bar | J11: the page's padding moves into the page's slot; a conformance case for a page that overflows and bleeds; `MorphTabs.pageInset`'s comment rewritten |
| S2 every tab page becomes a terminal Tab stop | J11: the terminal's `SlotImpl::scrolls` follows the same overflow rule, with a test |
| S3 J3 and J11 change the same `MorphSlot` lines | J11: one set of bindings for both axes, `reveal()` on both |
| S4 when to check, the splitter, unbounded reports, the channel | J3: checked only while visible, sized and in a window; no splitter check; once per container while mounted; a `morph.layout` Qt warning, not a `ComponentFailure` |
| S5 the smoke cannot see the report | J3: a Qt warning is what every smoke records; the smokes settle after a resize; P8 measured the other five apps at their smoke size (no report) |
| S6 the table's bar is not the part | J3: the table makes its bar from the `scrollBar` part, and reserves its height while it shows |
| S7 how a selected item colours its content | J10: `ButtonImpl::scheme()` hands the content `contentScheme`, a scheme bound role by role whose `text` follows the selection |
| S8 the lanes share files | Lanes: an owner and an order for each shared file; framework commits do not wait on J-F; only `visual-review.md`'s grab command is a lane's, and the "After batch J" section is the coordinator's |
| N1 `setPassesBleed` and `TableRowImpl`; the flag in `lowerAll` | J1 |
| N2 tabs mount dumps change | J11's tests and J-A's file list |
| N3 the navigator's tests | J9 and J10 name `tests/test_ui_workspace_start.cpp` |
| N4 the select's lines | J8 |
| N5 Qt 6.8 `bleedInsetOf` | J1, and the lanes start on `lg-w` |
| N6 the terminal stays silent | Not in this batch, said explicitly |

## Defects this batch fixes

These are defects in shipped library behaviour, each closed by the item named. They are this batch's own work, so
they are not filed separately.

- **J1:**
  - `bleed` on a child of a panel or a tab that has more than one child reaches past nothing on either frontend;
  - `bleed` on any child of a tab page reaches past nothing, since the page's slot hides the tabs' padding;
  - on the terminal, a child laid past its parent's area is cut back to it, so forwarding an inset is not enough
    there;
  - batch I's tests missed both because they build the tree on the backend directly.
- **J3:** content wider than its container is cut off and nothing reports it; a stack's content minimum never reaches
  its parent; a table that scrolls sideways shows no bar at rest, and its bar is not the restyled part.
- **J4:** a field narrower than its text shows the text's end.
- **J5:** an inline lead wraps after the trail, against `document.md`.
- **J8:** a text area's text starts 4 px further in than an input's (the Basic style's `leftPadding`).
- **J11:**
  - a tab page in a tabs node that stretches is centred in the page area and does not fill it;
  - a `scroll` in such a page is 20 lines tall;
  - a scrollable slot whose content fits is clipped and interactive, and on the terminal takes the focus on Tab.

## Not in this batch

- **Overflow on the terminal** (J3): it is neither reported nor scrolled sideways, so it stays silent there, against
  the vocal-over-silent rule. The terminal has no diagnostics channel that does not draw over the screen; a frontend
  option for one, `onReport`, is the seam it needs, and is the next step for this item.
- **Screen re-fits for the 785 px page and the per-screen fixes** (lane J-F): audit's chip row `wrap`, samples',
  stability's and console's columns, review's footer and queue split, instrument's right column, receipt's sample
  grid, plate's range text, and stability's compact "Continue". J3's smoke test holds them once they land.
- **CI, and the `darkHead` `color.track` and `nav.*` icon warnings** (lane W).
- **The console's lab select at the leading side**, the sidebar's 215 px against the design's 176, and the review
  shell title above the queue: batch I's accepted residuals, unchanged.
- **A shadow under cards on the white page** (J6's residual).

## Lanes

Three lanes, each starting from `lg-w`: lane W's branch on `lg-fold-i`, which carries the Qt 6.8 `bleedInsetOf` fix
that J-A's new `childBleedInset` declarations follow.

**Framework commits never wait on J-F.** Every framework item has its own tests and controls and lands on `lg-w`'s
line on its own. Only a lane's gallery commit (screen files, the gallery's tests and bundle) waits for J-F, since J-F
is changing the same screen files; it is the lane's last commit and is rebased onto `lg-jf` once J-F's work is
verified. J3's no-overflow smoke is the only check that needs J-F's re-fits to pass.

### J-C: controls and theme (J4, J7, J8, J10's look)

- `include/morph/ui/theme.hpp` (the two tokens; `kThemeTokens` from 68 to 70; `kKeywords`);
- `src/qt_quick/theme.cpp` (the token map; `listSelectionFill` for the theme's scheme and each surface's);
- `src/qt_quick/qml/`: `Theme.qml`, `MorphScheme.qml`, `MorphTextInput.qml`, `MorphTextArea.qml`,
  `MorphDateTimeInput.qml`, `MorphSelect.qml`, `MorphDialog.qml`, `MorphDrawer.qml`, `MorphButton.qml`,
  `MorphButtonBackground.qml`;
- `src/qt_quick/kinds.hpp` (`ButtonImpl::scheme()`, one line; shared, below);
- tests: `tests/qt_quick/test_qt_quick_widgets.cpp`, `test_qt_quick_overlays.cpp`, `test_qt_quick_theme.cpp`,
  `tests/test_ui_theme.cpp`, `tests/tui/test_tui_fields.cpp`;
- spec: `theme.md`, `qt_quick/backend.md` (shared, below).
- **Gallery commit:** `lims_gallery.cpp`'s `sidebar` surface (`list.selection: fill`, `color.selection`).

### J-B: shell and navigator (J2, J5, J6, J9, J10's badge)

- `include/morph/ui/shell_frame.hpp` (the title line; `setPageSurface`);
- `include/morph/ui/navigator.hpp` (the groups; the item's content);
- `include/morph/ui/document.hpp` (`pageSurface`). The edit moves lines the branch-coverage allowlist names, so the
  lane updates the hints and verifies them per commit with the checker's resolver;
- `include/morph/ui/interpreter.hpp` (`Screen::pageSurface`, near line 10004; shared, below);
- `include/morph/ui/host.hpp`, `workspace.hpp`, `author.hpp`; `src/client/shell.cpp`;
- tests: `tests/test_ui_shell_frame.cpp`, `tests/test_ui_workspace_start.cpp`, `tests/test_ui_workspace.cpp`,
  `tests/client/test_shell.cpp`, `tests/tui/test_tui_shell_frame.cpp`, `tests/ui/corpus/documents.json`, and J5's
  Qt Quick case in `tests/qt_quick/test_qt_quick_containers.cpp` (shared, below);
- spec: `shell_frame.md`, `host.md`, `author.md`, `client/client.md`, and the shared ones below.
- **Gallery commit:** `lims_gallery.cpp`'s `plainPage` surface; the seven screens' `pageSurface`;
  `design/visual-review.md`'s grab command (`--user`).

### J-A: layout (J1, J3, J11)

- `include/morph/ui/view.hpp` (`Column::holder`);
- `include/morph/ui/interpreter.hpp` (`lowerAll`, near line 6906; shared, below);
- `include/morph/ui/mount.hpp` (`setPassesBleed`; `mountPage`'s stretch and `setScrollable`);
- `include/morph/ui/backend.hpp` (`StackWidget::setPassesBleed`);
- `include/morph/ui/testing/recording_backend.hpp` and `backend_conformance.hpp`;
- `src/qt_quick/qml/`: `MorphWidget.qml` (the propagated minimum), `MorphStack.qml`, `MorphSlot.qml`,
  `MorphScroll.qml`, `MorphTable.qml`, `MorphPanel.qml`, `MorphTabs.qml`;
- `src/qt_quick/widget.cpp`/`.hpp` (the overflow relay), `src/qt_quick/containers.cpp`, `src/qt_quick/table.cpp`
  (the table's bar from the part; a cell's `honoursContentMinimum`), `src/qt_quick/context.cpp`/`.hpp` (the report), `src/qt_quick/kinds.hpp`
  (shared, below);
- `src/tui/widget.cpp`/`.hpp`, `src/tui/container_widgets.cpp`, `src/tui/list_widgets.cpp` (`TabsImpl::childAreas`);
- `examples/common/testkit/render_smoke.hpp` (settling after a resize);
- tests: `tests/test_ui_interpreter_detail.cpp`, `tests/test_ui_recording_backend.cpp`, `tests/test_ui_mount.cpp`
  (and every mount test whose expected dump holds a tabs node: its page slots now record
  `layout=stretch(1)/stretch(1)` and `scrollable`), `tests/qt_quick/test_qt_quick_containers.cpp`,
  `tests/qt_quick/test_qt_quick_table.cpp`, `tests/tui/test_tui_containers.cpp`;
- spec: `view_tree.md`, `backend_contract.md`, `tui/frontend.md`, and the shared ones below.
- **Gallery commit:** `test_receipt.cpp`; review's compact tab pages (the page column with no `scroll`, All/Flagged
  stretched if J-F did not); then, once J-F has merged, `lims_gallery_render_smoke`'s no-overflow check.

### Shared files: owner and order

The lanes land in the order **J-C, then J-B, then J-A**: smallest first, and J-A last because it touches the most
shared files. A later lane rebases onto the earlier ones before it merges. Where a file is shared, the first lane in
that order edits it first, and the later lanes' edits are additions in other places of the file.

| File | Lanes | Owner | What each adds |
|---|---|---|---|
| `src/qt_quick/kinds.hpp` | J-C, J-A | J-A | J-C: `ButtonImpl::scheme()` returns `contentScheme`. J-A: `setPassesBleed` on `StackImpl` and `TableRowImpl`, the overflow relay |
| `include/morph/ui/interpreter.hpp` | J-B, J-A | J-A | J-B: `Screen::pageSurface` near line 10004. J-A: `lowerAll` near line 6906 |
| `tests/qt_quick/test_qt_quick_containers.cpp` | J-B, J-A | J-A | J-B: one new case for J5, appended. J-A: its cases, appended after |
| `docs/spec/qt_quick/backend.md` | all three | J-A | J-C: the input, dialog, drawer and item-button rows. J-B: none beyond the frame's, in `shell_frame.md`. J-A: Layout, Slot, Scroll, Table and the report |
| `docs/spec/ui/document.md` | J-B, J-A | J-B | J-B: `pageSurface`. J-A: one sentence under `bleed` |
| `docs/spec/ui/workspace.md` | J-B, J-A | J-B | J-B: `activePageSurface`, "Navigator and pins". J-A: a page that scrolls sideways |
| `docs/spec/ui/interpreter.md` | J-B, J-A | J-A | J-B: `Screen::pageSurface`. J-A: `lowerAll`'s holder |
| `examples/lims_gallery/src/lims_gallery.cpp` | J-C, J-B | J-B | J-C: the `sidebar` surface's tokens. J-B: the `plainPage` surface. Both gallery commits, in different string constants |
| `examples/lims_gallery/design/visual-review.md` | J-B | J-B | the grab command. No lane writes an "After batch J" section: the coordinator writes it once, from one set of re-grabs after all three lanes and J-F have merged |

Each lane re-grabs the screens its items name, at both sizes and with the `--user` grab command, and reports what it
measured to the coordinator, not into `visual-review.md`.
