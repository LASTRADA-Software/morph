// SPDX-License-Identifier: Apache-2.0
//
// A level cache's batching and its retry delay: a key whose load failed is not asked for again until its delay has
// passed, the delay doubles with each failure up to a ceiling, and a retry asks at once.

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
                    sizes.push_back(asked.size());
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

    tabletest::ManualExecutor loads;
    std::chrono::steady_clock::time_point now{std::chrono::hours{1}};
    std::vector<std::size_t> sizes;
    bool fail = false;
    CellLevelCache cache;
};

std::vector<RowId> oneKey() { return {RowId{std::int64_t{1}}}; }

}  // namespace

TEST_CASE("a batch of zero loads one key at a time", "[covc][table][levels]") {
    Rig rig{0};
    std::vector<RowId> const keys{RowId{std::int64_t{1}}, RowId{std::int64_t{2}}, RowId{std::int64_t{3}}};
    REQUIRE(rig.cache.want(keys, LevelUse::Display));
    tabletest::drain(rig.loads);
    CHECK(rig.sizes == std::vector<std::size_t>{1, 1, 1});
    CHECK(rig.stateOf(2) == LevelRead::Loaded);
}

TEST_CASE("a failed key waits out a delay that doubles, up to a ceiling, unless it is retried",
          "[covc][table][levels]") {
    Rig rig;
    rig.fail = true;
    auto const ask = [&](bool retry = false) {
        REQUIRE(rig.cache.want(oneKey(), LevelUse::Display, retry));
        tabletest::drain(rig.loads);
    };
    ask();
    CHECK(rig.sizes.size() == 1);
    CHECK(rig.stateOf(1) == LevelRead::Failed);

    rig.now += 29s;
    ask();
    CHECK(rig.sizes.size() == 1);
    rig.now += 2s;
    ask();
    CHECK(rig.sizes.size() == 2);

    rig.now += 59s;
    ask();
    CHECK(rig.sizes.size() == 2);
    rig.now += 2s;
    ask();
    CHECK(rig.sizes.size() == 3);

    ask(true);
    CHECK(rig.sizes.size() == 4);
    for (int failure = 0; failure < 40; ++failure) {
        ask(true);
    }
    auto const before = rig.sizes.size();
    rig.now += 599s;
    ask();
    CHECK(rig.sizes.size() == before);
    rig.now += 2s;
    rig.fail = false;
    ask();
    CHECK(rig.sizes.size() == before + 1);
    CHECK(rig.stateOf(1) == LevelRead::Loaded);
}

TEST_CASE("a cache asked about a key it does not hold reports it as not loaded", "[covc][table][levels]") {
    Rig const rig;
    CHECK(rig.stateOf(7) != LevelRead::Loaded);
    CHECK(rig.cache.stats().loaderCalls == 0);
}
