// SPDX-License-Identifier: Apache-2.0
//
// morph::table filter specs (spec 7 §6).
//
// Mutations these tests were seen to fail on:
//  - CompiledFilter::matches letting a non-empty operator through on an
//    invalid cell (drop the `state != Valid` early return): "a numeric filter
//    never matches an invalid cell" fails.
//  - CompiledFilter::passes ignoring `combine` (always `any`): "include
//    entries combine by any and all" fails.
//  - CompiledFilter::passes ignoring `exclude`: "an exclude entry rejects the
//    row" fails.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/table/filter.hpp>
#include <numeric>
#include <ranges>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::dec;

namespace {

using Rows = std::vector<std::uint32_t>;

struct Filtered {
    std::shared_ptr<VectorSource> source;
    Services services;

    // The rows that pass `spec`, in source order; an error when it does not compile.
    std::expected<Rows, TableError> by(FilterSpec const& spec) const {
        auto const compiled = compileFilter(spec, source->columns(), services);
        if (!compiled) {
            return std::unexpected(compiled.error());
        }
        auto const snapshot = source->snapshot();
        auto const columns = source->columns();
        std::vector<KeyColumn> keys;
        keys.reserve(columns.size());
        std::vector<KeyColumn const*> pointers;
        for (std::size_t c = 0; c < columns.size(); ++c) {
            keys.push_back(KeyColumn::forColumn(columns[c], services, snapshot->rowCount()));
            buildKeys(*snapshot, c, columns[c], services, 0, snapshot->rowCount(), keys.back());
        }
        pointers.reserve(keys.size());
        for (auto const& key : keys) {
            pointers.push_back(&key);
        }
        Rows rows(snapshot->rowCount());
        std::ranges::iota(rows, 0U);
        FilterRun run{*compiled, pointers, rows};
        static_cast<void>(run.run(kNoDeadline, ::core::async::StopToken{}));
        return std::move(run).take();
    }
};

FilterEntry eq(std::string v) { return FilterEntry{.eq = std::move(v)}; }
FilterEntry ne(std::string v) { return FilterEntry{.ne = std::move(v)}; }
FilterEntry lt(std::string v) { return FilterEntry{.lt = std::move(v)}; }
FilterEntry le(std::string v) { return FilterEntry{.le = std::move(v)}; }
FilterEntry gt(std::string v) { return FilterEntry{.gt = std::move(v)}; }
FilterEntry ge(std::string v) { return FilterEntry{.ge = std::move(v)}; }
FilterEntry between(std::string a, std::string b) {
    return FilterEntry{.between = std::array<std::string, 2>{std::move(a), std::move(b)}};
}
FilterEntry contains(std::string v) { return FilterEntry{.contains = std::move(v)}; }
FilterEntry startsWith(std::string v) { return FilterEntry{.startsWith = std::move(v)}; }
FilterEntry endsWith(std::string v) { return FilterEntry{.endsWith = std::move(v)}; }
FilterEntry isEmpty() { return FilterEntry{.isEmpty = true}; }
FilterEntry notEmpty() { return FilterEntry{.notEmpty = true}; }

FilterSpec on(std::string column, std::vector<FilterEntry> include, Combine combine = Combine::Any,
              std::vector<FilterEntry> exclude = {}) {
    FilterSpec spec;
    spec.columns.emplace(
        std::move(column),
        ColumnFilter{.combine = combine, .include = std::move(include), .exclude = std::move(exclude)});
    return spec;
}

}  // namespace

TEST_CASE("table: numeric operators compare exactly", "[table][filter]") {
    // 0: 1.5, 1: 2, 2: invalid, 3: empty, 4: 0.1
    Filtered const f{.source = tabletest::column(ColumnKind::Decimal,
                                                 {dec("1.5"), std::int64_t{2}, std::string{"x"}, Cell{}, dec("0.1")}),
                     .services = {}};
    CHECK(f.by(on("c", {eq("1.50")})) == Rows{0});
    CHECK(f.by(on("c", {ne("1.5")})) == Rows{1, 4});
    CHECK(f.by(on("c", {lt("1.5")})) == Rows{4});
    CHECK(f.by(on("c", {le("1.5")})) == Rows{0, 4});
    CHECK(f.by(on("c", {gt("1.5")})) == Rows{1});
    CHECK(f.by(on("c", {ge("1.5")})) == Rows{0, 1});
    CHECK(f.by(on("c", {between("0.1", "1.5")})) == Rows{0, 4});
    CHECK(f.by(on("c", {isEmpty()})) == Rows{3});
    CHECK(f.by(on("c", {notEmpty()})) == Rows{0, 1, 2, 4});
}

TEST_CASE("table: an integer column compares against exact decimal values", "[table][filter]") {
    Filtered const f{
        .source = tabletest::column(ColumnKind::Integer, {std::int64_t{1}, std::int64_t{2}, std::int64_t{3}}),
        .services = {}};
    CHECK(f.by(on("c", {ge("1.5")})) == Rows{1, 2});
    CHECK(f.by(on("c", {eq("2.0")})) == Rows{1});
}

TEST_CASE("table: a numeric filter never matches an invalid cell", "[table][filter]") {
    Filtered const f{.source = tabletest::column(ColumnKind::Integer, {std::string{"n/a"}, std::int64_t{5}, Cell{}}),
                     .services = {}};
    CHECK(f.by(on("c", {ne("7")})) == Rows{1});
    CHECK(f.by(on("c", {lt("100")})) == Rows{1});
    CHECK(f.by(on("c", {ge("-100")})) == Rows{1});
}

TEST_CASE("table: Number, date, date-time and boolean operators", "[table][filter]") {
    auto const source =
        tabletest::makeSource({tabletest::col("x", ColumnKind::Number), tabletest::col("d", ColumnKind::Date),
                               tabletest::col("t", ColumnKind::DateTime), tabletest::col("b", ColumnKind::Bool)},
                              {{0.5, std::string{"2026-01-01"}, std::string{"2026-01-01T12:00:00Z"}, true},
                               {2.5, std::string{"2026-06-30"}, std::string{"2026-01-02T00:00:00Z"}, false},
                               {Cell{}, std::string{"bad"}, Cell{}, Cell{}}});
    Filtered const f{.source = source, .services = {}};
    CHECK(f.by(on("x", {gt("1")})) == Rows{1});
    CHECK(f.by(on("x", {between("0", "0.5")})) == Rows{0});
    CHECK(f.by(on("d", {ge("2026-02-01")})) == Rows{1});
    CHECK(f.by(on("d", {ne("2026-01-01")})) == Rows{1});
    CHECK(f.by(on("t", {lt("2026-01-02")})) == Rows{0});
    CHECK(f.by(on("t", {ge("2026-01-01T18:00:00Z")})) == Rows{1});
    CHECK(f.by(on("b", {eq("true")})) == Rows{0});
    CHECK(f.by(on("b", {ne("true")})) == Rows{1});
    CHECK(f.by(on("b", {isEmpty()})) == Rows{2});
}

TEST_CASE("table: text operators ignore case and accents through the collator", "[table][filter]") {
    auto const cells = std::vector<Cell>{std::string{"pH value"}, std::string{"Phosphate"},
                                         std::string{"\xC3\xA9tage"}, std::string{"100% pure"}, Cell{}};
    Filtered const byDefault{.source = tabletest::column(ColumnKind::Text, cells), .services = {}};
    CHECK(byDefault.by(on("c", {contains("PH")})) == Rows{0, 1});
    CHECK(byDefault.by(on("c", {startsWith("ph")})) == Rows{0, 1});
    CHECK(byDefault.by(on("c", {endsWith("VALUE")})) == Rows{0});
    CHECK(byDefault.by(on("c", {eq("PHOSPHATE")})) == Rows{1});
    CHECK(byDefault.by(on("c", {ne("phosphate")})) == Rows{0, 2, 3});
    // The default collator does not fold accents.
    CHECK(byDefault.by(on("c", {startsWith("eta")})) == Rows{});
    // `%` and `_` are ordinary characters.
    CHECK(byDefault.by(on("c", {contains("0%")})) == Rows{3});
    CHECK(byDefault.by(on("c", {contains("_")})) == Rows{});

    Services accentFolding;
    accentFolding.collator = std::make_shared<tabletest::AccentFoldingCollator>();
    Filtered const folded{.source = tabletest::column(ColumnKind::Text, cells), .services = accentFolding};
    CHECK(folded.by(on("c", {startsWith("ETA")})) == Rows{2});

    auto sensitive = contains("pH");
    sensitive.caseSensitive = true;
    CHECK(byDefault.by(on("c", {sensitive})) == Rows{0});
}

TEST_CASE("table: include entries combine by any and all", "[table][filter]") {
    Filtered const f{.source = tabletest::column(
                         ColumnKind::Integer, {std::int64_t{1}, std::int64_t{5}, std::int64_t{10}, std::int64_t{15}}),
                     .services = {}};
    CHECK(f.by(on("c", {lt("3"), gt("12")})) == Rows{0, 3});
    CHECK(f.by(on("c", {ge("5"), lt("12")}, Combine::All)) == Rows{1, 2});
}

TEST_CASE("table: an exclude entry rejects the row", "[table][filter]") {
    Filtered const f{.source = tabletest::column(ColumnKind::Text, {std::string{"open"}, std::string{"hold"},
                                                                    std::string{"void"}, std::string{"done"}}),
                     .services = {}};
    CHECK(f.by(on("c", {eq("open"), eq("hold"), eq("void")}, Combine::Any, {eq("void")})) == Rows{0, 1});
    CHECK(f.by(on("c", {}, Combine::Any, {eq("void")})) == Rows{0, 1, 3});
}

TEST_CASE("table: columns combine by AND, and a group matches on any of its columns", "[table][filter]") {
    auto const source = tabletest::makeSource(
        {tabletest::col("analysis", ColumnKind::Text), tabletest::col("analyst", ColumnKind::Text),
         tabletest::col("mass", ColumnKind::Decimal)},
        {{std::string{"pH"}, std::string{"Anna"}, dec("2")},
         {std::string{"Lead"}, std::string{"Phil"}, dec("20")},
         {std::string{"Iron"}, std::string{"Bob"}, dec("3")},
         {std::string{"pH"}, std::string{"Bob"}, dec("30")}});
    Filtered const f{.source = source, .services = {}};
    auto spec = on("mass", {lt("10")});
    CHECK(f.by(spec) == Rows{0, 2});
    spec.groups.push_back(
        GroupFilter{.columns = {"analysis", "analyst", "mass"}, .combine = {}, .include = {contains("ph")}});
    // "ph" applies to the two text columns only; mass < 10 still applies.
    CHECK(f.by(spec) == Rows{0});
    spec.columns.clear();
    CHECK(f.by(spec) == Rows{0, 1, 3});
}

TEST_CASE("table: a value that does not parse is an error naming its column", "[table][filter]") {
    Filtered const f{.source = tabletest::makeSource(
                         {tabletest::col("mass", ColumnKind::Decimal), tabletest::col("on", ColumnKind::Date),
                          tabletest::col("name", ColumnKind::Text), tabletest::col("c", ColumnKind::Custom, "x")},
                         {}),
                     .services = {}};
    auto const badDecimal = f.by(on("mass", {gt("1,5")}));
    REQUIRE_FALSE(badDecimal.has_value());
    CHECK(badDecimal.error().code == TableErrorCode::InvalidValue);
    CHECK(badDecimal.error().column == "mass");
    CHECK(f.by(on("on", {lt("yesterday")})).error().code == TableErrorCode::InvalidValue);
    CHECK(f.by(on("name", {lt("m")})).error().code == TableErrorCode::UnsupportedOperator);
    CHECK(f.by(on("mass", {contains("1")})).error().code == TableErrorCode::UnsupportedOperator);
    CHECK(f.by(on("c", {eq("1")})).error().code == TableErrorCode::UnsupportedOperator);
    CHECK(f.by(on("nope", {eq("1")})).error().code == TableErrorCode::UnknownColumn);
    CHECK(f.by(on("name", {FilterEntry{}})).error().code == TableErrorCode::InvalidSpec);
    CHECK(f.by(on("name", {FilterEntry{.eq = "a", .ne = "b"}})).error().code == TableErrorCode::InvalidSpec);
    FilterSpec group;
    group.groups.push_back(GroupFilter{.columns = {"mass", "on"}, .combine = {}, .include = {contains("x")}});
    CHECK(f.by(group).error().code == TableErrorCode::UnsupportedOperator);
}

TEST_CASE("table: the spec's FilterSpec JSON reads and compiles", "[table][filter]") {
    constexpr std::string_view json = R"({ "columns": {
    "analysis": { "include": [ { "contains": "pH" } ] },
    "mass":     { "combine": "all", "include": [ { "ge": "1.5" }, { "lt": "10" } ] },
    "status":   { "include": [ { "eq": "open" }, { "eq": "hold" } ], "exclude": [ { "eq": "void" } ] } },
  "groups": [ { "columns": ["analysis", "analyst"], "include": [ { "contains": "ph" } ] } ] })";
    FilterSpec spec;
    auto const error = glz::read_json(spec, json);
    REQUIRE_FALSE(error);
    CHECK(spec.columns.at("mass").combine == Combine::All);
    CHECK(spec.columns.at("status").exclude.size() == 1);

    auto const source = tabletest::makeSource(
        {tabletest::col("analysis", ColumnKind::Text), tabletest::col("analyst", ColumnKind::Text),
         tabletest::col("mass", ColumnKind::Quantity), tabletest::col("status", ColumnKind::Text)},
        {{std::string{"pH"}, std::string{"Ann"}, dec("2"), std::string{"open"}},
         {std::string{"pH"}, std::string{"Ann"}, dec("12"), std::string{"open"}},
         {std::string{"pH"}, std::string{"Ann"}, dec("2"), std::string{"void"}},
         {std::string{"Fe"}, std::string{"Ann"}, dec("2"), std::string{"hold"}}});
    Filtered const f{.source = source, .services = {}};
    CHECK(f.by(spec) == Rows{0});

    std::string written;
    REQUIRE_FALSE(glz::write_json(spec, written));
    FilterSpec again;
    REQUIRE_FALSE(glz::read_json(again, written));
    CHECK(f.by(again) == Rows{0});
}

TEST_CASE("table: a filter run yields at a deadline and keeps candidate order", "[table][filter]") {
    std::vector<Cell> cells;
    cells.reserve(10000);
    for (std::int64_t i = 0; i < 10000; ++i) {
        cells.emplace_back(i);
    }
    auto const source = tabletest::column(ColumnKind::Integer, cells);
    Services const services;
    auto const compiled = compileFilter(on("c", {FilterEntry{.ge = "5000"}}), source->columns(), services);
    REQUIRE(compiled.has_value());
    auto keys = KeyColumn::forColumn(source->columns()[0], services, cells.size());
    buildKeys(*source->snapshot(), 0, source->columns()[0], services, 0, cells.size(), keys);
    Rows reversed(cells.size());
    std::ranges::iota(std::views::reverse(reversed), 0U);
    FilterRun run{*compiled, {&keys}, reversed};
    std::size_t steps = 0;
    while (!run.run(std::chrono::steady_clock::now(), ::core::async::StopToken{})) {
        ++steps;
    }
    CHECK(steps > 1);
    auto const rows = std::move(run).take();
    REQUIRE(rows.size() == 5000);
    CHECK(rows.front() == 9999U);
    CHECK(rows.back() == 5000U);
}
