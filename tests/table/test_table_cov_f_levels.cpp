// SPDX-License-Identifier: Apache-2.0
//
// A level cache's order uses: the sets it accepts and ends, the leases it keeps, how a queued key changes use,
// and how a failed key of an accepted set is asked for again.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/table/level_cache.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using namespace std::chrono_literals;

namespace {

struct Rig {
    explicit Rig(std::size_t batch = 64)
        : cache{[this, batch] {
                    LevelCacheOptions options;
                    options.records = {"T.rows"};
                    options.level = "slow";
                    options.columns = {tabletest::levelCol("c", ColumnKind::Text, "slow")};
                    options.loads = &loads;
                    options.batch = batch;
                    options.clock = [this] { return now; };
                    return options;
                }(),
                [this](std::span<RowId const> asked) {
                    calls.emplace_back(asked.begin(), asked.end());
                    if (fail) {
                        throw std::runtime_error{"unavailable"};
                    }
                    std::vector<std::pair<RowId, std::vector<Cell>>> out;
                    for (auto const& key : asked) {
                        out.emplace_back(key, std::vector<Cell>{Cell{std::string{"v"}}});
                    }
                    return out;
                }} {}

    [[nodiscard]] LevelRead stateOf(std::int64_t key) const {
        std::vector<Cell> out(1);
        return cache.snapshot()->read(RowId{key}, out);
    }

    void settle() { tabletest::drain(loads); }

    tabletest::ManualExecutor loads;
    std::chrono::steady_clock::time_point now{std::chrono::hours{1}};
    std::vector<std::vector<RowId>> calls;
    bool fail = false;
    CellLevelCache cache;
};

std::shared_ptr<std::vector<RowId> const> setOf(std::initializer_list<std::int64_t> keys) {
    auto out = std::make_shared<std::vector<RowId>>();
    for (auto const key : keys) {
        out->emplace_back(key);
    }
    return out;
}

using Access = detail::LevelCacheAccess;

}  // namespace

TEST_CASE("a key the cache never held has no lease and is not kept", "[covf][table][levels]") {
    Rig const rig;
    CHECK(Access::leaseEnd(rig.cache, RowId{std::int64_t{9}}) == 0);
    CHECK_FALSE(Access::kept(rig.cache, RowId{std::int64_t{9}}));
    CHECK_FALSE(Access::leasePending(rig.cache, RowId{std::int64_t{9}}));
}

TEST_CASE("a key queued for display stays queued for display when it is then wanted for order",
          "[covf][table][levels]") {
    Rig rig{1};
    std::vector<RowId> const both{RowId{std::int64_t{1}}, RowId{std::int64_t{2}}};
    REQUIRE(rig.cache.want(both, LevelUse::Display));
    std::vector<RowId> const second{RowId{std::int64_t{2}}};
    REQUIRE(rig.cache.want(second, LevelUse::Order));
    rig.settle();
    CHECK(rig.stateOf(1) == LevelRead::Loaded);
    CHECK(rig.stateOf(2) == LevelRead::Loaded);
    // Each key was asked for once, in batches of one: the order want did not queue key 2 a second time.
    CHECK(rig.calls.size() == 2);
}

TEST_CASE("an order use of a set keeps its keys leased, and a repeat of the set is a hit", "[covf][table][levels]") {
    Rig rig;
    auto const set = setOf({1, 2, 3});
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    rig.settle();
    CHECK(Access::acceptedSets(rig.cache) == 1);
    CHECK(rig.stateOf(2) == LevelRead::Loaded);
    CHECK(Access::kept(rig.cache, RowId{std::int64_t{2}}));
    auto const hitsBefore = rig.cache.stats().hits;
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    CHECK(rig.cache.stats().hits == hitsBefore + 3);
    CHECK(Access::acceptedSets(rig.cache) == 1);
    CHECK(rig.calls.size() == 1);
}

TEST_CASE("a set whose lease has run out is ended and accepted anew by the next want", "[covf][table][levels]") {
    Rig rig;
    auto const set = setOf({1, 2});
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    rig.settle();
    CHECK(Access::acceptedSets(rig.cache) == 1);
    rig.now += 61s;
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    rig.settle();
    CHECK(Access::acceptedSets(rig.cache) == 1);
    CHECK(Access::kept(rig.cache, RowId{std::int64_t{1}}));
}

TEST_CASE("a retried order want is not a set and leases its keys one by one", "[covf][table][levels]") {
    Rig rig;
    auto const set = setOf({4, 5});
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order, true));
    rig.settle();
    CHECK(Access::acceptedSets(rig.cache) == 0);
    CHECK(rig.stateOf(4) == LevelRead::Loaded);
    CHECK(Access::kept(rig.cache, RowId{std::int64_t{4}}));
    REQUIRE(rig.cache.wantSet(set, LevelUse::Display));
    CHECK(Access::acceptedSets(rig.cache) == 0);
}

TEST_CASE("a key of an accepted set that failed is asked for again once its delay has passed",
          "[covf][table][levels]") {
    Rig rig;
    rig.fail = true;
    auto const set = setOf({1});
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    rig.settle();
    CHECK(rig.stateOf(1) == LevelRead::Failed);
    CHECK(rig.calls.size() == 1);

    // Inside the delay the repeat of the set asks for nothing.
    rig.now += 10s;
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    rig.settle();
    CHECK(rig.calls.size() == 1);

    // Past the delay the key is asked for again, and loads.
    rig.now += 25s;
    rig.fail = false;
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    rig.settle();
    CHECK(rig.calls.size() == 2);
    CHECK(rig.stateOf(1) == LevelRead::Loaded);
}

TEST_CASE("taking back an order want ends its lease with its last holder", "[covf][table][levels]") {
    Rig rig;
    std::vector<RowId> const keys{RowId{std::int64_t{1}}, RowId{std::int64_t{2}}};
    REQUIRE(rig.cache.want(keys, LevelUse::Order));
    REQUIRE(rig.cache.want(keys, LevelUse::Order));
    rig.settle();
    CHECK(Access::kept(rig.cache, keys[0]));
    rig.cache.unwant(keys);
    CHECK(Access::kept(rig.cache, keys[0]));
    rig.cache.unwant(keys);
    CHECK(Access::leaseEnd(rig.cache, keys[0]) == 0);
    CHECK_FALSE(Access::kept(rig.cache, keys[0]));
    // A release of keys the cache never leased is harmless.
    rig.cache.unwant(std::vector<RowId>{RowId{std::int64_t{77}}});
    CHECK(rig.stateOf(1) == LevelRead::Loaded);
}

TEST_CASE("an invalidated key is loaded again by the next want", "[covf][table][levels]") {
    Rig rig;
    std::vector<RowId> const keys{RowId{std::int64_t{1}}};
    REQUIRE(rig.cache.want(keys, LevelUse::Display));
    rig.settle();
    CHECK(rig.stateOf(1) == LevelRead::Loaded);
    rig.cache.invalidate(keys);
    CHECK(rig.stateOf(1) != LevelRead::Loaded);
    REQUIRE(rig.cache.want(keys, LevelUse::Display));
    rig.settle();
    CHECK(rig.calls.size() == 2);
    rig.cache.invalidateAll();
    CHECK(rig.stateOf(1) != LevelRead::Loaded);
}
