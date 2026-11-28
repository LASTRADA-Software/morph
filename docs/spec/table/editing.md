# `morph::table` — editing and grids

Design spec for typing into a table: a grid that is part of a record being edited, cells of a model's table
edited one at a time, the cell editor, walking, pasting and validation, and the layouts an entry grid needs and
a read-only table uses too (header bands, spans, summary rows, fixed grids). Read [`table.md`](table.md) first,
for the pipeline this builds on, and [`../forms/engine.md`](../forms/engine.md) for fields, drafts and exact
encoding, which this reuses and does not repeat.

## Contents

- [Goal](#goal)
- [Two homes for an edit](#two-homes-for-an-edit)
- [A grid of a form](#a-grid-of-a-form)
- [Cells of a model's table](#cells-of-a-models-table)
- [The cell](#the-cell)
- [Walking](#walking)
- [Selecting cells, clipboard and fill](#selecting-cells-clipboard-and-fill)
- [Rows of a grid](#rows-of-a-grid)
- [Layouts](#layouts)
- [Document vocabulary](#document-vocabulary)
- [Invariants](#invariants)
- [API surface](#api-surface)
- [Tests](#tests)
- [What this does not do](#what-this-does-not-do)
- [Alternatives considered](#alternatives-considered)
- [Failure modes](#failure-modes)
- [Neighbouring specs](#neighbouring-specs)
- [Delivery order](#delivery-order)

## Goal

A laboratory application enters most of its data in grids: the specimens of a test, the fractions of a sieve
analysis, the layers of a core, the limits of a specification. What such grids need, in the order it matters:

- **An editor per cell that follows the record's types:** text, integers, exact decimals with fixed places,
  quantities, nullable numbers, dates, choices whose options depend on the row, check boxes. The kind can differ
  between the rows of one column.
- **Computed cells beside entered ones**, read-only, which the cursor skips, and which follow every entered value.
  The computation is often the domain's, on the server, over the whole record.
- **Validation that keeps what was typed** and says why it is wrong, at the cell.
- **A keyboard walk** that enters a column of values without the mouse, and leaves the grid at its ends.
- **Layouts a printed form has:** headers in two or three bands with units, cells spanning rows or columns,
  blocks of rows per specimen with a mean, a minimum and a deviation under each, grids whose rows are fixed by a
  standard rather than by data.
- **Paste and fill** from a spreadsheet, and **a mark on what changed** since the record was loaded.

Two things decide the design.

- **An entry grid is a form.** Its values are members of one record being edited, saved together, checked
  together and derived from each other. The forms engine already holds drafts, encodes them exactly, keeps what
  was typed across a reload and submits; a grid is a way to lay out an `ObjectArray` field, not a second editor.
- **It is still a table.** The renderer, the windows, the keys, the copy, the status line and the arrangement are
  [`table.md`](table.md)'s. A grid of 30 specimens and a table of 100,000 designs scroll, focus and copy alike.

## Two homes for an edit

| | A grid of a form | Cells of a model's table |
|---|---|---|
| The rows are | Members of one record: an `ObjectArray` field, or fields laid out as a fixed grid | Records of their own: the rows of a `Table.<id>` |
| An edit goes | Into the form's draft; the form submits | To an action, per cell or per row, at once |
| Derived values | The form's evaluation and derive call, or the document's expressions | The action's result and the table's change events |
| Mode | Client: every row is held | Server: a model's table always is |
| Size | Tens to a few thousand rows | Any size the table has |

A table node is in one home for its lifetime, decided by what it names, as its mode is.

## A grid of a form

### Binding

A form's `ObjectArray` field is drawn as a grid, inside the form or anywhere in its scope:

- inside a `form` node, the field's overlay `{"widget": "grid", "grid": {…}}`: `widget` chooses the grid as the
  field's control, as `x-widget: grid` on the field's schema does, and `grid` holds the table's properties below;
- anywhere the form is in scope, a `table` node with `form` (the form node's id) and `field` (the field's path)
  in place of `rows`.

The grid's rows are the field's `RowState`s, keyed by their stable keys. `ui::FormRowsSource` is the
`DataSource` over them, as `ValueRowsSource` is over a document's list: a cell is the member's encoded value
when its draft encodes, and an invalid cell when it does not, so sorting and filtering a grid follow the table's
rules. Sorting or filtering a grid changes what is shown and never the array's order or its members: the body
holds the rows in the order they were entered. A row added while the grid is sorted or filtered is shown at
the end of the view, and passes the filter, until the sort or the filter next changes, so the row being entered
does not jump or vanish under the cursor. For the same reason the row whose cell editor is open keeps the view
position it had, and stays in the view whatever the filter makes of its draft, until the edit is committed or
cancelled: its position, not its neighbours, since rows inserted or removed above it, or a new sort or filter, rearrange
the rows around it; then the sort and the filter place it, after the walk that the commit's key starts has moved on by view
position. A grid sorts and filters through a table's own header and menus: a column's
sort by its header, its filter button and values list, the cell menu's "Filter by this value" and "Exclude this
value", and the table menu's "Clear all filters", so a filter the user set shows on its column's header and is
cleared where any table's is. A display column neither sorts nor filters.

A grid of a form takes `group`, with its headings and its summaries ([Summary rows](#summary-rows)), as a table
over `rows` does: a grid of specimens grouped by sample is one declaration.

### Columns are fields

A grid's column names a member of the row type: `{"id": "mass", "field": "mass"}`. The member's `FieldSpec`
gives the column its kind, its places, its units, its options and its bounds, so a grid needs no column
declaration beyond layout. A column without `field` is a display column: its `cell` or `value` expression is
read in the row's scope and it takes no edit.

**Per row.** A column's field part may be an expression in the row's scope, naming a different member per row:
`{"field": {"if": [{"ref": "row.isChoice"}, "choice", "amount"]}}`. A row type that holds a value
of one kind for some rows and another kind for others has a member per kind, and the grid shows the one each row
uses. The kind of a value is the record's, never the grid's.

A column also takes **overlays per row**, as a form field takes overlays: `decimals`, `unit`, `readonly`,
`options` (narrowing a choice's options), `placeholder`, each an expression in the row's scope. A row of a
specification grid that holds a value to one place and the next row to three says so here.

On a grid column, `decimals` and `unit` are the field's overlays and mean what they mean on a `form` field: the
entry places, which are also the wire's `dp`, and the unit entered in. The column's display
([`table.md`](table.md#how-a-column-shows-its-cells)) follows them: a cell shows its value to the places its
field takes, so a grid never shows a value at places other than those it is entered with. The display-only
keys (`rounding`, `padPlaces`, `grouping`, `empty`, `labels`) keep their meaning on a grid.

### Derived values

A grid's derived cells come from three places, by who can compute them.

- **The document's expressions**, for what the screen can compute from the values it holds: a column whose
  `value` is `div(row.mass, row.volume)`, a summary row's `mean`. They are exact, follow every encoded draft at
  once, and are display only: they are never sent.
- **Computed fields**, for a pure function of the record: members the action declares in `computedFields`
  ([`../forms/forms.md`](../forms/forms.md#computed-fields)), on the action and, for a grid, on its row type.
  The server recomputes them on every path and overwrites them on submit; the client asks for them while the
  user is entering through the action's existing evaluation (`ActionDispatcher::evaluate`, the UI service's
  `ui-evaluate`), which decodes a body, recomputes the computed fields and returns the recomputed body, with no
  model instance, no journal entry and no state change ([`../core/registry.md`](../core/registry.md)).
- **Derived members**, for what needs the domain's data (a value looked up from the material, a limit from the
  standard in force): members marked `derived()` in their type's field metadata, which the schema writes
  `x-derived`. A computed field must be pure, so such a value cannot be one. A derived member is read-only in
  the form, is not overwritten by `recomputeAll`, and is the model's to set when it executes the action: the
  value the client holds is shown, never trusted. The form names its derive action with `derive`, an action of
  the form's model that the application declares and registers as any action:

  ```cpp
  /// The input of a derive action: the form's body as far as it encodes.
  template <typename Action>
  struct DeriveOf { Action body; };

  /// The values of derived members, by path into the body that was sent.
  /// Its JSON is {"values": [{"path": "specimens[3].density", "value": …}, …]}.
  struct Derived {
      struct Value { std::string path; glz::raw_json value; };
      std::vector<Value> values;
  };

  struct DeriveSieveAnalysis : forms::DeriveOf<lims::RecordSieveAnalysis> {};
  forms::Derived SieveModel::execute(DeriveSieveAnalysis const& derive);   // `Loggable::No`, no effects
  ```

  It runs on the form's model instance (the form's `model` and `key`), declares no effect and is
  `Loggable::No`, so a derive call is a read that leaves no trace.

**The call.** After a cell's edit is committed ([The cell](#the-cell)), not per key, once the form's
`deriveDebounce` (a literal number of milliseconds on the `form` node, default 150) has passed with no further
commit, the form evaluates its body, and calls its derive action when it names one.

- **The body as far as it encodes.** A member whose draft does not encode, or that is blank, is left out of the
  body. A left-out member decodes to its default, as any body decodes; a computation that must tell "not
  entered" from a value takes the member as `std::optional`, which is how the forms engine already represents a
  nullable field. The domain computes what it can from what is there, as a lab sheet does with half its
  fractions entered.
- **Loading the replies.** `FormSession::loadDerived` writes, from the evaluation's recomputed body, the computed
  members, and from a derive reply, the derived members it names, and nothing else. Neither is a member the user
  can edit, so a reply never replaces a draft the user typed, whatever was typed while it was in flight; and the
  load moves no field's snapshot, so what the user typed stays an edit. A path is resolved against the rows as
  they were when the call was sent (the session records the row keys of each `ObjectArray` at sending), so a
  value lands on the row it was computed for even if rows were inserted or removed meanwhile, and a value for a
  row since removed is dropped. The load is programmatic and causes no submission.
- **Order.** At most one evaluation and one derive call are in flight. A commit during them marks their replies
  stale; the replies are still loaded, and then one more round is made with the newer body. Each round has a
  sequence, and a reply older than one already loaded is ignored, so derived cells show at most one round trip
  behind the last commit and never go back to an older result.
- While a round is in flight, derived cells draw muted. A failed round leaves the derived cells as they were,
  marked stale, and the status line offers "Recalculate", which makes one more round. The failure is a standing
  state, "Derived values could not be recalculated" with the entry "Recalculate": it lasts, through the hold of any
  message and through a retry in flight, until a round succeeds, and the cells stay marked stale as long.

### Submission

The grid submits with its form, in the form's mode. In explicit mode the form's submit first commits an open
cell editor, so a value typed and not yet confirmed is sent with the rest. In automatic mode a committed cell
whose form becomes ready submits the body. A failed submission's field errors land on the cells they name
(`specimens[3].mass`). Computed members are recomputed by the server and derived members set by the model,
whatever the body holds.

## Cells of a model's table

A column of a model's table is editable when its description names the action that changes it:

```cpp
/// One cell's change: the row and the new value of one member. Its schema carries `x-cellEditOf` with the
/// table's record; which column it edits is the column's `EditInfo`.
template <auto Function, typename T>
struct CellEdit {
    RowId key;
    T value;
};

/// Several members of one row changed together. `Values` is the application's struct of the row's editable
/// members, each optional: a member present is changed, an absent one is left as it is. Its schema carries
/// `x-rowEditOf` with the table's record.
template <auto Function, typename Values>
struct RowEdit {
    RowId key;
    Values values;
};

struct EditInfo {
    std::string action;     // the action id, taking a CellEdit or a RowEdit of this table
    std::string member;     // for a RowEdit, the member of `values` this column edits; empty for a CellEdit
    std::string readonly;   // the id of a companion column whose `true` makes this row's cell read-only, or empty
};

struct TableColumn {
    // … table.md's members …
    std::optional<EditInfo> edit;
};
```

- The cell's field is the `value` member of a `CellEdit` action's input schema, or the named member of a
  `RowEdit`'s `values`, so its kind, places, bounds and options are the action's. A `Measure` cell, in its own
  unit, opens in its `Quantity` field's canonical unit, converted through the description's units; a cell the units
  cannot convert (a unit they lack, another dimension) is read-only, as its value could not be shown in the unit it
  would be written in. A `Measure` column whose field's canonical unit is not one of the description's units is
  `SourceContract`. A column whose kind and whose
  field's kind differ, or a `member` that `values` does not have, is `SourceContract`.
- **The action is the application's.** It declares `updatesTable` and returns the `ChangedRows` of the row, as
  any action that changes a table does ([`table.md`](table.md#a-models-tables)). A `FailedRow` for the row, or a
  `FieldError` naming `value`, settles the cell with that message. A row's commit may carry several cells: a
  `FieldError` naming a cell's member (`value` for a `CellEdit`, the `EditInfo::member` for a `RowEdit`) is that
  cell's; one that names no cell of the commit, a `FailedRow` included, is the error of every cell of the commit
  that no named error reached; and a cell no error reached keeps its draft and takes no error. A commit that one
  call cannot carry (a `CellEdit` cell beside `RowEdit` cells in `row` mode) is one call per action, and an error one
  of them gives that names no cell is that call's cells'.
- **Committing** is `CellEdits`: edits to one row run one at a time, in order. A cell is stale from its commit
  until the action replies. Then it shows the committed value (a column's own `cell` template shows the row's
  value, which it reads from the record), marked modified, until the window next fetches
  the row, which shows the row as the server has it; an edit that changes nothing, or that moves the row out of
  the view, leaves nothing waiting. A draft that does not encode, or one waiting to be committed, is held by row
  key and column, not by the window, so a row that scrolls out of the window and back finds its draft and its
  error where they were, and a key change by `from` moves them ([`table.md`](table.md#rows-an-action-changed)).
  `errorCount` counts them wherever the row is. A keyed refetch that names a row and returns none means the row
  was deleted or left the filter: its drafts, errors and marks go, and so do its edits not yet sent.
- **A row at once.** A column whose `EditInfo` names a `RowEdit` collects the row's edits while focus stays in the
  row and commits them together when focus leaves it, or on `Ctrl+Enter`. A refusal puts each `FieldError` on
  its cell and, while the table has focus, moves focus back to the first refused cell: focus has left the row
  by the time the reply arrives. A held cell whose row has become busy or locked, or whose value is no longer loaded,
  when the row is sent is refused at the cell: it keeps its draft with the refusal as its error, and its next commit,
  changed or not, sends it. This is for a record whose members are checked together, and
  `editMode: "row"` on the table chooses it for all its columns.
- **Who may edit.** The action's authorisation is the rule: a principal the authoriser refuses for the action
  sees the column read-only, which the client learns from the catalog's permitted actions. A row is read-only
  per cell through `EditInfo::readonly`, and entirely while it is locked
  ([`table.md`](table.md#how-a-column-shows-its-cells)). `TableColumn::edit` absent is read-only.
- A cell that is unloaded, or in a busy row ([`table.md`](table.md#rows-at-work-rows-that-failed)), takes no
  edit: an edit of a value the user has not seen is an overwrite of whatever arrives.
- **A table's own edits make no row busy.** They are not registered as runs in the table's row states: a cell
  being saved is stale, not busy, so a second cell of the row can be entered while the first is saving, and
  its edit is queued behind it. A row is busy only through another run that names it, such as a document's
  mutation or another table's action.
- **No row is held still.** An edit in a sorted or filtered column moves its row, or takes it out of the view,
  when the window next fetches it. The walk moves by view position at the moment of the commit
  ([Walking](#walking)), so entering a column of a sorted table continues down the rows that were below, and
  does not follow the edited row.

## The cell

### The editor

A cell's editor is the forms control for its field's kind, drawn inside the cell: a text input, a number input
with its places and unit, a check box, a dropdown for an `Enum` or a `Choice`, a multi-select for an `Array` of
options, a date or date-time input. A grid adds no control kind and changes none; a cell is a field with a
smaller frame.

- **Choices that depend on the row.** A `Choice` field's `x-optionsDependsOn` names members of the same row
  first, then the form's top-level fields. The options are fetched when the editor opens and cached by the
  values they depend on, so a column of 300 rows over five materials makes five requests.
- **A check box** toggles with `Space` or a click and commits at once; it opens no editor.
- **Nullable numbers** are blank: a blank optional cell is no value and is left out of its row object, as the
  forms engine encodes it. `Delete` on a focused cell makes it blank.

### Entering and leaving

| Gesture | Effect |
|---|---|
| Typing a character on a focused cell | Opens the editor with that character replacing the value |
| `F2`, `Enter`, double click on an editable cell | Opens the editor holding the value, caret at its end |
| `Enter`, double click on a read-only cell | Runs the table's `onActivate`, as on any table |
| `Enter` in the editor | Commits and moves by `enterMoves` ([Walking](#walking)) |
| `Tab`, `Shift+Tab` in the editor | Commits and walks forward or back |
| Arrow keys in the editor | Move the caret; `Alt+Down` opens a dropdown |
| `Escape` in the editor | Closes it and restores the draft the cell had when it opened |
| Focus leaving the table | Commits |

A cell that is read-only opens no editor; typing on it says so in the status line.

### Validation

- **What was typed stays.** A draft that does not encode keeps its text, draws the error mark and shows the
  forms error (`malformed`, `too-precise`, `below-minimum` and the rest) as its tooltip and in the status line
  while the cell has focus. No dialog is opened and focus is not held: the user may walk on and come back.
- In a grid of a form the form is not ready while a cell does not encode, as for any field. In a model's table a
  cell that does not encode is not committed.
- Rules beyond one value (`x-rules`, instance constraints, the server's checks) report through the form or
  through the action's `FieldError`, on the cells they name.

### Read-only and computed

A cell is read-only when its field is `x-readonly`, `x-computed` or `x-derived`, a `readonlyWhen` rule holds,
the column's `readonly` overlay is true for its row, its `EditInfo::readonly` companion is true, the column has
no field or no edit action, the row is busy or locked, or the cell is unloaded. A read-only cell:

- draws on the theme's read-only cell surface, so computed values read as computed;
- is skipped by the walk (`Tab`, `Enter`) and still reached by arrows and by a click, so it can be read,
  selected and copied;
- takes no paste and no fill; a pasted value that lands on it is dropped and counted
  ([Selecting cells](#selecting-cells-clipboard-and-fill)).

Read-only, its surface and its place in the walk are one consequence of one fact, so they cannot disagree.

### What changed

A cell whose value differs from what the record held when it was loaded draws the theme's modified mark, and its
row's header draws the row's modified mark while any of its cells does.

- In a grid of a form the comparison is per cell: the forms engine keeps, for each member of each row of an
  `ObjectArray`, the draft it held when the form was last loaded, by row key (`FormSession::modified(path)`), as
  it keeps a top-level field's. A row added since the load is modified as a whole. `refill` keeps comparing
  top-level fields as it does; the per-cell record is what the marks and "Revert cell" read.
- In a model's table a cell is modified from its commit until the window next fetches its row.
- "Revert cell" in the cell's menu restores the loaded value (`FormSession::revert(path)`); "Revert row" restores
  the row's. The element values `modifiedCount` and `errorCount` count such cells, and `modifiedRows` the rows.

## Walking

- **`enterMoves`**: `down` (the default: a column of measurements is entered top to bottom), `right`, or
  `none`.
- **`walk`**: the order `Tab` follows, `rows` (along a row, then the next row; the default) or `columns` (down a
  column, then the next column). The walk visits editable cells only, from the view position of the cell it
  leaves.
- **The ends.** `Tab` on the last editable cell, and `Shift+Tab` on the first, leave the table to the next or
  previous control of the screen's focus chain, so a grid sits in a form like any field. With `appendRows` a
  `Tab` on the last cell, and `Enter` moving down from the last row, add a row instead.
- Arrow keys move to any cell, editable or not, and `Ctrl` with an arrow to the last cell in that direction, as
  in [`table.md`](table.md#keyboard-focus-and-assistive-technology).

**What the walk needs from the view tree.** Leaving the table moves focus to another node, which the view tree
cannot do yet: it is why the forms engine lists a grid widget as not supported. The view tree gains a focus
chain in document order and `focusNext`/`focusPrevious` from a node ([Neighbouring specs](#neighbouring-specs));
step 1 of this design lands it.

## Selecting cells, clipboard and fill

A grid selects **cells**: `selectionMode: "cells"` is a rectangle of the view, extended with `Shift` and the
arrows or a drag, and the whole grid with `Ctrl+A`. A row header click selects its row, a column header's
selection gesture its column. It is the default for a grid of a form, and the design allows it on any table held
whole on the client; a model's table, which is in server mode, selects rows, and refuses `cells`. The code lowers
`cells` for a grid of a form only: on a table over `rows` it is refused when the document loads, and lowering it there
is deferred work.

- **Its value.** `selection` and `onSelectionChange`'s `event` are `{"anchor": {"key", "column"}, "focus":
  {"key", "column"}}`, the rectangle's two corners by row key and column id; the rectangle is the rows between
  the two keys' view positions and the columns between the two ids' places in the layout. `null` is no
  selection. A sort or a column move keeps the corners and so may change the rectangle, as a spreadsheet does.
- **Copy** writes the rectangle as [`table.md`](table.md#copy) writes rows: tab-separated display text.
- **Paste** (`Ctrl+V`) takes tab-separated text and writes it from the focused cell right and down, the block
  keeping its shape. Each value is read as the cell's draft, in the user's locale, and encoded by the cell's
  field, so a pasted "12,5" in a comma locale is 12.5 and a pasted "abc" in a number column is an invalid draft,
  kept. A value that lands on a read-only or covered cell is dropped, and the status line says how many were.
  Rows the block runs past are added when `appendRows` allows, up to the field's `maxItems`, and dropped
  otherwise, said in the status line.
- **Fill down** (`Ctrl+D`) copies the first row of the rectangle into its other rows, editable cells only.
- **Clear** (`Delete`) blanks the rectangle's editable cells.
- A paste, a fill or a clear in a grid of a form is one change of the form: the round of derived values it
  causes is one.

## Rows of a grid

A grid of a form adds and removes rows of its field.

- `appendRows` (a literal flag) adds a row by the walk past the end, and offers "Add row" under the grid.
- The cell's and the table's menus offer "Insert row above", "Insert row below" and "Delete row" (or "Delete
  rows" for a selection); `Ctrl+Shift+=` inserts below and `Ctrl+-` deletes, as in a spreadsheet. A deletion
  happens at once, with no question first: the status line says "Deleted {n} rows" with "Undo". The offer ends at
  the grid's next change (an edit, a paste, a fill, a clear, a revert or a row operation of the grid), or 6 s after
  the deletion, the hold of a status-line notice, whichever comes first; once it ends, "Undo" restores nothing. A
  derive round, which writes only computed and derived members, is no change of the grid's.
- The field's `minItems` and `maxItems` bound them: the entries are disabled at a bound, and say why.
- `rowsFixed` (a literal flag) offers none of them: the rows are the record's, such as one per sieve of a
  standard series.

Rows of a model's table are added and removed by the application's actions, through its menu entries
([`table.md`](table.md#the-applications-menu-entries)).

## Layouts

Each layout here is drawn by the mount and holds for any table it fits, editable or not.

### Header bands

```cpp
struct BandInfo {
    std::string label;
    std::string unit;                    // shown after the label: "Mass retained [g]"; empty for none
    std::vector<std::string> columns;    // the column ids under the band
    std::vector<BandInfo> bands;         // bands one level down, each over a subset of `columns`
};
```

`bands` (`TableDescription::bands`, or the document's parts of the same shape) are headings over columns,
nested up to three levels, drawn above the column headers. A band one level down covers a subset of its
parent's `columns`. A column belongs to at most one band per level.

- **Units.** A band's `unit` is shown after its label as a column's is, and a column under it whose unit is the
  same does not repeat it, so a band "Mass retained [g]" over six sieve columns shows the unit once.
- **Under rearranging.** When the layout keeps a band's columns side by side, the band is drawn once over them.
  When the user moves or hides columns so that they are not, the band is drawn over each run of its columns that
  stands together, so a band never covers a column that is not its own. A band whose columns straddle the
  pinned edge is drawn as two parts, one pinned, one scrolling.
- A band with no visible column is not drawn. A user does not form bands. A column group, user-formed or declared
  ([`table.md`](table.md#column-groups)), draws as a one-level heading on its own line below the bands, with its
  filter button, over its columns while they stand together; it is not a band, and is not drawn over each run of
  its columns.
- A band's accessible name is read before each of its columns' headers: "Mass retained, sieve 4 mm".
- Bands are windowed with their columns: a band exists for the columns in the column window.

### Spans

A cell may cover cells beside it and below it. A span part is `{column, rows, columns}`: the cell in `column`
covers `rows` rows, itself included, downwards, and `columns` columns, itself included, to the right in the
layout; either count defaults to 1.

- A table over rows gives its spans through `rowSpans`
  ([`table.md`](table.md#the-document-table-node-on-the-engine)), whose parts take `columns` beside `rows`. A
  fixed grid gives them on its cells ([below](#fixed-grids)), as `rows` and `columns` of the cell part.
- A span across a column the user has moved away or hidden is cut at that column.
- Spans are drawn only while the table is unsorted and under its windowing thresholds, as row spans are; a
  fixed grid is always both.
- A covered cell is not focused, selected alone, walked or pasted into; the walk passes over it.

### Summary rows

`summaries` are parts `{key, label, position, cells}`: rows drawn after the table's rows (`position: "bottom"`,
the default) or before them (`"top"`), outside the order, the filter and the selection.

- `cells` maps column ids to expressions read in a scope with `rows`, the records of the view's rows in view
  order, and `column`: `{"fn": "mean", "args": [{"map": {"ref": "rows"}, "as": "r", "to": {"ref": "r.mass"}}]}`.
  The expression language's `count`, `sum`, `mean`, `min`, `max`, `stdev(list, dp, mode?)` and
  `rsd(list, dp, mode?)` are the aggregates; its exact arithmetic and its rounding modes are theirs.
- `rows` are the view's rows: a summary follows the filter, so it summarises what the user sees.
- A summary cell shows by its column's display and draws on the theme's summary surface. It is read-only, is
  copied when a copy covers it, and is never pasted into.
- **Per group.** A `group` part takes `summaries` too: drawn after each run of the group, in a scope whose
  `rows` are that run's. A grid of specimens grouped by sample shows each sample's mean, minimum, maximum and
  deviation under its rows, and the table's own summaries under all of them.
- A summary row is a table over held rows' feature: a model-backed table refuses `summaries` and group
  summaries, as it refuses `group`. A total over a whole model's table is the application's report.

### Fixed grids

Some grids have rows fixed by a standard or a specification, each row a different quantity: rows of tests and
columns of minimum, maximum and declared value; a key-value sheet of a sample's parameters. A **fixed grid** is a
table node with `layoutRows` in place of `rows`, and `form`, the id of the form node whose fields its cells
edit (absent for a fixed grid that shows values only):

- `layoutRows` are parts `{key, label, cells}`, in order; keys are unique in the grid. `cells` maps column ids to
  a cell part: `{field}` (a path of that form's fields, `limits.density.min`), `{value}` (an expression), or
  `{text}`, with the column overlays, and `rows` and `columns` for a span.
- Each cell is its own field, so each takes its own kind, places and editor: one row's minimum is a decimal to
  three places and the next row's a choice.
- A fixed grid is not sorted, filtered or rearranged by rows; its columns may still be hidden and resized. It
  walks, selects, pastes and validates as any grid.
- **Blocks.** A part `{"forEach": "specimens", "rows": […]}` repeats its rows for each row of that
  `ObjectArray`, in the block's own scope, where `row` is the specimen and a cell's `{field}` path is relative to
  it. A block row's key is the specimen's row key and the part's key joined (`"<row key>/<key>"`), so it is
  stable while specimens are added and removed. A block of several fixed rows per specimen is one declaration. A
  row's `label` is read once for every row of the block, so it does not read `row`; only its cells do.

## Document vocabulary

| Key | On | Meaning |
|---|---|---|
| `form`, `field` | `table` | A grid of a form's `ObjectArray` field, in place of `rows` |
| `{"widget": "grid", "grid": {…}}` | a `form` field overlay | The field drawn as a grid, with the table's properties |
| `layoutRows`, with `form` | `table` | A fixed grid, in place of `rows`; `form` names the form its cells edit |
| a column's `field` | grid column | The member the column edits; an expression for one member per row |
| a column's `value` | grid column | A display column's expression in the row's scope |
| a column's `decimals`, `unit`, `readonly`, `options`, `placeholder` | grid column | Overlays per row, expressions in the row's scope |
| `derive`, `deriveDebounce` | `form` | The form's derive action; milliseconds of no commit before a round, default 150 |
| `editMode` | `table` | `cell` (the default) or `row`, for a model's table |
| `enterMoves`, `walk` | `table` | The walk ([Walking](#walking)) |
| `selectionMode: "cells"` | `table` | A rectangle of cells |
| `appendRows`, `rowsFixed` | `table` | Which row operations a grid offers |
| `bands` | `table` | Header bands |
| a span part's `columns` | `rowSpans` part, fixed-grid cell | A span across columns |
| `summaries` | `table`, `group` | Summary rows |
| Element values `modifiedCount`, `modifiedRows`, `errorCount`, `editing` (the cell being edited, `{key, column}` or `null`) | `table` | |

Every key is optional, and a table that names none of them is what [`table.md`](table.md) describes.

## Invariants

1. **One field model.** A cell's draft, encoding, error and bounds are its field's, from the forms engine; a
   grid holds no value the form does not.
2. **Typed text survives.** No commit, evaluation, derive reply, refill or change event replaces a draft the
   user changed since it was loaded, until that draft is submitted, committed or reverted.
3. **Derived values never run backwards.** A reply older than one already loaded is not loaded, and a loaded
   value lands on the row it was computed for or nowhere.
4. **Read-only is one rule.** A cell is skipped by the walk, refuses an edit, a paste and a fill, and draws as
   read-only exactly when [Read-only and computed](#read-only-and-computed) says it is read-only.
5. **The record's order is the record's.** Sorting or filtering a grid of a form never changes the order or
   the content of the field's array.
6. **No edit of an unseen value.** No edit is committed for an unloaded cell, a busy or locked row.
7. **A paste is its cells.** A paste writes exactly the drafts a user would have typed into the editable cells
   under the block, and says how many values it dropped.
8. **A span covers what it says.** A covered cell is never focused, walked, selected alone or pasted into.
9. **The server decides derived values.** A computed or derived member the client sends is never the value the
   server keeps.

## API surface

| Header | Holds |
|---|---|
| `table/edits.hpp` | `CellEdit<fn, T>` and `x-cellEditOf`, `RowEdit<fn, Values>` and `x-rowEditOf`; `checkEdits`; `CellEdits` committing a row's cells as one request in `row` mode, and holding drafts by row key and column, moved by a key change. |
| `table/description.hpp` | `EditInfo` and `TableColumn::edit` (`edits.hpp` includes this header, so `EditInfo` is declared here); `TableDescription::bands` and `BandInfo` ([`table.md`](table.md#api-surface)). |
| `forms/derive.hpp` (new) | `DeriveOf<Action>`, `Derived`; the `derived()` field metadata and `x-derived`. |
| `forms/engine` | `FormSession::loadDerived`, the round's debounce, sequence and row-key mapping, over `evaluate` and the derive action; per-cell snapshots of `ObjectArray` rows, `modified(path)`, `revert(path)`; `computedFields` on a row type, recomputed by `recomputeAll`; options of a `Choice` depending on its row. |
| `ui/form_rows.hpp` (new) | `FormRowsSource`: a `DataSource` over an `ObjectArray` field's rows. |
| `ui/grid.hpp` (new) | The cell editor host, the walk, cell-range selection and its value, paste, fill and clear, row operations; summary rows and fixed grids' lowering, with blocks. |
| `ui/view.hpp`, `ui/mount.hpp`, `ui/backend.hpp` | The focus chain and `focusNext` / `focusPrevious`. |
| `ui/interpreter.hpp` | The lowering of a grid, a fixed grid and an editable model column; the keys of [Document vocabulary](#document-vocabulary) and its element values. |
| `ui/view.hpp`, `ui/mount.hpp` | Bands, spans and summary rows in the mount; the read-only, modified, stale and locked cell and row marks. |
| `qt_quick` and the terminal backend | Editors in cells; the marks; bands and spans. |

## Tests

| Condition | Test | Layer |
|---|---|---|
| A grid is a form | A grid over an `ObjectArray` of three rows: typing "12,5" in a comma locale gives the body `12.5` exactly; a draft with too many places is kept, marked and keeps the form unready. Mutation: a grid that encodes with its own parser. | `tests/test_ui_grid.cpp` |
| Read-only is one rule (grid) | A computed member, a derived member and a `readonlyWhen` cell are each skipped by `Tab`, refuse typing, drop a pasted value and draw the read-only surface. Mutation: the walk testing `x-readonly` only. | `tests/test_ui_grid.cpp` |
| Read-only is one rule (model table) | An unloaded cell, a busy row, a locked row and an `EditInfo::readonly` cell refuse an edit and are skipped by the walk. Mutation: the busy test left out of the rule. | `tests/test_ui_table_model_*.cpp` |
| The walk | `enterMoves: down` fills a column; `Tab` on the last editable cell moves focus to the next control; with `appendRows` it adds a row; `walk: columns` goes down first; after an edit that moves its row in a sorted model table, `Enter` lands on the row that was below. Mutation: `Tab` wrapping to the first cell, which never leaves the grid. | `tests/test_ui_grid.cpp`; `tests/qt_quick` |
| Derived values | A commit evaluates once after `deriveDebounce` and calls the derive action with the body as far as it encodes; a commit during the round yields one more; computed and derived members load, onto the rows they were computed for after a row was inserted above them, and a member typed meanwhile stays as typed and modified; an older reply is ignored; a failed round marks the cells stale and "Recalculate" runs one more. Mutation: `loadDerived` resolving paths against the current rows, which writes a value onto the wrong row. | `tests/test_ui_grid.cpp` |
| The server decides | A submitted body with a forged computed and derived member: the server keeps `recomputeAll`'s value and the model's. Mutation: `recomputeAll` skipping row members. | `tests/forms/test_forms_derive.cpp` |
| Kind per row | A column whose `field` expression names `choice` for one row and `amount` for the next opens a dropdown and a number input, and encodes each by its own field. Mutation: the column's field resolved once for every row. | `tests/test_ui_grid.cpp`; `tests/qt_quick` |
| Choices per row | Options depending on a row member are fetched once per distinct value. Mutation: a fetch per row. | `tests/test_ui_grid.cpp` |
| Paste, fill, clear | A 3 × 4 block over a read-only column keeps its shape, drops and counts those values, adds rows to `maxItems` and reports the rest; fill down and clear touch editable cells only; one paste is one derive round. Mutation: a paste that shifts values past a read-only cell. | `tests/test_ui_grid.cpp` |
| Cell selection | `selection` is the two corners by key and column; `Shift`+arrows extend it; `cells` on a model's table is refused. Mutation: corners stored as view positions, which a sort moves to other rows. | `tests/test_ui_grid.cpp` |
| Rows of a grid | Insert and delete at `minItems` and `maxItems` are disabled; a deletion removes the rows at once and offers "Undo"; `rowsFixed` offers none. Mutation: bounds unchecked. | `tests/test_ui_grid.cpp` |
| A sorted grid keeps its record | Sorting a grid by a column and editing a row submits the array in its entered order with the edit in place; an added row shows at the end until the sort changes. Mutation: a source that reorders the field's rows. | `tests/test_ui_grid.cpp` |
| Submission | An explicit submit with a cell editor open sends the typed value; a field error `specimens[3].mass` lands on that cell. Mutation: submit before the editor commits. | `tests/test_ui_grid.cpp` |
| What changed | Typing in one cell marks that cell and its row, not its neighbours; "Revert cell" restores the loaded value; a derive reply moves no snapshot. Mutation: a mark per `ObjectArray`, which marks every cell. | `tests/test_ui_grid.cpp` |
| Cells of a model's table | An edit commits through the column's action, is stale until the reply and modified until the row's next fetch, and shows a `FieldError` on refusal; `row` mode commits once when focus leaves the row and a refusal brings focus back to the first refused cell; a second cell of a row whose first edit is saving takes an edit; a draft survives its row scrolling out and back, and follows a `from`; a principal refused the action sees the column read-only. Mutation: drafts held by the window, which loses them on scroll. | `tests/test_ui_table_model_*.cpp`; `tests/net/test_push_tables.cpp` |
| Bands | A band over three columns draws once; moving one away draws it over two runs; across the pinned edge it draws as two parts; hiding all three draws none; the band's unit is not repeated by its columns. Mutation: a band drawn over its first and last column's span, which covers a foreign column. | `tests/test_ui_mount.cpp`; `tests/qt_quick` |
| Spans | A cell spanning two rows and three columns covers them; the walk passes over covered cells; a paste onto a covered cell drops the value; moving a covered column cuts the span. Mutation: covered cells left focusable. | `tests/test_ui_mount.cpp`; `tests/test_ui_grid.cpp` |
| Summary rows | A grid's `mean` and `stdev` follow every committed value exactly and follow the filter; group summaries are per run; a model-backed table refuses `summaries`. Mutation: summaries over the source rows, which ignore the filter. | `tests/test_ui_grid.cpp` |
| Fixed grids | A `layoutRows` grid of four parameters with different kinds per cell walks, pastes and validates per cell; a `forEach` block repeats per specimen with keys that survive inserting a specimen above. Mutation: block keys by position, which move every row's state on insertion. | `tests/test_ui_grid.cpp` |

## What this does not do

- **It does not lay out printed reports.** A table drawn on a report page, filled by the report and split across
  pages, is a report's design, not an interactive table's.
- **It has no formula cells.** No cell holds an expression a user typed, and there are no references by cell
  address. Computation is the server's (computed fields and derived members) or the document's (an expression
  column or a summary row).
- **It does not undo.** "Revert cell" and "Revert row" restore the loaded value, a deleted row offers "Undo"
  until the next change, and the form's `reset` restores the record; a history of edits is not kept.
- It does not paste into, fill or clear a model's table, which selects rows, not cells: a bulk change of a
  model's rows is an action over a selection ([`table.md`](table.md#acting-on-rows)).
- It does not derive values for a model's table on the client: its derived values arrive with the row.
- It does not hold an edited row still in a model's table.
- It does not let a user form header bands.

## Alternatives considered

- **A grid with its own cell store**, filled and read back by the screen. Every screen then writes the copy in
  and out, the encoding is the screen's, and the dirty state is a second bookkeeping beside the form's. The
  form already holds the record.
- **One derive mechanism for both pure and domain values.** A computed field must be pure and is overwritten on
  submit; a value from the domain's data needs a model and is the model's to set. Folding both into computed
  fields breaks the purity the server relies on; folding both into a derive action makes every form with a sum
  declare an action. `evaluate` already serves the pure half.
- **Recomputing on the client by rules in the schema.** A lab computation is the domain's code, with its
  standards' rounding, and is needed on the server to save anyway. The server runs it; the document's
  expressions cover what is display only.
- **Deriving on every key.** A request per character, and values that change under the cursor while a number is
  half typed. A commit is the moment a value is meant.
- **Loading derived values with `refill`.** `refill` keeps or replaces whole top-level fields, and a grid is one
  such field: once any cell is typed, every derived member of every row would be kept as it was. Writing only
  members the user cannot edit needs no comparison at all.
- **A cell kind chosen by the grid**, overriding the record's type. The same value would then encode one way in
  the grid and another in the body. A member per kind keeps the record honest; the grid chooses which to show.
- **Holding focus on an invalid cell**, or a dialog on a refusal. The user loses the place they were going to,
  and a dialog per cell turns a column of entries into a column of confirmations.
- **Skipping read-only cells by a per-cell flag.** Separate flags for read-only, its colour and its place in the
  walk disagree the first time one is forgotten.
- **Pasting around read-only cells**, shifting values into the next editable cell. The pasted block then lands
  in cells it did not come from.
- **Pasting into a model's table, one commit per cell.** Thousands of serial round trips, edits of rows the
  client does not hold, and a refetch per change event; a bulk change is an action over a selection.
- **Holding an edited row still in a server-mode table.** The window would show a row at a place the server no
  longer reports, which [`table.md`](table.md#invariants)'s window rules forbid; moving the walk by position
  gives the entry behaviour without it.
- **Cell-selection corners by view position.** A sort would move the selection onto other rows.
- **Summary rows as rows of the data.** They would be sorted, filtered, selected and submitted.
- **Bands that move columns together.** A user reordering columns would drag several at once without having
  chosen to; a band follows the layout instead.

## Failure modes

| Situation | Behaviour |
|---|---|
| A draft that does not encode | Kept, marked, its error in the tooltip and the status line; the form is not ready, or the cell is not committed. |
| An evaluation or a derive call fails | Derived cells keep their values, marked stale; the status line offers "Recalculate". |
| A reply arrives after a newer one | Ignored. |
| A derive reply names a path that is not a derived member | That value is dropped and reported once. |
| A column's kind and its edit field's kind differ, or a `member` that `values` lacks | `SourceContract` when the description is built. |
| A column `field` naming no member of the row type | Refused when the document loads; an expression naming none is reported once and the cell is read-only. |
| An edit of an unloaded cell, a busy or a locked row | Refused at the cell; nothing is sent. |
| A `FieldError` or `FailedRow` for an edited cell | The cell keeps the committed draft and shows the message. |
| A paste onto read-only or covered cells, or past the last row without `appendRows` | Those values are dropped; the status line says how many. |
| Insert or delete past `maxItems` or `minItems` | The entry is disabled and says why. |
| `summaries` or a group's summaries on a model-backed table | Refused when the document loads. |
| `selectionMode: "cells"` on a model's table | Refused when the document loads. |
| `editMode` on a table that is not a model's table | Refused when the document loads. |
| A span over a column the user moved or hid | Cut at that column. |
| A band nested past three levels, a band naming no column, a child band over a column its parent lacks, or a column in two bands of one level | Refused where the bands are read. |
| Two `layoutRows` parts with one key | Refused when the document loads. |
| `form` and `rows` both on a table, `layoutRows` with `rows` or `field`, or a `{field}` cell in a fixed grid without `form` | Refused when the document loads. |
| A `layoutRows` key holding a `/`, which joins a block row's keys | Refused when the document loads. |
| `sort`, `group`, `summaries`, `rowSpans`, `appendRows`, `rowsFixed` or `key` on a fixed grid, or `layoutRows` on a model's table | Refused when the document loads: a fixed grid's rows are its parts, in order. |
| A fixed-grid cell under an id the column parts lack, naming none or more than one of `field`, `value` and `text`, or giving `decimals`, `unit`, `options` or `placeholder` with no `field` (`readonly` is taken by any cell); a `rows` or `columns` that is not a literal count from 1; a fixed grid's column part naming a `field` or a `value` | Refused when the document loads. |
| A block without `form` or a block inside a block | Refused when the document loads. |
| A `forEach` that is no list of records of the form, or a `{field}` that is no field of the form (of the row type, in a block) | Refused when the document loads while the catalog has the form action's schema; otherwise reported when the grid is lowered, and the grid drawn as muted text. |
| Two overlays in one document giving one form field different literal units (a fixed-grid cell's `unit`, a grid column's `unit` over the same member) | Refused when the document loads: a field is entered in one unit. Units that expressions give cannot be compared when the document loads, and are not: after each load every grid that gives the field a unit gives it again, in the order the grids' overlay effects run, and the field is entered in the unit the last of them gave, which settles once each has given it; no order among them is promised, and a document should not rely on one. |
| A block row's `label` reading `row` | Refused when the document loads: a row's label is read once, for every collection row. |

## Neighbouring specs

This document is the design. The specs below describe the code, so each takes its part when the step that
builds it lands, and until then says nothing of it.

| Spec | Takes | With step |
|---|---|---|
| [`table.md`](table.md) | Points here for editing; `TableColumn::edit` and `TableDescription::bands` among its members | 1 |
| [`../ui/view_tree.md`](../ui/view_tree.md), [`../ui/backend_contract.md`](../ui/backend_contract.md) | The focus chain and `focusNext` / `focusPrevious` | 1 |
| [`../forms/engine.md`](../forms/engine.md) | The grid widget leaves "Not supported"; per-cell snapshots, `modified`, `revert` | 1 |
| [`../forms/engine.md`](../forms/engine.md) | Rounds of derived values and `FormSession::loadDerived`; `deriveDebounce`; a `Choice` whose options depend on its row | 2 |
| [`../forms/forms.md`](../forms/forms.md) | `computedFields` on a row type, recomputed on every path, as an exception to "computed fields are top-level only"; the `derived()` metadata and `x-derived`, which `recomputeAll` does not touch | 2 |
| [`../core/registry.md`](../core/registry.md) | `DeriveOf<A>` and `Derived`; `evaluate` recomputing row members | 2 |
| [`../ui/document.md`](../ui/document.md), [`../ui/interpreter.md`](../ui/interpreter.md) | The keys of [Document vocabulary](#document-vocabulary) on `table`, `form`, `group` and a field overlay, and their lowering | 1 to 6 |
| [`../qt_quick/backend.md`](../qt_quick/backend.md) | Editors inside cells, the cell and row marks, bands and spans | 1, 4 |

## Delivery order

| Step | Lands | Gates |
|---|---|---|
| 1 | The focus chain in the view tree. `FormRowsSource`; a grid of an `ObjectArray` field with the forms editors in cells, validation at the cell, read-only as one rule, the walk, per-cell modified marks, submission. Needs [`table.md`](table.md)'s steps 4, 6 and 9a. | "A grid is a form", "Read-only is one rule (grid)", "The walk" (its grid cases), "A sorted grid keeps its record", "Submission", "What changed" |
| 2 | Derived values: rounds over `evaluate` and the derive action, `loadDerived`, `computedFields` on row types, `derived()`; choices depending on their row; kind and overlays per row. | "Derived values", "The server decides", "Kind per row", "Choices per row" |
| 3 | Cell-range selection, paste, fill, clear; row operations. | "Cell selection", "Paste, fill, clear", "Rows of a grid" |
| 4 | Bands, spans, summary rows and group summaries. | "Bands", "Spans", "Summary rows" |
| 5 | Fixed grids, with blocks per row of an `ObjectArray`. | "Fixed grids" |
| 6 | Cells of a model's table: `CellEdit`, `RowEdit`, `row` mode, stale, modified and refused cells, drafts held by key. Needs [`table.md`](table.md)'s steps 4 and 9b (busy and locked rows, `FailedRow`). | "Cells of a model's table", "Read-only is one rule (model table)", "The walk" (its model-table case) |
