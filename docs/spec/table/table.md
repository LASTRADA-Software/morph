# `morph::table` — the table

Design spec for the one table every surface of morph uses: what a table is, how a model offers one or many, the
single pipeline from a model's rows to the screen, slow columns and the caches that hold them, where sort and
filter run, what a user can configure, and the performance all of it is held to. Read [`engine.md`](engine.md)
first, for the engine this builds on.

## Contents

- [Goal](#goal)
- [A table is a description](#a-table-is-a-description)
- [The pipeline](#the-pipeline)
- [Sources](#sources)
- [The client table model](#the-client-table-model)
- [The renderer](#the-renderer)
- [Every surface on the pipeline](#every-surface-on-the-pipeline)
- [The unloaded cell](#the-unloaded-cell)
- [Semantics of an unloaded cell](#semantics-of-an-unloaded-cell)
- [Data levels](#data-levels)
- [Partial pages](#partial-pages)
- [Keyed queries](#keyed-queries)
- [Arrival](#arrival)
- [The window under changes](#the-window-under-changes)
- [Caching layers](#caching-layers)
- [Where sort and filter run](#where-sort-and-filter-run)
- [Configuring a table](#configuring-a-table)
- [Acting on rows](#acting-on-rows)
- [Values with units](#values-with-units)
- [Performance](#performance)
- [Invariants](#invariants)
- [API surface](#api-surface)
- [Tests](#tests)
- [What this does not do](#what-this-does-not-do)
- [Alternatives considered](#alternatives-considered)
- [Failure modes](#failure-modes)
- [Open questions](#open-questions)
- [Neighbouring specs](#neighbouring-specs)
- [Delivery order](#delivery-order)

## Goal

The goal is one table implementation, complete and fast, that every surface reuses. Three things define it.

- **A table is generic.** It is a description a model builds at run time and rows that fit it, not a C++ row
  type. Any model can offer a table, or several.
- **One pipeline.** Every surface that shows rows in columns goes through `morph::table`: the same order, the
  same filters, the same window, the same renderer.
- **Held to a size.** 100,000 rows and 300 columns, natively and in a browser, with budgets and a benchmark for
  every layer.

### Why one pipeline

Rows in columns come from four kinds of source: a list of values a document holds, a collection's rows, a
model's table paged by the server, and a typed list action's pages. Each is a source of the one pipeline, and
none has ordering code of its own.

| What one pipeline gives | Without it |
|---|---|
| One text order on a screen | Two tables side by side order the same text differently, one by a collator's key and one by a fold inside each comparison. |
| A repair when a row changes | A changed row sorts the whole list again, and folds text once per comparison instead of once per row. |
| A window at any size | A screen over a server's pages holds one page, and a header sort orders that page instead of the table. |
| Work off the painting thread where there is one, in bounded steps where there is not | A sort of 100,000 rows blocks the frame it runs in. |
| A table from a description | Every table needs a row struct and an action written for it. |

`forEach` and `grid` lay rows out without columns, sort nothing and are not tables.

### Slow columns

A row can be cheap to read while some of its columns cost a second query per row. Without a way to say "not loaded
yet", a server either holds every page until all of its columns are read, or sends the unloaded cells as empty
cells, which the engine then sorts, filters and counts as real empty values while the client polls for the rest.

Three things are needed together, because each is useless without the next: a cell state that is neither a value
nor empty; a name on each column for the load it depends on, so a query's columns say which loads it needs; and a
reply that says which of its cells are still coming, with a push when they arrive.

**The name.** The state is `Unloaded`, and this document says "unloaded cell". `pending` already means "a request
is in flight" on `Engine::pending()`, `PageWindow::pending()`, a cache entry and a `table` element's `pending`
value, which documents read; those keep their name and their meaning. The two are different facts: an unloaded
cell may have no request in flight at all (its level is not wanted, or its load failed), and a table that is
`pending` may hold no unloaded cell.

## A table is a description

A table is not a C++ type. It is a **description**, built by a model when it is asked, and rows that fit it. A
model offers no table, one, or many, and what a table's columns are may depend on who asks.

```cpp
struct TableColumn {
    ColumnInfo info;                       // id, kind, comparator, format, level, dimensions
    std::string label;
    std::string group;                     // a declared column group's id, or empty
    bool sortable = true;
    bool filterable = true;
    bool shown = true;                     // the default visibility
    ColumnDisplay display;                 // how a cell's value becomes its text; see "How a column shows its cells"
    std::string tone;                      // the id of a companion column holding this cell's tone, or empty
    std::string tooltip;                   // the id of a companion column holding this cell's tooltip, or empty
    std::optional<EditInfo> edit;          // the action that edits its cells on a model's table; see editing.md
};

struct ColumnGroupInfo {
    std::string id;
    std::string label;
    std::vector<std::string> columns;      // 2 to 16 column ids of one kind
};

struct TableCapabilities {
    bool keys = true;                      // keyed queries
    bool distinct = true;                  // distinct values
    std::uint32_t maxSortKeys = 8;         // set by the framework from `ServedTable::limits`
};

struct TableDescription {
    std::string id;                        // unique in the process, across every model: "designs"
    std::string label;
    std::vector<TableColumn> columns;      // in the declared order
    std::string key;                       // the id of the column that is the row's key
    std::vector<ColumnGroupInfo> groups;
    std::vector<BandInfo> bands;           // header bands over columns: {label, columns, bands} (editing.md)
    std::vector<std::string> levels;       // every level a column names, base excluded
    std::vector<UnitInfo> units;           // the units its `Measure` columns may hold
    std::string rowTone;                   // the id of a companion column holding each row's tone, or empty
    std::string rowLock;                   // the id of a companion column holding why a row is locked, or empty
    TableCapabilities capabilities;
    std::string revision;                  // set by the framework: a hash of everything above
};
```

Everything this design says about a table is said of its description: its levels, its groups, its units, which
columns sort and filter. A client draws a table from the description alone, with no compile-time knowledge of
it. The model leaves `revision` empty; the framework fills it from the description's canonical JSON, so two
descriptions are equal exactly when their revisions are.

### A model's tables

A model serves a table by returning what the framework needs to run it:

```cpp
struct TableContext {
    session::Context const& session;       // who asks
};

struct ServedTable {
    TableDescription description;
    /// The rows the caller may see, held in memory or read lazily: `apply` runs over it.
    std::shared_ptr<DataSource const> source;
    /// The slow levels, one per level the description names.
    std::vector<std::shared_ptr<LevelSource>> levels;
    /// Whether the caller may read a column; empty allows every column of the description.
    std::function<bool(std::string_view column)> mayRead;
    /// Names everything about the caller that changes the view beyond the query: the row scope and the
    /// collation. Empty when every caller sees the same rows in the same order.
    std::string scope;
    /// The collator, date parser, custom comparators and unit registry this caller's order uses.
    Services services;
    QueryLimits limits;
};

class DesignModel {
public:
    table::ServedTable designs(table::TableContext const& context);
    table::ServedTable recipes(table::TableContext const& context);
};

BRIDGE_REGISTER_TABLE(lims::DesignModel, "designs", &lims::DesignModel::designs)
BRIDGE_REGISTER_TABLE(lims::DesignModel, "recipes", &lims::DesignModel::recipes, table::rowScoped)
```

- The model's function runs per request, on the model's strand, with the caller's session, so a description may
  differ by principal: columns a user's country adds, columns a role may not see. It returns shared, immutable
  things and is expected to be cheap; the description of a table whose columns do not vary is built once.
- **The framework does the rest**, and none of it on the model's strand. When the function returns, the
  framework takes the source's snapshot, and then on the server's table executor
  (`ServerConfig::tableWork`, a pool) validates the query against the description and `limits`, finds or
  computes the view ([The view cache](#the-view-cache)), reads the level snapshots, builds the reply, and wants
  what the reply showed as unloaded. A model writes no paging, sorting, filtering, keyed or distinct-value code.
- **Services are the caller's.** `services` is what makes text order by the caller's locale. Left empty it is the
  default `Services`, whose collator orders by code point; a table with text columns that users sort sets a
  collator. Two callers with different collators have different `scope`s.
- **The row scope is the model's, and it is applied before the framework sees a row.** `source` holds only
  rows the caller may see. Nothing downstream filters rows by principal: a level
  cache, the view cache and a reply all work on the rows they are given, which is why a cached value cannot
  cross a scope. `scope` names the scope so that callers who see the same rows share a view.
- **What a row scope costs.** A model whose callers all see the same rows returns one shared source. A model
  with row scopes keeps one source per scope, built when the scope is first asked for and shared from then on;
  `table::scoped(source, keys)` makes one from a shared source and the scope's row keys, without copying a cell.
  Building a source per request at 100,000 rows is a full pass per request and defeats the view cache, whose key
  holds the source's snapshot.
- **Rows in a database are a source too.** A model whose rows live in a database serves them as a `DataSource`
  it reads, whole or lazily by column, and the server sorts, filters, pages and counts them with `apply`. The
  database is never asked to order or filter a view: one order on every surface needs one comparator, the
  table's, and slow levels, unloaded cells, column groups and values with units are the engine's.
- **A table is always C++ code.** Every table is a registered function of a C++ model; nothing on the server
  defines a table from a document or from data alone. What is generic is the table's shape, which that function
  decides when it runs. A model's set of table ids is fixed when it is registered; a table's columns are not.

**Actions per table, made by the registration.** `BRIDGE_REGISTER_TABLE(Model, "designs", …)` registers:

| Action id | Takes | Returns |
|---|---|---|
| `Table.designs` | `TableQuery` | `GenericPage`: a page, a keyed reply or distinct values, as the query asks |
| `Table.designs.describe` | nothing | `TableDescription` |
| `Table.designs.resolve` | `ResolveRequest` (a `RowSelection` and a `SelectionPolicy`) | `SelectedRows` ([Acting on rows](#acting-on-rows)) |

**A table id is unique in the process**, not only in its model. The action ids carry the table's id and no
model, and an action id names one action type: the effects registries are keyed by it, and an id registered by
two action types fails every effects lookup of the process. Two models that each register a table `"rows"`
are therefore refused when the registries resolve, with "action `Table.rows` is registered by two action types;
give each its own id".

One generic action taking a table id in its body was the alternative, and loses on every count that matters:

- **Authorisation** is per model type and action id. An action per table lets a principal read one table of a
  model and not another with the authorizer as it is.
- **The catalog** lists actions; a table per action appears in it, with its effects and the record it holds,
  without a second directory of tables.
- **The client's cache key** is the action and its body; an action per table keeps two tables' entries apart
  without the cache reading bodies.
- **Effects** are declared per action.

A typed list action that takes a `TableQuery` and returns a `Page<Row>` remains a table: it is the same contract,
read against a description the client builds from the node's columns or the action's view document
([The client table model](#the-client-table-model)), since a client sees an action's request schema and never
its reply's.

**Changing a row is the application's action.** The registration makes no edit action. A model changes a row
through an action of its own, which declares its effect on the table and returns the keys it changed. A column
whose description names such an action (`TableColumn::edit`) is edited in place with an editor for its kind,
as [`editing.md`](editing.md#cells-of-a-models-table) specifies; the table's own actions change nothing.

**The table's record.** Each registered table is a keyed record named `<model type>.<table id>`
(`DesignModel.designs`), registered by `BRIDGE_REGISTER_TABLE` beside the table's actions. A record is
registered under a C++ type, and a table has one: `table::TableOf<&DesignModel::designs>`, a tag type made from
the table's function, which is distinct for each table and is no row struct. The registration specialises it
with the model and the id, so everything that names a table in C++ names its function, and a function no
registration names does not compile as a table. The record is what a client watches, what an action's effects
name, and what a level cache announces.

Registration options after the function are the record's:

| Option | Effect |
|---|---|
| `table::rowScoped` | The record is registered with `privateKeys`. Required when callers see different rows: a change event then reaches each watcher without keys, except the writer's own, so no watcher learns the key of a row outside its scope. |
| `table::derivedFrom<Records…>` | A change of any of the records is a change of the table's record, as for any derived record. |

A row-scoped table pays for its privacy: an arrival that a level cache announces has no caller, so it reaches
every watcher without keys, and each refetches its visible pages instead of its rows
([The window under changes](#the-window-under-changes)). A table that is not row-scoped sends keys to every
watcher the authoriser admits.

```cpp
struct ChangedRow { RowId key; std::optional<RowId> from; };   // `from`: a key the action changed
struct FailedRow { RowId key; std::string message; };

/// The rows of one table that an action changed, and those it failed to. Its JSON is
/// {"rows": [{"key": …, "from": …}, …], "failed": [{"key": …, "message": …}, …]}.
template <auto Function>
struct ChangedRows {
    std::vector<ChangedRow> rows;
    std::vector<FailedRow> failed;
};

struct RenameDesignResult {
    table::ChangedRows<&lims::DesignModel::designs> designs;
};

BRIDGE_REGISTER_EFFECTS(lims::RenameDesign, ::morph::effects::updatesTable<&lims::DesignModel::designs>)
BRIDGE_REGISTER_EFFECTS(lims::ImportDesigns, ::morph::effects::changesTable<&lims::DesignModel::designs>)
```

- `changesTable<fn>` is a whole-table change.
- `updatesTable<fn>` names rows. An `updates` effect is resolved against the action's result: the result walk
  matches each slot to a record by its C++ type, and the keys are collected only at the paths where the result
  holds that record. `ChangedRows<fn>` is that slot. Its walk reports `TableOf<fn>` at its `rows` member with
  the key member `key`, so the keys of one table are collected as any record's are, and `updatesTable<fn>` on
  an action whose result has no `ChangedRows<fn>` is refused when the registries are resolved.
- **An action that changes two tables returns two members**, one `ChangedRows` per table, and declares both
  effects. One untyped list of keys could not say which table a key belongs to.
- `from` and `failed` are what a screen shows of an action's outcome per row
  ([Acting on rows](#acting-on-rows)). The walk collects a `from` as a changed key beside its `key`; a failed
  row's key is not collected, since the action did not change it.
- Watches, admission, fan-out, coalescing and `announce` are the record machinery's: a table is one more
  record.

**The table's action holds the record opaquely.** Which records an action's result holds is found by walking
the result's type, and a `GenericPage` has parallel `keys` and `rows` arrays, not objects with a key member. So
the registration states that `Table.<id>` holds the table's record with no path. That statement admits a watch
of the record to a principal who may run the action, and tells a client cache that it cannot find rows in the
value: an entry whose record is held opaquely is marked by every change of the record, keyed or not, and
nothing is merged into it ([The client's caches](#the-clients-caches)).

### Typed rows are a convenience

A row struct still describes a table, by reflection:

```cpp
BRIDGE_REGISTER_TABLE(lims::SampleModel, "samples", table::typed<lims::SampleView>(&lims::SampleModel::sampleRows))
```

`table::typed<Row>` builds the description from the row type: columns and kinds from its members, labels and
units from its schema, levels and groups from `columnLevels()` and `columnGroups()`, the key from its record. It
serves the rows through a `RowsSource<Row>`. Wherever this document shows a typed form (`columnLevels()`,
`LevelLoaded<Row>`, `Page<Row>`), it is this layer; the contract underneath is the description, the
`DataSource` and the generic page.

A row type cannot carry a unit registry, which is the application's run-time knowledge
([Measures and the unit registry](#measures-and-the-unit-registry)). A model whose typed table has a
`MeasureValue` member declares one more function, which the framework calls when it serves the table:

```cpp
std::shared_ptr<table::UnitRegistry const> tableUnits(table::TableContext const& context);
```

Its registry resolves the members' unit ids and gives the description its `units`. A typed table with a
`MeasureValue` member whose model declares no `tableUnits` is a `SourceContract` error.

### Rows on the wire

```cpp
using GenericRow = std::vector<Cell>;      // one cell per entry of the page's `columns`

struct GenericPage {
    std::string revision;                  // the description's
    std::vector<std::string> columns;      // the columns of each row, in the description's order
    std::vector<RowId> keys;               // each row's key
    std::vector<GenericRow> rows;
    // … the members of Page<Row> other than `rows`; see "Partial pages" …
};
```

A page of a generic table is row arrays under one header:

```json
{ "revision": "9f2c",
  "columns": ["id", "name", "net", "share", "weight", "dose"],
  "keys":    [101, 102],
  "rows":    [[101, "Alpha", [1250, 100], [3, 1000], [3, 10, "kg"], [5, 1, [1, 1000]]],
              [102, "Beta",  null,        null,      [200, 1, "g"], [250, 1]]],
  "total": 9093, "totalKind": "exact", "offset": 0,
  "unloadedRows": [{"index": 1, "levels": ["recipe"]}],
  "undecided": 0, "awaiting": [] }
```

- `columns` names the columns of each row array: the ones the query asked for
  ([Data levels](#data-levels)), in the description's order. `keys` holds each row's key whether or not the key
  column is among them, so a reader finds a row without knowing which column is the key.
- A cell is written by its `Cell` alternative:

  | Cell | JSON |
  |---|---|
  | empty | `null` |
  | integer; a date as days, a date-time as seconds | an integer |
  | `double`, in a `Number` column only | a number with a fraction or an exponent, so `2.0` and never `2`; NaN and the infinities, which JSON has no number for, as the strings `"nan"`, `"inf"` and `"-inf"` |
  | text | a string |
  | boolean | `true` or `false` |
  | exact value (`Rational`) | `[numerator, denominator]`, scaled to the value's decimal places where that fits: 12.50 at two places is `[1250, 100]`, and a reader takes the places back from a denominator that is a power of ten |
  | `QuantityCell` | `[numerator, denominator, [factor numerator, factor denominator]]`: the amount in the cell's own unit and its factor to the column's canonical unit. A factor of 1 is left out, which is also how a plain `Rational` in a `Quantity` column reads. |
  | `Measure` | `[numerator, denominator, unit id]`, the unit's string id |
  | unloaded | `null`, with the row and level named in `unloadedRows` |

  The column's kind, from the description, says how to read a cell; nothing is guessed from the JSON. The third
  element tells a quantity from a measure only as a check, since the kind already does. A cell that does not
  read as its column's kind is an invalid cell on the client, as it is on the server.
- An unloaded cell costs nothing per cell: the state is per row and level.
- Every other member is the typed page's, with the same meaning.
- `revision` is the description's. A client holding another revision fetches the description again and resets
  its window: the columns changed under it. On a screen the table is mounted again over the new columns, under
  the same element state, because a mounted table's columns are set once: it keeps its window, sort, filters
  and selection, and loses its realized cells and scroll offset.
- **Why rows and not columns.** Column arrays would be the same bytes. A reply is read by row: a keyed reply
  replaces rows, and a reply carries a few rows as cheaply as a page.
- **Size.** A row object repeats every column's id, which is most of the 21 bytes a cell costs in a typed page.
  Row arrays drop them (W5), and a query that names the forty columns in or near view instead of all 300
  carries an eighth of the cells (W1).
- A typed table served through `table::typed<Row>` is written in this form, its cells read by the row's cell
  readers.

### A table on the server

- **A `DataSource` per table and scope.** The engine and `apply` take a `DataSource` and `ColumnInfo`s and need
  no row type; a generic table is that, with nothing in between.
- **Level caches** are keyed by table record, level and row key. Two tables of one model that show the same
  rows, by the same key, share one cache: the cache names both records (`LevelCacheOptions::records`), is
  invalidated by a change of either, and announces an arrival on both. One load serves both tables.
- **Announcing and invalidating** are by table record, with the row keys of that table.
- **A table whose columns depend on the session** builds its description in its function. Its rows may be one
  `DataSource` with every possible column, narrowed by `mayRead`. The level caches hold raw values for a column
  list that does not depend on the caller ([The server level cache](#the-server-level-cache)) and are shared.

### How a column shows its cells

A cell crosses the wire as its exact value; the client turns it into text, in the session's locale, by the
column's display. Order, filters and distinct values always use the exact value, never the text.

```cpp
/// The expression language's two modes and names (`"halfEven"`, `"halfUp"`), so a value a document rounds and
/// the same value a column shows agree.
enum class Rounding : std::uint8_t {
    HalfEven,   // 0.125 at 2 places is 0.12
    HalfUp,     // half away from zero: 0.125 at 2 places is 0.13, the rule most test standards prescribe
};

struct ValueLabel {
    std::string operand;     // a value as a filter entry takes it: "true", "3", "V"
    std::string text;        // what the cell shows for it
};

struct ColumnDisplay {
    /// Places shown for an `Integer`, `Decimal`, `Quantity` or `Number` cell. Absent shows the value's own places
    /// (a `Rational`'s declared places, a `double`'s shortest round trip).
    std::optional<std::uint32_t> decimals;
    Rounding rounding = Rounding::HalfEven;
    /// With `decimals`, whether fewer places are padded with zeros: 2 places shows 1.5 as "1.50".
    bool padPlaces = true;
    /// Digit grouping by the locale: 12,500.
    bool grouping = false;
    /// What an empty cell shows: "–", "n/a". Empty shows nothing.
    std::string empty;
    /// The unit of a plain numeric column, shown in its header; a `Measure` column's units are its cells'.
    std::string unit;
    /// A `Date` or `DateTime` column's pattern; empty takes the session's.
    std::string date;
    /// Text for particular values: codes, booleans in words. A value no entry names shows as `decimals` says.
    std::vector<ValueLabel> labels;
};
```

- **Where it is formatted.** On the client, by `ui::formatCell(cell, column, format)`, the one function every
  backend, the copy, the status line and the filter menu call. `format` is the session's `ui::FormatContext`,
  which carries the locale's numbers, its date patterns and the display zone. The server sends no text for a
  value it can send exactly; a model that formats on the server sends a `Text` column, which then sorts and
  filters as text.
- **Rounding is for showing.** 0.125 shown at two places under `HalfUp` is "0.13", and a filter
  `eq 0.13` does not match it: the filter compares the exact value. A filter menu's condition field accepts
  more places than the column shows for that reason.
- **The values list** formats each `DistinctValue` by the column's display. Two values that show alike (0.124
  and 0.123 at two places) stay two entries, because ticking one must not match the other; the menu shows the
  value's full places for entries whose texts collide.
- **`labels`** change the text and nothing else. A column of `Bool` shows "available" and "not available", and
  its values list shows those texts; it still orders false before true. Ordering by a label's text is a `sortBy`
  over a column that holds it.
- **Empty, unloaded and invalid differ on screen.** `empty` is shown for an empty cell only. An unloaded cell
  draws its placeholder ([Cells](#cells)), an invalid one an error mark whose tooltip says the value could not be
  read.
- **A `Measure` column** shows each cell in its unit and to that unit's decimals ([Display](#display)), so
  `decimals` and `unit` on it are a `SourceContract` error.
- **The header** shows `unit` after the label, as the backend draws a unit ("Mass [kg]" or the unit on a second,
  muted line); the header's accessible name is "Mass, kg", and "Copy with headers" writes "Mass [kg]".
- **A document's column parts** `decimals`, `rounding`, `padPlaces`, `grouping`, `empty`, `unit`, `date` and
  `labels` override the description's for that column, and are the only source on a table over `rows`.

**Tone and tooltip.** A cell may carry a tone (`neutral`, `info`, `ok`, `warn`, `err`) and a tooltip, and a
row a tone. A model that decides them names **companion columns**: ordinary `Text` columns of the description,
usually `shown: false`, whose cell for a row is the tone's name or the tooltip's text.

- `TableColumn::tone` and `tooltip` name a cell's companions, `TableDescription::rowTone` the row's. A
  companion that names no column, or one that is not `Text`, is `SourceContract`.
- **They travel with their column.** A query that names a column names its companions too, and one that draws
  any row names the `rowTone` companion, so a tone or a tooltip costs a cell of the rows in view and nothing for
  columns out of the window. A companion may have a level, which makes an expensive tooltip a `display` level
  loaded for the rows in view only.
- **A document overrides them.** A column's parts `tone` and `tooltip` are expressions in the row's scope; the
  node's `rowTone` is one already. They win over the description's companions, whose columns are then not
  fetched for them. A range rule ("weight ratio above 0.5 is `err`") is one of these two places: the model's
  companion or the document's expression. The description holds no rule language of its own.
- **Drawing.** A cell with a tone is tinted with the theme's tone surface, and keeps its selection and focus
  marks over it. A tooltip shows on hover after the theme's delay and on keyboard focus at once, and is the
  cell's accessible description. A cell with a tooltip draws the theme's mark for "more here", so a user knows to
  hover.
- **Companions are not columns a user arranges.** A column that the description names as a `tone`, `tooltip`,
  `rowTone` or `rowLock` companion is never shown and is not listed in the column chooser or the header menus;
  it sorts and filters only where its own `sortable` and `filterable` say so, through a document's `sort` or
  `filters`.
- **An unknown tone name** shows no tone and is reported once for the column, as `rowTone` is.
- **A locked row.** A row whose `rowLock` companion holds text is locked: it draws the theme's locked hatch, its
  text is the row's tooltip ("Being edited by another user"), and it takes no cell edit
  ([`editing.md`](editing.md)). It can be selected and copied; whether an action accepts a locked row is the
  action's rule, and a `FailedRow` says so when it does not.

## The pipeline

```text
 in-document rows            a collection's rows          a model's table
 (a list of values)          (the session's reply)        (description; TableQuery → GenericPage)
        │                            │                            │
   ValueRowsSource              VectorSource         ViewCache, LevelCache, apply      ← server
        │                            │                            │  pages, keyed rows, change events
        └────────── table::Engine ───┘                       PageWindow                    ← client
              (client mode: sort, filter,               (server mode: the pages around
               repair, off-thread where possible)        the visible rows)
                       │                                          │
                       └──────────────── TableRows ───────────────┘
                              rows by view index, counts, changes
                                             │
                          the mount: realizes rows and columns in view
                                             │
                               MorphTable (Qt Quick), the terminal table
```

There is one of each stage. A surface chooses a source; the action's shape chooses the mode
([Where sort and filter run](#where-sort-and-filter-run)); everything below `TableRows` is shared.

## Sources

- **`ValueRowsSource`**, in the UI layer, is a `DataSource` over a document's list of values
  ([below](#a-documents-rows-as-a-source)).
- **`VectorSource`** holds a collection's rows, as [`../forms/engine.md`](../forms/engine.md) specifies.
- **A model's table** ([A table is a description](#a-table-is-a-description)) is the server-mode source: its
  `Table.<id>` action takes a `TableQuery`. The framework answers through `apply` over the table's `DataSource`,
  or through a translated plan; a slow level is served from a `LevelCache`.

### A document's rows as a source

`ValueRowsSource` turns a list of document values into typed cells, and turns a change of the list into the
smallest row changes.

- **Columns are built when used.** The source has a column for each column of the node that can be sorted or
  filtered, and builds a column's cells only while the column is in the sort chain, has a filter, or has a
  values list open. A column no one uses holds no cells, so a table of 300 filterable columns is not held
  twice. A column's cells are dropped when its last use ends.
- **A cell is a derived value.** A column's cell for a row is the column's `sortBy` evaluated in the row's
  scope, or the row's field of the column's `id`. Each is a tracked computation: when the row, or anything else
  its `sortBy` reads, changes, that cell is derived again and the source notifies `Updated` for that row alone.
  A change to what one `sortBy` reads for every row derives the column again and notifies `Reset`.
- **A new list is matched by key, then by cell.** A row whose value is the same object as before is kept
  without being read. A row whose value is another object (a refetched list shares no storage with the previous
  one) has its cells of the columns in use derived and compared with the cells held, and is `Updated` only when
  one differs, or when the row's value differs in a field no column in use reads: what the table shows of the
  row is then read again, and the row keeps its place, since its keys did not change. The cells compared are those
  of the columns in use, so deriving costs rows times columns in use, never rows times fields; a row's value is
  compared whole only when its cells are equal. Keys the previous list lacks are `Inserted`, keys the new one lacks
  are `Removed`; a list that both adds and drops keys, or reorders the rows it keeps, is a `Reset`.
- **Kinds.** A column part's `kind` (a literal: `text`, `integer`, `decimal`, `number`, `date`, `dateTime`,
  `bool`) decides the column's kind. Without it the kind is inferred from every non-null cell of the column:
  all integers is `Integer`; integers and exact decimals together is `Decimal`, an integer being an exact
  decimal; all timestamps is `DateTime`; all booleans is `Bool`; all text is `Text`. Any other mix
  takes the kind of the first non-null cell, `Decimal` when that is an integer and a decimal is among the cells,
  and a cell not of that kind is invalid: it sorts last and is reported once at the `sort` path. So integers,
  decimals and one text are a `Decimal` column with one invalid cell, and a text followed by numbers is a `Text`
  column whose numbers are invalid. A change that alters the inferred kind is a `Reset` of that column.
- **`Number` only by name.** A document value holds no `double`: a number with a fraction that reaches `rows`
  through the query cache is already an exact decimal. So inference never yields `Number`, and a column is
  `Number` only through a literal `kind: "number"`.
- **Timestamps to the second.** A timestamp becomes a `DateTime` cell of whole seconds, rounded down (toward
  negative infinity, so −1 ms is −1 s), as a model's `DateTime` column holds it. Two timestamps within one second
  tie and keep their source order, so a `rows` table orders them as a model's table does, where the expression
  language's `sort` orders them to the millisecond.
- **Text order.** The source orders text with a collator whose sort key is the default collator's fold followed
  by the text itself, which is the expression language's order for text: case-folded first, then by its own
  bytes. `null` is an empty cell and sorts last in either direction, as the expression language's `sort` places
  it.
- It keeps each row's value beside its cells for the row's scope.
- **Groups.** A grouped table's runs are adjacent rows whose `by` is the same element as the run's first row's, as
  the interpreter's `group` and `runs` compare them: numbers by value (`1`, `1.0` and `1.00` are one run, a
  quantity equals one of another unit that converts to it), lists and records element by element, `null` only
  `null`. A run's key is its first row's `by`. A run whose heading record changed while the runs kept their places
  is an `Updated` of its first source row, so its heading is read again.

## The client table model

`ui::TableRows` is what the mount reads, whatever the source:

```cpp
using SortChain = std::vector<table::SortKey>;

struct TableTotal {
    std::int64_t rows = 0;
    table::TotalKind kind = table::TotalKind::Exact;
    bool known = false;                    // false until a server-mode table's first reply
};

struct TableRowRef {
    table::RowId const& key;
    /// The row as a record keyed by column id. A field is read from the held cell when it is asked for.
    Value const& row;
};

struct TableCountsView {
    TableTotal view;
    std::optional<std::int64_t> source;    // rows before the filter, where the model knows it
    std::int64_t undecided = 0;
    bool pending = false;                  // a sort, filter or fetch is in flight
    std::vector<std::string> awaiting;     // levels the order still waits for
    std::vector<std::string> failed;       // levels whose load failed for a row the table shows or orders by
};

class TableRows {
public:
    virtual ~TableRows() = default;
    [[nodiscard]] virtual TableTotal total() const = 0;
    /// The row at a view index, or nothing while it is not fetched.
    [[nodiscard]] virtual std::optional<TableRowRef> row(std::int64_t index) const = 0;
    /// The levels not loaded for the row at a view index, and which of them failed.
    [[nodiscard]] virtual table::UnloadedLevels unloadedLevels(std::int64_t index) const = 0;
    /// Tells the model which rows and which columns are in or near view; a server-mode model fetches them.
    virtual void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) = 0;
    /// The keys of a range of view rows, for a range selection; completes when every key is known.
    virtual async::Completion<std::expected<std::vector<table::RowId>, table::TableError>>
        keysOf(std::int64_t first, std::int64_t count) = 0;
    /// Where the row with a key is in the view, fetched or not: its view index, or nothing when the view lacks it.
    virtual void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) = 0;
    virtual std::expected<void, table::TableError> setSort(SortChain sort) = 0;
    virtual std::expected<void, table::TableError> setFilter(table::FilterSpec filter) = 0;
    [[nodiscard]] virtual TableCountsView counts() const = 0;
    /// Called after the rows changed, with the operations that turn the old view into the new one.
    virtual void onChange(std::function<void(table::ViewChange const&)> handler) = 0;
};
```

- **`EngineRows`** wraps an `Engine` over a `ValueRowsSource` or a `VectorSource`. Every row is present;
  `visible` does nothing; `keysOf` completes at once, and `locateRow` answers from the engine's view; a change is
  the engine's `ViewChange`.
- **`WindowRows`** wraps a `PageWindow`, the table's description and the session. `visible` returns fetches,
  which it sends through the session's read that makes no cache entry; replies go to `accept` and `patch`; the
  table record's change events go to `refresh` and `invalidate`
  ([The window under changes](#the-window-under-changes)). A change is the diff of the rows it held against the
  rows it holds, by key. `locateRow` is a keyed fetch naming no columns, whose reply's `indices` give the row's
  view index: a reply with no row says the view lacks the key, and one that holds the row without its index is
  `InvalidValue`, an answer the rows could not give, not an absence. It owns the two timing rules the window itself has no clock for: when a fetch is sent
  while the user scrolls, and how often an awaited order is fetched again.

The mount's windowing, which decides the realized rows and columns of a large table, calls `visible` with both
ranges. The mount applies a `ViewChange` to its keyed rows as it applies a new list, so a row keeps its widget,
its native state and its selection when it moves.

**A row is a lazy record.** A document reads a row as `row.name`, whichever way the table is served. In server
mode the record is a view over the row's held cells: reading a field converts that one cell, so a row of 300
columns of which twenty are drawn converts twenty cells.

**Which columns a table fetches.** A server-mode table asks for the columns it draws and the columns its
expressions read:

- the columns in the renderer's column window ([The renderer](#the-renderer));
- for each of those, the columns its `cell`, `tone` and `tooltip` expressions read as `row.<id>`, found when the
  document loads, or, where the document gives none, its `tone` and `tooltip` companions
  ([How a column shows its cells](#how-a-column-shows-its-cells));
- the columns `rowTone` (or the description's `rowTone` companion), `rowLabel`, `onActivate` and the table's menu
  entries read, for every row;
- every column, when an expression reads the row in a way the loader cannot follow (`row[name]`, or the row
  passed whole to a function); the loader reports that once as a warning naming the node, since it turns the
  column window off for the wire.

A field read of a column the window does not hold for that row reads as absent and asks the window for the
column, so an expression the loader misjudged costs a fetch and not a wrong value.

A model-backed `table` node names a `model` and a `table` (the table's id), or a `model` and the `action` of a
typed list action. It lowers to a `WindowRows`: the description comes from the table's `describe` action,
through the query cache, and gives the node its columns, so a node that declares no `columns` shows the table
as described, and one that declares some lays those out and takes the rest of each column from the
description. Its `body` is the query's fixed part, its header sorts and its `filters` set the query's `sort` and
`filters`, and `highlightKey` is a keyed query for that row's view index. A typed list action is read into the
same window, and its description is built on the client, because a client's catalog holds an action's request
schema and never its reply's: the columns are the node's `columns` parts, or else the `v-columns` of the
action's view document, with the key from the node's `key`, the view document's `v-rowKey`, or `id`. A column's
kind is the node part's literal `kind`, else the view document's, else inferred from the first reply's rows and
then fixed for the window's life. The query is written into the request's one member marked `x-table`. A node
whose action has no such member, or which has neither `columns` nor a view document, is refused when the
document loads. The reader turns each row object into cells; it returns whole rows whatever `columns` says.

## The renderer

A table realizes a widget only for cells whose row and whose column are in or near view. Both windows are the
mount's, so every backend has them: Qt Quick's `MorphTable` and the terminal's table draw what the mount
realizes.

### When a table is windowed

```cpp
struct Virtualization {
    std::size_t threshold = 2000;          // rows: at or above it, rows are windowed
    std::size_t overscan = 10;             // rows kept realized beyond the visible range, each side
    std::size_t columnThreshold = 40;      // visible columns: above it, columns are windowed
    std::size_t cellThreshold = 20'000;    // rows × visible columns: above it, both are windowed
    std::size_t columnOverscan = 4;        // columns kept realized beyond the visible range, each side
    std::size_t realizePerFrame = 240;     // cells created or rebound in one frame at most
};
```

A table under all three thresholds realizes everything, as a short table does. A table of 1,999 rows and 300
columns is over `cellThreshold` and is windowed both ways; without that bound it would realize 600,000 cells.
A server-mode table is always windowed by rows.

### The column window

- **Which columns.** The pinned columns ([Columns](#columns-order-visibility-pinning-width)), then the visible
  columns that intersect the horizontal viewport, then `columnOverscan` columns each side. The window runs over
  the visible columns in layout order.
- **Extents live in the mount.** The mount holds each visible column's width and the running sum of their
  offsets (`ColumnExtents`). Finding the columns at a scroll position is a binary search, and a backend is given
  the offsets and widths of the columns in the window and the total width. No per-frame work reads every
  column.
- **Widths.** A column's width comes from its header, its declared width, and the cells measured while it was
  in the window. As for rows, a windowed table's content widths only grow; a width the user set does not change. A
  stretch column is the exception: its width follows the table's viewport and the widths measured so far by the
  rules of the backend contract's "Table cells", so it narrows as well as widens.
- **Anchoring.** When a column to the left of the first fully visible unpinned column grows, the horizontal
  scroll offset grows by the same amount in the same frame, so the columns in view do not move.
- **The header is windowed too.** A header cell and a group heading exist for the columns in the window only.
- **A hidden column has no header, no width and no cells.**

### Cells

- **Recycling.** A cell that leaves either window goes to a pool and is rebound to the next cell that enters.
  Cells drawn by the default cell of one role share a pool, whatever their column; a column with its own
  `cell` template has a pool of its own. A row that scrolls in therefore creates no widgets once the pools are
  warm, and a column that scrolls in rebinds the cells of the column that left.
- **A frame's budget.** One frame creates or rebinds at most `realizePerFrame` cells. Cells beyond it draw as
  blank placeholders of their column's width and are realized over the following frames, nearest the centre of
  the viewport first. A fling or a drag of the scroll thumb therefore passes over placeholders and never waits
  for realization; rows the view only passes are not realized at all.
- **Row height.** A row's height is the tallest its realized cells measure, and does not shrink when the
  column window moves: a wrapped cell that scrolls in sideways may make its row taller, and one that scrolls out
  does not make it shorter. Measured again in the same window, a row takes what it measures. A row that leaves the row window forgets its height; an unrealized row
  counts as the mean of the measured heights. When a row above the first visible row grows, the vertical offset
  grows with it. A table whose columns all have `wrap: false` has one row height and skips the measurement.
- **A row not fetched yet** draws as a placeholder row of the estimated height.
- **An unloaded cell** draws a placeholder of a fixed width in its column's alignment. A cell whose level
  failed to load draws an error mark with the same width, and the status line offers "Retry"
  ([The status line](#the-status-line)).
- The levels a server-mode table asks for are those of the columns it fetches
  ([Data levels](#data-levels)), so a level shown only far to the right is not loaded until the user scrolls to
  it.

### Keyboard, focus and assistive technology

Windowing must not change what a keyboard or a screen reader can reach.

- **Focus is a row key and a column id**, not a widget. The focused cell's row and its column stay realized
  while the table has focus, whatever the windows: one row and one column beyond them at most. When the focused
  row leaves the view, focus moves to the row now at its view index, or the last row.
- **Keys.** Arrows move the focused cell; `Home` and `End` go to the first and last visible column of the row;
  `Ctrl+Home` and `Ctrl+End` to the first and last row of the view; `Page Up` and `Page Down` move by the rows
  in view. `Ctrl+A` and `Shift` with the arrows select as they do on a short table.
- **Over rows not fetched.** Moving to a view row the window does not hold moves focus to that index: the
  window fetches it, the placeholder row holds focus and is announced as loading, and the row takes the focus
  when it arrives. With a total that is a lower bound, `Ctrl+End` goes to the last row known.
- **Counts.** The table reports its row count as the view's total when that is exact and as unknown when it is
  a lower bound, and its column count as the visible columns. Every realized row and cell reports its view row
  and its column's place in the layout, so "row 5,012 of 9,093" is read out although fifty rows exist.
- **Range selection in server mode.** A backend that selects a range itself needs every key in row order
  (`setExistingKeys`), which a server-mode table does not have. There the backend reports the gesture as two
  view rows and the lowering resolves it through `TableRows::keysOf`: rows the window holds give their keys,
  and the rest are fetched as pages that name no columns, which carry keys alone. A range of more than
  `SessionOptions::maxRangeRows` (10,000) rows is refused with a message in the status line. `Ctrl+A` selects
  the view as a whole ([Acting on rows](#acting-on-rows)), which needs no keys.
- **Selection in server mode** is a set of keys, or the view less a set of keys. A set of keys is not pruned by
  the filter, which the client cannot evaluate; `selectedCount` counts the keys, or the view's total less them.

## Every surface on the pipeline

Every surface that shows rows in columns is on the pipeline.

| Surface | On the pipeline |
|---|---|
| A document `table` over `rows` | `ValueRowsSource` → `Engine` → `EngineRows`. |
| A document `table` over a model's table (`model` and `table`) or a typed list action (`model` and `action`) | The description from the catalog's `describe` action, rows through a `PageWindow` as `WindowRows`. |
| A forms `collection` or `master-detail` | Its `VectorSource` feeds an `EngineRows` through the same table lowering. |
| A `rows` table bound to a typed page's `rows` | A client-mode table over one page, on the query cache; declared as model-backed it gets a window. |
| `PageWindow` | The server-mode client cache of every model-backed table. |
| `forEach`, `grid`, the expression language's `sort` | Not tables. |

### The document `table` node on the engine

Each property of the node, and where it is served. "Rows table" is a table over `rows`; "model-backed" is one
over `model`.

| Property | Served by | Notes |
|---|---|---|
| `rows` | `ValueRowsSource`, matched by `key` | A changed row is repaired, not sorted again. |
| `model` with `table` or `action`, `body`, `mode` | `WindowRows`, over the table's description | `table` is the id of one of the model's tables. |
| `columns` on a model-backed table | The description's columns, overridden per id by the node's parts | Optional there: absent, the table shows what its description says. |
| `columns`, `cell`, `role`, `align`, `valign`, `width`, `wrap`, `shrink`, a column's `kind` | The mount and the backend; `kind` by `ValueRowsSource` | Cells exist for columns in the window only. |
| `sort`, `sortable`, `sortBy`, `onSortChange` | `TableRows::setSort`; `sortBy` is the column's cell. `Shift`+click and the header menu build a chain ([Sort chains](#sort-chains)) | Values of one kind order as the expression language orders them. A cell not of its column's kind is invalid, sorts last and is reported once at the `sort` path. |
| `pageSize`, `page` | A slice of the engine's view | Refused on a model-backed table, whose window pages. |
| `group` (`by`, `heading`) | The run index is a hidden leading sort key computed by `ValueRowsSource`; headings are placed by the lowering after paging | Rows table only. |
| `rowSpans` | The lowering, on the page's rows | Drawn only unsorted and below the windowing thresholds. Rows table only. |
| `selectionMode`, `selection`, `onSelectionChange`, `selectColumn`, `selectAllLabel`, `rowLabel` | The lowering, by key; select-all selects the view through `Selection::selectAll` | A model-backed table has ranges through `keysOf`, and its select-all is a selection of the view ([Acting on rows](#acting-on-rows)). |
| `cellMenu`, `headerMenu`, `retainChanged`, `headerEnabled`, `maxSortKeys` | The lowering ([Acting on rows](#acting-on-rows)) | Served on every table; `retainChanged` has no effect on a table that cannot retain ([Rows an action changed](#rows-an-action-changed)), and `maxSortKeys` is a `rows` table's only. |
| A column's `decimals`, `rounding`, `padPlaces`, `grouping`, `empty`, `unit`, `date`, `labels`, `tone`, `tooltip` | `ui::formatCell` and the mount ([How a column shows its cells](#how-a-column-shows-its-cells)) | Override the description's per column. |
| `rowTone`, `onActivate` | The row's scope | Their columns are fetched for every row. |
| `empty` | Shown while the view is empty | On a model-backed table it waits for the first reply, and is not shown while rows are undecided. |
| `filters`, `onFiltersChange`, a column's `filterable` | `TableRows::setFilter`, as a `FilterSpec`; set from the header menu ([Filters from the header](#filters-from-the-header)) | Served on every table. |
| `layout`, `onLayoutChange`, `configurable`, a column's `locked` | `ColumnLayout` ([Columns](#columns-order-visibility-pinning-width)) | Every table has the header and table menus unless `configurable` is `false`. |
| `status` | The status line ([The status line](#the-status-line)) | A literal: `true`, `false`, or absent for the default. |
| `persist` | The table's persisted state ([Persisted state](#persisted-state)) | A literal. |
| `highlightKey` | A keyed query for the row's view index, then `visible` around it | Model-backed only. |
| Element values `selection`, `selectedCount`, `sort`, `viewCount`, `pending`, `page`, `pageCount`, `pageFirst`, `pageLast` | `TableRows::counts` and the page slice | On a model-backed table `viewCount` is the reply's total, with `viewCountKind` (`exact` or `atLeast`) and `undecided` beside it. Every table has `layout`, `visibleColumns`, `hiddenCount`, `filters`, `awaiting`, `failed`, `selectionSet` and `busyCount` ([Acting on rows](#acting-on-rows)). |

A document that uses none of `layout`, `status`, `persist`, a column's `kind` or `locked` loads and orders as it
does without them, with two visible differences that are intended: its tables have header and table menus, and
a table with a state store restores its arrangement.

The engine's additions for configuring a table are `oneOf`, `distinctValues`, and widths and pinning in
`ColumnLayout`. Everything else is in the UI layer: `ValueRowsSource`, the two `TableRows`, and the lowering
that reads them.

### Collections and master-detail

A `CollectionSession` keeps its rows in a `VectorSource` and leaves sorting, filtering and selection to the
engine. Its list is an `EngineRows`, and a `collection` or `master-detail` node lowers its table through the
same lowering as a document `table`, so the interpreter draws a collection's rows with the table every other
screen has. The chrome around the table (header, confirm, editor) is its own lowering and is not part of this
design. A collection is always in client mode: its `v-query` is executed with an empty body and its rows arrive
whole. A collection too large for that is a model's table shown by a `table` node.

### Screens over a server's pages

A screen that binds a `rows` table to a `Page`'s `rows` shows the page the query fetched, as a client-mode
table over those rows: a header sort orders the page. To get a window, a header sort that orders the table and
row fetches on arrival, it declares the table with `model` and `table` or `action`. The framework does not
guess that a `rows` expression is a page: an expression can be anything, and a table that changed mode because
of the shape of an expression would be the surprise this design removes.

### What the interpreter does for a table

The interpreter does not sort, filter or page a table. It builds the node's `TableRows`, reads rows from it by
view index, and does what is the document's around them: group headings, row spans, selection across pages, the
status line and the element values. The order of a table is the engine's on every surface. The expression
language's own `sort` function is not a table and orders as it does.

## The unloaded cell

`Cell` has an alternative for a value that is not loaded:

```cpp
/// A cell whose value is not loaded yet. It is neither a value nor empty.
struct Unloaded {};

using Cell = std::variant<std::monostate, std::int64_t, math::Rational, double, std::string, bool, QuantityCell,
                          Unloaded, Measure>;   // `Measure`: see "Values with units"
```

`CellState` has `Unloaded`, after its other values. `KeyColumn::read` maps an `Unloaded` cell to
`CellState::Unloaded` before
it looks at the column's kind, so every kind treats it alike, and stores no key for it.

Why a cell alternative inside the engine:

- **No second call to forget.** A cell is read only through a `RowSnapshot`, as a `Cell`. An unloaded state carried
  in the `Cell` reaches key building, `Engine::cellAt` and a renderer on a `DataSource` together with the value's
  absence.
- **Atomic with the snapshot.** A snapshot is immutable; a cell and its "loaded" state cannot disagree across
  threads when they are one object.
- **Arrival needs no new source API.** A level arriving is the row's cells changing from `Unloaded` to values:
  `VectorSource::updateRow` or `RowPatcher::patchRow`, a new snapshot sharing every other chunk, and
  `RowChange{Updated}`. `TableSnapshot`, `VectorSource`, `RowPatcher`, `ColumnSink` and `ChangeKind` are unchanged.
- **Finer than a level when needed.** A level is the unit of loading, but nothing forces every cell of a level to
  arrive together.

Cost: every reader of `Cell` must know the alternative. `Cell` does not grow (an empty struct is smaller than
`QuantityCell`), and placing `Unloaded` after the existing alternatives keeps every one's index. `displayText`
is the one `std::visit` over a `Cell`; its last branch formats the value, so the new alternative does not compile
until it has a branch.

**The wire is a second channel.** A page says which cells are unloaded beside its rows, per row and level
([Partial pages](#partial-pages)): a generic page holds `null` in an unloaded cell, and a typed row holds whatever
the server left in the member. A table node reads the page through a window, which always reads `unloadedRows`.
A reader that takes a typed page's `rows` and nothing else shows an unloaded member as empty; that is left as a
stated limit of typed pages read outside a table, because closing it means changing every row type
([Alternatives](#alternatives-considered)).

## Semantics of an unloaded cell

### Unloaded cells in the order

`compareKeys` ranks a cell before it compares keys: valid first, then empty and invalid (which tie), then unloaded
(which tie). The rank ignores the key's direction, so unloaded cells are last ascending and descending. `RowOrder`
still breaks every tie by source row, so the order stays strict and total and every sort stays stable.

Unloaded is ranked after empty, and does not tie with it, so an unloaded cell is never placed among the empties
it might not belong to. Under a single sort key the rows whose place is unknown are one tail at the end of the
view. Under a chain, a row with an unloaded cell in a later key is last among the rows that tie on the keys
before it, so there is one such tail per run of equal earlier keys.

### Unloaded cells in filters

A filter over unloaded cells is three-valued. `CompiledFilter::evaluate` returns `Match::Yes`, `Match::No` or
`Match::Unknown`:

- A column filter on an unloaded cell is `Unknown`, whatever its entries: `isEmpty` and `notEmpty` included, because
  whether the cell is empty is not known, and `exclude` included, because whether an exclude entry matches is not
  known. On any other cell it is `Yes` or `No` exactly as `passes` decides.
- A group is decided by its quantifier over its columns' answers, by the table in
  [Column groups](#column-groups). A plain group is `Yes` when any of its columns is `Yes`; otherwise `Unknown`
  when any is `Unknown`; otherwise `No`.
- A row is `No` when any column filter or group is `No`; otherwise `Unknown` when any is `Unknown`; otherwise `Yes`.

So a row is rejected by what is known even while other cells load, and admitted by a group as soon as one loaded
column matches. `FilterRun` drops a row at the stage that answers `No`; a row a stage answers `Unknown` stays a
candidate, marked, and runs through the later stages, any of which may still reject it.

A row that evaluates `Yes` is in the view. A row that evaluates `Unknown` is **undecided**: it is not in the view,
it is not rejected, and it is counted. `Engine::undecidedRowCount()` and `Page::undecided` report the count, so a
table's status line says "40 rows still loading" under an empty or short view rather than "no rows".

Undecided rows are held out of the view, not shown in it, for three reasons. A filter that shows rows it may
reject lets a user select and act on rows that do not match. The count of such a view is an upper bound, which
`TotalKind` cannot say, while the count of the decided rows is a lower bound, which `AtLeast` says. And it keeps
the engine's rule in one sentence: a cell matches an operator only when the cell is known to.

### Totals

`total` counts the rows in the view. When `undecided` is not zero the view may still grow, so the total is a lower
bound and `totalKind` is `AtLeast`; with no undecided rows `apply` reports `Exact`. Above `QueryLimits::totalCap`
`apply` does not count the view and omits `total`, and a window keeps the total it already knows. A client shows
"12 so far, 40 loading" while rows are undecided, and the cap followed by "or more" ("100,000 or more") when
the view is past the cap and the page carries no total. A
sort on an unloaded column changes no membership and leaves the total `Exact`.

### Unique values

An unloaded cell is not a value and is not the empty value. `distinctValues`
([Filters from the header](#filters-from-the-header)) leaves out a row whose cell in the
column is unloaded, and a row another column's filter leaves undecided, and reports `partial = true` when it left
any out, so a filter menu built from the list can say that more values may appear. A `oneOf` entry on an unloaded
cell is `Unknown`, as every operator is.

### Selection and edits

- A `Selection` holds `RowId`s and is unaffected by what a row's cells hold. A row with unloaded cells can be
  selected. `selectAll(engine)` selects the view, so it does not select undecided rows. With `pruneOnFilter`, a
  selected row that becomes undecided has left the view and is pruned, as any row leaving the view is.
- `CellEdits::commit` refuses an edit of a cell that is `Unloaded` in its engine's view: the cell gets the
  `FieldError` "not loaded yet" and the injected commit is not called. An edit of a value the user has not seen is
  an overwrite of whatever arrives. A `CellEdits` without an engine cannot check, and the server's action is then
  the only guard.
- An edit of a loaded cell in a row whose other cells are unloaded commits as usual. The commit's settled row may
  still hold `Unloaded` cells.

### On arrival

Arrival is an update. The source publishes a snapshot in which the row's cells hold values and notifies
`RowChange{Updated, rows}`; the engine's repair does the rest. A repair's input holds the previous undecided rows
beside the previous view (`JobInput::previousUndecided`), because a dirty row may have been in either, and its
result holds the new ones (`JobResult::undecided`).

- The rows' keys are rebuilt on copies of the key columns, and each row is evaluated again.
- A row that was undecided and is now `Yes` enters the view at its sorted place (`Inserted`), and the rows below
  it shift by one. One that is now `No` produces no view operation.
- A row that was in the view is merged into the order at its true place (`Moved`) and reported `Changed`.
- `Engine::onUndecidedChange` receives the new count whenever it changed, after `onViewChange` when the same job
  changed the view, and on its own when the job changed nothing else. Without it a row going from undecided to
  rejected would leave "40 rows still loading" on screen.
- Under `ReorderPolicy::Deferred` a row that was in the view keeps its displayed index until the view settles, as
  any updated row does. A row entering from the undecided set was not displayed and takes its true place at once,
  so `Deferred` stops displayed rows from jumping and does not stop them from shifting.

A cell may also go from a value back to `Unloaded` (a server dropping what it loaded). That is the same update in
the other direction: the row moves to the unloaded tail, or becomes undecided and leaves the view.

## Data levels

A **level** is a named unit of loading. A column of a table's description names the level its cells need:

```cpp
struct ColumnInfo {
    std::string id;
    ColumnKind kind = ColumnKind::Text;
    std::string comparator;
    std::string format;
    /// The data level the column's cells need. Empty is the base level, which every row has when it exists.
    std::string level;
};
```

Levels are strings, like column ids, so a table whose columns depend on the session names levels the same way.

**A cell of a base-level column is never `Unloaded`.** No level names such a cell, so no load would resolve it. It
is a defect of the source and is refused, not shown: key building fails its job with
`TableErrorCode::SourceContract` naming the column (from `apply` as the returned error, from an `Engine` through
`onError`, the view staying as it was).

A table's description says each column's level, and its `levels` lists them. A typed row declares its levels
once, on the row type, and `table::typed<Row>` puts them in the description:

```cpp
struct ColumnLevel { std::string_view column; std::string_view level; };

struct DesignRow {
    // … members …
    static constexpr std::array<table::ColumnLevel, 2> columnLevels() noexcept {
        return {{{"component", "recipe"}, {"quantity", "recipe"}}};
    }
};
```

`RowsSource<Row>` fills `ColumnInfo::level` from that declaration, and the row's schema carries `x-level` on each
named property, which `deriveColumns` copies to a collection's column as it copies `x-comparator`. An entry
naming a column the row does not have is refused where the columns are reflected.

Which levels a row has loaded is not a property of the description. A generic table's level columns are read
from the level's cache ([The server level cache](#the-server-level-cache)): a row the cache does not hold has
`Unloaded` cells there. A typed table says it with a predicate:

```cpp
/// Whether `level` is loaded for `row`. Empty means every level is loaded.
template <typename Row>
using LevelLoaded = std::function<bool(Row const& row, std::string_view level)>;
```

`RowsSource<Row>` and the typed `apply` take a `LevelLoaded<Row>` after `comparators`. A cell of a column whose
level it refuses is `Unloaded`; the member's own value is not read.

A query says which columns the reply carries:

```cpp
struct TableQuery {
    std::vector<SortKey> sort;
    FilterSpec filters;
    PageRequest page;
    /// The columns the reply carries, by id. Absent means every column the caller may read; empty means none,
    /// and the reply then holds keys alone.
    std::optional<std::vector<std::string>> columns;
    /// When set, the reply holds only the view's rows with these keys (see "Keyed queries").
    std::optional<std::vector<RowId>> keys;
    /// When set, the reply holds a column's or a group's distinct values and no rows (see "Filters from the
    /// header").
    std::optional<DistinctRequest> distinct;
    /// The user asked for failed loads to be tried again: the level caches load the failed keys this query
    /// needs without waiting out their retry delay.
    bool retry = false;
    /// Rows the view keeps although the filter rejects them (see "Acting on rows").
    std::vector<RowId> retain;
};
```

**The query names columns, and the levels follow from them.** A client names the columns it draws and reads
([The client table model](#the-client-table-model)); the server sends those cells and loads the levels those
columns need. This is the one definition of what a view "shows": the columns its query names. Two things rest
on it.

- **Bytes.** A page of 200 rows and 300 columns is about 1.27 MB as row objects and takes 15 ms to decode
  natively; in a browser that decode runs on the thread that paints, for the twenty columns the user can see. A
  page of the forty columns in or near view is an eighth of it (W1, W2).
- **Loads.** A level is loaded for a page's rows only when a named column needs it, so a level whose columns
  are all out of the column window is not loaded by scrolling down.

The query cache is not involved: a window's fetches make no cache entry
([The client's caches](#the-clients-caches)), so a column that scrolls into view changes no cache key. The
window holds, for each page, the columns it has, and fetches the ones it lacks.

`validate` bounds `columns` by `QueryLimits::maxColumns` (512), puts it in the description's order and removes
duplicates. A name that is no column is `UnknownColumn`, and one the caller may not read is
`ColumnNotReadable`, as for a sorted or filtered column. A client that is refused either way holds a stale
description: it fetches the description again, fits its layout to it and asks once more.

```cpp
struct LevelsNeeded {
    /// Levels of the sorted and filtered columns: the view's membership and order depend on them for every row.
    std::vector<std::string> order;
    /// Levels of the columns the query names: needed only for the rows of the reply.
    std::vector<std::string> display;
};

[[nodiscard]] LevelsNeeded levelsNeeded(TableQuery const& query, std::span<ColumnInfo const> columns,
                                        std::function<bool(std::string_view)> const& mayRead = {},
                                        Services const& services = {});
```

- `order` is the levels of exactly the columns `apply` builds keys for: every sort key's column, every filtered
  column that has an entry, each column of a group that has an `include` entry on which at least one of those
  entries compiles (as `compileFilter` decides it, with `services`, so by the column's kind and the operand), and
  the column or group of a distinct request. A group without an `include` entry, or a group's column on which no
  entry compiles, reads no cell and adds no level; nor does a filter that does not compile, since `apply` refuses
  its query.
- `display` is the levels of the columns in `query.columns`, or of every column the caller may read when
  `columns` is absent: a client that does not say what it shows is assumed to show everything.
- Both are sorted, without duplicates, and never hold the base level. A level may be in both.
- The function reads no rows. It is called on the query `validate` returned, with the services `apply` is
  given.

The two lists differ in how much a server must load. An `order` level is needed for every row the rest of the
filter does not reject, because any of them may enter or move. A `display` level is needed for the rows of the
reply only. A server loads nothing else: a view with no column of a level named, sorted or filtered does not
need the level, and its load never starts. A client-mode table calls the same function with its engine's sort
and filter and the columns of its column window.

A query is a request, not a subscription: nothing tells the server that a client removed the sort or filter that
needed an `order` level. The level cache finishes the loads it was asked for; what it loaded stays useful to the
next query that needs the level.

`apply` reads cells only of the sorted and filtered columns, and the reply's writer reads cells only of the
named columns for the reply's rows, so a source may produce every other cell lazily or not at all.

## Partial pages

A reply says which of its cells are still coming. The members below are a typed `Page<Row>`'s; a `GenericPage`
([Rows on the wire](#rows-on-the-wire)) carries the same members with the same meaning beside its `columns`,
`keys` and `rows`.

```cpp
/// A row of a page with levels that are not loaded.
struct UnloadedRow {
    std::int64_t index = 0;            // into Page::rows
    std::vector<std::string> levels;   // sorted, without duplicates, never the base level
    std::vector<std::string> failed;   // the subset of `levels` whose load failed; usually empty
};

template <typename Row>
struct Page {
    std::vector<Row> rows;
    std::optional<std::int64_t> total;
    std::int64_t offset = 0;
    TotalKind totalKind = TotalKind::Exact;
    /// The rows of this reply that have unloaded levels, ascending by index. A row not listed is fully loaded.
    std::vector<UnloadedRow> unloadedRows;
    /// Rows the filter can neither admit nor reject until a level loads. They are in no page.
    std::int64_t undecided = 0;
    /// The order levels with a load wanted or in flight for at least one row. Not empty means the order and
    /// the membership of the view may change without any row being edited.
    std::vector<std::string> awaiting;
    /// The order levels whose load failed for at least one row, and is not being tried again yet.
    std::vector<std::string> failed;
    /// A keyed reply only: the view row of each of `rows`.
    std::vector<std::int64_t> indices;
    /// A distinct-values reply only; `rows` is then empty.
    std::optional<DistinctValues> distinct;
    /// The indices of rows the view retains although the filter rejects them (see "Acting on rows").
    std::vector<std::int64_t> unmatched;
    /// Changes whenever the view's membership may have changed; a selection of the view records it.
    std::string viewRevision;
};
```

- A cell of row `i` in column `c` is unloaded exactly when `c`'s level is in the `levels` of the `UnloadedRow`
  with `index == i`. A generic row holds `null` there; the member of a typed row holds whatever the server
  left, and a client does not read it.
- `unloadedRows` lists the unloaded levels of the levels the reply needed (`order` and `display`), not every
  level of the table: a level no named column has is not looked up, so its state is not known to the reply.
- **A failed load is still unloaded.** A cell whose load failed has no value, so the engine treats it as any
  unloaded cell: it sorts last and its filter is `Unknown`. What differs is what the client is told, so a table
  can stop saying "loading": the level is in the row's `failed`, and for an order level in `Page::failed`
  instead of `awaiting`. `awaiting` and `failed` together are the order levels that leave rows unloaded.
- The members beyond `rows`, `total`, `offset` and `totalKind` are absent-tolerant: a `Page` read without them
  is a fully loaded page. A server writes them as the example in [Rows on the wire](#rows-on-the-wire) shows:
  `unloadedRows`, `undecided` and `awaiting` always; `failed`, `indices` and an `UnloadedRow`'s `failed` only
  when not empty; an optional member such as `distinct` only when set. A reader takes each as present, empty
  or not, or absent.

`PageRows`, what `apply(DataSource const&, …)` returns, carries `totalKind`, `undecided`, `awaiting`, `failed`
and, for a keyed query, `indices`. The framework builds a generic page from it and takes each row's unloaded
levels from the level snapshots it read; the typed `apply` copies the same members into a `Page<Row>` and builds
`unloadedRows` from its `LevelLoaded`.

```cpp
/// What is not loaded for one row: views into the page or window that holds the row.
struct UnloadedLevels {
    std::span<std::string const> levels;
    std::span<std::string const> failed;
};
```

A `PageWindow` keeps each held row's unloaded levels (`unloadedLevels(index)`), and the `undecided()`,
`awaiting()` and `failed()` of the latest page reply; `reset` clears them. Unloaded cells do not make a page
incomplete: an unloaded row is a row the server has sent.

## Keyed queries

A query with `keys` set asks for specific rows of the view through the table's own action, so the model's
scope and authorisation apply to it as to a page.

- The view is the one a page of the same sort, filter and `retain` has. The reply's `rows` are the view's rows whose key
  is in `keys`, in view order, with the query's `columns`; `indices` holds each one's view row; `unloadedRows`
  describes them. A key that is not in the view (the row was removed, is rejected, or is undecided) has no row
  in the reply.
- `page` does not apply; `validate` refuses a keyed query with a non-zero `page.offset` as `InvalidSpec`, one
  that also sets `distinct` as `InvalidSpec`, and more than `QueryLimits::maxKeys` (1,000) keys as
  `LimitExceeded`. Each text key is bounded by `QueryLimits::maxTextLength`.
- **A keyed reply carries the view's `total`, `totalKind`, `undecided`, `awaiting` and `failed`**, as a page
  does. A window does not adopt them: keyed replies can arrive out of order, and only page replies, which a
  revision orders, set the window's totals. It compares them with its own, and a difference tells it that rows
  it did not ask about have entered or left the view
  ([The window under changes](#the-window-under-changes)).
- The server answers a keyed query from the view it has cached for the sort and filter
  ([The view cache](#the-view-cache)): a key's view row is a lookup, not a sort.
- A client whose keyed fetch is refused with `Unavailable` falls back to `invalidate`.

## Arrival

No part of the path polls. Each step is caused by the one before it.

1. **The level cache** ([Caching layers](#the-server-level-cache)) finishes a load for some rows and stores the
   values. A read of the cache from that moment returns them.
2. **An engine on a source fed from it** (a client-mode table, or a server that keeps one) is given the rows as
   `RowChange{Updated, rows}` and repairs them, as in [On arrival](#on-arrival). Its `ViewChange` names exactly the
   rows that entered, moved or changed.
3. **The server tells the watchers.** The table is a registered record
   ([The table's record](#a-models-tables)), and the cache announces the rows that arrived, after it stored
   them:

   ```cpp
   /// Reports a change to rows of `record` that no execute made. Callable from any thread.
   /// `keys` absent reports the whole type. An unregistered record is refused and nothing is sent.
   std::expected<void, std::string> RemoteServer::announce(std::string_view record,
                                                           std::optional<std::vector<glz::raw_json>> keys);
   ```

   `announce` builds its `effects::Changes` with the helpers an execute's changes are built with, so the record's
   derived records are included and more than `effects::kMaxChangeKeys` keys become a whole-type change. It posts
   to the server strand and runs `fanOut` with no caller, as a time fact does: watches, re-authorisation and
   coalescing apply unchanged, and a time-fact reporter naming the record is asked again. An announced change is
   not handed to the change observers, so an arrival never invalidates the cache that announced it. The level is
   not in the event: the keys are enough, and a client asks for what it shows.

   Keys cross as JSON. `table::changeKeys(std::span<RowId const>)` writes an integer key as a JSON number and a
   text key as a JSON string; `table::rowIdOf(glz::raw_json const&)` reads one back, and an event key that is
   neither makes the client treat the event as a whole-type change. The table's key column and the keys an
   action's `ChangedRows` names must write the same JSON for the same row; a test of each table holds them to it.

   A row-scoped table's record has `privateKeys`, and an announcement has no caller, so it reaches every
   watcher without its keys: each watcher refetches its visible pages instead of its rows. That is the price of
   not telling a watcher the key of a row it may not see. It is one page request per watcher per coalesced
   event, answered from the view cache.

   A load that fails is announced the same way, once, with the failed keys, so a client stops showing them as
   loading.
4. **The client's cache** takes the event. A window answers with a keyed fetch for the event's keys, or with
   page fetches ([below](#the-window-under-changes)); the query cache marks the entries concerned and they fetch
   their page again ([Caching layers](#the-clients-caches)).
5. **The server answers** the keyed query or the page through the same action, from the view cache and the
   level cache: nothing is loaded again, and the view is computed again only when the arrival was of a level
   it orders by, once for all its watchers.
6. **The window patches** the rows in place and reports their view rows; the renderer reports exactly those rows
   as changed.

The record's `changes` events are not only arrivals. An action that edits a row declares `updates` or `changes` on
the same record, and its event reaches the same caches. A client cache therefore treats every event alike and
never assumes an event is an arrival.

## The window under changes

A window holds the rows of a model-backed table around the visible rows, and for each page the columns it has
fetched. `PageWindow<GenericRow>` is the window of every `table` node: a row's key is its entry of the page's
`keys`. `PageWindow<Row>` holds a typed page's rows for a C++ client, with a `RowKey<Row>` function for the key
and no column window, since a typed row is whole.

```cpp
struct PageFetch {
    PageRequest request;
    std::vector<std::string> columns;      // the columns asked for; a held page asks for the ones it lacks
    std::uint64_t query = 0;
    std::uint64_t revision = 0;
};
struct RowsFetch {
    std::vector<RowId> keys;
    std::vector<std::string> columns;
    std::uint64_t query = 0;
    std::uint64_t sequence = 0;
};
enum class Refresh : std::uint8_t { Nothing, Rows, Pages };
struct RefreshPlan { Refresh kind = Refresh::Nothing; RowsFetch rows; };

explicit PageWindow(std::int64_t pageSize = 200, std::size_t maxPages = 8, RowKey<Row> key = {},
                    std::size_t maxKeys = 1000, std::size_t maxColumns = 96);

std::vector<PageFetch> visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns = {});
std::vector<PageFetch> superseded() const;          // fetches in flight for pages no longer near the visible rows
void cancelled(PageFetch const& fetch);
RefreshPlan refresh(std::span<RowId const> keys);   // a keyed change of the table's record
RefreshPlan owed();                                 // after each accept
void invalidate();                                  // a whole-type change, a resync, a refused keyed fetch
bool accept(PageFetch const& fetch, Page<Row> reply);           // false when the reply is ignored
std::vector<std::int64_t> patch(RowsFetch const& fetch, Page<Row> reply);
```

The window has no clock and sends nothing: it returns what to fetch, and `WindowRows` decides when
([When a fetch is sent](#when-a-fetch-is-sent)).

### Pages and columns

- A held page stores its cells by column, so a column that scrolls into the column window is added to the page
  and one that has left it is dropped without touching the others. A window holds at most `maxColumns` columns
  a page; over that, the columns farthest from the column window go first, and never a column the latest `visible`
  named. `visible` takes the first `maxColumns` of the columns it is given, so a wider set cannot be dropped and
  fetched again without end.
- `visible(first, count, columns)` covers the visible rows and one page either side. For a page it does not
  hold, or holds stale, it returns a fetch of the whole page with `columns`. For a page it holds complete but
  without some of `columns`, it returns a fetch of the same rows naming only the columns the page lacks. One
  fetch per page is in flight at most.
- `accept` of a column fetch adds the columns when the reply's `keys` are the page's keys, row for row. When
  they are not, the rows moved between the two fetches, and the window invalidates.
- `accept` of a page reply whose `total`, `totalKind` or `undecided` differ from the window's adopts them and
  marks every other held page stale: the view changed between two replies, whatever the events said. An absent
  `total` is neither adopted nor a difference.
- **A view that shrank holds nothing past its end.** When the total the window holds is exact, a held row at or
  past it is dropped (a page wholly past it goes, the page it falls in is cut and is complete), and a fetch in
  flight for a page wholly past it is forgotten, so its late reply is ignored. Otherwise such a page is never
  fetched again, and its first key would be a probe no reply answers.
- `superseded()` returns the fetches in flight for pages that are no longer within one page of the visible
  rows. The client cancels them through their stop tokens and tells the window with `cancelled`.

### `refresh(keys)`: rows or pages

- `Pages` (it calls `invalidate`) when the window has no key function; when `awaiting()` is not empty, because
  nearly every arrival then moves rows; or when the keys it would ask for exceed `maxKeys`.
- Otherwise `Rows`, asking for **every key of the event**, held or not, and for one **probe** per run of held
  rows: the key of the run's first row. A held row with no unloaded level is asked for too: the event may be an
  edit.
- `Nothing` when the window holds no row.
- While any page fetch is in flight, it also records the event's keys.

An event's keys are asked for whether or not the window holds them because an edit can bring a row the window
has never seen into its pages, and the author of an action cannot know how each client has sorted. The reply's
view rows say what happened ([`patch`](#patchfetch-reply)).

**`owed()`** closes the race between an event and a page reply. The reply to a page fetch and the event for a
change leave the server separately and arrive in either order, so an event can precede the reply whose rows it
concerns. After each `accept` the client calls `owed`, which returns `Rows` with the keys recorded while the
fetch was in flight. The recorded keys are dropped when no page fetch is in flight, and whenever the plan is
`Pages`: that invalidation leaves every reply still in flight older than the window, so the refetch that follows
covers every recorded key. Kept, they would invalidate again at every `accept` while two pages are in flight, with
no event behind it.

### `patch(fetch, reply)`

`patch` replaces a held row when the reply has a row with its key at the view row the window holds it at,
updates its unloaded levels, and returns the view rows it replaced, ascending. It calls `invalidate` instead
when the reply shows that rows shifted:

| The reply shows | Meaning |
|---|---|
| A requested key the window holds has no row | The row left the view. |
| A key the window holds is at another view row | The row moved, or a row above it entered, left or moved across it. The probes are held keys, so this catches a shift caused by a row the window does not hold. |
| A key the window does not hold is at a view row inside a held page | A row entered the pages. |
| `total`, `totalKind` or `undecided` differ from the window's | A row entered or left somewhere in the view. Checked only on the reply to the latest keyed fetch sent, so an old reply cannot report an old total. |

Why this is enough: a change that leaves every held row at its view row, with the same content, and leaves the
totals as they were, changes nothing the window shows. Any other change moves a held row, which a probe or a
requested key reports; or brings an unheld key into a held page, which its view row reports; or changes a
total. A check that fires wrongly, because a keyed reply and a page reply crossed, costs one page fetch and
ends there, since the refetched page agrees with itself.

Each `RowsFetch` has a sequence, and the window remembers the latest sequence that asked for each held key: a reply
row whose key a later fetch has asked for is ignored, missing or not, so an out-of-order reply cannot put older
values over newer ones. A reply of an older query is ignored. `patch` never changes which rows are held or
their order. Only a held key is remembered, and it is forgotten when it leaves the held pages, so the memory
never outgrows the held rows: a row for a key the window does not hold is ignored outside the held pages and
refetched inside them, whatever its sequence.

### `invalidate()`

`invalidate` takes a new revision and marks every held page stale. It keeps the held rows showing and keeps the
fetches in flight.

- `visible` returns a whole-page fetch, tagged with the current revision, for each stale page around the visible
  rows that has no fetch in flight.
- `accept` takes a whole-page reply whose revision is not older than the page's held content, and replaces the
  page. The page is complete only when the reply's revision is the current one; otherwise it stays stale and the
  next `visible` fetches it again. A reply older than the held content, or of an older query, is ignored.
- So a window invalidated more often than a round trip takes still advances by one reply per round trip, and is
  current one round trip after the last event. The client calls `visible` after every `accept`.

### When a fetch is sent

`WindowRows` owns two delays. Neither asks for anything because time passed: each only holds back or merges a
request that a scroll or an event caused.

- **Scrolling.** A fetch for a page next to a held page is sent at once: that is ordinary scrolling, and the
  held margin hides the round trip. So is the page of the first visible row while no page is held: the first
  query's page, and the first after a sort or filter, since nothing is passing over it. A fetch for a page
  farther away (a drag of the scroll thumb, a fling, `Ctrl+End`) is sent when the visible rows have stayed
  within one page for `SessionOptions::tableSettle` (80 ms). Columns follow the same rule sideways. Every fetch `superseded()` returns is cancelled. A drag of the
  thumb across 100,000 rows therefore sends a fetch where it rests, not five hundred on the way.
- **An awaited order.** While `awaiting()` is not empty, every event invalidates. `WindowRows` sends the first
  refetch at once and the next no sooner than `SessionOptions::tableAwaitedRefetch` (250 ms) after it, so a
  watcher of a level that loads for a minute fetches its visible pages four times a second at most, and once
  more after the last arrival. A loader announces per batch of rows, not per row.

## Caching layers

A value that is slow to load is loaded once and kept until something says it is wrong. Five layers hold what a
table shows on the way to the screen, and one notification, the table record's change event, refreshes all of
them.

| Layer | Holds | Key | Bound | What removes or refreshes a value |
|---|---|---|---|---|
| Server level cache, `table::LevelCache<Value>` | The loaded values of one level, as the loader returned them, for every principal | table record, level, row key | `maxBytes`, least recently used first | `invalidate`; an action's effects on a record in `invalidatedBy`; eviction at the bound |
| Server view cache, `table::ViewCache` | The order and the undecided rows of a view | table record, `ServedTable::scope`, validated sort and filter | `maxBytes`, least recently used first | A new snapshot of the source; a new generation of an order level |
| Client query cache, the interpreter's `Session` | Whole replies of a query: a table's description, a distinct-values list, a page read by a `rows` table | model type, instance token, action, canonical body | `keepUnused` after the last query leaves | A change event marks it and it fetches again; `resync`; a move onto it after `staleTime` |
| Client `PageWindow` | The pages of a model-backed table around the visible rows, the columns each holds, each row's unloaded levels | query, then page index | `maxPages` (8) of `pageSize` (200) rows, `maxColumns` (96) columns | A change event (`refresh`, `invalidate`); `reset` on a new sort or filter |
| Renderer | The realized cells and the column widths measured from them | row key and column id | The two windows | The mount's diff of the rows it is given |

Nothing below the server level cache ever causes a level to be loaded twice: a client cache that fetches again is
answered from the level cache.

### The server level cache

The framework owns the cache; the model supplies only the function that reads the values.

```cpp
enum class LevelRead : std::uint8_t { Loaded, Unloaded, Failed };
enum class LevelUse : std::uint8_t { Display, Order };

/// One state of a level. Immutable: nothing it returns changes while it lives, and holding it blocks no one.
class LevelSnapshot {
public:
    virtual ~LevelSnapshot() = default;
    /// Writes the row's cells for the level's columns, in the order of `LevelSource::columns()`.
    [[nodiscard]] virtual LevelRead read(RowId const& key, std::span<Cell> out) const = 0;
    /// Differs between two snapshots that differ in any value.
    [[nodiscard]] virtual std::uint64_t generation() const = 0;
};

/// What the framework needs of a level, whatever the type of its values.
class LevelSource {
public:
    virtual ~LevelSource() = default;
    [[nodiscard]] virtual std::string_view level() const = 0;
    /// The level's columns. The same for every caller.
    [[nodiscard]] virtual std::span<ColumnInfo const> columns() const = 0;
    [[nodiscard]] virtual std::shared_ptr<LevelSnapshot const> snapshot() const = 0;
    /// Says the keys are needed. `Unavailable` when an order use cannot fit (rule 10).
    virtual std::expected<void, TableError> want(std::span<RowId const> keys, LevelUse use, bool retry = false) = 0;
};

struct LevelCacheOptions {
    std::vector<std::string> records;          // the table records it serves; one, or several that share rows
    std::string level;
    std::vector<ColumnInfo> columns;           // the level's columns, for every caller, in a fixed order
    std::size_t maxBytes = 256U << 20U;
    std::size_t expectedBytes = 0;             // a value's size before any is stored; 0: 64 bytes a column
    /// Records whose change invalidates values. Default: `records`.
    std::vector<std::string> invalidatedBy;
    exec::IExecutor* loads = nullptr;          // where the loader runs; never the server strand
    std::size_t batch = 64;                    // most keys handed to one loader call
    std::chrono::seconds orderLease{60};       // how long an order use keeps its keys from eviction
    std::chrono::seconds retryAfter{30};       // before a failed key is loaded again; doubles to `retryMax`
    std::chrono::seconds retryMax{600};
    std::function<std::chrono::steady_clock::time_point()> clock;   // empty: the steady clock
};

struct LevelCacheStats {
    std::size_t hits, misses, loaderCalls, loadedKeys, failedLoads, droppedLoads, invalidatedKeys, evictedKeys,
        leasedKeys, refusedOrders, bytes;
};

template <typename Value>
class LevelCache final : public LevelSource, public effects::IChangeObserver {
public:
    /// Reads the level for `keys`. Called on `options.loads`; may block. It returns a value for every key it
    /// read, including a row that has nothing at this level, whose value is then empty. A key it returns no
    /// value for failed; throwing fails every key of the call.
    using Loader = std::function<std::vector<std::pair<RowId, Value>>(std::span<RowId const> keys)>;
    using SizeOf = std::function<std::size_t(Value const&)>;                        // deep bytes
    using Cells = std::function<void(Value const&, std::span<Cell> out)>;           // in `columns` order

    LevelCache(LevelCacheOptions options, Loader loader, SizeOf sizeOf, Cells cells);
    /// For `Value` = `std::vector<Cell>` only: the value is the cells, and its size is their deep size.
    LevelCache(LevelCacheOptions options, Loader loader);

    void invalidate(std::span<RowId const> keys);
    void invalidateAll();
    void onLoadFailed(std::function<void(std::span<RowId const>)> handler);
    [[nodiscard]] LevelCacheStats stats() const;
};

using CellLevelCache = LevelCache<std::vector<Cell>>;
```

**What a value is laid out against.** A level's columns are `LevelCacheOptions::columns`: one list for every
caller, declared when the cache is made. A generic value is one cell per entry of that list, in its order; a
typed value is turned into the same cells by `Cells`. A caller's description may hold fewer of the level's
columns than the list, in another order; the framework maps each of the caller's columns to its place in the
list by id and passes it through `mayRead`. A cached value is never laid out against a description, because
descriptions differ by caller and the cache is shared.

**Size is deep.** `maxBytes` bounds what the values own, not `sizeof(Value)`: a `std::vector<Cell>` of twenty
cells is 24 bytes by `sizeof` and about 1.1 kB in truth. `CellLevelCache` measures cells and their texts
itself. A cache of another value type is given `SizeOf`; there is no shallow default.

**How the framework uses it**, for every query of a table: `levelsNeeded` on the validated query; a snapshot of
each needed level; the view, from the view cache or `apply`, over base columns from the table's source and
level columns from the snapshots, `Unloaded` where a snapshot holds no value; the reply; then `want` with the
keys the reply needed: the reply's rows for a `display` level, every row the filter did not reject for an
`order` level. The reply is built entirely from the snapshots taken at its start, so its rows and its
`unloadedRows` describe one state of each level.

Rules:

1. **Hit.** A key the cache holds is served to every query, page, session and client until it is invalidated or
   evicted. No age expires it.
2. **One load.** `want` starts a load only for keys neither held nor in flight. Queries that need the same rows
   while a load runs wait for that load's arrival event like any other client.
3. **Order of work.** Keys are loaded in batches of `batch`, `Display` keys before `Order` keys and the most
   recently wanted first, so the rows a user is looking at now come before the rows a sort needs eventually.
4. **Raw values, scoped reads.** The cache holds what the loader returned and knows no principal. A value reaches
   a reply only through a row of the caller's own source and a column `mayRead` allows, both decided before the
   cache is read. A level whose values differ by principal is not one level: it is declared as one level per
   scope, or not cached.
5. **Invalidation is explicit**, and there are three sources.
   - The application calls `invalidate(keys)` or `invalidateAll()`.
   - An action's effects. The server hands every change an execute or a time fact derived to each attached
     observer, on the server strand, before it queues the change's events. A keyed change of the row's own record
     invalidates those keys; a whole-type change of it invalidates the level. The action's own event is the
     notification, so the cache announces nothing.
   - A change of another record in `invalidatedBy` (a recipe, when the level is a design's recipe data). Its keys
     are not row keys, so the whole level is invalidated, and the cache announces the table's record without keys,
     because no event for the table's record exists yet.
6. **An effect invalidates every level of the rows it names.** Effects name records, not levels, so a change to a
   row is taken to change everything derived from it. That over-invalidates by one row's reload per edited row. A
   level that does not depend on the row's own record says so by leaving the record out of `invalidatedBy`.
7. **A load in flight does not land stale.** Each row has a generation that an invalidation advances. A load
   remembers the generation of each key when it starts and stores a value only where it is unchanged; the rest
   are dropped and counted, and the query that follows the invalidation's event wants them again.
8. **Stored, then told.** A load publishes its values before it announces their keys; an invalidation removes
   its values before any event for it is queued. A client that fetches because of an event therefore reads the
   state the event is about.
9. **Bound.** Over `maxBytes`, the least recently used values that no order lease holds are evicted. An evicted
   value is absent, its cells are unloaded in the next reply, and that reply's `want` loads it again. An
   eviction sends no event: the clients that hold the value hold a correct one.
10. **An order use is leased, and refused when it cannot fit.** A sort or filter needs the level for every
    candidate row at once.
    - `want(keys, Order)` gives each key a lease until `orderLease` from now. A leased value is not evicted, so
      a view that is ordering by the level does not lose rows to another query's loads, and two order uses
      cannot evict each other in turn. A query that needs the level for order renews the lease and becomes one
      more holder of it; `unwant`, which a query refused by another level calls, ends one holder, and the lease
      ends with its last. A view's order keys are one kept set (`wantSet`), as are a distinct request's (the
      "every row" entry of [The view cache](#the-view-cache)), and its lease is the set's: the
      set holds a count of the queries holding it and a lease end. The first query of a set is fitted and
      protects every key of the set, a chunk at a time under the writer mutex, by a per-key count of the sets
      protecting it; that is the only pass over the keys. A repeat while the set's lease runs is one more holder
      and a longer lease, under the lock, touching no key, unless the cache's count of set events (a protected
      key removed, a protected failure's delay passed) has moved since that set last looked, when it also queues
      the keys the set misses; each set compares its own last-seen count, so one set's repeat consumes no other
      set's event. A fit also releases the sets no query has wanted since the level's generation moved past the
      one they were last wanted at: a view is found only at its level's generation, so their views are
      superseded. An accepted set's keys count in the level's bound. `unwant` of the set is one
      holder fewer; the set's protection is taken off its keys, a chunk at a time under the lock, only with its
      last holder or when its lease has run out. A repeat that arrives while the set's fitting `want` runs waits
      for it, and is then a repeat, or fitted itself when that want was refused. A key is kept from eviction
      while its own lease runs or a set protects it. A level no query has ordered by for `orderLease` is
      evictable again. Leases are compared with the clock when a request reads them; nothing is scheduled.
    - The cache decides whether the use fits before it loads: the keys times the mean size of its stored values
      (`expectedBytes` while it holds none), plus the bytes other leases hold, against `maxBytes`. Over it,
      `want` returns `Unavailable`, the framework refuses the query naming the column, and nothing is loaded.
      The second of two order uses that do not fit together is the one refused, while the first's lease lives.
    - Sizes are not known before a load. When leased values come to exceed `maxBytes` while they load, the cache
      stops loading for order, drops the leases, ends every set, and announces the table's record without keys;
      the next query meets the estimate, which is now measured, and is refused.
    - A `Display` load that finds nothing evictable is stored over the bound and is the first evicted. The
      overshoot is at most one batch for each page being answered.
11. **A failed load is remembered.** A key the loader returned no value for is `Failed` in every snapshot until
    it is loaded, and is announced once, with its key, so that clients stop showing it as loading. `want` does
    not load a failed key again until `retryAfter` has passed since the failure, doubling with each failure in
    a row up to `retryMax`; the delay is read from the clock when a query wants the key. `want(…, retry: true)`,
    which a query carrying `TableQuery::retry` makes, loads the failed keys it names at once. An invalidation
    clears the failure with the value. A failed key is not in `awaiting`, so a level that cannot be read does
    not make every window refetch on every event.
12. **Snapshots are immutable, and nothing waits on a reader.** The values are held in shards by key hash, each
    an immutable sorted vector behind a shared pointer. The shard count is a power of two that doubles as the
    cache grows, so a shard averages at most 64 rows and a change copies little beyond the rows it touches. A
    published state is the shard pointers and a generation behind one shared pointer, and a snapshot holds it.
    A store, an eviction, a removal and the observer call prepare their new state from the published one with
    no lock held (the shard copies, the scans and sorts, the re-spread), then take a writer mutex only to check
    that nothing was published meanwhile, preparing again if it was, and to publish; the state is published
    under a second mutex held for the pointer swap alone, and a reader takes only that one. A `want` takes its
    keys 1,024 at a time under the writer mutex and releases it between chunks; inside a chunk each key is
    classified, leased and queued at once, so an eviction's publish sees every lease given before it and a key
    removed before its chunk is queued again. A reply may hold a snapshot for as long as its `apply` takes,
    40 ms at 100,000 rows, and the server strand's observer call still returns in the time its keys take, plus
    one chunk of a want at most. A held value's recency and order lease (its end and its number of holders) are
    atomics on its slot, and the lease of a key not held yet is kept, under the writer mutex, until the key is
    stored; none of these is part of a snapshot's state. A whole-level invalidation publishes empty shards.
13. **Threads.** `snapshot`, `want` and `invalidate` may be called from any thread. The loader runs on `loads`.
    The observer call runs on the server strand and only removes entries: it does not load, wait or call out.

The server learns of a cache through `RemoteServer::attach(std::shared_ptr<effects::IChangeObserver>)`, which the
application calls after it constructs the server. `attach` gives the observer its announce sink
(`effects::AnnounceSink`, a function taking a record name and optional keys, holding the server weakly), and
registers it for derived changes. `IEnvelopeService::bound` is not used for this: it runs at the first envelope a
service answers, which may be long after the first page, or never.

### The view cache

Sorting and filtering 100,000 rows takes 33 to 46 ms. Without a cache every request pays it: each page of a
scroll, each keyed fetch, and, while an order level loads, every watcher once per arrival. The server therefore
keeps the views it has computed.

```cpp
struct ViewCacheOptions {
    std::size_t maxBytes = 64U << 20U;     // across the server's tables; the views, not the snapshots they keep
    std::function<void()> onCompute;       // called on the computing thread as each computation starts
};
```

An entry keeps the snapshot it was computed over alive, and `maxBytes` does not count that snapshot: an entry for
a sort nobody asks for again holds its rows until it is evicted or found invalid. Each request carries how
recently its snapshot was pinned and the generation of each order level it read. An entry is replaced only by one
at least as new in every part; a request that is older in any part computes its view without displacing the entry,
so a straggler never undoes a newer view.

- **Key.** The table's record, `ServedTable::scope`, and the validated query's sort and filter in canonical
  form. `page`, `columns`, `keys`, `distinct` and `retain` are not in it: every page, every column set and every
  keyed fetch of one sort and filter read one entry. A query with `retain` merges its retained rows into the
  entry's view per request ([Rows an action changed](#rows-an-action-changed)), so sessions that retain
  different rows still share one sort.
- **Valid while** the table's source has the snapshot the entry was computed from and each of the view's order
  levels has the generation it read. A request finds an entry, compares those, and uses it or computes again.
  An entry is never repaired in place.
- **Holds** the view's order as source rows (4 bytes a row), the undecided rows, `total`, `awaiting` and
  `failed`, and, built at the first keyed query, the view row of each source row (4 bytes a row). A view of
  100,000 rows is 400 kB, or 800 kB once a keyed query has asked. A view whose query sorts or filters by a
  level also keeps the keys of its rows and undecided rows, the set its order levels are wanted with:
  `sizeof(RowId)` a row, plus a text key's heap buffer, counted against the bound like the rest. A distinct
  request wants its order levels for every row, so the cache also holds one "every row" entry per table record
  and `ServedTable::scope`, keyed under a query no sort or filter writes, valid while the table's source has the
  snapshot it was made from and whatever the levels' generations: its rows and their keys, so the distinct
  requests over one snapshot share one kept set and repeat it.
- **One computation.** Requests that need the same entry while it is computed wait for that one computation. So
  fifty watchers of one view that refetch after an arrival cost one `apply`, not fifty.
- **Where it runs.** Finding, computing and reading an entry, and encoding the reply, run on
  `ServerConfig::tableWork`, over immutable snapshots. Neither the model's strand nor the server strand
  waits for a sort.
- **Bound.** Over `maxBytes` the least recently used entries go. An entry is also dropped when it is found
  invalid.
- **What it does not hold.** Cells: a reply reads them from the source and the level snapshots. Distinct
  values: a values list runs the filter again without the column's own entry, and its reply is cached by the
  client's query cache.

A scoped table whose `scope` is left empty would share views between callers who see different rows. The
framework cannot see that from the outside, so `table::rowScoped` and an empty `scope` together are refused
when the table is first served, as `SourceContract`.

### The client's caches

Both client caches hold pages until a change event marks them. Neither refetches because time passed.

**The query cache** is told three things about a result that holds records, none specific to tables. The first
two a holder type declares, as a type declares `schemaMarker()`:

```cpp
static constexpr auto openMembers() noexcept;                 // Page: "awaiting", "distinct"
static constexpr std::string_view partialMember() noexcept;   // Page: "unloadedRows"
```

The result walk that records where an action's result holds each record also records these two members' paths,
and the catalog carries them as `ActionRecordInfo::open` (a list: the entry is open while any is set) and
`ActionRecordInfo::partial`. The cache reads the values at those paths and knows nothing else about the result's
shape. A member counts as set when it is a non-empty array, an object, `true`, or a number other than zero. The
third is `ActionRecordInfo::opaque`, which a registration states when a result holds a record at no path
([The table's record](#a-models-tables)).

- **An open entry is marked by every change of its record.** While `open` is set, a row the entry does not hold
  may come to belong to it, so a keyed event marks the entry whether or not it holds one of the keys. This is what
  makes an undecided row that becomes admitted refresh the page it now belongs to.
- **A partial entry is not merged into.** While `partial` or `open` is set, an `updates` effect on the record acts
  as `changes` for that entry: it is marked and fetched again. A merge replaces rows by key and cannot update a
  sidecar indexed by position, so it would leave a merged row's unloaded state describing the row it replaced. The
  cache applies the same downgrade to the effects of a discarded `latest` reply.
- **An opaque entry is always both.** The cache finds an entry's rows by the record's key member at the
  record's paths, and a `GenericPage` has neither. Every entry of a `Table.<id>` action (a page a document
  query reads, a distinct-values list) is marked by every change of the table's record and is never merged
  into. A table's description is an entry of `Table.<id>.describe`, which holds no record: it is replaced when
  a reply names another `revision`.
- **An opaque entry keeps fractions.** A `Value` holds no JSON number with a fraction or an exponent, and an
  ordinary action's reply that has one is refused. A page's `Number` cells are such numbers, so the reply of an
  opaque entry is decoded by a table-aware variant of `valueFromJson`: a number with a fraction becomes the
  decimal `Value` its shortest text spells (`table::parseDecimal`), and one that does not parse as a decimal
  becomes its text. Every other action keeps the strict rule. A document's `rows` bound to such a page
  therefore hold decimals, and a column of fractional numbers there infers as `Decimal`.

**A window's fetches are not queries and make no entry.** The session has a read for them and a way to watch
without an entry:

```cpp
/// Runs a read action and resolves with its reply's JSON text, or fails as an entry's fetch fails. Makes no
/// cache entry and reads none. A stop request sends the wire's `cancel` for the call, where the server takes
/// one, and drops the reply in either case.
async::Completion<std::string>
Session::read(std::shared_ptr<ModelChannel> const& channel, std::string const& actionId, Value const& body,
              ::core::async::StopToken stop = {});

/// Watches `record` for as long as the returned holder lives, and hands its events to `handler` on the
/// runtime's owner. Resolves when the server has the watch.
async::Completion<WatchHolder>
Session::watchRecord(std::string const& record, std::function<void(effects::RecordChange const&)> handler);
```

- A watch is counted per holder, as it is per entry: the session's watch set names a record while any entry or
  any holder needs it.
- **Watch, then fetch.** `WindowRows` sends its first page fetch after `watchRecord` resolves, as an entry
  watches before it fetches, so no change between the reply and the watch is missed.
- **`read` resolves with text, not a `Value`.** A page's `Number` cells are JSON numbers with fractions, which
  a `Value` cannot hold, so the caller decodes the text against the table's description
  (`table::readGenericPage`) and every cell keeps its exact value.
- `read` does not route its reply through the cache's effects: a read has none, and the caller's own change
  events reach it like anyone's.
- On reconnect and on `resync` the session calls each holder's handler with a whole-type change.

**Freshness.**

- `staleTime` decides whether moving onto an entry fetches. Such a fetch re-reads a page and is answered from the
  server's caches: it costs a page, never a load. A table with levels needs no `refreshEvery`, and one that
  has it still loads nothing twice. A `PageWindow` has no stale time.
- On reconnect and on `resync` the session sends its watch set again and marks every entry stale; a window is
  invalidated. Events missed while disconnected are covered by those fetches.
- Offline, a held page shows as it was, its unloaded cells unloaded. No arrival reaches it until the connection
  returns, when the reconnect rule applies.

**Column widths.** The Qt Quick table measures a column from its realized cells, and in a windowed table a
column's width only grows. An unloaded cell draws a placeholder of a fixed width, so a level arriving widens each
of its columns at most once, to its widest realized value, and never narrows one.

### Which client cache a screen uses

A `rows` table bound to a query's page reads through the query cache. With the rules above it is correct and
push-driven: an arrival marks the entries concerned, each fetches its page again, and the mount's keyed rows
repaint only the rows whose content changed. What it pays is the transfer: a page of 200 rows and 300 columns
for an arrival of twenty rows ([Performance](#performance), W7 against W4).

A model-backed `table` reads through a `PageWindow` ([The client table model](#the-client-table-model)): the
node owns a window, feeds the mount's visible range to `visible`, sends the window's fetches through the
session, and gives the mount the window's rows. An arrival there transfers the rows that arrived.

### A persistent tier

Loaded values do not survive a server restart: the cache starts empty and loads on demand. A tier that keeps them
needs a durable generation per row, written with the data it describes, or a restart serves values an action
invalidated while the server was down. That is a storage design, not a cache option, and is not part of this one.
`LevelCache` keeps its storage behind its own members so that a tier can be put under it.

## Where sort and filter run

morph has two modes. In client mode an `Engine` sorts and filters a `DataSource` the client holds entirely. In
server mode a table's action takes a `TableQuery`, the server sorts, filters and pages, and the client holds
pages. A table over rows the document holds, and a collection, are in client mode; a model's table is in server
mode.

A browser client is the Qt WebAssembly build, which is single-threaded. That decides three things:

- **No worker.** An `Engine` there runs its jobs as steps on the owner thread, the one that paints. A step's
  size is set for the browser, not for native code: the size that takes 4 ms natively is divided by the factor
  B2 measures, so a step keeps to half a frame where the code is slower. The work is not done in parallel with
  painting, and a recompute's diff and a `Deferred` repair's layout are one step each, 10 to 55 ms natively at
  100,000 rows ([`engine.md`, "Not here yet"](engine.md#not-here-yet)). Client mode in a browser is therefore
  held to the bound of rule 4 below, 20,000 rows, and B1 holds the worst step at that size.
- **A 32-bit address space.** The heap cannot exceed 4 GiB and is usually configured lower.
- **Slower code.** WebAssembly runs this kind of code slower than native; the factor is measured by benchmark B2
  and taken as 1.5 to 3 until it is.

**What the table weighs.** A `Cell` is as large as its largest alternative, two `Rational`s, about 56 bytes. A
`VectorSource` of 100,000 rows and 300 columns is 30 million cells: about 1.7 GB before any text. A page of rows
costs about 21 bytes a cell on the wire (212 bytes for a row of ten small cells), so the whole table is about
630 MB of JSON: over a minute at 100 Mbit/s before it is parsed. Holding the table in a browser is not slow, it
is impossible; and a table with a slow level is not held by the server in full either. A window of 1,600 rows is
480,000 cells and about 10 MB of JSON if every column is sent, and 64,000 cells and about 1.3 MB for the
forty columns in or near view, spread over eight replies.

**The rule.** Where sort and filter run follows from what the client holds, and what the client holds follows
from the action's shape:

| The source | Mode | Sort, filter, totals, distinct values, the undecided set | Selecting all |
|---|---|---|---|
| A model's table: its action takes a `TableQuery` (the schema carries `x-table`) | Server | On the server, once for each view and state of its data | A selection of the view, which an action resolves on the server ([Acting on rows](#acting-on-rows)) |
| Rows the client holds entirely: a document's `rows`, a collection's reply | Client | An `Engine` on the client | `Selection::selectAll` |

A table is in server mode, and must be, when any of these holds:

1. the server pages it, because the rows are too many to send;
2. a column that can be sorted or filtered has a level other than the base one: a client cannot order by values
   it does not hold, and the level cache is on the server;
3. the rows a principal may see are decided per request; the server filters those rows in either mode, and client
   mode is then possible only over the scoped result;
4. it has more than 20,000 rows or more than 1,000,000 cells. Under that, the whole table is at most about 21 MB
   of JSON and 56 MB of cells, which a browser holds, and a three-key sort with a filter over 10,000 rows takes a
   few milliseconds on one thread.

The action's shape decides, not the application at run time and not the framework by measuring the data. A table
that changes mode when it grows past a threshold changes its text order (a server orders by its own collator and
a client does not re-sort a page), its selection behaviour and its totals on the day it grows. The author chooses
once by writing a paged action or a plain list. The framework checks the choice: a document table over a plain
list whose result exceeds the bound in 4 is reported as a warning naming the table, as the document's other
warnings are, and a client-mode sort or filter on a column with a level is refused.

**What the user waits for** in server mode, at 100,000 rows and 300 columns, in a browser. The server figures are
measured natively on the engine; the rest are estimates until the named benchmarks run.

| Action | Path | Time |
|---|---|---|
| Click a header to sort | request, `apply` (sort one key: 33 to 46 ms measured), encode a page, reply, decode, repaint | about 150 ms on a 50 ms round trip (B3) |
| Type in a text filter | debounce (250 ms), then request, `apply` (text filter: 9 ms measured), reply, decode, repaint | about 350 ms after the last key (B3) |
| Scroll into pages not held | request, a read of the cached view, reply | about 110 ms a page; the held margin of one page each side hides it at reading speed |

A text filter is debounced through the query's `debounce`, so a word typed quickly is one request, not one per
key. The table keeps showing its rows, with the header marking the sort in flight, until the reply replaces them.

**Hybrids.** Two were considered and one is kept for later.

- *Sorting or filtering the held rows locally while the reply is in flight* is rejected. The window holds a slice
  of the old order. Its local re-sort is not a slice of the new order, so the rows move once to a wrong place and
  again to the right one, and a local filter shows a subset of 1,600 rows as if it were the subset of 100,000.
- *Local work when the window holds the whole view* (an `Exact` total no larger than the rows held, nothing
  awaited) is sound: a filter that leaves 300 rows could be re-sorted with no request. It needs an `Engine` on the
  client with the server's collator and a rule for handing back to the server when the view grows. It is not
  specified here; the 150 ms above is the cost of leaving it out.

## Configuring a table

A user arranges a table: the order, visibility, pinning and widths of its columns, a chain of sort keys, and
filters set from each column's header. All of it is state of the client's table, in three values, each of which
a document may bind (then it is controlled, as `selection` is) or leave to the table:

| Value | Shape | Held by |
|---|---|---|
| `layout` | `{"order": [id…], "hidden": [id…], "pinned": n, "widths": {id: units}}`, and where used `"groups"` ([Column groups](#column-groups)) and `"units"` ([Display](#display)) | `table::ColumnLayout` |
| `sort` | `[{"column", "direction"}…]`, most significant first | `TableRows::setSort` |
| `filters` | A `FilterSpec`: column filters and group filters | `TableRows::setFilter` |

It works the same in client and server mode: layout reaches the server only as the columns a query names; sort
and filters go where the mode puts them.

### Columns: order, visibility, pinning, width

- **Reorder.** Dragging a header moves its column. With the header focused, `Ctrl+Shift+Left` and
  `Ctrl+Shift+Right` move it, and the header's menu has "Move left" and "Move right". `Alt` with an arrow is
  not used: in a browser it is Back and Forward. The selection column and a group heading are not columns and
  do not move.
- **Hide and show.** The header's menu has "Hide column"; the table's menu opens a column chooser, a list of every
  column with a check box, in order, with the same move actions as "Move up" and "Move down", which stop where
  "Move left" and "Move right" do. At least one column stays visible.
- **Pin.** The first `pinned` columns of the layout do not scroll sideways: they stay at the leading edge, and
  are always in the column window. The header's menu has "Pin column", which moves the column to the end of
  the pinned ones, and "Unpin". The selection column is always pinned and is not counted. Pinned columns may
  take at most half the table's width; while the table is narrower than that allows, the last pinned columns
  scroll with the rest, and the stored count is not changed. At 300 columns this is what keeps a row's name
  beside whichever of its values the user has scrolled to.
- **Resize.** Dragging a header's trailing edge sets the column's width in units; a width the user set stays as
  set, whatever the cells measure. A stored width is the content's, as a declared fixed width is: each backend adds
  its own padding around it (`TableMeasure::padding`: Qt Quick's cell padding, the terminal's gap), so a stored
  layout means the same on every backend. Double-clicking the edge removes it, and the column takes its declared
  width again.
- **Reset.** "Reset columns" restores the declared order, visibility, pinning and widths.

`ColumnLayout` holds the order and the hidden set, with `setWidth(id, std::optional<int>)`, `widths()`,
`setPinned(n)`, `pinned()`, and `value()` and `apply(value)` for the stored shape.

**A layout survives a changed table.** Ids are the columns' string ids. One rule fits a layout to a table's
columns, whether the layout is a stored one (`apply`) or the table's own when its description takes a new
revision (`rebuild`):

- an id the table does not have is dropped, from `order`, `hidden` and `widths` alike;
- a column the layout does not mention takes its declared visibility, and is placed after the nearest column
  before it in the declared order that the layout does place, or first when there is none, so a new column
  appears where its author put it, as its author declared it, and not at the end;
- `pinned` is clamped to the visible columns;
- if that leaves no column visible, the first declared column is shown.

**Persistence.** The layout is part of the table's persisted state ([Persisted state](#persisted-state)).

**Document vocabulary.** `layout` (the value; bound, it is controlled), `onLayoutChange` (after a user's change,
with `event` the new layout), `configurable` (a literal: `true`, the default, `false`, or a list of `reorder`,
`hide`, `pin`, `resize`), and on a column `locked` (a literal flag: the user cannot move, hide, pin, unpin or
resize it). `locked` is not called `fixed`: `fixed(n)` is a column's width sizing. Element values: `layout`,
`visibleColumns` (ids in order) and `hiddenCount`. Every key is optional.

**With levels.** A hidden column that is neither sorted nor filtered is not in the column window, so no query
names it and its level is not loaded for it. Showing it fetches that column for the held pages.

**With the column window.** Hiding a column returns its realized cells to their pool and creates none; moving
one creates none unless it moves into the window.

### Sort chains

The engine, `TableQuery` and the document's `sort` take a list of keys; what a user needs is a way to build one.

- A plain click on a sortable header sorts by that column alone, ascending, or descending when it already is
  the chain's first key, ascending.
- `Shift`+click adds the column as the last key, ascending; on a column already in the chain it flips that key's
  direction and keeps its place. `Shift+Enter` and `Shift+Space` on a focused header do the same.
- The header's menu has "Sort ascending", "Sort descending", "Add to sort" and "Remove from sort", so the chain
  can be built without a modifier key, on a touch screen included.
- The table's menu opens a sort dialog: the keys in order, each with its column, its direction, move up, move
  down and remove, and "Add key".
- Every header in the chain shows its direction; with more than one key each also shows its place in the chain,
  from 1. A header's accessible description says "sorted ascending, key 2 of 3".
- A chain has at most `maxSortKeys` keys: 8 by default, and up to 32 where a model's table's `limits` raise it,
  which the description carries as `TableCapabilities::maxSortKeys` so the client knows the bound; a table over
  `rows` takes it as the literal `maxSortKeys` on the node. A key costs one comparison only between rows that tie on every key before it, so a long
  chain costs little more than a short one; the default is what a header shows legibly. At the bound, adding a
  key is refused with a message in the status line, not by dropping the first key.

`sort` and `onSortChange` keep their shapes. `Table::sort` carries the whole chain to the backend.

### Filters from the header

A filterable column's header has a filter button, drawn filled while the column has a filter and named so for
assistive technology ("Name, filtered"). It opens a menu with three parts.

1. **Values.** A search field over a list of the column's distinct values, each with a check box and the number
   of rows that have it, under "(Select all)" and, when the column has empty cells, "(Blanks)". Typing narrows the
   list; "(Select all)" then means the values the search shows.
2. **Conditions**, by the column's kind, each one `FilterEntry`:

   | Kind | Conditions |
   |---|---|
   | `Text` | equals, does not equal, contains, begins with, ends with; match case |
   | `Integer`, `Key`, `Decimal`, `Quantity`, `Number` | =, ≠, <, ≤, >, ≥, between |
   | `Date`, `DateTime` | the same, with a date picker; and the values list as a tree of years, months and days |
   | `Bool` | the values list alone |
   | `Custom` | is empty, is not empty |

   Two conditions may be joined by "and" or "or" (`combine`).
3. **Clear filter.** The table's menu has "Clear all filters".

A column is filterable when it has an `id` and the document does not say `filterable: false`. `filters` is served
on every table, bound or held by the table, with `onFiltersChange`.

**The value-set operator.** `FilterEntry::oneOf` is a list of operand texts:

```cpp
struct FilterEntry {
    // … the existing operators …
    /// Matches a cell whose value equals any of these. Each is parsed for the column's kind, as `eq`'s operand is.
    std::optional<std::vector<std::string>> oneOf;
};
```

- It matches as `eq` does, against any member: text by fold unless `caseSensitive`, numbers exactly. It never
  matches an empty or invalid cell, and is `Unknown` on an unloaded one.
- Ticked values are one `oneOf` entry in `include`; "(Blanks)" ticked adds an `isEmpty` entry beside it, which
  `combine: any` joins. Unticking a few values of many is the same entry in `exclude`. A menu sends whichever side
  is smaller, so unticking one value of 50,000 sends one value; when the two sides are equal it sends the
  unticked side, an exclude, so unticking one of two values sends one excluded value.
- `compileFilter` parses the members once into a set; a row is matched by one lookup, whatever the set's size.
- `QueryLimits::maxSetValues` (10,000) bounds one entry, `maxQuerySetValues` (20,000) the members of every set
  of one query together, and `maxQuerySetBytes` (1 MiB) their text; over any of them is `LimitExceeded`. The
  entry bound alone would allow a query of millions of values across its columns. When both the ticked and the
  unticked side exceed it, the menu disables the list and says so: the user narrows with a condition.

**Distinct values.**

```cpp
struct DistinctRequest {
    std::string column;
    /// In place of `column`: a declared group's id, for values pooled over its columns.
    std::string group;
    /// Narrows to values whose text contains this, folded; empty for all.
    std::string search;
    /// For `Date` and `DateTime`: `Value`, or the bucket to count by.
    DateBucket bucket = DateBucket::Value;      // Value, Year, Month, Day
    std::int64_t offset = 0;
    std::int64_t limit = 200;
};

struct DistinctValue {
    std::string text;      // as the cell shows it
    std::string operand;   // as a filter entry takes it
    std::int64_t count = 0;
};

struct DistinctValues {
    std::vector<DistinctValue> values;        // in the column's ascending order
    std::int64_t distinct = 0;                // values matching `search`, before offset and limit
    TotalKind distinctKind = TotalKind::Exact;
    std::int64_t blanks = 0;                  // rows whose cell is empty
    std::int64_t invalid = 0;                 // rows whose cell is invalid; they are under no value
    bool partial = false;                     // rows left out because a cell was unloaded
};

[[nodiscard]] std::expected<DistinctValues, TableError>
distinctValues(DataSource const& source, TableQuery const& query, DistinctRequest const& request,
               ApplyOptions const& options = {});
```

- **Which rows.** The rows that pass `query.filters` with the named column's own entry in `filters.columns`
  removed. So each column's list shows what the other columns' filters leave, and a value the column's own filter
  unticked stays in the list to be ticked again. Groups stay applied as they are.
- **Which values.** Valid cells, grouped by the key the column sorts and filters by: text that folds alike is one
  value, shown as its first row's text. Empty cells are counted in `blanks`. Invalid cells are not values and
  are counted in `invalid`; the menu says under the list how many cells could not be read, and offers nothing
  to tick for them.
- **Unloaded.** A row whose cell in the column is unloaded, and a row another column's filter leaves undecided, is
  in no count, and sets `partial`. The menu then says the list may grow.
- **Order and bound.** Values are in the column's ascending order. `limit` is clamped to `QueryLimits::maxLimit`;
  `distinct` is counted up to `QueryLimits::totalCap` and is `AtLeast` at the cap. A column of 100,000 distinct
  values is paged and searched, never sent whole.
- **Dates.** With a bucket, a value is a year, a month or a day, its `operand` the two ends of a `between`; the
  menu draws the tree by asking for years, then the months of an opened year, then its days. The tree's filter
  is include-only: it opens with every box unticked and no "(Select all)" (or with the buckets a stored filter
  covers ticked), ticking a year or a month covers the buckets under it, and Apply sends one `between` per
  ticked bucket under `combine: any`, with `isEmpty` beside them when "(Blanks)" is ticked; with nothing ticked
  it clears the column's values filter. Buckets that touch (the next begins the day, or the second, after the
  previous ends) are merged into one `between` first. A selection still over `QueryLimits::maxEntries` (32)
  entries applies nothing, and the menu says "Too many dates to list; filter with a condition".
- **Levels.** The column's level is an `order` level of the request: it is needed for every candidate row.
- **Client mode.** `Engine::distinctValues(request)` answers from the key columns the engine holds.
- **Server mode.** `TableQuery::distinct` carries the request through the table's own action, so the model's
  scope applies; the reply is a `Page` with no rows and `Page::distinct` set.
- **Validation.** `validate` holds a distinct request to the query's rules. Exactly one of `column` and `group`
  is set. The column, or every column of the group, exists and the caller may read it (`UnknownColumn`,
  `ColumnNotReadable`): a values list is a read of the column. `search` is at most `maxTextLength` bytes and
  `offset` at most `maxOffset`. A query that sets both `keys` and `distinct` is `InvalidSpec`. In the filter, a
  group's `exclude` entries count toward `maxEntries` with its `include` and are bounded by `maxTextLength` as
  they are, and so is every member of a `oneOf`.
- **Caching.** A distinct reply is kept by the table's rows (`WindowRows`), keyed by the query's body, so reopening a
  menu costs nothing; each table keeps its own, and they are not shared between tables. At most 64 replies are kept,
  the oldest going first. `Page` declares `distinct` as an open member beside `awaiting`: any change of the table's
  record drops them, because any row may change a count, and so do a reconnect and a new revision; a reply asked for
  before such a change answers its menu and is not kept.

Top-N filters are not included: "the ten largest" is a property of the view's order, not of a cell, and belongs
with a ranking function over the view.

### Column groups

Several columns can hold the same kind of thing: `companyA`, `companyB` and `companyC` each hold a company. A
**column group** names them, so one filter asks about all of them: "a company is F", "F and S are both there",
"at least two of them are F or S". The columns stay ordinary columns, shown, hidden, sorted and filtered one by
one as well.

**Declaring a group.** A typed row declares its groups once, beside its levels:

```cpp
struct ColumnGroup { std::string_view id; std::span<std::string_view const> columns; };

struct DesignRow {
    static constexpr auto columnGroups() noexcept;   // {"company", {"companyA", "companyB", "companyC"}}
};
```

`DataSource::groups()` returns them (empty by default); the row's schema carries `x-column-group` on each member
property, and `deriveColumns` copies it. A document table over `rows` declares `groups` as parts
`{id, label, columns}`. A group has 2 to `QueryLimits::maxGroupColumns` (16) columns of one kind, and a column
is in at most one declared group; a declaration that breaks either is refused where the columns are read.

A user may also form a group: the column chooser has "Group columns…", which offers columns of one kind and asks
for a name. Such a group is part of the layout (`"groups": [{"id", "label", "columns"}]` in its stored shape) and
is fitted to the table as the layout is: a column the table lacks leaves the group, a column stays only in the
first group that holds it, and a group left with fewer than two or more than 16 columns, or repeating an earlier
group's id, is dropped, with any filter on it. The column chooser lists the groups a user formed, each with
"Remove group", which removes the group and any filter on it.

**Drawing a group.** A group draws as a one-level heading on its own line above the column headers and below any
header bands ([`editing.md`](editing.md#header-bands)), its label over its columns and its filter button at its
end, while its columns stand together in the layout. It is not a band: a group whose columns the user moved apart
is not split into runs as a band is.

**The filter.** `GroupFilter` is the engine's filter over several columns, with a name, a quantifier, a mode
and excludes:

```cpp
enum class GroupMode : std::uint8_t {
    Columns,    // count the columns whose cell satisfies the entries
    Together,   // every entry is found, each in a column of its own
};

struct ColumnCount {
    enum class Kind : std::uint8_t { Any, All, AtLeast, Exactly } kind = Kind::Any;
    std::int64_t n = 1;     // for AtLeast and Exactly
};

struct GroupFilter {
    std::string group;                  // a declared group's id; or
    std::vector<std::string> columns;   // the columns themselves
    GroupMode mode = GroupMode::Columns;
    ColumnCount match;                  // Columns mode only
    Combine combine = Combine::Any;     // Columns mode: how one column's cell must satisfy `include`
    std::vector<FilterEntry> include;
    std::vector<FilterEntry> exclude;
};
```

In JSON a group is `{"group" | "columns", "mode", "match", "combine", "include", "exclude"}`, with `match` one of
`"any"`, `"all"`, `{"atLeast": n}` and `{"exactly": n}`. A filter that sets only `columns`, `combine` and
`include` is the plain group: the entries match on any of the columns.

Write k for the number of columns and say a column *satisfies* the entries when its cell matches any of them
(`combine: any`) or every one of them (`combine: all`), by the rules of a column filter: an empty cell matches
nothing but `isEmpty`, and an invalid cell nothing but `notEmpty`.

- **`Columns` mode.** Count the columns that satisfy the entries. The row passes when the count is at least 1
  (`any`), k (`all`), at least n, or exactly n.
- **`Together` mode.** The row passes when the entries can be given a column each, no column twice, such that
  each entry matches its column's cell. `match` and `combine` are not used. It takes at least 2 include entries
  and at most as many as the group has columns, so at most 16.
  A group with no include entry is an exclude-only group in either mode: only its `exclude` decides.
- **`exclude`.** The row is rejected when any column's cell matches any exclude entry: "none of the companies is
  X".
- Groups combine with each other and with column filters by AND, so "F somewhere and S somewhere, in the same
  column or not" is two `any` groups.

The cases, as filters on the group `company`:

| Wanted | Filter |
|---|---|
| At least one of the columns is F | `{"group": "company", "include": [{"eq": "F"}]}` |
| F and S are both there, in two of the columns | `{"group": "company", "mode": "together", "include": [{"eq": "F"}, {"eq": "S"}]}` |
| At least one of F, S, T in any of the columns | `{"group": "company", "include": [{"oneOf": ["F", "S", "T"]}]}` |
| At least two of the columns hold F or S | `{"group": "company", "match": {"atLeast": 2}, "include": [{"oneOf": ["F", "S"]}]}` |

The second and the fourth differ, which is why both exist:

| `companyA`, `companyB`, `companyC` | any is F | F and S together | at least 2 are F or S | exactly 1 is F |
|---|---|---|---|---|
| F, S, X | yes | yes | yes | yes |
| F, F, X | yes | **no** | **yes** | no |
| S, F, F | yes | yes | yes | no |
| F, X, Y | yes | no | no | yes |
| X, Y, Z | no | no | no | no |

Entries are any `FilterEntry`, not only values: "two of the three dates are after 1 May" is `atLeast: 2` with a
`gt` entry.

**Deciding `Together`.** It is a matching between entries and columns, found in C++ for each row; no store is
asked to express it. The engine reads each of the row's k cells once and gets, per cell, the set of entries it
matches (for value entries one lookup of the cell in a map from value to entries; a condition is tested per
entry), then looks for a matching that covers every entry by augmenting paths over those bit sets. With at most
16 entries and 16 columns one row costs at most 16 augmentations of at most 16 steps, and a row whose cells
match no entry is rejected after the k reads.

**Unloaded cells.** An entry on an unloaded cell is `Unknown`, so a column may satisfy the entries, not satisfy
them, or be unknown. Write y for the columns that satisfy and q for the unknown ones.

| Quantifier | Yes when | No when | Otherwise |
|---|---|---|---|
| `any` | y ≥ 1 | y + q = 0 | `Unknown` |
| `all` | y = k | y + q < k | `Unknown` |
| `atLeast n` | y ≥ n | y + q < n | `Unknown` |
| `exactly n` | y = n and q = 0 | y > n, or y + q < n | `Unknown` |
| `together` | a matching covers every entry using known matches only | none does even counting every unknown cell as matching every entry | `Unknown` |
| `exclude` | (rejects) a known cell matches | no cell matches and none is unknown | `Unknown` |

So `atLeast 2` over three columns is `Yes` with two known matches and the third unloaded, `No` with no match and
one unloaded, and `Unknown` with one match and one unloaded. A row is decided as soon as its known cells decide it,
and invariant 1 holds for groups as for columns.

**The menu.** A group has a filter entry of its own: in the table's menu, in each member column's header menu
("Filter Company, 3 columns…"), and, while its columns stand side by side in the layout, as a header over them
with its own filter button. Its menu is a column's, with two additions:

- the values list is pooled: `DistinctRequest::group` names the group, a value's count is the rows that have it
  in at least one of the group's columns, "(Blanks)" is the rows with an empty cell in at least one of the
  group's columns, which are the rows ticking it admits (an `isEmpty` entry under `any`), and the rows
  considered are those passing every other filter, this group's own removed. A row with an unloaded cell
  in the group contributes its known cells and sets `partial`;
- a line under the list chooses how the ticked values must be found: "in any column" (the default), "in every
  column", "in at least N columns", "in exactly N columns", or "all of them, each in a column". The first four
  are one `oneOf` entry with a `match`; the last is one `eq` entry per ticked value in `together` mode, and is
  offered while at least 2 values are ticked and no more than the group has columns.

**On a server.** `apply` evaluates every form above.

### The status line

A table has one line under its rows where it says what the rows alone cannot. It is part of the table, drawn
by each backend from `Table::status`, and it is where every refusal this document mentions is shown.

| Part | Shows | Examples |
|---|---|---|
| Counts, at the leading edge | The view's size and what is still coming, from `TableRows::counts` | "9,093 rows"; "100,000 or more"; "12 so far, 40 loading"; "3 of 9,093 selected" |
| State | A level that failed, with "Retry"; a fetch that failed, with its error and "Retry" | "Recipe data could not be loaded. Retry" |
| Message, at the trailing edge | The latest refusal or notice, until the user's next gesture on the table or for six seconds | "A table sorts by at most 8 columns" (the table's bound); "Copied 200 rows, 12 cells were not loaded" |

- "Retry" sends the table's query again with `TableQuery::retry` set, which makes the level cache load the
  failed keys the reply needs at once ([The server level cache](#the-server-level-cache), rule 11).
- A fetch that failed offers "Retry" too: the refetch carries the same flag, so a level that failed with it is
  tried again in the same request.
- The line is a polite live region for assistive technology: a message is read out once, a count when it
  settles.
- **When it shows.** A model-backed table has it always: its counts are not visible any other way. A table
  over `rows` shows it while it has a state or a message, and otherwise takes no space. A document sets
  `status: true` to show it always or `status: false` to never show it; with `false` a message goes to the
  session's notifications instead, so a refusal is never silent.
- The counts are also element values (`viewCount`, `viewCountKind`, `undecided`, `selectedCount`, `awaiting`,
  `failed`), so a document that hides the line can say the same things in its own words.

### Copy

`Ctrl+C`, and "Copy" in the table's menu, put the selection on the clipboard as text.

- **What.** The selected rows, in view order, by the visible columns, in layout order: one line a row, cells
  separated by tabs, each cell its display text as the column formats it, in the display unit the user chose.
  A tab or a line break inside a cell's text is written as a space. A column's own `cell` template is not
  copied; its value's text is. With no row selected, the focused cell's text is copied. "Copy with headers"
  puts the columns' labels on a first line.
- **Rows and columns the client does not hold.** A server-mode table fetches them: keyed queries naming the
  visible columns, `maxKeys` rows a request, for at most `SessionOptions::maxCopyRows` (10,000) rows. Over
  that the copy is refused with a message in the status line.
- **Unloaded cells** are copied as empty, and the message says how many there were. Copying does not wait for
  a level.
- **What the status line says.** "Copied {n} rows" ("Copied 1 row"), with ", {m} cells were not loaded" ("1 cell
  was not loaded") when there were any. A fetch that failed says "The rows could not be copied: {reason}", and a
  backend with no clipboard "The rows could not be copied: there is no clipboard"; neither changes the clipboard.
  A copy whose rows have all left the view changes nothing and says nothing, and a copy started before another
  whose rows arrive later writes nothing.
- The text reaches the clipboard through `IViewBackend::copyText`.

Writing a view to a file (CSV, a spreadsheet) is not part of the table: it is an action of the application
over the same query, which runs on the server and is not bound by what a client holds.

### Persisted state

A table's arrangement outlives the screen: its column order, hidden and pinned columns, widths and display
units, its sort chain, its filters, the groups a user formed, and its saved views. Every table stores and loads
it through one interface the application injects. Selection and scroll position are not arrangement and are
not stored.

```cpp
/// Which table's state. No member names a person: a store keeps one person's state apart from another's itself.
struct TableStateKey {
    std::string application;
    std::string model;       // the model's type; empty for a table over a document's rows
    std::string table;       // the table's id; for a `rows` table, the node's id
    std::string qualifier;   // the screen's id, or what the table's `persist` names
    std::string part;        // "state", "views" or "shared"
};

struct StoredTableState {
    std::string json;
    std::string revision;    // names this stored document; opaque
};

enum class SaveOutcome : std::uint8_t { Saved, Conflict, Refused };
struct SaveResult {
    SaveOutcome outcome = SaveOutcome::Saved;
    std::string revision;    // of what is stored now
    std::string reason;      // for Refused
};

/// An `expected` revision that stores only when nothing is stored under the key, and answers `Conflict`
/// otherwise.
inline constexpr std::string_view kNoDocument = "-";

class ITableStateStore {
public:
    virtual ~ITableStateStore() = default;
    /// The stored document, or nothing when none is stored.
    virtual async::Completion<std::optional<StoredTableState>> load(TableStateKey const& key) = 0;
    /// Stores `json`. With `expected` empty it replaces whatever is stored; with `kNoDocument` it stores only when
    /// nothing is stored; otherwise it stores only when the stored document has that revision. It answers
    /// `Conflict` with the current revision when the condition does not hold.
    virtual async::Completion<SaveResult> save(TableStateKey const& key, std::string json,
                                               std::string expected = {}) = 0;
    virtual async::Completion<bool> remove(TableStateKey const& key) = 0;
    /// Whether the store keeps the `shared` part for everyone rather than for one person.
    [[nodiscard]] virtual bool shares() const { return false; }
    /// Applies one operation (add, replace, rename or remove one view; see "Saved views") to the `shared` part,
    /// under the `expected` rule of `save`. A store that does not share answers `Refused`, "this store does not
    /// share views", which `refusePublish` gives it on the store's own executor. Pure: the interface owns no
    /// executor to deliver a default answer on.
    virtual async::Completion<SaveResult> publish(TableStateKey const& key, ViewOperation const& operation,
                                                  std::string expected) = 0;
protected:
    static async::Completion<SaveResult> refusePublish(exec::IExecutor& executor);
};
```

`kNoDocument` lets a save say "I loaded nothing": a table whose `views` part was empty when it loaded saves its
first view with it, so two windows that both start from no views cannot overwrite each other's first view. An
empty `expected` cannot say that, because it means "replace whatever is stored".

- **One place.** The store is `SessionOptions::tableState`, a `std::shared_ptr<ITableStateStore>`. The session is
  what every table surface reaches: a document table over rows, a model-backed table, a collection, in client
  and in server mode. A table mounted from C++ with no session is given the store through its `TableRows`. With
  no store, a table keeps its state for as long as it is mounted.
- **Asynchronous.** Every call returns a completion; nothing waits on the thread that paints. A store delivers
  its completions on an executor it owns, as a preferences store does.
- **The person is the store's.** A key names no person, because a client must not be able to name someone
  else's. The framework's default store derives the person on the server from the verified session; an
  application's own store scopes by whoever it knows is signed in.
- **What a key distinguishes.** Tables of two instances of one model type share a key and so an arrangement:
  the state belongs to the kind of table on a screen, not to the data it shows. A table over `rows` needs an
  `id` on its node to have a key; without one it persists nothing, and a `persist` that says otherwise is
  refused when the document loads.
- **No size limit.** A state is as large as the arrangement it holds: a filter that ticks 20,000 values is
  stored whole, and a table keeps as many views as the user saves. Neither the interface nor the framework
  bounds a document, and a store may not truncate one. An operator who wants a ceiling sets a quota on the
  server ([The framework's stores](#the-frameworks-stores)); a save over a quota is `Refused`, and the table
  says so in its status line and keeps its arrangement.

**What is stored.** JSON documents per table, each with a version: the arrangement and the saved views below, and the shared views, of the views' shape ([Saved views](#saved-views)):

```json
{ "version": 1,
  "layout":  { "order": ["id", "name"], "hidden": ["notes"], "pinned": 1, "widths": {"name": 24},
               "groups": [{"id": "g1", "label": "Company", "columns": ["companyA", "companyB"]}],
               "units": {"weight": {"mass": "g"}} },
  "sort":    [{"column": "name", "direction": "asc"}],
  "filters": { "columns": {}, "groups": [] },
  "view":    "Open orders" }
```

```json
{ "version": 1, "default": "Open orders", "defaultOnMount": false,
  "views": [{ "name": "Open orders", "layout": {}, "sort": [], "filters": {} }] }
```

- `part: "state"` is the arrangement in use; `part: "views"` is the saved views; `part: "shared"` is the views
  published for everyone ([Saved views](#saved-views)).
- `view` names the saved view the arrangement was taken from, or is absent. The menu shows the name, and shows
  it as modified when the arrangement differs from that view; nothing more is stored for it.
- **Versions.** A document of an older version is brought forward by the framework, one version at a time, when
  it is read. One of a version the framework does not know is not used and is reported; the table starts from
  its defaults, and writes its own version when the user next changes something.

**Fitting stored state to the table.** State is fitted to the description the caller receives, never the other
way round, and a table is never refused over its state:

- columns are fitted as a layout is ([Columns](#columns-order-visibility-pinning-width));
- a sort key on a column the table lacks, or that is not sortable, is dropped;
- a filter on a column or group the table lacks, one that does not compile for the column's kind, and a unit
  the description does not list are dropped, each reported once at the table's path;
- a user-formed group that is left with fewer than two columns is dropped with its filter;
- what was dropped because the caller's description lacks the column or group is **carried**: it is kept
  beside the fitted state and written back as it was, a carried column keeping its place after the column it
  followed. A table whose columns depend on the session therefore keeps a user's place for a column that is
  absent now and returns later. What was dropped for any other reason is gone at the next save.

**When it is loaded.** A table loads its state before it shows rows. Its `TableRows` starts in a restoring
state: a server-mode table sends no query, and a client-mode table draws placeholder rows. When the load
completes it applies the fitted state and then makes its first query, so the first rows are in the stored order
and under the stored filters, with no rows of the default order shown first and no second fetch.

- The session keeps every state it has loaded or saved, by key, for its lifetime. A table mounted a second
  time is restored from it at once, with no request; the first mount of a screen is the only one that waits.
- A load that has not completed after `SessionOptions::tableStateTimeout` (300 ms) is given up on for the first
  query: the table starts from its defaults. If the state arrives later it is applied, at the cost of one more
  query, unless the user has touched the table meanwhile (scrolled it, selected in it or changed its
  arrangement), in which case it is kept for the next mount and not applied under the user's hands.
- A load that fails is reported once through the session and the table starts from its defaults. Its changes are
  still saved.

**When it is saved.** A change marks the state unsaved; a save goes out when the arrangement has not changed for
`SessionOptions::tableStateDebounce` (500 ms), and at once when the table unmounts. One save per key is in
flight; a change made during it is one more save after it. A failed or refused save is reported in the status
line and the table keeps the arrangement. A client that is closed loses at most the changes of the last
debounce.

**Two windows, two devices.** The two parts differ.

- `state` is saved with no expected revision: the last save wins, whole document. Nothing is merged: an
  arrangement is a choice, and half of one window's filter joined to half of another's is neither's. Every
  window and device reaching one server has this guarantee in full. Two servers over one preferences store share
  no lock: two saves of one key reaching two servers in the same moment can interleave, and the worst outcome is
  a head naming a removed segment, which is a failed load until the next save, and segments no intent names,
  which stay in the store. A deployment of several servers that needs the guarantee across them keeps state in a
  store with a conditional write.
- `views` is a list, and losing a view another window saved is losing work. A change to it is one operation
  (add, replace, rename or remove one view, or set the default). The table saves with the revision it loaded,
  or with `kNoDocument` when it loaded nothing; on `Conflict` it loads again, applies its one operation to what
  is stored now, and saves once more, with the revision it read. When that reload finds nothing stored, the one
  retry saves with an empty `expected`: a store that does not honour `kNoDocument` answers `Conflict` over
  nothing, and one that does cannot, unless the document was removed in between.
- A mounted table does not follow saves made elsewhere; it reads its state when it mounts.

#### The framework's stores

| Store | Keeps state in | Use |
|---|---|---|
| `PreferencesTableStateStore` | The application's `IPreferencesStore`, through the server's `morph.tableState` model, the person derived on the server | The default. A host that has a preferences store gives every table persistence with no further wiring. |
| `MemoryTableStateStore` | A map | Tests, and a host that wants state for a session only. Built with `Sharing::Private` by default, it does not share and offers no publishing; built with `Sharing::Shared`, it keeps the `shared` part for everyone |
| A host's own | Anything: a settings file, a browser's local storage for a build with no server | The interface is all a host implements |

**`PreferencesTableStateStore` has its own path to the preferences store.** It does not go through the
preferences model, whose `maxValueBytes` (16 KiB) bounds a preference and would bound a table's state. The
client half calls a model of its own, `morph.tableState`, through the interpreter's `Dispatcher`, so it travels
the same bridge and session as the screen's models; the server half, `TableStateModel`, reads the person from
the verified session exactly as the preferences model does, refuses a caller with no verified person, and
writes to the same `IPreferencesStore` the application already has. Table state therefore has one home, the
application's preferences storage, and no second database.

How it keeps a document of any size in a store built for small values:

- **Where.** Entries of the caller's person and the key's `application`, under the scope
  `morph.table.<digest>`, the digest being of the key's `model`, `table` and `qualifier`. A digest keeps the
  scope inside a preference name's length whatever the ids are; the head entry repeats the key in clear for
  whoever reads the store.
- **Small documents.** A document of at most `TableStateOptions::segmentBytes` (12 KiB) is one entry at the
  point `<part>`: `{"revision": …, "document": …}`. Nearly every state is this.
- **Large documents** are written as segments. Each segment is an entry at the point
  `<part>.<revision>.<index>` whose value is a JSON string holding a slice of the document's text, so every
  entry is valid JSON, as a preferences store expects, and no longer than `segmentBytes`, which is inside the
  size such stores are built for. The head entry at `<part>` is `{"revision": …, "segments": n, "bytes": b}`.
- **Order of writes.** The segments of a new revision first, then the head, then the removal of the previous
  revision's segments. A reader takes the head and then the segments it names, so it reads either the old
  document or the new one and never a mixture. Reads take no lock: a segment missing because a save replaced the
  head meanwhile sends the reader back to the head stored now, so a load answers the new document and a `read`
  answers the new revision, which the client starts again from. A save that stops halfway leaves segments no head
  names; the next save of that key removes them. A segment missing under an unchanged head is a failed load. Every write of one key
  (a save, a removal, a publish) holds that key's lock on the server, shared by all its model instances, so two
  writes of one key run one after the other: the last wins whole, and no save removes segments another is still
  writing.
- **`expected`.** The model compares it with the head's revision before it writes, under the key's lock. Two
  servers over one preferences store share no lock, and the store has no conditional write, so two saves that
  reach two servers within the same moment can both pass the comparison and interleave their writes, with the
  outcome "Two windows, two devices" states. That window is the store's call time, not a user's think time; a
  host store with a conditional write closes it.
- **Transport.** A call is bounded by an envelope (8 MiB). `save` carries a document of up to 1 MiB; a larger
  one is sent as parts of 1 MiB under one upload id and committed when the last arrives. `load` returns the
  first 1 MiB and the document's revision and size, and the client asks for the rest by offset, starting again
  if the revision changes meanwhile. Loads made in one turn of the owner go out as one call naming all their
  keys (at most 256 a call), so a screen of five tables waits for one round trip. Each key is answered alone: a
  key out of bounds, or one whose segment is missing, fails its own load and no other. A reply stops adding heads
  once their text, as it travels escaped in the reply and again in the envelope, would pass 6 MiB, and answers
  the remaining keys as deferred; the client asks for those in its next call, so a screen of large states takes
  a few round trips and no reply outgrows its envelope. A server keeps at most four uploads in parts open per
  person: a save that would start a fifth is `Refused`, "too many table-state uploads in progress", and the
  client sends it again after a session's `tableStateTimeout`, until one of the four ends. It drops an upload
  whose next part has not come within
  `TableStateOptions::uploadExpiry` (30 s), and, with a quota set, refuses a part that takes its upload past it.
- **Bounds.** `application`, `table`, `qualifier` and `part` are 1 to 256 bytes each, and `part` holds no `.`,
  which would name another part's points; `model` is 0 to 256 bytes, empty for a table over a document's rows.
  `TableStateOptions::maxBytesPerPerson` is the operator's quota across one person's stored state, in every application, and across the shared views, which count as
  one more account; it is unset by default, and then nothing limits
  a state. The model checks it before it writes, against a usage total it keeps for the person. A preferences
  store has no conditional write, so two saves of one person's different keys in the same moment can both pass
  the check: each such race of two different keys can leave one document uncounted, so the quota is overshot by
  one document per race, and races add up; a ceiling tolerates that. A usage that has fallen
  below what a replace removes (a quota set over states stored before it, that race, a usage write the store
  refused) counts as nothing rather than going negative, so it never locks a person out.
- **Reserved names.** The preferences model writes wherever its caller names, so it refuses an application
  `morph.tableState` and a scope that starts with `morph.table`, on every call: without that rule a person could
  overwrite a state's head or the quota's usage entry through the preferences model. The same rule means a
  person cannot reset their own quota.

A document's `columns` and `saved` preference points that name a table hold no value of their own: their
editors read and write the table's state, a `saved` point its `view`, on a model-backed table and on a table over
`rows` alike. A point naming a table over `rows` that keeps no state (no store, `persist: false`) holds its choice as
a plain preference. A `saved` point without `of` names no table.

**Document vocabulary.** `persist` is a literal on a table: `true` (the default), `false` (nothing is loaded or
saved), or a string, which becomes the key's qualifier in place of the screen's id, so the same table on two
screens shares one arrangement, or two tables of one screen keep theirs apart.

**Bound values.** A `layout`, `sort` or `filters` the document binds is the document's: the stored value of that
part is neither applied nor saved, and the rest of the state persists as usual. A literal `sort` is the table's
default, and a stored one replaces it.

### Saved views

A saved view is a name with a `layout`, a `sort` and `filters`. A table's views are the `views` part of its
persisted state; the table's menu lists them, applies one, saves the current arrangement under a name, renames
and removes one, and marks one as the default. Applying one sets the three values, through their handlers where
they are bound, and records its name in the state's `view`. The view the `default` member names is applied
when the table mounts and no `state` is stored for it. A stored view is fitted to the table as its state is.

- **The default first.** With `"defaultOnMount": true` in the views document, the default view's sort and
  filters are applied at every mount, over those of the stored `state`; the stored layout still applies. It is
  for a user who wants each session to start from the same order, and the menu sets it as "Start from the
  default view".
- **Shared views.** A user may publish a view, under a name, for everyone who uses the table. Published views
  are a third part, `part: "shared"`, of the same shape as `views`, which the store keeps for the application
  and not for one person: a `TableStateKey` still names no person, and the store decides what the part means.
  The table's menu lists them under the user's own, applies one as it applies one of the user's, and copies one
  into the user's views with "Save as my view".
  - **Who may publish.** Reading the part is the `morph.tableState` model's `load`, open to everyone who may use
    the table. Writing it is a separate action of the model, `morph.tableState.publish`, carrying one operation
    (add, replace, rename or remove one shared view), so the application's authoriser admits or refuses
    publishing by action id, without reading a body. A user's own `save` or `remove` of the `shared` part is
    refused. The part is no one's, so it is an account of its own under the operator's quota: with
    `maxBytesPerPerson` set it is bounded as each person's state is, and a `publish` that would pass it is
    `Refused`, "over the table-state quota", as a save is; without a quota nothing limits it, as for everyone.
  - A shared view is changed under the `Conflict` rule, as a user's views are. The shared document has no
    `default` and no `defaultOnMount`; they are the user's choice, kept in the user's own views document.
  - **Which stores share.** `ITableStateStore::shares()` (default `false`) says whether a store keeps the
    `shared` part for everyone. A table offers publishing only over a store that shares, so a host's own store
    that does not never keeps a "shared" view for one person. `PreferencesTableStateStore` shares: it keeps the
    part under the application's own scope in the preferences store, with no person.

## Acting on rows

A table is where a user picks the rows an application's action works on, and where they see what it did: a
selection that is the whole view, the application's own entries in the table's menus, rows an action changed
that stay in view, a row whose key an action changed, and rows an action is working on or failed on.

### A selection of the view

On a model-backed table the client holds a window, not the view, so "every row I see" cannot be a list of keys.
It is a description of the view, which the server turns into keys when an action runs.

```cpp
/// What a user selected in one table. Its JSON is {"keys": […]}, or {"view": {…}, "except": […]}.
template <auto Function>
struct RowSelection {
    struct View {
        FilterSpec filters;              // the view's filter, as the table queried it
        std::vector<RowId> retained;     // the rows the view kept although they no longer match
        std::string revision;            // the view's `viewRevision` when the user selected it
    };
    std::vector<RowId> keys;             // without `view`: exactly these rows
    std::optional<View> view;            // with it: every row of that view, less `except`
    std::vector<RowId> except;
};

enum class SelectionPolicy : std::uint8_t {
    Exact,      // refuse when the view's rows may have changed since the user selected it
    Current,    // act on the view as it is when the action runs
};

struct SelectedRows {
    std::vector<RowId> keys;             // in the table's source order
    std::int64_t dropped = 0;            // keys the caller may no longer see, left out
};

/// Calls the table's function on the caller's thread, the model's strand, and resolves on the table executor;
/// a Task handler awaits it. The model is not const because a table function is not.
template <auto Function, typename Model>
[[nodiscard]] async::Completion<std::expected<SelectedRows, TableError>>
resolve(Model& model, RowSelection<Function> const& selection, TableContext const& context,
        SelectionPolicy policy);

/// The body of `Table.<id>.resolve`, for an action of another model or a task off the model's strand.
template <auto Function>
struct ResolveRequest {
    RowSelection<Function> selection;
    SelectionPolicy policy = SelectionPolicy::Exact;
};
```

`RowSelection` is the value an action receives; `table::Selection`, the set of selected keys
([`engine.md`](engine.md)), is what a client-mode table holds while the user selects.

- **Making one.** On a model-backed table `Ctrl+A`, the
  select-all box and "Select all" in the table's menu select the view: no row is fetched. `Ctrl`+click then
  toggles a row in `except`, and a `Shift` range adds or removes a range of `except` through `keysOf`. A plain
  click replaces the selection with that one key. A table held whole on the client selects all by keys, since it
  holds them.
- **Its count.** `selectedCount` is the view's total less the keys of `except` the window still sees in the
  view; a key of `except` that has left the view is dropped from it when the window learns so. The count is
  exact when the total is and no key of `except` is unaccounted for; the element value `selectedCountKind`
  (`exact` or `approximate`) says which, and the status line shows an approximate count with "about".
- **Its life.** A view selection is of one filter. A change of the filter, or of the retained rows by "Show only
  matching rows", ends it: the selection becomes empty. A change of the sort keeps it, since the rows are the
  same.
- **Not while membership is open.** A view whose filter waits for a level does not yet know which rows are in
  it, and "the rows I saw" says nothing about them. While `undecided` is above zero, or `awaiting` names a
  level a filtered column needs, select-all is disabled and the status line says why: "Select all is available
  once 40 rows are decided", or "… once recipe data has loaded" when no row is undecided yet. A level only the
  sort needs does not disable it, since order is not membership.
- **The view's revision.** Every page reply carries `viewRevision`, which names the view's membership: two
  replies with the same revision describe the same rows, and the sort is not part of it. It is a digest of the
  keys of the view's rows, taken in source order when the view is computed, a pass the computation makes
  anyway; the view cache keeps it with the entry, and the retained rows merged into a reply are added to it. An
  edit that leaves every row in or out of the view as it was, the session's own previous action included,
  leaves the revision as it was; an edit that brings a row in or takes one out changes it.
- **Resolving.** An action of the table's model takes a `table::RowSelection<&Model::designs>` member, and its
  Task handler ([`../core/coroutines.md`](../core/coroutines.md#task-handlers)) awaits
  `resolve(*this, selection, context, policy)`. `resolve` calls the table's function on that model for the
  caller, on the model's strand, so the caller's row scope and `mayRead` apply; then, on the table executor, it
  computes the view's membership: the filter's rows with an empty sort, which the view cache holds under that
  key when another request computed it, and the retained rows merged in. An action of another model, or a
  server task, sends `Table.<id>.resolve` with the selection and the policy instead, under its own principal. A
  key selection resolves to the keys the caller can still see, counting the others in `dropped`. A view of more than
  `QueryLimits::maxSelectedRows` (1,000,000) rows is `LimitExceeded`.
- **When the rows changed.** Under `Exact`, `resolve` refuses with `TableErrorCode::ViewChanged` when the
  revision differs. The client then takes the selection again from the view as it is now (its filter, its
  current revision and retained rows, the same `except`), says "The rows changed since you selected them: 9,120
  rows now. Run on these?", and sends the action again with the new selection when the user confirms.
- **The action's input says which table.** `RowSelection<fn>`'s schema carries `x-selectionOf` with the
  table's record, so the client knows which table an action works on
  ([Rows at work](#rows-at-work-rows-that-failed)).
- **One filter language.** A server task that works on "the rows matching a filter" builds a view selection and
  resolves it under `Current`, so the rows a user filters to in a header and the rows a task selects on the
  server are chosen by the same code.
- **Copying a selection of the view** fetches the view's rows as pages naming the visible columns, in view
  order, less `except`, for at most `maxCopyRows` rows; over that the copy is refused, as any copy is
  ([Copy](#copy)).

In a document a table's element value `selectionSet` is the selection's JSON on every table, for an action's
body. `selection` is the list of keys, and is empty while the selection is a view.

### The application's menu entries

A table's built-in menus ([Configuring a table](#configuring-a-table)) take the application's entries after
their own:

- `cellMenu` and `headerMenu` are parts of the `menu` node's entry shape: `{label, icon, keys, checked, enabled,
  onClick, items, description, visible}` or `{separator: true}`.
- **A cell's menu** opens on a right-click, the menu key or a long press on a cell. It focuses the cell, and
  selects its row alone when the row is not already selected. It shows the table's entries ("Copy", "Copy with
  headers", "Filter by this value", "Exclude this value"), a separator, then `cellMenu`, read in the row's scope
  with `column` (the column's id) and `value` (the cell) beside `row`.
- **A header's menu** shows its built-in entries, a separator, then `headerMenu`, read in a scope with `column`.
  An entry's `visible` decides which columns it appears on: `{"fn": "eq", "args": [{"ref": "column"}, "kind"]}`
  shows it on one.
- **"Filter by this value"** sets the column's filter to a `oneOf` of the cell's operand in `include`, replacing
  that column's entries; "Exclude this value" adds the operand to the column's `exclude`. On an empty cell they
  use `isEmpty`; on an unloaded or invalid cell they are disabled. On a column of a declared group the entry
  "Filter by this value in any <group label>" sets the group's filter instead.
- An entry's `keys` work while the table has focus. The columns its expressions read are fetched for every row,
  as `rowTone`'s are.

### Rows an action changed

**Kept in view.** A user who changes the rows they filtered to (replaces a component in the designs that hold
it) expects to see the result, not to watch the rows vanish because they no longer match. With `retainChanged`
(a literal flag, default `false`) a row an action of this session changed stays in the view while it no longer
passes the filter.

- The retained rows are the keys of the `ChangedRows` the session's own actions returned for this table, since
  the filter last changed. A change of the filter, or "Show only matching rows" in the status line, clears them.
- `TableQuery::retain` carries them, at most `QueryLimits::maxRetained` (10,000) in all. An action whose rows
  would take the set past the bound adds none of them, and the status line says so; the rows retained before
  stay.
- **The view.** The rows that pass the filter, and the retained rows the caller may see, in the sort's order. A
  retained row whose filter is `Unknown` is in the view, not undecided. `Page::unmatched` holds the indices,
  into the reply's `rows`, of its retained rows that the filter rejects.
- **On the server** the base view of the sort and filter is the view cache's shared entry; the retained rows not
  in it are ordered by the sort's keys and merged into it per request, a cost of the retained rows and not of
  the table. A keyed query sees the merged view; distinct values ignore `retain`, being the filter's.
- **In client mode** `Engine::retain(keys)` does the same; its filter run admits those keys.
- **Ordering with the event.** The action's change event can reach the client before the action's reply, whose
  `ChangedRows` adds the keys. On a table with `retainChanged`, while an action of this session that declares an
  effect on the table is in flight, the window holds back the refetches the table's change events cause, for at
  most `SessionOptions::retainHold` (2 s), and sends one when the action settles or the hold runs out, with the
  retained rows added. Within the hold a row never leaves the view and comes back; past it, other users'
  changes are not held up by a long action.
- A retained row that does not match draws muted, with a tooltip saying it no longer matches the filter, and
  the status line counts them: "3 rows shown that no longer match. Show only matching rows".
- Retained rows are not stored with the table's state. A view selection names them, so it resolves to the rows
  the user saw.

**A key that changes.** An action may give a row a new key: saving a modified design as a new one. The action
returns the row in its `ChangedRows` with `from`, the key the row had ([A model's tables](#a-models-tables)).

- The result walk collects both keys, so the change event names both.
- The session that ran the action moves what it holds by key from `from` to `key`: the row's membership of the
  selection, the focus, its place among the retained rows, its busy or failed state and its cell drafts
  ([`editing.md`](editing.md)). Both keys are then fetched, since the old one may still be a row (the original
  design, unchanged).
- Another session's change event names both keys without saying which became which, so there a selected or
  focused old key behaves as any row that left the view: it is dropped from the selection, and focus moves as
  [Keyboard, focus and assistive technology](#keyboard-focus-and-assistive-technology) says.

### Rows at work, rows that failed

While the session runs an action whose input names rows of a table on screen, the rows it names are **busy**:
for a `RowSelection` member, the keys of a key selection or the held rows of a view selection that `except` does
not name; the row of a `CellEdit` or a `RowEdit` that a document runs as a mutation ([`editing.md`](editing.md));
and the row of a `table::RowRef<&Model::designs>` member, a row key whose schema carries `x-rowOf` with the
table's record, for an action that works on one row. A table's own cell and row edits are not runs of the
session: they make no row busy, and a cell being saved shows as stale instead
([`editing.md`](editing.md#cells-of-a-models-table)).

- A busy row draws the theme's busy mark and takes no cell edit ([`editing.md`](editing.md)). It can still be
  selected, copied and scrolled past.
- The element value `busyCount` counts them, and the status line says "Saving 40 rows".
- When the action settles, its rows stop being busy, whatever its outcome.

An action that changes some rows and not others names the others in `ChangedRows::failed`, each with its
reason ([A model's tables](#a-models-tables)).

- A failed row draws with the `err` tone and its message as the row's tooltip, until the next action of this
  session on it, until a change event that names it arrives after the action settled (a failed row's key is not
  among the keys the action's own event names), or until the user chooses "Clear marks" in the status line. A
  row-scoped table's events name no keys, so there the mark lasts until the next action on the row or "Clear
  marks".
- The status line says "12 rows could not be saved", with "Select them", which makes them the selection.

### The header while the application is busy

`headerEnabled` (a condition, default `true`): while it is false, the header's sort, filter and arrangement
gestures, and the menu entries that change them, are disabled. A gesture made anyway is refused with a message
in the status line. Selecting, scrolling, copying and the application's own entries stay available. A screen
that runs a long operation over a table's rows sets it, so the rows do not reorder or leave the view under the
operation.

## Values with units

A column can hold amounts in different units, and of different kinds of unit: 0.3 kg in one row, 200 g in the
next, 0.3 % in a third. Such a column sorts and filters by what the values mean: 200 g is less than 0.3 kg, and
neither is comparable with 0.3 %.

A `Quantity` column stays what it is: one dimension, every cell converted to the column's canonical unit by the
factor the cell carries. `Measure` is the kind for a column whose cells name their unit.

### Measures and the unit registry

```cpp
/// A unit known to a table: which dimension it measures and how it converts to that dimension's base unit.
struct UnitInfo {
    std::string id;            // "kg", "g", "percent"; what a cell and an operand name
    std::string symbol;        // "kg", "g", "%"; what is shown
    std::string dimension;     // "mass", "ratio"
    math::Rational toBase;     // exact: 1 for kg, 1/1000 for g, 1/100 for percent
    std::uint32_t decimals;    // places shown in this unit
};

using UnitId = std::uint32_t;

/// The units of a process, immutable once built and safe to read from any thread.
class UnitRegistry {
public:
    [[nodiscard]] std::optional<UnitId> find(std::string_view id) const;
    [[nodiscard]] UnitInfo const& info(UnitId unit) const;
    /// Dimensions in the order they were first registered.
    [[nodiscard]] std::span<std::string const> dimensions() const;
    /// The units of one dimension, largest base factor first.
    [[nodiscard]] std::span<UnitId const> unitsOf(std::string_view dimension) const;
};

/// An exact amount in a named unit.
struct Measure {
    math::Rational amount;
    UnitId unit = 0;
};
```

- `Cell` has `Measure`, its last alternative, after `Unloaded`. It is smaller than `QuantityCell`, so a `Cell`
  does not grow.
- `ColumnKind` has `Measure` (`"measure"`). `ColumnInfo::dimensions` is the dimensions the column allows,
  in the order their groups sort; empty allows every dimension, in the registry's order.
- `Services::units` is a `std::shared_ptr<UnitRegistry const>`. A `Measure` column with no registry is a
  `SourceContract` error.
- **The application fills the registry at run time**, on the server, from its own configuration or data: which
  units exist is the application's knowledge, and may be a table in its database. `UnitRegistryBuilder` takes
  `UnitInfo`s and builds an immutable registry; `addUnitsOf<E>()` adds the units of one of morph's compile-time
  unit enums, as a convenience for an application that has them.
- **A client learns units from the table's description** (`TableDescription::units`, the units of the
  dimensions the table's columns allow), and from nowhere else: it has no registry of its own. A typed row's
  schema carries none either; it marks a measure's property `x-measure` only.
- **A unit's id** is 1 to 32 bytes, holds no white space, and does not begin with a digit, a sign, a point or
  a comma; the builder refuses any other, and a second unit of the same id. An operand is a number, a space and
  an id ([Filters on values with units](#filters-on-values-with-units)), so an id with a space in it, or one
  that begins like a number, could not be told from the amount. The ratio's unit is the one empty id.
- **A `UnitId` is stable within a lineage.** The number is a place in one registry, and cells that hold it
  outlive that registry in a level cache. So a registry is changed by building its successor from it:
  `UnitRegistryBuilder{previous}` keeps every unit's number, gives a new unit the next one, and leaves a
  removed unit's number as a tombstone that `find` does not return and that is never given out again. A
  registry and its successors share a `lineage()`. A cell holding a removed unit's number is an invalid cell.
- **Changing the units.** The application builds the successor and swaps it in; requests in flight finish on
  the one they started with. Every description that lists units takes a new `revision`, so each client fetches
  the description again and resets its window, which re-reads its rows and redraws them. Key columns built
  with the previous registry are dropped with the snapshot they belong to, and the view cache's entries with
  them. A level cache holds amounts and unit numbers, not base amounts, and remembers the lineage they are
  of: under a successor its values stay, and under a registry of another lineage, built from nothing, the
  cache invalidates the level when it is next read, because its numbers mean nothing there. A stored display
  unit or filter operand names a unit by its string id, and one naming a removed unit is dropped when it is
  applied.
- Amounts and factors are `Rational`s. A value's key is its amount times its unit's factor, with checked
  arithmetic: a product that does not fit is an invalid cell, as for a quantity. No value passes through a
  double.
- **A plain number is a ratio.** The registry always holds the unit `""` of the dimension `ratio` with factor 1,
  and `percent` is a ratio with factor 1/100. So 0.3 % is 0.003, equals a plain 0.003, and sorts among plain
  numbers. Percent means a hundredth, and treating it as a dimension of its own would make 50 % and 0.5 two
  values. A count of things is not a ratio: it is given a unit of its own (`pcs`).
- On the wire a unit is always its string id, never its number. A generic page writes a measure as
  `[numerator, denominator, unit id]` ([Rows on the wire](#rows-on-the-wire)). A typed row's member is a
  `table::MeasureValue`, written `{"num", "den", "unit"}` as a `Rational` is with the unit's id beside it.
  `RowsSource` resolves the id through the registry it is given, which for a typed table is the model's
  `tableUnits` ([Typed rows are a convenience](#typed-rows-are-a-convenience)); a document's row value of that
  shape is read the same way by `ValueRowsSource`. The row's schema marks the property `x-measure` and lists no
  units: it is built from the row type alone, and the units are the application's run-time registry. A client
  parses, converts and shows a measure with the units of the table's description, without asking.

### Order of values with units

A value's key is a pair: its dimension's place among the column's dimensions, and its amount in the dimension's
base unit.

- Values of one dimension are ordered by base amount, whatever their units: 0.2 kg, 300 g, 0.5 kg.
- Dimensions form groups, in the column's declared order or the registry's. The sort direction reverses the
  amounts inside each group and leaves the groups where they are: a descending sort shows 0.5 kg, 300 g, 0.2 kg,
  then 0.3 %, 0.2 %. Comparing kilograms with percent means nothing, so the direction has nothing to reverse
  between them, and a group that stays put is easier to find; empty cells keep their place for the same reason.
- A cell whose unit the registry does not hold, or whose dimension the column does not allow, is invalid. Valid
  cells come first, then empty and invalid, then unloaded, in either direction, as in every column. An unloaded cell
  has no dimension.
- Ties break by source row. Equal amounts in different units (0.3 kg and 300 g) tie.

### Filters on values with units

An operand names its unit: `"250 g"`, `"0.3 kg"`, `"0.5 percent"`. On the wire it is a decimal with a point, a
space and a unit id, whatever the user's locale; the client turns what was typed into that form.

- **Comparisons** (`lt`, `le`, `gt`, `ge`, `between`) and `eq` match a cell of the operand's dimension by base
  amount, and are `No` on a cell of another dimension: "more than 250 g" keeps 0.3 kg and drops 200 g and 0.3 %.
  They are never `Unknown` for a loaded cell.
- **`ne`** matches every valid cell that is not equal to the operand, a cell of another dimension included:
  0.3 % is not 250 g.
- **Equality is by amount.** `eq "300 g"` matches 0.3 kg. `oneOf` is a set of such keys.
- **`between`** takes two operands of one dimension, in any of its units; of two dimensions it is `InvalidValue`.
- **An operand with no unit is a ratio.** `"0.003"` is the plain number, which equals 0.3 %. On a column that
  does not allow ratios it is `InvalidValue`: the engine does not guess a unit. A menu always sends a unit,
  because it makes the user choose one.
- An operand whose unit the registry lacks, or whose dimension the column does not allow, is `InvalidValue`
  naming the column.
- A group of `Measure` columns filters as any group does; every form of [Column groups](#column-groups) applies.
- An unloaded cell is `Unknown`, as everywhere.

**What a user types.** `ui::parseMeasure(text, units, locale)` reads "0.3kg", "0.3 kg", "300 g" and, in a locale
whose decimal separator is a comma, "0,3 kg": the locale's number, optional space, then a unit's symbol or id,
matched exactly and then without regard to case. Text with no unit takes the unit the menu shows as chosen.

### The values list and the menu

`distinctValues` on a `Measure` column groups by key, so 0.3 kg and 300 g are one value with one count, in key
order: the list comes out grouped by dimension, each group in order of amount. `DistinctValue` has `dimension`
and `unit`: the dimension's name, and the unit of the first row that had the value. The menu draws a heading per
dimension, and its conditions take a number and a unit picker offering the units of the dimensions the column
holds.

### Display

A cell is shown in its own unit, to that unit's decimals. A user may choose a display unit per dimension for a
column, from the header menu; the layout's stored shape holds it as `"units": {column: {dimension: unit}}`. The
shown amount is then the exact conversion, rounded half to even to the display unit's decimals for showing only;
order and filters always use the exact amount. A `Measure` column aligns to the end with digits of one width, as
an `end` column does, with the symbol after the number, so amounts in one unit line up. Changing the display
unit changes the cells' text and may widen the column once.

## Performance

The target is a table of 100,000 rows and 300 columns that stays fluent on the screen. Every layer has a named
benchmark, a budget, and the regression it exists to catch. Budgets marked **M** come from figures measured on
this code ([`engine.md`, "Measurements"](engine.md#measurements)); those marked **P** are provisional and are
replaced by the first recorded baseline plus its spread.

**The fixture.** Every measurement here, at every size, runs against one pure C++ model: no database, no SQL
and no store translator is in any benchmark's path.

- `bench::WideTableModel`, in `tests/table/wide_table_model.hpp` and `.cpp`, built as the static library
  `morph_wide_table_model` and linked by the table, UI, net and Qt Quick test executables, so the server, wire,
  cache, end-to-end and frontend cases all drive the same model.
- It is a registered model type that serves three tables through `BRIDGE_REGISTER_TABLE`: `wide`, the
  100,000 × 300 table; `small`, 1,000 × 30 over the same generator; and `scoped`, whose description adds and
  removes columns by the caller's principal. `wide` and `small` share the rows they have in common and one level
  cache. `EditWideRow` changes one cell, declares `updatesTable` on `wide` and on `small`, and returns a
  `ChangedRows` for each; `ReplaceWideRows` regenerates the rows and declares `changesTable`; `EditWideRecipe`
  changes a record the slow level depends on.
  Together they exercise descriptions, pages, keyed replies, distinct values, effects, invalidation and
  arrivals, and a model with more than one table.
- Its rows are a column store: one typed array per column, generated from `WideTableOptions{rows, columns,
  seed}`, read by a `DataSource` whose snapshot hands the arrays out in runs. The same seed gives the same table
  on every machine. There is no row type anywhere in it: the description is built from the options, and a reply
  is the generic page written from the arrays.
- It is built in the order the design lands ([Delivery order](#delivery-order)): its rows and generator
  first, as `bench::WideTableSource`, which needs nothing but a `DataSource`; registered as a model with its
  three tables when the generic contract lands; its slow level with the level cache. A benchmark is recorded
  from the step that lands what it measures. No baseline is taken of a thing that does not exist.
- Its slow level is a `LevelCache` whose loader waits a configured time per key (`WideTableOptions::loadDelay`)
  or on a gate a test holds, so a load's duration is known and can be subtracted, held or released.

**The data.** Rows × columns of 1,000, 10,000 and 100,000 by 30 and 300. Per 300 columns: 100 integers, 60
decimals, 30 quantities in two units, 10 values with units over three dimensions and ten units, 70 texts with
accents and mixed case, 69 of 4
to 60 characters and one of 200 to 2,000, 20 dates and date-times, 10 booleans; one column in twelve has 10%
empty cells. Three of the text columns form one column group and sixteen another. Twenty columns (ten decimals,
ten texts) are at the level `recipe`, loaded for 70% of the rows unless a benchmark says otherwise.

**How figures are taken.** Release (`-O3`), the same compiler for a comparison, median and 10th to 90th
percentile over at least seven runs at 100,000 rows, the machine's load average recorded beside each table as
`engine.md` records it. The benchmark machine also hosts CI runners, so a timing is never a CI gate: the gates
are the counts (cells read, loader calls, requests sent, widgets realized, bytes), which are exact and run as
ordinary tests. Timings are run by tag, `"[.benchmark]"`, and written into this section. Each benchmark first
asserts that its feature was active (unloaded cells present, the cache hit, the page at 300 columns), so a figure
is never taken over a path the feature did not reach.

### Benchmarks: server

`morph_table_tests "[.benchmark]"`, beside the existing cases.

| Id | Measures | Budget | Catches |
|---|---|---|---|
| S1 | `apply`, sort one numeric key at the `recipe` level, page of 200, 100,000 × 300, the level read through its snapshot | ≤ 48 ms (M: 33.0 ms with all cells loaded, and L1's 8 ms for the level's 100,000 reads); ≤ 1.10 × the same query with no unloaded cell | The unloaded rank slowing every comparison |
| S2 | `apply`, text `contains` on a `recipe` column | ≤ 12 ms (M: 9.25 ms); ≤ 1.15 × the two-valued filter | Three-valued evaluation costing more than a state test |
| S3 | Cells read by `apply`, per column, through a recording source | Exactly the sorted and filtered columns; zero for the other 298 (gate) | `apply` building keys, or reading unloaded state, for every column |
| S4 | `levelsNeeded`, 300 columns, 8 sort keys, 32 filtered columns | ≤ 50 µs (P) | A lookup that scans the columns per key |
| S5 | Engine repair when a level arrives for 200 and for 1,000 rows of a sorted 100,000-row view | ≤ 15 ms (P) and ≤ 35 ms (M: 27.4 ms for 1,000 updates); one repair and no full sort on `EngineStats`, and the arrived rows at the view rows the oracle gives them (gates) | An arrival recomputing the view; an arrival that changes nothing |
| S6 | Repair with 50,000 undecided rows when 1,000 are decided | ≤ 40 ms (P); one repair, no filter run over all rows, and each decided row in the view or rejected as the oracle has it (gates) | The undecided set rebuilt per arrival; a decided row left undecided |
| S7 | Ten consecutive page requests of one sorted view | One view computed, by `ViewCacheStats` (gate); requests two to ten ≤ 2 ms each (P) | A request that sorts again |
| S8 | Resident memory of the source at 100,000 × 300 | Recorded; the fixture's column store, against a `VectorSource` of the same rows, expected near 1.7 GB | A server holding a wide table as cells |
| S9 | A keyed query of 20 keys against that view: the first, which builds the view rows by source row, and the next | ≤ 3 ms and ≤ 0.5 ms (P); no view computed (gate) | A keyed fetch that sorts |

### Benchmarks: server level cache

`morph_table_tests "[.benchmark]"`, with a loader that sleeps a fixed time per key so that load time is known and
subtracted.

| Id | Measures | Budget | Catches |
|---|---|---|---|
| L1 | The hit path of one request: a snapshot and 200 reads (a page), and 100,000 reads (an `order` level) | ≤ 20 µs and ≤ 8 ms (P) | A copy of the cache per request; a lock per read |
| L2 | Bytes per cached row at twenty columns, and for 100,000 rows | ≤ 1.5 kB a row, ≤ 150 MB (P; twenty cells are about 1.1 kB) | Per-value allocation overhead |
| L3 | `invalidate` of 1,000 keys, and the observer call for a 4,096-key change, which runs on the server strand; the same while a reader holds a snapshot for 40 ms | ≤ 0.5 ms and ≤ 1 ms (P), with or without the reader (gate: the call does not wait for it) | Invalidation that scans the cache, or waits for a reply being built |
| L4 | Loader calls when 8 threads want the same 200 keys | 200 keys loaded, once (gate) | No in-flight set |
| L5 | Loader calls for a second query and a second client | Zero (gate) | A miss on a held key |
| L6 | The cache's own time from loader return to announce, per batch of 64, at 100,000 held rows | ≤ 1 ms (P); shards copied ≤ keys stored (gate) | Storing under a lock held across the announce; copying the cache per batch |
| L7 | Two order uses of one level whose candidates do not fit `maxBytes` together | The first loads its candidates once; the second is refused with `Unavailable`; loader calls equal the first's candidates (gates) | Two sorts evicting each other's rows in turn |
| L8 | Ten page requests over rows whose loads failed, inside `retryAfter`; then one with `retry` | Zero loader calls, `awaiting` empty and `failed` set; then one load of the page's keys (gates) | A failed level wanted again by every request |
| L9 | Bytes counted for 1,000 values of twenty cells with texts, against the allocator's figure | Within 10% (gate) | A bound that counts `sizeof` and never evicts |

### Benchmarks: many clients

`tests/net`, one server and fifty clients in one process, each with a window on the fixture's `wide`.

| Id | Measures | Budget | Catches |
|---|---|---|---|
| C1 | Fifty watchers of one sorted view while its order level loads for 100,000 rows in batches | Views computed ≤ level generations read, never one per watcher (gate); refetches per watcher ≤ 4 a second (gate); the server strand's longest task ≤ 2 ms (P) | A sort per watcher per arrival; a strand that sorts |
| C2 | The thread each request's sort runs on, and the model strand's time per request | A `tableWork` thread (gate); ≤ 50 µs on the model's strand (P) | A model that answers nothing else while its table sorts |
| C3 | Fifty watchers with fifty different sorts | Bytes held ≤ `ViewCacheOptions::maxBytes` (gate); views computed per second recorded | A cache without a bound |

### Benchmarks: wire

| Id | Measures | Budget | Catches |
|---|---|---|---|
| W1 | Bytes of a generic page of 200 rows and 40 named columns in an `ok` envelope | ≤ 200 kB (P; 21 bytes a cell in a typed page gives 168 kB); no cell of a column the query did not name (gate) | A reply that carries the table's width |
| W2 | Encode of that page from the column store, and decode on the client | ≤ 2 ms and ≤ 3 ms (P) | A writer or reader whose cost grows faster than the cells |
| W3 | Bytes added by `unloadedRows` when all 200 rows have one unloaded level | ≤ 1% of W1 (P) | The sidecar repeating per cell what is true per level |
| W4 | A keyed reply of 20 rows and 40 columns: bytes, encode and decode | ≤ 20 kB, ≤ 0.5 ms (P) | A keyed reply carrying the page |
| W5 | The same 200 × 30 page as generic rows and as a typed `Page<Row>` | Generic ≤ 0.7 × typed in bytes (P) | An encoding that repeats what the header says |
| W6 | A 300-column `TableDescription`: bytes, encode, decode, and requests for it per mounted table | ≤ 60 kB, ≤ 2 ms (P); one request (gate) | A description fetched per page |
| W7 | The page of W1 with `columns` absent, all 300: bytes, encode, decode | ≤ 1.5 MB (P; 1.27 MB as a typed page) and under `kMaxEnvelopeBytes` (gate); ≤ 10 ms and ≤ 15 ms (P) | A whole page that does not fit an envelope |

### Benchmarks: client cache and window

`morph_table_tests` and `morph_ui_tests`, `"[.benchmark]"`, beside `bench_interpreter.cpp` and `bench_mount.cpp`.

| Id | Measures | Budget | Catches |
|---|---|---|---|
| K1 | `PageWindow::accept` of a 200 × 300 page | ≤ 0.1 ms (P) | Rows copied instead of moved |
| K2 | `refresh` with 900 event keys against 1,600 held rows in eight runs | ≤ 0.5 ms (P); the fetch holds the 900 keys and eight probes (gate) | Matching keys by scanning the held rows per key |
| K3 | `patch` of 200 rows | ≤ 0.3 ms (P) | The same scan |
| K4 | Query cache: a 4,096-key event against an entry of 200 rows; storing a decoded 200 × 300 page | ≤ 2 ms and ≤ 30 ms (P) | A walk of the whole value per key |
| K5 | Rows whose cells are evaluated again after a page refetch that changed 20 of 200 rows | 20 (gate) | A refetch repainting the page |
| K6 | Page fetches sent, and cancelled, while the scroll thumb is dragged across 100,000 rows in two seconds and released | At most three sent, and every one not at the resting place cancelled (gate) | A fetch per page passed |
| K7 | Fetches sent by scrolling sideways by one viewport over held pages | One column fetch per page around the visible rows, naming only the columns not held (gate); bytes ≤ W1 | A sideways scroll that fetches rows it holds |
| K8 | Cells converted to document values for a realized row of 300 columns, twenty drawn | Twenty (gate) | A row record built whole |

### The frontend at 300 columns

Windowing rows alone is not enough at 300 columns. A table that realizes, for each realized row, a widget for
every column, in one horizontal scroll area as wide as all the columns together, holds 50 rows × 300 columns at
thirty visible rows and a margin of ten each side: 15,000 cell widgets, 300 created for each row that scrolls
in. For scale, mounting 14,001 nodes takes 19 ms on the recording backend, which creates no Qt items. With the
columns windowed too ([The renderer](#the-renderer)) the same view is about 50 × 20: 1,000 widgets.

`morph_qt_quick_tests "[.benchmark]"`, a 1,280 × 800 window, a model-backed table of the fixture's `wide`,
100,000 × 300, at a fixed height. The step 0 baseline is a `rows` table of 2,000 × 300, which is the size a mount
with no column window holds.

| Id | Measures | Budget | Catches |
|---|---|---|---|
| F1 | From a page reply to the first frame showing its rows | ≤ 250 ms (P) | Cell creation that scales with all columns |
| F2 | Cell and header widgets alive | ≤ realized rows × (pinned columns + columns in view + 2 × `columnOverscan`), and one header cell for each of those columns (gate) | A change that realizes more than the windows |
| F3 | Frame time scrolling vertically: at 20 rows a second, in a fling at 2,000 rows a second, and under a drag of the thumb across the table | p50 ≤ 8 ms, p99 ≤ 16.6 ms, in each | 300 widgets created per row scrolled in; realization that keeps up by missing frames |
| F4 | Frame time scrolling horizontally across all columns: at 10 columns a second, and in a fling at 100 columns a second | p50 ≤ 8 ms, p99 ≤ 16.6 ms, in each | Layout or rendering cost that follows the full width |
| F5 | Memory per realized row | ≤ 64 kB a row (P) | Widget growth |
| F6 | An arrival for the 50 realized rows, twenty cells each: frames it takes and bindings evaluated | One frame ≤ 16.6 ms (P); 1,000 cells evaluated and no other (gate) | An arrival rebuilding rows |
| F7 | Column-width work on that arrival | Each of the twenty columns grows at most once; ≤ 2 ms (P) | A rescan of every realized row per cell |
| F8 | Cells created, and cells rebound, per frame in F3's and F4's flings | Created: zero once the pools are warm; rebound: ≤ `realizePerFrame` (gates) | A scroll that allocates; a frame that realizes without bound |
| F9 | Work per frame that reads every column: extents, widths, header | Zero reads outside the column window, counted by `ColumnExtents` (gate) | Widths computed over 300 columns per frame |
| F10 | The focused cell after scrolling 10,000 rows and 200 columns away, and the row and column counts the table reports | The cell is realized and holds focus; counts are the view's total and the visible columns (gates) | Focus lost to recycling; counts of the realized rows |

How a frame is timed:

- **In CI**, under `QT_QPA_PLATFORM=offscreen` with the software scene graph, the test drives the scroll position
  in fixed steps through `QQuickRenderControl` and times `polishItems`, `sync` and `render` for each step. The
  figure is CPU time for one frame's work. It is comparable between commits on one machine and says nothing about
  a GPU. The counts in F2, F6 and F7 are exact there and are the gates.
- **Locally, on a display**, with the threaded render loop and vsync, the same test records the intervals between
  `QQuickWindow::frameSwapped` and reports p50 and p99 and the frames over 16.6 ms. This is the figure the 60
  frames a second target is held to, and it is recorded here with the machine and its load.

F2 to F4 are the budgets the column window exists to meet: a table that realizes every column of its realized
rows is recorded against them first, and that record is the measure of what the window buys.

### Benchmarks: end to end

`tests/net`, a socket server and one client in one process, the loader held or timed.

| Id | Measures | Budget | Catches |
|---|---|---|---|
| E1 | First page of a sorted 100,000 × 300 table whose `recipe` level is not loaded: request to accepted page | ≤ 1.10 × the same table with no level; ≤ 120 ms (P) | A reply that waits for a load |
| E2 | From the loader's return to the patched rows, for 20 rows | p50 ≤ 80 ms, p99 ≤ 150 ms (P; 50 ms of it is the coalescing window) | A second round trip, or a page where rows would do |
| E3 | Requests the client sends from first page to patched rows | One page request, then one keyed request (gate) | Polling |
| E4 | The same arrival through the query cache | One page request per marked entry (gate); time recorded against E2 | The cost of a screen that is not on a window |

### Benchmarks: browser

| Id | Measures | Budget | Catches |
|---|---|---|---|
| B1 | Client-mode engine with no worker, native: a three-key sort then a text filter, request to result and worst owner step, at 10,000, 20,000 and 100,000 rows | ≤ 5 ms, ≤ 12 ms (P) and ≤ 70 ms; worst step ≤ 5 ms at 20,000 (P) and ≤ 12 ms at 100,000 (M: 3.27 ms, 57.3 ms, 9.6 to 11.6 ms) | A step that runs past the frame at the size client mode is allowed in a browser |
| B2 | The same, and S1, S2 and W2's decode, compiled to WebAssembly and run under Node | Recorded as a factor over native; expected 1.5 to 3 (estimate) | Nothing: it replaces the estimate this design's browser figures use |
| B3 | Server mode: from a sort change, and from the last key of a text filter, to the accepted page, over a socket with 50 ms added each way | ≤ 150 ms; ≤ 250 ms debounce + 150 ms (P) | A sort or filter change that costs more than one round trip |
| B4 | Requests sent while typing a ten-letter word in 1.5 s | One (gate) | A filter that is not debounced |
| B5 | The WebAssembly build in a headless browser: mount of a `rows` table of 2,000 × 300, then one screen of vertical scroll | Recorded in step 0, against F1 and F3 natively | Nothing: it is the browser's baseline, taken before anything is built for it |

The table module builds for WebAssembly without Qt. Step 0 builds the table tests for that
target and runs them under Node, so the count gates of the single-threaded path run there as they do natively;
B2's factor is a timing and is recorded here with its date, as every timing is, and the engine's
single-threaded step size is set from it. B5 and U8 need a browser and the Qt WebAssembly build and are run
locally, under a headless browser. U8 is the exit criterion of the step that lands the column window, so the
browser is measured when the frontend's cost is decided and not after everything else is built.

### Benchmarks: configuring

| Id | Measures | Budget | Catches |
|---|---|---|---|
| D1 | `distinctValues` on a text column with 50,000 distinct values in 100,000 rows, first 200, keys built | ≤ 30 ms (P; a sort of one text key with its keys is 46.3 ms; recorded runs 15.9 to 35.4 ms on a busy host) | Grouping by comparing texts instead of keys |
| D2 | The same with a three-letter search | ≤ D1 (P; not met in the recorded runs) | A search that folds every value again |
| D3 | The same with a filter on two other columns, one at an unloaded level | ≤ 1.5 × D1 (P); `partial` set (gate) | The cascade running the full filter per value |
| D4 | Compile a `oneOf` of 10,000 values; filter 100,000 rows with it | ≤ 2.2 ms; ≤ 9.1 ms (recorded 2.07 ms and 6.51 ms, spreads to 2.21 and 9.13 ms) | A membership test that scans the set per row |
| D5 | Lookups per row by that filter | One (gate, counted by the set) | The same |
| D6 | Wire bytes of a query with a 10,000-value `oneOf` of the fixture's texts, 16 bytes a value on average; of unticking one value of 50,000 | ≤ 250 kB (P) and under `maxQuerySetBytes` (gate); one value (gate) | A menu that sends the larger side |
| D7 | Adding a third key to a two-key sort of 100,000 rows | ≤ 50 ms (M: a three-key sort is 40.4 ms) | Nothing new in the engine; it bounds the gesture |
| D8 | Cell widgets created and released by hiding a column, and by moving one within the window | None created; the hidden column's cells, one per realized row, returned to their pool; every other cell still bound to its row and column (gates) | A layout change that rebuilds rows; a hide that does nothing |
| D9 | A server-mode distinct request: menu opened to list shown, over a socket | ≤ 150 ms (P) | A request per keystroke of the search (it is debounced, gate: one request per pause) |
| T1 | Parse, fit and apply a stored state for 300 columns with 8 sort keys and 32 filters, the store's own time excluded | ≤ 2 ms (P) | Fitting that scans the columns per stored id |
| T2 | Bytes of that state, and store entries and calls for it; the same for a state whose filter ticks 20,000 values | Recorded; one entry and one call for the first; the second reads back equal, through segments of at most `segmentBytes` (gates) | A store that shortens a state; one that writes every state as segments |
| T3 | Queries sent by a table mounting with a stored sort and filter | One (gate) | A first page fetched before the state is applied |
| T4 | Saves sent for 50 changes within a second | One (gate) | A save per change |
| T5 | Calls to load state when a screen of five tables mounts, and when it mounts a second time | One, then none (gates) | A round trip per table; a state fetched again by the session that holds it |

### Benchmarks: column groups

100,000 rows; a group of 3 and a group of 16 text columns; the group's keys built.

| Id | Measures | Budget | Catches |
|---|---|---|---|
| G1 | `any`, a `oneOf` of 2 values: 3 columns, 16 columns | ≤ 10 ms; ≤ 45 ms (P; about one set lookup a cell) | Compiling the set once per column |
| G2 | `any`, a `oneOf` of 1,000 values: 3 columns, 16 columns | ≤ 1.2 × G1 (P) | Cost that follows the set's size |
| G3 | `atLeast 2` and `exactly 1`, the same sets | ≤ 1.2 × G1 (P) | Counting that does not stop when decided |
| G4 | `together`, 2 entries over 3 columns; 16 entries over 16 columns | ≤ 12 ms; ≤ 120 ms (P) | A search over assignments instead of a matching |
| G5 | Per row: cell lookups, and augmentations in `together` | k lookups in either mode for value entries; at most one augmentation per entry, none for a row with an unmatched entry (gate) | A lookup per entry and cell; a matching attempted on rows that cannot match |
| G6 | Pooled distinct values over 3 and 16 columns, first 200 | ≤ 3 × D1; ≤ 16 × D1 (P) | Pooling by merging per-column lists per request |
| G7 | G1 and G4 with a third of the group's cells unloaded | ≤ 1.2 × the loaded figure (P); undecided rows equal the oracle's (gate) | Three-valued counting done by a second pass |

### Benchmarks: values with units

100,000 rows; one column of three dimensions and ten units, against a `Quantity` column of the same amounts.

| Id | Measures | Budget | Catches |
|---|---|---|---|
| M1 | Build keys and sort the `Measure` column | ≤ 1.2 × the `Quantity` column (P) | A registry lookup by name per cell; a dimension compared as text |
| M2 | Filter `gt "250 g"` | ≤ 1.2 × the `Quantity` column's `gt` (P) | Converting the operand per row |
| M3 | Compile 1,000 operands with units; `ui::parseMeasure` of 1,000 typed texts | ≤ 1 ms each (P) | A scan of the registry per operand |
| M4 | `distinctValues` on the column | ≤ 1.2 × D1 on a decimal column (P) | Grouping by shown text instead of key |
| M5 | Registry lookups per cell while building keys | One, by id (gate) | The same as M1 |

### Benchmarks: every surface

The budgets above are for the pipeline, and every surface is on it, natively and in the browser. These cases hold
each surface to them and record what each costs before it moves.

| Id | Measures | Budget | Catches |
|---|---|---|---|
| U1 | A document `rows` table of 100,000 rows: a header click on a text column, to the sorted view | ≤ 60 ms (M: the engine sorts one text key in 46.3 ms) | A surface sorting outside the engine. The interpreter's pipeline is recorded first as the figure this replaces. |
| U2 | The same table sorted, one row's value changed | ≤ 10 ms when the list shares its unchanged rows, ≤ 40 ms when it is a fresh decode of 100,000 rows (P); one repair, no full sort, cells derived for the column in use only (gates) | A changed row sorting the list; a diff that reads every field |
| U3 | Text folds performed by a sort of 100,000 rows on one text key | 100,000: one per row (gate) | Folding inside the comparison |
| U4 | Memory of a 10,000 × 300 `rows` table as document values and as the source's cells, with one column sorted | Recorded; the source holds one column's cells (gate) | A second copy of the rows |
| U5 | A collection of 20,000 rows: mount, sort, one row edited | As U1 and U2 scaled; recorded | The collection leaving the shared path |
| U6 | A model-backed table of 100,000 × 300: F1 to F7, E1 to E3 on a real screen | Those budgets | A lowering that fetches pages where rows would do |
| U7 | Cell widgets alive in the terminal backend at 100,000 × 300 | Rows in view × columns in view (gate) | A backend that ignores the column range |
| U8 | U1 and U2 at 20,000 rows, and U6's scrolls, in the WebAssembly build in a browser | Recorded against the native figures; the frame budgets of F3 and F4 hold. The exit criterion of the column window's step | A path that works natively and not on one thread |

**What a baseline is for.** A baseline is the figure of a path before the step that moves it onto the
pipeline, recorded once in step 0 and kept beside the budget it is held to afterwards:

- *A `rows` table ordered outside the engine* (U1 to U4): a comparison that folds both texts each time it is
  called folds about 3.4 million strings to sort 100,000 rows, where the engine folds 100,000 once.
- *A reply of every column* (W7 against W1 and W4): a page of 200 × 300 for an arrival of twenty rows.
- *A sort per request* (S7): ten page requests of one view as ten sorts, against one.
- *Every column realized* (F2): 15,000 cell widgets for 50 realized rows.
- *A table held as cells* (U5, S8): about 56 bytes each.

### Recorded baselines

Step 0's figures, taken before anything they measure moves onto the pipeline. Timings are the median and the 10th
to 90th percentile; a count or a size is exact. *Machine A* is a Windows Server 2025 host, AMD EPYC 7763 (128
logical processors), 256 GB, MSVC 19.51.36257 Release (`cl-release`, `cl-qt-release`); it is shared with CI builds, and
its processor time was 82 to 91% busy while these ran (five one-second samples before the run), which is what the
spread reflects. A row marked *not yet measured* says why; its figure is taken where the environment exists.

| Id | Step | Figure | Machine, load | Date |
|---|---|---|---|---|
| S1 | 0 | `apply`, sort `d000` asc, page of 200, 100,000 × 300: 91.9 ms (84.9–109.5) | A, 82–91% busy | 2026-11-17 |
| S2 | 0 | `apply`, filter `t001` contains `"ka"`, 100,000 × 300: 175.3 ms (158.3–182.7) | A, 82–91% busy | 2026-11-17 |
| S7 | 0 | Ten consecutive `apply` calls of S1, offsets 0 to 1,800: 937.7 ms (894.3–946.6) in all, 94.6 ms (80.0–107.5) a call — ten sorts | A, 82–91% busy | 2026-11-17 |
| S8 | 0 | The column store at 100,000 × 300: 757,023,934 bytes (722.0 MiB); the same rows as a `VectorSource`'s cells, by `approximateCellBytes`: 2,143,111,296 bytes (2,043.8 MiB) | A | 2026-11-17 |
| B1 | 0 | Client mode, an owner and no worker, 30 columns, three-key sort then a text filter, request to result: 21.0 ms (19.8–24.0) at 10,000 rows, 42.7 ms (37.0–44.8) at 20,000, 242.5 ms (215.3–245.0) at 100,000 | A, 82–91% busy | 2026-11-17 |
| B1 | 0 | The same, the longest owner step: 10.0 ms (8.1–10.5) at 10,000, 10.2 ms (8.9–10.5) at 20,000, 11.6 ms (10.6–14.2) at 100,000 | A, 82–91% busy | 2026-11-17 |
| B2 | 0 | S1, S2 and B1 under Node from the `table-wasm` build, as a factor over native: *not yet measured* — no Emscripten or Node on machine A; the CI job builds and runs the table tests there, and the factor is taken by running `node build-wasm/tests/morph_table_tests.js "[.benchmark][baseline]"` beside a native run on one machine | — | — |
| B2 | 1 | W2's decode under Node: *not yet measured* — no Emscripten or Node on machine A; the figure is taken by running `node build-wasm/tests/morph_table_tests.js "[.benchmark][wire]"` beside a native run on one machine | — | — |
| B5 | 0 | Qt for WebAssembly in a headless browser, mount of the 2,000 × 300 `rows` table and one screen of scroll: *not yet measured* — no Qt for WebAssembly kit, Emscripten or browser runner on machine A; the harness is `tests/qt_quick/wasm/wide_rows_baseline.cpp`, built and run as its header says | — | — |
| W7 | 0 | A page of 200 × 300 as typed row objects (a decimal as its `{"num","den","dp"}` object, a quantity as its amount): 2,162,606 bytes; encode 14.7 ms (14.4–15.7) | A, 82–91% busy | 2026-11-17 |
| W1 | 1 | A generic page of 200 rows and the 40 columns at positions 100 to 139, in an `ok` envelope: 116,207 bytes, of which 52 are the empty partial-page members (`unloadedRows`, `undecided`, `awaiting`); no other column's cell | A, 43–71% busy | 2026-11-18 |
| W2 | 1 | That page: `writeGenericPage` and its JSON from the column store 1.48 ms (1.45–1.62); `readGenericPage` 2.33 ms (2.21–3.01) | A, 12–23% busy | 2026-11-18 |
| W1 | 1 | The same page as the `Table.wide` reply through `LocalBackend` (a `TableQuery` naming those 40 columns, page of 200), in an `ok` envelope: 116,223 bytes, 16 more than W1's for the description's revision; the round trip 1.79 ms (1.41–2.15) | A, 80–88% busy | 2026-11-18 |
| W2 | 1 | That reply's server side: the table function, the pinned snapshot, `answerQuery` and the page's JSON, 2.25 ms (1.65–2.44) | A, 80–88% busy | 2026-11-18 |
| S1 | 2a | `apply`, sort `d000` asc, page of 200, 100,000 × 300 through `bench::withUnloaded` with the `recipe` level loaded for 70% of the rows: 104.6 ms (90.8–111.4); the same source with every row loaded: 111.7 ms (103.7–117.4). Above the 48 ms budget, as step 0's S1 already was on this host (91.9 ms); within the 1.10 factor, at 0.94, a factor inside the overlapping spreads, so noise and not a speed-up | A, 34–76% busy | 2026-11-18 |
| S2 | 2a | `apply`, filter `t001` contains `"ka"`, the same sources: 147.3 ms (120.5–167.5) at 70% loaded, 219.9 ms (194.8–226.1) with every row loaded. Above the 12 ms budget, as step 0's S2 already was on this host (175.3 ms); within the 1.15 factor, at 0.67. Inferred, not measured: the unloaded rows are faster because an unloaded cell is decided by its state and its text is not searched | A, 34–76% busy | 2026-11-18 |
| S4 | 2a | `levelsNeeded`, 300 columns, 8 sort keys, 32 filtered columns, the filter compiled to find the columns `apply` reads: 35.8 µs (33.8–45.0) a call, within the 50 µs budget | A, 57–76% busy | 2026-11-18 |
| S5 | 2a | An `Engine` sorted on `d000` over the 100,000 × 300 table at 70% loaded: 200 rows arrive by one `load`, 18.3 ms (16.6–22.9); the next 1,000, 21.5 ms (17.9–23.9); one repair and no full sort each. The 200-row arrival is above its 15 ms budget (P); the 1,000-row one is within 35 ms. Inferred, not measured: the cost is mostly the merge into the sorted 100,000-row view, since five times the rows cost 3 ms more | A, 34–76% busy | 2026-11-18 |
| S6 | 2a | An `Engine` filtered `d000 gt 0` at 50% loaded (about 50,000 undecided rows), 1,000 of them decided by one `load`: 4.18 ms (3.91–4.28); one repair, no filter run | A, 34–76% busy | 2026-11-18 |
| W5 | 1 | 200 × 30: generic page 405,396 bytes, encode 3.00 ms (2.91–4.00); typed `Page<Row>` with an absent member for an empty cell 479,205 bytes, encode 1.87 ms (1.80–1.91); generic / typed 0.85, above the 0.7 budget | A, 12–23% busy | 2026-11-18 |
| W6 | 1 | The 300-column description: 42,494 bytes; encode with `describe` 0.23 ms (0.23–0.24); decode 0.14 ms (0.14–0.15) | A, 12–23% busy | 2026-11-18 |
| W6 | 9a | The 300-column description with every column's default `display`, `tone` and `tooltip` left out: 51,727 bytes (91,927 while every column wrote them), under the 60 kB budget. Above step 1's 42,494 by the members added since (each column's `comparator`, `format` and `level`, about 30 bytes a column; inferred, not measured member by member). Measured on a `cl-debug` build, so its timings are not comparable with the Release rows: encode 5.58 ms (5.46–6.47), decode 7.21 ms (7.00–10.32); the byte count does not depend on the build | A, load not sampled, `cl-debug` | 2026-11-20 |
| W7 | 1 | A generic page of 200 × 300 in an `ok` envelope: 1,443,726 bytes, under `kMaxEnvelopeBytes`; encode 17.0 ms (16.5–19.0) and decode 18.7 ms (17.8–21.6), above the 10 ms and 15 ms budgets | A, 12–23% busy | 2026-11-18 |
| W3 | 2b | W1's page (200 rows, the 40 columns at positions 100 to 139, which hold the recipe decimals) with no row's recipe level loaded, in an `ok` envelope: 105,592 bytes; the same page with `unloadedRows` cleared 97,703 bytes; the 200 entries 7,889 bytes, 39 a row, 6.8% of W1 (116,207 bytes), above the 1% budget. One entry a row is the spec's shape, and 1% of W1 is 5.8 bytes a row | A, 43–71% busy | 2026-11-18 |
| W4 | 2b | A keyed reply of 20 keys spread over the view and W1's 40 columns: 12,265 bytes in an `ok` envelope; encode from the column store 0.132 ms (0.127–0.152); `readGenericPage` 0.273 ms (0.258–0.399) | A, 37–64% busy | 2026-11-18 |
| L1 | 2c | A level snapshot and 200 reads of twenty-cell values at 100,000 held rows: 0.066 ms (0.066–0.067), above the 20 µs budget; 100,000 reads 59.8 ms (57.1–77.6), above the 8 ms budget. Inferred from the code, not measured apart: each read copies the value's twenty cells, ten of them strings, into the caller's buffer, which `LevelSnapshot::read(key, span)` requires | A, Release | 2026-11-19 |
| L2 | 2c | Bytes the cache counts for 100,000 values of ten decimals and ten texts of 4 to 60 characters: 151,449,600, 1,514 a row, 0.9% over the 1.5 kB budget (the cells by `cellBytes` and the vector; the shard entries are not counted); the process's private bytes grew 196,718,592 over the store, which includes the 100,000 values built to be stored | A, Release | 2026-11-19 |
| L3 | 2c | Shards of about 64 rows (2,048 at 100,000 rows); every change prepared with no lock held and published under the writer mutex, and a `want` holding that mutex for 1,024 keys at a time: `invalidate` of 1,000 keys 4.11 ms (3.39–6.98), the observer call for a 4,096-key change 15.0 ms (11.7–19.8); with a reader holding a snapshot for 40 ms, 2.55 ms (1.90–3.72) and 13.1 ms (12.1–19.8) (gate `"level cache: invalidation does not wait for a reader"`; gate `"level cache: an invalidation waits for one chunk of a large want at most"`). Measured with the host nearly idle (one compiler process running): L1, whose code this measurement did not change, read 0.064 ms (0.064–0.082) against its recorded 0.066 ms. **Both miss the 0.5 ms and 1 ms budgets**; the budget is an owner's question. Cause, inferred from the code and not measured apart: random keys spread over the shards, so 1,000 keys touch about 800 of the 2,048 shards and the call copies about 40% of the entries and frees the shards they replace; the 4,096-key call touches nearly every shard | A, Release | 2026-11-19 |
| L6 | 2c | Loader return to announce for a batch of 64 at 100,000 held rows: 0.385 ms (0.374–0.480), within the 1 ms budget; 960 shards copied for 960 keys stored over fifteen batches (gate: at most one shard a key, `"level cache: a batch copies only the shards it touches"`). Measured with the host nearly idle (one compiler process running): L1, whose code this measurement did not change, read 0.064 ms (0.064–0.082) against its recorded 0.066 ms | A, Release | 2026-11-19 |
| S7 | 2c | Ten `answerQuery` calls through one `ViewCache` over the 100,000 × 300 column store, sorted on `i001`, pages 0 to 9 of 200: the first 75.1 ms (it sorts), requests two to ten 1.48 ms (1.43–1.49) each, within the 2 ms budget; one view computed for the ten (gate) | A, Release | 2026-11-19 |
| S9 | 2c | A keyed query of 20 keys against that view: the first 22.0 ms (it builds the view rows by source row and the key index, 100,000 rows each), above the 3 ms budget; the next 1.22 ms, above the 0.5 ms budget; no view computed (gate) | A, Release | 2026-11-19 |
| C1 | 2c | In process, not over a socket (`"table benchmark: C1 C2 C3 many requests through an in-process server"`): fifty `Table.wide` requests on fifty instances, sorted on the `recipe` column `d000` at 100,000 rows, sent together after an arrival on the level: **399–583 ms** over six runs (558.9, 432.1, 399.1, 461.1, 582.8, 512.2) from the first send to the last reply, on a loaded host (3 to 63 compiler processes during the runs; L1, whose code did not change, read 0.101 and 0.106 ms against its recorded 0.066 ms); one view computed, 7 requests waited for it, the other 42 found it computed; the new view's order set was fitted once and protected its 100,000 keys in one pass, releasing the superseded set of the view before the arrival after the repeats waiting for the new set were woken, and the other 49 requests were repeats of the set, each one more holder and a longer lease under the lock. Without that release, and without counting the sets' keys in the bound, the same benchmark measured 339–401 ms on a nearly idle host; how much of the difference is the host is not measured apart. Measured apart (`"table benchmark: C1 where a request's time goes"`): the table function, its description and the pinned snapshot 0.184 ms; a whole `answerQuery` from the cached view 0.934 ms (0.897–0.974) sorted on the `recipe` column and 0.853 ms on a base column; an order `want` of 100,000 held keys as a plain list, which leases each key, 23.9 ms (22.8–29.0); the one sort after the arrival is S7's first request, 75 ms. The bound holds ten base-column views and one view sorted by a level with its 100,000 order keys (8,000,024 bytes); at a bound of ten 400 kB views the level-sorted view, about 4.4 MB with its keys, does not fit and is computed again by later requests (3,338.7 ms, 7 views computed). The server strand's longest task is not measured: it needs the socket benchmark (`[.benchmark][net]`, POSIX only, not run) | A, Release | 2026-11-19 |
| C2 | 2c | In process: twenty distinct sorts of `wide` one after another, 21.2 views computed a second | A, Release | 2026-11-19 |
| C3 | 2c | In process: fifty sorts against the C1 bound, ten 100,000-row base-column views and one level-sorted view's 100,000 order keys (8,000,024 bytes), which holds about twenty base-column views once the level-sorted one is evicted: 8,000,000 bytes held, 31 evictions (gate: held within the bound). The evictions are fewer than at a bound of ten views because the bound is wider, not because a view holds less | A, Release | 2026-11-19 |
| K1 | 2d | `PageWindow<GenericRow>::accept` of a whole 200 × 300 page of integer cells, into a fresh window built with `maxColumns` 300 (at the default of 96 a page holds no 300 columns): 3.05 ms (2.81–3.69), **above the 0.1 ms budget (P)**. Inferred from the code, not measured apart: the page is stored by column, so each of the 60,000 cells is moved into its column's vector, and the reply's 200 row vectors are freed; the cells are moved, never copied | A, Release | 2026-11-19 |
| K2 | 2d | `refresh` of 900 event keys against 1,600 held rows in eight runs (pages 0, 2, …, 14 of 200): 0.102 ms (0.090–0.194), within the 0.5 ms budget; the fetch holds the 900 keys and eight probes (gate `"window: a refresh of 900 keys over eight runs asks 908"`) | A, Release | 2026-11-19 |
| T2 | 9 | Through the `morph.tableState` model over an in-process server, as the `state` part and alike as the `views` part: a state of 300 columns, 8 sort keys and 32 filters is 7,700 bytes, one store `set` and one entry (gate); a state whose filter ticks 20,000 values is 1,380,070 bytes, 123 segments of at most 12 KiB with a quote and a two-byte character in every value, 125 `set` calls (the intent, the segments, the head) and 124 entries after the save, and reads back byte-equal (gate `"table state: a full state is one entry and one call; …"`) | A, Debug | 2026-11-19 |
| K3 | 2d | `patch` of 200 rows and the seven other runs' probes, one column: 0.325 ms (0.306–0.374), just above the 0.3 ms budget (P). It builds an index of the 1,600 held keys for each patch | A, Release | 2026-11-19 |
| K4 | 2d | Debug build only (`cl-debug`), so not comparable with the budgets: a 4,096-key event against an entry of 200 rows, 86.4 ms (74.6–101.4); storing a decoded 200 × 300 page of fractional numbers in an opaque entry, 708.1 ms (623.3–759.6). The Release figure is *not yet measured*: a Release build of `morph_tests` did not fit the session; the case is `"interpreter bench: K4 the query cache under an event and a wide page"` | A, Debug | 2026-11-19 |
| E1, E2 | 2d | *Not yet measured* — the case `"table benchmark: E1 E2 a window over the bridge"` is a socket benchmark (`[.benchmark][net]`, POSIX only); machine A builds no `morph_net_tests` | — | — |
| D1 | 3 | `distinctValues` through an `Engine` holding the column's keys, a text column of 50,000 distinct values in 100,000 rows, first 200: 35.4 ms (26.8–43.9); an earlier run of the code before its unused reserve was removed, at 40–54% busy, 15.9 ms (15.5–22.1) | A, 51–77% busy | 2026-11-18 |
| D2 | 3 | The same with the search `"abc"`: 41.4 ms (27.5–42.5), above D1 as in the earlier run (30.9 ms against 15.9) | A, 51–77% busy | 2026-11-18 |
| D3 | 3 | The same with `o1 ge 20` and `o2 lt 80`, `o2` unloaded in 30% of the rows, `partial` set: 25.3 ms (24.4–26.6) | A, 51–77% busy | 2026-11-18 |
| D4 | 3 | A `oneOf` of 10,000 texts: compiled in 2.07 ms (2.01–2.21); 100,000 rows filtered with it, keys built, in 6.51 ms (6.22–9.13) | A, 40–54% busy | 2026-11-18 |
| G1 | 3 | An `any` group with a `oneOf` of 2 values, 100,000 rows: 3 columns 12.2 ms (11.5–13.8), **over** the 10 ms budget (earlier runs 18.3 and 23.3 ms); 16 columns 53.3 ms (49.1–68.1), **over** the 45 ms budget. Each figure is an `Engine::setFilter` with the group's keys held, so it includes the view's rebuild and change, not the group filter alone | A, 20–46% busy | 2026-11-19 |
| G2 | 3 | The same with a `oneOf` of 1,000 values: 19.8 ms (16.5–21.3), **over** 1.2 × G1 (14.7 ms) in this run; 66.5 ms (54.5–82.4), **over** 1.2 × G1 (64.0 ms) in this run. The previous run had 15.3 ms against G1's 18.3, and 82.0 ms against 70.6, both within; the run before that, 113.2 ms at 16 columns, over | A, 20–46% busy | 2026-11-19 |
| G3 | 3 | `atLeast 2`: 11.9 ms (9.8–15.1); 68.7 ms (63.8–74.7), **over** 1.2 × G1. `exactly 1`: 12.8 ms (12.0–15.8); 70.1 ms (68.8–81.6), **over** 1.2 × G1 | A, 20–46% busy | 2026-11-19 |
| G4 | 3 | `together`, 2 entries over 3 columns: 11.9 ms (10.6–13.9), within the 12 ms budget in this run (17.4 ms, over, in the previous one); 16 entries over 16 columns: 74.5 ms (67.0–95.3) | A, 20–46% busy | 2026-11-19 |
| G6 | 3 | Pooled distinct values through the engine, first 200 of 32: 3 columns 7.8 ms (7.7–8.0); 16 columns 51.7 ms (46.9–64.3) | A, 20–46% busy | 2026-11-19 |
| G7 | 3 | G1 and G4 with a third of the cells unloaded: G1 15.9 ms (15.6–17.1), **over** 1.2 × the loaded 12.2 ms, and 58.3 ms (45.5–61.4), within; G4 14.5 ms (13.3–14.8), **over** 1.2 × the loaded 11.9 ms by a hair, and 76.2 ms (72.5–97.7), within 1.2 × 74.5 ms. The 16-column `together` figure rests on a count of the columns that can match any entry, taken before the wildcard matching: with the unknown columns' masks full, the covered test passes on nearly every row, and a row with fewer such columns than entries is refused without running a failing augmenting-path search for each entry | A, 20–46% busy | 2026-11-19 |
| M1 | 3 | `apply` sorting 100,000 rows, keys built in the call: the `Measure` column over the fixture's registry (three dimensions, ten units) 80.5 ms (75.5–114.4) and 78.2 ms (73.0–83.2) in two runs, against the `Quantity` column of the same amounts with their units' factors 90.9 ms (86.2–102.3) and 72.6 ms (71.8–83.4): 0.89 and 1.08 times, within the 1.2 budget | A, 5–55% busy | 2026-11-19 |
| M2 | 3 | `apply` filtering `gt "250 g"`: 30.9 ms (19.3–31.6) and 23.6 ms (19.3–24.1), against the quantity's `gt "0.25"` 22.4 ms (19.3–24.7) and 23.8 ms (23.7–23.9): 1.38 times, **over** the 1.2 budget, in the first run, and 0.99 in the second; the 10th percentiles of the first run are equal, so its median is inferred to be the host's noise, not measured apart | A, 5–55% busy | 2026-11-19 |
| M3 | 3 | Compiling a `oneOf` of 1,000 operands with units in nine units: 0.57 ms (0.56–0.59) and 0.50 ms (0.49–0.52); `ui::parseMeasure` of 1,000 texts typed in German: 0.29 ms (0.22–0.31) and 0.29 ms (0.27–0.39); both within 1 ms | A, 5–55% busy | 2026-11-19 |
| M4 | 3 | `distinctValues` of the `Measure` column, first 200, keys built in the call: 49.4 ms (46.4–51.9) and 46.5 ms (45.9–57.0), against a `Decimal` column of the same amounts 30.3 ms (28.8–33.8) and 37.6 ms (34.6–45.6): 1.63 and 1.24 times, **over** the 1.2 budget in both runs. Inferred from the code, not measured apart: a measure's key is read with a checked product by its unit's factor and grouped by a place and an amount, where a decimal's is copied and grouped by the amount alone, and the units make more distinct values of the same amounts. Measured as D1 is, through an `Engine` holding the column's keys, in a third run on a busier host (M1's two columns at 109.0 and 121.7 ms): 67.7 ms (64.4–73.9) against the `Decimal` column's 56.1 ms (54.0–66.3), 1.21 times, at the budget; the same run's in-call figures were 97.9 against 68.5 ms | A, 5–55% busy; third run busier, not sampled | 2026-11-19 |
| U1 | 0 | A document `rows` table of 100,000 rows on the recording backend, a bound sort set on a text column, click to settled: 2,921.5 ms (2,860.0–3,017.6). The rows realized after the sort are the ones realized before it, reordered, not the view's first rows: the figure is the interpreter's sort and reconcile, not a correct window | A, 82–91% busy | 2026-11-17 |
| U2 | 0 | The same table sorted, one row's name replaced in `items`, click to settled: 2,941.3 ms (2,881.4–2,992.2) | A, 82–91% busy | 2026-11-17 |
| U3 | 0 | Text folds of the interpreter's sort of 100,000 rows: *not yet measured* — the interpreter folds with a `CodePointCollator` it constructs inside the comparison and takes none from outside, so no counting collator can be installed; the count needs that seam | — | — |
| U4 | 0 | A `rows` table of 10,000 × 300 sorted on one column: the rows as cells, by `approximateCellBytes`, 214,423,008 bytes (204.5 MiB, 71.5 bytes a cell); the state's `items` as document values, counted the same way (each value, a string's heap, a list's or a record's container, a map node's links; shared lists and records once), 432,663,112 bytes (412.6 MiB, 144.2 bytes a cell) | A | 2026-11-18 |
| U5 | 0 | A collection of 20,000 rows over a `ListRows` reply: mount 117.0 ms (102.8–143.5); sort on a text column through the engine 46.9 ms (37.6–62.1); one row edited, the list reloaded and the sorted view repaired, 206.4 ms (171.9–226.2) | A, 82–91% busy | 2026-11-17 |
| U8 | 5 | U1 and U2 at 20,000 rows, and U6's scrolls (F3 and F4), in the WebAssembly build in a browser: *not yet measured* — the harness, `tests/qt_quick/wasm/u8_scrolls.cpp`, is written and unbuilt: no Emscripten, Qt for WebAssembly kit or Chrome on machine A, and Qt Quick cannot be configured there. The window-shaped part of the exit criterion (every scroll's p50 ≤ 8 ms and p99 ≤ 16.6 ms) is open until it is built and run as its header says | — | — |
| U1 | 6 | A document `rows` table of 100,000 rows on the recording backend, a bound sort set on a text column, click to settled, through `ValueRowsSource` and the engine (owner, no worker, 4 ms steps): 494.5 ms (472.2–528.7), **over** the 60 ms budget. Measured apart on an engine of its own (8 ms steps, the default collator): the source deriving the column's 100,000 cells 91.4 ms (88.1–100.7), the engine absorbing them 40.1 ms (36.9–51.8), the sort to settled 257.0 ms (247.6–284.7). The rest of the screen figure, about 100 ms, is the lowering's, inferred by subtraction and not measured apart | A, 35% busy (128 logical processors) | 2026-11-19 |
| U2 | 6 | The same table sorted, one row's name replaced in `items` (its other rows shared), click to settled: 356.5 ms (341.9–378.5), **over** the 10 ms budget. Measured apart on an engine of its own: the source matching the new list 101.1 ms (99.0–127.5), the engine absorbing and repairing it 116.0 ms (115.5–130.6); counts hold (one repair, no full sort, one column's cells derived: the gate cases). The rest, about 140 ms, is inferred by subtraction to be the lowering's per-change passes over every row: the view's records read again for `TableView`, the page's entries rebuilt, every field of every row read for a unit the screen does not list, and above the virtualization threshold every key given to the backend; the mount itself receives the engine's own operations for the one row | A, 35% busy (128 logical processors) | 2026-11-19 |
| U3 | 6 | Text folds of a sort of 100,000 rows on one text key: 100,000, one per row (the gate case "a sort of 100,000 rows folds each text once") | — | 2026-11-19 |
| U4 | 6 | A `rows` table of 10,000 × 300 through `ValueRowsSource`, sorted on one column: the source holds 10,000 cells, 1,033,840 bytes (0.99 MiB), against 214,423,008 bytes (204.5 MiB) for every column as cells | A | 2026-11-19 |
| B1 | 6 | A `rows` table's engine (owner, no worker, 4 ms steps, `ValueRowsCollator`), three-key sort then a text filter, request to result: 19.6 ms (17.1–21.3) at 10,000 rows, 39.3 ms (34.9–44.8) at 20,000, 216.4 ms (202.9–251.0) at 100,000. The longest owner step: 6.1 ms (5.3–6.6) at 10,000, 6.3 ms (6.1–6.8) at 20,000, **over** the 5 ms bound, and 23.4 ms (21.0–26.1) at 100,000, **over** 12 ms | A, 35% busy (128 logical processors) | 2026-11-19 |
| F2–F5 | 0 | A `rows` table of 2,000 × 300 in a 1,280 × 800 offscreen window: widgets alive, frame times of the vertical and horizontal scrolls, memory per realized row: *not yet measured* — on machine A the interpreter's mount of that table on the Qt Quick backend did not finish within its 300 s budget (the document's text 0.4 s, its parse 0.8 s); the case, `"qt quick baseline: F2 to F5 on a rows table of 2,000 x 300"`, bounds every step and names the one that overruns | A, 80–90% busy | 2026-11-18 |
| W6 | 9a | The 300-column description of the W6 case, Release (`cl-release`): 51,727 bytes, as the `cl-debug` row has it, under the 60 kB budget; encode with `describe` 0.385 ms (0.371–0.406), decode 0.280 ms (0.275–0.292), both within the 2 ms budget | A, 30–60% busy, `cl-release` | 2026-11-21 |
| T1 | 4 | Parse, fit and apply a stored state of 300 columns with 8 sort keys and 32 filters, no store (`"table state benchmark: parse, fit and apply 300 columns, 8 keys, 32 filters"`): 0.47 ms (0.45–0.67), within the 2 ms budget | A, 30–60% busy, `cl-release` | 2026-11-21 |
| D6 | 3 | A query with a 10,000-value `oneOf` of 16-byte texts: at most 250,000 bytes and under `maxQuerySetBytes`, and valid (the gate case `"table query: a 10,000-value set fits the wire bound"`, passing on `cl-release`). The case asserts the bound and does not print the byte count, so the exact figure is not recorded | A, `cl-release` | 2026-11-21 |
| K4 | 2d | Release (`cl-release`), the case `"interpreter bench: K4 the query cache under an event and a wide page"`; the median and the lowest to highest of the runs, which is what the case prints: a 4,096-key event against an entry of 200 rows, 3.50 ms (2.47–5.43) over fifteen runs, **above the 2 ms budget (P)**; storing a decoded 200 × 300 page of fractional numbers in an opaque entry, 23.3 ms (22.0–31.2) over seven runs, within the 30 ms budget | A, 27–55% busy, `cl-release` | 2026-11-21 |
| D7 | 7 | A document `table` of 100,000 rows on the recording backend, sorted on two keys from the header; a Shift-click adding the third key, click to settled (`"interpreter bench: a third sort key on 100,000 rows"`): 233.4 ms (222.5–262.3), **above the 50 ms budget (M)** by 4.7 times | A, 20–59% busy, `cl-release` | 2026-11-21 |
| U5 | 8 | A `collection` node of 20,000 rows on the table engine, on the recording backend (`"interpreter bench: a collection of 20,000 rows: mount, sort, one row edited"`): mount, the list's reply to settled, 190.0 ms (179.0–216.9); a header sort on Name, click to settled, 40.6 ms (36.2–53.4); one row edited, the reloaded list's reply to settled, 284.8 ms (240.3–305.6). Against U1 and U2 scaled by rows to 20,000 (U1's 60 ms to 12 ms; U2's 40 ms for a fresh decode, which a reloaded list is, to 8 ms): **the sort is above its scaled budget by 3.4 times and the edit by 36 times**; the mount has no budget of its own. Step 0's figures for the same three, through the engine and not on a screen, were 117.0, 46.9 and 206.4 ms | A, 15–44% busy, `cl-release` | 2026-11-21 |

## Invariants

1. **Soundness of the filter.** For every way of replacing the unloaded cells of a table with loaded cells, a
   row `apply` puts in the view is in the view of the completed table, and a row it rejects is rejected there.
2. **Soundness of the order.** Two rows with no unloaded cell in any sort column have the same relative order
   before and after any completion.
3. **Counts.** view rows + undecided rows + rejected rows = source rows, a retained row counting in the view and
   not as rejected. `totalKind` is `AtLeast` whenever `undecided > 0`.
4. **Unloaded is not empty.** `isEmpty` and `notEmpty` are `Unknown` on an unloaded cell; `displayText(Unloaded{})`
   is the empty string and is never used as a key. A cell whose load failed is unloaded.
5. **No unloaded cell at the base level.** One is refused as `SourceContract`.
6. **Nothing unneeded is read or sent.** `apply` reads cells only of sorted and filtered columns; a reply holds
   cells only of the columns its query names; `levelsNeeded` names no level outside the query's sorted, filtered
   and named columns.
7. **Absent means loaded and whole.** A source that never produces `Unloaded`, columns with no `level`, a query
   without `columns`, `keys` and `retain`, and a `Page` without `unloadedRows`, `undecided`, `awaiting`, `failed`
   and `unmatched` are a
   fully loaded table of every column: no cell is unloaded, no row is undecided, and every result is the one
   [`engine.md`](engine.md) specifies.
8. **A window's totals come from page replies only**, and a window never shows a row at a view row the server did
   not report for it in the reply that supplied it.
9. **No event is lost to ordering.** For any interleaving of a page reply and an event for one of its rows, the
   window ends holding the row as the server had it after the change.
10. **No polling.** Nothing is requested because time passed. Time only delays or merges a request that an
    event or a gesture caused: the server's coalescing window, the window's settle and awaited-refetch delays,
    a filter's debounce, a state save's debounce. Leases and retry delays are read from a clock when a request
    arrives and schedule nothing.
11. **Loaded once.** Between two invalidations of a row's level, the loader is called for that row and level at
    most once, however many queries, pages, sessions and clients need it, unless the value was evicted or its
    load failed.
12. **Nothing stale lands.** A value whose load began before an invalidation of its row is never stored.
13. **Visible before told.** A change event for a row is sent only after a read of the cache would return the
    row's new state, whether that state is a value, a failure or its absence.
14. **No value crosses a scope.** A reply holds a row only from the source the model returned for that caller,
    and a cell only in a column `mayRead` allows. A cached value is laid out against the cache's own column list,
    never a caller's description, and a cached view is shared only between callers of one `scope`.
15. **A table is its description.** Nothing a client or the framework needs to show, sort, filter or page a
    table comes from a C++ type: a model that can build a `TableDescription` and a `DataSource` has a table.
16. **One order.** Every table on a screen orders and filters through `morph::table`: the same keys, the same
    collation rules, the same place for empty, invalid and unloaded cells.
17. **A document keeps its rows.** A document that names none of this design's vocabulary shows the same rows in
    the same order on the pipeline, except where [The document `table` node](#the-document-table-node-on-the-engine)
    says otherwise.
18. **One mode per table**, decided by its declaration, for its lifetime.
19. **Nothing realized out of view.** The cells and header cells alive are bounded by the rows and columns in or
    near view, the pinned columns and the focused cell's row and column, whatever the table's size.
20. **A changed row is a repair** on every surface that holds its rows.
21. **Arranging is not ordering.** A change of layout never changes the view's rows or their order, never runs a
    sort or a filter on the server, and fetches only the cells of columns the window does not hold for the
    rows around the view.
22. **A value list is what the other filters leave.** A column's distinct values and counts are those of the rows
    passing every other column's filter, and the sum of the counts, `blanks` and `invalid` is that row count
    when `partial` is false.
23. **A set filter is its conditions.** `oneOf` with values v1…vn matches exactly the rows `eq v1` or … or `eq vn`
    match.
24. **A group filter is its definition.** For every row with no unloaded cell, a `Columns` filter passes exactly
    when the count of satisfying columns meets `match`, and a `Together` filter exactly when an assignment of
    entries to distinct columns exists; `{columns, combine, include}` alone is the `any` case.
25. **Units do not change an amount.** Two `Measure` cells of one dimension order and compare as their amounts in
    the base unit do, exactly; two of different dimensions are never equal and never ordered by amount. A unit's
    numeric id means the same unit in every registry of one lineage.
26. **A stored arrangement never breaks a table.** Whatever a store holds, the table shows at least one
    column and applies only keys and filters on columns it has.
27. **State before rows.** A table with a store shows its first rows only after its stored state is applied or
    its load has timed out or failed; a table whose load completes in time sends exactly one first query.
28. **One store.** Every table's arrangement is read and written through the injected `ITableStateStore` and
    nowhere else, no key it is given names a person, and no layer shortens what it is given.
29. **A cached view is the view.** A reply built from a view cache entry equals the reply `apply` would build
    from the same source snapshot and level snapshots.
30. **A shift is seen.** After a keyed reply is patched, either every held row is at the view row the server
    has it at and the window's totals are the server's, or the window is invalidated.
31. **A refusal is shown.** Every refusal of a user's gesture (a sort key past the bound, a set too large, a
    range or a copy too long, a save refused, a select-all while rows are undecided, a gesture on a disabled
    header) appears in the status line or, where a document hides it, in the session's notifications.
32. **Text never orders.** A column's display (places, rounding, labels, empty text) changes what a cell shows
    and nothing a sort, a filter, a distinct value or a selection uses.
33. **A selection of the view is the view.** Under `Exact`, `resolve` returns exactly the rows of the view the
    user selected, less `except`, or refuses; under either policy it returns no row outside the caller's scope.
34. **A revision that holds means rows that held.** Two page replies with the same `viewRevision` describe views
    of the same rows.
35. **Retaining adds, never removes.** A view with `retain` holds every row it holds without it, and the retained
    rows the caller may see; `unmatched` names exactly the retained rows the filter rejects.

## API surface

Headers marked (new) do not exist before the step that lands them ([Delivery order](#delivery-order)).

| Header | Holds |
|---|---|
| `table/data_source.hpp` | `struct Unloaded`; `Cell`'s alternatives `Unloaded` and then `Measure`, last; `ColumnInfo::level`, `dimensions`; `ColumnKind::Measure`; `TableErrorCode::SourceContract` (`"sourceContract"`); `displayText` returns empty text for `Unloaded`; `UnitInfo`, `UnitId`, `UnitRegistry` with `lineage()`, `UnitRegistryBuilder` (from a previous registry), `addUnitsOf<E>`, `Measure`; `Services::units`; `DataSource::groups()`; `cellBytes`. |
| `table/sort.hpp` | `CellState::Unloaded`; `KeyColumn::read` maps `Unloaded`; `compareKeys` ranks valid, then empty and invalid, then unloaded. |
| `table/filter.hpp` | `enum class Match { No, Yes, Unknown }`; `CompiledFilter::evaluate(Column const&, KeyColumn const&, std::uint32_t) -> Match`; `passes` is `evaluate == Yes`; `FilterRun::takeUndecided() &&`; `FilterEntry::oneOf`; `GroupMode`, `ColumnCount`; `GroupFilter::group`, `mode`, `match`, `exclude`; matching for `Together`. |
| `table/engine.hpp` | `detail::JobInput::previousUndecided`, `detail::JobResult::undecided`; `Engine::undecidedRowCount()`, `undecidedRows()` (source rows), `onUndecidedChange(std::function<void(std::size_t)>)`; `Engine::distinctValues`; `ColumnLayout::setWidth`, `widths`, `setPinned`, `pinned`, `value`, `apply`, and `rebuild` placing a column by the fitting rule. |
| `table/query.hpp` | `TableQuery::columns`, `keys`, `distinct`, `retry`; `QueryLimits::maxColumns`, `maxKeys`, `maxSetValues`, `maxQuerySetValues`, `maxQuerySetBytes`; `ColumnLevel` and the row type's `columnLevels()`; `LevelLoaded<Row>` as a trailing parameter of `RowsSource` and the typed `apply`; `LevelsNeeded`, `levelsNeeded`; `PageRows::totalKind`, `undecided`, `awaiting`, `failed`, `indices`; `UnloadedRow`, `UnloadedLevels`; `Page::unloadedRows`, `undecided`, `awaiting`, `failed`, `indices`, `distinct`; `DistinctRequest`, `DistinctValue`, `DistinctValues`, `DateBucket`, `distinctValues`; `changeKeys`, `rowIdOf`; `PageFetch::columns`, `revision`; `RowsFetch`, `Refresh`, `RefreshPlan`; `PageWindow`'s key function, `maxKeys` and `maxColumns`; `PageWindow::visible` with columns, `superseded`, `cancelled`, `refresh`, `owed`, `invalidate`, `patch`, `unloadedLevels`, `undecided`, `awaiting`, `failed`; the specialisation `PageWindow<GenericRow>`. |
| `table/edits.hpp` | `CellEdits::commit` refuses an unloaded cell. |
| `table/description.hpp` (new) | `TableColumn`, `ColumnGroupInfo`, `TableCapabilities`, `TableDescription`, `TableContext`, `ServedTable`, `GenericRow`, `GenericPage`, `ChangedRow`, `ChangedRows<fn>`, `TableOf<fn>`, `table::typed<Row>` and the model's `tableUnits` hook it calls, `table::scoped`, `table::rowScoped`, `table::derivedFrom`; the generic page's writer and reader. `ColumnDisplay`, `Rounding`, `ValueLabel`; `TableColumn::display`, `tone`, `tooltip`, `edit` and `EditInfo` ([`editing.md`](editing.md)); `TableDescription::rowTone`, `rowLock`, `bands` and `BandInfo`; `TableCapabilities::maxSortKeys`; `ResolveRequest` and the `Table.<id>.resolve` action; `ChangedRow::from`, `FailedRow`, `ChangedRows::failed`. |
| `table/row_selection.hpp` (new) | `RowSelection<fn>` and its schema's `x-selectionOf`, `SelectionPolicy`, `SelectedRows`, `resolve`, `ResolveRequest`; `RowRef<fn>` and its schema's `x-rowOf`. |
| `table/data_source.hpp`, `table/query.hpp`, `table/engine.hpp` (additions) | `TableErrorCode::ViewChanged`; the view cache entry's membership digest; `QueryLimits::maxSelectedRows`, `maxRetained`; `TableQuery::retain`, `Page::unmatched`, `Page::viewRevision` and their generic-page members; `Engine::retain`. |
| `table/query.hpp`, `table/filter.hpp` (removals) | `QueryPlan` and the planned types, `PlanUse`, `planQuery`, and `plan_conformance.hpp` whole: no table is paged in a database. |
| `ui/cell_format.hpp` (new) | `ui::formatCell`, the one formatter of a cell's text from its column's display and the session's `ui::FormatContext`. |
| `table/level_cache.hpp` (new) | `LevelRead`, `LevelUse`, `LevelSnapshot`, `LevelSource`, `LevelCacheOptions`, `LevelCacheStats`, `LevelCache<Value>`, `CellLevelCache`. |
| `table/view_cache.hpp` (new) | `ViewCacheOptions`, `ViewCacheStats`, `ViewCache`. |
| `table/state_store.hpp` (new) | `TableStateKey`, `StoredTableState`, `SaveOutcome`, `SaveResult`, `kNoDocument`, `ITableStateStore` with `shares()` and `publish`, `Sharing`, `MemoryTableStateStore`; the views operations; the state's and the views' readers, writers, version migration and fitting. |
| `forms/views.hpp`, `forms/engine/collection.hpp` | The schema carries `x-level` from `columnLevels()` and `x-column-group` from `columnGroups()`; `deriveColumns` copies them; `CollectionColumn::level`. |
| `core/change_types.hpp` | `effects::IChangeObserver` (`changed(Changes const&)`, `bind(AnnounceSink)`); `effects::AnnounceSink`. |
| `core/remote.hpp` | `RemoteServer::announce`, `RemoteServer::attach(std::shared_ptr<effects::IChangeObserver>)`; `ServerConfig::tableWork`, `views`. |
| `core/registry.hpp`, `core/effects.hpp` | `BRIDGE_REGISTER_TABLE` and its options; the actions `Table.<id>` and `Table.<id>.describe`; the table's record; `effects::updatesTable<fn>`, `effects::changesTable<fn>`; the result walk's slot for `ChangedRows<fn>`, collecting each row's `key` and `from` and no `failed` key, and its record of a holder's `openMembers()` and `partialMember()`; `ActionRecordInfo::open`, `partial`, `opaque`. `Page<Row>` declares both members. |
| `ui/table_rows.hpp` (new) | `SortChain`, `TableTotal`, `TableRowRef`, `TableCountsView`, `TableRows`, `EngineRows`, `WindowRows`, `ValueRowsSource`; `ui::parseMeasure`; a selection of the view, retained, busy, failed and locked rows, and key moves by `from`, in both `TableRows`; `TableCountsView::retained`, `busy`; `SessionOptions::retainHold`; the element value `selectedCountKind`. |
| `ui/interpreter.hpp` | `Session::read`, `Session::watchRecord`, `WatchHolder`; the query cache's rules for open, partial and opaque entries; `SessionOptions::tableState`, `tableStateTimeout`, `tableStateDebounce`, `tableSettle`, `tableAwaitedRefetch`, `maxRangeRows`, `maxCopyRows`; the lowering of every table node onto `TableRows`; a table node's `cellMenu`, `headerMenu`, `retainChanged`, `headerEnabled` and element values `selectionSet`, `busyCount`; a column's display parts, `tone` and `tooltip`; a table node's `status`, `persist`, a column's `kind` and `locked`. |
| `ui/view.hpp`, `ui/mount.hpp`, `ui/backend.hpp` | `Virtualization::columnThreshold`, `cellThreshold`, `columnOverscan`, `realizePerFrame`; `ColumnExtents`; the column window, cell pools and the frame budget; `Table::status`; `IViewBackend::copyText`; the range-selection report by view rows. |
| `ui/table_state.hpp` (new), `ui/table_state_model.hpp` (new) | `PreferencesTableStateStore`; `TableStateModel` (`morph.tableState`) with its `publish` action for shared views, `TableStateOptions`. |
| `qt_quick` and the terminal backend | Cells and header cells for columns in the window only; pinned columns; header menus, drag, resize, the sort dialog, the column chooser, the filter menu, the status line. |

A caller that passes nothing new gets what it gets without this design, with one exception, stated where it
applies: `ColumnLayout::rebuild` places a new column after its declared predecessor instead of last.

## Tests

Each case names the mutation it was seen to fail on, as the module's other tests do.

| Condition | Test | Layer |
|---|---|---|
| (a) the first page arrives before a slow level is loaded | A recording `DataSource` logs the column of every `cell` and `readColumn` call and returns `Unloaded` for the slow level. `apply` returns the page, and the log holds only the sorted and filtered columns. Mutation: `apply` building keys for every column. Over the bridge: the list action answers while the loader is held on a latch the test never releases, and the reply's `unloadedRows` names exactly the unloaded rows and the level. Mutation: the model waiting for its loader. | `tests/table/test_table_unloaded_cells.cpp`; `tests/net/test_push_unloaded_rows.cpp` |
| (b) cells show as unloaded | `Page::unloadedRows` names exactly the unloaded rows and levels; `Engine::cellAt` returns `Unloaded`; `PageWindow::unloadedLevels` agrees after `accept`. Mutation: the typed `apply` dropping `unloadedRows`. | `test_table_unloaded_cells.cpp`, `test_table_server.cpp` |
| (b) unloaded cells do not match a filter | Per operator of every kind, `isEmpty` and `notEmpty` included, include and exclude, `any` and `all`, and a group: the row is undecided, the view lacks it, `totalKind` is `AtLeast`. A row `Unknown` at one stage and `No` at a later one is rejected. Mutation: `evaluate` returning `passes`. Property: invariant 1 over random tables, unloaded cells and completions. | `test_table_filter.cpp`, `test_table_unloaded_cells.cpp` |
| (b) unloaded cells do not sort as empty | A column with values, empties and unloaded cells, both directions: unloaded rows are the tail, after the empties. Mutation: `compareKeys` tying unloaded with empty. Property: invariant 2. | `test_table_sort.cpp` |
| (c) a level loads only when a shown, sorted or filtered column needs it | A table of `levelsNeeded` cases: a column of the level named, sorted, filtered, in a group, not named, `columns` absent, `columns` empty. A model with a recording loader: a query using no column of the level records nothing; one that names a column of it records the page's rows only; one that sorts on it records every candidate row. Mutation: `levelsNeeded` returning every level, which makes the first case record a load. | `test_table_query.cpp`; `test_push_unloaded_rows.cpp` |
| (d) exactly the arrived rows update, without polling | Engine: an undecided row arriving is one `Inserted` and one `onUndecidedChange`; an undecided row rejected is no view operation and one `onUndecidedChange`; a displayed row arriving is `Changed` for that row only, under `Immediate` and `Deferred`. Mutation: a repair that ignores `previousUndecided`. Window: `refresh` asks for the event's keys and one probe per held run; `patch` returns the replaced view rows only; a missing key, a moved row, an unheld key inside a held page and a changed total each invalidate; a keyed reply older than a later fetch for the same key is ignored. Bridge: over a real socket the server's action counter shows one page request, then none until `announce`, then one keyed request holding the announced keys and the window's probes; the test has no `refreshEvery` and advances no clock. Mutations: `refresh` asking for every held key; `refresh` dropping the keys the window does not hold, which leaves a row an edit moved into the window unseen. | `test_table_changes.cpp`, `test_table_server.cpp`, `test_push_unloaded_rows.cpp` |
| An event that precedes its page reply | The window is given `refresh(keys)` while the page fetch is in flight, then the reply with those rows unloaded: `owed` returns those keys. Mutation: `refresh` not recording keys in flight. | `test_table_server.cpp` |
| An ordinary edit on the same record | A held, fully loaded row's event yields a keyed fetch; a reply at another view row invalidates. Mutation: `refresh` skipping rows with no unloaded level. | `test_table_server.cpp` |
| Invalidation faster than a round trip | Invalidate, fetch, invalidate again, then accept the first reply: the page shows the reply's rows and is still stale; `visible` returns one fetch, not two. Mutation: `accept` ignoring every older revision. | `test_table_server.cpp` |
| `announce` | A derived record is included; more than `kMaxChangeKeys` keys become whole-type; an unregistered record is refused. `changeKeys` and `rowIdOf` round-trip both key kinds and agree with the record's key member. | `tests/net/test_push_unloaded_rows.cpp`, `test_table_query.cpp` |
| `TableQuery` on the wire | `columns` and `keys`, integer and text, round-trip through glaze; a query without them reads with both absent. An unknown column in `columns` is `UnknownColumn` and an unreadable one `ColumnNotReadable`; an empty `columns` gives a page of keys. | `test_table_query.cpp` |
| A value is loaded once | Two queries, then two clients over a socket, need the same 200 rows' level: `LevelCacheStats::loadedKeys` is 200 and the loader's own call log lists each key once. Mutation: `want` not checking the cache, which makes it 400 and 800. | `tests/table/test_table_level_cache.cpp`; `test_push_unloaded_rows.cpp` |
| Concurrent queries share a load | Eight threads call `want` for the same keys while the loader is held on a latch: one loader call. Mutation: no in-flight set. | `test_table_level_cache.cpp` |
| Reloaded once after invalidation | After `invalidate` of 10 keys the view lacks them; the next two queries load exactly those 10, once. An action declaring `updates` on the record does the same through the server with no call to `invalidate`. Mutation: `changed` ignoring the record, which leaves the old values served and the loader uncalled. | `test_table_level_cache.cpp`; `test_push_unloaded_rows.cpp` |
| A load in flight at invalidation | The loader is held; the row is invalidated; the loader returns the old value: the view does not hold it, `droppedLoads` is 1, and the next `want` loads again. Mutation: storing without the generation check, which serves the old value. | `test_table_level_cache.cpp` |
| Visible before told | The announce sink reads the cache when called and finds the value for every key it is given; on invalidation it finds none. Mutation: announcing before storing. | `test_table_level_cache.cpp` |
| Eviction | Over `maxBytes` the least recently used rows that no order lease holds go, the view shows them absent, and a query needing them loads them again; no event is sent for an eviction. | `test_table_level_cache.cpp` |
| No value crosses a scope | Two principals with different row scopes query one cached table: each reply holds only its rows, and the loader ran once for the shared rows. | `test_push_unloaded_rows.cpp` |
| An open entry hears every change | A cached page whose `awaiting` is not empty is marked by an event for a key it does not hold; one whose `awaiting` is empty is not. Mutation: the cache ignoring `open`. | `tests/test_ui_interpreter_cache.cpp` |
| A partial entry is not merged into | An `updates` reply for a row of a cached page with `unloadedRows` marks the entry and leaves its value; without `unloadedRows` it merges. Mutation: the cache ignoring `partial`, which shows an unloaded member's default as a value. | `tests/test_ui_interpreter_cache.cpp` |
| A table from a description | A model with no row type serves a table; a client given only the catalog draws its columns, sorts, filters and pages it. Mutation: a lowering that needs the node's `columns`. | `tests/table/test_table_description.cpp`; `tests/test_ui_table_model_*.cpp` |
| A model with several tables | Two tables of one model are authorised, cached and changed apart: a principal allowed one action is refused the other; an action declaring `updatesTable` on one marks only its watchers; an action that changes two tables returns two `ChangedRows`, and each table's watchers get that table's keys; `updatesTable` without its `ChangedRows` in the result is refused when the registries resolve; one table id in two models is refused when the registries resolve. Mutation: one record for the model. | `tests/net/test_push_tables.cpp`; `tests/table/test_table_description.cpp` |
| Columns by session | Two principals get different descriptions of one table; a page's `columns` are the caller's; a changed `revision` makes the client fetch the description again. | `tests/net/test_push_tables.cpp` |
| The generic page | Every kind of cell round-trips: empty, unloaded, a quantity with a factor and with none, a measure; a typed table served through `table::typed` writes the same page as the generic source of the same rows. | `test_table_description.cpp` |
| Column layout | A stored layout with unknown ids, a new column and every column hidden yields the fitted layout; hiding and moving create no cell widgets (gate D8). Mutation: `apply` ignoring `hidden`. | `tests/table/test_table_columns.cpp`; `tests/qt_quick` |
| Sort chains | `Shift`+click adds a key and flips one in the chain; a plain click replaces; a key past the table's `maxSortKeys` is refused. Mutation: `Shift` ignored. | `tests/test_ui_table_model_*.cpp`; `tests/qt_quick` |
| `oneOf` | It matches exactly the rows the OR of its `eq` entries matches, in include and exclude, with blanks beside it; an unloaded cell is undecided. Mutation: a `oneOf` that matches nothing. | `test_table_filter.cpp` |
| Distinct values | Counts are those of the rows the other filters leave; the column's own filter is ignored; counts, `blanks` and `invalid` sum to those rows when `partial` is false; a group's "(Blanks)" count is the rows its `isEmpty` entry admits; unloaded rows set `partial`. Mutation: counting over the view. | `tests/table/test_table_distinct.cpp` |
| Column groups | The truth table of [Column groups](#column-groups) row by row, including the row that tells `together` from `atLeast 2`; each quantifier with unloaded cells against its three-valued rule; an exclude; a group with exclude entries and no include entry, in either mode. Mutation: `together` evaluated as `atLeast`. | `test_table_filter.cpp` |
| Values with units | 300 g sorts between 0.2 kg and 0.5 kg in both directions; `gt "250 g"` drops 0.3 % rows; 0.3 % and 0.3 kg are not equal; 0.3 kg and 300 g are one distinct value; an operand with no unit is a ratio. Mutation: a key that ignores the unit's factor. | `tests/table/test_table_measure.cpp` |
| Persisted state before the first query | A table mounts with a stored sort and filter in a `MemoryTableStateStore` whose load is held: no query is sent; released, one query goes out carrying the stored sort and filter. Mutation: a table that ignores its store sends the default query. | `tests/test_ui_table_model_*.cpp` |
| Stale state is fitted | A stored state naming a removed column, a removed group, an unknown unit, a filter that no longer compiles and every column hidden: the table shows, with one column at least, the rest applied, each drop reported once; what a session's description lacks is written back unchanged. Mutation: applying the state unfitted. | `tests/table/test_table_state.cpp` |
| Saves are coalesced | Fifty changes inside the debounce are one save holding the last arrangement; a change during a save in flight is one more; unmounting saves at once. Mutation: no debounce. | `tests/test_ui_table_model_*.cpp` |
| A slow or failed load | A load past the timeout starts the table from defaults and is applied when it arrives, unless the user changed something; a failed load is reported once and later changes still save. | `tests/test_ui_table_model_*.cpp` |
| The person is the store's | Two principals mounting one table through the preferences-backed store read and write separate state; a key naming a person does not exist to forge. | `tests/net/test_push_tables.cpp` |
| Bound values win | With `sort` bound, a stored sort is neither applied nor saved, and the stored layout still is; `persist: false` loads and saves nothing. | `tests/test_ui_table_model_*.cpp` |
| One order on every surface | A `rows` table, a collection and a model-backed table over the same rows show the same order for the same sort, text and measures included. Mutation: the interpreter's pipeline left in place for one surface. | `tests/test_ui_table_model_*.cpp` |

| A cached view is the view | Property: for random sorts, filters and unloaded cells, a page, a keyed reply and a second page built from one cache entry equal `apply`'s over the same snapshots. An arrival on an order level makes the next request compute again; one on a display level does not. Mutation: an entry that ignores a level's generation, which serves the old order. | `tests/table/test_table_view_cache.cpp` |
| One sort for many watchers | Fifty requests for one view while it is computed: one computation, on a `tableWork` thread. Mutation: no single flight. | `test_table_view_cache.cpp`; `tests/net/test_push_tables.cpp` |
| A reply holds the named columns only | A page of forty named columns holds no other cell, and loads only the levels those columns have; a column that scrolls in is fetched for the held pages and merged by key; a column fetch whose keys differ invalidates. Mutation: a writer that ignores `columns`. | `test_table_description.cpp`, `test_table_server.cpp` |
| A row the window never held | An edit moves an unheld row into a held page, above it, and across it: the keyed reply's view row, a probe and the total each invalidate; an edit far below the pages with the total unchanged does not. Mutation: no probes, which leaves the held rows one view row off. | `test_table_server.cpp` |
| A scroll that passes | A drag across the table sends no fetch until the visible rows rest for `tableSettle`, and cancels what it left behind; a scroll into the next page fetches at once. Mutation: a fetch per `visible` call. | `tests/test_ui_table_model_*.cpp` |
| An awaited order is throttled | With `awaiting` set, ten events in 100 ms are one refetch at once and one after `tableAwaitedRefetch`. Mutation: a refetch per event. | `tests/test_ui_table_model_*.cpp` |
| A level's layout is the cache's | Two principals whose descriptions hold different columns of one level, in different orders, read one cached value: each reply has each cell under its own column, and a column `mayRead` refuses is absent. Mutation: reading the value by the caller's column order. | `test_table_level_cache.cpp`; `test_push_tables.cpp` |
| A cache's size is deep | Values of twenty text cells evict at `maxBytes` of their real size. Mutation: `sizeof(Value)`, under which nothing is ever evicted. | `test_table_level_cache.cpp` |
| A reader blocks no one | With a snapshot held, a store, an invalidation and the observer call complete, and the snapshot still reads its old values. Mutation: a shared lock held by the snapshot. | `test_table_level_cache.cpp` |
| Order uses are leased | Two order uses that do not fit together: the first loads once and keeps every row, the second is `Unavailable`; after `orderLease` on an injected clock the first's rows are evictable. A level whose values outgrow the estimate stops loading and announces without keys. Mutation: no lease, under which the loader's call count grows without end. | `test_table_level_cache.cpp` |
| A failed load | The loader returns no value for ten keys: their cells are unloaded, the reply names the level in `failed` and not in `awaiting`, the keys are announced once, and no request loads them until `retryAfter` on an injected clock, a `retry` query or an invalidation. A row with nothing at the level comes back empty and loaded. Mutation: a failed key wanted again by every request. | `test_table_level_cache.cpp`; `test_push_unloaded_rows.cpp` |
| A row-scoped table | A principal's watch receives an arrival without keys and refetches its pages; no event it receives names a key outside its scope. `rowScoped` with an empty `scope` is `SourceContract`. Mutation: the record registered without `privateKeys`. | `test_push_tables.cpp` |
| An opaque entry | A document query on `Table.<id>` and a distinct-values entry are marked by a keyed change of the table's record that names none of their rows, and are not merged into; a page's fractional `Number` cell is held as a decimal, while an ordinary action's reply with a fraction is still refused. Mutation: the cache looking for a key member. | `tests/test_ui_interpreter_cache.cpp` |
| A read with no entry | `Session::read` makes no cache entry and resolves with the reply's text, a fractional number intact; `watchRecord` puts the record in the watch set while its holder lives and is told a whole-type change on resync; the first page fetch follows the watch. | `tests/test_ui_interpreter_cache.cpp` |
| A document's rows as a source | A fresh decode of the same rows notifies nothing; one changed row is one `Updated`; a change to what a `sortBy` reads updates that row; an unused column holds no cells; integers with decimals are one `Decimal` column and a text among them is one invalid cell; a timestamp is a `DateTime` cell of whole seconds, rounded down; only a literal `kind` makes a `Number` column. Mutation: a diff by row value. | `tests/test_ui_table_model_*.cpp` |
| Windows of the renderer | 1,999 × 300 is windowed both ways, and 1,000 × 30 by its cells alone; cells and header cells alive match F2, pooled ones counted; a fling creates no cell once the pools are warm; a column growing left of the view keeps the columns in view where they are; pinned columns stay realized, narrow tables unpin for display only and pin again when the viewport widens. Mutation: a column window that ignores `cellThreshold`. | `tests/test_ui_mount.cpp`; `tests/qt_quick` |
| Keyboard and focus under windowing | The focused cell survives a scroll of 10,000 rows; `Ctrl+End` over rows not fetched lands on the last row when it arrives; the reported row count is the total, or unknown for a lower bound. | `tests/test_ui_mount.cpp`; `tests/qt_quick` |
| Range selection in server mode | A range over rows not held resolves to the view's keys through pages that name no columns; over `maxRangeRows` it is refused with a status message and selects nothing. | `tests/test_ui_table_model_*.cpp` |
| Copy | Selected rows copy as tab-separated display text in layout order, in the chosen display units; rows not held are fetched; unloaded cells are empty and counted in the message; over `maxCopyRows` is refused. | `tests/test_ui_table_model_*.cpp` |
| The status line | Counts for exact, lower-bound and undecided totals; a failed level with "Retry", which sends `retry`; a refusal shown once; `status: false` sends the message to the session's notifications. Mutation: a refusal with no message. | `tests/test_ui_table_model_*.cpp` |
| State of any size | A state with a 20,000-value filter saves through `PreferencesTableStateStore` and reads back equal, as segments each within `segmentBytes`; a save stopped after its segments leaves the previous state readable; a missing segment is a failed load. Mutation: no segments, the whole document in its head. | `tests/test_ui_table_state.cpp` |
| Views are not lost between windows | Two windows each add a view from the same loaded revision: the second save answers `Conflict`, reloads, and both views are stored. A table that loaded no views saves with `kNoDocument`; over a store that does not honour it, the retry after a `Conflict` whose reload finds nothing saves with an empty `expected`. A `state` save overwrites. | `tests/test_ui_table_state.cpp` |
| Unit numbers across registries | A successor registry keeps every number and tombstones a removed unit; a level cache keeps its values under a successor and invalidates under another lineage; a unit id with a space is refused. | `tests/table/test_table_measure.cpp` |
| A column's display | 0.125 at two places shows "0.12" under `HalfEven` and "0.13" under `HalfUp`; `eq 0.13` matches neither; an empty cell shows `empty`, an unloaded one its placeholder; a `Bool` with labels orders false first; `decimals` on a `Measure` column is `SourceContract`; copy writes the formatted text. Mutation: a sort over the formatted text, which orders "10.00" before "9.00". | `tests/test_ui_cell_format.cpp`; `test_table_description.cpp` |
| Companion columns travel with their column | A page naming a column names its `tone` and `tooltip` companions and the `rowTone` one; a column out of the window fetches neither; a document `tone` expression stops the companion being fetched. Mutation: companions fetched for every column. | `test_table_description.cpp`; `tests/test_ui_table_model_*.cpp` |
| A selection of the view | `Ctrl+A` on a model-backed table fetches no row; `resolve` under `Exact` returns the view's keys less `except` and refuses `ViewChanged` after an edit that moves a row into the view; under `Current` it returns the new view; a principal's resolution holds no row outside its scope; select-all is refused while rows are undecided. Mutation: `viewRevision` that ignores the source snapshot, which resolves the edited view under `Exact`. | `tests/table/test_table_selection.cpp`; `tests/net/test_push_tables.cpp` |
| Retained rows | After an action changes three rows so that the filter rejects them, they stay in the view, listed in `unmatched` and counted; a change of the filter clears them; an action over `maxRetained` retains none and says so; a client-mode engine does the same. Mutation: the merged view written into the shared view cache entry, which serves one session's retained rows to another. | `test_table_selection.cpp`; `tests/test_ui_table_model_*.cpp` |
| A key that changes | A `ChangedRow` with `from` moves the selection, the focus and a failed mark to the new key and fetches both. Mutation: the walk ignoring `from`, which leaves the old row's watchers untold. | `tests/test_ui_table_model_*.cpp`; `test_push_tables.cpp` |
| Busy and failed rows | While an action over a selection runs, its rows draw busy and refuse an edit; a `failed` entry draws `err` with its message until the row's next change; "Select them" selects exactly the failed keys. Mutation: busy state cleared only on success. | `tests/test_ui_table_model_*.cpp` |
| The application's menu entries | `cellMenu` entries read `row`, `column` and `value`; a right-click on an unselected row selects it alone; "Filter by this value" sets a one-member `oneOf` and on an empty cell `isEmpty`; a `visible` on `column` shows an entry on one header only. Mutation: the cell menu read in the table's scope. | `tests/test_ui_table_model_*.cpp`; `tests/qt_quick` |
| Resolving from elsewhere | `Table.<id>.resolve` from another model's action returns the caller's keys and counts unseen keys in `dropped`; a view over `maxSelectedRows` is `LimitExceeded`; after `ViewChanged` the client offers the view as it is and resends on confirmation; a change of the filter ends a view selection. Mutation: `resolve` run under the server's principal, which returns rows outside the caller's scope. | `tests/net/test_push_tables.cpp`; `tests/test_ui_table_model_*.cpp` |
| The view revision is membership | An edit that changes a cell of a row in the view without moving it in or out leaves `viewRevision` as it was; one that brings a row in changes it. Mutation: the revision taken from the snapshot's version, which refuses the first case. | `test_table_selection.cpp` |
| Retained rows and the event | With `retainChanged`, an event that precedes its action's reply causes no refetch until the reply or `retainHold`. Mutation: no hold, under which a retained row leaves the view and comes back. | `tests/test_ui_table_model_*.cpp` |
| Locked rows and companions | A row whose `rowLock` companion holds text draws locked with that tooltip and refuses a cell edit; companion columns are absent from the column chooser and the header menus; an unknown tone is reported once. Mutation: the chooser listing every column of the description. | `tests/test_ui_table_model_*.cpp` |
| A raised sort bound | A table whose `limits` set 32 accepts a 32-key chain from the header and refuses the 33rd; the description carries the bound; `limits` above 32 is `SourceContract`. Mutation: the client holding 8 whatever the description says. | `tests/test_ui_table_model_*.cpp`; `test_table_description.cpp` |
| Shared views | A view published through `morph.tableState.publish` is read by a second principal and copied by "Save as my view"; a principal the authoriser refuses `publish` is refused; a store whose `shares()` is false, a `MemoryTableStateStore` built with the default `Sharing::Private` among them, offers no publishing; `defaultOnMount` applies the default view's sort and filters over the stored state at mount. Mutation: the shared part saved through `save`, which an authoriser cannot tell from a user's own state. | `tests/test_ui_table_state.cpp`; `tests/net/test_push_tables.cpp` |
| Copy of a selection of the view | A copy of a view selection less two keys writes the view's rows in order without them, fetched as pages; over `maxCopyRows` it is refused. | `tests/test_ui_table_model_*.cpp` |
| A disabled header | With `headerEnabled` false a header click, a filter menu and a column drag change nothing and each is refused in the status line; selection and copy still work. Mutation: `headerEnabled` read once at mount, which leaves the header enabled after the condition turns false. | `tests/test_ui_table_model_*.cpp` |

The arrival path on a screen (a model-backed `table`, its repaint and its frame cost) is tested and measured
with the lowering, as cases U6, F6 and F7 of [Performance](#performance).

## What this does not do

- It does not know how a level is read. The loader is the model's function; the level cache decides when it runs.
- It does not fetch one cell's detail on demand. A level that is never loaded in bulk and is fetched for one row
  by a keyed query naming its column is expressible, and nothing here assumes a level is loaded for all rows.
- It does not add row attributes. `UnloadedRow` is a sidecar keyed by row index; another per-row sidecar on `Page`
  does not conflict with it.
- It does not give `RowsSource<Row>` a row-level update: `setRows` notifies `Reset`. A client-mode table that
  wants exact row updates on arrival uses a `VectorSource`.
- It does not page a forms collection or make its rows unloaded: a collection's rows arrive whole, and a
  collection whose `v-query` takes a query is a design for the forms specs.
- **It does not edit cells.** Editing a cell in place, the grid that lays out a fixed form as rows and columns,
  spans, header bands and summary rows are [`editing.md`](editing.md)'s, which builds on this pipeline. A row
  of a model's table is changed by an action of the application that declares `updatesTable`.
- It does not format a value on the server for a client: the client formats from the exact value
  ([How a column shows its cells](#how-a-column-shows-its-cells)).
- **It does not export.** Copy puts selected rows on the clipboard ([Copy](#copy)); writing a view to a file is
  an application action over the query.
- It does not group rows, aggregate, or total a model-backed table: `group` and `rowSpans` are a `rows` table's.
- It does not page, sort or filter a table in a database. A model with rows in a database reads them into a
  `DataSource`; a report that aggregates over a database is the application's.
- It does not offer top-N or ranking filters, or colour and icon filters. A tone is shown, never filtered on;
  a filter on what decides it is a filter on that column.
- It does not keep loaded values across a server restart ([Caching layers](#a-persistent-tier)).
- It does not repair a cached view when a level's generation changes: the view is computed again, once for all
  its watchers ([The view cache](#the-view-cache)).
- It does not cache distinct values on the server.
- It does not invalidate a level cache in local mode, where no server derives changes: the application calls
  `invalidate` itself.
- It does not lower a collection's or a wizard's chrome.
- It does not turn a `rows` table bound to a page into a model-backed one.
- It does not give a model-backed table grouping or row spans; the node refuses them. Its select-all is not
  a client-side one: it is a selection of the view, resolved on the server.
- It does not sort or filter held rows on the client in server mode
  ([Where sort and filter run](#where-sort-and-filter-run)).
- It does not change `forEach` or `grid`.
- It does not define the `layout` preference point. A table's arrangement is its persisted state, and the
  `columns` and `saved` points that name a table edit that state.
- It does not merge two windows' arrangements or push a saved arrangement to a table that is already mounted.
- It does not limit the size of a table's stored state.

## Alternatives considered

- **A readiness mask per row and level instead of a `Cell` alternative.** It states the loader's unit directly and
  costs one bit per row and level. But inside the engine the mask is a second call beside every cell:
  `RowSnapshot` needs a new virtual, key building needs the mask beside each cell it is handed through
  `ColumnSink`, `TableSnapshot`'s chunks and `withRow`, `VectorSource::updateRow` and `RowPatcher::patchRow` need
  a mask argument, and every reader of `Engine::cellAt` must ask a second question or show an unloaded cell as
  empty. The mask is used where a `Cell` cannot go, on the wire beside typed rows.
- **A `Loadable<T>` member type in the row.** It would make the wire state impossible to ignore, but changes every
  row type, its schema and every reader of its JSON, and says per member what is true per level.
- **Showing undecided rows in the view.** See [Unloaded cells in filters](#unloaded-cells-in-filters): it shows rows
  the filter may reject and needs a
  total that is an upper bound.
- **Two-valued filters, with an unloaded cell matching nothing.** An include then hides the row and an exclude shows
  it, so the same unknown is answered both ways, and invariant 1 fails for excludes.
- **Unloaded tying with empty in the order.** Rows whose place is unknown would be scattered among the empties by
  source row.
- **A separate "rows by key" action.** A second action per table repeats the table action's scope and
  authorisation and can drift from it; `keys` on the query reuses both.
- **A keyed reply without view rows, patched in place.** It cannot tell an arrival from an edit that moves the
  row, so a row edited in a sorted column would stay where it was.
- **Refreshing only rows with an unloaded level.** The record's events also carry edits; a loaded row would then
  never refresh.
- **Dropping every reply older than the current revision, or cancelling fetches on `invalidate`.** With events
  closer together than a round trip no reply is ever current, and the window never updates.
- **`announce` taking a ready-made `effects::Changes`.** It skips derived records and the key bound unless every
  caller rebuilds them.
- **A level name in the event.** The server would decide per connection what a client needs; the client knows
  what it shows and holds.
- **`ChangeKind::Loaded`.** An engine does the same work for it as for `Updated`.
- **Shown levels in the query instead of columns.** It keeps a query equal while columns of one level come and
  go, which matters only where the query's body is a cache key. A window's fetches make no cache entry, and a
  reply of every column is 1.27 MB at 300 columns, decoded on the painting thread in a browser, for the twenty
  a user sees.
- **Asking only for the keys a window holds.** An edit that moves a row the window has never held into its
  pages is then never seen, unless every action that might do so declares a whole-table change; and an action's
  author cannot know how each client has sorted.
- **Sorting for every request.** Every page of a scroll, every keyed fetch and every watcher of an arrival pays
  33 to 46 ms at 100,000 rows, on a strand that answers nothing else meanwhile.
- **A view of the level cache that holds a shared lock.** A reply holds it for as long as its sort takes, and
  the observer call that removes entries runs on the server strand and would wait for it.
- **Eviction by recency alone.** A row a view orders by is evicted by another query's loads, leaves the view
  with no event, and is loaded again; two sorts on one level can then evict each other without end.
- **One untyped list of changed keys.** An action's keys are matched to a record by the type of the result's
  slot, so one list cannot belong to two tables.
- **An edit action in the table's contract.** It needs client vocabulary, validation and effects of its own;
  half of it in the contract is a capability no client can use.
- **`Pending` for the cell's state, renaming the table's other uses of the word.** A `table` element's `pending`
  is read by documents, and the two facts differ.
- **Table state through the preferences model.** Its value bound is right for a preference and wrong for a
  filter of 20,000 values; an exemption per caller would make the bound mean nothing.
- **A size limit on table state, dropping what does not fit.** A filter the user set would be forgotten
  silently at the next mount.
- **Each model keeping its own store of loaded values.** Every model would re-derive deduplication, the bound,
  the generation check and the order of storing and announcing, and a table would be as correct as its model's
  copy of them.
- **Expiring cached values by age.** A slow value would be reloaded although nothing changed it, and a changed
  one would be served until its age ran out. Invalidation says exactly when a value is wrong.
- **One cache per principal.** It multiplies the slowest work by the number of users. Scope is applied to rows
  before the cache is read, so raw values can be shared.
- **An effect naming the levels it invalidates.** Effects name records; a second vocabulary beside them would
  drift from the first. The cache's own `invalidatedBy` narrows instead.
- **Announcing an `order` level's arrival without keys for cache-driven screens.** It makes the loader know what
  kind of client is listening. The cache's rule for open entries needs nothing from the server.
- **Sorting and filtering the held window on the client while the server's reply is in flight.** The held rows
  are a slice of the old order, so their local order is not a slice of the new one: rows would move twice.
- **A row type as the contract.** The columns would be fixed when the server is compiled, so a table whose
  columns depend on the session, or on data, could not be said, and every table would need a struct.
- **One generic table action addressed by a table id in its body.** See
  [A model's tables](#a-models-tables): authorisation, the catalog, the cache key and effects are all per
  action.
- **Column arrays on the wire.** The same bytes as row arrays, and every client structure is by row.
- **An unloaded marker in each cell.** Twenty markers a row for what one level name says.
- **A second, simpler pipeline for small tables.** Two orders on one screen, and the small table is the
  one that grows.
- **A second engine for document values.** The engine's keys are typed arrays built once; ordering document
  values directly is the per-comparison cost this replaces.
- **Detecting a paged `rows` expression and windowing it.** See
  [Screens over a server's pages](#screens-over-a-servers-pages).
- **Virtualizing columns in the Qt Quick backend alone.** The mount would still build every cell's node; the
  window has to be the mount's, as the row window is.
- **A list of `eq` entries for a value filter.** `QueryLimits::maxEntries` is 32, and 10,000 entries are 10,000
  comparisons a row.
- **One "all values" mode that lets two values share a column's match.** For values it is the same as
  `together`, since a cell has one value; for conditions it is two `any` groups, which AND already.
- **Trying every assignment for `together`.** Sixteen entries over sixteen columns are 16! assignments; a matching
  is found in a few hundred steps.
- **Paging in the database**, the model translating each query into the database's own query and the server
  answering only what the database cannot say. The table's order would then be the database's collation
  wherever the query was pushed down and the engine's where it was not, so two tables, or one table before and
  after a filter, would order the same text differently. Every feature (column groups, `together`, values with
  units, unloaded cells and undecided rows, distinct values by fold) would depend on what each store can
  translate, and every model would need a translator and a conformance run to show it agrees with `apply`.
  A `DataSource` read from the database and `apply` over it give one order and every feature, at the cost of
  holding a column's keys on the server while it is sorted or filtered, which the 100,000-row budget allows.
- **A quantifier on the existing `combine`.** `combine` says how entries meet in one cell; the quantifier counts
  columns. One name for both leaves the fourth example unsayable.
- **Groups as a merged virtual column.** Sorting and showing the columns one by one would be lost, and "exactly
  one" cannot be asked of a merged value.
- **One canonical unit for a mixed column.** There is no factor from kilograms to percent.
- **A per-cell dimension inside `QuantityCell`.** It grows every cell of every table by eight bytes; `Measure` is
  smaller than the largest alternative.
- **Reversing the dimension groups with the direction.** The groups' order is a convention, not a magnitude.
- **Percent as its own dimension.** 50 % and 0.5 would be different values that mean the same.
- **A column default unit for an operand with none.** The same text would mean different amounts on different
  columns; the menu supplies the unit instead.
- **Comparisons `Unknown` across dimensions.** `Unknown` means "not loaded yet"; a row would wait for a value
  that will never come.
- **A separate `notIn` operator.** `exclude` already negates an entry, with the engine's rule for empty cells.
- **Distinct values over the view, the column's own filter included.** A value the user unticked would vanish
  from the list and could not be ticked again.
- **A sibling action for distinct values.** It repeats the table action's scope and can drift from it.
- **A third click on a header clearing the sort.** It changes what a click does in documents that exist; the
  header menu removes a key.
- **Persisting the layout through a bound `layout` and a handler in every document.** Every document would repeat
  it, and the first page would be fetched before the handler ran. The binding stays for a document that wants
  the value.
- **Table state as preference points.** A point's value is loaded after the screen mounts, which is after the
  first query; and each application would need the points declared per table. One interface the table itself
  calls loads before the query and needs no declaration.
- **Two stores, preferences and table state.** One arrangement could be written to both and read from either.
  The default table store writes into the preferences store, so there is one.
- **The person in the key.** A client could ask for another person's state.
- **Merging concurrent saves.** See [Persisted state](#persisted-state).
- **Waiting for the state however long it takes.** A slow store would hold every table blank.
- **Text formatted on the server.** It cannot follow the user's locale or display unit without a request per
  change, and a column sorted by its text orders "10.00" before "9.00". The exact value crosses; the client
  formats it.
- **A rule language for tones in the description** (ranges, thresholds, colours). It would be a second expression
  language beside the document's, evaluated on a client that may lack the values a rule needs. A companion
  column computed by the model, or a document expression, says the same with what exists.
- **Select-all as a list of keys in server mode.** It fetches every key of up to 100,000 rows before the user
  has done anything with them, and is stale by the time the action runs. The view's filter and revision are
  what the user selected.
- **Retained rows frozen at their place.** A retained row that the sort would move is a row whose edit the
  user cannot see take effect; retaining exempts it from the filter and leaves the order truthful. Holding a row
  still is `ReorderPolicy::Deferred`'s, for the time an edit is in flight.
- **A rename as a removal and an insertion.** The selection, the focus and a failed mark would be lost on the
  row the user is looking at.
- **One `TableRows` implementation that holds either an engine or a window.** The two differ in whether a row can
  be absent, which every reader would then test; two implementations of one interface keep that in one place.

## Failure modes

| Situation | Behaviour |
|---|---|
| A level never arrives | Its cells stay unloaded and its rows stay undecided; nothing times out in the engine. A load that failed is named in the reply's `failed`, and the status line offers "Retry". |
| A `Unloaded` cell in a base-level column | `SourceContract`, naming the column. |
| A `columnLevels()` entry naming no column | Refused where the row's columns are reflected. |
| An event without keys, a `resync`, or a key that is neither integer nor text | `PageWindow::invalidate`: the shown pages are fetched again. |
| An event that precedes the page reply holding its rows | The keys are recorded and `owed` asks for them after the reply. |
| A keyed reply misses a requested key, or returns it at another view row | The window invalidates. |
| A keyed reply after a later fetch for the same key | That row is ignored. |
| A page reply older than the current revision | Accepted when newer than what is held; the page stays stale. |
| More event keys and probes than `maxKeys` | The window invalidates instead of asking for rows. |
| A keyed query to a model that does not translate keys | `Unavailable`; the window invalidates. |
| `announce` for an unregistered record | Refused; nothing is sent. |
| `announce` after the server is gone | The cache's sink finds no server and does nothing. |
| The loader throws, or returns no value for a key it was given | The keys are `Failed`: their cells stay unloaded, replies name the level in `failed` and not in `awaiting`, `LevelCacheStats::failedLoads` counts them, `onLoadFailed` is told and the keys are announced once. They are loaded again after `retryAfter`, by a `retry` query, or after an invalidation. |
| A row is invalidated while its load is in flight | The load's result is dropped for that row. |
| The cache is over `maxBytes` | The least recently used values no order lease holds are evicted; their cells are unloaded again and reload on demand. |
| A sort or filter on a level whose candidate rows cannot fit in `maxBytes` | `want` finds the keys times the mean value size over the room left, and the framework refuses the query with `Unavailable` naming the column; nothing is loaded. |
| A change of a record in `invalidatedBy` other than the row's own | The whole level is invalidated and the table's record is announced without keys. |
| The server restarts | The cache is empty; every level is unloaded until it is asked for. |
| An edit of an unloaded cell | Refused on the cell with a `FieldError`; nothing is sent. |
| A filter on a level no cache serves | Every row is undecided and `awaiting` names the level. |
| A reader of `Page::rows` that ignores `unloadedRows` | It shows unloaded members as the server left them. |
| A `rows` table whose sort column holds values of different kinds | Those cells are invalid and sort last; reported once at the `sort` path. |
| A `rows` table over more rows or cells than the client-mode bound | A warning naming the table; it still shows. |
| A model-backed table whose action does not take a `TableQuery` | Refused when the document loads. |
| `pageSize`, `page`, `group` or `rowSpans` on a model-backed table | Refused when the document loads. |
| A row not fetched yet in a server-mode table | The row draws as a placeholder row of the estimated height. |
| A column scrolled into range | Its cells are realized for the realized rows; its width may grow once. |
| A fetch fails | `PageWindow::fail`: the table shows the error until the next reply. |
| A document names a table the model does not have | Refused when the document loads, against the catalog. |
| A reply's `revision` differs from the description held | The client fetches the description again and resets its window. |
| A description with two columns of one id, a key that is no column, a level or group naming no column | The table's action answers `SourceContract`; nothing is shown. |
| A principal may run a table's action but read none of its columns | A page with no columns; the table shows its empty state. |
| A stored layout names a column the table lacks, or hides every column | The id is dropped; the first declared column is shown. |
| A sort key past the table's `maxSortKeys` | Refused, with a message in the table's status line. |
| A table whose `limits` set `maxSortKeys` above 32 | `SourceContract` when the table is first served. |
| A `oneOf` over `maxSetValues` | `LimitExceeded`; the menu sends the smaller side, and disables the list when both exceed it. |
| A distinct request to a model that does not translate it | `Unavailable`; the menu offers conditions only. |
| A values list while the column's level loads | The loaded values with `partial`; the list refreshes on each arrival. |
| A saved view with a filter on a removed column | That filter is dropped when the view is applied. |
| No table state store is injected | Tables keep their state while mounted and persist nothing. |
| The state's load is slower than the timeout | The table starts from defaults; the state is applied when it arrives unless the user changed the arrangement. |
| The state's load or save fails | Reported once through the session; the table keeps working, from defaults or with the arrangement it has. |
| Stored state of a version the framework does not know, or that is not JSON | Not used; reported; overwritten at the next change. |
| A save over the operator's quota, or refused by the store | `Refused`: the status line says so, and the table keeps its arrangement, unsaved. |
| A stored state whose segment is missing | A failed load: reported, and the table starts from its defaults. |
| A `views` save over a revision another window replaced | `Conflict`: the table loads the views again, applies its one change and saves once more. |
| Stored state names what the caller's description lacks | Not applied; carried and written back unchanged. |
| A `Measure` cell whose unit the registry lacks, or whose dimension the column does not allow | An invalid cell: it sorts with the empty cells and matches nothing but `notEmpty`. |
| A `Measure` cell whose base amount does not fit | An invalid cell. |
| An operand with an unknown unit, a dimension the column does not allow, or two dimensions in a `between` | `InvalidValue`, naming the column. |
| An operand with no unit on a column that allows no ratio | `InvalidValue`. |
| A `Measure` column and no unit registry in the services | `SourceContract`. |
| A stored display unit the registry no longer has | Dropped; the cells show in their own units. |
| A group of columns of different kinds, of fewer than 2 or more than 16 columns, or sharing a column with another declared group | Refused where the columns are read. |
| A group filter naming a group the table lacks | `UnknownColumn`, naming the group. |
| `together` with exactly 1 include entry, or more include entries than the group has columns | `InvalidSpec`. |
| `atLeast` or `exactly` with n below 0 or above the group's column count | `InvalidSpec`. |
| A user-formed group loses a column | The column leaves the group; under two columns the group and its filter are dropped. |
| A query's `columns` names a column the table lacks, or one the caller may not read | `UnknownColumn` or `ColumnNotReadable`; the client fetches the description again and asks once more. |
| A column fetch whose keys are not the held page's | The rows moved between the two fetches; the window invalidates. |
| A page reply whose total differs from the window's | The window adopts it and marks its other pages stale. |
| A keyed reply that shows a probe, or any held key, at another view row; an unheld key inside a held page; another total | The window invalidates. A check that fires because two replies crossed costs one page fetch. |
| A cell expression reads a column the window does not hold | The field reads as absent, the column is fetched, and the cell is evaluated again when it arrives. |
| An expression reads the row in a way the loader cannot follow | The table fetches every column; a warning names the node. |
| A fetch for a page the user scrolled away from | Cancelled; its reply, if it comes, is ignored. |
| `table::rowScoped` with an empty `scope` | `SourceContract` when the table is first served. |
| `updatesTable<fn>` on an action whose result holds no `ChangedRows<fn>` | A registration error when the registries resolve. |
| A second order use of a level that does not fit beside the first | `Unavailable` while the first's lease lives. |
| Leased values outgrow `maxBytes` while they load | The cache stops loading for order, drops the lease and announces the record without keys; the next query is refused. |
| A range selection over more than `maxRangeRows` rows, or a copy of more than `maxCopyRows` | Refused, with a message in the status line; nothing is selected or copied. |
| A unit id with white space, or one that begins like a number | Refused by `UnitRegistryBuilder`. |
| A level cache read under a registry of another lineage | The level is invalidated: its unit numbers mean nothing there. |
| A `rows` table that sets `persist` and has no `id` | Refused when the document loads. Without `persist` it shows, and persists nothing. |
| A refusal on a table whose document sets `status: false` | Shown through the session's notifications. |
| A companion column that names no column, or is not `Text`; `decimals` or `unit` on a `Measure` column | `SourceContract` when the description is built. |
| A companion's tone that is not a tone name | No tone; reported once for the column. |
| `resolve` under `Exact` after the view's membership changed | `ViewChanged`; the client keeps the selection and asks the user to confirm on the view as it is. |
| A selection of the view larger than `maxSelectedRows` | `LimitExceeded`. |
| Select-all while rows are undecided or a level is awaited | Refused, with a message in the status line. |
| An action that retains more rows than `maxRetained` | None of its rows are retained; the status line says so. |
| A `ChangedRow::from` the client does not hold | The new key is fetched as any changed key; nothing moves. |
| A gesture on a header while `headerEnabled` is false | Refused, with a message in the status line. |
| A `publish` the authoriser refuses, or a store whose `shares()` is false | Refused with a message; over a store that does not share, publishing is not offered. |
| A user's own `save` of the `shared` part | Refused. |
| A `rowLock` companion that names no column, or is not `Text` | `SourceContract` when the description is built. |

## Open questions

- **Retaining the rows of an action over a whole view.** `retain` carries keys, so an action that changes more
  than `maxRetained` rows (a replacement over every visible design) retains none of them. Carrying a set the
  server holds for the session, named in the query, would lift the bound; whether that is needed is settled by
  the first application that meets it.
- **A level cache in local mode.** With no server deriving changes, the application invalidates a cache itself.
  A hook for it belongs with local mode's effects.
- **Frame timing in CI.** Whether `QQuickRenderControl` with the software backend gives stable step times on the
  CI image is settled by the first baseline.
- **The shard count of a level cache.** 256 shards make a batch of 64 keys copy about a quarter of the cache's
  key entries at 100,000 rows; L6 measures it, and the count is a constant to tune from that figure.

## Neighbouring specs

This document is the design. The specs below describe the code, so each takes its part when the step that
builds it lands, and until then says nothing of it; a link from here to one of them is to the file, not to a
section that does not exist yet.

| Spec | Takes | With step |
|---|---|---|
| [`../core/registry.md`](../core/registry.md) | `BRIDGE_REGISTER_TABLE` and its options, the table's record and `TableOf`, the result walk's slot for `ChangedRows`, `openMembers`, `partialMember`, an opaque holder | 1, 2d |
| [`../core/backend.md`](../core/backend.md), "Fan-out" | `ServerConfig::tableWork`; `RemoteServer::announce`, `attach`, `IChangeObserver`; the view cache | 1, 2c |
| [`../ui/interpreter.md`](../ui/interpreter.md), "Queries and the cache" | The rules for open, partial and opaque entries; `Session::read` and `watchRecord` | 2d |
| [`../ui/interpreter.md`](../ui/interpreter.md), "Not yet lowered" and the rows' pipeline | The entry for a model-backed `table` goes; the lowering onto `TableRows` is described; the "Rows' pipeline" and "Groups" rules keep what the lowering does around the engine's order | 4, 6 |
| [`../ui/document.md`](../ui/document.md), the `table` kind | `table`, `status`, `persist`, `layout`, `configurable`, a column's `kind` and `locked`; `filters` on every table; `cellMenu`, `headerMenu`, `retainChanged`, `headerEnabled`; a column's display parts, `tone` and `tooltip`; the element values this design names | 4, 6, 7, 9a, 9b |
| [`../ui/preferences.md`](../ui/preferences.md) | That table state has its own model and is not bounded by `maxValueBytes`; the `columns` and `saved` points | 4 |
| [`../qt_quick/backend.md`](../qt_quick/backend.md) | The column window, cell pools, pinned columns, the status line; the menus and dialogs | 5, 7 |
| [`../forms/views.md`](../forms/views.md) | `x-level`, `x-column-group` and `x-measure` on a schema property, and the `measure` kind | 2a, 3 |
| [`engine.md`](engine.md) | Its "Not here yet" entries for the model-backed table and the single-threaded steps; `ColumnLayout`'s fitting rule | 4, 6, 7 |
| [`engine.md`](engine.md), `database_pages.md` | `planQuery`, `QueryPlan` and the conformance helper leave `engine.md`; `database_pages.md` is removed | 0a |
| [`editing.md`](editing.md) | Editing cells in place, grids, spans, header bands and summary rows, on this pipeline | its own order |

## Delivery order

The design lands in this order. Each step is shippable and green alone, and carries the gates named. A count
gate runs as an ordinary test; a timing is recorded in [Performance](#performance) when the step lands.

| Step | Lands | Gates and records |
|---|---|---|
| 0 | The fixture's rows, with nothing that does not exist yet: the column-store `DataSource` and its generator (`bench::WideTableSource`), and a baseline of what the code does today. The table tests are built for WebAssembly and run under Node. | Gate: S3. Recorded: S1 and S2 with every cell loaded; S7 as ten `apply` calls; S8; B1 at 10,000, 20,000 and 100,000 rows; B2; B5; U1 to U5; W7 as a typed page; F2 to F5 on a `rows` table of 2,000 × 300, the size a mount without a column window holds |
| 0a | Store paging goes: `QueryPlan`, `planQuery`, `PlanUse` and `plan_conformance.hpp`, their tests, and the example models that page in SQLite, which serve their rows as a `DataSource` instead. | The table, net and example suites green without them |
| 1 | The generic table contract: `TableDescription`, `ServedTable` over a `source`, `BRIDGE_REGISTER_TABLE`, its options and its two actions, the table's record with `updatesTable`, `changesTable` and `ChangedRows`, the generic page with every cell encoding, `TableQuery::columns`, `table::typed<Row>`, requests answered on `tableWork`. The fixture becomes the registered model `bench::WideTableModel` with its three tables and no slow level. | W1, W2, W5, W7; W6's bytes and codec |
| 2a | The engine's part of slow columns: the `Unloaded` cell and its rank, the three-valued filter and the undecided set, `ColumnInfo::level`, `levelsNeeded`, `LevelLoaded`, `unloadedRows` on `PageRows` and the typed `Page`. | S1 to S6 |
| 2b | The wire's part: `unloadedRows`, `undecided`, `awaiting`, `failed` and `indices` on the generic page; `TableQuery::keys` and the keyed reply with its totals. `TableQuery::columns` is step 1's. | W3, W4 |
| 2c | The server's caches and the push: `LevelSource`, `LevelCache` with leases, failures and immutable snapshots; `RemoteServer::announce` and `attach`; the view cache. The fixture gains its slow level. | L1 to L8, S7, S9, C1 to C3 |
| 2d | The client's part: `PageWindow` under changes, with columns, probes and `superseded`; `Session::read` and `watchRecord`; the query cache's rules for open, partial and opaque entries. A table served over `apply` and read through a `PageWindow` then shows a first page before a slow level loads, shows its cells as unloaded, loads a level only when a named column needs it, and updates exactly the rows that arrived. | K1 to K4, K6, K7, E1 to E4 |
| 3 | The engine's configuring additions: `oneOf`, `distinctValues`, column groups with quantifiers and `together`, `Measure` and the unit registry, `ColumnLayout`'s widths, pinning, fitting rule and stored shape. | D1 to D6, G1 to G7, M1 to M5 |
| 4 | `TableRows` and the lowering of a model-backed `table` onto `PageWindow`, from the description, with the status line's counts. The four behaviours of 2d then hold on a screen. `ITableStateStore`, the framework's stores and the load before the first rows land here, for sort and filters, so the rule is built into the first lowering. | U6 (E1 to E3 on a screen), K5, K8, F6, F7, T1, T3 to T5; W6's one request; F1 recorded |
| 5 | The column window in the mount and in both backends: extents, pinned columns, cell pools, the frame budget, focus, keys and counts under windowing; range selection in server mode. **Exit criterion:** U8, the same scrolls in the WebAssembly build in a browser, within F3's and F4's frame budgets. | F1 to F5, F8 to F10, U7, U8, D8 |
| 6 | `rows` tables on the engine through `ValueRowsSource`; the interpreter's own sort and page go. | U1 to U4; B1's 20,000-row step bound |
| 7 | The configuration interface: header and table menus, column order, visibility, pinning and width, persisted with the rest of the table's state; sort chains; the filter menu with values, conditions, groups and units; copy. Screenshots of existing screens change here, as intended. | D7, D9, B3, B4 |
| 8 | Collections through the same lowering. | U5 |
| 9 | Saved views, as the `views` part of the persisted state, with the conflict rule; shared views and the default first. | T2 |
| 9a | A column's display and `ui::formatCell`; companion columns for tone and tooltip; header units. It may land before step 4; if it lands after, step 4's lowering formats through a stand-in that 9a replaces. | The display and companion tests of [Tests](#tests) |
| 9b | Acting on rows: `RowSelection`, `resolve` and `Table.<id>.resolve`, `viewRevision`, retained rows, `ChangedRow::from`, busy, failed and locked rows, the application's menu entries, `headerEnabled`. Needs step 4. | The selection, store selection, retained, key-change, busy, locked, menu and disabled-header tests of [Tests](#tests) |
| 9c | `maxSortKeys` configurable up to 32, carried in the description. | "A raised sort bound" |
| 10 | As measured: repairing a cached view in place of computing it again, local work when a window holds the whole view, a persistent tier under the level cache. | The figures that justify each |
