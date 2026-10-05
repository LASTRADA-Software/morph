# Backend contract — design

Design spec for `include/morph/ui/backend.hpp` — the retained widgets a mount drives and the
`IViewBackend` that makes them — and for the two test instruments that fix the contract:
`ui::testing::RecordingBackend` (`include/morph/ui/testing/recording_backend.hpp`) and the
conformance cases (`include/morph/ui/testing/backend_conformance.hpp`).

Read this before implementing a backend for a frontend, and before changing a widget interface.
What the mount does with these widgets is [`view_tree.md`](view_tree.md).

## Contents

- [Widgets](#widgets)
- [Handlers](#handlers)
- [Containers](#containers)
- [Interfaces](#interfaces)
- [Selection by key](#selection-by-key)
- [Drag and drop](#drag-and-drop)
- [RecordingBackend](#recordingbackend)
- [The conformance suite](#the-conformance-suite)
- [Design decisions](#design-decisions)

## Widgets

- `IViewBackend` has one factory per kind. Each makes the widget, appends it as the last child of
  `parent` — or makes a root when `parent` is null — and never returns null. A kind's fixed
  parameters (a TextInput's mode, a stack's or scroll area's axis, a DateTimeInput's mode and
  display zone, a Select's style, a FilePicker's mode) are factory arguments.
- **A new widget is visible, enabled, content-sized, not draggable and not a drop target. A new
  dialog is closed.** The mount calls the `Widget` setters for those defaults (`setVisible`,
  `setEnabled`, `setLayout`, `setDragKey`, `setDropHandler`) only to change them; it calls a
  dialog's `setOpen` at mount whatever the value, `false` included.
- Every setter takes UTF-8 and may be called with the value the widget already has.
- **No setter calls a handler.** Setting a value, replacing options, items or rows, requesting a
  selection, opening or closing, collapsing — none of it is a user's doing, so none of it reaches
  `onChange`, `onToggle`, `onSelect`, `onSelectionChange` or any other handler. That covers what a
  widget does by itself in response to a setter: a Select re-marking its requested key after
  `setOptions`, a Table marking or unmarking a row that arrives or goes. A binding that writes back
  what the user typed therefore cannot loop.
- `setVisible(false)` hides the widget and everything inside it; `setEnabled(false)` stops input to
  the widget and everything inside it. **A widget inside a hidden, disabled or collapsed container,
  or inside a closed dialog, takes no input**, however deep it sits; it takes input again once
  every container around it is shown, enabled, expanded and open. A collapsed panel's own collapse
  control still works.
- A widget's destructor detaches it from its parent and frees its native resources. The mount
  destroys children before parents.

## Handlers

- Handlers are handed over once (`setOnClick`, `setOnChange`, …); an empty one does nothing. The
  mount wraps each in `Runtime::widgetEvent`, so a backend calls them directly from its native
  event handler.
- **A handler may destroy its own widget.** Every backend keeps a handler alive for the duration
  of its own call — for example by calling a copy of it — as `RecordingBackend` does. A backend
  that calls the stored `std::function` in place frees the running closure when the handler
  destroys its widget.
- **No handler runs after its widget's destructor returns**, even for input that arrived before.
  A backend whose input can be delivered later than the event that caused it (a queued connection)
  drops what is still pending for a destroyed widget.
- A backend may assume a handler never throws, a drop's `accepts` predicate included: the mount
  catches and reports what one throws ([`view_tree.md`, "Callbacks"](view_tree.md#callbacks)).

## Containers

`ContainerWidget::moveChild(child, index)` removes `child` and inserts it before the child now at
`index`; an index past the end means last. **The child is moved, not recreated**: it keeps its
native state — properties, handlers, focus, selection, the text a user typed into it. A backend
whose toolkit can only remove and re-add a child must carry that state across. Only the mount
reorders children, and only through this.

| Container | Children |
|---|---|
| `StackWidget` | In order along the axis fixed at creation, `setGap` apart |
| `GridWidget` | Row-major in `setColumns` columns, `setGap` apart; `setSpan(child, n)` widens one, and a child spans 1 until told |
| `PanelWidget` | Inside the frame, `setPadding` in; a collapsed panel hides its content, which takes no input |
| `ScrollWidget` | One scrolled viewport, on the axis fixed at creation, that keeps the focused child visible |
| `SlotWidget` | Shown as they are; hosts a Switch case or a Tabs page |
| `TabsWidget` | Page slots in the order first shown; the mount keeps exactly the selected page's slot visible, and `setSelected` moves the bar's highlight only |
| `DialogWidget` | The overlay's content; a focus trap while open; a closed dialog shows nothing and takes no input, its children included |
| `TableWidget` | Rows, each a horizontal stack of cells in column order; `setRowKey(row, key)` once per row, right after it is built. A hidden cell keeps its column: the cells after it stay under their own headers |

## Interfaces

| Interface | Setters | Handlers |
|---|---|---|
| `Widget` | `setVisible`, `setEnabled`, `setLayout(LayoutHints)`, `setDragKey(optional<Key>)` | `setDropHandler(accepts, onDrop)` |
| `ContainerWidget` | `moveChild(child, index)` | — |
| `TextWidget` | `setText`, `setRole` | — |
| `ButtonWidget` | `setLabel` | `setOnClick` (Enter, Space or a click) |
| `TextInputWidget` | `setText`, `setPlaceholder` | `setOnChange` (the whole new text after each user edit), `setOnSubmit` (Enter in single-line mode) |
| `CheckboxWidget` | `setLabel`, `setChecked` | `setOnToggle(bool)` |
| `SelectWidget` | `setOptions`, `setSelected(optional<Key>)` | `setOnSelect(Key)` |
| `MenuWidget` | `setItems(labels)` | `setOnActivate(index)` |
| `StackWidget` | `setGap` | — |
| `GridWidget` | `setColumns` (at least one), `setGap`, `setSpan(child, span)` | — |
| `SpacerWidget` | — | — |
| `PanelWidget` | `setTitle`, `setPadding`, `setCollapsible`, `setCollapsed` | `setOnToggle(collapsed)`, with the state the user asked for |
| `ScrollWidget`, `SlotWidget` | — | — |
| `TabsWidget` | `setTabs(labels)`, `setSelected(index)` (past the last highlights none) | `setOnSelect(index)` |
| `DialogWidget` | `setOpen`, `setTitle` | `setOnDismiss` (Esc on the TUI), for an open dialog |
| `BusyWidget` | `setActive`, `setLabel` | — |
| `TableWidget` | `setColumns(vector<TableColumn>)`, `setSelectionMode`, `setRowKey(row, key)`, `setSelection(keys)` | `setOnSelectionChange(keys)`, `setOnActivate(Key)` (Enter or a double click) |
| `DateTimeInputWidget` | `setValue(optional<Timestamp>)` (`nullopt` or an empty `Timestamp` shows an empty field) | `setOnChange(optional<Timestamp>)` (`nullopt` for a cleared field) |
| `SliderWidget` | `setRange(minimum, maximum, step)`, `setValue` | `setOnChange(value)` |
| `FilePickerWidget` | `setPath` | `setOnPicked(path)` |

## Selection by key

Both selecting widgets keep what was **requested** apart from what they **mark**, so a value and
the list it refers to may arrive in either order.

- **Select.** The widget keeps the key last requested by `setSelected` or chosen by the user, and
  marks the option with that key whenever the current options contain one. A key outside the
  options marks none and stays requested: options that bring it back mark it again.
- **Table.** The widget keeps the keys last requested by `setSelection` or selected by the user, and
  marks the current rows that have one of them. A key with no row marks nothing and stays
  requested, so a row that arrives with it later is marked, a row that goes is unmarked, and a
  reorder keeps the marks. The selection mode limits what the user can select, not what
  `setSelection` marks.
- A Table shows the user's selection whether or not the application binds `selection`. A user's
  selection replaces the requested keys, and `onSelectionChange` then gets exactly the keys of the
  existing rows now selected, in any mode: a key still waiting for its row is dropped by a user's
  change, while `setSelection` keeps it waiting. A backend may report each step of the user's
  picking (a click, then a Ctrl-click); the last report is the whole selection, and no report names
  a key without a row.

## Drag and drop

A widget with a drag key can be dragged; a widget with a drop handler is a target. While dragging,
the backend asks the target's `accepts(key)` before highlighting it and before a drop. A drop it
refuses delivers nothing; an accepted one calls `onDrop(key)` once. The mount always passes a
predicate — an accept-everything one when the node's `accepts` is empty — so a backend never sees
an empty one. A source or target the user cannot reach (hidden, disabled, or inside such a
container) takes part in no drag.

## RecordingBackend

The headless reference backend and the test double for everything above the contract.

- **Ids and kinds.** Widget ids are creation numbers from 1. A stack's kind is `Column` or `Row` by
  its axis; every other kind is the interface name without `Widget` (`Text`, `Slot`, `Table`, …).
- **Operation log** (`log()`, `clearLog()`), one line per operation, oldest first:
  `create Kind#id in Parent#id` (`in root` for a root), `set Kind#id name=value`,
  `move Kind#id to n`, `destroy Kind#id`. Factory parameters are recorded as properties without a
  log line. Handlers are stored without a log line, except that `setDropHandler` records the
  property `drop=handler`. What a widget changes by itself — an interaction helper's effect, a
  Select or Table re-marking its selection — changes a property without a log line.
- **Golden dump** (`dump()`): one line per live widget, `Kind#id name=value …`, properties in name
  order, children indented two spaces per depth, every line ending in a newline. A child whose
  parent was destroyed first becomes a root.
- **Properties.** Every kind records `visible`, `enabled`, `layout`, `dragKey` and `drop` when their
  setters run, and its own: `text`, `role` (Text); `label` (Button); `mode`, `text`, `placeholder`
  (TextInput); `label`, `checked` (Checkbox); `style`, `options`, `selected` (Select, where
  `selected` is the option it marks, else `none`); `items` (Menu); `gap` (Column, Row); `columns`,
  `gap`, and `span` on a child (Grid); `title`, `padding`, `collapsible`, `collapsed` (Panel);
  `axis` (Scroll); `tabs`, `selected` (Tabs); `open`, `title` (Dialog); `active`, `label` (Busy);
  `columns`, `selectionMode`, `selection` (the requested keys it marks, in the order requested),
  and `rowKey` on a row (Table); `mode`, `offset`, `value` (DateTimeInput); `range`, `value`
  (Slider); `mode`, `path` (FilePicker).
- **Values.** `true`/`false`; an enumerator by its name; a key as a decimal integer or a
  double-quoted string (`7` and `"7"` differ); `none` for an absent key or date; a date as an
  ISO-8601 UTC instant with milliseconds, `empty` for an empty `Timestamp`; sizing as `content`,
  `fixed(n)` or `stretch(n)`, a layout as `width/height`; lists as `[a,b]`; options as
  `[key:label,…]`; columns as `[label:sizing,…]`; a slider's range as `minimum..maximum/step`.
- **Escaping.** Free text is escaped so that every value stays on its line and reads as one field:
  a line feed becomes `\n`, a carriage return `\r`, and a backslash or `=` is preceded by a
  backslash. Text inside a list also escapes `,`, `[` and `]`, and a string key also escapes `"`.
- **Lookups.** `find(kind, name, value)` is the first live widget, by id, of that kind whose
  property equals `value`, compared against the escaped form; `all(kind)` lists them in creation
  order; `prop(id, name)` is the escaped value, or empty when never set, and `hasProp(id, name)`
  tells the two apart; `exists(id)`; `idOf(widget)`, `kindOf(id)`, `children(id)` and
  `widget(id)`. `prop`, `hasProp`, `kindOf`, `children` and `widget` throw `std::out_of_range`
  for an id that names no live widget, where `exists` returns false; `idOf` throws it for a widget
  that is not one of this backend's live widgets.
- **Interaction helpers** act as the user would: `click`, `edit`, `submit`, `toggle`, `choose`,
  `chooseIndex`, `collapse`, `dismiss`, `selectRows`, `activateRow`, `setDateTime`, `slide`, `pick`
  and `drag`. A helper first changes what the native widget would change by itself — the field's
  text, the box's check mark, the marked option or rows, the highlighted tab, the collapsed state,
  the value or path — and then calls the stored handler. It calls a copy of the handler, so a handler may
  destroy its own widget.
  - A widget the user cannot reach ignores every helper: one that is hidden or disabled, or inside a
    container that is hidden, disabled or collapsed, or a closed dialog or anything inside one.
    `activateRow` asks that of the row, `drag` of both widgets.
  - A helper used on a kind it does not apply to throws `std::logic_error`, and so does an action
    no user could take: choosing a key the Select does not offer, collapsing a panel that is not
    collapsible, selecting a key with no row, a row twice, any row in a `None` table or more than
    one in a `Single` one, activating a key with no row, dragging a widget onto itself. That is a
    mistake in the test.
  - `drag(source, target)` returns true when the source has a drag key, the target a drop handler,
    and the target's predicate accepted the key; then `onDrop` has run. An empty predicate accepts
    nothing here, so a drop that lands on a mounted target without `accepts` shows the mount's
    accept-everything default at work.

`tests/test_ui_recording_backend.cpp` tests the log, the dump, the lookups and the helpers.

## The conformance suite

`conformanceCases()` returns every case, in a fixed order, as `ConformanceCase{name, run}`; each
`run` takes a `ConformanceProbe` and returns `nullopt` for a pass or the failure message. A
backend's test suite runs every case on a fresh probe. `RecordingBackend` passes them in
`tests/test_ui_conformance.cpp`.

### The probe

A backend author implements one `ConformanceProbe`. Its driving operations act as a user would,
through the backend's own input path where it has one, and each may run pending work before and
after it; its reading operations report what the widget shows now.

| Operation | Meaning |
|---|---|
| `backend()` | The backend under test |
| `runtime()` | The runtime the cases' signals and mounts belong to |
| `settle()` | Runs posted flushes and pending native events until nothing is left |
| `textOf(widget)` | A Text's or TextInput's text, a Button's or Checkbox's label, the label of the option a Select marks (empty when it marks none) |
| `visibleOf(widget)` | The widget's own flag as its last `setVisible` left it: a hidden ancestor, a collapsed panel or a closed dialog around it does not make it false |
| `enabledOf(widget)` | The widget's own flag as its last `setEnabled` left it |
| `childCount(container)` | How many children a container has |
| `childAt(container, index)` | A child, or null past the end; a case drives it as it drives a root |
| `click(widget)` | Activates a button |
| `type(widget, text)` | Replaces a field's text as typing would: the field is cleared first, so it ends up holding exactly `text`; `onChange` may see intermediate texts |
| `drag(source, target)` | Drags one widget onto another |
| `dismiss(dialog)` | Dismisses a dialog (Esc on the TUI) |
| `selectRows(table, rows)` | Selects rows by position, in the order the user picks them — at least one, and only one in a `Single` table — so the user's selection is exactly those rows |
| `selectedRows(table)` | The positions of the rows the table shows as selected, ascending |

A case reaches widgets only through `Mounted::root()`, `childAt` and further roots it mounts, or
through widgets it makes with `backend()` itself, so a probe needs no lookup by name.

### The cases

Each case is named by the rule it checks:

1. A Text shows its constant text.
2. A bound Text updates once per batch, and not for an equal write.
3. `visible` and `enabled` follow their bindings.
4. A click runs the button's action once, and what it wrote reaches the screen.
5. Typing reaches `onChange`, and a value set by the application is shown and not echoed.
6. A Switch shows the selected case, and nothing for a key without one.
7. Tabs mount a page on its first selection, keep it, and show only the selected one.
8. A ForEach updates a kept row in place: the same widget, its view not built again.
9. A ForEach follows inserts, removals and reorders.
10. A moved widget keeps its native state: the text a user typed into it.
11. A Dialog holds its content only while open.
12. A new Dialog is closed, and a closed Dialog takes no input — neither a dismissal nor a click on
    a child.
13. A widget inside a hidden, disabled or collapsed container takes no click, typing, drag or row
    selection, with each gate at least two levels above the widget, and takes them again once the
    gate is lifted.
14. No setter calls a handler: values, options, menu items, rows, selections, tab indices, opening
    and collapsing, including a Select re-resolving its request after new options and a Table
    re-marking rows that arrive or go.
15. A Select marks the requested key whenever its options contain it, whichever arrives first, and
    remembers it across options that lack it.
16. A Table in `Multiple` mode reports a user's change as exactly the existing rows selected.
17. A Table shows a user's selection with no selection bound.
18. A hidden Table cell keeps its column in its row.
19. No handler runs after its widget was destroyed.
20. A handler may destroy its own widget; the backend keeps the running handler alive and stays
    usable afterwards.
21. A drag onto an accepting target delivers the key once.
22. A drag onto a refusing target delivers nothing.
23. A drop target without `accepts` takes every key.

### What the probe cannot reach

The probe drives clicks, typing, drags, dismissals and row selection only, so some rules of this
contract are each backend's own tests to make:

- that a widget the user cannot reach takes no `choose`, `toggle` or row activation, and that a
  drag onto an unreachable target delivers nothing;
- that rules 19 and 20 hold for handler kinds other than a click;
- that a hidden Table cell's neighbours are drawn under their own headers — rule 18 checks the
  row's structure only.

Rule 19 is observable only for a backend whose input can be delivered later than the driving call,
such as a queued connection; a probe whose `click` delivers before it returns passes it without
exercising it.

## Design decisions

| Decision | Why |
|---|---|
| Typed factories, one interface per kind | A backend implements exactly what each node needs; a missing setter is a compile error, not a runtime lookup. |
| Setters never call handlers | Two-way bindings (a field bound to the signal its `onChange` writes) cannot loop, on any toolkit. |
| Defaults stated by the contract | The mount skips the setters that would only restate them, which keeps logs and native call counts to what a node actually says. |
| Selection kept as a request, marked against what exists | The value and the list it names come from different bindings, in either order; neither order may lose the selection. |
| The backend keeps a running handler alive | A button that closes its own dialog or removes its own row is ordinary UI, not an edge case. |
| A recording backend as the reference | Every mount behaviour is asserted as a log or a dump without a toolkit, and the real backends are held to the same cases through the probe. |
