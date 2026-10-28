# Terminal frontend — design

Design spec for `morph::tui` (`include/morph/tui/`, sources in `src/tui/`): the terminal renderer of the view tree
and the frontend that runs an application in a terminal. It renders what a terminal can of every widget of
[`../ui/backend_contract.md`](../ui/backend_contract.md), passes that contract's conformance suite, and runs the
application on one thread through [`../ui/frontend.md`](../ui/frontend.md)'s seam. The screens are designed for
desktop and web; the terminal is best effort.

## Contents

- [Packaging](#packaging)
- [The loop executor](#the-loop-executor)
- [The frontend](#the-frontend)
- [Widgets](#widgets)
- [Layout](#layout)
- [Focus, dialogs and drawers](#focus-dialogs-and-drawers)
- [Keys and chords](#keys-and-chords)
- [Pointer and drag](#pointer-and-drag)
- [Field state](#field-state)
- [The conformance probe](#the-conformance-probe)
- [Design decisions](#design-decisions)

## Packaging

`morph_tui` (alias `morph::tui`) is a STATIC library built when `MORPH_BUILD_TUI` is on (default off). It links
`morph` and core-cpp's `core::tui` PUBLIC. With the option on, core-cpp is built with `CORE_CPP_WITH_TUI ON` and
`CORE_CPP_WITH_IMAGES OFF`, and libunicode 0.9.3 is found first, else fetched; a fetch runs libunicode's configure,
which downloads the Unicode database. Native builds only: core-cpp builds no `core::tui` under Emscripten, so the
option is ignored there.

Public headers: `backend.hpp` (`tui::Backend`), `frontend.hpp` (`tui::Frontend`, `FrontendConfig`, `ShellFactory`,
`frontendOption`, `kInterruptExitCode`) and `loop_executor.hpp` (`tui::LoopExecutor`). They name `core::tui` types
and no private `src/tui/` type.

## The loop executor

`LoopExecutor : exec::IExecutor` runs each posted task in a later turn of one `core::net::EventLoop`, with itself
current. The frontend's reactive runtime is owned by one, and every bridge handler an application makes takes it as
its callback executor, so replies, timers, sockets and input run on the thread that turns the loop. `post` may be
called from any thread; a task still queued when the executor is destroyed is dropped, and a task that throws is
logged. It is destroyed on the loop's thread, between turns or from one of its own tasks.

## The frontend

`tui::Frontend` is named `"tui"`. `frontendOption(config)` makes it, usable when standard input is a terminal.

`run(source)`:

1. builds an `exec::IoLoop{IoLoopDriver::Caller}`, a `LoopExecutor` on it, the terminal (the process's own unless
   `FrontendConfig::terminal` names one), a `core::tui::Screen`, a `TuiRuntime` over the terminal's input (or
   `FrontendConfig::input`), and the session: the `ui::AppContext`, its runtime, its scheduler and the
   `tui::Backend`;
2. runs the source through `ui::runApp`: the shell is mounted by `FrontendConfig::mountShell`, which receives the
   backend with the context and the source's result;
3. pumps input until `AppContext::quit`, Ctrl+C or SIGINT (exit code 130, `kInterruptExitCode`), or the end of
   input (exit code 0);
4. tears down in reverse: the shell, the source, the session, the input, the screen, the executor, the loop. The
   process's own terminal is restored.

`mountShell` must not be empty: `run` throws `std::invalid_argument` before the source is opened. The mouse mode is
`FrontendConfig::mouse`, `MouseTracking::Drag` by default, which drag-and-drop needs.

Between events: each input event is dispatched once, to the screen, with the loop executor current; the pump then
waits for a later turn, so the flush and the frame the event caused run before the next event is handled. Redraws
are coalesced to one per turn. A frame fits the roots to the screen, gives the focus to the first focusable widget
when nothing holds it, and draws; while a visible `busy` spins, a timer advances it. Tab and Shift+Tab move the
focus; Esc ends a drag wherever the focus is.

The width class (`Backend::widthClass()`) is the viewport's columns, classed by `ui::widthClassOf`, read when asked:
an 80-column terminal is `medium`. After dispatching a resize the session calls `Backend::refreshWidthClass()`, which
compares the class with the one last reported and calls the `setOnWidthClass` listener on a change, outside the draw,
so the re-render comes on the next turn; `fit()`, which runs inside the draw, never calls it.

## Widgets

Every `ui::IViewBackend` factory makes a widget whose view is a `core::tui::Component` it owns.

| Widget | Shows | Input |
|---|---|---|
| Text | one row per line, styled by role: a heading bold, a section bold in the muted style, muted, error and success in their theme styles; mono as normal text, since every cell is drawn in one fixed-pitch font. Spans draw run after run in the role's style, a strong run bold and a mono run as the others; a run's line feed starts the next row | — |
| Button | `[ label ]`, a framework icon's glyph before the label, the caption in the list's selected-item style while selected (the brackets keep the button's style, so a selected push button is as wide as another); a list item ` label`, unbracketed and padded to its slot's width, in the list's selected-item style while selected and the focus style while focused. An item with content holds it as a column, a cell in from its leading edge, paints its line style across every row and the content over it on that background, and takes Enter, Space and a click on any row | Enter, Space, click |
| TextInput | `core::tui::InputField`; single-line, multiline or masked; a unit is drawn muted at the right edge before the field marks, and the editable width ends one cell before it | typing; Enter submits a single-line field after its commit, and a field without a submit handler commits and passes Enter on; the focus leaving after an edit commits |
| Checkbox | `[x] label` | Space, click; Enter passes on |
| Select | radio: a row per option, `(•)` marking; dropdown: `[label ▾]` opening a list overlay; segmented: one row of the options, each padded by a space and joined by `│`, the marked one in the list's selected-item style | Enter, Space, click; arrows in the list; Left and Right on a segmented one, and a click on a segment |
| Menu | one level of entries as a list, `>` on an entry with a submenu, the mark of a checkable one, its chord, its description muted; a separator a `─` row the cursor skips. Button style: `[label ▾]`, or an icon-only trigger's glyph (`[⋯]` for `more`) or else its accessible name | Right, Enter, Space or a click opens a submenu; Left or Backspace goes up; choosing a leaf shows the top again. Button style: Enter, Space, Down or a click opens the entries as a list overlay, which drills into submenus the same way; a choice, Esc or the focus leaving closes it and gives the focus back to the trigger |
| Stack, Slot | children along an axis, a spacing step apart; a scrollable slot whose shown children keep their content height is a viewport over them that follows the focus, as Scroll is, and takes the focus itself while nothing inside can | a scrolling slot: arrows, PageUp/PageDown, Home/End, wheel |
| Grid | equal columns, or columns sized by their tracks; cells row-major, spans | — |
| Spacer | nothing; stretches in a stack | — |
| Panel | a box with a title; a collapsible one folds to its title line | Enter, Space, click on the title |
| Scroll | a viewport that follows the focus | arrows, PageUp/PageDown, Home/End, wheel |
| Tabs | a one-row bar over the page slots, with no blank row under it (a terminal row costs more than the separation earns); a bar narrower than its labels draws whole labels from a first visible one, which painting moves so the highlighted label fits, with `‹` at the start and `›` at the end while labels are cut there; a hidden bar gives its row to the pages and takes no focus | Left, Right, click on a label; a click on `‹` or `›` selects the previous or next tab |
| Dialog | a centred box over everything; its footer is the box's bottom row, its children against the right edge, one cell apart, and stays last when the content mounts again | Esc dismisses |
| Drawer | a box against its side, the screen's height; its footer as a dialog's | Esc dismisses |
| Busy | a spinner and its label while active | — |
| Table | a header and keyed rows, `*` on selected rows, `>` on the cursor; with a select column a five-cell gutter, `[x]` or `[ ]` per row in place of the `*`, under a header box `[x]`, `[-]` or `[ ]` — a click on a row's box toggles the row as Ctrl+click does, and on the header box is Ctrl+A; a toned row's cells draw in the tone's style, keeping any inverse; the layout solver never squeezes a column below its header's width (a fixed column below its own width), and the columns past the table's edge are cut; a row's cells keep their own height and sit at its top, centre (rounded down) or bottom by their column's `valign`; a text, a badge, a check box or a push button is drawn at its natural width at its column's start, centre or end by the column's `align`, and the header's label with it; a text input keeps its text at the start, since `core::tui::InputField` has no alignment; the sorted column's label is followed by ` ▲` or ` ▼`, and a sortable column's header keeps room for it; a heading row is one cell across every column and their gaps, with nothing in its gutter, and widens no column | arrows, Space selects or toggles, Shift+Up and Shift+Down select a range from the anchor, Ctrl+A selects every row or clears them (Multiple), Enter activates; a click selects only its row, Ctrl+click toggles it and Shift+click selects a range where the emulator passes the modifier on (`clickWith`), and a plain double click activates; a click on a sortable column's header asks to sort by it (the header takes no keyboard focus); the cursor passes over a heading row, a click on one does nothing, and Ctrl+A, a range and the header box leave it out |
| DateTimeInput | `YYYY-MM-DD[ HH:MM]` in the display zone | typing; Up/Down step |
| Slider | `[====\|----] value` | Left, Right, Home, End, press and drag on the track |
| FilePicker | a path field, prompted `Open: ` or `Save: ` | typing; Enter |
| Banner | its text in its tone (spans as a text's, on the one row, a strong run bold), `[ action ]`, `[x]` when dismissible | Enter runs the action; Esc or Delete dismisses; a click on either |
| Badge | `[text]` in its tone, `(text)` when outlined, a framework icon's glyph before the text; a removable one `[text ×]`, inverse while focused | a removable one: Delete, Backspace, Enter or Space while focused, or a click on the `×`, removes it; a click elsewhere on it focuses it |
| Progress | `[####......] 40%` and the label; `<=>` when indeterminate | — |
| Steps | a row per step: `[ ]` pending, `[>]` active, `[x]` done, `[!]` failed; the current one accented | — |
| KeyValue | captions, then the values lined up after the widest; stacked, each pair two rows, the caption muted over its value, with no rule, since a rule would cost a row per pair and the muted caption already separates them. A toned value draws in its tone's style, bold; a value with spans draws them as a text's | — |
| EmptyState | the title, the explanation, `[ action ]` | Enter, click on the action |
| Chart | points in cells, below; its summary, muted and wrapped, when smaller than 20 by 5 cells; content size 40 by 8 | — |
| Splitter | the panes and a handle between each two | dragging a handle; the release reports every size |
| Collapsible | `▾ title` with the header beside it, the body below while open | Enter, Space, click on the title; the header stays live while closed |
| DropZone | a dashed frame around its children | a paste while it has the focus is a drop (see below) |

**A chart in cells.** The plot fills the widget below `yLabel` (its own row, when given) and above the points'
labels and the legend. A gutter at the left, at most 8 columns, holds the lines' labels when three or more label
the axis and the ticks' otherwise, right-aligned; with ticks, a line's label is written at the right end of its row,
over the line. A value lands on row `round((yMax − y) / (yMax − yMin) × (rows − 1))` of the plot, a position on
column `round((x − xMin) / (xMax − xMin) × (columns − 1))`. A line is a row of `─` (solid), `╌` (dashed) or `┄`
(dotted) in its tone's style, muted for neutral; two lines on one row draw the one of the more severe tone, and its
label. A point is its series' glyph, `●`, `■`, `▲` or `◆` in token order, in the series colour (`color.chart.<n>`'s
default as a true colour, since the terminal theme has no chart colours); a marked point is `✕` in its tone's style
with its note two cells after it when it fits; a clipped point is `△` on the top row or `▽` on the bottom one. A
point is drawn over a line in the same cell, and of two points in one cell the marked one, else the later. Points
are not joined: a cell is about twice as tall as it is wide, and box characters between cells would draw slopes
that are not there. The points' labels take the row below the plot, each skipped when it would overlap the last one
drawn and the newest always drawn; the legend row (two or more series) is each glyph beside its label. At the
content height a value is placed to within a fraction of the range, so the summary carries what a reader must not
miss, and a document that needs the values shows a table too.

A text input, select or date-time input whose caption is shown (`setCaption(label, true)`) draws it on its own row
above the control, one row taller; without it nothing changes. A radio select's rows, and a dropdown's list, keep
to the rows below the caption.

A terminal has no icon map and no assistive technology: icons, accessible names and roles are kept and not drawn,
and a widget draws on its surface, its own or else its nearest container's: `sidebar` and `toolbar` draw every cell
of the widget inverse, set after it paints so a child painting over its container stays inverse, and any other
surface draws as no surface does. A surface changes no size. The test id is kept for the probe. The tooltip shows on hover.

**Files dropped on a terminal.** A terminal reports a file dragged onto it as its path pasted. A drop zone takes the
focus, and a paste while it has it is a drop: the text read as paths, separated by whitespace, each quoted with `'`
or `"`, or with spaces escaped by `\`, or as a `file://` URI whose `%XX` escapes are decoded. The contract's rules
then apply: a file outside the accepted extensions, compared without case, or several files when one is taken,
refuse the drop whole.

## Layout

A cell-based solver. A stack gives Fixed children their amount, Content children their natural extent, each clamped
into its bounds, and shares what is left among Stretch children by weight (`layout::distribute`). A share stops at
its child's maximum, and what a capped child leaves goes to the Stretch children not capped, at most one pass per
Stretch child; a share below its minimum is raised to it. An overflow is taken from the last child first, down to
each child's minimum, and then the stack overflows and clips. A grid places cells
row-major in equal columns, or in columns its tracks size: `layout::planGrid` gives each track to `distribute` as a
stack item, a content track's natural width its widest cell that spans only it. Several roots share the screen as a column, in the order they were made: each takes the
height it asks for, and the last shown one what is left; a single root takes the whole screen.

Across a stack's axis, a Content or Stretch child fills the stack's extent and a Fixed child gets its amount, clamped
into its bounds and then into the space there is (`layout::crossExtent`): in a column every content child is as wide
as the column, in a row as tall as the row, and one its maximum stops sits at the start. A grid cell, and the one child
of a panel, slot or scroll area, fills its width the same way, and a grid row is as tall as its tallest cell's
requested height clamped into that cell's bounds. A widget's requested size, which a container's natural size sums,
is its natural size clamped into its bounds.

**Alignment.** Across the axis a child's `alignSelf`, else its stack's `align`, places it (`slotOf`): under `start`,
`center` or `end` a Content child takes its requested extent instead of the stack's (`alignedExtent`), every other
child its `crossExtent`, and one narrower than the space is offset to the start, the centre (rounded down) or the end
(`alignedOffset`). Along the axis, while no shown child is Stretch, `solvedAreas` places the spare length by
`justify`: before the first child, half of it for `center` and all of it for `end`; for `between`, `spare / (shown −
1)` cells added to every gap and the remainder one cell to each gap from the first. A grid places each cell in its row
the same way by the cell's `alignSelf`, else the grid's `align`; across its column the cell fills the width as
before, except that a track with `align` `center` or `end` places a cell drawn at its natural width
(`placedAcrossColumn`: a text, a badge, a check box, a push button) there, as a table column does.

**Spacing.** A `gap` or `padding` arrives as a spacing step, 0 to 5 (`ui::spacingStep`), and the container maps it to
cells by axis with `layout::stepCells`. A cell is about twice as tall as it is wide, so a step is about half as many
rows as columns:

| Step | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| Rows (vertical) | 0 | 0 | 0 | 1 | 1 | 2 |
| Columns (horizontal) | 0 | 1 | 1 | 2 | 2 | 3 |

A stack's gap is along its axis; a grid has a column gap and a row gap from the same step; a panel's padding is rows at
its top and bottom and columns at its sides.

## Focus, dialogs and drawers

A dialog's or a drawer's box is an overlay shown while it is open and the user could see it. The box on top is modal:
Tab cycles inside it, and neither the pointer nor a key reaches anything behind it. Opening moves the focus inside;
closing hands it back to the widget that had it. Esc inside the open box closes it, and then calls `onDismiss`, as a
toolkit's dismissal does: an application that keeps it open opens it again. A description is drawn muted on the
first row inside the frame, under the title in the border, then a `─` rule row, and the content is laid out below,
two rows lower; it is one row, clipped at the frame's width like any terminal text, and the box's
`accessibleDescription()`.

## Keys and chords

A key goes to the focused view and bubbles to its ancestors until one handles it, in the order every backend follows
([`backend_contract.md`](../ui/backend_contract.md), Chords). A widget's own chords come first: `WidgetBase::dispatch`,
a field's view and a list's view each try the widget's chords before its own handling of the key. Then the widget's
own handling, and a key it leaves bubbles on to the next ancestor, which tries its chords the same way. The innermost
widget declaring a chord therefore takes it, and a hidden or disabled widget, which takes no key, runs none. A handler
may destroy the widget, so a handled key returns at once; a text input that commits on Enter without submitting lets
the key bubble only while it is still alive.

A chord is modifiers (`Ctrl` or `Control`, `Alt`, `Shift`, `Super`, `Meta` or `Cmd`) joined by `+` to one character
or a named key (`Enter`, `Tab`, `Escape`, `Space`, `Backspace`, `Delete`, `Insert`, `Home`, `End`, `PageUp`,
`PageDown`, the arrows, `F1`–`F12`). Names and letters match without regard to case, the modifiers must be exactly
those pressed, and the lock keys are not modifiers. A capital letter without a reported Shift counts as Shift and
the letter, since a terminal speaking no keyboard protocol reports Shift+S as `S`.

## Pointer and drag

Mouse reports are SGR, 1-based; a press handled by a widget makes it core::tui's pointer capture target, so the moves
and the release reach it wherever the pointer is. A widget with a drag key arms a drag on a press; the first move a
cell away starts it and shows a label; the drop target is the first widget from the one under the pointer outwards
that the user can reach and that accepts the key, outlined while the pointer is over it. The release drops the key
there. Esc ends a drag with no drop. A terminal without mouse reporting ignores `drag` and `drop`.

## Field state

A field shows its state as marks at the right edge of its control's first row: `*` required, `!` with errors, `~`
stale; a field toned `err` or `warn` adds `!` too, since the terminal draws no outline. Its description takes the
rows under the control, muted or in the tone's style, one per line and clipped at the field's width; the messages,
the description and a text input's unit are its `accessibleDescription()`, and its hover text follows the tooltip with them. A
captioned field draws its `*` after its caption instead, as ` *` in the error style, and its caption's width
counts those two cells; a required text draws ` *` after its last line's end, and its natural width counts them, so
in a row it is not drawn over the text's last characters (given less, it is drawn at the right edge). The
messages show on hover, after the tooltip. A read-only field takes the focus and moves its cursor and selection, but
any edit is undone and nothing is reported: no change, submit, commit, toggle or choice. A field draws no input
fill, so a read-only one reads as its text in place, in the normal text colour where a disabled one is muted.

## The conformance probe

`tests/tui/test_tui_conformance.cpp` runs every case of `ui::testing::conformanceCases()` on the terminal backend
with a probe that acts as a user does, through `core::tui` events dispatched to the screen:

- a click is a press and a release on the first cell the widget was drawn at; a widget never drawn has no cell, and
  is not clicked;
- typing selects all (Ctrl+Shift+A) and pastes the text, one edit, as a user pasting does;
- a drag is a press on the source, moves, and the release on the target;
- a dismissal is Esc; a table's selection is the cursor moved to each row and Space; a menu entry is a click on each
  level's row, after Backspace back to the top; files are their quoted paths pasted to the focused drop zone.

It reads back what the widgets show. Run with a probe that drops every input or misreads every text, the suite fails
the cases that check them, so its passes say something about the terminal backend.

## Design decisions

- **Retained widgets over core::tui components.** Each widget owns its view, and the containers lay their children
  out in cells. `core::tui` holds raw pointers to components (children, focus, overlays, the pointer capture); a
  widget clears every one of them before it is destroyed.
- **One thread.** The loop is caller-driven, so input, timers, sockets and bridge callbacks need no locks.
- **The shell factory receives the backend.** `ui::AppContext` names no renderer, since a frontend that generates its
  views has none to lend. A terminal renders through retained widgets, so its shell factory is given them.
- **Best effort.** What a terminal cannot show (icons, pixel sizes, OS file drops) is kept or mapped to what it can.
