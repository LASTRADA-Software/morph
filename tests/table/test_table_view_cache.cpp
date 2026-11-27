// SPDX-License-Identifier: Apache-2.0
//
// morph::table's server view cache (docs/spec/table/table.md, "The view cache"): a cached view is the view `apply`
// computes, an entry is valid while its snapshot's identity and its order levels' generations hold, requests for an
// entry being computed wait for that one computation and no other, the bound evicts the least recently used entry,
// and a keyed fetch reads the entry it finds. Each case names the mutation it was seen to fail on.

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <latch>
#include <memory>
#include <morph/table/description.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/table/query.hpp>
#include <morph/table/view_cache.hpp>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using namespace std::chrono_literals;

namespace {

using Levels = std::vector<std::pair<std::string, std::uint64_t>>;
using Values = std::vector<std::pair<RowId, std::vector<Cell>>>;

std::vector<ColumnInfo> fourColumns() {
    return {tabletest::col("a", ColumnKind::Integer), tabletest::col("b", ColumnKind::Text),
            tabletest::levelCol("c", ColumnKind::Decimal, "recipe"),
            tabletest::levelCol("d", ColumnKind::Text, "recipe")};
}

std::string word(std::mt19937& random) {
    static constexpr std::string_view kLetters = "abcde";
    std::string out;
    auto const length = 1 + (random() % 4);
    for (std::uint32_t i = 0; i < length; ++i) {
        out += kLetters[random() % kLetters.size()];
    }
    return out;
}

// A 300-row table: `a` and `b` in the base source, `c` and `d` at level `recipe` in a cache holding 70% of the
// rows, its loads queued and never run.
struct Table {
    explicit Table(std::mt19937& random, std::size_t rows = 300) {
        std::vector<std::vector<Cell>> base;
        for (std::size_t row = 0; row < rows; ++row) {
            base.push_back({Cell{static_cast<std::int64_t>(random() % 50)}, Cell{word(random)}});
            recipe.emplace_back(
                RowId{static_cast<std::int64_t>(row)},
                std::vector<Cell>{Cell{tabletest::dec(std::to_string(random() % 40) + ".5")}, Cell{word(random)}});
        }
        source = tabletest::makeSource(
            {tabletest::col("a", ColumnKind::Integer), tabletest::col("b", ColumnKind::Text)}, std::move(base));
        LevelCacheOptions options;
        options.records = {"T.rows"};
        options.level = "recipe";
        options.columns = {fourColumns()[2], fourColumns()[3]};
        options.loads = &loads;
        cache = std::make_shared<CellLevelCache>(std::move(options), [](std::span<RowId const>) { return Values{}; });
        Values held;
        for (auto const& entry : recipe) {
            if (random() % 10 < 7) {
                held.push_back(entry);
            }
        }
        detail::LevelCacheAccess::store(*cache, std::move(held));
    }

    [[nodiscard]] LeveledSource leveled() const {
        return LeveledSource{source, fourColumns(), {LevelInput{.source = cache, .snapshot = cache->snapshot()}}};
    }

    tabletest::ManualExecutor loads;
    std::shared_ptr<VectorSource> source;
    std::shared_ptr<CellLevelCache> cache;
    Values recipe;
};

TableQuery randomQuery(std::mt19937& random) {
    static constexpr std::array<std::string_view, 4> kColumns{"a", "b", "c", "d"};
    TableQuery query;
    auto const keys = 1 + (random() % 2);
    for (std::uint32_t i = 0; i < keys; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): `random() % 4` is below the 4 entries of `kColumns`
        std::string const column{kColumns[random() % 4]};
        if (std::ranges::find(query.sort, column, &SortKey::column) == query.sort.end()) {
            query.sort.push_back(SortKey{
                .column = column, .dir = random() % 2 == 0 ? SortDirection::Ascending : SortDirection::Descending});
        }
    }
    auto const entries = 1 + (random() % 2);
    for (std::uint32_t i = 0; i < entries; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): `random() % 4` is below the 4 entries of `kColumns`
        std::string const column{kColumns[random() % 4]};
        FilterEntry entry;
        if (column == "a") {
            entry.gt = std::to_string(random() % 50);
        } else if (column == "c") {
            entry.ge = std::to_string(random() % 40);
        } else {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): `random() % 5` is below the 5 characters of the literal
            entry.contains = std::string(1, "abcde"[random() % 5]);
        }
        query.filters.columns.insert_or_assign(
            column, ColumnFilter{.combine = Combine::Any, .include = {entry}, .exclude = {}});
    }
    query.page = PageRequest{.offset = 0, .limit = 50};
    return query;
}

Levels orderLevels(TableQuery const& query, CellLevelCache const& cache) {
    auto const needed = levelsNeeded(query, fourColumns());
    Levels out;
    for (auto const& level : needed.order) {
        out.emplace_back(level, cache.snapshot()->generation());
    }
    return out;
}

ViewCache::Found findIn(ViewCache& views, ViewKey const& key, LeveledSource const& leveled, Table const& table,
                        TableQuery const& validated) {
    return views.find(key, table.source->snapshot(), orderLevels(validated, *table.cache),
                      [&]() -> std::expected<ViewComputation, TableError> {
                          auto view = detail::computeView(leveled, validated, ApplyOptions{});
                          if (!view) {
                              return std::unexpected(view.error());
                          }
                          return ViewComputation{.view = std::move(*view), .awaiting = {}, .failed = {}};
                      });
}

// The page of `validated` from `entry`, as answerQuery reads it.
PageRows pageFrom(ViewEntry const& entry, LeveledSource const& leveled, TableQuery const& validated) {
    if (validated.keys) {
        return detail::pageRows(entry.view, *leveled.snapshot(), validated, QueryLimits{}, entry.viewRowOf(),
                                [&entry](RowId const& key) { return entry.sourceRowOf(key); });
    }
    return detail::pageRows(entry.view, *leveled.snapshot(), validated, QueryLimits{});
}

bool samePage(PageRows const& a, PageRows const& b) {
    return a.rows == b.rows && a.total == b.total && a.totalKind == b.totalKind && a.undecided == b.undecided &&
           a.indices == b.indices && a.offset == b.offset;
}

TableQuery validated(TableQuery const& query) {
    auto checked = validate(query, fourColumns(), QueryLimits{}, {});
    REQUIRE(checked.has_value());
    return *checked;
}

ViewKey keyOf(std::string query) { return ViewKey{.record = "T.rows", .scope = {}, .query = std::move(query)}; }

}  // namespace

// Mutation: an entry valid whatever its order levels' generations, which serves the old order.
TEST_CASE("view cache: a cached view is the view", "[table][views]") {
    for (std::uint32_t seed = 0; seed < 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 random{seed};
        Table table{random};
        ViewCache views;
        auto const query = validated(randomQuery(random));
        auto const key = keyOf("q");
        auto const pageOf = [&](TableQuery const& asked) {
            auto const leveled = table.leveled();
            auto entry = findIn(views, key, leveled, table, query);
            REQUIRE(entry.has_value());
            auto const expected = apply(leveled, asked);
            REQUIRE(expected.has_value());
            CHECK(samePage(pageFrom(**entry, leveled, asked), *expected));
        };
        pageOf(query);
        auto keyed = query;
        keyed.keys.emplace();
        for (int i = 0; i < 10; ++i) {
            keyed.keys->emplace_back(static_cast<std::int64_t>(random() % 300));
        }
        pageOf(keyed);
        auto second = query;
        second.page.offset = 50;
        pageOf(second);
        // An arrival on the level: every row stored, so the order and the undecided rows change.
        detail::LevelCacheAccess::store(*table.cache, table.recipe);
        pageOf(query);
    }
}

// Mutation: as the case above (an entry valid whatever its order levels' generations).
TEST_CASE("view cache: an arrival on an order level computes again; on a display level it does not",
          "[table][views]") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 7 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 random{7};
    {
        Table const table{random};
        ViewCache views;
        TableQuery query;
        query.sort = {SortKey{.column = "c", .dir = SortDirection::Ascending}};
        auto const checked = validated(query);
        REQUIRE(findIn(views, keyOf("c"), table.leveled(), table, checked).has_value());
        detail::LevelCacheAccess::store(*table.cache, table.recipe);
        REQUIRE(findIn(views, keyOf("c"), table.leveled(), table, checked).has_value());
        CHECK(views.stats().computations == 2);
    }
    {
        Table const table{random};
        ViewCache views;
        TableQuery query;
        query.sort = {SortKey{.column = "a", .dir = SortDirection::Ascending}};
        query.columns = std::vector<std::string>{"a", "d"};
        auto const checked = validated(query);
        REQUIRE(findIn(views, keyOf("a"), table.leveled(), table, checked).has_value());
        detail::LevelCacheAccess::store(*table.cache, table.recipe);  // the display level `d` changes
        REQUIRE(findIn(views, keyOf("a"), table.leveled(), table, checked).has_value());
        CHECK(views.stats().computations == 1);
        CHECK(views.stats().hits == 1);
    }
}

// Mutation: single flight joined on the key alone, which hands B the view over S1.
TEST_CASE("view cache: a request on a newer snapshot does not join an older computation", "[table][views][threads]") {
    auto const source =
        tabletest::makeSource({tabletest::col("a", ColumnKind::Integer)},
                              {{Cell{std::int64_t{3}}}, {Cell{std::int64_t{1}}}, {Cell{std::int64_t{2}}}});
    auto const s1 = source->snapshot();
    source->updateRow(0, {Cell{std::int64_t{0}}});
    auto const s2 = source->snapshot();
    ViewCache views;
    auto const key = keyOf("a");
    auto const computation = [](std::vector<std::uint32_t> const& rows) {
        return [rows]() -> std::expected<ViewComputation, TableError> {
            return ViewComputation{.view = {.rows = rows, .undecided = {}}, .awaiting = {}, .failed = {}};
        };
    };
    std::latch release{1};
    auto a = std::async(std::launch::async, [&] {
        return views.find(key, s1, {}, [&]() -> std::expected<ViewComputation, TableError> {
            release.wait();
            return computation({1, 2, 0})();
        });
    });
    while (views.stats().computations < 1) {
        std::this_thread::sleep_for(1ms);
    }
    auto b = std::async(std::launch::async, [&] { return views.find(key, s2, {}, computation({0, 1, 2})); });
    bool const bReady = b.wait_for(5s) == std::future_status::ready;
    CHECK(bReady);
    release.count_down();
    auto const fromB = b.get();
    REQUIRE(fromB.has_value());
    CHECK((*fromB)->snapshot == s2);
    CHECK((*fromB)->view.rows == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(views.stats().computations == 2);
    CHECK(views.stats().waits == 0);
    REQUIRE(a.get().has_value());
    auto const third = views.find(key, s2, {}, computation({2, 1, 0}));
    REQUIRE(third.has_value());
    CHECK(*third == *fromB);

    // The same snapshot at another order-level generation computes on its own too.
    std::latch hold{1};
    Levels const g1{{"recipe", 1}};
    Levels const g2{{"recipe", 2}};
    auto older = std::async(std::launch::async, [&] {
        return views.find(keyOf("c"), s2, g1, [&]() -> std::expected<ViewComputation, TableError> {
            hold.wait();
            return computation({1})();
        });
    });
    while (views.stats().computations < 3) {
        std::this_thread::sleep_for(1ms);
    }
    auto newer = std::async(std::launch::async, [&] { return views.find(keyOf("c"), s2, g2, computation({2})); });
    bool const newerReady = newer.wait_for(5s) == std::future_status::ready;
    CHECK(newerReady);
    hold.count_down();
    auto const fromNewer = newer.get();
    REQUIRE(fromNewer.has_value());
    CHECK((*fromNewer)->levels == g2);
    REQUIRE(older.get().has_value());
    CHECK(views.stats().waits == 0);
}

// Mutation: no single flight (fifty computations).
TEST_CASE("view cache: fifty requests compute once", "[table][views][threads]") {
    auto const source = tabletest::makeSource({tabletest::col("a", ColumnKind::Integer)}, {{Cell{std::int64_t{1}}}});
    auto const snapshot = source->snapshot();
    ViewCache views;
    std::latch release{1};
    std::vector<std::future<ViewCache::Found>> found;
    for (int i = 0; i < 50; ++i) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `found` is a fixture list built once; sizing it first would not change what the case checks
        found.push_back(std::async(std::launch::async, [&] {
            return views.find(keyOf("a"), snapshot, {}, [&]() -> std::expected<ViewComputation, TableError> {
                release.wait();
                return ViewComputation{.view = {.rows = {0}, .undecided = {}}, .awaiting = {}, .failed = {}};
            });
        }));
    }
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (views.stats().computations + views.stats().waits < 50 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    release.count_down();
    std::vector<std::shared_ptr<ViewEntry const>> entries;
    for (auto& each : found) {
        auto result = each.get();
        REQUIRE(result.has_value());
        entries.push_back(*result);
    }
    CHECK(views.stats().computations == 1);
    CHECK(views.stats().waits == 49);
    CHECK(std::ranges::all_of(entries, [&](auto const& entry) { return entry == entries.front(); }));
}

// Mutation: no bound (nothing is evicted and the bytes exceed maxBytes).
TEST_CASE("view cache: the bound evicts least recently used", "[table][views]") {
    std::vector<std::vector<Cell>> rows;
    for (std::int64_t row = 0; row < 10'000; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
        rows.push_back({Cell{row % 97}});
    }
    auto const source = tabletest::makeSource({tabletest::col("a", ColumnKind::Integer)}, std::move(rows));
    auto const snapshot = source->snapshot();
    ViewCache views{ViewCacheOptions{.maxBytes = 2UZ * 4 * 10'000, .onCompute = {}}};
    auto const compute = [&]() -> std::expected<ViewComputation, TableError> {
        ViewComputation out;
        out.view.rows.resize(10'000);
        // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
        std::iota(out.view.rows.begin(), out.view.rows.end(), 0U);
        return out;
    };
    for (auto const* const name : {"one", "two", "three"}) {
        REQUIRE(views.find(keyOf(name), snapshot, {}, compute).has_value());
    }
    CHECK(views.stats().evictions == 1);
    CHECK(views.stats().bytes <= std::size_t{2} * 4 * 10'000);
    REQUIRE(views.find(keyOf("three"), snapshot, {}, compute).has_value());
    REQUIRE(views.find(keyOf("two"), snapshot, {}, compute).has_value());
    CHECK(views.stats().computations == 3);
    REQUIRE(views.find(keyOf("one"), snapshot, {}, compute).has_value());
    CHECK(views.stats().computations == 4);  // the first was evicted
}

// Mutation: bytes() leaving out the order keys (three entries fit the bound, and none is evicted).
TEST_CASE("view cache: the bound counts a view's order keys", "[table][views]") {
    constexpr std::size_t kRows = 10'000;
    std::vector<std::vector<Cell>> rows;
    for (std::size_t row = 0; row < kRows; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
        rows.push_back({Cell{static_cast<std::int64_t>(row)}});
    }
    auto const source = tabletest::makeSource({tabletest::col("a", ColumnKind::Integer)}, std::move(rows));
    auto const snapshot = source->snapshot();
    auto const orderKeys = [] {
        std::vector<RowId> out;
        out.reserve(kRows);
        for (std::size_t row = 0; row < kRows; ++row) {
            out.emplace_back("an order key longer than a small buffer " + std::to_string(row));
        }
        return out;
    }();
    std::size_t keyBytes = sizeof(std::vector<RowId>);
    for (auto const& key : orderKeys) {
        keyBytes += rowIdBytes(key);
    }
    auto const entryBytes = (4 * kRows) + keyBytes;
    ViewCache views{ViewCacheOptions{.maxBytes = 2 * entryBytes, .onCompute = {}}};
    auto const compute = [&]() -> std::expected<ViewComputation, TableError> {
        ViewComputation out;
        out.view.rows.resize(kRows);
        // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
        std::iota(out.view.rows.begin(), out.view.rows.end(), 0U);
        out.orderKeys = std::make_shared<std::vector<RowId> const>(orderKeys);
        return out;
    };
    for (auto const* const name : {"one", "two", "three"}) {
        REQUIRE(views.find(keyOf(name), snapshot, {}, compute).has_value());
    }
    CHECK(views.stats().evictions == 1);
    CHECK(views.stats().bytes == 2 * entryBytes);
}

// Mutation: a keyed query bypassing the cache (it computes the view again).
TEST_CASE("view cache: a keyed fetch sorts nothing", "[table][views]") {
    std::vector<std::vector<Cell>> rows;
    for (std::int64_t row = 0; row < 1'000; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
        rows.push_back({Cell{row}, Cell{(row * 37) % 101}});
    }
    auto description = describe(TableDescription{
        .id = "t",
        .label = "T",
        .columns = {TableColumn{.info = tabletest::col("id", ColumnKind::Integer), .label = "id", .group = {}},
                    TableColumn{.info = tabletest::col("n", ColumnKind::Integer), .label = "n", .group = {}}},
        .key = "id"});
    description.capabilities.keys = true;
    auto const served = detail::pinned(ServedTable{
        .description = describe(std::move(description)),
        .source = tabletest::makeSource(
            {tabletest::col("id", ColumnKind::Integer), tabletest::col("n", ColumnKind::Integer)}, std::move(rows)),
        .levels = {},
        .mayRead = {},
        .scope = {},
        .services = {},
        .limits = {}});
    ViewCache views;
    detail::AnswerContext const context{.views = &views, .record = "T.rows", .rowScoped = false};
    TableQuery query;
    query.sort = {SortKey{.column = "n", .dir = SortDirection::Descending}};
    REQUIRE(detail::answerQuery(served, query, context).has_value());
    CHECK(views.stats().computations == 1);
    auto keyed = query;
    keyed.keys.emplace();
    for (std::int64_t i = 0; i < 20; ++i) {
        keyed.keys->emplace_back(i * 50);
    }
    auto const page = detail::answerQuery(served, keyed, context);
    REQUIRE(page.has_value());
    CHECK(page->keys.size() == 20);
    CHECK(views.stats().computations == 1);
    CHECK(views.stats().hits == 1);
}

namespace {

// One row, whose destructor waits until the test lets it finish: a snapshot whose release is slow.
class SlowToFree final : public RowSnapshot {
public:
    SlowToFree(std::atomic<bool>& entered, std::shared_future<void> release)
        : _entered{&entered}, _release{std::move(release)} {}
    SlowToFree(SlowToFree const&) = delete;
    SlowToFree& operator=(SlowToFree const&) = delete;
    SlowToFree(SlowToFree&&) = delete;
    SlowToFree& operator=(SlowToFree&&) = delete;
    ~SlowToFree() override {
        _entered->store(true);
        static_cast<void>(_release.wait_for(10s));
    }
    [[nodiscard]] std::size_t rowCount() const override { return 1; }
    [[nodiscard]] RowId rowId(std::size_t /*row*/) const override { return RowId{std::int64_t{0}}; }
    [[nodiscard]] Cell cell(std::size_t /*row*/, std::size_t /*column*/) const override { return Cell{}; }

private:
    std::atomic<bool>* _entered;
    std::shared_future<void> _release;
};

std::expected<ViewComputation, TableError> oneRowView() {
    return ViewComputation{.view = {.rows = {0}, .undecided = {}}, .awaiting = {}, .failed = {}};
}

}  // namespace

// Mutation: drop() destroying the entry under the mutex (the hit on another key waits for the snapshot's release).
TEST_CASE("view cache: an evicted entry is released outside the mutex", "[table][views][threads]") {
    ViewCache views{ViewCacheOptions{.maxBytes = 4, .onCompute = {}}};  // one one-row view
    std::atomic<bool> entered{false};
    std::promise<void> release;
    auto const releaseFuture = release.get_future().share();
    {
        auto const slow = std::make_shared<SlowToFree const>(entered, releaseFuture);
        REQUIRE(views.find(keyOf("slow"), slow, {}, oneRowView).has_value());
    }
    auto const source = tabletest::makeSource({tabletest::col("a", ColumnKind::Integer)}, {{Cell{std::int64_t{1}}}});
    auto const other = source->snapshot();
    // Inserting "other" evicts "slow", whose snapshot only the cache held: its release blocks this thread.
    auto evicting = std::async(std::launch::async, [&] { return views.find(keyOf("other"), other, {}, oneRowView); });
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!entered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(entered.load());
    auto hit = std::async(std::launch::async, [&] { return views.stats(); });
    auto third = std::async(std::launch::async, [&] {
        auto const source2 =
            tabletest::makeSource({tabletest::col("a", ColumnKind::Integer)}, {{Cell{std::int64_t{2}}}});
        return views.find(keyOf("third"), source2->snapshot(), {}, oneRowView).has_value();
    });
    bool const thirdDone = third.wait_for(5s) == std::future_status::ready;
    release.set_value();
    CHECK(thirdDone);
    CHECK(third.get());
    REQUIRE(evicting.get().has_value());
    static_cast<void>(hit.get());
}

// Mutation: a stale entry dropped whatever the request's newness (the newer entry is replaced, and computed again).
TEST_CASE("view cache: an older request does not displace a newer entry", "[table][views]") {
    auto const source = tabletest::makeSource({tabletest::col("a", ColumnKind::Integer)}, {{Cell{std::int64_t{1}}}});
    auto const older = source->snapshot();
    source->updateRow(0, {Cell{std::int64_t{2}}});
    auto const newer = source->snapshot();
    ViewCache views;
    REQUIRE(views.find(keyOf("a"), newer, {}, oneRowView, 2).has_value());
    auto const straggler = views.find(keyOf("a"), older, {}, oneRowView, 1);
    REQUIRE(straggler.has_value());
    CHECK((*straggler)->snapshot == older);
    REQUIRE(views.find(keyOf("a"), newer, {}, oneRowView, 3).has_value());
    CHECK(views.stats().computations == 2);
    CHECK(views.stats().hits == 1);
}

// Mutation: an empty scope in the view key (the second caller is answered the first caller's view).
TEST_CASE("view cache: callers of two scopes over one snapshot get two views", "[table][views]") {
    std::vector<std::vector<Cell>> rows;
    for (std::int64_t row = 0; row < 20; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
        rows.push_back({Cell{row}, Cell{(row * 7) % 5}});
    }
    auto const description = describe(TableDescription{
        .id = "t",
        .label = "T",
        .columns = {TableColumn{.info = tabletest::col("id", ColumnKind::Integer), .label = "id", .group = {}},
                    TableColumn{.info = tabletest::col("n", ColumnKind::Integer), .label = "n", .group = {}}},
        .key = "id"});
    auto const first = detail::pinned(ServedTable{
        .description = description,
        .source = tabletest::makeSource(
            {tabletest::col("id", ColumnKind::Integer), tabletest::col("n", ColumnKind::Integer)}, std::move(rows)),
        .levels = {},
        .mayRead = {},
        .scope = "p1",
        .services = {},
        .limits = {}});
    auto second = first;  // the same pinned source: one snapshot, one identity
    second.scope = "p2";
    ViewCache views;
    detail::AnswerContext const context{.views = &views, .record = "T.rows", .rowScoped = true};
    TableQuery query;
    query.sort = {SortKey{.column = "n", .dir = SortDirection::Ascending}};
    REQUIRE(detail::answerQuery(first, query, context).has_value());
    REQUIRE(detail::answerQuery(second, query, context).has_value());
    REQUIRE(detail::answerQuery(first, query, context).has_value());
    CHECK(views.stats().computations == 2);
    CHECK(views.stats().hits == 1);
}

// Mutation: newness compared on the rows alone (the later-pinned request on the older generation replaces the entry).
TEST_CASE("view cache: a request on an older level generation does not displace a newer entry", "[table][views]") {
    auto const source = tabletest::makeSource({tabletest::col("a", ColumnKind::Integer)}, {{Cell{std::int64_t{1}}}});
    auto const snapshot = source->snapshot();
    Levels const newer{{"recipe", 5}};
    Levels const older{{"recipe", 4}};
    ViewCache views;
    REQUIRE(views.find(keyOf("c"), snapshot, newer, oneRowView, 2).has_value());
    // Pinned later, but its level snapshot was read earlier.
    auto const straggler = views.find(keyOf("c"), snapshot, older, oneRowView, 3);
    REQUIRE(straggler.has_value());
    CHECK((*straggler)->levels == older);
    REQUIRE(views.find(keyOf("c"), snapshot, newer, oneRowView, 4).has_value());
    CHECK(views.stats().computations == 2);
    CHECK(views.stats().hits == 1);
}
