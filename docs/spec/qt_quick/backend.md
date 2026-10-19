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
- [Several mounts](#several-mounts)
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

## Several mounts

An application with a shell of its own (a tab workspace, a split view) shows several screens in one window: each
page item of its shell is a `host`, with one `Backend` per page, and every root that backend makes is appended to the
column it adds to that page.

| Shared by every mount | Per mount |
|---|---|
| `ui::Session` (cache, locale, translations), `reactive::Runtime`, `Dispatcher`, `QQmlEngine`, `qt_quick::Components` (restyles, host and bundle components), the `Theme` singleton (one per engine) | `qt_quick::Backend` and its context (the per-type component cache), `ui::DocumentScreenHost` (its interpreter, close guard, application host and `diagnostics` callback), the screens and their handles |

- A restyle is resolved per `Backend` when it first makes the type, so a restyle mapped after a backend made that
  type reaches only backends made later, and that backend keeps the component it made. Restyles are mapped before
  the first backend makes anything: in `FrontendConfig::onEngine`, or before the application builds its backends.
- Every `Backend` is destroyed before the engine, and every `DocumentScreenHost` before the `Session`. Destroying one
  mount (its screens, its host, its backend) leaves the others working.
- A mutation on one page applies its effects through the shared session, so a query another page shows refreshes.
- Nothing new is refused: a `DocumentScreenHost` already needs a backend and a document lookup.

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
  changed it since. A setter writes only a property the item declares: a name it does not declare would become a
  dynamic property that no binding reads, so the write is refused and reported once per QML type and name, through
  the `Components` callback and `failures()` (``MorphText does not declare the property `x`; the write is refused``),
  or on `std::cerr` for a backend without components. It is reported rather than thrown because writes also run from
  Qt signal handlers and from a table restriping its rows while one is destroyed, where an exception cannot pass. The
  placement and scheme a container writes into each child (`fillsWidth`, `fillsHeight`, `inheritedScheme`) are
  offered instead: every `MorphUi` item declares them, and a custom component takes part only if it does.
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
  for a radio select, CheckBox and Slider. A drop-down the document names no name reads the option it shows. A
  ComboBox node has no value of its own, so the option a drop-down shows is the value of its one child, the style's
  text field, which the select declares EditableText from C++ (`QQmlProperty` on `Accessible.role`, since QML
  writes an attached property only on the item that declares it); a named drop-down keeps its name and still
  reads the option there. A check box reads its label. A radio select's group carries the name, and each option is a RadioButton named by its
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
| TextInput | `MorphTextInput` (single line, password), `MorphTextArea` (multi-line) | a `TextField` / `TextArea`; a single-line field draws its `unit` as a muted trailing label inside the field, its right padding grown by the label's width, and carries `decimals` for a kit (the stock input draws nothing from it) | each edit's whole text; Enter commits then submits (single line); focus leaving after an edit commits |
| Checkbox | `MorphCheckbox` | a `CheckBox` | the state toggled to |
| Select | `MorphSelect` | a `ComboBox`, or `RadioButton`s | the chosen option's index, which C++ turns into its key |
| Menu | `MorphMenu` | a bar of top-level entries; a submenu opens in a popup, one level at a time | the chosen entry's path |
| Column, Row | `MorphStack` | a `GridLayout` flowing on the axis | — |
| Grid | `MorphGrid` | a `GridLayout`; a child's span is its `Layout.columnSpan` | — |
| Spacer | `MorphSpacer` | space that fills the layout | — |
| Panel | `MorphPanel` | a framed region with a title and padding; collapsible, with a header button | the collapsed state asked for |
| Scroll | `MorphScroll` | a `Flickable` that scrolls the focused item into view, with one bar on its axis made from its `bar` part (`MorphScrollBar`, a child of the flickable, where the attached `ScrollBar` lays it out) | — |
| Splitter | `MorphSplitter` | a `SplitView` whose handles are made from its `handle` part (`MorphSplitterHandle`); the splitter sets each handle's `axis` and `scheme`, and leaves a handle's implicit extent along the axis between two panes | the panes' sizes once a drag ends |
| Slot | `MorphSlot` | its children, packed at the top unless one stretches vertically | — |
| Tabs | `MorphTabs` | a `TabBar` over the page slots | the tab clicked |
| Dialog | `MorphDialog` | a modal `Popup`, centred | Escape or a press outside, as a dismissal |
| Drawer | `MorphDrawer` | a modal `Drawer` on the start or end edge; it cannot be swiped open | as a dialog |
| Busy | `MorphBusy` | a `BusyIndicator` and a label, both shown only while active (inactive, its implicit size is 0×0) | — |
| Table | `MorphTable`, rows `MorphTableRow` over a `MorphTableRowBackground` | column headers over rows | selection and activation, below |
| DateTimeInput | `MorphDateTimeInput` | `yyyy-MM-dd` or `yyyy-MM-dd HH:mm` in the display zone | the instant typed, once the user finishes; empty is cleared |
| Slider | `MorphSlider` | an integer `Slider` | the rounded value moved to |
| FilePicker | `MorphFilePicker` | a path and a button opening Qt Quick Dialogs' `FileDialog` | the local path picked |
| Banner, Badge, Progress, Steps, KeyValue, EmptyState | the component of that name | as the backend contract says | a banner's action and dismissal, an empty state's action |
| Collapsible | `MorphCollapsible` | a toggle with the title, the header beside it, the content below while open | the open state asked for |
| DropZone | `MorphDropZone` | a framed `DropArea` for files | the local paths of a drop it took |

- **Captions.** `MorphField` carries `caption` and `labelShown`; while `labelShown` it draws the caption above its
  control and moves the control under it, and the field's implicit size takes it in. A text input, select and
  date-time input write both from `setCaption`. A restyle reads `caption` with `labelShown` false to know its
  label is drawn elsewhere. (The property is not `label`: `MorphCheckbox` declares `label` for its own text.)
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

A row draws its background part (`background`, the `tableRow` part restyle) under its cells and hands it its state:
`selected`, `hovered` (the pointer over the row), `current` (keyboard focus within the row), `alternate` (an odd
position, which C++ writes on every row after each append, destroy and move), `tone` (`setRowTone`: −1 for none,
else the `ui::Tone` index) and its `scheme`. The row's
height is the larger of its cells' and the background's `implicitHeight`; the stock background draws the selection
fill and the hairline between rows, and its implicit height is a dense row's, `Theme.controlHeight - 8`. A toned
row fills with the tone's `bg` colour and draws a 3 px leading bar in its `dot` colour; a selected toned row shows
the selection colour and keeps the bar.

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
horizontally and its line height vertically (`Theme.unitWidth`, `Theme.unitHeight`).

A content-sized dimension fills the container across its axis ([`../ui/backend_contract.md`](../ui/backend_contract.md),
the cross axis). A QML child cannot see its parent's axis, so the container says: `ContainerCore::adopt` writes the
child's `fillsWidth` and `fillsHeight` from `crossFill()`, which is the width for every container but a row (the
height), a splitter and a table row (neither, since they size their own children). `MorphWidget` binds
`Layout.fillWidth` to a stretch width, or a content width with `fillsWidth`, and the same for height. Along its axis a
`MorphStack` whose shown children do not stretch keeps its layout at its natural length, so the children pack at the
start: a `GridLayout` given more length hands the spare to the gaps between its cells.

**Surfaces.** `Theme.scheme(name)` is the colour scheme of a surface (`MorphScheme`: the colour roles, each the
theme's unless the surface sets it), and the theme itself for the empty name. Every `MorphWidget` draws with
`scheme`: its own surface's when it has one, else `inheritedScheme`, which its container writes when it adopts it
(`ContainerCore::scheme()`; the roots give the theme). The mount sets a node's surface before it mounts the node's
children, so a container's scheme is known when they are adopted. A widget with a surface draws its background under
its content. `applyTheme(engine, values, surfaces)` sets every role of each named surface's scheme in place, so a
widget that holds one follows a later theme.

The roots fill the window as one column: a root whose height stretches takes what the others leave, as on the
terminal; without one, the roots keep their own heights at the top.

A spacing step (a stack's or grid's `gap`, a panel's `padding`) is `Theme.spaceOf(step)` px, from the `space.0` …
`space.5` tokens, not a number of units. A stale field is dimmed; a
field's errors show under it in the error colour; a required field is marked. The `Theme` singleton holds the colour
roles every component reads, with the `ui-theme/1` defaults; `applyTheme(engine, values)` sets them from a theme's
tokens, and a host component that imports `MorphUi` reads the same instance ([`../ui/theme.md`](../ui/theme.md)).
The controls are Qt Quick Controls' `Basic` ones, drawn from the theme rather than with that style's look: an input
(`MorphInputBackground`) and a button (`MorphButtonBackground`) are `Theme.controlHeight` tall with
`Theme.radiusControl` corners and a border that takes the focus colour; a check box's indicator is accent-filled when
checked; a tab bar underlines the current tab in the accent; a table's header is muted and its rows dense with a
hairline between them; a panel is a card; a heading is `Theme.headingPixels`, bold; and the window behind every
surface is `Theme.background`.

## Components

`Components` holds the QML a client renders beside the built-in kinds (spec 5 §10):

- **Bundle components** (`addBundle(name, qml, digest)`) load only when the text's SHA-256 is the digest the bundle
  names (`sha256:<hex>`), and compile in a contained engine whose root context holds nothing, from a `qrc:` base URL
  in a directory no resource has, so a relative import finds nothing.
- **Host components** (`addHost(name, version, url)`) are QML the client carries; they load into the backend's own
  engine, so they may import the application's other modules, at the version a document asks for or later.
- **The restyle map** (`restyle(engine, kind, component)`) names a component for a built-in kind or part. The
  component must declare every property and function of its `MorphUi` base — one that extends it does — and one that
  does not is refused when it is mapped, naming what it lacks; the kind then keeps its `MorphUi` component. A base's
  properties are its restyle API, so a property a base gains is one a restyle written from scratch must then declare.
  There are two kinds of restyle:
  - **A kind restyle** replaces a kind's whole component. A text input's restyle (`textInput`) replaces its
    single-line component, and a `textArea` restyle its multi-line one (`MorphTextArea`, whose API is
    `MorphField`'s field API with `placeholder`, `edited`, `assigning` and `assign(value)`).
  - **A part restyle** replaces one drawn part inside a component C++ creates, never its layout. The container's
    component declares the part as a `Component` property, and C++ passes the context's component for the part's
    type, restyled or not, as an initial property when it makes the container.

  | Restyle kind | `MorphUi` base | What it replaces | Base API (beyond its Qt type's) |
  |---|---|---|---|
  | `tableRow` | `MorphTableRowBackground` | a table row's background: selection, hover, focus, zebra, tone, and the row's minimum height | `selected`, `hovered`, `current`, `alternate`, `tone` (−1 none, else a `ui::Tone` index), `scheme` |
  | `scrollBar` | `MorphScrollBar` (a `ScrollBar`) | the bar a scroll area attaches on its axis | `scheme` |
  | `splitterHandle` | `MorphSplitterHandle` | the handle between two panes of a splitter | `axis`, `scheme`; the handle reads `SplitHandle.pressed` and `.hovered` |

  A `scrollBar` restyle is a `ScrollBar` subclass: the names checked are every one beyond `QQuickItem`'s, so the
  base's include all of `ScrollBar`'s. The layout kinds (column, row, grid, spacer, scroll, slot, splitter, drop zone)
  are not restyled as kinds; what a scroll area and a splitter draw of their own is restyled as their parts, and the
  refusal says so (`` `scroll` is a layout kind; restyle its bar as `scrollBar` ``).
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
- **Several mounts:** two backends on two items of one window over one engine, one `Components` with a
  `textInput` restyle and one session: both pages' inputs are the restyle, a mutation on one page refreshes a query
  the other shows, each host's diagnostics hold only its screen's, and destroying the first page leaves the second's
  input editable; a restyle mapped after the first page made its input reaches only the second.
- **Components:** props and events; a digest mismatch, a compile error, a missing component, an old host component
  and a missing prop each leave the fallback, reported once; a restyle that lacks the kind's API is refused; a
  `tableRow` restyle draws every row's background, sets the row's height, and sees `alternate` follow a move and a
  destroy; a `textArea` restyle is what a multi-line input is made of, and one without `assign` is refused; a
  `scrollBar` restyle is the bar a scroll area attaches, and one that is no `ScrollBar` is refused; a `splitterHandle`
  restyle is every handle, and a 12 px one makes the panes leave 12 px where the stock one leaves 6; `scroll` and
  `splitter` as kinds are refused naming their part kinds; a write
  of a property the item does not declare is refused and reported once, and a custom component in a container is
  offered no placement it does not declare.
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
- A runtime density from theme tokens; the units come from the default font, not the
  scale tokens.
- A per-template QML generator in place of per-kind components, which needs the interpreter to lower without
  mounting and to number slots per template.
