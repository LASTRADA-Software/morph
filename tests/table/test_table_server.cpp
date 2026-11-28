// SPDX-License-Identifier: Apache-2.0
//
// morph::table's server side and its client window (spec 7 §4, §9): typed rows
// read through glaze's own view of the row, table::apply under concurrent use
// of one collator, PageWindow against late and short replies, and the window
// under changes: keyed refreshes, probes, patches, revisions and columns.
//
// Each case names the mutation it was seen to fail on.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <initializer_list>
#include <limits>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/records.hpp>
#include <morph/forms/choice.hpp>
#include <morph/forms/widget_hints.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/util/tagged.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
    std::vector<NoteRow> const rows{{.id = 1, .internalNote = "secret-z", .analysis = "Arsenic", .ph = 7.5},
                                    {.id = 2, .internalNote = "secret-a", .analysis = "Zinc", .ph = 3.25}};
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
    std::vector<ReorderedRow> const reordered{{.name = "bolt", .qty = 4}, {.name = "axle", .qty = 30}};
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
    std::vector<CodeRow> const rows{{.id = 1, .code = 10}, {.id = 2, .code = 30}, {.id = 3, .code = 20}};
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
    rows.reserve(300);
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

// ── The window under changes ────────────────────────────────────────────────

namespace {

using Win = PageWindow<GenericRow>;

std::vector<RowId> ids(std::initializer_list<std::int64_t> keys) {
    std::vector<RowId> out;
    for (auto const key : keys) {
        out.emplace_back(key);
    }
    return out;
}

std::vector<std::int64_t> range(std::int64_t first, std::int64_t count) {
    std::vector<std::int64_t> out;
    for (std::int64_t at = 0; at < count; ++at) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.push_back(first + at);
    }
    return out;
}

// One integer cell per column, the key's value.
GenericPage pageOf(
    std::int64_t offset, std::vector<std::int64_t> const& keys, std::vector<std::string> const& columns,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `pageOf`'s `total` and `undecided` are row counts that every call spells out as a pair; a swap would change the page's counts the case asserts on
    std::int64_t total, std::int64_t undecided = 0) {
    GenericPage page;
    page.columns = columns;
    page.offset = offset;
    page.total = total;
    page.undecided = undecided;
    for (auto const key : keys) {
        page.keys.emplace_back(key);
        page.rows.emplace_back(columns.size(), Cell{key});
    }
    return page;
}

// A keyed reply: each pair is a key and its view row.
GenericPage keyedOf(
    std::vector<std::pair<std::int64_t, std::int64_t>> const& keyAtIndex,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `keyedOf`'s `total` and `undecided` are row counts that every call spells out as a pair; a swap would change the page's counts the case asserts on
    std::vector<std::string> const& columns, std::int64_t total, std::int64_t undecided = 0) {
    GenericPage page;
    page.columns = columns;
    page.total = total;
    page.undecided = undecided;
    for (auto const& [key, index] : keyAtIndex) {
        page.keys.emplace_back(key);
        page.rows.emplace_back(columns.size(), Cell{key});
        page.indices.push_back(index);
    }
    return page;
}

std::optional<std::int64_t> intCell(Win const& window, std::int64_t index, std::string_view column) {
    auto const* const cell = window.cell(index, column);
    if (cell == nullptr || !std::holds_alternative<std::int64_t>(*cell)) {
        return std::nullopt;
    }
    return std::get<std::int64_t>(*cell);
}

// Pages 0 and 1 of a view of `total` rows, keys equal to view rows.
void holdTwoPages(Win& window, std::vector<std::string> const& cols, std::int64_t total) {
    auto const fetches = window.visible(0, 5, cols);
    REQUIRE(fetches.size() == 2);
    for (auto const& fetch : fetches) {
        REQUIRE(window.accept(fetch, pageOf(fetch.request.offset, range(fetch.request.offset, 10), cols, total)));
    }
}

}  // namespace

// Mutation: refresh not recording keys while a page fetch is in flight.
TEST_CASE("window: an event that precedes its page reply is owed", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    auto const fetches = window.visible(0, 5, cols);
    REQUIRE(fetches.size() == 2);
    CHECK(window.refresh(ids({3, 4})).kind == Refresh::Nothing);  // nothing held yet
    GenericPage reply = pageOf(0, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, cols, 20);
    reply.unloadedRows = {{.index = 3, .levels = {"recipe"}, .failed = {}},
                          {.index = 4, .levels = {"recipe"}, .failed = {}}};
    bool const accepted = window.accept(fetches[0], std::move(reply));
    REQUIRE(accepted);
    auto const plan = window.owed();
    REQUIRE(plan.kind == Refresh::Rows);
    CHECK(std::ranges::is_permutation(plan.rows.keys, ids({3, 4, 0})));  // the keys and the run's probe
    CHECK(window.unloadedLevels(3).levels.size() == 1);
}

// Mutation: refresh skipping held rows with no unloaded level.
TEST_CASE("window: an ordinary edit on a held row is a keyed fetch, a move invalidates", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    for (auto const& fetch : window.visible(0, 5, cols)) {
        REQUIRE(window.accept(fetch, pageOf(fetch.request.offset,
                                            fetch.request.offset == 0
                                                ? std::vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}
                                                : std::vector<std::int64_t>{10, 11, 12, 13, 14, 15, 16, 17, 18, 19},
                                            cols, 20)));
    }
    auto const plan = window.refresh(ids({7}));
    REQUIRE(plan.kind == Refresh::Rows);
    CHECK(std::ranges::is_permutation(plan.rows.keys, ids({7, 0})));  // one run, rows 0–19: one probe
    auto const replaced = window.patch(plan.rows, keyedOf({{0, 0}, {7, 7}}, cols, 20));
    CHECK(replaced == std::vector<std::int64_t>{0, 7});
    auto const again = window.refresh(ids({7}));
    auto const before = window.revision();
    CHECK(window.patch(again.rows, keyedOf({{0, 0}, {7, 12}}, cols, 20)).empty());
    CHECK(window.revision() == before + 1);
}

// Mutation: accept ignoring every reply whose revision is older than the window's.
TEST_CASE("window: invalidation faster than a round trip advances one reply per trip", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    auto const first = window.visible(0, 5, cols);
    REQUIRE(first.size() == 2);
    REQUIRE(window.accept(first[0], pageOf(0, range(0, 10), cols, 10)));
    CHECK_FALSE(window.pending());  // page 1 lies past the exact total

    window.invalidate();
    auto const f1 = window.visible(0, 5, cols);
    REQUIRE(f1.size() == 1);
    CHECK(f1[0].revision == window.revision());
    window.invalidate();
    CHECK(window.visible(0, 5, cols).empty());  // page 0 is in flight
    REQUIRE(window.accept(f1[0], pageOf(0, range(100, 10), cols, 10)));
    CHECK(intCell(window, 0, "a") == 100);
    auto const next = window.visible(0, 5, cols);  // still stale: the reply was older than the revision
    REQUIRE(next.size() == 1);
    CHECK(next[0].revision == window.revision());
    CHECK(next[0].request == PageRequest{.offset = 0, .limit = 10});
}

// Mutation: a column fetch naming every column.
TEST_CASE("window: a column scrolled in is fetched for held pages and merged by key", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const a{"a"};
    std::vector<std::string> const ab{"a", "b"};
    std::vector<std::string> const b{"b"};
    holdTwoPages(window, a, 20);
    CHECK_FALSE(window.holds(3, "b"));
    auto const more = window.visible(0, 5, ab);
    REQUIRE(more.size() == 2);
    CHECK(more[0].columns == b);
    CHECK(more[1].columns == b);
    CHECK(more[0].request == PageRequest{.offset = 0, .limit = 10});
    CHECK(more[1].request == PageRequest{.offset = 10, .limit = 10});

    auto reply = pageOf(0, range(0, 10), b, 20);
    reply.rows[3][0] = Cell{std::int64_t{33}};
    bool const accepted = window.accept(more[0], std::move(reply));
    REQUIRE(accepted);
    CHECK(intCell(window, 3, "b") == 33);
    CHECK(intCell(window, 3, "a") == 3);

    auto const before = window.revision();
    REQUIRE(window.accept(more[1], pageOf(10, range(20, 10), b, 20)));  // the rows moved between the fetches
    CHECK(window.revision() == before + 1);
    CHECK_FALSE(window.holds(13, "b"));
}

// The columns shown now are kept, so only `a` can go; which column goes first among the ones not shown is held by
// "window: of the columns not shown, the one named longest ago goes first", and its mutation is named there.
TEST_CASE("window: over maxColumns the columns farthest from view go first", "[table][window]") {
    Win window{10, 8, 1000, 2};
    std::vector<std::string> const ab{"a", "b"};
    std::vector<std::string> const bc{"b", "c"};
    auto const first = window.visible(0, 5, ab);
    REQUIRE_FALSE(first.empty());
    REQUIRE(window.accept(first[0], pageOf(0, range(0, 10), ab, 10)));
    auto const more = window.visible(0, 5, bc);
    REQUIRE(more.size() == 1);
    CHECK(more[0].columns == std::vector<std::string>{"c"});
    REQUIRE(window.accept(more[0], pageOf(0, range(0, 10), {"c"}, 10)));
    CHECK(window.holds(0, "c"));
    CHECK(window.holds(0, "b"));
    CHECK_FALSE(window.holds(0, "a"));
}

// Mutation: the totals adopted without marking the other pages stale (page 3's fetch alone).
TEST_CASE("window: a page reply with another total adopts it and marks other pages stale", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    holdTwoPages(window, cols, 30);
    auto const third = window.visible(15, 10, cols);
    REQUIRE(third.size() == 1);
    CHECK(third[0].request == PageRequest{.offset = 20, .limit = 10});
    REQUIRE(window.accept(third[0], pageOf(20, range(20, 10), cols, 35)));
    CHECK(window.total() == 35);
    auto const next = window.visible(15, 10, cols);
    REQUIRE(next.size() == 3);
    CHECK(next[0].request == PageRequest{.offset = 0, .limit = 10});
    CHECK(next[1].request == PageRequest{.offset = 10, .limit = 10});
    CHECK(next[2].request == PageRequest{.offset = 30, .limit = 10});
    for (auto const& fetch : next) {
        CHECK(fetch.columns == cols);
    }
}

// Mutation: an absent total counted as a difference.
TEST_CASE("window: a reply without a total keeps the known one", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    auto const fetches = window.visible(0, 5, cols);
    REQUIRE(fetches.size() == 2);
    REQUIRE(window.accept(fetches[0], pageOf(0, range(0, 10), cols, 20)));
    auto second = pageOf(10, range(10, 10), cols, 0);
    second.total.reset();
    bool const accepted = window.accept(fetches[1], std::move(second));
    REQUIRE(accepted);
    CHECK(window.total() == 20);
    CHECK(window.visible(0, 5, cols).empty());  // page 0 was not marked stale
}

// Mutation: patch keeping the old `b` cells.
TEST_CASE("window: a patch drops the columns its reply lacks", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const ab{"a", "b"};
    std::vector<std::string> const a{"a"};
    auto const first = window.visible(0, 5, ab);
    REQUIRE_FALSE(first.empty());
    REQUIRE(window.accept(first[0], pageOf(0, range(0, 10), ab, 10)));
    auto const plan = window.refresh(ids({3}));
    REQUIRE(plan.kind == Refresh::Rows);
    auto reply = keyedOf({{0, 0}, {3, 3}}, a, 10);
    reply.rows[1][0] = Cell{std::int64_t{30}};
    auto const patched = window.patch(plan.rows, std::move(reply));
    CHECK(patched == std::vector<std::int64_t>{0, 3});
    CHECK(intCell(window, 3, "a") == 30);
    CHECK_FALSE(window.holds(3, "b"));
    auto const refetch = window.visible(0, 5, ab);
    REQUIRE(refetch.size() == 1);
    CHECK(refetch[0].columns == std::vector<std::string>{"b"});
}

// Mutation: no probes (shape ii undetected; the plan lacks the run's first key).
TEST_CASE("window: a row the window never held", "[table][window]") {
    std::vector<std::string> const cols{"a"};
    auto const invalidates = [&](std::initializer_list<std::int64_t> event,
                                 std::vector<std::pair<std::int64_t, std::int64_t>> const& rows, std::int64_t total) {
        Win window{10, 8};
        holdTwoPages(window, cols, 100);
        auto const plan = window.refresh(ids(event));
        REQUIRE(plan.kind == Refresh::Rows);
        CHECK(std::ranges::find(plan.rows.keys, RowId{std::int64_t{0}}) != plan.rows.keys.end());
        auto const before = window.revision();
        auto replaced = window.patch(plan.rows, keyedOf(rows, cols, total));
        return std::pair{window.revision() == before + 1, std::move(replaced)};
    };
    SECTION("an unheld key inside a held page") { CHECK(invalidates({50}, {{0, 0}, {50, 5}}, 100).first); }
    SECTION("the probe moved") { CHECK(invalidates({50}, {{0, 1}}, 100).first); }
    SECTION("another total on the latest fetch") { CHECK(invalidates({50}, {{0, 0}}, 101).first); }
    SECTION("a requested held key missing") { CHECK(invalidates({5}, {{0, 0}}, 100).first); }
    SECTION("an unheld key outside the held pages") {
        auto const [invalidated, replaced] = invalidates({80}, {{0, 0}, {80, 80}}, 100);
        CHECK_FALSE(invalidated);
        CHECK(replaced == std::vector<std::int64_t>{0});
    }
}

// Mutation: no per-key sequence (the older reply is patched over the newer).
TEST_CASE("window: an older keyed reply never overwrites a newer one", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    holdTwoPages(window, cols, 20);
    auto const f1 = window.refresh(ids({7}));
    auto const f2 = window.refresh(ids({7}));
    REQUIRE(f1.kind == Refresh::Rows);
    REQUIRE(f2.kind == Refresh::Rows);
    auto newer = keyedOf({{0, 0}, {7, 7}}, cols, 20);
    newer.rows[1][0] = Cell{std::int64_t{2}};
    auto const patchedNewer = window.patch(f2.rows, std::move(newer));
    CHECK(patchedNewer == std::vector<std::int64_t>{0, 7});
    auto older = keyedOf({{0, 0}, {7, 7}}, cols, 20);
    older.rows[1][0] = Cell{std::int64_t{1}};
    auto const patchedOlder = window.patch(f1.rows, std::move(older));
    CHECK(patchedOlder.empty());
    CHECK(intCell(window, 7, "a") == 2);
    auto const before = window.revision();
    CHECK(window.patch(f1.rows, keyedOf({{0, 0}}, cols, 20)).empty());
    CHECK(window.revision() == before);
}

// Mutation: comparing the totals on every keyed reply.
TEST_CASE("window: the totals of an old keyed reply are not compared", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    holdTwoPages(window, cols, 20);
    auto const f1 = window.refresh(ids({7}));
    auto const f2 = window.refresh(ids({7}));
    REQUIRE(f2.kind == Refresh::Rows);
    auto const before = window.revision();
    static_cast<void>(window.patch(f1.rows, keyedOf({{0, 0}, {7, 7}}, cols, 30)));
    CHECK(window.revision() == before);
}

// Mutation: the key count compared with maxKeys before the probes are added.
TEST_CASE("window: an awaited order or too many keys refetches pages", "[table][window]") {
    std::vector<std::string> const cols{"a"};
    Win awaited{10, 8};
    auto const first = awaited.visible(0, 5, cols);
    REQUIRE_FALSE(first.empty());
    auto page = pageOf(0, range(0, 10), cols, 10);
    page.awaiting = {"recipe"};
    bool const accepted = awaited.accept(first[0], std::move(page));
    REQUIRE(accepted);
    auto const before = awaited.revision();
    CHECK(awaited.refresh(ids({3})).kind == Refresh::Pages);
    CHECK(awaited.revision() == before + 1);

    Win few{10, 8, 3};
    holdTwoPages(few, cols, 20);
    CHECK(few.refresh(ids({3, 4, 5})).kind == Refresh::Pages);  // three keys and one probe
    CHECK(few.refresh(ids({3, 4})).kind == Refresh::Rows);
}

// Mutation: superseded returning every fetch in flight.
TEST_CASE("window: superseded fetches are those left behind", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    auto const first = window.visible(0, 5, cols);
    REQUIRE(first.size() == 2);
    auto const far = window.visible(50'000, 20, cols);
    CHECK(far.size() == 4);
    auto const left = window.superseded();
    REQUIRE(left.size() == 2);
    CHECK(std::ranges::is_permutation(left, first));
    for (auto const& fetch : left) {
        window.cancelled(fetch);
    }
    CHECK_FALSE(window.accept(first[0], pageOf(0, range(0, 10), cols, 100'000)));
    CHECK_FALSE(window.accept(first[1], pageOf(10, range(10, 10), cols, 100'000)));
    CHECK(window.superseded().empty());
}

// Mutation: a typed window without a key function probing (Rows with no keys).
TEST_CASE("window: a typed window without a key function refreshes pages", "[table][window]") {
    PageWindow<int> window{10, 8};
    auto const fetches = window.visible(0, 5);
    REQUIRE_FALSE(fetches.empty());
    REQUIRE(window.accept(fetches[0], Page<int>{.rows = std::vector<int>(10, 1), .total = 10, .offset = 0}));
    auto const before = window.revision();
    CHECK(window.refresh(ids({3})).kind == Refresh::Pages);
    CHECK(window.revision() == before + 1);
}

// Mutation: accept dropping `unloadedRows`.
TEST_CASE("window: unloaded levels agree with the page after accept", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    auto const fetches = window.visible(0, 5, cols);
    REQUIRE_FALSE(fetches.empty());
    auto page = pageOf(0, range(0, 10), cols, 10);
    page.unloadedRows = {{.index = 2, .levels = {"recipe"}, .failed = {"recipe"}}};
    bool const accepted = window.accept(fetches[0], std::move(page));
    REQUIRE(accepted);
    std::vector<std::string> const recipe{"recipe"};
    CHECK(std::ranges::equal(window.unloadedLevels(2).levels, recipe));
    CHECK(std::ranges::equal(window.unloadedLevels(2).failed, recipe));
    CHECK(window.unloadedLevels(1).levels.empty());
    CHECK(window.unloadedLevels(50).levels.empty());
}

// Mutations: the total adopted without dropping what lies past it (`key(10)` stays); the pages past it dropped
// but their fetches kept in flight (`pending()` stays true, the late reply is accepted).
TEST_CASE("window: a view that empties shows no rows and asks no more", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    holdTwoPages(window, cols, 20);
    auto const plan = window.refresh(ids({3}));
    REQUIRE(plan.kind == Refresh::Rows);
    auto const before = window.revision();
    CHECK(window.patch(plan.rows, keyedOf({}, cols, 0)).empty());
    CHECK(window.revision() == before + 1);
    auto const refetch = window.visible(0, 5, cols);
    REQUIRE(refetch.size() == 2);
    CHECK(refetch[0].request.offset == 0);
    CHECK(refetch[1].request.offset == 10);
    REQUIRE(window.accept(refetch[0], pageOf(0, {}, cols, 0)));
    CHECK(window.key(0) == nullptr);
    CHECK(window.key(10) == nullptr);
    CHECK(window.total() == 0);
    CHECK_FALSE(window.pending());
    CHECK_FALSE(window.accept(refetch[1], pageOf(10, range(10, 10), cols, 20)));
    CHECK(window.visible(0, 5, cols).empty());
    CHECK(window.refresh(ids({3})).kind == Refresh::Nothing);
}

namespace {

// Pages 0 and 1 held with an awaited order level, invalidated, one event recorded while both refetches fly: the
// window asks once more and then comes to rest.
template <typename Window, typename MakePage>
void owedInvalidationEnds(Window& window, std::vector<std::string> const& cols, MakePage const& makePage) {
    auto const first = window.visible(0, 5, cols);
    REQUIRE(first.size() == 2);
    for (auto const& fetch : first) {
        REQUIRE(window.accept(fetch, makePage(fetch.request.offset)));
    }
    auto const start = window.revision();
    window.invalidate();
    auto const refetch = window.visible(0, 5, cols);
    REQUIRE(refetch.size() == 2);
    CHECK(window.refresh(ids({3})).kind == Refresh::Pages);
    REQUIRE(window.accept(refetch[0], makePage(0)));
    CHECK(window.owed().kind == Refresh::Pages);
    auto inFlight = window.visible(0, 5, cols);
    REQUIRE(inFlight.size() == 1);
    CHECK(inFlight[0].request.offset == 0);
    REQUIRE(window.accept(refetch[1], makePage(10)));
    CHECK(window.owed().kind == Refresh::Nothing);
    for (int round = 0; round < 6; ++round) {
        for (auto const& fetch : inFlight) {
            REQUIRE(window.accept(fetch, makePage(fetch.request.offset)));
            static_cast<void>(window.owed());
        }
        inFlight = window.visible(0, 5, cols);
    }
    CHECK(inFlight.empty());
    CHECK_FALSE(window.pending());
    CHECK(window.revision() == start + 3);
}

}  // namespace

// Mutation: owed() keeping its keys after a Pages plan (the revision rises at every accept, without end).
TEST_CASE("window: an owed invalidation ends", "[table][window]") {
    std::vector<std::string> const cols{"a"};
    SECTION("generic") {
        Win window{10, 8};
        owedInvalidationEnds(window, cols, [&](std::int64_t offset) {
            auto page = pageOf(offset, range(offset, 10), cols, 20);
            page.awaiting = {"recipe"};
            return page;
        });
    }
    SECTION("typed without a key function") {
        PageWindow<int> window{10, 8};
        owedInvalidationEnds(window, cols, [](std::int64_t offset) {
            return Page<int>{.rows = std::vector<int>(10, 1), .total = 20, .offset = offset, .awaiting = {"recipe"}};
        });
    }
}

// Mutation: probing every held row (1,600 probes: the plan becomes Pages).
TEST_CASE("window: a refresh of 900 keys over eight runs asks 908", "[table][window]") {
    Win window{200, 8};
    std::vector<std::string> const cols{"a"};
    for (std::int64_t page = 0; page < 16; page += 2) {
        for (auto const& fetch : window.visible(page * 200, 1, cols)) {
            if (fetch.request.offset == page * 200) {
                REQUIRE(window.accept(fetch,
                                      pageOf(fetch.request.offset, range(fetch.request.offset, 200), cols, 100'000)));
            }
        }
    }
    REQUIRE(window.pagesHeld() == 8);
    std::vector<RowId> event;
    for (std::int64_t key = 50'000; key < 50'900; ++key) {
        event.emplace_back(key);
    }
    auto const plan = window.refresh(event);
    REQUIRE(plan.kind == Refresh::Rows);
    CHECK(plan.rows.keys.size() == 908);
}

// Mutations: the visible set not clamped to maxColumns (the first fetch names three columns); the visible set not
// clamped and the shown columns not kept (each accept drops columns the next `visible` fetches again, for ever).
TEST_CASE("window: a visible set wider than maxColumns is clamped and comes to rest", "[table][window]") {
    Win window{10, 8, 1000, 2};
    std::vector<std::string> const abc{"a", "b", "c"};
    auto fetches = window.visible(0, 5, abc);
    REQUIRE(fetches.size() == 2);  // pages 0 and 1: no total is known yet
    CHECK(fetches[0].columns == std::vector<std::string>{"a", "b"});
    for (int round = 0; round < 6 && !fetches.empty(); ++round) {
        for (auto const& fetch : fetches) {
            // Page 1's reply is ignored once page 0's total of 10 drops it.
            static_cast<void>(window.accept(
                fetch, pageOf(fetch.request.offset, range(fetch.request.offset, 10), fetch.columns, 10)));
        }
        fetches = window.visible(0, 5, abc);
    }
    CHECK(fetches.empty());
    CHECK(window.holds(0, "a"));
    CHECK(window.holds(0, "b"));
    CHECK_FALSE(window.holds(0, "c"));
}

// Mutations: every asked key remembered, held or not (the count grows by one an event); keys that left the held
// pages never forgotten (the count stays at 20 after the pages hold other keys).
TEST_CASE("window: the keys it remembers stay within the rows it holds", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    holdTwoPages(window, cols, 1'000'000);
    for (std::int64_t inserted = 0; inserted < 5'000; ++inserted) {
        auto const plan = window.refresh(ids({500'000 + inserted, 3}));
        REQUIRE(plan.kind == Refresh::Rows);
        static_cast<void>(window.patch(plan.rows, keyedOf({{0, 0}, {3, 3}}, cols, 1'000'000)));
    }
    CHECK(window.keysRemembered() <= 20);
    auto const all = window.refresh(ids({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19}));
    REQUIRE(all.kind == Refresh::Rows);
    CHECK(window.keysRemembered() == 20);
    window.invalidate();
    for (auto const& fetch : window.visible(0, 5, cols)) {
        REQUIRE(window.accept(fetch,
                              pageOf(fetch.request.offset, range(1'000 + fetch.request.offset, 10), cols, 1'000'000)));
    }
    CHECK(window.keysRemembered() == 0);
}

// ── A page's members in a registered action's records ───────────────────────

namespace tserver {

struct PagedRow {
    std::int64_t code = 0;
};

struct ListPage {
    int limit{};
};

struct ListPages {
    int limit{};
};

struct PagedModel {
    Page<PagedRow> execute(ListPage const& /*action*/) { return {}; }
    std::vector<Page<PagedRow>> execute(ListPages const& /*action*/) { return {}; }
};

}  // namespace tserver

BRIDGE_REGISTER_RECORD(tserver::PagedRow, "T7_PagedRow", morph::record::key<&tserver::PagedRow::code>)
BRIDGE_REGISTER_MODEL(tserver::PagedModel, "T7_PagedModel")
BRIDGE_REGISTER_ACTION(tserver::PagedModel, tserver::ListPage, "T7_ListPage")
BRIDGE_REGISTER_ACTION(tserver::PagedModel, tserver::ListPages, "T7_ListPages")

// Mutation: resolution giving the records nothing from the holders above them (`open` and `partial` empty).
TEST_CASE("table: a registered page action's record is open and partial", "[table][server]") {
    for (std::string_view const action : {"T7_ListPage", "T7_ListPages"}) {
        INFO(action);
        auto const records = morph::effects::recordsOf(action);
        REQUIRE(records.size() == 1);
        CHECK(records[0].record == "T7_PagedRow");
        CHECK(records[0].keyField == "code");
        CHECK(records[0].paths == std::vector<std::string>{"rows"});
        CHECK(records[0].open == std::vector<std::string>{"awaiting", "distinct"});
        CHECK(records[0].partial == std::vector<std::string>{"unloadedRows"});
        CHECK_FALSE(records[0].opaque);
    }
}

// Mutation: dropping the most recently named of the columns not shown (`b` goes, `a` stays).
TEST_CASE("window: of the columns not shown, the one named longest ago goes first", "[table][window]") {
    Win window{10, 8, 1000, 2};
    for (std::string const column : {"a", "b", "c"}) {
        std::vector<std::string> const shown{column};
        for (auto const& fetch : window.visible(0, 5, shown)) {
            static_cast<void>(window.accept(
                fetch, pageOf(fetch.request.offset, range(fetch.request.offset, 10), fetch.columns, 10)));
        }
    }
    CHECK(window.holds(0, "c"));
    CHECK(window.holds(0, "b"));
    CHECK_FALSE(window.holds(0, "a"));
}

// Mutation: a page's completeness decided before the reply's total is adopted (the short first page is not
// complete, so its rest is fetched).
TEST_CASE("window: a short first page that reaches its total completes the window", "[table][window]") {
    PageWindow<int> window{10, 8};
    auto const fetches = window.visible(0, 5);
    REQUIRE(fetches.size() == 2);
    REQUIRE(window.accept(fetches[0], Page<int>{.rows = std::vector<int>(4, 1), .total = 4, .offset = 0}));
    CHECK_FALSE(window.pending());  // page 1 lies past the total
    CHECK(window.visible(0, 5).empty());
}
