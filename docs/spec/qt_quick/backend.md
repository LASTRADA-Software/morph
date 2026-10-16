# Qt Quick backend — design

Design spec for `morph::qt_quick::Backend` (`include/morph/qt_quick/backend.hpp`) and
`morph::qt_quick::Components` (`include/morph/qt_quick/components.hpp`): the view tree's widgets as Qt Quick items,
and the application's own QML components beside them. The code is in `src/qt_quick/`; the QML is the private module
`MorphUi` (`src/qt_quick/qml/`).

Read this before changing a `MorphUi` component or a widget class. The contract the backend implements is
[`../ui/backend_contract.md`](../ui/backend_contract.md); what mounts on it is [`../ui/view_tree.md`](../ui/view_tree.md);
the frontend around it is [`frontend.md`](frontend.md).

## Contents

- [Shape](#shape)
- [Packaging](#packaging)
- [Widgets](#widgets)
- [User actions](#user-actions)
- [Kinds](#kinds)
- [Containers and lists](#containers-and-lists)
- [Keys](#keys)
- [Layout and theme](#layout-and-theme)
- [Components](#components)
- [Tests](#tests)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## Shape

```cpp
QQmlEngine engine;
QQuickWindow window;
qt_quick::Components components{[](qt_quick::ComponentFailure const& failure) { /* report */ }};
qt_quick::Backend backend{engine, *window.contentItem(), {.components = &components}};
auto screen = interpreter.mount(document, params, backend);   // or ui::Mounted{runtime, backend, view}
```

- `Backend(engine, host, options)` implements `ui::IViewBackend`. Every root a mount makes is appended to one column
  the backend adds to `host`, top to bottom. The backend belongs to its engine's thread, and every widget it makes is
  destroyed before it.
- `Backend::itemOf(widget)` is the `QQuickItem` a widget shows, for a host that embeds a screen or a test that drives
  one; null for a widget another backend made.
- `BackendOptions::components` is the restyle map and the custom components; `onWidthClass` is called with the host's
  width class (`widthClassOf`: below 600 logical pixels `compact`, below 1200 `medium`, else `expanded`) once at
  construction and each time the class changes, so the shell can set `{"env": "widthClass"}`.

## Packaging

`morph::qt_quick` is a compiled STATIC library behind `MORPH_BUILD_QT_QUICK` (default off). It needs `MORPH_BUILD_QT`,
for `QtExecutor`, and Qt 6.5+ `Quick`, `QuickControls2` and `Qml`; configure fails naming the missing option. The QML
is the static module `MorphUi` (`qt_add_qml_module`, no plugin, no C++ types) in a target of its own, so morph's
warning set does not reach Qt's generated sources; its resource objects are linked into every executable that links
`morph::qt_quick`, and the backend loads each component with `QQmlComponent::loadFromModule("MorphUi", …)`.

The public headers hold declarations only, so `include/morph/qt_quick` has no branch-coverage records and no floor
in `scripts/check_branch_coverage.py`. It installs and exports as component `qt_quick`, with the module's object
libraries; `morphConfig.cmake` finds Qt 6.5 `Core Gui Qml Quick QuickControls2` for it.

## Widgets

Each widget class (`src/qt_quick/kinds.hpp`) owns one item made from its kind's component, and:

- **writes what the item shows.** A setter writes a property of the item, or of the control inside it, and does
  nothing when the item already shows the value, so a re-asserted text keeps its cursor and its input-method
  composition. State the user can change — a field's text, a check mark, a marked option, a slider's value, an open
  overlay — is read back from the control itself, never from a property C++ last wrote, because the user may have
  changed it since.
- **is reached through an `ItemBase`.** The common properties of `ui::Widget` (visibility, enablement, layout hints,
  drag key, drop target, accessible name and role, test id as the item's `objectName`, tooltip, surface, chords,
  focus) are one implementation for every kind; `FieldItemWidget` adds `readOnly`, `required`, `errors` and `stale`.
- **leaves the scene when destroyed.** The destructor releases the widget's handlers at once, takes the item out of
  its container, hides it and deletes it later: QML may be running one of the item's own handlers, and the handlers
  hold the screen's scopes, which must not outlive it.
- **is one accessible node.** The node assistive technology and an automation driver see carries the document's
  `a11y` name and role, falling back to the kind's own text and role, and the test id as both `objectName` and, on
  a Qt that has the property, `Accessible.id`. A role the document names maps to Qt's: `group` Grouping, `heading`
  Heading, `text` StaticText, `button` Button, `edit` (or `textbox`) EditableText, `checkbox` CheckBox, `list`
  List, `table` Table, `chart` Chart, `status` StatusBar, and the further names `widget.cpp` lists; an unknown name
  keeps the kind's role. For most kinds the node is the widget's item. A button is a Button named by its label,
  with the press action, and a text is StaticText named by its text; the control inside either is
  `Accessible.ignored`. A key-value list is one node per pair, named `"<caption>: <value>"`.
- **an input's node is its control.** For a text input, text area, date input, select, check box, slider and file
  picker the node is the Qt Quick Controls control a driver focuses and types into or sets, because its text or
  value interface and its focus live there; the widget's item names it in `accessibleItem` and is
  `Accessible.ignored`. The control binds the item's `accessibleName`, `accessibleRole` and its messages as
  `Accessible.description`, and `showTestId` writes the test id onto it as well as onto the item, whose
  `objectName` `findWidget` matches. So `(name, role)` and the test id resolve to the same single node. Kinds' own
  roles are EditableText for the text kinds and the file picker's path, ComboBox for a drop-down select, Grouping
  for a radio select, CheckBox and Slider. A drop-down the document names no name reads the option it shows; a
  check box reads its label. A radio select's group carries the name, and each option is a RadioButton named by its
  label, with the press action. A select's hidden presentation is ignored, because a hidden item stays in the
  accessibility tree. The file picker's button is a Button node of its own. Each control declares its name and
  role itself: Qt Quick Controls makes a control an accessible node of its own accord only while the platform's
  accessibility is active, and on a platform with no accessibility bridge, such as `offscreen` on Qt 6.8, it never
  is.
- **takes no focus out of reach.** `focus()` on a widget that is hidden or disabled, or inside such a container,
  does nothing; a widget that becomes hidden while focused gives its focus up.

## User actions

QML reports what the user did through one relay per widget: the component calls `host.fire(event, payload)`, or
`host.ask(question, payload)` when it needs an answer (whether a drop target takes a key, whether a chord is
declared). The relay calls a copy of the handler registered under that name, so a handler may destroy its own
widget; once the widget is destroyed the relay calls nothing, even for input QML delivers later; a handler that
throws is logged and goes no further. Only the user's own actions fire: Qt Quick's `textEdited`, `toggled`,
`activated`, `moved` and `clicked` signals, never a property change a setter made.

## Kinds

| Kind | Component | Shows | Reports |
|---|---|---|---|
| Text | `MorphText` | a label in a text role | — |
| Button | `MorphButton` | a `Button` | click, Space or Enter |
| TextInput | `MorphTextInput` (single line, password), `MorphTextArea` (multi-line) | a `TextField` / `TextArea` | each edit's whole text; Enter commits then submits (single line); focus leaving after an edit commits |
| Checkbox | `MorphCheckbox` | a `CheckBox` | the state toggled to |
| Select | `MorphSelect` | a `ComboBox`, or `RadioButton`s | the chosen option's index, which C++ turns into its key |
| Menu | `MorphMenu` | a bar of top-level entries; a submenu opens in a popup, one level at a time | the chosen entry's path |
| Column, Row | `MorphStack` | a `GridLayout` flowing on the axis | — |
| Grid | `MorphGrid` | a `GridLayout`; a child's span is its `Layout.columnSpan` | — |
| Spacer | `MorphSpacer` | space that fills the layout | — |
| Panel | `MorphPanel` | a framed region with a title and padding; collapsible, with a header button | the collapsed state asked for |
| Scroll | `MorphScroll` | a `Flickable` that scrolls the focused item into view | — |
| Slot | `MorphSlot` | its children | — |
| Tabs | `MorphTabs` | a `TabBar` over the page slots | the tab clicked |
| Dialog | `MorphDialog` | a modal `Popup`, centred | Escape or a press outside, as a dismissal |
| Drawer | `MorphDrawer` | a modal `Drawer` on the start or end edge; it cannot be swiped open | as a dialog |
| Busy | `MorphBusy` | a `BusyIndicator` and a label | — |
| Table | `MorphTable`, rows `MorphTableRow` | column headers over rows | selection and activation, below |
| DateTimeInput | `MorphDateTimeInput` | `yyyy-MM-dd` or `yyyy-MM-dd HH:mm` in the display zone | the instant typed, once the user finishes; empty is cleared |
| Slider | `MorphSlider` | an integer `Slider` | the rounded value moved to |
| FilePicker | `MorphFilePicker` | a path and a button opening Qt Quick Dialogs' `FileDialog` | the local path picked |
| Banner, Badge, Progress, Steps, KeyValue, EmptyState | the component of that name | as the backend contract says | a banner's action and dismissal, an empty state's action |
| Collapsible | `MorphCollapsible` | a toggle with the title, the header beside it, the content below while open | the open state asked for |
| DropZone | `MorphDropZone` | a framed `DropArea` for files | the local paths of a drop it took |

- **Controlled inputs** behave as the contract says: the control shows the user's action at once, as Qt Quick does,
  and the mount re-asserts the document's value in a later turn. A read-only check box, select or slider takes
  focus and puts its own state back when the user acts; a read-only text field reports no edit, commit or submit.
- **Dialogs and drawers** open and close without a transition, so what the document says is what the window shows.
  A close by `setOpen(false)` reports nothing; Escape or a press outside closes the overlay first and then reports
  the dismissal.
- **A date-time field** that the user leaves holding text that names no instant reports nothing and shows again what
  it showed before.

## Containers and lists

A container keeps its children in the mount's order in C++ and appends each child's item to its content item. The
layouts follow the items' stacking order, so `moveChild` is one `stackBefore`/`stackAfter` of the moved item: the item
is moved, never made again, and keeps its focus, its selection and the text a user typed into it. A split view takes
and orders its panes through its own container API (`addItem`, `moveItem`, `takeItem`).

A `forEach` or a `table` is such a container, reconciled by the mount: a list change reaches the backend as the
mount's keyed operations — an append, a destroy, and the fewest `moveChild` calls (n minus a longest run already in
order) — and each of those is one operation on the Qt side. Nothing re-creates or re-orders a kept row's item.

**Tables.** A horizontal stack the mount makes inside a table is one of its rows (`MorphTableRow`). A row places its
cells by column index at the table's column widths — fixed units, the widest of the header and the column's shown cells
for content (as the terminal backend sizes it), a share of the rest for stretch — so a hidden cell keeps its column
and the cells after it stay under their headers; a cell's implicit size changing widens its content column and places
the rows again. A click selects the row, Ctrl+click toggles it in `Multiple` mode, a double
click or Enter activates it without toggling the selection. Selection is kept as the requested keys, marked on the
rows that have them, as the contract's "Selection by key" says; a user's change reports exactly the existing rows
selected.

## Keys

A `ui::Key` crosses into QML as text — `i:<int64>` or `s:<text>` — and back through `decodeKey`, never as a JavaScript
number, so an id above 2^53 is exact: a drag key, a table row's key, and an option's identity (QML sees only the
option's index and label; the keys stay in C++).

**Chords** are matched in C++: QML hands the key and the modifiers of a key press to the widget's relay, and the
first declared chord they press runs. A chord is modifiers (`Ctrl`, `Shift`, `Alt`, `Meta`) joined to a key name by
`+`: a letter or a digit, `F1` to `F24`, or `Enter` (either Return key), `Esc` or `Escape`, `Tab`, `Up`, `Down`,
`Left`, `Right`, `Space`, `Delete`. Letters match without regard to case; the modifiers must be exactly those
pressed. A key press propagates from the focused item outwards, so the innermost widget declaring the chord takes
it; a widget out of reach takes none.

## Layout and theme

A `MorphWidget`'s sizing maps to `QtQuick.Layouts`: content to the implicit size, `fixed(n)` to a preferred size of
`n` units, `stretch(w)` to fill with stretch factor `w`. A unit is the default font's average character width
horizontally and its line height vertically (`Theme.unitWidth`, `Theme.unitHeight`). A stale field is dimmed; a
field's errors show under it in the error colour; a required field is marked. The `Theme` singleton holds the colour
roles every component reads, with the `ui-theme/1` defaults; `applyTheme(engine, values)` sets them from a theme's
tokens, and a host component that imports `MorphUi` reads the same instance ([`../ui/theme.md`](../ui/theme.md)).
The style is Qt Quick Controls' `Basic`.

## Components

`Components` holds the QML a client renders beside the built-in kinds (spec 5 §10):

- **Bundle components** (`addBundle(name, qml, digest)`) load only when the text's SHA-256 is the digest the bundle
  names (`sha256:<hex>`), and compile in a contained engine whose root context holds nothing, from a `qrc:` base URL
  in a directory no resource has, so a relative import finds nothing.
- **Host components** (`addHost(name, version, url)`) are QML the client carries; they load into the backend's own
  engine, so they may import the application's other modules, at the version a document asks for or later.
- **The restyle map** (`restyle(engine, kind, component)`) names a component for a built-in kind. The component must
  declare every property and function of the kind's `MorphUi` component — one that extends it does — and one that
  does not is refused when it is mapped; the kind then keeps its `MorphUi` component. The layout kinds (column, row,
  grid, spacer, scroll, slot, splitter, drop zone) are not restyled. A text input's restyle replaces its single-line
  component.
- `Backend::createCustom(parent, {name, props, minVersion})` loads a component and checks that it declares every
  prop the document declares. A component reports its events through `host.fire(event, payload)` (`host` a `var`
  property it declares and the backend sets); `CustomWidget::setOnEvent` receives the event's name and the payload
  as text, a JavaScript array or object as JSON. A prop is set as text (`string`, `int`, `exact`, `json`), a `bool`,
  or a `double` (`number`, the one kind of value that crosses as a double).
- A component that is not registered, does not match its digest, does not compile, is a host component below the
  version asked for, or lacks a declared prop makes `createCustom` return null, and the caller shows the fallback.
  Each failure is reported once per component and reason, through the callback and `failures()`.

`createCustom` is `ui::IViewBackend::createCustom`, and `CustomWidget` and `CustomRequest` are the view tree's own
types: `Mounted` asks for a document's `custom` node here first, binds its props, and shows the fallback only on null.

## Tests

`tests/qt_quick/`, registered under `MORPH_BUILD_QT_QUICK` with the ctest label `qt_quick`, run offscreen with the
software scene graph:

- **Conformance.** Every case of `ui::testing::conformanceCases()` passes on the real engine through a probe that
  sends mouse presses, moves, releases and key events to the window at a widget's place (a menu entry and a file drop
  go through the component's own entry point, which its items and its drop area call). A probe that delivers no
  input, or misreads every text, fails the cases that check them.
- **The backend's own cases**, for what the probe cannot reach: read-only fields of every kind, widgets out of reach,
  chords, dates in a zone, table placement and selection modes, overlays dismissed by Escape and by a press outside,
  moves that keep the item, split views, scroll areas, drop zones.
- **Documents on the real engine:** a bound property follows its value; a flush is one frame and a later flush
  another; a refused selection snaps back; typing keeps the cursor; a dialog dismisses and reopens; a list change
  moves the fewest rows and keeps the kept rows' items and focus; a table row keyed above 2^53 is reported exactly;
  spec 5 §7's server-calculated values show in their rows, stale and never blank while a newer request is in flight,
  with the row's error on a failure.
- **Components:** props and events; a digest mismatch, a compile error, a missing component, an old host component
  and a missing prop each leave the fallback, reported once; a restyle that lacks the kind's API is refused.
- **Automation conformance:** a generated screen read back through `QAccessible` from the window's interface, as a
  driver reads it, with the accessibility machinery switched on: the document's names and roles, a button's role,
  name and press action, a key-value pair named `"<caption>: <value>"`, the test id as `objectName` and, from Qt
  6.8, as the accessible identifier, and every input kind as one node found by its name and role, the same node as
  its test id, that a driver focuses and types into (a text input) or sets (a slider), with a radio select's options
  as radio buttons.
- **qmllint** over every `MorphUi` component, through the module's lint target, failing on any warning
  (`tests/qt_quick/run_qmllint.cmake`).

## Design decisions

| Decision | Why |
|---|---|
| An `IViewBackend`, not a QML generator | The interpreter mounts only through `Mounted`, numbers slots per instance, and builds rows, cases and dialog content per instance from closures; a static per-screen QML document cannot hold them. Implementing the backend contract holds this renderer to the same conformance suite as every other. |
| One component per kind, compiled once | `QQmlComponent` caches the compiled type; each widget is one `create`. |
| C++ writes the control's own state, not a property it binds | A binding is not re-evaluated when its source has not changed, so a refused edit could not snap back through it. |
| Keys stay in C++ | A JavaScript number loses integers above 2^53; QML never needs a key, only a label and an index, or the key's text. |
| Handlers released when the widget is destroyed, the item deleted later | QML may be inside the item's handler; the handlers must not keep the screen's scopes alive. |
| Restyle checked by declared names | A restyle component that extends the `MorphUi` one passes by construction; one written from scratch must offer everything the backend writes and calls. |

## Not yet

- The WebAssembly build, its smoke executable and the browser history (spec 3 §10), which come separately.
- Per-subtree surfaces and a runtime density from theme tokens; the units come from the default font, not the
  scale tokens.
- A per-template QML generator in place of per-kind components, which needs the interpreter to lower without
  mounting and to number slots per template.
