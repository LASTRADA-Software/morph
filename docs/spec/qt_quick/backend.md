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
- `BackendOptions::components` is the restyle map and the custom components.
- `Backend::widthClass()` is the host's width in `Theme.unitWidth`s, classed by `ui::widthClassOf`. `MorphRoots`
  reports that width in units (`widthUnits`) through its relay whenever it changes, from a resize or a theme with
  another font, and the context re-classes it there, on the GUI thread and never inside polish; a change sets
  `Theme.touchTargets` (true while `compact`) and calls the `setOnWidthClass` listener. `Theme` is one per engine,
  so two backends of different widths in one engine share whichever class was written last.

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
- **draws spans as escaped rich text.** `TextImpl::setSpans` writes `MorphText`'s `richText`: the runs inside one
  `<span style="white-space:pre-wrap">` (rich text otherwise folds a line feed and a run of spaces into one space),
  each run's text through `QString::toHtmlEscaped`, a strong run as `<b>` (the weight `Theme.strongFont` has), a mono
  run as a `<span style="font-family:…">` naming `Theme.monoFamilies`, each family escaped for a quoted CSS string.
  While `richText` is not empty the label shows it as `Text.RichText`; `setSpans` also writes `text` as the runs'
  text joined, which the accessible name and `shownText` read, and `setText` clears `richText`, so a plain text never
  goes through HTML. `MorphBanner` does the same through `BannerImpl`. Spans are the one `RichText` path.
- **draws text as plain text.** Every `Label` and `Text` that shows a document's or a value's text sets
  `textFormat: Text.PlainText`: the default, `Text.AutoText`, draws a string that `Qt::mightBeRichText` takes for
  markup as styled text, so data holding `<font>` or `<a href>` would be drawn as markup. The Basic style's own
  content items read markup too and have no `textFormat`, so a button's, a collapsible's and a menu entry's
  content is a `MorphPlainContent` (the control's icon, then its text as `PlainText`), a check box's and a radio
  button's content is a `PlainText` `Text` past the indicator, a drop-down's options are delegates with that
  content, a tab's content is a `PlainText` `Text`, and a widget's tooltip is a `ToolTip` of its own, made the
  first time it has one, whose content is a `PlainText` `Text`.
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
| Text | `MorphText` | a label in a text role, or its spans (below): a heading in `Theme.headingFont`, a section in `Theme.strongFont`, a mono text in `Theme.monoFont`, the rest in the body font; the colour is `MorphWidget.textColorOf(role)` (the scheme's muted colour for muted and section, error and success theirs, the text colour otherwise), which a restyle reads | — |
| Button | `MorphButton` | a `Button`; its `icon` drawn as the control's icon when the theme's icon set names it ([`../ui/theme.md`](../ui/theme.md#icons)). `style` 1, a list item: it fills its line (`fillsAcross`), its content an icon and a left-aligned label padded by `Theme.spaceOf(3)`, no border, a clear background that is `scheme.neutral` while hovered or pressed; while `selected`, an accent tint (`Qt.alpha(scheme.accent, 0.12)`), the label in the accent and a 3 px accent bar at its leading edge. A selected push button (`style` 0) keeps its frame, size and label: its background draws a 2 px `scheme.accent` ring outside its border (`selectionRing`) and, unless filled (primary, danger), fills with the item's accent tint, darkened while hovered or pressed; a focused one keeps its `scheme.focus` border inside the ring. Once told `selected` it is `Accessible.checkable`, with `Accessible.checked` following `selected`; the control itself is not `checkable`, so a click the document does not answer leaves it checked. `style` and `selected` are part of the button's restyle API | click, Space or Enter |
| TextInput | `MorphTextInput` (single line, password), `MorphTextArea` (multi-line) | a `TextField` / `TextArea`; a single-line field draws its `unit` as a muted trailing label inside the field, its right padding grown by the label's width, draws its `icon` as a leading image inside the field (its left padding grown by the image's width; no themed icon for the name reserves no space), and carries `decimals` for a kit (the stock input draws nothing from it) | each edit's whole text; Enter commits then submits (single line); focus leaving after an edit commits |
| Checkbox | `MorphCheckbox` | a `CheckBox` | the state toggled to |
| Select | `MorphSelect` | a `ComboBox` with a themed chevron indicator (`nav.expand`/`nav.collapse`, flipping with the popup), or `RadioButton`s | the chosen option's index, which C++ turns into its key |
| Menu | `MorphMenu` | a bar of top-level entries; with `buttonStyle`, one button drawn as a `MorphButton` (`triggerLabel`, `triggerIcon`, `triggerEmphasis`) that opens the top level in the popup, its item the one accessible node (`ButtonMenu`); a submenu opens in the popup, one level at a time; a separator is a `MenuSeparator`, a description a muted line under its `Accessible.MenuItem` entry | the chosen entry's path |
| Column, Row | `MorphStack` | a `GridLayout` flowing on the axis; `align` and `justify` (the `ui::Align` and `ui::Justify` indices) | — |
| Grid | `MorphGrid` | a `GridLayout`; a child's span is its `Layout.columnSpan`; `align`. With `tracks` (`{kind, amount, align}` each), `place()` lays the cells out in an item of its own instead, as the terminal does, and C++ calls it again whenever a cell's implicit size, visibility or span changes; across its track a label-width cell (`fillsAcross` false) keeps its natural width at the track's start, centre or end, and a text cell its `textAlign` | — |
| Spacer | `MorphSpacer` | space that fills the layout | — |
| Panel | `MorphPanel` | a framed region with a title and padding; collapsible, with a header button | the collapsed state asked for |
| Scroll | `MorphScroll` | a `Flickable` that scrolls the focused item into view, its content along a horizontal axis at least the viewport's width, with one bar on its axis made from its `bar` part (`MorphScrollBar`, a child of the flickable, where the attached `ScrollBar` lays it out) | — |
| Splitter | `MorphSplitter` | a `SplitView` whose handles are made from its `handle` part (`MorphSplitterHandle`); the splitter sets each handle's `axis` and `scheme`, and leaves a handle's implicit extent along the axis between two panes | the panes' sizes once a drag ends |
| Slot | `MorphSlot` | its children, packed at the top unless one stretches vertically; `scrollable` and no shown child stretching down, they sit in a `Flickable` at their natural height with a `MorphScrollBar`, and the focused item is kept in view as in a scroll | wheel, drag |
| Tabs | `MorphTabs` | a `TabBar` over the page slots, one `TabButton` per caption (the `Repeater`'s model is the caption count, so a relabel keeps the buttons, the highlight and the focus), the chosen tab kept wholly in view (`select` calls the bar's `positionViewAtIndex(index, ListView.Contain)` once the bar is laid out, and again when its width changes, so a tab chosen before the first layout is placed too), `Theme.spaceOf(3)` above them; `barShown` false hides the bar and gives its height to the pages, and its `focusTarget` is then the pages, so a hidden bar never holds the keyboard focus | the tab clicked |
| Dialog | `MorphDialog` | a modal `Popup`, centred, drawn as the theme's `card` surface (`color.surface`, its border, `radius.card` and `elevation.card`) with `Theme.spaceOf(4)` padding, whatever surface encloses the node: its content and title draw with the card's scheme; its `footer` item, under the content, holds the footer (a `MorphFooter`: its children in a row against the right edge, `Theme.spaceOf(2)` apart), shown only while it holds something. A `description` is a muted, wrapping label under the title, then a 1 px `Theme.cardScheme.border` rule across the content width, both outside the `ScrollView`, whose height cap subtracts them and their spacing. A `Popup` takes no `Accessible` attached property, so the content column is the overlay's accessible node (`accessibleItem`): `Accessible.Dialog`, named by the title or the node's `a11y.name`, described by the description | Escape or a press outside, as a dismissal |
| Drawer | `MorphDrawer` | a modal `Drawer` on the start or end edge, drawn as a dialog is but square against its edge; it cannot be swiped open; its `footer` as a dialog's, at the bottom | as a dialog |
| Toast | `MorphToast` | a non-modal, non-dimming `Popup` near a corner of `Overlay.overlay`, `closePolicy: Popup.NoAutoClose`; `show(text, tone)` is a QML function call, not a property write, so a repeated call with the same text and tone still restarts the dismiss `Timer` and reopens the popup if it closed | — |
| Busy | `MorphBusy` | a `BusyIndicator` and a label, both shown only while active (inactive, its implicit size is 0×0) | — |
| Table | `MorphTable`, a header over a `MorphTableHeaderBackground`, rows `MorphTableRow` over a `MorphTableRowBackground` | column headers over rows | selection and activation, below |
| DateTimeInput | `MorphDateTimeInput` | `yyyy-MM-dd` or `yyyy-MM-dd HH:mm` in the display zone | the instant typed, once the user finishes; empty is cleared |
| Slider | `MorphSlider` | an integer `Slider` | the rounded value moved to |
| FilePicker | `MorphFilePicker` | a path and a button opening Qt Quick Dialogs' `FileDialog` | the local path picked |
| KeyValue | `MorphKeyValue` | captions beside their values in a grid; while `captionsAbove`, a column of pairs, each caption in `Theme.bodyFont` and `scheme.muted` over its value, with a 1 px `scheme.border` rule `Theme.spaceOf(1)` from each side after every pair but the last. A toned value takes `toneOf(tone)` and `Theme.strongFont`; a value with spans draws the same escaped rich text as a Text. Each pair is one accessible node named `"<caption>: <value>"`, a value with spans by its runs' text | — |
| Banner, Badge, Progress, Steps, EmptyState | the component of that name; a removable badge's `ToolButton` (`remove`, drawing `close`, its accessible name `removeLabel`) is the badge's focus target | as the backend contract says | a banner's action and dismissal, a badge's removal, an empty state's action |
| Chart | `MorphChart` | one `Canvas`, below | — |
| Collapsible | `MorphCollapsible` | a toggle with the title, the header beside it, the content below while open | the open state asked for |
| DropZone | `MorphDropZone` | a framed `DropArea` for files | the local paths of a drop it took |

- **The chart.** `MorphChart` paints the whole chart on one `Canvas`, part of `QtQuick`, which the frontend already
  links: Qt Graphs and Qt Charts are separate modules (a package for every build and the WASM client), and
  `QtQuick.Shapes` is a QML module to deploy, and a chart of a few hundred points paints in well under a frame.
  `ChartImpl` writes the `ChartData` as one `chart` map of numbers and strings (a tone or clip as its index, `-1`
  for no tone, a gap as `gap`) and the `summary` text; the item repaints when either, its size or its scheme
  changes. The labels are painted too, in `Theme.bodyFont`: the axis labels (the lines' when three or more label the
  axis, else the ticks') right-aligned in a left gutter as wide as the widest, `yLabel` above them; with ticks, each
  line's label at its right end, above the line; the `xLabel`s in one row below the plot, each skipped when it would
  overlap the last one drawn, the newest always drawn; the legend below that for two or more series, each series'
  shape beside its label. It draws, in order: the lines (1 px on a pixel row's centre, so a line is one row of
  device pixels at any ratio; dashes 4 on 3 off, dots 2 on 3 off, in the tone's colour, `muted` for neutral); each
  series as a 1.5 px polyline in `scheme.chart1` to `chart4`, broken at gaps, when joined; its points, 5 px across,
  as a circle, square, triangle or diamond in token order; then over every series the clipped points (an open
  triangle at the edge, pointing out of the plot) and the marked points (a cross 10 px across, 2 px stroke, in the
  tone's colour) and the notes (the tone's colour, beside the point, flipped to its left near the right edge).
  Its content size is 40 units by 8, so a chart in a stretching column is readable before any `sizing`. It is
  `Accessible.Chart`, named by the node's `a11y` name or else the summary; the summary is the description only
  when the name is not, so a screen reader reads it once. `shownText()`, the probe's `textOf`, is the summary. A
  `Canvas` paints at the device pixel ratio, and the chart's tests run again at `QT_SCALE_FACTOR=2`
  (`qt_quick_chart_scale_2`).
- **Description and tone.** `MorphField` carries `description` and `tone` (the `Tone` index, 0 none). A `description`
  label under the messages wraps, is muted or `toneOf(tone)`, and counts in the field's height; the field's
  `accessibleDescription`, which every control declares as `Accessible.description`, is the messages and then the
  description. `shownTone` is `Err` while the field has errors, else its tone, and outlines the control:
  `MorphInputBackground` takes `tone`, draws its border in the tone's colour focused or not (focus shows as the 2 px
  width), and a 2 px ring of the tone's fill outside it as a sibling `Rectangle`, since a `Rectangle` draws nothing
  outside itself. A check box draws its indicator's border in the tone, a radio select each indicator's, and a slider
  its handle's.
- **Required mark.** While a caption is drawn, `MorphField`'s mark sits on the caption's line, a space step after its
  `contentWidth`, the caption elides short of it, and the control takes the field's whole width. `MorphText` sets
  `markerFollowsText` and gives the end of its last line (`textEndX`, `textEndY`, recorded from `lineLaidOut`; rich
  text reports no lines, so for spans the widest line's end on the last line) and wraps short of the mark. Otherwise
  the mark sits at the field's right edge beside the control, which gives up its width.
- **Captions.** `MorphField` carries `caption` and `labelShown`; while `labelShown` it draws the caption above its
  control and moves the control under it, and the field's implicit size takes it in. A text input, select and
  date-time input write both from `setCaption`. A restyle reads `caption` with `labelShown` false to know its
  label is drawn elsewhere. (The property is not `label`: `MorphCheckbox` declares `label` for its own text.)
- **Controlled inputs** behave as the contract says: the control shows the user's action at once, as Qt Quick does,
  and the mount re-asserts the document's value in a later turn. A read-only check box, select or slider takes
  focus and puts its own state back when the user acts; a read-only text field reports no edit, commit or submit.
- **Icons.** A badge, an empty state, a menu entry and a button draw a node's icon from `Theme.iconSource` when the
  bundle's icon set names it, and today's text otherwise; a banner draws its tone's framework icon and its close
  button's, and a collapsible and a panel their chevrons, from the set when it names them. While the bundle declares
  a set, a node icon outside it is reported once per name (`Context::checkIcon`).
- **Dialogs and drawers** open and close without a transition, so what the document says is what the window shows.
  `footer` is part of their restyle API: a kit's dialog places the footer item where it wants, and a restyle that
  does not declare it is refused.
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
for content (as the terminal backend sizes it), a share of the rest for stretch but never less than its header (the
header's width and a unit a side, at least 8 units, as an empty content column's), the share a floored column gives up
taken from the others — so a hidden cell keeps its column and the cells after it stay under their headers. A cell is
its column's width less `Theme.spaceOf(2)` a side, except one whose `fillsAcross` is false (a badge, a check box, a
button), which is its implicit width at the column's start, centre or end, by the column's `align`. A cell that
declares `textAlign` (`MorphText`, `MorphTextInput`) is given the column's `align` as `Qt.AlignLeft`,
`Qt.AlignHCenter` or `Qt.AlignRight`, its label's or field's `horizontalAlignment`, which a right-to-left layout
mirrors; an `end` column's cells also set `tabularFigures`, drawing digits at one width (`tnum`) so numbers line up.
The header's label takes the column's alignment. Every cell keeps its implicit height, up to the tallest, and sits at
the row's top, centre or bottom by the column's `valign`; `Stretch` gives it the row's height. The header and the rows sit in a horizontal `Flickable` (`hflick`) as wide as the
columns: while they are wider than the table it scrolls sideways, clips and shows a horizontal `MorphScrollBar`;
otherwise it takes no drag or wheel event, and it never takes a vertical one. The rows' own vertical `Flickable`
(`flick`) is, virtualized, what the header leaves of the table's height, and otherwise its rows' height. A content column's width is cached per column rather than recomputed
by rescanning every row on every change: a cell growing only raises its column's cached width; a cell shrinking, a row
leaving, or a cell's visibility flipping rescans that one column, since the cache alone cannot tell whether the row
that set its current maximum is still contributing. Either way, every row still places its cells again once a
column's width actually changes, since a widened column's cells all need their new x/width. A table is a Tab stop
(`activeFocusOnTab`) that hands the focus on to its current row, or the first; the focused row is the current one,
which its background draws and assistive technology announces with `Accessible.selected`. A click makes its row
current and focuses it, unless a control in a cell took the focus with the press; the `TapHandler` reports Ctrl and
Shift, which `TableImpl::tapped` reads as a `RowClick` against the anchor it keeps. The table's own keys
(`MorphWidget.ownKey`, after its chords) run while the table or a row holds the focus itself: the arrows, Home, End,
PageUp and PageDown move the focus between the shown rows, Shift with Up or Down marks the row left as the anchor
when there is none and selects the range, Space toggles the current row, and Ctrl+A (`multiple` only) selects every
row or clears them. A plain double click or Enter on a row activates it without toggling the selection. A range and
Ctrl+A run over `setExistingKeys`' keys in row order when the table virtualizes, and over the realized rows
otherwise. A select column takes `Theme.targetHeight` before the first column, which `columnWidths` leaves out of
what it shares: the header draws a tri-state `CheckBox` there (`selectAll`, named `selectAllLabel`) and each row a
`CheckBox` bound to `selected` (`selectBox`, named as its row). Each box's `nextCheckState` returns its current
state, so its look follows the selection alone; its `clicked` fires the row's `tap` with Ctrl, or the table's
`selectAll`, and a tap on a row's box is not also a tap on the row. A row is named by `rowLabel`, else its key. Selection is kept as the requested keys, marked on the
rows that have them, as the contract's "Selection by key" says; a user's change reports exactly the existing rows
selected.

A heading row (`setRowHeading`, the row's `heading`) places its one cell from the first inset across the select
column and every column, less two insets; its `TapHandler`, Enter and select box are off, and `shownRows` (the rows the
current row, Ctrl+A and the select-all box run over) leaves it out, as does a content column's width scan. Its
background gets `heading` only when the part declares it, so a `tableRow` restyle that draws no headings still loads;
the stock `MorphTableRowBackground` fills a heading with `scheme.tableHeader`, as the header row is filled, which is
clear until a theme sets `color.table.header`, so in the stock theme a heading stands out by its content alone.

A sortable column's header label (`header<index>`) is a Tab stop, and a tap, Enter or Space on it fires `sort` with
the column's index, which reaches `setOnSort`; another header takes neither. The table never sorts: `setSort` writes
`sortColumn` (−1 for none) and `sortDescending`, and the sorted column's label shows ` ▲` or ` ▼` after its text.
Every sortable header keeps room for the mark in its column's floor, so the columns do not move when the sort does.
Qt's accessibility has no sort state, so the header (a `ColumnHeader` named by its label) states the order in its
`Accessible.description`, "Sorted ascending" or "Sorted descending".

A row draws its background part (`background`, the `tableRow` part restyle) under its cells and hands it its state:
`selected`, `hovered` (the pointer over the row), `current` (keyboard focus within the row), `alternate` (an odd
position, which C++ writes on every row after each append, destroy and move), `tone` (`setRowTone`: −1 for none,
else the `ui::Tone` index) and its `scheme`. The row's
height is the larger of its cells' and the background's `implicitHeight`; the stock background draws the selection
fill and the hairline between rows, and its implicit height is a dense row's, `Theme.targetHeight - 8`. A toned
row fills with the tone's `bg` colour and draws a 3 px leading bar in its `dot` colour; a selected toned row shows
the selection colour and keeps the bar.

The header row draws its own background part (`headerBackground`, the `tableHeader` part restyle) behind the column
labels, and hands it only `scheme`: the header has no selection, hover, focus or tone of its own. The stock
background fills with `scheme.tableHeader` (`color.table.header`, transparent by default), so a table draws no
header fill until a theme sets one. A document-authored column's `role` (a `TextRole`) styles its default cell's
text and has no effect on a column with its own `cell`.

**Virtualized tables.** A table whose row count has crossed its `Virtualization::threshold`
(`docs/superpowers/specs/2026-10-22-table-row-virtualization-design.md`) realizes a widget only for
rows the mount's windowing Effect decides are in or near the visible range; `MorphTable.qml` owns its
own `Flickable`, with leading and trailing spacer items standing in for the rows not realized, sized
by an estimate (`estimatedRowHeight`) that corrects itself toward the real average height of whichever
rows are actually realized. A document-authored `table` must therefore have an explicit height
(`Fixed` or `Stretch` sizing) once it is large enough to virtualize — unlike every other table, which
grows to its content's full height and relies on an ancestor to scroll it.

Two consequences follow directly from only some rows ever being realized:

- **Column widths become monotonic, not exact.** The width cache (above) can only answer "how wide is
  the widest *realized* cell," not the widest cell in the whole table; without a rule for that, a
  content column's cached width would shrink back down whenever the row that set its current maximum
  scrolls out of the realized range, then grow again if a wider row scrolls back in — jitter on every
  scroll, not a one-time correction. A virtualized table's column widths therefore only ever grow: a
  column may stay wider than currently necessary once the row that justified it has scrolled away, in
  exchange for never visibly narrowing and widening as the user scrolls.
- **Selection survives a row leaving the realized range.** `existingSelection()`'s ordinary check — is
  this requested key a currently-realized row's key — would otherwise drop a selected key the moment
  its row scrolls out of view, since nothing short of the row's own `TableRowImpl` normally proves a
  key still exists. A virtualized table additionally tracks every key the mount reports as existing in
  the data at all (`TableWidget::setExistingKeys`), realized or not, and a requested key already in
  that set is never dropped from the reported selection just because it has no realized row right now.

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
`n` units, `stretch(w)` to fill with stretch factor `w`. The bounds map to `Layout.minimumWidth`, `maximumWidth`,
`minimumHeight` and `maximumHeight` of `n` units (`MorphWidget`'s `minWidthUnits`, `maxWidthUnits`,
`minHeightUnits`, `maxHeightUnits`; −1 for none, which resets the layout's default): `QtQuick.Layouts` shares the
length a capped item leaves among the others, and places a fill item its maximum stops at the start. `showLayout`
writes a bound given and offers an absent one, so a component that declares no bounds is reported only for a node
that has one. A unit is the body font's average character width
horizontally and its line height vertically (`Theme.unitWidth`, `Theme.unitHeight`): the platform font's, or the
theme's `font.family` at the body size once a theme names one ([`../ui/theme.md`](../ui/theme.md), "On Qt Quick").

A content-sized dimension fills the container across its axis ([`../ui/backend_contract.md`](../ui/backend_contract.md),
the cross axis). A QML child cannot see its parent's axis, so the container says: `ContainerCore::adopt` writes the
child's `fillsWidth` and `fillsHeight` from `crossFill()`, which is the width for every container but a row (the
height), a splitter and a table row (neither, since they size their own children). `MorphWidget` binds
`Layout.fillWidth` to a stretch width, or a content width with `fillsWidth`, and the same for height. Along its axis a
`MorphStack` whose shown children do not stretch keeps its layout at its natural length, so the children pack at the
start: a `GridLayout` given more length hands the spare to the gaps between its cells.

**Alignment.** `ContainerCore::adopt` also offers each child `alignAcross` (1 for a column's child, placed
horizontally; 2 for a row's or a grid's, placed vertically; 0 elsewhere) and `crossAlign`, the container's
`ui::Align` index; `setAlign` re-offers `crossAlign` to every child already held (`alignChildren`), since a child is
adopted in its constructor, before its container can be aligned. `showLayout` writes a node's `alignSelf` (−1 for
none, offered). `MorphWidget` places itself by `alignSelf`, else `crossAlign`: under start, centre or end it sets
`Layout.alignment` (`AlignLeft`, `AlignHCenter`, `AlignRight` in a column; `AlignTop`, `AlignVCenter`, `AlignBottom`
in a row or grid) and drops the cross fill of a content-sized dimension, while a `stretch` one keeps its fill and its
maximum places it. Along the axis, while no shown child stretches, `MorphStack` sizes its layout to its natural length
and offsets it by `justify`: none, half the spare length or all of it. For `between` the layout keeps the full length
and its spacing is `max(gap, spare / (shown − 1))`, the spare computed from each shown child's preferred (else
implicit) length clamped into its `Layout` minimum and maximum, because a `GridLayout` given more length gives it to
its cells, not its gaps. Its implicit length along the axis is then those lengths and the gaps, not the layout's:
the layout's includes the spacing taken from the stack's length, which a parent that sizes the stack from its
implicit length would feed back, widening the stack on every pass.

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
(`MorphInputBackground`) and a button (`MorphButtonBackground`) are `Theme.targetHeight` tall (`controlHeight`, or
at least `touchHeight` while the window is compact, [`../ui/theme.md`](../ui/theme.md)) with
`Theme.radiusControl` corners and a border that takes the focus colour; a read-only input (`readOnly`, which every
input passes its background) draws on `Theme.background` rather than its scheme's surface; a check box's indicator is accent-filled when
checked; a tab bar underlines the current tab in the accent; a progress bar is the scheme's accent on its `neutral` track,
`Theme.spaceOf(1)` tall with rounded ends, and an indeterminate one a third-wide accent segment sliding across it while
shown; a key-value's captions keep their whole width on one line, and its values fill what is left and wrap in it; a table's header is muted and its rows dense with a
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
  | `tableHeader` | `MorphTableHeaderBackground` | a table's header row background | `scheme` |
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
- A runtime density from theme tokens; the units come from the body font, not the scale tokens.
- A per-template QML generator in place of per-kind components, which needs the interpreter to lower without
  mounting and to number slots per template.
