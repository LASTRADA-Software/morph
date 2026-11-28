// SPDX-License-Identifier: Apache-2.0
//
// The server's view cache: when a computed view is reused, replaced, evicted or refused.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/table/view_cache.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;

namespace {

ViewComputation viewOf(std::vector<std::uint32_t> rows) {
    ViewComputation out{.view = {}, .awaiting = {}, .failed = {}, .orderKeys = nullptr};
    out.view.rows = std::move(rows);
    return out;
}

ViewKey keyOf(std::string query) { return ViewKey{.record = "T.rows", .scope = "", .query = std::move(query)}; }

}  // namespace

TEST_CASE("a view is computed once per snapshot and read again from the cache", "[covc][table][viewcache]") {
    auto const source = tabletest::column(ColumnKind::Integer,
                                          {Cell{std::int64_t{10}}, Cell{std::int64_t{20}}, Cell{std::int64_t{30}}});
    auto const snapshot = source->snapshot();
    ViewCache cache;
    int computed = 0;
    auto const compute = [&computed]() -> std::expected<ViewComputation, TableError> {
        ++computed;
        return viewOf({2, 0});
    };
    auto const first = cache.find(keyOf("a"), snapshot, {}, compute);
    REQUIRE(first);
    auto const again = cache.find(keyOf("a"), snapshot, {}, compute);
    REQUIRE(again);
    CHECK(first->get() == again->get());
    CHECK(computed == 1);
    CHECK(cache.stats().hits == 1);

    auto const& entry = **first;
    CHECK(entry.sourceRowOf(RowId{std::int64_t{2}}) == std::optional<std::uint32_t>{2});
    CHECK_FALSE(entry.sourceRowOf(RowId{std::int64_t{9}}));
    auto const viewRows = entry.viewRowOf();
    REQUIRE(viewRows.size() == 3);
    CHECK(viewRows[2] == 0);
    CHECK(viewRows[0] == 1);
    CHECK(entry.bytes() > 0);

    auto const other = tabletest::column(ColumnKind::Integer, {Cell{std::int64_t{1}}});
    auto const elsewhere = cache.find(keyOf("a"), other->snapshot(), {}, compute);
    REQUIRE(elsewhere);
    CHECK(computed == 2);
    CHECK(cache.stats().invalidations == 1);
}

TEST_CASE("a failed computation is returned and not cached, and a throwing one rethrows", "[covc][table][viewcache]") {
    auto const source = tabletest::column(ColumnKind::Integer, {Cell{std::int64_t{1}}});
    auto const snapshot = source->snapshot();
    ViewCache cache;
    int computed = 0;
    auto const failing = [&computed]() -> std::expected<ViewComputation, TableError> {
        ++computed;
        return std::unexpected(TableError{.code = TableErrorCode::SourceContract, .column = {}, .message = "no"});
    };
    auto const first = cache.find(keyOf("q"), snapshot, {}, failing);
    REQUIRE_FALSE(first);
    CHECK(first.error().message == "no");
    CHECK_FALSE(cache.find(keyOf("q"), snapshot, {}, failing));
    CHECK(computed == 2);

    auto const throwing = []() -> std::expected<ViewComputation, TableError> { throw std::runtime_error{"boom"}; };
    CHECK_THROWS_AS(std::ignore = cache.find(keyOf("q"), snapshot, {}, throwing), std::runtime_error);
    CHECK(
        cache.find(keyOf("q"), snapshot, {}, [] { return std::expected<ViewComputation, TableError>{viewOf({0})}; }));
}

TEST_CASE("an entry is replaced by a view of newer levels and evicted past the byte bound",
          "[covc][table][viewcache]") {
    auto const source = tabletest::column(ColumnKind::Integer, {Cell{std::int64_t{1}}, Cell{std::int64_t{2}}});
    auto const snapshot = source->snapshot();
    ViewCache cache;
    int computed = 0;
    auto const compute = [&computed]() -> std::expected<ViewComputation, TableError> {
        ++computed;
        return viewOf({0, 1});
    };
    std::vector<std::pair<std::string, std::uint64_t>> const older{{"slow", 1}};
    std::vector<std::pair<std::string, std::uint64_t>> const newer{{"slow", 2}};
    REQUIRE(cache.find(keyOf("l"), snapshot, older, compute));
    REQUIRE(cache.find(keyOf("l"), snapshot, older, compute));
    CHECK(computed == 1);
    REQUIRE(cache.find(keyOf("l"), snapshot, newer, compute));
    CHECK(computed == 2);
    REQUIRE(cache.find(keyOf("l"), snapshot, older, compute));
    CHECK(computed == 3);

    ViewCache small{ViewCacheOptions{.maxBytes = 1}};
    REQUIRE(small.find(keyOf("a"), snapshot, {}, compute));
    REQUIRE(small.find(keyOf("b"), snapshot, {}, compute));
    CHECK(small.stats().evictions >= 1);
    CHECK(small.stats().bytes <= 8);
}
