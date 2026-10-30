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

None of the probes touches the tree.

**P1. A row that cannot shrink** (`lg-jd-probe1.qml`). A `ColumnLayout` 300 px wide holds a `RowLayout` of two
`Text`s and two 150 px rectangles, none of them filling, plus a sibling that fills:

```
qml: PROBE col 300 row w 609 row implW 609 rowMin 609 a 0 147 146.78125 b 152 147 r 304 150 p 459 150 fill 609
```

The row's minimum is its natural width. The column keeps its own width of 300 px but lays both children out at
609 px, so the sibling that fills is also 609 px wide. Whatever clips the column then cuts both, and nothing reports
it.

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

## The items

| Item | Verdict | Where it lands | Lane |
|---|---|---|---|
| J1 `bleed` inside a panel | **Reproduced** (P4, P5, a new grab). A library defect: the column the interpreter inserts hides the panel's padding. A tab page's slot does too | a holder column and a slot pass their parent's inset on | J-A |
| J2 the principal in the head | **Reproduced** (P7). The feature works; the grabs had no principal, since no `--user` was passed | the grab command; no framework change | J-B |
| J3 silent clipping | **Reproduced** (P1) for rows and columns. **Read** for tables: their sideways scroll exists, but its bar is hidden at rest | a report from every container whose content does not fit; a page that scrolls sideways; a table bar that shows | J-A |
| J4 a field shows the end of its text | **Reproduced** (P2, on stock Qt) | a field that is not focused shows its text from the start | J-C |
| J5 the inline lead wraps after the trail | **Read**: the top line is one flow in order, so the last item that does not fit is the one that wraps | the inline lead wraps inside the title's block | J-B |
| J6 a white compact page | **Read** from the mockups' CSS: seven of the nine compact windows set `background: #fff` | the screen's `pageSurface` | J-B |
| J7 the dialog's and sheet's rule spacing | **Read**: the rules are items of a column whose spacing is 5 px | a step-3 margin on each side of each rule | J-C |
| J8 a text area's inner padding | **Reproduced** (P3) | the text area sets its own sides; the token `input.paddingX` | J-C |
| J9 the sidebar's section overlines | **Read**: the navigator's root stacks its headings and its entries with no gap | each group is a column; the groups and the search are step 3 apart | J-B |
| J10 navigator selection | **Read**: the badge is the item's sibling, not part of it | the badge goes inside the item; the theme switch `list.selection` | J-B (badge), J-C (look) |
| J11 review 390's gaps; a scroll in a tab page | **Reproduced** (P6). It is one cause: a tab page's slot does not fill the page area | a tab page fills the page area and scrolls itself | J-A |

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

This is a defect in shipped library behaviour (`include/morph/ui/interpreter.hpp`, `src/qt_quick/qml`), not a
gallery bug.

**Decision.** A container that the mount inserts, and that the document did not write, passes its parent's bleed
inset to its children. The parent a document names is then the parent whose padding a bleed reaches past, as the
batch I spec already says. The direct-parent rule stands: a container the author wrote (a column with no padding,
say) still gives its children nothing to bleed past.

- **The holder column.** `Column` gains `bool holder = false`: the column `lowerAll` makes to hold several children.
  `listColumn` callers that make one for `lowerAll` set it. A `forEach`'s own column is not a holder.
- **The backend.** `StackWidget::setPassesBleed(bool)` is a new pure virtual. The mount calls it once, only with
  `true`, for a holder column.
  - The stack's horizontal padding is 0, so its children's bleed inset is the inset its own parent gives it.
  - It changes nothing else about the stack.
- **A slot always passes it on.** A slot is never a document node; it is where the mount shows a switch case or a
  tab page. So `SlotWidget` needs no method, and a backend's slot behaves as if `setPassesBleed(true)` had been
  called.

**Per backend.**

- **Qt Quick.**
  - `MorphStack`'s layout: `childBleedInset: root.insetX + (root.passesBleed ? root.bleedInset : 0)`, with a new
    `property bool passesBleed: false` that `StackImpl::setPassesBleed` writes.
  - `MorphSlot`'s `column`: `readonly property real childBleedInset: root.bleedInset`.
  - A `MorphWidget`'s own `bleedInset` binding is unchanged.
- **Terminal.**
  - `bleedAreas` (`src/tui/widget.cpp`) takes the span the container hands down. A stack that passes the bleed on, and
    a slot, hand down their own parent's span in place of their own content area.
  - The panel's span is its frame's inner edges (`container_widgets.cpp`), and the tabs' span is its page area.
- **`RecordingBackend`** records `passesBleed` on the stack's line.

**Spec.**

- `view_tree.md` (`Column::holder`);
- `interpreter.md` (`lowerAll`'s holder column);
- `backend_contract.md` ("Layout hints": the parent a bleed reaches past; `StackWidget::setPassesBleed`; a slot);
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
  - **Control:** a backend whose holder stack and slot pass nothing on. Both cases fail. Without the fix, the first
    case measures the same 9 px as P5.
- the interpreter's tests (`tests/test_ui_interpreter_detail.cpp`):
  - a panel with two children lowers to a `Column` with `holder` set;
  - a panel with one child lowers to that child, unwrapped;
  - **Control:** drop the flag from `lowerAll`.
- `tests/test_ui_recording_backend.cpp`: a holder column's line carries `passesBleed`.
- `tests/qt_quick/test_qt_quick_containers.cpp` and `tests/tui/test_tui_containers.cpp`: the backend-level cases from
  batch I stay, each joined by a case through a holder stack and through a slot.
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

- **A row is as narrow as its children's natural widths, and no narrower** (P1). In Qt Quick Layouts a child that
  does not fill has its preferred width as its minimum. A row of such children has their sum as its minimum.
  - A parent narrower than that lays the row out at its minimum anyway, and every sibling that fills is laid out
    that wide too. Then the nearest clip cuts the result: a page, a splitter pane, a card.
  - This is audit's 1000 px trail. Its chip row is about 640 px wide and does not wrap, so the trail table beside it
    is laid out wider than its pane, and the Reason column falls off the right edge.
  - It is also review's footer. The row holds "Approving needs your signature" and the three buttons, and is wider
    than its pane, so "Approve and sign…" is cut off.
- **A table wider than its columns allow scrolls sideways.** `MorphTable` puts the rows in `hflick`, which is
  interactive while `columnsWidth > width`, and attaches `MorphScrollBar`.
  - The stock bar is the Basic style's `ScrollBar`, whose `policy` is `AsNeeded`, so it shows only while the bar is
    active. A table at rest therefore looks cut off: stability's Pulls column, console's Test and Status.
  - The P7 grab shows console's worklist this way: the Status column ends in mid-word at the pane's edge, with no
    bar.
- **Nothing reports either case.** The comparison found them only by looking at the grabs.

**Decision.** Three parts: a report, so the problem is heard; a sideways scroll on the page, so the content can be
reached; a table bar that shows, so a sideways scroll can be seen. A container does not shrink its children on its
own. Whether text elides or a row wraps is the document's choice (`wrap`, `minWidth`, stretch), and the report names
where that choice is missing.

### The overflow report

- **What is checked.** Every container compares the minimum width of the layout that holds its children with the
  width that layout was given. Content that is wider overflows. The 0.5 px tolerance absorbs rounding. Each
  container names the layout it checks:
  - a stack's `layout`;
  - a panel's `inner` column;
  - a slot's `column`;
  - the tabs node's `pages`;
  - each pane of a splitter.

  A wrapping row never overflows, unless a single child is wider than the row.
- **When it reports.** On the change from fitting to overflowing, after the layout settles (`Qt.callLater`). It does
  not report again until the content fits once more. A resize that keeps overflowing reports nothing new.
- **How.** The QML fires `overflow` with `{needed, given, child}`, where `child` is the test id of the widest child
  whose minimum exceeds the width, or "" for none.
  - The widget's C++ side reports it through `Context::report`. That is the same channel the icon and
    undeclared-property reports use: a `ComponentFailure` when the application gave `Components`, otherwise stderr.
  - The text: `` row `review-footer` needs 512 px and has 430 px: `approve-hint` and its siblings neither wrap nor
    shrink, so they are cut at its edge ``.
  - A container with no test id is named by its kind and the nearest ancestor that has one: `` a column inside
    `trail-pane` ``.
- **A table** reports too, once, on the same change: `` table `conditions` scrolls sideways: its columns need 612 px
  and it has 490 px ``. Its content is reachable, so this is information rather than a fault. It is still worth
  saying, because a screen's fixed columns rarely mean it.

### The page scrolls sideways

`MorphSlot` (a workspace page, a tab page) and `MorphScroll` on the vertical axis lay their content column out at
`Math.max(viewport width, the column's minimum width)`.

- While the content is wider than the viewport, the flickable also scrolls sideways and shows a horizontal bar with
  `policy: ScrollBar.AlwaysOn`.
- Content that fits is laid out at the viewport's width, exactly as today.
- A page that stretches down keeps stretching. Only its width changes.

### The table's bar shows

`MorphTable`'s horizontal bar gets `policy: hflick.interactive ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded`. A restyled
`scrollBar` part keeps that policy, since the table sets it on the instance it makes.

**Per backend.**

- **Qt Quick:** as above. `ContainerItemWidget` gains the `overflow` relay once, for every container.
- **Terminal:** a stack, a panel and a slot know their children's natural cells and their own, so they detect the
  same condition. The terminal has no diagnostics channel, and writing to stderr would draw over the screen, so this
  batch neither reports overflow there nor scrolls a page sideways. Content keeps being cut at the column. That is
  listed under **Not in this batch**.

**Spec.**

- `backend_contract.md` ("Layout": what a backend does when content does not fit);
- `qt_quick/backend.md` (the report, its text, when it fires; the page's sideways scroll; the table's bar);
- `workspace.md` (a page scrolls sideways when its content cannot be narrower);
- `tui/frontend.md` (the terminal cuts, unreported).

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - **Row overflow.** A column 300 px wide holding a row of two texts and two fixed 20-unit spacers, which cannot
    fit. Exactly one report names the column and the row, with the needed and given widths.
    - Widen the column until it fits, then narrow it again, and there is exactly one more report.
    - **Control:** a column that fits reports nothing.
    - **Mutation:** compare the width against the implicit width in place of the minimum, and the case with stretch
      children reports falsely.
  - **Page scroll.** A scrollable slot 300 px wide holding the same row. Its flickable's `contentWidth` is the row's
    minimum width, and the horizontal bar is visible.
    - **Control:** a row that fits. `contentWidth` equals the viewport and no bar shows.
  - **Table bar.** A table 200 px wide whose fixed columns need 400 px. The horizontal bar is visible at rest, and one
    report names the table.
    - **Control:** the same table 500 px wide. No bar and no report.
- **The gallery.** `lims_gallery_render_smoke` renders the ten screens at 1000×830 and 390×844 and fails on any
  overflow report. It is the check that turns J-F's re-fits into a guarantee.
  - **Control:** revert one of J-F's fixes (audit's chip row without `wrap`), and the smoke fails naming it.
  - It lands after J-F, since before J-F it fails by design.

**Verify.**

- Re-grab audit, review, stability and console at 1000 px **before** J-F's fixes land on the lane's branch. Each logs
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
  rule against 14. 14 is not a step, and a dimension token for 2 px is not worth its name. See question Q5.
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
  measured from (today `Theme.spaceOf(3)` in the text input and date input, and `spaceOf(2)` in the select).
- **The text area's other sides.** `MorphTextArea` sets `leftPadding` and `rightPadding` from the token, and its
  `topPadding` and `bottomPadding` to `(Theme.controlHeight - line height) / 2`. Its first line then sits where a
  single-line input's text sits: about 8 px at the default 32 px control and a 16 px line, against the design's 6.
- **The default** is 8, the select's value today and the design's, so the stock controls agree. That moves every
  application's text inputs 4 px left, so it is the user's to confirm (Q6).
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

**Tests.** `tests/test_ui_shell_frame.cpp`, where `Navigator` is tested:

- the root records `gap=3`, and each group's section `gap=1` holding its heading first;
- a search that matches no entry of a group hides its section;
- **Control:** the old tree.

**Verify.** Re-grab any screen at 1000 px. "GALLERY" sits 12 px under the search box, and each later heading 12 px
under the entry above it.

## J10: a navigator entry is selected across its badge, and the selection's look is the theme's

**Problem (read).**

- **The badge.** Each entry is a row holding a stretching `ButtonStyle::Item` button and then its badge, `gap` 1.
  `MorphButtonBackground` draws the selection under the button only, so the highlight stops at the badge. In
  instrument at 1000 px it ends at x = 180, against 203 for an entry without a badge.
- **The look.**
  - A selected list item draws `scheme.selection`, a 3 px accent bar at its leading edge, and its label (and icon) in
    the accent.
  - The design's `.nav.on` is `background: #2B3038; color: #fff`: a fill, with the label in the surface's text
    colour and no bar.
  - Both looks are reasonable. Changing ours changes every application's lists, so it is a theme switch.

**Decision.**

- **The badge goes inside the item.** The navigator gives each entry's button content, which `ButtonWidget::content()`
  already supports for `ButtonStyle::Item`:
  - a horizontal stack (`nav_content_<screen id>`, gap step 1, `align: center`);
  - inside it, the title as a text that stretches, then the badge (`nav_badge_<screen id>`, as today).

  The button's background then spans the whole entry, the badge with it. The button keeps its label, which names it
  with its count, as it does today. The entry's row is the button alone.
- **A selected item's content follows the look.** While an item that has content is selected, its content's `Normal`
  text role draws in the colour the item's label would have. The item passes its content a scheme whose `text` role
  is that colour. `muted` and the tones are untouched.
- **The theme switch.**

  | Token | Values (default first) | What it switches | Read by |
  |---|---|---|---|
  | `list.selection` | `accent`, `fill` | A selected list item: `accent` draws the selection colour, a leading accent bar and the label in the accent; `fill` draws the selection colour and the label in the scheme's text colour, with no bar | `MorphButtonBackground`, `MorphButton` (through `Theme.listSelection`) |

  - It is a `keyword` token, as `collapsible.marker` is. Any other value is refused at `addFile` (`list.selection:
    only accent and fill are drawn`).
  - It applies to every item-style button: the navigator, menus drawn as lists, and a document's list items. A key
    means one thing everywhere.
- **The gallery** sets `list.selection: fill`, and its `sidebar` surface's `color.selection` to `#2B3038`.

**Per backend.**

- **Terminal.** The item's content draws on the entry's row, as an item with content does today. The terminal
  marks a selected item by inverting it and reads no theme token, so `list.selection` does not change it.
- **Qt Quick.** As above.

**Spec.**

- `theme.md` (`list.selection`);
- `qt_quick/backend.md` (the item button's selected look; the content's text role while selected);
- `workspace.md`, "Navigator and pins" (the entry: one button with content).

**Tests.**

- `tests/test_ui_shell_frame.cpp`: each entry is a button whose content holds `nav_badge_<id>`; `setBadge` shows it
  there.
  - **Control:** the old row.
- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - a selected item with content of a stretching text and a badge: the background's width is the button's, and the
    badge lies inside it;
  - under `list.selection: fill`, the bar is hidden and the label is `scheme.text`;
  - under `accent`, the bar shows and the content's text is the accent;
  - **Control:** the default theme with the look forced to `accent`. The `fill` checks fail.
- `tests/test_ui_theme.cpp`: `list.selection: bar` is refused.

**Verify.** Re-grab instrument and console at 1000 px. The selected entry's fill runs to the sidebar's inner edge,
past the badge, with a white label and no bar.

## J11: a tab page fills the tabs node, and scrolls itself

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

**Decision.** A tab page is a page like the workspace's.

- **It fills the page area.** `Mounter::mountPage` sets each page slot's layout to `stretch` both ways, as
  `Workspace::mountPage` does. A tabs node that is content-sized is unchanged, since its pages' area is then its
  content's height.
- **It scrolls itself.** The mount makes each page slot scrollable (`SlotWidget::setScrollable(true)`), as the
  workspace makes its pages.
  - Content that keeps its height scrolls inside the page.
  - Content that stretches fills it, never shorter than its `minHeight`.
  - The screen then needs no `scroll` of its own, and its padded column is the page's content, so its bleeding rows
    still reach the edges (J1).
- **A slot scrolls only while its content overflows.** Today `MorphSlot.scrolls` is true for any scrollable slot
  whose child does not stretch, even when the child fits. That clips, and makes the flickable interactive over
  content that cannot move. It becomes:

  ```
  scrollable && (stretchesDown ? stretchFloor > height : column.implicitHeight > height + 0.5)
  ```

  A workspace page whose content fits is then neither clipped nor interactive.
- **Review's All/Flagged** is a screen fix the vocabulary already expresses: `.sizing(ui::stretch())` on the compact
  segmented select, whose segments share a stretched control's width (`MorphSelect`). It belongs to J-F. If J-F
  lands without it, lane J-A's gallery commit makes it.

**Per backend.** The terminal's tabs lay their pages out as a vertical stack in the page area
(`TabsImpl::childAreas`), so a content-sized page sits at the top there and is not centred. The stretch makes it
fill the page area on the terminal too, and `setScrollable` makes it scroll as a workspace page does. The terminal's
`bleedAreas` in `TabsImpl::childAreas` hands its span to the page slot, which J1 makes pass it on.

**Spec.**

- `backend_contract.md` (a tabs node's page: a slot that fills and scrolls);
- `interpreter.md` or `view_tree.md`, wherever the tab page's mount is described;
- `qt_quick/backend.md` (`MorphSlot.scrolls`).

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`, P6 as a test:
  - a tabs node that stretches, 364 px tall, whose page is one short column. The column's top is the strip's rule
    plus `pageInset`, and the strip's top is the node's top;
  - with the page's content taller than the page, the page scrolls: its flickable's `contentHeight` is the content's
    and its bar is visible;
  - **Control:** today's mount. The column's top is mid-page, and the strip's top is 5 px or so down.
- `tests/qt_quick/test_qt_quick_containers.cpp`: a scrollable slot whose content fits has `scrolls` false and `clip`
  false.
  - **Control:** today's binding.
- `tests/test_ui_mount.cpp` (on `RecordingBackend`): a tab's page slot records `layout=stretch(1)/stretch(1)` and
  `scrollable`.

**Verify.** Re-grab review at 390 px.

- The strip sits directly under the divider, with no grey band.
- All/Flagged sits `space.4` under the strip's rule, and stretches.
- The comment field follows the cards. The page scrolls when the comment section is open.

## Theme tokens

| Token | Kind | Values or default | Item |
|---|---|---|---|
| `input.paddingX` | dimension | 8 | J8 |
| `list.selection` | keyword | `accent` (default), `fill` | J10 |

- `kThemeTokens` grows from 68 to 70.
- `kKeywords` gains `list.selection`.
- A bad value of either is refused at `addFile`.

The gallery's bundle (`lims_gallery.cpp`) takes several of these:

- it sets `list.selection: fill`;
- it gives its `sidebar` surface `color.selection: #2B3038`;
- it declares the `plainPage` surface (J6);
- it leaves `input.paddingX` at the default.

## Decisions for the user

Each has a recommended answer. The lanes start on the recommendation unless told otherwise.

- **Q1. Overflow: report only, or report and scroll sideways?**
  - **Recommended:** both (J3).
  - The report tells the author, which is the user's standing preference. The sideways scroll keeps the content
    reachable for the user of a screen nobody re-fitted.
  - Reporting alone would leave production screens with content nobody can reach.
- **Q2. The white compact page: a screen member or a theme switch?**
  - **Recommended:** the screen member `pageSurface` (J6).
  - Two of the nine compact mockups (samples, console) keep the grey page with cards on it, so a theme switch would
    still need a per-screen override.
- **Q3. A selected list item's look.**
  - **Recommended:** the theme switch `list.selection`, default `accent` (today's look), with the gallery on `fill`.
  - The alternative is to make `fill` the default, which changes every application's navigator and lists.
- **Q4. How the gallery's grabs show a signed-in shell.**
  - **Recommended:** `--user "Dr. A. Meyer"` in the grab command (J2), with no framework change.
  - The alternative is a default development principal on `LocalApp`.
- **Q5. The dialog's body is 12 px under its head rule, against the design's 14.**
  - **Recommended:** accept step 3 for every rule gap (J7).
  - The alternative is a dimension token, `overlay.sectionGap`, for 2 px on two edges.
- **Q6. `input.paddingX` defaults to 8, moving every application's text inputs 4 px left.**
  - **Recommended:** yes. The select already insets its value by 8, so the stock controls disagree today, and the
    design draws 8.
  - The alternative is a default of 12, with the gallery setting 8. The select then moves 4 px right in every other
    application.

## Defects this batch fixes

These are defects in shipped library behaviour, each closed by the item named. They are this batch's own work, so
they are not filed separately.

- **J1:**
  - `bleed` on a child of a panel or a tab that has more than one child reaches past nothing on either frontend;
  - `bleed` on any child of a tab page reaches past nothing, since the page's slot hides the tabs' padding;
  - batch I's tests missed both because they build the tree on the backend directly.
- **J3:** content wider than its container is cut off and nothing reports it; a table that scrolls sideways shows no
  bar at rest.
- **J4:** a field narrower than its text shows the text's end.
- **J5:** an inline lead wraps after the trail, against `document.md`.
- **J8:** a text area's text starts 4 px further in than an input's (the Basic style's `leftPadding`).
- **J11:**
  - a tab page in a tabs node that stretches is centred in the page area and does not fill it;
  - a `scroll` in such a page is 20 lines tall;
  - a scrollable slot whose content fits is clipped and interactive.

## Not in this batch

- **Overflow on the terminal** (J3): it is neither reported nor scrolled sideways. The terminal has no diagnostics
  channel that does not draw over the screen. A frontend option for one, `onReport`, would be the seam.
- **Screen re-fits for the 785 px page and the per-screen fixes** (lane J-F): audit's chip row `wrap`, samples',
  stability's and console's columns, review's footer and queue split, instrument's right column, receipt's sample
  grid, plate's range text, and stability's compact "Continue". J3's smoke test holds them once they land.
- **CI, and the `darkHead` `color.track` and `nav.*` icon warnings** (lane W).
- **The console's lab select at the leading side**, the sidebar's 215 px against the design's 176, and the review
  shell title above the queue: batch I's accepted residuals, unchanged.
- **A shadow under cards on the white page** (J6's residual).

## Lanes

Three lanes. Each starts from `lg-fold-i` once lane W has landed, and their framework files are disjoint except where
noted. Each lane's gallery changes are its **last** commit, rebased onto J-F's branch, since J-F is changing the same
screen files now.

### J-A: layout (J1, J3, J11)

The largest lane. Its framework files:

- `include/morph/ui/view.hpp` (`Column::holder`);
- `include/morph/ui/interpreter.hpp` (`lowerAll`; the region at about line 6906);
- `include/morph/ui/mount.hpp` (`setPassesBleed`; `mountPage`'s stretch and `setScrollable`);
- `include/morph/ui/backend.hpp` (`StackWidget::setPassesBleed`);
- `include/morph/ui/testing/recording_backend.hpp` and `backend_conformance.hpp`;
- `src/qt_quick/qml/`: `MorphStack.qml`, `MorphSlot.qml`, `MorphScroll.qml`, `MorphTable.qml`, `MorphPanel.qml`,
  `MorphTabs.qml`, `MorphSplitter.qml` (each declares the layout it checks for overflow);
- `src/qt_quick/widget.cpp`/`.hpp` (the overflow relay);
- `src/qt_quick/kinds.hpp` and `src/qt_quick/containers.cpp`;
- `src/qt_quick/context.cpp`/`.hpp` (the report's text);
- `src/tui/widget.cpp`/`.hpp` and `src/tui/container_widgets.cpp` (`bleedAreas` through a holder and a slot).

Its gallery commit:

- `test_receipt.cpp` (J1);
- review's compact pages: the tab's page column back in place, with no `scroll`, and All/Flagged stretched if J-F
  did not;
- `lims_gallery_render_smoke`'s no-overflow check, last of all, after J-F.

It waits on J-F for the smoke check only.

### J-B: shell and navigator (J2, J5, J6, J9, J10's badge)

Its framework files:

- `include/morph/ui/shell_frame.hpp` (the title line; `setPageSurface`);
- `include/morph/ui/navigator.hpp` (the groups; the item's content);
- `include/morph/ui/document.hpp` (`pageSurface`);
- `include/morph/ui/interpreter.hpp` (`Screen::pageSurface`; the region at about line 10004, away from J-A's);
- `include/morph/ui/host.hpp`, `include/morph/ui/workspace.hpp`, `include/morph/ui/author.hpp`;
- `src/client/shell.cpp`.

Its gallery commit:

- `lims_gallery.cpp`: the `plainPage` surface;
- seven screens' `pageSurface`;
- `design/visual-review.md`'s grab command (`--user`).

The `document.hpp` edit moves the lines the branch-coverage allowlist names, so the lane updates its hints and
verifies them per commit with the checker's resolver.

### J-C: controls and theme (J4, J7, J8, J10's look)

Its framework files:

- `include/morph/ui/theme.hpp` (the two tokens, `kThemeTokens` to 70);
- `src/qt_quick/theme.cpp` and `src/qt_quick/qml/Theme.qml`;
- `src/qt_quick/qml/`: `MorphTextInput.qml`, `MorphTextArea.qml`, `MorphDateTimeInput.qml`, `MorphSelect.qml`,
  `MorphDialog.qml`, `MorphDrawer.qml`, `MorphButton.qml`, `MorphButtonBackground.qml`.

Its gallery commit: `lims_gallery.cpp`'s theme index (`list.selection: fill`, `sidebar`'s `color.selection`).

### Shared files and landing order

- **`lims_gallery.cpp`** is touched by J-B (a surface and the app document) and J-C (theme tokens), in different
  string constants.
- **`interpreter.hpp`** is touched by J-A and J-B in regions about 3000 lines apart.
- **J10** is split: J-B moves the badge inside the item and J-C draws the look. Either lands alone without harm: the
  badge inside the item draws today's `accent` look until J-C lands, and J-C's `fill` draws on today's row until J-B
  lands.

Landing order:

1. **J-C first.** It is the smallest and is QML plus a token table.
2. **Then J-B.**
3. **Then J-A.** Its smoke check needs J-F's re-fits on master, and J-A's own J3 report makes J-F's work
   measurable. J-A may open its pull request without the smoke commit and add it once J-F has merged.

Each lane re-grabs the screens its items name, at both sizes and with the `--user` grab command, and records what it
measured in `design/visual-review.md`'s "After batch J" section.
