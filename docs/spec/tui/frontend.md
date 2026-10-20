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

## Widgets

Every `ui::IViewBackend` factory makes a widget whose view is a `core::tui::Component` it owns.

| Widget | Shows | Input |
|---|---|---|
| Text | one row per line, styled by role | — |
| Button | `[ label ]`, a framework icon's glyph before the label | Enter, Space, click |
| TextInput | `core::tui::InputField`; single-line, multiline or masked; a unit is drawn muted at the right edge before the field marks, and the editable width ends one cell before it | typing; Enter submits a single-line field after its commit; the focus leaving after an edit commits |
| Checkbox | `[x] label` | Enter, Space, click |
| Select | radio: a row per option, `(•)` marking; dropdown: `[label ▾]` opening a list overlay | Enter, Space, click; arrows in the list |
| Menu | one level of entries as a list, `>` on an entry with a submenu, the mark of a checkable one, its chord | Right, Enter, Space or a click opens a submenu; Left or Backspace goes up; choosing a leaf shows the top again |
| Stack, Slot | children along an axis, a spacing step apart | — |
| Grid | equal columns, cells row-major, spans | — |
| Spacer | nothing; stretches in a stack | — |
| Panel | a box with a title; a collapsible one folds to its title line | Enter, Space, click on the title |
| Scroll | a viewport that follows the focus | arrows, PageUp/PageDown, Home/End, wheel |
| Tabs | a one-row bar over the page slots, with no blank row under it (a terminal row costs more than the separation earns); a hidden bar gives its row to the pages and takes no focus | Left, Right, click on a label |
| Dialog | a centred box over everything; its footer is the box's bottom row, its children against the right edge, one cell apart, and stays last when the content mounts again | Esc dismisses |
| Drawer | a box against its side, the screen's height; its footer as a dialog's | Esc dismisses |
| Busy | a spinner and its label while active | — |
| Table | a header and keyed rows, `*` on selected rows, `>` on the cursor; a toned row's cells draw in the tone's style, keeping any inverse | arrows, Space selects or toggles, Enter activates, click and double click |
| DateTimeInput | `YYYY-MM-DD[ HH:MM]` in the display zone | typing; Up/Down step |
| Slider | `[====\|----] value` | Left, Right, Home, End, press and drag on the track |
| FilePicker | a path field, prompted `Open: ` or `Save: ` | typing; Enter |
| Banner | its text in its tone, `[ action ]`, `[x]` when dismissible | Enter runs the action; Esc or Delete dismisses; a click on either |
| Badge | `[text]` in its tone, a framework icon's glyph before the text | — |
| Progress | `[####......] 40%` and the label; `<=>` when indeterminate | — |
| Steps | a row per step: `[ ]` pending, `[>]` active, `[x]` done, `[!]` failed; the current one accented | — |
| KeyValue | captions, then the values lined up after the widest | — |
| EmptyState | the title, the explanation, `[ action ]` | Enter, click on the action |
| Splitter | the panes and a handle between each two | dragging a handle; the release reports every size |
| Collapsible | `▾ title` with the header beside it, the body below while open | Enter, Space, click on the title; the header stays live while closed |
| DropZone | a dashed frame around its children | a paste while it has the focus is a drop (see below) |

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

A cell-based solver. A stack gives Fixed children their amount, Content children their natural extent, and shares
what is left among Stretch children by weight; an overflow is taken from the last child first. A grid places cells
row-major in equal columns. Several roots share the screen as a column, in the order they were made: each takes the
height it asks for, and the last shown one what is left; a single root takes the whole screen.

Across a stack's axis, a Content or Stretch child fills the stack's extent and a Fixed child gets its amount, at most
the space there is (`layout::crossExtent`): in a column every content child is as wide as the column, in a row as tall
as the row. A grid cell, and the one child of a panel, slot or scroll area, fills its width the same way.

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
toolkit's dismissal does: an application that keeps it open opens it again.

## Keys and chords

A key goes to the focused view and bubbles to its ancestors until one handles it. A widget's own key handling comes
first; a key it leaves goes to the widget's chords, and on to the next ancestor. The innermost widget declaring a
chord therefore takes it, and a hidden or disabled widget, which takes no key, runs none.

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

A field shows its state as marks at the right edge of its first row: `*` required, `!` with errors, `~` stale. The
messages show on hover, after the tooltip. A read-only field takes the focus and moves its cursor and selection, but
any edit is undone and nothing is reported: no change, submit, commit, toggle or choice.

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
