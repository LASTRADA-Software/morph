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
//  - `ValueSet` matching nothing: "oneOf matches what its eq entries match"
//    fails.
//  - `Together` evaluated as `atLeast` with n = entries: "groups: the spec's
//    truth table" fails on row F, F, X.
//  - `Filtered::by` compiling without `source->groups()`: every declared-group
//    case fails with `UnknownColumn`.
//  The oneOf and group cases name their own mutations above them.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <utility>
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
    [[nodiscard]] std::expected<Rows, TableError> by(FilterSpec const& spec) const {
        auto const compiled = compileFilter(spec, source->columns(), services, source->groups());
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
        // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
        std::iota(rows.begin(), rows.end(), 0U);
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
FilterEntry oneOf(std::vector<std::string> v) { return FilterEntry{.oneOf = std::move(v)}; }

// The rows of a filter that must compile: a refused one fails the case here rather than being dereferenced.
Rows must(std::expected<Rows, TableError> const& result) {
    REQUIRE(result.has_value());
    return *result;
}

std::vector<FilterEntry> eqEntries(std::vector<std::string> const& values) {
    std::vector<FilterEntry> out;
    for (auto const& value : values) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.push_back(eq(value));
    }
    return out;
}

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

// Mutation: parseDecimal clamping INT64_MIN to -INT64_MAX: `eq` the most
// negative integer never matches the cell that holds it.
TEST_CASE("table: an integer filter matches the most negative integer", "[table][filter]") {
    Filtered const f{.source = tabletest::column(ColumnKind::Integer, {std::numeric_limits<std::int64_t>::min(),
                                                                       std::numeric_limits<std::int64_t>::max()}),
                     .services = {}};
    CHECK(f.by(on("c", {eq("-9223372036854775808")})) == Rows{0});
    CHECK(f.by(on("c", {lt("-9223372036854775807")})) == Rows{0});
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
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
    std::iota(reversed.rbegin(), reversed.rend(), 0U);
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

TEST_CASE("table: Key, Number and Bool columns parse their operands", "[table][filter]") {
    auto const source = tabletest::makeSource(
        {tabletest::col("id", ColumnKind::Key), tabletest::col("x", ColumnKind::Number),
         tabletest::col("ok", ColumnKind::Bool)},
        {{std::int64_t{10}, 1.5, true}, {std::int64_t{20}, 2.5, false}, {std::int64_t{30}, 3.5, false}});
    Filtered const f{.source = source, .services = {}};
    CHECK(f.by(on("id", {ge("20")})) == Rows{1, 2});
    CHECK(f.by(on("ok", {eq("false")})) == Rows{1, 2});
    CHECK(f.by(on("ok", {ne("false")})) == Rows{0});
    CHECK(f.by(on("x", {gt("abc")})).error().code == TableErrorCode::InvalidValue);
    CHECK(f.by(on("ok", {eq("yes")})).error().code == TableErrorCode::InvalidValue);
    CHECK(f.by(on("ok", {gt("true")})).error().code == TableErrorCode::UnsupportedOperator);
}

TEST_CASE("table: an exclude entry that does not parse is an error", "[table][filter]") {
    auto const source = tabletest::column(ColumnKind::Decimal, {dec("1")}, "mass");
    Filtered const f{.source = source, .services = {}};
    auto const bad = f.by(on("mass", {}, Combine::Any, {gt("1,5")}));
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == TableErrorCode::InvalidValue);
    CHECK(bad.error().column == "mass");
}

TEST_CASE("table: a group refuses an unknown column and a malformed entry", "[table][filter]") {
    auto const source = tabletest::column(ColumnKind::Text, {std::string{"a"}}, "name");
    Filtered const f{.source = source, .services = {}};
    FilterSpec unknown;
    unknown.groups.push_back(GroupFilter{.columns = {"name", "nope"}, .combine = {}, .include = {contains("a")}});
    auto const missing = f.by(unknown);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == TableErrorCode::UnknownColumn);
    CHECK(missing.error().column == "nope");
    FilterSpec malformed;
    malformed.groups.push_back(GroupFilter{.columns = {"name"}, .combine = {}, .include = {FilterEntry{}}});
    CHECK(f.by(malformed).error().code == TableErrorCode::InvalidSpec);
}

TEST_CASE("table: an all-group uses only the columns that evaluate every entry", "[table][filter]") {
    auto const source = tabletest::makeSource(
        {tabletest::col("analysis", ColumnKind::Text), tabletest::col("analyst", ColumnKind::Text),
         tabletest::col("mass", ColumnKind::Decimal)},
        {{std::string{"pH"}, std::string{"Anna"}, dec("2")},
         {std::string{"Lead"}, std::string{"Phil"}, dec("20")},
         {std::string{"Iron"}, std::string{"Bob"}, dec("3")},
         {std::string{"pH"}, std::string{"Bob"}, dec("30")}});
    Filtered const f{.source = source, .services = {}};
    FilterSpec spec;
    spec.groups.push_back(GroupFilter{.columns = {"analysis", "analyst", "mass"},
                                      .combine = Combine::All,
                                      .include = {startsWith("p"), endsWith("h")}});
    // A column matches when it meets both entries; "Phil" starts with p but does not end with h.
    CHECK(f.by(spec) == Rows{0, 3});
    spec.groups[0].combine = Combine::Any;
    CHECK(f.by(spec) == Rows{0, 1, 3});
}

TEST_CASE("table: filters with no entries compile to nothing and pass every row", "[table][filter]") {
    auto const source = tabletest::column(ColumnKind::Integer, {std::int64_t{1}, std::int64_t{2}});
    Filtered const f{.source = source, .services = {}};
    FilterSpec spec = on("c", {});
    spec.groups.push_back(GroupFilter{.columns = {"c"}, .combine = {}, .include = {}});
    CHECK_FALSE(spec.empty());
    auto const compiled = compileFilter(spec, source->columns(), {});
    REQUIRE(compiled.has_value());
    CHECK(compiled->columns().empty());
    CHECK(compiled->groups().empty());
    CHECK(f.by(spec) == Rows{0, 1});

    FilterSpec groupOnly;
    groupOnly.groups.push_back(GroupFilter{});
    CHECK_FALSE(groupOnly.empty());
    CHECK(FilterSpec{}.empty());
}

TEST_CASE("table: a filter run yields between stages at a passed deadline", "[table][filter]") {
    auto const source = tabletest::column(ColumnKind::Integer, {std::int64_t{1}, std::int64_t{5}, std::int64_t{9}});
    Services const services;
    auto spec = on("c", {gt("2")});
    spec.groups.push_back(GroupFilter{.columns = {"c"}, .combine = {}, .include = {lt("7")}});
    auto const compiled = compileFilter(spec, source->columns(), services);
    REQUIRE(compiled.has_value());
    auto keys = KeyColumn::forColumn(source->columns()[0], services, 3);
    buildKeys(*source->snapshot(), 0, source->columns()[0], services, 0, 3, keys);
    FilterRun run{*compiled, {&keys}, Rows{0, 1, 2}};
    auto const passed = std::chrono::steady_clock::now() - std::chrono::seconds{1};
    CHECK_FALSE(run.run(passed, ::core::async::StopToken{}));  // the column stage ran; the group waits
    CHECK(run.run(kNoDeadline, ::core::async::StopToken{}));
    auto const rows = std::move(run).take();
    CHECK(rows == Rows{1});
}

// Mutation: evaluate returning passes (two-valued), which rejects the unloaded row.
TEST_CASE("table: an unloaded cell is unknown to every operator", "[table][filter][unloaded]") {
    using morph::table::Unloaded;
    struct Case {
        ColumnKind kind;
        FilterEntry entry;
    };
    std::vector<Case> const cases{
        {.kind = ColumnKind::Integer, .entry = {.eq = "1"}},
        {.kind = ColumnKind::Integer, .entry = {.ne = "1"}},
        {.kind = ColumnKind::Integer, .entry = {.lt = "1"}},
        {.kind = ColumnKind::Integer, .entry = {.le = "1"}},
        {.kind = ColumnKind::Integer, .entry = {.gt = "1"}},
        {.kind = ColumnKind::Integer, .entry = {.ge = "1"}},
        {.kind = ColumnKind::Decimal, .entry = {.between = std::array<std::string, 2>{"1", "2"}}},
        {.kind = ColumnKind::Text, .entry = {.contains = "a"}},
        {.kind = ColumnKind::Text, .entry = {.startsWith = "a"}},
        {.kind = ColumnKind::Text, .entry = {.endsWith = "a"}},
        {.kind = ColumnKind::Date, .entry = {.eq = "2026-01-01"}},
        {.kind = ColumnKind::Bool, .entry = {.eq = "true"}},
        {.kind = ColumnKind::Text, .entry = {.isEmpty = true}},
        {.kind = ColumnKind::Text, .entry = {.notEmpty = true}}};
    for (auto const& [kind, entry] : cases) {
        for (bool const exclude : {false, true}) {
            for (auto const combine : {Combine::Any, Combine::All}) {
                auto const source = tabletest::slowColumn(kind, {Cell{Unloaded{}}});
                ColumnFilter filter{.combine = combine};
                (exclude ? filter.exclude : filter.include).push_back(entry);
                auto page = apply(*source, TableQuery{.filters = FilterSpec{.columns = {{"c", filter}}}});
                REQUIRE(page.has_value());
                CHECK(page->rows.empty());
                CHECK(page->undecided == 1);
                CHECK(page->total == 0);
                CHECK(page->totalKind == TotalKind::AtLeast);
            }
        }
    }
}

// Mutation: a group evaluated as `all` of its columns (Yes only when every column is Yes).
TEST_CASE("table: a group admits on one loaded match and is unknown otherwise", "[table][filter][unloaded]") {
    using morph::table::Unloaded;
    auto const text = [](char const* value) { return Cell{std::string{value}}; };
    auto const source = tabletest::makeSource(
        {tabletest::levelCol("a", ColumnKind::Text, "slow"), tabletest::levelCol("b", ColumnKind::Text, "slow")},
        {{text("x"), Cell{Unloaded{}}},
         {Cell{Unloaded{}}, text("y")},
         {text("y"), text("z")},
         {Cell{Unloaded{}}, Cell{Unloaded{}}}});
    FilterSpec spec;
    spec.groups.push_back(GroupFilter{.columns = {"a", "b"}, .combine = Combine::Any, .include = {contains("x")}});
    Engine engine{source};
    REQUIRE(engine.setFilter(spec));
    CHECK(tabletest::viewKeys(engine) == std::vector<RowId>{0});
    CHECK(Rows(engine.undecidedRows().begin(), engine.undecidedRows().end()) == Rows{1, 3});
    auto const page = apply(*source, TableQuery{.filters = spec});
    REQUIRE(page.has_value());
    CHECK(page->rows == std::vector<std::size_t>{0});
    CHECK(page->undecided == 2);
    CHECK(page->totalKind == TotalKind::AtLeast);
}

// Mutation: FilterRun keeping a marked row undecided when a later stage answers No.
TEST_CASE("table: a row unknown at one stage and rejected at a later one is rejected", "[table][filter][unloaded]") {
    using morph::table::Unloaded;
    auto const source = tabletest::makeSource(
        {tabletest::levelCol("a", ColumnKind::Integer, "slow"), tabletest::col("b", ColumnKind::Integer)},
        {{Cell{Unloaded{}}, Cell{std::int64_t{3}}}});
    FilterSpec spec = on("a", {eq("1")});
    spec.columns.emplace("b", ColumnFilter{.combine = Combine::Any, .include = {eq("2")}, .exclude = {}});
    auto const page = apply(*source, TableQuery{.filters = spec});
    REQUIRE(page.has_value());
    CHECK(page->rows.empty());
    CHECK(page->undecided == 0);
    CHECK(page->totalKind == TotalKind::Exact);
}

namespace {

// A random cell of a column of the soundness property: integers 0..5, short texts, booleans, a tenth of them empty.
Cell randomValue(ColumnKind kind, std::mt19937& rng) {
    static constexpr std::array<char const*, 4> kTexts{"a", "ab", "b", "ba"};
    if (rng() % 10 == 0) {
        return Cell{};
    }
    switch (kind) {
        case ColumnKind::Integer:
            return Cell{static_cast<std::int64_t>(rng() % 6)};
        case ColumnKind::Bool:
            return Cell{rng() % 2 == 0};
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
        case ColumnKind::Number:
        case ColumnKind::Text:
        case ColumnKind::Date:
        case ColumnKind::DateTime:
        case ColumnKind::Key:
        case ColumnKind::Custom:
        case ColumnKind::Measure:
        default:
            return Cell{std::string{kTexts.at(rng() % kTexts.size())}};
    }
}

// A random entry that `kind` defines, from eq, ne, lt, gt, contains, isEmpty and notEmpty.
FilterEntry randomEntry(ColumnKind kind, std::mt19937& rng) {
    std::string operand;
    if (kind == ColumnKind::Integer) {
        operand = std::to_string(rng() % 6);
    } else if (kind == ColumnKind::Bool) {
        operand = rng() % 2 == 0 ? "true" : "false";
    } else {
        operand = rng() % 2 == 0 ? "a" : "b";
    }
    std::vector<FilterEntry> choices{eq(operand), ne(operand), isEmpty(), notEmpty()};
    if (kind == ColumnKind::Integer) {
        choices.push_back(lt(operand));
        choices.push_back(gt(operand));
    } else if (kind == ColumnKind::Text) {
        choices.push_back(contains(operand));
    }
    return choices.at(rng() % choices.size());
}

}  // namespace

// Mutation: evaluate answering No for `ne` on an unloaded cell.
TEST_CASE("table: the filter is sound under every completion", "[table][filter][unloaded]") {
    using morph::table::Unloaded;
    constexpr std::uint32_t kRows = 30;
    std::array<ColumnKind, 3> const kinds{ColumnKind::Integer, ColumnKind::Text, ColumnKind::Bool};
    std::array<std::string, 3> const ids{"i", "t", "b"};
    std::vector<ColumnInfo> columns;
    for (std::size_t c = 0; c < kinds.size(); ++c) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `columns` is a fixture list built once; sizing it first would not change what the case checks
        columns.push_back(tabletest::levelCol(ids.at(c), kinds.at(c), "slow"));
    }
    // The view, sorted, and the undecided rows of `rows` under `spec`, as source rows.
    auto const run = [&](std::vector<std::vector<Cell>> rows, FilterSpec const& spec) {
        Engine engine{tabletest::makeSource(columns, std::move(rows))};
        REQUIRE(engine.setFilter(spec));
        Rows view(engine.view().begin(), engine.view().end());
        Rows const undecided(engine.undecidedRows().begin(), engine.undecidedRows().end());
        std::ranges::sort(view);
        return std::pair{view, undecided};
    };
    for (std::uint32_t seed = 0; seed < 200; ++seed) {
        std::mt19937 rng{seed};
        std::vector<std::vector<Cell>> rows(kRows);
        for (auto& row : rows) {
            for (auto const kind : kinds) {
                row.push_back(rng() % 4 == 0 ? Cell{Unloaded{}} : randomValue(kind, rng));
            }
        }
        FilterSpec spec;
        auto const filtered = 1 + (rng() % 3);
        for (std::size_t i = 0; i < filtered; ++i) {
            auto const c = rng() % kinds.size();
            auto& filter = spec.columns[ids.at(c)];
            (rng() % 2 == 0 ? filter.include : filter.exclude).push_back(randomEntry(kinds.at(c), rng));
        }
        if (rng() % 2 == 0) {
            auto const first = rng() % kinds.size();
            GroupFilter group{.columns = {ids.at(first)}, .combine = Combine::Any, .include = {}};
            group.include.push_back(randomEntry(kinds.at(first), rng));
            for (std::size_t c = 0; c < kinds.size(); ++c) {
                if (c != first && rng() % 2 == 0) {
                    group.columns.push_back(ids.at(c));
                }
            }
            spec.groups.push_back(std::move(group));
        }
        auto const [view, undecided] = run(rows, spec);
        REQUIRE(view.size() + undecided.size() <= kRows);
        for (int completion = 0; completion < 20; ++completion) {
            auto completed = rows;
            for (auto& row : completed) {
                for (std::size_t c = 0; c < kinds.size(); ++c) {
                    if (std::holds_alternative<Unloaded>(row.at(c))) {
                        row.at(c) = randomValue(kinds.at(c), rng);
                    }
                }
            }
            auto const [after, none] = run(std::move(completed), spec);
            REQUIRE(none.empty());
            for (std::uint32_t row = 0; row < kRows; ++row) {
                bool const shown = std::ranges::binary_search(view, row);
                bool const open = std::ranges::find(undecided, row) != undecided.end();
                bool const shownAfter = std::ranges::binary_search(after, row);
                if (shown) {
                    REQUIRE(shownAfter);
                } else if (!open) {
                    REQUIRE_FALSE(shownAfter);
                }
            }
        }
    }
}

// Mutation: `ValueSet::contains` answering false.
TEST_CASE("table: oneOf matches what its eq entries match", "[table][filter]") {
    Filtered const text{
        .source = tabletest::column(ColumnKind::Text, {std::string{"Alpha"}, std::string{"beta"}, std::string{"Gamma"},
                                                       Cell{}, std::string{"alpha"}}),
        .services = {}};
    CHECK(text.by(on("c", {oneOf({"alpha", "BETA"})})) == Rows{0, 1, 4});
    CHECK(must(text.by(on("c", {oneOf({"alpha", "BETA"})}))) == must(text.by(on("c", {eq("alpha"), eq("BETA")}))));
    CHECK(text.by(on("c", {}, Combine::Any, {oneOf({"alpha", "BETA"})})) == Rows{2, 3});
    CHECK(text.by(on("c", {oneOf({"alpha", "BETA"}), isEmpty()})) == Rows{0, 1, 3, 4});
    FilterEntry exact = oneOf({"alpha"});
    exact.caseSensitive = true;
    CHECK(text.by(on("c", {exact})) == Rows{4});

    Filtered const ints{.source = tabletest::column(ColumnKind::Integer, {std::int64_t{1}, std::int64_t{2},
                                                                          std::int64_t{3}, std::string{"x"}}),
                        .services = {}};
    CHECK(ints.by(on("c", {oneOf({"2", "1.5"})})) == Rows{1});
    CHECK(ints.by(on("c", {oneOf({"2", "zz"})})).error().code == TableErrorCode::InvalidValue);
}

// Mutation: a lone oneOf entry answering No, not Unknown, on an unloaded cell.
TEST_CASE("table: oneOf is unknown on an unloaded cell", "[table][filter]") {
    auto const source = tabletest::slowColumn(
        ColumnKind::Text, {std::string{"Alpha"}, Cell{morph::table::Unloaded{}}, std::string{"b"}});
    auto const page = apply(*source, TableQuery{.sort = {}, .filters = on("c", {oneOf({"alpha"})}), .page = {}});
    REQUIRE(page);
    CHECK(page->rows == std::vector<std::size_t>{0});
    CHECK(page->undecided == 1);
    CHECK(page->totalKind == TotalKind::AtLeast);
}

// Mutation: an empty set matching every valid cell.
TEST_CASE("table: an empty or repeating oneOf", "[table][filter]") {
    Filtered const text{
        .source = tabletest::column(ColumnKind::Text, {std::string{"Alpha"}, std::string{"beta"}, Cell{}}),
        .services = {}};
    CHECK(text.by(on("c", {oneOf({})})) == Rows{});
    CHECK(text.by(on("c", {}, Combine::Any, {oneOf({})})) == Rows{0, 1, 2});
    CHECK(must(text.by(on("c", {oneOf({"alpha", "ALPHA"})}))) == must(text.by(on("c", {oneOf({"alpha"})}))));
}

// Gate D5: one lookup a cell, whatever the set's size. The count is kept by the set, so a set that scanned its
// members but counted one probe would pass; D4's timing is what guards against that. Mutation: a text set scanned
// member by member, each comparison counted as a lookup.
TEST_CASE("table: a oneOf of any size is one lookup a cell", "[table][filter]") {
    constexpr std::uint32_t kRows = 100'000;
    std::vector<Cell> cells;
    cells.reserve(kRows);
    for (std::uint32_t row = 0; row < kRows; ++row) {
        cells.emplace_back("v" + std::to_string(row % 20'000));
    }
    auto const source = tabletest::column(ColumnKind::Text, std::move(cells));
    std::vector<std::string> members;
    for (int value = 0; value < 10'000; ++value) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `members` is a fixture list built once; sizing it first would not change what the case checks
        members.push_back("v" + std::to_string(value));
    }
    Services const services;
    auto const compiled = compileFilter(on("c", {oneOf(members)}), source->columns(), services);
    REQUIRE(compiled);
    auto const snapshot = source->snapshot();
    auto keys = KeyColumn::forColumn(source->columns()[0], services, kRows);
    buildKeys(*snapshot, 0, source->columns()[0], services, 0, kRows, keys);
    FilterCounters counters;
    std::size_t passed = 0;
    for (std::uint32_t row = 0; row < kRows; ++row) {
        passed += CompiledFilter::evaluate(compiled->columns()[0], keys, row, &counters) == Match::Yes ? 1U : 0U;
    }
    CHECK(counters.setLookups == 100'000);
    CHECK(passed == 50'000);
}

// Invariant 23. Mutation: text members kept as given rather than folded.
TEST_CASE("table: oneOf is the OR of its eq entries over random tables", "[table][filter]") {
    struct Kind {
        ColumnKind kind;
        std::vector<Cell> cells;            // the values a cell is drawn from
        std::vector<std::string> operands;  // the same values as filter text
    };
    std::vector<Kind> const kinds{
        {.kind = ColumnKind::Text,
         .cells = {std::string{"a"}, std::string{"B"}, std::string{"b"}, std::string{"c"}, std::string{"D"},
                   std::string{"e"}, std::string{"F"}, std::string{"g"}},
         .operands = {"a", "B", "b", "c", "D", "e", "F", "g"}},
        {.kind = ColumnKind::Integer,
         .cells = {std::int64_t{1}, std::int64_t{2}, std::int64_t{3}, std::int64_t{4}, std::int64_t{5},
                   std::int64_t{6}, std::int64_t{7}, std::int64_t{8}},
         .operands = {"1", "2", "3", "4", "5", "6.0", "7", "8"}},
        {.kind = ColumnKind::Decimal,
         .cells = {dec("0.5"), dec("1"), dec("1.25"), dec("2"), dec("2.5"), dec("3"), dec("3.75"), dec("4")},
         .operands = {"0.5", "1.0", "1.25", "2", "2.50", "3", "3.75", "4"}},
        {.kind = ColumnKind::Date,
         .cells = {std::string{"2026-01-01"}, std::string{"2026-01-02"}, std::string{"2026-01-03"},
                   std::string{"2026-01-04"}, std::string{"2026-01-05"}, std::string{"2026-01-06"},
                   std::string{"2026-01-07"}, std::string{"2026-01-08"}},
         .operands = {"2026-01-01", "2026-01-02", "2026-01-03", "2026-01-04", "2026-01-05", "2026-01-06", "2026-01-07",
                      "2026-01-08"}},
        {.kind = ColumnKind::Bool, .cells = {true, false}, .operands = {"true", "false"}},
    };
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 23 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 rng{23};
    for (int table = 0; table < 200; ++table) {
        auto const& kind = kinds.at(static_cast<std::size_t>(table) % kinds.size());
        std::vector<Cell> cells;
        for (int row = 0; row < 50; ++row) {
            auto const roll = rng() % (kind.cells.size() + 2);
            if (roll < kind.cells.size()) {
                cells.push_back(kind.cells.at(roll));
            } else if (roll == kind.cells.size()) {
                cells.emplace_back();
            } else {
                cells.emplace_back(std::string{"not a value"});
            }
        }
        Filtered const f{.source = tabletest::column(kind.kind, std::move(cells)), .services = {}};
        std::vector<std::string> values;
        auto const count = 1 + (rng() % 6);
        for (std::size_t i = 0; i < count; ++i) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `values` is a fixture list built once; sizing it first would not change what the case checks
            values.push_back(kind.operands.at(rng() % kind.operands.size()));
        }
        REQUIRE(f.by(on("c", {oneOf(values)})));
        CHECK(must(f.by(on("c", {oneOf(values)}))) == must(f.by(on("c", eqEntries(values)))));
        CHECK(must(f.by(on("c", {}, Combine::Any, {oneOf(values)}))) ==
              must(f.by(on("c", {}, Combine::Any, eqEntries(values)))));
    }
}

// ── Column groups ───────────────────────────────────────────────────────────

namespace {

using morph::table::Unloaded;

// companyA, companyB, companyC (Text), declared group {"company", "Company", {companyA, companyB, companyC}}.
// 0: F, S, X   1: F, F, X   2: S, F, F   3: F, X, Y   4: X, Y, Z
std::shared_ptr<VectorSource> companySource(std::vector<std::vector<Cell>> rows, bool slow = false) {
    std::vector<ColumnInfo> columns;
    for (auto const* const id : {"companyA", "companyB", "companyC"}) {
        columns.push_back(slow ? tabletest::levelCol(id, ColumnKind::Text, "slow")
                               : tabletest::col(id, ColumnKind::Text));
    }
    auto source = tabletest::makeSource(std::move(columns), std::move(rows));
    source->setGroups(
        {ColumnGroupInfo{.id = "company", .label = "Company", .columns = {"companyA", "companyB", "companyC"}}});
    return source;
}

std::vector<std::vector<Cell>> truthTableRows() {
    auto const row = [](char const* a, char const* b, char const* c) {
        return std::vector<Cell>{std::string{a}, std::string{b}, std::string{c}};
    };
    return {row("F", "S", "X"), row("F", "F", "X"), row("S", "F", "F"), row("F", "X", "Y"), row("X", "Y", "Z")};
}

Filtered const& truthTable() {
    static Filtered const f{.source = companySource(truthTableRows()), .services = {}};
    return f;
}

std::expected<Rows, TableError> by(GroupFilter group) {
    FilterSpec spec;
    spec.groups.push_back(std::move(group));
    return truthTable().by(spec);
}

// A group compiled once over a source whose keys are built once: what CompiledFilter::evaluateGroup says of a row.
class GroupEval {
public:
    GroupEval(VectorSource const& source, GroupFilter const& group) {
        Services const services;
        FilterSpec spec;
        spec.groups.push_back(group);
        auto compiled = compileFilter(spec, source.columns(), services, source.groups());
        REQUIRE(compiled);
        REQUIRE(compiled->groups().size() == 1);
        _compiled = std::move(*compiled);
        auto const snapshot = source.snapshot();
        auto const columns = source.columns();
        _keys.reserve(columns.size());
        for (std::size_t c = 0; c < columns.size(); ++c) {
            _keys.push_back(KeyColumn::forColumn(columns[c], services, snapshot->rowCount()));
            buildKeys(*snapshot, c, columns[c], services, 0, snapshot->rowCount(), _keys.back());
        }
        for (auto const& key : _keys) {
            _pointers.push_back(&key);
        }
    }

    [[nodiscard]] Match operator()(std::uint32_t row, FilterCounters* counters = nullptr) const {
        return CompiledFilter::evaluateGroup(_compiled.groups()[0], _pointers, row, counters);
    }

private:
    CompiledFilter _compiled;
    std::vector<KeyColumn> _keys;
    std::vector<KeyColumn const*> _pointers;
};

// What the group says of one row.
Match groupMatch(VectorSource const& source, GroupFilter const& group, std::uint32_t row) {
    return GroupEval{source, group}(row);
}

// The error code of a refused filter, or nothing when it compiled.
std::optional<TableErrorCode> codeOf(std::expected<Rows, TableError> const& result) {
    return result ? std::nullopt : std::optional{result.error().code};
}

ColumnCount atLeast(std::int64_t n) { return ColumnCount{.kind = ColumnCount::Kind::AtLeast, .n = n}; }
ColumnCount exactly(std::int64_t n) { return ColumnCount{.kind = ColumnCount::Kind::Exactly, .n = n}; }
ColumnCount every() { return ColumnCount{.kind = ColumnCount::Kind::All, .n = 1}; }

}  // namespace

// Mutation: `Together` decided as `atLeast` with n = the entry count: fails on row F, F, X.
TEST_CASE("groups: the spec's truth table", "[table][filter][groups]") {
    CHECK(by({.group = "company", .include = {eq("F")}}) == Rows{0, 1, 2, 3});
    CHECK(by({.group = "company", .mode = GroupMode::Together, .include = {eq("F"), eq("S")}}) == Rows{0, 2});
    CHECK(by({.group = "company", .include = {oneOf({"F", "S", "T"})}}) == Rows{0, 1, 2, 3});
    CHECK(by({.group = "company", .match = atLeast(2), .include = {oneOf({"F", "S"})}}) == Rows{0, 1, 2});
    CHECK(by({.group = "company", .match = exactly(1), .include = {eq("F")}}) == Rows{0, 3});
    CHECK(by({.group = "company", .match = every(), .include = {oneOf({"F", "S"})}}) == Rows{2});
    CHECK(must(by({.columns = {"companyA", "companyB", "companyC"}, .include = {eq("F")}})) ==
          must(by({.group = "company", .include = {eq("F")}})));                      // the plain group is `any`
    CHECK(by({.group = "company", .include = {}, .exclude = {eq("X")}}) == Rows{2});  // "none of the companies is X"
    std::vector<FilterEntry> seventeen;                                               // only the 17th entry can match
    for (int i = 1; i <= 16; ++i) {
        seventeen.push_back(eq("A" + std::to_string(i)));
    }
    seventeen.push_back(eq("F"));
    CHECK(by({.group = "company", .include = seventeen}) == Rows{0, 1, 2, 3});  // a 16-bit entry mask would miss it
    // "two of the three dates are after 2026-05-01": atLeast 2 with a gt entry over three Date columns.
    auto const dates =
        tabletest::makeSource({tabletest::col("d1", ColumnKind::Date), tabletest::col("d2", ColumnKind::Date),
                               tabletest::col("d3", ColumnKind::Date)},
                              {{std::string{"2026-06-01"}, std::string{"2026-07-01"}, std::string{"2026-01-01"}},
                               {std::string{"2026-06-01"}, std::string{"2026-02-01"}, std::string{"2026-01-01"}}});
    Filtered const f{.source = dates, .services = {}};
    FilterSpec spec;
    spec.groups.push_back({.columns = {"d1", "d2", "d3"}, .match = atLeast(2), .include = {gt("2026-05-01")}});
    CHECK(f.by(spec) == Rows{0});
}

// Mutations: `exactly` deciding Yes while q > 0 ({F,X,U} under exactly 1 answers Yes); the known pass reading an
// unknown column as matching every entry ({F,F,U} under Together F, S answers Yes).
TEST_CASE("groups: three-valued over unloaded cells", "[table][filter][groups]") {
    struct Case {
        GroupFilter filter;
        std::array<char const*, 3> cells;  // "U" is an unloaded cell
        Match expected;
    };
    GroupFilter const anyF{.group = "company", .include = {eq("F")}};
    GroupFilter const atLeast2F{.group = "company", .match = atLeast(2), .include = {eq("F")}};
    GroupFilter const allF{.group = "company", .match = every(), .include = {eq("F")}};
    GroupFilter const exactly1F{.group = "company", .match = exactly(1), .include = {eq("F")}};
    GroupFilter const togetherFS{.group = "company", .mode = GroupMode::Together, .include = {eq("F"), eq("S")}};
    GroupFilter const noneX{.group = "company", .include = {}, .exclude = {eq("X")}};
    GroupFilter const anyEmpty{.group = "company", .include = {isEmpty()}};
    std::vector<Case> const cases{
        {.filter = atLeast2F, .cells = {"F", "F", "U"}, .expected = Match::Yes},
        {.filter = atLeast2F, .cells = {"X", "X", "U"}, .expected = Match::No},
        {.filter = atLeast2F, .cells = {"F", "X", "U"}, .expected = Match::Unknown},
        {.filter = anyF, .cells = {"F", "U", "U"}, .expected = Match::Yes},
        {.filter = anyF, .cells = {"X", "X", "U"}, .expected = Match::Unknown},
        {.filter = anyF, .cells = {"X", "X", "X"}, .expected = Match::No},
        {.filter = allF, .cells = {"F", "F", "U"}, .expected = Match::Unknown},
        {.filter = allF, .cells = {"F", "X", "U"}, .expected = Match::No},
        {.filter = exactly1F, .cells = {"F", "F", "U"}, .expected = Match::No},
        {.filter = exactly1F, .cells = {"F", "X", "U"}, .expected = Match::Unknown},
        {.filter = exactly1F, .cells = {"X", "X", "U"}, .expected = Match::Unknown},
        {.filter = exactly1F, .cells = {"F", "X", "X"}, .expected = Match::Yes},
        {.filter = exactly1F, .cells = {"X", "X", "X"}, .expected = Match::No},
        {.filter = togetherFS, .cells = {"F", "S", "U"}, .expected = Match::Yes},
        {.filter = togetherFS, .cells = {"F", "F", "U"}, .expected = Match::Unknown},
        {.filter = togetherFS, .cells = {"U", "U", "X"}, .expected = Match::Unknown},
        {.filter = togetherFS, .cells = {"X", "X", "U"}, .expected = Match::No},
        {.filter = noneX, .cells = {"F", "X", "U"}, .expected = Match::No},
        {.filter = noneX, .cells = {"F", "S", "U"}, .expected = Match::Unknown},
        {.filter = noneX, .cells = {"F", "S", "F"}, .expected = Match::Yes},
        {.filter = anyEmpty, .cells = {"X", "X", "U"}, .expected = Match::Unknown},
    };
    for (std::size_t at = 0; at < cases.size(); ++at) {
        auto const& c = cases[at];
        std::vector<Cell> row;
        for (auto const* const cell : c.cells) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `row` is a fixture list built once; sizing it first would not change what the case checks
            row.push_back(std::string_view{cell} == "U" ? Cell{Unloaded{}} : Cell{std::string{cell}});
        }
        auto const source = companySource({row}, true);
        INFO("case " << at << ": " << c.cells[0] << ", " << c.cells[1] << ", " << c.cells[2]);
        CHECK(groupMatch(*source, c.filter, 0) == c.expected);
    }
}

// Mutation: compileGroup requiring 2 to k include entries in Together mode with no exception for 0: the
// exclude-only Together lines fail.
TEST_CASE("groups: duplicate columns, exactly zero and too many together entries", "[table][filter][groups]") {
    auto const& f = truthTable();
    CHECK(codeOf(f.by(
              FilterSpec{.columns = {}, .groups = {{.columns = {"companyA", "companyA"}, .include = {eq("F")}}}})) ==
          TableErrorCode::InvalidSpec);
    CHECK(by({.group = "company", .match = exactly(0), .include = {eq("F")}}) == Rows{4});
    CHECK(codeOf(f.by(FilterSpec{.columns = {},
                                 .groups = {{.group = "company",
                                             .mode = GroupMode::Together,
                                             .include = {eq("A"), eq("B"), eq("C"), eq("D")}}}})) ==
          TableErrorCode::InvalidSpec);
    CHECK(codeOf(f.by(FilterSpec{
              .columns = {}, .groups = {{.group = "company", .mode = GroupMode::Together, .include = {eq("A")}}}})) ==
          TableErrorCode::InvalidSpec);  // one entry: neither 0 nor 2..k
    // Together takes 0, or 2 to k, include entries: an exclude-only group compiles in either mode.
    CHECK(by({.group = "company", .mode = GroupMode::Together, .include = {}, .exclude = {eq("X")}}) == Rows{2});
    CHECK(must(by({.group = "company", .mode = GroupMode::Together, .include = {}, .exclude = {eq("X")}})) ==
          must(by({.group = "company", .include = {}, .exclude = {eq("X")}})));
    CHECK(by({.group = "company", .mode = GroupMode::Together, .include = {}, .exclude = {}}) == Rows{0, 1, 2, 3, 4});
    CHECK(codeOf(f.by(FilterSpec{.columns = {},
                                 .groups = {{.group = "company", .match = atLeast(4), .include = {eq("F")}}}})) ==
          TableErrorCode::InvalidSpec);
    CHECK(codeOf(f.by(FilterSpec{
              .columns = {},
              .groups = {{.group = "company", .columns = {"companyA", "companyB"}, .include = {eq("F")}}}})) ==
          TableErrorCode::InvalidSpec);
    CHECK(codeOf(f.by(FilterSpec{.columns = {}, .groups = {{.group = "nope", .include = {eq("F")}}}})) ==
          TableErrorCode::UnknownColumn);
}

// Mutation: compileGroup without its 32-a-side bound: the first CHECK gets rows, not LimitExceeded.
TEST_CASE("groups: more than 32 entries on a side is refused where the filter compiles", "[table][filter][groups]") {
    // Engine::setFilter compiles without validate's QueryLimits, so the bound that keeps every entry a bit of a
    // std::uint32_t mask is compileGroup's own. 33 include entries, only the 33rd ("F") able to match.
    std::vector<FilterEntry> thirtyThree;
    for (int i = 1; i <= 32; ++i) {
        thirtyThree.push_back(eq("A" + std::to_string(i)));
    }
    thirtyThree.push_back(eq("F"));
    std::vector<FilterEntry> const thirtyTwo(thirtyThree.end() - 32, thirtyThree.end());  // "A2".."A32", "F"
    CHECK(codeOf(by({.group = "company", .include = thirtyThree})) == TableErrorCode::LimitExceeded);
    CHECK(codeOf(by({.group = "company", .include = {eq("F")}, .exclude = thirtyThree})) ==
          TableErrorCode::LimitExceeded);
    CHECK(by({.group = "company", .include = thirtyTwo}) == Rows{0, 1, 2, 3});  // 32 a side compiles
    CHECK(by({.group = "company", .include = thirtyTwo, .exclude = {eq("X")}}) == Rows{2});
    Engine engine{truthTable().source};
    FilterSpec over;
    over.groups.push_back({.group = "company", .include = thirtyThree});
    auto const refused = engine.setFilter(over);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == TableErrorCode::LimitExceeded);
    CHECK(engine.filter().empty());  // the filter is unchanged
}

// Mutation: detail::checkGroups accepting every declaration: each malformed one applies.
TEST_CASE("groups: a declared group is checked where the columns are read", "[table][filter][groups]") {
    std::vector<ColumnInfo> columns;
    for (int i = 0; i < 18; ++i) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `columns` is a fixture list built once; sizing it first would not change what the case checks
        columns.push_back(tabletest::col("t" + std::to_string(i), ColumnKind::Text));
    }
    columns.push_back(tabletest::col("n", ColumnKind::Integer));
    std::vector<Cell> const row(columns.size(), Cell{std::string{"1"}});
    auto const source = tabletest::makeSource(columns, {row});
    auto const group = [](std::string id, std::vector<std::string> members) {
        return ColumnGroupInfo{.id = std::move(id), .label = {}, .columns = std::move(members)};
    };
    std::vector<std::string> seventeen;
    for (int i = 0; i < 17; ++i) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `seventeen` is a fixture list built once; sizing it first would not change what the case checks
        seventeen.push_back("t" + std::to_string(i));
    }
    std::vector<std::vector<ColumnGroupInfo>> const malformed{
        {group("one", {"t0"})},
        {group("many", seventeen)},
        {group("mixed", {"t0", "n"})},
        {group("a", {"t0", "t1"}), group("b", {"t1", "t2"})},
        {group("missing", {"t0", "nope"})},
    };
    for (auto const& groups : malformed) {
        source->setGroups(groups);
        auto const page = apply(*source, TableQuery{});
        REQUIRE_FALSE(page);
        CHECK(page.error().code == TableErrorCode::SourceContract);
    }
    source->setGroups({group("good", {"t0", "t1", "t2"})});
    CHECK(apply(*source, TableQuery{}).has_value());
}

// Mutations: ColumnCount's reader taking `{"atLeast": n}` as `exactly` (the shape does not read back); the malformed
// check made in Columns mode only (a Together group with a malformed match compiles).
TEST_CASE("groups: the JSON shape reads and writes", "[table][filter][groups]") {
    const auto* const json =
        R"({"group":"company","mode":"together","match":{"atLeast":2},"combine":"all","include":[{"eq":"F"},{"eq":"S"}],"exclude":[{"eq":"X"}]})";
    GroupFilter g;
    REQUIRE_FALSE(glz::read_json(g, std::string{json}));
    CHECK(g.group == "company");
    CHECK(g.mode == GroupMode::Together);
    CHECK(g.match.kind == ColumnCount::Kind::AtLeast);
    CHECK(g.match.n == 2);
    CHECK(g.combine == Combine::All);
    CHECK(g.exclude == std::vector<FilterEntry>{eq("X")});
    for (std::string const shape : {R"("any")", R"("all")", R"({"atLeast":3})", R"({"exactly":0})"}) {
        ColumnCount count;
        REQUIRE_FALSE(glz::read_json(count, shape));
        std::string back;
        REQUIRE_FALSE(glz::write_json(count, back));
        CHECK(back == shape);
    }
    ColumnCount unknown;
    REQUIRE_FALSE(glz::read_json(unknown, std::string{R"({"most":2})"}));
    CHECK(codeOf(by({.group = "company", .match = unknown, .include = {eq("F")}})) == TableErrorCode::InvalidSpec);
    CHECK(codeOf(by(
              {.group = "company", .mode = GroupMode::Together, .match = unknown, .include = {eq("F"), eq("S")}})) ==
          TableErrorCode::InvalidSpec);
}

// Mutation: ColumnCount::operator== comparing `kind` only: a changed `match.n` compares equal.
TEST_CASE("groups: filters compare by value", "[table][filter][groups]") {
    auto const make = [] {
        FilterSpec spec = on("companyA", {oneOf({"F", "S"})});
        spec.groups.push_back({.group = "company", .match = atLeast(2), .include = {oneOf({"F", "S"})}});
        return spec;
    };
    CHECK(make() == make());
    auto member = make();
    member.columns["companyA"].include[0].oneOf->back() = "T";
    CHECK_FALSE(member == make());
    auto count = make();
    count.groups[0].match.n = 3;
    CHECK_FALSE(count == make());
    auto mode = make();
    mode.groups[0].mode = GroupMode::Together;
    CHECK_FALSE(mode == make());
}

namespace {

// One of "A".."E" or empty; Cell{Unloaded{}} with probability `unloaded`.
Cell randomCell(std::mt19937& rng, double unloaded) {
    if (std::bernoulli_distribution{unloaded}(rng)) {
        return Cell{Unloaded{}};
    }
    auto const pick = std::uniform_int_distribution<int>{0, 5}(rng);
    return pick == 5 ? Cell{} : Cell{std::string(1, static_cast<char>('A' + pick))};
}

FilterEntry randomGroupEntry(std::mt19937& rng) {
    auto const letter = [&rng] { return std::string(1, static_cast<char>('A' + (rng() % 5))); };
    switch (rng() % 4) {
        case 0:
            return eq(letter());
        case 1: {
            std::vector<std::string> members;
            auto const count = 1 + (rng() % 3);
            for (std::size_t i = 0; i < count; ++i) {
                // NOLINTNEXTLINE(performance-inefficient-vector-operation): `members` is a fixture list built once; sizing it first would not change what the case checks
                members.push_back(letter());
            }
            return oneOf(std::move(members));
        }
        case 2:
            return ne(letter());
        default:
            return startsWith(letter());
    }
}

// A random group over c0..c{k-1}: Columns mode with any quantifier and combine, or Together with 2..k entries.
GroupFilter randomGroupFilter(std::mt19937& rng, std::size_t k) {
    GroupFilter out;
    for (std::size_t c = 0; c < k; ++c) {
        out.columns.push_back("c" + std::to_string(c));
    }
    std::size_t entries = 0;
    if (rng() % 2 == 0) {
        out.mode = GroupMode::Columns;
        auto const n = static_cast<std::int64_t>(rng() % (k + 1));
        switch (rng() % 4) {
            case 0:
                out.match = ColumnCount{};
                break;
            case 1:
                out.match = every();
                break;
            case 2:
                out.match = atLeast(n);
                break;
            default:
                out.match = exactly(n);
                break;
        }
        out.combine = rng() % 2 == 0 ? Combine::Any : Combine::All;
        entries = 1 + (rng() % k);
    } else {
        out.mode = GroupMode::Together;
        entries = 2 + (rng() % (k - 1));
    }
    for (std::size_t i = 0; i < entries; ++i) {
        out.include.push_back(randomGroupEntry(rng));
    }
    auto const excludes = rng() % 3;
    for (std::size_t i = 0; i < excludes; ++i) {
        out.exclude.push_back(randomGroupEntry(rng));
    }
    return out;
}

// Every row obtained by replacing each unloaded cell by one of "A".."E" or empty.
std::vector<std::vector<Cell>> completions(std::vector<Cell> const& row) {
    std::vector<std::vector<Cell>> out{row};
    for (std::size_t c = 0; c < row.size(); ++c) {
        if (!std::holds_alternative<Unloaded>(row[c])) {
            continue;
        }
        std::vector<std::vector<Cell>> next;
        for (auto const& partial : out) {
            for (int pick = 0; pick <= 5; ++pick) {
                auto completed = partial;
                completed[c] = pick == 5 ? Cell{} : Cell{std::string(1, static_cast<char>('A' + pick))};
                next.push_back(std::move(completed));
            }
        }
        out = std::move(next);
    }
    return out;
}

// A source of `k` text columns c0..c{k-1}, at level "slow" so their cells may be unloaded.
std::shared_ptr<VectorSource> groupColumnsSource(std::size_t k, std::vector<std::vector<Cell>> rows) {
    std::vector<ColumnInfo> columns;
    for (std::size_t c = 0; c < k; ++c) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `columns` is a fixture list built once; sizing it first would not change what the case checks
        columns.push_back(tabletest::levelCol("c" + std::to_string(c), ColumnKind::Text, "slow"));
    }
    return tabletest::makeSource(std::move(columns), std::move(rows));
}

// The six loaded cells the random tables hold: "A".."E", then empty.
std::size_t cellIndex(Cell const& cell) {
    auto const* const text = std::get_if<std::string>(&cell);
    return text == nullptr || text->empty() ? 5 : static_cast<std::size_t>((*text)[0] - 'A');
}

// Per entry, whether it matches each of the six loaded cells, through CompiledFilter::matches.
std::vector<std::array<bool, 6>> entryHits(std::vector<FilterEntry> const& entries) {
    auto const source = tabletest::column(ColumnKind::Text, {std::string{"A"}, std::string{"B"}, std::string{"C"},
                                                             std::string{"D"}, std::string{"E"}, Cell{}});
    Services const services;
    auto keys = KeyColumn::forColumn(source->columns()[0], services, 6);
    buildKeys(*source->snapshot(), 0, source->columns()[0], services, 0, 6, keys);
    std::vector<std::array<bool, 6>> out;
    for (auto const& entry : entries) {
        auto const compiled = detail::compileEntry(entry, source->columns()[0], services);
        REQUIRE(compiled);
        std::array<bool, 6> hits{};
        for (std::uint32_t at = 0; at < 6; ++at) {
            hits.at(at) = CompiledFilter::matches(*compiled, keys, at);
        }
        out.push_back(hits);
    }
    return out;
}

// A group filter with each side's entry hits computed once.
struct Oracle {
    GroupFilter filter;
    std::vector<std::array<bool, 6>> include = entryHits(filter.include);
    std::vector<std::array<bool, 6>> exclude = entryHits(filter.exclude);
};

// The definition of a group filter on a fully loaded row (invariant 24), sharing no code with evaluateGroup.
bool oracle(Oracle const& definition, std::vector<Cell> const& row) {
    auto const& filter = definition.filter;
    std::vector<std::vector<bool>> hits(row.size(), std::vector<bool>(filter.include.size()));
    for (std::size_t c = 0; c < row.size(); ++c) {
        auto const at = cellIndex(row[c]);
        for (std::size_t e = 0; e < filter.include.size(); ++e) {
            hits[c][e] = definition.include[e].at(at);
        }
        for (auto const& entry : definition.exclude) {
            if (entry.at(at)) {
                return false;
            }
        }
    }
    if (filter.include.empty()) {
        return true;
    }
    auto const k = static_cast<std::int64_t>(row.size());
    if (filter.mode == GroupMode::Together) {
        // Try every injective assignment of entries to columns.
        std::vector<std::size_t> columns(row.size());
        // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
        std::iota(columns.begin(), columns.end(), std::size_t{0});
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while): the permutation walk must run its body once before the first permutation step, which a `for` would reorder
        do {
            bool all = true;
            for (std::size_t e = 0; e < filter.include.size() && all; ++e) {
                all = hits[columns[e]][e];
            }
            if (all) {
                return true;
            }
        } while (std::ranges::next_permutation(columns).found);
        return false;
    }
    std::int64_t satisfied = 0;
    for (auto const& column : hits) {
        bool const any = std::ranges::any_of(column, [](bool hit) { return hit; });
        bool const all = std::ranges::all_of(column, [](bool hit) { return hit; });
        if (filter.combine == Combine::All ? all : any) {
            ++satisfied;
        }
    }
    switch (filter.match.kind) {
        case ColumnCount::Kind::All:
            return satisfied == k;
        case ColumnCount::Kind::AtLeast:
            return satisfied >= filter.match.n;
        case ColumnCount::Kind::Exactly:
            return satisfied == filter.match.n;
        case ColumnCount::Kind::Any:
        default:
            return satisfied >= 1;
    }
}

}  // namespace

// Mutation: Together decided as atLeast with n = the entry count.
TEST_CASE("groups: invariant 24 over random tables", "[table][filter][groups]") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 24 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 rng{24};
    for (int table = 0; table < 300; ++table) {
        auto const k = 2 + (rng() % 4);
        std::vector<std::vector<Cell>> rows(40);
        for (auto& row : rows) {
            for (std::size_t c = 0; c < k; ++c) {
                row.push_back(randomCell(rng, 0.0));
            }
        }
        auto const filter = randomGroupFilter(rng, k);
        Oracle const definition{.filter = filter};
        Rows expected;
        for (std::uint32_t r = 0; r < rows.size(); ++r) {
            if (oracle(definition, rows[r])) {
                expected.push_back(r);
            }
        }
        Filtered const f{.source = groupColumnsSource(k, rows), .services = {}};
        FilterSpec spec;
        spec.groups.push_back(filter);
        CHECK(f.by(spec) == expected);
    }
}

// Gate G7's oracle. Mutation: the known pass reading an unknown column as matching every entry: a Yes row has a
// failing completion.
TEST_CASE("groups: invariant 1 over random completions", "[table][filter][groups]") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 1 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 rng{1};
    for (int table = 0; table < 200; ++table) {
        std::vector<std::vector<Cell>> rows(30);
        for (auto& row : rows) {
            for (std::size_t c = 0; c < 3; ++c) {
                row.push_back(randomCell(rng, 1.0 / 3));
            }
        }
        auto const filter = randomGroupFilter(rng, 3);
        Oracle const definition{.filter = filter};
        auto const source = groupColumnsSource(3, rows);
        GroupEval const evaluate{*source, filter};
        for (std::uint32_t r = 0; r < rows.size(); ++r) {
            auto const m = evaluate(r);
            if (m == Match::Unknown) {
                continue;
            }
            bool sound = true;
            for (auto const& completed : completions(rows[r])) {
                sound = sound && oracle(definition, completed) == (m == Match::Yes);
            }
            INFO("table " << table << " row " << r);
            CHECK(sound);
        }
    }
}

// Gate G7. Mutation: Together's wildcard pass skipped (No instead of Unknown): `undecided` is below the oracle's.
TEST_CASE("groups: undecided rows are the three-valued rule's", "[table][filter][groups]") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 7 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 rng{7};
    std::vector<std::vector<Cell>> rows(1000);
    for (auto& row : rows) {
        for (std::size_t c = 0; c < 3; ++c) {
            row.push_back(randomCell(rng, 1.0 / 3));
        }
    }
    auto const source = groupColumnsSource(3, rows);
    Filtered const f{.source = source, .services = {}};
    GroupFilter const anyAB{.columns = {"c0", "c1", "c2"}, .include = {oneOf({"A", "B"})}};
    GroupFilter const togetherAB{
        .columns = {"c0", "c1", "c2"}, .mode = GroupMode::Together, .include = {eq("A"), eq("B")}};
    for (auto const& filter : {anyAB, togetherAB}) {
        std::int64_t oracleYes = 0;
        std::int64_t oracleUnknown = 0;
        for (auto const& row : rows) {
            // Per column: does it hold A, B; is it unloaded.
            std::array<bool, 3> a{};
            std::array<bool, 3> b{};
            std::array<bool, 3> u{};
            for (std::size_t c = 0; c < 3; ++c) {
                u.at(c) = std::holds_alternative<Unloaded>(row[c]);
                auto const* const text = std::get_if<std::string>(&row[c]);
                a.at(c) = text != nullptr && *text == "A";
                b.at(c) = text != nullptr && *text == "B";
            }
            if (filter.mode == GroupMode::Columns) {
                int y = 0;
                int q = 0;
                for (std::size_t c = 0; c < 3; ++c) {
                    y += a.at(c) || b.at(c) ? 1 : 0;
                    q += u.at(c) ? 1 : 0;
                }
                oracleYes += y >= 1 ? 1 : 0;
                oracleUnknown += y == 0 && q > 0 ? 1 : 0;
                continue;
            }
            auto const assignable = [&](std::array<bool, 3> const& ea, std::array<bool, 3> const& eb) {
                for (std::size_t i = 0; i < 3; ++i) {
                    for (std::size_t j = 0; j < 3; ++j) {
                        if (i != j && ea.at(i) && eb.at(j)) {
                            return true;
                        }
                    }
                }
                return false;
            };
            std::array<bool, 3> wa{};
            std::array<bool, 3> wb{};
            for (std::size_t c = 0; c < 3; ++c) {
                wa.at(c) = a.at(c) || u.at(c);
                wb.at(c) = b.at(c) || u.at(c);
            }
            if (assignable(a, b)) {
                ++oracleYes;
            } else if (assignable(wa, wb)) {
                ++oracleUnknown;
            }
        }
        FilterSpec spec;
        spec.groups.push_back(filter);
        auto const page = apply(*source, TableQuery{.sort = {}, .filters = spec, .page = {}});
        REQUIRE(page);
        CHECK(page->undecided == oracleUnknown);
        CHECK(std::cmp_equal(must(f.by(spec)).size(), oracleYes));
    }
}

// Gate G5. Mutations: skipping the covered-mask check (rows that cannot match run augmentations); the matching
// started again from no assignment for each entry (136 searches on a covered row, not 16).
TEST_CASE("groups: a row costs k lookups and at most one augmentation an entry", "[table][filter][groups]") {
    constexpr std::uint32_t kRows = 1000;
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 5 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 rng{5};
    std::vector<ColumnInfo> columns;
    std::vector<std::string> ids;
    for (int c = 0; c < 16; ++c) {
        ids.push_back("g" + std::to_string(c));
        columns.push_back(tabletest::col(ids.back(), ColumnKind::Text));
    }
    std::vector<std::vector<Cell>> rows(kRows);
    for (std::uint32_t r = 0; r < kRows; ++r) {
        // Every 50th row holds v0..v15 in a shuffled order: the together filter below covers it, so its matching
        // runs to the bound. Random rows almost never hold all sixteen values.
        std::vector<int> values(16);
        // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
        std::iota(values.begin(), values.end(), 0);
        if (r % 50 == 0) {
            std::ranges::shuffle(values, rng);
        } else {
            for (auto& value : values) {
                value = static_cast<int>(rng() % 32);
            }
        }
        for (auto const value : values) {
            rows[r].emplace_back("v" + std::to_string(value));
        }
    }
    auto const source = tabletest::makeSource(columns, rows);
    auto const counted = [&](GroupFilter const& filter) {
        GroupEval const evaluate{*source, filter};
        FilterCounters counters;
        for (std::uint32_t r = 0; r < kRows; ++r) {
            static_cast<void>(evaluate(r, &counters));
        }
        return counters;
    };
    auto const absentSetCounters = counted({.columns = ids, .include = {oneOf({"zz1", "zz2"})}});
    auto const heldSetCounters = counted({.columns = ids, .include = {oneOf({"v0", "v1"})}});
    std::vector<FilterEntry> sixteen;
    for (int e = 0; e < 16; ++e) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `sixteen` is a fixture list built once; sizing it first would not change what the case checks
        sixteen.push_back(eq("v" + std::to_string(e)));
    }
    GroupFilter const together{.columns = ids, .mode = GroupMode::Together, .include = sixteen};
    auto const togetherCounters = counted(together);
    CHECK(absentSetCounters.setLookups == std::uint64_t{16} * kRows);
    CHECK(heldSetCounters.setLookups >= kRows);
    CHECK(heldSetCounters.setLookups < std::uint64_t{16} * kRows);
    CHECK(togetherCounters.setLookups == std::uint64_t{16} * kRows);
    std::uint64_t maxAugmentationsOnUncoveredRows = 0;
    std::uint64_t maxAugmentationsOnCoveredRows = 0;
    std::size_t coveredRows = 0;
    GroupEval const evaluateTogether{*source, together};
    for (std::uint32_t r = 0; r < kRows; ++r) {
        FilterCounters counters;
        static_cast<void>(evaluateTogether(r, &counters));
        std::set<std::string> held;
        for (auto const& cell : rows[r]) {
            held.insert(std::get<std::string>(cell));
        }
        bool const covered =
            std::ranges::all_of(sixteen, [&](FilterEntry const& entry) { return held.contains(*entry.eq); });
        coveredRows += covered ? 1 : 0;
        auto& worst = covered ? maxAugmentationsOnCoveredRows : maxAugmentationsOnUncoveredRows;
        worst = std::max(worst, counters.augmentations);
    }
    REQUIRE(coveredRows >= kRows / 50);
    CHECK(maxAugmentationsOnUncoveredRows == 0);
    CHECK(maxAugmentationsOnCoveredRows == 16);  // one search an entry, reached on every covered row
}

// Mutation: the wildcard matching run without the count of matching columns (the first row's search runs, and
// fails, instead of being refused by the count).
TEST_CASE("groups: the wildcard matching needs as many matching columns as entries", "[table][filter][groups]") {
    GroupFilter const together{
        .group = "company", .mode = GroupMode::Together, .include = {eq("A"), eq("B"), eq("C")}};
    // Row 0: A, unloaded, X: two columns can match anything, three entries need three. Row 1: A, two unloaded.
    auto const source = companySource({{std::string{"A"}, Cell{Unloaded{}}, std::string{"X"}},
                                       {std::string{"A"}, Cell{Unloaded{}}, Cell{Unloaded{}}}},
                                      true);
    GroupEval const evaluate{*source, together};
    FilterCounters refused;
    CHECK(evaluate(0, &refused) == Match::No);
    CHECK(refused.wildcardAugmentations == 0);
    FilterCounters searched;
    CHECK(evaluate(1, &searched) == Match::Unknown);
    CHECK(searched.wildcardAugmentations == 3);
}
