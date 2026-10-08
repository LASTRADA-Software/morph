// SPDX-License-Identifier: Apache-2.0
//
// morph::table TableQuery, Page, validate, apply and PageWindow (spec 7 §2, §9).
//
// Mutations these tests were seen to fail on:
//  - validate not clamping page.limit: "an untrusted query is bounded" fails.
//  - validate skipping the readable-column check: "a column the principal may
//    not read is refused" fails.
//  - forms' annotateBasicMemberProperty not emitting a schema marker: "a list
//    action's table member is marked x-table" fails.
//  - PageWindow::visible not skipping pages in flight: "the window fetches the
//    pages around the visible rows" fails.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <morph/table/query.hpp>
#include <optional>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::viewKeys;

namespace {

struct ResultRow {
    std::int64_t id = 0;
    std::string analysis;
    morph::math::Rational mass;
    std::optional<double> ph;
    bool approved = false;
};

struct ListResults {
    std::optional<std::int64_t> sampleId;
    TableQuery table;
};

RowKey<ResultRow> byId() {
    return [](ResultRow const& row) { return RowId{row.id}; };
}

std::vector<ResultRow> resultRows(std::size_t count) {
    std::vector<ResultRow> rows;
    for (std::size_t i = 0; i < count; ++i) {
        auto const n = static_cast<std::int64_t>(i);
        rows.push_back(
            ResultRow{.id = n,
                      .analysis = (i % 3 == 0 ? "pH " : "Lead ") + std::to_string(i % 17),
                      .mass = morph::math::Rational{morph::math::Numerator{(n * 37) % 1000},
                                                    morph::math::Denominator{10}, morph::math::DecimalPlaces{1}},
                      .ph = i % 5 == 0 ? std::nullopt : std::optional<double>{static_cast<double>(i % 14)},
                      .approved = i % 2 == 0});
    }
    return rows;
}

FilterSpec filterOn(std::string column, FilterEntry entry) {
    FilterSpec spec;
    spec.columns.emplace(std::move(column),
                         ColumnFilter{.combine = Combine::Any, .include = {std::move(entry)}, .exclude = {}});
    return spec;
}

}  // namespace

TEST_CASE("table: a TableQuery round-trips through JSON in the spec's shape", "[table][query]") {
    constexpr std::string_view json =
        R"({"sort":[{"column":"mass","dir":"desc"}],"filters":{"columns":{"analysis":{"include":[{"contains":"pH"}]}}},"page":{"offset":40,"limit":20}})";
    TableQuery query;
    REQUIRE_FALSE(glz::read_json(query, json));
    REQUIRE(query.sort.size() == 1);
    CHECK(query.sort[0] == SortKey{"mass", SortDirection::Descending});
    CHECK(query.page == PageRequest{.offset = 40, .limit = 20});
    CHECK(query.filters.columns.at("analysis").include[0].contains == "pH");
    std::string written;
    REQUIRE_FALSE(glz::write_json(query, written));
    TableQuery again;
    REQUIRE_FALSE(glz::read_json(again, written));
    CHECK(again.sort == query.sort);
    CHECK(again.page == query.page);

    std::string kind;
    REQUIRE_FALSE(glz::write_json(ColumnKind::DateTime, kind));
    CHECK(kind == R"("dateTime")");
    ColumnInfo info;
    REQUIRE_FALSE(glz::read_json(info, R"({"id":"on","kind":"date"})"));
    CHECK(info.kind == ColumnKind::Date);

    Page<ResultRow> page{.rows = resultRows(2), .total = 2, .offset = 0};
    std::string pageJson;
    REQUIRE_FALSE(glz::write_json(page, pageJson));
    CHECK(pageJson.find(R"("total":2)") != std::string::npos);
}

TEST_CASE("table: a list action's table member is marked x-table", "[table][query]") {
    auto const& schema = morph::forms::schemaJson<ListResults>();
    glz::generic dom;
    REQUIRE_FALSE(glz::read_json(dom, schema));
    auto const& table = dom["properties"]["table"];
    REQUIRE(table.contains("x-table"));
    CHECK(table["x-table"].get<bool>());
    CHECK_FALSE(dom["properties"]["sampleId"].contains("x-table"));
}

TEST_CASE("table: typed rows become columns with inferred kinds", "[table][query]") {
    RowsSource<ResultRow> const source{
        std::make_shared<std::vector<ResultRow> const>(resultRows(3)), byId(), {{"id", ColumnKind::Key}}};
    auto const columns = source.columns();
    REQUIRE(columns.size() == 5);
    CHECK(columns[0].id == "id");
    CHECK(columns[0].kind == ColumnKind::Key);
    CHECK(columns[1].kind == ColumnKind::Text);
    CHECK(columns[2].kind == ColumnKind::Decimal);
    CHECK(columns[3].kind == ColumnKind::Number);
    CHECK(columns[4].kind == ColumnKind::Bool);
    auto const snapshot = source.snapshot();
    CHECK(std::holds_alternative<std::monostate>(snapshot->cell(0, 3)));
    CHECK(std::get<double>(snapshot->cell(1, 3)) == 1.0);
    CHECK(std::get<std::int64_t>(snapshot->rowId(2)) == 2);
}

TEST_CASE("table: client mode and server mode give the same rows", "[table][query]") {
    auto const rows = resultRows(2500);
    auto source =
        std::make_shared<RowsSource<ResultRow>>(std::make_shared<std::vector<ResultRow> const>(rows), byId());
    std::vector<std::pair<SortChain, FilterSpec>> const corpus{
        {{{"mass", SortDirection::Descending}}, {}},
        {{{"analysis", SortDirection::Ascending}, {"id", SortDirection::Descending}},
         filterOn("ph", FilterEntry{.ge = "7"})},
        {{{"ph", SortDirection::Ascending}}, filterOn("analysis", FilterEntry{.startsWith = "PH"})},
        {{}, filterOn("approved", FilterEntry{.eq = "true"})},
    };
    for (auto const& [sort, filters] : corpus) {
        Engine client{source};
        REQUIRE(client.setSort(sort).has_value());
        REQUIRE(client.setFilter(filters).has_value());

        std::vector<RowId> served;
        std::optional<std::int64_t> total;
        for (std::int64_t offset = 0;; offset += 333) {
            auto const page = apply(
                rows, byId(), TableQuery{.sort = sort, .filters = filters, .page = {.offset = offset, .limit = 333}});
            REQUIRE(page.has_value());
            total = page->total;
            for (auto const& row : page->rows) {
                served.emplace_back(row.id);
            }
            if (page->rows.size() < 333) {
                break;
            }
        }
        CHECK(served == viewKeys(client));
        CHECK(total == static_cast<std::int64_t>(client.viewRowCount()));
    }
}

TEST_CASE("table: an untrusted query is bounded", "[table][query]") {
    auto const rows = resultRows(10);
    RowsSource<ResultRow> const source{std::make_shared<std::vector<ResultRow> const>(rows), byId()};
    QueryLimits limits;
    limits.maxLimit = 5;
    limits.maxSortKeys = 1;
    limits.maxTextLength = 4;
    limits.maxFilterColumns = 1;
    limits.maxEntries = 2;
    limits.maxGroups = 1;
    limits.totalCap = 8;

    auto const clamped = validate(TableQuery{.sort = {}, .filters = {}, .page = {.offset = 0, .limit = 1000}},
                                  source.columns(), limits);
    REQUIRE(clamped.has_value());
    CHECK(clamped->page.limit == 5);

    auto const code = [&](TableQuery const& query) {
        auto const result = validate(query, source.columns(), limits);
        return result ? std::optional<TableErrorCode>{} : std::optional{result.error().code};
    };
    CHECK(code(TableQuery{.sort = {}, .filters = {}, .page = {.offset = -1, .limit = 1}}) ==
          TableErrorCode::LimitExceeded);
    CHECK(code(TableQuery{.sort = {}, .filters = {}, .page = {.offset = 0, .limit = -1}}) ==
          TableErrorCode::LimitExceeded);
    CHECK(code(TableQuery{.sort = {{"id", {}}, {"mass", {}}}, .filters = {}, .page = {}}) ==
          TableErrorCode::LimitExceeded);
    CHECK(code(TableQuery{.sort = {{"nope", {}}}, .filters = {}, .page = {}}) == TableErrorCode::UnknownColumn);
    CHECK(code(TableQuery{
              .sort = {}, .filters = filterOn("analysis", FilterEntry{.contains = "toolong"}), .page = {}}) ==
          TableErrorCode::LimitExceeded);
    auto twoColumns = filterOn("analysis", FilterEntry{.contains = "pH"});
    twoColumns.columns.emplace("id", ColumnFilter{});
    CHECK(code(TableQuery{.sort = {}, .filters = twoColumns, .page = {}}) == TableErrorCode::LimitExceeded);
    auto manyEntries = filterOn("analysis", FilterEntry{.contains = "a"});
    manyEntries.columns["analysis"].exclude = {FilterEntry{.eq = "b"}, FilterEntry{.eq = "c"}};
    CHECK(code(TableQuery{.sort = {}, .filters = manyEntries, .page = {}}) == TableErrorCode::LimitExceeded);
    FilterSpec groups;
    groups.groups = {GroupFilter{.columns = {"analysis"}, .combine = {}, .include = {FilterEntry{.contains = "a"}}},
                     GroupFilter{.columns = {"analysis"}, .combine = {}, .include = {FilterEntry{.contains = "b"}}}};
    CHECK(code(TableQuery{.sort = {}, .filters = groups, .page = {}}) == TableErrorCode::LimitExceeded);
    groups.groups.resize(1);
    groups.groups[0].columns = {"secret"};
    CHECK(code(TableQuery{.sort = {}, .filters = groups, .page = {}}) == TableErrorCode::UnknownColumn);

    ApplyOptions options;
    options.limits = limits;
    auto const page =
        apply(rows, byId(), TableQuery{.sort = {}, .filters = {}, .page = {.offset = 0, .limit = 100}}, options);
    REQUIRE(page.has_value());
    CHECK(page->rows.size() == 5);
    CHECK_FALSE(page->total.has_value());  // 10 rows is above the cap of 8
    auto const past =
        apply(rows, byId(), TableQuery{.sort = {}, .filters = {}, .page = {.offset = 50, .limit = 5}}, options);
    REQUIRE(past.has_value());
    CHECK(past->rows.empty());
}

TEST_CASE("table: a column the principal may not read is refused", "[table][query]") {
    auto const rows = resultRows(10);
    ApplyOptions options;
    options.mayRead = [](std::string_view column) { return column != "mass"; };
    auto const sorted = apply(rows, byId(), TableQuery{.sort = {{"mass", {}}}, .filters = {}, .page = {}}, options);
    REQUIRE_FALSE(sorted.has_value());
    CHECK(sorted.error().code == TableErrorCode::ColumnNotReadable);
    CHECK(sorted.error().column == "mass");
    auto const filtered =
        apply(rows, byId(), TableQuery{.sort = {}, .filters = filterOn("mass", FilterEntry{.gt = "1"}), .page = {}},
              options);
    REQUIRE_FALSE(filtered.has_value());
    CHECK(filtered.error().code == TableErrorCode::ColumnNotReadable);
    CHECK(apply(rows, byId(), TableQuery{.sort = {{"id", {}}}, .filters = {}, .page = {}}, options).has_value());
}

TEST_CASE("table: a server's filter error is typed and travels as JSON", "[table][query]") {
    auto const rows = resultRows(3);
    auto const result = apply(
        rows, byId(), TableQuery{.sort = {}, .filters = filterOn("mass", FilterEntry{.gt = "heavy"}), .page = {}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == TableErrorCode::InvalidValue);
    std::string json;
    REQUIRE_FALSE(glz::write_json(result.error(), json));
    CHECK(json.find(R"("code":"invalidValue")") != std::string::npos);
    CHECK(json.find(R"("column":"mass")") != std::string::npos);
}

TEST_CASE("table: LIKE patterns keep % and _ ordinary", "[table][query]") {
    CHECK(escapeLikePattern("100%_a\\b") == "100\\%\\_a\\\\b");
    CHECK(escapeLikePattern("50%", '!') == "50!%");
}

TEST_CASE("table: the window fetches the pages around the visible rows", "[table][query]") {
    PageWindow<int> window{100, 4};
    auto const first = window.visible(250, 30);
    REQUIRE(first.size() == 3);
    CHECK(first[0] == PageRequest{.offset = 100, .limit = 100});
    CHECK(first[1] == PageRequest{.offset = 200, .limit = 100});
    CHECK(first[2] == PageRequest{.offset = 300, .limit = 100});
    CHECK(window.pending());
    CHECK(window.visible(250, 30).empty());  // all in flight

    window.accept(Page<int>{.rows = std::vector<int>(100, 2), .total = 350, .offset = 200});
    CHECK(window.row(250) != nullptr);
    CHECK(*window.row(250) == 2);
    CHECK(window.row(150) == nullptr);
    CHECK(window.total() == 350);
    window.accept(Page<int>{.rows = std::vector<int>(50, 3), .total = 350, .offset = 300});
    CHECK(window.row(349) != nullptr);
    CHECK(window.row(350) == nullptr);

    // Pages past the total are never requested.
    auto const near = window.visible(320, 30);
    CHECK(near.empty());

    // The window keeps at most four pages, dropping the farthest.
    window.accept(Page<int>{.rows = std::vector<int>(100, 1), .total = 350, .offset = 100});
    window.reset();
    CHECK(window.pagesHeld() == 0);
    auto const top = window.visible(0, 10);
    REQUIRE(top.size() == 2);
    for (std::int64_t page = 0; page < 6; ++page) {
        window.accept(
            Page<int>{.rows = std::vector<int>(100, static_cast<int>(page)), .total = 600, .offset = page * 100});
    }
    CHECK(window.pagesHeld() == 4);
    CHECK(window.row(0) != nullptr);
    CHECK(window.row(550) == nullptr);
}

TEST_CASE("table: a failed page shows its error until a reply arrives", "[table][query]") {
    PageWindow<int> window{10, 4};
    auto const requests = window.visible(0, 5);
    REQUIRE_FALSE(requests.empty());
    window.fail(requests[0], TableError{.code = TableErrorCode::UnsupportedOperator, .column = "x", .message = "no"});
    REQUIRE(window.error().has_value());
    CHECK(window.error()->code == TableErrorCode::UnsupportedOperator);
    auto const retry = window.visible(0, 5);
    REQUIRE_FALSE(retry.empty());
    CHECK(retry[0] == requests[0]);
    window.accept(Page<int>{.rows = std::vector<int>(10, 0), .total = 10, .offset = 0});
    CHECK_FALSE(window.error().has_value());
}
