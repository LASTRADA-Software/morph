// SPDX-License-Identifier: Apache-2.0
//
// morph::table's server level cache (docs/spec/table/table.md, "The server level cache"): a value is loaded once and
// served until invalidated or evicted, a load does not land stale, values are stored before they are announced, the
// bound is deep and evicts the least recently used values no order lease holds, an order use that cannot fit is
// refused, a failed load waits out its delay, and a snapshot held by a reader blocks no store. Each case names the
// mutation it was seen to fail on.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <morph/core/change_types.hpp>
#include <morph/core/executor.hpp>
#include <morph/table/level_cache.hpp>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using namespace std::chrono_literals;

namespace {

using Values = std::vector<std::pair<RowId, std::vector<Cell>>>;
using Body = std::function<Values(std::span<RowId const>)>;

// RowId{first} … RowId{last - 1}.
std::vector<RowId> keyRange(std::int64_t first, std::int64_t last) {
    std::vector<RowId> out;
    for (auto key = first; key < last; ++key) {
        out.emplace_back(key);
    }
    return out;
}

// RowId{0} … RowId{n - 1}.
std::vector<RowId> keys(std::int64_t n) { return keyRange(0, n); }

std::string text(RowId const& key) {
    return std::visit(
        [](auto const& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>) {
                return value;
            } else {
                return std::to_string(value);
            }
        },
        key);
}

// The first cell of the key's value, or Unloaded when the snapshot does not hold it.
Cell readCell(LevelSnapshot const& snapshot, RowId const& key) {
    std::vector<Cell> out(1);
    return snapshot.read(key, out) == LevelRead::Loaded ? out[0] : Cell{Unloaded{}};
}

LevelRead stateOf(LevelSnapshot const& snapshot, RowId const& key) {
    std::vector<Cell> out(1);
    return snapshot.read(key, out);
}

// The bytes a one-cell value `v<key>` owns: every such value is in the string's small buffer, so all are equal.
std::size_t smallValueBytes() { return detail::cellsBytes({Cell{std::string{"v0"}}}); }

struct Announced {
    std::string record;
    std::optional<std::vector<RowId>> keys;
};

// A cache over one text column, a manual executor for its loads, an injected clock, a loader that logs its keys,
// and an announce sink that records what it is told.
struct Rig {
    explicit Rig(std::function<void(LevelCacheOptions&)> const& adjust = {}, Body loader = {})
        : body{std::move(loader)},
          cache{optionsOf(adjust), [this](std::span<RowId const> asked) { return load(asked); }} {
        cache.bind([this](std::string_view record, std::optional<std::vector<glz::raw_json>> raw) {
            Announced entry{.record = std::string{record}, .keys = std::nullopt};
            if (raw) {
                entry.keys.emplace();
                for (auto const& key : *raw) {
                    entry.keys->push_back(rowIdOf(key).value_or(RowId{std::string{"?"}}));
                }
            }
            if (readOnAnnounce) {
                auto const snapshot = cache.snapshot();
                for (auto const& key : entry.keys ? *entry.keys : readOnWhole) {
                    readsAtAnnounce.push_back(stateOf(*snapshot, key));
                }
            }
            std::scoped_lock const lock{mutex};
            announced.push_back(std::move(entry));
        });
    }

    LevelCacheOptions optionsOf(std::function<void(LevelCacheOptions&)> const& adjust) {
        LevelCacheOptions options;
        options.records = {"T.rows"};
        options.level = "recipe";
        options.columns = {tabletest::levelCol("r", ColumnKind::Text, "recipe")};
        options.loads = &loads;
        options.clock = [this] { return now; };
        if (adjust) {
            adjust(options);
        }
        return options;
    }

    Values load(std::span<RowId const> asked) {
        {
            std::scoped_lock const lock{mutex};
            calls.emplace_back(asked.begin(), asked.end());
        }
        if (body) {
            return body(asked);
        }
        Values out;
        for (auto const& key : asked) {
            out.emplace_back(key, std::vector<Cell>{Cell{"v" + text(key)}});
        }
        return out;
    }

    void drain() { tabletest::drain(loads); }

    // Every key the loader was given, sorted.
    std::vector<RowId> loggedKeys() {
        std::scoped_lock const lock{mutex};
        std::vector<RowId> out;
        for (auto const& call : calls) {
            out.insert(out.end(), call.begin(), call.end());
        }
        std::ranges::sort(out);
        return out;
    }

    std::size_t callCount() {
        std::scoped_lock const lock{mutex};
        return calls.size();
    }

    tabletest::ManualExecutor loads;
    std::chrono::steady_clock::time_point now{std::chrono::hours{1}};
    std::mutex mutex;
    std::vector<std::vector<RowId>> calls;
    std::vector<Announced> announced;
    bool readOnAnnounce = false;
    std::vector<RowId> readOnWhole;
    std::vector<LevelRead> readsAtAnnounce;
    Body body;
    CellLevelCache cache;
};

// The bytes an accepted set of `n` integer keys owns in the bound.
std::size_t setKeyBytes(std::size_t n) {
    return sizeof(std::vector<RowId>) + (n * rowIdBytes(RowId{std::int64_t{0}}));
}

// Each key's value as the rig's loader returns it, for a store that bypasses the loader.
Values valuesOf(std::vector<RowId> const& stored) {
    Values out;
    for (auto const& key : stored) {
        out.emplace_back(key, std::vector<Cell>{Cell{"v" + text(key)}});
    }
    return out;
}

std::vector<glz::raw_json> rawKeys(std::vector<std::string> const& texts) {
    std::vector<glz::raw_json> out;
    for (auto const& entry : texts) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.emplace_back(entry);
    }
    return out;
}

}  // namespace

// Mutation: want not checking the cache (loads held keys again): loadedKeys 400.
TEST_CASE("level cache: a value is loaded once", "[table][levels]") {
    Rig rig;
    REQUIRE(rig.cache.want(keys(200), LevelUse::Display));
    rig.drain();  // all four batches
    REQUIRE(rig.cache.want(keys(200), LevelUse::Display));
    rig.drain();
    CHECK(rig.cache.stats().loadedKeys == 200);
    CHECK(rig.loggedKeys() == keys(200));  // each once, in any order: compared sorted
    CHECK(readCell(*rig.cache.snapshot(), RowId{std::int64_t{5}}) == Cell{std::string{"v5"}});
}

// Mutation: want queueing a key that is already queued (272 keys loaded).
TEST_CASE("level cache: a key wanted while it is queued is loaded once", "[table][levels]") {
    Rig rig;
    REQUIRE(rig.cache.want(keys(200), LevelUse::Display));
    rig.loads.runAll();  // one batch of 64 stored, the next posted
    REQUIRE(rig.cache.want(keys(200), LevelUse::Display));
    rig.drain();
    CHECK(rig.cache.stats().loadedKeys == 200);
    CHECK(rig.loggedKeys() == keys(200));
}

// Mutation: no in-flight set (every thread's want loads the 60 keys again).
TEST_CASE("level cache: concurrent wants share one load", "[table][levels][threads]") {
    morph::exec::ThreadPoolExecutor pool{1};
    std::latch open{1};
    Rig rig{[&pool](LevelCacheOptions& options) { options.loads = &pool; },
            [&open](std::span<RowId const> asked) {
                open.wait();
                Values out;
                for (auto const& key : asked) {
                    out.emplace_back(key, std::vector<Cell>{Cell{"v" + text(key)}});
                }
                return out;
            }};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&rig] { std::ignore = (rig.cache.want(keys(60), LevelUse::Display)); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    open.count_down();
    // The one load was posted before this no-op, and the pool has one thread: once the no-op runs, the load has.
    std::promise<void> idle;
    pool.post([&idle] { idle.set_value(); });
    idle.get_future().wait();
    CHECK(rig.callCount() == 1);
    CHECK(rig.cache.stats().loadedKeys == 60);
}

// Mutation: invalidate leaving the values.
TEST_CASE("level cache: invalidated keys are absent and load once more", "[table][levels]") {
    Rig rig;
    REQUIRE(rig.cache.want(keys(200), LevelUse::Display));
    rig.drain();
    auto const before = rig.loggedKeys();
    rig.cache.invalidate(keys(10));
    auto const snapshot = rig.cache.snapshot();
    for (auto const& key : keys(200)) {
        CHECK(stateOf(*snapshot, key) == (std::get<std::int64_t>(key) < 10 ? LevelRead::Unloaded : LevelRead::Loaded));
    }
    REQUIRE(rig.cache.want(keys(200), LevelUse::Display));
    REQUIRE(rig.cache.want(keys(200), LevelUse::Display));
    rig.drain();
    auto expected = keys(200);
    auto const again = keys(10);
    expected.insert(expected.end(), again.begin(), again.end());
    std::ranges::sort(expected);
    CHECK(rig.loggedKeys() == expected);
    CHECK(rig.cache.stats().invalidatedKeys == 10);
    CHECK(before.size() == 200);
}

// Mutation: changed ignoring the record (every change empties the level).
TEST_CASE("level cache: the observer invalidates by record", "[table][levels]") {
    {
        Rig rig;
        REQUIRE(rig.cache.want(keys(10), LevelUse::Display));
        rig.drain();
        auto const loads = rig.announced.size();
        rig.cache.changed(
            morph::effects::Changes{.records = {{.record = "T.rows", .keys = rawKeys({"3"})}}, .instances = {}});
        auto snapshot = rig.cache.snapshot();
        CHECK(stateOf(*snapshot, RowId{std::int64_t{3}}) == LevelRead::Unloaded);
        CHECK(stateOf(*snapshot, RowId{std::int64_t{4}}) == LevelRead::Loaded);
        CHECK(rig.announced.size() == loads);  // the action's own event is the notification

        rig.cache.changed(
            morph::effects::Changes{.records = {{.record = "Other", .keys = std::nullopt}}, .instances = {}});
        CHECK(stateOf(*rig.cache.snapshot(), RowId{std::int64_t{4}}) == LevelRead::Loaded);  // in neither list

        rig.cache.changed(
            morph::effects::Changes{.records = {{.record = "T.rows", .keys = std::nullopt}}, .instances = {}});
        snapshot = rig.cache.snapshot();
        for (auto const& key : keys(10)) {
            CHECK(stateOf(*snapshot, key) == LevelRead::Unloaded);
        }
        CHECK(rig.announced.size() == loads);
    }
    {
        Rig rig{[](LevelCacheOptions& options) { options.invalidatedBy = {"T.rows", "Recipe"}; }};
        REQUIRE(rig.cache.want(keys(10), LevelUse::Display));
        rig.drain();
        auto const loads = rig.announced.size();
        rig.cache.changed(
            morph::effects::Changes{.records = {{.record = "Other", .keys = std::nullopt}}, .instances = {}});
        CHECK(stateOf(*rig.cache.snapshot(), RowId{std::int64_t{4}}) == LevelRead::Loaded);
        CHECK(rig.announced.size() == loads);
        rig.cache.changed(
            morph::effects::Changes{.records = {{.record = "Recipe", .keys = rawKeys({"77"})}}, .instances = {}});
        auto const snapshot = rig.cache.snapshot();
        for (auto const& key : keys(10)) {
            CHECK(stateOf(*snapshot, key) == LevelRead::Unloaded);
        }
        REQUIRE(rig.announced.size() == loads + 1);
        CHECK(rig.announced.back().record == "T.rows");
        CHECK_FALSE(rig.announced.back().keys.has_value());
    }
}

// Mutation: storing without the generation check (key 1 reads the stale value).
TEST_CASE("level cache: a load in flight at invalidation does not land", "[table][levels]") {
    Rig rig;
    std::vector<RowId> const one{RowId{std::int64_t{1}}};
    REQUIRE(rig.cache.want(one, LevelUse::Display));  // the load is posted and held until the executor runs it
    rig.cache.invalidate(one);
    rig.drain();  // the loader returns the old value
    CHECK(stateOf(*rig.cache.snapshot(), one[0]) == LevelRead::Unloaded);
    CHECK(rig.cache.stats().droppedLoads == 1);
    REQUIRE(rig.cache.want(one, LevelUse::Display));
    rig.drain();
    CHECK(rig.callCount() == 2);
    CHECK(stateOf(*rig.cache.snapshot(), one[0]) == LevelRead::Loaded);
}

// Mutation: "in flight" ignoring the generation, which leaves key 1 unloaded after the stale load is dropped.
TEST_CASE("level cache: a want after an invalidation does not wait for the stale load", "[table][levels]") {
    int call = 0;
    Rig rig{{}, [&call](std::span<RowId const> asked) {
                ++call;
                Values out;
                for (auto const& key : asked) {
                    out.emplace_back(key, std::vector<Cell>{Cell{"call" + std::to_string(call)}});
                }
                return out;
            }};
    std::vector<RowId> const one{RowId{std::int64_t{1}}};
    REQUIRE(rig.cache.want(one, LevelUse::Display));
    rig.cache.invalidate(one);
    REQUIRE(rig.cache.want(one, LevelUse::Display));
    rig.drain();
    CHECK(rig.callCount() == 2);
    CHECK(readCell(*rig.cache.snapshot(), one[0]) == Cell{std::string{"call2"}});
    CHECK(rig.cache.stats().droppedLoads == 1);
}

// Mutation: announcing before storing (the sink reads the announced keys unloaded).
TEST_CASE("level cache: values are stored before they are announced", "[table][levels]") {
    Rig rig{[](LevelCacheOptions& options) { options.invalidatedBy = {"T.rows", "Recipe"}; }};
    rig.readOnAnnounce = true;
    REQUIRE(rig.cache.want(keys(100), LevelUse::Display));
    rig.drain();
    REQUIRE(rig.readsAtAnnounce.size() == 100);
    CHECK(std::ranges::all_of(rig.readsAtAnnounce, [](LevelRead read) { return read == LevelRead::Loaded; }));
    rig.readsAtAnnounce.clear();
    rig.readOnWhole = keys(100);
    rig.cache.changed(
        morph::effects::Changes{.records = {{.record = "Recipe", .keys = std::nullopt}}, .instances = {}});
    REQUIRE(rig.readsAtAnnounce.size() == 100);
    CHECK(std::ranges::all_of(rig.readsAtAnnounce, [](LevelRead read) { return read == LevelRead::Unloaded; }));
}

// Mutation: evicting the most recently used (keys 50–99 go instead of 0–49).
TEST_CASE("level cache: eviction drops the least recently used", "[table][levels]") {
    Rig rig{[](LevelCacheOptions& options) { options.maxBytes = 100 * smallValueBytes(); }};
    REQUIRE(rig.cache.want(keys(100), LevelUse::Display));
    rig.drain();
    REQUIRE(rig.cache.want(keyRange(50, 100), LevelUse::Display));  // read: their recency is renewed
    rig.drain();
    REQUIRE(rig.cache.want(keyRange(100, 150), LevelUse::Display));
    rig.drain();
    auto const snapshot = rig.cache.snapshot();
    for (auto const& key : keys(150)) {
        CHECK(stateOf(*snapshot, key) == (std::get<std::int64_t>(key) < 50 ? LevelRead::Unloaded : LevelRead::Loaded));
    }
    CHECK(rig.cache.stats().evictedKeys == 50);
    std::size_t announcedKeys = 0;
    for (auto const& entry : rig.announced) {
        REQUIRE(entry.keys.has_value());  // no event for an eviction
        announcedKeys += entry.keys->size();
    }
    CHECK(announcedKeys == 150);
    auto const before = rig.callCount();
    REQUIRE(rig.cache.want(keys(10), LevelUse::Display));
    rig.drain();
    REQUIRE(rig.callCount() == before + 1);
    CHECK(rig.calls.back() == keys(10));
}

// Mutation: reading the value by the caller's column order (A's r3 then holds r1's value).
TEST_CASE("level cache: a value's layout is the cache's", "[table][levels]") {
    using tabletest::levelCol;
    Rig rig{[](LevelCacheOptions& options) {
                options.columns = {levelCol("r1", ColumnKind::Text, "recipe"),
                                   levelCol("r2", ColumnKind::Text, "recipe"),
                                   levelCol("r3", ColumnKind::Text, "recipe")};
            },
            [](std::span<RowId const> asked) {
                Values out;
                for (auto const& key : asked) {
                    out.emplace_back(
                        key, std::vector<Cell>{Cell{"a" + text(key)}, Cell{"b" + text(key)}, Cell{"c" + text(key)}});
                }
                return out;
            }};
    auto const base = tabletest::makeSource({tabletest::col("id", ColumnKind::Integer)}, {{Cell{std::int64_t{0}}}});
    REQUIRE(rig.cache.want(keys(1), LevelUse::Display));
    rig.drain();
    std::shared_ptr<LevelSource> const level{std::shared_ptr<void>{}, &rig.cache};
    LeveledSource const a{base,
                          {tabletest::col("id", ColumnKind::Integer), levelCol("r3", ColumnKind::Text, "recipe"),
                           levelCol("r1", ColumnKind::Text, "recipe")},
                          {LevelInput{.source = level, .snapshot = rig.cache.snapshot()}}};
    LeveledSource const b{base,
                          {tabletest::col("id", ColumnKind::Integer), levelCol("r2", ColumnKind::Text, "recipe")},
                          {LevelInput{.source = level, .snapshot = rig.cache.snapshot()}}};
    CHECK(a.snapshot()->cell(0, 1) == Cell{std::string{"c0"}});
    CHECK(a.snapshot()->cell(0, 2) == Cell{std::string{"a0"}});
    CHECK(b.snapshot()->cell(0, 1) == Cell{std::string{"b0"}});
    CHECK(a.state(0, "recipe") == LevelRead::Loaded);
    CHECK(rig.callCount() == 1);
}

// Mutation: SizeOf as sizeof(Value) (nothing is ever evicted).
TEST_CASE("level cache: size is deep", "[table][levels]") {
    std::vector<Cell> const value(20, Cell{std::string(40, 'x')});
    auto const valueBytes = detail::cellsBytes(value);
    Rig rig{[valueBytes](LevelCacheOptions& options) { options.maxBytes = 100 * valueBytes; },
            [&value](std::span<RowId const> asked) {
                Values out;
                for (auto const& key : asked) {
                    out.emplace_back(key, value);
                }
                return out;
            }};
    CHECK(valueBytes > std::size_t{20} * 40);
    REQUIRE(rig.cache.want(keys(150), LevelUse::Display));
    rig.drain();
    CHECK(rig.cache.stats().bytes <= 100 * valueBytes);
    CHECK(rig.cache.stats().evictedKeys == 50);
}

// Mutation: a snapshot holding a shared lock (the stores wait for the reader and time out).
TEST_CASE("level cache: a held snapshot blocks no store", "[table][levels][threads]") {
    Rig rig{[](LevelCacheOptions& options) { options.invalidatedBy = {"T.rows", "Recipe"}; }};
    REQUIRE(rig.cache.want(keys(10), LevelUse::Display));
    rig.drain();
    auto held = rig.cache.snapshot();
    // Each call runs on its own thread; the case waits ten seconds for it, and joins it only after releasing the
    // snapshot, so a store that waited for the reader fails the check instead of hanging the case.
    std::vector<std::thread> calls;
    auto const finishes = [&calls](std::function<void()> work) {
        auto const done = std::make_shared<std::promise<void>>();
        auto const finished = done->get_future();
        calls.emplace_back([work = std::move(work), done] {
            work();
            done->set_value();
        });
        return finished.wait_for(10s) == std::future_status::ready;
    };
    CHECK(finishes([&rig] {
        std::ignore = (rig.cache.want(keyRange(10, 20), LevelUse::Display));
        rig.drain();
    }));
    CHECK(finishes([&rig] { rig.cache.invalidate(keys(1)); }));
    CHECK(finishes([&rig] {
        rig.cache.changed(morph::effects::Changes{.records = {{"T.rows", rawKeys({"2"})}}, .instances = {}});
    }));
    CHECK(readCell(*held, RowId{std::int64_t{0}}) == Cell{std::string{"v0"}});
    CHECK(stateOf(*held, RowId{std::int64_t{15}}) == LevelRead::Unloaded);
    auto const heldGeneration = held->generation();
    held.reset();
    for (auto& call : calls) {
        call.join();
    }
    CHECK(heldGeneration != rig.cache.snapshot()->generation());
}

// Mutation: no lease (the second order use is accepted instead of refused).
TEST_CASE("level cache: order uses are leased and the second is refused", "[table][levels]") {
    auto const value = smallValueBytes();
    auto const adjust = [value](LevelCacheOptions& options) {
        options.maxBytes = 150 * value;
        options.expectedBytes = value;
    };
    {
        Rig rig{adjust};
        REQUIRE(rig.cache.want(keys(100), LevelUse::Order));
        rig.drain();
        CHECK(rig.cache.stats().loadedKeys == 100);
        auto const calls = rig.callCount();
        auto const refused = rig.cache.want(keyRange(100, 200), LevelUse::Order);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == TableErrorCode::Unavailable);
        CHECK(rig.cache.stats().refusedOrders == 1);
        rig.drain();
        CHECK(rig.callCount() == calls);
        REQUIRE(rig.cache.want(keyRange(200, 300), LevelUse::Display));
        rig.drain();
        auto snapshot = rig.cache.snapshot();
        CHECK(std::ranges::all_of(keys(100),
                                  [&](RowId const& key) { return stateOf(*snapshot, key) == LevelRead::Loaded; }));
        rig.now += 61s;
        REQUIRE(rig.cache.want(keyRange(300, 400), LevelUse::Display));
        rig.drain();
        snapshot = rig.cache.snapshot();
        CHECK(std::ranges::any_of(keys(100),
                                  [&](RowId const& key) { return stateOf(*snapshot, key) == LevelRead::Unloaded; }));
    }
    {
        Rig rig{adjust};
        std::size_t afterOne = 0;
        for (int round = 0; round < 10; ++round) {
            std::ignore = (rig.cache.want(keys(100), LevelUse::Order));
            rig.drain();
            std::ignore = (rig.cache.want(keyRange(100, 200), LevelUse::Order));
            rig.drain();
            if (round == 0) {
                afterOne = rig.callCount();
            }
        }
        CHECK(rig.callCount() == afterOne);
    }
}

// Mutation: the over-bound check skipped (all 100 order keys load, and nothing is announced without keys).
TEST_CASE("level cache: leased values outgrowing the bound stop the order load", "[table][levels]") {
    auto const value = smallValueBytes();
    Rig rig{[value](LevelCacheOptions& options) {
        options.maxBytes = 50 * value;
        options.expectedBytes = value / 10;
    }};
    REQUIRE(rig.cache.want(keys(100), LevelUse::Order));
    rig.drain();
    CHECK(rig.callCount() == 1);  // the first batch of 64 overran the bound; the rest were not loaded
    CHECK(std::ranges::any_of(
        rig.announced, [](Announced const& entry) { return entry.record == "T.rows" && !entry.keys.has_value(); }));
    auto const again = rig.cache.want(keys(100), LevelUse::Order);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == TableErrorCode::Unavailable);
    // The lease is dropped: a display load evicts the order values it no longer protects.
    REQUIRE(rig.cache.want(keyRange(200, 210), LevelUse::Display));
    rig.drain();
    CHECK(rig.cache.stats().evictedKeys >= 10);
}

// Mutation: a failed key wanted again by every request (the loader is called inside the delay).
TEST_CASE("level cache: a failed load is remembered", "[table][levels]") {
    Rig rig{{}, [](std::span<RowId const> asked) {
                Values out;
                for (auto const& key : asked) {
                    auto const at = std::get<std::int64_t>(key);
                    if (at == 15) {
                        out.emplace_back(key, std::vector<Cell>{});
                    } else if (at >= 10) {
                        out.emplace_back(key, std::vector<Cell>{Cell{"v" + text(key)}});
                    }
                }
                return out;
            }};
    std::vector<std::vector<RowId>> failedCalls;
    rig.cache.onLoadFailed(
        [&failedCalls](std::span<RowId const> failed) { failedCalls.emplace_back(failed.begin(), failed.end()); });
    REQUIRE(rig.cache.want(keys(20), LevelUse::Display));
    rig.drain();
    auto const snapshot = rig.cache.snapshot();
    for (auto const& key : keys(10)) {
        CHECK(stateOf(*snapshot, key) == LevelRead::Failed);
    }
    CHECK(rig.cache.stats().failedLoads == 10);
    REQUIRE(failedCalls.size() == 1);
    CHECK(failedCalls[0] == keys(10));
    CHECK(std::ranges::count_if(rig.announced, [](Announced const& entry) { return entry.keys == keys(10); }) == 1);
    std::vector<Cell> empty(1, Cell{std::string{"unwritten"}});
    CHECK(snapshot->read(RowId{std::int64_t{15}}, empty) == LevelRead::Loaded);  // a row with nothing at the level
    CHECK(std::holds_alternative<std::monostate>(empty[0]));

    auto const calls = rig.callCount();
    REQUIRE(rig.cache.want(keys(10), LevelUse::Display));
    rig.drain();
    CHECK(rig.callCount() == calls);  // inside retryAfter
    rig.now += 30s;
    REQUIRE(rig.cache.want(keys(10), LevelUse::Display));
    rig.drain();
    CHECK(rig.callCount() == calls + 1);  // once, and it fails again
    rig.now += 30s;
    REQUIRE(rig.cache.want(keys(10), LevelUse::Display));
    rig.drain();
    CHECK(rig.callCount() == calls + 1);  // the second failure waits 60 s
    rig.now += 30s;
    REQUIRE(rig.cache.want(keys(10), LevelUse::Display));
    rig.drain();
    CHECK(rig.callCount() == calls + 2);
    REQUIRE(rig.cache.want(keys(10), LevelUse::Display, true));
    rig.drain();
    CHECK(rig.callCount() == calls + 3);  // retry: at once
    rig.cache.invalidate(std::vector<RowId>{RowId{std::int64_t{3}}});
    CHECK(stateOf(*rig.cache.snapshot(), RowId{std::int64_t{3}}) == LevelRead::Unloaded);

    Rig throwing{{}, [](std::span<RowId const>) -> Values { throw std::runtime_error{"the store is down"}; }};
    REQUIRE(throwing.cache.want(keys(5), LevelUse::Display));
    throwing.drain();
    CHECK(throwing.cache.stats().failedLoads == 5);
    CHECK(stateOf(*throwing.cache.snapshot(), RowId{std::int64_t{4}}) == LevelRead::Failed);
}

namespace {

// Queues like `ManualExecutor`, but throws from the first `throws` posts.
class ThrowingExecutor final : public morph::exec::IExecutor {
public:
    explicit ThrowingExecutor(int throws) : _throws{throws} {}
    void post(std::function<void()> task) override {
        if (_throws > 0) {
            --_throws;
            throw std::runtime_error{"the executor is closing"};
        }
        queue.post(std::move(task));
    }
    [[nodiscard]] bool isSerial() const noexcept override { return false; }
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): `queue` is the manual executor the cases step directly
    tabletest::ManualExecutor queue;

private:
    int _throws;
};

}  // namespace

// Mutations: tell() calling the sink and the failure handler unguarded (the second want loads nothing); dispatch
// without catching a throwing post (the want throws).
TEST_CASE("level cache: a throwing callback or post does not stop the loads", "[table][levels]") {
    {
        Rig rig{{}, [](std::span<RowId const> asked) {
                    Values out;
                    for (auto const& key : asked) {
                        if (std::get<std::int64_t>(key) % 2 == 0) {
                            out.emplace_back(key, std::vector<Cell>{Cell{"v" + text(key)}});
                        }
                    }
                    return out;
                }};
        rig.cache.bind([](std::string_view, std::optional<std::vector<glz::raw_json>> const&) {
            throw std::runtime_error{"the server is gone"};
        });
        rig.cache.onLoadFailed([](std::span<RowId const>) { throw std::runtime_error{"the handler failed"}; });
        // More keys than one batch: a throw that escaped the first load would leave the second batch undispatched.
        REQUIRE(rig.cache.want(keys(100), LevelUse::Display));
        try {
            rig.drain();
            // NOLINTNEXTLINE(bugprone-empty-catch): the case looks for a throw escaping the load; what the cache does next shows its effect, so the throw itself is not inspected
        } catch (...) {
            // A throw escaping the load is the defect this case looks for; what follows shows its effect.
        }
        REQUIRE(rig.cache.want(keyRange(100, 120), LevelUse::Display));
        try {
            rig.drain();
            // NOLINTNEXTLINE(bugprone-empty-catch): the same drain as above, repeated after the second batch; a throw escaping it is what the following checks would show
        } catch (...) {
        }
        CHECK(rig.loggedKeys() == keys(120));
        CHECK(rig.cache.stats().loadedKeys == 60);
    }
    {
        ThrowingExecutor loads{1};
        Rig rig{[&loads](LevelCacheOptions& options) { options.loads = &loads; }};
        auto const first = [&] { return rig.cache.want(keys(5), LevelUse::Display); };
        std::optional<std::expected<void, TableError>> wanted;
        CHECK_NOTHROW(wanted = first());
        CHECK(loads.queue.queued() == 0);
        REQUIRE(rig.cache.want(keys(5), LevelUse::Display));
        tabletest::drain(loads.queue);
        CHECK(rig.loggedKeys() == keys(5));
        CHECK(stateOf(*rig.cache.snapshot(), RowId{std::int64_t{4}}) == LevelRead::Loaded);
    }
}

// Mutation: want pushing a queued key again on every want (the queue holds ten entries a key).
TEST_CASE("level cache: the wait queue holds a key once", "[table][levels]") {
    Rig rig;
    auto const many = keys(100'000);
    for (int round = 0; round < 10; ++round) {
        REQUIRE(rig.cache.want(many, LevelUse::Order));
    }
    // The first want's batch is in flight; the rest of the keys wait, once each.
    CHECK(detail::LevelCacheAccess::queuedEntries(rig.cache) + 64 == 100'000);
}

// Mutation: the eviction scan run under the writer mutex (the invalidation waits for the paused store).
TEST_CASE("level cache: an invalidation does not wait for a store's eviction", "[table][levels][threads]") {
    morph::exec::ThreadPoolExecutor pool{1};
    Rig rig{[&pool](LevelCacheOptions& options) {
        options.loads = &pool;
        options.maxBytes = 10 * smallValueBytes();
    }};
    Values preload;
    for (auto const& key : keys(10)) {
        preload.emplace_back(key, std::vector<Cell>{Cell{"v" + text(key)}});
    }
    detail::LevelCacheAccess::store(rig.cache, preload);
    std::atomic<bool> paused{false};
    std::promise<void> resume;
    auto const resumed = resume.get_future().share();
    std::atomic<bool> first{true};
    detail::LevelCacheAccess::onEvictionScan(rig.cache, [&] {
        if (first.exchange(false)) {
            paused = true;
            static_cast<void>(resumed.wait_for(10s));
        }
    });
    REQUIRE(rig.cache.want(keyRange(10, 15), LevelUse::Display));  // over the bound: the store evicts, and pauses
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!paused.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(paused.load());
    auto invalidation = std::async(std::launch::async, [&rig] { rig.cache.invalidate(keys(2)); });
    bool const done = invalidation.wait_for(5s) == std::future_status::ready;
    resume.set_value();
    CHECK(done);
    invalidation.get();
    std::promise<void> idle;
    pool.post([&idle] { idle.set_value(); });
    idle.get_future().wait();
    CHECK(stateOf(*rig.cache.snapshot(), RowId{std::int64_t{0}}) == LevelRead::Unloaded);
    CHECK(rig.cache.stats().bytes <= 10 * smallValueBytes());
}

// Mutation: a want of a queued key leaving it in its place (B's keys load before A's).
TEST_CASE("level cache: a key wanted again moves to the front", "[table][levels]") {
    Rig rig{[](LevelCacheOptions& options) { options.batch = 5; }};
    REQUIRE(rig.cache.want(keys(10), LevelUse::Display));          // page A: 0-4 in flight, 5-9 queued
    REQUIRE(rig.cache.want(keyRange(10, 20), LevelUse::Display));  // page B
    REQUIRE(rig.cache.want(keys(10), LevelUse::Display));          // page A again
    CHECK(detail::LevelCacheAccess::queuedEntries(rig.cache) == 15);
    rig.drain();
    REQUIRE(rig.calls.size() == 4);
    CHECK(rig.calls[1] == keyRange(5, 10));
    CHECK(rig.calls[2] == keyRange(10, 15));
}

// Mutation: a failure evicted inside its retry delay (the failed keys read unloaded after the next store).
TEST_CASE("level cache: a failure stays through its retry delay under memory pressure", "[table][levels]") {
    Rig rig{[](LevelCacheOptions& options) { options.maxBytes = 20 * smallValueBytes(); },
            [](std::span<RowId const> asked) {
                Values out;
                for (auto const& key : asked) {
                    if (std::get<std::int64_t>(key) >= 5) {
                        out.emplace_back(key, std::vector<Cell>{Cell{"v" + text(key)}});
                    }
                }
                return out;
            }};
    REQUIRE(rig.cache.want(keys(5), LevelUse::Display));  // all five fail
    rig.drain();
    REQUIRE(rig.cache.want(keyRange(5, 80), LevelUse::Display));  // 75 values in two batches: over the bound
    rig.drain();
    CHECK(rig.cache.stats().evictedKeys > 0);
    auto const snapshot = rig.cache.snapshot();
    for (auto const& key : keys(5)) {
        CHECK(stateOf(*snapshot, key) == LevelRead::Failed);
    }
}

// Mutation: invalidate advancing the version only when it removes a value (the store publishes key 1's stale value).
TEST_CASE("level cache: an invalidation between a store's preparation and its publication is not lost",
          "[table][levels]") {
    Rig rig;
    std::vector<RowId> const one{RowId{std::int64_t{1}}};
    bool first = true;
    detail::LevelCacheAccess::onStorePrepared(rig.cache, [&] {
        if (std::exchange(first, false)) {
            rig.cache.invalidate(one);  // the key is in flight: the store has prepared its old value
        }
    });
    REQUIRE(rig.cache.want(one, LevelUse::Display));
    rig.drain();
    CHECK(stateOf(*rig.cache.snapshot(), one[0]) == LevelRead::Unloaded);
    CHECK(rig.cache.stats().droppedLoads == 1);
    REQUIRE(rig.cache.want(one, LevelUse::Display));
    rig.drain();
    CHECK(rig.callCount() == 2);
    CHECK(stateOf(*rig.cache.snapshot(), one[0]) == LevelRead::Loaded);
}

// Mutation: one chunk spanning the whole want (the want never stops between chunks, so the hook never pauses it).
TEST_CASE("level cache: an invalidation waits for one chunk of a large want at most", "[table][levels][threads]") {
    Rig rig;
    std::atomic<bool> paused{false};
    std::atomic<std::size_t> queuedAtPause{0};
    std::promise<void> resume;
    auto const resumed = resume.get_future().share();
    detail::LevelCacheAccess::onWantChunk(rig.cache, [&](std::size_t chunk) {
        if (chunk == 1) {
            queuedAtPause = detail::LevelCacheAccess::queuedEntries(rig.cache);
            paused = true;
            static_cast<void>(resumed.wait_for(10s));
        }
    });
    auto wanting = std::async(std::launch::async, [&rig] { return rig.cache.want(keys(100'000), LevelUse::Order); });
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!paused.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(paused.load());
    CHECK(queuedAtPause.load() == detail::kWantChunk);  // the first chunk is queued, and the mutex is free
    auto invalidation = std::async(std::launch::async, [&rig] { rig.cache.invalidate(keys(2)); });
    bool const done = invalidation.wait_for(5s) == std::future_status::ready;
    resume.set_value();
    CHECK(done);
    invalidation.get();
    CHECK(wanting.get().has_value());
}

// Mutation: a chunk giving a key it finds held a pending lease instead of leasing its slot (the keys stored before
// their chunk are held with no lease).
TEST_CASE("level cache: a key stored while an order want runs is leased on its slot", "[table][levels]") {
    Rig rig;
    auto const chunk = static_cast<std::int64_t>(detail::kWantChunk);
    auto const beforeFirst = keyRange(0, 10);               // stored after the fit, before their chunk
    auto const afterFirst = keyRange(10, 20);               // stored after their chunk gave them a pending lease
    auto const beforeSecond = keyRange(chunk, chunk + 10);  // stored before the second chunk
    detail::LevelCacheAccess::onWantChunk(rig.cache, [&](std::size_t index) {
        if (index == 0) {
            detail::LevelCacheAccess::store(rig.cache, valuesOf(beforeFirst));
        } else if (index == 1) {
            detail::LevelCacheAccess::store(rig.cache, valuesOf(afterFirst));
            detail::LevelCacheAccess::store(rig.cache, valuesOf(beforeSecond));
        }
    });
    REQUIRE(rig.cache.want(keys(2 * chunk), LevelUse::Order));
    auto const nowTicks = rig.now.time_since_epoch().count();
    for (auto const* const stored : {&beforeFirst, &afterFirst, &beforeSecond}) {
        for (auto const& key : *stored) {
            CHECK(detail::LevelCacheAccess::leaseEnd(rig.cache, key) > nowTicks);
            CHECK_FALSE(detail::LevelCacheAccess::leasePending(rig.cache, key));
        }
    }
    CHECK(detail::LevelCacheAccess::leasePending(rig.cache, RowId{chunk + 10}));  // not stored: still loading
}

// Mutation: the chunks reading the state the fit read (the evicted key is leased on its removed slot and never
// loaded again).
TEST_CASE("level cache: a held key evicted while an order want runs is queued again", "[table][levels]") {
    auto const value = smallValueBytes();
    Rig rig{[value](LevelCacheOptions& options) { options.maxBytes = 2'100 * value; }};
    RowId const held{std::int64_t{2'000}};  // in the want's second chunk
    detail::LevelCacheAccess::store(rig.cache, valuesOf({held}));
    detail::LevelCacheAccess::onWantChunk(rig.cache, [&](std::size_t index) {
        if (index == 1) {
            // One value over the bound: the held key, the least recently used and not yet leased, is evicted.
            detail::LevelCacheAccess::store(rig.cache, valuesOf(keyRange(10'000, 12'100)));
        }
    });
    REQUIRE(rig.cache.want(keys(2 * static_cast<std::int64_t>(detail::kWantChunk)), LevelUse::Order));
    CHECK(rig.cache.stats().evictedKeys == 1);
    CHECK(stateOf(*rig.cache.snapshot(), held) == LevelRead::Unloaded);
    CHECK(detail::LevelCacheAccess::leasePending(rig.cache, held));
    rig.drain();
    CHECK(std::ranges::binary_search(rig.loggedKeys(), held));
    CHECK(stateOf(*rig.cache.snapshot(), held) == LevelRead::Loaded);
    CHECK(detail::LevelCacheAccess::leaseEnd(rig.cache, held) > rig.now.time_since_epoch().count());
}

// Mutation: a set's last-but-one holder leaving ending the set's protection (the first query's unwant ends the
// second query's lease).
TEST_CASE("level cache: a repeated order want keeps its lease when another holder takes its want back",
          "[table][levels]") {
    auto const value = smallValueBytes();
    Rig rig{[value](LevelCacheOptions& options) {
        options.maxBytes = (150 * value) + setKeyBytes(100);  // the set's keys count in the bound
        options.expectedBytes = value;
    }};
    auto const set = std::make_shared<std::vector<RowId> const>(keys(100));
    REQUIRE(rig.cache.want(*set, LevelUse::Display));
    rig.drain();
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));  // query A: the set is fitted and accepted
    auto const fits = detail::LevelCacheAccess::fits(rig.cache);
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));  // query B: a repeat, not fitted again
    REQUIRE(detail::LevelCacheAccess::fits(rig.cache) == fits);
    auto const others = keyRange(100, 200);  // fits only while the set is not leased
    rig.cache.unwant(*set);                  // A is refused by another level and takes its want back
    CHECK_FALSE(rig.cache.want(others, LevelUse::Order).has_value());
    rig.cache.unwant(*set);  // B goes too
    CHECK(rig.cache.want(others, LevelUse::Order).has_value());
}

// Mutation: no single flight for a set (the second want takes every key while the first is paused).
TEST_CASE("level cache: concurrent repeats of an order set are fitted once", "[table][levels][threads]") {
    auto const value = smallValueBytes();
    Rig rig{[value](LevelCacheOptions& options) {
        options.maxBytes = (150 * value) + setKeyBytes(100);  // the set's keys count in the bound
        options.expectedBytes = value;
    }};
    auto const set = std::make_shared<std::vector<RowId> const>(keys(100));
    REQUIRE(rig.cache.want(*set, LevelUse::Display));
    rig.drain();
    std::atomic<bool> paused{false};
    std::atomic<bool> first{true};
    std::promise<void> resume;
    auto const resumed = resume.get_future().share();
    detail::LevelCacheAccess::onWantChunk(rig.cache, [&](std::size_t) {
        if (first.exchange(false)) {
            paused = true;
            static_cast<void>(resumed.wait_for(10s));
        }
    });
    auto const fits = detail::LevelCacheAccess::fits(rig.cache);
    auto one = std::async(std::launch::async, [&] { return rig.cache.wantSet(set, LevelUse::Order); });
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!paused.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(paused.load());
    auto two = std::async(std::launch::async, [&] { return rig.cache.wantSet(set, LevelUse::Order); });
    CHECK(two.wait_for(1s) == std::future_status::timeout);  // it waits for the first
    resume.set_value();
    CHECK(one.get().has_value());
    CHECK(two.get().has_value());
    CHECK(detail::LevelCacheAccess::fits(rig.cache) == fits + 1);
    auto const others = keyRange(100, 200);  // fits only while the set is not leased
    rig.cache.unwant(*set);
    CHECK_FALSE(rig.cache.want(others, LevelUse::Order).has_value());  // the second still holds the lease
}

// Mutation: unwant ending a lease whatever its holders (the second order use is accepted).
TEST_CASE("level cache: an order lease ends with its last holder", "[table][levels]") {
    auto const value = smallValueBytes();
    Rig rig{[value](LevelCacheOptions& options) {
        options.maxBytes = 150 * value;
        options.expectedBytes = value;
    }};
    REQUIRE(rig.cache.want(keys(100), LevelUse::Order));
    rig.drain();
    REQUIRE(rig.cache.want(keys(100), LevelUse::Order));  // a second query orders by the same rows
    auto const others = keyRange(100, 200);
    rig.cache.unwant(keys(100));  // the first query is refused elsewhere and takes its want back
    CHECK_FALSE(rig.cache.want(others, LevelUse::Order).has_value());
    rig.cache.unwant(keys(100));  // the second goes too
    CHECK(rig.cache.want(others, LevelUse::Order).has_value());
}

// Mutation: a repeat of an accepted set not fitted again whatever its lease (the set is not fitted once its lease
// has run out).
TEST_CASE("level cache: a repeated order want of an accepted set is not fitted again", "[table][levels]") {
    Rig rig;
    auto const set = std::make_shared<std::vector<RowId> const>(keys(100));
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    rig.drain();
    auto const fits = detail::LevelCacheAccess::fits(rig.cache);
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    CHECK(detail::LevelCacheAccess::fits(rig.cache) == fits);
    rig.cache.invalidate(std::vector<RowId>{RowId{std::int64_t{3}}});
    auto const calls = rig.callCount();
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));  // a repeat still queues the key it finds missing
    rig.drain();
    REQUIRE(rig.callCount() == calls + 1);
    CHECK(rig.calls.back() == std::vector<RowId>{RowId{std::int64_t{3}}});
    CHECK(detail::LevelCacheAccess::fits(rig.cache) == fits);
    rig.now += 61s;  // the lease has run out: the set is fitted again
    REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    CHECK(detail::LevelCacheAccess::fits(rig.cache) == fits + 1);
}

namespace {

// A set of 2,048 keys, two chunks, held and leased by one accepted order use, under a bound that fits another order
// use of 2,048 keys only while fewer than all of the set's keys are leased.
struct HeldSet {
    HeldSet()
        : rig{[](LevelCacheOptions& options) {
              options.maxBytes = 4'095 * smallValueBytes();
              options.expectedBytes = smallValueBytes();
          }},
          set{std::make_shared<std::vector<RowId> const>(keys(2 * static_cast<std::int64_t>(detail::kWantChunk)))} {
        REQUIRE(rig.cache.want(*set, LevelUse::Display));
        rig.drain();
        REQUIRE(rig.cache.wantSet(set, LevelUse::Order));
    }

    [[nodiscard]] bool othersFit() {
        auto const fitted =
            rig.cache.want(keyRange(10'000, 10'000 + static_cast<std::int64_t>(set->size())), LevelUse::Order);
        return fitted.has_value();
    }

    Rig rig;
    std::shared_ptr<std::vector<RowId> const> set;
};

}  // namespace

// Mutation: a set's last-but-one holder leaving ending the set's protection (the two refusals unprotect the keys the
// third query still orders by).
TEST_CASE("level cache: two refusals of one set leave a third holder's lease", "[table][levels][threads]") {
    HeldSet held;                                                // query A
    REQUIRE(held.rig.cache.wantSet(held.set, LevelUse::Order));  // query B
    REQUIRE(held.rig.cache.wantSet(held.set, LevelUse::Order));  // query C
    std::atomic<bool> paused{false};
    std::atomic<bool> first{true};
    std::promise<void> resume;
    auto const resumed = resume.get_future().share();
    detail::LevelCacheAccess::onReleaseChunk(held.rig.cache, [&](std::size_t chunk) {
        if (chunk == 1 && first.exchange(false)) {
            paused = true;
            static_cast<void>(resumed.wait_for(10s));
        }
    });
    // A's unwant, paused between chunks should it take the keys one by one, while B's runs whole.
    auto refusedA = std::async(std::launch::async, [&] { held.rig.cache.unwant(*held.set); });
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!paused.load() && refusedA.wait_for(0s) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    held.rig.cache.unwant(*held.set);
    resume.set_value();
    refusedA.get();
    CHECK(detail::LevelCacheAccess::kept(held.rig.cache, held.set->front()));
    CHECK(detail::LevelCacheAccess::kept(held.rig.cache, held.set->back()));
    CHECK_FALSE(held.othersFit());  // C still orders by the set
}

// Mutation: a set's last-but-one holder leaving ending the set's protection (A's unwant unprotects the keys B's
// repeat holds).
TEST_CASE("level cache: a repeat interleaved with another holder's unwant keeps every key leased",
          "[table][levels][threads]") {
    HeldSet held;  // query A
    std::atomic<bool> paused{false};
    std::atomic<bool> first{true};
    std::promise<void> resume;
    auto const resumed = resume.get_future().share();
    detail::LevelCacheAccess::onWantChunk(held.rig.cache, [&](std::size_t chunk) {
        if (chunk == 1 && first.exchange(false)) {
            paused = true;
            static_cast<void>(resumed.wait_for(10s));
        }
    });
    // B's repeat, paused between chunks should it take the keys one by one, while A, refused elsewhere, takes its
    // want back.
    auto repeatB = std::async(std::launch::async, [&] { return held.rig.cache.wantSet(held.set, LevelUse::Order); });
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!paused.load() && repeatB.wait_for(0s) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    held.rig.cache.unwant(*held.set);
    resume.set_value();
    CHECK(repeatB.get().has_value());
    detail::LevelCacheAccess::onWantChunk(held.rig.cache, {});
    CHECK(detail::LevelCacheAccess::kept(held.rig.cache, held.set->front()));
    CHECK(detail::LevelCacheAccess::kept(held.rig.cache, held.set->back()));
    CHECK_FALSE(held.othersFit());  // B still orders by the set
}

// Mutation: a repeat marking the event seen for every accepted set (X's repeat consumes it, and SY's failed key is
// never wanted again).
TEST_CASE("level cache: a failure only one set holds is retried on that set's repeat after another set's",
          "[table][levels]") {
    RowId const failing{std::int64_t{100}};
    Rig rig{{}, [&](std::span<RowId const> asked) {
                Values out;
                for (auto const& key : asked) {
                    if (key != failing) {
                        out.emplace_back(key, std::vector<Cell>{Cell{"v" + text(key)}});
                    }
                }
                return out;
            }};
    auto const setX = std::make_shared<std::vector<RowId> const>(keys(10));
    auto setYKeys = keys(10);
    setYKeys.push_back(failing);
    auto const setY = std::make_shared<std::vector<RowId> const>(std::move(setYKeys));
    REQUIRE(rig.cache.wantSet(setX, LevelUse::Order));
    REQUIRE(rig.cache.wantSet(setY, LevelUse::Order));
    rig.drain();
    REQUIRE(stateOf(*rig.cache.snapshot(), failing) == LevelRead::Failed);
    rig.now += 31s;  // past the failure's delay, within both sets' leases
    REQUIRE(rig.cache.wantSet(setX, LevelUse::Order));
    auto const calls = rig.callCount();
    REQUIRE(rig.cache.wantSet(setY, LevelUse::Order));
    rig.drain();
    REQUIRE(rig.callCount() == calls + 1);
    CHECK(rig.calls.back() == std::vector<RowId>{failing});
}

// Mutation: a repeat marking the event seen for every accepted set (X's repeat consumes it, and SY's invalidated
// key is never wanted again).
TEST_CASE("level cache: a protected key invalidated in flight is queued again on its set's repeat after another set's",
          "[table][levels]") {
    RowId const flying{std::int64_t{100}};
    Rig rig;
    auto const setX = std::make_shared<std::vector<RowId> const>(keys(10));
    auto setYKeys = keys(10);
    setYKeys.push_back(flying);
    auto const setY = std::make_shared<std::vector<RowId> const>(std::move(setYKeys));
    REQUIRE(rig.cache.wantSet(setX, LevelUse::Order));
    rig.drain();
    REQUIRE(rig.cache.wantSet(setX, LevelUse::Order));  // wanted at the current generation, so SY's fit keeps it
    REQUIRE(rig.cache.wantSet(setY, LevelUse::Order));  // the key is posted to load
    rig.cache.invalidate(std::vector<RowId>{flying});   // in flight, not held: nothing is published
    rig.drain();                                        // its stale load is dropped
    REQUIRE(stateOf(*rig.cache.snapshot(), flying) == LevelRead::Unloaded);
    REQUIRE(rig.cache.stats().droppedLoads == 1);
    REQUIRE(detail::LevelCacheAccess::acceptedSets(rig.cache) == 2);
    auto const fits = detail::LevelCacheAccess::fits(rig.cache);
    REQUIRE(rig.cache.wantSet(setX, LevelUse::Order));
    REQUIRE(detail::LevelCacheAccess::fits(rig.cache) == fits);  // a repeat, not a fit
    auto const calls = rig.callCount();
    REQUIRE(rig.cache.wantSet(setY, LevelUse::Order));
    rig.drain();
    REQUIRE(rig.callCount() == calls + 1);
    CHECK(rig.calls.back() == std::vector<RowId>{flying});
    CHECK(stateOf(*rig.cache.snapshot(), flying) == LevelRead::Loaded);
}

// Mutation: a fit keeping the superseded sets (SX, not wanted since the level's generation moved, stays accepted and
// keeps its keys protected).
TEST_CASE("level cache: a set superseded by the level's generation is released at the next fit", "[table][levels]") {
    Rig rig;
    auto const setX = std::make_shared<std::vector<RowId> const>(keys(10));
    auto const setY = std::make_shared<std::vector<RowId> const>(keyRange(10, 20));
    REQUIRE(rig.cache.wantSet(setX, LevelUse::Order));
    rig.drain();
    REQUIRE(rig.cache.wantSet(setX, LevelUse::Order));  // wanted at the generation its keys were stored at
    detail::LevelCacheAccess::store(rig.cache, valuesOf(keyRange(50, 51)));  // the generation moves
    REQUIRE(rig.cache.wantSet(setY, LevelUse::Order));
    CHECK(detail::LevelCacheAccess::acceptedSets(rig.cache) == 1);
    CHECK_FALSE(detail::LevelCacheAccess::kept(rig.cache, RowId{std::int64_t{0}}));
    // Eleven values (SX's ten and the one stored) and SY's keys; SX's keys have left the count.
    auto const setYBytes = sizeof(std::vector<RowId>) + (10 * rowIdBytes(RowId{std::int64_t{10}}));
    CHECK(rig.cache.stats().bytes == (11 * smallValueBytes()) + setYBytes);
}

// Mutation: the fit leaving out the set's own key bytes (the set is accepted, and the cache holds more than its
// bound once the values load).
TEST_CASE("level cache: a set that fits on its values but not with its keys is refused", "[table][levels]") {
    auto const set = std::make_shared<std::vector<RowId> const>(keys(100));
    auto const keyBytes = sizeof(std::vector<RowId>) + (100 * rowIdBytes(RowId{std::int64_t{0}}));
    auto const maxBytes = (100 * smallValueBytes()) + keyBytes - 1;
    Rig rig{[&](LevelCacheOptions& options) {
        options.maxBytes = maxBytes;
        options.expectedBytes = smallValueBytes();
    }};
    auto const refused = rig.cache.wantSet(set, LevelUse::Order);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::Unavailable);
    CHECK(detail::LevelCacheAccess::acceptedSets(rig.cache) == 0);
    rig.drain();
    CHECK(rig.callCount() == 0);
    CHECK(rig.cache.stats().bytes <= maxBytes);
}
