# Layout, form presentation, empty tables and navigation in `ui/1` — design

A screen built from a design needs to say "at most 70 units wide", "centred on the row", "this form is called
*Deposit* and its button says *Deposit*", "this table says *No bookmarks yet* when it is empty", and needs a frame
whose navigation reads as a list. This design adds that vocabulary to `ui/1` (#996), on top of the field chrome,
named parts, theme and workspace model of
[`2026-10-19-ui-host-integration-design.md`](2026-10-19-ui-host-integration-design.md), on the same branch and PR.

## Contents

[1 Intent](#1-intent) · [2 The gaps at HEAD](#2-the-gaps-at-head) · [3 Signed-out frame](#3-the-signed-out-frame) ·
[4 One-tab bar](#4-a-tab-bar-that-hides-with-one-tab) · [5 Form presentation](#5-form-presentation) ·
[6 Empty table](#6-a-tables-empty-state) · [7 Size bounds](#7-size-bounds-and-height) ·
[8 Alignment](#8-alignment-and-distribution) · [9 Field layout](#9-field-layout) ·
[10 Navigation list](#10-navigation-as-a-list) · [11 Type scale](#11-the-frames-type-scale) ·
[12 Example screens](#12-the-example-screens) · [13 Tests](#13-tests) · [14 Delivery](#14-delivery) ·
[15 Traceability](#15-traceability) · [16 Out of scope](#16-out-of-scope) · [17 Decisions](#17-decisions-recorded)

## 1. Intent

**Today**, a document cannot bound a size, place a child on the cross axis or spread children along the main axis;
a `form` cannot name itself, its button or hide its status line; a `table` with no rows is a header bar over
nothing; the frame's navigation is a column of bordered buttons with no current entry, its section headings are as
large as the application's name, the tab bar shows for one tab and touches the page, and a local client shows the
navigation, a principal and *Sign out* on its sign-in screen. Authors work around it with stretch spacers either
side of a fixed-width card (bank and bookmarks both do, `bank_screens.cpp:79-104`, `bookmarks_screens.cpp:40-43`),
which is the same example defect twice: a missing framework seam.

**With this design**, every node takes `minWidth`/`maxWidth`/`minHeight`/`maxHeight` and `alignSelf`, `sizing`
takes a height, `row`/`column` take `align` and `justify` and `grid` takes `align`; `form`, `collection` and
`wizard` take `title`, `submitLabel`, `showStatus`, `columns` and `fieldLayout`; `table` takes `empty`; `button`
takes `style: "item"` and `selected`; `text` takes `role: "section"`. The frame uses them: the navigator is a list
with the open screen marked, the type scale runs application name → section → entry, the tab bar hides with one
tab, and the frame shows its chrome only while signed in. Every new key has closed values or literal bounds, and an
invalid one is refused at load with its JSON Pointer; the same misuse through the C++ view tree throws at mount.
`bank.move`, `bookmarks.main` and the three sign-in screens use the keys and are re-shot for the PR.

## 2. The gaps at HEAD

Checked at `dfbd48b6` by reading the code; nothing here was re-shot or run.

| #996 item | At HEAD | Still missing? |
|---|---|---|
| 1 min/max size | `Sizing` is `Content`/`Fixed`/`Stretch` with no bound (`include/morph/ui/view.hpp:267-298`); `LayoutHints` is width and height only (`view.hpp:301-310`). **The document sets only the width:** `sizing` lowers to `common.layout.width` (`interpreter.hpp:5599-5601`, `interpreter.md:276-277`), so a document cannot ask for a height at all; and `sizingOf` reads anything it does not recognise as `content`, silently (`interpreter.hpp:5513-5521`) | Yes, and height sizing too |
| 2 align / justify | `Column`, `Row` hold `children`, `gap` (`view.hpp:538-559`), `Grid` `columns`, `cells`, `gap` (`view.hpp:569-579`); document keys `kContainerKeys`, `kGridKeys` (`document.hpp:747-750`). The cross axis always fills (`backend_contract.md`, "The cross axis"); `MorphStack` packs at the start (`MorphStack.qml:18-36`), the terminal fills across (`layout.cpp:117-122`) | Yes |
| 3 form presentation | `FormViewOptions` has `submitLabel`, `showTitle`, `showStatus`, `gridColumns`, `flatGridColumns` (`forms/engine/form_view.hpp:36-53`). The `form` node's keys are the issue's ten plus #1002's field `component` (`document.hpp:875-885`); `lowerForm` sets only `showTitle = false` and the component overrides (`interpreter.hpp:6380-6391`). `collection`/`wizard` take `model`, `action` only (`document.hpp:886-887`), lowered with `showTitle = false` (`interpreter.hpp:6648-6666`). **No option names a title:** `showTitle` shows the schema's (`form_view.hpp:445-448`). The status line says `ready: press Submit` whatever the label (`form_view.hpp:68`). The cell is always label over control (`form_view.hpp:381-403`) | Yes; `title` needs a new option, and the status line must follow `submitLabel` |
| 4 empty table | `kTableKeys` (`document.hpp:788-801`) has no `empty`; `lowerRows` mounts the table whatever its row count (`interpreter.hpp:6794`) | Yes |
| 5 navigation | `Navigator` makes one `createButton` per entry (`navigator.hpp:209-211`); nothing marks the open screen; titles now come from `catalogTitles` (#1003-3). **Section headings are not larger than the application's name:** both are `TextRole::Heading` (`shell_frame.hpp:83`, `navigator.hpp:198`), so they draw at one size; the defect is the missing hierarchy, not an inversion (inferred; the issue's screenshot was not re-taken) | Yes |
| 5 tab bar | The bar is `ui::Workspace`'s `TabsWidget` over `WorkspaceModel` (`workspace.hpp:68-74`), shown for any count (`workspace.hpp:420`); `MorphTabs` puts the pages under the bar with `spacing: 0` (`MorphTabs.qml:27`) | Yes |
| 5 signed-out frame | **Not in `ShellFrame`:** `showSignedIn(false)` hides the sidebar and top bar correctly (`shell_frame.hpp:141-144`). The client decides it from the catalog, `showSignedIn(!entries.empty())` (`src/client/shell.cpp:314`), and a `LocalSource` signs out to its `--user` principal (`include/morph/client/source.hpp:299`), whose catalog lists the screens before any sign-in; so the frame shows navigation, the principal (`shell.cpp:315`) and *Sign out* on a local sign-in screen. A server whose authorizer refuses the anonymous principal lists none and hides the frame correctly (inferred from `ui/service.hpp:979-986`) | Yes, as a client defect |

Two spec rows have also drifted from the code and are fixed by the commits that touch them: `document.md`'s
`button` row lacks `emphasis` (`document.hpp:711` has it), and `roleOf` accepts `title` as an alias of `heading`
(`interpreter.hpp:5472-5487`), which `document.md` does not list.

**"Literal", in every load check below,** means the key's parsed expression has `expr->kind ==
ExprKind::Literal` (`expression.hpp:138`): a JSON scalar, or a constant record or list written `{"lit": …}`. A key
the kind table types `Shape::Expr` cannot be a bare JSON record (`expression.hpp:410-414` refuses one), so the
record forms here are written `{"lit": {…}}`, as `tests/ui/corpus/documents.json:114` and `palette.json` already
write `sizing`. There is no integer shape: `columns` and the size bounds are `Shape::Expr` keys checked as literal
integers; `showStatus` is a `Shape::Flag`.

## 3. The signed-out frame

A defect in `morph::client`; no vocabulary change.

- **Rule.** The frame shows its sidebar and top bar while the shell is signed in. The shell is signed in after a
  sign-in, until a sign-out (today's `_signedIn`, `shell.cpp:424`), and **from the start** when its start screen is
  not public: an application without a public screen, or a start screen asked for by name (`--screen`) that is not
  public, is used by the principal it opened with. `buildFrame` calls `showSignedIn(_signedIn)` and
  `setPrincipal(_signedIn ? _source->principal() : "")`; `start` sets `_signedIn = !_source->isPublic(start)`
  before the first frame is built. **Routes too:** while the shell is not signed in, `mount(screenId)` sets
  `_signedIn = !_source->isPublic(screenId)` and, when that changed it, calls `showSignedIn` and `setPrincipal`
  again; so a route the history opens at start (`openWanted()`, `shell.cpp:190-192`) or a move back or forward
  (`openRoute`, `shell.cpp:498-501`) to a listed, non-public screen shows the chrome as `--screen` does. A sign-out
  sets it false until a sign-in or such a mount. The navigator's entries stay what the catalog lists.
- **Why not the principal.** A local source's signed-out principal is the `--user` name, so "has a principal"
  cannot tell signed in from signed out; whether a sign-in happened is the shell's own fact.
- **Specs.** `docs/spec/client/client.md` (the shell: when the frame shows its chrome), `ui/shell_frame.md`
  ("Signed in and signed out": what a client passes, with the client's rule linked).
- **Terminal and Qt Quick** alike: the frame is backend widgets.

## 4. A tab bar that hides with one tab

- **Backend contract.** `TabsWidget` gains `virtual void setBarShown(bool shown) = 0;` — whether the strip of tab
  buttons is drawn; the selected page shows either way. A new tabs widget shows its bar. The mount never calls it, so
  a document's `tabs` node is unchanged.
- **`Workspace`.** After each change of the model it calls `setBarShown(tabs > 1)`. The chords (`Ctrl+Tab`,
  `Ctrl+Shift+Tab`, `Ctrl+W`) work with the bar hidden. There is no option to show it for one tab: an application
  that wants its own strip uses `WorkspaceModel` (§15 of the host design), and nothing in the repository needs a
  one-tab bar.
- **Qt Quick.** `MorphTabs` gains `property bool barShown: true`, binding the `TabBar`'s `visible`; while shown, the
  pages sit `Theme.spaceOf(3)` below the bar (the `ColumnLayout`'s spacing), so the page no longer touches it.
  `focusTarget` becomes `bar.visible ? bar : pages`, so a hidden bar never holds the keyboard focus. (Measured: with
  `focusTarget: bar`, focusing the tabs widget gives the active focus to the invisible `TabBar`; a chord still reaches
  the widget's root from there, so the defect is a hidden item holding the focus, not a lost chord.)
- **Terminal.** A hidden bar gives its row to the pages (`TabsImpl::childAreas` starts at row 0); a shown one keeps
  today's single row with no blank row under it, since a terminal row costs more than the separation earns.
- **`RecordingBackend`** stores `barShown`.
- **Specs.** `ui/backend_contract.md` (Containers: `TabsWidget`), `ui/workspace.md` (Shape: the bar shows with two
  or more tabs), `qt_quick/backend.md` (Kinds: Tabs), `tui/frontend.md` (Widgets: tabs).

## 5. Form presentation

`form`, `collection` and `wizard` take the same five presentation keys. On `collection` and `wizard` they reach
every form the view draws (`options.form`, as overrides do), and `title` replaces the view's heading.

| Key | Shape | Default | Notes |
|---|---|---|---|
| `title` | expression | none | `form`: a heading over the fields (test id `form_title`); `collection`/`wizard`: replaces the schema title in `collection_title` / `wizard_title` |
| `submitLabel` | expression | `"Submit"` | the explicit-mode button, and the status line's `ready: press <label>` |
| `showStatus` | `Shape::Flag` (literal bool) | `true` | the `form_status` line |
| `columns` | `Shape::Expr`, checked as a literal integer 1 to 12 | the engine's (2, flat 1) | every group's grid and the implicit group's: `gridColumns = flatGridColumns = columns` |
| `fieldLayout` | `stacked` or `inline` | `stacked` | §9; it joins the kind table in §9's commit, so no commit has a key that loads and does nothing |

- **`document.md` rows.** `form` gains "`title`, `submitLabel`, `showStatus` (a literal `bool`; default `true`),
  `columns` (a literal integer, 1 to 12)" here and "`fieldLayout` (`stacked`, `inline`)" in §9's commit; `collection`,
  `wizard` gain the same
  keys, "reaching every form the view draws; `title` replaces its heading".
- **Load-time refusals.** `columns` not a literal integer, or outside 1 to 12 (`columns: 0`: "`columns` is 1 to 12;
  a form lays its fields in at least one column"); `showStatus` not a literal bool; and, from §9's commit,
  `fieldLayout` not one of its values (the existing `checkChoice`). 12 is the `x-colspan` convention's full row (`forms/forms.md`).
- **Forms engine.** `FormViewOptions::submitLabel` becomes `ui::Prop<std::string>` (a constant string still
  converts), and gains `ui::Prop<std::string> title` (empty: none; when set, shown whatever `showTitle` says, since a
  title given is a title wanted); `FieldLayout fieldLayout` is §9's. `statusText(session, submitLabel)` names the
  label: `ready: press Deposit`. `CollectionViewOptions` and `WizardViewOptions` gain `ui::Prop<std::string> title`
  (empty: the schema's).
- **Lowering.** `lowerForm` and `lowerComposite` bind `title` and `submitLabel` in the node's scope (a `t` re-resolves
  on `setLocale`), and pass `showStatus` and `columns` as read at load.
- **Author API.** A new `struct FormPresentation { AnyHandle title; AnyHandle submitLabel; std::optional<bool>
  showStatus; std::optional<int> columns; };`, which §9's commit extends with `std::optional<FieldLayout>
  fieldLayout` (the forms engine's enum, re-exported). `FormOptions<Action>` gains `FormPresentation presentation`; `collection` and `wizard`
  (both overloads, `author.hpp` and `author_forms.hpp`) gain a `FormPresentation const& = {}` parameter.
- **Both backends:** the keys change the view tree only, so Qt Quick and the terminal draw them as they draw any
  heading, button and grid.
- **Specs.** `ui/document.md`, `ui/interpreter.md` (Lowering: `form`, `collection`, `wizard`), `forms/engine.md`
  (Rendering: Chrome, Layout; Rendering collections and wizards), `forms/forms.md` (the `gridColumns` paragraph:
  also settable from a document), `ui/author.md`.

## 6. A table's empty state

- **Decision: child nodes, not `{title, text, action}`.** An `emptyState` node already is that record, with an icon
  and an action that runs commands; a slot of nodes takes it as is, and also takes a design's illustration or two
  buttons, with no second shape to keep in step with `emptyState`.
- **Document.** `table` gains `empty` (nodes), in the table's own scope (not a row's). `document.md` row: "`empty`
  (nodes: shown instead of the table while `rows` is an empty list)".
- **Rule.** While `rows` evaluates to an empty list, the table is hidden and the `empty` content is shown; while it
  is a non-empty list, or `null` (a query not answered yet), the table is shown and the content hidden. A hidden
  table keeps its element values: `selection` is empty because there are no rows, as today.
- **Lowering, no new view-tree kind.** `lowerRows` with `empty` returns a `ui::Column` holding the table (its
  `visible` bound to "rows is not an empty list" and the node's own `visible`) and the lowered `empty` content (bound
  to the opposite). The wrapper takes only the node's `visible`, its layout (`sizing` and bounds) and `alignSelf`, so
  it is placed as the table would be; everything else stays on the table: `testId`, `focusId` (the element id `focus`
  finds), `enabled`, `tooltip`, `a11y`, `keys`, drag and drop. The wrapper has no test id. `interpreter.md`
  (Lowering: `table`) states this split. Both backends therefore draw it with what they have. A model-backed table is not lowered
  yet (`interpreter.md`, "Not yet lowered"), and its existing report covers `empty` too.
- **Load-time refusal.** `empty` on a node without `rows` and `model`: the existing "a table takes `rows`, or a
  `model` and `action`" rule.
- **Author API.** `TableOptions` gains `Nodes empty`.
- **Specs.** `ui/document.md`, `ui/interpreter.md` (Lowering: `table`), `ui/author.md`.

## 7. Size bounds and height

### 7.1 Vocabulary

| Key | On | Shape | Meaning |
|---|---|---|---|
| `minWidth`, `maxWidth`, `minHeight`, `maxHeight` | every node | literal integer ≥ 0, in backend units | the node's extent on that axis is clamped into the bounds, whatever its sizing |
| `sizing` | every node | expression, as today; or a constant record `{"lit": {"width": s, "height": s}}` | the record form sizes both dimensions (one left out is content); the plain form stays the width |

- **Why literal bounds.** A bound is a design constant, and only a literal can be checked at load; an adaptive layout
  already varies `sizing` or `columns` with `env("widthClass")`.
- **Load-time refusals,** each at the key's pointer (literal as defined in §2): a bound not a literal non-negative
  integer; `minWidth >
  maxWidth` (and the height pair); a bound on `dialog` or `drawer` ("an overlay is sized by the frontend, not by its
  container") and on `custom` (which carries no common properties); a literal `{"fixed": n}` width outside its width
  bounds, and the same for height ("a fixed size has no room to bound"). A literal `sizing` that is
  none of `"content"`, `{"lit": {"fixed": n}}`, `{"lit": {"stretch": w}}` or the `width`/`height` record of them is
  refused (decided, §17); a non-literal one (a `record` or `cond` expression, or what `ui::obj` builds) is evaluated
  at mount, and one that evaluates to none of them is reported once and lowers to content (today it is content
  silently). The bound and fixed-inside-bounds checks see literal sizings only.
- **`document.md`.** "Every node takes" gains the four bounds and the record form of `sizing`, with the example
  `"sizing": {"lit": {"width": {"stretch": 1}, "height": {"stretch": 1}}}`; Load-time rules,
  Shape, gains the refusals above.

### 7.2 View tree and mount

```cpp
// include/morph/ui/view.hpp
struct LayoutHints {
    Sizing width{};
    Sizing height{};
    std::optional<int> minWidth, maxWidth, minHeight, maxHeight;  // backend units; nullopt: unbounded
    std::optional<Align> alignSelf;                               // §8; nullopt: the container's align
    bool operator==(LayoutHints const&) const = default;
};
```

- The mount calls `setLayout` when the hints differ from `LayoutHints{}` (today: not content × content), so an
  unbounded node still costs no call. It throws `std::logic_error` for a negative bound, a minimum above its maximum
  and a `Fixed` amount outside its bounds, naming the node's test id: the same rules as the loader, for C++ authors.
- `Sizing` is unchanged, so a table column's width takes no bound it could not honour.
- `alignSelf` joins `LayoutHints` with §8's commit; this commit adds the four bounds only.

### 7.3 Backends

- **Rule (`backend_contract.md`, "The cross axis" and a new "Bounds").** A bound clamps the extent the sizing rule
  gives: along a stack's axis, a stretch child's share stops at its maximum and the rest goes to the other stretch
  children; a content or fixed child takes at least its minimum. Across, a child its maximum stops from filling
  keeps its stretch or fill and is placed by its alignment (`alignSelf`, else the container's `align`; `stretch` then
  places it at the start): a card sized stretch with `maxWidth: 44`, centred, is 44 units wide and centred, on both
  backends.
- **Qt Quick.** `MorphWidget` gains `minWidthUnits`, `maxWidthUnits`, `minHeightUnits`, `maxHeightUnits` (−1: none),
  written by `showLayout` when given and offered when absent (so a custom component that declares no bounds is not
  reported for a node without one), and binds `Layout.minimumWidth`/`maximumWidth`/`minimumHeight`/`maximumHeight` to
  `n * Theme.unitWidth` (`unitHeight`). `QtQuick.Layouts` already shares spare length past a maximum, and places a
  fill item stopped at its maximum by its `Layout.alignment`.
- **Terminal.** `layout::Item` gains `minimum` and `maximum`; `distribute` clamps every request into them, caps each
  stretch share at its maximum and hands what a capped child leaves to the uncapped ones (at most one pass per
  stretch child), and `shrinkFromLast` stops at a minimum (the stack then overflows and clips, as a fixed child does
  today). `crossExtent` clamps into the bounds, then into the space, and `slotOf` offsets by the
  alignment any child whose cross extent is less than the space, a capped stretch child included (today it offsets
  none).
- **`RecordingBackend`.** `formatLayout` appends `min=`/`max=` per dimension and `self=` when set.
- **Author API.** `Node::minWidth(int)`, `maxWidth(int)`, `minHeight(int)`, `maxHeight(int)`, and
  `Node::sizing(AnyHandle width, AnyHandle height)` writing the record; free helpers `ui::author::content()`,
  `fixed(int)`, `stretch(int weight = 1)` returning literal (`{"lit": …}`) sizing handles, which bank and bookmarks
  hand-roll today with `ui::obj` (a non-literal record the loader cannot check).
- **Specs.** `ui/document.md`, `ui/interpreter.md` (Lowering: `sizing` sizes the width, or both from the record),
  `ui/view_tree.md` (Common: `layout`), `ui/backend_contract.md`, `qt_quick/backend.md` (Layout and theme),
  `tui/frontend.md` (Layout), `ui/author.md`.

## 8. Alignment and distribution

### 8.1 Vocabulary

| Key | On | Values | Default (today's behaviour) |
|---|---|---|---|
| `align` | `row`, `column`, `grid` | `start`, `center`, `end`, `stretch` | `stretch` |
| `justify` | `row`, `column` | `start`, `center`, `end`, `between` | `start` |
| `alignSelf` | a child of a `row`, `column`, `grid` or `forEach` | as `align` | the container's `align` |

- `align` places children across the container's axis (a row's: vertically; a column's: horizontally; a grid's:
  each cell's content vertically in its row). `justify` places the spare length along the axis: none when a child
  stretches. `between` puts it in equal gaps, never less than `gap`.
- **Decision: `align` on `grid`, `justify` not.** A grid's columns are equal shares of its width, so a row of cells
  has no spare length to distribute, while its rows do have height to align cells in. `justify` on a `grid` is
  refused ("a grid's columns share its width; there is no spare length to justify").
- **Decision: `alignSelf`, yes.** The sign-in card and a capped content column need one child centred in a stack
  whose other children fill; without it the author nests a stack per child.
- **Load-time refusals.** An unknown value of any of the three (`checkChoice`: "`middle` is not one of the values of
  `align`"); `justify` on a `grid`; `alignSelf` on a node whose parent is not a `row`, `column`, `grid` or `forEach`
  (the view root, a panel's or a dialog's content): "`alignSelf` places a node in its row, column or grid".
- **`document.md` rows.** `column`, `row`: "`children`, `gap`, `align`, `justify`"; `grid`: "`children`, `gap`,
  `columns`, `align`"; "Every node takes" gains `alignSelf`; Shape gains the three closed choices.

### 8.2 View tree, contract, backends

```cpp
// include/morph/ui/alignment.hpp (shared by view.hpp and author.hpp, as button_emphasis.hpp is)
enum class Align : std::uint8_t { Stretch, Start, Center, End };
enum class Justify : std::uint8_t { Start, Center, End, Between };
```

- `Column` and `Row` gain `Align align = Align::Stretch;` and `Justify justify = Justify::Start;`, `Grid` gains
  `Align align = Align::Stretch;`, all set once; `LayoutHints::alignSelf` (§7.2).
- `StackWidget` gains `virtual void setAlign(Align) = 0;` and `virtual void setJustify(Justify) = 0;`, `GridWidget`
  `virtual void setAlign(Align) = 0;`. The mount calls each only for a value other than the default, as it does
  `setEmphasis`, so an existing screen's recording does not change. `RecordingBackend` stores `align`, `justify`.
- **Qt Quick.** `MorphStack` and `MorphGrid` gain `align` and `justify`; `ContainerCore::adopt` writes the child's
  `crossAlign` beside `fillsWidth`/`fillsHeight`. `MorphWidget` takes its effective alignment from `alignSelf`, else
  `crossAlign`, and sets `Layout.alignment` from it (`AlignLeft`/`HCenter`/`Right` in a column, `AlignTop`/`VCenter`/
  `Bottom` in a row or grid). A non-`stretch` alignment clears the cross fill of a **content-sized** child only; a
  stretch child keeps `Layout.fillWidth` (or `fillHeight`), and its maximum with its alignment places it (§7.3).
  Adoption runs in the child's constructor (`widget.cpp:60-64`), before a container's `setAlign` (`ShellFrame` makes
  every child in its initializer list, `shell_frame.hpp:58-63`, and aligns in its body), so `setAlign` re-offers
  `crossAlign` to every child it already holds, as `passSchemeDown` re-offers the scheme (`widget.cpp:302-314`). `MorphStack` places its layout along the
  axis when no shown child stretches: at the start (today), centred, at the end, or, for `between`, at full length
  with its spacing `max(gap, spare / (shown − 1))`.
- **Terminal.** `slotOf` gives a content child under a non-`stretch` alignment its natural cross extent and every
  child its bounded extent (§7.3), and offsets one narrower than the space to the start, centre (rounded down) or
  end; `solvedAreas` offsets the first child by the spare length for `center`/`end`
  and adds `spare / (shown − 1)` to each gap for `between`, the remainder one cell at a time from the first gap.
  `planGrid` aligns a cell in its row the same way.
- **The frame.** `ShellFrame`'s top bar is `align: center` (`setAlign(Align::Center)`), so the principal sits level
  with *Sign out*.
- **Author API.** `ContainerOptions` gains `std::optional<Align> align` and `std::optional<Justify> justify`;
  `Node::alignSelf(Align)`.
- **Specs.** `ui/document.md`, `ui/view_tree.md` (the palette rows, Common), `ui/backend_contract.md` (Containers,
  The cross axis), `qt_quick/backend.md`, `tui/frontend.md`, `ui/shell_frame.md` (the top bar), `ui/author.md`.

## 9. Field layout

The cell rule of the host design §4 stands: the cell draws the caption, the control the required mark and the unit.
`fieldLayout` changes only where the cell puts its caption.

- **`stacked`** (default): today's cell, the label over the control over the help.
- **`inline`**: the cell is a row, `align: center`, gap step 3: the label at `fixed(inlineLabelWidth)` (test id
  `label_<path>`, unchanged) and the control stretching; the help, when there is one, is a second row under the
  control, indented by a spacer of the label's width. A Boolean field, whose checkbox carries its own label, gets an
  empty label spacer so its box lines up with the controls; an Object field's panel stays stacked and its members
  follow the form's layout.
- **Columns** are `columns` (§5), independent of the caption's place: `inline` in two columns is valid.
- **Document.** This commit adds `fieldLayout` to the `form`, `collection` and `wizard` kind tables, with its
  `checkChoice`, its corpus refusal, its `document.md` row and `FormPresentation::fieldLayout` (§5 lists the key).
- **Forms engine.** `enum class FieldLayout : std::uint8_t { Stacked, Inline };` in `form_view.hpp`;
  `FormViewOptions` gains `FieldLayout fieldLayout = FieldLayout::Stacked` and `int inlineLabelWidth = 16` (units;
  a value below 1 is `std::invalid_argument` from `formView`). `fieldCell` builds the row for `Inline`. The input
  API of the host design §4.2 does not change: `showLabel` stays false inside a form.
- **Limit, stated.** While a field shows an error, an inline label centres on the control and its message together;
  centring on the control alone needs a baseline the view tree does not carry.
- **Both backends** draw the row with §8's alignment; on the terminal a one-row control makes the centring exact.
- **Specs.** `forms/engine.md` (Rendering: a cell, Layout), `ui/document.md` (§5's row), `ui/view_tree.md` ("Field
  chrome": the cell may put the caption beside the control).

## 10. Navigation as a list

- **Decision: a `button` style, not a new kind.** A list item is a button in every respect but its look: a label,
  an icon, an activation, enablement, a tooltip, the hover the navigator prefetches on. A new kind would repeat
  each of these in the view tree, the contract, both backends and the restyle table; a style changes one QML
  component and one paint function.
- **Document.** `button` gains `style` (`push`, the default, or `item`) and `selected` (a condition; only with
  `style: "item"`). Refusals: an unknown `style`; `selected` without `"style": "item"` ("`selected` marks a list
  item; give the button `style: item`"). `document.md` row: "`label`, `icon`, `emphasis` (`normal`, `primary`,
  `danger`), `style` (`push`, `item`), `selected` (with `item`), `onClick`, `onActivate`".
- **View tree.** `ui::Button` gains `ButtonStyle style = ButtonStyle::Push` (set once; `enum class ButtonStyle :
  std::uint8_t { Push, Item }` in a new `include/morph/ui/button_style.hpp`, beside `button_emphasis.hpp`, so
  `author.hpp` takes it without `view.hpp`) and `Prop<bool> selected = false`. The mount refuses
  (`std::logic_error`) a bound or `true` `selected` on a push button.
- **Backend contract.** `ButtonWidget` gains `setStyle(ButtonStyle)` (called once, only for `Item`) and
  `setSelected(bool)` (only for an item, when bound or `true`).
- **Qt Quick.** `MorphButton` gains `style` and `selected`. An item fills its line (`fillsAcross` true), draws its
  label left-aligned with `Theme.spaceOf(3)` padding and no border, a `scheme.neutral` fill on hover, and while
  selected an accent tint (`Qt.alpha(scheme.accent, 0.12)`), the label in the accent and a 3 px leading bar. Both
  properties join `button`'s restyle API, so a from-scratch `button` restyle is refused until it declares them.
- **Terminal.** An item draws its label without brackets, padded to its slot's width; selected, in the table's
  selected-row style, focused, in the focus style as today.
- **Navigator.** Entries are created with `setStyle(ButtonStyle::Item)` and a stretch width. A new
  `Navigator::setCurrent(std::string_view screenId)` marks that entry selected and clears the previous one (an id
  the navigator does not list clears the mark; pins stay push buttons). The client shell calls
  `_frame->navigator().setCurrent(activeScreen())` where it follows the active tab (`showActiveTitle`).
- **Author API.** `ButtonOptions` gains `std::optional<ButtonStyle> style` and `AnyHandle selected`.
- **Specs.** `ui/document.md`, `ui/view_tree.md`, `ui/backend_contract.md` (Button emphasis gains style and
  selection), `qt_quick/backend.md` (Kinds, Components), `tui/frontend.md`, `ui/workspace.md` (Navigator: the list
  and `setCurrent`), `ui/author.md`.

## 11. The frame's type scale

- **Scale.** Application name: `heading` (body ×1.3, bold). Section: a new role `section` (body size, bold, muted).
  Entry: body. The top bar's title stays `heading`.
- **View tree.** `TextRole` gains `Section` after `Success`; the navigator's group headings use it
  (`navigator.hpp:198`).
- **Document.** `text`'s `role` gains `section`; `document.md` lists the roles: `normal`, `muted`, `heading` (`title`
  is accepted as its alias), `section`, `error`, `success`.
- **Qt Quick.** `MorphText` draws `Section` in `Theme.strongFont`; `MorphWidget.textColorOf` (`MorphWidget.qml:53-64`,
  which a restyle reads, so it is restyle API) answers the scheme's muted colour for role 5 instead of the default
  text colour. No new token: the body size and weight already exist (`ui/theme.md`, "On Qt Quick").
- **Terminal.** `roleStyle` draws `Section` as the muted style, bold.
- **An unknown role** is today `Normal`, silently (`interpreter.hpp:5487`). A literal one is refused at load
  (decided, §17); a non-literal one that evaluates to an unknown name is reported once per path and drawn `Normal`.
  This commit turns `tests/test_ui_interpreter_commands.cpp:367` (`"role": "plain"`) into a corpus refusal case and
  drops its `Normal` assertion (`:380`).
- **Specs.** `ui/view_tree.md` (Text), `ui/document.md`, `ui/theme.md` (On Qt Quick: the roles' fonts),
  `qt_quick/backend.md`, `tui/frontend.md`, `ui/shell_frame.md`, `ui/workspace.md`.

## 12. The example screens

Minimal changes, test ids kept, so the rungs' unit and GUI tests stay green.

- **Sign-in (bank, bookmarks, lims).** The root is a column sized `{"width": stretch, "height": stretch}`, `align:
  center`, `justify: center`; the card is a `panel` with `maxWidth: 44` and a stretch width, so it shrinks in a
  narrow window. Bank's spacer row and comment (`bank_screens.cpp:79-104`) and bookmarks' `centred` helper go; lims,
  today a bare row, gets the same card. With §3, the frame shows no navigation, principal or *Sign out*.
- **`bank.move`.** `page()` caps the column at `maxWidth: 80`. Each form gets `title` and `submitLabel` (*Deposit*,
  *Withdraw*, *Transfer*), `showStatus: false` and `fieldLayout: inline`, so the amount input is no longer page
  wide, the forms are told apart and none says "fill the required (*) fields".
- **`bookmarks.main`.** The toolbar row is `align: center`, the search field `maxWidth: 60`; the bulk row is
  `justify: between` over the count and a row of the two buttons, with no stretch on the count; the table's `empty` is
  an `emptyState` (*No bookmarks yet*, *Save a link with New bookmark.*, action *New bookmark* opening the drawer).
- **Re-shooting.** The repository has no screenshot path today (no `grabWindow` or `screenshot` in `src`,
  `examples`, `tests` or `scripts`; the offscreen runs are smoke tests). `examples/client` gains `--grab <file.png>`:
  it takes the window from `FrontendConfig::onEngine` and sets 1280×900; its `onMounted` posts one task to the
  executor, and that task, one turn later (after the mount's own posted work), saves `QQuickWindow::grabWindow()`
  and quits as `--exit-after-mount` does. `scripts/shoot_screens.sh <dir>` runs it with `QT_QPA_PLATFORM=offscreen`
  (the frontend already picks the Software scene graph there, `frontend.cpp:144-148`) for the five screens (`--app bank`, `--app lims`, `--app bookmarks` on their sign-in; `--app bank --user alice --screen
  bank.move`; `--app bookmarks --user alice --screen bookmarks.main`). The sign-in cards are sized
  `{"lit": {"width": {"stretch": 1}}}` with `maxWidth: 44` and the roots `{"lit": {"width": {"stretch": 1}, "height":
  {"stretch": 1}}}`, through §7.3's helpers. The exact arguments are confirmed when the
  commit is made; that `grabWindow` returns a drawn image there is inferred, not yet run. The images go in the PR description, not the repository.

## 13. Tests

Each test is run once against its named mutation before its commit, and must fail there.

- **§3 frame.** `tests/client/test_sources.cpp`: a local source with `--user alice`, a public login and a listed
  screen: on the login, `shell_sidebar` and `shell_topbar` are hidden; after the sign-in both show and
  `shell_principal` reads `alice`; after sign-out both hide. `--screen` of the listed screen starts with both shown, and
  so does a start whose history route names it (`openWanted`).
  *Mutation: restore `showSignedIn(!entries.empty())`; the first assertion fails.*
- **§4 bar.** `RecordingBackend`: one tab, `barShown=false`; two, `true`; closing back to one, `false`. Qt: the
  `TabBar` is invisible with one tab, and with two the page's top is at least `Theme.spaceOf(3)` below the bar's
  bottom. Terminal: with one tab the page's first row is row 0. Qt: a tabs widget focused while its bar is hidden
  holds the focus in a visible item, and its `Ctrl+Tab` chord arrives. *Mutations: never call `setBarShown`; for the
  focus case, keep `focusTarget: bar`.*
- **§5 presentation.** Interpreter on `RecordingBackend`: a form with `title: "Deposit"`, `submitLabel: "Deposit"`,
  `showStatus: false`, `columns: 2` has a `form_title` text "Deposit", a `form_submit` labelled "Deposit", no
  `form_status`, and its implicit grid `columns=2`; with `showStatus` left out the status reads `ready: press
  Deposit` once ready. A collection's `title` reaches `collection_title`. Corpus: `columns: 0`, `columns: 13`,
  `columns: {"ref": "n"}` (not a literal), `showStatus: "no"` refused. *Mutation: drop the options in `lowerForm`.*
- **§6 empty.** Interpreter: with `rows` `[]`, the `Table` found by its test id is invisible and the `EmptyState`
  found by its own test id is visible; with one row, the reverse; with `null`, the table is visible. The `Table`, not
  the wrapper, holds the test id and the element id `focus` finds. Qt and terminal: an `emptyState` inside `empty` is drawn (a visible item with its title; the
  title's text in the canvas). *Mutation: lower `table` without `empty`.*
- **§7 bounds.** Conformance (both backends, through a new probe read `boundsOf(widget)` in the backend's own
  units): a stretch child with `maxWidth: 20` in a 60-unit row is 20 wide and its stretch sibling takes the rest; a
  content text of natural width under 30 with `minWidth: 30`, in a row (where content does not fill along the axis),
  is 30 wide; a stretch child with `maxWidth: 44` in a 70-unit column is 44 wide at the start; a stretch child with
  `maxHeight: 5` in a 20-unit column is 5 tall and its sibling 15. §8's commit adds the centred cases: a column
  child capped below the column's width with `alignSelf: center` has equal space either side (±1 unit), and the
  sign-in card centred by the column's `align: center`. As built, these are a second list,
  `layoutConformanceCases()`, run by the Qt Quick and terminal suites only: `RecordingBackend` lays nothing out, so
  `boundsOf` defaults to none and a layout case run on a probe without geometry fails (a test checks that it does)
  rather than passing unmeasured. Each case mounts the measured container inside another, since the terminal's root
  fills the screen whatever its own sizing, and the column is 70 units, not 100, so it fits the terminal probe's
  80-column screen. Terminal `test_tui_layout.cpp`: `distribute` hands a capped share's
  remainder to the uncapped child. A document `sizing` record stretches the height: the root fills the page.
  Corpus: `minWidth > maxWidth`, a negative bound, a bound on a `dialog`, `{"lit": {"fixed": 50}}` with
  `maxWidth: 40`, `sizing: "wide"` refused; `sizing: {"lit": {"height": {"stretch": 1}}}` loads. Mount: the same through the view tree throws. *Mutation: drop the bounds in
  `showLayout` (Qt) and in `distribute` (terminal); the conformance cases fail on each.*
- **§8 alignment.** Conformance: a row 3 units tall with `align: center` places a 1-unit text at row offset 1; a
  column with `justify: end` places its last child's end at the column's end; `between` puts the first child at the
  start and the last at the end. `ShellFrame` on Qt: the vertical centre of the `Text` item inside
  `shell_principal` equals that of the `Button` control inside `shell_signOut` (±1 px); the inner items, since the
  `MorphWidget` roots fill the row and share a centre with or without alignment. Corpus: `align: "middle"`, `justify` on a grid, `alignSelf` on the view root refused. *Mutation: never
  call `setAlign`/`setJustify` in the mount; every case fails, the default-only guard (an existing screen's
  recording is unchanged) passes.*
- **§9 field layout.** `tests/test_forms_engine_view.cpp`: with `Inline`, `cell_<path>` is a Row whose first child
  is `label_<path>` with `fixed(16)` and whose second holds `field_<path>`; a Boolean's first child is a spacer; with
  `Stacked` the recording is today's. Qt: an inline label's x is left of its control's and their vertical centres
  agree (±1 px). Terminal: label and control on one row. Corpus: `fieldLayout: "grid"` refused. *Mutation: ignore
  `fieldLayout` in `fieldCell`.*
- **§10 navigation.** `RecordingBackend`: navigator entries record `style=Item`; `setCurrent("b")` gives `nav_b`
  `selected=true` and `nav_a` `false`; an unknown id clears both. Client shell: opening a screen selects its entry.
  Qt: an item button's width equals the sidebar content's, and a selected one's fill differs from an unselected
  one's. Terminal: an item paints no `[`. Corpus: `selected` on a push button, `style: "link"` refused.
  *Mutation: create entries without `setStyle`.*
- **§11 type scale.** Qt: a `section` text's `font.bold` is true and its pixel size equals a body text's, and the
  application name's is larger; the navigator's group heading records `role=Section`. `MorphWidget.textColorOf(5)` is the scheme's muted colour. Terminal: a
  section text is bold and muted. Corpus: `"role": "plain"` refused (the case moved from
  `test_ui_interpreter_commands.cpp:367`). A bound role evaluating to `"big"` is reported once. *Mutation: map `section` to `Normal`.*
- **§12 examples.** The rungs' existing screen and GUI tests (`ladder-*` labels) pass with the new screens; the
  bookmarks screen test gains: with no bookmarks, the `empty` state shows and `bookmarks` is hidden.

## 14. Delivery

One commit per item on `feat/ui-host-integration`, after the host-integration commits, cheap first. Every commit
updates the `docs/spec` files its section names and adds its corpus cases.

| # | Commit | Section |
|---|---|---|
| 1 | client: the frame shows its chrome only while signed in | 3 |
| 2 | ui, qt_quick, tui: the workspace hides its tab bar with one tab | 4 |
| 3 | ui, forms: `title`, `submitLabel`, `showStatus`, `columns` on form, collection, wizard | 5 |
| 4 | ui: a table's `empty` content | 6 |
| 5 | ui, qt_quick, tui: size bounds and a document height | 7 |
| 6 | ui, qt_quick, tui: `align`, `justify`, `alignSelf` | 8 |
| 7 | ui, forms: the `fieldLayout` key and the inline field layout | 9 |
| 8 | ui, qt_quick, tui: list-item buttons and the navigator's current entry | 10 |
| 9 | ui, qt_quick, tui: the `section` text role and the frame's type scale | 11 |
| 10 | examples: sign-in, `bank.move` and `bookmarks.main` use the new keys; `--grab` and `shoot_screens.sh` | 12 |

Commit 7 needs 6 (the inline row is centred); 10 needs all. Commits 1, 2 and 4 stand alone.

**Before the push**, every build, test and tidy run inside the shared build lock, `-j10`:

1. GCC and clang builds, each with `MORPH_BUILD_QT=ON MORPH_BUILD_QT_QUICK=ON MORPH_BUILD_TUI=ON MORPH_BUILD_NET=ON
   MORPH_BUILD_EXAMPLES=ON MORPH_BUILD_TESTS=ON` and the ladder on, warnings as errors.
2. The full `ctest --output-on-failure` in both builds (offscreen Qt Quick, qmllint over MorphUi, the ladder).
3. clang-tidy 22 on the changed lines, headers through a translation unit that includes them (`clang-tidy-diff -p1`
   against the clang build's compile database).
4. `git clang-format --diff master` clean, and QML formatted as the module's lint target requires.
5. Doxygen with `WARN_AS_ERROR = FAIL_ON_WARNINGS` (`cmake -S . -B build-doc -G Ninja
   -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF && cmake --build build-doc
   --target doc`): `Align`, `Justify`, `ButtonStyle`, `FieldLayout`, `FormPresentation` and every new member and
   function with complete `@param`/`@return`.
6. Header self-containment: `cmake --build <build> --target all_verify_interface_header_sets` (the new
   `alignment.hpp` and `button_style.hpp` included alone).
7. `git diff master -- include/morph src | grep -nE '^\+.*\b(slots|signals|emit|foreach)\b'` finds nothing.
8. `scripts/shoot_screens.sh` run once, its five images checked by eye against §2's symptoms and attached to the PR.

## 15. Traceability

| #996 item | Section | Commit |
|---|---|---|
| 1 min/max size (and a page's readable width) | 7 | 5 |
| 2 cross-axis `align`, main-axis `justify`, `alignSelf` | 8 | 6 |
| 3 `submitLabel`, `title`, `showStatus` on form, collection, wizard | 5 | 3 |
| 3 field layout: `stacked`, `inline` | 9 | 7 |
| 3 field layout: `columns: N` | 5 | 3 |
| 4 a table's empty state | 6 | 4 |
| 5 navigation as a list, with a selected state | 10 | 8 |
| 5 a type scale the frame follows | 11 | 9 |
| 5 a tab bar that hides with one tab, spaced from the page | 4 | 2 |
| 5 the signed-out frame (defect) | 3 | 1 |
| Close: `bank.move`, `bookmarks.main`, three sign-in screens re-shot | 12 | 10 |

## 16. Out of scope

- **`align`/`justify` on `forEach`, `panel` or `scroll`.** The issue names `row`, `column` and `grid`; a horizontal
  `forEach` can follow the same contract calls when a screen needs it.
- **Bounds on a table column** and a document key for the inline label width (`inlineLabelWidth` is C++ only).
- **The gallery screens.** They show the same symptoms; updating them is not part of the closing condition.
- **A baseline-aligned inline label** (§9's limit) and a navigator with collapsible sections.
- **A model-backed table's `empty`**, which arrives with the table engine (spec 7).
- **Re-shooting on the terminal.** Its behaviour is covered by the tests in §13; the issue's screenshots were Qt only.

## 17. Decisions recorded

Both were put to the owner and decided.

- **Literal unknown values are refused at load.** A literal `sizing` or text `role` outside its set is refused
  (§7.1, §11), where today both lower silently to a default (`interpreter.hpp:5513-5521, 5472-5487`). A server
  whose document carries one stops registering it, at its own registration test. One document in the repository
  does: `tests/test_ui_interpreter_commands.cpp:367` (`"role": "plain"`), which commit 9 turns into a corpus
  refusal case; the rungs' tests in commits 5 and 9 confirm no example screen carries another.
- **New backend calls are pure virtuals.** `setAlign`, `setJustify`, `setStyle`, `setSelected` and `setBarShown`
  are pure, so an out-of-tree backend stops compiling until it implements them, as `setCaption` and `footer()` did
  in the host design; a defaulted no-op would let a backend ignore alignment silently.
