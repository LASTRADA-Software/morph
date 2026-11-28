// SPDX-License-Identifier: Apache-2.0
//
// A served table's answers: what it refuses before it reads a row, and what a values list over an order level
// does with its level cache.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/table/description.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/table/view_cache.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;

namespace {

TableColumn describedColumn(std::string const& id, ColumnKind kind, std::string level = {}) {
    return TableColumn{.info = {.id = id, .kind = kind, .comparator = {}, .format = {}, .level = std::move(level)},
                       .label = id,
                       .group = {}};
}

// Three rows of an integer key and a text column held at level "slow"; the text arrives only through the cache.
struct Slow {
    Slow() {
        std::vector<std::vector<Cell>> cells;
        cells.reserve(3);
        for (std::int64_t row = 0; row < 3; ++row) {
            cells.push_back({Cell{row}, Cell{Unloaded{}}});
        }
        source = tabletest::makeSource(
            {tabletest::col("id", ColumnKind::Integer), tabletest::levelCol("text", ColumnKind::Text, "slow")},
            std::move(cells));
    }

    [[nodiscard]] ServedTable served(std::shared_ptr<LevelSource> level = {}) const {
        auto description = TableDescription{
            .id = "t",
            .label = "T",
            .columns = {describedColumn("id", ColumnKind::Integer), describedColumn("text", ColumnKind::Text, "slow")},
            .key = "id"};
        ServedTable out{.description = describe(std::move(description)),
                        .source = source,
                        .levels = {},
                        .mayRead = {},
                        .scope = {},
                        .services = {},
                        .limits = {}};
        if (level != nullptr) {
            out.levels.push_back(std::move(level));
        }
        return detail::pinned(std::move(out));
    }

    [[nodiscard]] std::shared_ptr<CellLevelCache> cache(std::size_t maxBytes = std::size_t{1} << 20U) {
        LevelCacheOptions options;
        options.records = {"T.rows"};
        options.level = "slow";
        options.columns = {tabletest::levelCol("text", ColumnKind::Text, "slow")};
        options.loads = &loads;
        options.maxBytes = maxBytes;
        options.expectedBytes = 1024;
        return std::make_shared<CellLevelCache>(std::move(options), [](std::span<RowId const> asked) {
            std::vector<std::pair<RowId, std::vector<Cell>>> out;
            for (auto const& key : asked) {
                out.emplace_back(key, std::vector<Cell>{Cell{std::string{"v"}}});
            }
            return out;
        });
    }

    std::shared_ptr<VectorSource> source;
    tabletest::ManualExecutor loads;
};

}  // namespace

TEST_CASE("a served table refuses before it reads a row", "[covc][table][answer]") {
    Slow const slow;
    auto const served = slow.served();

    auto scoped = detail::answerQuery(served, TableQuery{},
                                      detail::AnswerContext{.views = nullptr, .record = {}, .rowScoped = true});
    REQUIRE_FALSE(scoped);
    CHECK(scoped.error().message == "a row-scoped table needs a scope");

    ServedTable none = served;
    none.source = nullptr;
    auto const noSource = detail::answerQuery(none, TableQuery{});
    REQUIRE_FALSE(noSource);
    CHECK(noSource.error().code == TableErrorCode::Unavailable);

    ServedTable mismatched = served;
    mismatched.description.columns.push_back(describedColumn("ghost", ColumnKind::Text));
    auto const ghost = detail::answerQuery(mismatched, TableQuery{});
    REQUIRE_FALSE(ghost);
    CHECK(ghost.error().code == TableErrorCode::UnknownColumn);
    CHECK(ghost.error().column == "ghost");

    ServedTable noDistinct = served;
    noDistinct.description.capabilities.distinct = false;
    TableQuery distinct;
    distinct.distinct = DistinctRequest{.column = "id"};
    auto const refused = detail::answerQuery(noDistinct, distinct);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message == "this table answers no distinct request");
}

TEST_CASE("a values list over an order level wants every row's key from the level, once per snapshot",
          "[covc][table][answer]") {
    Slow slow;
    auto const level = slow.cache();
    auto const served = slow.served(level);
    ViewCache views;
    TableQuery distinct;
    distinct.distinct = DistinctRequest{.column = "text"};

    auto const first = detail::answerQuery(
        served, distinct, detail::AnswerContext{.views = &views, .record = "T.rows", .rowScoped = false});
    REQUIRE(first);
    REQUIRE(first->distinct);
    CHECK(first->distinct->partial);
    tabletest::drain(slow.loads);
    auto const second = detail::answerQuery(
        served, distinct, detail::AnswerContext{.views = &views, .record = "T.rows", .rowScoped = false});
    REQUIRE(second);
    REQUIRE(second->distinct);
    CHECK_FALSE(second->distinct->partial);
    REQUIRE(second->distinct->values.size() == 1);
    CHECK(second->distinct->values[0].text == "v");
    CHECK(second->distinct->values[0].count == 3);
    auto const uncached = detail::answerQuery(served, distinct);
    REQUIRE(uncached);
}

TEST_CASE("an order level that cannot hold the rows refuses, naming the column that needs it",
          "[covc][table][answer]") {
    Slow slow;
    auto const level = slow.cache(1);
    auto const served = slow.served(level);

    TableQuery distinct;
    distinct.distinct = DistinctRequest{.column = "text"};
    auto const byDistinct = detail::answerQuery(served, distinct);
    REQUIRE_FALSE(byDistinct);
    CHECK(byDistinct.error().code == TableErrorCode::Unavailable);
    CHECK(byDistinct.error().column == "text");
}

TEST_CASE("an order level too small for a page's sort or filter refuses, naming the column that asked",
          "[covc][table][answer]") {
    Slow slow;
    auto const level = slow.cache(1);
    auto const served = slow.served(level);

    TableQuery sorted;
    sorted.sort = {SortKey{.column = "text"}};
    auto const bySort = detail::answerQuery(served, sorted);
    REQUIRE_FALSE(bySort);
    CHECK(bySort.error().code == TableErrorCode::Unavailable);
    CHECK(bySort.error().column == "text");

    TableQuery filtered;
    filtered.filters.columns["text"].include = {FilterEntry{.eq = "v"}};
    auto const byFilter = detail::answerQuery(served, filtered);
    REQUIRE_FALSE(byFilter);
    CHECK(byFilter.error().column == "text");
}

TEST_CASE("a page sorted by an order level waits for the level, then answers in its order", "[covc][table][answer]") {
    Slow slow;
    auto const level = slow.cache();
    auto const served = slow.served(level);
    ViewCache views;
    TableQuery sorted;
    sorted.sort = {SortKey{.column = "text"}, SortKey{.column = "id", .dir = SortDirection::Descending}};
    auto const context = detail::AnswerContext{.views = &views, .record = "T.rows", .rowScoped = false};

    auto const first = detail::answerQuery(served, sorted, context);
    REQUIRE(first);
    CHECK(first->awaiting == std::vector<std::string>{"slow"});
    tabletest::drain(slow.loads);
    auto const second = detail::answerQuery(served, sorted, context);
    REQUIRE(second);
    CHECK(second->awaiting.empty());
    CHECK(second->keys == std::vector<RowId>{RowId{std::int64_t{2}}, RowId{std::int64_t{1}}, RowId{std::int64_t{0}}});
    CHECK(second->undecided == 0);
}

TEST_CASE("a selection of the view filtered by an order level reads the level's values", "[covc][table][answer]") {
    Slow slow;
    auto const level = slow.cache();
    auto const served = slow.served(level);
    detail::PlainSelection selection;
    selection.isView = true;
    selection.filters.columns["text"].include = {FilterEntry{.eq = "v"}};
    REQUIRE(level->want(std::vector<RowId>{RowId{std::int64_t{0}}, RowId{std::int64_t{1}}, RowId{std::int64_t{2}}},
                        LevelUse::Display));
    tabletest::drain(slow.loads);
    auto const members = detail::membership(served, selection.filters, {}, nullptr, {});
    REQUIRE(members);
    CHECK(members->rows == std::vector<std::uint32_t>{0, 1, 2});
    auto const resolved = detail::resolveSelection(served, selection, SelectionPolicy::Current, nullptr, {}, false);
    REQUIRE(resolved);
    CHECK(resolved->keys.size() == 3);

    selection.filters.columns["text"].include = {FilterEntry{.eq = "other"}};
    auto const none = detail::resolveSelection(served, selection, SelectionPolicy::Current, nullptr, {}, false);
    REQUIRE(none);
    CHECK(none->keys.empty());
}
