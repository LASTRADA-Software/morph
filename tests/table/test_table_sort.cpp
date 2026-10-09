// SPDX-License-Identifier: Apache-2.0
//
// morph::table sort keys and the merge sort (spec 7 §4, §5).
//
// Mutations these tests were seen to fail on:
//  - compareKeys negating the validity result with the value result in the
//    descending branch (invalid first when descending): "invalid cells sort
//    last in both directions" fails.
//  - RowOrder returning `false` on a full tie instead of `a < b`: "ties break
//    by source row" fails.
//  - readExact going through toDouble: "decimals compare exactly" fails.
//  - readExact using saturating `*` instead of checkedMul: "a quantity that
//    overflows its canonical unit is invalid" fails.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <morph/table/sort.hpp>
#include <numeric>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::dec;

namespace {

// Builds every column's keys and sorts the source by `chain`; returns row indices.
struct Sorted {
    std::shared_ptr<VectorSource> source;
    Services services;
    std::vector<KeyColumn> keys;

    std::vector<std::uint32_t> by(SortChain const& chain) {
        auto const snapshot = source->snapshot();
        auto const columns = source->columns();
        keys.clear();
        for (std::size_t c = 0; c < columns.size(); ++c) {
            keys.push_back(KeyColumn::forColumn(columns[c], services, snapshot->rowCount()));
            buildKeys(*snapshot, c, columns[c], services, 0, snapshot->rowCount(), keys.back());
        }
        std::vector<RowOrder::Key> resolved;
        for (auto const& key : chain) {
            for (std::size_t c = 0; c < columns.size(); ++c) {
                if (columns[c].id == key.column) {
                    resolved.push_back({.keys = &keys[c], .dir = key.dir});
                }
            }
        }
        std::vector<std::uint32_t> rows(snapshot->rowCount());
        std::ranges::iota(rows, 0U);
        return sortRows(std::move(rows), RowOrder{std::move(resolved)});
    }
};

using Rows = std::vector<std::uint32_t>;
constexpr auto kAsc = SortDirection::Ascending;
constexpr auto kDesc = SortDirection::Descending;

}  // namespace

TEST_CASE("table: a multi-key sort applies each key's own direction", "[table][sort]") {
    auto const source =
        tabletest::makeSource({tabletest::col("group", ColumnKind::Text), tabletest::col("n", ColumnKind::Integer)},
                              {{std::string{"b"}, std::int64_t{1}},
                               {std::string{"a"}, std::int64_t{1}},
                               {std::string{"b"}, std::int64_t{3}},
                               {std::string{"a"}, std::int64_t{2}}});
    Sorted sorted{.source = source, .services = {}, .keys = {}};
    CHECK(sorted.by({{"group", kAsc}, {"n", kDesc}}) == Rows{3, 1, 2, 0});
    CHECK(sorted.by({{"group", kDesc}, {"n", kAsc}}) == Rows{0, 2, 1, 3});
    CHECK(sorted.by({}) == Rows{0, 1, 2, 3});
}

TEST_CASE("table: ties break by source row in both directions", "[table][sort]") {
    auto const source = tabletest::column(
        ColumnKind::Integer, {std::int64_t{2}, std::int64_t{1}, std::int64_t{2}, std::int64_t{1}, std::int64_t{2}});
    Sorted sorted{.source = source, .services = {}, .keys = {}};
    CHECK(sorted.by({{"c", kAsc}}) == Rows{1, 3, 0, 2, 4});
    CHECK(sorted.by({{"c", kDesc}}) == Rows{0, 2, 4, 1, 3});
}

TEST_CASE("table: ties break by source row across many runs", "[table][sort]") {
    // Enough rows for several initial runs and merge passes, all with one of
    // three keys: a merge that is not stable reorders equal keys.
    std::vector<Cell> cells;
    cells.reserve(3000);
    for (std::int64_t i = 0; i < 3000; ++i) {
        cells.emplace_back(((i * 7) % 3));
    }
    Sorted sorted{.source = tabletest::column(ColumnKind::Integer, cells), .services = {}, .keys = {}};
    auto const rows = sorted.by({{.column = "c", .dir = kAsc}});
    for (std::size_t i = 1; i < rows.size(); ++i) {
        auto const keyA = sorted.keys[0].ints[rows[i - 1]];
        auto const keyB = sorted.keys[0].ints[rows[i]];
        REQUIRE((keyA < keyB || (keyA == keyB && rows[i - 1] < rows[i])));
    }
}

TEST_CASE("table: invalid cells sort last in both directions", "[table][sort]") {
    auto const source = tabletest::column(ColumnKind::Integer, {std::string{"x"}, std::int64_t{5}, Cell{},
                                                                std::int64_t{1}, std::string{""}, std::int64_t{3}});
    Sorted sorted{.source = source, .services = {}, .keys = {}};
    CHECK(sorted.by({{"c", kAsc}}) == Rows{3, 5, 1, 0, 2, 4});
    CHECK(sorted.by({{"c", kDesc}}) == Rows{1, 5, 3, 0, 2, 4});
    CHECK(sorted.keys[0].states[0] == CellState::Invalid);
    CHECK(sorted.keys[0].states[2] == CellState::Empty);
    CHECK(sorted.keys[0].states[4] == CellState::Empty);
}

TEST_CASE("table: an empty source and all-equal keys sort", "[table][sort]") {
    Sorted empty{.source = tabletest::column(ColumnKind::Text, {}), .services = {}, .keys = {}};
    CHECK(empty.by({{"c", kAsc}}).empty());
    Sorted same{.source = tabletest::column(ColumnKind::Text, {std::string{"a"}, std::string{"a"}, std::string{"a"}}),
                .services = {},
                .keys = {}};
    CHECK(same.by({{"c", kDesc}}) == Rows{0, 1, 2});
}

TEST_CASE("table: text sorts through the collator, case-folded by default", "[table][sort]") {
    auto const source = tabletest::column(ColumnKind::Text, {std::string{"Echo"},
                                                             std::string{"\xC3\x89"
                                                                         "b\xC3\xA8ne"},
                                                             std::string{"zulu"}, std::string{"echo"}});
    Sorted byDefault{.source = source, .services = {}, .keys = {}};
    // Code-point order: É folds to é (U+00E9), after every ASCII letter; Echo
    // and echo tie and keep source order.
    CHECK(byDefault.by({{"c", kAsc}}) == Rows{0, 3, 2, 1});

    Services accentFolding;
    accentFolding.collator = std::make_shared<tabletest::AccentFoldingCollator>();
    Sorted folded{.source = source, .services = accentFolding, .keys = {}};
    // Ébène folds to "ebene", before "echo".
    CHECK(folded.by({{"c", kAsc}}) == Rows{1, 0, 3, 2});
    CHECK(folded.keys[0].raw[1] ==
          "\xC3\x89"
          "b\xC3\xA8ne");
    CHECK(folded.keys[0].folded[1] == "ebene");
}

TEST_CASE("table: dates sort from epoch integers and from ISO text", "[table][sort]") {
    auto const source =
        tabletest::makeSource({tabletest::col("d", ColumnKind::Date), tabletest::col("t", ColumnKind::DateTime)},
                              {{std::string{"2026-10-08"}, std::string{"2026-10-08T10:00:00Z"}},
                               {std::int64_t{0}, std::int64_t{86400}},
                               {std::string{"not a date"}, std::string{"1970-01-01T00:00:01Z"}},
                               {std::string{"1999-12-31"}, std::int64_t{-1}}});
    Sorted sorted{.source = source, .services = {}, .keys = {}};
    CHECK(sorted.by({{"d", kAsc}}) == Rows{1, 3, 0, 2});
    CHECK(sorted.by({{"t", kAsc}}) == Rows{3, 2, 1, 0});
    CHECK(sorted.keys[1].ints[0] == (20734 * 86400) + 36000);
}

TEST_CASE("table: decimals compare exactly", "[table][sort]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // 1/3 and 333333333333333333/10^18 differ by 1/(3*10^18): the same double,
    // ordered exactly as Rationals.
    auto const third = morph::math::Rational{Numerator{1}, Denominator{3}, DecimalPlaces{18}};
    auto const almost = dec("0.333333333333333333");
    REQUIRE(third.toDouble(19) == almost.toDouble(19));
    auto const source = tabletest::column(ColumnKind::Decimal, {third, almost, std::string{"0.25"}, std::int64_t{1}});
    Sorted sorted{.source = source, .services = {}, .keys = {}};
    CHECK(sorted.by({{"c", kAsc}}) == Rows{2, 1, 0, 3});
    CHECK(sorted.by({{"c", kDesc}}) == Rows{3, 0, 1, 2});
}

TEST_CASE("table: quantities compare in the canonical unit across units", "[table][sort]") {
    // Canonical unit gram: 1.5 kg, 2,000,000 mg and 1499.999 g.
    auto const kg = QuantityCell{.amount = dec("1.5"), .toCanonical = dec("1000")};
    auto const mg = QuantityCell{.amount = dec("2000000"), .toCanonical = dec("0.001")};
    auto const g = dec("1499.999");
    auto const source = tabletest::column(ColumnKind::Quantity, {kg, mg, g});
    Sorted sorted{.source = source, .services = {}, .keys = {}};
    CHECK(sorted.by({{"c", kAsc}}) == Rows{2, 0, 1});
}

TEST_CASE("table: a quantity that overflows its canonical unit is invalid, not saturated", "[table][sort]") {
    auto const huge = QuantityCell{.amount = morph::math::Rational{std::numeric_limits<std::int64_t>::max() / 2, {}},
                                   .toCanonical = dec("1000")};
    auto const source = tabletest::column(ColumnKind::Quantity, {huge, dec("1")});
    Sorted sorted{.source = source, .services = {}, .keys = {}};
    CHECK(sorted.by({{"c", kDesc}}) == Rows{1, 0});
    CHECK(sorted.keys[0].states[0] == CellState::Invalid);
}

TEST_CASE("table: a Number column treats NaN and unparsable text as invalid", "[table][sort]") {
    auto const source = tabletest::column(
        ColumnKind::Number,
        {std::numeric_limits<double>::quiet_NaN(), 2.5, std::string{"1e2"}, std::string{"abc"}, std::int64_t{-1}});
    Sorted sorted{.source = source, .services = {}, .keys = {}};
    CHECK(sorted.by({{"c", kAsc}}) == Rows{4, 1, 2, 0, 3});
    CHECK(sorted.by({{"c", kDesc}}) == Rows{2, 1, 4, 0, 3});
}

TEST_CASE("table: booleans sort false before true", "[table][sort]") {
    auto const source = tabletest::column(ColumnKind::Bool, {true, std::string{"false"}, std::int64_t{2}, false});
    Sorted sorted{.source = source, .services = {}, .keys = {}};
    CHECK(sorted.by({{"c", kAsc}}) == Rows{1, 3, 0, 2});
}

TEST_CASE("table: a Custom column uses its comparator, or its display text when the name is unknown",
          "[table][sort]") {
    Services services;
    services.comparators.emplace(
        "byLength", [](Cell const& a, Cell const& b) { return displayText(a).size() <=> displayText(b).size(); });
    std::vector<std::vector<Cell>> const rows{{std::string{"zz"}}, {std::string{"a"}}, {std::string{"bbb"}}};

    Sorted known{.source = tabletest::makeSource({tabletest::col("c", ColumnKind::Custom, "byLength")}, rows),
                 .services = services,
                 .keys = {}};
    CHECK(known.by({{"c", kAsc}}) == Rows{1, 0, 2});

    Sorted unknown{.source = tabletest::makeSource({tabletest::col("c", ColumnKind::Custom, "noSuchName")}, rows),
                   .services = services,
                   .keys = {}};
    CHECK(unknown.by({{"c", kAsc}}) == Rows{1, 2, 0});
}

TEST_CASE("table: a stop request ends a merge sort early, and a later run finishes it", "[table][sort]") {
    std::vector<Cell> cells;
    cells.reserve(20000);
    for (std::int64_t i = 0; i < 20000; ++i) {
        cells.emplace_back(((i * 7919) % 20000));
    }
    auto const source = tabletest::column(ColumnKind::Integer, cells);
    Services const services;
    auto const snapshot = source->snapshot();
    auto keys = KeyColumn::forColumn(source->columns()[0], services, snapshot->rowCount());
    buildKeys(*snapshot, 0, source->columns()[0], services, 0, snapshot->rowCount(), keys);
    std::vector<std::uint32_t> rows(snapshot->rowCount());
    std::ranges::iota(rows, 0U);
    ChunkedMergeSort sort{rows, RowOrder{{{.keys = &keys, .dir = kAsc}}}};

    ::core::async::StopSource stopSource;  // NOLINT(misc-const-correctness): request_stop() is non-const
    static_cast<void>(stopSource.request_stop());
    CHECK_FALSE(sort.run(kNoDeadline, stopSource.get_token()));
    CHECK_FALSE(sort.done());
    CHECK(sort.run(kNoDeadline, ::core::async::StopToken{}));
    auto const sorted = std::move(sort).take();
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        REQUIRE(std::cmp_equal(keys.ints[sorted[i]], i));
    }
}

TEST_CASE("table: a past deadline yields between chunks", "[table][sort]") {
    std::vector<Cell> cells;
    cells.reserve(20000);
    for (std::int64_t i = 0; i < 20000; ++i) {
        cells.emplace_back((20000 - i));
    }
    auto const source = tabletest::column(ColumnKind::Integer, cells);
    Services const services;
    auto const snapshot = source->snapshot();
    auto keys = KeyColumn::forColumn(source->columns()[0], services, snapshot->rowCount());
    buildKeys(*snapshot, 0, source->columns()[0], services, 0, snapshot->rowCount(), keys);
    std::vector<std::uint32_t> rows(snapshot->rowCount());
    std::ranges::iota(rows, 0U);
    ChunkedMergeSort sort{rows, RowOrder{{{.keys = &keys, .dir = kAsc}}}};
    std::size_t steps = 0;
    while (!sort.run(std::chrono::steady_clock::now(), ::core::async::StopToken{})) {
        ++steps;
    }
    CHECK(steps > 1);
    auto const sorted = std::move(sort).take();
    CHECK(sorted.front() == 19999U);
    CHECK(sorted.back() == 0U);
}
