// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <morph/offline/sqlite_offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <string>
#include <vector>

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

    queue.enqueue("a");
    queue.enqueue("b");

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
