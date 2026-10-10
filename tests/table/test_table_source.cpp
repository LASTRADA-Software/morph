// SPDX-License-Identifier: Apache-2.0
//
// morph::table data sources, snapshots and the default services.
//
// Mutations these tests were seen to fail on:
//  - TableSnapshot::withRow writing into the shared chunk instead of a copy:
//    "an update leaves an earlier snapshot unchanged" fails.
//  - CodePointCollator::fold not lowering ASCII: "the default collator folds
//    case" fails.
//  - parseDecimal accepting 19 fractional digits: "parseDecimal is exact and
//    strict" fails.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
#include <memory>
#include <morph/table/data_source.hpp>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::dec;

namespace {

struct RecordingListener final : ChangeListener {
    std::vector<RowChange> changes;
    void rowsChanged(RowChange const& change) override { changes.push_back(change); }
};

std::vector<std::vector<Cell>> numberedRows(std::size_t count) {
    std::vector<std::vector<Cell>> rows;
    rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back({Cell{static_cast<std::int64_t>(i)}, Cell{std::string{"row "} + std::to_string(i)}});
    }
    return rows;
}

std::vector<ColumnInfo> twoColumns() {
    return {tabletest::col("n", ColumnKind::Integer), tabletest::col("t", ColumnKind::Text)};
}

}  // namespace

TEST_CASE("table: a snapshot is a pointer copy of the source's rows", "[table][source]") {
    auto const source = tabletest::makeSource(twoColumns(), numberedRows(3));
    auto const first = source->snapshot();
    auto const second = source->snapshot();
    CHECK(first.get() == second.get());
    REQUIRE(first->rowCount() == 3);
    CHECK(std::get<std::int64_t>(first->cell(2, 0)) == 2);
    CHECK(std::get<std::int64_t>(first->rowId(1)) == 1);
}

TEST_CASE("table: an update leaves an earlier snapshot unchanged and copies one chunk", "[table][source]") {
    auto const rows = TableSnapshot::kChunkRows * 3;
    auto const source = tabletest::makeSource(twoColumns(), numberedRows(rows));
    auto const before = source->tableSnapshot();
    source->updateRow(5, {Cell{std::int64_t{500}}, Cell{std::string{"five"}}});
    auto const after = source->tableSnapshot();

    CHECK(std::get<std::int64_t>(before->cell(5, 0)) == 5);
    CHECK(std::get<std::int64_t>(after->cell(5, 0)) == 500);
    CHECK_FALSE(after->sharesChunk(*before, 5));
    CHECK(after->sharesChunk(*before, TableSnapshot::kChunkRows));
    CHECK(after->sharesChunk(*before, TableSnapshot::kChunkRows * 2));
}

TEST_CASE("table: readColumn hands runs that cover exactly the rows asked for", "[table][source]") {
    auto const source = tabletest::makeSource(twoColumns(), numberedRows(TableSnapshot::kChunkRows + 10));
    struct Collect final : ColumnSink {
        std::vector<std::int64_t> values;
        std::size_t calls = 0;
        std::size_t next = 3;
        bool contiguous = true;
        void cells(std::size_t firstRow, std::span<Cell const> cells) override {
            contiguous = contiguous && firstRow == next;
            next = firstRow + cells.size();
            ++calls;
            for (auto const& cell : cells) {
                values.push_back(std::get<std::int64_t>(cell));
            }
        }
    } sink;
    source->snapshot()->readColumn(0, sink, 3, TableSnapshot::kChunkRows + 5);
    CHECK(sink.contiguous);
    CHECK(sink.calls == 2);
    REQUIRE(sink.values.size() == TableSnapshot::kChunkRows + 2);
    CHECK(sink.values.front() == 3);
    CHECK(std::cmp_equal(sink.values.back(), TableSnapshot::kChunkRows + 4));
}

TEST_CASE("table: insert and remove keep keys and cells aligned across chunks", "[table][source]") {
    auto const source = tabletest::makeSource(twoColumns(), numberedRows(TableSnapshot::kChunkRows + 2));
    source->insertRow(1, RowId{std::string{"new"}}, {Cell{std::int64_t{-1}}, Cell{std::string{"inserted"}}});
    auto snap = source->snapshot();
    REQUIRE(snap->rowCount() == TableSnapshot::kChunkRows + 3);
    CHECK(std::get<std::string>(snap->rowId(1)) == "new");
    CHECK(std::get<std::int64_t>(snap->cell(2, 0)) == 1);
    CHECK(std::cmp_equal(std::get<std::int64_t>(snap->cell(snap->rowCount() - 1, 0)), TableSnapshot::kChunkRows + 1));
    CHECK(source->indexOf(RowId{std::string{"new"}}) == 1U);

    source->removeRow(0);
    snap = source->snapshot();
    CHECK(std::get<std::string>(snap->rowId(0)) == "new");
    CHECK(source->indexOf(RowId{std::int64_t{0}}) == std::nullopt);
    CHECK(source->indexOf(RowId{std::int64_t{1}}) == 1U);
}

TEST_CASE("table: listeners hear every kind of change, and patchRow finds rows by key", "[table][source]") {
    auto const source = tabletest::makeSource(twoColumns(), numberedRows(4));
    RecordingListener listener;
    source->subscribe(listener);
    source->updateRow(1, {Cell{std::int64_t{10}}, Cell{std::string{"x"}}});
    source->insertRow(4, RowId{std::int64_t{99}}, {Cell{std::int64_t{99}}, Cell{}});
    source->removeRow(0);
    CHECK(source->patchRow(RowId{std::int64_t{2}}, {Cell{std::int64_t{20}}, Cell{}}));
    CHECK_FALSE(source->patchRow(RowId{std::int64_t{1234}}, {}));
    source->setRows({}, {});
    source->unsubscribe(listener);
    source->setRows({}, {});

    REQUIRE(listener.changes.size() == 5);
    CHECK(listener.changes[0].kind == ChangeKind::Updated);
    CHECK(listener.changes[0].rows == std::vector<std::size_t>{1});
    CHECK(listener.changes[1].kind == ChangeKind::Inserted);
    CHECK(listener.changes[2].kind == ChangeKind::Removed);
    CHECK(listener.changes[3].kind == ChangeKind::Updated);
    CHECK(listener.changes[3].rows == std::vector<std::size_t>{1});  // key 2 sits at index 1 after the removal
    CHECK(listener.changes[4].kind == ChangeKind::Reset);
}

TEST_CASE("table: the default collator folds case and keeps code-point order", "[table][source]") {
    CodePointCollator const collator;
    CHECK(collator.fold("pH Value") == "ph value");
    CHECK(collator.fold("\xC3\x89TAGE") == "\xC3\xA9tage");  // ÉTAGE -> étage
    CHECK(collator.fold("\xC3\x97") == "\xC3\x97");          // × has no lower case
    CHECK(collator.sortKey("B") < collator.sortKey("c"));
    CHECK(collator.sortKey("z") < collator.sortKey("\xC3\xA9"));  // code-point order: é after z
    CHECK(collator.cloneForTask() == nullptr);
}

TEST_CASE("table: the default date parser reads ISO 8601 and nothing else", "[table][source]") {
    IsoDateParser const parser;
    CHECK(parser.parseDate("1970-01-02", {}) == 1);
    CHECK(parser.parseDate("2026-10-08", {}) == 20734);
    CHECK(parser.parseDate("2026-02-30", {}) == std::nullopt);
    CHECK(parser.parseDate("08.10.2026", {}) == std::nullopt);
    CHECK(parser.parseDate("", {}) == std::nullopt);
    CHECK(parser.parseDateTime("1970-01-01T00:01:05Z", {}) == 65);
    CHECK(parser.parseDateTime("1970-01-02", {}) == 86400);
    CHECK(parser.parseDateTime("yesterday", {}) == std::nullopt);
}

TEST_CASE("table: parseDecimal is exact and strict", "[table][source]") {
    using morph::math::Denominator;
    using morph::math::Numerator;
    auto const half = parseDecimal("1.5");
    REQUIRE(half.has_value());
    CHECK(half->numerator == 3);
    CHECK(half->denominator == 2);
    CHECK(half->getDecimalPlaces().value == 1);
    CHECK(parseDecimal("-0.25").value() == morph::math::Rational{Numerator{-1}, Denominator{4}, {}});
    CHECK(parseDecimal("+7").value() == morph::math::Rational{7, {}});
    CHECK(parseDecimal("9223372036854775807").has_value());
    CHECK(parseDecimal("-9223372036854775808").has_value());
    CHECK_FALSE(parseDecimal("9223372036854775808").has_value());
    CHECK(parseDecimal("0.123456789012345678").has_value());
    CHECK_FALSE(parseDecimal("0.1234567890123456789").has_value());
    CHECK_FALSE(parseDecimal("1e3").has_value());
    CHECK_FALSE(parseDecimal("").has_value());
    CHECK_FALSE(parseDecimal("-").has_value());
    CHECK_FALSE(parseDecimal("1.2.3").has_value());
    CHECK_FALSE(parseDecimal(" 1").has_value());
    CHECK(parseDecimal("x").error().code == TableErrorCode::InvalidValue);
    CHECK(dec("0.1") + dec("0.2") == dec("0.3"));
}

TEST_CASE("table: displayText renders every cell alternative", "[table][source]") {
    CHECK(displayText(Cell{}).empty());
    CHECK(displayText(Cell{std::int64_t{-3}}) == "-3");
    CHECK(displayText(Cell{true}) == "true");
    CHECK(displayText(Cell{std::string{"x"}}) == "x");
    CHECK(displayText(Cell{QuantityCell{.amount = dec("2"), .toCanonical = dec("1000")}}) == "2");
    CHECK(displayText(Cell{2.5}) == "2.5");
}

// Mutation: displayText formatting a Rational with `std::format("{}")`, which
// writes the exact fraction: "3/2".
TEST_CASE("table: displayText writes a decimal as a decimal", "[table][source]") {
    CHECK(displayText(Cell{dec("1.5")}) == "1.5");
    CHECK(displayText(Cell{dec("-12.50")}) == "-12.5");
    CHECK(displayText(Cell{QuantityCell{.amount = dec("0.25"), .toCanonical = dec("1000")}}) == "0.25");
}

// Mutation: parseDecimal building every value through the canonicalising
// constructor, which clamps INT64_MIN to -INT64_MAX.
TEST_CASE("table: parseDecimal keeps the most negative integer", "[table][source]") {
    auto const value = parseDecimal("-9223372036854775808");
    REQUIRE(value.has_value());
    CHECK(value->numerator == std::numeric_limits<std::int64_t>::min());
    CHECK(value->denominator == 1);
    CHECK_FALSE(parseDecimal("-922337203685477580.8").has_value());
}

namespace {

template <typename T>
constexpr bool kHasGlazeMeta = requires { glz::meta<T>::value; };

}  // namespace

// Mutation: glz::meta<TableErrorCode> declared in query.hpp, which this file
// does not include: the code would be written as a number here and by name
// where query.hpp is included, two definitions of one template.
TEST_CASE("table: the error code's JSON names come with the type", "[table][source]") {
    CHECK(kHasGlazeMeta<TableErrorCode>);
    CHECK(kHasGlazeMeta<ColumnKind>);
}
