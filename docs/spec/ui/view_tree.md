# View tree and mount — design

Design spec for `morph::ui`'s view tree (`include/morph/ui/view.hpp`) and its mount
(`include/morph/ui/mount.hpp`): a screen is described once, as immutable nodes whose properties are
constants or slots — reactive values with a stable id — and `Mounted` turns that description into
retained widgets on any backend and keeps them current. The document interpreter builds the tree from
a UI document, one node per document node; C++ code that uses the reactive core directly (the forms
engine, tests, local tools) builds it with the same builders. A renderer that generates code instead
of driving widgets (Qt Quick) reads the same tree and the same slots, and does not use `Mounted`.

Read this before writing a view, and before changing what the mount does with a node. The widgets
the mount drives are [`backend_contract.md`](backend_contract.md); the frontend around a mount is
[`frontend.md`](frontend.md); the reactive graph underneath is
[`../reactive/signals.md`](../reactive/signals.md).

## Contents

- [Nodes](#nodes)
- [Props](#props)
- [The palette](#the-palette)
- [Keyed collections](#keyed-collections)
- [Mount](#mount)
- [Controlled inputs](#controlled-inputs)
- [Failures and misuse](#failures-and-misuse)
- [Design decisions](#design-decisions)
- [Out of scope](#out-of-scope)

## Nodes

- A node is an aggregate (`ui::Text`, `ui::Column`, …) wrapped by its builder (`ui::text`,
  `ui::column`, …) into `ui::Node`, a `std::shared_ptr<NodeData const>`. `NodeData::kind` is a
  `std::variant` over every node type. Nodes are immutable and shared: a subtree may appear in
  several places, and mounting never changes it.
- Nodes are not templated on a message type. Events are command sinks: `ui::Action`
  (`std::function<void()>`) or typed callbacks — `onChange(std::string)`, `onToggle(bool)`,
  `onSelect(Key)` and the like. The interpreter makes each one run a command list.
- Strings are UTF-8; a backend converts.
- `ui::Key` is `std::variant<std::int64_t, std::string>`. It identifies Switch cases, Select
  options, ForEach and Table rows and drag payloads. It is never a floating-point number: morph ids
  can exceed 2^53. The integer `7` and the string `"7"` are different keys.
- Every node carries `Common`:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `visible` | `Prop<bool>` | `true` | Shown |
| `enabled` | `Prop<bool>` | `true` | Accepts input |
| `layout` | `LayoutHints{width, height}` | content × content | Set once. `Sizing::content()`, `fixed(units)` or `stretch(weight)` per dimension, in backend units (a character cell on the TUI) |
| `dragKey` | `Prop<std::optional<Key>>` | `nullopt` | Engaged: the widget can be dragged, and a drop delivers this key |
| `accepts` | `std::function<bool(Key const&)>` | empty | Which keys a drop target takes; empty takes every key. Used only when `onDrop` is set |
| `onDrop` | `std::function<void(Key)>` | empty | Set: the widget is a drop target, called with the dropped key |

## Props

`Prop<T>` holds a constant or a slot:

| Constructed from | Is | At mount |
|---|---|---|
| nothing | the value-initialised constant | as any constant |
| a non-callable value `U` with `detail::ConstantConvertible<U, T>` | a constant | the setter is called once; no reactive node is made |
| a callable taking no arguments and returning something convertible to `T` — a lambda, a function pointer, a `std::function` | a slot without an id | an equality-gated `Computed<T>` plus an `Effect` that calls one setter; a Tabs index and a Dialog's `open` are an `Effect` alone ([why](#switch-tabs-dialog)) |
| `ui::slot(id, read)`, a `SlotBinding{id, read}` | a slot with that id | as a slot without an id |

- **A slot is a reactive value with a stable id.** `ui::SlotId` is a `std::uint32_t`; whoever builds
  the tree assigns it. The interpreter numbers every bound property of a document, and a renderer that
  generates code names the value it reads after it (`v.s17`). `slotId()` returns the id, or `nullopt`
  for a constant and for a slot made from a bare callable. `Mounted` does not read ids.

- **A callable is never a constant**, even when it converts to `T`: a captureless lambda converts
  to a function pointer and that to `bool`, so without the rule `Prop<bool>{[] { return false; }}`
  would be the constant `true`.
- **A constant may not silently change the kind of value.** `detail::ConstantConvertible` admits an
  implicit conversion except two that compile and then show something else: a pointer, a string
  literal included, to `bool` (`Prop<bool>{"false"}` would be `true`), and a floating-point value
  to an integral type (`Prop<std::int64_t>{2.9}` would be `2`). Other narrowing is kept:
  `Prop<std::size_t> count = 0` is the ordinary way to write a count.
- **An empty binding is refused where it is written**: an empty `std::function` or a null function
  pointer throws `std::invalid_argument` from the constructor, not when a mount first calls it.
- **A braced list cannot initialise a `Prop`**, because the constant constructor deduces its
  argument's type. Name the type: `.options = std::vector<ui::SelectOption>{{…}, {…}}`.
- `isBound()`, `constant()`, `binding()` (the slot's read) and `slotId()` read the prop;
  `evaluate()` returns the constant or reads the slot once.

## The palette

The node kinds are the base palette of the UI document design (§9): text, button, text input,
checkbox, select, menu, column, row, grid, spacer, panel, scroll, switch, tabs, dialog, busy,
forEach, table, date-time input, slider and file picker, with the properties a widget needs to show
and edit them. The document's further kinds (banner, badge, progress, …, boundary), its extra common
properties (`a11y`, `testId`, `tooltip`, `keys`, `autofocus`, `surface`), its input decorations
(`readonly`, `required`, `errors`, `stale`) and richer menu items join the tree, and `IViewBackend`,
with the interpreter that maps them; until then the interpreter lowers a further kind to its fallback
subtree of base kinds.

| Node | Builder | Props and callbacks |
|---|---|---|
| `Text` | `text` | `text`, `role` (`TextRole{Normal, Muted, Heading, Error, Success}`) |
| `Button` | `button` | `label`, `onClick` |
| `TextInput` | `textInput` | `value`, `onChange` (the whole text after each user edit), `onSubmit`, `placeholder`, `mode` (`TextInputMode{SingleLine, Multiline, Password}`, set once) |
| `Checkbox` | `checkbox` | `label`, `checked`, `onToggle` |
| `Select` | `select` | `options` (`std::vector<SelectOption{key, label}>`), `selected` (`std::optional<Key>`; a key outside the options marks none), `onSelect`, `style` (`SelectStyle{Dropdown, Radio}`, set once) |
| `Menu` | `menu` | `items` (`MenuItem{label, onSelect}`: each label beside its handler, so there are no parallel lists; the list is set once, each label may be bound) |
| `Column`, `Row` | `column`, `row` | `children` (a null child is skipped), `gap` (set once) |
| `Grid` | `grid` | `columns`, `cells` (`GridCell{node, span}`; a null node leaves no cell), `gap` — all set once |
| `Spacer` | `spacer` | none |
| `Panel` | `panel` | `title`, `padding` (set once), `child`, `collapsible` (set once), `collapsed`, `onToggle` — a collapsible panel is an accordion section |
| `Scroll` | `scroll` | `child`, `axis` (`Axis{Vertical, Horizontal}`, set once) |
| `Switch` | `switchOf`, `switchOn<E>` | `selector` (`Prop<Key>`), `cases` (`SwitchCase{key, node}`; the first case with the key wins), `fallback` |
| `Tabs` | `tabs` | `tabs` (`Tab{label, node}`, set once), `selected` (`Prop<std::size_t>`), `onSelect` |
| `Dialog` | `dialog` | `open`, `title`, `child`, `onDismiss` |
| `Busy` | `busy` | `active`, `label` |
| `ForEach` | `forEach<RowT>` | rows, key function, row view, `axis`, `gap` ([below](#keyed-collections)) |
| `Table` | `table<RowT>` | `columns` (`TableColumn{label, width}`, set once), rows, key function, cells, and `TableOptions{selectionMode, selection, onSelectionChange, onActivate, common}` ([below](#keyed-collections)) |
| `DateTimeInput` | `dateTimeInput` | `value` (`std::optional<time::Timestamp>`; `nullopt` or an empty `Timestamp` shows an empty field), `onChange` (`nullopt` for a cleared field), `mode` (`DateMode{Date, DateTime}`), `offsetMinutes` (the display zone's offset from UTC) — the last two set once |
| `Slider` | `slider` | `value` (`std::int64_t`), `minimum`, `maximum`, `step` (the three set once), `onChange` |
| `FilePicker` | `filePicker` | `path`, `mode` (`FilePickerMode{Open, Save}`, set once), `onPicked` |

The `Prop` fields — the ones a binding may drive — are `Common`'s `visible`, `enabled` and
`dragKey`, and: Text `text` and `role`; Button `label`; TextInput `value` and `placeholder`; Checkbox
`label` and `checked`; Select `options` and `selected`; a MenuItem's `label`; Panel `title` and
`collapsed`; Switch `selector`; Tabs `selected`; Dialog `open` and `title`; Busy `active` and
`label`; Table `selection`; DateTimeInput `value`; Slider `value`; FilePicker `path`. Every other
field is fixed when the node is built.

The builder for a Switch is `switchOf` because `switch` is a keyword. `switchOn<E>(selector, cases,
fallback)` is the enumeration form: `selector` is a `std::function<E()>`, `cases` pairs enumerators
with nodes, and each enumerator becomes the `int64` key of its case. An unsigned enumerator above
`INT64_MAX` wraps to a negative key; distinct enumerators still give distinct keys. An empty
selector throws `std::invalid_argument` from `switchOn`.

## Keyed collections

`forEach<RowT>(rows, keyOf, rowView, axis, gap)` mounts one widget per row and keeps it while its
key stays:

- `rows` is a binding, `std::function<std::vector<RowT>()>`, so a `Query`'s `value()` works; every
  signal it reads is a dependency of the ForEach. The overload taking a
  `Signal<std::vector<RowT>> const&` reads that signal; it must outlive every mount of the node.
- `keyOf` gives a row's `Key`. `rowView` builds a row's view **once**, from a
  `Signal<RowT> const&` that the mount keeps equal to the row. The view reads that signal inside
  bindings: what it reads directly while it is built is read once, and is a dependency of nothing.
  A null view mounts no widget for its row; the other rows keep their order.
- `RowT` is copyable. With an `operator==`, a kept row whose new entry is equal notifies nobody;
  without one, every new snapshot notifies every kept row, and only the row's own equality-gated
  bindings keep the setters quiet.
- An empty `rows`, `keyOf` or `rowView` throws `std::invalid_argument` from `forEach`.

`table<RowT>(columns, rows, keyOf, cells, options)` is the same machinery. `rows` is a binding;
`cells` builds a row's cells once, from the row's signal. The row view is a `Row` whose children
are those cells, so each row is a horizontal stack of exactly one cell per column. A row whose
cells are not one non-null node per column would shift every column after it, so it is refused as
a row that fails to mount is ([below](#failures-and-misuse)). An empty `rows`, `keyOf` or `cells`
throws `std::invalid_argument` from `table`.

A Table's selection is by key. `TableOptions::selection` is the requested keys; a key whose row is
not there yet selects nothing until such a row appears. `onSelectionChange` reports the keys of the
rows the user selected, and `onActivate` the key of a row the user activated (Enter or a double
click). How the widget keeps and shows the selection is
[`backend_contract.md`, "Selection by key"](backend_contract.md#selection-by-key).

The row type is erased behind `detail::ForEachModel`. Each mount opens a `detail::ForEachSession`
from it, which keeps the latest snapshot of the rows (`pull()`, which returns their keys) and makes
one `detail::RowSlot` per mounted row (`makeRow(index)`). A slot owns the row's `Signal<RowT>` and
the view built from it; `assign(index)` sets the signal to an entry of the latest snapshot. A
`ForEach` or `Table` whose model is null shows no rows.

## Mount

`ui::Mounted(runtime, backend, root, parent = nullptr, depth = 0)` builds the tree once into a
`reactive::Scope` it owns, at scope depth `depth`, appending the root widget to `parent`, or making a
backend root when `parent` is null. It holds `root` as long as it lives; a null root throws `std::invalid_argument`.
`root()` is the root node's widget, valid as long as the `Mounted`. It is neither copyable nor
movable: bindings point into it.

The build is untracked, so a `Mounted` constructed inside an Effect does not subscribe that Effect
to anything the mount reads.

The runtime, the backend and every signal a binding reads outlive the `Mounted`, and the runtime's
owner constructs and destroys it. A mount must run to completion: nothing it calls — a backend
factory or setter, a binding's evaluation — may destroy the `Mounted`, its runtime or its backend.
Widget callbacks are events, not part of a mount, and may ([below](#callbacks)).

### Per node

1. The widget is made by its factory, under its parent, with the node's fixed parameters (a
   TextInput's mode, a Select's style, a stack's or scroll area's axis, a DateTimeInput's mode and
   zone, a FilePicker's mode), and adopted into the scope **first**.
2. `Common`, in this order. `visible` and `enabled` only when bound or `false`; `layout` only when
   it is not content × content; `dragKey` only when bound or engaged; the drop handler only when
   `onDrop` is set, with an accept-everything predicate when `accepts` is empty. A new widget is
   already visible, enabled, content-sized and neither draggable nor a drop target, so a node that
   says nothing more costs no call.
3. The kind's own props and callbacks. Every `Prop` is applied, even one that holds its default: a
   constant calls its setter once; a binding makes a `Computed` (equality-gated, so an unchanged
   result calls nothing) and an `Effect` that calls one setter — except a Tabs index and a
   Dialog's `open`, whose `Effect` reads the binding itself ([below](#switch-tabs-dialog)). Fields
   set once (a stack's gap, a grid's columns and gap, a panel's padding and collapsibility, a
   slider's range, a Tabs' labels, a Table's columns and selection mode) are passed to their
   setters once. Each callback is handed over once, as an empty function when the node has none —
   except a Menu's `setOnActivate`, which always gets the mount's dispatcher over its items. Then
   the exceptions:
   - a Grid child's span is set only when it is not 1;
   - a Panel's `collapsed` and `onToggle` are applied only when it is collapsible;
   - a Menu sends all its labels through one `setItems`; when any label is bound, one binding over
     all of them sends every label again whenever one changes. Choosing an entry runs that entry's
     `onSelect`; an index past the last entry runs nothing.
4. Its children, appended in order.

Destroying the scope destroys in reverse creation order: every binding before the widget it
drives, every child before its parent.

### Owner order

The reactive runtime runs queued Effects shallowest scope first. The tree's own bindings are at the
depth the `Mounted` was given; a Switch case, a Tabs page, a Dialog's content and every ForEach or
Table row are in a scope one level deeper than the binding that mounts them, and their nested
content one level deeper again. So an Effect that takes content away runs before that content's own
bindings in the same flush, whichever was made first, and a binding never evaluates against state
its owner is removing. A mount placed under another scope — a screen's, a row's — passes that
scope's depth plus one, so the outer scope's Effects run first too.

### Callbacks

Every callback the mount hands a widget, a drop's `accepts` predicate included, runs:

- **inside `Runtime::widgetEvent`**: one batch, and a flush that falls due while it runs — from a
  nested event loop pumping the owner inside the callback — waits until the outermost widget event
  returns and is posted then ([`../reactive/signals.md`](../reactive/signals.md)). Flushes are always
  posted, so a write inside a handler never destroys the handler's own widget while it runs, even
  from a handler that pumps the loop itself.
- **untracked**: should a backend call a handler from inside a setter, and so from inside the
  binding Effect that called the setter, that Effect does not subscribe to what the handler reads.
- **with its exceptions contained**: one the callback throws is reported
  (`ui::detail::site::kCallbackThrew`) and goes no further. The backend's call returns normally, a
  predicate that threw refuses the drop, and what the callback wrote before throwing still flushes
  when the batch closes.

A callback may destroy what holds its own widget — remove its row, switch its case away, close
its dialog, or destroy the whole `Mounted`. The write only queues a flush, which runs after the
callback has returned; a callback that destroys the `Mounted` directly is safe as long as the
backend keeps the running handler alive, which
[the backend contract requires](backend_contract.md#handlers).

### Switch, Tabs, Dialog

Content a binding mounts — a Switch's case, a Tabs page, a Dialog's content — is built untracked,
in a `Scope` of its own, and the kind takes that scope only once the content is complete. Content
is therefore all or nothing ([below](#failures-and-misuse)), the binding's Effect depends on its own
prop alone, and destroying the content destroys its bindings before its widgets.

- **Switch** is a slot widget. A new key first tears the old case down, newest first, and then
  mounts the case with that key, or the fallback when no case has it. A key with neither shows
  nothing. The selector's `Computed` is equality-gated, so an unchanged key never remounts, and
  neither does a key that changes and changes back before the flush runs.
- **Tabs** passes its labels to the tab bar once. It mounts a page the first time its tab is
  selected, in a page slot under the bar, and keeps it: selecting another tab hides the old page's
  slot and shows the new one, so a page's widgets keep their focus, scroll position and half-typed
  text. A new page is complete before the old one is hidden. An index past the last tab hides the
  shown page and shows none; a null page is an empty slot. `setSelected` follows the index every
  time. The pages are destroyed newest-shown first, after the binding that shows them and before
  the tab bar.
- **Dialog** mounts its content when `open` becomes true and then opens the overlay; when `open`
  becomes false it destroys the content and then closes the overlay. A closed dialog holds no
  content.

A bound Tabs index and a bound Dialog `open` are not equality-gated: their `Effect` reads the
binding itself rather than a `Computed`. When content fails to mount, these two kinds tell the
application through a callback — `onSelect`, `onDismiss` ([below](#content-that-fails)) — whose
write lands inside the binding's own Effect run. An Effect's own write never re-runs it, and it
stays subscribed to the signals the binding read, so the application's next change — picking the
failed tab, opening the dialog — is seen as the change it is. A `Computed` in between would have
cached the failed value without seeing the write, and would find that next change equal to it.
The cost: when something the binding reads changes but its result does not, the Effect runs
anyway. While the value names mounted content, a Tabs only re-sends `setSelected` for the page
already shown, and a Dialog only re-sends `setOpen`; the backend contract allows a setter with an
unchanged value. While it names content that failed — a Tabs index on a page that failed, a
Dialog `open` still true over content that failed — that run tries the mount again
([below](#content-that-fails)).

### ForEach and Table rows

The container is a stack along the ForEach's `axis` with its `gap`, or the Table widget, which
gets its columns and selection mode first. The mount then adopts the model, opens a session,
makes the row list and makes one `Effect`, in that order, so they are destroyed in reverse: the
Effect stops first, then the rows go — last first, as a container's children are — and the
session outlives every slot that points into its snapshot. The Effect pulls the keys, tracked (the
rows binding is its only source), and reconciles untracked, so what a row view reads while it is
built, or what a kept row's update touches, is not a dependency of the whole list:

1. The first occurrence of each key is kept; every later duplicate is reported
   (`ui::detail::site::kDuplicateKey`) and refused. The first occurrence decides the row's value
   and place.
2. A row whose key is gone is unmounted, last first. A row whose key stays is updated in place: its slot's
   signal is set to the new snapshot entry, and the row's own bindings run later in the same
   flush. Its widget, focus and selection survive.
3. A new key gets a row `Scope` — the slot adopted first, then the row's widgets — and its widget
   is appended to the container. A Table then calls `setRowKey(rowWidget, key)`.
4. The container is brought into snapshot order with `ContainerWidget::moveChild`. The widgets on
   one longest run already in order stay; every other widget moves once, directly before its
   successor in the target order, which is n minus the length of a longest increasing run: the
   fewest moves possible. An append or a removal moves nothing; an insert at the front, or moving
   one row, is one move; reversing three rows is two.

A Table's `selection` binding is made after its rows. The widget marks a requested key whenever
its row arrives, so the order only shapes the log: the first `setSelection` already finds the
rows.

## Controlled inputs

The document is the single source of truth: a widget shows what its slot says, and a user's action
is a request the application may refuse. An input whose value is a slot is controlled:

| Kind | Value | Request |
|---|---|---|
| TextInput | `value` | `onChange`, `onSubmit` (the text) |
| Checkbox | `checked` | `onToggle` |
| Select | `selected` | `onSelect` (the key) |
| Slider | `value` | `onChange` |
| DateTimeInput | `value` | `onChange` |
| FilePicker | `path` | `onPicked` |
| Panel (collapsible) | `collapsed` | `onToggle` |
| Tabs | `selected` | `onSelect` (the bar's highlight) |
| Dialog | `open` | `onDismiss` (a request to close) |
| Table | `selection` | `onSelectionChange` |

The widget shows the request already — that is what the toolkit does when the user acts. The mount
records what each controlled widget shows: the last value it set, or the user's request since. After
the application's handler, the event posts **one turn** to the owner, behind the flush the event
caused; that turn reads the slot untracked and, when the widget shows anything else, calls the
setter with the slot's value. So:

- a request the application refuses snaps back after the turn;
- an accepted request costs no setter call at all: the binding sees the value the widget already
  shows and sends nothing;
- a request the application transforms (trims, clamps, upper-cases) shows the raw request until the
  flush, then the transform. The cursor rule of the backend contract keeps that one turn harmless for
  a text input and an input method.

A slot without a handler is read-only: the event is installed anyway, and the request snaps back. A
constant is not a slot: the widget keeps what the user did, as a QML literal does once a control is
edited. A Tabs is re-asserted to the page it shows; a Dialog whose content failed to mount stays
closed when the application keeps `open` true, rather than opening empty, and the re-assertion does
not retry the mount. The turn holds its check weakly, so a turn posted for content that is gone by
then does nothing; an event whose handler destroyed its own widget posts no turn.

## Failures and misuse

Misuse and failures in this layer are reported through the runtime's owner probe and then refused,
as in [`../reactive/signals.md`, "Misuse"](../reactive/signals.md#misuse-reported-then-refused). A
test that triggers one installs `morph::testing::OwnerProbeRecorder`.

| What happens | Site | Refusal |
|---|---|---|
| A binding throws, at mount or in a flush | `reactive::detail::site::kEffectThrew` | Its setter is not called, so the widget keeps the last value it was given, or its default. The mount goes on with the rest of the node. The next change of what the binding read evaluates it again. |
| Content a binding mounts throws partway: a Switch case, a Tabs page, a Dialog's content, a row | `reactive::detail::site::kEffectThrew` | Nothing of the content is left behind. A Switch shows nothing, a Tabs keeps the previous page, a Dialog stays closed, a row is left out ([below](#content-that-fails)). |
| A Table row's cells are not exactly one non-null node per column | `reactive::detail::site::kEffectThrew` | The row is left out, as a row that fails to mount is. |
| A ForEach or Table snapshot repeats a key | `ui::detail::site::kDuplicateKey`, once per later occurrence | The later row is skipped; the first decides value and place. |
| A widget callback or a drop's `accepts` throws | `ui::detail::site::kCallbackThrew` | It goes no further; the backend's call returns normally; a predicate that threw refuses the drop; earlier writes still flush. |

A report repeats on every run that meets the same condition: a snapshot that still repeats a key,
or a row that still fails, is reported again at the next change of the rows.

Mistakes in the description itself throw `std::invalid_argument` where they are written, not when
the tree is mounted: an empty binding in a `Prop`, an empty selector in `switchOf`'s enumeration
form `switchOn`, an empty function in `forEach` or `table`, and a null root in `Mounted`.

### Content that fails

A mount that throws goes no further than the content it was building, and leaves none of it
behind.

- **A node mounted directly** — the tree outside any content a binding mounts, including content a
  *constant* Switch selector, Tabs index or Dialog `open` mounts — throws out of the `Mounted`
  constructor.
- **Content a binding mounts** is built in an Effect, also during the constructor, so its failure
  is reported as `kEffectThrew` instead. When the mount is tried again depends on the kind.
  - A Switch shows nothing for that key: it tore the old case down before mounting the new one,
    and an old case shown for a new key would be stale. Its selector is equality-gated, so the
    next change of the key mounts again.
  - A Tabs keeps the previous page shown and selected, and puts the bar's highlight, which the
    user's pick may already have moved, back on it. It then calls its `onSelect` with the page
    still shown, as a widget callback, so the application's index follows the widget and selecting
    the failed tab again is a change that mounts again. All of this needs a page that was already
    shown: when the first page a Tabs mounts fails, there is nothing to restore — `onSelect` is not
    called and `setSelected` does not run, so the bar keeps whatever highlight it has.
  - A Dialog stays closed, and calls its `onDismiss`, as a widget callback, so the application's
    `open` follows the widget and opening the dialog again is a change that mounts again.
  - A Tabs index or a Dialog `open` that still names the failed content — a Tabs without
    `onSelect`, a first page that failed, a Dialog without `onDismiss`, or a callback that did not
    move the application's value — is not gated: every run of its binding tries the mount again,
    including a run caused by another signal the binding reads. That is at most one mount attempt
    per run, each failure reported again and `onSelect` or `onDismiss` called again. Without such a
    run, a Tabs keeps showing the previous page and a Dialog stays closed. An application whose
    callback puts the failed value back — posting `open = true` again, say — retries once per
    owner turn; how often to retry is that application's policy.
  - `onSelect` and `onDismiss` are called last: a widget callback may destroy the `Mounted`
    ([Callbacks](#callbacks)), and nothing of the mount is touched after them. A *constant* index or
    `open` has no application state to bring in line; its content's failure throws out of the
    constructor, and neither callback is called.
  - A ForEach or Table row is reported, left out, and mounted again at the next change of the
    rows. The other rows are mounted and ordered all the same, and the row list stays equal to the
    container's order even when a move throws. Only the first failure of one run is reported.

A row view that writes the signal its ForEach's rows binding reads directly, while it is being
built, does not make the ForEach run again for that write: the write lands during the ForEach's
own Effect run, which an Effect's write to a signal it reads never re-runs
([`../reactive/signals.md`, "The algorithm"](../reactive/signals.md#the-algorithm)). The written
rows appear at the next change of the rows.

## Design decisions

| Decision | Why |
|---|---|
| Fine-grained bindings, no virtual tree and no diff | A change reaches exactly the setter that depends on it; widget identity, focus and selection survive. |
| A constant creates no reactive node | Most props of most screens never change; they cost one setter call and nothing after. |
| A callable is always a binding, and a pointer or a floating-point value is never a lossy constant | Both mistakes compile and then show something else; the type rules make them a compile error or a binding. |
| Content in child scopes, taken only once complete | One mechanism gives "bindings before widgets, children before parents" and all-or-nothing content for a case, a page, a dialog and a row. |
| Content scopes one level deeper than their owner | Owner-first Effect order then holds across mounted content and the scopes a mount is placed under, whatever the creation order. |
| Slots carry an optional id | A code-generating renderer refers to a value by number; a C++ author who builds a tree for `Mounted` alone need not number anything. |
| Controlled inputs re-assert in a posted turn, against what the widget shows | Re-asserting inside the event would show a transform before the flush and fight an input method; comparing with what the widget shows makes an accepted edit free. |
| Lazy, kept Tabs pages | A tab that is never opened costs nothing; a tab left keeps its state. |
| Failed Tabs and Dialog content is reported to the application through `onSelect` / `onDismiss`, over an ungated binding | The application's state then matches what the widget shows, so the user's next pick or opening retries, with no mechanism beyond the callbacks the application already handles. |
| Keyed rows with a longest-run reorder | Keys keep identity across updates; the run keeps a reorder to the fewest moves a backend has to animate or re-lay out. |
| Row construction untracked | A row view that reads a signal directly must not make the whole list a dependent of that signal. |
| Callbacks contained by the mount | A backend need not be exception-safe around a handler, and a failing handler cannot take a native event loop down with it. |
| `switchOf`, not `switch_` | A keyword cannot be a function name; the spelling reads as a phrase at the call site. |

## Out of scope

Not in the palette: themes beyond `TextRole`, images, charts, and translation catalogues — a
binding produces translated text. The document's further kinds and properties join with the
interpreter ([The palette](#the-palette)).
