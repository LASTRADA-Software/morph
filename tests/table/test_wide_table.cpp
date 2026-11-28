// SPDX-License-Identifier: Apache-2.0
//
// The wide-table fixture (docs/spec/table/table.md, "Performance", "The fixture"): its layout, its generator, the
// columns `apply` reads from it, and an edit's copy-on-write. Each case names the mutation it was seen to fail on.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <morph/table/query.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"
#include "table/wide_table_model.hpp"

using morph::table::Cell;
using morph::table::QuantityCell;

namespace {

// FNV-1a 64 of seed 7's first 1,000 rows by 300 columns, as `digestForTesting` takes it.
constexpr std::uint64_t kPinnedDigest = 0xe7998de91954d00aULL;

// A source that hands out another source's snapshot, counting the cells read from it per column through both
// cell() and readColumn().
class RecordingSource final : public morph::table::DataSource {
public:
    explicit RecordingSource(morph::table::DataSource& inner)
        : _inner{&inner}, _counts{std::make_shared<std::vector<std::size_t>>(inner.columns().size(), 0)} {}

    [[nodiscard]] std::span<morph::table::ColumnInfo const> columns() const override { return _inner->columns(); }
    [[nodiscard]] std::shared_ptr<morph::table::RowSnapshot const> snapshot() const override {
        return std::make_shared<Recording const>(_inner->snapshot(), _counts);
    }
    void subscribe(morph::table::ChangeListener& listener) override { _inner->subscribe(listener); }
    void unsubscribe(morph::table::ChangeListener& listener) override { _inner->unsubscribe(listener); }
    [[nodiscard]] std::vector<std::size_t> const& counts() const { return *_counts; }

private:
    class Recording final : public morph::table::RowSnapshot {
    public:
        Recording(std::shared_ptr<morph::table::RowSnapshot const> inner,
                  std::shared_ptr<std::vector<std::size_t>> counts)
            : _inner{std::move(inner)}, _counts{std::move(counts)} {}
        [[nodiscard]] std::size_t rowCount() const override { return _inner->rowCount(); }
        [[nodiscard]] morph::table::RowId rowId(std::size_t row) const override { return _inner->rowId(row); }
        [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override {
            ++_counts->at(column);
            return _inner->cell(row, column);
        }
        void readColumn(std::size_t column, morph::table::ColumnSink& sink, std::size_t first,
                        std::size_t last) const override {
            _counts->at(column) += last - first;
            _inner->readColumn(column, sink, first, last);
        }

    private:
        std::shared_ptr<morph::table::RowSnapshot const> _inner;
        std::shared_ptr<std::vector<std::size_t>> _counts;
    };

    morph::table::DataSource* _inner;
    std::shared_ptr<std::vector<std::size_t>> _counts;
};

class RecordingListener final : public morph::table::ChangeListener {
public:
    void rowsChanged(morph::table::RowChange const& change) override { changes.push_back(change); }
    std::vector<morph::table::RowChange> changes;
};

[[nodiscard]] std::size_t columnIndex(bench::WideTableSource const& source, std::string_view id) {
    auto const columns = source.columns();
    auto const at = std::ranges::find(columns, id, &morph::table::ColumnInfo::id);
    REQUIRE(at != columns.end());
    return static_cast<std::size_t>(at - columns.begin());
}

}  // namespace

TEST_CASE("wide table: the same seed gives the same cells", "[table][wide]") {
    bench::WideTableSource const a{{.rows = 1'000, .columns = 300, .seed = 7}};
    bench::WideTableSource const b{{.rows = 1'000, .columns = 300, .seed = 7}};
    auto const sa = a.snapshot();
    auto const sb = b.snapshot();
    REQUIRE(sa->rowCount() == 1'000);
    for (std::size_t r = 0; r < 1'000; ++r) {
        REQUIRE(sa->rowId(r) == sb->rowId(r));
        for (std::size_t c = 0; c < 300; ++c) {
            REQUIRE(tabletest::sameCell(sa->cell(r, c), sb->cell(r, c)));  // Mutation: seed from std::random_device
        }
    }
    bench::WideTableSource const other{{.rows = 1'000, .columns = 300, .seed = 8}};
    CHECK_FALSE(other.snapshot()->cell(0, 1) == sa->cell(0, 1));  // Mutation: generator ignoring the seed
}

TEST_CASE("wide table: the column mix is the spec's", "[table][wide]") {
    auto const roles = [](int n) {
        std::map<bench::WideRole, int> count;
        for (auto const& c : bench::wideColumns(n)) {
            ++count[c.role];
        }
        return count;
    };
    auto const wide = roles(300);
    CHECK(wide.at(bench::WideRole::Key) + wide.at(bench::WideRole::Integer) == 100);
    CHECK(wide.at(bench::WideRole::Decimal) == 60);
    CHECK(wide.at(bench::WideRole::Quantity) == 30);
    CHECK(wide.at(bench::WideRole::Measure) == 10);
    CHECK(wide.at(bench::WideRole::Text) + wide.at(bench::WideRole::LongText) == 70);
    CHECK(wide.at(bench::WideRole::LongText) == 1);
    CHECK(wide.at(bench::WideRole::Date) + wide.at(bench::WideRole::DateTime) == 20);
    CHECK(wide.at(bench::WideRole::Bool) == 10);
    auto const columns = bench::wideColumns(300);
    CHECK(std::ranges::count_if(columns, &bench::WideColumn::sparse) == 25);
    CHECK(std::ranges::count(columns, std::string{"recipe"}, &bench::WideColumn::level) == 20);
    CHECK(std::ranges::count(columns, std::string{"g3"}, &bench::WideColumn::group) == 3);
    CHECK(std::ranges::count(columns, std::string{"g16"}, &bench::WideColumn::group) == 16);
    CHECK(bench::wideColumns(30).size() == 30);
    // One column in twelve has 10% empty cells: 1,000 rows of a sparse column hold between 50 and 150 empties.
    bench::WideTableSource const source{{.rows = 1'000, .columns = 300, .seed = 7}};
    auto const snap = source.snapshot();
    auto const empties = [&](std::size_t c) {
        int n = 0;
        for (std::size_t r = 0; r < 1'000; ++r) {
            n += std::holds_alternative<std::monostate>(snap->cell(r, c)) ? 1 : 0;
        }
        return n;
    };
    CHECK(empties(11) > 50);  // position 11 is sparse
    CHECK(empties(11) < 150);
    CHECK(empties(0) == 0);  // `id` is never empty
    // Mutation: sparse every 11th column instead of every 12th.
}

TEST_CASE("wide table: shared rows agree between 300 and 30 columns", "[table][wide]") {
    bench::WideTableSource const wide{{.rows = 2'000, .columns = 300, .seed = 7}};
    bench::WideTableSource const small{{.rows = 1'000, .columns = 30, .seed = 7}};
    auto const ws = wide.snapshot();
    auto const ss = small.snapshot();
    auto const wideIds = wide.columns();
    int empties = 0;
    for (std::size_t c = 0; c < small.columns().size(); ++c) {
        auto const at = std::ranges::find(wideIds, small.columns()[c].id, &morph::table::ColumnInfo::id);
        REQUIRE(at != wideIds.end());
        auto const w = static_cast<std::size_t>(at - wideIds.begin());
        CHECK(small.layout()[c].sparse == wide.layout()[w].sparse);
        for (std::size_t r = 0; r < 1'000; ++r) {
            REQUIRE(tabletest::sameCell(ss->cell(r, c), ws->cell(r, w)));
            empties += std::holds_alternative<std::monostate>(ss->cell(r, c)) ? 1 : 0;
        }
    }
    CHECK(empties > 0);  // `t003` is sparse in both layouts, so empty cells were compared
    // Mutation: wideColumns(30) recomputing `sparse` from the 30 layout's positions.
    // Mutation: hashing the column's position instead of its id.
}

TEST_CASE("table fixtures: sameCell tells places apart and operator== does not", "[table][wide]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;
    Cell const two{Rational{Numerator{5}, Denominator{2}, DecimalPlaces{2}}};
    Cell const four{Rational{Numerator{5}, Denominator{2}, DecimalPlaces{4}}};
    CHECK(two == four);
    CHECK_FALSE(tabletest::sameCell(two, four));  // Mutation: sameCell delegating to operator==
    CHECK(tabletest::sameCell(two, two));
    Cell const kg{QuantityCell{.amount = std::get<Rational>(two),
                               .toCanonical = Rational{Numerator{1000}, Denominator{1}, DecimalPlaces{0}}}};
    Cell const g{QuantityCell{.amount = std::get<Rational>(two),
                              .toCanonical = Rational{Numerator{1}, Denominator{1}, DecimalPlaces{0}}}};
    CHECK(kg == kg);
    CHECK_FALSE(kg == g);  // Mutation: QuantityCell::operator== comparing `amount` only
    CHECK_FALSE(tabletest::sameCell(two, Cell{std::int64_t{2}}));
}

TEST_CASE("wide table: seed 7 has the pinned digest", "[table][wide]") {
    // FNV-1a 64 over displayText(cell) of rows 0..999, columns 0..299, each followed by '\x1f'. Pinned on MSVC; the
    // Linux legs and Node check it unchanged, so a generator that depends on the standard library fails there.
    CHECK(bench::digestForTesting(bench::WideTableSource{{.rows = 1'000, .columns = 300, .seed = 7}}) ==
          kPinnedDigest);
    // Mutation: a std::uniform_int_distribution in the generator (passes on one standard library, fails on another).
}

TEST_CASE("wide table: apply reads only the sorted and filtered columns", "[table][wide]") {
    bench::WideTableSource wide{{.rows = 1'000, .columns = 300, .seed = 7}};
    RecordingSource const recording{wide};
    morph::table::TableQuery query;
    query.sort = {{.column = "i001", .dir = morph::table::SortDirection::Ascending}};
    query.filters.columns.emplace(
        "t001", morph::table::ColumnFilter{
                    .combine = morph::table::Combine::Any, .include = {{.contains = "a"}}, .exclude = {}});
    query.page = {.offset = 0, .limit = 200};
    morph::table::ApplyOptions options;
    options.services.units = wide.units();
    auto const page = morph::table::apply(recording, query, options);
    REQUIRE(page.has_value());
    REQUIRE(page->rows.size() == 200);
    auto const i001 = columnIndex(wide, "i001");
    auto const t001 = columnIndex(wide, "t001");
    auto const& counts = recording.counts();
    CHECK(counts.at(i001) == 1'000);
    CHECK(counts.at(t001) == 1'000);
    std::size_t others = 0;
    for (std::size_t c = 0; c < counts.size(); ++c) {
        others += c == i001 || c == t001 ? 0 : counts[c];
    }
    CHECK(others == 0);
    // Mutation: apply listing every column in JobInput::needed.
}

TEST_CASE("wide table: an edit changes one cell and copies one column", "[table][wide]") {
    bench::WideTableSource source{{.rows = 1'000, .columns = 300, .seed = 7}};
    RecordingListener listener;
    source.subscribe(listener);
    auto const i001 = columnIndex(source, "i001");
    auto const before = source.snapshot();
    Cell const old = before->cell(5, i001);
    REQUIRE_FALSE(old == Cell{std::int64_t{42}});
    source.setCell(5, "i001", Cell{std::int64_t{42}});
    CHECK(source.snapshot()->cell(5, i001) == Cell{std::int64_t{42}});
    CHECK(tabletest::sameCell(before->cell(5, i001), old));  // Mutation: setCell writing into the shared array
    CHECK(tabletest::sameCell(source.snapshot()->cell(6, i001), before->cell(6, i001)));
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes[0].kind == morph::table::ChangeKind::Updated);
    CHECK(listener.changes[0].rows == std::vector<std::size_t>{5});
    source.unsubscribe(listener);
}

TEST_CASE("wide table: a text edit shifts the rows after it and copies the column", "[table][wide]") {
    bench::WideTableSource source{{.rows = 1'000, .columns = 300, .seed = 7}};
    auto const t001 = columnIndex(source, "t001");
    auto const before = source.snapshot();
    Cell const oldFive = before->cell(5, t001);
    Cell const oldSix = before->cell(6, t001);
    Cell const oldLast = before->cell(999, t001);
    // A text of another length than the one it replaces, so every later row's offset moves.
    std::string const replacement = "ÃditÃ©";
    REQUIRE(std::get<std::string>(oldFive).size() != replacement.size());
    source.setCell(5, "t001", Cell{replacement});
    auto const after = source.snapshot();
    CHECK(tabletest::sameCell(after->cell(5, t001), Cell{replacement}));
    CHECK(tabletest::sameCell(after->cell(6, t001), oldSix));  // Mutation: setCell leaving the later offsets
    CHECK(tabletest::sameCell(after->cell(999, t001), oldLast));
    CHECK(tabletest::sameCell(before->cell(5, t001), oldFive));
    CHECK(tabletest::sameCell(before->cell(6, t001), oldSix));
    source.setCell(6, "t001", Cell{});
    CHECK(std::holds_alternative<std::monostate>(source.snapshot()->cell(6, t001)));
    CHECK(tabletest::sameCell(source.snapshot()->cell(5, t001), Cell{replacement}));
}

TEST_CASE("wide table: a decimal edit is kept at the column's places, and one that does not fit is refused",
          "[table][wide]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;
    bench::WideTableSource source{{.rows = 1'000, .columns = 300, .seed = 7}};
    auto const d000 = columnIndex(source, "d000");
    // 1.5 at one place is stored at the column's two: it reads back as 1.50.
    // Mutation: storing the numerator unscaled.
    source.setCell(5, "d000", Cell{Rational{Numerator{3}, Denominator{2}, DecimalPlaces{1}}});
    CHECK(tabletest::sameCell(source.snapshot()->cell(5, d000),
                              Cell{Rational{Numerator{150}, Denominator{100}, DecimalPlaces{2}}}));
    // A third has no exact value at two places.
    // Mutation: scaling without checking that the denominator divides the scale.
    auto const unchanged = source.snapshot()->cell(5, d000);
    CHECK_THROWS_AS(source.setCell(5, "d000", Cell{Rational{Numerator{1}, Denominator{3}, DecimalPlaces{2}}}),
                    std::invalid_argument);
    CHECK(tabletest::sameCell(source.snapshot()->cell(5, d000), unchanged));
}
