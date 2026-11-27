// SPDX-License-Identifier: Apache-2.0

// What the split `table model` test files share: the helpers and constants that more than one of them uses.

#pragma once

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ui_table_support.hpp"

namespace table = morph::table;

namespace table_model_support {

using morph::ui::Value;
using morph::ui::testing::RowsFixture;
using namespace std::chrono_literals;
using morph::ui::Key;
using morph::ui::ValueColumn;
using morph::ui::ValueRecord;
using morph::ui::ValueRowsSource;
using morph::ui::testing::kNine;
using morph::ui::testing::TableHarness;
using morph::ui::testing::value;

// The revision of the description the harness answered last.

std::string revisionOf(TableHarness const& h);

// Whether a `Table.samples` call from @p first on asks for the page at @p offset.

bool askedForPage(TableHarness const& h, std::size_t first, std::int64_t offset);

// Clicks the button with @p testId, then settles.

void click(TableHarness& h, std::string_view testId);

// Whether the call at @p index was stopped.

bool stopped(TableHarness const& h, std::size_t index);

// A document with the model alias `sample` and @p view.

std::string doc(std::string_view view, std::string_view mutations = "{}");

// `id` (the key), `mass` (two places, half up, in kg, toned by `massTone` and described by `massTip`), `name`,
// `count`, and the companions `massTone`, `massTip` and `lockedBy`, the row's lock.

table::TableDescription companionDescription();

inline constexpr std::string_view kTable =
    R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples"})";

using CompanionRow = std::map<std::string, table::Cell, std::less<>>;

// The row the companion cases show: mass 0.125, toned "warn", described "Above limit"; `count` holds text, which an
// Integer column cannot read.

CompanionRow companionRow(std::int64_t key);

// The page answering the latest query with @p rows, each holding the columns the query names.

std::string companionPage(TableHarness const& h, std::vector<CompanionRow> const& rows = {companionRow(1)});

Value rec(ValueRecord fields);

Value qty(std::string_view amount, std::string unit);

ValueColumn col(std::string id);

ValueColumn col(std::string id, table::ColumnKind kind);

std::shared_ptr<ValueRowsSource> sourceOf(RowsFixture& h, std::vector<ValueColumn> columns,
                                          std::shared_ptr<table::UnitRegistry const> units = {});

std::size_t columnIndex(table::DataSource const& source, std::string_view column);

table::ColumnKind kindOf(table::DataSource const& source, std::string_view column);

// A document's text with a top-level `state` entry @p name initialised to @p init (JSON).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `withState(text, name, init)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::string withState(std::string_view text, std::string_view name, std::string_view init);

// A document's text whose table's first column part takes the member @p member, written `"key":value`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `withFirstColumnKey(text, member)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::string withFirstColumnKey(std::string_view text, std::string_view member);

// The commands that count a table event's runs in `n` and keep its last `event` in `last`.

inline constexpr std::string_view kCount =
    R"([{"set":"n","value":{"fn":"add","args":[{"ref":"n"},1]}},{"set":"last","value":{"ref":"event"}}])";

// kNine with a counter `n` and `last`, and @p event running `kCount`.

std::string countingNine(std::string_view event, std::string_view text = kNine);

// Presses a dialog's button by its test id, then settles.

void press(morph::ui::testing::TableHarness& h, std::string_view testId);

// Whether a widget, found by its kind and test id, is enabled.

bool enabled(morph::ui::testing::TableHarness const& h, std::string_view kind, std::string_view testId);

// A rows table `t` of the Text columns a, b and c and the Integer column n.

inline constexpr std::string_view kKinds = R"({
  "vocab": "ui/1", "id": "kinds",
  "state": { "items": { "init": { "lit": [ { "id": 1, "a": "x", "b": "y", "c": "z", "n": 1 } ] } } },
  "view": { "kind": "table", "id": "t", "rows": { "ref": "items" },
            "columns": [ { "id": "a", "label": "A" }, { "id": "b", "label": "B" }, { "id": "c", "label": "C" },
                         { "id": "n", "label": "N", "kind": "integer" } ] } })";

// A rows table `t` of `status` (Open×3, Closed×2, Held×1 and one empty) and `site` (A×5, B×2), counting the runs of
// `onFiltersChange` in `n`.

inline constexpr std::string_view kStatusRows = R"({
  "vocab": "ui/1", "id": "status",
  "state": { "n": { "init": { "lit": 0 } }, "items": { "init": { "lit": [
    { "id": 1, "status": "Open", "site": "A" }, { "id": 2, "status": "Open", "site": "A" },
    { "id": 3, "status": "Open", "site": "B" }, { "id": 4, "status": "Closed", "site": "A" },
    { "id": 5, "status": "Closed", "site": "A" }, { "id": 6, "status": "Held", "site": "B" },
    { "id": 7, "site": "A" } ] } } },
  "view": { "kind": "table", "id": "t", "rows": { "ref": "items" },
            "columns": [ { "id": "status", "label": "Status" }, { "id": "site", "label": "Site" } ],
            "onFiltersChange": [ { "set": "n", "value": { "fn": "add", "args": [ { "ref": "n" }, 1 ] } } ] } })";

// Whether a widget of a kind with a test id is mounted.

bool mounted(TableHarness const& h, std::string_view kind, std::string_view testId);

// A widget's property, found by its kind and test id.

std::string propOf(TableHarness const& h, std::string_view kind, std::string_view testId, std::string_view name);

// Toggles a check box of the filter menu, then settles.

void toggleBox(TableHarness& h, std::string_view testId);

// Ticks or unticks "(Select all)" as a user who wants every value under the search ticked, or unticked, does: a box
// that already shows the wanted state is toggled twice, since it shows only the values listed.

void selectAll(TableHarness& h, bool ticked);

// Chooses an option of one of the filter menu's selects by its label, then settles.

void chooseOption(TableHarness& h, std::string_view testId, std::string_view label);

// Types into one of the filter menu's text inputs, then settles.

void typeInto(TableHarness& h, std::string_view testId, std::string_view text);

// Closes the filter menu as a dismissal does.

void dismissMenu(TableHarness& h);

// Presses one of the filter menu's buttons, then settles.

void pressButton(TableHarness& h, std::string_view testId);

// A select's `options` property for options whose keys are their labels.

std::string optionsOf(std::vector<std::string> const& labels);

// A distinct-values reply against @p revision: a page with no rows whose `distinct` reads @p values.

std::string distinctReply(std::string const& revision, std::string_view values);

// The `Table.samples` calls that fetch a query's first page of rows: one per query the rows are given.

std::size_t pageCalls(TableHarness const& h);

// The latest `Table.samples` call that asks for values.

std::size_t lastDistinct(TableHarness const& h);

// The model table `t` over @p description, its first page answered.

std::unique_ptr<morph::ui::Screen> mountDescribed(TableHarness& h, table::TableDescription const& description,
                                                  std::string_view text = morph::ui::testing::kConfigModelTable);

// `id`, then a column of each kind a condition is offered by: `n` Integer, `b` Bool, `code` Custom, `d` Date, `dec`
// Decimal and `weight` a Measure in g or kg.

table::TableDescription kindsDescription();

// The days of a month as a distinct reply's values: `YYYY-MM-DD`, each a bucket of one day.

std::string dayValues(std::string_view month, int days);

/// The four rows of `kCollectionBesideTable`, as a `Lims_ListSamples` reply lists them.

inline constexpr std::string_view kFourSamples =
    R"({"rows":[{"id":1,"name":"beta"},{"id":2,"name":"Alpha"},{"id":3,"name":"álpha"},{"id":4,"name":"Gamma"}]})";

// id (Integer, key), status (Text), and the lock companion. Ten rows, keys 0..9: "open" for 0..5, "closed" for 6..9.

morph::table::TableDescription selectionDescription();

// One row of a page: its key, its status and its lock text.

struct SelRow {
    std::int64_t key;
    std::string status = "open";
    std::string lockedBy;
};

// A page reply of selectionDescription(), against the revision @p description has; a keyed reply carries `indices`,
// each row's view row from 0. @p extra is more members, written `"name":value,…`.

std::string selectionReply(std::vector<SelRow> const& rows, std::int64_t total, std::string_view viewRevision,
                           std::vector<std::int64_t> const& unmatched, bool keyed, std::string_view extra = {},
                           morph::table::TableDescription const& description = selectionDescription());

// A page reply of selectionDescription(): {"revision", "columns":["id","status","lockedBy"], "keys", "rows", "total",
// "offset": 0, "viewRevision", "unmatched"}.

std::string selectionPage(std::vector<SelRow> const& rows, std::int64_t total, std::string_view viewRevision,
                          std::vector<std::int64_t> const& unmatched = {});

std::vector<SelRow> openRows();

// The catalog entry of an action on Lims_SampleModel that declares `updatesTable` on `samples` and returns
// {"rows": ChangedRows}: the held record's keys are at "rows.rows", by the member "key".

morph::ui::ActionInfo samplesAction();

// An action whose member `rows` is a RowSelection of the table, and one whose member `row` is a RowRef of it.

inline constexpr std::string_view kSelectionSchema =
    R"({"type":"object","properties":{"rows":{"type":"object","x-selectionOf":"Lims_SampleModel.samples"}}})";

inline constexpr std::string_view kRowSchema =
    R"({"type":"object","properties":{"row":{"x-rowOf":"Lims_SampleModel.samples"}}})";

// The table the selection cases mount: filtered to open(), with the mutation `tag`, which takes its selection.

inline constexpr std::string_view kSelectionTable =
    R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
    "selectionMode":"multiple","filters":{"columns":{"status":{"include":[{"eq":"open"}]}}}})";

inline constexpr std::string_view kSelectionMutations = R"({
    "tag":{"model":"sample","action":"Lims_TagRows","body":{"obj":{"rows":{"ref":"t.selectionSet"}}}}})";

// The document of @p table and @p mutations: the table, and a button `run-<name>` running each mutation.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `selectionDoc(table, mutations, h)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::string selectionDoc(std::string_view table, std::string_view mutations, TableHarness& h);

// Mounts it and answers the describe request and the first page: six open rows, total 6, viewRevision "v1".

std::unique_ptr<morph::ui::Screen> mountSelection(TableHarness& h, std::string_view table = kSelectionTable,
                                                  std::string_view mutations = kSelectionMutations);

Key key(std::int64_t value);

table::RowId row(std::int64_t value);

// The document of the edit cases: a table `t` of `samples` of the alias `sample`, @p extra added to its node.

std::string editDoc(std::string_view extra = "");

// `samples` with `count` edited by `Lims_EditSpecimenCount`, and `label` and `remark` by `Lims_EditSpecimenRow`.

table::TableDescription editDescription();

// A row of the edit cases: name `row-<key>`, count @p count (the key by default), label `L<key>`, remark `R<key>`.

CompanionRow editRow(std::int64_t key, std::int64_t count = -1);

// The reply to the latest query, a page of @p rows from view row 0, or a keyed reply of them at @p indices.

std::string editPage(TableHarness const& h, std::vector<CompanionRow> const& rows,
                     std::vector<std::int64_t> indices = {});

// Adds the two edit actions to the harness's catalog, each declaring `updatesTable` on `samples`, and permits them,
// `Lims_EditSpecimenCount` only when @p count says so.

void addEditActions(TableHarness& h, bool count = true);

// Mounts @p text, answers the description and a page of rows 1 to 3.

std::unique_ptr<morph::ui::Screen> mountEdits(TableHarness& h, std::string const& text);

// Types @p text into the cell of view row @p viewRow in @p column of the table `t`, then leaves the cell with Tab.

void typeCell(TableHarness& h, std::size_t viewRow, std::string_view column, std::string const& text);

}  // namespace table_model_support
