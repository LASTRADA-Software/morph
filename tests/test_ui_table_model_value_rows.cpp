// SPDX-License-Identifier: Apache-2.0

// `ValueRowsSource` and the `table` node over a document's rows.
//
// Part of the client table model tests; see docs/spec/table/table.md.

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

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace table = morph::table;
namespace reactive = morph::reactive;

using namespace table_model_support;

using morph::ui::EngineRows;
using morph::ui::Value;
using morph::ui::testing::describeJson;
using morph::ui::testing::RowsFixture;
using namespace std::chrono_literals;
using morph::ui::Decimal;
using morph::ui::kGroupRunColumn;
using morph::ui::ValueColumn;
using morph::ui::ValueList;
using morph::ui::ValueRecord;
using morph::ui::ValueRowsCollator;
using morph::ui::ValueRowsSource;
using morph::ui::testing::coreOf;
using morph::ui::testing::describedFrom;
using morph::ui::testing::diagnosticsMentioning;
using morph::ui::testing::elementOf;
using morph::ui::testing::engineOf;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::idRows;
using morph::ui::testing::kMixedSamplesView;
using morph::ui::testing::kRowsSamples;
using morph::ui::testing::kSamplesCollection;
using morph::ui::testing::kThree;
using morph::ui::testing::lastQuery;
using morph::ui::testing::mixedRows;
using morph::ui::testing::pagesOf;
using morph::ui::testing::refusalOf;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::rowOrder;
using morph::ui::testing::rowsDoc;
using morph::ui::testing::rowsDocNoId;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using morph::ui::testing::tableOf;

// Mutation: reporting per row, which reports twice ("purple" and "pink").

TEST_CASE("table display: an unknown tone is reported once for its column", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto const screen = h.mount(doc(kTable));
    replyDescribe(h, describeJson(companionDescription()));
    CompanionRow first = companionRow(1);
    CompanionRow second = companionRow(2);
    first.insert_or_assign("massTone", table::Cell{std::string{"purple"}});
    second.insert_or_assign("massTone", table::Cell{std::string{"pink"}});
    replyPage(h, companionPage(h, {first, second}));
    CHECK_FALSE(cellAccent(h, 1, "mass").tone.has_value());
    CHECK_FALSE(cellAccent(h, 2, "mass").tone.has_value());
    // One report for the column, though the two rows name two unknown tones.
    std::vector<std::string> const reports = reportsOf(h);
    REQUIRE(reports.size() == 1);
    CHECK(reports.front().starts_with("column `mass`'s tone is \"p"));
    CHECK(reports.front().ends_with("\"; a tone is neutral, info, ok, warn or err"));
}

// Mutation: a sort over the formatted text, which orders "10.00" before "9.00".

TEST_CASE("table display: text never orders", "[ui][table][display]") {
    using namespace morph::ui::testing;
    auto const texts = [](TableHarness const& h, std::size_t cell) {
        std::vector<std::string> out;
        int const table = h.backend.all("Table").front();
        for (int const row : h.backend.children(table)) {
            out.push_back(h.backend.prop(h.backend.children(row).at(cell), "text"));
        }
        return out;
    };
    {
        TableHarness h;
        auto const screen = h.mount(R"({"vocab":"ui/1","id":"plain",
            "state":{"items":{"init":{"lit":[{"id":1,"v":10},{"id":2,"v":9}]}},
                     "order":{"init":{"lit":[{"column":"v","direction":"asc"}]}}},
            "view":{"kind":"table","id":"t","rows":{"ref":"items"},"sort":{"ref":"order"},
                    "columns":[{"id":"id","label":"Id"},{"id":"v","label":"V","decimals":2,"sortable":true}]}})");
        CHECK(texts(h, 1) == std::vector<std::string>{"9.00", "10.00"});
    }
    // Labels change the text and not the order: 1 shows "zeta" and 3 "alpha", and 1 still comes first.
    TableHarness h;
    auto const screen = h.mount(R"({"vocab":"ui/1","id":"plain",
        "state":{"items":{"init":{"lit":[{"id":1,"code":3},{"id":2,"code":1}]}},
                 "order":{"init":{"lit":[{"column":"code","direction":"asc"}]}}},
        "view":{"kind":"table","id":"t","rows":{"ref":"items"},"sort":{"ref":"order"},
                "columns":[{"id":"id","label":"Id"},{"id":"code","label":"Code","sortable":true,
                            "labels":{"lit":[{"operand":"3","text":"alpha"},{"operand":"1","text":"zeta"}]}}]}})");
    CHECK(texts(h, 1) == std::vector<std::string>{"zeta", "alpha"});
}

namespace {

// `id` (the key) and `v`, a Number column.

table::TableDescription numberDescription() {
    table::TableDescription d;
    d.id = "samples";
    d.label = "Samples";
    d.key = "id";
    d.columns = {
        table::TableColumn{.info = {.id = "id", .kind = table::ColumnKind::Integer}, .label = "Id"},
        table::TableColumn{.info = {.id = "v", .kind = table::ColumnKind::Number}, .label = "V"},
    };
    return table::describe(std::move(d));
}

// The page answering the latest query: row `key` holds `v` for each of @p values, keys from 1.

std::string numberPage(TableHarness const& h, std::vector<double> const& values) {
    table::GenericPage page;
    page.revision = numberDescription().revision;
    page.columns = morph::ui::testing::lastQuery(h).columns.value_or(std::vector<std::string>{});
    page.total = static_cast<std::int64_t>(values.size());
    page.totalKind = table::TotalKind::Exact;
    page.offset = 0;
    std::int64_t key = 1;
    for (double const value : values) {
        page.keys.emplace_back(key);
        table::GenericRow cells;
        for (std::string const& column : page.columns) {
            cells.push_back(column == "id" ? table::Cell{key} : table::Cell{value});
        }
        page.rows.push_back(std::move(cells));
        ++key;
    }
    return glz::write_json(page).value_or("");
}

}  // namespace

// Mutation: a Number column reading only exact decimal text (no `double` fallback), which draws each of these as
// invalid and empty.

TEST_CASE("table display: a Number beyond an exact decimal still shows", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto const screen = h.mount(doc(kTable));
    replyDescribe(h, describeJson(numberDescription()));
    replyPage(h, numberPage(h, {1e21, 1e-20, 9.3e18}));
    CHECK(cellText(h, 1, "v") == "1000000000000000000000");
    CHECK(cellText(h, 2, "v") == "0.00000000000000000001");
    CHECK(cellText(h, 3, "v") == "9300000000000000000");
    for (std::int64_t const key : {1, 2, 3}) {
        CHECK_FALSE(cellAccent(h, key, "v").invalid);
    }
}

// Mutation: the decimals of a Number beyond an exact decimal ignored, which shows "0.00000000000000000001".

TEST_CASE("table display: a Number beyond an exact decimal follows the column's places", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto const screen = h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
                                  "columns":[{"id":"id"},{"id":"v","decimals":2,"grouping":true}]})"));
    replyDescribe(h, describeJson(numberDescription()));
    replyPage(h, numberPage(h, {1e21, 1e-20}));
    CHECK(cellText(h, 1, "v") == "1,000,000,000,000,000,000,000.00");
    CHECK(cellText(h, 2, "v") == "0.00");
}

// Mutation: a rows table's column built without its `unit`, which gives the header none.

TEST_CASE("table display: a rows table's header shows its column's unit", "[ui][table][display]") {
    TableHarness h;
    auto const screen = h.mount(R"({"vocab":"ui/1","id":"plain",
        "state":{"items":{"init":{"lit":[{"id":1,"m":3}]}}},
        "view":{"kind":"table","id":"t","testId":"t","rows":{"ref":"items"},
                "columns":[{"id":"id","label":"Id"},{"id":"m","label":"Mass","unit":"kg","decimals":2}]}})");
    int const table = h.backend.all("Table").front();
    std::vector<morph::ui::TableColumn> const columns = h.backend.tableColumns(table);
    REQUIRE(columns.size() == 2);
    CHECK(columns.at(0).unit.empty());
    CHECK(columns.at(1).unit == "kg");
    CHECK(h.backend.prop(table, "columns").contains(";unit=kg"));
}

// Mutation: the column part's display checks left out of checkColumnPart, which loads each of these.

TEST_CASE("ui document: a column's display parts are checked when the document loads", "[ui][document]") {
    auto const column = [](std::string_view part) {
        return R"({"vocab":"ui/1","id":"plain","state":{"items":{"init":{"lit":[]}}},)"
               R"("view":{"kind":"table","id":"t","rows":{"ref":"items"},"columns":[{"id":"v",)" +
               std::string{part} + "}]}}";
    };
    REQUIRE(refusalOf(column(R"("decimals":2,"rounding":"halfUp","date":"dd-MMM-yyyy",)"
                             R"("labels":{"lit":[{"operand":"1","text":"one"}]})"))
                .empty());
    CHECK(refusalOf(column(R"("decimals":19)")) == "`decimals` is a literal integer, 0 to 18");
    CHECK(refusalOf(column(R"("rounding":"up")")) ==
          "`up` is not one of the values of `rounding`: halfEven or halfUp");
    CHECK(refusalOf(column(R"("labels":{"lit":[{"operand":1,"text":"one"}]})")) ==
          R"(`labels` is a literal list of `{"operand": text, "text": text}`)");
    CHECK_FALSE(refusalOf(column(R"("date":"qq")")).empty());
}

// Mutation: the unit appended whatever the text, which shows "three kg" for the labelled value.

TEST_CASE("table display: a rows table's quantity keeps its unit unless a label names it", "[ui][table][display]") {
    TableHarness h;
    auto const screen = h.mount(R"({"vocab":"ui/1","id":"plain",
        "state":{"items":{"init":{"lit":[{"id":1,"m":{"num":3,"den":1,"dp":0,"unit":"kg"}},
                                         {"id":2,"m":{"num":5,"den":2,"dp":1,"unit":"kg"}}]}}},
        "view":{"kind":"table","id":"t","rows":{"ref":"items"},
                "columns":[{"id":"id","label":"Id"},{"id":"m","label":"Mass","decimals":2,
                            "labels":{"lit":[{"operand":"3","text":"three"}]}}]}})");
    int const table = h.backend.all("Table").front();
    std::vector<std::string> texts;
    for (int const row : h.backend.children(table)) {
        texts.push_back(h.backend.prop(h.backend.children(row).at(1), "text"));
    }
    CHECK(texts == std::vector<std::string>{"three", "2.50 kg"});
}

namespace {

Value ts(std::int64_t ms) {
    return Value{
        morph::time::DateTime{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{ms}}}};
}

Value dec(std::string_view text) {
    auto const parsed = table::parseDecimal(text);
    REQUIRE(parsed.has_value());
    return Value{Decimal{*parsed}};
}

// kg and g of mass.

std::shared_ptr<table::UnitRegistry const> testUnits() {
    table::UnitRegistryBuilder builder;
    REQUIRE(builder.add(table::UnitInfo{.id = "kg",
                                        .symbol = "kg",
                                        .dimension = "mass",
                                        .toBase = morph::math::Rational{1, morph::math::DecimalPlaces{0}},
                                        .decimals = 3}));
    REQUIRE(builder.add(table::UnitInfo{
        .id = "g", .symbol = "g", .dimension = "mass", .toBase = *table::parseDecimal("0.001"), .decimals = 0}));
    return std::move(builder).build();
}

// Every change a source notifies, kept.

struct RecordingListener final : table::ChangeListener {
    explicit RecordingListener(table::DataSource& watched) : source{&watched} { watched.subscribe(*this); }
    RecordingListener(RecordingListener const&) = delete;
    RecordingListener& operator=(RecordingListener const&) = delete;
    RecordingListener(RecordingListener&&) = delete;
    RecordingListener& operator=(RecordingListener&&) = delete;
    ~RecordingListener() override { source->unsubscribe(*this); }
    void rowsChanged(table::RowChange const& change) override { changes.push_back(change); }
    void clear() { changes.clear(); }

    table::DataSource* source;
    std::vector<table::RowChange> changes;
};

// The list written to JSON and read back: no storage shared with @p list.

ValueList freshDecodeOf(ValueList const& list) {
    auto const json = morph::ui::parseJson(morph::ui::detail::canonicalJson(Value{list}));
    REQUIRE(json.has_value());
    auto const value = morph::ui::valueFromJson(*json);
    REQUIRE(value.has_value());
    REQUIRE(value->asList() != nullptr);
    return *value->asList();
}

// A copy of @p list with one row's field replaced; the other rows share storage with @p list's.

ValueList withField(ValueList list, std::size_t row, std::string const& name, Value value) {
    ValueRecord fields = *list.at(row).asRecord();
    fields.insert_or_assign(name, std::move(value));
    list.at(row) = Value{std::move(fields)};
    return list;
}

table::Cell cellAt(table::DataSource const& source, std::size_t row, std::string_view column) {
    return source.snapshot()->cell(row, columnIndex(source, column));
}

std::unique_ptr<table::Engine> engineOver(RowsFixture& h, std::shared_ptr<ValueRowsSource> const& source,
                                          std::shared_ptr<table::TextCollator const> collator = {}) {
    return std::make_unique<table::Engine>(
        source, table::EngineOptions{.owner = &h.owner, .services = {.collator = std::move(collator)}});
}

std::vector<std::int64_t> keysInView(table::Engine const& engine) {
    std::vector<std::int64_t> keys;
    for (std::size_t at = 0; at < engine.viewRowCount(); ++at) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `keys` is a fixture list built once; sizing it first would not change what the case checks
        keys.push_back(std::get<std::int64_t>(engine.rowIdAt(at)));
    }
    return keys;
}

// `n` rows `{id: i, name: "n<i>", notes: "x", amount: i}`.

ValueList sampleRows(std::size_t n) {
    ValueList rows;
    rows.reserve(n);
    for (std::size_t at = 0; at < n; ++at) {
        auto const i = static_cast<std::int64_t>(at);
        rows.push_back(rec({{"id", i}, {"name", "n" + std::to_string(at)}, {"notes", "x"}, {"amount", i}}));
    }
    return rows;
}

}  // namespace

// Mutation: a diff by row value (the fresh decode notifies, or `cellsDerived` counts every field).

TEST_CASE("value rows source: a document's rows as a source", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("name"), col("notes"), col("amount")});
    RecordingListener listener{*src};
    ValueList const rows = sampleRows(100);
    src->use("name");
    src->assign(rows);
    h.settle();
    listener.clear();
    std::size_t const derived = src->cellsDerived();
    src->assign(freshDecodeOf(rows));
    h.settle();
    CHECK(listener.changes.empty());              // the same cells: nothing notified
    CHECK(src->cellsDerived() - derived == 100);  // one column in use, not three fields
    src->assign(withField(freshDecodeOf(rows), 7, "name", Value{std::string{"zeta"}}));
    h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{7});
    CHECK(src->cellsHeld() == 100);  // "notes" and "amount" hold no cells
    src->use("amount");
    h.settle();
    CHECK(src->cellsHeld() == 200);
    src->unuse("amount");
    CHECK(src->cellsHeld() == 100);  // its last use ended: its cells go
}

// Mutations: `sortBy` read outside a tracked computation (no change is notified); a per-row change notified as Reset.

TEST_CASE("value rows source: a change to what one row's sortBy reads updates that row", "[ui][table]") {
    RowsFixture h;
    std::vector<std::unique_ptr<reactive::Signal<std::int64_t>>> rank;
    for (int i = 0; i < 10; ++i) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rank` is a fixture list built once; sizing it first would not change what the case checks
        rank.push_back(std::make_unique<reactive::Signal<std::int64_t>>(h.runtime, i));
    }
    reactive::Signal<std::int64_t> scale{h.runtime, 1};
    ValueColumn ranked = col("name");
    ranked.sortBy = [&](Value const& row) {
        return Value{rank.at(static_cast<std::size_t>(*row.field("id")->asInt()))->get() * scale.get()};
    };
    auto const src = sourceOf(h, {ranked});
    RecordingListener listener{*src};
    src->use("name");
    src->assign(sampleRows(10));
    h.settle();
    listener.clear();
    rank.at(3)->set(99);
    h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{3});
    CHECK(cellAt(*src, 3, "name") == table::Cell{std::int64_t{99}});
    listener.clear();
    scale.set(2);
    h.settle();  // what every row's cell reads
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Reset);
}

// Mutation: keep the last duplicate (the view holds "c" and not "a").

TEST_CASE("value rows source: a duplicate key keeps its first row", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("name")});
    std::size_t const reports = h.reported(morph::ui::detail::site::kDuplicateKey);
    src->use("name");
    src->assign(
        ValueList{rec({{"id", 1}, {"name", "a"}}), rec({{"id", 2}, {"name", "b"}}), rec({{"id", 1}, {"name", "c"}})});
    h.settle();
    CHECK(src->snapshot()->rowCount() == 2);
    CHECK(h.reported(morph::ui::detail::site::kDuplicateKey) - reports == 1);  // once per duplicate
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = "name", .dir = table::SortDirection::Descending}}));
    h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 1});  // "b", then the first "a"; never "c"
    CHECK(cellAt(*src, 0, "name") == table::Cell{std::string{"a"}});
}

// Mutation: Timestamp cells kept in milliseconds (row 2 reads 1'700'000'000'500 and sorts apart from row 1).

TEST_CASE("value rows source: a timestamp is a DateTime of whole seconds", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("at")});
    src->use("at");
    src->assign(ValueList{rec({{"id", 1}, {"at", ts(1'700'000'000'000)}}),
                          rec({{"id", 2}, {"at", ts(1'700'000'000'500)}}), rec({{"id", 3}, {"at", ts(-1)}})});
    h.settle();
    CHECK(kindOf(*src, "at") == table::ColumnKind::DateTime);
    CHECK(cellAt(*src, 0, "at") == table::Cell{std::int64_t{1'700'000'000}});
    CHECK(cellAt(*src, 1, "at") == table::Cell{std::int64_t{1'700'000'000}});  // the same second: they tie
    CHECK(cellAt(*src, 2, "at") == table::Cell{std::int64_t{-1}});             // floor, not truncation toward 0
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = "at", .dir = table::SortDirection::Descending}}));
    h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{1, 2, 3});  // the tie keeps source order
}

// Mutations: a missing case in the inference (a kind differs); an empty column given no kind (the "e" entry);
// a mixed Int and Decimal column inferred from its first cell (Integer, with one invalid cell).

TEST_CASE("value rows source: every value kind infers its column kind", "[ui][table]") {
    using enum table::ColumnKind;
    RowsFixture h;
    auto const src = sourceOf(h,
                              {col("b"), col("i"), col("d"), col("s"), col("t"), col("q"), col("l"), col("id_d"),
                               col("e"), col("n", Number)},
                              testUnits());  // testUnits(): kg and g of mass
    for (auto const* id : {"b", "i", "d", "s", "t", "q", "l", "id_d", "e", "n"}) {
        src->use(id);
    }
    src->assign(ValueList{rec({{"id", 1},
                               {"b", true},
                               {"i", 4},
                               {"d", dec("2.50")},
                               {"s", "x"},
                               {"t", ts(1'000)},
                               {"q", qty("3", "kg")},
                               {"l", 7},
                               {"id_d", 4},
                               {"e", Value{}},
                               {"n", dec("0.5")}}),
                          rec({{"id", 2},
                               {"b", false},
                               {"i", 5},
                               {"d", dec("0.125")},
                               {"s", "y"},
                               {"t", ts(2'000)},
                               {"q", qty("3", "zz")},
                               {"l", ValueList{}},
                               {"id_d", dec("0.5")},
                               {"e", Value{}},
                               {"n", 2}})});
    h.settle();
    std::map<std::string, table::ColumnKind> kinds;
    for (table::ColumnInfo const& column : src->columns()) {
        kinds.emplace(column.id, column.kind);
    }
    CHECK(kinds == std::map<std::string, table::ColumnKind>{{"b", Bool},
                                                            {"i", Integer},
                                                            {"d", Decimal},
                                                            {"s", Text},
                                                            {"t", DateTime},
                                                            {"q", Measure},
                                                            {"l", Integer},
                                                            {"id_d", Decimal},
                                                            {"e", Text},
                                                            {"n", Number}});
    CHECK(src->invalidCells("q") == 1);     // the second row's unit "zz" is unknown
    CHECK(src->invalidCells("l") == 1);     // the list cell
    CHECK(src->invalidCells("id_d") == 0);  // an integer is an exact decimal
    CHECK(cellAt(*src, 0, "n") == table::Cell{0.5});
    CHECK(cellAt(*src, 1, "n") == table::Cell{2.0});
    RecordingListener listener{*src};
    src->assign(ValueList{rec({{"id", 1}, {"e", 9}})});
    h.settle();  // the empty column's first vote
    CHECK(kindOf(*src, "e") == Integer);
    CHECK(listener.changes.back().kind == table::ChangeKind::Reset);
}

// Mutations: every row given run 0, as an index computed after the sort would leave it (groups split: 2, 3, 1);
// the run column named plain "group" (the source has two columns of that id, and the count CHECK reads 2).

TEST_CASE("value rows source: a grouped table sorts within its runs", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("name"), col("group")});  // a document column named "group"
    src->setGroup([](Value const& row) { return *row.field("site"); }, {});
    src->use("name");
    src->use("group");
    src->assign(ValueList{rec({{"id", 1}, {"site", "A"}, {"name", "z"}, {"group", "q"}}),
                          rec({{"id", 2}, {"site", "A"}, {"name", "a"}, {"group", "r"}}),
                          rec({{"id", 3}, {"site", "B"}, {"name", "m"}, {"group", "p"}})});
    h.settle();
    CHECK(std::ranges::count(src->columns(), std::string{"group"}, &table::ColumnInfo::id) == 1);
    CHECK(std::ranges::count(src->columns(), std::string{kGroupRunColumn}, &table::ColumnInfo::id) == 1);
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = std::string{kGroupRunColumn}}, {.column = "name"}}));
    h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 1, 3});
    REQUIRE(engine->setSort({{.column = std::string{kGroupRunColumn}}, {.column = "group"}}));
    h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{1, 2, 3});  // by the document's "group", in runs
    CHECK(src->runRecords().size() == 2);
    CHECK(src->runOf(2) == 1);
}

// Mutation: fold inside the comparison (about 3.4 million folds).  Gate U3.

TEST_CASE("value rows source: a sort of 100,000 rows folds each text once", "[ui][table]") {
    RowsFixture h;
    auto const collator = std::make_shared<ValueRowsCollator>();
    auto const src = sourceOf(h, {col("name")});
    src->use("name");
    src->assign(sampleRows(100'000));
    h.settle();
    auto engine = engineOver(h, src, collator);
    REQUIRE(engine->setSort({{.column = "name"}}));
    h.settle();
    CHECK(collator->folds() == 100'000);
    CHECK(engine->stats().fullSorts == 1);
}

// Mutations: a fresh decode deriving every column (cellsDerived is 300,000); the changed row sorting the list
// (fullSorts grows).  Gates U2 and U4, as counts.

TEST_CASE("value rows source: one changed row derives one column and repairs", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("name"), col("notes"), col("amount")});
    ValueList const rows = sampleRows(10'000);
    src->use("name");
    src->assign(rows);
    h.settle();
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = "name"}}));
    h.settle();
    CHECK(src->cellsHeld() == 10'000);  // U4: one column's cells
    table::EngineStats const before = engine->stats();
    std::size_t const derived = src->cellsDerived();
    src->assign(withField(freshDecodeOf(rows), 42, "name", Value{std::string{"zzz"}}));
    h.settle();
    CHECK(src->cellsDerived() - derived == 10'000);  // U2: the column in use only
    CHECK(engine->stats().repairs - before.repairs == 1);
    CHECK(engine->stats().fullSorts == before.fullSorts);
    CHECK(keysInView(*engine).back() == 42);
}

namespace {

// The counts a mounted grouped table's headings show, in order; each heading's text is its run's `count`.

std::vector<std::string> headingCounts(morph::ui::testing::Harness const& h) {
    std::vector<std::string> counts;
    for (int const row : h.backend.children(h.backend.all("Table").front())) {
        if (h.backend.prop(row, "heading") != "true") {
            continue;
        }
        int cell = row;
        while (h.backend.kindOf(cell) != "Text" && !h.backend.children(cell).empty()) {
            cell = h.backend.children(cell).front();
        }
        counts.push_back(h.backend.prop(cell, "text"));
    }
    return counts;
}

}  // namespace

// Mutation: runs broken where `Value ==` differs (three runs of one: 1 and 1.0 and 1.00 split; the counts read 1, 1,
// 1, 1).

TEST_CASE("value rows source: a group's runs are the interpreter's runs", "[ui][table]") {
    // `g` is 1, then 1.0, then 1.00, then 2: one value spelled three ways, then another.
    std::string const rows = R"([ { "id": 1, "g": 1 }, { "id": 2, "g": { "num": 1, "den": 1, "dp": 1 } },
        { "id": 3, "g": { "num": 1, "den": 1, "dp": 2 } }, { "id": 4, "g": 2 } ])";
    morph::ui::testing::Harness interpreter;
    auto const screen = interpreter.mount(R"({ "vocab": "ui/1", "id": "runs",
      "state": { "items": { "init": { "lit": )" +
                                          rows + R"( } } },
      "view": { "kind": "table", "rows": { "ref": "items" },
                "group": { "by": { "row": "g" }, "heading": { "kind": "text", "text": { "fn": "format",
                           "args": [ "{0}", { "row": "count" } ] } } },
                "columns": [ { "id": "id", "label": "Id" } ] } })");
    REQUIRE(headingCounts(interpreter) == std::vector<std::string>{"3", "1"});

    RowsFixture h;
    auto const src = sourceOf(h, {col("id")});
    src->setGroup([](Value const& row) { return *row.field("g"); }, {});
    auto const json = morph::ui::parseJson(rows);
    REQUIRE(json.has_value());
    auto const list = morph::ui::valueFromJson(*json);
    REQUIRE(list.has_value());
    src->assign(*list->asList());
    h.settle();
    std::vector<std::string> counts;
    for (Value const& record : src->runRecords()) {
        counts.push_back(std::to_string(*record.field("count")->asInt()));
    }
    CHECK(counts == headingCounts(interpreter));
    CHECK(*src->runRecords().front().field("key") == Value{1});  // the run's first row's `by`
    CHECK(src->runOf(2) == 0);
    CHECK(src->runOf(3) == 1);
}

// Mutation: the row value not compared when its cells are equal (no change is notified, and the record still reads
// "x").

TEST_CASE("value rows source: a field no column reads is redrawn in place", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("name"), col("notes")});
    src->use("name");
    src->assign(sampleRows(10));
    h.settle();
    EngineRows rows{src, morph::ui::ValueRowsSource::recordOf(), table::EngineOptions{.owner = &h.owner}};
    REQUIRE(rows.setSort({{.column = "name"}}));
    h.settle();
    RecordingListener listener{*src};
    auto const viewOf7 = [&rows] {
        for (std::int64_t at = 0; at < rows.total().rows; ++at) {
            if (rows.row(at)->key == table::RowId{std::int64_t{7}}) {
                return at;
            }
        }
        return std::int64_t{-1};
    };
    std::int64_t const before = viewOf7();
    CHECK(*rows.row(before)->row.field("notes")->asString() == "x");
    std::size_t const sorts = rows.engine().stats().fullSorts;
    src->assign(withField(freshDecodeOf(sampleRows(10)), 7, "notes", Value{std::string{"y"}}));
    h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{7});
    CHECK(viewOf7() == before);                                       // in place
    CHECK(*rows.row(before)->row.field("notes")->asString() == "y");  // read again
    CHECK(rows.engine().stats().fullSorts == sorts);
}

// Mutation: a changed heading record not notified (the change names row 1 alone).

TEST_CASE("value rows source: a changed run heading is an update of the run's first row", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("name")});
    src->setGroup([](Value const& row) { return *row.field("site"); }, {});
    src->use("name");
    ValueList const rows{rec({{"id", 1}, {"site", "A"}, {"name", "a"}, {"notes", "x"}}),
                         rec({{"id", 2}, {"site", "A"}, {"name", "b"}, {"notes", "x"}}),
                         rec({{"id", 3}, {"site", "B"}, {"name", "c"}, {"notes", "x"}})};
    src->assign(rows);
    h.settle();
    RecordingListener listener{*src};
    src->assign(withField(rows, 1, "notes", Value{std::string{"y"}}));
    h.settle();  // run A's `last` changes
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{0, 1});  // row 1, and run A's heading at row 0
    CHECK(*src->runRecords().front().field("last")->field("notes") == Value{std::string{"y"}});
}

// Mutations: the first cell's kind taken as it is (the number-first column reads Integer, with 2 invalid cells);
// the clause applied whatever the first cell's kind (the text-first column reads Decimal, with 1 invalid cell).

TEST_CASE("value rows source: a text among numbers is one invalid cell", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("nf"), col("tf")});
    src->use("nf");
    src->use("tf");
    src->assign(ValueList{rec({{"id", 1}, {"nf", 4}, {"tf", "x"}}), rec({{"id", 2}, {"nf", dec("0.5")}, {"tf", 4}}),
                          rec({{"id", 3}, {"nf", "x"}, {"tf", dec("0.5")}})});
    h.settle();
    CHECK(kindOf(*src, "nf") == table::ColumnKind::Decimal);  // integers with decimals: one Decimal
    CHECK(src->invalidCells("nf") == 1);                      // and the text is the one invalid cell
    CHECK(cellAt(*src, 0, "nf") == table::Cell{morph::math::Rational{4, morph::math::DecimalPlaces{0}}});
    CHECK(kindOf(*src, "tf") == table::ColumnKind::Text);  // text first: a Text column
    CHECK(src->invalidCells("tf") == 2);                   // and both numbers are invalid
}

// Mutations: an insertion notified as Reset (the Inserted CHECK); a removal notified as Reset (the Removed
// CHECK).

TEST_CASE("value rows source: a new key is inserted and a missing key removed", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("name")});
    src->use("name");
    ValueList rows = sampleRows(3);
    src->assign(rows);
    h.settle();
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = "name", .dir = table::SortDirection::Descending}}));
    h.settle();
    RecordingListener listener{*src};
    rows.push_back(rec({{"id", 3}, {"name", "a"}}));
    src->assign(rows);
    h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Inserted);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{3});
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 1, 0, 3});
    listener.clear();
    src->assign(ValueList{rows.at(0), rows.at(2), rows.at(3)});
    h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Removed);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{1});  // its index in the old rows
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 0, 3});
    listener.clear();
    src->assign(ValueList{rows.at(2), rows.at(0)});
    h.settle();  // one key gone and the rest reordered
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Reset);
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 0});
}

// Mutations: days truncated toward 0 (an instant 1 ms before the epoch reads day 0); a {num, den, unit} record not
// read as a measure (the column reads Text).

TEST_CASE("value rows source: a literal date and a measure record", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("d", table::ColumnKind::Date), col("m")}, testUnits());
    src->use("d");
    src->use("m");
    src->assign(ValueList{
        rec({{"id", 1}, {"d", ts(86'400'000 + 1)}, {"m", rec({{"num", 3}, {"den", 2}, {"unit", "kg"}})}}),
        rec({{"id", 2}, {"d", ts(-1)}, {"m", rec({{"num", 1}, {"den", 1}, {"unit", "zz"}})}}),
        rec({{"id", 3}, {"d", "1970-01-03"}, {"m", Value{}}}), rec({{"id", 4}, {"d", "not a date"}, {"m", Value{}}})});
    h.settle();
    CHECK(cellAt(*src, 0, "d") == table::Cell{std::int64_t{1}});
    CHECK(cellAt(*src, 1, "d") == table::Cell{std::int64_t{-1}});  // the day before the epoch
    CHECK(cellAt(*src, 2, "d") == table::Cell{std::int64_t{2}});   // ISO text
    CHECK(src->invalidCells("d") == 1);                            // text that is no date
    CHECK(kindOf(*src, "m") == table::ColumnKind::Measure);
    auto const kg = testUnits()->find("kg");
    REQUIRE(kg.has_value());
    CHECK(cellAt(*src, 0, "m") ==
          table::Cell{
              table::Measure{.amount = morph::math::Rational{morph::math::Numerator{3}, morph::math::Denominator{2},
                                                             morph::math::DecimalPlaces{1}},
                             .unit = *kg}});
    CHECK(src->invalidCells("m") == 1);  // the unit "zz" is unknown
}

// Mutations: the interpreter's pipeline left in place for `rows` tables (the accented names order apart); the
// collection keeping the reply's order (its rows table not lowered by the interpreter).

TEST_CASE("one order on every surface: a rows table, a model table and a collection order alike", "[ui][table]") {
    std::string const rows = mixedRows();
    for (std::vector<std::size_t> const& clicks : {std::vector<std::size_t>{1},     // by name
                                                   std::vector<std::size_t>{1, 1},  // by name, descending
                                                   std::vector<std::size_t>{2}}) {  // by weight
        TableHarness asRows;
        auto const rowsScreen = asRows.mount(rowsDoc(rows));
        TableHarness asModel;
        auto const modelScreen = asModel.mount(kRowsSamples);
        TableHarness asCollection;
        asCollection.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
        auto const collectionScreen = asCollection.mount(kSamplesCollection);
        asCollection.reply(asCollection.dispatcher.last("Lims_ListSamples"), R"({"rows":)" + rows + "}");
        int const collection = asCollection.byTestId("Table", "collection_rows");
        replyDescribe(asModel, describedFrom(rows));
        replyPage(asModel, pagesOf(rows, lastQuery(asModel)));
        for (std::size_t const column : clicks) {
            asRows.backend.clickHeader(tableOf(asRows), column);
            asRows.settle();
            asModel.backend.clickHeader(tableOf(asModel), column);
            asModel.settle();
            asCollection.backend.clickHeader(collection, column);
            asCollection.settle();
            replyPage(asModel, pagesOf(rows, lastQuery(asModel)));
        }
        asModel.backend.scrollTable(tableOf(asModel), 0, 200);
        asModel.settle();  // a model table is windowed
        CHECK(rowOrder(asRows) == rowOrder(asModel));
        CHECK(rowOrder(asCollection) == rowOrder(asRows));
        CHECK_FALSE(rowOrder(asRows).empty());
    }
}

// Mutation: sorting the whole list on a row change (fullSorts grows).  Gate U2 counts, on a screen.

TEST_CASE("rows table: one changed row is one repair", "[ui][table]") {
    TableHarness h;
    auto screen = h.mount(rowsDoc(kThree));
    h.backend.clickHeader(tableOf(h), 1);
    h.settle();
    REQUIRE(rowOrder(h) == "a b c");
    table::EngineStats const before = engineOf(*screen, "list").stats();
    h.backend.click(h.byTestId("Button", "rename"));
    h.settle();
    CHECK(engineOf(*screen, "list").stats().repairs - before.repairs == 1);
    CHECK(engineOf(*screen, "list").stats().fullSorts == before.fullSorts);
    CHECK(rowOrder(h) == "a c b");  // "zz" moved last
}

// Mutations: no bound check (no warning); the bound read as `>=` (a warning at 20,000); a warning per change of the
// list (two, for the list that grows to 20,002 rows: the screen drops only a repeat of the same words).

TEST_CASE("rows table: a list over 20,000 rows is reported once", "[ui][table]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(idRows(20'001)));
    std::string const warning =
        "table list holds 20,001 rows; a table over rows is ordered on the client up to "
        "20,000 rows or 1,000,000 cells — serve it as a model's table";
    CHECK(diagnosticsMentioning(h, warning) == 1);
    h.backend.click(h.byTestId("Button", "rename"));
    h.settle();  // the list changes: no second report
    CHECK(diagnosticsMentioning(h, warning) == 1);
    // A list of another size would word its report apart: still the one report.
    std::string const growing = [] {
        std::string text = rowsDoc(idRows(20'001));
        std::string const renamed = morph::ui::testing::renamedB(idRows(20'001));
        text.replace(text.find(renamed), renamed.size(), idRows(20'002));
        return text;
    }();
    TableHarness grows;
    auto const grown = grows.mount(growing);
    grows.backend.click(grows.byTestId("Button", "rename"));
    grows.settle();
    CHECK(diagnosticsMentioning(grows, "serve it as a model's table") == 1);
    TableHarness under;
    auto const small = under.mount(rowsDoc(idRows(20'000)));
    CHECK(diagnosticsMentioning(under, "serve it as a model's table") == 0);  // at the bound: nothing
}

// Mutation: drawing the rows before the load settles (the first CHECK reads "a b c").

TEST_CASE("rows table: persisted state before the first rows", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(
        storeExecutor(), R"({"version":1,"sort":[{"column":"name","direction":"desc"}]})");
    TableHarness h{store};
    auto const screen = h.mount(rowsDoc(kThree));
    CHECK(rowOrder(h).empty());  // placeholders only while restoring
    CHECK(h.backend.children(tableOf(h)).size() == 3);
    CHECK(store->lastLoadKey() ==
          table::TableStateKey{
              .application = "lims", .model = "", .table = "list", .qualifier = "samples", .part = "state"});
    store->release();
    h.settle();
    CHECK(rowOrder(h) == "c b a");  // first drawn: the stored order
    h.backend.clickHeader(tableOf(h), 1);
    h.advance(500ms);
    CHECK(store->saves() == 1);
}

// Mutation: a rows table without `id` given a key (a load goes out with table "").

TEST_CASE("rows table: a table without an id persists nothing", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto const screen = h.mount(rowsDocNoId(kThree));
    CHECK(rowOrder(h) == "a b c");  // no restoring state
    h.backend.clickHeader(h.backend.all("Table").front(), 1);
    h.advance(500ms);
    CHECK(store->loads() == 0);
    CHECK(store->saves() == 0);
}

// Mutations: `shown` always true for a rows table; a message under `status: false` kept in the line.

TEST_CASE("rows table: the status line takes space only while it has something to say", "[ui][table]") {
    TableHarness h;
    auto screen = h.mount(rowsDoc(kThree));
    CHECK_FALSE(statusOf(h).shown);
    coreOf(*screen).tableMessage(elementOf(*screen, "list"), "Refused");
    h.settle();
    CHECK(statusOf(h).shown);
    CHECK(statusOf(h).message == "Refused");
    CHECK(statusOf(h).counts == "3 rows");
    h.advance(6'000ms);
    CHECK_FALSE(statusOf(h).shown);
    TableHarness always;
    auto const shown = always.mount(rowsDoc(kThree, R"(,"status":true)"));
    CHECK(statusOf(always).shown);
    CHECK(statusOf(always).counts == "3 rows");
    TableHarness never;
    auto hidden = never.mount(rowsDoc(kThree, R"(,"status":false)"));
    coreOf(*hidden).tableMessage(elementOf(*hidden, "list"), "Refused");
    never.settle();
    CHECK_FALSE(statusOf(never).shown);
    CHECK(statusOf(never).message.empty());
}
