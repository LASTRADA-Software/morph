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
//  - PageWindow stopping at an AtLeast total: "a total is exact or a lower
//    bound, and a lower bound does not end the window" fails.
//  - apply skipping validate: "apply refuses what validate and compile refuse,
//    by code" fails (the offset case answers an empty page).

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/forms/forms.hpp>
#include <morph/table/query.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::viewKeys;

// Reflected by glaze, so not in an anonymous namespace.
namespace tquery {

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

}  // namespace tquery

using tquery::ListResults;
using tquery::ResultRow;

namespace {

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
    CHECK(pageJson.contains(R"("total":2)"));
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
    auto const source =
        std::make_shared<RowsSource<ResultRow>>(std::make_shared<std::vector<ResultRow> const>(rows), byId());
    std::vector<std::pair<SortChain, FilterSpec>> const corpus{
        {{{.column = "mass", .dir = SortDirection::Descending}}, {}},
        {{{.column = "analysis", .dir = SortDirection::Ascending}, {.column = "id", .dir = SortDirection::Descending}},
         filterOn("ph", FilterEntry{.ge = "7"})},
        {{{.column = "ph", .dir = SortDirection::Ascending}}, filterOn("analysis", FilterEntry{.startsWith = "PH"})},
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
    auto const sorted =
        apply(rows, byId(), TableQuery{.sort = {{.column = "mass", .dir = {}}}, .filters = {}, .page = {}}, options);
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
    CHECK(json.contains(R"("code":"invalidValue")"));
    CHECK(json.contains(R"("column":"mass")"));
}

TEST_CASE("table: the window fetches the pages around the visible rows", "[table][query]") {
    PageWindow<int> window{100, 4};
    auto const first = window.visible(250, 30);
    REQUIRE(first.size() == 3);
    CHECK(first[0].request == PageRequest{.offset = 100, .limit = 100});
    CHECK(first[1].request == PageRequest{.offset = 200, .limit = 100});
    CHECK(first[2].request == PageRequest{.offset = 300, .limit = 100});
    CHECK(window.pending());
    CHECK(window.visible(250, 30).empty());  // all in flight

    CHECK(window.accept(first[1], Page<int>{.rows = std::vector<int>(100, 2), .total = 350, .offset = 200}));
    CHECK(window.row(250) != nullptr);
    CHECK(*window.row(250) == 2);
    CHECK(window.row(150) == nullptr);
    CHECK(window.total() == 350);
    CHECK(window.accept(first[2], Page<int>{.rows = std::vector<int>(50, 3), .total = 350, .offset = 300}));
    CHECK(window.row(349) != nullptr);
    CHECK(window.row(350) == nullptr);

    // Pages past the total are never requested, and a short last page is complete.
    auto const near = window.visible(320, 30);
    CHECK(near.empty());

    // The window keeps at most four pages, dropping the farthest.
    CHECK(window.accept(first[0], Page<int>{.rows = std::vector<int>(100, 1), .total = 350, .offset = 100}));
    window.reset();
    CHECK(window.pagesHeld() == 0);
    auto const top = window.visible(0, 10);
    REQUIRE(top.size() == 2);
    auto more = window.visible(500, 10);
    REQUIRE(more.size() == 3);
    std::vector<PageFetch> fetches{top.begin(), top.end()};
    fetches.insert(fetches.end(), more.begin(), more.end());
    for (auto const& fetch : fetches) {
        auto const page = fetch.request.offset / 100;
        CHECK(window.accept(fetch, Page<int>{.rows = std::vector<int>(100, static_cast<int>(page)),
                                             .total = 700,
                                             .offset = fetch.request.offset}));
    }
    CHECK(window.pagesHeld() == 4);
    CHECK(window.row(550) != nullptr);
    CHECK(window.row(0) == nullptr);
}

TEST_CASE("table: a failed page shows its error until a reply arrives", "[table][query]") {
    PageWindow<int> window{10, 4};
    auto const requests = window.visible(0, 5);
    REQUIRE_FALSE(requests.empty());
    CHECK(window.fail(requests[0],
                      TableError{.code = TableErrorCode::UnsupportedOperator, .column = "x", .message = "no"}));
    REQUIRE(window.error().has_value());
    CHECK(window.error()->code == TableErrorCode::UnsupportedOperator);
    auto const retry = window.visible(0, 5);
    REQUIRE_FALSE(retry.empty());
    CHECK(retry[0] == requests[0]);
    CHECK(window.accept(retry[0], Page<int>{.rows = std::vector<int>(10, 0), .total = 10, .offset = 0}));
    CHECK_FALSE(window.error().has_value());
}

TEST_CASE("table: every operand of a filter entry is bounded in length", "[table][query]") {
    auto const rows = resultRows(1);
    RowsSource<ResultRow> const source{std::make_shared<std::vector<ResultRow> const>(rows), byId()};
    QueryLimits limits;
    limits.maxTextLength = 4;
    limits.maxGroupColumns = 2;
    limits.maxEntries = 2;
    limits.maxOffset = 100;
    auto const code = [&](FilterSpec const& filters) {
        auto const result = validate(TableQuery{.sort = {}, .filters = filters, .page = {}}, source.columns(), limits);
        return result ? std::optional<TableErrorCode>{} : std::optional{result.error().code};
    };
    auto const longText = std::string{"12345"};
    std::vector<FilterEntry> const tooLong{
        FilterEntry{.eq = longText},
        FilterEntry{.ne = longText},
        FilterEntry{.lt = longText},
        FilterEntry{.le = longText},
        FilterEntry{.gt = longText},
        FilterEntry{.ge = longText},
        FilterEntry{.startsWith = longText},
        FilterEntry{.endsWith = longText},
        FilterEntry{.between = std::array<std::string, 2>{longText, "1"}},
        FilterEntry{.between = std::array<std::string, 2>{"1", longText}},
    };
    for (auto const& entry : tooLong) {
        CHECK(code(filterOn("mass", entry)) == TableErrorCode::LimitExceeded);
    }
    CHECK(code(filterOn("mass", FilterEntry{.between = std::array<std::string, 2>{"1", "2"}})) == std::nullopt);

    FilterSpec excluded;
    excluded.columns["analysis"].exclude.push_back(FilterEntry{.eq = longText});
    CHECK(code(excluded) == TableErrorCode::LimitExceeded);

    auto const group = [](std::vector<std::string> columns, std::vector<FilterEntry> include) {
        FilterSpec spec;
        spec.groups.push_back(
            GroupFilter{.columns = std::move(columns), .combine = {}, .include = std::move(include)});
        return spec;
    };
    CHECK(code(group({"analysis", "id", "mass"}, {FilterEntry{.eq = "a"}})) == TableErrorCode::LimitExceeded);
    CHECK(code(group({"analysis"}, {FilterEntry{.eq = "a"}, FilterEntry{.eq = "b"}, FilterEntry{.eq = "c"}})) ==
          TableErrorCode::LimitExceeded);
    CHECK(code(group({"analysis"}, {FilterEntry{.contains = longText}})) == TableErrorCode::LimitExceeded);
    CHECK(code(group({"analysis", "id"}, {FilterEntry{.contains = "pH"}})) == std::nullopt);

    auto const offset =
        validate(TableQuery{.sort = {}, .filters = {}, .page = {.offset = 101, .limit = 1}}, source.columns(), limits);
    REQUIRE_FALSE(offset.has_value());
    CHECK(offset.error().code == TableErrorCode::LimitExceeded);
}

TEST_CASE("table: a service that throws in apply is a typed error", "[table][query]") {
    std::vector<std::vector<morph::table::Cell>> cells{{std::int64_t{2}}, {std::int64_t{1}}};
    auto const source =
        tabletest::makeSource({tabletest::col("c", morph::table::ColumnKind::Custom, "broken")}, std::move(cells));
    ApplyOptions options;
    options.services.comparators.emplace(
        "broken", [](morph::table::Cell const&, morph::table::Cell const&) -> std::strong_ordering {
            throw std::runtime_error{"comparator down"};
        });
    auto const page =
        apply(*source, TableQuery{.sort = {{.column = "c", .dir = {}}}, .filters = {}, .page = {}}, options);
    REQUIRE_FALSE(page.has_value());
    CHECK(page.error().code == TableErrorCode::ServiceFailed);
    CHECK(page.error().message.contains("comparator down"));
}

TEST_CASE("table: the window ignores a reply it does not await or that no longer fits", "[table][query]") {
    PageWindow<int> window{10, 8};
    auto const first = window.visible(0, 5);
    REQUIRE(first.size() == 2);  // pages 0 and 1
    CHECK(window.row(-1) == nullptr);

    // A short reply with a known, larger total leaves page 0 incomplete.
    CHECK(window.accept(first[0], Page<int>{.rows = std::vector<int>(4, 7), .total = 100, .offset = 0}));
    CHECK(window.row(3) != nullptr);
    CHECK(window.row(4) == nullptr);
    CHECK_FALSE(window.accept(first[0], Page<int>{.rows = std::vector<int>(10, 8), .total = 100, .offset = 0}));

    // The rest of page 0 is fetched from where it ends; a late reply to its
    // start is not the fetch in flight, so it stores nothing and the rest
    // stays awaited.
    auto const rest = window.visible(0, 5);
    REQUIRE(rest.size() == 1);
    CHECK(rest[0].request == PageRequest{.offset = 4, .limit = 6});
    CHECK_FALSE(window.accept(first[0], Page<int>{.rows = std::vector<int>(10, 8), .total = 100, .offset = 0}));
    CHECK(*window.row(0) == 7);
    CHECK(window.row(4) == nullptr);
    CHECK(window.visible(0, 5).empty());
    CHECK(window.accept(rest[0], Page<int>{.rows = std::vector<int>(6, 9), .total = 100, .offset = 4}));
    REQUIRE(window.row(4) != nullptr);
    CHECK(*window.row(4) == 9);
}

TEST_CASE("table: the window evicts the page farthest from the visible rows", "[table][query]") {
    PageWindow<int> window{10, 1};
    auto const fetches = window.visible(0, 5);
    REQUIRE(fetches.size() == 2);
    CHECK(window.accept(fetches[0], Page<int>{.rows = std::vector<int>(10, 0), .total = 100, .offset = 0}));
    CHECK(window.accept(fetches[1], Page<int>{.rows = std::vector<int>(10, 1), .total = 100, .offset = 10}));
    CHECK(window.pagesHeld() == 1);
    REQUIRE(window.row(0) != nullptr);
    CHECK(*window.row(0) == 0);
    CHECK(window.row(10) == nullptr);
}

TEST_CASE("table: with no total known, a short reply waits for the rest and an empty one ends the page",
          "[table][query]") {
    PageWindow<int> window{10, 8};
    auto const first = window.visible(0, 5);
    REQUIRE(first.size() == 2);
    CHECK(window.accept(first[0], Page<int>{.rows = std::vector<int>(4, 7), .total = std::nullopt, .offset = 0}));
    CHECK_FALSE(window.total().has_value());
    auto const rest = window.visible(0, 5);
    REQUIRE(rest.size() == 1);
    CHECK(rest[0].request == PageRequest{.offset = 4, .limit = 6});
    CHECK(window.accept(rest[0], Page<int>{.rows = {}, .total = std::nullopt, .offset = 4}));
    CHECK(window.visible(0, 5).empty());  // page 0 is complete, page 1 still in flight
    CHECK(window.row(4) == nullptr);
}

namespace {

class ThrowingFold final : public TextCollator {
public:
    [[nodiscard]] std::string sortKey(std::string_view text) const override { return std::string{text}; }
    [[nodiscard]] std::string fold(std::string_view /*text*/) const override {
        throw std::runtime_error{"collator down"};
    }
};

}  // namespace

TEST_CASE("table: a collator that throws while the filter compiles is a ServiceFailed error from apply",
          "[table][query]") {
    ApplyOptions options;
    options.services.collator = std::make_shared<ThrowingFold const>();
    TableQuery query;
    query.filters = filterOn("analysis", FilterEntry{.contains = "pH"});
    auto const page = apply(resultRows(10), byId(), query, options);
    REQUIRE_FALSE(page.has_value());
    CHECK(page.error().code == TableErrorCode::ServiceFailed);
}

namespace {

TableQuery withFilter(std::string column, FilterEntry entry) {
    TableQuery query;
    query.filters.columns.emplace(std::move(column),
                                  ColumnFilter{.combine = Combine::Any, .include = {std::move(entry)}, .exclude = {}});
    return query;
}

}  // namespace

TEST_CASE("table: apply refuses what validate and compile refuse, by code", "[table][query]") {
    auto const source =
        tabletest::makeSource({tabletest::col("id", ColumnKind::Key), tabletest::col("n", ColumnKind::Integer)},
                              {{Cell{std::int64_t{1}}, Cell{std::int64_t{5}}}});
    auto const code = [&](TableQuery const& q) {
        auto r = apply(*source, q);
        REQUIRE_FALSE(r.has_value());
        return std::pair{r.error().code, r.error().column};
    };
    TableQuery far;
    far.page.offset = 10'000'001;
    CHECK(code(far).first == TableErrorCode::LimitExceeded);
    CHECK(code(withFilter("n", FilterEntry{.contains = "1"})) ==
          std::pair{TableErrorCode::UnsupportedOperator, std::string{"n"}});
    CHECK(code(withFilter("n", FilterEntry{.eq = "1", .ne = "2"})) ==
          std::pair{TableErrorCode::InvalidSpec, std::string{"n"}});
    TableQuery group;
    group.filters.groups.push_back(
        GroupFilter{.columns = {"n"}, .combine = Combine::Any, .include = {FilterEntry{.contains = "x"}}});
    CHECK(code(group) == std::pair{TableErrorCode::UnsupportedOperator, std::string{"n"}});
    // Mutation: apply skipping validate (the offset case answers an empty page).
}

TEST_CASE("table: a total is exact or a lower bound, and a lower bound does not end the window", "[table][query]") {
    Page<int> page{.rows = {1, 2}, .total = 100, .offset = 0, .totalKind = TotalKind::AtLeast};
    auto const json = glz::write_json(page);
    REQUIRE(json.has_value());
    Page<int> back;
    REQUIRE_FALSE(glz::read_json(back, *json));
    CHECK(back.totalKind == TotalKind::AtLeast);
    CHECK(back.total == 100);
    Page<int> without;
    REQUIRE_FALSE(glz::read_json(without, R"({"rows":[1],"total":1,"offset":0})"));
    CHECK(without.totalKind == TotalKind::Exact);

    PageWindow<int> bounded{10, 8};
    auto const first = bounded.visible(0, 5);
    REQUIRE_FALSE(first.empty());
    CHECK(bounded.accept(
        first[0],
        Page<int>{.rows = std::vector<int>(10, 1), .total = 10, .offset = 0, .totalKind = TotalKind::AtLeast}));
    CHECK(bounded.totalKind() == TotalKind::AtLeast);
    // Rows 10.. are fetched although the bound is 10: a lower bound says there are at least that many.
    auto const next = bounded.visible(10, 5);
    CHECK_FALSE(next.empty());
    bounded.reset();
    CHECK(bounded.totalKind() == TotalKind::Exact);

    PageWindow<int> exact{10, 8};
    auto const start = exact.visible(0, 5);
    REQUIRE_FALSE(start.empty());
    CHECK(exact.accept(start[0], Page<int>{.rows = std::vector<int>(10, 1), .total = 10, .offset = 0}));
    CHECK(exact.visible(10, 5).empty());
    // Mutation: PageWindow stopping at an AtLeast total.
}

TEST_CASE("table: a total kind goes over the wire by name", "[table][query]") {
    Page<int> page{.rows = {1}, .total = 5, .offset = 0, .totalKind = TotalKind::AtLeast};
    auto const json = glz::write_json(page);
    REQUIRE(json.has_value());
    CHECK(json->contains("\"totalKind\":\"atLeast\""));
    page.totalKind = TotalKind::Exact;
    CHECK(glz::write_json(page).value().contains("\"totalKind\":\"exact\""));
    Page<int> back;
    REQUIRE_FALSE(glz::read_json(back, R"({"rows":[],"total":3,"offset":0,"totalKind":"atLeast"})"));
    CHECK(back.totalKind == TotalKind::AtLeast);
    CHECK(glz::read_json(back, R"({"rows":[],"total":3,"offset":0,"totalKind":1})"));  // a number is refused
}

// Mutation: levelsNeeded returning every level of the columns.
// Mutation: levelsNeeded adding every column of a group with no include entry.
TEST_CASE("table: levelsNeeded names the levels a query sorts, filters and shows", "[table][query][levels]") {
    using tabletest::levelCol;
    std::vector<ColumnInfo> const columns{
        tabletest::col("id", ColumnKind::Integer), levelCol("r1", ColumnKind::Decimal, "recipe"),
        levelCol("r2", ColumnKind::Text, "recipe"), levelCol("s1", ColumnKind::Text, "stock"),
        levelCol("p1", ColumnKind::Integer, "price")};
    auto const needed = [&](TableQuery const& query) { return levelsNeeded(query, columns); };
    using V = std::vector<std::string>;
    TableQuery const named{.columns = V{"id", "r2"}};
    CHECK(needed(named).order.empty());
    CHECK(needed(named).display == V{"recipe"});
    TableQuery const sorted{.sort = {SortKey{.column = "s1"}}, .filters = FilterSpec{}, .columns = V{"id"}};
    CHECK(needed(sorted).order == V{"stock"});
    CHECK(needed(sorted).display.empty());
    TableQuery const filtered{.filters = FilterSpec{.columns = {{"p1", ColumnFilter{.include = {{.gt = "1"}}}}}},
                              .columns = V{"p1"}};
    CHECK(needed(filtered).order == V{"price"});
    CHECK(needed(filtered).display == V{"price"});
    TableQuery const grouped{
        .filters = FilterSpec{.groups = {GroupFilter{.columns = {"r2", "s1"}, .include = {{.contains = "a"}}}}},
        .columns = V{}};
    CHECK(needed(grouped).order == V{"recipe", "stock"});
    TableQuery const emptyGroup{.filters = FilterSpec{.groups = {GroupFilter{.columns = {"r1", "s1"}}}},
                                .columns = V{}};
    CHECK(needed(emptyGroup).order.empty());  // a group with no entry reads no column
    CHECK(needed(TableQuery{.columns = V{"id"}}).display.empty());
    CHECK(needed(TableQuery{}).display == V{"price", "recipe", "stock"});  // columns absent: every column
    CHECK(levelsNeeded(TableQuery{}, columns, [](std::string_view c) { return c != "p1"; }).display ==
          V{"recipe", "stock"});                                // only what the caller may read
    CHECK(needed(TableQuery{.columns = V{}}).display.empty());  // columns empty: keys alone
}

// Mutation: TableQuery's JSON leaving out `keys` (a glz::meta naming every member but `keys`).
TEST_CASE("table: columns and keys round-trip through glaze", "[table][query][keyed]") {
    TableQuery query;
    query.columns = std::vector<std::string>{"id", "name"};
    query.keys = std::vector<RowId>{RowId{std::int64_t{7}}, RowId{std::string{"A-1"}}};
    query.retry = true;
    std::string json;
    REQUIRE_FALSE(glz::write_json(query, json));
    TableQuery back;
    REQUIRE_FALSE(glz::read_json(back, json));
    CHECK(back.columns == query.columns);
    CHECK(back.keys == query.keys);
    CHECK(back.retry);
    TableQuery bare;
    REQUIRE_FALSE(glz::read_json(bare, R"({"sort":[],"filters":{},"page":{"offset":0,"limit":200}})"));
    CHECK_FALSE(bare.columns.has_value());
    CHECK_FALSE(bare.keys.has_value());
    CHECK_FALSE(bare.retry);
}

// Mutation: the keyed checks skipped.
TEST_CASE("table: validate refuses a keyed query out of bounds", "[table][query][keyed]") {
    std::vector<ColumnInfo> const columns{tabletest::col("id", ColumnKind::Integer)};
    auto const code = [&](TableQuery const& query) {
        auto const result = validate(query, columns);
        return result ? std::optional<TableErrorCode>{} : std::optional{result.error().code};
    };
    TableQuery offset;
    offset.keys = std::vector<RowId>{RowId{std::int64_t{1}}};
    CHECK_FALSE(code(offset).has_value());
    offset.page.offset = 10;
    CHECK(code(offset) == TableErrorCode::InvalidSpec);
    TableQuery many;
    many.keys = std::vector<RowId>(1'001, RowId{std::int64_t{1}});
    CHECK(code(many) == TableErrorCode::LimitExceeded);
    many.keys->resize(1'000);
    CHECK_FALSE(code(many).has_value());
    TableQuery longKey;
    longKey.keys = std::vector<RowId>{RowId{std::string(257, 'k')}};
    CHECK(code(longKey) == TableErrorCode::LimitExceeded);
    longKey.keys = std::vector<RowId>{RowId{std::string(256, 'k')}};
    CHECK_FALSE(code(longKey).has_value());
}

// Mutation: indices filled with positions in the reply instead of view rows.
TEST_CASE("table: a keyed reply carries the view's totals and each row's view row", "[table][query][keyed]") {
    using morph::table::Unloaded;
    auto const source =
        tabletest::makeSource({tabletest::levelCol("a", ColumnKind::Integer, "slow")}, {{Cell{std::int64_t{40}}},
                                                                                        {Cell{std::int64_t{10}}},
                                                                                        {Cell{Unloaded{}}},
                                                                                        {Cell{std::int64_t{30}}},
                                                                                        {Cell{std::int64_t{-1}}},
                                                                                        {Cell{std::int64_t{20}}}});
    TableQuery query{.sort = {SortKey{.column = "a"}},
                     .filters = FilterSpec{.columns = {{"a", ColumnFilter{.include = {{.gt = "0"}}}}}}};
    query.keys = std::vector<RowId>{RowId{std::int64_t{3}}, RowId{std::int64_t{1}}, RowId{std::int64_t{2}},
                                    RowId{std::int64_t{4}}, RowId{std::int64_t{99}}};
    auto page = apply(*source, query);
    REQUIRE(page.has_value());
    CHECK(page->rows == std::vector<std::size_t>{1, 3});  // view order; 2 undecided, 4 rejected, 99 absent
    CHECK(page->indices == std::vector<std::int64_t>{0, 2});
    CHECK(page->total == 4);
    CHECK(page->undecided == 1);
    CHECK(page->totalKind == TotalKind::AtLeast);
}

// Mutation: the typed apply not copying indices.
TEST_CASE("table: a typed keyed reply carries each row's view row", "[table][query][keyed]") {
    auto const rows = resultRows(10);
    TableQuery query{.sort = {SortKey{.column = "id", .dir = SortDirection::Descending}}};
    query.keys = std::vector<RowId>{RowId{std::int64_t{2}}, RowId{std::int64_t{8}}, RowId{std::int64_t{42}}};
    auto const page = apply(rows, byId(), query);
    REQUIRE(page.has_value());
    REQUIRE(page->rows.size() == 2);
    CHECK(page->rows[0].id == 8);
    CHECK(page->rows[1].id == 2);
    CHECK(page->indices == std::vector<std::int64_t>{1, 7});
    CHECK(page->offset == 0);
    CHECK(page->total == 10);
}

namespace {

// A snapshot that counts the row ids read through it.
class CountingSnapshot final : public RowSnapshot {
public:
    explicit CountingSnapshot(std::shared_ptr<RowSnapshot const> base) : _base{std::move(base)} {}
    [[nodiscard]] std::size_t rowCount() const override { return _base->rowCount(); }
    [[nodiscard]] RowId rowId(std::size_t row) const override {
        ++ids;
        return _base->rowId(row);
    }
    [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override { return _base->cell(row, column); }
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): `ids` counts the row-id reads the case asserts on, and the read is const
    mutable std::size_t ids = 0;

private:
    std::shared_ptr<RowSnapshot const> _base;
};

}  // namespace

// Mutation: pageRows ignoring sourceRowOf.
TEST_CASE("table: a keyed page uses the lookup it is given", "[table][query][keyed]") {
    using morph::table::Unloaded;
    auto const source =
        tabletest::makeSource({tabletest::levelCol("a", ColumnKind::Integer, "slow")}, {{Cell{std::int64_t{40}}},
                                                                                        {Cell{std::int64_t{10}}},
                                                                                        {Cell{Unloaded{}}},
                                                                                        {Cell{std::int64_t{30}}},
                                                                                        {Cell{std::int64_t{-1}}},
                                                                                        {Cell{std::int64_t{20}}}});
    TableQuery query{.sort = {SortKey{.column = "a"}},
                     .filters = FilterSpec{.columns = {{"a", ColumnFilter{.include = {{.gt = "0"}}}}}}};
    query.keys = std::vector<RowId>{RowId{std::int64_t{3}}, RowId{std::int64_t{1}}, RowId{std::int64_t{2}},
                                    RowId{std::int64_t{4}}, RowId{std::int64_t{99}}};
    auto const validated = validate(query, source->columns());
    REQUIRE(validated.has_value());
    auto const view = detail::computeView(*source, *validated, ApplyOptions{});
    REQUIRE(view.has_value());
    CountingSnapshot const snapshot{source->snapshot()};

    auto const scanned = detail::pageRows(*view, snapshot, *validated, QueryLimits{});
    CHECK(scanned.rows == std::vector<std::size_t>{1, 3});
    CHECK(scanned.indices == std::vector<std::int64_t>{0, 2});
    CHECK(snapshot.ids > 0);  // the scan reads every row's id once

    std::unordered_map<RowId, std::uint32_t> byKey;
    for (std::uint32_t row = 0; row < source->snapshot()->rowCount(); ++row) {
        byKey.emplace(source->snapshot()->rowId(row), row);
    }
    detail::SourceRowOf const lookup = [&byKey](RowId const& key) -> std::optional<std::uint32_t> {
        auto const found = byKey.find(key);
        return found == byKey.end() ? std::nullopt : std::optional{found->second};
    };
    snapshot.ids = 0;
    auto const looked = detail::pageRows(*view, snapshot, *validated, QueryLimits{}, {}, lookup);
    CHECK(looked.rows == scanned.rows);
    CHECK(looked.indices == scanned.indices);
    CHECK(looked.total == scanned.total);
    CHECK(looked.undecided == scanned.undecided);
    CHECK(snapshot.ids == 0);
}

// Mutation: levelsNeeded adding a group's column on which none of its entries compiles.
TEST_CASE("table: levelsNeeded leaves out a group's column on which no entry compiles", "[table][query][levels]") {
    using tabletest::levelCol;
    std::vector<ColumnInfo> const columns{tabletest::col("id", ColumnKind::Integer),
                                          levelCol("r1", ColumnKind::Decimal, "recipe"),
                                          levelCol("s1", ColumnKind::Text, "stock")};
    using V = std::vector<std::string>;
    // `contains` is a text operator: it compiles on s1 and not on the decimal r1, so the group reads s1 alone.
    TableQuery const search{
        .filters = FilterSpec{.groups = {GroupFilter{.columns = {"r1", "s1"}, .include = {{.contains = "a"}}}}},
        .columns = V{}};
    CHECK(levelsNeeded(search, columns).order == V{"stock"});
    // `gt "5"` parses on the decimal and is no text operator: the group reads r1 alone.
    TableQuery const above{
        .filters = FilterSpec{.groups = {GroupFilter{.columns = {"r1", "s1"}, .include = {{.gt = "5"}}}}},
        .columns = V{}};
    CHECK(levelsNeeded(above, columns).order == V{"recipe"});
}

// Mutation: writing an integer key through double (9'007'199'254'740'993 comes back one less).
TEST_CASE("table: change keys round-trip awkward keys", "[table][query][levels]") {
    std::vector<RowId> const awkward{RowId{std::string{"a\"b\\c \xC3\xA9"}},
                                     RowId{std::int64_t{9'007'199'254'740'993}}};
    auto const written = changeKeys(awkward);
    REQUIRE(written.size() == 2);
    CHECK(rowIdOf(written[0]) == std::optional<RowId>{awkward[0]});
    CHECK(rowIdOf(written[1]) == std::optional<RowId>{awkward[1]});
    for (std::string const other : {"true", "1.5", "{}"}) {
        CHECK_FALSE(rowIdOf(glz::raw_json{other}).has_value());
    }
    std::vector<RowId> const plain{RowId{std::int64_t{7}}, RowId{std::string{"x"}}};
    auto const simple = changeKeys(plain);
    REQUIRE(simple.size() == 2);
    CHECK(simple[0].str == "7");
    CHECK(simple[1].str == "\"x\"");
}

namespace {

// The error code of a refused query, or nothing when it was accepted.
std::optional<TableErrorCode> codeOf(std::expected<TableQuery, TableError> const& result) {
    return result ? std::nullopt : std::optional{result.error().code};
}

// `n` distinct texts of exactly `bytes` bytes each.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `texts(n, bytes)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::vector<std::string> texts(std::size_t n, std::size_t bytes) {
    std::vector<std::string> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto const text = std::to_string(i);
        out.push_back(std::string(bytes - text.size(), '0') + text);
    }
    return out;
}

// A query with one `oneOf` include entry on each named column.
TableQuery setQuery(std::vector<std::pair<std::string, std::vector<std::string>>> sets) {
    TableQuery q;
    for (auto& [column, values] : sets) {
        q.filters.columns[column] =
            ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.oneOf = std::move(values)}}, .exclude = {}};
    }
    return q;
}

std::vector<ColumnInfo> threeTextColumns() {
    return {tabletest::col("a", ColumnKind::Text), tabletest::col("b", ColumnKind::Text),
            tabletest::col("c", ColumnKind::Text)};
}

}  // namespace

// Mutation: the per-query sum of set values not checked.
TEST_CASE("table query: value sets are bounded per entry and per query", "[table][query]") {
    auto const columns = threeTextColumns();
    CHECK(validate(setQuery({{"a", texts(10'000, 16)}}), columns).has_value());
    CHECK(codeOf(validate(setQuery({{"a", texts(10'001, 16)}}), columns)) == TableErrorCode::LimitExceeded);
    auto const overEntry = validate(setQuery({{"a", texts(10'001, 16)}}), columns);
    REQUIRE_FALSE(overEntry.has_value());
    CHECK(overEntry.error().column == "a");
    CHECK(validate(setQuery({{"a", texts(10'000, 16)}, {"b", texts(10'000, 16)}}), columns).has_value());
    CHECK(codeOf(validate(setQuery({{"a", texts(10'000, 16)}, {"b", texts(10'000, 16)}, {"c", texts(1, 16)}}),
                          columns)) == TableErrorCode::LimitExceeded);
    // 1,080,000 bytes, over maxQuerySetBytes
    CHECK(codeOf(validate(setQuery({{"a", texts(9'000, 120)}}), columns)) == TableErrorCode::LimitExceeded);
    // over maxTextLength
    CHECK(codeOf(validate(setQuery({{"a", texts(1, 257)}}), columns)) == TableErrorCode::LimitExceeded);
}

// Gate D6. Mutation: one set's bound one below maxSetValues, so 10,000 values are refused.
TEST_CASE("table query: a 10,000-value set fits the wire bound", "[table][query]") {
    auto const columns = threeTextColumns();
    auto const q = setQuery({{"a", texts(10'000, 16)}});
    std::string json;
    REQUIRE_FALSE(glz::write_json(q, json));
    CHECK(json.size() <= 250'000);
    CHECK(json.size() < QueryLimits{}.maxQuerySetBytes);
    CHECK(validate(q, columns).has_value());
}
