// SPDX-License-Identifier: Apache-2.0

// The helpers the split `table model` test files share.

#include "ui_table_model_support.hpp"

#include "grid_test_support.hpp"

namespace table = morph::table;

using morph::ui::Value;
using morph::ui::testing::describeJson;
using morph::ui::testing::pageJsonAt;
using morph::ui::testing::RowsFixture;
using namespace std::chrono_literals;
using morph::ui::Key;
using morph::ui::Quantity;
using morph::ui::ValueColumn;
using morph::ui::ValueRecord;
using morph::ui::ValueRowsSource;
using morph::ui::testing::addAction;
using morph::ui::testing::kNine;
using morph::ui::testing::lastQuery;
using morph::ui::testing::oldestOpen;
using morph::ui::testing::queryOf;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::TableHarness;
using morph::ui::testing::withTableKey;

namespace table_model_support {

// The revision of the description the harness answered last.

std::string revisionOf(TableHarness const& h) { return h.described.revision; }

// Whether a `Table.samples` call from @p first on asks for the page at @p offset.

bool askedForPage(TableHarness const& h, std::size_t first, std::int64_t offset) {
    for (std::size_t index = first; index < h.dispatcher.calls.size(); ++index) {
        if (h.dispatcher.calls[index].action == "Table.samples" && queryOf(h, index).page.offset == offset) {
            return true;
        }
    }
    return false;
}

// Clicks the button with @p testId, then settles.

void click(TableHarness& h, std::string_view testId) {
    h.backend.click(h.byTestId("Button", testId));
    h.settle();
}

// Whether the call at @p index was stopped.

bool stopped(TableHarness const& h, std::size_t index) {
    return h.dispatcher.calls.at(index).options.stop.stop_requested();
}

// A document with the model alias `sample` and @p view.

std::string doc(std::string_view view, std::string_view mutations) {
    return R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},"mutations":)" +
           std::string{mutations} + R"(,"view":)" + std::string{view} + "}";
}

// `id` (the key), `mass` (two places, half up, in kg, toned by `massTone` and described by `massTip`), `name`,
// `count`, and the companions `massTone`, `massTip` and `lockedBy`, the row's lock.

table::TableDescription companionDescription() {
    table::TableDescription d;
    d.id = "samples";
    d.label = "Samples";
    d.key = "id";
    d.columns = {
        table::TableColumn{.info = {.id = "id", .kind = table::ColumnKind::Integer}, .label = "Id"},
        table::TableColumn{.info = {.id = "mass", .kind = table::ColumnKind::Decimal},
                           .label = "Mass",
                           .display = {.decimals = 2, .rounding = table::Rounding::HalfUp, .unit = "kg"},
                           .tone = "massTone",
                           .tooltip = "massTip"},
        table::TableColumn{.info = {.id = "name", .kind = table::ColumnKind::Text}, .label = "Name"},
        table::TableColumn{.info = {.id = "count", .kind = table::ColumnKind::Integer}, .label = "Count"},
        table::TableColumn{.info = {.id = "massTone", .kind = table::ColumnKind::Text}, .shown = false},
        table::TableColumn{.info = {.id = "massTip", .kind = table::ColumnKind::Text}, .shown = false},
        table::TableColumn{.info = {.id = "lockedBy", .kind = table::ColumnKind::Text}, .shown = false},
    };
    d.rowLock = "lockedBy";
    return table::describe(std::move(d));
}

// The row the companion cases show: mass 0.125, toned "warn", described "Above limit"; `count` holds text, which an
// Integer column cannot read.

CompanionRow companionRow(std::int64_t key) {
    return CompanionRow{{"id", table::Cell{key}},
                        {"mass", table::Cell{table::parseDecimal("0.125").value()}},
                        {"name", table::Cell{std::string{"A"}}},
                        {"count", table::Cell{std::string{"abc"}}},
                        {"massTone", table::Cell{std::string{"warn"}}},
                        {"massTip", table::Cell{std::string{"Above limit"}}},
                        {"lockedBy", table::Cell{std::string{}}}};
}

// The page answering the latest query with @p rows, each holding the columns the query names.

std::string companionPage(TableHarness const& h, std::vector<CompanionRow> const& rows) {
    table::GenericPage page;
    page.revision = companionDescription().revision;
    page.columns = morph::ui::testing::lastQuery(h).columns.value_or(std::vector<std::string>{});
    page.total = static_cast<std::int64_t>(rows.size());
    page.totalKind = table::TotalKind::Exact;
    page.offset = 0;
    for (CompanionRow const& row : rows) {
        page.keys.emplace_back(std::get<std::int64_t>(row.at("id")));
        table::GenericRow cells;
        for (std::string const& column : page.columns) {
            auto const found = row.find(column);
            cells.push_back(found == row.end() ? table::Cell{} : found->second);
        }
        page.rows.push_back(std::move(cells));
    }
    return glz::write_json(page).value_or("");
}

Value rec(ValueRecord fields) { return Value{std::move(fields)}; }

Value qty(std::string_view amount, std::string unit) {
    auto const parsed = table::parseDecimal(amount);
    REQUIRE(parsed.has_value());
    return Value{Quantity{.value = *parsed, .unit = std::move(unit)}};
}

ValueColumn col(std::string id) { return ValueColumn{.id = std::move(id), .kind = std::nullopt, .sortBy = {}}; }

ValueColumn col(std::string id, table::ColumnKind kind) {
    return ValueColumn{.id = std::move(id), .kind = kind, .sortBy = {}};
}

std::shared_ptr<ValueRowsSource> sourceOf(RowsFixture& h, std::vector<ValueColumn> columns,
                                          std::shared_ptr<table::UnitRegistry const> units) {
    return std::make_shared<ValueRowsSource>(
        h.runtime, std::move(columns), [](Value const& row) { return Key{*row.field("id")->asInt()}; },
        std::move(units));
}

std::size_t columnIndex(table::DataSource const& source, std::string_view column) {
    auto const columns = source.columns();
    auto const found = std::ranges::find(columns, column, &table::ColumnInfo::id);
    REQUIRE(found != columns.end());
    return static_cast<std::size_t>(found - columns.begin());
}

table::ColumnKind kindOf(table::DataSource const& source, std::string_view column) {
    return source.columns()[columnIndex(source, column)].kind;
}

// A document's text with a top-level `state` entry @p name initialised to @p init (JSON).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `withState(text, name, init)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::string withState(std::string_view text, std::string_view name, std::string_view init) {
    auto json = morph::ui::parseJson(text);
    REQUIRE(json.has_value());
    auto initJson = morph::ui::parseJson(R"({"init":{"lit":)" + std::string{init} + "}}");
    REQUIRE(initJson.has_value());
    auto& root = std::get<morph::ui::Json::object_t>(json->data);
    if (!root.contains("state")) {
        root.insert_or_assign("state", morph::ui::Json{morph::ui::Json::object_t{}});
    }
    std::get<morph::ui::Json::object_t>(root.at("state").data).insert_or_assign(std::string{name}, *initJson);
    return glz::write_json(*json).value_or("");
}

// A document's text whose table's first column part takes the member @p member, written `"key":value`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `withFirstColumnKey(text, member)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::string withFirstColumnKey(std::string_view text, std::string_view member) {
    auto json = morph::ui::parseJson(text);
    REQUIRE(json.has_value());
    auto field = morph::ui::parseJson("{" + std::string{member} + "}");
    REQUIRE(field.has_value());
    auto& view = std::get<morph::ui::Json::object_t>(std::get<morph::ui::Json::object_t>(json->data).at("view").data);
    auto& first =
        std::get<morph::ui::Json::object_t>(std::get<morph::ui::Json::array_t>(view.at("columns").data).front().data);
    for (auto& [key, value] : std::get<morph::ui::Json::object_t>(field->data)) {
        first.insert_or_assign(key, value);
    }
    return glz::write_json(*json).value_or("");
}

// kNine with a counter `n` and `last`, and @p event running `kCount`.

std::string countingNine(std::string_view event, std::string_view text) {
    return withTableKey(withState(withState(text, "n", "0"), "last", "null"),
                        "\"" + std::string{event} + "\":" + std::string{kCount});
}

// Presses a dialog's button by its test id, then settles.

void press(morph::ui::testing::TableHarness& h, std::string_view testId) {
    h.backend.click(h.byTestId("Button", testId));
    h.settle();
}

// Whether a widget, found by its kind and test id, is enabled.

bool enabled(morph::ui::testing::TableHarness const& h, std::string_view kind, std::string_view testId) {
    return h.backend.prop(h.byTestId(kind, testId), "enabled") != "false";
}

// Whether a widget of a kind with a test id is mounted.

bool mounted(TableHarness const& h, std::string_view kind, std::string_view testId) {
    return h.backend.find(kind, "testId", testId).has_value();
}

// A widget's property, found by its kind and test id.

std::string propOf(TableHarness const& h, std::string_view kind, std::string_view testId, std::string_view name) {
    return h.backend.prop(h.byTestId(kind, testId), name);
}

// Toggles a check box of the filter menu, then settles.

void toggleBox(TableHarness& h, std::string_view testId) {
    h.backend.toggle(h.byTestId("Checkbox", testId));
    h.settle();
}

// Ticks or unticks "(Select all)" as a user who wants every value under the search ticked, or unticked, does: a box
// that already shows the wanted state is toggled twice, since it shows only the values listed.

void selectAll(TableHarness& h, bool ticked) {
    toggleBox(h, "table_filter_all");
    if ((propOf(h, "Checkbox", "table_filter_all", "checked") == "true") != ticked) {
        toggleBox(h, "table_filter_all");
    }
}

// Chooses an option of one of the filter menu's selects by its label, then settles.

void chooseOption(TableHarness& h, std::string_view testId, std::string_view label) {
    h.backend.choose(h.byTestId("Select", testId), Key{std::string{label}});
    h.settle();
}

// Types into one of the filter menu's text inputs, then settles.

void typeInto(TableHarness& h, std::string_view testId, std::string_view text) {
    h.backend.edit(h.byTestId("TextInput", testId), std::string{text});
    h.settle();
}

// Closes the filter menu as a dismissal does.

void dismissMenu(TableHarness& h) {
    h.backend.dismiss(h.byTestId("Dialog", "table_filter"));
    h.settle();
}

// Presses one of the filter menu's buttons, then settles.

void pressButton(TableHarness& h, std::string_view testId) {
    h.backend.click(h.byTestId("Button", testId));
    h.settle();
}

// A select's `options` property for options whose keys are their labels.

std::string optionsOf(std::vector<std::string> const& labels) {
    std::vector<morph::ui::SelectOption> options;
    for (std::string const& label : labels) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `options` is a fixture list built once; sizing it first would not change what the case checks
        options.push_back(morph::ui::SelectOption{.key = Key{label}, .label = label});
    }
    return morph::ui::testing::detail::formatList(options, [](morph::ui::SelectOption const& option) {
        return morph::ui::testing::detail::formatKey(option.key) + ":" +
               morph::ui::testing::detail::formatItem(option.label);
    });
}

// A distinct-values reply against @p revision: a page with no rows whose `distinct` reads @p values.

std::string distinctReply(std::string const& revision, std::string_view values) {
    table::GenericPage page;
    page.revision = revision;
    table::DistinctValues read;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(read, values));
    page.distinct = std::move(read);
    return glz::write_json(page).value_or("");
}

// The `Table.samples` calls that fetch a query's first page of rows: one per query the rows are given.

std::size_t pageCalls(TableHarness const& h) {
    std::size_t out = 0;
    for (std::size_t at = 0; at < h.dispatcher.calls.size(); ++at) {
        if (h.dispatcher.calls[at].action == "Table.samples" && !queryOf(h, at).distinct &&
            queryOf(h, at).page.offset == 0) {
            ++out;
        }
    }
    return out;
}

// The latest `Table.samples` call that asks for values.

std::size_t lastDistinct(TableHarness const& h) {
    for (std::size_t at = h.dispatcher.calls.size(); at-- > 0;) {
        if (h.dispatcher.calls[at].action == "Table.samples" && queryOf(h, at).distinct) {
            return at;
        }
    }
    FAIL("no distinct request");
    return 0;
}

// The model table `t` over @p description, its first page answered.

std::unique_ptr<morph::ui::Screen> mountDescribed(TableHarness& h, table::TableDescription const& description,
                                                  std::string_view text) {
    auto screen = h.mount(text);
    replyDescribe(h, description);
    table::TableQuery const first = lastQuery(h);
    h.reply(oldestOpen(h, "Table.samples"),
            pageJsonAt(description.revision, 0, 0, 0, first.columns.value_or(std::vector<std::string>{})));
    return screen;
}

// `id`, then a column of each kind a condition is offered by: `n` Integer, `b` Bool, `code` Custom, `d` Date, `dec`
// Decimal and `weight` a Measure in g or kg.

table::TableDescription kindsDescription() {
    table::TableDescription d;
    d.id = "samples";
    d.label = "Samples";
    d.key = "id";
    auto const add = [&d](std::string id, table::ColumnKind kind) {
        d.columns.push_back(table::TableColumn{.info = {.id = std::move(id), .kind = kind}});
    };
    add("id", table::ColumnKind::Integer);
    add("n", table::ColumnKind::Integer);
    add("b", table::ColumnKind::Bool);
    add("code", table::ColumnKind::Custom);
    add("d", table::ColumnKind::Date);
    add("dec", table::ColumnKind::Decimal);
    add("weight", table::ColumnKind::Measure);
    d.columns.back().info.dimensions = {"mass"};
    d.units = {
        table::UnitInfo{
            .id = "g", .symbol = "g", .dimension = "mass", .toBase = *table::parseDecimal("0.001"), .decimals = 0},
        table::UnitInfo{
            .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = *table::parseDecimal("1"), .decimals = 3},
        table::UnitInfo{
            .id = "%", .symbol = "%", .dimension = "ratio", .toBase = *table::parseDecimal("0.01"), .decimals = 1}};
    return table::describe(std::move(d));
}

// The days of a month as a distinct reply's values: `YYYY-MM-DD`, each a bucket of one day.

std::string dayValues(std::string_view month, int days) {
    std::string out = R"({"values":[)";
    for (int day = 1; day <= days; ++day) {
        std::string const text = std::format("{}-{:02}", month, day);
        // NOLINTNEXTLINE(performance-inefficient-string-concatenation): builds the case's small document text once; the temporaries cost nothing next to the mount it feeds
        out += std::string{day == 1 ? "" : ","} + R"({"text":")" + text + R"(","operand":")" + text + ".." + text +
               R"(","count":1})";
    }
    return out + "],\"distinct\":" + std::to_string(days) + "}";
}

// id (Integer, key), status (Text), and the lock companion. Ten rows, keys 0..9: "open" for 0..5, "closed" for 6..9.

morph::table::TableDescription selectionDescription() {
    using namespace morph::table;
    TableDescription d;
    d.id = "samples";
    d.key = "id";
    d.columns = {
        TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"},
        TableColumn{.info = {.id = "status", .kind = ColumnKind::Text}, .label = "Status"},
        TableColumn{.info = {.id = "lockedBy", .kind = ColumnKind::Text}, .shown = false},
    };
    d.rowLock = "lockedBy";
    return describe(std::move(d));
}

// A page reply of selectionDescription(), against the revision @p description has; a keyed reply carries `indices`,
// each row's view row from 0. @p extra is more members, written `"name":value,…`.

std::string selectionReply(std::vector<SelRow> const& rows, std::int64_t total, std::string_view viewRevision,
                           std::vector<std::int64_t> const& unmatched, bool keyed, std::string_view extra,
                           morph::table::TableDescription const& description) {
    std::string keys;
    std::string cells;
    std::string indices;
    for (std::size_t at = 0; at < rows.size(); ++at) {
        std::string const comma = at == 0 ? "" : ",";
        keys += comma + std::to_string(rows[at].key);
        cells +=
            comma + "[" + std::to_string(rows[at].key) + ",\"" + rows[at].status + "\",\"" + rows[at].lockedBy + "\"]";
        indices += comma + std::to_string(at);
    }
    std::string out = R"({"revision":")" + description.revision +
                      R"(","columns":["id","status","lockedBy"],"keys":[)" + keys + R"(],"rows":[)" + cells +
                      R"(],"total":)" + std::to_string(total) + R"(,"offset":0,"viewRevision":")" +
                      std::string{viewRevision} + "\"";
    if (keyed) {
        out += R"(,"indices":[)" + indices + "]";
    }
    if (!unmatched.empty()) {
        std::string list;
        for (auto const key : unmatched) {
            list += (list.empty() ? "" : ",") + std::to_string(key);
        }
        out += R"(,"unmatched":[)" + list + "]";
    }
    if (!extra.empty()) {
        out += "," + std::string{extra};
    }
    return out + "}";
}

// A page reply of selectionDescription(): {"revision", "columns":["id","status","lockedBy"], "keys", "rows", "total",
// "offset": 0, "viewRevision", "unmatched"}.

std::string selectionPage(std::vector<SelRow> const& rows, std::int64_t total, std::string_view viewRevision,
                          std::vector<std::int64_t> const& unmatched) {
    return selectionReply(rows, total, viewRevision, unmatched, false);
}

std::vector<SelRow> openRows() { return {{.key = 0}, {.key = 1}, {.key = 2}, {.key = 3}, {.key = 4}, {.key = 5}}; }

// The catalog entry of an action on Lims_SampleModel that declares `updatesTable` on `samples` and returns
// {"rows": ChangedRows}: the held record's keys are at "rows.rows", by the member "key".

morph::ui::ActionInfo samplesAction() {
    morph::ui::ActionInfo info;
    info.modelType = "Lims_SampleModel";
    info.effects = std::vector<morph::ui::ActionEffect>{{.kind = "updates", .record = "Lims_SampleModel.samples"}};
    info.records = {
        morph::ui::ActionRecordInfo{.record = "Lims_SampleModel.samples", .keyField = "key", .paths = {"rows.rows"}}};
    return info;
}

// The document of @p table and @p mutations: the table, and a button `run-<name>` running each mutation.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `selectionDoc(table, mutations, h)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::string selectionDoc(std::string_view table, std::string_view mutations, TableHarness& h) {
    auto const parsed = morph::ui::parseJson(mutations);
    REQUIRE(parsed.has_value());
    std::string children{table};
    for (auto const& [name, mutation] : std::get<morph::ui::Json::object_t>(parsed->data)) {
        auto const& fields = std::get<morph::ui::Json::object_t>(mutation.data);
        h.mutationActions.insert_or_assign(name, std::get<std::string>(fields.at("action").data));
        // NOLINTBEGIN(performance-inefficient-string-concatenation): builds the case's small document text once; the temporaries cost nothing next to the mount it feeds
        children += R"(,{"kind":"button","testId":"run-)" + name + R"(","label":")" + name +
                    R"(","onClick":[{"run":")" + name + R"("}]})";
        // NOLINTEND(performance-inefficient-string-concatenation)
    }
    return doc(R"({"kind":"column","children":[)" + children + "]}", mutations);
}

// Mounts it and answers the describe request and the first page: six open rows, total 6, viewRevision "v1".

std::unique_ptr<morph::ui::Screen> mountSelection(TableHarness& h, std::string_view table,
                                                  std::string_view mutations) {
    addAction(h, "Lims_TagRows", std::string{kSelectionSchema}, samplesAction());
    auto screen = h.mount(selectionDoc(table, mutations, h));
    replyDescribe(h, describeJson(selectionDescription()));
    replyPage(h, selectionPage(openRows(), 6, "v1"));
    return screen;
}

Key key(std::int64_t value) { return Key{value}; }

table::RowId row(std::int64_t value) { return table::RowId{value}; }

// The document of the edit cases: a table `t` of `samples` of the alias `sample`, @p extra added to its node.

std::string editDoc(std::string_view extra) {
    return doc(
        R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",)"
        R"("columns":[{"id":"id"},{"id":"name"},)"
        R"({"id":"count","tone":{"lit":"warn"},"cell":[{"kind":"text","testId":"count_cell","text":{"row":"count"}}]},)"
        R"({"id":"label"},{"id":"remark"}])" +
        std::string{extra} + "}");
}

// `samples` with `count` edited by `Lims_EditSpecimenCount`, and `label` and `remark` by `Lims_EditSpecimenRow`.

table::TableDescription editDescription() {
    table::TableDescription d;
    d.id = "samples";
    d.label = "Samples";
    d.key = "id";
    d.columns = {
        table::TableColumn{.info = {.id = "id", .kind = table::ColumnKind::Integer}, .label = "Id"},
        table::TableColumn{.info = {.id = "name", .kind = table::ColumnKind::Text}, .label = "Name"},
        table::TableColumn{.info = {.id = "count", .kind = table::ColumnKind::Integer},
                           .label = "Count",
                           .edit = table::EditInfo{.action = "Lims_EditSpecimenCount"}},
        table::TableColumn{.info = {.id = "label", .kind = table::ColumnKind::Text},
                           .label = "Label",
                           .edit = table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "label"}},
        table::TableColumn{.info = {.id = "remark", .kind = table::ColumnKind::Text},
                           .label = "Remark",
                           .edit = table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "remarkText"}},
    };
    return table::describe(std::move(d));
}

// A row of the edit cases: name `row-<key>`, count @p count (the key by default), label `L<key>`, remark `R<key>`.

CompanionRow editRow(std::int64_t key, std::int64_t count) {
    return CompanionRow{{"id", table::Cell{key}},
                        {"name", table::Cell{"row-" + std::to_string(key)}},
                        {"count", table::Cell{count < 0 ? key : count}},
                        {"label", table::Cell{"L" + std::to_string(key)}},
                        {"remark", table::Cell{"R" + std::to_string(key)}}};
}

// The reply to the latest query, a page of @p rows from view row 0, or a keyed reply of them at @p indices.

std::string editPage(TableHarness const& h, std::vector<CompanionRow> const& rows, std::vector<std::int64_t> indices) {
    table::GenericPage page;
    page.revision = editDescription().revision;
    page.columns = morph::ui::testing::lastQuery(h).columns.value_or(std::vector<std::string>{});
    page.total = 3;
    page.totalKind = table::TotalKind::Exact;
    page.offset = 0;
    page.indices = std::move(indices);
    for (CompanionRow const& row : rows) {
        page.keys.emplace_back(std::get<std::int64_t>(row.at("id")));
        table::GenericRow cells;
        for (std::string const& column : page.columns) {
            auto const found = row.find(column);
            cells.push_back(found == row.end() ? table::Cell{} : found->second);
        }
        page.rows.push_back(std::move(cells));
    }
    return glz::write_json(page).value_or("");
}

// Adds the two edit actions to the harness's catalog, each declaring `updatesTable` on `samples`, and permits them,
// `Lims_EditSpecimenCount` only when @p count says so.

void addEditActions(TableHarness& h, bool count) {
    morph::ui::testing::addAction(h, "Lims_EditSpecimenCount", std::string{morph::test::grid::kEditSpecimenCount},
                                  samplesAction());
    morph::ui::testing::addAction(h, "Lims_EditSpecimenRow", std::string{morph::test::grid::kEditSpecimenRow},
                                  samplesAction());
    if (count) {
        h.session.setPermitted({"Lims_EditSpecimenCount", "Lims_EditSpecimenRow"});
    } else {
        h.session.setPermitted({"Lims_EditSpecimenRow"});
    }
}

// Mounts @p text, answers the description and a page of rows 1 to 3.

std::unique_ptr<morph::ui::Screen> mountEdits(TableHarness& h, std::string const& text) {
    auto screen = h.mount(text);
    morph::ui::testing::replyDescribe(h, describeJson(editDescription()));
    morph::ui::testing::replyPage(h, editPage(h, {editRow(1), editRow(2), editRow(3)}));
    return screen;
}

// Types @p text into the cell of view row @p viewRow in @p column of the table `t`, then leaves the cell with Tab.

void typeCell(TableHarness& h, std::size_t viewRow, std::string_view column, std::string const& text) {
    int const table = h.byTestId("Table", "t");
    h.backend.clickCell(table, viewRow, morph::ui::testing::columnIndex(h, column));
    h.settle();
    h.backend.typeInto(table, text);
    h.settle();
    h.backend.press(table, "Tab");
    h.settle();
}

}  // namespace table_model_support
