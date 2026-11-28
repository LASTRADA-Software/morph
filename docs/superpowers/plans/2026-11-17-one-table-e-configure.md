# The One Table, Part E: Configuring, Collections, Saved Views, the Sort Bound — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a user arrange any table from its header and its table menu (column order, visibility, pinning, width, sort chains, filters by values and conditions, column groups, display units, copy, range selection in server mode, a status line that shows every refusal), put collections on the same lowering, keep saved and shared views, and raise the sort bound to 32 keys.

**Architecture:** The gestures are pure functions in a new UI header (`include/morph/ui/table_config.hpp`) over `table::ColumnLayout`, `ui::SortChain` and `table::FilterSpec`, tested without a backend. The backend contract grows header menus, sort chains, column moves and resizes, filter buttons, group headers, the status line and the clipboard; both backends and `RecordingBackend` implement them and one set of conformance cases holds all three to it. The interpreter's table lowering (built by Tasks 9 and 11) reads the new document keys and wires gestures to `TableRows`; the filter menu, the column chooser and the sort dialog are view trees the lowering mounts in dialogs, so both backends draw them with widgets they already have. Collections lower through the same table surface; saved views are the `views` part of persisted state, with an operation-and-conflict rule and a `publish` action for shared views.

**Tech Stack:** C++23, header-only morph plus `src/ui`, `src/qt_quick`, `src/tui`; glaze; Catch2; Qt 6 Quick (offscreen `morph_qt_quick_tests` only); the terminal backend (`morph_tui_tests`).

**Spec:** `docs/spec/table/table.md` — "Configuring a table" (all subsections: "Columns: order, visibility, pinning, width", "Sort chains", "Filters from the header", "Column groups" (the menu), "The status line", "Copy", "Persisted state" with "The framework's stores", "Saved views"), "Keyboard, focus and assistive technology" (range selection in server mode), "Collections and master-detail", "Values with units" ("The values list and the menu", "Display"), the matching rows of "Tests", invariants 21, 26, 31, gates D7, D9, B3, B4, U5, T2, and the step 7, 8, 9 and 9c rows of "Neighbouring specs" and "Delivery order". Roadmap: `docs/superpowers/plans/2026-11-17-one-table.md` (Tasks 12, 13, 14, 16).

Part E of 7; index: 2026-11-17-one-table.md

## Global Constraints

Everything in the roadmap's Global Constraints and its standard task loop applies to every task here. In addition:

- Limits, verbatim from the spec: a chain holds at most `maxSortKeys` keys, 8 by default and up to 32 (`TableCapabilities::maxSortKeys`, a `rows` table's literal `maxSortKeys`); `SessionOptions::maxRangeRows = 10'000`, `maxCopyRows = 10'000`; `QueryLimits::maxKeys = 1000` (rows per keyed copy request), `maxSetValues = 10'000`, `maxGroupColumns = 16`; `DistinctRequest::limit = 200`; a text filter and a values search are debounced 250 ms; a status message holds for six seconds or until the user's next gesture on the table; `tableStateDebounce = 500 ms`; `segmentBytes = 12 KiB`.
- Status line and menu copy is exact, English, with counts grouped by `ui::groupedCount` with the session's numeric grouping separator (`FormatContext`; "10,000" in English):
  - `A table sorts by at most {n} columns` · `A range selects at most {n} rows` · `A copy holds at most {n} rows`
  - `Copied {n} rows` (`Copied 1 row`), and with unloaded cells `Copied {n} rows, {m} cells were not loaded` (`1 cell was not loaded`)
  - `{label} is locked` · `At least one column stays visible` · `This table's columns cannot be moved` (`hidden`, `pinned`, `resized`)
  - `A group has 2 to 16 columns` · `A group's columns hold one kind of value` · `{label} is already in {group}`
  - `Too many values to list; filter with a condition` · `Too many dates to list; filter with a condition` · `The list may grow while data loads` · `{n} cells could not be read` (`1 cell could not be read`)
  - header accessible description, lower case as the spec writes it: `sorted ascending` / `sorted descending`, and with two or more keys `sorted ascending, key 2 of 3`; header mark ` ▲` / ` ▼`, and with two or more keys ` ▲2`; a filtered header's accessible name `{label}, filtered`
  - menus and dialogs: `Sort ascending`, `Sort descending`, `Add to sort`, `Remove from sort`, `Filter…`, `Filter {group}, {k} columns…`, `Move left`, `Move right`, `Pin column`, `Unpin`, `Hide column`, `Display unit`; `Columns…`, `Sort…`, `Clear all filters`, `Reset columns`, `Copy`, `Copy with headers`; `(Select all)`, `(Blanks)`, `Group columns…`, `Remove group`, `Add key`; views (Task 14): `Views`, `Save view…`, `Rename view…`, `Remove view`, `Set as default`, `Start from the default view`, `Shared views`, `Publish view…`, `Save as my view`, `{name} (modified)`, `Saved view {name}`, `A view named {name} exists`, `Views could not be saved: another window changed them at the same time`, `Published view {name}`, `This view could not be published: {reason}`
- Test trees. `morph_tests` and `morph_table_tests` build in `cl-debug`. `morph_qt_quick_tests` and `morph_tui_tests` need Qt Quick and the terminal backend, which no checked-in Windows preset turns on: configure once with `cmake --preset cl-qt-debug -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TUI=ON`, build with `cmake --build build/cl-qt-debug --target <target>`, run with `ctest --test-dir build/cl-qt-debug -R "<pattern>" --output-on-failure` (Qt Quick cases run with `QT_QPA_PLATFORM=offscreen`, set by their `catch_discover_tests`). `morph_net_tests` is POSIX-only (`MORPH_BUILD_NET` is ignored on Windows): its cases build and run on the `linux-everything` preset, `ctest --test-dir build/linux-everything -R "<pattern>"`. Every case that can run in `morph_tests` is placed there, so a Windows tree runs it.
- Timings marked (P) or (M) in the spec are recorded into `docs/spec/table/table.md` "Performance" with the date and the machine, from a `cl-release` (or `linux-everything` for `tests/net`) build; a gate is an ordinary test.
- No GUI application is launched. The one step that grabs screens (Task 12f) runs the example client offscreen through the repository's grab scripts and is run by the user, not by an agent.

## Review Focus

1. **Hiding a column the view is sorted or filtered by.** Arranging is not ordering (invariant 21): the sort and the filter keep applying, the next query still sorts and filters by the hidden column while no longer naming it in `columns`, the header mark and the filtered state reappear when it is shown, and the chooser lists it unticked. Test goes to Task 12c ("hiding a sorted and filtered column changes no row").
2. **A chain at its bound.** With `maxSortKeys` keys held, `Shift`+click on a column already in the chain flips its direction and is not refused; on any other column it is refused and the chain is unchanged; "Add to sort" is disabled and "Remove from sort" frees a place. Tests go to Task 12a (the gesture) and Task 12c (the message).
3. **Exactly at a row bound.** A range of exactly `maxRangeRows` (10,000) rows selects them all; 10,001 is refused and selects nothing (Part D's Task 10a rule, held here at its edge). A copy of exactly 10,000 rows copies; 10,001 is refused. Test goes to Task 12f ("range and copy at their bounds").
4. **Cell text that breaks a line or a column.** A cell holding `a\tb\r\nc`, a lone `\r` and a label with a tab copy as one line of tab-separated fields: each tab, `\r\n`, `\r` and `\n` is one space, headers included. Test goes to Task 12a ("copy text: tabs and line breaks become one space").
5. **A view name already taken.** Saving a view under a name the user already has replaces that view (one `replace` operation, still one view of that name); renaming a view onto a taken name is refused with `A view named {name} exists` and changes nothing; the same holds when the taken name arrived from another window between load and save (the operation is applied to what is stored now). Test goes to Task 14b ("views: a taken name is replaced on save and refused on rename").

---

## Interfaces this part consumes from Parts A–D

Names the roadmap pins are used as it gives them. Where the roadmap leaves a name open, this plan uses the one below; the executor of each consuming task checks it against the landed code first and, if it differs, uses the landed name and corrects this plan.

| From | Pinned by the roadmap | Assumed here (not pinned) |
|---|---|---|
| Task 1 | `table::TableColumn{info, label, group, sortable, filterable, shown}`, `ColumnGroupInfo{id, label, columns}`, `TableCapabilities{keys, distinct, maxSortKeys = 8}`, `TableDescription`, `GenericRow`, `GenericPage` | — |
| Task 2 | `ServedTable{description, source, levels, mayRead, scope, services, limits}`, `BRIDGE_REGISTER_TABLE` with `Table.<id>` and `Table.<id>.describe` | the index's answer path `table::detail::answerQuery(ServedTable const&, TableQuery const&, AnswerContext const& = {})` in `description.hpp`; Task 16 makes it and the `describe` handler take their description from `table::servedDescription` |
| Tasks 3–5 | `struct Unloaded`, `TableQuery::{columns, keys}`, `Page::indices` | `TableQuery::retry` (bool), from the level cache's rule 11 |
| Task 8a | `FilterEntry::oneOf`, `DistinctRequest`, `DistinctValue`, `DistinctValues`, `DateBucket`, `Engine::distinctValues`, `TableQuery::distinct`, `Page::distinct` | — |
| Task 8b | `GroupMode`, `ColumnCount{Kind, n}`, `GroupFilter{group, columns, mode, match, combine, include, exclude}` | — |
| Task 8c | `ColumnKind::Measure`, `UnitInfo`, `DistinctValue::{dimension, unit}` | — |
| Task 8d | `ColumnLayout::setWidth(id, std::optional<int>)`, `widths()`, `setPinned(n)`, `pinned()`, `value()`, `apply(value)` | as Part C writes it: `table::ColumnLayoutValue{order, hidden, pinned, widths, groups, units}` (`groups` a `std::vector<ColumnGroupInfo>` of user-formed groups, `units` column → dimension → unit id), `std::size_t pinned()` counting the first visible columns, `widths()` a `std::map<std::string, int> const&`; `apply` and `rebuild` fit groups and units |
| Task 9 (Part D) | `ui::SortChain`, `TableCountsView`, `TableRows` (`total`, `row`, `unloadedLevels`, `visible`, `keysOf`, `setSort`, `setFilter`, `counts`, `onChange`), `EngineRows`, `WindowRows`, `TableStateKey`, `StoredTableState`, `SaveOutcome`, `SaveResult`, `ITableStateStore`, `MemoryTableStateStore`, `PreferencesTableStateStore`, `TableStateModel` (`morph.tableState`), `SessionOptions::tableState` | as Part D writes them: `EngineRows(std::shared_ptr<table::DataSource>, RecordOf, table::EngineOptions)`; `WindowRows::retry()`, `WindowRows::description()`; `ui::countsText(TableCountsView const&, std::int64_t selected, FormatContext const&)`; `ui::detail::standInCellText(Cell const&, table::TableColumn const&)`; `ui::TableStatus{counts, state, retry, message, shown}`, `Table::status`, `Table::onRetry`, `TableWidget::setStatus`, `setOnRetry` (the status line, its counts, Retry and the `status` key are Part D's); the status line's message channel, Task 9d's per index decision 11: `void ScreenCore::tableMessage(ElementState& table, std::string text)` in `src/ui/interpreter.cpp` and `inline constexpr std::chrono::seconds kStatusMessageHold{6}` (sets `TableStatus::message` and restarts the hold on the session's scheduler; under `status: false` sends `text` once to `InterpreterHost::notify(text, "warning")`; an empty `text` clears the message and notifies nothing — this part's gestures clear it that way); `ui::TableStates` (`include/morph/ui/table_state.hpp`, 9c: `kept`, `load`, `changed`, `flush`, `onSaveFailed`, and, per index decision 32, `store() -> table::ITableStateStore&` and `keep(key, state)`, which 14b saves views through; loads made in one turn of the owner go out as one `TableStateLoad`) and `Session::tableStates() -> TableStates*`; `SessionOptions::application`; `table::StateDocument{version, layout, sort, filters, view, carried}`, `readState`, `writeState`, `fitState(StateDocument, TableDescription const&, FitReport&)`; `MemoryTableStateStore(exec::IExecutor&)` with revisions `"1"`, `"2"`, …; `ui::TableStateLoad{keys}`, `TableStateSave{key, expected, upload, offset, text, last}` → `TableStateSaved{result}`, `registerTableState(std::shared_ptr<IPreferencesStore>, dispatcher, registry, TableStateOptions)`; `PreferencesTableStateStore(Dispatcher&, exec::IExecutor& owner)`; test support `tests/ui_table_support.hpp`: `tableCatalog()` (the corpus catalog plus `Table.samples` and `.describe` on `Lims_SampleModel`), `samplesDescription(int columns)` (`id` Integer key, `name` Text, then `c3…` Decimal), `describeJson`, `pageJson(offset, count, total, columns, kind, undecided, awaiting)`, `GatedTableStateStore`, the harness `ui::testing::TableHarness` (9d, index decision 42; its constructor taking a `std::shared_ptr<table::ITableStateStore>` sets `tableState` and `application = "lims"`), `elementValue(h, id, name)` (an element value as JSON text), `h.replyDescribe()`, `h.replyPage(json)`, `lastQuery(h)`, `statusOf(h)` |
| Task 10 (Part D) | the column window; `TableWidget`'s range report by view rows | as Part D writes them: `ui::TableColumn::id`, `ui::TableArrangement{visible, pinned, widths}`, `Table::arrangement` (applied without rebuilding rows, gate D8), `TableWidget::setOnSelectRange(std::function<void(std::size_t anchorRow, std::size_t focusRow, SelectionChange)>)`, `RecordingBackend::selectTableRange(id, anchor, focus)`, `SessionOptions::maxRangeRows`; range selection through `keysOf` and its refusal are Task 10a's |
| Task 11 (Part D) | `ValueRowsSource`, rows tables on `EngineRows`; the internal lowering of a table node onto `TableRows` | a column part's `kind` (`ValueColumn::kind`) and the refusal of `persist` on a table with no `id` are Task 11's |

---

## Task 12: The configuration interface (table.md step 7)

**Spec:** "Configuring a table" (all), "Values with units" ("The values list and the menu", "Display"), "Keyboard, focus and assistive technology" (range selection in server mode), "The document `table` node on the engine" (rows for `layout`, `status`, `persist`, `filters`, a column's `kind`, `locked`, `filterable`). Gates D7, D9, B3, B4. Six commits, each its own reviewer gate.

### Task 12a: Gestures, menus, value filters and copy text, without a backend

**Files:**
- Create: `include/morph/ui/table_config.hpp`, `tests/test_ui_table_config.cpp` (add to `morph_tests` in `tests/CMakeLists.txt`, beside `test_ui_table.cpp`)
- Modify: `include/morph/table/engine.hpp` (`ColumnLayout`, as Task 8d leaves it: the mutators for user-formed groups and display units), `tests/table/test_table_columns.cpp`, `CMakeLists.txt` (list the new header in the `morph` header set)
- Docs: `docs/spec/table/engine.md` ("`ColumnLayout`": the group and unit mutators)

**Interfaces:**
- Consumes: Task 8d's `ColumnLayout` and `ColumnLayoutValue`; Task 8a's `FilterEntry::oneOf`; Task 8b's `GroupFilter`, `GroupMode`, `ColumnCount`; `table::SortKey{column, dir}`, `table::SortDirection`, `table::ColumnKind`.
- Produces (namespace `morph::table`, `engine.hpp`; the stored shape and its fitting are Task 8d's):
  - `ColumnLayout::formGroup(std::string label, std::vector<std::string> columns) -> std::expected<std::string, TableError>` (returns the new id `g1`, `g2`, … the first unused; `InvalidSpec` for fewer than 2 or more than 16 columns, an unknown column, or a column already in a user-formed group), `removeGroup(std::string const& id) -> bool`, `groups() -> std::span<ColumnGroupInfo const>`, `setUnit(std::string const& column, std::string const& dimension, std::optional<std::string> unit) -> bool` (`nullopt` removes it), `units() -> std::map<std::string, std::map<std::string, std::string>> const&`; all of them read and write the same members `value()` and `apply` carry.
- Produces (namespace `morph::ui`, `table_config.hpp`):
  - `enum class SortGesture : std::uint8_t { Click, Extend, Ascending, Descending, Add, Remove }`; `struct SortChange { SortChain chain; std::optional<std::string> refusal; }`; `[[nodiscard]] SortChange applySortGesture(SortChain const& chain, std::string const& column, SortGesture gesture, std::size_t maxSortKeys)`
  - `enum class Configure : std::uint8_t { None = 0, Reorder = 1, Hide = 2, Pin = 4, Resize = 8, All = 15 }` with `operator|`, `operator&` and `[[nodiscard]] constexpr bool allows(Configure set, Configure what)`
  - `struct LayoutColumn { std::string id; std::string label; bool locked = false; table::ColumnKind kind = table::ColumnKind::Text; }`
  - gestures `MoveColumn{id, int by}`, `MoveColumnTo{id, std::size_t position}`, `HideColumn{id}`, `ShowColumn{id}`, `PinColumn{id}`, `UnpinColumn{id}`, `ResizeColumn{id, std::optional<int> units}`, `ResetColumns{}`, `SetDisplayUnit{id, dimension, std::optional<std::string> unit}`, `FormGroup{label, columns}`, `RemoveGroup{id}`, as `using LayoutGesture = std::variant<…>`; `struct LayoutResult { bool changed = false; std::optional<std::string> refusal; }`; `[[nodiscard]] LayoutResult applyLayoutGesture(table::ColumnLayout& layout, std::span<LayoutColumn const> columns, Configure allowed, LayoutGesture const& gesture)`
  - The tick model. Under a search, "(Select all)" means the values the search shows (spec 1766-1767), which the menu does not hold when they pass a page, so a tick is recorded as an operation and resolved only at Apply:
    - `struct TickOp { std::string search; std::optional<std::string> operand; bool ticked = true; }` — `operand` set: one value's box; unset: "(Select all)" under `search` (empty `search`: every value)
    - `struct ValueTicks { bool ticked = true; std::vector<TickOp> ops; std::optional<bool> blanks; }` — `ticked` is every value's state before the first operation (true with no filter on the column); later operations win
    - `struct ResolvedTicks { bool ticked = true; std::map<std::string, bool> covered; std::optional<bool> blanks; }` — `covered`: the final state of every value an operation reached, by operand
    - `[[nodiscard]] ResolvedTicks resolveTicks(ValueTicks const& ticks, std::function<std::vector<std::string>(std::string const& search)> const& valuesUnder)` — `valuesUnder` returns the operands a search shows; it is synchronous, and `distinctValues` on a `WindowRows` is not: at Apply the menu first awaits, for each distinct non-empty `search` among the operand-less ops, every page of `distinctValues` at `limit = QueryLimits::maxLimit` (1000) into a `std::map<std::string, std::vector<std::string>>`, and only then calls `resolveTicks` with a `valuesUnder` that reads the map; an op with an empty search and no operand clears `covered` and sets `ticked`. A menu reopened on a stored values filter seeds its `ValueTicks` from it: an `include` `oneOf` is `ticked = false` plus one `TickOp{.search = "", .operand = member, .ticked = true}` per member, an `exclude` `oneOf` is `ticked = true` plus one such op with `ticked = false` per member, and `blanks` is `true` for an included `isEmpty`, `false` for an excluded one
    - `struct ValueFilterPlan { enum class Side : std::uint8_t { All, Include, Exclude, Nothing, Disabled } side = Side::All; std::size_t members = 0; bool needsComplement = false; }`; `[[nodiscard]] ValueFilterPlan planValueFilter(std::int64_t distinct, ResolvedTicks const& ticks, std::size_t maxSetValues)`
    - `enum class ValueForm : std::uint8_t { Set, Ranges }` (`Ranges` for date buckets, whose operand is `a..b`); `[[nodiscard]] std::optional<table::ColumnFilter> valueFilter(ValueFilterPlan::Side side, std::vector<std::string> members, std::optional<bool> blanks, ValueForm form = ValueForm::Set)`
  - Values and conditions are exclusive per column: one `ColumnFilter` has one `combine` (`filter.hpp:86-93`), so `[oneOf, isEmpty, condition]` under `All` would match nothing. `enum class FilterPart : std::uint8_t { None, Values, Conditions }`; `[[nodiscard]] FilterPart classifyFilter(table::ColumnFilter const& filter, table::ColumnKind kind)` — `Values` when the filter is a values shape: `combine = Any` and `include` is `{oneOf}`, `{oneOf, isEmpty}`, one or more `between` entries (a date column's buckets) with or without `isEmpty`, or a lone `{isEmpty}` on a column that is not `Custom`, and `exclude` the same shapes with `include` empty; a lone `isEmpty`/`notEmpty` on a `Custom` column is `Conditions` (it has no values list); anything else not empty is `Conditions`
  - `[[nodiscard]] std::optional<std::string> canonicalOperand(std::string_view typed, table::ColumnKind kind, LocaleFacts const& locale)` — the wire form of a typed operand: a number in the locale's form (`"1.234,5"` under a comma locale) becomes a decimal with a point and no grouping (`"1234.5"`), through `render::normalizeLocaleNumber`; a date is the picker's ISO text; text is unchanged; `nullopt` when it does not read. 12a landed before 8c, so it ships without the `Measure` branch; Part C's Task 8c, Step 8a, adds the units parameters, the branch, `Measure` in `conditionOps` and their tests. `ui::SortChain` is declared here (`using SortChain = table::SortChain;`), and Part D's Task 9a reuses it
  - `enum class GroupMatch : std::uint8_t { AnyColumn, EveryColumn, AtLeast, Exactly, Together }`; `[[nodiscard]] std::expected<table::GroupFilter, std::string> groupValueFilter(std::string group, std::size_t groupColumns, std::vector<std::string> ticked, std::optional<bool> blanks, GroupMatch match, std::int64_t n)`
  - `enum class ConditionOp : std::uint8_t { Equals, NotEquals, Contains, BeginsWith, EndsWith, Less, LessOrEqual, Greater, GreaterOrEqual, Between, IsEmpty, IsNotEmpty }`; `[[nodiscard]] std::vector<ConditionOp> conditionOps(table::ColumnKind kind)`; `[[nodiscard]] std::string_view conditionLabel(ConditionOp op, table::ColumnKind kind)`; `[[nodiscard]] table::FilterEntry conditionEntry(ConditionOp op, std::string operand, std::string second, bool matchCase)`
  - `enum class MenuCommand : std::uint8_t { SortAscending, SortDescending, AddToSort, RemoveFromSort, Filter, FilterGroup, MoveLeft, MoveRight, PinColumn, UnpinColumn, HideColumn, DisplayUnit, Columns, SortDialog, ClearAllFilters, ResetColumns, Copy, CopyWithHeaders }` (Task 14 appends its view commands); `struct TableMenuEntry { MenuEntry entry; MenuCommand command; std::string argument; std::vector<TableMenuEntry> items; }`; `struct MenuGroup { std::string id; std::string label; std::size_t columns = 0; }`; `struct HeaderMenuInput { LayoutColumn column; bool sortable, filterable; std::optional<std::size_t> sortPlace; std::size_t chainSize, maxSortKeys, visibleIndex, visibleCount, pinned; std::vector<MenuDimension> dimensions; std::vector<MenuGroup> groups; Configure allowed; }` with `struct MenuDimension { std::string dimension; std::vector<std::string> units; std::string chosen; }` (`units`: unit ids in the description's order; `chosen`: the layout's display unit for the dimension, empty when the cells show their own). A column with a non-empty `dimensions` gets, last in its header menu, `Display unit` with a submenu: per dimension and unit one checkable entry labelled `{dimension}: {unit}` (`mass: g`), checked when it is `chosen`, with `command = DisplayUnit` and `argument = "{dimension}\x1F{unit}"`; choosing the checked entry clears the display unit (`SetDisplayUnit{id, dimension, nullopt}`). `FilterGroup`'s `argument` is the group's id; every other command's is empty; `struct TableMenuInput { bool anyFilter; std::vector<MenuGroup> groups; Configure allowed; }`; `[[nodiscard]] std::vector<TableMenuEntry> headerMenu(HeaderMenuInput const&)`; `[[nodiscard]] std::vector<TableMenuEntry> tableMenu(TableMenuInput const&)`; `[[nodiscard]] std::vector<MenuEntry> menuEntries(std::span<TableMenuEntry const>)`; `[[nodiscard]] TableMenuEntry const* chosen(std::span<TableMenuEntry const>, std::span<std::size_t const> path)`
  - `struct CopiedText { std::string text; std::size_t unloaded = 0; }`; `[[nodiscard]] CopiedText copyText(std::span<std::string const> labels, std::span<std::vector<std::optional<std::string>> const> rows)` (empty `labels`: no header line; `nullopt`: an unloaded cell)
  - `[[nodiscard]] std::string groupedCount(std::int64_t n, std::string_view separator = ",")`
  - `namespace tableText`: `sortBound(std::size_t)`, `rangeBound(std::size_t, std::string_view separator = ",")`, `copyBound(std::size_t, std::string_view separator = ",")`, `copied(std::size_t rows, std::size_t unloaded, std::string_view separator = ",")`, `locked(std::string_view label)`, `notConfigurable(Configure what)`, `lastVisible()`, `tooManyValues()`, `tooManyDates()`, `mayGrow()`, `unreadable(std::size_t, std::string_view separator = ",")` (its count is grouped as the others are), each `-> std::string`, the exact copy of Global Constraints
  - (The six-second hold, `kStatusMessageHold`, is Part D's Task 9d's, with the channel that uses it; this header does not declare it.)

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_config.cpp` (tag `[ui][table][config]`), each preceded by its mutation comment:

```cpp
// Mutation: Extend treated as Click (Shift ignored).
TEST_CASE("sort gesture: a click replaces the chain; Shift adds a key or flips one in place", "[ui][table][config]") {
    using table::SortDirection::Ascending; using table::SortDirection::Descending;
    ui::SortChain const two{{"name", Ascending}, {"mass", Descending}};
    CHECK(ui::applySortGesture(two, "name", ui::SortGesture::Click, 8).chain == ui::SortChain{{"name", Descending}});
    CHECK(ui::applySortGesture(two, "mass", ui::SortGesture::Click, 8).chain == ui::SortChain{{"mass", Ascending}});
    CHECK(ui::applySortGesture({{"name", Descending}}, "name", ui::SortGesture::Click, 8).chain ==
          ui::SortChain{{"name", Ascending}});
    CHECK(ui::applySortGesture(two, "site", ui::SortGesture::Extend, 8).chain ==
          ui::SortChain{{"name", Ascending}, {"mass", Descending}, {"site", Ascending}});
    CHECK(ui::applySortGesture(two, "name", ui::SortGesture::Extend, 8).chain ==
          ui::SortChain{{"name", Descending}, {"mass", Descending}});
    CHECK(ui::applySortGesture(two, "mass", ui::SortGesture::Remove, 8).chain == ui::SortChain{{"name", Ascending}});
    CHECK(ui::applySortGesture(two, "site", ui::SortGesture::Descending, 8).chain == ui::SortChain{{"site", Descending}});
}

// Mutation: the bound checked before the "already in the chain" case (the flip at the bound is refused).
TEST_CASE("sort gesture: at the bound a new key is refused and a key in the chain still flips", "[ui][table][config]") {
    using table::SortDirection::Ascending; using table::SortDirection::Descending;
    ui::SortChain const full{{"a", Ascending}, {"b", Ascending}};
    ui::SortChange const added = ui::applySortGesture(full, "c", ui::SortGesture::Extend, 2);
    CHECK(added.chain == full);
    CHECK(added.refusal == "A table sorts by at most 2 columns");
    CHECK(ui::applySortGesture(full, "c", ui::SortGesture::Add, 2).refusal == "A table sorts by at most 2 columns");
    ui::SortChange const flipped = ui::applySortGesture(full, "b", ui::SortGesture::Extend, 2);
    CHECK(flipped.chain == ui::SortChain{{"a", Ascending}, {"b", Descending}});
    CHECK_FALSE(flipped.refusal);
}

// Mutation: applyLayoutGesture ignoring `locked`.
TEST_CASE("layout gesture: moves, hides, pins and resets, refusing a locked column and the last visible one",
          "[ui][table][config]") {
    table::ColumnLayout layout{{"id", "name", "mass", "site"}};
    std::vector<ui::LayoutColumn> const columns{{"id", "ID", true}, {"name", "Name"}, {"mass", "Mass"}, {"site", "Site"}};
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumn{"mass", -1}).changed);
    CHECK(std::ranges::equal(layout.order(), std::vector<std::string>{"id", "mass", "name", "site"}));
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumn{"id", 1}).refusal == "ID is locked");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::PinColumn{"name"}).changed);
    CHECK(layout.pinned() == 1);
    CHECK(layout.visible().front() == "name");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::ResizeColumn{"mass", 14}).changed);
    CHECK(layout.widths().at("mass") == 14);
    for (std::string const id : {"name", "mass", "site"}) {
        CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::HideColumn{id}).changed);
    }
    CHECK(layout.visible() == std::vector<std::string>{"id"});
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::HideColumn{"id"}).refusal ==
          "ID is locked");
    table::ColumnLayout lone{{"a", "b"}};
    std::vector<ui::LayoutColumn> const ab{{"a", "A"}, {"b", "B"}};
    CHECK(ui::applyLayoutGesture(lone, ab, ui::Configure::All, ui::HideColumn{"a"}).changed);
    CHECK(ui::applyLayoutGesture(lone, ab, ui::Configure::All, ui::HideColumn{"b"}).refusal ==
          "At least one column stays visible");
    CHECK(ui::applyLayoutGesture(lone, ab, ui::Configure::Hide, ui::MoveColumn{"b", -1}).refusal ==
          "This table's columns cannot be moved");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::ResetColumns{}).changed);
    CHECK(std::ranges::equal(layout.order(), std::vector<std::string>{"id", "name", "mass", "site"}));
    CHECK(layout.pinned() == 0);
    CHECK(layout.widths().empty());
}

// Mutation: formGroup accepting a column already in a user-formed group.
TEST_CASE("column layout: the user forms and removes groups and sets display units", "[table][columns]") {
    table::ColumnLayout layout{{"companyA", "companyB", "companyC", "weight"}};
    auto const id = layout.formGroup("Company", {"companyA", "companyB"});
    REQUIRE(id == "g1");
    CHECK_FALSE(layout.formGroup("Again", {"companyB", "companyC"}));
    CHECK_FALSE(layout.formGroup("One", {"companyC"}));
    CHECK_FALSE(layout.formGroup("Ghost", {"companyC", "ghost"}));
    CHECK(layout.formGroup("Pair", {"companyC", "weight"}) == "g2");
    CHECK(layout.removeGroup("g2"));
    CHECK(layout.setUnit("weight", "mass", "g"));
    CHECK_FALSE(layout.setUnit("ghost", "mass", "g"));
    table::ColumnLayoutValue const value = layout.value();
    REQUIRE(value.groups.size() == 1);
    CHECK(value.groups.at(0).columns == std::vector<std::string>{"companyA", "companyB"});
    CHECK(value.units == std::map<std::string, std::map<std::string, std::string>>{{"weight", {{"mass", "g"}}}});
    CHECK(layout.setUnit("weight", "mass", std::nullopt));
    CHECK(layout.units().empty());
}

// Mutation: planValueFilter always sending the side the operations reached.
TEST_CASE("value filter: the smaller side is sent, and both sides over the bound disable the list", "[ui][table][config]") {
    using Side = ui::ValueFilterPlan::Side;
    auto const covering = [](int count, bool ticked) {
        std::map<std::string, bool> covered;
        for (int i = 0; i < count; ++i) { covered.emplace("v" + std::to_string(i), ticked); }
        return covered;
    };
    auto const one = ui::planValueFilter(50'000, ui::ResolvedTicks{.ticked = true, .covered = {{"v00007", false}}}, 10'000);
    CHECK(one.side == Side::Exclude);
    CHECK(one.members == 1);
    CHECK_FALSE(one.needsComplement);
    CHECK(ui::planValueFilter(25'000, ui::ResolvedTicks{.ticked = true}, 10'000).side == Side::All);
    CHECK(ui::planValueFilter(25'000, ui::ResolvedTicks{.ticked = false, .covered = covering(12'000, true)}, 10'000).side ==
          Side::Disabled);
    auto const complement = ui::planValueFilter(25'000, ui::ResolvedTicks{.ticked = true, .covered = covering(20'000, false)}, 10'000);
    CHECK(complement.side == Side::Include);
    CHECK(complement.members == 5'000);
    CHECK(complement.needsComplement);
    CHECK(ui::planValueFilter(3, ui::ResolvedTicks{.ticked = false, .blanks = false}, 10'000).side == Side::Nothing);
    // Mutation: the tie decided as Include (`T <= U`): one of two values unticked sends the other as an include.
    auto const tie = ui::planValueFilter(2, ui::ResolvedTicks{.ticked = true, .covered = {{"Open", false}}, .blanks = true}, 10'000);
    CHECK(tie.side == Side::Exclude);
    CHECK(tie.members == 1);
    // Mutation: an empty `oneOf` written beside `isEmpty` when only "(Blanks)" is ticked.
    auto const blanksOnly = ui::planValueFilter(2, ui::ResolvedTicks{.ticked = false, .blanks = true}, 10'000);
    CHECK(blanksOnly.side == Side::Include);
    CHECK(blanksOnly.members == 0);
    auto const onlyBlanks = ui::valueFilter(Side::Include, {}, true);
    REQUIRE(onlyBlanks);
    REQUIRE(onlyBlanks->include.size() == 1);
    CHECK(onlyBlanks->include.at(0).isEmpty == true);
    CHECK_FALSE(ui::valueFilter(Side::Exclude, {}, std::nullopt));
    auto const include = ui::valueFilter(Side::Include, {"F", "S"}, true);
    REQUIRE(include);
    CHECK(include->combine == table::Combine::Any);
    REQUIRE(include->include.size() == 2);
    CHECK(include->include.at(0).oneOf == std::vector<std::string>{"F", "S"});
    CHECK(include->include.at(1).isEmpty == true);
    auto const exclude = ui::valueFilter(Side::Exclude, {"X"}, false);
    REQUIRE(exclude);
    REQUIRE(exclude->exclude.size() == 2);
    CHECK(exclude->exclude.at(1).isEmpty == true);
    auto const ranges = ui::valueFilter(Side::Include, {"2026-03-01..2026-03-31", "2026-05-14..2026-05-14"}, std::nullopt,
                                        ui::ValueForm::Ranges);
    REQUIRE(ranges);
    REQUIRE(ranges->include.size() == 2);
    CHECK(ranges->include.at(0).between == std::array<std::string, 2>{"2026-03-01", "2026-03-31"});
}

// Mutation: resolveTicks applying the operations in reverse order.
TEST_CASE("value ticks: operations under searches resolve in order, the last one winning", "[ui][table][config]") {
    std::map<std::string, std::vector<std::string>> const shows{{"ab", {"ab1", "xab"}}, {"x", {"xab", "xy"}}};
    auto const under = [&](std::string const& search) { return shows.at(search); };
    ui::ValueTicks const ticks{.ticked = true,
                               .ops = {{.search = "", .operand = std::nullopt, .ticked = false},
                                       {.search = "ab", .operand = std::nullopt, .ticked = true},
                                       {.search = "x", .operand = std::nullopt, .ticked = false},
                                       {.search = "", .operand = "xab", .ticked = true}}};
    ui::ResolvedTicks const resolved = ui::resolveTicks(ticks, under);
    CHECK_FALSE(resolved.ticked);
    CHECK(resolved.covered == std::map<std::string, bool>{{"ab1", true}, {"xab", true}, {"xy", false}});
}

// Mutation: classifyFilter reading a Custom column's lone isEmpty as "(Blanks)".
TEST_CASE("filter parts: a column's filter is values or conditions, never both", "[ui][table][config]") {
    using table::ColumnKind;
    auto const entry = [](auto set) { table::FilterEntry e; set(e); return e; };
    table::ColumnFilter values{.combine = table::Combine::Any,
                               .include = {entry([](auto& e) { e.oneOf = std::vector<std::string>{"F"}; }),
                                           entry([](auto& e) { e.isEmpty = true; })}};
    CHECK(ui::classifyFilter(values, ColumnKind::Text) == ui::FilterPart::Values);
    table::ColumnFilter blanks{.combine = table::Combine::Any, .include = {entry([](auto& e) { e.isEmpty = true; })}};
    CHECK(ui::classifyFilter(blanks, ColumnKind::Text) == ui::FilterPart::Values);
    CHECK(ui::classifyFilter(blanks, ColumnKind::Custom) == ui::FilterPart::Conditions);
    table::ColumnFilter conditions{.combine = table::Combine::All,
                                   .include = {entry([](auto& e) { e.gt = "2"; }), entry([](auto& e) { e.lt = "5"; })}};
    CHECK(ui::classifyFilter(conditions, ColumnKind::Integer) == ui::FilterPart::Conditions);
    CHECK(ui::classifyFilter({}, ColumnKind::Integer) == ui::FilterPart::None);
}

// Mutation: canonicalOperand passing typed text through unchanged.
TEST_CASE("typed operands: numbers and measures reach the wire with a point and a unit id", "[ui][table][config]") {
    ui::LocaleFacts const de = ui::localeFacts("de");
    CHECK(ui::canonicalOperand("1.234,5", table::ColumnKind::Decimal, de) == "1234.5");
    CHECK(ui::canonicalOperand("12,5", table::ColumnKind::Integer, de) == std::nullopt);
    CHECK(ui::canonicalOperand("0,3 kg", table::ColumnKind::Measure, de, massUnits()) == "0.3 kg");
    CHECK(ui::canonicalOperand("250", table::ColumnKind::Measure, de, massUnits(), "g") == "250 g");
    CHECK(ui::canonicalOperand("abc", table::ColumnKind::Text, de) == "abc");
}

// Mutation: Together built as one oneOf entry (the row F, F, X would pass).
TEST_CASE("group filter from ticks: the match line chooses the quantifier, together takes one eq per value",
          "[ui][table][config]") {
    auto const atLeast = ui::groupValueFilter("company", 3, {"F", "S"}, std::nullopt, ui::GroupMatch::AtLeast, 2);
    REQUIRE(atLeast);
    CHECK(atLeast->match.kind == table::ColumnCount::Kind::AtLeast);
    CHECK(atLeast->match.n == 2);
    CHECK(atLeast->include.at(0).oneOf == std::vector<std::string>{"F", "S"});
    auto const together = ui::groupValueFilter("company", 3, {"F", "S"}, std::nullopt, ui::GroupMatch::Together, 0);
    REQUIRE(together);
    CHECK(together->mode == table::GroupMode::Together);
    REQUIRE(together->include.size() == 2);
    CHECK(together->include.at(0).eq == "F");
    CHECK(together->include.at(1).eq == "S");
    CHECK_FALSE(ui::groupValueFilter("company", 3, {"F"}, std::nullopt, ui::GroupMatch::Together, 0));
    CHECK_FALSE(ui::groupValueFilter("company", 3, {"F", "S", "T", "U"}, std::nullopt, ui::GroupMatch::Together, 0));
    auto const blanks = ui::groupValueFilter("company", 3, {"F"}, true, ui::GroupMatch::AnyColumn, 0);
    REQUIRE(blanks);
    CHECK(blanks->include.at(1).isEmpty == true);
}

// Mutation: conditionOps returning the numeric list for every kind.
TEST_CASE("conditions follow the column's kind", "[ui][table][config]") {
    using enum ui::ConditionOp;
    CHECK(ui::conditionOps(table::ColumnKind::Text) ==
          std::vector<ui::ConditionOp>{Equals, NotEquals, Contains, BeginsWith, EndsWith});
    auto const numeric = std::vector<ui::ConditionOp>{Equals, NotEquals, Less, LessOrEqual, Greater, GreaterOrEqual, Between};
    for (auto const kind : {table::ColumnKind::Integer, table::ColumnKind::Key, table::ColumnKind::Decimal,
                            table::ColumnKind::Quantity, table::ColumnKind::Number, table::ColumnKind::Measure,
                            table::ColumnKind::Date, table::ColumnKind::DateTime}) {
        CHECK(ui::conditionOps(kind) == numeric);
    }
    CHECK(ui::conditionOps(table::ColumnKind::Bool).empty());
    CHECK(ui::conditionOps(table::ColumnKind::Custom) == std::vector<ui::ConditionOp>{IsEmpty, IsNotEmpty});
    CHECK(ui::conditionLabel(NotEquals, table::ColumnKind::Text) == "does not equal");
    CHECK(ui::conditionLabel(NotEquals, table::ColumnKind::Integer) == "≠");
    table::FilterEntry const between = ui::conditionEntry(Between, "1", "5", false);
    CHECK(between.between == std::array<std::string, 2>{"1", "5"});
    CHECK(ui::conditionEntry(Contains, "ab", {}, true).caseSensitive == true);
}

// Mutation: headerMenu leaving "Add to sort" enabled for a column already in the chain.
TEST_CASE("header menu: entries, order and enablement", "[ui][table][config]") {
    ui::HeaderMenuInput const input{
        .column = {"name", "Name"}, .sortable = true, .filterable = true, .sortPlace = 0, .chainSize = 1,
        .maxSortKeys = 8, .visibleIndex = 1, .visibleCount = 3, .pinned = 0, .dimensions = {},
        .groups = {{"company", "Company", 3}}, .allowed = ui::Configure::All};
    std::vector<ui::MenuEntry> const entries = ui::menuEntries(ui::headerMenu(input));
    std::vector<std::string> labels;
    for (ui::MenuEntry const& entry : entries) { labels.push_back(entry.separator ? "-" : entry.label); }
    CHECK(labels == std::vector<std::string>{"Sort ascending", "Sort descending", "Add to sort", "Remove from sort", "-",
                                             "Filter…", "Filter Company, 3 columns…", "-", "Move left", "Move right",
                                             "Pin column", "Hide column"});
    CHECK_FALSE(entries.at(2).enabled);
    CHECK(entries.at(3).enabled);
    CHECK(entries.at(8).keys == "Ctrl+Shift+Left");
    ui::HeaderMenuInput locked = input;
    locked.column.locked = true;
    std::vector<ui::MenuEntry> const lockedEntries = ui::menuEntries(ui::headerMenu(locked));
    CHECK_FALSE(lockedEntries.at(8).enabled);
    CHECK_FALSE(lockedEntries.at(11).enabled);
    ui::HeaderMenuInput hideOnly = input;
    hideOnly.allowed = ui::Configure::Hide;
    CHECK(ui::menuEntries(ui::headerMenu(hideOnly)).back().label == "Hide column");
    CHECK(ui::menuEntries(ui::headerMenu(hideOnly)).size() == 9);
    // Mutation: the Display unit submenu drawn with no check on the chosen unit.
    ui::HeaderMenuInput measure = input;
    measure.column = {"weight", "Weight", false, table::ColumnKind::Measure};
    measure.dimensions = {{.dimension = "mass", .units = {"g", "kg"}, .chosen = "g"}};
    std::vector<ui::TableMenuEntry> const measureMenu = ui::headerMenu(measure);
    REQUIRE(measureMenu.back().entry.label == "Display unit");
    REQUIRE(measureMenu.back().items.size() == 2);
    CHECK(measureMenu.back().items.at(0).entry.label == "mass: g");
    CHECK(measureMenu.back().items.at(0).entry.checked == true);
    CHECK(measureMenu.back().items.at(1).entry.checked == false);
    CHECK(measureMenu.back().items.at(1).command == ui::MenuCommand::DisplayUnit);
    CHECK(measureMenu.back().items.at(1).argument == "mass\x1Fkg");
    std::vector<ui::MenuEntry> const table = ui::menuEntries(ui::tableMenu({.anyFilter = false, .groups = {}, .allowed = ui::Configure::All}));
    std::vector<std::string> tableLabels;
    for (ui::MenuEntry const& entry : table) { tableLabels.push_back(entry.separator ? "-" : entry.label); }
    CHECK(tableLabels == std::vector<std::string>{"Columns…", "Sort…", "Clear all filters", "Reset columns", "-", "Copy",
                                                  "Copy with headers"});
    CHECK_FALSE(table.at(2).enabled);
    CHECK(table.at(5).keys == "Ctrl+C");
}

// Mutation: copyText replacing "\r\n" with two spaces.
TEST_CASE("copy text: tabs and line breaks become one space; unloaded cells are empty and counted", "[ui][table][config]") {
    std::vector<std::string> const labels{"Na\tme", "N"};
    std::vector<std::vector<std::optional<std::string>>> const rows{{"a\tb\r\nc", "1"}, {std::nullopt, "x\ry\nz"}};
    ui::CopiedText const plain = ui::copyText({}, rows);
    CHECK(plain.text == "a b c\t1\n\tx y z");
    CHECK(plain.unloaded == 1);
    CHECK(ui::copyText(labels, rows).text == "Na me\tN\na b c\t1\n\tx y z");
}

// Mutations: groupedCount writing the digits ungrouped; `copied` using the plural for one row or one cell.
TEST_CASE("status copy: counts are grouped and messages are exact", "[ui][table][config]") {
    CHECK(ui::tableText::tooManyDates() == "Too many dates to list; filter with a condition");
    CHECK(ui::groupedCount(999) == "999");
    CHECK(ui::groupedCount(10'000) == "10,000");
    CHECK(ui::groupedCount(-1'234'567) == "-1,234,567");
    CHECK(ui::groupedCount(10'000, ".") == "10.000");
    CHECK(ui::tableText::sortBound(32) == "A table sorts by at most 32 columns");
    CHECK(ui::tableText::rangeBound(10'000) == "A range selects at most 10,000 rows");
    CHECK(ui::tableText::copyBound(10'000) == "A copy holds at most 10,000 rows");
    CHECK(ui::tableText::copied(200, 12) == "Copied 200 rows, 12 cells were not loaded");
    CHECK(ui::tableText::copied(1, 1) == "Copied 1 row, 1 cell was not loaded");
    CHECK(ui::tableText::copied(3, 0) == "Copied 3 rows");
    CHECK(ui::tableText::unreadable(1) == "1 cell could not be read");
}
```

(`massUnits()` is a file-local helper returning the `UnitInfo`s `kg` and `g` of dimension mass, as Part C's measure tests build them; `ui::LocaleFacts` and `ui::localeFacts` are the existing locale table's.)

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset cl-debug --target morph_tests morph_table_tests`
Expected: compile errors naming `morph/ui/table_config.hpp` (missing) and `ColumnLayout::formGroup`.

- [ ] **Step 3: Implement** the interfaces above in `include/morph/ui/table_config.hpp` and `include/morph/table/engine.hpp`.
  - `groupedCount` groups by three with `separator`; the lowering passes the session's numeric grouping separator, as Part D's `countsText` does; the status copy is English, as the framework's other defaults (`Select all`) are.
  - `applySortGesture`: `Click` makes `{column, Descending}` when `chain[0]` is `{column, Ascending}`, else `{column, Ascending}`; `Ascending`/`Descending` make that one key; `Extend` flips a key already in the chain in place, else appends ascending; `Add` appends only a column not in the chain; `Remove` erases it. The "in the chain" case is decided before the bound; a new key past `maxSortKeys` returns the chain unchanged with `tableText::sortBound(maxSortKeys)`.
  - The rule that pinned columns take at most half the table's width is the renderer's (Task 10), for display only; no gesture here changes the stored count for it.
  - `applyLayoutGesture`: a gesture on a locked column refuses with `tableText::locked(label)` before anything else, a gesture the `allowed` set lacks with `notConfigurable` (`Reset` is allowed while any capability is); `MoveColumn` moves among visible columns, `PinColumn` places the column right after the last pinned visible column and adds one, `UnpinColumn` places it right after the pinned ones less one; `ResetColumns` restores the declared order and `shown`, `pinned = 0`, no widths; a hide that would leave none visible refuses with `lastVisible()`.
  - `planValueFilter`: with D = `distinct`, C = `covered`, T = `(ticked ? D − |C| : 0) + |{c in C : c ticked}|`, U = D − T: `All` when T = D and `blanks` is not `false`; `Nothing` when T = 0 and `blanks` is not `true`; otherwise `Include` when T < U, else `Exclude` (a tie sends the unticked side, so unticking one of two values is one excluded value), with `members = min(T, U)`, `Disabled` when that exceeds `maxSetValues`, and `needsComplement` when the side's members include values outside C (then the menu pages every value at `maxLimit` to name them).
  - `valueFilter`: `Set` → `Include`: `include = {oneOf members} + {isEmpty} if blanks == true`, `combine = Any`; `Exclude`: `exclude = {oneOf members} + {isEmpty} if blanks == false`. On both sides the `oneOf` entry is left out when `members` is empty (ticking only "(Blanks)" is `include = [{isEmpty}]`), and a side left with no entry at all is `nullopt`. `Ranges` → the same with one `between` entry per member, its two ends the member split at `..`. `All`, `Nothing`, `Disabled` → `nullopt`.
  - `canonicalOperand` reads the locale's decimal and grouping separators from `LocaleFacts` (`ui::localeFacts`), never assuming a point.
  - `groupValueFilter`: `AnyColumn`, `EveryColumn`, `AtLeast n`, `Exactly n` → one `oneOf` entry (plus `isEmpty` under `AnyColumn` when `blanks == true`) with `match` `Any`, `All`, `AtLeast`, `Exactly`; `Together` → one `eq` per ticked value, `mode = Together`, refused unless 2 ≤ ticked ≤ `groupColumns`.
  - Every public symbol gets complete Doxygen.

- [ ] **Step 4: Run them to see them pass**

Run: `cmake --build --preset cl-debug --target morph_tests morph_table_tests && ctest --test-dir build/cl-debug -R "sort gesture|layout gesture|column layout|value filter|group filter from ticks|conditions follow|header menu|copy text|status copy" --output-on-failure`
Expected: all pass; then the whole of `morph_table_tests` and the `[ui]` cases of `morph_tests` pass.

- [ ] **Step 5: Mutations, docs, commit.** Apply each mutation named above, see its case fail, revert; paste the failing lines into the commit body. Update `docs/spec/table/engine.md` ("`ColumnLayout`": user-formed groups and display units, and that `apply` and `rebuild` fit them). Run the docs build. Commit: `feat(ui): table gestures, menus and value filters as functions`.

### Task 12b: The backend contract for configuring, in every backend

**Files:**
- Modify: `include/morph/ui/view.hpp:364-374` (`TableSort`), `:1217-1283` (`TableColumn`, `Table`), `:2026-2049` (`TableOptions`); `include/morph/ui/backend.hpp:764-879` (`TableWidget`), `:1293-1510` (`IViewBackend::copyText`); `include/morph/ui/mount.hpp:1622-1645` (binding the new members); `include/morph/ui/testing/recording_backend.hpp:290-298` (`formatSort`), `:1986-2240` (`FakeTable`), `:3015-3040` (drivers); `include/morph/ui/testing/backend_conformance.hpp:60-225` (probe operations) and its cases
- Modify: `src/qt_quick/kinds.hpp:614-695`, `src/qt_quick/table.cpp`, `src/qt_quick/qml/MorphTable.qml`, `src/qt_quick/qml/MorphMenu.qml` (as the header and table menus' popup), `src/qt_quick/backend.cpp` (`copyText` through `QGuiApplication::clipboard()`); `src/tui/table_widget.hpp`, `src/tui/table_widget.cpp` (and the TUI backend's `copyText`: OSC 52)
- Modify: `include/morph/forms/engine/collection_view.hpp:143-227` (the `sort` and `onSort` members it leaves unset keep compiling)
- Modify, so this commit builds and stays green on its own: `src/ui/interpreter.cpp` (the table lowering's `options.sort` and `options.onSort`, today at `:7655-7673`: `sort` becomes a `std::vector<TableSort>` of the one key it holds, or empty, and `onSort` takes `(std::size_t, HeaderClick)` and ignores the click — the chain is 12c's); `tests/test_ui_table_row_span.cpp:52,79,170-198` (its `Signal<std::optional<ui::TableSort>>` becomes a `Signal<std::vector<ui::TableSort>>`, `std::nullopt` an empty vector); the existing `setSort(ui::TableSort{…})` and `setSort(std::nullopt)` calls at `tests/qt_quick/test_qt_quick_table.cpp:1148,1163` and `tests/tui/test_tui_table.cpp:910` (a one-key vector, an empty one); `tests/test_ui_conformance.cpp` (`REQUIRE(cases.size() == 56)` becomes `64`, and the faults below)
- Test: `tests/test_ui_conformance.cpp`, `tests/test_ui_table_config.cpp` (the `sort marks` case), `tests/qt_quick/test_qt_quick_conformance.cpp`, `tests/qt_quick/test_qt_quick_table.cpp`, `tests/tui/test_tui_conformance.cpp`, `tests/tui/test_tui_table.cpp`
- Docs: `docs/spec/ui/backend_contract.md` ("Widgets" table entry, "The probe", "The cases", "RecordingBackend"), `docs/spec/ui/view_tree.md` ("ForEach and Table rows"), `docs/spec/qt_quick/backend.md` (header menus, the table menu button, header drag and resize, the group header), the TUI table's header comment in `src/tui/table_widget.hpp`.

**Interfaces:**
- Consumes: Part D's `ui::TableColumn::id`, `Table::arrangement` (`TableArrangement`, whose visible places `onColumnMove` positions and `TableColumnGroup::first` count in) and `Table::status`.
- **Column indices (index decision 37).** The `column` argument of every handler, `TableWidget` call, probe operation and `RecordingBackend` driver below is the index into `Table::columns`, which holds every column, hidden ones included, and does not change when the layout does. Only a `position` argument (`onColumnMove`, `moveColumn`) and `TableColumnGroup::first` are visible places of the arrangement. A driver given a hidden column's index throws `std::logic_error`: a user cannot reach its header.
- **Parallel structs (index decision 41).** Every member below is added to both `ui::Table` and `TableOptions`, and `ui::table<Row>()` copies each across.
- Produces (namespace `morph::ui`):
  - `enum class HeaderClick : std::uint8_t { Plain, Shift }`
  - `[[nodiscard]] std::string sortMarkText(SortDirection direction, std::size_t place, std::size_t count)` and `[[nodiscard]] std::string sortDescription(SortDirection direction, std::size_t place, std::size_t count)` in `view.hpp` (`place` from 1): `sortMarkText` is `" ▲"`/`" ▼"` when `count == 1`, else `" ▲<place>"`/`" ▼<place>"`; `sortDescription` is exactly `"sorted ascending"`/`"sorted descending"` when `count == 1`, else `"sorted ascending, key <place> of <count>"` (lower case, as table.md "Sort chains" writes it); both backends set the header's accessible description from it and nothing else
  - `struct TableColumnGroup { std::string label; std::size_t first = 0; std::size_t count = 0; bool filtered = false; }`
  - `ui::TableColumn::filterable` (`bool`, default `false`)
  - `ui::Table` and `TableOptions`: `Prop<std::vector<TableSort>> sort` (replacing `Prop<std::optional<TableSort>>`, chain order), `std::function<void(std::size_t, HeaderClick)> onSort` (replacing the one-argument handler), `std::function<std::vector<MenuEntry>(std::optional<std::size_t>)> menu` and `std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onMenu` (no column: the table's menu), `std::function<void(std::size_t column, std::size_t position)> onColumnMove`, `std::function<void(std::size_t column, std::optional<int> units)> onColumnResize`, `std::function<void(std::size_t column)> onFilter`, `Prop<std::vector<std::size_t>> filtered`, `Prop<std::vector<TableColumnGroup>> columnGroups`, `std::function<void(std::size_t group)> onGroupFilter`, `Action onCopy`
  - `TableWidget`: `virtual void setSort(std::vector<TableSort> const& chain) = 0;` and `virtual void setOnSort(std::function<void(std::size_t, HeaderClick)> onSort) = 0;` (replacing both), and with default empty bodies `setMenu(std::function<std::vector<MenuEntry>(std::optional<std::size_t>)> entries, std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onChoose)`, `setOnColumnMove(…)`, `setOnColumnResize(…)`, `setOnFilter(…)`, `setFiltered(std::vector<std::size_t> const&)`, `setColumnGroups(std::vector<TableColumnGroup> const&)`, `setOnGroupFilter(…)`, `setOnCopy(Action)` (the status line's `setStatus` and `setOnRetry` are Part D's)
  - `IViewBackend::copyText(std::string_view text) -> bool` (default `false`: no clipboard)
  - `RecordingBackend` drivers: the existing `clickHeader(int table, std::size_t column)` gains a defaulted third parameter, `HeaderClick click = HeaderClick::Plain` (index decision 42), so its callers in `tests/test_ui_interpreter_table.cpp`, `tests/test_ui_conformance.cpp` and `examples/lims_gallery/tests/test_samples.cpp` compile unchanged; `menuOf(int table, std::optional<std::size_t> column) -> std::vector<MenuEntry>`, `chooseMenu(int table, std::optional<std::size_t> column, std::vector<std::string_view> const& labels)` (labels from the top, resolved to the entry path), `moveColumn(int table, std::size_t column, std::size_t position)`, `resizeColumn(int table, std::size_t column, std::optional<int> units)`, `clickFilter(int table, std::size_t column)`, `clickGroupFilter(int table, std::size_t group)`, `pressCopy(int table)`, `clipboardText() -> std::string` (the last text `copyText` was given; empty before any — index decision 27); properties `sort` (`"0:Ascending,2:Descending"`, `none` when empty), `filtered` (`[0,2]`), `columnGroups` (`[Company:0+3]`, label, first visible place, count)
  - `ConformanceProbe`: the existing pure `clickHeader(Widget& table, std::size_t column)` gains the same defaulted `HeaderClick click = HeaderClick::Plain` (one virtual, three parameters; the probes' overrides take the third parameter, the existing cases' calls are unchanged). The operations added here are virtual with default bodies that do nothing and return an empty value, as `descriptionOf` and `boundsOf` are, so the null probe of `tests/test_ui_conformance.cpp` needs no override and fails the new cases by observing nothing; the three real probes (`RecordingProbe` in `tests/test_ui_conformance.cpp`, the Qt Quick probe, the terminal probe) override every one: `headerMenu(Widget& table, std::optional<std::size_t> column) -> std::vector<MenuEntry>`, `chooseHeaderMenuEntry(Widget& table, std::optional<std::size_t> column, std::vector<std::size_t> const& path)`, `moveColumn(Widget& table, std::size_t column, std::size_t position)`, `headerText(Widget const& table, std::size_t column) -> std::string`, `headerName(Widget const& table, std::size_t column) -> std::string`, `headerDescription(Widget const& table, std::size_t column) -> std::string`, `clipboardText() -> std::string`
- Backend behaviour both backends implement: a header's menu opens on a right click on the header, on `Shift+F10` or the context-menu key with the header focused, and from a `⋯` button at the header row's trailing end for the table's menu (accessible name `Table options`); `Shift`+click, `Shift+Enter` and `Shift+Space` report `HeaderClick::Shift`; `Ctrl+Shift+Left` and `Ctrl+Shift+Right` on a focused header report `onColumnMove(column, position ∓ 1)` among the arrangement's visible columns; on Qt Quick dragging a header onto another reports the move, dragging a header's trailing edge reports `onColumnResize` in units, a double click on the edge reports `nullopt`; a filterable header draws a filter button, filled while `setFiltered` names it; a group header row is drawn over side-by-side group columns with its own filter button; `Ctrl+C` on a focused table reports `onCopy`. The terminal header becomes focusable: `Up` on the first row focuses the header at the sorted column (else the first), `Left`/`Right` move along it, `Enter`/`Space` sort, `Shift+Enter` extends, `Ctrl+Shift+Left/Right` move, `Shift+F10` opens its menu, `Down` returns to the rows; the terminal draws no drag or resize handle.

- [ ] **Step 1: Write the failing conformance cases** in `include/morph/ui/testing/backend_conformance.hpp`, appended to `conformanceCases()` in this order (the `std::array<ConformanceCase, 56>` becomes `64`, as does the count `tests/test_ui_conformance.cpp` requires), each with its control comment. A control is a `Fault` of `tests/test_ui_conformance.cpp`'s `RecordingProbe`, the repository's way to show that a conformance case fails on a broken backend. This task adds `PlainHeaderClicks` (every header click reported as `Plain`), `FirstSortKeyOnly` (`headerText` and `headerDescription` answer for the chain's first key only), `MenuColumnOffByOne` (`chooseHeaderMenuEntry` chooses on `column + 1`), `MoveTargetIsSource` (`moveColumn` reports the column's own place), `UnnamedFilter` (`headerName` answers the bare label) and `SilentClipboard` (`clipboardText` answers empty), and extends `"ui conformance: the suite flags a backend that breaks the contract"` to assert, per fault, that `failingCases(fault)` contains the case its control stands beside below. The existing `DeadInput` covers the table-menu and `Ctrl+C` cases, whose probe operations go through it:

```cpp
// Control: report every header click as Plain.
{"table: a Shift-click on a sortable header reports Shift, a plain click Plain", /* mount a Table of columns
  {Name sortable, Mass sortable}, onSort recording (column, click); probe.clickHeader(table, 1, HeaderClick::Shift);
  probe.clickHeader(table, 0, HeaderClick::Plain); expect {(1, Shift), (0, Plain)} */},
// Control: draw only the chain's first key.
{"table: a chain of three keys marks every header with its place", /* columns {Name, Mass, Site} all sortable;
  sort = {{1, Descending}, {0, Ascending}, {2, Ascending}}: headerText(table, 0) == "Name ▲2",
  headerText(table, 1) == "Mass ▼1", headerText(table, 2) == "Site ▲3";
  headerDescription(table, 0) == "sorted ascending, key 2 of 3" (compared whole, lower case);
  with sort = {{0, Descending}}: headerText(table, 0) == "Name ▼", headerDescription(table, 0) == "sorted descending",
  headerDescription(table, 1) == "" */},
// Control: pass the clicked header's index plus one to onMenu.
{"table: a header's menu shows its entries and choosing one reports the column and the path", /* menu returns
  {Sort ascending, separator, Hide column} for column 1; headerMenu(table, 1) labels == those three;
  chooseHeaderMenuEntry(table, 1, {2}) reports (1, {2}) */},
{"table: the table's menu is the menu with no column", /* chooseHeaderMenuEntry(table, nullopt, {0}) reports (nullopt, {0}) */},
// Control: report the move's target as the source.
{"table: a column moved by the user reports its column and new place", /* moveColumn(table, 0, 1) reports (0, 1) */},
// Control: name a filtered header as an unfiltered one.
{"table: a filtered column's header is named so", /* columns filterable; filtered = {0}: headerName(table, 0) ==
  "Name, filtered", headerName(table, 1) == "Mass" */},
{"table: Ctrl+C on a focused table reports a copy", /* press(table, "Ctrl+C") calls onCopy once */},
// Control: copyText returning true without writing.
{"backend: copyText puts its text on the clipboard", /* backend().copyText("a\tb") == true; clipboardText() == "a\tb" */},
```

The accessible description is also held by a unit case in `tests/test_ui_table_config.cpp` (it needs no backend):

```cpp
// Mutation: sortDescription capitalised ("Sorted ascending, …"), or the count written as the place.
TEST_CASE("sort marks: the header's mark and accessible description", "[ui][table][config]") {
    CHECK(ui::sortDescription(ui::SortDirection::Ascending, 2, 3) == "sorted ascending, key 2 of 3");
    CHECK(ui::sortDescription(ui::SortDirection::Descending, 1, 1) == "sorted descending");
    CHECK(ui::sortMarkText(ui::SortDirection::Ascending, 2, 3) == " ▲2");
    CHECK(ui::sortMarkText(ui::SortDirection::Descending, 1, 1) == " ▼");
}
```

Write each case's body with the assertions in its comment. Add to `tests/qt_quick/test_qt_quick_table.cpp`:

```cpp
// Control: report the edge drag's width in pixels instead of units.
TEST_CASE("qt_quick table: dragging a header's trailing edge reports units, a double click on it none", "[qt_quick][table]")
// header 0 fixed(12); drag its trailing edge right by 4 units (theme unit width × 4): onColumnResize == (0, 16);
// double-click the edge: onColumnResize == (0, nullopt).
// Control: ignore a drag that ends on another header.
TEST_CASE("qt_quick table: dragging a header onto another reports the move", "[qt_quick][table]")
// drag header 0 onto header 2 of three: onColumnMove == (0, 2).
```

and to `tests/tui/test_tui_table.cpp`:

```cpp
// Control: leave the header unfocusable.
TEST_CASE("tui table: Up on the first row focuses the header; Left, Right, Shift+Enter and Ctrl+Shift+Right act on it", "[tui]")
// three sortable columns, sort {{1, Ascending}}: Up on row 0 focuses header 1; Right focuses header 2;
// Shift+Enter reports (2, Shift); Ctrl+Shift+Left reports onColumnMove(2, 1); Down returns to row 0.
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset cl-debug --target morph_tests` then `cmake --build build/cl-qt-debug --target morph_qt_quick_tests morph_tui_tests`
Expected: compile errors for `HeaderClick`, `TableColumnGroup`, the probe operations and the new `Fault` enumerators.

- [ ] **Step 3: Implement** the interfaces and behaviour above in every listed file. `mount.hpp` binds `sort`, `filtered` and `columnGroups` as it binds `sort` today, wires each handler with `event(...)`, calls `setMenu` only when `menu` is set and `setOnColumnResize` only when `onColumnResize` is set, and keeps calling `setSort` only for a table with a sortable column. The row-span rule reads the chain where it read the optional: `spans.sorted = sortable && sortValue == nullptr && !spec.sort.constant().empty()` (`mount.hpp:1646` tests `has_value()` today), and inside the rows Effect a bound chain counts as sorted while it is not empty. `copyText` writes `QGuiApplication::clipboard()` on Qt Quick, an OSC 52 sequence on the terminal, and a record on `RecordingBackend`; each backend's conformance probe reads back what its `copyText` wrote.

- [ ] **Step 4: Run them to see them pass**

Run: `ctest --test-dir build/cl-debug -R "conformance|sort marks" --output-on-failure`; `ctest --test-dir build/cl-qt-debug -R "qt_quick table|tui table|conformance" --output-on-failure`
Expected: all pass, the existing table cases included.

- [ ] **Step 5: Mutations, docs, commit.** Run each control; paste the failures. Update the docs listed. Run the docs build. Commit: `feat(ui): header menus, sort chains, column moves and the status line in every backend`.

### Task 12c: Lowering sort chains, the layout, the menus and the status line's messages

**Files:**
- Modify: `src/ui/document.cpp:292-334` (keys), `:3209-3290` (load checks); `src/ui/interpreter.cpp` (the table lowering Part D built: its `sort`, `onSort`, columns, element values and `TableStatus::message`), `include/morph/ui/interpreter.hpp:371-395` (`SessionOptions::maxCopyRows`)
- Test: `tests/test_ui_table_model.cpp`, `tests/test_ui_document.cpp`, `tests/bench_interpreter.cpp`, `tests/ui_table_support.hpp` (the helpers and documents of Step 1)
- Docs: `docs/spec/ui/document.md` (the `table` row: `layout`, `onLayoutChange`, `configurable`, `onFiltersChange`, a column's `locked` and `filterable`; element values `layout`, `visibleColumns`, `hiddenCount`), `docs/spec/ui/interpreter.md` (the lowering's menus and messages), `docs/spec/table/table.md` (D7 figure)

**Interfaces:**
- Consumes: 12a, 12b; Part D's `TableRows`, `TableStatus`, the state store's load and save, `Table::arrangement`, the lowering of both table kinds, and Task 9d's message channel `ScreenCore::tableMessage(ElementState&, std::string)` with its `kStatusMessageHold` and its `status: false` routing (index decision 11). Every refusal this part makes is posted through it; this part adds no second channel.
- Produces:
  - document keys on `table`: `layout` (Expr; bound, it is controlled), `onLayoutChange` (Commands, `event` the new layout), `configurable` (a literal `true`, `false` or a list of `"reorder"`, `"hide"`, `"pin"`, `"resize"`; anything else is refused at load), `onFiltersChange` (Commands, `event` the new `filters`); on a column part: `locked` (Flag), `filterable` (a literal boolean, default `true`; a column without `id` is never filterable)
  - element values `layout` (the stored shape), `visibleColumns` (ids in order), `hiddenCount`
  - `SessionOptions::maxCopyRows = 10'000` (used by 12f)
  - the clearing half of the message rule for the gestures this part adds: each one the lowering receives (a header click, a menu choice, a column move or resize, a filter button, a copy) calls `tableMessage(table, "")` before it acts, so the user's next gesture on the table clears a message; a refusal it then makes posts its own
- The lowering's rules: `onSort(column, click)` maps `Plain` to `SortGesture::Click` and `Shift` to `Extend`, applies `applySortGesture` with the table's bound (on a model-backed table `description.capabilities.maxSortKeys`, which Task 16's `servedDescription` fills from the server's `limits`; on a `rows` table the node's literal `maxSortKeys`, which Task 16's load check already accepts as an integer 1 to 32; else 8), and on success calls `TableRows::setSort`, sets the held `sort` (when not bound) and runs `onSortChange`; a refusal goes to `tableMessage`. Menu commands map to the same gestures and to 12d, 12e and 12f's openers; `menu` and `onMenu` are set unless `configurable` is `false`. A layout change updates `Table::arrangement`, calls `TableRows::visible` with the new visible columns, sets the held `layout` (when not bound), runs `onLayoutChange` and marks the state unsaved; the stored state's `layout` member is applied before the first rows, as `sort` and `filters` are, and a bound `layout` is neither applied nor saved. A `rows` table's `TableStatus::shown` holds while its `state` or `message` is not empty (a model-backed table's is always shown, Part D); a literal `status: true` shows it always. Every refusal is shown (invariant 31).


- **Carried from Task 16** (its server half and the document key landed before this lowering existed; this task owns the client half):
  - The bound above is read by the lowering: Task 16 left `maxSortKeys` checked at load and read by nothing. When this lands, `docs/spec/ui/document.md`'s `maxSortKeys` entry changes from "checked at load; it takes effect when the table's sort gestures are lowered" to the bound the header and menu gestures are held to.
  - The test, verbatim from Task 16's brief, in `tests/test_ui_table_model.cpp`:

```cpp
// Mutation: the client holding 8 whatever the description says.
TEST_CASE("sort chains: a table whose limits set 32 accepts 32 keys from the header and refuses the 33rd", "[ui][table][config]")
// kConfigModelTable over samplesDescription(40) whose capabilities.maxSortKeys is 32: Shift-click 33 sortable headers: t.sort
// has 32 keys, the last page request's sort has 32 keys, status message "A table sorts by at most 32 columns".
// A rows table with "maxSortKeys": 12 over 13 sortable columns: 12 keys, the 13th refused with "…at most 12 columns".
```
  - Run the mutation "the client holding 8 whatever the description says" (the lowering passing 8 on both kinds) once and record its failing output in this task's commit, as the spec's Tests row "A raised sort bound" names it.

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_model.cpp` (tag `[ui][table][config]`). Every test of this part runs on `ui::testing::TableHarness` (9d, index decision 42) and mounts a whole document.

Helpers, added to `tests/ui_table_support.hpp` by this task (index decisions 20 and 42): free functions over `ui::testing::TableHarness&` that call 12b's `RecordingBackend` driver of the same name on `h.backend` and then `h.settle()` — `menuOf(h, table, column) -> std::vector<ui::MenuEntry>`, `chooseMenu(h, table, column, labels)`, `moveColumn(h, table, column, position)`, `resizeColumn(h, table, column, units)`, `clickFilter(h, table, column)`, `clickGroupFilter(h, table, group)`, `pressCopy(h, table)`, `clipboardText(h) -> std::string`; `table` is the widget id, `column` an index into `Table::columns` (index decision 37). A header click is `h.backend.clickHeader(table, column, click)`. In the sketches of this part a helper written without `h` (`chooseMenu(table, …)`) is that helper with `h` first, `<name>` in angle brackets (`<status>`) is that column's index, and `t.<name>` is `elementValue(h, "t", "<name>")`, the element value as JSON text.

The documents, in `tests/ui_table_support.hpp` (a sketch that says "with …" mounts the document with that one key added to its `table` node or column):

```cpp
/// A rows table `t`: nine sortable columns c1…c9 labelled C1…C9, three rows keyed by `id`.
inline constexpr std::string_view kNine = R"({
  "vocab": "ui/1", "id": "nine",
  "state": { "items": { "init": { "lit": [
    { "id": 1, "c1": 3, "c2": 1, "c3": 2, "c4": 1, "c5": 2, "c6": 3, "c7": 1, "c8": 2, "c9": 3 },
    { "id": 2, "c1": 1, "c2": 2, "c3": 3, "c4": 2, "c5": 3, "c6": 1, "c7": 2, "c8": 3, "c9": 1 },
    { "id": 3, "c1": 2, "c2": 3, "c3": 1, "c4": 3, "c5": 1, "c6": 2, "c7": 3, "c8": 1, "c9": 2 } ] } } },
  "view": { "kind": "table", "id": "t", "rows": { "ref": "items" },
            "columns": [ { "id": "c1", "label": "C1", "sortable": true }, { "id": "c2", "label": "C2", "sortable": true },
                         { "id": "c3", "label": "C3", "sortable": true }, { "id": "c4", "label": "C4", "sortable": true },
                         { "id": "c5", "label": "C5", "sortable": true }, { "id": "c6", "label": "C6", "sortable": true },
                         { "id": "c7", "label": "C7", "sortable": true }, { "id": "c8", "label": "C8", "sortable": true },
                         { "id": "c9", "label": "C9", "sortable": true } ] } })";

/// The model-backed table `t` over `Table.samples`; its description is what the test replies (`h.replyDescribe`).
inline constexpr std::string_view kConfigModelTable = R"({
  "vocab": "ui/1", "id": "samples",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "view": { "kind": "table", "id": "t", "model": "sample", "table": "samples" } })";
```

The cases:

```cpp
// Mutation: the lowering mapping HeaderClick::Shift to Click.
TEST_CASE("sort chains: a plain click replaces, Shift adds and flips, a key past the bound is refused", "[ui][table][config]") {
    ui::testing::TableHarness h;
    auto screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    h.backend.clickHeader(table, 0, ui::HeaderClick::Plain);
    h.backend.clickHeader(table, 1, ui::HeaderClick::Shift);
    h.backend.clickHeader(table, 0, ui::HeaderClick::Shift);
    h.settle();
    CHECK(elementValue(h, "t", "sort") == R"([{"column":"c1","direction":"desc"},{"column":"c2","direction":"asc"}])");
    CHECK(h.backend.prop(table, "sort") == "0:Descending,1:Ascending");
    for (std::size_t column = 2; column < 9; ++column) { h.backend.clickHeader(table, column, ui::HeaderClick::Shift); }
    h.settle();
    CHECK(h.backend.prop(table, "sort") ==
          "0:Descending,1:Ascending,2:Ascending,3:Ascending,4:Ascending,5:Ascending,6:Ascending,7:Ascending");
    CHECK(statusOf(h).message == "A table sorts by at most 8 columns");
    CHECK(statusOf(h).shown);
    h.backend.clickHeader(table, 4, ui::HeaderClick::Plain);
    h.settle();
    CHECK(elementValue(h, "t", "sort") == R"([{"column":"c5","direction":"asc"}])");
    CHECK(statusOf(h).message.empty());
    CHECK_FALSE(statusOf(h).shown);
}

// Mutation: "Add to sort" mapped to SortGesture::Click.
TEST_CASE("sort chains: the header menu builds a chain without a modifier", "[ui][table][config]")
// chooseMenu(table, 0, {"Sort descending"}); chooseMenu(table, 2, {"Add to sort"}); chooseMenu(table, 0, {"Remove from sort"}):
// sort == [{"column":"c3","direction":"asc"}]; onSortChange ran three times.

// Mutations: the lowering applying a menu gesture without updating `visibleColumns`; a driver's column read as a
// visible place (the resize after the move lands on c1).
TEST_CASE("column layout: hiding, moving, pinning and resetting from the menus change the layout and the visible columns",
          "[ui][table][config]")
// chooseMenu(table, 2, {"Hide column"}); chooseMenu(table, 1, {"Move left"}); chooseMenu(table, 1, {"Pin column"});
// t.visibleColumns == ["c2","c1","c4",…,"c9"]; t.hiddenCount == 1;
// t.layout == {"order":["c2","c1","c3",…],"hidden":["c3"],"pinned":1,"widths":{}};
// resizeColumn(table, 1, 20) (index 1 is c2 wherever it stands): t.layout.widths == {"c2":20};
// resizeColumn(table, 1, nullopt): widths == {};
// chooseMenu(table, nullopt, {"Reset columns"}): t.layout == the declared order, nothing hidden, pinned 0;
// onLayoutChange ran six times with `event` the new layout.

// Mutation: `configurable` ignored for menus (the hide entry is present).
TEST_CASE("column layout: configurable and locked limit what the user can do, and each refusal is shown", "[ui][table][config]")
// "configurable": ["hide"], c1 "locked": true: menuOf(table, 1) has no "Move left"; moveColumn(table, 1, 0) leaves
// visibleColumns unchanged and status message "This table's columns cannot be moved"; menuOf(table, 0)'s "Hide column"
// is disabled and moveColumn(table, 0, 1) shows "C1 is locked"; "configurable": false: menuOf(table, 1) is empty and no
// "⋯" table menu is offered (menuOf(table, nullopt) empty).

// Mutation: a header click lowered without `tableMessage(table, "")` (the message outlives the gesture).
// (The hold and the `status: false` routing are 9d's; this case holds them through this part's refusals.)
TEST_CASE("status line: a refusal is shown once, until the next gesture or six seconds", "[ui][table][config]")
// refusal at the bound shows the message; h.advance(5'999ms): still shown; h.advance(1ms): message empty and
// statusOf(h).shown false (a rows table with nothing else to say takes no room); refuse again then
// clickHeader(table, 0, Plain): message gone. "status": false: the harness's record of `notify` calls (9d's
// `h.host.notified`) holds exactly one entry, the text "A table sorts by at most 8 columns" with the role "warning", and
// statusOf(h).shown stays false.

// Mutation: the lowering applying a stored layout after the first query.
TEST_CASE("persisted layout: a stored layout applies before the first query, a change saves once after the debounce",
          "[ui][table][config]")
// TableHarness(store) over a MemoryTableStateStore holding, under the table's key, part "state",
// {"version":1,"layout":{"order":["c2","c1"],"hidden":["c3"],"pinned":0,"widths":{"c2":20}}}: before any rows,
// t.visibleColumns starts with "c2","c1" and lacks "c3"; chooseMenu(table, 0, {"Hide column"}), chooseMenu(table, 3,
// {"Hide column"}) and chooseMenu(table, 4, {"Hide column"}) (c1, c4, c5: indices into Table::columns) inside 500 ms, then
// h.advance(500ms): the store received exactly one save, whose layout's `hidden` is c1, c3, c4 and c5. With `layout` bound to
// state `lay`, the stored layout is neither applied nor saved and the sort part still is.

// Review Focus 1. Mutation: hiding a column removing it from the query's sort.
TEST_CASE("hiding a sorted and filtered column changes no row", "[ui][table][config]")
// kConfigModelTable over samplesDescription(5), sorted by `name`, filter `{"columns":{"name":{"include":[{"contains":"a"}]}}}`:
// h.replyPage(pageJson(0, 50, 50, cols)) holds rows R; chooseMenu(table, 1, {"Hide column"}): no new Table.samples call
// whose sort or filters differ (any call sent is a column fetch of held rows: lastQuery(h)'s `sort` and `filters` equal
// the previous ones and its `columns` lacks "name");
// the view's row keys equal R; showing it again restores the header mark ("sort" prop names its index) and `filtered`.

// Review Focus 2 (message half). Mutation: refusing Shift on a column already in the chain at the bound.
TEST_CASE("sort chains: at the bound a key in the chain still flips, with no message", "[ui][table][config]")
// eight keys held; clickHeader(table, 3, Shift): sort's 4th key flips to desc; status message empty.
```

In `tests/test_ui_document.cpp`:

```cpp
// Mutation: the loader accepting any `configurable` literal.
TEST_CASE("table: configurable, locked and filterable are literals the loader checks", "[ui][document]")
// "configurable": ["reorder","sort"] is refused naming `sort`; "configurable": {"ref":"x"} refused; a column
// "filterable": {"ref":"x"} refused; "locked": true and "filterable": false accepted; "onLayoutChange" and
// "onFiltersChange" take commands.
```

In `tests/bench_interpreter.cpp`:

```cpp
TEST_CASE("interpreter bench: a third sort key on 100,000 rows", "[.benchmark][interpreter]")
// a rows table of 100,000 rows sorted by two keys; BENCHMARK the Shift-click that adds a third key to the settled
// view (D7, budget ≤ 50 ms).
```

**From Task 15 (Part F), which landed first.** A companion column (a column of
`table::companionColumns(description)`: a `tone`, `tooltip`, `rowTone` or `rowLock` companion) is never one of the
table's columns, so it is neither visible nor hidden: the layout, `visibleColumns`, `hiddenCount` and the header
menus' hide, show and pin entries never name one. Task 15's lowering already leaves companions out of the table's
columns; the risk is this task's layout, which a description's `shown: false` columns enter as hidden. In Step 1,
**extend the existing case** "table display: companion columns are not arranged" in `tests/test_ui_table_model.cpp`
(do not add a second case of that name, which Catch2 refuses): over `companionDescription()`, whose companions are
`shown: false`, `t.visibleColumns` is `["id","mass","name","count"]`, `t.hiddenCount` is 0, and no entry of any
column's header menu names `massTone`, `massTip` or `lockedBy`. It fails first because neither element value exists
before this task; its mutation is the layout built from every column of the description, which counts the three
companions hidden (`hiddenCount` 3, and the menus offer to show them).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset cl-debug --target morph_tests && ctest --test-dir build/cl-debug -R "sort chains|column layout|status line|persisted layout|hiding a sorted|configurable, locked|companion columns are not arranged" --output-on-failure`
Expected: FAIL — unknown keys `configurable` and `locked` refused at load, and no `layout` element value.

- [ ] **Step 3: Implement** the keys, the checks, the element values, `maxCopyRows` and the lowering's rules above, posting every refusal through Part D's `tableMessage`.

- [ ] **Step 4: Run them to see them pass**

Run: the Step 2 command, then `ctest --test-dir build/cl-debug -R "ui" --output-on-failure`
Expected: all pass, the existing `test_ui_table.cpp` and `test_ui_interpreter_table.cpp` cases included.

- [ ] **Step 5: Record D7.** Run: `cmake --preset cl-release && cmake --build --preset cl-release --target morph_tests && build/cl-release/tests/morph_tests.exe "interpreter bench: a third sort key on 100,000 rows"`. Expected: a mean ≤ 50 ms; record it, dated, in table.md's D7 row.

- [ ] **Step 6: Mutations, docs, commit.** Commit: `feat(ui): configure a table from its header`.

### Task 12d: The filter menu

**Files:**
- Create: `include/morph/ui/table_filter_menu.hpp` (the menu's view tree and its state, built by the lowering)
- Modify: `include/morph/ui/table_rows.hpp` (`TableRows::distinctValues`), `src/ui/interpreter.cpp`, `src/ui/document.cpp` (a `rows` table's `groups` parts `{id, label, columns}`), `include/morph/ui/interpreter.hpp` (`SessionOptions::tableFilterDebounce`)
- Test: `tests/test_ui_table_model.cpp`, `tests/net/test_push_tables.cpp`
- Docs: `docs/spec/ui/document.md` (a `rows` table's `groups`), `docs/spec/qt_quick/backend.md` (the filter menu), table.md (B3 and D9 figures)

**Interfaces:**
- Consumes: 12e's column chooser (`table_columns_group`, `table_group_create`, `table_group_remove_<id>`), through which the user-formed-group case forms and removes its group, so 12e lands first; 12a's tick model, `planValueFilter`, `valueFilter`, `classifyFilter`, `canonicalOperand`, `groupValueFilter`, `conditionOps`, `conditionEntry`; 12b's `onFilter`, `onGroupFilter`, `filtered`, `columnGroups`; Task 8a's `DistinctRequest`/`DistinctValues` and `DateBucket` (a bucket's `text` is its ISO form, `2026`, `2026-03`, `2026-03-14`, and its operand `a..b`, as Part C's tests pin them); Task 8c's `DistinctValue::{dimension, unit}`, `ui::parseMeasure`, `ui::measureOperand`; Part D's `ScreenCore::tableMessage`.
- Produces:
  - `TableRows::distinctValues(table::DistinctRequest request) -> async::Completion<std::expected<table::DistinctValues, table::TableError>>` (pure virtual; `EngineRows` answers at once through `Engine::distinctValues`; `WindowRows` sends a query carrying `distinct` through the session's query cache, so reopening a menu sends nothing; `TableCapabilities::distinct == false` → `Unavailable`, and the menu shows conditions only)
  - `SessionOptions::tableFilterDebounce = std::chrono::milliseconds{250}`
  - the menu, a `ui::Dialog` (test id `table_filter`, title the column's or group's label) the lowering mounts beside the table, with test ids: `table_filter_search` (TextInput), `table_filter_all` (`(Select all)` Checkbox), `table_filter_blanks` (`(Blanks)`, only while `blanks > 0`), `table_filter_value_<i>` (Checkbox, label `{text} ({count})`, in the reply's order, `i` from 0), `table_filter_heading_<dimension>` (Text, a `Measure` column's dimension heading before its first value), `table_filter_open_<i>` (Button `▸`, a date bucket that opens to its months or days), `table_filter_more` (Button, the next 200), `table_filter_note` (Text: `mayGrow()` while `partial`, `unreadable(invalid)` while `invalid > 0`, `tooManyValues()` after an Apply whose plan is `Disabled`), `table_filter_op_1` / `_2` (Select of `conditionLabel`s), `table_filter_operand_1` / `_2` and `table_filter_second_1` / `_2` (a TextInput, or on a `Date`/`DateTime` column a `DateTimeInput` in `Date`/`DateTime` mode — the date picker; the second only for `between`), `table_filter_case` (Checkbox `match case`, Text columns), `table_filter_combine` (Select `and`/`or`), `table_filter_unit_1` / `_2` (Select of the column's units, `Measure` only), `table_filter_match` and `table_filter_count` (group menus: `in any column`, `in every column`, `in at least N columns`, `in exactly N columns`, `all of them, each in a column`; the last only while 2 to k values are ticked), `table_filter_apply` (Button, disabled while nothing is ticked), `table_filter_clear` (Button `Clear filter`)
  - the `Display unit` header submenu (Measure columns) is 12a's (`HeaderMenuInput::dimensions`, entries `{dimension}: {unit}`); the lowering fills `dimensions` from the description's `units` and each `chosen` from `layout.units[column][dimension]`; choosing an entry sets `layout.units[column][dimension]`, and choosing the checked one removes it
- Rules:
  - **Values or conditions.** A column's filter is either its values part or its conditions part (12a's `classifyFilter`). Reopening the menu shows the part the stored filter is, the other part empty; editing one part clears the other's draft. A user who wants both writes conditions only.
  - **When it applies.** Ticks and "(Select all)" are recorded as `TickOp`s and applied only by Apply, which resolves them (paging each search's values at `maxLimit`), plans the side, and sets the filter; a `Disabled` plan applies nothing and puts `tooManyValues()` in the note. Conditions apply live: an edit of an operator, an operand, the unit, `match case` or `and`/`or` is sent `tableFilterDebounce` after the last such edit, provided every operand canonicalises (`canonicalOperand`) and a `between` has both ends; otherwise nothing is sent and the operand shows its error. Apply on the conditions part sends a pending edit at once. A sent filter calls `TableRows::setFilter`, sets the held `filters` and runs `onFiltersChange`.
  - **The values list** asks `DistinctRequest{column, search, offset, limit = 200}`, the search sent `tableFilterDebounce` after the last keystroke; a group menu asks with `group`. A `Measure` column's list draws `table_filter_heading_<dimension>` before each dimension's first value.
  - **Dates.** A `Date`/`DateTime` column's list is a tree: it asks `bucket = Year`; opening a year asks `bucket = Month, search = "<year>-"`; opening a month asks `bucket = Day, search = "<year>-<month>-"`, each paged at 200 like any list. Its ticks are a set of buckets, not `TickOp`s, and its filter is include-only (`planValueFilter` is not used), as table.md's "Distinct values" bullet **Dates.** states:
    - with no values filter on the column the tree opens with every box unticked and draws no `(Select all)`; with one, the buckets whose range a stored `between` covers are ticked;
    - ticking a year or a month covers the buckets under it, whose boxes then show ticked and disabled;
    - Apply writes `valueFilter(Side::Include, members, blanks, ValueForm::Ranges)`: one `between` per ticked bucket (a covered child adds none), in ascending order of its first end whatever the order of ticking, `combine = Any`, `isEmpty` beside them when `(Blanks)` is ticked; with no box ticked it clears the column's values filter;
    - one column's entries are bounded by `QueryLimits::maxEntries` (32, `query.hpp:112`), which the server's `validate` enforces. Before sending, buckets that touch (the next begins the day, or the second, after the previous ends) are merged into one `between`; a selection still over 32 entries applies nothing and puts `tableText::tooManyDates()` in `table_filter_note`.
  The conditions take dates from the picker.
  - **Groups.** A declared group's menu pools its values (`DistinctRequest::group`). A user-formed group (a layout group, index decision #26) has no values list: its menu sends no `DistinctRequest`, shows no search, `(Select all)` or value boxes, and offers the conditions of its columns' kind (a user-formed group is of one kind, 12e) and the match line's four quantifiers (`all of them, each in a column` takes ticked values, so it is not offered). Its filter is a `GroupFilter` with `group` empty and `columns` the layout group's columns in group order (Part C's `compileGroup` takes exactly one of `group` and `columns`), `match` from the match line, `combine` from `and`/`or`, `include` the condition entries; that one object is what the held `filters`, the stored state and the query carry. The lowering finds a layout group's filter as the `GroupFilter` whose `columns` equal the group's columns; removing the group (`RemoveGroup`) removes that filter with it.

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_model.cpp`:

```cpp
// Mutation: the menu counting over the view (the column's own filter applied).
TEST_CASE("filter menu: values are counted over what the other filters leave, and ticks build the smaller side",
          "[ui][table][config]")
// rows table `t` over 7 rows, status: Open×3, Closed×2, Held×1, empty×1; site: A×5, B×2; filter on site eq "A" leaves
// Open×2, Closed×2, empty×1 → clickFilter(table, <status>): table_filter_value_0 label "Closed (2)", _1 "Open (2)",
// no "Held"; table_filter_blanks label "(Blanks)"; untick "Open", Apply: t.filters.columns.status ==
// {"combine":"any","exclude":[{"oneOf":["Open"]}],"include":[]} — then reopen: "Open (2)" is listed unticked;
// onFiltersChange ran once; the header's `filtered` prop names the status column. chooseMenu(table, nullopt,
// {"Clear all filters"}): t.filters == {"columns":{},"groups":[]}, `filtered` is empty, onFiltersChange ran twice.

// Mutation: Apply writing the ticks and the conditions into one entry list.
TEST_CASE("filter menu: values and conditions are exclusive, and reopening shows the stored part", "[ui][table][config]")
// status (Text): tick only "(Blanks)", Apply → include [{"isEmpty":true}]; reopen: table_filter_blanks checked, every
// value unticked, table_filter_op_1 empty. Then choose "contains", operand "pe": after 250 ms t.filters.columns.status ==
// {"combine":"any","include":[{"contains":"pe"}],"exclude":[]} and the ticks are gone; reopen shows the condition and
// (Select all) ticked. A Custom column `code` filtered {"include":[{"isEmpty":true}]} reopens with table_filter_op_1
// "is empty" and no values list.

// Mutation: the search sent on every keystroke (no debounce).
TEST_CASE("filter menu: a search typed quickly is one distinct request after the pause", "[ui][table][config]")
// kConfigModelTable over samplesDescription(5), first page replied: open the menu on `name` (one Table.samples call with
// `distinct`, replied by hand); type "a","ab","abc" 50 ms apart in table_filter_search;
// h.advance(249ms): no new call; h.advance(1ms): exactly one more, whose body's distinct.search == "abc".   (D9 gate)

// Mutation: the condition sent on every keystroke, or a half-typed between sent.
TEST_CASE("filter menu: a condition applies once after typing pauses, and an incomplete between is not sent",
          "[ui][table][config]")
// kConfigModelTable, `name` (Text), op "contains": type ten letters 150 ms apart into table_filter_operand_1:
// h.advance(249ms) after the last: no new page request; h.advance(1ms): exactly one, filtering contains the ten
// letters (B4 gate). c3 (Decimal), op "between", operand_1 "2", no second: h.advance(1s): no request; second "5": one,
// include [{"between":["2","5"]}]; edit operand_1 to "3" and press Apply at once: the request goes without waiting.

// Mutation: the note line ignoring `partial`.
TEST_CASE("filter menu: a partial list says it may grow and invalid cells are counted under it", "[ui][table][config]")
// kConfigModelTable; reply the distinct request with {"values":[{"text":"x","operand":"x","count":4}],"distinct":1,
// "blanks":0,"invalid":3,"partial":true}: table_filter_note text == "The list may grow while data loads\n3 cells could not
// be read"; no checkbox for the invalid cells.

// Mutation: "(Select all)" under a search ticking only the values the page shows.
TEST_CASE("filter menu: Select all under a search covers every match, and both sides over the bound refuse", "[ui][table][config]")
// rows table of 25,000 rows, `code` "c00000".."c24999": untick (Select all), search "c0" (10,000 matches, 200 shown),
// tick (Select all), Apply: include [{"oneOf": the 10,000 operands "c00000".."c09999"}], not the 200 shown — resolved by
// ten distinct requests of limit 1000. Untick (Select all), tick (Select all) under "c1" (10,000) and under "c2" (5,000),
// Apply: T = 15,000, U = 10,000 → exclude [{"oneOf": "c00000".."c09999"}], named by paging every value. Untick
// (Select all), tick it under "c1", "c00" and "c01" (T = 12,000, U = 13,000), Apply: table_filter_note == "Too many
// values to list; filter with a condition" and t.filters is unchanged.

// Mutation: conditions offered by Text for every kind; an operand sent as typed.
TEST_CASE("filter menu: conditions follow the column's kind, and typed operands are canonical", "[ui][table][config]")
// Integer column: table_filter_op_1 options "=","≠","<","≤",">","≥","between"; Bool column: no table_filter_op_1;
// Custom column: "is empty","is not empty"; a Date column: table_filter_operand_1 is a DateTimeInput (Date mode).
// Under a session locale "de": a Decimal column, ">" and operand "1.234,5" → include [{"gt":"1234.5"}]; a Measure column
// `weight` (mass: g, kg): table_filter_unit_1 offers "g","kg"; ">" with "0,3 kg" → [{"gt":"0.3 kg"}]; ">" with "250" and
// unit "g" → [{"gt":"250 g"}]; "12,5 xx" shows an error on the operand and sends nothing.

// Mutation: the Measure list drawn without dimension headings.
TEST_CASE("filter menu: a Measure column's values are grouped under a heading per dimension", "[ui][table][config]")
// kConfigModelTable; distinct reply values 0.3 kg (mass), 300 g counted as one with it, 0.5 % (ratio): table_filter_heading_mass
// precedes table_filter_value_0, table_filter_heading_ratio precedes the ratio value.

// Mutation: the group menu sending `together` as one oneOf.
TEST_CASE("filter menu: a group's values are pooled and the match line builds the quantifier", "[ui][table][config]")
// rows table with "groups": [{"id":"company","label":"Company","columns":["companyA","companyB","companyC"]}] and the five
// rows of the spec's truth table; the table's `columnGroups` prop is "[Company:0+3]" (the three stand side by side) and
// clickGroupFilter(table, 0) opens the group's menu; after moving companyB to the end it is "[]" and the menu is reached
// from the header and table menus only; menuOf(table, <companyA>) has "Filter Company, 3 columns…"; its values list counts
// F 4, S 2, X 4, Y 2, Z 1 (rows having it in any column: F in rows 1 to 4, S in 1 and 3, X in 1, 2, 4 and 5, Y in 4 and 5, Z in 5); tick F and S, match "all of them, each in a column", Apply:
// filters.groups == [{"group":"company","mode":"together","include":[{"eq":"F"},{"eq":"S"}]}] and the view holds rows 1
// and 3 only; match "in at least N columns", N 2: rows 1, 2, 3.

// Index decision 26. Mutations: a user-formed group's menu sending DistinctRequest::group with its layout id, a group
// the source does not declare; the filter sent with "group":"g1" (Part C's compileGroup answers UnknownColumn for it).
TEST_CASE("filter menu: a user-formed group offers conditions and the match line, no values list", "[ui][table][config]")
// columns a, b (Text) formed into "Pair" (12e's chooser, id g1): chooseMenu(table, <a>, {"Filter Pair, 2 columns…"}):
// no table_filter_search, table_filter_all or table_filter_value_* widget, and no distinct request is sent;
// table_filter_op_1 offers the Text conditions and table_filter_match the four quantifiers (not "all of them, each in a
// column"); "equals" "F" with "in every column", after 250 ms: the request's filters.groups ==
// [{"columns":["a","b"],"match":"all","include":[{"eq":"F"}]}] with no "group" member (table.md "Column groups":
// `match` is "any", "all", {"atLeast": n} or {"exactly": n}), and t.filters.groups holds the same one object; reopening the
// group's menu shows "equals" "F" and "in every column"; table_group_remove_g1 in the chooser (12e) empties
// t.filters.groups.

// Mutation: the date tree filtering months on the client (an opened year past 200 months shows nothing).
TEST_CASE("filter menu: a date column lists years, then a year's months, then a month's days", "[ui][table][config]")
// kConfigModelTable over a description whose c3 is a Date column: opening its menu sends distinct {"bucket":"year"}; the reply
// lists 2025, 2026; table_filter_open_1 (2026) sends {"bucket":"month","search":"2026-"}; opening March sends
// {"bucket":"day","search":"2026-03-"}; every box starts unticked and there is no table_filter_all; tick March, then 2025
// (in that order), Apply: include [{"between":["2025-01-01","2025-12-31"]}, {"between":["2026-03-01","2026-03-31"]}]
// (ascending, not the order of ticking) with combine "any"; reopen: 2025 and March are ticked, 2026 is not.
// Mutation: adjacent buckets sent as one entry each. Tick the 31 days of March and the first 9 of April one by one (40
// touching days): Apply sends one entry, {"between":["2026-03-01","2026-04-09"]}.
// Mutation: a selection over maxEntries sent as it is (the server refuses the query). Tick 33 days no two of which
// touch (every second day from 2026-03-01): Apply sends nothing, t.filters is unchanged and table_filter_note ==
// "Too many dates to list; filter with a condition".

// Mutation: the unit choice written to the column's own unit instead of the layout.
TEST_CASE("display unit: the header menu writes the layout's units", "[ui][table][config]")
// Measure column `weight` (dimension mass, units g and kg): chooseMenu(table, <weight>, {"Display unit", "mass: g"}):
// t.layout.units == {"weight":{"mass":"g"}}; no new page request.
```

In `tests/net/test_push_tables.cpp` (Linux, recorded, not gates):

```cpp
TEST_CASE("table benchmark: a sort change and a text filter over a socket with 50 ms each way", "[.benchmark][table]")
// B3: the server's handler waits 50 ms before running each request and 50 ms before sending each reply; a header sort on
// `wide` (100,000 × 300) to the accepted page (≤ 150 ms), and the last key of a text filter to the accepted page
// (≤ 250 ms + 150 ms).
TEST_CASE("table benchmark: a server-mode values list from open to shown", "[.benchmark][table]")   // D9, ≤ 150 ms
```

**From Task 15 (Part F), which landed first.** The values list labels each entry with the column's display, not the
reply's `text`: `table_filter_value_<i>` is `ui::distinctTexts(values, column, locale, units)[i] + " (" + count + ")"`
(`include/morph/ui/cell_format.hpp`; `column` the shown column's `table::TableColumn` with the document's display
parts over the description's, `units` the table's `DisplayUnits`). Two values that show alike keep their own places
there. Add to Step 1 a new case, "filter menu: the values list labels by the column's display": over
`companionDescription()`'s `mass` (2 places, half up), distinct values `0.124`, `0.123` and `0.5` label `0.124 (1)`,
`0.123 (2)` and `0.50 (1)`. Its name falls under this task's Step 2 filter (`filter menu`). Mutation: the entry
labelled with the reply's `text`.

- [ ] **Step 2: Run them to see them fail.** Run: `cmake --build --preset cl-debug --target morph_tests && ctest --test-dir build/cl-debug -R "filter menu|display unit" --output-on-failure`. Expected: FAIL — no `table_filter` dialog is mounted.

- [ ] **Step 3: Implement** `TableRows::distinctValues` in both implementations, `tableFilterDebounce`, the `groups` key, the menu and the `Display unit` submenu per the rules above.

- [ ] **Step 4: Run them to see them pass.** The Step 2 command, then all `[ui]` cases. Expected: all pass.

- [ ] **Step 5: Record B3 and D9** on Linux: `cmake --preset linux-everything && cmake --build build/linux-everything --target morph_net_tests && build/linux-everything/tests/net/morph_net_tests "table benchmark: a sort change*" "table benchmark: a server-mode values list*"`; write the figures into table.md's B3 and D9 rows.

- [ ] **Step 6: Mutations, docs, commit.** Commit: `feat(ui): the filter menu with values, conditions, groups and units`.

### Task 12e: The column chooser and the sort dialog

**Files:**
- Create: `include/morph/ui/table_dialogs.hpp` (both dialogs' view trees)
- Modify: `src/ui/interpreter.cpp`
- Test: `tests/test_ui_table_model.cpp`
- Docs: `docs/spec/qt_quick/backend.md` (the dialogs)

**Interfaces:**
- Consumes: 12a's gestures and `FormGroup`; 12c's lowering.
- Produces: `Columns…` opens a `ui::Dialog` `table_columns` listing every column of the description in layout order (a companion column is not listed once Task 15 marks companions): per column a Checkbox `table_columns_show_<id>` (its label; disabled for a locked column and for the only visible one), Buttons `table_columns_up_<id>` and `table_columns_down_<id>`; Buttons `table_columns_reset` (`Reset columns`) and `table_columns_group` (`Group columns…`, offered while `hide` or `reorder` is allowed), which shows `table_group_label` (TextInput), `table_group_column_<id>` Checkboxes (after the first is ticked, only columns of its kind stay enabled), `table_group_create` (Button, enabled at 2 to 16 ticked and a label), and under them one line per user-formed group of the layout, `table_group_<id>` (Text, its label and column count, `Pair, 2 columns`) with Button `table_group_remove_<id>` (`Remove group`), which applies `RemoveGroup{id}` and, per 12d's rule, removes the group's filter; `Sort…` opens `table_sort` listing the chain: per key `i` from 0 a Select `table_sort_column_<i>` (sortable columns), Select `table_sort_direction_<i>` (`ascending`, `descending`), Buttons `table_sort_up_<i>`, `table_sort_down_<i>`, `table_sort_remove_<i>`; Button `table_sort_add` (`Add key`, adds the first sortable column not in the chain; at the bound it refuses with the message). Both dialogs apply each change as it is made, through the same gesture functions; a `Done` button and a dismissal close them.

- [ ] **Step 1: Write the failing tests:**

```cpp
// Mutation: the chooser listing only the visible columns.
TEST_CASE("column chooser: every column in order, with show, move and reset", "[ui][table][config]")
// kNine with c3 hidden: table_columns_show_c3 is listed unchecked in third place; check it: t.hiddenCount == 0;
// table_columns_down_c1: t.visibleColumns starts "c2","c1"; table_columns_reset: declared order.

// Mutation: group creation accepting columns of two kinds.
TEST_CASE("column chooser: Group columns forms a group of one kind, which the layout keeps", "[ui][table][config]")
// columns a, b (Text) and n (Integer): tick table_group_column_a → table_group_column_n disabled; tick b, label
// "Pair", create: t.layout.groups == [{"id":"g1","label":"Pair","columns":["a","b"]}]; menuOf(table, <a>) holds
// "Filter Pair, 2 columns…"; the chooser lists table_group_g1 "Pair, 2 columns".
// Mutation: the Remove group button closing the line without applying RemoveGroup.
// click table_group_remove_g1: t.layout.groups == [], menuOf(table, <a>) no longer holds "Filter Pair, 2 columns…",
// and onLayoutChange ran once more.

// Mutation: the sort dialog's move applying to the wrong neighbour.
TEST_CASE("sort dialog: keys in order with direction, move and remove; Add key refuses at the bound", "[ui][table][config]")
// sort c1 asc, c2 desc: table_sort_up_1 → [c2 desc, c1 asc]; table_sort_direction_0 "ascending" → c2 asc;
// table_sort_remove_1 → [c2 asc]; with eight keys table_sort_add leaves eight and shows "A table sorts by at most 8 columns".
```

**From Task 15 (Part F), which landed first.** The column chooser lists no companion column (the columns of
`table::companionColumns(description)`), even one the description marks `shown`. In Step 1, **extend the existing
case** "table display: companion columns are not arranged" in `tests/test_ui_table_model.cpp` (do not add a second
case of that name): over `companionDescription()` the chooser (`Columns…`, dialog `table_columns`) has
`table_columns_show_id`, `_mass`, `_name` and `_count`, and no `table_columns_show_massTone`, `_massTip` or
`_lockedBy`. Mutation: the chooser listing every column of the description.

- [ ] **Step 2: Run to see them fail.** `ctest --test-dir build/cl-debug -R "column chooser|sort dialog|companion columns are not arranged" --output-on-failure` after building `morph_tests`. Expected: FAIL — no `table_columns` dialog.
- [ ] **Step 3: Implement** the dialogs and their openers.
- [ ] **Step 4: Run to see them pass**, then all `[ui]` cases.
- [ ] **Step 5: Mutations, docs, commit.** Commit: `feat(ui): the column chooser and the sort dialog`.

### Task 12f: Copy, and the screens

**Files:**
- Modify: `include/morph/ui/table_rows.hpp` (`TableRows::fetchRows`), `src/ui/interpreter.cpp`
- Test: `tests/test_ui_table_model.cpp`
- Docs: `docs/spec/ui/interpreter.md` (copy), `docs/spec/ui/document.md` (`maxCopyRows` named in the copy rule)

**Interfaces:**
- Consumes: 12a's `copyText`, `tableText`; 12b's `onCopy`, `copyText`; Part D's `tableMessage` (Task 9d, index decision 11); Part D's `ui::detail::standInCellText(table::Cell const&, table::TableColumn const&)` (replaced by Task 15's `ui::formatCell(table::Cell const&, table::TableColumn const&, ui::FormatContext const&, DisplayUnits const& = {})`, which copy calls with the session's `FormatContext` and the layout's display units) and its range selection (Task 10a), whose bound this task's Review Focus case holds at its edge.
- Produces:
  - `struct FetchedRow { table::RowId key; std::int64_t viewRow = 0; std::vector<table::Cell> cells; }`; `TableRows::fetchRows(std::vector<table::RowId> keys, std::vector<std::string> columns) -> async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>>` (pure virtual; `EngineRows` answers at once; `WindowRows` sends keyed queries naming `columns`, at most `QueryLimits::maxKeys` keys each, through the read that makes no cache entry, and returns the rows the replies hold with their view rows; a key no reply holds is not returned)
- Rules: `Ctrl+C` and `Copy` copy the selected rows in view order by the visible columns in layout order, each cell's display text; with no row selected the focused cell's text; `Copy with headers` adds the labels' line. Over `maxCopyRows` selected keys it refuses with `copyBound` and copies nothing; otherwise it fetches the rows the window does not hold, writes `copyText`, calls `IViewBackend::copyText`, and shows `copied(rows, unloaded)`; every message goes through `tableMessage`.

- [ ] **Step 1: Write the failing tests:**

```cpp
// Mutation: copy writing rows in selection order instead of view order.
TEST_CASE("copy: selected rows as tab-separated display text in layout order, rows not held fetched", "[ui][table][config]")
// kConfigModelTable over samplesDescription(5) sorted by name, c3 hidden, `name` moved first; the page holds view rows 0-29;
// select keys 9 and 2 (view rows 40 and 3); pressCopy(table): one keyed Table.samples request naming only the visible
// columns and keys [9] (the held row is not
// fetched); clipboardText(h) == "<row 3 cells>\n<row 40 cells>" with "\t" between cells in the arrangement's
// order; statusOf(h).message "Copied 2 rows". chooseMenu(table, nullopt, {"Copy with headers"}) prefixes the labels' line.
// No row selected: the focused cell's text alone. A rows table whose `name` column has its own `cell` (a badge reading
// `row.name`) copies the value's text, not the template's.

// Mutation: unloaded cells copied as their placeholder text.
TEST_CASE("copy: unloaded cells copy as empty and are counted", "[ui][table][config]")
// kConfigModelTable whose page reply names both rows in `unloadedRows` for level `recipe` (hand-written reply; c4 of that
// level): select the two rows, copy: their c4 cells are empty fields;
// message "Copied 2 rows, 2 cells were not loaded"; no request waits for the level.

// Review Focus 3. Mutation: the bounds compared with `>=`.
TEST_CASE("range and copy at their bounds: 10,000 pass, 10,001 are refused", "[ui][table][config]")
// kConfigModelTable, pageJson total 100,000, nothing held past the first page: selectTableRange(table, 0, 9'999): keysOf
// fetches pages naming no columns (each body's "columns" is []), t.selectedCount == 10000; pressCopy: copies 10,000 lines;
// selectTableRange(table, 0, 10'000): statusOf(h).message "A range selects at most 10,000 rows", selection unchanged;
// with 10,001 keys selected (a 10,000-row range, then a Ctrl-click), pressCopy: "A copy holds at most 10,000 rows",
// clipboardText(h) unchanged (still the 10,000 lines).
```

(`selectTableRange` is the `RecordingBackend` driver Part D's Task 10a adds for its range report. Replies the helpers of `tests/ui_table_support.hpp` do not build (distinct values, keyed replies, `unloadedRows`) are written as JSON in the test.)

**From Task 15 (Part F), which landed first.** Copy writes each cell as the table shows it, with
`ui::formatCell(cell, column, scope->formatting(), displayUnits)`, and each header with `ui::headerText(label, unit)`
(`include/morph/ui/cell_format.hpp`), before `ui::copyText(labels, rows)` and `IViewBackend::copyText`. Add to Step 1
a new case, "copy: writes the shown text and the header with its unit" (Task 15's brief planned it as "table
display: copy writes …"; it was never written, and this name falls under Step 2's `copy:` filter): over
`companionDescription()` with `"selectionMode":"multiple"` and `"columns":[{"id":"id"},{"id":"mass"},{"id":"name"}]`,
select row 1 and choose "Copy with headers": `clipboardText()` is `"Id\tMass [kg]\tName\n1\t0.13\tA"`. Mutation: copy
writing `table::displayText`, which writes `0.125`.

- [ ] **Step 2: Run to see them fail.** `ctest --test-dir build/cl-debug -R "copy:|range and copy" --output-on-failure` after building `morph_tests`. Expected: FAIL — `fetchRows` does not exist; the clipboard stays empty (the range half of the bounds case passes already: it holds Part D's rule at its edge, and is here so the two bounds are tested side by side).
- [ ] **Step 3: Implement** `fetchRows` in both `TableRows` and the copy rule above.
- [ ] **Step 4: Run to see them pass**, then the whole of `morph_tests`, `morph_table_tests`, and in `build/cl-qt-debug` `morph_qt_quick_tests` and `morph_tui_tests`: all pass.
- [ ] **Step 5: The screens.** Existing tables now draw a `⋯` table-menu button, filter buttons on filterable headers and, on model-backed tables, a status line. This repository checks in no pixel baselines (`git ls-files '*.png'` lists none); screens are grabbed by `scripts/shoot_screens.sh` and `scripts/shoot_lims.sh`, which run the example clients with `QT_QPA_PLATFORM=offscreen --grab`. The user runs, on a Linux tree with Qt Quick and examples (`cmake --preset linux-everything && cmake --build build/linux-everything --target morph_examples_client morph_lims_client`), once on `master` and once on this branch:
  `scripts/shoot_screens.sh /tmp/grabs-<rev> build/linux-everything/examples/client/morph_examples_client` and `scripts/shoot_lims.sh /tmp/lims-grabs-<rev> build/linux-everything/examples/client/morph_lims_client`, and compares each pair: the only differences are the three intended ones. The authoring goldens (`tests/ui/golden/*.json`) do not change, since no builder writes the new keys; confirm with `ctest --test-dir build/cl-debug -R "golden" --output-on-failure` (a golden that does change is regenerated with `MORPH_UPDATE_GOLDENS=1 build/cl-debug/tests/morph_tests.exe "[golden]"` and its diff reviewed in this commit).
- [ ] **Step 6: Mutations, docs, commit.** Commit: `feat(ui): copy a table's selection`.

---

## Task 13: Collections through the same lowering (table.md step 8)

**As landed (093110af and its fix round).** No `TableSurface` or `lowerTableSurface`: factoring the `rows` table's
node-reading lowering would have rewritten most of it while Task 17a changed the same file. A collection keeps a
`RowsTable` in its element (no source, no page) made by `ScreenCore::wireCollectionTable`, and
`ScreenCore::lowerCollectionRows` mounts it; it shares `configureTable` (gestures, menus, chooser, sort dialog, filter
menu, copy), the status line's message and gesture routing, the stored state's key and debounced saves, and the
saved views' `TableViews` with every other table, and has its own columns, cells, key translation and status line.
A per-table rule a later task adds to the lowering is therefore wired in three places: `makeRowsTable`/`lowerRows`,
`wireModelTable`/`modelTableOf`, and `wireCollectionTable`/`lowerCollectionRows`. The interface text below is the
design as planned.

**Spec:** "Collections and master-detail"; "Every surface on the pipeline" (the collection row); gate U5 (recorded); the collection half of the Tests row "One order on every surface".

**Files:**
- Modify: `include/morph/forms/engine/collection_view.hpp:31-46` (`CollectionChrome::rows`), `:177-227` (`collectionRows` calls it when set), `include/morph/forms/engine/collection.hpp:366-430` (nothing new in the session; its `source()` is the list's `DataSource`), `src/ui/interpreter.cpp:7033-7055` (`lowerComposite` sets `chrome.rows`) and the table lowering (factored as below)
- Test: `tests/test_ui_table_model.cpp`, `tests/bench_interpreter.cpp`, `tests/ui_table_support.hpp` (adds `rowKeys`, index decision 20); existing `tests/test_ui_interpreter_forms.cpp` and `tests/test_forms_engine_collection_view.cpp` stay green unchanged
- Docs: `docs/spec/forms/engine.md` ("Collections": the rows through the table lowering), `docs/spec/ui/interpreter.md` (a collection's table), table.md (U5 figure)

**Interfaces:**
- Consumes: Part D's `EngineRows(std::shared_ptr<table::DataSource>, RecordOf, table::EngineOptions)` over `session.source()` (a `RecordOf` reading the collection row's key and its cells as a record), the state store and `SessionOptions::application`; 12c's lowering.
- Produces:
  - `forms::CollectionChrome::rows : std::function<ui::Node(CollectionSession&, std::function<ui::Node(std::string const& rowKey)> const& buttons)>` — replaces the rows table; `buttons` is empty when the view has no Open or row action
  - in `src/ui/interpreter.cpp`: `lowerTableSurface` — the part of the table lowering below reading the node. It reads no `DocNode`: everything 12c to 12f and 14 read from a `table` node reaches it as a member of the surface, so the document `table` node fills every member from its keys and a collection leaves the defaults. The types:
    - `struct SurfaceColumn { std::string id; std::string label; table::ColumnKind kind = table::ColumnKind::Text; bool shown = true; bool sortable = false; bool filterable = false; bool locked = false; ui::TableColumn look; CellView cell; }` — `look` carries the column's `width`, `role`, `align`, `valign`, `wrap` and `shrink` (its `label`, `id`, `sortable` and `filterable` are overwritten from the members beside it); `cell` is the column's own cell view, of the type 9d's `TableSource::cellView` returns for one column (index decision 28), empty for the formatted text cell;
    - `struct SurfaceValue { ExprPtr bound; DocCommands onChange; }` — `bound`: the node's expression when the document binds the value, null when the table holds it; `onChange`: what `DocNode::event` returns for the value's change key, run with `event` the new value;
    - `struct TableSurface { std::shared_ptr<TableRows> rows; std::vector<SurfaceColumn> columns; std::vector<table::ColumnGroupInfo> groups; std::function<Node(Key const&)> trailingCell; std::string trailingLabel; std::function<std::string(std::string const& column, Key const&)> cellTestId; RowView rowView; ui::TableOptions options; SurfaceValue layout; SurfaceValue sort; SurfaceValue filters; Configure configurable = Configure::All; bool menus = true; std::optional<bool> status; std::optional<table::TableStateKey> stateKey; std::size_t maxSortKeys = 8; }` — `groups`: the declared groups (12d); `rowView`: 9d's row-level view (tone, label), empty for none; `menus`: false under `configurable: false`; `status`: the node's literal, absent for the default; `stateKey`: absent when the table persists nothing (`persist: false`, a `rows` table with no `id`);
    - `Node ScreenCore::lowerTableSurface(TableSurface surface, LowerCtx const& ctx, ElementState* element)`.
    What only a `rows` table has — `group` headings, `pageSize`/`page`, `rowSpans`, `empty` — stays in the document table's own code around the call. `CellView`, `RowView` and `DocCommands` stand for the types Part D's lowering already uses for these three things; the executor writes the landed names
  - a collection's surface: columns from `CollectionModel::columns()` (`id = field`, `label`, `kind`, `shown = !hidden`, sortable and filterable), the trailing buttons column (label empty), cell test ids `cell_<field>_<rowKey>`, options from `forms::detail::engine::rowsTableOptions(session)` (test id `collection_rows`, master-detail selection), state key `{application = SessionOptions::application, model = the node's model type, table = the collection's action, qualifier = the screen's id, part}`
  - test support, this task's (no earlier task defines it; later tasks reuse it by name): `std::vector<std::string> rowKeys(ui::testing::TableHarness& h, int table)` in `tests/ui_table_support.hpp` (index decision 42 names it; if 9d has landed a `rowKeys` of this meaning, this task reuses it and adds none) — the `rowKey` property of each of `h.backend.children(table)` that has one, in child order (the order the mount placed the realized rows), with a string key's surrounding quotes removed (`RecordingBackend`'s `formatKey` quotes text keys and not integer ones), so a collection keyed by text and a rows table keyed by integer compare equal

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_model.cpp`, over this document in `tests/ui_table_support.hpp` (`Lims_ListSamples` is in `corpusCatalog()`, which `TableHarness`'s catalog includes; the rows table holds the rows the test replies to the collection, so both surfaces have the same four):

```cpp
/// The Samples collection beside a rows table `t` over the same four rows.
inline constexpr std::string_view kCollectionBesideTable = R"({
  "vocab": "ui/1", "id": "both",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "rows": { "init": { "lit": [ { "id": 1, "name": "beta" }, { "id": 2, "name": "Alpha" },
                                          { "id": 3, "name": "álpha" }, { "id": 4, "name": "Gamma" } ] } } },
  "view": { "kind": "column", "children": [
    { "kind": "collection", "model": "sample", "action": "Lims_ListSamples" },
    { "kind": "table", "id": "t", "testId": "t", "rows": { "ref": "rows" },
      "columns": [ { "id": "id", "label": "ID", "sortable": true },
                   { "id": "name", "label": "Name", "sortable": true } ] } ] } })";
```

`h.reply`, `h.dispatcher.last`, `h.find`, `h.byTestId` and `h.text` are the members `TableHarness` has in common with the existing `Harness` (`tests/ui_interpreter_support.hpp:560-603`); where 9d left one out, the test reads the same thing through `h.backend`.

```cpp
// Mutation: the collection keeping the reply's order (chrome.rows not set by the interpreter).
TEST_CASE("collection: a collection and a rows table over the same rows show the same order and selection", "[ui][table]") {
    ui::testing::TableHarness h;
    auto screen = h.mount(kCollectionBesideTable);
    std::string const rows = R"([{"id":1,"name":"beta"},{"id":2,"name":"Alpha"},{"id":3,"name":"álpha"},{"id":4,"name":"Gamma"}])";
    h.reply(h.dispatcher.last("Lims_ListSamples"), R"({"rows":)" + rows + "}");
    int const collection = h.byTestId("Table", "collection_rows");
    int const table = h.find("Table", "testId", "t");
    // A header index counts every column of `Table::columns`, hidden ones included (the surface lays out every
    // column and hides through `Table::arrangement`): ID is index 0 and hidden, Name is index 1 on both tables.
    REQUIRE(h.backend.prop(collection, "columns").starts_with("[ID"));
    h.backend.clickHeader(collection, 1, ui::HeaderClick::Plain);   // Name
    h.backend.clickHeader(table, 1, ui::HeaderClick::Plain);        // Name
    h.settle();
    CHECK(rowKeys(h, collection) == rowKeys(h, table));
    CHECK(rowKeys(h, collection) != std::vector<std::string>{"1", "2", "3", "4"});
    CHECK(h.text("cell_name_2") == "Alpha");
}

// Mutation: the master-detail selection lost when the engine reorders.
TEST_CASE("collection: a master-detail selection and its open row hold through a sort", "[ui][table]")
// master-detail view over the same rows: selectByUser(collection_rows, {3}) opens row 3; clickHeader(collection, 1, Plain)
// (Name): the table's selection prop still marks 3; detailForm visible; the editor's sampleId field reads 3.

// Mutation: the collection's state saved under the screen's rows key.
TEST_CASE("collection: header and table menus are present and its arrangement persists under its action", "[ui][table]")
// TableHarness(store) over a MemoryTableStateStore: menuOf(collection, 1) (Name) holds "Hide column"; choose it, h.advance(500ms):
// one save whose key is {application "lims", model "Lims_SampleModel", table "Lims_ListSamples", qualifier the screen's
// id, part "state"}.
```

Extend Task 11b's `"one order on every surface"` case with the collection over the same rows (the same sort gives the same key order on all three surfaces). In `tests/bench_interpreter.cpp`:

```cpp
TEST_CASE("interpreter bench: a collection of 20,000 rows: mount, sort, one row edited", "[.benchmark][interpreter]")   // U5
```

- [ ] **Step 2: Run to see them fail.** `ctest --test-dir build/cl-debug -R "collection:" --output-on-failure` after building `morph_tests`. Expected: FAIL — the collection's order is the reply's.
- [ ] **Step 3: Implement** `CollectionChrome::rows`, `TableSurface`, `lowerTableSurface` and the collection's surface as above.
- [ ] **Step 4: Run to see them pass**, then `ctest --test-dir build/cl-debug -R "collection|forms engine collection|one order on every surface" --output-on-failure` and the whole of `morph_tests`. Expected: all pass, existing collection tests unchanged.
- [ ] **Step 5: Record U5** with `build/cl-release/tests/morph_tests.exe "interpreter bench: a collection of 20,000 rows*"` into table.md's U5 row.
- [ ] **Step 6: Mutations, docs, commit.** Commit: `feat(ui): collections on the table pipeline`.

---

## Task 14: Saved and shared views (table.md step 9)

**Spec:** "Saved views", "Persisted state" ("What is stored", "Two windows, two devices", "The framework's stores"), the Tests rows "Views are not lost between windows" and "Shared views", gate T2. Three commits.

### Task 14a: The views document, its operations, and publishing on the server

**Files:**
- Modify: `include/morph/table/state_store.hpp` (views document, operations, `ITableStateStore::publish`, `MemoryTableStateStore` sharing), `include/morph/ui/table_state_model.hpp` (`publish`; a user's `save` of `shared` refused), `include/morph/ui/table_state.hpp` (`PreferencesTableStateStore::shares()`, `publish`)
- Test: `tests/table/test_table_state.cpp`, `tests/test_ui_table_state.cpp`
- Docs: `docs/spec/ui/preferences.md` (the `shared` part kept with no person); `docs/spec/table/table.md` is not edited for these: its `ITableStateStore` block in "Persisted state" already declares `publish` (with its default refusal) and `kNoDocument`, with the paragraph saying what the sentinel means; "Two windows, two devices" already says a `views` save from a table that loaded nothing sends `kNoDocument`; "The framework's stores" already names `MemoryTableStateStore`'s `Sharing` and its default, `Private`. Check: `grep -n "kNoDocument\|Sharing::Private" docs/spec/table/table.md` prints those lines.

**Interfaces:**
- Consumes: Part D's `TableStateKey`, `SaveOutcome`, `SaveResult`, `ITableStateStore`, `MemoryTableStateStore`, `TableStateModel`, `TableStateSaved`, `registerTableState`, `fitState`; Task 8d's `ColumnLayoutValue` (with `groups`, `units`).
- Produces (namespace `morph::table`):
  - `struct SavedView { std::string name; ColumnLayoutValue layout; SortChain sort; FilterSpec filters; }`
  - `enum class ViewsPart : std::uint8_t { Own, Shared }`
  - `struct ViewsDocument { int version = 1; std::optional<std::string> defaultView; bool defaultOnMount = false; std::vector<SavedView> views; }` (JSON `{"version", "default", "defaultOnMount", "views"}`; a `Shared` document writes neither `default` nor `defaultOnMount`)
  - operations `AddView{SavedView}`, `ReplaceView{SavedView}`, `RenameView{std::string from; std::string to;}`, `RemoveView{std::string name}`, `SetDefaultView{std::optional<std::string> name}`, `SetDefaultOnMount{bool on}`; `using ViewOperation = std::variant<…>`; JSON `{"add": view}`, `{"replace": view}`, `{"rename": {"from", "to"}}`, `{"remove": name}`, `{"default": name|null}`, `{"defaultOnMount": bool}`
  - `enum class ViewError : std::uint8_t { NameTaken, NoSuchView, NotShared }`; `[[nodiscard]] std::expected<ViewsDocument, ViewError> applyViewOperation(ViewsDocument document, ViewOperation const& operation, ViewsPart part)` (`AddView` and `RenameView` onto a taken name → `NameTaken`; `ReplaceView`, `RenameView`, `RemoveView`, `SetDefaultView` naming no view → `NoSuchView`; `SetDefaultView`/`SetDefaultOnMount` on `Shared` → `NotShared`; removing the default view clears `defaultView`)
  - `[[nodiscard]] std::expected<ViewsDocument, TableError> readViews(std::string_view json)` (an unknown `version` → `InvalidSpec`), `[[nodiscard]] std::string writeViews(ViewsDocument const&, ViewsPart part)`; an operation's JSON through `[[nodiscard]] std::string writeViewOperation(ViewOperation const&)` and `[[nodiscard]] std::expected<ViewOperation, TableError> readViewOperation(std::string_view)`; `[[nodiscard]] std::string viewErrorText(ViewError, ViewOperation const&)` (`A view named {name} exists`, `No view named {name}`, `The shared views have no default`)
  - `inline constexpr std::string_view kNoDocument = "-";` — an `expected` revision that stores only when nothing is stored under the key and answers `Conflict` otherwise; `MemoryTableStateStore` and `TableStateModel` honour it in `save` and `publish` (a `views` part saved by a table that loaded nothing sends it, so two windows that both start from no views cannot overwrite each other)
  - `ITableStateStore::publish(TableStateKey const& key, ViewOperation const& operation, std::string expected) -> async::Completion<SaveResult>` (pure virtual: the interface owns no executor to deliver a default answer on; a store that does not share returns the protected `static async::Completion<SaveResult> refusePublish(exec::IExecutor&)`, which answers `Refused` with reason `"this store does not share views"` on the store's executor; the test store `GatedTableStateStore` does so)
  - `enum class Sharing : std::uint8_t { Private, Shared }`; Part D's constructor gains a defaulted parameter, `explicit MemoryTableStateStore(exec::IExecutor& executor, Sharing sharing = Sharing::Private)`, so every Part D call `MemoryTableStateStore(executor)` compiles unchanged and keeps `shares() == false`; `shares()` is `sharing == Sharing::Shared`; its `publish` applies the operation to the `shared` part under the `expected` rule (`kNoDocument` included); over `Private` its `publish` keeps the base class's refusal
- Produces (namespace `morph::ui`): `struct TableStatePublish { table::TableStateKey key; std::string operation; std::string expected; }` (the key's `part` must be `"shared"`; `operation` the operation's JSON) → `TableStateSaved`, registered as `morph.tableState`'s action `publish` by `registerTableState`; the model applies it to the person-less `shared` entry (preferences `person` empty: no verified caller is ever empty, since the model refuses one with none), comparing `expected` with the head's revision (`Conflict` with the current revision when they differ), refuses `SetDefaultView`/`SetDefaultOnMount`, and refuses a caller with no verified person; its `save` with `part == "shared"` answers `Refused` (`"the shared views are written by publish"`); `PreferencesTableStateStore::shares()` returns `true` and its `publish` calls `morph.tableState.publish`.

- [ ] **Step 1: Write the failing tests.** In `tests/table/test_table_state.cpp`:

```cpp
// Mutation: AddView over a taken name appending a second view of that name.
TEST_CASE("views document: operations apply to what is stored, and names stay unique", "[table][state]") {
    table::ViewsDocument doc;
    auto const add = [](std::string name) { return table::AddView{table::SavedView{.name = std::move(name)}}; };
    doc = *table::applyViewOperation(doc, add("Open orders"), table::ViewsPart::Own);
    doc = *table::applyViewOperation(doc, table::SetDefaultView{"Open orders"}, table::ViewsPart::Own);
    // Compared with std::unexpected, not through .error(): under the mutation the result holds a value, and .error()
    // on it aborts the run instead of failing the check.
    CHECK(table::applyViewOperation(doc, add("Open orders"), table::ViewsPart::Own) == std::unexpected(table::ViewError::NameTaken));
    doc = *table::applyViewOperation(doc, add("Late"), table::ViewsPart::Own);
    CHECK(table::applyViewOperation(doc, table::RenameView{"Late", "Open orders"}, table::ViewsPart::Own).error() ==
          table::ViewError::NameTaken);
    doc = *table::applyViewOperation(doc, table::RemoveView{"Open orders"}, table::ViewsPart::Own);
    CHECK_FALSE(doc.defaultView);
    CHECK(table::applyViewOperation(doc, table::SetDefaultOnMount{true}, table::ViewsPart::Shared).error() ==
          table::ViewError::NotShared);
    CHECK(table::writeViews(doc, table::ViewsPart::Shared) ==
          R"({"version":1,"views":[{"name":"Late","layout":{"order":[],"hidden":[],"pinned":0,"widths":{},"groups":[],"units":{}},"sort":[],"filters":{"columns":{},"groups":[]}}]})");
    CHECK(table::readViews(R"({"version":2,"views":[]})").error().code == table::TableErrorCode::InvalidSpec);
}
```

(The expected `layout` text follows `ColumnLayoutValue`'s writer as Task 8d lands it; keep the rest exact.)

In the same file, the sentinel and the sharing default on the memory store (completions delivered on the file's `StepExecutor` and read through 9b's `saveNow`/`loadNow` and a `publishNow` beside them; an `exec::InlineExecutor` is not serial, so a `Completion` refuses it as its owner; the code below is written with `resultOf`, read it as those helpers). Revisions are asserted as the values the store returned, never as literals:

```cpp
// Mutation: MemoryTableStateStore treating kNoDocument as an ordinary revision (the first save answers Conflict).
// Mutation: the constructor's Sharing defaulting to Shared (the default store offers publishing).
TEST_CASE("memory table state store: kNoDocument stores only over nothing, and a default store does not share",
          "[table][state]") {
    exec::InlineExecutor executor;
    table::MemoryTableStateStore store{executor};
    CHECK_FALSE(store.shares());
    table::TableStateKey const key{.application = "lims", .model = "", .table = "t", .qualifier = "s", .part = "views"};
    table::SaveResult const first = resultOf(store.save(key, R"({"version":1,"views":[]})", std::string{table::kNoDocument}));
    REQUIRE(first.outcome == table::SaveOutcome::Saved);
    table::SaveResult const again = resultOf(store.save(key, R"({"version":1,"views":[]})", std::string{table::kNoDocument}));
    CHECK(again.outcome == table::SaveOutcome::Conflict);
    CHECK(again.revision == first.revision);
    CHECK(resultOf(store.load(key))->revision == first.revision);
    table::SaveResult const refused = resultOf(store.publish(key, table::AddView{table::SavedView{.name = "X"}},
                                                             std::string{table::kNoDocument}));
    CHECK(refused.outcome == table::SaveOutcome::Refused);
    CHECK(refused.reason == "this store does not share views");
    table::MemoryTableStateStore shared{executor, table::Sharing::Shared};
    CHECK(shared.shares());
    auto sharedKey = key;
    sharedKey.part = "shared";
    table::SaveResult const published = resultOf(shared.publish(sharedKey, table::AddView{table::SavedView{.name = "X"}},
                                                                std::string{table::kNoDocument}));
    REQUIRE(published.outcome == table::SaveOutcome::Saved);
    CHECK(resultOf(shared.publish(sharedKey, table::AddView{table::SavedView{.name = "Y"}},
                                  std::string{table::kNoDocument})).revision == published.revision);   // Conflict
}
```

In `tests/test_ui_table_state.cpp`, over the server harness Task 9 built for `morph.tableState` (`RemoteServer`, a recording `IPreferencesStore`, calls as a chosen token). The model's revisions are digests (Part D: the first 16 hex digits of `sha256(previousRevision + document)`), so every case below captures the revision a call returned and asserts against that value, never against `"1"` or `"2"`:

```cpp
// Mutation: publish written under the caller's person (bob does not see it).
TEST_CASE("shared views: published by one principal, read by another", "[ui][table][state][remote]")
// alice publish {"add": {"name":"Mine",…}} with expected "-" → Saved with revision r1 (16 hex digits); bob load part
// "shared" → a document holding "Mine" whose revision == r1; the store's calls show the write under person "" and the
// read under person "".

// Mutation: the model treating kNoDocument as an ordinary revision in `save` (the first save answers Conflict).
TEST_CASE("table state model: a save with kNoDocument stores over nothing and conflicts over something",
          "[ui][table][state][remote]")
// alice save part "views" with expected "-" → Saved r1; again with expected "-" → {"outcome":"conflict"} whose
// revision == r1; the store holds the first document.

// Mutation: the authoriser consulted with action "save" for a publish.
TEST_CASE("shared views: an authoriser that refuses publish refuses it by action id", "[ui][table][state][remote]")
// an IAuthorizer refusing (morph.tableState, "publish") for token "bob": bob's publish answers an error envelope and the
// store sees no write; alice's succeeds.

// Mutation: save of part "shared" accepted.
TEST_CASE("shared views: a user's own save of the shared part is refused", "[ui][table][state][remote]")
// alice save {part "shared"} → {"result":{"outcome":"refused","reason":"the shared views are written by publish"}}; no write.

// Mutation: publish ignoring `expected`.
TEST_CASE("shared views: a publish over a replaced revision is a conflict", "[ui][table][state][remote]")
// publish "A" with expected "-" → Saved r1; publish "B" with expected r1 → Saved r2, r2 != r1; publish "C" with
// expected r1 → {"result":{"outcome":"conflict","revision":r2}} (the revision compared is the one the second publish
// returned); the shared document holds A and B only.

// Mutation: every state written as segments.  (T2 gates)
TEST_CASE("table state: a full state is one entry and one call; a 20,000-value filter reads back equal through segments",
          "[ui][table][state][remote]")
// a state of 300 columns, 8 sort keys and 32 filters: one store `set` and one entry; its byte count is logged (INFO) for
// table.md's T2 row; a state whose filter ticks 20,000 values: reads back byte-equal, every segment entry ≤ 12 KiB; the
// same two documents as the `views` part behave alike.
```

In `tests/net/test_push_tables.cpp` (Linux): `"shared views: a view published over a socket is read by a second client"` — the first two remote cases above, over a real socket.

- [ ] **Step 2: Run to see them fail.** `cmake --build --preset cl-debug --target morph_tests morph_table_tests && ctest --test-dir build/cl-debug -R "views document|kNoDocument|shared views|table state: a full state" --output-on-failure`. Expected: compile errors for `ViewsDocument`, `kNoDocument`, `Sharing`, `TableStatePublish`.
- [ ] **Step 3: Implement** the interfaces above.
- [ ] **Step 4: Run to see them pass**; on Linux `ctest --test-dir build/linux-everything -R "shared views" --output-on-failure`.
- [ ] **Step 5: Mutations, docs, commit.** Record T2's bytes and entry counts in table.md. Run the Docs line's check, and the docs build, since `kNoDocument`, `Sharing` and `publish` are public. Commit: `feat(table): saved views, their operations and publishing`.

### Task 14b: Saved views on a table

**Files:**
- Create: `include/morph/ui/table_views.hpp`
- Modify: `include/morph/ui/table_config.hpp` (view menu commands), `src/ui/interpreter.cpp` (the `Views` submenu, the name dialog, mount behaviour), `tests/ui_table_support.hpp` (`RecordingTableStateStore`, `LiteralTableStateStore`, index decision 20)
- Test: `tests/test_ui_table_state.cpp`
- Docs: `docs/spec/ui/interpreter.md` (saved views), `docs/spec/ui/document.md` (the table menu's views), `docs/spec/table/table.md` is not edited: "Two windows, two devices" already states the retry when the reload finds nothing stored

**Interfaces:**
- Consumes: 14a; Part D's `TableStates` (9c) with its `[[nodiscard]] table::ITableStateStore& store() const noexcept` (the store it was built over) and `void keep(table::TableStateKey const& key, table::StoredTableState state)` (replaces the kept document of `key`; the next `load(key)` resolves to it with no call) — both declared and tested by 9c, index decision 32; this task adds nothing to `TableStates` — and `Session::tableStates()`, the state key, load at mount, save and fitting; 12c's arrangement.
- Produces:
  - `MenuCommand::{ApplyView, SaveView, RenameView, RemoveView, SetDefaultView, DefaultOnMount, ApplySharedView, PublishView, SaveAsMyView}`
  - `class TableViews` (`morph::ui`, `table_views.hpp`): `TableViews(TableStates& states, table::TableStateKey key)` (`key.part` is ignored; it reads and writes the parts `"views"` and `"shared"` of `key`); `load() -> async::Completion<std::expected<void, std::string>>` — calls `states.load` for the `views` key and, when `states.store().shares()`, for the `shared` key, so they join the turn's one batched `TableStateLoad` with the screen's `state` keys (gate T5) and a second mount reads them from the kept copies with no call; `own() -> table::ViewsDocument const&`; `shared() -> table::ViewsDocument const&`; `change(table::ViewOperation operation) -> async::Completion<std::expected<void, std::string>>` — applies the operation to the loaded document (an operation error answers at once, with no save) and saves through `states.store().save` with the loaded revision, or `kNoDocument` when nothing was loaded; on `Saved` calls `states.keep`; on `Conflict` loads again through `states.store().load` (not `states.load`, whose kept copy is the one that conflicted), keeps what it read, applies the operation to it and saves once more, with that document's revision, or with an empty `expected` when the reload finds nothing stored (a store that does not honour `kNoDocument` answers `Conflict` over nothing; a store that does cannot, unless the document was removed in between); a second `Conflict` answers `Views could not be saved: another window changed them at the same time`; an operation error after the reload answers `A view named {name} exists` or `No view named {name}` and saves nothing; `publish(table::ViewOperation)` the same through `states.store().publish` on the `shared` key; `shares() -> bool` (`states.store().shares()`)
  - the lowering builds one `TableViews` per mounted table that has a state key, over `Session::tableStates()`, and calls its `load()` in the same turn as the table's `state` load
- Rules: the table menu gains `Views` (a submenu): each own view (checked when it is the state's `view`; labelled `{name} (modified)` while the arrangement differs from the fitted view), separator, `Save view…`, `Rename view…`, `Remove view`, `Set as default`, `Start from the default view` (checkable). `Save view…` and `Rename view…` open `table_view_dialog` with `table_view_name` (TextInput) and `table_view_save` (Button); saving under a name the user already has is a `ReplaceView`. Applying a view fits it as stored state is fitted, sets `layout`, `sort` and `filters` (through their handlers where bound) and writes its name to the state's `view`; the status line shows `Saved view {name}` after a save. At mount: with no stored `state`, the `default` view is applied; with `defaultOnMount`, the default view's `sort` and `filters` are applied over the stored state's, and the stored `layout` still applies. A mounted table does not follow saves made elsewhere.

- **Split on landing.** The table menu is not lowered on a screen yet (Task 12a built its model in `table_config.hpp`; the lowering, `menuOf`, `chooseMenu` and the arrangement on screen are Tasks 12b to 12e). So 14b lands `TableViews`, the `MenuCommand` entries, and the lowering's views load in the turn of the state load; its cases drive `TableViews` over `TableStates` directly, as two windows are two `TableStates` over one store. `TableViews` also takes the session's owner (`TableViews(TableStates&, TableStateKey, exec::IExecutor& owner)`), where its completions are delivered: `TableStates` does not expose its own. The `Views` submenu, the name dialog, "Applying a view", the `(modified)` mark, the default view and `defaultOnMount` at mount, and the cases "the default view at mount, and Start from the default view" and "applying a view records its name, and a change marks it modified" move to Task 14c, after 12e; the taken-name case's "Save view… named A" is `ReplaceView` sent by the test, the rule the menu will apply, and its AddView mutation is 14c's. The first case's last clause (both windows change the sort; the state part holds B's whole arrangement) is 9c's behaviour, tested there, and is not repeated. A further case mounts a `rows` table and a model-backed table over a sharing `RecordingTableStateStore` and checks the `views` and `shared` loads beside `state`, and none on a second mount (mutation: the lowering's `views->load()` calls deleted).

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_state.cpp`. Two test stores join `tests/ui_table_support.hpp` here: `RecordingTableStateStore` (a map store under the `expected` rule, `kNoDocument` included, delivering on the executor it is given, that records each call as `{op, part, expected, outcome, revision}`, `op` one of `"load"`, `"save"`, `"publish"`; it does not wrap a `MemoryTableStateStore`, whose completions belong to its strand and cannot be attached to from the owner), and `LiteralTableStateStore` (a map store that knows no sentinel: it compares `expected` with the stored revision as text, nothing stored counting as revision `""`, so `expected "-"` over nothing answers `Conflict` with revision `""`; an empty `expected` replaces; revisions `"r1"`, `"r2"`, …). Revisions are asserted as the values a call returned, never as literals.

```cpp
// Mutation: a views save with no expected revision (the second window's save overwrites the first's view).
TEST_CASE("views: views are not lost between windows", "[ui][table][state]")
// two TableHarnesses (two sessions) over one RecordingTableStateStore, the same table: both mount with no views stored;
// window A saves view "A": the record shows save(views, expected "-") → Saved rA; window B saves view "B": save(views,
// expected "-") → Conflict carrying rA, then load(views), then save(views, expected rA) → Saved rB, rB != rA; the
// store's views part holds "A" and "B". Both mount again (new sessions: the views document read has revision rB) and
// each adds a view: the second's first save answers Conflict carrying the first's new revision, it reloads and saves
// once more; four views stored. Then A and B each change the sort: the state part holds B's whole arrangement (last
// save wins, no merge).

// Mutation: TableViews loading through the store directly rather than through TableStates (one call per part).
// Gate T5, extended to the views parts.
TEST_CASE("views: a screen's state, views and shared parts load in one call, and a second mount in none",
          "[ui][table][state]")
// 9c's five-table fixture over PreferencesTableStateStore (which shares): dispatcher.count("load") == 1 and that one
// TableStateLoad names 15 keys, parts "state", "views" and "shared" for each table; remountScreen():
// dispatcher.count("load") is still 1. Over a MemoryTableStateStore (Private): no key of part "shared" is loaded.

// Mutation: the retry after a Conflict resending kNoDocument (the second Conflict reports the views unsaved).
TEST_CASE("views: a store that ignores kNoDocument still saves the first view", "[ui][table][state]")
// TableHarness over LiteralTableStateStore, no views stored: Save view… named "A": the store saw save(views, expected "-")
// → Conflict with revision "", a load finding nothing, save(views, expected "") → Saved; status "Saved view A"; the
// stored views document holds "A".

// Mutation: defaultOnMount applying the default view's layout too.
TEST_CASE("views: the default view at mount, and Start from the default view", "[ui][table][state]")
// no state stored, views {"default":"Open orders", views:[Open orders: sort c2 desc, hidden c3]}: the first query sorts by
// c2 desc and c3 is hidden. Then state {sort c1 asc, layout hiding c4} and "defaultOnMount": true: the first query sorts
// by c2 desc and c4 (stored layout) is hidden, c3 is shown.

// Mutation: the modified mark compared against the unfitted view.
TEST_CASE("views: applying a view records its name, and a change marks it modified", "[ui][table][state]")
// chooseMenu(table, nullopt, {"Views", "Open orders"}): t.sort is the view's; after h.advance(500ms) the saved state has
// "view":"Open orders"; clickHeader(table, 0): menuOf(table, nullopt)'s Views submenu labels it "Open orders (modified)".
// A view whose filter names a removed column applies without that filter.

// Review Focus 5. Mutations: Save view… with a taken name sending AddView (refused as NameTaken, the sort change lost);
// change() on Conflict saving its own document with the new revision instead of applying the operation to the reloaded
// one (window 2's view "C" is lost, or two views are named "C").
TEST_CASE("views: a taken name is replaced on save and refused on rename", "[ui][table][state]")
// Window 1 over a RecordingTableStateStore holding views "A" and "B" at revision r0. Save view… named "A" after a sort
// change: one save(views, expected r0) → Saved r1; one view "A" with the new sort, two views in all. Rename view… on
// "B" to "A": status "A view named A exists", no save is recorded, views unchanged.
// Then window 2 (a second TableHarness over the same store) mounts, reads r1, and saves view "C" → Saved r2. Window 1, which
// still holds r1 and no "C", renames "B" to "C": the record shows save(views, expected r1) → Conflict carrying r2, then
// load(views) (the reload: the first load of that key since window 1 mounted), and no further save; status
// "A view named C exists"; the stored document is window 2's, views "A", "B", "C" at revision r2.
```

- [ ] **Step 2: Run to see them fail.** `ctest --test-dir build/cl-debug -R "views:" --output-on-failure` after building `morph_tests`. Expected: compile error for `ui::TableViews` (`morph/ui/table_views.hpp` missing); with the header stubbed, FAIL — no `Views` entry in the table menu.
- [ ] **Step 3: Implement** `TableViews`, the menu, the dialog and the mount rules. table.md "Two windows, two devices" already says that a table that loaded no views saves with `kNoDocument`, and that when the reload after a `Conflict` finds nothing stored the one retry saves with an empty `expected`; check with `grep -n "retry saves with an empty" docs/spec/table/table.md`.
- [ ] **Step 4: Run to see them pass**, then the whole of `morph_tests`.
- [ ] **Step 5: Mutations, docs, commit.** Apply each named mutation, see its case fail, revert. Run the docs build (`TableViews` is public). Commit: `feat(ui): saved table views`.

### Task 14c: Saved views in the table menu, and shared views

Needs the table menu on a screen (Tasks 12b to 12e: the menu's lowering, `menuOf`, `chooseMenu`, and 12c's
arrangement). 14b landed `TableViews`, the `MenuCommand` entries and the views load at mount, and no menu; what a
user touches of the saved views is this task's, below, with the shared views.

A collection's table is a third wiring site (Task 13 as landed): `wireCollectionTable` builds its `TableViews` and
`lowerCollectionRows` mounts its menu, so the `Views` submenu, applying a view and the default view at mount are
wired there too, beside the `rows` table's and the model table's, with a collection case.

**Files:**
- Modify: `include/morph/ui/table_views.hpp`, `include/morph/ui/table_config.hpp` (the `Views` submenu's entries, built from the `MenuCommand` entries 14b declared), `src/ui/interpreter.cpp` (the `Views` submenu, the name dialog `table_view_dialog`, applying a view, the status messages, the default view and `defaultOnMount` at mount)
- Test: `tests/test_ui_table_state.cpp`, `tests/net/test_push_tables.cpp`
- Docs: `docs/spec/ui/interpreter.md` (the `Views` submenu, applying a view, the default view at mount, shared views), `docs/spec/ui/document.md` (the table menu's views)

**Interfaces:**
- Consumes: 14a's `publish`; 14b's `TableViews` (`load`, `own`, `shared`, `change`, `publish`, `shares`), its `MenuCommand::{ApplyView, SaveView, RenameView, RemoveView, SetDefaultView, DefaultOnMount, ApplySharedView, PublishView, SaveAsMyView}` and the `TableViews` each table with a state key holds and loads at mount; 12b–12e's table menu and arrangement. 14b delivered no menu.
- Produces, the user's own views: the table menu gains `Views` (a submenu): each own view (checked when it is the state's `view`; labelled `{name} (modified)` while the arrangement differs from the fitted view), separator, `Save view…`, `Rename view…`, `Remove view`, `Set as default`, `Start from the default view` (checkable). `Save view…` and `Rename view…` open `table_view_dialog` with `table_view_name` (TextInput) and `table_view_save` (Button); saving under a name the user already has is a `ReplaceView`. Applying a view fits it as stored state is fitted, sets `layout`, `sort` and `filters` (through their handlers where bound) and writes its name to the state's `view`; the status line shows `Saved view {name}` after a save. At mount: with no stored `state`, the `default` view is applied; with `defaultOnMount`, the default view's `sort` and `filters` are applied over the stored state's, and the stored `layout` still applies. A mounted table does not follow saves made elsewhere.
- Produces, shared views: the `Views` submenu gains, while `shared().views` is not empty, a heading `Shared views` and each shared view (applied as an own view is), and `Save as my view` while the current view is a shared one (an `AddView` of it into the user's views); while `shares()`, `Publish view…` (the name dialog, then a `publish` of `AddView`, or `ReplaceView` over a taken shared name); a refused publish shows `This view could not be published: {reason}`, a done one `Published view {name}`. Over a store whose `shares()` is false none of the publish entries is offered.

- [ ] **Step 1: Write the failing tests:**

```cpp
// Mutation: the shared part saved through `save` instead of `publish` (an authoriser cannot tell it from own state).
TEST_CASE("shared views on a table: publish, read by another person, Save as my view; none over a store that does not share",
          "[ui][table][state][remote]")
// two TableHarnesses with PreferencesTableStateStore against one server harness, tokens alice and bob: alice's
// chooseMenu(table, nullopt, {"Views", "Publish view…"}) named "Team", dispatched as action "publish" (never "save");
// bob mounts: his Views submenu has heading "Shared views" and "Team"; applying "Team" then "Save as my view" adds
// "Team" to bob's own views. An authoriser refusing bob's publish: status starts "This view could not be published:".
// A TableHarness over MemoryTableStateStore(executor) (Sharing defaults to Private): no "Publish view…" entry, and no load of
// part "shared". Alice's publish result is asserted by the revision it returned, read back by bob's load.
```

```cpp
// Mutation: defaultOnMount applying the default view's layout too.
TEST_CASE("views: the default view at mount, and Start from the default view", "[ui][table][state]")
// no state stored, views {"default":"Open orders", views:[Open orders: sort c2 desc, hidden c3]}: the first query sorts by
// c2 desc and c3 is hidden. Then state {sort c1 asc, layout hiding c4} and "defaultOnMount": true: the first query sorts
// by c2 desc and c4 (stored layout) is hidden, c3 is shown.

// Mutation: the modified mark compared against the unfitted view.
TEST_CASE("views: applying a view records its name, and a change marks it modified", "[ui][table][state]")
// chooseMenu(table, nullopt, {"Views", "Open orders"}): t.sort is the view's; after h.advance(500ms) the saved state has
// "view":"Open orders"; clickHeader(table, 0): menuOf(table, nullopt)'s Views submenu labels it "Open orders (modified)".
// A view whose filter names a removed column applies without that filter.

// Mutation: Save view… with a taken name sending AddView (refused as NameTaken, the sort change lost).
TEST_CASE("views: Save view… under a taken name replaces that view", "[ui][table][state]")
// Window 1 over a RecordingTableStateStore holding views "A" and "B" at revision r0: after a sort change,
// chooseMenu(table, nullopt, {"Views", "Save view…"}), table_view_name "A", table_view_save: one save(views, expected
// r0) → Saved r1; one view "A" with the new sort, two views in all; the status line shows "Saved view A".
```

In `tests/net/test_push_tables.cpp`: the publish-then-read flow over a real socket. Gate T2 is held by 14a.

- [ ] **Step 2: Run to see them fail.** `ctest --test-dir build/cl-debug -R "shared views on a table" --output-on-failure`. Expected: FAIL — no `Publish view…` entry.
- [ ] **Step 3: Implement** the `Views` submenu and its own-view entries, `table_view_dialog` (a taken name saved as a `ReplaceView`), applying a view (fitted, through the handlers where bound, its name written to the state's `view`), the `{name} (modified)` mark, `Saved view {name}`, the default view and `defaultOnMount` at mount; then the shared entries and the publish flow.
- [ ] **Step 4: Run to see them pass**, then `morph_tests`; on Linux `ctest --test-dir build/linux-everything -R "shared views" --output-on-failure`.
- [ ] **Step 5: Mutations, docs, commit.** Each case's named mutation run once. Docs: interpreter.md (the `Views` submenu, applying a view, the default view at mount, shared views) and document.md (the table menu's views). Commit: `feat(ui): saved and shared table views`.

---

## Task 16: A raised sort bound (table.md step 9c)

**Spec:** "Sort chains" (the bound); Failure modes "A table whose `limits` set `maxSortKeys` above 32" and "A sort key past the table's `maxSortKeys`"; the Tests row "A raised sort bound". Depends only on Task 12.

A collection's bound is `config.maxSortKeys` in `wireCollectionTable` (8; a collection has no `maxSortKeys` key):
a rule that changes the bound reaches it there too.

**Files:**
- Modify: `include/morph/table/description.hpp` (`servedDescription`; `table::detail::answerQuery` takes its description from it and answers its `SourceContract`), `table::DescribeTable::invoke` in the same header (the `Table.<id>.describe` handler `BRIDGE_REGISTER_TABLE` registers obtains the description through it; `registry.hpp` only names the action type), `src/ui/document.cpp` (`maxSortKeys` on `table`), `src/ui/interpreter.cpp` (the bound the lowering passes to `applySortGesture`: the description's on a model-backed table, the literal on a `rows` table, else 8)
- Test: `tests/table/test_table_description.cpp`, `tests/test_ui_table_model.cpp`, `tests/test_ui_document_rules.cpp` (its `rowsTable` and `modelTable` helpers)
- **Split:** the server half, the document key and its load check are Task 16's; the lowering's bound (`src/ui/interpreter.cpp`) and the `tests/test_ui_table_model.cpp` case below are owned by Task 12c ("Carried from Task 16" there), because the lowering that calls `applySortGesture` is 12c's and did not exist when Task 16 ran. The `test_ui_table_model.cpp` case and the interpreter bound below are kept for reference; 12c writes them.
- Docs: `docs/spec/ui/document.md` (`maxSortKeys`), `docs/spec/core/registry.md` (the bound carried from `limits`)

**Interfaces:**
- One revision. After this task every server path that names a table's revision takes it from `servedDescription`: `answerQuery` (the page's `revision`), the `describe` handler, and whatever Parts B to F have added by then (the view cache's key, `resolve`). Step 3 ends with `grep -rn "describe(served" include src` listing `servedDescription` alone; a path left on `describe(served.description)` would answer a page whose revision differs from the description's whenever `limits.maxSortKeys` is not 8, and the client would refetch the description on every page.
- Consumes: Task 2's `ServedTable::limits` (`QueryLimits`, whose `maxSortKeys` defaults to 8); Task 1's `TableCapabilities::maxSortKeys`; 12a's `applySortGesture`.
- Produces: `inline constexpr std::size_t kMaxSortKeysBound = 32;` and `[[nodiscard]] std::expected<TableDescription, TableError> servedDescription(ServedTable const& served)` in `morph::table` — `describe(d)` of a copy `d` of `served.description` with `d.capabilities.maxSortKeys = static_cast<std::uint32_t>(served.limits.maxSortKeys)` (`QueryLimits::maxSortKeys` is a `std::size_t`, the capability a `std::uint32_t`), the bound set before the revision is taken because `capabilities` is part of the canonical JSON `describe` hashes; or `SourceContract` (message ``"a table's `limits.maxSortKeys` is 1 to 32"``) when it is 0 or above `kMaxSortKeysBound`; the document key `maxSortKeys` on `table`: a literal integer 1 to 32, a `rows` table's only (on a model-backed table refused at load with ``"`maxSortKeys` is a `rows` table's; a model's table carries its bound in its description"``; out of range refused with ``"`maxSortKeys` is 1 to 32"``).

- [ ] **Step 1: Write the failing tests.** In `tests/table/test_table_description.cpp`:

```cpp
// Mutation: servedDescription leaving capabilities at the default 8.
TEST_CASE("served description: limits carry the sort bound, up to 32", "[table][description]") {
    table::ServedTable served{.description = threeColumns()};
    auto const eight = table::servedDescription(served);
    REQUIRE(eight);
    CHECK(eight->capabilities.maxSortKeys == 8);
    CHECK(eight->revision.size() == 16);
    CHECK(eight->revision == table::describe(*eight).revision);
    served.limits.maxSortKeys = 32;
    auto const wide = table::servedDescription(served);
    REQUIRE(wide);
    CHECK(wide->capabilities.maxSortKeys == 32);
    CHECK(wide->revision != eight->revision);   // Mutation: the revision taken before the bound is set.
    served.limits.maxSortKeys = 33;
    CHECK(table::servedDescription(served).error().code == table::TableErrorCode::SourceContract);
    served.limits.maxSortKeys = 0;
    CHECK(table::servedDescription(served).error().code == table::TableErrorCode::SourceContract);
}
```

(`threeColumns()` is a file-local helper returning a `TableDescription` of `id` Integer key, `name` Text and `c3` Decimal, built as this file's Task 1 cases build theirs. `samplesDescription` is not used here: it lives in `tests/ui_table_support.hpp`, the UI harness header of `morph_tests`, and this file is in `morph_table_tests`, which links `morph::morph` and `morph_test_main` only.)

In the same file, the page's revision:

```cpp
// Mutation: answerQuery hashing served.description as the model gave it (the page names a revision no describe reply has).
TEST_CASE("served description: a page carries the served description's revision", "[table][description]")
// a ServedTable over a VectorSource of three rows with limits.maxSortKeys = 32, built as this file's Task 2 answerQuery
// cases build theirs: table::detail::answerQuery(served, {})->revision == table::servedDescription(served)->revision;
// with limits.maxSortKeys = 33 answerQuery answers SourceContract.
```

In `tests/test_ui_table_model.cpp`:

```cpp
// Mutation: the client holding 8 whatever the description says.
TEST_CASE("sort chains: a table whose limits set 32 accepts 32 keys from the header and refuses the 33rd", "[ui][table][config]")
// kConfigModelTable over samplesDescription(40) whose capabilities.maxSortKeys is 32: Shift-click 33 sortable headers: t.sort
// has 32 keys, the last page request's sort has 32 keys, status message "A table sorts by at most 32 columns".
// A rows table with "maxSortKeys": 12 over 13 sortable columns: 12 keys, the 13th refused with "…at most 12 columns".
```

In `tests/test_ui_document_rules.cpp`: `"ui document rules: table: maxSortKeys is a rows table's literal from 1 to 32"` — `33` refused, `{"ref":"n"}` refused, on a model-backed table refused with the reason above.

- [ ] **Step 2: Run to see them fail.** `cmake --build --preset cl-debug --target morph_tests morph_table_tests && ctest --test-dir build/cl-debug -R "served description|accepts 32 keys|maxSortKeys is a rows" --output-on-failure`. Expected: compile error for `servedDescription`; the 32-key case stops at 8.
- [ ] **Step 3: Implement** `servedDescription`, call it from `table::detail::answerQuery` and from the `describe` handler (a `SourceContract` answers the request), the document key and the lowering's bound.
- [ ] **Step 4: Run to see them pass**, then `morph_table_tests` and `morph_tests`.
- [ ] **Step 5: Mutations, docs, commit.** Commit: `feat(table): a sort bound up to 32 keys`.

---

## Order within this part

12a → 12b → 12c → 12e → 12d, with 12f anywhere after 12c (12d's user-formed-group case forms and removes its group through 12e's chooser; each of the three touches the lowering, so they merge one at a time) → 13 → 14a → 14b → 14c. Task 16 needs only 12c and may run beside 13 and 14. All of Task 12 needs Part D's Tasks 9 to 11 (the lowering, the status line, the arrangement); Task 13 needs Tasks 9 and 11; Task 14 needs Task 9's store and model.
