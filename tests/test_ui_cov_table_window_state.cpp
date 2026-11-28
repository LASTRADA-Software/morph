// SPDX-License-Identifier: Apache-2.0

// `WindowRows`' sort, filter, values lists and retained rows: what each refuses, what it leaves alone, and what it
// asks of the server.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <morph/table/filter.hpp>
#include <morph/table/sort.hpp>
#include <morph/ui/table_rows.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
namespace ui = morph::ui;
using morph::ui::testing::pageJson;
using morph::ui::testing::RowsFixture;
using morph::ui::testing::samplesDescription;

table::RowId id(std::int64_t key) { return table::RowId{key}; }

table::FilterSpec filterOn(std::string column, std::string equals) {
    table::FilterEntry entry;
    entry.eq = std::move(equals);
    table::FilterSpec spec;
    spec.columns[std::move(column)] =
        table::ColumnFilter{.combine = table::Combine::Any, .include = {entry}, .exclude = {}};
    return spec;
}

using Distinct = std::expected<table::DistinctValues, table::TableError>;
}  // namespace

TEST_CASE("cov window state: a sort is checked against the description and a repeat asks nothing",
          "[ui][table][cov]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, 50);
    std::size_t const sent = h.dispatcher.count("Table.samples");
    CHECK(rows->setSort({{.column = "name", .dir = table::SortDirection::Ascending}}).has_value());
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") > sent);
    std::size_t const sorted = h.dispatcher.count("Table.samples");
    CHECK(rows->setSort({{.column = "name", .dir = table::SortDirection::Ascending}}).has_value());  // unchanged
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == sorted);
    auto const unknown = rows->setSort({{.column = "nope", .dir = table::SortDirection::Ascending}});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().message.contains("nope"));
    auto const twice = rows->setSort({{.column = "name", .dir = table::SortDirection::Ascending},
                                      {.column = "name", .dir = table::SortDirection::Descending}});
    CHECK_FALSE(twice.has_value());
    table::SortChain tooLong;
    for (int at = 0; at < 9; ++at) {
        tooLong.push_back({.column = at % 2 == 0 ? "id" : "name", .dir = table::SortDirection::Ascending});
    }
    CHECK_FALSE(rows->setSort(tooLong).has_value());
}

TEST_CASE("cov window state: a filter is compiled before it is kept, and a new one drops what was retained",
          "[ui][table][cov]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, 50);
    auto const refused = rows->setFilter(filterOn("nope", "x"));
    CHECK_FALSE(refused.has_value());
    CHECK(rows->filter().columns.empty());
    REQUIRE(rows->retain(std::vector<table::RowId>{id(3)}) == ui::RetainResult::Retained);
    h.settle();
    CHECK(rows->setFilter(filterOn("name", "row-1")).has_value());
    h.settle();
    CHECK(rows->filter().columns.contains("name"));
    std::size_t const sent = h.dispatcher.count("Table.samples");
    CHECK(rows->setFilter(filterOn("name", "row-1")).has_value());  // the same filter again
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == sent);
    CHECK(rows->counts().retained == 0);
}

TEST_CASE("cov window state: retaining rows asks for them again, moves with a key and clears", "[ui][table][cov]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, 50);
    std::size_t const before = h.dispatcher.count("Table.samples");
    CHECK(rows->retain(std::vector<table::RowId>{id(1), id(2), id(1)}) == ui::RetainResult::Retained);
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") > before);
    std::size_t const asked = h.dispatcher.count("Table.samples");
    CHECK(rows->retain(std::vector<table::RowId>{id(1)}) == ui::RetainResult::Retained);  // held already
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == asked);
    std::vector<table::RowId> many;
    for (std::int64_t key = 100; key < 100 + 10'001; ++key) {
        many.push_back(id(key));
    }
    CHECK(rows->retain(many) == ui::RetainResult::OverBound);
    // A key that moves takes the retained place; onto one held already it replaces it.
    rows->moveKey(id(1), id(1));
    rows->moveKey(id(9), id(10));  // not retained
    rows->moveKey(id(1), id(11));
    rows->moveKey(id(2), id(11));
    rows->clearRetained();
    h.settle();
    rows->clearRetained();  // nothing left to clear
    auto const view = rows->selectView();
    CHECK_FALSE((view.has_value() && !view->retained.empty()));
}

TEST_CASE("cov window state: the values list is asked once per request and cached", "[ui][table][cov]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, 50);
    std::optional<Distinct> first;
    table::DistinctRequest request;
    request.column = "name";
    rows->distinctValues(request).then([&first](Distinct const& got) { first = got; });
    h.settle();
    REQUIRE(h.pending("Table.samples") == 1);
    h.replyPage(
        R"({"revision":")" + h.described.revision +
        R"(","columns":[],"keys":[],"rows":[],"total":50,"distinct":{"values":[{"text":"a","operand":"a","count":2}],"distinct":1}})");
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK((*first)->distinct == 1);
    CHECK(rows->stats().distinctFetches == 1);
    std::optional<Distinct> again;
    rows->distinctValues(request).then([&again](Distinct const& got) { again = got; });
    h.settle();
    REQUIRE(again.has_value());
    CHECK(again->has_value());
    CHECK(rows->stats().distinctFetches == 1);  // served from what was kept
    // A reply with no values list, and a failed call, are errors.
    table::DistinctRequest other;
    other.column = "id";
    std::optional<Distinct> none;
    rows->distinctValues(other).then([&none](Distinct const& got) { none = got; });
    h.settle();
    h.replyPage(R"({"revision":")" + h.described.revision + R"(","columns":[],"keys":[],"rows":[],"total":50})");
    REQUIRE(none.has_value());
    CHECK_FALSE(none->has_value());
    table::DistinctRequest third;
    third.column = "c3";
    std::optional<Distinct> failed;
    rows->distinctValues(third).then([&failed](Distinct const& got) { failed = got; });
    h.settle();
    h.dispatcher.fail(h.oldest("Table.samples"), "error", "no list");
    h.settle();
    REQUIRE(failed.has_value());
    CHECK_FALSE(failed->has_value());
    table::DistinctRequest bad;
    bad.column = "id";
    bad.search = "x";
    std::optional<Distinct> garbled;
    rows->distinctValues(bad).then([&garbled](Distinct const& got) { garbled = got; });
    h.settle();
    h.replyPage("garbage");
    REQUIRE(garbled.has_value());
    CHECK_FALSE(garbled->has_value());
}

TEST_CASE("cov window state: a table that lists no values says so", "[ui][table][cov]") {
    RowsFixture h;
    table::TableDescription d = samplesDescription(3);
    d.capabilities.distinct = false;
    auto const rows = h.windowRows("samples", table::describe(std::move(d)));
    std::optional<Distinct> got;
    table::DistinctRequest request;
    request.column = "name";
    rows->distinctValues(request).then([&got](Distinct const& result) { got = result; });
    h.settle();
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->has_value());
    CHECK(got->error().code == table::TableErrorCode::Unavailable);
    CHECK(h.dispatcher.count("Table.samples") == 0);
}

TEST_CASE("cov window state: a selection of the view counts what it excepts and is refused until the view is known",
          "[ui][table][cov]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    CHECK_FALSE(rows->selectView().has_value());  // nothing has answered: no revision to name
    h.answerFirstPage(*rows, 500);
    ui::TableSelection keys;
    keys.keys = {id(1), id(2)};
    CHECK(rows->selectedCount(keys).rows == 2);
    CHECK(rows->selectedCount(keys).kind == ui::CountKind::Exact);
    ui::TableSelection whole;
    whole.view.emplace().revision = "r";
    whole.except = {id(1)};  // held: one fewer, and exact once nothing is owed
    CHECK(rows->selectedCount(whole).rows == 499);
    whole.except = {id(4000)};  // not held: it may or may not be in the view
    auto const outside = rows->selectedCount(whole);
    CHECK(outside.rows == 500);
    CHECK(outside.kind == ui::CountKind::Approximate);
    whole.except = {id(1), id(2), id(3)};
    CHECK(rows->selectedCount(whole).rows == 497);
}

TEST_CASE("cov window state: a window with no description keeps what it is asked and asks nothing",
          "[ui][table][cov][covr4]") {
    RowsFixture h;
    auto const rows = std::make_shared<ui::WindowRows>(h.session, h.options("samples"));
    h.settle();
    CHECK(rows->setSort({{.column = "anything", .dir = table::SortDirection::Ascending}}).has_value());
    CHECK(rows->setFilter(filterOn("anything", "x")).has_value());
    rows->visible(0, 30, h.columns);
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 0);
    CHECK_FALSE(rows->selectView().has_value());
    CHECK(rows->description() == nullptr);
    std::optional<Distinct> values;
    rows->distinctValues({}).then([&values](Distinct const& got) { values = got; });
    h.settle();
    REQUIRE(values.has_value());
    CHECK(values->error().code == table::TableErrorCode::Unavailable);
    std::optional<std::expected<std::vector<ui::FetchedRow>, table::TableError>> range;
    rows->rowsOf(0, 3, std::vector<std::string>{"id"}).then([&range](auto const& got) { range = got; });
    h.settle();
    REQUIRE(range.has_value());
    CHECK_FALSE(range->has_value());
    // A description that fits neither the sort nor the filter drops them as it arrives.
    rows->setDescription(samplesDescription(3));
    CHECK(rows->filter().columns.empty());
}

TEST_CASE("cov window state: a page of another revision is reported stale once", "[ui][table][cov][covr4]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    int stale = 0;
    rows->onStaleDescription([&stale] { ++stale; });
    rows->visible(0, 30, h.columns);
    h.replyPage(R"({"revision":"0000000000000000","columns":["id"],"keys":[1],"rows":[[1]],"total":1})");
    CHECK(stale == 1);
    rows->visible(0, 30, h.columns);
    h.settle();
    if (h.pending("Table.samples") > 0) {
        h.replyPage(R"({"revision":"0000000000000000","columns":["id"],"keys":[1],"rows":[[1]],"total":1})");
    }
    CHECK(stale == 1);  // the same revision is not said again
}

TEST_CASE("cov window state: a description that does not read or whose units do not build",
          "[ui][table][cov][covr4]") {
    RowsFixture h;
    auto const rows = std::make_shared<ui::WindowRows>(h.session, h.options("samples"));
    h.settle();
    rows->setDescription(ui::Value{std::int64_t{3}});
    REQUIRE(rows->error().has_value());
    CHECK(rows->error()->message.contains("does not read"));
    rows->setDescription(samplesDescription(3));
    CHECK_FALSE(rows->error().has_value());  // a description that reads clears it

    table::TableDescription d = samplesDescription(3);
    d.columns.at(2).info.kind = table::ColumnKind::Measure;
    d.units = {
        table::UnitInfo{
            .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = *table::parseDecimal("1"), .decimals = 3},
        table::UnitInfo{
            .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = *table::parseDecimal("1"), .decimals = 3}};
    auto const bad = std::make_shared<ui::WindowRows>(h.session, h.options("samples"));
    h.settle();
    bad->setDescription(table::describe(std::move(d)));
    bad->release();
    h.settle();
    CHECK(bad->description() != nullptr);
}

TEST_CASE("cov window state: an unsortable column and a range that is no range", "[ui][table][cov][covr4]") {
    RowsFixture h;
    table::TableDescription d = samplesDescription(3);
    d.columns.at(1).sortable = false;
    auto const rows = h.windowRows("samples", table::describe(std::move(d)));
    auto const refused = rows->setSort({{.column = "name", .dir = table::SortDirection::Ascending}});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.contains("name"));
    std::optional<std::expected<std::vector<table::RowId>, table::TableError>> got;
    rows->keysOf(-1, 5).then([&got](auto const& result) { got = result; });
    h.settle();
    REQUIRE(got.has_value());
    CHECK_FALSE(got->has_value());
    got.reset();
    rows->keysOf(std::numeric_limits<std::int64_t>::max(), 5).then([&got](auto const& result) { got = result; });
    h.settle();
    REQUIRE(got.has_value());
    CHECK_FALSE(got->has_value());
    got.reset();
    rows->keysOf(0, 0).then([&got](auto const& result) { got = result; });
    h.settle();
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    CHECK((*got)->empty());
}

TEST_CASE("cov window state: keys across two pages are answered when both have arrived", "[ui][table][cov][covr4]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    std::optional<std::expected<std::vector<table::RowId>, table::TableError>> got;
    rows->keysOf(190, 20).then([&got](auto const& result) { got = result; });  // rows 190..209: pages 0 and 1
    h.settle();
    REQUIRE(h.pending("Table.samples") == 2);
    h.replyPage(pageJson(0, 200, 400, std::vector<std::string>{}));
    CHECK_FALSE(got.has_value());  // one page still owed
    h.replyPage(pageJson(200, 200, 400, std::vector<std::string>{}));
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    CHECK((*got)->size() == 20);
    CHECK((*got)->front() == id(190));
    // A page that fails fails the range, whatever the other says.
    got.reset();
    rows->keysOf(390, 20).then([&got](auto const& result) { got = result; });
    h.settle();
    h.dispatcher.fail(h.oldest("Table.samples"), "error", "no");
    h.settle();
    CHECK_FALSE(got.has_value());  // the other page is still owed
    h.dispatcher.fail(h.oldest("Table.samples"), "error", "no");
    h.settle();
    REQUIRE(got.has_value());
    CHECK_FALSE(got->has_value());
}

TEST_CASE("cov window state: locate answers the row's index to its caller and a later locate stops the earlier",
          "[ui][table][cov][covr4]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    std::vector<std::optional<std::int64_t>> found;
    rows->locate(id(5), [&found](std::optional<std::int64_t> index) { found.push_back(index); });
    h.settle();
    h.replyPage(R"({"revision":")" + h.described.revision +
                R"(","columns":["id"],"keys":[5],"rows":[[5]],"indices":[41],"total":100})");
    REQUIRE(found.size() == 1);
    CHECK(found.front() == 41);
    rows->locate(id(6), [&found](std::optional<std::int64_t> index) { found.push_back(index); });
    h.settle();
    rows->locate(id(7), [&found](std::optional<std::int64_t> index) { found.push_back(index); });  // stops id 6's
    h.settle();
    CHECK(h.stopped(h.dispatcher.count("Table.samples") - 2));
    h.dispatcher.reply(
        h.dispatcher.last("Table.samples"),
        R"({"revision":")" + h.described.revision + R"(","columns":["id"],"keys":[],"rows":[],"total":100})");
    h.settle();
    REQUIRE(found.size() == 2);
    CHECK_FALSE(found.back().has_value());  // not in the view
    rows->stopLocating();
}
