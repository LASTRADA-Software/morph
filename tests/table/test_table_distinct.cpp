// SPDX-License-Identifier: Apache-2.0
//
// morph::table distinct values: what a header's filter menu lists (docs/spec/table/table.md, "Filters from the
// header", "Unique values"; invariant 22).
//
// Mutations these tests were seen to fail on:
//  - counting over the view (the column's own filter kept): "counts are those of the rows the other filters leave"
//    fails.
//  - grouping text by raw text instead of its fold: "counts are those of the rows the other filters leave" fails.
//  - an invalid cell counted as a blank: "invalid cells are counted and listed under no value" fails.
//  - distinctOver counting an Unknown row as a candidate: "an unloaded cell or an undecided row sets partial" fails.
//  - `distinct` not capped at totalCap: "search, paging and the cap" fails.
//  - the search text not folded: "search on numbers and accents, and an offset past the end" fails.
//  - a month bucket ending on its first day: "dates by year, month and day" fails.
//  - a query setting both keys and distinct accepted: "requests are validated as queries are" fails.
//  - levelsNeeded ignoring query.distinct: "the column's level is an order level of the request" fails.
//  - apply ignoring query.distinct: "server mode answers through apply, client mode through the engine" fails.
//  - Engine::distinctValues passing no keys: "the engine reuses the keys it holds" fails on the fold count.
//  - empty cells not counted in blanks: "invariant 22 over random tables" fails.
//  - distinctOver without the base-level check: "an unloaded cell in a base-level column breaks the contract" fails.
//  - the typed apply ignoring query.distinct: "the typed apply answers a distinct request" fails.
//  - the request's group columns not passed through checkColumn: "a group's values are pooled" fails.
//  - levelsNeeded leaving a group's columns out: "a group's levels are order levels of the request" fails.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;

namespace {

FilterEntry eq(std::string v) { return FilterEntry{.eq = std::move(v)}; }
FilterEntry oneOf(std::vector<std::string> v) { return FilterEntry{.oneOf = std::move(v)}; }

// The first case's query: city oneOf Berlin, Paris; status eq closed.
TableQuery cityAndStatus() {
    TableQuery q;
    q.filters.columns["city"] =
        ColumnFilter{.combine = Combine::Any, .include = {oneOf({"Berlin", "Paris"})}, .exclude = {}};
    q.filters.columns["status"] = ColumnFilter{.combine = Combine::Any, .include = {eq("closed")}, .exclude = {}};
    return q;
}

std::vector<std::string> names(std::expected<DistinctValues, TableError> const& values) {
    std::vector<std::string> out;
    if (values) {
        for (auto const& value : values->values) {
            out.push_back(value.text);
        }
    }
    return out;
}

// The error code of a refused call, or nothing when it succeeded.
template <typename T>
std::optional<TableErrorCode> codeOf(std::expected<T, TableError> const& result) {
    return result ? std::nullopt : std::optional{result.error().code};
}

std::int64_t epochDays(std::chrono::year_month_day date) {
    return std::chrono::sys_days{date}.time_since_epoch().count();
}

// A collator that counts its folds; shared by every task, so the count is the call's.
class CountingCollator final : public TextCollator {
public:
    [[nodiscard]] std::string sortKey(std::string_view text) const override { return _inner.sortKey(text); }
    [[nodiscard]] std::string fold(std::string_view text) const override {
        ++folds;
        return _inner.fold(text);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): `folds` counts the calls the case asserts on, and the fold is const
    mutable std::atomic<std::int64_t> folds{0};

private:
    CodePointCollator _inner;
};

}  // namespace

TEST_CASE("distinct: counts are those of the rows the other filters leave", "[table][distinct]") {
    auto const source = tabletest::distinctSource();
    auto const q = cityAndStatus();
    auto const d = distinctValues(*source, q, DistinctRequest{.column = "status"});
    REQUIRE(d);
    REQUIRE(d->values.size() == 2);
    CHECK(d->values[0].text == "closed");
    CHECK(d->values[0].operand == "closed");
    CHECK(d->values[0].count == 1);
    CHECK(d->values[1].text == "open");
    CHECK(d->values[1].operand == "open");
    CHECK(d->values[1].count == 3);
    CHECK(d->distinct == 2);
    CHECK(d->distinctKind == TotalKind::Exact);
    CHECK(d->blanks == 1);
    CHECK(d->invalid == 0);
    CHECK_FALSE(d->partial);
    // invariant 22: the counts, blanks and invalid cells sum to the rows the other filters leave
    std::int64_t sum = d->blanks + d->invalid;
    for (auto const& value : d->values) {
        sum += value.count;
    }
    CHECK(sum == 5);
}

TEST_CASE("distinct: invalid cells are counted and listed under no value", "[table][distinct]") {
    auto const source = tabletest::distinctSource();
    TableQuery q;
    q.filters.columns["city"] = ColumnFilter{.combine = Combine::Any, .include = {eq("Paris")}, .exclude = {}};
    auto const d = distinctValues(*source, q, DistinctRequest{.column = "qty"});
    REQUIRE(d);
    REQUIRE(d->values.size() == 1);
    CHECK(d->values[0].text == "2");
    CHECK(d->values[0].operand == "2");
    CHECK(d->values[0].count == 1);
    CHECK(d->invalid == 1);
}

TEST_CASE("distinct: an unloaded cell or an undecided row sets partial", "[table][distinct]") {
    // Under the first case's query, which drops row 5, so the values are closed, open.
    auto const q = cityAndStatus();
    auto const unloadedStatus = tabletest::distinctSource({"status"}, {{0, 0}});
    auto const withUnloadedStatus = distinctValues(*unloadedStatus, q, DistinctRequest{.column = "status"});
    REQUIRE(withUnloadedStatus);
    REQUIRE(withUnloadedStatus->values.size() == 2);
    CHECK(withUnloadedStatus->partial);
    CHECK(withUnloadedStatus->values[1].count == 2);  // "open": rows 1, 4
    auto const unloadedCity = tabletest::distinctSource({"city"}, {{1, 1}});
    auto const withUndecidedRow = distinctValues(*unloadedCity, q, DistinctRequest{.column = "status"});
    REQUIRE(withUndecidedRow);
    REQUIRE(withUndecidedRow->values.size() == 2);
    CHECK(withUndecidedRow->partial);
    CHECK(withUndecidedRow->values[1].count == 2);  // row 1 undecided by city
}

TEST_CASE("distinct: search, paging and the cap", "[table][distinct]") {
    auto const source = tabletest::distinctSource();
    CHECK(names(distinctValues(*source, {}, {.column = "status", .search = "OP"})) ==
          std::vector<std::string>{"open"});
    auto const page = distinctValues(*source, {}, {.column = "status", .offset = 1, .limit = 1});
    REQUIRE(page);
    CHECK(names(page) == std::vector<std::string>{"hold"});  // ascending: closed, hold, open
    CHECK(page->distinct == 3);
    ApplyOptions capped;
    capped.limits.totalCap = 2;
    auto const atCap = distinctValues(*source, {}, {.column = "status"}, capped);
    REQUIRE(atCap);
    CHECK(atCap->distinct == 2);
    CHECK(atCap->distinctKind == TotalKind::AtLeast);
    CHECK(atCap->values.size() == 3);
    ApplyOptions small;
    small.limits.maxLimit = 2;
    auto const clamped = distinctValues(*source, {}, {.column = "status", .limit = 500}, small);
    REQUIRE(clamped);
    CHECK(clamped->values.size() == 2);
}

TEST_CASE("distinct: search on numbers and accents, and an offset past the end", "[table][distinct]") {
    auto const decimals =
        tabletest::column(ColumnKind::Decimal, {tabletest::dec("11.5"), tabletest::dec("1.5"), std::int64_t{2}});
    CHECK(names(distinctValues(*decimals, {}, {.column = "c", .search = "1.5"})) ==
          std::vector<std::string>{"1.5", "11.5"});
    auto const texts = tabletest::column(ColumnKind::Text, {std::string{"\xC3\xA9"
                                                                        "cole"},
                                                            std::string{"Ecole"}, std::string{"x"}});
    ApplyOptions foldingOptions;
    foldingOptions.services.collator = std::make_shared<tabletest::AccentFoldingCollator>();
    CHECK(names(distinctValues(*texts, {}, {.column = "c", .search = "\xC3\x89"}, foldingOptions)).size() == 1);
    auto const past = distinctValues(*decimals, {}, {.column = "c", .offset = 10});
    REQUIRE(past);
    CHECK(past->values.empty());
    CHECK(past->distinct == 3);
}

TEST_CASE("distinct: dates by year, month and day", "[table][distinct]") {
    using namespace std::chrono;
    auto const dates = tabletest::column(ColumnKind::Date,
                                         {epochDays(2025y / December / 31), epochDays(2026y / January / 15),
                                          epochDays(2026y / January / 20), epochDays(2026y / February / 1)},
                                         "d");
    auto const years = distinctValues(*dates, {}, {.column = "d", .bucket = DateBucket::Year});
    REQUIRE(years);
    REQUIRE(years->values.size() == 2);
    CHECK(years->values[0].text == "2025");
    CHECK(years->values[0].operand == "2025-01-01..2025-12-31");
    CHECK(years->values[0].count == 1);
    CHECK(years->values[1].text == "2026");
    CHECK(years->values[1].operand == "2026-01-01..2026-12-31");
    CHECK(years->values[1].count == 3);
    auto const months = distinctValues(*dates, {}, {.column = "d", .bucket = DateBucket::Month});
    REQUIRE(months);
    CHECK(names(months) == std::vector<std::string>{"2025-12", "2026-01", "2026-02"});
    CHECK(months->values[1].operand == "2026-01-01..2026-01-31");
    CHECK(months->values[1].count == 2);
    auto const byDay = distinctValues(*dates, {}, {.column = "d", .bucket = DateBucket::Day});
    REQUIRE(byDay);
    REQUIRE(byDay->values.size() == 4);
    CHECK(byDay->values[1].text == "2026-01-15");
    CHECK(byDay->values[1].operand == "2026-01-15..2026-01-15");
    auto const values = distinctValues(*dates, {}, {.column = "d"});
    REQUIRE(values);
    CHECK(values->values[0].text == "2025-12-31");

    auto const stamps =
        tabletest::column(ColumnKind::DateTime, {(epochDays(2026y / January / 15) * 86'400L) + (10 * 3600L)}, "t");
    auto const stampDays = distinctValues(*stamps, {}, {.column = "t", .bucket = DateBucket::Day});
    REQUIRE(stampDays);
    REQUIRE(stampDays->values.size() == 1);
    CHECK(stampDays->values[0].text == "2026-01-15");
    CHECK(stampDays->values[0].operand == "2026-01-15T00:00:00Z..2026-01-15T23:59:59Z");
    auto const stampValues = distinctValues(*stamps, {}, {.column = "t"});
    REQUIRE(stampValues);
    CHECK(stampValues->values[0].text == "2026-01-15T10:00:00Z");

    auto const source = tabletest::distinctSource();
    CHECK(codeOf(distinctValues(*source, {}, {.column = "status", .bucket = DateBucket::Year})) ==
          TableErrorCode::InvalidSpec);
}

TEST_CASE("distinct: requests are validated as queries are", "[table][distinct]") {
    auto const source = tabletest::distinctSource();
    CHECK(codeOf(distinctValues(*source, {}, {.column = "status", .group = "g"})) == TableErrorCode::InvalidSpec);
    CHECK(codeOf(distinctValues(*source, {}, {})) == TableErrorCode::InvalidSpec);
    CHECK(codeOf(distinctValues(*source, {}, {.column = "nope"})) == TableErrorCode::UnknownColumn);
    ApplyOptions hidden;
    hidden.mayRead = [](std::string_view id) { return id != "status"; };
    CHECK(codeOf(distinctValues(*source, {}, {.column = "status"}, hidden)) == TableErrorCode::ColumnNotReadable);
    CHECK(codeOf(distinctValues(*source, {}, {.column = "status", .search = std::string(257, 'a')})) ==
          TableErrorCode::LimitExceeded);
    CHECK(codeOf(distinctValues(*source, {}, {.column = "status", .offset = 10'000'001})) ==
          TableErrorCode::LimitExceeded);
    CHECK(codeOf(distinctValues(*source, {}, {.column = "status", .offset = -1})) == TableErrorCode::LimitExceeded);
    TableQuery keyed;
    keyed.keys = std::vector<RowId>{std::int64_t{1}};
    keyed.distinct = DistinctRequest{.column = "status"};
    CHECK(codeOf(validate(keyed, source->columns())) == TableErrorCode::InvalidSpec);
}

TEST_CASE("distinct: the column's level is an order level of the request", "[table][distinct]") {
    auto const source = tabletest::distinctSource({"status"});
    TableQuery q;
    q.distinct = DistinctRequest{.column = "status"};
    q.columns = std::vector<std::string>{"city"};
    auto const needed = levelsNeeded(q, source->columns(), {});
    CHECK(needed.order == std::vector<std::string>{"slow"});
}

TEST_CASE("distinct: server mode answers through apply, client mode through the engine", "[table][distinct]") {
    auto const source = tabletest::distinctSource();
    auto q = cityAndStatus();
    q.distinct = DistinctRequest{.column = "status"};
    auto const page = apply(*source, q);
    REQUIRE(page);
    CHECK(page->rows.empty());
    REQUIRE(page->distinct);
    CHECK(*page->distinct == *distinctValues(*source, q, *q.distinct));
    Engine engine{source};
    REQUIRE(engine.setFilter(q.filters));
    CHECK(*engine.distinctValues(*q.distinct) == *distinctValues(*source, q, *q.distinct));
}

TEST_CASE("distinct: the engine reuses the keys it holds", "[table][distinct]") {
    auto const source = tabletest::distinctSource();
    auto const collator = std::make_shared<CountingCollator>();
    Engine engine{source, EngineOptions{.services = Services{.collator = collator}}};
    REQUIRE(engine.setSort({SortKey{.column = "status"}}));
    REQUIRE(engine.stats().keyBuilds == 1);  // a sanity check of the setup; the fold count below is the test
    auto const foldsBefore = collator->folds.load();
    auto const values = engine.distinctValues({.column = "status"});
    REQUIRE(values);
    CHECK(engine.stats().keyBuilds == 1);
    CHECK(collator->folds.load() == foldsBefore);  // the engine's status keys, not a second build
    ApplyOptions options;
    options.services.collator = collator;
    CHECK(*values == *distinctValues(*source, {}, {.column = "status"}, options));
}

TEST_CASE("distinct: invariant 22 over random tables", "[table][distinct]") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 22 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 rng{22};
    auto const pick = [&rng](int bound) { return static_cast<int>(rng() % static_cast<unsigned>(bound)); };
    static constexpr std::array<char const*, 6> kWords{"a", "B", "b", "c", "D", "e"};
    for (int table = 0; table < 100; ++table) {
        std::vector<std::vector<Cell>> rows;
        for (int row = 0; row < 300; ++row) {
            auto const text = [&]() -> Cell {
                auto const roll = pick(8);
                return roll < 6 ? Cell{std::string{kWords.at(static_cast<std::size_t>(roll))}} : Cell{};
            };
            auto const number = [&]() -> Cell {
                auto const roll = pick(10);
                if (roll == 8) {
                    return Cell{};
                }
                return roll == 9 ? Cell{std::string{"x"}} : Cell{std::int64_t{roll}};
            };
            rows.push_back({text(), number(), text()});
        }
        auto const source =
            tabletest::makeSource({tabletest::col("c0", ColumnKind::Text), tabletest::col("c1", ColumnKind::Integer),
                                   tabletest::col("c2", ColumnKind::Text)},
                                  std::move(rows));
        TableQuery query;
        query.page.limit = 0;
        query.filters.columns["c0"] = ColumnFilter{.combine = Combine::Any,
                                                   .include = {oneOf({kWords.at(static_cast<std::size_t>(pick(6)))})},
                                                   .exclude = {}};
        query.filters.columns["c1"] = ColumnFilter{
            .combine = Combine::Any, .include = {FilterEntry{.ge = std::to_string(pick(8))}}, .exclude = {}};
        if (pick(2) == 0) {
            query.filters.columns["c2"] = ColumnFilter{
                .combine = Combine::Any, .include = {}, .exclude = {eq(kWords.at(static_cast<std::size_t>(pick(6))))}};
        }
        auto const d = distinctValues(*source, query, {.column = "c0", .limit = 1000});
        REQUIRE(d);
        REQUIRE_FALSE(d->partial);
        std::int64_t sumOfCounts = 0;
        for (auto const& value : d->values) {
            sumOfCounts += value.count;
        }
        auto withoutC0 = query;
        withoutC0.filters.columns.erase("c0");
        auto const page = apply(*source, withoutC0);
        REQUIRE(page);
        REQUIRE(page->total);
        CHECK(sumOfCounts + d->blanks + d->invalid == *page->total);
    }
}

namespace {

// The groups truth-table source plus row 5: empty, F, unloaded; declared group "company" over companyA..C.
std::shared_ptr<VectorSource> pooledSource(std::vector<std::string> const& slow = {}) {
    using S = std::string;
    std::vector<std::vector<Cell>> rows{{S{"F"}, S{"S"}, S{"X"}}, {S{"F"}, S{"F"}, S{"X"}},
                                        {S{"S"}, S{"F"}, S{"F"}}, {S{"F"}, S{"X"}, S{"Y"}},
                                        {S{"X"}, S{"Y"}, S{"Z"}}, {Cell{}, S{"F"}, Cell{Unloaded{}}}};
    std::vector<ColumnInfo> columns;
    for (auto const* const id : {"companyA", "companyB", "companyC"}) {
        // companyC holds the unloaded cell, so it is at a level whatever `slow` says.
        bool const atLevel = std::ranges::find(slow, id) != slow.end() || std::string_view{id} == "companyC";
        columns.push_back(tabletest::levelCol(id, ColumnKind::Text, atLevel ? "slow" : ""));
    }
    auto source = tabletest::makeSource(std::move(columns), std::move(rows));
    source->setGroups(
        {ColumnGroupInfo{.id = "company", .label = "Company", .columns = {"companyA", "companyB", "companyC"}}});
    return source;
}

std::int64_t countOf(std::expected<DistinctValues, TableError> const& values, std::string_view text) {
    if (!values) {
        return -1;
    }
    auto const found = std::ranges::find(values->values, text, &DistinctValue::text);
    return found == values->values.end() ? 0 : found->count;
}

}  // namespace

// Mutation: the request's group columns not passed through detail::checkColumn: the list comes back with
// companyB's values in it.
TEST_CASE("distinct: a group's values are pooled", "[table][distinct][groups]") {
    auto const source = pooledSource();
    auto const d = distinctValues(*source, {}, DistinctRequest{.group = "company"});
    REQUIRE(d);
    CHECK(countOf(d, "F") == 5);  // rows 0, 1, 2, 3, 5: once a row
    CHECK(countOf(d, "S") == 2);  // rows 0, 2
    CHECK(d->blanks == 1);        // row 5: the rows an isEmpty entry under any admits
    CHECK(d->partial);            // row 5's unloaded cell
    TableQuery own;               // the group's own filter is removed, others stay
    own.filters.groups.push_back({.group = "company", .include = {eq("Z")}});
    CHECK(countOf(distinctValues(*source, own, {.group = "company"}), "F") == 5);
    CHECK(codeOf(distinctValues(*source, {}, {.group = "nope"})) == TableErrorCode::UnknownColumn);
    // A values list is a read of every column of the group.
    ApplyOptions hiddenB;
    hiddenB.mayRead = [](std::string_view id) { return id != "companyB"; };
    auto const refused = distinctValues(*source, {}, {.group = "company"}, hiddenB);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == TableErrorCode::ColumnNotReadable);
    CHECK(refused.error().column == "companyB");
    TableQuery viaQuery;
    viaQuery.distinct = DistinctRequest{.group = "company"};
    CHECK(codeOf(validate(viaQuery, source->columns(), {}, hiddenB.mayRead, source->groups())) ==
          TableErrorCode::ColumnNotReadable);
}

// Mutation: levelsNeeded leaving the group's columns out of `order`.
TEST_CASE("distinct: a group's levels are order levels of the request", "[table][distinct][groups]") {
    auto const source = pooledSource({"companyB"});
    // Only companyB at a level here, so the expected levels are its alone.
    std::vector<ColumnInfo> columns(source->columns().begin(), source->columns().end());
    columns[2].level.clear();
    auto const levelled = tabletest::makeSource(columns, {{std::string{"a"}, std::string{"b"}, std::string{"c"}}});
    levelled->setGroups(std::vector<ColumnGroupInfo>(source->groups().begin(), source->groups().end()));
    TableQuery q;
    q.distinct = DistinctRequest{.group = "company"};
    q.columns = std::vector<std::string>{};
    auto const needed = levelsNeeded(q, levelled->columns(), {}, {}, levelled->groups());
    CHECK(needed.order == std::vector<std::string>{"slow"});
}

TEST_CASE("distinct: an unloaded cell in a base-level column breaks the contract", "[table][distinct]") {
    // status at the base level, with an unloaded cell: no level would ever load it.
    auto const source = tabletest::distinctSource();
    source->updateRow(0, {Cell{Unloaded{}}, Cell{std::string{"Berlin"}}, Cell{std::int64_t{1}}});
    auto const listed = distinctValues(*source, {}, {.column = "status"});
    REQUIRE_FALSE(listed);
    CHECK(listed.error().code == TableErrorCode::SourceContract);
    CHECK(listed.error().column == "status");
    // The same through a filter on it, as apply refuses it.
    TableQuery q;
    q.filters.columns["status"] = ColumnFilter{.combine = Combine::Any, .include = {eq("open")}, .exclude = {}};
    CHECK(codeOf(distinctValues(*source, q, {.column = "city"})) == TableErrorCode::SourceContract);
    CHECK(codeOf(apply(*source, q)) == TableErrorCode::SourceContract);
}

namespace tdistinct {

// A typed row for the typed apply's distinct case. Reflected by glaze.
struct StatusRow {
    std::int64_t id = 0;
    std::string status;
};

}  // namespace tdistinct

TEST_CASE("distinct: the typed apply answers a distinct request", "[table][distinct]") {
    std::vector<tdistinct::StatusRow> const rows{
        {.id = 1, .status = "open"}, {.id = 2, .status = "Open"}, {.id = 3, .status = "closed"}};
    RowKey<tdistinct::StatusRow> const key = [](tdistinct::StatusRow const& row) { return RowId{row.id}; };
    TableQuery q;
    q.distinct = DistinctRequest{.column = "status"};
    auto const page = apply(rows, key, q);
    REQUIRE(page);
    CHECK(page->rows.empty());
    REQUIRE(page->distinct);
    std::shared_ptr<std::vector<tdistinct::StatusRow> const> const shared{std::shared_ptr<void>{}, &rows};
    RowsSource<tdistinct::StatusRow> const source{shared, key};
    CHECK(*page->distinct == *distinctValues(source, q, *q.distinct));
    REQUIRE(page->distinct->values.size() == 2);
    CHECK(page->distinct->values[1].count == 2);  // "open", "Open"
}
