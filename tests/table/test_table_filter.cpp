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
//  The oneOf cases name their own mutations above them.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <numeric>
#include <random>
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
    [[nodiscard]] std::expected<Rows, TableError> by(FilterSpec const& spec) const {
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

std::vector<FilterEntry> eqEntries(std::vector<std::string> const& values) {
    std::vector<FilterEntry> out;
    for (auto const& value : values) {
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
    std::vector<Case> const cases{{ColumnKind::Integer, {.eq = "1"}},
                                  {ColumnKind::Integer, {.ne = "1"}},
                                  {ColumnKind::Integer, {.lt = "1"}},
                                  {ColumnKind::Integer, {.le = "1"}},
                                  {ColumnKind::Integer, {.gt = "1"}},
                                  {ColumnKind::Integer, {.ge = "1"}},
                                  {ColumnKind::Decimal, {.between = std::array<std::string, 2>{"1", "2"}}},
                                  {ColumnKind::Text, {.contains = "a"}},
                                  {ColumnKind::Text, {.startsWith = "a"}},
                                  {ColumnKind::Text, {.endsWith = "a"}},
                                  {ColumnKind::Date, {.eq = "2026-01-01"}},
                                  {ColumnKind::Bool, {.eq = "true"}},
                                  {ColumnKind::Text, {.isEmpty = true}},
                                  {ColumnKind::Text, {.notEmpty = true}}};
    for (auto const& [kind, entry] : cases) {
        for (bool const exclude : {false, true}) {
            for (auto const combine : {Combine::Any, Combine::All}) {
                auto source = tabletest::slowColumn(kind, {Cell{Unloaded{}}});
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
        columns.push_back(tabletest::levelCol(ids.at(c), kinds.at(c), "slow"));
    }
    // The view, sorted, and the undecided rows of `rows` under `spec`, as source rows.
    auto const run = [&](std::vector<std::vector<Cell>> rows, FilterSpec const& spec) {
        Engine engine{tabletest::makeSource(columns, std::move(rows))};
        REQUIRE(engine.setFilter(spec));
        Rows view(engine.view().begin(), engine.view().end());
        Rows undecided(engine.undecidedRows().begin(), engine.undecidedRows().end());
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
    Filtered const text{.source = tabletest::column(ColumnKind::Text, {std::string{"Alpha"}, std::string{"beta"},
                                                                       std::string{"Gamma"}, Cell{},
                                                                       std::string{"alpha"}}),
                        .services = {}};
    CHECK(text.by(on("c", {oneOf({"alpha", "BETA"})})) == Rows{0, 1, 4});
    CHECK(*text.by(on("c", {oneOf({"alpha", "BETA"})})) == *text.by(on("c", {eq("alpha"), eq("BETA")})));
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
    auto const source = tabletest::slowColumn(ColumnKind::Text,
                                              {std::string{"Alpha"}, Cell{morph::table::Unloaded{}}, std::string{"b"}});
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
    CHECK(*text.by(on("c", {oneOf({"alpha", "ALPHA"})})) == *text.by(on("c", {oneOf({"alpha"})})));
}

// Gate D5: one lookup a cell, whatever the set's size. Mutation: a text set scanned member by member, each
// comparison counted as a lookup.
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
        std::vector<Cell> cells;           // the values a cell is drawn from
        std::vector<std::string> operands;  // the same values as filter text
    };
    std::vector<Kind> const kinds{
        {ColumnKind::Text,
         {std::string{"a"}, std::string{"B"}, std::string{"b"}, std::string{"c"}, std::string{"D"}, std::string{"e"},
          std::string{"F"}, std::string{"g"}},
         {"a", "B", "b", "c", "D", "e", "F", "g"}},
        {ColumnKind::Integer,
         {std::int64_t{1}, std::int64_t{2}, std::int64_t{3}, std::int64_t{4}, std::int64_t{5}, std::int64_t{6},
          std::int64_t{7}, std::int64_t{8}},
         {"1", "2", "3", "4", "5", "6.0", "7", "8"}},
        {ColumnKind::Decimal,
         {dec("0.5"), dec("1"), dec("1.25"), dec("2"), dec("2.5"), dec("3"), dec("3.75"), dec("4")},
         {"0.5", "1.0", "1.25", "2", "2.50", "3", "3.75", "4"}},
        {ColumnKind::Date,
         {std::string{"2026-01-01"}, std::string{"2026-01-02"}, std::string{"2026-01-03"}, std::string{"2026-01-04"},
          std::string{"2026-01-05"}, std::string{"2026-01-06"}, std::string{"2026-01-07"},
          std::string{"2026-01-08"}},
         {"2026-01-01", "2026-01-02", "2026-01-03", "2026-01-04", "2026-01-05", "2026-01-06", "2026-01-07",
          "2026-01-08"}},
        {ColumnKind::Bool, {true, false}, {"true", "false"}},
    };
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
            values.push_back(kind.operands.at(rng() % kind.operands.size()));
        }
        REQUIRE(f.by(on("c", {oneOf(values)})));
        CHECK(*f.by(on("c", {oneOf(values)})) == *f.by(on("c", eqEntries(values))));
        CHECK(*f.by(on("c", {}, Combine::Any, {oneOf(values)})) ==
              *f.by(on("c", {}, Combine::Any, eqEntries(values))));
    }
}
