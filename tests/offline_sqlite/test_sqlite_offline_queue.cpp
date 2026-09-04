// SPDX-License-Identifier: Apache-2.0

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <morph/core/observability.hpp>
#include <morph/offline/sqlite_offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <optional>
#include <string>
#include <vector>

#include "../offline_queue_conformance.hpp"

namespace {

std::filesystem::path tempDbPath() {
    static std::atomic<int> counter{0};
    auto const now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("morph_sqlite_offline_queue_test_" + std::to_string(now) + "_" + std::to_string(++counter) + ".db");
}

void removeDbFiles(const std::filesystem::path& path) {
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
}

}  // namespace

TEST_CASE("morph::offline::SqliteOfflineQueue: items survive destroying and reopening over the same file",
          "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);

    uint64_t id1 = 0;
    uint64_t id2 = 0;
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        id1 = queue.enqueue("payload-1", "key-1");
        id2 = queue.enqueue("payload-2");
        queue.setAttempts(id2, 2);
    }
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        auto items = queue.drain();
        REQUIRE(items.size() == 2);
        REQUIRE(items[0].id == id1);
        REQUIRE(items[0].payload == "payload-1");
        REQUIRE(items[0].idempotencyKey == "key-1");
        REQUIRE(items[0].attempts == 0);
        REQUIRE(items[1].id == id2);
        REQUIRE(items[1].payload == "payload-2");
        REQUIRE(items[1].attempts == 2);
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: item survives a crash between drain() and markDone()", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);

    uint64_t enqueuedId = 0;
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        enqueuedId = queue.enqueue("payload");
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        // Simulate a crash: the replay side effect notionally ran, but the
        // process dies before markDone() -- the queue is destroyed without it.
    }
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        REQUIRE(items[0].id == enqueuedId);
        queue.markDone(enqueuedId);
        REQUIRE(queue.drain().empty());
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: re-enqueue with the same idempotencyKey is deduplicated", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::offline::SqliteOfflineQueue queue{dbPath};

    auto id1 = queue.enqueue("first-payload", "op-123");
    auto id2 = queue.enqueue("second-payload", "op-123");

    REQUIRE(id1 == id2);
    auto items = queue.drain();
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].payload == "first-payload");  // first write wins

    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: empty idempotencyKey items are never deduplicated", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::offline::SqliteOfflineQueue queue{dbPath};

    (void)queue.enqueue("a");
    (void)queue.enqueue("b");

    REQUIRE(queue.drain().size() == 2);
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue + SyncWorker: poison item dead-letters across a simulated restart",
          "[sqlite][sync]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);

    uint64_t enqueuedId = 0;
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        enqueuedId = queue.enqueue("poison-payload");
    }

    auto alwaysFail = [](const std::string&) { return false; };

    // 3 pre-restart run() calls persist attempts == 3 in the database.
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        morph::offline::SyncWorker worker{queue, alwaysFail};
        worker.run();
        worker.run();
        worker.run();
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        REQUIRE(items[0].attempts == 3);
    }

    // "Restart": brand-new SqliteOfflineQueue + brand-new SyncWorker over the
    // same file -- the in-memory _attempts map is gone; only the persisted
    // `attempts` column survives.
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        std::vector<morph::offline::QueueItem> deadLettered;
        morph::offline::SyncWorker worker{
            queue, alwaysFail, [&](const morph::offline::QueueItem& item) { deadLettered.push_back(item); }};

        auto result1 = worker.run();  // 4th cumulative attempt
        REQUIRE(result1.failed == 1);
        auto result2 = worker.run();  // 5th cumulative attempt -> dead-letters
        REQUIRE(result2.deadLettered == 1);
        REQUIRE(deadLettered.size() == 1);
        REQUIRE(deadLettered[0].id == enqueuedId);
        REQUIRE(deadLettered[0].attempts == 5);
        REQUIRE(queue.drain().empty());
    }
    removeDbFiles(dbPath);
}

// ── Coverage: maxDepth / overflow policy (morph#112) ───────────────────────

TEST_CASE("morph::offline::SqliteOfflineQueue: enqueue at maxDepth throws OfflineQueueFullError",
          "[sqlite][overflow]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath, 2};
        (void)queue.enqueue("a");
        (void)queue.enqueue("b");
        REQUIRE_THROWS_AS(queue.enqueue("c"), morph::offline::OfflineQueueFullError);
        REQUIRE(queue.drain().size() == 2);
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: maxDepth survives destroying and reopening over the same file",
          "[sqlite][overflow]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath, 1};
        (void)queue.enqueue("a");
        REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);
    }
    {
        // Reopened with the same maxDepth argument -- still enforced. maxDepth
        // is a per-construction parameter, not persisted in the database itself.
        morph::offline::SqliteOfflineQueue queue{dbPath, 1};
        REQUIRE(queue.drain().size() == 1);
        REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: size() matches COUNT(*) against the table", "[sqlite][overflow]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        REQUIRE(queue.size() == 0);
        (void)queue.enqueue("a");
        (void)queue.enqueue("b");
        (void)queue.enqueue("c");
        REQUIRE(queue.size() == 3);

        // Cross-check via a raw query against the same database file.
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(dbPath.string().c_str(), &raw) == SQLITE_OK);
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM morph_offline_queue;", -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        auto const rawCount = static_cast<std::size_t>(sqlite3_column_int64(stmt, 0));
        sqlite3_finalize(stmt);
        sqlite3_close(raw);

        REQUIRE(rawCount == queue.size());
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: a dedup hit at capacity is rejected (documented conservatism)",
          "[sqlite][overflow]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath, 1};
        (void)queue.enqueue("first-payload", "op-1");
        // The keyed path checks capacity BEFORE attempting the insert, so a
        // call that would otherwise resolve to a dedup hit (inserting
        // nothing) is still rejected once the queue is full -- documented
        // conservatism, not a bug.
        REQUIRE_THROWS_AS(queue.enqueue("second-payload", "op-1"), morph::offline::OfflineQueueFullError);
        REQUIRE(queue.drain().size() == 1);
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: enqueue at maxDepth emits queueOverflow metric",
          "[sqlite][overflow][observability]") {
    morph::observe::ScopedObserveOverride guard;
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath, 1};
        (void)queue.enqueue("a");

        std::vector<double> samples;
        morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
            if (evt.metric == morph::observe::Metric::queueOverflow) {
                samples.push_back(evt.value);
            }
        });

        REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);
        REQUIRE(samples.size() == 1);
        REQUIRE(samples[0] == 1.0);
    }
    removeDbFiles(dbPath);
}

// ── IOfflineQueue conformance ─────────────────────────────────────────────────
//
// `SqliteOfflineQueue` deduplicates a non-empty idempotency key via the partial
// unique index `ix_queue_idem`. That is a permitted strengthening of the
// `IOfflineQueue` contract, declared here so the shared suite asserts it rather
// than tolerating either behaviour.

TEST_CASE("morph::offline::SqliteOfflineQueue: IOfflineQueue idempotency-key conformance", "[sqlite]") {
    std::vector<std::filesystem::path> created;
    morph::test::checkIdempotencyKeyContract("SqliteOfflineQueue", morph::test::KeyDedup::onPendingItems, [&created] {
        auto dbPath = tempDbPath();
        removeDbFiles(dbPath);
        created.push_back(dbPath);
        return std::make_unique<morph::offline::SqliteOfflineQueue>(dbPath);
    });
    for (auto const& dbPath : created) {
        removeDbFiles(dbPath);
    }
}

TEST_CASE("morph::offline::SqliteOfflineQueue: the idempotency-key contract survives a reopen", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::test::checkIdempotencyKeyContractAcrossReopen(
        "SqliteOfflineQueue", morph::test::KeyDedup::onPendingItems,
        [&dbPath] { return std::make_unique<morph::offline::SqliteOfflineQueue>(dbPath); });
    removeDbFiles(dbPath);
}

// ── setIdempotencyKey on a conflicting key (morph#249) ───────────────────────
//
// The protected hook is reached only through the *base* default
// `IOfflineQueue::enqueue(payload, key)`, which inserts first and stamps
// second. On a key a pending row already holds, the partial unique index
// rejects the stamp. It used to throw, while this same class's own
// `enqueue(payload, key)` resolved the identical conflict silently by keeping
// the existing row — one conflict, two answers.
//
// The scope-qualified call below is how the base default is reached; it mirrors
// tests/test_file_offline_queue.cpp's existing scope-qualified case for the
// sibling implementation.

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("morph::offline::SqliteOfflineQueue: a conflicting setIdempotencyKey does not throw", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        auto const first = queue.enqueue("payload-A", "K1");

        // Base-qualified: insert, then stamp a key "K1" already holds.
        std::uint64_t viaBase = 0;
        CHECK_NOTHROW(viaBase = queue.morph::offline::IOfflineQueue::enqueue("payload-B", "K1"));

        // The row the base default inserted exists either way — the stamp is
        // what conflicts, and it is skipped rather than raised. The pre-existing
        // keyed row keeps both its key and its payload.
        auto const items = queue.drain();
        REQUIRE(items.size() == 2);

        auto const firstItem = std::ranges::find_if(items, [first](auto const& i) { return i.id == first; });
        REQUIRE(firstItem != items.end());
        CHECK(firstItem->payload == "payload-A");
        CHECK(firstItem->idempotencyKey == "K1");

        // The newly-inserted row is present and unkeyed: the conflict cost it
        // its key, not its existence. It is *not* a dedup hit — the base path
        // cannot produce one, since it has already inserted by the time it
        // stamps. Callers wanting dedup use the virtual two-arg enqueue.
        auto const secondItem = std::ranges::find_if(items, [viaBase](auto const& i) { return i.id == viaBase; });
        REQUIRE(secondItem != items.end());
        CHECK(secondItem->payload == "payload-B");
        CHECK(secondItem->idempotencyKey.empty());
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: a non-conflicting setIdempotencyKey still stamps", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        (void)queue.enqueue("payload-A", "K1");
        // Control: without it, the case above would pass against a hook that
        // silently stamped nothing at all.
        auto const fresh = queue.morph::offline::IOfflineQueue::enqueue("payload-B", "K2");
        auto const items = queue.drain();
        auto const item = std::ranges::find_if(items, [fresh](auto const& i) { return i.id == fresh; });
        REQUIRE(item != items.end());
        CHECK(item->idempotencyKey == "K2");
    }
    removeDbFiles(dbPath);
}
