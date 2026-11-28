// SPDX-License-Identifier: Apache-2.0
//
// The rows a selection resolves to: where a view's membership is cached, which retained rows join it, and where
// the search for chosen keys stops.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/table/description.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/row_selection.hpp>
#include <morph/table/view_cache.hpp>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using morph::table::detail::PlainSelection;

namespace {

ServedTable served(std::shared_ptr<DataSource const> source) {
    ServedTable out;
    out.source = std::move(source);
    return out;
}

std::shared_ptr<VectorSource> numbered() {
    std::vector<std::vector<Cell>> rows;
    rows.reserve(6);
    for (std::int64_t at = 0; at < 6; ++at) {
        rows.push_back({Cell{at}, Cell{std::string{at % 2 == 0 ? "even" : "odd"}}});
    }
    return tabletest::makeSource(
        {tabletest::col("n", ColumnKind::Integer), tabletest::col("parity", ColumnKind::Text)}, std::move(rows));
}

RowId key(std::int64_t value) { return RowId{value}; }

FilterSpec evens() {
    FilterSpec filters;
    filters.columns["parity"].include = {FilterEntry{.eq = "even"}};
    return filters;
}

}  // namespace

TEST_CASE("a view's membership is cached only under a record name, whether or not the source is pinned",
          "[covf][table][selection]") {
    auto const source = numbered();
    ViewCache views;
    auto const table = served(source);
    auto const uncached = detail::membership(table, evens(), {}, &views, {});
    REQUIRE(uncached);
    CHECK(uncached->rows == std::vector<std::uint32_t>{0, 2, 4});

    // A source that is not a pinned snapshot is cached under no pin.
    auto const first = detail::membership(table, evens(), {}, &views, "T.rows");
    auto const second = detail::membership(table, evens(), {}, &views, "T.rows");
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->rows == uncached->rows);
    CHECK(second->digest.text() == first->digest.text());

    auto const pinned = detail::pinned(table);
    auto const third = detail::membership(pinned, evens(), {}, &views, "T.rows");
    REQUIRE(third);
    CHECK(third->rows == uncached->rows);
}

TEST_CASE("a retained row joins the view in source order unless the view already holds it",
          "[covf][table][selection]") {
    auto const table = served(numbered());
    auto const held = detail::membership(table, evens(), std::vector<RowId>{key(2)}, nullptr, {});
    REQUIRE(held);
    CHECK(held->rows == std::vector<std::uint32_t>{0, 2, 4});
    auto const added = detail::membership(table, evens(), std::vector<RowId>{key(3), key(99), key(1)}, nullptr, {});
    REQUIRE(added);
    CHECK(added->rows == std::vector<std::uint32_t>{0, 1, 2, 3, 4});
    CHECK(added->digest.text() != held->digest.text());
}

TEST_CASE("a row-scoped selection with a scope is resolved, and a search for keys stops at the last one found",
          "[covf][table][selection]") {
    auto table = served(numbered());
    table.scope = "row-7";
    PlainSelection selection;
    selection.keys = {key(0), key(1)};
    auto const resolved = detail::resolveSelection(table, selection, SelectionPolicy::Exact, nullptr, {}, true);
    REQUIRE(resolved);
    CHECK(resolved->keys == std::vector<RowId>{key(0), key(1)});
    CHECK(resolved->dropped == 0);

    selection.keys = {};
    auto const none = detail::resolveSelection(table, selection, SelectionPolicy::Exact, nullptr, {}, true);
    REQUIRE(none);
    CHECK(none->keys.empty());
}

TEST_CASE("the inline executor runs a task at once and ignores an empty one", "[covf][table][selection]") {
    detail::ResolveInline owner;
    bool ran = false;
    owner.post([&ran] { ran = true; });
    CHECK(ran);
    owner.post({});
    CHECK(ran);
}

namespace {

TableColumn describedColumn(std::string const& id, ColumnKind kind) {
    return TableColumn{
        .info = {.id = id, .kind = kind, .comparator = {}, .format = {}, .level = {}}, .label = id, .group = {}};
}

ServedTable describedTable(bool pin) {
    auto const description = describe(TableDescription{
        .id = "t",
        .label = "T",
        .columns = {describedColumn("n", ColumnKind::Integer), describedColumn("parity", ColumnKind::Text)},
        .key = "n"});
    ServedTable out{.description = description,
                    .source = numbered(),
                    .levels = {},
                    .mayRead = {},
                    .scope = {},
                    .services = {},
                    .limits = {}};
    return pin ? detail::pinned(std::move(out)) : out;
}

}  // namespace

TEST_CASE("a page and a values list are cached under a record name, pinned or not, and not cached without one",
          "[covf][table][selection]") {
    for (bool const pin : {false, true}) {
        CAPTURE(pin);
        auto const table = describedTable(pin);
        ViewCache views;
        TableQuery page;
        page.filters = evens();
        TableQuery values;
        values.distinct = DistinctRequest{.column = "parity"};
        for (auto const& query : {page, values}) {
            for (std::string_view const record : {"T.rows", ""}) {
                CAPTURE(record);
                auto const first = detail::answerQuery(
                    table, query, detail::AnswerContext{.views = &views, .record = record, .rowScoped = false});
                auto const second = detail::answerQuery(
                    table, query, detail::AnswerContext{.views = &views, .record = record, .rowScoped = false});
                REQUIRE(first);
                REQUIRE(second);
                if (query.distinct) {
                    REQUIRE(second->distinct);
                    CHECK(second->distinct->values.size() == 2);
                } else {
                    CHECK(second->total == 3);
                    CHECK(second->rows.size() == 3);
                }
            }
        }
    }
}
