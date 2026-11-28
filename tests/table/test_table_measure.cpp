// SPDX-License-Identifier: Apache-2.0
//
// Values with units (docs/spec/table/table.md, "Values with units"): the unit registry and its lineage, a measure's
// key (its dimension's place, then its amount in the dimension's base unit), filters and value lists over measures,
// a level cache under a changed registry, typed rows with a `MeasureValue`, and what a user types for a measure.
//
// Mutations these tests were seen to fail on (each case names its own as well):
//  - a key that ignores the unit's factor: "300 g sorts between 0.2 kg and 0.5 kg" fails.
//  - compareKeys negating the place comparison under Descending: the same case's descending line fails.
//  - `ne` false across dimensions: "filters compare base amounts" fails on ne("250 g").
//  - forColumn's unit tables dropped and the unit looked up by id per cell: "keys look units up by number only"
//    fails.
//  - the level cache skipping the lineage check: "a level cache keeps values under a successor" fails.
//  - the group value index keyed by amount without the place: "a group of measure columns" fails on its first CHECK.
//  - inferColumnKind testing x-measure after its num/den rule: "typed rows carry their unit by id" fails on the kind.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/forms/forms.hpp>
#include <morph/forms/views.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/measure.hpp>
#include <morph/util/quantity.hpp>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using tabletest::col;
using tabletest::dec;

namespace tmeasure {

// A typed row with an amount in a named unit. File scope, so glaze reflects it.
struct MRow {
    std::int64_t id = 0;
    MeasureValue weight;
};

enum class TmUnit : std::uint8_t { kg, g, percent };

// No column overrides: every member is a column.
struct NoOverride {};

}  // namespace tmeasure

template <>
struct morph::units::UnitTraits<tmeasure::TmUnit> {
    static constexpr morph::units::UnitMeta meta(tmeasure::TmUnit unit) noexcept {
        switch (unit) {
            case tmeasure::TmUnit::kg:
                return {.id = "kg", .display = "kg", .defaultDecimals = 3};
            case tmeasure::TmUnit::g:
                return {.id = "g", .display = "g", .defaultDecimals = 1};
            case tmeasure::TmUnit::percent:
            default:
                return {.id = "percent", .display = "%", .defaultDecimals = 2};
        }
    }
    static constexpr std::array<morph::units::UnitRelation<tmeasure::TmUnit>, 1> relations{{
        {.from = tmeasure::TmUnit::g,
         .to = tmeasure::TmUnit::kg,
         .fromTo = Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}},
    }};
};

namespace {

using Rows = std::vector<std::uint32_t>;
using Order = std::vector<std::size_t>;

UnitInfo unitOf(std::string id, std::string symbol, std::string dimension, Rational toBase, std::uint32_t decimals) {
    return UnitInfo{.id = std::move(id),
                    .symbol = std::move(symbol),
                    .dimension = std::move(dimension),
                    .toBase = toBase,
                    .decimals = decimals};
}

UnitInfo kgUnit() { return unitOf("kg", "kg", "mass", Rational{1, DecimalPlaces{0}}, 3); }
UnitInfo gUnit() { return unitOf("g", "g", "mass", dec("0.001"), 0); }
UnitInfo percentUnit() { return unitOf("percent", "%", "ratio", dec("0.01"), 2); }

// "" (seeded), kg, g, percent: numbers 0 to 3.
std::shared_ptr<UnitRegistry const> buildRegistry() {
    UnitRegistryBuilder builder;
    REQUIRE(builder.add(kgUnit()));
    REQUIRE(builder.add(gUnit()));
    REQUIRE(builder.add(percentUnit()));
    return std::move(builder).build();
}

std::shared_ptr<UnitRegistry const> const& registry() {
    static auto const built = buildRegistry();
    return built;
}

Cell m(std::string_view amount, std::string_view unit) {
    return Cell{Measure{.amount = dec(amount), .unit = *registry()->find(unit)}};
}

ColumnInfo measureCol(std::string id, std::vector<std::string> dimensions = {"mass", "ratio"}) {
    auto out = col(std::move(id), ColumnKind::Measure);
    out.dimensions = std::move(dimensions);
    return out;
}

Services withUnits() {
    Services out;
    out.units = registry();
    return out;
}

ApplyOptions applyOptions() {
    ApplyOptions out;
    out.services = withUnits();
    return out;
}

FilterEntry eq(std::string v) { return FilterEntry{.eq = std::move(v)}; }
FilterEntry ne(std::string v) { return FilterEntry{.ne = std::move(v)}; }
FilterEntry gt(std::string v) { return FilterEntry{.gt = std::move(v)}; }
FilterEntry between(std::string a, std::string b) {
    return FilterEntry{.between = std::array<std::string, 2>{std::move(a), std::move(b)}};
}
FilterEntry oneOf(std::vector<std::string> v) { return FilterEntry{.oneOf = std::move(v)}; }

// rows: 0: 0.5 kg   1: 0.3 %   2: 300 g   3: 0.2 kg   4: empty   5: 0.2 %
std::shared_ptr<VectorSource> sixRows(std::vector<std::string> dimensions = {"mass", "ratio"}) {
    return tabletest::makeSource(
        {measureCol("w", std::move(dimensions))},
        {{m("0.5", "kg")}, {m("0.3", "percent")}, {m("300", "g")}, {m("0.2", "kg")}, {Cell{}}, {m("0.2", "percent")}});
}

TableQuery sortBy(std::string column, SortDirection dir = SortDirection::Ascending) {
    TableQuery query;
    query.sort = {SortKey{.column = std::move(column), .dir = dir}};
    query.page = PageRequest{.offset = 0, .limit = 1000};
    return query;
}

Order sortedOf(DataSource const& source, SortDirection dir) {
    auto const page = apply(source, sortBy("w", dir), applyOptions());
    REQUIRE(page.has_value());
    return page->rows;
}

Order sorted(SortDirection dir) { return sortedOf(*sixRows(), dir); }

// The rows of `source` a filter on `w` admits, in source order; an error when it does not compile.
std::expected<Rows, TableError> filterRows(VectorSource const& source, FilterEntry entry) {
    FilterSpec spec;
    spec.columns.emplace("w", ColumnFilter{.combine = Combine::Any, .include = {std::move(entry)}, .exclude = {}});
    auto const services = withUnits();
    auto const compiled = compileFilter(spec, source.columns(), services, source.groups());
    if (!compiled) {
        return std::unexpected(compiled.error());
    }
    auto const snapshot = source.snapshot();
    auto keys = KeyColumn::forColumn(source.columns()[0], services, snapshot->rowCount());
    buildKeys(*snapshot, 0, source.columns()[0], services, 0, snapshot->rowCount(), keys);
    Rows rows(snapshot->rowCount());
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
    std::iota(rows.begin(), rows.end(), 0U);
    FilterRun run{*compiled, {&keys}, rows};
    static_cast<void>(run.run(kNoDeadline, ::core::async::StopToken{}));
    return std::move(run).take();
}

Rows rowsOf(FilterEntry entry) {
    auto const rows = filterRows(*sixRows(), std::move(entry));
    REQUIRE(rows.has_value());
    return *rows;
}

TableError error(FilterEntry entry) {
    auto const rows = filterRows(*sixRows(), std::move(entry));
    REQUIRE_FALSE(rows.has_value());
    return rows.error();
}

TableError errorOnMassOnly(FilterEntry entry) {
    auto const rows = filterRows(*sixRows({"mass"}), std::move(entry));
    REQUIRE_FALSE(rows.has_value());
    return rows.error();
}

// w1, w2 (dimensions mass and ratio), declared group "weights":
// 0: 0.3 kg, 1 kg     1: 5 %, 300 g     2: 30 %, 2 kg     3: empty, 0.3 kg     4: 300 g, 0.3 kg
std::shared_ptr<VectorSource> groupSource() {
    auto source = tabletest::makeSource({measureCol("w1"), measureCol("w2")}, {{m("0.3", "kg"), m("1", "kg")},
                                                                               {m("5", "percent"), m("300", "g")},
                                                                               {m("30", "percent"), m("2", "kg")},
                                                                               {Cell{}, m("0.3", "kg")},
                                                                               {m("300", "g"), m("0.3", "kg")}});
    source->setGroups({ColumnGroupInfo{.id = "weights", .label = "Weights", .columns = {"w1", "w2"}}});
    return source;
}

// The rows a group filter admits; `counters` counts each row's evaluateGroup of the group when not null.
Rows groupRows(GroupFilter group, FilterCounters* counters = nullptr) {
    auto const source = groupSource();
    auto const services = withUnits();
    FilterSpec spec;
    spec.groups.push_back(std::move(group));
    auto const compiled = compileFilter(spec, source->columns(), services, source->groups());
    REQUIRE(compiled.has_value());
    auto const snapshot = source->snapshot();
    std::vector<KeyColumn> keys;
    for (std::size_t column = 0; column < 2; ++column) {
        keys.push_back(KeyColumn::forColumn(source->columns()[column], services, snapshot->rowCount()));
        buildKeys(*snapshot, column, source->columns()[column], services, 0, snapshot->rowCount(), keys.back());
    }
    std::vector<KeyColumn const*> const pointers{&keys.at(0), &keys.at(1)};
    Rows out;
    for (std::uint32_t row = 0; row < snapshot->rowCount(); ++row) {
        if (CompiledFilter::evaluateGroup(compiled->groups()[0], pointers, row, counters) == Match::Yes) {
            out.push_back(row);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("measure: 300 g sorts between 0.2 kg and 0.5 kg in both directions", "[table][measure]") {
    CHECK(sorted(SortDirection::Ascending) == Order{3, 2, 0, 5, 1, 4});
    CHECK(sorted(SortDirection::Descending) == Order{0, 2, 3, 1, 5, 4});  // groups stay, amounts reverse
    // Mutation: a key that ignores the unit's factor (300 g sorts last among the masses).
    // Mutation: compareKeys negating the place comparison under Descending (the descending line fails).
}

TEST_CASE("measure: filters compare base amounts within a dimension", "[table][measure]") {
    CHECK(rowsOf(gt("250 g")) == Rows{0, 2});  // drops 0.2 kg and both percents
    CHECK(rowsOf(eq("300 g")) == rowsOf(eq("0.3 kg")));
    CHECK(rowsOf(eq("0.3 kg")) == Rows{2});             // 300 g; not 0.3 %: they are not equal
    CHECK(rowsOf(ne("250 g")) == Rows{0, 1, 2, 3, 5});  // other dimensions included; empty never matches
    CHECK(rowsOf(eq("0.003")) == Rows{1});              // no unit: a ratio, equal to 0.3 %
    CHECK(rowsOf(oneOf({"300 g", "0.2 percent"})) == Rows{2, 5});
    CHECK(rowsOf(between("0.25 kg", "600 g")) == Rows{0, 2});
    CHECK(error(eq("250 lb")).code == TableErrorCode::InvalidValue);
    CHECK(error(eq("250 lb")).column == "w");
    CHECK(error(between("1 kg", "50 percent")).code == TableErrorCode::InvalidValue);
    CHECK(errorOnMassOnly(eq("0.003")).code == TableErrorCode::InvalidValue);  // dimensions = {"mass"}
    // Mutation: `ne` false across dimensions (ne("250 g") keeps only the masses).
}

TEST_CASE("measure: a group of measure columns filters by base amount", "[table][measure][groups]") {
    // Not row 2: 30 % is 0.3 in the base unit of another dimension than 300 g's.
    CHECK(groupRows({.group = "weights", .include = {eq("300 g")}}) == Rows{0, 1, 3, 4});
    CHECK(groupRows({.group = "weights", .include = {oneOf({"0.3 kg", "0.3"})}}) == Rows{0, 1, 2, 3, 4});
    CHECK(
        groupRows({.group = "weights", .match = {.kind = ColumnCount::Kind::All, .n = 1}, .include = {eq("300 g")}}) ==
        Rows{4});
    CHECK(groupRows({.group = "weights", .mode = GroupMode::Together, .include = {eq("300 g"), eq("1 kg")}}) ==
          Rows{0});
    CHECK(groupRows({.group = "weights", .include = {}, .exclude = {gt("1.5 kg")}}) == Rows{0, 1, 3, 4});
    FilterCounters counters;
    static_cast<void>(groupRows({.group = "weights", .include = {eq("300 g")}}, &counters));
    CHECK(counters.setLookups == 7);  // `any` stops at a match: rows cost 1, 2, 2, 1 (the empty cell none), 1
    // Mutation: the group value index keyed by amount without the place (row 2's 30 % joins the first result).
    // Mutation: a Measure `eq` kept as a condition entry instead of an index entry (setLookups is 0).
}

TEST_CASE("measure: invalid cells and the registry contract", "[table][measure]") {
    // 0: 0.5 kg   1: an unknown unit   2: empty   3: 0.2 % (a ratio, in a mass-only column)   4: 300 g
    auto const source =
        tabletest::makeSource({measureCol("w", {"mass"})}, {{m("0.5", "kg")},
                                                            {Cell{Measure{.amount = dec("1"), .unit = kUnknownUnit}}},
                                                            {Cell{}},
                                                            {m("0.2", "percent")},
                                                            {m("300", "g")}});
    auto const services = withUnits();
    auto keys = KeyColumn::forColumn(source->columns()[0], services, 5);
    buildKeys(*source->snapshot(), 0, source->columns()[0], services, 0, 5, keys);
    CHECK(keys.states == std::vector<CellState>{CellState::Valid, CellState::Invalid, CellState::Empty,
                                                CellState::Invalid, CellState::Valid});
    CHECK(sortedOf(*source, SortDirection::Ascending) == Order{4, 0, 1, 2, 3});  // empty and invalid after valid
    CHECK(sortedOf(*source, SortDirection::Descending) == Order{0, 4, 1, 2, 3});
    ApplyOptions const none;  // services.units null
    auto const refused = apply(*source, sortBy("w"), none);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::SourceContract);
    TableQuery filtered;
    filtered.filters.columns.emplace("w",
                                     ColumnFilter{.combine = Combine::Any, .include = {eq("1 kg")}, .exclude = {}});
    auto const refusedFilter = apply(*source, filtered, none);
    REQUIRE_FALSE(refusedFilter.has_value());
    CHECK(refusedFilter.error().code == TableErrorCode::SourceContract);
    auto const refusedValues = distinctValues(*source, TableQuery{}, DistinctRequest{.column = "w"}, none);
    REQUIRE_FALSE(refusedValues.has_value());
    CHECK(refusedValues.error().code == TableErrorCode::SourceContract);
    // Mutation: computeView not checking the sort's columns for a registry (the sort without one is answered).
}

TEST_CASE("measure: negative and zero amounts", "[table][measure]") {
    // 0: -300 g   1: -0.2 kg   2: 0 g   3: 0 kg   4: -0.5 %
    auto const source = tabletest::makeSource(
        {measureCol("w")},
        {{m("-300", "g")}, {m("-0.2", "kg")}, {m("0", "g")}, {m("0", "kg")}, {m("-0.5", "percent")}});
    CHECK(sortedOf(*source, SortDirection::Ascending) == Order{0, 1, 2, 3, 4});   // ties by row, then the ratio
    CHECK(sortedOf(*source, SortDirection::Descending) == Order{2, 3, 1, 0, 4});  // ratio last in both directions
    auto const values = distinctValues(*source, TableQuery{}, DistinctRequest{.column = "w"}, applyOptions());
    REQUIRE(values.has_value());
    auto const zero = std::ranges::find(values->values, std::string{"0 g"}, &DistinctValue::text);
    REQUIRE(zero != values->values.end());
    CHECK(zero->count == 2);  // 0 g and 0 kg are one value
    CHECK(errorOnMassOnly(eq("0")).code == TableErrorCode::InvalidValue);
    // Mutation: a dimension's place read from the registry's order even where the column names its own (the ratio
    // sorts first, and "0" compiles on the mass-only column).
}

TEST_CASE("measure: 0.3 kg and 300 g are one distinct value", "[table][measure]") {
    auto const source =
        tabletest::makeSource({measureCol("w")}, {{m("0.3", "kg")}, {m("300", "g")}, {m("0.3", "percent")}});
    auto const d = distinctValues(*source, TableQuery{}, DistinctRequest{.column = "w"}, applyOptions());
    REQUIRE(d.has_value());
    REQUIRE(d->values.size() == 2);
    CHECK(d->values[0].count == 2);
    CHECK(d->values[0].dimension == "mass");
    CHECK(d->values[0].unit == "kg");
    CHECK(d->values[0].text == "0.3 kg");
    CHECK(d->values[0].operand == "0.3 kg");
    CHECK(d->values[1].dimension == "ratio");
    CHECK(d->values[1].text == "0.3 %");
    CHECK(d->values[1].operand == "0.3 percent");
    // Mutation: a measure value described by its key's base amount, with no unit (`text` is "0.3").
}

TEST_CASE("measure: invariant 25 over random amounts", "[table][measure]") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 25 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 random{25};
    std::array<std::string_view, 4> const units{"kg", "g", "percent", ""};
    std::uniform_int_distribution<std::int64_t> amounts{-20'000, 20'000};
    std::uniform_int_distribution<int> places{0, 3};
    std::uniform_int_distribution<std::size_t> unitOfRow{0, units.size() - 1};
    std::vector<std::vector<Cell>> rows;
    struct Expected {
        std::uint32_t place = 0;
        Rational base;
        std::string operand;
    };
    std::vector<Expected> expected;
    for (int row = 0; row < 500; ++row) {
        auto const scale = places(random);
        std::int64_t denominator = 1;
        for (int i = 0; i < scale; ++i) {
            denominator *= 10;
        }
        Rational const amount{Numerator{amounts(random)}, Denominator{denominator},
                              DecimalPlaces{static_cast<std::uint32_t>(scale)}};
        auto const unit = units.at(unitOfRow(random));
        auto const number = *registry()->find(unit);
        rows.push_back({Cell{Measure{.amount = amount, .unit = number}}});
        auto const& info = registry()->info(number);
        auto const base = morph::math::checkedMul(amount, info.toBase);
        REQUIRE(base.has_value());
        auto const text = morph::units::detail::formatRationalDecimal(amount);
        expected.push_back(Expected{.place = info.dimension == "mass" ? 0U : 1U,
                                    .base = *base,
                                    .operand = unit.empty() ? text : text + " " + std::string{unit}});
    }
    auto const source = tabletest::makeSource({measureCol("w")}, std::move(rows));
    for (auto const dir : {SortDirection::Ascending, SortDirection::Descending}) {
        Order want(expected.size());
        // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
        std::iota(want.begin(), want.end(), std::size_t{0});
        std::ranges::sort(want, [&](std::size_t a, std::size_t b) {
            auto const& x = expected[a];
            auto const& y = expected[b];
            if (x.place != y.place) {
                return x.place < y.place;
            }
            if (!(x.base == y.base)) {
                return dir == SortDirection::Ascending ? x.base < y.base : y.base < x.base;
            }
            return a < b;
        });
        CHECK(sortedOf(*source, dir) == want);
    }
    for (std::size_t row = 0; row < expected.size(); row += 37) {
        Rows want;
        for (std::size_t other = 0; other < expected.size(); ++other) {
            if (expected[other].place == expected[row].place && expected[other].base == expected[row].base) {
                want.push_back(static_cast<std::uint32_t>(other));
            }
        }
        INFO("operand " << expected[row].operand);
        auto const got = filterRows(*source, eq(expected[row].operand));
        REQUIRE(got.has_value());
        CHECK(*got == want);
    }
    // Mutation: a key that ignores the unit's factor (both orders differ from the oracle's).
}

TEST_CASE("measure: keys look units up by number only", "[table][measure]") {
    // Gate M5. Mutation: forColumn's unit tables dropped and `find(info(unit).id)` read per cell (100,000 finds).
    auto const kg = *registry()->find("kg");
    auto const g = *registry()->find("g");
    std::vector<std::vector<Cell>> rows;
    rows.reserve(100'000);
    for (std::int64_t row = 0; row < 100'000; ++row) {
        rows.push_back({Cell{Measure{.amount = Rational{row, DecimalPlaces{0}}, .unit = row % 2 == 0 ? kg : g}}});
    }
    auto const source = tabletest::makeSource({measureCol("w")}, std::move(rows));
    auto const services = withUnits();
    auto const before = registry()->findCalls();
    auto keys = KeyColumn::forColumn(source->columns()[0], services, 100'000);
    buildKeys(*source->snapshot(), 0, source->columns()[0], services, 0, 100'000, keys);
    CHECK(registry()->findCalls() == before);
    CHECK(keys.states[1] == CellState::Valid);
    CHECK(keys.exact[1] == dec("0.001"));
}

TEST_CASE("units: a unit id is a token", "[table][measure]") {
    UnitRegistryBuilder b;
    for (std::string_view const bad :
         {"k g", "1kg", "-kg", "+kg", ".kg", ",kg", "", "abcdefghijklmnopqrstuvwxyz0123456"}) {  // last: 33 bytes
        INFO("id \"" << bad << "\"");
        auto const added = b.add(unitOf(std::string{bad}, "x", "mass", dec("1"), 0));
        REQUIRE_FALSE(added.has_value());
        CHECK(added.error().code == TableErrorCode::InvalidSpec);
    }
    CHECK(b.add(kgUnit()));
    CHECK_FALSE(b.add(kgUnit()));  // a second unit of one id
    auto const zero = b.add(unitOf("z", "z", "mass", dec("0"), 0));
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error().code == TableErrorCode::InvalidSpec);
    auto const r = std::move(b).build();
    CHECK(r->find("") == UnitId{0});
    CHECK(r->info(0).dimension == "ratio");
    CHECK(std::ranges::equal(r->dimensions(), std::vector<std::string>{"ratio", "mass"}));
    // Mutation: the builder accepting an id that begins with a digit (`1kg` is added).
}

TEST_CASE("units: numbers survive a successor and tombstones stay", "[table][measure]") {
    auto const& first = registry();
    UnitRegistryBuilder next{first};
    CHECK(next.remove("g"));
    CHECK_FALSE(next.remove(""));  // the ratio's unit stays
    auto const mg = next.add(unitOf("mg", "mg", "mass", dec("0.000001"), 0));
    REQUIRE(mg.has_value());
    auto const second = std::move(next).build();
    CHECK(second->find("kg") == first->find("kg"));
    CHECK_FALSE(second->find("g"));
    CHECK_FALSE(second->contains(*first->find("g")));
    CHECK(*mg == 4);  // after "", kg, g, percent; g's 2 is never given again
    CHECK(second->lineage() == first->lineage());
    CHECK(std::move(UnitRegistryBuilder{}).build()->lineage() != first->lineage());
    CHECK(std::ranges::equal(second->unitsOf("mass"), std::vector<UnitId>{*first->find("kg"), *mg}));  // largest first
    // A measure holding g's number under `second` is an invalid cell.
    Services services;
    services.units = second;
    auto const source = tabletest::makeSource({measureCol("w")}, {{Cell{Measure{.amount = dec("1"), .unit = 2}}}});
    auto keys = KeyColumn::forColumn(source->columns()[0], services, 1);
    buildKeys(*source->snapshot(), 0, source->columns()[0], services, 0, 1, keys);
    CHECK(keys.states[0] == CellState::Invalid);
    // Mutation: build() giving `find` the tombstone's id (second->find("g") is engaged).
}

TEST_CASE("units: a level cache keeps values under a successor and drops them under another lineage",
          "[table][measure]") {
    auto current = registry();
    auto const gNumber = *registry()->find("g");
    LevelCacheOptions options;
    options.records = {"Weights"};
    options.level = "slow";
    options.columns = {measureCol("w")};
    options.units = [&current] { return current; };
    CellLevelCache cache{options, [gNumber](std::span<RowId const> keys) {
                             std::vector<std::pair<RowId, std::vector<Cell>>> out;
                             for (auto const& key : keys) {
                                 out.emplace_back(
                                     key, std::vector<Cell>{Cell{Measure{.amount = dec("300"), .unit = gNumber}}});
                             }
                             return out;
                         }};
    std::vector<RowId> const one{RowId{std::int64_t{1}}};
    REQUIRE(cache.want(one, LevelUse::Display));
    auto const read = [&cache](std::int64_t key) {
        std::vector<Cell> out(1);
        return cache.snapshot()->read(RowId{key}, out);
    };
    REQUIRE(read(1) == LevelRead::Loaded);
    CHECK(cache.stats().loaderCalls == 1);
    UnitRegistryBuilder next{registry()};
    REQUIRE(next.add(unitOf("t", "t", "mass", dec("1000"), 0)));
    current = std::move(next).build();  // a successor: the same lineage
    CHECK(read(1) == LevelRead::Loaded);
    CHECK(cache.stats().loaderCalls == 1);
    current = buildRegistry();  // another lineage
    CHECK(read(1) == LevelRead::Unloaded);
    CHECK(cache.stats().invalidatedKeys >= 1);
    // Mutation: the level cache skipping the lineage check (the last read is Loaded).
}

namespace {

// A one-column level cache whose loader returns 300 g for every key, numbered in `registry()`, and runs `during`
// while it loads; its registry is whatever `current` holds when the cache asks.
struct LineageRig {
    explicit LineageRig(std::function<void()> during = {})
        : _during{std::move(during)}, cache{optionsOf(current), [this](std::span<RowId const> keys) {
                                                if (_during) {
                                                    _during();
                                                }
                                                std::vector<std::pair<RowId, std::vector<Cell>>> out;
                                                for (auto const& key : keys) {
                                                    out.emplace_back(key, std::vector<Cell>{m("300", "g")});
                                                }
                                                return out;
                                            }} {}

    static LevelCacheOptions optionsOf(std::shared_ptr<UnitRegistry const>& current) {
        LevelCacheOptions options;
        options.records = {"Weights"};
        options.level = "slow";
        options.columns = {measureCol("w")};
        options.units = [&current] { return current; };
        return options;
    }

    [[nodiscard]] LevelRead read(std::int64_t key) const {
        std::vector<Cell> out(1);
        return cache.snapshot()->read(RowId{key}, out);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): `current` is the registry the case swaps between reads
    std::shared_ptr<UnitRegistry const> current = registry();

private:
    std::function<void()> _during;

public:
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): `cache` is the cache under test, which the cases call directly
    CellLevelCache cache;
};

}  // namespace

// Mutation: the lineage of a load taken when it is stored rather than when it is issued (the value loaded under
// `registry()` is stored as the new lineage's, and read Loaded).
TEST_CASE("units: a load across a swap to another lineage is not stored", "[table][measure]") {
    LineageRig* rig = nullptr;
    LineageRig rigged{[&rig] { rig->current = buildRegistry(); }};  // the swap lands while the loader runs
    rig = &rigged;
    REQUIRE(rigged.cache.want(std::vector<RowId>{RowId{std::int64_t{1}}}, LevelUse::Display));
    CHECK(rigged.cache.stats().loaderCalls == 1);
    CHECK(rigged.cache.stats().droppedLoads == 1);
    CHECK(rigged.read(1) == LevelRead::Unloaded);
}

// Mutation: a want and a store not checking the lineage before they add values (key 1, loaded under the old
// lineage, is still read Loaded after key 2 is stored under the new one).
TEST_CASE("units: a load under another lineage drops the values of the old one first", "[table][measure]") {
    LineageRig rig;
    REQUIRE(rig.cache.want(std::vector<RowId>{RowId{std::int64_t{1}}}, LevelUse::Display));
    REQUIRE(rig.read(1) == LevelRead::Loaded);
    rig.current = buildRegistry();  // another lineage, swapped in with no read in between
    REQUIRE(rig.cache.want(std::vector<RowId>{RowId{std::int64_t{2}}}, LevelUse::Display));
    CHECK(rig.read(2) == LevelRead::Loaded);
    CHECK(rig.read(1) == LevelRead::Unloaded);
}

TEST_CASE("units: addUnitsOf reads a unit enum", "[table][measure]") {
    using tmeasure::TmUnit;
    UnitRegistryBuilder builder;
    std::array<TmUnit, 3> const units{TmUnit::kg, TmUnit::g, TmUnit::percent};
    auto const added = addUnitsOf<TmUnit>(
        builder, units, +[](TmUnit unit) -> std::string_view { return unit == TmUnit::percent ? "ratio" : "mass"; });
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error().code == TableErrorCode::InvalidSpec);  // percent reaches no "" through relations
    CHECK(added.error().column == "percent");
    auto const built = std::move(builder).build();
    REQUIRE(built->find("kg"));
    REQUIRE(built->find("g"));
    CHECK_FALSE(built->find("percent"));
    CHECK(built->info(*built->find("kg")) == unitOf("kg", "kg", "mass", dec("1"), 3));
    CHECK(built->info(*built->find("g")) == unitOf("g", "g", "mass", dec("0.001"), 1));
    // Mutation: the relation search not taking a relation in reverse (with kg first, g still resolves forward;
    // a g-first list then fails: kg is refused).
    UnitRegistryBuilder reversed;
    std::array<TmUnit, 2> const gFirst{TmUnit::g, TmUnit::kg};
    REQUIRE(addUnitsOf<TmUnit>(reversed, gFirst, +[](TmUnit) -> std::string_view { return "mass"; }));
    auto const fromG = std::move(reversed).build();
    CHECK(fromG->info(*fromG->find("kg")).toBase == dec("1000"));
}

// Mutation: addUnitsOf taking the first enumerator of a dimension the builder holds with no unit of factor 1 as its
// base (kg is added with g as its base, at factor 1000 of a base that is not one).
TEST_CASE("units: addUnitsOf refuses a dimension held with no base unit", "[table][measure]") {
    using tmeasure::TmUnit;
    UnitRegistryBuilder builder;
    REQUIRE(builder.add(gUnit()));  // mass, at 1/1000 of a base the builder does not hold
    std::array<TmUnit, 2> const units{TmUnit::g, TmUnit::kg};
    auto const added = addUnitsOf<TmUnit>(builder, units, +[](TmUnit) -> std::string_view { return "mass"; });
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error().code == TableErrorCode::InvalidSpec);
    auto const built = std::move(builder).build();
    CHECK_FALSE(built->find("kg"));
    CHECK(built->info(*built->find("g")).toBase == dec("0.001"));
}

TEST_CASE("measure: typed rows carry their unit by id", "[table][measure]") {
    using tmeasure::MRow;
    std::vector<MRow> const rows{{.id = 1, .weight = {.amount = dec("0.5"), .unit = "kg"}},
                                 {.id = 2, .weight = {.amount = dec("300"), .unit = "g"}},
                                 {.id = 3, .weight = {.amount = dec("1"), .unit = "lb"}}};
    RowKey<MRow> const key = [](MRow const& row) { return RowId{row.id}; };
    auto const page = apply<MRow>(rows, key, sortBy("weight"), applyOptions());
    REQUIRE(page.has_value());
    std::vector<std::int64_t> ids;
    for (auto const& row : page->rows) {
        ids.push_back(row.id);
    }
    CHECK(ids == std::vector<std::int64_t>{2, 1, 3});  // 300 g, 0.5 kg, then the unknown unit as invalid
    CHECK(detail::reflectedColumns<MRow>().at(1).kind == ColumnKind::Measure);
    std::string written;
    REQUIRE_FALSE(glz::write_json(rows[1], written));
    CHECK(written == R"({"id":2,"weight":{"num":300,"den":1,"unit":"g"}})");
    MRow read;
    REQUIRE_FALSE(glz::read_json(read, written));
    CHECK(read.weight == rows[1].weight);
    glz::generic_u64 schema{};
    REQUIRE_FALSE(glz::read_json(schema, morph::forms::schemaJson<MRow>()));
    INFO(morph::forms::schemaJson<MRow>());
    CHECK(schema["properties"]["weight"]["x-measure"].get_boolean());
    CHECK_FALSE(schema["properties"]["weight"].contains("x-units"));
    auto const json = morph::views::detail::deriveColumns<tmeasure::NoOverride, MRow>();
    INFO(json);
    glz::generic columns;
    REQUIRE_FALSE(glz::read_json(columns, json));
    REQUIRE(columns.is_array());
    REQUIRE(columns.get_array().size() == 2);
    CHECK(columns.get_array()[1]["kind"].get_string() == "measure");
    // Mutation: inferColumnKind testing x-measure after its num/den rule (the kind reads "decimal").
}

TEST_CASE("measure: what a user types", "[table][measure]") {
    auto const& en = morph::ui::localeFacts("en");
    auto const& de = morph::ui::localeFacts("de");
    std::vector<UnitInfo> const units{kgUnit(), gUnit(), percentUnit()};
    using morph::ui::TypedMeasure;
    CHECK(morph::ui::parseMeasure("0.3kg", units, en) == TypedMeasure{dec("0.3"), "kg"});
    CHECK(morph::ui::parseMeasure("0.3 kg", units, en) == TypedMeasure{dec("0.3"), "kg"});
    CHECK(morph::ui::parseMeasure("300 g", units, en) == TypedMeasure{dec("300"), "g"});
    CHECK(morph::ui::parseMeasure("0.3 KG", units, en) == TypedMeasure{dec("0.3"), "kg"});  // then without case
    CHECK(morph::ui::parseMeasure("5 %", units, en) == TypedMeasure{dec("5"), "percent"});  // by symbol
    CHECK(morph::ui::parseMeasure("0,3 kg", units, de) == TypedMeasure{dec("0.3"), "kg"});
    CHECK_FALSE(morph::ui::parseMeasure("0.3 kg", units, de));  // '.' is de's group separator
    CHECK(morph::ui::parseMeasure("12", units, en, "g") == TypedMeasure{dec("12"), "g"});
    CHECK(morph::ui::parseMeasure("12", units, en) == TypedMeasure{dec("12"), ""});
    CHECK(morph::ui::parseMeasure("-1.5 g", units, en) == TypedMeasure{dec("-1.5"), "g"});
    CHECK_FALSE(morph::ui::parseMeasure("0.3 lb", units, en));
    CHECK(morph::ui::measureOperand({dec("0.3"), "kg"}) == "0.3 kg");
    CHECK(morph::ui::measureOperand({dec("0.003"), ""}) == "0.003");
    // Mutation: the unit matched with case only (`0.3 KG` is refused).
}
