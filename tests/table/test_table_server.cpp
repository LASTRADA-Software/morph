// SPDX-License-Identifier: Apache-2.0
//
// morph::table's server side and its client window (spec 7 §4, §9): typed rows
// read through glaze's own view of the row, table::apply under concurrent use
// of one collator, and PageWindow against late and short replies.
//
// Each case names the mutation it was seen to fail on.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
#include <memory>
#include <morph/forms/choice.hpp>
#include <morph/forms/widget_hints.hpp>
#include <morph/table/query.hpp>
#include <morph/util/tagged.hpp>
#include <optional>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

// Reflected by glaze, so not in an anonymous namespace.
namespace tserver {

// glaze writes `id`, `analysis`, `ph`: the note stays off the wire.
struct NoteRow {
    std::int64_t id = 0;
    std::string internalNote;
    std::string analysis;
    double ph = 0;
};

// glaze writes `qty` before `name`.
struct ReorderedRow {
    std::string name;
    std::int64_t qty = 0;
};

struct WrappedRow {
    std::int64_t id = 0;
    morph::forms::Ranged<0, 1000> score;
    morph::util::Tagged<std::int64_t, "Version"> version;
    morph::forms::Choice<std::string, "ListCodes"> pick;
    std::uint64_t big = 0;
};

struct CodeRow {
    std::int64_t id = 0;
    std::int64_t code = 0;
};

}  // namespace tserver

template <>
struct glz::meta<tserver::NoteRow> {
    using T = tserver::NoteRow;
    static constexpr auto value = glz::object("id", &T::id, "analysis", &T::analysis, "ph", &T::ph);
};

template <>
struct glz::meta<tserver::ReorderedRow> {
    using T = tserver::ReorderedRow;
    static constexpr auto value = glz::object("qty", &T::qty, "name", &T::name);
};

using namespace morph::table;

namespace {

std::string text(Cell const& cell) { return displayText(cell); }

}  // namespace

// Mutation: detail::cellReaders reading `glz::get<I>(glz::to_tie(row))`, the
// I-th declared member, under glaze's I-th key: `analysis` shows the note, and
// a filter on it reads the hidden member.
TEST_CASE("table: typed rows read the member glaze writes under each key", "[table][server]") {
    using tserver::NoteRow;
    std::vector<NoteRow> rows{{1, "secret-z", "Arsenic", 7.5}, {2, "secret-a", "Zinc", 3.25}};
    RowKey<NoteRow> const key = [](NoteRow const& row) { return RowId{row.id}; };
    RowsSource<NoteRow> const source{std::make_shared<std::vector<NoteRow> const>(rows), key};
    REQUIRE(source.columns().size() == 3);
    CHECK(source.columns()[1].id == "analysis");
    CHECK(source.columns()[2].kind == ColumnKind::Number);
    CHECK(text(source.snapshot()->cell(0, 1)) == "Arsenic");

    TableQuery probe;
    probe.filters.columns["analysis"].include.push_back(FilterEntry{.startsWith = std::string{"secret"}});
    auto const page = apply(rows, key, probe);
    REQUIRE(page.has_value());
    CHECK(page->rows.empty());

    using tserver::ReorderedRow;
    std::vector<ReorderedRow> reordered{{"bolt", 4}, {"axle", 30}};
    RowsSource<ReorderedRow> const second{std::make_shared<std::vector<ReorderedRow> const>(reordered),
                                          [](ReorderedRow const& row) { return RowId{row.name}; }};
    REQUIRE(second.columns().size() == 2);
    CHECK(second.columns()[0].id == "qty");
    CHECK(second.columns()[0].kind == ColumnKind::Integer);
    CHECK(text(second.snapshot()->cell(0, 0)) == "4");
}

// Mutation: detail::kindOf and detail::toCell without their `MemberWrapper`
// branches: the wrappers are text written as JSON, so an empty one reads
// "null" and numbers sort as text. And toCell casting every unsigned value to
// int64: 2^63 + 5 wraps negative and sorts first.
TEST_CASE("table: wrapped and unsigned members keep their kind and value", "[table][server]") {
    using tserver::WrappedRow;
    std::vector<WrappedRow> rows(3);
    rows[0].id = 0;
    rows[0].score = 900;
    rows[0].version.value = 12;
    rows[0].pick = std::string{"apple"};
    rows[0].big = 7;
    rows[1].id = 1;  // score and pick empty
    rows[1].version.value = 3;
    rows[1].big = (std::uint64_t{1} << 63U) + 5U;
    rows[2].id = 2;
    rows[2].score = 80;
    rows[2].version.value = 100;
    rows[2].big = 1;
    RowKey<WrappedRow> const key = [](WrappedRow const& row) { return RowId{row.id}; };
    RowsSource<WrappedRow> const source{std::make_shared<std::vector<WrappedRow> const>(rows), key};
    auto const columns = source.columns();
    CHECK(columns[1].kind == ColumnKind::Integer);
    CHECK(columns[2].kind == ColumnKind::Integer);
    CHECK(columns[3].kind == ColumnKind::Text);
    auto const snapshot = source.snapshot();
    CHECK(std::holds_alternative<std::monostate>(snapshot->cell(1, 1)));
    CHECK(std::holds_alternative<std::monostate>(snapshot->cell(1, 3)));
    CHECK(text(snapshot->cell(0, 3)) == "apple");

    auto const sortedBy = [&](std::string column) {
        TableQuery query;
        query.sort.push_back(SortKey{.column = std::move(column), .dir = SortDirection::Ascending});
        auto const page = apply(rows, key, query);
        REQUIRE(page.has_value());
        std::vector<std::int64_t> ids;
        for (auto const& row : page->rows) {
            ids.push_back(row.id);
        }
        return ids;
    };
    CHECK(sortedBy("score") == std::vector<std::int64_t>{2, 0, 1});    // 80 < 900; empty last
    CHECK(sortedBy("version") == std::vector<std::int64_t>{1, 0, 2});  // numerically, not as text
    CHECK(sortedBy("big") == std::vector<std::int64_t>{2, 0, 1});      // past INT64_MAX is invalid: last
}

// Mutation: RowsSource ignoring its `comparators` map: the column stays an
// integer and sorts ascending.
TEST_CASE("table: typed rows name a comparator for a custom column", "[table][server]") {
    using tserver::CodeRow;
    std::vector<CodeRow> rows{{1, 10}, {2, 30}, {3, 20}};
    ApplyOptions options;
    options.services.comparators.emplace("reverse", [](Cell const& left, Cell const& right) {
        return std::get<std::int64_t>(right) <=> std::get<std::int64_t>(left);
    });
    TableQuery query;
    query.sort.push_back(SortKey{.column = "code", .dir = SortDirection::Ascending});
    auto const page = apply<CodeRow>(rows, [](CodeRow const& row) { return RowId{row.id}; }, query, options, {},
                                     {{"code", "reverse"}});
    REQUIRE(page.has_value());
    REQUIRE(page->rows.size() == 3);
    CHECK(page->rows[0].id == 2);
    CHECK(page->rows[2].id == 1);
}

// Mutation: table::apply slicing with `std::min(begin + limit, count)`: with
// `maxLimit` at INT64_MAX the sum overflows and the page comes back empty.
TEST_CASE("table: a page limit as large as int64 allows returns the rest", "[table][server]") {
    auto const source = tabletest::column(ColumnKind::Integer, {Cell{std::int64_t{1}}, Cell{std::int64_t{2}},
                                                                Cell{std::int64_t{3}}, Cell{std::int64_t{4}}});
    ApplyOptions options;
    options.limits.maxLimit = std::numeric_limits<std::int64_t>::max();
    TableQuery query;
    query.page = PageRequest{.offset = 1, .limit = std::numeric_limits<std::int64_t>::max()};
    auto const page = apply(*source, query, options);
    REQUIRE(page.has_value());
    CHECK(page->rows == std::vector<std::size_t>{1, 2, 3});
}

namespace {

// Folds by lowering ASCII; counts the folds made on the instance it was built
// as, and hands each task a fresh instance, as a collator that is not safe to
// share must.
class CloningCollator final : public TextCollator {
public:
    CloningCollator(std::shared_ptr<int> originalFolds, bool original)
        : _folds{std::move(originalFolds)}, _original{original} {}
    [[nodiscard]] std::string sortKey(std::string_view value) const override { return fold(value); }
    [[nodiscard]] std::string fold(std::string_view value) const override {
        if (_original) {
            ++*_folds;
        }
        std::string out{value};
        for (auto& character : out) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        return out;
    }
    [[nodiscard]] std::shared_ptr<TextCollator const> cloneForTask() const override {
        return std::make_shared<CloningCollator>(_folds, false);
    }

private:
    std::shared_ptr<int> _folds;
    bool _original;
};

}  // namespace

// Mutation: table::apply compiling the filter with `options.services` rather
// than its per-call copy: the caller's collator, which concurrent calls share,
// folds the filter's operand.
TEST_CASE("table: apply folds on its own copy of a collator that is not safe to share", "[table][server]") {
    auto const source = tabletest::column(
        ColumnKind::Text, {Cell{std::string{"Alpha"}}, Cell{std::string{"beta"}}, Cell{std::string{"ALPS"}}});
    auto const originalFolds = std::make_shared<int>(0);
    ApplyOptions options;
    options.services.collator = std::make_shared<CloningCollator>(originalFolds, true);
    TableQuery query;
    query.filters.columns["c"].include.push_back(FilterEntry{.startsWith = std::string{"AL"}});
    auto const page = apply(*source, query, options);
    REQUIRE(page.has_value());
    CHECK(page->rows == std::vector<std::size_t>{0, 2});
    CHECK(*originalFolds == 0);
}

// Mutation: PageWindow::accept ignoring `fetch.query`: the late page of the
// old query is stored as the new query's, its total sticks, and its failure
// clears the new request's in-flight mark.
TEST_CASE("table: the window ignores replies to a query it was reset from", "[table][server]") {
    PageWindow<int> window{10, 4};
    auto const old = window.visible(0, 5);
    REQUIRE(old.size() == 2);
    window.reset();
    auto const current = window.visible(0, 5);
    REQUIRE(current.size() == 2);
    CHECK(current[0].query != old[0].query);

    CHECK_FALSE(window.accept(old[0], Page<int>{.rows = std::vector<int>(10, 1), .total = 999, .offset = 0}));
    CHECK(window.row(0) == nullptr);
    CHECK_FALSE(window.total().has_value());
    CHECK_FALSE(window.fail(old[1], TableError{.code = TableErrorCode::Unavailable, .column = {}, .message = "x"}));
    CHECK_FALSE(window.error().has_value());
    CHECK(window.visible(0, 5).empty());  // both pages of the new query are still in flight

    CHECK(window.accept(current[0], Page<int>{.rows = std::vector<int>(10, 2), .total = 15, .offset = 0}));
    REQUIRE(window.row(0) != nullptr);
    CHECK(*window.row(0) == 2);
    CHECK(window.total() == 15);
}

// Mutation: PageWindow treating a held page as complete whatever its size (no
// `_complete` set): the rows past a clamped reply are never requested.
TEST_CASE("table: the window fetches the rest of a page a server cut short", "[table][server]") {
    using Row = tserver::CodeRow;
    std::vector<Row> rows;
    for (std::int64_t i = 0; i < 300; ++i) {
        rows.push_back(Row{.id = i, .code = 0});
    }
    ApplyOptions options;
    options.limits.maxLimit = 40;  // under the window's page size
    RowKey<Row> const key = [](Row const& row) { return RowId{row.id}; };
    PageWindow<Row> window{100, 8};
    for (int round = 0; round < 20; ++round) {
        auto const fetches = window.visible(0, 150);
        if (fetches.empty()) {
            break;
        }
        for (auto const& fetch : fetches) {
            TableQuery query;
            query.page = fetch.request;
            auto page = apply(rows, key, query, options);
            REQUIRE(page.has_value());
            CHECK(page->rows.size() <= 40U);
            REQUIRE(window.accept(fetch, std::move(*page)));
        }
    }
    for (std::int64_t index = 0; index < 300; ++index) {
        INFO("view row " << index);
        REQUIRE(window.row(index) != nullptr);
        CHECK(window.row(index)->id == index);
    }
    CHECK_FALSE(window.pending());
}

// Mutation: PageWindow::accept assigning `page.total` as it comes (`_total =
// page.total`): a later page that does not count forgets the known total, and
// pages past the end are requested again.
TEST_CASE("table: the window keeps a known total when a reply omits it", "[table][server]") {
    PageWindow<int> window{10, 8};
    auto const first = window.visible(0, 5);
    REQUIRE(first.size() == 2);
    REQUIRE(window.accept(first[0], Page<int>{.rows = std::vector<int>(10, 0), .total = 20, .offset = 0}));
    REQUIRE(window.accept(first[1], Page<int>{.rows = std::vector<int>(10, 1), .total = std::nullopt, .offset = 10}));
    CHECK(window.total() == 20);
    CHECK(window.visible(15, 5).empty());
}
