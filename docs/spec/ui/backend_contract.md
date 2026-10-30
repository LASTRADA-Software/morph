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
  parameters (a TextInput's mode, a stack's, scroll area's or splitter's axis, a DateTimeInput's
  mode and display zone, a Select's style, a FilePicker's mode, a Drawer's side) are factory
  arguments.
- `createCustom(parent, {name, props, minVersion})` is the one factory that may return null, and is
  not pure: null means the backend did not load the component, and the mount shows the node's
  fallback instead. A backend that loads no components keeps the default, which returns null; one
  that loads a component returns a `CustomWidget`, whose props are set by name (`setTextProp`,
  `setBoolProp`, `setNumberProp`) and whose events go to the handler given to `setOnEvent`.
- **A new widget is visible, enabled, content-sized, not draggable and not a drop target, and has
  no accessible name or role of its own, no test id, no tooltip, the theme's default surface and no
  chords. A new field (text and the six inputs) is editable, not required, without errors and not
  stale. A new dialog or drawer is closed; a new collapsible section is open.** The mount calls the `Widget` setters for those defaults
  (`setVisible`, `setEnabled`, `setLayout`, `setDragKey`, `setDropHandler`, `setAccessibleName`,
  `setAccessibleRole`, `setTestId`, `setTooltip`, `setSurface`, `setKeys`) and the `FieldWidget`
  setters (`setReadOnly`, `setRequired`, `setErrors`, `setStale`) only to change them; it
  calls a dialog's `setOpen` at mount whatever the value, `false` included.
- **Focus.** `focus()` moves keyboard focus to the widget. The mount calls it on the first
  `autofocus` widget in document order once a mount completes ([view tree](view_tree.md#focus)).
- **Hover.** `setOnHover(onHover)` calls `onHover` each time the pointer comes to rest over the widget. The
  default does nothing, so a backend without a pointer conforms. Hovering is a hint, such as a prefetch, never
  something a widget relies on. Qt Quick reports it through `MorphWidget`'s `HoverHandler` when a handler is set.
- **Opening a select.** `SelectWidget::setOnOpen(onOpen)` calls `onOpen` each time the user opens a dropdown,
  also one with no options to show. The default does nothing, and a radio group is never opened. Like hovering it
  is a hint, used to fetch again options whose fetch failed. Qt Quick reports the combo box's popup opening; the
  TUI reports the list opening, or the attempt to open it when there are no options.
- **Chords.** `setKeys(chords, onChord)` declares the chords a widget handles while focus is inside
  it, and `onChord` receives the chord pressed. A chord pressed while focus is inside several
  widgets that declare it goes to the innermost of them only, and a chord pressed inside a widget
  that takes no input (below) goes to none. A key pressed with focus on a widget goes, the same on every backend:
  1. to a chord the focused widget declares itself, since declaring it on that very node is how an author says the
     key means something else there;
  2. then to the widget's own handling of the key, for the keys its kind takes. A button takes Enter and Space,
     which click it. A single-line or password text input takes Enter only when it has a submit handler: it commits,
     submits, and the key stops there; without one it commits, and Enter goes on. A segmented select takes Left and Right, which
     choose the previous or the next option (`onSelect` once) and stop at the ends; Space and Enter on it go on, so
     Enter reaches a dialog's `default`. It is one Tab stop, entered at the marked option, as ARIA's radio group is.
     A check box takes Space, which
     toggles it, and lets Enter go on. A table takes Enter, which activates the current row, and, while it or one of
     its rows holds the focus itself, the arrows, Home, End, PageUp, PageDown, Space, Shift+Up, Shift+Down and, in
     `Multiple` mode, Ctrl+A ([Selection by key](#selection-by-key)); a `Single` table's Ctrl+A, and a key a control
     inside a cell leaves, go on. Every other key a kind handles (editing keys, arrows, a list's navigation) is its
     own as before;
  3. then to the ancestors' chords, innermost first, for a key the widget left. A dialog's Enter chord (a document's
     `default`) is one of them, so it runs for Enter that no focused widget took.

  The mount gives `setOnSubmit` an empty handler for a text input without `onSubmit`, and a backend reads an empty
  handler as "this field does not submit".
- **Read-only.** A field made read-only by `setReadOnly(true)` shows its value, takes focus and
  lets its text be selected, and reports no edit, submit or commit; `setReadOnly(false)` makes it
  editable again. It is drawn so that it does not look editable: on Qt Quick on the page's background colour
  instead of the input fill, keeping its border and focus ring; on the terminal, where no field has an input fill,
  as its text in place in the normal text colour, distinct from a disabled field's muted text.
- **Date inputs.** A graphical backend offers a calendar beside the typed date field, opening on
  the field's date or on today in the display zone. A day chosen produces the same text an edit
  would and reaches `onChange` the same way, so the field's text is the one source of the value.
  The terminal offers typed text only. A read-only date input shows no calendar. `setPattern(pattern)`, called once
  before the value and only for a pattern that is not empty, makes the field show its value with `formatDate` and
  read typed text with `parseDate`, ISO always being read too; the calendar writes its day in the same pattern.
- **Menus.** `createMenu(parent, style)` makes a bar of top-level entries (`MenuStyle::Bar`) or one
  button that opens the top level as a popup (`MenuStyle::Button`), whose trigger `setTrigger(MenuTrigger{label,
  icon, emphasis})` sets: the label is its text and accessible name; an icon-only trigger draws its
  icon from the theme's set, `more` as `⋯` when the set does not name it, and another icon the set
  does not name as the widget's accessible name, so a trigger is never blank. `setItems` replaces
  the whole tree of `MenuEntry{label, icon, keys, checked, enabled, items, separator, description}`.
  `keys` is only shown beside the label; `checked` engaged shows a check mark that only `setItems`
  moves; an entry with `items` opens its submenu; a `separator` is a line between entries, never
  chosen or opened; a `description` is a muted line of help with its entry. `onActivate` receives
  the chosen entry's path, its index at each level from the top with separators counted, and only
  for an enabled entry without a submenu whose ancestors are all enabled.
- **Spans.** `TextWidget::setSpans` and `BannerWidget::setSpans` replace what the widget shows with runs, in order,
  in the widget's role or tone: a backend draws a `strong` run in its bold weight and a `mono` run in its monospace
  face, and every run's text as its characters. The mount calls `setSpans` in place of `setText` for a node that has
  spans. Both are pure, so a backend cannot drop the emphasis silently. What a probe and assistive technology read
  of such a widget is the runs' text joined (`spansText`).
- **Description and tone.** `setDescription` shows a line under the control, after the messages, and makes it the
  control's accessible description after the messages (`aria-describedby`). `setTone` evaluates the value without
  making it invalid: the control is outlined in the tone and the description drawn in the tone's colour instead of
  muted. A field with errors is drawn as `Err`, so errors and an evaluation share one outline, and only errors block.
  On the terminal, which draws no outline, an `err` or `warn` field's control row adds the `!` mark. A text takes
  neither: the mount never calls them on one, and every backend refuses them there (`std::logic_error`).
- **Required mark.** `setRequired(true)` marks a field that must be given. The mark follows the caption, on the
  caption's line, while the field draws one (`setCaption(label, true)`), so the control keeps the field's whole
  width; without a caption it sits beside the control. A required text's mark follows the end of its last line.
- **Key-values.** Each caption sits beside its value, or above it once `setCaptionPlacement(Above)` is called. A
  toned value (`KeyValueItem::tone` other than `Neutral`) is drawn in the tone's colour and the strong weight, and a
  value with spans draws them as a text does; its accessible text is the runs' text joined.
- **Text is characters.** Every setter's text is shown as the characters it holds: a backend never interprets a
  document's or a value's text as markup, so a name or a comment that holds `<b>` or `<a href=…>` shows those
  characters.
- Every setter takes UTF-8. **A setter called with the value the widget already shows changes
  nothing**: in particular a text input keeps its cursor, its selection and an input method's
  composition. A controlled input's re-assertion, or another renderer, may make such a call.
- **Input widgets are controlled.** A user's action changes what the widget shows, as the toolkit
  does by itself, and reports the request through the handler; the mount then shows what the
  application's slot says, in a turn posted after the request's flush, so a request the application
  refuses snaps back ([`view_tree.md`, "Controlled inputs"](view_tree.md#controlled-inputs)). A
  commit requests no value, so it is not followed by a re-assertion. A user's dismissal closes a
  dialog or drawer before `onDismiss` runs; one the application keeps open is opened again. A
  banner's dismissal changes nothing the banner shows. A splitter shows the sizes the user dragged
  to, and a collapsible section the state the user asked for, before the handler runs.
- **No setter calls a handler.** Setting a value, replacing options, items or rows, requesting a
  selection, opening or closing, collapsing — none of it is a user's doing, so none of it reaches
  `onChange`, `onToggle`, `onSelect`, `onSelectionChange` or any other handler. That covers what a
  widget does by itself in response to a setter: a Select re-marking its requested key after
  `setOptions`, a Table marking or unmarking a row that arrives or goes. A binding that writes back
  what the user typed therefore cannot loop.
- `setVisible(false)` hides the widget and everything inside it; `setEnabled(false)` stops input to
  the widget and everything inside it. **A widget inside a hidden, disabled or collapsed container,
  inside a closed dialog or drawer, or inside a closed collapsible section other than in its
  header, takes no input**, however deep it sits; it takes input again once every container around
  it is shown, enabled, expanded and open. A collapsed panel's own collapse control, and a closed
  section's own control and header, still work.
- **Banners.** A dismissal of a dismissible banner reaches `onDismiss` and leaves the banner shown:
  the application hides it. A banner or empty state with an empty action label shows no button.
- **Drop zones.** A drop is taken whole or refused whole: every file must have an accepted
  extension (compared without regard to case; no extensions accepts every file), and a zone that is
  not `multiple` refuses a drop of several files. A taken drop reaches `onDrop` with the paths in
  the order dropped.
- **Splitters.** `setSizes` with an empty list, or with one whose length differs from the number of
  panes, shares the space equally. A user's drag reaches `onResize` with every pane's size once the
  user lets go.
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
| `StackWidget` | In order along the axis fixed at creation, `setGap` apart; `setAlign` places them across the axis and `setJustify` the spare length along it. A wrapping row (`setWrap(true)`, called before its first child) fills lines in order: each shown child has a basis (a content child its natural width, a fixed child its amount, a stretch child its minimum width, no more than its natural width, else its natural width, each within its bounds and at most the row's width); a child starts a new line when its basis and a gap do not fit after those on the line (a line always holds one child; a child with no basis takes no gap and starts no line); on a line the stretch children share the spare width by weight, each stopping at its maximum, and a line with none places it by `justify`; a line is as tall as its tallest child, `align` places a child down it, and lines are a gap apart, the same step as between children; a child wider than the row sits alone at the row's width. Its natural width is its children's on one line, its natural height its lines' at the width it is given. A divided stack (`setDivided(true)`, called before its first child) draws a 1 px rule in the theme's divider colour between each two shown children, centred in the gap; the rule takes 1 px of its own, so a gap of 0 still separates the children. The terminal draws no rule: it would take a row of its own and double a list's height, and its lines are already apart |
| `GridWidget` | Row-major in `setColumns` columns sharing the width, or in `setTracks` columns sized by their tracks, `setGap` apart; `setSpan(child, n)` widens one, and a child spans 1 until told; `setAlign` places each cell's content in its row |
| `PanelWidget` | A card unless the node names a surface: filled with the card scheme's surface, which its content inherits, with the card's border, radius and shadow; on a card it draws no shadow. Inside the frame, `setPadding` in; a collapsed panel hides its content, which takes no input. `setHeader`'s widget is not part of "its content" for this: it stays shown and usable while collapsed |
| `ScrollWidget` | One scrolled viewport, on the axis fixed at creation, that keeps the focused child visible |
| `SlotWidget` | Shown as they are; hosts a Switch case or a Tabs page. A scrollable slot (`setScrollable(true)`) shows a child that does not stretch in height at its natural height in a vertical viewport of its own height, which keeps the focused widget in view; a child that stretches in height fills it, as in a slot that does not scroll, but is never shorter than its `minHeight`: while the slot is shorter than that, the slot scrolls it at that height. The slot reads its shown child's height sizing itself |
| `TabsWidget` | Page slots in the order first shown; the mount keeps exactly the selected page's slot visible, and `setSelected` moves the bar's highlight only |
| `DialogWidget` | The overlay's content; a focus trap while open; a closed dialog shows nothing and takes no input, its children included. `footer()` is a container the overlay owns and lays out after its content, showing nothing while it is empty; the mount puts the overlay's actions there. A `DrawerWidget` has the same footer. `setSheet(true)` draws the dialog as a sheet that fills the window, square, with a head holding a dismiss control (`setDismissLabel`, "Close" by default) before the title, which dismisses exactly as Escape does; a press outside is not possible. A sheet keeps every rule of the dialog, and changing it while open keeps the content and its state. The sheet's dismiss control is drawn as a ghost button (its label in the accent). On Qt Quick a modal dialog or drawer dims the window behind it with `color.scrim`; a sheet fills the window and draws none, and the terminal draws none |
| `TableWidget` | Rows, each a horizontal stack of cells in column order; `setSelectColumn(true)` once, before any row, for a table that draws its selection as check boxes; `setRowKey(row, key)` once per row, right after it is built, then `setRowHeading(row)` for a heading row, whose one cell spans every column and the select column, then `setRowTone(row, tone)` for a toned row and `setRowLabel(row, label)` for a named one, and again on every change. A hidden cell keeps its column: the cells after it stay under their own headers |

**Spacing.** `setGap` and `setPadding` take a spacing step, 0 to `kMaxSpacingStep` (5); the mount clamps a document's
value with `spacingStep`, so a backend never sees another. Each backend maps a step to its own distance: the theme's
`space.<step>` in px on Qt Quick, cells by axis on the terminal.

**Button emphasis.** `ButtonWidget::setEmphasis(ButtonEmphasis)` takes `Normal`, `Primary`, `Danger`, `Ghost` or
`DangerOutline`; the mount calls it only for another value than `Normal`, so a document without the `emphasis` key
(`normal`, `primary`, `danger`, `ghost`, `dangerOutline`) records nothing new. `DangerOutline` is a destructive action
that is not the screen's main one: Qt Quick draws it as a normal button (the control surface's fill, `color.controlSurface`, and a 1 px border in `color.controlBorder`) with the
border in `color.err.border` and the label in the error colour, and the terminal in the error colour and not bold,
which keeps it apart from both `Danger` and `Normal`. Qt Quick fills a primary button in the accent and a danger one in the error
colour, and draws a ghost button with its label in the accent, no border or fill, minimal padding, and a faint tint in `color.hover` on hover or press
— for an action embedded in other content (a table row's remove, a disclosure chevron) where full button chrome would
read as a second control beside it rather than part of it. The terminal draws `Primary`/`Danger` bold, a danger one
in the error colour, and falls back to `Normal`'s plain style for `Ghost`.

**Badge style.** `BadgeWidget::setStyle(BadgeStyle::Outline)` draws a badge outlined: the surface's fill with the
tone's outline colour as border and its colour as text, where a filled one is drawn in the tone's fill. A red outline then means one thing
(STAT) and a red fill another (out of specification). The terminal draws an outline badge as `(text)`, a filled one
as `[text]`.

**Shape, size and mono.** `ButtonWidget::setShape`, `BadgeWidget::setShape(ControlShape::Round)` draw a control with
corners of half its height, at least as wide as it is tall: a circle for a short label, a stadium for a longer one, and
the surface it sits on is drawn in that shape too. `ButtonWidget::setSize(ButtonSize::Small)` makes a push button
dense: the least side padding, a strong label in the small type step, and the control height kept. `setMono(true)` on a
button or a badge draws the label in the `mono` text role's face. The mount calls each once, and only for the
non-default value. The terminal has no corners and only one face, so a shape and `mono` draw nothing there; a small
push button is `[S1]` where a normal one is `[ S1 ]`.

**Button style and selection.** `ButtonWidget::setStyle(ButtonStyle)` makes a button an entry of a list (`Item`): it
behaves as a push button (activation, enablement, tooltip, hover) and is drawn as a line, as wide as its line, its
label at the start and unframed. The mount calls it once, only for `Item`. `setSelected(bool)` marks an item as the
current entry, or a push button as pressed in (a well picked on a plate), or clears the mark; the mount calls it for
either style, only when `selected` is bound or `true`, and a new button is a push button and not selected. Qt Quick
tints the current item in `color.selection` with an accent bar at its leading edge, and draws a selected push button with its frame
and size unchanged, a 2 px accent ring outside its border and, unless it is filled (primary, danger), the item's
accent tint, so selection reads apart from focus, whose border sits inside; assistive technology hears a selected push
button as checked. The terminal draws an item, and a push button's caption inside its brackets, in the list's
selected-item style.

An item may hold content: `ButtonWidget::content()` is a container the button owns, asked for only when an item has
content, as an overlay's `footer()` is asked for only when an action bar mounts. The button draws it as the entry in
place of its label, which stays its accessible name and is not drawn, and it stays one button: one focus stop, one
activation, one current mark across all its lines. A click anywhere on the content activates the button: its
widgets take no input of their own, which the loader guarantees for documents. Qt Quick puts the content inside the
`Button`, whose background and bar span its height; the terminal paints the item's line style across every row and
the content over it, its cells keeping that background; RecordingBackend makes a `Column` child of
the button, and its `click` on a widget inside an entry's content clicks the entry.

**The cross axis.** A child sized `content` fills its container across the container's axis; `fixed(n)` gets `n`
units, at most the space there is; `stretch` is unchanged. In a column every content child is as wide as the column,
in a row as tall as the row; a grid cell, and the content of a panel, scroll area, slot, tab page, dialog or drawer,
fills its width. A kind whose width is its label (a button, a badge, a checkbox) fills the line but draws at its
natural width at the line's start, on Qt Quick as on the terminal. Along the axis a stack packs its children at the
start, `setGap` apart; only a `stretch` child takes spare length, so a `stretch` child of a column takes the column's
spare height, and a stretching scroll above a bar keeps the bar at the column's end. A content-sized splitter keeps each pane at its own
size until the user drags. A splitter's panes are sized by the splitter, not by this rule. A panel's or a collapsible's content starts under its
header whatever the panel's height; only a stretching child takes the spare height.

**Table cells.** A table draws a 1 px rule under each row in `color.divider` on Qt Quick. A table sizes and places its cells itself. On Qt Quick a cell is inset by `table.cellPaddingX` and `table.cellPaddingY`, and the header's labels with it; the padding is added to a column's declared width on both sides (a `fixed(n)` column is `n` units plus twice the inset, a content column its widest cell plus twice the inset), so it never takes room from the content; the terminal keeps one column between cells, no vertical padding and no inset in a column's width. A cell fills its column's width, except a kind whose
width is its label (a badge, a check box, a push button), which draws at its natural width at the column's start,
centre or end, as `TableColumn::align` says. The column's `align` also places the text of a text cell and of a text
input cell (where the frontend can align an input's text), and the header's label; a container cell fills its
column and places its own content. Every cell keeps its own height and is placed down its row by its column's
`valign`: at the top, centred (the default; rounded down on the terminal) or at the bottom, or, for `Stretch`, at
the row's whole height. No column is squeezed below its header: a content column is at
least its header, a stretch column takes what the others leave but never less than its header, and a fixed column
keeps its width (a fixed column narrower than its header cuts the header). A table whose columns are wider than the
table scrolls sideways on Qt Quick; on the terminal the columns past its edge are cut and cannot be reached.

**Alignment.** All of the above is the default, `Align::Stretch` and `Justify::Start`; the mount calls `setAlign` and
`setJustify` only for another value, as it does `setEmphasis`, so a screen that says nothing of alignment records
nothing new. `StackWidget::setAlign(Align)` places every child across the axis, and a child's `alignSelf` (in its
`LayoutHints`) places that child instead: under `Start`, `Center` or `End` a content child keeps its own extent across
(a text is one line tall in a row, its natural width in a column) and sits at the start, centred (rounded down on the
terminal) or at the end; a `fixed` or `stretch` child keeps its extent, clamped into its bounds, and is placed the
same way, so a `stretch` card with `maxWidth: 44` in a 70-unit column under `Center` is 44 units wide with 13 either
side. `StackWidget::setJustify(Justify)` places the spare length along the axis while no shown child stretches (one
that stretches takes it all): packed at the start, centred, at the end, or for `Between` in equal gaps, each at least
`setGap`'s, the first child at the start and the last at the end. `GridWidget::setAlign` places each cell's content
vertically in its row as a row places its children; a grid's columns are sized by their count or tracks, so it has no
`setJustify`. `GridWidget::setTracks` sizes each column as a stack sizes its children: fixed tracks their amount,
content tracks their widest cell that spans only them, and stretch tracks what is left, by weight; a spanning cell
covers its tracks and the gaps between them and sizes none. Across its track a cell sits as across a table column
with that `align`: a text's text and a label-width cell (a badge, a check box, a push button) at its natural width at
the start, centre or end, a spanning cell by its first track, and a container cell fills the track.

**Bounds.** A bound clamps the extent the sizing rule gives, in backend units. Along a stack's axis a `stretch`
child's share stops at its maximum and the rest goes to the other `stretch` children; a content or fixed child takes
at least its minimum and at most its maximum; a stack whose minimums do not fit overflows and clips, as a fixed
child does. Across, a child its maximum stops from filling is placed by its alignment, at the start by default: a card sized `stretch` with
`maxWidth: 44` in a 70-unit column is 44 units wide, on both backends.

## Interfaces

| Interface | Setters | Handlers |
|---|---|---|
| `Widget` | `setVisible`, `setEnabled`, `setLayout(LayoutHints)`, `setDragKey(optional<Key>)`, `setAccessibleName`, `setAccessibleRole`, `setTestId`, `setTooltip`, `setSurface` (may be called again, and restyles the widget and everything inside it; empty is the theme's default), `focus()` | `setDropHandler(accepts, onDrop)`, `setKeys(chords, onChord)` (the chord pressed) |
| `ContainerWidget` | `moveChild(child, index)` | — |
| `FieldWidget` (base of Text and the six inputs) | `setReadOnly`, `setRequired`, `setErrors(messages)`, `setStale`, `setDescription`, `setTone` (a text refuses `setDescription`; its tone is its colour, whatever its role) | — |
| `TextWidget` | `setText`, `setRole`, `setSpans` | — |
| `ButtonWidget` | `setLabel`, `setEmphasis`, `setIcon` (once, for a button with an icon), `setStyle` (once, for an item), `setShape`, `setSize`, `setMono` (each once, only for a non-default value), `setSelected`, `content()` (an item's content, asked for only when it has some) | `setOnClick` (Enter, Space or a click) |
| `TextInputWidget` | `setText` (with the text shown, it keeps the cursor), `setPlaceholder`, `setUnit` (the unit shown, muted, at the trailing edge inside the field; single-line and password only), `setPrefix` (the prefix shown, muted, at the leading edge inside the field, after the icon, and announced before the field's description; single-line and password only), `setMono` (once, only for `true`: the text and the placeholder in the `mono` role's face), `setAlign` (once, only for `Center` or `End`; single-line and password only: where the text sits; `End` draws digits at one width each), `setDecimals` (a number's entry places, for a backend or kit to read; single-line and password only), `setIcon` (an icon of the theme's icon set, drawn at the leading edge; single-line and password only, called once), `setCaption(label, shown)` (the field's label, and whether the input draws it as a visible caption above its control), `setEmphasis` (once, only for `Prominent`: drawn as the focal control, on Qt Quick larger and outlined in the accent, on the terminal bold) | `setOnChange` (the whole new text after each user edit), `setOnSubmit` (Enter in single-line mode), `setOnCommit` (Enter, before the submit, or focus leaving the field after an edit) |
| `CheckboxWidget` | `setLabel`, `setChecked` | `setOnToggle(bool)` |
| `SelectWidget` | `setOptions`, `setSelected(optional<Key>)`, `setCaption(label, shown)`; a dropdown's list opens under the field, at the field's start when it fits in the window, else ending at the field's end, and always inside the window | `setOnSelect(Key)`, `setOnOpen()` (optional, see below) |
| `MenuWidget` | `setItems(entries)`, `setTrigger(MenuTrigger{label, icon, emphasis})` (a button-style menu's); a popup opens under its control, at the control's start when it fits in the window, else ending at the control's end, and always inside the window | `setOnActivate(path)` |
| `StackWidget` | `setGap`, `setAlign(Align)`, `setJustify(Justify)`, `setWrap(bool)`, `setDivided(bool)`, `setPadding(Padding)` (the vertical step above and below, the horizontal step at the sides; once, before any child, only for a padded stack; the stack's natural size includes the inset) | — |
| `GridWidget` | `setColumns` (at least one), `setTracks(tracks)` (at least one, in place of `setColumns`), `setGap`, `setSpan(child, span)`, `setAlign(Align)` | — |
| `SpacerWidget` | — | — |
| `PanelWidget` | `setTitle`, `setHeader(Widget&)` (shown beside the title, called at most once), `setPadding`, `setCollapsible`, `setCollapsed` | `setOnToggle(collapsed)`, with the state the user asked for |
| `ScrollWidget` | — | — |
| `SlotWidget` | `setScrollable(scrollable)` (a new slot does not scroll) | — |
| `TabsWidget` | `setTabs(labels)` (may be called again; a call with the same number of labels replaces the captions and keeps the highlight, the shown page and the keyboard focus), `setSelected(index)` (past the last highlights none), `setBarShown(shown)` (whether the strip of tab buttons is drawn; the selected page shows either way, and a new tabs widget shows its strip), `setMoreLabel(label)` (the label of the menu that holds the tabs that do not fit in the bar; a frontend that does not draw one stores it) | `setOnSelect(index)` |
| `DialogWidget` | `setOpen`, `setTitle`, `setDescription` (a line under the title in the head, not scrolled with the content, and the overlay's accessible description), `footer()`, `setSheet`, `setDismissLabel` (called only when the document binds or sets them) | `setOnDismiss` (Esc on the TUI), for an open dialog, which the dismissal closes first |
| `ToastWidget` | `show(text, tone)` (one call: shows the message and restarts the dismiss interval regardless of whether the toast is already on-screen) | — |
| `BusyWidget` | `setActive`, `setLabel` | — |
| `TableWidget` | `setColumns(vector<TableColumn>)`, `setSelectionMode`, `setRowKey(row, key)`, `setRowTone(row, optional<Tone>)` (after `setRowKey`, and on every change), `setSelection(keys)`, `setSelectColumn(bool)`, `setSelectAllLabel(text)`, `setRowLabel(row, text)`, `setRowHeading(row)` (once, after `setRowKey`: the row is never marked, reported or activated), `setExistingKeys(keys)` (every key, in row order, above the virtualization threshold), `setSort(optional<TableSort>)` (marks the column's header with the direction, or none; only for a table with a sortable column) | `setOnSelectionChange(keys, SelectionChange)`, `setOnActivate(Key)` (Enter or a double click), `setOnSort(column)` (a click on a sortable column's header, or Enter or Space on it where the header takes focus; the index counts the columns `setColumns` gave, which the select column is not one of; another header reports nothing). The backend never reorders rows for a sort: it shows `setSort`, and what a click means is the lowering's |
| `DateTimeInputWidget` | `setValue(optional<Timestamp>)` (`nullopt` or an empty `Timestamp` shows an empty field), `setCaption(label, shown)`; the calendar opens under the field, at the field's start when it fits in the window, else ending at the field's end, and always inside the window | `setOnChange(optional<Timestamp>)` (`nullopt` for a cleared field) |
| `SliderWidget` | `setRange(minimum, maximum, step)`, `setValue` | `setOnChange(value)`, `setOnCommit(value)` (a finished move: a release after a drag, a key step, or a wheel step) |
| `FilePickerWidget` | `setPath` | `setOnPicked(path)` |
| `BannerWidget` | `setTone`, `setText`, `setSpans`, `setActionLabel`, `setDismissible` (once) | `setOnAction`, `setOnDismiss` |
| `BadgeWidget` | `setTone`, `setText`, `setIcon` (once), `setStyle` (once, only for `Outline`), `setShape` (once, only for `Round`), `setMono` (once, only for `true`) | `setOnRemove(onRemove, label)`: only for a removable badge, which then draws a remove control after its text (the framework icon `close`) named `label`, and takes the focus; Delete, Backspace, Enter or Space on it, or a click on its control, calls `onRemove`. A badge without it takes no focus |
| `ProgressWidget` | `setValue(optional<double>)`, `setLabel` | — |
| `StepsWidget` | `setSteps(vector<Step>)`, `setCurrent(optional<size_t>)`, `setLabelled(bool)` (pure; the mount calls it only when the document gives `labelled`) | — |
| `KeyValueWidget` | `setItems(vector<KeyValueItem>)`, `setCaptionPlacement` (once, only for `Above`) | — |
| `EmptyStateWidget` | `setTitle`, `setText`, `setIcon` (once), `setActionLabel` | `setOnAction` |
| `ChartWidget` | `setData(ChartData)`, at mount and on every change (`view_tree.md`, Chart data); it takes no focus | — |
| `DrawerWidget` | `setOpen`, `setTitle`, `setDescription` (as a dialog's), `footer()` | `setOnDismiss` (Esc on the TUI), for an open drawer |
| `SplitterWidget` | `setSizes(vector<int>)` | `setOnResize(sizes)` |
| `CollapsibleWidget` | `setTitle`, `setHeader(child)` (at most once), `setOpen` | `setOnToggle(open)`, with the state the user asked for |
| `DropZoneWidget` | `setAccept(extensions)`, `setMultiple` (both once) | `setOnDrop(paths)` |
| `CustomWidget` | `setTextProp(name, text)`, `setBoolProp(name, bool)`, `setNumberProp(name, double)` | `setOnEvent(event, payload)` |

**The width class.** `IViewBackend::widthClass()` is the class of the surface the backend draws on, now, and
`setOnWidthClass(listener)` registers the one listener told of each later change, on the owner and outside any layout
pass; empty clears it. Registering does not call the listener: the caller reads `widthClass()` for the current class,
so the initial value has one write path, before the first mount. Both are pure, so a backend cannot silently report
nothing. Every backend classes its width in units through `ui::widthClassOf(units)` (`compact` below 80, `medium` below
160, `expanded` otherwise), so the thresholds live in one place: the terminal its viewport's columns, Qt Quick its host
item's width over `Theme.unitWidth`.

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
  `setSelection` marks. A heading row is never marked: a click on it does nothing, the current row
  skips it, and Ctrl+A, a range and the select-all box pass over it.
- A Table shows the user's selection whether or not the application binds `selection`. A user's
  selection replaces the requested keys, and `onSelectionChange` then gets exactly the keys of the
  existing rows now selected, in any mode: a key still waiting for its row is dropped by a user's
  change, while `setSelection` keeps it waiting. A backend may report each step of the user's
  picking (a click, then a Ctrl-click); the last report is the whole selection, and no report names
  a key without a row. A paged table's backend sees one page: the interpreter keeps the selected keys
  of the rows on other pages when a report is an `Extend`, so a backend reports only the keys it
  holds, by the same drop rule.
- **The table's selection model** is the same on every backend; the document says nothing of it:

  | Input | `Single` | `Multiple` | Reports |
  |---|---|---|---|
  | Click | selects the row | selects only the row, and makes it the anchor | `Replace` |
  | Ctrl+click | as a click | toggles the row, keeps the rest, and makes it the anchor | `Extend` (`Replace` in `Single`) |
  | Shift+click | as a click | selects the rows from the anchor to this one, in place of the rest | `Replace` |
  | Up, Down, Home, End, PageUp, PageDown | move the current row | move the current row | — |
  | Space | selects the current row | toggles the current row, and makes it the anchor | `Extend` (`Replace` in `Single`) |
  | Shift+Up, Shift+Down | as Up and Down | move the current row and select from the anchor to it | `Replace` |
  | Ctrl+A | goes on outwards | selects every row; with every row selected already, clears them | `Extend` |
  | Enter | activates the current row | activates the current row | — |

  A **select column** (`setSelectColumn(true)`) draws the same selection as a leading column of check boxes. A row's
  box is checked while the row is selected, and toggling it is a Ctrl-click on the row. The header box is checked
  while every row shown is selected, partly checked while some are, clear while none are, and disabled while the
  table shows no rows; toggling it is Ctrl+A. Neither box changes its own state, so a selection the document refuses
  leaves both as the selection is. A row's box carries the row's name (`setRowLabel`, else its key).

  Only a plain click is half of a double click, so two quick Ctrl-clicks toggle a row twice. Shift with an arrow and
  no anchor starts the range at the row the current row leaves. A range and Ctrl+A run over the keys in row order,
  rows the backend has not realized included (`setExistingKeys`). The kind (`SelectionChange`) travels with every
  report: a `Replace` is the whole selection, and an `Extend` keeps every selected key the backend does not hold.
  A modified click reaches a terminal application only where the emulator passes it on (xterm keeps Shift+click
  for its own text selection), so on a terminal the dependable toggle is Space and the dependable range Shift with
  an arrow.

## Drag and drop

A widget with a drag key can be dragged; a widget with a drop handler is a target. While dragging,
the backend asks the target's `accepts(key)` before highlighting it and before a drop. A drop it
refuses delivers nothing; an accepted one calls `onDrop(key)` once. The mount always passes a
predicate — an accept-everything one when the node's `accepts` is empty — so a backend never sees
an empty one. A source or target the user cannot reach (hidden, disabled, or inside such a
container) takes part in no drag.

## RecordingBackend

The headless reference backend and the test double for everything above the contract.

- **Width class.** It has no width, and reports `expanded`, the class of every screen before a client set one, until
  `setWidthClass(name)` changes it; a change calls the `setOnWidthClass` listener, so a test drives a resize.

- **Ids and kinds.** Widget ids are creation numbers from 1. A stack's kind is `Column` or `Row` by
  its axis; every other kind is the interface name without `Widget` (`Text`, `Slot`, `Table`, …).
- **Operation log** (`log()`, `clearLog()`), one line per operation, oldest first:
  `create Kind#id in Parent#id` (`in root` for a root), `set Kind#id name=value`,
  `move Kind#id to n`, `destroy Kind#id`. Factory parameters are recorded as properties without a
  log line. Handlers are stored without a log line, except that `setDropHandler` records the
  property `drop=handler` and `setKeys` the property `keys`, the chords joined by commas. `focus()`
  logs `focus Kind#id`, `setOnHover` keeps its handler for `hover(id)` and `setOnOpen` for `open(id)` without a log line, and `focused()` returns the id of the widget that last took focus, or 0. What a widget changes by itself — an interaction helper's effect, a
  Select or Table re-marking its selection — changes a property without a log line.
- **Golden dump** (`dump()`): one line per live widget, `Kind#id name=value …`, properties in name
  order, children indented two spaces per depth, every line ending in a newline. A child whose
  parent was destroyed first becomes a root.
- **Properties.** Every kind records `visible`, `enabled`, `layout` (as `width/height`, each followed by ` min=<n>`
  and ` max=<n>` for its bounds, then ` self=<align>` for an `alignSelf`), `dragKey`, `drop`, `a11yName`,
  `a11yRole`, `testId`, `tooltip`, `surface` and `keys` when their setters run; text and the six
  inputs record `readonly`, `required`, `errors`, `stale`, `description` and `tone`; and each kind its own: `text`, `spans`, `role` (Text); `label`, `emphasis`, `icon`, `style`, `selected` (Button); `mode`, `text`, `placeholder`,
  `unit`, `decimals`, `icon`, `emphasis`, `caption`, `captionShown` (TextInput); `label`, `checked` (Checkbox); `style` (`Dropdown`, `Radio` or `Segmented`), `options`, `selected` (Select, where
  `selected` is the option it marks, else `none`); `items` (Menu, as below), and a button-style menu's `style=Button` (at creation), `label`, `icon` and `emphasis`; `gap`, `align`, `justify`, `padding` (Column, Row); `columns`,
  `tracks` (each width, then `;align=` where it is not `start`), `gap`, `align`, and `span` on a child (Grid); `title`, `padding`, `collapsible`, `collapsed` (Panel);
  `axis` (Scroll); `tabs`, `selected`, `barShown` (Tabs); `open`, `title`, `description` (Dialog); `active`, `label` (Busy);
  `columns` (each `label:width`, then `;align=` and `;valign=` for a place other than the default),
  `selectionMode`, `selection` (the requested keys it marks, in the order requested),
  and `rowKey` on a row (Table); `mode`, `offset`, `value`, `pattern` (DateTimeInput); `range`, `value`
  (Slider); `mode`, `path` (FilePicker); `tone`, `text`, `spans`, `actionLabel`, `dismissible` (Banner);
  `tone`, `text`, `icon`, `style`, and `removable` (the remove control's name) of a removable one (Badge); `value`, `label` (Progress); `steps`, `current` (Steps); `items`
  and `captions` (KeyValue); `title`, `text`, `icon`, `actionLabel` (EmptyState); `side`, `open`, `title`, `description` (Drawer);
  `axis`, `sizes` (Splitter); `title`, `header` (the header child's id), `open` (Collapsible);
  `accept`, `multiple` (DropZone); `component`, `minVersion` and each prop as `prop.<name>` (Custom);
  `yRange`, `xRange`, `yLabel`, `yTicks`, `lines`, `series`, `summary` (Chart, each set only when its text
  changed, so a test sees which parts of the data moved).
- **Components.** `createCustom` loads only the components named by `loadsComponents(names)`, none
  until it is called, so a custom node shows its fallback by default. A loaded one is a `Custom`
  widget, and `raiseEvent(id, event, payload)` raises its event as the component would.
- **Values.** `true`/`false`; an enumerator by its name; a key as a decimal integer or a
  double-quoted string (`7` and `"7"` differ); `none` for an absent key or date; a date as an
  ISO-8601 UTC instant with milliseconds, `empty` for an empty `Timestamp`; sizing as `content`,
  `fixed(n)` or `stretch(n)`, a layout as `width/height`; lists as `[a,b]`; options as
  `[key:label,…]`; columns as `[label:sizing,…]`; a slider's range as `minimum..maximum/step`;
  a fraction in its shortest decimal form (`0.25`); steps as `[label:State,…]`; key-value pairs
  as `[label:value,…]`, a value with spans as its runs in brackets and a tone other than `Neutral` following as
  `;tone=<Tone>` (`[Serial number:[DEAB-902114;mono],Containers:180 needed;tone=Err]`), with `;` escaped in a caption
  or value so it cannot forge a tone; sizes as `[n,…]`; a chart's ranges as `[lo,hi]`, its ticks as `[text@value,…]`, its lines
  as `[label@value;tone;style,…]` (`err`, `dashed`), and its series as `[label;joined;[point|…],…]`, each point
  `xLabel@x,y;tone;note` with `y` as a number, `gap` for none, or the axis end followed by `↑` or `↓` for a clipped
  point, and the tone empty for an unmarked one; every chart number shortest-round-trip, so a log is the same on
  every platform; runs as `[Lot ,L-26-0381;mono]`, each run's text followed by `;strong` and
  `;mono` when set (a node with spans records `spans` and no `text`); a
  menu as `[label;attr=value…,…]`, where an entry's attributes follow its label only when they
  differ from the default (`;icon=`, `;keys=`, `;checked=true` or `false`, `;enabled=false`,
  `;description=`), a separator is `;separator` alone, and
  its submenu follows in brackets: `[File[Open;keys=Ctrl+O,Recent[a.txt]],;separator,Wrap;checked=true]`.
- **Escaping.** Free text is escaped so that every value stays on its line and reads as one field:
  a line feed becomes `\n`, a carriage return `\r`, and a backslash or `=` is preceded by a
  backslash. Text inside a list also escapes `,`, `[` and `]`, text inside a menu or a run also `;`, text inside a chart
  property also `;`, `@` and `|`, and a
  string key also escapes `"`.
- **Lookups.** `find(kind, name, value)` is the first live widget, by id, of that kind whose
  property equals `value`, compared against the escaped form; `all(kind)` lists them in creation
  order; `prop(id, name)` is the escaped value, or empty when never set, and `hasProp(id, name)`
  tells the two apart; `exists(id)`; `idOf(widget)`, `kindOf(id)`, `children(id)` and
  `widget(id)`. `prop`, `hasProp`, `kindOf`, `children` and `widget` throw `std::out_of_range`
  for an id that names no live widget, where `exists` returns false; `idOf` throws it for a widget
  that is not one of this backend's live widgets.
- **Interaction helpers** act as the user would: `click` (a button, or the action button of a
  banner or empty state), `edit`, `submit` (Enter: the commit, then the submit), `commit` (focus
  leaving: the text the field shows), `moveCursor`, `toggle`, `choose`, `chooseIndex` (a menu's
  top-level entry or a tab), `chooseMenuEntry(id, path)`, `collapse`, `expand(id, open)` (a
  collapsible section), `dismiss` (a dialog, a drawer, a dismissible banner, or a removable badge, whose `onRemove` it runs), `resize(id,
  sizes)`, `dropFiles(id, paths)` (true when the zone took the drop), `selectRows` (reported as `Replace`, its last
  key the anchor), `clickRow(id, key, RowClick)` (a click with its modifiers, as the selection model has it),
  `clickHeader(id, column)` (a sortable column's index reaches `onSort`; another header does nothing, as a
  user's click on it does), `activateRow`,
  `setDateTime`, `slide`, `pick` and `drag`. A helper first changes what the native widget would
  change by itself — the field's text and cursor, the box's check mark, the marked option or rows,
  the highlighted tab, the collapsed or open state, a dismissed dialog's or drawer's `open`, the
  pane sizes, the value or path — and then calls the stored handler. It calls a copy of the
  handler, so a handler may
  destroy its own widget.
  - A widget the user cannot reach ignores every helper: one that is hidden or disabled, or inside a
    container that is hidden, disabled or collapsed, or a closed dialog or anything inside one.
    `activateRow` asks that of the row, `drag` of both widgets.
  - A read-only field also ignores `edit`, `submit`, `commit`, `toggle`, `choose`, `setDateTime`,
    `slide` and `pick`.
  - `chooseMenuEntry` and `chooseIndex` on a menu run nothing for a path that names no entry, a
    separator, an entry with a submenu, or an entry that is disabled or inside a disabled one.
  - `press(id, chord)` presses a chord with focus on widget `id`, in the key order of **Chords** above: a chord
    `id` declares; then the built-in keys of its kind (a `Button`'s `Enter` or `Space` clicks it; a single-line or
    password `TextInput`'s `Enter` commits, then submits and stops when the field has a submit handler, and goes on
    when it has none; a `Checkbox`'s `Space` toggles it; a `Table`'s `Enter` activates its first marked row, and
    stops there whether or not one is marked; a `Multiple` table's `Ctrl+A` selects every row or clears them, as
    `Extend`, and a `Single` one's goes on — a recording table keeps no current row, so it takes no arrow or Space); then the innermost ancestor that declares the chord. A handler that
    destroys `id` stops the key there. `press` returns whether any step took the key. It changes no recorded
    focus.
  - A helper used on a kind it does not apply to throws `std::logic_error`, and so does an action
    no user could take: choosing a key the Select does not offer, collapsing a panel that is not
    collapsible, selecting a key with no row, a row twice, any row in a `None` table or more than
    one in a `Single` one, activating a key with no row, dragging a widget onto itself, clicking a
    banner or empty state that shows no action button, dismissing a banner that is not
    dismissible, resizing with a size count other than the pane count, dropping no files. That is
    a mistake in the test.
  - `drag(source, target)` returns true when the source has a drag key, the target a drop handler,
    and the target's predicate accepted the key; then `onDrop` has run. An empty predicate accepts
    nothing here, so a drop that lands on a mounted target without `accepts` shows the mount's
    accept-everything default at work.

- **Text cursor.** A TextInput keeps a cursor, a byte offset into its text, read with `cursor(id)`.
  `edit` and `submit` leave it at the end of the new text, `moveCursor(id, position)` moves it
  (clamped to the text), `setText` with the text the field shows leaves it alone, and `setText` with
  another text puts it at the end.

`tests/test_ui_recording_backend.cpp` tests the log, the dump, the lookups and the helpers.

## The conformance suite

`conformanceCases()` returns every case, in a fixed order, as `ConformanceCase{name, run}`; each
`run` takes a `ConformanceProbe` and returns `nullopt` for a pass or the failure message. A
backend's test suite runs every case on a fresh probe. `RecordingBackend` passes them in
`tests/test_ui_conformance.cpp`.

`layoutConformanceCases()` are the cases of where a backend that lays widgets out draws them, read through the
probe's `boundsOf(widget)`: the widget's bounds in the backend's own units (a cell on the terminal, the theme's unit
width and height on Qt Quick), a `WidgetBounds{x, y, width, height}` from the top left of the window or screen,
compared within one unit. A probe that reads no geometry answers none, and every layout case run on it fails rather
than passing unmeasured; `RecordingBackend` lays nothing out and runs only `conformanceCases()`. Each case mounts
the container it measures inside another, since a root fills the window or screen whatever its own sizing:

1. Along a row, a `stretch` child's share stops at its maximum and its `stretch` sibling takes the rest.
2. Along a row, a content child is at least its minimum wide.
3. Across a column, a `stretch` child stops at its maximum, at the start.
4. Down a column, a `stretch` child's height stops at its maximum and its sibling takes the rest.
5. A row 3 units tall with `align: center` places a one-unit text one unit down.
6. A column with `justify: end` places its last child's end at its own end; with `between`, its first child at the
   start and its last at the end.
7. A `stretch` child capped at 20 in a 70-unit column with `alignSelf: center`, and one capped at 44 under the
   column's `align: center`, each have equal space either side.
8. In a 30-unit table column with `align: End`, a badge cell's right edge is the column's text's right edge: the
   column's right edge less the inset every cell has.
9. In a table row where one cell is five lines tall, a one-line cell of a `valign: Start` column has its top at the
   row's top, and one of a `valign: End` column its bottom at the row's bottom. Five lines, so a centred cell is two
   lines from either, more than the unit a measure may be off by.
10. A heading row's one cell, in a table of a 10-unit and a 20-unit column whose cells are containers, starts where
    the first column's cell starts and ends where the second's ends.
11. In a 20-unit column, a scroll that stretches in height above a one-line bar keeps the bar at the column's end
    however long the scroll's content is: the scroll takes the column's spare height, not its content's.
12. A scrollable slot 20 units tall shows a content-height column of 40 lines in a viewport of its own height and
    brings the focused 40th line into view; a column that stretches in height fills the slot from its top.
13. In a 40-unit grid with no gap, tracks `fixed(10)` and `stretch` give their texts cells 10 and 30 units wide.
14. A badge in a `fixed(12)` track with `align: Center` is narrower than its track and has as much space before it
    as after it.
15. A content-sized chart in a 60-unit column with `align: start` is 40 units wide and 8 tall: wide and tall enough
    to read before any sizing.
16. An open dialog drawn as a sheet fills the surface: a stretching text inside it is at least as wide as a
    stretching text beside it in the root, less 10 units for the sheet's border and padding.
17. A slot of fixed height 20 showing a one-row `ForEach` sized as the content it holds, a column of a stretching
    scroll above a one-line bar, ends the bar at the slot's end: the slot reads its direct child's sizing alone, so
    that `ForEach` carries the stretch.
18. A wrapping row with gap 1 and three `fixed(12)` texts keeps them on one line at 36 units plus two step-1
    distances, and at 30 units puts the third at the row's left under the first, the row as tall as its two lines.
19. In a wrapping row, a stretch text with `minWidth: 10` beside a `fixed(12)` text takes the line's spare width
    (the row's width less 12 and a step) at 40 units, and sits alone at the row's width, the fixed text below it,
    at 20: a stretch child's basis is its minimum width. A stretch text of two characters with `minWidth: 20` shares
    a line with a `fixed(12)` text in a 30-unit row: a minimum is a basis only up to the child's natural width.

### The probe

A backend author implements one `ConformanceProbe`. Its driving operations act as a user would,
through the backend's own input path where it has one, and each may run pending work before and
after it; its reading operations report what the widget shows now.

| Operation | Meaning |
|---|---|
| `backend()` | The backend under test |
| `runtime()` | The runtime the cases' signals and mounts belong to |
| `settle()` | Runs posted flushes and pending native events until nothing is left |
| `textOf(widget)` | A Text's or TextInput's text, a Button's or Checkbox's label, a button-style menu's trigger label, the label of the option a Select marks (empty when it marks none), a Chart's summary |
| `visibleOf(widget)` | The widget's own flag as its last `setVisible` left it: a hidden ancestor, a collapsed panel or a closed dialog around it does not make it false |
| `enabledOf(widget)` | The widget's own flag as its last `setEnabled` left it |
| `childCount(container)` | How many children a container has |
| `childAt(container, index)` | A child, or null past the end; a case drives it as it drives a root |
| `click(widget)` | Activates a button |
| `type(widget, text)` | Replaces a field's text as typing would: the field is cleared first, so it ends up holding exactly `text`; `onChange` may see intermediate texts |
| `drag(source, target)` | Drags one widget onto another |
| `dismiss(dialog)` | Dismisses a dialog or drawer (Esc on the TUI), or a dismissible banner; removes a removable badge with its remove control |
| `selectRows(table, rows)` | Selects rows by position, in the order the user picks them — at least one, and only one in a `Single` table — so the user's selection is exactly those rows |
| `clickRow(table, row, click)` | Clicks a row by position as a user would, plain (`RowClick::Only`), with Ctrl (`Toggle`) or with Shift (`Range`) |
| `clickHeader(table, column)` | Clicks a column's header as a user would |
| `selectedRows(table)` | The positions of the rows the table shows as selected, ascending |
| `moveCursor(field, position)` | Moves a text field's cursor as the arrow keys or a click would, without changing the text |
| `cursorOf(field)` | Where a text field's cursor is, as a byte offset into its text |
| `press(focused, chord)` | Presses a chord with focus on a widget, moving focus there first if the probe must |
| `hasFocus(widget)` | Whether the widget has keyboard focus |
| `chooseMenuEntry(menu, path)` | Chooses a menu entry by its path, opening a button-style menu through its trigger and each submenu on the way |
| `expand(section, open)` | Opens or closes a collapsible section with its own control |
| `dropFiles(zone, paths)` | Drops files from the operating system onto a drop zone |
| `boundsOf(widget)` | Where the widget is drawn, in the backend's units, for the layout cases; none by default, for a probe that reads no geometry |

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
14. A setter given the text a field already shows keeps the cursor where the user left it.
15. A text field shows what the application makes of an edit: a refused edit snaps back, an
    upper-cased one shows the upper case.
16. A dismissal the application refuses leaves the dialog open, with its content reachable.
17. No setter calls a handler: values, options, menu items, rows, selections, a table's sort, tab indices,
    opening and collapsing, a chart's data, including a Select re-resolving its request after new options and a Table
    re-marking rows that arrive or go.
18. A Select marks the requested key whenever its options contain it, whichever arrives first, and
    remembers it across options that lack it.
19. A Table in `Multiple` mode reports a user's change as exactly the existing rows selected; the
    application writes it back to the selection slot, as a controlled table needs.
20. A Table shows a user's selection with no selection bound.
21. A hidden Table cell keeps its column in its row.
22. No handler runs after its widget was destroyed.
23. A handler may destroy its own widget; the backend keeps the running handler alive and stays
    usable afterwards.
24. A drag onto an accepting target delivers the key once.
25. A drag onto a refusing target delivers nothing.
26. A drop target without `accepts` takes every key.
27. A chord goes to the innermost widget that declares it, and one only an outer widget declares
    reaches that widget.
28. A chord pressed inside a container hidden two levels up runs nothing; while shown it runs once.
29. Enter on a focused button that declares an Enter chord runs the chord, not the click.
30. Enter on a focused button inside a column that declares an Enter chord clicks the button, and the column's
    chord does not run.
31. In a dialog whose content column declares an Enter chord, Enter on a single-line text input with a submit
    handler commits and submits it, and the dialog's chord does not run.
32. In the same dialog, Enter on a single-line text input without a submit handler commits it, and the dialog's
    chord runs once.
33. In the same dialog, Enter on a focused check box leaves it unchanged and runs the dialog's chord; Space toggles
    it and runs no chord.
34. Enter on a single-line text input that declares an Enter chord itself runs the chord, and neither commits nor
    submits.
35. The first `autofocus` widget in document order has focus once the mount completes.
36. A menu reports only an enabled entry without a submenu, and an entry enabled later can be
    chosen.
37. Typing into a read-only input reaches no handler and leaves its value; once editable again,
    typing reaches `onChange`.
38. Dismissing a dismissible banner reaches `onDismiss` once, and the banner stays shown.
39. A closed collapsible section's content takes no click and its header does; the user's opening
    reaches `onToggle`, and once the application opens it the content takes clicks.
40. A drop zone takes a drop of one accepted file, compared without case, and refuses a file it
    does not accept and several files when it takes one.
41. In a `Multiple` table a plain click selects only its row, and a Ctrl-click adds a second row and then removes
    it, keeping the first; the click reports `Replace`, each Ctrl-click `Extend`.
42. A click on row 1, then a Shift-click on row 3, selects rows 1 to 3; a Shift-click on row 0 then selects rows 0
    and 1, the range from the same anchor in place of the rest. Each Shift-click reports `Replace`.
43. On a focused table of text cells, Ctrl+A in `Multiple` mode selects every row and pressed again clears them,
    each as `Extend`; in `Single` mode it selects nothing and reaches the enclosing column's chord, and after a click
    on row 1, Enter activates row 1.
44. A click on a sortable column's header reports its index once, a click on another column's header reports
    nothing, and showing a sort (`setSort`) reports nothing.
45. In a table of a heading row and three data rows, Ctrl+A selects the three data rows and not the heading, and a
    click on the heading row changes nothing and reports nothing.
46. Tabs labelled `A` and `B`, with `B` picked by the user, then relabelled `A` and `B · 3`, still highlight
    `B · 3`, and `B`'s page is the one shown.
47. A button-style menu labelled `More` over `[A, separator, B, C]` reads `More`; choosing `[2]` runs `B`, and
    choosing the separator `[1]` then runs nothing.
48. Dismissing a removable badge, the user removing it, runs its `onRemove` once.
49. A chart shows the summary of the data it was last given: set with summary `A`, then `B`, it reads `B`.
50. A Text given spans `Lot `, a mono `L-26-0381` and a strong ` <b>x</b>` reads `Lot L-26-0381 <b>x</b>`: its
    runs' characters joined in order, a run's markup shown as characters.
51. A text input described `Required range 2–8 °C` reads that as its accessible description; given the error
    `Outside 2–8 °C` it reads the error, a line feed and the description; the error cleared, the description again.
52. An open dialog described `Enter your password to sign.` reads that as its accessible description, and
    `Signing as Approved.` once the binding changes.
53. A segmented select over `A`, `B`, `C` with `B` marked reads `B`; Right chooses `C` and reports it once, a
    second Right at the end reports nothing, and Enter reaches the enclosing column's chord.
54. A list entry holding a column of two texts shows them in its `content()`; a click on the entry and a click on its
    first text each run the entry's action once, and inside a hidden column neither does.

### What the probe cannot reach

The probe drives clicks, typing, chords, menu choices, section toggles, file drops, drags,
dismissals and row selection only, so some rules of this
contract are each backend's own tests to make:

- that a widget the user cannot reach takes no `choose`, `toggle` or row activation, and that a
  drag onto an unreachable target delivers nothing;
- that a read-only check box, select, date-time input, slider or file picker takes no change, and
  that a read-only text input takes no submit or commit — rule 31 checks typing only;
- that rules 22 and 23 hold for handler kinds other than a click;
- that a select column's boxes are drawn and named — case 43's Ctrl+A is the header box's operation;
- that the header marks the sorted column and its direction — case 44 checks the click alone;
- that a Table column's `align` places a text's characters and the header's label at its end — layout case 8
  measures a badge's box only;
- that a hidden Table cell's neighbours are drawn under their own headers — rule 21 checks the
  row's structure only;
- that a controlled input other than a text field and a dialog snaps back — `RecordingBackend`'s own
  tests drive every kind.

Rule 22 is observable only for a backend whose input can be delivered later than the driving call,
such as a queued connection; a probe whose `click` delivers before it returns passes it without
exercising it.

## Design decisions

| Decision | Why |
|---|---|
| Typed factories, one interface per kind | A backend implements exactly what each node needs; a missing setter is a compile error, not a runtime lookup. |
| Setters never call handlers | Two-way bindings (a field bound to the signal its `onChange` writes) cannot loop, on any toolkit. |
| A setter with the shown value changes nothing | A controlled input is re-asserted after every edit; a cursor or a composition that jumped on each would make typing impossible. |
| Defaults stated by the contract | The mount skips the setters that would only restate them, which keeps logs and native call counts to what a node actually says. |
| Selection kept as a request, marked against what exists | The value and the list it names come from different bindings, in either order; neither order may lose the selection. |
| The backend keeps a running handler alive | A button that closes its own dialog or removes its own row is ordinary UI, not an edge case. |
| A recording backend as the reference | Every mount behaviour is asserted as a log or a dump without a toolkit, and the real backends are held to the same cases through the probe. |
