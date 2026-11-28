// SPDX-License-Identifier: Apache-2.0
//
// The filter text a selection carries: what it leaves out of the wire form of a filter.

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

TEST_CASE("a filter's compact text leaves out what is empty and what is the default", "[covc][table][selection]") {
    FilterSpec const empty;
    auto const none = detail::compactFilterJson(empty);
    CHECK(none.contains("columns"));
    CHECK_FALSE(none.contains("groups"));

    FilterSpec filters;
    filters.columns["a"] = ColumnFilter{
        .combine = Combine::Any, .include = {FilterEntry{.eq = "x", .oneOf = std::nullopt}}, .exclude = {}};
    filters.columns["b"] =
        ColumnFilter{.combine = Combine::All, .include = {FilterEntry{.contains = "y"}}, .exclude = {}};
    filters.groups = {GroupFilter{.columns = {"a", "b"}, .include = {FilterEntry{.eq = ""}}}};
    auto const text = detail::compactFilterJson(filters);
    CHECK(text.contains("\"eq\":\"x\""));
    CHECK(text.contains("\"contains\":\"y\""));
    CHECK(text.contains("\"combine\":\"all\""));
    CHECK_FALSE(text.contains("\"combine\":\"any\""));
    CHECK_FALSE(text.contains("\"exclude\""));
    CHECK_FALSE(text.contains("\"group\""));
    CHECK_FALSE(text.contains("\"eq\":\"\""));
}

namespace {

ServedTable served(std::shared_ptr<DataSource const> source) {
    ServedTable out;
    out.source = std::move(source);
    return out;
}

std::shared_ptr<VectorSource> numbered() {
    std::vector<std::vector<Cell>> rows;
    for (std::int64_t at = 0; at < 6; ++at) {
        rows.push_back({Cell{at}, Cell{std::string{at % 2 == 0 ? "even" : "odd"}}});
    }
    return tabletest::makeSource(
        {tabletest::col("n", ColumnKind::Integer), tabletest::col("parity", ColumnKind::Text)}, std::move(rows));
}

RowId key(std::int64_t value) { return RowId{value}; }

}  // namespace

TEST_CASE("a selection by keys keeps the rows that exist and counts the rest as dropped", "[covc][table][selection]") {
    auto const source = numbered();
    PlainSelection selection;
    selection.keys = {key(4), key(1), key(99), key(1)};
    auto const resolved =
        detail::resolveSelection(served(source), selection, SelectionPolicy::Exact, nullptr, {}, false);
    REQUIRE(resolved);
    CHECK(resolved->keys == std::vector<RowId>{key(1), key(4)});
    CHECK(resolved->dropped == 1);

    ServedTable limited = served(source);
    limited.limits.maxSelectedRows = 1;
    auto const over = detail::resolveSelection(limited, selection, SelectionPolicy::Exact, nullptr, {}, false);
    REQUIRE_FALSE(over);
    CHECK(over.error().code == TableErrorCode::LimitExceeded);
}

TEST_CASE("a selection is refused for a table that cannot serve it", "[covc][table][selection]") {
    PlainSelection const selection;
    auto const noScope =
        detail::resolveSelection(served(numbered()), selection, SelectionPolicy::Exact, nullptr, {}, true);
    REQUIRE_FALSE(noScope);
    CHECK(noScope.error().code == TableErrorCode::SourceContract);
    auto const noSource =
        detail::resolveSelection(ServedTable{}, selection, SelectionPolicy::Exact, nullptr, {}, false);
    REQUIRE_FALSE(noSource);
    CHECK(noSource.error().code == TableErrorCode::Unavailable);
    auto const noMembers = detail::membership(ServedTable{}, FilterSpec{}, {}, nullptr, {});
    REQUIRE_FALSE(noMembers);
    CHECK(noMembers.error().code == TableErrorCode::Unavailable);
}

TEST_CASE("a selection of the view is its filtered rows, the retained ones, less the exceptions",
          "[covc][table][selection]") {
    auto const source = numbered();
    auto const table = served(source);
    PlainSelection selection;
    selection.isView = true;
    selection.filters.columns["parity"].include = {FilterEntry{.eq = "even"}};
    selection.retained = {key(1)};
    selection.except = {key(2)};
    auto const current = detail::resolveSelection(table, selection, SelectionPolicy::Current, nullptr, {}, false);
    REQUIRE(current);
    CHECK(current->keys == std::vector<RowId>{key(0), key(1), key(4)});

    auto const members = detail::membership(table, selection.filters, selection.retained, nullptr, {});
    REQUIRE(members);
    CHECK(members->rows == std::vector<std::uint32_t>{0, 1, 2, 4});

    selection.revision = "stale";
    auto const exact = detail::resolveSelection(table, selection, SelectionPolicy::Exact, nullptr, {}, false);
    REQUIRE_FALSE(exact);
    CHECK(exact.error().code == TableErrorCode::ViewChanged);
    selection.revision = members->digest.text();
    CHECK(detail::resolveSelection(table, selection, SelectionPolicy::Exact, nullptr, {}, false));

    ServedTable limited = table;
    limited.limits.maxSelectedRows = 2;
    auto const tooMany = detail::resolveSelection(limited, selection, SelectionPolicy::Current, nullptr, {}, false);
    REQUIRE_FALSE(tooMany);
    CHECK(tooMany.error().code == TableErrorCode::LimitExceeded);

    PlainSelection unknown;
    unknown.isView = true;
    unknown.filters.columns["nope"] = {};
    auto const refused = detail::resolveSelection(table, unknown, SelectionPolicy::Current, nullptr, {}, false);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == TableErrorCode::UnknownColumn);
}

TEST_CASE("a selection of the view built twice with a cache reads the second from it", "[covc][table][selection]") {
    auto const source = numbered();
    auto const table = served(source);
    ViewCache views;
    PlainSelection selection;
    selection.isView = true;
    selection.filters.columns["parity"].include = {FilterEntry{.eq = "odd"}};
    auto const first = detail::resolveSelection(table, selection, SelectionPolicy::Current, &views, "rec", false);
    auto const second = detail::resolveSelection(table, selection, SelectionPolicy::Current, &views, "rec", false);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->keys == std::vector<RowId>{key(1), key(3), key(5)});
    CHECK(second->keys == first->keys);
}
